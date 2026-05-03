#!/usr/bin/env python3

import argparse
import difflib
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Run text snapshot tests for YIR dumps.")
    parser.add_argument(
        "--binary",
        default="./build/linux/x86_64/release/compiler",
        help="path to the compiler binary",
    )
    parser.add_argument(
        "--test-dir",
        default="test/yir",
        help="directory containing .sy YIR tests",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="overwrite .sy.yir snapshots with current output",
    )
    return parser.parse_args()


def run_one(binary: Path, source: Path):
    proc = subprocess.run(
        [str(binary), "--emit-yir", str(source)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def main():
    args = parse_args()
    binary = Path(args.binary)
    test_dir = Path(args.test_dir)

    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2
    if not test_dir.exists():
        print(f"error: test directory not found: {test_dir}", file=sys.stderr)
        return 2

    sources = sorted(test_dir.glob("*.sy"))
    if not sources:
        print(f"error: no .sy files found in {test_dir}", file=sys.stderr)
        return 2

    failed = 0
    for source in sources:
        expected_path = source.with_suffix(source.suffix + ".yir")
        rc, stdout, stderr = run_one(binary, source)
        if rc != 0:
            failed += 1
            print(f"FAIL {source}: compiler exited with {rc}", file=sys.stderr)
            if stderr:
                print(stderr, file=sys.stderr, end="" if stderr.endswith("\n") else "\n")
            continue

        if args.update:
            expected_path.write_text(stdout, encoding="utf-8")
            print(f"UPDATE {expected_path}")
            continue

        if not expected_path.exists():
            failed += 1
            print(f"FAIL {source}: missing snapshot {expected_path}", file=sys.stderr)
            continue

        expected = expected_path.read_text(encoding="utf-8")
        if stdout != expected:
            failed += 1
            print(f"FAIL {source}: snapshot mismatch", file=sys.stderr)
            diff = difflib.unified_diff(
                expected.splitlines(keepends=True),
                stdout.splitlines(keepends=True),
                fromfile=str(expected_path),
                tofile=f"{source} (actual)",
            )
            print("".join(diff), file=sys.stderr, end="")
        else:
            print(f"PASS {source}")

    if failed:
        print(f"{failed} YIR snapshot test(s) failed", file=sys.stderr)
        return 1
    print(f"{len(sources)} YIR snapshot test(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
