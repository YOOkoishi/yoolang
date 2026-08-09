#!/usr/bin/env python3

"""End-to-end gate for the standard aggregate ABI in RVV fat deployment.

The test deliberately assembles the combined file with an rv64gc baseline,
then selects either hidden implementation through the weak detector.  It reuses
the ordinary fixed-vector ABI corpus and adds awkward fixed widths, large
by-reference/sret values, packed mask tails, globals/arrays, direct externs,
and a builtin runtime call.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap

import standard_vector_aggregate_abi_e2e as standard_abi


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER = ROOT / "build/linux/x86_64/release/compiler"
VLENS = (128, 256, 512, 1024)
OPT_LEVELS = (0, 1, 2, 3)


MASK33_ONES = ",".join("1" for _ in range(33))
N33_SEED_LANES = ",".join(f"seed + {lane}" for lane in range(33))


EXTENDED_SOURCE = f"""
vector<int,1> fat_n1(vector<int,1> value) {{
  value[0] = value[0] + 101;
  return value;
}}

vector<int,7> fat_n7(vector<int,7> value) {{
  value[0] = value[0] + 7;
  value[6] = value[6] - 9;
  return value;
}}

vector<int,31> fat_n31(vector<int,31> value) {{
  value[0] = value[0] + 31;
  value[30] = value[30] - 17;
  return value;
}}

vector<int,33> fat_n33(vector<int,33> value) {{
  value[0] = value[0] + 33;
  value[32] = value[32] - 19;
  return value;
}}

mask<33> fat_mask33(mask<33> value) {{
  return value & mask<33>{{{MASK33_ONES}}};
}}

int fat_split(int p0, int p1, int p2, int p3, int p4, int p5, int p6,
              vector<int,3> value, int tail) {{
  if (p0 != 10 || p1 != 11 || p2 != 12 || p3 != 13) return 1;
  if (p4 != 14 || p5 != 15 || p6 != 16) return 2;
  if (value[0] != 101 || value[1] != 202 || value[2] != 303) return 3;
  if (tail != 404) return 4;
  return 0;
}}

vector<int,3> fat_global_vectors[2];
mask<33> fat_global_mask;

void fat_set_objects(int seed) {{
  fat_global_vectors[0] = vector<int,3>{{seed, seed + 1, seed + 2}};
  fat_global_vectors[1] = vector<int,3>{{seed + 10, seed + 11, seed + 12}};
  fat_global_mask = mask<33>{{{MASK33_ONES}}};
}}

vector<int,7> fat_recursive_n7(vector<int,7> value, int count) {{
  if (count <= 0) return value;
  return fat_recursive_n7(value + vector<int,7>{{1,2,3,4,5,6,7}}, count - 1);
}}

vector<int,7> fat_nested_n7(vector<int,7> value, int count) {{
  return fat_recursive_n7(value, count);
}}

extern vector<int,33> c_ext_n33(vector<int,33> value);
extern mask<33> c_ext_mask33(mask<33> value);
extern int c_ext_split(int p0, int p1, int p2, int p3,
                       int p4, int p5, int p6,
                       vector<int,3> value, int tail);
extern int c_ext_varargs(int tag, ...);

int fat_external_probe(int seed) {{
  vector<int,33> sent = vector<int,33>{{{N33_SEED_LANES}}};
  vector<int,33> got = c_ext_n33(sent);
  if (got[0] != seed + 100 || got[16] != seed + 216) return 1;
  if (got[32] != seed + 332) return 2;

  mask<33> mask_value = mask<33>{{{MASK33_ONES}}};
  mask<33> mask_result = c_ext_mask33(mask_value);
  if (mask_result[0] || !mask_result[1] || !mask_result[32]) return 3;

  vector<int,3> split = vector<int,3>{{101, 202, 303}};
  return c_ext_split(10, 11, 12, 13, 14, 15, 16, split, 404);
}}

int fat_runtime_probe(int value) {{
  putint(value);
  return value + 1;
}}

int fat_variadic_probe() {{
  return c_ext_varargs(7, 11, 13);
}}
"""


EXTENDED_DRIVER = r"""
#include <stdarg.h>

typedef unsigned char abi_u8;
typedef int abi_i32;

typedef union { abi_i32 lanes[1]; abi_u8 raw[4]; } abi_i32x1;
typedef union { abi_i32 lanes[3]; abi_u8 raw[12]; } abi_i32x3;
typedef union { abi_i32 lanes[7]; abi_u8 raw[28]; } abi_i32x7;
typedef union { abi_i32 lanes[31]; abi_u8 raw[124]; } abi_i32x31;
typedef union { abi_i32 lanes[33]; abi_u8 raw[132]; } abi_i32x33;
typedef struct { abi_u8 raw[5]; } abi_mask33;

_Static_assert(sizeof(abi_i32x1) == 4, "N=1 layout");
_Static_assert(sizeof(abi_i32x3) == 12, "N=3 layout");
_Static_assert(sizeof(abi_i32x7) == 28, "N=7 layout");
_Static_assert(sizeof(abi_i32x31) == 124, "N=31 layout");
_Static_assert(sizeof(abi_i32x33) == 132, "N=33 layout");
_Static_assert(_Alignof(abi_i32x33) == 4, "N=33 alignment");
_Static_assert(sizeof(abi_mask33) == 5, "mask<33> packed layout");
_Static_assert(_Alignof(abi_mask33) == 1, "mask<33> packed alignment");

extern abi_i32x1 fat_n1(abi_i32x1);
extern abi_i32x7 fat_n7(abi_i32x7);
extern abi_i32x31 fat_n31(abi_i32x31);
extern abi_i32x33 fat_n33(abi_i32x33);
extern abi_mask33 fat_mask33(abi_mask33);
extern int fat_split(int, int, int, int, int, int, int, abi_i32x3, int);
extern abi_i32x3 fat_global_vectors[2];
extern abi_mask33 fat_global_mask;
extern void fat_set_objects(int);
extern abi_i32x7 fat_nested_n7(abi_i32x7, int);
extern int fat_external_probe(int);
extern int fat_runtime_probe(int);
extern int fat_variadic_probe(void);

abi_i32x33 c_ext_n33(abi_i32x33 value) {
  value.lanes[0] += 100;
  value.lanes[16] += 200;
  value.lanes[32] += 300;
  return value;
}

abi_mask33 c_ext_mask33(abi_mask33 value) {
  value.raw[0] &= 0xfeu;
  value.raw[4] &= 0x01u;
  return value;
}

int c_ext_split(int p0, int p1, int p2, int p3,
                int p4, int p5, int p6, abi_i32x3 value, int tail) {
  if (p0 != 10 || p1 != 11 || p2 != 12 || p3 != 13) return 1;
  if (p4 != 14 || p5 != 15 || p6 != 16) return 2;
  if (value.lanes[0] != 101 || value.lanes[1] != 202 ||
      value.lanes[2] != 303) return 3;
  return tail == 404 ? 0 : 4;
}

int c_ext_varargs(int tag, ...) {
  va_list arguments;
  va_start(arguments, tag);
  int first = va_arg(arguments, int);
  int second = va_arg(arguments, int);
  va_end(arguments);
  return tag + first + second;
}

static int check_unchanged(const abi_i32 *before, const abi_i32 *after,
                           int lanes, int first_delta, int last_delta) {
  int lane;
  for (lane = 0; lane < lanes; ++lane) {
    int expected = before[lane];
    if (lane == 0) expected += first_delta;
    if (lane == lanes - 1) expected += last_delta;
    if (after[lane] != expected) return lane + 1;
  }
  return 0;
}

int main(void) {
  int lane;
  abi_i32x1 n1 = {{7}};
  abi_i32x1 r1 = fat_n1(n1);
  if (r1.lanes[0] != 108) return 1;

  abi_i32x7 n7;
  for (lane = 0; lane < 7; ++lane) n7.lanes[lane] = 1000 + lane;
  abi_i32x7 r7 = fat_n7(n7);
  if (check_unchanged(n7.lanes, r7.lanes, 7, 7, -9)) return 2;

  abi_i32x31 n31;
  for (lane = 0; lane < 31; ++lane) n31.lanes[lane] = 2000 + lane;
  abi_i32x31 r31 = fat_n31(n31);
  if (check_unchanged(n31.lanes, r31.lanes, 31, 31, -17)) return 3;

  abi_i32x33 n33;
  for (lane = 0; lane < 33; ++lane) n33.lanes[lane] = 3000 + lane;
  abi_i32x33 r33 = fat_n33(n33);
  if (check_unchanged(n33.lanes, r33.lanes, 33, 33, -19)) return 4;

  abi_mask33 mask = {{0xffu, 0xa5u, 0x5au, 0x81u, 0xffu}};
  abi_mask33 cleaned = fat_mask33(mask);
  if (cleaned.raw[0] != 0xffu || cleaned.raw[1] != 0xa5u ||
      cleaned.raw[2] != 0x5au || cleaned.raw[3] != 0x81u ||
      cleaned.raw[4] != 0x01u) return 5;

  abi_i32x3 split = {{101, 202, 303}};
  if (fat_split(10, 11, 12, 13, 14, 15, 16, split, 404) != 0) return 6;

  fat_set_objects(70);
  if (fat_global_vectors[0].lanes[0] != 70 ||
      fat_global_vectors[0].lanes[2] != 72 ||
      fat_global_vectors[1].lanes[0] != 80 ||
      fat_global_vectors[1].lanes[2] != 82) return 7;
  if (fat_global_mask.raw[0] != 0xffu || fat_global_mask.raw[3] != 0xffu ||
      fat_global_mask.raw[4] != 0x01u) return 8;

  abi_i32x7 recursive;
  for (lane = 0; lane < 7; ++lane) recursive.lanes[lane] = lane + 10;
  abi_i32x7 recursive_result = fat_nested_n7(recursive, 3);
  for (lane = 0; lane < 7; ++lane)
    if (recursive_result.lanes[lane] != recursive.lanes[lane] + 3 * (lane + 1))
      return 9;

  if (fat_external_probe(5) != 0) return 10;
  if (fat_runtime_probe(0) != 1) return 11;
  if (fat_variadic_probe() != 31) return 12;
  return 0;
}
"""


EXTENDED_CASE = standard_abi.ABICase(
    "fat_extended_n1_n7_n31_n33_mask33",
    EXTENDED_SOURCE,
    EXTENDED_DRIVER,
    ("c_ext_n33", "c_ext_mask33", "c_ext_split", "c_ext_varargs", "putint"),
)


class GateError(RuntimeError):
    pass


def command_text(command: list[str]) -> str:
    return shlex.join(command)


def run(
    command: list[str], *, timeout: float = 90.0
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise GateError(
            f"command timed out after {timeout:.0f}s: {command_text(command)}"
        ) from error


def checked(command: list[str], description: str, *, timeout: float = 90.0) -> str:
    result = run(command, timeout=timeout)
    if result.returncode != 0:
        raise GateError(
            f"{description} failed ({result.returncode}): {command_text(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def require_tool(name: str) -> str:
    tool = shutil.which(name)
    if tool is None:
        raise GateError(f"required tool not found in PATH: {name}")
    return tool


def write_text(path: Path, value: str) -> None:
    path.write_text(textwrap.dedent(value).strip() + "\n", encoding="utf-8")


def vector_mnemonics(text: str) -> list[str]:
    result: list[str] = []
    pattern = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]*)\b")
    for line in text.splitlines():
        match = pattern.match(line.lower())
        if match is not None and match.group(1).startswith("v"):
            result.append(match.group(1))
    return result


def assembly_has_vector_instruction(text: str) -> bool:
    return re.search(r"(?m)^\s*v[a-z][a-z0-9_.]*\s", text) is not None


def require_scalar_attributes(attributes: str, context: str) -> None:
    arch_lines = [line.lower() for line in attributes.splitlines() if "tag_riscv_arch" in line.lower()]
    require(bool(arch_lines), f"{context} lacks Tag_RISCV_arch")
    joined = " ".join(arch_lines)
    require("_v" not in joined and "zve" not in joined,
            f"{context} advertises vector ISA support: {joined}")


def function_body(assembly: str, name: str) -> str:
    match = re.search(
        rf"(?m)^{re.escape(name)}:\n(?P<body>.*?)(?=^\s*\.size\s+{re.escape(name)}\s*,)",
        assembly,
        re.DOTALL | re.MULTILINE,
    )
    if match is None:
        raise GateError(f"assembly lacks function body for {name}")
    return match.group("body")


def defined_names(assembly: str) -> tuple[str, ...]:
    names = tuple(re.findall(r"(?m)^__yoolang_scalar_([A-Za-z_][A-Za-z0-9_]*):$", assembly))
    require(bool(names), "fat assembly has no scalar variant definitions")
    require(len(names) == len(set(names)), "fat assembly duplicates a scalar variant")
    return names


def audit_dispatcher_contract(assembly: str, names: tuple[str, ...]) -> None:
    required = [
        "\tlla t0, __yoolang_rvv_available\n",
        "\taddi sp, sp, -144\n",
        "\tsd ra, 128(sp)\n",
        "\tjalr ra, 0(t0)\n",
        "\tld ra, 128(sp)\n",
        "\taddi sp, sp, 144\n",
    ]
    required.extend(f"\tsd a{index}, {index * 8}(sp)\n" for index in range(8))
    required.extend(f"\tld a{index}, {index * 8}(sp)\n" for index in range(8))
    required.extend(f"\tfsd fa{index}, {64 + index * 8}(sp)\n" for index in range(8))
    required.extend(f"\tfld fa{index}, {64 + index * 8}(sp)\n" for index in range(8))
    for name in names:
        body = function_body(assembly, name)
        for instruction in required:
            require(instruction in body,
                    f"dispatcher {name} does not preserve the complete LP64D call state: "
                    f"{instruction.strip()}")
        require(f"\ttail __yoolang_scalar_{name}\n" in body,
                f"dispatcher {name} lacks scalar tail call")
        require(f"\ttail __yoolang_rvv_{name}\n" in body,
                f"dispatcher {name} lacks RVV tail call")


def audit_symbol_table(symbols: str, names: tuple[str, ...], externals: tuple[str, ...]) -> None:
    entries: dict[str, tuple[int, int, str, str, str]] = {}
    ranges: list[tuple[int, int, str, str]] = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        try:
            value = int(fields[1], 16)
            size = int(fields[2])
        except ValueError:
            continue
        type_name, binding, visibility, section = fields[3:7]
        name = fields[7]
        entries[name] = (value, size, binding, visibility, section)
        if type_name == "FUNC" and section != "UND" and size > 0:
            ranges.append((value, value + size, section, name))

    for name in names:
        public = entries.get(name)
        scalar = entries.get(f"__yoolang_scalar_{name}")
        vector = entries.get(f"__yoolang_rvv_{name}")
        require(public is not None and public[2:4] == ("GLOBAL", "DEFAULT"),
                f"public dispatcher has wrong ELF visibility: {name}")
        require(scalar is not None and scalar[2:4] == ("GLOBAL", "HIDDEN"),
                f"scalar variant has wrong ELF visibility: {name}")
        require(vector is not None and vector[2:4] == ("GLOBAL", "HIDDEN"),
                f"RVV variant has wrong ELF visibility: {name}")

    for external in externals:
        entry = entries.get(external)
        require(entry is not None and entry[4] == "UND",
                f"direct external is not retained as ELF UND: {external}")

    ranges.sort(key=lambda item: (item[2], item[0], item[1], item[3]))
    for previous, current in zip(ranges, ranges[1:]):
        if previous[2] != current[2]:
            continue
        require(previous[1] <= current[0],
                f"ELF function ranges overlap: {previous[3]} and {current[3]}")


def audit_assembly(case: standard_abi.ABICase, assembly: str) -> tuple[str, ...]:
    require(assembly.startswith('\t.attribute arch, "rv64gc"\n'),
            f"{case.name} does not start with the rv64gc file attribute")
    require(assembly.count(".attribute arch") == 1,
            f"{case.name} leaks a branch architecture attribute")
    require(".variant_cc" not in assembly,
            f"{case.name} opted into the vector-register ABI")
    require("\t.option push\n\t.option arch, +v\n" in assembly and
            "\t.option pop\n" in assembly,
            f"{case.name} lacks a balanced local RVV option scope")
    scalar, remainder = assembly.split("\t.option push\n", 1)
    vector, dispatch = remainder.split("\t.option pop\n", 1)
    require(not assembly_has_vector_instruction(scalar),
            f"{case.name} scalar branch contains RVV")
    require(not assembly_has_vector_instruction(dispatch),
            f"{case.name} dispatcher region contains RVV")
    if case.requires_rvv_opcode:
        require(assembly_has_vector_instruction(vector),
                f"{case.name} RVV branch contains no RVV instruction")
    names = defined_names(assembly)
    for name in names:
        require(f"__yoolang_rvv_{name}:" in assembly,
                f"{case.name} lacks RVV variant for {name}")
        require(re.search(rf"(?m)^{re.escape(name)}:$", assembly) is not None,
                f"{case.name} lacks public dispatcher for {name}")
    audit_dispatcher_contract(assembly, names)
    for external in case.external_symbols:
        require(external in scalar and external in vector,
                f"{case.name} does not call the same external in both variants: {external}")
        require(f"__yoolang_scalar_{external}" not in assembly and
                f"__yoolang_rvv_{external}" not in assembly,
                f"{case.name} renamed external declaration {external}")
    if case.name == "recursive_nested_vector_calls":
        require("__yoolang_scalar_y_recursive_i3" in scalar and
                "__yoolang_rvv_y_recursive_i3" in vector,
                "recursive aggregate calls were not rebound to private variants")
    if case.name == EXTENDED_CASE.name:
        require("__yoolang_scalar_fat_recursive_n7" in scalar and
                "__yoolang_rvv_fat_recursive_n7" in vector,
                "extended recursive call was not rebound to private variants")
    return names


def assemble_object(assembler: str, assembly: Path, output: Path) -> None:
    checked(
        [assembler, "-march=rv64gc", "-mabi=lp64d", str(assembly), "-o", str(output)],
        f"assemble {assembly.name}",
    )


def link_executable(
    gcc: str,
    yoolang_object: Path,
    driver_object: Path,
    detector_object: Path | None,
    output: Path,
    use_runtime: bool,
) -> None:
    command = [
        gcc,
        "-static",
        "-march=rv64gc",
        "-mabi=lp64d",
        "-mcmodel=medany",
        str(yoolang_object),
        str(driver_object),
    ]
    if detector_object is not None:
        command.append(str(detector_object))
    if use_runtime:
        command.append(str(ROOT / "runtime/libsysy_riscv.a"))
    command.extend(["-o", str(output)])
    checked(command, f"link {output.name}", timeout=120.0)


def qemu_cpu(vlen: int | None) -> str:
    if vlen is None:
        return "rv64,v=false"
    return f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0"


def run_binary(qemu: str, executable: Path, vlen: int | None, context: str) -> None:
    checked([qemu, "-cpu", qemu_cpu(vlen), str(executable)], context, timeout=45.0)


def compile_detector(gcc: str, source: Path, output: Path) -> None:
    checked(
        [
            gcc,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-march=rv64gc",
            "-mabi=lp64d",
            "-c",
            str(source),
            "-o",
            str(output),
        ],
        f"compile {source.name}",
    )


def audit_object(
    case: standard_abi.ABICase,
    names: tuple[str, ...],
    obj: Path,
    readelf: str,
    objdump: str,
) -> None:
    require_scalar_attributes(checked([readelf, "-A", str(obj)], f"attributes {obj.name}"),
                              f"{case.name} object")
    symbols = checked([readelf, "-Ws", str(obj)], f"symbols {obj.name}")
    audit_symbol_table(symbols, names, case.external_symbols)
    disassembly = checked([objdump, "-dr", str(obj)], f"disassemble {obj.name}")
    if case.requires_rvv_opcode:
        require(bool(vector_mnemonics(disassembly)),
                f"{case.name} object has no decoded RVV instruction")
    for external in case.external_symbols:
        relocations = re.findall(
            rf"R_RISCV_[A-Z0-9_]+\s+{re.escape(external)}\b", disassembly
        )
        require(len(relocations) >= 2,
                f"{case.name} lacks one relocation per variant for {external}")
    for name in names:
        for symbol in (f"__yoolang_scalar_{name}", name):
            body = checked(
                [objdump, "-d", f"--disassemble={symbol}", str(obj)],
                f"disassemble baseline symbol {symbol}",
            )
            require(not vector_mnemonics(body),
                    f"baseline symbol contains decoded RVV: {symbol}")


def run_case(
    case: standard_abi.ABICase,
    work: Path,
    compiler: Path,
    c_compilers: tuple[standard_abi.CCompiler, standard_abi.CCompiler],
    assembler: str,
    readelf: str,
    objdump: str,
    gcc: str,
    qemu: str,
    detector_zero: Path,
    detector_one: Path,
) -> None:
    source = work / f"{case.name}.sy"
    driver = work / f"{case.name}.c"
    write_text(source, case.yoolang_source)
    write_text(driver, case.c_driver)
    driver_objects: dict[str, Path] = {}
    for c_compiler in c_compilers:
        output = work / f"{case.name}-{c_compiler.name}-driver.o"
        checked(c_compiler.compile_command(driver, output, assembly=False),
                f"compile {case.name} driver with {c_compiler.name}")
        driver_objects[c_compiler.name] = output

    use_runtime = "putint" in case.external_symbols
    for opt in OPT_LEVELS:
        assembly_path = work / f"{case.name}-O{opt}.s"
        assembly_path.unlink(missing_ok=True)
        checked(
            [
                str(compiler),
                str(source),
                "-S",
                f"-O{opt}",
                "-march=rv64gc",
                "-mabi=lp64d",
                "-mrvv-deployment=fat",
                "-o",
                str(assembly_path),
            ],
            f"compile {case.name} O{opt} fat",
            timeout=180.0,
        )
        assembly = assembly_path.read_text(encoding="utf-8")
        require(bool(assembly.strip()), f"{case.name} O{opt} produced empty assembly")
        names = audit_assembly(case, assembly)

        obj = work / f"{case.name}-O{opt}.o"
        assemble_object(assembler, assembly_path, obj)
        audit_object(case, names, obj, readelf, objdump)

        for c_compiler in c_compilers:
            driver_object = driver_objects[c_compiler.name]
            missing = work / f"{case.name}-O{opt}-{c_compiler.name}-missing"
            zero = work / f"{case.name}-O{opt}-{c_compiler.name}-zero"
            one = work / f"{case.name}-O{opt}-{c_compiler.name}-one"
            link_executable(gcc, obj, driver_object, None, missing, use_runtime)
            link_executable(gcc, obj, driver_object, detector_zero, zero, use_runtime)
            link_executable(gcc, obj, driver_object, detector_one, one, use_runtime)
            for executable in (missing, zero, one):
                require_scalar_attributes(
                    checked([readelf, "-A", str(executable)],
                            f"attributes {executable.name}"),
                    f"{case.name} linked executable",
                )
            run_binary(qemu, missing, None,
                       f"{case.name} O{opt} {c_compiler.name} missing detector")
            run_binary(qemu, zero, None,
                       f"{case.name} O{opt} {c_compiler.name} zero detector v=false")
            for vlen in VLENS:
                run_binary(qemu, one, vlen,
                           f"{case.name} O{opt} {c_compiler.name} RVV VLEN={vlen}")
                if case.name == EXTENDED_CASE.name:
                    run_binary(qemu, zero, vlen,
                               f"{case.name} O{opt} {c_compiler.name} zero VLEN={vlen}")
        print(f"PASS {case.name}_O{opt}_fat_standard_abi")


def expect_failure(command: list[str], needle: str, output: Path, context: str) -> None:
    output.unlink(missing_ok=True)
    result = run(command)
    require(result.returncode != 0, f"{context} unexpectedly succeeded")
    diagnostics = result.stdout + result.stderr
    require(needle in diagnostics, f"{context} lost diagnostic {needle!r}:\n{diagnostics}")
    require(not output.exists() or output.stat().st_size == 0,
            f"{context} left a non-empty partial output")


def negative_gates(compiler: Path, work: Path) -> None:
    source = work / "negative.sy"
    write_text(source, "int ordinary(int value) { return value; }")
    output = work / "negative.s"
    expect_failure(
        [
            str(compiler),
            "-S",
            "-mrvv-deployment=fat",
            "-mvector-abi=psabi-vector",
            "-o",
            str(output),
            str(source),
        ],
        "requires compile-time V or Zve code generation",
        output,
        "fat psabi-vector",
    )
    expect_failure(
        [
            str(compiler),
            "-S",
            "-mrvv-deployment=fat",
            "-mrvv-vector-bits=256",
            "-o",
            str(output),
            str(source),
        ],
        "FAT_FIXED_VLEN_UNSUPPORTED",
        output,
        "fat numeric VLEN",
    )
    reserved = work / "reserved.sy"
    write_text(reserved, "int __yoolang_bad(int value) { return value; }")
    expect_failure(
        [
            str(compiler),
            "-S",
            "-mrvv-deployment=fat",
            "-o",
            str(output),
            str(reserved),
        ],
        "FAT_RESERVED_SYMBOL",
        output,
        "fat reserved symbol",
    )
    print("PASS fat_standard_abi_negative_fail_closed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RVV fat standard aggregate ABI gate")
    parser.add_argument(
        "--compiler",
        type=Path,
        default=Path(os.environ.get("YOOLANG_COMPILER", DEFAULT_COMPILER)),
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="retain generated sources, objects, and executables in this directory",
    )
    return parser.parse_args()


def run_gate(args: argparse.Namespace, work: Path) -> int:
    compiler = args.compiler.resolve()
    require(compiler.is_file(), f"compiler not found: {compiler}")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    clang = require_tool(os.environ.get("RISCV_CLANG", "clang"))
    assembler = require_tool("riscv64-linux-gnu-as")
    readelf = require_tool("riscv64-linux-gnu-readelf")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    qemu = require_tool("qemu-riscv64")
    standard_abi.MARCH = "rv64gc"
    standard_abi.MABI = "lp64d"
    compiler_set = standard_abi.c_compilers(gcc, clang)

    zero_source = work / "detector_zero.c"
    one_source = work / "detector_one.c"
    zero_object = work / "detector_zero.o"
    one_object = work / "detector_one.o"
    write_text(zero_source, "int __yoolang_rvv_available(void) { return 0; }")
    write_text(one_source, "int __yoolang_rvv_available(void) { return 1; }")
    compile_detector(gcc, zero_source, zero_object)
    compile_detector(gcc, one_source, one_object)

    cases = (*standard_abi.ABI_CASES, EXTENDED_CASE)
    for case in cases:
        run_case(
            case,
            work,
            compiler,
            compiler_set,
            assembler,
            readelf,
            objdump,
            gcc,
            qemu,
            zero_object,
            one_object,
        )
    negative_gates(compiler, work)

    print(f"PASS rvv_fat_standard_abi_cases_{len(cases)}_O0_O3_gcc_clang")
    print("PASS rvv_fat_dispatch_preserves_gpr_fpr_stack_sret")
    print("PASS rvv_fat_external_runtime_elf_contract")
    return 0


def main() -> int:
    args = parse_args()
    if args.artifacts is not None:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        return run_gate(args, args.artifacts)
    with tempfile.TemporaryDirectory(prefix="yoolang-fat-standard-abi-") as temporary:
        return run_gate(args, Path(temporary))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateError as error:
        print(f"FAIL rvv_fat_standard_abi: {error}", file=sys.stderr)
        raise SystemExit(1)
