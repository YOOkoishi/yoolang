import os
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class RunResult:
    compile_ok: bool
    run_ok: bool
    elapsed_sec: float
    stdout: str = ""
    stderr: str = ""
    exit_code: Optional[int] = None
    detail: str = ""
    metrics: dict[str, int] | None = None


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
STATUS_EMOJI = {
    "PASS": "✅",
    "FAIL": "❌",
    "OK": "✅",
    "CFAIL": "🛠️",
    "TIMEOUT": "⏱️",
}


def _resolve_compiler_binary() -> Path:
    env_bin = os.environ.get("COMPILER_BIN", "").strip()
    if env_bin:
        candidate = Path(env_bin)
        return candidate if candidate.is_absolute() else (WORKSPACE / candidate)

    candidates = []
    for path in (WORKSPACE / "build").rglob("compiler"):
        if path.is_file() and "release" in path.parts:
            candidates.append(path)
    if not candidates:
        raise FileNotFoundError("cannot find built compiler binary under build/**/release")
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

    excluded: set[str] = set()
    env_excludes = os.environ.get("PERF_EXCLUDE_CASES", "").strip()
    if env_excludes:
        for item in re.split(r"[,\n]+", env_excludes):
            item = item.strip()
            if item:
                excluded.add(item)

    seen: set[Path] = set()
    ordered: list[Path] = []
    for case in cases:
        rel_case = str(case.relative_to(WORKSPACE))
        if rel_case in excluded:
            continue
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
    COMPILER_BIN = _resolve_compiler_binary()
    RUNTIME_LIB = _ensure_runtime_lib()
    RUNTIME_WRAPPER = _ensure_runtime_wrapper()
except Exception as exc:
    print(f"❌ [ERROR] {exc}")
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


def _emit_compiler_asm(src: Path, out_dir: Path) -> tuple[Path, str]:
    asm_file = out_dir / f"{src.stem}.compiler.s"
    subprocess.run(
        [str(COMPILER_BIN), str(src), "-S", "-O1", "-o", str(asm_file)],
        check=True,
        capture_output=True,
        text=True,
    )
    return asm_file, "OK"


def _compile_compiler(src: Path, out_dir: Path) -> tuple[Path, str]:
    asm_file, _ = _emit_compiler_asm(src, out_dir)
    obj = out_dir / f"{src.stem}.compiler.o"
    exe = out_dir / f"{src.stem}.compiler.riscv"
    subprocess.run([GCC_BIN, "-c", str(asm_file), "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _collect_codegen_metrics(src: Path, out_dir: Path) -> dict[str, int]:
    asm_file = out_dir / f"{src.stem}.compiler.s"
    metrics = {
        "mir_instrs": 0,
        "stack_slots": 0,
        "load_slot": 0,
        "store_slot": 0,
        "move": 0,
        "fmove": 0,
        "asm_lines": 0,
    }

    if asm_file.exists():
        metrics["asm_lines"] = sum(
            1
            for line in asm_file.read_text(errors="replace").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        )

    try:
        proc = subprocess.run(
            [str(COMPILER_BIN), str(src), "--emit-mir", "-O1"],
            check=True,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SEC,
        )
    except Exception:
        return metrics

    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if re.match(r"^(LI|FLI\.S|LA|FI_ADDR|LOAD_SLOT|STORE_SLOT|LOAD_MEM|STORE_MEM|MEMZERO|MV|FMV\.S|ADD|ADDW|SUBW|MUL|MULW|DIVW|REMW|AND|SLLI|SRLIW|XOR|XORI|SLT|SEQZ|SNEZ|FADD\.S|FSUB\.S|FMUL\.S|FDIV\.S|FEQ\.S|FLT\.S|FLE\.S|FCVT\.S\.W|FCVT\.W\.S|FMV\.W\.X|STORE_OUT_ARG|LOAD_IN_ARG|BNEZ|J|CALL)\b", stripped):
            metrics["mir_instrs"] += 1
        if " fi#" in line or stripped.startswith("fi#"):
            metrics["stack_slots"] += 1
        if stripped.startswith("LOAD_SLOT"):
            metrics["load_slot"] += 1
        elif stripped.startswith("STORE_SLOT"):
            metrics["store_slot"] += 1
        elif stripped.startswith("MV"):
            metrics["move"] += 1
        elif stripped.startswith("FMV.S"):
            metrics["fmove"] += 1
    return metrics


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


def _expected_output(case: Path) -> tuple[str, int] | None:
    candidate = case.with_suffix(".out")
    if not candidate.exists():
        return None

    text = candidate.read_text(errors="replace").replace("\r\n", "\n").strip()
    if not text:
        return "", 0

    lines = text.splitlines()
    last = lines[-1].strip()
    if re.fullmatch(r"-?\d+", last):
        return "\n".join(lines[:-1]).strip(), int(last)
    return text, 0


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
    expected = _expected_output(src)
    case_dir = _case_work_dir(src)

    results: dict[str, RunResult] = {}

    compiler_specs = (
        ("gcc", _compile_gcc),
        ("clang++", _compile_clang),
        ("compiler", _compile_compiler),
    )

    for name, compile_func in compiler_specs:
        start = time.perf_counter()
        try:
            exe, _ = compile_func(src, case_dir)
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

    if "compiler" in results and results["compiler"].compile_ok:
        results["compiler"].metrics = _collect_codegen_metrics(src, case_dir)

    gcc = results["gcc"]
    clang = results["clang++"]
    compiler = results["compiler"]

    if not gcc.compile_ok or not gcc.run_ok:
        return gcc, clang, compiler, False, f"gcc {gcc.detail}"
    if not clang.compile_ok or not clang.run_ok:
        return gcc, clang, compiler, False, f"clang++ {clang.detail}"
    if not compiler.compile_ok or not compiler.run_ok:
        return gcc, clang, compiler, False, f"compiler {compiler.detail}"

    clang_stdout = _normalize_text(clang.stdout)
    compiler_stdout = _normalize_text(compiler.stdout)

    if expected is not None:
        expected_stdout, expected_exit = expected
        if compiler_stdout != _normalize_text(expected_stdout) or compiler.exit_code != expected_exit:
            return (
                gcc,
                clang,
                compiler,
                False,
                f"compiler output mismatch vs expected .out for {src.name}",
            )
        return gcc, clang, compiler, True, "OK"

    baseline_stdout = _normalize_text(gcc.stdout)
    baseline_exit = gcc.exit_code

    if clang_stdout != baseline_stdout or clang.exit_code != baseline_exit:
        return gcc, clang, compiler, False, f"clang++ output mismatch vs gcc for {src.name}"
    if compiler_stdout != baseline_stdout or compiler.exit_code != baseline_exit:
        return gcc, clang, compiler, False, f"compiler output mismatch vs gcc for {src.name}"

    return gcc, clang, compiler, True, "OK"


def _print_header() -> None:
    print("=== RISC-V/QEMU perf compare ===")
    print(f"Workspace: {WORKSPACE}")
    print(f"Compiler binary: {COMPILER_BIN}")
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
        "# 📊 RISC-V QEMU Perf Report",
        "",
        f"- Status: {STATUS_EMOJI[status]} {status}",
        f"- Generated: {generated}",
        f"- Cases: {len(rows)}",
        f"- Failed: {failures}",
        "- Compiler opt: `-O1`",
        f"- Total runtime (s): {total_runtime:.4f}",
        f"- Compiler binary: {COMPILER_BIN}",
        f"- Runtime lib: {RUNTIME_LIB}",
        "",
        "| Case | GCC | Clang++ | Compiler | Status |",
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
                    _md_escape(str(row["compiler"])),
                    _md_escape(status_cell),
                ]
            )
            + " |"
        )

    REPORT_MD.write_text("\n".join(md_lines) + "\n")

    payload = {
        "status": status,
        "status_emoji": STATUS_EMOJI[status],
        "generated_utc": generated,
        "workspace": str(WORKSPACE),
        "compiler_binary": str(COMPILER_BIN),
        "compiler_opt": "-O1",
        "runtime_lib": str(RUNTIME_LIB),
        "test_dirs": [str(root.relative_to(WORKSPACE)) for root in TEST_ROOTS],
        "cases": len(rows),
        "failures": failures,
        "total_runtime_sec": total_runtime,
        "rows": rows,
    }
    REPORT_JSON.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    print(f"{'Case':<42} | {'GCC':<12} | {'Clang++':<12} | {'Compiler':<12} | Status")
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
    print("❌ [ERROR] no .sy testcases found")
    sys.exit(2)



def main() -> int:
    _print_header()
    failures = 0
    total_runtime = 0.0
    report_rows: list[dict] = []

    for case in CASES:
        gcc, clang, compiler, ok, detail = _compile_and_run(case)
        total_runtime += gcc.elapsed_sec + clang.elapsed_sec + compiler.elapsed_sec
        rel = str(case.relative_to(WORKSPACE))
        error_detail = ""
        if not ok:
            error_detail = "\n\n".join(
                item
                for item in (
                    _format_detail("gcc", gcc),
                    _format_detail("clang++", clang),
                    _format_detail("compiler", compiler),
                )
                if item
            )
        print(
            f"{rel:<42} | {_format_cell(gcc):<12} | {_format_cell(clang):<12} | {_format_cell(compiler):<12} | {STATUS_EMOJI.get(detail, 'ℹ️')} {detail}"
        )
        report_rows.append(
            {
                "case": rel,
                "gcc": _format_cell(gcc),
                "clang": _format_cell(clang),
                "compiler": _format_cell(compiler),
                "status": detail,
                "detail": error_detail,
                "codegen_metrics": compiler.metrics or {},
            }
        )
        if not ok:
            failures += 1
            print(f"❌ [FAIL] {rel}: {detail}")
            if error_detail:
                print(error_detail)

    print("-" * 140)
    print(f"📌 Summary: cases={len(CASES)} failed={failures} total_run_time={total_runtime:.4f}s")

    _write_reports(report_rows, failures, total_runtime)
    print(f"📄 Report markdown: {REPORT_MD}")
    print(f"🧾 Report json: {REPORT_JSON}")

    if failures > 0:
        print("❌ [ERROR] perf compare failed.")
        return 1
    print("✅ [OK] perf compare passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
