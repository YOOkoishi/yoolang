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
LANE_COUNTS = (33, 63, 65)
OPT_LEVELS = (0, 1, 2, 3)
VLENS = (128, 256, 512, 1024)


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


def vector_source(lanes: int) -> str:
    alternating = ", ".join("1" if lane % 2 == 0 else "0" for lane in range(lanes))
    reverse = ", ".join(str(lanes - 1 - lane) for lane in range(lanes))
    return textwrap.dedent(
        f"""
        int idata[{lanes + 8}] = {{}};
        vector<int,{lanes}> sink_i = vector<int,{lanes}>{{}};
        vector<int,{lanes}> sink_loaded = vector<int,{lanes}>{{}};
        vector<float,{lanes}> sink_f = vector<float,{lanes}>{{}};
        mask<{lanes}> sink_m = mask<{lanes}>{{}};
        mask<{lanes}> source_m = mask<{lanes}>{{{alternating}}};

        mask<{lanes}> mask_callee(mask<{lanes}> a, mask<{lanes}> b,
                                  int seed) {{
          mask<{lanes}> x = a ^ b;
          mask<{lanes}> y = (x & a) | (x ^ a);
          mask<{lanes}> p;
          if (seed & 1) p = y;
          else p = a;
          return p;
        }}

        vector<int,{lanes}> int_callee(vector<int,{lanes}> a,
                                       vector<int,{lanes}> b,
                                       mask<{lanes}> m, int lane, int seed) {{
          vector<int,{lanes}> ids = iota(vector<int,{lanes}>{{}});
          vector<int,{lanes}> x = (a + b) * (ids + vector<int,{lanes}>(1));
          vector<int,{lanes}> q0 = x + vector<int,{lanes}>(1);
          vector<int,{lanes}> q1 = x + vector<int,{lanes}>(2);
          vector<int,{lanes}> q2 = x + vector<int,{lanes}>(3);
          vector<int,{lanes}> q3 = x + vector<int,{lanes}>(4);
          vector<int,{lanes}> q4 = x + vector<int,{lanes}>(5);
          vector<int,{lanes}> q5 = x + vector<int,{lanes}>(6);
          vector<int,{lanes}> pressure = q0 + q1 + q2 + q3 + q4 + q5;
          mask<{lanes}> cmp = pressure >= b;
          mask<{lanes}> bits = (cmp & m) | (cmp ^ m);
          vector<int,{lanes}> selected = select(bits, pressure, b);
          selected = insert_lane(selected, lane,
                                 extract_lane(selected, lane) + 7);
          vector<int,{lanes}> reversed = shuffle(
              selected, b, vector<int,{lanes}>{{{reverse}}});
          vector<int,{lanes}> p;
          if (seed & 1) p = reversed;
          else p = b;
          return p;
        }}

        vector<float,{lanes}> float_callee(int seed, mask<{lanes}> active) {{
          vector<int,{lanes}> base_i = vector<int,{lanes}>(seed);
          vector<float,{lanes}> base = vector<float,{lanes}>(base_i);
          vector<float,{lanes}> two = vector<float,{lanes}>(2);
          vector<float,{lanes}> calc = (base + two) * two;
          mask<{lanes}> cmp = calc >= two;
          return select(cmp & active, calc, two);
        }}

        int kernel(int seed) {{
          mask<{lanes}> all = mask_callee(~source_m, source_m, seed);
          vector<int,{lanes}> a = vector<int,{lanes}>(seed);
          vector<int,{lanes}> b = vector<int,{lanes}>(2);
          vector<int,{lanes}> iv = int_callee(a, b, all, seed % {lanes}, seed);
          vector<float,{lanes}> fv = float_callee(seed, all);
          vector<int,{lanes}> fi = vector<int,{lanes}>(fv);
          vector<int,{lanes}> loaded = masked_load(idata, source_m, iv);
          masked_store(idata, source_m, loaded);
          sink_i = iv;
          sink_loaded = loaded;
          sink_f = fv;
          sink_m = all;
          return iv[0] + iv[{lanes - 1}] + fi[0] + all[{lanes - 1}] +
                 loaded[0] + loaded[1];
        }}
        """
    ).strip() + "\n"


def c_driver(lanes: int) -> str:
    mask_bytes = (lanes + 7) // 8
    valid_bits = lanes % 8
    final_mask = 0xFF if valid_bits == 0 else (1 << valid_bits) - 1
    return textwrap.dedent(
        f"""
        #include <stdint.h>

        extern int kernel(int);
        extern int32_t idata[{lanes + 8}];
        extern int32_t sink_i[{lanes}];
        extern int32_t sink_loaded[{lanes}];
        extern float sink_f[{lanes}];
        extern uint8_t sink_m[{mask_bytes}];

        int main(void) {{
          for (int lane = 0; lane < {lanes + 8}; ++lane)
            idata[lane] = 100 + lane;
          if (kernel(5) != {84 * lanes + 178}) return 1;
          for (int lane = 0; lane < {lanes}; ++lane) {{
            int source_lane = {lanes - 1} - lane;
            int32_t expected_i = 42 * (source_lane + 1) + 21;
            if (source_lane == 5) expected_i += 7;
            if (sink_i[lane] != expected_i) return 10 + lane;
            int32_t expected_loaded = (lane & 1) ? expected_i : 100 + lane;
            if (sink_loaded[lane] != expected_loaded) return 100 + lane;
            if (sink_f[lane] != 14.0f) return 200 + lane;
            if ((sink_m[lane / 8] & (uint8_t)(1u << (lane % 8))) == 0)
              return 300 + lane;
            if (idata[lane] != 100 + lane) return 400 + lane;
          }}
          if ((sink_m[{mask_bytes - 1}] & (uint8_t)~{final_mask}) != 0)
            return 500;
          return 0;
        }}
        """
    ).strip() + "\n"


def validate_assembly(assembly: str, lanes: int) -> None:
    for symbol in ("mask_callee", "int_callee", "float_callee", "kernel"):
        if re.search(rf"^{symbol}:$", assembly, re.MULTILINE) is None:
            raise RuntimeError(f"N={lanes} assembly lacks {symbol}")
    if re.search(r"\bvsetvli\b.*\be32,\s*m8\b", assembly) is None:
        raise RuntimeError(f"N={lanes} lacks the leading 32-lane m8 piece")
    if lanes in (33, 65):
        if re.search(r"\bvsetivli\b[^\n]*,\s*1,\s*e32,\s*mf2\b", assembly) is None:
            raise RuntimeError(f"N={lanes} lacks its one-lane mf2 tail piece")
    if lanes == 63:
        if re.search(r"\bvsetivli\b[^\n]*,\s*31,\s*e32,\s*m8\b", assembly) is None:
            raise RuntimeError("N=63 lacks its 31-lane m8 tail piece")
    for opcode in ("vadd.vv", "vmul.vv", "vm", "vmerge.vvm", "vle32.v", "vse32.v", "vlm.v", "vsm.v"):
        if opcode not in assembly:
            raise RuntimeError(f"N={lanes} assembly lacks expected RVV opcode fragment {opcode}")
    for call in ("call mask_callee", "call int_callee", "call float_callee"):
        if call not in assembly:
            raise RuntimeError(f"N={lanes} assembly lacks standard aggregate {call}")
    for pseudo in ("Pseudo", "RVVIntBinary", "RVVMask", "RVVLoad"):
        if pseudo in assembly:
            raise RuntimeError(f"N={lanes} final assembly leaks pseudo text {pseudo}")


def qemu_cpu(vlen: int) -> str:
    return f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0"


def main() -> int:
    if not COMPILER.is_file():
        print(f"compiler not found: {COMPILER}", file=sys.stderr)
        return 1
    assembler = require_tool("riscv64-linux-gnu-as")
    compiler = require_tool("riscv64-linux-gnu-gcc")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    qemu = require_tool("qemu-riscv64")

    with tempfile.TemporaryDirectory(prefix="yoolang-rvv-fixed-chunks-") as tmp:
        tmpdir = Path(tmp)
        for lanes in LANE_COUNTS:
            source = tmpdir / f"chunk_{lanes}.sy"
            driver = tmpdir / f"chunk_{lanes}_driver.c"
            source.write_text(vector_source(lanes), encoding="utf-8")
            driver.write_text(c_driver(lanes), encoding="utf-8")
            driver_object = tmpdir / f"chunk_{lanes}_driver.o"
            run(
                [
                    compiler,
                    "-c",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    str(driver),
                    "-o",
                    str(driver_object),
                ]
            )

            for opt in OPT_LEVELS:
                assembly = tmpdir / f"chunk_{lanes}_O{opt}.s"
                object_file = tmpdir / f"chunk_{lanes}_O{opt}.o"
                executable = tmpdir / f"chunk_{lanes}_O{opt}"
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
                validate_assembly(assembly_text, lanes)
                run([assembler, "-march=rv64gcv", str(assembly), "-o", str(object_file)])
                disassembly = run([objdump, "-dr", str(object_file)]).stdout
                if "vset" not in disassembly or "vse32.v" not in disassembly:
                    raise RuntimeError(f"N={lanes} O{opt} object lost RVV instructions")
                run(
                    [
                        compiler,
                        "-static",
                        "-march=rv64gcv",
                        "-mabi=lp64d",
                        str(object_file),
                        str(driver_object),
                        "-o",
                        str(executable),
                    ]
                )
                for vlen in VLENS:
                    run([qemu, "-cpu", qemu_cpu(vlen), str(executable)])
                print(
                    f"PASS fixed_chunk_N{lanes}_O{opt}_qemu_vlen128_256_512_1024"
                )

    print("PASS fixed_chunk_gnu_as_objdump_zero_pseudo")
    print("13/13 fixed chunk backend checks passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
