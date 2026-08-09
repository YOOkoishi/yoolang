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
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) throw std::runtime_error(#condition);                   \
    } while (false)

target::TargetProfile profile() {
    target::TargetProfile result;
    result.march = "rv64gcv";
    std::string error;
    REQUIRE(target::finalize_target_profile(result, error));
    return result;
}

std::unique_ptr<oir::Module> make_module(const std::uint64_t maximum_lanes) {
    auto module = std::make_unique<oir::Module>("scalable-lmul-contract");
    auto &types = module->types();
    auto *i32 = types.int32_ty();
    auto *pointer = types.ptr_ty(i32);
    for (std::uint64_t lanes : {1U, 2U, 4U, 8U, 16U, 32U, 64U}) {
        if (lanes > maximum_lanes) continue;
        auto *vector = types.scalable_vector_ty(i32, lanes);
        auto *mask = types.scalable_vector_ty(types.int1_ty(), lanes);
        auto *function = module->create_function(
            "scalable_" + std::to_string(lanes),
            types.func_ty(i32, {pointer, i32, i32}));
        auto *entry = function->create_block("entry");
        oir::IRBuilder builder(module.get());
        builder.set_insert_point(entry);
        auto *vl = builder.create_set_vl(vector, function->args()[2].get(), "vl");
        auto *value = builder.create_splat(vector, function->args()[1].get(), "value");
        auto *active = builder.create_splat(mask, builder.i1(true), "active");
        builder.create_vp_store(value, function->args()[0].get(), active, vl,
                                oir::TailPolicy::Agnostic,
                                oir::MaskPolicy::Agnostic, 4);
        builder.create_ret(vl);
    }
    std::string error;
    REQUIRE(module->verify(&error));
    return module;
}

std::string lower_to_final_assembly(std::unique_ptr<oir::Module> module) {
    const auto target = profile();
    auto machine = pass::oir_to_mir::lower_with_vregs(*module, target);
    machine->target().arch = target.march;
    machine->target().has_vector = true;
    machine->target().minimum_vlen_bits = target.minimum_vlen_bits;
    const auto pre = mir::verify_module(*machine, mir::MIRVerificationStage::PreRA);
    if (!pre.ok) throw std::runtime_error(pre.message);
    pass::PassContext context;
    context.set_machine_module(std::move(machine));
    pass::MIRRegAllocPass allocate;
    const auto allocated = allocate.run(context);
    if (!allocated.success) throw std::runtime_error(allocated.message);
    machine = context.take_machine_module();
    std::string error;
    REQUIRE(pass::expand_machine_pseudos(*machine, error));
    const auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
    if (!final.ok) throw std::runtime_error(final.message);
    for (const auto &function : machine->functions())
        for (const auto &block : function->blocks())
            for (const auto &instruction : block->instructions())
                REQUIRE(!mir::instruction_desc(instruction.opcode()).has_flag(
                    mir::MIF_Pseudo));
    std::ostringstream out;
    mir::AsmPrinter(out).print(*machine);
    return out.str();
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    const auto assembly = lower_to_final_assembly(make_module(32));
    std::ofstream output(argv[1]);
    REQUIRE(output.good());
    output << assembly;
    output.close();
    REQUIRE(output.good());

    auto oversized = make_module(64);
    try {
        (void)pass::oir_to_mir::lower_with_vregs(*oversized, profile());
        throw std::runtime_error("scalable minimum shape above m8 unexpectedly lowered");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string(error.what()).find("larger than RVV LMUL=m8") !=
                std::string::npos);
    }
    std::cout << "PASS scalable_lmul_capacity_reject\n";
    return 0;
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


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def run(command: list[str], *, timeout: float = 180.0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def function_body(assembly: str, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}:$\n(?P<body>.*?)(?=^\s*\.size\s+{re.escape(name)},)",
        assembly,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"assembly lacks function {name}")
    return match.group("body")


def main() -> int:
    cxx = os.environ.get("CXX") or require_tool("c++")
    assembler = require_tool("riscv64-linux-gnu-as")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    with tempfile.TemporaryDirectory(prefix="yoolang-scalable-lmul-") as tmp:
        directory = Path(tmp)
        source = directory / "probe.cpp"
        binary = directory / "probe"
        assembly = directory / "probe.s"
        object_file = directory / "probe.o"
        source.write_text(textwrap.dedent(PROBE_SOURCE), encoding="utf-8")
        run(
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
                str(binary),
            ]
        )
        output = run([str(binary), str(assembly)]).stdout
        if "PASS scalable_lmul_capacity_reject" not in output:
            raise RuntimeError("probe did not validate the oversized scalable reject")
        text = assembly.read_text(encoding="utf-8")
        expected = {
            1: "mf2",
            2: "mf2",
            4: "m1",
            8: "m2",
            16: "m4",
            32: "m8",
        }
        for lanes, lmul in expected.items():
            body = function_body(text, f"scalable_{lanes}")
            if re.search(rf"\bvsetvli\b[^\n]*\be32,\s*{lmul}\b", body) is None:
                raise RuntimeError(
                    f"scalable minimum lanes={lanes} did not lower with LMUL={lmul}"
                )
        if re.search(r"\be32,\s*mf[48]\b", text) is not None:
            raise RuntimeError("backend accepted forbidden e32 fractional mf4/mf8")
        run([assembler, "-march=rv64gcv", str(assembly), "-o", str(object_file)])
        disassembly = run([objdump, "-dr", str(object_file)]).stdout
        for lmul in ("mf2", "m1", "m2", "m4", "m8"):
            if re.search(rf"vsetvli[^\n]*e32,{lmul}", disassembly) is None:
                raise RuntimeError(f"objdump lacks scalable LMUL {lmul}")
        print("PASS scalable_lmul_mf2_m1_m2_m4_m8_to_final_asm")
        print("PASS scalable_lmul_gnu_as_objdump")
        print("PASS scalable_lmul_illegal_fractional_and_capacity_fail_closed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL scalable_lmul: {error}", file=sys.stderr)
        raise SystemExit(1)
