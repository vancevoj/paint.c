/* pc_comp.c - flattening over tiles with the scalar oracle (any threads),
 * transaction substitution, overlay layers and cell signatures. */
#include "pc/pc_comp.h"

#include <string.h>

static bool eff_visible(const pc_comp_opts *o, const pc_layer *l)
{
    if (o && o->vis)
        for (uint32_t i = 0; i < o->n_vis; i++)
            if (o->vis[i].layer_id == l->id) return o->vis[i].visible;
    return l->visible;
}

/* Effective tile data of l at ti (NULL = transparent) and its version: the
 * transaction's private copy when touched, else the published tile. */
static const uint8_t *eff_tile(const pc_comp_opts *o, const pc_layer *l, size_t ti,
                               uint64_t *ver)
{
    const pc_tile *t;
    if (o && o->txn) {
        const uint8_t *data = NULL;
        uint64_t v = 0;
        if (pc_txn_lookup(o->txn, l->id, (uint32_t)ti, &data, &v)) {
            *ver = v;
            return data;
        }
    }
    t = l->grid[ti];
    *ver = pc_tile_serial(t);
    return t ? t->data : NULL;
}

static pc_status check_opts(const pc_doc *d, const pc_comp_opts *o)
{
    const pc_comp_overlay *ov;
    if (!o) return PC_OK;
    if (o->txn && pc_txn_doc(o->txn) != d) return PC_ERR_ARG;
    if (o->n_vis && !o->vis) return PC_ERR_ARG;
    ov = o->overlay;
    if (ov) {
        if (!ov->src || ov->src->tiles_x != d->tiles_x || ov->src->tiles_y != d->tiles_y)
            return PC_ERR_ARG;
        if ((unsigned)ov->mode >= (unsigned)PC_BLEND_COUNT) return PC_ERR_ARG;
        if (pc_doc_layer_index(d, ov->layer_id) < 0) return PC_ERR_ARG;
    }
    return PC_OK;
}

/* Composite cell (tx, ty) into acc (PC_TILE_PX). tmp is PC_TILE_PX scratch. */
static void comp_core(const pc_doc *d, const pc_comp_opts *o, uint32_t tx, uint32_t ty,
                      pc_px32 *acc, pc_px32 *tmp)
{
    size_t ti = (size_t)ty * d->tiles_x + tx;
    const pc_comp_overlay *ov = o ? o->overlay : NULL;
    memset(acc, 0, PC_TILE_PX * sizeof *acc);
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        uint64_t ver;
        bool vis = eff_visible(o, l);
        const uint8_t *data = eff_tile(o, l, ti, &ver);
        if (ov && ov->layer_id == l->id) {
            const pc_tile *ot = ov->src->grid[ti];
            bool ov_on = ot != NULL && ov->opacity != 0u;
            if (ov->into) {
                if (!vis || l->opacity == 0u) continue;
                if (ov_on) {
                    if (data) memcpy(tmp, data, PC_TILE_PX * sizeof *tmp);
                    else      memset(tmp, 0, PC_TILE_PX * sizeof *tmp);
                    pc_composite_span(tmp, (const pc_px32 *)(const void *)ot->data,
                                      PC_TILE_PX, ov->mode, ov->opacity);
                    data = (const uint8_t *)(const void *)tmp;
                }
                if (data)
                    pc_composite_span(acc, (const pc_px32 *)(const void *)data,
                                      PC_TILE_PX, l->mode, l->opacity);
                continue;
            }
            if (vis && l->opacity != 0u && data)
                pc_composite_span(acc, (const pc_px32 *)(const void *)data, PC_TILE_PX,
                                  l->mode, l->opacity);
            if (ov_on)
                pc_composite_span(acc, (const pc_px32 *)(const void *)ot->data, PC_TILE_PX,
                                  ov->mode, ov->opacity);
            continue;
        }
        if (!vis || l->opacity == 0u || !data) continue;   /* C-02: NULL is a no-op */
        pc_composite_span(acc, (const pc_px32 *)(const void *)data, PC_TILE_PX,
                          l->mode, l->opacity);
    }
    if (o && o->background.a != 0u) {
        uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
        uint32_t cw = d->w - x0 < PC_TILE_DIM ? d->w - x0 : PC_TILE_DIM;
        uint32_t ch = d->h - y0 < PC_TILE_DIM ? d->h - y0 : PC_TILE_DIM;
        for (size_t k = 0; k < PC_TILE_PX; k++) tmp[k] = o->background;
        pc_composite_span(tmp, acc, PC_TILE_PX, PC_BLEND_NORMAL, 255u);
        memcpy(acc, tmp, PC_TILE_PX * sizeof *acc);
        if (cw < PC_TILE_DIM || ch < PC_TILE_DIM)       /* INV-TILE-EDGE */
            for (uint32_t y = 0; y < PC_TILE_DIM; y++)
                for (uint32_t x = (y < ch ? cw : 0u); x < PC_TILE_DIM; x++)
                    memset(&acc[(size_t)y * PC_TILE_DIM + x], 0, sizeof *acc);
    }
}

pc_status pc_comp_tile(const pc_doc *d, uint32_t tx, uint32_t ty, pc_px32 *out,
                       const pc_comp_opts *o)
{
    pc_px32 tmp[PC_TILE_PX];
    pc_status st;
    if (!d || !out || tx >= d->tiles_x || ty >= d->tiles_y) return PC_ERR_ARG;
    st = check_opts(d, o);
    if (st != PC_OK) return st;
    comp_core(d, o, tx, ty, out, tmp);
    return PC_OK;
}

typedef struct comp_job {
    const pc_doc       *d;
    const pc_comp_opts *o;
    pc_rect             r;       /* requested rect, clipped to the doc */
    pc_rect             req;     /* original rect (dst origin) */
    int32_t             tx0, ty0, ntx;
    pc_px32            *dst;
    size_t              stride;
} comp_job;

static void comp_job_tile(void *ud, uint32_t index, uint32_t worker)
{
    const comp_job *j = (const comp_job *)ud;
    pc_px32 acc[PC_TILE_PX], tmp[PC_TILE_PX];
    int32_t tx = j->tx0 + (int32_t)(index % (uint32_t)j->ntx);
    int32_t ty = j->ty0 + (int32_t)(index / (uint32_t)j->ntx);
    pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                              (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
    pc_rect s = pc_rect_intersect(tr, j->r);
    (void)worker;
    if (pc_rect_is_empty(s)) return;
    comp_core(j->d, j->o, (uint32_t)tx, (uint32_t)ty, acc, tmp);
    for (int32_t y = s.y; y < s.y + s.h; y++)
        memcpy(j->dst + (size_t)(y - j->req.y) * j->stride + (size_t)(s.x - j->req.x),
               acc + (size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x),
               (size_t)s.w * sizeof(pc_px32));
}

pc_status pc_comp_rect_ex(const pc_doc *d, pc_rect r, pc_px32 *dst,
                          size_t dst_stride, const pc_comp_opts *o)
{
    comp_job j;
    int32_t tx1, ty1;
    uint32_t count;
    pc_status st;
    if (!d || !dst || (size_t)(r.w > 0 ? r.w : 0) > dst_stride) return PC_ERR_ARG;
    st = check_opts(d, o);
    if (st != PC_OK) return st;
    if (pc_rect_is_empty(r)) return PC_OK;
    for (int32_t y = 0; y < r.h; y++)
        memset(dst + (size_t)y * dst_stride, 0, (size_t)r.w * sizeof(pc_px32));
    j.d = d; j.o = o; j.req = r; j.dst = dst; j.stride = dst_stride;
    j.r = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(j.r)) return PC_OK;
    j.tx0 = j.r.x >> PC_TILE_SHIFT; tx1 = (j.r.x + j.r.w - 1) >> PC_TILE_SHIFT;
    j.ty0 = j.r.y >> PC_TILE_SHIFT; ty1 = (j.r.y + j.r.h - 1) >> PC_TILE_SHIFT;
    j.ntx = tx1 - j.tx0 + 1;
    count = (uint32_t)j.ntx * (uint32_t)(ty1 - j.ty0 + 1);
    pc_par_for(o ? o->par : NULL, comp_job_tile, &j, count);
    return PC_OK;
}

pc_status pc_comp_rect(const pc_doc *d, pc_rect r, pc_px32 *dst,
                       size_t dst_stride, const pc_par *par)
{
    pc_comp_opts o = pc_comp_opts_default();
    o.par = par;
    return pc_comp_rect_ex(d, r, dst, dst_stride, &o);
}

/* ---- signatures ------------------------------------------------------------- */
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static uint64_t hstep(uint64_t h, uint64_t v)
{
    return mix64((h * 0x100000001b3ull) ^ (v + 0x9e3779b97f4a7c15ull));
}

static uint64_t props_word(uint32_t tag, pc_blend_mode m, uint8_t opacity)
{
    return ((uint64_t)tag << 32) | ((uint64_t)(uint32_t)m << 8) | opacity;
}

uint64_t pc_comp_tile_sig(const pc_doc *d, uint32_t tx, uint32_t ty,
                          const pc_comp_opts *o)
{
    uint64_t h = 0x6a09e667f3bcc909ull;
    const pc_comp_overlay *ov;
    size_t ti;
    if (!d) return 1u;
    h = hstep(h, ((uint64_t)d->w << 32) | d->h);
    if (tx >= d->tiles_x || ty >= d->tiles_y || check_opts(d, o) != PC_OK)
        return hstep(h, 0xdeadull) | 1u;
    ti = (size_t)ty * d->tiles_x + tx;
    ov = o ? o->overlay : NULL;
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        uint64_t ver = 0;
        bool vis = eff_visible(o, l);
        const uint8_t *data = eff_tile(o, l, ti, &ver);
        if (ov && ov->layer_id == l->id) {
            const pc_tile *ot = ov->src->grid[ti];
            bool ov_on = ot != NULL && ov->opacity != 0u;
            if (ov->into) {
                if (!vis || l->opacity == 0u) continue;
                if (ov_on) {
                    h = hstep(h, props_word(3u, ov->mode, ov->opacity));
                    h = hstep(h, ot->serial);
                    h = hstep(h, ov->version);
                    h = hstep(h, data ? ver : 0u);
                    h = hstep(h, props_word(4u, l->mode, l->opacity));
                } else if (data) {
                    h = hstep(h, props_word(1u, l->mode, l->opacity));
                    h = hstep(h, ver);
                }
                continue;
            }
            if (vis && l->opacity != 0u && data) {
                h = hstep(h, props_word(1u, l->mode, l->opacity));
                h = hstep(h, ver);
            }
            if (ov_on) {
                h = hstep(h, props_word(2u, ov->mode, ov->opacity));
                h = hstep(h, ot->serial);
                h = hstep(h, ov->version);
            }
            continue;
        }
        if (!vis || l->opacity == 0u || !data) continue;
        h = hstep(h, props_word(1u, l->mode, l->opacity));
        h = hstep(h, ver);
    }
    if (o && o->background.a != 0u) {
        uint32_t c = (uint32_t)o->background.b | ((uint32_t)o->background.g << 8) |
                     ((uint32_t)o->background.r << 16) | ((uint32_t)o->background.a << 24);
        h = hstep(h, 0xb6000000000ull | c);
    }
    return h ? h : 1u;
}
