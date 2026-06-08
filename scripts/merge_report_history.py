#!/usr/bin/env python3
"""Merge remote report-history/report-index JSON files into local copies."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in {3, 4}:
        print(f"usage: {sys.argv[0]} <local-public-dir> <remote-history-json> [remote-report-index-json]", file=sys.stderr)
        return 2

    local_public = Path(sys.argv[1])
    remote_history = Path(sys.argv[2])
    remote_index = Path(sys.argv[3]) if len(sys.argv) == 4 else None
    merge_history(local_public / "report-history.json", remote_history)
    if remote_index is not None:
        merge_index(local_public / "report-index.json", remote_index)
    return 0


def merge_history(local: Path, remote: Path) -> None:
    if not local.exists() or not remote.exists():
        return

    try:
        lr = json.loads(local.read_text()).get("runs", [])
        rr = json.loads(remote.read_text()).get("runs", [])
    except Exception:
        return

    ids = {r["run_id"] for r in lr if isinstance(r, dict)}
    new_entries = [r for r in rr if isinstance(r, dict) and r.get("run_id") not in ids]
    if not new_entries:
        return

    merged = lr + new_entries
    local.write_text(json.dumps({"runs": merged}, ensure_ascii=True, indent=2) + "\n")


def merge_index(local: Path, remote: Path) -> None:
    if not remote.exists():
        return

    try:
        local_rows = json.loads(local.read_text()).get("reports", []) if local.exists() else []
        remote_rows = json.loads(remote.read_text()).get("reports", [])
    except Exception:
        return

    keys = {
        str(row.get("id") or row.get("perf_report_url") or "")
        for row in local_rows
        if isinstance(row, dict)
    }
    new_entries = []
    for row in remote_rows:
        if not isinstance(row, dict):
            continue
        key = str(row.get("id") or row.get("perf_report_url") or "")
        if not key or key in keys:
            continue
        keys.add(key)
        new_entries.append(row)
    if not new_entries:
        return

    merged = local_rows + new_entries
    local.write_text(json.dumps({"reports": merged}, ensure_ascii=True, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
