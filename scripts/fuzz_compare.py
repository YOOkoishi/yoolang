#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(os.environ.get("GITHUB_WORKSPACE", Path(__file__).resolve().parents[1])).resolve()
DEFAULT_COMPILER = ROOT / "build/linux/x86_64/release/compiler"
DEFAULT_RUNTIME = ROOT / "runtime/libsysy_riscv.a"
DEFAULT_WORK_DIR = ROOT / "build/fuzz-ci"


@dataclass
class ProcResult:
    ok: bool
    returncode: int | None
    stdout: bytes
    stderr: bytes
    elapsed_sec: float
    detail: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Differential fuzz tests for yoolang vs Clang.")
    parser.add_argument("--cases", type=int, default=int(os.environ.get("FUZZ_CASES", "40")))
    parser.add_argument("--seed", default=os.environ.get("FUZZ_SEED", "yoolang-fuzz"))
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR)
    parser.add_argument("--compiler", type=Path, default=Path(os.environ.get("COMPILER_BIN", DEFAULT_COMPILER)))
    parser.add_argument("--runtime", type=Path, default=Path(os.environ.get("SYSY_RUNTIME_LIB", DEFAULT_RUNTIME)))
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN", "qemu-riscv64"))
    parser.add_argument("--gcc", default=os.environ.get("RISCV_GCC", "riscv64-linux-gnu-g++"))
    parser.add_argument("--clang", default=os.environ.get("RISCV_CLANGXX", "clang++"))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("FUZZ_TIMEOUT_SEC", "5")))
    parser.add_argument("--loops", action="store_true", help="include while-loop cases")
    parser.add_argument("--generate-only", action="store_true", help="only generate sources and reports")
    parser.add_argument("--keep-going", action="store_true", help="continue after the first failing case")
    return parser.parse_args()


def stable_seed(text: str) -> int:
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "little")


def run_process(argv: list[str], *, input_bytes: bytes | None = None, timeout: float = 20.0) -> ProcResult:
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            argv,
            input=input_bytes,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        elapsed = time.perf_counter() - start
        return ProcResult(proc.returncode == 0, proc.returncode, proc.stdout, proc.stderr, elapsed)
    except subprocess.TimeoutExpired as exc:
        elapsed = time.perf_counter() - start
        stdout = exc.stdout if isinstance(exc.stdout, bytes) else b""
        stderr = exc.stderr if isinstance(exc.stderr, bytes) else b""
        return ProcResult(False, None, stdout, stderr, elapsed, f"timeout after {timeout:.1f}s")
    except Exception as exc:
        elapsed = time.perf_counter() - start
        return ProcResult(False, None, b"", str(exc).encode("utf-8", errors="replace"), elapsed, f"error: {exc}")


class ProgramGenerator:
    def __init__(self, rng: random.Random, case_index: int, *, allow_loops: bool = False) -> None:
        self.rng = rng
        self.case_index = case_index
        self.allow_loops = allow_loops
        self.tmp_index = 0

    def const(self) -> str:
        value = self.rng.randint(-31, 31)
        return str(value) if value >= 0 else f"(0 - {-value})"

    def value(self) -> str:
        return self.rng.choice(["a", "b", "c", "d", self.const()])

    def expr(self, depth: int = 0) -> str:
        if depth >= 2 or self.rng.random() < 0.35:
            return self.value()
        op = self.rng.choice(["+", "-", "*", "%"])
        left = self.expr(depth + 1)
        right = self.expr(depth + 1)
        if op == "%":
            right = str(self.rng.randint(2, 97))
        if op == "*":
            left = f"(({left}) % 97)"
            right = f"(({right}) % 97)"
        return f"(({left}) {op} ({right}))"

    def bounded_expr(self) -> str:
        return f"(({self.expr()}) % 10007)"

    def cond(self) -> str:
        left = self.expr()
        right = self.expr()
        op = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
        base = f"(({left}) {op} ({right}))"
        if self.rng.random() < 0.35:
            other = f"(({self.expr()}) {self.rng.choice(['<', '!=', '>='])} ({self.expr()}))"
            join = self.rng.choice(["&&", "||"])
            return f"({base} {join} {other})"
        return base

    def assignment(self, indent: str = "  ") -> str:
        target = self.rng.choice(["a", "b", "c", "d"])
        return f"{indent}{target} = {self.bounded_expr()};"

    def function(self, name: str) -> str:
        loop_count = self.rng.randint(2, 8)
        lines = [
            f"int {name}(int p0, int p1, int p2) {{",
            "  int a = p0;",
            "  int b = p1;",
            "  int c = p2;",
            f"  int d = {self.const()};",
        ]
        if self.allow_loops:
            lines.append("  int i = 0;")
        for _ in range(self.rng.randint(3, 6)):
            lines.append(self.assignment())
        if self.allow_loops:
            lines.extend(
                [
                    f"  while (i < {loop_count}) {{",
                    self.assignment("    "),
                    "    if " + self.cond() + " {",
                    self.assignment("      "),
                    "    } else {",
                    self.assignment("      "),
                    "    }",
                    "    i = i + 1;",
                    "  }",
                ]
            )
        for _ in range(self.rng.randint(1, 3)):
            lines.extend(["  if " + self.cond() + " {", self.assignment("    "), "  }"])
        lines.extend(
            [
                "  d = (a + b + c + d) % 10007;",
                "  return d;",
                "}",
            ]
        )
        return "\n".join(lines)

    def program(self) -> str:
        fn_count = self.rng.randint(2, 4)
        function_names = [f"f{idx}" for idx in range(fn_count)]
        parts = [self.function(name) for name in function_names]
        lines = [
            "int main() {",
            f"  int r = {self.rng.randint(0, 997)};",
        ]
        if self.allow_loops:
            lines.append("  int i = 0;")
        for idx, name in enumerate(function_names):
            a = self.rng.randint(-23, 23)
            b = self.rng.randint(-23, 23)
            c = self.rng.randint(-23, 23)
            lines.append(f"  r = (r + {name}({a}, {b}, {c})) % 1000003;")
            if idx % 2 == 0:
                lines.append("  if (r < 0) { r = 0 - r; }")
        if self.allow_loops:
            lines.extend(
                [
                    f"  while (i < {self.rng.randint(1, 5)}) {{",
                    f"    r = (r + {self.rng.choice(function_names)}(r % 97, i, {self.rng.randint(1, 19)})) % 1000003;",
                    "    if (r < 0) { r = 0 - r; }",
                    "    i = i + 1;",
                    "  }",
                ]
            )
        else:
            for _ in range(self.rng.randint(1, 3)):
                lines.append(
                    f"  r = (r + {self.rng.choice(function_names)}(r % 97, {self.rng.randint(-17, 17)}, {self.rng.randint(1, 19)})) % 1000003;"
                )
                lines.append("  if (r < 0) { r = 0 - r; }")
        lines.extend(["  putint(r);", "  putch(10);", "  return r % 127;", "}"])
        parts.append("\n".join(lines))
        return "\n\n".join(parts) + "\n"


def write_runtime_wrapper(path: Path) -> None:
    path.write_text(
        "#ifndef YOOLANG_FUZZ_SYLIB_WRAPPER_HPP\n"
        "#define YOOLANG_FUZZ_SYLIB_WRAPPER_HPP\n"
        "extern \"C\" {\n"
        "int getint();\n"
        "int getch();\n"
        "int getarray(int a[]);\n"
        "void putint(int a);\n"
        "void putch(int a);\n"
        "void putarray(int n, int a[]);\n"
        "}\n"
        "#endif\n"
    )


def compile_yoolang(args: argparse.Namespace, src: Path, case_dir: Path) -> tuple[bool, str, Path | None]:
    asm = case_dir / "yoolang.s"
    obj = case_dir / "yoolang.o"
    exe = case_dir / "yoolang.riscv"
    for cmd in (
        [str(args.compiler), str(src), "-S", "-O1", "-o", str(asm)],
        [args.gcc, "-c", str(asm), "-o", str(obj)],
        [args.gcc, "-static", str(obj), str(args.runtime), "-o", str(exe)],
    ):
        result = run_process(cmd, timeout=args.timeout)
        if not result.ok:
            return False, command_detail(cmd, result), None
    return True, "OK", exe


def compile_clang(args: argparse.Namespace, src: Path, case_dir: Path, wrapper: Path) -> tuple[bool, str, Path | None]:
    obj = case_dir / "clang.o"
    exe = case_dir / "clang.riscv"
    compile_cmd = [
        args.clang,
        "-O2",
        "-std=gnu++17",
        "--target=riscv64-linux-gnu",
        "--sysroot=/usr/riscv64-linux-gnu",
        "-include",
        str(wrapper),
        "-x",
        "c++",
        str(src),
        "-c",
        "-o",
        str(obj),
    ]
    for cmd in (compile_cmd, [args.gcc, "-static", str(obj), str(args.runtime), "-o", str(exe)]):
        result = run_process(cmd, timeout=args.timeout)
        if not result.ok:
            return False, command_detail(cmd, result), None
    return True, "OK", exe


def command_detail(cmd: list[str], result: ProcResult) -> str:
    stderr = result.stderr.decode("utf-8", errors="replace").strip()
    stdout = result.stdout.decode("utf-8", errors="replace").strip()
    detail = result.detail or f"exit={result.returncode}"
    text = stderr or stdout
    if text:
        detail += "\n" + text[:4000]
    return "cmd: " + " ".join(cmd) + "\n" + detail


def run_exe(args: argparse.Namespace, exe: Path) -> ProcResult:
    return run_process([args.qemu, "-L", "/usr/riscv64-linux-gnu", str(exe)], timeout=args.timeout)


def check_tools(args: argparse.Namespace) -> list[str]:
    missing: list[str] = []
    if not args.compiler.exists():
        missing.append(f"compiler not found: {args.compiler}")
    if not args.runtime.exists():
        missing.append(f"runtime not found: {args.runtime}")
    for label, tool in (("qemu", args.qemu), ("gcc", args.gcc), ("clang", args.clang)):
        if shutil.which(tool) is None and not Path(tool).exists():
            missing.append(f"{label} not found: {tool}")
    return missing


def write_reports(work_dir: Path, payload: dict[str, Any]) -> None:
    report_json = work_dir / "fuzz-report.json"
    report_md = work_dir / "fuzz-report.md"
    report_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    lines = [
        "# Yoolang Fuzz Differential Report",
        "",
        f"- Status: {payload['status']}",
        f"- Seed: `{payload['seed']}`",
        f"- Cases: {payload['cases']}",
        f"- Loops: {payload.get('loops', False)}",
        f"- Failed: {payload['failures']}",
        "",
        "| Case | Status | Detail |",
        "| --- | --- | --- |",
    ]
    for row in payload["rows"]:
        detail_lines = str(row.get("detail", "")).splitlines()
        detail = detail_lines[0][:160] if detail_lines else ""
        lines.append(f"| {row['case']} | {row['status']} | {detail} |")
    report_md.write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    if args.cases <= 0:
        print("error: --cases must be positive", file=sys.stderr)
        return 2

    work_dir = args.work_dir.resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    cases_dir = work_dir / "cases"
    cases_dir.mkdir(parents=True, exist_ok=True)
    wrapper = work_dir / "sylib_wrapper.hpp"
    write_runtime_wrapper(wrapper)

    seed_value = stable_seed(str(args.seed))
    master_rng = random.Random(seed_value)
    rows: list[dict[str, Any]] = []
    failures = 0

    missing = [] if args.generate_only else check_tools(args)
    if missing:
        payload = {
            "status": "FAIL",
            "seed": str(args.seed),
            "seed_value": seed_value,
            "cases": args.cases,
            "loops": args.loops,
            "failures": 1,
            "reason": "; ".join(missing),
            "rows": [{"case": "setup", "status": "FAIL", "detail": "; ".join(missing)}],
        }
        write_reports(work_dir, payload)
        print(payload["reason"], file=sys.stderr)
        return 1

    for index in range(args.cases):
        case_seed = master_rng.getrandbits(64)
        case_dir = cases_dir / f"case_{index:04d}"
        case_dir.mkdir(parents=True, exist_ok=True)
        src = case_dir / f"case_{index:04d}.sy"
        program = ProgramGenerator(random.Random(case_seed), index, allow_loops=args.loops).program()
        src.write_text(program)

        row: dict[str, Any] = {
            "case": index,
            "seed": case_seed,
            "source": str(src.relative_to(work_dir)),
            "status": "GENERATED" if args.generate_only else "OK",
            "detail": "",
        }
        if args.generate_only:
            rows.append(row)
            continue

        ok_yoo, detail_yoo, exe_yoo = compile_yoolang(args, src, case_dir)
        if not ok_yoo or exe_yoo is None:
            row.update({"status": "YOOLANG_COMPILE_FAIL", "detail": detail_yoo})
            failures += 1
            rows.append(row)
            if not args.keep_going:
                break
            continue

        ok_clang, detail_clang, exe_clang = compile_clang(args, src, case_dir, wrapper)
        if not ok_clang or exe_clang is None:
            row.update({"status": "CLANG_COMPILE_FAIL", "detail": detail_clang})
            failures += 1
            rows.append(row)
            if not args.keep_going:
                break
            continue

        yoolang_run = run_exe(args, exe_yoo)
        clang_run = run_exe(args, exe_clang)
        (case_dir / "yoolang.stdout").write_bytes(yoolang_run.stdout)
        (case_dir / "yoolang.stderr").write_bytes(yoolang_run.stderr)
        (case_dir / "clang.stdout").write_bytes(clang_run.stdout)
        (case_dir / "clang.stderr").write_bytes(clang_run.stderr)

        if yoolang_run.returncode != clang_run.returncode or yoolang_run.stdout != clang_run.stdout:
            detail = (
                f"output mismatch: yoolang exit={yoolang_run.returncode}, clang exit={clang_run.returncode}; "
                f"yoolang stdout={yoolang_run.stdout!r}; clang stdout={clang_run.stdout!r}"
            )
            row.update({"status": "MISMATCH", "detail": detail})
            failures += 1
            rows.append(row)
            print(f"FAIL case {index}: {detail}")
            if not args.keep_going:
                break
            continue

        rows.append(row)
        print(f"PASS case {index:04d} seed={case_seed}")

    status = "PASS" if failures == 0 else "FAIL"
    payload = {
        "status": status,
        "seed": str(args.seed),
        "seed_value": seed_value,
        "cases": args.cases,
        "loops": args.loops,
        "executed_cases": len(rows),
        "failures": failures,
        "work_dir": str(work_dir),
        "rows": rows,
    }
    write_reports(work_dir, payload)
    print(f"fuzz summary: status={status} cases={args.cases} executed={len(rows)} failures={failures}")
    print(f"fuzz report: {work_dir / 'fuzz-report.json'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
