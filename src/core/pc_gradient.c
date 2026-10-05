/* pc_gradient.c - the Gradient tool engine (lane E2). See pc_gradient.h.
 *
 * The linear, reflected, diamond and radial parameterizations and the
 * Transparency-mode alpha rule (start alpha = primary alpha, end alpha
 * = 255 - secondary alpha, multiplied into the layer alpha with alpha
 * blending, written directly without) follow the MIT-licensed Paint.NET
 * 3.36 GradientRenderers.cs / GradientRenderer.cs / GradientTool.cs (see
 * docs/notice/e2.md). The conical and spiral parameterizations, repeat
 * modes and aliased quantization were measured black-box on Paint.NET;
 * seam supersampling, the dither hash and the transaction plumbing are this
 * project's own. */
#include "pc/pc_gradient.h"
#include "pc/pc_sel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979323846

static const char *const k_type_names[PC_GRAD_TYPE_COUNT] = {
    "Linear", "Linear (Reflected)", "Linear (Diamond)", "Radial", "Conical",
    "Spiral (Clockwise)", "Spiral (Counter-clockwise)"
};
static const char *const k_repeat_names[PC_GRAD_REPEAT_COUNT] = {
    "No Repeat", "Repeat Wrapped", "Repeat Reflected"
};

const char *pc_grad_type_name(pc_grad_type t)
{
    return (unsigned)t < (unsigned)PC_GRAD_TYPE_COUNT ? k_type_names[t] : NULL;
}

const char *pc_grad_repeat_name(pc_grad_repeat r)
{
    return (unsigned)r < (unsigned)PC_GRAD_REPEAT_COUNT ? k_repeat_names[r] : NULL;
}

const char *pc_grad_mode_name(pc_grad_mode m)
{
    if (m == PC_GRAD_COLOR) return "Color Mode";
    if (m == PC_GRAD_TRANSPARENCY) return "Transparency Mode";
    return NULL;
}

static pc_px32 mkpx(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    pc_px32 p;
    p.b = b; p.g = g; p.r = r; p.a = a;
    return p;
}

pc_gradient_desc pc_gradient_desc_default(void)
{
    pc_gradient_desc g;
    memset(&g, 0, sizeof g);
    g.type = PC_GRAD_LINEAR;
    g.repeat = PC_GRAD_NO_REPEAT;
    g.mode = PC_GRAD_COLOR;
    g.antialias = true;
    g.start = pc_pt_make(0.0, 0.0);
    g.end = pc_pt_make(0.0, 0.0);
    g.c0 = mkpx(0u, 0u, 0u, 255u);
    g.c1 = mkpx(255u, 255u, 255u, 255u);
    g.a0 = 255u;
    g.a1 = 0u;
    return g;
}

void pc_gradient_colors(pc_gradient_desc *g, pc_px32 primary, pc_px32 secondary, bool reversed)
{
    if (!g) return;
    if (!reversed) {
        g->c0 = primary;
        g->c1 = secondary;
        g->a0 = primary.a;
        g->a1 = (uint8_t)(255u - secondary.a);
    } else {
        /* Color mode swaps the colors; Transparency mode reverses and
         * inverts the alpha pair, which runs the same ramp backwards. */
        g->c0 = secondary;
        g->c1 = primary;
        g->a0 = (uint8_t)(255u - secondary.a);
        g->a1 = primary.a;
    }
}

pc_status pc_gradient_prepare(pc_gradient *g, const pc_gradient_desc *desc)
{
    double l2;
    if (!g) return PC_ERR_ARG;
    memset(g, 0, sizeof *g);
    if (!desc) return PC_ERR_ARG;
    if ((unsigned)desc->type >= (unsigned)PC_GRAD_TYPE_COUNT ||
        (unsigned)desc->repeat >= (unsigned)PC_GRAD_REPEAT_COUNT ||
        (desc->mode != PC_GRAD_COLOR && desc->mode != PC_GRAD_TRANSPARENCY) ||
        !isfinite(desc->start.x) || !isfinite(desc->start.y) ||
        !isfinite(desc->end.x) || !isfinite(desc->end.y))
        return PC_ERR_ARG;
    g->d = *desc;
    g->vx = desc->end.x - desc->start.x;
    g->vy = desc->end.y - desc->start.y;
    l2 = g->vx * g->vx + g->vy * g->vy;
    g->degenerate = !(l2 > 0.0) || !isfinite(l2);
    if (!g->degenerate) {
        g->inv_len2 = 1.0 / l2;
        g->inv_len = 1.0 / sqrt(l2);
        g->base_angle = atan2(g->vy, g->vx);
        /* u = (d . v) / L^2 changes by (|vx| + |vy|) / L^2 across a pixel */
        g->span_lin = 0.5 * (fabs(g->vx) + fabs(g->vy)) * g->inv_len2;
    }
    {
        const pc_px32 *c[2] = { &desc->c0, &desc->c1 };
        float *o[2] = { g->c0p, g->c1p };
        for (int i = 0; i < 2; i++) {
            float a = (float)c[i]->a;
            o[i][0] = (float)c[i]->b * a;
            o[i][1] = (float)c[i]->g * a;
            o[i][2] = (float)c[i]->r * a;
            o[i][3] = a;
        }
    }
    return PC_OK;
}

/* Clockwise (screen, y down) angle from the start -> end direction to d,
 * in [0, 2 pi). */
static double cw_angle(const pc_gradient *g, double dx, double dy)
{
    double a = atan2(dy, dx) - g->base_angle;
    while (a < 0.0) a += 2.0 * PI_D;
    while (a >= 2.0 * PI_D) a -= 2.0 * PI_D;
    return a;
}

static bool is_spiral(const pc_gradient *g)
{
    return g->d.type == PC_GRAD_SPIRAL_CW || g->d.type == PC_GRAD_SPIRAL_CCW;
}

static bool is_angular(const pc_gradient *g)
{
    return g->d.type == PC_GRAD_CONICAL || is_spiral(g);
}

double pc_gradient_u(const pc_gradient *g, double x, double y)
{
    double dx, dy, a;
    if (!g || g->degenerate) return 1.0;
    dx = x - g->d.start.x;
    dy = y - g->d.start.y;
    switch (g->d.type) {
    case PC_GRAD_LINEAR:
        return (dx * g->vx + dy * g->vy) * g->inv_len2;
    case PC_GRAD_LINEAR_REFLECTED:
        return fabs((dx * g->vx + dy * g->vy) * g->inv_len2);
    case PC_GRAD_LINEAR_DIAMOND:
        return (fabs(dx * g->vx + dy * g->vy) + fabs(dx * g->vy - dy * g->vx)) * g->inv_len2;
    case PC_GRAD_RADIAL:
        return sqrt(dx * dx + dy * dy) * g->inv_len;
    case PC_GRAD_CONICAL:
        a = (dx == 0.0 && dy == 0.0) ? 0.0 : cw_angle(g, dx, dy);
        if (g->d.repeat == PC_GRAD_NO_REPEAT) return a / (2.0 * PI_D);
        if (g->d.repeat == PC_GRAD_REPEAT_WRAPPED) return a / PI_D;
        return a / PI_D + 1.0;
    case PC_GRAD_SPIRAL_CW:
    case PC_GRAD_SPIRAL_CCW:
        a = (dx == 0.0 && dy == 0.0) ? 0.0 : cw_angle(g, dx, dy);
        if (g->d.type == PC_GRAD_SPIRAL_CW) {       /* counter-clockwise angle, [0, 2 pi) */
            if (a > 0.0) a = 2.0 * PI_D - a;
        } else if (a == 0.0 && (dx != 0.0 || dy != 0.0)) {
            a = 2.0 * PI_D;                         /* clockwise angle, (0, 2 pi] */
        }
        return sqrt(dx * dx + dy * dy) * g->inv_len +
               a / (g->d.repeat == PC_GRAD_REPEAT_REFLECTED ? PI_D : 2.0 * PI_D);
    case PC_GRAD_TYPE_COUNT:
        break;
    }
    return 0.0;
}

static double bound_s(const pc_gradient *g, double u)
{
    if (g->degenerate) return 1.0;
    switch (g->d.repeat) {
    case PC_GRAD_REPEAT_WRAPPED:
        return u - floor(u);
    case PC_GRAD_REPEAT_REFLECTED: {
        double m = fmod(fabs(u), 2.0);
        return 1.0 - fabs(m - 1.0);
    }
    case PC_GRAD_NO_REPEAT:
    case PC_GRAD_REPEAT_COUNT:
        break;
    }
    return u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
}

double pc_gradient_s(const pc_gradient *g, double x, double y)
{
    if (!g) return 1.0;
    return bound_s(g, pc_gradient_u(g, x, y));
}

/* s with values within rounding noise of the ends snapped to them, so the
 * pixels on the start and end points get the exact end colors */
static double snap_s(double s)
{
    if (s < 1e-9) return 0.0;
    if (s > 1.0 - 1e-9) return 1.0;
    return s;
}

/* ---- seams --------------------------------------------------------------------- */

/* Distance from d to the ray start + t v, t >= 0 (or the opposite ray). */
static double ray_dist(const pc_gradient *g, double dx, double dy, bool opposite)
{
    double t = (dx * g->vx + dy * g->vy) * g->inv_len2;
    if (opposite) t = -t;
    if (t < 0.0) return sqrt(dx * dx + dy * dy);
    return fabs(dx * g->vy - dy * g->vx) * g->inv_len;
}

/* Can the pixel centered at (cx, cy) straddle a hard seam of s? Seams are
 * whole values of u under REPEAT_WRAPPED, and for the angular types the ray
 * through the end point where the angle restarts (conical No Repeat and
 * Wrapped, spiral No Repeat while the radius term is below 1), plus the
 * opposite ray of a wrapped conical gradient and the center. The extent
 * of u over the pixel is bounded analytically. */
static bool near_seam(const pc_gradient *g, double cx, double cy, double u)
{
    double span, dx, dy, r;
    pc_grad_repeat rep = g->d.repeat;
    if (g->degenerate || !g->d.antialias) return false;
    dx = cx - g->d.start.x;
    dy = cy - g->d.start.y;
    r = sqrt(dx * dx + dy * dy);
    if (is_angular(g)) {
        double k = rep == PC_GRAD_REPEAT_REFLECTED || (!is_spiral(g) && rep != PC_GRAD_NO_REPEAT)
                       ? 1.0 : 2.0;               /* u has angle / (k pi) */
        if (r < 1.5) return true;
        if (rep != PC_GRAD_REPEAT_REFLECTED && !(is_spiral(g) && rep == PC_GRAD_REPEAT_WRAPPED) &&
            ray_dist(g, dx, dy, false) < 0.7072 &&
            (!is_spiral(g) || r * g->inv_len < 1.0 + 0.7072 * g->inv_len))
            return true;
        if (rep != PC_GRAD_REPEAT_WRAPPED) return false;
        span = 0.7072 / (k * PI_D * (r - 0.7072));
        if (is_spiral(g)) span += 0.7072 * g->inv_len;
    } else {
        if (rep != PC_GRAD_REPEAT_WRAPPED) return false;
        switch (g->d.type) {
        case PC_GRAD_LINEAR_DIAMOND:
            span = 2.0 * g->span_lin;
            break;
        case PC_GRAD_RADIAL:
            span = 0.7072 * g->inv_len;
            break;
        default:
            span = g->span_lin;
            break;
        }
    }
    span += 1e-9;
    return floor(u - span) != floor(u + span);
}

/* ---- color evaluation -------------------------------------------------------------- */

/* Triangular-PDF dither noise in (-1, 1), white, per pixel and channel,
 * anchored at the document origin (a hash, so results do not depend on how
 * rows are split between threads). */
static double tpdf(int32_t x, int32_t y, uint32_t ch)
{
    uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u ^ ch * 0xC2B2AE3Du;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return ((double)(h & 0xFFFFu) - (double)(h >> 16)) / 65536.0;
}

/* Quantize a real channel value: round to nearest, after adding dither
 * noise when antialiasing (values that are already whole stay exact). */
static uint8_t quant(double v, bool dither, int32_t x, int32_t y, uint32_t ch)
{
    double f;
    if (dither && fabs(v - floor(v + 0.5)) > 1e-9) v += tpdf(x, y, ch);
    f = floor(v + 0.5);
    if (f <= 0.0) return 0u;
    if (f >= 255.0) return 255u;
    return (uint8_t)f;
}

/* premultiplied color (b*a, g*a, r*a, a) at s */
static void premul_at(const pc_gradient *g, double s, double out[4])
{
    for (int i = 0; i < 4; i++)
        out[i] = (double)g->c0p[i] * (1.0 - s) + (double)g->c1p[i] * s;
}

static pc_px32 to_straight(const double p[4], bool dither, int32_t x, int32_t y)
{
    pc_px32 o = mkpx(0u, 0u, 0u, 0u);
    uint8_t a;
    if (!(p[3] > 0.0)) return o;
    a = quant(p[3], dither, x, y, 3u);
    if (a == 0u) return o;
    o.a = a;
    o.b = quant(p[0] / p[3], dither, x, y, 0u);
    o.g = quant(p[1] / p[3], dither, x, y, 1u);
    o.r = quant(p[2] / p[3], dither, x, y, 2u);
    return o;
}

static pc_px32 color_px(const pc_gradient *g, int32_t x, int32_t y)
{
    double cx = (double)x + 0.5, cy = (double)y + 0.5;
    double u = pc_gradient_u(g, cx, cy), s, p[4];
    if (near_seam(g, cx, cy, u)) {
        double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                double q[4];
                premul_at(g, bound_s(g, pc_gradient_u(g, (double)x + (i + 0.5) / 4.0,
                                                      (double)y + (j + 0.5) / 4.0)), q);
                for (int c = 0; c < 4; c++) acc[c] += q[c];
            }
        for (int c = 0; c < 4; c++) acc[c] /= 16.0;
        return to_straight(acc, true, x, y);
    }
    s = snap_s(bound_s(g, u));
    if (s <= 0.0) return g->d.c0;
    if (s >= 1.0) return g->d.c1;
    /* without antialiasing the ramp position is quantized to 255 steps
     * (truncated) like Paint.NET's aliased gradients */
    if (!g->d.antialias) s = floor(s * 255.0) / 255.0;
    premul_at(g, s, p);
    return to_straight(p, g->d.antialias, x, y);
}

static uint8_t alpha_px(const pc_gradient *g, int32_t x, int32_t y)
{
    double cx = (double)x + 0.5, cy = (double)y + 0.5;
    double u = pc_gradient_u(g, cx, cy), s;
    double a0 = (double)g->d.a0, a1 = (double)g->d.a1;
    if (near_seam(g, cx, cy, u)) {
        double acc = 0.0;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                double si = bound_s(g, pc_gradient_u(g, (double)x + (i + 0.5) / 4.0,
                                                     (double)y + (j + 0.5) / 4.0));
                acc += a0 + (a1 - a0) * si;
            }
        return quant(acc / 16.0, true, x, y, 3u);
    }
    s = snap_s(bound_s(g, u));
    if (s <= 0.0) return g->d.a0;
    if (s >= 1.0) return g->d.a1;
    return quant(a0 + (a1 - a0) * s, g->d.antialias, x, y, 3u);
}

void pc_gradient_row(const pc_gradient *g, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    if (!g || !out) return;
    for (int32_t i = 0; i < n; i++) out[i] = color_px(g, x + i, y);
}

void pc_gradient_alpha_row(const pc_gradient *g, int32_t x, int32_t y, int32_t n,
                           uint8_t *out)
{
    if (!g || !out) return;
    for (int32_t i = 0; i < n; i++) out[i] = alpha_px(g, x + i, y);
}

static void grad_src_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    pc_gradient_row((const pc_gradient *)ud, x, y, n, out);
}

pc_paint_src pc_gradient_paint_src(const pc_gradient *g)
{
    pc_paint_src s;
    s.row = grad_src_row;
    s.ud = (void *)(uintptr_t)g;          /* the callback only reads it */
    s.solid = g ? g->d.c1 : mkpx(0u, 0u, 0u, 255u);
    return s;
}

/* ---- applying ------------------------------------------------------------------- */

#define BAND_ROWS  ((int32_t)PC_TILE_DIM)
#define ONE_CALL_PX ((size_t)1 << 24)       /* render in one atomic call up to 16 Mpx */

static pc_status apply_color(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                             const pc_paint_opts *opts, const pc_par *par, pc_rect area,
                             pc_rect *dirty)
{
    pc_paint_src src = pc_gradient_paint_src(g);
    pc_rect out = pc_rect_make(0, 0, 0, 0), d;
    pc_mask cov;
    size_t npx;
    bool single;
    int32_t y = area.y, y_end = area.y + area.h;
    pc_status st;
    if (!pc_mul_size((size_t)area.w, (size_t)area.h, &npx)) return PC_ERR_LIMIT;
    single = npx <= ONE_CALL_PX;
    st = pc_mask_alloc(&cov, pc_rect_make(area.x, area.y, area.w,
                                          single || area.h < BAND_ROWS ? area.h : BAND_ROWS));
    if (st != PC_OK) return st;
    memset(cov.px, 255, (size_t)cov.stride * (size_t)cov.h);
    while (y < y_end) {
        /* one call, or one call per tile row (area.y >= 0 here) */
        int32_t ye = single ? y_end : (int32_t)(((uint32_t)y >> PC_TILE_SHIFT) + 1u) * BAND_ROWS;
        if (ye > y_end) ye = y_end;
        cov.y = y;
        cov.h = ye - y;
        st = pc_paint_apply(t, layer_id, &cov, &src, opts, par, &d);
        if (st != PC_OK) {
            /* never leave a half-rendered gradient behind */
            if (!pc_rect_is_empty(out)) (void)pc_txn_restore_rect(t, layer_id, out);
            pc_mask_free(&cov);
            return st;
        }
        out = pc_rect_union(out, d);
        y = ye;
    }
    pc_mask_free(&cov);
    if (dirty) *dirty = out;
    return PC_OK;
}

typedef struct alpha_tile {
    pc_rect        r;          /* part of the tile inside the area */
    int32_t        x0, y0;     /* tile origin */
    uint8_t       *dst;
    const uint8_t *orig;
} alpha_tile;

typedef struct alpha_job {
    const pc_gradient *g;
    const pc_doc      *doc;
    bool               use_sel;
    bool               overwrite;
    uint8_t            opacity;
    alpha_tile        *tiles;
} alpha_job;

static void alpha_one(void *ud, uint32_t index, uint32_t worker)
{
    const alpha_job *j = (const alpha_job *)ud;
    const alpha_tile *at = &j->tiles[index];
    uint8_t sel[PC_TILE_PX];
    uint8_t ga[PC_TILE_DIM];
    (void)worker;
    if (j->use_sel)
        pc_sel_read_rect(j->doc, at->r, sel, (size_t)at->r.w, true);
    for (int32_t y = 0; y < at->r.h; y++) {
        int32_t dy = at->r.y + y;
        size_t row = (size_t)(dy - at->y0) * PC_TILE_DIM;
        pc_px32 *dst = (pc_px32 *)(void *)at->dst + row;
        const pc_px32 *org = at->orig ? (const pc_px32 *)(const void *)at->orig + row : NULL;
        pc_gradient_alpha_row(j->g, at->r.x, dy, at->r.w, ga);
        for (int32_t x = 0; x < at->r.w; x++) {
            size_t c = (size_t)(at->r.x + x - at->x0);
            uint32_t k = j->use_sel ? sel[(size_t)y * (size_t)at->r.w + (size_t)x] : 255u;
            pc_px32 o;
            uint32_t target;
            if (k == 0u) continue;
            if (j->opacity != 255u) k = pc_mul255(k, j->opacity);
            if (org) o = org[c]; else o = mkpx(0u, 0u, 0u, 0u);
            target = j->overwrite ? ga[x] : pc_mul255(o.a, ga[x]);
            if (k != 255u) target = ((uint32_t)o.a * (255u - k) + target * k + 127u) / 255u;
            o.a = (uint8_t)target;
            dst[c] = o;
        }
    }
}

static bool any_sel(const pc_doc *d, pc_rect r)
{
    uint8_t buf[PC_TILE_PX];
    pc_sel_read_rect(d, r, buf, (size_t)r.w, true);
    for (size_t i = 0; i < (size_t)r.w * (size_t)r.h; i++)
        if (buf[i]) return true;
    return false;
}

static pc_status apply_alpha(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                             const pc_paint_opts *opts, const pc_par *par, pc_rect area,
                             pc_rect *dirty)
{
    pc_doc *d = pc_txn_doc(t);
    alpha_job j;
    pc_rect out = pc_rect_make(0, 0, 0, 0);
    int32_t tx0 = area.x >> PC_TILE_SHIFT, tx1 = (area.x + area.w - 1) >> PC_TILE_SHIFT;
    int32_t ty0 = area.y >> PC_TILE_SHIFT, ty1 = (area.y + area.h - 1) >> PC_TILE_SHIFT;
    size_t cap, n = 0;
    memset(&j, 0, sizeof j);
    j.g = g;
    j.doc = d;
    j.use_sel = opts->clip_to_selection && pc_sel_is_active(d);
    j.overwrite = opts->mode == PC_PAINT_OVERWRITE;
    j.opacity = opts->opacity;
    if (opts->opacity == 0u) { if (dirty) *dirty = out; return PC_OK; }
    if (!pc_mul_size((size_t)(tx1 - tx0 + 1), (size_t)(ty1 - ty0 + 1), &cap)) return PC_ERR_LIMIT;
    j.tiles = pc_fault_check() ? NULL : (alpha_tile *)calloc(cap, sizeof *j.tiles);
    if (!j.tiles) return PC_ERR_NOMEM;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                                      (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
            pc_rect s = pc_rect_intersect(tr, area);
            if (pc_rect_is_empty(s)) continue;
            if (j.use_sel && !any_sel(d, s)) continue;
            j.tiles[n].r = s;
            j.tiles[n].x0 = tr.x;
            j.tiles[n].y0 = tr.y;
            n++;
        }
    /* private data first (only clones), so failing leaves every pixel as is */
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)((size_t)(j.tiles[i].y0 >> PC_TILE_SHIFT) * d->tiles_x +
                                  (size_t)(j.tiles[i].x0 >> PC_TILE_SHIFT));
        const pc_tile *o = pc_txn_original(t, layer_id, idx);
        if (o && o->bpp != 4u) { free(j.tiles); return PC_ERR_ARG; }
        j.tiles[i].orig = o ? o->data : NULL;
        j.tiles[i].dst = pc_txn_tile_rw(t, layer_id, idx);
        if (!j.tiles[i].dst) { free(j.tiles); return PC_ERR_NOMEM; }
    }
    pc_par_for(par, alpha_one, &j, (uint32_t)n);
    for (size_t i = 0; i < n; i++) out = pc_rect_union(out, j.tiles[i].r);
    free(j.tiles);
    if (dirty) *dirty = out;
    return PC_OK;
}

static pc_status apply_impl(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                            const pc_paint_opts *opts, const pc_par *par, const pc_rect *clip,
                            pc_rect *dirty)
{
    pc_doc *d;
    const pc_layer *l;
    pc_rect area;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!t || !g || !opts) return PC_ERR_ARG;
    if (opts->mode != PC_PAINT_BLEND && opts->mode != PC_PAINT_OVERWRITE) return PC_ERR_ARG;
    if ((unsigned)opts->blend >= (unsigned)PC_BLEND_COUNT) return PC_ERR_ARG;
    if ((unsigned)g->d.type >= (unsigned)PC_GRAD_TYPE_COUNT) return PC_ERR_ARG;
    d = pc_txn_doc(t);
    l = pc_doc_layer_by_id(d, layer_id);
    if (!l || l->tiles_x != d->tiles_x) return PC_ERR_ARG;
    area = opts->clip_to_selection ? pc_sel_extent(d) : pc_doc_rect(d);
    area = pc_rect_intersect(area, pc_doc_rect(d));
    if (clip) area = pc_rect_intersect(area, *clip);
    if (pc_rect_is_empty(area)) return PC_OK;
    if (g->d.mode == PC_GRAD_TRANSPARENCY)
        return apply_alpha(t, layer_id, g, opts, par, area, dirty);
    return apply_color(t, layer_id, g, opts, par, area, dirty);
}

pc_status pc_gradient_apply(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                            const pc_paint_opts *opts, const pc_par *par, pc_rect *dirty)
{
    return apply_impl(t, layer_id, g, opts, par, NULL, dirty);
}

pc_status pc_gradient_apply_rect(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                                 const pc_paint_opts *opts, const pc_par *par, pc_rect clip,
                                 pc_rect *dirty)
{
    return apply_impl(t, layer_id, g, opts, par, &clip, dirty);
}

/* ---- handles ---------------------------------------------------------------------- */

pc_pt pc_gradient_move_handle(const pc_gradient_desc *g, double move_offset)
{
    double vx, vy, l;
    if (!g) return pc_pt_make(0.0, 0.0);
    vx = g->end.x - g->start.x;
    vy = g->end.y - g->start.y;
    l = sqrt(vx * vx + vy * vy);
    if (!(l > 0.0) || !isfinite(l)) return pc_pt_make(g->end.x + move_offset, g->end.y);
    return pc_pt_make(g->end.x + vx / l * move_offset, g->end.y + vy / l * move_offset);
}

static double dist2(pc_pt a, pc_pt b)
{
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

pc_grad_handle pc_gradient_hit(const pc_gradient_desc *g, pc_pt p, double radius,
                               double move_offset)
{
    pc_grad_handle best = PC_GRAD_HANDLE_NONE;
    double bd, r2;
    pc_pt pts[3];
    static const pc_grad_handle ids[3] = {
        PC_GRAD_HANDLE_END, PC_GRAD_HANDLE_START, PC_GRAD_HANDLE_MOVE
    };
    if (!g || !(radius >= 0.0)) return PC_GRAD_HANDLE_NONE;
    pts[0] = g->end;
    pts[1] = g->start;
    pts[2] = pc_gradient_move_handle(g, move_offset);
    r2 = radius * radius;
    bd = r2;
    for (int i = 0; i < 3; i++) {
        double d2 = dist2(p, pts[i]);
        if (d2 <= r2 && (best == PC_GRAD_HANDLE_NONE || d2 < bd)) {
            best = ids[i];
            bd = d2;
        }
    }
    return best;
}

pc_pt pc_gradient_constrain(pc_pt anchor, pc_pt p)
{
    double dx = p.x - anchor.x, dy = p.y - anchor.y;
    double len = sqrt(dx * dx + dy * dy), th;
    if (!(len > 0.0) || !isfinite(len)) return p;
    th = floor(atan2(dy, dx) * 12.0 / PI_D + 0.5) * PI_D / 12.0;
    return pc_pt_make(anchor.x + len * cos(th), anchor.y + len * sin(th));
}

void pc_gradient_measure(const pc_gradient_desc *g, double *angle_deg, double *length)
{
    double dx = 0.0, dy = 0.0, a;
    if (g) {
        dx = g->end.x - g->start.x;
        dy = g->end.y - g->start.y;
    }
    a = (dx == 0.0 && dy == 0.0) ? 0.0 : -atan2(dy, dx) * 180.0 / PI_D;
    if (a <= -180.0) a += 360.0;
    if (angle_deg) *angle_deg = a == 0.0 ? 0.0 : a;    /* no -0 */
    if (length) *length = sqrt(dx * dx + dy * dy);
}
