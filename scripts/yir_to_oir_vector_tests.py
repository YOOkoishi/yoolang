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
#include "pass/yir/YIRToOIRLowerer.h"
#include "yir/YIRVerifier.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

namespace {

void require_yir_valid(const yir::Module &module) {
    auto result = yir::verify_high_level_yir(module);
    if (!result.success) {
        std::string message;
        for (const auto &error : result.errors) message += error + "\n";
        throw std::runtime_error(message);
    }
}

void require_oir_valid(const oir::Module &module) {
    std::string message;
    if (!module.verify(&message)) throw std::runtime_error(message);
}

void test_typed_constants_survive_lowering() {
    yir::Module source;
    auto i32 = yir::Type::get_i32();
    auto v3i = yir::Type::get_vector(3, i32);
    auto m10 = yir::Type::get_mask(10);
    auto one = std::make_shared<yir::ConstantInt>(i32, 1);
    auto two = std::make_shared<yir::ConstantInt>(i32, 2);
    auto three = std::make_shared<yir::ConstantInt>(i32, 3);
    source.add_global("vector", v3i, true)->set_initializer(
        std::make_shared<yir::ConstantVector>(
            v3i, std::vector<yir::ConstantPtr>{one, two, three}));
    source.add_global("mask", m10, true)->set_initializer(
        std::make_shared<yir::ConstantMask>(m10, std::vector<std::uint8_t>{0x01, 0x02}));
    require_yir_valid(source);

    auto lowered = pass::yir_to_oir::lower_yir_to_oir(source);
    require_oir_valid(*lowered);
    REQUIRE(lowered->globals().size() == 2);
    REQUIRE(dynamic_cast<oir::ConstantVector *>(lowered->globals()[0]->initializer()) != nullptr);
    auto *mask = dynamic_cast<oir::ConstantMask *>(lowered->globals()[1]->initializer());
    REQUIRE(mask != nullptr);
    REQUIRE(mask->lane(0));
    REQUIRE(mask->lane(9));
}

void test_fixed_vector_operation_families_lower_and_verify() {
    yir::Module source;
    auto i32 = yir::Type::get_i32();
    auto f32 = yir::Type::get_f32();
    auto v3i = yir::Type::get_vector(3, i32);
    auto v3f = yir::Type::get_vector(3, f32);
    auto ptr = yir::Type::get_ptr(i32);
    auto *function = source.add_function("vectors", yir::Type::get_void(), {v3i, v3i, ptr});
    auto *a = function->add_param(v3i, "a");
    auto *b = function->add_param(v3i, "b");
    auto *base = function->add_param(ptr, "base");
    auto &body = function->body();
    auto *zero = body.append<yir::ConstI32Op>(0, "zero")->result();
    auto *one = body.append<yir::ConstI32Op>(1, "one")->result();
    auto *two = body.append<yir::ConstI32Op>(2, "two")->result();
    auto *created = body.append<yir::VectorCreateOp>(
        v3i, std::vector<yir::Value *>{zero, one, two}, "created")->result();
    auto *splat = body.append<yir::SplatOp>(one, v3i, "splat")->result();
    auto *sum = body.append<yir::AddIOp>(created, splat, "sum")->result();
    auto *mask = body.append<yir::ICmpOp>(yir::ICmpOp::Predicate::Lt, sum, a, "mask")->result();
    auto *mask_not = body.append<yir::MaskNotOp>(mask, "mask.not")->result();
    auto *mask_or = body.append<yir::MaskBinaryOp>(
        yir::MaskBinaryOp::Kind::Or, mask, mask_not, "mask.or")->result();
    auto *selected = body.append<yir::SelectOp>(mask_or, a, b, "selected")->result();
    auto *lane = body.append<yir::ExtractLaneOp>(selected, one, "lane")->result();
    auto *inserted = body.append<yir::InsertLaneOp>(selected, two, lane, "inserted")->result();
    auto *floats = body.append<yir::VectorCastOp>(inserted, v3f, "floats")->result();
    body.append<yir::VectorReduceOp>(yir::VectorReduceOp::Kind::Add, floats, true,
                                     "ordered.sum");
    body.append<yir::VectorReduceOp>(yir::VectorReduceOp::Kind::Or, inserted, false,
                                     "bits.or");
    body.append<yir::MaskReduceOp>(yir::MaskReduceOp::Kind::Any, mask, "any");
    body.append<yir::MaskReduceOp>(yir::MaskReduceOp::Kind::All, mask, "all");
    body.append<yir::MaskReduceOp>(yir::MaskReduceOp::Kind::None, mask, "none");
    auto *indices = body.append<yir::StepVectorOp>(v3i, "indices")->result();
    auto *passthrough = body.append<yir::ZeroOp>(v3i, "passthrough")->result();
    auto *loaded = body.append<yir::MaskedLoadOp>(base, mask, passthrough, 4, "loaded")->result();
    body.append<yir::MaskedStoreOp>(loaded, base, mask, 4);
    auto *gathered = body.append<yir::GatherOp>(base, indices, mask, passthrough, 4,
                                               "gathered")->result();
    body.append<yir::ScatterOp>(gathered, base, indices, mask, 4);
    body.append<yir::ShuffleOp>(a, b,
                                std::vector<std::uint64_t>{yir::ShuffleOp::UndefLane, 4, 2}, v3i,
                                "shuffle");
    auto *not_a = body.append<yir::BitNotOp>(a, "not.a")->result();
    auto *and_bits = body.append<yir::AndIOp>(not_a, b, "and.bits")->result();
    auto *or_bits = body.append<yir::OrIOp>(and_bits, a, "or.bits")->result();
    body.append<yir::XorIOp>(or_bits, b, "xor.bits");
    body.append<yir::CallOp>("putf", std::vector<yir::Value *>{one},
                             yir::Type::get_void());
    body.append<yir::ReturnOp>();
    require_yir_valid(source);

    auto lowered = pass::yir_to_oir::lower_yir_to_oir(source);
    require_oir_valid(*lowered);
    const auto text = lowered->print();
    REQUIRE(text.find("insertelement") != std::string::npos);
    REQUIRE(text.find("splat") != std::string::npos);
    REQUIRE(text.find("select") != std::string::npos);
    REQUIRE(text.find("vector.sitofp") != std::string::npos);
    REQUIRE(text.find("vp.reduce.ordered.fadd") != std::string::npos);
    REQUIRE(text.find("masked.load") != std::string::npos);
    REQUIRE(text.find("masked.store") != std::string::npos);
    REQUIRE(text.find("vp.gather") != std::string::npos);
    REQUIRE(text.find("vp.scatter") != std::string::npos);
    REQUIRE(text.find("shufflevector") != std::string::npos);
    REQUIRE(text.find("[-1, 4, 2]") != std::string::npos);
    REQUIRE(text.find(" and ") != std::string::npos);
    REQUIRE(text.find(" or ") != std::string::npos);
    REQUIRE(text.find(" xor ") != std::string::npos);
    auto *putf = lowered->get_function("putf");
    REQUIRE(putf != nullptr);
    REQUIRE(putf->function_type()->is_variadic());
    REQUIRE(putf->function_type()->param_types().empty());
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"typed_constants_survive_lowering", test_typed_constants_survive_lowering},
        {"fixed_vector_operation_families_lower_and_verify",
         test_fixed_vector_operation_families_lower_and_verify},
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
    with tempfile.TemporaryDirectory(prefix="yir-to-oir-vector-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "tests.cpp"
        binary = tmp_dir / "tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        command = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include"),
            str(source),
            str(ROOT / "src/yir/YIR.cpp"),
            str(ROOT / "src/yir/YIRVerifier.cpp"),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRDataLayout.cpp"),
            str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
            str(ROOT / "src/pass/yir/YIRToOIRLowerer.cpp"),
            "-o",
            str(binary),
        ]
        compiled = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        ran = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, check=False)
        print(ran.stdout, end="")
        print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
