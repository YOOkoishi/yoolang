#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


SOURCE = r'''
#include "oir/OIR.h"
#include "oir/OIRAnalysis.h"
#include "oir/OIRParser.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Failure final : std::exception {
    explicit Failure(std::string text) : text(std::move(text)) {}
    const char *what() const noexcept override { return text.c_str(); }
    std::string text;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition))                                                                       \
            throw Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                          ": requirement failed: " #condition);                               \
    } while (false)

void require_verified(const oir::Module &module) {
    std::string error;
    if (!module.verify(&error)) throw Failure("verification failed: " + error);
}

std::unique_ptr<oir::Module> build_boundary_module() {
    auto module = std::make_unique<oir::Module>("fixed-abi-boundary");
    auto &types = module->types();
    auto *i1 = types.int1_ty();
    auto *i32 = types.int32_ty();
    auto *v3 = types.fixed_vector_ty(i32, 3);
    auto *m31 = types.fixed_vector_ty(i1, 31);
    auto *v3_ptr = types.ptr_ty(v3);
    auto *m31_ptr = types.ptr_ty(m31);

    auto *callee = module->create_function(
        "foreign", types.func_ty(v3, {v3, m31}), true);
    auto *function = module->create_function(
        "bridge", types.func_ty(v3, {v3, v3_ptr, m31, m31_ptr}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);

    std::vector<oir::Value *> numeric_lanes;
    for (std::uint64_t lane = 0; lane < 3; ++lane) {
        auto *incoming = builder.create_fixed_abi_extract_lane(
            function->args()[0].get(), lane, "incoming." + std::to_string(lane));
        auto *loaded = builder.create_fixed_abi_object_load_lane(
            function->args()[1].get(), lane, "object." + std::to_string(lane));
        numeric_lanes.push_back(builder.create_binary(
            oir::Instruction::OpID::Add, incoming, loaded,
            "sum." + std::to_string(lane)));
    }

    std::vector<oir::Value *> mask_lanes(31, module->create_i1(false));
    for (std::uint64_t lane : {0U, 1U, 7U, 30U}) {
        auto *incoming = builder.create_fixed_abi_extract_lane(
            function->args()[2].get(), lane, "mask.in." + std::to_string(lane));
        auto *store = builder.create_fixed_abi_object_store_lane(
            incoming, function->args()[3].get(), lane);
        (void)store;
        mask_lanes[lane] = builder.create_fixed_abi_object_load_lane(
            function->args()[3].get(), lane, "mask.out." + std::to_string(lane));
    }

    auto *numeric_pack = builder.create_fixed_abi_pack(v3, numeric_lanes, "numeric.pack");
    auto *mask_pack = builder.create_fixed_abi_pack(m31, mask_lanes, "mask.pack");
    auto *call = builder.create_call(callee, v3, {numeric_pack, mask_pack}, "foreign.result");
    std::vector<oir::Value *> result_lanes;
    for (std::uint64_t lane = 0; lane < 3; ++lane) {
        result_lanes.push_back(builder.create_fixed_abi_extract_lane(
            call, lane, "result." + std::to_string(lane)));
    }
    auto *result_pack = builder.create_fixed_abi_pack(v3, result_lanes, "result.pack");
    builder.create_ret(result_pack);

    auto *store_boundary = module->create_function(
        "store_boundary", types.func_ty(types.void_ty(), {v3, v3_ptr}));
    auto *store_entry = store_boundary->create_block("entry");
    builder.set_insert_point(store_entry);
    std::vector<oir::Value *> store_lanes;
    for (std::uint64_t lane = 0; lane < 3; ++lane) {
        store_lanes.push_back(builder.create_fixed_abi_extract_lane(
            store_boundary->args()[0].get(), lane,
            "store.in." + std::to_string(lane)));
    }
    auto *store_pack =
        builder.create_fixed_abi_pack(v3, store_lanes, "store.pack");
    builder.create_store(store_pack, store_boundary->args()[1].get());
    builder.create_ret();

    auto *load_boundary =
        module->create_function("load_boundary", types.func_ty(v3, {v3_ptr}));
    auto *load_entry = load_boundary->create_block("entry");
    builder.set_insert_point(load_entry);
    auto *aggregate_load =
        builder.create_load(load_boundary->args()[0].get(), v3, "aggregate.load");
    std::vector<oir::Value *> load_lanes;
    for (std::uint64_t lane = 0; lane < 3; ++lane) {
        load_lanes.push_back(builder.create_fixed_abi_extract_lane(
            aggregate_load, lane, "load.out." + std::to_string(lane)));
    }
    builder.create_ret(builder.create_fixed_abi_pack(v3, load_lanes, "load.pack"));
    return module;
}

void test_api_roundtrip_ownership_and_memory_model() {
    auto module = build_boundary_module();
    require_verified(*module);
    auto *function = module->get_function("bridge");
    REQUIRE(function != nullptr);
    REQUIRE(function->function_type()->print() ==
            "<3 x i32> (<3 x i32>, <3 x i32>*, <31 x i1>, <31 x i1>*)");
    REQUIRE(function->args()[0]->use_count() == 3);
    REQUIRE(function->args()[2]->use_count() == 4);

    oir::OIRAliasAnalysis alias;
    unsigned extracts = 0, packs = 0, lane_loads = 0, lane_stores = 0;
    for (const auto &block : function->blocks()) {
        for (const auto &owned : block->instructions()) {
            const auto &inst = *owned;
            if (dynamic_cast<const oir::FixedABIExtractLaneInst *>(&inst)) {
                ++extracts;
                REQUIRE(!alias.may_read_memory(inst));
                REQUIRE(!alias.may_write_memory(inst));
                REQUIRE(!alias.has_side_effect(inst));
            } else if (dynamic_cast<const oir::FixedABIPackInst *>(&inst)) {
                ++packs;
                REQUIRE(inst.has_uses());
                REQUIRE(!alias.may_read_memory(inst));
                REQUIRE(!alias.may_write_memory(inst));
            } else if (dynamic_cast<const oir::FixedABIObjectLoadLaneInst *>(&inst)) {
                ++lane_loads;
                REQUIRE(alias.may_read_memory(inst));
                REQUIRE(!alias.may_write_memory(inst));
                REQUIRE(!alias.has_side_effect(inst));
            } else if (dynamic_cast<const oir::FixedABIObjectStoreLaneInst *>(&inst)) {
                ++lane_stores;
                // Mask stores are packed-bit RMW: conservatively both read and write.
                REQUIRE(alias.may_read_memory(inst));
                REQUIRE(alias.may_write_memory(inst));
                REQUIRE(alias.has_side_effect(inst));
            }
        }
    }
    REQUIRE(extracts == 10);
    REQUIRE(packs == 3);
    REQUIRE(lane_loads == 7);
    REQUIRE(lane_stores == 4);

    const auto printed = module->print();
    REQUIRE(printed.find("abi.fixed.extract <3 x i32>") != std::string::npos);
    REQUIRE(printed.find("abi.fixed.pack <31 x i1>") != std::string::npos);
    REQUIRE(printed.find("abi.fixed.load_lane <31 x i1>*") != std::string::npos);
    REQUIRE(printed.find("abi.fixed.store_lane i1") != std::string::npos);
    REQUIRE(printed.find("store <3 x i32> %store.pack") != std::string::npos);
    REQUIRE(printed.find("abi.fixed.extract <3 x i32> %aggregate.load") !=
            std::string::npos);
    auto parsed = oir::OIRParser::parse(printed, "boundary-roundtrip.oir");
    REQUIRE(parsed.ok() && parsed.module != nullptr);
    require_verified(*parsed.module);
    REQUIRE(parsed.module->print() == printed);
    auto *parsed_bridge = parsed.module->get_function("bridge");
    REQUIRE(parsed_bridge != nullptr);
    REQUIRE(parsed_bridge->args()[0]->use_count() == 3);
    REQUIRE(parsed_bridge->args()[2]->use_count() == 4);
}

void require_parse_failure(const std::string &source, const std::string &needle) {
    auto result = oir::OIRParser::parse(source, "bad-boundary.oir");
    REQUIRE(!result.ok() && result.module == nullptr && !result.errors.empty());
    REQUIRE(result.errors.front().message.find(needle) != std::string::npos);
}

void require_verify_failure(const std::string &source, const std::string &needle) {
    auto result = oir::OIRParser::parse(source, "invalid-boundary.oir");
    if (!result.ok() || result.module == nullptr) {
        REQUIRE(!result.errors.empty());
        REQUIRE(result.errors.front().message.find(needle) != std::string::npos);
        return;
    }
    std::string error;
    REQUIRE(!result.module->verify(&error));
    REQUIRE(error.find(needle) != std::string::npos);
}

void test_parser_rejects_malformed_fixed_boundary() {
    require_parse_failure(R"OIR(; module: bad-lane

define i32 @f(<3 x i32> %arg0) {
entry:
  %bad = abi.fixed.extract <3 x i32> %arg0, lane 3
  ret i32 %bad
}
)OIR", "lane index is out of range");

    require_parse_failure(R"OIR(; module: bad-pack-count

define <3 x i32> @f() {
entry:
  %bad = abi.fixed.pack <3 x i32> [i32 1, i32 2]
  ret <3 x i32> %bad
}
)OIR", "exactly one value per fixed lane");

    require_parse_failure(R"OIR(; module: bad-object

define i32 @f(i32* %arg0) {
entry:
  %bad = abi.fixed.load_lane i32* %arg0, lane 0
  ret i32 %bad
}
)OIR", "ptr<fixed-vector>");

    require_parse_failure(R"OIR(; module: scalable

define i32 @f(<vscale x 4 x i32> %arg0) {
entry:
  %bad = abi.fixed.extract <vscale x 4 x i32> %arg0, lane 0
  ret i32 %bad
}
)OIR", "fixed-vector aggregate");
}

void test_verifier_rejects_non_boundary_source_and_consumer() {
    require_verify_failure(R"OIR(; module: bad-source

define i32 @f() {
entry:
  %vector = splat i32 1 to <3 x i32>
  %bad = abi.fixed.extract <3 x i32> %vector, lane 0
  ret i32 %bad
}
)OIR", "OIRV_FIXED_ABI_EXTRACT_SOURCE");

    require_verify_failure(R"OIR(; module: bad-consumer

define i32 @f() {
entry:
  %packed = abi.fixed.pack <3 x i32> [i32 1, i32 2, i32 3]
  %bad = extractelement <3 x i32> %packed, i32 0
  ret i32 %bad
}
)OIR", "OIRV_FIXED_ABI_PACK_CONSUMER");
}

} // namespace

int main() {
    const std::vector<std::pair<const char *, void (*)()>> tests = {
        {"api_roundtrip_ownership_and_memory_model",
         test_api_roundtrip_ownership_and_memory_model},
        {"parser_rejects_malformed_fixed_boundary",
         test_parser_rejects_malformed_fixed_boundary},
        {"verifier_rejects_non_boundary_source_and_consumer",
         test_verifier_rejects_non_boundary_source_and_consumer},
    };
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
'''


def find_cxx() -> str | None:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="oir-fixed-abi-boundary-") as tmp:
        directory = Path(tmp)
        source = directory / "oir_fixed_abi_boundary_tests.cpp"
        binary = directory / "oir_fixed_abi_boundary_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        compiled = subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
                str(ROOT / "src/oir/OIR.cpp"),
                str(ROOT / "src/oir/OIRAnalysis.cpp"),
                str(ROOT / "src/oir/OIRDataLayout.cpp"),
                str(ROOT / "src/oir/OIRParser.cpp"),
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        ran = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        print(ran.stdout, end="")
        print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
