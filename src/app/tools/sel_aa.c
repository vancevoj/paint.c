/* sel_aa.c - antialiased selection shapes with 4 x 4 supersampling (lane
 * TOOLA, TOOLS.md T-SEL-QUALITY, R 4.3): the coverage of a pixel is the
 * share of 16 sample points (x + (i + 0.5) / 4, y + (j + 0.5) / 4) inside
 * the polygon, so edges get Paint.NET's 17 coverage levels instead of the
 * exact area of the analytic rasterizer. See sel_common.h (sel_ss).
 *
 * The polygon is reduced once to sorted crossings per sample row (a
 * scanline table), so every tile fill and uniform test only walks the
 * crossings of its own rows. Sizes are bounded before allocating (P-08).
 * Main thread (any thread for a built table: it is read only). */
#include "sel_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SS            4                    /* samples per pixel side */
#define SS_MAX_XINGS  ((size_t)1 << 25)    /* crossing budget (about 200 MB worst case) */

typedef struct ss_xing {
    float  x;
    int8_t dir;
} ss_xing;

static int cmp_xing(const void *pa, const void *pb)
{
    const ss_xing *a = (const ss_xing *)pa, *b = (const ss_xing *)pb;
    return a->x < b->x ? -1 : (a->x > b->x ? 1 : 0);
}

void sel_ss_free(sel_ss *s)
{
    if (!s) return;
    free(s->off);
    free(s->xs);
    memset(s, 0, sizeof *s);
}

/* Sub-row k samples document y = y0 + (k + 0.5) / SS; the rows an edge
 * from ya to yb (ya < yb) crosses are [ceil(SS * (ya - y0) - 0.5), ...). */
static int64_t first_row(double y, int32_t y0)
{
    return (int64_t)ceil(((double)SS) * (y - (double)y0) - 0.5);
}

/* Walk every edge of p, calling back with each crossing (pass 2) or only
 * counting them (pass 1, out == NULL). */
static void edges(sel_ss *s, const pc_poly *p, uint32_t *fill_pos, ss_xing *out)
{
    size_t start = 0;
    for (size_t c = 0; c < p->n_contours; c++) {
        size_t end = p->ends[c], n = end - start;
        if (n >= 2u) {
            for (size_t i = 0; i < n; i++) {
                pc_pt a = p->pts[start + i], b = p->pts[start + (i + 1u) % n];
                double ya, yb, xa, xb;
                int64_t k0, k1;
                int8_t dir;
                if (!(a.y == a.y) || !(b.y == b.y) || a.y == b.y) continue;
                if (a.y < b.y) {
                    ya = a.y; yb = b.y; xa = a.x; xb = b.x; dir = 1;
                } else {
                    ya = b.y; yb = a.y; xa = b.x; xb = a.x; dir = -1;
                }
                k0 = first_row(ya, s->y0);
                k1 = first_row(yb, s->y0);
                if (k0 < 0) k0 = 0;
                if (k1 > (int64_t)s->nsub) k1 = (int64_t)s->nsub;
                for (int64_t k = k0; k < k1; k++) {
                    if (out) {
                        double yc = (double)s->y0 + ((double)k + 0.5) / (double)SS;
                        double t = (yc - ya) / (yb - ya);
                        ss_xing *x = &out[fill_pos[k]++];
                        x->x = (float)(xa + (xb - xa) * t);
                        x->dir = dir;
                    } else {
                        fill_pos[k]++;
                    }
                }
            }
        }
        start = end;
    }
}

/* Samples [s0, s1) of a sub-row inside the shape between document sample
 * columns c0 and c1 (column s is at x = (s + 0.5) / SS). Calls span(lo,
 * hi) for each inside run. */
typedef void (*ss_span_fn)(void *ud, int64_t lo, int64_t hi);

static void row_spans(const sel_ss *s, int32_t k, int64_t c0, int64_t c1, ss_span_fn fn,
                      void *ud)
{
    const ss_xing *x = s->xs + s->off[k];
    uint32_t n = s->off[k + 1] - s->off[k];
    int w = 0;
    for (uint32_t i = 0; i + 1u <= n; i++) {
        bool in;
        w += x[i].dir;
        in = s->evenodd ? (w & 1) != 0 : w != 0;
        if (in && i + 1u < n) {
            int64_t lo = (int64_t)ceil((double)SS * (double)x[i].x - 0.5);
            int64_t hi = (int64_t)ceil((double)SS * (double)x[i + 1u].x - 0.5);
            if (lo < c0) lo = c0;
            if (hi > c1) hi = c1;
            if (hi > lo) fn(ud, lo, hi);
        }
    }
}

static void add_area(void *ud, int64_t lo, int64_t hi) { *(double *)ud += (double)(hi - lo); }

pc_status sel_ss_build(sel_ss *s, const pc_poly *p, pc_fill_rule rule, const pc_doc *d)
{
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    size_t total = 0, rows;
    uint32_t *pos = NULL;
    pc_rect b;
    memset(s, 0, sizeof *s);
    s->evenodd = rule == PC_FILL_EVENODD;
    if (!p || !d) return PC_ERR_ARG;
    for (size_t i = 0; i < p->n_pts; i++) {
        if (!(p->pts[i].x == p->pts[i].x) || !(p->pts[i].y == p->pts[i].y)) continue;
        mnx = fmin(mnx, p->pts[i].x);
        mny = fmin(mny, p->pts[i].y);
        mxx = fmax(mxx, p->pts[i].x);
        mxy = fmax(mxy, p->pts[i].y);
    }
    if (!(mxx > mnx) || !(mxy > mny)) return PC_OK;            /* empty */
    mnx = floor(sel_clampd(mnx));
    mny = floor(sel_clampd(mny));
    mxx = ceil(sel_clampd(mxx));
    mxy = ceil(sel_clampd(mxy));
    b = pc_rect_intersect(pc_rect_make((int32_t)mnx, (int32_t)mny, (int32_t)(mxx - mnx),
                                       (int32_t)(mxy - mny)),
                          pc_doc_rect(d));
    if (pc_rect_is_empty(b)) return PC_OK;
    s->bounds = b;
    s->y0 = b.y;
    s->nsub = b.h * SS;
    if (!pc_mul_size((size_t)s->nsub, 1u, &rows) || !pc_add_size(rows, 1u, &rows))
        return PC_ERR_LIMIT;
    s->off = (uint32_t *)calloc(rows, sizeof *s->off);
    pos = (uint32_t *)calloc(rows, sizeof *pos);
    if (!s->off || !pos) {
        free(pos);
        sel_ss_free(s);
        return PC_ERR_NOMEM;
    }
    /* pass 1: crossings per sub-row (bounded before the table is allocated) */
    edges(s, p, pos, NULL);
    for (int32_t k = 0; k < s->nsub; k++) {
        if (!pc_add_size(total, pos[k], &total) || total > SS_MAX_XINGS) {
            free(pos);
            sel_ss_free(s);
            return PC_ERR_LIMIT;
        }
        s->off[k + 1] = (uint32_t)total;
    }
    {
        size_t bytes;
        if (!pc_mul_size(total ? total : 1u, sizeof(ss_xing), &bytes)) {
            free(pos);
            sel_ss_free(s);
            return PC_ERR_LIMIT;
        }
        s->xs = (ss_xing *)malloc(bytes);
    }
    if (!s->xs) {
        free(pos);
        sel_ss_free(s);
        return PC_ERR_NOMEM;
    }
    /* pass 2: the crossings, then sorted per sub-row */
    for (int32_t k = 0; k < s->nsub; k++) pos[k] = s->off[k];
    edges(s, p, pos, s->xs);
    free(pos);
    for (int32_t k = 0; k < s->nsub; k++) {
        ss_xing *x = s->xs + s->off[k];
        uint32_t n = s->off[k + 1] - s->off[k];
        if (n > 24u) {
            qsort(x, n, sizeof *x, cmp_xing);
        } else {
            for (uint32_t i = 1; i < n; i++) {
                ss_xing v = x[i];
                uint32_t j = i;
                while (j > 0 && x[j - 1u].x > v.x) {
                    x[j] = x[j - 1u];
                    j--;
                }
                x[j] = v;
            }
        }
    }
    /* covered area in pixels, inside the document */
    {
        double samples = 0.0;
        for (int32_t k = 0; k < s->nsub; k++)
            row_spans(s, k, (int64_t)b.x * SS, (int64_t)(b.x + b.w) * SS, add_area, &samples);
        s->area = samples / (double)(SS * SS);
    }
    return PC_OK;
}

typedef struct ss_acc {
    uint8_t *cnt;          /* samples per pixel of the row (0..16) */
    int64_t  c0;           /* sample column of cnt[0] */
} ss_acc;

static void acc_span(void *ud, int64_t lo, int64_t hi)
{
    ss_acc *a = (ss_acc *)ud;
    for (int64_t s = lo; s < hi;) {
        int64_t px = (s - a->c0) / SS, next = a->c0 + (px + 1) * SS;
        int64_t e = next < hi ? next : hi;
        a->cnt[px] = (uint8_t)(a->cnt[px] + (uint8_t)(e - s));
        s = e;
    }
}

static void ss_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    const sel_ss *s = (const sel_ss *)ud;
    uint8_t cnt[64];
    for (int32_t x0 = 0; x0 < r.w; x0 += 64) {
        int32_t w = r.w - x0 < 64 ? r.w - x0 : 64;
        for (int32_t y = 0; y < r.h; y++) {
            int32_t dy = r.y + y;
            uint8_t *row = dst + (size_t)y * stride + (size_t)x0;
            ss_acc acc;
            memset(cnt, 0, sizeof cnt);
            acc.cnt = cnt;
            acc.c0 = (int64_t)(r.x + x0) * SS;
            if (dy >= s->bounds.y && dy < s->bounds.y + s->bounds.h && s->xs) {
                int32_t k0 = (dy - s->y0) * SS;
                for (int32_t k = k0; k < k0 + SS; k++)
                    row_spans(s, k, acc.c0, acc.c0 + (int64_t)w * SS, acc_span, &acc);
            }
            for (int32_t x = 0; x < w; x++)
                row[x] = (uint8_t)(((uint32_t)cnt[x] * 255u + 8u) / 16u);
        }
    }
}

/* 0 or 255 when every sample of r is outside or inside, else -1. */
static int ss_uniform(void *ud, pc_rect r)
{
    const sel_ss *s = (const sel_ss *)ud;
    int state = -2;
    int64_t c0 = (int64_t)r.x * SS, c1 = (int64_t)(r.x + r.w) * SS;
    if (!s->xs || pc_rect_is_empty(pc_rect_intersect(r, s->bounds))) return 0;
    if (r.y < s->bounds.y || r.y + r.h > s->bounds.y + s->bounds.h) return -1;
    for (int32_t k = (r.y - s->y0) * SS; k < (r.y + r.h - s->y0) * SS; k++) {
        const ss_xing *x = s->xs + s->off[k];
        uint32_t n = s->off[k + 1] - s->off[k];
        int w = 0, row = -2;
        /* the inside state at every sample of [c0, c1) must be the same */
        for (uint32_t i = 0; i < n; i++) {
            int64_t col = (int64_t)ceil((double)SS * (double)x[i].x - 0.5);
            if (col > c0 && col < c1) return -1;      /* the state changes inside r */
            if (col <= c0) w += x[i].dir;
        }
        row = (s->evenodd ? (w & 1) != 0 : w != 0) ? 255 : 0;
        if (state == -2) state = row;
        else if (state != row) return -1;
    }
    return state < 0 ? 0 : state;
}

void sel_ss_src(pc_sel_src *src, const sel_ss *s)
{
    src->bounds = s->bounds;
    src->fill = ss_fill;
    src->uniform = ss_uniform;
    src->ud = (void *)(uintptr_t)s;
}
