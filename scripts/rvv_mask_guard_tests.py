#!/usr/bin/env python3

"""Required RVV masked-memory fault-suppression gate.

This runner deliberately uses scalar public ABIs and source-local fixed vector
values.  It therefore tests masked RVV memory semantics without depending on a
vector calling convention.  A single statically linked binary is executed at
VLEN=128/256/512/1024.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "scripts/fixtures/rvv_mask_guard"
SOURCE = FIXTURE_DIR / "masked_guard.sy"
HARNESS = FIXTURE_DIR / "masked_guard_harness.c"
DEFAULT_COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
VLENS = (128, 256, 512, 1024)
DEFAULT_CASE_SEED = 0x4D_41_53_4B_32


@dataclasses.dataclass(frozen=True)
class GuardCase:
    name: str
    function: str
    mask: tuple[bool, ...]
    rw_lanes: tuple[int, ...]
    guard_lanes: tuple[int, ...]
    expected_return: int
    alignment_offset: int


CASES = (
    GuardCase(
        name="allfalse",
        function="masked_guard_allfalse",
        mask=(False,) * 7,
        rw_lanes=(),
        guard_lanes=tuple(range(7)),
        expected_return=143,
        alignment_offset=0,
    ),
    GuardCase(
        name="sparse",
        function="masked_guard_sparse",
        mask=(True, False, True, False, False, False, False),
        rw_lanes=(0, 1, 2),
        guard_lanes=(3, 4, 5, 6),
        expected_return=1569,
        alignment_offset=4,
    ),
)


class GateFailure(RuntimeError):
    pass


class ImplementationBlocked(RuntimeError):
    pass


def harness_arguments(
    case_name: str,
    case_seed: int = DEFAULT_CASE_SEED,
    alignment_offset: int = 0,
) -> list[str]:
    if case_name not in {case.name for case in CASES}:
        raise ValueError(f"unknown MASK2 case: {case_name}")
    if case_seed < 0 or case_seed > 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError("MASK2 case seed must fit unsigned 64-bit")
    if alignment_offset < 0 or alignment_offset > 60 or alignment_offset % 4 != 0:
        raise ValueError("MASK2 alignment offset must be 0..60 and four-byte aligned")
    return [case_name, str(case_seed), str(alignment_offset)]


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise GateFailure(f"required MASK2 tool not found: {name}")
    return path


def run_command(
    argv: list[str], *, timeout: float, label: str
) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            argv,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise GateFailure(f"{label} timed out after {timeout:g}s") from error
    if result.returncode != 0:
        raise GateFailure(
            f"{label} failed ({result.returncode}): {' '.join(argv)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def assembly_function(text: str, function: str) -> str:
    start_match = re.search(rf"(?m)^{re.escape(function)}:\s*$", text)
    if start_match is None:
        raise ImplementationBlocked(f"assembly lacks @{function}")
    end_match = re.search(
        rf"(?m)^\s*\.size\s+{re.escape(function)}\s*,", text[start_match.end() :]
    )
    if end_match is None:
        raise GateFailure(f"assembly lacks .size boundary for @{function}")
    return text[start_match.end() : start_match.end() + end_match.start()]


def objdump_function(text: str, function: str) -> str:
    start_match = re.search(
        rf"(?m)^[0-9a-fA-F]+\s+<{re.escape(function)}>:\s*$", text
    )
    if start_match is None:
        raise GateFailure(f"objdump lacks <{function}>")
    later_global = re.search(
        r"(?m)^[0-9a-fA-F]+\s+<masked_guard_(?:allfalse|sparse)>:\s*$",
        text[start_match.end() :],
    )
    end = len(text) if later_global is None else start_match.end() + later_global.start()
    return text[start_match.end() : end]


def masked_memory_mnemonics(body: str) -> tuple[str, ...]:
    operations: list[str] = []
    for line in body.splitlines():
        match = re.search(r"\b(vle32\.v|vse32\.v)\b", line)
        if match is None:
            continue
        if re.search(r"\bv0\.t\b", line) is None:
            raise GateFailure(
                f"unsafe unmasked vector memory operation in guard kernel: {line.strip()}"
            )
        operations.append(match.group(1))
    return tuple(operations)


def validate_codegen(text: str, *, objdump: bool) -> None:
    extractor = objdump_function if objdump else assembly_function
    for case in CASES:
        body = extractor(text, case.function)
        operations = masked_memory_mnemonics(body)
        if operations.count("vle32.v") != 1 or operations.count("vse32.v") != 1:
            raise ImplementationBlocked(
                f"@{case.function} must contain exactly one masked vle32.v and vse32.v; "
                f"found {operations or 'none'}"
            )
        if "vsetivli" not in body and "vsetvli" not in body:
            raise ImplementationBlocked(f"@{case.function} lacks an RVV VL setup")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run required RVV all-false/sparse PROT_NONE masked-memory tests"
    )
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--timeout", type=float, default=30.0)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        compiler = args.compiler.resolve()
        if not compiler.is_file():
            raise GateFailure(f"release compiler not found: {compiler}")
        if not SOURCE.is_file() or not HARNESS.is_file():
            raise GateFailure(f"MASK2 fixture is incomplete under {FIXTURE_DIR}")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")

        with tempfile.TemporaryDirectory(prefix="yoolang-rvv-mask-guard-") as temp:
            directory = Path(temp)
            assembly = directory / "masked_guard.s"
            obj = directory / "masked_guard.o"
            executable = directory / "masked_guard"

            try:
                compile_result = subprocess.run(
                    [
                        str(compiler),
                        str(SOURCE),
                        "-S",
                        "-O0",
                        "-march=rv64gcv",
                        "-mabi=lp64d",
                        "-o",
                        str(assembly),
                    ],
                    cwd=ROOT,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    check=False,
                    timeout=args.timeout,
                )
            except subprocess.TimeoutExpired as error:
                raise GateFailure(f"MASK2 compiler timed out after {args.timeout:g}s") from error
            if compile_result.returncode != 0:
                raise ImplementationBlocked(
                    "source-local fixed masked memory did not compile for rv64gcv\n"
                    f"stdout:\n{compile_result.stdout}\nstderr:\n{compile_result.stderr}"
                )

            assembly_text = assembly.read_text(encoding="utf-8")
            if '.attribute arch, "rv64gcv"' not in assembly_text:
                raise GateFailure("RVV assembly lacks the rv64gcv architecture attribute")
            validate_codegen(assembly_text, objdump=False)
            print("PASS mask_guard_assembly_model")

            run_command(
                [
                    gcc,
                    "-c",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    "-Wa,--fatal-warnings",
                    str(assembly),
                    "-o",
                    str(obj),
                ],
                timeout=args.timeout,
                label="GNU as MASK2 assembly",
            )
            disassembly = run_command(
                [objdump, "-dr", str(obj)],
                timeout=args.timeout,
                label="GNU objdump MASK2 decode",
            ).stdout
            validate_codegen(disassembly, objdump=True)
            print("PASS mask_guard_gnu_as_objdump")

            run_command(
                [
                    gcc,
                    "-O1",
                    "-fno-tree-vectorize",
                    "-fno-tree-slp-vectorize",
                    "-static",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    "-mcmodel=medany",
                    str(obj),
                    str(HARNESS),
                    "-o",
                    str(executable),
                ],
                timeout=args.timeout,
                label="MASK2 static link",
            )

            for vlen in VLENS:
                cpu = f"rv64,v=true,vlen={vlen},elen=64"
                for case in CASES:
                    run_command(
                        [
                            qemu,
                            "-cpu",
                            cpu,
                            str(executable),
                            *harness_arguments(
                                case.name,
                                DEFAULT_CASE_SEED,
                                case.alignment_offset,
                            ),
                        ],
                        timeout=args.timeout,
                        label=f"MASK2 {case.name} VLEN={vlen}",
                    )
                    print(f"PASS mask_guard_{case.name}_vlen_{vlen}")
    except ImplementationBlocked as error:
        print(f"BLOCKED_IMPLEMENTATION mask_guard: {error}", file=sys.stderr)
        return 2
    except GateFailure as error:
        print(f"FAIL mask_guard: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
