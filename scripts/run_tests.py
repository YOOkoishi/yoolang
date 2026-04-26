#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import difflib
import fnmatch
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build/linux/x86_64/release/yoolang"
DEFAULT_WORK_DIR = ROOT / "tmp/test-run"
DEFAULT_RUNTIME = ROOT / "runtime/libsysy_riscv.a"
STAGE_FLAGS = {
    "yir": "--emit-yir",
    "oir": "--emit-oir",
    "mir": "--emit-mir",
    "asm": "--emit-asm",
}


@dataclasses.dataclass
class TestResult:
    suite: str
    name: str
    status: str
    elapsed: float = 0.0
    detail: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run yoolang IR and end-to-end tests.")
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--test-root", type=Path, default=ROOT / "test")
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR)
    parser.add_argument("--runtime", type=Path, default=DEFAULT_RUNTIME)
    parser.add_argument("--xfail-file", type=Path, default=ROOT / "test/xfail.txt")
    parser.add_argument("--suite", action="append", choices=["all", "filecheck", "stage", "e2e"])
    parser.add_argument("--stage", action="append", choices=sorted(STAGE_FLAGS))
    parser.add_argument("--filter", help="only run tests whose path contains this substring")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=float, default=20.0, help="per subprocess timeout in seconds")
    parser.add_argument(
        "--max-input-bytes",
        type=int,
        default=10 * 1024 * 1024,
        help="skip e2e tests with input files larger than this; use 0 for no limit",
    )
    parser.add_argument("--build", action="store_true", help="run xmake before tests")
    parser.add_argument("--keep-tmp", action="store_true", help="keep previous files under --work-dir")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def expand_suites(values: list[str] | None) -> list[str]:
    if not values or "all" in values:
        return ["filecheck", "stage", "e2e"]
    out: list[str] = []
    for value in values:
        if value not in out:
            out.append(value)
    return out


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def safe_name(path: Path) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", rel(path))


def run_process(
    argv: list[str],
    *,
    input_bytes: bytes | None = None,
    stdout_target=subprocess.PIPE,
    timeout: float,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        argv,
        cwd=ROOT,
        input=input_bytes,
        stdout=stdout_target,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )


def short_output(data: bytes, limit: int = 4096) -> str:
    text = data.decode("utf-8", errors="replace")
    if len(text) <= limit:
        return text
    return text[:limit] + "\n... <truncated> ...\n"


def discover_sources(test_root: Path, pattern: str | None) -> list[Path]:
    sources = sorted(test_root.rglob("*.sy"))
    out = []
    for source in sources:
        if "/test/ir/" in f"/{rel(source.parent)}/":
            continue
        if pattern and pattern not in rel(source):
            continue
        out.append(source)
    return out


def discover_e2e_cases(test_root: Path, pattern: str | None) -> list[Path]:
    return [source for source in discover_sources(test_root, pattern) if source.with_suffix(".out").exists()]


def discover_filecheck_tests(test_root: Path, pattern: str | None) -> list[Path]:
    roots = [test_root / "ir"]
    tests: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for source in sorted(root.rglob("*.sy")):
            if pattern and pattern not in rel(source):
                continue
            if any(line.lstrip().startswith("// RUN:") for line in source.read_text().splitlines()):
                tests.append(source)
    return tests


def load_xfails(path: Path) -> list[tuple[str, str]]:
    if not path.exists():
        return []
    out: list[tuple[str, str]] = []
    for raw_line in path.read_text().splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(maxsplit=1)
        if len(parts) == 2 and parts[0] in {"*", "filecheck", "stage", "e2e"}:
            out.append((parts[0], parts[1]))
        else:
            out.append(("*", line))
    return out


def is_xfail(result: TestResult, xfails: list[tuple[str, str]]) -> bool:
    for suite, pattern in xfails:
        if suite not in {"*", result.suite}:
            continue
        if fnmatch.fnmatch(result.name, pattern):
            return True
    return False


def apply_xfail(result: TestResult, xfails: list[tuple[str, str]]) -> TestResult:
    if result.status == "SKIP" or not is_xfail(result, xfails):
        return result
    if result.status == "FAIL":
        result.status = "XFAIL"
        return result
    if result.status == "PASS":
        result.status = "XPASS"
        result.detail = result.detail or "test unexpectedly passed; remove it from test/xfail.txt"
    return result


def run_filecheck(source: Path, binary: Path, work_dir: Path, timeout: float) -> TestResult:
    start = time.monotonic()
    commands = [
        line.split("RUN:", 1)[1].strip()
        for line in source.read_text().splitlines()
        if line.lstrip().startswith("// RUN:")
    ]
    if not commands:
        return TestResult("filecheck", rel(source), "SKIP", detail="no RUN lines")

    tmp_dir = work_dir / "filecheck" / safe_name(source)
    tmp_dir.mkdir(parents=True, exist_ok=True)
    tmp_base = tmp_dir / "out"
    substitutions = {
        "%yoolang": shlex.quote(str(binary)),
        "%s": shlex.quote(str(source)),
        "%S": shlex.quote(str(source.parent)),
        "%T": shlex.quote(str(tmp_dir)),
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
            return TestResult(
                "filecheck",
                rel(source),
                "FAIL",
                time.monotonic() - start,
                f"RUN line {index} timed out: {expanded}",
            )
        if proc.returncode != 0:
            detail = f"RUN line {index} failed: {expanded}\n"
            if proc.stdout:
                detail += "\nstdout:\n" + short_output(proc.stdout)
            if proc.stderr:
                detail += "\nstderr:\n" + short_output(proc.stderr)
            return TestResult("filecheck", rel(source), "FAIL", time.monotonic() - start, detail)

    return TestResult("filecheck", rel(source), "PASS", time.monotonic() - start)


def run_stage(source: Path, stage: str, binary: Path, timeout: float) -> TestResult:
    name = f"{rel(source)} [{stage}]"
    start = time.monotonic()
    try:
        proc = run_process(
            [str(binary), STAGE_FLAGS[stage], str(source)],
            stdout_target=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return TestResult("stage", name, "FAIL", time.monotonic() - start, "compiler timed out")
    if proc.returncode != 0:
        return TestResult("stage", name, "FAIL", time.monotonic() - start, short_output(proc.stderr))
    return TestResult("stage", name, "PASS", time.monotonic() - start)


def normalize_expected(data: bytes) -> str:
    text = data.decode("utf-8", errors="replace").replace("\r\n", "\n")
    return text.rstrip() + "\n"


def normalize_actual(stdout: bytes, return_code: int) -> str:
    text = stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
    if text and not text.endswith("\n"):
        text += "\n"
    text += f"{return_code}\n"
    return text.rstrip() + "\n"


def diff_text(expected: str, actual: str) -> str:
    diff = difflib.unified_diff(
        expected.splitlines(keepends=True),
        actual.splitlines(keepends=True),
        fromfile="expected",
        tofile="actual",
    )
    text = "".join(diff)
    if len(text) > 4096:
        return text[:4096] + "\n... <diff truncated> ...\n"
    return text


def run_e2e(
    source: Path,
    binary: Path,
    work_dir: Path,
    runtime: Path,
    gcc: str | None,
    qemu: str | None,
    timeout: float,
    max_input_bytes: int,
) -> TestResult:
    start = time.monotonic()
    name = rel(source)
    if gcc is None:
        return TestResult("e2e", name, "SKIP", detail="riscv64-linux-gnu-gcc not found")
    if qemu is None:
        return TestResult("e2e", name, "SKIP", detail="qemu-riscv64 not found")
    if not runtime.exists():
        return TestResult("e2e", name, "SKIP", detail=f"runtime archive not found: {runtime}")

    input_path = source.with_suffix(".in")
    expected_path = source.with_suffix(".out")
    if input_path.exists() and max_input_bytes and input_path.stat().st_size > max_input_bytes:
        size_mib = input_path.stat().st_size / (1024 * 1024)
        return TestResult("e2e", name, "SKIP", detail=f"input is {size_mib:.1f} MiB")

    case_dir = work_dir / "e2e" / safe_name(source)
    case_dir.mkdir(parents=True, exist_ok=True)
    asm_path = case_dir / "program.s"
    exe_path = case_dir / "program"

    try:
        with asm_path.open("wb") as asm_file:
            compile_proc = run_process(
                [str(binary), "--emit-asm", str(source)],
                stdout_target=asm_file,
                timeout=timeout,
            )
    except subprocess.TimeoutExpired:
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, "compiler timed out")
    if compile_proc.returncode != 0:
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, short_output(compile_proc.stderr))

    try:
        link_proc = run_process(
            [gcc, "-static", str(asm_path), str(runtime), "-o", str(exe_path)],
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, "linker timed out")
    if link_proc.returncode != 0:
        detail = "link failed\n" + short_output(link_proc.stderr)
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, detail)

    input_bytes = input_path.read_bytes() if input_path.exists() else b""
    try:
        run_proc = run_process([qemu, str(exe_path)], input_bytes=input_bytes, timeout=timeout)
    except subprocess.TimeoutExpired:
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, "program timed out")

    expected = normalize_expected(expected_path.read_bytes())
    actual = normalize_actual(run_proc.stdout, run_proc.returncode)
    if expected != actual:
        actual_path = case_dir / "actual.out"
        actual_path.write_text(actual)
        detail = f"output mismatch; actual saved to {rel(actual_path)}\n" + diff_text(expected, actual)
        if run_proc.stderr:
            detail += "\nstderr:\n" + short_output(run_proc.stderr)
        return TestResult("e2e", name, "FAIL", time.monotonic() - start, detail)

    return TestResult("e2e", name, "PASS", time.monotonic() - start)


def run_many(label: str, jobs: int, items: Iterable, worker, xfails: list[tuple[str, str]]) -> list[TestResult]:
    item_list = list(items)
    if not item_list:
        return []
    print(f"[{label}] {len(item_list)} test item(s), jobs={jobs}")
    results: list[TestResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = [pool.submit(worker, item) for item in item_list]
        for future in concurrent.futures.as_completed(futures):
            result = apply_xfail(future.result(), xfails)
            results.append(result)
            print(f"{result.status:4} {result.suite:9} {result.name} ({result.elapsed:.2f}s)")
            if result.status == "FAIL" and result.detail:
                print(indent(result.detail.rstrip(), "    "))
    return results


def indent(text: str, prefix: str) -> str:
    return "\n".join(prefix + line for line in text.splitlines())


def ensure_ready(args: argparse.Namespace) -> None:
    if args.build:
        proc = subprocess.run(["xmake"], cwd=ROOT, check=False)
        if proc.returncode != 0:
            raise SystemExit(proc.returncode)
    if not args.binary.exists():
        raise SystemExit(f"error: binary not found: {args.binary}; run xmake or pass --binary")
    if not args.keep_tmp and args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True, exist_ok=True)


def summarize(results: list[TestResult]) -> int:
    passed = sum(1 for result in results if result.status == "PASS")
    failed = sum(1 for result in results if result.status == "FAIL")
    skipped = sum(1 for result in results if result.status == "SKIP")
    xfailed = sum(1 for result in results if result.status == "XFAIL")
    xpassed = sum(1 for result in results if result.status == "XPASS")
    print(
        f"\nsummary: {passed} passed, {failed} failed, {skipped} skipped, "
        f"{xfailed} xfailed, {xpassed} xpassed"
    )
    return 1 if failed or xpassed else 0


def main() -> int:
    args = parse_args()
    args.binary = (ROOT / args.binary).resolve() if not args.binary.is_absolute() else args.binary
    args.test_root = (ROOT / args.test_root).resolve() if not args.test_root.is_absolute() else args.test_root
    args.work_dir = (ROOT / args.work_dir).resolve() if not args.work_dir.is_absolute() else args.work_dir
    args.runtime = (ROOT / args.runtime).resolve() if not args.runtime.is_absolute() else args.runtime
    ensure_ready(args)

    suites = expand_suites(args.suite)
    stages = args.stage or sorted(STAGE_FLAGS)
    xfails = load_xfails(args.xfail_file)
    results: list[TestResult] = []

    if "filecheck" in suites:
        tests = discover_filecheck_tests(args.test_root, args.filter)
        results.extend(
            run_many(
                "filecheck",
                args.jobs,
                tests,
                lambda path: run_filecheck(path, args.binary, args.work_dir, args.timeout),
                xfails,
            )
        )

    if "stage" in suites:
        sources = discover_sources(args.test_root, args.filter)
        stage_items = [(source, stage) for source in sources for stage in stages]
        results.extend(
            run_many(
                "stage",
                args.jobs,
                stage_items,
                lambda item: run_stage(item[0], item[1], args.binary, args.timeout),
                xfails,
            )
        )

    if "e2e" in suites:
        gcc = shutil.which("riscv64-linux-gnu-gcc") or shutil.which("riscv64-unknown-linux-gnu-gcc")
        qemu = shutil.which("qemu-riscv64")
        cases = discover_e2e_cases(args.test_root, args.filter)
        results.extend(
            run_many(
                "e2e",
                args.jobs,
                cases,
                lambda path: run_e2e(
                    path,
                    args.binary,
                    args.work_dir,
                    args.runtime,
                    gcc,
                    qemu,
                    args.timeout,
                    args.max_input_bytes,
                ),
                xfails,
            )
        )

    return summarize(results)


if __name__ == "__main__":
    raise SystemExit(main())
