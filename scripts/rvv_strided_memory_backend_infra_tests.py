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
#include <memory>
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

void add_fixed_i32_ap(oir::Module &module, const char *name, std::int32_t stride) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *pointer = types.ptr_ty(i32);
    auto *function = module.create_function(
        name, types.func_ty(types.void_ty(), {pointer, pointer, pointer, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module); builder.set_insert_point(entry);
    auto *active = module.create_constant_mask(mask, {0x47});
    auto *all = module.create_constant_mask(mask, {0x7f});
    auto *lanes = builder.create_step_vector(vector, "lanes");
    auto *scale = builder.create_splat(vector, builder.i32(stride), "scale");
    auto *indices = builder.create_vp_binary(
        oir::Instruction::OpID::Mul, lanes, scale, active,
        function->args()[3].get(), module.create_undef(vector),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "indices");
    auto *passthrough = module.create_constant_vector(
        vector, i32_values(module, {-90, -91, -92, -93, -94, -95, -96}));
    auto *values = module.create_constant_vector(
        vector, i32_values(module, {101, 102, 103, 104, 105, 106, 107}));
    auto *loaded = builder.create_vp_gather(
        vector, function->args()[2].get(), indices, active,
        function->args()[3].get(), passthrough,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, 4, "loaded");
    builder.create_vp_store(loaded, function->args()[0].get(), all, builder.i32(7),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_scatter(values, function->args()[1].get(), indices, active,
                              function->args()[3].get(), oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
}

void add_fixed_float_ap(oir::Module &module) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *indices_type = types.fixed_vector_ty(i32, 7);
    auto *vector = types.fixed_vector_ty(f32, 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *pointer = types.ptr_ty(f32);
    auto *function = module.create_function(
        "fixed_float_pos",
        types.func_ty(types.void_ty(), {pointer, pointer, pointer, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module); builder.set_insert_point(entry);
    auto *active = module.create_constant_mask(mask, {0x47});
    auto *all = module.create_constant_mask(mask, {0x7f});
    auto *lanes = builder.create_step_vector(indices_type, "lanes");
    auto *scale = builder.create_splat(indices_type, builder.i32(2), "scale");
    auto *indices = builder.create_vp_binary(
        oir::Instruction::OpID::Mul, lanes, scale, active,
        function->args()[3].get(), module.create_undef(indices_type),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "indices");
    auto *passthrough = module.create_constant_vector(
        vector, f32_values(module, {-9.0F, -9.1F, -9.2F, -9.3F,
                                    -9.4F, -9.5F, -9.6F}));
    auto *values = module.create_constant_vector(
        vector, f32_values(module, {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F}));
    auto *loaded = builder.create_vp_gather(
        vector, function->args()[2].get(), indices, active,
        function->args()[3].get(), passthrough,
        oir::TailPolicy::Undisturbed, oir::MaskPolicy::Undisturbed, 4, "loaded");
    builder.create_vp_store(loaded, function->args()[0].get(), all, builder.i32(7),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_scatter(values, function->args()[1].get(), indices, active,
                              function->args()[3].get(), oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
}

void add_segment2(oir::Module &module) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *pointer = types.ptr_ty(i32);
    auto *function = module.create_function(
        "segment2_i32",
        types.func_ty(types.void_ty(), {pointer, pointer, pointer, pointer, i32}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module); builder.set_insert_point(entry);
    auto *load_field1 = builder.create_gep(function->args()[3].get(), pointer,
                                           {builder.i32(1)}, "load.field1");
    auto *store_field1 = builder.create_gep(function->args()[2].get(), pointer,
                                            {builder.i32(1)}, "store.field1");
    auto *active = module.create_constant_mask(mask, {0x47});
    auto *all = module.create_constant_mask(mask, {0x7f});
    auto *lanes = builder.create_step_vector(vector, "lanes");
    auto *two = builder.create_splat(vector, builder.i32(2), "two");
    auto *indices = builder.create_vp_binary(
        oir::Instruction::OpID::Mul, lanes, two, active,
        function->args()[4].get(), module.create_undef(vector),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, "indices");
    auto *field0 = builder.create_vp_gather(
        vector, function->args()[3].get(), indices, active,
        function->args()[4].get(), module.create_undef(vector),
        oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 4, "field0");
    auto *field1 = builder.create_vp_gather(
        vector, load_field1, indices, active, function->args()[4].get(),
        module.create_undef(vector), oir::TailPolicy::Agnostic,
        oir::MaskPolicy::Agnostic, 4, "field1");
    builder.create_vp_store(field0, function->args()[0].get(), all, builder.i32(7),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_store(field1, function->args()[1].get(), all, builder.i32(7),
                            oir::TailPolicy::Agnostic,
                            oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_scatter(field0, function->args()[2].get(), indices, active,
                              function->args()[4].get(), oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    builder.create_vp_scatter(field1, store_field1, indices, active,
                              function->args()[4].get(), oir::TailPolicy::Agnostic,
                              oir::MaskPolicy::Agnostic, 4);
    builder.create_ret();
}

void add_non_affine_fallbacks(oir::Module &module) {
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *vector = types.fixed_vector_ty(i32, 7);
    auto *mask = types.fixed_vector_ty(types.int1_ty(), 7);
    auto *pointer = types.ptr_ty(i32);
    auto *signature = types.func_ty(types.void_ty(), {pointer, pointer, pointer});

    {
        auto *function = module.create_function("nonlinear_indexed", signature);
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *active = module.create_constant_mask(mask, {0x7f});
        auto *indices = module.create_constant_vector(
            vector, i32_values(module, {0, 1, 4, 2, 6, 3, 5}));
        auto *loaded = builder.create_vp_gather(
            vector, function->args()[2].get(), indices, active, builder.i32(7),
            module.create_undef(vector), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, 4, "loaded");
        builder.create_vp_store(loaded, function->args()[0].get(), active, builder.i32(7),
                                oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_vp_scatter(loaded, function->args()[1].get(), indices, active,
                                  builder.i32(7), oir::TailPolicy::Agnostic,
                                  oir::MaskPolicy::Agnostic, 4);
        builder.create_ret();
    }
    {
        auto *function = module.create_function("signed_irregular", signature);
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(&module); builder.set_insert_point(entry);
        auto *active = module.create_constant_mask(mask, {0x7f});
        auto *indices = module.create_constant_vector(
            vector, i32_values(module, {-1, 0, -2, 2, 4, 3, 1}));
        auto *loaded = builder.create_vp_gather(
            vector, function->args()[2].get(), indices, active, builder.i32(7),
            module.create_undef(vector), oir::TailPolicy::Agnostic,
            oir::MaskPolicy::Agnostic, 4, "loaded");
        builder.create_vp_store(loaded, function->args()[0].get(), active, builder.i32(7),
                                oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_vp_scatter(loaded, function->args()[1].get(), indices, active,
                                  builder.i32(7), oir::TailPolicy::Agnostic,
                                  oir::MaskPolicy::Agnostic, 4);
        builder.create_ret();
    }
}

std::unique_ptr<oir::Module> make_module() {
    auto module = std::make_unique<oir::Module>("rvv-strided-memory");
    add_fixed_i32_ap(*module, "fixed_pos", 2);
    add_fixed_i32_ap(*module, "fixed_neg", -2);
    add_fixed_float_ap(*module);
    add_segment2(*module);
    add_non_affine_fallbacks(*module);
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    const auto &load_desc = mir::instruction_desc(mir::Opcode::RISCVVLoadStrided);
    const auto &store_desc = mir::instruction_desc(mir::Opcode::RISCVVStoreStrided);
    REQUIRE(load_desc.may_load() && !load_desc.may_store());
    REQUIRE(store_desc.may_store() && !store_desc.may_load());
    REQUIRE(!load_desc.has_flag(mir::MIF_Pseudo));
    REQUIRE(!store_desc.has_flag(mir::MIF_Pseudo));
    REQUIRE(load_desc.implicitly_uses(mir::MVS_VL));
    REQUIRE(load_desc.implicitly_uses(mir::MVS_VTYPE));

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
    bool checked_segment_tuple_rejection = false;
    for (const auto &function : machine->functions()) {
        for (const auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                if (instruction.opcode() != mir::Opcode::RISCVVLoadSegment2) continue;
                auto original = instruction.operands()[1].reg_value();
                instruction.operands()[1].set_reg(mir::Register::physical_vector(
                    "v28", mir::RegisterClass::VR, instruction.vector_info().vector_type));
                const auto rejected =
                    mir::verify_module(*machine, mir::MIRVerificationStage::Final);
                REQUIRE(!rejected.ok);
                REQUIRE(rejected.message.find("segment fields must be consecutive") !=
                        std::string::npos);
                instruction.operands()[1].set_reg(std::move(original));
                checked_segment_tuple_rejection = true;
                break;
            }
            if (checked_segment_tuple_rejection) break;
        }
        if (checked_segment_tuple_rejection) break;
    }
    REQUIRE(checked_segment_tuple_rejection);
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void fixed_pos(int *, int *, int *, int);
void fixed_neg(int *, int *, int *, int);
void fixed_float_pos(float *, float *, float *, int);
void segment2_i32(int *, int *, int *, int *, int);
void nonlinear_indexed(int *, int *, int *);
void signed_irregular(int *, int *, int *);

struct guarded {
    char *mapping;
    int *middle;
    long page;
};

static struct guarded make_guarded(void) {
    struct guarded result = {0};
    result.page = sysconf(_SC_PAGESIZE);
    result.mapping = mmap(0, (size_t)result.page * 3U, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result.mapping == MAP_FAILED) return result;
    if (mprotect(result.mapping, (size_t)result.page, PROT_NONE) != 0 ||
        mprotect(result.mapping + 2 * result.page, (size_t)result.page, PROT_NONE) != 0) {
        munmap(result.mapping, (size_t)result.page * 3U);
        result.mapping = MAP_FAILED;
        return result;
    }
    result.middle = (int *)(result.mapping + result.page);
    return result;
}

static void destroy_guarded(struct guarded value) {
    munmap(value.mapping, (size_t)value.page * 3U);
}

static int check_direction(int negative) {
    struct guarded load = make_guarded(), scatter = make_guarded();
    if (load.mapping == MAP_FAILED || scatter.mapping == MAP_FAILED) return 1;
    const int count = (int)(load.page / (long)sizeof(int));
    for (int i = 0; i < count; ++i) {
        load.middle[i] = 1000 + i;
        scatter.middle[i] = -1;
    }
    int *load_base = negative ? load.middle + 10 : load.middle + count - 10;
    int *scatter_base = negative ? scatter.middle + 10 : scatter.middle + count - 10;
    int out[7] = {0};
    if (negative) fixed_neg(out, scatter_base, load_base, 5);
    else fixed_pos(out, scatter_base, load_base, 5);
    for (int lane = 0; lane < 7; ++lane) {
        const int active = lane < 5 && (lane == 0 || lane == 1 || lane == 2);
        const int index = (negative ? -2 : 2) * lane;
        const int expected = active ? load_base[index] : -90 - lane;
        if (out[lane] != expected) return 10 + lane;
        if (active && scatter_base[index] != 101 + lane) return 20 + lane;
    }
    int snapshot[16];
    memcpy(snapshot, scatter_base - 8, sizeof(snapshot));
    if (negative) fixed_neg(out, scatter_base, load.middle - 1024, 0);
    else fixed_pos(out, scatter_base, load.middle + count + 1024, 0);
    for (int lane = 0; lane < 7; ++lane)
        if (out[lane] != -90 - lane) return 30 + lane;
    if (memcmp(snapshot, scatter_base - 8, sizeof(snapshot)) != 0) return 40;
    destroy_guarded(load); destroy_guarded(scatter);
    return 0;
}

static int check_float(void) {
    float input[32], output[7], scatter[32];
    for (int i = 0; i < 32; ++i) { input[i] = 0.25F * i; scatter[i] = -1.0F; }
    fixed_float_pos(output, scatter, input, 5);
    for (int lane = 0; lane < 7; ++lane) {
        const int active = lane < 5 && (lane == 0 || lane == 1 || lane == 2);
        const float expected = active ? input[lane * 2] : -9.0F - 0.1F * lane;
        if (output[lane] != expected) return 1 + lane;
        if (active && scatter[lane * 2] != (float)(lane + 1)) return 10 + lane;
    }
    return 0;
}

static int check_segment2(void) {
    struct guarded load = make_guarded(), scatter = make_guarded();
    if (load.mapping == MAP_FAILED || scatter.mapping == MAP_FAILED) return 1;
    const int count = (int)(load.page / (long)sizeof(int));
    for (int i = 0; i < count; ++i) {
        load.middle[i] = 5000 + i;
        scatter.middle[i] = -1;
    }
    int *load_base = load.middle + count - 10;
    int *scatter_base = scatter.middle + count - 10;
    int out0[7] = {0}, out1[7] = {0};
    segment2_i32(out0, out1, scatter_base, load_base, 5);
    for (int lane = 0; lane < 5; ++lane) {
        const int active = lane == 0 || lane == 1 || lane == 2;
        if (!active) continue;
        if (out0[lane] != load_base[2 * lane] ||
            out1[lane] != load_base[2 * lane + 1]) return 10 + lane;
        if (scatter_base[2 * lane] != load_base[2 * lane] ||
            scatter_base[2 * lane + 1] != load_base[2 * lane + 1]) return 20 + lane;
    }
    int snapshot[16];
    memcpy(snapshot, scatter_base - 8, sizeof(snapshot));
    segment2_i32(out0, out1, scatter_base, load.middle + count, 0);
    if (memcmp(snapshot, scatter_base - 8, sizeof(snapshot)) != 0) return 30;
    destroy_guarded(load); destroy_guarded(scatter);
    return 0;
}

static int check_fallbacks(void) {
    int input[64], output[7], scatter[64];
    for (int i = 0; i < 64; ++i) { input[i] = 200 + i; scatter[i] = -1; }
    nonlinear_indexed(output, scatter, input);
    const int nonlinear_indexes[7] = {0, 1, 4, 2, 6, 3, 5};
    for (int lane = 0; lane < 7; ++lane) {
        const int index = nonlinear_indexes[lane];
        if (output[lane] != input[index] || scatter[index] != input[index]) return 1 + lane;
    }
    for (int i = 0; i < 64; ++i) scatter[i] = -1;
    signed_irregular(output, scatter + 16, input + 16);
    const int indexes[7] = {-1, 0, -2, 2, 4, 3, 1};
    for (int lane = 0; lane < 7; ++lane) {
        const int index = indexes[lane];
        if (output[lane] != input[16 + index] || scatter[16 + index] != input[16 + index])
            return 20 + lane;
    }
    return 0;
}

int main(void) {
    int result = check_direction(0); if (result) return 100 + result;
    result = check_direction(1); if (result) return 200 + result;
    result = check_float(); if (result) return 300 + result;
    result = check_segment2(); if (result) return 400 + result;
    result = check_fallbacks(); if (result) return 500 + result;
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


def function_body(assembly: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^{re.escape(name)}:\n(.*?)"
        rf"(?=^\s*\.size\s+{re.escape(name)},|^\s*\.globl\s+|\Z)",
        assembly,
    )
    if match is None:
        raise RuntimeError(f"assembly lacks function {name}")
    return match.group(1)


def main() -> int:
    try:
        cxx = find_cxx()
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="rvv-strided-memory-infra-") as temp:
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
            assembly_text = assembly.read_text(encoding="utf-8")
            for name in ("fixed_pos", "fixed_neg", "fixed_float_pos"):
                body = function_body(assembly_text, name)
                if "vlse32.v" not in body or "vsse32.v" not in body:
                    raise RuntimeError(f"{name} did not select vlse32/vsse32")
                if "vloxei32.v" in body or "vsoxei32.v" in body or "vfirst.m" in body:
                    raise RuntimeError(f"{name} retained indexed/scalar fallback")
                strided_lines = [line for line in body.splitlines()
                                 if "vlse32.v" in line or "vsse32.v" in line]
                if not strided_lines or any("v0.t" not in line for line in strided_lines):
                    raise RuntimeError(f"{name} lost its explicit execution mask")
            nonlinear = function_body(assembly_text, "nonlinear_indexed")
            if "vloxei32.v" not in nonlinear or "vsoxei32.v" not in nonlinear:
                raise RuntimeError(
                    "non-affine index did not preserve ordered indexed fallback:\n" + nonlinear
                )
            if "vlse32.v" in nonlinear or "vsse32.v" in nonlinear:
                raise RuntimeError("non-affine index was misclassified as affine")
            irregular = function_body(assembly_text, "signed_irregular")
            if "vfirst.m" not in irregular:
                raise RuntimeError("signed irregular indexes did not preserve scalar fallback")
            if "vlse32.v" in irregular or "vsse32.v" in irregular:
                raise RuntimeError("signed irregular indexes were misclassified as affine")
            segment = function_body(assembly_text, "segment2_i32")
            if "vlseg2e32.v v24" not in segment or "vsseg2e32.v v24" not in segment:
                raise RuntimeError("eligible adjacent fields did not select a physical 2-field tuple")
            if "vlse32.v" in segment or "vsse32.v" in segment or "vloxei32.v" in segment:
                raise RuntimeError("selected segment pair retained per-field memory operations")
            segment_lines = [line for line in segment.splitlines()
                             if "seg2e32.v" in line]
            if len(segment_lines) != 2 or any("v0.t" not in line for line in segment_lines):
                raise RuntimeError("segment selection lost mask or emitted the wrong field count")
            if "PseudoV" in assembly_text:
                raise RuntimeError("unexpanded RVV pseudo reached assembly")
            run_checked([assembler, "-march=rv64gcv", "-mabi=lp64d",
                         str(assembly), "-o", str(obj)])
            disassembly = run_checked([objdump, "-dr", str(obj)]).stdout.decode()
            for mnemonic in (
                "vlse32.v", "vsse32.v", "vloxei32.v", "vsoxei32.v",
                "vlseg2e32.v", "vsseg2e32.v",
            ):
                if mnemonic not in disassembly:
                    raise RuntimeError(f"object disassembly lacks {mnemonic}")
            run_checked([
                gcc, "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                "-march=rv64gcv", "-mabi=lp64d", str(obj), str(driver),
                "-o", str(executable),
            ])
            for vlen in VLENS:
                try:
                    run_checked([
                        qemu, "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                        str(executable),
                    ], timeout=60.0)
                except RuntimeError as error:
                    raise RuntimeError(
                        f"{error}\nfixed_pos:\n{function_body(assembly_text, 'fixed_pos')}\n"
                        f"fixed_neg:\n{function_body(assembly_text, 'fixed_neg')}"
                    ) from error
                print(f"PASS strided_memory_qemu_vlen_{vlen}")
            print("PASS positive_negative_float_mask_evl_guard_pages")
            print("PASS nonlinear_indexed_and_signed_irregular_fail_closed")
            print("PASS segment2_tuple_continuity_mask_guard_and_verifier_rejection")
            print("PASS strided_final_mir_descriptor_gnu_as_objdump")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_strided_memory_backend: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
