#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


HOST_TEST = r"""
#include "alias_runtime.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define REQUIRE(expr) do {                                                        \
    if (!(expr)) {                                                               \
        fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expr);                                                          \
        return 1;                                                                \
    }                                                                            \
} while (0)

static int test_integer_ranges(void) {
    const uintptr_t high = UINTPTR_MAX;

    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 4, 1, 4,
                                               0x1010, 4, 1, 4) == 1);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 4, 1, 4,
                                               0x100c, 4, 1, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 4, 1, 4,
                                               0x1000, 4, 1, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x100c, 4, -1, 4,
                                               0x1010, 1, 0, 4) == 1);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x100c, 4, -1, 4,
                                               0x1008, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 8, 0, 4,
                                               0x1004, 8, 0, 4) == 1);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 8, 0, 4,
                                               0x1000, 8, 0, 4) == 0);

    /* Empty streams are disjoint and never inspect their fabricated bases. */
    REQUIRE(__yoolang_uintptr_ranges_disjoint(high, 0, 1, 4,
                                               high, 1, 1, 4) == 1);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0, 1, 1, 4,
                                               high, 0, 1, 4) == 1);

    /* A representable last byte is insufficient if the half-open end wraps. */
    REQUIRE(__yoolang_uintptr_ranges_disjoint(high - 3, 1, 0, 4,
                                               0x1000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(high - 7, 1, 0, 4,
                                               high - 3, 1, 0, 2) == 1);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(high - 7, 2, 1, 4,
                                               0x1000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(3, 2, -1, 4,
                                               0x1000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, INT32_MAX, INT32_MAX,
                                               INT32_MAX, 0x2000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 2, INT32_MIN, 4,
                                               0x2000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, -1, 1, 4,
                                               0x2000, 1, 0, 4) == 0);
    REQUIRE(__yoolang_uintptr_ranges_disjoint(0x1000, 1, 1, 0,
                                               0x2000, 1, 0, 4) == 0);
    return 0;
}

static int test_guard_pages_are_not_dereferenced(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    REQUIRE(page_size > 0 && page_size <= INT32_MAX);
    size_t bytes = (size_t)page_size * 3U;
    unsigned char *mapping = mmap(NULL, bytes, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(mapping != MAP_FAILED);

    REQUIRE(__yoolang_ranges_disjoint(mapping, 0, 1, 4,
                                       mapping + page_size, 0, 1, 4) == 1);
    REQUIRE(__yoolang_ranges_disjoint(mapping, (int32_t)(page_size / 4), 1, 4,
                                       mapping + page_size, 1, 0, 4) == 1);
    REQUIRE(__yoolang_ranges_disjoint(mapping + page_size, 1, 0, 4,
                                       mapping + page_size, 1, 0, 4) == 0);
    REQUIRE(__yoolang_ranges_disjoint(mapping + page_size - 4, 2, 1, 4,
                                       mapping + 2 * page_size, 1, 0, 4) == 1);
    REQUIRE(munmap(mapping, bytes) == 0);
    return 0;
}

static void scalar_copy(int *source, int *destination, int count) {
    for (int i = 0; i < count; ++i)
        destination[i] = source[i] + 7;
}

static void versioned_copy(int *source, int *destination, int count, int vlmax,
                           int *took_fast) {
    *took_fast = __yoolang_ranges_disjoint(source, count, 1, 4,
                                            destination, count, 1, 4);
    if (!*took_fast) {
        scalar_copy(source, destination, count);
        return;
    }
    int iv = 0;
    int remaining = count > 0 ? count : 0;
    while (remaining != 0) {
        int actual_vl = remaining < vlmax ? remaining : vlmax;
        for (int lane = 0; lane < actual_vl; ++lane)
            destination[iv + lane] = source[iv + lane] + 7;
        iv += actual_vl;
        remaining -= actual_vl;
    }
}

static int test_fast_slow_boundary_model(void) {
    const int counts[] = {0, 1, 3, 4, 5};
    for (unsigned count_index = 0;
         count_index < sizeof(counts) / sizeof(counts[0]); ++count_index) {
        int count = counts[count_index];
        for (int scenario = 0; scenario < 3; ++scenario) {
            int expected[32];
            int actual[32];
            for (int i = 0; i < 32; ++i)
                expected[i] = actual[i] = i * 3 - 17;

            int *expected_source = expected;
            int *actual_source = actual;
            int *expected_destination = scenario == 0 ? expected + 16
                                                       : expected + (scenario == 1 ? 0 : 1);
            int *actual_destination = scenario == 0 ? actual + 16
                                                     : actual + (scenario == 1 ? 0 : 1);
            scalar_copy(expected_source, expected_destination, count);
            int took_fast = -1;
            versioned_copy(actual_source, actual_destination, count, 4, &took_fast);
            REQUIRE(memcmp(actual, expected, sizeof(actual)) == 0);
            if (count == 0)
                REQUIRE(took_fast == 1);
            else if (scenario == 0 || (scenario == 2 && count == 1))
                REQUIRE(took_fast == 1);
            else
                REQUIRE(took_fast == 0);
        }
    }
    return 0;
}

int main(void) {
    if (test_integer_ranges() != 0)
        return 1;
    puts("PASS alias_integer_ranges_disjoint_overlap_exact_zero_overflow");
    if (test_guard_pages_are_not_dereferenced() != 0)
        return 1;
    puts("PASS alias_guard_pages_no_dereference");
    if (test_fast_slow_boundary_model() != 0)
        return 1;
    puts("PASS alias_fast_slow_model_0_1_vlmax_boundaries");
    return 0;
}
"""


def main() -> int:
    cc = os.environ.get("CC") or shutil.which("cc")
    if not cc:
        print("FAIL host C compiler is unavailable", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="rvv-alias-runtime-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "alias_runtime_test.c"
        binary = tmp_dir / "alias_runtime_test"
        source.write_text(textwrap.dedent(HOST_TEST), encoding="utf-8")
        command = [
            cc,
            "-std=c11",
            "-D_GNU_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=undefined",
            "-fno-sanitize-recover=undefined",
            "-I",
            str(ROOT / "runtime"),
            str(source),
            str(ROOT / "runtime/alias_runtime.c"),
            "-o",
            str(binary),
        ]
        build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
        if build.returncode != 0:
            print(build.stdout, end="")
            print(build.stderr, end="", file=sys.stderr)
            return build.returncode
        run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, check=False)
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
