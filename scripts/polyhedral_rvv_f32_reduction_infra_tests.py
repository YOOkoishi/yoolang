#!/usr/bin/env python3

from __future__ import annotations

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
SOURCES = {
    ROOT / "test/poly/50_float_output_reduction_rvv.sy": (
        "vle32.v",
        "vse32.v",
        "vfmul.vv",
        "vfadd.vv",
    ),
    ROOT / "test/poly/51_float_guarded_output_reduction_rvv.sy": (
        "vle32.v",
        "vse32.v",
        "vfmul.vv",
        "vfadd.vv",
    ),
    ROOT / "test/poly/53_float_guarded_direct_output_reduction_rvv.sy": (
        "vle32.v",
        "vse32.v",
        "vfadd.vv",
    ),
    ROOT / "test/poly/54_float_lane_guarded_direct_output_reduction_rvv.sy": (
        "vle32.v",
        "vse32.v",
        "vmflt.vv",
        "vfadd.vv",
    ),
    ROOT / "test/poly/56_float_lane_guarded_binary_output_reduction_rvv.sy": (
        "vle32.v",
        "vlse32.v",
        "vse32.v",
        "vmflt.vv",
        "vfmul.vv",
        "vfadd.vv",
    ),
    ROOT / "test/poly/59_float_false_arm_lane_guarded_output_reduction_rvv.sy": (
        "vle32.v",
        "vse32.v",
        "vmflt.vv",
        "vmnand.mm",
        "vfadd.vv",
    ),
}
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)


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


def run(command: list[str], timeout: float = 120.0) -> subprocess.CompletedProcess[str]:
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


def verify_source(
    source: Path,
    root: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    qemu: str,
    required_mnemonics: tuple[str, ...],
) -> None:
    directory = root / source.stem
    directory.mkdir()
    assembly = directory / "reduction.s"
    obj = directory / "reduction.o"
    executable = directory / "reduction.exe"

    run(
        [
            str(COMPILER),
            str(source),
            "-S",
            "-O1",
            "-march=rv64gcv",
            "-fvectorize",
            "--polyhedral",
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
    decoded = set(
        re.findall(
            r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b",
            disassembly,
        )
    )
    for mnemonic in required_mnemonics:
        require(mnemonic in decoded, f"{source.name}: objdump lacks {mnemonic}")

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
    print(f"PASS {source.stem}_gnu_as_and_objdump")

    for vlen in VLENS:
        run(
            [
                qemu,
                "-cpu",
                f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                str(executable),
            ]
        )
        print(f"PASS {source.stem}_qemu_vlen_{vlen}")


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        for source in SOURCES:
            require(source.is_file(), f"test source not found: {source}")
        require(RUNTIME.is_file(), f"runtime archive not found: {RUNTIME}")
        assembler = tool("riscv64-linux-gnu-as")
        gcc = tool("riscv64-linux-gnu-gcc")
        objdump = tool("riscv64-linux-gnu-objdump")
        qemu = tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"polyhedral RVV f32 tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77
    except GateFailure as error:
        print(f"FAIL polyhedral_rvv_f32_reduction: {error}", file=sys.stderr)
        return 1

    try:
        with tempfile.TemporaryDirectory(prefix="polyhedral-rvv-f32-") as tmp:
            for source, required_mnemonics in SOURCES.items():
                verify_source(
                    source,
                    Path(tmp),
                    assembler,
                    gcc,
                    objdump,
                    qemu,
                    required_mnemonics,
                )
    except (GateFailure, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL polyhedral_rvv_f32_reduction: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
