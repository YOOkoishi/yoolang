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
#include "pass/PassManager.h"
#include "pass/oir/OIRVectorCleanupPass.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition);  \
    } while (false)

unsigned count_op(const oir::Module &module, oir::Instruction::OpID op) {
    unsigned count = 0;
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                count += instruction->op() == op;
            }
        }
    }
    return count;
}

int main() {
    auto module = std::make_unique<oir::Module>("vector_cleanup");
    auto &types = module->types();
    auto *fixed = types.fixed_vector_ty(types.int32_ty(), 3);
    auto *mask_type = types.fixed_vector_ty(types.int1_ty(), 3);
    auto *scalable = types.scalable_vector_ty(types.int32_ty(), 1);
    auto *ptr = types.ptr_ty(types.int32_ty());
    auto *function = module->create_function(
        "cleanup", types.func_ty(types.void_ty(), {ptr}));
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);

    auto *dead_splat = builder.create_splat(fixed, builder.i32(2), "dead.splat");
    auto *dead_binary = builder.create_binary(oir::Instruction::OpID::Add,
                                               dead_splat, dead_splat, "dead.binary");
    builder.create_extract_element(dead_binary, builder.i32(0), "dead.extract");
    builder.create_set_vl(scalable, builder.i32(100), "dead.setvl");

    // This pass is intentionally not a second scalar optimizer.
    builder.create_binary(oir::Instruction::OpID::Add,
                          builder.i32(1), builder.i32(2), "dead.scalar");

    auto *live_splat = builder.create_splat(fixed, builder.i32(9), "live.splat");
    auto *mask = module->create_constant_mask(mask_type, {0x07});
    auto *evl = builder.i32(3);
    auto *passthrough = builder.undef(fixed);
    builder.create_vp_load(fixed, function->args()[0].get(), mask, evl, passthrough,
                           oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 1,
                           "unused.load");
    builder.create_vp_reduction(oir::ReductionKind::Add, false, live_splat, mask, evl,
                                builder.i32(0), oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, "unused.reduction");
    builder.create_vp_store(live_splat, function->args()[0].get(), mask, evl,
                            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic, 1);
    builder.create_ret();

    std::string error;
    REQUIRE(module->verify(&error));
    pass::PassContext context;
    context.set_ssa_module(std::move(module));
    pass::OIRVectorCleanupPass cleanup;
    const auto result = cleanup.run(context);
    REQUIRE(result.success && result.changed);
    REQUIRE(result.message == "dead_vector_instructions=4");
    auto *cleaned = context.ssa_module();
    REQUIRE(cleaned != nullptr && cleaned->verify(&error));
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::ExtractElement) == 0);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::SetVL) == 0);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::Add) == 1);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::Splat) == 1);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::VPLoad) == 1);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::VPReduction) == 1);
    REQUIRE(count_op(*cleaned, oir::Instruction::OpID::VPStore) == 1);
    std::cout << "PASS vector_safe_dead_computation_cleanup\n";

    pass::PassContext empty;
    const auto missing = cleanup.run(empty);
    REQUIRE(!missing.success);
    std::cout << "PASS vector_cleanup_requires_oir\n";
    return 0;
}
"""


def find_cxx() -> str | None:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("FAIL vector_cleanup: no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="vector-cleanup-") as temp:
        directory = Path(temp)
        source = directory / "vector_cleanup.cpp"
        executable = directory / "vector_cleanup"
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
            str(ROOT / "src/pass/PassManager.cpp"),
            str(ROOT / "src/pass/oir/OIRVectorCleanupPass.cpp"),
            "-o",
            str(executable),
        ]
        compiled = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
        if compiled.returncode != 0:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        ran = subprocess.run([str(executable)], cwd=ROOT, text=True, capture_output=True, check=False)
        print(ran.stdout, end="")
        if ran.returncode != 0:
            print(ran.stderr, end="", file=sys.stderr)
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
