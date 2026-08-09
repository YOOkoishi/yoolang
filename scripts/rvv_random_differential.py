#!/usr/bin/env python3

"""Reproducible RVV differential-test case generation and execution.

The required smoke tier executes unit-stride and deliberately unaligned i32,
IEEE-f32 bit-pattern, masked guard-page, signed/positive constant-stride, and
integer-reduction cases against the Yoolang scalar build and lane-wise oracle
at VLEN 128/256/512/1024.  Unsupported legality cases are compiled and checked
against stable rejection codes.  Case IDs, replay inputs, minimized failure
artifacts, and deterministic extended/nightly tiers make every mismatch
reproducible; adding a dimension can never silently turn an unexecuted case
into a passing result.

GCC and Clang currently build the C harness/oracle, not the same generated
kernel body.  Cross-compiler random-kernel comparison therefore remains a
separate validation gap rather than an implied part of this runner.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Callable, Iterable, Sequence

import rvv_mask_guard_tests as mask_guard


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPILER = Path(
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
DEFAULT_ARTIFACT_DIR = ROOT / "build/test-artifacts/rvv-random-differential"
DEFAULT_SEED = 0x5256564449464632
SCHEMA_VERSION = 1
VLENS = (128, 256, 512, 1024)
# Include every byte residue below an e32 element.  Offsets 1/2/3 are
# intentionally truly misaligned; they are not aliases for 4-byte-aligned
# addresses with a different cache-line position.
ALIGNMENT_OFFSETS = (0, 1, 2, 3, 4, 8, 12, 20, 28, 60)
CONSTANT_STRIDES = (1, 2, 4, -1, -2, -4)
MASK_PATTERNS = ("alltrue", "allfalse", "sparse")
ALIAS_PATTERNS = (
    "inplace",
    "disjoint",
    "exact",
    "forward-overlap",
    "backward-overlap",
    "may-alias",
)

EXPECT_RUN = "run"
EXPECT_PLAN = "plan"
EXPECT_REJECT = "reject"
EXPECT_BLOCKED = "blocked"
EXPECTATIONS = frozenset({EXPECT_RUN, EXPECT_PLAN, EXPECT_REJECT, EXPECT_BLOCKED})
TIERS = ("smoke", "extended", "nightly")


ACTIVE_SOURCE = r"""
void random_add_bias(int values[], int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + 1;
  }
}
"""


ACTIVE_DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void random_add_bias(void *values, int32_t n);

enum { MAX_N = 4095, STORAGE_BYTES = MAX_N * 4 + 128 };
static uint8_t storage[STORAGE_BYTES] __attribute__((aligned(64)));

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint32_t load_bits(const void *value) {
    uint32_t bits = 0;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void store_bits(void *value, uint32_t bits) {
    memcpy(value, &bits, sizeof(bits));
}

static uint64_t hash_word(uint64_t hash, uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        hash ^= (word >> (byte * 8U)) & UINT32_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int main(void) {
    int32_t n = 0;
    unsigned offset = 0;
    unsigned long long input_seed = 0;
    if (scanf("%d %u %llu", &n, &offset, &input_seed) != 3) return 10;
    if (n < 0 || n > MAX_N || offset >= 64) return 11;
    const size_t payload_end = (size_t)offset + (size_t)n * sizeof(int32_t);
    if (payload_end > sizeof(storage)) return 12;

    memset(storage, 0xa5, sizeof(storage));
    static const uint32_t boundaries[] = {
        UINT32_C(0x80000000), UINT32_C(0x7fffffff), UINT32_C(0xffffffff),
        UINT32_C(0x00000000), UINT32_C(0x00000001), UINT32_C(0x7ffffff8),
        UINT32_C(0xfffffff9), UINT32_C(0x55555555), UINT32_C(0xaaaaaaaa),
    };
    uint64_t state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    for (int32_t lane = 0; lane < n; ++lane) {
        uint32_t bits = lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                            ? boundaries[lane]
                            : (uint32_t)next_random(&state);
        store_bits(storage + offset + (size_t)lane * sizeof(uint32_t), bits);
    }

    random_add_bias(storage + offset, n);

    state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int32_t lane = 0; lane < n; ++lane) {
        uint32_t before = lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                              ? boundaries[lane]
                              : (uint32_t)next_random(&state);
        const uint32_t expected = before + UINT32_C(7);
        const uint32_t actual =
            load_bits(storage + offset + (size_t)lane * sizeof(uint32_t));
        if (actual != expected) {
            fprintf(stderr,
                    "lane mismatch lane=%d before=%08x expected=%08x actual=%08x\n",
                    lane, before, expected, actual);
            return 20;
        }
        hash = hash_word(hash, actual);
    }
    for (size_t byte = 0; byte < offset; ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 30;
    for (size_t byte = payload_end; byte < sizeof(storage); ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 31;

    printf("OK %d %u %016llx\n", n, offset, (unsigned long long)hash);
    return 0;
}
"""


FLOAT_SOURCE = r"""
void random_float_bias(float values[], int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 1.25;
    i = i + 1;
  }
}
"""


FLOAT_DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void random_float_bias(void *values, int32_t n);

enum { MAX_N = 4095, STORAGE_BYTES = MAX_N * 4 + 128 };
static uint8_t storage[STORAGE_BYTES] __attribute__((aligned(64)));

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint32_t load_bits(const void *value) {
    uint32_t bits = 0;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void store_bits(void *value, uint32_t bits) {
    memcpy(value, &bits, sizeof(bits));
}

static uint32_t scalar_expected(uint32_t bits) {
    float input = 0.0f;
    memcpy(&input, &bits, sizeof(input));
    volatile float lhs = input;
    volatile float rhs = 1.25f;
    float result = lhs + rhs;
    uint32_t output = 0;
    memcpy(&output, &result, sizeof(output));
    return output;
}

static uint64_t hash_word(uint64_t hash, uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        hash ^= (word >> (byte * 8U)) & UINT32_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int main(void) {
    int32_t n = 0;
    unsigned offset = 0;
    unsigned long long input_seed = 0;
    if (scanf("%d %u %llu", &n, &offset, &input_seed) != 3) return 10;
    if (n < 0 || n > MAX_N || offset >= 64) return 11;
    const size_t payload_end = (size_t)offset + (size_t)n * sizeof(float);
    if (payload_end > sizeof(storage)) return 12;

    memset(storage, 0xa5, sizeof(storage));
    static const uint32_t boundaries[] = {
        UINT32_C(0x00000000), UINT32_C(0x80000000),
        UINT32_C(0x7f800000), UINT32_C(0xff800000),
        UINT32_C(0x7fc12345), UINT32_C(0x7f812345),
        UINT32_C(0x00000001), UINT32_C(0x80000001),
        UINT32_C(0x007fffff), UINT32_C(0x00800000),
        UINT32_C(0x3f800000), UINT32_C(0xbf800000),
        UINT32_C(0x7f7fffff), UINT32_C(0xff7fffff),
    };
    uint64_t state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t bits =
            lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                ? boundaries[lane]
                : (uint32_t)next_random(&state);
        store_bits(storage + offset + (size_t)lane * sizeof(uint32_t), bits);
    }

    random_float_bias(storage + offset, n);

    state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t before =
            lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                ? boundaries[lane]
                : (uint32_t)next_random(&state);
        const uint32_t expected = scalar_expected(before);
        const uint32_t actual =
            load_bits(storage + offset + (size_t)lane * sizeof(uint32_t));
        if (actual != expected) {
            fprintf(stderr,
                    "float lane mismatch lane=%d before=%08x expected=%08x "
                    "actual=%08x\n",
                    lane, before, expected, actual);
            return 20;
        }
        hash = hash_word(hash, actual);
    }
    for (size_t byte = 0; byte < offset; ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 30;
    for (size_t byte = payload_end; byte < sizeof(storage); ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 31;

    printf("OK %d %u %016llx\n", n, offset, (unsigned long long)hash);
    return 0;
}
"""


INDEXED_SOURCE = r"""
void random_stride_p2(int values[], int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + 2;
  }
}

void random_stride_p4(int values[], int n) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + 4;
  }
}

void random_stride_n1(int values[], int n) {
  int i = n - 1;
  while (i >= 0) {
    values[i] = values[i] + 7;
    i = i - 1;
  }
}

void random_stride_n2(int values[], int n) {
  int i = n - 1;
  while (i >= 0) {
    values[i] = values[i] + 7;
    i = i - 2;
  }
}

void random_stride_n4(int values[], int n) {
  int i = n - 1;
  while (i >= 0) {
    values[i] = values[i] + 7;
    i = i - 4;
  }
}
"""


INDEXED_DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void random_stride_p2(void *, int32_t);
void random_stride_p4(void *, int32_t);
void random_stride_n1(void *, int32_t);
void random_stride_n2(void *, int32_t);
void random_stride_n4(void *, int32_t);

enum { MAX_N = 4095, STORAGE_BYTES = MAX_N * 4 + 128 };
static uint8_t storage[STORAGE_BYTES] __attribute__((aligned(64)));

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint32_t load_bits(const void *value) {
    uint32_t bits = 0;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void store_bits(void *value, uint32_t bits) {
    memcpy(value, &bits, sizeof(bits));
}

static uint64_t hash_word(uint64_t hash, uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        hash ^= (word >> (byte * 8U)) & UINT32_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int is_touched(int32_t lane, int32_t n, int32_t stride) {
    if (stride > 0) return lane % stride == 0;
    const int32_t magnitude = -stride;
    return (n - 1 - lane) % magnitude == 0;
}

int main(void) {
    int32_t n = 0;
    int32_t stride = 0;
    unsigned offset = 0;
    unsigned long long input_seed = 0;
    if (scanf("%d %u %llu %d", &n, &offset, &input_seed, &stride) != 4)
        return 10;
    if (n < 0 || n > MAX_N || offset >= 64)
        return 11;
    if (stride != 2 && stride != 4 && stride != -1 && stride != -2 &&
        stride != -4)
        return 12;
    const size_t payload_end = (size_t)offset + (size_t)n * sizeof(int32_t);
    if (payload_end > sizeof(storage)) return 13;

    memset(storage, 0xa5, sizeof(storage));
    static const uint32_t boundaries[] = {
        UINT32_C(0x80000000), UINT32_C(0x7fffffff), UINT32_C(0xffffffff),
        UINT32_C(0x00000000), UINT32_C(0x00000001), UINT32_C(0x7ffffff8),
        UINT32_C(0xfffffff9), UINT32_C(0x55555555), UINT32_C(0xaaaaaaaa),
    };
    uint64_t state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t bits =
            lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                ? boundaries[lane]
                : (uint32_t)next_random(&state);
        store_bits(storage + offset + (size_t)lane * sizeof(uint32_t), bits);
    }

    if (stride == 2) random_stride_p2(storage + offset, n);
    else if (stride == 4) random_stride_p4(storage + offset, n);
    else if (stride == -1) random_stride_n1(storage + offset, n);
    else if (stride == -2) random_stride_n2(storage + offset, n);
    else random_stride_n4(storage + offset, n);

    state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t before =
            lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                ? boundaries[lane]
                : (uint32_t)next_random(&state);
        const uint32_t expected = before + (is_touched(lane, n, stride) ? 7U : 0U);
        const uint32_t actual =
            load_bits(storage + offset + (size_t)lane * sizeof(uint32_t));
        if (actual != expected) {
            fprintf(stderr,
                    "indexed lane mismatch lane=%d stride=%d before=%08x "
                    "expected=%08x actual=%08x\n",
                    lane, stride, before, expected, actual);
            return 20;
        }
        hash = hash_word(hash, actual);
    }
    for (size_t byte = 0; byte < offset; ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 30;
    for (size_t byte = payload_end; byte < sizeof(storage); ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 31;

    printf("OK %d %u %d %016llx\n", n, offset, stride,
           (unsigned long long)hash);
    return 0;
}
"""


REDUCTION_SOURCE = r"""
int random_reduce_add(int values[], int n, int seed) {
  int i = 0;
  int result = seed;
  while (i < n) {
    result = result + values[i];
    i = i + 1;
  }
  return result;
}

int random_reduce_mul(int values[], int n, int seed) {
  int i = 0;
  int result = seed;
  while (i < n) {
    result = result * values[i];
    i = i + 1;
  }
  return result;
}

int random_reduce_and(int values[], int n, int seed) {
  int i = 0;
  int result = seed;
  while (i < n) {
    result = result & values[i];
    i = i + 1;
  }
  return result;
}

int random_reduce_or(int values[], int n, int seed) {
  int i = 0;
  int result = seed;
  while (i < n) {
    result = result | values[i];
    i = i + 1;
  }
  return result;
}

int random_reduce_xor(int values[], int n, int seed) {
  int i = 0;
  int result = seed;
  while (i < n) {
    result = result ^ values[i];
    i = i + 1;
  }
  return result;
}
"""


REDUCTION_DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t random_reduce_add(void *, int32_t, int32_t);
int32_t random_reduce_mul(void *, int32_t, int32_t);
int32_t random_reduce_and(void *, int32_t, int32_t);
int32_t random_reduce_or(void *, int32_t, int32_t);
int32_t random_reduce_xor(void *, int32_t, int32_t);

enum { MAX_N = 4095, STORAGE_BYTES = MAX_N * 4 + 128 };
static uint8_t storage[STORAGE_BYTES] __attribute__((aligned(64)));

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint32_t load_bits(const void *value) {
    uint32_t bits = 0;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void store_bits(void *value, uint32_t bits) {
    memcpy(value, &bits, sizeof(bits));
}

static uint64_t hash_word(uint64_t hash, uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        hash ^= (word >> (byte * 8U)) & UINT32_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int main(void) {
    int32_t n = 0;
    unsigned offset = 0;
    unsigned long long input_seed = 0;
    if (scanf("%d %u %llu", &n, &offset, &input_seed) != 3) return 10;
    if (n < 0 || n > MAX_N || offset >= 64) return 11;
    const size_t payload_end = (size_t)offset + (size_t)n * sizeof(int32_t);
    if (payload_end > sizeof(storage)) return 12;

    memset(storage, 0xa5, sizeof(storage));
    static const uint32_t boundaries[] = {
        UINT32_C(0x80000001), UINT32_C(0x7fffffff), UINT32_C(0xffffffff),
        UINT32_C(0x00000001), UINT32_C(0x00000003), UINT32_C(0xfffffff9),
        UINT32_C(0x55555555), UINT32_C(0xaaaaaaab), UINT32_C(0x40000001),
    };
    uint64_t state = (uint64_t)input_seed;
    if (state == 0) state = UINT64_C(0x9e3779b97f4a7c15);
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t bits =
            lane < (int32_t)(sizeof(boundaries) / sizeof(boundaries[0]))
                ? boundaries[lane]
                : ((uint32_t)next_random(&state) | UINT32_C(1));
        store_bits(storage + offset + (size_t)lane * sizeof(uint32_t), bits);
    }

    const uint32_t add_seed = (uint32_t)input_seed;
    const uint32_t mul_seed = ((uint32_t)(input_seed >> 32) | UINT32_C(1));
    const uint32_t and_seed = UINT32_C(0xf0f0f0f0) ^ (uint32_t)input_seed;
    const uint32_t or_seed = UINT32_C(0x01020408) ^ (uint32_t)(input_seed >> 32);
    const uint32_t xor_seed = UINT32_C(0x12345678) ^ (uint32_t)input_seed;
    uint32_t expected_add = add_seed;
    uint32_t expected_mul = mul_seed;
    uint32_t expected_and = and_seed;
    uint32_t expected_or = or_seed;
    uint32_t expected_xor = xor_seed;
    for (int32_t lane = 0; lane < n; ++lane) {
        const uint32_t bits =
            load_bits(storage + offset + (size_t)lane * sizeof(uint32_t));
        expected_add += bits;
        expected_mul *= bits;
        expected_and &= bits;
        expected_or |= bits;
        expected_xor ^= bits;
    }

    const uint32_t actual_add = (uint32_t)random_reduce_add(
        storage + offset, n, (int32_t)add_seed);
    const uint32_t actual_mul = (uint32_t)random_reduce_mul(
        storage + offset, n, (int32_t)mul_seed);
    const uint32_t actual_and = (uint32_t)random_reduce_and(
        storage + offset, n, (int32_t)and_seed);
    const uint32_t actual_or = (uint32_t)random_reduce_or(
        storage + offset, n, (int32_t)or_seed);
    const uint32_t actual_xor = (uint32_t)random_reduce_xor(
        storage + offset, n, (int32_t)xor_seed);
    if (actual_add != expected_add || actual_mul != expected_mul ||
        actual_and != expected_and || actual_or != expected_or ||
        actual_xor != expected_xor) {
        fprintf(stderr,
                "reduction mismatch add=%08x/%08x mul=%08x/%08x "
                "and=%08x/%08x or=%08x/%08x xor=%08x/%08x\n",
                actual_add, expected_add, actual_mul, expected_mul,
                actual_and, expected_and, actual_or, expected_or,
                actual_xor, expected_xor);
        return 20;
    }
    for (size_t byte = 0; byte < offset; ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 30;
    for (size_t byte = payload_end; byte < sizeof(storage); ++byte)
        if (storage[byte] != UINT8_C(0xa5)) return 31;

    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, actual_add);
    hash = hash_word(hash, actual_mul);
    hash = hash_word(hash, actual_and);
    hash = hash_word(hash, actual_or);
    hash = hash_word(hash, actual_xor);
    printf("OK %d %u %016llx\n", n, offset, (unsigned long long)hash);
    return 0;
}
"""


ALIAS_REJECT_SOURCE = r"""
void alias_kernel(int source[], int destination[], int n) {
  int i = 0;
  while (i < n) {
    destination[i] = source[i] + 7;
    i = i + 1;
  }
}

int main() { return 0; }
"""


DYNAMIC_STRIDE_REJECT_SOURCE = r"""
void dynamic_stride_kernel(int values[], int n, int stride) {
  int i = 0;
  while (i < n) {
    values[i] = values[i] + 7;
    i = i + stride;
  }
}

int main() { return 0; }
"""


STRICT_FLOAT_REDUCTION_REJECT_SOURCE = r"""
float reduction_values[4096] = {};

float strict_float_reduction(int n) {
  int i = 0;
  float sum = 0.0;
  while (i < n) {
    sum = sum + reduction_values[i];
    i = i + 1;
  }
  return sum;
}

int main() { return 0; }
"""


@dataclasses.dataclass(frozen=True)
class ContractSource:
    source: str
    function: str


CONTRACT_SOURCES = {
    "alias_i32": ContractSource(ALIAS_REJECT_SOURCE, "alias_kernel"),
    "dynamic_stride_i32": ContractSource(
        DYNAMIC_STRIDE_REJECT_SOURCE, "dynamic_stride_kernel"
    ),
    "strict_float_reduction": ContractSource(
        STRICT_FLOAT_REDUCTION_REJECT_SOURCE, "strict_float_reduction"
    ),
}


def parse_seed(value: str | int) -> int:
    if isinstance(value, int):
        seed = value
    else:
        try:
            seed = int(value, 0)
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"invalid integer seed: {value}") from error
    if seed < 0 or seed > 0xFFFF_FFFF_FFFF_FFFF:
        raise argparse.ArgumentTypeError("seed must fit in an unsigned 64-bit integer")
    return seed


def derive_seed(master_seed: int, domain: str) -> int:
    seed = parse_seed(master_seed)
    digest = hashlib.blake2b(digest_size=8, person=b"yoolang-diff2")
    digest.update(seed.to_bytes(8, byteorder="little", signed=False))
    digest.update(domain.encode("utf-8"))
    return int.from_bytes(digest.digest(), byteorder="little", signed=False)


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


@dataclasses.dataclass(frozen=True)
class DifferentialCase:
    case_id: str
    case_seed: int
    kernel: str
    dtype: str
    length: int
    alignment_offset: int
    alias: str
    stride: int
    stride_mode: str
    mask: str
    opt_level: int
    expectation: str
    expected_code: str | None = None
    reason: str | None = None

    def identity_payload(self) -> dict[str, Any]:
        return {
            "case_seed": f"0x{self.case_seed:016x}",
            "kernel": self.kernel,
            "dtype": self.dtype,
            "length": self.length,
            "alignment_offset": self.alignment_offset,
            "alias": self.alias,
            "stride": self.stride,
            "stride_mode": self.stride_mode,
            "mask": self.mask,
            "opt_level": self.opt_level,
            "expectation": self.expectation,
            "expected_code": self.expected_code,
            "reason": self.reason,
        }

    def computed_case_id(self) -> str:
        digest = hashlib.sha256(canonical_json(self.identity_payload()).encode("utf-8"))
        return digest.hexdigest()[:20]

    def validate(self) -> None:
        if self.case_id != self.computed_case_id():
            raise ValueError(
                f"case id mismatch: stored={self.case_id}, computed={self.computed_case_id()}"
            )
        if self.dtype not in {"i32", "f32"}:
            raise ValueError(f"unsupported differential dtype: {self.dtype}")
        if self.length < 0 or self.length > 4095:
            raise ValueError(f"case length is outside the smoke harness: {self.length}")
        if self.alignment_offset not in ALIGNMENT_OFFSETS:
            raise ValueError(f"unsupported alignment offset: {self.alignment_offset}")
        if self.alias not in ALIAS_PATTERNS:
            raise ValueError(f"unknown alias pattern: {self.alias}")
        if self.stride_mode not in {"constant", "dynamic"}:
            raise ValueError(f"unknown stride mode: {self.stride_mode}")
        if self.mask not in MASK_PATTERNS:
            raise ValueError(f"unknown mask pattern: {self.mask}")
        if self.opt_level not in {0, 1, 2, 3}:
            raise ValueError(f"invalid optimization level: {self.opt_level}")
        if self.expectation not in EXPECTATIONS:
            raise ValueError(f"unknown expectation: {self.expectation}")
        if self.expectation == EXPECT_RUN:
            if self.expected_code is not None or self.reason is not None:
                raise ValueError("run cases cannot carry reject/blocked metadata")
        elif self.expectation in {EXPECT_PLAN, EXPECT_REJECT}:
            if not self.expected_code or self.kernel not in CONTRACT_SOURCES:
                raise ValueError("plan/reject cases require a stable code and contract source")
            if self.reason is not None:
                raise ValueError("plan/reject cases cannot carry a blocked reason")
        elif not self.reason:
            raise ValueError("blocked cases require a non-empty reason")

    def to_dict(self) -> dict[str, Any]:
        result = {"case_id": self.case_id, **self.identity_payload()}
        return result

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "DifferentialCase":
        data = dict(value)
        seed_value = data.get("case_seed")
        if seed_value is None:
            raise ValueError("replay case lacks case_seed")
        data["case_seed"] = parse_seed(seed_value)
        case = cls(**data)
        case.validate()
        return case


def make_case(master_seed: int, **values: Any) -> DifferentialCase:
    seed_key = canonical_json(values)
    case_seed = derive_seed(master_seed, seed_key)
    provisional = DifferentialCase(case_id="", case_seed=case_seed, **values)
    case = dataclasses.replace(provisional, case_id=provisional.computed_case_id())
    case.validate()
    return case


def mandatory_boundary_lengths() -> tuple[int, ...]:
    lengths = {0, 1, 127, 257, 1023, 4095}
    for vlen in VLENS:
        vlmax = vlen // 32
        lengths.update(
            {
                vlmax - 1,
                vlmax,
                vlmax + 1,
                2 * vlmax - 1,
                2 * vlmax,
                2 * vlmax + 1,
            }
        )
    return tuple(sorted(lengths))


def _random_lengths(master_seed: int, count: int) -> list[int]:
    result: list[int] = []
    used = set(mandatory_boundary_lengths())
    index = 0
    while len(result) < count:
        rng = random.Random(derive_seed(master_seed, f"random-length:{index}"))
        candidate = rng.randint(66, 2048)
        index += 1
        if candidate in used:
            continue
        used.add(candidate)
        result.append(candidate)
    return result


def generate_smoke_cases(master_seed: int = DEFAULT_SEED) -> list[DifferentialCase]:
    master_seed = parse_seed(master_seed)
    cases: list[DifferentialCase] = []

    active_lengths = list(mandatory_boundary_lengths()) + _random_lengths(master_seed, 4)
    for index, length in enumerate(active_lengths):
        cases.append(
            make_case(
                master_seed,
                kernel="unit_i32",
                dtype="i32",
                length=length,
                alignment_offset=ALIGNMENT_OFFSETS[index % len(ALIGNMENT_OFFSETS)],
                alias="inplace",
                stride=1,
                stride_mode="constant",
                mask="alltrue",
                opt_level=2,
                expectation=EXPECT_RUN,
                expected_code=None,
                reason=None,
            )
        )

    # Unknown source/destination relationships use overflow-safe runtime range
    # versioning.  These cases lock the compile-time success plan; dedicated
    # alias/guard runners execute disjoint, exact and overlapping routes.
    for index, alias in enumerate(ALIAS_PATTERNS[1:]):
        cases.append(
            make_case(
                master_seed,
                kernel="alias_i32",
                dtype="i32",
                length=33 + index,
                alignment_offset=ALIGNMENT_OFFSETS[index],
                alias=alias,
                stride=1,
                stride_mode="constant",
                mask="alltrue",
                opt_level=2,
                expectation=EXPECT_PLAN,
                expected_code="VECTORIZED",
                reason=None,
            )
        )

    # A runtime induction step is not a proven constant recurrence.  Check the
    # exact legality contract for every signed stride value in the GA matrix.
    for index, stride in enumerate(CONSTANT_STRIDES):
        cases.append(
            make_case(
                master_seed,
                kernel="dynamic_stride_i32",
                dtype="i32",
                length=65,
                alignment_offset=ALIGNMENT_OFFSETS[index],
                alias="inplace",
                stride=stride,
                stride_mode="dynamic",
                mask="alltrue",
                opt_level=2,
                expectation=EXPECT_REJECT,
                expected_code="REJECT_NON_CANONICAL_LOOP",
                reason=None,
            )
        )

    cases.append(
        make_case(
            master_seed,
            kernel="strict_float_reduction",
            dtype="f32",
            length=65,
            alignment_offset=0,
            alias="inplace",
            stride=1,
            stride_mode="constant",
            mask="alltrue",
            opt_level=2,
            expectation=EXPECT_REJECT,
            expected_code="REJECT_FP_ORDER",
            reason=None,
        )
    )

    # Required backend dimensions remain in every smoke manifest.  Each one is
    # promoted from an explicit blocker to a real scalar/RVV run only after its
    # assembly shape, oracle and multi-VLEN execution are wired below.
    for index, stride in enumerate((2, 4, -1, -2, -4)):
        cases.append(
            make_case(
                master_seed,
                kernel="indexed_i32",
                dtype="i32",
                length=65,
                alignment_offset=ALIGNMENT_OFFSETS[index],
                alias="inplace",
                stride=stride,
                stride_mode="constant",
                mask="alltrue",
                opt_level=2,
                expectation=EXPECT_RUN,
                expected_code=None,
                reason=None,
            )
        )
    for index, mask in enumerate(("allfalse", "sparse")):
        cases.append(
            make_case(
                master_seed,
                kernel="masked_i32",
                dtype="i32",
                length=33,
                # MASK2 uses page-relative word slots to place inactive lanes
                # against PROT_NONE.  Keep that independent guard fixture
                # naturally aligned; unit/indexed/float/reduction cases carry
                # the byte-offset 1/2/3 differential coverage.
                alignment_offset=(0, 4)[index],
                alias="inplace",
                stride=1,
                stride_mode="constant",
                mask=mask,
                opt_level=2,
                expectation=EXPECT_RUN,
                expected_code=None,
                reason=None,
            )
        )
    cases.append(
        make_case(
            master_seed,
            kernel="unit_f32",
            dtype="f32",
            length=65,
            alignment_offset=12,
            alias="inplace",
            stride=1,
            stride_mode="constant",
            mask="alltrue",
            opt_level=2,
            expectation=EXPECT_RUN,
            expected_code=None,
            reason=None,
        )
    )
    cases.append(
        make_case(
            master_seed,
            kernel="integer_reduction",
            dtype="i32",
            length=65,
            alignment_offset=20,
            alias="inplace",
            stride=1,
            stride_mode="constant",
            mask="alltrue",
            opt_level=2,
            expectation=EXPECT_RUN,
            expected_code=None,
            reason=None,
        )
    )

    result = sorted(cases, key=lambda case: case.case_id)
    if len({case.case_id for case in result}) != len(result):
        raise RuntimeError("generated duplicate random-differential case ids")
    return result


def _tier_random_lengths(master_seed: int, domain: str, count: int) -> list[int]:
    """Return stable, distinct large/small lengths for an extended tier."""

    result: list[int] = []
    used = set(mandatory_boundary_lengths())
    index = 0
    while len(result) < count:
        rng = random.Random(derive_seed(master_seed, f"{domain}:length:{index}"))
        candidate = rng.randint(2, 4095)
        index += 1
        if candidate in used:
            continue
        used.add(candidate)
        result.append(candidate)
    return result


def generate_tier_cases(
    master_seed: int = DEFAULT_SEED, tier: str = "smoke"
) -> list[DifferentialCase]:
    """Generate the deterministic smoke, extended, or nightly manifest.

    Extended tiers retain every smoke correctness and rejection contract, then
    add independent data seeds, lengths, byte alignments, strides, masks and
    reductions.  Compilation is still amortized per kernel by the executor.
    """

    if tier not in TIERS:
        raise ValueError(f"unknown random differential tier: {tier}")
    cases = list(generate_smoke_cases(master_seed))
    if tier == "smoke":
        return cases

    # Nightly is a strict manifest superset of extended and adds three more
    # batches, for four times the extended-only sample count in total.
    if tier == "nightly":
        cases = list(generate_tier_cases(master_seed, "extended"))
        scale = 3
    else:
        scale = 1

    def add_run(
        *,
        kernel: str,
        dtype: str,
        length: int,
        alignment_offset: int,
        stride: int = 1,
        mask: str = "alltrue",
    ) -> None:
        cases.append(
            make_case(
                master_seed,
                kernel=kernel,
                dtype=dtype,
                length=length,
                alignment_offset=alignment_offset,
                alias="inplace",
                stride=stride,
                stride_mode="constant",
                mask=mask,
                opt_level=2,
                expectation=EXPECT_RUN,
                expected_code=None,
                reason=None,
            )
        )

    unit_lengths = _tier_random_lengths(master_seed, f"{tier}:unit-i32", 32 * scale)
    for index, length in enumerate(unit_lengths):
        add_run(
            kernel="unit_i32",
            dtype="i32",
            length=length,
            alignment_offset=ALIGNMENT_OFFSETS[index % len(ALIGNMENT_OFFSETS)],
        )

    float_lengths = _tier_random_lengths(master_seed, f"{tier}:unit-f32", 12 * scale)
    for index, length in enumerate(float_lengths):
        add_run(
            kernel="unit_f32",
            dtype="f32",
            length=length,
            alignment_offset=ALIGNMENT_OFFSETS[(index + 1) % len(ALIGNMENT_OFFSETS)],
        )

    for stride_index, stride in enumerate((2, 4, -1, -2, -4)):
        lengths = _tier_random_lengths(
            master_seed, f"{tier}:indexed:{stride}", 6 * scale
        )
        for index, length in enumerate(lengths):
            add_run(
                kernel="indexed_i32",
                dtype="i32",
                length=length,
                alignment_offset=ALIGNMENT_OFFSETS[
                    (index + stride_index) % len(ALIGNMENT_OFFSETS)
                ],
                stride=stride,
            )

    reduction_lengths = _tier_random_lengths(
        master_seed, f"{tier}:integer-reduction", 10 * scale
    )
    for index, length in enumerate(reduction_lengths):
        add_run(
            kernel="integer_reduction",
            dtype="i32",
            length=length,
            alignment_offset=ALIGNMENT_OFFSETS[(index + 2) % len(ALIGNMENT_OFFSETS)],
        )

    # The guard-page fixture owns a word-addressed sparse layout, so its base
    # remains naturally aligned.  Vary its data seed independently through the
    # case identity while ordinary kernels cover byte offsets 1/2/3.
    mask_sample_base = 0 if tier == "extended" else 4
    for mask_index, mask in enumerate(("allfalse", "sparse")):
        for sample in range(mask_sample_base, mask_sample_base + 4 * scale):
            add_run(
                kernel="masked_i32",
                dtype="i32",
                length=1000 + mask_index * 100 + sample,
                alignment_offset=(0, 4, 8, 12)[sample % 4],
                mask=mask,
            )

    result = sorted(cases, key=lambda case: case.case_id)
    if len({case.case_id for case in result}) != len(result):
        raise RuntimeError(f"generated duplicate {tier} random-differential case ids")
    return result


def manifest_payload(
    master_seed: int, cases: Sequence[DifferentialCase], tier: str = "smoke"
) -> dict[str, Any]:
    return {
        "schema": SCHEMA_VERSION,
        "master_seed": f"0x{parse_seed(master_seed):016x}",
        "tier": tier,
        "cases": [case.to_dict() for case in cases],
    }


def replay_payload(master_seed: int, case: DifferentialCase) -> dict[str, Any]:
    return {
        "schema": SCHEMA_VERSION,
        "master_seed": f"0x{parse_seed(master_seed):016x}",
        "case": case.to_dict(),
    }


def load_replay(path: Path) -> tuple[int, DifferentialCase]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read replay manifest {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("replay manifest must be a JSON object")
    if value.get("schema") != SCHEMA_VERSION:
        raise ValueError(
            f"unsupported replay schema {value.get('schema')!r}; expected {SCHEMA_VERSION}"
        )
    raw_case = value.get("case", value if "case_id" in value else None)
    if raw_case is None and isinstance(value.get("cases"), list):
        manifest_cases = value["cases"]
        if len(manifest_cases) == 1:
            raw_case = manifest_cases[0]
    if not isinstance(raw_case, dict):
        raise ValueError("replay manifest lacks a single case object")
    fallback_seed = raw_case.get("case_seed")
    if value.get("master_seed") is None and fallback_seed is None:
        raise ValueError("replay manifest lacks both master_seed and case_seed")
    master_seed = parse_seed(value.get("master_seed", fallback_seed))
    try:
        return master_seed, DifferentialCase.from_dict(raw_case)
    except (KeyError, TypeError, argparse.ArgumentTypeError) as error:
        raise ValueError(f"invalid replay case: {error}") from error


def select_case_id(cases: Sequence[DifferentialCase], requested: str) -> DifferentialCase:
    exact = [case for case in cases if case.case_id == requested]
    if exact:
        return exact[0]
    prefix = [case for case in cases if case.case_id.startswith(requested)]
    if len(prefix) == 1:
        return prefix[0]
    if not prefix:
        raise ValueError(f"unknown case id or prefix: {requested}")
    raise ValueError(f"ambiguous case-id prefix {requested}: {len(prefix)} matches")


def active_case_is_supported(case: DifferentialCase) -> bool:
    common = (
        case.expectation == EXPECT_RUN
        and case.alias == "inplace"
        and case.stride_mode == "constant"
        and case.opt_level == 2
    )
    if not common:
        return False
    if case.kernel == "unit_i32" and case.dtype == "i32":
        return case.mask == "alltrue" and case.stride == 1
    if case.kernel == "masked_i32" and case.dtype == "i32":
        return case.mask in {"allfalse", "sparse"} and case.stride == 1
    if case.kernel == "indexed_i32" and case.dtype == "i32":
        return case.mask == "alltrue" and case.stride in {2, 4, -1, -2, -4}
    if case.kernel == "integer_reduction" and case.dtype == "i32":
        return case.mask == "alltrue" and case.stride == 1
    if case.kernel == "unit_f32" and case.dtype == "f32":
        return case.mask == "alltrue" and case.stride == 1
    return False


def replace_case(case: DifferentialCase, **changes: Any) -> DifferentialCase:
    """Return a replay-valid case with a newly computed stable id."""

    provisional = dataclasses.replace(case, case_id="", **changes)
    result = dataclasses.replace(
        provisional, case_id=provisional.computed_case_id()
    )
    result.validate()
    return result


def _smaller_length_candidates(length: int) -> list[int]:
    boundaries = [
        0,
        1,
        2,
        3,
        4,
        7,
        8,
        15,
        16,
        31,
        32,
        63,
        64,
        127,
        128,
        255,
        256,
        511,
        512,
        1023,
        1024,
        2047,
        2048,
    ]
    probe = length
    while probe > 1:
        probe //= 2
        boundaries.append(probe)
    return sorted({candidate for candidate in boundaries if candidate < length})


def _smaller_seed_candidates(seed: int) -> list[int]:
    candidates = [0, 1, seed & 0xFF, seed & 0xFFFF, seed & 0xFFFF_FFFF]
    # Clear successively larger high halves.  This is bounded and stable while
    # still reducing arbitrary 64-bit random inputs substantially.
    for keep_bits in (56, 48, 40, 32, 24, 16, 8):
        candidates.append(seed & ((1 << keep_bits) - 1))
    return sorted({candidate for candidate in candidates if candidate < seed})


def minimize_failing_case(
    case: DifferentialCase,
    reproduces: Callable[[DifferentialCase], bool],
) -> tuple[DifferentialCase, list[dict[str, Any]]]:
    """Greedily minimize a failing runtime case with a caller predicate.

    The predicate must return true only when the same failure still occurs.
    Candidates are replay-valid and the trace is persisted with the artifact.
    """

    if case.expectation != EXPECT_RUN:
        return case, []
    current = case
    trace: list[dict[str, Any]] = []

    def try_candidates(field: str, values: Iterable[int]) -> None:
        nonlocal current
        for value in values:
            if value == getattr(current, field):
                continue
            candidate = replace_case(current, **{field: value})
            try:
                failed = bool(reproduces(candidate))
                detail = None
            except Exception as error:
                # A broken shrink probe must not be mistaken for reproduction
                # of the original compiler/runtime failure.
                failed = False
                detail = str(error)
            trace.append(
                {
                    "field": field,
                    "value": value,
                    "case_id": candidate.case_id,
                    "reproduced": failed,
                    "detail": detail,
                }
            )
            if failed:
                current = candidate
                break

    try_candidates("length", _smaller_length_candidates(current.length))
    allowed_offsets = (0, 4, 8, 12) if current.kernel == "masked_i32" else ALIGNMENT_OFFSETS
    try_candidates(
        "alignment_offset",
        (offset for offset in allowed_offsets if offset < current.alignment_offset),
    )
    try_candidates("case_seed", _smaller_seed_candidates(current.case_seed))
    return current, trace


@dataclasses.dataclass
class CommandRecord:
    argv: list[str]
    returncode: int | None
    stdout: str
    stderr: str
    stdin_sha256: str | None = None
    timed_out: bool = False

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


class RecordedRunner:
    def __init__(self, timeout: float) -> None:
        self.timeout = timeout
        self.records: list[CommandRecord] = []

    def run(
        self,
        argv: Sequence[str | Path],
        *,
        input_bytes: bytes | None = None,
        timeout: float | None = None,
    ) -> subprocess.CompletedProcess[bytes]:
        command = [str(item) for item in argv]
        stdin_hash = (
            hashlib.sha256(input_bytes).hexdigest() if input_bytes is not None else None
        )
        try:
            result = subprocess.run(
                command,
                cwd=ROOT,
                input=input_bytes,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=self.timeout if timeout is None else timeout,
            )
        except subprocess.TimeoutExpired as error:
            stdout = (error.stdout or b"").decode(errors="replace")
            stderr = (error.stderr or b"").decode(errors="replace")
            self.records.append(
                CommandRecord(command, None, stdout, stderr, stdin_hash, True)
            )
            raise RuntimeError(f"command timed out: {shlex.join(command)}") from error
        self.records.append(
            CommandRecord(
                command,
                result.returncode,
                result.stdout.decode(errors="replace"),
                result.stderr.decode(errors="replace"),
                stdin_hash,
                False,
            )
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"command failed ({result.returncode}): {shlex.join(command)}\n"
                f"stdout:\n{result.stdout.decode(errors='replace')}\n"
                f"stderr:\n{result.stderr.decode(errors='replace')}"
            )
        return result


class CaseExecutionError(RuntimeError):
    def __init__(self, case: DifferentialCase, error: BaseException | str) -> None:
        self.case = case
        self.original_error = error
        super().__init__(str(error))


def failure_signature(error: BaseException | str) -> tuple[str, ...]:
    """Classify a failure independently of case id, data, and temp paths."""

    underlying = error.original_error if isinstance(error, CaseExecutionError) else error
    text = str(underlying)
    mismatch = re.search(r"scalar/RVV mismatch at VLEN=(\d+)", text)
    if mismatch is not None:
        return ("scalar-rvv-mismatch", mismatch.group(1))
    command = re.search(r"command failed \((-?\d+)\):", text)
    if command is not None:
        return ("command-exit", command.group(1))
    timeout = "timed out" in text.lower()
    if timeout:
        return ("timeout",)
    return (type(underlying).__name__, text.splitlines()[0] if text else "")


def require_tool(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise RuntimeError(f"required RVV random-differential tool not found: {name}")
    return resolved


def assembly_mnemonics(text: str) -> list[str]:
    result: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((".", "#")) or stripped.endswith(":"):
            continue
        mnemonic = stripped.split(None, 1)[0].lower()
        if re.fullmatch(r"[a-z][a-z0-9_.]*", mnemonic):
            result.append(mnemonic)
    return result


def disassembly_mnemonics(text: str) -> list[str]:
    pattern = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]*)\b")
    result: list[str] = []
    for line in text.lower().splitlines():
        match = pattern.match(line)
        if match is not None:
            result.append(match.group(1))
    return result


def is_rvv_mnemonic(mnemonic: str) -> bool:
    return mnemonic.startswith("v")


def disassembled_symbol(disassembly: str, symbol: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+ <{re.escape(symbol)}>:\n(?P<body>.*?)"
        rf"(?=^[0-9a-f]+ <[^>]+>:\n|\Z)",
        disassembly,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise RuntimeError(f"linked binary lacks disassembled symbol {symbol}")
    return match.group("body")


def require_scalar_attributes(readelf: str) -> None:
    lines = [line.lower() for line in readelf.splitlines() if "tag_riscv_arch" in line.lower()]
    if not lines:
        raise RuntimeError("scalar binary lacks Tag_RISCV_arch")
    arch = " ".join(lines)
    if re.search(r"(?:^|_)v\d", arch) or "zve" in arch:
        raise RuntimeError(f"scalar binary unexpectedly advertises vector ISA: {arch}")


def require_rvv_1p0_attributes(readelf: str) -> None:
    lines = [line.lower() for line in readelf.splitlines() if "tag_riscv_arch" in line.lower()]
    if not lines or "v1p0" not in " ".join(lines):
        raise RuntimeError("RVV binary does not advertise the V 1.0 ELF attribute")


@dataclasses.dataclass(frozen=True)
class ActiveBinaries:
    scalar: Path
    rvv: Path


def prepare_kernel_binaries(
    directory: Path,
    runner: RecordedRunner,
    compiler: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    readelf: str,
    *,
    kernel_name: str,
    source_text: str,
    driver_text: str,
    symbol_requirements: Sequence[tuple[str, Sequence[str]]],
) -> ActiveBinaries:
    source = directory / f"{kernel_name}.sy"
    driver = directory / f"{kernel_name}_driver.c"
    source.write_text(source_text.strip() + "\n", encoding="utf-8")
    driver.write_text(driver_text.strip() + "\n", encoding="utf-8")

    outputs: dict[str, Path] = {}
    for name, march, optimization in (
        ("scalar", "rv64gc", 1),
        ("rvv", "rv64gcv", 2),
    ):
        assembly = directory / f"{kernel_name}_{name}.s"
        obj = directory / f"{kernel_name}_{name}.o"
        executable = directory / f"{kernel_name}_{name}"
        runner.run(
            [
                compiler,
                source,
                "-S",
                f"-O{optimization}",
                f"-march={march}",
                "-mabi=lp64d",
                "-o",
                assembly,
            ]
        )
        runner.run(
            [assembler, f"-march={march}", "-mabi=lp64d", assembly, "-o", obj]
        )
        runner.run(
            [
                gcc,
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-tree-vectorize",
                "-fno-tree-slp-vectorize",
                "-static",
                f"-march={march}",
                "-mabi=lp64d",
                "-mcmodel=medany",
                obj,
                driver,
                "-o",
                executable,
            ]
        )
        assembly_text = assembly.read_text(encoding="utf-8")
        binary_disassembly = runner.run([objdump, "-d", executable]).stdout.decode()
        symbol_bodies = {
            symbol: disassembled_symbol(binary_disassembly, symbol)
            for symbol, _ in symbol_requirements
        }
        attributes = runner.run([readelf, "-A", executable]).stdout.decode()
        if name == "scalar":
            vector_asm = sorted(
                {mnemonic for mnemonic in assembly_mnemonics(assembly_text) if is_rvv_mnemonic(mnemonic)}
            )
            vector_binary = sorted(
                {
                    mnemonic
                    for body in symbol_bodies.values()
                    for mnemonic in disassembly_mnemonics(body)
                    if is_rvv_mnemonic(mnemonic)
                }
            )
            if vector_asm or vector_binary:
                raise RuntimeError(
                    "rv64gc control contains RVV instructions: "
                    + ", ".join(sorted(set(vector_asm + vector_binary)))
                )
            require_scalar_attributes(attributes)
        else:
            for symbol, required_mnemonics in symbol_requirements:
                for mnemonic in required_mnemonics:
                    if mnemonic not in symbol_bodies[symbol]:
                        raise RuntimeError(
                            f"executed RVV symbol {symbol} lacks {mnemonic}"
                        )
                    if mnemonic not in assembly_text:
                        raise RuntimeError(f"compiler RVV assembly lacks {mnemonic}")
            require_rvv_1p0_attributes(attributes)
        outputs[name] = executable
    return ActiveBinaries(outputs["scalar"], outputs["rvv"])


def prepare_active_binaries(
    directory: Path,
    runner: RecordedRunner,
    compiler: Path,
    assembler: str,
    gcc: str,
    objdump: str,
    readelf: str,
    kernels: Iterable[str],
) -> dict[str, ActiveBinaries]:
    requested = set(kernels)
    result: dict[str, ActiveBinaries] = {}
    if "unit_i32" in requested:
        result["unit_i32"] = prepare_kernel_binaries(
            directory,
            runner,
            compiler,
            assembler,
            gcc,
            objdump,
            readelf,
            kernel_name="unit_i32",
            source_text=ACTIVE_SOURCE,
            driver_text=ACTIVE_DRIVER,
            symbol_requirements=(
                (
                    "random_add_bias",
                    ("vsetvli", "vle32.v", "vadd.vv", "vse32.v"),
                ),
            ),
        )
    if "masked_i32" in requested:
        result["masked_i32"] = prepare_kernel_binaries(
            directory,
            runner,
            compiler,
            assembler,
            gcc,
            objdump,
            readelf,
            kernel_name="masked_i32",
            source_text=mask_guard.SOURCE.read_text(encoding="utf-8"),
            driver_text=mask_guard.HARNESS.read_text(encoding="utf-8"),
            symbol_requirements=tuple(
                (
                    case.function,
                    ("vsetivli", "vle32.v", "vse32.v"),
                )
                for case in mask_guard.CASES
            ),
        )
    if "unit_f32" in requested:
        result["unit_f32"] = prepare_kernel_binaries(
            directory,
            runner,
            compiler,
            assembler,
            gcc,
            objdump,
            readelf,
            kernel_name="unit_f32",
            source_text=FLOAT_SOURCE,
            driver_text=FLOAT_DRIVER,
            symbol_requirements=(
                (
                    "random_float_bias",
                    ("vsetvli", "vle32.v", "vfadd.vv", "vse32.v"),
                ),
            ),
        )
    if "indexed_i32" in requested:
        indexed_symbols = (
            "random_stride_p2",
            "random_stride_p4",
            "random_stride_n1",
            "random_stride_n2",
            "random_stride_n4",
        )
        result["indexed_i32"] = prepare_kernel_binaries(
            directory,
            runner,
            compiler,
            assembler,
            gcc,
            objdump,
            readelf,
            kernel_name="indexed_i32",
            source_text=INDEXED_SOURCE,
            driver_text=INDEXED_DRIVER,
            symbol_requirements=tuple(
                (symbol, ("vsetvli", "vlse32.v", "vsse32.v"))
                for symbol in indexed_symbols
            ),
        )
    if "integer_reduction" in requested:
        result["integer_reduction"] = prepare_kernel_binaries(
            directory,
            runner,
            compiler,
            assembler,
            gcc,
            objdump,
            readelf,
            kernel_name="integer_reduction",
            source_text=REDUCTION_SOURCE,
            driver_text=REDUCTION_DRIVER,
            symbol_requirements=(
                ("random_reduce_add", ("vle32.v", "vredsum.vs")),
                ("random_reduce_mul", ("vle32.v", "vfirst.m", "mulw")),
                ("random_reduce_and", ("vle32.v", "vredand.vs")),
                ("random_reduce_or", ("vle32.v", "vredor.vs")),
                ("random_reduce_xor", ("vle32.v", "vredxor.vs")),
            ),
        )
    unknown = requested - set(result)
    if unknown:
        raise RuntimeError("no active binary builder for: " + ", ".join(sorted(unknown)))
    return result


def run_compile_contracts(
    cases: Iterable[DifferentialCase],
    directory: Path,
    runner: RecordedRunner,
    compiler: Path,
) -> int:
    cached: dict[str, dict[str, Any]] = {}
    checked = 0
    for case in cases:
        if case.expectation not in {EXPECT_PLAN, EXPECT_REJECT}:
            continue
        try:
            contract = CONTRACT_SOURCES[case.kernel]
            if case.kernel not in cached:
                source = directory / f"contract_{case.kernel}.sy"
                source.write_text(contract.source.strip() + "\n", encoding="utf-8")
                output = runner.run(
                    [
                        compiler,
                        source,
                        "--emit-vector-plan",
                        "-O2",
                        "-march=rv64gcv",
                        "-mabi=lp64d",
                    ]
                ).stdout.decode()
                try:
                    cached[case.kernel] = json.loads(output)
                except json.JSONDecodeError as error:
                    raise RuntimeError(
                        f"{case.kernel} vector-plan output is not JSON: {output}"
                    ) from error
            plans = cached[case.kernel].get("vectorization_plans", [])
            matching = [
                plan
                for plan in plans
                if isinstance(plan, dict) and plan.get("function") == contract.function
            ]
            if len(matching) != 1:
                raise RuntimeError(
                    f"{case.kernel} expected one plan for {contract.function}, "
                    f"got {len(matching)}"
                )
            actual_code = matching[0].get("code")
            if actual_code != case.expected_code:
                raise RuntimeError(
                    f"{case.kernel} rejection contract changed: expected "
                    f"{case.expected_code}, actual {actual_code}; "
                    f"plan={canonical_json(matching[0])}"
                )
        except CaseExecutionError:
            raise
        except Exception as error:
            raise CaseExecutionError(case, error) from error
        label = "PLAN" if case.expectation == EXPECT_PLAN else "REJECT"
        print(f"PASS {label} {case.case_id} {case.expected_code}")
        checked += 1
    return checked


def execute_active_cases(
    cases: Iterable[DifferentialCase],
    binaries: dict[str, ActiveBinaries],
    runner: RecordedRunner,
    qemu: str,
    vlens: Sequence[int],
    *,
    quiet: bool = False,
) -> int:
    executed = 0
    for case in cases:
        try:
            if case.expectation != EXPECT_RUN:
                continue
            if not active_case_is_supported(case):
                raise RuntimeError(
                    f"case {case.case_id} was marked run outside the supported surface"
                )
            kernel_binaries = binaries[case.kernel]
            if case.kernel == "masked_i32":
                runtime_arguments = mask_guard.harness_arguments(
                    case.mask, case.case_seed, case.alignment_offset
                )
                input_bytes = None
            else:
                runtime_arguments = []
                fields = [
                    str(case.length),
                    str(case.alignment_offset),
                    str(case.case_seed),
                ]
                if case.kernel == "indexed_i32":
                    fields.append(str(case.stride))
                input_bytes = (" ".join(fields) + "\n").encode("ascii")
            scalar = runner.run(
                [
                    qemu,
                    "-cpu",
                    "rv64,v=false",
                    kernel_binaries.scalar,
                    *runtime_arguments,
                ],
                input_bytes=input_bytes,
                timeout=30.0,
            ).stdout
            for vlen in vlens:
                rvv = runner.run(
                    [
                        qemu,
                        "-cpu",
                        f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0",
                        kernel_binaries.rvv,
                        *runtime_arguments,
                    ],
                    input_bytes=input_bytes,
                    timeout=30.0,
                ).stdout
                if rvv != scalar:
                    raise RuntimeError(
                        f"case {case.case_id} scalar/RVV mismatch at VLEN={vlen}: "
                        f"scalar={scalar!r}, rvv={rvv!r}"
                    )
            if not quiet:
                print(
                    f"PASS RUN {case.case_id} n={case.length} "
                    f"align+={case.alignment_offset} mask={case.mask} "
                    f"VLEN={','.join(map(str, vlens))}"
                )
            executed += 1
        except CaseExecutionError:
            raise
        except Exception as error:
            raise CaseExecutionError(case, error) from error
    return executed


def unique_artifact_directory(root: Path, case_id: str) -> Path:
    base = root / case_id
    if not base.exists():
        return base
    index = 1
    while (root / f"{case_id}-{index}").exists():
        index += 1
    return root / f"{case_id}-{index}"


def preserve_failure(
    artifact_root: Path,
    master_seed: int,
    case: DifferentialCase,
    error: BaseException | str,
    workspace: Path,
    records: Sequence[CommandRecord],
    compiler: Path | None = None,
    minimized_case: DifferentialCase | None = None,
    shrink_trace: Sequence[dict[str, Any]] = (),
) -> Path:
    destination = unique_artifact_directory(artifact_root, case.case_id)
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "case.json").write_text(
        json.dumps(replay_payload(master_seed, case), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (destination / "failure.txt").write_text(str(error) + "\n", encoding="utf-8")
    (destination / "commands.json").write_text(
        json.dumps([record.to_dict() for record in records], indent=2, sort_keys=True)
        + "\n",
        encoding="utf-8",
    )
    replay_name = "case.json"
    if minimized_case is not None and minimized_case != case:
        replay_name = "minimized-case.json"
        (destination / replay_name).write_text(
            json.dumps(replay_payload(master_seed, minimized_case), indent=2, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        (destination / "shrink.json").write_text(
            json.dumps(list(shrink_trace), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if workspace.exists():
        shutil.copytree(workspace, destination / "workspace")
    if case.expectation == EXPECT_RUN:
        if case.kernel == "masked_i32":
            input_description = "argv: " + " ".join(
                mask_guard.harness_arguments(
                    case.mask, case.case_seed, case.alignment_offset
                )
            )
        else:
            input_description = f"{case.length} {case.alignment_offset} {case.case_seed}"
            if case.kernel == "indexed_i32":
                input_description += f" {case.stride}"
        (destination / "input.txt").write_text(
            input_description + "\n", encoding="ascii"
        )
    replay_script = destination / "repro.sh"
    compiler_argument = (
        f"{shlex.quote('--compiler=' + str(compiler))} "
        if compiler is not None
        else ""
    )
    replay_script.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        'artifact_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        f"exec {shlex.quote(sys.executable)} "
        f"{shlex.quote(str(ROOT / 'scripts/rvv_random_differential_infra_tests.py'))} "
        f"{compiler_argument}"
        f'"--replay=$artifact_dir/{replay_name}" '
        '"--artifact-dir=$artifact_dir/replay-artifacts"\n',
        encoding="utf-8",
    )
    replay_script.chmod(0o755)
    return destination


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run reproducible RVV random differential smoke tests"
    )
    parser.add_argument("--seed", type=parse_seed, default=DEFAULT_SEED)
    parser.add_argument("--case-id", help="run one exact case id or unique id prefix")
    parser.add_argument("--replay", type=Path, help="replay a preserved case.json")
    parser.add_argument("--artifact-dir", type=Path, default=DEFAULT_ARTIFACT_DIR)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--tier", choices=TIERS, default="smoke")
    parser.add_argument(
        "--vlen",
        type=int,
        action="append",
        choices=VLENS,
        help="limit execution to one or more VLEN values",
    )
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument(
        "--manifest-only",
        action="store_true",
        help="print the selected deterministic manifest without requiring a toolchain",
    )
    parser.add_argument(
        "--allow-blocked",
        action="store_true",
        help="run the supported developer subset despite required backend blockers",
    )
    return parser.parse_args(argv)


def selected_cases(args: argparse.Namespace) -> tuple[int, list[DifferentialCase]]:
    if args.replay is not None:
        master_seed, case = load_replay(args.replay)
        cases = [case]
    else:
        master_seed = args.seed
        cases = generate_tier_cases(master_seed, args.tier)
    if args.case_id:
        cases = [select_case_id(cases, args.case_id)]
    return master_seed, cases


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        master_seed, cases = selected_cases(args)
        if args.manifest_only:
            print(
                json.dumps(
                    manifest_payload(master_seed, cases, args.tier),
                    indent=2,
                    sort_keys=True,
                )
            )
            return 0
        active = [case for case in cases if case.expectation == EXPECT_RUN]
        plans = [case for case in cases if case.expectation == EXPECT_PLAN]
        rejects = [case for case in cases if case.expectation == EXPECT_REJECT]
        blocked = [case for case in cases if case.expectation == EXPECT_BLOCKED]
        if (active or plans or rejects) and not args.compiler.is_file():
            raise RuntimeError(f"compiler not found: {args.compiler}")
        assembler = require_tool("riscv64-linux-gnu-as") if active else None
        gcc = require_tool("riscv64-linux-gnu-gcc") if active else None
        objdump = require_tool("riscv64-linux-gnu-objdump") if active else None
        readelf = require_tool("riscv64-linux-gnu-readelf") if active else None
        qemu = require_tool("qemu-riscv64") if active else None
        vlens = tuple(args.vlen or VLENS)
        runner = RecordedRunner(args.timeout)
        fallback_case = cases[0]
        binaries: dict[str, ActiveBinaries] = {}
        with tempfile.TemporaryDirectory(prefix="yoolang-rvv-random-diff-") as temp:
            workspace = Path(temp)
            try:
                # Declare known debt before the potentially long QEMU matrix so
                # bounded CI log capture cannot hide unsupported dimensions.
                for case in blocked:
                    print(f"BLOCKED {case.case_id} {case.reason}")
                contract_count = run_compile_contracts(
                    cases, workspace, runner, args.compiler
                )
                plan_count = len(plans)
                reject_count = contract_count - plan_count
                run_count = 0
                if active:
                    fallback_case = active[0]
                    binaries = prepare_active_binaries(
                        workspace,
                        runner,
                        args.compiler,
                        str(assembler),
                        str(gcc),
                        str(objdump),
                        str(readelf),
                        {case.kernel for case in active},
                    )
                    for case in active:
                        fallback_case = case
                        run_count += execute_active_cases(
                            [case], binaries, runner, str(qemu), vlens
                        )
                if blocked and not args.allow_blocked:
                    reasons = ",".join(sorted({str(case.reason) for case in blocked}))
                    print(
                        "BLOCKED_IMPLEMENTATION rvv_random_differential "
                        f"required_backend_contracts={len(blocked)} reasons={reasons}",
                        file=sys.stderr,
                    )
                    return 2
                print(
                    f"PASS rvv_random_differential_{args.tier} run={run_count} "
                    f"plan={plan_count} reject={reject_count} blocked={len(blocked)} "
                    f"developer_subset={str(bool(blocked)).lower()} "
                    f"seed=0x{master_seed:016x}"
                )
                return 0
            except Exception as error:
                artifact_case = (
                    error.case if isinstance(error, CaseExecutionError) else fallback_case
                )
                minimized_case = artifact_case
                shrink_trace: list[dict[str, Any]] = []
                if (
                    isinstance(error, CaseExecutionError)
                    and artifact_case.expectation == EXPECT_RUN
                    and artifact_case.kernel in binaries
                    and qemu is not None
                ):
                    original_signature = failure_signature(error)

                    def reproduces(candidate: DifferentialCase) -> bool:
                        probe_runner = RecordedRunner(args.timeout)
                        try:
                            execute_active_cases(
                                [candidate],
                                binaries,
                                probe_runner,
                                str(qemu),
                                vlens,
                                quiet=True,
                            )
                        except CaseExecutionError as probe_error:
                            return failure_signature(probe_error) == original_signature
                        return False

                    minimized_case, shrink_trace = minimize_failing_case(
                        artifact_case, reproduces
                    )
                artifact = preserve_failure(
                    args.artifact_dir,
                    master_seed,
                    artifact_case,
                    error,
                    workspace,
                    runner.records,
                    args.compiler,
                    minimized_case,
                    shrink_trace,
                )
                minimized = ""
                if minimized_case != artifact_case:
                    minimized = (
                        f"\nminimized case: {minimized_case.case_id} "
                        f"n={minimized_case.length} "
                        f"align+={minimized_case.alignment_offset} "
                        f"seed=0x{minimized_case.case_seed:016x}"
                    )
                print(
                    f"FAIL rvv_random_differential: {error}\n"
                    f"failure artifact: {artifact}{minimized}",
                    file=sys.stderr,
                )
                return 1
    except (RuntimeError, ValueError, argparse.ArgumentTypeError) as error:
        print(f"FAIL rvv_random_differential: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
