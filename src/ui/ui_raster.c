/* ui_raster.c - paths, flattening, stroking and the coverage filler. */
#include "ui_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SUBSAMPLES 16

/* ---- path storage -------------------------------------------------------- */
void ui_path_init(ui_path *p, float tolerance)
{
    memset(p, 0, sizeof *p);
    p->tol = tolerance > 0.01f ? tolerance : 0.01f;
}

void ui_path_free(ui_path *p)
{
    free(p->xy);
    free(p->ends);
    free(p->closed);
    ui_path_init(p, p->tol);
}

void ui_path_reset(ui_path *p)
{
    p->n = 0;
    p->nc = 0;
    p->open = false;
    p->oom = false;
    p->cx = p->cy = 0.0f;
}

static bool grow_points(ui_path *p, int32_t extra)
{
    size_t want, bytes;
    int32_t ncap;
    float *nxy;
    if (p->n + extra <= p->cap) return true;
    if (p->n > INT32_MAX / 2 - extra) { p->oom = true; return false; }
    ncap = p->cap ? p->cap : 64;
    while (ncap < p->n + extra) ncap *= 2;
    want = (size_t)ncap;
    if (!pc_mul_size(want, 2u * sizeof(float), &bytes)) { p->oom = true; return false; }
    nxy = (float *)realloc(p->xy, bytes);
    if (!nxy) { p->oom = true; return false; }
    p->xy = nxy;
    p->cap = ncap;
    return true;
}

static bool grow_contours(ui_path *p)
{
    int32_t ncap;
    int32_t *ne;
    uint8_t *ncl;
    if (p->nc < p->ccap) return true;
    if (p->ccap > INT32_MAX / 4) { p->oom = true; return false; }
    ncap = p->ccap ? p->ccap * 2 : 16;
    ne = (int32_t *)realloc(p->ends, (size_t)ncap * sizeof(int32_t));
    if (!ne) { p->oom = true; return false; }
    p->ends = ne;
    ncl = (uint8_t *)realloc(p->closed, (size_t)ncap);
    if (!ncl) { p->oom = true; return false; }
    p->closed = ncl;
    p->ccap = ncap;
    return true;
}

static int32_t contour_start(const ui_path *p, int32_t c)
{
    return c == 0 ? 0 : p->ends[c - 1];
}

static void push_point(ui_path *p, float x, float y)
{
    if (!grow_points(p, 1)) return;
    if (p->open && p->n > 0) {
        float lx = p->xy[2 * (p->n - 1)], ly = p->xy[2 * (p->n - 1) + 1];
        int32_t cs = p->nc > 0 ? p->ends[p->nc - 1] : 0;
        if (p->n > cs && lx == x && ly == y) return;   /* drop duplicates */
    }
    p->xy[2 * p->n] = x;
    p->xy[2 * p->n + 1] = y;
    p->n++;
}

static void finish_contour(ui_path *p, bool closed)
{
    int32_t start = p->nc > 0 ? p->ends[p->nc - 1] : 0;
    if (!p->open) return;
    p->open = false;
    if (p->n <= start) return;
    if (!grow_contours(p)) { p->n = start; return; }
    p->ends[p->nc] = p->n;
    p->closed[p->nc] = (uint8_t)(closed ? 1 : 0);
    p->nc++;
    if (closed) {
        p->cx = p->xy[2 * start];
        p->cy = p->xy[2 * start + 1];
    }
}

void ui_path_move(ui_path *p, float x, float y)
{
    if (p->open) finish_contour(p, false);
    p->open = true;
    push_point(p, x, y);
    p->cx = x; p->cy = y;
}

static void ensure_open(ui_path *p)
{
    if (!p->open) {
        p->open = true;
        push_point(p, p->cx, p->cy);
    }
}

void ui_path_line(ui_path *p, float x, float y)
{
    ensure_open(p);
    push_point(p, x, y);
    p->cx = x; p->cy = y;
}

static int32_t seg_count(float dd, float tol, float factor)
{
    float n = sqrtf(factor * dd / tol);
    if (!(n > 1.0f)) return 1;
    if (n > 256.0f) return 256;
    return (int32_t)ceilf(n);
}

void ui_path_quad(ui_path *p, float x1, float y1, float x, float y)
{
    float x0, y0, ddx, ddy;
    int32_t n;
    ensure_open(p);
    x0 = p->cx; y0 = p->cy;
    ddx = x0 - 2.0f * x1 + x; ddy = y0 - 2.0f * y1 + y;
    n = seg_count(sqrtf(ddx * ddx + ddy * ddy), p->tol, 0.25f);
    for (int32_t i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float px = mt * mt * x0 + 2.0f * mt * t * x1 + t * t * x;
        float py = mt * mt * y0 + 2.0f * mt * t * y1 + t * t * y;
        push_point(p, px, py);
    }
    p->cx = x; p->cy = y;
}

void ui_path_cubic(ui_path *p, float x1, float y1, float x2, float y2, float x, float y)
{
    float x0, y0, ax, ay, bx, by, dd;
    int32_t n;
    ensure_open(p);
    x0 = p->cx; y0 = p->cy;
    ax = x0 - 2.0f * x1 + x2; ay = y0 - 2.0f * y1 + y2;
    bx = x1 - 2.0f * x2 + x; by = y1 - 2.0f * y2 + y;
    dd = sqrtf(ax * ax + ay * ay);
    {
        float d2 = sqrtf(bx * bx + by * by);
        if (d2 > dd) dd = d2;
    }
    n = seg_count(dd, p->tol, 0.75f);
    for (int32_t i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float a = mt * mt * mt, b = 3.0f * mt * mt * t, c = 3.0f * mt * t * t, d = t * t * t;
        push_point(p, a * x0 + b * x1 + c * x2 + d * x, a * y0 + b * y1 + c * y2 + d * y);
    }
    p->cx = x; p->cy = y;
}

static float vec_angle(float ux, float uy, float vx, float vy)
{
    float a = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
}

static int32_t arc_steps(float r, float sweep, float tol)
{
    float step, n;
    if (r <= tol) return 4;
    step = 2.0f * acosf(1.0f - tol / r);
    if (!(step > 0.001f)) step = 0.001f;
    n = ceilf(fabsf(sweep) / step);
    if (n < 2.0f) n = 2.0f;
    if (n > 512.0f) n = 512.0f;
    return (int32_t)n;
}

void ui_path_arc(ui_path *p, float rx, float ry, float rot_deg, bool large, bool sweep,
                 float x, float y)
{
    float x1, y1, cs, sn, dx2, dy2, x1p, y1p, lam, num, den, coef, cxp, cyp, cx, cy;
    float th1, dth, ux, uy, vx, vy;
    int32_t n;
    ensure_open(p);
    x1 = p->cx; y1 = p->cy;
    if (x1 == x && y1 == y) return;
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f) { ui_path_line(p, x, y); return; }
    cs = cosf(rot_deg * UI_PI / 180.0f); sn = sinf(rot_deg * UI_PI / 180.0f);
    dx2 = (x1 - x) * 0.5f; dy2 = (y1 - y) * 0.5f;
    x1p = cs * dx2 + sn * dy2;
    y1p = -sn * dx2 + cs * dy2;
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0f) { float s = sqrtf(lam); rx *= s; ry *= s; }
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    coef = den > 0.0f && num > 0.0f ? sqrtf(num / den) : 0.0f;
    if (large == sweep) coef = -coef;
    cxp = coef * rx * y1p / ry;
    cyp = -coef * ry * x1p / rx;
    cx = cs * cxp - sn * cyp + (x1 + x) * 0.5f;
    cy = sn * cxp + cs * cyp + (y1 + y) * 0.5f;
    ux = (x1p - cxp) / rx; uy = (y1p - cyp) / ry;
    vx = (-x1p - cxp) / rx; vy = (-y1p - cyp) / ry;
    th1 = vec_angle(1.0f, 0.0f, ux, uy);
    dth = vec_angle(ux, uy, vx, vy);
    if (!sweep && dth > 0.0f) dth -= 2.0f * UI_PI;
    if (sweep && dth < 0.0f) dth += 2.0f * UI_PI;
    n = arc_steps(rx > ry ? rx : ry, dth, p->tol);
    {
        /* Interior vertices sit slightly outside the arc so the polygon
         * keeps the exact area of the sector (an inscribed polyline would
         * lose a sliver per segment); the end points stay exact. Solves
         * (n - 2) k^2 + 2 k = n th / sin th for the radius factor k. */
        float th = fabsf(dth) / (float)n, ratio = th > 1e-4f ? th / sinf(th) : 1.0f, k;
        if (n > 2) {
            float a = (float)(n - 2), q = 4.0f + 4.0f * a * (float)n * ratio;
            k = (-2.0f + sqrtf(q)) / (2.0f * a);
        } else {
            k = ratio;
        }
        for (int32_t i = 1; i < n; i++) {
            float t = th1 + dth * (float)i / (float)n;
            float ex = rx * k * cosf(t), ey = ry * k * sinf(t);
            push_point(p, cx + cs * ex - sn * ey, cy + sn * ex + cs * ey);
        }
    }
    push_point(p, x, y);
    p->cx = x; p->cy = y;
}

void ui_path_close(ui_path *p)
{
    finish_contour(p, true);
}

void ui_path_end(ui_path *p)
{
    finish_contour(p, false);
}

void ui_path_ellipse(ui_path *p, float cx, float cy, float rx, float ry)
{
    int32_t n = arc_steps(rx > ry ? rx : ry, 2.0f * UI_PI, p->tol);
    float th, k;
    if (n < 8) n = 8;
    /* area-preserving polygon: vertices at k r with n/2 k^2 sin(th) = pi */
    th = 2.0f * UI_PI / (float)n;
    k = sqrtf(th / sinf(th));
    rx *= k;
    ry *= k;
    ui_path_move(p, cx + rx, cy);
    for (int32_t i = 1; i < n; i++) {
        float t = 2.0f * UI_PI * (float)i / (float)n;
        ui_path_line(p, cx + rx * cosf(t), cy + ry * sinf(t));
    }
    ui_path_close(p);
}

void ui_path_rect(ui_path *p, float x, float y, float w, float h)
{
    ui_path_move(p, x, y);
    ui_path_line(p, x + w, y);
    ui_path_line(p, x + w, y + h);
    ui_path_line(p, x, y + h);
    ui_path_close(p);
}

void ui_path_rrect(ui_path *p, float x, float y, float w, float h, float r)
{
    float m = (w < h ? w : h) * 0.5f;
    if (r > m) r = m;
    if (r <= 0.0f) { ui_path_rect(p, x, y, w, h); return; }
    ui_path_move(p, x + r, y);
    ui_path_line(p, x + w - r, y);
    ui_path_arc(p, r, r, 0.0f, false, true, x + w, y + r);
    ui_path_line(p, x + w, y + h - r);
    ui_path_arc(p, r, r, 0.0f, false, true, x + w - r, y + h);
    ui_path_line(p, x + r, y + h);
    ui_path_arc(p, r, r, 0.0f, false, true, x, y + h - r);
    ui_path_line(p, x, y + r);
    ui_path_arc(p, r, r, 0.0f, false, true, x + r, y);
    ui_path_close(p);
}

float ui_path_contour_area(const ui_path *p, int32_t c)
{
    int32_t s = contour_start(p, c), e = p->ends[c];
    double a = 0.0;
    for (int32_t i = s; i < e; i++) {
        int32_t j = i + 1 < e ? i + 1 : s;
        a += (double)p->xy[2 * i] * p->xy[2 * j + 1] - (double)p->xy[2 * j] * p->xy[2 * i + 1];
    }
    return (float)(a * 0.5);
}

void ui_path_reverse_contour(ui_path *p, int32_t c)
{
    int32_t s = contour_start(p, c), e = p->ends[c] - 1;
    while (s < e) {
        float tx = p->xy[2 * s], ty = p->xy[2 * s + 1];
        p->xy[2 * s] = p->xy[2 * e]; p->xy[2 * s + 1] = p->xy[2 * e + 1];
        p->xy[2 * e] = tx; p->xy[2 * e + 1] = ty;
        s++; e--;
    }
}

void ui_path_bounds(const ui_path *p, float *x0, float *y0, float *x1, float *y1)
{
    float a = 0, b = 0, c = 0, d = 0;
    for (int32_t i = 0; i < p->n; i++) {
        float x = p->xy[2 * i], y = p->xy[2 * i + 1];
        if (i == 0 || x < a) a = x;
        if (i == 0 || y < b) b = y;
        if (i == 0 || x > c) c = x;
        if (i == 0 || y > d) d = y;
    }
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}

/* ---- stroking ------------------------------------------------------------ */
/* Append a closed polygon to dst with clockwise orientation. */
static void emit_poly(ui_path *dst, const float *pts, int32_t n)
{
    double a = 0.0;
    if (n < 3) return;
    for (int32_t i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;
        a += (double)pts[2 * i] * pts[2 * j + 1] - (double)pts[2 * j] * pts[2 * i + 1];
    }
    if (a == 0.0) return;
    if (a > 0.0) {
        ui_path_move(dst, pts[0], pts[1]);
        for (int32_t i = 1; i < n; i++) ui_path_line(dst, pts[2 * i], pts[2 * i + 1]);
    } else {
        ui_path_move(dst, pts[2 * (n - 1)], pts[2 * (n - 1) + 1]);
        for (int32_t i = n - 2; i >= 0; i--) ui_path_line(dst, pts[2 * i], pts[2 * i + 1]);
    }
    ui_path_close(dst);
}

static void emit_circle(ui_path *dst, float cx, float cy, float r)
{
    ui_path_ellipse(dst, cx, cy, r, r);
}

static void emit_join(ui_path *dst, float px, float py, float n0x, float n0y, float n1x,
                      float n1y, float turn, float hw, int join, float miter_limit)
{
    float pts[8], s;
    if (join == UI_JOIN_ROUND) { emit_circle(dst, px, py, hw); return; }
    /* Outer side: opposite to the turn direction. */
    s = turn > 0.0f ? -1.0f : 1.0f;
    pts[0] = px; pts[1] = py;
    pts[2] = px + s * n0x * hw; pts[3] = py + s * n0y * hw;
    if (join == UI_JOIN_MITER) {
        float mx = n0x + n1x, my = n0y + n1y, ml = sqrtf(mx * mx + my * my);
        if (ml > 1e-6f) {
            float cosh = ml * 0.5f;                  /* cos of half the angle */
            float len = hw / cosh;
            if (1.0f / cosh <= miter_limit) {
                pts[4] = px + s * mx / ml * len; pts[5] = py + s * my / ml * len;
                pts[6] = px + s * n1x * hw; pts[7] = py + s * n1y * hw;
                emit_poly(dst, pts, 4);
                return;
            }
        }
    }
    pts[4] = px + s * n1x * hw; pts[5] = py + s * n1y * hw;
    emit_poly(dst, pts, 3);
}

void ui_path_stroke(const ui_path *src, float width, int join, int cap, float miter_limit,
                    ui_path *dst)
{
    float hw = width * 0.5f;
    if (!(hw > 0.0f)) return;
    for (int32_t c = 0; c < src->nc; c++) {
        int32_t s = contour_start(src, c), e = src->ends[c], n = e - s;
        bool closed = src->closed[c] != 0;
        const float *xy = src->xy + 2 * s;
        int32_t nseg;
        if (closed && n > 2 && xy[0] == xy[2 * (n - 1)] && xy[1] == xy[2 * (n - 1) + 1]) n--;
        if (n == 1 || (n == 2 && xy[0] == xy[2] && xy[1] == xy[3])) {
            if (cap == UI_CAP_ROUND) emit_circle(dst, xy[0], xy[1], hw);
            else if (cap == UI_CAP_SQUARE) ui_path_rect(dst, xy[0] - hw, xy[1] - hw, width, width);
            continue;
        }
        nseg = closed ? n : n - 1;
        for (int32_t i = 0; i < nseg; i++) {
            int32_t j = (i + 1) % n;
            float x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
            float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny, q[8];
            if (l < 1e-6f) continue;
            dx /= l; dy /= l;
            nx = -dy; ny = dx;
            if (!closed && cap == UI_CAP_SQUARE) {
                if (i == 0) { x0 -= dx * hw; y0 -= dy * hw; }
                if (i == nseg - 1) { x1 += dx * hw; y1 += dy * hw; }
            }
            q[0] = x0 + nx * hw; q[1] = y0 + ny * hw;
            q[2] = x1 + nx * hw; q[3] = y1 + ny * hw;
            q[4] = x1 - nx * hw; q[5] = y1 - ny * hw;
            q[6] = x0 - nx * hw; q[7] = y0 - ny * hw;
            emit_poly(dst, q, 4);
        }
        /* joins */
        for (int32_t i = 0; i < n; i++) {
            int32_t ip, inx;
            float ax, ay, bx, by, la, lb, turn;
            if (!closed && (i == 0 || i == n - 1)) continue;
            ip = (i + n - 1) % n; inx = (i + 1) % n;
            ax = xy[2 * i] - xy[2 * ip]; ay = xy[2 * i + 1] - xy[2 * ip + 1];
            bx = xy[2 * inx] - xy[2 * i]; by = xy[2 * inx + 1] - xy[2 * i + 1];
            la = sqrtf(ax * ax + ay * ay); lb = sqrtf(bx * bx + by * by);
            if (la < 1e-6f || lb < 1e-6f) continue;
            ax /= la; ay /= la; bx /= lb; by /= lb;
            turn = ax * by - ay * bx;
            if (fabsf(turn) < 1e-4f && ax * bx + ay * by > 0.0f) continue;   /* straight */
            emit_join(dst, xy[2 * i], xy[2 * i + 1], -ay, ax, -by, bx, turn, hw, join,
                      miter_limit);
        }
        if (!closed && cap == UI_CAP_ROUND) {
            emit_circle(dst, xy[0], xy[1], hw);
            emit_circle(dst, xy[2 * (n - 1)], xy[2 * (n - 1) + 1], hw);
        }
    }
}

/* ---- filling ------------------------------------------------------------- */
typedef struct edge { float x0, y0, x1, y1, dxdy; int32_t dir; } edge;

static int cmp_edge(const void *a, const void *b)
{
    float ya = ((const edge *)a)->y0, yb = ((const edge *)b)->y0;
    return ya < yb ? -1 : (ya > yb ? 1 : 0);
}

static void add_span(float *area, float *cover, int32_t w, float xa, float xb, float wgt,
                     int32_t *minx, int32_t *maxx)
{
    int32_t ia, ib;
    if (xa < 0.0f) xa = 0.0f;
    if (xb > (float)w) xb = (float)w;
    if (!(xb > xa)) return;
    ia = (int32_t)xa;
    ib = (int32_t)xb;
    if (ia >= w) return;
    if (ia == ib) {
        area[ia] += (xb - xa) * wgt;
    } else {
        area[ia] += ((float)(ia + 1) - xa) * wgt;
        cover[ia + 1] += wgt;
        cover[ib] -= wgt;
        if (ib < w) area[ib] += (xb - (float)ib) * wgt;
    }
    if (ia < *minx) *minx = ia;
    if (ib > *maxx) *maxx = ib < w ? ib : w - 1;
}

static void add_span_aliased(float *cover, int32_t w, float xa, float xb, int32_t *minx,
                             int32_t *maxx)
{
    float fa = ceilf(xa - 0.5f), fb = ceilf(xb - 0.5f);
    int32_t a, b;
    if (fa < 0.0f) fa = 0.0f;
    if (fb > (float)w) fb = (float)w;
    if (!(fb > fa)) return;
    a = (int32_t)fa; b = (int32_t)fb;
    cover[a] += 1.0f;
    cover[b] -= 1.0f;
    if (a < *minx) *minx = a;
    if (b - 1 > *maxx) *maxx = b - 1;
}

static uint8_t combine(uint8_t d, uint8_t c, int mode)
{
    switch (mode) {
    case UI_RASTER_OVER:
        return (uint8_t)(d + c - (d * c + 127) / 255);
    case UI_RASTER_ERASE:
        return (uint8_t)((d * (255 - c) + 127) / 255);
    case UI_RASTER_MAX:
        return c > d ? c : d;
    default:
        return c;
    }
}

/* Active edge with its crossing at the current sub-scanline. */
typedef struct act { float x; int32_t e; } act;

static int cmp_act(const void *a, const void *b)
{
    float xa = ((const act *)a)->x, xb = ((const act *)b)->x;
    return xa < xb ? -1 : (xa > xb ? 1 : 0);
}

/* The active list stays sorted from one sub-scanline to the next, so an
 * insertion sort is close to linear. Paths whose edges cross a lot (many
 * inversions) fall back to qsort, which bounds the cost at n log n. */
static void sort_acts(act *a, int32_t n)
{
    int64_t moves = 0, budget = 8 * (int64_t)n + 64;
    for (int32_t i = 1; i < n; i++) {
        act t = a[i];
        int32_t j = i - 1;
        while (j >= 0 && a[j].x > t.x) {
            a[j + 1] = a[j];
            j--;
            if (++moves > budget) {
                a[j + 1] = t;
                qsort(a, (size_t)n, sizeof *a, cmp_act);
                return;
            }
        }
        a[j + 1] = t;
    }
}

static bool finite4(float a, float b, float c, float d)
{
    return isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d);
}

pc_status ui_raster_fill(const ui_path *p, int rule, bool aa, uint8_t *dst, int32_t w,
                         int32_t h, int32_t stride, int mode)
{
    edge *edges = NULL;
    act *acts = NULL;
    int32_t ne = 0, nact = 0, next = 0;
    float *area = NULL, *cover = NULL;
    size_t bytes, abytes;
    float ymin = 0.0f, ymax = 0.0f;
    int32_t row0, row1;
    const int32_t sub = aa ? SUBSAMPLES : 1;
    if (w <= 0 || h <= 0 || stride < w || !dst) return PC_ERR_ARG;
    if (mode == UI_RASTER_SET)
        for (int32_t y = 0; y < h; y++) memset(dst + (size_t)y * (size_t)stride, 0, (size_t)w);
    if (p->n == 0) return PC_OK;
    if (!pc_mul_size((size_t)p->n, sizeof(edge), &bytes) ||
        !pc_mul_size((size_t)p->n, sizeof(act), &abytes))
        return PC_ERR_NOMEM;
    edges = (edge *)malloc(bytes);
    acts = (act *)malloc(abytes);
    area = (float *)calloc((size_t)w + 2u, sizeof(float));
    cover = (float *)calloc((size_t)w + 2u, sizeof(float));
    if (!edges || !acts || !area || !cover) {
        free(edges); free(acts); free(area); free(cover);
        return PC_ERR_NOMEM;
    }
    for (int32_t c = 0; c < p->nc; c++) {
        int32_t s = contour_start(p, c), e = p->ends[c];
        if (e - s < 2) continue;
        for (int32_t i = s; i < e; i++) {
            int32_t j = i + 1 < e ? i + 1 : s;
            float x0 = p->xy[2 * i], y0 = p->xy[2 * i + 1];
            float x1 = p->xy[2 * j], y1 = p->xy[2 * j + 1];
            edge *ed;
            if (y0 == y1 || !finite4(x0, y0, x1, y1)) continue;   /* NaN and inf dropped */
            ed = &edges[ne++];
            if (y0 < y1) {
                ed->x0 = x0; ed->y0 = y0; ed->x1 = x1; ed->y1 = y1; ed->dir = 1;
            } else {
                ed->x0 = x1; ed->y0 = y1; ed->x1 = x0; ed->y1 = y0; ed->dir = -1;
            }
            ed->dxdy = (ed->x1 - ed->x0) / (ed->y1 - ed->y0);
            if (!isfinite(ed->dxdy)) { ne--; continue; }
            if (ne == 1 || ed->y0 < ymin) ymin = ed->y0;
            if (ne == 1 || ed->y1 > ymax) ymax = ed->y1;
        }
    }
    if (ne == 0 || !(ymin < (float)h) || !(ymax > 0.0f)) goto done;
    qsort(edges, (size_t)ne, sizeof(edge), cmp_edge);
    row0 = ymin < 0.0f ? 0 : (int32_t)floorf(ymin);
    row1 = ymax > (float)h ? h : (int32_t)ceilf(ymax);
    for (int32_t y = row0; y < row1; y++) {
        int32_t minx = w, maxx = -1;
        for (int32_t s = 0; s < sub; s++) {
            float ys = aa ? (float)y + ((float)s + 0.5f) / (float)sub : (float)y + 0.5f;
            int32_t m = 0, wind = 0;
            /* drop finished edges (keeping the order), move the rest to ys */
            for (int32_t k = 0; k < nact; k++) {
                const edge *ed = &edges[acts[k].e];
                if (ed->y1 <= ys) continue;
                acts[m].e = acts[k].e;
                acts[m].x = ed->x0 + (ys - ed->y0) * ed->dxdy;
                m++;
            }
            nact = m;
            while (next < ne && edges[next].y0 <= ys) {
                const edge *ed = &edges[next];
                if (ed->y1 > ys) {
                    acts[nact].e = next;
                    acts[nact].x = ed->x0 + (ys - ed->y0) * ed->dxdy;
                    nact++;
                }
                next++;
            }
            sort_acts(acts, nact);
            for (int32_t i = 0; i + 1 < nact; i++) {
                bool inside;
                wind += edges[acts[i].e].dir;
                inside = rule == UI_FILL_EVENODD ? (wind & 1) != 0 : wind != 0;
                if (!inside) continue;
                if (aa)
                    add_span(area, cover, w, acts[i].x, acts[i + 1].x, 1.0f / (float)sub,
                             &minx, &maxx);
                else
                    add_span_aliased(cover, w, acts[i].x, acts[i + 1].x, &minx, &maxx);
            }
        }
        if (maxx >= minx) {
            uint8_t *row = dst + (size_t)y * (size_t)stride;
            float run = 0.0f;
            for (int32_t x = minx; x <= maxx; x++) {
                float v;
                uint8_t c8;
                run += cover[x];
                v = area[x] + run;
                if (v < 0.0f) v = -v;
                c8 = (uint8_t)(v >= 1.0f ? 255.0f : v * 255.0f + 0.5f);
                if (c8 || mode == UI_RASTER_SET) row[x] = combine(row[x], c8, mode);
            }
            memset(area, 0, ((size_t)w + 2u) * sizeof(float));
            memset(cover, 0, ((size_t)w + 2u) * sizeof(float));
        }
    }
done:
    free(edges); free(acts); free(area); free(cover);
    return PC_OK;
}
