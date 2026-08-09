#!/usr/bin/env python3

"""Fail-closed RVV release performance gate for native RISC-V hardware.

This frontend deliberately does not have a QEMU execution mode.  QEMU timing is
useful for semantic diagnostics but is not release-performance evidence.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as _datetime
import errno
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:
    from perf_common import (
        normalize_output,
        positive_geomean,
        read_sysy_expected_output,
        relative_mad,
        strict_median,
    )
except ModuleNotFoundError:  # Support imports as scripts.rvv_hardware_perf.
    from scripts.perf_common import (
        normalize_output,
        positive_geomean,
        read_sysy_expected_output,
        relative_mad,
        strict_median,
    )


SCHEMA_VERSION = 3
REPORT_SCHEMA = "yoolang.rvv-hardware-performance.v3"
EXPECTED_VECTORIZABLE = "expected-vectorizable"
NEGATIVE_CONTROL = "negative-control"
CLASSIFICATIONS = {EXPECTED_VECTORIZABLE, NEGATIVE_CONTROL}
RELEASE_THRESHOLDS = {
    "unit_stride_geomean_min_speedup": 1.50,
    "vectorizable_corpus_geomean_min_speedup": 1.15,
    "hotspot_max_slowdown_fraction": 0.05,
    "negative_control_max_geomean_regression": 0.02,
    "compile_time_max_geomean_regression": 0.10,
}
RELEASE_PROTOCOL_MINIMUMS = {
    "minimum_warmups": 3,
    "minimum_repetitions": 11,
    "compile_warmups": 1,
    "compile_repetitions": 5,
}
RELEASE_MAX_RELATIVE_MAD = 0.05
PR_RISCV_V_GET_CONTROL = 70
PR_RISCV_V_VSTATE_CTRL_OFF = 1
PR_RISCV_V_VSTATE_CTRL_CUR_MASK = 0x3
RVV_INSTRUCTION_RE = re.compile(
    r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2,16}\s+)+"
    r"([A-Za-z][A-Za-z0-9_.]*)\s*(.*)$"
)
VSET_RE = re.compile(
    r"\be(8|16|32|64)\s*,\s*(mf8|mf4|mf2|m1|m2|m4|m8)\b",
    re.IGNORECASE,
)
WHOLE_REGISTER_STORE_RE = re.compile(r"^vs([1248])r\.v$", re.IGNORECASE)
WHOLE_REGISTER_RELOAD_RE = re.compile(
    r"^vl([1248])re(?:8|16|32|64)\.v$", re.IGNORECASE
)
MIR_FUNCTION_RE = re.compile(r"^func\s+@([^\s(]+)")
MIR_FRAME_OBJECT_RE = re.compile(
    r"^\s+fi#(\d+)\s+(\S+)\s+.*?\bsize=(vlenb(?:\*\d+(?:/8)?)?)\s+"
    r"align=\S+\s+offset=\S+\s+kind=(spill|callee-saved)\s*$"
)
MIR_WHOLE_SPILL_RE = re.compile(r"^\s+PseudoVSPILL_WHOLE\s+fi#(\d+),")
MIR_WHOLE_RELOAD_RE = re.compile(
    r"^\s+PseudoVRELOAD_WHOLE\s+.*?,\s+fi#(\d+),"
)


class GateError(RuntimeError):
    """An infrastructure or evidence error with a stable machine code."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class Toolchain:
    mode: str
    perf: Path
    objdump: Path
    scalar_compiler: Path | None = None
    rvv_compiler: Path | None = None
    linker: Path | None = None
    runtime_lib: Path | None = None
    scalar_binary_dir: Path | None = None
    rvv_binary_dir: Path | None = None


def utc_now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace").replace("\x00", " ").strip()
    except OSError:
        return ""


def _read_int(path: Path) -> int | None:
    text = _read_text(path)
    try:
        return int(text) if text else None
    except ValueError:
        return None


def _path_inside(root: Path, raw: str, label: str) -> Path:
    if not isinstance(raw, str) or not raw:
        raise GateError("MANIFEST_PATH_INVALID", f"{label} must be a non-empty path")
    candidate = (root / raw).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise GateError(
            "MANIFEST_PATH_ESCAPE", f"{label} escapes the workspace: {raw}"
        ) from exc
    if not candidate.is_file():
        raise GateError("MANIFEST_PATH_MISSING", f"{label} does not exist: {candidate}")
    return candidate


def _float_field(mapping: Mapping[str, Any], key: str) -> float:
    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise GateError("MANIFEST_NUMBER_INVALID", f"{key} must be numeric")
    converted = float(value)
    if not math.isfinite(converted):
        raise GateError("MANIFEST_NUMBER_INVALID", f"{key} must be finite")
    return converted


def _int_field(mapping: Mapping[str, Any], key: str) -> int:
    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise GateError("MANIFEST_NUMBER_INVALID", f"{key} must be an integer")
    return value


def load_manifest(path: Path, workspace: Path) -> dict[str, Any]:
    """Parse and fully validate the release corpus manifest."""

    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError("MANIFEST_PARSE_FAILED", f"cannot parse {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise GateError("MANIFEST_ROOT_INVALID", "manifest root must be an object")
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise GateError(
            "MANIFEST_SCHEMA_UNSUPPORTED",
            f"schema_version must be {SCHEMA_VERSION}",
        )

    protocol = payload.get("protocol")
    thresholds = payload.get("thresholds")
    build = payload.get("build")
    kernels = payload.get("kernels")
    if not isinstance(protocol, dict):
        raise GateError("MANIFEST_PROTOCOL_MISSING", "protocol must be an object")
    if not isinstance(thresholds, dict):
        raise GateError("MANIFEST_THRESHOLDS_MISSING", "thresholds must be an object")
    if not isinstance(build, dict):
        raise GateError("MANIFEST_BUILD_MISSING", "build must be an object")
    if not isinstance(kernels, list) or not kernels:
        raise GateError("MANIFEST_KERNELS_MISSING", "kernels must be a non-empty array")

    if protocol.get("primary_metric") != "cycles":
        raise GateError("MANIFEST_PRIMARY_METRIC", "formal gate primary_metric must be cycles")
    for key in (
        "minimum_warmups",
        "minimum_repetitions",
        "default_warmups",
        "default_repetitions",
        "compile_warmups",
        "compile_repetitions",
    ):
        if _int_field(protocol, key) < 0:
            raise GateError("MANIFEST_PROTOCOL_INVALID", f"{key} cannot be negative")
    for key, release_minimum in RELEASE_PROTOCOL_MINIMUMS.items():
        if _int_field(protocol, key) < release_minimum:
            raise GateError(
                "MANIFEST_PROTOCOL_WEAKENED",
                f"{key} is below the release minimum {release_minimum}",
            )
    if _int_field(protocol, "default_warmups") < _int_field(protocol, "minimum_warmups"):
        raise GateError("MANIFEST_PROTOCOL_INVALID", "default_warmups is below its minimum")
    if _int_field(protocol, "default_repetitions") < _int_field(
        protocol, "minimum_repetitions"
    ):
        raise GateError(
            "MANIFEST_PROTOCOL_INVALID", "default_repetitions is below its minimum"
        )
    max_mad = _float_field(protocol, "max_relative_mad")
    if not 0.0 < max_mad <= RELEASE_MAX_RELATIVE_MAD:
        raise GateError(
            "MANIFEST_PROTOCOL_WEAKENED",
            f"max_relative_mad must not exceed {RELEASE_MAX_RELATIVE_MAD}",
        )

    for key, release_value in RELEASE_THRESHOLDS.items():
        configured = _float_field(thresholds, key)
        if "min_speedup" in key and configured < release_value:
            raise GateError(
                "MANIFEST_THRESHOLD_WEAKENED",
                f"{key}={configured} weakens the release minimum {release_value}",
            )
        if "max_" in key and configured > release_value:
            raise GateError(
                "MANIFEST_THRESHOLD_WEAKENED",
                f"{key}={configured} weakens the release maximum {release_value}",
            )

    for variant in ("scalar", "rvv"):
        variant_build = build.get(variant)
        if not isinstance(variant_build, dict):
            raise GateError("MANIFEST_BUILD_INVALID", f"build.{variant} must be an object")
        for key in ("compiler_args", "linker_args"):
            tokens = variant_build.get(key)
            if not isinstance(tokens, list) or not tokens or not all(
                isinstance(token, str) and token for token in tokens
            ):
                raise GateError(
                    "MANIFEST_BUILD_INVALID", f"build.{variant}.{key} must be string tokens"
                )
        if variant == "rvv":
            for key in ("vector_plan_args", "post_ra_mir_args"):
                tokens = variant_build.get(key)
                if not isinstance(tokens, list) or not tokens or not all(
                    isinstance(token, str) and token for token in tokens
                ):
                    raise GateError(
                        "MANIFEST_BUILD_INVALID",
                        f"build.rvv.{key} must be string tokens",
                    )

    seen: set[str] = set()
    counts = {EXPECTED_VECTORIZABLE: 0, NEGATIVE_CONTROL: 0, "unit-stride": 0}
    for index, kernel in enumerate(kernels):
        if not isinstance(kernel, dict):
            raise GateError("MANIFEST_KERNEL_INVALID", f"kernel {index} must be an object")
        kernel_id = kernel.get("id")
        if not isinstance(kernel_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", kernel_id):
            raise GateError("MANIFEST_KERNEL_ID", f"kernel {index} has an unsafe id")
        if kernel_id in seen:
            raise GateError("MANIFEST_KERNEL_DUPLICATE", f"duplicate kernel id: {kernel_id}")
        seen.add(kernel_id)
        classification = kernel.get("classification")
        if classification not in CLASSIFICATIONS:
            raise GateError(
                "MANIFEST_CLASSIFICATION",
                f"{kernel_id} classification must be one of {sorted(CLASSIFICATIONS)}",
            )
        counts[str(classification)] += 1
        tags = kernel.get("tags")
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            raise GateError("MANIFEST_TAGS", f"{kernel_id} tags must be strings")
        if "unit-stride" in tags:
            if classification != EXPECTED_VECTORIZABLE:
                raise GateError(
                    "MANIFEST_TAGS", f"{kernel_id} unit-stride must be expected-vectorizable"
                )
            counts["unit-stride"] += 1
        if not isinstance(kernel.get("hotspot"), bool):
            raise GateError("MANIFEST_HOTSPOT", f"{kernel_id} hotspot must be boolean")
        if classification == EXPECTED_VECTORIZABLE and not isinstance(
            kernel.get("dedupe_group"), str
        ):
            raise GateError(
                "MANIFEST_DEDUPE", f"{kernel_id} requires an explicit dedupe_group"
            )
        minimum_vectorized_loops = kernel.get("minimum_verified_vectorized_loops")
        if classification == EXPECTED_VECTORIZABLE:
            if (
                isinstance(minimum_vectorized_loops, bool)
                or not isinstance(minimum_vectorized_loops, int)
                or minimum_vectorized_loops < 1
            ):
                raise GateError(
                    "MANIFEST_VECTOR_PLAN_MINIMUM",
                    f"{kernel_id} minimum_verified_vectorized_loops must be an integer >= 1",
                )
        elif minimum_vectorized_loops is not None:
            raise GateError(
                "MANIFEST_VECTOR_PLAN_MINIMUM",
                f"{kernel_id} negative-control must not require vectorized loops",
            )
        for path_key in ("source", "input", "output"):
            _path_inside(workspace, kernel.get(path_key), f"{kernel_id}.{path_key}")
        binary_name = kernel.get("binary_name", kernel_id)
        if not isinstance(binary_name, str) or not re.fullmatch(
            r"[A-Za-z0-9_.-]+", binary_name
        ):
            raise GateError("MANIFEST_BINARY_NAME", f"{kernel_id} has an unsafe binary_name")

    if not all(counts.values()):
        raise GateError(
            "MANIFEST_CATEGORY_EMPTY",
            "manifest requires expected-vectorizable, unit-stride, and negative-control cases",
        )
    return payload


def _extract_isa(cpuinfo: str) -> list[str]:
    values: list[str] = []
    for line in cpuinfo.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        if key.strip().lower() in {"isa", "isa extensions"}:
            values.append(value.strip().lower())
    return values


def isa_has_vector(isa_values: Iterable[str]) -> bool:
    for value in isa_values:
        for token in re.split(r"[\s,]+", value.lower()):
            if token == "v":
                return True
            match = re.match(r"rv(?:32|64)([a-z]+)", token)
            if match and "v" in match.group(1):
                return True
    return False


def query_vector_control(machine: str) -> dict[str, Any]:
    if machine.lower() != "riscv64":
        return {"status": "not-applicable", "raw": None}
    libc = ctypes.CDLL(None, use_errno=True)
    ctypes.set_errno(0)
    result = int(libc.prctl(PR_RISCV_V_GET_CONTROL, 0, 0, 0, 0))
    if result < 0:
        error_number = ctypes.get_errno()
        return {
            "status": "unavailable",
            "raw": None,
            "errno": error_number,
            "detail": os.strerror(error_number),
        }
    current = result & PR_RISCV_V_VSTATE_CTRL_CUR_MASK
    names = {0: "default", 1: "off", 2: "on"}
    return {"status": names.get(current, "unknown"), "raw": result}


def read_frequency(cpu: int) -> dict[str, Any]:
    root = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
    current = _read_int(root / "scaling_cur_freq")
    if current is None:
        current = _read_int(root / "cpuinfo_cur_freq")
    return {
        "available": root.is_dir(),
        "governor": _read_text(root / "scaling_governor"),
        "driver": _read_text(root / "scaling_driver"),
        "scaling_min_khz": _read_int(root / "scaling_min_freq"),
        "scaling_max_khz": _read_int(root / "scaling_max_freq"),
        "current_khz": current,
    }


def collect_environment(cpu: int) -> dict[str, Any]:
    machine = platform.machine().lower()
    cpuinfo = _read_text(Path("/proc/cpuinfo"))
    model = _read_text(Path("/sys/firmware/devicetree/base/model"))
    dmi_product = _read_text(Path("/sys/class/dmi/id/product_name"))
    hypervisor = _read_text(Path("/sys/hypervisor/type"))
    evidence = "\n".join(
        (cpuinfo, model, dmi_product, hypervisor, platform.platform(), platform.version())
    ).lower()
    emulation_tokens = sorted(
        token for token in ("qemu", "tcg", "emulator") if token in evidence
    )
    try:
        affinity = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        affinity = []
    isa_values = _extract_isa(cpuinfo)
    return {
        "machine": machine,
        "platform": platform.platform(),
        "kernel": platform.release(),
        "cpu": cpu,
        "affinity": affinity,
        "cpu_model": model or dmi_product,
        "isa": isa_values,
        "v_extension": isa_has_vector(isa_values),
        "emulation_tokens": emulation_tokens,
        "hypervisor": hypervisor,
        "vector_control": query_vector_control(machine),
        "vector_execution_verified": False,
        "frequency_initial": read_frequency(cpu),
        "frequency_samples_khz": [],
    }


def environment_failures(
    environment: Mapping[str, Any],
    frequency_policy: str,
    *,
    require_vector_execution: bool,
) -> list[dict[str, str]]:
    """Pure, testable official-environment policy."""

    failures: list[dict[str, str]] = []

    def fail(code: str, detail: str) -> None:
        failures.append({"code": code, "detail": detail})

    if str(environment.get("machine", "")).lower() != "riscv64":
        fail("ENV_NOT_RISCV64", f"machine is {environment.get('machine')!r}, not riscv64")
    tokens = environment.get("emulation_tokens")
    if isinstance(tokens, list) and tokens:
        fail("ENV_EMULATED", f"emulation evidence found: {', '.join(map(str, tokens))}")
    if not environment.get("v_extension"):
        fail("ENV_NO_V_EXTENSION", "CPU ISA evidence does not advertise the V extension")
    vector_control = environment.get("vector_control")
    vector_status = (
        vector_control.get("status") if isinstance(vector_control, Mapping) else "unavailable"
    )
    if vector_status == "off":
        fail("ENV_VECTOR_STATE_OFF", "PR_RISCV_V_GET_CONTROL reports vector state off")
    elif vector_status not in {"on", "default"}:
        fail(
            "ENV_VECTOR_STATE_UNVERIFIED",
            f"cannot verify Linux vector state control (status={vector_status})",
        )
    cpu = environment.get("cpu")
    affinity = environment.get("affinity")
    if not isinstance(cpu, int) or not isinstance(affinity, list) or affinity != [cpu]:
        fail("ENV_AFFINITY", f"process affinity is {affinity!r}, expected exactly [{cpu!r}]")
    if require_vector_execution and not environment.get("vector_execution_verified"):
        fail(
            "ENV_VECTOR_EXECUTION_UNVERIFIED",
            "no successfully executed binary with a decoded RVV opcode was observed",
        )

    frequency = environment.get("frequency_initial")
    if frequency_policy == "record":
        fail(
            "ENV_FREQUENCY_NOT_FIXED",
            "frequency-policy=record is informational and cannot produce an official gate",
        )
    elif frequency_policy == "require-fixed":
        if not isinstance(frequency, Mapping) or not frequency.get("available"):
            fail("ENV_CPUFREQ_UNAVAILABLE", "cpufreq sysfs is unavailable for the pinned CPU")
        else:
            minimum = frequency.get("scaling_min_khz")
            maximum = frequency.get("scaling_max_khz")
            current = frequency.get("current_khz")
            if not isinstance(minimum, int) or not isinstance(maximum, int) or minimum <= 0:
                fail("ENV_CPUFREQ_UNVERIFIED", "fixed min/max frequency cannot be read")
            elif minimum != maximum:
                fail(
                    "ENV_CPUFREQ_NOT_FIXED",
                    f"scaling_min_freq={minimum} differs from scaling_max_freq={maximum}",
                )
            if not isinstance(current, int) or current <= 0:
                fail("ENV_CPUFREQ_UNVERIFIED", "current CPU frequency cannot be read")
            samples = environment.get("frequency_samples_khz", [])
            if isinstance(minimum, int) and minimum > 0 and isinstance(samples, list):
                bad = [
                    sample
                    for sample in samples
                    if not isinstance(sample, int) or abs(sample - minimum) / minimum > 0.01
                ]
                if bad:
                    fail(
                        "ENV_CPUFREQ_DRIFT",
                        f"observed frequency samples differ from fixed target {minimum} kHz",
                    )
    else:
        fail("ENV_FREQUENCY_POLICY", f"unknown frequency policy: {frequency_policy}")
    return failures


def preflight_blocking_failures(
    findings: Sequence[Mapping[str, str]], frequency_policy: str
) -> list[dict[str, str]]:
    """Allow record-mode measurements while preserving their non-official finding."""

    advisory_codes = {"ENV_FREQUENCY_NOT_FIXED"} if frequency_policy == "record" else set()
    return [dict(finding) for finding in findings if finding.get("code") not in advisory_codes]


def apply_cpu_affinity(cpu: int) -> None:
    if cpu < 0:
        raise GateError("CPU_AFFINITY_INVALID", "--cpu must be non-negative")
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise GateError("CPU_AFFINITY_UNAVAILABLE", "sched affinity APIs are unavailable")
    allowed = os.sched_getaffinity(0)
    if cpu not in allowed:
        raise GateError(
            "CPU_AFFINITY_DENIED", f"CPU {cpu} is outside the allowed set {sorted(allowed)}"
        )
    try:
        os.sched_setaffinity(0, {cpu})
    except OSError as exc:
        raise GateError("CPU_AFFINITY_FAILED", f"cannot pin to CPU {cpu}: {exc}") from exc
    if os.sched_getaffinity(0) != {cpu}:
        raise GateError("CPU_AFFINITY_FAILED", "affinity verification did not match requested CPU")


def parse_perf_stat(text: str) -> dict[str, float]:
    """Parse perf stat -x ';' output and reject missing hardware evidence."""

    values: dict[str, float] = {}
    wall: float | None = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(";")]
        if any(marker in line.lower() for marker in ("<not counted>", "<not supported>")):
            if "cycles" in line.lower() or "instructions" in line.lower():
                raise GateError("PERF_COUNTER_UNAVAILABLE", f"perf counter unavailable: {line}")
        numeric: float | None = None
        if fields:
            try:
                numeric = float(fields[0])
            except ValueError:
                numeric = None
        for field in fields[1:]:
            event = field.lower()
            if re.fullmatch(r"cycles(?::[ukhp]+)?", event) and numeric is not None:
                values["cycles"] = numeric
            elif re.fullmatch(r"instructions(?::[ukhp]+)?", event) and numeric is not None:
                values["instructions"] = numeric
        if "seconds time elapsed" in line.lower() and numeric is not None:
            wall = numeric
    for required in ("cycles", "instructions"):
        if (
            required not in values
            or not math.isfinite(values[required])
            or values[required] <= 0.0
        ):
            raise GateError("PERF_COUNTER_MISSING", f"perf stat did not report positive {required}")
    if wall is None or not math.isfinite(wall) or wall <= 0.0:
        raise GateError(
            "PERF_WALL_MISSING", "perf stat did not report positive seconds time elapsed"
        )
    values["wall_time_sec"] = wall
    values["ipc"] = values["instructions"] / values["cycles"]
    return values


def parse_objdump(
    disassembly: str, *, vlenb_bytes: int | None = None
) -> dict[str, Any]:
    if vlenb_bytes is not None and vlenb_bytes <= 0:
        raise GateError("VLENB_INVALID", "vlenb_bytes must be positive")
    histogram: dict[str, int] = {}
    vsetvl_count = 0
    load_count = 0
    store_count = 0
    mask_count = 0
    reduction_count = 0
    spill_like_count = 0
    vr_spill_store_sites = 0
    vr_spill_reload_sites = 0
    vr_spill_store_vlenb_units = 0
    vr_spill_reload_vlenb_units = 0
    sew_lmul: dict[str, int] = {}
    for line in disassembly.splitlines():
        match = RVV_INSTRUCTION_RE.match(line)
        if not match:
            continue
        mnemonic = match.group(1).lower()
        operands = match.group(2).lower()
        if not mnemonic.startswith("v"):
            continue
        histogram[mnemonic] = histogram.get(mnemonic, 0) + 1
        if mnemonic in {"vsetvl", "vsetvli", "vsetivli"}:
            vsetvl_count += 1
            setting = VSET_RE.search(operands)
            if setting:
                key = f"e{setting.group(1).lower()},{setting.group(2).lower()}"
                sew_lmul[key] = sew_lmul.get(key, 0) + 1
            continue
        if mnemonic.startswith("vl"):
            load_count += 1
        elif mnemonic.startswith("vs"):
            store_count += 1
        if re.match(r"v(?:m(?:s|and|or|xor|not|adc|sbc)|cpop|first)", mnemonic):
            mask_count += 1
        if re.match(r"v(?:f?w?red|fwred)", mnemonic):
            reduction_count += 1
        if (mnemonic.startswith("vl") or mnemonic.startswith("vs")) and re.search(
            r"\((?:sp|s0|fp)\)", operands
        ):
            spill_like_count += 1
        whole_store = WHOLE_REGISTER_STORE_RE.fullmatch(mnemonic)
        if whole_store is not None:
            vr_spill_store_sites += 1
            vr_spill_store_vlenb_units += int(whole_store.group(1))
        whole_reload = WHOLE_REGISTER_RELOAD_RE.fullmatch(mnemonic)
        if whole_reload is not None:
            vr_spill_reload_sites += 1
            vr_spill_reload_vlenb_units += int(whole_reload.group(1))
    rvv_count = sum(histogram.values())
    store_bytes = (
        None
        if vlenb_bytes is None
        else vr_spill_store_vlenb_units * vlenb_bytes
    )
    reload_bytes = (
        None
        if vlenb_bytes is None
        else vr_spill_reload_vlenb_units * vlenb_bytes
    )
    return {
        "rvv_opcode_count": rvv_count,
        "rvv_opcode_histogram": dict(sorted(histogram.items())),
        "vsetvl_count": vsetvl_count,
        "vector_load_count": load_count,
        "vector_store_count": store_count,
        "vector_alu_count": max(0, rvv_count - vsetvl_count - load_count - store_count),
        "mask_count": mask_count,
        "reduction_count": reduction_count,
        "spill_like_count": spill_like_count,
        "vlenb_bytes": vlenb_bytes,
        "vr_spill_store_sites": vr_spill_store_sites,
        "vr_spill_reload_sites": vr_spill_reload_sites,
        "vr_spill_store_vlenb_units": vr_spill_store_vlenb_units,
        "vr_spill_reload_vlenb_units": vr_spill_reload_vlenb_units,
        "vr_spill_store_bytes": store_bytes,
        "vr_spill_reload_bytes": reload_bytes,
        "vr_spill_transfer_bytes": (
            None
            if store_bytes is None or reload_bytes is None
            else store_bytes + reload_bytes
        ),
        "sew_lmul_distribution": dict(sorted(sew_lmul.items())),
    }


def parse_vlenb_output(output: str) -> int:
    text = output.strip()
    if not re.fullmatch(r"[0-9]+", text):
        raise GateError("VLENB_PROBE_INVALID", f"invalid vlenb probe output: {text!r}")
    value = int(text)
    if value < 16 or value > 8192 or value & (value - 1):
        raise GateError(
            "VLENB_PROBE_INVALID",
            f"vlenb must be a power of two in [16, 8192], got {value}",
        )
    return value


def parse_vectorization_plan(output: str) -> dict[str, Any]:
    try:
        document = json.loads(output)
    except json.JSONDecodeError as exc:
        raise GateError(
            "VECTOR_PLAN_PARSE_FAILED", f"compiler vector-plan output is not JSON: {exc}"
        ) from exc
    if not isinstance(document, dict) or not isinstance(
        document.get("vectorization_plans"), list
    ):
        raise GateError(
            "VECTOR_PLAN_SCHEMA_INVALID",
            "compiler vector-plan output lacks vectorization_plans",
        )
    plans: list[dict[str, Any]] = []
    code_counts: dict[str, int] = {}
    vectorizer_counts: dict[str, int] = {}
    for index, raw in enumerate(document["vectorization_plans"]):
        if not isinstance(raw, dict):
            raise GateError(
                "VECTOR_PLAN_SCHEMA_INVALID", f"vector plan {index} is not an object"
            )
        for key in ("vectorizer", "code", "function", "region", "explanation"):
            if not isinstance(raw.get(key), str) or not raw[key]:
                raise GateError(
                    "VECTOR_PLAN_SCHEMA_INVALID",
                    f"vector plan {index} has no non-empty {key}",
                )
        if not isinstance(raw.get("plan"), dict):
            raise GateError(
                "VECTOR_PLAN_SCHEMA_INVALID", f"vector plan {index} has no plan object"
            )
        plan = dict(raw)
        plans.append(plan)
        code = str(plan["code"])
        vectorizer = str(plan["vectorizer"])
        code_counts[code] = code_counts.get(code, 0) + 1
        vectorizer_counts[vectorizer] = vectorizer_counts.get(vectorizer, 0) + 1
    vectorized_count = code_counts.get("VECTORIZED", 0)
    loop_vectorized_count = sum(
        plan["code"] == "VECTORIZED" and plan["vectorizer"] == "loop"
        for plan in plans
    )
    return {
        "status": "OK",
        "plan_count": len(plans),
        "vectorized_count": vectorized_count,
        "loop_vectorized_count": loop_vectorized_count,
        "rejected_count": len(plans) - vectorized_count,
        "code_counts": dict(sorted(code_counts.items())),
        "vectorizer_counts": dict(sorted(vectorizer_counts.items())),
        "plans": plans,
    }


def parse_scalable_size_eighths(text: str) -> int:
    if text == "vlenb":
        return 8
    multiplied = re.fullmatch(r"vlenb\*([1-9][0-9]*)", text)
    if multiplied is not None:
        return int(multiplied.group(1)) * 8
    fractional = re.fullmatch(r"vlenb\*([1-9][0-9]*)/8", text)
    if fractional is not None:
        return int(fractional.group(1))
    raise GateError("MIR_SPILL_SCHEMA_INVALID", f"invalid scalable MIR size: {text}")


def parse_post_ra_mir_spills(mir_text: str, *, vlenb_bytes: int) -> dict[str, Any]:
    if vlenb_bytes <= 0 or vlenb_bytes % 8 != 0:
        raise GateError(
            "VLENB_INVALID", "post-RA spill byte accounting requires vlenb divisible by 8"
        )
    current_function: str | None = None
    slot_eighths: dict[tuple[str, int], int] = {}
    slot_kinds: dict[tuple[str, int], str] = {}
    spill_sites = 0
    reload_sites = 0
    referenced_slots: set[tuple[str, int]] = set()
    per_function: dict[str, dict[str, int]] = {}

    for line in mir_text.splitlines():
        function_match = MIR_FUNCTION_RE.match(line)
        if function_match is not None:
            current_function = function_match.group(1)
            per_function.setdefault(
                current_function,
                {
                    "spill_slot_count": 0,
                    "callee_saved_slot_count": 0,
                    "spill_slot_bytes": 0,
                    "callee_saved_slot_bytes": 0,
                    "spill_sites": 0,
                    "reload_sites": 0,
                },
            )
            continue
        if line == "}":
            current_function = None
            continue
        if current_function is None:
            continue
        slot_match = MIR_FRAME_OBJECT_RE.match(line)
        if slot_match is not None:
            key = (current_function, int(slot_match.group(1)))
            if key in slot_eighths:
                raise GateError(
                    "MIR_SPILL_SCHEMA_INVALID",
                    f"duplicate scalable frame slot @{current_function} fi#{key[1]}",
                )
            eighths = parse_scalable_size_eighths(slot_match.group(3))
            if eighths * vlenb_bytes % 8 != 0:
                raise GateError(
                    "MIR_SPILL_SCHEMA_INVALID",
                    f"non-integral scalable slot bytes @{current_function} fi#{key[1]}",
                )
            slot_eighths[key] = eighths
            kind = slot_match.group(4)
            slot_kinds[key] = kind
            byte_count = eighths * vlenb_bytes // 8
            summary = per_function[current_function]
            if kind == "spill":
                summary["spill_slot_count"] += 1
                summary["spill_slot_bytes"] += byte_count
            else:
                summary["callee_saved_slot_count"] += 1
                summary["callee_saved_slot_bytes"] += byte_count
            continue
        spill_match = MIR_WHOLE_SPILL_RE.match(line)
        if spill_match is not None:
            key = (current_function, int(spill_match.group(1)))
            referenced_slots.add(key)
            spill_sites += 1
            per_function[current_function]["spill_sites"] += 1
            continue
        reload_match = MIR_WHOLE_RELOAD_RE.match(line)
        if reload_match is not None:
            key = (current_function, int(reload_match.group(1)))
            referenced_slots.add(key)
            reload_sites += 1
            per_function[current_function]["reload_sites"] += 1

    missing = sorted(referenced_slots - set(slot_eighths))
    if missing:
        rendered = ", ".join(f"@{function} fi#{slot}" for function, slot in missing)
        raise GateError(
            "MIR_SPILL_SCHEMA_INVALID",
            f"whole-register pseudo references undeclared scalable slots: {rendered}",
        )
    spill_slot_bytes = sum(
        eighths * vlenb_bytes // 8
        for key, eighths in slot_eighths.items()
        if slot_kinds[key] == "spill"
    )
    callee_saved_slot_bytes = sum(
        eighths * vlenb_bytes // 8
        for key, eighths in slot_eighths.items()
        if slot_kinds[key] == "callee-saved"
    )
    return {
        "status": "OK",
        "vlenb_bytes": vlenb_bytes,
        "spill_slot_count": sum(kind == "spill" for kind in slot_kinds.values()),
        "callee_saved_slot_count": sum(
            kind == "callee-saved" for kind in slot_kinds.values()
        ),
        "spill_slot_bytes": spill_slot_bytes,
        "callee_saved_slot_bytes": callee_saved_slot_bytes,
        "whole_spill_sites": spill_sites,
        "whole_reload_sites": reload_sites,
        "functions": dict(sorted(per_function.items())),
    }


def parse_text_size(section_headers: str) -> int:
    for line in section_headers.splitlines():
        match = re.match(r"^\s*\d+\s+\.text\s+([0-9a-fA-F]+)\s+", line)
        if match:
            size = int(match.group(1), 16)
            if size > 0:
                return size
    raise GateError("CODE_SIZE_MISSING", "objdump -h did not report a positive .text size")


def _run_checked(
    command: Sequence[str], *, timeout: float, input_data: bytes | None = None
) -> subprocess.CompletedProcess[bytes]:
    try:
        result = subprocess.run(
            list(command),
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env={**os.environ, "LC_ALL": "C"},
        )
    except subprocess.TimeoutExpired as exc:
        raise GateError("COMMAND_TIMEOUT", f"command timed out: {command[0]}") from exc
    if result.returncode != 0:
        stderr = result.stderr.decode(errors="replace").strip()
        raise GateError(
            "COMMAND_FAILED",
            f"command failed ({result.returncode}): {' '.join(command)}"
            + (f"; stderr: {stderr[-1000:]}" if stderr else ""),
        )
    return result


def inspect_binary(
    binary: Path,
    objdump: Path,
    timeout: float,
    *,
    vlenb_bytes: int | None,
) -> dict[str, Any]:
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise GateError("BINARY_INVALID", f"benchmark binary is not executable: {binary}")
    disassembly = _run_checked([str(objdump), "-d", str(binary)], timeout=timeout)
    sections = _run_checked([str(objdump), "-h", str(binary)], timeout=timeout)
    metrics = parse_objdump(
        disassembly.stdout.decode(errors="replace"), vlenb_bytes=vlenb_bytes
    )
    metrics.update(
        {
            "path": str(binary),
            "sha256": sha256_file(binary),
            "file_size_bytes": binary.stat().st_size,
            "text_size_bytes": parse_text_size(sections.stdout.decode(errors="replace")),
        }
    )
    return metrics


class _StrictFormat(dict[str, str]):
    def __missing__(self, key: str) -> str:
        raise GateError("COMMAND_TEMPLATE_UNKNOWN", f"unknown command placeholder: {key}")


def expand_tokens(tokens: Sequence[str], values: Mapping[str, str]) -> list[str]:
    formatter = _StrictFormat(values)
    try:
        return [token.format_map(formatter) for token in tokens]
    except (ValueError, KeyError) as exc:
        raise GateError("COMMAND_TEMPLATE_INVALID", f"invalid command template: {exc}") from exc


def probe_vlenb(linker: Path, run_dir: Path, timeout: float) -> dict[str, Any]:
    source = run_dir / "probe-vlenb.c"
    binary = run_dir / "probe-vlenb.bin"
    source.write_text(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "  unsigned long value = 0;\n"
        "  __asm__ volatile (\"csrr %0, vlenb\" : \"=r\"(value));\n"
        "  return printf(\"%lu\\n\", value) < 0;\n"
        "}\n"
    )
    compiler_command = [
        str(linker),
        "-O2",
        "-static",
        "-march=rv64gcv",
        "-mabi=lp64d",
        str(source),
        "-o",
        str(binary),
    ]
    _run_checked(compiler_command, timeout=timeout)
    result = _run_checked([str(binary)], timeout=timeout)
    value = parse_vlenb_output(result.stdout.decode(errors="replace"))
    return {
        "vlenb_bytes": value,
        "vlen_bits": value * 8,
        "compiler_command": compiler_command,
        "binary_sha256": sha256_file(binary),
    }


def collect_vectorization_analysis(
    *,
    kernel: Mapping[str, Any],
    manifest: Mapping[str, Any],
    workspace: Path,
    run_dir: Path,
    compiler: Path,
    timeout: float,
) -> dict[str, Any]:
    kernel_id = str(kernel["id"])
    source = _path_inside(workspace, str(kernel["source"]), f"{kernel_id}.source")
    values = {
        "source": str(source),
        "workspace": str(workspace),
        "assembly": str(run_dir / f"{kernel_id}.rvv.analysis-unused.s"),
        "binary": str(run_dir / f"{kernel_id}.rvv.analysis-unused.bin"),
        "runtime_lib": "",
    }
    command = [
        str(compiler),
        *expand_tokens(manifest["build"]["rvv"]["vector_plan_args"], values),
    ]
    result = _run_checked(command, timeout=timeout)
    raw = result.stdout.decode(errors="replace")
    artifact = run_dir / f"{kernel_id}.rvv.vector-plan.json"
    artifact.write_text(raw)
    parsed = parse_vectorization_plan(raw)
    parsed.update(
        {
            "command": command,
            "artifact": str(artifact),
            "artifact_sha256": sha256_file(artifact),
        }
    )
    return parsed


def collect_post_ra_spill_analysis(
    *,
    kernel: Mapping[str, Any],
    manifest: Mapping[str, Any],
    workspace: Path,
    run_dir: Path,
    compiler: Path,
    timeout: float,
    vlenb_bytes: int,
) -> dict[str, Any]:
    kernel_id = str(kernel["id"])
    source = _path_inside(workspace, str(kernel["source"]), f"{kernel_id}.source")
    values = {
        "source": str(source),
        "workspace": str(workspace),
        "assembly": str(run_dir / f"{kernel_id}.rvv.analysis-unused.s"),
        "binary": str(run_dir / f"{kernel_id}.rvv.analysis-unused.bin"),
        "runtime_lib": "",
    }
    command = [
        str(compiler),
        *expand_tokens(manifest["build"]["rvv"]["post_ra_mir_args"], values),
    ]
    result = _run_checked(command, timeout=timeout)
    raw = result.stdout.decode(errors="replace")
    if not raw.strip():
        raise GateError("MIR_SPILL_EVIDENCE_MISSING", "post-RA MIR output is empty")
    artifact = run_dir / f"{kernel_id}.rvv.post-ra.mir"
    artifact.write_text(raw)
    parsed = parse_post_ra_mir_spills(raw, vlenb_bytes=vlenb_bytes)
    parsed.update(
        {
            "command": command,
            "artifact": str(artifact),
            "artifact_sha256": sha256_file(artifact),
        }
    )
    return parsed


def _measure_command(command: Sequence[str], timeout: float) -> float:
    start = time.perf_counter()
    _run_checked(command, timeout=timeout)
    return time.perf_counter() - start


def build_variant(
    *,
    variant: str,
    kernel: Mapping[str, Any],
    manifest: Mapping[str, Any],
    workspace: Path,
    run_dir: Path,
    toolchain: Toolchain,
    environment: dict[str, Any],
    compile_warmups: int,
    compile_repetitions: int,
    timeout: float,
) -> tuple[Path, dict[str, Any]]:
    kernel_id = str(kernel["id"])
    binary_name = str(kernel.get("binary_name", kernel_id))
    if toolchain.mode == "prebuilt":
        directory = (
            toolchain.scalar_binary_dir if variant == "scalar" else toolchain.rvv_binary_dir
        )
        assert directory is not None
        return directory / binary_name, {
            "status": "UNAVAILABLE_PREBUILT",
            "samples_sec": [],
            "median_sec": None,
            "link_time_sec": None,
        }

    compiler = toolchain.scalar_compiler if variant == "scalar" else toolchain.rvv_compiler
    assert compiler is not None and toolchain.linker is not None and toolchain.runtime_lib is not None
    source = _path_inside(workspace, str(kernel["source"]), f"{kernel_id}.source")
    assembly = run_dir / f"{kernel_id}.{variant}.s"
    binary = run_dir / f"{kernel_id}.{variant}.bin"
    values = {
        "source": str(source),
        "assembly": str(assembly),
        "binary": str(binary),
        "runtime_lib": str(toolchain.runtime_lib),
        "workspace": str(workspace),
    }
    variant_build = manifest["build"][variant]
    compiler_command = [
        str(compiler),
        *expand_tokens(variant_build["compiler_args"], values),
    ]
    for _ in range(compile_warmups):
        append_frequency_sample(environment, int(environment["cpu"]))
        _measure_command(compiler_command, timeout)
    samples: list[float] = []
    for _ in range(compile_repetitions):
        append_frequency_sample(environment, int(environment["cpu"]))
        samples.append(_measure_command(compiler_command, timeout))
    if not assembly.is_file():
        raise GateError("COMPILER_OUTPUT_MISSING", f"compiler did not create {assembly}")
    linker_command = [
        str(toolchain.linker),
        *expand_tokens(variant_build["linker_args"], values),
    ]
    append_frequency_sample(environment, int(environment["cpu"]))
    link_time = _measure_command(linker_command, timeout)
    if not binary.is_file():
        raise GateError("LINK_OUTPUT_MISSING", f"linker did not create {binary}")
    return binary, {
        "status": "OK",
        "samples_sec": samples,
        "median_sec": strict_median(samples),
        "link_time_sec": link_time,
        "compiler_command": compiler_command,
        "linker_command": linker_command,
    }


def _validate_program_result(
    result: subprocess.CompletedProcess[bytes], expected_stdout: str, expected_exit: int
) -> None:
    actual_stdout = normalize_output(result.stdout.decode(errors="replace"))
    if result.returncode != expected_exit:
        raise GateError(
            "BENCHMARK_EXIT_MISMATCH",
            f"benchmark exited {result.returncode}, expected {expected_exit}",
        )
    if actual_stdout != normalize_output(expected_stdout):
        raise GateError("BENCHMARK_OUTPUT_MISMATCH", "benchmark stdout differs from .out")


def run_warmup(
    binary: Path,
    input_data: bytes,
    expected_stdout: str,
    expected_exit: int,
    timeout: float,
) -> None:
    try:
        result = subprocess.run(
            [str(binary)],
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env={**os.environ, "LC_ALL": "C"},
        )
    except subprocess.TimeoutExpired as exc:
        raise GateError("BENCHMARK_TIMEOUT", f"warmup timed out: {binary}") from exc
    _validate_program_result(result, expected_stdout, expected_exit)


def run_perf_sample(
    *,
    perf: Path,
    binary: Path,
    perf_output: Path,
    input_data: bytes,
    expected_stdout: str,
    expected_exit: int,
    timeout: float,
) -> dict[str, float]:
    perf_output.unlink(missing_ok=True)
    command = [
        str(perf),
        "stat",
        "--no-big-num",
        "-x",
        ";",
        "-e",
        "cycles,instructions",
        "-o",
        str(perf_output),
        "--",
        str(binary),
    ]
    start = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env={**os.environ, "LC_ALL": "C"},
        )
    except subprocess.TimeoutExpired as exc:
        raise GateError("BENCHMARK_TIMEOUT", f"perf sample timed out: {binary}") from exc
    controller_wall = time.perf_counter() - start
    _validate_program_result(result, expected_stdout, expected_exit)
    if not perf_output.is_file():
        raise GateError("PERF_OUTPUT_MISSING", f"perf did not create {perf_output}")
    metrics = parse_perf_stat(perf_output.read_text(errors="replace"))
    metrics["controller_wall_time_sec"] = controller_wall
    return metrics


def summarize_samples(samples: Sequence[Mapping[str, float]]) -> dict[str, Any]:
    if not samples:
        raise GateError("SAMPLES_MISSING", "no measurement samples were recorded")
    summary: dict[str, Any] = {"samples": [dict(sample) for sample in samples]}
    for key in (
        "cycles",
        "instructions",
        "ipc",
        "wall_time_sec",
        "controller_wall_time_sec",
    ):
        values = [float(sample[key]) for sample in samples]
        summary[f"median_{key}"] = strict_median(values)
        if key in {"cycles", "wall_time_sec"}:
            summary[f"relative_mad_{key}"] = relative_mad(values)
    return summary


def append_frequency_sample(environment: dict[str, Any], cpu: int) -> None:
    current = read_frequency(cpu).get("current_khz")
    samples = environment.setdefault("frequency_samples_khz", [])
    if isinstance(samples, list):
        samples.append(current)


def measure_kernel(
    *,
    kernel: Mapping[str, Any],
    manifest: Mapping[str, Any],
    workspace: Path,
    run_dir: Path,
    toolchain: Toolchain,
    warmups: int,
    repetitions: int,
    timeout: float,
    environment: dict[str, Any],
) -> dict[str, Any]:
    protocol = manifest["protocol"]
    binaries: dict[str, Path] = {}
    compile_metrics: dict[str, dict[str, Any]] = {}
    analysis: dict[str, dict[str, Any]] = {}
    vlenb_bytes = environment.get("vlenb_bytes")
    if vlenb_bytes is not None and not isinstance(vlenb_bytes, int):
        raise GateError("VLENB_INVALID", "environment vlenb_bytes must be an integer")
    for variant in ("scalar", "rvv"):
        binary, compile_metric = build_variant(
            variant=variant,
            kernel=kernel,
            manifest=manifest,
            workspace=workspace,
            run_dir=run_dir,
            toolchain=toolchain,
            environment=environment,
            compile_warmups=int(protocol["compile_warmups"]),
            compile_repetitions=int(protocol["compile_repetitions"]),
            timeout=timeout,
        )
        binaries[variant] = binary
        compile_metrics[variant] = compile_metric
        analysis[variant] = inspect_binary(
            binary,
            toolchain.objdump,
            timeout,
            vlenb_bytes=vlenb_bytes,
        )

    if toolchain.mode == "compile":
        assert toolchain.rvv_compiler is not None
        if not isinstance(vlenb_bytes, int):
            raise GateError(
                "VLENB_PROBE_REQUIRED",
                "compile-mode post-RA spill analysis requires the native vlenb probe",
            )
        vectorization = collect_vectorization_analysis(
            kernel=kernel,
            manifest=manifest,
            workspace=workspace,
            run_dir=run_dir,
            compiler=toolchain.rvv_compiler,
            timeout=timeout,
        )
        register_allocation = collect_post_ra_spill_analysis(
            kernel=kernel,
            manifest=manifest,
            workspace=workspace,
            run_dir=run_dir,
            compiler=toolchain.rvv_compiler,
            timeout=timeout,
            vlenb_bytes=vlenb_bytes,
        )
    else:
        vectorization = {
            "status": "UNAVAILABLE_PREBUILT",
            "plan_count": None,
            "vectorized_count": None,
            "loop_vectorized_count": None,
            "rejected_count": None,
            "code_counts": {},
            "vectorizer_counts": {},
            "plans": [],
        }
        register_allocation = {
            "status": "UNAVAILABLE_PREBUILT",
            "spill_slot_count": None,
            "callee_saved_slot_count": None,
            "spill_slot_bytes": None,
            "callee_saved_slot_bytes": None,
            "whole_spill_sites": None,
            "whole_reload_sites": None,
            "functions": {},
        }

    if analysis["scalar"]["rvv_opcode_count"] != 0:
        raise GateError(
            "SCALAR_CONTAINS_RVV",
            f"{kernel['id']} scalar binary contains decoded RVV instructions",
        )
    if (
        kernel["classification"] == EXPECTED_VECTORIZABLE
        and analysis["rvv"]["rvv_opcode_count"] <= 0
    ):
        raise GateError(
            "EXPECTED_RVV_OPCODE_MISSING",
            f"{kernel['id']} is expected-vectorizable but RVV binary has no RVV opcode",
        )

    source = _path_inside(workspace, str(kernel["source"]), f"{kernel['id']}.source")
    input_path = _path_inside(workspace, str(kernel["input"]), f"{kernel['id']}.input")
    output_path = _path_inside(workspace, str(kernel["output"]), f"{kernel['id']}.output")
    expected = read_sysy_expected_output(source)
    if expected is None or output_path != source.with_suffix(".out"):
        raise GateError("EXPECTED_OUTPUT_INVALID", f"{kernel['id']} .out contract is invalid")
    expected_stdout, expected_exit = expected
    input_data = input_path.read_bytes()

    for round_index in range(warmups):
        order = ("scalar", "rvv") if round_index % 2 == 0 else ("rvv", "scalar")
        for variant in order:
            append_frequency_sample(environment, int(environment["cpu"]))
            run_warmup(
                binaries[variant], input_data, expected_stdout, expected_exit, timeout
            )
            if variant == "rvv" and analysis["rvv"]["rvv_opcode_count"] > 0:
                environment["vector_execution_verified"] = True

    raw_samples: dict[str, list[dict[str, float]]] = {"scalar": [], "rvv": []}
    for sample_index in range(repetitions):
        order = ("scalar", "rvv") if sample_index % 2 == 0 else ("rvv", "scalar")
        for variant in order:
            append_frequency_sample(environment, int(environment["cpu"]))
            perf_output = run_dir / f"{kernel['id']}.{variant}.{sample_index}.perf.txt"
            raw_samples[variant].append(
                run_perf_sample(
                    perf=toolchain.perf,
                    binary=binaries[variant],
                    perf_output=perf_output,
                    input_data=input_data,
                    expected_stdout=expected_stdout,
                    expected_exit=expected_exit,
                    timeout=timeout,
                )
            )

    variants: dict[str, Any] = {}
    for variant in ("scalar", "rvv"):
        variants[variant] = {
            "compile": compile_metrics[variant],
            "binary": analysis[variant],
            "performance": summarize_samples(raw_samples[variant]),
        }
    variants["rvv"]["vectorization"] = vectorization
    variants["rvv"]["register_allocation"] = register_allocation
    scalar_cycles = variants["scalar"]["performance"]["median_cycles"]
    rvv_cycles = variants["rvv"]["performance"]["median_cycles"]
    return {
        "id": kernel["id"],
        "source": kernel["source"],
        "classification": kernel["classification"],
        "minimum_verified_vectorized_loops": kernel.get(
            "minimum_verified_vectorized_loops"
        ),
        "dedupe_group": kernel.get("dedupe_group", kernel["id"]),
        "tags": list(kernel["tags"]),
        "hotspot": kernel["hotspot"],
        "status": "OK",
        "scalar": variants["scalar"],
        "rvv": variants["rvv"],
        "speedup_cycles": scalar_cycles / rvv_cycles,
        "slowdown_fraction_cycles": rvv_cycles / scalar_cycles - 1.0,
    }


def _failure(code: str, detail: str, kernel: str | None = None) -> dict[str, str]:
    result = {"code": code, "detail": detail}
    if kernel is not None:
        result["kernel"] = kernel
    return result


def _group_geomean(rows: Sequence[Mapping[str, Any]], value_key: str) -> float | None:
    grouped: dict[str, list[float]] = {}
    for row in rows:
        group = str(row.get("dedupe_group", row.get("id", "")))
        value = row.get(value_key)
        if isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0.0:
            grouped.setdefault(group, []).append(float(value))
    group_values = [positive_geomean(values) for values in grouped.values()]
    return positive_geomean(value for value in group_values if value is not None)


def evaluate_release_gate(
    manifest: Mapping[str, Any], kernel_results: Sequence[Mapping[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, str]]]:
    """Evaluate all five GA thresholds; missing evidence always fails."""

    failures: list[dict[str, str]] = []
    definitions = {str(kernel["id"]): kernel for kernel in manifest["kernels"]}
    expected_ids = set(definitions)
    actual_id_list = [str(row.get("id", "")) for row in kernel_results]
    actual_ids = set(actual_id_list)
    missing = sorted(expected_ids - actual_ids)
    extra = sorted(actual_ids - expected_ids)
    if missing:
        failures.append(_failure("KERNEL_RESULTS_MISSING", f"missing results: {missing}"))
    if extra:
        failures.append(_failure("KERNEL_RESULTS_EXTRA", f"unexpected results: {extra}"))
    if len(actual_id_list) != len(actual_ids):
        failures.append(_failure("KERNEL_RESULTS_DUPLICATE", "duplicate kernel result ids"))

    rows: list[dict[str, Any]] = []
    max_mad = float(manifest["protocol"]["max_relative_mad"])
    minimum_samples = int(manifest["protocol"]["minimum_repetitions"])
    for raw_row in kernel_results:
        kernel_id = str(raw_row.get("id"))
        if raw_row.get("status") != "OK":
            failures.append(
                _failure("KERNEL_RESULT_NOT_OK", "kernel result status is not OK", kernel_id)
            )
            continue
        definition = definitions.get(kernel_id)
        if definition is None:
            continue
        for key in (
            "classification",
            "minimum_verified_vectorized_loops",
            "dedupe_group",
            "tags",
            "hotspot",
        ):
            if raw_row.get(key) != definition.get(key):
                failures.append(
                    _failure(
                        "KERNEL_METADATA_MISMATCH",
                        f"result {key} does not match the manifest",
                        kernel_id,
                    )
                )
        row = dict(raw_row)
        row["classification"] = definition["classification"]
        row["minimum_verified_vectorized_loops"] = definition.get(
            "minimum_verified_vectorized_loops"
        )
        row["dedupe_group"] = definition.get("dedupe_group", kernel_id)
        row["tags"] = list(definition["tags"])
        row["hotspot"] = bool(definition["hotspot"])
        variant_cycles: dict[str, float] = {}
        for variant in ("scalar", "rvv"):
            try:
                performance = row[variant]["performance"]
                cycles = float(performance["median_cycles"])
                instructions = float(performance["median_instructions"])
                wall = float(performance["median_wall_time_sec"])
                ipc = float(performance["median_ipc"])
                mad = float(performance["relative_mad_cycles"])
                text_size = int(row[variant]["binary"]["text_size_bytes"])
                samples = performance["samples"]
            except (KeyError, TypeError, ValueError):
                failures.append(
                    _failure("KERNEL_METRICS_INCOMPLETE", f"{variant} metrics incomplete", kernel_id)
                )
                continue
            if not all(
                math.isfinite(value)
                for value in (cycles, instructions, wall, ipc, mad, float(text_size))
            ) or min(cycles, instructions, wall, ipc, text_size) <= 0:
                failures.append(
                    _failure("KERNEL_METRICS_NONPOSITIVE", f"{variant} metrics are invalid", kernel_id)
                )
            if mad > max_mad:
                failures.append(
                    _failure(
                        "KERNEL_SAMPLES_UNSTABLE",
                        f"{variant} cycles relative MAD {mad:.4f} exceeds {max_mad:.4f}",
                        kernel_id,
                    )
                )
            if not isinstance(samples, list) or len(samples) < minimum_samples:
                failures.append(
                    _failure(
                        "KERNEL_SAMPLES_INSUFFICIENT",
                        f"{variant} has fewer than {minimum_samples} raw samples",
                        kernel_id,
                    )
                )
            else:
                try:
                    raw_cycles = [float(sample["cycles"]) for sample in samples]
                    for sample in samples:
                        raw_values = (
                            float(sample["cycles"]),
                            float(sample["instructions"]),
                            float(sample["ipc"]),
                            float(sample["wall_time_sec"]),
                        )
                        if not all(math.isfinite(value) for value in raw_values) or min(
                            raw_values
                        ) <= 0.0:
                            raise ValueError("non-positive raw metric")
                    recomputed_cycles = strict_median(raw_cycles)
                except (KeyError, TypeError, ValueError):
                    failures.append(
                        _failure(
                            "KERNEL_RAW_SAMPLES_INVALID",
                            f"{variant} raw samples are incomplete or non-positive",
                            kernel_id,
                        )
                    )
                else:
                    if cycles > 0.0 and abs(recomputed_cycles - cycles) / cycles > 1.0e-9:
                        failures.append(
                            _failure(
                                "KERNEL_SUMMARY_MISMATCH",
                                f"{variant} median cycles do not match raw samples",
                                kernel_id,
                            )
                        )
            if math.isfinite(cycles) and cycles > 0.0:
                variant_cycles[variant] = cycles
        try:
            scalar_rvv = int(row["scalar"]["binary"]["rvv_opcode_count"])
            rvv_rvv = int(row["rvv"]["binary"]["rvv_opcode_count"])
        except (AttributeError, KeyError, TypeError, ValueError):
            failures.append(_failure("RVV_ANALYSIS_MISSING", "opcode evidence missing", kernel_id))
        else:
            if scalar_rvv != 0:
                failures.append(
                    _failure("SCALAR_CONTAINS_RVV", "scalar binary contains RVV opcodes", kernel_id)
                )
            if row.get("classification") == EXPECTED_VECTORIZABLE and rvv_rvv <= 0:
                failures.append(
                    _failure(
                        "EXPECTED_RVV_OPCODE_MISSING",
                        "expected-vectorizable RVV binary contains no decoded RVV opcode",
                        kernel_id,
                    )
                )

        try:
            rvv_binary = row["rvv"]["binary"]
            vlenb_bytes = int(rvv_binary["vlenb_bytes"])
            spill_store_units = int(rvv_binary["vr_spill_store_vlenb_units"])
            spill_reload_units = int(rvv_binary["vr_spill_reload_vlenb_units"])
            spill_store_bytes = int(rvv_binary["vr_spill_store_bytes"])
            spill_reload_bytes = int(rvv_binary["vr_spill_reload_bytes"])
            spill_transfer_bytes = int(rvv_binary["vr_spill_transfer_bytes"])
        except (KeyError, TypeError, ValueError):
            failures.append(
                _failure(
                    "VR_SPILL_BYTES_MISSING",
                    "exact whole-register spill/reload byte evidence is missing",
                    kernel_id,
                )
            )
        else:
            expected_store_bytes = spill_store_units * vlenb_bytes
            expected_reload_bytes = spill_reload_units * vlenb_bytes
            if (
                vlenb_bytes <= 0
                or min(spill_store_units, spill_reload_units) < 0
                or spill_store_bytes != expected_store_bytes
                or spill_reload_bytes != expected_reload_bytes
                or spill_transfer_bytes != expected_store_bytes + expected_reload_bytes
            ):
                failures.append(
                    _failure(
                        "VR_SPILL_BYTES_INVALID",
                        "whole-register spill byte totals do not match vlenb and decoded widths",
                        kernel_id,
                    )
                )

        try:
            register_allocation = row["rvv"]["register_allocation"]
            ra_status = register_allocation["status"]
            ra_vlenb = int(register_allocation["vlenb_bytes"])
            binary_vlenb = int(row["rvv"]["binary"]["vlenb_bytes"])
            spill_slot_count = int(register_allocation["spill_slot_count"])
            callee_saved_slot_count = int(
                register_allocation["callee_saved_slot_count"]
            )
            spill_slot_bytes = int(register_allocation["spill_slot_bytes"])
            callee_saved_slot_bytes = int(
                register_allocation["callee_saved_slot_bytes"]
            )
            mir_spill_sites = int(register_allocation["whole_spill_sites"])
            mir_reload_sites = int(register_allocation["whole_reload_sites"])
            binary_spill_sites = int(
                row["rvv"]["binary"]["vr_spill_store_sites"]
            )
            binary_reload_sites = int(
                row["rvv"]["binary"]["vr_spill_reload_sites"]
            )
        except (KeyError, TypeError, ValueError):
            failures.append(
                _failure(
                    "MIR_SPILL_EVIDENCE_MISSING",
                    "post-RA unique spill-slot evidence is missing",
                    kernel_id,
                )
            )
        else:
            if (
                ra_status != "OK"
                or ra_vlenb != binary_vlenb
                or min(
                    spill_slot_count,
                    callee_saved_slot_count,
                    spill_slot_bytes,
                    callee_saved_slot_bytes,
                    mir_spill_sites,
                    mir_reload_sites,
                )
                < 0
                or mir_spill_sites != binary_spill_sites
                or mir_reload_sites != binary_reload_sites
            ):
                failures.append(
                    _failure(
                        "MIR_SPILL_EVIDENCE_INVALID",
                        "post-RA slots and final whole-register sites do not agree",
                        kernel_id,
                    )
                )

        try:
            vectorization = row["rvv"]["vectorization"]
            vectorization_status = vectorization["status"]
            vectorized_count = int(vectorization["vectorized_count"])
            loop_vectorized_count = int(vectorization["loop_vectorized_count"])
            rejected_count = int(vectorization["rejected_count"])
            plans = vectorization["plans"]
        except (KeyError, TypeError, ValueError):
            failures.append(
                _failure(
                    "VECTOR_PLAN_EVIDENCE_MISSING",
                    "machine-readable vectorized/rejected loop evidence is missing",
                    kernel_id,
                )
            )
        else:
            if (
                vectorization_status != "OK"
                or vectorized_count < 0
                or loop_vectorized_count < 0
                or rejected_count < 0
                or not isinstance(plans, list)
                or vectorized_count + rejected_count != len(plans)
                or vectorized_count
                != sum(
                    isinstance(plan, Mapping) and plan.get("code") == "VECTORIZED"
                    for plan in plans
                )
                or loop_vectorized_count
                != sum(
                    isinstance(plan, Mapping)
                    and plan.get("code") == "VECTORIZED"
                    and plan.get("vectorizer") == "loop"
                    for plan in plans
                )
            ):
                failures.append(
                    _failure(
                        "VECTOR_PLAN_EVIDENCE_INVALID",
                        "vector-plan counts or status are inconsistent",
                        kernel_id,
                    )
                )
            elif (
                row.get("classification") == EXPECTED_VECTORIZABLE
                and loop_vectorized_count
                < int(definition["minimum_verified_vectorized_loops"])
            ):
                failures.append(
                    _failure(
                        "EXPECTED_VECTORIZED_PLAN_MISSING",
                        "expected-vectorizable kernel has fewer verified loop VECTORIZED "
                        "plans than minimum_verified_vectorized_loops",
                        kernel_id,
                    )
                )

        if set(variant_cycles) == {"scalar", "rvv"}:
            computed_speedup = variant_cycles["scalar"] / variant_cycles["rvv"]
            row["speedup_cycles"] = computed_speedup
            row["slowdown_fraction_cycles"] = 1.0 / computed_speedup - 1.0
        else:
            row.pop("speedup_cycles", None)
            row.pop("slowdown_fraction_cycles", None)
        rows.append(row)

    expected = [row for row in rows if row.get("classification") == EXPECTED_VECTORIZABLE]
    unit_stride = [row for row in expected if "unit-stride" in row.get("tags", [])]
    negative = [row for row in rows if row.get("classification") == NEGATIVE_CONTROL]
    thresholds = manifest["thresholds"]
    unit_geomean = _group_geomean(unit_stride, "speedup_cycles")
    corpus_geomean = _group_geomean(expected, "speedup_cycles")
    negative_rows = []
    for row in negative:
        speedup = row.get("speedup_cycles")
        if isinstance(speedup, (int, float)) and speedup > 0:
            copy = dict(row)
            copy["regression_factor"] = 1.0 / float(speedup)
            negative_rows.append(copy)
    negative_regression = _group_geomean(negative_rows, "regression_factor")

    compile_rows: list[dict[str, Any]] = []
    for row in rows:
        try:
            scalar_compile_info = row["scalar"]["compile"]
            rvv_compile_info = row["rvv"]["compile"]
            if (
                scalar_compile_info.get("status") != "OK"
                or rvv_compile_info.get("status") != "OK"
            ):
                raise ValueError("compile status is not OK")
            scalar_compile = float(scalar_compile_info["median_sec"])
            rvv_compile = float(rvv_compile_info["median_sec"])
            scalar_compile_samples = scalar_compile_info["samples_sec"]
            rvv_compile_samples = rvv_compile_info["samples_sec"]
        except (AttributeError, KeyError, TypeError, ValueError):
            failures.append(
                _failure(
                    "COMPILE_TIME_MISSING",
                    "compile-time evidence is required for an official gate",
                    str(row.get("id")),
                )
            )
            continue
        required_compile_samples = int(manifest["protocol"]["compile_repetitions"])
        if (
            not isinstance(scalar_compile_samples, list)
            or not isinstance(rvv_compile_samples, list)
            or len(scalar_compile_samples) < required_compile_samples
            or len(rvv_compile_samples) < required_compile_samples
        ):
            failures.append(
                _failure(
                    "COMPILE_SAMPLES_INSUFFICIENT",
                    f"compile evidence requires {required_compile_samples} samples per variant",
                    str(row.get("id")),
                )
            )
            continue
        if scalar_compile <= 0.0 or rvv_compile <= 0.0:
            failures.append(
                _failure("COMPILE_TIME_INVALID", "compile times must be positive", str(row.get("id")))
            )
            continue
        try:
            scalar_compile_values = [float(value) for value in scalar_compile_samples]
            rvv_compile_values = [float(value) for value in rvv_compile_samples]
            if not all(
                math.isfinite(value)
                for value in scalar_compile_values + rvv_compile_values
            ) or min(scalar_compile_values + rvv_compile_values) <= 0.0:
                raise ValueError("non-positive compile sample")
            recomputed_scalar_compile = strict_median(scalar_compile_values)
            recomputed_rvv_compile = strict_median(rvv_compile_values)
        except (TypeError, ValueError):
            failures.append(
                _failure(
                    "COMPILE_SAMPLES_INVALID",
                    "compile samples must be positive numeric values",
                    str(row.get("id")),
                )
            )
            continue
        if (
            recomputed_scalar_compile <= 0.0
            or recomputed_rvv_compile <= 0.0
            or abs(recomputed_scalar_compile - scalar_compile) / scalar_compile > 1.0e-9
            or abs(recomputed_rvv_compile - rvv_compile) / rvv_compile > 1.0e-9
        ):
            failures.append(
                _failure(
                    "COMPILE_SUMMARY_MISMATCH",
                    "compile medians do not match positive raw samples",
                    str(row.get("id")),
                )
            )
            continue
        compile_rows.append(
            {
                "id": row.get("id"),
                "dedupe_group": row.get("dedupe_group", row.get("id")),
                "compile_regression_factor": rvv_compile / scalar_compile,
            }
        )
    compile_regression = _group_geomean(compile_rows, "compile_regression_factor")

    if unit_geomean is None:
        failures.append(_failure("UNIT_STRIDE_AGGREGATE_MISSING", "unit-stride evidence missing"))
    elif unit_geomean + 1.0e-12 < float(thresholds["unit_stride_geomean_min_speedup"]):
        failures.append(
            _failure(
                "UNIT_STRIDE_SPEEDUP",
                f"geomean {unit_geomean:.4f}x is below {thresholds['unit_stride_geomean_min_speedup']:.4f}x",
            )
        )
    if corpus_geomean is None:
        failures.append(_failure("VECTOR_CORPUS_AGGREGATE_MISSING", "vector corpus evidence missing"))
    elif corpus_geomean + 1.0e-12 < float(
        thresholds["vectorizable_corpus_geomean_min_speedup"]
    ):
        failures.append(
            _failure(
                "VECTOR_CORPUS_SPEEDUP",
                f"deduplicated geomean {corpus_geomean:.4f}x is below {thresholds['vectorizable_corpus_geomean_min_speedup']:.4f}x",
            )
        )
    hotspot_limit = float(thresholds["hotspot_max_slowdown_fraction"])
    for row in expected:
        slowdown = row.get("slowdown_fraction_cycles")
        if row.get("hotspot") and not isinstance(slowdown, (int, float)):
            failures.append(
                _failure(
                    "HOTSPOT_METRICS_MISSING",
                    "hotspot slowdown evidence is missing",
                    str(row.get("id")),
                )
            )
        elif row.get("hotspot") and float(slowdown) > hotspot_limit + 1.0e-12:
            failures.append(
                _failure(
                    "HOTSPOT_SLOWDOWN",
                    f"RVV median cycles are {float(slowdown) * 100.0:.2f}% slower than scalar; limit is {hotspot_limit * 100.0:.2f}%",
                    str(row.get("id")),
                )
            )
    negative_limit = 1.0 + float(thresholds["negative_control_max_geomean_regression"])
    if negative_regression is None:
        failures.append(_failure("NEGATIVE_AGGREGATE_MISSING", "negative-control evidence missing"))
    elif negative_regression > negative_limit + 1.0e-12:
        failures.append(
            _failure(
                "NEGATIVE_CONTROL_REGRESSION",
                f"geomean regression factor {negative_regression:.4f} exceeds {negative_limit:.4f}",
            )
        )
    compile_limit = 1.0 + float(thresholds["compile_time_max_geomean_regression"])
    if compile_regression is None:
        failures.append(_failure("COMPILE_AGGREGATE_MISSING", "compile-time aggregate missing"))
    elif compile_regression > compile_limit + 1.0e-12:
        failures.append(
            _failure(
                "COMPILE_TIME_REGRESSION",
                f"geomean compile regression factor {compile_regression:.4f} exceeds {compile_limit:.4f}",
            )
        )

    hotspot_slowdowns = [
        float(row["slowdown_fraction_cycles"])
        for row in expected
        if row.get("hotspot")
        and isinstance(row.get("slowdown_fraction_cycles"), (int, float))
    ]
    max_hotspot_slowdown = max(hotspot_slowdowns) if hotspot_slowdowns else None

    def minimum_check(observed: float | None, threshold: float) -> dict[str, Any]:
        return {
            "observed": observed,
            "operator": ">=",
            "threshold": threshold,
            "passed": observed is not None and observed + 1.0e-12 >= threshold,
        }

    def maximum_check(observed: float | None, threshold: float) -> dict[str, Any]:
        return {
            "observed": observed,
            "operator": "<=",
            "threshold": threshold,
            "passed": observed is not None and observed <= threshold + 1.0e-12,
        }

    threshold_evaluations = {
        "unit_stride_speedup": minimum_check(
            unit_geomean, float(thresholds["unit_stride_geomean_min_speedup"])
        ),
        "vectorizable_corpus_speedup": minimum_check(
            corpus_geomean, float(thresholds["vectorizable_corpus_geomean_min_speedup"])
        ),
        "hotspot_slowdown_fraction": maximum_check(max_hotspot_slowdown, hotspot_limit),
        "negative_control_regression_factor": maximum_check(
            negative_regression, negative_limit
        ),
        "compile_time_regression_factor": maximum_check(compile_regression, compile_limit),
    }

    aggregates = {
        "unit_stride_geomean_speedup": unit_geomean,
        "vectorizable_corpus_deduplicated_geomean_speedup": corpus_geomean,
        "negative_control_geomean_regression_factor": negative_regression,
        "compile_time_geomean_regression_factor": compile_regression,
        "unit_stride_groups": sorted({str(row.get("dedupe_group")) for row in unit_stride}),
        "vectorizable_groups": sorted({str(row.get("dedupe_group")) for row in expected}),
        "negative_control_groups": sorted({str(row.get("dedupe_group")) for row in negative}),
        "maximum_hotspot_slowdown_fraction": max_hotspot_slowdown,
        "threshold_evaluations": threshold_evaluations,
    }
    return aggregates, failures


def render_markdown(report: Mapping[str, Any]) -> str:
    status = str(report.get("status", "ERROR"))
    official = bool(report.get("official"))
    lines = [
        "# RVV Native Hardware Performance Gate",
        "",
        f"- Status: **{status}**",
        f"- Official hardware result: **{'YES' if official else 'NO'}**",
        f"- Generated: {report.get('generated_utc', '')}",
        "- QEMU timing accepted: **NO**",
    ]
    if not report.get("gate_executed"):
        lines.extend(
            [
                "",
                "> The formal RVV hardware gate was not run. No performance claim is made.",
            ]
        )
    environment = report.get("environment", {})
    if isinstance(environment, Mapping):
        lines.extend(
            [
                "",
                "## Environment",
                "",
                f"- Machine: `{environment.get('machine', 'unknown')}`",
                f"- CPU affinity: `{environment.get('affinity', [])}`",
                f"- ISA: `{environment.get('isa', [])}`",
                f"- V extension: `{environment.get('v_extension', False)}`",
                f"- Vector execution verified: `{environment.get('vector_execution_verified', False)}`",
                f"- VLEN: `{environment.get('vlen_bits', 'unavailable')}` bits "
                f"(`vlenb={environment.get('vlenb_bytes', 'unavailable')}`)",
                f"- Frequency policy: `{report.get('configuration', {}).get('frequency_policy', 'unknown')}`",
                f"- Initial frequency: `{environment.get('frequency_initial', {})}`",
            ]
        )
    failures = report.get("failures", [])
    if isinstance(failures, list) and failures:
        lines.extend(["", "## Blocking Findings", ""])
        for finding in failures:
            if isinstance(finding, Mapping):
                kernel = f" ({finding['kernel']})" if finding.get("kernel") else ""
                lines.append(f"- `{finding.get('code', 'UNKNOWN')}`{kernel}: {finding.get('detail', '')}")
    aggregates = report.get("aggregates")
    if isinstance(aggregates, Mapping) and aggregates:
        lines.extend(
            [
                "",
                "## Release Aggregates",
                "",
                "| Metric | Observed |",
                "| --- | ---: |",
            ]
        )
        for key, value in aggregates.items():
            if isinstance(value, (int, float)) or value is None:
                rendered = "N/A" if value is None else f"{float(value):.4f}"
                lines.append(f"| {key} | {rendered} |")
        evaluations = aggregates.get("threshold_evaluations")
        if isinstance(evaluations, Mapping):
            lines.extend(
                [
                    "",
                    "| Release check | Observed | Requirement | Result |",
                    "| --- | ---: | ---: | --- |",
                ]
            )
            for name, evaluation in evaluations.items():
                if not isinstance(evaluation, Mapping):
                    continue
                observed = evaluation.get("observed")
                rendered_observed = (
                    "N/A" if observed is None else f"{float(observed):.4f}"
                )
                lines.append(
                    f"| {name} | {rendered_observed} | "
                    f"{evaluation.get('operator')} {float(evaluation.get('threshold')):.4f} | "
                    f"{'PASS' if evaluation.get('passed') else 'FAIL'} |"
                )
    kernels = report.get("kernels", [])
    if isinstance(kernels, list) and kernels:
        lines.extend(
            [
                "",
                "## Per-kernel Scalar vs RVV",
                "",
                "| Kernel | Class | Scalar cycles | RVV cycles | Speedup | Scalar wall (s) | RVV wall (s) | Scalar IPC | RVV IPC | Compile ratio | Text ratio | RVV ops | vsetvl | VR spill slot bytes | VR spill bytes | VR reload bytes | Loop/total/reject plans (min loop) |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in kernels:
            if not isinstance(row, Mapping) or row.get("status") != "OK":
                continue
            scalar = row["scalar"]
            rvv = row["rvv"]
            scalar_perf = scalar["performance"]
            rvv_perf = rvv["performance"]
            scalar_compile = scalar["compile"].get("median_sec")
            rvv_compile = rvv["compile"].get("median_sec")
            compile_ratio = (
                rvv_compile / scalar_compile
                if isinstance(scalar_compile, (int, float))
                and isinstance(rvv_compile, (int, float))
                and scalar_compile > 0
                else None
            )
            text_ratio = rvv["binary"]["text_size_bytes"] / scalar["binary"]["text_size_bytes"]
            vectorization = rvv.get("vectorization", {})
            register_allocation = rvv.get("register_allocation", {})
            values = [
                str(row["id"]),
                str(row["classification"]),
                f"{scalar_perf['median_cycles']:.0f}",
                f"{rvv_perf['median_cycles']:.0f}",
                f"{row['speedup_cycles']:.3f}x",
                f"{scalar_perf['median_wall_time_sec']:.6f}",
                f"{rvv_perf['median_wall_time_sec']:.6f}",
                f"{scalar_perf['median_ipc']:.3f}",
                f"{rvv_perf['median_ipc']:.3f}",
                "N/A" if compile_ratio is None else f"{compile_ratio:.3f}x",
                f"{text_ratio:.3f}x",
                str(rvv["binary"]["rvv_opcode_count"]),
                str(rvv["binary"]["vsetvl_count"]),
                str(register_allocation.get("spill_slot_bytes", "N/A")),
                str(rvv["binary"].get("vr_spill_store_bytes", "N/A")),
                str(rvv["binary"].get("vr_spill_reload_bytes", "N/A")),
                f"{vectorization.get('loop_vectorized_count', 'N/A')}/"
                f"{vectorization.get('vectorized_count', 'N/A')}/"
                f"{vectorization.get('rejected_count', 'N/A')} "
                f"(>={row.get('minimum_verified_vectorized_loops', 'N/A')})",
            ]
            lines.append("| " + " | ".join(values) + " |")
        lines.extend(["", "## Vectorization Decisions", ""])
        for row in kernels:
            if not isinstance(row, Mapping) or row.get("status") != "OK":
                continue
            lines.append(f"### {row.get('id', 'unknown')}")
            lines.append("")
            rvv = row.get("rvv", {})
            vectorization = rvv.get("vectorization", {}) if isinstance(rvv, Mapping) else {}
            plans = vectorization.get("plans", []) if isinstance(vectorization, Mapping) else []
            if not isinstance(plans, list) or not plans:
                lines.append("- No compiler vectorization decision evidence was available.")
                lines.append("")
                continue
            for plan in plans:
                if not isinstance(plan, Mapping):
                    continue
                location = f"{plan.get('function', '?')}:{plan.get('region', '?')}"
                lines.append(
                    f"- `{plan.get('code', 'UNKNOWN')}` "
                    f"({plan.get('vectorizer', 'unknown')}, `{location}`): "
                    f"{plan.get('explanation', '')}"
                )
            lines.append("")
    return "\n".join(lines) + "\n"


def write_reports(report: Mapping[str, Any], json_path: Path, markdown_path: Path) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    markdown_path.write_text(render_markdown(report))


def _explicit_file(path: Path | None, label: str, *, executable: bool) -> Path:
    if path is None:
        raise GateError("TOOL_PATH_REQUIRED", f"{label} requires an explicit absolute path")
    if not path.is_absolute():
        raise GateError("TOOL_PATH_NOT_ABSOLUTE", f"{label} must be absolute: {path}")
    resolved = path.resolve()
    if not resolved.is_file():
        raise GateError("TOOL_PATH_INVALID", f"{label} is not a file: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise GateError("TOOL_PATH_INVALID", f"{label} is not executable: {resolved}")
    return resolved


def _explicit_directory(path: Path | None, label: str) -> Path:
    if path is None:
        raise GateError("TOOL_PATH_REQUIRED", f"{label} requires an explicit absolute path")
    if not path.is_absolute():
        raise GateError("TOOL_PATH_NOT_ABSOLUTE", f"{label} must be absolute: {path}")
    resolved = path.resolve()
    if not resolved.is_dir():
        raise GateError("TOOL_PATH_INVALID", f"{label} is not a directory: {resolved}")
    return resolved


def resolve_toolchain(args: argparse.Namespace) -> Toolchain:
    perf = _explicit_file(args.perf_tool, "--perf-tool", executable=True)
    objdump = _explicit_file(args.objdump, "--objdump", executable=True)
    compile_selected = args.scalar_compiler is not None or args.rvv_compiler is not None
    prebuilt_selected = args.scalar_binary_dir is not None or args.rvv_binary_dir is not None
    if compile_selected and prebuilt_selected:
        raise GateError("TOOL_MODE_CONFLICT", "compiler and prebuilt modes are mutually exclusive")
    if compile_selected:
        return Toolchain(
            mode="compile",
            perf=perf,
            objdump=objdump,
            scalar_compiler=_explicit_file(
                args.scalar_compiler, "--scalar-compiler", executable=True
            ),
            rvv_compiler=_explicit_file(args.rvv_compiler, "--rvv-compiler", executable=True),
            linker=_explicit_file(args.linker, "--linker", executable=True),
            runtime_lib=_explicit_file(args.runtime_lib, "--runtime-lib", executable=False),
        )
    if prebuilt_selected:
        return Toolchain(
            mode="prebuilt",
            perf=perf,
            objdump=objdump,
            scalar_binary_dir=_explicit_directory(
                args.scalar_binary_dir, "--scalar-binary-dir"
            ),
            rvv_binary_dir=_explicit_directory(args.rvv_binary_dir, "--rvv-binary-dir"),
        )
    raise GateError(
        "TOOL_MODE_REQUIRED",
        "provide both compiler paths or both prebuilt binary directories",
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    workspace = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Run the fail-closed native RVV release performance gate."
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=workspace / "test/performance/rvv_hardware_manifest.json",
    )
    parser.add_argument("--cpu", type=int, required=True, help="single CPU to pin the run to")
    parser.add_argument(
        "--frequency-policy",
        choices=("require-fixed", "record"),
        default="require-fixed",
    )
    parser.add_argument("--warmups", type=int)
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument("--scalar-compiler", type=Path)
    parser.add_argument("--rvv-compiler", type=Path)
    parser.add_argument("--linker", type=Path)
    parser.add_argument("--runtime-lib", type=Path)
    parser.add_argument("--scalar-binary-dir", type=Path)
    parser.add_argument("--rvv-binary-dir", type=Path)
    parser.add_argument("--perf-tool", type=Path)
    parser.add_argument("--objdump", type=Path)
    parser.add_argument("--probe-only", action="store_true")
    parser.add_argument(
        "--work-dir", type=Path, default=workspace / "build/rvv-hardware-perf/work"
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        default=workspace / "build/rvv-hardware-perf/report.json",
    )
    parser.add_argument(
        "--output-markdown",
        type=Path,
        default=workspace / "build/rvv-hardware-perf/report.md",
    )
    return parser.parse_args(argv)


def _toolchain_report(toolchain: Toolchain) -> dict[str, Any]:
    result: dict[str, Any] = {"mode": toolchain.mode}
    for name in (
        "perf",
        "objdump",
        "scalar_compiler",
        "rvv_compiler",
        "linker",
        "runtime_lib",
        "scalar_binary_dir",
        "rvv_binary_dir",
    ):
        path = getattr(toolchain, name)
        if path is not None:
            item: dict[str, Any] = {"path": str(path)}
            if path.is_file():
                item["sha256"] = sha256_file(path)
            result[name] = item
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    workspace = Path(__file__).resolve().parents[1]
    report: dict[str, Any] = {
        "schema": REPORT_SCHEMA,
        "generated_utc": utc_now(),
        "status": "ERROR",
        "official": False,
        "gate_executed": False,
        "qemu_timing_accepted": False,
        "configuration": {
            "manifest": str(args.manifest.resolve()),
            "cpu": args.cpu,
            "frequency_policy": args.frequency_policy,
        },
        "environment": {},
        "thresholds": dict(RELEASE_THRESHOLDS),
        "aggregates": {},
        "failures": [],
        "kernels": [],
    }
    exit_code = 2
    try:
        manifest = load_manifest(args.manifest.resolve(), workspace)
        report["manifest_sha256"] = sha256_file(args.manifest.resolve())
        report["thresholds"] = dict(manifest["thresholds"])
        protocol = manifest["protocol"]
        warmups = args.warmups if args.warmups is not None else int(protocol["default_warmups"])
        repetitions = (
            args.repetitions
            if args.repetitions is not None
            else int(protocol["default_repetitions"])
        )
        if warmups < int(protocol["minimum_warmups"]):
            raise GateError(
                "PROTOCOL_WARMUPS_LOW",
                f"warmups={warmups} is below {protocol['minimum_warmups']}",
            )
        if repetitions < int(protocol["minimum_repetitions"]):
            raise GateError(
                "PROTOCOL_REPETITIONS_LOW",
                f"repetitions={repetitions} is below {protocol['minimum_repetitions']}",
            )
        if args.timeout_seconds <= 0.0:
            raise GateError("PROTOCOL_TIMEOUT", "timeout must be positive")
        report["configuration"].update(
            {
                "warmups": warmups,
                "repetitions": repetitions,
                "compile_warmups": protocol["compile_warmups"],
                "compile_repetitions": protocol["compile_repetitions"],
                "primary_metric": protocol["primary_metric"],
            }
        )

        apply_cpu_affinity(args.cpu)
        environment = collect_environment(args.cpu)
        report["environment"] = environment
        static_failures = environment_failures(
            environment,
            args.frequency_policy,
            require_vector_execution=False,
        )
        blocking_static_failures = preflight_blocking_failures(
            static_failures, args.frequency_policy
        )
        if blocking_static_failures:
            report["status"] = "BLOCKED"
            report["failures"] = blocking_static_failures
            raise GateError("ENVIRONMENT_BLOCKED", "native hardware preflight failed")
        if args.probe_only:
            report["status"] = "NOT_RUN"
            report["failures"] = [
                _failure(
                    "PROBE_ONLY",
                    "environment probe requested; formal performance measurements were not run",
                )
            ]
            raise GateError("PROBE_ONLY", "probe-only mode never produces an official result")

        toolchain = resolve_toolchain(args)
        report["toolchain"] = _toolchain_report(toolchain)
        run_id = _datetime.datetime.now(_datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        run_dir = args.work_dir.resolve() / run_id
        run_dir.mkdir(parents=True, exist_ok=False)
        report["configuration"]["work_dir"] = str(run_dir)
        report["gate_executed"] = True

        if toolchain.mode == "compile":
            assert toolchain.linker is not None
            vlenb_probe = probe_vlenb(toolchain.linker, run_dir, args.timeout_seconds)
            environment["vlenb_bytes"] = vlenb_probe["vlenb_bytes"]
            environment["vlen_bits"] = vlenb_probe["vlen_bits"]
            report["vlenb_probe"] = vlenb_probe

        kernel_results: list[dict[str, Any]] = []
        for kernel in manifest["kernels"]:
            kernel_results.append(
                measure_kernel(
                    kernel=kernel,
                    manifest=manifest,
                    workspace=workspace,
                    run_dir=run_dir,
                    toolchain=toolchain,
                    warmups=warmups,
                    repetitions=repetitions,
                    timeout=args.timeout_seconds,
                    environment=environment,
                )
            )
        append_frequency_sample(environment, args.cpu)
        report["kernels"] = kernel_results
        aggregates, gate_failures = evaluate_release_gate(manifest, kernel_results)
        report["aggregates"] = aggregates
        final_environment_failures = environment_failures(
            environment,
            args.frequency_policy,
            require_vector_execution=True,
        )
        failures = [*final_environment_failures, *gate_failures]
        report["failures"] = failures
        if failures:
            if gate_failures:
                report["status"] = "FAIL"
                exit_code = 1
            else:
                report["status"] = "NOT_OFFICIAL"
                exit_code = 2
        else:
            report["status"] = "PASS"
            report["official"] = True
            exit_code = 0
    except GateError as exc:
        if exc.code not in {"ENVIRONMENT_BLOCKED", "PROBE_ONLY"}:
            report["failures"] = [
                *report.get("failures", []),
                _failure(exc.code, str(exc)),
            ]
            if report.get("gate_executed"):
                report["status"] = "FAIL"
                exit_code = 1
            elif report.get("status") == "ERROR":
                report["status"] = "BLOCKED"
        print(f"{report['status']}: {exc.code}: {exc}", file=sys.stderr)
    except Exception as exc:  # pragma: no cover - last-resort report preservation.
        report["status"] = "ERROR"
        report["failures"] = [
            *report.get("failures", []),
            _failure("UNEXPECTED_ERROR", f"{type(exc).__name__}: {exc}"),
        ]
        exit_code = 2
        print(f"ERROR: {type(exc).__name__}: {exc}", file=sys.stderr)
    finally:
        report["generated_utc"] = utc_now()
        write_reports(report, args.output_json.resolve(), args.output_markdown.resolve())
        print(f"JSON report: {args.output_json.resolve()}")
        print(f"Markdown report: {args.output_markdown.resolve()}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
