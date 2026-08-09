#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
VLENS = (128, 256, 512, 1024)


PROBE_SOURCE = r"""
#include "mir/AsmPrinter.h"
#include "mir/MIRVerifier.h"
#include "oir/OIR.h"
#include "pass/PassManager.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/oir/OIRToMIRCommon.h"
#include "target/TargetMachine.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string(__FILE__) + ":" +                            \
                                     std::to_string(__LINE__) +                                 \
                                     ": requirement failed: " #condition);                     \
        }                                                                                       \
    } while (false)

target::TargetProfile staged_profile() {
    target::TargetProfile profile;
    profile.march = "rv64gcv_zvl128b";
    std::string error;
    REQUIRE(target::parse_rvv_deployment("compile-time", profile, error));
    REQUIRE(target::parse_vector_bits("128", profile, error));
    // Finalize the target capabilities while the public vector ABI remains
    // standard, then opt the programmatic lowering probe into the staged ABI.
    REQUIRE(target::finalize_target_profile(profile, error));
    profile.vector_abi = target::VectorABI::PsABIVector;
    return profile;
}

std::unique_ptr<oir::Module> make_module() {
    auto module = std::make_unique<oir::Module>("psabi-runtime-staged");
    auto &types = module->types();
    auto *putint = module->create_function(
        "putint", types.func_ty(types.void_ty(), {types.int32_ty()}), true);
    auto *getint = module->create_function(
        "getint", types.func_ty(types.int32_ty(), {}), true);
    auto *main = module->create_function(
        "main", types.func_ty(types.int32_ty(), {}));
    auto *entry = main->create_block("entry");
    oir::IRBuilder builder(module.get());
    builder.set_insert_point(entry);
    auto *v4i = types.fixed_vector_ty(types.int32_ty(), 4);
    auto *ten = builder.create_splat(v4i, builder.i32(10), "ten");
    auto *two = builder.create_splat(v4i, builder.i32(2), "two");
    auto *live = builder.create_binary(oir::Instruction::OpID::Add,
                                       ten, two, "live.across.runtime");
    builder.create_call(putint, types.void_ty(), {builder.i32(7)});
    auto *input = builder.create_call(getint, types.int32_ty(), {}, "input");
    auto *lane = builder.create_extract_element(live, builder.i32(0), "lane");
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add,
                                      lane, input, "sum");
    auto *status = builder.create_binary(oir::Instruction::OpID::Sub,
                                         sum, builder.i32(15), "status");
    builder.create_ret(status);
    std::string verify_error;
    REQUIRE(module->verify(&verify_error));
    return module;
}

void require_unknown_external_fails_closed(const target::TargetProfile &profile) {
    oir::Module module("unknown-external");
    auto &types = module.types();
    module.create_function("foreign",
                           types.func_ty(types.int32_ty(), {types.int32_ty()}),
                           true);
    auto *main = module.create_function("main", types.func_ty(types.int32_ty(), {}));
    auto *entry = main->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(builder.i32(0));
    try {
        (void)pass::oir_to_mir::lower_with_vregs(module, profile);
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string(error.what()).find(
                    "PSABI_VECTOR_EXTERNAL_CC_UNSPECIFIED") != std::string::npos);
        return;
    }
    throw std::runtime_error("unknown external ABI ownership was guessed");
}

mir::MachineFunction *find_function(mir::Module &module, const std::string &name) {
    for (const auto &function : module.functions()) {
        if (function->name() == name) {
            return function.get();
        }
    }
    return nullptr;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    const auto profile = staged_profile();
    require_unknown_external_fails_closed(profile);
    auto oir_module = make_module();
    auto machine = pass::oir_to_mir::lower_with_vregs(*oir_module, profile);
    auto &target = machine->target();
    target.arch = profile.march;
    target.abi = profile.mabi;
    target.has_vector = true;
    target.minimum_vlen_bits = 128;
    target.abi_vlen_bits = 128;
    target.psabi_vector = true;

    auto *main_function = find_function(*machine, "main");
    auto *putint = find_function(*machine, "putint");
    auto *getint = find_function(*machine, "getint");
    REQUIRE(main_function != nullptr && main_function->is_variant_cc());
    REQUIRE(putint != nullptr && !putint->is_variant_cc());
    REQUIRE(getint != nullptr && !getint->is_variant_cc());
    unsigned standard_runtime_calls = 0;
    for (const auto &block : main_function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (instruction.opcode() != mir::Opcode::Call) {
                continue;
            }
            REQUIRE(!instruction.is_variant_cc_call());
            const auto &symbol = instruction.operands().front().string_value();
            REQUIRE(symbol == "putint" || symbol == "getint");
            ++standard_runtime_calls;
        }
    }
    REQUIRE(standard_runtime_calls == 2);
    auto pre = mir::verify_module(*machine, mir::MIRVerificationStage::PreRA);
    REQUIRE(pre.ok);

    pass::PassContext context;
    context.set_machine_module(std::move(machine));
    pass::MIRRegAllocPass allocation;
    const auto allocated = allocation.run(context);
    if (!allocated.success) {
        throw std::runtime_error(allocated.message);
    }
    machine = context.take_machine_module();
    main_function = find_function(*machine, "main");
    bool saw_spill = false;
    bool saw_reload = false;
    for (const auto &block : main_function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            saw_spill = saw_spill ||
                        instruction.opcode() == mir::Opcode::RVVWholeRegSpill;
            saw_reload = saw_reload ||
                         instruction.opcode() == mir::Opcode::RVVWholeRegReload;
        }
    }
    REQUIRE(saw_spill && saw_reload);

    std::string expansion_error;
    REQUIRE(pass::expand_machine_pseudos(*machine, expansion_error));
    const auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
    if (!final.ok) {
        throw std::runtime_error(final.message);
    }
    std::ostringstream assembly;
    mir::AsmPrinter(assembly).print(*machine);
    const auto text = assembly.str();
    REQUIRE(text.find("\t.variant_cc main\n") != std::string::npos);
    REQUIRE(text.find("\t.variant_cc putint\n") == std::string::npos);
    REQUIRE(text.find("\t.variant_cc getint\n") == std::string::npos);
    REQUIRE(text.find("vs1r.v") != std::string::npos);
    REQUIRE(text.find("vl1re8.v") != std::string::npos);
    std::ofstream output(argv[1]);
    output << text;
    REQUIRE(output.good());
    std::cout << "PASS staged_runtime_identity_spill_and_variant_metadata\n";
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
        raise RuntimeError(f"required staged psABI tool not found: {name}")
    return path


def checked(command: list[str], *, input_bytes: bytes | None = None,
            timeout: float = 120.0) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=input_bytes,
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


def symbol_line(table: str, name: str) -> str:
    lines = [line for line in table.splitlines() if line.split()[-1:] == [name]]
    if len(lines) != 1:
        raise RuntimeError(f"expected one symbol {name}, got {lines}")
    return lines[0]


def main() -> int:
    cxx = os.environ.get("CXX") or require_tool("c++")
    assembler = require_tool("riscv64-linux-gnu-as")
    readelf = require_tool("riscv64-linux-gnu-readelf")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    qemu = require_tool("qemu-riscv64")
    runtime = ROOT / "runtime/libsysy_riscv.a"
    if not runtime.is_file():
        raise RuntimeError(f"required runtime archive not found: {runtime}")

    with tempfile.TemporaryDirectory(prefix="yoolang-psabi-staged-runtime-") as directory:
        work = Path(directory)
        source = work / "probe.cpp"
        probe = work / "probe"
        assembly = work / "runtime_live.s"
        object_file = work / "runtime_live.o"
        executable = work / "runtime_live"
        source.write_text(textwrap.dedent(PROBE_SOURCE), encoding="utf-8")
        checked([
            cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "include"), str(source),
            *(str(ROOT / item) for item in PROBE_SOURCES),
            "-o", str(probe),
        ])
        output = checked([str(probe), str(assembly)]).stdout.decode()
        if "PASS staged_runtime_identity_spill_and_variant_metadata" not in output:
            raise RuntimeError("programmatic staged-runtime probe did not pass")
        checked([assembler, "-march=rv64gcv_zvl128b", "-mabi=lp64d",
                 str(assembly), "-o", str(object_file)])
        symbols = checked([readelf, "-Ws", str(object_file)]).stdout.decode()
        if "[VARIANT_CC]" not in symbol_line(symbols, "main"):
            raise RuntimeError("variant main definition lost STO_RISCV_VARIANT_CC")
        for name in ("putint", "getint"):
            line = symbol_line(symbols, name)
            if "UND" not in line or "[VARIANT_CC]" in line:
                raise RuntimeError(f"standard runtime reference was variant-marked: {line}")
        disassembly = checked([objdump, "-dr", str(object_file)]).stdout.decode()
        if "vs1r.v" not in disassembly or "vl1re" not in disassembly:
            raise RuntimeError("runtime-live object lacks whole-register spill/reload")
        checked([gcc, "-static", "-march=rv64gcv_zvl128b", "-mabi=lp64d",
                 str(object_file), str(runtime), "-o", str(executable)])
        for vlen in VLENS:
            result = checked(
                [qemu, "-cpu",
                 f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                 str(executable)],
                input_bytes=b"3\n",
            )
            if result.stdout != b"7":
                raise RuntimeError(
                    f"unexpected VLEN {vlen} stdout: {result.stdout!r}"
                )
    print("PASS staged_vector_live_across_putint_getint_qemu_all_vlen")
    print("PASS standard_runtime_undefined_symbols_have_zero_variant_cc")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL psabi_vector_staged_runtime: {error}")
        raise SystemExit(1)
