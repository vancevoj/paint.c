/* pc_tile.c - tile allocation, cloning and reference counting. */
#include "pc/pc_tile.h"

#include <stdlib.h>
#include <string.h>

/* Live accounting. Production: replace malloc for the header and the data
 * block with a slab allocator (2 MiB chunks) and drive history eviction
 * from these counters. */
static pc_atomic_u32 g_live_tiles;
static pc_atomic_u32 g_live_kib;     /* KiB granularity keeps it 32-bit */

size_t pc_tile_bytes(uint8_t bpp)
{
    return (size_t)PC_TILE_PX * (size_t)bpp;
}

static pc_tile *tile_alloc(uint8_t bpp)
{
    pc_tile *t;
    size_t bytes;
    if (bpp != 1u && bpp != 4u) return NULL;
    bytes = pc_tile_bytes(bpp);
    t = (pc_tile *)malloc(sizeof *t);
    if (!t) return NULL;
    t->data = (uint8_t *)pc_aligned_alloc(PC_TILE_ALIGN, bytes);
    if (!t->data) { free(t); return NULL; }
    pc_atomic_store(&t->refs, 1u);
    t->bpp = bpp;
    t->flags = 0u;
    t->reserved = 0u;
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
