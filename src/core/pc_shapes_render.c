/* pc_shapes_render.c - the vector renderer shared by the Shapes, Line/Curve
 * and Text tools: banded rasterization of up to PC_VLAYER_MAX coverage
 * layers, 1 px aliased thin lines, layer stacking into one paint source,
 * pixelated selection clipping, and restore-then-repaint bookkeeping on a
 * transaction (lane E3). */
#include "pc/pc_shapes.h"
#include "pc/pc_sel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PC_PI 3.14159265358979323846

/* Upper bound for the band buffers of one draw (all layer masks, the
 * combined mask and the selection rows together). */
#define BAND_BYTES ((size_t)4u << 20)
/* Coordinates are clamped to this before conversion to int32. */
#define COORD_CLAMP 536870912.0          /* 2^29 */
/* Hard cap on dash pieces produced by pc_poly_dash_split. */
#define MAX_DASH_PIECES ((size_t)1u << 22)

struct pc_vrender {
    pc_txn    *txn;          /* transaction of the last draw (identity only) */
    uint32_t   layer_id;     /* layer of the last draw */
    pc_rect    painted;      /* area painted by the last draw */
    pc_raster *ras[PC_VLAYER_MAX];
    uint8_t   *buf;
    size_t     buf_cap;
};

/* ---- small helpers ------------------------------------------------------------ */

pc_handle_metrics pc_handle_metrics_for_zoom(double zoom)
{
    pc_handle_metrics m;
    if (!(zoom > 0.0) || !isfinite(zoom)) zoom = 1.0;
    m.nub_radius = 6.0 / zoom;
    m.handle_offset = 18.0 / zoom;
    m.corridor = 16.0 / zoom;
    return m;
}

double pc_snap_angle(double rad, double step_deg)
{
    double step;
    if (!(step_deg > 0.0) || !isfinite(step_deg) || !isfinite(rad)) return rad;
    step = step_deg * PC_PI / 180.0;
    return floor(rad / step + 0.5) * step;
}

double pc_snap_stroke_coord(double v, double width)
{
    bool odd = false;
    if (!isfinite(v)) return v;
    if (!(width > 1.0)) {
        odd = true;
    } else if (isfinite(width) && fabs(width - floor(width + 0.5)) < 1e-9) {
        double half = floor(width + 0.5) * 0.5;
        odd = half != floor(half);
    }
    return odd ? floor(v) + 0.5 : floor(v + 0.5);
}

pc_vdraw_opts pc_vdraw_opts_default(void)
{
    pc_vdraw_opts o;
    o.paint = pc_paint_opts_default();
    o.antialias = true;
    o.clip_pixelated = false;
    return o;
}

static int32_t clamp_i32(double v)
{
    if (!(v > -COORD_CLAMP)) return -(int32_t)COORD_CLAMP;
    if (v > COORD_CLAMP) return (int32_t)COORD_CLAMP;
    return (int32_t)v;
}

pc_rect pc_vlayer_bounds(const pc_vlayer *layers, size_t n)
{
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    bool any = false;
    pc_rect r = pc_rect_make(0, 0, 0, 0);
    if (!layers) return r;
    for (size_t i = 0; i < n; i++) {
        for (int part = 0; part < 2; part++) {
            const pc_poly *p = part ? layers[i].thin : layers[i].fill;
            pc_pt mn, mx;
            double g = part ? 1.0 : 0.0;
            if (!p || !p->n_pts || !pc_poly_bounds(p, &mn, &mx)) continue;
            mn.x -= g; mn.y -= g; mx.x += g; mx.y += g;
            if (!any) {
                x0 = mn.x; y0 = mn.y; x1 = mx.x; y1 = mx.y;
                any = true;
            } else {
                if (mn.x < x0) x0 = mn.x;
                if (mn.y < y0) y0 = mn.y;
                if (mx.x > x1) x1 = mx.x;
                if (mx.y > y1) y1 = mx.y;
            }
        }
    }
    if (!any || !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return r;
    r.x = clamp_i32(floor(x0));
    r.y = clamp_i32(floor(y0));
    r.w = clamp_i32(ceil(x1)) - r.x;
    r.h = clamp_i32(ceil(y1)) - r.y;
    if (r.w <= 0 || r.h <= 0) {
        /* a degenerate (zero area) extent still may plot thin pixels */
        if (r.w <= 0) r.w = 1;
        if (r.h <= 0) r.h = 1;
    }
    return r;
}

/* ---- thin lines ------------------------------------------------------------------ */

static void plot(const pc_mask *m, double fx, double fy)
{
    int64_t x, y;
    if (!(fx >= (double)m->x) || !(fx < (double)m->x + (double)m->w)) return;
    if (!(fy >= (double)m->y) || !(fy < (double)m->y + (double)m->h)) return;
    x = (int64_t)fx - m->x;
    y = (int64_t)fy - m->y;
    m->px[(size_t)y * (size_t)m->stride + (size_t)x] = 255u;
}

/* Pixel samples of the segment a -> b: one per column (x-major) or row
 * (y-major) whose center lies in [a, b) along the direction of travel, or
 * [a, b] with include_end. Returns the number of samples before clipping
 * to the mask (pixels outside the mask are skipped). */
static size_t thin_seg(const pc_mask *m, pc_pt a, pc_pt b, bool include_end)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    bool xmaj;
    double c0, c1, s, lo, hi;
    size_t count;
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(a.x) || !isfinite(a.y)) return 0u;
    if (dx == 0.0 && dy == 0.0) {
        if (!include_end) return 0u;
        plot(m, floor(a.x), floor(a.y));
        return 1u;
    }
    xmaj = fabs(dx) >= fabs(dy);
    if (!xmaj) {
        /* swap the roles of x and y */
        double t = a.x; a.x = a.y; a.y = t;
        t = b.x; b.x = b.y; b.y = t;
        t = dx; dx = dy; dy = t;
    }
    s = dy / dx;
    if (dx > 0.0) {
        c0 = ceil(a.x - 0.5);
        c1 = include_end ? floor(b.x - 0.5) : ceil(b.x - 0.5) - 1.0;
    } else {
        c1 = floor(a.x - 0.5);
        c0 = include_end ? ceil(b.x - 0.5) : floor(b.x - 0.5) + 1.0;
    }
    if (c0 > c1) return 0u;
    count = (c1 - c0 + 1.0) > 4e9 ? (size_t)4000000000u : (size_t)(c1 - c0 + 1.0);
    /* clip the major axis to the mask, and (x-major) to the band rows */
    lo = xmaj ? (double)m->x : (double)m->y;
    hi = xmaj ? (double)m->x + (double)m->w - 1.0 : (double)m->y + (double)m->h - 1.0;
    if (xmaj) {
        if (s != 0.0) {
            double xa = a.x + ((double)m->y - 1.0 - a.y) / s;
            double xb = a.x + ((double)m->y + (double)m->h + 1.0 - a.y) / s;
            double xmn = (xa < xb ? xa : xb) - 1.0, xmx = (xa < xb ? xb : xa) + 1.0;
            if (floor(xmn) > lo) lo = floor(xmn);
            if (ceil(xmx) < hi) hi = ceil(xmx);
        } else {
            double fy = floor(a.y);
            if (fy < (double)m->y || fy >= (double)m->y + (double)m->h) return count;
        }
    }
    if (c0 < lo) c0 = lo;
    if (c1 > hi) c1 = hi;
    if (c0 > c1) return count;
    for (int64_t c = (int64_t)c0; c <= (int64_t)c1; c++) {
        double v = a.y + ((double)c + 0.5 - a.x) * s;
        if (xmaj) plot(m, (double)c, floor(v));
        else plot(m, floor(v), (double)c);
    }
    return count;
}

static void thin_poly(const pc_mask *m, const pc_poly *p)
{
    for (size_t ci = 0; ci < p->n_contours; ci++) {
        size_t s = pc_poly_contour_start(p, ci), e = p->ends[ci], count = 0;
        bool closed = p->closed[ci] && e - s > 2u;
        bool end = !closed && !(p->flags[s] & PC_PT_THIN_NO_END);
        if (e <= s) continue;
        for (size_t i = s; i + 1u < e; i++)
            count += thin_seg(m, p->pts[i], p->pts[i + 1u], end && i + 2u == e);
        if (closed) count += thin_seg(m, p->pts[e - 1u], p->pts[s], false);
        /* a contour too short for any sample still shows one pixel */
        if (count == 0u) plot(m, floor(p->pts[s].x), floor(p->pts[s].y));
    }
}

/* ---- dash splitting ------------------------------------------------------------ */

pc_status pc_poly_dash_split(const pc_poly *src, const double *dash, size_t n_dash,
                             double offset, pc_poly *dst)
{
    double dl[2u * PC_DASH_MAX], total = 0.0, len = 0.0;
    size_t nd;
    pc_status st = PC_OK;
    if (!src || !dst || src == dst || !dash || n_dash == 0u || n_dash > PC_DASH_MAX ||
        !isfinite(offset))
        return PC_ERR_ARG;
    for (size_t i = 0; i < n_dash; i++) {
        if (!(dash[i] >= 0.0) || !isfinite(dash[i])) return PC_ERR_ARG;
        dl[i] = dash[i];
        total += dash[i];
    }
    if (!(total > 0.0)) return PC_ERR_ARG;
    nd = n_dash;
    if (nd & 1u) {
        for (size_t i = 0; i < n_dash; i++) dl[n_dash + i] = dl[i];
        nd *= 2u;
        total *= 2.0;
    }
    /* bound the number of pieces before producing them (P-08) */
    for (size_t ci = 0; ci < src->n_contours; ci++) {
        size_t s = pc_poly_contour_start(src, ci), e = src->ends[ci];
        for (size_t i = s; i + 1u < e; i++) {
            double dx = src->pts[i + 1u].x - src->pts[i].x, dy = src->pts[i + 1u].y - src->pts[i].y;
            len += sqrt(dx * dx + dy * dy);
        }
        if (src->closed[ci] && e - s > 2u) {
            double dx = src->pts[s].x - src->pts[e - 1u].x, dy = src->pts[s].y - src->pts[e - 1u].y;
            len += sqrt(dx * dx + dy * dy);
        }
    }
    if (!isfinite(len)) return PC_ERR_ARG;
    if (len / total * (double)nd > (double)MAX_DASH_PIECES) return PC_ERR_LIMIT;
    dst->n_pts = dst->open_start;    /* drop an unfinished contour */
    for (size_t ci = 0; ci < src->n_contours && st == PC_OK; ci++) {
        size_t s = pc_poly_contour_start(src, ci), e = src->ends[ci];
        size_t cnt = e - s, nseg, k = 0;
        double phase = fmod(offset, total), rem;
        bool on, open = false;
        if (cnt == 0u) continue;
        if (phase < 0.0) phase += total;
        while (phase >= dl[k] && phase > 0.0) {
            phase -= dl[k];
            k = (k + 1u) % nd;
        }
        rem = dl[k] - phase;
        on = (k & 1u) == 0u;
        if (cnt == 1u) {
            if (on) {
                st = pc_poly_add(dst, src->pts[s], 0u);
                if (st == PC_OK) st = pc_poly_end(dst, false);
            }
            continue;
        }
        nseg = (src->closed[ci] && cnt > 2u) ? cnt : cnt - 1u;
        if (on) {
            st = pc_poly_add(dst, src->pts[s], PC_PT_THIN_NO_END);
            open = true;
        }
        for (size_t i = 0; i < nseg && st == PC_OK; i++) {
            pc_pt a = src->pts[s + i], b = src->pts[s + (i + 1u) % cnt];
            double dx = b.x - a.x, dy = b.y - a.y, l = sqrt(dx * dx + dy * dy), pos = 0.0;
            if (l == 0.0) continue;
            /* every interval that ends inside this segment */
            while (st == PC_OK && rem <= l - pos) {
                pc_pt q;
                pos += rem;
                q = pos >= l ? b : pc_pt_make(a.x + dx * (pos / l), a.y + dy * (pos / l));
                if (on && open) {
                    st = pc_poly_add(dst, q, 0u);
                    if (st == PC_OK) st = pc_poly_end(dst, false);
                    open = false;
                }
                k = (k + 1u) % nd;
                rem = dl[k];
                on = (k & 1u) == 0u;
                if (st == PC_OK && on) {
                    st = pc_poly_add(dst, q, PC_PT_THIN_NO_END);
                    open = true;
                }
            }
            if (st != PC_OK) break;
            rem -= l - pos;
            if (on && open && pos < l) st = pc_poly_add(dst, b, 0u);
        }
        if (st == PC_OK && open) {
            /* a dash that would start exactly at the end has no length */
            if (dst->n_pts - dst->open_start < 2u) dst->n_pts = dst->open_start;
            else st = pc_poly_end(dst, false);
        }
    }
    if (st != PC_OK) dst->n_pts = dst->open_start;
    return st;
}

/* ---- layer stacking ---------------------------------------------------------------- */

typedef struct comb_src {
    const uint8_t      *mask[PC_VLAYER_MAX];
    const pc_paint_src *src[PC_VLAYER_MAX];
    size_t              n;
    int32_t             bx, by;
    size_t              stride;
    bool                overwrite;
} comb_src;

static void fetch(const pc_paint_src *s, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    if (!s) {
        pc_px32 k;
        k.b = 0u; k.g = 0u; k.r = 0u; k.a = 255u;
        for (int32_t i = 0; i < n; i++) out[i] = k;
    } else if (s->row) {
        s->row(s->ud, x, y, n, out);
    } else {
        for (int32_t i = 0; i < n; i++) out[i] = s->solid;
    }
}

static uint8_t to_u8(double v)
{
    if (!(v > 0.0)) return 0u;
    if (v >= 255.0) return 255u;
    return (uint8_t)(v + 0.5);
}

/* "over" stack of the layers in BLEND mode, straight alpha out. */
static pc_px32 mix_over(const uint8_t *k, const pc_px32 *p, size_t n)
{
    pc_px32 o;
    double A = 0.0, cb = 0.0, cg = 0.0, cr = 0.0;
    size_t cnt = 0, last = 0;
    memset(&o, 0, sizeof o);
    for (size_t i = 0; i < n; i++)
        if (k[i]) { cnt++; last = i; }
    if (cnt == 0u) return o;
    if (cnt == 1u) {
        o = p[last];
        o.a = (uint8_t)pc_mul255(o.a, k[last]);
        return o;
    }
    for (size_t i = 0; i < n; i++) {
        double a;
        if (!k[i]) continue;
        a = (double)p[i].a * (double)k[i] / (255.0 * 255.0);
        cb = (double)p[i].b * a + cb * (1.0 - a);
        cg = (double)p[i].g * a + cg * (1.0 - a);
        cr = (double)p[i].r * a + cr * (1.0 - a);
        A = a + A * (1.0 - a);
    }
    o.a = to_u8(A * 255.0);
    if (o.a == 0u || !(A > 0.0)) {
        memset(&o, 0, sizeof o);
        return o;
    }
    o.b = to_u8(cb / A);
    o.g = to_u8(cg / A);
    o.r = to_u8(cr / A);
    return o;
}

/* Lerp chain of the layers in OVERWRITE mode expressed as one paint Q
 * applied with coverage K = 1 - prod(1 - k_i) (computed in comb_cov). */
static pc_px32 mix_lerp(const uint8_t *k, const pc_px32 *p, size_t n)
{
    pc_px32 o;
    double wsum = 0.0, qa = 0.0, qb = 0.0, qg = 0.0, qr = 0.0, keep = 1.0;
    size_t cnt = 0, last = 0;
    memset(&o, 0, sizeof o);
    for (size_t i = 0; i < n; i++)
        if (k[i]) { cnt++; last = i; }
    if (cnt == 0u) return o;
    if (cnt == 1u) return p[last];
    for (size_t i = n; i-- > 0u;) {
        double t = (double)k[i] / 255.0, w = t * keep;
        wsum += w;
        qa += (double)p[i].a * w;
        qb += (double)p[i].b * (double)p[i].a * w;
        qg += (double)p[i].g * (double)p[i].a * w;
        qr += (double)p[i].r * (double)p[i].a * w;
        keep *= 1.0 - t;
    }
    if (!(wsum > 0.0) || !(qa > 0.0)) return o;
    o.a = to_u8(qa / wsum);
    if (o.a == 0u) return o;
    o.b = to_u8(qb / qa);
    o.g = to_u8(qg / qa);
    o.r = to_u8(qr / qa);
    return o;
}

static void comb_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const comb_src *c = (const comb_src *)ud;
    pc_px32 buf[PC_VLAYER_MAX][64];
    int32_t done = 0;
    while (done < n) {
        int32_t m = n - done > 64 ? 64 : n - done;
        size_t row = (size_t)(y - c->by) * c->stride;
        for (size_t i = 0; i < c->n; i++) fetch(c->src[i], x + done, y, m, buf[i]);
        for (int32_t j = 0; j < m; j++) {
            size_t off = row + (size_t)(x + done + j - c->bx);
            uint8_t k[PC_VLAYER_MAX];
            pc_px32 p[PC_VLAYER_MAX];
            for (size_t i = 0; i < c->n; i++) {
                k[i] = c->mask[i][off];
                p[i] = buf[i][j];
            }
            out[done + j] = c->overwrite ? mix_lerp(k, p, c->n) : mix_over(k, p, c->n);
        }
        done += m;
    }
}

/* Combined coverage for the stacked layers. */
static void comb_cov(const comb_src *c, uint8_t *cov, size_t count)
{
    for (size_t off = 0; off < count; off++) {
        size_t cnt = 0, last = 0;
        for (size_t i = 0; i < c->n; i++)
            if (c->mask[i][off]) { cnt++; last = i; }
        if (cnt == 0u) {
            cov[off] = 0u;
        } else if (!c->overwrite) {
            cov[off] = 255u;
        } else if (cnt == 1u) {
            cov[off] = c->mask[last][off];
        } else {
            double keep = 1.0;
            for (size_t i = 0; i < c->n; i++) keep *= 1.0 - (double)c->mask[i][off] / 255.0;
            cov[off] = to_u8((1.0 - keep) * 255.0);
        }
    }
}

/* ---- renderer ------------------------------------------------------------------------- */

pc_vrender *pc_vrender_create(void)
{
    return (pc_vrender *)calloc(1u, sizeof(pc_vrender));
}

void pc_vrender_destroy(pc_vrender *vr)
{
    if (!vr) return;
    for (size_t i = 0; i < PC_VLAYER_MAX; i++) pc_raster_destroy(vr->ras[i]);
    free(vr->buf);
    free(vr);
}

void pc_vrender_reset(pc_vrender *vr)
{
    if (!vr) return;
    vr->txn = NULL;
    vr->painted = pc_rect_make(0, 0, 0, 0);
}

pc_rect pc_vrender_painted(const pc_vrender *vr)
{
    return vr ? vr->painted : pc_rect_make(0, 0, 0, 0);
}

pc_status pc_vrender_clear(pc_vrender *vr, pc_txn *t, pc_rect *dirty)
{
    pc_rect old;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!vr || !t) return PC_ERR_ARG;
    if (vr->txn != t) {
        vr->txn = t;
        vr->painted = pc_rect_make(0, 0, 0, 0);
        return PC_OK;
    }
    old = vr->painted;
    if (!pc_rect_is_empty(old)) {
        pc_status st = pc_txn_restore_rect(t, vr->layer_id, old);
        if (st != PC_OK) return st;
    }
    vr->painted = pc_rect_make(0, 0, 0, 0);
    if (dirty) *dirty = old;
    return PC_OK;
}

static bool valid_opts(const pc_vdraw_opts *o)
{
    if (!o) return false;
    if (o->paint.mode != PC_PAINT_BLEND && o->paint.mode != PC_PAINT_OVERWRITE &&
        o->paint.mode != PC_PAINT_ERASE)
        return false;
    return (unsigned)o->paint.blend < (unsigned)PC_BLEND_COUNT;
}

static pc_status grow(uint8_t **buf, size_t *cap, size_t need)
{
    uint8_t *nb;
    if (need <= *cap) return PC_OK;
    nb = (uint8_t *)realloc(*buf, need);
    if (!nb) return PC_ERR_NOMEM;
    *buf = nb;
    *cap = need;
    return PC_OK;
}

static pc_status load_edges(pc_raster **slot, const pc_poly *p)
{
    if (!*slot) {
        *slot = pc_raster_create();
        if (!*slot) return PC_ERR_NOMEM;
    }
    pc_raster_reset(*slot);
    return pc_raster_add_poly(*slot, p, NULL);
}

static void threshold_clip(const pc_doc *d, pc_rect band, uint8_t *sel, uint8_t *cov)
{
    size_t count = (size_t)band.w * (size_t)band.h;
    pc_sel_read_rect(d, band, sel, (size_t)band.w, true);
    for (size_t i = 0; i < count; i++)
        if (sel[i] < 128u) cov[i] = 0u;
}

pc_status pc_vrender_draw(pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                          const pc_vlayer *layers, size_t n,
                          const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty)
{
    pc_doc *d;
    pc_rect old, area;
    pc_paint_opts po;
    size_t rows, plane, nplanes, need;
    bool pix_clip, has_fill[PC_VLAYER_MAX];
    pc_status st = PC_OK;

    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!vr || !t || !layers || n == 0u || n > PC_VLAYER_MAX || !valid_opts(o))
        return PC_ERR_ARG;
    d = pc_txn_doc(t);
    if (!pc_doc_layer_by_id(d, layer_id)) return PC_ERR_ARG;
    if (vr->txn != t) {
        vr->txn = t;
        vr->painted = pc_rect_make(0, 0, 0, 0);
    }
    old = vr->painted;
    if (!pc_rect_is_empty(old)) {
        st = pc_txn_restore_rect(t, vr->layer_id, old);
        if (st != PC_OK) return st;
    }
    vr->painted = pc_rect_make(0, 0, 0, 0);
    vr->layer_id = layer_id;
    if (dirty) *dirty = old;

    po = o->paint;
    pix_clip = po.clip_to_selection && o->clip_pixelated && pc_sel_is_active(d);
    if (pix_clip) po.clip_to_selection = false;

    area = pc_rect_intersect(pc_vlayer_bounds(layers, n), pc_doc_rect(d));
    if (o->paint.clip_to_selection && pc_sel_is_active(d))
        area = pc_rect_intersect(area, pc_sel_bounds(d));
    if (pc_rect_is_empty(area)) return PC_OK;

    for (size_t i = 0; i < n; i++) {
        has_fill[i] = layers[i].fill && layers[i].fill->n_pts > 0u;
        if (has_fill[i]) {
            st = load_edges(&vr->ras[i], layers[i].fill);
            if (st != PC_OK) return st;
        }
    }

    /* band height: all planes together stay within BAND_BYTES */
    nplanes = n + (n > 1u ? 1u : 0u) + (pix_clip ? 1u : 0u);
    rows = BAND_BYTES / ((size_t)area.w * nplanes);
    if (rows < 1u) rows = 1u;
    if (rows >= PC_TILE_DIM) rows -= rows % PC_TILE_DIM;
    if (rows > (size_t)area.h) rows = (size_t)area.h;
    if (!pc_mul_size((size_t)area.w, rows, &plane) || !pc_mul_size(plane, nplanes, &need))
        return PC_ERR_LIMIT;
    st = grow(&vr->buf, &vr->buf_cap, need);
    if (st != PC_OK) return st;

    for (int32_t y = area.y; y < area.y + area.h && st == PC_OK;) {
        int32_t yend = area.y + area.h, bh;
        pc_rect band, dr;
        pc_mask m[PC_VLAYER_MAX], cov;
        comb_src cs;
        pc_paint_src comb;
        const pc_paint_src *src;
        size_t count;
        if (rows >= PC_TILE_DIM) {
            int64_t lim = ((int64_t)y >> PC_TILE_SHIFT << PC_TILE_SHIFT) + (int64_t)rows;
            if (lim < (int64_t)yend) yend = (int32_t)lim;
        } else if ((int64_t)y + (int64_t)rows < (int64_t)yend) {
            yend = y + (int32_t)rows;
        }
        bh = yend - y;
        band = pc_rect_make(area.x, y, area.w, bh);
        count = (size_t)area.w * (size_t)bh;
        for (size_t i = 0; i < n && st == PC_OK; i++) {
            m[i].px = vr->buf + i * plane;
            m[i].x = band.x;
            m[i].y = band.y;
            m[i].w = band.w;
            m[i].h = band.h;
            m[i].stride = band.w;
            if (has_fill[i]) st = pc_raster_fill(vr->ras[i], &m[i], layers[i].rule, o->antialias);
            else memset(m[i].px, 0, count);
            if (st == PC_OK && layers[i].thin) thin_poly(&m[i], layers[i].thin);
        }
        if (st != PC_OK) break;
        if (n == 1u) {
            cov = m[0];
            src = layers[0].src;
        } else {
            memset(&cs, 0, sizeof cs);
            for (size_t i = 0; i < n; i++) {
                cs.mask[i] = m[i].px;
                cs.src[i] = layers[i].src;
            }
            cs.n = n;
            cs.bx = band.x;
            cs.by = band.y;
            cs.stride = (size_t)band.w;
            cs.overwrite = o->paint.mode != PC_PAINT_BLEND;
            cov = m[0];
            cov.px = vr->buf + n * plane;
            comb_cov(&cs, cov.px, count);
            comb.row = comb_row;
            comb.ud = &cs;
            memset(&comb.solid, 0, sizeof comb.solid);
            src = &comb;
        }
        if (pix_clip)
            threshold_clip(d, band, vr->buf + (nplanes - 1u) * plane, cov.px);
        st = pc_paint_apply(t, layer_id, &cov, src, &po, par, &dr);
        if (st == PC_OK) vr->painted = pc_rect_union(vr->painted, dr);
        y = yend;
    }
    if (dirty) *dirty = pc_rect_union(old, vr->painted);
    return st;
}

pc_status pc_vlayer_coverage(const pc_vlayer *layers, size_t n, bool antialias,
                             const pc_mask *dst)
{
    pc_raster *r;
    pc_mask tmp;
    size_t bytes;
    pc_status st = PC_OK;
    if (!layers || n > PC_VLAYER_MAX || !dst || !dst->px || dst->w <= 0 || dst->h <= 0 ||
        dst->stride < dst->w)
        return PC_ERR_ARG;
    for (int32_t y = 0; y < dst->h; y++)
        memset(dst->px + (size_t)y * (size_t)dst->stride, 0, (size_t)dst->w);
    if (!pc_mul_size((size_t)dst->w, (size_t)dst->h, &bytes)) return PC_ERR_LIMIT;
    r = pc_raster_create();
    if (!r) return PC_ERR_NOMEM;
    tmp = *dst;
    tmp.stride = dst->w;
    tmp.px = (uint8_t *)malloc(bytes);
    if (!tmp.px) {
        pc_raster_destroy(r);
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (layers[i].fill && layers[i].fill->n_pts) {
            pc_raster_reset(r);
            st = pc_raster_add_poly(r, layers[i].fill, NULL);
            if (st == PC_OK) st = pc_raster_fill(r, &tmp, layers[i].rule, antialias);
            if (st != PC_OK) break;
            for (int32_t y = 0; y < dst->h; y++) {
                uint8_t *o = dst->px + (size_t)y * (size_t)dst->stride;
                const uint8_t *s = tmp.px + (size_t)y * (size_t)tmp.w;
                for (int32_t x = 0; x < dst->w; x++)
                    if (s[x] > o[x]) o[x] = s[x];
            }
        }
        if (layers[i].thin) thin_poly(dst, layers[i].thin);
    }
    free(tmp.px);
    pc_raster_destroy(r);
    return st;
}
