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
#include "mir/MIRVectorState.h"
#include "mir/MIRVerifier.h"
#include "pass/mir/MIRVectorStatePass.h"

#include <exception>
#include <functional>
#include <iostream>
#include <memory>
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

#define REQUIRE(condition) do { if (!(condition)) throw Failure(                 \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) +                  \
    ": requirement failed: " #condition); } while (false)

mir::TypeInfo void_type() {
    return {mir::ValueType::Void, "void", 0, 1, std::nullopt};
}

mir::Register gpr(const char *name = "zero") {
    return mir::Register::physical(name, mir::RegisterClass::GPR,
                                   mir::ValueType::I32);
}

mir::Register state_reg(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::VSTATE,
                                   mir::ValueType::Void);
}

mir::MachineVectorType vector_type() {
    return mir::MachineVectorType::scalable(mir::ValueType::I32, 32,
                                            mir::VectorLMUL::M1);
}

mir::Register vector_reg(const char *name = "v8") {
    return mir::Register::physical_vector(name, mir::RegisterClass::VRNoV0,
                                          vector_type());
}

mir::MachineInstr setvl(std::int64_t avl,
                        mir::VectorTailPolicy tail = mir::VectorTailPolicy::Agnostic,
                        mir::VectorMaskPolicy mask = mir::VectorMaskPolicy::Agnostic) {
    mir::MachineVectorInfo info(vector_type());
    info.operation = mir::RVVOperation::SetVL;
    info.avl = mir::MachineVectorAVL::operand(1);
    info.tail_policy = tail;
    info.mask_policy = mask;
    return mir::MachineInstr(
        mir::Opcode::RVVSetVLI,
        {mir::MachineOperand::reg_def(gpr()), mir::MachineOperand::imm(avl),
         mir::MachineOperand::implicit_reg_def(state_reg("vl")),
         mir::MachineOperand::implicit_reg_def(state_reg("vtype"))},
        std::move(info));
}

mir::MachineInstr policy_set(mir::VectorTailPolicy tail,
                             mir::VectorMaskPolicy mask) {
    mir::MachineVectorInfo info(vector_type());
    info.operation = mir::RVVOperation::SetVL;
    info.avl = mir::MachineVectorAVL::operand(1);
    info.tail_policy = tail;
    info.mask_policy = mask;
    return mir::MachineInstr(
        mir::Opcode::RVVSetVLI,
        {mir::MachineOperand::reg_def(gpr()), mir::MachineOperand::reg_use(gpr()),
         mir::MachineOperand::implicit_reg_def(state_reg("vl")),
         mir::MachineOperand::implicit_reg_def(state_reg("vtype"))},
        std::move(info));
}

mir::MachineInstr step(
    mir::VectorTailPolicy tail = mir::VectorTailPolicy::Agnostic,
    mir::VectorMaskPolicy mask = mir::VectorMaskPolicy::Agnostic,
    std::uint64_t expected_vl = 0) {
    mir::MachineVectorInfo info(vector_type());
    info.operation = mir::RVVOperation::Step;
    info.avl = mir::MachineVectorAVL::current_vl();
    info.tail_policy = tail;
    info.mask_policy = mask;
    info.vl_identity = expected_vl;
    return mir::MachineInstr(
        mir::Opcode::RVVStepTA,
        {mir::MachineOperand::reg_def(vector_reg()),
         mir::MachineOperand::implicit_reg_use(state_reg("vl")),
         mir::MachineOperand::implicit_reg_use(state_reg("vtype"))},
        std::move(info));
}

std::unique_ptr<mir::Module> module_with_function(const std::string &name,
                                                  mir::MachineFunction *&function) {
    auto module = std::make_unique<mir::Module>(name);
    module->target().has_vector = true;
    module->target().arch = "rv64gcv";
    module->target().minimum_vlen_bits = 128;
    function = module->create_function("kernel", void_type(), {});
    return module;
}

pass::PassResult run_state_pass(std::unique_ptr<mir::Module> module,
                                pass::MIRVectorStateMetrics *metrics = nullptr) {
    pass::PassContext context;
    context.set_machine_module(std::move(module));
    pass::MIRVectorStatePass pass;
    auto result = pass.run(context);
    if (metrics != nullptr) {
        const auto *stored = context.get_artifact<pass::MIRVectorStateMetrics>(
            pass::MIRVectorStatePass::kMetricsArtifactKey);
        if (stored != nullptr) *metrics = *stored;
    }
    if (result.success) {
        auto verify = mir::verify_module(*context.machine_module(),
                                         mir::MIRVerificationStage::PostRA);
        REQUIRE(verify.ok);
    }
    return result;
}

void test_cfg_propagates_exact_state() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("propagate", function);
    auto *entry = function->create_block("entry");
    auto *body = function->create_block("body");
    entry->add_instr(setvl(7));
    entry->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("body")});
    body->add_instr(step());
    function->rebuild_cfg();
    auto analysis = mir::analyze_mir_vector_state(*function, true);
    REQUIRE(analysis.ok);
    REQUIRE(analysis.block_in.at(body).fully_known());
}

void test_diamond_policy_join_inserts_one_set() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("diamond", function);
    auto *entry = function->create_block("entry");
    auto *left = function->create_block("left");
    auto *right = function->create_block("right");
    auto *join = function->create_block("join");
    entry->add_instr(setvl(7));
    entry->add_instr(mir::Opcode::BranchNonZero,
                     {mir::MachineOperand::reg_use(gpr("a0")),
                      mir::MachineOperand::block("left")});
    entry->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("right")});
    left->add_instr(policy_set(mir::VectorTailPolicy::Undisturbed,
                               mir::VectorMaskPolicy::Undisturbed));
    left->add_instr(mir::Opcode::Jump,
                    {mir::MachineOperand::block("join")});
    right->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("join")});
    join->add_instr(step(mir::VectorTailPolicy::Undisturbed,
                         mir::VectorMaskPolicy::Undisturbed));
    function->rebuild_cfg();
    pass::MIRVectorStateMetrics metrics;
    auto result = run_state_pass(std::move(module), &metrics);
    REQUIRE(result.success);
    REQUIRE(metrics.inserted_vset == 1);
}

void test_different_avl_join_fails_closed() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("different_avl", function);
    auto *entry = function->create_block("entry");
    auto *left = function->create_block("left");
    auto *right = function->create_block("right");
    auto *join = function->create_block("join");
    entry->add_instr(mir::Opcode::BranchNonZero,
                     {mir::MachineOperand::reg_use(gpr("a0")),
                      mir::MachineOperand::block("left")});
    entry->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("right")});
    left->add_instr(setvl(3));
    left->add_instr(mir::Opcode::Jump,
                    {mir::MachineOperand::block("join")});
    right->add_instr(setvl(4));
    right->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("join")});
    join->add_instr(step());
    function->rebuild_cfg();
    auto result = run_state_pass(std::move(module));
    REQUIRE(!result.success);
    REQUIRE(result.message.find("no exact reaching VL/VTYPE state") != std::string::npos);
}

void test_call_kills_state_and_descriptor_declares_clobbers() {
    const auto &descriptor = mir::instruction_desc(mir::Opcode::Call);
    REQUIRE(descriptor.implicitly_defines(mir::MVS_VL));
    REQUIRE(descriptor.implicitly_defines(mir::MVS_VTYPE));
    REQUIRE(descriptor.implicitly_defines(mir::MVS_VXRM));
    REQUIRE(descriptor.implicitly_defines(mir::MVS_VXSAT));
    REQUIRE(descriptor.implicitly_defines(mir::MVS_VSTART));
    mir::MachineInstr call(mir::Opcode::Call,
                           {mir::MachineOperand::symbol("callee")});
    REQUIRE(call.operands().size() == 6);

    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("call_kill", function);
    auto *entry = function->create_block("entry");
    entry->add_instr(setvl(7));
    entry->add_instr(std::move(call));
    entry->add_instr(step());
    function->note_call();
    function->rebuild_cfg();
    auto result = run_state_pass(std::move(module));
    REQUIRE(!result.success);
    REQUIRE(result.message.find("no exact reaching VL/VTYPE state") != std::string::npos);
}

void test_redundant_set_is_removed() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("redundant", function);
    auto *entry = function->create_block("entry");
    auto *body = function->create_block("body");
    entry->add_instr(setvl(7));
    entry->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("body")});
    body->add_instr(setvl(7));
    body->add_instr(step());
    function->rebuild_cfg();
    pass::MIRVectorStateMetrics metrics;
    auto result = run_state_pass(std::move(module), &metrics);
    REQUIRE(result.success);
    REQUIRE(metrics.removed_vset == 1);
}

void test_loop_backedge_meet() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("loop", function);
    auto *preheader = function->create_block("preheader");
    auto *header = function->create_block("header");
    auto *latch = function->create_block("latch");
    preheader->add_instr(setvl(7));
    preheader->add_instr(mir::Opcode::Jump,
                         {mir::MachineOperand::block("header")});
    header->add_instr(step());
    header->add_instr(mir::Opcode::Jump,
                      {mir::MachineOperand::block("latch")});
    latch->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("header")});
    function->rebuild_cfg();
    auto result = run_state_pass(std::move(module));
    REQUIRE(result.success);
}

void test_loop_fixed_point_is_independent_of_block_storage_order() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("loop_layout", function);
    auto *entry = function->create_block("entry");
    // Deliberately store the loop header before both of its predecessor
    // blocks.  A forward sweep therefore sees the backedge and the dedicated
    // entry edge as dataflow-bottom on its first iteration.
    auto *header = function->create_block("header");
    auto *late_entry = function->create_block("late_entry");
    auto *latch = function->create_block("latch");
    entry->add_instr(setvl(7));
    entry->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("late_entry")});
    header->add_instr(step());
    header->add_instr(mir::Opcode::Jump,
                      {mir::MachineOperand::block("latch")});
    late_entry->add_instr(mir::Opcode::Jump,
                          {mir::MachineOperand::block("header")});
    latch->add_instr(mir::Opcode::Jump,
                     {mir::MachineOperand::block("header")});
    function->rebuild_cfg();
    auto result = run_state_pass(std::move(module));
    REQUIRE(result.success);
}

void test_persisted_vl_identity_detects_wrong_reaching_set() {
    mir::MachineFunction *function = nullptr;
    auto module = module_with_function("identity", function);
    auto *entry = function->create_block("entry");
    auto established = setvl(7);
    established.vector_info().vl_identity = 41;
    entry->add_instr(std::move(established));
    entry->add_instr(step(mir::VectorTailPolicy::Agnostic,
                          mir::VectorMaskPolicy::Agnostic, 42));
    function->rebuild_cfg();
    auto result = run_state_pass(std::move(module));
    REQUIRE(!result.success);
    REQUIRE(result.message.find("VL identity") != std::string::npos);
}

} // namespace

int main() {
    const std::vector<std::pair<const char *, std::function<void()>>> tests = {
        {"cfg_propagates_exact_state", test_cfg_propagates_exact_state},
        {"diamond_policy_join_inserts_one_set", test_diamond_policy_join_inserts_one_set},
        {"different_avl_join_fails_closed", test_different_avl_join_fails_closed},
        {"call_kills_state_and_descriptor_declares_clobbers",
         test_call_kills_state_and_descriptor_declares_clobbers},
        {"redundant_set_is_removed", test_redundant_set_is_removed},
        {"loop_backedge_meet", test_loop_backedge_meet},
        {"loop_fixed_point_is_independent_of_block_storage_order",
         test_loop_fixed_point_is_independent_of_block_storage_order},
        {"persisted_vl_identity_detects_wrong_reaching_set",
         test_persisted_vl_identity_detects_wrong_reaching_set},
    };
    std::size_t passed = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &exception) {
            std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
        }
    }
    std::cout << passed << "/" << tests.size() << " passed\n";
    return passed == tests.size() ? 0 : 1;
}
"""


def main() -> int:
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
    if not cxx:
        print("error: no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="mir-vector-state-") as tmp:
        tmpdir = Path(tmp)
        source = tmpdir / "mir_vector_state_tests.cpp"
        binary = tmpdir / "mir_vector_state_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        command = [
            cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "include"), str(source),
            str(ROOT / "src/mir/MIR.cpp"),
            str(ROOT / "src/mir/MachineInstrDesc.cpp"),
            str(ROOT / "src/mir/MIRVectorState.cpp"),
            str(ROOT / "src/mir/MIRVerifier.cpp"),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRDataLayout.cpp"),
            str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
            str(ROOT / "src/pass/PassManager.cpp"),
            str(ROOT / "src/pass/mir/MIRVectorStatePass.cpp"),
            "-o", str(binary),
        ]
        compiled = subprocess.run(command, cwd=ROOT, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  check=False)
        if compiled.returncode:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        result = subprocess.run([str(binary)], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=False)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
