/* pc_text_hint.c - grid fitting of glyph outlines for the Text tool's
 * Sharp rendering modes (lane TOOLB, TOOLS.md 3.3: Sharp (Modern) =
 * natural symmetric hinting, Sharp (Classic) = GDI-like hinting).
 *
 * paint.c's own automatic hinter; it reads no hinting instructions from the
 * font. One pass per hinted axis (y for both Sharp modes, x as well for
 * Sharp (Classic)):
 *  1. Edges: runs of outline links (on- and off-curve points alike) that
 *     are nearly parallel to the grid lines being fitted (slope below about
 *     3.4 degrees) with one direction. The outline orientation tells on
 *     which side of an edge the ink is. Runs that contain off-curve points
 *     are round (the extremum of a curve), the others flat.
 *  2. Stems: edges on one grid line with the same ink side merge (the
 *     crossbar of a t is split by its stem); an edge with ink on its far
 *     side is paired with the nearest opposite edge whose span overlaps
 *     it, at most 0.35 em away (vertical stems of n and m, crossbars,
 *     serifs, the bowls of o).
 *  3. Alignment zones (y only): the baseline, the x-height and the cap
 *     height. Flat edges at a zone go to the zone's rounded position;
 *     round edges overshooting it (by up to 0.035 em) by less than half a
 *     pixel are pulled onto it, larger overshoots are rounded and kept.
 *  4. Fitting: stem widths round to whole pixels (at least 1); a stem with
 *     an aligned edge keeps that edge, others keep their center as close
 *     as possible; lone edges round to the nearest pixel boundary.
 *  5. Every point of the outline then moves by the piecewise linear,
 *     monotonic map through the (original, fitted) edge positions (where
 *     edges of one position disagree, zone alignment beats stems, stems
 *     beat lone edges), so curves and points between edges follow
 *     smoothly and contours never fold over.
 *
 * Thread rules: pure function of its arguments (any thread). Ownership:
 * the path is modified in place; temporary arrays are owned and freed
 * here. On allocation failure the outline is left unhinted.
 */
#include "pc_text_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLAT_SLOPE 0.06          /* |du| <= FLAT_SLOPE * |dv|: parallel to the grid line */
#define MIN_EDGE_EM 0.01         /* shortest edge, em */
#define MAX_STEM_EM 0.35         /* widest stem, em */
#define ZONE_OVERSHOOT_EM 0.035  /* round overshoot accepted by a zone, em */
#define ZONE_TOL_EM 0.012        /* flat edges this close to a zone align with it, em */
#define MAX_EDGES 4096u          /* more: the glyph is left unhinted */
#define MAX_PAIRS ((size_t)1u << 20)

typedef struct hpt {
    size_t idx;                  /* into the path's points */
    bool   on;                   /* on-curve */
} hpt;

typedef struct hedge {
    double u;                    /* position on the fitted axis */
    double v0, v1;               /* extent along the edge */
    int    ink;                  /* +1: ink towards +u, -1: towards -u */
    bool   round;
    int    pair;                 /* index of the stem partner, -1 */
    double fit;                  /* fitted position */
    int    prio;                 /* as hknot.prio */
    bool   done;
} hedge;

typedef struct hknot {
    double u, f;
    int    prio;                 /* 2 aligned to a zone, 1 stem, 0 lone edge */
} hknot;

typedef struct hctx {
    pc_path *p;
    hpt     *pts;                /* points of all contours, contour after contour */
    size_t  *cstart;             /* contour c: pts[cstart[c] .. cstart[c + 1]) */
    size_t   n_pts, n_cont;
    hedge   *e;
    size_t   n_e, cap_e;
    double   em;
    int      orient;             /* +1 when outer contours run clockwise on screen */
} hctx;

static double coord(const hctx *h, size_t k, int axis)
{
    const pc_pt *q = &h->p->pts[h->pts[k].idx];
    return axis ? q->y : q->x;
}

/* Contours as point lists. False for paths the hinter does not handle
 * (arcs, no points) or on OOM. */
static bool collect(hctx *h)
{
    const pc_path *p = h->p;
    size_t pi = 0, n = 0, nc = 0, maxc;
    bool open = false;
    if (!p->n_verbs || !p->n_pts) return false;
    maxc = p->n_verbs + 1u;
    h->pts = (hpt *)malloc(p->n_pts * sizeof *h->pts);
    h->cstart = (size_t *)malloc((maxc + 1u) * sizeof *h->cstart);
    if (!h->pts || !h->cstart) return false;
    for (size_t v = 0; v < p->n_verbs; v++) {
        size_t cnt;
        switch ((pc_path_verb)p->verbs[v]) {
        case PC_PATH_MOVE: cnt = 1u; break;
        case PC_PATH_LINE: cnt = 1u; break;
        case PC_PATH_QUAD: cnt = 2u; break;
        case PC_PATH_CUBIC: cnt = 3u; break;
        case PC_PATH_CLOSE: cnt = 0u; break;
        default: return false;                       /* arcs: not a glyph outline */
        }
        if (pi + cnt > p->n_pts) return false;
        if (p->verbs[v] == PC_PATH_MOVE || (!open && cnt)) {
            if (open && nc < maxc) nc++;
            h->cstart[nc] = n;
            open = true;
        }
        for (size_t k = 0; k < cnt; k++) {
            h->pts[n].idx = pi + k;
            h->pts[n].on = k + 1u == cnt;
            n++;
        }
        pi += cnt;
        if (p->verbs[v] == PC_PATH_CLOSE && open) {
            nc++;
            open = false;
        }
    }
    if (open) nc++;
    h->cstart[nc] = n;
    h->n_pts = n;
    h->n_cont = nc;
    return n > 0u && nc > 0u;
}

/* Outline orientation from the signed area of all contours. */
static int orientation(const hctx *h)
{
    double a = 0.0;
    for (size_t c = 0; c < h->n_cont; c++) {
        size_t s = h->cstart[c], e = h->cstart[c + 1u];
        for (size_t k = s; k < e; k++) {
            size_t j = k + 1u < e ? k + 1u : s;
            a += coord(h, k, 0) * coord(h, j, 1) - coord(h, j, 0) * coord(h, k, 1);
        }
    }
    return a >= 0.0 ? 1 : -1;
}

static bool push_edge(hctx *h, const hedge *e)
{
    if (h->n_e == h->cap_e) {
        size_t nc = h->cap_e ? h->cap_e * 2u : 32u;
        hedge *ne = (hedge *)realloc(h->e, nc * sizeof *ne);
        if (!ne) return false;
        h->e = ne;
        h->cap_e = nc;
    }
    h->e[h->n_e++] = *e;
    return true;
}

/* Sign of a link along v when it is parallel to the fitted grid lines, 0
 * when it is not. */
static int flat_dir(const hctx *h, size_t a, size_t b, int axis)
{
    double du = coord(h, b, axis) - coord(h, a, axis);
    double dv = coord(h, b, 1 - axis) - coord(h, a, 1 - axis);
    if (!(fabs(dv) > 1e-9) || fabs(du) > FLAT_SLOPE * fabs(dv)) return 0;
    return dv > 0.0 ? 1 : -1;
}

static int edge_cmp(const void *x, const void *y)
{
    const hedge *a = (const hedge *)x, *b = (const hedge *)y;
    if (a->ink != b->ink) return a->ink < b->ink ? -1 : 1;
    if (a->u != b->u) return a->u < b->u ? -1 : 1;
    return a->v0 < b->v0 ? -1 : (a->v0 > b->v0 ? 1 : 0);
}

/* Edges on one grid line with the same ink side are one edge (the
 * crossbar of a t is split by its stem): union of their spans. */
static void merge_edges(hctx *h)
{
    size_t n = 0;
    double eps = 1e-6 * h->em;
    if (h->n_e < 2u) return;
    qsort(h->e, h->n_e, sizeof *h->e, edge_cmp);
    for (size_t i = 0; i < h->n_e; i++) {
        hedge *e = &h->e[i];
        if (n && h->e[n - 1u].ink == e->ink && fabs(h->e[n - 1u].u - e->u) <= eps) {
            hedge *m = &h->e[n - 1u];
            if (e->v0 < m->v0) m->v0 = e->v0;
            if (e->v1 > m->v1) m->v1 = e->v1;
            m->round = m->round && e->round;
            continue;
        }
        h->e[n++] = *e;
    }
    h->n_e = n;
}

/* Edges of one axis (u = axis coordinate). */
static bool find_edges(hctx *h, int axis)
{
    h->n_e = 0;
    for (size_t c = 0; c < h->n_cont; c++) {
        size_t s = h->cstart[c], n = h->cstart[c + 1u] - s, first = n;
        if (n < 2u) continue;
        /* start the walk after a link that is not flat */
        for (size_t k = 0; k < n; k++)
            if (!flat_dir(h, s + k, s + (k + 1u) % n, axis)) { first = (k + 1u) % n; break; }
        if (first == n) continue;                    /* degenerate: all flat */
        for (size_t k = 0; k < n;) {
            size_t a = s + (first + k) % n;
            int d = flat_dir(h, a, s + (first + k + 1u) % n, axis);
            hedge e;
            double sum = 0.0, sum_all = 0.0;
            size_t cnt = 0, cnt_all = 0, len = 0;
            if (!d) { k++; continue; }
            memset(&e, 0, sizeof e);
            e.v0 = e.v1 = coord(h, a, 1 - axis);
            /* extend while the links stay flat with the same direction */
            while (k + len < n) {
                size_t i0 = s + (first + k + len) % n, i1 = s + (first + k + len + 1u) % n;
                if (flat_dir(h, i0, i1, axis) != d) break;
                len++;
            }
            for (size_t m = 0; m <= len; m++) {
                size_t i = s + (first + k + m) % n;
                double u = coord(h, i, axis), v = coord(h, i, 1 - axis);
                if (h->pts[i].on) { sum += u; cnt++; }
                else e.round = true;
                sum_all += u;
                cnt_all++;
                if (v < e.v0) e.v0 = v;
                if (v > e.v1) e.v1 = v;
            }
            k += len;
            if (e.v1 - e.v0 < MIN_EDGE_EM * h->em) continue;
            e.u = cnt ? sum / (double)cnt : sum_all / (double)cnt_all;
            /* ink side: for outer contours clockwise on screen (y down), a
             * link towards +x has the ink at +y and a link towards +y has
             * it at -x */
            e.ink = (axis ? d : -d) * h->orient;
            e.pair = -1;
            if (!push_edge(h, &e)) return false;
        }
    }
    return true;
}

static double overlap(const hedge *a, const hedge *b)
{
    double lo = a->v0 > b->v0 ? a->v0 : b->v0, hi = a->v1 < b->v1 ? a->v1 : b->v1;
    return hi - lo;
}

typedef struct hpair {
    double w;
    size_t lo, hi;
} hpair;

static int pair_cmp(const void *x, const void *y)
{
    const hpair *a = (const hpair *)x, *b = (const hpair *)y;
    if (a->w != b->w) return a->w < b->w ? -1 : 1;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    return a->hi < b->hi ? -1 : (a->hi > b->hi ? 1 : 0);
}

/* Greedy pairing: every candidate (low edge, high edge) pair sorted by
 * width, nearest first, each edge used once. False on OOM. */
static bool pair_stems(hctx *h)
{
    double maxw = MAX_STEM_EM * h->em;
    size_t n = 0, cap = 0;
    hpair *v = NULL;
    for (size_t i = 0; i < h->n_e; i++) {
        const hedge *a = &h->e[i];
        if (a->ink != 1) continue;
        for (size_t j = 0; j < h->n_e; j++) {
            const hedge *b = &h->e[j];
            double w = b->u - a->u;
            if (b->ink != -1 || !(w > 0.0) || w > maxw || overlap(a, b) <= 0.0) continue;
            if (n == cap) {
                size_t nc = cap ? cap * 2u : 64u;
                hpair *nv;
                if (nc > MAX_PAIRS) {
                    free(v);
                    return false;
                }
                nv = (hpair *)realloc(v, nc * sizeof *nv);
                if (!nv) {
                    free(v);
                    return false;
                }
                v = nv;
                cap = nc;
            }
            v[n].w = w;
            v[n].lo = i;
            v[n].hi = j;
            n++;
        }
    }
    if (n > 1u) qsort(v, n, sizeof *v, pair_cmp);
    for (size_t k = 0; k < n; k++) {
        hedge *a = &h->e[v[k].lo], *b = &h->e[v[k].hi];
        if (a->pair >= 0 || b->pair >= 0) continue;
        a->pair = (int)v[k].hi;
        b->pair = (int)v[k].lo;
    }
    free(v);
    return true;
}

/* Zone position of an edge (y axis), or false. Zones are y-down pixel
 * positions: 0 (baseline, ink above), -xh and -cap (ink below). */
static bool zone_fit(const hctx *h, const hedge *e, const double *zones, size_t nz, double *out)
{
    double os = ZONE_OVERSHOOT_EM * h->em, tol = ZONE_TOL_EM * h->em;
    for (size_t z = 0; z < nz; z++) {
        double zu = zones[z], d;
        bool base = z == 0u;
        if (base ? e->ink != -1 : e->ink != 1) continue;
        /* overshoot goes away from the ink: below the baseline, above the tops */
        d = base ? e->u - zu : zu - e->u;
        if (d < -tol || d > (e->round ? os : tol)) continue;
        if (!e->round || d < 0.5) {
            *out = floor(zu + 0.5);
        } else {
            double o = floor(d + 0.5);
            *out = floor(zu + 0.5) + (base ? o : -o);
        }
        return true;
    }
    return false;
}

static int knot_cmp(const void *x, const void *y)
{
    const hknot *a = (const hknot *)x, *b = (const hknot *)y;
    if (a->u != b->u) return a->u < b->u ? -1 : 1;
    return a->prio > b->prio ? -1 : (a->prio < b->prio ? 1 : 0);
}

/* Fit the edges of one axis and move every point. */
static bool fit_axis(hctx *h, int axis, const double *zones, size_t nz)
{
    hknot *k;
    size_t nk = 0;
    if (!find_edges(h, axis) || h->n_e > MAX_EDGES) return false;
    if (!h->n_e) return true;
    merge_edges(h);
    if (!pair_stems(h)) return false;
    for (size_t i = 0; i < h->n_e; i++) {
        hedge *e = &h->e[i];
        double z = 0.0;            /* MSVC C4701: set when zone_fit returns true */
        if (e->done) continue;
        if (e->pair < 0) {
            bool zone = axis && zone_fit(h, e, zones, nz, &z);
            e->fit = zone ? z : floor(e->u + 0.5);
            e->prio = zone ? 2 : 0;
            e->done = true;
        } else {
            hedge *lo = e->ink == 1 ? e : &h->e[e->pair];
            hedge *hi = e->ink == 1 ? &h->e[e->pair] : e;
            double w = hi->u - lo->u, W = floor(w + 0.5);
            if (W < 1.0) W = 1.0;
            lo->prio = hi->prio = 1;
            if (axis && zone_fit(h, lo, zones, nz, &z)) {
                lo->fit = z;
                hi->fit = z + W;
                lo->prio = 2;
            } else if (axis && zone_fit(h, hi, zones, nz, &z)) {
                hi->fit = z;
                lo->fit = z - W;
                hi->prio = 2;
            } else {
                lo->fit = floor(0.5 * (lo->u + hi->u) - 0.5 * W + 0.5);
                hi->fit = lo->fit + W;
            }
            lo->done = hi->done = true;
        }
    }
    k = (hknot *)malloc(h->n_e * sizeof *k);
    if (!k) return false;
    for (size_t i = 0; i < h->n_e; i++) {
        k[i].u = h->e[i].u;
        k[i].f = h->e[i].fit;
        k[i].prio = h->e[i].prio;
    }
    qsort(k, h->n_e, sizeof *k, knot_cmp);
    /* merge equal positions (the strongest fit wins), then keep the map
     * monotonic */
    for (size_t i = 0; i < h->n_e; i++) {
        if (nk && fabs(k[i].u - k[nk - 1u].u) < 1e-9) {
            if (k[i].prio > k[nk - 1u].prio) k[nk - 1u] = k[i];
            continue;
        }
        k[nk++] = k[i];
    }
    for (size_t i = 1; i < nk; i++)
        if (k[i].f < k[i - 1u].f) k[i].f = k[i - 1u].f;
    /* move every point (on- and off-curve) through the map */
    for (size_t i = 0; i < h->p->n_pts; i++) {
        double *u = axis ? &h->p->pts[i].y : &h->p->pts[i].x, v = *u, nv;
        size_t lo = 0, hi = nk;
        if (!isfinite(v)) continue;
        while (lo < hi) {                           /* first knot with u > v */
            size_t mid = lo + (hi - lo) / 2u;
            if (k[mid].u <= v) lo = mid + 1u;
            else hi = mid;
        }
        if (lo == 0u) {
            nv = v + (k[0].f - k[0].u);
        } else if (lo == nk) {
            nv = v + (k[nk - 1u].f - k[nk - 1u].u);
        } else {
            const hknot *a = &k[lo - 1u], *b = &k[lo];
            double t = (v - a->u) / (b->u - a->u);
            nv = a->f + (b->f - a->f) * t;
        }
        *u = nv;
    }
    free(k);
    return true;
}

void pc_text_hint_outline(pc_path *p, pc_text_mode mode, double em, double x_height,
                          double cap_height)
{
    hctx h;
    double zones[3];
    size_t nz = 0;
    pc_path backup;
    if (!p || mode == PC_TEXT_SMOOTH || !(em >= 1.0) || !isfinite(em)) return;
    memset(&h, 0, sizeof h);
    h.p = p;
    h.em = em;
    if (!collect(&h)) goto done;
    h.orient = orientation(&h);
    zones[nz++] = 0.0;
    if (x_height > 0.0 && isfinite(x_height) && x_height < 2.0 * em) zones[nz++] = -x_height;
    if (cap_height > 0.0 && isfinite(cap_height) && cap_height < 2.0 * em &&
        fabs(cap_height - x_height) > ZONE_OVERSHOOT_EM * em)
        zones[nz++] = -cap_height;
    /* work on a copy so a failure halfway leaves the outline unhinted */
    pc_path_init(&backup);
    if (pc_path_copy(&backup, p) != PC_OK) {
        pc_path_free(&backup);
        goto done;
    }
    if (!fit_axis(&h, 1, zones, nz) ||
        (mode == PC_TEXT_SHARP_CLASSIC && !fit_axis(&h, 0, NULL, 0u)))
        memcpy(p->pts, backup.pts, p->n_pts * sizeof *p->pts);
    pc_path_free(&backup);
done:
    free(h.pts);
    free(h.cstart);
    free(h.e);
}
