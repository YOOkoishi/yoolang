#ifndef YOOLANG_ALIAS_RUNTIME_H
#define YOOLANG_ALIAS_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return one only when the complete half-open byte ranges touched by the two
 * affine streams are representable in uintptr_t and are disjoint.  The
 * implementation never dereferences either address.  An invalid or
 * overflowing description fails closed and returns zero.
 */
int __yoolang_uintptr_ranges_disjoint(uintptr_t lhs_base, int32_t lhs_count,
                                      int32_t lhs_stride_elements, int32_t lhs_element_bytes,
                                      uintptr_t rhs_base, int32_t rhs_count,
                                      int32_t rhs_stride_elements, int32_t rhs_element_bytes);

int __yoolang_ranges_disjoint(const void *lhs_base, int32_t lhs_count, int32_t lhs_stride_elements,
                              int32_t lhs_element_bytes, const void *rhs_base, int32_t rhs_count,
                              int32_t rhs_stride_elements, int32_t rhs_element_bytes);

#ifdef __cplusplus
}
#endif

#endif
