#!/usr/bin/env python3

"""Fail-closed standard fixed-vector aggregate ABI interoperability probe.

The auto-discovered ``*_infra_tests.py`` wrapper makes this a required
toolchain gate. A complete run exits 0. Missing external tools or an
allowlisted compiler implementation regression exits 2 (and therefore fails
that gate); every malformed diagnostic, partial output, ABI mismatch, link
failure, or runtime mismatch exits 1.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER = ROOT / "build/linux/x86_64/release/compiler"
MARCH = "rv64gcv"
MABI = "lp64d"
VLEN_BITS = (128, 256, 512, 1024)
OPT_LEVELS = (0, 1, 2, 3)


def qemu_cpu(vlen_bits: int) -> str:
    if MARCH == "rv64gc":
        return "rv64,v=false"
    return f"rv64,v=true,vlen={vlen_bits},elen=64,vext_spec=v1.0"


def disassembly_mnemonics(disassembly: str) -> list[str]:
    result: list[str] = []
    pattern = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]*)\b")
    for line in disassembly.splitlines():
        match = pattern.match(line.lower())
        if match is not None:
            result.append(match.group(1))
    return result


def require_scalar_elf_attributes(attributes: str, context: str) -> None:
    arch_lines = [
        line.lower() for line in attributes.splitlines() if "tag_riscv_arch" in line.lower()
    ]
    if not arch_lines:
        raise ProbeError(f"{context} lacks Tag_RISCV_arch")
    arch = " ".join(arch_lines)
    if "_v" in arch or "zve" in arch:
        raise ProbeError(f"{context} unexpectedly advertises vector ISA support: {arch}")

EXPECTED_UNIMPLEMENTED_MARKERS = (
    "PORTABLE_VECTOR_AGGREGATE_ABI_UNAVAILABLE",
    "indirect aggregate passing is not implemented by the MIR lowerer",
    "multi-location value passing is not implemented by the MIR lowerer",
    "aggregate or vector value passing is not implemented by the MIR lowerer",
    "indirect sret lowering is not implemented by the MIR lowerer",
)


C_ABI_HEADER = r"""
typedef unsigned char abi_u8;
typedef unsigned int abi_u32;
typedef int abi_i32;

typedef union {
  abi_i32 lanes[3];
  abi_u32 words[3];
  abi_u8 raw[12];
} abi_i32x3;

typedef union {
  abi_i32 lanes[5];
  abi_u32 words[5];
  abi_u8 raw[20];
} abi_i32x5;

/* A union is intentional.  A struct of scalar float fields can use the
   LP64D hardware-floating-point aggregate convention; a union cannot be
   flattened that way and is the C layout oracle for vector<float,3>. */
typedef union {
  float lanes[3];
  abi_u32 words[3];
  abi_u8 raw[12];
} abi_f32x3_raw;

/* mask<31> is four packed bytes with ABI alignment one.  Do not add a u32
   union member: doing so would accidentally raise the C alignment to four. */
typedef struct {
  abi_u8 raw[4];
} abi_mask31;

_Static_assert(sizeof(abi_i32) == 4, "the oracle requires 32-bit int");
_Static_assert(sizeof(float) == 4, "the oracle requires binary32 float");
_Static_assert(sizeof(abi_i32x3) == 12, "vector<int,3> ABI size");
_Static_assert(_Alignof(abi_i32x3) == 4, "vector<int,3> ABI alignment");
_Static_assert(sizeof(abi_i32x5) == 20, "vector<int,5> ABI size");
_Static_assert(_Alignof(abi_i32x5) == 4, "vector<int,5> ABI alignment");
_Static_assert(sizeof(abi_f32x3_raw) == 12, "vector<float,3> ABI size");
_Static_assert(_Alignof(abi_f32x3_raw) == 4, "vector<float,3> ABI alignment");
_Static_assert(sizeof(abi_mask31) == 4, "mask<31> packed ABI size");
_Static_assert(_Alignof(abi_mask31) == 1, "mask<31> packed ABI alignment");
"""


C_ORACLE_PROVIDER = C_ABI_HEADER + r"""
int abi_oracle_entry_i3(abi_i32x3 value) {
  if (value.lanes[0] != 0x10203040) return 1;
  if (value.lanes[1] != -73) return 2;
  if (value.lanes[2] != 0x556677) return 3;
  return 0;
}

abi_i32x3 abi_oracle_return_i3(abi_i32 seed) {
  abi_i32x3 result;
  result.lanes[0] = seed + 11;
  result.lanes[1] = seed - 22;
  result.lanes[2] = seed ^ 0x3456;
  return result;
}

int abi_oracle_split(abi_i32 p0, abi_i32 p1, abi_i32 p2, abi_i32 p3,
                     abi_i32 p4, abi_i32 p5, abi_i32 p6,
                     abi_i32x3 value, abi_i32 tail) {
  if (p0 != 10 || p1 != 11 || p2 != 12 || p3 != 13) return 1;
  if (p4 != 14 || p5 != 15 || p6 != 16) return 2;
  if (value.lanes[0] != 101 || value.lanes[1] != 202 ||
      value.lanes[2] != 303) return 3;
  if (tail != 404) return 4;
  return 0;
}

int abi_oracle_entry_f3(abi_f32x3_raw value) {
  if (value.words[0] != 0x3f800000u) return 1;
  if (value.words[1] != 0xc0200000u) return 2;
  if (value.words[2] != 0x40500000u) return 3;
  return 0;
}

abi_mask31 abi_oracle_mask31(abi_mask31 value) {
  value.raw[3] &= 0x7fu;
  return value;
}

abi_i32x3 abi_oracle_cross_compiler_transform(abi_i32x3 value) {
  value.lanes[0] += 100;
  value.lanes[1] += 200;
  value.lanes[2] += 300;
  return value;
}
"""


C_ORACLE_CONSUMER = C_ABI_HEADER + r"""
extern int abi_oracle_entry_i3(abi_i32x3);
extern abi_i32x3 abi_oracle_return_i3(abi_i32);
extern int abi_oracle_split(abi_i32, abi_i32, abi_i32, abi_i32,
                            abi_i32, abi_i32, abi_i32, abi_i32x3, abi_i32);
extern int abi_oracle_entry_f3(abi_f32x3_raw);
extern abi_mask31 abi_oracle_mask31(abi_mask31);
extern abi_i32x3 abi_oracle_cross_compiler_transform(abi_i32x3);

int main(void) {
  abi_i32x3 input;
  input.lanes[0] = 0x10203040;
  input.lanes[1] = -73;
  input.lanes[2] = 0x556677;
  if (abi_oracle_entry_i3(input) != 0) return 1;

  abi_i32x3 returned = abi_oracle_return_i3(77);
  if (returned.lanes[0] != 88 || returned.lanes[1] != 55 ||
      returned.lanes[2] != (77 ^ 0x3456)) return 2;

  abi_i32x3 split;
  split.lanes[0] = 101;
  split.lanes[1] = 202;
  split.lanes[2] = 303;
  if (abi_oracle_split(10, 11, 12, 13, 14, 15, 16, split, 404) != 0)
    return 3;

  abi_f32x3_raw floats;
  floats.words[0] = 0x3f800000u;
  floats.words[1] = 0xc0200000u;
  floats.words[2] = 0x40500000u;
  if (abi_oracle_entry_f3(floats) != 0) return 4;

  abi_mask31 mask = {{0xffu, 0xa5u, 0x5au, 0xffu}};
  abi_mask31 clean = abi_oracle_mask31(mask);
  if (clean.raw[0] != 0xffu || clean.raw[1] != 0xa5u ||
      clean.raw[2] != 0x5au || clean.raw[3] != 0x7fu) return 5;

  abi_i32x3 transformed = abi_oracle_cross_compiler_transform(input);
  if (transformed.lanes[0] != 0x10203040 + 100 ||
      transformed.lanes[1] != 127 ||
      transformed.lanes[2] != 0x556677 + 300) return 6;
  return 0;
}
"""


MASK31_ONES = ",".join("1" for _ in range(31))


@dataclass(frozen=True)
class ABICase:
    name: str
    yoolang_source: str
    c_driver: str
    external_symbols: tuple[str, ...] = ()
    requires_rvv_opcode: bool = True
    allows_medany_direct_call: bool = False


ABI_CASES = (
    ABICase(
        "c_to_y_direct_entry_i3",
        r"""
        int y_entry_i3(vector<int,3> value) {
          if (value[0] != 270544960) return 1;
          if (value[1] != -73) return 2;
          if (value[2] != 5596791) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        extern int y_entry_i3(abi_i32x3);
        int main(void) {
          abi_i32x3 value;
          value.lanes[0] = 0x10203040;
          value.lanes[1] = -73;
          value.lanes[2] = 0x556677;
          return y_entry_i3(value);
        }
        """,
    ),
    ABICase(
        "c_to_y_direct_return_i3",
        r"""
        vector<int,3> y_return_i3(int seed) {
          return vector<int,3>{seed + 11, seed - 22, seed ^ 13398};
        }
        """,
        C_ABI_HEADER
        + r"""
        extern abi_i32x3 y_return_i3(abi_i32);
        int main(void) {
          abi_i32x3 value = y_return_i3(77);
          if (value.lanes[0] != 88) return 1;
          if (value.lanes[1] != 55) return 2;
          if (value.lanes[2] != (77 ^ 0x3456)) return 3;
          return 0;
        }
        """,
    ),
    ABICase(
        "c_to_y_seven_gpr_plus_i3_split",
        r"""
        int y_split(int p0, int p1, int p2, int p3, int p4, int p5, int p6,
                    vector<int,3> value, int tail) {
          if (p0 != 10 || p1 != 11 || p2 != 12 || p3 != 13) return 1;
          if (p4 != 14 || p5 != 15 || p6 != 16) return 2;
          if (value[0] != 101 || value[1] != 202 || value[2] != 303) return 3;
          if (tail != 404) return 4;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        extern int y_split(abi_i32, abi_i32, abi_i32, abi_i32,
                           abi_i32, abi_i32, abi_i32, abi_i32x3, abi_i32);
        int main(void) {
          abi_i32x3 value;
          value.lanes[0] = 101;
          value.lanes[1] = 202;
          value.lanes[2] = 303;
          return y_split(10, 11, 12, 13, 14, 15, 16, value, 404);
        }
        """,
    ),
    ABICase(
        "c_to_y_float_union_raw_entry",
        r"""
        int y_entry_f3(vector<float,3> value) {
          if (value[0] != 1.0) return 1;
          if (value[1] != -2.5) return 2;
          if (value[2] != 3.25) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        extern int y_entry_f3(abi_f32x3_raw);
        int main(void) {
          abi_f32x3_raw value;
          value.words[0] = 0x3f800000u;
          value.words[1] = 0xc0200000u;
          value.words[2] = 0x40500000u;
          return y_entry_f3(value);
        }
        """,
    ),
    ABICase(
        "c_to_y_mask31_packed_tail",
        f"""
        mask<31> y_mask31(mask<31> value) {{
          return value & mask<31>{{{MASK31_ONES}}};
        }}
        """,
        C_ABI_HEADER
        + r"""
        extern abi_mask31 y_mask31(abi_mask31);
        int main(void) {
          abi_mask31 input = {{0xffu, 0xa5u, 0x5au, 0xffu}};
          abi_mask31 result = y_mask31(input);
          if (result.raw[0] != 0xffu || result.raw[1] != 0xa5u ||
              result.raw[2] != 0x5au || result.raw[3] != 0x7fu) return 1;
          return 0;
        }
        """,
    ),
    ABICase(
        "y_to_c_scalar_float_array_extern",
        r"""
        extern int c_scalar_array_mix(int count, float scale, int values[]);

        int y_call_scalar_array_mix() {
          int values[3] = {4, 5, 6};
          return c_scalar_array_mix(3, 1.5, values);
        }
        """,
        C_ABI_HEADER
        + r"""
        int c_scalar_array_mix(abi_i32 count, float scale, abi_i32 values[]) {
          if (count != 3 || scale != 1.5f) return 1;
          if (values[0] != 4 || values[1] != 5 || values[2] != 6) return 2;
          values[1] = 99;
          return values[1] == 99 ? 0 : 3;
        }
        extern int y_call_scalar_array_mix(void);
        int main(void) { return y_call_scalar_array_mix(); }
        """,
        ("c_scalar_array_mix",),
        False,
        True,
    ),
    ABICase(
        "y_to_c_direct_i3_param_and_return",
        r"""
        extern vector<int,3> c_i3_roundtrip(vector<int,3> value);

        int y_call_c_i3(int seed) {
          vector<int,3> sent = vector<int,3>{seed, seed + 1, seed + 2};
          vector<int,3> got = c_i3_roundtrip(sent);
          if (got[0] != seed + 10) return 1;
          if (got[1] != seed + 21) return 2;
          if (got[2] != seed + 32) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        abi_i32x3 c_i3_roundtrip(abi_i32x3 value) {
          value.lanes[0] += 10;
          value.lanes[1] += 20;
          value.lanes[2] += 30;
          return value;
        }
        extern int y_call_c_i3(int);
        int main(void) { return y_call_c_i3(7); }
        """,
        ("c_i3_roundtrip",),
    ),
    ABICase(
        "y_to_c_float_union_raw_param_and_return",
        r"""
        extern vector<float,3> c_f3_roundtrip(vector<float,3> value);

        int y_call_c_f3() {
          vector<float,3> sent = vector<float,3>{1.0, 2.0, 3.0};
          vector<float,3> got = c_f3_roundtrip(sent);
          if (got[0] != 4.0) return 1;
          if (got[1] != -5.0) return 2;
          if (got[2] != 6.5) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        abi_f32x3_raw c_f3_roundtrip(abi_f32x3_raw value) {
          if (value.words[0] != 0x3f800000u ||
              value.words[1] != 0x40000000u ||
              value.words[2] != 0x40400000u) {
            value.words[0] = 0x7fc00001u;
            value.words[1] = 0x7fc00002u;
            value.words[2] = 0x7fc00003u;
            return value;
          }
          value.words[0] = 0x40800000u;
          value.words[1] = 0xc0a00000u;
          value.words[2] = 0x40d00000u;
          return value;
        }
        extern int y_call_c_f3(void);
        int main(void) { return y_call_c_f3(); }
        """,
        ("c_f3_roundtrip",),
    ),
    ABICase(
        "c_to_y_large_byref_and_sret",
        r"""
        int y_entry_i5(vector<int,5> value) {
          if (value[0] != 101 || value[1] != -202 || value[2] != 303) return 1;
          if (value[3] != -404 || value[4] != 505) return 2;
          return 0;
        }

        vector<int,5> y_return_i5(int seed) {
          return vector<int,5>{seed + 1, seed + 2, seed + 3, seed + 4, seed + 5};
        }
        """,
        C_ABI_HEADER
        + r"""
        extern int y_entry_i5(abi_i32x5);
        extern abi_i32x5 y_return_i5(abi_i32);
        int main(void) {
          abi_i32x5 input;
          input.lanes[0] = 101; input.lanes[1] = -202;
          input.lanes[2] = 303; input.lanes[3] = -404; input.lanes[4] = 505;
          if (y_entry_i5(input) != 0) return 1;
          abi_i32x5 result = y_return_i5(70);
          for (abi_i32 lane = 0; lane < 5; ++lane)
            if (result.lanes[lane] != 71 + lane) return 2 + lane;
          return 0;
        }
        """,
    ),
    ABICase(
        "y_to_c_large_byref_and_sret",
        r"""
        extern vector<int,5> c_i5_transform(vector<int,5> value);

        int y_call_c_i5(int seed) {
          vector<int,5> sent = vector<int,5>{seed, seed + 1, seed + 2,
                                             seed + 3, seed + 4};
          vector<int,5> got = c_i5_transform(sent);
          if (got[0] != seed + 10 || got[1] != seed + 21) return 1;
          if (got[2] != seed + 32 || got[3] != seed + 43) return 2;
          if (got[4] != seed + 54) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        abi_i32x5 c_i5_transform(abi_i32x5 value) {
          value.lanes[0] += 10; value.lanes[1] += 20;
          value.lanes[2] += 30; value.lanes[3] += 40;
          value.lanes[4] += 50;
          return value;
        }
        extern int y_call_c_i5(int);
        int main(void) { return y_call_c_i5(7); }
        """,
        ("c_i5_transform",),
    ),
    ABICase(
        "y_to_c_seven_gpr_plus_i3_split",
        r"""
        extern int c_split_i3(int p0, int p1, int p2, int p3,
                              int p4, int p5, int p6,
                              vector<int,3> value, int tail);

        int y_call_c_split_i3() {
          vector<int,3> value = vector<int,3>{101, 202, 303};
          return c_split_i3(10, 11, 12, 13, 14, 15, 16, value, 404);
        }
        """,
        C_ABI_HEADER
        + r"""
        int c_split_i3(abi_i32 p0, abi_i32 p1, abi_i32 p2, abi_i32 p3,
                       abi_i32 p4, abi_i32 p5, abi_i32 p6,
                       abi_i32x3 value, abi_i32 tail) {
          if (p0 != 10 || p1 != 11 || p2 != 12 || p3 != 13) return 1;
          if (p4 != 14 || p5 != 15 || p6 != 16) return 2;
          if (value.lanes[0] != 101 || value.lanes[1] != 202 ||
              value.lanes[2] != 303) return 3;
          if (tail != 404) return 4;
          return 0;
        }
        extern int y_call_c_split_i3(void);
        int main(void) { return y_call_c_split_i3(); }
        """,
        ("c_split_i3",),
    ),
    ABICase(
        "y_mask31_global_object_rmw",
        f"""
        mask<31> y_mask31_object;

        void y_store_mask31_object() {{
          y_mask31_object = mask<31>{{{MASK31_ONES}}};
        }}
        """,
        C_ABI_HEADER
        + r"""
        extern abi_mask31 y_mask31_object;
        extern void y_store_mask31_object(void);
        int main(void) {
          y_mask31_object.raw[0] = 0x00u;
          y_mask31_object.raw[1] = 0x55u;
          y_mask31_object.raw[2] = 0xaau;
          y_mask31_object.raw[3] = 0xffu;
          y_store_mask31_object();
          if (y_mask31_object.raw[0] != 0xffu ||
              y_mask31_object.raw[1] != 0xffu ||
              y_mask31_object.raw[2] != 0xffu ||
              y_mask31_object.raw[3] != 0x7fu) return 1;
          return 0;
        }
        """,
    ),
    ABICase(
        "recursive_nested_vector_calls",
        r"""
        vector<int,3> y_recursive_i3(vector<int,3> value, int count) {
          if (count <= 0) return value;
          vector<int,3> step = vector<int,3>{1, 2, 3};
          return y_recursive_i3(value + step, count - 1);
        }

        vector<int,3> y_nested_i3(vector<int,3> value, int count) {
          return y_recursive_i3(value, count);
        }

        int y_test_nested_i3(int seed) {
          vector<int,3> value = vector<int,3>{seed, seed + 10, seed + 20};
          vector<int,3> first = y_nested_i3(value, 3);
          vector<int,3> second = y_nested_i3(first, 2);
          if (second[0] != seed + 5) return 1;
          if (second[1] != seed + 20) return 2;
          if (second[2] != seed + 35) return 3;
          return 0;
        }

        vector<int,5> y_recursive_i5(vector<int,5> value, int count) {
          if (count <= 0) return value;
          vector<int,5> step = vector<int,5>{1, 2, 3, 4, 5};
          return y_recursive_i5(value + step, count - 1);
        }

        int y_test_recursive_i5(int seed) {
          vector<int,5> value = vector<int,5>{seed, seed + 10, seed + 20,
                                              seed + 30, seed + 40};
          vector<int,5> result = y_recursive_i5(value, 3);
          if (result[0] != seed + 3 || result[1] != seed + 16) return 1;
          if (result[2] != seed + 29 || result[3] != seed + 42) return 2;
          if (result[4] != seed + 55) return 3;
          return 0;
        }
        """,
        C_ABI_HEADER
        + r"""
        extern int y_test_nested_i3(abi_i32);
        extern int y_test_recursive_i5(abi_i32);
        int main(void) {
          if (y_test_nested_i3(7) != 0) return 1;
          if (y_test_recursive_i5(9) != 0) return 2;
          return 0;
        }
        """,
    ),
)


class ProbeError(RuntimeError):
    pass


class ProbeBlocked(RuntimeError):
    pass


def command_text(command: list[str]) -> str:
    return shlex.join(command)


def run_process(
    command: list[str], *, cwd: Path = ROOT, timeout: float = 90.0
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise ProbeError(
            f"command timed out after {timeout:.0f}s: {command_text(command)}"
        ) from error


def run_checked(command: list[str], *, cwd: Path = ROOT, timeout: float = 90.0) -> str:
    result = run_process(command, cwd=cwd, timeout=timeout)
    if result.returncode != 0:
        raise ProbeError(
            f"command failed ({result.returncode}): {command_text(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def require_tool(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise ProbeBlocked(f"required tool is not installed: {name}")
    return resolved


def function_body(assembly: str, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}:[ \t]*(?:#[^\n]*)?\n(?P<body>.*?)"
        rf"(?=^\s*\.size\s+{re.escape(name)}\s*,)",
        assembly,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise ProbeError(f"C oracle assembly lacks function body: {name}")
    return match.group("body")


@dataclass(frozen=True)
class CCompiler:
    name: str
    command: tuple[str, ...]

    def compile_command(self, source: Path, output: Path, *, assembly: bool) -> list[str]:
        return [
            *self.command,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fno-stack-protector",
            f"-march={MARCH}",
            f"-mabi={MABI}",
            "-S" if assembly else "-c",
            str(source),
            "-o",
            str(output),
        ]


def c_compilers(gcc: str, clang: str) -> tuple[CCompiler, CCompiler]:
    sysroot = run_checked([gcc, "-print-sysroot"]).strip()
    clang_command = [clang, "--target=riscv64-linux-gnu"]
    if sysroot:
        clang_command.append(f"--sysroot={sysroot}")
    return CCompiler("gcc", (gcc,)), CCompiler("clang", tuple(clang_command))


def write_source(path: Path, source: str) -> None:
    path.write_text(textwrap.dedent(source).strip() + "\n", encoding="utf-8")


def validate_c_assembly_contract(assembly: str, compiler_name: str) -> None:
    float_body = function_body(assembly, "abi_oracle_entry_f3")
    if re.search(r"\bfa[0-7]\b", float_body):
        raise ProbeError(
            f"{compiler_name} classified abi_f32x3_raw through fa registers"
        )
    for register in ("a0", "a1"):
        if not re.search(rf"\b{register}\b", float_body):
            raise ProbeError(
                f"{compiler_name} float-union oracle does not consume {register}"
            )

    split_body = function_body(assembly, "abi_oracle_split")
    if not re.search(r"\ba7\b", split_body):
        raise ProbeError(f"{compiler_name} split oracle does not consume a7")
    # Some compilers first allocate a local reconstruction area for the
    # register/stack-split union.  Incoming stack+0/+8 then appear at
    # frame_size(sp)/frame_size+8(sp), respectively.
    frame_match = re.search(r"\baddi\s+sp\s*,\s*sp\s*,\s*-(\d+)\b", split_body)
    frame_size = int(frame_match.group(1)) if frame_match is not None else 0
    for incoming_offset in (0, 8):
        offset = frame_size + incoming_offset
        if not re.search(rf"\b(?:lw|ld)\s+[^\n,]+,\s*{offset}\(sp\)", split_body):
            raise ProbeError(
                f"{compiler_name} split oracle does not load incoming "
                f"stack+{incoming_offset} (current sp+{offset})"
            )

    return_body = function_body(assembly, "abi_oracle_return_i3")
    for register in ("a0", "a1"):
        if not re.search(rf"\b{register}\b", return_body):
            raise ProbeError(
                f"{compiler_name} 12-byte direct return does not define/use {register}"
            )


def run_classifier_gate(python: str) -> None:
    command = [python, str(ROOT / "scripts/calling_convention_tests.py")]
    result = run_process(command, timeout=120.0)
    if result.returncode != 0:
        raise ProbeError(
            "shared RISCVCallingConvention classifier gate failed:\n"
            + result.stdout
            + result.stderr
        )
    required = (
        "PASS direct_aggregate_size_boundaries",
        "PASS float_vectors_use_integer_aggregate_locations",
        "PASS mask_layout_and_large_sret",
        "PASS register_stack_split_and_variadic_float",
    )
    missing = [line for line in required if line not in result.stdout]
    if missing:
        raise ProbeError("classifier gate omitted: " + ", ".join(missing))
    print("PASS shared_riscv_calling_convention_classifier")


def run_c_oracle(
    directory: Path,
    compilers: tuple[CCompiler, CCompiler],
    gcc_linker: str,
    qemu: str,
) -> None:
    provider_source = directory / "abi_oracle_provider.c"
    consumer_source = directory / "abi_oracle_consumer.c"
    write_source(provider_source, C_ORACLE_PROVIDER)
    write_source(consumer_source, C_ORACLE_CONSUMER)

    objects: dict[tuple[str, str], Path] = {}
    for compiler in compilers:
        provider_object = directory / f"provider-{compiler.name}.o"
        consumer_object = directory / f"consumer-{compiler.name}.o"
        provider_assembly = directory / f"provider-{compiler.name}.s"
        run_checked(compiler.compile_command(provider_source, provider_object, assembly=False))
        run_checked(compiler.compile_command(consumer_source, consumer_object, assembly=False))
        run_checked(compiler.compile_command(provider_source, provider_assembly, assembly=True))
        validate_c_assembly_contract(
            provider_assembly.read_text(encoding="utf-8"), compiler.name
        )
        objects[(compiler.name, "provider")] = provider_object
        objects[(compiler.name, "consumer")] = consumer_object
        print(f"PASS {compiler.name}_c_aggregate_layout_assembly_contract")

    for caller, callee in ((compilers[0], compilers[1]), (compilers[1], compilers[0])):
        executable = directory / f"oracle-{caller.name}-to-{callee.name}"
        run_checked(
            [
                gcc_linker,
                "-static",
                f"-march={MARCH}",
                f"-mabi={MABI}",
                "-mcmodel=medany",
                str(objects[(caller.name, "consumer")]),
                str(objects[(callee.name, "provider")]),
                "-o",
                str(executable),
            ]
        )
        for vlen_bits in VLEN_BITS:
            run_checked(
                [qemu, "-cpu", qemu_cpu(vlen_bits), str(executable)], timeout=30.0
            )
            print(
                f"PASS c_oracle_{caller.name}_caller_{callee.name}_callee_"
                f"vlen{vlen_bits}"
            )
    print("PASS c_oracle_cross_compiler_aggregate_calls")


def expected_unimplemented(diagnostics: str) -> str | None:
    return next(
        (marker for marker in EXPECTED_UNIMPLEMENTED_MARKERS if marker in diagnostics),
        None,
    )


def assemble_yoolang_object(assembler: str, assembly: Path, output: Path) -> None:
    run_checked(
        [
            assembler,
            f"-march={MARCH}",
            f"-mabi={MABI}",
            str(assembly),
            "-o",
            str(output),
        ]
    )


def run_yoolang_case(
    case: ABICase,
    directory: Path,
    compiler: Path,
    c_compiler_set: tuple[CCompiler, CCompiler],
    assembler: str,
    nm: str,
    readelf: str,
    objdump: str,
    gcc_linker: str,
    qemu: str,
) -> bool:
    source = directory / f"{case.name}.sy"
    write_source(source, case.yoolang_source)
    c_source = directory / f"{case.name}-driver.c"
    write_source(c_source, case.c_driver)
    driver_objects: dict[str, Path] = {}
    for c_compiler in c_compiler_set:
        driver_object = directory / f"{case.name}-{c_compiler.name}-driver.o"
        run_checked(c_compiler.compile_command(c_source, driver_object, assembly=False))
        driver_objects[c_compiler.name] = driver_object

    for opt_level in OPT_LEVELS:
        assembly = directory / f"{case.name}-O{opt_level}.s"
        command = [
            str(compiler),
            str(source),
            "-S",
            f"-O{opt_level}",
            f"-march={MARCH}",
            f"-mabi={MABI}",
            "-o",
            str(assembly),
        ]
        assembly.unlink(missing_ok=True)
        result = run_process(command, timeout=90.0)
        if result.returncode != 0:
            diagnostics = result.stdout + result.stderr
            marker = expected_unimplemented(diagnostics)
            if marker is None:
                raise ProbeError(
                    f"{case.name} O{opt_level} failed for a non-allowlisted reason: "
                    f"{command_text(command)}\n{diagnostics}"
                )
            if assembly.exists() and assembly.stat().st_size != 0:
                raise ProbeError(
                    f"{case.name} O{opt_level} emitted a non-empty partial assembly on "
                    f"failure: {assembly}"
                )
            print(f"EXPECTED_UNIMPLEMENTED {case.name}_O{opt_level}: {marker}")
            return False

        assembly_text = assembly.read_text(encoding="utf-8")
        if not assembly_text.strip():
            raise ProbeError(
                f"{case.name} O{opt_level} compiler success produced empty assembly"
            )
        if not re.search(r"^\s*\.attribute\s+stack_align\s*,\s*16\s*$",
                         assembly_text, re.MULTILINE):
            raise ProbeError(
                f"{case.name} O{opt_level} does not declare 16-byte stack alignment"
            )
        if ".variant_cc" in assembly_text:
            raise ProbeError(
                f"{case.name} O{opt_level} incorrectly opted into the vector-register ABI"
            )
        if MARCH == "rv64gc" and '.attribute arch, "rv64gc"' not in assembly_text:
            raise ProbeError(
                f"{case.name} O{opt_level} portable assembly does not advertise rv64gc"
            )
        for external_symbol in case.external_symbols:
            escaped = re.escape(external_symbol)
            if not re.search(
                rf"(?:\bcall\s+{escaped}\b|%pcrel_hi\({escaped}\))",
                assembly_text,
            ):
                raise ProbeError(
                    f"{case.name} O{opt_level} lacks the required externalizable call "
                    f"to {external_symbol}"
                )

        yoolang_object = directory / f"{case.name}-O{opt_level}.o"
        assemble_yoolang_object(assembler, assembly, yoolang_object)
        elf_header = run_checked([readelf, "-h", str(yoolang_object)])
        if "RISC-V" not in elf_header:
            raise ProbeError(f"{case.name} O{opt_level} object is not RISC-V ELF")
        disassembly = run_checked([objdump, "-dr", str(yoolang_object)])
        if MARCH == "rv64gc":
            vector_opcodes = sorted(
                {
                    mnemonic
                    for mnemonic in disassembly_mnemonics(disassembly)
                    if mnemonic.startswith("v")
                }
            )
            if vector_opcodes:
                raise ProbeError(
                    f"{case.name} O{opt_level} portable object contains vector "
                    f"opcodes: {', '.join(vector_opcodes)}"
                )
            require_scalar_elf_attributes(
                run_checked([readelf, "-A", str(yoolang_object)]),
                f"{case.name} O{opt_level} portable object",
            )
        elif case.requires_rvv_opcode and not re.search(
            r"\bvset(?:i)?vli\b", disassembly
        ):
            raise ProbeError(
                f"{case.name} O{opt_level} object lacks an RVV configuration opcode"
            )
        symbols = run_checked([nm, str(yoolang_object)])
        for external_symbol in case.external_symbols:
            relocation_kinds = ["CALL_PLT"]
            if case.allows_medany_direct_call:
                relocation_kinds.append("PCREL_HI20")
            kinds = "|".join(relocation_kinds)
            if not re.search(
                rf"R_RISCV_(?:{kinds})\s+{re.escape(external_symbol)}\b",
                disassembly,
            ):
                raise ProbeError(
                    f"{case.name} O{opt_level} lacks an external-call relocation "
                    f"for {external_symbol} (expected {kinds})"
                )
            if not re.search(
                rf"^\s+U\s+{re.escape(external_symbol)}$", symbols, re.MULTILINE
            ):
                raise ProbeError(
                    f"{case.name} O{opt_level} does not retain {external_symbol} "
                    "as an undefined external symbol"
                )

        for c_compiler in c_compiler_set:
            executable = directory / f"{case.name}-O{opt_level}-{c_compiler.name}"
            run_checked(
                [
                    gcc_linker,
                    "-static",
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    "-mcmodel=medany",
                    str(yoolang_object),
                    str(driver_objects[c_compiler.name]),
                    "-o",
                    str(executable),
                ]
            )
            if MARCH == "rv64gc":
                require_scalar_elf_attributes(
                    run_checked([readelf, "-A", str(executable)]),
                    f"{case.name} O{opt_level} {c_compiler.name} executable",
                )
                linked_vector_opcodes = sorted(
                    {
                        mnemonic
                        for mnemonic in disassembly_mnemonics(
                            run_checked([objdump, "-d", str(executable)])
                        )
                        if mnemonic.startswith("v")
                    }
                )
                if linked_vector_opcodes:
                    raise ProbeError(
                        f"{case.name} O{opt_level} {c_compiler.name} executable "
                        "contains vector opcodes: "
                        + ", ".join(linked_vector_opcodes)
                    )
            for vlen_bits in VLEN_BITS:
                run_checked(
                    [qemu, "-cpu", qemu_cpu(vlen_bits), str(executable)],
                    timeout=30.0,
                )
                print(
                    f"PASS {case.name}_O{opt_level}_{c_compiler.name}_"
                    f"vlen{vlen_bits}"
                )
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Probe standard aggregate ABI interoperability for source vectors/masks"
    )
    parser.add_argument(
        "--compiler",
        type=Path,
        default=Path(os.environ.get("YOOLANG_COMPILER", DEFAULT_COMPILER)),
        help="yoolang compiler binary",
    )
    parser.add_argument(
        "--march",
        choices=("rv64gcv", "rv64gc"),
        default="rv64gcv",
        help="target RVV implementation or portable scalar aggregate boundary",
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="retain generated source, assembly, objects, and executables in this directory",
    )
    return parser.parse_args()


def run_probe(args: argparse.Namespace, directory: Path) -> int:
    global MARCH, VLEN_BITS
    MARCH = getattr(args, "march", "rv64gcv")
    VLEN_BITS = (0,) if MARCH == "rv64gc" else (128, 256, 512, 1024)
    if not args.compiler.is_file():
        raise ProbeBlocked(f"yoolang compiler is not built: {args.compiler}")

    python = require_tool("python3")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    clang = require_tool(os.environ.get("RISCV_CLANG", "clang"))
    assembler = require_tool("riscv64-linux-gnu-as")
    nm = require_tool("riscv64-linux-gnu-nm")
    readelf = require_tool("riscv64-linux-gnu-readelf")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    qemu = require_tool("qemu-riscv64")
    compiler_set = c_compilers(gcc, clang)

    run_classifier_gate(python)
    run_c_oracle(directory, compiler_set, gcc, qemu)

    implemented = 0
    unimplemented = 0
    for case in ABI_CASES:
        if run_yoolang_case(
            case,
            directory,
            args.compiler,
            compiler_set,
            assembler,
            nm,
            readelf,
            objdump,
            gcc,
            qemu,
        ):
            implemented += 1
        else:
            unimplemented += 1

    print(
        "SUMMARY standard_vector_aggregate_abi "
        f"implemented={implemented} expected_unimplemented={unimplemented}"
    )
    return 2 if unimplemented else 0


def main() -> int:
    args = parse_args()
    try:
        if args.artifacts is not None:
            args.artifacts.mkdir(parents=True, exist_ok=True)
            return run_probe(args, args.artifacts)
        with tempfile.TemporaryDirectory(prefix="yoolang-standard-vector-abi-") as temporary:
            return run_probe(args, Path(temporary))
    except ProbeBlocked as error:
        print(f"BLOCKED standard_vector_aggregate_abi: {error}", file=sys.stderr)
        return 2
    except ProbeError as error:
        print(f"FAIL standard_vector_aggregate_abi: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
