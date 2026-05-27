#include "qemu-plugin.h"

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

int qemu_plugin_install(qemu_plugin_id_t id, const struct qemu_info_t *info, int argc, char **argv)
{
    (void)info;
    (void)argc;
    (void)argv;
    atomic_init(&total_instructions, 0);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_translate);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
