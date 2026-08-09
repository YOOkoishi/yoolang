#ifndef YOOLANG_RVV_RUNTIME_H
#define YOOLANG_RVV_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return non-zero only when the calling Linux thread may execute RVV code.
 *
 * The definition supplied by the yoolang runtime is weak.  This declaration
 * intentionally is not: an application may provide a normal strong
 * definition when testing a dispatcher or integrating with another runtime.
 */
int __yoolang_rvv_available(void);

#ifdef __cplusplus
}
#endif

#endif
