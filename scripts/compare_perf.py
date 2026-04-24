import os
import subprocess
import sys
import time
from pathlib import Path


def _find_repo_root() -> Path:
    script_dir = Path(__file__).resolve().parent
    return script_dir.parent


def _resolve_binary(repo_root: Path) -> Path:
    env_bin = os.environ.get("YOO_LANG_BIN", "").strip()
    if env_bin:
        p = Path(env_bin)
        return p if p.is_absolute() else (repo_root / p)
    return repo_root / "build" / "macosx" / "arm64" / "release" / "yoolang"


def _collect_sy_files(repo_root: Path) -> list[Path]:
    test_root = repo_root / "test"
    patterns = [
        "simple_test.sy",
        "bsb2025-final/*.sy",
        "bsb2025-prel/*.sy",
    ]
    files: list[Path] = []
    for pat in patterns:
        files.extend(sorted(test_root.glob(pat)))
    # De-dup while preserving order.
    seen = set()
    result: list[Path] = []
    for f in files:
        if f not in seen:
            seen.add(f)
            result.append(f)
    return result


def _run_one(binary: Path, src: Path, timeout_sec: int) -> tuple[bool, float, str]:
    start = time.perf_counter()
    try:
        ret = subprocess.run(
            [str(binary), str(src)],
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            check=False,
        )
        elapsed = time.perf_counter() - start
        if ret.returncode != 0:
            msg = ret.stderr.strip() or ret.stdout.strip() or f"exit={ret.returncode}"
            return False, elapsed, msg
        return True, elapsed, "OK"
    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - start
        return False, elapsed, "TIMEOUT"
    except Exception as exc:  # pragma: no cover
        elapsed = time.perf_counter() - start
        return False, elapsed, f"ERR: {exc}"


def main() -> int:
    repo_root = _find_repo_root()
    binary = _resolve_binary(repo_root)

    if not binary.exists():
        print(f"[ERROR] yoolang binary not found: {binary}")
        print("Hint: build first with xmake -m release")
        return 2

    all_cases = _collect_sy_files(repo_root)
    if not all_cases:
        print("[ERROR] no .sy testcases found under test/")
        return 2

    max_cases = int(os.environ.get("PERF_MAX_CASES", "30"))
    timeout_sec = int(os.environ.get("PERF_TIMEOUT_SEC", "20"))
    cases = all_cases[:max_cases]

    print("=== yoolang perf (compile-time) ===")
    print(f"Repo: {repo_root}")
    print(f"Binary: {binary}")
    print(f"Cases: {len(cases)}/{len(all_cases)} (PERF_MAX_CASES={max_cases})")
    print(f"Timeout per case: {timeout_sec}s")
    print("-" * 96)
    print(f"{'Case':<48} | {'Status':<8} | {'Time(s)':>9} | Detail")
    print("-" * 96)

    failed = 0
    total_time = 0.0
    times: list[float] = []

    for case in cases:
        ok, cost, detail = _run_one(binary, case, timeout_sec)
        status = "PASS" if ok else "FAIL"
        rel = case.relative_to(repo_root)
        print(f"{str(rel):<48} | {status:<8} | {cost:>9.4f} | {detail}")
        if ok:
            times.append(cost)
            total_time += cost
        else:
            failed += 1

    print("-" * 96)
    passed = len(cases) - failed
    avg = (sum(times) / len(times)) if times else 0.0
    fastest = min(times) if times else 0.0
    slowest = max(times) if times else 0.0
    print(
        f"Summary: total={len(cases)} pass={passed} fail={failed} "
        f"total_time={total_time:.4f}s avg={avg:.4f}s "
        f"min={fastest:.4f}s max={slowest:.4f}s"
    )

    if failed > 0:
        print("[ERROR] perf run has failures.")
        return 1
    print("[OK] perf run passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
