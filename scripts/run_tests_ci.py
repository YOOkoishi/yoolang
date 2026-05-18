#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FUNCTIONAL_ROOT = ROOT / "test" / "functional"
DEFAULT_PERF_ROOT = ROOT / "test" / "performance"


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def resolve_binary() -> Path:
    env_bin = os.environ.get("COMPILER_BIN", "").strip()
    if env_bin:
        candidate = Path(env_bin)
        candidate = candidate if candidate.is_absolute() else (ROOT / candidate)
        if candidate.exists():
            return candidate
        raise SystemExit(f"error: COMPILER_BIN does not exist: {candidate}")

    preferred = ROOT / "build/linux/x86_64/release/compiler"
    if preferred.exists():
        return preferred

    candidates = sorted(
        candidate
        for candidate in (ROOT / "build").rglob("compiler")
        if candidate.is_file() and "release" in candidate.parts
    )
    if candidates:
        return candidates[0]

    raise SystemExit("error: compiler binary not found; run xmake first or set COMPILER_BIN")


def resolve_runtime() -> Path:
    candidates: list[Path] = []

    env_runtime = os.environ.get("SYSY_RUNTIME_LIB", "").strip()
    if env_runtime:
        candidate = Path(env_runtime)
        candidates.append(candidate if candidate.is_absolute() else (ROOT / candidate))

    candidates.extend(
        [
            ROOT / "runtime/libsysy.a",
            ROOT / "runtime/libsysy_riscv.a",
        ]
    )

    for candidate in candidates:
        if candidate.exists():
            return candidate

    raise SystemExit("error: SysY runtime library not found; expected runtime/libsysy.a or runtime/libsysy_riscv.a")


def require_command(label: str, env_key: str | None, candidates: list[str]) -> str:
    if env_key:
        value = os.environ.get(env_key, "").strip()
        if value:
            if shutil.which(value):
                return value
            raise SystemExit(f"error: required command from {env_key} not found in PATH: {value}")

    for candidate in candidates:
        if shutil.which(candidate):
            return candidate

    names = ", ".join(candidates)
    if env_key:
        raise SystemExit(f"error: required command for {label} not found; set {env_key} or install one of: {names}")
    raise SystemExit(f"error: required command for {label} not found; install one of: {names}")


def run_step(name: str, argv: list[str], env: dict[str, str]) -> None:
    print(f"== {name} ==", flush=True)
    print(" ".join(argv), flush=True)
    proc = subprocess.run(argv, cwd=ROOT, env=env, check=False)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def main() -> int:
    binary = resolve_binary()
    runtime = resolve_runtime()

    if not DEFAULT_FUNCTIONAL_ROOT.exists():
        raise SystemExit(f"error: functional testcase directory not found: {DEFAULT_FUNCTIONAL_ROOT}")
    if not DEFAULT_PERF_ROOT.exists():
        raise SystemExit(f"error: perf testcase directory not found: {DEFAULT_PERF_ROOT}")

    functional_gcc = require_command(
        "functional e2e compiler",
        None,
        ["riscv64-linux-gnu-gcc", "riscv64-unknown-linux-gnu-gcc"],
    )
    functional_qemu = require_command("functional e2e runner", None, ["qemu-riscv64"])
    perf_gxx = require_command("perf gcc baseline", "RISCV_GCC", ["riscv64-linux-gnu-g++"])
    perf_clang = require_command("perf clang baseline", "RISCV_CLANGXX", ["clang++"])
    perf_qemu = require_command("perf qemu runner", "QEMU_BIN", ["qemu-riscv64"])

    env = os.environ.copy()
    env["COMPILER_BIN"] = str(binary)
    env["SYSY_RUNTIME_LIB"] = str(runtime)
    env["PERF_TEST_DIRS"] = rel(DEFAULT_PERF_ROOT)

    print(f"Using compiler binary: {binary}", flush=True)
    print(f"Using SysY runtime: {runtime}", flush=True)
    print(f"Functional tests: {rel(DEFAULT_FUNCTIONAL_ROOT)}", flush=True)
    print(f"Perf tests: {env['PERF_TEST_DIRS']}", flush=True)
    print(f"Functional e2e compiler: {functional_gcc}", flush=True)
    print(f"Functional e2e runner: {functional_qemu}", flush=True)
    print(f"Perf gcc baseline: {perf_gxx}", flush=True)
    print(f"Perf clang baseline: {perf_clang}", flush=True)
    print(f"Perf qemu runner: {perf_qemu}", flush=True)

    run_step(
        "Functional tests",
        [
            sys.executable,
            str(ROOT / "scripts/run_tests.py"),
            "--suite",
            "stage",
            "--suite",
            "e2e",
            "--test-root",
            str(DEFAULT_FUNCTIONAL_ROOT),
            "--binary",
            str(binary),
            "--runtime",
            str(runtime),
            "--timeout",
            "50",
        ],
        env,
    )
    run_step(
        "Performance tests",
        [sys.executable, str(ROOT / "scripts/compare_perf.py")],
        env,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
