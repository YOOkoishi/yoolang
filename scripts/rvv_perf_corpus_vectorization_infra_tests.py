#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
SOURCE = ROOT / "test/performance/transpose2.sy"
INPUT = ROOT / "test/performance/transpose2.in"
EXPECTED = ROOT / "test/performance/transpose2.out"
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)


class GateFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateFailure(message)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise FileNotFoundError(name)
    return path


def run(
    command: list[str],
    *,
    stdin: str | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise GateFailure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def function_text(oir_text: str, signature: str) -> str:
    start = oir_text.find(signature)
    require(start >= 0, f"missing OIR function signature: {signature}")
    end = oir_text.find("\n}\n", start)
    require(end >= 0, f"unterminated OIR function: {signature}")
    return oir_text[start : end + 3]


def decoded_vector_mnemonics(disassembly: str) -> list[str]:
    return re.findall(
        r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b",
        disassembly,
    )


def verify_plan(plan_text: str) -> None:
    document = json.loads(plan_text)
    plans = document.get("vectorization_plans")
    require(isinstance(plans, list), "vector plan JSON has no plan list")

    successes = [
        plan
        for plan in plans
        if plan.get("vectorizer") == "loop"
        and plan.get("code") == "VECTORIZED"
        and plan.get("function") == "main"
        and plan.get("region") == "while.body.2"
    ]
    require(
        len(successes) == 1,
        "transpose2 must have exactly one verified main:while.body.2 vector plan",
    )
    success = successes[0]
    require(
        success.get("explanation") == "transformed verified scalable VLA loop",
        "success remark must identify the post-verifier transformation",
    )
    choice = success.get("plan")
    require(isinstance(choice, dict), "success has no plan choice")
    require(choice.get("scalable") is True, "transpose2 plan is not scalable")
    require(choice.get("uses_mask") is True, "rotated diamond plan lost masking")
    require(
        choice.get("runtime_alias_check") is False,
        "rotated diamond must not claim runtime alias versioning",
    )

    expected_alias_rejects = {
        ("transpose", "while.body.5"),
        ("transpose", "while.cond.1"),
        ("main", "inl.transpose.while.body.5.21"),
        ("main", "inl.transpose.while.cond.1.17"),
    }
    actual_alias_rejects = {
        (str(plan.get("function")), str(plan.get("region")))
        for plan in plans
        if plan.get("vectorizer") == "loop"
        and plan.get("code") == "REJECT_ALIAS"
        and plan.get("explanation")
        == "unknown alias requires overflow-safe loop versioning, which LV1 does not claim"
    }
    require(
        expected_alias_rejects <= actual_alias_rejects,
        "transpose2's unresolved alias loops must remain fail-closed",
    )


def verify_oir(oir_text: str) -> None:
    main = function_text(oir_text, "define i32 @main() {")
    start = main.find("while.body.2:")
    end = main.find("while.cond.7:", start)
    require(start >= 0 and end > start, "missing transformed transpose2 init region")
    region = main[start:end]
    require("setvl <vscale x " in region, "init region has no scalable setvl")
    require(region.count("vp.store ") == 2, "init region must contain two VP stores")
    require("vp.icmp eq" in region, "lane condition was not widened")
    require(".if.then.mask = vp.and" in region, "then predicate mask is missing")
    require("mask <vscale x " in region, "VP stores are not predicated")
    require("if.then.4:" not in main, "scalar then arm survived if-conversion")
    require(
        re.search(r"(?m)^\s+store\s", region) is None,
        "scalar store survived in the transformed init region",
    )
    require(
        re.search(
            r"remaining = phi \[[^\]]+, %entry\.0\], "
            r"\[[^\]]+, %if\.end\.6\] : i32",
            region,
        )
        is not None,
        "remaining phi must retain the latch as its backedge predecessor",
    )
    require(
        "br %if.end.6" in region
        and re.search(
            r"if\.end\.6:\n\s+br i1 %[^,]+, %while\.body\.2, %while\.cond\.7",
            region,
        )
        is not None,
        "rotated loop must preserve its dedicated latch and common exit",
    )
    require(
        re.search(
            r"entry\.0:.*?icmp lt i32 0, %v0.*?"
            r"br i1 %[^,]+, %while\.body\.2, %while\.cond\.7",
            main,
            re.DOTALL,
        )
        is not None,
        "zero-trip guard no longer bypasses the vector body",
    )


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"required RVV corpus tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77

    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        for path in (SOURCE, INPUT, EXPECTED, RUNTIME):
            require(path.is_file(), f"required corpus artifact not found: {path}")

        plan = run(
            [
                str(COMPILER),
                str(SOURCE),
                "--emit-vector-plan",
                "-O2",
                "-march=rv64gcv",
            ]
        )
        verify_plan(plan.stdout)
        print("PASS transpose2_verified_vector_plan_and_alias_rejections")

        oir_result = run(
            [
                str(COMPILER),
                str(SOURCE),
                "--emit-oir",
                "-O2",
                "-march=rv64gcv",
            ]
        )
        verify_oir(oir_result.stdout)
        print("PASS transpose2_guarded_rotated_diamond_oir")

        with tempfile.TemporaryDirectory(prefix="rvv-perf-corpus-") as tmp:
            tmp_dir = Path(tmp)
            rvv_asm = tmp_dir / "transpose2.rvv.s"
            scalar_asm = tmp_dir / "transpose2.scalar.s"
            rvv_object = tmp_dir / "transpose2.rvv.o"
            scalar_object = tmp_dir / "transpose2.scalar.o"
            rvv_executable = tmp_dir / "transpose2.rvv.exe"
            scalar_executable = tmp_dir / "transpose2.scalar.exe"

            run(
                [
                    str(COMPILER),
                    str(SOURCE),
                    "-S",
                    "-O2",
                    "-march=rv64gcv",
                    "-o",
                    str(rvv_asm),
                ]
            )
            run(
                [
                    str(COMPILER),
                    str(SOURCE),
                    "-S",
                    "-O2",
                    "-march=rv64gc",
                    "-o",
                    str(scalar_asm),
                ]
            )
            run(
                [
                    gcc,
                    "-c",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    str(rvv_asm),
                    "-o",
                    str(rvv_object),
                ]
            )
            run(
                [
                    gcc,
                    "-c",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    str(scalar_asm),
                    "-o",
                    str(scalar_object),
                ]
            )
            rvv_disassembly = run([objdump, "-d", str(rvv_object)]).stdout
            scalar_disassembly = run([objdump, "-d", str(scalar_object)]).stdout
            rvv_mnemonics = set(decoded_vector_mnemonics(rvv_disassembly))
            required_mnemonics = {
                "vsetvli",
                "vid.v",
                "vse32.v",
                "vmseq.vv",
                "vmand.mm",
            }
            require(
                required_mnemonics <= rvv_mnemonics,
                "decoded transpose2 object lacks real rotated-diamond RVV opcodes: "
                + ", ".join(sorted(required_mnemonics - rvv_mnemonics)),
            )
            require(
                not decoded_vector_mnemonics(scalar_disassembly),
                "rv64gc transpose2 object unexpectedly contains decoded RVV opcodes",
            )
            print("PASS transpose2_decoded_rvv_and_scalar_baseline")

            run(
                [
                    gcc,
                    "-static",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    str(rvv_asm),
                    str(RUNTIME),
                    "-lm",
                    "-o",
                    str(rvv_executable),
                ]
            )
            run(
                [
                    gcc,
                    "-static",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    str(scalar_asm),
                    str(RUNTIME),
                    "-lm",
                    "-o",
                    str(scalar_executable),
                ]
            )
            input_text = INPUT.read_text(encoding="utf-8")
            expected_first_line = EXPECTED.read_text(encoding="utf-8").splitlines()[0]
            scalar_run = run(
                [qemu, "-cpu", "rv64,v=false", str(scalar_executable)],
                stdin=input_text,
            )
            require(
                scalar_run.stdout.splitlines() == [expected_first_line],
                "scalar transpose2 output differs from the corpus oracle",
            )
            for vlen in VLENS:
                rvv_run = run(
                    [
                        qemu,
                        "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64",
                        str(rvv_executable),
                    ],
                    stdin=input_text,
                )
                require(
                    rvv_run.stdout == scalar_run.stdout,
                    f"transpose2 RVV output differs at VLEN={vlen}",
                )
            print("PASS transpose2_scalar_and_vlen_128_256_512_1024")
    except (GateFailure, json.JSONDecodeError, OSError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
