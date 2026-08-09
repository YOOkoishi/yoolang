#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


VLENS = (128, 256, 512, 1024)


PROBE = r"""
#include <stddef.h>
#include <stdint.h>

static volatile uint32_t output[32];

int main(void) {
    size_t avl = 1000;
    size_t vl = 0;
    __asm__ volatile(
        "vsetvli %0, %1, e32, m1, ta, ma\n\t"
        "vmv.v.i v1, 1\n\t"
        "vmv.v.i v2, 2\n\t"
        "vadd.vv v3, v1, v2\n\t"
        "vse32.v v3, (%2)"
        : "=r"(vl)
        : "r"(avl), "r"(output)
        : "memory");
    if (vl == 0 || vl > 32) return 1;
    for (size_t lane = 0; lane < vl; ++lane) {
        if (output[lane] != 3) return 2;
    }
    return 0;
}
"""


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required RVV test tool not found: {name}")
    return path


def run_checked(command: list[str], *, timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
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


def main() -> int:
    try:
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        readelf = require_tool("riscv64-linux-gnu-readelf")
        qemu = require_tool("qemu-riscv64")
        with tempfile.TemporaryDirectory(prefix="rvv-toolchain-") as temp:
            directory = Path(temp)
            source = directory / "probe.c"
            executable = directory / "probe"
            source.write_text(textwrap.dedent(PROBE), encoding="utf-8")
            run_checked(
                [
                    gcc,
                    "-O2",
                    "-static",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    str(source),
                    "-o",
                    str(executable),
                ]
            )

            disassembly = run_checked([objdump, "-d", str(executable)]).stdout
            for mnemonic in ("vsetvli", "vadd.vv", "vse32.v"):
                if mnemonic not in disassembly:
                    raise RuntimeError(f"RVV probe disassembly lacks {mnemonic}")
            attributes = run_checked([readelf, "-A", str(executable)]).stdout.lower()
            if "tag_riscv_arch" not in attributes or "v1p0" not in attributes:
                raise RuntimeError("RVV probe ELF attributes do not advertise V 1.0")
            print("PASS rvv_assemble_objdump_readelf")

            for vlen in VLENS:
                cpu = f"rv64,v=true,vlen={vlen},elen=64"
                run_checked([qemu, "-cpu", cpu, str(executable)])
                print(f"PASS rvv_qemu_vlen_{vlen}")

            disabled = subprocess.run(
                [qemu, "-cpu", "rv64,v=false", str(executable)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
                timeout=10.0,
            )
            if disabled.returncode == 0:
                raise RuntimeError("RVV probe unexpectedly executed with V disabled")
            print("PASS rvv_qemu_v_disabled_traps")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL rvv_toolchain: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
