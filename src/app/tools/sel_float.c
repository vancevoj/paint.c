/* sel_float.c - coverage sources and floating pixels of the move tools
 * (see sel_float.h). The resampling filters are paint.c's own (Catmull-Rom
 * cubic, bilinear, supersampling); the coverage sampler reproduces the
 * pc_sel_transform rule so rendered pixels and committed selections agree.
 * Main thread; tile jobs on pc_par workers. */
#include "sel_float.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TD ((int32_t)PC_TILE_DIM)

/* ---- coverage sources -------------------------------------------------------------- */
static uint8_t snap_at(const sel_cov *c, int32_t x, int32_t y)
{
    const pc_tile *t;
    const pc_sel_snap *s = c->snap;
    if (!s->active || !s->grid) return 0u;
    t = s->grid[(size_t)((uint32_t)y >> PC_TILE_SHIFT) * c->tiles_x +
                ((uint32_t)x >> PC_TILE_SHIFT)];
    if (!t) return 0u;
    return t->data[(((size_t)y & (PC_TILE_DIM - 1u)) << PC_TILE_SHIFT) +
                   ((size_t)x & (PC_TILE_DIM - 1u))];
}

uint8_t sel_cov_at(const sel_cov *c, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0u;
    if (!c->snap) return pc_rect_contains(c->rect, x, y) ? 255u : 0u;
    return snap_at(c, x, y);
}

static bool tile_full(const pc_tile *t, int32_t cw, int32_t ch)
{
    for (int32_t y = 0; y < ch; y++) {
        const uint8_t *r = t->data + (size_t)y * PC_TILE_DIM;
        for (int32_t x = 0; x < cw; x++)
            if (r[x] != 255u) return false;
    }
    return true;
}

pc_status sel_cov_from_snap(sel_cov *c, const pc_sel_snap *snap, const pc_doc *d)
{
    size_t n;
    const pc_tile *last = NULL;
    uint8_t last_kind = 0u;
    int32_t last_cw = -1, last_ch = -1;
    memset(c, 0, sizeof *c);
    c->snap = snap;
    c->w = (int32_t)snap->w;
    c->h = (int32_t)snap->h;
    c->tiles_x = snap->tiles_x;
    c->tiles_y = snap->tiles_y;
    if (!pc_mul_size(snap->tiles_x, snap->tiles_y, &n)) return PC_ERR_LIMIT;
    c->kinds = (uint8_t *)calloc(n ? n : 1u, 1u);
    if (!c->kinds) return PC_ERR_NOMEM;
    if (snap->active && snap->grid) {
        for (uint32_t ty = 0; ty < snap->tiles_y; ty++)
            for (uint32_t tx = 0; tx < snap->tiles_x; tx++) {
                size_t i = (size_t)ty * snap->tiles_x + tx;
                const pc_tile *t = snap->grid[i];
                int32_t cw = c->w - (int32_t)tx * TD, ch = c->h - (int32_t)ty * TD;
                if (cw > TD) cw = TD;
                if (ch > TD) ch = TD;
                if (!t) continue;
                if (t != last || cw != last_cw || ch != last_ch) {
                    last = t;
                    last_cw = cw;
                    last_ch = ch;
                    last_kind = tile_full(t, cw, ch) ? 1u : 2u;
                }
                c->kinds[i] = last_kind;
            }
    }
    /* bounds of the coverage: the document's cached selection bounds when
     * the snapshot is current, else the nonzero tiles */
    if (d && snap->gen == d->sel_gen) {
        c->bounds = pc_sel_bounds(d);
    } else {
        pc_rect b = pc_rect_make(0, 0, 0, 0);
        for (uint32_t ty = 0; ty < snap->tiles_y; ty++)
            for (uint32_t tx = 0; tx < snap->tiles_x; tx++)
                if (c->kinds[(size_t)ty * snap->tiles_x + tx])
                    b = pc_rect_union(b, pc_rect_make((int32_t)tx * TD, (int32_t)ty * TD, TD, TD));
        c->bounds = pc_rect_intersect(b, pc_rect_make(0, 0, c->w, c->h));
    }
    return PC_OK;
}

void sel_cov_from_rect(sel_cov *c, pc_rect r)
{
    memset(c, 0, sizeof *c);
    c->rect = r;
    c->w = r.x + r.w;
    c->h = r.y + r.h;
    c->bounds = r;
}

void sel_cov_free(sel_cov *c)
{
    if (!c) return;
    free(c->kinds);
    memset(c, 0, sizeof *c);
}

bool sel_cov_map_init(sel_cov_map *m, const sel_cov *c, const pc_affine *fwd, bool nearest,
                      bool hard, const pc_doc *d)
{
    double k, mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    memset(m, 0, sizeof *m);
    if (!pc_affine_is_finite(fwd) || !pc_affine_invert(fwd, &m->inv)) return false;
    m->cov = c;
    k = sqrt(fwd->a * fwd->a + fwd->b * fwd->b);
    m->kx = (k > 1.0 && isfinite(k)) ? k : 1.0;
    k = sqrt(fwd->c * fwd->c + fwd->d * fwd->d);
    m->ky = (k > 1.0 && isfinite(k)) ? k : 1.0;
    m->nearest = nearest;
    m->hard = hard;
    m->translate = fwd->a == 1.0 && fwd->b == 0.0 && fwd->c == 0.0 && fwd->d == 1.0 &&
                   fwd->e == floor(fwd->e) && fwd->f == floor(fwd->f) &&
                   fabs(fwd->e) < 1e9 && fabs(fwd->f) < 1e9;
    if (m->translate) {
        m->tx = (int32_t)fwd->e;
        m->ty = (int32_t)fwd->f;
    }
    if (pc_rect_is_empty(c->bounds)) {
        m->dst_bounds = pc_rect_make(0, 0, 0, 0);
        return true;
    }
    for (int i = 0; i < 4; i++) {
        double lx = (double)c->bounds.x + ((i & 1) ? c->bounds.w : 0);
        double ly = (double)c->bounds.y + ((i & 2) ? c->bounds.h : 0);
        pc_pt p = pc_affine_apply(fwd, pc_pt_make(lx, ly));
        mnx = fmin(mnx, p.x);
        mny = fmin(mny, p.y);
        mxx = fmax(mxx, p.x);
        mxy = fmax(mxy, p.y);
    }
    mnx = floor(sel_clampd(mnx)) - 2.0;
    mny = floor(sel_clampd(mny)) - 2.0;
    mxx = ceil(sel_clampd(mxx)) + 2.0;
    mxy = ceil(sel_clampd(mxy)) + 2.0;
    m->dst_bounds = pc_rect_intersect(pc_rect_make((int32_t)mnx, (int32_t)mny,
                                                   (int32_t)(mxx - mnx), (int32_t)(mxy - mny)),
                                      pc_doc_rect(d));
    return true;
}

/* pc_sel_transform's rule: linear interpolation steepened by k around the
 * 50 % level, kept within the two neighbors. */
static double lerp_sharp(double a, double b, double f, double k)
{
    double v = a + (b - a) * f, lo = a < b ? a : b, hi = a < b ? b : a;
    if (k <= 1.0 || a == b) return v;
    v = (v - 127.5) * k + 127.5;
    return v < lo ? lo : (v > hi ? hi : v);
}

uint8_t sel_cov_sample(const sel_cov_map *m, int32_t x, int32_t y)
{
    const sel_cov *c = m->cov;
    uint8_t r;
    if (m->translate) {
        r = sel_cov_at(c, x - m->tx, y - m->ty);
    } else if (m->nearest) {
        pc_pt q = pc_affine_apply(&m->inv, pc_pt_make((double)x + 0.5, (double)y + 0.5));
        if (!(q.x >= 0.0 && q.y >= 0.0 && q.x < (double)c->w && q.y < (double)c->h)) return 0u;
        r = sel_cov_at(c, (int32_t)floor(q.x), (int32_t)floor(q.y));
    } else {
        pc_pt q = pc_affine_apply(&m->inv, pc_pt_make((double)x + 0.5, (double)y + 0.5));
        double sx = q.x - 0.5, sy = q.y - 0.5, fx, fy, v, x0, y0;
        int32_t ix, iy;
        if (!(sx > -2.0 && sy > -2.0 && sx < (double)c->w + 1.0 && sy < (double)c->h + 1.0))
            return 0u;
        x0 = floor(sx);
        y0 = floor(sy);
        fx = sx - x0;
        fy = sy - y0;
        ix = (int32_t)x0;
        iy = (int32_t)y0;
        if (fx < 1e-9) fx = 0.0;
        if (fx > 1.0 - 1e-9) { fx = 0.0; ix++; }
        if (fy < 1e-9) fy = 0.0;
        if (fy > 1.0 - 1e-9) { fy = 0.0; iy++; }
        v = fx > 0.0 ? lerp_sharp(sel_cov_at(c, ix, iy), sel_cov_at(c, ix + 1, iy), fx, m->kx)
                     : (double)sel_cov_at(c, ix, iy);
        if (fy > 0.0) {
            double v1 = sel_cov_at(c, ix, iy + 1);
            if (fx > 0.0) v1 = lerp_sharp(v1, sel_cov_at(c, ix + 1, iy + 1), fx, m->kx);
            v = lerp_sharp(v, v1, fy, m->ky);
        }
        r = v <= 0.0 ? 0u : (v >= 255.0 ? 255u : (uint8_t)(v + 0.5));
    }
    if (m->hard) r = r >= 128u ? 255u : 0u;
    return r;
}

/* Coverage of a local rectangle when it is uniform: 0, 255 or -1. */
static int local_uniform(const sel_cov *c, pc_rect lr)
{
    pc_rect ext = pc_rect_make(0, 0, c->w, c->h), in = pc_rect_intersect(lr, ext);
    bool inside = in.x == lr.x && in.y == lr.y && in.w == lr.w && in.h == lr.h;
    bool all_empty = true, all_full = inside;
    int32_t tx0, ty0, tx1, ty1;
    if (pc_rect_is_empty(in)) return 0;
    if (!c->snap) {
        pc_rect ri = pc_rect_intersect(lr, c->rect);
        if (pc_rect_is_empty(ri)) return 0;
        return (ri.x == lr.x && ri.y == lr.y && ri.w == lr.w && ri.h == lr.h) ? 255 : -1;
    }
    if (!c->snap->active || !c->snap->grid) return 0;
    tx0 = in.x / TD;
    ty0 = in.y / TD;
    tx1 = (in.x + in.w - 1) / TD;
    ty1 = (in.y + in.h - 1) / TD;
    if ((int64_t)(tx1 - tx0 + 1) * (int64_t)(ty1 - ty0 + 1) > 4096) return -1;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            uint8_t k = c->kinds[(size_t)ty * c->tiles_x + (size_t)tx];
            if (k != 0u) all_empty = false;
            if (k != 1u) all_full = false;
            if (!all_empty && !all_full) return -1;
        }
    if (all_empty) return 0;
    return all_full ? 255 : -1;
}

static int cov_uniform(void *ud, pc_rect r)
{
    const sel_cov_map *m = (const sel_cov_map *)ud;
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    if (m->translate)
        return local_uniform(m->cov, pc_rect_make(r.x - m->tx, r.y - m->ty, r.w, r.h));
    for (int i = 0; i < 4; i++) {
        pc_pt p = pc_affine_apply(&m->inv, pc_pt_make((double)r.x + ((i & 1) ? r.w : 0),
                                                      (double)r.y + ((i & 2) ? r.h : 0)));
        mnx = fmin(mnx, p.x);
        mny = fmin(mny, p.y);
        mxx = fmax(mxx, p.x);
        mxy = fmax(mxy, p.y);
    }
    if (!isfinite(mnx) || !isfinite(mny) || !isfinite(mxx) || !isfinite(mxy)) return -1;
    mnx = floor(sel_clampd(mnx)) - 2.0;
    mny = floor(sel_clampd(mny)) - 2.0;
    mxx = ceil(sel_clampd(mxx)) + 2.0;
    mxy = ceil(sel_clampd(mxy)) + 2.0;
    return local_uniform(m->cov, pc_rect_make((int32_t)mnx, (int32_t)mny, (int32_t)(mxx - mnx),
                                              (int32_t)(mxy - mny)));
}

static void cov_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    const sel_cov_map *m = (const sel_cov_map *)ud;
    for (int32_t y = 0; y < r.h; y++)
        for (int32_t x = 0; x < r.w; x++)
            dst[(size_t)y * stride + (size_t)x] = sel_cov_sample(m, r.x + x, r.y + y);
}

void sel_cov_src(pc_sel_src *s, const sel_cov_map *m)
{
    s->bounds = m->dst_bounds;
    s->fill = cov_fill;
    s->uniform = cov_uniform;
    s->ud = (void *)(uintptr_t)m;
}

/* ---- gamma tables -------------------------------------------------------------------- */
#define ENC_N 65536u
static float   g_dec[256];          /* 255 * linear(c / 255) */
static uint8_t g_enc[ENC_N];        /* round(255 * srgb(i / (ENC_N - 1))) */
static bool    g_gamma_ready;

static void gamma_init(void)
{
    if (g_gamma_ready) return;
    for (uint32_t i = 0; i < 256u; i++) {
        double c = (double)i / 255.0;
        g_dec[i] = (float)(255.0 * (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)));
    }
    for (uint32_t i = 0; i < ENC_N; i++) {
        double l = (double)i / (double)(ENC_N - 1u);
        double v = 255.0 * (l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055);
        g_enc[i] = (uint8_t)(v <= 0.0 ? 0 : (v >= 255.0 ? 255 : (int)(v + 0.5)));
    }
    g_gamma_ready = true;
}

/* ---- floating pixels ----------------------------------------------------------------- */
struct sel_float {
    uint32_t     doc_id, layer_id;
    uint32_t     w, h, tiles_x, tiles_y;
    size_t       nt;
    bool         copy, paste;
    pc_tile    **l0;            /* the layer at the lift (one reference per tile) */
    pc_sel_snap  lift;          /* lifted coverage (layer lifts) */
    pc_surf      surf;          /* pasted pixels (owned) */
    sel_cov      cov;
    sel_box      box, box0;
    uint8_t     *ts, *ts0;      /* tile states: 0 original, 1 vacated only, 2 floating */
    pc_rect      touched, touched0;   /* tile rectangle of states != 0 */
    pc_poly      outline;       /* lifted outline, local coordinates */
    pc_poly      scratch;
    uint64_t     node_seq, sel_gen;
    bool         txn_open;
};

static sel_float *float_new(const app_doc *d, uint32_t layer_id, pc_status *st)
{
    sel_float *f = (sel_float *)calloc(1u, sizeof *f);
    const pc_layer *l = pc_doc_layer_by_id(d->doc, layer_id);
    *st = PC_ERR_NOMEM;
    if (!f) return NULL;
    pc_poly_init(&f->outline);
    pc_poly_init(&f->scratch);
    if (!l) {
        free(f);
        *st = PC_ERR_ARG;
        return NULL;
    }
    f->doc_id = d->id;
    f->layer_id = layer_id;
    f->w = d->doc->w;
    f->h = d->doc->h;
    f->tiles_x = d->doc->tiles_x;
    f->tiles_y = d->doc->tiles_y;
    if (!pc_mul_size(f->tiles_x, f->tiles_y, &f->nt)) {
        free(f);
        *st = PC_ERR_LIMIT;
        return NULL;
    }
    f->l0 = (pc_tile **)calloc(f->nt, sizeof *f->l0);
    f->ts = (uint8_t *)calloc(f->nt, 1u);
    f->ts0 = (uint8_t *)calloc(f->nt, 1u);
    if (!f->l0 || !f->ts || !f->ts0) {
        sel_float_free(f);
        return NULL;
    }
    for (size_t i = 0; i < f->nt; i++) {
        f->l0[i] = l->grid[i];
        pc_tile_retain(f->l0[i]);
    }
    f->node_seq = d->hist->cur->seq;
    f->sel_gen = d->doc->sel_gen;
    *st = PC_OK;
    return f;
}

sel_float *sel_float_lift(app *a, app_doc *d, bool copy, pc_status *st)
{
    pc_layer *l = app_doc_layer(d);
    sel_float *f;
    (void)a;
    if (!l || !pc_sel_is_active(d->doc)) {
        *st = PC_ERR_STATE;
        return NULL;
    }
    f = float_new(d, l->id, st);
    if (!f) return NULL;
    f->copy = copy;
    *st = pc_sel_snap_take(d->doc, &f->lift);
    if (*st == PC_OK) *st = sel_cov_from_snap(&f->cov, &f->lift, d->doc);
    if (*st == PC_OK) *st = pc_sel_contour(d->doc, 0.0, &f->outline);
    if (*st != PC_OK) {
        sel_float_free(f);
        return NULL;
    }
    sel_box_set(&f->box, (double)f->cov.bounds.x, (double)f->cov.bounds.y,
                (double)(f->cov.bounds.x + f->cov.bounds.w),
                (double)(f->cov.bounds.y + f->cov.bounds.h));
    return f;
}

sel_float *sel_float_paste(app *a, app_doc *d, uint32_t layer_id, const pc_surf *src, int32_t x,
                           int32_t y, pc_status *st)
{
    sel_float *f;
    pc_affine t;
    (void)a;
    if (!src || !src->px || src->w <= 0 || src->h <= 0) {
        *st = PC_ERR_ARG;
        return NULL;
    }
    f = float_new(d, layer_id, st);
    if (!f) return NULL;
    f->copy = true;
    f->paste = true;
    *st = pc_surf_alloc(&f->surf, src->w, src->h);
    if (*st != PC_OK) {
        sel_float_free(f);
        return NULL;
    }
    for (int32_t r = 0; r < src->h; r++)
        memcpy(pc_surf_row(&f->surf, r), pc_surf_row(src, r), (size_t)src->w * sizeof(pc_px32));
    sel_cov_from_rect(&f->cov, pc_rect_make(0, 0, src->w, src->h));
    *st = pc_poly_add(&f->outline, pc_pt_make(0.0, 0.0), 0u);
    if (*st == PC_OK) *st = pc_poly_add(&f->outline, pc_pt_make(0.0, (double)src->h), 0u);
    if (*st == PC_OK)
        *st = pc_poly_add(&f->outline, pc_pt_make((double)src->w, (double)src->h), 0u);
    if (*st == PC_OK) *st = pc_poly_add(&f->outline, pc_pt_make((double)src->w, 0.0), 0u);
    if (*st == PC_OK) *st = pc_poly_end(&f->outline, true);
    if (*st != PC_OK) {
        sel_float_free(f);
        return NULL;
    }
    sel_box_set(&f->box, 0.0, 0.0, (double)src->w, (double)src->h);
    t = pc_affine_translate((double)x, (double)y);
    f->box.m = t;
    return f;
}

void sel_float_free(sel_float *f)
{
    if (!f) return;
    if (f->l0)
        for (size_t i = 0; i < f->nt; i++) pc_tile_release(f->l0[i]);
    free(f->l0);
    free(f->ts);
    free(f->ts0);
    sel_cov_free(&f->cov);
    pc_sel_snap_free(&f->lift);
    pc_surf_free(&f->surf);
    pc_poly_free(&f->outline);
    pc_poly_free(&f->scratch);
    free(f);
}

bool sel_float_valid(const sel_float *f, const app_doc *d)
{
    if (!f || !d || d->id != f->doc_id || d->doc->w != f->w || d->doc->h != f->h) return false;
    if (d->layer_id != f->layer_id || !pc_doc_layer_by_id(d->doc, f->layer_id)) return false;
    if (f->txn_open) return d->txn != NULL && d->txn_owner == (const void *)f;
    return d->txn == NULL && d->hist->cur->seq == f->node_seq && d->doc->sel_gen == f->sel_gen;
}

uint32_t sel_float_doc(const sel_float *f) { return f ? f->doc_id : 0u; }
sel_box *sel_float_box(sel_float *f) { return &f->box; }
bool sel_float_drag_open(const sel_float *f) { return f && f->txn_open; }

void sel_float_sync(sel_float *f, const app_doc *d)
{
    f->node_seq = d->hist->cur->seq;
    f->sel_gen = d->doc->sel_gen;
}

void sel_float_restore(sel_float *f, const sel_box *box, const app_doc *d)
{
    sel_cov_map cm;
    pc_rect dirty = pc_rect_make(0, 0, 0, 0), lifted = pc_rect_make(0, 0, 0, 0);
    if (!f || !box || !d || f->txn_open) return;
    f->box = *box;
    if (sel_cov_map_init(&cm, &f->cov, &f->box.m, false, false, d->doc)) dirty = cm.dst_bounds;
    if (!f->copy) lifted = pc_rect_intersect(f->cov.bounds, pc_doc_rect(d->doc));
    /* sel_float_render leaves floating tiles (content may land) = 2,
     * vacated-only tiles = 1 and every other touched tile back at the
     * original = 0; touched only ever grows (a superset is fine) */
    for (uint32_t ty = 0; ty < f->tiles_y; ty++)
        for (uint32_t tx = 0; tx < f->tiles_x; tx++) {
            size_t idx = (size_t)ty * f->tiles_x + tx;
            pc_rect tr = pc_rect_make((int32_t)tx * TD, (int32_t)ty * TD, TD, TD);
            bool in_d = !pc_rect_is_empty(pc_rect_intersect(tr, dirty));
            bool in_l = !pc_rect_is_empty(pc_rect_intersect(tr, lifted));
            f->ts[idx] = in_d ? 2u : (in_l ? 1u : 0u);
            if (f->ts[idx])
                f->touched = pc_rect_union(f->touched,
                                           pc_rect_make((int32_t)tx, (int32_t)ty, 1, 1));
        }
    sel_float_sync(f, d);
}

pc_status sel_float_begin(app *a, sel_float *f, app_doc *d)
{
    if (f->txn_open) return PC_OK;
    if (d->txn) return PC_ERR_STATE;
    if (!app_doc_txn_begin(a, d, f, "Move Selected Pixels")) return PC_ERR_NOMEM;
    memcpy(f->ts0, f->ts, f->nt);
    f->box0 = f->box;
    f->touched0 = f->touched;
    f->txn_open = true;
    return PC_OK;
}

void sel_float_cancel(app *a, sel_float *f, app_doc *d)
{
    if (!f->txn_open) return;
    if (d && d->txn_owner == (const void *)f) app_doc_txn_cancel(a, d);
    f->txn_open = false;
    memcpy(f->ts, f->ts0, f->nt);
    f->box = f->box0;
    f->touched = f->touched0;
    if (d) (void)app_doc_ants_preview(d, NULL);
}

void sel_float_preview_ants(sel_float *f, app_doc *d)
{
    pc_poly_clear(&f->scratch);
    if (pc_poly_append(&f->scratch, &f->outline, &f->box.m) == PC_OK)
        (void)app_doc_ants_preview(d, &f->scratch);
}

/* ---- rendering ------------------------------------------------------------------------- */
typedef struct rjob {
    uint32_t idx;
    bool     full;           /* floating content may land here */
    uint8_t *dst;            /* private transaction tile */
} rjob;

typedef struct rctx {
    const sel_float *f;
    const rjob      *jobs;
    sel_cov_map      cm;
    pc_rect          dirty;  /* document pixels that may receive floating content */
    sel_rs           rs;
    bool             gamma;
    uint32_t         nx, ny; /* samples per axis */
} rctx;

typedef struct acc4 {
    float F[4], U[4];
    float C, W;
} acc4;

/* Straight pixel of the floating source at local (i, j). */
static pc_px32 src_px(const sel_float *f, int32_t i, int32_t j)
{
    pc_px32 z;
    memset(&z, 0, sizeof z);
    if (f->paste) {
        if (i < 0 || j < 0 || i >= f->surf.w || j >= f->surf.h) return z;
        return pc_surf_row(&f->surf, j)[i];
    }
    if (i < 0 || j < 0 || (uint32_t)i >= f->w || (uint32_t)j >= f->h) return z;
    {
        const pc_tile *t = f->l0[(size_t)((uint32_t)j >> PC_TILE_SHIFT) * f->tiles_x +
                                 ((uint32_t)i >> PC_TILE_SHIFT)];
        if (!t) return z;
        memcpy(&z, t->data + ((((size_t)j & (PC_TILE_DIM - 1u)) << PC_TILE_SHIFT) +
                              ((size_t)i & (PC_TILE_DIM - 1u))) * 4u, 4u);
    }
    return z;
}

static void tap(const rctx *c, int32_t i, int32_t j, float w, acc4 *a)
{
    pc_px32 p;
    float cv, P[4], k;
    if (w == 0.0f) return;
    a->W += w;
    p = src_px(c->f, i, j);
    if (p.a == 0u) {
        cv = (float)sel_cov_at(&c->f->cov, i, j) * (1.0f / 255.0f);
        a->C += w * cv;
        return;
    }
    cv = (float)sel_cov_at(&c->f->cov, i, j) * (1.0f / 255.0f);
    k = (float)p.a * (1.0f / 255.0f);
    if (c->gamma) {
        P[0] = g_dec[p.b] * k;
        P[1] = g_dec[p.g] * k;
        P[2] = g_dec[p.r] * k;
    } else {
        P[0] = (float)p.b * k;
        P[1] = (float)p.g * k;
        P[2] = (float)p.r * k;
    }
    P[3] = (float)p.a;
    for (int q = 0; q < 4; q++) {
        a->F[q] += w * cv * P[q];
        a->U[q] += w * P[q];
    }
    a->C += w * cv;
}

static void cr_w(double t, float w[4])
{
    /* Catmull-Rom (B = 0, C = 0.5) at distances 1 + t, t, 1 - t, 2 - t */
    double d[4];
    d[0] = 1.0 + t;
    d[1] = t;
    d[2] = 1.0 - t;
    d[3] = 2.0 - t;
    for (int i = 0; i < 4; i++) {
        double x = fabs(d[i]), v;
        if (x < 1.0) v = 1.5 * x * x * x - 2.5 * x * x + 1.0;
        else if (x < 2.0) v = -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
        else v = 0.0;
        w[i] = (float)v;
    }
}

static void sample_at(const rctx *c, double u, double v, acc4 *a)
{
    if (!(u > -4.0 && v > -4.0 && u < 1e9 && v < 1e9)) return;
    switch (c->rs) {
    case SEL_RS_NEAREST:
        tap(c, (int32_t)floor(u), (int32_t)floor(v), 1.0f, a);
        break;
    case SEL_RS_BICUBIC: {
        double fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);
        float wx[4], wy[4];
        int32_t i0 = (int32_t)iu - 1, j0 = (int32_t)iv - 1;
        cr_w(fu - iu, wx);
        cr_w(fv - iv, wy);
        for (int b = 0; b < 4; b++)
            for (int q = 0; q < 4; q++) tap(c, i0 + q, j0 + b, wx[q] * wy[b], a);
        break;
    }
    default: {
        double fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);
        float tu = (float)(fu - iu), tv = (float)(fv - iv);
        int32_t i0 = (int32_t)iu, j0 = (int32_t)iv;
        tap(c, i0, j0, (1.0f - tu) * (1.0f - tv), a);
        tap(c, i0 + 1, j0, tu * (1.0f - tv), a);
        tap(c, i0, j0 + 1, (1.0f - tu) * tv, a);
        tap(c, i0 + 1, j0 + 1, tu * tv, a);
        break;
    }
    }
}

/* Resampled floating color at document pixel (x, y): premultiplied,
 * gamma-encoded, 0..255 per channel (BGRA order). */
static void float_color(const rctx *c, int32_t x, int32_t y, float out[4])
{
    acc4 a;
    const pc_affine *m = &c->cm.inv;
    memset(&a, 0, sizeof a);
    for (uint32_t sj = 0; sj < c->ny; sj++) {
        double dy = (double)y + ((double)sj + 0.5) / (double)c->ny;
        for (uint32_t si = 0; si < c->nx; si++) {
            double dx = (double)x + ((double)si + 0.5) / (double)c->nx;
            sample_at(c, m->a * dx + m->c * dy + m->e, m->b * dx + m->d * dy + m->f, &a);
        }
    }
    if (a.C > 1e-4f) {
        for (int q = 0; q < 4; q++) out[q] = a.F[q] / a.C;
    } else if (a.W > 1e-6f) {
        for (int q = 0; q < 4; q++) out[q] = a.U[q] / a.W;
    } else {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
    }
    if (!(out[3] > 0.0f)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (out[3] > 255.0f) out[3] = 255.0f;
    for (int q = 0; q < 3; q++) {
        if (!(out[q] > 0.0f)) out[q] = 0.0f;
        if (out[q] > out[3]) out[q] = out[3];
    }
    if (c->gamma) {
        float k = (float)(ENC_N - 1u) / out[3];
        for (int q = 0; q < 3; q++) {
            float idx = out[q] * k + 0.5f;
            uint32_t ix = idx >= (float)(ENC_N - 1u) ? ENC_N - 1u : (uint32_t)idx;
            out[q] = (float)g_enc[ix] * out[3] * (1.0f / 255.0f);
        }
    }
}

static uint8_t rnd8(float v)
{
    if (!(v > 0.0f)) return 0u;
    if (v >= 255.0f) return 255u;
    return (uint8_t)(v + 0.5f);
}

static pc_px32 to_straight(const float p[4])
{
    pc_px32 o;
    uint8_t A = rnd8(p[3]);
    if (A == 0u) {
        memset(&o, 0, sizeof o);
        return o;
    }
    {
        float k = 255.0f / p[3];
        o.b = rnd8(p[0] * k);
        o.g = rnd8(p[1] * k);
        o.r = rnd8(p[2] * k);
    }
    o.a = A;
    return o;
}

/* One document pixel. L: the layer at the lift; cl: lifted coverage there
 * (0 for copies); cp: transformed coverage. */
static pc_px32 pixel(const rctx *c, int32_t x, int32_t y, pc_px32 L, uint8_t cl, uint8_t cp)
{
    float P[4], Lp[4], k, kp;
    if (cp == 0u) {
        if (cl == 0u) return L;
        L.a = (uint8_t)pc_mul255(L.a, 255u - cl);          /* vacated (T-MOVEPX-LEAVE) */
        if (L.a == 0u) memset(&L, 0, sizeof L);
        return L;
    }
    if (c->cm.translate) {
        pc_px32 s = src_px(c->f, x - c->cm.tx, y - c->cm.ty);
        if (cp == 255u) return s;
        k = (float)s.a * (1.0f / 255.0f);
        P[0] = (float)s.b * k;
        P[1] = (float)s.g * k;
        P[2] = (float)s.r * k;
        P[3] = (float)s.a;
    } else {
        float_color(c, x, y, P);
        if (cp == 255u) return to_straight(P);
    }
    k = (float)L.a * (1.0f / 255.0f);
    Lp[0] = (float)L.b * k;
    Lp[1] = (float)L.g * k;
    Lp[2] = (float)L.r * k;
    Lp[3] = (float)L.a;
    {
        float r = 1.0f - (float)cl * (1.0f / 255.0f), q = 1.0f - (float)cp * (1.0f / 255.0f);
        k = r < q ? r : q;
    }
    kp = (float)cp * (1.0f / 255.0f);
    for (int i = 0; i < 4; i++) P[i] = Lp[i] * k + P[i] * kp;
    return to_straight(P);
}

static void rjob_fn(void *ud, uint32_t index, uint32_t worker)
{
    const rctx *c = (const rctx *)ud;
    const rjob *j = &c->jobs[index];
    const sel_float *f = c->f;
    uint32_t tx = j->idx % f->tiles_x, ty = j->idx / f->tiles_x;
    int32_t x0 = (int32_t)(tx * PC_TILE_DIM), y0 = (int32_t)(ty * PC_TILE_DIM);
    int32_t cw = (int32_t)f->w - x0, ch = (int32_t)f->h - y0;
    const pc_tile *lt = f->l0[j->idx];
    (void)worker;
    if (cw > TD) cw = TD;
    if (ch > TD) ch = TD;
    for (int32_t y = 0; y < ch; y++) {
        pc_px32 *row = (pc_px32 *)(void *)(j->dst + (size_t)y * PC_TILE_DIM * 4u);
        int32_t py = y0 + y;
        bool rowin = j->full && py >= c->dirty.y && py < c->dirty.y + c->dirty.h;
        for (int32_t x = 0; x < cw; x++) {
            int32_t px = x0 + x;
            pc_px32 L;
            uint8_t cl = 0u, cp = 0u;
            if (lt) memcpy(&L, lt->data + ((size_t)y * PC_TILE_DIM + (size_t)x) * 4u, 4u);
            else memset(&L, 0, sizeof L);
            if (!f->copy) cl = sel_cov_at(&f->cov, px, py);
            if (rowin && px >= c->dirty.x && px < c->dirty.x + c->dirty.w)
                cp = sel_cov_sample(&c->cm, px, py);
            row[x] = pixel(c, px, py, L, cl, cp);
        }
    }
}

static uint32_t samples_for(double scale)
{
    double n = ceil(scale - 1e-9);
    if (!(n >= 1.0)) return 1u;
    return n > 8.0 ? 8u : (uint32_t)n;
}

static pc_rect tiles_of(pc_rect r)
{
    int32_t x0, y0, x1, y1;
    if (pc_rect_is_empty(r)) return pc_rect_make(0, 0, 0, 0);
    x0 = r.x / TD;
    y0 = r.y / TD;
    x1 = (r.x + r.w - 1) / TD + 1;
    y1 = (r.y + r.h - 1) / TD + 1;
    return pc_rect_make(x0, y0, x1 - x0, y1 - y0);
}

pc_status sel_float_render(app *a, sel_float *f, app_doc *d, const sel_quality *q)
{
    rctx c;
    rjob *jobs = NULL;
    pc_rect span, lifted = pc_rect_make(0, 0, 0, 0);
    size_t nj = 0, cap = 0, bytes;
    pc_status st = PC_OK;
    if (!f->txn_open || !d->txn) return PC_ERR_STATE;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.rs = q->rs;
    c.gamma = q->gamma;
    if (!sel_cov_map_init(&c.cm, &f->cov, &f->box.m, q->rs == SEL_RS_NEAREST, q->hard, d->doc))
        return PC_ERR_ARG;
    if (c.gamma) gamma_init();
    c.dirty = c.cm.dst_bounds;
    if (q->rs == SEL_RS_MULTISAMPLE) {
        c.nx = c.ny = 2u;
    } else if (q->rs == SEL_RS_ANISOTROPIC || q->rs == SEL_RS_BICUBIC) {
        c.nx = samples_for(hypot(c.cm.inv.a, c.cm.inv.b));
        c.ny = samples_for(hypot(c.cm.inv.c, c.cm.inv.d));
    } else {
        c.nx = c.ny = 1u;
    }
    if (!f->copy) lifted = pc_rect_intersect(f->cov.bounds, pc_doc_rect(d->doc));
    span = pc_rect_union(pc_rect_union(tiles_of(c.dirty), tiles_of(lifted)), f->touched);
    span = pc_rect_intersect(span, pc_rect_make(0, 0, (int32_t)f->tiles_x, (int32_t)f->tiles_y));
    if (!pc_rect_is_empty(span)) {
        if (!pc_mul_size((size_t)span.w, (size_t)span.h, &cap) ||
            !pc_mul_size(cap, sizeof *jobs, &bytes))
            return PC_ERR_LIMIT;
        jobs = (rjob *)malloc(bytes ? bytes : 1u);
        if (!jobs) return PC_ERR_NOMEM;
    }
    for (int32_t ty = span.y; ty < span.y + span.h && st == PC_OK; ty++)
        for (int32_t tx = span.x; tx < span.x + span.w && st == PC_OK; tx++) {
            uint32_t idx = (uint32_t)ty * f->tiles_x + (uint32_t)tx;
            pc_rect tr = pc_rect_make(tx * TD, ty * TD, TD, TD);
            bool in_d = !pc_rect_is_empty(pc_rect_intersect(tr, c.dirty));
            bool in_l = !pc_rect_is_empty(pc_rect_intersect(tr, lifted));
            if (in_d || (in_l && f->ts[idx] != 1u)) {
                uint8_t *p = pc_txn_tile_rw(d->txn, f->layer_id, idx);
                if (!p) {
                    st = PC_ERR_NOMEM;
                    break;
                }
                jobs[nj].idx = idx;
                jobs[nj].full = in_d;
                jobs[nj].dst = p;
                nj++;
                f->ts[idx] = in_d ? 2u : 1u;
                f->touched = pc_rect_union(f->touched, pc_rect_make(tx, ty, 1, 1));
            } else if (!in_l && f->ts[idx] != 0u) {
                pc_tile_retain(f->l0[idx]);
                st = pc_txn_put_tile(d->txn, f->layer_id, idx, f->l0[idx]);
                if (st == PC_OK) f->ts[idx] = 0u;
            }
        }
    if (st == PC_OK && nj) {
        c.jobs = jobs;
        pc_par_for(app_par(a), rjob_fn, &c, (uint32_t)nj);
    }
    free(jobs);
    app_request_frame(a);
    return st;
}

pc_status sel_float_commit(app *a, sel_float *f, app_doc *d, const sel_quality *q,
                           const char *label, const sel_hist_group *outer)
{
    sel_hist_group g;
    sel_cov_map cm;
    pc_sel_src src;
    pc_status st = sel_float_begin(a, f, d);
    if (st != PC_OK) return st;
    st = sel_float_render(a, f, d, q);
    if (st != PC_OK) {
        sel_float_cancel(a, f, d);
        return st;
    }
    if (!sel_cov_map_init(&cm, &f->cov, &f->box.m, q->rs == SEL_RS_NEAREST, q->hard, d->doc)) {
        sel_float_cancel(a, f, d);
        return PC_ERR_ARG;
    }
    if (!outer) sel_hist_group_begin(d->hist, &g);
    st = app_doc_txn_commit(a, d);
    f->txn_open = false;
    if (st == PC_OK) {
        sel_cov_src(&src, &cm);
        st = pc_sel_apply_src(d->hist, &src, PC_SEL_REPLACE, label);
    } else {
        /* the transaction was dropped: the tiles are as before the drag */
        memcpy(f->ts, f->ts0, f->nt);
        f->touched = f->touched0;
    }
    if (!outer) (void)sel_hist_group_end(d->hist, &g, label);
    (void)app_doc_ants_preview(d, NULL);
    app_doc_history_changed(a, d);
    sel_float_sync(f, d);
    return st;
}
