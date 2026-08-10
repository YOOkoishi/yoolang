#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/vectorization.md"
DOC_INDEX = ROOT / "docs/README.md"
PSABI_DOC = ROOT / "docs/psabi-vector-abi.md"
LOOP_SOURCE = ROOT / "test/ir/oir_loop_vectorize.sy"
SLP_SOURCE = ROOT / "test/ir/oir_slp_vectorize.sy"


class ContractError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the documented Loop/SLP vectorization CLI contracts."
    )
    parser.add_argument("--compiler", type=Path, help="compiler binary to test")
    return parser.parse_args()


def find_compiler(explicit: Path | None) -> Path | None:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    configured = os.environ.get("YOOLANG_COMPILER")
    if configured:
        candidates.append(Path(configured))
    candidates.extend(
        (
            ROOT / "build/linux/x86_64/release/compiler",
            ROOT / "build/linux/x86_64/debug/compiler",
        )
    )
    for candidate in candidates:
        resolved = candidate if candidate.is_absolute() else ROOT / candidate
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved.resolve()
    return None


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def run_compiler(
    compiler: Path,
    source: Path,
    *arguments: str,
    expect_success: bool = True,
) -> subprocess.CompletedProcess[str]:
    command = [str(compiler), *arguments, str(source)]
    try:
        process = subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ContractError(f"compiler timed out: {' '.join(command)}") from error
    if expect_success and process.returncode != 0:
        raise ContractError(
            f"compiler exited {process.returncode}: {' '.join(command)}\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}"
        )
    if not expect_success and process.returncode == 0:
        raise ContractError(
            f"compiler unexpectedly accepted: {' '.join(command)}\n{process.stdout}"
        )
    return process


def function_text(oir_text: str, name: str) -> str:
    marker = f"define void @{name}("
    start = oir_text.find(marker)
    require(start >= 0, f"OIR omitted function @{name}")
    end = oir_text.find("\n}\n", start)
    if end < 0:
        end = oir_text.find("\n}", start)
    require(end >= 0, f"OIR function @{name} has no closing brace")
    return oir_text[start : end + 2]


def load_plans(output: str) -> list[dict[str, object]]:
    try:
        document = json.loads(output)
    except json.JSONDecodeError as error:
        raise ContractError(f"vector plan is not valid JSON: {error}\n{output}") from error
    plans = document.get("vectorization_plans")
    require(isinstance(plans, list), "JSON omitted vectorization_plans array")
    return plans


def matching_plan(
    plans: list[dict[str, object]], vectorizer: str, code: str, function: str
) -> dict[str, object]:
    matches = [
        plan
        for plan in plans
        if plan.get("vectorizer") == vectorizer
        and plan.get("code") == code
        and plan.get("function") == function
    ]
    require(
        len(matches) == 1,
        f"expected one {vectorizer}/{code}/{function} plan, got {len(matches)}",
    )
    return matches[0]


def validate_document_contract() -> None:
    require(DOC.is_file(), "docs/vectorization.md is missing")
    require(PSABI_DOC.is_file(), "docs/psabi-vector-abi.md is missing")
    require(LOOP_SOURCE.is_file(), "existing Loop Vectorizer FileCheck source is missing")
    require(SLP_SOURCE.is_file(), "existing SLP FileCheck source is missing")
    text = DOC.read_text(encoding="utf-8")
    required = (
        "OIRLoopVectorizerPass",
        "OIRSLPVectorizerPass",
        "remaining",
        "actual_vl",
        "setvl",
        "VPLoad",
        "VPGather",
        "-1",
        "-2",
        "-4",
        "then_mask",
        "else_mask",
        "VPReductionInst",
        "mutation journal",
        "REJECT_ALIAS",
        "REJECT_POTENTIAL_TRAP",
        "runtime alias versioning",
        "volatile/atomic",
        "force",
        "-O2",
        "-O3",
        "-mvector-abi=psabi-vector",
        "standard ABI",
        "portable object layout",
        "不是“完整 GA 向量化支持”",
        "VECTORIZED",
        "--emit-vector-plan",
    )
    missing = [needle for needle in required if needle not in text]
    require(not missing, f"documentation omitted required contracts: {missing}")
    index = DOC_INDEX.read_text(encoding="utf-8")
    require(
        "[vectorization.md](vectorization.md)" in index,
        "docs/README.md does not index vectorization.md",
    )
    require(
        "[psabi-vector-abi.md](psabi-vector-abi.md)" in index,
        "docs/README.md does not index psabi-vector-abi.md",
    )
    psabi = PSABI_DOC.read_text(encoding="utf-8")
    for needle in (
        "ABI_VLEN",
        "v8`–`v23",
        "v1`–`v7",
        ".variant_cc",
        "STO_RISCV_VARIANT_CC",
        "VLEN 128/256/512/1024",
    ):
        require(needle in psabi, f"psABI vector documentation omitted {needle}")
    print("PASS vectorization_document_contract")


def validate_loop_contract(compiler: Path) -> None:
    success = run_compiler(
        compiler, LOOP_SOURCE, "--emit-oir", "-O2", "-march=rv64gcv"
    )
    body = function_text(success.stdout, "add_bias")
    for needle in ("setvl <vscale x", "vp.load <vscale x", "vp.add <vscale x", "vp.store <vscale x", "evl i32"):
        require(needle in body, f"Loop Vectorizer OIR omitted {needle!r}")

    plan_process = run_compiler(
        compiler, LOOP_SOURCE, "--emit-vector-plan", "-O2", "-march=rv64gcv"
    )
    plan = matching_plan(load_plans(plan_process.stdout), "loop", "VECTORIZED", "add_bias")
    choice = plan.get("plan")
    require(isinstance(choice, dict), "loop plan omitted plan object")
    require(choice.get("scalable") is True, "loop plan is not scalable")
    require(
        isinstance(choice.get("minimum_lanes"), int) and choice["minimum_lanes"] > 0,
        "loop plan has no positive minimum lane count",
    )
    require(choice.get("uses_mask") is True, "loop plan omitted mask usage")
    require(
        choice.get("runtime_alias_check") is False,
        "loop plan falsely claims runtime alias versioning",
    )

    remarks = run_compiler(
        compiler,
        LOOP_SOURCE,
        "--emit-oir",
        "-O2",
        "-march=rv64gcv",
        "-Rpass=loop",
    )
    require(
        "remark: vectorize(loop): VECTORIZED: add_bias:" in remarks.stderr,
        "loop success remark contract changed",
    )

    for label, arguments in (
        ("disabled", ("-O2", "-fno-vectorize", "-march=rv64gcv")),
        ("o1_default", ("-O1", "-march=rv64gcv")),
        ("non_v", ("-O2", "-march=rv64gc")),
    ):
        scalar = run_compiler(compiler, LOOP_SOURCE, "--emit-oir", *arguments)
        scalar_body = function_text(scalar.stdout, "add_bias")
        require("setvl " not in scalar_body and "vp." not in scalar_body,
                f"loop {label} unexpectedly auto-vectorized")
        require("load i32" in scalar_body and "store i32" in scalar_body,
                f"loop {label} did not retain scalar memory operations")

    disabled_plan = load_plans(
        run_compiler(
            compiler,
            LOOP_SOURCE,
            "--emit-vector-plan",
            "-O2",
            "-fno-vectorize",
            "-march=rv64gcv",
        ).stdout
    )
    require(not disabled_plan, "disabled loop pipeline unexpectedly emitted a plan")
    print("PASS loop_vectorization_cli_contract")


def validate_slp_contract(compiler: Path) -> None:
    success = run_compiler(
        compiler, SLP_SOURCE, "--emit-oir", "-O3", "-march=rv64gcv"
    )
    body = function_text(success.stdout, "four_lane_add")
    for needle in ("vp.load <4 x i32>", "splat i32 7", "vp.add <4 x i32>", "vp.store <4 x i32>"):
        require(needle in body, f"SLP OIR omitted {needle!r}")

    plan_process = run_compiler(
        compiler, SLP_SOURCE, "--emit-vector-plan", "-O3", "-march=rv64gcv"
    )
    plans = load_plans(plan_process.stdout)
    vectorized = matching_plan(plans, "slp", "VECTORIZED", "four_lane_add")
    choice = vectorized.get("plan")
    require(isinstance(choice, dict), "SLP plan omitted plan object")
    require(choice.get("scalable") is False, "SLP plan is not fixed-width")
    require(choice.get("minimum_lanes") == 4, "SLP example changed its four-lane pack")
    require(choice.get("uses_mask") is True, "SLP plan omitted full-lane mask")
    require(
        choice.get("runtime_alias_check") is False,
        "SLP plan falsely claims runtime alias versioning",
    )

    rejected = matching_plan(plans, "slp", "REJECT_CALL", "main")
    explanation = rejected.get("explanation")
    require(
        isinstance(explanation, str) and explanation.startswith("SLP_REJECT_CALL:"),
        "SLP stable reject lost its SLP_REJECT_CALL detail",
    )

    remarks = run_compiler(
        compiler,
        SLP_SOURCE,
        "--emit-oir",
        "-O3",
        "-march=rv64gcv",
        "-Rpass=slp",
        "-Rpass-missed=slp",
    )
    require(
        "remark: vectorize(slp): VECTORIZED: four_lane_add:" in remarks.stderr,
        "SLP success remark contract changed",
    )
    require(
        "remark: vectorize(slp): REJECT_CALL: main:entry.0: SLP_REJECT_CALL:" in remarks.stderr,
        "SLP stable missed-remark contract changed",
    )

    for label, arguments in (
        ("disabled", ("-O3", "-fno-slp-vectorize", "-march=rv64gcv")),
        ("o2_default", ("-O2", "-march=rv64gcv")),
        ("non_v", ("-O3", "-march=rv64gc")),
    ):
        scalar = run_compiler(compiler, SLP_SOURCE, "--emit-oir", *arguments)
        scalar_body = function_text(scalar.stdout, "four_lane_add")
        require("vp." not in scalar_body, f"SLP {label} unexpectedly auto-vectorized")
        require("load i32" in scalar_body and "store i32" in scalar_body,
                f"SLP {label} did not retain scalar memory operations")

    non_v_plan = load_plans(
        run_compiler(
            compiler,
            SLP_SOURCE,
            "--emit-vector-plan",
            "-O3",
            "-march=rv64gc",
        ).stdout
    )
    require(not non_v_plan, "non-V SLP pipeline unexpectedly emitted a plan")
    print("PASS slp_vectorization_cli_and_reject_contract")


def validate_psabi_contract(compiler: Path) -> None:
    rejected = run_compiler(
        compiler,
        LOOP_SOURCE,
        "--emit-oir",
        "-O2",
        "-march=rv64gcv",
        "-mvector-abi=psabi-vector",
        expect_success=False,
    )
    require(
        "requires an explicit numeric -mrvv-vector-bits=ABI_VLEN" in rejected.stderr,
        "psABI-vector missing-ABI_VLEN rejection is no longer explicit and stable",
    )
    gated = run_compiler(
        compiler,
        LOOP_SOURCE,
        "-S",
        "-O2",
        "-march=rv64gcv_zvl128b",
        "-mrvv-deployment=compile-time",
        "-mrvv-vector-bits=128",
        "-mvector-abi=psabi-vector",
        expect_success=False,
    )
    require(
        "PSABI_VECTOR_ABI_UNAVAILABLE" in gated.stderr,
        "psABI-vector complete profile no longer has the stable public gate",
    )
    require(
        "vector tuple" in gated.stderr and "GCC/Clang" in gated.stderr,
        "psABI-vector public gate no longer names both release blockers",
    )
    print("PASS vector_psabi_explicit_abi_vlen_then_release_gate")


def main() -> int:
    args = parse_args()
    compiler = find_compiler(args.compiler)
    if compiler is None:
        print(
            "FAIL vectorization_docs: compiler binary not found; run xmake or pass --compiler",
            file=sys.stderr,
        )
        return 1
    try:
        validate_document_contract()
        validate_loop_contract(compiler)
        validate_slp_contract(compiler)
        validate_psabi_contract(compiler)
    except ContractError as error:
        print(f"FAIL vectorization_docs: {error}", file=sys.stderr)
        return 1
    print("PASS vectorization_docs (Loop/SLP OIR, plan, remarks, gates, ABI boundary)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
