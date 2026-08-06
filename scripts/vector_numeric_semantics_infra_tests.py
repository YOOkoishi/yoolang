#!/usr/bin/env python3

from __future__ import annotations

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
    os.environ.get("YOOLANG_COMPILER", ROOT / "build/linux/x86_64/release/compiler")
)
MARCH = "rv64gc"
MABI = "lp64d"


KERNEL_SOURCE = r"""
int scalar_pick_i7(int lane, int minimum, int maximum) {
  if (lane == 0) return minimum;
  if (lane == 1) return maximum;
  if (lane == 2) return -1;
  if (lane == 3) return 0;
  if (lane == 4) return 1;
  if (lane == 5) return minimum + 1;
  return maximum - 1;
}

int vector_iadd7(int lane, int minimum, int maximum, int rhs) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -1, 0, 1, minimum + 1, maximum - 1};
  vector<int,7> result = values + vector<int,7>(rhs);
  return result[lane];
}

int scalar_iadd7(int lane, int minimum, int maximum, int rhs) {
  return scalar_pick_i7(lane, minimum, maximum) + rhs;
}

int vector_isub7(int lane, int minimum, int maximum, int rhs) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -1, 0, 1, minimum + 1, maximum - 1};
  vector<int,7> result = values - vector<int,7>(rhs);
  return result[lane];
}

int scalar_isub7(int lane, int minimum, int maximum, int rhs) {
  return scalar_pick_i7(lane, minimum, maximum) - rhs;
}

int vector_imul7(int lane, int minimum, int maximum, int rhs) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -1, 0, 1, minimum + 1, maximum - 1};
  vector<int,7> result = values * vector<int,7>(rhs);
  return result[lane];
}

int scalar_imul7(int lane, int minimum, int maximum, int rhs) {
  return scalar_pick_i7(lane, minimum, maximum) * rhs;
}

int vector_idiv7(int lane, int minimum, int maximum, int rhs) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -1, 0, 1, minimum + 1, maximum - 1};
  vector<int,7> result = values / vector<int,7>(rhs);
  return result[lane];
}

int scalar_idiv7(int lane, int minimum, int maximum, int rhs) {
  return scalar_pick_i7(lane, minimum, maximum) / rhs;
}

int vector_irem7(int lane, int minimum, int maximum, int rhs) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -1, 0, 1, minimum + 1, maximum - 1};
  vector<int,7> result = values % vector<int,7>(rhs);
  return result[lane];
}

int scalar_irem7(int lane, int minimum, int maximum, int rhs) {
  return scalar_pick_i7(lane, minimum, maximum) % rhs;
}

int vector_shape_n1(int value, int rhs) {
  vector<int,1> values = vector<int,1>{value};
  vector<int,1> result = values + vector<int,1>(rhs);
  return result[0];
}

int scalar_shape_n1(int value, int rhs) { return value + rhs; }

int vector_shape_n3(int lane, int value, int rhs) {
  vector<int,3> values = vector<int,3>{value, rhs, value ^ rhs};
  vector<int,3> result = values * vector<int,3>(-1);
  return result[lane];
}

int scalar_shape_n3(int lane, int value, int rhs) {
  int selected = value;
  if (lane == 1) selected = rhs;
  if (lane == 2) selected = value ^ rhs;
  return selected * -1;
}

int vector_shape_n31(int lane, int value, int rhs) {
  vector<int,31> values = vector<int,31>(value);
  vector<int,31> result = values + vector<int,31>(rhs);
  return result[lane];
}

int scalar_shape_n31(int lane, int value, int rhs) {
  return value + rhs + lane * 0;
}

float scalar_pick_f7(int lane, float x0, float x1, float x2, float x3,
                     float x4, float x5, float x6) {
  if (lane == 0) return x0;
  if (lane == 1) return x1;
  if (lane == 2) return x2;
  if (lane == 3) return x3;
  if (lane == 4) return x4;
  if (lane == 5) return x5;
  return x6;
}

float vector_fadd7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<float,7> result = values + vector<float,7>(rhs);
  return result[lane];
}

float scalar_fadd7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  return scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6) + rhs;
}

float vector_fsub7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<float,7> result = values - vector<float,7>(rhs);
  return result[lane];
}

float scalar_fsub7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  return scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6) - rhs;
}

float vector_fmul7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<float,7> result = values * vector<float,7>(rhs);
  return result[lane];
}

float scalar_fmul7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  return scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6) * rhs;
}

float vector_fdiv7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<float,7> result = values / vector<float,7>(rhs);
  return result[lane];
}

float scalar_fdiv7(int lane, float x0, float x1, float x2, float x3,
                   float x4, float x5, float x6, float rhs) {
  return scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6) / rhs;
}

int vector_fcmp7(int lane, float x0, float x1, float x2, float x3,
                 float x4, float x5, float x6, float rhs) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<float,7> other = vector<float,7>(rhs);
  mask<7> eq = values == other;
  mask<7> ne = values != other;
  mask<7> lt = values < other;
  mask<7> le = values <= other;
  mask<7> gt = values > other;
  mask<7> ge = values >= other;
  return eq[lane] + ne[lane] * 2 + lt[lane] * 4 +
         le[lane] * 8 + gt[lane] * 16 + ge[lane] * 32;
}

int scalar_fcmp7(int lane, float x0, float x1, float x2, float x3,
                 float x4, float x5, float x6, float rhs) {
  float value = scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6);
  return (value == rhs) + (value != rhs) * 2 + (value < rhs) * 4 +
         (value <= rhs) * 8 + (value > rhs) * 16 + (value >= rhs) * 32;
}

float vector_i2f7(int lane, int minimum, int maximum) {
  vector<int,7> values =
      vector<int,7>{minimum, maximum, -16777217, -1, 0, 1, 16777217};
  vector<float,7> converted = vector<float,7>(values);
  return converted[lane];
}

float scalar_i2f7(int lane, int minimum, int maximum) {
  int value = minimum;
  if (lane == 1) value = maximum;
  if (lane == 2) value = -16777217;
  if (lane == 3) value = -1;
  if (lane == 4) value = 0;
  if (lane == 5) value = 1;
  if (lane == 6) value = 16777217;
  return value;
}

int vector_f2i7(int lane, float x0, float x1, float x2, float x3,
                float x4, float x5, float x6) {
  vector<float,7> values = vector<float,7>{x0,x1,x2,x3,x4,x5,x6};
  vector<int,7> converted = vector<int,7>(values);
  return converted[lane];
}

int scalar_f2i7(int lane, float x0, float x1, float x2, float x3,
                float x4, float x5, float x6) {
  float value = scalar_pick_f7(lane, x0, x1, x2, x3, x4, x5, x6);
  return value;
}

float vector_reduce_add7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  return reduce_add(vector<float,7>{x0,x1,x2,x3,x4,x5,x6});
}

float scalar_reduce_add7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  float result = x0;
  result = result + x1;
  result = result + x2;
  result = result + x3;
  result = result + x4;
  result = result + x5;
  result = result + x6;
  return result;
}

float vector_reduce_mul7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  return reduce_mul(vector<float,7>{x0,x1,x2,x3,x4,x5,x6});
}

float scalar_reduce_mul7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  float result = x0;
  result = result * x1;
  result = result * x2;
  result = result * x3;
  result = result * x4;
  result = result * x5;
  result = result * x6;
  return result;
}

float vector_reduce_min7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  return reduce_min(vector<float,7>{x0,x1,x2,x3,x4,x5,x6});
}

float scalar_reduce_min7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  float result = x0;
  if (x1 < result) result = x1;
  if (x2 < result) result = x2;
  if (x3 < result) result = x3;
  if (x4 < result) result = x4;
  if (x5 < result) result = x5;
  if (x6 < result) result = x6;
  return result;
}

float vector_reduce_max7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  return reduce_max(vector<float,7>{x0,x1,x2,x3,x4,x5,x6});
}

float scalar_reduce_max7(float x0, float x1, float x2, float x3,
                         float x4, float x5, float x6) {
  float result = x0;
  if (x1 > result) result = x1;
  if (x2 > result) result = x2;
  if (x3 > result) result = x3;
  if (x4 > result) result = x4;
  if (x5 > result) result = x5;
  if (x6 > result) result = x6;
  return result;
}
"""


DRIVER_SOURCE = r"""
#include <limits.h>
#include <stdint.h>
#include <string.h>

extern int32_t vector_iadd7(int32_t, int32_t, int32_t, int32_t);
extern int32_t scalar_iadd7(int32_t, int32_t, int32_t, int32_t);
extern int32_t vector_isub7(int32_t, int32_t, int32_t, int32_t);
extern int32_t scalar_isub7(int32_t, int32_t, int32_t, int32_t);
extern int32_t vector_imul7(int32_t, int32_t, int32_t, int32_t);
extern int32_t scalar_imul7(int32_t, int32_t, int32_t, int32_t);
extern int32_t vector_idiv7(int32_t, int32_t, int32_t, int32_t);
extern int32_t scalar_idiv7(int32_t, int32_t, int32_t, int32_t);
extern int32_t vector_irem7(int32_t, int32_t, int32_t, int32_t);
extern int32_t scalar_irem7(int32_t, int32_t, int32_t, int32_t);
extern int32_t vector_shape_n1(int32_t, int32_t);
extern int32_t scalar_shape_n1(int32_t, int32_t);
extern int32_t vector_shape_n3(int32_t, int32_t, int32_t);
extern int32_t scalar_shape_n3(int32_t, int32_t, int32_t);
extern int32_t vector_shape_n31(int32_t, int32_t, int32_t);
extern int32_t scalar_shape_n31(int32_t, int32_t, int32_t);

#define FLOAT_ARGS float, float, float, float, float, float, float
#define FLOAT_VALUES float x0, float x1, float x2, float x3, float x4, float x5, float x6
#define PASS_FLOATS x[0], x[1], x[2], x[3], x[4], x[5], x[6]

extern float vector_fadd7(int32_t, FLOAT_ARGS, float);
extern float scalar_fadd7(int32_t, FLOAT_ARGS, float);
extern float vector_fsub7(int32_t, FLOAT_ARGS, float);
extern float scalar_fsub7(int32_t, FLOAT_ARGS, float);
extern float vector_fmul7(int32_t, FLOAT_ARGS, float);
extern float scalar_fmul7(int32_t, FLOAT_ARGS, float);
extern float vector_fdiv7(int32_t, FLOAT_ARGS, float);
extern float scalar_fdiv7(int32_t, FLOAT_ARGS, float);
extern int32_t vector_fcmp7(int32_t, FLOAT_ARGS, float);
extern int32_t scalar_fcmp7(int32_t, FLOAT_ARGS, float);
extern float vector_i2f7(int32_t, int32_t, int32_t);
extern float scalar_i2f7(int32_t, int32_t, int32_t);
extern int32_t vector_f2i7(int32_t, FLOAT_ARGS);
extern int32_t scalar_f2i7(int32_t, FLOAT_ARGS);
extern float vector_reduce_add7(FLOAT_ARGS);
extern float scalar_reduce_add7(FLOAT_ARGS);
extern float vector_reduce_mul7(FLOAT_ARGS);
extern float scalar_reduce_mul7(FLOAT_ARGS);
extern float vector_reduce_min7(FLOAT_ARGS);
extern float scalar_reduce_min7(FLOAT_ARGS);
extern float vector_reduce_max7(FLOAT_ARGS);
extern float scalar_reduce_max7(FLOAT_ARGS);

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int32_t int_from_bits(uint32_t bits) {
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int32_t pick_i7(int32_t lane) {
    static const int32_t values[7] = {
        INT32_MIN, INT32_MAX, -1, 0, 1, INT32_MIN + 1, INT32_MAX - 1,
    };
    return values[lane];
}

static int32_t wrap_add(int32_t lhs, int32_t rhs) {
    return int_from_bits((uint32_t)lhs + (uint32_t)rhs);
}

static int32_t wrap_sub(int32_t lhs, int32_t rhs) {
    return int_from_bits((uint32_t)lhs - (uint32_t)rhs);
}

static int32_t wrap_mul(int32_t lhs, int32_t rhs) {
    return int_from_bits((uint32_t)lhs * (uint32_t)rhs);
}

static int check_integer_semantics(void) {
    static const int32_t wrap_rhs[] = {1, -1, 2, INT32_MAX, INT32_MIN};
    static const int32_t div_rhs[] = {-1, 1, 2, -3, INT32_MIN, INT32_MAX};
    for (int32_t lane = 0; lane < 7; ++lane) {
        const int32_t lhs = pick_i7(lane);
        for (unsigned index = 0; index < sizeof(wrap_rhs) / sizeof(wrap_rhs[0]); ++index) {
            const int32_t rhs = wrap_rhs[index];
            const int32_t va = vector_iadd7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t sa = scalar_iadd7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t vs = vector_isub7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t ss = scalar_isub7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t vm = vector_imul7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t sm = scalar_imul7(lane, INT32_MIN, INT32_MAX, rhs);
            if (va != sa || sa != wrap_add(lhs, rhs)) return 11;
            if (vs != ss || ss != wrap_sub(lhs, rhs)) return 12;
            if (vm != sm || sm != wrap_mul(lhs, rhs)) return 13;
        }
        for (unsigned index = 0; index < sizeof(div_rhs) / sizeof(div_rhs[0]); ++index) {
            const int32_t rhs = div_rhs[index];
            const int32_t vd = vector_idiv7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t sd = scalar_idiv7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t vr = vector_irem7(lane, INT32_MIN, INT32_MAX, rhs);
            const int32_t sr = scalar_irem7(lane, INT32_MIN, INT32_MAX, rhs);
            if (lhs == INT32_MIN && rhs == -1) {
                /* SysY/C does not independently define this overflow case.  It
                   remains a differential check against the same compiler's
                   scalar kernel, without assigning it a new source meaning. */
                if (vd != sd) return 14;
                if (vr != sr) return 15;
            } else {
                const int32_t expected_div = lhs / rhs;
                const int32_t expected_rem = lhs % rhs;
                if (vd != sd || sd != expected_div) return 14;
                if (vr != sr || sr != expected_rem) return 15;
            }
        }
    }
    return 0;
}

static int check_shapes(void) {
    if (vector_shape_n1(INT32_MAX, 1) != scalar_shape_n1(INT32_MAX, 1)) return 21;
    for (int32_t lane = 0; lane < 3; ++lane) {
        if (vector_shape_n3(lane, INT32_MIN, INT32_MAX) !=
            scalar_shape_n3(lane, INT32_MIN, INT32_MAX)) return 22;
    }
    static const int32_t lanes[] = {0, 7, 30};
    for (unsigned index = 0; index < sizeof(lanes) / sizeof(lanes[0]); ++index) {
        const int32_t lane = lanes[index];
        if (vector_shape_n31(lane, INT32_MAX, 1) !=
            scalar_shape_n31(lane, INT32_MAX, 1)) return 23;
    }
    return 0;
}

static int check_float_arithmetic_and_comparisons(void) {
    float x[7] = {
        float_from_bits(UINT32_C(0x7fc12345)),
        float_from_bits(UINT32_C(0x7f800000)),
        float_from_bits(UINT32_C(0xff800000)),
        float_from_bits(UINT32_C(0x00000000)),
        float_from_bits(UINT32_C(0x80000000)),
        float_from_bits(UINT32_C(0x00000001)),
        float_from_bits(UINT32_C(0x3fc00000)),
    };
    static const uint32_t rhs_bits[] = {
        UINT32_C(0x3f800000), UINT32_C(0x80000000),
        UINT32_C(0x00000001), UINT32_C(0x7f800000),
    };
    for (unsigned rhs_index = 0; rhs_index < sizeof(rhs_bits) / sizeof(rhs_bits[0]); ++rhs_index) {
        const float rhs = float_from_bits(rhs_bits[rhs_index]);
        for (int32_t lane = 0; lane < 7; ++lane) {
            if (float_bits(vector_fadd7(lane, PASS_FLOATS, rhs)) !=
                float_bits(scalar_fadd7(lane, PASS_FLOATS, rhs))) return 31;
            if (float_bits(vector_fsub7(lane, PASS_FLOATS, rhs)) !=
                float_bits(scalar_fsub7(lane, PASS_FLOATS, rhs))) return 32;
            if (float_bits(vector_fmul7(lane, PASS_FLOATS, rhs)) !=
                float_bits(scalar_fmul7(lane, PASS_FLOATS, rhs))) return 33;
            if (float_bits(vector_fdiv7(lane, PASS_FLOATS, rhs)) !=
                float_bits(scalar_fdiv7(lane, PASS_FLOATS, rhs))) return 34;
            if (vector_fcmp7(lane, PASS_FLOATS, rhs) !=
                scalar_fcmp7(lane, PASS_FLOATS, rhs)) return 35;
        }
    }
    const float nan_rhs = float_from_bits(UINT32_C(0x7fc0beef));
    for (int32_t lane = 0; lane < 7; ++lane) {
        if (vector_fcmp7(lane, PASS_FLOATS, nan_rhs) !=
            scalar_fcmp7(lane, PASS_FLOATS, nan_rhs)) return 36;
    }
    return 0;
}

static int check_casts(void) {
    for (int32_t lane = 0; lane < 7; ++lane) {
        if (float_bits(vector_i2f7(lane, INT32_MIN, INT32_MAX)) !=
            float_bits(scalar_i2f7(lane, INT32_MIN, INT32_MAX))) return 41;
    }
    float x[7] = {
        -12345.75f, -1.75f, -0.0f, 0.0f, 1.75f, 12345.75f, 16777215.0f,
    };
    for (int32_t lane = 0; lane < 7; ++lane) {
        if (vector_f2i7(lane, PASS_FLOATS) != scalar_f2i7(lane, PASS_FLOATS)) return 42;
    }
    return 0;
}

typedef float (*reduction_fn)(FLOAT_ARGS);

static int compare_reduction_pair(reduction_fn vector_fn, reduction_fn scalar_fn,
                                  const float x[7], int code) {
    const float vector_value = vector_fn(PASS_FLOATS);
    const float scalar_value = scalar_fn(PASS_FLOATS);
    return float_bits(vector_value) == float_bits(scalar_value) ? 0 : code;
}

static int check_reductions(void) {
    float cases[][7] = {
        {1.0e20f, -1.0e20f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f},
        {0.0f, -0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f},
        {-0.0f, 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f},
        {float_from_bits(UINT32_C(0x7fc12345)), 1.0f, -2.0f,
         float_from_bits(UINT32_C(0x7f800000)),
         float_from_bits(UINT32_C(0xff800000)),
         float_from_bits(UINT32_C(0x00000001)), -0.0f},
        {1.0f, float_from_bits(UINT32_C(0x7fc0beef)), -2.0f,
         float_from_bits(UINT32_C(0x7f800000)),
         float_from_bits(UINT32_C(0xff800000)),
         float_from_bits(UINT32_C(0x00000001)), -0.0f},
    };
    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        float *x = cases[index];
        int result = compare_reduction_pair(vector_reduce_add7, scalar_reduce_add7, x, 51);
        if (result != 0) return result;
        result = compare_reduction_pair(vector_reduce_mul7, scalar_reduce_mul7, x, 52);
        if (result != 0) return result;
        result = compare_reduction_pair(vector_reduce_min7, scalar_reduce_min7, x, 53);
        if (result != 0) return result;
        result = compare_reduction_pair(vector_reduce_max7, scalar_reduce_max7, x, 54);
        if (result != 0) return result;
    }
    if (float_bits(vector_reduce_add7(
            cases[0][0], cases[0][1], cases[0][2], cases[0][3],
            cases[0][4], cases[0][5], cases[0][6])) != UINT32_C(0x41c80000)) return 55;
    if (float_bits(vector_reduce_min7(
            cases[1][0], cases[1][1], cases[1][2], cases[1][3],
            cases[1][4], cases[1][5], cases[1][6])) != UINT32_C(0x00000000)) return 56;
    if (float_bits(vector_reduce_max7(
            cases[2][0], cases[2][1], cases[2][2], cases[2][3],
            cases[2][4], cases[2][5], cases[2][6])) != UINT32_C(0x80000000)) return 57;
    if (float_bits(vector_reduce_min7(
            cases[3][0], cases[3][1], cases[3][2], cases[3][3],
            cases[3][4], cases[3][5], cases[3][6])) != UINT32_C(0x7fc12345)) return 58;
    if (float_bits(vector_reduce_max7(
            cases[3][0], cases[3][1], cases[3][2], cases[3][3],
            cases[3][4], cases[3][5], cases[3][6])) != UINT32_C(0x7fc12345)) return 59;
    return 0;
}

int main(void) {
    int result = check_integer_semantics();
    if (result != 0) return result;
    result = check_shapes();
    if (result != 0) return result;
    result = check_float_arithmetic_and_comparisons();
    if (result != 0) return result;
    result = check_casts();
    if (result != 0) return result;
    return check_reductions();
}
"""


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"required {description} not found: {path}")
    return path


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required vector numeric semantics tool not found: {name}")
    return path


def run_checked(command: list[str], *, timeout: float = 120.0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def compiler_command(source: Path, output: Path, *options: str) -> list[str]:
    return [
        str(COMPILER),
        str(source),
        *options,
        f"-march={MARCH}",
        f"-mabi={MABI}",
        "-o",
        str(output),
    ]


def instruction_mnemonics(assembly: str) -> list[str]:
    result: list[str] = []
    for line in assembly.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((".", "#")) or stripped.endswith(":"):
            continue
        token = stripped.split(None, 1)[0].lower()
        if re.fullmatch(r"[a-z][a-z0-9_.]*", token):
            result.append(token)
    return result


def disassembly_mnemonics(disassembly: str) -> list[str]:
    result: list[str] = []
    pattern = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+([a-z][a-z0-9_.]*)\b")
    for line in disassembly.splitlines():
        match = pattern.match(line.lower())
        if match is not None:
            result.append(match.group(1))
    return result


def require_no_rvv(mnemonics: list[str], context: str) -> None:
    if not mnemonics:
        raise RuntimeError(f"{context} produced no recognizable instruction mnemonics")
    vector = sorted({mnemonic for mnemonic in mnemonics if mnemonic.startswith("v")})
    if vector:
        raise RuntimeError(f"{context} contains RVV mnemonics: {', '.join(vector)}")


def require_scalar_attributes(readelf: str, context: str) -> None:
    arch_lines = [line.lower() for line in readelf.splitlines() if "tag_riscv_arch" in line.lower()]
    if not arch_lines:
        raise RuntimeError(f"{context} lacks Tag_RISCV_arch")
    arch = " ".join(arch_lines)
    if "_v" in arch or "zve" in arch:
        raise RuntimeError(f"{context} unexpectedly advertises a vector ISA: {arch}")


def require_typed_numeric_oir(text: str) -> None:
    required = (
        "<1 x i32>",
        "<3 x i32>",
        "<7 x i32>",
        "<31 x i32>",
        "<7 x float>",
        "sdiv <7 x i32>",
        "srem <7 x i32>",
        "fadd <7 x float>",
        "fsub <7 x float>",
        "fmul <7 x float>",
        "fdiv <7 x float>",
        "fcmp eq <7 x float>",
        "fcmp ne <7 x float>",
        "fcmp lt <7 x float>",
        "fcmp le <7 x float>",
        "fcmp gt <7 x float>",
        "fcmp ge <7 x float>",
        "vector.sitofp <7 x i32>",
        "vector.fptosi <7 x float>",
        "vp.reduce.ordered.fadd <7 x float>",
        "vp.reduce.ordered.fmul <7 x float>",
        "vp.reduce.ordered.fmin <7 x float>",
        "vp.reduce.ordered.fmax <7 x float>",
    )
    missing = [spelling for spelling in required if spelling not in text]
    if missing:
        raise RuntimeError("typed OIR lacks numeric contracts: " + ", ".join(missing))


def require_scalar_mir(text: str) -> None:
    forbidden = (":vr", ":vmask", "fv<", "VSET", "RVV")
    present = [spelling for spelling in forbidden if spelling in text]
    if present:
        raise RuntimeError("portable numeric MIR retained vector state: " + ", ".join(present))
    for symbol in ("vector_iadd7", "vector_fdiv7", "vector_reduce_max7", "vector_shape_n31"):
        if f"func @{symbol}" not in text:
            raise RuntimeError(f"portable numeric MIR lacks executed kernel {symbol}")


def main() -> int:
    try:
        require_file(COMPILER, "release compiler")
        assembler = require_tool("riscv64-linux-gnu-as")
        gcc = require_tool("riscv64-linux-gnu-gcc")
        objdump = require_tool("riscv64-linux-gnu-objdump")
        readelf = require_tool("riscv64-linux-gnu-readelf")
        qemu = require_tool("qemu-riscv64")

        with tempfile.TemporaryDirectory(prefix="yoolang-vector-numeric-semantics-") as temp:
            directory = Path(temp)
            kernel_source = directory / "vector_numeric_kernels.sy"
            driver_source = directory / "vector_numeric_driver.c"
            kernel_source.write_text(textwrap.dedent(KERNEL_SOURCE), encoding="utf-8")
            driver_source.write_text(textwrap.dedent(DRIVER_SOURCE), encoding="utf-8")

            typed_oir = directory / "numeric.oir"
            run_checked(compiler_command(kernel_source, typed_oir, "--emit-oir", "-O0"))
            require_typed_numeric_oir(typed_oir.read_text(encoding="utf-8"))
            print("PASS vector_numeric_frontend_typed_oir")

            scalar_mir = directory / "numeric.mir"
            run_checked(compiler_command(kernel_source, scalar_mir, "--emit-mir", "-O0"))
            require_scalar_mir(scalar_mir.read_text(encoding="utf-8"))
            print("PASS vector_numeric_portable_scalar_mir")

            assembly = directory / "numeric.s"
            run_checked(compiler_command(kernel_source, assembly, "-S", "-O0"))
            assembly_text = assembly.read_text(encoding="utf-8")
            if '.attribute arch, "rv64gc"' not in assembly_text:
                raise RuntimeError("numeric Final ASM does not advertise rv64gc")
            require_no_rvv(instruction_mnemonics(assembly_text), "numeric Final ASM")

            kernel_object = directory / "numeric.o"
            run_checked(
                [
                    assembler,
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    str(assembly),
                    "-o",
                    str(kernel_object),
                ]
            )
            require_scalar_attributes(
                run_checked([readelf, "-A", str(kernel_object)]).stdout,
                "numeric GNU-as object",
            )
            print("PASS vector_numeric_final_asm_gnu_as_rv64gc_no_rvv")

            driver_object = directory / "driver.o"
            run_checked(
                [
                    gcc,
                    "-O2",
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    "-c",
                    str(driver_source),
                    "-o",
                    str(driver_object),
                ]
            )
            executable = directory / "vector-numeric-semantics"
            run_checked(
                [
                    gcc,
                    "-static",
                    f"-march={MARCH}",
                    f"-mabi={MABI}",
                    "-mcmodel=medany",
                    str(kernel_object),
                    str(driver_object),
                    "-o",
                    str(executable),
                ]
            )
            require_scalar_attributes(
                run_checked([readelf, "-A", str(executable)]).stdout,
                "numeric linked executable",
            )
            disassembly = run_checked([objdump, "-d", str(executable)]).stdout
            require_no_rvv(disassembly_mnemonics(disassembly), "numeric linked executable")
            for symbol in ("vector_iadd7", "vector_fcmp7", "vector_reduce_add7"):
                if f"<{symbol}>" not in disassembly:
                    raise RuntimeError(f"linked executable lacks numeric kernel {symbol}")
            run_checked([qemu, "-cpu", "rv64,v=false", str(executable)], timeout=60.0)
            print("PASS vector_numeric_qemu_rv64_v_false_bit_exact")
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL vector_numeric_semantics: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
