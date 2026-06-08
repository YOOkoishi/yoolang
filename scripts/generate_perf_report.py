#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo


CHINA_TZ = ZoneInfo("Asia/Shanghai")
TIME_RE = re.compile(r"([0-9]+(?:\.[0-9]+)?)s?")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate static Yoolang performance report.")
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--delta-report", type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--out-html", required=True, type=Path)
    parser.add_argument("--pages-base-url", default="")
    parser.add_argument("--branch", default="")
    parser.add_argument("--commit-sha", default="")
    parser.add_argument("--commit-title", default="")
    parser.add_argument("--actor", default="")
    return parser.parse_args()


def read_json(path: Path | None) -> dict[str, Any]:
    if path is None or not path.exists():
        return {}
    try:
        payload = json.loads(path.read_text(errors="replace"))
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def report_meta(args: argparse.Namespace) -> dict[str, str]:
    commit_title = args.commit_title or os.environ.get("HEAD_COMMIT_MESSAGE", "")
    return {
        "branch": args.branch or os.environ.get("GITHUB_REF_NAME", "") or git_value("branch", "--show-current"),
        "commit_sha": args.commit_sha or os.environ.get("GITHUB_SHA", "") or git_value("rev-parse", "HEAD"),
        "commit_title": (commit_title.splitlines() or [""])[0],
        "actor": args.actor or os.environ.get("GITHUB_ACTOR", "") or git_value("show", "-s", "--format=%an", "HEAD"),
    }


def format_china_time(value: Any) -> str:
    if not isinstance(value, str) or not value.strip():
        return ""
    text = value.strip()
    normalized = text[:-4] + "+00:00" if text.endswith(" UTC") else text
    normalized = normalized[:-1] + "+00:00" if normalized.endswith("Z") else normalized
    try:
        dt = datetime.fromisoformat(normalized)
    except ValueError:
        return text
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(CHINA_TZ).strftime("%Y-%m-%d %H:%M:%S CST")


def parse_seconds(value: Any) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    match = TIME_RE.fullmatch(str(value).strip())
    return float(match.group(1)) if match else None


def speedup(reference: float | None, current: float | None) -> float | None:
    if reference is None or current is None or current <= 0.0:
        return None
    return reference / current


def geometric_mean(values: list[float]) -> float | None:
    positives = [value for value in values if value > 0.0]
    if not positives:
        return None
    return math.exp(sum(math.log(value) for value in positives) / len(positives))


def failure_key(row: dict[str, Any]) -> str:
    reason = str(row.get("reason") or row.get("status") or "unknown").strip()
    first_line = reason.splitlines()[0] if reason else "unknown"
    return first_line[:120]


def rows_by_case(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    rows = payload.get("rows", [])
    if not isinstance(rows, list):
        return {}
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        if isinstance(row, dict) and isinstance(row.get("case"), str):
            result[row["case"]] = row
    return result


def build_payload(perf: dict[str, Any], delta: dict[str, Any], meta: dict[str, str]) -> dict[str, Any]:
    delta_rows = rows_by_case(delta)
    rows: list[dict[str, Any]] = []
    for row in perf.get("rows", []):
        if not isinstance(row, dict):
            continue
        case = str(row.get("case", ""))
        gcc = parse_seconds(row.get("gcc"))
        clang = parse_seconds(row.get("clang"))
        yoolang = parse_seconds(row.get("compiler", row.get("yoolang")))
        delta_row = delta_rows.get(case, {})
        status = str(row.get("status", "UNKNOWN"))
        detail = str(row.get("detail", "")).strip()
        rows.append(
            {
                "case": case,
                "yoolang_sec": yoolang,
                "gcc_sec": gcc,
                "clang_sec": clang,
                "yoolang_vs_gcc_speedup": speedup(gcc, yoolang),
                "yoolang_vs_clang_speedup": speedup(clang, yoolang),
                "baseline_sec": delta_row.get("baseline") if isinstance(delta_row.get("baseline"), (int, float)) else None,
                "delta_pct": delta_row.get("delta_pct") if isinstance(delta_row.get("delta_pct"), (int, float)) else None,
                "baseline_speedup": delta_row.get("speedup") if isinstance(delta_row.get("speedup"), (int, float)) else None,
                "baseline_status": str(delta_row.get("status", "")),
                "status": status,
                "reason": detail,
            }
        )

    ok_rows = [row for row in rows if row["status"] == "OK"]
    failed_rows = [row for row in rows if row["status"] != "OK"]
    loses_gcc = [
        row
        for row in ok_rows
        if isinstance(row["yoolang_sec"], float) and isinstance(row["gcc_sec"], float) and row["yoolang_sec"] > row["gcc_sec"]
    ]
    loses_clang = [
        row
        for row in ok_rows
        if isinstance(row["yoolang_sec"], float) and isinstance(row["clang_sec"], float) and row["yoolang_sec"] > row["clang_sec"]
    ]
    wins_gcc = [
        row
        for row in ok_rows
        if isinstance(row["yoolang_sec"], float) and isinstance(row["gcc_sec"], float) and row["yoolang_sec"] < row["gcc_sec"]
    ]
    wins_clang = [
        row
        for row in ok_rows
        if isinstance(row["yoolang_sec"], float) and isinstance(row["clang_sec"], float) and row["yoolang_sec"] < row["clang_sec"]
    ]
    regressions = [
        row
        for row in rows
        if isinstance(row["delta_pct"], (int, float)) and row["delta_pct"] > 0.0
    ]
    improvements = [
        row
        for row in rows
        if isinstance(row["delta_pct"], (int, float)) and row["delta_pct"] < 0.0
    ]
    total_yoolang = sum(row["yoolang_sec"] for row in ok_rows if isinstance(row["yoolang_sec"], float))
    total_gcc = sum(row["gcc_sec"] for row in ok_rows if isinstance(row["gcc_sec"], float))
    total_clang = sum(row["clang_sec"] for row in ok_rows if isinstance(row["clang_sec"], float))
    failure_reasons: dict[str, int] = {}
    for row in failed_rows:
        key = failure_key(row)
        failure_reasons[key] = failure_reasons.get(key, 0) + 1

    compiler_comparisons: list[dict[str, Any]] = []
    for row in ok_rows:
        for label, ref_key, speed_key in (
            ("GCC", "gcc_sec", "yoolang_vs_gcc_speedup"),
            ("Clang++", "clang_sec", "yoolang_vs_clang_speedup"),
        ):
            reference = row.get(ref_key)
            current = row.get("yoolang_sec")
            ratio = row.get(speed_key)
            if not isinstance(reference, float) or not isinstance(current, float) or not isinstance(ratio, float):
                continue
            compiler_comparisons.append(
                {
                    "case": row["case"],
                    "target": label,
                    "yoolang_sec": current,
                    "reference_sec": reference,
                    "speedup": ratio,
                    "delta_pct": ((current - reference) / reference) * 100.0 if reference > 0.0 else None,
                }
            )
    top_faster = sorted(
        [item for item in compiler_comparisons if item["speedup"] > 1.0],
        key=lambda item: item["speedup"],
        reverse=True,
    )[:10]
    top_slower = sorted(
        [item for item in compiler_comparisons if item["speedup"] < 1.0],
        key=lambda item: item["speedup"],
    )[:10]
    top_baseline_faster = sorted(improvements, key=lambda row: row.get("delta_pct", 0.0))[:10]
    top_baseline_slower = sorted(regressions, key=lambda row: row.get("delta_pct", 0.0), reverse=True)[:10]

    return {
        "generated_china": datetime.now(CHINA_TZ).strftime("%Y-%m-%d %H:%M:%S CST"),
        "source_generated_china": format_china_time(perf.get("generated_utc", "")),
        "meta": meta,
        "status": perf.get("status", "UNKNOWN"),
        "baseline": {
            "status": delta.get("status", "NO BASELINE"),
            "branch": delta.get("baseline_branch", ""),
            "commit_sha": delta.get("baseline_commit_sha", ""),
            "commit_title": delta.get("baseline_commit_title", ""),
            "commit_author": delta.get("baseline_commit_author", ""),
        },
        "summary": {
            "cases": len(rows),
            "ok_cases": len(ok_rows),
            "yoolang_wins_gcc": len(wins_gcc),
            "yoolang_wins_clang": len(wins_clang),
            "failed_cases": len(failed_rows),
            "yoolang_loses_gcc": len(loses_gcc),
            "yoolang_loses_clang": len(loses_clang),
            "baseline_improvements": len(improvements),
            "baseline_regressions": len(regressions),
            "total_runtime_sec": perf.get("total_runtime_sec"),
            "total_yoolang_sec": total_yoolang if ok_rows else None,
            "total_gcc_sec": total_gcc if ok_rows else None,
            "total_clang_sec": total_clang if ok_rows else None,
            "total_yoolang_vs_gcc_speedup": speedup(total_gcc, total_yoolang),
            "total_yoolang_vs_clang_speedup": speedup(total_clang, total_yoolang),
            "gcc_geomean_speedup": perf.get("gcc_o3_geomean"),
            "clang_geomean_speedup": perf.get("clang_o3_geomean"),
            "computed_gcc_geomean_speedup": geometric_mean(
                [row["yoolang_vs_gcc_speedup"] for row in ok_rows if isinstance(row["yoolang_vs_gcc_speedup"], float)]
            ),
            "computed_clang_geomean_speedup": geometric_mean(
                [row["yoolang_vs_clang_speedup"] for row in ok_rows if isinstance(row["yoolang_vs_clang_speedup"], float)]
            ),
            "baseline_current_total_sec": delta.get("current_compiler_total")
            if isinstance(delta.get("current_compiler_total"), (int, float))
            else None,
            "baseline_total_sec": delta.get("baseline_compiler_total")
            if isinstance(delta.get("baseline_compiler_total"), (int, float))
            else None,
            "baseline_total_delta_pct": delta.get("total_delta_pct")
            if isinstance(delta.get("total_delta_pct"), (int, float))
            else None,
            "baseline_total_speedup": delta.get("total_speedup")
            if isinstance(delta.get("total_speedup"), (int, float))
            else None,
            "baseline_geomean_speedup": delta.get("case_speedup_geomean")
            if isinstance(delta.get("case_speedup_geomean"), (int, float))
            else None,
        },
        "failure_reasons": [{"reason": key, "count": value} for key, value in sorted(failure_reasons.items(), key=lambda item: item[1], reverse=True)],
        "top": {
            "compiler_faster": top_faster,
            "compiler_slower": top_slower,
            "baseline_faster": top_baseline_faster,
            "baseline_slower": top_baseline_slower,
        },
        "rows": rows,
    }


def html_escape_json(payload: dict[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=True, indent=2).replace("</", "<\\/")


def write_html(payload: dict[str, Any], out_html: Path, pages_base_url: str = "") -> None:
    data = html_escape_json(payload)
    base_url = pages_base_url.rstrip("/") if pages_base_url else ""
    main_perf_href = f"{base_url}/perf-report/" if base_url else "../perf-report/"
    main_insn_href = f"{base_url}/instruction-report/" if base_url else "../instruction-report/"
    history_href = f"{base_url}/history.html" if base_url else "../history.html"
    html = f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Yoolang QEMU 性能</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f5f7fb;
      --panel: #ffffff;
      --text: #1f2430;
      --muted: #667085;
      --line: #d8dee9;
      --head: #edf2f8;
      --accent: #0b6bcb;
      --accent-soft: #e8f2ff;
      --good: #067647;
      --good-soft: #ecfdf3;
      --bad: #b42318;
      --bad-soft: #fff1f0;
      --warn: #b54708;
      --warn-soft: #fff7e6;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }}
    header {{
      padding: 26px 32px 18px;
      background: var(--panel);
      border-bottom: 1px solid var(--line);
    }}
    h1 {{ margin: 0 0 8px; font-size: 26px; letter-spacing: 0; }}
    .meta {{ color: var(--muted); font-size: 14px; line-height: 1.7; }}
    .chips {{ display: flex; flex-wrap: wrap; gap: 8px; margin-top: 8px; }}
    .chip {{
      display: inline-flex;
      align-items: center;
      min-height: 26px;
      padding: 3px 9px;
      border-radius: 999px;
      border: 1px solid var(--line);
      background: #f8fafc;
      color: var(--muted);
      font-size: 13px;
    }}
    .chip.ok {{ border-color: #abefc6; background: var(--good-soft); color: var(--good); }}
    .chip.fail {{ border-color: #fecdca; background: var(--bad-soft); color: var(--bad); }}
    main {{ padding: 22px 32px 36px; }}
    .stats {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
      gap: 12px;
      margin-bottom: 18px;
    }}
    .stat {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-left: 3px solid var(--accent);
      border-radius: 8px;
      padding: 13px 15px;
    }}
    .stat span {{ display: block; color: var(--muted); font-size: 13px; }}
    .stat strong {{ display: block; margin-top: 4px; font-size: 22px; font-variant-numeric: tabular-nums; }}
    .section {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 14px 16px;
      margin-bottom: 16px;
    }}
    .section h2 {{ margin: 0 0 10px; font-size: 17px; letter-spacing: 0; }}
    .section-grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
      gap: 12px;
    }}
    .measure {{
      margin-top: 8px;
      padding: 8px 10px;
      border: 1px solid #cfe1f8;
      border-radius: 6px;
      background: var(--accent-soft);
      color: #344054;
    }}
    .metric {{
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: 6px;
      background: #fafcff;
    }}
    .metric strong {{ display: block; margin-bottom: 4px; }}
    .top-card {{
      padding: 10px 12px;
      border: 1px solid var(--line);
      border-radius: 6px;
      background: #fcfdff;
    }}
    .top-card h2 {{ font-size: 15px; margin-bottom: 8px; }}
    .mini-list {{ margin: 0; padding-left: 20px; color: var(--text); }}
    .mini-list li {{ margin: 5px 0; }}
    .muted {{ color: var(--muted); }}
    .downloads {{ display: flex; flex-wrap: wrap; gap: 10px; margin-top: 10px; }}
    .downloads a {{
      color: var(--accent);
      text-decoration: none;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 6px 10px;
      background: #f8fafc;
    }}
    .toolbar {{
      display: grid;
      grid-template-columns: minmax(240px, 1fr) minmax(260px, 380px);
      gap: 12px;
      margin-bottom: 12px;
    }}
    .selection-toolbar {{
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 10px;
      margin: 0 0 12px;
    }}
    .selection-count {{
      color: var(--muted);
      font-size: 14px;
      font-variant-numeric: tabular-nums;
    }}
    input, select, button {{
      width: 100%;
      min-height: 40px;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 8px 10px;
      background: var(--panel);
      color: var(--text);
      font: inherit;
    }}
    button {{
      width: auto;
      min-width: 78px;
      cursor: pointer;
      background: #f8fafc;
      color: var(--accent);
      font-weight: 600;
    }}
    button:hover {{ background: var(--accent-soft); }}
    input[type="checkbox"] {{
      width: 16px;
      min-height: 16px;
      height: 16px;
      margin: 0;
      padding: 0;
      vertical-align: middle;
      accent-color: var(--accent);
    }}
    input:focus, select:focus, button:focus {{
      border-color: var(--accent);
      box-shadow: 0 0 0 3px var(--accent-soft);
      outline: none;
    }}
    .table-wrap {{
      overflow: auto;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }}
    table {{ width: 100%; border-collapse: collapse; min-width: 1230px; }}
    th, td {{
      padding: 10px 12px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      vertical-align: top;
      font-size: 14px;
    }}
    th {{
      position: sticky;
      top: 0;
      background: var(--head);
      cursor: pointer;
      user-select: none;
      white-space: nowrap;
    }}
    th.active {{ color: var(--accent); }}
    th.select-col {{
      width: 44px;
      cursor: default;
      text-align: center;
    }}
    th .sort {{ color: var(--muted); margin-left: 4px; }}
    tr:hover td {{ background: #fafcff; }}
    td.num {{ text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }}
    td.case {{ font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }}
    td.select-col {{ text-align: center; }}
    td.better {{ background: var(--good-soft); color: var(--good); font-weight: 600; }}
    td.worse {{ background: var(--bad-soft); color: var(--bad); font-weight: 600; }}
    td.warn {{ background: var(--warn-soft); color: var(--warn); font-weight: 600; }}
    .ok {{ color: var(--good); font-weight: 600; }}
    .fail {{ color: var(--bad); font-weight: 600; }}
    .reason {{ color: var(--muted); max-width: 360px; }}
    .empty {{ padding: 24px; color: var(--muted); }}
    @media (max-width: 760px) {{
      header, main {{ padding-left: 16px; padding-right: 16px; }}
      .toolbar {{ grid-template-columns: 1fr; }}
      .selection-toolbar {{ align-items: stretch; }}
      .selection-count {{ width: 100%; }}
    }}
  </style>
</head>
<body>
  <header>
    <h1>Yoolang QEMU 性能</h1>
    <div class="meta">
      <div id="meta"></div>
      <div id="baseline"></div>
      <div class="measure"><strong>测量方式:</strong> RISC-V 可执行文件在 qemu-riscv64 下运行的 case 级耗时；数值越小代表生成代码运行越快。</div>
      <div class="downloads">
        <a href="{main_perf_href}">最新性能报告 (main)</a>
        <a href="{main_insn_href}">最新指令数报告 (main)</a>
        <a href="{history_href}">历史报告索引</a>
        <a href="./perf_compare.json" download>下载 perf_compare.json</a>
      </div>
      <div class="chips" id="chips"></div>
    </div>
  </header>
  <main>
    <section class="stats" id="stats"></section>
    <section class="section" id="totals"></section>
    <section class="section">
      <h2>Top 变化</h2>
      <div class="section-grid" id="top-lists"></div>
    </section>
    <section class="section" id="failures"></section>
    <section class="toolbar">
      <input id="search" type="search" placeholder="搜索 case 名">
      <select id="filter">
        <option value="all">全部 case</option>
        <option value="win-any">只看 yoolang 快于 GCC 或 Clang++</option>
        <option value="win-gcc">只看 yoolang 快于 GCC</option>
        <option value="win-clang">只看 yoolang 快于 Clang++</option>
        <option value="lose-any">只看 yoolang 输给 GCC 或 Clang++</option>
        <option value="lose-gcc">只看 yoolang 输给 GCC</option>
        <option value="lose-clang">只看 yoolang 输给 Clang++</option>
        <option value="improvement">只看相对 baseline 变快</option>
        <option value="regression">只看相对 baseline 变慢</option>
        <option value="failed">只看失败 case</option>
      </select>
    </section>
    <section class="selection-toolbar">
      <span class="selection-count" id="selection-count"></span>
      <button id="select-all" type="button">全选</button>
      <button id="select-none" type="button">清空</button>
      <button id="invert-visible" type="button">反选当前筛选结果</button>
    </section>
    <section class="table-wrap">
      <table>
        <thead>
          <tr>
            <th class="select-col"><input id="select-visible" type="checkbox" aria-label="选择当前筛选结果"></th>
            <th data-key="case">Case <span class="sort"></span></th>
            <th data-key="yoolang_sec">Yoolang 时间 <span class="sort"></span></th>
            <th data-key="gcc_sec">GCC 时间 <span class="sort"></span></th>
            <th data-key="clang_sec">Clang++ 时间 <span class="sort"></span></th>
            <th data-key="yoolang_vs_gcc_speedup">Yoolang vs GCC <span class="sort"></span></th>
            <th data-key="yoolang_vs_clang_speedup">Yoolang vs Clang++ <span class="sort"></span></th>
            <th data-key="baseline_sec">Baseline 时间 <span class="sort"></span></th>
            <th data-key="delta_pct">相对 baseline <span class="sort"></span></th>
            <th data-key="status">状态 <span class="sort"></span></th>
            <th data-key="reason">失败原因 <span class="sort"></span></th>
          </tr>
        </thead>
        <tbody id="rows"></tbody>
      </table>
      <div class="empty" id="empty" hidden>没有匹配的 case。</div>
    </section>
  </main>
  <script id="report-data" type="application/json">{data}</script>
  <script>
    const report = JSON.parse(document.getElementById('report-data').textContent);
    const allRows = Array.isArray(report.rows) ? report.rows : [];
    const selectedCases = new Set(allRows.map((row) => String(row.case || '')));
    let sortKey = 'case';
    let sortDir = 1;

    const isNumber = (value) => Number.isFinite(value);
    const fmtInt = (value) => Number.isInteger(value) ? value.toLocaleString() : 'N/A';
    const fmtSec = (value) => isNumber(value) ? `${{value.toFixed(4)}}s` : 'N/A';
    const fmtPct = (value) => isNumber(value) ? `${{value > 0 ? '+' : ''}}${{value.toFixed(2)}}%` : 'N/A';
    const fmtSpeedup = (value) => isNumber(value) ? `${{value.toFixed(2)}}x` : 'N/A';
    const winsGcc = (row) => isNumber(row.yoolang_sec) && isNumber(row.gcc_sec) && row.yoolang_sec < row.gcc_sec;
    const winsClang = (row) => isNumber(row.yoolang_sec) && isNumber(row.clang_sec) && row.yoolang_sec < row.clang_sec;
    const losesGcc = (row) => isNumber(row.yoolang_sec) && isNumber(row.gcc_sec) && row.yoolang_sec > row.gcc_sec;
    const losesClang = (row) => isNumber(row.yoolang_sec) && isNumber(row.clang_sec) && row.yoolang_sec > row.clang_sec;
    const compareTimeClass = (current, reference) => {{
      if (!isNumber(current) || !isNumber(reference)) return '';
      if (current < reference) return 'better';
      if (current > reference) return 'worse';
      return 'warn';
    }};
    const baselineClass = (delta) => {{
      if (!isNumber(delta)) return '';
      // Speed-based: positive = yoolang faster
      if (delta > 0) return 'better';
      if (delta < 0) return 'worse';
      return 'warn';
    }};

    function speedup(reference, current) {{
      return isNumber(reference) && isNumber(current) && current > 0 ? reference / current : null;
    }}

    function geometricMean(values) {{
      const positives = values.filter((value) => isNumber(value) && value > 0);
      if (!positives.length) return null;
      return Math.exp(positives.reduce((sum, value) => sum + Math.log(value), 0) / positives.length);
    }}

    function sumValues(rows, key) {{
      const values = rows.map((row) => row[key]).filter(isNumber);
      return values.length ? values.reduce((sum, value) => sum + value, 0) : null;
    }}

    function failureKey(row) {{
      const reason = String(row.reason || row.status || 'unknown').trim() || 'unknown';
      return reason.split('\\n')[0].slice(0, 120);
    }}

    function selectedRows() {{
      return allRows.filter((row) => selectedCases.has(String(row.case || '')));
    }}

    function selectedReport() {{
      const rows = selectedRows();
      const okRows = rows.filter((row) => row.status === 'OK');
      const failedRows = rows.filter((row) => row.status !== 'OK');
      const baselineRows = okRows.filter((row) => isNumber(row.yoolang_sec) && isNumber(row.baseline_sec));
      const totalYoolang = sumValues(okRows, 'yoolang_sec');
      const totalGcc = sumValues(okRows, 'gcc_sec');
      const totalClang = sumValues(okRows, 'clang_sec');
      const baselineCurrentTotal = sumValues(baselineRows, 'yoolang_sec');
      const baselineTotal = sumValues(baselineRows, 'baseline_sec');

      const compilerComparisons = [];
      okRows.forEach((row) => {{
        [
          ['GCC', 'gcc_sec', 'yoolang_vs_gcc_speedup'],
          ['Clang++', 'clang_sec', 'yoolang_vs_clang_speedup'],
        ].forEach(([target, referenceKey, speedupKey]) => {{
          const reference = row[referenceKey];
          const current = row.yoolang_sec;
          const ratio = row[speedupKey];
          if (!isNumber(reference) || !isNumber(current) || !isNumber(ratio)) return;
          compilerComparisons.push({{
            case: row.case,
            target,
            yoolang_sec: current,
            reference_sec: reference,
            speedup: ratio,
            delta_pct: reference > 0 ? ((current - reference) / reference) * 100 : null,
          }});
        }});
      }});

      const failureCounts = new Map();
      failedRows.forEach((row) => {{
        const key = failureKey(row);
        failureCounts.set(key, (failureCounts.get(key) || 0) + 1);
      }});

      const baselineSpeedups = baselineRows.map((row) => speedup(row.baseline_sec, row.yoolang_sec)).filter(isNumber);
      const baselineTotalSpeedup = speedup(baselineTotal, baselineCurrentTotal);
      const baselineTotalDeltaPct = isNumber(baselineTotal) && baselineTotal > 0 && isNumber(baselineCurrentTotal)
        ? ((baselineTotal - baselineCurrentTotal) / baselineTotal) * 100
        : null;

      return {{
        summary: {{
          cases: rows.length,
          ok_cases: okRows.length,
          failed_cases: failedRows.length,
          yoolang_wins_gcc: okRows.filter(winsGcc).length,
          yoolang_wins_clang: okRows.filter(winsClang).length,
          yoolang_loses_gcc: okRows.filter(losesGcc).length,
          yoolang_loses_clang: okRows.filter(losesClang).length,
          baseline_improvements: rows.filter((row) => isNumber(row.delta_pct) && row.delta_pct > 0).length,
          baseline_regressions: rows.filter((row) => isNumber(row.delta_pct) && row.delta_pct < 0).length,
          total_yoolang_sec: totalYoolang,
          total_gcc_sec: totalGcc,
          total_clang_sec: totalClang,
          total_yoolang_vs_gcc_speedup: speedup(totalGcc, totalYoolang),
          total_yoolang_vs_clang_speedup: speedup(totalClang, totalYoolang),
          gcc_geomean_speedup: geometricMean(okRows.map((row) => row.yoolang_vs_gcc_speedup)),
          clang_geomean_speedup: geometricMean(okRows.map((row) => row.yoolang_vs_clang_speedup)),
          baseline_current_total_sec: baselineCurrentTotal,
          baseline_total_sec: baselineTotal,
          baseline_total_speedup: baselineTotalSpeedup,
          baseline_geomean_speedup: geometricMean(baselineSpeedups),
          baseline_total_delta_pct: baselineTotalDeltaPct,
        }},
        top: {{
          compiler_faster: compilerComparisons.filter((item) => item.speedup > 1).sort((a, b) => b.speedup - a.speedup).slice(0, 10),
          compiler_slower: compilerComparisons.filter((item) => item.speedup < 1).sort((a, b) => a.speedup - b.speedup).slice(0, 10),
          baseline_faster: rows.filter((row) => isNumber(row.delta_pct) && row.delta_pct > 0).sort((a, b) => b.delta_pct - a.delta_pct).slice(0, 10),
          baseline_slower: rows.filter((row) => isNumber(row.delta_pct) && row.delta_pct < 0).sort((a, b) => a.delta_pct - b.delta_pct).slice(0, 10),
        }},
        failureReasons: Array.from(failureCounts, ([reason, count]) => ({{reason, count}})).sort((a, b) => b.count - a.count),
      }};
    }}

    function renderStats() {{
      const selected = selectedReport();
      const stats = selected.summary;
      const meta = report.meta || {{}};
      const baseline = report.baseline || {{}};
      const status = report.status || 'UNKNOWN';
      const shortSha = meta.commit_sha ? String(meta.commit_sha).slice(0, 12) : 'unknown';
      const title = meta.commit_title ? ` ${{meta.commit_title}}` : '';
      document.getElementById('meta').textContent = `生成时间: ${{report.generated_china || 'unknown'}} | 源数据时间: ${{report.source_generated_china || 'unknown'}} | branch: ${{meta.branch || 'unknown'}} | commit: ${{shortSha}}${{title}} | 提交人: ${{meta.actor || 'unknown'}}`;
      const baselineSha = baseline.commit_sha ? String(baseline.commit_sha).slice(0, 12) : '';
      document.getElementById('baseline').textContent = baselineSha ? `baseline: ${{baseline.branch || 'unknown'}} @ ${{baselineSha}} ${{baseline.commit_title || ''}}` : `baseline: ${{baseline.status || 'NO BASELINE'}}`;
      document.getElementById('chips').innerHTML = [
        `<span class="chip ${{status === 'PASS' ? 'ok' : 'fail'}}">状态: ${{escapeHtml(status)}}</span>`,
        `<span class="chip">统计范围: 已选 case</span>`,
        `<span class="chip">点击表头排序</span>`,
        `<span class="chip">绿色表示 yoolang 更快</span>`,
        `<span class="chip">红色表示 yoolang 更慢</span>`,
      ].join('');
      document.getElementById('stats').innerHTML = [
        ['总 case', stats.cases],
        ['成功 case', stats.ok_cases],
        ['yoolang 快于 GCC', stats.yoolang_wins_gcc],
        ['yoolang 快于 Clang++', stats.yoolang_wins_clang],
        ['yoolang 输给 GCC', stats.yoolang_loses_gcc],
        ['yoolang 输给 Clang++', stats.yoolang_loses_clang],
        ['baseline 变快', stats.baseline_improvements],
        ['baseline 变慢', stats.baseline_regressions],
        ['失败 case', stats.failed_cases],
      ].map(([label, value]) => `<div class="stat"><span>${{label}}</span><strong>${{fmtInt(value)}}</strong></div>`).join('');
      const totalsTitle = selectedCases.size === allRows.length
        ? '整体指标'
        : `已选 case 指标（${{selectedCases.size}} / ${{allRows.length}}）`;
      document.getElementById('totals').innerHTML = `
        <h2>${{totalsTitle}}</h2>
        <div class="section-grid">
          <div class="metric"><strong>总运行时间</strong><div class="muted">Yoolang ${{fmtSec(stats.total_yoolang_sec)}} / GCC ${{fmtSec(stats.total_gcc_sec)}} / Clang++ ${{fmtSec(stats.total_clang_sec)}}</div></div>
          <div class="metric"><strong>总时间加速比</strong><div class="muted">vs GCC ${{fmtSpeedup(stats.total_yoolang_vs_gcc_speedup)}} / vs Clang++ ${{fmtSpeedup(stats.total_yoolang_vs_clang_speedup)}}</div></div>
          <div class="metric"><strong>几何平均加速比</strong><div class="muted">vs GCC ${{fmtSpeedup(stats.gcc_geomean_speedup)}} / vs Clang++ ${{fmtSpeedup(stats.clang_geomean_speedup)}}</div></div>
          <div class="metric"><strong>相对 baseline</strong><div class="muted">当前 ${{fmtSec(stats.baseline_current_total_sec)}} / baseline ${{fmtSec(stats.baseline_total_sec)}} / 总加速比 ${{fmtSpeedup(stats.baseline_total_speedup)}} / 几何平均 ${{fmtSpeedup(stats.baseline_geomean_speedup)}} / 变化 ${{fmtPct(stats.baseline_total_delta_pct)}}</div></div>
        </div>
      `;
      renderTopLists(selected.top);
      renderFailures(selected.failureReasons);
    }}

    function renderTopLists(top) {{
      const compilerItem = (item) => `${{escapeHtml(item.case || '')}} <span class="muted">vs ${{escapeHtml(item.target || '')}}: ${{fmtSpeedup(item.speedup)}} (${{fmtPct(item.delta_pct)}})</span>`;
      const baselineItem = (item) => `${{escapeHtml(item.case || '')}} <span class="muted">${{fmtPct(item.delta_pct)}} / 当前 ${{fmtSec(item.yoolang_sec)}} / baseline ${{fmtSec(item.baseline_sec)}}</span>`;
      document.getElementById('top-lists').innerHTML = [
        renderList('快于 GCC/Clang++ Top 10', top.compiler_faster || [], compilerItem),
        renderList('慢于 GCC/Clang++ Top 10', top.compiler_slower || [], compilerItem),
        renderList('相对 baseline 变快 Top 10', top.baseline_faster || [], baselineItem),
        renderList('相对 baseline 变慢 Top 10', top.baseline_slower || [], baselineItem),
      ].join('');
    }}

    function renderFailures(reasons) {{
      document.getElementById('failures').innerHTML = `
        <h2>失败原因聚合</h2>
        ${{renderList('', reasons, (item) => `${{escapeHtml(item.reason)}} <span class="muted">${{fmtInt(item.count)}} case</span>`)}}
      `;
    }}

    function renderList(title, rows, itemRenderer) {{
      const heading = title ? `<h2>${{escapeHtml(title)}}</h2>` : '';
      if (!rows.length) return `<div class="top-card">${{heading}}<div class="muted">暂无数据</div></div>`;
      return `<div class="top-card">${{heading}}<ol class="mini-list">${{rows.map((item) => `<li>${{itemRenderer(item)}}</li>`).join('')}}</ol></div>`;
    }}

    function filteredRows() {{
      const query = document.getElementById('search').value.trim().toLowerCase();
      const filter = document.getElementById('filter').value;
      return (report.rows || []).filter((row) => {{
        if (query && !String(row.case || '').toLowerCase().includes(query)) return false;
        if (filter === 'win-gcc' && !winsGcc(row)) return false;
        if (filter === 'win-clang' && !winsClang(row)) return false;
        if (filter === 'win-any' && !winsGcc(row) && !winsClang(row)) return false;
        if (filter === 'lose-gcc' && !losesGcc(row)) return false;
        if (filter === 'lose-clang' && !losesClang(row)) return false;
        if (filter === 'lose-any' && !losesGcc(row) && !losesClang(row)) return false;
        if (filter === 'improvement' && !(isNumber(row.delta_pct) && row.delta_pct > 0)) return false;
        if (filter === 'regression' && !(isNumber(row.delta_pct) && row.delta_pct < 0)) return false;
        if (filter === 'failed' && row.status === 'OK') return false;
        return true;
      }}).sort(compareRows);
    }}

    function compareRows(a, b) {{
      const av = a[sortKey];
      const bv = b[sortKey];
      const an = isNumber(av);
      const bn = isNumber(bv);
      if (an || bn) {{
        if (!an) return 1;
        if (!bn) return -1;
        return (av - bv) * sortDir;
      }}
      return String(av || '').localeCompare(String(bv || '')) * sortDir;
    }}

    function updateSelectionState(rows) {{
      document.getElementById('selection-count').textContent = `已选 ${{selectedCases.size}} / 总 ${{allRows.length}}`;
      const selectVisible = document.getElementById('select-visible');
      const visibleSelected = rows.filter((row) => selectedCases.has(String(row.case || ''))).length;
      selectVisible.disabled = rows.length === 0;
      selectVisible.checked = rows.length > 0 && visibleSelected === rows.length;
      selectVisible.indeterminate = visibleSelected > 0 && visibleSelected < rows.length;
    }}

    function rerenderSelection() {{
      renderStats();
      renderRows();
    }}

    function setSelected(rows, checked) {{
      rows.forEach((row) => {{
        const key = String(row.case || '');
        if (checked) selectedCases.add(key);
        else selectedCases.delete(key);
      }});
      rerenderSelection();
    }}

    function renderRows() {{
      const rows = filteredRows();
      document.getElementById('empty').hidden = rows.length !== 0;
      updateSortIndicators();
      updateSelectionState(rows);
      document.getElementById('rows').innerHTML = rows.map((row) => `
        <tr>
          <td class="select-col"><input class="case-select" type="checkbox" data-case="${{escapeHtml(row.case || '')}}" aria-label="选择 ${{escapeHtml(row.case || '')}}" ${{selectedCases.has(String(row.case || '')) ? 'checked' : ''}}></td>
          <td class="case">${{escapeHtml(row.case || '')}}</td>
          <td class="num">${{fmtSec(row.yoolang_sec)}}</td>
          <td class="num ${{compareTimeClass(row.yoolang_sec, row.gcc_sec)}}">${{fmtSec(row.gcc_sec)}}</td>
          <td class="num ${{compareTimeClass(row.yoolang_sec, row.clang_sec)}}">${{fmtSec(row.clang_sec)}}</td>
          <td class="num ${{compareTimeClass(row.yoolang_sec, row.gcc_sec)}}">${{fmtSpeedup(row.yoolang_vs_gcc_speedup)}}</td>
          <td class="num ${{compareTimeClass(row.yoolang_sec, row.clang_sec)}}">${{fmtSpeedup(row.yoolang_vs_clang_speedup)}}</td>
          <td class="num">${{fmtSec(row.baseline_sec)}}</td>
          <td class="num ${{baselineClass(row.delta_pct)}}">${{fmtPct(row.delta_pct)}}</td>
          <td class="${{row.status === 'OK' ? 'ok' : 'fail'}}">${{escapeHtml(row.status || '')}}</td>
          <td class="reason">${{escapeHtml(row.reason || '')}}</td>
        </tr>
      `).join('');
    }}

    function updateSortIndicators() {{
      document.querySelectorAll('th[data-key]').forEach((th) => {{
        const active = th.dataset.key === sortKey;
        th.classList.toggle('active', active);
        const mark = th.querySelector('.sort');
        if (mark) mark.textContent = active ? (sortDir > 0 ? '↑' : '↓') : '';
      }});
    }}

    function escapeHtml(value) {{
      return String(value)
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#039;');
    }}

    document.querySelectorAll('th[data-key]').forEach((th) => {{
      th.addEventListener('click', () => {{
        const key = th.dataset.key;
        if (sortKey === key) sortDir *= -1;
        else {{
          sortKey = key;
          sortDir = key === 'case' ? 1 : -1;
        }}
        renderRows();
      }});
    }});
    document.getElementById('search').addEventListener('input', renderRows);
    document.getElementById('filter').addEventListener('change', renderRows);
    document.getElementById('select-all').addEventListener('click', () => setSelected(allRows, true));
    document.getElementById('select-none').addEventListener('click', () => setSelected(allRows, false));
    document.getElementById('invert-visible').addEventListener('click', () => {{
      filteredRows().forEach((row) => {{
        const key = String(row.case || '');
        if (selectedCases.has(key)) selectedCases.delete(key);
        else selectedCases.add(key);
      }});
      rerenderSelection();
    }});
    document.getElementById('select-visible').addEventListener('change', (event) => {{
      setSelected(filteredRows(), event.target.checked);
    }});
    document.getElementById('rows').addEventListener('change', (event) => {{
      const target = event.target;
      if (!target.classList || !target.classList.contains('case-select')) return;
      const key = String(target.dataset.case || '');
      if (target.checked) selectedCases.add(key);
      else selectedCases.delete(key);
      rerenderSelection();
    }});
    renderStats();
    renderRows();
  </script>
</body>
</html>
"""
    out_html.parent.mkdir(parents=True, exist_ok=True)
    out_html.write_text(html)


def main() -> int:
    args = parse_args()
    perf = read_json(args.perf_report)
    delta = read_json(args.delta_report)
    payload = build_payload(perf, delta, report_meta(args))
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    json_text = json.dumps(payload, ensure_ascii=True, indent=2) + "\n"
    args.out_json.write_text(json_text)
    write_html(payload, args.out_html, args.pages_base_url)
    page_json = args.out_html.parent / args.out_json.name
    if page_json != args.out_json:
        page_json.write_text(json_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
