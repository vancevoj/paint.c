/* pc_recolor.c - Recolor: tolerance match and channel shift of the
 * stroke-start pixels. The shift rule follows the MIT Paint.NET 3.36
 * RecolorTool (docs/notice/e1.md); the tolerance metric is the core one. */
#include "pc/pc_recolor.h"
#include "pc/pc_fill.h"

#include <math.h>
#include <string.h>

pc_recolor_opts pc_recolor_opts_default(void)
{
    pc_recolor_opts o;
    memset(&o, 0, sizeof o);
    o.sampling = PC_RECOLOR_SAMPLING_ONCE;
    o.replacement.a = 255u;
    o.target.b = o.target.g = o.target.r = o.target.a = 255u;
    o.tolerance = 50u;
    o.alpha_mode = PC_RECOLOR_ALPHA_PREMULTIPLIED;
    o.clip_to_selection = true;
    return o;
}

bool pc_recolor_match(pc_px32 a, pc_px32 b, uint32_t lim, pc_recolor_alpha mode)
{
    int32_t dr, dg, db, da;
    uint32_t d;
    if (mode != PC_RECOLOR_ALPHA_STRAIGHT) return pc_color_within(a, b, lim);
    dr = (int32_t)a.r - (int32_t)b.r;
    dg = (int32_t)a.g - (int32_t)b.g;
    db = (int32_t)a.b - (int32_t)b.b;
    da = (int32_t)a.a - (int32_t)b.a;
    d = (uint32_t)(dr * dr + dg * dg + db * db + da * da) * 255u;   /* <= 66325500 */
    if (lim > 65025u) lim = 65025u;
    return d <= lim * 1020u;
}

static uint8_t shift8(uint8_t v, uint8_t to, uint8_t from)
{
    int32_t r = (int32_t)v + (int32_t)to - (int32_t)from;
    return (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
}

pc_px32 pc_recolor_pixel(pc_px32 orig, pc_px32 target, pc_px32 replacement, uint32_t lim,
                         pc_recolor_alpha mode)
{
    pc_px32 o = orig;
    if (!pc_recolor_match(orig, target, lim, mode)) return orig;
    o.b = shift8(orig.b, replacement.b, target.b);
    o.g = shift8(orig.g, replacement.g, target.g);
    o.r = shift8(orig.r, replacement.r, target.r);
    return o;
}

/* original (stroke-start) pixels of the layer, zero outside the image */
static void read_orig(const pc_recolor *rc, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    int32_t i = 0;
    if (y < 0 || (uint32_t)y >= rc->h) { memset(out, 0, (size_t)n * sizeof *out); return; }
    while (i < n) {
        int64_t px = (int64_t)x + i;
        int32_t run;
        if (px < 0) {
            int64_t r = -px;
            run = r < (int64_t)(n - i) ? (int32_t)r : n - i;
            memset(out + i, 0, (size_t)run * sizeof *out);
        } else if (px >= (int64_t)rc->w) {
            run = n - i;
            memset(out + i, 0, (size_t)run * sizeof *out);
        } else {
            int64_t lim = (int64_t)PC_TILE_DIM - (px & (int64_t)(PC_TILE_DIM - 1u));
            uint32_t idx = (uint32_t)(y >> PC_TILE_SHIFT) * rc->tiles_x +
                           (uint32_t)(px >> PC_TILE_SHIFT);
            const pc_tile *tile = pc_txn_original(rc->t, rc->layer, idx);
            if ((int64_t)rc->w - px < lim) lim = (int64_t)rc->w - px;
            run = lim < (int64_t)(n - i) ? (int32_t)lim : n - i;
            if (tile && tile->bpp == 4u) {
                size_t o = ((size_t)(y & (int32_t)(PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                            (size_t)(px & (int64_t)(PC_TILE_DIM - 1u))) * 4u;
                memcpy(out + i, tile->data + o, (size_t)run * sizeof *out);
            } else {
                memset(out + i, 0, (size_t)run * sizeof *out);
            }
        }
        i += run;
    }
}

void pc_recolor_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const pc_recolor *rc = (const pc_recolor *)ud;
    if (n <= 0) return;
    read_orig(rc, x, y, n, out);
    if (!rc->have_target) return;
    for (int32_t i = 0; i < n; i++)
        out[i] = pc_recolor_pixel(out[i], rc->target, rc->o.replacement, rc->lim,
                                  rc->o.alpha_mode);
}

/* Sampling Once: the first canvas point of the stroke defines the target. */
static void pick_target(pc_recolor *rc, const pc_brush_sample *s)
{
    double fx, fy;
    if (rc->have_target || !s || !isfinite(s->x) || !isfinite(s->y)) return;
    fx = floor(s->x);
    fy = floor(s->y);
    if (fx < 0.0 || fy < 0.0 || fx >= (double)rc->w || fy >= (double)rc->h) return;
    read_orig(rc, (int32_t)fx, (int32_t)fy, 1, &rc->target);
    rc->have_target = true;
}

pc_status pc_recolor_begin(pc_recolor *rc, pc_brush *b, pc_txn *t, uint32_t layer_id,
                           const pc_brush_params *params, const pc_recolor_opts *o,
                           const pc_par *par, const pc_brush_sample *s, uint32_t flags,
                           pc_rect *dirty)
{
    pc_paint_src src;
    pc_paint_opts opts;
    const pc_doc *d;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!rc || !b || !t || !o) return PC_ERR_ARG;
    if (pc_brush_is_active(b)) return PC_ERR_STATE;
    if ((unsigned)o->sampling > (unsigned)PC_RECOLOR_SAMPLING_SECONDARY ||
        (unsigned)o->alpha_mode > (unsigned)PC_RECOLOR_ALPHA_STRAIGHT)
        return PC_ERR_ARG;
    d = pc_txn_doc(t);
    if (!d) return PC_ERR_ARG;
    memset(rc, 0, sizeof *rc);
    rc->o = *o;
    rc->lim = pc_tol_limit_from_percent(o->tolerance > 100u ? 100u : o->tolerance);
    rc->t = t;
    rc->layer = layer_id;
    rc->w = d->w;
    rc->h = d->h;
    rc->tiles_x = d->tiles_x;
    if (o->sampling == PC_RECOLOR_SAMPLING_SECONDARY) {
        rc->target = o->target;
        rc->have_target = true;
    } else if (pc_doc_layer_by_id(d, layer_id)) {
        pick_target(rc, s);
    }
    memset(&src, 0, sizeof src);
    src.row = pc_recolor_row;
    src.ud = rc;
    opts = pc_paint_opts_default();
    opts.mode = PC_PAINT_OVERWRITE;
    opts.opacity = o->replacement.a;
    opts.clip_to_selection = o->clip_to_selection;
    return pc_brush_begin(b, t, layer_id, params, &src, &opts, par, s, flags, dirty);
}

pc_status pc_recolor_add(pc_recolor *rc, pc_brush *b, const pc_brush_sample *s,
                         pc_rect *dirty)
{
    if (rc && pc_brush_is_active(b)) pick_target(rc, s);
    return pc_brush_add(b, s, dirty);
}

bool pc_recolor_target(const pc_recolor *rc, pc_px32 *out)
{
    if (!rc || !rc->have_target) return false;
    if (out) *out = rc->target;
    return true;
}
