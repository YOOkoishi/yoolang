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
#include "target/RISCVCallingConvention.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

target::TargetProfile vector_profile(const std::string &march = "rv64gcv") {
    target::TargetProfile profile;
    profile.march = march;
    std::string error;
    if (!target::finalize_target_profile(profile, error)) {
        throw std::runtime_error("failed to make vector profile: " + error);
    }
    REQUIRE(profile.vector_abi == target::VectorABI::Standard);
    return profile;
}

target::CCPSABIVectorValue value(const oir::Type *type, unsigned tuple_fields = 1) {
    return {type, tuple_fields};
}

const target::CCPSABIVectorRegisterGroup &
group(const target::CCPSABIVectorValueAssignment &assignment) {
    REQUIRE(assignment.vector_group.has_value());
    return *assignment.vector_group;
}

void test_register_convention_metadata() {
    const auto &convention = target::riscv_psabi_vector_register_convention();
    REQUIRE(convention.first_mask_argument_register == 0);
    for (unsigned index = 0; index < convention.data_argument_registers.size(); ++index) {
        REQUIRE(convention.data_argument_registers[index] == index + 8);
    }
    REQUIRE(convention.call_clobbered_vector_registers[0] == 0);
    for (unsigned index = 1; index < convention.call_clobbered_vector_registers.size(); ++index) {
        REQUIRE(convention.call_clobbered_vector_registers[index] == index + 7);
    }
    for (unsigned index = 0; index < 7; ++index) {
        REQUIRE(convention.callee_saved_vector_registers[index] == index + 1);
    }
    for (unsigned index = 7; index < convention.callee_saved_vector_registers.size(); ++index) {
        REQUIRE(convention.callee_saved_vector_registers[index] == index + 17);
    }
    REQUIRE(!convention.preserves_vl);
    REQUIRE(!convention.preserves_vtype);
    REQUIRE(!convention.preserves_vxrm);
    REQUIRE(!convention.preserves_vxsat);
    REQUIRE(convention.requires_vstart_zero_on_entry);
    REQUIRE(convention.requires_vstart_zero_after_call);
}

void test_lmul_alignment_and_search_restart() {
    oir::TypeContext types;
    auto *m1 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *m2 = types.fixed_vector_ty(types.int32_ty(), 8);
    auto *m4 = types.fixed_vector_ty(types.int32_ty(), 16);
    auto *m8 = types.fixed_vector_ty(types.int32_ty(), 32);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);

    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(types.void_ty());
    signature.parameters = {value(m1), value(m2), value(m1), value(m4), value(m8)};
    auto result = cc.assign(signature);
    REQUIRE(result.valid);
    REQUIRE(group(result.parameters[0]).first_register == 8);
    REQUIRE(group(result.parameters[0]).register_count == 1);
    REQUIRE(result.parameters[0].lmul == 1);
    REQUIRE(group(result.parameters[1]).first_register == 10);
    REQUIRE(group(result.parameters[1]).register_count == 2);
    REQUIRE(result.parameters[1].lmul == 2);
    // The psABI restarts its search at v8 for every value, filling v9 here.
    REQUIRE(group(result.parameters[2]).first_register == 9);
    REQUIRE(group(result.parameters[3]).first_register == 12);
    REQUIRE(group(result.parameters[3]).register_count == 4);
    REQUIRE(result.parameters[3].lmul == 4);
    REQUIRE(group(result.parameters[4]).first_register == 16);
    REQUIRE(group(result.parameters[4]).register_count == 8);
    REQUIRE(result.parameters[4].lmul == 8);
    for (const auto &assignment : result.parameters) {
        REQUIRE(!assignment.value.indirect);
        REQUIRE(assignment.value.locations.empty());
        REQUIRE(group(assignment).first_register % assignment.lmul == 0);
    }
}

void test_mask_v0_and_later_mask_allocation() {
    oir::TypeContext types;
    auto *m1 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 31);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);

    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(mask);
    signature.parameters = {value(m1), value(mask), value(mask), value(m1)};
    auto result = cc.assign(signature);
    REQUIRE(result.valid);
    REQUIRE(result.return_value.kind == target::CCPSABIVectorValueKind::Mask);
    REQUIRE(group(result.return_value).first_register == 0);
    REQUIRE(group(result.parameters[0]).first_register == 8);
    REQUIRE(result.parameters[1].kind == target::CCPSABIVectorValueKind::Mask);
    REQUIRE(group(result.parameters[1]).first_register == 0);
    REQUIRE(group(result.parameters[2]).first_register == 9);
    REQUIRE(group(result.parameters[2]).lmul == 1);
    REQUIRE(group(result.parameters[3]).first_register == 10);

    auto *oversized_mask = types.fixed_vector_ty(types.int1_ty(), 129);
    target::CCPSABIVectorFunctionType oversized_first;
    oversized_first.return_value = value(types.void_ty());
    oversized_first.parameters = {value(oversized_mask), value(mask)};
    auto oversized_result = cc.assign(oversized_first);
    REQUIRE(oversized_result.valid);
    REQUIRE(oversized_result.parameters[0].value.indirect);
    REQUIRE(oversized_result.parameters[0].value.locations[0].register_name == "a0");
    REQUIRE(group(oversized_result.parameters[1]).first_register == 8);
}

void test_tuple_consecutive_groups_and_hole_reuse() {
    oir::TypeContext types;
    auto *m1 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *m2 = types.fixed_vector_ty(types.int32_ty(), 8);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);

    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(m2, 3);
    signature.parameters = {value(m1), value(m2), value(m1, 2), value(m1)};
    auto result = cc.assign(signature);
    REQUIRE(result.valid);
    REQUIRE(result.return_value.kind == target::CCPSABIVectorValueKind::Tuple);
    REQUIRE(group(result.return_value).first_register == 8);
    REQUIRE(group(result.return_value).lmul == 2);
    REQUIRE(group(result.return_value).tuple_fields == 3);
    REQUIRE(group(result.return_value).register_count == 6);

    REQUIRE(group(result.parameters[0]).first_register == 8);
    REQUIRE(group(result.parameters[1]).first_register == 10);
    REQUIRE(result.parameters[2].kind == target::CCPSABIVectorValueKind::Tuple);
    REQUIRE(group(result.parameters[2]).first_register == 12);
    REQUIRE(group(result.parameters[2]).register_count == 2);
    REQUIRE(group(result.parameters[2]).tuple_fields == 2);
    REQUIRE(group(result.parameters[3]).first_register == 9);

    target::CCPSABIVectorFunctionType m4x2;
    auto *m4 = types.fixed_vector_ty(types.int32_ty(), 16);
    m4x2.return_value = value(types.void_ty());
    m4x2.parameters = {value(m4, 2)};
    auto m4x2_result = cc.assign(m4x2);
    REQUIRE(m4x2_result.valid);
    REQUIRE(group(m4x2_result.parameters[0]).first_register == 8);
    REQUIRE(group(m4x2_result.parameters[0]).register_count == 8);
}

void test_exhaustion_uses_indirect_pointer_and_stack() {
    oir::TypeContext types;
    auto *m8 = types.fixed_vector_ty(types.int32_ty(), 32);
    auto *m1 = types.fixed_vector_ty(types.int32_ty(), 4);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);

    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(types.void_ty());
    signature.parameters.assign(8, value(types.int32_ty()));
    signature.parameters.push_back(value(m8));
    signature.parameters.push_back(value(m8));
    signature.parameters.push_back(value(m1));
    signature.parameters.push_back(value(m1));
    auto result = cc.assign(signature);
    REQUIRE(result.valid);
    REQUIRE(group(result.parameters[8]).first_register == 8);
    REQUIRE(group(result.parameters[9]).first_register == 16);
    for (unsigned index = 10; index < 12; ++index) {
        const auto &fallback = result.parameters[index];
        REQUIRE(fallback.value.indirect);
        REQUIRE(!fallback.vector_group.has_value());
        REQUIRE(fallback.value.locations.size() == 1);
        REQUIRE(fallback.value.locations[0].kind == target::CCLocationKind::Stack);
        REQUIRE(fallback.value.locations[0].stack_offset == (index - 10) * 8);
        REQUIRE(fallback.value.locations[0].size == 8);
    }
    REQUIRE(result.stack_argument_size == 16);

    target::CCPSABIVectorFunctionType gpr_fallback;
    gpr_fallback.return_value = value(types.void_ty());
    gpr_fallback.parameters = {value(m8), value(m8), value(m1), value(types.int32_ty())};
    auto gpr_result = cc.assign(gpr_fallback);
    REQUIRE(gpr_result.valid);
    REQUIRE(gpr_result.parameters[2].value.indirect);
    REQUIRE(gpr_result.parameters[2].value.locations[0].register_name == "a0");
    REQUIRE(gpr_result.parameters[3].value.locations[0].register_name == "a1");
}

void test_returns_and_indirect_sret() {
    oir::TypeContext types;
    auto *m2 = types.fixed_vector_ty(types.int32_ty(), 8);
    auto *m4 = types.fixed_vector_ty(types.int32_ty(), 16);
    auto *m8 = types.fixed_vector_ty(types.int32_ty(), 32);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);

    for (const auto &[type, expected_lmul] :
         std::vector<std::pair<oir::Type *, unsigned>>{{m2, 2}, {m4, 4}, {m8, 8}}) {
        target::CCPSABIVectorFunctionType signature;
        signature.return_value = value(type);
        signature.parameters = {value(type)};
        auto result = cc.assign(signature);
        REQUIRE(result.valid);
        REQUIRE(!result.has_sret);
        REQUIRE(result.return_value.lmul == expected_lmul);
        REQUIRE(group(result.return_value).first_register == 8);
        REQUIRE(group(result.return_value).register_count == expected_lmul);
        // Return and parameter allocation are independent and may overlap.
        REQUIRE(group(result.parameters[0]).first_register == 8);
    }

    auto *oversized = types.fixed_vector_ty(types.int32_ty(), 33);
    target::CCPSABIVectorFunctionType indirect;
    indirect.return_value = value(oversized);
    indirect.parameters = {value(types.int32_ty()), value(oversized)};
    auto result = cc.assign(indirect);
    REQUIRE(result.valid);
    REQUIRE(result.has_sret);
    REQUIRE(result.return_value.value.indirect);
    REQUIRE(result.return_value.value.locations[0].register_name == "a0");
    REQUIRE(result.parameters[0].value.locations[0].register_name == "a1");
    REQUIRE(result.parameters[1].value.indirect);
    REQUIRE(result.parameters[1].value.locations[0].register_name == "a2");

    auto *oversized_mask = types.fixed_vector_ty(types.int1_ty(), 129);
    target::CCPSABIVectorFunctionType mask_indirect;
    mask_indirect.return_value = value(oversized_mask);
    auto mask_result = cc.assign(mask_indirect);
    REQUIRE(mask_result.valid);
    REQUIRE(mask_result.has_sret);
    REQUIRE(mask_result.return_value.value.indirect);
    REQUIRE(!mask_result.return_value.vector_group.has_value());
}

void test_standard_scalar_rules_are_unchanged() {
    oir::TypeContext types;
    auto profile = vector_profile();
    target::RISCVCallingConvention standard(profile);
    target::RISCVPSABIVectorCallingConvention vector_cc(profile, 128);
    std::vector<oir::Type *> params = {types.int32_ty(), types.float_ty(),
                                       types.ptr_ty(types.int32_ty())};
    auto *standard_type = types.func_ty(types.int32_ty(), params);
    auto expected = standard.assign(*standard_type);

    target::CCPSABIVectorFunctionType vector_type;
    vector_type.return_value = value(types.int32_ty());
    for (auto *param : params) {
        vector_type.parameters.push_back(value(param));
    }
    auto actual = vector_cc.assign(vector_type);
    REQUIRE(expected.valid && actual.valid);
    REQUIRE(actual.return_value.kind == target::CCPSABIVectorValueKind::NonVector);
    REQUIRE(actual.return_value.value.locations[0].register_name ==
            expected.return_value.locations[0].register_name);
    for (unsigned index = 0; index < params.size(); ++index) {
        REQUIRE(actual.parameters[index].kind == target::CCPSABIVectorValueKind::NonVector);
        REQUIRE(actual.parameters[index].value.locations[0].kind ==
                expected.parameters[index].locations[0].kind);
        REQUIRE(actual.parameters[index].value.locations[0].register_name ==
                expected.parameters[index].locations[0].register_name);
    }
}

void test_fail_closed_profiles_and_illegal_shapes() {
    oir::TypeContext types;
    auto profile = vector_profile();
    target::RISCVPSABIVectorCallingConvention bad_vlen(profile, 256);
    target::CCPSABIVectorFunctionType scalar;
    scalar.return_value = value(types.void_ty());
    auto bad_vlen_result = bad_vlen.assign(scalar);
    REQUIRE(!bad_vlen_result.valid);
    REQUIRE(bad_vlen_result.error.find("ABI_VLEN") != std::string::npos);

    target::TargetProfile scalar_profile;
    std::string error;
    REQUIRE(target::finalize_target_profile(scalar_profile, error));
    target::RISCVPSABIVectorCallingConvention no_vector(scalar_profile, 128);
    auto no_vector_result = no_vector.assign(scalar);
    REQUIRE(!no_vector_result.valid);
    REQUIRE(no_vector_result.error.find("V or Zve") != std::string::npos);

    // The compiler-facing profile requires an explicit numeric ABI_VLEN.  The
    // classifier itself stays independently usable with the ABI_VLEN passed to
    // its constructor.
    auto missing_bits_profile = vector_profile();
    REQUIRE(target::parse_vector_abi("psabi-vector", missing_bits_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(missing_bits_profile, error));
    REQUIRE(error.find("explicit numeric") != std::string::npos);

    target::TargetProfile gated_profile;
    gated_profile.march = "rv64gcv_zvl128b";
    REQUIRE(target::parse_rvv_deployment("compile-time", gated_profile, error));
    REQUIRE(target::parse_vector_bits("128", gated_profile, error));
    REQUIRE(target::parse_vector_abi("psabi-vector", gated_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(gated_profile, error));
    REQUIRE(error.find("PSABI_VECTOR_ABI_UNAVAILABLE") != std::string::npos);
    REQUIRE(error.find("vector tuple") != std::string::npos);
    REQUIRE(error.find("GCC/Clang") != std::string::npos);
    REQUIRE(gated_profile.vector_abi == target::VectorABI::PsABIVector);
    REQUIRE(gated_profile.fixed_vector_bits.has_value());
    REQUIRE(*gated_profile.fixed_vector_bits == 128);

    target::TargetProfile mismatched_profile;
    mismatched_profile.march = "rv64gcv_zvl128b";
    REQUIRE(target::parse_rvv_deployment("compile-time", mismatched_profile, error));
    REQUIRE(target::parse_vector_bits("256", mismatched_profile, error));
    REQUIRE(target::parse_vector_abi("psabi-vector", mismatched_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(mismatched_profile, error));
    REQUIRE(error.find("must match") != std::string::npos);

    target::RISCVPSABIVectorCallingConvention cc(profile, 128);
    auto *scalable = types.scalable_vector_ty(types.int32_ty(), 4);
    target::CCPSABIVectorFunctionType scalable_signature;
    scalable_signature.return_value = value(types.void_ty());
    scalable_signature.parameters = {value(scalable)};
    auto scalable_result = cc.assign(scalable_signature);
    REQUIRE(!scalable_result.valid);
    REQUIRE(scalable_result.error.find("scalable") != std::string::npos);

    target::CCPSABIVectorFunctionType scalable_return;
    scalable_return.return_value = value(scalable);
    auto scalable_return_result = cc.assign(scalable_return);
    REQUIRE(!scalable_return_result.valid);
    REQUIRE(scalable_return_result.error.find("scalable") != std::string::npos);

    auto *mask = types.fixed_vector_ty(types.int1_ty(), 31);
    target::CCPSABIVectorFunctionType mask_tuple;
    mask_tuple.return_value = value(types.void_ty());
    mask_tuple.parameters = {value(mask, 2)};
    auto mask_tuple_result = cc.assign(mask_tuple);
    REQUIRE(!mask_tuple_result.valid);
    REQUIRE(mask_tuple_result.error.find("mask tuple") != std::string::npos);

    auto *m4 = types.fixed_vector_ty(types.int32_ty(), 16);
    target::CCPSABIVectorFunctionType illegal_tuple;
    illegal_tuple.return_value = value(types.void_ty());
    illegal_tuple.parameters = {value(m4, 3)};
    auto illegal_tuple_result = cc.assign(illegal_tuple);
    REQUIRE(!illegal_tuple_result.valid);
    REQUIRE(illegal_tuple_result.error.find("LMUL times NFIELDS") != std::string::npos);

    target::CCPSABIVectorFunctionType unprototyped;
    unprototyped.return_value = value(types.void_ty());
    unprototyped.parameters = {value(m4)};
    unprototyped.prototyped = false;
    auto unprototyped_result = cc.assign(unprototyped);
    REQUIRE(!unprototyped_result.valid);
    REQUIRE(unprototyped_result.error.find("unprototyped") != std::string::npos);
}

void test_variadic_vector_rejection_and_scalar_tail() {
    oir::TypeContext types;
    auto *m1 = types.fixed_vector_ty(types.float_ty(), 4);
    target::RISCVPSABIVectorCallingConvention cc(vector_profile(), 128);
    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(types.void_ty());
    signature.parameters = {value(m1), value(types.float_ty())};
    signature.variadic = true;

    auto entry = cc.assign(signature);
    REQUIRE(entry.valid);
    REQUIRE(group(entry.parameters[0]).first_register == 8);
    REQUIRE(entry.parameters[1].value.locations[0].register_name == "fa0");

    auto scalar_tail = cc.assign_call(
        signature, {value(m1), value(types.float_ty()), value(types.float_ty())});
    REQUIRE(scalar_tail.valid);
    REQUIRE(scalar_tail.parameters[1].value.locations[0].register_name == "fa0");
    REQUIRE(scalar_tail.parameters[2].value.locations[0].register_name == "a0");

    auto vector_tail =
        cc.assign_call(signature, {value(m1), value(types.float_ty()), value(m1)});
    REQUIRE(!vector_tail.valid);
    REQUIRE(vector_tail.error.find("variadic vector") != std::string::npos);

    auto *mask = types.fixed_vector_ty(types.int1_ty(), 31);
    auto mask_tail =
        cc.assign_call(signature, {value(m1), value(types.float_ty()), value(mask)});
    REQUIRE(!mask_tail.valid);
    REQUIRE(mask_tail.error.find("variadic vector") != std::string::npos);

    auto tuple_tail =
        cc.assign_call(signature, {value(m1), value(types.float_ty()), value(m1, 2)});
    REQUIRE(!tuple_tail.valid);
    REQUIRE(tuple_tail.error.find("variadic vector") != std::string::npos);

    auto wrong_fixed =
        cc.assign_call(signature, {value(m1, 2), value(types.float_ty())});
    REQUIRE(!wrong_fixed.valid);
    REQUIRE(wrong_fixed.error.find("tuple shape") != std::string::npos);
}

void test_embedded_abi_vlen_32_is_classifiable() {
    oir::TypeContext types;
    auto profile = vector_profile("rv64gc_zve32x");
    REQUIRE(!profile.features.v);
    REQUIRE(profile.features.zve32x);
    REQUIRE(profile.minimum_vlen_bits == 32);
    target::RISCVPSABIVectorCallingConvention cc(profile, 32);
    target::CCPSABIVectorFunctionType signature;
    signature.return_value = value(types.void_ty());
    signature.parameters = {value(types.fixed_vector_ty(types.int32_ty(), 1))};
    auto result = cc.assign(signature);
    REQUIRE(result.valid);
    REQUIRE(group(result.parameters[0]).first_register == 8);
    REQUIRE(group(result.parameters[0]).lmul == 1);

    target::CCPSABIVectorFunctionType float_signature;
    float_signature.return_value = value(types.void_ty());
    float_signature.parameters = {
        value(types.fixed_vector_ty(types.float_ty(), 1))};
    auto float_result = cc.assign(float_signature);
    REQUIRE(!float_result.valid);
    REQUIRE(float_result.error.find("f32 vector support") != std::string::npos);
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"register_convention_metadata", test_register_convention_metadata},
        {"lmul_alignment_and_search_restart", test_lmul_alignment_and_search_restart},
        {"mask_v0_and_later_mask_allocation", test_mask_v0_and_later_mask_allocation},
        {"tuple_consecutive_groups_and_hole_reuse", test_tuple_consecutive_groups_and_hole_reuse},
        {"exhaustion_uses_indirect_pointer_and_stack", test_exhaustion_uses_indirect_pointer_and_stack},
        {"returns_and_indirect_sret", test_returns_and_indirect_sret},
        {"standard_scalar_rules_are_unchanged", test_standard_scalar_rules_are_unchanged},
        {"fail_closed_profiles_and_illegal_shapes", test_fail_closed_profiles_and_illegal_shapes},
        {"variadic_vector_rejection_and_scalar_tail", test_variadic_vector_rejection_and_scalar_tail},
        {"embedded_abi_vlen_32_is_classifiable", test_embedded_abi_vlen_32_is_classifiable},
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
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="riscv-psabi-vector-cc-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "psabi_vector_calling_convention_tests.cpp"
        binary = tmp_dir / "psabi_vector_calling_convention_tests"
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
                str(ROOT / "src/oir/OIR.cpp"),
                str(ROOT / "src/oir/OIRAnalysis.cpp"),
                str(ROOT / "src/oir/OIRDataLayout.cpp"),
                str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
                str(ROOT / "src/target/RISCVCallingConvention.cpp"),
                str(ROOT / "src/target/TargetMachine.cpp"),
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
