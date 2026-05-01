import os
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


MARKER = "; === RISC-V Assembly ==="


@dataclass
class RunResult:
    compile_ok: bool
    run_ok: bool
    elapsed_sec: float
    stdout: str = ""
    stderr: str = ""
    exit_code: Optional[int] = None
    detail: str = ""


WORKSPACE = Path(os.environ.get("GITHUB_WORKSPACE", Path(__file__).resolve().parents[1])).resolve()
RUNTIME_HEADER = WORKSPACE / "runtime" / "sylib.h"
RUNTIME_SOURCE = WORKSPACE / "runtime" / "sylib.c"
DEFAULT_RUNTIME_LIB = WORKSPACE / "runtime" / "libsysy_riscv.a"
DEFAULT_RUNTIME_LIB_ALT = WORKSPACE / "runtime" / "libsysy.a"
QEMU_BIN = os.environ.get("QEMU_BIN", "qemu-riscv64")
GCC_BIN = os.environ.get("RISCV_GCC", "riscv64-linux-gnu-g++")
CLANG_BIN = os.environ.get("RISCV_CLANGXX", "clang++")
AR_BIN = os.environ.get("RISCV_AR", "riscv64-linux-gnu-ar")
RANLIB_BIN = os.environ.get("RISCV_RANLIB", "riscv64-linux-gnu-ranlib")
TIMEOUT_SEC = int(os.environ.get("PERF_TIMEOUT_SEC", "20"))
REPORT_DIR = WORKSPACE / "build" / "perf-ci"
REPORT_MD = REPORT_DIR / "perf-report.md"
REPORT_JSON = REPORT_DIR / "perf-report.json"
MAX_STATUS_CELL_CHARS = 4000


def _resolve_yoolang_binary() -> Path:
    env_bin = os.environ.get("YOO_LANG_BIN", "").strip()
    if env_bin:
        candidate = Path(env_bin)
        return candidate if candidate.is_absolute() else (WORKSPACE / candidate)

    candidates = []
    for path in (WORKSPACE / "build").rglob("yoolang"):
        if path.is_file() and "release" in path.parts:
            candidates.append(path)
    if not candidates:
        raise FileNotFoundError("cannot find built yoolang binary under build/**/release")
    return sorted(candidates)[0]


def _normalize_text(text: str) -> str:
    return text.replace("\r\n", "\n").strip()


def _collect_cases() -> list[Path]:
    env_dirs = os.environ.get("PERF_TEST_DIRS", "").strip()
    if env_dirs:
        roots = [WORKSPACE / item.strip() for item in env_dirs.split(",") if item.strip()]
    else:
        preferred = ["test/perf_tests", "test/bsb-final", "test/bsb-perl"]
        roots = [WORKSPACE / rel for rel in preferred if (WORKSPACE / rel).exists()]
        if not roots:
            roots = [WORKSPACE / "test"]

    cases: list[Path] = []
    for root in roots:
        if root.is_file() and root.suffix == ".sy":
            cases.append(root)
        elif root.is_dir():
            cases.extend(sorted(root.rglob("*.sy")))

    seen: set[Path] = set()
    ordered: list[Path] = []
    for case in cases:
        if case not in seen:
            seen.add(case)
            ordered.append(case)
    max_cases = int(os.environ.get("PERF_MAX_CASES", "0"))
    if max_cases > 0:
        return ordered[:max_cases]
    return ordered


CASES = _collect_cases()


def _ensure_runtime_lib() -> Path:
    env_lib = os.environ.get("SYSY_RUNTIME_LIB", "").strip()
    if env_lib:
        p = Path(env_lib)
        runtime_lib = p if p.is_absolute() else (WORKSPACE / p)
        if runtime_lib.exists():
            return runtime_lib
        raise FileNotFoundError(f"SYSY_RUNTIME_LIB was set but file does not exist: {runtime_lib}")

    for runtime_lib in (DEFAULT_RUNTIME_LIB_ALT, DEFAULT_RUNTIME_LIB):
        if runtime_lib.exists():
            return runtime_lib

    if not RUNTIME_SOURCE.exists() or not RUNTIME_HEADER.exists():
        raise FileNotFoundError(
            "missing runtime/sylib.c or runtime/sylib.h, cannot build sysy runtime"
        )

    build_dir = WORKSPACE / "build" / "perf-ci" / "runtime"
    build_dir.mkdir(parents=True, exist_ok=True)
    obj_file = build_dir / "sylib.o"
    lib_file = build_dir / "libsysy.a"

    cmake_file = WORKSPACE / "runtime" / "CMakeLists.txt"
    if cmake_file.exists():
        cmake_build_dir = build_dir / "cmake"
        subprocess.run(
            [
                "cmake",
                "-S",
                str(WORKSPACE / "runtime"),
                "-B",
                str(cmake_build_dir),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc",
                f"-DCMAKE_AR={AR_BIN}",
                f"-DCMAKE_RANLIB={RANLIB_BIN}",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["cmake", "--build", str(cmake_build_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        cmake_candidates = list(cmake_build_dir.rglob("libsysy.a")) + list(cmake_build_dir.rglob("libsysy_riscv.a"))
        if cmake_candidates:
            picked = sorted(cmake_candidates)[0]
            if picked != lib_file:
                lib_file.write_bytes(picked.read_bytes())
            return lib_file
        raise FileNotFoundError(
            f"runtime/CMakeLists.txt exists but no libsysy.a or libsysy_riscv.a was produced under {cmake_build_dir}"
        )

    subprocess.run(
        [
            "riscv64-linux-gnu-gcc",
            "-O2",
            "-I",
            str(RUNTIME_HEADER.parent),
            "-c",
            str(RUNTIME_SOURCE),
            "-o",
            str(obj_file),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        [AR_BIN, "rcs", str(lib_file), str(obj_file)],
        check=True,
        capture_output=True,
        text=True,
    )
    if not lib_file.exists():
        raise FileNotFoundError(f"runtime library build failed, expected: {lib_file}")
    return lib_file


def _ensure_runtime_wrapper() -> Path:
    wrapper_dir = WORKSPACE / "build" / "perf-ci" / "runtime"
    wrapper_dir.mkdir(parents=True, exist_ok=True)
    wrapper = wrapper_dir / "sylib_wrapper.hpp"
    wrapper.write_text(
        "#ifndef SYLIB_WRAPPER_HPP\n"
        "#define SYLIB_WRAPPER_HPP\n"
        "extern \"C\" {\n"
        "int getint();\n"
        "int getch();\n"
        "int getarray(int a[]);\n"
        "float getfloat();\n"
        "int getfarray(float a[]);\n"
        "void putint(int a);\n"
        "void putch(int a);\n"
        "void putarray(int n, int a[]);\n"
        "void putfloat(float a);\n"
        "void putfarray(int n, float a[]);\n"
        "void putf(char a[], ...);\n"
        "void _sysy_starttime(int lineno);\n"
        "void _sysy_stoptime(int lineno);\n"
        "}\n"
        "#define starttime() _sysy_starttime(__LINE__)\n"
        "#define stoptime() _sysy_stoptime(__LINE__)\n"
        "#endif\n"
    )
    return wrapper


def flatten_multidim_array_io_calls_for_baseline(src: str) -> str:
    multidim_int_arrays: set[str] = set()
    decl_re = re.compile(r"\bint\s+([A-Za-z_]\w*)\s*(?:\[[^\]]+\]){2,}\s*(?:=|;)")

    for match in decl_re.finditer(src):
        multidim_int_arrays.add(match.group(1))

    for name in sorted(multidim_int_arrays, key=len, reverse=True):
        ident = re.escape(name)
        src = re.sub(
            rf"\bgetarray\s*\(\s*{ident}\s*\)",
            f"getarray((int*){name})",
            src,
        )
        src = re.sub(
            rf"\bputarray\s*\(\s*([^,\n]+?)\s*,\s*{ident}\s*\)",
            rf"putarray(\1, (int*){name})",
            src,
        )

    return src


def _prepare_baseline_source(src: Path, out_dir: Path) -> Path:
    baseline_src = flatten_multidim_array_io_calls_for_baseline(src.read_text())
    baseline_file = out_dir / f"{src.stem}.baseline.cc"
    baseline_file.write_text(baseline_src)
    return baseline_file


try:
    YOOLANG_BIN = _resolve_yoolang_binary()
    RUNTIME_LIB = _ensure_runtime_lib()
    RUNTIME_WRAPPER = _ensure_runtime_wrapper()
except Exception as exc:
    print(f"[ERROR] {exc}")
    sys.exit(2)


def _compile_gcc(src: Path, out_dir: Path) -> tuple[Path, str]:
    baseline_src = _prepare_baseline_source(src, out_dir)
    obj = out_dir / f"{src.stem}.gcc.o"
    exe = out_dir / f"{src.stem}.gcc.riscv"
    cmd = [
        GCC_BIN,
        "-O3",
        "-std=gnu++17",
        "-include",
        str(RUNTIME_WRAPPER),
        "-x",
        "c++",
        "-c",
        str(baseline_src),
        "-o",
        str(obj),
    ]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _compile_clang(src: Path, out_dir: Path) -> tuple[Path, str]:
    baseline_src = _prepare_baseline_source(src, out_dir)
    obj = out_dir / f"{src.stem}.clang.o"
    exe = out_dir / f"{src.stem}.clang.riscv"
    cmd = [
        CLANG_BIN,
        "-O3",
        "-std=gnu++17",
        "--target=riscv64-linux-gnu",
        "--sysroot=/usr/riscv64-linux-gnu",
        "-include",
        str(RUNTIME_WRAPPER),
        "-x",
        "c++",
        "-c",
        str(baseline_src),
        "-o",
        str(obj),
    ]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _emit_yoolang_asm(src: Path, out_dir: Path) -> tuple[Path, str]:
    result = subprocess.run(
        [str(YOOLANG_BIN), str(src), "--emit-asm"],
        check=True,
        capture_output=True,
        text=True,
    )
    raw = result.stdout
    marker_pos = raw.find(MARKER)
    # Newer frontend prints plain assembly directly; older builds may still emit a marker.
    if marker_pos >= 0:
        asm_text = raw[marker_pos + len(MARKER):].lstrip("\r\n")
    else:
        asm_text = raw
    asm_file = out_dir / f"{src.stem}.yoolang.s"
    asm_file.write_text(asm_text)
    return asm_file, "OK"


def _compile_yoolang(src: Path, out_dir: Path) -> tuple[Path, str]:
    asm_file, _ = _emit_yoolang_asm(src, out_dir)
    obj = out_dir / f"{src.stem}.yoolang.o"
    exe = out_dir / f"{src.stem}.yoolang.riscv"
    subprocess.run([GCC_BIN, "-c", str(asm_file), "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _run_qemu(exe: Path, input_file: Optional[Path]) -> tuple[bool, float, str, str, Optional[int], str]:
    stdin_data = input_file.read_bytes() if input_file and input_file.exists() else None
    start = time.perf_counter()
    try:
        result = subprocess.run(
            [QEMU_BIN, "-L", "/usr/riscv64-linux-gnu", str(exe)],
            input=stdin_data,
            capture_output=True,
            text=False,
            timeout=TIMEOUT_SEC,
            check=False,
        )
        elapsed = time.perf_counter() - start
        stdout = result.stdout.decode(errors="replace") if result.stdout else ""
        stderr = result.stderr.decode(errors="replace") if result.stderr else ""
        ok = result.returncode == 0
        return ok, elapsed, stdout, stderr, result.returncode, "OK" if ok else f"exit={result.returncode}"
    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - start
        return False, elapsed, "", "", None, "TIMEOUT"
    except Exception as exc:  # pragma: no cover
        elapsed = time.perf_counter() - start
        return False, elapsed, "", "", None, f"ERR: {exc}"


def _expected_input(case: Path) -> Optional[Path]:
    candidate = case.with_suffix(".in")
    return candidate if candidate.exists() else None


def _case_work_dir(case: Path) -> Path:
    out_dir = WORKSPACE / "build" / "perf-ci" / case.parent.relative_to(WORKSPACE) / case.stem
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir


def _format_cell(result: RunResult) -> str:
    if not result.compile_ok:
        return "CFAIL"
    if not result.run_ok:
        return result.detail
    return f"{result.elapsed_sec:.4f}s"


def _format_detail(label: str, result: RunResult) -> str:
    parts: list[str] = []
    if not result.compile_ok:
        parts.append(f"{label} compile failed: {result.detail}")
    elif not result.run_ok:
        parts.append(f"{label} run failed: {result.detail}")
    if result.exit_code is not None:
        parts.append(f"{label} exit code: {result.exit_code}")
    if result.stdout.strip():
        parts.append(f"{label} stdout:\n{result.stdout.strip()}")
    if result.stderr.strip():
        parts.append(f"{label} stderr:\n{result.stderr.strip()}")
    return "\n\n".join(parts)


def _compile_and_run(src: Path) -> tuple[RunResult, RunResult, RunResult, bool, str]:
    input_file = _expected_input(src)
    case_dir = _case_work_dir(src)

    results: dict[str, RunResult] = {}

    compiler_specs = (
        ("gcc", _compile_gcc),
        ("clang++", _compile_clang),
        ("yoolang", _compile_yoolang),
    )

    for name, compiler in compiler_specs:
        start = time.perf_counter()
        try:
            exe, _ = compiler(src, case_dir)
        except subprocess.CalledProcessError as exc:
            elapsed = time.perf_counter() - start
            stderr = (exc.stderr or "").strip()
            stdout = (exc.stdout or "").strip()
            detail = stderr or stdout or "compile error"
            results[name] = RunResult(False, False, elapsed, detail=detail)
            continue
        except Exception as exc:
            elapsed = time.perf_counter() - start
            results[name] = RunResult(False, False, elapsed, detail=f"ERR: {exc}")
            continue

        run_ok, elapsed, stdout, stderr, exit_code, detail = _run_qemu(exe, input_file)
        results[name] = RunResult(True, run_ok, elapsed, stdout, stderr, exit_code, detail)

    gcc = results["gcc"]
    clang = results["clang++"]
    yoolang = results["yoolang"]

    if not gcc.compile_ok or not gcc.run_ok:
        return gcc, clang, yoolang, False, f"gcc {gcc.detail}"
    if not clang.compile_ok or not clang.run_ok:
        return gcc, clang, yoolang, False, f"clang++ {clang.detail}"
    if not yoolang.compile_ok or not yoolang.run_ok:
        return gcc, clang, yoolang, False, f"yoolang {yoolang.detail}"

    baseline_stdout = _normalize_text(gcc.stdout)
    baseline_exit = gcc.exit_code
    clang_stdout = _normalize_text(clang.stdout)
    yoolang_stdout = _normalize_text(yoolang.stdout)

    if clang_stdout != baseline_stdout or clang.exit_code != baseline_exit:
        return gcc, clang, yoolang, False, f"clang++ output mismatch vs gcc for {src.name}"
    if yoolang_stdout != baseline_stdout or yoolang.exit_code != baseline_exit:
        return gcc, clang, yoolang, False, f"yoolang output mismatch vs gcc for {src.name}"

    return gcc, clang, yoolang, True, "OK"


def _print_header() -> None:
    print("=== RISC-V/QEMU perf compare ===")
    print(f"Workspace: {WORKSPACE}")
    print(f"Yoolang binary: {YOOLANG_BIN}")
    print(f"Runtime lib: {RUNTIME_LIB}")
    print(f"Runtime wrapper: {RUNTIME_WRAPPER}")
    print(f"Test dirs: {', '.join(str(root.relative_to(WORKSPACE)) for root in TEST_ROOTS)}")
    print(f"Cases: {len(CASES)}")
    print(f"Timeout per qemu run: {TIMEOUT_SEC}s")
    print("-" * 140)


def _md_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", "<br>")


def _truncate_text(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit] + f"\n... <truncated, {len(text) - limit} chars omitted> ..."


def _write_reports(rows: list[dict], failures: int, total_runtime: float) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)

    status = "PASS" if failures == 0 else "FAIL"
    generated = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())

    md_lines = [
        "# RISC-V QEMU Perf Report",
        "",
        f"- Status: {status}",
        f"- Generated: {generated}",
        f"- Cases: {len(rows)}",
        f"- Failed: {failures}",
        f"- Total runtime (s): {total_runtime:.4f}",
        f"- Yoolang binary: {YOOLANG_BIN}",
        f"- Runtime lib: {RUNTIME_LIB}",
        "",
        "| Case | GCC | Clang++ | Yoolang | Status |",
        "| --- | --- | --- | --- | --- |",
    ]

    display_rows = sorted(rows, key=lambda row: str(row.get("status", "")) == "OK")
    for row in display_rows:
        detail = str(row.get("detail", "")).strip()
        status_cell = str(row["status"])
        if detail:
            status_cell = f"{status_cell}\n\n{detail}"
        status_cell = _truncate_text(status_cell, MAX_STATUS_CELL_CHARS)
        md_lines.append(
            "| "
            + " | ".join(
                [
                    _md_escape(str(row["case"])),
                    _md_escape(str(row["gcc"])),
                    _md_escape(str(row["clang"])),
                    _md_escape(str(row["yoolang"])),
                    _md_escape(status_cell),
                ]
            )
            + " |"
        )

    REPORT_MD.write_text("\n".join(md_lines) + "\n")

    payload = {
        "status": status,
        "generated_utc": generated,
        "workspace": str(WORKSPACE),
        "yoolang_binary": str(YOOLANG_BIN),
        "runtime_lib": str(RUNTIME_LIB),
        "test_dirs": [str(root.relative_to(WORKSPACE)) for root in TEST_ROOTS],
        "cases": len(rows),
        "failures": failures,
        "total_runtime_sec": total_runtime,
        "rows": rows,
    }
    REPORT_JSON.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    print(f"{'Case':<42} | {'GCC':<12} | {'Clang++':<12} | {'Yoolang':<12} | Status")
    print("-" * 140)


TEST_ROOTS = []
env_dirs = os.environ.get("PERF_TEST_DIRS", "").strip()
if env_dirs:
    TEST_ROOTS = [WORKSPACE / item.strip() for item in env_dirs.split(",") if item.strip()]
else:
    for rel in ("test/perf_tests", "test/bsb-final", "test/bsb-perl"):
        candidate = WORKSPACE / rel
        if candidate.exists():
            TEST_ROOTS.append(candidate)
    if not TEST_ROOTS:
        TEST_ROOTS = [WORKSPACE / "test"]


if not CASES:
    print("[ERROR] no .sy testcases found")
    sys.exit(2)



def main() -> int:
    _print_header()
    failures = 0
    total_runtime = 0.0
    report_rows: list[dict] = []

    for case in CASES:
        gcc, clang, yoolang, ok, detail = _compile_and_run(case)
        total_runtime += gcc.elapsed_sec + clang.elapsed_sec + yoolang.elapsed_sec
        rel = str(case.relative_to(WORKSPACE))
        error_detail = ""
        if not ok:
            error_detail = "\n\n".join(
                item
                for item in (
                    _format_detail("gcc", gcc),
                    _format_detail("clang++", clang),
                    _format_detail("yoolang", yoolang),
                )
                if item
            )
        print(
            f"{rel:<42} | {_format_cell(gcc):<12} | {_format_cell(clang):<12} | {_format_cell(yoolang):<12} | {detail}"
        )
        report_rows.append(
            {
                "case": rel,
                "gcc": _format_cell(gcc),
                "clang": _format_cell(clang),
                "yoolang": _format_cell(yoolang),
                "status": detail,
                "detail": error_detail,
            }
        )
        if not ok:
            failures += 1
            print(f"[FAIL] {rel}: {detail}")
            if error_detail:
                print(error_detail)

    print("-" * 140)
    print(f"Summary: cases={len(CASES)} failed={failures} total_run_time={total_runtime:.4f}s")

    _write_reports(report_rows, failures, total_runtime)
    print(f"Report markdown: {REPORT_MD}")
    print(f"Report json: {REPORT_JSON}")

    if failures > 0:
        print("[ERROR] perf compare failed.")
        return 1
    print("[OK] perf compare passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
