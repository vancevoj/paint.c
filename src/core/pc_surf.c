/* pc_surf.c - rectangles, surfaces, masks and layer transfers. */
#include "pc/pc_surf.h"

#include <stdlib.h>
#include <string.h>

/* ---- rectangles ---------------------------------------------------------- */
static int64_t imin64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t imax64(int64_t a, int64_t b) { return a > b ? a : b; }

pc_rect pc_rect_intersect(pc_rect a, pc_rect b)
{
    int64_t x0 = imax64(a.x, b.x), y0 = imax64(a.y, b.y);
    int64_t x1 = imin64((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    int64_t y1 = imin64((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    if (pc_rect_is_empty(a) || pc_rect_is_empty(b) || x1 <= x0 || y1 <= y0)
        return pc_rect_make(0, 0, 0, 0);
    return pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

pc_rect pc_rect_union(pc_rect a, pc_rect b)
{
    int64_t x0, y0, x1, y1;
    if (pc_rect_is_empty(a)) return pc_rect_is_empty(b) ? pc_rect_make(0, 0, 0, 0) : b;
    if (pc_rect_is_empty(b)) return a;
    x0 = imin64(a.x, b.x); y0 = imin64(a.y, b.y);
    x1 = imax64((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    y1 = imax64((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    if (x1 - x0 > INT32_MAX) x1 = x0 + INT32_MAX;
    if (y1 - y0 > INT32_MAX) y1 = y0 + INT32_MAX;
    return pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

bool pc_rect_contains(pc_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && (int64_t)x < (int64_t)r.x + r.w &&
           (int64_t)y < (int64_t)r.y + r.h;
}

/* ---- surfaces and masks -------------------------------------------------- */
pc_status pc_surf_alloc(pc_surf *s, int32_t w, int32_t h)
{
    size_t n, bytes;
    memset(s, 0, sizeof *s);
    if (w <= 0 || h <= 0 || (uint32_t)w > PC_MAX_DIM || (uint32_t)h > PC_MAX_DIM)
        return PC_ERR_ARG;
    if (!pc_mul_size((size_t)w, (size_t)h, &n) || !pc_mul_size(n, sizeof(pc_px32), &bytes))
        return PC_ERR_LIMIT;
    s->px = (pc_px32 *)calloc(n, sizeof(pc_px32));
    if (!s->px) return PC_ERR_NOMEM;
    (void)bytes;
    s->w = w; s->h = h; s->stride = w;
    return PC_OK;
}

void pc_surf_free(pc_surf *s)
{
    if (!s) return;
    free(s->px);
    memset(s, 0, sizeof *s);
}

pc_status pc_mask_alloc(pc_mask *m, pc_rect r)
{
    size_t n;
    memset(m, 0, sizeof *m);
    if (r.w <= 0 || r.h <= 0 || (uint32_t)r.w > PC_MAX_DIM || (uint32_t)r.h > PC_MAX_DIM)
        return PC_ERR_ARG;
    if (!pc_mul_size((size_t)r.w, (size_t)r.h, &n)) return PC_ERR_LIMIT;
    m->px = (uint8_t *)calloc(n, 1u);
    if (!m->px) return PC_ERR_NOMEM;
    m->x = r.x; m->y = r.y; m->w = r.w; m->h = r.h; m->stride = r.w;
    return PC_OK;
}

void pc_mask_free(pc_mask *m)
{
    if (!m) return;
    free(m->px);
    memset(m, 0, sizeof *m);
}

uint8_t pc_mask_at(const pc_mask *m, int32_t x, int32_t y)
{
    int64_t dx = (int64_t)x - m->x, dy = (int64_t)y - m->y;
    if (!m->px || dx < 0 || dy < 0 || dx >= m->w || dy >= m->h) return 0u;
    return m->px[(size_t)dy * (size_t)m->stride + (size_t)dx];
}

/* ---- layer transfers ------------------------------------------------------ */
void pc_layer_read_rect(const pc_doc *d, const pc_layer *l, pc_rect r,
                        pc_px32 *dst, size_t dst_stride)
{
    pc_rect c;
    int32_t ty0, ty1, tx0, tx1;
    if (pc_rect_is_empty(r)) return;
    for (int32_t y = 0; y < r.h; y++)
        memset(dst + (size_t)y * dst_stride, 0, (size_t)r.w * sizeof(pc_px32));
    c = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(c)) return;
    PC_ASSERT(l->tiles_x == d->tiles_x && l->tiles_y == d->tiles_y);
    tx0 = c.x >> PC_TILE_SHIFT; tx1 = (c.x + c.w - 1) >> PC_TILE_SHIFT;
    ty0 = c.y >> PC_TILE_SHIFT; ty1 = (c.y + c.h - 1) >> PC_TILE_SHIFT;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            const pc_tile *t = l->grid[(size_t)ty * l->tiles_x + (size_t)tx];
            pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                                      (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
            pc_rect s = pc_rect_intersect(tr, c);
            if (!t || pc_rect_is_empty(s)) continue;
            for (int32_t y = s.y; y < s.y + s.h; y++) {
                const pc_px32 *srow = (const pc_px32 *)t->data +
                    (size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x);
                memcpy(dst + (size_t)(y - r.y) * dst_stride + (size_t)(s.x - r.x), srow,
                       (size_t)s.w * sizeof(pc_px32));
            }
        }
    }
}

static bool tile_is_zero(const pc_tile *t)
{
    const uint32_t *p = (const uint32_t *)(const void *)t->data;
    for (size_t i = 0; i < PC_TILE_PX; i++)
        if (p[i]) return false;
    return true;
}

pc_status pc_layer_store_rect(const pc_doc *d, pc_layer *l, pc_rect r,
                              const pc_px32 *src, size_t src_stride)
{
    pc_rect c = pc_rect_intersect(r, pc_doc_rect(d));
    int32_t ty0, ty1, tx0, tx1;
    if (pc_rect_is_empty(c)) return PC_OK;
    PC_ASSERT(l->tiles_x == d->tiles_x && l->tiles_y == d->tiles_y);
    tx0 = c.x >> PC_TILE_SHIFT; tx1 = (c.x + c.w - 1) >> PC_TILE_SHIFT;
    ty0 = c.y >> PC_TILE_SHIFT; ty1 = (c.y + c.h - 1) >> PC_TILE_SHIFT;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            pc_tile **slot = &l->grid[(size_t)ty * l->tiles_x + (size_t)tx];
            pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                                      (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
            pc_rect s = pc_rect_intersect(tr, c);
            bool any = false;
            if (pc_rect_is_empty(s)) continue;
            if (!*slot) {   /* skip allocation when the incoming pixels are all zero */
                for (int32_t y = s.y; y < s.y + s.h && !any; y++) {
                    const pc_px32 *row = src + (size_t)(y - r.y) * src_stride + (size_t)(s.x - r.x);
                    for (int32_t x = 0; x < s.w; x++)
                        if (row[x].a | row[x].r | row[x].g | row[x].b) { any = true; break; }
                }
                if (!any) continue;
                *slot = pc_tile_new_zero(4u);
                if (!*slot) return PC_ERR_NOMEM;
            }
            PC_ASSERT(pc_tile_refs(*slot) == 1u && (*slot)->bpp == 4u);
            for (int32_t y = s.y; y < s.y + s.h; y++) {
                pc_px32 *drow = (pc_px32 *)(void *)(*slot)->data +
                    (size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x);
                memcpy(drow, src + (size_t)(y - r.y) * src_stride + (size_t)(s.x - r.x),
                       (size_t)s.w * sizeof(pc_px32));
            }
            if (tile_is_zero(*slot)) { pc_tile_release(*slot); *slot = NULL; }
        }
    }
    l->gen++;
    return PC_OK;
}
