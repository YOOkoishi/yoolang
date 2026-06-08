#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HY_REPO_URL="${HY_REPO_URL:-https://gitlab.eduxiji.net/educg-group-36290-2935672/T202500000205464-2455.git}"
HY_BASE_DIR="${HY_BASE_DIR:-$HOME/.cache/yoolang-ci/external/hy}"
HY_SRC_DIR="$HY_BASE_DIR/src"
HY_REPORT_DIR="$HY_BASE_DIR/reports"
PUBLIC_DIR="${PUBLIC_DIR:-$ROOT/public}"

mkdir -p "$HY_BASE_DIR" "$HY_REPORT_DIR"

if [ ! -d "$HY_SRC_DIR/.git" ]; then
  rm -rf "$HY_SRC_DIR"
  git clone "$HY_REPO_URL" "$HY_SRC_DIR"
else
  git -C "$HY_SRC_DIR" fetch --prune origin
fi

default_branch="$(git -C "$HY_SRC_DIR" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | sed 's#^origin/##')"
default_branch="${default_branch:-main}"
git -C "$HY_SRC_DIR" checkout -f "$default_branch"
git -C "$HY_SRC_DIR" reset --hard "origin/$default_branch"
git -C "$HY_SRC_DIR" clean -fdx

hy_commit="$(git -C "$HY_SRC_DIR" rev-parse HEAD)"
hy_title="$(git -C "$HY_SRC_DIR" show -s --format=%s HEAD)"

(
  cd "$HY_SRC_DIR"
  python3 test.py --no-execute --test test/custom/basic.sy
)

hy_bin="$HY_SRC_DIR/build/sysc"
if [ ! -x "$hy_bin" ]; then
  echo "HY compiler not found or not executable: $hy_bin" >&2
  exit 1
fi

(
  cd "$ROOT"
  mkdir -p build/github-actions
  HY_COMPILER_BIN="$hy_bin" \
  PERF_TEST_DIRS="${PERF_TEST_DIRS:-test/performance,test/bsb-final}" \
  PERF_EXCLUDE_CASES="${PERF_EXCLUDE_CASES:-test/performance/h-10-02.sy
test/performance/h-10-03.sy
test/bsb-final/2025-CPS-39.sy
test/bsb-final/2025-Z8N-28.sy}" \
  PERF_TIMEOUT_SEC="${PERF_TIMEOUT_SEC:-20}" \
  SYSY_RUNTIME_LIB="${SYSY_RUNTIME_LIB:-runtime/libsysy_riscv.a}" \
  ENABLE_QEMU_INSN_COUNT="${ENABLE_QEMU_INSN_COUNT:-0}" \
  python3 scripts/compare_perf.py 2>&1 | tee build/github-actions/hy-perf-tests.log
)

run_dir="$HY_REPORT_DIR/${hy_commit:0:12}"
mkdir -p "$run_dir"
cp "$ROOT/build/perf-ci/perf-report.json" "$run_dir/perf-report.json"
cp "$ROOT/build/perf-ci/perf-report.md" "$run_dir/perf-report.md"

python3 "$ROOT/scripts/register_external_perf_report.py" \
  --public-dir "$PUBLIC_DIR" \
  --perf-report "$run_dir/perf-report.json" \
  --name hy \
  --branch "$default_branch" \
  --commit-sha "$hy_commit" \
  --commit-title "$hy_title" \
  --source-url "$HY_REPO_URL"

echo "HY report: $run_dir/perf-report.json"
