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
#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"

#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final : public std::exception {
    explicit TestFailure(std::string message) : message(std::move(message)) {}

    const char *what() const noexcept override {
        return message.c_str();
    }

    std::string message;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +        \
                              ": requirement failed: " #condition);                           \
        }                                                                                       \
    } while (false)

void require_verify(oir::Module &module) {
    std::string message;
    if (!module.verify(&message)) {
        throw TestFailure("expected verifier success, got: " + message);
    }
}

void require_verify_fails(oir::Module &module, const std::string &needle) {
    std::string message;
    if (module.verify(&message)) {
        throw TestFailure("expected verifier failure containing '" + needle + "'");
    }
    if (message.find(needle) == std::string::npos) {
        throw TestFailure("expected verifier failure containing '" + needle + "', got: " +
                          message);
    }
}

void test_scalar_fold_rejects_noncanonical_i32_payloads() {
    using OpID = oir::Instruction::OpID;
    const std::int64_t min_i32 = std::numeric_limits<std::int32_t>::min();
    const std::int64_t max_i32 = std::numeric_limits<std::int32_t>::max();
    const std::int64_t min_i64 = std::numeric_limits<std::int64_t>::min();
    const std::int64_t max_i64 = std::numeric_limits<std::int64_t>::max();
    const std::vector<OpID> integer_ops = {
        OpID::Add, OpID::Sub, OpID::Mul, OpID::And,
        OpID::Xor, OpID::SDiv, OpID::SRem,
    };

    for (auto op : integer_ops) {
        REQUIRE(!pass::oir_opt::fold_int_binary(op, min_i64, 1).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, max_i64, 1).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, 1, min_i64).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, 1, max_i64).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, min_i32 - 1, 1).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, max_i32 + 1, 1).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, 1, min_i32 - 1).has_value());
        REQUIRE(!pass::oir_opt::fold_int_binary(op, 1, max_i32 + 1).has_value());
    }
    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SDiv, min_i64, -1).has_value());
    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SRem, min_i64, -1).has_value());

    auto require_fold = [](OpID op, std::int64_t lhs, std::int64_t rhs,
                           std::int64_t expected) {
        auto folded = pass::oir_opt::fold_int_binary(op, lhs, rhs);
        REQUIRE(folded.has_value());
        REQUIRE(*folded == expected);
    };

    require_fold(OpID::Add, max_i32, 1, min_i32);
    require_fold(OpID::Sub, min_i32, 1, max_i32);
    require_fold(OpID::Mul, max_i32, 2, -2);
    require_fold(OpID::Mul, min_i32, -1, min_i32);
    require_fold(OpID::And, -1, 15, 15);
    require_fold(OpID::Xor, -1, 15, -16);
    require_fold(OpID::SDiv, 7, 2, 3);
    require_fold(OpID::SDiv, -7, 2, -3);
    require_fold(OpID::SDiv, 7, -1, -7);
    require_fold(OpID::SRem, -7, 2, -1);
    require_fold(OpID::SRem, 7, -1, 0);

    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SDiv, 7, 0).has_value());
    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SRem, 7, 0).has_value());
    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SDiv, min_i32, -1).has_value());
    REQUIRE(!pass::oir_opt::fold_int_binary(OpID::SRem, min_i32, -1).has_value());
}

oir::Function *create_function(oir::Module &module, const std::string &name,
                               oir::Type *return_type,
                               const std::vector<oir::Type *> &param_types = {}) {
    return module.create_function(name, module.types().func_ty(return_type, param_types));
}

void test_replace_all_uses_with_updates_use_lists() {
    oir::Module module("replace");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int32_ty()});
    auto *arg = function->args().front().get();
    arg->set_name("x");
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *one = builder.i32(1);
    auto *two = builder.i32(2);
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add, arg, one, "sum");
    auto *twice = builder.create_binary(oir::Instruction::OpID::Mul, sum, two, "twice");
    builder.create_ret(twice);

    require_verify(module);
    REQUIRE(sum->use_count() == 1);
    REQUIRE(twice->lhs() == sum);

    sum->replace_all_uses_with(arg);

    REQUIRE(sum->use_count() == 0);
    REQUIRE(twice->lhs() == arg);
    require_verify(module);
}

void test_erasing_instruction_drops_operand_uses() {
    oir::Module module("erase_inst");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int32_ty()});
    auto *arg = function->args().front().get();
    arg->set_name("x");
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *one = builder.i32(1);
    auto *dead = builder.create_binary(oir::Instruction::OpID::Add, arg, one, "dead");
    builder.create_ret(arg);

    require_verify(module);
    REQUIRE(arg->use_count() == 2);
    REQUIRE(one->use_count() == 1);

    for (auto it = entry->instructions().begin(); it != entry->instructions().end(); ++it) {
        if (it->get() == dead) {
            (*it)->drop_all_operands();
            entry->instructions().erase(it);
            break;
        }
    }

    REQUIRE(arg->use_count() == 1);
    REQUIRE(one->use_count() == 0);
    require_verify(module);
}

void test_phi_incoming_removal_reindexes_use_lists() {
    oir::Module module("phi_remove");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int1_ty()});
    auto *cond = function->args().front().get();
    cond->set_name("c");
    auto *entry = function->create_block("entry");
    auto *then_block = function->create_block("then");
    auto *else_block = function->create_block("else");
    auto *merge_block = function->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(cond, then_block, else_block);
    builder.set_insert_point(then_block);
    builder.create_br(merge_block);
    builder.set_insert_point(else_block);
    builder.create_br(merge_block);
    builder.set_insert_point(merge_block);
    auto *from_then = builder.i32(10);
    auto *from_else = builder.i32(20);
    auto *else_return = builder.i32(30);
    auto *phi = builder.create_phi(module.types().int32_ty(), "p");
    phi->add_incoming(from_then, then_block);
    phi->add_incoming(from_else, else_block);
    builder.create_ret(phi);

    require_verify(module);
    REQUIRE(from_else->use_count() == 1);

    oir::cfg::remove_edge(else_block, merge_block);
    else_block->terminator()->drop_all_operands();
    else_block->instructions().pop_back();
    builder.set_insert_point(else_block);
    builder.create_ret(else_return);

    REQUIRE(phi->incoming().size() == 1);
    REQUIRE(phi->operand_count() == 2);
    REQUIRE(from_else->use_count() == 0);
    require_verify(module);
}

void test_erasing_block_drops_instruction_operand_uses() {
    oir::Module module("erase_block");
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");
    auto *dead_block = function->create_block("dead");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *zero = builder.i32(0);
    builder.create_ret(zero);

    builder.set_insert_point(dead_block);
    auto *four = builder.i32(4);
    auto *five = builder.i32(5);
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add, four, five, "sum");
    builder.create_ret(sum);

    require_verify(module);
    REQUIRE(four->use_count() == 1);
    REQUIRE(five->use_count() == 1);

    function->erase_block(dead_block);

    REQUIRE(four->use_count() == 0);
    REQUIRE(five->use_count() == 0);
    require_verify(module);
}

class RawOperandInst final : public oir::Instruction {
  public:
    RawOperandInst(oir::Type *type, oir::Value *value, oir::BasicBlock *parent)
        : oir::Instruction(type, oir::Instruction::OpID::Alloca, parent, "bad") {
        add_operand(value);
    }

    void corrupt_by_clearing_operands_without_use_update() {
        operands_.clear();
    }

    std::string print() const override {
        return "%bad = alloca";
    }
};

void test_verifier_catches_stale_constant_use_list() {
    oir::Module module("stale_constant");
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *constant = builder.i32(42);
    auto bad = std::make_unique<RawOperandInst>(module.types().ptr_ty(module.types().int32_ty()),
                                                constant, entry);
    auto *raw_bad = bad.get();
    entry->append_instruction(std::move(bad));
    raw_bad->corrupt_by_clearing_operands_without_use_update();
    builder.create_ret(constant);

    require_verify_fails(module, "stale use-list");
}

void test_memory_ssa_skips_noalias_defs() {
    oir::Module module("memory_ssa_noalias");
    auto *g = module.create_global("g", module.types().int32_ty(), false);
    auto *h = module.create_global("h", module.types().int32_ty(), false);
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *store_g = builder.create_store(builder.i32(1), g);
    auto *store_h = builder.create_store(builder.i32(2), h);
    auto *load_g = builder.create_load(g, module.types().int32_ty(), "lg");
    builder.create_ret(load_g);

    require_verify(module);

    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    oir::MemorySSA memory_ssa(*function, aa, modref);

    auto *clobber = memory_ssa.clobbering_access(*load_g);
    REQUIRE(clobber != nullptr);
    REQUIRE(clobber->instruction() == store_g);
    REQUIRE(memory_ssa.access_for(store_h) != nullptr);
    REQUIRE(memory_ssa.access_for(store_h)->is_def());
}

void test_memory_ssa_uses_phi_at_cfg_join() {
    oir::Module module("memory_ssa_phi");
    auto *g = module.create_global("g", module.types().int32_ty(), false);
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int1_ty()});
    auto *cond = function->args().front().get();
    cond->set_name("cond");
    auto *entry = function->create_block("entry");
    auto *then_block = function->create_block("then");
    auto *else_block = function->create_block("else");
    auto *merge_block = function->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(cond, then_block, else_block);
    builder.set_insert_point(then_block);
    builder.create_store(builder.i32(1), g);
    builder.create_br(merge_block);
    builder.set_insert_point(else_block);
    builder.create_br(merge_block);
    builder.set_insert_point(merge_block);
    auto *load_g = builder.create_load(g, module.types().int32_ty(), "lg");
    builder.create_ret(load_g);

    require_verify(module);

    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    oir::MemorySSA memory_ssa(*function, aa, modref);

    auto *phi = memory_ssa.memory_phi(merge_block);
    REQUIRE(phi != nullptr);
    REQUIRE(phi->is_phi());
    REQUIRE(phi->incoming().size() == 2);
    REQUIRE(memory_ssa.clobbering_access(*load_g) == phi);
}

void test_modref_unknown_pointer_actual_fails_closed() {
    oir::Module module("modref_unknown_pointer");
    auto *i32 = module.types().int32_ty();
    auto *i1 = module.types().int1_ty();
    auto *i32_ptr = module.types().ptr_ty(i32);
    auto *reader = create_function(module, "reader", i32, {i32_ptr});
    auto *reader_entry = reader->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(reader_entry);
    auto *loaded = builder.create_load(reader->args().front().get(), i32, "loaded");
    builder.create_ret(loaded);

    auto *wrapper = create_function(module, "wrapper", i32, {i32_ptr});
    auto *wrapper_entry = wrapper->create_block("entry");
    builder.set_insert_point(wrapper_entry);
    auto *inner_formal_call =
        builder.create_call(reader, i32, {wrapper->args().front().get()}, "inner_read");
    builder.create_ret(inner_formal_call);

    auto *writer = create_function(module, "writer", i32, {i32_ptr});
    auto *writer_entry = writer->create_block("entry");
    builder.set_insert_point(writer_entry);
    builder.create_store(builder.i32(7), writer->args().front().get());
    builder.create_ret(builder.i32(0));

    auto *caller = create_function(module, "caller", i32);
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *known_storage = builder.create_alloca(i32, "known");
    auto *known_call = builder.create_call(wrapper, i32, {known_storage}, "known_read");
    auto *unknown_call = builder.create_call(wrapper, i32, {builder.undef(i32_ptr)},
                                             "unknown_read");
    auto *writer_local_call =
        builder.create_call(writer, i32, {known_storage}, "local_write");
    auto *sum =
        builder.create_binary(oir::Instruction::OpID::Add, known_call, unknown_call, "sum");
    auto *sum_with_write = builder.create_binary(oir::Instruction::OpID::Add, sum,
                                                 writer_local_call, "sum_with_write");
    builder.create_ret(sum_with_write);

    auto *mixed = create_function(module, "mixed", i32, {i1});
    auto *mixed_entry = mixed->create_block("entry");
    auto *mixed_known = mixed->create_block("known");
    auto *mixed_unknown = mixed->create_block("unknown");
    auto *mixed_merge = mixed->create_block("merge");
    builder.set_insert_point(mixed_entry);
    auto *mixed_storage = builder.create_alloca(i32, "mixed_storage");
    builder.create_cond_br(mixed->args().front().get(), mixed_known, mixed_unknown);
    builder.set_insert_point(mixed_known);
    builder.create_br(mixed_merge);
    builder.set_insert_point(mixed_unknown);
    builder.create_br(mixed_merge);
    builder.set_insert_point(mixed_merge);
    auto *mixed_ptr = builder.create_phi(i32_ptr, "mixed_ptr");
    mixed_ptr->add_incoming(mixed_storage, mixed_known);
    mixed_ptr->add_incoming(builder.undef(i32_ptr), mixed_unknown);
    auto *mixed_call = builder.create_call(reader, i32, {mixed_ptr}, "mixed_read");
    builder.create_ret(mixed_call);

    auto *pure_cycle = create_function(module, "pure_cycle", i32, {i1});
    auto *cycle_header = pure_cycle->create_block("header");
    auto *cycle_exit = pure_cycle->create_block("exit");
    builder.set_insert_point(cycle_header);
    auto *cycle_ptr = builder.create_phi(i32_ptr, "cycle_ptr");
    cycle_ptr->add_incoming(cycle_ptr, cycle_header);
    auto *cycle_call = builder.create_call(reader, i32, {cycle_ptr}, "cycle_read");
    builder.create_cond_br(pure_cycle->args().front().get(), cycle_header, cycle_exit);
    builder.set_insert_point(cycle_exit);
    builder.create_ret(builder.i32(0));

    require_verify(module);
    oir::FunctionModRefAnalysis modref(module);
    const auto &reader_summary = modref.summary(reader);
    REQUIRE(!reader_summary.has_side_effect);
    REQUIRE(reader_summary.read_param_indices.count(0) == 1);
    const auto &wrapper_summary = modref.summary(wrapper);
    REQUIRE(!wrapper_summary.has_side_effect);
    REQUIRE(wrapper_summary.read_param_indices.count(0) == 1);
    REQUIRE(modref.call_has_side_effect(*inner_formal_call));
    REQUIRE(!modref.call_has_side_effect(*known_call));
    REQUIRE(modref.call_has_side_effect(*unknown_call));
    REQUIRE(modref.call_has_side_effect(*writer_local_call));
    REQUIRE(modref.call_has_side_effect(*mixed_call));
    REQUIRE(modref.call_has_side_effect(*cycle_call));
}

void test_modref_deep_reverse_chain_reaches_fixed_point() {
    oir::Module module("modref_deep_reverse_chain");
    auto *i32 = module.types().int32_ty();
    auto *i32_ptr = module.types().ptr_ty(i32);
    auto *void_type = module.types().void_ty();
    auto *putint = module.create_function(
        "putint", module.types().func_ty(void_type, {i32}), true);

    constexpr std::size_t chain_length = 96;
    std::vector<oir::Function *> chain;
    chain.reserve(chain_length);
    for (std::size_t index = 0; index < chain_length; ++index) {
        chain.push_back(create_function(module, "chain_" + std::to_string(index), i32,
                                        {i32_ptr}));
    }
    auto *cycle_a = create_function(module, "cycle_a", i32, {i32_ptr});
    auto *cycle_b = create_function(module, "cycle_b", i32, {i32_ptr});

    oir::IRBuilder builder(&module);
    for (std::size_t index = 0; index < chain_length; ++index) {
        auto *entry = chain[index]->create_block("entry");
        builder.set_insert_point(entry);
        auto *callee = index + 1 < chain_length ? chain[index + 1] : cycle_a;
        auto *forwarded = builder.create_call(
            callee, i32, {chain[index]->args().front().get()}, "forwarded");
        builder.create_ret(forwarded);
    }

    auto *cycle_a_entry = cycle_a->create_block("entry");
    builder.set_insert_point(cycle_a_entry);
    auto *from_b =
        builder.create_call(cycle_b, i32, {cycle_a->args().front().get()}, "from_b");
    builder.create_ret(from_b);

    auto *cycle_b_entry = cycle_b->create_block("entry");
    builder.set_insert_point(cycle_b_entry);
    auto *loaded = builder.create_load(cycle_b->args().front().get(), i32, "loaded");
    builder.create_store(loaded, cycle_b->args().front().get());
    builder.create_call(putint, void_type, {loaded});
    auto *from_a =
        builder.create_call(cycle_a, i32, {cycle_b->args().front().get()}, "from_a");
    builder.create_ret(from_a);

    require_verify(module);
    oir::FunctionModRefAnalysis modref(module);
    const auto &top_summary = modref.summary(chain.front());
    REQUIRE(top_summary.has_side_effect);
    REQUIRE(top_summary.read_param_indices.count(0) == 1);
    REQUIRE(top_summary.written_param_indices.count(0) == 1);
    REQUIRE(top_summary.may_not_return);
    REQUIRE(modref.summary(cycle_a).may_not_return);
    REQUIRE(modref.summary(cycle_b).may_not_return);
}

void test_modref_termination_fails_closed_and_proves_canonical_loops() {
    oir::Module module("modref_termination");
    auto *i32 = module.types().int32_ty();
    auto *i1 = module.types().int1_ty();

    oir::IRBuilder builder(&module);

    // Rotated latch-tested fixed countdown: {32,+,-1}, testing next != 0.
    auto *fixed = create_function(module, "fixed_countdown", i32);
    auto *fixed_entry = fixed->create_block("entry");
    auto *fixed_header = fixed->create_block("header");
    auto *fixed_exit = fixed->create_block("exit");
    builder.set_insert_point(fixed_entry);
    builder.create_br(fixed_header);
    builder.set_insert_point(fixed_header);
    auto *fixed_phi = builder.create_phi(i32, "remaining");
    fixed_phi->add_incoming(builder.i32(32), fixed_entry);
    auto *fixed_next = builder.create_binary(oir::Instruction::OpID::Sub, fixed_phi,
                                             builder.i32(1), "remaining.next");
    auto *fixed_more = builder.create_icmp(oir::CmpPred::NE, fixed_next, builder.i32(0),
                                           "remaining.more");
    builder.create_cond_br(fixed_more, fixed_header, fixed_exit);
    fixed_phi->add_incoming(fixed_next, fixed_header);
    builder.set_insert_point(fixed_exit);
    builder.create_ret(builder.i32(0));

    auto *fixed_eight = create_function(module, "fixed_countdown_eight", i32);
    auto *fixed_eight_entry = fixed_eight->create_block("entry");
    auto *fixed_eight_header = fixed_eight->create_block("header");
    auto *fixed_eight_exit = fixed_eight->create_block("exit");
    builder.set_insert_point(fixed_eight_entry);
    builder.create_br(fixed_eight_header);
    builder.set_insert_point(fixed_eight_header);
    auto *fixed_eight_phi = builder.create_phi(i32, "remaining");
    fixed_eight_phi->add_incoming(builder.i32(8), fixed_eight_entry);
    auto *fixed_eight_next = builder.create_binary(
        oir::Instruction::OpID::Sub, fixed_eight_phi, builder.i32(1), "remaining.next");
    auto *fixed_eight_more = builder.create_icmp(
        oir::CmpPred::NE, fixed_eight_next, builder.i32(0), "remaining.more");
    builder.create_cond_br(fixed_eight_more, fixed_eight_header, fixed_eight_exit);
    fixed_eight_phi->add_incoming(fixed_eight_next, fixed_eight_header);
    builder.set_insert_point(fixed_eight_exit);
    builder.create_ret(builder.i32(0));

    // Header-tested symbolic bound: {0,+,1} < arbitrary invariant i32 limit.
    auto *symbolic = create_function(module, "symbolic_lt", i32, {i32});
    auto *symbolic_entry = symbolic->create_block("entry");
    auto *symbolic_header = symbolic->create_block("header");
    auto *symbolic_body = symbolic->create_block("body");
    auto *symbolic_exit = symbolic->create_block("exit");
    builder.set_insert_point(symbolic_entry);
    builder.create_br(symbolic_header);
    builder.set_insert_point(symbolic_header);
    auto *symbolic_phi = builder.create_phi(i32, "index");
    symbolic_phi->add_incoming(builder.i32(0), symbolic_entry);
    auto *symbolic_more = builder.create_icmp(oir::CmpPred::LT, symbolic_phi,
                                              symbolic->args().front().get(), "index.more");
    builder.create_cond_br(symbolic_more, symbolic_body, symbolic_exit);
    builder.set_insert_point(symbolic_body);
    builder.create_call(fixed, i32, {}, "fixed");
    builder.create_call(fixed_eight, i32, {}, "fixed_eight");
    auto *symbolic_next = builder.create_binary(oir::Instruction::OpID::Add, symbolic_phi,
                                                builder.i32(1), "index.next");
    builder.create_br(symbolic_header);
    symbolic_phi->add_incoming(symbolic_next, symbolic_body);
    builder.set_insert_point(symbolic_exit);
    builder.create_ret(builder.i32(0));

    // Symmetric header-tested form: {0,+,-1} > arbitrary invariant i32 limit.
    auto *symbolic_gt = create_function(module, "symbolic_gt", i32, {i32});
    auto *symbolic_gt_entry = symbolic_gt->create_block("entry");
    auto *symbolic_gt_header = symbolic_gt->create_block("header");
    auto *symbolic_gt_body = symbolic_gt->create_block("body");
    auto *symbolic_gt_exit = symbolic_gt->create_block("exit");
    builder.set_insert_point(symbolic_gt_entry);
    builder.create_br(symbolic_gt_header);
    builder.set_insert_point(symbolic_gt_header);
    auto *symbolic_gt_phi = builder.create_phi(i32, "index");
    symbolic_gt_phi->add_incoming(builder.i32(0), symbolic_gt_entry);
    auto *symbolic_gt_more = builder.create_icmp(
        oir::CmpPred::GT, symbolic_gt_phi, symbolic_gt->args().front().get(), "index.more");
    builder.create_cond_br(symbolic_gt_more, symbolic_gt_body, symbolic_gt_exit);
    builder.set_insert_point(symbolic_gt_body);
    auto *symbolic_gt_next = builder.create_binary(
        oir::Instruction::OpID::Sub, symbolic_gt_phi, builder.i32(1), "index.next");
    builder.create_br(symbolic_gt_header);
    symbolic_gt_phi->add_incoming(symbolic_gt_next, symbolic_gt_body);
    builder.set_insert_point(symbolic_gt_exit);
    builder.create_ret(builder.i32(0));

    // Same LT/+1 proof through both normalizations: the false edge continues,
    // and the recurrence is the comparison's right operand (limit <= index).
    auto *normalized_lt = create_function(module, "normalized_lt", i32, {i32});
    auto *normalized_entry = normalized_lt->create_block("entry");
    auto *normalized_header = normalized_lt->create_block("header");
    auto *normalized_body = normalized_lt->create_block("body");
    auto *normalized_exit = normalized_lt->create_block("exit");
    builder.set_insert_point(normalized_entry);
    builder.create_br(normalized_header);
    builder.set_insert_point(normalized_header);
    auto *normalized_phi = builder.create_phi(i32, "index");
    normalized_phi->add_incoming(builder.i32(0), normalized_entry);
    auto *normalized_done = builder.create_icmp(
        oir::CmpPred::LE, normalized_lt->args().front().get(), normalized_phi, "done");
    builder.create_cond_br(normalized_done, normalized_exit, normalized_body);
    builder.set_insert_point(normalized_body);
    auto *normalized_next = builder.create_binary(
        oir::Instruction::OpID::Add, normalized_phi, builder.i32(1), "index.next");
    builder.create_br(normalized_header);
    normalized_phi->add_incoming(normalized_next, normalized_body);
    builder.set_insert_point(normalized_exit);
    builder.create_ret(builder.i32(0));

    auto *known_caller = create_function(module, "known_caller", i32, {i32});
    auto *known_entry = known_caller->create_block("entry");
    builder.set_insert_point(known_entry);
    auto *known_call = builder.create_call(symbolic, i32,
                                           {known_caller->args().front().get()}, "known");
    builder.create_ret(builder.i32(0));

    // Non-strict symbolic progress is unsafe at INT_MAX.
    auto *non_strict = create_function(module, "non_strict", i32, {i32});
    auto *le_entry = non_strict->create_block("entry");
    auto *le_header = non_strict->create_block("header");
    auto *le_body = non_strict->create_block("body");
    auto *le_exit = non_strict->create_block("exit");
    builder.set_insert_point(le_entry);
    builder.create_br(le_header);
    builder.set_insert_point(le_header);
    auto *le_phi = builder.create_phi(i32, "index");
    le_phi->add_incoming(builder.i32(0), le_entry);
    auto *le_more = builder.create_icmp(oir::CmpPred::LE, le_phi,
                                        non_strict->args().front().get(), "index.more");
    builder.create_cond_br(le_more, le_body, le_exit);
    builder.set_insert_point(le_body);
    auto *le_next = builder.create_binary(oir::Instruction::OpID::Add, le_phi, builder.i32(1),
                                          "index.next");
    builder.create_br(le_header);
    le_phi->add_incoming(le_next, le_body);
    builder.set_insert_point(le_exit);
    builder.create_ret(builder.i32(0));

    // A +2 recurrence reaches the mathematical exit only after signed wrap.
    auto *wrap = create_function(module, "wrap", i32);
    auto *wrap_entry = wrap->create_block("entry");
    auto *wrap_header = wrap->create_block("header");
    auto *wrap_body = wrap->create_block("body");
    auto *wrap_exit = wrap->create_block("exit");
    builder.set_insert_point(wrap_entry);
    builder.create_br(wrap_header);
    builder.set_insert_point(wrap_header);
    auto *wrap_phi = builder.create_phi(i32, "index");
    wrap_phi->add_incoming(builder.i32(2147483646), wrap_entry);
    auto *wrap_more = builder.create_icmp(oir::CmpPred::LT, wrap_phi,
                                          builder.i32(2147483647), "index.more");
    builder.create_cond_br(wrap_more, wrap_body, wrap_exit);
    builder.set_insert_point(wrap_body);
    auto *wrap_next = builder.create_binary(oir::Instruction::OpID::Add, wrap_phi,
                                            builder.i32(2), "index.next");
    builder.create_br(wrap_header);
    wrap_phi->add_incoming(wrap_next, wrap_body);
    builder.set_insert_point(wrap_exit);
    builder.create_ret(builder.i32(0));

    // The verifier currently permits an out-of-range ConstantInt payload for
    // i32.  Step matching must reject INT64_MIN without host negation overflow.
    auto *host_overflow_step = create_function(module, "host_overflow_step", i32);
    auto *host_overflow_entry = host_overflow_step->create_block("entry");
    auto *host_overflow_header = host_overflow_step->create_block("header");
    auto *host_overflow_body = host_overflow_step->create_block("body");
    auto *host_overflow_exit = host_overflow_step->create_block("exit");
    builder.set_insert_point(host_overflow_entry);
    builder.create_br(host_overflow_header);
    builder.set_insert_point(host_overflow_header);
    auto *host_overflow_phi = builder.create_phi(i32, "index");
    host_overflow_phi->add_incoming(builder.i32(0), host_overflow_entry);
    auto *host_overflow_more = builder.create_icmp(
        oir::CmpPred::LT, host_overflow_phi, builder.i32(1), "index.more");
    builder.create_cond_br(host_overflow_more, host_overflow_body, host_overflow_exit);
    builder.set_insert_point(host_overflow_body);
    auto *host_overflow_next = builder.create_binary(
        oir::Instruction::OpID::Sub, host_overflow_phi,
        builder.i32(-9223372036854775807LL - 1), "index.next");
    builder.create_br(host_overflow_header);
    host_overflow_phi->add_incoming(host_overflow_next, host_overflow_body);
    builder.set_insert_point(host_overflow_exit);
    builder.create_ret(builder.i32(0));

    // Direct and mutual call-graph SCCs never bootstrap must-return from self.
    auto *recursive = create_function(module, "recursive", i32, {i1});
    auto *recursive_entry = recursive->create_block("entry");
    auto *recursive_base = recursive->create_block("base");
    auto *recursive_step = recursive->create_block("step");
    builder.set_insert_point(recursive_entry);
    builder.create_cond_br(recursive->args().front().get(), recursive_step, recursive_base);
    builder.set_insert_point(recursive_base);
    builder.create_ret(builder.i32(0));
    builder.set_insert_point(recursive_step);
    auto *recursive_call = builder.create_call(recursive, i32,
                                               {recursive->args().front().get()}, "again");
    builder.create_ret(recursive_call);

    auto *mutual_a = create_function(module, "mutual_a", i32);
    auto *mutual_b = create_function(module, "mutual_b", i32);
    auto *mutual_a_entry = mutual_a->create_block("entry");
    builder.set_insert_point(mutual_a_entry);
    builder.create_ret(builder.create_call(mutual_b, i32, {}, "from_b"));
    auto *mutual_b_entry = mutual_b->create_block("entry");
    builder.set_insert_point(mutual_b_entry);
    builder.create_ret(builder.create_call(mutual_a, i32, {}, "from_a"));

    // Unknown externals and irreducible reachable cycles fail closed.
    auto *unknown_external = module.create_function(
        "unknown_external", module.types().func_ty(i32, {}), true);
    auto *external_caller = create_function(module, "external_caller", i32);
    auto *external_entry = external_caller->create_block("entry");
    builder.set_insert_point(external_entry);
    builder.create_ret(builder.create_call(unknown_external, i32, {}, "unknown"));

    auto *irreducible = create_function(module, "irreducible", i32, {i1, i1});
    auto *irreducible_entry = irreducible->create_block("entry");
    auto *irreducible_a = irreducible->create_block("a");
    auto *irreducible_b = irreducible->create_block("b");
    auto *irreducible_exit = irreducible->create_block("exit");
    builder.set_insert_point(irreducible_entry);
    builder.create_cond_br(irreducible->args()[0].get(), irreducible_a, irreducible_b);
    builder.set_insert_point(irreducible_a);
    builder.create_cond_br(irreducible->args()[1].get(), irreducible_b, irreducible_exit);
    builder.set_insert_point(irreducible_b);
    builder.create_cond_br(irreducible->args()[1].get(), irreducible_a, irreducible_exit);
    builder.set_insert_point(irreducible_exit);
    builder.create_ret(builder.i32(0));

    require_verify(module);
    oir::FunctionModRefAnalysis modref(module);
    REQUIRE(!modref.summary(fixed).may_not_return);
    REQUIRE(!modref.summary(fixed_eight).may_not_return);
    REQUIRE(!modref.summary(symbolic).may_not_return);
    REQUIRE(!modref.summary(symbolic_gt).may_not_return);
    REQUIRE(!modref.summary(normalized_lt).may_not_return);
    REQUIRE(!modref.summary(known_caller).may_not_return);
    REQUIRE(!modref.call_has_side_effect(*known_call));
    REQUIRE(modref.summary(non_strict).may_not_return);
    REQUIRE(modref.summary(wrap).may_not_return);
    REQUIRE(modref.summary(host_overflow_step).may_not_return);
    REQUIRE(modref.summary(recursive).may_not_return);
    REQUIRE(modref.summary(mutual_a).may_not_return);
    REQUIRE(modref.summary(mutual_b).may_not_return);
    REQUIRE(modref.summary(external_caller).may_not_return);
    REQUIRE(modref.summary(irreducible).may_not_return);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"replace_all_uses_with_updates_use_lists", test_replace_all_uses_with_updates_use_lists},
        {"erasing_instruction_drops_operand_uses", test_erasing_instruction_drops_operand_uses},
        {"phi_incoming_removal_reindexes_use_lists",
         test_phi_incoming_removal_reindexes_use_lists},
        {"erasing_block_drops_instruction_operand_uses",
         test_erasing_block_drops_instruction_operand_uses},
        {"verifier_catches_stale_constant_use_list",
         test_verifier_catches_stale_constant_use_list},
        {"scalar_fold_rejects_noncanonical_i32_payloads",
         test_scalar_fold_rejects_noncanonical_i32_payloads},
        {"memory_ssa_skips_noalias_defs", test_memory_ssa_skips_noalias_defs},
        {"memory_ssa_uses_phi_at_cfg_join", test_memory_ssa_uses_phi_at_cfg_join},
        {"modref_unknown_pointer_actual_fails_closed",
         test_modref_unknown_pointer_actual_fails_closed},
        {"modref_deep_reverse_chain_reaches_fixed_point",
         test_modref_deep_reverse_chain_reaches_fixed_point},
        {"modref_termination_fails_closed_and_proves_canonical_loops",
         test_modref_termination_fails_closed_and_proves_canonical_loops},
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
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
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

    with tempfile.TemporaryDirectory(prefix="oir-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "oir_infra_tests.cpp"
        binary = tmp_dir / "oir_infra_tests"
        source.write_text(textwrap.dedent(SOURCE))

        compile_cmd = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-I",
            str(ROOT / "include"),
            str(source),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRCFGUtils.cpp"),
            str(ROOT / "src/pass/oir/OIRScalarOptUtils.cpp"),
            "-o",
            str(binary),
        ]
        compile_proc = subprocess.run(
            compile_cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compile_proc.returncode != 0:
            if compile_proc.stdout:
                print(compile_proc.stdout, end="")
            if compile_proc.stderr:
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
        if run_proc.stdout:
            print(run_proc.stdout, end="")
        if run_proc.stderr:
            print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
