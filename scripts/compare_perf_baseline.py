#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


TIME_RE = re.compile(r"^([0-9]+(?:\.[0-9]+)?)s$")
CASE_REGRESSION_DELTA_PCT = 20.0
CASE_REGRESSION_DELTA_SEC = 0.05
TOTAL_REGRESSION_DELTA_PCT = 10.0
TOTAL_IMPROVEMENT_DELTA_PCT = -10.0
MAX_REGRESSION_ROWS = 10


STATUS_EMOJI = {
    "IMPROVEMENT": "🚀",
    "OK": "✅",
    "REGRESSION": "⚠️",
    "NO BASELINE": "ℹ️",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare current perf report against a baseline report.")
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--out-md", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--baseline-label", default="main latest successful run")
    parser.add_argument("--baseline-branch", default="")
    parser.add_argument("--baseline-run-id", default="")
    parser.add_argument("--baseline-run-url", default="")
    parser.add_argument("--baseline-commit-sha", default="")
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


def format_signed_sec(value: float) -> str:
    return f"{value:+.4f}s"


def format_signed_pct(value: float) -> str:
    return f"{value:+.2f}%"


def md_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", "<br>")


def baseline_meta(args: argparse.Namespace) -> dict[str, str]:
    return {
        "branch": args.baseline_branch.strip(),
        "run_id": args.baseline_run_id.strip(),
        "run_url": args.baseline_run_url.strip(),
        "commit_sha": args.baseline_commit_sha.strip(),
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
        "baseline_commit_title": args.baseline_commit_title,
        "baseline_commit_author": args.baseline_commit_author,
        "baseline_meta": baseline_meta(args),
        "reason": reason,
        "comparable_cases": 0,
        "rows": [],
    }
    args.out_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
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

    rows: list[dict[str, Any]] = []
    current_total = 0.0
    baseline_total = 0.0

    for case in sorted(set(current_rows) & set(baseline_rows)):
        current_time = parse_time(compiler_cell(current_rows[case]))
        baseline_time = parse_time(compiler_cell(baseline_rows[case]))
        if current_time is None or baseline_time is None:
            continue

        delta_sec = current_time - baseline_time
        delta_pct = 0.0 if baseline_time == 0.0 else (delta_sec / baseline_time) * 100.0
        is_regression = delta_pct >= CASE_REGRESSION_DELTA_PCT and delta_sec >= CASE_REGRESSION_DELTA_SEC
        rows.append(
            {
                "case": case,
                "current": current_time,
                "baseline": baseline_time,
                "delta_sec": delta_sec,
                "delta_pct": delta_pct,
                "status": "REGRESSION" if is_regression else "OK",
            }
        )
        current_total += current_time
        baseline_total += baseline_time

    if not rows:
        status = "NO BASELINE"
        total_delta_sec = 0.0
        total_delta_pct = 0.0
    else:
        total_delta_sec = current_total - baseline_total
        total_delta_pct = 0.0 if baseline_total == 0.0 else (total_delta_sec / baseline_total) * 100.0
        if total_delta_pct >= TOTAL_REGRESSION_DELTA_PCT:
            status = "REGRESSION"
        elif total_delta_pct <= TOTAL_IMPROVEMENT_DELTA_PCT:
            status = "IMPROVEMENT"
        else:
            status = "OK"

    regressions = [row for row in rows if row["status"] == "REGRESSION"]
    regressions.sort(key=lambda row: (row["delta_pct"], row["delta_sec"]), reverse=True)
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
        f"- Current compiler total: {current_total:.4f}s",
        f"- Baseline compiler total: {baseline_total:.4f}s",
        f"- Delta: {format_signed_sec(total_delta_sec)} ({format_signed_pct(total_delta_pct)})",
        "",
    ]

    if shown_regressions:
        md_lines.extend(
            [
                "| Case | Current | Baseline | Delta | Status |",
                "| --- | --- | --- | --- | --- |",
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
        "baseline_commit_title": args.baseline_commit_title,
        "baseline_commit_author": args.baseline_commit_author,
        "baseline_meta": baseline_meta(args),
        "comparable_cases": len(rows),
        "current_compiler_total": current_total,
        "baseline_compiler_total": baseline_total,
        "total_delta_sec": total_delta_sec,
        "total_delta_pct": total_delta_pct,
        "regressions": regressions,
        "rows": rows,
    }
    args.out_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    args.out_md.write_text("\n".join(md_lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
