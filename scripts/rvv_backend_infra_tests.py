#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
LOOP_SOURCE = ROOT / "test/ir/oir_loop_vectorize.sy"
VLENS = (128, 256, 512, 1024)


PROBE_SOURCE = r"""
#include "mir/AsmPrinter.h"
#include "mir/MIRVerifier.h"
#include "mir/MachineInstrDesc.h"
#include "oir/OIR.h"
#include "pass/PassManager.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/oir/OIRToMIRCommon.h"
#include "target/TargetMachine.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw std::runtime_error(#condition);                                           \
        }                                                                                   \
    } while (false)

std::unique_ptr<oir::Module> make_integer_module(std::uint8_t mask_bits) {
    auto module = std::make_unique<oir::Module>("constant-vector-isel");
    auto &types = module->types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *pointer = types.ptr_ty(types.int32_ty());
    auto *function = module->create_function(
        "store_literal", types.func_ty(types.void_ty(), {pointer}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    auto *literal = module->create_constant_vector(
        vector, {module->create_i32(1), module->create_i32(-2),
                 module->create_i32(3), module->create_i32(0x12345678)});
    auto *active = module->create_constant_mask(mask, {mask_bits});
    builder.create_vp_store(literal, function->args()[0].get(), active,
                            builder.i32(4), oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

std::unique_ptr<oir::Module> make_fixed_boundary_module(std::uint64_t lanes) {
    auto module = std::make_unique<oir::Module>("fixed-boundary");
    auto &types = module->types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *pointer = types.ptr_ty(types.int32_ty());
    auto *function = module->create_function(
        "store_boundary", types.func_ty(types.void_ty(), {pointer}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    std::vector<oir::Constant *> elements;
    elements.reserve(lanes);
    for (std::uint64_t lane = 0; lane < lanes; ++lane) {
        elements.push_back(module->create_i32(static_cast<std::int64_t>(lane)));
    }
    std::vector<std::uint8_t> packed((lanes + 7U) / 8U, 0xffU);
    if (lanes % 8U != 0U) {
        packed.back() = static_cast<std::uint8_t>((1U << (lanes % 8U)) - 1U);
    }
    auto *literal = module->create_constant_vector(vector, elements);
    auto *active = module->create_constant_mask(mask, packed);
    builder.create_vp_store(literal, function->args()[0].get(), active,
                            builder.i32(static_cast<std::int64_t>(lanes)),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
    std::string error;
    if (!module->verify(&error)) {
        throw std::runtime_error(error);
    }
    return module;
}

std::unique_ptr<oir::Module> make_float_module() {
    auto module = std::make_unique<oir::Module>("float-vector-isel");
    auto &types = module->types();
    auto *vector = types.fixed_vector_ty(types.float_ty(), 4);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *pointer = types.ptr_ty(types.float_ty());
    auto *function = module->create_function(
        "store_float_sum", types.func_ty(types.void_ty(), {pointer}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    auto *lhs = module->create_constant_vector(
        vector, {module->create_f32(1.0F), module->create_f32(2.0F),
                 module->create_f32(3.0F), module->create_f32(4.0F)});
    auto *rhs = module->create_constant_vector(
        vector, {module->create_f32(0.5F), module->create_f32(1.5F),
                 module->create_f32(2.5F), module->create_f32(3.5F)});
    auto *active = module->create_constant_mask(mask, {0x0f});
    auto *sum = builder.create_vp_binary(
        oir::Instruction::OpID::FAdd, lhs, rhs, active, builder.i32(4),
        module->create_undef(vector), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, "sum");
    builder.create_vp_store(sum, function->args()[0].get(), active, builder.i32(4),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

std::unique_ptr<oir::Module> make_policy_module() {
    auto module = std::make_unique<oir::Module>("vp-policy-runtime");
    auto &types = module->types();
    auto *i32 = types.int32_ty();

    auto build = [&](const char *name, oir::VectorType *vector,
                     oir::VectorType *mask, bool scalable) {
        auto *pointer = types.ptr_ty(vector);
        auto *function = module->create_function(
            name, types.func_ty(scalable ? static_cast<oir::Type *>(i32)
                                        : static_cast<oir::Type *>(types.void_ty()),
                                {pointer, pointer, pointer, pointer, pointer, i32,
                                 i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        if (scalable) {
            (void)builder.create_set_vl(vector, function->args()[5].get(),
                                        "entry.vlmax");
        }
        auto *out = function->args()[0].get();
        auto *pass_out = function->args()[1].get();
        auto *lhs = builder.create_load(function->args()[2].get(), vector, "lhs");
        auto *rhs = builder.create_load(function->args()[3].get(), vector, "rhs");
        auto *passthrough =
            builder.create_load(function->args()[4].get(), vector, "passthrough");
        oir::Value *active = nullptr;
        if (scalable) {
            active = builder.create_splat(mask, builder.i1(true), "all.active");
        } else {
            active = module->create_constant_mask(mask, {0x55});
        }
        auto *evl = function->args()[6].get();
        auto *agnostic = builder.create_vp_binary(
            oir::Instruction::OpID::Add, lhs, rhs, active, evl,
            module->create_undef(vector), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, "agnostic");
        auto *preserved = builder.create_vp_binary(
            oir::Instruction::OpID::Add, agnostic, rhs, active, evl,
            passthrough, oir::TailPolicy::Undisturbed,
            oir::MaskPolicy::Undisturbed, "preserved");
        builder.create_store(preserved, out);
        builder.create_store(passthrough, pass_out);
        if (scalable) {
            auto *vlmax =
                builder.create_set_vl(vector, function->args()[5].get(),
                                      "return.vlmax");
            builder.create_ret(vlmax);
        } else {
            builder.create_ret();
        }
    };

    auto *fixed = types.fixed_vector_ty(i32, 7);
    auto *fixed_mask = types.fixed_vector_ty(types.int1_ty(), 7);
    build("fixed_policy", fixed, fixed_mask, false);
    auto *scalable = types.scalable_vector_ty(i32, 4);
    auto *scalable_mask = types.scalable_vector_ty(types.int1_ty(), 4);
    build("scalable_policy", scalable, scalable_mask, true);
    std::string error;
    if (!module->verify(&error)) {
        throw std::runtime_error(error);
    }
    return module;
}

mir::TypeInfo direct_i32_type() {
    return {mir::ValueType::I32, "i32", 4, 4, std::nullopt};
}

mir::Register direct_state(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::VSTATE);
}

mir::MachineVectorInfo direct_info(const mir::MachineVectorType &type,
                                   mir::RVVOperation operation,
                                   mir::MachineVectorAVL avl) {
    mir::MachineVectorInfo info(type);
    info.operation = operation;
    info.avl = avl;
    info.tail_policy = mir::VectorTailPolicy::Agnostic;
    info.mask_policy = mir::VectorMaskPolicy::Agnostic;
    return info;
}

mir::MachineInstr direct_set_vlmax(const mir::MachineVectorType &type) {
    return mir::MachineInstr(
        mir::Opcode::RVVSetVLI,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "t0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::reg_use(mir::Register::physical(
             "zero", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::implicit_reg_def(direct_state("vl")),
         mir::MachineOperand::implicit_reg_def(direct_state("vtype"))},
        direct_info(type, mir::RVVOperation::SetVL,
                    mir::MachineVectorAVL::operand(1)));
}

std::unique_ptr<mir::Module> make_whole_spill_runtime_module() {
    auto module = std::make_unique<mir::Module>("whole-spill-runtime");
    const auto i32 = direct_i32_type();
    const auto fractional = mir::MachineVectorType::scalable(
        mir::ValueType::I32, 32, mir::VectorLMUL::MF2);

    auto *fractional_function = module->create_function(
        "fractional_spill_probe", i32, {i32});
    auto *fractional_block = fractional_function->create_block("entry");
    auto fractional_value = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, fractional);
    fractional_block->add_instr(direct_set_vlmax(fractional));
    fractional_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVSplatVXTA,
        {mir::MachineOperand::reg_def(fractional_value),
         mir::MachineOperand::reg_use(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        direct_info(fractional, mir::RVVOperation::Splat,
                    mir::MachineVectorAVL::current_vl())));
    fractional_block->add_instr(
        mir::Opcode::Call, {mir::MachineOperand::symbol("vector_clobber")});
    fractional_block->add_instr(direct_set_vlmax(fractional));
    fractional_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVExtractElement,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::reg_use(fractional_value),
         mir::MachineOperand::imm(0),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        direct_info(fractional, mir::RVVOperation::Extract,
                    mir::MachineVectorAVL::current_vl())));
    fractional_block->add_instr(
        mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
    fractional_function->note_call();
    fractional_function->layout_frame();

    const auto data = mir::MachineVectorType::scalable(
        mir::ValueType::I32, 32, mir::VectorLMUL::M1);
    const auto mask = mir::MachineVectorType::mask_for(data);
    auto *mask_function = module->create_function(
        "mask_spill_probe", i32, {});
    auto *mask_block = mask_function->create_block("entry");
    auto live_mask = mask_function->regs().create_virtual_vector(
        mir::RegisterClass::VMASK, mask);
    mask_block->add_instr(direct_set_vlmax(data));
    mask_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMaskSet,
        {mir::MachineOperand::reg_def(live_mask),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        direct_info(data, mir::RVVOperation::MaskSet,
                    mir::MachineVectorAVL::current_vl())));
    mask_block->add_instr(
        mir::Opcode::Call, {mir::MachineOperand::symbol("vector_clobber")});
    mask_block->add_instr(direct_set_vlmax(data));
    const auto physical_v0 = mir::Register::physical_vector(
        "v0", mir::RegisterClass::VMASK, mask);
    mask_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMaskCopy,
        {mir::MachineOperand::reg_def(physical_v0),
         mir::MachineOperand::reg_use(live_mask),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        direct_info(data, mir::RVVOperation::Copy,
                    mir::MachineVectorAVL::current_vl())));
    auto zero = mask_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, data);
    auto one = mask_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, data);
    auto selected = mask_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, data);
    const auto add_splat = [&](mir::Register destination, std::int64_t value) {
        mask_block->add_instr(mir::MachineInstr(
            mir::Opcode::RVVSplatVITA,
            {mir::MachineOperand::reg_def(destination),
             mir::MachineOperand::imm(value),
             mir::MachineOperand::implicit_reg_use(direct_state("vl")),
             mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
            direct_info(data, mir::RVVOperation::Splat,
                        mir::MachineVectorAVL::current_vl())));
    };
    add_splat(zero, 0);
    add_splat(one, 1);
    auto merge_info = direct_info(data, mir::RVVOperation::Merge,
                                  mir::MachineVectorAVL::current_vl());
    merge_info.mask_operand = 3;
    mask_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMergeVVM,
        {mir::MachineOperand::reg_def(selected),
         mir::MachineOperand::reg_use(zero),
         mir::MachineOperand::reg_use(one),
         mir::MachineOperand::reg_use(physical_v0),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        std::move(merge_info)));
    mask_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVExtractElement,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::reg_use(selected), mir::MachineOperand::imm(0),
         mir::MachineOperand::implicit_reg_use(direct_state("vl")),
         mir::MachineOperand::implicit_reg_use(direct_state("vtype"))},
        direct_info(data, mir::RVVOperation::Extract,
                    mir::MachineVectorAVL::current_vl())));
    mask_block->add_instr(
        mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
    mask_function->note_call();
    mask_function->layout_frame();
    return module;
}

target::TargetProfile profile(const char *march) {
    target::TargetProfile result;
    result.march = march;
    std::string error;
    REQUIRE(target::finalize_target_profile(result, error));
    return result;
}

std::string allocate_expand(std::unique_ptr<mir::Module> machine,
                            const target::TargetProfile &target) {
    REQUIRE(machine != nullptr);
    machine->target().arch = target.march;
    machine->target().has_vector = target.has_vector();
    machine->target().minimum_vlen_bits = target.minimum_vlen_bits;
    const auto pre = mir::verify_module(*machine, mir::MIRVerificationStage::PreRA);
    if (!pre.ok) {
        throw std::runtime_error(pre.message);
    }
    pass::PassContext context;
    context.set_machine_module(std::move(machine));
    pass::MIRRegAllocPass regalloc;
    const auto allocated = regalloc.run(context);
    if (!allocated.success) {
        throw std::runtime_error(allocated.message);
    }
    machine = context.take_machine_module();
    std::string error;
    REQUIRE(pass::expand_machine_pseudos(*machine, error));
    const auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
    if (!final.ok) {
        throw std::runtime_error(final.message);
    }
    for (const auto &function : machine->functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                REQUIRE(!mir::instruction_desc(instruction.opcode()).has_flag(
                    mir::MIF_Pseudo));
            }
        }
    }
    std::ostringstream assembly;
    mir::AsmPrinter(assembly).print(*machine);
    return assembly.str();
}

void allocate_expand_and_check(std::unique_ptr<mir::Module> machine,
                               const target::TargetProfile &target,
                               const std::string &assembly_needle) {
    REQUIRE(allocate_expand(std::move(machine), target).find(assembly_needle) !=
            std::string::npos);
}

int main(int argc, char **argv) {
    const auto rvv = profile("rv64gcv");
    auto integer_oir = make_integer_module(0x0f);
    auto integer_machine = pass::oir_to_mir::lower_with_vregs(*integer_oir, rvv);
    REQUIRE(integer_machine->globals().size() == 1);
    const auto &bytes = integer_machine->globals()[0].initializer_bytes;
    REQUIRE(bytes.size() == 16 && bytes[0] == 1 && bytes[4] == 0xfe &&
            bytes[5] == 0xff && bytes[6] == 0xff && bytes[7] == 0xff &&
            bytes[12] == 0x78 && bytes[13] == 0x56 && bytes[14] == 0x34 &&
            bytes[15] == 0x12);
    allocate_expand_and_check(std::move(integer_machine), rvv, "vse32.v");
    std::cout << "PASS constant_vector_and_all_true_mask_to_final\n";

    auto float_oir = make_float_module();
    allocate_expand_and_check(
        pass::oir_to_mir::lower_with_vregs(*float_oir, rvv), rvv, "vfadd.vv");
    std::cout << "PASS fp_vp_binary_dynamic_frm_to_final\n";

    auto mixed = make_integer_module(0x05);
    auto mixed_machine = pass::oir_to_mir::lower_with_vregs(*mixed, rvv);
    REQUIRE(mixed_machine->globals().size() == 2);
    REQUIRE(mixed_machine->globals()[1].initializer_bytes.size() == 1);
    REQUIRE(mixed_machine->globals()[1].initializer_bytes[0] == 0x05);
    allocate_expand_and_check(std::move(mixed_machine), rvv, "vlm.v");
    std::cout << "PASS mixed_mask_constant_pool_to_final\n";

    const auto zve32 = profile("rv64gc_zve32x");
    allocate_expand_and_check(
        pass::oir_to_mir::lower_with_vregs(*make_fixed_boundary_module(8), zve32),
        zve32, "vse32.v");
    allocate_expand_and_check(
        pass::oir_to_mir::lower_with_vregs(*make_fixed_boundary_module(9), zve32),
        zve32, "vse32.v");
    const auto zve64 = profile("rv64gc_zve64x");
    allocate_expand_and_check(
        pass::oir_to_mir::lower_with_vregs(*make_fixed_boundary_module(16), zve64),
        zve64, "vse32.v");
    allocate_expand_and_check(
        pass::oir_to_mir::lower_with_vregs(*make_fixed_boundary_module(17), zve64),
        zve64, "vse32.v");
    std::cout << "PASS zve32_zve64_fixed_vlen_boundaries_and_chunks\n";

    if (argc == 3) {
        auto policy = make_policy_module();
        auto policy_assembly = allocate_expand(
            pass::oir_to_mir::lower_with_vregs(*policy, rvv), rvv);
        REQUIRE(policy_assembly.find("ta, ma") != std::string::npos);
        REQUIRE(policy_assembly.find("tu, mu") != std::string::npos);
        REQUIRE(policy_assembly.find("vmv.v.v") != std::string::npos);
        auto spill_assembly = allocate_expand(
            make_whole_spill_runtime_module(), rvv);
        REQUIRE(spill_assembly.find("vs1r.v") != std::string::npos);
        REQUIRE(spill_assembly.find("vl1re32.v") != std::string::npos);
        std::ofstream output(argv[1]);
        REQUIRE(output.good());
        output << policy_assembly;
        output.close();
        REQUIRE(output.good());
        std::ofstream spill_output(argv[2]);
        REQUIRE(spill_output.good());
        spill_output << spill_assembly;
        spill_output.close();
        REQUIRE(spill_output.good());
        std::cout << "PASS fixed_and_scalable_vp_policy_to_final\n";
        std::cout << "PASS fractional_and_mask_whole_spill_to_final\n";
    } else {
        REQUIRE(argc == 1);
    }

    try {
        auto no_vector = make_integer_module(0x0f);
        (void)pass::oir_to_mir::lower_with_vregs(*no_vector, profile("rv64gc"));
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string(error.what()).find("requires a vector-enabled target") !=
                std::string::npos);
        std::cout << "PASS no_v_target_fails_closed\n";
        return 0;
    }
    throw std::runtime_error("vector OIR unexpectedly lowered for rv64gc");
}
"""


PROBE_SOURCES = (
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
    "src/pass/oir/OIRToMIRCommon.cpp",
    "src/pass/oir/OIRToMIRVRegLowerer.cpp",
    "src/pass/mir/MIRVectorRegAlloc.cpp",
    "src/pass/mir/MIRRegAllocPass.cpp",
    "src/pass/mir/MIRVectorStatePass.cpp",
    "src/pass/mir/MIRPseudoExpansionPass.cpp",
)


SCALAR_PSEUDOS = (
    "COMMENT",
    "LI",
    "FLI.S",
    "LA",
    "FI_ADDR",
    "LOAD_SLOT",
    "STORE_SLOT",
    "LOAD_MEM",
    "STORE_MEM",
    "LOAD_MEM_OFF",
    "STORE_MEM_OFF",
    "MEMZERO",
    "MV",
    "FMV.S",
    "SEQZ",
    "SNEZ",
    "STORE_OUT_ARG",
    "LOAD_IN_ARG",
    "BNEZ",
    "BEQZ",
    "J",
    "CALL",
)


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"required {description} not found: {path}")
    return path


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required RVV backend-test tool not found: {name}")
    return path


def find_cxx() -> str:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path is not None:
            return path
    raise RuntimeError("no C++ compiler found in PATH")


def run_checked(
    command: list[str],
    *,
    input_bytes: bytes | None = None,
    timeout: float = 60.0,
) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout.decode(errors='replace')}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result


def test_programmatic_isel(
    directory: Path, cxx: str, assembler: str, gcc: str, qemu: str
) -> None:
    source = directory / "rvv_backend_probe.cpp"
    binary = directory / "rvv_backend_probe"
    policy_assembly = directory / "vp_policy.s"
    policy_object = directory / "vp_policy.o"
    spill_assembly = directory / "whole_spill.s"
    spill_object = directory / "whole_spill.o"
    policy_driver = directory / "vp_policy_driver.c"
    policy_binary = directory / "vp_policy"
    source.write_text(textwrap.dedent(PROBE_SOURCE), encoding="utf-8")
    run_checked(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include"),
            str(source),
            *(str(ROOT / item) for item in PROBE_SOURCES),
            "-o",
            str(binary),
        ],
        timeout=120.0,
    )
    output = run_checked(
        [str(binary), str(policy_assembly), str(spill_assembly)]
    ).stdout.decode()
    expected = (
        "PASS constant_vector_and_all_true_mask_to_final",
        "PASS fp_vp_binary_dynamic_frm_to_final",
        "PASS mixed_mask_constant_pool_to_final",
        "PASS zve32_zve64_fixed_vlen_boundaries_and_chunks",
        "PASS fixed_and_scalable_vp_policy_to_final",
        "PASS fractional_and_mask_whole_spill_to_final",
        "PASS no_v_target_fails_closed",
    )
    for line in expected:
        if line not in output:
            raise RuntimeError(f"programmatic RVV probe lacks result: {line}")
    print("PASS rvv_oir_to_final_programmatic")

    policy_text = policy_assembly.read_text(encoding="utf-8")
    spill_text = spill_assembly.read_text(encoding="utf-8")
    for fragment in (
        "ta, ma", "tu, mu", "vmv.v.v", "vadd.vv", "vse32.v",
        "vs1r.v", "vl1re32.v",
    ):
        if fragment not in policy_text + spill_text:
            raise RuntimeError(f"VP policy assembly lacks {fragment}")
    policy_driver.write_text(
        textwrap.dedent(
            """
            #include <stdint.h>

            extern void fixed_policy(int32_t *, int32_t *, const int32_t *,
                                     const int32_t *, const int32_t *, int, int);
            extern int scalable_policy(int32_t *, int32_t *, const int32_t *,
                                       const int32_t *, const int32_t *, int, int);
            extern int fractional_spill_probe(int);
            extern int mask_spill_probe(void);

            __attribute__((noinline)) void vector_clobber(void) {
              __asm__ volatile(
                  "vsetvli zero, zero, e32, m8, ta, ma\\n\\t"
                  "vmv.v.i v0, 0\\n\\t"
                  "vmv.v.i v8, 0\\n\\t"
                  "vmv.v.i v16, 0\\n\\t"
                  "vmv.v.i v24, 0\\n\\t"
                  : : : "memory");
            }

            static int check_fixed(int evl) {
              int32_t out[7] = {0};
              int32_t pass_out[7] = {0};
              int32_t lhs[7];
              int32_t rhs[7];
              int32_t pass[7];
              for (int lane = 0; lane < 7; ++lane) {
                lhs[lane] = lane + 1;
                rhs[lane] = 10 - lane;
                pass[lane] = 100 + lane;
              }
              fixed_policy(out, pass_out, lhs, rhs, pass, 0, evl);
              int effective = evl < 0 ? 0 : (evl > 7 ? 7 : evl);
              for (int lane = 0; lane < 7; ++lane) {
                int active = ((0x55 >> lane) & 1) != 0;
                int32_t expected = lane < effective && active
                                       ? lhs[lane] + rhs[lane] + rhs[lane]
                                       : pass[lane];
                if (out[lane] != expected) return 1 + lane;
                if (pass_out[lane] != pass[lane]) return 10 + lane;
              }
              return 0;
            }

            static int check_scalable(void) {
              int32_t out[32] = {0};
              int32_t pass_out[32] = {0};
              int32_t lhs[32];
              int32_t rhs[32];
              int32_t pass[32];
              for (int lane = 0; lane < 32; ++lane) {
                lhs[lane] = lane + 3;
                rhs[lane] = 2 * lane + 1;
                pass[lane] = 300 + lane;
              }
              int vlmax = scalable_policy(out, pass_out, lhs, rhs, pass,
                                          0x7fffffff, 1);
              if (vlmax < 4 || vlmax > 32) return 1;
              for (int lane = 0; lane < vlmax; ++lane) {
                int32_t expected = lane == 0
                                       ? lhs[lane] + rhs[lane] + rhs[lane]
                                       : pass[lane];
                if (out[lane] != expected) return 2 + lane;
                if (pass_out[lane] != pass[lane]) return 40 + lane;
              }
              int second = scalable_policy(out, pass_out, lhs, rhs, pass,
                                            0x7fffffff, vlmax + 1);
              if (second != vlmax) return 80;
              for (int lane = 0; lane < vlmax; ++lane) {
                int32_t expected = lhs[lane] + rhs[lane] + rhs[lane];
                if (out[lane] != expected) return 81 + lane;
                if (pass_out[lane] != pass[lane]) return 114 + lane;
              }
              return 0;
            }

            int main(void) {
              if (fractional_spill_probe(37) != 37) return 1;
              if (mask_spill_probe() != 1) return 2;
              const int evls[] = {0, 1, 6, 7, 8};
              for (unsigned index = 0; index < sizeof(evls) / sizeof(evls[0]);
                   ++index) {
                int result = check_fixed(evls[index]);
                if (result != 0) return 10 * (int)(index + 1) + result;
              }
              int scalable = check_scalable();
              return scalable == 0 ? 0 : 100 + scalable;
            }
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )
    run_checked(
        [assembler, "-march=rv64gcv", "-mabi=lp64d", str(policy_assembly),
         "-o", str(policy_object)]
    )
    run_checked(
        [assembler, "-march=rv64gcv", "-mabi=lp64d", str(spill_assembly),
         "-o", str(spill_object)]
    )
    run_checked(
        [gcc, "-static", "-march=rv64gcv", "-mabi=lp64d",
         str(policy_object), str(spill_object), str(policy_driver),
         "-o", str(policy_binary)]
    )
    for vlen in VLENS:
        run_checked(
            [qemu, "-cpu", f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
             str(policy_binary)]
        )
    print("PASS fixed_evl_and_fixed_scalable_tu_mu_qemu_all_vlen")
    print("PASS fractional_and_mask_whole_spill_qemu_all_vlen")


def assert_final_mir(final_mir: str) -> None:
    for opcode in SCALAR_PSEUDOS:
        if re.search(rf"^  {re.escape(opcode)}(?: |$)", final_mir, re.MULTILINE):
            raise RuntimeError(f"Final MIR retains scalar pseudo {opcode}")
    if "PseudoV" in final_mir:
        raise RuntimeError("Final MIR retains an RVV pseudo")
    for opcode in ("VSETVLI", "VLE", "VMV.V.I", "VINT.VV", "VSE"):
        if re.search(rf"^  {re.escape(opcode)}(?: |$)", final_mir, re.MULTILINE) is None:
            raise RuntimeError(f"Final MIR lacks concrete opcode {opcode}")
    if re.search(r"^  VMSET\.M(?: |$)", final_mir, re.MULTILINE) is not None:
        raise RuntimeError("all-true loop mask was not eliminated from Final MIR")


def test_compiler_pipeline(
    directory: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    qemu: str,
) -> None:
    final_mir = directory / "loop.final.mir"
    assembly = directory / "loop.s"
    obj = directory / "loop.o"
    executable = directory / "loop"

    run_checked(
        [
            str(COMPILER),
            str(LOOP_SOURCE),
            "--emit-mir",
            "--emit-mir-stage=final",
            "-O2",
            "-march=rv64gcv",
            "-mabi=lp64d",
            "-o",
            str(final_mir),
        ]
    )
    assert_final_mir(final_mir.read_text(encoding="utf-8"))
    print("PASS rvv_compiler_final_mir_has_no_pseudos")

    run_checked(
        [
            str(COMPILER),
            str(LOOP_SOURCE),
            "-S",
            "-O2",
            "-march=rv64gcv",
            "-mabi=lp64d",
            "-o",
            str(assembly),
        ]
    )
    assembly_text = assembly.read_text(encoding="utf-8")
    for mnemonic in ("vsetvli", "vle32.v", "vadd.vv", "vse32.v"):
        if mnemonic not in assembly_text:
            raise RuntimeError(f"compiler assembly lacks {mnemonic}")
    if "vmset.m" in assembly_text or "v0.t" in assembly_text:
        raise RuntimeError("all-true loop mask survived in compiler assembly")
    alias = re.search(
        r"^\s+(li|la|mv|j|call|ret|bnez|beqz)(?:\s|$)",
        assembly_text,
        re.MULTILINE,
    )
    if alias is not None:
        raise RuntimeError(f"AsmPrinter emitted source pseudo alias {alias.group(1)}")

    run_checked(
        [assembler, "-march=rv64gcv", "-mabi=lp64d", str(assembly), "-o", str(obj)]
    )
    disassembly = run_checked([objdump, "-dr", str(obj)]).stdout.decode()
    for needle in (
        "vsetvli",
        "vle32.v",
        "vadd.vv",
        "vse32.v",
        "R_RISCV_PCREL_HI20",
        "R_RISCV_PCREL_LO12_I",
    ):
        if needle not in disassembly:
            raise RuntimeError(f"RVV object disassembly lacks {needle}")
    if "vmset.m" in disassembly or "v0.t" in disassembly:
        raise RuntimeError("all-true loop mask survived in object disassembly")
    print("PASS rvv_final_assembly_and_objdump")

    run_checked(
        [
            gcc,
            "-static",
            "-march=rv64gcv",
            "-mabi=lp64d",
            "-mcmodel=medany",
            str(obj),
            str(RUNTIME),
            "-o",
            str(executable),
        ]
    )
    input_bytes = b"5 1 2 3 4 5\n"
    for vlen in VLENS:
        result = subprocess.run(
            [qemu, "-cpu", f"rv64,v=true,vlen={vlen},elen=64", str(executable)],
            cwd=ROOT,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=30.0,
        )
        if result.returncode != 8 or result.stdout:
            raise RuntimeError(
                f"RVV backend executable failed at VLEN={vlen}: "
                f"exit={result.returncode}, stdout={result.stdout!r}, "
                f"stderr={result.stderr.decode(errors='replace')}"
            )
        print(f"PASS rvv_backend_qemu_vlen_{vlen}")


def main() -> int:
    try:
        require_file(COMPILER, "release compiler")
        require_file(RUNTIME, "RISC-V SysY runtime")
        require_file(LOOP_SOURCE, "loop-vectorization source")
        cxx = find_cxx()
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="rvv-backend-infra-") as temp:
            directory = Path(temp)
            test_programmatic_isel(directory, cxx, assembler, gcc, qemu)
            test_compiler_pipeline(directory, assembler, gcc, objdump, qemu)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_backend: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
