/* pc_mem.c - aligned allocation and panic. libc only (no OS headers). */
#include "pc/pc_base.h"

#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER) || defined(_WIN32)
#  include <malloc.h>   /* _aligned_malloc / _aligned_free (CRT, not Win32) */
#endif

void pc_panic(const char *file, int line, const char *expr)
{
    fprintf(stderr, "PC_ASSERT failed: %s (%s:%d)\n", expr, file, line);
    fflush(stderr);
    abort();
}

void *pc_aligned_alloc(size_t align, size_t size)
{
    size_t rounded;
    if (align == 0u || (align & (align - 1u)) != 0u) return NULL;
    if (!pc_add_size(size, align - 1u, &rounded)) return NULL;
    rounded &= ~(align - 1u);
    if (rounded == 0u) rounded = align;
#if defined(_WIN32)
    /* MSVC and MinGW CRTs lack C11 aligned_alloc; free() cannot release
     * these blocks, so pc_aligned_free must be used. */
    return _aligned_malloc(rounded, align);
#else
    return aligned_alloc(align, rounded);
#endif
}

void pc_aligned_free(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}
