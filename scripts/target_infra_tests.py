#!/usr/bin/env python3

from __future__ import annotations

import argparse
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
DEFAULT_COMPILER = ROOT / "build/linux/x86_64/release/compiler"


TARGET_HARNESS = r"""
#include "target/TargetMachine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);         \
        }                                                                                       \
    } while (false)

int main() {
    target::TargetMachine default_machine;
    const auto &default_profile = default_machine.profile();
    REQUIRE(default_profile.triple == "riscv64-unknown-linux-gnu");
    REQUIRE(default_profile.march == "rv64gc");
    REQUIRE(default_profile.mabi == "lp64d");
    REQUIRE(default_profile.features.i);
    REQUIRE(default_profile.features.m);
    REQUIRE(default_profile.features.a);
    REQUIRE(default_profile.features.f);
    REQUIRE(default_profile.features.d);
    REQUIRE(default_profile.features.c);
    REQUIRE(!default_profile.has_vector());
    REQUIRE(default_profile.deployment == target::DeploymentMode::Scalar);
    REQUIRE(!default_profile.deployment_explicit);
    REQUIRE(default_profile.deployment_name() == "scalar");
    REQUIRE(default_machine.data_layout().pointer_size == 8);
    REQUIRE(default_machine.data_layout().stack_alignment == 16);
    REQUIRE(default_profile.tuning.vector_load_cost == 5);
    REQUIRE(target::supported_target_cpu_names() == "generic-rv64, generic-rvv");

    target::TargetProfile cpu_selected_profile;
    cpu_selected_profile.cpu = "generic-rvv";
    cpu_selected_profile.cpu_explicit = true;
    std::string cpu_error;
    REQUIRE(target::finalize_target_profile(cpu_selected_profile, cpu_error));
    REQUIRE(cpu_selected_profile.march == "rv64gcv");
    REQUIRE(cpu_selected_profile.tune == "generic-rvv");
    REQUIRE(cpu_selected_profile.tuning.maximum_interleave_factor == 2);
    REQUIRE(cpu_selected_profile.has_vector());
    REQUIRE(cpu_selected_profile.tuning.vector_load_cost == 3);

    target::TargetProfile unknown_cpu_profile;
    unknown_cpu_profile.cpu = "unknown-rv64";
    unknown_cpu_profile.cpu_explicit = true;
    cpu_error.clear();
    REQUIRE(!target::finalize_target_profile(unknown_cpu_profile, cpu_error));
    REQUIRE(cpu_error.find("supported CPUs are generic-rv64, generic-rvv") !=
            std::string::npos);

    std::string error;
    target::TargetProfile vector_profile;
    vector_profile.march = "rv64gcv_zvl256b";
    REQUIRE(target::finalize_target_profile(vector_profile, error));
    REQUIRE(vector_profile.features.v);
    REQUIRE(vector_profile.features.has("zve64d"));
    REQUIRE(vector_profile.deployment == target::DeploymentMode::CompileTimeVector);
    REQUIRE(!vector_profile.deployment_explicit);
    REQUIRE(vector_profile.deployment_name() == "compile-time");
    REQUIRE(vector_profile.minimum_vlen_bits == 256);
    REQUIRE(vector_profile.supports_vector_element(false, 32));
    REQUIRE(vector_profile.supports_vector_element(true, 32));

    target::TargetProfile zve_profile;
    zve_profile.march = "rv64gc_zve32x";
    error.clear();
    REQUIRE(target::finalize_target_profile(zve_profile, error));
    REQUIRE(zve_profile.has_vector());
    REQUIRE(zve_profile.features.has("zve32x"));
    REQUIRE(zve_profile.minimum_vlen_bits == 32);
    REQUIRE(zve_profile.supports_vector_element(false, 32));
    REQUIRE(!zve_profile.supports_vector_element(true, 32));

    target::TargetProfile explicit_scalar_profile;
    explicit_scalar_profile.march = "rv64gcv";
    REQUIRE(target::parse_rvv_deployment("ScAlAr", explicit_scalar_profile, error));
    error.clear();
    REQUIRE(target::finalize_target_profile(explicit_scalar_profile, error));
    REQUIRE(explicit_scalar_profile.features.has_any_vector());
    REQUIRE(!explicit_scalar_profile.has_vector());
    REQUIRE(!explicit_scalar_profile.supports_vector_element(false, 32));
    REQUIRE(explicit_scalar_profile.march == "rv64gcv");
    REQUIRE(explicit_scalar_profile.deployment_explicit);
    REQUIRE(explicit_scalar_profile.deployment_name() == "scalar");

    target::TargetProfile deployment_roundtrip_profile;
    deployment_roundtrip_profile.march = "rv64gcv";
    REQUIRE(target::parse_rvv_deployment(explicit_scalar_profile.deployment_name(),
                                         deployment_roundtrip_profile, error));
    error.clear();
    REQUIRE(target::finalize_target_profile(deployment_roundtrip_profile, error));
    REQUIRE(deployment_roundtrip_profile.deployment == target::DeploymentMode::Scalar);

    target::TargetProfile explicit_compile_time_profile;
    explicit_compile_time_profile.march = "rv64gcv";
    REQUIRE(target::parse_rvv_deployment("COMPILE-TIME", explicit_compile_time_profile,
                                         error));
    error.clear();
    REQUIRE(target::finalize_target_profile(explicit_compile_time_profile, error));
    REQUIRE(explicit_compile_time_profile.has_vector());
    REQUIRE(explicit_compile_time_profile.deployment_name() == "compile-time");

    target::TargetProfile scalar_compile_time_profile;
    REQUIRE(target::parse_rvv_deployment("compile-time", scalar_compile_time_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(scalar_compile_time_profile, error));
    REQUIRE(error.find("compile-time requires V or Zve") != std::string::npos);

    target::TargetProfile fat_profile;
    REQUIRE(target::parse_rvv_deployment("fat", fat_profile, error));
    REQUIRE(fat_profile.deployment_name() == "fat");
    error.clear();
    REQUIRE(target::finalize_target_profile(fat_profile, error));
    REQUIRE(!fat_profile.has_vector());
    target::TargetProfile fat_scalar_profile;
    target::TargetProfile fat_vector_profile;
    REQUIRE(target::make_rvv_multiversion_profiles(
        fat_profile, fat_scalar_profile, fat_vector_profile, error));
    REQUIRE(fat_scalar_profile.march == "rv64gc");
    REQUIRE(fat_scalar_profile.deployment == target::DeploymentMode::Scalar);
    REQUIRE(!fat_scalar_profile.has_vector());
    REQUIRE(fat_vector_profile.march == "rv64gcv");
    REQUIRE(fat_vector_profile.deployment == target::DeploymentMode::CompileTimeVector);
    REQUIRE(fat_vector_profile.has_vector());

    target::TargetProfile invalid_deployment_profile;
    error.clear();
    REQUIRE(!target::parse_rvv_deployment("runtime", invalid_deployment_profile, error));
    REQUIRE(error.find("expected scalar, compile-time, or fat") != std::string::npos);

    target::TargetProfile scalar_fixed_bits_profile;
    scalar_fixed_bits_profile.march = "rv64gcv";
    REQUIRE(target::parse_rvv_deployment("scalar", scalar_fixed_bits_profile, error));
    REQUIRE(target::parse_vector_bits("256", scalar_fixed_bits_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(scalar_fixed_bits_profile, error));
    REQUIRE(error.find("incompatible with -mrvv-deployment=scalar") != std::string::npos);

    target::TargetProfile versioned_profile;
    versioned_profile.march =
        "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_v1p0_zvl256b1p0";
    error.clear();
    REQUIRE(target::finalize_target_profile(versioned_profile, error));
    REQUIRE(versioned_profile.features.v);
    REQUIRE(versioned_profile.minimum_vlen_bits == 256);

    target::TargetProfile fixed_profile;
    fixed_profile.march = "rv64gcv_zvl128b";
    REQUIRE(target::parse_vector_bits("256", fixed_profile, error));
    error.clear();
    REQUIRE(target::finalize_target_profile(fixed_profile, error));
    REQUIRE(fixed_profile.fixed_vector_bits.has_value());
    REQUIRE(*fixed_profile.fixed_vector_bits == 256);

    target::TargetProfile too_small_profile;
    too_small_profile.march = "rv64gcv_zvl256b";
    REQUIRE(target::parse_vector_bits("128", too_small_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(too_small_profile, error));
    REQUIRE(error.find("smaller than the target minimum VLEN") != std::string::npos);

    target::TargetProfile scalar_bits_profile;
    REQUIRE(target::parse_vector_bits("scalable", scalar_bits_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(scalar_bits_profile, error));
    REQUIRE(error.find("requires V or Zve") != std::string::npos);

    target::TargetProfile vector_abi_profile;
    vector_abi_profile.march = "rv64gcv";
    REQUIRE(target::parse_vector_abi("psabi-vector", vector_abi_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(vector_abi_profile, error));
    REQUIRE(error.find("explicit numeric") != std::string::npos);
    REQUIRE(target::parse_vector_bits("128", vector_abi_profile, error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(vector_abi_profile, error));
    REQUIRE(error.find("PSABI_VECTOR_ABI_UNAVAILABLE") != std::string::npos);
    REQUIRE(error.find("vector tuple") != std::string::npos);
    REQUIRE(error.find("GCC/Clang") != std::string::npos);
    REQUIRE(vector_abi_profile.vector_abi == target::VectorABI::PsABIVector);
    REQUIRE(vector_abi_profile.fixed_vector_bits.has_value());
    REQUIRE(*vector_abi_profile.fixed_vector_bits == 128);

    target::TargetProfile mismatched_vector_abi_profile;
    mismatched_vector_abi_profile.march = "rv64gcv_zvl128b";
    REQUIRE(target::parse_vector_bits("256", mismatched_vector_abi_profile, error));
    REQUIRE(target::parse_vector_abi("psabi-vector", mismatched_vector_abi_profile,
                                     error));
    error.clear();
    REQUIRE(!target::finalize_target_profile(mismatched_vector_abi_profile, error));
    REQUIRE(error.find("must match") != std::string::npos);

    target::TargetProfile invalid_profile;
    invalid_profile.march = "rv64ifd";
    error.clear();
    REQUIRE(!target::finalize_target_profile(invalid_profile, error));
    REQUIRE(error.find("rv64g baseline") != std::string::npos);

    target::TargetProfile invalid_zv_profile;
    invalid_zv_profile.march = "rv64gc_zvl128b";
    error.clear();
    REQUIRE(!target::finalize_target_profile(invalid_zv_profile, error));
    REQUIRE(error.find("requires V or Zve") != std::string::npos);

    target::TargetProfile invalid_zvl_width_profile;
    invalid_zvl_width_profile.march = "rv64gcv_zvl48b";
    error.clear();
    REQUIRE(!target::finalize_target_profile(invalid_zvl_width_profile, error));
    REQUIRE(error.find("malformed Zvl") != std::string::npos);

    target::TargetProfile malformed_zvl_profile;
    malformed_zvl_profile.march = "rv64gcv_zvl256";
    error.clear();
    REQUIRE(!target::finalize_target_profile(malformed_zvl_profile, error));
    REQUIRE(error.find("malformed Zvl") != std::string::npos);

    target::TargetProfile invalid_width_profile;
    error.clear();
    REQUIRE(!target::parse_vector_bits("48", invalid_width_profile, error));
    error.clear();
    REQUIRE(!target::parse_vector_bits("131072", invalid_width_profile, error));

    std::cout << "PASS target_profile_and_data_layout\n";
    return 0;
}
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run target-machine and target CLI tests.")
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    return parser.parse_args()


def find_cxx() -> str | None:
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def run(argv: list[str], *, timeout: float = 20.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def require_success(proc: subprocess.CompletedProcess[str], context: str) -> None:
    require(
        proc.returncode == 0,
        f"{context} failed with exit {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
    )


def require_failure(
    proc: subprocess.CompletedProcess[str], needle: str, context: str
) -> None:
    require(proc.returncode != 0, f"{context} unexpectedly succeeded")
    combined = proc.stdout + proc.stderr
    require(needle in combined, f"{context} did not contain {needle!r}:\n{combined}")


def test_target_harness(tmp_dir: Path) -> None:
    cxx = find_cxx()
    require(cxx is not None, "no host C++ compiler found")
    source = tmp_dir / "target_infra_tests.cpp"
    binary = tmp_dir / "target_infra_tests"
    source.write_text(textwrap.dedent(TARGET_HARNESS))
    compile_proc = run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-I",
            str(ROOT / "include"),
            str(source),
            str(ROOT / "src/target/TargetMachine.cpp"),
            "-o",
            str(binary),
        ]
    )
    require_success(compile_proc, "target harness compilation")
    require_success(run([str(binary)]), "target harness")
    print("PASS target_harness")


def test_cli(compiler: Path, tmp_dir: Path) -> None:
    require(compiler.exists(), f"compiler not found: {compiler}")
    source = tmp_dir / "target_cli.sy"
    source.write_text(
        "int add(int a, int b) { return a + b; }\n"
        "int main() { return add(1, 2); }\n"
    )

    help_proc = run([str(compiler), "--help", "--target=definitely-not-riscv64"])
    require_success(help_proc, "--help with an otherwise invalid target")
    require("-O3" in help_proc.stdout and "-march=<arch>" in help_proc.stdout,
            "--help is missing target/optimization options")
    require("-mrvv-deployment=scalar|compile-time|fat" in help_proc.stdout,
            "--help is missing the RVV deployment option")
    require("generic-rv64 or generic-rvv" in help_proc.stdout,
            "--help is missing the strict CPU/tuning registry")

    default_asm = run([str(compiler), "-S", str(source)])
    require_success(default_asm, "default target assembly")
    require('\t.attribute arch, "rv64gc"' in default_asm.stdout,
            "default assembly is missing the rv64gc arch attribute")
    require("\t.attribute stack_align, 16" in default_asm.stdout,
            "default assembly is missing the 16-byte stack alignment attribute")

    vector_args = [
        str(compiler),
        "-S",
        "--target=riscv64-unknown-elf",
        "-march=RV64GCV",
        "-mabi=LP64D",
        "-mcpu=generic-rv64",
        "-mtune=generic-rv64",
        "-mrvv-vector-bits=scalable",
        str(source),
    ]
    vector_asm = run(vector_args)
    require_success(vector_asm, "RVV target assembly")
    require('\t.attribute arch, "rv64gcv"' in vector_asm.stdout,
            "RVV assembly is missing the rv64gcv arch attribute")

    explicit_compile_time_asm = run(
        [
            str(compiler),
            "-S",
            "-mrvv-deployment=compile-time",
            "-march=rv64gcv",
            str(source),
        ]
    )
    require_success(explicit_compile_time_asm, "explicit compile-time RVV deployment")
    require(explicit_compile_time_asm.stdout == vector_asm.stdout,
            "explicit compile-time deployment changed ordinary RVV assembly output")

    for scalar_deployment_flags in (
        ["-march=rv64gcv", "-mrvv-deployment=scalar"],
        ["-mrvv-deployment", "SCALAR", "-march=RV64GCV"],
    ):
        scalar_deployment_asm = run(
            [str(compiler), "-S", *scalar_deployment_flags, str(source)]
        )
        require_success(scalar_deployment_asm, "explicit scalar deployment on an RVV ISA")
        require('\t.attribute arch, "rv64gcv"' in scalar_deployment_asm.stdout,
                "scalar deployment did not preserve the requested RVV architecture string")

    fat_asm = run(
        [str(compiler), "-S", "-O2", "-mrvv-deployment=fat", str(source)]
    )
    require_success(fat_asm, "fat RVV deployment")
    require(fat_asm.stdout.startswith('\t.attribute arch, "rv64gc"\n'),
            "fat assembly lost its rv64gc baseline attribute")
    require("\t.option arch, +v" in fat_asm.stdout,
            "fat assembly does not locally enable RVV")
    require("__yoolang_scalar_add" in fat_asm.stdout and
            "__yoolang_rvv_add" in fat_asm.stdout and "add:" in fat_asm.stdout,
            "fat assembly is missing variants or the public dispatcher")
    require("__yoolang_scalar_main" in fat_asm.stdout and
            "__yoolang_rvv_main" in fat_asm.stdout,
            "fat assembly did not rewrite the internal call graph")

    fixed_asm = run(
        [str(compiler), "-S", "-march=rv64gcv", "-mrvv-vector-bits=256", str(source)]
    )
    require_success(fixed_asm, "fixed-VLEN target assembly")

    psabi_rejected = run(
        [
            str(compiler),
            "-S",
            "-march=rv64gcv_zvl128b",
            "-mrvv-deployment=compile-time",
            "-mrvv-vector-bits=128",
            "-mvector-abi=psabi-vector",
            str(source),
        ]
    )
    require_failure(psabi_rejected, "PSABI_VECTOR_ABI_UNAVAILABLE",
                    "fixed-length psABI vector public gate")
    require("vector tuple" in psabi_rejected.stderr and
            "GCC/Clang" in psabi_rejected.stderr,
            "psabi-vector public gate lost its two release blockers")

    zve_asm = run(
        [str(compiler), "-S", "-march=rv64gc_zve32x", "-mrvv-vector-bits=32", str(source)]
    )
    require_success(zve_asm, "Zve target assembly")
    require('\t.attribute arch, "rv64gc_zve32x"' in zve_asm.stdout,
            "Zve assembly is missing the requested arch attribute")

    target_mir = run(
        [
            str(compiler),
            "--emit-mir-stage=lowered",
            "--target=riscv64-unknown-elf",
            "-march=rv64gcv_zvl256b",
            "-mcpu=generic-rvv",
            "-mtune=generic-rv64",
            str(source),
        ]
    )
    require_success(target_mir, "target MIR propagation")
    require("target: rv64gcv_zvl256b" in target_mir.stdout, "MIR lost -march")
    require("triple=riscv64-unknown-elf" in target_mir.stdout, "MIR lost --target")
    require("cpu=generic-rvv" in target_mir.stdout, "MIR lost -mcpu")
    require("tune=generic-rv64" in target_mir.stdout, "MIR lost -mtune")
    require("min-vlen=256" in target_mir.stdout, "MIR lost minimum VLEN")

    require_failure(
        run([str(compiler), "-S", "-mcpu=cli-cpu", str(source)]),
        "unsupported -mcpu 'cli-cpu'",
        "unknown CPU rejection",
    )
    require_failure(
        run([str(compiler), "-S", "-mtune=cli-tune", str(source)]),
        "unsupported -mtune 'cli-tune'",
        "unknown tuning CPU rejection",
    )

    vector_plan = run([str(compiler), "--emit-vector-plan", "-O2", str(source)])
    require_success(vector_plan, "vector-plan emission")
    try:
        vector_plan_document = json.loads(vector_plan.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"vector-plan output is not valid JSON: {exc}") from exc
    require(isinstance(vector_plan_document.get("vectorization_plans"), list),
            "vector-plan output lacks the vectorization_plans array")

    o0_loop_source = tmp_dir / "o0_no_autovectorize.sy"
    o0_loop_source.write_text(
        "int values[4096] = {};\n"
        "int main() { int n = getint(); int i = 0; while (i < n) { "
        "values[i] = values[i] + 1; i = i + 1; } "
        "return values[0]; }\n"
    )
    o3_cost_plan = run([
        str(compiler), "--emit-vector-plan", "-O3", "-march=rv64gcv",
        str(o0_loop_source)
    ])
    require_success(o3_cost_plan, "O3 RVV cost/interleave plan")
    o3_plans = json.loads(o3_cost_plan.stdout).get("vectorization_plans", [])
    o3_loop_plans = [
        entry for entry in o3_plans
        if entry.get("vectorizer") == "loop" and
        entry.get("plan", {}).get("minimum_lanes", 0) > 0
    ]
    require(o3_loop_plans, "O3 did not produce a costed Loop Vectorizer plan")
    for entry in o3_loop_plans:
        choice = entry.get("plan", {})
        require(choice.get("interleave") == 2,
                "ordinary -O3 -march=rv64gcv did not select the verified factor-two recipe")
        require(not choice.get("interleave_capability_gate"),
                "a selected factor-two plan retained a capability rejection")
        require(choice.get("interleave_overlap_credit", 0) > 0,
                "O3 factor-two plan lost its target-derived overlap credit")
        require(isinstance(choice.get("predicted_spill_registers"), int),
                "O3 plan lost predicted spill diagnostics")
        require(isinstance(choice.get("estimated_code_bytes"), int),
                "O3 plan lost code-size diagnostics")
        require(isinstance(choice.get("break_even_trip_count"), int),
                "O3 plan lost break-even diagnostics")
        require(choice.get("tuning") == "generic-rv64",
                "ordinary explicit-V plan lost default generic-rv64 tuning identity")

    o3_interleaved_oir = run([
        str(compiler), "--emit-oir", "-O3", "-march=rv64gcv", str(o0_loop_source)
    ])
    require_success(o3_interleaved_oir, "O3 factor-two OIR emission")
    setvl_names = re.findall(r"%(\S+) = setvl ", o3_interleaved_oir.stdout)
    require(len(setvl_names) >= 2, "factor-two OIR did not contain two setvl groups")
    require("remaining.after.chunk0" in o3_interleaved_oir.stdout,
            "factor-two OIR lost the second chunk AVL")
    require("vl.total" in o3_interleaved_oir.stdout,
            "factor-two OIR lost the combined backedge increment")
    for setvl_name in setvl_names[:2]:
        require(f"evl i32 %{setvl_name}" in o3_interleaved_oir.stdout,
                f"factor-two OIR setvl %{setvl_name} is not consumed as its own EVL")

    o2_cost_plan = run([
        str(compiler), "--emit-vector-plan", "-O2", "-mcpu=generic-rvv",
        str(o0_loop_source)
    ])
    require_success(o2_cost_plan, "O2 RVV cost plan")
    o2_plans = json.loads(o2_cost_plan.stdout).get("vectorization_plans", [])
    o2_loop_plans = [
        entry for entry in o2_plans
        if entry.get("vectorizer") == "loop" and
        entry.get("plan", {}).get("minimum_lanes", 0) > 0
    ]
    require(o2_loop_plans, "O2 did not produce a costed Loop Vectorizer plan")
    require(all(not entry.get("plan", {}).get("interleave_capability_gate")
                for entry in o2_loop_plans),
            "O2 unexpectedly requested O3 interleave exploration")

    o0_plan = run([
        str(compiler), "--emit-vector-plan", "-O0", "-fvectorize", "-fslp-vectorize",
        "-march=rv64gcv", str(o0_loop_source)
    ])
    require_success(o0_plan, "O0 vector-plan suppression")
    require(json.loads(o0_plan.stdout).get("vectorization_plans") == [],
            "O0 ran an automatic vectorizer despite its hard pipeline contract")

    scalar_deployment_plan = run([
        str(compiler), "--emit-vector-plan", "-O3", "-fvectorize", "-fslp-vectorize",
        "-march=rv64gcv", "-mrvv-deployment=scalar", str(o0_loop_source)
    ])
    require_success(scalar_deployment_plan, "scalar-deployment vector-plan suppression")
    require(json.loads(scalar_deployment_plan.stdout).get("vectorization_plans") == [],
            "scalar deployment ran an automatic vectorizer on an RVV ISA")

    scalar_deployment_loop_asm = run([
        str(compiler), "-S", "-O3", "-fvectorize", "-fslp-vectorize",
        "-march=rv64gcv", "-mrvv-deployment=scalar", str(o0_loop_source)
    ])
    require_success(scalar_deployment_loop_asm, "scalar-deployment loop assembly")
    require("vsetvli" not in scalar_deployment_loop_asm.stdout and
            "vsetivli" not in scalar_deployment_loop_asm.stdout,
            "scalar deployment emitted an RVV vector-length instruction")

    for level in range(4):
        proc = run([str(compiler), "-S", f"-O{level}", str(source)])
        require_success(proc, f"-O{level} target compilation")

    post_ra = run([str(compiler), "--emit-mir-stage=post-ra", "-O0", str(source)])
    require_success(post_ra, "O0 post-RA MIR")
    instruction_lines = [
        line for line in post_ra.stdout.splitlines() if re.match(r"^\s+[A-Z][A-Z0-9_.]* ", line)
    ]
    require(instruction_lines, "O0 post-RA MIR contained no instructions")
    require(all("%v" not in line for line in instruction_lines),
            "O0 post-RA MIR still contains virtual-register operands")
    require_success(
        run([str(compiler), "--emit-mir-stage=final", "-O0", str(source)]),
        "O0 final MIR",
    )

    # LP64D uses fa0-fa7 for the first eight named f32 parameters, then falls
    # back to the independently available integer argument registers.  This
    # exercises the shared classifier at both the caller and callee boundary.
    fp_fallback_source = tmp_dir / "fp_register_fallback.sy"
    fp_fallback_source.write_text(
        "float ninth(float x0, float x1, float x2, float x3, float x4,\n"
        "            float x5, float x6, float x7, float x8) {\n"
        "  return x8;\n"
        "}\n"
        "int main() {\n"
        "  float value = ninth(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.5);\n"
        "  if (value > 9.0 && value < 10.0) { return 0; }\n"
        "  return 1;\n"
        "}\n"
    )
    fp_fallback_assembly: list[Path] = []
    for level in (0, 1):
        asm_path = tmp_dir / f"fp_register_fallback_o{level}.s"
        proc = run(
            [str(compiler), "-S", f"-O{level}", "-o", str(asm_path),
             str(fp_fallback_source)]
        )
        require_success(proc, f"LP64D float-register fallback at O{level}")
        assembly = asm_path.read_text()
        # O1 is allowed to specialize this constant call away.  O0 preserves
        # the ABI boundary and therefore provides the structural checks.
        if level == 0:
            require(re.search(r"\bfmv\.w\.x\s+\w+,\s*a0\b", assembly) is not None,
                    "O0 callee did not receive the ninth f32 from a0")
            require(re.search(r"\bfmv\.x\.w\s+a0,\s*\w+\b", assembly) is not None,
                    "O0 caller did not pass the ninth f32 through a0")
        fp_fallback_assembly.append(asm_path)

    cross_gcc = shutil.which("riscv64-linux-gnu-gcc")
    qemu = shutil.which("qemu-riscv64")
    if cross_gcc and qemu:
        for level, asm_path in enumerate(fp_fallback_assembly):
            executable = tmp_dir / f"fp_register_fallback_o{level}"
            require_success(
                run([cross_gcc, "-static", "-march=rv64gc", "-mabi=lp64d", "-o",
                     str(executable), str(asm_path)]),
                f"LP64D float-register fallback link at O{level}",
            )
            require_success(
                run([qemu, "-cpu", "rv64,v=false", str(executable)]),
                f"LP64D float-register fallback execution at O{level}",
            )
    else:
        missing = [
            name
            for name, path in (
                ("riscv64-linux-gnu-gcc", cross_gcc),
                ("qemu-riscv64", qemu),
            )
            if path is None
        ]
        require(
            os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") != "1",
            "required LP64D runtime capability is missing: " + ", ".join(missing),
        )
        print(
            "SKIP target_cli_lp64d_runtime (optional host profile capability missing: "
            + ", ".join(missing)
            + ")"
        )

    # This intentionally crosses the former 12,000-OIR-instruction cutoff.  The
    # lowered snapshot must still be produced by the virtual-register selector,
    # and the same invocation must continue through register allocation.
    large_source = tmp_dir / "large_lowering.sy"
    large_source.write_text(
        "int large_lowering(int x) {\n  int value = x;\n"
        + "".join("  value = value + 1;\n" for _ in range(12100))
        + "  return value;\n}\nint main() { return large_lowering(0); }\n"
    )
    large_oir_path = tmp_dir / "large_lowering.oir"
    large_oir = run(
        [str(compiler), "--emit-oir", "-O0", "-o", str(large_oir_path), str(large_source)],
        timeout=30.0,
    )
    require_success(large_oir, "large-function OIR construction")
    oir_instruction_count = sum(
        line.startswith("  ") for line in large_oir_path.read_text().splitlines()
    )
    require(oir_instruction_count > 12000,
            f"large-function fixture has only {oir_instruction_count} OIR instructions")
    large_mir_path = tmp_dir / "large_lowering.mir"
    large_lowering = run(
        [
            str(compiler),
            "--emit-mir-stage=lowered",
            "-O0",
            "-o",
            str(large_mir_path),
            str(large_source),
        ],
        timeout=30.0,
    )
    require_success(large_lowering, "large-function O0 lowering")
    large_mir = large_mir_path.read_text()
    require("virtual registers:" in large_mir,
            "large function fell back from virtual-register lowering")

    invalid_cases = [
        (["--target=x86_64"], "unsupported target triple"),
        (["-march=rv32gc"], "currently requires rv64"),
        (["-march=rv64gc\""], "invalid -march"),
        (["-march=rv64ifd"], "rv64g baseline"),
        (["-mabi=lp64"], "only lp64d is implemented"),
        (["-mrvv-vector-bits=scalable"], "requires V or Zve"),
        (["-march=rv64gcv", "-mrvv-vector-bits=64"], "smaller than the target minimum VLEN"),
        (["-march=rv64gcv", "-mvector-abi=psabi-vector"], "explicit numeric"),
        (["-march=rv64gcv_zvl128b", "-mrvv-vector-bits=256",
          "-mvector-abi=psabi-vector"], "must match"),
        (["-mrvv-deployment=runtime"], "expected scalar, compile-time, or fat"),
        (["-mrvv-deployment=compile-time"], "compile-time requires V or Zve"),
        (["-march=rv64gcv", "-mrvv-deployment=scalar", "-mrvv-vector-bits=256"],
         "incompatible with -mrvv-deployment=scalar"),
        (["-march=rv64gcv_zvl128b", "-mrvv-deployment=fat"],
         "FAT_TARGET_UNSUPPORTED"),
    ]
    for flags, needle in invalid_cases:
        proc = run([str(compiler), *flags, str(source)])
        require_failure(proc, needle, " ".join(flags))

    missing_deployment_value = run([str(compiler), str(source), "-mrvv-deployment"])
    require_failure(missing_deployment_value, "requires scalar, compile-time, or fat",
                    "missing -mrvv-deployment value")

    assembler = shutil.which("riscv64-linux-gnu-as")
    if assembler:
        for name, text in (
            ("default", default_asm.stdout),
            ("vector", vector_asm.stdout),
            ("zve", zve_asm.stdout),
        ):
            asm_path = tmp_dir / f"{name}.s"
            object_path = tmp_dir / f"{name}.o"
            asm_path.write_text(text)
            require_success(
                run([assembler, "-o", str(object_path), str(asm_path)]),
                f"GNU assembler acceptance for {name} target",
            )
    else:
        require(
            os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") != "1",
            "required GNU assembler capability is missing: riscv64-linux-gnu-as",
        )
        print(
            "SKIP target_cli_gnu_as (optional host profile capability missing: "
            "riscv64-linux-gnu-as)"
        )

    print("PASS target_cli")


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    try:
        with tempfile.TemporaryDirectory(prefix="target-infra-") as tmp:
            tmp_dir = Path(tmp)
            test_target_harness(tmp_dir)
            test_cli(compiler, tmp_dir)
    except (RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL target_infra: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
