#!/usr/bin/env python3
"""
QEMU instruction-count plugin wrapper.

Stores the C source of a QEMU TCG plugin inline, compiles it on-the-fly
in a system temp directory with gcc, runs QEMU with the compiled plugin,
and cleans up all temporary files afterward.

Usage:
    python count_insn.py <qemu-bin> [qemu-args...]
"""

import os
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# QEMU plugin header -- inline replacement for qemu-plugin.h
# ---------------------------------------------------------------------------
# QEMU plugin API version: 1 for QEMU 8.x, 2 for QEMU 9.x
# Override via QEMU_INSN_API_VERSION env var if needed
_QEMU_API_VER = os.environ.get("QEMU_INSN_API_VERSION", "1").strip()

QEMU_PLUGIN_H = (
    f"""#ifndef YOOLANG_QEMU_PLUGIN_COMPAT_H
#define YOOLANG_QEMU_PLUGIN_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#define QEMU_PLUGIN_VERSION {_QEMU_API_VER}
"""
    + r"""

typedef uint64_t qemu_plugin_id_t;

struct qemu_info_t;
struct qemu_plugin_tb;

enum qemu_plugin_cb_flags {
    QEMU_PLUGIN_CB_NO_REGS,
    QEMU_PLUGIN_CB_R_REGS,
    QEMU_PLUGIN_CB_RW_REGS,
};

typedef void (*qemu_plugin_vcpu_udata_cb_t)(unsigned int vcpu_index, void *userdata);
typedef void (*qemu_plugin_udata_cb_t)(qemu_plugin_id_t id, void *userdata);

size_t qemu_plugin_tb_n_insns(const struct qemu_plugin_tb *tb);
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
)

# ---------------------------------------------------------------------------
# QEMU plugin source -- counts dynamic instructions via TCG callbacks
# ---------------------------------------------------------------------------
INSN_COUNT_C = r"""#include "qemu-plugin.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static atomic_uint_fast64_t total_instructions;

static void on_tb_exec(unsigned int vcpu_index, void *userdata)
{
    (void)vcpu_index;
    atomic_fetch_add_explicit(
        &total_instructions,
        (uint64_t)(uintptr_t)userdata,
        memory_order_relaxed);
}

static void on_tb_translate(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;
    size_t insns = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_cb(
        tb,
        on_tb_exec,
        QEMU_PLUGIN_CB_NO_REGS,
        (void *)(uintptr_t)insns);
}

static void on_exit(qemu_plugin_id_t id, void *userdata)
{
    (void)id;
    (void)userdata;
    uint64_t total = atomic_load_explicit(&total_instructions, memory_order_relaxed);
    fprintf(stderr, "QEMU_INSN_COUNT total_instructions=%" PRIu64 "\n", total);
    fflush(stderr);
}

int qemu_plugin_install(qemu_plugin_id_t id, const struct qemu_info_t *info,
                        int argc, char **argv)
{
    (void)info;
    (void)argc;
    (void)argv;
    atomic_init(&total_instructions, 0);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_translate);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
"""

ALLOWED_GCC_NAMES = ("gcc",)


def _find_gcc() -> str:
    """Return the path to a usable gcc, or exit if none is found."""
    for name in ALLOWED_GCC_NAMES:
        path = shutil.which(name)
        if path:
            return path
    print("count_insn: gcc not found on PATH", file=sys.stderr)
    sys.exit(1)


def compile_plugin(work_dir: str) -> str:
    """Write C sources into *work_dir*, compile them, and return the .so path."""
    header_path = os.path.join(work_dir, "qemu-plugin.h")
    source_path = os.path.join(work_dir, "insn_count.c")
    output_path = os.path.join(work_dir, "insn_count.so")

    with open(header_path, "w") as f:
        f.write(QEMU_PLUGIN_H)
    with open(source_path, "w") as f:
        f.write(INSN_COUNT_C)

    gcc = _find_gcc()
    compile_cmd = [
        gcc,
        "-shared",
        "-fPIC",
        "-O3",
        "-I", work_dir,
        source_path,
        "-o", output_path,
    ]

    try:
        subprocess.run(compile_cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        print(f"count_insn: gcc failed: {exc.stderr}", file=sys.stderr)
        sys.exit(1)

    return output_path


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: python count_insn.py <qemu-bin> [qemu-args...]", file=sys.stderr)
        sys.exit(1)

    qemu_cmd = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="yoo-qemu-plugin-") as temp_dir:
        plugin_so = compile_plugin(temp_dir)

        # Insert "-plugin <so>" right after the qemu binary name
        full_cmd = [qemu_cmd[0], "-plugin", plugin_so] + qemu_cmd[1:]

        sys.exit(subprocess.call(full_cmd))


if __name__ == "__main__":
    main()
