#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap

from portable_vector_e2e_infra_tests import (
    DRIVER_SOURCE as FIXED_DRIVER_SOURCE,
    KERNEL_SOURCE as FIXED_KERNEL_SOURCE,
)


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER = ROOT / "build/linux/x86_64/release/compiler"
VLENS = (128, 256, 512, 1024)


SOURCE = r"""
int values[259] = {};

void fill(int n) {
  int i = 0;
  while (i < n) {
    values[i] = i + 3;
    i = i + 1;
  }
}

void add_bias(int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 5;
    i = i + 1;
  }
}

int recursive_sum(int n) {
  if (n <= 0) { return 0; }
  return n + recursive_sum(n - 1);
}

int run_case(int n) {
  fill(n);
  add_bias(n);
  return values[0] + values[n - 1] + recursive_sum(4);
}

int abi_int_probe(int p0, int p1, int p2, int p3, int p4,
                  int p5, int p6, int p7, int p8, int p9) {
  return p0 + p7 + p8 + p9;
}

float abi_float_probe(float p0, float p1, float p2, float p3, float p4,
                      float p5, float p6, float p7, float p8, float p9) {
  return p0 + p7 + p8 + p9;
}

int pointer_probe(int data[], int index) {
  return data[index];
}
"""


HARNESS = r"""
#include <math.h>

extern int run_case(int);
extern int abi_int_probe(int, int, int, int, int, int, int, int, int, int);
extern float abi_float_probe(float, float, float, float, float,
                             float, float, float, float, float);
extern int pointer_probe(int *, int);
#ifdef EXPECT_REAL_DETECTOR
extern int __yoolang_rvv_available(void);
#endif

int main(void) {
#ifdef EXPECT_REAL_DETECTOR
  /* QEMU currently advertises V but rejects PR_RISCV_V_GET_CONTROL with
     EINVAL, so the production detector must fail closed. */
  if (__yoolang_rvv_available() != 0) return 90;
#endif
  if (run_case(37) != 62) return 1;
  if (abi_int_probe(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 28) return 2;
  float value = abi_float_probe(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  if (fabsf(value - 28.0f) > 0.001f) return 3;
  int data[3] = {17, 19, 23};
  if (pointer_probe(data, 2) != 23) return 4;
  return 0;
}
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RVV fat multiversion end-to-end gate")
    parser.add_argument(
        "--compiler",
        type=Path,
        default=Path(os.environ.get("YOOLANG_COMPILER", DEFAULT_COMPILER)),
    )
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def require_tool(name: str) -> str:
    tool = shutil.which(name)
    if tool is None:
        raise RuntimeError(f"required tool not found in PATH: {name}")
    return tool


def run(
    command: list[str], *, timeout: float = 60.0
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def checked(command: list[str], description: str, *, timeout: float = 60.0) -> str:
    result = run(command, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(
            f"{description} failed ({result.returncode})\n"
            f"command: {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def expect_failure(command: list[str], needle: str, description: str) -> None:
    result = run(command)
    require(result.returncode != 0, f"{description} unexpectedly succeeded")
    combined = result.stdout + result.stderr
    require(needle in combined, f"{description} lost diagnostic {needle!r}:\n{combined}")


def qemu_cpu(vlen: int | None) -> str:
    if vlen is None:
        return "rv64,v=false"
    return f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0"


def link(
    gcc: str,
    obj: Path,
    harness: Path,
    output: Path,
    extras: list[Path],
    extra_flags: list[str] | None = None,
) -> None:
    checked(
        [
            gcc,
            "-std=c11",
            "-O2",
            "-static",
            "-march=rv64gc",
            "-mabi=lp64d",
            *(extra_flags or []),
            str(obj),
            str(harness),
            *(str(path) for path in extras),
            "-o",
            str(output),
        ],
        f"link {output.name}",
    )


def run_qemu(
    qemu: str,
    executable: Path,
    vlen: int | None,
    description: str,
    arguments: list[str] | None = None,
) -> None:
    checked(
        [qemu, "-cpu", qemu_cpu(vlen), str(executable), *(arguments or [])],
        description,
    )


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    require(compiler.is_file(), f"compiler not found: {compiler}")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    readelf = require_tool("riscv64-linux-gnu-readelf")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    qemu = require_tool("qemu-riscv64")

    with tempfile.TemporaryDirectory(prefix="yoolang-fat-rvv-") as directory:
        work = Path(directory)
        source = work / "fat.sy"
        harness = work / "harness.c"
        zero_stub = work / "detector_zero.c"
        one_stub = work / "detector_one.c"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        harness.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        zero_stub.write_text(
            "int __yoolang_rvv_available(void) { return 0; }\n", encoding="utf-8"
        )
        one_stub.write_text(
            "int __yoolang_rvv_available(void) { return 1; }\n", encoding="utf-8"
        )

        assembly_path = work / "fat.s"
        checked(
            [
                str(compiler),
                "-S",
                "-O2",
                "-mrvv-deployment=fat",
                "-o",
                str(assembly_path),
                str(source),
            ],
            "fat compilation",
        )
        assembly = assembly_path.read_text(encoding="utf-8")
        require(
            assembly.startswith('\t.attribute arch, "rv64gc"\n'),
            "fat assembly does not start with the rv64gc baseline attribute",
        )
        require(
            assembly.count(".attribute arch") == 1,
            "fat assembly leaked the RVV branch architecture attribute",
        )
        require("\t.option push\n\t.option arch, +v\n" in assembly, "missing local +v scope")
        require("\t.option pop\n" in assembly, "missing local +v scope pop")
        baseline, vector_and_dispatch = assembly.split("\t.option push\n", 1)
        vector_text, dispatch_text = vector_and_dispatch.split("\t.option pop\n", 1)
        require(
            not re.search(r"(?m)^\s*v[a-z][a-z0-9_.]*\s", baseline),
            "scalar variant region contains an RVV instruction",
        )
        require(re.search(r"(?m)^\s*vset(?:i?vli|vl)\s", vector_text) is not None,
                "auto-vectorized RVV variant contains no vsetvl instruction")
        require(
            not re.search(r"(?m)^\s*v[a-z][a-z0-9_.]*\s", dispatch_text),
            "baseline dispatcher region contains an RVV instruction",
        )
        require("\t.weak __yoolang_rvv_available\n" in dispatch_text,
                "dispatcher does not use the weak detector contract")
        for name in (
            "fill",
            "add_bias",
            "recursive_sum",
            "run_case",
            "abi_int_probe",
            "abi_float_probe",
            "pointer_probe",
        ):
            require(f"__yoolang_scalar_{name}" in assembly, f"missing scalar variant for {name}")
            require(f"__yoolang_rvv_{name}" in assembly, f"missing RVV variant for {name}")
            require(re.search(rf"(?m)^{re.escape(name)}:$", assembly) is not None,
                    f"missing public dispatcher for {name}")
        require("%pcrel_hi(__yoolang_scalar_recursive_sum)" in baseline,
                "scalar recursion was not rewritten to the scalar variant")
        require("%pcrel_hi(__yoolang_rvv_recursive_sum)" in vector_text,
                "RVV recursion was not rewritten to the RVV variant")
        require("%pcrel_hi(recursive_sum)" not in assembly,
                "a variant recursively calls the public dispatcher")
        variant_globals = re.findall(
            r"(?m)^\s*\.globl\s+(\S*__yoolang_(?:scalar|rvv)_\S*)$", assembly
        )
        require(variant_globals, "no private variants were emitted")
        for name in variant_globals:
            require(f"\t.hidden {name}\n" in assembly, f"variant is not hidden: {name}")

        obj = work / "fat.o"
        checked(
            [
                gcc,
                "-c",
                "-march=rv64gc",
                "-mabi=lp64d",
                str(assembly_path),
                "-o",
                str(obj),
            ],
            "assemble fat object under rv64gc",
        )
        attributes = checked([readelf, "-A", str(obj)], "read fat object attributes")
        arch_line = next(
            (line for line in attributes.splitlines() if "Tag_RISCV_arch:" in line), ""
        )
        require(arch_line and not re.search(r"(?:^|_)v(?:\d|$)", arch_line),
                f"fat object baseline attribute advertises V: {arch_line}")
        symbols = checked([readelf, "-Ws", str(obj)], "read fat object symbols")
        for name in ("__yoolang_scalar_run_case", "__yoolang_rvv_run_case"):
            require(
                re.search(rf"(?m)\bGLOBAL\s+HIDDEN\s+\S+\s+{re.escape(name)}$", symbols)
                is not None,
                f"private variant lacks GLOBAL HIDDEN visibility: {name}",
            )
        require(
            re.search(r"(?m)\bGLOBAL\s+DEFAULT\s+\S+\s+run_case$", symbols) is not None,
            "public source symbol does not retain GLOBAL DEFAULT visibility",
        )
        disassembly = checked([objdump, "-dr", str(obj)], "disassemble fat object")
        require(re.search(r"\bvset(?:i?vli|vl)\b", disassembly) is not None,
                "fat object has no decoded RVV instructions")
        for prefix in ("__yoolang_scalar_", ""):
            for name in (
                "fill",
                "add_bias",
                "recursive_sum",
                "run_case",
                "abi_int_probe",
                "abi_float_probe",
                "pointer_probe",
            ):
                symbol = prefix + name
                symbol_disassembly = checked(
                    [objdump, "-d", f"--disassemble={symbol}", str(obj)],
                    f"disassemble baseline symbol {symbol}",
                )
                require(
                    re.search(
                        r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]{4,8}\s+v[a-z][a-z0-9_.]*\b",
                        symbol_disassembly,
                    )
                    is None,
                    f"baseline object symbol contains RVV: {symbol}",
                )

        absent = work / "fat_absent"
        zero = work / "fat_zero"
        one = work / "fat_one"
        link(gcc, obj, harness, absent, [])
        link(gcc, obj, harness, zero, [zero_stub])
        link(gcc, obj, harness, one, [one_stub])
        for executable in (absent, zero, one):
            executable_attributes = checked(
                [readelf, "-A", str(executable)],
                f"read {executable.name} attributes",
            )
            executable_arch = next(
                (
                    line
                    for line in executable_attributes.splitlines()
                    if "Tag_RISCV_arch:" in line
                ),
                "",
            )
            require(
                executable_arch
                and not re.search(r"(?:^|_)v(?:\d|$)", executable_arch),
                f"combined executable advertises V: {executable_arch}",
            )
        run_qemu(qemu, absent, None, "missing detector must select scalar")
        run_qemu(qemu, zero, None, "zero detector must select scalar")
        for vlen in VLENS:
            run_qemu(qemu, zero, vlen, f"zero detector scalar VLEN={vlen}")
            run_qemu(qemu, one, vlen, f"one detector RVV VLEN={vlen}")
        illegal = run([qemu, "-cpu", qemu_cpu(None), str(one)])
        require(illegal.returncode != 0,
                "one detector unexpectedly avoided the RVV variant on v=false")

        runtime_obj = work / "rvv_runtime.o"
        checked(
            [
                gcc,
                "-std=c11",
                "-O3",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-tree-vectorize",
                "-march=rv64gc",
                "-mabi=lp64d",
                f"-I{ROOT / 'runtime'}",
                "-c",
                str(ROOT / "runtime/rvv_runtime.c"),
                "-o",
                str(runtime_obj),
            ],
            "compile production detector",
        )
        real = work / "fat_real_detector"
        link(gcc, obj, harness, real, [runtime_obj], ["-DEXPECT_REAL_DETECTOR=1"])
        run_qemu(qemu, real, None, "production detector v=false fallback")
        run_qemu(qemu, real, 128, "production detector prctl-EINVAL fallback")

        # Source-local fixed vectors at the awkward N=3/7/31 widths must be
        # scalarized in the baseline variant and remain RVV in the vector
        # variant.  Execute the same public scalar ABI on v=false and at every
        # supported QEMU VLEN.
        fixed_source = work / "fixed_widths.sy"
        fixed_driver = work / "fixed_widths_driver.c"
        fixed_source.write_text(textwrap.dedent(FIXED_KERNEL_SOURCE), encoding="utf-8")
        fixed_driver.write_text(textwrap.dedent(FIXED_DRIVER_SOURCE), encoding="utf-8")
        fixed_assembly_path = work / "fixed_widths.s"
        checked(
            [
                str(compiler),
                "-S",
                "-O0",
                "-mrvv-deployment=fat",
                "-o",
                str(fixed_assembly_path),
                str(fixed_source),
            ],
            "fixed N=3/7/31 fat compilation",
        )
        fixed_assembly = fixed_assembly_path.read_text(encoding="utf-8")
        fixed_scalar, fixed_vector_and_dispatch = fixed_assembly.split("\t.option push\n", 1)
        fixed_vector, fixed_dispatch = fixed_vector_and_dispatch.split("\t.option pop\n", 1)
        require(not re.search(r"(?m)^\s*v[a-z][a-z0-9_.]*\s", fixed_scalar),
                "fixed-vector scalar variant contains RVV")
        require(re.search(r"(?m)^\s*vset(?:i?vli|vl)\s", fixed_vector) is not None,
                "fixed-vector RVV variant contains no vsetvl")
        require(not re.search(r"(?m)^\s*v[a-z][a-z0-9_.]*\s", fixed_dispatch),
                "fixed-vector dispatcher contains RVV")
        fixed_obj = work / "fixed_widths.o"
        checked(
            [
                gcc,
                "-c",
                "-march=rv64gc",
                "-mabi=lp64d",
                str(fixed_assembly_path),
                "-o",
                str(fixed_obj),
            ],
            "assemble fixed N=3/7/31 fat object",
        )
        fixed_zero = work / "fixed_widths_zero"
        fixed_one = work / "fixed_widths_one"
        link(gcc, fixed_obj, fixed_driver, fixed_zero, [zero_stub])
        link(gcc, fixed_obj, fixed_driver, fixed_one, [one_stub])
        run_qemu(qemu, fixed_zero, None, "fixed N=3/7/31 scalar v=false")
        for vlen in VLENS:
            run_qemu(qemu, fixed_one, vlen, f"fixed N=3/7/31 RVV VLEN={vlen}")

        non_scalar = work / "non_scalar.sy"
        non_scalar.write_text(
            "vector<int,3> identity(vector<int,3> value) { return value; }\n"
            "int main() { return 0; }\n",
            encoding="utf-8",
        )
        non_scalar_output = work / "non_scalar.s"
        checked(
            [
                str(compiler),
                "-S",
                "-mrvv-deployment=fat",
                "-o",
                str(non_scalar_output),
                str(non_scalar),
            ],
            "fixed aggregate public ABI",
        )
        require("identity:" in non_scalar_output.read_text(encoding="utf-8"),
                "fixed aggregate public ABI lacks its dispatcher")
        external = work / "external.sy"
        external.write_text("int main() { putint(1); return 0; }\n", encoding="utf-8")
        external_output = work / "external.s"
        checked(
            [
                str(compiler),
                "-S",
                "-mrvv-deployment=fat",
                "-o",
                str(external_output),
                str(external),
            ],
            "direct builtin runtime call",
        )
        external_assembly = external_output.read_text(encoding="utf-8")
        require(len(re.findall(r"(?:\bcall\s+putint\b|%pcrel_hi\(putint\))",
                               external_assembly)) == 2,
                "both variants must call the same builtin runtime symbol")
        reserved = work / "reserved.sy"
        reserved.write_text(
            "int __yoolang_collision(int value) { return value; }\n"
            "int main() { return __yoolang_collision(0); }\n",
            encoding="utf-8",
        )
        expect_failure(
            [str(compiler), "-S", "-mrvv-deployment=fat", str(reserved)],
            "FAT_RESERVED_SYMBOL",
            "reserved symbol collision",
        )
        expect_failure(
            [
                str(compiler),
                "-S",
                "-mrvv-deployment=fat",
                "-march=rv64gcv_zvl128b",
                str(source),
            ],
            "FAT_TARGET_UNSUPPORTED",
            "unsupported fat architecture",
        )
        expect_failure(
            [
                str(compiler),
                "-S",
                "-mrvv-deployment=fat",
                "-mvector-abi=psabi-vector",
                str(source),
            ],
            "requires compile-time V or Zve code generation",
            "psabi-vector fat ABI",
        )
        expect_failure(
            [
                str(compiler),
                "-S",
                "-mrvv-deployment=fat",
                "-mrvv-vector-bits=256",
                str(source),
            ],
            "FAT_FIXED_VLEN_UNSUPPORTED",
            "fixed target VLEN without a VLEN-aware detector",
        )
        expect_failure(
            [str(compiler), "--emit-oir", "-mrvv-deployment=fat", str(source)],
            "FAT_UNSUPPORTED_EMIT_MODE",
            "unsupported fat output mode",
        )

    print("PASS rvv_fat_multiversion_assembly")
    print("PASS rvv_fat_multiversion_dispatch")
    print("PASS rvv_fat_fixed_widths_n3_n7_n31")
    print("PASS rvv_fat_multiversion_fail_closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
