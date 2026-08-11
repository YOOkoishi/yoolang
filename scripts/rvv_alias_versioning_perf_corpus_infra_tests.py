#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
SOURCE = ROOT / "test/performance/01_mm3.sy"
INPUT = ROOT / "test/performance/01_mm3.in"
EXPECTED = ROOT / "test/performance/01_mm3.out"
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)


DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STRIDE = 1024, MAX_N = 65, CELLS = (MAX_N + 1) * STRIDE + 64 };

extern void mm(int n, int (*a)[STRIDE], int (*b)[STRIDE], int (*c)[STRIDE]);

static void reference_mm(int n, int (*a)[STRIDE], int (*b)[STRIDE],
                         int (*c)[STRIDE]) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            c[i][j] = 0;
    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < n; ++i) {
            if (a[i][k] == 1)
                continue;
            for (int j = 0; j < n; ++j)
                c[i][j] = c[i][j] * a[i][k] + b[k][j];
        }
    }
}

static int run_case(int scenario, int n, uint64_t *checksum) {
    int *actual[3];
    int *expected[3];
    for (int buffer = 0; buffer < 3; ++buffer) {
        actual[buffer] = (int *)malloc(sizeof(int) * CELLS);
        expected[buffer] = (int *)malloc(sizeof(int) * CELLS);
        if (!actual[buffer] || !expected[buffer])
            return 90;
        for (int index = 0; index < CELLS; ++index) {
            int value = ((index + 5 * buffer) % 5 == 0) ? 1 : 0;
            actual[buffer][index] = value;
            expected[buffer][index] = value;
        }
    }

    int a_buffer = 0, b_buffer = 1, c_buffer = 2;
    int a_offset = 0, b_offset = 0, c_offset = 0;
    if (scenario == 1) {
        c_buffer = a_buffer;             /* exact C == A */
    } else if (scenario == 2) {
        c_buffer = b_buffer;             /* exact C == B */
    } else if (scenario == 3) {
        c_buffer = b_buffer;
        c_offset = 1;                    /* forward-overlapping C == B + 1 */
    } else if (scenario == 4) {
        c_buffer = a_buffer;
        c_offset = 1;                    /* forward-overlapping C == A + 1 */
    }

    int (*actual_a)[STRIDE] = (int (*)[STRIDE])(actual[a_buffer] + a_offset);
    int (*actual_b)[STRIDE] = (int (*)[STRIDE])(actual[b_buffer] + b_offset);
    int (*actual_c)[STRIDE] = (int (*)[STRIDE])(actual[c_buffer] + c_offset);
    int (*expected_a)[STRIDE] = (int (*)[STRIDE])(expected[a_buffer] + a_offset);
    int (*expected_b)[STRIDE] = (int (*)[STRIDE])(expected[b_buffer] + b_offset);
    int (*expected_c)[STRIDE] = (int (*)[STRIDE])(expected[c_buffer] + c_offset);

    reference_mm(n, expected_a, expected_b, expected_c);
    mm(n, actual_a, actual_b, actual_c);
    for (int buffer = 0; buffer < 3; ++buffer) {
        if (memcmp(actual[buffer], expected[buffer], sizeof(int) * CELLS) != 0)
            return 10 + scenario;
        for (int index = 0; index < CELLS; index += 997)
            *checksum = *checksum * UINT64_C(1315423911) +
                        (uint32_t)actual[buffer][index];
        free(actual[buffer]);
        free(expected[buffer]);
    }
    return 0;
}

int main(void) {
    static const int counts[] = {0, 1, 3, 7, 8, 9, 15, 16, 17,
                                 31, 32, 33, 63, 64, 65};
    uint64_t checksum = 0;
    for (int scenario = 0; scenario < 5; ++scenario) {
        for (unsigned index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
            int status = run_case(scenario, counts[index], &checksum);
            if (status != 0) {
                fprintf(stderr, "scenario=%d n=%d status=%d\n", scenario, counts[index], status);
                return status;
            }
        }
    }
    printf("PASS actual_01_mm3_disjoint_exact_partial checksum=%llu\n",
           (unsigned long long)checksum);
    return 0;
}
"""


class Failure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def tool(name: str) -> str:
    value = shutil.which(name)
    if value is None:
        raise FileNotFoundError(name)
    return value


def run(command: list[str], *, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise Failure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def decoded_vector_mnemonics(disassembly: str) -> set[str]:
    return set(
        re.findall(r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b", disassembly)
    )


def verify_plan(text: str) -> None:
    plans = json.loads(text).get("vectorization_plans")
    require(isinstance(plans, list), "plan JSON has no vectorization_plans list")
    successes = [
        item
        for item in plans
        if item.get("vectorizer") == "loop"
        and item.get("code") == "VECTORIZED"
        and item.get("function") == "mm"
        and item.get("region") == "while.cond.16"
    ]
    require(len(successes) == 1, "01_mm3 mm:while.cond.16 must vectorize exactly once")
    success = successes[0]
    require(
        success.get("explanation")
        == "transformed verified scalable VLA fast path with overflow-safe runtime alias "
        "versioning for 2 complete byte-range pair(s); preserved scalar slow path",
        "01_mm3 success remark does not identify verified two-pair versioning",
    )
    choice = success.get("plan")
    require(isinstance(choice, dict), "01_mm3 success has no plan")
    require(choice.get("scalable") is True, "01_mm3 plan is not scalable")
    require(choice.get("runtime_alias_check") is True, "01_mm3 plan has no alias guard")
    require(choice.get("uses_mask") is True, "01_mm3 plan lost VLA masking")

    outer = [
        item
        for item in plans
        if item.get("function") == "mm" and item.get("region") == "while.cond.7"
    ]
    require(len(outer) == 1 and outer[0].get("code") == "REJECT_ALIAS",
            "01_mm3 unresolved outer unknown alias must remain REJECT_ALIAS")


def verify_oir(text: str) -> None:
    start = text.find("define void @mm(")
    end = text.find("\n}\n", start)
    require(start >= 0 and end > start, "missing mm OIR")
    mm = text[start : end + 3]
    require(mm.count("call i32 @__yoolang_ranges_disjoint") == 2,
            "mm must AND exactly two complete unknown-pair checks")
    require("lv.alias.all.disjoint = and i32" in mm, "pair checks are not conjoined")
    require("lv.alias.fast.guard = icmp ne i32" in mm, "fast guard is missing")
    require("lv.alias.fast" in mm and "lv.alias.slow" in mm, "dedicated version preheaders missing")
    require("lv.slow.while.cond.16" in mm and "lv.slow.while.body.17" in mm,
            "scalar fallback clone is missing")
    require("setvl <vscale x " in mm and "vp.gather" in mm and "vp.store" in mm,
            "fast path lacks real VLA invariant gather/store recipes")
    require("declare i32 @__yoolang_ranges_disjoint" in text,
            "pure runtime helper declaration missing")


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        gcc = tool("riscv64-linux-gnu-gcc")
        objcopy = tool("riscv64-linux-gnu-objcopy")
        objdump = tool("riscv64-linux-gnu-objdump")
        ar = tool("riscv64-linux-gnu-ar")
        nm = tool("riscv64-linux-gnu-nm")
        qemu = tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"required alias-versioning corpus tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77

    try:
        for path in (COMPILER, SOURCE, INPUT, EXPECTED, RUNTIME):
            require(path.is_file(), f"required artifact not found: {path}")
        archive_members = run([ar, "t", str(RUNTIME)]).stdout
        symbols = run([nm, "-g", "--defined-only", str(RUNTIME)]).stdout
        require("alias_runtime.c.o" in archive_members, "runtime archive lacks alias helper member")
        require("__yoolang_ranges_disjoint" in symbols, "runtime archive lacks helper symbol")
        print("PASS runtime_archive_contains_overflow_safe_alias_helper")

        plan = run([str(COMPILER), str(SOURCE), "--emit-vector-plan", "-O2", "-march=rv64gcv"])
        verify_plan(plan.stdout)
        print("PASS 01_mm3_verified_versioned_plan")

        oir = run([str(COMPILER), str(SOURCE), "--emit-oir", "-O2", "-march=rv64gcv"])
        verify_oir(oir.stdout)
        print("PASS 01_mm3_versioned_fast_slow_oir")

        with tempfile.TemporaryDirectory(prefix="rvv-alias-mm3-") as tmp:
            work = Path(tmp)
            rvv_asm = work / "mm3.rvv.s"
            scalar_asm = work / "mm3.scalar.s"
            rvv_obj = work / "mm3.rvv.o"
            scalar_obj = work / "mm3.scalar.o"
            rvv_exe = work / "mm3.rvv.exe"
            scalar_exe = work / "mm3.scalar.exe"
            driver_c = work / "driver.c"
            driver_obj = work / "driver.o"
            alias_exe = work / "mm3.alias.exe"

            run([str(COMPILER), str(SOURCE), "-S", "-O2", "-march=rv64gcv", "-o", str(rvv_asm)])
            run([str(COMPILER), str(SOURCE), "-S", "-O2", "-march=rv64gc", "-o", str(scalar_asm)])
            run([gcc, "-c", "-march=rv64gcv", "-mabi=lp64d", str(rvv_asm), "-o", str(rvv_obj)])
            run([gcc, "-c", "-march=rv64gc", "-mabi=lp64d", str(scalar_asm), "-o", str(scalar_obj)])
            disassembly = run([objdump, "-d", str(rvv_obj)]).stdout
            # The invariant A operand is an OIR gather with a zero index
            # stride, which the backend canonically selects as vlse32.v with
            # an x0 byte stride instead of materializing indexed offsets.
            required_ops = {
                "vsetvli", "vle32.v", "vlse32.v",
                "vmul.vv", "vadd.vv", "vse32.v",
            }
            missing = required_ops - decoded_vector_mnemonics(disassembly)
            require(not missing, "01_mm3 object lacks decoded RVV ops: " + ", ".join(sorted(missing)))
            require("__yoolang_ranges_disjoint" in rvv_asm.read_text(encoding="utf-8"),
                    "RVV assembly has no helper call")
            require("__yoolang_ranges_disjoint" not in scalar_asm.read_text(encoding="utf-8"),
                    "rv64gc scalar path calls alias helper")
            require(not decoded_vector_mnemonics(run([objdump, "-d", str(scalar_obj)]).stdout),
                    "rv64gc object unexpectedly contains decoded RVV")
            print("PASS 01_mm3_decoded_rvv_helper_call_and_scalar_baseline")

            run([gcc, "-static", "-march=rv64gcv", "-mabi=lp64d", str(rvv_asm), str(RUNTIME), "-lm", "-o", str(rvv_exe)])
            run([gcc, "-static", "-march=rv64gc", "-mabi=lp64d", str(scalar_asm), str(RUNTIME), "-lm", "-o", str(scalar_exe)])
            input_text = INPUT.read_text(encoding="utf-8")
            oracle = EXPECTED.read_text(encoding="utf-8").splitlines()[0]
            scalar = run([qemu, "-cpu", "rv64,v=false", str(scalar_exe)], stdin=input_text)
            require(scalar.stdout.splitlines() == [oracle], "01_mm3 scalar output differs from oracle")
            for vlen in VLENS:
                vector = run([qemu, "-cpu", f"rv64,v=true,vlen={vlen},elen=64", str(rvv_exe)], stdin=input_text)
                require(vector.stdout == scalar.stdout, f"01_mm3 corpus output differs at VLEN={vlen}")
            print("PASS 01_mm3_scalar_and_vlen_128_256_512_1024")

            # Reuse the actual corpus mm symbol, renaming only its original main.
            run([objcopy, "--redefine-sym", "main=corpus_original_main", str(rvv_obj)])
            driver_c.write_text(textwrap.dedent(DRIVER), encoding="utf-8")
            run([gcc, "-c", "-O2", "-fno-tree-vectorize", "-march=rv64gc", "-mabi=lp64d", str(driver_c), "-o", str(driver_obj)])
            run([gcc, "-static", "-march=rv64gcv", "-mabi=lp64d", str(rvv_obj), str(driver_obj), str(RUNTIME), "-lm", "-o", str(alias_exe)])
            for vlen in VLENS:
                result = run([qemu, "-cpu", f"rv64,v=true,vlen={vlen},elen=64", str(alias_exe)])
                require(result.stdout.startswith("PASS actual_01_mm3_disjoint_exact_partial"),
                        f"actual overlap/disjoint driver failed at VLEN={vlen}")
            print("PASS 01_mm3_actual_disjoint_overlap_exact_version_paths")
    except (Failure, json.JSONDecodeError, OSError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
