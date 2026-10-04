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
    /* Appended in W1-L1b. Process-wide unique, never 0, drawn from one
     * atomic counter at creation and redrawn by pc_tile_touch. A tile is
     * identified for caching by (pointer, serial): a freed and reused
     * pointer always comes back with a new serial, so there is no ABA. */
    uint64_t serial;
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

/* ---- W1-L1b additions (thread rules: any thread unless noted) ---------- */

/* Serial of t, 0 for NULL (borrowed). */
uint64_t pc_tile_serial(const pc_tile *t);

/* A fresh process-wide serial (never 0, strictly increasing). The same
 * counter feeds tile serials and transaction versions (pc_txn.h), so
 * values from both never collide. */
uint64_t pc_tile_next_serial(void);

/* Give a PRIVATE tile (refs == 1, asserted) a new serial after its pixels
 * were written in place, so caches keyed by serial see the change. Only
 * the thread that owns the private tile may call it. */
void     pc_tile_touch(pc_tile *t);

/* True when every byte of t is zero. NULL counts as zero (borrowed). */
bool     pc_tile_is_zero(const pc_tile *t);

/* Content equality; NULL equals an all-zero tile. Tiles of different bpp
 * are never equal (borrowed). */
bool     pc_tile_equal(const pc_tile *a, const pc_tile *b);

/* New tile (refs == 1) whose pixels in [0, w) x [0, h) (tile-local, w and
 * h clamped to PC_TILE_DIM) all equal the bpp bytes at px; the rest is
 * zero, which keeps INV-TILE-EDGE for edge tiles. NULL on OOM / bad bpp. */
pc_tile *pc_tile_new_fill(uint8_t bpp, const void *px, uint32_t w, uint32_t h);

/* ---- allocation fault injection (tests only) --------------------------- */
/* pc_fault_set(n) with n >= 0 lets the next n guarded allocations succeed
 * and fails every later one, until pc_fault_set(-1) disables injection
 * (the default). Guarded allocations: every tile allocation plus the
 * grid, scratch, payload and node allocations of pc_txn, pc_comp, pc_mip,
 * pc_resample, pc_geom and pc_layerops. pc_fault_check is what those
 * allocators call: true means "behave as if malloc returned NULL". The
 * counter is atomic, so the hook works under pc_par, but which parallel
 * allocation fails first is then not deterministic. */
void     pc_fault_set(long n);
bool     pc_fault_check(void);

#endif /* PC_TILE_H */
