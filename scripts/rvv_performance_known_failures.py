#!/usr/bin/env python3

"""Reproduce and preserve evidence for current RVV known failures.

This command succeeds only when every documented regression is reproduced and
every control build still returns zero.  It does not relabel regressions as
passing correctness tests.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
CASES_DIR = ROOT / "test/rvv_performance_generated/known_failures"
VLENS = (128, 256, 512, 1024)
CRASH_CASES = (
    "explicit_masked_memory_optimized_oir_crash",
    "explicit_indexed_mask_optimized_oir_crash",
)
WRONG_RESULT_CASES = (
    "stride2_load_contiguous_store_wrong_result",
    "stride2_store_wrong_result",
)


class Failure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise Failure(f"required tool is unavailable: {name}")
    return path


def invoke(
    command: list[str], *, input_bytes: bytes | None = None, timeout: float = 90.0
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        command,
        cwd=ROOT,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=timeout,
    )


def checked(command: list[str]) -> subprocess.CompletedProcess[bytes]:
    result = invoke(command)
    if result.returncode != 0:
        raise Failure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout.decode(errors='replace')}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result


def main_body(disassembly: str) -> str:
    match = re.search(
        r"(?ms)^\s*[0-9a-f]+ <main>:\n(.*?)(?=^\s*[0-9a-f]+ <[^>]+>:\n|\Z)",
        disassembly,
    )
    require(match is not None, "linked executable has no decoded main")
    return match.group(1)


def compile_elf(
    source: Path,
    stem: str,
    optimization: int,
    march: str,
    work: Path,
    gcc: str,
    objdump: str,
) -> tuple[Path, str]:
    assembly = work / f"{stem}.s"
    executable = work / f"{stem}.elf"
    result = invoke(
        [
            str(COMPILER),
            str(source),
            "-S",
            f"-O{optimization}",
            f"-march={march}",
            "-mabi=lp64d",
            "-o",
            str(assembly),
        ]
    )
    require(
        result.returncode == 0,
        f"{source.name}: control compilation failed at O{optimization} {march}: "
        f"{result.stderr.decode(errors='replace')}",
    )
    checked(
        [
            gcc,
            "-static",
            f"-march={march}",
            "-mabi=lp64d",
            "-mcmodel=medany",
            str(assembly),
            str(RUNTIME),
            "-o",
            str(executable),
        ]
    )
    disassembly = checked([objdump, "-d", str(executable)]).stdout.decode()
    body = main_body(disassembly)
    (work / f"{stem}.main.objdump").write_text(body)
    return executable, body


def qemu_matrix(
    executable: Path,
    qemu: str,
    input_bytes: bytes,
    expected: str | tuple[int, int, int, int],
) -> list[dict[str, object]]:
    records = []
    for vlen in VLENS:
        result = invoke(
            [
                qemu,
                "-cpu",
                f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                str(executable),
            ],
            input_bytes=input_bytes,
        )
        if expected == "zero":
            require(
                result.returncode == 0,
                f"{executable.name} VLEN={vlen}: expected 0, "
                f"got {result.returncode}",
            )
        else:
            expected_code = expected[len(records)]
            require(
                result.returncode == expected_code,
                f"{executable.name} VLEN={vlen}: expected diagnostic return "
                f"{expected_code}, got {result.returncode}",
            )
        require(
            result.stdout == b"",
            f"{executable.name} VLEN={vlen}: unexpected stdout",
        )
        records.append(
            {
                "vlen": vlen,
                "returncode": result.returncode,
                "stdout": result.stdout.decode(errors="replace"),
                "stderr": result.stderr.decode(errors="replace"),
            }
        )
    return records


def reproduce_crashes(
    work: Path, gcc: str, objdump: str, qemu: str
) -> list[dict[str, object]]:
    records = []
    for name in CRASH_CASES:
        source = CASES_DIR / f"{name}.sy"
        stem = f"{name}.O0.rv64gcv"
        executable, _ = compile_elf(source, stem, 0, "rv64gcv", work, gcc, objdump)
        controls = qemu_matrix(executable, qemu, b"", "zero")
        optimized = []
        for optimization in (1, 2, 3):
            result = invoke(
                [
                    str(COMPILER),
                    str(source),
                    "--emit-oir",
                    f"-O{optimization}",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                ]
            )
            require(
                result.returncode in (-11, 139),
                f"{name}: O{optimization} expected SIGSEGV, got return code "
                f"{result.returncode}",
            )
            stderr_name = f"{name}.O{optimization}.compiler.stderr"
            (work / stderr_name).write_bytes(result.stderr)
            optimized.append(
                {
                    "optimization": optimization,
                    "returncode": result.returncode,
                    "signal": -result.returncode if result.returncode < 0 else None,
                    "stderr": stderr_name,
                }
            )
        print(f"XFAIL {name} optimized-OIR crash reproduced; O0 controls=4/4")
        records.append(
            {
                "case": name,
                "source": str(source.relative_to(ROOT)),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "class": "optimized_oir_crash",
                "o0_qemu_controls": controls,
                "optimized_compiler_results": optimized,
            }
        )
    return records


def reproduce_wrong_results(
    work: Path, gcc: str, objdump: str, qemu: str
) -> list[dict[str, object]]:
    records = []
    for name in WRONG_RESULT_CASES:
        source = CASES_DIR / f"{name}.sy"
        input_bytes = (CASES_DIR / f"{name}.in").read_bytes()
        configurations = []
        wrong_codes = (
            (11, 11, 11, 11)
            if name.startswith("stride2_load_")
            else (18, 19, 19, 19)
        )
        for optimization, march, expected in (
            (0, "rv64gcv", "zero"),
            (1, "rv64gcv", "zero"),
            (2, "rv64gc", "zero"),
            (3, "rv64gc", "zero"),
            (2, "rv64gcv", wrong_codes),
            (3, "rv64gcv", wrong_codes),
        ):
            stem = f"{name}.O{optimization}.{march}"
            executable, body = compile_elf(
                source, stem, optimization, march, work, gcc, objdump
            )
            results = qemu_matrix(executable, qemu, input_bytes, expected)
            is_known_failure = expected != "zero"
            if is_known_failure:
                require(
                    "vle32.v" in body and "vlse32.v" not in body,
                    f"{stem}: expected misclassified unit-stride load evidence",
                )
            configurations.append(
                {
                    "optimization": optimization,
                    "march": march,
                    "expected_return": expected,
                    "is_known_failure": is_known_failure,
                    "qemu": results,
                    "assembly": f"{stem}.s",
                    "assembly_sha256": hashlib.sha256(
                        (work / f"{stem}.s").read_bytes()
                    ).hexdigest(),
                    "executable": f"{stem}.elf",
                    "main_disassembly": f"{stem}.main.objdump",
                }
            )
        print(
            f"XFAIL {name} RVV O2/O3 wrong result reproduced; "
            "scalar/O0/O1 controls=16/16"
        )
        records.append(
            {
                "case": name,
                "source": str(source.relative_to(ROOT)),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "class": "stride2_wrong_result",
                "configurations": configurations,
            }
        )
    return records


def write_report(work: Path, records: list[dict[str, object]]) -> None:
    source_revision = os.environ.get("YOOLANG_SOURCE_REVISION")
    if source_revision is None:
        source_revision = checked(["git", "rev-parse", "HEAD"]).stdout.decode().strip()
    document = {
        "schema": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "source_base_revision": source_revision,
        "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "status": "known failures reproduced",
        "known_failures": len(records),
        "results": records,
    }
    (work / "report.json").write_text(json.dumps(document, indent=2) + "\n")
    lines = [
        "# RVV known-failure evidence",
        "",
        f"Source base revision: `{document['source_base_revision']}`",
        "",
        "This is an XFAIL reproduction report, not a passing correctness claim.",
        "All O0/O1/scalar controls returned 0; the optimized configurations "
        "failed exactly at the documented boundary.",
        "",
        "| Case | Observed failure | Controls |",
        "|---|---|---|",
    ]
    for record in records:
        if record["class"] == "optimized_oir_crash":
            returns = "/".join(
                str(item["returncode"])
                for item in record["optimized_compiler_results"]
            )
            lines.append(
                f"| `{record['case']}` | O1/O2/O3 compiler returns `{returns}` | "
                "O0 QEMU: 4/4 return 0 |"
            )
        else:
            bad = [item for item in record["configurations"] if item["is_known_failure"]]
            returns = "; ".join(
                f"O{item['optimization']}="
                + "/".join(str(run["returncode"]) for run in item["qemu"])
                for item in bad
            )
            lines.append(
                f"| `{record['case']}` | RVV `{returns}` | "
                "RVV O0/O1 + scalar O2/O3: 16/16 return 0 |"
            )
    lines.extend(
        [
            "",
            "Configurations, return codes, assembly hashes, and artifact names "
            "are in `report.json`.",
            "",
        ]
    )
    (work / "report.md").write_text("\n".join(lines))


def make_work(parent: Path) -> Path:
    parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for suffix in range(1000):
        candidate = parent / f"run-{stamp}-{os.getpid()}-{suffix}"
        try:
            candidate.mkdir()
            return candidate
        except FileExistsError:
            continue
    raise Failure(f"could not create evidence directory under {parent}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        require(RUNTIME.is_file(), f"runtime not found: {RUNTIME}")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        qemu = require_tool("qemu-riscv64")
        work = make_work(args.artifacts_dir.resolve())
        records = reproduce_crashes(work, gcc, objdump, qemu)
        records.extend(reproduce_wrong_results(work, gcc, objdump, qemu))
        write_report(work, records)
        print(f"KNOWN-FAIL reproduced={len(records)}")
        print(f"EVIDENCE {work}")
        return 0
    except (Failure, subprocess.TimeoutExpired) as error:
        print(f"FAIL known-failure probe: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
