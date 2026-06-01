#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build/linux/x86_64/release/compiler"
DEFAULT_TEST_DIR = ROOT / "test/poly"
DEFAULT_WORK_DIR = ROOT / "tmp/poly-test-run"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run polyhedral FileCheck tests.")
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--test-dir", type=Path, default=DEFAULT_TEST_DIR)
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR)
    parser.add_argument("--filter", help="only run tests whose path contains this substring")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--keep-tmp", action="store_true")
    return parser.parse_args()


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def short_output(data: bytes, limit: int = 4096) -> str:
    text = data.decode("utf-8", errors="replace")
    if len(text) <= limit:
        return text
    return text[:limit] + "\n... <truncated> ...\n"


def discover_tests(test_dir: Path, pattern: str | None) -> list[Path]:
    tests: list[Path] = []
    for source in sorted(test_dir.rglob("*.sy")):
        if pattern and pattern not in rel(source):
            continue
        if any(line.lstrip().startswith("// RUN:") for line in source.read_text().splitlines()):
            tests.append(source)
    return tests


def run_one(source: Path, binary: Path, work_dir: Path, timeout: float) -> tuple[bool, float, str]:
    start = time.monotonic()
    commands = [
        line.split("RUN:", 1)[1].strip()
        for line in source.read_text().splitlines()
        if line.lstrip().startswith("// RUN:")
    ]
    if not commands:
        return False, time.monotonic() - start, "no RUN lines"

    case_dir = work_dir / rel(source).replace("/", "_")
    case_dir.mkdir(parents=True, exist_ok=True)
    tmp_base = case_dir / "out"
    substitutions = {
        "%compiler": shlex.quote(str(binary)),
        "%s": shlex.quote(str(source)),
        "%S": shlex.quote(str(source.parent)),
        "%T": shlex.quote(str(case_dir)),
        "%t": shlex.quote(str(tmp_base)),
    }

    for index, command in enumerate(commands, start=1):
        expanded = command
        for key, value in substitutions.items():
            expanded = expanded.replace(key, value)
        try:
            proc = subprocess.run(
                expanded,
                cwd=ROOT,
                shell=True,
                executable="/bin/bash",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return False, time.monotonic() - start, f"RUN line {index} timed out: {expanded}"
        if proc.returncode != 0:
            detail = f"RUN line {index} failed: {expanded}\n"
            if proc.stdout:
                detail += "\nstdout:\n" + short_output(proc.stdout)
            if proc.stderr:
                detail += "\nstderr:\n" + short_output(proc.stderr)
            return False, time.monotonic() - start, detail

    return True, time.monotonic() - start, ""


def main() -> int:
    args = parse_args()
    args.binary = (ROOT / args.binary).resolve() if not args.binary.is_absolute() else args.binary
    args.test_dir = (ROOT / args.test_dir).resolve() if not args.test_dir.is_absolute() else args.test_dir
    args.work_dir = (ROOT / args.work_dir).resolve() if not args.work_dir.is_absolute() else args.work_dir

    if not args.binary.exists():
        print(f"error: binary not found: {args.binary}; run xmake or pass --binary", file=sys.stderr)
        return 2
    if not args.test_dir.exists():
        print(f"error: poly test directory not found: {args.test_dir}", file=sys.stderr)
        return 2
    if shutil.which("FileCheck") is None:
        print("error: FileCheck not found in PATH", file=sys.stderr)
        return 2
    if args.work_dir.exists() and not args.keep_tmp:
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True, exist_ok=True)

    tests = discover_tests(args.test_dir, args.filter)
    if not tests:
        print(f"error: no poly .sy tests found in {args.test_dir}", file=sys.stderr)
        return 2

    failed = 0
    print(f"[poly] {len(tests)} test item(s)")
    for source in tests:
        ok, elapsed, detail = run_one(source, args.binary, args.work_dir, args.timeout)
        status = "PASS" if ok else "FAIL"
        print(f"{status:4} poly      {rel(source)} ({elapsed:.2f}s)")
        if not ok:
            failed += 1
            if detail:
                for line in detail.rstrip().splitlines():
                    print(f"    {line}")

    if failed:
        print(f"\nsummary: {len(tests) - failed} passed, {failed} failed")
        return 1
    print(f"\nsummary: {len(tests)} passed, 0 failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
