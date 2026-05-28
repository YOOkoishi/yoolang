#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate QEMU dynamic instruction count report.")
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--out-html", required=True, type=Path)
    return parser.parse_args()


def as_int(value: Any) -> int | None:
    return value if isinstance(value, int) else None


def speedup(reference: int | None, current: int | None) -> float | None:
    if reference is None or current is None or current <= 0:
        return None
    return reference / current


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


def build_payload(perf: dict[str, Any]) -> dict[str, Any]:
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

    return {
        "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC"),
        "source_generated_utc": perf.get("generated_utc", ""),
        "status": perf.get("instruction_count_summary", {}).get("status", "UNKNOWN")
        if isinstance(perf.get("instruction_count_summary"), dict)
        else "UNKNOWN",
        "summary": {
            "cases": len(rows),
            "counted_yoolang_cases": len(counted),
            "failed_cases": len(failed),
            "yoolang_loses_gcc": len(yoolang_loses_gcc),
            "yoolang_loses_clang": len(yoolang_loses_clang),
        },
        "rows": rows,
    }


def html_escape_json(payload: dict[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=True, indent=2).replace("</", "<\\/")


def write_html(payload: dict[str, Any], out_html: Path) -> None:
    data = html_escape_json(payload)
    html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Yoolang QEMU Instruction Report</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f7f8fb;
      --panel: #ffffff;
      --text: #20242c;
      --muted: #667085;
      --line: #d9dee8;
      --accent: #006adc;
      --bad: #b42318;
      --good: #067647;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--text);
    }}
    header {{
      padding: 28px 32px 18px;
      border-bottom: 1px solid var(--line);
      background: var(--panel);
    }}
    h1 {{ margin: 0 0 8px; font-size: 28px; letter-spacing: 0; }}
    .meta {{ color: var(--muted); font-size: 14px; }}
    main {{ padding: 22px 32px 36px; }}
    .stats {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 12px;
      margin-bottom: 18px;
    }}
    .stat {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 14px 16px;
    }}
    .stat span {{ display: block; color: var(--muted); font-size: 13px; }}
    .stat strong {{ display: block; margin-top: 4px; font-size: 22px; }}
    .toolbar {{
      display: grid;
      grid-template-columns: minmax(220px, 1fr) minmax(220px, 320px);
      gap: 12px;
      margin-bottom: 12px;
    }}
    input, select {{
      width: 100%;
      min-height: 40px;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 8px 10px;
      background: var(--panel);
      color: var(--text);
      font: inherit;
    }}
    .table-wrap {{
      overflow: auto;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }}
    table {{ width: 100%; border-collapse: collapse; min-width: 980px; }}
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
    td.num {{ text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }}
    td.case {{ font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }}
    .ok {{ color: var(--good); font-weight: 600; }}
    .fail {{ color: var(--bad); font-weight: 600; }}
    .reason {{ color: var(--muted); max-width: 360px; }}
    .empty {{ padding: 24px; color: var(--muted); }}
    @media (max-width: 760px) {{
      header, main {{ padding-left: 16px; padding-right: 16px; }}
      .toolbar {{ grid-template-columns: 1fr; }}
    }}
  </style>
</head>
<body>
  <header>
    <h1>Yoolang QEMU Dynamic Instruction Report</h1>
    <div class="meta" id="meta"></div>
  </header>
  <main>
    <section class="stats" id="stats"></section>
    <section class="toolbar">
      <input id="search" type="search" placeholder="Search case name">
      <select id="filter">
        <option value="all">All cases</option>
        <option value="lose-any">Only yoolang loses to GCC or Clang++</option>
        <option value="lose-gcc">Only yoolang loses to GCC</option>
        <option value="lose-clang">Only yoolang loses to Clang++</option>
        <option value="failed">Only failed / missing counts</option>
      </select>
    </section>
    <section class="table-wrap">
      <table>
        <thead>
          <tr>
            <th data-key="case">Case</th>
            <th data-key="yoolang_instructions">Yoolang</th>
            <th data-key="gcc_instructions">GCC</th>
            <th data-key="clang_instructions">Clang++</th>
            <th data-key="yoolang_vs_gcc_speedup">Yoolang vs GCC</th>
            <th data-key="yoolang_vs_clang_speedup">Yoolang vs Clang++</th>
            <th data-key="status">Status</th>
            <th data-key="reason">Failed reason</th>
          </tr>
        </thead>
        <tbody id="rows"></tbody>
      </table>
      <div class="empty" id="empty" hidden>No matching cases.</div>
    </section>
  </main>
  <script id="report-data" type="application/json">{data}</script>
  <script>
    const report = JSON.parse(document.getElementById('report-data').textContent);
    let sortKey = 'case';
    let sortDir = 1;

    const fmtInt = (value) => Number.isInteger(value) ? value.toLocaleString() : 'N/A';
    const fmtSpeedup = (value) => Number.isFinite(value) ? `${{value.toFixed(2)}}x` : 'N/A';
    const losesGcc = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.gcc_instructions) && row.yoolang_instructions > row.gcc_instructions;
    const losesClang = (row) => Number.isInteger(row.yoolang_instructions) && Number.isInteger(row.clang_instructions) && row.yoolang_instructions > row.clang_instructions;

    function renderStats() {{
      const stats = report.summary || {{}};
      document.getElementById('meta').textContent = `Generated: ${{report.generated_utc || 'unknown'}} | Source: ${{report.source_generated_utc || 'unknown'}} | Status: ${{report.status || 'UNKNOWN'}}`;
      document.getElementById('stats').innerHTML = [
        ['Cases', stats.cases],
        ['Counted yoolang cases', stats.counted_yoolang_cases],
        ['Yoolang loses to GCC', stats.yoolang_loses_gcc],
        ['Yoolang loses to Clang++', stats.yoolang_loses_clang],
        ['Failed / missing', stats.failed_cases],
      ].map(([label, value]) => `<div class="stat"><span>${{label}}</span><strong>${{fmtInt(value)}}</strong></div>`).join('');
    }}

    function filteredRows() {{
      const query = document.getElementById('search').value.trim().toLowerCase();
      const filter = document.getElementById('filter').value;
      return (report.rows || []).filter((row) => {{
        if (query && !String(row.case || '').toLowerCase().includes(query)) return false;
        if (filter === 'lose-gcc' && !losesGcc(row)) return false;
        if (filter === 'lose-clang' && !losesClang(row)) return false;
        if (filter === 'lose-any' && !losesGcc(row) && !losesClang(row)) return false;
        if (filter === 'failed' && row.status === 'OK') return false;
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

    function renderRows() {{
      const rows = filteredRows();
      document.getElementById('empty').hidden = rows.length !== 0;
      document.getElementById('rows').innerHTML = rows.map((row) => `
        <tr>
          <td class="case">${{escapeHtml(row.case || '')}}</td>
          <td class="num">${{fmtInt(row.yoolang_instructions)}}</td>
          <td class="num">${{fmtInt(row.gcc_instructions)}}</td>
          <td class="num">${{fmtInt(row.clang_instructions)}}</td>
          <td class="num">${{fmtSpeedup(row.yoolang_vs_gcc_speedup)}}</td>
          <td class="num">${{fmtSpeedup(row.yoolang_vs_clang_speedup)}}</td>
          <td class="${{row.status === 'OK' ? 'ok' : 'fail'}}">${{escapeHtml(row.status || '')}}</td>
          <td class="reason">${{escapeHtml(row.reason || '')}}</td>
        </tr>
      `).join('');
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
    if args.perf_report.exists():
        perf = json.loads(args.perf_report.read_text())
    else:
        perf = {
            "generated_utc": "",
            "instruction_count_summary": {"status": "FAILED"},
            "rows": [],
        }
    payload = build_payload(perf)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    write_html(payload, args.out_html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
