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
#include "oir/OIRDataLayout.h"
#include "yir/Presburger.h"

#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
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

oir::Function *create_function(oir::Module &module, const std::string &name,
                               oir::Type *return_type,
                               const std::vector<oir::Type *> &param_types = {}) {
    return module.create_function(name, module.types().func_ty(return_type, param_types));
}

void test_vector_type_interning_printing_and_predicates() {
    oir::TypeContext types;
    auto *i1 = types.int1_ty();
    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();

    auto *v3i32 = types.fixed_vector_ty(i32, 3);
    auto *v3i32_again = types.vector_ty(i32, oir::ElementCount{3, false});
    auto *v3f32 = types.fixed_vector_ty(f32, 3);
    auto *m10 = types.fixed_vector_ty(i1, 10);
    auto *nxv3i32 = types.scalable_vector_ty(i32, 3);
    auto *nxv3i32_again = types.vector_ty(i32, oir::ElementCount{3, true});

    REQUIRE(v3i32 == v3i32_again);
    REQUIRE(nxv3i32 == nxv3i32_again);
    REQUIRE(v3i32 != v3f32);
    REQUIRE(v3i32 != nxv3i32);
    REQUIRE(v3i32->print() == "<3 x i32>");
    REQUIRE(v3f32->print() == "<3 x float>");
    REQUIRE(m10->print() == "<10 x i1>");
    REQUIRE(nxv3i32->print() == "<vscale x 3 x i32>");

    REQUIRE(i32->is_integer());
    REQUIRE(i32->is_scalar_integer());
    REQUIRE(i32->is_scalar_numeric());
    REQUIRE(i32->is_scalar());
    REQUIRE(!i32->is_vector());
    REQUIRE(v3i32->is_vector());
    REQUIRE(v3i32->is_fixed_vector());
    REQUIRE(!v3i32->is_scalable_vector());
    REQUIRE(v3i32->is_integer_vector());
    REQUIRE(!v3i32->is_integer());
    REQUIRE(!v3i32->is_scalar_numeric());
    REQUIRE(!v3i32->is_mask());
    REQUIRE(m10->is_mask());
    REQUIRE(nxv3i32->is_scalable_vector());

    // Composite types use structural keys built from canonical type identity
    // and fields, rather than serialized type spelling.
    REQUIRE(types.ptr_ty(v3i32) == types.ptr_ty(v3i32_again));
    REQUIRE(types.array_ty(v3i32, 7) == types.array_ty(v3i32_again, 7));
    REQUIRE(types.func_ty(v3i32, {m10}) == types.func_ty(v3i32_again, {m10}));
    auto *fixed_function = types.func_ty(types.void_ty(), {i32});
    auto *variadic_function = types.func_ty(types.void_ty(), {i32}, true);
    REQUIRE(fixed_function != variadic_function);
    REQUIRE(variadic_function == types.func_ty(types.void_ty(), {i32}, true));
    REQUIRE(variadic_function->is_variadic());
    REQUIRE(variadic_function->print() == "void (i32, ...)");
}

void test_vector_type_rejects_illegal_element_counts() {
    bool rejected_fixed_zero = false;
    try {
        (void)oir::ElementCount{0, false};
    } catch (const std::invalid_argument &) {
        rejected_fixed_zero = true;
    }
    REQUIRE(rejected_fixed_zero);

    bool rejected_scalable_zero = false;
    try {
        (void)oir::ElementCount{0, true};
    } catch (const std::invalid_argument &) {
        rejected_scalable_zero = true;
    }
    REQUIRE(rejected_scalable_zero);

    oir::TypeContext types;
    bool rejected_pointer_element = false;
    try {
        (void)types.fixed_vector_ty(types.ptr_ty(types.int32_ty()), 4);
    } catch (const std::invalid_argument &) {
        rejected_pointer_element = true;
    }
    REQUIRE(rejected_pointer_element);
}

void test_data_layout_fixed_scalable_and_mask_storage() {
    oir::Module module("data_layout");
    auto &types = module.types();
    oir::DataLayout layout;

    auto *v3i32 = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *v5f32 = types.fixed_vector_ty(types.float_ty(), 5);
    auto *m10 = types.fixed_vector_ty(types.int1_ty(), 10);
    auto *nxv3i32 = types.scalable_vector_ty(types.int32_ty(), 3);
    auto *nxm10 = types.scalable_vector_ty(types.int1_ty(), 10);

    REQUIRE(layout.store_size(types.int1_ty()) == oir::TypeSize::fixed(1));
    REQUIRE(layout.store_size(types.int32_ty()) == oir::TypeSize::fixed(4));
    REQUIRE(layout.store_size(types.float_ty()) == oir::TypeSize::fixed(4));
    REQUIRE(layout.store_size(v3i32) == oir::TypeSize::fixed(12));
    REQUIRE(layout.store_size(v5f32) == oir::TypeSize::fixed(20));
    REQUIRE(layout.store_size(m10) == oir::TypeSize::fixed(2));
    REQUIRE(layout.abi_alignment(m10) == 1);
    REQUIRE(layout.abi_alignment(v3i32) == 4);

    auto scalable_numeric_size = layout.store_size(nxv3i32);
    REQUIRE(scalable_numeric_size.is_scalable());
    REQUIRE(scalable_numeric_size.minimum_bytes() == 12);
    REQUIRE(!scalable_numeric_size.fixed_bytes().has_value());
    REQUIRE(!layout.fixed_store_size(nxv3i32).has_value());
    REQUIRE(!layout.fixed_alloc_size(nxv3i32).has_value());

    auto scalable_mask_size = layout.store_size(nxm10);
    REQUIRE(scalable_mask_size.is_scalable());
    REQUIRE(scalable_mask_size.minimum_bytes() == 2);
    REQUIRE(layout.abi_alignment(nxm10) == 1);

    auto *mask_array = types.array_ty(m10, 3);
    REQUIRE(layout.store_size(mask_array) == oir::TypeSize::fixed(6));
    REQUIRE(!layout.is_sized(types.void_ty()));
    REQUIRE(!layout.fixed_store_size(types.void_ty()).has_value());

    // AliasAnalysis now consumes DataLayout.  Fixed masks have a packed
    // two-byte location; scalable locations deliberately have no fixed size.
    auto *fixed_mask_global = module.create_global("fixed_mask", m10, false);
    auto *scalable_global = module.create_global("scalable_mask", nxm10, false);
    oir::OIRAliasAnalysis aa;
    REQUIRE(aa.memory_location(fixed_mask_global).size == 2);
    REQUIRE(!aa.memory_location(scalable_global).size.has_value());
}

void test_typed_constant_initializers_print_and_verify() {
    static_assert(std::is_abstract_v<oir::Constant>);

    oir::Module module("typed_constants");
    auto &types = module.types();
    auto *one = module.create_i32(1);
    auto *minus_two = module.create_i32(-2);
    auto *three = module.create_i32(3);

    auto *array_type = types.array_ty(types.int32_ty(), 3);
    auto *array = module.create_constant_array(array_type, {one, minus_two, three});
    auto *array_global = module.create_global("numbers", array_type, true, array);
    REQUIRE(array_global->initializer() == array);
    REQUIRE(array_global->init_value() == array);
    REQUIRE(array->array_type() == array_type);
    REQUIRE(array->elements().size() == 3);

    auto *vector_type = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *vector = module.create_constant_vector(vector_type, {one, minus_two, three});
    module.create_global("lanes", vector_type, false, vector);
    REQUIRE(vector->vector_type() == vector_type);

    auto *mask_type = types.fixed_vector_ty(types.int1_ty(), 10);
    auto *mask = module.create_constant_mask(mask_type, {0x55U, 0x03U});
    module.create_global("mask", mask_type, true, mask);
    REQUIRE(mask->lane_count() == 10);
    REQUIRE(mask->lane(0));
    REQUIRE(!mask->lane(1));
    REQUIRE(mask->lane(8));
    REQUIRE(mask->lane(9));
    REQUIRE(mask->packed_bits().size() == 2);

    auto *zero = module.create_zero(array_type);
    module.create_global("zeros", array_type, false, zero);

    module.create_global("tentative", types.int32_ty(), false);

    const auto printed = module.print();
    REQUIRE(printed.find("@numbers = constant [3 x i32] [i32 1, i32 -2, i32 3]") !=
            std::string::npos);
    REQUIRE(printed.find("@lanes = global <3 x i32> <i32 1, i32 -2, i32 3>") !=
            std::string::npos);
    REQUIRE(printed.find("@mask = constant <10 x i1> <i1 1, i1 0, i1 1, i1 0, i1 1, "
                         "i1 0, i1 1, i1 0, i1 1, i1 1>") != std::string::npos);
    REQUIRE(printed.find("@zeros = global [3 x i32] zero") != std::string::npos);
    REQUIRE(printed.find("@tentative = global i32\n") != std::string::npos);
    require_verify(module);
}

void test_typed_constant_constructors_reject_invalid_shapes() {
    oir::Module module("bad_typed_constants");
    auto &types = module.types();
    auto rejects = [](auto &&action) {
        try {
            action();
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };

    auto *one = module.create_i32(1);
    auto *two = module.create_i32(2);
    auto *float_one = module.create_f32(1.0F);
    auto *array_type = types.array_ty(types.int32_ty(), 2);
    REQUIRE(rejects([&] { (void)module.create_constant_array(array_type, {one}); }));
    REQUIRE(rejects(
        [&] { (void)module.create_constant_array(array_type, {one, float_one}); }));

    auto *vector_type = types.fixed_vector_ty(types.int32_ty(), 3);
    REQUIRE(rejects(
        [&] { (void)module.create_constant_vector(vector_type, {one, two}); }));
    REQUIRE(rejects([&] {
        (void)module.create_constant_vector(vector_type, {one, two, float_one});
    }));

    auto *mask_type = types.fixed_vector_ty(types.int1_ty(), 10);
    REQUIRE(rejects([&] { (void)module.create_constant_mask(mask_type, {0x01U}); }));
    REQUIRE(rejects(
        [&] { (void)module.create_constant_mask(mask_type, {0x00U, 0x80U}); }));
    REQUIRE(rejects([&] {
        (void)module.create_constant_vector(
            mask_type, {module.create_i1(false), module.create_i1(false),
                        module.create_i1(false), module.create_i1(false),
                        module.create_i1(false), module.create_i1(false),
                        module.create_i1(false), module.create_i1(false),
                        module.create_i1(false), module.create_i1(false)});
    }));

    auto *scalable_vector = types.scalable_vector_ty(types.int32_ty(), 3);
    REQUIRE(rejects([&] {
        (void)module.create_constant_vector(scalable_vector, {one, one, one});
    }));
    auto *scalable_mask = types.scalable_vector_ty(types.int1_ty(), 10);
    REQUIRE(rejects(
        [&] { (void)module.create_constant_mask(scalable_mask, {0x00U, 0x00U}); }));

    REQUIRE(rejects([&] { oir::ConstantInt invalid(types.int1_ty(), 2); }));
    REQUIRE(rejects([&] {
        oir::ConstantInt invalid(types.int32_ty(),
                                 static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) +
                                     1);
    }));
    REQUIRE(rejects([&] {
        (void)module.create_i32(
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) - 1);
    }));
    REQUIRE(rejects([&] { oir::ConstantFloat invalid(types.int32_ty(), 1.0F); }));
}

void test_typed_constant_verifier_rejects_bad_initializers() {
    {
        oir::Module module("wrong_initializer_type");
        module.create_global("g", module.types().int32_ty(), false,
                             module.create_f32(1.0F));
        require_verify_fails(module, "OIRV_GLOBAL_INITIALIZER_TYPE");
    }

    {
        oir::Module module("foreign_initializer");
        oir::ConstantInt foreign(module.types().int32_ty(), 1);
        module.create_global("g", module.types().int32_ty(), false, &foreign);
        require_verify_fails(module, "OIRV_CONSTANT_OWNERSHIP");
    }

    {
        oir::Module module("foreign_nested_initializer");
        oir::ConstantInt foreign(module.types().int32_ty(), 1);
        auto *array_type = module.types().array_ty(module.types().int32_ty(), 1);
        auto *array = module.create_constant_array(array_type, {&foreign});
        module.create_global("g", array_type, false, array);
        require_verify_fails(module, "OIRV_CONSTANT_OWNERSHIP");
    }

    {
        oir::Module module("scalable_initializer");
        auto *scalable =
            module.types().scalable_vector_ty(module.types().int32_ty(), 4);
        module.create_global("g", scalable, false, module.create_zero(scalable));
        require_verify_fails(module, "OIRV_SCALABLE_GLOBAL");
    }
}

void test_vector_primitives_fixed_and_scalable() {
    oir::Module module("vector_primitives");
    auto &types = module.types();
    auto *v4i32 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *v4f32 = types.fixed_vector_ty(types.float_ty(), 4);
    auto *m4 = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *nxv4i32 = types.scalable_vector_ty(types.int32_ty(), 4);
    auto *nxm4 = types.scalable_vector_ty(types.int1_ty(), 4);

    auto *function = create_function(module, "f", types.void_ty());
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);

    auto *splat = builder.create_splat(v4i32, builder.i32(7), "splat");
    auto *mask = builder.create_splat(m4, builder.i1(true), "mask");
    auto *mask_or = builder.create_binary(oir::Instruction::OpID::Or, mask, mask, "mask.or");
    auto *step = builder.create_step_vector(v4i32, "step");
    auto *extracted = builder.create_extract_element(splat, builder.i32(2), "extract");
    auto *inserted = builder.create_insert_element(step, extracted, builder.i32(1), "insert");
    auto *shuffled =
        builder.create_shuffle_vector(v4i32, inserted, splat, {0, 5, -1, 3}, "shuffle");
    auto *selected = builder.create_vector_select(mask_or, shuffled, inserted, "select");
    auto *extended = builder.create_vector_cast(oir::VectorCastKind::ZExt, v4i32, mask,
                                                "zext");
    auto *to_float = builder.create_vector_cast(oir::VectorCastKind::SIToFP, v4f32, selected,
                                                "sitofp");
    auto *to_int = builder.create_vector_cast(oir::VectorCastKind::FPToSI, v4i32, to_float,
                                              "fptosi");
    auto *bitcast = builder.create_vector_cast(oir::VectorCastKind::Bitcast, v4f32, extended,
                                               "bitcast");
    REQUIRE(to_int->type() == v4i32);
    REQUIRE(bitcast->type() == v4f32);

    auto *nx_scalar = builder.create_splat(nxv4i32, builder.i32(3), "nx.splat");
    auto *nx_step = builder.create_step_vector(nxv4i32, "nx.step");
    auto *nx_mask = builder.create_splat(nxm4, builder.i1(true), "nx.mask");
    auto *nx_select = builder.create_vector_select(nx_mask, nx_scalar, nx_step, "nx.select");
    REQUIRE(nx_select->type() == nxv4i32);
    builder.create_ret();

    require_verify(module);
    const auto printed = module.print();
    REQUIRE(printed.find("stepvector <4 x i32>") != std::string::npos);
    REQUIRE(printed.find("shufflevector") != std::string::npos);
    REQUIRE(printed.find("vector.sitofp") != std::string::npos);
    REQUIRE(printed.find("<vscale x 4 x i32>") != std::string::npos);
}

void test_vector_primitives_reject_bad_shapes_and_bounds() {
    {
        oir::Module module("bad_splat");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_splat(module.types().fixed_vector_ty(module.types().int32_ty(), 4),
                             builder.f32(1.0F), "bad");
        builder.create_ret();
        require_verify_fails(module, "OIRV_SPLAT_SHAPE");
    }

    {
        oir::Module module("bad_shuffle");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *type = module.types().fixed_vector_ty(module.types().int32_ty(), 4);
        auto *lhs = module.create_undef(type);
        auto *rhs = module.create_undef(type);
        builder.create_shuffle_vector(type, lhs, rhs, {0, 1, 2, 8}, "bad");
        builder.create_ret();
        require_verify_fails(module, "OIRV_SHUFFLE_BOUNDS");
    }

    {
        oir::Module module("bad_vector_cast");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *source_type = module.types().fixed_vector_ty(module.types().float_ty(), 4);
        auto *result_type = module.types().fixed_vector_ty(module.types().int1_ty(), 4);
        builder.create_vector_cast(oir::VectorCastKind::FPToSI, result_type,
                                   module.create_undef(source_type), "bad");
        builder.create_ret();
        require_verify_fails(module, "OIRV_VECTOR_CAST_TYPE");
    }
}

void test_vp_arithmetic_compare_and_reductions() {
    oir::Module module("vp_arithmetic");
    auto &types = module.types();
    auto *v4i32 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *v4f32 = types.fixed_vector_ty(types.float_ty(), 4);
    auto *m4 = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *nxv4i32 = types.scalable_vector_ty(types.int32_ty(), 4);
    auto *nxm4 = types.scalable_vector_ty(types.int1_ty(), 4);

    auto *function = create_function(module, "f", types.void_ty());
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *lhs = module.create_undef(v4i32);
    auto *rhs = module.create_undef(v4i32);
    auto *flhs = module.create_undef(v4f32);
    auto *frhs = module.create_undef(v4f32);
    auto *active = module.create_undef(m4);
    auto *evl = builder.i32(4);

    auto *sum = builder.create_vp_binary(
        oir::Instruction::OpID::Add, lhs, rhs, active, evl, module.create_undef(v4i32),
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, "sum");
    auto *fsum = builder.create_vp_binary(
        oir::Instruction::OpID::FAdd, flhs, frhs, active, evl, module.create_undef(v4f32),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Undisturbed, "fsum");
    auto *icmp = builder.create_vp_icmp(
        oir::CmpPred::LT, sum, rhs, active, evl, module.create_undef(m4),
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, "icmp");
    auto *fcmp = builder.create_vp_fcmp(
        oir::CmpPred::GE, fsum, frhs, active, evl, module.create_undef(m4),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "fcmp");
    REQUIRE(icmp->type() == m4);
    REQUIRE(fcmp->type() == m4);

    auto *int_reduction = builder.create_vp_reduction(
        oir::ReductionKind::Add, false, sum, active, evl, builder.i32(0),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce.i");
    auto *or_reduction = builder.create_vp_reduction(
        oir::ReductionKind::Or, false, sum, active, evl, builder.i32(0),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce.or");
    auto *float_reduction = builder.create_vp_reduction(
        oir::ReductionKind::Add, true, fsum, active, evl, builder.f32(0.0F),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce.f");
    REQUIRE(int_reduction->type() == types.int32_ty());
    REQUIRE(or_reduction->type() == types.int32_ty());
    REQUIRE(float_reduction->type() == types.float_ty());

    auto *nxlhs = module.create_undef(nxv4i32);
    auto *nxrhs = module.create_undef(nxv4i32);
    auto *nxactive = module.create_undef(nxm4);
    auto *nxsum = builder.create_vp_binary(
        oir::Instruction::OpID::Or, nxlhs, nxrhs, nxactive, builder.i32(9),
        module.create_undef(nxv4i32), oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Agnostic, "nx.sum");
    REQUIRE(nxsum->type() == nxv4i32);
    builder.create_ret();

    require_verify(module);
    const auto printed = module.print();
    REQUIRE(printed.find("vp.add") != std::string::npos);
    REQUIRE(printed.find("tail=undisturbed") != std::string::npos);
    REQUIRE(printed.find("vp.reduce.ordered.fadd") != std::string::npos);
}

void test_vp_memory_analysis_and_memory_ssa() {
    oir::Module module("vp_memory");
    auto &types = module.types();
    auto *v4i32 = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *m4 = types.fixed_vector_ty(types.int1_ty(), 4);
    auto *base = module.create_global("base", types.int32_ty(), false);
    auto *function = create_function(module, "f", types.void_ty());
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *value = module.create_undef(v4i32);
    auto *passthrough = module.create_undef(v4i32);
    auto *active = module.create_undef(m4);
    auto *evl = builder.i32(4);
    auto *indices = builder.create_step_vector(v4i32, "indices");

    auto *vp_store = builder.create_vp_store(
        value, base, active, evl, oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, 4);
    auto *vp_load = builder.create_vp_load(
        v4i32, base, active, evl, passthrough, oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Undisturbed, 4, "load");
    auto *masked_store = builder.create_masked_store(
        value, base, active, evl, oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, 4);
    auto *masked_load = builder.create_masked_load(
        v4i32, base, active, evl, passthrough, oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Undisturbed, 4, "masked.load");
    auto *gather = builder.create_vp_gather(
        v4i32, base, indices, active, evl, passthrough, oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Undisturbed, 4, "gather");
    auto *scatter = builder.create_vp_scatter(
        value, base, indices, active, evl, oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
    require_verify(module);

    oir::OIRAliasAnalysis aa;
    REQUIRE(aa.may_write_memory(*vp_store));
    REQUIRE(!aa.may_read_memory(*vp_store));
    REQUIRE(aa.may_read_memory(*vp_load));
    REQUIRE(!aa.may_write_memory(*vp_load));
    REQUIRE(aa.may_write_memory(*masked_store));
    REQUIRE(aa.may_read_memory(*masked_load));
    REQUIRE(aa.may_read_memory(*gather));
    REQUIRE(aa.may_write_memory(*scatter));

    oir::FunctionModRefAnalysis modref(module);
    const auto &summary = modref.summary(function);
    REQUIRE(summary.read_globals.find(base) != summary.read_globals.end());
    REQUIRE(summary.written_globals.find(base) != summary.written_globals.end());

    oir::MemorySSA memory_ssa(*function, aa, modref);
    REQUIRE(memory_ssa.access_for(vp_store)->is_def());
    REQUIRE(memory_ssa.access_for(vp_load)->is_use());
    REQUIRE(memory_ssa.access_for(masked_store)->is_def());
    REQUIRE(memory_ssa.access_for(masked_load)->is_use());
    REQUIRE(memory_ssa.access_for(gather)->is_use());
    REQUIRE(memory_ssa.access_for(scatter)->is_def());
    REQUIRE(memory_ssa.clobbering_access(*vp_load) == memory_ssa.access_for(vp_store));
    REQUIRE(memory_ssa.clobbering_access(*gather) == memory_ssa.access_for(masked_store));
}

void test_vp_verifier_rejects_metadata_memory_and_reduction_errors() {
    auto make_binary_module = [](const std::string &name, auto build_bad) {
        auto module = std::make_unique<oir::Module>(name);
        auto &types = module->types();
        auto *function = create_function(*module, "f", types.void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *v4 = types.fixed_vector_ty(types.int32_ty(), 4);
        auto *m4 = types.fixed_vector_ty(types.int1_ty(), 4);
        build_bad(*module, builder, v4, m4);
        builder.create_ret();
        return module;
    };

    {
        auto module = make_binary_module("null_vp_mask", [](auto &module, auto &builder,
                                                             auto *v4, auto *) {
            builder.create_vp_binary(
                oir::Instruction::OpID::Or, module.create_undef(v4), module.create_undef(v4),
                nullptr, builder.i32(4), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_OPERAND_NULL");
    }

    {
        auto module = make_binary_module("bad_vp_mask", [](auto &module, auto &builder,
                                                            auto *v4, auto *) {
            auto *m8 = module.types().fixed_vector_ty(module.types().int1_ty(), 8);
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v4),
                module.create_undef(m8), builder.i32(4), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_MASK_SHAPE");
    }

    {
        auto module = make_binary_module("bad_vp_evl", [](auto &module, auto &builder,
                                                           auto *v4, auto *m4) {
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v4),
                module.create_undef(m4), builder.i1(true), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_EVL_TYPE");
    }

    {
        auto module = make_binary_module("negative_vp_evl", [](auto &module, auto &builder,
                                                                auto *v4, auto *m4) {
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v4),
                module.create_undef(m4), builder.i32(-1), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_EVL_RANGE");
    }

    {
        auto module = make_binary_module("oversized_fixed_vp_evl", [](auto &module, auto &builder,
                                                                       auto *v4, auto *m4) {
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v4),
                module.create_undef(m4), builder.i32(5), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_EVL_RANGE");
    }

    {
        auto module = make_binary_module("bad_vp_passthrough", [](auto &module, auto &builder,
                                                                   auto *v4, auto *m4) {
            auto *v4f = module.types().fixed_vector_ty(module.types().float_ty(), 4);
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v4),
                module.create_undef(m4), builder.i32(4), module.create_undef(v4f),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_PASSTHROUGH");
    }

    {
        auto module = make_binary_module("bad_vp_shape", [](auto &module, auto &builder,
                                                             auto *v4, auto *m4) {
            auto *v8 = module.types().fixed_vector_ty(module.types().int32_ty(), 8);
            builder.create_vp_binary(
                oir::Instruction::OpID::Add, module.create_undef(v4), module.create_undef(v8),
                module.create_undef(m4), builder.i32(4), module.create_undef(v4),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_BINARY_SHAPE");
    }

    {
        auto module = make_binary_module("bad_vp_pointer", [](auto &module, auto &builder,
                                                               auto *v4, auto *m4) {
            auto *wrong = module.create_global("p", module.types().float_ty(), false);
            builder.create_vp_load(v4, wrong, module.create_undef(m4), builder.i32(4),
                                   module.create_undef(v4), oir::TailPolicy::Agnostic,
                                   oir::MaskPolicy::Agnostic, 4, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_MEMORY_POINTER");
    }

    {
        auto module = make_binary_module("bad_vp_alignment", [](auto &module, auto &builder,
                                                                 auto *v4, auto *m4) {
            auto *base = module.create_global("p", module.types().int32_ty(), false);
            builder.create_vp_load(v4, base, module.create_undef(m4), builder.i32(4),
                                   module.create_undef(v4), oir::TailPolicy::Agnostic,
                                   oir::MaskPolicy::Agnostic, 3, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_MEMORY_ALIGNMENT");
    }

    {
        auto module = make_binary_module("bad_vp_policy", [](auto &module, auto &builder,
                                                              auto *v4, auto *m4) {
            auto *base = module.create_global("p", module.types().int32_ty(), false);
            builder.create_vp_store(module.create_undef(v4), base, module.create_undef(m4),
                                    builder.i32(4), oir::TailPolicy::Undisturbed,
                                    oir::MaskPolicy::Agnostic, 4);
        });
        require_verify_fails(*module, "OIRV_VP_POLICY");
    }

    {
        auto module = make_binary_module("bad_masked_evl", [](auto &module, auto &builder,
                                                               auto *v4, auto *m4) {
            auto *base = module.create_global("p", module.types().int32_ty(), false);
            builder.create_masked_load(v4, base, module.create_undef(m4), builder.i32(3),
                                       module.create_undef(v4), oir::TailPolicy::Agnostic,
                                       oir::MaskPolicy::Agnostic, 4, "bad");
        });
        require_verify_fails(*module, "OIRV_MASKED_VECTOR_FORM");
    }

    {
        auto module = make_binary_module("bad_float_or", [](auto &module, auto &builder,
                                                             auto *, auto *m4) {
            auto *v4f = module.types().fixed_vector_ty(module.types().float_ty(), 4);
            builder.create_vp_binary(
                oir::Instruction::OpID::Or, module.create_undef(v4f), module.create_undef(v4f),
                module.create_undef(m4), builder.i32(4), module.create_undef(v4f),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_BINARY_FAMILY");
    }

    {
        auto module = make_binary_module("bad_float_reduction", [](auto &module, auto &builder,
                                                                    auto *, auto *m4) {
            auto *v4f = module.types().fixed_vector_ty(module.types().float_ty(), 4);
            builder.create_vp_reduction(
                oir::ReductionKind::Add, false, module.create_undef(v4f),
                module.create_undef(m4), builder.i32(4), builder.f32(0.0F),
                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "bad");
        });
        require_verify_fails(*module, "OIRV_VP_REDUCTION_TYPE");
    }
}

void test_unknown_memory_opcode_fails_closed() {
    class UnknownMemoryInst final : public oir::Instruction {
      public:
        UnknownMemoryInst(oir::Type *type, oir::BasicBlock *parent)
            : Instruction(type, static_cast<OpID>(999), parent, "unknown") {}
        std::string print() const override { return "unknown.memory"; }
    };

    oir::Module module("unknown_memory");
    auto *function = create_function(module, "f", module.types().void_ty());
    auto *entry = function->create_block("entry");
    auto *unknown = static_cast<UnknownMemoryInst *>(entry->append_instruction(
        std::make_unique<UnknownMemoryInst>(module.types().void_ty(), entry)));
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();

    oir::OIRAliasAnalysis aa;
    REQUIRE(aa.may_read_memory(*unknown));
    REQUIRE(aa.may_write_memory(*unknown));
    REQUIRE(aa.has_side_effect(*unknown));
    oir::FunctionModRefAnalysis modref(module);
    REQUIRE(modref.summary(function).reads_unknown);
    REQUIRE(modref.summary(function).writes_unknown);
    oir::MemorySSA memory_ssa(*function, aa, modref);
    REQUIRE(memory_ssa.access_for(unknown) != nullptr);
    REQUIRE(memory_ssa.access_for(unknown)->is_def());
    require_verify_fails(module, "OIRV_UNKNOWN_OPCODE");
}

void test_runtime_alias_helper_is_readnone_in_modref() {
    oir::Module module("alias_helper_modref");
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *pointer = types.ptr_ty(i32);
    std::vector<oir::Type *> params{pointer, i32, i32, i32, pointer, i32, i32, i32};
    auto *helper = module.create_function(
        "__yoolang_ranges_disjoint", types.func_ty(i32, params), true);
    auto *caller = create_function(module, "caller", i32, {pointer, pointer, i32});
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *call = builder.create_call(
        helper, i32,
        {caller->args()[0].get(), caller->args()[2].get(), builder.i32(1), builder.i32(4),
         caller->args()[1].get(), caller->args()[2].get(), builder.i32(1), builder.i32(4)},
        "disjoint");
    builder.create_ret(call);
    require_verify(module);

    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    const auto &summary = modref.summary(helper);
    REQUIRE(!summary.may_read_memory());
    REQUIRE(!summary.may_write_memory());
    REQUIRE(!summary.has_side_effect);
    REQUIRE(!modref.call_may_read_memory(*call));
    REQUIRE(!modref.call_may_write_memory(*call));
    REQUIRE(!modref.call_has_side_effect(*call));
    REQUIRE(!modref.call_may_read(*call, caller->args()[0].get(), aa));
    REQUIRE(!modref.call_may_clobber(*call, caller->args()[1].get(), aa));
}

void test_vector_compare_builder_produces_same_shape_mask() {
    oir::Module module("vector_cmp_builder");
    auto &types = module.types();
    auto *v3i32 = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *v3f32 = types.fixed_vector_ty(types.float_ty(), 3);
    auto *m3 = types.fixed_vector_ty(types.int1_ty(), 3);

    auto *integer_function = create_function(module, "icmp_vec", m3, {v3i32, v3i32});
    auto *integer_entry = integer_function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(integer_entry);
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add,
                                      integer_function->args()[0].get(),
                                      integer_function->args()[1].get(), "sum");
    auto *icmp = builder.create_icmp(oir::CmpPred::LT, sum,
                                     integer_function->args()[1].get(), "cmp");
    REQUIRE(icmp->type() == m3);
    builder.create_ret(icmp);

    auto *float_function = create_function(module, "fcmp_vec", m3, {v3f32, v3f32});
    auto *float_entry = float_function->create_block("entry");
    builder.set_insert_point(float_entry);
    auto *fcmp = builder.create_fcmp(oir::CmpPred::GE, float_function->args()[0].get(),
                                     float_function->args()[1].get(), "cmp");
    REQUIRE(fcmp->type() == m3);
    builder.create_ret(fcmp);

    // Scalable vectors are legal SSA values even though the ordinary function
    // ABI cannot pass or return them yet.
    auto *scalable_function = create_function(module, "icmp_scalable_ssa", types.void_ty());
    auto *scalable_entry = scalable_function->create_block("entry");
    builder.set_insert_point(scalable_entry);
    auto *nxv3i32 = types.scalable_vector_ty(types.int32_ty(), 3);
    auto *lhs = module.create_undef(nxv3i32);
    auto *rhs = module.create_undef(nxv3i32);
    auto *scalable_cmp = builder.create_icmp(oir::CmpPred::EQ, lhs, rhs, "cmp");
    auto *scalable_mask = dynamic_cast<oir::VectorType *>(scalable_cmp->type());
    REQUIRE(scalable_mask != nullptr);
    REQUIRE(scalable_mask->is_mask());
    REQUIRE(scalable_mask->element_count() == nxv3i32->element_count());
    builder.create_ret();

    require_verify(module);
}

void test_verifier_rejects_vector_shape_and_family_errors() {
    {
        oir::Module module("bad_vector_cmp_result");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        auto *v3i32 = module.types().fixed_vector_ty(module.types().int32_ty(), 3);
        auto *lhs = module.create_undef(v3i32);
        auto *rhs = module.create_undef(v3i32);
        entry->append_instruction(std::make_unique<oir::CmpInst>(
            module.types().int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::EQ, lhs, rhs,
            entry, "bad_cmp"));
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret();
        require_verify_fails(module, "OIRV_CMP_RESULT_SHAPE");
    }

    {
        oir::Module module("bad_vector_binary_shape");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *lhs = module.create_undef(
            module.types().fixed_vector_ty(module.types().int32_ty(), 3));
        auto *rhs = module.create_undef(
            module.types().fixed_vector_ty(module.types().int32_ty(), 4));
        builder.create_binary(oir::Instruction::OpID::Add, lhs, rhs, "bad_add");
        builder.create_ret();
        require_verify_fails(module, "OIRV_BINARY_SHAPE");
    }

    {
        oir::Module module("bad_mask_arithmetic");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *mask = module.types().fixed_vector_ty(module.types().int1_ty(), 5);
        auto *lhs = module.create_undef(mask);
        auto *rhs = module.create_undef(mask);
        builder.create_binary(oir::Instruction::OpID::Add, lhs, rhs, "bad_mask_add");
        builder.create_ret();
        require_verify_fails(module, "OIRV_BINARY_TYPE_FAMILY");
    }
}

void test_verifier_rejects_scalar_op_and_cast_type_errors() {
    {
        oir::Module module("bad_scalar_family");
        auto *function = create_function(module, "f", module.types().int32_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *bad = builder.create_binary(oir::Instruction::OpID::FAdd, builder.i32(1),
                                          builder.i32(2), "bad_fadd");
        builder.create_ret(bad);
        require_verify_fails(module, "OIRV_BINARY_TYPE_FAMILY");
    }

    {
        oir::Module module("bad_zext");
        auto *function = create_function(module, "f", module.types().int1_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *bad = builder.create_zext(builder.i32(1), module.types().int1_ty(), "bad_zext");
        builder.create_ret(bad);
        require_verify_fails(module, "OIRV_ZEXT_TYPE");
    }

    {
        oir::Module module("bad_sitofp");
        auto *function = create_function(module, "f", module.types().float_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *bad = builder.create_sitofp(builder.f32(1.0F), module.types().float_ty(),
                                          "bad_sitofp");
        builder.create_ret(bad);
        require_verify_fails(module, "OIRV_SITOFP_TYPE");
    }

    {
        oir::Module module("bad_fptosi");
        auto *function = create_function(module, "f", module.types().int32_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *bad = builder.create_fptosi(builder.i32(1), module.types().int32_ty(),
                                          "bad_fptosi");
        builder.create_ret(bad);
        require_verify_fails(module, "OIRV_FPTOSI_TYPE");
    }
}

void test_verifier_rejects_alloca_and_scalable_storage() {
    {
        oir::Module module("bad_alloca_type");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        entry->append_instruction(std::make_unique<oir::AllocaInst>(
            module.types().ptr_ty(module.types().float_ty()), module.types().int32_ty(), entry,
            "bad_alloca"));
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret();
        require_verify_fails(module, "OIRV_ALLOCA_TYPE");
    }

    {
        oir::Module module("bad_scalable_alloca");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_alloca(
            module.types().scalable_vector_ty(module.types().int32_ty(), 4), "bad_alloca");
        builder.create_ret();
        require_verify_fails(module, "OIRV_SCALABLE_ALLOCA");
    }

    {
        oir::Module module("bad_scalable_global");
        module.create_global(
            "g", module.types().scalable_vector_ty(module.types().float_ty(), 4), false);
        require_verify_fails(module, "OIRV_SCALABLE_GLOBAL");
    }

    {
        oir::Module module("bad_scalable_abi");
        auto *vector = module.types().scalable_vector_ty(module.types().int32_ty(), 4);
        module.create_function("external_vec", module.types().func_ty(module.types().void_ty(),
                                                                       {vector}), true);
        require_verify_fails(module, "OIRV_SCALABLE_ABI");
    }
}

void test_verifier_checks_fixed_and_variadic_calls() {
    {
        oir::Module module("bad_call_arity");
        auto *callee = module.create_function(
            "callee", module.types().func_ty(module.types().int32_ty(),
                                               {module.types().int32_ty()}),
            true);
        auto *caller = create_function(module, "caller", module.types().int32_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *call = builder.create_call(callee, module.types().int32_ty(), {}, "call");
        builder.create_ret(call);
        require_verify_fails(module, "OIRV_CALL_ARITY");
    }

    {
        oir::Module module("bad_call_argument");
        auto *callee = module.create_function(
            "callee", module.types().func_ty(module.types().int32_ty(),
                                               {module.types().int32_ty()}),
            true);
        auto *caller = create_function(module, "caller", module.types().int32_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *call = builder.create_call(callee, module.types().int32_ty(), {builder.f32(1.0F)},
                                         "call");
        builder.create_ret(call);
        require_verify_fails(module, "OIRV_CALL_ARGUMENT_TYPE");
    }

    {
        oir::Module module("bad_call_return");
        auto *callee = module.create_function(
            "callee", module.types().func_ty(module.types().int32_ty(),
                                               {module.types().int32_ty()}),
            true);
        auto *caller = create_function(module, "caller", module.types().float_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *call = builder.create_call(callee, module.types().float_ty(), {builder.i32(1)},
                                         "call");
        builder.create_ret(call);
        require_verify_fails(module, "OIRV_CALL_RETURN_TYPE");
    }

    {
        oir::Module module("variadic_putf");
        auto *putf = module.create_function(
            "putf", module.types().func_ty(module.types().void_ty(), {}, true), true);
        auto *caller = create_function(module, "caller", module.types().void_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_call(putf, module.types().void_ty(), {builder.i32(1), builder.f32(2.0F)});
        builder.create_ret();
        require_verify(module);
    }

    {
        oir::Module module("ordinary_variadic");
        auto *callee = module.create_function(
            "log", module.types().func_ty(module.types().void_ty(),
                                            {module.types().int32_ty()}, true), true);
        auto *caller = create_function(module, "caller", module.types().void_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_call(callee, module.types().void_ty(),
                            {builder.i32(1), builder.f32(2.0F)});
        builder.create_ret();
        require_verify(module);
    }

    {
        oir::Module module("variadic_fixed_prefix_type");
        auto *callee = module.create_function(
            "log", module.types().func_ty(module.types().void_ty(),
                                            {module.types().int32_ty()}, true), true);
        auto *caller = create_function(module, "caller", module.types().void_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_call(callee, module.types().void_ty(), {builder.f32(1.0F)});
        builder.create_ret();
        require_verify_fails(module, "OIRV_CALL_ARGUMENT_TYPE");
    }

    {
        oir::Module module("variadic_scalable_argument");
        auto *callee = module.create_function(
            "log", module.types().func_ty(module.types().void_ty(), {}, true), true);
        auto *caller = create_function(module, "caller", module.types().void_ty());
        auto *entry = caller->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *scalable =
            module.types().scalable_vector_ty(module.types().int32_ty(), 4);
        builder.create_call(callee, module.types().void_ty(),
                            {module.create_undef(scalable)});
        builder.create_ret();
        require_verify_fails(module, "OIRV_SCALABLE_ABI");
    }
}

void test_verifier_rejects_gep_branch_phi_return_and_function_shape_errors() {
    {
        oir::Module module("bad_gep_result");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *array = builder.create_alloca(module.types().array_ty(module.types().int32_ty(), 4),
                                            "array");
        builder.create_gep(array, module.types().ptr_ty(module.types().float_ty()),
                           {builder.i32(0), builder.i32(0)}, "bad_gep");
        builder.create_ret();
        require_verify_fails(module, "OIRV_GEP_RESULT_TYPE");
    }

    {
        oir::Module module("bad_gep_path");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *scalar = builder.create_alloca(module.types().int32_ty(), "scalar");
        builder.create_gep(scalar, module.types().ptr_ty(module.types().int32_ty()),
                           {builder.i32(0), builder.i32(0)}, "bad_gep");
        builder.create_ret();
        require_verify_fails(module, "OIRV_GEP_PATH");
    }

    {
        oir::Module module("bad_branch_mask");
        auto *function = create_function(module, "f", module.types().void_ty());
        auto *entry = function->create_block("entry");
        auto *left = function->create_block("left");
        auto *right = function->create_block("right");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *mask = module.create_undef(
            module.types().fixed_vector_ty(module.types().int1_ty(), 4));
        builder.create_cond_br(mask, left, right);
        builder.set_insert_point(left);
        builder.create_ret();
        builder.set_insert_point(right);
        builder.create_ret();
        require_verify_fails(module, "OIRV_BRANCH_CONDITION");
    }

    {
        oir::Module module("bad_phi_type");
        auto *function = create_function(module, "f", module.types().int32_ty(),
                                         {module.types().int1_ty()});
        auto *entry = function->create_block("entry");
        auto *left = function->create_block("left");
        auto *right = function->create_block("right");
        auto *merge = function->create_block("merge");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_cond_br(function->args()[0].get(), left, right);
        builder.set_insert_point(left);
        builder.create_br(merge);
        builder.set_insert_point(right);
        builder.create_br(merge);
        builder.set_insert_point(merge);
        auto *phi = builder.create_phi(module.types().int32_ty(), "bad_phi");
        phi->add_incoming(builder.i32(1), left);
        phi->add_incoming(builder.f32(2.0F), right);
        builder.create_ret(phi);
        require_verify_fails(module, "OIRV_PHI_TYPE");
    }

    {
        oir::Module module("bad_return_type");
        auto *function = create_function(module, "f", module.types().int32_ty());
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        builder.create_ret(builder.f32(1.0F));
        require_verify_fails(module, "OIRV_RETURN_TYPE");
    }

    {
        oir::Module module("bad_function_argument_count");
        auto *function = create_function(module, "f", module.types().int32_ty(),
                                         {module.types().int32_ty()});
        function->add_argument(module.types().int32_ty(), "extra");
        require_verify_fails(module, "OIRV_FUNCTION_ARGUMENT_COUNT");
    }
}

void test_replace_all_uses_with_updates_use_lists() {
    oir::Module module("replace");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int32_ty()});
    auto *arg = function->args()[0].get();
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
    auto *arg = function->args()[0].get();
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
    auto *cond = function->args()[0].get();
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
    auto *cond = function->args()[0].get();
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

void test_dynamic_gep_aliases_fail_closed() {
    oir::Module module("dynamic_gep_alias");
    auto *i32 = module.types().int32_ty();
    auto *row_type = module.types().array_ty(i32, 4);
    auto *matrix_type = module.types().array_ty(row_type, 2);
    auto *function = create_function(module, "f", i32, {i32, i32});
    auto *row = function->args()[0].get();
    auto *col = function->args()[1].get();
    row->set_name("row");
    col->set_name("col");
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *matrix = builder.create_alloca(matrix_type, "matrix");
    auto *zero = builder.i32(0);
    auto *one = builder.i32(1);

    // Both paths flatten to [0, row, col] in the old PointerPath model, but
    // the second GEP's first index has a whole-row stride.  Dynamic byte
    // offsets are unknown, so the shared flattened sequence cannot prove
    // MustAlias.
    auto *direct = builder.create_gep(matrix, module.types().ptr_ty(i32), {zero, row, col},
                                      "direct");
    auto *row_base = builder.create_gep(matrix, module.types().ptr_ty(row_type), {zero, row},
                                        "row_base");
    auto *segmented = builder.create_gep(row_base, module.types().ptr_ty(row_type), {col},
                                         "segmented");

    // This is the executable regression's address shape.  The read may
    // overlap the store when row == 1, despite the first differing flattened
    // constant index.
    auto *zero_row = builder.create_gep(matrix, module.types().ptr_ty(row_type), {zero, zero},
                                        "zero_row");
    auto *dynamic_read = builder.create_gep(
        zero_row, module.types().ptr_ty(i32), {row, col}, "dynamic_read");
    auto *dynamic_store = builder.create_gep(
        matrix, module.types().ptr_ty(i32), {zero, one, col}, "dynamic_store");

    // Fully known byte intervals retain precise results.
    auto *constant_left = builder.create_gep(
        matrix, module.types().ptr_ty(i32), {zero, zero, zero}, "constant_left");
    auto *constant_left_again = builder.create_gep(
        matrix, module.types().ptr_ty(i32), {zero, zero, zero}, "constant_left_again");
    auto *constant_right = builder.create_gep(
        matrix, module.types().ptr_ty(i32), {zero, zero, one}, "constant_right");
    builder.create_ret(zero);

    require_verify(module);

    oir::OIRAliasAnalysis aa;
    REQUIRE(aa.alias(direct, direct) == oir::AliasResult::MustAlias);
    REQUIRE(aa.alias(direct, segmented) == oir::AliasResult::MayAlias);
    REQUIRE(aa.alias(dynamic_read, dynamic_store) == oir::AliasResult::MayAlias);
    REQUIRE(aa.alias(constant_left, constant_left_again) == oir::AliasResult::MustAlias);
    REQUIRE(aa.alias(constant_left, constant_right) == oir::AliasResult::NoAlias);
}

void test_function_modref_does_not_promote_cfg_effects() {
    oir::Module module("modref-cfg-effects");
    auto &types = module.types();
    auto *callee = create_function(module, "pure_branch", types.int32_ty(),
                                   {types.int32_ty()});
    auto *entry = callee->create_block("entry");
    auto *nonzero = callee->create_block("nonzero");
    auto *zero = callee->create_block("zero");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *condition =
        builder.create_icmp(oir::CmpPred::NE, callee->args()[0].get(), builder.i32(0));
    builder.create_cond_br(condition, nonzero, zero);
    builder.set_insert_point(nonzero);
    builder.create_ret(callee->args()[0].get());
    builder.set_insert_point(zero);
    builder.create_ret(builder.i32(0));

    auto *caller = create_function(module, "caller", types.int32_ty());
    auto *caller_entry = caller->create_block("entry");
    builder.set_insert_point(caller_entry);
    auto *call = builder.create_call(callee, types.int32_ty(), {builder.i32(7)}, "call");
    builder.create_ret(call);
    require_verify(module);

    oir::FunctionModRefAnalysis modref(module);
    const auto &summary = modref.summary(callee);
    REQUIRE(!summary.has_side_effect);
    REQUIRE(!summary.may_read_memory());
    REQUIRE(!summary.may_write_memory());
    REQUIRE(!modref.call_has_side_effect(*call));
    REQUIRE(!modref.call_may_read_memory(*call));
    REQUIRE(!modref.call_may_write_memory(*call));
}

void test_presburger_uses_explicit_bounds_outside_fallback_window() {
    yir::presburger::IntegerRelation relation(1);
    relation.add_inequality({1}, -100);
    relation.add_inequality({-1}, 128);

    auto sample = relation.find_integer_sample();
    REQUIRE(sample.has_value());
    REQUIRE(sample->size() == 1);
    REQUIRE((*sample)[0] >= 100);
    REQUIRE((*sample)[0] <= 128);

    auto lexmax = relation.find_lexicographic_maximum();
    REQUIRE(lexmax.has_value());
    REQUIRE((*lexmax)[0] == 128);

    yir::presburger::IntegerRelation empty(1);
    empty.add_inequality({1}, -100);
    empty.add_inequality({-1}, 99);
    REQUIRE(empty.is_integer_empty());
}

void test_presburger_prunes_infeasible_multivariable_box() {
    constexpr unsigned kVariables = 8;
    yir::presburger::IntegerRelation relation(kVariables);
    for (unsigned variable = 0; variable < kVariables; ++variable) {
        std::vector<std::int64_t> lower(kVariables, 0);
        std::vector<std::int64_t> upper(kVariables, 0);
        lower[variable] = 1;
        upper[variable] = -1;
        relation.add_inequality(std::move(lower), 0);
        relation.add_inequality(std::move(upper), 32);
    }
    std::vector<std::int64_t> sum(kVariables, 1);
    relation.add_equality(std::move(sum), -1000);
    REQUIRE(relation.is_integer_empty());
}

void test_presburger_local_floor_div_constraints() {
    yir::presburger::IntegerRelation relation(1);
    relation.add_vars(yir::presburger::VarKind::Local, 1);
    relation.add_equality({1, 0}, -9);
    relation.add_inequality({1, -4}, 0);
    relation.add_inequality({-1, 4}, 3);
    auto sample = relation.find_integer_sample();
    REQUIRE(sample.has_value());
    REQUIRE(sample->size() == 2);
    REQUIRE((*sample)[0] == 9);
    REQUIRE((*sample)[1] == 2);
}

void test_presburger_budgeted_feasibility_is_tristate() {
    yir::presburger::IntegerRelation empty(2);
    empty.add_inequality({1, 0}, 0);
    empty.add_inequality({-1, 0}, 4);
    empty.add_inequality({0, 1}, 0);
    empty.add_inequality({0, -1}, 4);
    empty.add_equality({1, 1}, -20);
    REQUIRE(empty.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::Empty);

    yir::presburger::IntegerRelation sample(1);
    sample.add_inequality({1}, 0);
    sample.add_inequality({-1}, 4);
    REQUIRE(sample.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::NonEmpty);
    REQUIRE(sample.check_integer_feasibility(1) ==
            yir::presburger::IntegerFeasibility::Unknown);

    yir::presburger::IntegerRelation unbounded(1);
    unbounded.add_inequality({1}, 0);
    REQUIRE(unbounded.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::Unknown);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"vector_type_interning_printing_and_predicates",
         test_vector_type_interning_printing_and_predicates},
        {"vector_type_rejects_illegal_element_counts",
         test_vector_type_rejects_illegal_element_counts},
        {"data_layout_fixed_scalable_and_mask_storage",
         test_data_layout_fixed_scalable_and_mask_storage},
        {"typed_constant_initializers_print_and_verify",
         test_typed_constant_initializers_print_and_verify},
        {"typed_constant_constructors_reject_invalid_shapes",
         test_typed_constant_constructors_reject_invalid_shapes},
        {"typed_constant_verifier_rejects_bad_initializers",
         test_typed_constant_verifier_rejects_bad_initializers},
        {"vector_primitives_fixed_and_scalable",
         test_vector_primitives_fixed_and_scalable},
        {"vector_primitives_reject_bad_shapes_and_bounds",
         test_vector_primitives_reject_bad_shapes_and_bounds},
        {"vp_arithmetic_compare_and_reductions",
         test_vp_arithmetic_compare_and_reductions},
        {"vp_memory_analysis_and_memory_ssa",
         test_vp_memory_analysis_and_memory_ssa},
        {"vp_verifier_rejects_metadata_memory_and_reduction_errors",
         test_vp_verifier_rejects_metadata_memory_and_reduction_errors},
        {"unknown_memory_opcode_fails_closed",
         test_unknown_memory_opcode_fails_closed},
        {"runtime_alias_helper_is_readnone_in_modref",
         test_runtime_alias_helper_is_readnone_in_modref},
        {"vector_compare_builder_produces_same_shape_mask",
         test_vector_compare_builder_produces_same_shape_mask},
        {"verifier_rejects_vector_shape_and_family_errors",
         test_verifier_rejects_vector_shape_and_family_errors},
        {"verifier_rejects_scalar_op_and_cast_type_errors",
         test_verifier_rejects_scalar_op_and_cast_type_errors},
        {"verifier_rejects_alloca_and_scalable_storage",
         test_verifier_rejects_alloca_and_scalable_storage},
        {"verifier_checks_fixed_and_variadic_calls",
         test_verifier_checks_fixed_and_variadic_calls},
        {"verifier_rejects_gep_branch_phi_return_and_function_shape_errors",
         test_verifier_rejects_gep_branch_phi_return_and_function_shape_errors},
        {"replace_all_uses_with_updates_use_lists", test_replace_all_uses_with_updates_use_lists},
        {"erasing_instruction_drops_operand_uses", test_erasing_instruction_drops_operand_uses},
        {"phi_incoming_removal_reindexes_use_lists",
         test_phi_incoming_removal_reindexes_use_lists},
        {"erasing_block_drops_instruction_operand_uses",
         test_erasing_block_drops_instruction_operand_uses},
        {"verifier_catches_stale_constant_use_list",
         test_verifier_catches_stale_constant_use_list},
        {"memory_ssa_skips_noalias_defs", test_memory_ssa_skips_noalias_defs},
        {"memory_ssa_uses_phi_at_cfg_join", test_memory_ssa_uses_phi_at_cfg_join},
        {"dynamic_gep_aliases_fail_closed", test_dynamic_gep_aliases_fail_closed},
        {"function_modref_does_not_promote_cfg_effects",
         test_function_modref_does_not_promote_cfg_effects},
        {"presburger_uses_explicit_bounds_outside_fallback_window",
         test_presburger_uses_explicit_bounds_outside_fallback_window},
        {"presburger_prunes_infeasible_multivariable_box",
         test_presburger_prunes_infeasible_multivariable_box},
        {"presburger_local_floor_div_constraints",
         test_presburger_local_floor_div_constraints},
        {"presburger_budgeted_feasibility_is_tristate",
         test_presburger_budgeted_feasibility_is_tristate},
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
            str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRCFGUtils.cpp"),
            str(ROOT / "src/oir/OIRDataLayout.cpp"),
            str(ROOT / "src/yir/Presburger.cpp"),
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
