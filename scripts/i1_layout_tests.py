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
#include "oir/OIR.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/oir/OIRToMIRCommon.h"
#include "target/TargetMachine.h"

#include <algorithm>
#include <iostream>
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

mir::TypeInfo type_info(mir::ValueType value_type, const char *ir, std::uint64_t size,
                        std::uint64_t align) {
    mir::TypeInfo type;
    type.value_type = value_type;
    type.ir = ir;
    type.size = size;
    type.align = align;
    return type;
}

void test_oir_i1_object_layout_reaches_mir() {
    oir::Module module("i1-objects");
    auto &types = module.types();
    auto *function = module.create_function(
        "objects", types.func_ty(types.void_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);

    auto *first = builder.create_alloca(types.int1_ty(), "first");
    auto *second = builder.create_alloca(types.int1_ty(), "second");
    (void)builder.create_alloca(types.array_ty(types.int1_ty(), 2), "bits");
    builder.create_store(builder.i1(true), first);
    builder.create_store(builder.i1(false), second);
    (void)builder.create_load(first, types.int1_ty(), "loaded");
    builder.create_ret();

    std::string error;
    REQUIRE(module.verify(&error));
    target::TargetProfile profile;
    REQUIRE(target::finalize_target_profile(profile, error));
    auto lowered = pass::oir_to_mir::lower_with_vregs(module, profile);
    REQUIRE(lowered != nullptr);
    REQUIRE(lowered->functions().size() == 1);

    lowered->functions().front()->layout_frame();

    const auto &slots = lowered->functions().front()->stack_slots();
    std::vector<const mir::StackSlot *> scalar_bits;
    const mir::StackSlot *bit_array = nullptr;
    for (const auto &slot : slots) {
        if (slot.kind != mir::StackSlotKind::Alloca) {
            continue;
        }
        if (slot.type.ir == "i1") {
            scalar_bits.push_back(&slot);
        } else if (slot.type.ir == "[2 x i1]") {
            bit_array = &slot;
        }
    }
    REQUIRE(scalar_bits.size() == 2);
    std::sort(scalar_bits.begin(), scalar_bits.end(),
              [](const auto *lhs, const auto *rhs) { return lhs->offset < rhs->offset; });
    REQUIRE(scalar_bits[0]->type.size == 1 && scalar_bits[0]->type.align == 1);
    REQUIRE(scalar_bits[1]->type.size == 1 && scalar_bits[1]->type.align == 1);
    REQUIRE(scalar_bits[1]->offset == scalar_bits[0]->offset + 1);
    REQUIRE(bit_array != nullptr);
    REQUIRE(bit_array->type.size == 2 && bit_array->type.align == 1);
}

void test_asm_i1_memory_width_and_spill_separation() {
    mir::Module module("i1-assembly");
    auto *function = module.create_function(
        "i1_widths", type_info(mir::ValueType::Void, "void", 0, 1), {});
    auto *entry = function->create_block("entry");

    const auto first = function->add_stack_slot(
        "first", type_info(mir::ValueType::I1, "i1", 1, 1), mir::StackSlotKind::Value);
    const auto second = function->add_stack_slot(
        "second", type_info(mir::ValueType::I1, "i1", 1, 1), mir::StackSlotKind::Value);
    const auto spill = function->add_stack_slot(
        "spill", type_info(mir::ValueType::I1, "i1.spill", 4, 4),
        mir::StackSlotKind::Spill);

    auto bit0 = mir::Register::physical("t0", mir::RegisterClass::GPR, mir::ValueType::I1);
    auto bit1 = mir::Register::physical("t1", mir::RegisterClass::GPR, mir::ValueType::I1);
    auto address = mir::Register::physical("a0", mir::RegisterClass::GPR, mir::ValueType::Ptr);
    entry->add_instr(
        mir::Opcode::LoadSlot,
        {mir::MachineOperand::reg_def(bit0), mir::MachineOperand::slot(first),
         mir::MachineOperand::type(mir::ValueType::I1)});
    entry->add_instr(
        mir::Opcode::StoreSlot,
        {mir::MachineOperand::slot(second), mir::MachineOperand::reg_use(bit0),
         mir::MachineOperand::type(mir::ValueType::I1)});
    entry->add_instr(
        mir::Opcode::LoadSlot,
        {mir::MachineOperand::reg_def(bit1), mir::MachineOperand::slot(spill),
         mir::MachineOperand::type(mir::ValueType::I1)});
    entry->add_instr(
        mir::Opcode::StoreSlot,
        {mir::MachineOperand::slot(spill), mir::MachineOperand::reg_use(bit1),
         mir::MachineOperand::type(mir::ValueType::I1)});
    entry->add_instr(
        mir::Opcode::LoadMem,
        {mir::MachineOperand::reg_def(bit0), mir::MachineOperand::reg_use(address),
         mir::MachineOperand::type(mir::ValueType::I1)});
    entry->add_instr(
        mir::Opcode::StoreMemOffset,
        {mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(bit0),
         mir::MachineOperand::imm(1), mir::MachineOperand::type(mir::ValueType::I1)});
    function->layout_frame();

    REQUIRE(function->stack_slot(second)->offset == function->stack_slot(first)->offset + 1);
    REQUIRE(function->stack_slot(spill)->offset % 4 == 0);

    std::string error;
    REQUIRE(pass::expand_machine_pseudos(module, error));
    REQUIRE(error.empty());

    std::ostringstream output;
    mir::AsmPrinter printer(output);
    printer.print(module);
    const auto assembly = output.str();
    REQUIRE(assembly.find("\tlbu t0, 0(sp)") != std::string::npos);
    REQUIRE(assembly.find("\tsb t0, 1(sp)") != std::string::npos);
    REQUIRE(assembly.find("\tlw t1, 4(sp)") != std::string::npos);
    REQUIRE(assembly.find("\tsw t1, 4(sp)") != std::string::npos);
    REQUIRE(assembly.find("\tlbu t0, 0(a0)") != std::string::npos);
    REQUIRE(assembly.find("\tsb t0, 1(a0)") != std::string::npos);
}

int main() {
    try {
        test_oir_i1_object_layout_reaches_mir();
        std::cout << "PASS i1_object_layout\n";
        test_asm_i1_memory_width_and_spill_separation();
        std::cout << "PASS i1_asm_widths\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL i1_layout: " << error.what() << '\n';
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
    with tempfile.TemporaryDirectory(prefix="i1-layout-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "i1_layout_tests.cpp"
        binary = tmp_dir / "i1_layout_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = [
            "src/oir/OIR.cpp",
            "src/oir/OIRAnalysis.cpp",
            "src/oir/OIRDataLayout.cpp",
            "src/builtin/BuiltinRegistry.cpp",
            "src/target/TargetMachine.cpp",
            "src/target/RVVFixedVectorLegalization.cpp",
            "src/target/RISCVCallingConvention.cpp",
            "src/mir/MIR.cpp",
            "src/mir/MachineInstrDesc.cpp",
            "src/mir/MIRVerifier.cpp",
            "src/mir/MIRVectorState.cpp",
            "src/mir/AsmPrinter.cpp",
            "src/pass/PassManager.cpp",
            "src/pass/mir/MIRPseudoExpansionPass.cpp",
            "src/pass/oir/OIRToMIRCommon.cpp",
            "src/pass/oir/OIRToMIRVRegLowerer.cpp",
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
