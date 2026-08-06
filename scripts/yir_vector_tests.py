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
#include "yir/YIR.h"
#include "yir/YIRPrinter.h"
#include "yir/YIRVerifier.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

void require_valid(const yir::Module &module) {
    auto result = yir::verify_high_level_yir(module);
    if (!result.success) {
        std::string message;
        for (const auto &error : result.errors) {
            message += error + "\n";
        }
        throw std::runtime_error(message);
    }
}

void require_invalid(const yir::Module &module, const std::string &needle) {
    auto result = yir::verify_high_level_yir(module);
    REQUIRE(!result.success);
    for (const auto &error : result.errors) {
        if (error.find(needle) != std::string::npos) {
            return;
        }
    }
    throw std::runtime_error("missing verifier error: " + needle);
}

void test_fixed_vector_operations_and_printer() {
    yir::Module module;
    auto i32 = yir::Type::get_i32();
    auto f32 = yir::Type::get_f32();
    auto v3i = yir::Type::get_vector(3, i32);
    auto v3f = yir::Type::get_vector(3, f32);
    auto m3 = yir::Type::get_mask(3);
    auto ptr = yir::Type::get_ptr(i32);
    auto *function = module.add_function("vectors", yir::Type::get_void(), {v3i, v3i, ptr});
    auto *a = function->add_param(v3i, "a");
    auto *b = function->add_param(v3i, "b");
    auto *base = function->add_param(ptr, "base");
    auto &body = function->body();
    auto *zero = body.append<yir::ConstI32Op>(0, "zero")->result();
    auto *one = body.append<yir::ConstI32Op>(1, "one")->result();
    auto *two = body.append<yir::ConstI32Op>(2, "two")->result();
    auto *created = body.append<yir::VectorCreateOp>(v3i, std::vector<yir::Value *>{zero, one, two},
                                                     "created")->result();
    auto *splat = body.append<yir::SplatOp>(one, v3i, "splat")->result();
    auto *sum = body.append<yir::AddIOp>(created, splat, "sum")->result();
    auto *not_sum = body.append<yir::BitNotOp>(sum, "not_sum")->result();
    auto *bit_and = body.append<yir::AndIOp>(sum, not_sum, "bit_and")->result();
    auto *bit_or = body.append<yir::OrIOp>(bit_and, a, "bit_or")->result();
    body.append<yir::XorIOp>(bit_or, b, "bit_xor");
    auto *mask = body.append<yir::ICmpOp>(yir::ICmpOp::Predicate::Lt, sum, a, "mask")->result();
    REQUIRE(mask->type() == m3);
    auto *inverted = body.append<yir::MaskNotOp>(mask, "not")->result();
    auto *combined = body.append<yir::MaskBinaryOp>(yir::MaskBinaryOp::Kind::And, mask, inverted,
                                                    "and")->result();
    auto *selected = body.append<yir::SelectOp>(combined, a, b, "selected")->result();
    auto *lane = body.append<yir::ExtractLaneOp>(selected, one, "lane")->result();
    auto *inserted = body.append<yir::InsertLaneOp>(selected, two, lane, "inserted")->result();
    auto *floats = body.append<yir::VectorCastOp>(inserted, v3f, "floats")->result();
    body.append<yir::VectorReduceOp>(yir::VectorReduceOp::Kind::Add, floats, true, "reduce");
    body.append<yir::MaskReduceOp>(yir::MaskReduceOp::Kind::Any, mask, "any");
    auto *indices = body.append<yir::StepVectorOp>(v3i, "indices")->result();
    auto *passthrough = body.append<yir::ZeroOp>(v3i, "passthrough")->result();
    auto *loaded = body.append<yir::MaskedLoadOp>(base, mask, passthrough, 4, "loaded")->result();
    body.append<yir::MaskedStoreOp>(loaded, base, mask, 4);
    auto *gathered = body.append<yir::GatherOp>(base, indices, mask, passthrough, 4,
                                               "gathered")->result();
    body.append<yir::ScatterOp>(gathered, base, indices, mask, 4);
    body.append<yir::ShuffleOp>(
        a, b, std::vector<std::uint64_t>{yir::ShuffleOp::UndefLane, 4, 2}, v3i,
        "shuffle");
    body.append<yir::ReturnOp>();

    require_valid(module);
    std::ostringstream out;
    yir::YIRPrinter printer(out);
    printer.print(module);
    REQUIRE(out.str().find("yir.vector.create") != std::string::npos);
    REQUIRE(out.str().find("yir.icmp lt") != std::string::npos);
    REQUIRE(out.str().find(": mask<3>") != std::string::npos);
    REQUIRE(out.str().find("yir.vector.masked_load") != std::string::npos);
    REQUIRE(out.str().find("yir.vector.gather") != std::string::npos);
    REQUIRE(out.str().find("yir.noti") != std::string::npos);
    REQUIRE(out.str().find("yir.andi") != std::string::npos);
    REQUIRE(out.str().find("yir.ori") != std::string::npos);
    REQUIRE(out.str().find("yir.xori") != std::string::npos);
    REQUIRE(out.str().find("[-1, 4, 2]") != std::string::npos);
}

void test_verifier_rejects_shape_and_mask_errors() {
    {
        yir::Module module;
        auto v3 = yir::Type::get_vector(3, yir::Type::get_i32());
        auto v4 = yir::Type::get_vector(4, yir::Type::get_i32());
        auto *function = module.add_function("bad", yir::Type::get_void(), {v3, v4});
        auto *a = function->add_param(v3, "a");
        auto *b = function->add_param(v4, "b");
        function->body().append<yir::AddIOp>(a, b, "bad");
        function->body().append<yir::ReturnOp>();
        require_invalid(module, "operand/result shapes");
    }
    {
        yir::Module module;
        auto mask = yir::Type::get_mask(3);
        auto *function = module.add_function("bad_if", yir::Type::get_void(), {mask});
        auto *m = function->add_param(mask, "m");
        function->body().append<yir::IfOp>(m);
        function->body().append<yir::ReturnOp>();
        require_invalid(module, "requires scalar i1");
    }
}

void test_verifier_rejects_memory_shuffle_and_reduction_errors() {
    {
        yir::Module module;
        auto v3 = yir::Type::get_vector(3, yir::Type::get_i32());
        auto m4 = yir::Type::get_mask(4);
        auto ptr = yir::Type::get_ptr(yir::Type::get_i32());
        auto *function = module.add_function("bad_mem", yir::Type::get_void(), {v3, m4, ptr});
        auto *v = function->add_param(v3, "v");
        auto *m = function->add_param(m4, "m");
        auto *p = function->add_param(ptr, "p");
        function->body().append<yir::MaskedLoadOp>(p, m, v, 3, "bad");
        function->body().append<yir::ReturnOp>();
        require_invalid(module, "vector/pointer/mask/alignment");
    }
    {
        yir::Module module;
        auto v3 = yir::Type::get_vector(3, yir::Type::get_i32());
        auto *function = module.add_function("bad_shuffle", yir::Type::get_void(), {v3});
        auto *v = function->add_param(v3, "v");
        function->body().append<yir::ShuffleOp>(
            v, v, std::vector<std::uint64_t>{0, 1, 6}, v3, "bad");
        function->body().append<yir::ReturnOp>();
        require_invalid(module, "index is out of bounds");
    }
    {
        yir::Module module;
        auto v3f = yir::Type::get_vector(3, yir::Type::get_f32());
        auto *function = module.add_function("bad_reduce", yir::Type::get_void(), {v3f});
        auto *v = function->add_param(v3f, "v");
        function->body().append<yir::VectorReduceOp>(yir::VectorReduceOp::Kind::And, v, true,
                                                     "bad");
        function->body().append<yir::ReturnOp>();
        require_invalid(module, "bitwise vector reduction");
    }
}

void test_typed_global_constants_and_mask_layout() {
    yir::Module module;
    auto i32 = yir::Type::get_i32();
    auto f32 = yir::Type::get_f32();
    auto v3 = yir::Type::get_vector(3, i32);
    auto v3f = yir::Type::get_vector(3, f32);
    auto m10 = yir::Type::get_mask(10);
    auto array = yir::Type::get_array(2, v3);
    auto one = std::make_shared<yir::ConstantInt>(i32, 1);
    auto two = std::make_shared<yir::ConstantInt>(i32, 2);
    auto three = std::make_shared<yir::ConstantInt>(i32, 3);
    auto vector = std::make_shared<yir::ConstantVector>(
        v3, std::vector<yir::ConstantPtr>{one, two, three});
    auto aggregate = std::make_shared<yir::ConstantArray>(
        array, std::vector<yir::ConstantPtr>{vector, vector});
    auto mask = std::make_shared<yir::ConstantMask>(m10,
                                                    std::vector<std::uint8_t>{0x01, 0x02});
    auto positive_zero = std::make_shared<yir::ConstantFloat>(0.0F);
    auto negative_zero = std::make_shared<yir::ConstantFloat>(-0.0F);
    REQUIRE(positive_zero->is_zero());
    REQUIRE(!negative_zero->is_zero());
    auto signed_zero_vector = std::make_shared<yir::ConstantVector>(
        v3f, std::vector<yir::ConstantPtr>{positive_zero, negative_zero, positive_zero});
    REQUIRE(!signed_zero_vector->is_zero());
    REQUIRE(mask->lane(0));
    REQUIRE(mask->lane(9));
    REQUIRE(!mask->lane(8));

    module.add_global("vectors", array, true)->set_initializer(aggregate);
    module.add_global("mask", m10, true)->set_initializer(mask);
    module.add_global("signed_zero", v3f, true)->set_initializer(signed_zero_vector);
    require_valid(module);
    std::ostringstream out;
    yir::YIRPrinter printer(out);
    printer.print(module);
    REQUIRE(out.str().find("maskbits<1,0,0,0,0,0,0,0,0,1>") != std::string::npos);
    REQUIRE(out.str().find("[<i32 1, i32 2, i32 3>") != std::string::npos);
    REQUIRE(out.str().find("<f32 0, f32 -0, f32 0>") != std::string::npos);

    bool rejected_high_bits = false;
    try {
        (void)yir::ConstantMask(m10, std::vector<std::uint8_t>{0x00, 0x80});
    } catch (const std::invalid_argument &) {
        rejected_high_bits = true;
    }
    REQUIRE(rejected_high_bits);

    yir::Module bad;
    bad.add_global("wrong", v3, false)->set_initializer(mask);
    require_invalid(bad, "initializer type does not match");
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"fixed_vector_operations_and_printer", test_fixed_vector_operations_and_printer},
        {"verifier_rejects_shape_and_mask_errors", test_verifier_rejects_shape_and_mask_errors},
        {"verifier_rejects_memory_shuffle_and_reduction_errors", test_verifier_rejects_memory_shuffle_and_reduction_errors},
        {"typed_global_constants_and_mask_layout", test_typed_global_constants_and_mask_layout},
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
    with tempfile.TemporaryDirectory(prefix="yir-vector-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "yir_vector_tests.cpp"
        binary = tmp_dir / "yir_vector_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        compile_proc = subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(ROOT / "src/yir/YIR.cpp"),
                str(ROOT / "src/yir/YIRPrinter.cpp"),
                str(ROOT / "src/yir/YIRVerifier.cpp"),
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compile_proc.returncode != 0:
            print(compile_proc.stdout, end="")
            print(compile_proc.stderr, end="", file=sys.stderr)
            return compile_proc.returncode
        run_proc = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        print(run_proc.stdout, end="")
        print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
