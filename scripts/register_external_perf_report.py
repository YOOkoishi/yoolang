#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo


CHINA_TZ = ZoneInfo("Asia/Shanghai")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Register an external perf report for static comparison pages.")
    parser.add_argument("--public-dir", required=True, type=Path)
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--branch", default="")
    parser.add_argument("--commit-title", default="")
    parser.add_argument("--source-url", default="")
    return parser.parse_args()


def load_index(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    try:
        payload = json.loads(path.read_text(errors="replace"))
    except Exception:
        return []
    rows = payload.get("reports") if isinstance(payload, dict) else payload
    return [row for row in rows if isinstance(row, dict)] if isinstance(rows, list) else []


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "-" for ch in value).strip("-") or "external"


def main() -> int:
    args = parse_args()
    public_dir = args.public_dir
    public_dir.mkdir(parents=True, exist_ok=True)
    short_sha = args.commit_sha[:12] if args.commit_sha else "unknown"
    name = safe_name(args.name)

    report_dir = public_dir / "external" / name / short_sha
    report_dir.mkdir(parents=True, exist_ok=True)
    target_report = report_dir / "perf-report.json"
    target_report.write_bytes(args.perf_report.read_bytes())

    index_path = public_dir / "report-index.json"
    rows = load_index(index_path)
    entry_id = f"external-{name}-{short_sha}"
    entry = {
        "id": entry_id,
        "kind": "external",
        "name": args.name,
        "branch": args.branch,
        "commit_sha": args.commit_sha,
        "commit_title": args.commit_title,
        "source_url": args.source_url,
        "generated_china": datetime.now(CHINA_TZ).strftime("%Y-%m-%d %H:%M:%S CST"),
        "perf_report_url": f"./external/{name}/{short_sha}/perf-report.json",
    }
    rows = [row for row in rows if row.get("id") != entry_id]
    rows.insert(0, entry)
    index_path.write_text(json.dumps({"reports": rows}, ensure_ascii=True, indent=2) + "\n")
    print(target_report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
