#!/usr/bin/env python3

"""QEMU gate for performance-derived positive and negative RVV source cases."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
PASSING_CASES = ROOT / "test/rvv_performance_generated/passing"
VLENS = (128, 256, 512, 1024)


@dataclass(frozen=True)
class Case:
    name: str
    kind: str
    mnemonics: tuple[str, ...]

    @property
    def source(self) -> Path:
        return PASSING_CASES / f"{self.name}.sy"

    @property
    def input_bytes(self) -> bytes:
        path = PASSING_CASES / f"{self.name}.in"
        return path.read_bytes() if path.exists() else b""


CASES = (
    Case("rvv_perf_explicit_mm_row", "explicit", ("vmul.vv", "vadd.vv")),
    Case("rvv_perf_explicit_conv_nonlinear", "explicit", ("vrem.vv",)),
    Case("rvv_perf_explicit_stencil7", "explicit", ("vdiv.vv",)),
    Case("rvv_perf_explicit_fft_pointwise", "explicit", ("vrem.vv",)),
    Case("rvv_perf_explicit_transpose4", "explicit", ("vsetivli",)),
    Case("rvv_perf_explicit_mask_select", "explicit", ("vmerge.vvm",)),
    Case("rvv_perf_o2_mm_row", "loop", ("vmul.vv", "vse32.v")),
    Case("rvv_perf_o2_conv_nonlinear", "loop", ("vmul.vv", "vse32.v")),
    Case("rvv_perf_o2_reduction", "loop", ("vredsum.vs",)),
    Case("rvv_perf_o2_reverse", "loop", ("vlse32.v", "vsse32.v")),
    Case("rvv_perf_o2_diamond", "loop", ("vmsle.vv", "vse32.v")),
    Case("rvv_perf_o2_bitwise", "loop", ("vxor.vv", "vand.vv")),
    Case("rvv_perf_o2_inplace_affine", "loop", ("vle32.v", "vmul.vv", "vse32.v")),
    Case("rvv_perf_o2_four_array", "loop", ("vle32.v", "vmul.vv", "vsub.vv")),
    Case("rvv_perf_o2_float_affine", "loop", ("vfmul.vv", "vfadd.vv")),
    Case("rvv_perf_o2_stride4_inplace", "interleave", ("vlse32.v", "vsse32.v")),
    Case("rvv_perf_o2_nonzero_tail", "reject", ()),
    Case("rvv_perf_o2_nested_clamp", "reject", ()),
    Case("rvv_perf_o2_short_control", "reject", ()),
    Case("rvv_perf_o3_slp", "slp", ("vsetivli", "vle32.v", "vse32.v")),
    Case("rvv_perf_o3_slp8", "slp", ("vsetivli", "vle32.v", "vse32.v")),
    Case("rvv_perf_o3_interleave", "interleave", ("vle32.v", "vse32.v")),
    Case("rvv_perf_o3_dual_output", "interleave", ("vadd.vv", "vmul.vv", "vse32.v")),
)


class Failure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise FileNotFoundError(name)
    return path


def run(
    command: list[str], *, input_bytes: bytes | None = None, timeout: float = 90.0
) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise Failure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout.decode(errors='replace')}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result


def vector_plan(source: Path, optimization: int) -> list[dict[str, object]]:
    result = run(
        [
            str(COMPILER),
            str(source),
            "--emit-vector-plan",
            f"-O{optimization}",
            "-march=rv64gcv",
            "-mabi=lp64d",
        ]
    )
    document = json.loads(result.stdout)
    plans = document.get("vectorization_plans")
    require(isinstance(plans, list), f"{source.name}: missing vector plan list")
    return plans


def successful_plan(
    plans: list[dict[str, object]], vectorizer: str
) -> dict[str, object] | None:
    matches = [
        entry
        for entry in plans
        if entry.get("vectorizer") == vectorizer
        and entry.get("function") == "main"
        and entry.get("code") == "VECTORIZED"
    ]
    require(len(matches) <= 1, f"main has duplicate {vectorizer} success plans")
    return matches[0] if matches else None


def disassembled_main(disassembly: str) -> str:
    match = re.search(
        r"(?ms)^\s*[0-9a-f]+ <main>:\n(.*?)(?=^\s*[0-9a-f]+ <[^>]+>:\n|\Z)",
        disassembly,
    )
    require(match is not None, "linked executable has no disassembled main")
    return match.group(1)


def decoded_vector_mnemonics(body: str) -> set[str]:
    return set(
        re.findall(
            r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b",
            body,
        )
    )


def verify_plan_contract(
    case: Case, optimization: int, plans: list[dict[str, object]]
) -> None:
    if case.kind == "explicit":
        return
    loop = successful_plan(plans, "loop")
    slp = successful_plan(plans, "slp")
    if case.kind == "reject":
        require(loop is None, f"{case.name}: O{optimization} unexpectedly vectorized loop")
        require(slp is None, f"{case.name}: O{optimization} unexpectedly vectorized SLP")
        return
    if case.kind == "slp":
        if optimization == 2:
            require(slp is None, f"{case.name}: O2 unexpectedly enabled SLP")
        else:
            require(slp is not None, f"{case.name}: O3 did not vectorize SLP pack")
        return

    require(loop is not None, f"{case.name}: O{optimization} loop was not vectorized")
    if case.kind == "interleave":
        choice = loop.get("plan")
        require(isinstance(choice, dict), f"{case.name}: loop plan has no choice")
        expected = 1 if optimization == 2 else 2
        require(
            choice.get("interleave") == expected,
            f"{case.name}: O{optimization} expected interleave {expected}, "
            f"got {choice.get('interleave')}",
        )


def verify_case(
    case: Case,
    optimization: int,
    work: Path,
    gcc: str,
    objdump: str,
    qemu: str,
) -> dict[str, object]:
    plans = vector_plan(case.source, optimization)
    verify_plan_contract(case, optimization, plans)
    stem = f"{case.name}.O{optimization}"
    assembly = work / f"{stem}.s"
    executable = work / f"{stem}.elf"
    plan_path = work / f"{stem}.vector-plan.json"
    main_disassembly_path = work / f"{stem}.main.objdump"
    plan_path.write_text(json.dumps({"vectorization_plans": plans}, indent=2) + "\n")
    run(
        [
            str(COMPILER),
            str(case.source),
            "-S",
            f"-O{optimization}",
            "-march=rv64gcv",
            "-mabi=lp64d",
            "-o",
            str(assembly),
        ]
    )
    run(
        [
            gcc,
            "-static",
            "-march=rv64gcv",
            "-mabi=lp64d",
            "-mcmodel=medany",
            str(assembly),
            str(RUNTIME),
            "-o",
            str(executable),
        ]
    )
    disassembly = run([objdump, "-d", str(executable)]).stdout.decode()
    main_disassembly = disassembled_main(disassembly)
    main_disassembly_path.write_text(main_disassembly)
    mnemonics = decoded_vector_mnemonics(main_disassembly)
    expects_vector = case.kind != "reject" and (
        case.kind != "slp" or optimization == 3
    )
    if expects_vector:
        require(mnemonics, f"{case.name}: O{optimization} main has no decoded RVV")
        missing = set(case.mnemonics) - mnemonics
        require(
            not missing,
            f"{case.name}: O{optimization} missing decoded mnemonics {sorted(missing)}",
        )
    else:
        require(
            not mnemonics,
            f"{case.name}: O{optimization} scalar control unexpectedly has RVV "
            f"{sorted(mnemonics)}",
        )

    qemu_results: list[dict[str, object]] = []
    for vlen in VLENS:
        result = run(
            [
                qemu,
                "-cpu",
                f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                str(executable),
            ],
            input_bytes=case.input_bytes,
        )
        require(
            result.stdout == b"",
            f"{case.name}: O{optimization} VLEN={vlen} produced unexpected stdout",
        )
        qemu_results.append(
            {
                "vlen": vlen,
                "returncode": result.returncode,
                "stdout": result.stdout.decode(errors="replace"),
                "stderr": result.stderr.decode(errors="replace"),
            }
        )
    print(
        f"PASS {case.name} O{optimization} "
        f"vector={'yes' if expects_vector else 'no'} vlens={len(VLENS)}"
    )
    return {
        "case": case.name,
        "source": str(case.source.relative_to(ROOT)),
        "source_sha256": hashlib.sha256(case.source.read_bytes()).hexdigest(),
        "optimization": optimization,
        "kind": case.kind,
        "expects_vector": expects_vector,
        "decoded_vector_mnemonics": sorted(mnemonics),
        "required_mnemonics": list(case.mnemonics),
        "assembly": assembly.name,
        "assembly_sha256": hashlib.sha256(assembly.read_bytes()).hexdigest(),
        "executable": executable.name,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "main_disassembly": main_disassembly_path.name,
        "vector_plan": plan_path.name,
        "qemu": qemu_results,
    }


def revision() -> str:
    supplied = os.environ.get("YOOLANG_SOURCE_REVISION")
    if supplied:
        return supplied
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.decode().strip() if result.returncode == 0 else "unknown"


def write_reports(work: Path, records: list[dict[str, object]]) -> None:
    document = {
        "schema": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "source_base_revision": revision(),
        "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "compiler": str(COMPILER),
        "march": "rv64gcv",
        "qemu_cpu_template": "rv64,v=true,vlen=<VLEN>,elen=64,vext_spec=v1.0",
        "cases": len(CASES),
        "optimizations": [2, 3],
        "vlens": list(VLENS),
        "executions": len(records) * len(VLENS),
        "results": records,
    }
    (work / "report.json").write_text(json.dumps(document, indent=2) + "\n")

    lines = [
        "# RVV O2/O3 QEMU evidence",
        "",
        f"- Source base revision: `{document['source_base_revision']}`",
        f"- Cases: {document['cases']}",
        f"- QEMU executions: {document['executions']}",
        f"- VLENs: {', '.join(str(value) for value in VLENS)}",
        "- Every row below compiled, linked, decoded, and returned 0 on every VLEN.",
        "",
        "| Case | Opt | RVV expected | Decoded RVV | QEMU return codes (128/256/512/1024) |",
        "|---|---:|:---:|---|---|",
    ]
    for record in records:
        mnemonics = ", ".join(record["decoded_vector_mnemonics"]) or "none"
        returns = "/".join(str(item["returncode"]) for item in record["qemu"])
        lines.append(
            f"| `{record['case']}` | O{record['optimization']} | "
            f"{'yes' if record['expects_vector'] else 'no'} | `{mnemonics}` | `{returns}` |"
        )
    lines.extend(
        [
            "",
            "Each row has matching `.s`, `.elf`, `.main.objdump`, and "
            "`.vector-plan.json` files in this directory. SHA-256 hashes are in `report.json`.",
            "",
        ]
    )
    (work / "report.md").write_text("\n".join(lines))


def make_persistent_work(parent: Path) -> Path:
    parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for suffix in range(1000):
        candidate = parent / f"run-{stamp}-{os.getpid()}-{suffix}"
        try:
            candidate.mkdir()
            return candidate
        except FileExistsError:
            continue
    raise Failure(f"could not create unique evidence directory under {parent}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts-dir",
        type=Path,
        help="preserve evidence in a new run-* child of this directory",
    )
    args = parser.parse_args()
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"required RVV generated-case tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77

    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        require(RUNTIME.is_file(), f"runtime not found: {RUNTIME}")
        for case in CASES:
            require(case.source.is_file(), f"missing generated case: {case.source}")
        records: list[dict[str, object]] = []

        def execute_matrix(work: Path) -> None:
            for optimization in (2, 3):
                for case in CASES:
                    records.append(
                        verify_case(case, optimization, work, gcc, objdump, qemu)
                    )
            write_reports(work, records)

        if args.artifacts_dir is None:
            with tempfile.TemporaryDirectory(prefix="yoolang-rvv-generated-perf-") as temp:
                execute_matrix(Path(temp))
        else:
            work = make_persistent_work(args.artifacts_dir.resolve())
            execute_matrix(work)
            print(f"EVIDENCE {work}")

        executions = len(records) * len(VLENS)
        require(executions == len(CASES) * 2 * len(VLENS), "execution matrix incomplete")
        print(
            f"PASS rvv_performance_generated_matrix cases={len(CASES)} "
            f"executions={executions}"
        )
    except (Failure, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        print(f"FAIL rvv_performance_generated_cases: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
