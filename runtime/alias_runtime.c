#include "alias_runtime.h"

#include <limits.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define YOOLANG_CONST_NOTHROW __attribute__((const, nothrow))
#else
#define YOOLANG_CONST_NOTHROW
#endif

struct yoolang_byte_range {
    uintptr_t begin;
    uintptr_t end;
};

static int checked_mul_uintptr(uintptr_t lhs, uintptr_t rhs, uintptr_t *result) {
    if (lhs != 0 && rhs > UINTPTR_MAX / lhs)
        return 0;
    *result = lhs * rhs;
    return 1;
}

static int checked_add_uintptr(uintptr_t lhs, uintptr_t rhs, uintptr_t *result) {
    if (rhs > UINTPTR_MAX - lhs)
        return 0;
    *result = lhs + rhs;
    return 1;
}

static int describe_range(uintptr_t base, int32_t count, int32_t stride_elements,
                          int32_t element_bytes, struct yoolang_byte_range *range) {
    uintptr_t stride_magnitude;
    uintptr_t iteration_span;
    uintptr_t byte_span;

    if (count <= 0 || element_bytes <= 0 || stride_elements == INT32_MIN)
        return 0;

    stride_magnitude =
        stride_elements < 0 ? (uintptr_t)(-stride_elements) : (uintptr_t)stride_elements;
    if (!checked_mul_uintptr((uintptr_t)(count - 1), stride_magnitude, &iteration_span) ||
        !checked_mul_uintptr(iteration_span, (uintptr_t)element_bytes, &byte_span))
        return 0;

    if (stride_elements < 0) {
        if (byte_span > base || !checked_add_uintptr(base, (uintptr_t)element_bytes, &range->end))
            return 0;
        range->begin = base - byte_span;
        return 1;
    }

    range->begin = base;
    if (!checked_add_uintptr(base, byte_span, &range->end) ||
        !checked_add_uintptr(range->end, (uintptr_t)element_bytes, &range->end))
        return 0;
    return 1;
}

YOOLANG_CONST_NOTHROW int __yoolang_uintptr_ranges_disjoint(
    uintptr_t lhs_base, int32_t lhs_count, int32_t lhs_stride_elements, int32_t lhs_element_bytes,
    uintptr_t rhs_base, int32_t rhs_count, int32_t rhs_stride_elements, int32_t rhs_element_bytes) {
    struct yoolang_byte_range lhs;
    struct yoolang_byte_range rhs;

    /* A zero-trip stream is empty, independent of its otherwise-unused base. */
    if (lhs_count == 0 || rhs_count == 0)
        return 1;
    if (!describe_range(lhs_base, lhs_count, lhs_stride_elements, lhs_element_bytes, &lhs) ||
        !describe_range(rhs_base, rhs_count, rhs_stride_elements, rhs_element_bytes, &rhs))
        return 0;
    return lhs.end <= rhs.begin || rhs.end <= lhs.begin;
}

YOOLANG_CONST_NOTHROW int __yoolang_ranges_disjoint(const void *lhs_base, int32_t lhs_count,
                                                    int32_t lhs_stride_elements,
                                                    int32_t lhs_element_bytes, const void *rhs_base,
                                                    int32_t rhs_count, int32_t rhs_stride_elements,
                                                    int32_t rhs_element_bytes) {
    return __yoolang_uintptr_ranges_disjoint((uintptr_t)lhs_base, lhs_count, lhs_stride_elements,
                                             lhs_element_bytes, (uintptr_t)rhs_base, rhs_count,
                                             rhs_stride_elements, rhs_element_bytes);
}
