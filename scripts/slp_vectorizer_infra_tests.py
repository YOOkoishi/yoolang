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
#include "pass/oir/OIRSLPVectorizer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

namespace {

using pass::oir_vectorize::SLPReasonCode;

target::TargetProfile rvv_profile() {
    target::TargetProfile profile;
    profile.march = "rv64gcv";
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    return profile;
}

target::TargetProfile scalar_profile() {
    target::TargetProfile profile;
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    return profile;
}

bool has_reason(const pass::oir_vectorize::SLPVectorizerResult &result,
                SLPReasonCode code) {
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

unsigned count_op(const oir::Function &function, oir::Instruction::OpID op) {
    unsigned count = 0;
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            count += instruction->op() == op;
        }
    }
    return count;
}

struct UseListSnapshot final {
    const oir::Value *value = nullptr;
    std::vector<std::pair<const oir::User *, std::size_t>> uses;
};

std::vector<std::pair<const oir::User *, std::size_t>>
canonical_uses(const oir::Value &value) {
    std::vector<std::pair<const oir::User *, std::size_t>> uses;
    for (const auto &use : value.uses()) uses.emplace_back(use.user, use.operand_index);
    std::sort(uses.begin(), uses.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first) return std::less<const oir::User *>{}(lhs.first, rhs.first);
        return lhs.second < rhs.second;
    });
    return uses;
}

std::vector<UseListSnapshot> snapshot_use_lists(const oir::Function &function) {
    std::vector<const oir::Value *> values;
    std::unordered_set<const oir::Value *> seen;
    const auto add = [&](const oir::Value *value) {
        if (value != nullptr && seen.insert(value).second) values.push_back(value);
    };
    for (const auto &argument : function.args()) add(argument.get());
    for (const auto &block : function.blocks()) {
        add(block.get());
        for (const auto &instruction : block->instructions()) {
            add(instruction.get());
            for (auto *operand : instruction->operands()) add(operand);
        }
    }
    std::vector<UseListSnapshot> snapshot;
    snapshot.reserve(values.size());
    for (auto *value : values) snapshot.push_back({value, canonical_uses(*value)});
    return snapshot;
}

bool use_lists_match(const std::vector<UseListSnapshot> &snapshot) {
    return std::all_of(snapshot.begin(), snapshot.end(), [](const auto &entry) {
        return entry.value != nullptr && canonical_uses(*entry.value) == entry.uses;
    });
}

struct IntegerPipelineFixture {
    oir::Module module{"slp_integer_pipeline"};
    oir::Function *function = nullptr;
    unsigned lanes = 0;

    explicit IntegerPipelineFixture(unsigned lane_count = 4) : lanes(lane_count) {
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        function = module.create_function(
            "integer_pipeline", types.func_ty(types.void_ty(), {ptr, ptr}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);

        std::vector<oir::Value *> loaded;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            auto *address = builder.create_gep(function->args()[0].get(), ptr,
                                               {builder.i32(static_cast<std::int64_t>(lane))},
                                               "in." + std::to_string(lane));
            loaded.push_back(builder.create_load(address, types.int32_ty(),
                                                 "load." + std::to_string(lane)));
        }
        std::vector<oir::Value *> sums;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            sums.push_back(builder.create_binary(oir::Instruction::OpID::Add,
                                                 loaded[static_cast<std::size_t>(lane)],
                                                 builder.i32(1),
                                                 "add." + std::to_string(lane)));
        }
        for (unsigned lane = 0; lane < lanes; ++lane) {
            auto *address = builder.create_gep(function->args()[1].get(), ptr,
                                               {builder.i32(static_cast<std::int64_t>(lane))},
                                               "out." + std::to_string(lane));
            builder.create_store(sums[static_cast<std::size_t>(lane)], address);
        }
        builder.create_ret();
        std::string error;
        REQUIRE(module.verify(&error));
    }
};

std::vector<std::int32_t>
simulate_vectorized_integer_pipeline(const oir::Function &function,
                                     const std::vector<std::int32_t> &input) {
    std::unordered_map<const oir::Value *, std::vector<std::int32_t>> vectors;
    std::function<const std::vector<std::int32_t> &(const oir::Value *)> evaluate;
    evaluate = [&](const oir::Value *value) -> const std::vector<std::int32_t> & {
        if (const auto found = vectors.find(value); found != vectors.end()) {
            return found->second;
        }
        if (const auto *load = dynamic_cast<const oir::VPLoadInst *>(value)) {
            auto *vector_type = dynamic_cast<oir::VectorType *>(load->type());
            REQUIRE(vector_type != nullptr && vector_type->element_count().is_fixed());
            const auto lanes = vector_type->element_count().min_lanes;
            REQUIRE(input.size() == lanes);
            auto *evl = dynamic_cast<const oir::ConstantInt *>(load->evl());
            auto *mask = dynamic_cast<const oir::ConstantMask *>(load->active_mask());
            REQUIRE(evl != nullptr && evl->value() == static_cast<std::int64_t>(lanes));
            REQUIRE(mask != nullptr && mask->lane_count() == lanes);
            for (std::uint64_t lane = 0; lane < lanes; ++lane) REQUIRE(mask->lane(lane));
            return vectors.emplace(value, input).first->second;
        }
        if (const auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
            auto *vector_type = dynamic_cast<oir::VectorType *>(splat->type());
            auto *scalar = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
            REQUIRE(vector_type != nullptr && vector_type->element_count().is_fixed());
            REQUIRE(scalar != nullptr);
            std::vector<std::int32_t> result(
                vector_type->element_count().min_lanes,
                static_cast<std::int32_t>(scalar->value()));
            return vectors.emplace(value, std::move(result)).first->second;
        }
        if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
            const auto lhs = evaluate(binary->lhs());
            const auto rhs = evaluate(binary->rhs());
            REQUIRE(lhs.size() == rhs.size());
            REQUIRE(binary->binary_op() == oir::Instruction::OpID::Add);
            std::vector<std::int32_t> result(lhs.size());
            for (std::size_t lane = 0; lane < lhs.size(); ++lane) {
                result[lane] = static_cast<std::int32_t>(lhs[lane] + rhs[lane]);
            }
            return vectors.emplace(value, std::move(result)).first->second;
        }
        throw std::runtime_error("semantic simulator encountered unsupported vector value");
    };

    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (const auto *store = dynamic_cast<const oir::VPStoreInst *>(instruction.get())) {
                const auto result = evaluate(store->value());
                auto *evl = dynamic_cast<const oir::ConstantInt *>(store->evl());
                auto *mask = dynamic_cast<const oir::ConstantMask *>(store->active_mask());
                REQUIRE(evl != nullptr && evl->value() == static_cast<std::int64_t>(result.size()));
                REQUIRE(mask != nullptr && mask->lane_count() == result.size());
                for (std::uint64_t lane = 0; lane < result.size(); ++lane) {
                    REQUIRE(mask->lane(lane));
                }
                return result;
            }
        }
    }
    throw std::runtime_error("semantic simulator did not find a VP store");
}

std::vector<std::int32_t>
simulate_scalar_integer_pipeline(const std::vector<std::int32_t> &input) {
    std::vector<std::int32_t> output;
    output.reserve(input.size());
    for (const auto value : input) output.push_back(value + 1);
    return output;
}

void test_contiguous_integer_pipeline_rewrites_ssa_and_memory() {
    IntegerPipelineFixture fixture;
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer{}.run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success);
    REQUIRE(result.changed);
    REQUIRE(result.packs_vectorized == 3);
    REQUIRE(result.scalar_instructions_replaced == 12);
    REQUIRE(has_reason(result, SLPReasonCode::Vectorized));
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Load) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Add) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Store) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPLoad) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPStore) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0);

    const oir::VPBinaryInst *vector_add = nullptr;
    for (const auto &instruction : fixture.function->entry_block()->instructions()) {
        if (const auto *candidate =
                dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
            vector_add = candidate;
        }
    }
    REQUIRE(vector_add != nullptr);
    auto *vector_type = dynamic_cast<oir::VectorType *>(vector_add->type());
    REQUIRE(vector_type != nullptr);
    REQUIRE(vector_type->element_count().is_fixed());
    REQUIRE(vector_type->element_count().min_lanes == 4);
    REQUIRE(vector_add->binary_op() == oir::Instruction::OpID::Add);
    auto *evl = dynamic_cast<oir::ConstantInt *>(vector_add->evl());
    REQUIRE(evl != nullptr && evl->value() == 4);
    REQUIRE(vector_add->active_mask()->type()->is_mask());
    std::string error;
    REQUIRE(fixture.module.verify(&error));
    REQUIRE(!remarks.empty());
    REQUIRE(remarks.remarks().back().succeeded());
    REQUIRE(remarks.remarks().back().vectorizer ==
            pass::oir_vectorize::VectorizerKind::SLP);

    const std::vector<std::int32_t> input{-7, 0, 19, 1000};
    REQUIRE(simulate_vectorized_integer_pipeline(*fixture.function, input) ==
            simulate_scalar_integer_pipeline(input));
}

void test_three_lane_non_power_of_two_pipeline() {
    IntegerPipelineFixture fixture(3);
    pass::oir_vectorize::RemarkLog remarks;
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.minimum_lanes = 3;
    options.preferred_lanes = 3;
    options.maximum_lanes = 3;
    auto result = pass::oir_vectorize::SLPVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.packs_vectorized == 3);
    REQUIRE(result.scalar_instructions_replaced == 9);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPLoad) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPStore) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0);
    for (const auto &instruction : fixture.function->entry_block()->instructions()) {
        const auto *vp = dynamic_cast<const oir::VPInstruction *>(instruction.get());
        if (vp == nullptr) continue;
        const auto *vector_type = dynamic_cast<const oir::VectorType *>(
            dynamic_cast<const oir::VPStoreInst *>(vp) == nullptr
                ? vp->type()
                : dynamic_cast<const oir::VPStoreInst *>(vp)->value()->type());
        REQUIRE(vector_type != nullptr && vector_type->element_count().is_fixed());
        REQUIRE(vector_type->element_count().min_lanes == 3);
        auto *mask = dynamic_cast<const oir::ConstantMask *>(vp->active_mask());
        REQUIRE(mask != nullptr && mask->packed_bits().size() == 1);
        REQUIRE(mask->packed_bits()[0] == 0x07U);
    }
    const std::vector<std::int32_t> input{-11, 40, 999};
    REQUIRE(simulate_vectorized_integer_pipeline(*fixture.function, input) ==
            simulate_scalar_integer_pipeline(input));
    std::string error;
    REQUIRE(fixture.module.verify(&error));
}

void test_forced_post_verify_failure_rolls_back_text_and_use_lists() {
    IntegerPipelineFixture fixture;
    const auto before = fixture.module.print();
    const auto uses_before = snapshot_use_lists(*fixture.function);
    bool saw_transformed_module = false;
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.post_transform_verifier =
        [&](const oir::Module &, std::string &error) {
            saw_transformed_module =
                count_op(*fixture.function, oir::Instruction::OpID::Load) == 0 &&
                count_op(*fixture.function, oir::Instruction::OpID::Add) == 0 &&
                count_op(*fixture.function, oir::Instruction::OpID::Store) == 0 &&
                count_op(*fixture.function, oir::Instruction::OpID::VPLoad) == 1 &&
                count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 1 &&
                count_op(*fixture.function, oir::Instruction::OpID::VPStore) == 1 &&
                count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0;
            error = "SLP_TEST_FORCED_POST_VERIFY_FAILURE";
            return false;
        };
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(saw_transformed_module);
    REQUIRE(!result.success && !result.changed);
    REQUIRE(result.packs_vectorized == 0);
    REQUIRE(result.scalar_instructions_replaced == 0);
    REQUIRE(has_reason(result, SLPReasonCode::RejectVerification));
    REQUIRE(result.message.find("SLP_TEST_FORCED_POST_VERIFY_FAILURE") != std::string::npos);
    REQUIRE(fixture.module.print() == before);
    REQUIRE(use_lists_match(uses_before));
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Load) == 4);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Add) == 4);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Store) == 4);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPLoad) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPStore) == 0);
    std::string error;
    REQUIRE(fixture.module.verify(&error));
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

struct ExpensiveFloatFixture {
    oir::Module module{"slp_float_cost"};
    oir::Function *function = nullptr;

    ExpensiveFloatFixture() {
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.float_ty());
        std::vector<oir::Type *> params(8, types.float_ty());
        params.insert(params.end(), 4, ptr);
        function = module.create_function(
            "float_cost", types.func_ty(types.void_ty(), params));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        std::vector<oir::Value *> values;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            values.push_back(builder.create_binary(
                oir::Instruction::OpID::FAdd, function->args()[lane].get(),
                function->args()[lane + 4].get(), "fadd." + std::to_string(lane)));
        }
        for (std::size_t lane = 0; lane < 4; ++lane) {
            builder.create_store(values[lane], function->args()[lane + 8].get());
        }
        builder.create_ret();
        std::string error;
        REQUIRE(module.verify(&error));
    }
};

std::vector<float> simulate_vectorized_float_adds(
    const oir::Function &function, const std::vector<float> &arguments) {
    REQUIRE(arguments.size() == 8);
    std::unordered_map<const oir::Value *, float> scalars;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        scalars.emplace(function.args()[index].get(), arguments[index]);
    }
    std::unordered_map<const oir::Value *, std::vector<float>> vectors;
    std::function<float(const oir::Value *)> evaluate_scalar;
    std::function<const std::vector<float> &(const oir::Value *)> evaluate_vector;
    evaluate_scalar = [&](const oir::Value *value) {
        if (const auto found = scalars.find(value); found != scalars.end()) {
            return found->second;
        }
        if (const auto *constant = dynamic_cast<const oir::ConstantFloat *>(value)) {
            return constant->value();
        }
        if (const auto *extract = dynamic_cast<const oir::ExtractElementInst *>(value)) {
            auto *lane = dynamic_cast<const oir::ConstantInt *>(extract->index());
            REQUIRE(lane != nullptr && lane->value() >= 0);
            const auto &vector = evaluate_vector(extract->vector());
            REQUIRE(static_cast<std::size_t>(lane->value()) < vector.size());
            return vector[static_cast<std::size_t>(lane->value())];
        }
        throw std::runtime_error("float simulator encountered unsupported scalar value");
    };
    evaluate_vector = [&](const oir::Value *value) -> const std::vector<float> & {
        if (const auto found = vectors.find(value); found != vectors.end()) {
            return found->second;
        }
        if (const auto *undef = dynamic_cast<const oir::UndefValue *>(value)) {
            auto *type = dynamic_cast<const oir::VectorType *>(undef->type());
            REQUIRE(type != nullptr && type->element_count().is_fixed());
            return vectors.emplace(
                value, std::vector<float>(type->element_count().min_lanes, 0.0F))
                .first->second;
        }
        if (const auto *insert = dynamic_cast<const oir::InsertElementInst *>(value)) {
            auto result = evaluate_vector(insert->vector());
            auto *lane = dynamic_cast<const oir::ConstantInt *>(insert->index());
            REQUIRE(lane != nullptr && lane->value() >= 0);
            REQUIRE(static_cast<std::size_t>(lane->value()) < result.size());
            result[static_cast<std::size_t>(lane->value())] =
                evaluate_scalar(insert->element());
            return vectors.emplace(value, std::move(result)).first->second;
        }
        if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
            REQUIRE(binary->binary_op() == oir::Instruction::OpID::FAdd);
            const auto lhs = evaluate_vector(binary->lhs());
            const auto rhs = evaluate_vector(binary->rhs());
            REQUIRE(lhs.size() == rhs.size());
            std::vector<float> result(lhs.size());
            for (std::size_t lane = 0; lane < lhs.size(); ++lane) {
                result[lane] = lhs[lane] + rhs[lane];
            }
            return vectors.emplace(value, std::move(result)).first->second;
        }
        throw std::runtime_error("float simulator encountered unsupported vector value");
    };

    std::vector<float> output;
    for (const auto &instruction : function.entry_block()->instructions()) {
        if (const auto *store = dynamic_cast<const oir::StoreInst *>(instruction.get())) {
            output.push_back(evaluate_scalar(store->value()));
        }
    }
    return output;
}

void test_float_cost_reject_and_forced_transform() {
    {
        ExpensiveFloatFixture fixture;
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer{}.run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(!result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectCost));
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::FAdd) == 4);
        for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
    }
    {
        ExpensiveFloatFixture fixture;
        pass::oir_vectorize::RemarkLog remarks;
        pass::oir_vectorize::SLPVectorizerOptions options;
        options.force = true;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(result.changed);
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::FAdd) == 0);
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 1);
        bool saw_float_vector = false;
        for (const auto &instruction : fixture.function->entry_block()->instructions()) {
            if (const auto *binary =
                    dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
                saw_float_vector |= binary->binary_op() == oir::Instruction::OpID::FAdd &&
                                    binary->type()->is_fixed_vector();
            }
        }
        REQUIRE(saw_float_vector);
        const std::vector<float> arguments{1.0F, -2.0F, 3.5F, 0.0F,
                                           4.0F,  8.0F, -1.5F, 7.0F};
        const std::vector<float> expected{5.0F, 6.0F, 2.0F, 7.0F};
        REQUIRE(simulate_vectorized_float_adds(*fixture.function, arguments) == expected);
        std::string error;
        REQUIRE(fixture.module.verify(&error));
    }
}

oir::Function *build_four_adds(oir::Module &module, const std::string &name) {
    auto &types = module.types();
    std::vector<oir::Type *> params(8, types.int32_ty());
    auto *function = module.create_function(name, types.func_ty(types.void_ty(), params));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    for (std::size_t lane = 0; lane < 4; ++lane) {
        builder.create_binary(oir::Instruction::OpID::Add,
                              function->args()[lane].get(),
                              function->args()[lane + 4].get(),
                              "add." + std::to_string(lane));
    }
    return function;
}

void test_force_does_not_bypass_call_or_trap_legality() {
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.force = true;
    {
        oir::Module module{"slp_call_reject"};
        auto *function = build_four_adds(module, "with_call");
        auto &types = module.types();
        auto *callee = module.create_function(
            "opaque", types.func_ty(types.void_ty(), {}), true);
        oir::IRBuilder builder(&module);
        builder.set_insert_point(function->entry_block());
        builder.create_call(callee, types.void_ty(), {});
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(!result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectCall));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPBinary) == 0);
    }
    {
        oir::Module module{"slp_trap_reject"};
        auto *function = build_four_adds(module, "with_division");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(function->entry_block());
        builder.create_binary(oir::Instruction::OpID::SDiv,
                              function->args()[0].get(), builder.i32(1), "division");
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(!result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectPotentialTrap));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPBinary) == 0);
    }
}

void test_force_does_not_bypass_target_or_alias_legality() {
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.force = true;
    {
        oir::Module module{"slp_target_reject"};
        auto *function = build_four_adds(module, "scalar_target");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(function->entry_block());
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, scalar_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(!result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectTargetFeature));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPBinary) == 0);
    }
    {
        oir::Module module{"slp_alias_reject"};
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        auto *function = module.create_function(
            "unknown_alias", types.func_ty(types.void_ty(), {ptr, ptr, ptr, ptr}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            builder.create_load(function->args()[lane].get(), types.int32_ty(),
                                "load." + std::to_string(lane));
        }
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success);
        REQUIRE(!result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectAlias));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPLoad) == 0);
    }
}

pass::oir_vectorize::SLPVectorizerOptions exact_three_lane_options() {
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.force = true;
    options.minimum_lanes = 3;
    options.preferred_lanes = 3;
    options.maximum_lanes = 3;
    return options;
}

void test_gap_and_unknown_alias_are_rejected_without_mutation() {
    oir::Module module{"slp_gap_reject"};
    auto &types = module.types();
    auto *ptr = types.ptr_ty(types.int32_ty());
    auto *function = module.create_function(
        "gapped_loads", types.func_ty(types.void_ty(), {ptr}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    for (const int lane : {0, 1, 3}) {
        auto *address = builder.create_gep(function->args()[0].get(), ptr,
                                           {builder.i32(lane)},
                                           "address." + std::to_string(lane));
        builder.create_load(address, types.int32_ty(),
                            "load." + std::to_string(lane));
    }
    builder.create_ret();
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(exact_three_lane_options()).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectAlias));
    REQUIRE(count_op(*function, oir::Instruction::OpID::Load) == 3);
    REQUIRE(count_op(*function, oir::Instruction::OpID::VPLoad) == 0);
    REQUIRE(module.print() == before);
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

void test_ssa_dependence_is_rejected_even_when_forced() {
    oir::Module module{"slp_dependence_reject"};
    auto &types = module.types();
    auto *function = module.create_function(
        "dependent_adds", types.func_ty(types.void_ty(), {types.int32_ty()}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *one = builder.i32(1);
    auto *add0 = builder.create_binary(oir::Instruction::OpID::Add,
                                       function->args()[0].get(), one, "add.0");
    auto *add1 = builder.create_binary(oir::Instruction::OpID::Add, add0, one, "add.1");
    builder.create_binary(oir::Instruction::OpID::Add, add1, one, "add.2");
    builder.create_ret();
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(exact_three_lane_options()).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectDependence));
    REQUIRE(count_op(*function, oir::Instruction::OpID::Add) == 3);
    REQUIRE(count_op(*function, oir::Instruction::OpID::VPBinary) == 0);
    REQUIRE(module.print() == before);
}

void test_memory_barrier_prevents_load_reordering() {
    oir::Module module{"slp_memory_reorder_reject"};
    auto &types = module.types();
    auto *ptr = types.ptr_ty(types.int32_ty());
    auto *function = module.create_function(
        "intervening_store", types.func_ty(types.void_ty(), {ptr, ptr}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    for (int lane = 0; lane < 2; ++lane) {
        auto *address = builder.create_gep(function->args()[0].get(), ptr,
                                           {builder.i32(lane)},
                                           "before." + std::to_string(lane));
        builder.create_load(address, types.int32_ty(),
                            "load.before." + std::to_string(lane));
    }
    builder.create_store(builder.i32(7), function->args()[1].get());
    auto *last_address = builder.create_gep(function->args()[0].get(), ptr,
                                            {builder.i32(2)}, "after.2");
    builder.create_load(last_address, types.int32_ty(), "load.after.2");
    builder.create_ret();
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(exact_three_lane_options()).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectMemoryOrder));
    REQUIRE(std::string(pass::oir_vectorize::slp_reason_code_name(
                SLPReasonCode::RejectMemoryOrder)) == "SLP_REJECT_MEMORY_ORDER");
    REQUIRE(count_op(*function, oir::Instruction::OpID::Load) == 3);
    REQUIRE(count_op(*function, oir::Instruction::OpID::VPLoad) == 0);
    REQUIRE(module.print() == before);
}

void append_contiguous_i32_store(oir::IRBuilder &builder, oir::Function &function,
                                 oir::PointerType *ptr, int lane) {
    auto *address = builder.create_gep(function.args()[0].get(), ptr,
                                       {builder.i32(lane)},
                                       "store.address." + std::to_string(lane));
    builder.create_store(builder.i32(10 + lane), address);
}

void test_store_pack_does_not_cross_load_call_or_trap() {
    const auto options = exact_three_lane_options();
    {
        oir::Module module{"slp_store_load_barrier"};
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        auto *function = module.create_function(
            "store_across_load", types.func_ty(types.void_ty(), {ptr, ptr}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        append_contiguous_i32_store(builder, *function, ptr, 0);
        builder.create_load(function->args()[1].get(), types.int32_ty(), "barrier.load");
        append_contiguous_i32_store(builder, *function, ptr, 1);
        append_contiguous_i32_store(builder, *function, ptr, 2);
        builder.create_ret();
        const auto before = module.print();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectMemoryOrder));
        REQUIRE(count_op(*function, oir::Instruction::OpID::Store) == 3);
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPStore) == 0);
        REQUIRE(module.print() == before);
    }
    {
        oir::Module module{"slp_store_call_barrier"};
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        auto *function = module.create_function(
            "store_across_call", types.func_ty(types.void_ty(), {ptr}));
        auto *callee = module.create_function(
            "opaque", types.func_ty(types.void_ty(), {}), true);
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        append_contiguous_i32_store(builder, *function, ptr, 0);
        builder.create_call(callee, types.void_ty(), {});
        append_contiguous_i32_store(builder, *function, ptr, 1);
        append_contiguous_i32_store(builder, *function, ptr, 2);
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectCall));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPStore) == 0);
    }
    {
        oir::Module module{"slp_store_trap_barrier"};
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        auto *function = module.create_function(
            "store_across_trap",
            types.func_ty(types.void_ty(), {ptr, types.int32_ty(), types.int32_ty()}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        append_contiguous_i32_store(builder, *function, ptr, 0);
        builder.create_binary(oir::Instruction::OpID::SDiv,
                              function->args()[1].get(), function->args()[2].get(),
                              "barrier.div");
        append_contiguous_i32_store(builder, *function, ptr, 1);
        append_contiguous_i32_store(builder, *function, ptr, 2);
        builder.create_ret();
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectPotentialTrap));
        REQUIRE(count_op(*function, oir::Instruction::OpID::VPStore) == 0);
    }
}

void test_unsupported_pack_type_has_stable_reason() {
    oir::Module module{"slp_unsupported_type"};
    auto &types = module.types();
    std::vector<oir::Type *> params(6, types.int1_ty());
    auto *function = module.create_function(
        "i1_is_out_of_scope", types.func_ty(types.void_ty(), params));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    for (std::size_t lane = 0; lane < 3; ++lane) {
        builder.create_binary(oir::Instruction::OpID::Add,
                              function->args()[lane].get(),
                              function->args()[lane + 3].get(),
                              "wide.add." + std::to_string(lane));
    }
    builder.create_ret();
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(exact_three_lane_options()).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectUnsupportedType));
    REQUIRE(std::string(pass::oir_vectorize::slp_reason_code_name(
                SLPReasonCode::RejectUnsupportedType)) == "SLP_REJECT_UNSUPPORTED_TYPE");
    REQUIRE(module.print() == before);
    REQUIRE(!remarks.empty());
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

void test_non_plain_memory_is_fail_closed() {
    oir::Module module{"slp_memory_semantics"};
    auto &types = module.types();
    auto *ptr = types.ptr_ty(types.int32_ty());
    std::vector<oir::Type *> params(8, types.int32_ty());
    params.push_back(ptr);
    auto *function = module.create_function(
        "bulk_memory", types.func_ty(types.void_ty(), params));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    for (std::size_t lane = 0; lane < 4; ++lane) {
        builder.create_binary(oir::Instruction::OpID::Add,
                              function->args()[lane].get(),
                              function->args()[lane + 4].get(),
                              "add." + std::to_string(lane));
    }
    builder.create_memzero(function->args()[8].get(), builder.i32(16));
    builder.create_ret();
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.force = true;
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(options).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success);
    REQUIRE(!result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectMemorySemantics));
    REQUIRE(std::string(pass::oir_vectorize::slp_reason_code_name(
                SLPReasonCode::RejectMemorySemantics)) == "SLP_REJECT_MEMORY_SEMANTICS");
}

pass::oir_vectorize::SLPVectorizerOptions forced_lane_options(unsigned lanes) {
    pass::oir_vectorize::SLPVectorizerOptions options;
    options.force = true;
    options.minimum_lanes = lanes;
    options.preferred_lanes = lanes;
    options.maximum_lanes = lanes;
    return options;
}

struct MaskPipelineFixture {
    oir::Module module;
    oir::Function *function = nullptr;
    unsigned lanes = 0;

    explicit MaskPipelineFixture(unsigned lane_count)
        : module("slp_mask_pipeline_" + std::to_string(lane_count)), lanes(lane_count) {
        auto &types = module.types();
        auto *ptr = types.ptr_ty(types.int32_ty());
        function = module.create_function(
            "mask_pipeline", types.func_ty(types.void_ty(), {ptr}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);

        std::vector<oir::Value *> loaded;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            auto *address = builder.create_gep(
                function->args()[0].get(), ptr,
                {builder.i32(static_cast<std::int64_t>(lane))},
                "input." + std::to_string(lane));
            loaded.push_back(builder.create_load(address, types.int32_ty(),
                                                 "load." + std::to_string(lane)));
        }
        std::vector<oir::Value *> added;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            added.push_back(builder.create_binary(
                oir::Instruction::OpID::Add, loaded[lane], builder.i32(1),
                "add." + std::to_string(lane)));
        }
        std::vector<oir::Value *> compared;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            compared.push_back(builder.create_icmp(
                oir::CmpPred::GT, added[lane],
                builder.i32(static_cast<std::int64_t>(lane * 2U)),
                "compare." + std::to_string(lane)));
        }
        std::vector<oir::Value *> xored;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            xored.push_back(builder.create_binary(
                oir::Instruction::OpID::Xor, compared[lane], builder.i1(lane % 2U != 0U),
                "xor." + std::to_string(lane)));
        }
        for (unsigned lane = 0; lane < lanes; ++lane) {
            builder.create_binary(oir::Instruction::OpID::Or, xored[lane],
                                  builder.i1(lane % 3U == 0U),
                                  "or." + std::to_string(lane));
        }
        builder.create_ret();
        std::string error;
        REQUIRE(module.verify(&error));
    }
};

std::vector<std::int64_t>
simulate_mask_pipeline(const oir::Function &function,
                       const std::vector<std::int64_t> &input) {
    std::unordered_map<const oir::Value *, std::vector<std::int64_t>> vectors;
    std::function<const std::vector<std::int64_t> &(const oir::Value *)> evaluate;
    evaluate = [&](const oir::Value *value) -> const std::vector<std::int64_t> & {
        if (const auto found = vectors.find(value); found != vectors.end())
            return found->second;
        if (const auto *load = dynamic_cast<const oir::VPLoadInst *>(value)) {
            auto *type = dynamic_cast<const oir::VectorType *>(load->type());
            REQUIRE(type != nullptr && type->element_count().is_fixed());
            REQUIRE(type->element_count().min_lanes == input.size());
            return vectors.emplace(value, input).first->second;
        }
        if (const auto *undef = dynamic_cast<const oir::UndefValue *>(value)) {
            auto *type = dynamic_cast<const oir::VectorType *>(undef->type());
            REQUIRE(type != nullptr && type->element_count().is_fixed());
            return vectors
                .emplace(value, std::vector<std::int64_t>(type->element_count().min_lanes, 0))
                .first->second;
        }
        if (const auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
            auto *type = dynamic_cast<const oir::VectorType *>(splat->type());
            auto *constant = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
            REQUIRE(type != nullptr && type->element_count().is_fixed() && constant != nullptr);
            return vectors
                .emplace(value, std::vector<std::int64_t>(
                                    type->element_count().min_lanes, constant->value()))
                .first->second;
        }
        if (const auto *insert = dynamic_cast<const oir::InsertElementInst *>(value)) {
            auto result = evaluate(insert->vector());
            auto *lane = dynamic_cast<const oir::ConstantInt *>(insert->index());
            auto *element = dynamic_cast<const oir::ConstantInt *>(insert->element());
            REQUIRE(lane != nullptr && lane->value() >= 0 && element != nullptr);
            REQUIRE(static_cast<std::size_t>(lane->value()) < result.size());
            result[static_cast<std::size_t>(lane->value())] = element->value();
            return vectors.emplace(value, std::move(result)).first->second;
        }
        if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
            const auto lhs = evaluate(binary->lhs());
            const auto rhs = evaluate(binary->rhs());
            REQUIRE(lhs.size() == rhs.size());
            std::vector<std::int64_t> result(lhs.size());
            for (std::size_t lane = 0; lane < lhs.size(); ++lane) {
                switch (binary->binary_op()) {
                case oir::Instruction::OpID::Add:
                    result[lane] = lhs[lane] + rhs[lane];
                    break;
                case oir::Instruction::OpID::Xor:
                    result[lane] = lhs[lane] ^ rhs[lane];
                    break;
                case oir::Instruction::OpID::Or:
                    result[lane] = lhs[lane] | rhs[lane];
                    break;
                default:
                    throw std::runtime_error("unsupported simulated SLP mask binary");
                }
            }
            return vectors.emplace(value, std::move(result)).first->second;
        }
        if (const auto *compare = dynamic_cast<const oir::VPCmpInst *>(value)) {
            const auto lhs = evaluate(compare->lhs());
            const auto rhs = evaluate(compare->rhs());
            REQUIRE(lhs.size() == rhs.size());
            REQUIRE(compare->comparison_op() == oir::Instruction::OpID::ICmp);
            REQUIRE(compare->pred() == oir::CmpPred::GT);
            std::vector<std::int64_t> result(lhs.size());
            for (std::size_t lane = 0; lane < lhs.size(); ++lane)
                result[lane] = lhs[lane] > rhs[lane] ? 1 : 0;
            return vectors.emplace(value, std::move(result)).first->second;
        }
        throw std::runtime_error("unsupported simulated SLP mask vector");
    };

    const oir::VPBinaryInst *final_or = nullptr;
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (const auto *binary =
                    dynamic_cast<const oir::VPBinaryInst *>(instruction.get());
                binary != nullptr && binary->binary_op() == oir::Instruction::OpID::Or &&
                binary->type()->is_mask()) {
                final_or = binary;
            }
        }
    }
    REQUIRE(final_or != nullptr);
    return evaluate(final_or);
}

void check_mask_pipeline(unsigned lanes) {
    MaskPipelineFixture fixture(lanes);
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(forced_lane_options(lanes)).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.packs_vectorized == 5);
    REQUIRE(result.scalar_instructions_replaced == lanes * 5U);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Load) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::Add) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::ICmp) == 0);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPLoad) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPCmp) == 1);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 3);
    REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0);

    const oir::VPLoadInst *load = nullptr;
    const oir::VPBinaryInst *add = nullptr;
    const oir::VPCmpInst *compare = nullptr;
    const oir::VPBinaryInst *xored = nullptr;
    const oir::VPBinaryInst *ored = nullptr;
    for (const auto &instruction : fixture.function->entry_block()->instructions()) {
        if (const auto *candidate = dynamic_cast<const oir::VPLoadInst *>(instruction.get()))
            load = candidate;
        if (const auto *candidate = dynamic_cast<const oir::VPCmpInst *>(instruction.get()))
            compare = candidate;
        if (const auto *candidate = dynamic_cast<const oir::VPBinaryInst *>(instruction.get())) {
            if (candidate->binary_op() == oir::Instruction::OpID::Add)
                add = candidate;
            else if (candidate->binary_op() == oir::Instruction::OpID::Xor)
                xored = candidate;
            else if (candidate->binary_op() == oir::Instruction::OpID::Or)
                ored = candidate;
        }
    }
    REQUIRE(load != nullptr && add != nullptr && compare != nullptr && xored != nullptr &&
            ored != nullptr);
    REQUIRE(add->lhs() == load);
    REQUIRE(compare->lhs() == add);
    REQUIRE(xored->lhs() == compare);
    REQUIRE(ored->lhs() == xored);
    for (const auto &instruction : fixture.function->entry_block()->instructions()) {
        if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(instruction.get())) {
            auto *evl = dynamic_cast<const oir::ConstantInt *>(vp->evl());
            auto *active = dynamic_cast<const oir::ConstantMask *>(vp->active_mask());
            REQUIRE(evl != nullptr && evl->value() == static_cast<std::int64_t>(lanes));
            REQUIRE(active != nullptr && active->lane_count() == lanes);
            for (std::uint64_t lane = 0; lane < lanes; ++lane) REQUIRE(active->lane(lane));
        }
    }

    std::vector<std::int64_t> input;
    std::vector<std::int64_t> expected;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        input.push_back(static_cast<std::int64_t>(lane * 4U) - 3);
        const auto compared = input.back() + 1 > static_cast<std::int64_t>(lane * 2U);
        const auto xored_lane = static_cast<std::int64_t>(compared) ^
                                static_cast<std::int64_t>(lane % 2U != 0U);
        expected.push_back(xored_lane |
                           static_cast<std::int64_t>(lane % 3U == 0U));
    }
    REQUIRE(simulate_mask_pipeline(*fixture.function, input) == expected);
    std::string error;
    REQUIRE(fixture.module.verify(&error));
    REQUIRE(!remarks.empty() && remarks.remarks().back().succeeded());
}

void test_compare_mask_tree_reuses_producers_n3_n7() {
    check_mask_pipeline(3);
    check_mask_pipeline(7);
}

struct FloatCompareFixture {
    oir::Module module{"slp_float_compare"};
    oir::Function *function = nullptr;

    FloatCompareFixture() {
        auto &types = module.types();
        std::vector<oir::Type *> params(6, types.float_ty());
        function = module.create_function(
            "float_compare", types.func_ty(types.void_ty(), params));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        for (std::size_t lane = 0; lane < 3; ++lane) {
            builder.create_fcmp(oir::CmpPred::LE, function->args()[lane].get(),
                                function->args()[lane + 3].get(),
                                "fcmp." + std::to_string(lane));
        }
        builder.create_ret();
        std::string error;
        REQUIRE(module.verify(&error));
    }
};

void test_float_compare_cost_and_forced_transform() {
    {
        FloatCompareFixture fixture;
        const auto before = fixture.module.print();
        auto options = forced_lane_options(3);
        options.force = false;
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(options).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && !result.changed);
        REQUIRE(has_reason(result, SLPReasonCode::RejectCost));
        REQUIRE(fixture.module.print() == before);
        REQUIRE(!remarks.empty());
        for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
    }
    {
        FloatCompareFixture fixture;
        pass::oir_vectorize::RemarkLog remarks;
        auto result = pass::oir_vectorize::SLPVectorizer(forced_lane_options(3)).run(
            fixture.module, rvv_profile(), remarks);
        REQUIRE(result.success && result.changed);
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::FCmp) == 0);
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::VPCmp) == 1);
        REQUIRE(count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0);
        const oir::VPCmpInst *compare = nullptr;
        for (const auto &instruction : fixture.function->entry_block()->instructions())
            if (const auto *candidate = dynamic_cast<const oir::VPCmpInst *>(instruction.get()))
                compare = candidate;
        REQUIRE(compare != nullptr && compare->comparison_op() == oir::Instruction::OpID::FCmp);
        REQUIRE(compare->pred() == oir::CmpPred::LE);
        REQUIRE(compare->type()->is_mask());
        std::string error;
        REQUIRE(fixture.module.verify(&error));
        REQUIRE(!remarks.empty() && remarks.remarks().back().succeeded());
    }
}

void check_extract_pack_is_not_reused(unsigned producer_lanes,
                                      const std::vector<unsigned> &order) {
    oir::Module module{"slp_extract_order"};
    auto &types = module.types();
    auto *function = module.create_function(
        "extract_order", types.func_ty(types.void_ty(), {types.int32_ty()}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *vector_type = types.fixed_vector_ty(types.int32_ty(), producer_lanes);
    auto *producer = builder.create_splat(vector_type, function->args()[0].get(), "producer");
    std::vector<oir::Value *> extracted;
    for (auto lane : order) {
        extracted.push_back(builder.create_extract_element(
            producer, builder.i32(static_cast<std::int64_t>(lane)),
            "extract." + std::to_string(lane)));
    }
    for (std::size_t lane = 0; lane < extracted.size(); ++lane) {
        builder.create_binary(oir::Instruction::OpID::Add, extracted[lane], builder.i32(1),
                              "add." + std::to_string(lane));
    }
    builder.create_ret();
    std::string error;
    REQUIRE(module.verify(&error));
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(forced_lane_options(order.size())).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && result.changed);
    const oir::VPBinaryInst *vector_add = nullptr;
    for (const auto &instruction : entry->instructions())
        if (const auto *candidate = dynamic_cast<const oir::VPBinaryInst *>(instruction.get()))
            vector_add = candidate;
    REQUIRE(vector_add != nullptr);
    REQUIRE(vector_add->lhs() != producer);
    REQUIRE(dynamic_cast<const oir::InsertElementInst *>(vector_add->lhs()) != nullptr);
    REQUIRE(count_op(*function, oir::Instruction::OpID::InsertElement) >= order.size());
    REQUIRE(module.verify(&error));
}

void test_reordered_and_shape_mismatched_extracts_do_not_reuse() {
    check_extract_pack_is_not_reused(3, {1, 0, 2});
    check_extract_pack_is_not_reused(7, {0, 1, 2});
}

void test_mask_dependence_chain_is_rejected_when_forced() {
    oir::Module module{"slp_mask_dependence"};
    auto &types = module.types();
    std::vector<oir::Type *> params(4, types.int1_ty());
    auto *function = module.create_function(
        "mask_dependence", types.func_ty(types.void_ty(), params));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *and0 = builder.create_binary(oir::Instruction::OpID::And,
                                       function->args()[0].get(), function->args()[1].get(),
                                       "and.0");
    auto *and1 = builder.create_binary(oir::Instruction::OpID::And, and0,
                                       function->args()[2].get(), "and.1");
    builder.create_binary(oir::Instruction::OpID::And, and1,
                          function->args()[3].get(), "and.2");
    builder.create_ret();
    std::string error;
    REQUIRE(module.verify(&error));
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(forced_lane_options(3)).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectDependence));
    REQUIRE(count_op(*function, oir::Instruction::OpID::And) == 3);
    REQUIRE(count_op(*function, oir::Instruction::OpID::VPBinary) == 0);
    REQUIRE(module.print() == before);
}

void test_mixed_compare_predicates_do_not_pack() {
    oir::Module module{"slp_mixed_compare_predicates"};
    auto &types = module.types();
    std::vector<oir::Type *> params(6, types.int32_ty());
    auto *function = module.create_function(
        "mixed_predicates", types.func_ty(types.void_ty(), params));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_icmp(oir::CmpPred::GT, function->args()[0].get(),
                        function->args()[3].get(), "compare.0");
    builder.create_icmp(oir::CmpPred::LT, function->args()[1].get(),
                        function->args()[4].get(), "compare.1");
    builder.create_icmp(oir::CmpPred::GT, function->args()[2].get(),
                        function->args()[5].get(), "compare.2");
    builder.create_ret();
    std::string error;
    REQUIRE(module.verify(&error));
    const auto before = module.print();
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(forced_lane_options(3)).run(
        module, rvv_profile(), remarks);
    REQUIRE(result.success && !result.changed);
    REQUIRE(count_op(*function, oir::Instruction::OpID::ICmp) == 3);
    REQUIRE(count_op(*function, oir::Instruction::OpID::VPCmp) == 0);
    REQUIRE(module.print() == before);
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

void test_mask_pipeline_post_verify_failure_rolls_back() {
    MaskPipelineFixture fixture(3);
    const auto before = fixture.module.print();
    const auto uses = snapshot_use_lists(*fixture.function);
    bool saw_new_packs = false;
    auto options = forced_lane_options(3);
    options.post_transform_verifier = [&](const oir::Module &, std::string &error) {
        saw_new_packs = count_op(*fixture.function, oir::Instruction::OpID::VPCmp) == 1 &&
                        count_op(*fixture.function, oir::Instruction::OpID::VPBinary) == 3 &&
                        count_op(*fixture.function, oir::Instruction::OpID::ExtractElement) == 0;
        error = "SLP2_TEST_POST_VERIFY_FAILURE";
        return false;
    };
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(saw_new_packs);
    REQUIRE(!result.success && !result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectVerification));
    REQUIRE(result.message.find("SLP2_TEST_POST_VERIFY_FAILURE") != std::string::npos);
    REQUIRE(fixture.module.print() == before);
    REQUIRE(use_lists_match(uses));
    std::string error;
    REQUIRE(fixture.module.verify(&error));
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

void test_invalid_input_never_emits_success() {
    oir::Module module{"slp_invalid"};
    auto &types = module.types();
    auto *function = module.create_function(
        "invalid", types.func_ty(types.void_ty(), {}));
    function->create_block("unterminated");
    pass::oir_vectorize::RemarkLog remarks;
    auto result = pass::oir_vectorize::SLPVectorizer{}.run(
        module, rvv_profile(), remarks);
    REQUIRE(!result.success);
    REQUIRE(!result.changed);
    REQUIRE(has_reason(result, SLPReasonCode::RejectVerification));
    for (const auto &remark : remarks.remarks()) REQUIRE(!remark.succeeded());
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"contiguous_integer_pipeline_rewrites_ssa_and_memory",
         test_contiguous_integer_pipeline_rewrites_ssa_and_memory},
        {"three_lane_non_power_of_two_pipeline",
         test_three_lane_non_power_of_two_pipeline},
        {"forced_post_verify_failure_rolls_back_text_and_use_lists",
         test_forced_post_verify_failure_rolls_back_text_and_use_lists},
        {"float_cost_reject_and_forced_transform",
         test_float_cost_reject_and_forced_transform},
        {"force_does_not_bypass_call_or_trap_legality",
         test_force_does_not_bypass_call_or_trap_legality},
        {"force_does_not_bypass_target_or_alias_legality",
         test_force_does_not_bypass_target_or_alias_legality},
        {"gap_and_unknown_alias_are_rejected_without_mutation",
         test_gap_and_unknown_alias_are_rejected_without_mutation},
        {"ssa_dependence_is_rejected_even_when_forced",
         test_ssa_dependence_is_rejected_even_when_forced},
        {"memory_barrier_prevents_load_reordering",
         test_memory_barrier_prevents_load_reordering},
        {"store_pack_does_not_cross_load_call_or_trap",
         test_store_pack_does_not_cross_load_call_or_trap},
        {"unsupported_pack_type_has_stable_reason",
         test_unsupported_pack_type_has_stable_reason},
        {"non_plain_memory_is_fail_closed", test_non_plain_memory_is_fail_closed},
        {"compare_mask_tree_reuses_producers_n3_n7",
         test_compare_mask_tree_reuses_producers_n3_n7},
        {"float_compare_cost_and_forced_transform",
         test_float_compare_cost_and_forced_transform},
        {"reordered_and_shape_mismatched_extracts_do_not_reuse",
         test_reordered_and_shape_mismatched_extracts_do_not_reuse},
        {"mask_dependence_chain_is_rejected_when_forced",
         test_mask_dependence_chain_is_rejected_when_forced},
        {"mixed_compare_predicates_do_not_pack",
         test_mixed_compare_predicates_do_not_pack},
        {"mask_pipeline_post_verify_failure_rolls_back",
         test_mask_pipeline_post_verify_failure_rolls_back},
        {"invalid_input_never_emits_success", test_invalid_input_never_emits_success},
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
    with tempfile.TemporaryDirectory(prefix="slp-vectorization-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "slp_vectorization_tests.cpp"
        binary = tmp_dir / "slp_vectorization_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        command = [
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
            str(ROOT / "src/pass/oir/OIRSLPVectorizer.cpp"),
            str(ROOT / "src/pass/oir/OIRVectorizationRemark.cpp"),
            str(ROOT / "src/target/TargetMachine.cpp"),
            "-o",
            str(binary),
        ]
        compiled = subprocess.run(
            command, cwd=ROOT, text=True, capture_output=True, check=False
        )
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        ran = subprocess.run(
            [str(binary)], cwd=ROOT, text=True, capture_output=True, check=False
        )
        print(ran.stdout, end="")
        print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
