#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)


SOURCE = r"""
int values[4096] = {};

void add_bias(int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + 1;
  }
}

int main() {
  int n = getarray(values);
  add_bias(n);
  putarray(n, values);
  return 0;
}
"""


SLP_SOURCE = r"""
int slp_input[4] = {};
int slp_output[4] = {};

void four_lane_add() {
  int a0 = slp_input[0];
  int a1 = slp_input[1];
  int a2 = slp_input[2];
  int a3 = slp_input[3];
  int b0 = a0 + 7;
  int b1 = a1 + 7;
  int b2 = a2 + 7;
  int b3 = a3 + 7;
  slp_output[0] = b0;
  slp_output[1] = b1;
  slp_output[2] = b2;
  slp_output[3] = b3;
}
"""


SLP_DRIVER = r"""
#include <stdint.h>
#include <stdio.h>

extern int32_t slp_input[4];
extern int32_t slp_output[4];
extern void four_lane_add(void);

int main(void) {
    int count = 0;
    if (scanf("%d", &count) != 1 || count != 4) {
        return 91;
    }
    for (int lane = 0; lane < 4; ++lane) {
        if (scanf("%d", &slp_input[lane]) != 1) {
            return 92;
        }
    }
    four_lane_add();
    printf("4: %d %d %d %d\n", slp_output[0], slp_output[1],
           slp_output[2], slp_output[3]);
    return 0;
}
"""


def require_path(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"required {description} not found: {path}")
    return path


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required RVV differential-test tool not found: {name}")
    return path


def run_checked(
    command: list[str],
    *,
    input_bytes: bytes | None = None,
    timeout: float = 60.0,
) -> subprocess.CompletedProcess[bytes]:
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


def compile_and_link(
    compiler: Path,
    gcc: str,
    source: Path,
    directory: Path,
    *,
    name: str,
    march: str,
    optimization: int,
    driver: Path | None = None,
) -> tuple[Path, Path]:
    assembly = directory / f"{name}.s"
    executable = directory / name
    run_checked(
        [
            str(compiler),
            str(source),
            "-S",
            f"-O{optimization}",
            f"-march={march}",
            "-mabi=lp64d",
            "-o",
            str(assembly),
        ]
    )
    link_command = [
        gcc,
        "-static",
        f"-march={march}",
        "-mabi=lp64d",
        "-mcmodel=medany",
        str(assembly),
    ]
    if driver is not None:
        link_command.append(str(driver))
    link_command.extend([str(RUNTIME), "-o", str(executable)])
    run_checked(link_command)
    return assembly, executable


def disassembled_symbol(disassembly: str, symbol: str) -> str:
    pattern = re.compile(
        rf"^[0-9a-f]+ <{re.escape(symbol)}>:\n(.*?)(?=^[0-9a-f]+ <[^>]+>:\n|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(disassembly)
    if match is None:
        raise RuntimeError(f"linked binary lacks disassembled symbol {symbol}")
    return match.group(1)


def signed_i32(value: int) -> int:
    value &= 0xFFFF_FFFF
    return value if value < 0x8000_0000 else value - 0x1_0000_0000


def encode_case(values: list[int]) -> bytes:
    return (str(len(values)) + (" " + " ".join(map(str, values)) if values else "") + "\n").encode()


def expected_output(values: list[int]) -> bytes:
    result = [signed_i32(value + 7) for value in values]
    suffix = "" if not result else " " + " ".join(map(str, result))
    return f"{len(result)}:{suffix}\n".encode()


def cases() -> list[list[int]]:
    # For e32,m1 the VLMAX values are 4/8/16/32 for the tested VLENs.  This
    # union covers 0, 1, VLMAX-1, VLMAX, VLMAX+1, 2*VLMAX +/- 1 and larger
    # strip-mined loops with the exact same RVV executable.
    lengths = {
        0,
        1,
        3,
        4,
        5,
        7,
        8,
        9,
        15,
        16,
        17,
        31,
        32,
        33,
        63,
        64,
        65,
        127,
        257,
        1023,
        4095,
    }
    rng = random.Random(0x52565631)
    boundary_values = [
        -(1 << 31),
        -(1 << 31) + 1,
        -8,
        -7,
        -1,
        0,
        1,
        (1 << 31) - 8,
        (1 << 31) - 7,
        (1 << 31) - 1,
    ]
    generated: list[list[int]] = []
    for length in sorted(lengths):
        values = [rng.randint(-(1 << 31), (1 << 31) - 1) for _ in range(length)]
        for index, value in enumerate(boundary_values[: min(length, len(boundary_values))]):
            values[index] = value
        generated.append(values)
    return generated


def slp_cases() -> list[list[int]]:
    fixed = [
        [0, 1, -1, 7],
        [-(1 << 31), (1 << 31) - 1, (1 << 31) - 7, (1 << 31) - 8],
        [-7, -8, 1024, -1024],
    ]
    rng = random.Random(0x534C5031)
    for _ in range(9):
        fixed.append([rng.randint(-(1 << 31), (1 << 31) - 1) for _ in range(4)])
    return fixed


def main() -> int:
    try:
        compiler = require_path(COMPILER, "release compiler")
        require_path(RUNTIME, "RISC-V SysY runtime")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="yoolang-rvv-diff-") as temp:
            directory = Path(temp)
            source = directory / "add_bias.sy"
            source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
            scalar_assembly, scalar = compile_and_link(
                compiler,
                gcc,
                source,
                directory,
                name="scalar",
                march="rv64gc",
                optimization=1,
            )
            rvv_assembly, rvv = compile_and_link(
                compiler,
                gcc,
                source,
                directory,
                name="rvv",
                march="rv64gcv",
                optimization=2,
            )

            scalar_text = scalar_assembly.read_text(encoding="utf-8")
            if any(mnemonic in scalar_text for mnemonic in ("vset", "vle", "vse", "vadd")):
                raise RuntimeError("rv64gc scalar assembly unexpectedly contains RVV mnemonics")
            rvv_disassembly = run_checked([objdump, "-d", str(rvv)]).stdout.decode()
            rvv_main_body = disassembled_symbol(rvv_disassembly, "main")
            for mnemonic in ("vsetvli", "vle32.v", "vadd.vv", "vse32.v"):
                if mnemonic not in rvv_disassembly:
                    raise RuntimeError(f"RVV differential binary lacks {mnemonic}")
                if mnemonic not in rvv_main_body:
                    raise RuntimeError(f"executed RVV main path lacks {mnemonic}")
            # Also ensure the compiler output itself is genuine final assembly,
            # not an objdump-only artifact introduced by the linker.
            rvv_text = rvv_assembly.read_text(encoding="utf-8")
            for mnemonic in ("vsetvli", "vle32.v", "vadd.vv", "vse32.v"):
                if mnemonic not in rvv_text:
                    raise RuntimeError(f"RVV compiler assembly lacks {mnemonic}")
            print("PASS rvv_differential_instruction_shape")

            all_cases = cases()
            scalar_outputs: list[bytes] = []
            for case_index, values in enumerate(all_cases):
                input_bytes = encode_case(values)
                output = run_checked(
                    [qemu, "-cpu", "rv64,v=false", str(scalar)],
                    input_bytes=input_bytes,
                    timeout=30.0,
                ).stdout
                oracle = expected_output(values)
                if output != oracle:
                    raise RuntimeError(
                        f"scalar/oracle mismatch in case {case_index}, n={len(values)}\n"
                        f"expected={oracle[:512]!r}\nactual={output[:512]!r}"
                    )
                scalar_outputs.append(output)
            print(f"PASS rvv_differential_scalar_oracle_{len(all_cases)}_cases")

            for vlen in VLENS:
                cpu = f"rv64,v=true,vlen={vlen},elen=64"
                for case_index, values in enumerate(all_cases):
                    output = run_checked(
                        [qemu, "-cpu", cpu, str(rvv)],
                        input_bytes=encode_case(values),
                        timeout=30.0,
                    ).stdout
                    if output != scalar_outputs[case_index]:
                        raise RuntimeError(
                            f"scalar/RVV mismatch for VLEN={vlen}, case={case_index}, "
                            f"n={len(values)}\nscalar={scalar_outputs[case_index][:512]!r}\n"
                            f"rvv={output[:512]!r}"
                        )
                print(f"PASS rvv_differential_vlen_{vlen}_{len(all_cases)}_cases")

            # Compile a kernel-only module and call it from a separate C main.
            # This prevents the scalar inliner from replacing the executed call
            # while leaving a dead, never-run vectorized function in the binary.
            slp_source = directory / "slp_kernel.sy"
            slp_source.write_text(textwrap.dedent(SLP_SOURCE), encoding="utf-8")
            slp_driver = directory / "slp_driver.c"
            slp_driver.write_text(textwrap.dedent(SLP_DRIVER), encoding="utf-8")
            slp_scalar_assembly, slp_scalar = compile_and_link(
                compiler,
                gcc,
                slp_source,
                directory,
                name="slp_scalar",
                march="rv64gc",
                optimization=2,
                driver=slp_driver,
            )
            slp_rvv_assembly, slp_rvv = compile_and_link(
                compiler,
                gcc,
                slp_source,
                directory,
                name="slp_rvv",
                march="rv64gcv",
                optimization=3,
                driver=slp_driver,
            )
            if "vset" in slp_scalar_assembly.read_text(encoding="utf-8"):
                raise RuntimeError("scalar SLP control binary unexpectedly contains RVV")
            slp_text = slp_rvv_assembly.read_text(encoding="utf-8")
            for mnemonic in ("vsetivli", "vle32.v", "vadd.vv", "vse32.v"):
                if mnemonic not in slp_text:
                    raise RuntimeError(f"SLP RVV compiler assembly lacks {mnemonic}")
            slp_disassembly = run_checked([objdump, "-d", str(slp_rvv)]).stdout.decode()
            slp_kernel_body = disassembled_symbol(slp_disassembly, "four_lane_add")
            for mnemonic in ("vsetivli", "vle32.v", "vadd.vv", "vse32.v"):
                if mnemonic not in slp_kernel_body:
                    raise RuntimeError(
                        f"executed SLP kernel four_lane_add lacks {mnemonic}"
                    )
            if "four_lane_add" not in disassembled_symbol(slp_disassembly, "main"):
                raise RuntimeError("SLP C driver main does not call four_lane_add")
            print("PASS rvv_slp_executed_kernel_instruction_shape")
            all_slp_cases = slp_cases()
            slp_scalar_outputs: list[bytes] = []
            for case_index, values in enumerate(all_slp_cases):
                output = run_checked(
                    [qemu, "-cpu", "rv64,v=false", str(slp_scalar)],
                    input_bytes=encode_case(values),
                    timeout=30.0,
                ).stdout
                oracle = expected_output(values)
                if output != oracle:
                    raise RuntimeError(
                        f"SLP scalar/oracle mismatch in case {case_index}: "
                        f"expected={oracle!r}, actual={output!r}"
                    )
                slp_scalar_outputs.append(output)
            print(f"PASS rvv_slp_scalar_oracle_{len(all_slp_cases)}_cases")
            for vlen in VLENS:
                cpu = f"rv64,v=true,vlen={vlen},elen=64"
                for case_index, values in enumerate(all_slp_cases):
                    output = run_checked(
                        [qemu, "-cpu", cpu, str(slp_rvv)],
                        input_bytes=encode_case(values),
                        timeout=30.0,
                    ).stdout
                    if output != slp_scalar_outputs[case_index]:
                        raise RuntimeError(
                            f"SLP scalar/RVV mismatch for VLEN={vlen}, case={case_index}: "
                            f"scalar={slp_scalar_outputs[case_index]!r}, rvv={output!r}"
                        )
                print(f"PASS rvv_slp_vlen_{vlen}_{len(all_slp_cases)}_cases")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_differential: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
