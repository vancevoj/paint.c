/* pc_geom.c - image geometry history operations and the grid remapper. */
#include "pc_geom_int.h"

#include <stdlib.h>
#include <string.h>

static void *gm_calloc(size_t n, size_t sz)
{
    if (pc_fault_check()) return NULL;
    return calloc(n ? n : 1u, sz ? sz : 1u);
}

pc_hist_node *pc_gm_node_new(const char *label)
{
    if (pc_fault_check()) return NULL;
    return pc_hist_node_new(label);
}

/* ---- maps ------------------------------------------------------------------- */
pc_gm_map pc_gm_translate(int32_t dx, int32_t dy)
{
    pc_gm_map m = {1, 0, 0, 0, 1, 0};
    m.cx = -(int64_t)dx;
    m.cy = -(int64_t)dy;
    return m;
}

pc_gm_map pc_gm_flip(uint32_t w, uint32_t h, bool horizontal)
{
    pc_gm_map m = {1, 0, 0, 0, 1, 0};
    if (horizontal) { m.ax = -1; m.cx = (int64_t)w - 1; }
    else            { m.by = -1; m.cy = (int64_t)h - 1; }
    return m;
}

pc_gm_map pc_gm_rotate(uint32_t w, uint32_t h, pc_rotation r)
{
    pc_gm_map m = {0, 0, 0, 0, 0, 0};
    switch (r) {
    case PC_ROTATE_90_CW:          /* new(x, y) = old(y, h - 1 - x) */
        m.bx = 1; m.ay = -1; m.cy = (int64_t)h - 1;
        break;
    case PC_ROTATE_90_CCW:         /* new(x, y) = old(w - 1 - y, x) */
        m.bx = -1; m.cx = (int64_t)w - 1; m.ay = 1;
        break;
    case PC_ROTATE_180:
    default:
        m.ax = -1; m.cx = (int64_t)w - 1; m.by = -1; m.cy = (int64_t)h - 1;
        break;
    }
    return m;
}

/* ---- remapper ------------------------------------------------------------------- */
typedef struct gm_ctx {
    const pc_tile *const *src;
    uint32_t       sw, sh, stx;
    uint8_t        bpp;
    pc_gm_map      m;
    bool           identity;          /* ax = by = 1, bx = ay = 0 */
    uint32_t       dw, dh, dtx;
    const uint8_t *fill;              /* bpp bytes or NULL */
    pc_tile       *fill_full;         /* shared full interior fill tile or NULL */
    const pc_tile *const *mask;
    pc_tile      **dst;
    pc_atomic_u32  fail;
} gm_ctx;

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void apply_mask(uint8_t *data, const uint8_t *mk, uint32_t cw, uint32_t ch)
{
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            uint32_t k = mk[y * PC_TILE_DIM + x], a;
            uint8_t *p = data + ((size_t)y * PC_TILE_DIM + x) * 4u;
            if (k == 255u) continue;
            a = ((uint32_t)p[3] * k + 127u) / 255u;
            if (a == 0u) memset(p, 0, 4u);
            else p[3] = (uint8_t)a;
        }
}

static void gm_job(void *ud, uint32_t t, uint32_t worker)
{
    gm_ctx *c = (gm_ctx *)ud;
    uint32_t tx = t % c->dtx, ty = t / c->dtx;
    int64_t x0 = (int64_t)tx * PC_TILE_DIM, y0 = (int64_t)ty * PC_TILE_DIM;
    uint32_t cw = min_u32(PC_TILE_DIM, c->dw - (uint32_t)x0);
    uint32_t ch = min_u32(PC_TILE_DIM, c->dh - (uint32_t)y0);
    const uint8_t *mk = NULL;
    const pc_gm_map *m = &c->m;
    int64_t cx[4], cy[4], mnx, mxx, mny, mxy;
    bool inside_all, any_src = false, need_fill;
    pc_tile *tile;
    size_t bpp = c->bpp;
    (void)worker;
    c->dst[t] = NULL;
    if (c->mask) {
        const pc_tile *mt = c->mask[t];
        bool full = true;
        if (!mt) return;                       /* coverage 0: transparent */
        for (uint32_t y = 0; y < ch && full; y++)
            for (uint32_t x = 0; x < cw; x++)
                if (mt->data[(size_t)y * PC_TILE_DIM + x] != 255u) { full = false; break; }
        if (!full) mk = mt->data;              /* fully covered tiles stay shareable */
    }
    /* source bounding box of the in-document destination rect */
    for (int k = 0; k < 4; k++) {
        int64_t X = x0 + ((k & 1) ? (int64_t)cw - 1 : 0);
        int64_t Y = y0 + ((k & 2) ? (int64_t)ch - 1 : 0);
        cx[k] = m->ax * X + m->bx * Y + m->cx;
        cy[k] = m->ay * X + m->by * Y + m->cy;
    }
    mnx = mxx = cx[0]; mny = mxy = cy[0];
    for (int k = 1; k < 4; k++) {
        if (cx[k] < mnx) mnx = cx[k];
        if (cx[k] > mxx) mxx = cx[k];
        if (cy[k] < mny) mny = cy[k];
        if (cy[k] > mxy) mxy = cy[k];
    }
    inside_all = mnx >= 0 && mny >= 0 && mxx < (int64_t)c->sw && mxy < (int64_t)c->sh;
    if (mnx < 0) mnx = 0;
    if (mny < 0) mny = 0;
    if (mxx >= (int64_t)c->sw) mxx = (int64_t)c->sw - 1;
    if (mxy >= (int64_t)c->sh) mxy = (int64_t)c->sh - 1;
    if (mnx <= mxx && mny <= mxy)
        for (int64_t sty = mny >> PC_TILE_SHIFT; sty <= mxy >> PC_TILE_SHIFT && !any_src; sty++)
            for (int64_t stx = mnx >> PC_TILE_SHIFT; stx <= mxx >> PC_TILE_SHIFT; stx++)
                if (c->src[(size_t)sty * c->stx + (size_t)stx]) { any_src = true; break; }
    need_fill = c->fill != NULL && !inside_all;
    if (!any_src && !need_fill) return;

    /* whole-tile sharing: pure translation by multiples of 64 */
    if (c->identity && !mk && inside_all && ((m->cx | m->cy) & 63) == 0) {
        int64_t sx0 = x0 + m->cx, sy0 = y0 + m->cy;
        uint32_t scw = min_u32(PC_TILE_DIM, c->sw - (uint32_t)sx0);
        uint32_t sch = min_u32(PC_TILE_DIM, c->sh - (uint32_t)sy0);
        if (scw == cw && sch == ch) {
            pc_tile *st = (pc_tile *)c->src[(size_t)(sy0 >> PC_TILE_SHIFT) * c->stx +
                                             (size_t)(sx0 >> PC_TILE_SHIFT)];
            pc_tile_retain(st);
            c->dst[t] = st;
            return;
        }
    }
    if (!any_src && c->fill_full && cw == PC_TILE_DIM && ch == PC_TILE_DIM && !mk &&
        (mnx > mxx || mny > mxy)) {
        pc_tile_retain(c->fill_full);
        c->dst[t] = c->fill_full;
        return;
    }
    if (pc_atomic_load(&c->fail)) return;
    tile = pc_tile_new_zero(c->bpp);
    if (!tile) { pc_atomic_store(&c->fail, 1u); return; }

    if (c->identity) {
        for (uint32_t y = 0; y < ch; y++) {
            int64_t sy = y0 + y + m->cy;
            uint8_t *drow = tile->data + (size_t)y * PC_TILE_DIM * bpp;
            for (uint32_t x = 0; x < cw;) {
                int64_t sx = x0 + x + m->cx;
                uint32_t run;
                if (sy < 0 || sy >= (int64_t)c->sh || sx < 0 || sx >= (int64_t)c->sw) {
                    if (c->fill) memcpy(drow + (size_t)x * bpp, c->fill, bpp);
                    x++;
                    continue;
                }
                run = PC_TILE_DIM - (uint32_t)(sx & (PC_TILE_DIM - 1));
                run = min_u32(run, cw - x);
                run = min_u32(run, (uint32_t)((int64_t)c->sw - sx));
                {
                    const pc_tile *st = c->src[(size_t)(sy >> PC_TILE_SHIFT) * c->stx +
                                               (size_t)(sx >> PC_TILE_SHIFT)];
                    if (st)
                        memcpy(drow + (size_t)x * bpp,
                               st->data + ((size_t)(sy & (PC_TILE_DIM - 1)) * PC_TILE_DIM +
                                           (size_t)(sx & (PC_TILE_DIM - 1))) * bpp,
                               (size_t)run * bpp);
                }
                x += run;
            }
        }
    } else {
        for (uint32_t y = 0; y < ch; y++) {
            int64_t Y = y0 + y;
            uint8_t *drow = tile->data + (size_t)y * PC_TILE_DIM * bpp;
            for (uint32_t x = 0; x < cw; x++) {
                int64_t X = x0 + x;
                int64_t sx = m->ax * X + m->bx * Y + m->cx, sy = m->ay * X + m->by * Y + m->cy;
                if (sx < 0 || sy < 0 || sx >= (int64_t)c->sw || sy >= (int64_t)c->sh) {
                    if (c->fill) memcpy(drow + (size_t)x * bpp, c->fill, bpp);
                    continue;
                }
                {
                    const pc_tile *st = c->src[(size_t)(sy >> PC_TILE_SHIFT) * c->stx +
                                               (size_t)(sx >> PC_TILE_SHIFT)];
                    if (st)
                        memcpy(drow + (size_t)x * bpp,
                               st->data + ((size_t)(sy & (PC_TILE_DIM - 1)) * PC_TILE_DIM +
                                           (size_t)(sx & (PC_TILE_DIM - 1))) * bpp, bpp);
                }
            }
        }
    }
    if (mk && bpp == 4u) apply_mask(tile->data, mk, cw, ch);
    if (pc_tile_is_zero(tile)) { pc_tile_release(tile); tile = NULL; }
    c->dst[t] = tile;
}

pc_status pc_gm_build(const pc_tile *const *src, uint32_t sw, uint32_t sh, uint8_t bpp,
                      pc_gm_map m, uint32_t dw, uint32_t dh, const void *fill,
                      const pc_tile *const *mask, const pc_par *par, pc_tile ***out)
{
    gm_ctx c;
    size_t n;
    *out = NULL;
    if (!src || (bpp != 1u && bpp != 4u) || sw == 0u || sh == 0u || dw == 0u || dh == 0u ||
        dw > PC_MAX_DIM || dh > PC_MAX_DIM)
        return PC_ERR_ARG;
    memset(&c, 0, sizeof c);
    c.src = src;
    c.sw = sw; c.sh = sh;
    c.stx = (sw + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    c.bpp = bpp;
    c.m = m;
    c.identity = m.ax == 1 && m.by == 1 && m.bx == 0 && m.ay == 0;
    c.dw = dw; c.dh = dh;
    c.dtx = (dw + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    n = (size_t)c.dtx * ((dh + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT);
    c.mask = mask;
    if (fill) {
        bool zero = true;
        for (uint8_t i = 0; i < bpp; i++) if (((const uint8_t *)fill)[i]) zero = false;
        if (!zero) c.fill = (const uint8_t *)fill;
    }
    c.dst = (pc_tile **)gm_calloc(n, sizeof *c.dst);
    if (!c.dst) return PC_ERR_NOMEM;
    if (c.fill) {
        c.fill_full = pc_tile_new_fill(bpp, c.fill, PC_TILE_DIM, PC_TILE_DIM);
        if (!c.fill_full) { free(c.dst); return PC_ERR_NOMEM; }
    }
    pc_par_for(par, gm_job, &c, (uint32_t)n);
    pc_tile_release(c.fill_full);
    if (pc_atomic_load(&c.fail)) {
        pc_grid_free(c.dst, n);
        return PC_ERR_NOMEM;
    }
    *out = c.dst;
    return PC_OK;
}

/* ---- geometry state payload ------------------------------------------------- */
typedef struct geom_state {
    uint32_t   w, h, tiles_x, tiles_y;
    uint32_t   n;              /* layers */
    uint32_t  *ids;
    pc_tile ***grids;
    pc_tile  **sel;            /* may be NULL */
    bool       sel_active;
} geom_state;

static size_t gs_tiles(const geom_state *g) { return (size_t)g->tiles_x * g->tiles_y; }

static void geom_destroy(void *p)
{
    geom_state *g = (geom_state *)p;
    if (!g) return;
    if (g->grids)
        for (uint32_t i = 0; i < g->n; i++) pc_grid_free(g->grids[i], gs_tiles(g));
    pc_grid_free(g->sel, gs_tiles(g));
    free(g->grids);
    free(g->ids);
    free(g);
}

static size_t grid_bytes(pc_tile **grid, size_t n)
{
    size_t b = 0;
    if (!grid) return 0u;
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = grid[i];
        if (t) {
            uint32_t r = pc_tile_refs(t);
            b += pc_tile_bytes(t->bpp) / (r ? r : 1u);
        }
    }
    return b + n * sizeof *grid;
}

static size_t geom_bytes(const void *p)
{
    const geom_state *g = (const geom_state *)p;
    size_t b = sizeof *g + (size_t)g->n * (sizeof *g->ids + sizeof *g->grids);
    for (uint32_t i = 0; i < g->n; i++) b += grid_bytes(g->grids[i], gs_tiles(g));
    return b + grid_bytes(g->sel, gs_tiles(g));
}

static void geom_swap(pc_doc *d, void *p)
{
    geom_state *g = (geom_state *)p;
    uint32_t t;
    bool b;
    pc_tile **s;
    PC_ASSERT(d->n_layers == g->n);                   /* INV-HIST-PATH */
    for (uint32_t i = 0; i < g->n; i++) {
        pc_layer *l = d->stack[i];
        pc_tile **tmp = l->grid;
        PC_ASSERT(l->id == g->ids[i]);
        l->grid = g->grids[i];
        g->grids[i] = tmp;
        l->tiles_x = g->tiles_x;
        l->tiles_y = g->tiles_y;
        l->gen++;
    }
    t = d->w; d->w = g->w; g->w = t;
    t = d->h; d->h = g->h; g->h = t;
    t = d->tiles_x; d->tiles_x = g->tiles_x; g->tiles_x = t;
    t = d->tiles_y; d->tiles_y = g->tiles_y; g->tiles_y = t;
    s = d->sel_grid; d->sel_grid = g->sel; g->sel = s;
    b = d->sel_active; d->sel_active = g->sel_active; g->sel_active = b;
    d->sel_gen++;
    d->gen++;
}

static const pc_hist_ops k_geom_ops = { geom_swap, geom_destroy, geom_bytes };

static geom_state *gs_new(const pc_doc *d, uint32_t w, uint32_t h)
{
    geom_state *g = (geom_state *)gm_calloc(1u, sizeof *g);
    if (!g) return NULL;
    g->w = w;
    g->h = h;
    g->tiles_x = (w + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    g->tiles_y = (h + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    g->n = d->n_layers;
    g->sel_active = d->sel_active;
    g->ids = (uint32_t *)gm_calloc(g->n, sizeof *g->ids);
    g->grids = (pc_tile ***)gm_calloc(g->n, sizeof *g->grids);
    if (!g->ids || !g->grids) { geom_destroy(g); return NULL; }
    for (uint32_t i = 0; i < g->n; i++) g->ids[i] = d->stack[i]->id;
    return g;
}

static const char *lbl(const char *label, const char *def) { return label ? label : def; }

/* Link the fully built target state: apply == swap. */
static pc_status gs_commit(pc_hist *h, geom_state *g, const char *label)
{
    pc_hist_node *node = pc_gm_node_new(label);
    if (!node) { geom_destroy(g); return PC_ERR_NOMEM; }
    geom_swap(h->doc, g);
    pc_hist_link(h, node, &k_geom_ops, g);
    return PC_OK;
}

static pc_status check_hist(const pc_hist *h)
{
    if (!h || !h->doc) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    return PC_OK;
}

/* Remap every layer (and the selection) with map m into a dw x dh doc. */
static pc_status geom_remap(pc_hist *h, uint32_t dw, uint32_t dh, pc_gm_map m,
                            const pc_px32 *bottom_fill, bool mask_by_sel,
                            const pc_par *par, const char *label)
{
    pc_doc *d = h->doc;
    geom_state *g;
    pc_status st;
    if (dw == 0u || dh == 0u || dw > PC_MAX_DIM || dh > PC_MAX_DIM) return PC_ERR_ARG;
    g = gs_new(d, dw, dh);
    if (!g) return PC_ERR_NOMEM;
    if (d->sel_grid) {
        st = pc_gm_build((const pc_tile *const *)d->sel_grid, d->w, d->h, 1u, m, dw, dh,
                         NULL, NULL, par, &g->sel);
        if (st != PC_OK) { geom_destroy(g); return st; }
    }
    if (mask_by_sel && !g->sel) { geom_destroy(g); return PC_ERR_STATE; }
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_tile *const *mask = mask_by_sel ? (const pc_tile *const *)g->sel : NULL;
        st = pc_gm_build((const pc_tile *const *)d->stack[i]->grid, d->w, d->h, 4u, m, dw, dh,
                         (i == 0u && bottom_fill && bottom_fill->a) ? (const void *)bottom_fill
                                                                    : NULL,
                         mask, par, &g->grids[i]);
        if (st != PC_OK) { geom_destroy(g); return st; }
    }
    return gs_commit(h, g, label);
}

/* ---- public operations --------------------------------------------------------- */
void pc_geom_anchor_offset(uint32_t old_w, uint32_t old_h, uint32_t new_w, uint32_t new_h,
                           pc_anchor a, int32_t *dx, int32_t *dy)
{
    int64_t ex = (int64_t)new_w - old_w, ey = (int64_t)new_h - old_h;
    int64_t x = 0, y = 0;
    switch ((int)a % 3) {
    case 1: x = ex / 2; break;          /* C division truncates toward zero */
    case 2: x = ex; break;
    default: break;
    }
    switch ((int)a / 3) {
    case 1: y = ey / 2; break;
    case 2: y = ey; break;
    default: break;
    }
    if (dx) *dx = (int32_t)x;
    if (dy) *dy = (int32_t)y;
}

pc_rect pc_geom_selection_bounds(const pc_doc *d)
{
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = -1, y1 = -1;
    if (!d || !d->sel_active || !d->sel_grid) return pc_rect_make(0, 0, 0, 0);
    for (uint32_t ty = 0; ty < d->tiles_y; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
            const pc_tile *t = d->sel_grid[(size_t)ty * d->tiles_x + tx];
            int64_t bx = (int64_t)tx * PC_TILE_DIM, by = (int64_t)ty * PC_TILE_DIM;
            if (!t) continue;
            /* skip tiles that cannot extend the current box */
            if (bx >= x0 && by >= y0 && bx + PC_TILE_DIM - 1 <= x1 && by + PC_TILE_DIM - 1 <= y1)
                continue;
            for (uint32_t y = 0; y < PC_TILE_DIM; y++) {
                const uint8_t *row = t->data + (size_t)y * PC_TILE_DIM;
                for (uint32_t x = 0; x < PC_TILE_DIM; x++) {
                    if (!row[x]) continue;
                    if (bx + x < x0) x0 = bx + x;
                    if (bx + x > x1) x1 = bx + x;
                    if (by + y < y0) y0 = by + y;
                    if (by + y > y1) y1 = by + y;
                }
            }
        }
    if (x1 < 0) return pc_rect_make(0, 0, 0, 0);
    return pc_rect_intersect(pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0 + 1),
                                          (int32_t)(y1 - y0 + 1)), pc_doc_rect(d));
}

pc_status pc_geom_resize(pc_hist *h, uint32_t w, uint32_t hgt, pc_resample mode,
                         uint32_t flags, const pc_par *par, const char *label)
{
    pc_doc *d;
    geom_state *g;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (w == 0u || hgt == 0u || w > PC_MAX_DIM || hgt > PC_MAX_DIM ||
        (unsigned)mode >= (unsigned)PC_RESAMPLE_COUNT)
        return PC_ERR_ARG;
    g = gs_new(d, w, hgt);
    if (!g) return PC_ERR_NOMEM;
    if (d->sel_grid) {
        pc_grid sg;
        sg.tiles = (const pc_tile *const *)d->sel_grid;
        sg.w = d->w; sg.h = d->h; sg.tiles_x = d->tiles_x; sg.tiles_y = d->tiles_y; sg.bpp = 1u;
        st = pc_resample_grid(&sg, w, hgt, mode, 0u, par, &g->sel);
        if (st != PC_OK) { geom_destroy(g); return st; }
    }
    for (uint32_t i = 0; i < d->n_layers; i++) {
        pc_grid lg = pc_grid_of_layer(d, d->stack[i]);
        st = pc_resample_grid(&lg, w, hgt, mode, flags, par, &g->grids[i]);
        if (st != PC_OK) { geom_destroy(g); return st; }
    }
    return gs_commit(h, g, lbl(label, "Resize"));
}

pc_status pc_geom_canvas_size(pc_hist *h, uint32_t w, uint32_t hgt, pc_anchor anchor,
                              pc_px32 fill, const pc_par *par, const char *label)
{
    int32_t dx, dy;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    if ((unsigned)anchor > (unsigned)PC_ANCHOR_BOTTOM_RIGHT) return PC_ERR_ARG;
    pc_geom_anchor_offset(h->doc->w, h->doc->h, w, hgt, anchor, &dx, &dy);
    return geom_remap(h, w, hgt, pc_gm_translate(dx, dy), &fill, false, par,
                      lbl(label, "Canvas Size"));
}

pc_status pc_geom_crop(pc_hist *h, pc_rect r, const pc_par *par, const char *label)
{
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    r = pc_rect_intersect(r, pc_doc_rect(h->doc));
    if (pc_rect_is_empty(r)) return PC_ERR_ARG;
    return geom_remap(h, (uint32_t)r.w, (uint32_t)r.h, pc_gm_translate(-r.x, -r.y), NULL,
                      false, par, lbl(label, "Crop"));
}

pc_status pc_geom_crop_to_selection(pc_hist *h, const pc_par *par, const char *label)
{
    pc_rect r;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    r = pc_geom_selection_bounds(h->doc);
    if (pc_rect_is_empty(r)) return PC_ERR_STATE;
    return geom_remap(h, (uint32_t)r.w, (uint32_t)r.h, pc_gm_translate(-r.x, -r.y), NULL,
                      true, par, lbl(label, "Crop to Selection"));
}

pc_status pc_geom_rotate(pc_hist *h, pc_rotation rot, const pc_par *par, const char *label)
{
    pc_doc *d;
    bool swap_dims;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    if ((unsigned)rot > (unsigned)PC_ROTATE_180) return PC_ERR_ARG;
    d = h->doc;
    swap_dims = rot != PC_ROTATE_180;
    return geom_remap(h, swap_dims ? d->h : d->w, swap_dims ? d->w : d->h,
                      pc_gm_rotate(d->w, d->h, rot), NULL, false, par,
                      lbl(label, rot == PC_ROTATE_90_CW ? "Rotate 90 CW" :
                                 rot == PC_ROTATE_90_CCW ? "Rotate 90 CCW" : "Rotate 180"));
}

pc_status pc_geom_flip(pc_hist *h, bool horizontal, const pc_par *par, const char *label)
{
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    return geom_remap(h, h->doc->w, h->doc->h, pc_gm_flip(h->doc->w, h->doc->h, horizontal),
                      NULL, false, par,
                      lbl(label, horizontal ? "Flip Horizontal" : "Flip Vertical"));
}
