#!/usr/bin/env python3
"""
Instruction-level comparison of yoolang vs GCC vs Clang generated RISC-V code.

Compiles each .sy benchmark with all three compilers, disassembles the output,
classifies each instruction (alu, load, store, branch, div, mul, fp, etc.),
and produces a per-function and per-benchmark diff report showing what kinds
of instructions GCC/Clang save vs yoolang.

Usage:
    python gcc_clang_diff.py [--suite performance] [--case sl3]
    python gcc_clang_diff.py --json        # JSON output for CI
    python gcc_clang_diff.py --summary     # One-line per benchmark for CI
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Optional


WORKSPACE = Path(__file__).resolve().parents[2]

COMPILER_BIN = WORKSPACE / "build" / "linux" / "x86_64" / "release" / "compiler"
RUNTIME_LIB = WORKSPACE / "runtime" / "libsysy_riscv.a"
RUNTIME_WRAPPER = WORKSPACE / "build" / "gcc-clang-diff-runtime" / "sylib_wrapper.hpp"

GCC_BIN = os.environ.get("RISCV_GCC", "riscv64-linux-gnu-g++")
CLANG_BIN = os.environ.get("RISCV_CLANGXX", "clang++")
OBJDUMP_BIN = os.environ.get("RISCV_OBJDUMP", "riscv64-linux-gnu-objdump")
QEMU_BIN = os.environ.get("QEMU_RISCV64", "qemu-riscv64")

RISCV_CODE_MODEL = ["-mcmodel=medany"]

# --- RISC-V instruction classification ---

_INSN_CLASSIFIERS: list[tuple[str, str]] = [
    (r"^(div|divu|rem|remu)w?$", "div"),
    (r"^(mul|mulh|mulhu|mulhsu)w?$", "mul"),
    (r"^(l[bhw]|l[bhw]u|ld|fl[whd]|fld)$", "load"),
    (r"^(s[bhw]|sd|fs[whd]|fsd)$", "store"),
    (r"^(beq|bne|blt|bge|bltu|bgeu)$", "branch"),
    (r"^(j|jal|jalr|jr|ret)$", "jump"),
    (r"^f(add|sub|mul|div|sqrt|madd|nmadd|msub|nmsub|min|max|eq|lt|le|gt|ge|cvt|class|mv|neg|abs|copysign|sgnj)",
     "fp"),
    (r"^call$", "call"),
    (r"^(mv|sext\.w|zext\.w|not|neg)w?$", "alu"),
    (r"^(sll|srl|sra)i?w?$", "alu"),
    (r"^\S+$", "alu"),
]

_OVERHEAD_MNEMONICS = {
    "mv", "li", "lui", "auipc", "nop",
    "sext.w", "zext.w",
    "slli", "srli", "srai",
}


def classify_instruction(line: str) -> str:
    line = line.strip()
    if not line or line.startswith("#") or line.startswith("."):
        return "other"
    parts = line.split("\t")
    if len(parts) >= 4:
        mnemonic_line = parts[-2].strip()
    elif len(parts) >= 2 and re.match(r"^\s*[0-9a-f]+:", parts[0]):
        mnemonic_line = parts[1].strip()
    elif len(parts) >= 2:
        mnemonic_line = parts[1].strip()
    else:
        mnemonic_line = line
    for pattern, category in _INSN_CLASSIFIERS:
        if re.match(pattern, mnemonic_line):
            return category
    return "other"


def is_overhead(line: str) -> bool:
    line = line.strip()
    parts = line.split("\t")
    if len(parts) >= 4:
        mnemonic_line = parts[-2].strip()
    elif len(parts) >= 2 and re.match(r"^\s*[0-9a-f]+:", parts[0]):
        mnemonic_line = parts[1].strip()
    elif len(parts) >= 2:
        mnemonic_line = parts[1].strip()
    else:
        mnemonic_line = line
    for mnem in _OVERHEAD_MNEMONICS:
        if mnemonic_line == mnem or mnemonic_line.startswith(mnem + "."):
            return True
    return False


@dataclasses.dataclass
class FuncStats:
    name: str
    total: int = 0
    alu: int = 0
    load: int = 0
    store: int = 0
    branch: int = 0
    jump: int = 0
    div: int = 0
    mul: int = 0
    fp: int = 0
    call: int = 0
    overhead: int = 0

    def add_insn(self, line: str) -> None:
        cat = classify_instruction(line)
        self.total += 1
        if cat == "alu":
            self.alu += 1
        elif cat == "load":
            self.load += 1
        elif cat == "store":
            self.store += 1
        elif cat == "branch":
            self.branch += 1
        elif cat == "jump":
            self.jump += 1
        elif cat == "div":
            self.div += 1
        elif cat == "mul":
            self.mul += 1
        elif cat == "fp":
            self.fp += 1
        elif cat == "call":
            self.call += 1
        if is_overhead(line):
            self.overhead += 1

    def to_dict(self) -> dict:
        return {
            "total": self.total, "alu": self.alu, "load": self.load,
            "store": self.store, "branch": self.branch, "jump": self.jump,
            "div": self.div, "mul": self.mul, "fp": self.fp,
            "call": self.call, "overhead": self.overhead,
        }


@dataclasses.dataclass
class CompilerStats:
    compiler: str
    functions: dict[str, FuncStats] = dataclasses.field(default_factory=dict)
    total: FuncStats = dataclasses.field(default_factory=lambda: FuncStats("TOTAL"))


def parse_objdump(output: str) -> CompilerStats:
    stats = CompilerStats("unknown")
    current_func: Optional[FuncStats] = None
    for line in output.splitlines():
        func_match = re.match(r"^[0-9a-f]+\s+<([^>]+)>:\s*$", line.strip())
        if func_match:
            func_name = func_match.group(1)
            if func_name not in stats.functions:
                stats.functions[func_name] = FuncStats(func_name)
            current_func = stats.functions[func_name]
            continue
        if current_func is not None and re.match(r"^\s*[0-9a-f]+:\s+[0-9a-f ]+\s+\w", line):
            current_func.add_insn(line)
            stats.total.add_insn(line)
    return stats


# --- Compilation ---

def ensure_runtime_wrapper() -> Path:
    wrapper_dir = RUNTIME_WRAPPER.parent
    wrapper_dir.mkdir(parents=True, exist_ok=True)
    wrapper_content = (
        '#ifndef SYLIB_WRAPPER_HPP\n#define SYLIB_WRAPPER_HPP\n'
        'extern "C" {\n'
        'int getint(); int getch(); int getarray(int a[]);\n'
        'float getfloat(); int getfarray(float a[]);\n'
        'void putint(int a); void putch(int a);\n'
        'void putarray(int n, int a[]); void putfloat(float a);\n'
        'void putfarray(int n, float a[]); void putf(char a[], ...);\n'
        'void _sysy_starttime(int lineno); void _sysy_stoptime(int lineno);\n'
        '}\n'
        '#define starttime() _sysy_starttime(__LINE__)\n'
        '#define stoptime() _sysy_stoptime(__LINE__)\n'
        '#endif\n'
    )
    RUNTIME_WRAPPER.write_text(wrapper_content)
    return RUNTIME_WRAPPER


def flatten_multidim_io(src_text: str) -> str:
    multidim_int_arrays: set[str] = set()
    decl_re = re.compile(r"\bint\s+([A-Za-z_]\w*)\s*(?:\[[^\]]+\]){2,}\s*(?:=|;)")
    for match in decl_re.finditer(src_text):
        multidim_int_arrays.add(match.group(1))
    for name in sorted(multidim_int_arrays, key=len, reverse=True):
        ident = re.escape(name)
        src_text = re.sub(rf"\bgetarray\s*\(\s*{ident}\s*\)", f"getarray((int*){name})", src_text)
        src_text = re.sub(rf"\bputarray\s*\(\s*([^,\n]+?)\s*,\s*{ident}\s*\)", rf"putarray(\1, (int*){name})", src_text)
    return src_text


def compile_yoolang(src: Path, out_dir: Path) -> tuple[Path, str]:
    asm_file = out_dir / f"{src.stem}.yoolang.s"
    try:
        subprocess.run(
            [str(COMPILER_BIN), str(src), "-S", "-O1", "-o", str(asm_file)],
            check=True, capture_output=True, text=True, timeout=60,
        )
    except subprocess.CalledProcessError as e:
        return asm_file, f"compile failed: {e.stderr[:200]}"
    except subprocess.TimeoutExpired:
        return asm_file, "compile timeout"
    return asm_file, "OK"


def compile_gcc(src: Path, out_dir: Path) -> tuple[Path, str]:
    wrapper = ensure_runtime_wrapper()
    baseline_src = flatten_multidim_io(src.read_text())
    cpp_file = out_dir / f"{src.stem}.gcc.cpp"
    cpp_file.write_text(baseline_src)
    obj_file = out_dir / f"{src.stem}.gcc.o"
    try:
        subprocess.run(
            [GCC_BIN, "-O3", *RISCV_CODE_MODEL, "-std=gnu++17",
             "-include", str(wrapper), "-x", "c++", str(cpp_file),
             "-c", "-o", str(obj_file)],
            check=True, capture_output=True, text=True, timeout=60,
        )
    except subprocess.CalledProcessError as e:
        return obj_file, f"compile failed: {e.stderr[:200]}"
    except subprocess.TimeoutExpired:
        return obj_file, "compile timeout"
    return obj_file, "OK"


def compile_clang(src: Path, out_dir: Path) -> tuple[Path, str]:
    wrapper = ensure_runtime_wrapper()
    baseline_src = flatten_multidim_io(src.read_text())
    cpp_file = out_dir / f"{src.stem}.clang.cpp"
    cpp_file.write_text(baseline_src)
    obj_file = out_dir / f"{src.stem}.clang.o"
    try:
        subprocess.run(
            [CLANG_BIN, "-O3", *RISCV_CODE_MODEL, "-std=gnu++17",
             "--target=riscv64-linux-gnu", "--sysroot=/usr/riscv64-linux-gnu",
             "-include", str(wrapper), "-x", "c++", str(cpp_file),
             "-c", "-o", str(obj_file)],
            check=True, capture_output=True, text=True, timeout=60,
        )
    except subprocess.CalledProcessError as e:
        return obj_file, f"compile failed: {e.stderr[:200]}"
    except subprocess.TimeoutExpired:
        return obj_file, "compile timeout"
    return obj_file, "OK"


def disassemble(input_file: Path) -> tuple[str, str]:
    try:
        if input_file.suffix == ".s":
            obj_file = input_file.with_suffix(".o")
            subprocess.run(
                [GCC_BIN, "-c", *RISCV_CODE_MODEL, str(input_file), "-o", str(obj_file)],
                check=True, capture_output=True, text=True, timeout=30,
            )
        else:
            obj_file = input_file
        proc = subprocess.run(
            [OBJDUMP_BIN, "-d", str(obj_file)],
            check=True, capture_output=True, text=True, timeout=30,
        )
        return proc.stdout, "OK"
    except subprocess.CalledProcessError as e:
        return "", f"disassemble failed: {e.stderr[:200]}"
    except subprocess.TimeoutExpired:
        return "", "disassemble timeout"


# --- Report generation ---

CATEGORIES = [
    ("total", "TOTAL"), ("alu", "ALU"), ("load", "Load"),
    ("store", "Store"), ("branch", "Branch"), ("jump", "Jump"),
    ("div", "Div"), ("mul", "Mul"), ("fp", "FP"),
    ("call", "Call"), ("overhead", "Overhead"),
]


def generate_report(
    case_name: str,
    yoolang_stats: CompilerStats,
    gcc_stats: CompilerStats,
    clang_stats: CompilerStats,
) -> str:
    lines = [f"=== {case_name} ===\n"]
    lines.append(f"{'':>10} {'yoolang':>8}  {'gcc -O3':>8}  {'clang -O3':>8}")
    lines.append(f"{'':>10} {'':>8}  {'vs yoolang':>11}  {'vs yoolang':>11}")
    lines.append("-" * 55)
    yt, gt, ct = yoolang_stats.total, gcc_stats.total, clang_stats.total
    lines.append(f"  {'TOTAL':<8} {yt.total:>5}       {gt.total:>5}       {ct.total:>5}")
    lines.append("")
    for key, label in CATEGORIES:
        if key == "total":
            continue
        yv, gv, cv = getattr(yt, key), getattr(gt, key), getattr(ct, key)
        gr = f"{gv / yv:.2f}x" if yv > 0 else "N/A"
        cr = f"{cv / yv:.2f}x" if yv > 0 else "N/A"
        lines.append(f"  {label:<8} {yv:>5}       {gv:>5} ({gr:>6})  {cv:>5} ({cr:>6})")
    lines.append("")
    gcc_spd = f"{yt.total / gt.total:.2f}x" if gt.total > 0 else "N/A"
    clang_spd = f"{yt.total / ct.total:.2f}x" if ct.total > 0 else "N/A"
    lines.append(f"  GCC static instr ratio:  {gcc_spd} (>1 means GCC emits fewer)")
    lines.append(f"  Clang static instr ratio: {clang_spd} (>1 means Clang emits fewer)")
    lines.append("")
    lines.append("  Per-function breakdown (hot functions only):")
    lines.append(f"  {'Function':<30} {'yoolang':>8}  {'gcc':>8}  {'clang':>8}")
    all_funcs = set(yoolang_stats.functions.keys()) | set(gcc_stats.functions.keys()) | set(clang_stats.functions.keys())
    func_diffs = []
    for fname in all_funcs:
        yf = yoolang_stats.functions.get(fname, FuncStats(fname))
        gf = gcc_stats.functions.get(fname, FuncStats(fname))
        cf = clang_stats.functions.get(fname, FuncStats(fname))
        if max(yf.total, gf.total, cf.total) >= 5:
            func_diffs.append((fname, yf.total, gf.total, cf.total))
    func_diffs.sort(key=lambda x: -max(x[1], x[2], x[3]))
    for fname, yc, gc, cc in func_diffs[:20]:
        lines.append(f"  {fname:<30} {yc:>5}       {gc:>5}       {cc:>5}")
    return "\n".join(lines)


def stats_to_json(case_name: str, yoo: CompilerStats, gcc: CompilerStats, clang: CompilerStats) -> dict:
    """Generate a JSON-serializable report for CI consumption."""
    def ratio(a: int, b: int) -> float | None:
        return round(a / b, 4) if b > 0 else None

    yt, gt, ct = yoo.total, gcc.total, clang.total

    result: dict = {
        "case": case_name,
        "yoolang": yt.to_dict(),
        "gcc": gt.to_dict(),
        "clang": ct.to_dict(),
        "ratios": {},
        "gcc_total_ratio": ratio(yt.total, gt.total),
        "clang_total_ratio": ratio(yt.total, ct.total),
        "hot_functions": [],
    }

    for key, label in CATEGORIES:
        if key == "total":
            continue
        yv, gv, cv = getattr(yt, key), getattr(gt, key), getattr(ct, key)
        result["ratios"][f"gcc_{key}"] = ratio(gv, yv)
        result["ratios"][f"clang_{key}"] = ratio(cv, yv)

    all_funcs = set(yoo.functions.keys()) | set(gcc.functions.keys()) | set(clang.functions.keys())
    funcs = []
    for fname in all_funcs:
        yf = yoo.functions.get(fname, FuncStats(fname))
        gf = gcc.functions.get(fname, FuncStats(fname))
        cf = clang.functions.get(fname, FuncStats(fname))
        if max(yf.total, gf.total, cf.total) >= 3:
            funcs.append({
                "name": fname,
                "yoolang": yf.total, "gcc": gf.total, "clang": cf.total,
            })
    funcs.sort(key=lambda x: -max(x["yoolang"], x["gcc"], x["clang"]))
    result["hot_functions"] = funcs[:30]
    return result


def generate_summary(results: list[dict]) -> str:
    """One-line per benchmark summary for quick CI scanning."""
    lines = []
    lines.append(f"{'Case':<30} {'yoolang':>7} {'gcc':>7} {'clang':>7}  {'GCC':>6} {'Clang':>6}")
    lines.append("-" * 72)
    for r in results:
        yt = r["yoolang"]["total"]
        gt = r["gcc"]["total"]
        ct = r["clang"]["total"]
        gr = f"{yt / gt:.2f}x" if gt > 0 else "N/A"
        cr = f"{yt / ct:.2f}x" if ct > 0 else "N/A"
        name = r["case"][:28]
        lines.append(f"  {name:<28} {yt:>5}   {gt:>5}   {ct:>5}   {gr:>5}  {cr:>5}")
    return "\n".join(lines)


# --- Core runner (callable as library) ---

def run_case(case: Path, out_dir: Path) -> dict | None:
    """Run comparison for a single benchmark. Returns JSON dict or None on failure."""
    case_dir = out_dir / case.stem
    case_dir.mkdir(parents=True, exist_ok=True)

    yoo_asm, yoo_status = compile_yoolang(case, case_dir)
    if yoo_status != "OK":
        return {"case": case.stem, "error": f"yoolang: {yoo_status}"}

    gcc_asm, gcc_status = compile_gcc(case, case_dir)
    if gcc_status != "OK":
        return {"case": case.stem, "error": f"gcc: {gcc_status}"}

    clang_asm, clang_status = compile_clang(case, case_dir)
    if clang_status != "OK":
        return {"case": case.stem, "error": f"clang: {clang_status}"}

    yoo_dump, ys = disassemble(yoo_asm)
    gcc_dump, gs = disassemble(gcc_asm)
    clang_dump, cs = disassemble(clang_asm)
    if ys != "OK": return {"case": case.stem, "error": f"yoolang objdump: {ys}"}
    if gs != "OK": return {"case": case.stem, "error": f"gcc objdump: {gs}"}
    if cs != "OK": return {"case": case.stem, "error": f"clang objdump: {cs}"}

    yoo_stats = parse_objdump(yoo_dump)
    gcc_stats = parse_objdump(gcc_dump)
    clang_stats = parse_objdump(clang_dump)
    return stats_to_json(case.stem, yoo_stats, gcc_stats, clang_stats)


def collect_cases(suite: Optional[str], case_filter: Optional[str]) -> list[Path]:
    if suite:
        test_root = WORKSPACE / "test" / suite
    else:
        test_root = WORKSPACE / "test" / "performance"
    cases = sorted(test_root.rglob("*.sy"))
    if case_filter:
        cases = [c for c in cases if case_filter in c.stem]
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare yoolang vs GCC vs Clang at instruction level")
    parser.add_argument("--suite", default="performance", help="Test suite directory")
    parser.add_argument("--case", help="Filter by case name")
    parser.add_argument("--output-dir", default=None, help="Directory for intermediate files")
    parser.add_argument("--json", action="store_true", help="Output JSON (for CI)")
    parser.add_argument("--summary", action="store_true", help="Output compact summary table (for CI)")
    parser.add_argument("--json-out", default=None, help="Write JSON to file")
    parser.add_argument("--summary-out", default=None, help="Write summary to file")
    parser.add_argument("--quiet", action="store_true", help="Suppress per-case output in JSON mode")
    args = parser.parse_args()

    cases = collect_cases(args.suite, args.case)
    if not cases:
        print("No cases found.", file=sys.stderr)
        return 1

    out_dir = Path(args.output_dir) if args.output_dir else (WORKSPACE / "build" / "gcc-clang-diff")
    out_dir.mkdir(parents=True, exist_ok=True)

    if not COMPILER_BIN.exists():
        print(f"Compiler binary not found: {COMPILER_BIN}", file=sys.stderr)
        return 1

    results = []
    for case in cases:
        result = run_case(case, out_dir)
        if result is None:
            continue
        results.append(result)
        if not args.quiet:
            if args.json:
                print(json.dumps(result, ensure_ascii=False))
            elif args.summary:
                pass  # summary printed at end
            else:
                case_dir = out_dir / case.stem
                yoo_dump, _ = disassemble(case_dir / f"{case.stem}.yoolang.s")
                gcc_dump, _ = disassemble(case_dir / f"{case.stem}.gcc.o")
                clang_dump, _ = disassemble(case_dir / f"{case.stem}.clang.o")
                if yoo_dump and gcc_dump and clang_dump:
                    yoo = parse_objdump(yoo_dump)
                    gcc = parse_objdump(gcc_dump)
                    clang = parse_objdump(clang_dump)
                    print(generate_report(case.stem, yoo, gcc, clang))
                    print()

    if args.summary:
        summary_text = generate_summary(results)
        print(summary_text)

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(results, f, ensure_ascii=False, indent=2)

    if args.summary_out:
        with open(args.summary_out, "w") as f:
            f.write(generate_summary(results) + "\n")

    # Print final aggregate stats
    successes = [r for r in results if "error" not in r]
    errors = [r for r in results if "error" in r]
    if not args.quiet:
        total_yoo = sum(r["yoolang"]["total"] for r in successes)
        total_gcc = sum(r["gcc"]["total"] for r in successes)
        total_clang = sum(r["clang"]["total"] for r in successes)
        gcc_ratio = total_yoo / total_gcc if total_gcc > 0 else 0
        clang_ratio = total_yoo / total_clang if total_clang > 0 else 0
        print(f"\nAggregate: {len(successes)} cases, {len(errors)} errors")
        print(f"  yoolang total: {total_yoo}, gcc: {total_gcc}, clang: {total_clang}")
        print(f"  gcc ratio: {gcc_ratio:.2f}x, clang ratio: {clang_ratio:.2f}x")

    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
