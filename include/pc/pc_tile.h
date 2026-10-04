/* pc_tile.h - fixed-size refcounted pixel tiles.
 *
 * Ownership model (INV-TILE-*):
 *  - Every reference to a tile is an owning reference counted in refs.
 *  - A tile reachable from a layer grid or a history payload is PUBLISHED
 *    and must never be written again. Writers obtain a private copy via a
 *    transaction (pc_txn), mutate it, and publish it on commit.
 *  - NULL in a grid means "fully transparent"; no tile is allocated.
 *  - Pixels of edge tiles that fall outside the document are kept zero.
 */
#ifndef PC_TILE_H
#define PC_TILE_H

#include "pc_base.h"

#define PC_TILE_SHIFT 6u
#define PC_TILE_DIM   64u                       /* pixels per side */
#define PC_TILE_PX    (PC_TILE_DIM * PC_TILE_DIM)
#define PC_TILE_ALIGN 64u                       /* cache line / AVX-512 */

typedef struct pc_tile {
    pc_atomic_u32 refs;
    uint8_t  bpp;        /* bytes per pixel: 4 = BGRA8, 1 = A8 mask */
    uint8_t  flags;      /* reserved for UNIFORM / PACKED / SWAPPED states */
    uint16_t reserved;
    uint8_t *data;       /* PC_TILE_PX * bpp bytes, PC_TILE_ALIGN aligned */
} pc_tile;

/* New zero-filled tile with refs == 1, or NULL on OOM / bad bpp. */
pc_tile *pc_tile_new_zero(uint8_t bpp);

/* New private copy of src (src may be NULL = transparent), refs == 1. */
pc_tile *pc_tile_clone(const pc_tile *src, uint8_t bpp);

void     pc_tile_retain(pc_tile *t);            /* NULL-safe */
void     pc_tile_release(pc_tile *t);           /* NULL-safe, frees at 0 */
uint32_t pc_tile_refs(pc_tile *t);
size_t   pc_tile_bytes(uint8_t bpp);

/* Process-wide live counters (for leak tests and memory budgets). */
void     pc_tile_stats(size_t *live_tiles, size_t *live_bytes);

#endif /* PC_TILE_H */
