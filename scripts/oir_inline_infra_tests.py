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


SOURCE = r"""
#include "oir/OIR.h"
#include "oir/OIRScalarOpt.h"
#include "pass/CostModel.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string(__FILE__) + ":" +                            \
                                     std::to_string(__LINE__) +                                 \
                                     ": requirement failed: " #condition);                    \
        }                                                                                       \
    } while (false)

void require_verified(oir::Module &module) {
    std::string error;
    if (!module.verify(&error)) {
        throw std::runtime_error("verification failed: " + error + "\n" + module.print());
    }
}

void require_gate_decision(const pass::cost_model::CostModelReport &report,
                           const std::string &reason) {
    REQUIRE(report.decisions.size() == 1);
    const auto &decision = report.decisions.front();
    REQUIRE(decision.action == pass::cost_model::DecisionAction::Reject);
    REQUIRE(!decision.legal);
    REQUIRE(decision.candidate_id.find("scalar-clone-gate") != std::string::npos);
    REQUIRE(decision.proof.summary.find(reason) != std::string::npos);
}

void require_inline_rejected_without_mutation(oir::Module &module,
                                              const std::string &reason) {
    require_verified(module);
    const auto before = module.print();
    pass::cost_model::CostModelReport report;
    pass::oir_opt::Stats stats;
    stats.cost_model_report = &report;
    stats.cost_model_filter = "OIRInlinePass";
    REQUIRE(!pass::oir_opt::inline_functions(module, stats));
    REQUIRE(stats.inlined == 0);
    REQUIRE(module.print() == before);
    require_verified(module);
    require_gate_decision(report, reason);
}

void test_typed_vector_constant_body_blocks_inline_and_specialization() {
    oir::Module module("inline-vector-constant");
    auto &types = module.types();
    auto *callee = module.create_function(
        "vector_body", types.func_ty(types.int32_ty(), {types.int32_ty()}));
    auto *entry = callee->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *vector_type = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *constant = module.create_constant_vector(
        vector_type, {builder.i32(1), builder.i32(2), builder.i32(3)});
    auto *lane = builder.create_extract_element(constant, builder.i32(0), "lane");
    builder.create_ret(builder.create_binary(oir::Instruction::OpID::Add, lane,
                                             callee->args()[0].get(), "sum"));

    auto *caller = module.create_function("caller", types.func_ty(types.int32_ty(), {}));
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *call = builder.create_call(callee, types.int32_ty(), {builder.i32(4)}, "call");
    builder.create_ret(call);
    require_verified(module);
    const auto before = module.print();

    pass::cost_model::CostModelReport specialization_report;
    pass::oir_opt::Stats specialization_stats;
    specialization_stats.cost_model_report = &specialization_report;
    specialization_stats.cost_model_filter = "OIRInlinePass";
    REQUIRE(!pass::oir_opt::specialize_constant_argument_calls(
        module, specialization_stats));
    REQUIRE(module.print() == before);
    require_gate_decision(specialization_report, "OIRINLINE_VECTOR_CONSTANT");

    require_inline_rejected_without_mutation(module, "OIRINLINE_VECTOR_CONSTANT");
}

void test_fixed_vector_signature_blocks_inline_before_cloning() {
    oir::Module module("inline-vector-signature");
    auto &types = module.types();
    auto *vector_type = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *callee = module.create_function(
        "identity", types.func_ty(vector_type, {vector_type}));
    auto *entry = callee->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(callee->args()[0].get());

    auto *caller = module.create_function("caller", types.func_ty(types.int32_ty(), {}));
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *constant = module.create_constant_vector(
        vector_type, {builder.i32(5), builder.i32(6), builder.i32(7)});
    auto *call = builder.create_call(callee, vector_type, {constant}, "call");
    builder.create_ret(builder.create_extract_element(call, builder.i32(0), "lane"));

    require_inline_rejected_without_mutation(module, "OIRINLINE_VECTOR_SIGNATURE");
}

void test_scalable_setvl_and_vp_body_blocks_inline() {
    oir::Module module("inline-vp-body");
    auto &types = module.types();
    auto *pointer_type = types.ptr_ty(types.int32_ty());
    auto *callee = module.create_function(
        "vp_body", types.func_ty(types.int32_ty(), {pointer_type, types.int32_ty()}));
    auto *entry = callee->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *vector_type = types.scalable_vector_ty(types.int32_ty(), 4);
    auto *mask_type = types.scalable_vector_ty(types.int1_ty(), 4);
    auto *vl = builder.create_set_vl(vector_type, callee->args()[1].get(), "vl");
    auto *active = builder.create_splat(mask_type, builder.i1(true), "active");
    auto *loaded = builder.create_vp_load(
        vector_type, callee->args()[0].get(), active, vl, builder.undef(vector_type),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 4, "loaded");
    auto *sum = builder.create_vp_reduction(
        oir::ReductionKind::Add, false, loaded, active, vl, builder.i32(0),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "sum");
    builder.create_ret(sum);

    auto *caller = module.create_function(
        "caller", types.func_ty(types.int32_ty(), {pointer_type, types.int32_ty()}));
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *call = builder.create_call(
        callee, types.int32_ty(),
        {caller->args()[0].get(), caller->args()[1].get()}, "call");
    builder.create_ret(call);

    require_inline_rejected_without_mutation(module, "OIRINLINE_VECTOR_BODY");
}

void test_scalar_clone_path_remains_available() {
    oir::Module module("inline-scalar-control");
    auto &types = module.types();
    auto *callee = module.create_function(
        "add_one", types.func_ty(types.int32_ty(), {types.int32_ty()}));
    auto *entry = callee->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(builder.create_binary(oir::Instruction::OpID::Add,
                                             callee->args()[0].get(), builder.i32(1),
                                             "sum"));
    auto *caller = module.create_function(
        "caller", types.func_ty(types.int32_ty(), {types.int32_ty()}));
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *call = builder.create_call(
        callee, types.int32_ty(), {caller->args()[0].get()}, "call");
    builder.create_ret(call);
    require_verified(module);

    pass::oir_opt::Stats stats;
    REQUIRE(pass::oir_opt::inline_functions(module, stats));
    REQUIRE(stats.inlined == 1);
    require_verified(module);
    REQUIRE(!module.print().empty());
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"typed_vector_constant_body_blocks_inline_and_specialization",
         test_typed_vector_constant_body_blocks_inline_and_specialization},
        {"fixed_vector_signature_blocks_inline_before_cloning",
         test_fixed_vector_signature_blocks_inline_before_cloning},
        {"scalable_setvl_and_vp_body_blocks_inline",
         test_scalable_setvl_and_vp_body_blocks_inline},
        {"scalar_clone_path_remains_available",
         test_scalar_clone_path_remains_available},
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
"""


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
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="oir-inline-infra-") as tmp:
        directory = Path(tmp)
        source = directory / "oir_inline_infra_tests.cpp"
        binary = directory / "oir_inline_infra_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = [
            source,
            ROOT / "src/oir/OIR.cpp",
            ROOT / "src/oir/OIRAnalysis.cpp",
            ROOT / "src/oir/OIRCFGUtils.cpp",
            ROOT / "src/oir/OIRDataLayout.cpp",
            ROOT / "src/builtin/BuiltinRegistry.cpp",
            ROOT / "src/pass/CostModel.cpp",
            ROOT / "src/pass/oir/OIRCostModel.cpp",
            ROOT / "src/pass/oir/OIRScalarOptUtils.cpp",
            ROOT / "src/pass/oir/OIRCountdownLoopAnalysis.cpp",
            ROOT / "src/pass/oir/OIRInlinePass.cpp",
        ]
        command = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            "-I",
            str(ROOT / "include"),
            *map(str, sources),
            "-Wl,--gc-sections",
            "-o",
            str(binary),
        ]
        compiled = subprocess.run(
            command, cwd=ROOT, text=True, capture_output=True, check=False
        )
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        ran = subprocess.run(
            [str(binary)], cwd=ROOT, text=True, capture_output=True, check=False
        )
        print(ran.stdout, end="")
        if ran.returncode != 0:
            print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
