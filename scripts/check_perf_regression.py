#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


NON_BLOCKING_STATUSES = {
    "IMPROVEMENT",
    "OK",
    "NO_CODE_CHANGE",
    "NO_DYNAMIC_CHANGE",
    "INCONCLUSIVE",
    "NO BASELINE",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fail when a baseline comparison contains an evidence-backed performance regression."
    )
    parser.add_argument("--delta", required=True, type=Path)
    return parser.parse_args()


def regression_reasons(payload: dict[str, Any]) -> list[str]:
    reasons: list[str] = []
    status = str(payload.get("status", "")).strip()
    if status == "REGRESSION":
        delta_pct = payload.get("total_delta_pct")
        if isinstance(delta_pct, (int, float)):
            reasons.append(f"overall evidence-backed runtime delta is {delta_pct:+.2f}%")
        else:
            reasons.append("overall evidence-backed runtime is classified as REGRESSION")
    elif status not in NON_BLOCKING_STATUSES:
        reasons.append(f"unrecognized performance comparison status: {status or '<missing>'}")

    rows = payload.get("regressions", [])
    if not isinstance(rows, list):
        reasons.append("performance comparison regressions field is invalid")
        return reasons

    for row in rows:
        if not isinstance(row, dict):
            reasons.append("performance comparison contains an invalid regression row")
            continue
        case = str(row.get("case", "<unknown>"))
        delta_pct = row.get("observed_delta_pct", row.get("delta_pct"))
        delta_sec = row.get("observed_delta_sec", row.get("delta_sec"))
        detail = []
        if isinstance(delta_pct, (int, float)):
            detail.append(f"{delta_pct:+.2f}%")
        if isinstance(delta_sec, (int, float)):
            detail.append(f"{delta_sec:+.4f}s")
        suffix = f" ({', '.join(detail)})" if detail else ""
        reasons.append(f"important per-case regression: {case}{suffix}")

    return reasons


def main() -> int:
    args = parse_args()
    try:
        payload = json.loads(args.delta.read_text())
    except Exception as exc:
        print(f"Performance delta report is unavailable or invalid: {exc}")
        return 2

    if not isinstance(payload, dict):
        print("Performance delta report root must be a JSON object")
        return 2

    reasons = regression_reasons(payload)
    if reasons:
        print("Performance regression gate failed:")
        for reason in reasons:
            print(f"- {reason}")
        return 1

    print(f"Performance regression gate passed: {payload.get('status', 'UNKNOWN')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
