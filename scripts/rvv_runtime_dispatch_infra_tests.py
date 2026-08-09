#!/usr/bin/env python3

"""Host policy tests for the scalar Linux RVV runtime detector.

The production C translation unit is compiled with injected getauxval,
riscv_hwprobe, and prctl hooks.  No test requires an RVV-capable host and no
test executes an RVV instruction.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
SOURCE = RUNTIME / "rvv_runtime.c"


def run(
    argv: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        cwd=cwd or ROOT,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class RVVRuntimeDispatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="yoolang-rvv-runtime-")
        cls.work = Path(cls.temporary.name)
        cls.cc = os.environ.get("CC", "cc")
        if shutil.which(cls.cc) is None:
            raise unittest.SkipTest(f"host C compiler is unavailable: {cls.cc}")
        cls.harness = cls._build_mock_harness()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    @classmethod
    def _build_mock_harness(cls) -> Path:
        hooks = cls.work / "mock_hooks.h"
        hooks.write_text(
            textwrap.dedent(
                r"""
                #ifndef YOOLANG_RVV_MOCK_HOOKS_H
                #define YOOLANG_RVV_MOCK_HOOKS_H
                #include <stddef.h>
                #include <stdint.h>

                #define YOOLANG_RVV_RUNTIME_FORCE_LINUX_RISCV 1
                #define YOOLANG_RISCV_HWPROBE_STRUCT_DEFINED 1
                struct yoolang_riscv_hwprobe {
                    int64_t key;
                    uint64_t value;
                };

                unsigned long yoolang_mock_getauxval(unsigned long type);
                long yoolang_mock_hwprobe(
                    struct yoolang_riscv_hwprobe *pairs,
                    size_t pair_count,
                    size_t cpu_count,
                    unsigned long *cpus,
                    unsigned int flags);
                long yoolang_mock_get_v_control(void);

                #define YOOLANG_RVV_GETAUXVAL(type) \
                    yoolang_mock_getauxval(type)
                #define YOOLANG_RVV_HWPROBE(pairs, pair_count, cpu_count, \
                                             cpus, flags) \
                    yoolang_mock_hwprobe((pairs), (pair_count), (cpu_count), \
                                          (cpus), (flags))
                #define YOOLANG_RVV_GET_V_CONTROL() \
                    yoolang_mock_get_v_control()
                #endif
                """
            ).strip()
            + "\n"
        )

        harness_source = cls.work / "runtime_mock_harness.c"
        harness_source.write_text(
            textwrap.dedent(
                r"""
                #include "mock_hooks.h"
                #include "rvv_runtime.h"

                #include <errno.h>
                #include <pthread.h>
                #include <stdatomic.h>
                #include <stdio.h>
                #include <stdlib.h>
                #include <string.h>

                #define HWCAP_V (1UL << ('V' - 'A'))
                #define HWPROBE_KEY_IMA_EXT_0 4
                #define HWPROBE_IMA_V (1ULL << 2)
                #define VSTATE_DEFAULT 0
                #define VSTATE_OFF 1
                #define VSTATE_ON 2
                #define VSTATE_INHERIT (1 << 4)

                enum scenario {
                    SC_SUCCESS,
                    SC_NO_V,
                    SC_HWCAP_ONLY,
                    SC_HWPROBE_ONLY,
                    SC_ENOSYS_V,
                    SC_ENOSYS_NO_V,
                    SC_HWPROBE_ERROR,
                    SC_HWPROBE_BAD_RETURN,
                    SC_HWPROBE_BAD_KEY,
                    SC_AUX_ERROR,
                    SC_AUX_NONZERO_ERROR,
                    SC_PRCTL_ERROR,
                    SC_PRCTL_DEFAULT,
                    SC_PRCTL_OFF,
                    SC_PRCTL_INHERIT_ON,
                    SC_PRCTL_RESERVED,
                    SC_CHANGE_IN_THREAD,
                    SC_THREADS
                };

                static enum scenario current_scenario;
                static _Atomic int aux_calls;
                static _Atomic int hwprobe_calls;
                static _Atomic int control_calls;
                static _Thread_local long thread_control = VSTATE_ON;

                unsigned long yoolang_mock_getauxval(unsigned long type) {
                    (void)type;
                    atomic_fetch_add_explicit(&aux_calls, 1,
                                              memory_order_relaxed);
                    errno = 0;
                    switch (current_scenario) {
                    case SC_NO_V:
                    case SC_HWPROBE_ONLY:
                    case SC_ENOSYS_NO_V:
                        return 0;
                    case SC_AUX_ERROR:
                        errno = ENOENT;
                        return 0;
                    case SC_AUX_NONZERO_ERROR:
                        errno = EIO;
                        return HWCAP_V;
                    default:
                        return HWCAP_V;
                    }
                }

                long yoolang_mock_hwprobe(
                    struct yoolang_riscv_hwprobe *pairs,
                    size_t pair_count,
                    size_t cpu_count,
                    unsigned long *cpus,
                    unsigned int flags) {
                    (void)pair_count;
                    (void)cpu_count;
                    (void)cpus;
                    (void)flags;
                    atomic_fetch_add_explicit(&hwprobe_calls, 1,
                                              memory_order_relaxed);
                    errno = 0;
                    pairs[0].key = HWPROBE_KEY_IMA_EXT_0;
                    pairs[0].value = HWPROBE_IMA_V;
                    switch (current_scenario) {
                    case SC_NO_V:
                    case SC_HWCAP_ONLY:
                        pairs[0].value = 0;
                        return 0;
                    case SC_ENOSYS_V:
                    case SC_ENOSYS_NO_V:
                        errno = ENOSYS;
                        return -1;
                    case SC_HWPROBE_ERROR:
                        errno = EPERM;
                        return -1;
                    case SC_HWPROBE_BAD_RETURN:
                        return 1;
                    case SC_HWPROBE_BAD_KEY:
                        pairs[0].key = -1;
                        return 0;
                    default:
                        return 0;
                    }
                }

                long yoolang_mock_get_v_control(void) {
                    atomic_fetch_add_explicit(&control_calls, 1,
                                              memory_order_relaxed);
                    errno = 0;
                    switch (current_scenario) {
                    case SC_PRCTL_ERROR:
                        errno = EINVAL;
                        return -1;
                    case SC_PRCTL_DEFAULT:
                        return VSTATE_DEFAULT;
                    case SC_PRCTL_OFF:
                        return VSTATE_OFF;
                    case SC_PRCTL_INHERIT_ON:
                        return VSTATE_ON | VSTATE_INHERIT;
                    case SC_PRCTL_RESERVED:
                        return 3;
                    default:
                        return thread_control;
                    }
                }

                struct thread_case {
                    long control;
                    int result;
                };

                static void *run_thread_case(void *opaque) {
                    struct thread_case *test = (struct thread_case *)opaque;
                    thread_control = test->control;
                    test->result = __yoolang_rvv_available();
                    return NULL;
                }

                static enum scenario parse_scenario(const char *name) {
                    static const struct {
                        const char *name;
                        enum scenario value;
                    } scenarios[] = {
                        {"success", SC_SUCCESS},
                        {"no-v", SC_NO_V},
                        {"hwcap-only", SC_HWCAP_ONLY},
                        {"hwprobe-only", SC_HWPROBE_ONLY},
                        {"enosys-v", SC_ENOSYS_V},
                        {"enosys-no-v", SC_ENOSYS_NO_V},
                        {"hwprobe-error", SC_HWPROBE_ERROR},
                        {"hwprobe-bad-return", SC_HWPROBE_BAD_RETURN},
                        {"hwprobe-bad-key", SC_HWPROBE_BAD_KEY},
                        {"aux-error", SC_AUX_ERROR},
                        {"aux-nonzero-error", SC_AUX_NONZERO_ERROR},
                        {"prctl-error", SC_PRCTL_ERROR},
                        {"prctl-default", SC_PRCTL_DEFAULT},
                        {"prctl-off", SC_PRCTL_OFF},
                        {"prctl-inherit-on", SC_PRCTL_INHERIT_ON},
                        {"prctl-reserved", SC_PRCTL_RESERVED},
                        {"change-in-thread", SC_CHANGE_IN_THREAD},
                        {"threads", SC_THREADS},
                    };
                    size_t i;
                    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]);
                         ++i) {
                        if (strcmp(name, scenarios[i].name) == 0) {
                            return scenarios[i].value;
                        }
                    }
                    fprintf(stderr, "unknown scenario: %s\n", name);
                    exit(64);
                }

                int main(int argc, char **argv) {
                    int first;
                    int second;
                    if (argc != 2) {
                        return 64;
                    }
                    current_scenario = parse_scenario(argv[1]);

                    if (current_scenario == SC_CHANGE_IN_THREAD) {
                        thread_control = VSTATE_ON;
                        first = __yoolang_rvv_available();
                        thread_control = VSTATE_OFF;
                        second = __yoolang_rvv_available();
                        printf("%d %d %d %d %d\n", first, second,
                               atomic_load(&aux_calls),
                               atomic_load(&hwprobe_calls),
                               atomic_load(&control_calls));
                        return 0;
                    }

                    if (current_scenario == SC_THREADS) {
                        pthread_t on_thread;
                        pthread_t off_thread;
                        struct thread_case on = {VSTATE_ON, -1};
                        struct thread_case off = {VSTATE_OFF, -1};

                        thread_control = VSTATE_ON;
                        first = __yoolang_rvv_available();
                        if (pthread_create(&on_thread, NULL, run_thread_case,
                                           &on) != 0 ||
                            pthread_create(&off_thread, NULL, run_thread_case,
                                           &off) != 0) {
                            return 70;
                        }
                        if (pthread_join(on_thread, NULL) != 0 ||
                            pthread_join(off_thread, NULL) != 0) {
                            return 70;
                        }
                        printf("%d %d %d %d %d %d\n", first, on.result,
                               off.result, atomic_load(&aux_calls),
                               atomic_load(&hwprobe_calls),
                               atomic_load(&control_calls));
                        return 0;
                    }

                    first = __yoolang_rvv_available();
                    second = __yoolang_rvv_available();
                    printf("%d %d %d %d %d\n", first, second,
                           atomic_load(&aux_calls),
                           atomic_load(&hwprobe_calls),
                           atomic_load(&control_calls));
                    return 0;
                }
                """
            ).strip()
            + "\n"
        )

        runtime_object = cls.work / "rvv_runtime_mock.o"
        executable = cls.work / "runtime_mock_harness"
        common_flags = [
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{RUNTIME}",
            f"-I{cls.work}",
        ]
        run(
            [
                cls.cc,
                *common_flags,
                "-include",
                str(hooks),
                "-c",
                str(SOURCE),
                "-o",
                str(runtime_object),
            ]
        )
        run(
            [
                cls.cc,
                *common_flags,
                str(harness_source),
                str(runtime_object),
                "-o",
                str(executable),
            ]
        )
        return executable

    def invoke(self, scenario: str) -> list[int]:
        result = run([str(self.harness), scenario])
        self.assertEqual(result.stderr, "")
        return [int(field) for field in result.stdout.split()]

    def test_agreeing_capability_and_exact_current_on(self) -> None:
        # result1, result2, getauxval calls, hwprobe calls, prctl calls
        self.assertEqual(self.invoke("success"), [1, 1, 1, 1, 2])
        self.assertEqual(self.invoke("prctl-inherit-on"), [1, 1, 1, 1, 2])

    def test_absent_v_and_both_contradiction_directions_fail_closed(self) -> None:
        self.assertEqual(self.invoke("no-v"), [0, 0, 1, 1, 0])
        self.assertEqual(self.invoke("hwcap-only"), [0, 0, 1, 1, 0])
        self.assertEqual(self.invoke("hwprobe-only"), [0, 0, 1, 1, 0])

    def test_only_enosys_falls_back_to_hwcap(self) -> None:
        self.assertEqual(self.invoke("enosys-v"), [1, 1, 1, 1, 2])
        self.assertEqual(self.invoke("enosys-no-v"), [0, 0, 1, 1, 0])
        self.assertEqual(self.invoke("hwprobe-error"), [0, 0, 1, 1, 0])
        self.assertEqual(
            self.invoke("hwprobe-bad-return"), [0, 0, 1, 1, 0]
        )
        self.assertEqual(self.invoke("hwprobe-bad-key"), [0, 0, 1, 1, 0])

    def test_getauxval_errors_fail_without_later_queries(self) -> None:
        self.assertEqual(self.invoke("aux-error"), [0, 0, 1, 0, 0])
        self.assertEqual(
            self.invoke("aux-nonzero-error"), [0, 0, 1, 0, 0]
        )

    def test_vector_control_errors_off_default_and_reserved_fail_closed(self) -> None:
        self.assertEqual(self.invoke("prctl-error"), [0, 0, 1, 1, 2])
        self.assertEqual(self.invoke("prctl-default"), [0, 0, 1, 1, 2])
        self.assertEqual(self.invoke("prctl-off"), [0, 0, 1, 1, 2])
        self.assertEqual(self.invoke("prctl-reserved"), [0, 0, 1, 1, 2])

    def test_state_is_rechecked_after_a_same_thread_change(self) -> None:
        # ON, then OFF; immutable hardware is cached but state is not.
        self.assertEqual(self.invoke("change-in-thread"), [1, 0, 1, 1, 2])

    def test_state_is_thread_local_and_never_process_cached(self) -> None:
        # Main ON, child ON, child OFF, hardware calls, control calls.
        self.assertEqual(self.invoke("threads"), [1, 1, 0, 1, 1, 3])

    def test_source_has_no_instruction_probe_or_state_mutation(self) -> None:
        source = SOURCE.read_text()
        forbidden = (
            "PR_RISCV_V_SET_CONTROL",
            "__asm__",
            "asm(",
            "riscv_vector.h",
            "__riscv_v_",
            "sigaction(",
            "signal(",
            "fopen(",
        )
        for token in forbidden:
            self.assertNotIn(token, source, token)

    def test_cmake_archive_has_separate_weak_detector_member(self) -> None:
        if shutil.which("cmake") is None or shutil.which("ar") is None:
            self.skipTest("cmake/ar are unavailable")
        build = self.work / "cmake-build"
        run(
            [
                "cmake",
                "-S",
                str(RUNTIME),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_C_COMPILER={self.cc}",
            ]
        )
        run(["cmake", "--build", str(build), "--parallel", "2"])
        archive = build / "libsysy.a"
        self.assertTrue(archive.is_file())
        members = run(["ar", "t", str(archive)]).stdout.splitlines()
        sylib_members = [name for name in members if "sylib" in name]
        detector_members = [name for name in members if "rvv_runtime" in name]
        self.assertEqual(len(sylib_members), 1, members)
        self.assertEqual(len(detector_members), 1, members)
        self.assertNotEqual(sylib_members[0], detector_members[0])

        nm = run(["nm", "-g", str(archive)]).stdout
        self.assertRegex(nm, r"(?m)^\s*[0-9a-fA-F]*\s+W\s+__yoolang_rvv_available$")

    def test_strong_stub_overrides_without_extracting_detector(self) -> None:
        if shutil.which("cmake") is None or shutil.which("nm") is None:
            self.skipTest("cmake/nm are unavailable")
        build = self.work / "cmake-build"
        if not (build / "libsysy.a").is_file():
            self.test_cmake_archive_has_separate_weak_detector_member()
        archive = build / "libsysy.a"
        strong_source = self.work / "strong_stub.c"
        strong_source.write_text(
            '#include "rvv_runtime.h"\n'
            "int __yoolang_rvv_available(void) { return 37; }\n"
            "int main(void) { return __yoolang_rvv_available() != 37; }\n"
        )
        executable = self.work / "strong_stub"
        link_map = self.work / "strong_stub.map"
        run(
            [
                self.cc,
                "-std=c11",
                "-O0",
                f"-I{RUNTIME}",
                str(strong_source),
                str(archive),
                f"-Wl,-Map,{link_map}",
                "-o",
                str(executable),
            ]
        )
        run([str(executable)])
        symbols = run(["nm", "-g", str(executable)]).stdout
        self.assertRegex(symbols, r"(?m)^\s*[0-9a-fA-F]+\s+T\s+__yoolang_rvv_available$")
        self.assertNotIn("rvv_runtime.c.o)", link_map.read_text())

    def test_rv64gc_object_decodes_no_rvv_instruction(self) -> None:
        gcc = shutil.which("riscv64-linux-gnu-gcc")
        objdump = shutil.which("riscv64-linux-gnu-objdump")
        readelf = shutil.which("riscv64-linux-gnu-readelf")
        if gcc is None or objdump is None or readelf is None:
            self.skipTest("RISC-V GCC/binutils cross tools are unavailable")
        obj = self.work / "rvv_runtime_rv64gc.o"
        run(
            [
                gcc,
                "-std=c11",
                "-O3",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-tree-vectorize",
                "-march=rv64gc",
                "-mabi=lp64d",
                f"-I{RUNTIME}",
                "-c",
                str(SOURCE),
                "-o",
                str(obj),
            ]
        )
        disassembly = run([objdump, "-dr", str(obj)]).stdout
        mnemonics = re.findall(
            r"(?m)^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s+)+([a-z][a-z0-9_.]*)",
            disassembly,
        )
        self.assertTrue(mnemonics, disassembly)
        self.assertFalse(
            [mnemonic for mnemonic in mnemonics if mnemonic.startswith("v")],
            disassembly,
        )
        self.assertNotRegex(
            disassembly,
            r"\b(?:vstart|vxsat|vxrm|vcsr|vl|vtype|vlenb)\b",
        )
        attributes = run([readelf, "-A", str(obj)]).stdout
        self.assertNotRegex(attributes, r"(?:^|_)v[0-9]+p[0-9]+(?:_|\"|$)")

    def test_existing_cross_cmake_invocation_builds_scalar_archive(self) -> None:
        gcc = shutil.which("riscv64-linux-gnu-gcc")
        ar = shutil.which("riscv64-linux-gnu-ar")
        ranlib = shutil.which("riscv64-linux-gnu-ranlib")
        objdump = shutil.which("riscv64-linux-gnu-objdump")
        if (
            shutil.which("cmake") is None
            or gcc is None
            or ar is None
            or ranlib is None
            or objdump is None
        ):
            self.skipTest("RISC-V CMake cross tools are unavailable")

        build = self.work / "cross-cmake-build"
        # Keep the bare ar/ranlib spelling used by scripts/setup_env.sh.  This
        # guards its existing integration contract, not just an idealized
        # absolute-tool-path invocation.
        run(
            [
                "cmake",
                "-S",
                str(RUNTIME),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc",
                "-DCMAKE_AR=riscv64-linux-gnu-ar",
                "-DCMAKE_RANLIB=riscv64-linux-gnu-ranlib",
            ]
        )
        run(["cmake", "--build", str(build), "--parallel", "2"])
        archive = build / "libsysy.a"
        members = run([ar, "t", str(archive)]).stdout.splitlines()
        self.assertEqual(len([name for name in members if "sylib" in name]), 1)
        self.assertEqual(
            len([name for name in members if "rvv_runtime" in name]), 1
        )
        disassembly = run([objdump, "-d", str(archive)]).stdout
        mnemonics = re.findall(
            r"(?m)^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s+)+([a-z][a-z0-9_.]*)",
            disassembly,
        )
        self.assertFalse(
            [mnemonic for mnemonic in mnemonics if mnemonic.startswith("v")],
            disassembly,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
