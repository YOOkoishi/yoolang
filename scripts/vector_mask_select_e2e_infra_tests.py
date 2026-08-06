#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
VLENS = (128, 256, 512, 1024)


SOURCE = r"""
int mask_select_kernel(int seed) {
  vector<int,7> lanes = iota(vector<int,7>{});
  mask<7> condition = lanes < vector<int,7>(seed);
  mask<7> even = mask<7>{1,0,1,0,1,0,1};
  mask<7> selected = select(condition, even, ~even);
  return selected[0] | (selected[1] * 2) | (selected[2] * 4) |
         (selected[3] * 8) | (selected[4] * 16) |
         (selected[5] * 32) | (selected[6] * 64);
}
"""


DRIVER = r"""
#include <stdint.h>

extern int32_t mask_select_kernel(int32_t);

static int32_t oracle(int32_t seed) {
    int32_t result = 0;
    for (int32_t lane = 0; lane < 7; ++lane) {
        int32_t even = (lane & 1) == 0;
        int32_t value = lane < seed ? even : !even;
        result |= value << lane;
    }
    return result;
}

int main(void) {
    static const int32_t seeds[] = {-3, 0, 1, 3, 7, 11};
    for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        if (mask_select_kernel(seeds[i]) != oracle(seeds[i]))
            return (int)(i + 1);
    }
    return 0;
}
"""


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def checked(command: list[str], description: str, timeout: float = 60.0) -> str:
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
            f"{description} failed ({result.returncode})\n"
            f"command: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def decoded_vector_mnemonics(disassembly: str) -> list[str]:
    return re.findall(
        r"(?m)^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s+)+"
        r"(v[a-z][a-z0-9_.]*)\b",
        disassembly,
    )


def main() -> int:
    compiler = COMPILER.resolve()
    if not compiler.is_file():
        raise RuntimeError(f"compiler not found: {compiler}")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    qemu = require_tool("qemu-riscv64")

    with tempfile.TemporaryDirectory(prefix="yoolang-mask-select-") as directory:
        work = Path(directory)
        source = work / "mask_select.sy"
        driver = work / "driver.c"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        driver.write_text(textwrap.dedent(DRIVER), encoding="utf-8")

        binaries: dict[str, Path] = {}
        for march in ("rv64gc", "rv64gcv"):
            assembly = work / f"mask_select.{march}.s"
            executable = work / f"mask_select.{march}"
            checked(
                [
                    str(compiler),
                    "-S",
                    "-O0",
                    f"-march={march}",
                    "-mabi=lp64d",
                    "-o",
                    str(assembly),
                    str(source),
                ],
                f"compile mask select for {march}",
            )
            assembly_text = assembly.read_text(encoding="utf-8")
            if f'\t.attribute arch, "{march}"\n' not in assembly_text:
                raise RuntimeError(f"{march} assembly has the wrong architecture attribute")
            if march == "rv64gcv" and "\tvmerge.vvm " not in assembly_text:
                raise RuntimeError("RVV mask select did not lower to a real vmerge.vvm")
            checked(
                [
                    gcc,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-static",
                    f"-march={march}",
                    "-mabi=lp64d",
                    str(assembly),
                    str(driver),
                    "-o",
                    str(executable),
                ],
                f"assemble and link mask select for {march}",
            )
            decoded = decoded_vector_mnemonics(
                checked([objdump, "-d", str(executable)], f"objdump {march}")
            )
            if march == "rv64gc" and decoded:
                raise RuntimeError(
                    "portable mask select executable contains RVV instructions: "
                    + ", ".join(decoded[:8])
                )
            if march == "rv64gcv" and not decoded:
                raise RuntimeError("RVV mask select executable has no decoded RVV instruction")
            binaries[march] = executable

        checked(
            [qemu, "-cpu", "rv64,v=false", str(binaries["rv64gc"])],
            "execute portable mask select with V disabled",
        )
        print("PASS mask_select_rv64gc_no_rvv")
        for vlen in VLENS:
            checked(
                [
                    qemu,
                    "-cpu",
                    f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                    str(binaries["rv64gcv"]),
                ],
                f"execute RVV mask select at VLEN={vlen}",
            )
            print(f"PASS mask_select_rvv_vlen{vlen}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
