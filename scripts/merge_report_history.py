#!/usr/bin/env python3
"""Merge remote report-history.json into local copy, deduplicating by run_id."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <local-public-dir> <remote-history-json>", file=sys.stderr)
        return 2

    local = Path(sys.argv[1]) / "report-history.json"
    remote = Path(sys.argv[2])

    if not local.exists() or not remote.exists():
        return 0

    try:
        lr = json.loads(local.read_text()).get("runs", [])
        rr = json.loads(remote.read_text()).get("runs", [])
    except Exception:
        return 0

    ids = {r["run_id"] for r in lr if isinstance(r, dict)}
    new_entries = [r for r in rr if isinstance(r, dict) and r.get("run_id") not in ids]
    if not new_entries:
        return 0

    merged = lr + new_entries
    local.write_text(json.dumps({"runs": merged}, ensure_ascii=True, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
