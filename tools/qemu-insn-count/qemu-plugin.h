#ifndef YOOLANG_QEMU_PLUGIN_COMPAT_H
#define YOOLANG_QEMU_PLUGIN_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#define QEMU_PLUGIN_VERSION 1

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
