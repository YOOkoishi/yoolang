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
#include "mir/MachineInstrDesc.h"
#include "mir/AsmPrinter.h"
#include "mir/MIRPrinter.h"
#include "mir/MIRVerifier.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/mir/MIRVectorRegAlloc.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class Failure final : public std::exception {
  public:
    explicit Failure(std::string message) : message_(std::move(message)) {}
    const char *what() const noexcept override { return message_.c_str(); }

  private:
    std::string message_;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                          ": requirement failed: " #condition);                               \
        }                                                                                       \
    } while (false)

mir::TypeInfo void_type() {
    return {mir::ValueType::Void, "void", 0, 1, std::nullopt};
}

mir::Register state_reg(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::VSTATE);
}

mir::Register scratch_reg(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::GPR,
                                   mir::ValueType::Ptr);
}

mir::MachineVectorInfo vector_info(const mir::MachineVectorType &type,
                                   mir::RVVOperation operation,
                                   mir::MachineVectorAVL avl) {
    mir::MachineVectorInfo info(type);
    info.operation = operation;
    info.avl = avl;
    info.tail_policy = mir::VectorTailPolicy::Undisturbed;
    info.mask_policy = mir::VectorMaskPolicy::Undisturbed;
    return info;
}

mir::MachineInstr make_setvli(const mir::MachineVectorType &type) {
    auto info = vector_info(type, mir::RVVOperation::SetVL, mir::MachineVectorAVL::operand(1));
    return mir::MachineInstr(
        mir::Opcode::RVVSetVLI,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::reg_use(
             mir::Register::physical("a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::implicit_reg_def(state_reg("vl")),
         mir::MachineOperand::implicit_reg_def(state_reg("vtype"))},
        std::move(info));
}

mir::MachineInstr make_binary(mir::Opcode opcode, const mir::MachineVectorType &type,
                              mir::Register dest, mir::Register passthrough,
                              mir::Register lhs, mir::Register rhs,
                              std::optional<mir::Register> mask = std::nullopt) {
    std::vector<mir::MachineOperand> operands = {
        mir::MachineOperand::reg_def(std::move(dest)),
        mir::MachineOperand::reg_use(std::move(passthrough)),
        mir::MachineOperand::reg_use(std::move(lhs)),
        mir::MachineOperand::reg_use(std::move(rhs)),
    };
    auto info = vector_info(type, mir::RVVOperation::Add, mir::MachineVectorAVL::current_vl());
    info.passthrough_operand = 1;
    if (mask.has_value()) {
        info.mask_operand = operands.size();
        operands.push_back(mir::MachineOperand::reg_use(std::move(*mask)));
    }
    operands.push_back(mir::MachineOperand::implicit_reg_use(state_reg("vl")));
    operands.push_back(mir::MachineOperand::implicit_reg_use(state_reg("vtype")));
    return mir::MachineInstr(opcode, std::move(operands), std::move(info));
}

mir::MachineInstr make_execution_mask_copy(const mir::MachineVectorType &type,
                                           mir::Register source) {
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    auto info = vector_info(type, mir::RVVOperation::Copy,
                            mir::MachineVectorAVL::current_vl());
    return mir::MachineInstr(
        mir::Opcode::RVVMaskCopy,
        {mir::MachineOperand::reg_def(mir::Register::physical_vector(
             "v0", mir::RegisterClass::VMASK, mask_type)),
         mir::MachineOperand::reg_use(std::move(source)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(info));
}

mir::MachineInstr make_whole_spill(const mir::MachineVectorType &type, int slot,
                                   mir::Register source) {
    auto info = vector_info(type, mir::RVVOperation::Spill,
                            mir::MachineVectorAVL::whole_register());
    return mir::MachineInstr(
        mir::Opcode::RVVWholeRegSpill,
        {mir::MachineOperand::slot(slot), mir::MachineOperand::reg_use(std::move(source)),
         mir::MachineOperand::implicit_reg_use(state_reg("vlenb")),
         mir::MachineOperand::implicit_reg_def(scratch_reg("t5")),
         mir::MachineOperand::implicit_reg_def(scratch_reg("t6"))},
        std::move(info));
}

mir::MachineInstr make_whole_reload(const mir::MachineVectorType &type, int slot,
                                    mir::Register dest) {
    auto info = vector_info(type, mir::RVVOperation::Reload,
                            mir::MachineVectorAVL::whole_register());
    return mir::MachineInstr(
        mir::Opcode::RVVWholeRegReload,
        {mir::MachineOperand::reg_def(std::move(dest)), mir::MachineOperand::slot(slot),
         mir::MachineOperand::implicit_reg_use(state_reg("vlenb")),
         mir::MachineOperand::implicit_reg_def(scratch_reg("t5")),
         mir::MachineOperand::implicit_reg_def(scratch_reg("t6"))},
        std::move(info));
}

mir::MachineInstr make_extract(const mir::MachineVectorType &type, mir::Register source) {
    auto info = vector_info(type, mir::RVVOperation::Extract,
                            mir::MachineVectorAVL::current_vl());
    return mir::MachineInstr(
        mir::Opcode::RVVExtractElement,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR,
                                     type.element_type())),
         mir::MachineOperand::reg_use(std::move(source)), mir::MachineOperand::imm(0),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(info));
}

void require_verify_ok(const mir::Module &module, mir::MIRVerificationStage stage) {
    const auto result = mir::verify_module(module, stage);
    if (!result.ok) {
        throw Failure("unexpected verifier failure: " + result.message);
    }
}

void require_verify_failure(const mir::Module &module, mir::MIRVerificationStage stage,
                            const std::string &needle) {
    const auto result = mir::verify_module(module, stage);
    REQUIRE(!result.ok);
    if (result.message.find(needle) == std::string::npos) {
        throw Failure("verifier failure did not contain '" + needle + "': " + result.message);
    }
}

void require_invalid_argument(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw Failure("expected std::invalid_argument");
}

std::unique_ptr<mir::Module> make_physical_binary_module(
    const mir::MachineVectorType &info_type, mir::Register dest, mir::Register passthrough,
    mir::Register lhs, mir::Register rhs, std::optional<mir::Register> mask = std::nullopt) {
    auto module = std::make_unique<mir::Module>("physical-rvv");
    module->target().has_vector = true;
    module->target().minimum_vlen_bits = 128;
    auto *function = module->create_function("physical", void_type(), {});
    auto *block = function->create_block("entry");
    block->add_instr(make_setvli(info_type));
    block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, info_type, std::move(dest),
                                 std::move(passthrough), std::move(lhs), std::move(rhs),
                                 std::move(mask)));
    function->layout_frame();
    return module;
}

void test_every_opcode_has_a_descriptor() {
    for (std::size_t index = 0; index < static_cast<std::size_t>(mir::Opcode::Count); ++index) {
        auto opcode = static_cast<mir::Opcode>(index);
        const auto &desc = mir::instruction_desc(opcode);
        REQUIRE(desc.name != nullptr);
        REQUIRE(std::string(desc.name) != "UNKNOWN");
        REQUIRE(desc.latency > 0);
        REQUIRE(desc.accepts_operand_count(desc.min_operands));
        REQUIRE(std::string(mir::opcode_name(opcode)) == desc.name);
    }
}

void test_control_and_memory_effects() {
    const auto &branch = mir::instruction_desc(mir::Opcode::BranchNonZero);
    REQUIRE(branch.has_flag(mir::MIF_Terminator));
    REQUIRE(branch.has_flag(mir::MIF_Branch));
    REQUIRE(branch.has_flag(mir::MIF_Barrier));

    const auto &load = mir::instruction_desc(mir::Opcode::LoadMem);
    REQUIRE(load.may_load());
    REQUIRE(!load.may_store());
    const auto &store = mir::instruction_desc(mir::Opcode::StoreMem);
    REQUIRE(!store.may_load());
    REQUIRE(store.may_store());

    const auto &call = mir::instruction_desc(mir::Opcode::Call);
    REQUIRE(call.has_flag(mir::MIF_Call));
    REQUIRE(call.has_flag(mir::MIF_Barrier));
    REQUIRE(call.has_flag(mir::MIF_SideEffects));
    REQUIRE(call.may_load());
    REQUIRE(call.may_store());
}

void test_entry_copy_descriptors_allow_implicit_argument_uses() {
    for (auto opcode : {mir::Opcode::Move, mir::Opcode::FMove, mir::Opcode::FmvWX,
                        mir::Opcode::FmvXW}) {
        const auto &desc = mir::instruction_desc(opcode);
        REQUIRE(!desc.accepts_operand_count(1));
        REQUIRE(desc.accepts_operand_count(2));
        REQUIRE(desc.accepts_operand_count(6));
    }
}

void test_machine_vector_type_and_register_identity() {
    const auto m1 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M1);
    const auto m2 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M2);
    const auto fixed = mir::MachineVectorType::fixed(mir::ValueType::F32, 32,
                                                     mir::VectorLMUL::M1, 8);
    const auto fixed3 = mir::MachineVectorType::fixed(mir::ValueType::I32, 32,
                                                      mir::VectorLMUL::M1, 3);
    const auto fixed7 = mir::MachineVectorType::fixed(mir::ValueType::I32, 32,
                                                      mir::VectorLMUL::M2, 7);
    const auto fixed31 = mir::MachineVectorType::fixed(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M4, 31);
    const auto fixed_mask = mir::MachineVectorType::mask_for(fixed3);
    const auto mask = mir::MachineVectorType::mask_for(m2);
    REQUIRE(m1.register_group_width() == 1);
    REQUIRE(m2.register_group_width() == 2);
    REQUIRE(fixed.is_fixed());
    REQUIRE(fixed.fixed_lanes() == 8);
    REQUIRE(fixed.fixed_bits() == 256);
    REQUIRE(fixed3.fixed_lanes() == 3);
    REQUIRE(fixed3.fixed_bits() == 96);
    REQUIRE(fixed7.fixed_lanes() == 7);
    REQUIRE(fixed7.fixed_bits() == 224);
    REQUIRE(fixed31.fixed_lanes() == 31);
    REQUIRE(fixed31.fixed_bits() == 992);
    REQUIRE(fixed_mask.fixed_lanes() == 3);
    REQUIRE(fixed_mask.fixed_bits() == 3);
    REQUIRE(mask.is_mask());
    REQUIRE(mask.mask_ratio() == 16);
    REQUIRE(mask.register_group_width() == 1);
    REQUIRE(mir::machine_vector_type_name(m2) == "sv<i32,e32,m2>");
    REQUIRE(mir::machine_vector_type_name(mask) == "sv<i1,e32,m2,vbool16>");
    REQUIRE(mir::machine_vector_type_name(fixed3) == "fv<3xi32,96b,e32,m1>");
    REQUIRE(mir::machine_vector_type_name(fixed_mask) ==
            "fv<3xi1,3b,e32,m1,vbool32>");

    require_invalid_argument([] {
        (void)mir::MachineVectorType::scalable(mir::ValueType::Ptr, 32,
                                               mir::VectorLMUL::M1);
    });
    require_invalid_argument([] {
        (void)mir::MachineVectorType::scalable(mir::ValueType::I32, 64,
                                               mir::VectorLMUL::M1);
    });
    require_invalid_argument([] {
        (void)mir::MachineVectorType::fixed(mir::ValueType::I32, 32,
                                            mir::VectorLMUL::M1, 0);
    });
    require_invalid_argument([] {
        (void)mir::MachineVectorType::scalable(mir::ValueType::I1, 32,
                                               mir::VectorLMUL::M2, 32);
    });

    const auto a0_void =
        mir::Register::physical("a0", mir::RegisterClass::GPR, mir::ValueType::Void);
    const auto a0_i32 =
        mir::Register::physical("a0", mir::RegisterClass::GPR, mir::ValueType::I32);
    REQUIRE(a0_void == a0_i32);

    const auto v8_m1 = mir::Register::physical_vector("v8", mir::RegisterClass::VR, m1);
    const auto v8_m2 = mir::Register::physical_vector("v8", mir::RegisterClass::VR, m2);
    REQUIRE(v8_m1 == v8_m2);
    REQUIRE(v8_m1.vector_group_width == 1);
    REQUIRE(v8_m2.vector_group_width == 2);
}

void test_rvv_descriptor_schema_and_effects() {
    for (std::size_t index = static_cast<std::size_t>(mir::Opcode::RVVSetVL);
         index <= static_cast<std::size_t>(mir::Opcode::RVVWholeRegReload); ++index) {
        const auto &desc = mir::instruction_desc(static_cast<mir::Opcode>(index));
        REQUIRE(desc.has_flag(mir::MIF_Vector));
        REQUIRE(desc.has_flag(mir::MIF_Pseudo));
        REQUIRE(desc.operand_constraints != nullptr);
        REQUIRE(desc.operand_constraint_count >= desc.min_explicit_operands);
        REQUIRE(desc.accepts_explicit_operand_count(desc.min_explicit_operands));
    }

    for (std::size_t index = static_cast<std::size_t>(mir::Opcode::RISCVVSetVLI);
         index <= static_cast<std::size_t>(mir::Opcode::RISCVVWholeRegReload); ++index) {
        const auto &desc = mir::instruction_desc(static_cast<mir::Opcode>(index));
        REQUIRE(desc.has_flag(mir::MIF_Vector));
        REQUIRE(!desc.has_flag(mir::MIF_Pseudo));
        REQUIRE(desc.operand_constraints != nullptr);
        REQUIRE(desc.operand_constraint_count >= desc.min_explicit_operands);
        REQUIRE(desc.accepts_explicit_operand_count(desc.min_explicit_operands));
    }

    const auto &setvli = mir::instruction_desc(mir::Opcode::RVVSetVLI);
    REQUIRE(setvli.implicitly_defines(mir::MVS_VL));
    REQUIRE(setvli.implicitly_defines(mir::MVS_VTYPE));
    REQUIRE(setvli.has_flag(mir::MIF_SideEffects));
    REQUIRE(setvli.operand_constraints[0].role == mir::MachineOperandRole::Def);

    const auto &binary = mir::instruction_desc(mir::Opcode::RVVIntBinaryVV);
    REQUIRE(binary.implicitly_uses(mir::MVS_VL));
    REQUIRE(binary.implicitly_uses(mir::MVS_VTYPE));
    REQUIRE(binary.operand_constraints[0].tied_to == 1);
    REQUIRE(binary.operand_constraints[0].carries_vector_group);
    REQUIRE(binary.operand_constraints[0].requires_group_alignment);

    const auto &floating = mir::instruction_desc(mir::Opcode::RVVFloatBinaryVV);
    REQUIRE(floating.has_flag(mir::MIF_HasRoundingMode));
    REQUIRE(floating.implicitly_uses(mir::MVS_FRM));

    const auto &indexed_load = mir::instruction_desc(mir::Opcode::RVVLoadIndexed);
    REQUIRE(indexed_load.may_load());
    REQUIRE(!indexed_load.may_store());
    const auto &indexed_store = mir::instruction_desc(mir::Opcode::RVVStoreIndexed);
    REQUIRE(!indexed_store.may_load());
    REQUIRE(indexed_store.may_store());

    const auto &whole = mir::instruction_desc(mir::Opcode::RVVWholeRegSpill);
    REQUIRE(whole.has_flag(mir::MIF_WholeRegister));
    REQUIRE(whole.implicitly_uses(mir::MVS_VLENB));
}

void test_positive_pre_ra_and_canonical_printer() {
    mir::Module module("pre-ra-rvv");
    module.target().has_vector = true;
    module.target().minimum_vlen_bits = 128;
    auto *function = module.create_function("pre_ra", void_type(), {});
    auto *block = function->create_block("entry");

    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M2);
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    auto dest = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto mask = function->regs().create_virtual_vector(mir::RegisterClass::VMASK, mask_type);
    auto reload = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    const auto float_type = mir::MachineVectorType::scalable(mir::ValueType::F32, 32,
                                                             mir::VectorLMUL::M1);
    auto float_dest =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, float_type);
    auto float_passthrough =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, float_type);
    auto float_lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, float_type);
    auto float_rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, float_type);
    const auto slot = function->add_scalable_stack_slot("rvv.spill", type,
                                                         mir::StackSlotKind::Spill);

    block->add_instr(make_setvli(type));
    block->add_instr(make_execution_mask_copy(type, mask));
    block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs,
                                 mir::Register::physical_vector(
                                     "v0", mir::RegisterClass::VMASK, mask_type)));
    block->add_instr(make_setvli(float_type));
    auto float_binary = make_binary(mir::Opcode::RVVFloatBinaryVV, float_type, float_dest,
                                    float_passthrough, float_lhs, float_rhs);
    float_binary.vector_info().rounding = mir::VectorRoundingMode::Dynamic;
    float_binary.operands().push_back(mir::MachineOperand::implicit_reg_use(state_reg("frm")));
    block->add_instr(std::move(float_binary));
    block->add_instr(make_whole_spill(type, slot, lhs));
    block->add_instr(make_whole_reload(type, slot, reload));
    function->layout_frame();

    REQUIRE(function->stack_slot(slot) != nullptr);
    REQUIRE(!function->stack_slot(slot)->has_fixed_offset);
    REQUIRE(function->stack_slot(slot)->scalable_offset.has_value());
    REQUIRE(function->stack_slot(slot)->scalable_offset->vlenb_eighths == 0);
    REQUIRE(function->scalable_frame_size().vlenb_eighths == 16);
    require_verify_ok(module, mir::MIRVerificationStage::PreRA);

    std::ostringstream printed;
    mir::MIRPrinter printer(printed);
    printer.print(module);
    const auto text = printed.str();
    REQUIRE(text.find("sv<i32,e32,m2>") != std::string::npos);
    REQUIRE(text.find("size=vlenb*2 align=vlenb*2 offset=scalable+0") !=
            std::string::npos);
    REQUIRE(text.find("rvv{op=add,type=sv<i32,e32,m2>,avl=current-vl") !=
            std::string::npos);
    REQUIRE(text.find("(implicit-use vtype)") != std::string::npos);
}

void test_positive_post_ra_vector_banks() {
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M2);
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    auto module = make_physical_binary_module(
        type,
        mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, type),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, type),
        mir::Register::physical_vector("v0", mir::RegisterClass::VMASK, mask_type));
    require_verify_ok(*module, mir::MIRVerificationStage::PostRA);
}

void test_vector_ra_groups_ties_mask_and_fractional() {
    mir::Module module("vector-ra-groups");
    module.target().has_vector = true;
    module.target().minimum_vlen_bits = 128;
    auto *function = module.create_function("groups", void_type(), {});
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M2);
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    auto dest = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto live_through =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto mask =
        function->regs().create_virtual_vector(mir::RegisterClass::VMASK, mask_type);
    block->add_instr(make_setvli(type));
    block->add_instr(make_execution_mask_copy(type, mask));
    block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough,
                                 lhs, rhs,
                                 mir::Register::physical_vector(
                                     "v0", mir::RegisterClass::VMASK, mask_type)));
    block->add_instr(make_extract(type, live_through));
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PreRA);

    pass::MIRVectorRegAllocator allocator;
    const auto allocated = allocator.run(*function);
    REQUIRE(allocated.success);
    REQUIRE(allocated.changed);
    REQUIRE(allocated.relegalize_requests.empty());
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);

    const auto &mask_copy = block->instructions()[1];
    const auto &binary = block->instructions()[2];
    const auto &allocated_dest = binary.operands()[0].reg_value();
    const auto &allocated_passthrough = binary.operands()[1].reg_value();
    REQUIRE(allocated_dest == allocated_passthrough);
    REQUIRE(allocated_dest.name != "v0");
    REQUIRE(allocated_dest.vector_group_width == 2);
    REQUIRE(std::stoul(allocated_dest.name.substr(1)) % 2 == 0);
    REQUIRE(binary.operands()[4].reg_value().name == "v0");
    REQUIRE(mask_copy.operands()[0].reg_value().name == "v0");
    REQUIRE(mask_copy.operands()[1].reg_value().name != "v0");
    REQUIRE(allocated_dest.name !=
            block->instructions()[3].operands()[1].reg_value().name);
    REQUIRE(function->regs().allocation(dest).has_value());
    REQUIRE(function->regs().allocation(passthrough).has_value());
    REQUIRE(*function->regs().allocation(dest) == *function->regs().allocation(passthrough));

    mir::Module fractional_module("fractional-ra");
    fractional_module.target().has_vector = true;
    auto *fractional_function =
        fractional_module.create_function("fractional", void_type(), {});
    auto *fractional_block = fractional_function->create_block("entry");
    const auto fractional = mir::MachineVectorType::scalable(
        mir::ValueType::I32, 32, mir::VectorLMUL::MF2);
    auto fractional_dest = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, fractional);
    auto fractional_passthrough = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VRNoV0, fractional);
    auto fractional_lhs = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, fractional);
    auto fractional_rhs = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, fractional);
    fractional_block->add_instr(make_setvli(fractional));
    fractional_block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, fractional,
                                            fractional_dest, fractional_passthrough,
                                            fractional_lhs, fractional_rhs));
    fractional_function->layout_frame();
    require_verify_ok(fractional_module, mir::MIRVerificationStage::PreRA);
    const auto fractional_allocated = allocator.run(*fractional_function);
    REQUIRE(fractional_allocated.success);
    require_verify_ok(fractional_module, mir::MIRVerificationStage::PostRA);
    REQUIRE(fractional_block->instructions()[1].operands()[0]
                .reg_value()
                .vector_group_width == 1);
    REQUIRE(fractional_block->instructions()[1].operands()[0].reg_value().name != "v0");
}

void test_vector_ra_call_clobber_spills_and_reloads() {
    mir::Module module("vector-ra-call");
    module.target().has_vector = true;
    auto *function = module.create_function("call_live", void_type(), {});
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    auto cross = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto pass1 = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs1 = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs1 = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto dest2 = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto pass2 = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto rhs2 = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    block->add_instr(make_setvli(type));
    block->add_instr(
        make_binary(mir::Opcode::RVVIntBinaryVV, type, cross, pass1, lhs1, rhs1));
    block->add_instr(mir::Opcode::Call, {mir::MachineOperand::symbol("callee")});
    block->add_instr(make_setvli(type));
    block->add_instr(
        make_binary(mir::Opcode::RVVIntBinaryVV, type, dest2, pass2, cross, rhs2));
    function->note_call();
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PreRA);

    pass::MIRVectorRegAllocator allocator;
    const auto allocated = allocator.run(*function);
    REQUIRE(allocated.success);
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);

    std::optional<std::size_t> call_index;
    bool saw_spill = false;
    bool saw_reload_after_call = false;
    for (std::size_t index = 0; index < block->instructions().size(); ++index) {
        const auto opcode = block->instructions()[index].opcode();
        if (opcode == mir::Opcode::Call) {
            call_index = index;
        } else if (opcode == mir::Opcode::RVVWholeRegSpill && !call_index.has_value()) {
            saw_spill = true;
        } else if (opcode == mir::Opcode::RVVWholeRegReload && call_index.has_value()) {
            saw_reload_after_call = true;
        }
    }
    REQUIRE(call_index.has_value());
    REQUIRE(saw_spill);
    REQUIRE(saw_reload_after_call);
    REQUIRE(std::any_of(function->stack_slots().begin(), function->stack_slots().end(),
                        [](const mir::StackSlot &slot) {
                            return slot.name.find("rvv.spill.v") == 0 &&
                                   slot.scalable_size.has_value();
                        }));
}

void test_vector_ra_fixed_shape_pressure_spills() {
    mir::Module module("fixed-vector-pressure");
    module.target().has_vector = true;
    auto *function = module.create_function("pressure", void_type(), {});
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::fixed(mir::ValueType::I32, 32,
                                                    mir::VectorLMUL::M1, 3);
    block->add_instr(make_setvli(type));
    for (unsigned index = 0; index < 32; ++index) {
        auto input = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0,
                                                             type);
        block->add_instr(make_extract(type, input));
    }
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PreRA);

    pass::MIRVectorRegAllocator allocator;
    const auto allocated = allocator.run(*function);
    REQUIRE(allocated.success);
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);
    REQUIRE(std::any_of(block->instructions().begin(), block->instructions().end(),
                        [](const mir::MachineInstr &instr) {
                            return instr.opcode() == mir::Opcode::RVVWholeRegReload;
                        }));
    REQUIRE(std::any_of(function->stack_slots().begin(), function->stack_slots().end(),
                        [](const mir::StackSlot &slot) {
                            return slot.name.find("rvv.spill.v") == 0 &&
                                   slot.type.vector_type.has_value() &&
                                   slot.type.vector_type->is_fixed() &&
                                   slot.type.vector_type->fixed_lanes() == 3 &&
                                   slot.scalable_size.has_value() &&
                                   slot.scalable_size->vlenb_eighths == 8;
                        }));
}

void test_m8_scavenge_scalar_liveness_and_final_whole_spills() {
    auto module = std::make_unique<mir::Module>("m8-scavenge");
    module->target().has_vector = true;
    module->target().arch = "rv64gcv";
    module->target().minimum_vlen_bits = 128;
    auto *function = module->create_function("pressure", void_type(), {});
    function->reserve_outgoing_arg_bytes(24);
    const mir::TypeInfo scalar_slot_type{mir::ValueType::I32, "i32", 4, 4,
                                         std::nullopt};
    (void)function->add_stack_slot("fixed", scalar_slot_type,
                                   mir::StackSlotKind::Spill);
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::fixed(
        mir::ValueType::I32, 32, mir::VectorLMUL::M8, 31);
    block->add_instr(make_setvli(type));

    auto scalar_live =
        function->regs().create_virtual(mir::RegisterClass::GPR,
                                        mir::ValueType::I32);
    block->add_instr(
        mir::Opcode::LoadImm,
        {mir::MachineOperand::reg_def(scalar_live), mir::MachineOperand::imm(123)});

    std::vector<mir::Register> results;
    for (unsigned index = 0; index < 8; ++index) {
        auto dest = function->regs().create_virtual_vector(
            mir::RegisterClass::VRNoV0, type);
        auto passthrough = function->regs().create_virtual_vector(
            mir::RegisterClass::VRNoV0, type);
        auto lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR,
                                                           type);
        auto rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR,
                                                           type);
        block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest,
                                     passthrough, lhs, rhs));
        results.push_back(dest);
    }

    auto scalar_result =
        function->regs().create_virtual(mir::RegisterClass::GPR,
                                        mir::ValueType::I32);
    block->add_instr(
        mir::Opcode::AddI,
        {mir::MachineOperand::reg_def(scalar_result),
         mir::MachineOperand::reg_use(scalar_live), mir::MachineOperand::imm(1)});
    for (auto result : results) {
        block->add_instr(make_extract(type, result));
    }
    function->layout_frame();

    pass::PassContext context;
    context.set_machine_module(std::move(module));
    pass::MIRRegAllocPass regalloc;
    const auto allocated = regalloc.run(context);
    if (!allocated.success) {
        throw Failure("M8 instruction-aware scavenging failed: " +
                      allocated.message);
    }
    module = context.take_machine_module();
    require_verify_ok(*module, mir::MIRVerificationStage::PostRA);
    function = module->functions().front().get();
    block = function->blocks().front().get();

    std::optional<std::string> scalar_def;
    std::optional<std::string> scalar_use;
    bool saw_spill = false;
    bool saw_reload = false;
    for (const auto &instr : block->instructions()) {
        if (instr.opcode() == mir::Opcode::LoadImm &&
            instr.operands()[1].int_value() == 123) {
            scalar_def = instr.operands()[0].reg_value().name;
        }
        if (instr.opcode() == mir::Opcode::AddI &&
            instr.operands()[2].int_value() == 1) {
            scalar_use = instr.operands()[1].reg_value().name;
        }
        if (instr.opcode() != mir::Opcode::RVVWholeRegSpill &&
            instr.opcode() != mir::Opcode::RVVWholeRegReload) {
            continue;
        }
        saw_spill = saw_spill || instr.opcode() == mir::Opcode::RVVWholeRegSpill;
        saw_reload = saw_reload || instr.opcode() == mir::Opcode::RVVWholeRegReload;
        REQUIRE(instr.operands().size() == 5);
        REQUIRE(instr.operands()[3].is_implicit() &&
                instr.operands()[3].is_def() &&
                instr.operands()[3].reg_value().name == "t5");
        REQUIRE(instr.operands()[4].is_implicit() &&
                instr.operands()[4].is_def() &&
                instr.operands()[4].reg_value().name == "t6");
    }
    REQUIRE(saw_spill && saw_reload);
    REQUIRE(scalar_def.has_value() && scalar_use.has_value());
    REQUIRE(*scalar_def == *scalar_use);
    REQUIRE(*scalar_def != "t5" && *scalar_def != "t6");
    REQUIRE(function->frame_size() % 16 == 0);
    REQUIRE(function->scalable_frame_size().is_valid());
    REQUIRE(function->scalable_frame_size().vlenb_eighths % 8 == 0);

    std::string error;
    if (!pass::expand_machine_pseudos(*module, error)) {
        throw Failure("M8 whole-register expansion failed: " + error);
    }
    require_verify_ok(*module, mir::MIRVerificationStage::Final);
    bool saw_final_spill = false;
    bool saw_final_reload = false;
    for (const auto &instr : block->instructions()) {
        REQUIRE(!mir::instruction_desc(instr.opcode()).has_flag(mir::MIF_Pseudo));
        saw_final_spill = saw_final_spill ||
                          instr.opcode() == mir::Opcode::RISCVVWholeRegSpill;
        saw_final_reload = saw_final_reload ||
                           instr.opcode() == mir::Opcode::RISCVVWholeRegReload;
    }
    REQUIRE(saw_final_spill && saw_final_reload);
    std::ostringstream assembly;
    mir::AsmPrinter(assembly).print(*module);
    REQUIRE(assembly.str().find("\tcsrr t6, vlenb") != std::string::npos);
    REQUIRE(assembly.str().find("\tvs8r.v") != std::string::npos);
    REQUIRE(assembly.str().find("\tvl8re32.v") != std::string::npos);
}

void test_scalable_frame_all_integral_lmul_layout_and_expansion() {
    mir::Module module("integral-lmul-frame");
    module.target().has_vector = true;
    module.target().arch = "rv64gcv";
    auto *function = module.create_function("all_lmul", void_type(), {});
    function->reserve_outgoing_arg_bytes(24);
    function->note_call();
    const mir::TypeInfo fixed_type{mir::ValueType::I32, "i32", 4, 4,
                                   std::nullopt};
    (void)function->add_stack_slot("fixed", fixed_type,
                                   mir::StackSlotKind::Spill);
    auto *block = function->create_block("entry");
    const std::vector<mir::VectorLMUL> lmuls = {
        mir::VectorLMUL::M1, mir::VectorLMUL::M2,
        mir::VectorLMUL::M4, mir::VectorLMUL::M8};
    for (std::size_t index = 0; index < lmuls.size(); ++index) {
        const auto lmul = lmuls[index];
        const auto type = mir::MachineVectorType::scalable(
            mir::ValueType::I32, 32, lmul);
        const auto slot = function->add_scalable_stack_slot(
            "slot-" + std::to_string(index), type, mir::StackSlotKind::Spill);
        const auto physical = mir::Register::physical_vector(
            "v8", mir::RegisterClass::VRNoV0, type);
        block->add_instr(make_whole_spill(type, slot, physical));
        block->add_instr(make_whole_reload(type, slot, physical));
    }
    block->add_instr(mir::Opcode::Call,
                     {mir::MachineOperand::symbol("callee")});
    function->layout_frame();
    REQUIRE(function->frame_size() == 48);
    REQUIRE(function->frame_size() % 16 == 0);

    std::vector<const mir::StackSlot *> scalable_slots;
    for (const auto &slot : function->stack_slots()) {
        if (!slot.scalable_size.has_value()) {
            continue;
        }
        REQUIRE(slot.scalable_offset.has_value());
        REQUIRE(slot.scalable_align.has_value());
        REQUIRE(slot.scalable_offset->vlenb_eighths %
                    slot.scalable_align->vlenb_eighths ==
                0);
        scalable_slots.push_back(&slot);
    }
    REQUIRE(scalable_slots.size() == lmuls.size());
    for (std::size_t lhs = 0; lhs < scalable_slots.size(); ++lhs) {
        const auto lhs_begin = scalable_slots[lhs]->scalable_offset->vlenb_eighths;
        const auto lhs_end = lhs_begin +
                             scalable_slots[lhs]->scalable_size->vlenb_eighths;
        for (std::size_t rhs = lhs + 1; rhs < scalable_slots.size(); ++rhs) {
            const auto rhs_begin =
                scalable_slots[rhs]->scalable_offset->vlenb_eighths;
            const auto rhs_end = rhs_begin +
                                 scalable_slots[rhs]->scalable_size->vlenb_eighths;
            REQUIRE(lhs_end <= rhs_begin || rhs_end <= lhs_begin);
        }
    }
    REQUIRE(function->scalable_frame_size().vlenb_eighths == 128);
    for (unsigned vlenb : {16U, 32U, 64U, 128U}) {
        const auto dynamic_bytes =
            function->scalable_frame_size().vlenb_eighths * vlenb / 8U;
        REQUIRE((dynamic_bytes + static_cast<unsigned>(function->frame_size())) %
                    16U ==
                0U);
    }
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);

    std::string error;
    if (!pass::expand_machine_pseudos(module, error)) {
        throw Failure("integral-LMUL whole-register expansion failed: " + error);
    }
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    std::ostringstream assembly;
    mir::AsmPrinter(assembly).print(module);
    for (unsigned width : {1U, 2U, 4U, 8U}) {
        REQUIRE(assembly.str().find("\tvs" + std::to_string(width) + "r.v") !=
                std::string::npos);
        REQUIRE(assembly.str().find("\tvl" + std::to_string(width) +
                                    "re32.v") != std::string::npos);
    }
    const auto &text = assembly.str();
    const auto scalable_alloc = text.find("\tsub sp, sp, t6");
    const auto fixed_alloc = text.find("\taddi sp, sp, -48");
    const auto fixed_free = text.find("\taddi sp, sp, 48");
    const auto scalable_free = text.rfind("\tadd sp, sp, t6");
    REQUIRE(scalable_alloc < fixed_alloc);
    REQUIRE(fixed_free < scalable_free);
}

void test_call_frame_metadata_is_fail_closed() {
    mir::Module missing_note("call-without-frame-note");
    auto *missing_note_function =
        missing_note.create_function("missing_note", void_type(), {});
    auto *missing_note_block = missing_note_function->create_block("entry");
    missing_note_block->add_instr(mir::Opcode::Call,
                                  {mir::MachineOperand::symbol("callee")});
    missing_note_function->layout_frame();
    require_verify_failure(missing_note, mir::MIRVerificationStage::PreRA,
                           "contains a call but has_call is false");

    mir::Module stale_note("frame-note-without-call");
    auto *stale_note_function =
        stale_note.create_function("stale_note", void_type(), {});
    (void)stale_note_function->create_block("entry");
    stale_note_function->note_call();
    stale_note_function->layout_frame();
    require_verify_failure(stale_note, mir::MIRVerificationStage::PreRA,
                           "has_call is true but contains no call");

    const auto address = mir::Register::physical(
        "t0", mir::RegisterClass::GPR, mir::ValueType::Ptr);
    const auto count = mir::Register::physical(
        "t1", mir::RegisterClass::GPR, mir::ValueType::I32);
    const auto small_memzero = mir::MachineInstr(
        mir::Opcode::MemZero,
        {mir::MachineOperand::reg_use(address), mir::MachineOperand::imm(0),
         mir::MachineOperand::imm(mir::kMemZeroMemsetThresholdBytes - 1)});
    const auto large_memzero = mir::MachineInstr(
        mir::Opcode::MemZero,
        {mir::MachineOperand::reg_use(address), mir::MachineOperand::imm(0),
         mir::MachineOperand::imm(mir::kMemZeroMemsetThresholdBytes)});
    const auto dynamic_memzero = mir::MachineInstr(
        mir::Opcode::MemZero,
        {mir::MachineOperand::reg_use(address), mir::MachineOperand::imm(0),
         mir::MachineOperand::reg_use(count)});
    REQUIRE(!mir::machine_instr_may_call(small_memzero));
    REQUIRE(mir::machine_instr_may_call(large_memzero));
    REQUIRE(mir::machine_instr_may_call(dynamic_memzero));

    mir::Module memzero_module("memzero-call-contract");
    auto *memzero_function =
        memzero_module.create_function("large_clear", void_type(), {});
    auto *memzero_block = memzero_function->create_block("entry");
    memzero_block->add_instr(large_memzero);
    memzero_function->layout_frame();
    require_verify_failure(memzero_module, mir::MIRVerificationStage::PreRA,
                           "contains a call but has_call is false");
    memzero_function->note_call();
    memzero_function->layout_frame();
    require_verify_ok(memzero_module, mir::MIRVerificationStage::PreRA);
}

void test_incoming_stack_arg_fast_path_without_scalable_frame() {
    mir::Module module("incoming-fixed-frame");
    const mir::TypeInfo i32_type{mir::ValueType::I32, "i32", 4, 4,
                                 std::nullopt};
    auto *function = module.create_function(
        "ninth_fixed", i32_type, std::vector<mir::TypeInfo>(9, i32_type));
    (void)function->add_stack_slot("fixed", i32_type,
                                   mir::StackSlotKind::Spill);
    auto *block = function->create_block("entry");
    block->add_instr(
        mir::Opcode::LoadIncomingArg,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "t0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::imm(8),
         mir::MachineOperand::type(mir::ValueType::I32)});
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);
    const auto expected_offset =
        static_cast<std::int64_t>(function->frame_size()) + 8;

    std::string error;
    if (!pass::expand_machine_pseudos(module, error)) {
        throw Failure("fixed incoming-argument expansion failed: " + error);
    }
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    REQUIRE(block->instructions().size() == 1);
    const auto &load = block->instructions().front();
    REQUIRE(load.opcode() == mir::Opcode::RISCVLW);
    REQUIRE(load.operands().size() == 3);
    REQUIRE(load.operands()[1].reg_value().name == "sp");
    REQUIRE(load.operands()[2].int_value() == expected_offset);
}

void test_incoming_stack_arg_accounts_for_aligned_scalable_frame() {
    mir::Module module("incoming-with-scalable-frame");
    module.target().has_vector = true;
    module.target().arch = "rv64gc_zve32x";
    module.target().minimum_vlen_bits = 32;
    const mir::TypeInfo i32_type{mir::ValueType::I32, "i32", 4, 4,
                                 std::nullopt};
    auto *function = module.create_function(
        "ninth", i32_type, std::vector<mir::TypeInfo>(9, i32_type));
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(
        mir::ValueType::I32, 32, mir::VectorLMUL::M1);
    const auto slot = function->add_scalable_stack_slot(
        "m1", type, mir::StackSlotKind::Spill);
    const auto vector = mir::Register::physical_vector(
        "v8", mir::RegisterClass::VRNoV0, type);
    block->add_instr(make_whole_spill(type, slot, vector));
    block->add_instr(make_whole_reload(type, slot, vector));
    block->add_instr(
        mir::Opcode::LoadIncomingArg,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "t0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::imm(0),
         mir::MachineOperand::type(mir::ValueType::I32)});
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);

    std::string error;
    if (!pass::expand_machine_pseudos(module, error)) {
        throw Failure("incoming scalable-frame expansion failed: " + error);
    }
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    bool saw_vlenb = false;
    bool saw_round_up = false;
    bool saw_align = false;
    bool saw_entry_base = false;
    bool saw_load = false;
    for (const auto &instr : block->instructions()) {
        const auto &ops = instr.operands();
        saw_vlenb = saw_vlenb || instr.opcode() == mir::Opcode::RISCVReadVLENB;
        saw_round_up = saw_round_up ||
                       (instr.opcode() == mir::Opcode::AddI && ops.size() == 3 &&
                        ops[0].reg_value().name == "t6" &&
                        ops[2].int_value() == 15);
        saw_align = saw_align ||
                    (instr.opcode() == mir::Opcode::AndI && ops.size() == 3 &&
                     ops[0].reg_value().name == "t6" &&
                     ops[2].int_value() == -16);
        saw_entry_base = saw_entry_base ||
                         (instr.opcode() == mir::Opcode::Add && ops.size() == 3 &&
                          ops[0].reg_value().name == "t6" &&
                          ops[1].reg_value().name == "sp" &&
                          ops[2].reg_value().name == "t6");
        saw_load = saw_load ||
                   (instr.opcode() == mir::Opcode::RISCVLW && ops.size() == 3 &&
                    ops[0].reg_value().name == "t0" &&
                    ops[1].reg_value().name == "t6" &&
                    ops[2].int_value() == 0);
    }
    REQUIRE(saw_vlenb && saw_round_up && saw_align && saw_entry_base && saw_load);
    for (unsigned vlenb : {4U, 8U, 16U, 32U}) {
        const auto logical = vlenb;
        const auto allocated = (logical + 15U) & ~15U;
        REQUIRE(allocated >= logical && allocated % 16U == 0U);
    }
}

void test_vector_ra_fractional_whole_spill() {
    mir::Module module("fractional-call-spill");
    module.target().has_vector = true;
    auto *function = module.create_function("fractional_call", void_type(), {});
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::MF2);
    auto input =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    block->add_instr(make_setvli(type));
    auto splat_info = vector_info(type, mir::RVVOperation::Splat,
                                  mir::MachineVectorAVL::current_vl());
    block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVSplatVXTA,
        {mir::MachineOperand::reg_def(input),
         mir::MachineOperand::reg_use(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(splat_info)));
    block->add_instr(mir::Opcode::Call, {mir::MachineOperand::symbol("callee")});
    block->add_instr(make_setvli(type));
    block->add_instr(make_extract(type, input));
    function->note_call();
    function->layout_frame();
    require_verify_ok(module, mir::MIRVerificationStage::PreRA);

    pass::MIRVectorRegAllocator allocator;
    const auto allocated = allocator.run(*function);
    REQUIRE(allocated.success);
    REQUIRE(allocated.relegalize_requests.empty());
    require_verify_ok(module, mir::MIRVerificationStage::PostRA);
    bool saw_spill = false;
    bool saw_reload = false;
    for (const auto &instr : block->instructions()) {
        saw_spill = saw_spill || instr.opcode() == mir::Opcode::RVVWholeRegSpill;
        saw_reload = saw_reload || instr.opcode() == mir::Opcode::RVVWholeRegReload;
    }
    REQUIRE(saw_spill && saw_reload);
    const auto &slot = function->stack_slots().front();
    REQUIRE(slot.scalable_size == mir::MachineScalableSize{8});
    REQUIRE(slot.scalable_align == mir::MachineScalableSize{8});

    std::string error;
    if (!pass::expand_machine_pseudos(module, error)) {
        throw Failure("fractional whole-register expansion failed: " + error);
    }
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    std::ostringstream assembly;
    mir::AsmPrinter(assembly).print(module);
    REQUIRE(assembly.str().find("\tvs1r.v") != std::string::npos);
    REQUIRE(assembly.str().find("\tvl1re32.v") != std::string::npos);
}

void test_fractional_and_mask_pressure_whole_spills() {
    auto run_fractional_pressure = [] {
        mir::Module module("fractional-pressure-spill");
        module.target().has_vector = true;
        auto *function = module.create_function("fractional_pressure", void_type(), {});
        auto *block = function->create_block("entry");
        const auto type = mir::MachineVectorType::scalable(
            mir::ValueType::I32, 32, mir::VectorLMUL::MF2);
        block->add_instr(make_setvli(type));
        std::vector<mir::Register> values;
        for (unsigned index = 0; index < 32; ++index) {
            auto value = function->regs().create_virtual_vector(
                mir::RegisterClass::VRNoV0, type);
            auto info = vector_info(type, mir::RVVOperation::Splat,
                                    mir::MachineVectorAVL::current_vl());
            block->add_instr(mir::MachineInstr(
                mir::Opcode::RVVSplatVXTA,
                {mir::MachineOperand::reg_def(value),
                 mir::MachineOperand::reg_use(mir::Register::physical(
                     "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
                 mir::MachineOperand::implicit_reg_use(state_reg("vl")),
                 mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
                std::move(info)));
            values.push_back(value);
        }
        for (auto value : values) {
            block->add_instr(make_extract(type, value));
        }
        function->layout_frame();
        require_verify_ok(module, mir::MIRVerificationStage::PreRA);
        pass::MIRVectorRegAllocator allocator;
        const auto result = allocator.run(*function);
        REQUIRE(result.success);
        REQUIRE(!function->stack_slots().empty());
        for (const auto &slot : function->stack_slots()) {
            REQUIRE(slot.scalable_size == mir::MachineScalableSize{8});
            REQUIRE(slot.scalable_align == mir::MachineScalableSize{8});
        }
        require_verify_ok(module, mir::MIRVerificationStage::PostRA);
        std::string error;
        REQUIRE(pass::expand_machine_pseudos(module, error));
        require_verify_ok(module, mir::MIRVerificationStage::Final);
        std::ostringstream assembly;
        mir::AsmPrinter(assembly).print(module);
        REQUIRE(assembly.str().find("\tvs1r.v") != std::string::npos);
        REQUIRE(assembly.str().find("\tvl1re32.v") != std::string::npos);
    };

    auto run_mask_pressure = [] {
        mir::Module module("mask-pressure-spill");
        module.target().has_vector = true;
        auto *function = module.create_function("mask_pressure_spill", void_type(), {});
        auto *block = function->create_block("entry");
        const auto data_type = mir::MachineVectorType::scalable(
            mir::ValueType::I32, 32, mir::VectorLMUL::M8);
        const auto mask_type = mir::MachineVectorType::mask_for(data_type);
        block->add_instr(make_setvli(data_type));
        std::vector<mir::Register> masks;
        for (unsigned index = 0; index < 32; ++index) {
            auto mask = function->regs().create_virtual_vector(
                mir::RegisterClass::VMASK, mask_type);
            auto info = vector_info(data_type, mir::RVVOperation::MaskSet,
                                    mir::MachineVectorAVL::current_vl());
            block->add_instr(mir::MachineInstr(
                mir::Opcode::RVVMaskSet,
                {mir::MachineOperand::reg_def(mask),
                 mir::MachineOperand::implicit_reg_use(state_reg("vl")),
                 mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
                std::move(info)));
            masks.push_back(mask);
        }
        for (auto mask : masks) {
            block->add_instr(make_execution_mask_copy(data_type, mask));
        }
        function->layout_frame();
        require_verify_ok(module, mir::MIRVerificationStage::PreRA);
        pass::MIRVectorRegAllocator allocator;
        const auto result = allocator.run(*function);
        REQUIRE(result.success);
        REQUIRE(!function->stack_slots().empty());
        for (const auto &slot : function->stack_slots()) {
            REQUIRE(slot.type.vector_type.has_value());
            REQUIRE(slot.type.vector_type->is_mask());
            REQUIRE(slot.scalable_size == mir::MachineScalableSize{8});
            REQUIRE(slot.scalable_align == mir::MachineScalableSize{8});
        }
        require_verify_ok(module, mir::MIRVerificationStage::PostRA);
        std::string error;
        REQUIRE(pass::expand_machine_pseudos(module, error));
        require_verify_ok(module, mir::MIRVerificationStage::Final);
        std::ostringstream assembly;
        mir::AsmPrinter(assembly).print(module);
        REQUIRE(assembly.str().find("\tvs1r.v") != std::string::npos);
        REQUIRE(assembly.str().find("\tvl1re32.v") != std::string::npos);
    };

    run_fractional_pressure();
    run_mask_pressure();
}

void test_vector_ra_mask_and_tie_relegalize_requests() {
    mir::Module mask_module("mask-pressure-relegalize");
    mask_module.target().has_vector = true;
    auto *mask_function = mask_module.create_function("mask_pressure", void_type(), {});
    auto *mask_block = mask_function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    auto mask1 = mask_function->regs().create_virtual_vector(mir::RegisterClass::VMASK,
                                                              mask_type);
    auto mask2 = mask_function->regs().create_virtual_vector(mir::RegisterClass::VMASK,
                                                              mask_type);
    auto make_masked = [&](mir::Register mask) {
        auto dest = mask_function->regs().create_virtual_vector(
            mir::RegisterClass::VRNoV0, type);
        auto passthrough = mask_function->regs().create_virtual_vector(
            mir::RegisterClass::VRNoV0, type);
        auto lhs =
            mask_function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
        auto rhs =
            mask_function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
        mask_block->add_instr(make_execution_mask_copy(type, mask));
        mask_block->add_instr(make_binary(
            mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs,
            mir::Register::physical_vector("v0", mir::RegisterClass::VMASK,
                                           mask_type)));
    };
    mask_block->add_instr(make_setvli(type));
    make_masked(mask1);
    make_masked(mask2);
    mask_function->layout_frame();
    require_verify_ok(mask_module, mir::MIRVerificationStage::PreRA);
    pass::MIRVectorRegAllocator allocator;
    const auto mask_result = allocator.run(*mask_function);
    REQUIRE(mask_result.success);
    REQUIRE(mask_result.relegalize_requests.empty());
    REQUIRE(mask_function->regs().allocation(mask1).has_value());
    REQUIRE(mask_function->regs().allocation(mask2).has_value());
    REQUIRE(mask_function->regs().allocation(mask1)->name != "v0");
    REQUIRE(mask_function->regs().allocation(mask2)->name != "v0");
    REQUIRE(mask_function->regs().allocation(mask1)->name !=
            mask_function->regs().allocation(mask2)->name);
    require_verify_ok(mask_module, mir::MIRVerificationStage::PostRA);

    mir::Module mask_spill_module("mask-call-spill");
    mask_spill_module.target().has_vector = true;
    auto *mask_spill_function =
        mask_spill_module.create_function("mask_call", void_type(), {});
    auto *mask_spill_block = mask_spill_function->create_block("entry");
    const auto wide_data = mir::MachineVectorType::scalable(
        mir::ValueType::I32, 32, mir::VectorLMUL::M8);
    const auto ordinary_mask = mir::MachineVectorType::mask_for(wide_data);
    auto live_mask = mask_spill_function->regs().create_virtual_vector(
        mir::RegisterClass::VMASK, ordinary_mask);
    mask_spill_block->add_instr(make_setvli(wide_data));
    auto set_info = vector_info(wide_data, mir::RVVOperation::MaskSet,
                                mir::MachineVectorAVL::current_vl());
    mask_spill_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMaskSet,
        {mir::MachineOperand::reg_def(live_mask),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(set_info)));
    mask_spill_block->add_instr(
        mir::Opcode::Call, {mir::MachineOperand::symbol("callee")});
    mask_spill_block->add_instr(make_setvli(wide_data));
    mask_spill_block->add_instr(make_execution_mask_copy(wide_data, live_mask));
    mask_spill_function->note_call();
    mask_spill_function->layout_frame();
    require_verify_ok(mask_spill_module, mir::MIRVerificationStage::PreRA);
    const auto mask_spill_result = allocator.run(*mask_spill_function);
    REQUIRE(mask_spill_result.success);
    REQUIRE(mask_spill_result.relegalize_requests.empty());
    require_verify_ok(mask_spill_module, mir::MIRVerificationStage::PostRA);
    bool saw_mask_spill = false;
    bool saw_mask_reload = false;
    for (const auto &instr : mask_spill_block->instructions()) {
        if (instr.opcode() == mir::Opcode::RVVWholeRegSpill) {
            saw_mask_spill = true;
            REQUIRE(instr.operands()[1].reg_value().reg_class ==
                    mir::RegisterClass::VMASK);
            REQUIRE(instr.operands()[1].reg_value().name != "v0");
        }
        if (instr.opcode() == mir::Opcode::RVVWholeRegReload) {
            saw_mask_reload = true;
            REQUIRE(instr.operands()[0].reg_value().reg_class ==
                    mir::RegisterClass::VMASK);
            REQUIRE(instr.operands()[0].reg_value().name != "v0");
        }
    }
    REQUIRE(saw_mask_spill && saw_mask_reload);
    const auto &mask_slot = mask_spill_function->stack_slots().front();
    REQUIRE(mask_slot.type.vector_type == ordinary_mask);
    REQUIRE(mask_slot.scalable_size == mir::MachineScalableSize{8});
    REQUIRE(mask_slot.scalable_align == mir::MachineScalableSize{8});
    std::string mask_expansion_error;
    if (!pass::expand_machine_pseudos(mask_spill_module,
                                      mask_expansion_error)) {
        throw Failure("mask whole-register expansion failed: " +
                      mask_expansion_error);
    }
    require_verify_ok(mask_spill_module, mir::MIRVerificationStage::Final);
    std::ostringstream mask_assembly;
    mir::AsmPrinter(mask_assembly).print(mask_spill_module);
    REQUIRE(mask_assembly.str().find("\tvs1r.v") != std::string::npos);
    REQUIRE(mask_assembly.str().find("\tvl1re32.v") != std::string::npos);

    mir::Module tie_module("tie-relegalize");
    tie_module.target().has_vector = true;
    auto *tie_function = tie_module.create_function("tie_live", void_type(), {});
    auto *tie_block = tie_function->create_block("entry");
    auto dest1 =
        tie_function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough1 =
        tie_function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs1 = tie_function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs1 = tie_function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto dest2 =
        tie_function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough2 =
        tie_function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto rhs2 = tie_function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    tie_block->add_instr(make_setvli(type));
    tie_block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest1,
                                     passthrough1, lhs1, rhs1));
    tie_block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest2,
                                     passthrough2, passthrough1, rhs2));
    tie_function->layout_frame();
    require_verify_ok(tie_module, mir::MIRVerificationStage::PreRA);
    const auto tie_result = allocator.run(*tie_function);
    REQUIRE(!tie_result.success);
    REQUIRE(std::any_of(tie_result.relegalize_requests.begin(),
                        tie_result.relegalize_requests.end(),
                        [](const pass::MIRVectorRelegalizeRequest &request) {
                            return request.reason ==
                                   pass::MIRVectorRelegalizeReason::TiedLiveRangeConflict;
                        }));
}

void test_fail_closed_vector_metadata_and_state() {
    mir::Module module("metadata-negative");
    module.target().has_vector = true;
    auto *function = module.create_function("metadata", void_type(), {});
    auto *block = function->create_block("entry");
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    auto dest = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs));
    function->layout_frame();

    block->instructions().back().clear_vector_info();
    require_verify_failure(module, mir::MIRVerificationStage::PreRA,
                           "lacks structured vector metadata");

    block->instructions().back() =
        make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs);
    block->instructions().back().operands().back() = mir::MachineOperand::implicit_reg_use(
        mir::Register::physical("a1", mir::RegisterClass::GPR));
    require_verify_failure(module, mir::MIRVerificationStage::PreRA,
                           "implicit use of vtype");

    block->instructions().back() =
        make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs);
    block->instructions().back().operands()[0] = mir::MachineOperand::reg_use(dest);
    require_verify_failure(module, mir::MIRVerificationStage::PreRA,
                           "must define a register");
}

void test_post_ra_group_overlap_alignment_and_v0_rules() {
    const auto m1 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M1);
    const auto m2 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M2);
    const auto mask_m1 = mir::MachineVectorType::mask_for(m1);

    auto misaligned = make_physical_binary_module(
        m2, mir::Register::physical_vector("v3", mir::RegisterClass::VRNoV0, m2),
        mir::Register::physical_vector("v3", mir::RegisterClass::VRNoV0, m2),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, m2),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, m2));
    require_verify_failure(*misaligned, mir::MIRVerificationStage::PostRA, "misaligned");

    auto out_of_range = make_physical_binary_module(
        m1, mir::Register::physical_vector("v32", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v32", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, m1));
    require_verify_failure(*out_of_range, mir::MIRVerificationStage::PostRA, "v0-v31");

    auto different_lmul_overlap = make_physical_binary_module(
        m1, mir::Register::physical_vector("v8", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v8", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v8", mir::RegisterClass::VR, m2),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, m1));
    require_verify_failure(*different_lmul_overlap, mir::MIRVerificationStage::PostRA,
                           "groups overlap");

    auto v0_conflict = make_physical_binary_module(
        m1, mir::Register::physical_vector("v0", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v0", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v2", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v3", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v0", mir::RegisterClass::VMASK, mask_m1));
    require_verify_failure(*v0_conflict, mir::MIRVerificationStage::PostRA, "conflicts with v0");

    auto bad_mask = make_physical_binary_module(
        m1, mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, m1),
        mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, m1),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v1", mir::RegisterClass::VMASK, mask_m1));
    require_verify_failure(*bad_mask, mir::MIRVerificationStage::PostRA,
                           "post-RA mask operand must be physical v0");

    auto bad_nov0 = make_physical_binary_module(
        m1, mir::Register::physical_vector("v0", mir::RegisterClass::VRNoV0, m1),
        mir::Register::physical_vector("v0", mir::RegisterClass::VRNoV0, m1),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, m1),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, m1));
    require_verify_failure(*bad_nov0, mir::MIRVerificationStage::PostRA, "cannot use v0");
}

void test_scalable_slot_and_whole_register_fail_closed_rules() {
    const auto fractional = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                             mir::VectorLMUL::MF2);
    mir::Module fractional_module("fractional-spill");
    fractional_module.target().has_vector = true;
    auto *fractional_function =
        fractional_module.create_function("fractional", void_type(), {});
    auto *fractional_block = fractional_function->create_block("entry");
    auto fractional_reg = fractional_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, fractional);
    const auto fractional_slot = fractional_function->add_scalable_stack_slot(
        "fractional", fractional, mir::StackSlotKind::Spill);
    fractional_block->add_instr(
        make_whole_spill(fractional, fractional_slot, fractional_reg));
    fractional_function->layout_frame();
    require_verify_ok(fractional_module, mir::MIRVerificationStage::PreRA);
    REQUIRE(fractional_function->stack_slot(fractional_slot)->scalable_size ==
            mir::MachineScalableSize{8});
    fractional_function->stack_slot(fractional_slot)->scalable_size =
        mir::MachineScalableSize::from_lmul(mir::VectorLMUL::MF2);
    require_verify_failure(
        fractional_module, mir::MIRVerificationStage::PreRA,
        "scalable size/alignment does not match the physical register group");

    const auto m1 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M1);
    mir::Module missing_scratch_module("missing-whole-spill-scratch");
    missing_scratch_module.target().has_vector = true;
    auto *missing_scratch_function =
        missing_scratch_module.create_function("missing_scratch", void_type(), {});
    auto *missing_scratch_block = missing_scratch_function->create_block("entry");
    auto missing_scratch_reg = missing_scratch_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, m1);
    const auto missing_scratch_slot = missing_scratch_function->add_scalable_stack_slot(
        "missing-scratch", m1, mir::StackSlotKind::Spill);
    auto missing_scratch =
        make_whole_spill(m1, missing_scratch_slot, missing_scratch_reg);
    missing_scratch.operands().erase(missing_scratch.operands().begin() + 3);
    missing_scratch_block->add_instr(std::move(missing_scratch));
    missing_scratch_function->layout_frame();
    require_verify_failure(missing_scratch_module, mir::MIRVerificationStage::PreRA,
                           "operand count does not match machine instruction descriptor");

    mir::Module wrong_scratch_role_module("wrong-whole-spill-scratch-role");
    wrong_scratch_role_module.target().has_vector = true;
    auto *wrong_scratch_function =
        wrong_scratch_role_module.create_function("wrong_scratch", void_type(), {});
    auto *wrong_scratch_block = wrong_scratch_function->create_block("entry");
    auto wrong_scratch_reg = wrong_scratch_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, m1);
    const auto wrong_scratch_slot = wrong_scratch_function->add_scalable_stack_slot(
        "wrong-scratch", m1, mir::StackSlotKind::Spill);
    auto wrong_scratch = make_whole_spill(m1, wrong_scratch_slot, wrong_scratch_reg);
    wrong_scratch.operands()[3] =
        mir::MachineOperand::implicit_reg_use(scratch_reg("t5"));
    wrong_scratch_block->add_instr(std::move(wrong_scratch));
    wrong_scratch_function->layout_frame();
    require_verify_failure(wrong_scratch_role_module,
                           mir::MIRVerificationStage::PreRA,
                           "whole-register pseudo requires implicit def of t5");

    mir::Module fixed_slot_module("fixed-slot-spill");
    fixed_slot_module.target().has_vector = true;
    auto *fixed_function = fixed_slot_module.create_function("fixed", void_type(), {});
    auto *fixed_block = fixed_function->create_block("entry");
    auto fixed_reg =
        fixed_function->regs().create_virtual_vector(mir::RegisterClass::VR, m1);
    mir::TypeInfo fake_fixed{mir::ValueType::I32, mir::machine_vector_type_name(m1), 16, 16,
                             std::nullopt};
    fake_fixed.vector_type = m1;
    const auto fixed_slot =
        fixed_function->add_stack_slot("fake-fixed", fake_fixed, mir::StackSlotKind::Spill);
    fixed_block->add_instr(make_whole_spill(m1, fixed_slot, fixed_reg));
    fixed_function->layout_frame();
    require_verify_failure(fixed_slot_module, mir::MIRVerificationStage::PreRA,
                           "requires a scalable vector stack slot");

    const auto m2 = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                     mir::VectorLMUL::M2);
    mir::Module mismatch_module("mismatched-slot");
    mismatch_module.target().has_vector = true;
    auto *mismatch_function = mismatch_module.create_function("mismatch", void_type(), {});
    auto *mismatch_block = mismatch_function->create_block("entry");
    auto mismatch_reg =
        mismatch_function->regs().create_virtual_vector(mir::RegisterClass::VR, m2);
    const auto mismatch_slot = mismatch_function->add_scalable_stack_slot(
        "mismatch", m1, mir::StackSlotKind::Spill);
    mismatch_block->add_instr(make_whole_spill(m2, mismatch_slot, mismatch_reg));
    mismatch_function->layout_frame();
    require_verify_failure(mismatch_module, mir::MIRVerificationStage::PreRA,
                           "slot size/type does not match");

    mir::Module scalar_access_module("scalar-scalable-access");
    scalar_access_module.target().has_vector = true;
    auto *scalar_function =
        scalar_access_module.create_function("scalar_access", void_type(), {});
    auto *scalar_block = scalar_function->create_block("entry");
    const auto scalable_slot = scalar_function->add_scalable_stack_slot(
        "scalable", m1, mir::StackSlotKind::Spill);
    scalar_block->add_instr(
        mir::Opcode::LoadStackAddr,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR, mir::ValueType::Ptr)),
         mir::MachineOperand::slot(scalable_slot)});
    scalar_function->layout_frame();
    require_verify_failure(scalar_access_module, mir::MIRVerificationStage::PreRA,
                           "dedicated scalable spill/reload pseudo");
}

void test_post_ra_virtual_and_final_pseudo_rejection() {
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    mir::Module virtual_module("post-ra-virtual");
    virtual_module.target().has_vector = true;
    auto *function = virtual_module.create_function("virtual", void_type(), {});
    auto *block = function->create_block("entry");
    auto dest = function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto passthrough =
        function->regs().create_virtual_vector(mir::RegisterClass::VRNoV0, type);
    auto lhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    auto rhs = function->regs().create_virtual_vector(mir::RegisterClass::VR, type);
    block->add_instr(make_binary(mir::Opcode::RVVIntBinaryVV, type, dest, passthrough, lhs, rhs));
    function->layout_frame();
    require_verify_failure(virtual_module, mir::MIRVerificationStage::PostRA,
                           "virtual register after RA");

    auto physical = make_physical_binary_module(
        type, mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, type),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, type));
    require_verify_ok(*physical, mir::MIRVerificationStage::PostRA);
    require_verify_failure(*physical, mir::MIRVerificationStage::Final,
                           "pseudo instruction remains at Final stage");

    mir::Module scalar_pseudo("final-scalar-pseudo");
    auto *scalar_function = scalar_pseudo.create_function("scalar", void_type(), {});
    auto *scalar_block = scalar_function->create_block("entry");
    scalar_block->add_instr(
        mir::Opcode::LoadImm,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("a0", mir::RegisterClass::GPR,
                                     mir::ValueType::I32)),
         mir::MachineOperand::imm(7)});
    scalar_function->layout_frame();
    require_verify_ok(scalar_pseudo, mir::MIRVerificationStage::PostRA);
    require_verify_failure(scalar_pseudo, mir::MIRVerificationStage::Final,
                           "pseudo instruction remains at Final stage");
}

void test_asm_printer_rejects_unexpanded_rvv_pseudos() {
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    auto physical = make_physical_binary_module(
        type, mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v8", mir::RegisterClass::VRNoV0, type),
        mir::Register::physical_vector("v10", mir::RegisterClass::VR, type),
        mir::Register::physical_vector("v12", mir::RegisterClass::VR, type));
    std::ostringstream output;
    bool rejected_vector = false;
    try {
        mir::AsmPrinter printer(output);
        printer.print(*physical);
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string(error.what()).find("unexpanded pseudo") != std::string::npos);
        rejected_vector = true;
    }
    REQUIRE(rejected_vector);

    mir::Module scalar("asm-printer-scalar-pseudo");
    auto *function = scalar.create_function("scalar", void_type(), {});
    auto *block = function->create_block("entry");
    block->add_instr(
        mir::Opcode::LoadImm,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("a0", mir::RegisterClass::GPR,
                                     mir::ValueType::I32)),
         mir::MachineOperand::imm(7)});
    function->layout_frame();
    bool rejected_scalar = false;
    try {
        std::ostringstream scalar_output;
        mir::AsmPrinter printer(scalar_output);
        printer.print(scalar);
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string(error.what()).find("unexpanded pseudo") != std::string::npos);
        rejected_scalar = true;
    }
    REQUIRE(rejected_scalar);
}

void test_scalar_load_immediate_expansion_is_width_correct() {
    mir::Module module("load-immediate-expansion");
    auto *function = module.create_function("constants", void_type(), {});
    auto *block = function->create_block("entry");
    block->add_instr(
        mir::Opcode::LoadImm,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("a0", mir::RegisterClass::GPR,
                                     mir::ValueType::I32)),
         mir::MachineOperand::imm(0xffffffffLL)});
    block->add_instr(
        mir::Opcode::LoadImm,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR,
                                     mir::ValueType::Ptr)),
         mir::MachineOperand::imm(0x100000000LL)});
    function->layout_frame();

    std::string error;
    REQUIRE(pass::expand_machine_pseudos(module, error));
    REQUIRE(error.empty());
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    for (const auto &instr : block->instructions()) {
        REQUIRE(!mir::instruction_desc(instr.opcode()).has_flag(mir::MIF_Pseudo));
    }

    std::ostringstream output;
    mir::AsmPrinter(output).print(module);
    const auto assembly = output.str();
    REQUIRE(assembly.find("\taddi a0, zero, -1") != std::string::npos);
    REQUIRE(assembly.find("\tlui t0, 256") != std::string::npos);
    REQUIRE(assembly.find("\tslli t0, t0, 12") != std::string::npos);
}

void test_pseudo_expansion_produces_final_rvv() {
    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    const auto mask_type = mir::MachineVectorType::mask_for(type);
    mir::Module module("expanded-rvv");
    module.target().has_vector = true;
    module.target().arch = "rv64gcv";
    auto *function = module.create_function("expanded", void_type(), {});
    auto *block = function->create_block("entry");
    auto setvli = make_setvli(type);
    setvli.vector_info().tail_policy = mir::VectorTailPolicy::Agnostic;
    setvli.vector_info().mask_policy = mir::VectorMaskPolicy::Agnostic;
    block->add_instr(std::move(setvli));

    auto mask_info = vector_info(type, mir::RVVOperation::MaskSet,
                                 mir::MachineVectorAVL::current_vl());
    mask_info.tail_policy = mir::VectorTailPolicy::Agnostic;
    mask_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
    block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMaskSet,
        {mir::MachineOperand::reg_def(mir::Register::physical_vector(
             "v1", mir::RegisterClass::VMASK, mask_type)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(mask_info)));
    auto mask_copy = make_execution_mask_copy(
        type, mir::Register::physical_vector("v1", mir::RegisterClass::VMASK,
                                             mask_type));
    mask_copy.vector_info().tail_policy = mir::VectorTailPolicy::Agnostic;
    mask_copy.vector_info().mask_policy = mir::VectorMaskPolicy::Agnostic;
    block->add_instr(std::move(mask_copy));

    auto binary_info = vector_info(type, mir::RVVOperation::Add,
                                   mir::MachineVectorAVL::current_vl());
    binary_info.tail_policy = mir::VectorTailPolicy::Agnostic;
    binary_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
    binary_info.mask_operand = 3;
    block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVIntBinaryVVTA,
        {mir::MachineOperand::reg_def(mir::Register::physical_vector(
             "v8", mir::RegisterClass::VRNoV0, type)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v10", mir::RegisterClass::VR, type)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v11", mir::RegisterClass::VR, type)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v0", mir::RegisterClass::VMASK, mask_type)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(binary_info)));
    function->layout_frame();

    require_verify_ok(module, mir::MIRVerificationStage::PostRA);
    std::string error;
    REQUIRE(pass::expand_machine_pseudos(module, error));
    REQUIRE(error.empty());
    require_verify_ok(module, mir::MIRVerificationStage::Final);
    for (const auto &instr : block->instructions()) {
        REQUIRE(!mir::instruction_desc(instr.opcode()).has_flag(mir::MIF_Pseudo));
    }

    std::ostringstream output;
    mir::AsmPrinter printer(output);
    printer.print(module);
    const auto assembly = output.str();
    REQUIRE(assembly.find("\tvsetvli t0, a0, e32, m1, ta, ma") != std::string::npos);
    REQUIRE(assembly.find("\tvmset.m v1") != std::string::npos);
    REQUIRE(assembly.find("\tvmand.mm v0, v1, v1") != std::string::npos);
    REQUIRE(assembly.find("\tvadd.vv v8, v10, v11, v0.t") != std::string::npos);
}

void test_final_opcode_schema_rejects_malformed_relocations_and_avl() {
    mir::Module reloc_module("bad-final-reloc");
    auto *reloc_function = reloc_module.create_function("bad_reloc", void_type(), {});
    auto *reloc_block = reloc_function->create_block("entry");
    reloc_block->add_instr(
        mir::Opcode::RISCVAUIPC,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR,
                                     mir::ValueType::Ptr)),
         mir::MachineOperand::reloc(mir::RelocationKind::PCRelLo, "symbol")});
    reloc_function->layout_frame();
    require_verify_failure(reloc_module, mir::MIRVerificationStage::Final,
                           "malformed final AUIPC relocation");

    const auto type = mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                                       mir::VectorLMUL::M1);
    mir::Module avl_module("bad-final-avl");
    avl_module.target().has_vector = true;
    auto *avl_function = avl_module.create_function("bad_avl", void_type(), {});
    auto *avl_block = avl_function->create_block("entry");
    auto info = vector_info(type, mir::RVVOperation::SetVL,
                            mir::MachineVectorAVL::operand(1));
    avl_block->add_instr(mir::MachineInstr(
        mir::Opcode::RISCVVSetIVLI,
        {mir::MachineOperand::reg_def(
             mir::Register::physical("t0", mir::RegisterClass::GPR,
                                     mir::ValueType::I32)),
         mir::MachineOperand::imm(32),
         mir::MachineOperand::implicit_reg_def(state_reg("vl")),
         mir::MachineOperand::implicit_reg_def(state_reg("vtype"))},
        std::move(info)));
    avl_function->layout_frame();
    require_verify_failure(avl_module, mir::MIRVerificationStage::Final,
                           "unsigned 5-bit AVL immediate");
}

void test_rvv_compare_extract_and_mask_cleanup_fail_closed() {
    const auto type = mir::MachineVectorType::fixed(
        mir::ValueType::I32, 32, mir::VectorLMUL::M1, 4);
    const auto mask_type = mir::MachineVectorType::mask_for(type);

    mir::Module bad_compare("bad-rvv-compare-predicate");
    bad_compare.target().has_vector = true;
    auto *compare_function = bad_compare.create_function("compare", void_type(), {});
    auto *compare_block = compare_function->create_block("entry");
    compare_block->add_instr(make_setvli(type));
    auto compare_info = vector_info(type, mir::RVVOperation::Gt,
                                    mir::MachineVectorAVL::current_vl());
    compare_info.tail_policy = mir::VectorTailPolicy::Agnostic;
    compare_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
    compare_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVCompareVVTA,
        {mir::MachineOperand::reg_def(compare_function->regs().create_virtual_vector(
             mir::RegisterClass::VMASK, mask_type)),
         mir::MachineOperand::reg_use(compare_function->regs().create_virtual_vector(
             mir::RegisterClass::VR, type)),
         mir::MachineOperand::reg_use(compare_function->regs().create_virtual_vector(
             mir::RegisterClass::VR, type)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(compare_info)));
    compare_function->layout_frame();
    require_verify_failure(bad_compare, mir::MIRVerificationStage::PreRA,
                           "RVV operation is incompatible with the pseudo opcode");

    mir::Module bad_extract("bad-rvv-extract-index");
    bad_extract.target().has_vector = true;
    auto *extract_function = bad_extract.create_function("extract", void_type(), {});
    auto *extract_block = extract_function->create_block("entry");
    extract_block->add_instr(make_setvli(type));
    auto source = extract_function->regs().create_virtual_vector(
        mir::RegisterClass::VR, type);
    auto extract = make_extract(type, source);
    extract.operands()[2] = mir::MachineOperand::imm(1);
    extract_block->add_instr(std::move(extract));
    extract_function->layout_frame();
    require_verify_failure(bad_extract, mir::MIRVerificationStage::PreRA,
                           "RVV extract final family only supports lane zero");

    // N=32777 makes the last packed-mask byte offset 4097, outside simm12.
    // The cleanup must preserve the byte in t5 while t6 forms the address.
    const auto wide_data = mir::MachineVectorType::fixed(
        mir::ValueType::I32, 32, mir::VectorLMUL::M8, 32777);
    const auto wide_mask = mir::MachineVectorType::mask_for(wide_data);
    mir::Module cleanup("wide-mask-tail-cleanup");
    cleanup.target().has_vector = true;
    cleanup.target().minimum_vlen_bits = 262144;
    auto *cleanup_function = cleanup.create_function("cleanup", void_type(), {});
    auto *cleanup_block = cleanup_function->create_block("entry");
    cleanup_block->add_instr(make_setvli(wide_data));
    auto cleanup_info = vector_info(wide_data, mir::RVVOperation::Store,
                                    mir::MachineVectorAVL::current_vl());
    cleanup_block->add_instr(mir::MachineInstr(
        mir::Opcode::RVVMaskStore,
        {mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v1", mir::RegisterClass::VMASK, wide_mask)),
         mir::MachineOperand::reg_use(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::Ptr)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(cleanup_info)));
    cleanup_function->layout_frame();
    std::string error;
    if (!pass::expand_machine_pseudos(cleanup, error)) {
        throw Failure("wide mask cleanup expansion failed: " + error);
    }
    REQUIRE(error.empty());
    bool saw_address = false;
    bool saw_load = false;
    bool saw_store = false;
    for (const auto &instruction : cleanup_block->instructions()) {
        const auto &operands = instruction.operands();
        if (instruction.opcode() == mir::Opcode::Add && operands.size() == 3 &&
            operands[0].reg_value().name == "t6" &&
            operands[1].reg_value().name == "a0" &&
            operands[2].reg_value().name == "t6") {
            saw_address = true;
        }
        if (instruction.opcode() == mir::Opcode::RISCVLBU &&
            operands[0].reg_value().name == "t5" &&
            operands[1].reg_value().name == "t6" &&
            operands[2].int_value() == 0) {
            saw_load = true;
        }
        if (instruction.opcode() == mir::Opcode::RISCVSB &&
            operands[0].reg_value().name == "t5" &&
            operands[1].reg_value().name == "t6" &&
            operands[2].int_value() == 0) {
            saw_store = true;
        }
    }
    REQUIRE(saw_address && saw_load && saw_store);
}

void test_indexed_reduction_and_mask_query_fail_closed() {
    const auto data = mir::MachineVectorType::fixed(
        mir::ValueType::I32, 32, mir::VectorLMUL::M1, 4);
    const auto wrong_index = mir::MachineVectorType::fixed(
        mir::ValueType::I32, 32, mir::VectorLMUL::M2, 4);

    mir::Module indexed("bad-indexed-vector-type");
    indexed.target().has_vector = true;
    auto *indexed_function = indexed.create_function("indexed", void_type(), {});
    auto *indexed_block = indexed_function->create_block("entry");
    auto indexed_info = vector_info(data, mir::RVVOperation::Load,
                                    mir::MachineVectorAVL::current_vl());
    indexed_info.index_vector_type = wrong_index;
    indexed_block->add_instr(mir::MachineInstr(
        mir::Opcode::RISCVVLoadIndexedOrdered,
        {mir::MachineOperand::reg_def(indexed_function->regs().create_virtual_vector(
             mir::RegisterClass::VRNoV0, data)),
         mir::MachineOperand::reg_use(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::Ptr)),
         mir::MachineOperand::reg_use(indexed_function->regs().create_virtual_vector(
             mir::RegisterClass::VR, wrong_index)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(indexed_info)));
    indexed_function->layout_frame();
    require_verify_failure(
        indexed, mir::MIRVerificationStage::PreRA,
        "indexed RVV instruction requires an e32 index vector with data LMUL");

    const auto float_type = mir::MachineVectorType::fixed(
        mir::ValueType::F32, 32, mir::VectorLMUL::M1, 4);
    mir::Module float_reduction("bad-float-reduction-operation");
    float_reduction.target().has_vector = true;
    auto *reduction_function =
        float_reduction.create_function("reduction", void_type(), {});
    auto *reduction_block = reduction_function->create_block("entry");
    auto reduction_info = vector_info(
        float_type, mir::RVVOperation::ReduceMin,
        mir::MachineVectorAVL::current_vl());
    reduction_info.rounding = mir::VectorRoundingMode::Dynamic;
    reduction_block->add_instr(mir::MachineInstr(
        mir::Opcode::RISCVVReductionFloatOrdered,
        {mir::MachineOperand::reg_def(mir::Register::physical_vector(
             "v8", mir::RegisterClass::VRNoV0, float_type)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v10", mir::RegisterClass::VR, float_type)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v8", mir::RegisterClass::VRNoV0, float_type)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype")),
         mir::MachineOperand::implicit_reg_use(state_reg("frm"))},
        std::move(reduction_info)));
    reduction_function->layout_frame();
    require_verify_failure(float_reduction, mir::MIRVerificationStage::Final,
                           "RVV operation is incompatible with the pseudo opcode");

    const auto float_mask = mir::MachineVectorType::mask_for(float_type);
    mir::Module ordinary_v0("bad-vfirst-ordinary-v0");
    ordinary_v0.target().has_vector = true;
    auto *vfirst_function = ordinary_v0.create_function("vfirst", void_type(), {});
    auto *vfirst_block = vfirst_function->create_block("entry");
    auto set_info = vector_info(float_type, mir::RVVOperation::SetVL,
                                mir::MachineVectorAVL::operand(1));
    vfirst_block->add_instr(mir::MachineInstr(
        mir::Opcode::RISCVVSetIVLI,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "t0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::imm(4),
         mir::MachineOperand::implicit_reg_def(state_reg("vl")),
         mir::MachineOperand::implicit_reg_def(state_reg("vtype"))},
        std::move(set_info)));
    auto first_info = vector_info(float_type, mir::RVVOperation::MaskFirst,
                                  mir::MachineVectorAVL::current_vl());
    vfirst_block->add_instr(mir::MachineInstr(
        mir::Opcode::RISCVVMaskFirst,
        {mir::MachineOperand::reg_def(mir::Register::physical(
             "a0", mir::RegisterClass::GPR, mir::ValueType::I32)),
         mir::MachineOperand::reg_use(mir::Register::physical_vector(
             "v0", mir::RegisterClass::VMASK, float_mask)),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(first_info)));
    vfirst_function->layout_frame();
    require_verify_failure(ordinary_v0, mir::MIRVerificationStage::Final,
                           "ordinary mask value cannot occupy physical v0");

    auto &first_operand = vfirst_block->instructions().back().operands()[1];
    first_operand = mir::MachineOperand::reg_use(mir::Register::physical_vector(
        "v1", mir::RegisterClass::VMASK, float_mask));
    require_verify_ok(ordinary_v0, mir::MIRVerificationStage::Final);
}

void test_invalid_opcode_is_rejected() {
    try {
        (void)mir::instruction_desc(mir::Opcode::Count);
    } catch (const std::out_of_range &) {
        return;
    }
    throw Failure("expected invalid opcode to throw");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"every_opcode_has_a_descriptor", test_every_opcode_has_a_descriptor},
        {"control_and_memory_effects", test_control_and_memory_effects},
        {"entry_copy_descriptors_allow_implicit_argument_uses",
         test_entry_copy_descriptors_allow_implicit_argument_uses},
        {"machine_vector_type_and_register_identity",
         test_machine_vector_type_and_register_identity},
        {"rvv_descriptor_schema_and_effects", test_rvv_descriptor_schema_and_effects},
        {"positive_pre_ra_and_canonical_printer",
         test_positive_pre_ra_and_canonical_printer},
        {"positive_post_ra_vector_banks", test_positive_post_ra_vector_banks},
        {"vector_ra_groups_ties_mask_and_fractional",
         test_vector_ra_groups_ties_mask_and_fractional},
        {"vector_ra_call_clobber_spills_and_reloads",
         test_vector_ra_call_clobber_spills_and_reloads},
        {"vector_ra_fixed_shape_pressure_spills",
         test_vector_ra_fixed_shape_pressure_spills},
        {"m8_scavenge_scalar_liveness_and_final_whole_spills",
         test_m8_scavenge_scalar_liveness_and_final_whole_spills},
        {"scalable_frame_all_integral_lmul_layout_and_expansion",
         test_scalable_frame_all_integral_lmul_layout_and_expansion},
        {"call_frame_metadata_is_fail_closed",
         test_call_frame_metadata_is_fail_closed},
        {"incoming_stack_arg_fast_path_without_scalable_frame",
         test_incoming_stack_arg_fast_path_without_scalable_frame},
        {"incoming_stack_arg_accounts_for_aligned_scalable_frame",
         test_incoming_stack_arg_accounts_for_aligned_scalable_frame},
        {"vector_ra_fractional_whole_spill",
         test_vector_ra_fractional_whole_spill},
        {"fractional_and_mask_pressure_whole_spills",
         test_fractional_and_mask_pressure_whole_spills},
        {"vector_ra_mask_and_tie_relegalize_requests",
         test_vector_ra_mask_and_tie_relegalize_requests},
        {"fail_closed_vector_metadata_and_state", test_fail_closed_vector_metadata_and_state},
        {"post_ra_group_overlap_alignment_and_v0_rules",
         test_post_ra_group_overlap_alignment_and_v0_rules},
        {"scalable_slot_and_whole_register_fail_closed_rules",
         test_scalable_slot_and_whole_register_fail_closed_rules},
        {"post_ra_virtual_and_final_pseudo_rejection",
         test_post_ra_virtual_and_final_pseudo_rejection},
        {"asm_printer_rejects_unexpanded_rvv_pseudos",
         test_asm_printer_rejects_unexpanded_rvv_pseudos},
        {"scalar_load_immediate_expansion_is_width_correct",
         test_scalar_load_immediate_expansion_is_width_correct},
        {"pseudo_expansion_produces_final_rvv",
         test_pseudo_expansion_produces_final_rvv},
        {"final_opcode_schema_rejects_malformed_relocations_and_avl",
         test_final_opcode_schema_rejects_malformed_relocations_and_avl},
        {"rvv_compare_extract_and_mask_cleanup_fail_closed",
         test_rvv_compare_extract_and_mask_cleanup_fail_closed},
        {"indexed_reduction_and_mask_query_fail_closed",
         test_indexed_reduction_and_mask_query_fail_closed},
        {"invalid_opcode_is_rejected", test_invalid_opcode_is_rejected},
    };
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &ex) {
            std::cerr << "FAIL " << name << ": " << ex.what() << '\n';
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
    with tempfile.TemporaryDirectory(prefix="mir-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "mir_infra_tests.cpp"
        binary = tmp_dir / "mir_infra_tests"
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
                str(ROOT / "src/mir/MIR.cpp"),
                str(ROOT / "src/mir/MachineInstrDesc.cpp"),
                str(ROOT / "src/mir/MIRVerifier.cpp"),
                str(ROOT / "src/mir/MIRVectorState.cpp"),
                str(ROOT / "src/mir/MIRPrinter.cpp"),
                str(ROOT / "src/mir/AsmPrinter.cpp"),
                str(ROOT / "src/oir/OIR.cpp"),
                str(ROOT / "src/oir/OIRAnalysis.cpp"),
                str(ROOT / "src/oir/OIRDataLayout.cpp"),
                str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
                str(ROOT / "src/pass/PassManager.cpp"),
                str(ROOT / "src/pass/mir/MIRPseudoExpansionPass.cpp"),
                str(ROOT / "src/pass/mir/MIRVectorRegAlloc.cpp"),
                str(ROOT / "src/pass/mir/MIRRegAllocPass.cpp"),
                str(ROOT / "src/pass/mir/MIRVectorStatePass.cpp"),
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
