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
#include "oir/OIRParser.h"
#include "pass/oir/OIRVectorization.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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

using pass::oir_vectorize::LoopVectorizer;
using pass::oir_vectorize::LoopVectorizerOptions;
using pass::oir_vectorize::RemarkCode;
using pass::oir_vectorize::RemarkLog;

target::TargetProfile rvv_profile() {
    target::TargetProfile profile;
    profile.march = "rv64gcv";
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    REQUIRE(profile.has_vector());
    return profile;
}

target::TargetProfile scalar_profile() {
    target::TargetProfile profile;
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    REQUIRE(!profile.has_vector());
    return profile;
}

void require_verified(oir::Module &module) {
    std::string error;
    if (!module.verify(&error)) {
        throw std::runtime_error("verification failed: " + error + "\n" + module.print());
    }
}

void require_roundtrip(oir::Module &module) {
    require_verified(module);
    const auto first = module.print();
    auto parsed = oir::OIRParser::parse(first, "loop-vectorized.oir");
    if (!parsed.ok()) {
        const auto &error = parsed.errors.front();
        throw std::runtime_error("roundtrip parse failed at " +
                                 std::to_string(error.range.begin.line) + ":" +
                                 std::to_string(error.range.begin.column) + ": " +
                                 error.message + "\n" + first);
    }
    REQUIRE(parsed.module->print() == first);
}

oir::Value *incoming_for(const oir::PhiInst &phi, const oir::BasicBlock *block) {
    for (const auto &[value, from] : phi.incoming()) {
        if (from == block) {
            return value;
        }
    }
    return nullptr;
}

struct IntLoopOptions final {
    bool unknown_trip = true;
    bool pointer_arguments = false;
    bool unsafe_division = false;
    bool call = false;
    bool reduction = false;
    bool stride_two = false;
    bool early_exit = false;
    bool pointer_induction = false;
    int induction_stride = 1;
    bool same_object = false;
    int store_offset = 0;
    bool exit_phi = false;
    bool guarded_preheader = false;
};

struct IntLoopFixture final {
    explicit IntLoopFixture(IntLoopOptions options = {}) : options(options) {
        auto &types = module.types();
        std::vector<oir::Type *> params;
        if (options.pointer_arguments) {
            params.push_back(types.ptr_ty(types.int32_ty()));
            params.push_back(types.ptr_ty(types.int32_ty()));
        }
        if (options.unknown_trip) {
            params.push_back(types.int32_ty());
        }
        oir::Type *return_type =
            options.exit_phi ? static_cast<oir::Type *>(types.int32_ty()) : types.void_ty();
        function = module.create_function("kernel", types.func_ty(return_type, params));
        std::size_t argument = 0;
        oir::Value *base_a = nullptr;
        oir::Value *base_b = nullptr;
        if (options.pointer_arguments) {
            base_a = function->args()[argument++].get();
            base_b = function->args()[argument++].get();
            base_a->set_name("a");
            base_b->set_name("b");
        } else {
            base_a = module.create_global("a", types.array_ty(types.int32_ty(), 4096), false);
            base_b = options.same_object
                         ? base_a
                         : static_cast<oir::Value *>(module.create_global(
                               "b", types.array_ty(types.int32_ty(), 4096), false));
        }
        oir::Value *trip = nullptr;
        if (options.unknown_trip) {
            trip = function->args()[argument++].get();
            trip->set_name("n");
        }

        entry = function->create_block("entry");
        header = function->create_block("loop.header");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        if (trip == nullptr) {
            trip = builder.i32(257);
        }

        oir::PhiInst *pointer_phi = nullptr;
        oir::Value *initial_pointer = nullptr;
        oir::Value *initial_iv = builder.i32(0);
        if (options.induction_stride < 0) {
            initial_iv = builder.create_binary(oir::Instruction::OpID::Sub, trip,
                                               builder.i32(1), "iv.start");
        }
        if (options.pointer_induction) {
            if (options.pointer_arguments) {
                initial_pointer = options.induction_stride < 0
                                      ? static_cast<oir::Value *>(builder.create_gep(
                                            base_a, types.ptr_ty(types.int32_ty()),
                                            {initial_iv}, "ptr.start"))
                                      : base_a;
            } else {
                initial_pointer = builder.create_gep(
                    base_a, types.ptr_ty(types.int32_ty()),
                    {builder.i32(0), initial_iv}, "ptr.start");
            }
        }
        if (options.guarded_preheader) {
            auto *has_trip = builder.create_icmp(oir::CmpPred::GT, trip, builder.i32(0),
                                                  "outer.has.trip");
            builder.create_cond_br(has_trip, header, exit);
        } else {
            builder.create_br(header);
        }

        builder.set_insert_point(header);
        iv = builder.create_phi(types.int32_ty(), "iv");
        oir::PhiInst *sum = nullptr;
        if (options.reduction) {
            sum = builder.create_phi(types.int32_ty(), "sum");
        }
        if (options.pointer_induction) {
            pointer_phi = builder.create_phi(types.ptr_ty(types.int32_ty()), "ptr.iv");
        }
        auto *condition = options.induction_stride > 0
                              ? builder.create_icmp(oir::CmpPred::LT, iv, trip,
                                                    "loop.cond")
                              : builder.create_icmp(oir::CmpPred::GE, iv,
                                                    builder.i32(0), "loop.cond");
        builder.create_cond_br(condition, body, exit);

        builder.set_insert_point(body);
        oir::Value *lane_index = iv;
        if (options.stride_two) {
            lane_index = builder.create_binary(oir::Instruction::OpID::Mul, iv,
                                               builder.i32(2), "stride.index");
        }
        oir::Value *store_index = lane_index;
        if (options.store_offset != 0) {
            store_index = builder.create_binary(
                oir::Instruction::OpID::Add, lane_index,
                builder.i32(options.store_offset), "store.index");
        }
        std::vector<oir::Value *> indices;
        std::vector<oir::Value *> store_indices;
        if (options.pointer_arguments) {
            indices = {lane_index};
            store_indices = {store_index};
        } else {
            indices = {builder.i32(0), lane_index};
            store_indices = {builder.i32(0), store_index};
        }
        auto *pa = options.pointer_induction
                       ? static_cast<oir::Value *>(pointer_phi)
                       : static_cast<oir::Value *>(builder.create_gep(
                             base_a, types.ptr_ty(types.int32_ty()), indices, "pa"));
        auto *pb = builder.create_gep(base_b, types.ptr_ty(types.int32_ty()),
                                      store_indices, "pb");
        auto *loaded = builder.create_load(pa, types.int32_ty(), "loaded");
        oir::Value *computed = builder.create_binary(oir::Instruction::OpID::Add, loaded,
                                                     builder.i32(7), "computed");
        if (options.unsafe_division) {
            computed = builder.create_binary(oir::Instruction::OpID::SDiv, computed, iv,
                                             "unsafe.div");
        }
        if (options.call) {
            auto *callee = module.create_function(
                "opaque", types.func_ty(types.void_ty(), {}), true);
            builder.create_call(callee, types.void_ty(), {});
        }
        oir::Value *reduction_next = nullptr;
        if (sum != nullptr) {
            reduction_next = builder.create_binary(oir::Instruction::OpID::Add, sum,
                                                   computed, "sum.next");
            computed = reduction_next;
        }
        builder.create_store(computed, pb);
        auto *next = options.induction_stride > 0
                         ? builder.create_binary(oir::Instruction::OpID::Add, iv,
                                                 builder.i32(options.induction_stride),
                                                 "iv.next")
                         : builder.create_binary(oir::Instruction::OpID::Sub, iv,
                                                 builder.i32(-options.induction_stride),
                                                 "iv.next");
        oir::GetElementPtrInst *pointer_next = nullptr;
        if (pointer_phi != nullptr) {
            pointer_next = builder.create_gep(pointer_phi, types.ptr_ty(types.int32_ty()),
                                              {builder.i32(options.induction_stride)},
                                              "ptr.next");
        }
        if (options.early_exit) {
            auto *leave = builder.create_icmp(oir::CmpPred::EQ, iv, builder.i32(13),
                                              "early.cond");
            builder.create_cond_br(leave, exit, header);
        } else {
            builder.create_br(header);
        }

        builder.set_insert_point(exit);
        oir::PhiInst *exit_value = nullptr;
        if (options.exit_phi) {
            exit_value = builder.create_phi(types.int32_ty(), "iv.exit");
            builder.create_ret(exit_value);
        } else {
            builder.create_ret();
        }
        iv->add_incoming(initial_iv, entry);
        iv->add_incoming(next, body);
        if (exit_value != nullptr) {
            exit_value->add_incoming(iv, header);
        }
        if (sum != nullptr) {
            sum->add_incoming(builder.i32(0), entry);
            sum->add_incoming(reduction_next, body);
        }
        if (pointer_phi != nullptr) {
            pointer_phi->add_incoming(initial_pointer, entry);
            pointer_phi->add_incoming(pointer_next, body);
        }
        require_verified(module);
    }

    IntLoopOptions options;
    oir::Module module{"lv-int"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *iv = nullptr;
};

struct FloatLoopFixture final {
    explicit FloatLoopFixture(int induction_stride = 1) {
        auto &types = module.types();
        function = module.create_function(
            "float_kernel", types.func_ty(types.void_ty(), {types.int32_ty()}));
        function->args()[0]->set_name("n");
        auto *input = module.create_global(
            "fa", types.array_ty(types.float_ty(), 4096), false);
        auto *output = module.create_global(
            "fb", types.array_ty(types.float_ty(), 4096), false);
        auto *entry = function->create_block("entry");
        auto *header = function->create_block("loop.header");
        auto *body = function->create_block("loop.body");
        auto *exit = function->create_block("exit");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        oir::Value *initial_iv = builder.i32(0);
        if (induction_stride < 0) {
            initial_iv = builder.create_binary(
                oir::Instruction::OpID::Sub, function->args()[0].get(),
                builder.i32(1), "iv.start");
        }
        builder.create_br(header);
        builder.set_insert_point(header);
        auto *iv = builder.create_phi(types.int32_ty(), "iv");
        auto *condition = induction_stride > 0
                              ? builder.create_icmp(oir::CmpPred::LT, iv,
                                                    function->args()[0].get(),
                                                    "loop.cond")
                              : builder.create_icmp(oir::CmpPred::GE, iv,
                                                    builder.i32(0), "loop.cond");
        builder.create_cond_br(condition, body, exit);
        builder.set_insert_point(body);
        auto *pa = builder.create_gep(input, types.ptr_ty(types.float_ty()),
                                      {builder.i32(0), iv}, "pa");
        auto *pb = builder.create_gep(output, types.ptr_ty(types.float_ty()),
                                      {builder.i32(0), iv}, "pb");
        auto *loaded = builder.create_load(pa, types.float_ty(), "loaded");
        auto *as_int = builder.create_fptosi(loaded, types.int32_ty(), "as.int");
        auto *biased = builder.create_binary(oir::Instruction::OpID::Add, as_int,
                                             builder.i32(3), "biased");
        auto *as_float = builder.create_sitofp(biased, types.float_ty(), "as.float");
        auto *computed = builder.create_binary(oir::Instruction::OpID::FAdd, as_float,
                                               builder.f32(0.5F), "computed");
        builder.create_store(computed, pb);
        auto *next = induction_stride > 0
                         ? builder.create_binary(oir::Instruction::OpID::Add, iv,
                                                 builder.i32(induction_stride),
                                                 "iv.next")
                         : builder.create_binary(oir::Instruction::OpID::Sub, iv,
                                                 builder.i32(-induction_stride),
                                                 "iv.next");
        builder.create_br(header);
        builder.set_insert_point(exit);
        builder.create_ret();
        iv->add_incoming(initial_iv, entry);
        iv->add_incoming(next, body);
        require_verified(module);
    }

    oir::Module module{"lv-float"};
    oir::Function *function = nullptr;
};

struct IntegerReductionFixture final {
    explicit IntegerReductionFixture(oir::Instruction::OpID reduction_op,
                                     bool guarded_preheader = false)
        : reduction_op(reduction_op) {
        auto &types = module.types();
        function = module.create_function(
            "reduce", types.func_ty(types.int32_ty(),
                                    {types.int32_ty(), types.int32_ty()}));
        function->args()[0]->set_name("n");
        function->args()[1]->set_name("seed");
        auto *input = module.create_global(
            "input", types.array_ty(types.int32_ty(), 4096), false);
        auto *entry = function->create_block("entry");
        auto *header = function->create_block("loop.header");
        auto *body = function->create_block("loop.body");
        auto *exit = function->create_block("exit");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        if (guarded_preheader) {
            auto *has_trip = builder.create_icmp(
                oir::CmpPred::GT, function->args()[0].get(), builder.i32(0),
                "outer.has.trip");
            builder.create_cond_br(has_trip, header, exit);
        } else {
            builder.create_br(header);
        }
        builder.set_insert_point(header);
        auto *iv = builder.create_phi(types.int32_ty(), "iv");
        accumulator = builder.create_phi(types.int32_ty(), "acc");
        auto *condition = builder.create_icmp(
            oir::CmpPred::LT, iv, function->args()[0].get(), "loop.cond");
        builder.create_cond_br(condition, body, exit);
        builder.set_insert_point(body);
        auto *pointer = builder.create_gep(
            input, types.ptr_ty(types.int32_ty()), {builder.i32(0), iv}, "ptr");
        auto *lane = builder.create_load(pointer, types.int32_ty(), "lane");
        update = builder.create_binary(reduction_op, accumulator, lane, "acc.next");
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(1), "iv.next");
        builder.create_br(header);
        builder.set_insert_point(exit);
        oir::PhiInst *guarded_result = nullptr;
        if (guarded_preheader) {
            guarded_result = builder.create_phi(types.int32_ty(), "guarded.result");
            builder.create_ret(guarded_result);
        } else {
            builder.create_ret(accumulator);
        }
        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        accumulator->add_incoming(function->args()[1].get(), entry);
        accumulator->add_incoming(update, body);
        if (guarded_result != nullptr) {
            guarded_result->add_incoming(function->args()[1].get(), entry);
            guarded_result->add_incoming(accumulator, header);
        }
        require_verified(module);
    }

    oir::Instruction::OpID reduction_op;
    oir::Module module{"lv-reduction"};
    oir::Function *function = nullptr;
    oir::PhiInst *accumulator = nullptr;
    oir::BinaryInst *update = nullptr;
};

struct UnrolledIntegerReductionFixture final {
    explicit UnrolledIntegerReductionFixture(bool escape_intermediate = false) {
        auto &types = module.types();
        function = module.create_function(
            "reduce4", types.func_ty(types.int32_ty(),
                                      {types.int32_ty(), types.int32_ty()}));
        function->args()[0]->set_name("groups");
        function->args()[1]->set_name("seed");
        auto *input = module.create_global(
            "input4", types.array_ty(types.int32_ty(), 4096), false);
        entry = function->create_block("entry");
        header = function->create_block("loop.header");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        bound = builder.create_binary(oir::Instruction::OpID::Mul,
                                      function->args()[0].get(), builder.i32(4),
                                      "element.count");
        auto *start_pointer = builder.create_gep(
            input, types.ptr_ty(types.int32_ty()),
            {builder.i32(0), builder.i32(0)}, "ptr.start");
        auto *has_groups = builder.create_icmp(
            oir::CmpPred::GT, function->args()[0].get(), builder.i32(0),
            "outer.has.groups");
        builder.create_cond_br(has_groups, header, exit);

        builder.set_insert_point(header);
        iv = builder.create_phi(types.int32_ty(), "iv");
        accumulator = builder.create_phi(types.int32_ty(), "acc");
        pointer = builder.create_phi(types.ptr_ty(types.int32_ty()), "ptr");
        auto *condition = builder.create_icmp(oir::CmpPred::LT, iv, bound,
                                              "loop.cond");
        builder.create_cond_br(condition, body, exit);

        builder.set_insert_point(body);
        oir::Value *carried = accumulator;
        for (int lane_index = 0; lane_index < 4; ++lane_index) {
            oir::Value *lane_pointer = pointer;
            if (lane_index != 0) {
                lane_pointer = builder.create_gep(
                    pointer, types.ptr_ty(types.int32_ty()),
                    {builder.i32(lane_index)},
                    "ptr.offset." + std::to_string(lane_index));
            }
            auto *lane = builder.create_load(
                lane_pointer, types.int32_ty(),
                "lane." + std::to_string(lane_index));
            auto *update = builder.create_binary(
                oir::Instruction::OpID::Add, carried, lane,
                "acc.next." + std::to_string(lane_index));
            updates.push_back(update);
            carried = update;
        }
        if (escape_intermediate) {
            builder.create_binary(oir::Instruction::OpID::Add, updates.front(),
                                  builder.i32(9), "escaped.intermediate");
        }
        final_update = updates.back();
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(4), "iv.next");
        auto *pointer_next = builder.create_gep(
            pointer, types.ptr_ty(types.int32_ty()), {builder.i32(4)},
            "ptr.next");
        builder.create_br(header);

        builder.set_insert_point(exit);
        result = builder.create_phi(types.int32_ty(), "result");
        builder.create_ret(result);

        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        accumulator->add_incoming(function->args()[1].get(), entry);
        accumulator->add_incoming(final_update, body);
        pointer->add_incoming(start_pointer, entry);
        pointer->add_incoming(pointer_next, body);
        result->add_incoming(function->args()[1].get(), entry);
        result->add_incoming(accumulator, header);
        require_verified(module);
    }

    oir::Module module{"lv-unrolled-reduction"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::BinaryInst *bound = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::PhiInst *accumulator = nullptr;
    oir::PhiInst *pointer = nullptr;
    std::vector<oir::BinaryInst *> updates;
    oir::BinaryInst *final_update = nullptr;
    oir::PhiInst *result = nullptr;
};

struct AggregatePointeeStrideFixture final {
    AggregatePointeeStrideFixture() {
        auto &types = module.types();
        auto *row_type = types.array_ty(types.int32_ty(), 4);
        function = module.create_function(
            "aggregate_stride",
            types.func_ty(types.int32_ty(), {types.int32_ty(), types.int32_ty()}));
        function->args()[0]->set_name("rows");
        function->args()[1]->set_name("seed");
        auto *input = module.create_global(
            "rows4", types.array_ty(row_type, 4096), false);
        auto *entry = function->create_block("entry");
        auto *header = function->create_block("loop.header");
        auto *body = function->create_block("loop.body");
        auto *exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *row_start = builder.create_gep(
            input, types.ptr_ty(row_type), {builder.i32(0), builder.i32(0)},
            "row.start");
        builder.create_br(header);

        builder.set_insert_point(header);
        auto *iv = builder.create_phi(types.int32_ty(), "iv");
        auto *accumulator = builder.create_phi(types.int32_ty(), "acc");
        auto *row = builder.create_phi(types.ptr_ty(row_type), "row");
        auto *condition = builder.create_icmp(
            oir::CmpPred::LT, iv, function->args()[0].get(), "loop.cond");
        builder.create_cond_br(condition, body, exit);

        builder.set_insert_point(body);
        auto *cell = builder.create_gep(
            row, types.ptr_ty(types.int32_ty()),
            {builder.i32(0), builder.i32(0)}, "row.first.cell");
        auto *lane = builder.create_load(cell, types.int32_ty(), "lane");
        auto *update = builder.create_binary(
            oir::Instruction::OpID::Add, accumulator, lane, "acc.next");
        auto *next = builder.create_binary(
            oir::Instruction::OpID::Add, iv, builder.i32(1), "iv.next");
        auto *row_next = builder.create_gep(
            row, types.ptr_ty(row_type), {builder.i32(1)}, "row.next");
        builder.create_br(header);

        builder.set_insert_point(exit);
        builder.create_ret(accumulator);

        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        accumulator->add_incoming(function->args()[1].get(), entry);
        accumulator->add_incoming(update, body);
        row->add_incoming(row_start, entry);
        row->add_incoming(row_next, body);
        require_verified(module);
    }

    oir::Module module{"lv-aggregate-pointee-stride"};
    oir::Function *function = nullptr;
};

struct FloatReductionFixture final {
    FloatReductionFixture() {
        auto &types = module.types();
        function = module.create_function(
            "float_reduce", types.func_ty(types.float_ty(),
                                          {types.int32_ty(), types.float_ty()}));
        function->args()[0]->set_name("n");
        function->args()[1]->set_name("seed");
        auto *input = module.create_global(
            "input", types.array_ty(types.float_ty(), 4096), false);
        auto *entry = function->create_block("entry");
        auto *header = function->create_block("loop.header");
        auto *body = function->create_block("loop.body");
        auto *exit = function->create_block("exit");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_br(header);
        builder.set_insert_point(header);
        auto *iv = builder.create_phi(types.int32_ty(), "iv");
        auto *accumulator = builder.create_phi(types.float_ty(), "acc");
        auto *condition = builder.create_icmp(
            oir::CmpPred::LT, iv, function->args()[0].get(), "loop.cond");
        builder.create_cond_br(condition, body, exit);
        builder.set_insert_point(body);
        auto *pointer = builder.create_gep(
            input, types.ptr_ty(types.float_ty()), {builder.i32(0), iv}, "ptr");
        auto *lane = builder.create_load(pointer, types.float_ty(), "lane");
        auto *update = builder.create_binary(oir::Instruction::OpID::FAdd,
                                             accumulator, lane, "acc.next");
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(1), "iv.next");
        builder.create_br(header);
        builder.set_insert_point(exit);
        builder.create_ret(accumulator);
        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        accumulator->add_incoming(function->args()[1].get(), entry);
        accumulator->add_incoming(update, body);
        require_verified(module);
    }

    oir::Module module{"lv-float-reduction"};
    oir::Function *function = nullptr;
};

struct RotatedLoopFixture final {
    RotatedLoopFixture() {
        auto &types = module.types();
        function = module.create_function(
            "rotated", types.func_ty(types.void_ty(), {types.int32_ty()}));
        function->args()[0]->set_name("n");
        auto *array = module.create_global(
            "array", types.array_ty(types.int32_ty(), 4096), false);
        entry = function->create_block("entry");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *pointer_start = builder.create_gep(
            array, types.ptr_ty(types.int32_ty()), {builder.i32(0), builder.i32(0)},
            "ptr.start");
        auto *guard = builder.create_icmp(oir::CmpPred::LT, builder.i32(0),
                                          function->args()[0].get(), "guard");
        builder.create_cond_br(guard, body, exit);
        builder.set_insert_point(body);
        iv = builder.create_phi(types.int32_ty(), "iv");
        auto *pointer = builder.create_phi(types.ptr_ty(types.int32_ty()), "ptr");
        auto *loaded = builder.create_load(pointer, types.int32_ty(), "loaded");
        auto *biased = builder.create_binary(oir::Instruction::OpID::Add, loaded,
                                             builder.i32(5), "biased");
        builder.create_store(biased, pointer);
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(1), "iv.next");
        auto *pointer_next = builder.create_gep(
            pointer, types.ptr_ty(types.int32_ty()), {builder.i32(1)}, "ptr.next");
        auto *continue_loop = builder.create_icmp(
            oir::CmpPred::LT, next, function->args()[0].get(), "latch.cond");
        builder.create_cond_br(continue_loop, body, exit);
        builder.set_insert_point(exit);
        builder.create_ret();
        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        pointer->add_incoming(pointer_start, entry);
        pointer->add_incoming(pointer_next, body);
        require_verified(module);
    }

    oir::Module module{"lv-rotated"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *iv = nullptr;
};

struct RotatedAliasVersioningFixture final {
    RotatedAliasVersioningFixture() {
        auto &types = module.types();
        auto *pointer = types.ptr_ty(types.int32_ty());
        function = module.create_function(
            "rotated_copy",
            types.func_ty(types.int32_ty(), {pointer, pointer, types.int32_ty()}));
        function->args()[0]->set_name("dst");
        function->args()[1]->set_name("src");
        function->args()[2]->set_name("n");
        entry = function->create_block("entry");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        guard = builder.create_icmp(oir::CmpPred::LT, builder.i32(0),
                                    function->args()[2].get(), "guard");
        builder.create_cond_br(guard, body, exit);

        builder.set_insert_point(body);
        iv = builder.create_phi(types.int32_ty(), "iv");
        dst = builder.create_phi(pointer, "dst.ptr");
        src = builder.create_phi(pointer, "src.ptr");
        auto *loaded = builder.create_load(src, types.int32_ty(), "loaded");
        builder.create_store(loaded, dst);
        next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                     builder.i32(1), "iv.next");
        dst_next = builder.create_gep(dst, pointer, {builder.i32(1)}, "dst.next");
        src_next = builder.create_gep(src, pointer, {builder.i32(1)}, "src.next");
        condition = builder.create_icmp(oir::CmpPred::LT, next,
                                        function->args()[2].get(), "latch.cond");
        builder.create_cond_br(condition, body, exit);

        builder.set_insert_point(exit);
        exit_phi = builder.create_phi(types.int32_ty(), "iv.exit");
        builder.create_ret(exit_phi);

        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        dst->add_incoming(function->args()[0].get(), entry);
        dst->add_incoming(dst_next, body);
        src->add_incoming(function->args()[1].get(), entry);
        src->add_incoming(src_next, body);
        exit_phi->add_incoming(builder.i32(0), entry);
        exit_phi->add_incoming(next, body);
        require_verified(module);
    }

    oir::Module module{"lv-rotated-alias"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::PhiInst *dst = nullptr;
    oir::PhiInst *src = nullptr;
    oir::BinaryInst *next = nullptr;
    oir::GetElementPtrInst *dst_next = nullptr;
    oir::GetElementPtrInst *src_next = nullptr;
    oir::CmpInst *guard = nullptr;
    oir::CmpInst *condition = nullptr;
    oir::PhiInst *exit_phi = nullptr;
};

struct RotatedDiamondFixture final {
    explicit RotatedDiamondFixture(bool shared_zero_trip_exit = true)
        : shared_zero_trip_exit(shared_zero_trip_exit) {
        auto &types = module.types();
        function = module.create_function(
            "rotated_diamond",
            types.func_ty(types.void_ty(), {types.int32_ty()}));
        function->args()[0]->set_name("n");
        auto *array = module.create_global(
            "rotated.diamond.output", types.array_ty(types.int32_ty(), 4096), false);
        entry = function->create_block("entry");
        header = function->create_block("loop.header");
        then_block = function->create_block("loop.then");
        latch = function->create_block("loop.latch");
        exit = function->create_block("exit");
        if (!shared_zero_trip_exit) {
            zero_trip_exit = function->create_block("zero.trip.exit");
        }

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *pointer_start = builder.create_gep(
            array, types.ptr_ty(types.int32_ty()),
            {builder.i32(0), builder.i32(0)}, "ptr.start");
        guard = builder.create_icmp(oir::CmpPred::LT, builder.i32(0),
                                    function->args()[0].get(), "guard");
        builder.create_cond_br(guard, header,
                               shared_zero_trip_exit ? exit : zero_trip_exit);

        builder.set_insert_point(header);
        iv = builder.create_phi(types.int32_ty(), "iv");
        pointer = builder.create_phi(types.ptr_ty(types.int32_ty()), "ptr");
        builder.create_store(iv, pointer);
        auto *remainder = builder.create_binary(oir::Instruction::OpID::And, iv,
                                                builder.i32(3), "lane.remainder");
        lane_condition = builder.create_icmp(oir::CmpPred::EQ, remainder,
                                             builder.i32(0), "lane.cond");
        builder.create_cond_br(lane_condition, then_block, latch);

        builder.set_insert_point(then_block);
        builder.create_store(builder.i32(4), pointer);
        builder.create_br(latch);

        builder.set_insert_point(latch);
        next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                     builder.i32(1), "iv.next");
        pointer_next = builder.create_gep(pointer, types.ptr_ty(types.int32_ty()),
                                          {builder.i32(1)}, "ptr.next");
        loop_condition = builder.create_icmp(
            oir::CmpPred::LT, next, function->args()[0].get(), "latch.cond");
        builder.create_cond_br(loop_condition, header, exit);

        builder.set_insert_point(exit);
        builder.create_ret();
        if (zero_trip_exit != nullptr) {
            builder.set_insert_point(zero_trip_exit);
            builder.create_ret();
        }

        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, latch);
        pointer->add_incoming(pointer_start, entry);
        pointer->add_incoming(pointer_next, latch);
        require_verified(module);
    }

    bool shared_zero_trip_exit;
    oir::Module module{"lv-rotated-diamond"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *then_block = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::BasicBlock *zero_trip_exit = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::PhiInst *pointer = nullptr;
    oir::BinaryInst *next = nullptr;
    oir::GetElementPtrInst *pointer_next = nullptr;
    oir::CmpInst *guard = nullptr;
    oir::CmpInst *lane_condition = nullptr;
    oir::CmpInst *loop_condition = nullptr;
};

enum class RotatedReductionExitShape {
    Canonical,
    WrongSeed,
    Duplicate,
};

struct RotatedReductionFixture final {
    explicit RotatedReductionFixture(
        oir::Instruction::OpID operation,
        RotatedReductionExitShape exit_shape = RotatedReductionExitShape::Canonical,
        bool extra_update_use = false) {
        auto &types = module.types();
        function = module.create_function(
            "rotated_reduce",
            types.func_ty(types.int32_ty(), {types.int32_ty(), types.int32_ty()}));
        function->args()[0]->set_name("n");
        function->args()[1]->set_name("seed");
        seed = function->args()[1].get();
        auto *input = module.create_global(
            "rotated.input", types.array_ty(types.int32_ty(), 4096), false);
        entry = function->create_block("entry");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *guard = builder.create_icmp(oir::CmpPred::LT, builder.i32(0),
                                          function->args()[0].get(), "guard");
        builder.create_cond_br(guard, body, exit);

        builder.set_insert_point(body);
        auto *iv = builder.create_phi(types.int32_ty(), "iv");
        accumulator = builder.create_phi(types.int32_ty(), "acc");
        auto *pointer = builder.create_gep(
            input, types.ptr_ty(types.int32_ty()), {builder.i32(0), iv}, "ptr");
        auto *lane = builder.create_load(pointer, types.int32_ty(), "lane");
        update = builder.create_binary(operation, accumulator, lane, "acc.next");
        if (extra_update_use) {
            builder.create_binary(oir::Instruction::OpID::Add, update, builder.i32(9),
                                  "forbidden.update.use");
        }
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(1), "iv.next");
        auto *continue_loop = builder.create_icmp(
            oir::CmpPred::LT, next, function->args()[0].get(), "latch.cond");
        builder.create_cond_br(continue_loop, body, exit);

        builder.set_insert_point(exit);
        exit_phi = builder.create_phi(types.int32_ty(), "acc.rot.exit");
        oir::Value *result = exit_phi;
        oir::PhiInst *duplicate = nullptr;
        if (exit_shape == RotatedReductionExitShape::Duplicate) {
            duplicate = builder.create_phi(types.int32_ty(), "acc.rot.exit.duplicate");
            result = builder.create_binary(oir::Instruction::OpID::Add, exit_phi,
                                           duplicate, "duplicate.sum");
        }
        builder.create_ret(result);

        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);
        accumulator->add_incoming(seed, entry);
        accumulator->add_incoming(update, body);
        exit_phi->add_incoming(
            exit_shape == RotatedReductionExitShape::WrongSeed
                ? static_cast<oir::Value *>(builder.i32(17))
                : seed,
            entry);
        exit_phi->add_incoming(update, body);
        if (duplicate != nullptr) {
            duplicate->add_incoming(seed, entry);
            duplicate->add_incoming(update, body);
        }
        require_verified(module);
    }

    oir::Module module{"lv-rotated-reduction"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::Value *seed = nullptr;
    oir::PhiInst *accumulator = nullptr;
    oir::BinaryInst *update = nullptr;
    oir::PhiInst *exit_phi = nullptr;
};

struct RotatedReverseLoopFixture final {
    explicit RotatedReverseLoopFixture(int stride) : stride(stride) {
        REQUIRE(stride == -1 || stride == -2 || stride == -4);
        auto &types = module.types();
        function = module.create_function(
            "rotated_reverse", types.func_ty(types.void_ty(), {types.int32_ty()}));
        function->args()[0]->set_name("n");
        auto *input = module.create_global(
            "reverse.input", types.array_ty(types.int32_ty(), 4096), false);
        auto *output = module.create_global(
            "reverse.output", types.array_ty(types.int32_ty(), 4096), false);
        entry = function->create_block("entry");
        body = function->create_block("loop.body");
        exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        start = builder.create_binary(oir::Instruction::OpID::Sub,
                                      function->args()[0].get(), builder.i32(1),
                                      "iv.start");
        auto *guard = builder.create_icmp(oir::CmpPred::GE, start, builder.i32(0),
                                          "guard");
        builder.create_cond_br(guard, body, exit);

        builder.set_insert_point(body);
        iv = builder.create_phi(types.int32_ty(), "iv");
        auto *source = builder.create_gep(
            input, types.ptr_ty(types.int32_ty()), {builder.i32(0), iv}, "source");
        auto *destination = builder.create_gep(
            output, types.ptr_ty(types.int32_ty()), {builder.i32(0), iv},
            "destination");
        auto *lane = builder.create_load(source, types.int32_ty(), "lane");
        auto *biased = builder.create_binary(oir::Instruction::OpID::Add, lane,
                                             builder.i32(3), "biased");
        builder.create_store(biased, destination);
        next = builder.create_binary(oir::Instruction::OpID::Sub, iv,
                                     builder.i32(-stride), "iv.next");
        auto *continue_loop = builder.create_icmp(oir::CmpPred::GE, next,
                                                  builder.i32(0), "latch.cond");
        builder.create_cond_br(continue_loop, body, exit);

        builder.set_insert_point(exit);
        builder.create_ret();
        iv->add_incoming(start, entry);
        iv->add_incoming(next, body);
        require_verified(module);
    }

    int stride;
    oir::Module module{"lv-rotated-reverse"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::BinaryInst *start = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::BinaryInst *next = nullptr;
};

struct DiamondLoopOptions final {
    bool else_arm = true;
    bool merge_phi = true;
    bool float_lane = false;
    bool pointer_arguments = false;
    bool call_in_then = false;
    bool trap_in_then = false;
};

struct DiamondLoopFixture final {
    explicit DiamondLoopFixture(DiamondLoopOptions options = {}) : options(options) {
        auto &types = module.types();
        auto *element_type = options.float_lane
                                 ? static_cast<oir::Type *>(types.float_ty())
                                 : static_cast<oir::Type *>(types.int32_ty());
        std::vector<oir::Type *> params;
        if (options.pointer_arguments) {
            params.push_back(types.ptr_ty(element_type));
            params.push_back(types.ptr_ty(element_type));
        }
        params.push_back(types.int32_ty());
        function = module.create_function(
            "diamond", types.func_ty(types.void_ty(), params));

        std::size_t argument = 0;
        oir::Value *input = nullptr;
        oir::Value *arm_output = nullptr;
        oir::Value *merge_output = nullptr;
        if (options.pointer_arguments) {
            input = function->args()[argument++].get();
            arm_output = function->args()[argument++].get();
            merge_output = arm_output;
        } else {
            input = module.create_global(
                "diamond.input", types.array_ty(element_type, 4096), false);
            arm_output = module.create_global(
                "diamond.arm", types.array_ty(element_type, 4096), false);
            merge_output = module.create_global(
                "diamond.merge", types.array_ty(element_type, 4096), false);
        }
        auto *trip_count = function->args()[argument].get();
        trip_count->set_name("n");

        entry = function->create_block("entry");
        header = function->create_block("loop.header");
        condition_block = function->create_block("loop.if");
        then_block = function->create_block("loop.then");
        if (options.else_arm) {
            else_block = function->create_block("loop.else");
        }
        latch = function->create_block("loop.latch");
        exit = function->create_block("exit");

        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_br(header);

        builder.set_insert_point(header);
        iv = builder.create_phi(types.int32_ty(), "iv");
        auto *loop_condition =
            builder.create_icmp(oir::CmpPred::LT, iv, trip_count, "loop.cond");
        builder.create_cond_br(loop_condition, condition_block, exit);

        auto make_address = [&](oir::Value *base, const std::string &name) {
            std::vector<oir::Value *> indices = options.pointer_arguments
                                                    ? std::vector<oir::Value *>{iv}
                                                    : std::vector<oir::Value *>{builder.i32(0), iv};
            return builder.create_gep(base, types.ptr_ty(element_type), indices, name);
        };

        builder.set_insert_point(condition_block);
        auto *input_address = make_address(input, "input.addr");
        loaded = builder.create_load(input_address, element_type, "lane");
        oir::Value *lane_condition = nullptr;
        if (options.float_lane) {
            lane_condition =
                builder.create_fcmp(oir::CmpPred::GT, loaded, builder.f32(0.0F), "lane.cond");
        } else {
            lane_condition =
                builder.create_icmp(oir::CmpPred::GT, loaded, builder.i32(0), "lane.cond");
        }
        builder.create_cond_br(lane_condition, then_block,
                               options.else_arm ? else_block : latch);

        builder.set_insert_point(then_block);
        auto *then_address = make_address(arm_output, "then.addr");
        if (options.float_lane) {
            then_value = builder.create_binary(oir::Instruction::OpID::FAdd, loaded,
                                               builder.f32(1.25F), "then.value");
        } else {
            then_value = builder.create_binary(oir::Instruction::OpID::Add, loaded,
                                               builder.i32(7), "then.value");
        }
        if (options.trap_in_then) {
            then_value = builder.create_binary(oir::Instruction::OpID::SDiv, then_value, iv,
                                               "then.trap");
        }
        if (options.call_in_then) {
            auto *callee = module.create_function(
                "diamond.opaque", types.func_ty(types.void_ty(), {}), true);
            builder.create_call(callee, types.void_ty(), {});
        }
        builder.create_store(then_value, then_address);
        builder.create_br(latch);

        if (else_block != nullptr) {
            builder.set_insert_point(else_block);
            auto *else_address = make_address(arm_output, "else.addr");
            if (options.float_lane) {
                else_value = builder.create_binary(oir::Instruction::OpID::FSub, loaded,
                                                   builder.f32(2.0F), "else.value");
            } else {
                else_value = builder.create_binary(oir::Instruction::OpID::Sub, loaded,
                                                   builder.i32(3), "else.value");
            }
            builder.create_store(else_value, else_address);
            builder.create_br(latch);
        } else {
            else_value = loaded;
        }

        builder.set_insert_point(latch);
        if (options.merge_phi) {
            merge = builder.create_phi(element_type, "merged");
            merge->add_incoming(then_value, then_block);
            merge->add_incoming(else_value,
                                else_block == nullptr ? condition_block : else_block);
            auto *merge_address = make_address(merge_output, "merge.addr");
            builder.create_store(merge, merge_address);
        }
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv,
                                           builder.i32(1), "iv.next");
        builder.create_br(header);

        builder.set_insert_point(exit);
        builder.create_ret();
        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, latch);
        require_verified(module);
    }

    DiamondLoopOptions options;
    oir::Module module{"lv-diamond"};
    oir::Function *function = nullptr;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *condition_block = nullptr;
    oir::BasicBlock *then_block = nullptr;
    oir::BasicBlock *else_block = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::PhiInst *merge = nullptr;
    oir::Value *loaded = nullptr;
    oir::Value *then_value = nullptr;
    oir::Value *else_value = nullptr;
};

const pass::oir_vectorize::Remark &single_remark(const RemarkLog &remarks) {
    REQUIRE(remarks.remarks().size() == 1);
    return remarks.remarks().front();
}

void require_rejected(IntLoopOptions fixture_options, RemarkCode expected) {
    IntLoopFixture fixture(fixture_options);
    const auto before = fixture.module.print();
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success);
    REQUIRE(!result.changed);
    REQUIRE(result.loops_vectorized == 0);
    REQUIRE(single_remark(remarks).code == expected);
    REQUIRE(!single_remark(remarks).succeeded());
    REQUIRE(fixture.module.print() == before);
    require_verified(fixture.module);
}

void test_unknown_trip_transforms_to_verified_vla_cfg() {
    IntLoopFixture fixture;
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success);
    REQUIRE(result.changed);
    REQUIRE(result.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).code == RemarkCode::Vectorized);
    REQUIRE(single_remark(remarks).succeeded());

    oir::SetVLInst *setvl = nullptr;
    unsigned vp_loads = 0;
    unsigned vp_stores = 0;
    unsigned vp_binaries = 0;
    unsigned scalar_loads = 0;
    unsigned scalar_stores = 0;
    for (auto &block : fixture.function->blocks()) {
        for (auto &instruction : block->instructions()) {
            if (auto *candidate = dynamic_cast<oir::SetVLInst *>(instruction.get())) {
                REQUIRE(setvl == nullptr);
                setvl = candidate;
            }
            vp_loads += dynamic_cast<oir::VPLoadInst *>(instruction.get()) != nullptr;
            vp_stores += dynamic_cast<oir::VPStoreInst *>(instruction.get()) != nullptr;
            vp_binaries += dynamic_cast<oir::VPBinaryInst *>(instruction.get()) != nullptr;
            scalar_loads += dynamic_cast<oir::LoadInst *>(instruction.get()) != nullptr;
            scalar_stores += dynamic_cast<oir::StoreInst *>(instruction.get()) != nullptr;
        }
    }
    REQUIRE(setvl != nullptr);
    REQUIRE(setvl->type() == fixture.module.types().int32_ty());
    REQUIRE(setvl->vector_type()->element_count().is_scalable());
    REQUIRE(setvl->vector_type()->element_count().min_lanes > 0);
    auto *remaining = dynamic_cast<oir::PhiInst *>(setvl->avl());
    REQUIRE(remaining != nullptr);
    REQUIRE(vp_loads == 1);
    REQUIRE(vp_stores == 1);
    REQUIRE(vp_binaries >= 1);
    REQUIRE(scalar_loads == 0);
    REQUIRE(scalar_stores == 0);

    bool evl_use = false;
    bool iv_increment = false;
    bool remaining_decrement = false;
    for (const auto &use : setvl->uses()) {
        if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(use.user)) {
            evl_use |= vp->evl() == setvl;
        }
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(use.user)) {
            const bool consumes_vl = binary->lhs() == setvl || binary->rhs() == setvl;
            iv_increment |= consumes_vl && binary->op() == oir::Instruction::OpID::Add;
            remaining_decrement |= consumes_vl && binary->op() == oir::Instruction::OpID::Sub;
        }
    }
    REQUIRE(evl_use);
    REQUIRE(iv_increment);
    REQUIRE(remaining_decrement);

    oir::Value *initial_remaining = nullptr;
    oir::Value *remaining_next = nullptr;
    for (const auto &[incoming, block] : remaining->incoming()) {
        if (block == fixture.entry) {
            initial_remaining = incoming;
        } else if (block == fixture.body) {
            remaining_next = incoming;
        }
    }
    auto *nonnegative_trip = dynamic_cast<oir::BinaryInst *>(initial_remaining);
    REQUIRE(nonnegative_trip != nullptr);
    REQUIRE(nonnegative_trip->op() == oir::Instruction::OpID::Mul);
    const auto *trip_mask = dynamic_cast<const oir::CastInst *>(nonnegative_trip->lhs());
    if (trip_mask == nullptr) {
        trip_mask = dynamic_cast<const oir::CastInst *>(nonnegative_trip->rhs());
    }
    REQUIRE(trip_mask != nullptr);
    REQUIRE(trip_mask->op() == oir::Instruction::OpID::ZExt);
    const auto *trip_positive = dynamic_cast<const oir::CmpInst *>(trip_mask->src());
    REQUIRE(trip_positive != nullptr);
    REQUIRE(trip_positive->pred() == oir::CmpPred::GT);
    auto *remaining_sub = dynamic_cast<oir::BinaryInst *>(remaining_next);
    REQUIRE(remaining_sub != nullptr);
    REQUIRE(remaining_sub->op() == oir::Instruction::OpID::Sub);
    REQUIRE(remaining_sub->lhs() == remaining);
    REQUIRE(remaining_sub->rhs() == setvl);

    const auto *header_branch =
        dynamic_cast<const oir::BranchInst *>(fixture.header->terminator());
    REQUIRE(header_branch != nullptr && header_branch->is_conditional());
    const auto *remaining_test =
        dynamic_cast<const oir::CmpInst *>(header_branch->cond());
    REQUIRE(remaining_test != nullptr);
    REQUIRE(remaining_test->pred() == oir::CmpPred::NE);
    REQUIRE(remaining_test->lhs() == remaining);
    const auto *remaining_zero =
        dynamic_cast<const oir::ConstantInt *>(remaining_test->rhs());
    REQUIRE(remaining_zero != nullptr && remaining_zero->value() == 0);

    oir::Value *iv_next = nullptr;
    for (const auto &[incoming, block] : fixture.iv->incoming()) {
        if (block == fixture.body) {
            iv_next = incoming;
        }
    }
    const auto *iv_add = dynamic_cast<const oir::BinaryInst *>(iv_next);
    REQUIRE(iv_add != nullptr && iv_add->op() == oir::Instruction::OpID::Add);
    REQUIRE((iv_add->lhs() == fixture.iv && iv_add->rhs() == setvl) ||
            (iv_add->rhs() == fixture.iv && iv_add->lhs() == setvl));
    for (const auto &instruction : fixture.body->instructions()) {
        if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(instruction.get())) {
            REQUIRE(vp->evl() == setvl);
            REQUIRE(vp->tail_policy() == oir::TailPolicy::Agnostic);
            REQUIRE(vp->mask_policy() == oir::MaskPolicy::Agnostic);
            if (vp->has_passthrough()) {
                REQUIRE(vp->passthrough() != nullptr);
            }
            REQUIRE(vp->type()->is_void() || vp->type()->is_scalable_vector() ||
                    dynamic_cast<const oir::VPReductionInst *>(vp) != nullptr);
        }
        REQUIRE(instruction->name() != "loop.cond");
    }

    oir::DominatorTree dominators(*fixture.function);
    oir::LoopInfo loops(*fixture.function, dominators);
    REQUIRE(loops.loops().size() == 1);
    oir::OIRAliasAnalysis alias_analysis;
    oir::FunctionModRefAnalysis modref(fixture.module);
    oir::MemorySSA memory_ssa(*fixture.function, alias_analysis, modref);
    REQUIRE(memory_ssa.access_for(setvl) == nullptr);
    REQUIRE(!alias_analysis.may_read_memory(*setvl));
    REQUIRE(!alias_analysis.may_write_memory(*setvl));
    REQUIRE(!alias_analysis.has_side_effect(*setvl));
    require_roundtrip(fixture.module);

    constexpr int vlmax = 4;
    const int boundaries[] = {0, 1, vlmax - 1, vlmax, vlmax + 1};
    for (int trip_count : boundaries) {
        std::vector<int> scalar(8, 0);
        std::vector<int> vectorized(8, 0);
        std::vector<int> input = {9, 8, 7, 6, 5, 4, 3, 2};
        for (int lane = 0; lane < trip_count; ++lane) {
            scalar[lane] = input[lane] + 7;
        }
        int remaining_model = trip_count > 0 ? trip_count : 0;
        int iv_model = 0;
        while (remaining_model != 0) {
            const int actual = std::min(remaining_model, vlmax);
            for (int lane = 0; lane < actual; ++lane) {
                vectorized[iv_model + lane] = input[iv_model + lane] + 7;
            }
            iv_model += actual;
            remaining_model -= actual;
        }
        REQUIRE(vectorized == scalar);
        REQUIRE(iv_model == trip_count);
    }

    RemarkLog second_remarks;
    auto second = LoopVectorizer(options).run(fixture.module, rvv_profile(), second_remarks);
    REQUIRE(second.success);
    REQUIRE(!second.changed);
    for (const auto &remark : second_remarks.remarks()) {
        REQUIRE(!remark.succeeded());
    }
}

void test_o3_interleave_two_emits_independent_vla_chunks() {
    IntLoopOptions fixture_options;
    fixture_options.pointer_induction = true;
    IntLoopFixture fixture(fixture_options);
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.explore_interleave = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    const auto &remark = single_remark(remarks);
    REQUIRE(remark.code == RemarkCode::Vectorized);
    if (remark.plan.interleave != 2) {
        throw std::runtime_error("factor-two plan not selected: " + remark.explanation +
                                 "; gate=" + remark.plan.interleave_capability_gate +
                                 "; vector_cost=" +
                                 std::to_string(remark.plan.estimated_vector_cost));
    }
    REQUIRE(remark.plan.interleave_capability_gate.empty());

    std::vector<oir::SetVLInst *> setvls;
    std::vector<const oir::VPInstruction *> vp_instructions;
    for (const auto &block : fixture.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (auto *setvl = dynamic_cast<oir::SetVLInst *>(instruction.get())) {
                setvls.push_back(setvl);
            }
            if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(instruction.get())) {
                vp_instructions.push_back(vp);
            }
        }
    }
    REQUIRE(setvls.size() == 2);
    REQUIRE(!vp_instructions.empty());
    for (auto *setvl : setvls) {
        REQUIRE(std::any_of(vp_instructions.begin(), vp_instructions.end(),
                            [&](const auto *vp) { return vp->evl() == setvl; }));
    }
    for (const auto *vp : vp_instructions) {
        REQUIRE(vp->evl() == setvls[0] || vp->evl() == setvls[1]);
    }

    const auto *second_avl = dynamic_cast<const oir::BinaryInst *>(setvls[1]->avl());
    REQUIRE(second_avl != nullptr);
    REQUIRE(second_avl->op() == oir::Instruction::OpID::Sub);
    REQUIRE(second_avl->rhs() == setvls[0]);
    const oir::BinaryInst *total_vl = nullptr;
    for (const auto &use : setvls[0]->uses()) {
        const auto *binary = dynamic_cast<const oir::BinaryInst *>(use.user);
        if (binary != nullptr && binary->op() == oir::Instruction::OpID::Add &&
            (binary->lhs() == setvls[1] || binary->rhs() == setvls[1])) {
            total_vl = binary;
        }
    }
    REQUIRE(total_vl != nullptr);
    bool total_updates_iv = false;
    bool total_updates_remaining = false;
    bool total_updates_pointer = false;
    for (const auto &use : total_vl->uses()) {
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(use.user)) {
            total_updates_iv |= binary->op() == oir::Instruction::OpID::Add &&
                                (binary->lhs() == fixture.iv || binary->rhs() == fixture.iv);
            total_updates_remaining |= binary->op() == oir::Instruction::OpID::Sub;
        }
        total_updates_pointer |= dynamic_cast<const oir::GetElementPtrInst *>(use.user) != nullptr;
    }
    REQUIRE(total_updates_iv);
    REQUIRE(total_updates_remaining);
    REQUIRE(total_updates_pointer);
    require_roundtrip(fixture.module);

    constexpr int vlmax = 4;
    const int boundaries[] = {0, 1, vlmax - 1, vlmax, vlmax + 1,
                              2 * vlmax - 1, 2 * vlmax, 2 * vlmax + 1, 37};
    for (int trip_count : boundaries) {
        std::vector<int> scalar(static_cast<std::size_t>(trip_count + 1), 0);
        std::vector<int> vectorized(static_cast<std::size_t>(trip_count + 1), 0);
        for (int lane = 0; lane < trip_count; ++lane) {
            scalar[static_cast<std::size_t>(lane)] = lane + 7;
        }
        int remaining = trip_count;
        int iv = 0;
        while (remaining != 0) {
            const int vl0 = std::min(remaining, vlmax);
            for (int lane = 0; lane < vl0; ++lane) {
                vectorized[static_cast<std::size_t>(iv + lane)] = iv + lane + 7;
            }
            const int remaining1 = remaining - vl0;
            const int vl1 = std::min(remaining1, vlmax);
            for (int lane = 0; lane < vl1; ++lane) {
                vectorized[static_cast<std::size_t>(iv + vl0 + lane)] =
                    iv + vl0 + lane + 7;
            }
            iv += vl0 + vl1;
            remaining = remaining1 - vl1;
        }
        REQUIRE(vectorized == scalar);
        REQUIRE(iv == trip_count);
    }

    IntLoopFixture o2_fixture(fixture_options);
    RemarkLog o2_remarks;
    LoopVectorizerOptions o2_options;
    o2_options.force = true;
    auto o2_result =
        LoopVectorizer(o2_options).run(o2_fixture.module, rvv_profile(), o2_remarks);
    REQUIRE(o2_result.success && o2_result.changed);
    REQUIRE(single_remark(o2_remarks).plan.interleave == 1);

    IntegerReductionFixture reduction_fixture(oir::Instruction::OpID::Add);
    RemarkLog reduction_remarks;
    auto unsupported_options = options;
    unsupported_options.force = true;
    auto reduction_result = LoopVectorizer(unsupported_options)
                                .run(reduction_fixture.module, rvv_profile(), reduction_remarks);
    if (!reduction_result.success || !reduction_result.changed) {
        throw std::runtime_error("factor-one reduction fallback failed: " +
                                 reduction_result.message + "; remark=" +
                                 (reduction_remarks.empty()
                                      ? std::string("none")
                                      : reduction_remarks.remarks().front().explanation));
    }
    REQUIRE(single_remark(reduction_remarks).plan.interleave == 1);
    REQUIRE(single_remark(reduction_remarks)
                .plan.interleave_capability_gate.find("loop-carried reductions") !=
            std::string::npos);

    DiamondLoopFixture diamond_fixture;
    RemarkLog diamond_remarks;
    auto diamond_result = LoopVectorizer(unsupported_options)
                              .run(diamond_fixture.module, rvv_profile(), diamond_remarks);
    REQUIRE(diamond_result.success && diamond_result.changed);
    REQUIRE(single_remark(diamond_remarks).plan.interleave == 1);
    REQUIRE(single_remark(diamond_remarks)
                .plan.interleave_capability_gate.find("diamond if-conversion") !=
            std::string::npos);

    IntLoopOptions versioned_options;
    versioned_options.pointer_arguments = true;
    IntLoopFixture versioned_fixture(versioned_options);
    RemarkLog versioned_remarks;
    auto versioned_result = LoopVectorizer(unsupported_options)
                                .run(versioned_fixture.module, rvv_profile(), versioned_remarks);
    REQUIRE(versioned_result.success && versioned_result.changed);
    REQUIRE(single_remark(versioned_remarks).plan.interleave == 1);
    REQUIRE(single_remark(versioned_remarks)
                .plan.interleave_capability_gate.find("runtime alias versioning") !=
            std::string::npos);

    RotatedReverseLoopFixture rotated_fixture(-2);
    RemarkLog rotated_remarks;
    auto rotated_result = LoopVectorizer(unsupported_options)
                              .run(rotated_fixture.module, rvv_profile(), rotated_remarks);
    REQUIRE(rotated_result.success && rotated_result.changed);
    REQUIRE(single_remark(rotated_remarks).plan.interleave == 2);
    unsigned rotated_setvls = 0;
    for (const auto &block : rotated_fixture.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            rotated_setvls +=
                dynamic_cast<const oir::SetVLInst *>(instruction.get()) != nullptr;
        }
    }
    REQUIRE(rotated_setvls == 2);
    require_roundtrip(rotated_fixture.module);

    IntLoopFixture rollback_fixture(fixture_options);
    const auto rollback_before = rollback_fixture.module.print();
    RemarkLog rollback_remarks;
    bool validator_saw_two_chunks = false;
    auto rollback_options = options;
    rollback_options.force = true;
    rollback_options.post_transform_validation =
        [&](const oir::Module &candidate, std::string &error) {
            unsigned setvl_count = 0;
            for (const auto &candidate_function : candidate.functions()) {
                for (const auto &block : candidate_function->blocks()) {
                    for (const auto &instruction : block->instructions()) {
                        setvl_count +=
                            dynamic_cast<const oir::SetVLInst *>(instruction.get()) != nullptr;
                    }
                }
            }
            validator_saw_two_chunks = setvl_count == 2;
            error = "intentional factor-two rollback";
            return false;
        };
    auto rollback_result = LoopVectorizer(rollback_options)
                               .run(rollback_fixture.module, rvv_profile(), rollback_remarks);
    REQUIRE(!rollback_result.success);
    REQUIRE(!rollback_result.changed);
    REQUIRE(validator_saw_two_chunks);
    REQUIRE(rollback_fixture.module.print() == rollback_before);
    require_verified(rollback_fixture.module);
}

void test_constant_trip_and_mixed_float_casts_transform() {
    IntLoopOptions constant_options;
    constant_options.unknown_trip = false;
    IntLoopFixture constant_loop(constant_options);
    RemarkLog constant_remarks;
    LoopVectorizerOptions forced;
    forced.force = true;
    auto constant_result =
        LoopVectorizer(forced).run(constant_loop.module, rvv_profile(), constant_remarks);
    REQUIRE(constant_result.success && constant_result.changed);
    REQUIRE(single_remark(constant_remarks).succeeded());
    require_roundtrip(constant_loop.module);

    FloatLoopFixture float_loop;
    RemarkLog float_remarks;
    LoopVectorizerOptions relaxed = forced;
    relaxed.strict_floating_point = true;
    auto float_result =
        LoopVectorizer(relaxed).run(float_loop.module, rvv_profile(), float_remarks);
    REQUIRE(float_result.success && float_result.changed);
    REQUIRE(single_remark(float_remarks).succeeded());
    bool saw_vector_cast = false;
    bool saw_vp_float = false;
    for (const auto &block : float_loop.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            saw_vector_cast |=
                dynamic_cast<const oir::VectorCastInst *>(instruction.get()) != nullptr;
            if (const auto *binary =
                    dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
                saw_vp_float |= binary->binary_op() == oir::Instruction::OpID::FAdd;
            }
        }
    }
    REQUIRE(saw_vector_cast);
    REQUIRE(saw_vp_float);
    require_roundtrip(float_loop.module);
}

oir::ReductionKind expected_reduction_kind(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
        return oir::ReductionKind::Add;
    case oir::Instruction::OpID::Mul:
        return oir::ReductionKind::Mul;
    case oir::Instruction::OpID::And:
        return oir::ReductionKind::And;
    case oir::Instruction::OpID::Or:
        return oir::ReductionKind::Or;
    case oir::Instruction::OpID::Xor:
        return oir::ReductionKind::Xor;
    default:
        throw std::runtime_error("unexpected reduction operation in test");
    }
}

std::int32_t fold_integer(oir::Instruction::OpID op, std::int32_t lhs,
                          std::int32_t rhs) {
    switch (op) {
    case oir::Instruction::OpID::Add:
        return lhs + rhs;
    case oir::Instruction::OpID::Mul:
        return lhs * rhs;
    case oir::Instruction::OpID::And:
        return lhs & rhs;
    case oir::Instruction::OpID::Or:
        return lhs | rhs;
    case oir::Instruction::OpID::Xor:
        return lhs ^ rhs;
    default:
        throw std::runtime_error("unexpected reduction operation in model");
    }
}

void test_integer_reductions_transform_and_preserve_chunk_semantics() {
    const oir::Instruction::OpID operations[] = {
        oir::Instruction::OpID::Add,
        oir::Instruction::OpID::Mul,
        oir::Instruction::OpID::And,
        oir::Instruction::OpID::Or,
        oir::Instruction::OpID::Xor,
    };
    constexpr std::int32_t vlmax = 4;
    const std::vector<std::int32_t> lanes = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::int32_t boundaries[] = {0, 1, vlmax - 1, vlmax, vlmax + 1};
    for (auto operation : operations) {
        IntegerReductionFixture fixture(operation);
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed);
        REQUIRE(single_remark(remarks).succeeded());
        const oir::VPReductionInst *vector_reduction = nullptr;
        const oir::SetVLInst *setvl = nullptr;
        for (const auto &block : fixture.function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                if (const auto *candidate =
                        dynamic_cast<const oir::VPReductionInst *>(instruction.get())) {
                    REQUIRE(vector_reduction == nullptr);
                    vector_reduction = candidate;
                }
                if (const auto *candidate =
                        dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
                    setvl = candidate;
                }
            }
        }
        REQUIRE(vector_reduction != nullptr);
        REQUIRE(setvl != nullptr);
        REQUIRE(vector_reduction->kind() == expected_reduction_kind(operation));
        REQUIRE(!vector_reduction->ordered());
        REQUIRE(vector_reduction->passthrough() == fixture.accumulator);
        REQUIRE(vector_reduction->evl() == setvl);
        REQUIRE(vector_reduction->active_mask()->type()->is_scalable_vector());
        bool carried = false;
        for (const auto &[incoming, block] : fixture.accumulator->incoming()) {
            (void)block;
            carried |= incoming == vector_reduction;
        }
        REQUIRE(carried);
        require_roundtrip(fixture.module);

        const std::int32_t seed = operation == oir::Instruction::OpID::Mul ? 1 : 11;
        for (auto trip_count : boundaries) {
            std::int32_t scalar = seed;
            for (std::int32_t lane = 0; lane < trip_count; ++lane) {
                scalar = fold_integer(operation, scalar, lanes[lane]);
            }
            std::int32_t vectorized = seed;
            std::int32_t remaining = trip_count;
            std::int32_t lane = 0;
            while (remaining != 0) {
                const auto actual_vl = std::min(remaining, vlmax);
                for (std::int32_t offset = 0; offset < actual_vl; ++offset) {
                    vectorized = fold_integer(operation, vectorized,
                                              lanes[lane + offset]);
                }
                lane += actual_vl;
                remaining -= actual_vl;
            }
            REQUIRE(vectorized == scalar);
        }
    }
}

void test_guarded_two_block_preheader_preserves_zero_trip_reduction() {
    IntegerReductionFixture fixture(oir::Instruction::OpID::Add, true);
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).code == RemarkCode::Vectorized);

    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *exit = nullptr;
    for (const auto &block : fixture.function->blocks()) {
        if (block->name().find("entry") == 0) {
            entry = block.get();
        } else if (block->name().find("loop.header") == 0) {
            header = block.get();
        } else if (block->name().find("exit") == 0) {
            exit = block.get();
        }
    }
    REQUIRE(entry != nullptr && header != nullptr && exit != nullptr);
    const auto *guard = dynamic_cast<const oir::BranchInst *>(entry->terminator());
    REQUIRE(guard != nullptr && guard->is_conditional());
    REQUIRE(guard->true_bb() == header && guard->false_bb() == exit);
    const auto *guard_condition = dynamic_cast<const oir::CmpInst *>(guard->cond());
    REQUIRE(guard_condition != nullptr && guard_condition->name() == "outer.has.trip");

    auto *merged_result = dynamic_cast<oir::PhiInst *>(exit->instructions().front().get());
    REQUIRE(merged_result != nullptr && merged_result->incoming().size() == 2);
    REQUIRE(incoming_for(*merged_result, entry) == fixture.function->args()[1].get());
    REQUIRE(incoming_for(*merged_result, header) == fixture.accumulator);

    bool saw_remaining_from_guard = false;
    bool saw_reduction = false;
    for (const auto &instruction : header->instructions()) {
        if (const auto *phi = dynamic_cast<const oir::PhiInst *>(instruction.get())) {
            saw_remaining_from_guard |= incoming_for(*phi, entry) != nullptr &&
                                        phi != fixture.accumulator;
        }
    }
    for (const auto &block : fixture.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            saw_reduction |= dynamic_cast<const oir::VPReductionInst *>(instruction.get()) !=
                             nullptr;
        }
    }
    REQUIRE(saw_remaining_from_guard);
    REQUIRE(saw_reduction);
    require_verified(fixture.module);
    require_roundtrip(fixture.module);

    constexpr std::int32_t seed = 17;
    for (std::int32_t count : {0, 1, 3, 4, 5}) {
        std::int32_t scalar = seed;
        std::int32_t vectorized = seed;
        for (std::int32_t lane = 0; lane < count; ++lane) {
            scalar += lane + 1;
        }
        std::int32_t lane = 0;
        while (lane < count) {
            const auto actual_vl = std::min<std::int32_t>(4, count - lane);
            for (std::int32_t offset = 0; offset < actual_vl; ++offset) {
                vectorized += lane + offset + 1;
            }
            lane += actual_vl;
        }
        REQUIRE(vectorized == scalar);
    }
}

void test_linear_unrolled_integer_reduction_chain_and_stride_inheritance() {
    UnrolledIntegerReductionFixture fixture;
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto vectorized = LoopVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(vectorized.success && vectorized.changed);
    REQUIRE(vectorized.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).code == RemarkCode::Vectorized);

    const oir::SetVLInst *setvl = nullptr;
    std::vector<const oir::VPReductionInst *> reductions;
    unsigned gathers = 0;
    for (const auto &instruction : fixture.body->instructions()) {
        if (const auto *candidate =
                dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
            setvl = candidate;
        }
        if (const auto *candidate =
                dynamic_cast<const oir::VPReductionInst *>(instruction.get())) {
            reductions.push_back(candidate);
        }
        gathers += dynamic_cast<const oir::VPGatherInst *>(instruction.get()) != nullptr;
    }
    REQUIRE(setvl != nullptr);
    REQUIRE(reductions.size() == 4);
    REQUIRE(gathers == 4);
    REQUIRE(reductions.front()->passthrough() == fixture.accumulator);
    for (std::size_t index = 1; index < reductions.size(); ++index) {
        REQUIRE(reductions[index]->passthrough() == reductions[index - 1]);
    }
    REQUIRE(incoming_for(*fixture.accumulator, fixture.body) == reductions.back());
    const auto text = fixture.module.print();
    REQUIRE(text.find("trip.iterations") != std::string::npos);
    REQUIRE(text.find("iv.delta") != std::string::npos);
    require_verified(fixture.module);
    require_roundtrip(fixture.module);

    constexpr std::int32_t vlmax = 4;
    const std::vector<std::int32_t> lanes = {
        3, -7, 11, 4, 9, 2, -5, 8, 6, 1, -2, 10,
        5, 12, -3, 7, 14, -9, 13, 15,
    };
    for (std::int32_t groups : {0, 1, vlmax - 1, vlmax, vlmax + 1}) {
        std::int32_t scalar = 19;
        for (std::int32_t lane = 0; lane < groups * 4; ++lane) {
            scalar += lanes[lane];
        }
        std::int32_t chunked = 19;
        std::int32_t group = 0;
        while (group < groups) {
            const auto actual_vl = std::min(vlmax, groups - group);
            for (std::int32_t residue = 0; residue < 4; ++residue) {
                for (std::int32_t lane = 0; lane < actual_vl; ++lane) {
                    chunked += lanes[(group + lane) * 4 + residue];
                }
            }
            group += actual_vl;
        }
        REQUIRE(chunked == scalar);
    }

    UnrolledIntegerReductionFixture escaped(true);
    const auto escaped_before = escaped.module.print();
    RemarkLog escaped_remarks;
    auto rejected = LoopVectorizer(options).run(
        escaped.module, rvv_profile(), escaped_remarks);
    REQUIRE(rejected.success && !rejected.changed);
    REQUIRE(single_remark(escaped_remarks).code == RemarkCode::RejectReduction);
    REQUIRE(single_remark(escaped_remarks).explanation.find("escapes its linear chain") !=
            std::string::npos);
    REQUIRE(escaped.module.print() == escaped_before);

    // A row-pointer step of one advances by an entire [4 x i32].  A GEP to
    // its first scalar element changes pointee units, so inheriting stride=1
    // would be unsound.  Keep the layout-scaled case fail closed until the
    // analysis models aggregate byte sizes explicitly.
    AggregatePointeeStrideFixture aggregate_stride;
    const auto aggregate_before = aggregate_stride.module.print();
    RemarkLog aggregate_remarks;
    auto aggregate_rejected = LoopVectorizer(options).run(
        aggregate_stride.module, rvv_profile(), aggregate_remarks);
    REQUIRE(aggregate_rejected.success && !aggregate_rejected.changed);
    REQUIRE(single_remark(aggregate_remarks).code == RemarkCode::RejectStride);
    REQUIRE(aggregate_stride.module.print() == aggregate_before);
}

void test_rotated_integer_reductions_rewire_only_canonical_exit_phi() {
    const oir::Instruction::OpID operations[] = {
        oir::Instruction::OpID::Add,
        oir::Instruction::OpID::Mul,
        oir::Instruction::OpID::And,
        oir::Instruction::OpID::Or,
        oir::Instruction::OpID::Xor,
    };
    constexpr std::int32_t vlmax = 4;
    const std::vector<std::int32_t> lanes = {2, 3, 1, 4, 2, 1, 3, 2,
                                             1, 2, 3, 1, 2};
    const std::int32_t trip_counts[] = {0, 1, vlmax - 1, vlmax,
                                        vlmax + 1, 13};
    for (auto operation : operations) {
        RotatedReductionFixture fixture(operation);
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result = LoopVectorizer(options).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
        REQUIRE(single_remark(remarks).succeeded());

        const oir::VPReductionInst *vector_reduction = nullptr;
        const oir::SetVLInst *setvl = nullptr;
        for (const auto &instruction : fixture.body->instructions()) {
            if (const auto *candidate =
                    dynamic_cast<const oir::VPReductionInst *>(instruction.get())) {
                REQUIRE(vector_reduction == nullptr);
                vector_reduction = candidate;
            }
            if (const auto *candidate =
                    dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
                setvl = candidate;
            }
        }
        REQUIRE(vector_reduction != nullptr && setvl != nullptr);
        REQUIRE(vector_reduction->kind() == expected_reduction_kind(operation));
        REQUIRE(vector_reduction->passthrough() == fixture.accumulator);
        REQUIRE(vector_reduction->evl() == setvl);
        REQUIRE(incoming_for(*fixture.accumulator, fixture.entry) == fixture.seed);
        REQUIRE(incoming_for(*fixture.accumulator, fixture.body) ==
                vector_reduction);
        REQUIRE(incoming_for(*fixture.exit_phi, fixture.entry) == fixture.seed);
        REQUIRE(incoming_for(*fixture.exit_phi, fixture.body) ==
                vector_reduction);
        require_roundtrip(fixture.module);

        const std::int32_t seed = operation == oir::Instruction::OpID::Mul ? 1 : 11;
        for (auto trip_count : trip_counts) {
            std::int32_t scalar = seed;
            for (std::int32_t lane = 0; lane < trip_count; ++lane) {
                scalar = fold_integer(operation, scalar, lanes[lane]);
            }
            std::int32_t vectorized = seed;
            std::int32_t remaining = trip_count;
            std::int32_t lane = 0;
            while (remaining != 0) {
                const auto actual_vl = std::min(remaining, vlmax);
                for (std::int32_t offset = 0; offset < actual_vl; ++offset) {
                    vectorized = fold_integer(operation, vectorized,
                                              lanes[lane + offset]);
                }
                lane += actual_vl;
                remaining -= actual_vl;
            }
            REQUIRE(vectorized == scalar);
            if (trip_count == 0) {
                REQUIRE(vectorized == seed);
            }
        }
    }

    auto require_rejected_shape = [](RotatedReductionExitShape shape,
                                     bool extra_update_use) {
        RotatedReductionFixture fixture(oir::Instruction::OpID::Add, shape,
                                        extra_update_use);
        const auto before = fixture.module.print();
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result = LoopVectorizer(options).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(single_remark(remarks).code == RemarkCode::RejectReduction);
        REQUIRE(fixture.module.print() == before);
        require_roundtrip(fixture.module);
    };
    require_rejected_shape(RotatedReductionExitShape::WrongSeed, false);
    require_rejected_shape(RotatedReductionExitShape::Duplicate, false);
    require_rejected_shape(RotatedReductionExitShape::Canonical, true);

    RotatedReductionFixture rollback_fixture(oir::Instruction::OpID::Add);
    const auto rollback_before = rollback_fixture.module.print();
    bool saw_rewired_verified_module = false;
    LoopVectorizerOptions rollback_options;
    rollback_options.force = true;
    rollback_options.post_transform_validation =
        [&](const oir::Module &module, std::string &error) {
            std::string verify_error;
            const auto transformed = module.print();
            saw_rewired_verified_module =
                module.verify(&verify_error) &&
                transformed.find("vp.reduce.add") != std::string::npos &&
                transformed.find("acc.rot.exit = phi") != std::string::npos;
            error = "injected rotated reduction rejection";
            return false;
        };
    RemarkLog rollback_remarks;
    auto rollback_result = LoopVectorizer(rollback_options).run(
        rollback_fixture.module, rvv_profile(), rollback_remarks);
    REQUIRE(!rollback_result.success && !rollback_result.changed);
    REQUIRE(saw_rewired_verified_module);
    REQUIRE(rollback_fixture.module.print() == rollback_before);
    REQUIRE(rollback_remarks.remarks().empty());
    require_roundtrip(rollback_fixture.module);
}

void test_actual_vl_advances_pointer_induction() {
    IntLoopOptions fixture_options;
    fixture_options.pointer_induction = true;
    IntLoopFixture fixture(fixture_options);
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    oir::SetVLInst *setvl = nullptr;
    bool pointer_uses_vl = false;
    for (auto &block : fixture.function->blocks()) {
        for (auto &instruction : block->instructions()) {
            if (auto *candidate = dynamic_cast<oir::SetVLInst *>(instruction.get())) {
                setvl = candidate;
            }
        }
    }
    REQUIRE(setvl != nullptr);
    for (const auto &use : setvl->uses()) {
        if (const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(use.user)) {
            const auto indices = gep->indices();
            pointer_uses_vl |= indices.size() == 1 && indices.front() == setvl;
        }
    }
    REQUIRE(pointer_uses_vl);
    require_roundtrip(fixture.module);
}

void test_rotated_single_block_loop_transforms_after_scalar_fixed_point() {
    RotatedLoopFixture fixture;
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(single_remark(remarks).succeeded());
    const oir::SetVLInst *setvl = nullptr;
    const oir::PhiInst *remaining = nullptr;
    for (const auto &instruction : fixture.body->instructions()) {
        if (const auto *candidate =
                dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
            setvl = candidate;
            remaining = dynamic_cast<const oir::PhiInst *>(candidate->avl());
        }
        REQUIRE(dynamic_cast<const oir::LoadInst *>(instruction.get()) == nullptr);
        REQUIRE(dynamic_cast<const oir::StoreInst *>(instruction.get()) == nullptr);
    }
    REQUIRE(setvl != nullptr && remaining != nullptr);
    const auto *branch =
        dynamic_cast<const oir::BranchInst *>(fixture.body->terminator());
    REQUIRE(branch != nullptr && branch->is_conditional());
    const auto *test = dynamic_cast<const oir::CmpInst *>(branch->cond());
    REQUIRE(test != nullptr && test->pred() == oir::CmpPred::NE);
    const auto *remaining_next = dynamic_cast<const oir::BinaryInst *>(test->lhs());
    REQUIRE(remaining_next != nullptr);
    REQUIRE(remaining_next->op() == oir::Instruction::OpID::Sub);
    REQUIRE(remaining_next->lhs() == remaining);
    REQUIRE(remaining_next->rhs() == setvl);
    bool pointer_advance = false;
    for (const auto &use : setvl->uses()) {
        if (const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(use.user)) {
            const auto indices = gep->indices();
            pointer_advance |= indices.size() == 1 && indices.front() == setvl;
        }
    }
    REQUIRE(pointer_advance);
    require_roundtrip(fixture.module);
}

void test_guarded_rotated_alias_versioning_preserves_zero_trip_and_iv_liveout() {
    RotatedAliasVersioningFixture fixture;
    const auto before = fixture.module.print();
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).code == RemarkCode::Vectorized);
    REQUIRE(single_remark(remarks).plan.requires_runtime_alias_check);
    REQUIRE(single_remark(remarks).explanation.find("overflow-safe runtime alias versioning") !=
            std::string::npos);

    const auto text = fixture.module.print();
    REQUIRE(text != before);
    REQUIRE(text.find("call i32 @__yoolang_ranges_disjoint") != std::string::npos);
    REQUIRE(text.find("lv.alias.fast") != std::string::npos);
    REQUIRE(text.find("lv.alias.slow") != std::string::npos);
    REQUIRE(text.find("lv.slow.loop.body") != std::string::npos);
    REQUIRE(text.find("vp.load") != std::string::npos);
    REQUIRE(text.find("vp.store") != std::string::npos);

    const auto *entry_branch =
        dynamic_cast<const oir::BranchInst *>(fixture.entry->terminator());
    REQUIRE(entry_branch != nullptr && entry_branch->is_conditional());
    REQUIRE(entry_branch->cond() == fixture.guard);
    REQUIRE(entry_branch->false_bb() == fixture.exit);
    REQUIRE(entry_branch->true_bb()->name().find("lv.alias.check") != std::string::npos);

    REQUIRE(fixture.exit_phi->incoming().size() == 3);
    REQUIRE(incoming_for(*fixture.exit_phi, fixture.entry) != nullptr);
    auto *fast_liveout = incoming_for(*fixture.exit_phi, fixture.body);
    REQUIRE(fast_liveout != nullptr && fast_liveout != fixture.next);
    bool saw_slow_liveout = false;
    for (const auto &[value, block] : fixture.exit_phi->incoming()) {
        if (block->name().find("lv.slow.loop.body") != std::string::npos) {
            REQUIRE(value->name().find("iv.next.lv.slow") != std::string::npos);
            saw_slow_liveout = true;
        }
    }
    REQUIRE(saw_slow_liveout);
    require_verified(fixture.module);
    require_roundtrip(fixture.module);

    RotatedAliasVersioningFixture disabled_fixture;
    const auto disabled_before = disabled_fixture.module.print();
    LoopVectorizerOptions disabled = options;
    disabled.enable_runtime_alias_versioning = false;
    RemarkLog disabled_remarks;
    auto disabled_result = LoopVectorizer(disabled).run(
        disabled_fixture.module, rvv_profile(), disabled_remarks);
    REQUIRE(disabled_result.success && !disabled_result.changed);
    REQUIRE(single_remark(disabled_remarks).code == RemarkCode::RejectAlias);
    REQUIRE(disabled_fixture.module.print() == disabled_before);

    RotatedAliasVersioningFixture rollback_fixture;
    const auto rollback_before = rollback_fixture.module.print();
    LoopVectorizerOptions rollback_options = options;
    rollback_options.post_transform_validation =
        [](const oir::Module &, std::string &error) {
            error = "injected guarded-rotated alias rejection";
            return false;
        };
    RemarkLog rollback_remarks;
    auto rollback_result = LoopVectorizer(rollback_options).run(
        rollback_fixture.module, rvv_profile(), rollback_remarks);
    REQUIRE(!rollback_result.success && !rollback_result.changed);
    REQUIRE(rollback_fixture.module.print() == rollback_before);
    REQUIRE(rollback_remarks.empty());
    require_verified(rollback_fixture.module);
}

void test_guarded_rotated_diamond_if_converts_with_latch_backedge() {
    RotatedDiamondFixture fixture;
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).code == RemarkCode::Vectorized);
    REQUIRE(single_remark(remarks).succeeded());

    bool has_then_block = false;
    for (const auto &block : fixture.function->blocks()) {
        has_then_block |= block->name() == "loop.then";
    }
    REQUIRE(!has_then_block);

    const auto *guard_branch =
        dynamic_cast<const oir::BranchInst *>(fixture.entry->terminator());
    REQUIRE(guard_branch != nullptr && guard_branch->is_conditional());
    REQUIRE(guard_branch->cond() == fixture.guard);
    REQUIRE(guard_branch->true_bb() == fixture.header);
    REQUIRE(guard_branch->false_bb() == fixture.exit);

    const auto *body_branch =
        dynamic_cast<const oir::BranchInst *>(fixture.header->terminator());
    REQUIRE(body_branch != nullptr && !body_branch->is_conditional());
    REQUIRE(body_branch->target_bb() == fixture.latch);
    const auto *loop_branch =
        dynamic_cast<const oir::BranchInst *>(fixture.latch->terminator());
    REQUIRE(loop_branch != nullptr && loop_branch->is_conditional());
    REQUIRE(loop_branch->true_bb() == fixture.header);
    REQUIRE(loop_branch->false_bb() == fixture.exit);
    const auto *remaining_test =
        dynamic_cast<const oir::CmpInst *>(loop_branch->cond());
    REQUIRE(remaining_test != nullptr &&
            remaining_test->pred() == oir::CmpPred::NE);

    const oir::SetVLInst *setvl = nullptr;
    const oir::Value *active_mask = nullptr;
    const oir::VPBinaryInst *then_mask = nullptr;
    const oir::VPCmpInst *vector_lane_condition = nullptr;
    std::vector<const oir::VPStoreInst *> stores;
    for (const auto &instruction : fixture.header->instructions()) {
        REQUIRE(dynamic_cast<const oir::StoreInst *>(instruction.get()) == nullptr);
        if (const auto *candidate =
                dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
            setvl = candidate;
        }
        if (const auto *splat =
                dynamic_cast<const oir::SplatInst *>(instruction.get())) {
            const auto *one = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
            if (splat->type()->is_mask() && one != nullptr && one->value() == 1) {
                active_mask = splat;
            }
        }
        if (const auto *compare =
                dynamic_cast<const oir::VPCmpInst *>(instruction.get())) {
            if (compare->name().find("lane.cond") != std::string::npos) {
                vector_lane_condition = compare;
            }
        }
        if (const auto *binary =
                dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
            if (binary->name().find("if.then.mask") != std::string::npos) {
                then_mask = binary;
            }
        }
        if (const auto *store =
                dynamic_cast<const oir::VPStoreInst *>(instruction.get())) {
            stores.push_back(store);
        }
    }
    REQUIRE(setvl != nullptr && active_mask != nullptr && then_mask != nullptr);
    REQUIRE(vector_lane_condition != nullptr);
    REQUIRE(then_mask->binary_op() == oir::Instruction::OpID::And);
    REQUIRE(then_mask->lhs() == active_mask);
    REQUIRE(then_mask->rhs() == vector_lane_condition);
    REQUIRE(stores.size() == 2);
    unsigned active_stores = 0;
    unsigned conditional_stores = 0;
    for (const auto *store : stores) {
        REQUIRE(store->evl() == setvl);
        active_stores += store->active_mask() == active_mask ? 1U : 0U;
        conditional_stores += store->active_mask() == then_mask ? 1U : 0U;
    }
    REQUIRE(active_stores == 1 && conditional_stores == 1);

    const auto *remaining = dynamic_cast<const oir::PhiInst *>(setvl->avl());
    REQUIRE(remaining != nullptr);
    REQUIRE(incoming_for(*remaining, fixture.entry) != nullptr);
    const auto *remaining_next =
        dynamic_cast<const oir::BinaryInst *>(incoming_for(*remaining, fixture.latch));
    REQUIRE(remaining_next != nullptr);
    REQUIRE(remaining_next->op() == oir::Instruction::OpID::Sub);
    REQUIRE(remaining_next->lhs() == remaining);
    REQUIRE(remaining_next->rhs() == setvl);
    REQUIRE(incoming_for(*fixture.iv, fixture.entry) != nullptr);
    REQUIRE(incoming_for(*fixture.iv, fixture.latch) != nullptr);
    REQUIRE(incoming_for(*fixture.pointer, fixture.entry) != nullptr);
    REQUIRE(incoming_for(*fixture.pointer, fixture.latch) != nullptr);
    require_roundtrip(fixture.module);

    RotatedDiamondFixture malformed(false);
    const auto malformed_before = malformed.module.print();
    RemarkLog malformed_remarks;
    auto malformed_result = LoopVectorizer(options).run(
        malformed.module, rvv_profile(), malformed_remarks);
    REQUIRE(malformed_result.success && !malformed_result.changed);
    REQUIRE(single_remark(malformed_remarks).code == RemarkCode::RejectEarlyExit);
    REQUIRE(malformed.module.print() == malformed_before);
    require_verified(malformed.module);

    RotatedDiamondFixture rollback;
    const auto rollback_before = rollback.module.print();
    bool saw_verified_rotated_diamond = false;
    LoopVectorizerOptions rollback_options;
    rollback_options.force = true;
    rollback_options.post_transform_validation =
        [&](const oir::Module &module, std::string &error) {
            std::string verify_error;
            const auto transformed = module.print();
            saw_verified_rotated_diamond =
                module.verify(&verify_error) &&
                transformed.find("vp.store") != std::string::npos &&
                transformed.find("loop.then:") == std::string::npos;
            error = "injected rotated diamond rejection";
            return false;
        };
    RemarkLog rollback_remarks;
    auto rollback_result = LoopVectorizer(rollback_options).run(
        rollback.module, rvv_profile(), rollback_remarks);
    REQUIRE(!rollback_result.success && !rollback_result.changed);
    REQUIRE(saw_verified_rotated_diamond);
    REQUIRE(rollback.module.print() == rollback_before);
    REQUIRE(rollback_remarks.empty());
    require_verified(rollback.module);
}

void test_rotated_then_only_mask_boundary_model_never_touches_inactive_lanes() {
    constexpr int vlmax = 4;
    const int trip_counts[] = {0, 1, vlmax - 1, vlmax, vlmax + 1};
    enum class Pattern { AllTrue, AllFalse, Sparse };
    const Pattern patterns[] = {Pattern::AllTrue, Pattern::AllFalse,
                                Pattern::Sparse};
    for (auto pattern : patterns) {
        for (int trip_count : trip_counts) {
            std::vector<int> scalar(vlmax + 2, -1);
            std::vector<int> vectorized(vlmax + 2, -1);
            std::vector<int> scalar_then_touched;
            std::vector<int> vector_then_touched;
            auto lane_condition = [&](int lane) {
                if (pattern == Pattern::AllTrue) {
                    return true;
                }
                if (pattern == Pattern::AllFalse) {
                    return false;
                }
                return (lane % 4) == 0;
            };
            for (int lane = 0; lane < trip_count; ++lane) {
                scalar[lane] = lane;
                if (lane_condition(lane)) {
                    scalar[lane] = 4;
                    scalar_then_touched.push_back(lane);
                }
            }

            int remaining = trip_count;
            int iv = 0;
            while (remaining != 0) {
                const int actual_vl = std::min(remaining, vlmax);
                for (int lane = 0; lane < vlmax; ++lane) {
                    const bool active = lane < actual_vl;
                    const bool then_mask = active && lane_condition(iv + lane);
                    if (active) {
                        vectorized[iv + lane] = iv + lane;
                    }
                    if (then_mask) {
                        REQUIRE(iv + lane < trip_count);
                        vectorized[iv + lane] = 4;
                        vector_then_touched.push_back(iv + lane);
                    }
                }
                iv += actual_vl;
                remaining -= actual_vl;
            }
            REQUIRE(vectorized == scalar);
            REQUIRE(vector_then_touched == scalar_then_touched);
            REQUIRE(iv == trip_count);
        }
    }
}

void test_disabled_non_vector_and_alias_controls() {
    IntLoopFixture disabled_fixture;
    const auto disabled_before = disabled_fixture.module.print();
    RemarkLog disabled_remarks;
    LoopVectorizerOptions disabled;
    disabled.enabled = false;
    disabled.force = true;
    auto disabled_result = LoopVectorizer(disabled).run(
        disabled_fixture.module, rvv_profile(), disabled_remarks);
    REQUIRE(disabled_result.success && !disabled_result.changed);
    REQUIRE(disabled_fixture.module.print() == disabled_before);
    REQUIRE(disabled_remarks.empty());

    IntLoopFixture scalar_target_fixture;
    const auto scalar_before = scalar_target_fixture.module.print();
    RemarkLog scalar_remarks;
    LoopVectorizerOptions forced;
    forced.force = true;
    auto scalar_result = LoopVectorizer(forced).run(
        scalar_target_fixture.module, scalar_profile(), scalar_remarks);
    REQUIRE(scalar_result.success && !scalar_result.changed);
    REQUIRE(single_remark(scalar_remarks).code == RemarkCode::RejectTargetFeature);
    REQUIRE(scalar_target_fixture.module.print() == scalar_before);

    IntLoopOptions alias_options;
    alias_options.pointer_arguments = true;
    IntLoopFixture alias_fixture(alias_options);
    const auto alias_before = alias_fixture.module.print();
    RemarkLog alias_remarks;
    auto alias_result = LoopVectorizer(forced).run(
        alias_fixture.module, rvv_profile(), alias_remarks);
    REQUIRE(alias_result.success && alias_result.changed);
    REQUIRE(single_remark(alias_remarks).code == RemarkCode::Vectorized);
    REQUIRE(single_remark(alias_remarks).plan.requires_runtime_alias_check);
    REQUIRE(single_remark(alias_remarks).explanation.find("overflow-safe runtime alias versioning") !=
            std::string::npos);
    const auto alias_after = alias_fixture.module.print();
    REQUIRE(alias_after != alias_before);
    REQUIRE(alias_after.find("call i32 @__yoolang_ranges_disjoint") != std::string::npos);
    REQUIRE(alias_after.find("lv.alias.fast") != std::string::npos);
    REQUIRE(alias_after.find("lv.alias.slow") != std::string::npos);
    REQUIRE(alias_after.find("lv.slow.loop.header") != std::string::npos);
    REQUIRE(alias_after.find("vp.load") != std::string::npos);
    REQUIRE(alias_after.find("vp.store") != std::string::npos);
    require_roundtrip(alias_fixture.module);

    IntLoopFixture legality_fixture(alias_options);
    oir::DominatorTree alias_dominators(*legality_fixture.function);
    oir::LoopInfo alias_loops(*legality_fixture.function, alias_dominators);
    REQUIRE(alias_loops.loops().size() == 1);
    oir::ScalarEvolution alias_scev(*legality_fixture.function, alias_loops);
    oir::OIRAliasAnalysis alias_analysis;
    pass::oir_vectorize::LegalityOptions available_versioning;
    available_versioning.allow_runtime_alias_checks = true;
    auto alias_legality = pass::oir_vectorize::LoopVectorizationLegality(
                              available_versioning)
                              .analyze(*legality_fixture.function,
                                       alias_loops.loops().front(), alias_loops,
                                       alias_scev, alias_analysis);
    REQUIRE(alias_legality.legal);
    REQUIRE(alias_legality.memory.requires_runtime_alias_check);
    REQUIRE(alias_legality.memory.may_alias_pairs.size() == 1);

    IntLoopFixture unavailable_fixture(alias_options);
    const auto unavailable_before = unavailable_fixture.module.print();
    LoopVectorizerOptions unavailable = forced;
    unavailable.enable_runtime_alias_versioning = false;
    RemarkLog unavailable_remarks;
    auto unavailable_result = LoopVectorizer(unavailable).run(
        unavailable_fixture.module, rvv_profile(), unavailable_remarks);
    REQUIRE(unavailable_result.success && !unavailable_result.changed);
    REQUIRE(single_remark(unavailable_remarks).code == RemarkCode::RejectAlias);
    REQUIRE(unavailable_fixture.module.print() == unavailable_before);
}

void test_runtime_alias_versioning_is_transactional_and_incomplete_ranges_fail_closed() {
    IntLoopOptions alias_options;
    alias_options.pointer_arguments = true;

    IntLoopFixture rollback(alias_options);
    const auto rollback_before = rollback.module.print();
    bool saw_complete_version = false;
    LoopVectorizerOptions rollback_options;
    rollback_options.force = true;
    rollback_options.post_transform_validation =
        [&](const oir::Module &module, std::string &error) {
            std::string verify_error;
            const auto text = module.print();
            saw_complete_version =
                module.verify(&verify_error) &&
                text.find("call i32 @__yoolang_ranges_disjoint") != std::string::npos &&
                text.find("lv.slow.loop.header") != std::string::npos &&
                text.find("vp.store") != std::string::npos;
            error = "injected alias-version rejection";
            return false;
        };
    RemarkLog rollback_remarks;
    auto rollback_result = LoopVectorizer(rollback_options).run(
        rollback.module, rvv_profile(), rollback_remarks);
    REQUIRE(!rollback_result.success && !rollback_result.changed);
    REQUIRE(saw_complete_version);
    REQUIRE(rollback.module.print() == rollback_before);
    REQUIRE(rollback_remarks.empty());
    require_verified(rollback.module);

    {
        IntLoopOptions liveout_options;
        liveout_options.pointer_arguments = true;
        liveout_options.exit_phi = true;
        IntLoopFixture liveout(liveout_options);
        LoopVectorizerOptions liveout_forced;
        liveout_forced.force = true;
        RemarkLog liveout_remarks;
        auto liveout_result = LoopVectorizer(liveout_forced).run(
            liveout.module, rvv_profile(), liveout_remarks);
        REQUIRE(liveout_result.success && liveout_result.changed);
        REQUIRE(single_remark(liveout_remarks).code == RemarkCode::Vectorized);

        auto *merged_liveout = dynamic_cast<oir::PhiInst *>(
            liveout.exit->instructions().front().get());
        REQUIRE(merged_liveout != nullptr);
        REQUIRE(merged_liveout->incoming().size() == 2);
        REQUIRE(incoming_for(*merged_liveout, liveout.header) == liveout.iv);
        bool saw_scalar_liveout = false;
        for (const auto &[value, block] : merged_liveout->incoming()) {
            if (block->name().find("lv.slow.loop.header") != std::string::npos) {
                REQUIRE(value->name().find("iv.lv.slow") != std::string::npos);
                saw_scalar_liveout = true;
            }
        }
        REQUIRE(saw_scalar_liveout);
        require_verified(liveout.module);
        require_roundtrip(liveout.module);
    }

    IntLoopOptions incomplete_options;
    incomplete_options.pointer_arguments = true;
    incomplete_options.store_offset = 1;
    IntLoopFixture incomplete(incomplete_options);
    const auto incomplete_before = incomplete.module.print();
    LoopVectorizerOptions forced;
    forced.force = true;
    RemarkLog incomplete_remarks;
    auto incomplete_result = LoopVectorizer(forced).run(
        incomplete.module, rvv_profile(), incomplete_remarks);
    REQUIRE(incomplete_result.success && !incomplete_result.changed);
    REQUIRE(single_remark(incomplete_remarks).code == RemarkCode::RejectAlias);
    REQUIRE(single_remark(incomplete_remarks).explanation.find("complete overflow-safe") !=
            std::string::npos);
    REQUIRE(incomplete.module.print() == incomplete_before);
    REQUIRE(incomplete.module.get_function("__yoolang_ranges_disjoint") == nullptr);
    require_verified(incomplete.module);
}

void test_stable_rejection_matrix_never_reports_success() {
    IntLoopOptions call;
    call.call = true;
    require_rejected(call, RemarkCode::RejectCall);

    IntLoopOptions trap;
    trap.unsafe_division = true;
    require_rejected(trap, RemarkCode::RejectPotentialTrap);

    IntLoopOptions reduction;
    reduction.reduction = true;
    require_rejected(reduction, RemarkCode::RejectReduction);

    IntLoopOptions stride;
    stride.induction_stride = 3;
    require_rejected(stride, RemarkCode::RejectStride);

    IntLoopOptions early_exit;
    early_exit.early_exit = true;
    require_rejected(early_exit, RemarkCode::RejectEarlyExit);

    IntegerReductionFixture unsupported(oir::Instruction::OpID::Sub);
    const auto unsupported_before = unsupported.module.print();
    RemarkLog unsupported_remarks;
    LoopVectorizerOptions forced;
    forced.force = true;
    auto unsupported_result = LoopVectorizer(forced).run(
        unsupported.module, rvv_profile(), unsupported_remarks);
    REQUIRE(unsupported_result.success && !unsupported_result.changed);
    REQUIRE(single_remark(unsupported_remarks).code == RemarkCode::RejectReduction);
    REQUIRE(unsupported.module.print() == unsupported_before);

    FloatReductionFixture strict_float;
    const auto before = strict_float.module.print();
    RemarkLog strict_remarks;
    LoopVectorizerOptions strict;
    strict.force = true;
    strict.strict_floating_point = true;
    auto result = LoopVectorizer(strict).run(strict_float.module, rvv_profile(), strict_remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(single_remark(strict_remarks).code == RemarkCode::RejectFPOrder);
    REQUIRE(!single_remark(strict_remarks).succeeded());
    REQUIRE(strict_float.module.print() == before);
}

std::int64_t positive_scaled_index_stride(const oir::Value *indices) {
    const auto *scaled = dynamic_cast<const oir::VPBinaryInst *>(indices);
    REQUIRE(scaled != nullptr);
    REQUIRE(scaled->binary_op() == oir::Instruction::OpID::Mul);
    const oir::SplatInst *splat = dynamic_cast<const oir::SplatInst *>(scaled->lhs());
    const oir::StepVectorInst *step =
        dynamic_cast<const oir::StepVectorInst *>(scaled->rhs());
    if (splat == nullptr || step == nullptr) {
        splat = dynamic_cast<const oir::SplatInst *>(scaled->rhs());
        step = dynamic_cast<const oir::StepVectorInst *>(scaled->lhs());
    }
    REQUIRE(splat != nullptr && step != nullptr);
    const auto *constant = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
    REQUIRE(constant != nullptr);
    REQUIRE(constant->value() > 0);
    return constant->value();
}

const oir::BinaryInst *require_last_active_lane(const oir::Value *value,
                                                const oir::SetVLInst *setvl) {
    const auto *last = dynamic_cast<const oir::BinaryInst *>(value);
    REQUIRE(last != nullptr);
    REQUIRE(last->op() == oir::Instruction::OpID::Sub);
    REQUIRE(last->lhs() == setvl);
    const auto *one = dynamic_cast<const oir::ConstantInt *>(last->rhs());
    REQUIRE(one != nullptr && one->value() == 1);
    return last;
}

void require_no_negative_index_splat(const oir::Value *value) {
    if (const auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
        if (const auto *constant =
                dynamic_cast<const oir::ConstantInt *>(splat->scalar())) {
            REQUIRE(constant->value() >= 0);
        }
        return;
    }
    if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
        require_no_negative_index_splat(binary->lhs());
        require_no_negative_index_splat(binary->rhs());
    }
}

const oir::BinaryInst *require_reverse_index_recipe(
    const oir::Value *indices, const oir::SetVLInst *setvl,
    std::int64_t expected_stride) {
    REQUIRE(expected_stride < 0);
    const auto *scaled = dynamic_cast<const oir::VPBinaryInst *>(indices);
    REQUIRE(scaled != nullptr);
    REQUIRE(scaled->binary_op() == oir::Instruction::OpID::Mul);
    REQUIRE(scaled->evl() == setvl);

    const oir::SplatInst *magnitude =
        dynamic_cast<const oir::SplatInst *>(scaled->lhs());
    const oir::VPBinaryInst *reverse =
        dynamic_cast<const oir::VPBinaryInst *>(scaled->rhs());
    if (magnitude == nullptr || reverse == nullptr) {
        magnitude = dynamic_cast<const oir::SplatInst *>(scaled->rhs());
        reverse = dynamic_cast<const oir::VPBinaryInst *>(scaled->lhs());
    }
    REQUIRE(magnitude != nullptr && reverse != nullptr);
    const auto *scale =
        dynamic_cast<const oir::ConstantInt *>(magnitude->scalar());
    REQUIRE(scale != nullptr && scale->value() == -expected_stride);
    REQUIRE(scale->value() > 0);

    REQUIRE(reverse->binary_op() == oir::Instruction::OpID::Sub);
    REQUIRE(reverse->evl() == setvl);
    REQUIRE(reverse->active_mask() == scaled->active_mask());
    const auto *last_splat =
        dynamic_cast<const oir::SplatInst *>(reverse->lhs());
    const auto *step =
        dynamic_cast<const oir::StepVectorInst *>(reverse->rhs());
    REQUIRE(last_splat != nullptr && step != nullptr);
    const auto *last = require_last_active_lane(last_splat->scalar(), setvl);
    require_no_negative_index_splat(indices);
    return last;
}

void require_reverse_chunk_low_base(const oir::Value *base,
                                    const oir::SetVLInst *setvl,
                                    const oir::BinaryInst *last,
                                    std::int64_t expected_stride) {
    REQUIRE(expected_stride < 0);
    const auto *biased = dynamic_cast<const oir::GetElementPtrInst *>(base);
    REQUIRE(biased != nullptr);
    REQUIRE(biased->name().find("chunk.low.base") != std::string::npos);
    const auto indices = biased->indices();
    REQUIRE(indices.size() == 1);
    const auto *delta = dynamic_cast<const oir::BinaryInst *>(indices.front());
    REQUIRE(delta != nullptr);
    REQUIRE(delta->op() == oir::Instruction::OpID::Mul);
    const oir::ConstantInt *stride = nullptr;
    if (delta->lhs() == last) {
        stride = dynamic_cast<const oir::ConstantInt *>(delta->rhs());
    } else if (delta->rhs() == last) {
        stride = dynamic_cast<const oir::ConstantInt *>(delta->lhs());
    }
    REQUIRE(stride != nullptr && stride->value() == expected_stride);
    REQUIRE(require_last_active_lane(last, setvl) == last);
}

oir::SetVLInst *find_setvl(oir::Function &function) {
    oir::SetVLInst *result = nullptr;
    for (auto &block : function.blocks()) {
        for (auto &instruction : block->instructions()) {
            if (auto *candidate = dynamic_cast<oir::SetVLInst *>(instruction.get())) {
                REQUIRE(result == nullptr);
                result = candidate;
            }
        }
    }
    REQUIRE(result != nullptr);
    return result;
}

void require_scalar_delta(const oir::Value *delta, const oir::SetVLInst *setvl,
                          std::int64_t expected_scale) {
    if (expected_scale == 1) {
        REQUIRE(delta == setvl);
        return;
    }
    const auto *scaled = dynamic_cast<const oir::BinaryInst *>(delta);
    REQUIRE(scaled != nullptr);
    REQUIRE(scaled->op() == oir::Instruction::OpID::Mul);
    const oir::ConstantInt *constant = nullptr;
    if (scaled->lhs() == setvl) {
        constant = dynamic_cast<const oir::ConstantInt *>(scaled->rhs());
    } else if (scaled->rhs() == setvl) {
        constant = dynamic_cast<const oir::ConstantInt *>(scaled->lhs());
    }
    REQUIRE(constant != nullptr);
    REQUIRE(constant->value() == expected_scale);
}

void test_signed_array_strides_select_contiguous_or_indexed_memory() {
    const int strides[] = {1, 2, 4, -1, -2, -4};
    for (const int stride : strides) {
        IntLoopOptions fixture_options;
        fixture_options.induction_stride = stride;
        IntLoopFixture fixture(fixture_options);
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result =
            LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
        REQUIRE(single_remark(remarks).succeeded());

        auto *setvl = find_setvl(*fixture.function);
        const oir::VPGatherInst *gather = nullptr;
        const oir::VPScatterInst *scatter = nullptr;
        unsigned contiguous_loads = 0;
        unsigned contiguous_stores = 0;
        for (const auto &block : fixture.function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                if (const auto *candidate =
                        dynamic_cast<const oir::VPGatherInst *>(instruction.get())) {
                    REQUIRE(gather == nullptr);
                    gather = candidate;
                }
                if (const auto *candidate =
                        dynamic_cast<const oir::VPScatterInst *>(instruction.get())) {
                    REQUIRE(scatter == nullptr);
                    scatter = candidate;
                }
                contiguous_loads +=
                    dynamic_cast<const oir::VPLoadInst *>(instruction.get()) != nullptr;
                contiguous_stores +=
                    dynamic_cast<const oir::VPStoreInst *>(instruction.get()) != nullptr;
            }
        }
        if (stride == 1) {
            REQUIRE(gather == nullptr && scatter == nullptr);
            REQUIRE(contiguous_loads == 1 && contiguous_stores == 1);
        } else {
            REQUIRE(gather != nullptr && scatter != nullptr);
            REQUIRE(contiguous_loads == 0 && contiguous_stores == 0);
            REQUIRE(gather->indices() == scatter->indices());
            REQUIRE(gather->evl() == setvl && scatter->evl() == setvl);
            if (stride > 0) {
                // The forward +2/+4 recipe remains stepvector * stride with
                // the original scalar lane-zero base.
                REQUIRE(positive_scaled_index_stride(gather->indices()) ==
                        stride);
                REQUIRE(gather->base_ptr()->name().find("chunk.low.base") ==
                        std::string::npos);
                REQUIRE(scatter->base_ptr()->name().find("chunk.low.base") ==
                        std::string::npos);
            } else {
                const auto *last = require_reverse_index_recipe(
                    gather->indices(), setvl, stride);
                require_reverse_chunk_low_base(gather->base_ptr(), setvl, last,
                                               stride);
                require_reverse_chunk_low_base(scatter->base_ptr(), setvl, last,
                                               stride);
                REQUIRE(single_remark(remarks).explanation.find(
                            "nonnegative reverse-memory indices") !=
                        std::string::npos);
            }
        }

        oir::Value *iv_next = nullptr;
        for (const auto &[incoming, block] : fixture.iv->incoming()) {
            if (block == fixture.body) {
                iv_next = incoming;
            }
        }
        const auto *update = dynamic_cast<const oir::BinaryInst *>(iv_next);
        REQUIRE(update != nullptr && update->lhs() == fixture.iv);
        REQUIRE(update->op() == (stride > 0 ? oir::Instruction::OpID::Add
                                            : oir::Instruction::OpID::Sub));
        require_scalar_delta(update->rhs(), setvl,
                             stride < 0 ? -stride : stride);
        require_roundtrip(fixture.module);
    }

    // Address strength can be independent of the scalar IV recurrence: i++
    // with a[i*2] still needs indexed memory while IV advances by actual VL.
    IntLoopOptions scaled_address_options;
    scaled_address_options.stride_two = true;
    IntLoopFixture scaled_address(scaled_address_options);
    RemarkLog remarks;
    LoopVectorizerOptions forced;
    forced.force = true;
    auto result = LoopVectorizer(forced).run(
        scaled_address.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    const oir::VPGatherInst *gather = nullptr;
    for (const auto &block : scaled_address.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (const auto *candidate =
                    dynamic_cast<const oir::VPGatherInst *>(instruction.get())) {
                gather = candidate;
            }
        }
    }
    REQUIRE(gather != nullptr);
    REQUIRE(positive_scaled_index_stride(gather->indices()) == 2);
    require_roundtrip(scaled_address.module);

    FloatLoopFixture reverse_float(-2);
    RemarkLog float_remarks;
    auto float_result = LoopVectorizer(forced).run(
        reverse_float.module, rvv_profile(), float_remarks);
    REQUIRE(float_result.success && float_result.changed);
    bool saw_float_gather = false;
    bool saw_float_scatter = false;
    bool saw_float_add = false;
    for (const auto &block : reverse_float.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (const auto *candidate =
                    dynamic_cast<const oir::VPGatherInst *>(instruction.get())) {
                const auto *type =
                    dynamic_cast<const oir::VectorType *>(candidate->type());
                saw_float_gather = type != nullptr &&
                                   type->element_type() ==
                                       reverse_float.module.types().float_ty();
                if (saw_float_gather) {
                    const auto *last = require_reverse_index_recipe(
                        candidate->indices(), find_setvl(*reverse_float.function),
                        -2);
                    require_reverse_chunk_low_base(
                        candidate->base_ptr(),
                        find_setvl(*reverse_float.function), last, -2);
                }
            }
            saw_float_scatter |=
                dynamic_cast<const oir::VPScatterInst *>(instruction.get()) != nullptr;
            if (const auto *binary =
                    dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
                saw_float_add |=
                    binary->binary_op() == oir::Instruction::OpID::FAdd;
            }
        }
    }
    REQUIRE(saw_float_gather && saw_float_scatter && saw_float_add);
    require_roundtrip(reverse_float.module);
}

void test_rotated_reverse_strides_preserve_vla_address_order() {
    constexpr std::int32_t vlmax = 4;
    const int strides[] = {-1, -2, -4};
    const std::int32_t trip_counts[] = {0, 1, vlmax - 1, vlmax,
                                        vlmax + 1, 17};
    for (const int stride : strides) {
        RotatedReverseLoopFixture fixture(stride);
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result = LoopVectorizer(options).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
        REQUIRE(single_remark(remarks).succeeded());
        REQUIRE(single_remark(remarks).explanation.find(
                    "nonnegative reverse-memory indices") != std::string::npos);

        auto *setvl = find_setvl(*fixture.function);
        const oir::VPGatherInst *gather = nullptr;
        const oir::VPScatterInst *scatter = nullptr;
        for (const auto &instruction : fixture.body->instructions()) {
            if (const auto *candidate =
                    dynamic_cast<const oir::VPGatherInst *>(instruction.get())) {
                REQUIRE(gather == nullptr);
                gather = candidate;
            }
            if (const auto *candidate =
                    dynamic_cast<const oir::VPScatterInst *>(instruction.get())) {
                REQUIRE(scatter == nullptr);
                scatter = candidate;
            }
        }
        REQUIRE(gather != nullptr && scatter != nullptr);
        REQUIRE(gather->indices() == scatter->indices());
        const auto *last = require_reverse_index_recipe(
            gather->indices(), setvl, stride);
        require_reverse_chunk_low_base(gather->base_ptr(), setvl, last, stride);
        require_reverse_chunk_low_base(scatter->base_ptr(), setvl, last, stride);

        const auto *new_next = dynamic_cast<const oir::BinaryInst *>(
            incoming_for(*fixture.iv, fixture.body));
        REQUIRE(new_next != nullptr);
        REQUIRE(new_next->op() == oir::Instruction::OpID::Sub);
        REQUIRE(new_next->lhs() == fixture.iv);
        require_scalar_delta(new_next->rhs(), setvl, -stride);
        const auto *body_branch =
            dynamic_cast<const oir::BranchInst *>(fixture.body->terminator());
        REQUIRE(body_branch != nullptr && body_branch->is_conditional());
        const auto *remaining_test =
            dynamic_cast<const oir::CmpInst *>(body_branch->cond());
        REQUIRE(remaining_test != nullptr &&
                remaining_test->pred() == oir::CmpPred::NE);
        require_roundtrip(fixture.module);

        const auto magnitude = static_cast<std::int32_t>(-stride);
        for (auto trip_count : trip_counts) {
            std::vector<std::int32_t> scalar_addresses;
            for (std::int32_t index = trip_count - 1; index >= 0;
                 index -= magnitude) {
                scalar_addresses.push_back(index);
            }

            std::vector<std::int32_t> vector_addresses;
            std::int32_t remaining =
                trip_count <= 0 ? 0 : (trip_count + magnitude - 1) / magnitude;
            std::int32_t current = trip_count - 1;
            while (remaining != 0) {
                const auto actual_vl = std::min(remaining, vlmax);
                const auto low = current + (actual_vl - 1) * stride;
                for (std::int32_t lane = 0; lane < actual_vl; ++lane) {
                    const auto offset = (actual_vl - 1 - lane) * magnitude;
                    REQUIRE(offset >= 0);
                    vector_addresses.push_back(low + offset);
                }
                current -= actual_vl * magnitude;
                remaining -= actual_vl;
            }
            REQUIRE(vector_addresses == scalar_addresses);
        }
    }
}

void test_signed_pointer_inductions_scale_actual_vl() {
    const int strides[] = {1, 2, 4, -1, -2, -4};
    for (const int stride : strides) {
        IntLoopOptions fixture_options;
        fixture_options.pointer_induction = true;
        fixture_options.induction_stride = stride;
        IntLoopFixture fixture(fixture_options);
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result =
            LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed);
        auto *setvl = find_setvl(*fixture.function);

        const oir::PhiInst *pointer_phi = nullptr;
        for (const auto &instruction : fixture.header->instructions()) {
            if (const auto *phi =
                    dynamic_cast<const oir::PhiInst *>(instruction.get());
                phi != nullptr && phi->type()->is_pointer()) {
                pointer_phi = phi;
            }
        }
        REQUIRE(pointer_phi != nullptr);
        const oir::Value *pointer_next = nullptr;
        for (const auto &[incoming, block] : pointer_phi->incoming()) {
            if (block == fixture.body) {
                pointer_next = incoming;
            }
        }
        const auto *gep =
            dynamic_cast<const oir::GetElementPtrInst *>(pointer_next);
        REQUIRE(gep != nullptr);
        const auto indices = gep->indices();
        REQUIRE(indices.size() == 1);
        require_scalar_delta(indices.front(), setvl, stride);

        bool saw_expected_memory = false;
        for (const auto &instruction : fixture.body->instructions()) {
            if (stride == 1) {
                saw_expected_memory |=
                    dynamic_cast<const oir::VPLoadInst *>(instruction.get()) != nullptr;
            } else if (const auto *gather =
                           dynamic_cast<const oir::VPGatherInst *>(instruction.get())) {
                if (stride > 0) {
                    REQUIRE(positive_scaled_index_stride(gather->indices()) ==
                            stride);
                    REQUIRE(gather->base_ptr() == pointer_phi);
                } else {
                    const auto *last = require_reverse_index_recipe(
                        gather->indices(), setvl, stride);
                    require_reverse_chunk_low_base(gather->base_ptr(), setvl,
                                                   last, stride);
                    const auto *biased =
                        dynamic_cast<const oir::GetElementPtrInst *>(
                            gather->base_ptr());
                    REQUIRE(biased != nullptr &&
                            biased->base_ptr() == pointer_phi);
                }
                saw_expected_memory = true;
            }
        }
        REQUIRE(saw_expected_memory);
        require_roundtrip(fixture.module);
    }
}

std::vector<int> scalar_stride_addresses(int n, int stride) {
    std::vector<int> result;
    if (stride > 0) {
        for (int index = 0; index < n; index += stride) {
            result.push_back(index);
        }
    } else {
        for (int index = n - 1; index >= 0; index += stride) {
            result.push_back(index);
        }
    }
    return result;
}

std::vector<int> indexed_chunk_addresses(int lane_zero_base, int stride,
                                         int actual_vl) {
    std::vector<int> result;
    if (actual_vl <= 0) {
        // EVL zero must not produce an indexed memory access.
        return result;
    }
    if (stride > 0) {
        for (int lane = 0; lane < actual_vl; ++lane) {
            result.push_back(lane_zero_base + lane * stride);
        }
        return result;
    }

    const int magnitude = -stride;
    const int last_lane = actual_vl - 1;
    const int chunk_low_base = lane_zero_base + last_lane * stride;
    std::vector<int> indices;
    for (int lane = 0; lane < actual_vl; ++lane) {
        const int index = (last_lane - lane) * magnitude;
        REQUIRE(index >= 0);
        indices.push_back(index);
        result.push_back(chunk_low_base + index);
    }
    auto unique = indices;
    std::sort(unique.begin(), unique.end());
    REQUIRE(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
    return result;
}

std::vector<int> vla_stride_addresses(int n, int stride, int vlmax) {
    std::vector<int> result;
    const int magnitude = stride < 0 ? -stride : stride;
    int remaining = n > 0 ? (n / magnitude + (n % magnitude != 0)) : 0;
    int base = stride > 0 ? 0 : n - 1;
    while (remaining != 0) {
        const int actual_vl = std::min(remaining, vlmax);
        const auto chunk = indexed_chunk_addresses(base, stride, actual_vl);
        result.insert(result.end(), chunk.begin(), chunk.end());
        base += actual_vl * stride;
        remaining -= actual_vl;
    }
    return result;
}

void test_signed_stride_vla_boundary_address_sequences() {
    constexpr int vlmax = 4;
    const int bounds[] = {0, 1, vlmax - 1, vlmax, vlmax + 1};
    const int strides[] = {1, 2, 4, -1, -2, -4};
    for (const int stride : strides) {
        for (const int n : bounds) {
            const auto scalar = scalar_stride_addresses(n, stride);
            const auto vectorized = vla_stride_addresses(n, stride, vlmax);
            REQUIRE(vectorized == scalar);
            for (const int address : vectorized) {
                REQUIRE(address >= 0 && address < n);
            }
        }
    }
    REQUIRE(vla_stride_addresses(9, -2, vlmax) ==
            std::vector<int>({8, 6, 4, 2, 0}));
    REQUIRE(vla_stride_addresses(10, -4, vlmax) ==
            std::vector<int>({9, 5, 1}));
    REQUIRE(indexed_chunk_addresses(123, -1, 0).empty());
    REQUIRE(indexed_chunk_addresses(123, -2, 0).empty());
    REQUIRE(indexed_chunk_addresses(123, -4, 0).empty());

    // Match the transform's deliberately conservative proof: even the
    // largest real minimum lane count (e32,m8 at VLEN=65536) multiplied by
    // the maximum architectural scale from a 32-bit minimum stays in i32 for
    // every admitted reverse stride.
    constexpr std::int64_t maximum_minimum_lanes = 16384;
    constexpr std::int64_t maximum_vscale = 65536 / 32;
    constexpr std::int64_t maximum_lane =
        maximum_minimum_lanes * maximum_vscale - 1;
    static_assert(maximum_lane * 4 * sizeof(std::int32_t) <=
                  std::numeric_limits<std::int32_t>::max());
}

void test_reverse_index_i32_proof_fails_closed() {
    IntLoopOptions fixture_options;
    fixture_options.induction_stride = -4;
    IntLoopFixture fixture(fixture_options);
    const auto before = fixture.module.print();
    oir::DominatorTree dominators(*fixture.function);
    oir::LoopInfo loops(*fixture.function, dominators);
    REQUIRE(loops.loops().size() == 1);
    oir::ScalarEvolution scev(*fixture.function, loops);
    oir::OIRAliasAnalysis alias_analysis;
    auto legality = pass::oir_vectorize::LoopVectorizationLegality{}.analyze(
        *fixture.function, loops.loops().front(), loops, scev, alias_analysis);
    REQUIRE(legality.legal);

    pass::oir_vectorize::CostModelOptions forced_options;
    forced_options.force = true;
    auto cost = pass::oir_vectorize::RVVCostModel(rvv_profile(), forced_options)
                    .choose(legality);
    REQUIRE(cost.profitable);
    auto valid_plan = pass::oir_vectorize::VectorizationPlanner{}.build(
        *fixture.function, loops.loops().front(), legality, cost.choice);
    REQUIRE(valid_plan.valid);
    REQUIRE(valid_plan.explanation.find("i32-proven nonnegative") !=
            std::string::npos);
    unsigned reverse_index_recipes = 0;
    for (const auto &recipe : valid_plan.plan.recipes) {
        if (recipe.kind == pass::oir_vectorize::RecipeKind::StridedIndex) {
            ++reverse_index_recipes;
            REQUIRE(recipe.explanation.find("chunk-low base") !=
                    std::string::npos);
            REQUIRE(recipe.explanation.find("injective nonnegative") !=
                    std::string::npos);
        }
    }
    REQUIRE(reverse_index_recipes == 2);

    pass::oir_vectorize::PlanChoice impossible_choice;
    impossible_choice.scalable = true;
    impossible_choice.minimum_lanes = std::numeric_limits<unsigned>::max();
    impossible_choice.lmul = "m8";
    auto plan = pass::oir_vectorize::VectorizationPlanner{}.build(
        *fixture.function, loops.loops().front(), legality, impossible_choice);
    REQUIRE(!plan.valid);
    REQUIRE(plan.code == RemarkCode::RejectStride);
    REQUIRE(plan.explanation.find("fits i32") != std::string::npos);
    REQUIRE(fixture.module.print() == before);
    require_verified(fixture.module);
}

void test_overlap_alias_and_indexed_profitability_are_conservative() {
    IntLoopOptions full_options;
    full_options.same_object = true;
    full_options.induction_stride = 2;
    IntLoopFixture full(full_options);
    RemarkLog full_remarks;
    LoopVectorizerOptions forced;
    forced.force = true;
    auto full_result =
        LoopVectorizer(forced).run(full.module, rvv_profile(), full_remarks);
    REQUIRE(full_result.success && full_result.changed);
    REQUIRE(single_remark(full_remarks).succeeded());
    require_roundtrip(full.module);

    IntLoopOptions partial_options;
    partial_options.same_object = true;
    partial_options.induction_stride = 2;
    partial_options.store_offset = 1;
    IntLoopFixture partial(partial_options);
    const auto partial_before = partial.module.print();
    RemarkLog partial_remarks;
    auto partial_result =
        LoopVectorizer(forced).run(partial.module, rvv_profile(), partial_remarks);
    REQUIRE(partial_result.success && !partial_result.changed);
    REQUIRE(single_remark(partial_remarks).code == RemarkCode::RejectDependence);
    REQUIRE(partial.module.print() == partial_before);
    require_verified(partial.module);

    IntLoopOptions indexed_options;
    indexed_options.stride_two = true;
    IntLoopFixture expensive(indexed_options);
    auto expensive_target = rvv_profile();
    expensive_target.tuning.vector_alu_cost = 100;
    expensive_target.tuning.vector_load_cost = 100;
    expensive_target.tuning.vector_store_cost = 100;
    RemarkLog expensive_remarks;
    LoopVectorizerOptions normal;
    normal.expected_trip_count = 64;
    auto expensive_result = LoopVectorizer(normal).run(
        expensive.module, expensive_target, expensive_remarks);
    REQUIRE(expensive_result.success && !expensive_result.changed);
    REQUIRE(single_remark(expensive_remarks).code == RemarkCode::RejectCost);
    REQUIRE(!single_remark(expensive_remarks).succeeded());

    IntLoopFixture forced_expensive(indexed_options);
    RemarkLog forced_remarks;
    normal.force = true;
    auto forced_result = LoopVectorizer(normal).run(
        forced_expensive.module, expensive_target, forced_remarks);
    REQUIRE(forced_result.success && forced_result.changed);
    REQUIRE(single_remark(forced_remarks).succeeded());
    require_verified(forced_expensive.module);
}

void test_post_verify_rejection_rolls_back_without_success_remark() {
    IntLoopOptions fixture_options;
    fixture_options.induction_stride = -2;
    IntLoopFixture fixture(fixture_options);
    const auto before = fixture.module.print();
    bool saw_verified_transformed_module = false;
    LoopVectorizerOptions options;
    options.force = true;
    options.post_transform_validation =
        [&](const oir::Module &module, std::string &error) {
            std::string verify_error;
            saw_verified_transformed_module = module.verify(&verify_error) &&
                                              module.print().find("vp.gather") !=
                                                  std::string::npos &&
                                              module.print().find(
                                                  "memory.chunk.low.base") !=
                                                  std::string::npos;
            error = "injected embedding rejection";
            return false;
        };
    RemarkLog remarks;
    auto result =
        LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(!result.success && !result.changed);
    REQUIRE(result.message.find("injected embedding rejection") != std::string::npos);
    REQUIRE(saw_verified_transformed_module);
    REQUIRE(fixture.module.print() == before);
    for (const auto &remark : remarks.remarks()) {
        REQUIRE(!remark.succeeded());
    }
    require_verified(fixture.module);
}

struct DiamondVectorState final {
    oir::SetVLInst *setvl = nullptr;
    oir::Value *active_mask = nullptr;
    oir::VPCmpInst *condition = nullptr;
    oir::VPBinaryInst *then_mask = nullptr;
    oir::VPBinaryInst *else_mask = nullptr;
    oir::VectorSelectInst *merge_select = nullptr;
    std::vector<oir::VPStoreInst *> stores;
};

DiamondVectorState inspect_diamond(DiamondLoopFixture &fixture) {
    DiamondVectorState state;
    for (const auto &instruction : fixture.condition_block->instructions()) {
        if (auto *candidate = dynamic_cast<oir::SetVLInst *>(instruction.get())) {
            REQUIRE(state.setvl == nullptr);
            state.setvl = candidate;
        }
        if (auto *splat = dynamic_cast<oir::SplatInst *>(instruction.get())) {
            const auto *one = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
            if (splat->type()->is_mask() && one != nullptr && one->value() == 1) {
                state.active_mask = splat;
            }
        }
        if (auto *compare = dynamic_cast<oir::VPCmpInst *>(instruction.get())) {
            if (compare->name().find("lane.cond") != std::string::npos) {
                state.condition = compare;
            }
        }
        if (auto *binary = dynamic_cast<oir::VPBinaryInst *>(instruction.get())) {
            if (binary->name().find("if.then.mask") != std::string::npos) {
                state.then_mask = binary;
            } else if (binary->name().find("if.else.mask") != std::string::npos) {
                state.else_mask = binary;
            }
        }
        if (auto *select = dynamic_cast<oir::VectorSelectInst *>(instruction.get())) {
            if (select->name().find("merged.ifc") != std::string::npos) {
                state.merge_select = select;
            }
        }
        if (auto *store = dynamic_cast<oir::VPStoreInst *>(instruction.get())) {
            state.stores.push_back(store);
        }
    }
    return state;
}

void require_if_masks(const DiamondVectorState &state) {
    REQUIRE(state.setvl != nullptr);
    REQUIRE(state.active_mask != nullptr);
    REQUIRE(state.condition != nullptr);
    REQUIRE(state.then_mask != nullptr);
    REQUIRE(state.else_mask != nullptr);
    REQUIRE(state.condition->type()->is_scalable_vector());
    REQUIRE(state.condition->type()->is_mask());
    const auto *active_type =
        dynamic_cast<const oir::VectorType *>(state.active_mask->type());
    const auto *condition_type =
        dynamic_cast<const oir::VectorType *>(state.condition->type());
    const auto *then_type =
        dynamic_cast<const oir::VectorType *>(state.then_mask->type());
    const auto *else_type =
        dynamic_cast<const oir::VectorType *>(state.else_mask->type());
    REQUIRE(active_type != nullptr && condition_type != nullptr &&
            then_type != nullptr && else_type != nullptr);
    REQUIRE(active_type->element_count() == condition_type->element_count());
    REQUIRE(then_type->element_count() == condition_type->element_count());
    REQUIRE(else_type->element_count() == condition_type->element_count());
    REQUIRE(condition_type->element_count().is_scalable());
    REQUIRE(state.condition->active_mask() == state.active_mask);
    REQUIRE(state.condition->evl() == state.setvl);
    REQUIRE(state.then_mask->binary_op() == oir::Instruction::OpID::And);
    REQUIRE(state.then_mask->lhs() == state.active_mask);
    REQUIRE(state.then_mask->rhs() == state.condition);
    REQUIRE(state.then_mask->active_mask() == state.active_mask);
    REQUIRE(state.then_mask->evl() == state.setvl);
    REQUIRE(state.else_mask->binary_op() == oir::Instruction::OpID::Xor);
    REQUIRE(state.else_mask->lhs() == state.active_mask);
    REQUIRE(state.else_mask->rhs() == state.then_mask);
    REQUIRE(state.else_mask->active_mask() == state.active_mask);
    REQUIRE(state.else_mask->evl() == state.setvl);
    for (const auto *value : {static_cast<const oir::VPInstruction *>(state.condition),
                              static_cast<const oir::VPInstruction *>(state.then_mask),
                              static_cast<const oir::VPInstruction *>(state.else_mask)}) {
        REQUIRE(value->has_passthrough());
        REQUIRE(value->passthrough() != nullptr);
        REQUIRE(value->tail_policy() == oir::TailPolicy::Agnostic);
        REQUIRE(value->mask_policy() == oir::MaskPolicy::Agnostic);
    }
}

void test_single_diamond_if_conversion_masks_phi_and_else() {
    DiamondLoopFixture fixture;
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    if (!result.success || !result.changed || result.loops_vectorized != 1) {
        const auto detail = remarks.empty() ? result.message
                                            : single_remark(remarks).explanation;
        throw std::runtime_error("diamond transform failed: " + detail);
    }
    REQUIRE(result.success && result.changed && result.loops_vectorized == 1);
    REQUIRE(single_remark(remarks).succeeded());

    auto state = inspect_diamond(fixture);
    require_if_masks(state);
    REQUIRE(state.merge_select != nullptr);
    REQUIRE(state.merge_select->condition() == state.then_mask);
    REQUIRE(state.merge_select->true_value()->type() == state.merge_select->type());
    REQUIRE(state.merge_select->false_value()->type() == state.merge_select->type());

    bool then_store = false;
    bool else_store = false;
    bool merge_store = false;
    for (const auto *store : state.stores) {
        REQUIRE(store->evl() == state.setvl);
        REQUIRE(store->tail_policy() == oir::TailPolicy::Agnostic);
        REQUIRE(store->mask_policy() == oir::MaskPolicy::Agnostic);
        then_store |= store->active_mask() == state.then_mask;
        else_store |= store->active_mask() == state.else_mask;
        merge_store |= store->active_mask() == state.active_mask &&
                       store->value() == state.merge_select;
    }
    REQUIRE(then_store);
    REQUIRE(else_store);
    REQUIRE(merge_store);

    bool has_then_block = false;
    bool has_else_block = false;
    bool has_latch_block = false;
    for (const auto &block : fixture.function->blocks()) {
        has_then_block |= block->name() == "loop.then";
        has_else_block |= block->name() == "loop.else";
        has_latch_block |= block->name() == "loop.latch";
    }
    REQUIRE(!has_then_block && !has_else_block && !has_latch_block);
    const auto *backedge =
        dynamic_cast<const oir::BranchInst *>(fixture.condition_block->terminator());
    REQUIRE(backedge != nullptr && !backedge->is_conditional());
    REQUIRE(backedge->target_bb() == fixture.header);
    for (const auto &instruction : fixture.condition_block->instructions()) {
        REQUIRE(dynamic_cast<const oir::LoadInst *>(instruction.get()) == nullptr);
        REQUIRE(dynamic_cast<const oir::StoreInst *>(instruction.get()) == nullptr);
        if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(instruction.get())) {
            REQUIRE(vp->evl() == state.setvl);
        }
    }
    require_roundtrip(fixture.module);
}

void test_then_only_store_is_predicated_and_never_scalarized() {
    DiamondLoopOptions fixture_options;
    fixture_options.else_arm = false;
    fixture_options.merge_phi = false;
    DiamondLoopFixture fixture(fixture_options);
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(single_remark(remarks).succeeded());

    auto state = inspect_diamond(fixture);
    require_if_masks(state);
    REQUIRE(state.merge_select == nullptr);
    REQUIRE(state.stores.size() == 1);
    REQUIRE(state.stores.front()->active_mask() == state.then_mask);
    REQUIRE(state.stores.front()->evl() == state.setvl);
    bool saw_cloned_address = false;
    for (const auto &instruction : fixture.condition_block->instructions()) {
        REQUIRE(dynamic_cast<const oir::StoreInst *>(instruction.get()) == nullptr);
        if (const auto *gep =
                dynamic_cast<const oir::GetElementPtrInst *>(instruction.get())) {
            saw_cloned_address |= gep->name().find("then.addr.ifc.addr") !=
                                  std::string::npos;
        }
    }
    REQUIRE(saw_cloned_address);
    require_roundtrip(fixture.module);
}

void test_float_diamond_preserves_lane_predicates() {
    DiamondLoopOptions fixture_options;
    fixture_options.float_lane = true;
    DiamondLoopFixture fixture(fixture_options);
    RemarkLog remarks;
    LoopVectorizerOptions options;
    options.force = true;
    options.strict_floating_point = true;
    auto result = LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(single_remark(remarks).succeeded());

    auto state = inspect_diamond(fixture);
    require_if_masks(state);
    REQUIRE(state.condition->comparison_op() == oir::Instruction::OpID::FCmp);
    REQUIRE(state.merge_select != nullptr);
    REQUIRE(state.merge_select->type()->is_scalable_vector());
    const auto *merge_type = dynamic_cast<const oir::VectorType *>(
        state.merge_select->type());
    REQUIRE(merge_type != nullptr);
    REQUIRE(merge_type->element_type() == fixture.module.types().float_ty());
    bool saw_then_float = false;
    bool saw_else_float = false;
    for (const auto &instruction : fixture.condition_block->instructions()) {
        if (const auto *binary =
                dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
            saw_then_float |= binary->binary_op() == oir::Instruction::OpID::FAdd &&
                              binary->active_mask() == state.then_mask;
            saw_else_float |= binary->binary_op() == oir::Instruction::OpID::FSub &&
                              binary->active_mask() == state.else_mask;
        }
    }
    REQUIRE(saw_then_float && saw_else_float);
    require_roundtrip(fixture.module);
}

void test_diamond_mask_chunk_model_covers_boundaries() {
    constexpr int vlmax = 4;
    const int boundaries[] = {0, 1, vlmax - 1, vlmax, vlmax + 1};
    enum class Pattern { AllTrue, AllFalse, Sparse };
    const Pattern patterns[] = {Pattern::AllTrue, Pattern::AllFalse,
                                Pattern::Sparse};
    const std::vector<int> input = {4, -7, 9, 0, 3, -1, 12, -5};
    for (auto pattern : patterns) {
        for (int trip_count : boundaries) {
            std::vector<int> scalar(input.size(), -99);
            std::vector<int> vectorized(input.size(), -99);
            std::vector<int> scalar_touched;
            std::vector<int> vector_touched;
            auto condition = [&](int lane) {
                if (pattern == Pattern::AllTrue) {
                    return true;
                }
                if (pattern == Pattern::AllFalse) {
                    return false;
                }
                return (lane % 3) != 1;
            };
            for (int lane = 0; lane < trip_count; ++lane) {
                if (condition(lane)) {
                    scalar[lane] = input[lane] + 7;
                    scalar_touched.push_back(lane);
                } else {
                    scalar[lane] = input[lane] - 3;
                }
            }
            int remaining = trip_count;
            int iv = 0;
            while (remaining != 0) {
                const int actual_vl = std::min(remaining, vlmax);
                for (int lane = 0; lane < vlmax; ++lane) {
                    const bool active = lane < actual_vl;
                    const bool lane_condition = active && condition(iv + lane);
                    const bool then_mask = active && lane_condition;
                    const bool else_mask = active && !then_mask;
                    REQUIRE(!(then_mask && else_mask));
                    REQUIRE(!active || then_mask || else_mask);
                    if (then_mask) {
                        vectorized[iv + lane] = input[iv + lane] + 7;
                        vector_touched.push_back(iv + lane);
                    } else if (else_mask) {
                        vectorized[iv + lane] = input[iv + lane] - 3;
                    }
                }
                iv += actual_vl;
                remaining -= actual_vl;
            }
            REQUIRE(vectorized == scalar);
            REQUIRE(vector_touched == scalar_touched);
            REQUIRE(iv == trip_count);
        }
    }
}

void test_diamond_rejections_are_stable_and_transactional() {
    auto require_diamond_rejected = [](DiamondLoopOptions fixture_options,
                                       RemarkCode expected) {
        DiamondLoopFixture fixture(fixture_options);
        const auto before = fixture.module.print();
        RemarkLog remarks;
        LoopVectorizerOptions options;
        options.force = true;
        auto result =
            LoopVectorizer(options).run(fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(result.loops_vectorized == 0);
        REQUIRE(single_remark(remarks).code == expected);
        REQUIRE(!single_remark(remarks).succeeded());
        REQUIRE(fixture.module.print() == before);
        require_verified(fixture.module);
    };

    DiamondLoopOptions alias;
    alias.pointer_arguments = true;
    require_diamond_rejected(alias, RemarkCode::RejectAlias);

    DiamondLoopOptions call;
    call.call_in_then = true;
    require_diamond_rejected(call, RemarkCode::RejectCall);

    DiamondLoopOptions trap;
    trap.trap_in_then = true;
    require_diamond_rejected(trap, RemarkCode::RejectPotentialTrap);
}

void test_setvl_roundtrip_and_fail_closed_verifier() {
    {
        oir::Module module("setvl-valid");
        auto &types = module.types();
        auto *function = module.create_function(
            "set", types.func_ty(types.int32_ty(), {types.int32_ty()}));
        function->args()[0]->set_name("avl");
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(
            types.scalable_vector_ty(types.int32_ty(), 3),
            function->args()[0].get(), "vl");
        builder.create_ret(vl);
        require_roundtrip(module);
    }

    auto require_bad = [](bool fixed, bool mask, bool wrong_avl, bool wrong_result,
                          const std::string &needle) {
        oir::Module module("setvl-invalid");
        auto &types = module.types();
        auto *function = module.create_function(
            "bad", types.func_ty(types.void_ty(), {}));
        auto *entry = function->create_block("entry");
        auto *element = mask ? static_cast<oir::Type *>(types.int1_ty())
                             : static_cast<oir::Type *>(types.int32_ty());
        auto *configuration = fixed
                                  ? types.fixed_vector_ty(element, 4)
                                  : types.scalable_vector_ty(element, 4);
        oir::Value *avl = wrong_avl ? static_cast<oir::Value *>(module.create_i1(true))
                                    : static_cast<oir::Value *>(module.create_i32(9));
        auto *result_type = wrong_result ? static_cast<oir::Type *>(types.float_ty())
                                         : static_cast<oir::Type *>(types.int32_ty());
        entry->append_instruction(std::make_unique<oir::SetVLInst>(
            result_type, configuration, avl, entry, "vl"));
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret();
        std::string error;
        REQUIRE(!module.verify(&error));
        REQUIRE(error.find(needle) != std::string::npos);
    };
    require_bad(true, false, false, false, "OIRV_SETVL_CONFIG");
    require_bad(false, true, false, false, "OIRV_SETVL_CONFIG");
    require_bad(false, false, true, false, "OIRV_SETVL_SHAPE");
    require_bad(false, false, false, true, "OIRV_SETVL_SHAPE");
}

void test_candidate_remark_is_not_a_success() {
    pass::oir_vectorize::Remark candidate;
    candidate.code = RemarkCode::Candidate;
    REQUIRE(!candidate.succeeded());
    candidate.code = RemarkCode::Vectorized;
    REQUIRE(candidate.succeeded());
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"unknown_trip_transforms_to_verified_vla_cfg",
         test_unknown_trip_transforms_to_verified_vla_cfg},
        {"o3_interleave_two_emits_independent_vla_chunks",
         test_o3_interleave_two_emits_independent_vla_chunks},
        {"constant_trip_and_mixed_float_casts_transform",
         test_constant_trip_and_mixed_float_casts_transform},
        {"integer_reductions_transform_and_preserve_chunk_semantics",
         test_integer_reductions_transform_and_preserve_chunk_semantics},
        {"guarded_two_block_preheader_preserves_zero_trip_reduction",
         test_guarded_two_block_preheader_preserves_zero_trip_reduction},
        {"linear_unrolled_integer_reduction_chain_and_stride_inheritance",
         test_linear_unrolled_integer_reduction_chain_and_stride_inheritance},
        {"rotated_integer_reductions_rewire_only_canonical_exit_phi",
         test_rotated_integer_reductions_rewire_only_canonical_exit_phi},
        {"actual_vl_advances_pointer_induction",
         test_actual_vl_advances_pointer_induction},
        {"rotated_single_block_loop_transforms_after_scalar_fixed_point",
         test_rotated_single_block_loop_transforms_after_scalar_fixed_point},
        {"guarded_rotated_alias_versioning_preserves_zero_trip_and_iv_liveout",
         test_guarded_rotated_alias_versioning_preserves_zero_trip_and_iv_liveout},
        {"guarded_rotated_diamond_if_converts_with_latch_backedge",
         test_guarded_rotated_diamond_if_converts_with_latch_backedge},
        {"rotated_then_only_mask_boundary_model_never_touches_inactive_lanes",
         test_rotated_then_only_mask_boundary_model_never_touches_inactive_lanes},
        {"disabled_non_vector_and_alias_controls",
         test_disabled_non_vector_and_alias_controls},
        {"runtime_alias_versioning_is_transactional_and_incomplete_ranges_fail_closed",
         test_runtime_alias_versioning_is_transactional_and_incomplete_ranges_fail_closed},
        {"stable_rejection_matrix_never_reports_success",
         test_stable_rejection_matrix_never_reports_success},
        {"signed_array_strides_select_contiguous_or_indexed_memory",
         test_signed_array_strides_select_contiguous_or_indexed_memory},
        {"rotated_reverse_strides_preserve_vla_address_order",
         test_rotated_reverse_strides_preserve_vla_address_order},
        {"signed_pointer_inductions_scale_actual_vl",
         test_signed_pointer_inductions_scale_actual_vl},
        {"signed_stride_vla_boundary_address_sequences",
         test_signed_stride_vla_boundary_address_sequences},
        {"reverse_index_i32_proof_fails_closed",
         test_reverse_index_i32_proof_fails_closed},
        {"overlap_alias_and_indexed_profitability_are_conservative",
         test_overlap_alias_and_indexed_profitability_are_conservative},
        {"post_verify_rejection_rolls_back_without_success_remark",
         test_post_verify_rejection_rolls_back_without_success_remark},
        {"single_diamond_if_conversion_masks_phi_and_else",
         test_single_diamond_if_conversion_masks_phi_and_else},
        {"then_only_store_is_predicated_and_never_scalarized",
         test_then_only_store_is_predicated_and_never_scalarized},
        {"float_diamond_preserves_lane_predicates",
         test_float_diamond_preserves_lane_predicates},
        {"diamond_mask_chunk_model_covers_boundaries",
         test_diamond_mask_chunk_model_covers_boundaries},
        {"diamond_rejections_are_stable_and_transactional",
         test_diamond_rejections_are_stable_and_transactional},
        {"setvl_roundtrip_and_fail_closed_verifier",
         test_setvl_roundtrip_and_fail_closed_verifier},
        {"candidate_remark_is_not_a_success",
         test_candidate_remark_is_not_a_success},
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
    with tempfile.TemporaryDirectory(prefix="loop-vectorizer-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "loop_vectorizer_infra_tests.cpp"
        binary = tmp_dir / "loop_vectorizer_infra_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = [
            source,
            ROOT / "src/oir/OIR.cpp",
            ROOT / "src/oir/OIRAnalysis.cpp",
            ROOT / "src/oir/OIRCFGUtils.cpp",
            ROOT / "src/oir/OIRDataLayout.cpp",
            ROOT / "src/oir/OIRParser.cpp",
            ROOT / "src/builtin/BuiltinRegistry.cpp",
            ROOT / "src/pass/oir/OIRVectorization.cpp",
            ROOT / "src/pass/oir/RVVTargetCostModel.cpp",
            ROOT / "src/pass/oir/OIRVectorizationRemark.cpp",
            ROOT / "src/target/TargetMachine.cpp",
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
                *map(str, sources),
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
