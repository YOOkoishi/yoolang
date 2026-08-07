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
#include "oir/OIRParser.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string(__FILE__) + ":" +                            \
                                     std::to_string(__LINE__) +                                 \
                                     ": requirement failed: " #condition);                     \
        }                                                                                       \
    } while (false)

void require_verified(oir::Module &module) {
    std::string message;
    if (!module.verify(&message)) {
        throw std::runtime_error("expected verifier success: " + message);
    }
}

void require_roundtrip(oir::Module &module) {
    require_verified(module);
    const auto first = module.print();
    auto parsed = oir::OIRParser::parse(first, "roundtrip.oir");
    if (!parsed.ok()) {
        const auto &error = parsed.errors.front();
        throw std::runtime_error("roundtrip parse failed at " +
                                 std::to_string(error.range.begin.line) + ":" +
                                 std::to_string(error.range.begin.column) + ": " +
                                 error.message + "\n" + first);
    }
    REQUIRE(parsed.module->print() == first);
}

void require_parse_failure(const std::string &source, const std::string &needle,
                           std::size_t expected_line = 0) {
    bool threw = false;
    oir::OIRParseResult result;
    try {
        result = oir::OIRParser::parse(source, "negative.oir");
    } catch (...) {
        threw = true;
    }
    REQUIRE(!threw);
    REQUIRE(!result.ok());
    REQUIRE(result.module == nullptr);
    REQUIRE(!result.errors.empty());
    REQUIRE(result.errors.front().range.begin.line >= 1);
    REQUIRE(result.errors.front().range.begin.column >= 1);
    if (expected_line != 0) {
        if (result.errors.front().range.begin.line != expected_line) {
            throw std::runtime_error("expected diagnostic on line " +
                                     std::to_string(expected_line) + ", got line " +
                                     std::to_string(result.errors.front().range.begin.line) +
                                     ": " + result.errors.front().message);
        }
    }
    REQUIRE(result.errors.front().message.find(needle) != std::string::npos);
}

float f32_from_bits(std::uint32_t bits) {
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t f32_bits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void test_scalar_cfg_forward_phi_and_every_scalar_opcode_roundtrip() {
    oir::Module module("scalar-roundtrip");
    auto &types = module.types();
    auto *array_type = types.array_ty(types.int32_ty(), 4);
    module.create_global("counter", types.int32_ty(), false, module.create_i32(7));
    auto *callee = module.create_function(
        "callee", types.func_ty(types.int32_ty(), {types.int32_ty()}), true);
    auto *putf = module.create_function(
        "putf", types.func_ty(types.void_ty(), {types.int32_ty()}, true), true);

    auto *function = module.create_function(
        "scalar", types.func_ty(types.int32_ty(), {types.int32_ty()}));
    function->args()[0]->set_name("n");
    auto *entry = function->create_block("entry");
    auto *loop = function->create_block("loop");
    auto *exit = function->create_block("exit");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *slot = builder.create_alloca(types.int32_ty(), "slot");
    auto *array = builder.create_alloca(array_type, "array");
    builder.create_store(function->args()[0].get(), slot);
    auto *loaded = builder.create_load(slot, types.int32_ty(), "loaded");
    auto *add = builder.create_binary(oir::Instruction::OpID::Add, loaded, builder.i32(3), "add");
    auto *sub = builder.create_binary(oir::Instruction::OpID::Sub, add, builder.i32(1), "sub");
    auto *mul = builder.create_binary(oir::Instruction::OpID::Mul, sub, builder.i32(2), "mul");
    auto *div = builder.create_binary(oir::Instruction::OpID::SDiv, mul, builder.i32(2), "div");
    auto *rem = builder.create_binary(oir::Instruction::OpID::SRem, div, builder.i32(5), "rem");
    auto *bit_and = builder.create_binary(oir::Instruction::OpID::And, rem, builder.i32(7), "and");
    auto *bit_or = builder.create_binary(oir::Instruction::OpID::Or, bit_and, builder.i32(8), "or");
    auto *bit_xor = builder.create_binary(oir::Instruction::OpID::Xor, bit_or, builder.i32(1), "xor");
    auto *icmp = builder.create_icmp(oir::CmpPred::NE, bit_xor, builder.i32(0), "icmp");
    auto *extended = builder.create_zext(icmp, types.int32_ty(), "extended");
    auto *to_float = builder.create_sitofp(extended, types.float_ty(), "to.float");
    auto *fadd = builder.create_binary(oir::Instruction::OpID::FAdd, to_float, builder.f32(1.0F), "fadd");
    auto *fsub = builder.create_binary(oir::Instruction::OpID::FSub, fadd, builder.f32(2.0F), "fsub");
    auto *fmul = builder.create_binary(oir::Instruction::OpID::FMul, fsub, builder.f32(3.0F), "fmul");
    auto *fdiv = builder.create_binary(oir::Instruction::OpID::FDiv, fmul, builder.f32(4.0F), "fdiv");
    (void)builder.create_fcmp(oir::CmpPred::GE, fdiv, builder.f32(0.0F), "fcmp");
    auto *back_to_int = builder.create_fptosi(fdiv, types.int32_ty(), "back.to.int");
    builder.create_memzero(slot, builder.i32(4));
    builder.create_memset(slot, builder.i32(1), builder.i32(4));
    (void)builder.create_gep(array, types.ptr_ty(types.int32_ty()),
                             {builder.i32(0), builder.i32(2)}, "element");
    auto *call = builder.create_call(callee, types.int32_ty(), {back_to_int}, "call");
    builder.create_call(putf, types.void_ty(), {builder.i32(1), builder.f32(2.0F)});
    builder.create_br(loop);

    builder.set_insert_point(loop);
    auto *phi = builder.create_phi(types.int32_ty(), "iv");
    phi->add_incoming(call, entry);
    auto *next = builder.create_binary(oir::Instruction::OpID::Add, phi, builder.i32(1), "next");
    phi->add_incoming(next, loop);
    auto *keep_going = builder.create_icmp(oir::CmpPred::LT, next, builder.i32(9), "keep.going");
    builder.create_cond_br(keep_going, loop, exit);

    builder.set_insert_point(exit);
    builder.create_ret(phi);
    require_roundtrip(module);
}

void test_typed_globals_vectors_masks_zero_and_variadic_types_roundtrip() {
    oir::Module module("typed-constants");
    auto &types = module.types();
    auto *array = types.array_ty(types.int32_ty(), 2);
    auto *vector = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 10);
    module.create_global("zero", array, false, module.create_zero(array));
    module.create_global("array", array, true,
                         module.create_constant_array(array, {module.create_i32(1),
                                                              module.create_i32(2)}));
    module.create_global("vector", vector, true,
                         module.create_constant_vector(vector, {module.create_i32(3),
                                                                 module.create_i32(4),
                                                                 module.create_i32(5)}));
    module.create_global("mask", mask, true,
                         module.create_constant_mask(mask, {0x55U, 0x01U}));
    auto *fixed = types.func_ty(types.void_ty(), {types.int32_ty()});
    auto *variadic = types.func_ty(types.void_ty(), {types.int32_ty()}, true);
    REQUIRE(fixed != variadic);
    REQUIRE(variadic == types.func_ty(types.void_ty(), {types.int32_ty()}, true));
    REQUIRE(variadic->is_variadic());
    REQUIRE(variadic->print() == "void (i32, ...)");
    module.create_function("sink", variadic, true);
    auto *vector_decl = module.create_function(
        "foreign_vector", types.func_ty(vector, {vector, mask}), true);
    vector_decl->args()[0]->set_name("value");
    vector_decl->args()[1]->set_name("active");
    REQUIRE(vector_decl->is_external());
    REQUIRE(vector_decl->blocks().empty());
    require_roundtrip(module);
}

void test_binary32_constants_roundtrip_bit_exactly() {
    constexpr std::uint32_t patterns[] = {
        0x00000000U, 0x80000000U, 0x00000001U, 0x007fffffU, 0x00800000U,
        0x3f800001U, 0x7f7fffffU, 0x7f800000U, 0xff800000U, 0x7fc12345U,
    };

    oir::Module module("binary32-roundtrip");
    auto &types = module.types();
    std::vector<oir::Constant *> lanes;
    lanes.reserve(std::size(patterns));
    for (std::size_t index = 0; index < std::size(patterns); ++index) {
        auto *constant = module.create_f32(f32_from_bits(patterns[index]));
        module.create_global("f" + std::to_string(index), types.float_ty(), true, constant);
        lanes.push_back(constant);
    }
    auto *vector_type = types.fixed_vector_ty(types.float_ty(), std::size(patterns));
    module.create_global("fv", vector_type, true,
                         module.create_constant_vector(vector_type, lanes));

    require_verified(module);
    const auto canonical = module.print();
    REQUIRE(canonical.find("0x00000001") != std::string::npos);
    REQUIRE(canonical.find("0x80000000") != std::string::npos);
    REQUIRE(canonical.find("0x7fc12345") != std::string::npos);
    auto parsed = oir::OIRParser::parse(canonical, "binary32-roundtrip.oir");
    REQUIRE(parsed.ok());
    REQUIRE(parsed.module->print() == canonical);
    for (std::size_t index = 0; index < std::size(patterns); ++index) {
        const auto *global = parsed.module->get_global("f" + std::to_string(index));
        REQUIRE(global != nullptr);
        const auto *constant = dynamic_cast<const oir::ConstantFloat *>(global->initializer());
        REQUIRE(constant != nullptr);
        REQUIRE(f32_bits(constant->value()) == patterns[index]);
    }

    const std::string legacy_decimal =
        "; module: decimal-subnormal\n\n@g = constant float 1.40129846e-45\n";
    auto decimal = oir::OIRParser::parse(legacy_decimal, "decimal-subnormal.oir");
    REQUIRE(decimal.ok());
    const auto *subnormal = dynamic_cast<const oir::ConstantFloat *>(
        decimal.module->get_global("g")->initializer());
    REQUIRE(subnormal != nullptr);
    REQUIRE(f32_bits(subnormal->value()) == 0x00000001U);
    REQUIRE(decimal.module->print().find("0x00000001") != std::string::npos);

    require_parse_failure("; module: bad\n\n@g = constant float 0x1234\n",
                          "exactly eight hexadecimal digits", 3);
    require_parse_failure("; module: bad\n\n@g = constant float 1e999\n",
                          "floating-point constant is out of range", 3);
    require_parse_failure("; module: bad\n\n@g = constant i32 2147483648\n",
                          "i32 constant is out of range", 3);
    require_parse_failure("; module: bad\n\n@g = constant i32 -2147483649\n",
                          "i32 constant is out of range", 3);
}

void test_vector_and_vp_operation_families_roundtrip() {
    oir::Module module("vector-vp-roundtrip");
    auto &types = module.types();
    auto *v4i = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *v4f = types.fixed_vector_ty(types.float_ty(), 4);
    auto *m4 = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *nxv4i = types.scalable_vector_ty(types.int32_ty(), 4);
    auto *nxm4 = types.scalable_vector_ty(types.int1_ty(), 4);
    auto *base = module.create_global("base", types.int32_ty(), false, module.create_i32(0));
    auto *function = module.create_function("vectors", types.func_ty(types.void_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *splat = builder.create_splat(v4i, builder.i32(7), "splat");
    auto *mask = builder.create_splat(m4, builder.i1(true), "mask");
    auto *step = builder.create_step_vector(v4i, "step");
    auto *normal_or = builder.create_binary(oir::Instruction::OpID::Or, mask, mask, "mask.or");
    auto *normal_cmp = builder.create_icmp(oir::CmpPred::LT, splat, step, "normal.cmp");
    auto *extract = builder.create_extract_element(splat, builder.i32(2), "extract");
    auto *insert = builder.create_insert_element(step, extract, builder.i32(1), "insert");
    auto *shuffle = builder.create_shuffle_vector(v4i, insert, splat, {0, 5, -1, 3}, "shuffle");
    auto *select = builder.create_vector_select(normal_cmp, shuffle, insert, "select");
    auto *zext = builder.create_vector_cast(oir::VectorCastKind::ZExt, v4i, normal_or, "zext");
    auto *to_float = builder.create_vector_cast(oir::VectorCastKind::SIToFP, v4f, select, "to.float");
    auto *to_int = builder.create_vector_cast(oir::VectorCastKind::FPToSI, v4i, to_float, "to.int");
    (void)builder.create_vector_cast(oir::VectorCastKind::Bitcast, v4f, zext, "bitcast");

    auto *active = module.create_undef(m4);
    auto *evl = builder.i32(4);
    auto *vp_or = builder.create_vp_binary(
        oir::Instruction::OpID::Or, to_int, splat, active, evl, module.create_undef(v4i),
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, "vp.or");
    auto *vp_fadd = builder.create_vp_binary(
        oir::Instruction::OpID::FAdd, to_float, to_float, active, evl,
        module.create_undef(v4f), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Undisturbed, "vp.fadd");
    (void)builder.create_vp_icmp(
        oir::CmpPred::GE, vp_or, splat, active, evl, module.create_undef(m4),
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, "vp.icmp");
    (void)builder.create_vp_fcmp(
        oir::CmpPred::NE, vp_fadd, to_float, active, evl, module.create_undef(m4),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "vp.fcmp");
    builder.create_vp_store(vp_or, base, active, evl, oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    (void)builder.create_vp_load(v4i, base, active, evl, module.create_undef(v4i),
                                 oir::TailPolicy::Undisturbed,
                                 oir::MaskPolicy::Undisturbed, 4, "vp.load");
    builder.create_masked_store(vp_or, base, active, evl, oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
    (void)builder.create_masked_load(v4i, base, active, evl, module.create_undef(v4i),
                                     oir::TailPolicy::Agnostic,
                                     oir::MaskPolicy::Undisturbed, 4, "masked.load");
    (void)builder.create_vp_gather(v4i, base, step, active, evl, module.create_undef(v4i),
                                   oir::TailPolicy::Undisturbed,
                                   oir::MaskPolicy::Undisturbed, 4, "gather");
    builder.create_vp_scatter(vp_or, base, step, active, evl, oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    (void)builder.create_vp_reduction(
        oir::ReductionKind::Or, false, vp_or, active, evl, builder.i32(0),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce.or");
    (void)builder.create_vp_reduction(
        oir::ReductionKind::Add, true, vp_fadd, active, evl, builder.f32(0.0F),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce.fadd");

    auto *nx_lhs = builder.create_splat(nxv4i, builder.i32(1), "nx.lhs");
    auto *nx_rhs = builder.create_step_vector(nxv4i, "nx.rhs");
    auto *nx_active = builder.create_splat(nxm4, builder.i1(true), "nx.active");
    (void)builder.create_vp_binary(
        oir::Instruction::OpID::Add, nx_lhs, nx_rhs, nx_active, builder.i32(9),
        module.create_undef(nxv4i), oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Agnostic, "nx.add");
    builder.create_ret();
    require_roundtrip(module);
}

void test_typed_array_global_and_function_pointer_type_roundtrip() {
    const std::string source =
        "; module: typed\n\n"
        "@table = global [2 x i32] [i32 1, i32 2]\n"
        "@callback = global i32 (i32, ...)*\n\n"
        "declare i32 @callee(i32 %x, ...)\n\n";
    auto parsed = oir::OIRParser::parse(source, "typed.oir");
    REQUIRE(parsed.ok());
    REQUIRE(dynamic_cast<oir::ConstantArray *>(
                parsed.module->get_global("table")->initializer()) != nullptr);
    const auto canonical = parsed.module->print();
    auto reparsed = oir::OIRParser::parse(canonical, "typed-canonical.oir");
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.module->print() == canonical);
}

void test_malformed_inputs_are_ranged_and_never_throw() {
    require_parse_failure("; module: bad\n\n#\n", "unexpected character", 3);
    require_parse_failure("; module: bad\n\n@g = global [2 x i32] {1, 2}\n",
                          "legacy textual global initializers are not supported", 3);
    require_parse_failure(
        "; module: bad\n\ndefine i32 @f() {\nentry:\n  ret i32 %missing\n}\n",
        "unresolved SSA value", 5);
    require_parse_failure(
        "; module: bad\n\ndefine void @f() {\nentry:\n  br %nowhere\n}\n",
        "unknown basic block", 5);
    require_parse_failure("; module: bad\n\n@g = constant <3 x i32> <i32 1, i32 2>\n",
                          "lane count mismatch", 3);
    require_parse_failure(
        "; module: bad\n\n@base = global i32 0\n\ndefine void @f() {\nentry:\n"
        "  %v = vp.load <4 x i32>, i32* @base, align 3, mask <4 x i1> undef, "
        "evl i32 4, passthrough <4 x i32> undef, tail=agnostic, mask-policy=agnostic\n"
        "  ret void\n}\n",
        "OIRV_VP_MEMORY_ALIGNMENT");
    require_parse_failure(
        "; module: bad\n\ndefine void @f() {\nentry:\n"
        "  %v = vp.add <4 x i32> undef, undef, mask <4 x i1> undef, evl i32 4, "
        "passthrough <4 x i32> undef, tail=broken, mask-policy=agnostic\n"
        "  ret void\n}\n",
        "unknown tail policy", 5);
    require_parse_failure(
        "; module: bad\n\ndefine void @f() {\nentry:\n"
        "  %v = shufflevector <4 x i32> undef, <4 x i32> undef, [0, 1, 2, 8] to <4 x i32>\n"
        "  ret void\n}\n",
        "OIRV_SHUFFLE_BOUNDS");
    require_parse_failure(
        "; module: bad\n\ndefine void @f() {\nentry:\n"
        "  br <4 x i1> undef, %yes, %no\nyes:\n  ret void\nno:\n  ret void\n}\n",
        "OIRV_BRANCH_CONDITION");
    require_parse_failure(
        "; module: bad\n\ndeclare void @v(i32 %fixed, ...)\n\n"
        "define void @f() {\nentry:\n  call void @v()\n  ret void\n}\n",
        "OIRV_CALL_ARITY");
    require_parse_failure(
        "; module: bad\n\ndeclare void @v(...)\n\n"
        "define void @f() {\nentry:\n  call void @v(<vscale x 4 x i32> undef)\n  ret void\n}\n",
        "OIRV_SCALABLE_ABI");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"scalar_cfg_forward_phi_and_every_scalar_opcode_roundtrip",
         test_scalar_cfg_forward_phi_and_every_scalar_opcode_roundtrip},
        {"typed_globals_vectors_masks_zero_and_variadic_types_roundtrip",
         test_typed_globals_vectors_masks_zero_and_variadic_types_roundtrip},
        {"binary32_constants_roundtrip_bit_exactly",
         test_binary32_constants_roundtrip_bit_exactly},
        {"vector_and_vp_operation_families_roundtrip",
         test_vector_and_vp_operation_families_roundtrip},
        {"typed_array_global_and_function_pointer_type_roundtrip",
         test_typed_array_global_and_function_pointer_type_roundtrip},
        {"malformed_inputs_are_ranged_and_never_throw",
         test_malformed_inputs_are_ranged_and_never_throw},
    };
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &exception) {
            std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
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
        path = shutil.which(candidate)
        if path:
            return path
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="oir-parser-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "oir_parser_infra_tests.cpp"
        binary = tmp_dir / "oir_parser_infra_tests"
        source.write_text(textwrap.dedent(SOURCE))
        compile_cmd = [
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
        ]
        compiled = subprocess.run(
            compile_cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compiled.returncode != 0:
            if compiled.stdout:
                print(compiled.stdout, end="")
            if compiled.stderr:
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
        if ran.stdout:
            print(ran.stdout, end="")
        if ran.stderr:
            print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
