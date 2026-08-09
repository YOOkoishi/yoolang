#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
VLENS = (128, 256, 512, 1024)


KERNEL = r"""
void add_bias_guarded(int values[], int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + 1;
  }
}
"""


HARNESS = r"""
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

extern void add_bias_guarded(int32_t *values, int32_t n);

int main(void) {
    static const int32_t lengths[] = {
        0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17,
        31, 32, 33, 63, 64, 65, 127, 257
    };
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 10;
    uint8_t *mapping = (uint8_t *)mmap(
        0, (size_t)page_size * 2U, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 11;
    if (mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) != 0) return 12;

    for (unsigned case_index = 0;
         case_index < sizeof(lengths) / sizeof(lengths[0]); ++case_index) {
        const int32_t n = lengths[case_index];
        int32_t *values = (int32_t *)(mapping + page_size) - n;
        for (int32_t lane = 0; lane < n; ++lane) {
            values[lane] = (int32_t)(lane * 17 - (int32_t)case_index * 31);
        }
        add_bias_guarded(values, n);
        for (int32_t lane = 0; lane < n; ++lane) {
            const int32_t expected = (int32_t)(lane * 17 - (int32_t)case_index * 31 + 7);
            if (values[lane] != expected) return 20 + (int)case_index;
        }
    }
    return munmap(mapping, (size_t)page_size * 2U) == 0 ? 0 : 60;
}
"""


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required guard-page test tool not found: {name}")
    return path


def run_checked(command: list[str], *, timeout: float = 60.0) -> subprocess.CompletedProcess[str]:
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
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def build_kernel(
    compiler: Path,
    gcc: str,
    source: Path,
    harness: Path,
    directory: Path,
    *,
    name: str,
    march: str,
    optimization: int,
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
    run_checked(
        [
            gcc,
            "-O2",
            "-static",
            f"-march={march}",
            "-mabi=lp64d",
            "-mcmodel=medany",
            str(assembly),
            str(harness),
            "-o",
            str(executable),
        ]
    )
    return assembly, executable


def main() -> int:
    try:
        if not COMPILER.is_file():
            raise RuntimeError(f"release compiler not found: {COMPILER}")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="yoolang-rvv-guard-") as temp:
            directory = Path(temp)
            source = directory / "guard.sy"
            harness = directory / "guard.c"
            source.write_text(textwrap.dedent(KERNEL), encoding="utf-8")
            harness.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
            scalar_assembly, scalar = build_kernel(
                COMPILER,
                gcc,
                source,
                harness,
                directory,
                name="guard_scalar",
                march="rv64gc",
                optimization=1,
            )
            rvv_assembly, rvv = build_kernel(
                COMPILER,
                gcc,
                source,
                harness,
                directory,
                name="guard_rvv",
                march="rv64gcv",
                optimization=2,
            )
            if "vset" in scalar_assembly.read_text(encoding="utf-8"):
                raise RuntimeError("scalar guard-page control unexpectedly contains RVV")
            rvv_text = rvv_assembly.read_text(encoding="utf-8")
            for mnemonic in ("vsetvli", "vle32.v", "vadd.vv", "vse32.v"):
                if mnemonic not in rvv_text:
                    raise RuntimeError(f"RVV guard-page kernel lacks {mnemonic}")

            run_checked([qemu, "-cpu", "rv64,v=false", str(scalar)], timeout=30.0)
            print("PASS rvv_guard_page_scalar_control")
            for vlen in VLENS:
                run_checked(
                    [qemu, "-cpu", f"rv64,v=true,vlen={vlen},elen=64", str(rvv)],
                    timeout=30.0,
                )
                print(f"PASS rvv_guard_page_vlen_{vlen}")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_guard_page: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
