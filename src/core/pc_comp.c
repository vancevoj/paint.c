/* pc_comp.c - reference flattening over tiles (scalar oracle, any threads). */
#include "pc/pc_comp.h"

#include <string.h>

typedef struct comp_job {
    const pc_doc *d;
    pc_rect       r;       /* requested rect, clipped to the doc */
    pc_rect       req;     /* original rect (dst origin) */
    int32_t       tx0, ty0, ntx;
    pc_px32      *dst;
    size_t        stride;
} comp_job;

static void comp_tile(void *ud, uint32_t index, uint32_t worker)
{
    const comp_job *j = (const comp_job *)ud;
    pc_px32 acc[PC_TILE_PX];
    int32_t tx = j->tx0 + (int32_t)(index % (uint32_t)j->ntx);
    int32_t ty = j->ty0 + (int32_t)(index / (uint32_t)j->ntx);
    size_t  ti = (size_t)ty * j->d->tiles_x + (size_t)tx;
    pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                              (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
    pc_rect s = pc_rect_intersect(tr, j->r);
    (void)worker;
    if (pc_rect_is_empty(s)) return;
    memset(acc, 0, sizeof acc);
    for (uint32_t i = 0; i < j->d->n_layers; i++) {
        const pc_layer *l = j->d->stack[i];
        const pc_tile *t;
        if (!l->visible || l->opacity == 0u) continue;
        t = l->grid[ti];
        if (!t) continue;                  /* transparent source: no-op (C-02) */
        pc_composite_span(acc, (const pc_px32 *)(const void *)t->data, PC_TILE_PX,
                          l->mode, l->opacity);
    }
    for (int32_t y = s.y; y < s.y + s.h; y++)
        memcpy(j->dst + (size_t)(y - j->req.y) * j->stride + (size_t)(s.x - j->req.x),
               acc + (size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x),
               (size_t)s.w * sizeof(pc_px32));
}

pc_status pc_comp_rect(const pc_doc *d, pc_rect r, pc_px32 *dst,
                       size_t dst_stride, const pc_par *par)
{
    comp_job j;
    int32_t tx1, ty1;
    uint32_t count;
    if (!d || !dst || (size_t)(r.w > 0 ? r.w : 0) > dst_stride) return PC_ERR_ARG;
    if (pc_rect_is_empty(r)) return PC_OK;
    for (int32_t y = 0; y < r.h; y++)
        memset(dst + (size_t)y * dst_stride, 0, (size_t)r.w * sizeof(pc_px32));
    j.d = d; j.req = r; j.dst = dst; j.stride = dst_stride;
    j.r = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(j.r)) return PC_OK;
    j.tx0 = j.r.x >> PC_TILE_SHIFT; tx1 = (j.r.x + j.r.w - 1) >> PC_TILE_SHIFT;
    j.ty0 = j.r.y >> PC_TILE_SHIFT; ty1 = (j.r.y + j.r.h - 1) >> PC_TILE_SHIFT;
    j.ntx = tx1 - j.tx0 + 1;
    count = (uint32_t)j.ntx * (uint32_t)(ty1 - j.ty0 + 1);
    pc_par_for(par, comp_tile, &j, count);
    return PC_OK;
}
