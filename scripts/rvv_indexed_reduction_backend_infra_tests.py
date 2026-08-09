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
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) throw std::runtime_error(#condition);                   \
    } while (false)

std::vector<oir::Constant *> i32_values(oir::Module &module,
                                        std::initializer_list<std::int32_t> values) {
    std::vector<oir::Constant *> result;
    for (auto value : values) result.push_back(module.create_i32(value));
    return result;
}

std::vector<oir::Constant *> f32_values(oir::Module &module,
                                        std::initializer_list<float> values) {
    std::vector<oir::Constant *> result;
    for (auto value : values) result.push_back(module.create_f32(value));
    return result;
}

void add_indexed_functions(oir::Module &module) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *pointer = types.ptr_ty(i32);
    auto *signature = types.func_ty(types.void_ty(), {pointer, pointer, pointer});

    auto build = [&](const char *name, std::initializer_list<std::int32_t> indexes,
                     std::uint8_t active_bits, std::int32_t evl) {
        auto *function = module.create_function(name, signature);
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module);
        builder.set_insert_point(entry);
        auto *index = module.create_constant_vector(vector, i32_values(module, indexes));
        auto *active = module.create_constant_mask(mask, {active_bits});
        auto *all = module.create_constant_mask(mask, {0x7f});
        auto *passthrough = module.create_constant_vector(
            vector, i32_values(module, {-99, -99, -99, -99, -99, -99, -99}));
        auto *values = module.create_constant_vector(
            vector, i32_values(module, {11, 22, 33, 44, 55, 66, 77}));
        auto *gather = builder.create_vp_gather(
            vector, function->args()[2].get(), index, active, builder.i32(evl),
            passthrough, oir::TailPolicy::Undisturbed,
            oir::MaskPolicy::Undisturbed, 4, "gather");
        builder.create_vp_store(gather, function->args()[0].get(), all,
                                builder.i32(7), oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_vp_scatter(values, function->args()[1].get(), index, active,
                                  builder.i32(evl), oir::TailPolicy::Agnostic,
                                  oir::MaskPolicy::Agnostic, 4);
        builder.create_ret();
    };

    // Index 1024 names the first element of a PROT_NONE page in the driver.
    build("native_indexed", {0, 1, 1024, 2, 3, 1024, 0}, 0x5b, 7);
    // Negative offsets force the signed scalar fallback.  Lane two is a
    // protected-page address but is inactive, and duplicate index zero must
    // leave the value from the highest active lane.
    build("signed_indexed", {-2, -1, 1024, 1, 2, 0, 0}, 0x7b, 7);
    // No address may even be computed on an executed path when EVL is zero.
    build("signed_indexed_evl0", {1024, -1, 0, 1, 2, 3, 4}, 0x7f, 0);
}

void add_int_reduction(oir::Module &module, const char *name,
                       oir::ReductionKind kind, std::uint8_t active_bits,
                       std::int32_t evl, std::int32_t seed,
                       bool lane_zero_clear = false) {
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *function = module.create_function(name, types.func_ty(types.int32_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto values = kind == oir::ReductionKind::Mul
                      ? i32_values(module, {2, -3, 5, 7, -11, 13, 17})
                      : i32_values(module, {
                            std::numeric_limits<std::int32_t>::min(), -3, 5, 7,
                            -11, 13, std::numeric_limits<std::int32_t>::max()});
    auto *source = module.create_constant_vector(vector, std::move(values));
    auto *active = module.create_constant_mask(
        mask, {static_cast<std::uint8_t>(lane_zero_clear ? active_bits & ~1U
                                                        : active_bits)});
    auto *result = builder.create_vp_reduction(
        kind, false, source, active, builder.i32(evl), builder.i32(seed),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce");
    builder.create_ret(result);
}

void add_external_seed_reduction(oir::Module &module) {
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *function = module.create_function(
        "int_add_external_seed", types.func_ty(types.int32_ty(), {types.int32_ty()}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_vector(
        vector, i32_values(module, {1, 2, 3, 4, 5, 6, 7}));
    auto *active = module.create_constant_mask(mask, {0x7f});
    auto *result = builder.create_vp_reduction(
        oir::ReductionKind::Add, false, source, active, builder.i32(7),
        function->args()[0].get(), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, "reduce");
    builder.create_ret(result);
}

void add_float_reduction(oir::Module &module, const char *name,
                         oir::ReductionKind kind,
                         std::initializer_list<float> values,
                         std::uint8_t active_bits, std::int32_t evl,
                         float seed) {
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.float_ty(), 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *function = module.create_function(name, types.func_ty(types.float_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_vector(vector, f32_values(module, values));
    auto *active = module.create_constant_mask(mask, {active_bits});
    auto *result = builder.create_vp_reduction(
        kind, true, source, active, builder.i32(evl), module.create_f32(seed),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce");
    builder.create_ret(result);
}

std::vector<std::uint8_t> packed_mask(std::uint64_t lanes,
                                      bool (*predicate)(std::uint64_t)) {
    std::vector<std::uint8_t> result((lanes + 7U) / 8U, 0);
    for (std::uint64_t lane = 0; lane < lanes; ++lane) {
        if (predicate(lane)) result[lane / 8U] |= 1U << (lane % 8U);
    }
    return result;
}

bool source_pattern(std::uint64_t lane) { return lane % 3U != 1U; }
bool active_pattern(std::uint64_t lane) { return lane % 4U != 2U; }

void add_mask_reduction(oir::Module &module, std::uint64_t lanes,
                        oir::ReductionKind kind, const char *name,
                        std::int32_t evl, bool seed) {
    auto &types = module.types();
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *function = module.create_function(name, types.func_ty(types.int32_ty(), {}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_mask(mask, packed_mask(lanes, source_pattern));
    auto *active = module.create_constant_mask(mask, packed_mask(lanes, active_pattern));
    auto *reduced = builder.create_vp_reduction(
        kind, false, source, active, builder.i32(evl), builder.i1(seed),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduce");
    builder.create_ret(builder.create_zext(reduced, types.int32_ty(), "result"));
}

void add_scalable_functions(oir::Module &module) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *iv = types.scalable_vector_ty(i32, 4);
    auto *im = types.scalable_vector_ty(types.int1_ty(), 4);
    auto *fv = types.scalable_vector_ty(f32, 4);
    auto *fm = types.scalable_vector_ty(types.int1_ty(), 4);
    auto *ip = types.ptr_ty(i32);
    auto *fp = types.ptr_ty(f32);

    {
        auto *function = module.create_function(
            "scalable_add", types.func_ty(i32, {ip, i32, i32, ip}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(iv, function->args()[1].get(), "vl");
        auto *active = builder.create_splat(im, builder.i1(true), "active");
        auto *loaded = builder.create_vp_load(
            iv, function->args()[0].get(), active, vl, module.create_undef(iv),
            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 4, "load");
        auto *sum = builder.create_vp_reduction(
            oir::ReductionKind::Add, false, loaded, active, vl,
            function->args()[2].get(), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, "sum");
        builder.create_store(vl, function->args()[3].get());
        builder.create_ret(sum);
    }
    {
        auto *function = module.create_function(
            "scalable_float_mul", types.func_ty(f32, {fp, i32, f32, ip}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(fv, function->args()[1].get(), "vl");
        auto *active = builder.create_splat(fm, builder.i1(true), "active");
        auto *loaded = builder.create_vp_load(
            fv, function->args()[0].get(), active, vl, module.create_undef(fv),
            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 4, "load");
        auto *product = builder.create_vp_reduction(
            oir::ReductionKind::Mul, true, loaded, active, vl,
            function->args()[2].get(), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, "product");
        builder.create_store(vl, function->args()[3].get());
        builder.create_ret(product);
    }
    auto build_gather = [&](const char *name, bool negative) {
        auto *function = module.create_function(name, types.func_ty(i32, {ip, ip, i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(iv, function->args()[2].get(), "vl");
        auto *active = builder.create_splat(im, builder.i1(true), "active");
        oir::Value *indices = builder.create_step_vector(iv, "lanes");
        if (negative) {
            auto *minus_one = builder.create_splat(iv, builder.i32(-1), "minus.one");
            indices = builder.create_vp_binary(
                oir::Instruction::OpID::Mul, indices, minus_one, active, vl,
                module.create_undef(iv), oir::TailPolicy::Agnostic,
                oir::MaskPolicy::Agnostic, "negative.indices");
        }
        auto *gather = builder.create_vp_gather(
            iv, function->args()[1].get(), indices, active, vl,
            module.create_undef(iv), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, 4, "gather");
        builder.create_vp_store(gather, function->args()[0].get(), active, vl,
                                oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_ret(vl);
    };
    build_gather("scalable_gather", false);
    build_gather("scalable_signed_gather", true);

    auto build_reverse = [&](const char *name, std::int32_t magnitude) {
        auto *function = module.create_function(
            name, types.func_ty(i32, {ip, ip, ip, i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(iv, function->args()[3].get(), "vl");
        auto *active = builder.create_splat(im, builder.i1(true), "active");
        auto *last = builder.create_binary(
            oir::Instruction::OpID::Sub, vl, builder.i32(1), "last");
        auto *last_vector = builder.create_splat(iv, last, "last.splat");
        auto *lanes = builder.create_step_vector(iv, "lanes");
        auto *reverse = builder.create_vp_binary(
            oir::Instruction::OpID::Sub, last_vector, lanes, active, vl,
            module.create_undef(iv), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, "reverse");
        auto *scale = builder.create_splat(iv, builder.i32(magnitude), "scale");
        auto *indices = builder.create_vp_binary(
            oir::Instruction::OpID::Mul, reverse, scale, active, vl,
            module.create_undef(iv), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, "indices");
        auto *gather = builder.create_vp_gather(
            iv, function->args()[2].get(), indices, active, vl,
            module.create_undef(iv), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, 4, "gather");
        builder.create_vp_store(gather, function->args()[0].get(), active, vl,
                                oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_vp_scatter(gather, function->args()[1].get(), indices,
                                  active, vl, oir::TailPolicy::Agnostic,
                                  oir::MaskPolicy::Agnostic, 4);
        builder.create_ret(vl);
    };
    build_reverse("scalable_reverse_1", 1);
    build_reverse("scalable_reverse_2", 2);
    build_reverse("scalable_reverse_4", 4);
}

std::unique_ptr<oir::Module> make_module() {
    auto module = std::make_unique<oir::Module>("rvv-indexed-reduction");
    add_indexed_functions(*module);
    constexpr std::uint8_t active = 0x2d;
    add_int_reduction(*module, "int_add", oir::ReductionKind::Add, active, 6, 17);
    add_int_reduction(*module, "int_min", oir::ReductionKind::Min, active, 6, 42);
    add_int_reduction(*module, "int_max", oir::ReductionKind::Max, active, 6, -42);
    add_int_reduction(*module, "int_and", oir::ReductionKind::And, active, 6, -1);
    add_int_reduction(*module, "int_or", oir::ReductionKind::Or, active, 6, 0);
    add_int_reduction(*module, "int_xor", oir::ReductionKind::Xor, active, 6, 0x12345678);
    add_int_reduction(*module, "int_mul", oir::ReductionKind::Mul, active, 6, 3);
    add_int_reduction(*module, "int_add_lane0_clear", oir::ReductionKind::Add,
                      active, 6, 100, true);
    add_int_reduction(*module, "int_add_evl0", oir::ReductionKind::Add, 0x7f, 0, 123);
    add_external_seed_reduction(*module);

    const float qnan = std::numeric_limits<float>::quiet_NaN();
    add_float_reduction(*module, "float_add", oir::ReductionKind::Add,
                        {1.5F, -2.0F, 3.25F, -0.0F, 0.0F, 4.0F, qnan},
                        0x2b, 6, 0.25F);
    add_float_reduction(*module, "float_mul", oir::ReductionKind::Mul,
                        {1.0e20F, 1.0e-20F, 3.25F, 1.0000001F,
                         0.0F, 1.0000002F, qnan},
                        0x2b, 6, 1.0000004F);
    add_float_reduction(*module, "float_min", oir::ReductionKind::Min,
                        {1.5F, -2.0F, 3.25F, -0.0F, 0.0F, 4.0F, qnan},
                        0x2b, 6, 0.0F);
    add_float_reduction(*module, "float_max", oir::ReductionKind::Max,
                        {1.5F, -2.0F, 3.25F, -0.0F, 0.0F, 4.0F, qnan},
                        0x2b, 6, -0.0F);
    add_float_reduction(*module, "float_min_acc_nan", oir::ReductionKind::Min,
                        {1.0F, -2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F},
                        0x7f, 7, qnan);
    add_float_reduction(*module, "float_min_lane_nan", oir::ReductionKind::Min,
                        {qnan, 2.0F, -3.0F, 4.0F, 5.0F, 6.0F, 7.0F},
                        0x07, 3, 1.0F);
    add_float_reduction(*module, "float_max_acc_nan", oir::ReductionKind::Max,
                        {1.0F, -2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F},
                        0x7f, 7, qnan);
    add_float_reduction(*module, "float_max_lane_nan", oir::ReductionKind::Max,
                        {qnan, -2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F},
                        0x07, 3, -1.0F);
    add_float_reduction(*module, "float_mul_acc_nan", oir::ReductionKind::Mul,
                        {2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F},
                        0x7f, 7, qnan);
    add_float_reduction(*module, "float_mul_lane_nan", oir::ReductionKind::Mul,
                        {2.0F, qnan, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F},
                        0x03, 2, 1.0F);
    add_float_reduction(*module, "float_mul_signed_zero", oir::ReductionKind::Mul,
                        {-0.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F},
                        0x01, 1, 1.0F);
    add_float_reduction(*module, "float_min_signed_zero", oir::ReductionKind::Min,
                        {-0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F},
                        0x01, 1, 0.0F);
    add_float_reduction(*module, "float_max_signed_zero", oir::ReductionKind::Max,
                        {0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F},
                        0x01, 1, -0.0F);

    for (auto lanes : {1U, 3U, 7U, 31U}) {
        const auto evl = lanes == 1 ? 0 : static_cast<int>(lanes - 1);
        const auto suffix = std::to_string(lanes);
        add_mask_reduction(*module, lanes, oir::ReductionKind::Or,
                           ("mask_or_n" + suffix).c_str(), evl, false);
        add_mask_reduction(*module, lanes, oir::ReductionKind::And,
                           ("mask_and_n" + suffix).c_str(), evl, true);
        add_mask_reduction(*module, lanes, oir::ReductionKind::Xor,
                           ("mask_xor_n" + suffix).c_str(), evl, true);
    }
    add_scalable_functions(*module);
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    std::string error;
    target::TargetProfile profile;
    profile.march = "rv64gcv";
    REQUIRE(target::finalize_target_profile(profile, error));
    auto machine = pass::oir_to_mir::lower_with_vregs(*make_module(), profile);
    machine->target().arch = profile.march;
    machine->target().has_vector = true;
    machine->target().minimum_vlen_bits = profile.minimum_vlen_bits;
    auto pre = mir::verify_module(*machine, mir::MIRVerificationStage::PreRA);
    if (!pre.ok) throw std::runtime_error(pre.message);
    pass::PassContext context;
    context.set_machine_module(std::move(machine));
    pass::MIRRegAllocPass regalloc;
    auto allocated = regalloc.run(context);
    if (!allocated.success) throw std::runtime_error(allocated.message);
    machine = context.take_machine_module();
    REQUIRE(pass::expand_machine_pseudos(*machine, error));
    auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
    if (!final.ok) throw std::runtime_error(final.message);
    for (const auto &function : machine->functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                REQUIRE(!mir::instruction_desc(instruction.opcode()).has_flag(mir::MIF_Pseudo));
            }
        }
    }
    std::ofstream output(argv[1]);
    REQUIRE(output.good());
    mir::AsmPrinter(output).print(*machine);
    output.close();
    REQUIRE(output.good());
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


DRIVER_SOURCE = r"""
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void native_indexed(int *, int *, int *);
void signed_indexed(int *, int *, int *);
void signed_indexed_evl0(int *, int *, int *);
int int_add(void); int int_min(void); int int_max(void); int int_and(void);
int int_or(void); int int_xor(void); int int_mul(void);
int int_add_lane0_clear(void); int int_add_evl0(void);
int int_add_external_seed(int);
float float_add(void); float float_mul(void); float float_min(void); float float_max(void);
float float_min_acc_nan(void); float float_min_lane_nan(void);
float float_max_acc_nan(void); float float_max_lane_nan(void);
float float_mul_acc_nan(void); float float_mul_lane_nan(void);
float float_mul_signed_zero(void);
float float_min_signed_zero(void); float float_max_signed_zero(void);
#define MASK_DECL(N) int mask_or_n##N(void); int mask_and_n##N(void); int mask_xor_n##N(void)
MASK_DECL(1); MASK_DECL(3); MASK_DECL(7); MASK_DECL(31);
int scalable_add(int *, int, int, int *);
float scalable_float_mul(float *, int, float, int *);
int scalable_gather(int *, int *, int);
int scalable_signed_gather(int *, int *, int);
int scalable_reverse_1(int *, int *, int *, int);
int scalable_reverse_2(int *, int *, int *, int);
int scalable_reverse_4(int *, int *, int *, int);

static uint32_t fbits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

static int check_indexed(void) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page != 4096) return 1;
    int *load = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int *scatter = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (load == MAP_FAILED || scatter == MAP_FAILED) return 2;
    if (mprotect((char *)load + page, page, PROT_NONE) != 0 ||
        mprotect((char *)scatter + page, page, PROT_NONE) != 0) return 3;
    for (int i = 0; i < 16; ++i) { load[i] = 100 + i; scatter[i] = -1; }
    int out[7] = {0};
    native_indexed(out, scatter, load);
    const int expected[7] = {100, 101, -99, 102, 103, -99, 100};
    for (int i = 0; i < 7; ++i) if (out[i] != expected[i]) return 10 + i;
    if (scatter[0] != 77 || scatter[1] != 22 || scatter[2] != 44 ||
        scatter[3] != 55) return 20;

    for (int i = 0; i < 16; ++i) scatter[i] = -1;
    int signed_out[7];
    signed_indexed(signed_out, scatter + 8, load + 8);
    const int signed_expected[7] = {106, 107, -99, 109, 110, 108, 108};
    for (int i = 0; i < 7; ++i) if (signed_out[i] != signed_expected[i]) return 30 + i;
    if (scatter[6] != 11 || scatter[7] != 22 || scatter[9] != 44 ||
        scatter[10] != 55 || scatter[8] != 77) return 40;
    int snapshot[16]; memcpy(snapshot, scatter, sizeof(snapshot));
    for (int i = 0; i < 7; ++i) signed_out[i] = 1234;
    signed_indexed_evl0(signed_out, scatter + 1024, load + 1024);
    for (int i = 0; i < 7; ++i) if (signed_out[i] != -99) return 50 + i;
    if (memcmp(snapshot, scatter, sizeof(snapshot)) != 0) return 60;
    munmap(load, (size_t)page * 2); munmap(scatter, (size_t)page * 2);
    return 0;
}

static int check_reductions(void) {
    const int32_t values[7] = {INT32_MIN, -3, 5, 7, -11, 13, INT32_MAX};
    const int active[7] = {1,0,1,1,0,1,0};
    int32_t add = 17, mn = 42, mx = -42, av = -1, ov = 0, xv = 0x12345678;
    for (int i = 0; i < 6; ++i) if (active[i]) {
        add = (int32_t)((uint32_t)add + (uint32_t)values[i]);
        if (values[i] < mn) mn = values[i];
        if (values[i] > mx) mx = values[i];
        av &= values[i]; ov |= values[i]; xv ^= values[i];
    }
    if (int_add() != add) return 1;
    if (int_min() != mn) return 2;
    if (int_max() != mx) return 3;
    if (int_and() != av) return 4;
    if (int_or() != ov) return 5;
    if (int_xor() != xv) return 6;
    if (int_mul() != 2730) return 7;
    if (int_add_lane0_clear() != 125) return 8;
    if (int_add_evl0() != 123) return 9;
    if (int_add_external_seed(37) != 65) return 10;

    volatile float seq = 0.25F;
    const float fv[7] = {1.5F, -2.0F, 3.25F, -0.0F, 0.0F, 4.0F, NAN};
    const int fa[7] = {1,1,0,1,0,1,0};
    for (int i = 0; i < 6; ++i) if (fa[i]) seq = seq + fv[i];
    if (fbits(float_add()) != fbits(seq)) return 11;
    volatile float product = 1.0000004F;
    const float mv[7] = {1.0e20F, 1.0e-20F, 3.25F, 1.0000001F,
                         0.0F, 1.0000002F, NAN};
    for (int i = 0; i < 6; ++i) if (fa[i]) product = product * mv[i];
    if (fbits(float_mul()) != fbits(product)) return 12;
    if (float_min() != -2.0F || float_max() != 4.0F) return 13;
    if (!isnan(float_min_acc_nan()) || float_min_lane_nan() != -3.0F) return 14;
    if (signbit(float_min_signed_zero()) || !signbit(float_max_signed_zero())) return 15;
    if (!isnan(float_max_acc_nan()) || float_max_lane_nan() != 3.0F) return 16;
    if (!isnan(float_mul_acc_nan()) || !isnan(float_mul_lane_nan())) return 17;
    if (float_mul_signed_zero() != 0.0F || !signbit(float_mul_signed_zero())) return 18;
    return 0;
}

static int mask_source(int lane) { return lane % 3 != 1; }
static int mask_active(int lane) { return lane % 4 != 2; }
static int mask_oracle(int lanes, int evl, int kind, int seed) {
    int result = seed;
    for (int lane = 0; lane < lanes && lane < evl; ++lane) if (mask_active(lane)) {
        if (kind == 0) result = result || mask_source(lane);
        else if (kind == 1) result = result && mask_source(lane);
        else result ^= mask_source(lane);
    }
    return result;
}
#define CHECK_MASK(N) do { int evl = (N) == 1 ? 0 : (N) - 1; \
    if (mask_or_n##N() != mask_oracle((N), evl, 0, 0)) return 100 + (N); \
    if (mask_and_n##N() != mask_oracle((N), evl, 1, 1)) return 200 + (N); \
    if (mask_xor_n##N() != mask_oracle((N), evl, 2, 1)) return 300 + (N); } while (0)

static int check_scalable(void) {
    int ints[64], out[64], vl = 0;
    float floats[64];
    for (int i = 0; i < 64; ++i) { ints[i] = i + 1; floats[i] = 1.0F + i / 64.0F; out[i] = 0; }
    int sum = scalable_add(ints, 63, 7, &vl), expected = 7;
    for (int i = 0; i < vl; ++i) expected += ints[i];
    if (sum != expected || vl <= 0 || vl > 32) return 1;
    int fvl = 0; volatile float fp = 0.5F;
    float product = scalable_float_mul(floats, 63, 0.5F, &fvl);
    for (int i = 0; i < fvl; ++i) fp = fp * floats[i];
    if (fvl != vl || fbits(product) != fbits(fp)) return 2;
    int gvl = scalable_gather(out, ints, 63);
    if (gvl != vl) return 3;
    for (int i = 0; i < gvl; ++i) if (out[i] != ints[i]) return 4;
    memset(out, 0, sizeof(out));
    int ngvl = scalable_signed_gather(out, ints + 32, 63);
    if (ngvl != vl) return 5;
    for (int i = 0; i < ngvl; ++i) if (out[i] != ints[32 - i]) return 6;

    int base[256], scatter[256];
    for (int i = 0; i < 256; ++i) base[i] = 1000 + i;
    int (*kernels[3])(int *, int *, int *, int) = {
        scalable_reverse_1, scalable_reverse_2, scalable_reverse_4};
    const int magnitudes[3] = {1, 2, 4};
    for (int test = 0; test < 3; ++test) {
        memset(out, 0, sizeof(out));
        for (int i = 0; i < 256; ++i) scatter[i] = -1;
        int reverse_vl = kernels[test](out, scatter, base, 63);
        if (reverse_vl != vl) return 7 + test;
        for (int lane = 0; lane < reverse_vl; ++lane) {
            int index = (reverse_vl - 1 - lane) * magnitudes[test];
            if (out[lane] != base[index] || scatter[index] != base[index])
                return 10 + test;
        }
    }
    return 0;
}

int main(void) {
    int indexed = check_indexed(); if (indexed) return indexed;
    int reductions = check_reductions(); if (reductions) return 80 + reductions;
    CHECK_MASK(1); CHECK_MASK(3); CHECK_MASK(7); CHECK_MASK(31);
    int scalable = check_scalable(); if (scalable) return 240 + scalable;
    return 0;
}
"""


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def find_cxx() -> str:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("no host C++ compiler found")


def run_checked(command: list[str], timeout: float = 120.0) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False, timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout.decode(errors='replace')}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result


def main() -> int:
    try:
        cxx = find_cxx()
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="rvv-indexed-reduce-infra-") as temp:
            directory = Path(temp)
            source = directory / "probe.cpp"
            probe = directory / "probe"
            assembly = directory / "probe.s"
            obj = directory / "probe.o"
            driver = directory / "driver.c"
            executable = directory / "probe-riscv"
            source.write_text(textwrap.dedent(PROBE_SOURCE), encoding="utf-8")
            driver.write_text(textwrap.dedent(DRIVER_SOURCE), encoding="utf-8")
            run_checked([
                cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                str(ROOT / "include"), str(source),
                *(str(ROOT / item) for item in PROBE_SOURCES), "-o", str(probe),
            ])
            run_checked([str(probe), str(assembly)])
            asm = assembly.read_text(encoding="utf-8")
            for mnemonic in (
                "vloxei32.v", "vsoxei32.v", "vlse32.v", "vsse32.v",
                "vredsum.vs", "vredmin.vs",
                "vredmax.vs", "vredand.vs", "vredor.vs", "vredxor.vs",
                "vfredosum.vs", "vcpop.m", "vfirst.m", "mulw", "fmul.s",
            ):
                if mnemonic not in asm:
                    raise RuntimeError(f"assembly lacks {mnemonic}")
            if "PseudoV" in asm:
                raise RuntimeError("assembly contains an RVV pseudo")
            for function_name in (
                "scalable_reverse_1", "scalable_reverse_2", "scalable_reverse_4"
            ):
                match = re.search(
                    rf"(?ms)^{re.escape(function_name)}:\n(.*?)"
                    rf"(?=^\s*\.size\s+{re.escape(function_name)},|^\s*\.globl\s+|\Z)",
                    asm,
                )
                if match is None:
                    raise RuntimeError(f"assembly lacks body for {function_name}")
                body = match.group(1)
                if "vlse32.v" not in body or "vsse32.v" not in body:
                    raise RuntimeError(
                        f"reverse recipe {function_name} did not select signed strided memory"
                    )
                if "vloxei32.v" in body or "vsoxei32.v" in body:
                    raise RuntimeError(
                        f"reverse recipe {function_name} retained indexed memory"
                    )
                if "vfirst.m" in body:
                    raise RuntimeError(
                        f"reverse recipe {function_name} unexpectedly scalarized"
                    )
            run_checked([assembler, "-march=rv64gcv", "-mabi=lp64d",
                         str(assembly), "-o", str(obj)])
            disassembly = run_checked([objdump, "-dr", str(obj)]).stdout.decode()
            for mnemonic in (
                "vloxei32.v", "vsoxei32.v", "vlse32.v", "vsse32.v",
                "vfredosum.vs", "vcpop.m", "vfirst.m",
            ):
                if mnemonic not in disassembly:
                    raise RuntimeError(f"object disassembly lacks {mnemonic}")
            run_checked([
                gcc, "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                "-march=rv64gcv", "-mabi=lp64d", str(obj), str(driver),
                "-lm", "-o", str(executable),
            ])
            for vlen in VLENS:
                run_checked([
                    qemu, "-cpu",
                    f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                    str(executable),
                ], timeout=60.0)
                print(f"PASS indexed_reduction_qemu_vlen_{vlen}")
            print("PASS indexed_memory_guard_signed_fallback_and_duplicate_order")
            print("PASS native_and_sequential_reduction_numeric_boundaries")
            print("PASS mask_reduction_n1_n3_n7_n31")
            print("PASS final_zero_pseudo_gnu_as_and_objdump")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_indexed_reduction_backend: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
