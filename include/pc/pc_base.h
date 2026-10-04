/* pc_base.h - common definitions for the portable-canvas core.
 * Language: C17. Includes only freestanding/libc headers, never OS headers.
 * Status: reference implementation, verified on GCC 13 (Linux x86-64).
 */
#ifndef PC_BASE_H
#define PC_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- status codes ------------------------------------------------------ */
typedef enum pc_status {
    PC_OK          = 0,
    PC_ERR_NOMEM   = -1,  /* allocation failed; state left unchanged */
    PC_ERR_ARG     = -2,  /* invalid argument */
    PC_ERR_STATE   = -3,  /* operation not valid in current state */
    PC_ERR_LIMIT   = -4,  /* a hard limit (dimension, count) was exceeded */
    PC_ERR_FORMAT  = -5,  /* malformed or corrupt input data */
    PC_ERR_UNSUPPORTED = -6, /* valid input using an unsupported feature */
    PC_ERR_IO      = -7,  /* file system or device error (pal layer) */
    PC_ERR_CANCELLED = -8 /* the operation was cancelled; state unchanged */
} pc_status;

/* Short English description of a status code. Never NULL. Any thread. */
const char *pc_status_str(pc_status s);

/* ---- hard limits --------------------------------------------------------- */
#define PC_MAX_DIM 65535u            /* per-side pixel limit for documents */

/* ---- assertions (kept in release builds unless PC_NO_ASSERT) -------------- */
void pc_panic(const char *file, int line, const char *expr);
#if defined(PC_NO_ASSERT)
#  define PC_ASSERT(c) ((void)0)
#else
#  define PC_ASSERT(c) ((c) ? (void)0 : pc_panic(__FILE__, __LINE__, #c))
#endif

/* ---- checked size arithmetic: return false on overflow ------------------- */
static inline bool pc_mul_size(size_t a, size_t b, size_t *out)
{
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
#endif
}

static inline bool pc_add_size(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

/* ---- 32-bit atomic counter ------------------------------------------------ */
#if defined(_MSC_VER) && !defined(__clang__)
/* MSVC path: C11 <stdatomic.h> needs /std:c17 /experimental:c11atomics
 * (VS 2022 17.5+). Interlocked intrinsics are used instead.
 * UNTESTED: no MSVC was available when this file was written. */
#  include <intrin.h>
typedef struct pc_atomic_u32 { volatile long v; } pc_atomic_u32;
static inline uint32_t pc_atomic_load(pc_atomic_u32 *a)
{ return (uint32_t)_InterlockedOr(&a->v, 0); }
static inline void pc_atomic_store(pc_atomic_u32 *a, uint32_t x)
{ (void)_InterlockedExchange(&a->v, (long)x); }
static inline uint32_t pc_atomic_inc(pc_atomic_u32 *a)
{ return (uint32_t)_InterlockedIncrement(&a->v); }
static inline uint32_t pc_atomic_dec(pc_atomic_u32 *a)
{ return (uint32_t)_InterlockedDecrement(&a->v); }
static inline uint32_t pc_atomic_add(pc_atomic_u32 *a, uint32_t x)
{ return (uint32_t)_InterlockedExchangeAdd(&a->v, (long)x) + x; }
static inline uint32_t pc_atomic_sub(pc_atomic_u32 *a, uint32_t x)
{ return (uint32_t)_InterlockedExchangeAdd(&a->v, -(long)x) - x; }
#else
#  include <stdatomic.h>
typedef struct pc_atomic_u32 { _Atomic uint32_t v; } pc_atomic_u32;
static inline uint32_t pc_atomic_load(pc_atomic_u32 *a)
{ return atomic_load_explicit(&a->v, memory_order_acquire); }
static inline void pc_atomic_store(pc_atomic_u32 *a, uint32_t x)
{ atomic_store_explicit(&a->v, x, memory_order_release); }
static inline uint32_t pc_atomic_inc(pc_atomic_u32 *a)
{ return atomic_fetch_add_explicit(&a->v, 1u, memory_order_relaxed) + 1u; }
static inline uint32_t pc_atomic_dec(pc_atomic_u32 *a)
{ return atomic_fetch_sub_explicit(&a->v, 1u, memory_order_acq_rel) - 1u; }
static inline uint32_t pc_atomic_add(pc_atomic_u32 *a, uint32_t x)
{ return atomic_fetch_add_explicit(&a->v, x, memory_order_relaxed) + x; }
static inline uint32_t pc_atomic_sub(pc_atomic_u32 *a, uint32_t x)
{ return atomic_fetch_sub_explicit(&a->v, x, memory_order_relaxed) - x; }
#endif

/* ---- aligned allocation (size is rounded up to a multiple of align) ------- */
void *pc_aligned_alloc(size_t align, size_t size);
void  pc_aligned_free(void *p);

#endif /* PC_BASE_H */
