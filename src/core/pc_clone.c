/* pc_clone.c - Clone Stamp: source point, locked offset, source rows. */
#include "pc/pc_clone.h"

#include <math.h>
#include <string.h>

void pc_clone_init(pc_clone *c)
{
    if (c) memset(c, 0, sizeof *c);
}

void pc_clone_set_source(pc_clone *c, uint32_t layer_id, double x, double y)
{
    if (!c || !isfinite(x) || !isfinite(y)) return;
    c->has_source = true;
    c->src_layer = layer_id;
    c->sx = x;
    c->sy = y;
    c->locked = false;
    c->dx = c->dy = 0;
}

bool pc_clone_source_pos(const pc_clone *c, double x, double y, double *sx, double *sy)
{
    double ox, oy;
    if (!c || !c->has_source) return false;
    if (c->locked) { ox = x - (double)c->dx; oy = y - (double)c->dy; }
    else { ox = c->sx; oy = c->sy; }
    if (sx) *sx = ox;
    if (sy) *sy = oy;
    return true;
}

static int32_t pix(double v)
{
    if (v < -PC_BRUSH_COORD_MAX) v = -PC_BRUSH_COORD_MAX;
    if (v > PC_BRUSH_COORD_MAX) v = PC_BRUSH_COORD_MAX;
    return (int32_t)floor(v);
}

void pc_clone_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const pc_clone *c = (const pc_clone *)ud;
    int64_t sy = (int64_t)y - c->dy, sx = (int64_t)x - c->dx;
    int32_t i = 0;
    if (n <= 0) return;
    if (sy < 0 || sy >= (int64_t)c->h) {
        memset(out, 0, (size_t)n * sizeof *out);
        return;
    }
    while (i < n) {
        int64_t px = sx + i;
        int32_t run;
        if (px < 0) {
            int64_t r = -px;
            run = r < (int64_t)(n - i) ? (int32_t)r : n - i;
            memset(out + i, 0, (size_t)run * sizeof *out);
        } else if (px >= (int64_t)c->w) {
            run = n - i;
            memset(out + i, 0, (size_t)run * sizeof *out);
        } else {
            uint32_t tx = (uint32_t)(px >> PC_TILE_SHIFT), ty = (uint32_t)(sy >> PC_TILE_SHIFT);
            int64_t lim = (int64_t)PC_TILE_DIM - (px & (int64_t)(PC_TILE_DIM - 1u));
            const pc_tile *tile;
            if ((int64_t)c->w - px < lim) lim = (int64_t)c->w - px;
            run = lim < (int64_t)(n - i) ? (int32_t)lim : n - i;
            tile = pc_txn_original(c->t, c->src_layer, ty * c->tiles_x + tx);
            if (tile && tile->bpp == 4u) {
                size_t o = ((size_t)(sy & (int64_t)(PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                            (size_t)(px & (int64_t)(PC_TILE_DIM - 1u))) * 4u;
                memcpy(out + i, tile->data + o, (size_t)run * sizeof *out);
            } else {
                memset(out + i, 0, (size_t)run * sizeof *out);
            }
        }
        i += run;
    }
}

pc_status pc_clone_begin(pc_clone *c, pc_brush *b, pc_txn *t, uint32_t layer_id,
                         const pc_brush_params *params, pc_px32 color, uint32_t tool_blend,
                         bool clip_to_selection, const pc_par *par,
                         const pc_brush_sample *s, uint32_t flags, pc_rect *dirty)
{
    pc_paint_src src;
    pc_paint_opts opts;
    const pc_doc *d;
    bool was_locked;
    int32_t odx, ody;
    pc_status st;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!c || !b || !t || !s || !isfinite(s->x) || !isfinite(s->y)) return PC_ERR_ARG;
    if (pc_brush_is_active(b)) return PC_ERR_STATE;
    d = pc_txn_doc(t);
    if (!d) return PC_ERR_ARG;
    if (!c->has_source) return PC_ERR_STATE;
    {
        const pc_layer *sl = pc_doc_layer_by_id(d, c->src_layer);
        if (!sl || sl->tiles_x != d->tiles_x || sl->tiles_y != d->tiles_y) return PC_ERR_STATE;
    }
    was_locked = c->locked;
    odx = c->dx;
    ody = c->dy;
    if (!c->locked) {
        c->dx = pix(s->x) - pix(c->sx);
        c->dy = pix(s->y) - pix(c->sy);
        c->locked = true;
    }
    c->t = t;
    c->w = d->w;
    c->h = d->h;
    c->tiles_x = d->tiles_x;
    pc_brush_paint_color(color, tool_blend, clip_to_selection, &src, &opts);
    opts.opacity = color.a;
    src.row = pc_clone_row;
    src.ud = c;
    st = pc_brush_begin(b, t, layer_id, params, &src, &opts, par, s, flags, dirty);
    if (!pc_brush_is_active(b)) {
        /* the stroke never started: keep the previous lock state */
        c->locked = was_locked;
        c->dx = odx;
        c->dy = ody;
        c->t = NULL;
    }
    return st;
}
