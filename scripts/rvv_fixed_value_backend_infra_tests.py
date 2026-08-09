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
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
LANE_COUNTS = (1, 3, 7, 31)
OPT_LEVELS = (0, 1, 2, 3)
VLENS = (128, 256, 512, 1024)


def wide_source_fixture() -> str:
    lanes = 31
    mask = [1 if index % 2 == 0 else 0 for index in range(lanes)]
    shuffle = [5] + [
        index if index % 2 == 0 else lanes + (lanes - 1 - index) % lanes
        for index in range(1, lanes)
    ]
    mask_text = ", ".join(str(value) for value in mask)
    shuffle_text = ", ".join(str(value) for value in shuffle)
    return textwrap.dedent(
        f"""
        mask<31> source_m = mask<31>{{{mask_text}}};
        vector<int,31> sink_v = vector<int,31>{{}};
        mask<31> sink_m = mask<31>{{}};

        int div_probe(int seed) {{
          int lane = seed % 31;
          vector<int,31> values =
              vector<int,31>(seed) + iota(vector<int,31>{{}});
          return extract_lane(values / vector<int,31>(2), lane);
        }}

        int rem_probe(int seed) {{
          int lane = seed % 31;
          vector<int,31> values =
              vector<int,31>(seed) + iota(vector<int,31>{{}});
          return extract_lane(values % vector<int,31>(2), lane);
        }}

        int edge_div_probe() {{
          int edge = -2147483647 - 1;
          vector<int,31> edge_values = vector<int,31>(edge);
          return extract_lane(edge_values / vector<int,31>(-1), 0) == edge;
        }}

        int edge_rem_probe() {{
          int edge = -2147483647 - 1;
          vector<int,31> edge_values = vector<int,31>(edge);
          return extract_lane(edge_values % vector<int,31>(-1), 0) == 0;
        }}

        int float_add_probe(int seed) {{
          vector<float,31> base =
              vector<float,31>(vector<int,31>(seed));
          vector<float,31> two = vector<float,31>(2);
          vector<int,31> result = vector<int,31>(base + two);
          return result[0];
        }}

        int float_sub_probe(int seed) {{
          vector<float,31> base =
              vector<float,31>(vector<int,31>(seed));
          vector<float,31> two = vector<float,31>(2);
          vector<int,31> result = vector<int,31>(base - two);
          return result[0];
        }}

        int float_mul_probe(int seed) {{
          vector<float,31> base =
              vector<float,31>(vector<int,31>(seed));
          vector<float,31> two = vector<float,31>(2);
          vector<int,31> result = vector<int,31>(base * two);
          return result[0];
        }}

        int float_div_probe(int seed) {{
          vector<float,31> base =
              vector<float,31>(vector<int,31>(seed));
          vector<float,31> two = vector<float,31>(2);
          vector<int,31> result = vector<int,31>(base / two);
          return result[0];
        }}

        int float_cmp_probe(int seed) {{
          vector<float,31> base =
              vector<float,31>(vector<int,31>(seed));
          mask<31> compared = base >= base;
          return compared[0];
        }}

        int kernel(int seed) {{
          int lane = seed % 31;
          vector<int,31> a = vector<int,31>(seed);
          vector<int,31> b = vector<int,31>(2);
          vector<int,31> sum = a + b;
          mask<31> compared = sum >= b;
          mask<31> literal = source_m;
          mask<31> bits = (compared & ~literal) | (compared ^ literal);
          vector<int,31> selected = select(bits, sum, b);
          selected = insert_lane(selected, lane,
                                 extract_lane(selected, lane) + 11);
          vector<int,31> shuffled = shuffle(
              selected, b, vector<int,31>{{{shuffle_text}}});
          vector<int,31> p;
          mask<31> pm;
          if (seed & 1) {{
            p = shuffled;
            pm = bits;
          }} else {{
            p = b;
            pm = literal;
          }}
          pm[lane] = literal[0];
          sink_v = p;
          sink_m = pm;
          return p[0] + pm[0];
        }}
        """
    ).strip() + "\n"


def wide_c_oracle_driver() -> str:
    mask = [1 if index % 2 == 0 else 0 for index in range(31)]
    shuffle = [5] + [
        index if index % 2 == 0 else 31 + (31 - 1 - index) % 31
        for index in range(1, 31)
    ]
    mask_text = ", ".join(str(value) for value in mask)
    shuffle_text = ", ".join(str(value) for value in shuffle)
    return textwrap.dedent(
        f"""
        #include <stdint.h>

        extern int div_probe(int);
        extern int rem_probe(int);
        extern int edge_div_probe(void);
        extern int edge_rem_probe(void);
        extern int float_add_probe(int);
        extern int float_sub_probe(int);
        extern int float_mul_probe(int);
        extern int float_div_probe(int);
        extern int float_cmp_probe(int);
        extern int kernel(int);
        extern int32_t sink_v[31];
        extern uint8_t sink_m[4];

        static int check_kernel(int seed) {{
          int literal[31] = {{{mask_text}}};
          int shuffle[31] = {{{shuffle_text}}};
          int32_t selected[31];
          int bits[31];
          int pm[31];
          for (int i = 0; i < 31; ++i) {{
            int compared = 1;
            bits[i] = (compared & !literal[i]) | (compared ^ literal[i]);
            selected[i] = bits[i] ? seed + 2 : 2;
          }}
          int lane = seed % 31;
          selected[lane] += 11;
          int32_t expected[31];
          for (int i = 0; i < 31; ++i) {{
            int index = shuffle[i];
            int32_t shuffled = index < 31 ? selected[index] : 2;
            expected[i] = (seed & 1) ? shuffled : 2;
            pm[i] = (seed & 1) ? bits[i] : literal[i];
          }}
          pm[lane] = literal[0];
          if (kernel(seed) != expected[0] + pm[0]) return 1;
          for (int i = 0; i < 31; ++i)
            if (sink_v[i] != expected[i]) return 2 + i;
          uint8_t packed[4] = {{0, 0, 0, 0}};
          for (int i = 0; i < 31; ++i)
            packed[i / 8] |= (uint8_t)(pm[i] << (i % 8));
          for (int i = 0; i < 4; ++i)
            if (sink_m[i] != packed[i]) return 40 + i;
          if ((sink_m[3] & 0x80u) != 0) return 50;
          return 0;
        }}

        int main(void) {{
          if (div_probe(5) != 5) return 1;
          if (rem_probe(5) != 0) return 2;
          if (!edge_div_probe()) return 3;
          if (!edge_rem_probe()) return 4;
          if (float_add_probe(5) != 7) return 5;
          if (float_sub_probe(5) != 3) return 6;
          if (float_mul_probe(5) != 10) return 7;
          if (float_div_probe(5) != 2) return 8;
          if (float_cmp_probe(5) != 1) return 9;
          int odd = check_kernel(5);
          if (odd != 0) return 10 + odd;
          int even = check_kernel(4);
          if (even != 0) return 100 + even;
          return 0;
        }}
        """
    ).strip() + "\n"


def require_tool(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise RuntimeError(f"required tool not found in PATH: {name}")
    return resolved


def run(command: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def assembly_function(assembly: str, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}:\n(?P<body>.*?)(?=^\s*\.size\s+"
        rf"{re.escape(name)},)",
        assembly,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"assembly lacks function body: {name}")
    return match.group("body")


def mir_function(module: str, name: str) -> str:
    match = re.search(
        rf"^func\s+@{re.escape(name)}\([^\n]*\)\s+->\s+[^\n]+\s+\{{\n"
        rf"(?P<body>.*?)(?=^\}}\s*$)",
        module,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"MIR lacks function body: {name}")
    return match.group("body")


def validate_dominating_whole_spills(post_ra_module: str, name: str) -> None:
    body = mir_function(post_ra_module, name)
    current_block = ""
    entry_block = ""
    spills_by_block: dict[str, set[int]] = {}
    spill_counts: dict[int, int] = {}
    reload_counts: dict[int, int] = {}
    block_pattern = re.compile(r"^([A-Za-z_.$][A-Za-z0-9_.$-]*):\s*$")
    spill_pattern = re.compile(r"^\s+PseudoVSPILL_WHOLE\s+fi#(\d+),")
    reload_pattern = re.compile(r"^\s+PseudoVRELOAD_WHOLE\s+.*?,\s+fi#(\d+),")

    for line in body.splitlines():
        block_match = block_pattern.match(line)
        if block_match is not None:
            current_block = block_match.group(1)
            if not entry_block:
                entry_block = current_block
            spills_by_block.setdefault(current_block, set())
            continue
        spill_match = spill_pattern.match(line)
        if spill_match is not None:
            if not current_block:
                raise RuntimeError(f"@{name} whole spill appears outside a basic block")
            slot = int(spill_match.group(1))
            spills_by_block[current_block].add(slot)
            spill_counts[slot] = spill_counts.get(slot, 0) + 1
            continue
        reload_match = reload_pattern.match(line)
        if reload_match is None:
            continue
        if not current_block:
            raise RuntimeError(f"@{name} whole reload appears outside a basic block")
        slot = int(reload_match.group(1))
        reload_counts[slot] = reload_counts.get(slot, 0) + 1
        # This generated fixture has a single dominating entry plus an if
        # diamond.  A definition earlier in the same block or in entry is
        # therefore present on every path reaching the reload.  Deliberately
        # do not compare raw store/load counts: one SSA backing slot may be
        # reloaded for multiple uses after its scratch group is reused.
        locally_defined = slot in spills_by_block[current_block]
        entry_defined = slot in spills_by_block.get(entry_block, set())
        if not locally_defined and not entry_defined:
            raise RuntimeError(
                f"@{name} reload of fi#{slot} lacks a dominating whole spill"
            )

    if not spill_counts or not reload_counts:
        raise RuntimeError(f"@{name} lacks real whole-register spill/reload traffic")
    orphan_stores = sorted(set(spill_counts) - set(reload_counts))
    orphan_loads = sorted(set(reload_counts) - set(spill_counts))
    if orphan_stores or orphan_loads:
        raise RuntimeError(
            f"@{name} whole-register slots are not used bidirectionally: "
            f"store-only={orphan_stores}, load-only={orphan_loads}"
        )


def source_fixture(lanes: int) -> str:
    if lanes == 31:
        return wide_source_fixture()
    integers = [index - 3 for index in range(lanes)]
    denominators = [-1 if index % 3 == 0 else index % 5 + 2 for index in range(lanes)]
    mask = [1 if index % 2 == 0 else 0 for index in range(lanes)]
    shuffle = [
        (index * 3) % lanes if index % 2 == 0 else lanes + (lanes - 1 - index) % lanes
        for index in range(lanes)
    ]

    def values(items: list[int]) -> str:
        return ", ".join(str(item) for item in items)

    return textwrap.dedent(
        f"""
        mask<{lanes}> source_m = mask<{lanes}>{{{values(mask)}}};
        vector<int,{lanes}> sink_v = vector<int,{lanes}>{{}};
        mask<{lanes}> sink_m = mask<{lanes}>{{}};

        int kernel(int seed) {{
          vector<int,{lanes}> a = vector<int,{lanes}>{{{values(integers)}}};
          vector<int,{lanes}> splat = vector<int,{lanes}>(seed);
          vector<int,{lanes}> ids = iota(vector<int,{lanes}>{{}});
          vector<int,{lanes}> mixed =
              (a + splat) * (ids + vector<int,{lanes}>(1));
          vector<int,{lanes}> denom =
              vector<int,{lanes}>{{{values(denominators)}}};
          vector<int,{lanes}> numeric = mixed / denom + mixed % denom;
          int edge = -2147483647 - 1;
          vector<int,{lanes}> edge_values = vector<int,{lanes}>(edge);
          vector<int,{lanes}> edge_quot =
              edge_values / vector<int,{lanes}>(-1);
          vector<int,{lanes}> edge_rem =
              edge_values % vector<int,{lanes}>(-1);
          int edge_ok = edge_quot[0] == edge && edge_rem[0] == 0;
          vector<float,{lanes}> f = vector<float,{lanes}>(numeric);
          vector<float,{lanes}> two = vector<float,{lanes}>(2);
          vector<float,{lanes}> fcalc = ((f + two) - two) * two / two;
          vector<int,{lanes}> round = vector<int,{lanes}>(fcalc);
          vector<float,{lanes}> af = vector<float,{lanes}>(a);
          mask<{lanes}> cmp = numeric >= a;
          mask<{lanes}> fcmp = fcalc > af;
          mask<{lanes}> literal = source_m;
          mask<{lanes}> bits = (cmp & ~literal) | (fcmp ^ literal);
          vector<int,{lanes}> selected = select(bits, round, a);
          int lane = seed % {lanes};
          selected = insert_lane(selected, lane,
                                 extract_lane(selected, lane) + 11);
          vector<int,{lanes}> shuffled = shuffle(
              selected, a, vector<int,{lanes}>{{{values(shuffle)}}});
          vector<int,{lanes}> p;
          mask<{lanes}> pm;
          if (seed & 1) {{
            p = shuffled;
            pm = (literal & bits) | (~literal & cmp);
          }} else {{
            p = a;
            pm = literal;
          }}
          pm[lane] = literal[0];
          sink_v = p;
          sink_m = pm;
          return p[lane] + pm[lane] + edge_ok - 1;
        }}
        """
    ).strip() + "\n"


def c_oracle_driver(lanes: int) -> str:
    if lanes == 31:
        return wide_c_oracle_driver()
    integers = [index - 3 for index in range(lanes)]
    denominators = [-1 if index % 3 == 0 else index % 5 + 2 for index in range(lanes)]
    mask = [1 if index % 2 == 0 else 0 for index in range(lanes)]
    shuffle = [
        (index * 3) % lanes if index % 2 == 0 else lanes + (lanes - 1 - index) % lanes
        for index in range(lanes)
    ]

    def values(items: list[int]) -> str:
        return ", ".join(str(item) for item in items)

    return textwrap.dedent(
        f"""
        #include <stdint.h>

        extern int kernel(int);
        extern int32_t sink_v[{lanes}];
        extern uint8_t sink_m[{(lanes + 7) // 8}];

        int main(void) {{
          const int seed = 5;
          int32_t a[{lanes}] = {{{values(integers)}}};
          int32_t denom[{lanes}] = {{{values(denominators)}}};
          int literal[{lanes}] = {{{values(mask)}}};
          int shuffle[{lanes}] = {{{values(shuffle)}}};
          int cmp[{lanes}];
          int fcmp[{lanes}];
          int bits[{lanes}];
          int pm[{lanes}];
          int32_t selected[{lanes}];
          int32_t expected[{lanes}];
          for (int i = 0; i < {lanes}; ++i) {{
            int32_t mixed = (a[i] + seed) * (i + 1);
            int32_t numeric = mixed / denom[i] + mixed % denom[i];
            cmp[i] = numeric >= a[i];
            fcmp[i] = numeric > a[i];
            bits[i] = (cmp[i] & !literal[i]) | (fcmp[i] ^ literal[i]);
            selected[i] = bits[i] ? numeric : a[i];
          }}
          int lane = seed % {lanes};
          selected[lane] += 11;
          for (int i = 0; i < {lanes}; ++i) {{
            int index = shuffle[i];
            expected[i] = index < {lanes} ? selected[index] : a[index - {lanes}];
            pm[i] = literal[i] ? bits[i] : cmp[i];
          }}
          pm[lane] = literal[0];
          int result = kernel(seed);
          if (result != expected[lane] + pm[lane]) return 10;
          for (int i = 0; i < {lanes}; ++i)
            if (sink_v[i] != expected[i]) return 20 + i;
          uint8_t packed[{(lanes + 7) // 8}] = {{0}};
          for (int i = 0; i < {lanes}; ++i)
            packed[i / 8] |= (uint8_t)(pm[i] << (i % 8));
          for (int i = 0; i < {(lanes + 7) // 8}; ++i)
            if (sink_m[i] != packed[i]) return 60 + i;
          if ((sink_m[{(lanes - 1) // 8}] &
               (uint8_t)~((1u << {lanes % 8 if lanes % 8 else 8}) - 1u)) != 0)
            return 90;
          return 0;
        }}
        """
    ).strip() + "\n"


def call_and_stack_arg_source_fixture() -> str:
    return textwrap.dedent(
        """
        vector<int,31> call_sink = vector<int,31>{};

        int nested_leaf(int x) {
          return x * 3 + 1;
        }

        int nested_mid(int x) {
          return nested_leaf(x + 2) - 4;
        }

        int ninth_with_vector_spill(int p0, int p1, int p2, int p3,
                                    int p4, int p5, int p6, int p7,
                                    int p8) {
          vector<int,31> base =
              vector<int,31>(p8) + iota(vector<int,31>{});
          vector<int,31> doubled = base * vector<int,31>(2);
          int nested = nested_mid(p8);
          putch(0);
          vector<int,31> result = doubled + vector<int,31>(nested);
          call_sink = result;
          return result[0] + result[30] + p0 + p1 + p2 + p3 + p4 +
                 p5 + p6 + p7 + p8;
        }
        """
    ).strip() + "\n"


def call_and_stack_arg_driver() -> str:
    return textwrap.dedent(
        """
        #include <stdint.h>

        extern int ninth_with_vector_spill(int, int, int, int, int, int, int,
                                           int, int);
        extern int32_t call_sink[31];

        int main(void) {
          int result = ninth_with_vector_spill(1, 2, 3, 4, 5, 6, 7, 8, 9);
          if (result != 201) return 1;
          for (int lane = 0; lane < 31; ++lane) {
            int32_t expected = 48 + 2 * lane;
            if (call_sink[lane] != expected) return 2 + lane;
          }
          return 0;
        }
        """
    ).strip() + "\n"


def final_mir(source: Path, opt: int) -> str:
    return run(
        [
            str(COMPILER),
            "--emit-mir",
            "--emit-mir-stage=final",
            f"-O{opt}",
            "-march=rv64gcv",
            str(source),
        ]
    ).stdout


def post_ra_mir(source: Path, opt: int) -> str:
    return run(
        [
            str(COMPILER),
            "--emit-mir",
            "--emit-mir-stage=post-ra",
            f"-O{opt}",
            "-march=rv64gcv",
            str(source),
        ]
    ).stdout


def compile_and_run_source_matrix(tmp: Path) -> None:
    gcc = require_tool("riscv64-linux-gnu-gcc")
    assembler = require_tool("riscv64-linux-gnu-as")
    qemu = require_tool("qemu-riscv64")
    required_mnemonics = {
        "vadd.vv",
        "vmul.vv",
        "vdiv.vv",
        "vrem.vv",
        "vfadd.vv",
        "vfsub.vv",
        "vfmul.vv",
        "vfdiv.vv",
        "vmerge.vvm",
        "vfcvt.f.x.v",
        "vfcvt.rtz.x.f.v",
        "vslidedown.vx",
        "vid.v",
        "vlm.v",
        "vsm.v",
    }
    seen: set[str] = set()

    for lanes in LANE_COUNTS:
        source = tmp / f"fixed_n{lanes}.sy"
        driver = tmp / f"fixed_n{lanes}_driver.c"
        source.write_text(source_fixture(lanes), encoding="utf-8")
        driver.write_text(c_oracle_driver(lanes), encoding="utf-8")
        for opt in OPT_LEVELS:
            assembly = tmp / f"fixed_n{lanes}_o{opt}.s"
            obj = tmp / f"fixed_n{lanes}_o{opt}.o"
            binary = tmp / f"fixed_n{lanes}_o{opt}"
            run(
                [
                    str(COMPILER),
                    str(source),
                    "-S",
                    f"-O{opt}",
                    "-march=rv64gcv",
                    "-o",
                    str(assembly),
                ]
            )
            assembly_text = assembly.read_text(encoding="utf-8")
            seen.update(
                mnemonic for mnemonic in required_mnemonics if mnemonic in assembly_text
            )
            if lanes == 31:
                kernel_text = assembly_function(assembly_text, "kernel")
                if "csrr t6, vlenb" not in kernel_text:
                    raise RuntimeError(
                        f"N=31 O{opt} @kernel lacks scalable-frame vlenb access"
                    )
                spill_count = len(
                    re.findall(r"^\s*vs8r\.v\b", kernel_text, re.MULTILINE)
                )
                reload_count = len(
                    re.findall(r"^\s*vl8re32\.v\b", kernel_text, re.MULTILINE)
                )
                if spill_count < 1 or reload_count < 1:
                    raise RuntimeError(
                        f"N=31 O{opt} @kernel lacks M8 spill traffic: "
                        f"stores={spill_count}, reloads={reload_count}"
                    )
                validate_dominating_whole_spills(post_ra_mir(source, opt), "kernel")
            if lanes % 8 != 0:
                for fragment in ("lbu t5", "andi t5", "sb t5"):
                    if fragment not in assembly_text:
                        raise RuntimeError(
                            f"N={lanes} O{opt} mask store lacks exact tail cleanup: {fragment}"
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
            run(
                [
                    gcc,
                    "-static",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    str(assembly),
                    str(driver),
                    str(RUNTIME),
                    "-o",
                    str(binary),
                ]
            )
            mir = final_mir(source, opt)
            if "Pseudo" in mir or re.search(r"\b(?:LI|LA|LOAD_SLOT|STORE_SLOT)\b", mir):
                raise RuntimeError(f"N={lanes} O{opt} Final MIR retains a pseudo")
            for vlen in VLENS:
                run(
                    [
                        qemu,
                        "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                        str(binary),
                    ]
                )
        print(f"PASS fixed_source_n{lanes}_o0_o1_o2_o3_all_vlen")

    missing = sorted(required_mnemonics - seen)
    if missing:
        raise RuntimeError("source matrix lacks RVV mnemonics: " + ", ".join(missing))
    print("PASS fixed_source_real_opcode_coverage")


def test_whole_register_assembler(tmp: Path) -> None:
    assembler = require_tool("riscv64-linux-gnu-as")
    source = tmp / "whole-register-families.s"
    obj = tmp / "whole-register-families.o"
    source.write_text(
        textwrap.dedent(
            """
            .attribute arch, "rv64gcv"
            .text
            .globl whole_register_families
            .type whole_register_families, @function
            whole_register_families:
              vs1r.v v1, (a0)
              vl1re32.v v1, (a0)
              vs2r.v v2, (a0)
              vl2re32.v v2, (a0)
              vs4r.v v4, (a0)
              vl4re32.v v4, (a0)
              vs8r.v v8, (a0)
              vl8re32.v v8, (a0)
              jalr zero, 0(ra)
            .size whole_register_families, .-whole_register_families
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )
    run(
        [
            assembler,
            "-march=rv64gcv",
            "-mabi=lp64d",
            str(source),
            "-o",
            str(obj),
        ]
    )
    print("PASS whole_register_m1_m2_m4_m8_gnu_as")


def test_call_live_and_ninth_stack_arg(tmp: Path) -> None:
    assembler = require_tool("riscv64-linux-gnu-as")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    qemu = require_tool("qemu-riscv64")
    source = tmp / "call_live_ninth.sy"
    driver = tmp / "call_live_ninth_driver.c"
    source.write_text(call_and_stack_arg_source_fixture(), encoding="utf-8")
    driver.write_text(call_and_stack_arg_driver(), encoding="utf-8")

    for opt in OPT_LEVELS:
        assembly = tmp / f"call_live_ninth_o{opt}.s"
        obj = tmp / f"call_live_ninth_o{opt}.o"
        binary = tmp / f"call_live_ninth_o{opt}"
        run(
            [
                str(COMPILER),
                str(source),
                "-S",
                f"-O{opt}",
                "-march=rv64gcv",
                "-o",
                str(assembly),
            ]
        )
        assembly_text = assembly.read_text(encoding="utf-8")
        body = assembly_function(assembly_text, "ninth_with_vector_spill")
        for fragment in (
            "putch",
            "csrr t6, vlenb",
            "addi t6, t6, 15",
            "andi t6, t6, -16",
            "vs8r.v",
            "vl8re32.v",
        ):
            if fragment not in body:
                raise RuntimeError(
                    f"O{opt} ninth-argument call-live function lacks {fragment}"
                )
        if opt == 0 and "nested_mid" not in body:
            raise RuntimeError("O0 nested call was not retained in the callee")
        mir = final_mir(source, opt)
        if "Pseudo" in mir or re.search(r"\b(?:LI|LA|LOAD_SLOT|STORE_SLOT)\b", mir):
            raise RuntimeError(f"call-live O{opt} Final MIR retains a pseudo")
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
        run(
            [
                gcc,
                "-static",
                "-march=rv64gcv",
                "-mabi=lp64d",
                str(assembly),
                str(driver),
                str(RUNTIME),
                "-o",
                str(binary),
            ]
        )
        for vlen in VLENS:
            run(
                [
                    qemu,
                    "-cpu",
                    f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                    str(binary),
                ]
            )
    print("PASS vector_live_across_call_nested_call_and_ninth_stack_arg")


def main() -> int:
    try:
        if not COMPILER.is_file():
            raise RuntimeError(f"compiler not found: {COMPILER}")
        if not RUNTIME.is_file():
            raise RuntimeError(f"RISC-V runtime not found: {RUNTIME}")
        with tempfile.TemporaryDirectory(prefix="rvv-fixed-values-") as directory:
            temporary = Path(directory)
            compile_and_run_source_matrix(temporary)
            test_whole_register_assembler(temporary)
            test_call_live_and_ninth_stack_arg(temporary)
        return 0
    except RuntimeError as error:
        print(f"FAIL rvv_fixed_values: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
