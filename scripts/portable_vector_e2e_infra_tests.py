#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
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
MARCH = "rv64gc"
MABI = "lp64d"


KERNEL_SOURCE = r"""
int portable_vector_kernel(int lane, int seed) {
  if (lane < 0 || lane >= 3) {
    return 90;
  }

  vector<int,1> one = vector<int,1>{7};
  vector<int,1> one_splat = vector<int,1>(seed);
  vector<int,1> one_sum = one + one_splat;
  if (one_sum[0] != seed + 7) {
    return 1;
  }

  vector<int,3> base = vector<int,3>{1,2,3};
  vector<int,3> splat = vector<int,3>(seed);
  vector<int,3> math = base + splat;
  mask<3> compared = math > vector<int,3>(seed + 1);
  mask<3> bits = (compared & mask<3>{1,1,0}) |
                 (compared ^ mask<3>{0,0,1});
  mask<3> flipped = ~bits;
  if (bits[0] != 0 || bits[1] != 1 || bits[2] != 0) {
    return 2;
  }
  if (flipped[0] != 1 || flipped[1] != 0 || flipped[2] != 1) {
    return 3;
  }
  if (any(bits) != 1 || all(bits) != 0 || none(bits) != 0) {
    return 4;
  }

  vector<int,3> selected = select(bits, math, base);
  if (selected[0] != 1 || selected[1] != seed + 2 || selected[2] != 3) {
    return 5;
  }

  vector<int,3> constant_insert = insert_lane(base, 1, 20);
  if (extract_lane(constant_insert, 1) != 20) {
    return 6;
  }
  vector<int,3> dynamic_insert = insert_lane(constant_insert, lane, seed + 30);
  if (extract_lane(dynamic_insert, lane) != seed + 30) {
    return 7;
  }

  vector<int,3> shuffled =
      shuffle(base, splat, vector<int,3>{2,3,5});
  if (shuffled[0] != 3 || shuffled[1] != seed || shuffled[2] != seed) {
    return 8;
  }

  vector<int,7> integers = vector<int,7>{1,-2,3,-4,5,-6,7};
  vector<float,7> floats = vector<float,7>(integers);
  vector<float,7> shifted = floats + vector<float,7>(0.5);
  vector<int,7> converted = vector<int,7>(shifted);
  if (converted[0] != 1 || converted[1] != -1 ||
      converted[5] != -5 || converted[6] != 7) {
    return 9;
  }
  mask<7> positive = shifted > vector<float,7>(0.0);
  vector<float,7> positive_values =
      select(positive, shifted, vector<float,7>(0.0));
  vector<int,7> positive_integers = vector<int,7>(positive_values);
  if (positive[0] != 1 || positive[1] != 0 || positive[6] != 1 ||
      positive_integers[0] != 1 || positive_integers[1] != 0 ||
      positive_integers[6] != 7) {
    return 10;
  }

  vector<int,31> wide_zero = vector<int,31>{};
  vector<int,31> wide_seed = vector<int,31>(seed);
  vector<int,31> wide_math = (wide_seed | wide_zero) ^ wide_zero;
  vector<int,31> wide_not = ~wide_zero;
  mask<31> wide_equal = wide_math == vector<int,31>(seed);
  if (wide_math[0] != seed || wide_math[7] != seed ||
      wide_math[30] != seed || wide_not[30] != -1 || all(wide_equal) != 1) {
    return 11;
  }
  return 0;
}
"""


DRIVER_SOURCE = r"""
#include <stdint.h>

extern int32_t portable_vector_kernel(int32_t lane, int32_t seed);

int main(void) {
    static const int32_t seeds[] = {-19, -1, 0, 7, 101};
    for (int32_t lane = 0; lane < 3; ++lane) {
        for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
            int32_t result = portable_vector_kernel(lane, seeds[i]);
            if (result != 0) {
                return (int)(result + lane * 20 + (int32_t)i);
            }
        }
    }
    return 0;
}
"""


OBJECT_SOURCES = {
    "global": r"""
        vector<int,3> values = vector<int,3>{1,2,3};
        int main() {
          if (values[0] != 1 || values[1] != 2 || values[2] != 3) return 1;
          values = vector<int,3>{4,5,6};
          if (values[0] != 4 || values[1] != 5 || values[2] != 6) return 2;
          return 0;
        }
    """,
    "signature": r"""
        vector<int,3> identity(vector<int,3> value) { return value; }
        int main() {
          vector<int,3> value = identity(vector<int,3>{7,8,9});
          if (value[0] != 7 || value[1] != 8 || value[2] != 9) return 1;
          return 0;
        }
    """,
    "object": r"""
        int main() {
          vector<int,3> rows[2] = {{1,2,3}, {4,5,6}};
          rows[0] = vector<int,3>{7,8,9};
          if (rows[0][0] != 7 || rows[0][1] != 8 || rows[0][2] != 9) return 1;
          if (rows[1][0] != 4 || rows[1][1] != 5 || rows[1][2] != 6) return 2;
          return 0;
        }
    """,
}


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"required {description} not found: {path}")
    return path


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required portable-vector e2e tool not found: {name}")
    return path


def run_checked(
    command: list[str],
    *,
    timeout: float = 60.0,
) -> subprocess.CompletedProcess[str]:
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


def compiler_command(source: Path, output: Path, *options: str) -> list[str]:
    return [
        str(COMPILER),
        str(source),
        *options,
        f"-march={MARCH}",
        f"-mabi={MABI}",
        "-o",
        str(output),
    ]


def instruction_mnemonics(assembly: str) -> list[str]:
    result: list[str] = []
    for line in assembly.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((".", "#")) or stripped.endswith(":"):
            continue
        token = stripped.split(None, 1)[0].lower()
        if re.fullmatch(r"[a-z][a-z0-9_.]*", token):
            result.append(token)
    return result


def disassembly_mnemonics(disassembly: str) -> list[str]:
    result: list[str] = []
    pattern = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]*)\b")
    for line in disassembly.splitlines():
        match = pattern.match(line.lower())
        if match is not None:
            result.append(match.group(1))
    return result


def require_no_rvv(mnemonics: list[str], context: str) -> None:
    if not mnemonics:
        raise RuntimeError(f"{context} produced no recognizable instruction mnemonics")
    vector = sorted({mnemonic for mnemonic in mnemonics if mnemonic.startswith("v")})
    if vector:
        raise RuntimeError(f"{context} contains RVV mnemonics: {', '.join(vector)}")


def require_scalar_mir(text: str) -> None:
    forbidden = (":vr", ":vmask", "fv<", "VSET", "RVV")
    present = [spelling for spelling in forbidden if spelling in text]
    if present:
        raise RuntimeError("portable MIR retained vector state: " + ", ".join(present))
    if "func @portable_vector_kernel" not in text:
        raise RuntimeError("portable MIR lacks the executed scalar-signature kernel")


def require_typed_vector_oir(text: str) -> None:
    required = (
        "<1 x i32>",
        "<3 x i32>",
        "<7 x float>",
        "<31 x i32>",
        "splat i32",
        "extractelement",
        "insertelement",
        "shufflevector",
        "vector.sitofp",
        "vector.fptosi",
        "icmp gt <3 x i32>",
        "fcmp gt <7 x float>",
        "and <3 x i1>",
        "xor <3 x i1>",
        "or <3 x i1>",
        "select <3 x i1>",
        "i32 %lane.arg",
    )
    missing = [spelling for spelling in required if spelling not in text]
    if missing:
        raise RuntimeError(
            "RVV-target OIR control did not retain expected source vectors: "
            + ", ".join(missing)
        )


def require_scalar_attributes(readelf: str, context: str) -> None:
    lowered = readelf.lower()
    arch_lines = [line for line in lowered.splitlines() if "tag_riscv_arch" in line]
    if not arch_lines:
        raise RuntimeError(f"{context} lacks Tag_RISCV_arch")
    arch = " ".join(arch_lines)
    if "_v" in arch or "zve" in arch:
        raise RuntimeError(f"{context} unexpectedly advertises a vector ISA: {arch}")


def compile_and_run_object_case(
    source: Path,
    directory: Path,
    name: str,
    assembler: str,
    gcc: str,
    objdump: str,
    readelf: str,
    qemu: str,
) -> None:
    for opt_level in range(4):
        assembly = directory / f"abi-{name}-O{opt_level}.s"
        run_checked(compiler_command(source, assembly, "-S", f"-O{opt_level}"))
        assembly_text = assembly.read_text(encoding="utf-8")
        if '.attribute arch, "rv64gc"' not in assembly_text:
            raise RuntimeError(
                f"portable aggregate object {name} O{opt_level} lacks rv64gc attribute"
            )
        require_no_rvv(
            instruction_mnemonics(assembly_text),
            f"portable aggregate object {name} O{opt_level} assembly",
        )

        object_file = directory / f"abi-{name}-O{opt_level}.o"
        run_checked(
            [
                assembler,
                f"-march={MARCH}",
                f"-mabi={MABI}",
                str(assembly),
                "-o",
                str(object_file),
            ]
        )
        require_scalar_attributes(
            run_checked([readelf, "-A", str(object_file)]).stdout,
            f"portable aggregate object {name} O{opt_level}",
        )

        executable = directory / f"abi-{name}-O{opt_level}"
        run_checked(
            [
                gcc,
                "-static",
                f"-march={MARCH}",
                f"-mabi={MABI}",
                "-mcmodel=medany",
                str(object_file),
                "-o",
                str(executable),
            ]
        )
        require_scalar_attributes(
            run_checked([readelf, "-A", str(executable)]).stdout,
            f"portable aggregate executable {name} O{opt_level}",
        )
        require_no_rvv(
            disassembly_mnemonics(
                run_checked([objdump, "-d", str(executable)]).stdout
            ),
            f"portable aggregate executable {name} O{opt_level}",
        )
        run_checked([qemu, "-cpu", "rv64,v=false", str(executable)], timeout=30.0)
        print(f"PASS portable_vector_aggregate_abi_{name}_O{opt_level}")


def main() -> int:
    try:
        require_file(COMPILER, "release compiler")
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        readelf = require_tool("riscv64-linux-gnu-readelf")
        qemu = require_tool("qemu-riscv64")

        with tempfile.TemporaryDirectory(prefix="yoolang-portable-vector-e2e-") as temp:
            directory = Path(temp)
            kernel_source = directory / "portable_vector_kernel.sy"
            driver_source = directory / "portable_vector_driver.c"
            kernel_source.write_text(textwrap.dedent(KERNEL_SOURCE), encoding="utf-8")
            driver_source.write_text(textwrap.dedent(DRIVER_SOURCE), encoding="utf-8")

            vector_oir = directory / "vector-control.oir"
            vector_command = [
                str(COMPILER),
                str(kernel_source),
                "--emit-oir",
                "-O0",
                "-march=rv64gcv",
                f"-mabi={MABI}",
                "-o",
                str(vector_oir),
            ]
            run_checked(vector_command)
            require_typed_vector_oir(vector_oir.read_text(encoding="utf-8"))

            portable_oir = directory / "portable-target-independent.oir"
            run_checked(compiler_command(kernel_source, portable_oir, "--emit-oir", "-O0"))
            require_typed_vector_oir(portable_oir.read_text(encoding="utf-8"))
            print("PASS portable_vector_frontend_typed_oir_boundary")

            scalar_mir = directory / "portable.mir"
            run_checked(compiler_command(kernel_source, scalar_mir, "--emit-mir", "-O0"))
            mir_text = scalar_mir.read_text(encoding="utf-8")
            if "portable_vector_kernel" not in mir_text or not mir_text.strip():
                raise RuntimeError("portable MIR is empty or lacks the executed kernel")
            require_scalar_mir(mir_text)
            print("PASS portable_vector_scalar_mir")

            assembly = directory / "portable.s"
            run_checked(compiler_command(kernel_source, assembly, "-S", "-O0"))
            assembly_text = assembly.read_text(encoding="utf-8")
            if '.attribute arch, "rv64gc"' not in assembly_text:
                raise RuntimeError("final compiler assembly does not advertise rv64gc")
            require_no_rvv(instruction_mnemonics(assembly_text), "compiler assembly")

            kernel_object = directory / "portable.o"
            run_checked(
                [
                    assembler,
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    str(assembly),
                    "-o",
                    str(kernel_object),
                ]
            )
            require_scalar_attributes(
                run_checked([readelf, "-A", str(kernel_object)]).stdout,
                "GNU-as kernel object",
            )
            print("PASS portable_vector_final_asm_gnu_as_rv64gc")

            driver_object = directory / "driver.o"
            run_checked(
                [
                    gcc,
                    "-O2",
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    "-c",
                    str(driver_source),
                    "-o",
                    str(driver_object),
                ]
            )
            executable = directory / "portable-vector-e2e"
            run_checked(
                [
                    gcc,
                    "-static",
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    "-mcmodel=medany",
                    str(kernel_object),
                    str(driver_object),
                    "-o",
                    str(executable),
                ]
            )
            require_scalar_attributes(
                run_checked([readelf, "-A", str(executable)]).stdout,
                "linked executable",
            )
            disassembly = run_checked([objdump, "-d", str(executable)]).stdout
            require_no_rvv(disassembly_mnemonics(disassembly), "linked executable")
            if "<portable_vector_kernel>" not in disassembly:
                raise RuntimeError("linked executable lacks portable_vector_kernel")
            run_checked([qemu, "-cpu", "rv64,v=false", str(executable)], timeout=30.0)
            print("PASS portable_vector_qemu_rv64_v_false_15_cases")

            for name, source_text in OBJECT_SOURCES.items():
                source = directory / f"abi_{name}.sy"
                source.write_text(textwrap.dedent(source_text), encoding="utf-8")
                compile_and_run_object_case(
                    source, directory, name, assembler, gcc, objdump, readelf, qemu
                )

            aggregate_gate = run_checked(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "standard_vector_aggregate_abi_e2e.py"),
                    "--compiler",
                    str(COMPILER),
                    "--march",
                    MARCH,
                ],
                timeout=300.0,
            )
            print(aggregate_gate.stdout, end="")
            print("PASS portable_vector_standard_aggregate_abi_matrix")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL portable_vector_e2e: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
