/*
 * Linux RVV availability probe for yoolang fat binaries.
 *
 * This translation unit deliberately contains no RVV intrinsic, inline
 * assembly, signal probe, /proc parser, or vector-state mutation.  Keep it in
 * a separate archive member so an application-provided strong implementation
 * can replace the weak production definition without pulling this object in.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rvv_runtime.h"

#if defined(__GNUC__) || defined(__clang__)
#define YOOLANG_WEAK_DEFAULT __attribute__((weak, visibility("default")))
#else
#define YOOLANG_WEAK_DEFAULT
#endif

#if (defined(__linux__) && defined(__riscv)) ||                         \
    defined(YOOLANG_RVV_RUNTIME_FORCE_LINUX_RISCV)

#include <elf.h>
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/prctl.h>
#endif

#if defined(__riscv)
#include <asm/unistd.h>
#include <sys/syscall.h>
#endif

/* These values are part of the Linux RISC-V UAPI.  Defining compatibility
 * fallbacks lets the scalar detector build with older libc kernel headers. */
#ifndef PR_RISCV_V_GET_CONTROL
#define PR_RISCV_V_GET_CONTROL 70
#endif
#ifndef PR_RISCV_V_VSTATE_CTRL_OFF
#define PR_RISCV_V_VSTATE_CTRL_OFF 1
#endif
#ifndef PR_RISCV_V_VSTATE_CTRL_ON
#define PR_RISCV_V_VSTATE_CTRL_ON 2
#endif
#ifndef PR_RISCV_V_VSTATE_CTRL_CUR_MASK
#define PR_RISCV_V_VSTATE_CTRL_CUR_MASK 0x3
#endif

#define YOOLANG_RISCV_HWCAP_ISA_V (1UL << ('V' - 'A'))
#define YOOLANG_RISCV_HWPROBE_KEY_IMA_EXT_0 4LL
#define YOOLANG_RISCV_HWPROBE_IMA_V (1ULL << 2)

#ifndef YOOLANG_RISCV_HWPROBE_STRUCT_DEFINED
#define YOOLANG_RISCV_HWPROBE_STRUCT_DEFINED 1
struct yoolang_riscv_hwprobe {
    int64_t key;
    uint64_t value;
};
#endif

#ifndef YOOLANG_RVV_GETAUXVAL
#define YOOLANG_RVV_GETAUXVAL(type) getauxval(type)
#endif

#ifndef YOOLANG_RVV_HWPROBE
static long yoolang_rvv_default_hwprobe(struct yoolang_riscv_hwprobe *pairs,
                                        size_t pair_count,
                                        size_t cpu_count,
                                        unsigned long *cpus,
                                        unsigned int flags) {
#if defined(SYS_riscv_hwprobe)
    return syscall(SYS_riscv_hwprobe, pairs, pair_count, cpu_count, cpus,
                   flags);
#elif defined(__NR_riscv_hwprobe)
    return syscall(__NR_riscv_hwprobe, pairs, pair_count, cpu_count, cpus,
                   flags);
#elif defined(__riscv)
    /* __NR_arch_specific_syscall is 244 in the generic Linux syscall UAPI;
     * riscv_hwprobe is RISC-V arch syscall 14. */
    return syscall(258, pairs, pair_count, cpu_count, cpus, flags);
#else
    (void)pairs;
    (void)pair_count;
    (void)cpu_count;
    (void)cpus;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}
#define YOOLANG_RVV_HWPROBE(pairs, pair_count, cpu_count, cpus, flags) \
    yoolang_rvv_default_hwprobe((pairs), (pair_count), (cpu_count), (cpus), \
                                (flags))
#endif

#ifndef YOOLANG_RVV_GET_V_CONTROL
#define YOOLANG_RVV_GET_V_CONTROL() prctl(PR_RISCV_V_GET_CONTROL, 0L, 0L, 0L, 0L)
#endif

enum {
    YOOLANG_RVV_HARDWARE_UNKNOWN = -1,
    YOOLANG_RVV_HARDWARE_NO = 0,
    YOOLANG_RVV_HARDWARE_YES = 1,
};

static _Atomic int yoolang_rvv_hardware_cache =
    YOOLANG_RVV_HARDWARE_UNKNOWN;

static int yoolang_rvv_probe_hardware(void) {
    unsigned long hwcap;
    int hwcap_has_v;
    int hwprobe_has_v;
    long result;
    int probe_errno;
    struct yoolang_riscv_hwprobe pair;

    errno = 0;
    hwcap = YOOLANG_RVV_GETAUXVAL(AT_HWCAP);
    if (errno != 0) {
        return YOOLANG_RVV_HARDWARE_NO;
    }
    hwcap_has_v = (hwcap & YOOLANG_RISCV_HWCAP_ISA_V) != 0;

    pair.key = YOOLANG_RISCV_HWPROBE_KEY_IMA_EXT_0;
    pair.value = 0;
    errno = 0;
    result = YOOLANG_RVV_HWPROBE(&pair, 1, 0, NULL, 0);
    probe_errno = errno;

    if (result == -1 && probe_errno == ENOSYS) {
        return hwcap_has_v ? YOOLANG_RVV_HARDWARE_YES
                           : YOOLANG_RVV_HARDWARE_NO;
    }
    if (result != 0 || pair.key != YOOLANG_RISCV_HWPROBE_KEY_IMA_EXT_0) {
        return YOOLANG_RVV_HARDWARE_NO;
    }

    hwprobe_has_v = (pair.value & YOOLANG_RISCV_HWPROBE_IMA_V) != 0;
    if (hwcap_has_v != hwprobe_has_v) {
        return YOOLANG_RVV_HARDWARE_NO;
    }
    return hwcap_has_v ? YOOLANG_RVV_HARDWARE_YES
                       : YOOLANG_RVV_HARDWARE_NO;
}

static int yoolang_rvv_hardware_available(void) {
    int cached = atomic_load_explicit(&yoolang_rvv_hardware_cache,
                                      memory_order_acquire);
    int detected;
    int expected;

    if (cached != YOOLANG_RVV_HARDWARE_UNKNOWN) {
        return cached;
    }

    detected = yoolang_rvv_probe_hardware();
    expected = YOOLANG_RVV_HARDWARE_UNKNOWN;
    if (!atomic_compare_exchange_strong_explicit(
            &yoolang_rvv_hardware_cache, &expected, detected,
            memory_order_release, memory_order_acquire)) {
        return expected;
    }
    return detected;
}

YOOLANG_WEAK_DEFAULT int __yoolang_rvv_available(void) {
    long control;

    if (!yoolang_rvv_hardware_available()) {
        return 0;
    }

    /* Vector control is per-thread and can change.  Query it on every call;
     * a process-global cache here would permit RVV in a disabled thread. */
    errno = 0;
    control = YOOLANG_RVV_GET_V_CONTROL();
    if (control < 0) {
        return 0;
    }
    return (control & PR_RISCV_V_VSTATE_CTRL_CUR_MASK) ==
           PR_RISCV_V_VSTATE_CTRL_ON;
}

#else

YOOLANG_WEAK_DEFAULT int __yoolang_rvv_available(void) {
    return 0;
}

#endif
