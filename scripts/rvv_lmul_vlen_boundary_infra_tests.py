#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
VLENS = (128, 256, 512, 1024)
SEW = 32


SELECTED_SOURCE = r"""
void kernel_m8(int values[], int n, int bias) {
  int i = 0;
  while (i < n) {
    values[i] = bias;
    i = i + 1;
  }
}

void kernel_m4(int values[], int n, int bias) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + bias;
    i = i + 1;
  }
}

void kernel_m2(int values[], int n, int bias) {
  int i = 0;
  while (i < n) {
    int x = values[i];
    x = x * 3 + bias;
    x = x ^ 1431655765;
    x = x * 5 - bias;
    x = x ^ 858993459;
    x = x * 7 + bias;
    values[i] = x;
    i = i + 1;
  }
}

void kernel_m1(int values[], int n, int bias) {
  int i = 0;
  while (i < n) {
    int x = values[i];
    x = x * 3 + bias; x = x ^ 1431655765;
    x = x * 5 - bias; x = x ^ 858993459;
    x = x * 7 + bias; x = x ^ 252645135;
    x = x * 9 - bias; x = x ^ 16711935;
    x = x * 11 + bias; x = x ^ 65535;
    x = x * 13 - bias; x = x ^ 305419896;
    x = x * 15 + bias; x = x ^ 324508639;
    x = x * 17 - bias; x = x ^ 610839776;
    x = x * 19 + bias; x = x ^ 195948557;
    x = x * 21 - bias; x = x ^ 19088743;
    x = x * 23 + bias; x = x ^ 826366246;
    values[i] = x;
    i = i + 1;
  }
}
"""


# MF2 is deliberately forced rather than advertised as a natural cost-model
# choice.  For an unknown dynamic trip count, e32/m1 has the same one-register
# pressure as e32/mf2 and twice its minimum lane count, so mf2 is dominated.
# The real compiler still constructs and verifies this complete dynamic VLA
# loop before the test narrows its scalable type from 16 lanes (selected m4)
# to 2 lanes (forced mf2) and sends it through the production backend.
FORCED_MF2_SOURCE = r"""
void kernel_mf2(int values[], int n, int bias) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + bias;
    i = i + 1;
  }
}
"""


HARNESS = r"""
#define _GNU_SOURCE 1
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef FORCED_MF2
extern void kernel_mf2(int32_t *values, int32_t n, int32_t bias);
#else
extern void kernel_m1(int32_t *values, int32_t n, int32_t bias);
extern void kernel_m2(int32_t *values, int32_t n, int32_t bias);
extern void kernel_m4(int32_t *values, int32_t n, int32_t bias);
extern void kernel_m8(int32_t *values, int32_t n, int32_t bias);
#endif

static int parse_nonnegative(const char *text, uint32_t *result) {
  uint32_t value = 0;
  if (text == 0 || *text == '\0') return 0;
  while (*text != '\0') {
    const unsigned digit = (unsigned)(*text - '0');
    if (digit > 9U || value > (UINT32_MAX - digit) / 10U) return 0;
    value = value * 10U + digit;
    ++text;
  }
  *result = value;
  return 1;
}

static uint32_t oracle(uint32_t kind, uint32_t x, uint32_t bias) {
#ifdef FORCED_MF2
  (void)kind;
  return x + bias;
#else
  if (kind == 4U) return bias;
  if (kind == 3U) return x + bias;
  if (kind == 2U) {
    x = x * 3U + bias; x ^= UINT32_C(1431655765);
    x = x * 5U - bias; x ^= UINT32_C(858993459);
    return x * 7U + bias;
  }
  if (kind == 1U) {
    x = x * 3U + bias; x ^= UINT32_C(1431655765);
    x = x * 5U - bias; x ^= UINT32_C(858993459);
    x = x * 7U + bias; x ^= UINT32_C(252645135);
    x = x * 9U - bias; x ^= UINT32_C(16711935);
    x = x * 11U + bias; x ^= UINT32_C(65535);
    x = x * 13U - bias; x ^= UINT32_C(305419896);
    x = x * 15U + bias; x ^= UINT32_C(324508639);
    x = x * 17U - bias; x ^= UINT32_C(610839776);
    x = x * 19U + bias; x ^= UINT32_C(195948557);
    x = x * 21U - bias; x ^= UINT32_C(19088743);
    x = x * 23U + bias; x ^= UINT32_C(826366246);
    return x;
  }
  return UINT32_C(0xdeadbeef);
#endif
}

static void invoke_kernel(uint32_t kind, uint8_t *bytes, uint32_t n,
                          int32_t bias) {
  int32_t *values = (int32_t *)(void *)bytes;
#ifdef FORCED_MF2
  (void)kind;
  kernel_mf2(values, (int32_t)n, bias);
#else
  if (kind == 1U) kernel_m1(values, (int32_t)n, bias);
  else if (kind == 2U) kernel_m2(values, (int32_t)n, bias);
  else if (kind == 3U) kernel_m4(values, (int32_t)n, bias);
  else if (kind == 4U) kernel_m8(values, (int32_t)n, bias);
#endif
}

int main(int argc, char **argv) {
  uint32_t kind = 0, n = 0, byte_offset = 0;
  if (argc != 4 || !parse_nonnegative(argv[1], &kind) ||
      !parse_nonnegative(argv[2], &n) ||
      !parse_nonnegative(argv[3], &byte_offset)) return 10;
#ifdef FORCED_MF2
  if (kind != 0U) return 11;
#else
  if (kind < 1U || kind > 4U) return 12;
#endif
  if (byte_offset > 1U) return 13;

  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return 14;
  const size_t bytes = (size_t)n * sizeof(uint32_t);
  if (bytes + byte_offset > (size_t)page) return 15;
  uint8_t *mapping = (uint8_t *)mmap(
      0, (size_t)page * 3U, PROT_NONE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 16;
  uint8_t *writable = mapping + page;
  if (mprotect(writable, (size_t)page, PROT_READ | PROT_WRITE) != 0) return 17;
  memset(writable, 0xa5, (size_t)page);

  uint8_t *logical_end = writable + page - byte_offset;
  uint8_t *values = logical_end - bytes;
  const uint32_t bias = UINT32_C(1234567) + n * 17U + kind * 101U;
  for (uint32_t lane = 0; lane < n; ++lane) {
    const uint32_t value = lane * UINT32_C(2654435761) ^
                           n * UINT32_C(2246822519) ^
                           kind * UINT32_C(3266489917);
    memcpy(values + (size_t)lane * 4U, &value, sizeof(value));
  }

  invoke_kernel(kind, values, n, (int32_t)bias);

  for (uint32_t lane = 0; lane < n; ++lane) {
    uint32_t actual = 0;
    const uint32_t initial = lane * UINT32_C(2654435761) ^
                             n * UINT32_C(2246822519) ^
                             kind * UINT32_C(3266489917);
    memcpy(&actual, values + (size_t)lane * 4U, sizeof(actual));
    if (actual != oracle(kind, initial, bias)) return 30;
  }
  for (uint8_t *cursor = writable; cursor < values; ++cursor)
    if (*cursor != UINT8_C(0xa5)) return 31;
  for (uint8_t *cursor = logical_end; cursor < writable + page; ++cursor)
    if (*cursor != UINT8_C(0xa5)) return 32;
  return munmap(mapping, (size_t)page * 3U) == 0 ? 0 : 33;
}
"""


OIR_TO_ASM_HELPER = r"""
#include "mir/AsmPrinter.h"
#include "mir/MIRVerifier.h"
#include "mir/MachineInstrDesc.h"
#include "oir/OIRParser.h"
#include "pass/PassManager.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/oir/OIRToMIRCommon.h"
#include "target/TargetMachine.h"

#include <fstream>
#include <iostream>
#include <iterator>
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
    result.march_explicit = true;
    std::string error;
    REQUIRE(target::finalize_target_profile(result, error));
    return result;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    std::ifstream input(argv[1]);
    REQUIRE(input.good());
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    auto parsed = oir::OIRParser::parse(source, argv[1]);
    if (!parsed.ok() || parsed.module == nullptr) {
        std::ostringstream error;
        error << "forced OIR failed to parse";
        for (const auto &item : parsed.errors)
            error << "\n" << item.range.begin.line << ':'
                  << item.range.begin.column << ": " << item.message;
        throw std::runtime_error(error.str());
    }
    std::string verify_error;
    REQUIRE(parsed.module->verify(&verify_error));

    const auto target = profile();
    auto machine = pass::oir_to_mir::lower_with_vregs(*parsed.module, target);
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
    std::string expansion_error;
    REQUIRE(pass::expand_machine_pseudos(*machine, expansion_error));
    const auto final = mir::verify_module(*machine, mir::MIRVerificationStage::Final);
    if (!final.ok) throw std::runtime_error(final.message);
    for (const auto &function : machine->functions())
        for (const auto &block : function->blocks())
            for (const auto &instruction : block->instructions())
                REQUIRE(!mir::instruction_desc(instruction.opcode()).has_flag(
                    mir::MIF_Pseudo));

    std::ofstream output(argv[2]);
    REQUIRE(output.good());
    mir::AsmPrinter(output).print(*machine);
    output.close();
    REQUIRE(output.good());
    return 0;
}
"""


HELPER_SOURCES = (
    "src/oir/OIR.cpp",
    "src/oir/OIRAnalysis.cpp",
    "src/oir/OIRDataLayout.cpp",
    "src/oir/OIRParser.cpp",
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


@dataclass(frozen=True)
class LMUL:
    name: str
    numerator: int
    denominator: int
    kind: int
    selected_lanes: int


LMULS = (
    LMUL("mf2", 1, 2, 0, 2),
    LMUL("m1", 1, 1, 1, 4),
    LMUL("m2", 2, 1, 2, 8),
    LMUL("m4", 4, 1, 3, 16),
    LMUL("m8", 8, 1, 4, 32),
)
SELECTED = {item.name: item for item in LMULS if item.name != "mf2"}


@dataclass(frozen=True)
class ProfileDecision:
    legal: bool
    vlmax: int = 0
    rejection: str = ""


class GateFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateFailure(message)


def tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise FileNotFoundError(name)
    return path


def run(
    command: list[str], *, timeout: float = 180.0
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise GateFailure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def profile_decision(vlen: int, lmul: LMUL) -> ProfileDecision:
    if vlen < 128:
        return ProfileDecision(False, rejection="runtime VLEN is below RVV 1.0 V minimum 128")
    if vlen > 65536 or vlen & (vlen - 1):
        return ProfileDecision(False, rejection="runtime VLEN is not a supported power of two")
    if SEW * lmul.denominator > 64 * lmul.numerator:
        return ProfileDecision(False, rejection="e32 LMUL requires an unsupported mask ratio")
    vlmax = (vlen * lmul.numerator) // (SEW * lmul.denominator)
    if vlmax < 1:
        return ProfileDecision(False, rejection="profile cannot hold one active e32 lane")
    return ProfileDecision(True, vlmax=vlmax)


def verify_host_profile_model() -> None:
    for lmul in LMULS:
        for vlen in VLENS:
            decision = profile_decision(vlen, lmul)
            require(
                decision.legal,
                f"required {lmul.name}/VLEN={vlen} rejected: {decision.rejection}",
            )
            expected = (vlen * lmul.numerator) // (SEW * lmul.denominator)
            require(decision.vlmax == expected, "host VLMAX model is inconsistent")

    mf4 = LMUL("mf4", 1, 4, -1, 0)
    forbidden_fractional = profile_decision(128, mf4)
    require(
        not forbidden_fractional.legal
        and "mask ratio" in forbidden_fractional.rejection,
        "e32/mf4 did not become an explicit host-model rejection",
    )
    too_small = profile_decision(64, SELECTED["m1"])
    require(
        not too_small.legal and "minimum 128" in too_small.rejection,
        "VLEN below the architectural minimum did not reject explicitly",
    )
    non_power = profile_decision(192, SELECTED["m1"])
    require(
        not non_power.legal and "power of two" in non_power.rejection,
        "non-power-of-two VLEN did not reject explicitly",
    )
    print("PASS rvv_lmul_host_model_legal_matrix_and_explicit_rejections")


def vectorized_choice(document: str, function: str) -> dict[str, object]:
    plans = json.loads(document).get("vectorization_plans", [])
    matches = [
        entry
        for entry in plans
        if entry.get("vectorizer") == "loop"
        and entry.get("code") == "VECTORIZED"
        and entry.get("function") == function
    ]
    require(len(matches) == 1, f"{function} needs exactly one verified loop plan")
    plan = matches[0].get("plan")
    require(isinstance(plan, dict), f"{function} vectorized remark has no plan")
    return plan


def verify_selected_plans(source: Path) -> None:
    result = run(
        [
            str(COMPILER),
            str(source),
            "--emit-vector-plan",
            "-O2",
            "-march=rv64gcv",
        ]
    )
    for name, lmul in SELECTED.items():
        plan = vectorized_choice(result.stdout, f"kernel_{name}")
        require(plan.get("scalable") is True, f"kernel_{name} is not scalable")
        require(plan.get("lmul") == name, f"kernel_{name} did not select {name}")
        require(
            plan.get("minimum_lanes") == lmul.selected_lanes,
            f"kernel_{name} has the wrong minimum lane count",
        )
        require(plan.get("interleave") == 1, f"kernel_{name} is not one VLA chunk")
        require(plan.get("uses_mask") is True, f"kernel_{name} lost tail masking")
    print("PASS rvv_lmul_m1_m2_m4_m8_real_compiler_selected_plans")


def compile_forced_mf2(
    source: Path, directory: Path, cxx: str
) -> Path:
    plan_result = run(
        [
            str(COMPILER),
            str(source),
            "--emit-vector-plan",
            "-O2",
            "-march=rv64gcv",
        ]
    )
    baseline = vectorized_choice(plan_result.stdout, "kernel_mf2")
    require(
        baseline.get("scalable") is True
        and baseline.get("lmul") == "m4"
        and baseline.get("minimum_lanes") == 16,
        "MF2 force fixture no longer starts from its verified dynamic m4 VLA plan",
    )
    oir_text = run(
        [
            str(COMPILER),
            str(source),
            "--emit-oir",
            "-O2",
            "-march=rv64gcv",
        ]
    ).stdout
    require(oir_text.count("define void @kernel_mf2") == 1, "forced OIR lost kernel_mf2")
    require(" = setvl <vscale x 16 x i32>" in oir_text, "forced OIR lost dynamic setvl")
    require("vp.load <vscale x 16 x i32>" in oir_text, "forced OIR lost VP load")
    require("vp.store <vscale x 16 x i32>" in oir_text, "forced OIR lost VP store")
    replacements = oir_text.count("<vscale x 16 x")
    require(replacements >= 8, "forced OIR has too few scalable type sites")
    forced_text = oir_text.replace("<vscale x 16 x", "<vscale x 2 x")
    require("<vscale x 16 x" not in forced_text, "forced OIR retained an m4 type")
    require(" = setvl <vscale x 2 x i32>" in forced_text, "MF2 force lost setvl")

    forced_oir = directory / "forced_mf2.oir"
    helper_source = directory / "oir_to_asm.cpp"
    helper = directory / "oir_to_asm"
    assembly = directory / "forced_mf2.s"
    forced_oir.write_text(forced_text, encoding="utf-8")
    helper_source.write_text(textwrap.dedent(OIR_TO_ASM_HELPER), encoding="utf-8")
    run(
        [
            cxx,
            "-std=c++17",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include"),
            str(helper_source),
            *(str(ROOT / item) for item in HELPER_SOURCES),
            "-o",
            str(helper),
        ],
        timeout=300.0,
    )
    run([str(helper), str(forced_oir), str(assembly)])
    print("PASS rvv_lmul_mf2_forced_verified_dynamic_oir_to_final_backend")
    return assembly


def assembly_function(text: str, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}:$\n(?P<body>.*?)(?=^\s*\.size\s+{re.escape(name)},)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    require(match is not None, f"assembly lacks function {name}")
    return match.group("body")


def objdump_function(text: str, name: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+\s+<{re.escape(name)}>:\n(?P<body>.*?)(?=^[0-9a-f]+\s+<(?!\.L)[^>]+>:\n|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    require(match is not None, f"objdump lacks function {name}")
    return match.group("body")


def verify_function_lmul(text: str, name: str, lmul: str, *, objdump: bool) -> None:
    body = objdump_function(text, name) if objdump else assembly_function(text, name)
    states = re.findall(r"\bvset(?:i)?vli\b[^\n]*\be32,\s*(mf2|m1|m2|m4|m8)\b", body)
    require(states, f"{name} has no decoded e32 vector state")
    require(set(states) == {lmul}, f"{name} has vector states {states}, expected only {lmul}")
    for opcode in ("vle32.v", "vse32.v"):
        require(opcode in body or name == "kernel_m8" and opcode == "vle32.v", f"{name} lacks {opcode}")


def verify_zero_pseudo(assembly: str, label: str) -> None:
    for marker in ("Pseudo", "RVVMask", "RVVVector", "%v"):
        require(marker not in assembly, f"{label} final assembly leaked pseudo marker {marker}")


def assemble_link_and_audit(
    *,
    directory: Path,
    stem: str,
    assembly: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    forced_mf2: bool,
) -> Path:
    object_file = directory / f"{stem}.o"
    harness_source = directory / f"{stem}_harness.c"
    harness_object = directory / f"{stem}_harness.o"
    executable = directory / f"{stem}.exe"
    harness_source.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
    run(
        [
            assembler,
            "-march=rv64gcv",
            "-mabi=lp64d",
            str(assembly),
            "-o",
            str(object_file),
        ]
    )
    harness_command = [
        gcc,
        "-std=c11",
        "-O2",
        "-fno-tree-vectorize",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-march=rv64gc",
        "-mabi=lp64d",
    ]
    if forced_mf2:
        harness_command.append("-DFORCED_MF2=1")
    harness_command.extend(["-c", str(harness_source), "-o", str(harness_object)])
    run(harness_command)

    harness_disassembly = run([objdump, "-d", str(harness_object)]).stdout
    harness_mnemonics = re.findall(
        r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]+)\b",
        harness_disassembly,
    )
    harness_vectors = [mnemonic for mnemonic in harness_mnemonics if mnemonic.startswith("v")]
    require(
        not harness_vectors,
        f"scalar oracle harness unexpectedly contains RVV: {harness_vectors}",
    )
    run(
        [
            gcc,
            "-static",
            "-march=rv64gcv",
            "-mabi=lp64d",
            str(object_file),
            str(harness_object),
            "-o",
            str(executable),
        ]
    )
    assembly_text = assembly.read_text(encoding="utf-8")
    verify_zero_pseudo(assembly_text, stem)
    object_disassembly = run([objdump, "-dr", str(object_file)]).stdout
    executable_disassembly = run([objdump, "-d", str(executable)]).stdout
    targets = {"mf2": LMULS[0]} if forced_mf2 else SELECTED
    for name in targets:
        symbol = f"kernel_{name}"
        verify_function_lmul(assembly_text, symbol, name, objdump=False)
        verify_function_lmul(object_disassembly, symbol, name, objdump=True)
        verify_function_lmul(executable_disassembly, symbol, name, objdump=True)
    print(f"PASS rvv_lmul_{stem}_gnu_as_link_objdump_and_final_zero_pseudo")
    return executable


def boundary_lengths(vlmax: int, vlen: int, lmul: LMUL) -> list[tuple[str, int]]:
    random_large = 521 + ((vlen * 17 + lmul.numerator * 97 + lmul.denominator * 53) % 401)
    return [
        ("zero", 0),
        ("one", 1),
        ("vlmax_minus_one", vlmax - 1),
        ("vlmax", vlmax),
        ("vlmax_plus_one", vlmax + 1),
        ("two_vlmax_minus_one", 2 * vlmax - 1),
        ("two_vlmax", 2 * vlmax),
        ("two_vlmax_plus_one", 2 * vlmax + 1),
        ("deterministic_random_large", random_large),
    ]


def run_matrix(qemu: str, selected: Path, forced: Path) -> None:
    for lmul in LMULS:
        executable = forced if lmul.name == "mf2" else selected
        for vlen in VLENS:
            decision = profile_decision(vlen, lmul)
            require(
                decision.legal,
                f"matrix model rejected {lmul.name}/VLEN={vlen}: {decision.rejection}",
            )
            seen_labels: set[str] = set()
            for label, length in boundary_lengths(decision.vlmax, vlen, lmul):
                require(label not in seen_labels, f"duplicate boundary label {label}")
                seen_labels.add(label)
                require(0 <= length <= 1023, f"{label} length does not fit guarded page")
                for byte_offset in (0, 1):
                    run(
                        [
                            qemu,
                            "-cpu",
                            f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                            str(executable),
                            str(lmul.kind),
                            str(length),
                            str(byte_offset),
                        ],
                        timeout=30.0,
                    )
            require(
                seen_labels
                == {
                    "zero",
                    "one",
                    "vlmax_minus_one",
                    "vlmax",
                    "vlmax_plus_one",
                    "two_vlmax_minus_one",
                    "two_vlmax",
                    "two_vlmax_plus_one",
                    "deterministic_random_large",
                },
                "boundary label matrix is incomplete",
            )
            print(
                f"PASS rvv_lmul_{lmul.name}_vlen_{vlen}_"
                "vlmax_boundaries_aligned_unaligned_tail_guard_scalar_oracle"
            )


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        assembler = tool("riscv64-linux-gnu-as")
        gcc = tool("riscv64-linux-gnu-gcc")
        objdump = tool("riscv64-linux-gnu-objdump")
        qemu = tool("qemu-riscv64")
        cxx = os.environ.get("CXX") or tool("c++")
    except FileNotFoundError as error:
        message = f"RVV LMUL/VLEN matrix tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77
    except GateFailure as error:
        print(f"FAIL rvv_lmul_vlen_boundary: {error}", file=sys.stderr)
        return 1

    try:
        verify_host_profile_model()
        with tempfile.TemporaryDirectory(prefix="yoolang-rvv-lmul-vlen-") as tmp:
            directory = Path(tmp)
            selected_source = directory / "selected.sy"
            selected_assembly = directory / "selected.s"
            forced_source = directory / "forced_mf2.sy"
            selected_source.write_text(textwrap.dedent(SELECTED_SOURCE), encoding="utf-8")
            forced_source.write_text(textwrap.dedent(FORCED_MF2_SOURCE), encoding="utf-8")
            verify_selected_plans(selected_source)
            run(
                [
                    str(COMPILER),
                    str(selected_source),
                    "-S",
                    "-O2",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    "-o",
                    str(selected_assembly),
                ]
            )
            forced_assembly = compile_forced_mf2(forced_source, directory, cxx)
            selected_executable = assemble_link_and_audit(
                directory=directory,
                stem="selected",
                assembly=selected_assembly,
                assembler=assembler,
                gcc=gcc,
                objdump=objdump,
                forced_mf2=False,
            )
            forced_executable = assemble_link_and_audit(
                directory=directory,
                stem="forced_mf2",
                assembly=forced_assembly,
                assembler=assembler,
                gcc=gcc,
                objdump=objdump,
                forced_mf2=True,
            )
            run_matrix(qemu, selected_executable, forced_executable)
    except (
        GateFailure,
        json.JSONDecodeError,
        OSError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"FAIL rvv_lmul_vlen_boundary: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
