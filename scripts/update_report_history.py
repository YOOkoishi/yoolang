#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo


CHINA_TZ = ZoneInfo("Asia/Shanghai")
MAX_HISTORY = int(os.environ.get("YOOLANG_REPORT_HISTORY_LIMIT", "1000"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Update static report history pages.")
    parser.add_argument("--public-dir", required=True, type=Path)
    parser.add_argument("--run-id", default="")
    parser.add_argument("--run-number", default="")
    parser.add_argument("--run-url", default="")
    parser.add_argument("--branch", default="")
    parser.add_argument("--commit-sha", default="")
    parser.add_argument("--commit-title", default="")
    parser.add_argument("--actor", default="")
    parser.add_argument("--perf-report", type=Path, default=None)
    parser.add_argument("--pages-base-url", default="")
    parser.add_argument("--is-main-branch", action="store_true")
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


def env_or_git(args: argparse.Namespace) -> dict[str, str]:
    commit_title = args.commit_title or os.environ.get("HEAD_COMMIT_MESSAGE", "")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = args.run_id or os.environ.get("GITHUB_RUN_ID", "")
    run_url = args.run_url or (f"https://github.com/{repository}/actions/runs/{run_id}" if repository and run_id else "")
    return {
        "run_id": run_id,
        "run_number": args.run_number or os.environ.get("GITHUB_RUN_NUMBER", ""),
        "run_url": run_url,
        "branch": args.branch or os.environ.get("GITHUB_REF_NAME", "") or git_value("branch", "--show-current"),
        "commit_sha": args.commit_sha or os.environ.get("GITHUB_SHA", "") or git_value("rev-parse", "HEAD"),
        "commit_title": (commit_title.splitlines() or [""])[0],
        "actor": args.actor or os.environ.get("GITHUB_ACTOR", "") or git_value("show", "-s", "--format=%an", "HEAD"),
    }


def load_history(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    try:
        payload = json.loads(path.read_text(errors="replace"))
    except Exception:
        return []
    rows = payload.get("runs") if isinstance(payload, dict) else payload
    return [row for row in rows if isinstance(row, dict)] if isinstance(rows, list) else []


def read_perf_summary(perf_path: Path | None) -> tuple[float | None, int | None]:
    if perf_path is None or not perf_path.exists():
        return None, None
    try:
        payload = json.loads(perf_path.read_text(errors="replace"))
    except Exception:
        return None, None
    yoolang_runtime = payload.get("compiler_total_sec")
    if not isinstance(yoolang_runtime, (int, float)):
        yoolang_runtime = None
    failures = payload.get("failures")
    if not isinstance(failures, int):
        failures = None
    return yoolang_runtime, failures


def safe_branch_name(branch: str) -> str:
    return branch.replace("/", "-")


def short_sha(value: str) -> str:
    return value[:12] if value else "unknown"


def escape_html(value: Any) -> str:
    return (
        str(value)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&#039;")
    )


def report_links(row: dict[str, Any], scope: str) -> tuple[str, str]:
    run_id = escape_html(row.get("run_id", ""))
    branch = str(row.get("branch", ""))
    is_main = bool(row.get("is_main_branch")) or branch == os.environ.get("GITHUB_REPOSITORY_DEFAULT_BRANCH", "main")

    if is_main:
        if scope == "perf":
            return f"./runs/{run_id}/", f"../instruction-report/runs/{run_id}/"
        if scope == "instruction":
            return f"../perf-report/runs/{run_id}/", f"./runs/{run_id}/"
        return f"./perf-report/runs/{run_id}/", f"./instruction-report/runs/{run_id}/"

    safe = safe_branch_name(branch)
    perf_url = f"../branches/{safe}/perf-report/runs/{run_id}/"
    insn_url = f"../branches/{safe}/instruction-report/runs/{run_id}/"
    if scope == "root":
        perf_url = f"./branches/{safe}/perf-report/runs/{run_id}/"
        insn_url = f"./branches/{safe}/instruction-report/runs/{run_id}/"
    return perf_url, insn_url


def write_history_html(public_dir: Path, runs: list[dict[str, Any]], scope: str = "root", pages_base_url: str = "") -> str:
    def _fmt_yoolang_runtime(row: dict[str, Any]) -> str:
        t = row.get("compiler_total_sec")
        if isinstance(t, (int, float)):
            return f"{t:.4f}s"
        return "N/A"

    def _fmt_failures(row: dict[str, Any]) -> str:
        f = row.get("failure_count")
        if isinstance(f, int):
            if f > 0:
                return f'<span style="color:#d32f2f;font-weight:600">{f}</span>'
            return "0"
        return "N/A"

    rows_html = "\n".join(
        (
            lambda perf_url, instruction_url: f"""
        <tr>
          <td>{escape_html(row.get('generated_china', ''))}</td>
          <td><a href="{perf_url}">性能</a> / <a href="{instruction_url}">指令数</a></td>
          <td>{escape_html(row.get('branch', ''))}</td>
          <td><code>{escape_html(short_sha(str(row.get('commit_sha', ''))))}</code><div class="muted">{escape_html(row.get('commit_title', ''))}</div></td>
          <td>{escape_html(row.get('actor', ''))}</td>
          <td>{_fmt_yoolang_runtime(row)}</td>
          <td>{_fmt_failures(row)}</td>
          <td><a href="{escape_html(row.get('run_url', '#'))}">#{escape_html(row.get('run_number') or row.get('run_id', ''))}</a></td>
        </tr>
        """
        )(*report_links(row, scope))
        for row in runs
    )

    base_url = pages_base_url.rstrip("/") if pages_base_url else ""
    if base_url:
        main_perf = f"{base_url}/perf-report/"
        main_insn = f"{base_url}/instruction-report/"
        history_url = f"{base_url}/history.html"
    elif scope == "perf":
        main_perf = "../perf-report/"
        main_insn = "../instruction-report/"
        history_url = "../history.html"
    elif scope == "instruction":
        main_perf = "../perf-report/"
        main_insn = "../instruction-report/"
        history_url = "../history.html"
    else:
        main_perf = "./perf-report/"
        main_insn = "./instruction-report/"
        history_url = "./history.html"

    if scope == "perf":
        latest_perf = "./"
        latest_instruction = "../instruction-report/"
        history_json = "../report-history.json"
    elif scope == "instruction":
        latest_perf = "../perf-report/"
        latest_instruction = "./"
        history_json = "../report-history.json"
    else:
        latest_perf = "./perf-report/"
        latest_instruction = "./instruction-report/"
        history_json = "./report-history.json"

    html = f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Yoolang CI 报告历史</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f5f7fb;
      --panel: #ffffff;
      --text: #1f2430;
      --muted: #667085;
      --line: #d8dee9;
      --accent: #0b6bcb;
    }}
    * {{ box-sizing: border-box; }}
    body {{ margin: 0; background: var(--bg); color: var(--text); font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }}
    header {{ padding: 26px 32px 18px; background: var(--panel); border-bottom: 1px solid var(--line); }}
    h1 {{ margin: 0 0 8px; font-size: 26px; letter-spacing: 0; }}
    main {{ padding: 22px 32px 36px; }}
    .muted {{ color: var(--muted); font-size: 13px; }}
    .links {{ display: flex; flex-wrap: wrap; gap: 10px; margin-top: 12px; }}
    a {{ color: var(--accent); text-decoration: none; }}
    .links a {{ border: 1px solid var(--line); border-radius: 6px; padding: 7px 10px; background: #f8fafc; }}
    .table-wrap {{ overflow: auto; background: var(--panel); border: 1px solid var(--line); border-radius: 8px; }}
    table {{ width: 100%; border-collapse: collapse; min-width: 1000px; }}
    th, td {{ padding: 10px 12px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; font-size: 14px; }}
    th {{ background: #edf2f8; white-space: nowrap; }}
    code {{ font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }}
    @media (max-width: 760px) {{ header, main {{ padding-left: 16px; padding-right: 16px; }} }}
  </style>
</head>
<body>
  <header>
    <h1>Yoolang CI 报告历史</h1>
    <div class="muted">保留最近 {MAX_HISTORY} 次 CI 的性能报告和 QEMU 动态指令数报告。</div>
    <div class="links">
      <a href="{main_perf}">最新性能报告 (main)</a>
      <a href="{main_insn}">最新指令数报告 (main)</a>
      <a href="{history_url}">历史报告索引</a>
      <a href="{latest_perf}">当前分支最新性能报告</a>
      <a href="{latest_instruction}">当前分支最新指令数报告</a>
      <a href="{history_json}">下载历史索引 JSON</a>
    </div>
  </header>
  <main>
    <section class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>生成时间</th>
            <th>报告</th>
            <th>Branch</th>
            <th>Commit</th>
            <th>提交人</th>
            <th>Yoolang 运行总耗时</th>
            <th>失败</th>
            <th>CI Run</th>
          </tr>
        </thead>
        <tbody>
          {rows_html or '<tr><td colspan="8" class="muted">暂无历史报告。</td></tr>'}
        </tbody>
      </table>
    </section>
  </main>
</body>
</html>
"""
    return html


def write_history_pages(public_dir: Path, runs: list[dict[str, Any]], pages_base_url: str = "") -> None:
    root_html = write_history_html(public_dir, runs, "root", pages_base_url)
    perf_html = write_history_html(public_dir, runs, "perf", pages_base_url)
    instruction_html = write_history_html(public_dir, runs, "instruction", pages_base_url)
    (public_dir / "index.html").write_text(root_html)
    (public_dir / "history.html").write_text(root_html)
    (public_dir / "perf-report").mkdir(parents=True, exist_ok=True)
    (public_dir / "instruction-report").mkdir(parents=True, exist_ok=True)
    (public_dir / "perf-report" / "history.html").write_text(perf_html)
    (public_dir / "instruction-report" / "history.html").write_text(instruction_html)


def main() -> int:
    args = parse_args()
    public_dir = args.public_dir
    public_dir.mkdir(parents=True, exist_ok=True)
    history_path = public_dir / "report-history.json"
    runs = load_history(history_path)
    meta = env_or_git(args)
    run_id = meta["run_id"] or datetime.now(CHINA_TZ).strftime("%Y%m%d%H%M%S")
    yoolang_runtime, failure_count = read_perf_summary(args.perf_report)
    branch = meta["branch"]
    is_main = args.is_main_branch or branch == os.environ.get("GITHUB_REPOSITORY_DEFAULT_BRANCH", "main")
    if is_main:
        perf_url = f"./perf-report/runs/{run_id}/"
        instruction_url = f"./instruction-report/runs/{run_id}/"
    else:
        safe = safe_branch_name(branch)
        perf_url = f"./branches/{safe}/perf-report/runs/{run_id}/"
        instruction_url = f"./branches/{safe}/instruction-report/runs/{run_id}/"
    current = {
        **meta,
        "run_id": run_id,
        "generated_china": datetime.now(CHINA_TZ).strftime("%Y-%m-%d %H:%M:%S CST"),
        "perf_url": perf_url,
        "instruction_url": instruction_url,
        "compiler_total_sec": yoolang_runtime,
        "failure_count": failure_count,
        "is_main_branch": is_main,
    }
    runs = [row for row in runs if str(row.get("run_id", "")) != run_id]
    runs.insert(0, current)
    runs = runs[:MAX_HISTORY]
    history_path.write_text(json.dumps({"runs": runs}, ensure_ascii=True, indent=2) + "\n")
    write_history_pages(public_dir, runs, args.pages_base_url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
