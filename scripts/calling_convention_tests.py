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

target::TargetProfile standard_profile() {
    target::TargetProfile profile;
    profile.vector_abi = target::VectorABI::Standard;
    return profile;
}

void test_scalar_lp64d_classification() {
    oir::TypeContext types;
    auto *signature = types.func_ty(types.int32_ty(),
                                    {types.int32_ty(), types.float_ty(), types.ptr_ty(types.int32_ty())});
    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*signature);
    REQUIRE(result.valid);
    REQUIRE(!result.has_sret);
    REQUIRE(result.return_value.locations[0].register_name == "a0");
    REQUIRE(result.parameters[0].locations[0].register_name == "a0");
    REQUIRE(result.parameters[1].locations[0].register_name == "fa0");
    REQUIRE(result.parameters[2].locations[0].register_name == "a1");
}

void test_direct_aggregate_size_boundaries() {
    oir::TypeContext types;
    target::RISCVCallingConvention cc(standard_profile());

    const struct Case {
        oir::Type *type;
        std::uint64_t size;
        std::size_t pieces;
        std::uint64_t last_piece_size;
    } cases[] = {
        {types.fixed_vector_ty(types.int32_ty(), 1), 4, 1, 4},
        {types.fixed_vector_ty(types.int32_ty(), 2), 8, 1, 8},
        {types.fixed_vector_ty(types.int32_ty(), 3), 12, 2, 4},
        {types.fixed_vector_ty(types.int32_ty(), 4), 16, 2, 8},
    };

    for (const auto &test : cases) {
        auto result = cc.assign(*types.func_ty(test.type, {test.type}));
        REQUIRE(result.valid);
        REQUIRE(!result.has_sret);
        REQUIRE(!result.return_value.indirect);
        REQUIRE(result.return_value.size == test.size);
        REQUIRE(result.return_value.alignment == 4);
        REQUIRE(result.return_value.locations.size() == test.pieces);
        REQUIRE(result.return_value.locations[0].kind == target::CCLocationKind::GPR);
        REQUIRE(result.return_value.locations[0].register_name == "a0");
        REQUIRE(result.return_value.locations[0].value_offset == 0);
        REQUIRE(result.return_value.locations[0].size == std::min<std::uint64_t>(test.size, 8));
        REQUIRE(result.return_value.locations.back().size == test.last_piece_size);
        if (test.pieces == 2) {
            REQUIRE(result.return_value.locations[1].register_name == "a1");
            REQUIRE(result.return_value.locations[1].value_offset == 8);
        }

        REQUIRE(result.parameters.size() == 1);
        REQUIRE(!result.parameters[0].indirect);
        REQUIRE(result.parameters[0].size == test.size);
        REQUIRE(result.parameters[0].locations.size() == test.pieces);
        REQUIRE(result.parameters[0].locations[0].register_name == "a0");
        REQUIRE(result.parameters[0].locations[0].value_offset == 0);
        REQUIRE(result.parameters[0].locations.back().size == test.last_piece_size);
        if (test.pieces == 2) {
            REQUIRE(result.parameters[0].locations[1].register_name == "a1");
            REQUIRE(result.parameters[0].locations[1].value_offset == 8);
        }
    }
}

void test_float_vectors_use_integer_aggregate_locations() {
    oir::TypeContext types;
    auto *v1f = types.fixed_vector_ty(types.float_ty(), 1);
    auto *v3f = types.fixed_vector_ty(types.float_ty(), 3);
    auto *signature = types.func_ty(v1f, {types.float_ty(), v1f, types.float_ty(), v3f});
    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*signature);

    REQUIRE(result.valid);
    REQUIRE(result.return_value.locations.size() == 1);
    REQUIRE(result.return_value.locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(result.return_value.locations[0].register_name == "a0");
    REQUIRE(result.parameters[0].locations[0].kind == target::CCLocationKind::FPR32);
    REQUIRE(result.parameters[0].locations[0].register_name == "fa0");
    REQUIRE(result.parameters[1].locations.size() == 1);
    REQUIRE(result.parameters[1].locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(result.parameters[1].locations[0].register_name == "a0");
    REQUIRE(result.parameters[2].locations[0].kind == target::CCLocationKind::FPR32);
    REQUIRE(result.parameters[2].locations[0].register_name == "fa1");
    REQUIRE(result.parameters[3].locations.size() == 2);
    REQUIRE(result.parameters[3].locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(result.parameters[3].locations[0].register_name == "a1");
    REQUIRE(result.parameters[3].locations[1].kind == target::CCLocationKind::GPR);
    REQUIRE(result.parameters[3].locations[1].register_name == "a2");
}

void test_fixed_vectors_are_aggregate_not_float_registers() {
    oir::TypeContext types;
    auto *v3f = types.fixed_vector_ty(types.float_ty(), 3);
    auto *v7i = types.fixed_vector_ty(types.int32_ty(), 7);
    auto *signature = types.func_ty(v3f, {v3f, v7i});
    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*signature);
    REQUIRE(result.valid);
    REQUIRE(result.return_value.locations.size() == 2);
    REQUIRE(result.return_value.locations[0].register_name == "a0");
    REQUIRE(result.return_value.locations[1].register_name == "a1");
    REQUIRE(!result.parameters[0].indirect);
    REQUIRE(result.parameters[0].locations.size() == 2);
    REQUIRE(result.parameters[0].locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(result.parameters[0].locations[0].register_name == "a0");
    REQUIRE(result.parameters[0].locations[1].register_name == "a1");
    REQUIRE(result.parameters[1].indirect);
    REQUIRE(result.parameters[1].locations[0].register_name == "a2");
}

void test_mask_layout_and_large_sret() {
    oir::TypeContext types;
    auto *m31 = types.fixed_vector_ty(types.int1_ty(), 31);
    auto *large = types.fixed_vector_ty(types.int32_ty(), 31);
    target::RISCVCallingConvention cc(standard_profile());

    auto *mask_signature = types.func_ty(m31, {m31});
    auto mask = cc.assign(*mask_signature);
    REQUIRE(mask.valid);
    REQUIRE(mask.return_value.size == 4);
    REQUIRE(mask.return_value.alignment == 1);
    REQUIRE(mask.return_value.locations.size() == 1);
    REQUIRE(mask.return_value.locations[0].value_offset == 0);
    REQUIRE(mask.return_value.locations[0].size == 4);
    REQUIRE(mask.parameters[0].locations[0].register_name == "a0");
    REQUIRE(mask.parameters[0].size == 4);
    REQUIRE(mask.parameters[0].alignment == 1);
    REQUIRE(mask.parameters[0].locations[0].size == 4);

    auto *large_signature = types.func_ty(large, {types.int32_ty(), large});
    auto large_result = cc.assign(*large_signature);
    REQUIRE(large_result.valid);
    REQUIRE(large_result.has_sret);
    REQUIRE(large_result.return_value.indirect);
    REQUIRE(large_result.return_value.locations[0].register_name == "a0");
    REQUIRE(large_result.parameters[0].locations[0].register_name == "a1");
    REQUIRE(large_result.parameters[1].indirect);
    REQUIRE(large_result.parameters[1].locations[0].register_name == "a2");
}

void test_indirect_parameter_after_exhausted_gprs() {
    oir::TypeContext types;
    auto *large = types.fixed_vector_ty(types.int32_ty(), 5);
    std::vector<oir::Type *> params(8, types.int32_ty());
    params.push_back(large);
    params.push_back(types.int32_ty());

    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*types.func_ty(types.void_ty(), params));
    REQUIRE(result.valid);
    REQUIRE(!result.has_sret);
    REQUIRE(result.parameters[7].locations[0].register_name == "a7");

    const auto &indirect = result.parameters[8];
    REQUIRE(indirect.indirect);
    REQUIRE(indirect.size == 20);
    REQUIRE(indirect.alignment == 4);
    REQUIRE(indirect.locations.size() == 1);
    REQUIRE(indirect.locations[0].kind == target::CCLocationKind::Stack);
    REQUIRE(indirect.locations[0].stack_offset == 0);
    REQUIRE(indirect.locations[0].value_offset == 0);
    REQUIRE(indirect.locations[0].size == 8);

    const auto &following = result.parameters[9];
    REQUIRE(!following.indirect);
    REQUIRE(following.locations.size() == 1);
    REQUIRE(following.locations[0].kind == target::CCLocationKind::Stack);
    REQUIRE(following.locations[0].stack_offset == 8);
    REQUIRE(following.locations[0].size == 4);
    REQUIRE(result.stack_argument_size == 16);
}

void test_sret_consumes_first_integer_argument_slot() {
    oir::TypeContext types;
    auto *large = types.fixed_vector_ty(types.int32_ty(), 5);
    std::vector<oir::Type *> params(8, types.int32_ty());
    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*types.func_ty(large, params));

    REQUIRE(result.valid);
    REQUIRE(result.has_sret);
    REQUIRE(result.return_value.indirect);
    REQUIRE(result.return_value.size == 20);
    REQUIRE(result.return_value.alignment == 4);
    REQUIRE(result.return_value.locations.size() == 1);
    REQUIRE(result.return_value.locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(result.return_value.locations[0].register_name == "a0");
    REQUIRE(result.return_value.locations[0].size == 8);

    REQUIRE(result.parameters[0].locations[0].register_name == "a1");
    REQUIRE(result.parameters[6].locations[0].register_name == "a7");
    REQUIRE(result.parameters[7].locations[0].kind == target::CCLocationKind::Stack);
    REQUIRE(result.parameters[7].locations[0].stack_offset == 0);
    REQUIRE(result.stack_argument_size == 16);

    auto call = cc.assign_call(*types.func_ty(large, params), params);
    REQUIRE(call.valid);
    REQUIRE(call.has_sret);
    REQUIRE(call.return_value.indirect);
    REQUIRE(call.return_value.locations[0].register_name == "a0");
    REQUIRE(call.parameters[0].locations[0].register_name == "a1");
    REQUIRE(call.parameters[6].locations[0].register_name == "a7");
    REQUIRE(call.parameters[7].locations[0].kind == target::CCLocationKind::Stack);
    REQUIRE(call.parameters[7].locations[0].stack_offset == 0);
    REQUIRE(call.stack_argument_size == 16);
}

void test_register_stack_split_and_variadic_float() {
    oir::TypeContext types;
    std::vector<oir::Type *> params(7, types.int32_ty());
    params.push_back(types.fixed_vector_ty(types.int32_ty(), 3));
    params.push_back(types.int32_ty());
    auto *signature = types.func_ty(types.void_ty(), params);
    target::RISCVCallingConvention cc(standard_profile());
    auto result = cc.assign(*signature);
    REQUIRE(result.valid);
    const auto &split = result.parameters[7];
    REQUIRE(split.locations.size() == 2);
    REQUIRE(split.locations[0].kind == target::CCLocationKind::GPR);
    REQUIRE(split.locations[0].register_name == "a7");
    REQUIRE(split.locations[0].value_offset == 0);
    REQUIRE(split.locations[0].size == 8);
    REQUIRE(split.locations[1].kind == target::CCLocationKind::Stack);
    REQUIRE(split.locations[1].stack_offset == 0);
    REQUIRE(split.locations[1].value_offset == 8);
    REQUIRE(split.locations[1].size == 4);
    REQUIRE(result.parameters[8].locations.size() == 1);
    REQUIRE(result.parameters[8].locations[0].kind == target::CCLocationKind::Stack);
    REQUIRE(result.parameters[8].locations[0].stack_offset == 8);
    REQUIRE(result.stack_argument_size == 16);

    auto call = cc.assign_call(*signature, params);
    REQUIRE(call.valid);
    REQUIRE(call.parameters[7].locations.size() == 2);
    REQUIRE(call.parameters[7].locations[0].register_name == "a7");
    REQUIRE(call.parameters[7].locations[1].kind == target::CCLocationKind::Stack);
    REQUIRE(call.parameters[7].locations[1].stack_offset == 0);
    REQUIRE(call.parameters[7].locations[1].value_offset == 8);
    REQUIRE(call.parameters[8].locations[0].stack_offset == 8);
    REQUIRE(call.stack_argument_size == 16);

    auto *variadic_signature = types.func_ty(types.void_ty(), {types.float_ty()}, true);
    auto variadic_entry = cc.assign(*variadic_signature);
    REQUIRE(variadic_entry.valid);
    REQUIRE(variadic_entry.parameters.size() == 1);
    REQUIRE(variadic_entry.parameters[0].locations[0].register_name == "fa0");

    auto variadic = cc.assign_call(*variadic_signature,
                                   {types.float_ty(), types.float_ty()});
    REQUIRE(variadic.valid);
    REQUIRE(variadic.parameters[0].locations[0].register_name == "fa0");
    REQUIRE(variadic.parameters[1].locations[0].register_name == "a0");

    auto vector_tail = cc.assign_call(
        *variadic_signature,
        {types.float_ty(), types.fixed_vector_ty(types.float_ty(), 3)});
    REQUIRE(!vector_tail.valid);
    REQUIRE(vector_tail.error.find("variadic") != std::string::npos);

    auto too_few = cc.assign_call(*variadic_signature, {});
    REQUIRE(!too_few.valid);
    REQUIRE(too_few.error.find("count") != std::string::npos);

    auto wrong_fixed_type =
        cc.assign_call(*variadic_signature, {types.int32_ty(), types.float_ty()});
    REQUIRE(!wrong_fixed_type.valid);
    REQUIRE(wrong_fixed_type.error.find("fixed parameter type") != std::string::npos);

    auto non_variadic_extra =
        cc.assign_call(*signature, {types.int32_ty(), types.int32_ty()});
    REQUIRE(!non_variadic_extra.valid);
    REQUIRE(non_variadic_extra.error.find("count") != std::string::npos);
}

void test_scalable_and_wrong_classifier_vector_abi_rejected() {
    oir::TypeContext types;
    auto *scalable = types.scalable_vector_ty(types.int32_ty(), 4);
    target::RISCVCallingConvention standard(standard_profile());
    auto standard_result = standard.assign(*types.func_ty(types.void_ty(), {scalable}));
    REQUIRE(!standard_result.valid);
    REQUIRE(standard_result.error.find("scalable") != std::string::npos);

    auto profile = standard_profile();
    profile.vector_abi = target::VectorABI::PsABIVector;
    target::RISCVCallingConvention vector_cc(profile);
    auto vector_result = vector_cc.assign(*types.func_ty(types.void_ty(), {types.int32_ty()}));
    REQUIRE(!vector_result.valid);
    REQUIRE(vector_result.error.find("only the standard aggregate ABI") != std::string::npos);
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"scalar_lp64d_classification", test_scalar_lp64d_classification},
        {"direct_aggregate_size_boundaries", test_direct_aggregate_size_boundaries},
        {"float_vectors_use_integer_aggregate_locations", test_float_vectors_use_integer_aggregate_locations},
        {"fixed_vectors_are_aggregate_not_float_registers", test_fixed_vectors_are_aggregate_not_float_registers},
        {"mask_layout_and_large_sret", test_mask_layout_and_large_sret},
        {"indirect_parameter_after_exhausted_gprs", test_indirect_parameter_after_exhausted_gprs},
        {"sret_consumes_first_integer_argument_slot", test_sret_consumes_first_integer_argument_slot},
        {"register_stack_split_and_variadic_float", test_register_stack_split_and_variadic_float},
        {"scalable_and_wrong_classifier_vector_abi_rejected", test_scalable_and_wrong_classifier_vector_abi_rejected},
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
    with tempfile.TemporaryDirectory(prefix="riscv-cc-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "calling_convention_tests.cpp"
        binary = tmp_dir / "calling_convention_tests"
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
