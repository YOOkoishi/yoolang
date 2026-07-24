from __future__ import annotations

import os
import json
import math
import re
import shutil
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
    metrics: dict[str, object] | None = None
    exe_path: Path | None = None
    instruction_count: int | None = None
    instruction_count_status: str = "DISABLED"
    instruction_count_detail: str = ""


WORKSPACE = Path(os.environ.get("GITHUB_WORKSPACE", Path(__file__).resolve().parents[1])).resolve()
RUNTIME_HEADER = WORKSPACE / "runtime" / "sylib.h"
RUNTIME_SOURCE = WORKSPACE / "runtime" / "sylib.c"
DEFAULT_RUNTIME_LIB = WORKSPACE / "runtime" / "libsysy_riscv.a"
DEFAULT_RUNTIME_LIB_ALT = WORKSPACE / "runtime" / "libsysy.a"
QEMU_BIN = os.environ.get("QEMU_BIN", "qemu-riscv64")
QEMU_RISCV64 = os.environ.get("QEMU_RISCV64", QEMU_BIN)
GCC_BIN = os.environ.get("RISCV_GCC", "riscv64-linux-gnu-g++")
CLANG_BIN = os.environ.get("RISCV_CLANGXX", "clang++")
AR_BIN = os.environ.get("RISCV_AR", "riscv64-linux-gnu-ar")
RANLIB_BIN = os.environ.get("RISCV_RANLIB", "riscv64-linux-gnu-ranlib")
TIMEOUT_SEC = int(os.environ.get("PERF_TIMEOUT_SEC", "20"))
QEMU_INSN_TIMEOUT = int(os.environ.get("QEMU_INSN_TIMEOUT", "30"))
ENABLE_QEMU_INSN_COUNT = os.environ.get("ENABLE_QEMU_INSN_COUNT", "0").strip().lower() in {"1", "true", "yes", "on"}
QEMU_INSN_STRICT = os.environ.get("QEMU_INSN_STRICT", "0").strip().lower() in {"1", "true", "yes", "on"}
RISCV_TARGET_FLAGS = ["-march=rv64gc", "-mabi=lp64d", "-mcmodel=medany"]
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
INSN_RE = re.compile(r"QEMU_INSN_COUNT\s+total_instructions=(\d+)")


@dataclass
class InstructionCountResult:
    status: str
    count: int | None = None
    detail: str = ""


class QemuInstructionCounter:
    def __init__(self) -> None:
        self.enabled = ENABLE_QEMU_INSN_COUNT
        self.strict = QEMU_INSN_STRICT
        self.qemu = QEMU_RISCV64
        env_plugin = os.environ.get("QEMU_INSN_PLUGIN", "").strip()
        if env_plugin:
            self.plugin = Path(env_plugin)
            if not self.plugin.is_absolute():
                self.plugin = WORKSPACE / self.plugin
        else:
            self.plugin = WORKSPACE / "tools" / "qemu-insn-count" / "count_insn.py"
        self._ready: bool | None = None
        self._reason = "disabled"

    def ensure_ready(self) -> bool:
        if not self.enabled:
            self._ready = False
            self._reason = "ENABLE_QEMU_INSN_COUNT is not enabled"
            return False
        if self._ready is not None:
            return self._ready

        qemu_path = shutil.which(self.qemu) if not Path(self.qemu).is_absolute() else self.qemu
        if not qemu_path:
            self._ready = False
            self._reason = f"QEMU_RISCV64 not found: {self.qemu}"
            return False

        help_proc = subprocess.run(
            [self.qemu, "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        help_text = (help_proc.stdout or "") + (help_proc.stderr or "")
        if "-plugin" not in help_text:
            self._ready = False
            self._reason = f"{self.qemu} does not advertise -plugin support"
            return False

        # If user provided a pre-built plugin, check it exists.
        if os.environ.get("QEMU_INSN_PLUGIN", "").strip():
            if not self.plugin.exists():
                self._ready = False
                self._reason = f"QEMU_INSN_PLUGIN not found: {self.plugin}"
                return False
            self._ready = True
            return True

        # Otherwise, verify the on-the-fly build tooling is available.
        if not self.plugin.exists():
            self._ready = False
            self._reason = f"count_insn.py not found: {self.plugin}"
            return False

        if not shutil.which("gcc"):
            self._ready = False
            self._reason = "gcc not found on PATH (required for plugin compilation)"
            return False

        self._ready = True
        return True

    @property
    def reason(self) -> str:
        self.ensure_ready()
        return self._reason

    def count(self, exe: Path, input_file: Optional[Path]) -> InstructionCountResult:
        if not self.enabled:
            return InstructionCountResult("DISABLED", detail="ENABLE_QEMU_INSN_COUNT is not enabled")
        if not self.ensure_ready():
            return InstructionCountResult("SKIPPED", detail=self._reason)

        count_script = WORKSPACE / "tools" / "qemu-insn-count" / "count_insn.py"
        if not count_script.exists():
             return InstructionCountResult("FAILED", detail=f"Count script not found: {count_script}")
        stdin_data = input_file.read_bytes() if input_file and input_file.exists() else None
        try:
            result = subprocess.run(
                [
                    sys.executable,
                    str(count_script),
                    self.qemu,
                    "-L",
                    "/usr/riscv64-linux-gnu",
                    str(exe),
                ],
                input=stdin_data,
                capture_output=True,
                text=False,
                timeout=QEMU_INSN_TIMEOUT,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return InstructionCountResult("FAILED", detail=f"TIMEOUT after {QEMU_INSN_TIMEOUT}s")
        except Exception as exc:
            return InstructionCountResult("FAILED", detail=f"ERR: {exc}")

        stdout = result.stdout.decode(errors="replace") if result.stdout else ""
        stderr = result.stderr.decode(errors="replace") if result.stderr else ""
        match = INSN_RE.search(stdout + "\n" + stderr)
        if result.returncode != 0:
            detail = f"qemu exited with {result.returncode}"
            if stderr.strip():
                detail += f": {stderr.strip().splitlines()[-1]}"
            return InstructionCountResult("FAILED", detail=detail)
        if not match:
            return InstructionCountResult("FAILED", detail="QEMU_INSN_COUNT marker not found")
        return InstructionCountResult("OK", count=int(match.group(1)), detail="OK")


INSN_COUNTER = QemuInstructionCounter()


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


def _resolve_optional_binary(env_name: str) -> Path | None:
    env_bin = os.environ.get(env_name, "").strip()
    if not env_bin:
        return None
    candidate = Path(env_bin)
    resolved = candidate if candidate.is_absolute() else (WORKSPACE / candidate)
    if not resolved.exists():
        raise FileNotFoundError(f"{env_name} was set but file does not exist: {resolved}")
    return resolved


def _normalize_text(text: str) -> str:
    return text.replace("\r\n", "\n").strip()


def _collect_cases() -> list[Path]:
    env_dirs = os.environ.get("PERF_TEST_DIRS", "").strip()
    if env_dirs:
        roots = [WORKSPACE / item.strip() for item in env_dirs.split(",") if item.strip()]
    else:
        preferred = ["test/performance"]
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
                f"-DCMAKE_C_FLAGS={' '.join(RISCV_TARGET_FLAGS)}",
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
            *RISCV_TARGET_FLAGS,
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
    HY_COMPILER_BIN = _resolve_optional_binary("HY_COMPILER_BIN")
    RUNTIME_LIB = _ensure_runtime_lib()
    RUNTIME_WRAPPER = _ensure_runtime_wrapper()
except Exception as exc:
    print(f"❌ [ERROR] {exc}")
    sys.exit(2)


def _compile_gcc(src: Path, out_dir: Path) -> tuple[Path, str]:
    baseline_src = _prepare_baseline_source(src, out_dir)
    asm = out_dir / f"{src.stem}.gcc.s"
    obj = out_dir / f"{src.stem}.gcc.o"
    exe = out_dir / f"{src.stem}.gcc.riscv"
    compile_cmd = [
        GCC_BIN,
        "-O3",
        *RISCV_TARGET_FLAGS,
        "-std=gnu++17",
        "-include",
        str(RUNTIME_WRAPPER),
        "-x",
        "c++",
        str(baseline_src),
    ]
    subprocess.run([*compile_cmd, "-S", "-o", str(asm)], check=True, capture_output=True, text=True)
    subprocess.run([*compile_cmd, "-c", "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", *RISCV_TARGET_FLAGS, str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _compile_clang(src: Path, out_dir: Path) -> tuple[Path, str]:
    baseline_src = _prepare_baseline_source(src, out_dir)
    asm = out_dir / f"{src.stem}.clang.s"
    obj = out_dir / f"{src.stem}.clang.o"
    exe = out_dir / f"{src.stem}.clang.riscv"
    compile_cmd = [
        CLANG_BIN,
        "-O3",
        *RISCV_TARGET_FLAGS,
        "-std=gnu++17",
        "--target=riscv64-linux-gnu",
        "--sysroot=/usr/riscv64-linux-gnu",
        "-include",
        str(RUNTIME_WRAPPER),
        "-x",
        "c++",
        str(baseline_src),
    ]
    subprocess.run([*compile_cmd, "-S", "-o", str(asm)], check=True, capture_output=True, text=True)
    subprocess.run([*compile_cmd, "-c", "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", *RISCV_TARGET_FLAGS, str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
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
    subprocess.run([GCC_BIN, *RISCV_TARGET_FLAGS, "-c", str(asm_file), "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", *RISCV_TARGET_FLAGS, str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _compile_hy(src: Path, out_dir: Path) -> tuple[Path, str]:
    if HY_COMPILER_BIN is None:
        raise RuntimeError("HY_COMPILER_BIN is not configured")
    asm_file = out_dir / f"{src.stem}.hy.s"
    obj = out_dir / f"{src.stem}.hy.o"
    exe = out_dir / f"{src.stem}.hy.riscv"
    subprocess.run(
        [str(HY_COMPILER_BIN), str(src), "-O1", "-o", str(asm_file)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([GCC_BIN, *RISCV_TARGET_FLAGS, "-c", str(asm_file), "-o", str(obj)], check=True, capture_output=True, text=True)
    subprocess.run([GCC_BIN, "-static", *RISCV_TARGET_FLAGS, str(obj), str(RUNTIME_LIB), "-o", str(exe)], check=True, capture_output=True, text=True)
    return exe, "OK"


def _collect_cost_model_summary(src: Path) -> dict[str, object]:
    summary: dict[str, object] = {
        "status": "NOT_RUN",
        "total_decisions": 0,
        "accepted": 0,
        "bypassed": 0,
        "rejected": 0,
        "accepted_estimated_gain": 0,
        "accepted_risk_penalty": 0,
        "accepted_final_score": 0,
        "bypassed_estimated_gain": 0,
        "bypassed_risk_penalty": 0,
        "bypassed_final_score": 0,
        "rejected_estimated_gain": 0,
        "rejected_risk_penalty": 0,
        "by_transform": {},
        "by_transform_action": {},
        "by_transform_reject_reason": {},
        "by_pass_transform_action": {},
        "by_reject_reason": {},
        "by_proof_status": {},
    }
    try:
        proc = subprocess.run(
            [str(COMPILER_BIN), str(src), "--emit-cost-model=json", "-O1"],
            check=True,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SEC,
        )
        payload = json.loads(proc.stdout)
    except Exception as exc:
        summary["status"] = f"FAILED: {exc}"
        return summary

    decisions = payload.get("decisions", [])
    if not isinstance(decisions, list):
        summary["status"] = "EMPTY"
        return summary

    by_transform: dict[str, int] = {}
    by_transform_action: dict[str, int] = {}
    by_transform_reject_reason: dict[str, int] = {}
    by_pass_transform_action: dict[str, int] = {}
    by_reject_reason: dict[str, int] = {}
    by_proof_status: dict[str, int] = {}
    accepted = 0
    bypassed = 0
    rejected = 0
    for decision in decisions:
        if not isinstance(decision, dict):
            continue
        transform = str(decision.get("transform", "Unknown"))
        by_transform[transform] = by_transform.get(transform, 0) + 1
        pass_name = str(decision.get("pass", "Unknown"))
        action = str(decision.get("action", ""))
        transform_action = f"{transform}/{action}"
        by_transform_action[transform_action] = by_transform_action.get(transform_action, 0) + 1
        pass_transform_action = f"{pass_name}/{transform}/{action}"
        by_pass_transform_action[pass_transform_action] = (
            by_pass_transform_action.get(pass_transform_action, 0) + 1
        )
        if action == "Accept":
            accepted += 1
            summary["accepted_estimated_gain"] = int(summary["accepted_estimated_gain"]) + int(
                decision.get("estimated_gain", 0)
            )
            summary["accepted_risk_penalty"] = int(summary["accepted_risk_penalty"]) + int(
                decision.get("risk_penalty", 0)
            )
            summary["accepted_final_score"] = int(summary["accepted_final_score"]) + int(
                decision.get("final_score", 0)
            )
        elif action == "BypassProfitability":
            bypassed += 1
            summary["bypassed_estimated_gain"] = int(summary["bypassed_estimated_gain"]) + int(
                decision.get("estimated_gain", 0)
            )
            summary["bypassed_risk_penalty"] = int(summary["bypassed_risk_penalty"]) + int(
                decision.get("risk_penalty", 0)
            )
            summary["bypassed_final_score"] = int(summary["bypassed_final_score"]) + int(
                decision.get("final_score", 0)
            )
        elif action == "Reject":
            rejected += 1
            summary["rejected_estimated_gain"] = int(summary["rejected_estimated_gain"]) + int(
                decision.get("estimated_gain", 0)
            )
            summary["rejected_risk_penalty"] = int(summary["rejected_risk_penalty"]) + int(
                decision.get("risk_penalty", 0)
            )
            reason = str(decision.get("reject_reason", "Unknown"))
            by_reject_reason[reason] = by_reject_reason.get(reason, 0) + 1
            transform_reject_reason = f"{transform}/{reason}"
            by_transform_reject_reason[transform_reject_reason] = (
                by_transform_reject_reason.get(transform_reject_reason, 0) + 1
            )
        proof = decision.get("proof")
        if isinstance(proof, dict):
            status = str(proof.get("status", "Unknown"))
            by_proof_status[status] = by_proof_status.get(status, 0) + 1

    summary["status"] = "OK"
    summary["total_decisions"] = accepted + bypassed + rejected
    summary["accepted"] = accepted
    summary["bypassed"] = bypassed
    summary["rejected"] = rejected
    summary["by_transform"] = by_transform
    summary["by_transform_action"] = by_transform_action
    summary["by_transform_reject_reason"] = by_transform_reject_reason
    summary["by_pass_transform_action"] = by_pass_transform_action
    summary["by_reject_reason"] = by_reject_reason
    summary["by_proof_status"] = by_proof_status
    return summary


def _collect_codegen_metrics(src: Path, out_dir: Path) -> dict[str, object]:
    asm_file = out_dir / f"{src.stem}.compiler.s"
    metrics: dict[str, object] = {
        "mir_instrs": 0,
        "stack_slots": 0,
        "load_slot": 0,
        "store_slot": 0,
        "move": 0,
        "fmove": 0,
        "jumps": 0,
        "branches": 0,
        "loads": 0,
        "stores": 0,
        "spills": 0,
        "asm_lines": 0,
        "mir_stage_metrics_status": "NOT_RUN",
        "mir_stages": {},
        "cost_model_summary": _collect_cost_model_summary(src),
    }

    if asm_file.exists():
        metrics["asm_lines"] = sum(
            1
            for line in asm_file.read_text(errors="replace").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        )

    try:
        proc = subprocess.run(
            [str(COMPILER_BIN), str(src), "--emit-mir-metrics", "-O1"],
            check=True,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SEC,
        )
        payload = json.loads(proc.stdout)
    except Exception as exc:
        metrics["mir_stage_metrics_status"] = f"FAILED: {exc}"
        return metrics

    stages: dict[str, dict[str, int]] = {}
    for raw_stage in payload.get("mir_stage_metrics", []):
        if not isinstance(raw_stage, dict):
            continue
        name = raw_stage.get("stage")
        if not isinstance(name, str) or not name:
            continue
        stage_metrics: dict[str, int] = {}
        for key, value in raw_stage.items():
            if key == "stage":
                continue
            if isinstance(value, int):
                stage_metrics[key] = value
        stages[name] = stage_metrics

    metrics["mir_stages"] = stages
    metrics["mir_stage_metrics_status"] = "OK" if stages else "EMPTY"

    final_metrics = stages.get("final")
    if final_metrics is None and stages:
        final_metrics = next(reversed(stages.values()))
    if final_metrics is not None:
        metrics["mir_instrs"] = final_metrics.get("instructions", 0)
        metrics["stack_slots"] = final_metrics.get("stack_slots", 0)
        metrics["load_slot"] = final_metrics.get("load_slots", 0)
        metrics["store_slot"] = final_metrics.get("store_slots", 0)
        metrics["move"] = final_metrics.get("moves", 0)
        metrics["jumps"] = final_metrics.get("jumps", 0)
        metrics["branches"] = final_metrics.get("branches", 0)
        metrics["loads"] = final_metrics.get("loads", 0)
        metrics["stores"] = final_metrics.get("stores", 0)
        metrics["spills"] = final_metrics.get("spills", 0)
    return metrics


def _run_qemu(exe: Path, input_file: Optional[Path]) -> tuple[bool, float, str, str, Optional[int], str]:
    stdin_data = input_file.read_bytes() if input_file and input_file.exists() else None
    times = []
    last_result = None

    for _ in range(3):
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
            times.append(elapsed)
            last_result = result
            if result.returncode != 0:
                break
        except subprocess.TimeoutExpired:
            elapsed = time.perf_counter() - start
            return False, elapsed, "", "", None, "TIMEOUT"
        except Exception as exc:  # pragma: no cover
            elapsed = time.perf_counter() - start
            return False, elapsed, "", "", None, f"ERR: {exc}"

    times.sort()
    median_elapsed = times[len(times) // 2]

    stdout = last_result.stdout.decode(errors="replace") if last_result.stdout else ""
    stderr = last_result.stderr.decode(errors="replace") if last_result.stderr else ""
    ok = last_result.returncode == 0
    return ok, median_elapsed, stdout, stderr, last_result.returncode, "OK" if ok else f"exit={last_result.returncode}"


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


def _compile_and_run(src: Path) -> tuple[RunResult, RunResult, RunResult, RunResult | None, bool, str]:
    input_file = _expected_input(src)
    expected = _expected_output(src)
    case_dir = _case_work_dir(src)

    results: dict[str, RunResult] = {}

    compiler_specs = (
        ("gcc", _compile_gcc),
        ("clang++", _compile_clang),
        ("compiler", _compile_compiler),
    )
    if HY_COMPILER_BIN is not None:
        compiler_specs = (*compiler_specs, ("hy", _compile_hy))

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
        results[name] = RunResult(True, run_ok, elapsed, stdout, stderr, exit_code, detail, exe_path=exe)

    if "compiler" in results and results["compiler"].compile_ok:
        results["compiler"].metrics = _collect_codegen_metrics(src, case_dir)

    for result in results.values():
        if result.compile_ok and result.run_ok:
            insn_result = INSN_COUNTER.count(result.exe_path or Path(), input_file)
            result.instruction_count = insn_result.count
            result.instruction_count_status = insn_result.status
            result.instruction_count_detail = insn_result.detail

    gcc = results["gcc"]
    clang = results["clang++"]
    compiler = results["compiler"]
    hy = results.get("hy")

    if not gcc.compile_ok or not gcc.run_ok:
        return gcc, clang, compiler, hy, False, f"gcc {gcc.detail}"
    if not clang.compile_ok or not clang.run_ok:
        return gcc, clang, compiler, hy, False, f"clang++ {clang.detail}"
    if not compiler.compile_ok or not compiler.run_ok:
        return gcc, clang, compiler, hy, False, f"compiler {compiler.detail}"
    if hy is not None and (not hy.compile_ok or not hy.run_ok):
        return gcc, clang, compiler, hy, True, "OK"

    clang_stdout = _normalize_text(clang.stdout)
    compiler_stdout = _normalize_text(compiler.stdout)

    if expected is not None:
        expected_stdout, expected_exit = expected
        if compiler_stdout != _normalize_text(expected_stdout) or compiler.exit_code != expected_exit:
            return (
                gcc,
                clang,
                compiler,
                hy,
                False,
                f"compiler output mismatch vs expected .out for {src.name}",
            )
        if hy is not None and (_normalize_text(hy.stdout) != _normalize_text(expected_stdout) or hy.exit_code != expected_exit):
            return (
                gcc,
                clang,
                compiler,
                hy,
                True,
                "OK",
            )
        return gcc, clang, compiler, hy, True, "OK"

    baseline_stdout = _normalize_text(gcc.stdout)
    baseline_exit = gcc.exit_code

    if clang_stdout != baseline_stdout or clang.exit_code != baseline_exit:
        return gcc, clang, compiler, hy, False, f"clang++ output mismatch vs gcc for {src.name}"
    if compiler_stdout != baseline_stdout or compiler.exit_code != baseline_exit:
        return gcc, clang, compiler, hy, False, f"compiler output mismatch vs gcc for {src.name}"
    if hy is not None and (_normalize_text(hy.stdout) != baseline_stdout or hy.exit_code != baseline_exit):
        return gcc, clang, compiler, hy, True, "OK"

    return gcc, clang, compiler, hy, True, "OK"


def _print_header() -> None:
    print("=== RISC-V/QEMU perf compare ===")
    print(f"Workspace: {WORKSPACE}")
    print(f"Compiler binary: {COMPILER_BIN}")
    if HY_COMPILER_BIN is not None:
        print(f"HY compiler binary: {HY_COMPILER_BIN}")
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


def _parse_time_cell(value: object) -> Optional[float]:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)s", str(value).strip())
    return float(match.group(1)) if match else None


def _geomean(values: list[float]) -> Optional[float]:
    positive_values = [value for value in values if value > 0.0]
    if not positive_values:
        return None
    return math.exp(sum(math.log(value) for value in positive_values) / len(positive_values))


def _format_ratio(value: Optional[float]) -> str:
    return "N/A" if value is None else f"{value:.2f}x"


def _compiler_vs_o3_stats(rows: list[dict]) -> dict[str, Optional[float] | int]:
    gcc_ratios: list[float] = []
    clang_ratios: list[float] = []
    gcc_faster_cases = 0
    clang_faster_cases = 0

    for row in rows:
        compiler = _parse_time_cell(row.get("compiler"))
        gcc = _parse_time_cell(row.get("gcc"))
        clang = _parse_time_cell(row.get("clang"))
        if compiler is None:
            continue
        if gcc is not None and gcc > 0.0:
            gcc_ratios.append(gcc / compiler)
            if compiler < gcc:
                gcc_faster_cases += 1
        if clang is not None and clang > 0.0:
            clang_ratios.append(clang / compiler)
            if compiler < clang:
                clang_faster_cases += 1

    return {
        "gcc_o3_geomean": _geomean(gcc_ratios),
        "clang_o3_geomean": _geomean(clang_ratios),
        "gcc_o3_faster_cases": gcc_faster_cases,
        "clang_o3_faster_cases": clang_faster_cases,
    }


def _hy_vs_compiler_stats(rows: list[dict]) -> dict[str, Optional[float] | int]:
    ratios: list[float] = []
    hy_faster_cases = 0

    for row in rows:
        hy = _parse_time_cell(row.get("hy"))
        compiler = _parse_time_cell(row.get("compiler"))
        if hy is None or compiler is None or hy <= 0.0:
            continue
        ratios.append(compiler / hy)
        if hy < compiler:
            hy_faster_cases += 1

    return {
        "hy_vs_compiler_geomean": _geomean(ratios),
        "hy_faster_cases": hy_faster_cases,
    }


def _instruction_count_summary(rows: list[dict]) -> dict[str, object]:
    counts = [
        row.get("instruction_counts", {}).get("compiler")
        for row in rows
        if isinstance(row.get("instruction_counts", {}).get("compiler"), int)
    ]
    statuses = [str(row.get("instruction_count_status", "DISABLED")) for row in rows]
    failed = sum(1 for status in statuses if status == "FAILED")
    skipped = sum(1 for status in statuses if status == "SKIPPED")
    disabled = sum(1 for status in statuses if status == "DISABLED")
    if counts:
        status = "OK" if failed == 0 else "FAILED"
        reason = ""
    elif ENABLE_QEMU_INSN_COUNT:
        status = "FAILED" if failed else "SKIPPED"
        reason = INSN_COUNTER.reason
    else:
        status = "DISABLED"
        reason = "ENABLE_QEMU_INSN_COUNT is not enabled"
    return {
        "enabled": ENABLE_QEMU_INSN_COUNT,
        "strict": QEMU_INSN_STRICT,
        "status": status,
        "reason": reason,
        "qemu": QEMU_RISCV64,
        "plugin": str(INSN_COUNTER.plugin),
        "total_instructions": sum(counts) if counts else None,
        "counted_cases": len(counts),
        "failed_cases": failed,
        "skipped_cases": skipped,
        "disabled_cases": disabled,
    }


MIR_STAGE_ORDER = ["lowered", "post-combine", "pre-ra", "post-ra", "final"]
MIR_STAGE_METRIC_KEYS = [
    "instructions",
    "moves",
    "branches",
    "jumps",
    "loads",
    "stores",
    "spills",
    "stack_slots",
]


def _mir_stage_metric_summary(rows: list[dict]) -> dict[str, object]:
    totals: dict[str, dict[str, int]] = {}
    counted_cases = 0
    failed_cases = 0

    for row in rows:
        codegen_metrics = row.get("codegen_metrics")
        if not isinstance(codegen_metrics, dict):
            continue
        status = str(codegen_metrics.get("mir_stage_metrics_status", "NOT_RUN"))
        stages = codegen_metrics.get("mir_stages")
        if not isinstance(stages, dict) or not stages:
            if status.startswith("FAILED"):
                failed_cases += 1
            continue
        counted_cases += 1
        for stage_name, raw_metrics in stages.items():
            if not isinstance(stage_name, str) or not isinstance(raw_metrics, dict):
                continue
            stage_totals = totals.setdefault(stage_name, {key: 0 for key in MIR_STAGE_METRIC_KEYS})
            for key in MIR_STAGE_METRIC_KEYS:
                value = raw_metrics.get(key)
                if isinstance(value, int):
                    stage_totals[key] += value

    if counted_cases:
        status = "OK" if failed_cases == 0 else "PARTIAL"
    elif failed_cases:
        status = "FAILED"
    else:
        status = "EMPTY"

    ordered_totals = {
        stage: totals[stage]
        for stage in MIR_STAGE_ORDER
        if stage in totals
    }
    for stage in sorted(totals):
        if stage not in ordered_totals:
            ordered_totals[stage] = totals[stage]

    deltas: dict[str, dict[str, int]] = {}
    previous_stage: str | None = None
    for stage_name in ordered_totals:
        if previous_stage is not None:
            deltas[f"{previous_stage}->{stage_name}"] = {
                key: ordered_totals[stage_name].get(key, 0)
                - ordered_totals[previous_stage].get(key, 0)
                for key in MIR_STAGE_METRIC_KEYS
            }
        previous_stage = stage_name

    return {
        "status": status,
        "counted_cases": counted_cases,
        "failed_cases": failed_cases,
        "stages": ordered_totals,
        "deltas": deltas,
    }


def _cost_model_decision_summary(rows: list[dict]) -> dict[str, object]:
    summary: dict[str, object] = {
        "status": "NOT_RUN",
        "total_decisions": 0,
        "accepted": 0,
        "bypassed": 0,
        "rejected": 0,
        "accepted_estimated_gain": 0,
        "accepted_risk_penalty": 0,
        "accepted_final_score": 0,
        "bypassed_estimated_gain": 0,
        "bypassed_risk_penalty": 0,
        "bypassed_final_score": 0,
        "rejected_estimated_gain": 0,
        "rejected_risk_penalty": 0,
        "by_transform": {},
        "by_transform_action": {},
        "by_transform_reject_reason": {},
        "by_pass_transform_action": {},
        "by_reject_reason": {},
        "by_proof_status": {},
    }
    by_transform: dict[str, int] = {}
    by_transform_action: dict[str, int] = {}
    by_transform_reject_reason: dict[str, int] = {}
    by_pass_transform_action: dict[str, int] = {}
    by_reject_reason: dict[str, int] = {}
    by_proof_status: dict[str, int] = {}
    statuses: list[str] = []

    for row in rows:
        codegen_metrics = row.get("codegen_metrics")
        if not isinstance(codegen_metrics, dict):
            continue
        cost_summary = codegen_metrics.get("cost_model_summary")
        if not isinstance(cost_summary, dict):
            continue
        status = str(cost_summary.get("status", "NOT_RUN"))
        statuses.append(status)
        if status != "OK":
            continue
        summary["total_decisions"] = int(summary["total_decisions"]) + int(
            cost_summary.get("total_decisions", 0)
        )
        summary["accepted"] = int(summary["accepted"]) + int(cost_summary.get("accepted", 0))
        summary["bypassed"] = int(summary["bypassed"]) + int(cost_summary.get("bypassed", 0))
        summary["rejected"] = int(summary["rejected"]) + int(cost_summary.get("rejected", 0))
        for total_key in (
            "accepted_estimated_gain",
            "accepted_risk_penalty",
            "accepted_final_score",
            "bypassed_estimated_gain",
            "bypassed_risk_penalty",
            "bypassed_final_score",
            "rejected_estimated_gain",
            "rejected_risk_penalty",
        ):
            summary[total_key] = int(summary[total_key]) + int(cost_summary.get(total_key, 0))
        for key, value in (cost_summary.get("by_transform") or {}).items():
            by_transform[str(key)] = by_transform.get(str(key), 0) + int(value)
        for key, value in (cost_summary.get("by_transform_action") or {}).items():
            by_transform_action[str(key)] = by_transform_action.get(str(key), 0) + int(value)
        for key, value in (cost_summary.get("by_transform_reject_reason") or {}).items():
            by_transform_reject_reason[str(key)] = (
                by_transform_reject_reason.get(str(key), 0) + int(value)
            )
        for key, value in (cost_summary.get("by_pass_transform_action") or {}).items():
            by_pass_transform_action[str(key)] = (
                by_pass_transform_action.get(str(key), 0) + int(value)
            )
        for key, value in (cost_summary.get("by_reject_reason") or {}).items():
            by_reject_reason[str(key)] = by_reject_reason.get(str(key), 0) + int(value)
        for key, value in (cost_summary.get("by_proof_status") or {}).items():
            by_proof_status[str(key)] = by_proof_status.get(str(key), 0) + int(value)

    if statuses:
        failures = [status for status in statuses if status != "OK"]
        summary["status"] = "OK" if not failures else "; ".join(sorted(set(failures)))
    summary["by_transform"] = by_transform
    summary["by_transform_action"] = by_transform_action
    summary["by_transform_reject_reason"] = by_transform_reject_reason
    summary["by_pass_transform_action"] = by_pass_transform_action
    summary["by_reject_reason"] = by_reject_reason
    summary["by_proof_status"] = by_proof_status
    return summary


def _slow_compiler_rows(rows: list[dict], limit: int = 10) -> list[dict]:
    timed_rows = [
        (compiler_time, row)
        for row in rows
        if (compiler_time := _parse_time_cell(row.get("compiler"))) is not None
    ]
    timed_rows.sort(key=lambda item: item[0], reverse=True)
    return [row for _, row in timed_rows[:limit]]


def _write_reports(rows: list[dict], failures: int, total_runtime: float, compiler_total_runtime: float) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)

    status = "PASS" if failures == 0 else "FAIL"
    generated = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    o3_stats = _compiler_vs_o3_stats(rows)
    hy_stats = _hy_vs_compiler_stats(rows) if HY_COMPILER_BIN is not None else None
    insn_summary = _instruction_count_summary(rows)
    mir_stage_summary = _mir_stage_metric_summary(rows)
    cost_model_summary = _cost_model_decision_summary(rows)

    md_lines = [
        "# 📊 RISC-V QEMU Perf Report",
        "",
        f"- Status: {STATUS_EMOJI[status]} {status}",
        f"- Generated: {generated}",
        f"- Cases: {len(rows)}",
        f"- Failed: {failures}",
        "- Compiler opt: `-O1`",
        f"- Total runtime (s): {total_runtime:.4f}",
        f"- 🧮 Geomean speedup: GCC {_format_ratio(o3_stats['gcc_o3_geomean'])} / Clang++ {_format_ratio(o3_stats['clang_o3_geomean'])}",
        f"- 🏁 Faster cases: GCC {o3_stats['gcc_o3_faster_cases']} / Clang++ {o3_stats['clang_o3_faster_cases']}",
        f"- QEMU dynamic instruction count: {insn_summary['status']}",
        f"- MIR stage metrics: {mir_stage_summary['status']} ({mir_stage_summary['counted_cases']} cases)",
        f"- Cost model decisions: {cost_model_summary['status']} ({cost_model_summary['accepted']} accepted / {cost_model_summary['bypassed']} bypassed / {cost_model_summary['rejected']} rejected)",
        f"- Compiler binary: {COMPILER_BIN}",
        f"- Runtime lib: {RUNTIME_LIB}",
    ]
    if HY_COMPILER_BIN is not None:
        md_lines.extend(
            [
                f"- HY compiler binary: {HY_COMPILER_BIN}",
                f"- HY vs yoolang geomean speedup: {_format_ratio(hy_stats['hy_vs_compiler_geomean'] if hy_stats else None)}",
                f"- HY faster than yoolang cases: {hy_stats['hy_faster_cases'] if hy_stats else 0}",
            ]
        )

    stage_totals = mir_stage_summary.get("stages")
    if isinstance(stage_totals, dict) and stage_totals:
        md_lines.extend(
            [
                "",
                "## MIR Stage Metrics",
                "",
                "| Stage | Instrs | Moves | Branches | Jumps | Loads | Stores | Spills | Stack slots |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for stage_name, totals in stage_totals.items():
            if not isinstance(totals, dict):
                continue
            md_lines.append(
                "| "
                + " | ".join(
                    [
                        _md_escape(str(stage_name)),
                        str(totals.get("instructions", 0)),
                        str(totals.get("moves", 0)),
                        str(totals.get("branches", 0)),
                        str(totals.get("jumps", 0)),
                        str(totals.get("loads", 0)),
                        str(totals.get("stores", 0)),
                        str(totals.get("spills", 0)),
                        str(totals.get("stack_slots", 0)),
                    ]
                )
                + " |"
            )

    stage_deltas = mir_stage_summary.get("deltas")
    if isinstance(stage_deltas, dict) and stage_deltas:
        md_lines.extend(
            [
                "",
                "## MIR Stage Metric Deltas",
                "",
                "| Transition | Instrs | Moves | Branches | Jumps | Loads | Stores | Spills | Stack slots |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for transition, totals in stage_deltas.items():
            if not isinstance(totals, dict):
                continue
            md_lines.append(
                "| "
                + " | ".join(
                    [
                        _md_escape(str(transition)),
                        str(totals.get("instructions", 0)),
                        str(totals.get("moves", 0)),
                        str(totals.get("branches", 0)),
                        str(totals.get("jumps", 0)),
                        str(totals.get("loads", 0)),
                        str(totals.get("stores", 0)),
                        str(totals.get("spills", 0)),
                        str(totals.get("stack_slots", 0)),
                    ]
                )
                + " |"
            )

    if cost_model_summary.get("status") != "NOT_RUN":
        md_lines.extend(
            [
                "",
                "## Cost Model Decisions",
                "",
                f"- Total: {cost_model_summary['total_decisions']}",
                f"- Accepted: {cost_model_summary['accepted']}",
                f"- Bypassed profitability: {cost_model_summary['bypassed']}",
                f"- Rejected: {cost_model_summary['rejected']}",
                f"- Accepted estimated gain total: {cost_model_summary['accepted_estimated_gain']}",
                f"- Accepted risk penalty total: {cost_model_summary['accepted_risk_penalty']}",
                f"- Accepted final score total: {cost_model_summary['accepted_final_score']}",
                f"- Bypassed estimated gain total: {cost_model_summary['bypassed_estimated_gain']}",
                f"- Bypassed risk penalty total: {cost_model_summary['bypassed_risk_penalty']}",
                f"- Bypassed final score total: {cost_model_summary['bypassed_final_score']}",
                f"- Rejected estimated gain total: {cost_model_summary['rejected_estimated_gain']}",
                f"- Rejected risk penalty total: {cost_model_summary['rejected_risk_penalty']}",
                "",
                "| Category | Name | Count |",
                "| --- | --- | ---: |",
            ]
        )
        for category, values in (
            ("transform", cost_model_summary.get("by_transform", {})),
            ("transform_action", cost_model_summary.get("by_transform_action", {})),
            (
                "transform_reject_reason",
                cost_model_summary.get("by_transform_reject_reason", {}),
            ),
            (
                "pass_transform_action",
                cost_model_summary.get("by_pass_transform_action", {}),
            ),
            ("reject_reason", cost_model_summary.get("by_reject_reason", {})),
            ("proof_status", cost_model_summary.get("by_proof_status", {})),
        ):
            if not isinstance(values, dict):
                continue
            for name, count in sorted(values.items()):
                md_lines.append(
                    "| "
                    + " | ".join(
                        [
                            _md_escape(category),
                            _md_escape(str(name)),
                            str(count),
                        ]
                    )
                    + " |"
                )

    slow_rows = _slow_compiler_rows(rows)
    if slow_rows:
        md_lines.extend(
            [
                "",
                "## Slow Compiler Cases",
                "",
                "| Case | Compiler | GCC | Clang++ | HY | Status |",
                "| --- | ---: | ---: | ---: | ---: | --- |",
            ]
        )
        for row in slow_rows:
            md_lines.append(
                "| "
                + " | ".join(
                    [
                        _md_escape(str(row["case"])),
                        _md_escape(str(row["compiler"])),
                        _md_escape(str(row["gcc"])),
                        _md_escape(str(row["clang"])),
                        _md_escape(str(row.get("hy", "N/A"))),
                        _md_escape(str(row["status"])),
                    ]
                )
                + " |"
            )

    md_lines.extend(
        [
            "",
            "## Case Results",
            "",
            "| Case | GCC | Clang++ | Compiler | HY | Status |",
            "| --- | --- | --- | --- | --- | --- |",
        ]
    )

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
                    _md_escape(str(row.get("hy", "N/A"))),
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
        "hy_compiler_binary": str(HY_COMPILER_BIN) if HY_COMPILER_BIN is not None else "",
        "compiler_opt": "-O1",
        "runtime_lib": str(RUNTIME_LIB),
        "test_dirs": [str(root.relative_to(WORKSPACE)) for root in TEST_ROOTS],
        "cases": len(rows),
        "failures": failures,
        "total_runtime_sec": total_runtime,
        "compiler_total_sec": compiler_total_runtime,
        "gcc_o3_geomean": o3_stats["gcc_o3_geomean"],
        "clang_o3_geomean": o3_stats["clang_o3_geomean"],
        "gcc_o3_faster_cases": o3_stats["gcc_o3_faster_cases"],
        "clang_o3_faster_cases": o3_stats["clang_o3_faster_cases"],
        "hy_vs_compiler_geomean": hy_stats["hy_vs_compiler_geomean"] if hy_stats else None,
        "hy_faster_cases": hy_stats["hy_faster_cases"] if hy_stats else 0,
        "instruction_count_summary": insn_summary,
        "mir_stage_metric_summary": mir_stage_summary,
        "cost_model_decision_summary": cost_model_summary,
        "rows": rows,
    }
    REPORT_JSON.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    hy_header = " | HY          " if HY_COMPILER_BIN is not None else ""
    print(f"{'Case':<42} | {'GCC':<12} | {'Clang++':<12} | {'Compiler':<12}{hy_header} | Status")
    print("-" * 140)


TEST_ROOTS = []
env_dirs = os.environ.get("PERF_TEST_DIRS", "").strip()
if env_dirs:
    TEST_ROOTS = [WORKSPACE / item.strip() for item in env_dirs.split(",") if item.strip()]
else:
    for rel in ("test/performance",):
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
    compiler_total_runtime = 0.0
    report_rows: list[dict] = []

    for case in CASES:
        gcc, clang, compiler, hy, ok, detail = _compile_and_run(case)
        total_runtime += gcc.elapsed_sec + clang.elapsed_sec + compiler.elapsed_sec
        if hy is not None:
            total_runtime += hy.elapsed_sec
        compiler_total_runtime += compiler.elapsed_sec
        rel = str(case.relative_to(WORKSPACE))
        error_detail = ""
        if not ok:
            error_detail = "\n\n".join(
                item
                for item in (
                    _format_detail("gcc", gcc),
                    _format_detail("clang++", clang),
                    _format_detail("compiler", compiler),
                    _format_detail("hy", hy) if hy is not None else "",
                )
                if item
            )
        hy_cell = f" | {_format_cell(hy):<12}" if hy is not None else ""
        print(
            f"{rel:<42} | {_format_cell(gcc):<12} | {_format_cell(clang):<12} | {_format_cell(compiler):<12}{hy_cell} | {STATUS_EMOJI.get(detail, 'ℹ️')} {detail}"
        )
        instruction_counts = {
            "gcc": gcc.instruction_count,
            "clang": clang.instruction_count,
            "compiler": compiler.instruction_count,
        }
        instruction_count_statuses = {
            "gcc": gcc.instruction_count_status,
            "clang": clang.instruction_count_status,
            "compiler": compiler.instruction_count_status,
        }
        instruction_count_details = {
            "gcc": gcc.instruction_count_detail,
            "clang": clang.instruction_count_detail,
            "compiler": compiler.instruction_count_detail,
        }
        if hy is not None:
            instruction_counts["hy"] = hy.instruction_count
            instruction_count_statuses["hy"] = hy.instruction_count_status
            instruction_count_details["hy"] = hy.instruction_count_detail
        row = {
            "case": rel,
            "gcc": _format_cell(gcc),
            "clang": _format_cell(clang),
            "compiler": _format_cell(compiler),
            "status": detail,
            "detail": error_detail,
            "codegen_metrics": compiler.metrics or {},
            "instruction_count": compiler.instruction_count,
            "instruction_count_status": compiler.instruction_count_status,
            "instruction_count_detail": compiler.instruction_count_detail,
            "instruction_counts": instruction_counts,
            "instruction_count_statuses": instruction_count_statuses,
            "instruction_count_details": instruction_count_details,
        }
        if hy is not None:
            row["hy"] = _format_cell(hy)
        report_rows.append(row)
        if not ok:
            failures += 1
            print(f"❌ [FAIL] {rel}: {detail}")
            if error_detail:
                print(error_detail)
        strict_statuses = {
            gcc.instruction_count_status,
            clang.instruction_count_status,
            compiler.instruction_count_status,
        }
        if hy is not None:
            strict_statuses.add(hy.instruction_count_status)
        if (
            QEMU_INSN_STRICT
            and "FAILED"
            in strict_statuses
        ):
            failures += 1
            print(f"❌ [FAIL] {rel}: qemu instruction count failed")

    print("-" * 140)
    print(f"📌 Summary: cases={len(CASES)} failed={failures} total_run_time={total_runtime:.4f}s")

    _write_reports(report_rows, failures, total_runtime, compiler_total_runtime)
    print(f"📄 Report markdown: {REPORT_MD}")
    print(f"🧾 Report json: {REPORT_JSON}")

    if failures > 0:
        print("❌ [ERROR] perf compare failed.")
        return 1
    print("✅ [OK] perf compare passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
