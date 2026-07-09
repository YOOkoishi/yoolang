#!/usr/bin/env python3

from __future__ import annotations

import argparse
import bisect
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


WORKSPACE = Path(os.environ.get("GITHUB_WORKSPACE", Path(__file__).resolve().parents[1])).resolve()
DEFAULT_QEMU = os.environ.get("QEMU_RISCV64", os.environ.get("QEMU_BIN", "qemu-riscv64"))
DEFAULT_SYSROOT = os.environ.get("QEMU_SYSROOT", "/usr/riscv64-linux-gnu")
DEFAULT_SAMPLE_FREQUENCY = int(os.environ.get("FLAMEGRAPH_SAMPLE_FREQUENCY", "997"))
DEFAULT_TIMEOUT_SEC = int(os.environ.get("FLAMEGRAPH_TIMEOUT_SEC", "60"))
FRAME_RE = re.compile(r"^\s+[0-9a-fA-F]+\s+(.+?)(?:\s+\((.+)\))?\s*$")
EVENT_RE = re.compile(r"^\S.*?:\s+[^:]+:\s*$")
TB_PROFILE_RE = re.compile(r"QEMU_TB_PROFILE\s+vaddr=0x([0-9a-fA-F]+)\s+n_insns=(\d+)\s+count=(\d+)")


QEMU_PLUGIN_H = r"""#ifndef YOOLANG_QEMU_PLUGIN_COMPAT_H
#define YOOLANG_QEMU_PLUGIN_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#define QEMU_PLUGIN_VERSION 1

typedef uint64_t qemu_plugin_id_t;

struct qemu_info_t;
struct qemu_plugin_tb;
struct qemu_plugin_insn;

enum qemu_plugin_cb_flags {
    QEMU_PLUGIN_CB_NO_REGS,
    QEMU_PLUGIN_CB_R_REGS,
    QEMU_PLUGIN_CB_RW_REGS,
};

typedef void (*qemu_plugin_vcpu_udata_cb_t)(unsigned int vcpu_index, void *userdata);
typedef void (*qemu_plugin_udata_cb_t)(qemu_plugin_id_t id, void *userdata);

size_t qemu_plugin_tb_n_insns(const struct qemu_plugin_tb *tb);
const struct qemu_plugin_insn *qemu_plugin_tb_get_insn(const struct qemu_plugin_tb *tb, size_t idx);
uint64_t qemu_plugin_insn_vaddr(const struct qemu_plugin_insn *insn);
void qemu_plugin_register_vcpu_tb_trans_cb(
    qemu_plugin_id_t id,
    void (*cb)(qemu_plugin_id_t id, struct qemu_plugin_tb *tb));
void qemu_plugin_register_vcpu_tb_exec_cb(
    struct qemu_plugin_tb *tb,
    qemu_plugin_vcpu_udata_cb_t cb,
    enum qemu_plugin_cb_flags flags,
    void *userdata);
void qemu_plugin_register_atexit_cb(
    qemu_plugin_id_t id,
    qemu_plugin_udata_cb_t cb,
    void *userdata);

#endif
"""


TB_PROFILE_C = r"""#include "qemu-plugin.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct tb_info {
    uint64_t vaddr;
    uint64_t n_insns;
    atomic_uint_fast64_t count;
    struct tb_info *next;
};

static struct tb_info *tb_head;

static void on_tb_exec(unsigned int vcpu_index, void *userdata)
{
    (void)vcpu_index;
    struct tb_info *info = (struct tb_info *)userdata;
    atomic_fetch_add_explicit(&info->count, 1, memory_order_relaxed);
}

static void on_tb_translate(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    size_t n_insns = qemu_plugin_tb_n_insns(tb);
    struct tb_info *info = (struct tb_info *)calloc(1, sizeof(*info));
    if (info == NULL) {
        return;
    }
    info->n_insns = n_insns;
    if (n_insns > 0) {
        const struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, 0);
        info->vaddr = qemu_plugin_insn_vaddr(insn);
    }
    info->next = tb_head;
    tb_head = info;
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb_exec, QEMU_PLUGIN_CB_NO_REGS, info);
}

static void on_plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    (void)id;
    (void)userdata;
    for (struct tb_info *info = tb_head; info != NULL; info = info->next) {
        uint64_t count = atomic_load_explicit(&info->count, memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        fprintf(stderr, "QEMU_TB_PROFILE vaddr=0x%" PRIx64 " n_insns=%" PRIu64 " count=%" PRIu64 "\n",
                info->vaddr, info->n_insns, count);
    }
    fflush(stderr);
}

int qemu_plugin_install(qemu_plugin_id_t id, const struct qemu_info_t *info,
                        int argc, char **argv)
{
    (void)info;
    (void)argc;
    (void)argv;
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_translate);
    qemu_plugin_register_atexit_cb(id, on_plugin_exit, NULL);
    return 0;
}
"""


@dataclass
class FlamegraphResult:
    case: str
    status: str
    samples: int = 0
    url: str = ""
    detail: str = ""
    folded_path: str = ""
    svg_path: str = ""
    html_path: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate QEMU/perf flamegraphs for yoolang perf cases.")
    parser.add_argument("--perf-report", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--qemu", default=DEFAULT_QEMU)
    parser.add_argument("--sample-frequency", type=int, default=DEFAULT_SAMPLE_FREQUENCY)
    parser.add_argument("--timeout-sec", type=int, default=DEFAULT_TIMEOUT_SEC)
    parser.add_argument("--mode", choices=("all", "list"), default=os.environ.get("FLAMEGRAPH_MODE", "all"))
    parser.add_argument("--cases", default=os.environ.get("FLAMEGRAPH_CASES", ""))
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(errors="replace"))
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def selected_cases(rows: list[dict[str, Any]], mode: str, case_list: str) -> list[str]:
    ok_cases = [str(row.get("case", "")) for row in rows if row.get("status") == "OK" and row.get("case")]
    if mode == "list":
        wanted = {item.strip() for item in re.split(r"[,\n]+", case_list) if item.strip()}
        return [case for case in ok_cases if case in wanted]
    return ok_cases


def case_exe(case: str) -> Path:
    src = WORKSPACE / case
    return WORKSPACE / "build" / "perf-ci" / src.parent.relative_to(WORKSPACE) / src.stem / f"{src.stem}.compiler.riscv"


def case_input(case: str) -> Path | None:
    candidate = (WORKSPACE / case).with_suffix(".in")
    return candidate if candidate.exists() else None


def case_slug(case: str) -> str:
    slug = Path(case).with_suffix("").as_posix()
    return re.sub(r"[^A-Za-z0-9_.-]+", "__", slug)


def shorten(text: str, limit: int = 400) -> str:
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 3].rstrip() + "..."


def check_tool(path_or_name: str) -> bool:
    return Path(path_or_name).exists() if Path(path_or_name).is_absolute() else shutil.which(path_or_name) is not None


def run_perf(case: str, exe: Path, out_dir: Path, qemu: str, sample_frequency: int, timeout_sec: int) -> tuple[bool, str, Path]:
    perf_data = out_dir / "perf.data"
    input_file = case_input(case)
    stdin_data = input_file.read_bytes() if input_file is not None else None
    cmd = [
        "perf",
        "record",
        "-F",
        str(sample_frequency),
        "-g",
        "--output",
        str(perf_data),
        "--",
        qemu,
        "-L",
        DEFAULT_SYSROOT,
        "-jitdump",
        "-perfmap",
        str(exe),
    ]
    try:
        result = subprocess.run(
            cmd,
            input=stdin_data,
            capture_output=True,
            timeout=timeout_sec,
            cwd=out_dir,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False, f"perf record timed out after {timeout_sec}s", perf_data
    except Exception as exc:
        return False, f"perf record failed: {exc}", perf_data

    stderr = result.stderr.decode(errors="replace") if result.stderr else ""
    stdout = result.stdout.decode(errors="replace") if result.stdout else ""
    if result.returncode != 0:
        return False, shorten(stderr or stdout or f"perf record exited {result.returncode}"), perf_data
    if not perf_data.exists():
        return False, "perf.data was not produced", perf_data
    return True, "OK", perf_data


def perf_script(perf_data: Path, out_dir: Path) -> tuple[bool, str, str]:
    injected = out_dir / "perf.jit.data"
    inject = subprocess.run(
        ["perf", "inject", "-j", "-i", str(perf_data), "-o", str(injected)],
        cwd=out_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    script_input = injected if inject.returncode == 0 and injected.exists() else perf_data
    result = subprocess.run(
        ["perf", "script", "-i", str(script_input)],
        cwd=out_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return False, shorten(result.stderr or result.stdout or f"perf script exited {result.returncode}"), ""
    return True, "OK", result.stdout


def compile_tb_profile_plugin(work_dir: Path) -> Path:
    header = work_dir / "qemu-plugin.h"
    source = work_dir / "tb_profile.c"
    plugin = work_dir / "tb_profile.so"
    header.write_text(QEMU_PLUGIN_H)
    source.write_text(TB_PROFILE_C)
    subprocess.run(
        [
            "gcc",
            "-shared",
            "-fPIC",
            "-O3",
            "-I",
            str(work_dir),
            str(source),
            "-o",
            str(plugin),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return plugin


def symbol_table(exe: Path) -> tuple[list[int], list[str]]:
    nm = shutil.which("riscv64-linux-gnu-nm") or shutil.which("nm")
    if nm is None:
        return [], []
    result = subprocess.run(
        [nm, "-n", "--defined-only", str(exe)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return [], []
    addrs: list[int] = []
    names: list[str] = []
    for line in result.stdout.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) != 3 or parts[1].lower() not in {"t", "w"}:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        addrs.append(addr)
        names.append(parts[2])
    return addrs, names


def symbol_for(addr: int, addrs: list[int], names: list[str]) -> str:
    index = bisect.bisect_right(addrs, addr) - 1
    if index < 0:
        return "[unknown]"
    return names[index]


def run_tb_profile(case: str, exe: Path, out_dir: Path, qemu: str, timeout_sec: int) -> tuple[bool, str, dict[tuple[str, ...], int]]:
    input_file = case_input(case)
    stdin_data = input_file.read_bytes() if input_file is not None else None
    with tempfile.TemporaryDirectory(prefix="yoo-qemu-tb-profile-") as temp:
        try:
            plugin = compile_tb_profile_plugin(Path(temp))
        except subprocess.CalledProcessError as exc:
            return False, shorten(exc.stderr or exc.stdout or "failed to compile QEMU TB profile plugin"), {}
        except Exception as exc:
            return False, f"failed to compile QEMU TB profile plugin: {exc}", {}

        try:
            result = subprocess.run(
                [
                    qemu,
                    "-plugin",
                    str(plugin),
                    "-L",
                    DEFAULT_SYSROOT,
                    str(exe),
                ],
                input=stdin_data,
                capture_output=True,
                timeout=timeout_sec,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return False, f"QEMU TB profile timed out after {timeout_sec}s", {}
        except Exception as exc:
            return False, f"QEMU TB profile failed: {exc}", {}

    stdout = result.stdout.decode(errors="replace") if result.stdout else ""
    stderr = result.stderr.decode(errors="replace") if result.stderr else ""
    if result.returncode != 0:
        return False, shorten(stderr or stdout or f"QEMU TB profile exited {result.returncode}"), {}

    addrs, names = symbol_table(exe)
    folded: dict[tuple[str, ...], int] = {}
    for match in TB_PROFILE_RE.finditer(stdout + "\n" + stderr):
        addr = int(match.group(1), 16)
        n_insns = int(match.group(2))
        count = int(match.group(3))
        weight = max(1, n_insns) * count
        symbol = symbol_for(addr, addrs, names)
        frame = f"tb@0x{addr:x}"
        stack = ("qemu-tb-profile", symbol, frame)
        folded[stack] = folded.get(stack, 0) + weight
    if not folded:
        return False, "QEMU TB profile produced no block samples", {}
    return True, "QEMU TB profile fallback", folded


def clean_symbol(symbol: str, dso: str | None) -> str:
    symbol = symbol.strip()
    symbol = re.sub(r"\+0x[0-9a-fA-F]+$", "", symbol)
    symbol = re.sub(r"\+[^+\s]+$", "", symbol)
    if not symbol:
        symbol = "[unknown]"
    if dso and "jitted" in dso.lower() and symbol.startswith("0x"):
        return f"qemu-jit:{symbol}"
    return symbol


def collapse_perf_script(text: str) -> dict[tuple[str, ...], int]:
    folded: dict[tuple[str, ...], int] = {}
    frames: list[str] = []

    def flush() -> None:
        nonlocal frames
        if frames:
            stack = tuple(reversed(frames))
            folded[stack] = folded.get(stack, 0) + 1
        frames = []

    for line in text.splitlines():
        if not line.strip():
            flush()
            continue
        if EVENT_RE.match(line):
            flush()
            continue
        match = FRAME_RE.match(line)
        if match:
            frames.append(clean_symbol(match.group(1), match.group(2)))
    flush()
    return folded


def write_folded(folded: dict[tuple[str, ...], int], path: Path) -> int:
    samples = sum(folded.values())
    lines = [";".join(stack) + f" {count}" for stack, count in sorted(folded.items())]
    path.write_text("\n".join(lines) + ("\n" if lines else ""))
    return samples


def color_for(name: str) -> str:
    value = 0
    for ch in name:
        value = (value * 131 + ord(ch)) & 0xFFFFFFFF
    hue = 20 + (value % 38)
    sat = 58 + (value % 18)
    light = 54 + (value % 12)
    return f"hsl({hue}, {sat}%, {light}%)"


def build_tree(folded: dict[tuple[str, ...], int]) -> dict[str, Any]:
    root: dict[str, Any] = {"name": "root", "value": 0, "children": {}}
    for stack, count in folded.items():
        node = root
        node["value"] += count
        for frame in stack:
            children = node["children"]
            node = children.setdefault(frame, {"name": frame, "value": 0, "children": {}})
            node["value"] += count
    return root


def flatten_tree(node: dict[str, Any], depth: int, x: float, scale: float, rects: list[dict[str, Any]]) -> None:
    current_x = x
    for child in sorted(node["children"].values(), key=lambda item: item["value"], reverse=True):
        width = child["value"] * scale
        rects.append({"name": child["name"], "value": child["value"], "depth": depth, "x": current_x, "width": width})
        flatten_tree(child, depth + 1, current_x, scale, rects)
        current_x += width


def write_svg(folded: dict[tuple[str, ...], int], path: Path, title: str) -> int:
    samples = sum(folded.values())
    width = 1400
    frame_h = 18
    pad_x = 10
    top = 48
    if samples <= 0:
        path.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="900" height="120">'
            '<text x="20" y="55" font-family="sans-serif" font-size="16">No perf samples captured.</text>'
            "</svg>\n"
        )
        return 0

    rects: list[dict[str, Any]] = []
    flatten_tree(build_tree(folded), 0, pad_x, (width - 2 * pad_x) / samples, rects)
    max_depth = max((rect["depth"] for rect in rects), default=0)
    height = top + (max_depth + 2) * frame_h + 20
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>text{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}"
        ".frame:hover{stroke:#111;stroke-width:1.2}</style>",
        f'<text x="{pad_x}" y="24" font-family="sans-serif" font-size="18">{html.escape(title)}</text>',
        f'<text x="{pad_x}" y="42" font-family="sans-serif" font-size="12">{samples} samples</text>',
    ]
    for rect in rects:
        if rect["width"] < 0.4:
            continue
        x = rect["x"]
        y = top + rect["depth"] * frame_h
        w = max(rect["width"], 0.4)
        label = rect["name"]
        pct = (rect["value"] / samples) * 100.0
        parts.append(
            f'<g class="frame"><title>{html.escape(label)} ({rect["value"]} samples, {pct:.2f}%)</title>'
            f'<rect x="{x:.3f}" y="{y}" width="{w:.3f}" height="{frame_h - 1}" fill="{color_for(label)}" rx="2" ry="2"/>'
        )
        if w > 42:
            max_chars = max(1, int(w // 7))
            text = label if len(label) <= max_chars else label[: max_chars - 1] + "~"
            parts.append(f'<text x="{x + 4:.3f}" y="{y + 13}" fill="#111">{html.escape(text)}</text>')
        parts.append("</g>")
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n")
    return samples


def write_html(svg_name: str, folded_name: str, path: Path, case: str, samples: int) -> None:
    path.write_text(
        f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Yoolang Flamegraph - {html.escape(case)}</title>
  <style>
    body {{ margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #2e3440; color: #eceff4; }}
    header {{ padding: 18px 24px; background: #3b4252; border-bottom: 1px solid #4c566a; }}
    h1 {{ margin: 0 0 6px; font-size: 22px; letter-spacing: 0; }}
    .meta {{ color: #d8dee9; font-size: 14px; }}
    .links {{ margin-top: 10px; display: flex; gap: 10px; flex-wrap: wrap; }}
    a {{ color: #88c0d0; }}
    main {{ overflow: auto; padding: 14px; }}
    .canvas {{ display: inline-block; background: #eceff4; border: 1px solid #4c566a; }}
  </style>
</head>
<body>
  <header>
    <h1>{html.escape(case)}</h1>
    <div class="meta">QEMU/perf flamegraph, yoolang .compiler.riscv, samples: {samples}</div>
    <div class="links"><a href="{html.escape(svg_name)}">打开 SVG</a><a href="{html.escape(folded_name)}">下载 folded stacks</a></div>
  </header>
  <main><div class="canvas"><img src="{html.escape(svg_name)}" alt="flamegraph for {html.escape(case)}"></div></main>
</body>
</html>
"""
    )


def write_case_artifacts(
    case: str,
    out_root: Path,
    out_dir: Path,
    folded: dict[tuple[str, ...], int],
    detail: str,
) -> FlamegraphResult:
    folded_path = out_dir / "folded.txt"
    svg_path = out_dir / "flamegraph.svg"
    html_path = out_dir / "index.html"
    samples = write_folded(folded, folded_path)
    write_svg(folded, svg_path, f"Yoolang QEMU flamegraph: {case}")
    write_html(svg_path.name, folded_path.name, html_path, case, samples)
    status = "OK" if samples > 0 else "EMPTY"
    return FlamegraphResult(
        case=case,
        status=status,
        samples=samples,
        url=f"{out_dir.name}/",
        detail=detail if samples > 0 else "no samples captured",
        folded_path=str(folded_path.relative_to(out_root)),
        svg_path=str(svg_path.relative_to(out_root)),
        html_path=str(html_path.relative_to(out_root)),
    )


def generate_case(case: str, out_root: Path, qemu: str, sample_frequency: int, timeout_sec: int) -> FlamegraphResult:
    slug = case_slug(case)
    out_dir = out_root / slug
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = case_exe(case)
    if not exe.exists():
        return FlamegraphResult(case=case, status="MISSING", detail=f"compiler executable not found: {exe}")

    ok, detail, perf_data = run_perf(case, exe, out_dir, qemu, sample_frequency, timeout_sec)
    if not ok:
        fallback_ok, fallback_detail, folded = run_tb_profile(case, exe, out_dir, qemu, timeout_sec)
        if fallback_ok:
            return write_case_artifacts(case, out_root, out_dir, folded, f"perf unavailable ({detail}); {fallback_detail}")
        return FlamegraphResult(case=case, status="FAILED", detail=f"{detail}; fallback failed: {fallback_detail}", url=f"{slug}/")

    ok, detail, script_text = perf_script(perf_data, out_dir)
    if not ok:
        fallback_ok, fallback_detail, folded = run_tb_profile(case, exe, out_dir, qemu, timeout_sec)
        if fallback_ok:
            return write_case_artifacts(case, out_root, out_dir, folded, f"perf script unavailable ({detail}); {fallback_detail}")
        return FlamegraphResult(case=case, status="FAILED", detail=f"{detail}; fallback failed: {fallback_detail}", url=f"{slug}/")

    folded = collapse_perf_script(script_text)
    return write_case_artifacts(case, out_root, out_dir, folded, "perf call graph")


def write_index(results: list[FlamegraphResult], out_dir: Path, meta: dict[str, Any]) -> None:
    payload = {
        "status": "OK" if all(result.status in {"OK", "EMPTY", "MISSING", "FAILED"} for result in results) else "UNKNOWN",
        "generated_cases": len(results),
        "ok_cases": sum(1 for result in results if result.status == "OK"),
        "meta": meta,
        "rows": [result.__dict__ for result in results],
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "index.json").write_text(json.dumps(payload, ensure_ascii=True, indent=2) + "\n")
    rows = "\n".join(
        f'<tr><td>{html.escape(row.case)}</td><td>{html.escape(row.status)}</td>'
        f'<td>{row.samples}</td><td><a href="{html.escape(row.url)}">open</a></td>'
        f'<td>{html.escape(shorten(row.detail, 160))}</td></tr>'
        for row in results
    )
    (out_dir / "index.html").write_text(
        f"""<!doctype html>
<html lang="zh-CN">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Yoolang Flamegraphs</title>
<style>body{{font-family:ui-sans-serif,system-ui,sans-serif;margin:24px;background:#2e3440;color:#eceff4}}a{{color:#88c0d0}}table{{border-collapse:collapse;width:100%}}td,th{{border-bottom:1px solid #4c566a;padding:8px;text-align:left}}code{{color:#ebcb8b}}</style></head>
<body><h1>Yoolang Flamegraphs</h1><p><code>{html.escape(str(meta.get("source_report", "")))}</code></p>
<table><thead><tr><th>Case</th><th>Status</th><th>Samples</th><th>Flamegraph</th><th>Detail</th></tr></thead><tbody>{rows}</tbody></table></body></html>
"""
    )


def main() -> int:
    args = parse_args()
    args.perf_report = args.perf_report.resolve()
    args.out_dir = args.out_dir.resolve()
    perf = read_json(args.perf_report)
    rows = [row for row in perf.get("rows", []) if isinstance(row, dict)]
    cases = selected_cases(rows, args.mode, args.cases)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    if not check_tool("perf"):
        results = [FlamegraphResult(case=case, status="SKIPPED", detail="perf not found") for case in cases]
    elif not check_tool(args.qemu):
        results = [FlamegraphResult(case=case, status="SKIPPED", detail=f"qemu not found: {args.qemu}") for case in cases]
    else:
        results = [
            generate_case(case, args.out_dir, args.qemu, args.sample_frequency, args.timeout_sec)
            for case in cases
        ]

    write_index(
        results,
        args.out_dir,
        {
            "source_report": str(args.perf_report),
            "qemu": args.qemu,
            "sample_frequency": args.sample_frequency,
            "timeout_sec": args.timeout_sec,
            "mode": args.mode,
        },
    )
    ok = sum(1 for result in results if result.status == "OK")
    print(f"Generated QEMU flamegraphs: {ok}/{len(results)} OK, index: {args.out_dir / 'index.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
