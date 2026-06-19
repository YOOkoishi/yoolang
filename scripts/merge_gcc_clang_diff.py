#!/usr/bin/env python3
"""
Merge gcc-clang-diff JSON output into perf-report.json.

Usage:
    python merge_gcc_clang_diff.py --perf-report build/perf-ci/perf-report.json \
                                   --diff-json build/gcc-clang-diff/summary.json \
                                   --out build/perf-ci/perf-report.json
"""

import argparse
import json
import sys
from pathlib import Path


def merge(perf_report_path: Path, diff_json_path: Path, out_path: Path) -> None:
    if not perf_report_path.exists():
        print(f"perf-report.json not found: {perf_report_path}", file=sys.stderr)
        sys.exit(1)
    if not diff_json_path.exists():
        print(f"diff JSON not found: {diff_json_path}", file=sys.stderr)
        sys.exit(1)

    perf = json.loads(perf_report_path.read_text())
    diffs = json.loads(diff_json_path.read_text())

    # Compute aggregate stats from diff results
    successes = [d for d in diffs if "error" not in d]
    total_yoo = sum(d["yoolang"]["total"] for d in successes)
    total_gcc = sum(d["gcc"]["total"] for d in successes)
    total_clang = sum(d["clang"]["total"] for d in successes)

    static_insn_summary = {
        "status": "OK" if successes else "EMPTY",
        "cases": len(diffs),
        "successful_cases": len(successes),
        "yoolang_total_static_insns": total_yoo,
        "gcc_total_static_insns": total_gcc,
        "clang_total_static_insns": total_clang,
        "gcc_static_ratio": round(total_yoo / total_gcc, 4) if total_gcc > 0 else None,
        "clang_static_ratio": round(total_yoo / total_clang, 4) if total_clang > 0 else None,
    }

    perf["static_instruction_diff"] = static_insn_summary

    # Also add per-case static diff data to rows
    diff_by_case = {d["case"]: d for d in successes}
    for row in perf.get("rows", []):
        case_name = Path(row.get("case", "")).stem
        if case_name in diff_by_case:
            row["static_insn_diff"] = diff_by_case[case_name]

    out_path.write_text(json.dumps(perf, ensure_ascii=True, indent=2) + "\n")
    print(f"Merged static instruction diff into {out_path}")
    print(f"  {len(successes)}/{len(diffs)} cases, "
          f"yoolang={total_yoo}, gcc={total_gcc}, clang={total_clang}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge gcc-clang-diff into perf-report")
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--diff-json", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    merge(args.perf_report, args.diff_json, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
