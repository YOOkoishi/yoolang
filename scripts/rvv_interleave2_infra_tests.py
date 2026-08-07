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
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)


SOURCE = r"""
int input_values[4098];
int output_values[4098];

void interleave_kernel(int n, int offset) {
  int i = 0;
  while (i < n) {
    output_values[offset + i] = input_values[offset + i] + 7;
    i = i + 1;
  }
}

int main() {
  int n = getint();
  int offset = getint();
  if (n < 0 || offset < 0 || offset + n > 4098) {
    return 91;
  }
  int i = 0;
  while (i < 4098) {
    input_values[i] = i * 13 - 17;
    output_values[i] = -1234567;
    i = i + 1;
  }
  interleave_kernel(n, offset);
  i = 0;
  while (i < 4098) {
    int expected = -1234567;
    if (i >= offset && i < offset + n) {
      expected = input_values[i] + 7;
    }
    if (output_values[i] != expected) {
      return 17;
    }
    i = i + 1;
  }
  return 0;
}
"""


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
    command: list[str], *, stdin: str | None = None, timeout: float = 60.0
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=stdin,
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


def function_text(module: str, name: str) -> str:
    start = module.find(f"define void @{name}")
    require(start >= 0, f"missing OIR function @{name}")
    end = module.find("\n}\n", start)
    require(end >= 0, f"unterminated OIR function @{name}")
    return module[start : end + 3]


def loop_success(plan_text: str) -> dict[str, object]:
    plans = json.loads(plan_text).get("vectorization_plans", [])
    matches = [
        entry
        for entry in plans
        if entry.get("vectorizer") == "loop"
        and entry.get("code") == "VECTORIZED"
        and entry.get("function") == "interleave_kernel"
    ]
    require(len(matches) == 1, "interleave_kernel needs one verified loop plan")
    choice = matches[0].get("plan")
    require(isinstance(choice, dict), "interleave_kernel plan has no choice")
    return choice


def verify_cli_contract(source: Path) -> tuple[int, int]:
    o3_plan = run(
        [
            str(COMPILER),
            str(source),
            "--emit-vector-plan",
            "-O3",
            "-march=rv64gcv",
        ]
    )
    o3_choice = loop_success(o3_plan.stdout)
    require(o3_choice.get("interleave") == 2, "ordinary O3 did not select factor two")
    require(
        not o3_choice.get("interleave_capability_gate"),
        "selected factor-two plan retained a capability rejection",
    )
    require(
        int(o3_choice.get("interleave_overlap_credit", 0)) > 0,
        "factor-two plan has no target-derived overlap credit",
    )
    require(
        o3_choice.get("tuning") == "generic-rv64",
        "explicit V march did not retain default generic-rv64 tuning",
    )
    lmul_factors = {
        "mf2": (1, 2),
        "m1": (1, 1),
        "m2": (2, 1),
        "m4": (4, 1),
        "m8": (8, 1),
    }
    lmul = lmul_factors.get(str(o3_choice.get("lmul")))
    require(lmul is not None, "factor-two plan has an unknown LMUL")

    o2_plan = run(
        [
            str(COMPILER),
            str(source),
            "--emit-vector-plan",
            "-O2",
            "-march=rv64gcv",
        ]
    )
    o2_choice = loop_success(o2_plan.stdout)
    require(o2_choice.get("interleave") == 1, "O2 unexpectedly selected factor two")
    require(
        not o2_choice.get("interleave_capability_gate"),
        "O2 unexpectedly requested interleave exploration",
    )

    o3_oir = run(
        [str(COMPILER), str(source), "--emit-oir", "-O3", "-march=rv64gcv"]
    ).stdout
    kernel = function_text(o3_oir, "interleave_kernel")
    setvls = re.findall(r"%(\S+) = setvl ", kernel)
    require(len(setvls) == 2, "factor-two kernel does not contain exactly two setvl")
    require(kernel.count("vp.load ") == 2, "factor-two kernel lacks two VP loads")
    require(kernel.count("vp.store ") == 2, "factor-two kernel lacks two VP stores")
    require(kernel.count("vp.add ") == 2, "factor-two kernel lacks two VP adds")
    require("remaining.after.chunk0" in kernel, "second chunk does not use remaining1")
    require("vl.total" in kernel, "backedge does not use vl0+vl1")
    for setvl in setvls:
        require(
            f"evl i32 %{setvl}" in kernel,
            f"setvl %{setvl} is not consumed as its own VP EVL",
        )

    o2_kernel = function_text(
        run(
            [str(COMPILER), str(source), "--emit-oir", "-O2", "-march=rv64gcv"]
        ).stdout,
        "interleave_kernel",
    )
    require(o2_kernel.count(" = setvl ") == 1, "O2 kernel is not factor one")
    print("PASS rvv_interleave2_cli_plan_and_independent_oir_groups")
    return lmul


def verify_toolchain_and_qemu(
    source: Path,
    directory: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    qemu: str,
    lmul: tuple[int, int],
) -> None:
    assembly = directory / "interleave2.s"
    obj = directory / "interleave2.o"
    executable = directory / "interleave2.exe"
    run(
        [
            str(COMPILER),
            str(source),
            "-S",
            "-O3",
            "-march=rv64gcv",
            "-o",
            str(assembly),
        ]
    )
    run(
        [
            assembler,
            "-march=rv64gcv",
            "-mabi=lp64d",
            str(assembly),
            "-o",
            str(obj),
        ]
    )
    disassembly = run([objdump, "-d", str(obj)]).stdout
    decoded = re.findall(
        r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b", disassembly
    )
    require(decoded.count("vle32.v") >= 2, "objdump lacks both factor-two loads")
    require(decoded.count("vse32.v") >= 2, "objdump lacks both factor-two stores")
    require(decoded.count("vadd.vv") >= 2, "objdump lacks both factor-two adds")
    require(decoded.count("vsetvli") >= 2, "objdump lacks factor-two vector setup")
    run(
        [
            gcc,
            "-static",
            "-march=rv64gcv",
            "-mabi=lp64d",
            str(obj),
            str(RUNTIME),
            "-lm",
            "-o",
            str(executable),
        ]
    )
    print("PASS rvv_interleave2_gnu_as_and_objdump")

    for vlen in VLENS:
        vlmax = (vlen // 32) * lmul[0] // lmul[1]
        require(vlmax > 0, "selected LMUL has no e32 lane at this VLEN")
        lengths = {
            0,
            1,
            vlmax - 1,
            vlmax,
            vlmax + 1,
            2 * vlmax - 1,
            2 * vlmax,
            2 * vlmax + 1,
            min(4096, 3 * vlmax + 7),
            997,
        }
        for offset in (0, 1):
            for length in sorted(lengths):
                require(length + offset <= 4098, "test length exceeds guard allocation")
                run(
                    [
                        qemu,
                        "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                        str(executable),
                    ],
                    stdin=f"{length}\n{offset}\n",
                )
        print(f"PASS rvv_interleave2_qemu_vlen_{vlen}_tails_aligned_unaligned")


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        require(RUNTIME.is_file(), f"runtime archive not found: {RUNTIME}")
        assembler = tool("riscv64-linux-gnu-as")
        gcc = tool("riscv64-linux-gnu-gcc")
        objdump = tool("riscv64-linux-gnu-objdump")
        qemu = tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"RVV interleave2 tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77
    except GateFailure as error:
        print(f"FAIL rvv_interleave2: {error}", file=sys.stderr)
        return 1

    try:
        with tempfile.TemporaryDirectory(prefix="rvv-interleave2-") as tmp:
            directory = Path(tmp)
            source = directory / "interleave2.sy"
            source.write_text(SOURCE, encoding="utf-8")
            lmul = verify_cli_contract(source)
            verify_toolchain_and_qemu(
                source, directory, assembler, gcc, objdump, qemu, lmul
            )
    except (GateFailure, json.JSONDecodeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_interleave2: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
