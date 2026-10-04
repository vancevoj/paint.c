/* pc_tile.c - tile allocation, cloning, reference counting and serials. */
#include "pc/pc_tile.h"

#include <stdlib.h>
#include <string.h>

/* Live accounting. Production: replace malloc for the header and the data
 * block with a slab allocator (2 MiB chunks) and drive history eviction
 * from these counters. */
static pc_atomic_u32 g_live_tiles;
static pc_atomic_u32 g_live_kib;     /* KiB granularity keeps it 32-bit */

/* ---- 64-bit serial counter ----------------------------------------------
 * pc_base.h only offers 32-bit atomics, and 32 bits of serials could wrap
 * within a long session (every transaction access draws one), so the
 * counter is 64-bit and kept private to this file. */
#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
static volatile __int64 g_serial;
static uint64_t serial_next(void)
{
#  if defined(_M_IX86)
    __int64 old, nv;
    do {
        old = g_serial;
        nv = old + 1;
    } while (_InterlockedCompareExchange64(&g_serial, nv, old) != old);
    return (uint64_t)nv;
#  else
    return (uint64_t)_InterlockedIncrement64(&g_serial);
#  endif
}
#else
#  include <stdatomic.h>
static _Atomic uint64_t g_serial;
static uint64_t serial_next(void)
{
    return atomic_fetch_add_explicit(&g_serial, 1u, memory_order_relaxed) + 1u;
}
#endif

uint64_t pc_tile_next_serial(void) { return serial_next(); }

/* ---- fault injection ------------------------------------------------------ */
static pc_atomic_u32 g_fault_on;
static pc_atomic_u32 g_fault_limit;
static pc_atomic_u32 g_fault_count;

void pc_fault_set(long n)
{
    if (n < 0) {
        pc_atomic_store(&g_fault_on, 0u);
        return;
    }
    pc_atomic_store(&g_fault_count, 0u);
    pc_atomic_store(&g_fault_limit, n > 0x7fffffffL ? 0x7fffffffu : (uint32_t)n);
    pc_atomic_store(&g_fault_on, 1u);
}

bool pc_fault_check(void)
{
    if (pc_atomic_load(&g_fault_on) == 0u) return false;
    return pc_atomic_inc(&g_fault_count) > pc_atomic_load(&g_fault_limit);
}

/* ---- tiles ------------------------------------------------------------------ */
size_t pc_tile_bytes(uint8_t bpp)
{
    return (size_t)PC_TILE_PX * (size_t)bpp;
}

static pc_tile *tile_alloc(uint8_t bpp)
{
    pc_tile *t;
    size_t bytes;
    if (bpp != 1u && bpp != 4u) return NULL;
    if (pc_fault_check()) return NULL;
    bytes = pc_tile_bytes(bpp);
    t = (pc_tile *)malloc(sizeof *t);
    if (!t) return NULL;
    t->data = (uint8_t *)pc_aligned_alloc(PC_TILE_ALIGN, bytes);
    if (!t->data) { free(t); return NULL; }
    pc_atomic_store(&t->refs, 1u);
    t->bpp = bpp;
    t->flags = 0u;
    t->reserved = 0u;
    t->serial = serial_next();
    (void)pc_atomic_inc(&g_live_tiles);
    (void)pc_atomic_add(&g_live_kib, (uint32_t)(bytes / 1024u));
    return t;
}

pc_tile *pc_tile_new_zero(uint8_t bpp)
{
    pc_tile *t = tile_alloc(bpp);
    if (t) memset(t->data, 0, pc_tile_bytes(bpp));
    return t;
}

pc_tile *pc_tile_clone(const pc_tile *src, uint8_t bpp)
{
    pc_tile *t;
    if (src && src->bpp != bpp) return NULL;
    t = tile_alloc(bpp);
    if (!t) return NULL;
    if (src) memcpy(t->data, src->data, pc_tile_bytes(bpp));
    else     memset(t->data, 0, pc_tile_bytes(bpp));
    return t;
}

pc_tile *pc_tile_new_fill(uint8_t bpp, const void *px, uint32_t w, uint32_t h)
{
    pc_tile *t = pc_tile_new_zero(bpp);
    if (!t || !px) return t;
    if (w > PC_TILE_DIM) w = PC_TILE_DIM;
    if (h > PC_TILE_DIM) h = PC_TILE_DIM;
    if (w == 0u || h == 0u) return t;
    for (uint32_t x = 0; x < w; x++)
        memcpy(t->data + (size_t)x * bpp, px, bpp);
    for (uint32_t y = 1; y < h; y++)
        memcpy(t->data + (size_t)y * PC_TILE_DIM * bpp, t->data, (size_t)w * bpp);
    return t;
}

void pc_tile_retain(pc_tile *t)
{
    if (t) (void)pc_atomic_inc(&t->refs);
}

void pc_tile_release(pc_tile *t)
{
    if (!t) return;
    if (pc_atomic_dec(&t->refs) == 0u) {
        size_t bytes = pc_tile_bytes(t->bpp);
        (void)pc_atomic_sub(&g_live_kib, (uint32_t)(bytes / 1024u));
        (void)pc_atomic_dec(&g_live_tiles);
        pc_aligned_free(t->data);
        free(t);
    }
}

uint32_t pc_tile_refs(pc_tile *t)
{
    return t ? pc_atomic_load(&t->refs) : 0u;
}

void pc_tile_stats(size_t *live_tiles, size_t *live_bytes)
{
    if (live_tiles) *live_tiles = pc_atomic_load(&g_live_tiles);
    if (live_bytes) *live_bytes = (size_t)pc_atomic_load(&g_live_kib) * 1024u;
}

uint64_t pc_tile_serial(const pc_tile *t)
{
    return t ? t->serial : 0u;
}

void pc_tile_touch(pc_tile *t)
{
    if (!t) return;
    PC_ASSERT(pc_atomic_load(&t->refs) == 1u);
    t->serial = serial_next();
}

bool pc_tile_is_zero(const pc_tile *t)
{
    const uint64_t *p;
    size_t n;
    if (!t) return true;
    p = (const uint64_t *)(const void *)t->data;   /* PC_TILE_ALIGN aligned */
    n = pc_tile_bytes(t->bpp) / sizeof *p;
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

bool pc_tile_equal(const pc_tile *a, const pc_tile *b)
{
    if (a == b) return true;
    if (!a) return pc_tile_is_zero(b);
    if (!b) return pc_tile_is_zero(a);
    if (a->bpp != b->bpp) return false;
    return memcmp(a->data, b->data, pc_tile_bytes(a->bpp)) == 0;
}
