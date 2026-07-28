#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any


TIME_RE = re.compile(r"^([0-9]+(?:\.[0-9]+)?)s$")
CASE_REGRESSION_DELTA_PCT = 20.0
CASE_REGRESSION_DELTA_SEC = 0.05
CASE_MEASURABLE_DELTA_PCT = 5.0
CASE_MEASURABLE_DELTA_SEC = 0.01
TOTAL_REGRESSION_DELTA_PCT = 10.0
TOTAL_IMPROVEMENT_DELTA_PCT = 10.0
MAX_REGRESSION_ROWS = 10


STATUS_EMOJI = {
    "IMPROVEMENT": "🚀",
    "OK": "✅",
    "REGRESSION": "⚠️",
    "NO_CODE_CHANGE": "✅",
    "NO_DYNAMIC_CHANGE": "✅",
    "INCONCLUSIVE": "ℹ️",
    "NO BASELINE": "ℹ️",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare current perf report against a baseline report.")
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--out-md", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--out-insn-json", type=Path)
    parser.add_argument("--baseline-label", default="main latest successful run")
    parser.add_argument("--baseline-branch", default="")
    parser.add_argument("--baseline-run-id", default="")
    parser.add_argument("--baseline-run-url", default="")
    parser.add_argument("--baseline-commit-sha", default="")
    parser.add_argument("--baseline-commit-time", default="")
    parser.add_argument("--baseline-commit-title", default="")
    parser.add_argument("--baseline-commit-author", default="")
    return parser.parse_args()


def parse_time(cell: Any) -> float | None:
    match = TIME_RE.fullmatch(str(cell).strip())
    if not match:
        return None
    return float(match.group(1))


def rows_by_case(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    rows = payload.get("rows", [])
    if not isinstance(rows, list):
        return {}
    out: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        case = row.get("case")
        if isinstance(case, str):
            out[case] = row
    return out


def compiler_cell(row: dict[str, Any]) -> Any:
    return row.get("compiler", row.get("yoolang"))


def instruction_count(row: dict[str, Any]) -> int | None:
    counts = row.get("instruction_counts")
    if isinstance(counts, dict) and isinstance(counts.get("compiler"), int):
        return counts["compiler"]
    value = row.get("instruction_count")
    return value if isinstance(value, int) else None


def assembly_sha256(row: dict[str, Any]) -> str | None:
    artifacts = row.get("assembly_artifacts")
    if isinstance(artifacts, dict):
        compiler = artifacts.get("compiler")
        if isinstance(compiler, dict):
            value = compiler.get("sha256")
            if isinstance(value, str) and value:
                return value
    value = row.get("compiler_asm_sha256")
    return value if isinstance(value, str) and value else None


def executable_sha256(row: dict[str, Any]) -> str | None:
    artifacts = row.get("assembly_artifacts")
    if not isinstance(artifacts, dict):
        return None
    compiler = artifacts.get("compiler")
    if not isinstance(compiler, dict):
        return None
    value = compiler.get("executable_sha256")
    return value if isinstance(value, str) and value else None


def timing_samples(row: dict[str, Any]) -> list[float]:
    samples_by_compiler = row.get("timing_samples_sec")
    if not isinstance(samples_by_compiler, dict):
        return []
    samples = samples_by_compiler.get("compiler")
    if not isinstance(samples, list):
        return []
    return [float(value) for value in samples if isinstance(value, (int, float)) and value >= 0.0]


def classify_change(
    current_row: dict[str, Any],
    baseline_row: dict[str, Any],
    current_time: float,
    baseline_time: float,
) -> tuple[str, str]:
    current_hash = assembly_sha256(current_row)
    baseline_hash = assembly_sha256(baseline_row)
    current_executable = executable_sha256(current_row)
    baseline_executable = executable_sha256(baseline_row)
    if current_executable is not None and baseline_executable is not None:
        if current_executable == baseline_executable:
            return "NO_CODE_CHANGE", "executed ELF SHA-256 is identical"
    elif current_hash is not None and current_hash == baseline_hash:
        return "NO_CODE_CHANGE", "compiler assembly SHA-256 is identical"

    current_count = instruction_count(current_row)
    baseline_count = instruction_count(baseline_row)
    if current_count is not None and current_count == baseline_count:
        return "NO_DYNAMIC_CHANGE", "QEMU dynamic instruction count is identical"

    delta_sec = current_time - baseline_time
    delta_pct = 0.0 if baseline_time == 0.0 else abs(delta_sec / baseline_time) * 100.0
    if delta_pct < CASE_MEASURABLE_DELTA_PCT or abs(delta_sec) < CASE_MEASURABLE_DELTA_SEC:
        return (
            "INCONCLUSIVE",
            f"wall-time delta is below {CASE_MEASURABLE_DELTA_PCT:.0f}%/{CASE_MEASURABLE_DELTA_SEC:.3f}s evidence floor",
        )

    current_samples = timing_samples(current_row)
    baseline_samples = timing_samples(baseline_row)
    if len(current_samples) >= 2 and len(baseline_samples) >= 2:
        intervals_overlap = min(current_samples) <= max(baseline_samples) and min(baseline_samples) <= max(current_samples)
        if intervals_overlap:
            return "INCONCLUSIVE", "current and baseline timing sample ranges overlap"

    return ("IMPROVEMENT", "measurable wall-time improvement") if current_time < baseline_time else (
        "REGRESSION",
        "measurable wall-time regression",
    )


def format_signed_sec(value: float) -> str:
    return f"{value:+.4f}s"


def format_signed_pct(value: float) -> str:
    return f"{value:+.2f}%"


def speedup_ratio(baseline_time: float, current_time: float) -> float | None:
    if baseline_time <= 0.0 or current_time <= 0.0:
        return None
    return baseline_time / current_time


def format_speedup(value: float | None) -> str:
    return "N/A" if value is None else f"{value:.2f}x"


def geometric_mean(values: list[float]) -> float | None:
    positive_values = [value for value in values if value > 0.0]
    if not positive_values:
        return None
    return math.exp(sum(math.log(value) for value in positive_values) / len(positive_values))


def md_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", "<br>")


def baseline_meta(args: argparse.Namespace) -> dict[str, str]:
    return {
        "branch": args.baseline_branch.strip(),
        "run_id": args.baseline_run_id.strip(),
        "run_url": args.baseline_run_url.strip(),
        "commit_sha": args.baseline_commit_sha.strip(),
        "commit_time": args.baseline_commit_time.strip(),
        "commit_title": args.baseline_commit_title.strip(),
        "commit_author": args.baseline_commit_author.strip(),
    }


def baseline_md_lines(args: argparse.Namespace) -> list[str]:
    meta = baseline_meta(args)
    lines: list[str] = []

    if meta["run_id"]:
        if meta["run_url"]:
            lines.append(f"- Baseline run: [#{md_escape(meta['run_id'])}]({meta['run_url']})")
        else:
            lines.append(f"- Baseline run: #{meta['run_id']}")

    if meta["commit_sha"]:
        commit_line = f"- Baseline commit: `{meta['commit_sha'][:12]}`"
        if meta["commit_title"]:
            commit_line += f" {md_escape(meta['commit_title'])}"
        lines.append(commit_line)
    elif meta["commit_title"]:
        lines.append(f"- Baseline commit: {md_escape(meta['commit_title'])}")

    if meta["commit_author"]:
        lines.append(f"- Baseline author: {md_escape(meta['commit_author'])}")

    return lines


def write_no_baseline(args: argparse.Namespace, reason: str) -> None:
    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "status": "NO BASELINE",
        "status_emoji": STATUS_EMOJI["NO BASELINE"],
        "baseline": args.baseline_label,
        "baseline_branch": args.baseline_branch,
        "baseline_run_id": args.baseline_run_id,
        "baseline_run_url": args.baseline_run_url,
        "baseline_commit_sha": args.baseline_commit_sha,
        "baseline_commit_time": args.baseline_commit_time,
        "baseline_commit_title": args.baseline_commit_title,
        "baseline_commit_author": args.baseline_commit_author,
        "baseline_meta": baseline_meta(args),
        "reason": reason,
        "comparable_cases": 0,
        "case_speedup_geomean": None,
        "case_wins": 0,
        "case_losses": 0,
        "case_ties": 0,
        "case_no_code_change": 0,
        "case_no_dynamic_change": 0,
        "case_inconclusive": 0,
        "rows": [],
    }
    args.out_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    if args.out_insn_json:
        args.out_insn_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_insn_json.write_text(
            json.dumps(
                {
                    "status": "NO BASELINE",
                    "reason": reason,
                    "current_total_instructions": None,
                    "baseline_total_instructions": None,
                    "rows": [],
                },
                ensure_ascii=True,
                indent=2,
            )
            + "\n"
        )
    args.out_md.write_text(
        "\n".join(
            [
                "## 📊 Perf Change vs main baseline",
                "",
                f"- Status: {STATUS_EMOJI['NO BASELINE']} NO BASELINE",
                f"- Baseline: {args.baseline_label}",
                *baseline_md_lines(args),
                f"- Reason: {reason}",
                "",
            ]
        )
    )


def compare_instruction_counts(
    args: argparse.Namespace,
    current_rows: dict[str, dict[str, Any]],
    baseline_rows: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    current_total = 0
    baseline_total = 0
    wins = 0
    losses = 0
    ties = 0
    failed = 0

    comparable_cases = sorted(set(current_rows) & set(baseline_rows))
    for case in comparable_cases:
        current_count = instruction_count(current_rows[case])
        baseline_count = instruction_count(baseline_rows[case])
        if current_count is None or baseline_count is None:
            failed += 1
            rows.append(
                {
                    "case": case,
                    "current": current_count,
                    "baseline": baseline_count,
                    "status": "FAILED",
                    "reason": "missing current or baseline instruction count",
                }
            )
            continue
        delta = current_count - baseline_count
        delta_pct = 0.0 if baseline_count == 0 else (delta / baseline_count) * 100.0
        speedup = (baseline_count / current_count) if current_count > 0 else None
        if current_count < baseline_count:
            wins += 1
            row_status = "WIN"
        elif current_count > baseline_count:
            losses += 1
            row_status = "LOSS"
        else:
            ties += 1
            row_status = "TIE"
        current_total += current_count
        baseline_total += baseline_count
        rows.append(
            {
                "case": case,
                "current": current_count,
                "baseline": baseline_count,
                "delta": delta,
                "delta_pct": delta_pct,
                "speedup": speedup,
                "status": row_status,
            }
        )

    if not comparable_cases:
        status = "SKIPPED"
        reason = "no comparable cases"
    elif current_total == 0 or baseline_total == 0:
        status = "SKIPPED" if failed else "OK"
        reason = "no comparable instruction counts" if failed else ""
    else:
        status = "OK"
        reason = ""

    total_delta = current_total - baseline_total
    total_delta_pct = 0.0 if baseline_total == 0 else (total_delta / baseline_total) * 100.0
    total_speedup = (baseline_total / current_total) if current_total > 0 else None
    payload: dict[str, Any] = {
        "status": status,
        "reason": reason,
        "baseline": args.baseline_label,
        "baseline_branch": args.baseline_branch,
        "baseline_commit_sha": args.baseline_commit_sha,
        "baseline_commit_time": args.baseline_commit_time,
        "baseline_commit_title": args.baseline_commit_title,
        "baseline_commit_author": args.baseline_commit_author,
        "comparable_cases": wins + losses + ties,
        "current_total_instructions": current_total if wins + losses + ties else None,
        "baseline_total_instructions": baseline_total if wins + losses + ties else None,
        "total_delta": total_delta if wins + losses + ties else None,
        "total_delta_pct": total_delta_pct if wins + losses + ties else None,
        "total_speedup": total_speedup,
        "case_wins": wins,
        "case_losses": losses,
        "case_ties": ties,
        "case_failed": failed,
        "rows": rows,
    }
    if args.out_insn_json:
        args.out_insn_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_insn_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    return payload


def main() -> int:
    args = parse_args()

    try:
        current = json.loads(args.current.read_text())
    except Exception as exc:
        write_no_baseline(args, f"current perf report unavailable: {exc}")
        return 0

    try:
        baseline = json.loads(args.baseline.read_text())
    except Exception as exc:
        write_no_baseline(args, f"baseline perf report unavailable: {exc}")
        return 0

    current_rows = rows_by_case(current)
    baseline_rows = rows_by_case(baseline)
    insn_compare = compare_instruction_counts(args, current_rows, baseline_rows)

    rows: list[dict[str, Any]] = []
    current_total = 0.0
    baseline_total = 0.0
    speedups: list[float] = []
    case_wins = 0
    case_losses = 0
    case_ties = 0
    case_no_code_change = 0
    case_no_dynamic_change = 0
    case_inconclusive = 0
    adjudicated_cases = 0
    raw_current_total = 0.0

    for case in sorted(set(current_rows) & set(baseline_rows)):
        current_time = parse_time(compiler_cell(current_rows[case]))
        baseline_time = parse_time(compiler_cell(baseline_rows[case]))
        if current_time is None or baseline_time is None:
            continue

        observed_delta_sec = current_time - baseline_time
        # Speed-based: positive = yoolang faster (comparing speed, not time)
        observed_delta_pct = (
            0.0 if baseline_time == 0.0 else ((baseline_time - current_time) / baseline_time) * 100.0
        )
        classification, evidence = classify_change(
            current_rows[case], baseline_rows[case], current_time, baseline_time
        )
        if classification == "IMPROVEMENT":
            case_wins += 1
            adjudicated_cases += 1
        elif classification == "REGRESSION":
            case_losses += 1
            adjudicated_cases += 1
        elif classification == "NO_CODE_CHANGE":
            case_no_code_change += 1
            case_ties += 1
        elif classification == "NO_DYNAMIC_CHANGE":
            case_no_dynamic_change += 1
            case_ties += 1
        else:
            case_inconclusive += 1
            case_ties += 1

        is_measurable = classification in {"IMPROVEMENT", "REGRESSION"}
        effective_current_time = current_time if is_measurable else baseline_time
        delta_sec = effective_current_time - baseline_time
        delta_pct = 0.0 if not is_measurable else observed_delta_pct
        case_speedup = speedup_ratio(baseline_time, effective_current_time)
        if case_speedup is not None:
            speedups.append(case_speedup)
        is_regression = (
            classification == "REGRESSION"
            and observed_delta_pct <= -CASE_REGRESSION_DELTA_PCT
            and observed_delta_sec >= CASE_REGRESSION_DELTA_SEC
        )
        rows.append(
            {
                "case": case,
                "current": current_time,
                "baseline": baseline_time,
                "delta_sec": delta_sec,
                "delta_pct": delta_pct,
                "observed_delta_sec": observed_delta_sec,
                "observed_delta_pct": observed_delta_pct,
                "speedup": case_speedup,
                "status": classification,
                "evidence": evidence,
                "important_regression": is_regression,
                "assembly_changed": (
                    assembly_sha256(current_rows[case]) != assembly_sha256(baseline_rows[case])
                    if assembly_sha256(current_rows[case]) is not None
                    and assembly_sha256(baseline_rows[case]) is not None
                    else None
                ),
            }
        )
        current_total += effective_current_time
        baseline_total += baseline_time
        raw_current_total += current_time

    if not rows:
        status = "NO BASELINE"
        total_delta_sec = 0.0
        total_delta_pct = 0.0
        case_speedup_geomean = None
    else:
        total_delta_sec = current_total - baseline_total
        total_delta_pct = 0.0 if baseline_total == 0.0 else ((baseline_total - current_total) / baseline_total) * 100.0
        total_speedup = speedup_ratio(baseline_total, current_total)
        case_speedup_geomean = geometric_mean(speedups)
        if adjudicated_cases == 0 and case_no_code_change == len(rows):
            status = "NO_CODE_CHANGE"
        elif adjudicated_cases == 0 and case_inconclusive == 0:
            status = "NO_DYNAMIC_CHANGE"
        elif adjudicated_cases == 0:
            status = "INCONCLUSIVE"
        elif total_delta_pct <= -TOTAL_REGRESSION_DELTA_PCT:
            status = "REGRESSION"
        elif total_delta_pct >= TOTAL_IMPROVEMENT_DELTA_PCT:
            status = "IMPROVEMENT"
        else:
            status = "OK"
    if not rows:
        total_speedup = None

    regressions = [row for row in rows if row["important_regression"]]
    regressions.sort(key=lambda row: (row["observed_delta_pct"], -row["observed_delta_sec"]))
    shown_regressions = regressions[:MAX_REGRESSION_ROWS]

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)

    md_lines = [
        "## 📊 Perf Change vs main baseline",
        "",
        f"- Status: {STATUS_EMOJI.get(status, 'ℹ️')} {status}",
        f"- Baseline: {args.baseline_label}",
        *baseline_md_lines(args),
        f"- Comparable cases: {len(rows)}",
        f"- Evidence-backed changes: {adjudicated_cases}",
        f"- No code change: {case_no_code_change}",
        f"- No dynamic instruction change: {case_no_dynamic_change}",
        f"- Inconclusive timing: {case_inconclusive}",
        f"- Current Yoolang runtime total: {current_total:.4f}s",
        f"- Baseline Yoolang runtime total: {baseline_total:.4f}s",
        f"- Raw current runtime total (diagnostic only): {raw_current_total:.4f}s",
        f"- Overall speedup vs baseline: {format_speedup(total_speedup)}",
        f"- Win/loss/neutral: 🚀 {case_wins} faster / ⚠️ {case_losses} slower / ✅ {case_ties} neutral",
        f"- Delta: {format_signed_sec(total_delta_sec)} ({format_signed_pct(total_delta_pct)})",
        "",
    ]

    if insn_compare.get("status") == "OK":
        current_insn = insn_compare.get("current_total_instructions")
        baseline_insn = insn_compare.get("baseline_total_instructions")
        total_speedup_insn = insn_compare.get("total_speedup")
        total_delta_pct_insn = insn_compare.get("total_delta_pct")
        md_lines.extend(
            [
                "## 🧮 QEMU Dynamic Instruction Count",
                "",
                f"- Current total instructions: {current_insn}",
                f"- Baseline total instructions: {baseline_insn}",
                f"- Overall speedup vs baseline: {format_speedup(total_speedup_insn if isinstance(total_speedup_insn, (int, float)) else None)}",
                f"- Delta: {format_signed_pct(total_delta_pct_insn if isinstance(total_delta_pct_insn, (int, float)) else 0.0)}",
                (
                    "- Case win/loss/tie: "
                    f"🚀 {insn_compare.get('case_wins', 0)} fewer / "
                    f"⚠️ {insn_compare.get('case_losses', 0)} more / "
                    f"✅ {insn_compare.get('case_ties', 0)} tied / "
                    f"❌ {insn_compare.get('case_failed', 0)} failed"
                ),
                "",
            ]
        )
    else:
        md_lines.extend(
            [
                "## 🧮 QEMU Dynamic Instruction Count",
                "",
                f"- Status: ⚠️ {insn_compare.get('status', 'SKIPPED')}",
                f"- Reason: {insn_compare.get('reason', 'unknown')}",
                "",
            ]
        )

    if shown_regressions:
        md_lines.extend(
            [
                "| Case | Current | Baseline | Speedup | Delta | Status |",
                "| --- | --- | --- | --- | --- | --- |",
            ]
        )
        for row in shown_regressions:
            md_lines.append(
                "| "
                + " | ".join(
                    [
                        md_escape(str(row["case"])),
                        f"{row['current']:.4f}s",
                        f"{row['baseline']:.4f}s",
                        format_speedup(row["speedup"]),
                        f"{format_signed_sec(row['delta_sec'])} ({format_signed_pct(row['delta_pct'])})",
                        str(row["status"]),
                    ]
                )
                + " |"
            )
    else:
        md_lines.append("✅ No significant per-case regressions.")

    payload = {
        "status": status,
        "status_emoji": STATUS_EMOJI.get(status, "ℹ️"),
        "baseline": args.baseline_label,
        "baseline_branch": args.baseline_branch,
        "baseline_run_id": args.baseline_run_id,
        "baseline_run_url": args.baseline_run_url,
        "baseline_commit_sha": args.baseline_commit_sha,
        "baseline_commit_time": args.baseline_commit_time,
        "baseline_commit_title": args.baseline_commit_title,
        "baseline_commit_author": args.baseline_commit_author,
        "baseline_meta": baseline_meta(args),
        "comparable_cases": len(rows),
        "adjudicated_cases": adjudicated_cases,
        "current_compiler_total": current_total,
        "baseline_compiler_total": baseline_total,
        "raw_current_compiler_total": raw_current_total,
        "total_delta_sec": total_delta_sec,
        "total_delta_pct": total_delta_pct,
        "total_speedup": total_speedup,
        "case_speedup_geomean": case_speedup_geomean,
        "case_wins": case_wins,
        "case_losses": case_losses,
        "case_ties": case_ties,
        "case_no_code_change": case_no_code_change,
        "case_no_dynamic_change": case_no_dynamic_change,
        "case_inconclusive": case_inconclusive,
        "instruction_count": insn_compare,
        "regressions": regressions,
        "rows": rows,
    }
    args.out_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    args.out_md.write_text("\n".join(md_lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
