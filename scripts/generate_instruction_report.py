#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from posixpath import relpath as posix_relpath
from typing import Any
from zoneinfo import ZoneInfo

from report_theme import REPORT_THEME_BIND_SCRIPT, REPORT_THEME_CSS, REPORT_THEME_HEAD_SCRIPT, REPORT_THEME_TOGGLE_HTML


CHINA_TZ = ZoneInfo("Asia/Shanghai")


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate QEMU dynamic instruction count report.")
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--out-html", required=True, type=Path)
    parser.add_argument("--pages-base-url", default="")
    parser.add_argument("--branch", default="")
    parser.add_argument("--commit-sha", default="")
    parser.add_argument("--commit-title", default="")
    parser.add_argument("--actor", default="")
    parser.add_argument("--report-index-url", default="")
    return parser.parse_args()


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


def as_int(value: Any) -> int | None:
    return value if isinstance(value, int) else None


def speedup(reference: int | None, current: int | None) -> float | None:
    if reference is None or current is None or current <= 0:
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


def status_for(row: dict[str, Any], counts: dict[str, int | None]) -> tuple[str, str]:
    if row.get("status") != "OK":
        return "FAILED", str(row.get("detail") or row.get("status") or "case failed")

    statuses = row.get("instruction_count_statuses")
    details = row.get("instruction_count_details")
    if not isinstance(statuses, dict):
        status = str(row.get("instruction_count_status", "UNKNOWN"))
        detail = str(row.get("instruction_count_detail", ""))
        return ("OK", "") if status == "OK" else (status, detail)

    bad_parts: list[str] = []
    for key, label in (("compiler", "yoolang"), ("gcc", "GCC"), ("clang", "Clang++")):
        if counts.get(key) is not None:
            continue
        status = str(statuses.get(key, "UNKNOWN"))
        detail = ""
        if isinstance(details, dict):
            detail = str(details.get(key, "")).strip()
        bad_parts.append(f"{label}: {status}{' - ' + detail if detail else ''}")

    if bad_parts:
        return "FAILED", "; ".join(bad_parts)
    return "OK", ""


def build_payload(perf: dict[str, Any], meta: dict[str, str]) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []

    for row in perf.get("rows", []):
        if not isinstance(row, dict):
            continue
        raw_counts = row.get("instruction_counts")
        if not isinstance(raw_counts, dict):
            raw_counts = {}
        counts = {
            "compiler": as_int(raw_counts.get("compiler", row.get("instruction_count"))),
            "gcc": as_int(raw_counts.get("gcc")),
            "clang": as_int(raw_counts.get("clang")),
            "hy": as_int(raw_counts.get("hy")),
        }
        status, reason = status_for(row, counts)
        yoolang_vs_gcc = speedup(counts["gcc"], counts["compiler"])
        yoolang_vs_clang = speedup(counts["clang"], counts["compiler"])
        rows.append(
            {
                "case": str(row.get("case", "")),
                "yoolang_instructions": counts["compiler"],
                "gcc_instructions": counts["gcc"],
                "clang_instructions": counts["clang"],
                "yoolang_vs_gcc_speedup": yoolang_vs_gcc,
                "yoolang_vs_clang_speedup": yoolang_vs_clang,
                "status": status,
                "reason": reason,
                "hy_instructions": counts.get("hy"),
            }
        )

    counted = [row for row in rows if isinstance(row["yoolang_instructions"], int)]
    failed = [row for row in rows if row["status"] != "OK"]
    yoolang_loses_gcc = [
        row
        for row in rows
        if isinstance(row["yoolang_instructions"], int)
        and isinstance(row["gcc_instructions"], int)
        and row["yoolang_instructions"] > row["gcc_instructions"]
    ]
    yoolang_loses_clang = [
        row
        for row in rows
        if isinstance(row["yoolang_instructions"], int)
        and isinstance(row["clang_instructions"], int)
        and row["yoolang_instructions"] > row["clang_instructions"]
    ]
    yoolang_wins_gcc = [
        row
        for row in rows
        if isinstance(row["yoolang_instructions"], int)
        and isinstance(row["gcc_instructions"], int)
        and row["yoolang_instructions"] < row["gcc_instructions"]
    ]
    yoolang_wins_clang = [
        row
        for row in rows
        if isinstance(row["yoolang_instructions"], int)
        and isinstance(row["clang_instructions"], int)
        and row["yoolang_instructions"] < row["clang_instructions"]
    ]
    total_yoolang = sum(row["yoolang_instructions"] for row in rows if isinstance(row["yoolang_instructions"], int))
    total_gcc = sum(row["gcc_instructions"] for row in rows if isinstance(row["gcc_instructions"], int))
    total_clang = sum(row["clang_instructions"] for row in rows if isinstance(row["clang_instructions"], int))
    failure_reasons: dict[str, int] = {}
    for row in failed:
        key = failure_key(row)
        failure_reasons[key] = failure_reasons.get(key, 0) + 1

    comparisons: list[dict[str, Any]] = []
    for row in rows:
        current = row.get("yoolang_instructions")
        if not isinstance(current, int):
            continue
        for label, ref_key, speed_key in (
            ("GCC", "gcc_instructions", "yoolang_vs_gcc_speedup"),
            ("Clang++", "clang_instructions", "yoolang_vs_clang_speedup"),
        ):
            reference = row.get(ref_key)
            ratio = row.get(speed_key)
            if not isinstance(reference, int) or not isinstance(ratio, float):
                continue
            comparisons.append(
                {
                    "case": row["case"],
                    "target": label,
                    "yoolang_instructions": current,
                    "reference_instructions": reference,
                    "speedup": ratio,
                    "delta_pct": ((current - reference) / reference) * 100.0 if reference > 0 else None,
                }
            )
    top_less = sorted(
        [item for item in comparisons if item["speedup"] > 1.0],
        key=lambda item: item["speedup"],
        reverse=True,
    )[:10]
    top_more = sorted(
        [item for item in comparisons if item["speedup"] < 1.0],
        key=lambda item: item["speedup"],
    )[:10]

    return {
        "generated_china": datetime.now(CHINA_TZ).strftime("%Y-%m-%d %H:%M:%S CST"),
        "source_generated_china": format_china_time(perf.get("generated_utc", "")),
        "meta": meta,
        "status": perf.get("instruction_count_summary", {}).get("status", "UNKNOWN")
        if isinstance(perf.get("instruction_count_summary"), dict)
        else "UNKNOWN",
        "summary": {
            "cases": len(rows),
            "counted_yoolang_cases": len(counted),
            "failed_cases": len(failed),
            "yoolang_wins_gcc": len(yoolang_wins_gcc),
            "yoolang_wins_clang": len(yoolang_wins_clang),
            "yoolang_loses_gcc": len(yoolang_loses_gcc),
            "yoolang_loses_clang": len(yoolang_loses_clang),
            "total_yoolang_instructions": total_yoolang if total_yoolang else None,
            "total_gcc_instructions": total_gcc if total_gcc else None,
            "total_clang_instructions": total_clang if total_clang else None,
            "total_yoolang_vs_gcc_speedup": speedup(total_gcc or None, total_yoolang or None),
            "total_yoolang_vs_clang_speedup": speedup(total_clang or None, total_yoolang or None),
            "geomean_yoolang_vs_gcc_speedup": geometric_mean(
                [row["yoolang_vs_gcc_speedup"] for row in rows if isinstance(row["yoolang_vs_gcc_speedup"], float)]
            ),
            "geomean_yoolang_vs_clang_speedup": geometric_mean(
                [row["yoolang_vs_clang_speedup"] for row in rows if isinstance(row["yoolang_vs_clang_speedup"], float)]
            ),
        },
        "failure_reasons": [{"reason": key, "count": value} for key, value in sorted(failure_reasons.items(), key=lambda item: item[1], reverse=True)],
        "top": {
            "instruction_less": top_less,
            "instruction_more": top_more,
        },
        "rows": rows,
        "report_index_url": "",
    }


def html_escape_json(payload: dict[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=True, indent=2).replace("</", "<\\/")


def default_report_index_url(out_html: Path, pages_base_url: str = "") -> str:
    if pages_base_url:
        return f"{pages_base_url.rstrip('/')}/report-index.json"
    parts = out_html.parent.parts
    if "public" not in parts:
        return "../report-index.json"
    public_index = parts.index("public")
    html_dir = "/".join(parts[public_index + 1 :])
    rel = posix_relpath("report-index.json", html_dir or ".")
    return rel if rel.startswith(".") else f"./{rel}"


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
  <title>Yoolang QEMU 动态指令数</title>
{REPORT_THEME_HEAD_SCRIPT}
  <style>
    :root {{
      color-scheme: light;
      --bg: #f4f6f9;
      --panel: #ffffff;
      --text: #20242c;
      --muted: #667085;
      --line: #d9dee8;
      --accent: #006adc;
      --accent-soft: #e8f1ff;
      --bad: #b42318;
      --bad-soft: #fff1f0;
      --good: #067647;
      --good-soft: #ecfdf3;
      --warn-soft: #fff7e6;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--text);
    }}
    header {{
      padding: 26px 32px 18px;
      border-bottom: 1px solid var(--line);
      background: var(--panel);
    }}
    h1 {{ margin: 0 0 8px; font-size: 26px; letter-spacing: 0; }}
    .meta {{ color: var(--muted); font-size: 14px; line-height: 1.6; }}
    .status-line {{ display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }}
    .pill {{
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
    .pill.ok {{ border-color: #abefc6; background: var(--good-soft); color: var(--good); }}
    .pill.fail {{ border-color: #fecdca; background: var(--bad-soft); color: var(--bad); }}
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
    .stat strong {{ display: block; margin-top: 4px; font-size: 22px; }}
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
    .baseline-toolbar {{
      display: grid;
      grid-template-columns: minmax(260px, 520px) minmax(220px, 1fr);
      gap: 12px;
      margin-bottom: 12px;
      align-items: center;
    }}
    .toolbar {{
      display: grid;
      grid-template-columns: minmax(240px, 1fr) minmax(240px, 360px);
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
    table {{ width: 100%; border-collapse: collapse; min-width: 1130px; }}
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
      background: #eef2f7;
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
    td.tie {{ background: var(--warn-soft); }}
    .ok {{ color: var(--good); font-weight: 600; }}
    .fail {{ color: var(--bad); font-weight: 600; }}
    .reason {{ color: var(--muted); max-width: 360px; }}
    .empty {{ padding: 24px; color: var(--muted); }}
{REPORT_THEME_CSS}
    @media (max-width: 760px) {{
      header, main {{ padding-left: 16px; padding-right: 16px; }}
      .toolbar, .baseline-toolbar {{ grid-template-columns: 1fr; }}
      .selection-toolbar {{ align-items: stretch; }}
      .selection-count {{ width: 100%; }}
    }}
  </style>
</head>
<body>
  <header>
    <div class="header-row">
      <h1>Yoolang QEMU 动态指令数</h1>
      {REPORT_THEME_TOGGLE_HTML}
    </div>
    <div class="meta">
      <div id="meta"></div>
      <div class="measure"><strong>测量方式:</strong> RISC-V 可执行文件通过 qemu-riscv64 TCG plugin 统计动态执行指令数；数值越小代表生成代码执行指令更少。</div>
      <div class="downloads">
        <a href="{main_perf_href}">最新性能报告 (main)</a>
        <a href="{main_insn_href}">最新指令数报告 (main)</a>
        <a href="{history_href}">历史报告索引</a>
        <a href="./instruction_count_compare.json" download>下载 instruction_count_compare.json</a>
      </div>
      <div class="status-line" id="status-line"></div>
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
    <section class="baseline-toolbar">
      <select id="baseline-select" aria-label="选择 baseline">
        <option value="embedded">当前 CI baseline（无）</option>
      </select>
      <div class="muted" id="baseline-load-status">正在读取可选对比报告...</div>
    </section>
    <section class="toolbar">
      <input id="search" type="search" placeholder="搜索 case 名">
      <select id="filter">
        <option value="all">全部 case</option>
        <option value="win-any">只看 yoolang 指令数少于 GCC 或 Clang++</option>
        <option value="win-gcc">只看 yoolang 指令数少于 GCC</option>
        <option value="win-clang">只看 yoolang 指令数少于 Clang++</option>
        <option value="lose-any">只看 yoolang 输给 GCC 或 Clang++</option>
        <option value="lose-gcc">只看 yoolang 输给 GCC</option>
        <option value="lose-clang">只看 yoolang 输给 Clang++</option>
        <option value="improvement">只看相对 baseline 指令数减少</option>
        <option value="regression">只看相对 baseline 指令数增加</option>
        <option value="failed">只看失败或缺失数据</option>
      </select>
    </section>
    <section class="selection-toolbar">
      <span class="selection-count" id="selection-count"></span>
      <button id="select-all" type="button">全选</button>
      <button id="select-none" type="button">清空</button>
      <button id="select-preliminary" type="button">初赛测例</button>
      <button id="invert-visible" type="button">反选当前筛选结果</button>
    </section>
    <section class="table-wrap">
      <table>
        <thead>
          <tr>
            <th class="select-col"><input id="select-visible" type="checkbox" aria-label="选择当前筛选结果"></th>
            <th data-key="case">Case <span class="sort"></span></th>
            <th data-key="yoolang_instructions">Yoolang 指令数 <span class="sort"></span></th>
            <th data-key="gcc_instructions">GCC 指令数 <span class="sort"></span></th>
            <th data-key="clang_instructions">Clang++ 指令数 <span class="sort"></span></th>
            <th data-key="yoolang_vs_gcc_speedup">Yoolang vs GCC <span class="sort"></span></th>
            <th data-key="yoolang_vs_clang_speedup">Yoolang vs Clang++ <span class="sort"></span></th>
            <th data-key="baseline_insn">Baseline 指令数 <span class="sort"></span></th>
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
{REPORT_THEME_BIND_SCRIPT}
  <script id="report-data" type="application/json">{data}</script>
  <script>
    const report = JSON.parse(document.getElementById('report-data').textContent);
    const sourceRows = Array.isArray(report.rows) ? report.rows : [];
    let allRows = sourceRows.map((row) => ({{...row}}));
    const selectedCases = new Set(allRows.map((row) => String(row.case || '')));
    let reportIndex = [];
    let activeBaseline = {{
      kind: 'embedded',
      label: '当前 CI baseline（无）',
      rowsByCase: null,
    }};
    let sortKey = 'case';
    let sortDir = 1;

    const isNumber = (value) => Number.isFinite(value);
    const fmtInt = (value) => Number.isInteger(value) ? value.toLocaleString() : 'N/A';
    const fmtSpeedup = (value) => isNumber(value) ? `${{value.toFixed(2)}}x` : 'N/A';
    const fmtPct = (value) => isNumber(value) ? `${{value > 0 ? '+' : ''}}${{value.toFixed(2)}}%` : 'N/A';
    const winsGcc = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.gcc_instructions) && row.yoolang_instructions < row.gcc_instructions;
    const winsClang = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.clang_instructions) && row.yoolang_instructions < row.clang_instructions;
    const losesGcc = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.gcc_instructions) && row.yoolang_instructions > row.gcc_instructions;
    const losesClang = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.clang_instructions) && row.yoolang_instructions > row.clang_instructions;
    const compareCountClass = (current, reference) => {{
      if (!Number.isInteger(current) || !Number.isInteger(reference)) return '';
      if (current < reference) return 'better';
      if (current > reference) return 'worse';
      return 'tie';
    }};

    function speedup(reference, current) {{
      return Number.isInteger(reference) && Number.isInteger(current) && current > 0 ? reference / current : null;
    }}

    function geometricMean(values) {{
      const positives = values.filter((value) => isNumber(value) && value > 0);
      if (!positives.length) return null;
      return Math.exp(positives.reduce((sum, value) => sum + Math.log(value), 0) / positives.length);
    }}

    function baselineLabel(entry) {{
      if (!entry || entry.kind === 'embedded') return '当前 CI baseline（无）';
      const name = entry.name || entry.kind || 'report';
      const branch = entry.branch ? `/${{entry.branch}}` : '';
      const sha = entry.commit_sha ? String(entry.commit_sha).slice(0, 12) : 'unknown';
      const title = entry.commit_title ? ` ${{entry.commit_title}}` : '';
      return `${{name}}${{branch}} @ ${{sha}}${{title}}`;
    }}

    function baselineInsnFromRow(baselineRow) {{
      if (!baselineRow) return null;
      if (activeBaseline.kind === 'external') {{
        if (Number.isInteger(baselineRow.hy_instructions)) return baselineRow.hy_instructions;
        const ic = baselineRow.instruction_counts;
        if (ic && Number.isInteger(ic.hy)) return ic.hy;
        return null;
      }}
      return Number.isInteger(baselineRow.yoolang_instructions) ? baselineRow.yoolang_instructions : null;
    }}

    function hyFailedInsn(row) {{
      if (!row) return false;
      const hy = row.hy_instructions ?? row.instruction_counts?.hy ?? row.hy;
      if (hy === null || hy === undefined) return false;
      if (Number.isInteger(hy)) return false;
      const s = String(hy).trim();
      return s && s !== 'MISSING';
    }}

    function rebuildRowsForBaseline() {{
      allRows = sourceRows.map((row) => {{
        const next = {{...row}};
        if (!activeBaseline.rowsByCase) {{
          next.baseline_insn = null;
          next.delta_pct = null;
          next.baseline_speedup = null;
          next.baseline_status = 'MISSING';
          return next;
        }}
        const baselineRow = activeBaseline.rowsByCase[String(row.case || '')];
        const baselineInsn = baselineRow ? baselineInsnFromRow(baselineRow) : null;
        const baselineRowStatus = baselineRow ? String(baselineRow.status || '') : 'MISSING';
        const baselineHyFailed = activeBaseline.kind === 'external' && hyFailedInsn(baselineRow);
        const baselineFailed = baselineRow && baselineRowStatus !== 'OK' && baselineRowStatus !== 'MISSING';
        const baselineFailedFinal = baselineFailed || baselineHyFailed;
        next.baseline_insn = baselineFailedFinal ? null : baselineInsn;
        next.baseline_speedup = baselineFailedFinal ? null : speedup(baselineInsn, row.yoolang_instructions);
        if (baselineHyFailed) {{
          const hyRaw = baselineRow.hy_instructions ?? baselineRow.instruction_counts?.hy ?? baselineRow.hy ?? 'CFAIL';
          next.baseline_status = String(hyRaw).trim();
        }} else if (baselineFailed) {{
          next.baseline_status = 'ERROR';
        }} else {{
          next.baseline_status = baselineRowStatus;
        }}
        next.delta_pct = Number.isInteger(baselineInsn) && baselineInsn > 0 && Number.isInteger(row.yoolang_instructions)
          ? ((baselineInsn - row.yoolang_instructions) / baselineInsn) * 100
          : null;
        return next;
      }});
      for (const key of Array.from(selectedCases)) {{
        if (!allRows.some((row) => String(row.case || '') === key)) selectedCases.delete(key);
      }}
    }}

    async function loadReportJson(url) {{
      const response = await fetch(url, {{cache: 'no-store'}});
      if (!response.ok) throw new Error(`${{response.status}} ${{response.statusText}}`);
      return response.json();
    }}

    function rowsByCaseFromPayload(payload) {{
      const out = {{}};
      const rows = Array.isArray(payload?.rows) ? payload.rows : [];
      rows.forEach((row) => {{
        if (!row || typeof row.case !== 'string') return;
        const normalized = {{...row}};
        // Normalize perf-report.json format: extract instruction_counts.* -> *_instructions
        if (row.instruction_counts) {{
          const ic = row.instruction_counts;
          if (!Number.isInteger(normalized.yoolang_instructions) && Number.isInteger(ic.compiler)) normalized.yoolang_instructions = ic.compiler;
          if (!Number.isInteger(normalized.gcc_instructions) && Number.isInteger(ic.gcc)) normalized.gcc_instructions = ic.gcc;
          if (!Number.isInteger(normalized.clang_instructions) && Number.isInteger(ic.clang)) normalized.clang_instructions = ic.clang;
          if (!Number.isInteger(normalized.hy_instructions) && Number.isInteger(ic.hy)) normalized.hy_instructions = ic.hy;
        }}
        out[row.case] = normalized;
      }});
      return out;
    }}

    async function selectBaseline(value) {{
      const status = document.getElementById('baseline-load-status');
      if (value === 'embedded') {{
        activeBaseline = {{
          kind: 'embedded',
          label: '当前 CI baseline（无）',
          rowsByCase: null,
        }};
        rebuildRowsForBaseline();
        status.textContent = '使用当前内嵌数据（无 baseline 对比）。';
        rerenderSelection();
        return;
      }}
      const entry = reportIndex.find((item) => item.id === value);
      if (!entry) return;
      try {{
        status.textContent = `正在加载 ${{baselineLabel(entry)}}...`;
        const payload = await loadReportJson(entry.perf_report_url);
        activeBaseline = {{...entry, label: baselineLabel(entry), rowsByCase: rowsByCaseFromPayload(payload)}};
        rebuildRowsForBaseline();
        status.textContent = `baseline: ${{baselineLabel(entry)}}`;
        rerenderSelection();
      }} catch (error) {{
        status.textContent = `baseline 加载失败: ${{error.message || error}}`;
      }}
    }}

    async function loadReportIndex() {{
      const status = document.getElementById('baseline-load-status');
      const select = document.getElementById('baseline-select');
      const indexUrl = report.report_index_url || '../report-index.json';
      try {{
        const payload = await loadReportJson(indexUrl);
        const indexBaseUrl = new URL(indexUrl, window.location.href);
        const rawEntries = Array.isArray(payload?.reports) ? payload.reports : [];
        reportIndex = rawEntries
          .filter((entry) => entry && entry.perf_report_url)
          .map((entry, index) => ({{
            ...entry,
            id: String(entry.id || `${{entry.kind || 'report'}}-${{entry.name || ''}}-${{entry.commit_sha || index}}`),
            perf_report_url: new URL(entry.perf_report_url, indexBaseUrl).href,
          }}));
        const yoolangEntries = reportIndex.filter((e) => e.kind === 'yoolang');
        const externalEntries = reportIndex.filter((e) => e.kind !== 'yoolang');
        if (yoolangEntries.length) {{
          const optgroup = document.createElement('optgroup');
          optgroup.label = 'yoolang 历史';
          yoolangEntries.forEach((entry) => {{
            const option = document.createElement('option');
            option.value = entry.id;
            option.textContent = baselineLabel(entry);
            optgroup.appendChild(option);
          }});
          select.appendChild(optgroup);
        }}
        if (externalEntries.length) {{
          const optgroup = document.createElement('optgroup');
          optgroup.label = '外部编译器';
          externalEntries.forEach((entry) => {{
            const option = document.createElement('option');
            option.value = entry.id;
            option.textContent = baselineLabel(entry);
            optgroup.appendChild(option);
          }});
          select.appendChild(optgroup);
        }}
        status.textContent = reportIndex.length
          ? `已加载 ${{reportIndex.length}} 个可选 baseline。`
          : '没有额外可选 baseline。';
      }} catch (error) {{
        status.textContent = '未找到 report-index.json，仅使用当前 CI baseline。';
      }}
    }}

    function sumCounts(rows, key) {{
      const values = rows.map((row) => row[key]).filter(Number.isInteger);
      return values.length ? values.reduce((sum, value) => sum + value, 0) : null;
    }}

    function failureKey(row) {{
      const reason = String(row.reason || row.status || 'unknown').trim() || 'unknown';
      return reason.split('\\n')[0].slice(0, 120);
    }}

    function selectedRows() {{
      return allRows.filter((row) => selectedCases.has(String(row.case || '')));
    }}

    function isPreliminaryCase(row) {{
      return String(row.case || '').startsWith('test/performance/');
    }}

    function selectedReport() {{
      const rows = selectedRows();
      const counted = rows.filter((row) => Number.isInteger(row.yoolang_instructions));
      const failed = rows.filter((row) => row.status !== 'OK');
      const totalYoolang = sumCounts(rows, 'yoolang_instructions');
      const totalGcc = sumCounts(rows, 'gcc_instructions');
      const totalClang = sumCounts(rows, 'clang_instructions');

      const comparisons = [];
      rows.forEach((row) => {{
        const current = row.yoolang_instructions;
        if (!Number.isInteger(current)) return;
        [
          ['GCC', 'gcc_instructions', 'yoolang_vs_gcc_speedup'],
          ['Clang++', 'clang_instructions', 'yoolang_vs_clang_speedup'],
        ].forEach(([target, referenceKey, speedupKey]) => {{
          const reference = row[referenceKey];
          const ratio = row[speedupKey];
          if (!Number.isInteger(reference) || !isNumber(ratio)) return;
          comparisons.push({{
            case: row.case,
            target,
            yoolang_instructions: current,
            reference_instructions: reference,
            speedup: ratio,
            delta_pct: reference > 0 ? ((current - reference) / reference) * 100 : null,
          }});
        }});
      }});

      const failureCounts = new Map();
      failed.forEach((row) => {{
        const key = failureKey(row);
        failureCounts.set(key, (failureCounts.get(key) || 0) + 1);
      }});

      const baselineRows = rows.filter((row) => Number.isInteger(row.baseline_insn));
      const baselineCurrentTotalInsn = sumCounts(baselineRows, 'yoolang_instructions');
      const baselineTotalInsn = sumCounts(baselineRows, 'baseline_insn');
      const baselineTotalSpeedup = speedup(baselineTotalInsn, baselineCurrentTotalInsn);
      const baselineGeomeanSpeedup = geometricMean(baselineRows.map((row) => row.baseline_speedup).filter(isNumber));

      return {{
        summary: {{
          cases: rows.length,
          counted_yoolang_cases: counted.length,
          failed_cases: failed.length,
          yoolang_wins_gcc: rows.filter(winsGcc).length,
          yoolang_wins_clang: rows.filter(winsClang).length,
          yoolang_loses_gcc: rows.filter(losesGcc).length,
          yoolang_loses_clang: rows.filter(losesClang).length,
          total_yoolang_instructions: totalYoolang,
          total_gcc_instructions: totalGcc,
          total_clang_instructions: totalClang,
          total_yoolang_vs_gcc_speedup: speedup(totalGcc, totalYoolang),
          total_yoolang_vs_clang_speedup: speedup(totalClang, totalYoolang),
          geomean_yoolang_vs_gcc_speedup: geometricMean(rows.map((row) => row.yoolang_vs_gcc_speedup)),
          geomean_yoolang_vs_clang_speedup: geometricMean(rows.map((row) => row.yoolang_vs_clang_speedup)),
          baseline_current_total_insn: baselineCurrentTotalInsn,
          baseline_total_insn: baselineTotalInsn,
          baseline_total_speedup: baselineTotalSpeedup,
          baseline_geomean_speedup: baselineGeomeanSpeedup,
        }},
        top: {{
          instruction_less: comparisons.filter((item) => item.speedup > 1).sort((a, b) => b.speedup - a.speedup).slice(0, 10),
          instruction_more: comparisons.filter((item) => item.speedup < 1).sort((a, b) => a.speedup - b.speedup).slice(0, 10),
        }},
        failureReasons: Array.from(failureCounts, ([reason, count]) => ({{reason, count}})).sort((a, b) => b.count - a.count),
      }};
    }}

    function renderStats() {{
      const selected = selectedReport();
      const stats = selected.summary;
      const status = report.status || 'UNKNOWN';
      const meta = report.meta || {{}};
      const shortSha = meta.commit_sha ? String(meta.commit_sha).slice(0, 12) : 'unknown';
      const commitTitle = meta.commit_title ? ` ${{meta.commit_title}}` : '';
      document.getElementById('meta').textContent = `生成时间: ${{report.generated_china || 'unknown'}} | 源数据时间: ${{report.source_generated_china || 'unknown'}} | branch: ${{meta.branch || 'unknown'}} | commit: ${{shortSha}}${{commitTitle}} | 提交人: ${{meta.actor || 'unknown'}}`;
      document.getElementById('status-line').innerHTML = [
        `<span class="pill ${{status === 'OK' ? 'ok' : 'fail'}}">状态: ${{escapeHtml(status)}}</span>`,
        `<span class="pill">统计范围: 已选 case</span>`,
        `<span class="pill">点击表头排序</span>`,
        `<span class="pill">绿色表示 yoolang 指令数更少</span>`,
        `<span class="pill">红色表示 yoolang 指令数更多</span>`,
      ].join('');
      document.getElementById('stats').innerHTML = [
        ['总 case', stats.cases],
        ['已统计 yoolang', stats.counted_yoolang_cases],
        ['yoolang 少于 GCC', stats.yoolang_wins_gcc],
        ['yoolang 少于 Clang++', stats.yoolang_wins_clang],
        ['yoolang 输给 GCC', stats.yoolang_loses_gcc],
        ['yoolang 输给 Clang++', stats.yoolang_loses_clang],
        ['失败 / 缺失', stats.failed_cases],
      ].map(([label, value]) => `<div class="stat"><span>${{label}}</span><strong>${{fmtInt(value)}}</strong></div>`).join('');
      const totalsTitle = selectedCases.size === allRows.length
        ? '整体指标'
        : `已选 case 指标（${{selectedCases.size}} / ${{allRows.length}}）`;
      document.getElementById('totals').innerHTML = `
        <h2>${{totalsTitle}}</h2>
        <div class="section-grid">
          <div class="metric"><strong>总指令数</strong><div class="muted">Yoolang ${{fmtInt(stats.total_yoolang_instructions)}} / GCC ${{fmtInt(stats.total_gcc_instructions)}} / Clang++ ${{fmtInt(stats.total_clang_instructions)}}</div></div>
          <div class="metric"><strong>总指令数加速比</strong><div class="muted">vs GCC ${{fmtSpeedup(stats.total_yoolang_vs_gcc_speedup)}} / vs Clang++ ${{fmtSpeedup(stats.total_yoolang_vs_clang_speedup)}}</div></div>
          <div class="metric"><strong>几何平均指令数加速比</strong><div class="muted">vs GCC ${{fmtSpeedup(stats.geomean_yoolang_vs_gcc_speedup)}} / vs Clang++ ${{fmtSpeedup(stats.geomean_yoolang_vs_clang_speedup)}}</div></div>
          <div class="metric"><strong>相对 baseline</strong><div class="muted">当前 ${{fmtInt(stats.baseline_current_total_insn)}} / baseline ${{fmtInt(stats.baseline_total_insn)}} / 总加速比 ${{fmtSpeedup(stats.baseline_total_speedup)}} / 几何平均 ${{fmtSpeedup(stats.baseline_geomean_speedup)}}</div></div>
        </div>
      `;
      renderTopLists(selected.top);
      renderFailures(selected.failureReasons);
    }}

    function renderTopLists(top) {{
      const item = (row) => `${{escapeHtml(row.case || '')}} <span class="muted">vs ${{escapeHtml(row.target || '')}}: ${{fmtSpeedup(row.speedup)}} (${{fmtPct(row.delta_pct)}})</span>`;
      document.getElementById('top-lists').innerHTML = [
        renderList('指令数更少 Top 10', top.instruction_less || [], item),
        renderList('指令数更多 Top 10', top.instruction_more || [], item),
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
      return allRows.filter((row) => {{
        if (query && !String(row.case || '').toLowerCase().includes(query)) return false;
        if (filter === 'win-gcc' && !winsGcc(row)) return false;
        if (filter === 'win-clang' && !winsClang(row)) return false;
        if (filter === 'win-any' && !winsGcc(row) && !winsClang(row)) return false;
        if (filter === 'lose-gcc' && !losesGcc(row)) return false;
        if (filter === 'lose-clang' && !losesClang(row)) return false;
        if (filter === 'lose-any' && !losesGcc(row) && !losesClang(row)) return false;
        if (filter === 'failed' && row.status === 'OK') return false;
        if (filter === 'improvement' && !(isNumber(row.delta_pct) && row.delta_pct > 0)) return false;
        if (filter === 'regression' && !(isNumber(row.delta_pct) && row.delta_pct < 0)) return false;
        return true;
      }}).sort(compareRows);
    }}

    function compareRows(a, b) {{
      const av = a[sortKey];
      const bv = b[sortKey];
      const an = Number.isFinite(av) || Number.isInteger(av);
      const bn = Number.isFinite(bv) || Number.isInteger(bv);
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
          <td class="num">${{fmtInt(row.yoolang_instructions)}}</td>
          <td class="num ${{compareCountClass(row.yoolang_instructions, row.gcc_instructions)}}">${{fmtInt(row.gcc_instructions)}}</td>
          <td class="num ${{compareCountClass(row.yoolang_instructions, row.clang_instructions)}}">${{fmtInt(row.clang_instructions)}}</td>
          <td class="num ${{compareCountClass(row.yoolang_instructions, row.gcc_instructions)}}">${{fmtSpeedup(row.yoolang_vs_gcc_speedup)}}</td>
          <td class="num ${{compareCountClass(row.yoolang_instructions, row.clang_instructions)}}">${{fmtSpeedup(row.yoolang_vs_clang_speedup)}}</td>
          <td class="num">${{row.baseline_insn != null ? fmtInt(row.baseline_insn) : (row.baseline_status !== 'OK' && row.baseline_status !== 'MISSING' ? row.baseline_status : 'N/A')}}</td>
          <td class="num ${{isNumber(row.delta_pct) ? (row.delta_pct > 0 ? 'better' : (row.delta_pct < 0 ? 'worse' : 'tie')) : ''}}">${{fmtPct(row.delta_pct)}}</td>
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
    document.getElementById('baseline-select').addEventListener('change', (event) => {{
      selectBaseline(event.target.value);
    }});
    document.getElementById('select-all').addEventListener('click', () => setSelected(allRows, true));
    document.getElementById('select-none').addEventListener('click', () => setSelected(allRows, false));
    document.getElementById('select-preliminary').addEventListener('click', () => {{
      selectedCases.clear();
      allRows.filter(isPreliminaryCase).forEach((row) => selectedCases.add(String(row.case || '')));
      rerenderSelection();
    }});
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
    rebuildRowsForBaseline();
    renderStats();
    renderRows();
    loadReportIndex();
  </script>
</body>
</html>
"""
    out_html.parent.mkdir(parents=True, exist_ok=True)
    out_html.write_text(html)


def main() -> int:
    args = parse_args()
    if args.perf_report.exists():
        perf = json.loads(args.perf_report.read_text())
    else:
        perf = {
            "generated_utc": "",
            "instruction_count_summary": {"status": "FAILED"},
            "rows": [],
        }
    report_index_url = args.report_index_url or default_report_index_url(args.out_html, args.pages_base_url)
    payload = build_payload(perf, report_meta(args))
    payload["report_index_url"] = report_index_url
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
