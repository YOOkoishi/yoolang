#!/usr/bin/env python3

"""End-to-end gates for the final three expected-vectorizable RVV kernels."""

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
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from perf_common import normalize_output, read_sysy_expected_output  # noqa: E402


COMPILER = Path(
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
RUNTIME = ROOT / "runtime/libsysy_riscv.a"
VLENS = (128, 256, 512, 1024)
VERSIONED_REMARK = (
    "transformed verified scalable VLA fast path with overflow-safe runtime alias "
    "versioning for 1 complete byte-range pair(s); preserved scalar slow path"
)
CASES = {
    "many_mat_cal-3": {
        "function": "main",
        "region": "for.cond.72",
        "minimum_loop_successes": 1,
        "mnemonics": {"vsetvli", "vle32.v", "vmul.vv", "vredsum.vs"},
    },
    "matmul2": {
        "function": "main",
        "region": "for.cond.69",
        "minimum_loop_successes": 1,
        "mnemonics": {"vsetvli", "vlse32.v", "vredsum.vs"},
    },
    "fft2": {
        "function": "memmove1",
        "region": "while.body.2",
        "minimum_loop_successes": 6,
        "mnemonics": {"vsetvli", "vle32.v", "vse32.v"},
    },
}


MEMMOVE_DRIVER = r"""
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { MAX_COUNT = 129, CELLS = MAX_COUNT + 8 };

extern int memmove1(int *dst, int dst_pos, int *src, int len);

static int forward_copy(int *dst, const int *src, int count) {
    int index = 0;
    while (index < count) {
        dst[index] = src[index];
        ++index;
    }
    return index;
}

static void initialize(int *buffer, int seed) {
    for (int index = 0; index < CELLS; ++index)
        buffer[index] = seed * 1009 + index * 37;
}

static int run_alias_case(int scenario, int count, uint64_t *checksum) {
    int actual_a[CELLS], actual_b[CELLS];
    int expected_a[CELLS], expected_b[CELLS];
    initialize(actual_a, 3);
    initialize(actual_b, 7);
    memcpy(expected_a, actual_a, sizeof(actual_a));
    memcpy(expected_b, actual_b, sizeof(actual_b));

    int *actual_dst = actual_b;
    int *actual_src = actual_a;
    int *expected_dst = expected_b;
    int *expected_src = expected_a;
    if (scenario == 1) {
        actual_dst = actual_src = actual_a;
        expected_dst = expected_src = expected_a;
    } else if (scenario == 2) {
        actual_dst = actual_a + 1;
        actual_src = actual_a;
        expected_dst = expected_a + 1;
        expected_src = expected_a;
    } else if (scenario == 3) {
        actual_dst = actual_a;
        actual_src = actual_a + 1;
        expected_dst = expected_a;
        expected_src = expected_a + 1;
    }

    int expected_return = forward_copy(expected_dst, expected_src, count);
    int actual_return = memmove1(actual_dst, 0, actual_src, count);
    if (actual_return != expected_return ||
        memcmp(actual_a, expected_a, sizeof(actual_a)) != 0 ||
        memcmp(actual_b, expected_b, sizeof(actual_b)) != 0)
        return 10 + scenario;
    for (int index = 0; index < CELLS; index += 17)
        *checksum = *checksum * UINT64_C(1315423911) +
                    (uint32_t)actual_a[index] + (uint32_t)actual_b[index];
    return 0;
}

static int run_guard_case(int count, uint64_t *checksum) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || (size_t)count * sizeof(int) > (size_t)page_size)
        return 70;
    unsigned char *mapping = mmap(NULL, (size_t)page_size * 4,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return 71;
    if (mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) != 0 ||
        mprotect(mapping + page_size * 3, (size_t)page_size, PROT_NONE) != 0)
        return 72;

    int *src = (int *)(mapping + page_size - (size_t)count * sizeof(int));
    int *dst = (int *)(mapping + page_size * 3 - (size_t)count * sizeof(int));
    for (int index = 0; index < count; ++index) {
        src[index] = 17 + index * 11;
        dst[index] = -1;
    }
    int actual_return = memmove1(dst, 0, src, count);
    if (actual_return != count) {
        munmap(mapping, (size_t)page_size * 4);
        return 73;
    }
    for (int index = 0; index < count; ++index) {
        if (dst[index] != 17 + index * 11) {
            munmap(mapping, (size_t)page_size * 4);
            return 74;
        }
        *checksum = *checksum * UINT64_C(2654435761) + (uint32_t)dst[index];
    }
    if (munmap(mapping, (size_t)page_size * 4) != 0)
        return 75;
    return 0;
}

int main(void) {
    static const int counts[] = {
        0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129
    };
    uint64_t checksum = 0;
    for (int scenario = 0; scenario < 4; ++scenario) {
        for (unsigned index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
            int status = run_alias_case(scenario, counts[index], &checksum);
            if (status != 0) {
                fprintf(stderr, "alias scenario=%d count=%d status=%d\n",
                        scenario, counts[index], status);
                return status;
            }
        }
    }
    for (unsigned index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
        int status = run_guard_case(counts[index], &checksum);
        if (status != 0) {
            fprintf(stderr, "guard count=%d status=%d\n", counts[index], status);
            return status;
        }
    }
    printf("PASS actual_fft2_memmove_paths checksum=%llu\n",
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
    path = shutil.which(name)
    if path is None:
        raise FileNotFoundError(name)
    return path


def run(
    command: list[str], *, stdin: str | None = None, allow_nonzero: bool = False
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0 and not allow_nonzero:
        raise Failure(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def function_text(oir: str, signature: str) -> str:
    start = oir.find(signature)
    require(start >= 0, f"missing OIR function {signature}")
    end = oir.find("\n}\n", start)
    require(end > start, f"unterminated OIR function {signature}")
    return oir[start : end + 3]


def decoded_vector_mnemonics(disassembly: str) -> set[str]:
    return set(
        re.findall(
            r"(?m)^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(v[a-z0-9_.]+)\b",
            disassembly,
        )
    )


def verify_plan(name: str, output: str) -> int:
    plans = json.loads(output).get("vectorization_plans")
    require(isinstance(plans, list), f"{name} plan JSON has no plan list")
    loop_successes = [
        item
        for item in plans
        if item.get("vectorizer") == "loop" and item.get("code") == "VECTORIZED"
    ]
    spec = CASES[name]
    require(
        len(loop_successes) >= spec["minimum_loop_successes"],
        f"{name} has {len(loop_successes)} verified loop plans, expected at least "
        f"{spec['minimum_loop_successes']}",
    )
    selected = [
        item
        for item in loop_successes
        if item.get("function") == spec["function"]
        and item.get("region") == spec["region"]
    ]
    require(len(selected) == 1, f"{name} selected loop did not vectorize exactly once")
    success = selected[0]
    expected_remark = VERSIONED_REMARK if name == "fft2" else "transformed verified scalable VLA loop"
    require(success.get("explanation") == expected_remark, f"{name} success remark changed")
    choice = success.get("plan")
    require(isinstance(choice, dict), f"{name} selected loop has no plan")
    require(choice.get("scalable") is True, f"{name} selected plan is not scalable")
    require(choice.get("uses_mask") is True, f"{name} selected plan is not VLA-masked")
    require(
        isinstance(choice.get("estimated_scalar_cost"), int)
        and isinstance(choice.get("estimated_vector_cost"), int)
        and choice["estimated_vector_cost"] < choice["estimated_scalar_cost"],
        f"{name} selected plan is not profitable",
    )
    require(
        choice.get("runtime_alias_check") is (name == "fft2"),
        f"{name} runtime alias-plan bit changed",
    )
    if name == "fft2":
        require(
            all(item.get("explanation") == VERSIONED_REMARK for item in loop_successes),
            "fft2 has an unverified or non-versioned loop success",
        )
    return len(loop_successes)


def verify_many_oir(oir: str) -> None:
    main = function_text(oir, "define i32 @main() {")
    require(
        "br i1 %i.for.cond.6, %for.cond.72, %for.end.71" in main,
        "many_mat_cal-3 lost the conditional outer-loop preheader guard",
    )
    start = main.find("for.cond.72:")
    end = main.find("licm.preheader.", start)
    require(start >= 0 and end > start, "many_mat_cal-3 vector region is missing")
    region = main[start:end]
    for token in (
        ".lv.remaining = phi",
        "setvl <vscale x 16 x i32>",
        "vp.load <vscale x 16 x i32>",
        "vp.mul <vscale x 16 x i32>",
        "vp.reduce.add <vscale x 16 x i32>",
        ".lv.remaining.next = sub i32",
    ):
        require(token in region, f"many_mat_cal-3 OIR lacks {token}")


def verify_matmul_oir(oir: str) -> None:
    main = function_text(oir, "define i32 @main() {")
    start = main.find("for.cond.65:")
    end = main.find("licm.preheader.", start)
    require(start >= 0 and end > start, "matmul2 vector region is missing")
    region = main[start:end]
    require(
        "lv.trip.nonnegative, 4" in region
        and "lv.trip.whole = sdiv i32" in region
        and "lv.trip.round_up = zext i1" in region
        and "lv.trip.iterations = add i32" in region,
        "matmul2 stride-4 scalar-iteration normalization is incomplete",
    )
    require(region.count(" = vp.gather <vscale x 8 x i32>") == 4,
            "matmul2 must contain four stride-4 VP gathers")
    require(region.count(" = vp.reduce.add <vscale x 8 x i32>") == 4,
            "matmul2 must contain four chained VP reductions")
    require(
        re.search(r"vp\.1 = vp\.reduce\.add .*passthrough i32 %[^,]+\.vp\.0", region)
        is not None
        and re.search(r"vp\.2 = vp\.reduce\.add .*passthrough i32 %[^,]+\.vp\.1", region)
        is not None
        and re.search(r"vp\.3 = vp\.reduce\.add .*passthrough i32 %[^,]+\.vp\.2", region)
        is not None,
        "matmul2 VP reductions are not chained in scalar source order",
    )
    require(
        re.search(r"\.lv\.iv\.delta = mul i32 %[^,]+\.lv\.vl, 4", region)
        is not None
        and re.search(r"\.addr\.ptr\.delta = mul i32 %[^,]+\.lv\.vl, 4", region)
        is not None,
        "matmul2 IV/pointer step is not actual-VL times four",
    )


def verify_fft_oir(oir: str) -> None:
    memmove = function_text(
        oir,
        "define i32 @memmove1(i32* %dst.arg, i32 %dst_pos.arg, i32* %src.arg, i32 %len.arg) {",
    )
    require(
        re.search(
            r"entry\.0:.*?br i1 %[^,]+, %lv\.alias\.check\.\d+, %while\.end\.3",
            memmove,
            re.DOTALL,
        )
        is not None,
        "fft2 zero-trip path must bypass both the alias helper and vector body",
    )
    require(memmove.count("call i32 @__yoolang_ranges_disjoint") == 1,
            "fft2 memmove1 must check exactly one complete range pair")
    require("lv.alias.fast.guard = icmp ne i32" in memmove,
            "fft2 memmove1 has no version-selection guard")
    require("lv.alias.fast" in memmove and "lv.alias.slow" in memmove,
            "fft2 memmove1 lacks explicit fast/slow preheaders")
    require("lv.slow.while.body.2" in memmove and "load i32" in memmove and "store i32" in memmove,
            "fft2 memmove1 scalar slow clone is missing")
    require("vp.load <vscale x 16 x i32>" in memmove and "vp.store <vscale x 16 x i32>" in memmove,
            "fft2 memmove1 vector fast path is missing")
    require(
        re.search(
            r"rot\.exit = phi \[0, %entry\.0\], \[[^]]+, %while\.body\.2\], "
            r"\[[^]]+, %lv\.slow\.while\.body\.2\.\d+\] : i32",
            memmove,
        )
        is not None,
        "fft2 memmove1 exit phi does not merge zero/fast/slow live-outs",
    )


def compiler_command(source: Path, mode: str, march: str, output: Path | None = None) -> list[str]:
    command = [
        str(COMPILER),
        str(source),
        mode,
        "-O3",
        f"-march={march}",
        "-mabi=lp64d",
    ]
    if march == "rv64gcv":
        command.extend(
            ["-fvectorize", "-fslp-vectorize", "-mrvv-vector-bits=scalable"]
        )
    else:
        command.extend(["-fno-vectorize", "-fno-slp-vectorize"])
    if output is not None:
        command.extend(["-o", str(output)])
    return command


def main() -> int:
    required = os.environ.get("YOOLANG_INFRA_TOOLCHAIN_REQUIRED") == "1"
    try:
        gcc = tool("riscv64-linux-gnu-gcc")
        objcopy = tool("riscv64-linux-gnu-objcopy")
        objdump = tool("riscv64-linux-gnu-objdump")
        qemu = tool("qemu-riscv64")
    except FileNotFoundError as error:
        message = f"required remaining-corpus tool is unavailable: {error.args[0]}"
        if required:
            print(f"FAIL {message}", file=sys.stderr)
            return 1
        print(f"SKIP {message}")
        return 77

    try:
        require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
        require(RUNTIME.is_file(), f"runtime archive not found: {RUNTIME}")
        sources = {name: ROOT / f"test/performance/{name}.sy" for name in CASES}
        for name, source in sources.items():
            for artifact in (source, source.with_suffix(".in"), source.with_suffix(".out")):
                require(artifact.is_file(), f"{name} corpus artifact is missing: {artifact}")

        loop_counts: dict[str, int] = {}
        oir_texts: dict[str, str] = {}
        for name, source in sources.items():
            plan = run(compiler_command(source, "--emit-vector-plan", "rv64gcv"))
            loop_counts[name] = verify_plan(name, plan.stdout)
            oir = run(compiler_command(source, "--emit-oir", "rv64gcv"))
            oir_texts[name] = oir.stdout
        verify_many_oir(oir_texts["many_mat_cal-3"])
        verify_matmul_oir(oir_texts["matmul2"])
        verify_fft_oir(oir_texts["fft2"])
        print(
            "PASS remaining_corpus_verified_plan_and_oir "
            + " ".join(f"{name}={loop_counts[name]}" for name in CASES)
        )

        with tempfile.TemporaryDirectory(prefix="rvv-remaining-corpus-") as temporary:
            work = Path(temporary)
            rvv_objects: dict[str, Path] = {}
            executables: dict[tuple[str, str], Path] = {}
            for name, source in sources.items():
                for variant, march in (("rvv", "rv64gcv"), ("scalar", "rv64gc")):
                    assembly = work / f"{name}.{variant}.s"
                    object_file = work / f"{name}.{variant}.o"
                    executable = work / f"{name}.{variant}.exe"
                    run(compiler_command(source, "-S", march, assembly))
                    run(
                        [
                            gcc,
                            "-c",
                            f"-march={march}",
                            "-mabi=lp64d",
                            str(assembly),
                            "-o",
                            str(object_file),
                        ]
                    )
                    disassembly = run([objdump, "-d", str(object_file)]).stdout
                    mnemonics = decoded_vector_mnemonics(disassembly)
                    if variant == "rvv":
                        missing = CASES[name]["mnemonics"] - mnemonics
                        require(
                            not missing,
                            f"{name} object lacks decoded RVV ops: "
                            + ", ".join(sorted(missing)),
                        )
                        rvv_objects[name] = object_file
                        if name == "fft2":
                            require(
                                "__yoolang_ranges_disjoint" in assembly.read_text(),
                                "fft2 RVV assembly has no alias-helper call",
                            )
                    else:
                        require(not mnemonics, f"{name} rv64gc object contains RVV opcodes")
                        require(
                            "__yoolang_ranges_disjoint" not in assembly.read_text(),
                            f"{name} rv64gc assembly calls the alias helper",
                        )
                    run(
                        [
                            gcc,
                            "-static",
                            f"-march={march}",
                            "-mabi=lp64d",
                            str(assembly),
                            str(RUNTIME),
                            "-lm",
                            "-o",
                            str(executable),
                        ]
                    )
                    executables[(name, variant)] = executable
            print("PASS remaining_corpus_decoded_rvv_and_scalar_objects")

            for name, source in sources.items():
                expected = read_sysy_expected_output(source)
                require(expected is not None, f"{name} has no SysY output oracle")
                expected_stdout, expected_exit = expected
                input_text = source.with_suffix(".in").read_text(encoding="utf-8")
                scalar = run(
                    [qemu, "-cpu", "rv64,v=false", str(executables[(name, "scalar")])],
                    stdin=input_text,
                    allow_nonzero=True,
                )
                require(scalar.returncode == expected_exit, f"{name} scalar exit differs")
                require(normalize_output(scalar.stdout) == expected_stdout,
                        f"{name} scalar stdout differs from oracle")
                for vlen in VLENS:
                    vector = run(
                        [
                            qemu,
                            "-cpu",
                            f"rv64,v=true,vlen={vlen},elen=64",
                            str(executables[(name, "rvv")]),
                        ],
                        stdin=input_text,
                        allow_nonzero=True,
                    )
                    require(vector.returncode == scalar.returncode,
                            f"{name} exit differs at VLEN={vlen}")
                    require(vector.stdout == scalar.stdout,
                            f"{name} stdout differs at VLEN={vlen}")
            print("PASS remaining_corpus_scalar_and_vlen_128_256_512_1024")

            fft_object = rvv_objects["fft2"]
            run([objcopy, "--redefine-sym", "main=fft_corpus_main", str(fft_object)])
            driver_c = work / "memmove_driver.c"
            driver_o = work / "memmove_driver.o"
            driver_exe = work / "memmove_driver.exe"
            driver_c.write_text(textwrap.dedent(MEMMOVE_DRIVER), encoding="utf-8")
            run(
                [
                    gcc,
                    "-c",
                    "-O2",
                    "-fno-tree-vectorize",
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    str(driver_c),
                    "-o",
                    str(driver_o),
                ]
            )
            run(
                [
                    gcc,
                    "-static",
                    "-march=rv64gcv",
                    "-mabi=lp64d",
                    str(fft_object),
                    str(driver_o),
                    str(RUNTIME),
                    "-lm",
                    "-o",
                    str(driver_exe),
                ]
            )
            for vlen in VLENS:
                result = run(
                    [
                        qemu,
                        "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64",
                        str(driver_exe),
                    ]
                )
                require(
                    result.stdout.startswith("PASS actual_fft2_memmove_paths"),
                    f"fft2 overlap/guard driver failed at VLEN={vlen}",
                )
            print("PASS fft2_actual_disjoint_overlap_exact_zero_and_guard_pages")
    except (Failure, json.JSONDecodeError, OSError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
