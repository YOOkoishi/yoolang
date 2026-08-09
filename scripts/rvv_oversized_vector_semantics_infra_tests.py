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
LANES = (33, 63, 65)
VLENS = (128, 256, 512, 1024)
OPT_LEVELS = (0, 1, 2, 3)


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
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) throw std::runtime_error(#condition);                   \
    } while (false)

using Predicate = bool (*)(std::uint64_t);

std::vector<std::uint8_t> mask_bytes(std::uint64_t lanes, Predicate predicate) {
    std::vector<std::uint8_t> bytes((lanes + 7U) / 8U, 0);
    for (std::uint64_t lane = 0; lane < lanes; ++lane) {
        if (predicate(lane)) bytes[lane / 8U] |= 1U << (lane % 8U);
    }
    return bytes;
}

bool all_lanes(std::uint64_t) { return true; }
bool active_lane(std::uint64_t lane) { return lane % 5U != 0U; }
bool indexed_active_lane(std::uint64_t lane) {
    return active_lane(lane) && lane != 32U && lane != 64U;
}
bool mask_source_lane(std::uint64_t lane) { return lane % 3U != 1U; }
bool compare_passthrough_lane(std::uint64_t lane) { return lane % 2U != 0U; }
bool xor_mask_lane(std::uint64_t lane) { return lane % 3U == 0U; }
bool binary_mask_passthrough_lane(std::uint64_t lane) { return lane % 4U == 1U; }

std::vector<oir::Constant *> i32_vector(oir::Module &module, std::uint64_t lanes,
                                        std::int32_t (*value)(std::uint64_t)) {
    std::vector<oir::Constant *> values;
    values.reserve(lanes);
    for (std::uint64_t lane = 0; lane < lanes; ++lane)
        values.push_back(module.create_i32(value(lane)));
    return values;
}

std::int32_t rhs_value(std::uint64_t lane) {
    return static_cast<std::int32_t>(3U * lane + 7U);
}
std::int32_t load_passthrough(std::uint64_t lane) {
    return -1000 - static_cast<std::int32_t>(lane);
}
std::int32_t binary_passthrough(std::uint64_t lane) {
    return 7000 + static_cast<std::int32_t>(lane);
}
std::int32_t native_index(std::uint64_t lane) {
    return static_cast<std::int32_t>((lane * 7U + 3U) % 17U);
}
std::int32_t signed_index(std::uint64_t lane) {
    if (lane == 0U || lane == 32U || lane == 64U) return 1024;
    return static_cast<std::int32_t>(lane % 9U) - 4;
}
std::int32_t scatter_value(std::uint64_t lane) {
    return 10000 + static_cast<std::int32_t>(lane);
}
std::int32_t gather_passthrough(std::uint64_t lane) {
    return -2000 - static_cast<std::int32_t>(lane);
}
std::int32_t reduction_value(std::uint64_t lane) {
    auto value = static_cast<std::int32_t>((lane * 13U) % 17U) - 8;
    return value == 0 ? 3 : value;
}
std::int32_t compare_rhs_value(std::uint64_t lane) {
    return static_cast<std::int32_t>(lane % 11U) - 5;
}

std::vector<oir::Constant *> float_vector(oir::Module &module,
                                          std::uint64_t lanes,
                                          oir::ReductionKind kind) {
    std::vector<oir::Constant *> values;
    values.reserve(lanes);
    for (std::uint64_t lane = 0; lane < lanes; ++lane) {
        float value = 0.0F;
        if (kind == oir::ReductionKind::Mul) {
            const float pattern[] = {1.0000001F, 0.75F, -1.25F, 1.5F,
                                     0.5F,       -0.875F, 1.0000002F};
            value = pattern[lane % 7U];
        } else {
            const float pattern[] = {1.0e10F, -1.0e10F, 3.25F, -0.5F,
                                     2.0F,    -4.0F,     1.0000001F};
            value = pattern[lane % 7U];
            if (lane == 30U || lane == 62U) value = 1.0e20F;
            if (lane == 31U || lane == 63U) value = -1.0e20F;
            if (lane == 32U || lane == 64U) value = 3.25F;
        }
        values.push_back(module.create_f32(value));
    }
    return values;
}

void add_memory_function(oir::Module &module, std::uint64_t lanes) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *pointer = types.ptr_ty(i32);
    auto *function = module.create_function(
        "bundle_memory_n" + std::to_string(lanes),
        types.func_ty(types.void_ty(), {pointer, pointer, pointer, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, active_lane));
    auto *all = module.create_constant_mask(mask, mask_bytes(lanes, all_lanes));
    auto *load_pass = module.create_constant_vector(
        vector, i32_vector(module, lanes, load_passthrough));
    auto *binary_pass = module.create_constant_vector(
        vector, i32_vector(module, lanes, binary_passthrough));
    auto *rhs = module.create_constant_vector(vector, i32_vector(module, lanes, rhs_value));
    auto *loaded = builder.create_vp_load(
        vector, function->args()[2].get(), active, function->args()[3].get(),
        load_pass, oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Undisturbed, 4, "loaded");
    auto *sum = builder.create_vp_binary(
        oir::Instruction::OpID::Add, loaded, rhs, active,
        function->args()[3].get(), binary_pass,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, "sum");
    builder.create_vp_store(sum, function->args()[0].get(), all,
                            builder.i32(static_cast<std::int32_t>(lanes)),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_store(sum, function->args()[1].get(), active,
                            function->args()[3].get(),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
}

void add_mask_policy_function(oir::Module &module, std::uint64_t lanes) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *function = module.create_function(
        "bundle_mask_policy_n" + std::to_string(lanes),
        types.func_ty(i32, {types.ptr_ty(i32), i32, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *lhs = module.create_constant_vector(
        vector, i32_vector(module, lanes, reduction_value));
    auto *rhs = module.create_constant_vector(
        vector, i32_vector(module, lanes, compare_rhs_value));
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, active_lane));
    auto *compare_passthrough = module.create_constant_mask(
        mask, mask_bytes(lanes, compare_passthrough_lane));
    auto *xor_mask = module.create_constant_mask(mask, mask_bytes(lanes, xor_mask_lane));
    auto *binary_passthrough = module.create_constant_mask(
        mask, mask_bytes(lanes, binary_mask_passthrough_lane));
    auto *compared = builder.create_vp_icmp(
        oir::CmpPred::LT, lhs, rhs, active, function->args()[2].get(),
        compare_passthrough, oir::TailPolicy::Undisturbed,
        oir::MaskPolicy::Undisturbed, "compared");
    auto *combined = builder.create_vp_binary(
        oir::Instruction::OpID::Xor, compared, xor_mask, active,
        function->args()[2].get(), binary_passthrough,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed,
        "combined");
    auto *bits = builder.create_vector_cast(oir::VectorCastKind::ZExt, vector,
                                            combined, "bits");
    auto *all = module.create_constant_mask(mask, mask_bytes(lanes, all_lanes));
    builder.create_vp_store(bits, function->args()[0].get(), all,
                            builder.i32(static_cast<std::int32_t>(lanes)),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    auto *lane = builder.create_extract_element(combined, function->args()[1].get(), "lane");
    builder.create_ret(builder.create_zext(lane, i32, "result"));
}

void add_indexed_function(oir::Module &module, std::uint64_t lanes,
                          bool signed_fallback) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *pointer = types.ptr_ty(i32);
    const auto name = std::string(signed_fallback ? "bundle_signed_n"
                                                  : "bundle_indexed_n") +
                      std::to_string(lanes);
    auto *function = module.create_function(
        name, types.func_ty(types.void_ty(), {pointer, pointer, pointer, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    const auto predicate = signed_fallback ? indexed_active_lane : active_lane;
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, predicate));
    auto *all = module.create_constant_mask(mask, mask_bytes(lanes, all_lanes));
    auto *indices = module.create_constant_vector(
        vector, i32_vector(module, lanes,
                           signed_fallback ? signed_index : native_index));
    auto *passthrough = module.create_constant_vector(
        vector, i32_vector(module, lanes, gather_passthrough));
    auto *values = module.create_constant_vector(
        vector, i32_vector(module, lanes, scatter_value));
    auto *gather = builder.create_vp_gather(
        vector, function->args()[2].get(), indices, active,
        function->args()[3].get(), passthrough,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, 4,
        "gather");
    builder.create_vp_store(gather, function->args()[0].get(), all,
                            builder.i32(static_cast<std::int32_t>(lanes)),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_scatter(
        values, function->args()[1].get(), indices, active,
        function->args()[3].get(), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
}

const char *kind_name(oir::ReductionKind kind) {
    switch (kind) {
    case oir::ReductionKind::Add: return "add";
    case oir::ReductionKind::Mul: return "mul";
    case oir::ReductionKind::Min: return "min";
    case oir::ReductionKind::Max: return "max";
    case oir::ReductionKind::And: return "and";
    case oir::ReductionKind::Or: return "or";
    case oir::ReductionKind::Xor: return "xor";
    }
    throw std::runtime_error("unknown reduction kind");
}

std::int32_t int_seed(oir::ReductionKind kind) {
    switch (kind) {
    case oir::ReductionKind::Add: return 17;
    case oir::ReductionKind::Mul: return 3;
    case oir::ReductionKind::Min: return 100000;
    case oir::ReductionKind::Max: return -100000;
    case oir::ReductionKind::And: return -1;
    case oir::ReductionKind::Or: return 0;
    case oir::ReductionKind::Xor: return 0x12345678;
    }
    throw std::runtime_error("unknown reduction seed");
}

void add_int_reduction(oir::Module &module, std::uint64_t lanes,
                       oir::ReductionKind kind, const std::string &name,
                       std::int32_t constant_evl, bool dynamic) {
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *signature = dynamic
                          ? types.func_ty(types.int32_ty(), {types.int32_ty()})
                          : types.func_ty(types.int32_ty(), {});
    auto *function = module.create_function(name, signature);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_vector(
        vector, i32_vector(module, lanes, reduction_value));
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, active_lane));
    oir::Value *evl = dynamic ? static_cast<oir::Value *>(function->args()[0].get())
                              : static_cast<oir::Value *>(builder.i32(constant_evl));
    auto *result = builder.create_vp_reduction(
        kind, false, source, active, evl, builder.i32(int_seed(kind)),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "result");
    builder.create_ret(result);
}

void add_float_reduction(oir::Module &module, std::uint64_t lanes,
                         oir::ReductionKind kind) {
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.float_ty(), lanes);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *function = module.create_function(
        std::string("f") + kind_name(kind) + "_n" + std::to_string(lanes),
        types.func_ty(types.float_ty(), {types.int32_ty()}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_vector(vector, float_vector(module, lanes, kind));
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, active_lane));
    const float seed = kind == oir::ReductionKind::Min
                           ? 100000.0F
                           : (kind == oir::ReductionKind::Max ? -100000.0F
                                                              : 1.0000004F);
    auto *result = builder.create_vp_reduction(
        kind, true, source, active, function->args()[0].get(),
        module.create_f32(seed), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, "result");
    builder.create_ret(result);
}

void add_mask_reduction(oir::Module &module, std::uint64_t lanes,
                        oir::ReductionKind kind) {
    auto &types = module.types();
    auto *mask = types.fixed_vector_ty(types.int1_ty(), lanes);
    auto *function = module.create_function(
        std::string("mask_") + kind_name(kind) + "_n" +
            std::to_string(lanes),
        types.func_ty(types.int32_ty(), {types.int32_ty()}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *source = module.create_constant_mask(mask, mask_bytes(lanes, mask_source_lane));
    auto *active = module.create_constant_mask(mask, mask_bytes(lanes, active_lane));
    const bool seed = kind != oir::ReductionKind::Or;
    auto *reduced = builder.create_vp_reduction(
        kind, false, source, active, function->args()[0].get(), builder.i1(seed),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "reduced");
    builder.create_ret(builder.create_zext(reduced, types.int32_ty(), "result"));
}

std::vector<std::int32_t> evls(std::uint64_t lanes) {
    std::vector<std::int32_t> values = {
        0, 1, 31, 32, 33, static_cast<std::int32_t>(lanes - 1U),
        static_cast<std::int32_t>(lanes),
        static_cast<std::int32_t>(lanes + 1U)};
    std::vector<std::int32_t> unique;
    for (auto value : values) {
        bool found = false;
        for (auto old : unique) found |= old == value;
        if (!found) unique.push_back(value);
    }
    return unique;
}

std::unique_ptr<oir::Module> make_module() {
    auto module = std::make_unique<oir::Module>("rvv-oversized-semantics");
    const oir::ReductionKind integer_kinds[] = {
        oir::ReductionKind::Add, oir::ReductionKind::Mul,
        oir::ReductionKind::Min, oir::ReductionKind::Max,
        oir::ReductionKind::And, oir::ReductionKind::Or,
        oir::ReductionKind::Xor};
    const oir::ReductionKind floating_kinds[] = {
        oir::ReductionKind::Add, oir::ReductionKind::Mul,
        oir::ReductionKind::Min, oir::ReductionKind::Max};
    const oir::ReductionKind mask_kinds[] = {
        oir::ReductionKind::And, oir::ReductionKind::Or,
        oir::ReductionKind::Xor};
    for (auto lanes : {33U, 63U, 65U}) {
        add_memory_function(*module, lanes);
        add_mask_policy_function(*module, lanes);
        add_indexed_function(*module, lanes, false);
        add_indexed_function(*module, lanes, true);
        for (auto kind : integer_kinds) {
            add_int_reduction(*module, lanes, kind,
                              std::string("i") + kind_name(kind) + "_n" +
                                  std::to_string(lanes),
                              0, true);
        }
        for (auto kind : floating_kinds)
            add_float_reduction(*module, lanes, kind);
        for (auto kind : mask_kinds)
            add_mask_reduction(*module, lanes, kind);
        for (auto evl : evls(lanes)) {
            if (evl > static_cast<std::int32_t>(lanes)) continue;
            add_int_reduction(*module, lanes, oir::ReductionKind::Add,
                              "iadd_const_n" + std::to_string(lanes) + "_e" +
                                  std::to_string(evl),
                              evl, false);
        }
    }
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

void check_negative_evl() {
    oir::Module module("negative-oversized-evl");
    auto &types = module.types();
    auto *vector = types.fixed_vector_ty(types.int32_ty(), 33);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 33);
    auto *function = module.create_function(
        "negative_evl",
        types.func_ty(types.void_ty(), {types.ptr_ty(types.int32_ty())}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *active = module.create_constant_mask(mask, mask_bytes(33, all_lanes));
    auto *passthrough = module.create_constant_vector(
        vector, i32_vector(module, 33, load_passthrough));
    (void)builder.create_vp_load(
        vector, function->args()[0].get(), active, builder.i32(-1), passthrough,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, 4, "bad");
    builder.create_ret();
    std::string error;
    REQUIRE(!module.verify(&error));
    REQUIRE(error.find("OIRV_VP_EVL_RANGE") != std::string::npos);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    std::string error;
    target::TargetProfile profile;
    profile.march = "rv64gcv";
    REQUIRE(target::finalize_target_profile(profile, error));
    check_negative_evl();
    auto machine = pass::oir_to_mir::lower_with_vregs(*make_module(), profile);
    machine->target().arch = profile.march;
    machine->target().has_vector = true;
    machine->target().minimum_vlen_bits = profile.minimum_vlen_bits;
    const auto pre = mir::verify_module(*machine, mir::MIRVerificationStage::PreRA);
    if (!pre.ok) throw std::runtime_error(pre.message);
    pass::PassContext context;
    context.set_machine_module(std::move(machine));
    pass::MIRRegAllocPass regalloc;
    const auto allocated = regalloc.run(context);
    if (!allocated.success) throw std::runtime_error(allocated.message);
    machine = context.take_machine_module();
    REQUIRE(pass::expand_machine_pseudos(*machine, error));
    const auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
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


def unique_evls(lanes: int) -> list[int]:
    return list(dict.fromkeys((0, 1, 31, 32, 33, lanes - 1, lanes, lanes + 1)))


def constant_evls(lanes: int) -> list[int]:
    return [evl for evl in unique_evls(lanes) if evl <= lanes]


def declarations() -> str:
    lines: list[str] = []
    for lanes in LANES:
        lines.extend(
            [
                f"void bundle_memory_n{lanes}(int *, int *, int *, int);",
                f"int bundle_mask_policy_n{lanes}(int *, int, int);",
                f"void bundle_indexed_n{lanes}(int *, int *, int *, int);",
                f"void bundle_signed_n{lanes}(int *, int *, int *, int);",
            ]
        )
        for kind in ("add", "mul", "min", "max", "and", "or", "xor"):
            lines.append(f"int i{kind}_n{lanes}(int);")
        for kind in ("add", "mul", "min", "max"):
            lines.append(f"float f{kind}_n{lanes}(int);")
        for kind in ("and", "or", "xor"):
            lines.append(f"int mask_{kind}_n{lanes}(int);")
        for evl in constant_evls(lanes):
            lines.append(f"int iadd_const_n{lanes}_e{evl}(void);")
    return "\n".join(lines)


def lane_block(lanes: int) -> str:
    evls = ", ".join(str(value) for value in unique_evls(lanes))
    constant_checks = "\n".join(
        f"  if (iadd_const_n{lanes}_e{evl}() != int_oracle({lanes}, {evl}, K_ADD)) return {700 + index};"
        for index, evl in enumerate(constant_evls(lanes))
    )
    return f"""
static int check_n{lanes}(int *guard, int *guard_load, int *guard_scatter) {{
  const int evls[] = {{{evls}}};
  int input[80], output[80], side[80], base[128], scatter[128];
  int mask_output[80];
  for (int lane = 0; lane < 80; ++lane) input[lane] = 100 + lane;
  for (int lane = 0; lane < 128; ++lane) base[lane] = 2000 + lane;
  for (unsigned test = 0; test < sizeof(evls) / sizeof(evls[0]); ++test) {{
    const int evl = evls[test];
    const int effective = evl < {lanes} ? evl : {lanes};
    for (int lane = 0; lane < 80; ++lane) output[lane] = side[lane] = -777777;
    bundle_memory_n{lanes}(output, side, input, evl);
    for (int lane = 0; lane < {lanes}; ++lane) {{
      const int active = active_lane(lane);
      const int expected = active && lane < effective
                               ? input[lane] + rhs_value(lane)
                               : binary_passthrough(lane);
      if (output[lane] != expected) return 10 + lane;
      if (side[lane] != (active && lane < effective ? expected : -777777))
        return 100 + lane;
      const int compared = active && lane < effective
                               ? reduction_value(lane) < compare_rhs_value(lane)
                               : lane % 2 != 0;
      const int expected_mask = active && lane < effective
                                    ? compared ^ (lane % 3 == 0)
                                    : lane % 4 == 1;
      const int extracted = bundle_mask_policy_n{lanes}(mask_output, lane, evl);
      if (mask_output[lane] != expected_mask || extracted != expected_mask)
        return 170 + lane;
    }}

    for (int lane = 0; lane < 128; ++lane) scatter[lane] = -1;
    for (int lane = 0; lane < 80; ++lane) output[lane] = -777777;
    bundle_indexed_n{lanes}(output, scatter, base, evl);
    int expected_scatter[128];
    for (int lane = 0; lane < 128; ++lane) expected_scatter[lane] = -1;
    for (int lane = 0; lane < {lanes}; ++lane) {{
      const int active = active_lane(lane) && lane < effective;
      const int index = native_index(lane);
      const int expected = active ? base[index] : gather_passthrough(lane);
      if (output[lane] != expected) return 200 + lane;
      if (active) expected_scatter[index] = scatter_value(lane);
    }}
    for (int lane = 0; lane < 128; ++lane)
      if (scatter[lane] != expected_scatter[lane]) return 300 + lane;

    if (iadd_n{lanes}(evl) != int_oracle({lanes}, evl, K_ADD)) return 410;
    if (imul_n{lanes}(evl) != int_oracle({lanes}, evl, K_MUL)) return 411;
    if (imin_n{lanes}(evl) != int_oracle({lanes}, evl, K_MIN)) return 412;
    if (imax_n{lanes}(evl) != int_oracle({lanes}, evl, K_MAX)) return 413;
    if (iand_n{lanes}(evl) != int_oracle({lanes}, evl, K_AND)) return 414;
    if (ior_n{lanes}(evl) != int_oracle({lanes}, evl, K_OR)) return 415;
    if (ixor_n{lanes}(evl) != int_oracle({lanes}, evl, K_XOR)) return 416;
    if (fbits(fadd_n{lanes}(evl)) != fbits(float_oracle({lanes}, evl, K_ADD))) return 420;
    if (fbits(fmul_n{lanes}(evl)) != fbits(float_oracle({lanes}, evl, K_MUL))) return 421;
    if (fmin_n{lanes}(evl) != float_oracle({lanes}, evl, K_MIN)) return 422;
    if (fmax_n{lanes}(evl) != float_oracle({lanes}, evl, K_MAX)) return 423;
    if (mask_and_n{lanes}(evl) != mask_oracle({lanes}, evl, K_AND)) return 430;
    if (mask_or_n{lanes}(evl) != mask_oracle({lanes}, evl, K_OR)) return 431;
    if (mask_xor_n{lanes}(evl) != mask_oracle({lanes}, evl, K_XOR)) return 432;
  }}

  for (int lane = 0; lane < 80; ++lane) output[lane] = -777777;
  bundle_memory_n{lanes}(output, guard, guard, 0);
  for (int lane = 0; lane < {lanes}; ++lane)
    if (output[lane] != binary_passthrough(lane)) return 500 + lane;
  bundle_indexed_n{lanes}(output, guard, guard, 0);

  for (int lane = 0; lane < 128; ++lane) guard_load[lane] = 3000 + lane;
  for (int lane = 0; lane < 128; ++lane) guard_scatter[lane] = -1;
  for (int lane = 0; lane < 80; ++lane) output[lane] = -777777;
  bundle_signed_n{lanes}(output, guard_scatter, guard_load, {lanes + 1});
  int signed_expected[128];
  for (int lane = 0; lane < 128; ++lane) signed_expected[lane] = -1;
  for (int lane = 0; lane < {lanes}; ++lane) {{
    const int active = indexed_active_lane(lane);
    const int index = signed_index(lane);
    const int expected = active ? guard_load[index] : gather_passthrough(lane);
    if (output[lane] != expected) return 550 + lane;
    if (active) signed_expected[index + 64] = scatter_value(lane);
  }}
  for (int index = -4; index <= 4; ++index)
    if (guard_scatter[index] != signed_expected[index + 64]) return 650 + index;
  bundle_signed_n{lanes}(output, guard, guard, 0);
{constant_checks}
  return 0;
}}
"""


def driver_source() -> str:
    blocks = "\n".join(lane_block(lanes) for lanes in LANES)
    calls = "\n".join(
        f"  result = check_n{lanes}(guard, load_base, scatter_base); if (result) return {index + 1} * 1000 + result;"
        for index, lanes in enumerate(LANES)
    )
    return f"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

{declarations()}

enum {{ K_ADD, K_MUL, K_MIN, K_MAX, K_AND, K_OR, K_XOR }};

static int active_lane(int lane) {{ return lane % 5 != 0; }}
static int indexed_active_lane(int lane) {{
  return active_lane(lane) && lane != 32 && lane != 64;
}}
static int rhs_value(int lane) {{ return 3 * lane + 7; }}
static int binary_passthrough(int lane) {{ return 7000 + lane; }}
static int native_index(int lane) {{ return (lane * 7 + 3) % 17; }}
static int signed_index(int lane) {{
  if (lane == 0 || lane == 32 || lane == 64) return 1024;
  return lane % 9 - 4;
}}
static int scatter_value(int lane) {{ return 10000 + lane; }}
static int gather_passthrough(int lane) {{ return -2000 - lane; }}
static int reduction_value(int lane) {{
  int value = (lane * 13) % 17 - 8;
  return value == 0 ? 3 : value;
}}
static int compare_rhs_value(int lane) {{ return lane % 11 - 5; }}

static int int_oracle(int lanes, int evl, int kind) {{
  uint32_t bits = kind == K_ADD ? 17U :
                  kind == K_MUL ? 3U :
                  kind == K_AND ? UINT32_MAX :
                  kind == K_XOR ? 0x12345678U : 0U;
  int signed_result = kind == K_MIN ? 100000 :
                      kind == K_MAX ? -100000 : (int32_t)bits;
  const int effective = evl < lanes ? evl : lanes;
  for (int lane = 0; lane < effective; ++lane) if (active_lane(lane)) {{
    const int value = reduction_value(lane);
    if (kind == K_ADD) bits += (uint32_t)value;
    else if (kind == K_MUL) bits *= (uint32_t)value;
    else if (kind == K_MIN) {{ if (value < signed_result) signed_result = value; }}
    else if (kind == K_MAX) {{ if (value > signed_result) signed_result = value; }}
    else if (kind == K_AND) bits &= (uint32_t)value;
    else if (kind == K_OR) bits |= (uint32_t)value;
    else bits ^= (uint32_t)value;
  }}
  return kind == K_MIN || kind == K_MAX ? signed_result : (int32_t)bits;
}}

static float float_value(int lane, int kind) {{
  if (kind == K_MUL) {{
    const float pattern[] = {{1.0000001F, 0.75F, -1.25F, 1.5F,
                              0.5F, -0.875F, 1.0000002F}};
    return pattern[lane % 7];
  }}
  const float pattern[] = {{1.0e10F, -1.0e10F, 3.25F, -0.5F,
                            2.0F, -4.0F, 1.0000001F}};
  if (lane == 30 || lane == 62) return 1.0e20F;
  if (lane == 31 || lane == 63) return -1.0e20F;
  if (lane == 32 || lane == 64) return 3.25F;
  return pattern[lane % 7];
}}

static float float_oracle(int lanes, int evl, int kind) {{
  volatile float result = kind == K_MIN ? 100000.0F :
                          kind == K_MAX ? -100000.0F : 1.0000004F;
  const int effective = evl < lanes ? evl : lanes;
  for (int lane = 0; lane < effective; ++lane) if (active_lane(lane)) {{
    const float value = float_value(lane, kind);
    if (kind == K_ADD) result = result + value;
    else if (kind == K_MUL) result = result * value;
    else if (kind == K_MIN) {{ if (value < result) result = value; }}
    else if (value > result) result = value;
  }}
  return result;
}}

static uint32_t fbits(float value) {{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}}

static int mask_oracle(int lanes, int evl, int kind) {{
  int result = kind != K_OR;
  const int effective = evl < lanes ? evl : lanes;
  for (int lane = 0; lane < effective; ++lane) if (active_lane(lane)) {{
    const int source = lane % 3 != 1;
    if (kind == K_AND) result = result && source;
    else if (kind == K_OR) result = result || source;
    else result ^= source;
  }}
  return result;
}}

{blocks}

int main(void) {{
  const long page = sysconf(_SC_PAGESIZE);
  if (page != 4096) return 1;
  char *load_map = mmap(0, (size_t)page * 3U, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *scatter_map = mmap(0, (size_t)page * 3U, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (load_map == MAP_FAILED || scatter_map == MAP_FAILED) return 2;
  if (mprotect(load_map, (size_t)page, PROT_NONE) != 0 ||
      mprotect(load_map + 2 * page, (size_t)page, PROT_NONE) != 0 ||
      mprotect(scatter_map, (size_t)page, PROT_NONE) != 0 ||
      mprotect(scatter_map + 2 * page, (size_t)page, PROT_NONE) != 0) return 3;
  int *load_base = (int *)(load_map + page) + 128;
  int *scatter_base = (int *)(scatter_map + page) + 128;
  int *guard = (int *)load_map;
  int result = 0;
{calls}
  munmap(load_map, (size_t)page * 3U);
  munmap(scatter_map, (size_t)page * 3U);
  return 0;
}}
"""


def require_tool(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise RuntimeError(f"required tool not found: {name}")
    return resolved


def find_cxx() -> str:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise RuntimeError("no host C++ compiler found")


def run_checked(command: list[str], *, timeout: float = 180.0) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command,
        cwd=ROOT,
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


def main() -> int:
    try:
        cxx = find_cxx()
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="rvv-oversized-semantics-") as temp:
            directory = Path(temp)
            source = directory / "probe.cpp"
            probe = directory / "probe"
            assembly = directory / "probe.s"
            obj = directory / "probe.o"
            driver = directory / "driver.c"
            source.write_text(textwrap.dedent(PROBE_SOURCE), encoding="utf-8")
            driver.write_text(textwrap.dedent(driver_source()), encoding="utf-8")
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
                    str(probe),
                ]
            )
            run_checked([str(probe), str(assembly)])
            asm = assembly.read_text(encoding="utf-8")
            for mnemonic in (
                "vloxei32.v",
                "vsoxei32.v",
                "vfirst.m",
                "vredsum.vs",
                "vredmin.vs",
                "vredmax.vs",
                "vredand.vs",
                "vredor.vs",
                "vredxor.vs",
                "vfredosum.vs",
                "mulw",
                "fmul.s",
            ):
                if mnemonic not in asm:
                    raise RuntimeError(f"assembly lacks {mnemonic}")
            for lanes in LANES:
                body = re.search(
                    rf"(?ms)^bundle_signed_n{lanes}:\n(.*?)"
                    rf"(?=^\s*\.size\s+bundle_signed_n{lanes},|^\s*\.globl\s+|\Z)",
                    asm,
                )
                if body is None or "vfirst.m" not in body.group(1):
                    raise RuntimeError(f"N={lanes} signed indexes did not use ordered fallback")
                native = re.search(
                    rf"(?ms)^bundle_indexed_n{lanes}:\n(.*?)"
                    rf"(?=^\s*\.size\s+bundle_indexed_n{lanes},|^\s*\.globl\s+|\Z)",
                    asm,
                )
                if native is None or "vloxei32.v" not in native.group(1):
                    raise RuntimeError(f"N={lanes} safe indexes did not use native gather")
            if "Pseudo" in asm or "RVVLoadIndexed" in asm:
                raise RuntimeError("final assembly leaks an RVV pseudo")
            run_checked(
                [assembler, "-march=rv64gcv", "-mabi=lp64d", str(assembly), "-o", str(obj)]
            )
            disassembly = run_checked([objdump, "-dr", str(obj)]).stdout.decode()
            for mnemonic in ("vloxei32.v", "vsoxei32.v", "vfirst.m", "vfredosum.vs"):
                if mnemonic not in disassembly:
                    raise RuntimeError(f"object disassembly lacks {mnemonic}")
            for opt in OPT_LEVELS:
                executable = directory / f"probe-O{opt}"
                run_checked(
                    [
                        gcc,
                        "-static",
                        f"-O{opt}",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-march=rv64gcv",
                        "-mabi=lp64d",
                        str(obj),
                        str(driver),
                        "-o",
                        str(executable),
                    ]
                )
                for vlen in VLENS:
                    run_checked(
                        [
                            qemu,
                            "-cpu",
                            f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                            str(executable),
                        ],
                        timeout=90.0,
                    )
                    print(f"PASS oversized_O{opt}_qemu_vlen_{vlen}")
            print("PASS oversized_N33_N63_N65_constant_and_dynamic_piece_evl")
            print("PASS oversized_guard_sparse_mask_native_and_signed_indexed")
            print("PASS oversized_cross_piece_integer_float_and_mask_reductions")
            print("PASS oversized_negative_evl_final_zero_pseudo_gnu_as_objdump")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_oversized_vector_semantics: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
