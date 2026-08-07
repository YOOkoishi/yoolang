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


SOURCE = r'''
#include "oir/OIR.h"
#include "oir/OIRParser.h"
#include "pass/PassManager.h"
#include "pass/oir/OIRPortableVectorScalarizerPass.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct Failure final : std::exception {
    explicit Failure(std::string text) : text(std::move(text)) {}
    const char *what() const noexcept override { return text.c_str(); }
    std::string text;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition))                                                                       \
            throw Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                          ": requirement failed: " #condition);                               \
    } while (false)

using Scalarizer = pass::oir_portable::PortableVectorScalarizer;
using Options = pass::oir_portable::PortableVectorScalarizerOptions;
using Reason = pass::oir_portable::ScalarizationReasonCode;

oir::ConstantVector *i32_vector(oir::Module &module, oir::VectorType *type,
                                const std::vector<std::int64_t> &values) {
    std::vector<oir::Constant *> elements;
    for (auto value : values) elements.push_back(module.create_i32(value));
    return module.create_constant_vector(type, elements);
}

oir::ConstantVector *f32_vector(oir::Module &module, oir::VectorType *type,
                                const std::vector<float> &values) {
    std::vector<oir::Constant *> elements;
    for (auto value : values) elements.push_back(module.create_f32(value));
    return module.create_constant_vector(type, elements);
}

oir::ConstantMask *mask(oir::Module &module, oir::VectorType *type, std::uint64_t bits) {
    const auto lanes = type->element_count().min_lanes;
    std::vector<std::uint8_t> packed(static_cast<std::size_t>((lanes + 7U) / 8U), 0);
    for (std::uint64_t lane = 0; lane < lanes; ++lane) {
        if (((bits >> lane) & 1U) != 0U)
            packed[static_cast<std::size_t>(lane / 8U)] |=
                static_cast<std::uint8_t>(1U << (lane % 8U));
    }
    return module.create_constant_mask(type, packed);
}

bool type_contains_vector(const oir::Type *type) {
    if (type == nullptr) return false;
    if (type->is_vector()) return true;
    if (const auto *pointer = dynamic_cast<const oir::PointerType *>(type))
        return type_contains_vector(pointer->element_type());
    if (const auto *array = dynamic_cast<const oir::ArrayType *>(type))
        return type_contains_vector(array->element_type());
    if (const auto *function = dynamic_cast<const oir::FunctionType *>(type)) {
        if (type_contains_vector(function->return_type())) return true;
        for (auto *parameter : function->param_types()) {
            if (type_contains_vector(parameter)) return true;
        }
    }
    return false;
}

void require_scalar_module(const oir::Module &module) {
    std::string error;
    REQUIRE(module.verify(&error));
    for (const auto &global : module.globals())
        REQUIRE(!type_contains_vector(global->value_type()));
    for (const auto &function : module.functions()) {
        REQUIRE(!type_contains_vector(function->function_type()));
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                REQUIRE(!type_contains_vector(instruction->type()));
                for (auto *operand : instruction->operands())
                    REQUIRE(operand == nullptr || !type_contains_vector(operand->type()));
            }
        }
    }
}

void require_boundary_scalarized_module(const oir::Module &module) {
    std::string error;
    REQUIRE(module.verify(&error));
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                bool direct_vector = instruction->type()->is_vector();
                for (auto *operand : instruction->operands())
                    direct_vector = direct_vector || operand->type()->is_vector();
                if (!direct_vector) continue;
                switch (instruction->op()) {
                case oir::Instruction::OpID::FixedABIExtractLane:
                case oir::Instruction::OpID::FixedABIPack:
                case oir::Instruction::OpID::FixedABIObjectLoadLane:
                case oir::Instruction::OpID::FixedABIObjectStoreLane:
                case oir::Instruction::OpID::Call:
                case oir::Instruction::OpID::Ret:
                    break;
                default:
                    throw Failure("non-boundary vector instruction remains: " +
                                  instruction->print());
                }
            }
        }
    }
}

void require_stable_round_trip(const oir::Module &module) {
    const auto printed = module.print();
    auto parsed = oir::OIRParser::parse(printed, "<portable-round-trip>");
    REQUIRE(parsed.ok() && parsed.module != nullptr);
    std::string error;
    REQUIRE(parsed.module->verify(&error));
    REQUIRE(parsed.module->print() == printed);
}

std::size_t count_op(const oir::Module &module, oir::Instruction::OpID op) {
    std::size_t count = 0;
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions())
                count += instruction->op() == op ? 1U : 0U;
        }
    }
    return count;
}

using UseEntry = std::tuple<const oir::Value *, const oir::User *, std::size_t>;

std::vector<UseEntry> use_snapshot(const oir::Module &module) {
    std::vector<UseEntry> snapshot;
    auto append = [&](const oir::Value *value) {
        for (const auto &use : value->uses())
            snapshot.emplace_back(value, use.user, use.operand_index);
    };
    for (const auto &constant : module.owned_constants()) append(constant.get());
    for (const auto &global : module.globals()) append(global.get());
    for (const auto &function : module.functions()) {
        append(function.get());
        for (const auto &argument : function->args()) append(argument.get());
        for (const auto &block : function->blocks()) {
            append(block.get());
            for (const auto &instruction : block->instructions()) append(instruction.get());
        }
    }
    return snapshot;
}

std::unique_ptr<oir::Module> build_pure_module() {
    auto module = std::make_unique<oir::Module>("portable-pure");
    auto &types = module->types();
    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *v1i = types.fixed_vector_ty(i32, 1);
    auto *v3i = types.fixed_vector_ty(i32, 3);
    auto *v7i = types.fixed_vector_ty(i32, 7);
    auto *v7f = types.fixed_vector_ty(f32, 7);
    auto *v31i = types.fixed_vector_ty(i32, 31);
    auto *m3 = types.fixed_vector_ty(types.int1_ty(), 3);
    auto *m7 = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *function = module->create_function("pure", types.func_ty(i32, {i32, i32}));
    auto *entry = function->create_block("entry");
    auto *x = function->args()[0].get();
    auto *index = function->args()[1].get();
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);

    auto *splat3 = builder.create_splat(v3i, x, "splat3");
    auto *step3 = builder.create_step_vector(v3i, "step3");
    auto *sum3 = builder.create_binary(oir::Instruction::OpID::Add, splat3, step3, "sum3");
    auto *one3 = i32_vector(*module, v3i, {1, 1, 1});
    auto *two3 = i32_vector(*module, v3i, {2, 2, 2});
    (void)builder.create_binary(oir::Instruction::OpID::Sub, sum3, one3, "sub3");
    (void)builder.create_binary(oir::Instruction::OpID::Mul, sum3, two3, "mul3");
    (void)builder.create_binary(oir::Instruction::OpID::SDiv, sum3, one3, "div3");
    (void)builder.create_binary(oir::Instruction::OpID::SRem, sum3, two3, "rem3");
    auto *compared = builder.create_icmp(oir::CmpPred::GT, sum3, one3, "compared");
    auto *mask_zero = mask(*module, m3, 0);
    auto *mask_same = builder.create_binary(oir::Instruction::OpID::Xor, compared, mask_zero,
                                             "mask.same");
    (void)builder.create_binary(oir::Instruction::OpID::And, mask_same, compared, "mask.and");
    (void)builder.create_binary(oir::Instruction::OpID::Or, mask_same, mask_zero, "mask.or");

    auto *vp_active3 = mask(*module, m3, 0x5);
    auto *vp_sum3 = builder.create_vp_binary(
        oir::Instruction::OpID::Add, sum3, one3, vp_active3, module->create_i32(3), two3,
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Undisturbed, "vp.sum3");
    auto *vp_compared3 = builder.create_vp_icmp(
        oir::CmpPred::GT, vp_sum3, two3, vp_active3, module->create_i32(3), mask_zero,
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Undisturbed, "vp.compared3");
    auto *vp_lane1 =
        builder.create_extract_element(vp_sum3, module->create_i32(1), "vp.lane1");
    auto *vp_cmp_lane0 =
        builder.create_extract_element(vp_compared3, module->create_i32(0), "vp.cmp.lane0");
    auto *vp_cmp_i32 = builder.create_zext(vp_cmp_lane0, i32, "vp.cmp.i32");

    auto *inserted = builder.create_insert_element(sum3, module->create_i32(9), index, "inserted");
    auto *dynamic = builder.create_extract_element(inserted, index, "dynamic");
    auto *shuffled = builder.create_shuffle_vector(v3i, inserted, step3, {2, 0, -1}, "shuffled");
    (void)builder.create_extract_element(shuffled, module->create_i32(1), "shuffle.use");
    auto *selected = builder.create_vector_select(mask_same, inserted, step3, "selected");
    auto *selected0 = builder.create_extract_element(selected, module->create_i32(0), "selected0");

    auto *float_x = builder.create_sitofp(x, f32, "float.x");
    auto *splat7f = builder.create_splat(v7f, float_x, "splat7f");
    auto *ones7f = f32_vector(*module, v7f, {1, 1, 1, 1, 1, 1, 1});
    auto *sum7f = builder.create_binary(oir::Instruction::OpID::FAdd, splat7f, ones7f, "sum7f");
    (void)builder.create_binary(oir::Instruction::OpID::FSub, sum7f, ones7f, "fsub7");
    (void)builder.create_binary(oir::Instruction::OpID::FMul, sum7f, ones7f, "fmul7");
    (void)builder.create_binary(oir::Instruction::OpID::FDiv, sum7f, ones7f, "fdiv7");
    (void)builder.create_fcmp(oir::CmpPred::GE, sum7f, ones7f, "fcmp7");
    auto *vp_sum7f = builder.create_vp_binary(
        oir::Instruction::OpID::FAdd, sum7f, ones7f, mask(*module, m7, 0x7f),
        module->create_i32(7), ones7f, oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Undisturbed, "vp.sum7f");
    (void)builder.create_vp_fcmp(
        oir::CmpPred::GT, vp_sum7f, ones7f, mask(*module, m7, 0x7f),
        module->create_i32(7), mask(*module, m7, 0), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Undisturbed, "vp.compared7f");
    auto *back7 = builder.create_vector_cast(oir::VectorCastKind::FPToSI, v7i, sum7f, "back7");
    auto *back_lane = builder.create_extract_element(back7, module->create_i32(6), "back.lane");
    (void)builder.create_vector_cast(oir::VectorCastKind::SIToFP, v7f,
                                     i32_vector(*module, v7i, {1, 2, 3, 4, 5, 6, 7}),
                                     "to.float7");

    auto *splat1 = builder.create_splat(v1i, x, "splat1");
    auto *lane1 = builder.create_extract_element(splat1, module->create_i32(0), "lane1");
    auto *step31 = builder.create_step_vector(v31i, "step31");
    auto *lane30 = builder.create_extract_element(step31, module->create_i32(30), "lane30");

    auto *active3 = mask(*module, m3, 0x7);
    auto *reduced3 = builder.create_vp_reduction(
        oir::ReductionKind::Add, false, sum3, active3, module->create_i32(3),
        module->create_i32(0), oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce3");
    unsigned integer_reduction_index = 0;
    for (auto kind : {oir::ReductionKind::Mul, oir::ReductionKind::Min,
                      oir::ReductionKind::Max, oir::ReductionKind::And,
                      oir::ReductionKind::Or, oir::ReductionKind::Xor}) {
        (void)builder.create_vp_reduction(kind, false, sum3, active3, module->create_i32(3),
                                          module->create_i32(kind == oir::ReductionKind::Mul ? 1 : 0),
                                          oir::TailPolicy::Agnostic,
                                          oir::MaskPolicy::Agnostic,
                                          "ireduce.extra" +
                                              std::to_string(integer_reduction_index++));
    }
    auto *active7 = mask(*module, m7, 0x7f);
    auto *freduced = builder.create_vp_reduction(
        oir::ReductionKind::Add, true, sum7f, active7, module->create_i32(7),
        module->create_f32(0), oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "freduce");
    unsigned float_reduction_index = 0;
    for (auto kind : {oir::ReductionKind::Mul, oir::ReductionKind::Min,
                      oir::ReductionKind::Max}) {
        (void)builder.create_vp_reduction(kind, true, sum7f, active7, module->create_i32(7),
                                          module->create_f32(kind == oir::ReductionKind::Mul ? 1 : 0),
                                          oir::TailPolicy::Agnostic,
                                          oir::MaskPolicy::Agnostic,
                                          "freduce.extra" +
                                              std::to_string(float_reduction_index++));
    }
    auto *freduced_i = builder.create_fptosi(freduced, i32, "freduced.i");

    auto *result = builder.create_binary(oir::Instruction::OpID::Add, dynamic, reduced3, "r0");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, back_lane, "r1");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, selected0, "r2");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, lane1, "r3");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, lane30, "r4");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, freduced_i, "r5");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, vp_lane1, "r6");
    result = builder.create_binary(oir::Instruction::OpID::Add, result, vp_cmp_i32, "r7");
    builder.create_ret(result);
    return module;
}

std::unique_ptr<oir::Module> build_phi_module() {
    auto module = std::make_unique<oir::Module>("portable-phi");
    auto &types = module->types();
    auto *i32 = types.int32_ty();
    auto *v3 = types.fixed_vector_ty(i32, 3);
    auto *function = module->create_function("choose", types.func_ty(i32, {i32}));
    auto *entry = function->create_block("entry");
    auto *left = function->create_block("left");
    auto *right = function->create_block("right");
    auto *merge = function->create_block("merge");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    auto *condition = builder.create_icmp(oir::CmpPred::NE, function->args()[0].get(),
                                          module->create_i32(0), "condition");
    builder.create_cond_br(condition, left, right);
    builder.set_insert_point(left);
    auto *left_value = builder.create_splat(v3, module->create_i32(11), "left.value");
    builder.create_br(merge);
    builder.set_insert_point(right);
    auto *right_value = builder.create_splat(v3, module->create_i32(22), "right.value");
    builder.create_br(merge);
    builder.set_insert_point(merge);
    auto *phi = builder.create_phi(v3, "vector.phi");
    phi->add_incoming(left_value, left);
    phi->add_incoming(right_value, right);
    auto *lane = builder.create_extract_element(phi, module->create_i32(2), "result");
    builder.create_ret(lane);
    return module;
}

std::unique_ptr<oir::Module> build_memory_module() {
    auto module = std::make_unique<oir::Module>("portable-memory");
    auto &types = module->types();
    auto *i32 = types.int32_ty();
    auto *ptr = types.ptr_ty(i32);
    auto *v3 = types.fixed_vector_ty(i32, 3);
    auto *m3 = types.fixed_vector_ty(types.int1_ty(), 3);
    auto *function = module->create_function("memory", types.func_ty(i32, {ptr, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    auto *enabled = builder.create_icmp(oir::CmpPred::NE, function->args()[1].get(),
                                        module->create_i32(0), "enabled");
    auto *active = builder.create_splat(m3, enabled, "active");
    auto *passthrough = i32_vector(*module, v3, {7, 8, 9});
    auto *indices = i32_vector(*module, v3, {2, 0, 1});
    auto *loaded = builder.create_masked_load(
        v3, function->args()[0].get(), active, module->create_i32(3), passthrough,
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Undisturbed, 4, "loaded");
    builder.create_masked_store(loaded, function->args()[0].get(), active,
                                module->create_i32(3), oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
    auto *gathered = builder.create_vp_gather(
        v3, function->args()[0].get(), indices, active, module->create_i32(3), passthrough,
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Undisturbed, 4, "gathered");
    builder.create_vp_scatter(gathered, function->args()[0].get(), indices, active,
                              module->create_i32(3), oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    auto *sum = builder.create_vp_reduction(
        oir::ReductionKind::Add, false, gathered, mask(*module, m3, 0x7),
        module->create_i32(3), module->create_i32(0), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, "sum");
    builder.create_ret(sum);
    return module;
}

std::unique_ptr<oir::Module> build_fixed_abi_boundary_module() {
    auto module = std::make_unique<oir::Module>("portable-fixed-abi");
    auto &types = module->types();
    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *v1 = types.fixed_vector_ty(i32, 1);
    auto *v3 = types.fixed_vector_ty(i32, 3);
    auto *v3f = types.fixed_vector_ty(f32, 3);
    auto *v7 = types.fixed_vector_ty(i32, 7);
    auto *m31 = types.fixed_vector_ty(types.int1_ty(), 31);
    auto *a2v7 = types.array_ty(v7, 2);

    auto *global_v3 = module->create_global("global_v3", v3, false, module->create_zero(v3));
    auto *global_a2v7 =
        module->create_global("global_a2v7", a2v7, false, module->create_zero(a2v7));
    auto *global_m31 =
        module->create_global("global_m31", m31, false, module->create_zero(m31));
    auto *external_v7 =
        module->create_function("external_v7", types.func_ty(v7, {v7}), true);

    {
        auto *function = module->create_function("identity_v1", types.func_ty(v1, {v1}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        builder.create_ret(function->args()[0].get());
    }
    {
        auto *function = module->create_function("object_v3", types.func_ty(v3, {v3}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *local = builder.create_alloca(v3, "local.v3");
        builder.create_store(function->args()[0].get(), local);
        auto *local_value = builder.create_load(local, v3, "local.value");
        auto *global_value = builder.create_load(global_v3, v3, "global.value");
        auto *sum = builder.create_binary(oir::Instruction::OpID::Add, local_value,
                                          global_value, "sum");
        builder.create_store(sum, global_v3);
        builder.create_ret(sum);
    }
    {
        auto *function = module->create_function("array_call_v7", types.func_ty(v7, {v7}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *slot = builder.create_gep(
            global_a2v7, types.ptr_ty(v7), {module->create_i32(0), module->create_i32(1)},
            "array.slot");
        builder.create_store(function->args()[0].get(), slot);
        auto *loaded = builder.create_load(slot, v7, "array.value");
        auto *called = builder.create_call(external_v7, v7, {loaded}, "called");
        auto *sum = builder.create_binary(oir::Instruction::OpID::Add, called,
                                          function->args()[0].get(), "sum");
        builder.create_ret(sum);
    }
    {
        auto *function = module->create_function("object_m31", types.func_ty(m31, {m31}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *local = builder.create_alloca(m31, "local.m31");
        builder.create_store(function->args()[0].get(), local);
        auto *local_value = builder.create_load(local, m31, "local.mask");
        auto *flipped = builder.create_binary(
            oir::Instruction::OpID::Xor, local_value, mask(*module, m31, 0x81), "flipped");
        builder.create_store(flipped, global_m31);
        auto *global_value = builder.create_load(global_m31, m31, "global.mask");
        builder.create_ret(global_value);
    }
    {
        auto *function = module->create_function("float_v3", types.func_ty(v3f, {v3f}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *sum = builder.create_binary(
            oir::Instruction::OpID::FAdd, function->args()[0].get(),
            f32_vector(*module, v3f, {1.0F, 2.0F, 3.0F}), "float.sum");
        builder.create_ret(sum);
    }
    return module;
}

using Number = std::variant<std::int64_t, float>;

std::int64_t integer(const Number &value) {
    REQUIRE(std::holds_alternative<std::int64_t>(value));
    return std::get<std::int64_t>(value);
}

float floating(const Number &value) {
    REQUIRE(std::holds_alternative<float>(value));
    return std::get<float>(value);
}

struct Execution final {
    std::int64_t result = 0;
    std::vector<std::int64_t> memory;
};

Execution execute(oir::Function &function, std::vector<Number> arguments,
                  std::vector<std::int64_t> memory = {}) {
    REQUIRE(arguments.size() == function.args().size());
    std::unordered_map<const oir::Value *, Number> values;
    for (std::size_t i = 0; i < arguments.size(); ++i) values[function.args()[i].get()] = arguments[i];

    auto value = [&](const oir::Value *operand) -> Number {
        if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(operand))
            return constant->value();
        if (const auto *constant = dynamic_cast<const oir::ConstantFloat *>(operand))
            return constant->value();
        if (dynamic_cast<const oir::ConstantAggregateZero *>(operand) != nullptr) {
            if (operand->type()->is_scalar_float()) return 0.0F;
            return std::int64_t{0};
        }
        auto found = values.find(operand);
        if (found == values.end()) throw Failure("interpreter value is unavailable: " + operand->print());
        return found->second;
    };

    oir::BasicBlock *block = function.entry_block();
    oir::BasicBlock *predecessor = nullptr;
    for (unsigned steps = 0; steps < 10000; ++steps) {
        REQUIRE(block != nullptr);
        std::vector<std::pair<const oir::PhiInst *, Number>> phi_values;
        for (const auto &owned : block->instructions()) {
            const auto *phi = dynamic_cast<const oir::PhiInst *>(owned.get());
            if (phi == nullptr) break;
            bool found = false;
            for (const auto &[incoming, incoming_block] : phi->incoming()) {
                if (incoming_block == predecessor) {
                    phi_values.emplace_back(phi, value(incoming));
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
        for (const auto &[phi, incoming] : phi_values) values[phi] = incoming;

        bool branched = false;
        for (const auto &owned : block->instructions()) {
            auto *instruction = owned.get();
            if (dynamic_cast<oir::PhiInst *>(instruction) != nullptr) continue;
            if (auto *binary = dynamic_cast<oir::BinaryInst *>(instruction)) {
                auto lhs = value(binary->lhs());
                auto rhs = value(binary->rhs());
                Number out;
                switch (binary->op()) {
                case oir::Instruction::OpID::Add: out = integer(lhs) + integer(rhs); break;
                case oir::Instruction::OpID::Sub: out = integer(lhs) - integer(rhs); break;
                case oir::Instruction::OpID::Mul: out = integer(lhs) * integer(rhs); break;
                case oir::Instruction::OpID::SDiv: out = integer(lhs) / integer(rhs); break;
                case oir::Instruction::OpID::SRem: out = integer(lhs) % integer(rhs); break;
                case oir::Instruction::OpID::And: out = integer(lhs) & integer(rhs); break;
                case oir::Instruction::OpID::Or: out = integer(lhs) | integer(rhs); break;
                case oir::Instruction::OpID::Xor: out = integer(lhs) ^ integer(rhs); break;
                case oir::Instruction::OpID::FAdd: out = floating(lhs) + floating(rhs); break;
                case oir::Instruction::OpID::FSub: out = floating(lhs) - floating(rhs); break;
                case oir::Instruction::OpID::FMul: out = floating(lhs) * floating(rhs); break;
                case oir::Instruction::OpID::FDiv: out = floating(lhs) / floating(rhs); break;
                default: throw Failure("unsupported interpreted binary");
                }
                values[instruction] = out;
                continue;
            }
            if (auto *compare = dynamic_cast<oir::CmpInst *>(instruction)) {
                auto lhs = value(compare->lhs());
                auto rhs = value(compare->rhs());
                bool result = false;
                if (compare->op() == oir::Instruction::OpID::ICmp) {
                    const auto l = integer(lhs), r = integer(rhs);
                    switch (compare->pred()) {
                    case oir::CmpPred::EQ: result = l == r; break;
                    case oir::CmpPred::NE: result = l != r; break;
                    case oir::CmpPred::LT: result = l < r; break;
                    case oir::CmpPred::LE: result = l <= r; break;
                    case oir::CmpPred::GT: result = l > r; break;
                    case oir::CmpPred::GE: result = l >= r; break;
                    }
                } else {
                    const auto l = floating(lhs), r = floating(rhs);
                    switch (compare->pred()) {
                    case oir::CmpPred::EQ: result = l == r; break;
                    case oir::CmpPred::NE: result = l != r; break;
                    case oir::CmpPred::LT: result = l < r; break;
                    case oir::CmpPred::LE: result = l <= r; break;
                    case oir::CmpPred::GT: result = l > r; break;
                    case oir::CmpPred::GE: result = l >= r; break;
                    }
                }
                values[instruction] = std::int64_t{result ? 1 : 0};
                continue;
            }
            if (auto *cast = dynamic_cast<oir::CastInst *>(instruction)) {
                auto source = value(cast->src());
                if (cast->op() == oir::Instruction::OpID::SIToFP)
                    values[instruction] = static_cast<float>(integer(source));
                else if (cast->op() == oir::Instruction::OpID::FPToSI)
                    values[instruction] = static_cast<std::int64_t>(floating(source));
                else if (cast->op() == oir::Instruction::OpID::ZExt)
                    values[instruction] = integer(source);
                else
                    throw Failure("unsupported interpreted cast");
                continue;
            }
            if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(instruction)) {
                REQUIRE(gep->indices().size() == 1);
                values[instruction] = integer(value(gep->base_ptr())) + integer(value(gep->indices()[0]));
                continue;
            }
            if (auto *load = dynamic_cast<oir::LoadInst *>(instruction)) {
                const auto address = integer(value(load->ptr()));
                REQUIRE(address >= 0 && static_cast<std::size_t>(address) < memory.size());
                values[instruction] = memory[static_cast<std::size_t>(address)];
                continue;
            }
            if (auto *store = dynamic_cast<oir::StoreInst *>(instruction)) {
                const auto address = integer(value(store->ptr()));
                REQUIRE(address >= 0 && static_cast<std::size_t>(address) < memory.size());
                memory[static_cast<std::size_t>(address)] = integer(value(store->value()));
                continue;
            }
            if (auto *branch = dynamic_cast<oir::BranchInst *>(instruction)) {
                predecessor = block;
                block = branch->is_conditional()
                            ? (integer(value(branch->cond())) != 0 ? branch->true_bb() : branch->false_bb())
                            : branch->target_bb();
                branched = true;
                break;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(instruction)) {
                REQUIRE(ret->has_value());
                return {integer(value(ret->value())), std::move(memory)};
            }
            throw Failure("unsupported interpreted instruction: " + instruction->print());
        }
        if (!branched) throw Failure("interpreter block has no branch or return");
    }
    throw Failure("interpreter exceeded step limit");
}

void test_pure_shapes_and_semantics() {
    auto module = build_pure_module();
    REQUIRE(module->globals().empty());
    auto *original_function = module->get_function("pure");
    REQUIRE(original_function != nullptr);
    REQUIRE(!type_contains_vector(original_function->function_type()));
    std::string error;
    REQUIRE(module->verify(&error));
    auto result = Scalarizer{}.run(*module);
    if (!result.success) throw Failure(result.message);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.code == Reason::Scalarized);
    REQUIRE(result.vector_instructions_scalarized >= 30);
    require_scalar_module(*module);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Splat) == 0);
    REQUIRE(count_op(*module, oir::Instruction::OpID::VPReduction) == 0);
    REQUIRE(count_op(*module, oir::Instruction::OpID::VPBinary) == 0);
    REQUIRE(count_op(*module, oir::Instruction::OpID::VPCmp) == 0);
    require_stable_round_trip(*module);
    auto *function = module->get_function("pure");
    REQUIRE(function != nullptr);
    REQUIRE(execute(*function, {std::int64_t{2}, std::int64_t{1}}).result == 79);
    REQUIRE(execute(*function, {std::int64_t{1}, std::int64_t{2}}).result == 64);
}

void test_vector_phi_and_dynamic_cfg() {
    auto module = build_phi_module();
    auto result = Scalarizer{}.run(*module);
    if (!result.success) throw Failure(result.message);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.cfg_blocks_created == 0);
    require_scalar_module(*module);
    auto *function = module->get_function("choose");
    REQUIRE(execute(*function, {std::int64_t{1}}).result == 11);
    REQUIRE(execute(*function, {std::int64_t{0}}).result == 22);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Phi) == 3);
}

void test_masked_memory_semantics_and_address_guards() {
    auto module = build_memory_module();
    auto result = Scalarizer{}.run(*module);
    if (!result.success) throw Failure(result.message);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.memory_operations_scalarized == 4);
    REQUIRE(result.cfg_blocks_created >= 12);
    require_scalar_module(*module);
    auto *function = module->get_function("memory");
    auto inactive = execute(*function, {std::int64_t{0}, std::int64_t{0}}, {1, 2, 3});
    REQUIRE(inactive.result == 24);
    REQUIRE(inactive.memory == std::vector<std::int64_t>({1, 2, 3}));
    auto active = execute(*function, {std::int64_t{0}, std::int64_t{1}}, {1, 2, 3});
    REQUIRE(active.result == 6);
    REQUIRE(active.memory == std::vector<std::int64_t>({1, 2, 3}));
    for (const auto &block : function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (dynamic_cast<const oir::LoadInst *>(instruction.get()) != nullptr ||
                dynamic_cast<const oir::StoreInst *>(instruction.get()) != nullptr ||
                dynamic_cast<const oir::GetElementPtrInst *>(instruction.get()) != nullptr) {
                REQUIRE(block->name().find(".true") != std::string::npos);
            }
        }
    }
}

void test_fixed_abi_signature_call_and_object_scalarization() {
    auto module = build_fixed_abi_boundary_module();
    std::string error;
    REQUIRE(module->verify(&error));
    std::unordered_map<std::string, std::string> signatures;
    for (const auto &function : module->functions())
        signatures.emplace(function->name(), function->function_type()->print());
    std::unordered_map<std::string, std::string> global_types;
    for (const auto &global : module->globals())
        global_types.emplace(global->name(), global->value_type()->print());

    auto result = Scalarizer{}.run(*module);
    if (!result.success) throw Failure(result.message);
    REQUIRE(result.success && result.changed && result.code == Reason::Scalarized);
    REQUIRE(result.boundary_instructions_created > 100);
    REQUIRE(result.memory_operations_scalarized == 10);
    require_boundary_scalarized_module(*module);
    REQUIRE(count_op(*module, oir::Instruction::OpID::FixedABIExtractLane) == 52);
    REQUIRE(count_op(*module, oir::Instruction::OpID::FixedABIPack) == 6);
    REQUIRE(count_op(*module, oir::Instruction::OpID::FixedABIObjectLoadLane) == 75);
    REQUIRE(count_op(*module, oir::Instruction::OpID::FixedABIObjectStoreLane) == 75);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Load) == 0);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Store) == 0);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Call) == 1);
    REQUIRE(count_op(*module, oir::Instruction::OpID::Alloca) == 2);
    REQUIRE(count_op(*module, oir::Instruction::OpID::GetElementPtr) == 1);
    for (const auto &function : module->functions()) {
        REQUIRE(function->function_type()->print() == signatures.at(function->name()));
    }
    for (const auto &global : module->globals()) {
        REQUIRE(global->value_type()->print() == global_types.at(global->name()));
    }
    REQUIRE(module->get_function("external_v7")->is_external());
    REQUIRE(module->get_function("external_v7")->blocks().empty());
    require_stable_round_trip(*module);
    const auto stable = module->print();
    auto second = Scalarizer{}.run(*module);
    REQUIRE(second.success && !second.changed && second.code == Reason::NoFixedVector);
    REQUIRE(module->print() == stable);
}

void test_fixed_abi_transactional_rollback() {
    auto module = build_fixed_abi_boundary_module();
    const auto printed = module->print();
    const auto uses = use_snapshot(*module);
    bool saw_boundary_candidate = false;
    Options options;
    options.post_transform_verifier = [&](const oir::Module &candidate, std::string &error) {
        saw_boundary_candidate = true;
        require_boundary_scalarized_module(candidate);
        error = "injected fixed ABI verifier failure";
        return false;
    };
    auto result = Scalarizer(options).run(*module);
    REQUIRE(!result.success && !result.changed);
    REQUIRE(result.code == Reason::PostVerificationFailed);
    REQUIRE(saw_boundary_candidate);
    REQUIRE(module->print() == printed);
    REQUIRE(use_snapshot(*module) == uses);
    std::string error;
    REQUIRE(module->verify(&error));
}

void test_transactional_post_verifier_rollback() {
    auto module = build_pure_module();
    const auto printed = module->print();
    const auto uses = use_snapshot(*module);
    bool saw_scalar_candidate = false;
    Options options;
    options.post_transform_verifier = [&](const oir::Module &candidate, std::string &error) {
        saw_scalar_candidate = true;
        require_scalar_module(candidate);
        error = "injected verifier failure";
        return false;
    };
    auto result = Scalarizer(options).run(*module);
    REQUIRE(!result.success && !result.changed);
    REQUIRE(result.code == Reason::PostVerificationFailed);
    REQUIRE(saw_scalar_candidate);
    REQUIRE(module->print() == printed);
    REQUIRE(use_snapshot(*module) == uses);
    std::string error;
    REQUIRE(module->verify(&error));
}

void test_duplicate_ssa_snapshot_transaction() {
    oir::Module module("duplicate-snapshot-names");
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *v3 = types.fixed_vector_ty(i32, 3);
    auto *function = module.create_function("f", types.func_ty(i32, {i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *first = builder.create_splat(v3, function->args()[0].get(), "duplicate");
    auto *second = builder.create_splat(v3, module.create_i32(3), "duplicate");
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add, first, second,
                                      "sum");
    builder.create_ret(
        builder.create_extract_element(sum, module.create_i32(0), "lane"));
    std::string error;
    REQUIRE(module.verify(&error));

    const auto printed = module.print();
    const auto uses = use_snapshot(module);
    Options reject;
    reject.post_transform_verifier = [](const oir::Module &, std::string &detail) {
        detail = "injected duplicate-name transaction failure";
        return false;
    };
    auto failed = Scalarizer(reject).run(module);
    REQUIRE(!failed.success && !failed.changed);
    REQUIRE(failed.code == Reason::PostVerificationFailed);
    REQUIRE(module.print() == printed);
    REQUIRE(use_snapshot(module) == uses);
    REQUIRE(module.verify(&error));

    auto result = Scalarizer{}.run(module);
    REQUIRE(result.success && result.changed && result.code == Reason::Scalarized);
    REQUIRE(module.verify(&error));
    require_scalar_module(module);
    require_stable_round_trip(module);
}

void test_late_mutation_and_unused_object_identity() {
    {
        oir::Module module("late-bitcast-reject");
        auto &types = module.types();
        auto *i32 = types.int32_ty();
        auto *v3i = types.fixed_vector_ty(i32, 3);
        auto *v3f = types.fixed_vector_ty(types.float_ty(), 3);
        auto *function = module.create_function("f", types.func_ty(i32, {i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *splat = builder.create_splat(v3i, function->args()[0].get(), "early.splat");
        auto *bitcast =
            builder.create_vector_cast(oir::VectorCastKind::Bitcast, v3f, splat, "late.bitcast");
        auto *lane = builder.create_extract_element(bitcast, module.create_i32(0), "lane");
        builder.create_ret(builder.create_fptosi(lane, i32, "result"));
        std::string error;
        REQUIRE(module.verify(&error));
        const auto printed = module.print();
        const auto uses = use_snapshot(module);
        auto result = Scalarizer{}.run(module);
        REQUIRE(!result.success && !result.changed);
        REQUIRE(result.code == Reason::UnsupportedOperation);
        REQUIRE(result.message.rfind("PORTABLE_VECTOR_OPERATION_UNSUPPORTED:", 0) == 0);
        REQUIRE(module.print() == printed && use_snapshot(module) == uses);
        REQUIRE(module.verify(&error));
    }
    {
        oir::Module module("vector-object-reject");
        auto &types = module.types();
        auto *i32 = types.int32_ty();
        auto *v3 = types.fixed_vector_ty(i32, 3);
        auto *function = module.create_function("f", types.func_ty(i32, {}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        (void)builder.create_alloca(v3, "vector.object");
        builder.create_ret(module.create_i32(0));
        std::string error;
        REQUIRE(module.verify(&error));
        const auto printed = module.print();
        const auto uses = use_snapshot(module);
        Options options;
        options.force = true;
        auto result = Scalarizer(options).run(module);
        REQUIRE(result.success && !result.changed);
        REQUIRE(result.code == Reason::NoFixedVector);
        REQUIRE(module.print() == printed && use_snapshot(module) == uses);
    }
}

void test_fixed_evl_scalable_and_abi_rejections() {
    {
        auto module = std::make_unique<oir::Module>("evl-reject");
        auto &types = module->types();
        auto *i32 = types.int32_ty();
        auto *v3 = types.fixed_vector_ty(i32, 3);
        auto *m3 = types.fixed_vector_ty(types.int1_ty(), 3);
        auto *function = module->create_function("f", types.func_ty(i32, {i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *lhs = i32_vector(*module, v3, {1, 2, 3});
        auto *rhs = i32_vector(*module, v3, {4, 5, 6});
        auto *vp = builder.create_vp_binary(
            oir::Instruction::OpID::Add, lhs, rhs, mask(*module, m3, 7),
            function->args()[0].get(), lhs, oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Undisturbed, "vp");
        builder.create_ret(builder.create_extract_element(vp, module->create_i32(0), "lane"));
        const auto printed = module->print();
        const auto uses = use_snapshot(*module);
        Options options;
        options.force = true;
        auto result = Scalarizer(options).run(*module);
        REQUIRE(!result.success && result.code == Reason::FixedEVLRequired);
        REQUIRE(module->print() == printed && use_snapshot(*module) == uses);
    }
    {
        oir::Module module("scalable-reject");
        auto &types = module.types();
        auto *function = module.create_function("f", types.func_ty(types.int32_ty(), {}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        (void)builder.create_set_vl(types.scalable_vector_ty(types.int32_ty(), 4),
                                    module.create_i32(8), "vl");
        builder.create_ret(module.create_i32(0));
        const auto printed = module.print();
        auto result = Scalarizer{}.run(module);
        REQUIRE(!result.success && result.code == Reason::ScalableVectorUnsupported);
        REQUIRE(module.print() == printed);
    }
    {
        oir::Module module("scalable-object-signature-reject");
        auto &types = module.types();
        auto *nx = types.scalable_vector_ty(types.int32_ty(), 4);
        auto *function = module.create_function(
            "f", types.func_ty(types.int32_ty(), {types.ptr_ty(nx)}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret(module.create_i32(0));
        const auto printed = module.print();
        const auto uses = use_snapshot(module);
        auto result = Scalarizer{}.run(module);
        REQUIRE(!result.success && result.code == Reason::ScalableVectorUnsupported);
        REQUIRE(module.print() == printed && use_snapshot(module) == uses);
    }
    {
        oir::Module module("vector-vararg-reject");
        auto &types = module.types();
        auto *v3 = types.fixed_vector_ty(types.int32_ty(), 3);
        auto *callee = module.create_function(
            "variadic", types.func_ty(types.int32_ty(), {types.int32_ty()}, true), true);
        auto *function = module.create_function("f", types.func_ty(types.int32_ty(), {}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *call = builder.create_call(
            callee, types.int32_ty(),
            {module.create_i32(1), i32_vector(module, v3, {1, 2, 3})}, "call");
        builder.create_ret(call);
        std::string error;
        REQUIRE(module.verify(&error));
        const auto printed = module.print();
        const auto uses = use_snapshot(module);
        auto result = Scalarizer{}.run(module);
        REQUIRE(!result.success && result.code == Reason::AggregateABIUnavailable);
        REQUIRE(result.message.rfind("PORTABLE_VECTOR_AGGREGATE_ABI_UNAVAILABLE:", 0) == 0);
        REQUIRE(module.print() == printed && use_snapshot(module) == uses);
    }
    {
        oir::Module module("abi-reject");
        auto &types = module.types();
        auto *v3 = types.fixed_vector_ty(types.int32_ty(), 3);
        auto *function = module.create_function("f", types.func_ty(v3, {v3}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret(function->args()[0].get());
        const auto printed = module.print();
        const auto uses = use_snapshot(module);
        Options options;
        options.force = true;
        auto result = Scalarizer(options).run(module);
        if (!result.success) throw Failure(result.message);
        REQUIRE(result.success && result.changed && result.code == Reason::Scalarized);
        REQUIRE(module.print() != printed);
        REQUIRE(result.boundary_instructions_created == 4);
        REQUIRE(count_op(module, oir::Instruction::OpID::FixedABIExtractLane) == 3);
        REQUIRE(count_op(module, oir::Instruction::OpID::FixedABIPack) == 1);
        REQUIRE(module.get_function("f")->function_type()->print() ==
                "<3 x i32> (<3 x i32>)");
        require_boundary_scalarized_module(module);
        require_stable_round_trip(module);
        auto second = Scalarizer{}.run(module);
        REQUIRE(second.success && !second.changed && second.code == Reason::NoFixedVector);
    }
}

void test_noop_and_pass_wrapper() {
    auto module = std::make_unique<oir::Module>("scalar-noop");
    auto &types = module->types();
    auto *function = module->create_function("f", types.func_ty(types.int32_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    builder.create_ret(module->create_i32(7));
    const auto printed = module->print();
    auto result = Scalarizer{}.run(*module);
    REQUIRE(result.success && !result.changed && result.code == Reason::NoFixedVector);
    REQUIRE(module->print() == printed);

    pass::PassContext context;
    pass::OIRPortableVectorScalarizerPass pass;
    auto missing = pass.run(context);
    REQUIRE(!missing.success &&
            missing.message.rfind("PORTABLE_MISSING_MODULE:", 0) == 0);
    context.set_ssa_module(std::move(module));
    auto noop = pass.run(context);
    REQUIRE(noop.success && !noop.changed);
}

} // namespace

int main() {
    try {
        test_pure_shapes_and_semantics();
        std::cout << "PASS pure_shapes_and_semantics\n";
        test_vector_phi_and_dynamic_cfg();
        std::cout << "PASS vector_phi_and_dynamic_cfg\n";
        test_masked_memory_semantics_and_address_guards();
        std::cout << "PASS masked_memory_semantics_and_address_guards\n";
        test_fixed_abi_signature_call_and_object_scalarization();
        std::cout << "PASS fixed_abi_signature_call_and_object_scalarization\n";
        test_fixed_abi_transactional_rollback();
        std::cout << "PASS fixed_abi_transactional_rollback\n";
        test_transactional_post_verifier_rollback();
        std::cout << "PASS transactional_post_verifier_rollback\n";
        test_duplicate_ssa_snapshot_transaction();
        std::cout << "PASS duplicate_ssa_snapshot_transaction\n";
        test_late_mutation_and_unused_object_identity();
        std::cout << "PASS late_mutation_and_unused_object_identity\n";
        test_fixed_evl_scalable_and_abi_rejections();
        std::cout << "PASS fixed_evl_scalable_and_abi_rejections\n";
        test_noop_and_pass_wrapper();
        std::cout << "PASS noop_and_pass_wrapper\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL portable_vector_scalarizer: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
'''


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
        print("error: no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="portable-vector-scalarizer-") as tmp:
        directory = Path(tmp)
        source = directory / "portable_vector_scalarizer_tests.cpp"
        binary = directory / "portable_vector_scalarizer_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = (
            "src/oir/OIR.cpp",
            "src/oir/OIRAnalysis.cpp",
            "src/oir/OIRCFGUtils.cpp",
            "src/oir/OIRDataLayout.cpp",
            "src/oir/OIRParser.cpp",
            "src/builtin/BuiltinRegistry.cpp",
            "src/pass/PassManager.cpp",
            "src/pass/oir/OIRPortableVectorScalarizerPass.cpp",
        )
        compile_process = subprocess.run(
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
        if compile_process.returncode != 0:
            print(compile_process.stdout, end="")
            print(compile_process.stderr, end="", file=sys.stderr)
            return compile_process.returncode
        run_process = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        print(run_process.stdout, end="")
        print(run_process.stderr, end="", file=sys.stderr)
        return run_process.returncode


if __name__ == "__main__":
    raise SystemExit(main())
