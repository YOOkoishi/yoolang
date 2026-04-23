#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="${MODE:-ir}"

COMPILER_BIN="${COMPILER_BIN:-}"
if [[ -z "$COMPILER_BIN" ]]; then
  COMPILER_BIN="$(find "$ROOT_DIR/build" -type f -name yoolang | head -n 1 || true)"
fi
if [[ -z "$COMPILER_BIN" || ! -x "$COMPILER_BIN" ]]; then
  xmake >/dev/null
  COMPILER_BIN="$(find "$ROOT_DIR/build" -type f -name yoolang | head -n 1 || true)"
fi
if [[ -z "$COMPILER_BIN" || ! -x "$COMPILER_BIN" ]]; then
  echo "error: cannot find built yoolang binary" >&2
  exit 1
fi

QEMU_BIN="${QEMU_BIN:-$(command -v qemu-riscv64 || true)}"
LINUX_GCC="${RISCV_LINUX_GCC:-$(command -v riscv64-linux-gnu-gcc || true)}"
ELF_GCC="${RISCV_ELF_GCC:-$(command -v riscv64-elf-gcc || true)}"
RUNTIME_PATTERN='\b(getint|getch|getfloat|getarray|getfarray|putint|putch|putfloat|putarray|putfarray|starttime|stoptime)\b'

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

usage() {
  cat <<'EOF'
Usage: test/run_tests.sh [--mode ir|run] [test1.sy test2.sy ...]

Modes:
  ir   Generate IR with --emit-ir and fail on compiler stderr / non-zero exit.
  run  Generate ASM, link, run under qemu, and compare against .out.
EOF
}

compile_ir() {
  local src="$1"
  local ir_file="$TMP_DIR/$(basename "${src%.sy}").ir"
  local err_file="$TMP_DIR/$(basename "${src%.sy}").err"

  if "$COMPILER_BIN" "$src" --emit-ir >"$ir_file" 2>"$err_file"; then
    if [[ -s "$err_file" ]]; then
      echo "FAIL: $src" >&2
      sed -n '1,20p' "$err_file" >&2
      return 1
    fi
    return 0
  fi

  echo "FAIL: $src" >&2
  sed -n '1,20p' "$err_file" >&2
  return 1
}

compile_and_run() {
  local src="$1"
  local asm="$TMP_DIR/$(basename "${src%.sy}").s"
  local exe="$TMP_DIR/$(basename "${src%.sy}").elf"
  local stdout_file="$TMP_DIR/$(basename "${src%.sy}").stdout"
  local actual_file="$TMP_DIR/$(basename "${src%.sy}").actual"
  local input_file="${src%.sy}.in"

  "$COMPILER_BIN" "$src" >"$asm"

  if [[ -n "$LINUX_GCC" ]]; then
    "$LINUX_GCC" -static "$asm" "$ROOT_DIR/runtime/libsysy_riscv.a" -o "$exe"
  elif [[ -n "$ELF_GCC" ]]; then
    if rg -q "$RUNTIME_PATTERN" "$src"; then
      return 2
    fi
    "$ELF_GCC" -nostdlib -nostartfiles "$ROOT_DIR/runtime/crt0_rv64.S" "$asm" -o "$exe"
  else
    return 3
  fi

  if [[ -z "$QEMU_BIN" ]]; then
    return 3
  fi

  local status=0
  if [[ -f "$input_file" ]]; then
    "$QEMU_BIN" "$exe" <"$input_file" >"$stdout_file" || status=$?
  else
    "$QEMU_BIN" "$exe" >"$stdout_file" || status=$?
  fi

  cat "$stdout_file" >"$actual_file"
  if [[ -s "$stdout_file" && "$(tail -c1 "$stdout_file")" != $'\n' ]]; then
    printf '\n' >>"$actual_file"
  fi
  printf '%s\n' "$status" >>"$actual_file"

  local expect_file="${src%.sy}.out"
  if [[ ! -f "$expect_file" ]]; then
    echo "warning: missing expected output for $src" >&2
    return 4
  fi

  if cmp -s "$actual_file" "$expect_file"; then
    return 0
  fi

  echo "FAIL: $src" >&2
  echo "--- expected ---" >&2
  sed -n '1,20p' "$expect_file" >&2
  echo "--- actual ---" >&2
  sed -n '1,20p' "$actual_file" >&2
  return 1
}

TESTS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="$2"
      shift 2
      ;;
    --ir)
      MODE="ir"
      shift
      ;;
    --run)
      MODE="run"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      TESTS+=("$1")
      shift
      ;;
  esac
done

if [[ "$MODE" != "ir" && "$MODE" != "run" ]]; then
  echo "error: unsupported mode '$MODE'" >&2
  usage >&2
  exit 1
fi

if [[ ${#TESTS[@]} -eq 0 ]]; then
  while IFS= read -r file; do
    TESTS+=("$file")
  done < <(find test -type f -name '*.sy' | sort)
fi

passed=0
failed=0
skipped=0

for test_file in "${TESTS[@]}"; do
  if [[ "$MODE" == "ir" ]]; then
    if compile_ir "$test_file"; then
      printf 'PASS %s\n' "$test_file"
      passed=$((passed + 1))
    else
      printf 'FAIL %s\n' "$test_file"
      failed=$((failed + 1))
    fi
  else
    if compile_and_run "$test_file"; then
      printf 'PASS %s\n' "$test_file"
      passed=$((passed + 1))
      continue
    fi

    rc=$?
    if [[ $rc -eq 2 ]]; then
      printf 'SKIP %s (runtime-dependent test requires riscv64-linux-gnu-gcc)\n' "$test_file"
      skipped=$((skipped + 1))
    elif [[ $rc -eq 3 ]]; then
      printf 'SKIP %s (missing runnable RISC-V toolchain)\n' "$test_file"
      skipped=$((skipped + 1))
    elif [[ $rc -eq 4 ]]; then
      printf 'SKIP %s (missing .out reference)\n' "$test_file"
      skipped=$((skipped + 1))
    else
      printf 'FAIL %s\n' "$test_file"
      failed=$((failed + 1))
    fi
  fi
done

printf '\nPassed: %d\nFailed: %d\nSkipped: %d\n' "$passed" "$failed" "$skipped"

if [[ $failed -ne 0 ]]; then
  exit 1
fi
