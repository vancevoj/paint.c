/* pc_contour.c - marching-squares outlines of coverage fields and the
 * selection polygon text format. */
#include "pc/pc_contour.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BLK 64
#define VN  (BLK + 1)           /* corner values per side of a cell block */
#define NO_KEY UINT64_MAX

/* ---- segment store -------------------------------------------------------- */
typedef struct seg {
    uint64_t from, to;          /* crossing ids */
    double   x, y;              /* start crossing point (area relative) */
    int32_t  mx, my;            /* pixel corner routed through, when mid */
    uint8_t  mid;
} seg;

typedef struct cls_ent {
    const uint8_t *p;           /* block data pointer returned by the field */
    int32_t vw, vh;
    uint8_t cls;
} cls_ent;

typedef struct ctx {
    const pc_cov_field *f;
    cls_ent  cc[4];             /* classes of recently seen shared blocks */
    unsigned cc_next;
    int32_t  nbx, nby;
    uint8_t *cls;               /* per block: 0 outside, 1 inside, 2 mixed */
    seg     *s;
    size_t   n, cap;
    uint8_t  scratch[BLK * BLK];
    uint8_t  vals[VN * VN];
} ctx;

static uint64_t key(int32_t x, int32_t y, unsigned o)
{
    return ((uint64_t)(uint32_t)(y + 1) << 33) | ((uint64_t)(uint32_t)(x + 1) << 1) |
           (uint64_t)o;
}

static pc_status push_seg(ctx *c, const seg *s)
{
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2u : 1024u, bytes;
        seg *ns;
        if (c->n >= PC_GEOM_MAX_POINTS) return PC_ERR_LIMIT;
        if (!pc_mul_size(nc, sizeof *ns, &bytes)) return PC_ERR_LIMIT;
        ns = (seg *)realloc(c->s, bytes);
        if (!ns) return PC_ERR_NOMEM;
        c->s = ns;
        c->cap = nc;
    }
    c->s[c->n++] = *s;
    return PC_OK;
}

/* ---- block access ------------------------------------------------------------ */
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

static uint8_t block_class(ctx *c, int32_t bx, int32_t by)
{
    const pc_cov_field *f = c->f;
    uint8_t u = 0;
    const uint8_t *p;
    int32_t vw = imin(BLK, f->area.w - bx * BLK), vh = imin(BLK, f->area.h - by * BLK);
    bool any_in = false, any_out = vw < BLK || vh < BLK;
    uint8_t k;
    p = f->block(f->ud, bx, by, c->scratch, &u);
    if (!p) {
        if (u < 128u) return 0u;
        return any_out ? 2u : 1u;
    }
    /* Fields may hand out the same immutable block many times (shared
     * full selection tiles): remember the class per pointer. Scratch
     * pointers are rebuilt on every call and never cached. */
    if (p != c->scratch)
        for (unsigned i = 0; i < 4u; i++)
            if (c->cc[i].p == p && c->cc[i].vw == vw && c->cc[i].vh == vh) return c->cc[i].cls;
    for (int32_t y = 0; y < vh && !(any_in && any_out); y++) {
        const uint8_t *row = p + (size_t)y * BLK;
        for (int32_t x = 0; x < vw; x++) {
            if (row[x] >= 128u) any_in = true;
            else any_out = true;
        }
    }
    k = any_in ? (any_out ? 2u : 1u) : 0u;
    if (p != c->scratch) {
        c->cc[c->cc_next].p = p;
        c->cc[c->cc_next].vw = vw;
        c->cc[c->cc_next].vh = vh;
        c->cc[c->cc_next].cls = k;
        c->cc_next = (c->cc_next + 1u) & 3u;
    }
    return k;
}

static uint8_t cls_at(const ctx *c, int32_t bx, int32_t by)
{
    if (bx < 0 || by < 0 || bx >= c->nbx || by >= c->nby) return 0u;
    return c->cls[(size_t)by * (size_t)c->nbx + (size_t)bx];
}

/* Copy block-local rect [lx, lx+w) x [ly, ly+h) of block (bx, by) into
 * vals at (dx, dy); out-of-range blocks and out-of-area pixels give 0. */
static void load(ctx *c, int32_t bx, int32_t by, int32_t lx, int32_t ly, int32_t w, int32_t h,
                 int32_t dx, int32_t dy)
{
    const pc_cov_field *f = c->f;
    const uint8_t *p = NULL;
    uint8_t u = 0;
    int32_t vw, vh;
    bool valid = bx >= 0 && by >= 0 && bx < c->nbx && by < c->nby;
    if (valid) p = f->block(f->ud, bx, by, c->scratch, &u);
    vw = valid ? imin(BLK, f->area.w - bx * BLK) : 0;
    vh = valid ? imin(BLK, f->area.h - by * BLK) : 0;
    for (int32_t y = 0; y < h; y++) {
        uint8_t *d = c->vals + (size_t)(dy + y) * VN + (size_t)dx;
        int32_t sy = ly + y;
        for (int32_t x = 0; x < w; x++) {
            int32_t sx = lx + x;
            if (sx >= vw || sy >= vh) d[x] = 0u;
            else d[x] = p ? p[(size_t)sy * BLK + (size_t)sx] : u;
        }
    }
}

/* ---- cells -------------------------------------------------------------------- */
/* Crossing on edge e of the cell whose top-left pixel is (px, py). */
static void crossing(const uint8_t *v4, unsigned e, int32_t px, int32_t py, double *x, double *y,
                     uint64_t *id)
{
    /* v4: a (px,py), b (px+1,py), c (px+1,py+1), d (px,py+1) */
    double a = v4[0], b = v4[1], cc = v4[2], d = v4[3], t;
    switch (e) {
    case 0:   /* top: a -> b */
        t = (127.5 - a) / (b - a);
        *x = (double)px + 0.5 + t; *y = (double)py + 0.5;
        *id = key(px, py, 0u);
        break;
    case 1:   /* right: b -> c */
        t = (127.5 - b) / (cc - b);
        *x = (double)px + 1.5; *y = (double)py + 0.5 + t;
        *id = key(px + 1, py, 1u);
        break;
    case 2:   /* bottom: d -> c */
        t = (127.5 - d) / (cc - d);
        *x = (double)px + 0.5 + t; *y = (double)py + 1.5;
        *id = key(px, py + 1, 0u);
        break;
    default:  /* left: a -> d */
        t = (127.5 - a) / (d - a);
        *x = (double)px + 0.5; *y = (double)py + 0.5 + t;
        *id = key(px, py, 1u);
        break;
    }
}

static pc_status cell(ctx *c, const uint8_t *v4, int32_t px, int32_t py)
{
    /* corners in clockwise order: a, b, c, d; edge e joins corner e and e+1 */
    bool in[4];
    unsigned entries[2], ne = 0, ncross = 0;
    bool hard = true;
    pc_status st = PC_OK;
    for (unsigned i = 0; i < 4u; i++) {
        in[i] = v4[i] >= 128u;
        if (v4[i] != 0u && v4[i] != 255u) hard = false;
    }
    for (unsigned e = 0; e < 4u; e++) {
        bool i0 = in[e], i1 = in[(e + 1u) & 3u];
        if (i0 != i1) {
            ncross++;
            if (!i0 && ne < 2u) entries[ne++] = e;
        }
    }
    if (ncross == 0u) return PC_OK;
    for (unsigned k = 0; k < ne && st == PC_OK; k++) {
        unsigned e0 = entries[k], e1;
        seg s;
        if (ncross == 2u) {
            e1 = (e0 + 1u) & 3u;
            while (!(in[e1] && !in[(e1 + 1u) & 3u])) e1 = (e1 + 1u) & 3u;
        } else {
            /* saddle: the cell average decides whether the inside corners
             * connect through the center */
            unsigned sum = (unsigned)v4[0] + v4[1] + v4[2] + v4[3];
            e1 = sum > 510u ? (e0 + 3u) & 3u : (e0 + 1u) & 3u;
        }
        crossing(v4, e0, px, py, &s.x, &s.y, &s.from);
        {
            double tx, ty;
            crossing(v4, e1, px, py, &tx, &ty, &s.to);
        }
        s.mid = (uint8_t)(hard && ((e1 - e0) & 1u) ? 1u : 0u);
        s.mx = px + 1;
        s.my = py + 1;
        st = push_seg(c, &s);
    }
    return st;
}

static pc_status cell_block(ctx *c, int32_t cbx, int32_t cby)
{
    pc_status st = PC_OK;
    /* vals (i, j) is area pixel (64*cbx - 1 + i, 64*cby - 1 + j) */
    load(c, cbx, cby, 0, 0, BLK, BLK, 1, 1);
    load(c, cbx - 1, cby, BLK - 1, 0, 1, BLK, 0, 1);
    load(c, cbx, cby - 1, 0, BLK - 1, BLK, 1, 1, 0);
    load(c, cbx - 1, cby - 1, BLK - 1, BLK - 1, 1, 1, 0, 0);
    for (int32_t j = 0; j < BLK && st == PC_OK; j++) {
        const uint8_t *r0 = c->vals + (size_t)j * VN, *r1 = r0 + VN;
        int32_t py = cby * BLK - 1 + j;
        if (py >= c->f->area.h) break;
        for (int32_t i = 0; i < BLK; i++) {
            uint8_t v4[4];
            unsigned code;
            v4[0] = r0[i]; v4[1] = r0[i + 1]; v4[2] = r1[i + 1]; v4[3] = r1[i];
            code = (unsigned)(v4[0] >= 128u) | (unsigned)(v4[1] >= 128u) << 1 |
                   (unsigned)(v4[2] >= 128u) << 2 | (unsigned)(v4[3] >= 128u) << 3;
            if (code == 0u || code == 15u) continue;
            st = cell(c, v4, cbx * BLK - 1 + i, py);
            if (st != PC_OK) break;
        }
    }
    return st;
}

/* ---- tracing ------------------------------------------------------------------ */
static bool redundant(pc_pt a, pc_pt b, pc_pt cpt, double simp)
{
    double ux = b.x - a.x, uy = b.y - a.y, vx = cpt.x - b.x, vy = cpt.y - b.y;
    if (simp <= 0.0) return ux * vy - uy * vx == 0.0 && ux * vx + uy * vy > 0.0;
    {
        double wx = cpt.x - a.x, wy = cpt.y - a.y, l2 = wx * wx + wy * wy, t;
        if (l2 == 0.0) return false;
        t = (ux * wx + uy * wy) / l2;
        if (t <= 0.0 || t >= 1.0) return false;
        return fabs(wx * uy - wy * ux) / sqrt(l2) <= simp;
    }
}

static pc_status add_merged(pc_poly *o, pc_pt p, double simp)
{
    while (o->n_pts - o->open_start >= 2u &&
           redundant(o->pts[o->n_pts - 2u], o->pts[o->n_pts - 1u], p, simp))
        o->n_pts--;
    return pc_poly_add(o, p, 0u);
}

static pc_status close_merged(pc_poly *o, double simp)
{
    size_t s = o->open_start;
    while (o->n_pts - s >= 4u &&
           redundant(o->pts[o->n_pts - 2u], o->pts[o->n_pts - 1u], o->pts[s], simp))
        o->n_pts--;
    while (o->n_pts - s >= 4u &&
           redundant(o->pts[o->n_pts - 1u], o->pts[s], o->pts[s + 1u], simp)) {
        memmove(o->pts + s, o->pts + s + 1u, (o->n_pts - s - 1u) * sizeof *o->pts);
        memmove(o->flags + s, o->flags + s + 1u, o->n_pts - s - 1u);
        o->n_pts--;
    }
    if (o->n_pts - s < 3u) {
        o->n_pts = s;          /* degenerate sliver */
        return PC_OK;
    }
    return pc_poly_end(o, true);
}

static size_t hslot(uint64_t k, unsigned shift)
{
    return (size_t)((k * 0x9E3779B97F4A7C15ull) >> shift);
}

static pc_status trace(ctx *c, double simp, pc_poly *out)
{
    size_t size = 1u, bytes;
    unsigned bits = 0;
    uint64_t *keys = NULL;
    uint32_t *vals = NULL;
    uint8_t *done = NULL;
    pc_status st = PC_OK;
    double ox = (double)c->f->area.x, oy = (double)c->f->area.y;
    if (c->n == 0u) return PC_OK;
    while (size < c->n * 2u) { size <<= 1; bits++; }
    if (bits == 0u) { size = 2u; bits = 1u; }
    if (!pc_mul_size(size, sizeof *keys, &bytes)) return PC_ERR_LIMIT;
    keys = (uint64_t *)malloc(bytes);
    vals = (uint32_t *)malloc(size * sizeof *vals);
    done = (uint8_t *)calloc(c->n, 1u);
    if (!keys || !vals || !done) {
        free(keys); free(vals); free(done);
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < size; i++) keys[i] = NO_KEY;
    for (size_t i = 0; i < c->n; i++) {
        size_t h = hslot(c->s[i].from, 64u - bits);
        while (keys[h] != NO_KEY) h = (h + 1u) & (size - 1u);
        keys[h] = c->s[i].from;
        vals[h] = (uint32_t)i;
    }
    for (size_t i0 = 0; i0 < c->n && st == PC_OK; i0++) {
        size_t i = i0;
        if (done[i0]) continue;
        while (st == PC_OK) {
            const seg *s = &c->s[i];
            size_t h;
            done[i] = 1u;
            st = add_merged(out, pc_pt_make(s->x + ox, s->y + oy), simp);
            if (st == PC_OK && s->mid)
                st = add_merged(out, pc_pt_make((double)s->mx + ox, (double)s->my + oy), simp);
            if (st != PC_OK) break;
            h = hslot(s->to, 64u - bits);
            while (keys[h] != NO_KEY && keys[h] != s->to) h = (h + 1u) & (size - 1u);
            if (keys[h] == NO_KEY) { st = PC_ERR_STATE; break; }   /* cannot happen */
            i = vals[h];
            if (done[i]) break;
        }
        if (st == PC_OK) st = close_merged(out, simp);
    }
    free(keys);
    free(vals);
    free(done);
    return st;
}

pc_status pc_contour_field(const pc_cov_field *f, double simplify, pc_poly *out)
{
    ctx c;
    pc_status st = PC_OK;
    size_t nblocks, keep_pts = out->n_pts, keep_cnt = out->n_contours;
    if (!f || !f->block) return PC_ERR_ARG;
    if (f->area.w <= 0 || f->area.h <= 0) return PC_OK;
    if ((uint32_t)f->area.w > PC_MAX_DIM + 1u || (uint32_t)f->area.h > PC_MAX_DIM + 1u)
        return PC_ERR_LIMIT;
    if (!(simplify >= 0.0)) simplify = 0.0;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.nbx = (f->area.w + BLK - 1) / BLK;
    c.nby = (f->area.h + BLK - 1) / BLK;
    nblocks = (size_t)c.nbx * (size_t)c.nby;
    c.cls = (uint8_t *)malloc(nblocks);
    if (!c.cls) return PC_ERR_NOMEM;
    out->n_pts = out->open_start;   /* drop an unfinished contour */
    keep_pts = out->n_pts;
    for (int32_t by = 0; by < c.nby; by++)
        for (int32_t bx = 0; bx < c.nbx; bx++)
            c.cls[(size_t)by * (size_t)c.nbx + (size_t)bx] = block_class(&c, bx, by);
    for (int32_t cby = 0; cby <= c.nby && st == PC_OK; cby++) {
        for (int32_t cbx = 0; cbx <= c.nbx && st == PC_OK; cbx++) {
            uint8_t k = cls_at(&c, cbx, cby);
            if (k != 2u && cls_at(&c, cbx - 1, cby) == k && cls_at(&c, cbx, cby - 1) == k &&
                cls_at(&c, cbx - 1, cby - 1) == k)
                continue;
            st = cell_block(&c, cbx, cby);
        }
    }
    if (st == PC_OK) st = trace(&c, simplify, out);
    free(c.cls);
    free(c.s);
    if (st != PC_OK) {
        out->n_contours = keep_cnt;
        out->n_pts = keep_pts;
        out->open_start = keep_pts;
    }
    return st;
}

/* ---- pc_mask field -------------------------------------------------------------- */
static const uint8_t *mask_block(void *ud, int32_t bx, int32_t by, uint8_t *scratch,
                                 uint8_t *uniform)
{
    const pc_mask *m = (const pc_mask *)ud;
    int32_t x0 = bx * BLK, y0 = by * BLK;
    int32_t w = imin(BLK, m->w - x0), h = imin(BLK, m->h - y0);
    (void)uniform;
    memset(scratch, 0, BLK * BLK);
    for (int32_t y = 0; y < h; y++)
        memcpy(scratch + (size_t)y * BLK,
               m->px + (size_t)(y0 + y) * (size_t)m->stride + (size_t)x0, (size_t)w);
    return scratch;
}

pc_status pc_contour_mask(const pc_mask *m, double simplify, pc_poly *out)
{
    pc_cov_field f;
    if (!m || !m->px || m->w <= 0 || m->h <= 0 || m->stride < m->w) return PC_ERR_ARG;
    f.area = pc_rect_make(m->x, m->y, m->w, m->h);
    f.block = mask_block;
    f.ud = (void *)(uintptr_t)m;
    return pc_contour_field(&f, simplify, out);
}

/* ---- polygon list text ------------------------------------------------------------ */
#define JSON_COORD_MAX 1e9

static size_t fmt_num(double v, char *buf)
{
    /* plain decimal, at most 6 fraction digits, no exponent, no locale */
    char tmp[32];
    size_t n = 0, k = 0;
    long long r = llround(v * 1e6);
    unsigned long long a, ip, fp;
    if (r < 0) { buf[n++] = '-'; a = (unsigned long long)(-r); }
    else a = (unsigned long long)r;
    ip = a / 1000000ull;
    fp = a % 1000000ull;
    if (r < 0 && a == 0u) n = 0;            /* no "-0" */
    do { tmp[k++] = (char)('0' + (int)(ip % 10u)); ip /= 10u; } while (ip);
    while (k) buf[n++] = tmp[--k];
    if (fp) {
        int digits = 6;
        while (fp % 10u == 0u) { fp /= 10u; digits--; }
        buf[n++] = '.';
        for (int i = digits - 1; i >= 0; i--) {
            unsigned long long p10 = 1u;
            for (int j = 0; j < i; j++) p10 *= 10u;
            buf[n++] = (char)('0' + (int)((fp / p10) % 10u));
        }
    }
    return n;
}

pc_status pc_poly_to_json(const pc_poly *p, char **out, size_t *len)
{
    static const char head[] = "{\n  \"polygonList\": [";
    static const char tail[] = "\n  ]\n}\n";
    size_t cap, n = 0, t;
    char *s;
    *out = NULL;
    if (len) *len = 0u;
    /* per point: two numbers of at most 1 + 10 + 1 + 6 chars plus commas */
    if (!pc_mul_size(p->n_pts + p->n_contours, 40u, &cap) ||
        !pc_mul_size(p->n_contours, 16u, &t) || !pc_add_size(cap, t, &cap) ||
        !pc_add_size(cap, sizeof head + sizeof tail + 16u, &cap))
        return PC_ERR_LIMIT;
    for (size_t i = 0; i < p->n_pts; i++)
        if (!(fabs(p->pts[i].x) <= JSON_COORD_MAX) || !(fabs(p->pts[i].y) <= JSON_COORD_MAX))
            return PC_ERR_LIMIT;
    s = (char *)malloc(cap);
    if (!s) return PC_ERR_NOMEM;
    memcpy(s, head, sizeof head - 1u);
    n = sizeof head - 1u;
    for (size_t i = 0; i < p->n_contours; i++) {
        size_t a = pc_poly_contour_start(p, i), e = p->ends[i];
        if (e == a) continue;
        if (i) s[n++] = ',';
        memcpy(s + n, "\n    \"", 6u);
        n += 6u;
        for (size_t k = a; k <= e; k++) {
            pc_pt q = p->pts[k < e ? k : a];   /* repeat the first point */
            if (k > a) s[n++] = ',';
            n += fmt_num(q.x, s + n);
            s[n++] = ',';
            n += fmt_num(q.y, s + n);
        }
        s[n++] = '"';
    }
    memcpy(s + n, tail, sizeof tail);
    n += sizeof tail - 1u;
    *out = s;
    if (len) *len = n;
    return PC_OK;
}

typedef struct jrd {
    const char *s;
    size_t n, i;
} jrd;

static void jws(jrd *r)
{
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' || r->s[r->i] == '\n' ||
                           r->s[r->i] == '\r'))
        r->i++;
}

static bool jpeek(jrd *r, char ch)
{
    jws(r);
    return r->i < r->n && r->s[r->i] == ch;
}

/* Skip a JSON string starting at '"'; sets [*a, *b) to its raw content. */
static bool jstring(jrd *r, size_t *a, size_t *b)
{
    if (!jpeek(r, '"')) return false;
    r->i++;
    *a = r->i;
    while (r->i < r->n && r->s[r->i] != '"') {
        if (r->s[r->i] == '\\') r->i++;
        r->i++;
    }
    if (r->i >= r->n) return false;
    *b = r->i;
    r->i++;
    return true;
}

/* Skip any JSON value without recursion (bracket depth counter). */
static bool jskip(jrd *r)
{
    size_t depth = 0, a, b;
    jws(r);
    do {
        char ch;
        jws(r);
        if (r->i >= r->n) return false;
        ch = r->s[r->i];
        if (ch == '"') {
            if (!jstring(r, &a, &b)) return false;
        } else if (ch == '{' || ch == '[') {
            if (++depth > 256u) return false;
            r->i++;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0u) return false;
            depth--;
            r->i++;
        } else if (ch == ',' || ch == ':') {
            if (depth == 0u) return false;
            r->i++;
        } else {
            /* literal or number */
            size_t s0 = r->i;
            while (r->i < r->n && strchr(",:]} \t\r\n", r->s[r->i]) == NULL) r->i++;
            if (r->i == s0) return false;
        }
    } while (depth > 0u);
    return true;
}

/* Parse a number at s[*i] (bounded by n), locale independent. */
static bool jnum(const char *s, size_t n, size_t *i, double *out)
{
    size_t k = *i;
    double m = 0.0, scale = 1.0;
    int ex = 0, esign = 1, digits = 0;
    bool neg = false;
    if (k < n && (s[k] == '-' || s[k] == '+')) { neg = s[k] == '-'; k++; }
    while (k < n && s[k] >= '0' && s[k] <= '9') {
        if (digits < 18) m = m * 10.0 + (double)(s[k] - '0');
        else ex++;
        digits++;
        k++;
    }
    if (k < n && s[k] == '.') {
        k++;
        while (k < n && s[k] >= '0' && s[k] <= '9') {
            if (digits < 18) { m = m * 10.0 + (double)(s[k] - '0'); ex--; }
            digits++;
            k++;
        }
    }
    if (digits == 0) return false;
    if (k < n && (s[k] == 'e' || s[k] == 'E')) {
        int e = 0, ed = 0;
        k++;
        if (k < n && (s[k] == '-' || s[k] == '+')) { esign = s[k] == '-' ? -1 : 1; k++; }
        while (k < n && s[k] >= '0' && s[k] <= '9') {
            if (e < 1000) e = e * 10 + (s[k] - '0');
            ed++;
            k++;
        }
        if (ed == 0) return false;
        ex += esign * e;
    }
    if (ex > 40 || ex < -40) {
        if (ex > 40 && m != 0.0) return false;   /* far out of range */
        m = 0.0;
        ex = 0;
    }
    for (int j = 0; j < (ex < 0 ? -ex : ex); j++) scale *= 10.0;
    m = ex < 0 ? m / scale : m * scale;
    *out = neg ? -m : m;
    *i = k;
    return true;
}

static pc_status parse_polygon(const char *s, size_t n, pc_poly *out)
{
    size_t i = 0, count = 0, start = out->n_pts;
    double xy[2];
    pc_status st;
    for (;;) {
        double v;
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i >= n) break;
        if (!jnum(s, n, &i, &v)) return PC_ERR_FORMAT;
        if (!(fabs(v) <= JSON_COORD_MAX)) return PC_ERR_LIMIT;
        xy[count & 1u] = v;
        count++;
        if ((count & 1u) == 0u) {
            st = pc_poly_add(out, pc_pt_make(xy[0], xy[1]), 0u);
            if (st != PC_OK) return st;
        }
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i < n) {
            if (s[i] != ',') return PC_ERR_FORMAT;
            i++;
        }
    }
    if (count & 1u) return PC_ERR_FORMAT;
    /* drop the repeated first point */
    if (out->n_pts - start >= 2u && out->pts[out->n_pts - 1u].x == out->pts[start].x &&
        out->pts[out->n_pts - 1u].y == out->pts[start].y)
        out->n_pts--;
    return pc_poly_end(out, true);
}

pc_status pc_poly_from_json(const char *text, size_t n, pc_poly *out)
{
    jrd r;
    bool found = false;
    pc_status st = PC_OK;
    size_t keep_pts, keep_cnt = out->n_contours;
    if (!text) return PC_ERR_ARG;
    out->n_pts = out->open_start;
    keep_pts = out->n_pts;
    r.s = text;
    r.n = n;
    r.i = 0;
    if (!jpeek(&r, '{')) return PC_ERR_FORMAT;
    r.i++;
    if (jpeek(&r, '}')) return PC_ERR_FORMAT;
    for (;;) {
        size_t ka, kb;
        if (!jstring(&r, &ka, &kb) || !jpeek(&r, ':')) { st = PC_ERR_FORMAT; break; }
        r.i++;
        if (kb - ka == 11u && memcmp(text + ka, "polygonList", 11u) == 0 && !found) {
            found = true;
            if (!jpeek(&r, '[')) { st = PC_ERR_FORMAT; break; }
            r.i++;
            if (jpeek(&r, ']')) {
                r.i++;
            } else {
                for (;;) {
                    size_t a, b;
                    if (!jstring(&r, &a, &b)) { st = PC_ERR_FORMAT; break; }
                    if (memchr(text + a, '\\', b - a)) { st = PC_ERR_FORMAT; break; }
                    st = parse_polygon(text + a, b - a, out);
                    if (st != PC_OK) break;
                    if (jpeek(&r, ',')) { r.i++; continue; }
                    if (jpeek(&r, ']')) { r.i++; break; }
                    st = PC_ERR_FORMAT;
                    break;
                }
                if (st != PC_OK) break;
            }
        } else if (!jskip(&r)) {
            st = PC_ERR_FORMAT;
            break;
        }
        if (jpeek(&r, ',')) { r.i++; continue; }
        if (jpeek(&r, '}')) { r.i++; break; }
        st = PC_ERR_FORMAT;
        break;
    }
    if (st == PC_OK && !found) st = PC_ERR_FORMAT;
    if (st != PC_OK) {
        out->n_contours = keep_cnt;
        out->n_pts = keep_pts;
        out->open_start = keep_pts;
    }
    return st;
}
