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
#include "mir/AsmPrinter.h"
#include "mir/MIRPrinter.h"
#include "oir/OIR.h"
#include "oir/OIRDataLayout.h"
#include "pass/oir/OIRToMIRCommon.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

void require_bytes(const oir::GlobalVariable &global,
                   const std::vector<std::uint8_t> &expected) {
    REQUIRE(pass::oir_to_mir::lower_global_initializer(global) == expected);
}

void test_scalar_array_and_vector_bytes() {
    oir::Module module("typed-bytes");
    auto &types = module.types();

    auto *negative = module.create_global("negative", types.int32_ty(), true,
                                          module.create_i32(-2));
    require_bytes(*negative, {0xfe, 0xff, 0xff, 0xff});

    auto *negative_zero = module.create_global("negative_zero", types.float_ty(), true,
                                               module.create_f32(-0.0f));
    require_bytes(*negative_zero, {0x00, 0x00, 0x00, 0x80});

    auto *array_type = types.array_ty(types.int32_ty(), 2);
    auto *array = module.create_constant_array(
        array_type, {module.create_i32(0x12345678), module.create_i32(-1)});
    auto *array_global = module.create_global("array", array_type, true, array);
    require_bytes(*array_global,
                  {0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff});

    auto *v3i = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *vector3 = module.create_constant_vector(
        v3i, {module.create_i32(1), module.create_i32(0x12345678), module.create_i32(-1)});
    auto *vector3_global = module.create_global("vector3", v3i, true, vector3);
    require_bytes(*vector3_global,
                  {0x01, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
                   0xff, 0xff, 0xff, 0xff});

    auto *v7f = types.fixed_vector_ty(types.float_ty(), 7);
    std::vector<oir::Constant *> float_lanes;
    for (int lane = 0; lane < 6; ++lane) {
        float_lanes.push_back(module.create_f32(0.0f));
    }
    float_lanes.push_back(module.create_f32(1.5f));
    auto *vector7 = module.create_constant_vector(v7f, float_lanes);
    auto bytes7 = pass::oir_to_mir::lower_global_initializer(
        *module.create_global("vector7", v7f, true, vector7));
    REQUIRE(bytes7.size() == 28);
    REQUIRE(bytes7[24] == 0x00 && bytes7[25] == 0x00 && bytes7[26] == 0xc0 &&
            bytes7[27] == 0x3f);
}

void test_packed_mask_zero_and_tentative_definition() {
    oir::Module module("mask-bytes");
    auto &types = module.types();
    auto *m10 = types.fixed_vector_ty(types.int1_ty(), 10);
    auto *mask = module.create_constant_mask(m10, {0x55, 0x03});
    auto *mask_global = module.create_global("mask", m10, true, mask);
    require_bytes(*mask_global, {0x55, 0x03});

    auto *array_type = types.array_ty(types.int32_ty(), 4);
    auto *zero_global = module.create_global(
        "zero", array_type, false, module.create_zero(array_type));
    require_bytes(*zero_global, {});

    auto *tentative = module.create_global("tentative", types.int32_ty(), false);
    require_bytes(*tentative, {});
}

mir::TypeInfo byte_array_type(std::uint64_t size) {
    mir::TypeInfo type;
    type.value_type = mir::ValueType::Aggregate;
    type.ir = "typed-bytes";
    type.size = size;
    type.align = 1;
    return type;
}

void test_mir_and_assembly_consume_only_bytes() {
    mir::Module module("byte-emission");
    mir::Global data;
    data.name = "packed";
    data.type = byte_array_type(6);
    data.is_const = true;
    data.initializer_bytes = {1, 0, 127, 0, 3, 0};
    module.add_global(std::move(data));

    mir::Global zero;
    zero.name = "zeros";
    zero.type = byte_array_type(7);
    module.add_global(std::move(zero));

    std::ostringstream mir_output;
    mir::MIRPrinter mir_printer(mir_output);
    mir_printer.print(module);
    const auto mir_text = mir_output.str();
    REQUIRE(mir_text.find("bytes[01 00 7f 00 03 00]") != std::string::npos);
    REQUIRE(mir_text.find("global @zeros") != std::string::npos);
    REQUIRE(mir_text.find("= zeroinit") != std::string::npos);

    std::ostringstream asm_output;
    mir::AsmPrinter asm_printer(asm_output);
    asm_printer.print(module);
    const auto assembly = asm_output.str();
    REQUIRE(assembly.find("packed:\n\t.byte 1\n\t.zero 1\n\t.byte 127\n\t.zero 1\n"
                          "\t.byte 3\n\t.zero 1") != std::string::npos);
    REQUIRE(assembly.find("zeros:\n\t.zero 7") != std::string::npos);

    mir::Module malformed("malformed");
    mir::Global wrong;
    wrong.name = "wrong";
    wrong.type = byte_array_type(4);
    wrong.initializer_bytes = {1, 2, 3};
    malformed.add_global(std::move(wrong));
    bool rejected = false;
    try {
        std::ostringstream sink;
        mir::AsmPrinter printer(sink);
        printer.print(malformed);
    } catch (const std::runtime_error &error) {
        rejected = std::string(error.what()).find("byte count") != std::string::npos;
    }
    REQUIRE(rejected);
}

void test_layout_stride_and_checked_gep_offset() {
    oir::TypeContext types;
    oir::DataLayout layout;
    auto *triple = types.array_ty(types.int32_ty(), 3);
    auto *pointer = types.ptr_ty(triple);
    REQUIRE(layout.preferred_alignment(types.int1_ty()) == 1);
    REQUIRE(layout.preferred_alignment(types.fixed_vector_ty(types.int32_ty(), 3)) == 4);
    REQUIRE(layout.fixed_array_stride(triple) == 4);

    const auto strides = layout.fixed_gep_index_strides(pointer, 2);
    REQUIRE(strides.has_value());
    REQUIRE(*strides == std::vector<std::uint64_t>({12, 4}));
    REQUIRE(layout.fixed_gep_offset(pointer, {2, 1}) == 28);
    REQUIRE(layout.fixed_gep_offset(pointer, {-1, 2}) == -4);
    REQUIRE(!layout.fixed_gep_offset(
                 pointer, {std::numeric_limits<std::int64_t>::max(), 0})
                 .has_value());

    auto *scalable = types.scalable_vector_ty(types.int32_ty(), 4);
    auto *scalable_pointer = types.ptr_ty(scalable);
    REQUIRE(!layout.fixed_gep_index_strides(scalable_pointer, 1).has_value());
    REQUIRE(!layout.fixed_gep_offset(scalable_pointer, {1}).has_value());
}

int main() {
    try {
        test_scalar_array_and_vector_bytes();
        std::cout << "PASS typed_global_bytes\n";
        test_packed_mask_zero_and_tentative_definition();
        std::cout << "PASS typed_global_mask_and_tentative\n";
        test_mir_and_assembly_consume_only_bytes();
        std::cout << "PASS typed_global_assembly\n";
        test_layout_stride_and_checked_gep_offset();
        std::cout << "PASS data_layout_gep\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL typed_global: " << error.what() << '\n';
        return 1;
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
    with tempfile.TemporaryDirectory(prefix="typed-global-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "typed_global_infra_tests.cpp"
        binary = tmp_dir / "typed_global_infra_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = [
            "src/oir/OIR.cpp",
            "src/oir/OIRAnalysis.cpp",
            "src/oir/OIRDataLayout.cpp",
            "src/builtin/BuiltinRegistry.cpp",
            "src/mir/MIR.cpp",
            "src/mir/MachineInstrDesc.cpp",
            "src/mir/MIRPrinter.cpp",
            "src/mir/AsmPrinter.cpp",
            "src/pass/oir/OIRToMIRCommon.cpp",
        ]
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
                *(str(ROOT / item) for item in sources),
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
