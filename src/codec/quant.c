/* quant.c - palette quantization, dithering, save pipeline helpers, band
 * flattener and row sink (lane L6a). API and contracts in quant.h.
 *
 * Palette generation works on a histogram of distinct colors. Points live in
 * premultiplied space P = (b*a/255, g*a/255, r*a/255, a), so opaque images
 * quantize in plain RGB and nearly transparent colors stop mattering. The
 * Octree and the variance-based Median Cut produce initial clusters; both
 * are refined by up to KMEANS_ITERS Lloyd (k-means) passes over the
 * histogram. Nearest-color search sorts the palette along its widest axis
 * and prunes by axis distance; ties go to the lowest palette index, so the
 * result never depends on search order.
 *
 * The dithering level and the save pipeline semantics (Auto-detect rules,
 * threshold, flattening onto white) follow the MIT-licensed Paint.NET 3.36
 * source; see docs/notice/l6a.md. The code is original.
 */
#include "quant.h"

#include <stdlib.h>
#include <string.h>

/* ---- tunables ------------------------------------------------------------- */
#define HIST_MAX_ENTRIES (1u << 17)     /* distinct colors before merging */
#define HIST_MAX_CAP     (1u << 18)     /* hash slots at most */
#define HIST_INIT_CAP    (1u << 12)
#define KMEANS_ITERS     8
#define CACHE_BITS       12u
#define CACHE_SIZE       (1u << CACHE_BITS)
#define EXACT_SLOTS      1024u

/* ---- small helpers ---------------------------------------------------------- */
static uint32_t px_pack(pc_px32 p)
{
    return (uint32_t)p.b | ((uint32_t)p.g << 8) | ((uint32_t)p.r << 16) | ((uint32_t)p.a << 24);
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int32_t clamp255(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Premultiplied integer coordinates of a straight color. */
static void px_coords(pc_px32 p, int32_t c[4])
{
    c[0] = (int32_t)pc_mul255(p.b, p.a);
    c[1] = (int32_t)pc_mul255(p.g, p.a);
    c[2] = (int32_t)pc_mul255(p.r, p.a);
    c[3] = (int32_t)p.a;
}

/* Straight 8-bit color from premultiplied sums (s[0..3]) over weight w. */
static pc_px32 px_from_sums(const double s[4], double w)
{
    pc_px32 p;
    double a = w > 0.0 ? s[3] / w : 0.0;
    int32_t ai = clamp255((int32_t)(a + 0.5));
    p.a = (uint8_t)ai;
    if (s[3] <= 0.0 || ai == 0) {
        p.b = p.g = p.r = 0;
        return p;
    }
    p.b = (uint8_t)clamp255((int32_t)(s[0] * 255.0 / s[3] + 0.5));
    p.g = (uint8_t)clamp255((int32_t)(s[1] * 255.0 / s[3] + 0.5));
    p.r = (uint8_t)clamp255((int32_t)(s[2] * 255.0 / s[3] + 0.5));
    return p;
}

/* ---- nearest-color search ------------------------------------------------- */
typedef struct qsearch {
    uint32_t n;
    uint32_t axis;
    int32_t  c[PC_QUANT_MAX_COLORS][4];   /* coordinates, sorted along axis */
    uint8_t  idx[PC_QUANT_MAX_COLORS];    /* palette index of each slot */
} qsearch;

/* cols: n straight colors; skip (or -1) is excluded from the search. */
static void qsearch_build(qsearch *s, const pc_px32 *cols, uint32_t n, int32_t skip)
{
    int32_t lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((int32_t)i == skip) continue;
        px_coords(cols[i], s->c[k]);
        s->idx[k] = (uint8_t)i;
        for (int j = 0; j < 4; j++) {
            if (s->c[k][j] < lo[j]) lo[j] = s->c[k][j];
            if (s->c[k][j] > hi[j]) hi[j] = s->c[k][j];
        }
        k++;
    }
    s->n = k;
    s->axis = 0;
    for (uint32_t j = 1; j < 4; j++)
        if (hi[j] - lo[j] > hi[s->axis] - lo[s->axis]) s->axis = j;
    /* insertion sort by (axis value, palette index): stable and tiny */
    for (uint32_t i = 1; i < k; i++) {
        int32_t c[4];
        uint8_t id = s->idx[i];
        uint32_t j = i;
        memcpy(c, s->c[i], sizeof c);
        while (j > 0 && (s->c[j - 1][s->axis] > c[s->axis] ||
                         (s->c[j - 1][s->axis] == c[s->axis] && s->idx[j - 1] > id))) {
            memcpy(s->c[j], s->c[j - 1], sizeof c);
            s->idx[j] = s->idx[j - 1];
            j--;
        }
        memcpy(s->c[j], c, sizeof c);
        s->idx[j] = id;
    }
}

/* Index of the nearest entry (squared Euclidean distance in premultiplied
 * space, ties to the lowest palette index), or -1 when the search is empty.
 * *dist receives the distance when not NULL. */
static int32_t qsearch_find(const qsearch *s, const int32_t p[4], int32_t *dist)
{
    uint32_t a = s->axis, lo = 0, hi = s->n;
    int32_t best = INT32_MAX, bi = -1;
    if (s->n == 0) return -1;
    while (lo < hi) {                       /* first slot with value >= p[a] */
        uint32_t mid = lo + (hi - lo) / 2u;
        if (s->c[mid][a] < p[a]) lo = mid + 1u; else hi = mid;
    }
    for (uint32_t i = lo; i < s->n; i++) {
        int32_t da = s->c[i][a] - p[a], d;
        if (da * da > best) break;
        d = (s->c[i][0] - p[0]) * (s->c[i][0] - p[0]) + (s->c[i][1] - p[1]) * (s->c[i][1] - p[1]) +
            (s->c[i][2] - p[2]) * (s->c[i][2] - p[2]) + (s->c[i][3] - p[3]) * (s->c[i][3] - p[3]);
        if (d < best || (d == best && (int32_t)s->idx[i] < bi)) { best = d; bi = s->idx[i]; }
    }
    for (uint32_t i = lo; i-- > 0;) {
        int32_t da = p[a] - s->c[i][a], d;
        if (da * da > best) break;
        d = (s->c[i][0] - p[0]) * (s->c[i][0] - p[0]) + (s->c[i][1] - p[1]) * (s->c[i][1] - p[1]) +
            (s->c[i][2] - p[2]) * (s->c[i][2] - p[2]) + (s->c[i][3] - p[3]) * (s->c[i][3] - p[3]);
        if (d < best || (d == best && (int32_t)s->idx[i] < bi)) { best = d; bi = s->idx[i]; }
    }
    if (dist) *dist = best;
    return bi;
}

/* ---- histogram ---------------------------------------------------------------- */
typedef struct qent {
    uint32_t key;          /* posterized BGRA */
    uint32_t pad;
    uint64_t cnt;          /* 0 = empty slot */
    uint64_t sum[4];       /* exact channel sums b, g, r, a */
} qent;

typedef struct qpoint {
    pc_px32  col;          /* straight mean color */
    int32_t  c[4];         /* premultiplied coordinates */
    uint64_t w;
} qpoint;

struct pc_quant {
    qent    *tab;
    uint32_t cap, used, shift;
    uint32_t last_key, last_slot;
    bool     have_last;
    uint64_t n_transparent;
    bool     built, exact;
    bool     alpha_mode;          /* some pixel had 0 < alpha < 255 */
    pc_px32  pal[PC_QUANT_MAX_COLORS];
    uint32_t n_pal;
    int32_t  transparent;
    qsearch  srch;
    /* exact palette lookup: straight color -> index + 1 */
    uint32_t ex_key[EXACT_SLOTS];
    uint16_t ex_val[EXACT_SLOTS];
    /* remap state */
    uint32_t cache_key[CACHE_SIZE];
    int16_t  cache_val[CACHE_SIZE];
    int32_t  pal_c[PC_QUANT_MAX_COLORS][4];
    int32_t *err;                 /* 2 rows of (width + 2) * 4 */
    uint32_t width;
    int32_t  dither;
    uint32_t row;
};

pc_status pc_quant_create(pc_quant **out)
{
    pc_quant *q;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    q = (pc_quant *)calloc(1u, sizeof *q);
    if (!q) return PC_ERR_NOMEM;
    q->tab = (qent *)calloc(HIST_INIT_CAP, sizeof *q->tab);
    if (!q->tab) { free(q); return PC_ERR_NOMEM; }
    q->cap = HIST_INIT_CAP;
    q->transparent = -1;
    *out = q;
    return PC_OK;
}

void pc_quant_destroy(pc_quant *q)
{
    if (!q) return;
    free(q->tab);
    free(q->err);
    free(q);
}

static uint32_t key_of(pc_px32 p, uint32_t s)
{
    return (uint32_t)(p.b >> s) | ((uint32_t)(p.g >> s) << 8) | ((uint32_t)(p.r >> s) << 16) |
           ((uint32_t)(p.a >> s) << 24);
}

static uint32_t slot_of(uint32_t key, uint32_t cap)
{
    return hash32(key) & (cap - 1u);
}

/* Re-insert every entry into a table of new_cap slots, shifting keys right
 * by `more` bits per channel (merging bins). */
static pc_status hist_rehash(pc_quant *q, uint32_t new_cap, uint32_t more)
{
    qent *t = (qent *)calloc(new_cap, sizeof *t);
    uint32_t used = 0;
    if (!t) return PC_ERR_NOMEM;
    for (uint32_t i = 0; i < q->cap; i++) {
        const qent *e = &q->tab[i];
        uint32_t k, s;
        if (!e->cnt) continue;
        k = e->key;
        if (more) {
            uint32_t m = (0xFFu >> more) * 0x01010101u;
            k = (k >> more) & m;
        }
        s = slot_of(k, new_cap);
        while (t[s].cnt && t[s].key != k) s = (s + 1u) & (new_cap - 1u);
        if (!t[s].cnt) { t[s].key = k; used++; }
        t[s].cnt += e->cnt;
        for (int j = 0; j < 4; j++) t[s].sum[j] += e->sum[j];
    }
    free(q->tab);
    q->tab = t;
    q->cap = new_cap;
    q->used = used;
    q->shift += more;
    q->have_last = false;
    return PC_OK;
}

pc_status pc_quant_add(pc_quant *q, const pc_px32 *px, size_t n)
{
    if (!q || (!px && n)) return PC_ERR_ARG;
    if (q->built) return PC_ERR_STATE;
    for (size_t i = 0; i < n; i++) {
        pc_px32 p = px[i];
        uint32_t k, s;
        if (p.a == 0) { q->n_transparent++; continue; }
        if (p.a != 255) q->alpha_mode = true;
        k = key_of(p, q->shift);
        if (q->have_last && k == q->last_key) {
            s = q->last_slot;
        } else {
            for (;;) {
                s = slot_of(k, q->cap);
                while (q->tab[s].cnt && q->tab[s].key != k) s = (s + 1u) & (q->cap - 1u);
                if (q->tab[s].cnt) break;                       /* found */
                if (q->used + 1u > q->cap / 2u) {
                    pc_status st;
                    if (q->cap < HIST_MAX_CAP) st = hist_rehash(q, q->cap * 2u, 0u);
                    else st = hist_rehash(q, q->cap, 1u);       /* merge neighbors */
                    if (st != PC_OK) return st;
                    k = key_of(p, q->shift);
                    continue;
                }
                if (q->used + 1u > HIST_MAX_ENTRIES && q->shift < 7u) {
                    pc_status st = hist_rehash(q, q->cap, 1u);
                    if (st != PC_OK) return st;
                    k = key_of(p, q->shift);
                    continue;
                }
                q->tab[s].key = k;
                q->used++;
                break;
            }
            q->last_key = k;
            q->last_slot = s;
            q->have_last = true;
        }
        q->tab[s].cnt++;
        q->tab[s].sum[0] += p.b; q->tab[s].sum[1] += p.g;
        q->tab[s].sum[2] += p.r; q->tab[s].sum[3] += p.a;
    }
    return PC_OK;
}

/* ---- clustering ------------------------------------------------------------------ */
static int cmp_point(const void *a, const void *b)
{
    uint32_t x = px_pack(((const qpoint *)a)->col), y = px_pack(((const qpoint *)b)->col);
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Counting sort of pts[lo, hi) by coordinate `axis` (stable) through tmp. */
static void sort_axis(qpoint *pts, qpoint *tmp, uint32_t lo, uint32_t hi, uint32_t axis)
{
    uint32_t cnt[257];
    memset(cnt, 0, sizeof cnt);
    for (uint32_t i = lo; i < hi; i++) cnt[pts[i].c[axis] + 1]++;
    for (uint32_t v = 1; v < 257u; v++) cnt[v] += cnt[v - 1];
    for (uint32_t i = lo; i < hi; i++) tmp[lo + cnt[pts[i].c[axis]]++] = pts[i];
    memcpy(pts + lo, tmp + lo, (size_t)(hi - lo) * sizeof *pts);
}

typedef struct mbox {
    uint32_t lo, hi;
    double   score;        /* weighted SSE, < 0 = cannot split */
} mbox;

static double box_sse(const qpoint *pts, uint32_t lo, uint32_t hi)
{
    uint64_t w = 0, s[4] = { 0, 0, 0, 0 }, s2[4] = { 0, 0, 0, 0 };
    double sse = 0.0;
    bool same = true;
    for (uint32_t i = lo; i < hi; i++) {
        w += pts[i].w;
        for (int j = 0; j < 4; j++) {
            uint64_t v = (uint64_t)pts[i].c[j];
            s[j] += pts[i].w * v;
            s2[j] += pts[i].w * v * v;
            if (pts[i].c[j] != pts[lo].c[j]) same = false;
        }
    }
    if (hi - lo < 2u || same || w == 0) return -1.0;
    for (int j = 0; j < 4; j++) sse += (double)s2[j] - (double)s[j] * (double)s[j] / (double)w;
    return sse;
}

/* Variance-based median cut: split the box with the largest SSE at the
 * position that maximizes the between-class variance. Returns box count. */
static uint32_t median_cut(qpoint *pts, uint32_t n, uint32_t k, mbox *boxes, qpoint *tmp)
{
    uint32_t nb = 1;
    boxes[0].lo = 0; boxes[0].hi = n;
    boxes[0].score = box_sse(pts, 0, n);
    while (nb < k) {
        uint32_t bi = 0, best_axis = 4, best_cut = 0;
        double best_gain = -1.0;
        mbox *b;
        for (uint32_t i = 1; i < nb; i++)
            if (boxes[i].score > boxes[bi].score) bi = i;
        b = &boxes[bi];
        if (b->score <= 0.0) break;
        for (uint32_t axis = 0; axis < 4u; axis++) {
            uint64_t wt = 0, st[4] = { 0, 0, 0, 0 }, wl = 0, sl[4] = { 0, 0, 0, 0 };
            sort_axis(pts, tmp, b->lo, b->hi, axis);
            if (pts[b->lo].c[axis] == pts[b->hi - 1u].c[axis]) continue;
            for (uint32_t i = b->lo; i < b->hi; i++) {
                wt += pts[i].w;
                for (int j = 0; j < 4; j++) st[j] += pts[i].w * (uint64_t)pts[i].c[j];
            }
            for (uint32_t i = b->lo; i + 1u < b->hi; i++) {
                double gain = 0.0;
                uint64_t wr;
                wl += pts[i].w;
                for (int j = 0; j < 4; j++) sl[j] += pts[i].w * (uint64_t)pts[i].c[j];
                if (pts[i].c[axis] == pts[i + 1u].c[axis]) continue;
                wr = wt - wl;
                if (wl == 0 || wr == 0) continue;
                for (int j = 0; j < 4; j++) {
                    double l = (double)sl[j], r = (double)(st[j] - sl[j]);
                    gain += l * l / (double)wl + r * r / (double)wr;
                }
                if (gain > best_gain) { best_gain = gain; best_axis = axis; best_cut = i + 1u; }
            }
        }
        if (best_axis == 4u) { b->score = -1.0; continue; }
        sort_axis(pts, tmp, b->lo, b->hi, best_axis);
        boxes[nb].lo = best_cut; boxes[nb].hi = b->hi;
        b->hi = best_cut;
        b->score = box_sse(pts, b->lo, b->hi);
        boxes[nb].score = box_sse(pts, boxes[nb].lo, boxes[nb].hi);
        nb++;
    }
    return nb;
}

/* Octree (Gervautz and Purgathofer) over the histogram points, built and
 * reduced iteratively. Leaves start at depth 6 (RGB) or 5 (RGBA, 16-way). */
typedef struct onode {
    uint64_t cnt;
    uint64_t sum[4];
    int32_t  chbase;       /* first child slot in the child pool, -1 = leaf */
    uint8_t  level;
    uint8_t  nchild;
    uint8_t  dead;         /* absorbed by a sibling or parent */
    uint8_t  pad;
} onode;

typedef struct octree {
    onode   *nodes;
    int32_t *child;
    uint32_t n_nodes, cap_nodes, n_child, cap_child;
    uint32_t fan;          /* 8 or 16 */
} octree;

static int32_t oct_new(octree *t, uint8_t level, bool leaf)
{
    onode *nd;
    if (t->n_nodes == t->cap_nodes) {
        uint32_t c = t->cap_nodes ? t->cap_nodes * 2u : 1024u;
        onode *p = (onode *)realloc(t->nodes, (size_t)c * sizeof *p);
        if (!p) return -1;
        t->nodes = p;
        t->cap_nodes = c;
    }
    nd = &t->nodes[t->n_nodes];
    memset(nd, 0, sizeof *nd);
    nd->level = level;
    nd->chbase = -1;
    if (!leaf) {
        if (t->n_child + t->fan > t->cap_child) {
            uint32_t c = t->cap_child ? t->cap_child * 2u : 4096u;
            int32_t *p;
            while (c < t->n_child + t->fan) c *= 2u;
            p = (int32_t *)realloc(t->child, (size_t)c * sizeof *p);
            if (!p) return -1;
            t->child = p;
            t->cap_child = c;
        }
        nd->chbase = (int32_t)t->n_child;
        for (uint32_t i = 0; i < t->fan; i++) t->child[t->n_child + i] = -1;
        t->n_child += t->fan;
    }
    return (int32_t)t->n_nodes++;
}

typedef struct ocand { uint64_t cnt; int32_t node; } ocand;

static int cmp_cand(const void *a, const void *b)
{
    const ocand *x = (const ocand *)a, *y = (const ocand *)b;
    if (x->cnt != y->cnt) return x->cnt < y->cnt ? -1 : 1;
    return x->node < y->node ? -1 : (x->node > y->node ? 1 : 0);
}

/* Merge the children of node nd into nd (all are leaves). */
static void oct_merge_all(octree *t, int32_t nd)
{
    onode *p = &t->nodes[nd];
    for (uint32_t i = 0; i < t->fan; i++) {
        int32_t c = t->child[(uint32_t)p->chbase + i];
        if (c < 0 || t->nodes[c].dead) continue;
        p->cnt += t->nodes[c].cnt;
        for (int j = 0; j < 4; j++) p->sum[j] += t->nodes[c].sum[j];
        t->nodes[c].dead = 1;
    }
    p->chbase = -1;
}

/* Returns the number of palette entries written to out (<= k), or -1. */
static int32_t octree_palette(const qpoint *pts, uint32_t n, uint32_t k, bool alpha,
                              pc_px32 *out)
{
    octree t;
    uint32_t depth = alpha ? 5u : 6u, leaves = 0, m = 0;
    int32_t root;
    ocand *cand = NULL;
    memset(&t, 0, sizeof t);
    t.fan = alpha ? 16u : 8u;
    root = oct_new(&t, 0, false);
    if (root < 0) goto fail;
    for (uint32_t i = 0; i < n; i++) {
        int32_t nd = root;
        for (uint32_t lv = 0; lv < depth; lv++) {
            uint32_t bit = 7u - lv, ci;
            int32_t ch;
            ci = (((uint32_t)pts[i].c[0] >> bit) & 1u) |
                 ((((uint32_t)pts[i].c[1] >> bit) & 1u) << 1) |
                 ((((uint32_t)pts[i].c[2] >> bit) & 1u) << 2);
            if (alpha) ci |= (((uint32_t)pts[i].c[3] >> bit) & 1u) << 3;
            ch = t.child[(uint32_t)t.nodes[nd].chbase + ci];
            if (ch < 0) {
                ch = oct_new(&t, (uint8_t)(lv + 1u), lv + 1u == depth);
                if (ch < 0) goto fail;
                t.child[(uint32_t)t.nodes[nd].chbase + ci] = ch;
                t.nodes[nd].nchild++;
                if (lv + 1u == depth) leaves++;
            }
            nd = ch;
        }
        t.nodes[nd].cnt += pts[i].w;
        for (int j = 0; j < 4; j++) t.nodes[nd].sum[j] += pts[i].w * (uint64_t)pts[i].c[j];
    }
    cand = (ocand *)malloc((size_t)t.n_nodes * sizeof *cand);
    if (!cand) goto fail;
    for (int32_t lv = (int32_t)depth - 1; lv >= 0 && leaves > k; lv--) {
        uint32_t nc = 0;
        for (uint32_t i = 0; i < t.n_nodes; i++) {
            onode *p = &t.nodes[i];
            uint64_t c = 0;
            if (p->dead || p->level != (uint32_t)lv || p->chbase < 0) continue;
            for (uint32_t j = 0; j < t.fan; j++) {
                int32_t ch = t.child[(uint32_t)p->chbase + j];
                if (ch >= 0 && !t.nodes[ch].dead) c += t.nodes[ch].cnt;
            }
            cand[nc].cnt = c;
            cand[nc].node = (int32_t)i;
            nc++;
        }
        qsort(cand, nc, sizeof *cand, cmp_cand);
        for (uint32_t ci = 0; ci < nc && leaves > k; ci++) {
            int32_t nd = cand[ci].node;
            onode *p = &t.nodes[nd];
            uint32_t live = 0, excess = leaves - k;
            for (uint32_t j = 0; j < t.fan; j++) {
                int32_t ch = t.child[(uint32_t)p->chbase + j];
                if (ch >= 0 && !t.nodes[ch].dead) live++;
            }
            if (live == 0) { p->chbase = -1; continue; }
            if (live - 1u <= excess) {
                oct_merge_all(&t, nd);
                leaves -= live - 1u;
            } else {
                /* partial merge: fold the excess + 1 smallest children into
                 * the smallest one, leaving exactly k leaves */
                ocand kids[16];
                uint32_t nk = 0;
                for (uint32_t j = 0; j < t.fan; j++) {
                    int32_t ch = t.child[(uint32_t)p->chbase + j];
                    if (ch >= 0 && !t.nodes[ch].dead) {
                        kids[nk].cnt = t.nodes[ch].cnt;
                        kids[nk].node = ch;
                        nk++;
                    }
                }
                qsort(kids, nk, sizeof kids[0], cmp_cand);
                for (uint32_t j = 1; j <= excess; j++) {
                    onode *dst = &t.nodes[kids[0].node], *src = &t.nodes[kids[j].node];
                    dst->cnt += src->cnt;
                    for (int c = 0; c < 4; c++) dst->sum[c] += src->sum[c];
                    src->dead = 1;
                }
                leaves -= excess;
            }
        }
    }
    for (uint32_t i = 0; i < t.n_nodes && m < k; i++) {
        const onode *p = &t.nodes[i];
        double s[4];
        if (p->dead || p->chbase >= 0 || p->cnt == 0) continue;
        for (int j = 0; j < 4; j++) s[j] = (double)p->sum[j];
        out[m++] = px_from_sums(s, (double)p->cnt);
    }
    free(cand);
    free(t.nodes);
    free(t.child);
    return (int32_t)m;
fail:
    free(cand);
    free(t.nodes);
    free(t.child);
    return -1;
}

typedef struct kacc { uint64_t w; uint64_t s[4]; } kacc;

/* Lloyd refinement of cols[0..k) over the points. assign: n slots of
 * scratch. wts receives the final weight of every center. */
static void kmeans(const qpoint *pts, uint32_t n, pc_px32 *cols, uint32_t k, uint8_t *assign,
                   uint64_t *wts, qsearch *srch)
{
    kacc acc[PC_QUANT_MAX_COLORS];
    for (int it = 0; it < KMEANS_ITERS; it++) {
        bool changed = false;
        qsearch_build(srch, cols, k, -1);
        memset(acc, 0, sizeof acc);
        for (uint32_t i = 0; i < n; i++) {
            int32_t j = qsearch_find(srch, pts[i].c, NULL);
            if (j < 0) j = 0;
            if (it == 0 || assign[i] != (uint8_t)j) changed = true;
            assign[i] = (uint8_t)j;
            acc[j].w += pts[i].w;
            for (int c = 0; c < 4; c++) acc[j].s[c] += pts[i].w * (uint64_t)pts[i].c[c];
        }
        for (uint32_t j = 0; j < k; j++) wts[j] = acc[j].w;
        if (!changed) break;
        for (uint32_t j = 0; j < k; j++) {
            if (acc[j].w) {
                double s[4];
                for (int c = 0; c < 4; c++) s[c] = (double)acc[j].s[c];
                cols[j] = px_from_sums(s, (double)acc[j].w);
            } else {
                /* empty cluster: move it onto the worst represented point */
                uint64_t worst = 0;
                uint32_t wi = UINT32_MAX;
                for (uint32_t i = 0; i < n; i++) {
                    int32_t cc[4], d = 0;
                    uint64_t e;
                    px_coords(cols[assign[i]], cc);
                    for (int c = 0; c < 4; c++) d += (cc[c] - pts[i].c[c]) * (cc[c] - pts[i].c[c]);
                    e = (uint64_t)d * pts[i].w;
                    if (e > worst) { worst = e; wi = i; }
                }
                if (wi != UINT32_MAX) {
                    cols[j] = pts[wi].col;
                    assign[wi] = (uint8_t)j;
                }
            }
        }
    }
}

typedef struct palent { pc_px32 c; uint64_t w; } palent;

static int cmp_palent(const void *a, const void *b)
{
    const palent *x = (const palent *)a, *y = (const palent *)b;
    uint32_t px, py;
    if (x->w != y->w) return x->w > y->w ? -1 : 1;
    px = px_pack(x->c); py = px_pack(y->c);
    return px < py ? -1 : (px > py ? 1 : 0);
}

static void exact_insert(pc_quant *q, uint32_t key, uint32_t idx)
{
    uint32_t s = hash32(key) & (EXACT_SLOTS - 1u);
    while (q->ex_val[s] && q->ex_key[s] != key) s = (s + 1u) & (EXACT_SLOTS - 1u);
    q->ex_key[s] = key;
    q->ex_val[s] = (uint16_t)(idx + 1u);
}

static int32_t exact_find(const pc_quant *q, uint32_t key)
{
    uint32_t s = hash32(key) & (EXACT_SLOTS - 1u);
    while (q->ex_val[s]) {
        if (q->ex_key[s] == key) return (int32_t)q->ex_val[s] - 1;
        s = (s + 1u) & (EXACT_SLOTS - 1u);
    }
    return -1;
}

pc_status pc_quant_build(pc_quant *q, uint32_t max_colors, pc_quant_algo algo)
{
    qpoint *pts = NULL, *tmp = NULL;
    mbox *boxes = NULL;
    uint8_t *assign = NULL;
    palent ent[PC_QUANT_MAX_COLORS];
    pc_px32 cols[PC_QUANT_MAX_COLORS];
    uint64_t wts[PC_QUANT_MAX_COLORS];
    uint32_t np = 0, k, nc = 0, reserve;
    pc_status st = PC_OK;
    if (!q || q->built || max_colors < 2u || max_colors > PC_QUANT_MAX_COLORS) return PC_ERR_ARG;
    if (algo != PC_QUANT_OCTREE && algo != PC_QUANT_MEDIAN_CUT) return PC_ERR_ARG;
    reserve = q->n_transparent ? 1u : 0u;
    k = max_colors - reserve;
    if (q->used) {
        pts = (qpoint *)malloc((size_t)q->used * sizeof *pts);
        if (!pts) return PC_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < q->cap; i++) {
        const qent *e = &q->tab[i];
        pc_px32 c;
        if (!e->cnt) continue;
        c.b = (uint8_t)((e->sum[0] + e->cnt / 2u) / e->cnt);
        c.g = (uint8_t)((e->sum[1] + e->cnt / 2u) / e->cnt);
        c.r = (uint8_t)((e->sum[2] + e->cnt / 2u) / e->cnt);
        c.a = (uint8_t)((e->sum[3] + e->cnt / 2u) / e->cnt);
        pts[np].col = c;
        px_coords(c, pts[np].c);
        pts[np].w = e->cnt;
        np++;
    }
    if (np > 1u) qsort(pts, np, sizeof *pts, cmp_point);   /* canonical order */
    q->exact = false;
    if (np <= k) {
        for (uint32_t i = 0; i < np; i++) { ent[i].c = pts[i].col; ent[i].w = pts[i].w; }
        nc = np;
        q->exact = q->shift == 0u;
        if (np == 0 && !reserve) {                 /* empty image: one black entry */
            ent[0].c.b = ent[0].c.g = ent[0].c.r = 0; ent[0].c.a = 255; ent[0].w = 0;
            nc = 1;
            q->exact = true;
        }
    } else {
        int32_t m;
        tmp = (qpoint *)malloc((size_t)np * sizeof *tmp);
        assign = (uint8_t *)malloc(np);
        if (!tmp || !assign) { st = PC_ERR_NOMEM; goto done; }
        if (algo == PC_QUANT_MEDIAN_CUT) {
            boxes = (mbox *)malloc((size_t)k * sizeof *boxes);
            if (!boxes) { st = PC_ERR_NOMEM; goto done; }
            m = (int32_t)median_cut(pts, np, k, boxes, tmp);
            for (int32_t b = 0; b < m; b++) {
                double s[4] = { 0, 0, 0, 0 }, w = 0.0;
                for (uint32_t i = boxes[b].lo; i < boxes[b].hi; i++) {
                    w += (double)pts[i].w;
                    for (int c = 0; c < 4; c++) s[c] += (double)pts[i].w * (double)pts[i].c[c];
                }
                cols[b] = px_from_sums(s, w);
            }
            /* restore canonical order for the refinement */
            qsort(pts, np, sizeof *pts, cmp_point);
        } else {
            m = octree_palette(pts, np, k, q->alpha_mode, cols);
            if (m < 0) { st = PC_ERR_NOMEM; goto done; }
        }
        if (m <= 0) { cols[0] = pts[0].col; m = 1; }
        kmeans(pts, np, cols, (uint32_t)m, assign, wts, &q->srch);
        for (int32_t i = 0; i < m; i++) { ent[i].c = cols[i]; ent[i].w = wts[i]; }
        nc = (uint32_t)m;
    }
    if (nc > 1u) qsort(ent, nc, sizeof ent[0], cmp_palent);
    {   /* drop duplicate colors (means that rounded to the same value) */
        uint32_t o = 0;
        for (uint32_t i = 0; i < nc; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < o && !dup; j++)
                dup = px_pack(ent[j].c) == px_pack(ent[i].c);
            if (!dup) ent[o++] = ent[i];
        }
        nc = o;
    }
    for (uint32_t i = 0; i < nc; i++) q->pal[i] = ent[i].c;
    q->n_pal = nc;
    q->transparent = -1;
    if (reserve) {
        memset(&q->pal[nc], 0, sizeof q->pal[nc]);
        q->transparent = (int32_t)nc;
        q->n_pal = nc + 1u;
    }
    qsearch_build(&q->srch, q->pal, q->n_pal, q->transparent);
    memset(q->ex_val, 0, sizeof q->ex_val);
    if (q->exact)
        for (uint32_t i = 0; i < nc; i++) exact_insert(q, px_pack(q->pal[i]), i);
    for (uint32_t i = 0; i < q->n_pal; i++) px_coords(q->pal[i], q->pal_c[i]);
    q->built = true;
    free(q->tab);               /* the histogram is no longer needed */
    q->tab = NULL;
    q->cap = 0;
done:
    free(pts);
    free(tmp);
    free(boxes);
    free(assign);
    return st;
}

uint32_t pc_quant_palette(const pc_quant *q, pc_px32 *pal, int32_t *transparent)
{
    if (transparent) *transparent = q && q->built ? q->transparent : -1;
    if (!q || !q->built) return 0;
    if (pal) memcpy(pal, q->pal, (size_t)q->n_pal * sizeof *pal);
    return q->n_pal;
}

bool pc_quant_exact(const pc_quant *q) { return q && q->built && q->exact; }
uint64_t pc_quant_transparent_count(const pc_quant *q) { return q ? q->n_transparent : 0u; }

/* ---- remapping ------------------------------------------------------------------- */
pc_status pc_quant_remap_begin(pc_quant *q, uint32_t width, int32_t dither)
{
    size_t n;
    if (!q || !q->built || width == 0u || dither < 0 || dither > PC_QUANT_DITHER_MAX)
        return PC_ERR_ARG;
    free(q->err);
    q->err = NULL;
    if (!pc_mul_size((size_t)width + 2u, 8u, &n)) return PC_ERR_LIMIT;
    q->err = (int32_t *)calloc(n, sizeof *q->err);
    if (!q->err) return PC_ERR_NOMEM;
    q->width = width;
    q->dither = dither;
    q->row = 0;
    for (uint32_t i = 0; i < CACHE_SIZE; i++) q->cache_val[i] = -1;
    return PC_OK;
}

static int32_t find_cached(pc_quant *q, const int32_t c[4])
{
    uint32_t key = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) |
                   ((uint32_t)c[3] << 24);
    uint32_t s = hash32(key) >> (32u - CACHE_BITS);
    int32_t r;
    if (q->cache_val[s] >= 0 && q->cache_key[s] == key) return q->cache_val[s];
    r = qsearch_find(&q->srch, c, NULL);
    if (r < 0) r = q->transparent >= 0 ? q->transparent : 0;
    q->cache_key[s] = key;
    q->cache_val[s] = (int16_t)r;
    return r;
}

void pc_quant_remap_row(pc_quant *q, const pc_px32 *src, uint8_t *dst)
{
    uint32_t w = q->width;
    int32_t *cur = q->err + (size_t)(q->row & 1u) * (size_t)(w + 2u) * 4u;
    int32_t *nxt = q->err + (size_t)((q->row + 1u) & 1u) * (size_t)(w + 2u) * 4u;
    int32_t dir = (q->row & 1u) ? -1 : 1;
    int32_t lvl = q->dither;
    memset(nxt, 0, (size_t)(w + 2u) * 4u * sizeof *nxt);
    for (uint32_t k = 0; k < w; k++) {
        uint32_t x = dir > 0 ? k : w - 1u - k;
        pc_px32 p = src[x];
        int32_t c[4], idx;
        int32_t *e = cur + (size_t)(x + 1u) * 4u;
        if (p.a == 0 && q->transparent >= 0) { dst[x] = (uint8_t)q->transparent; continue; }
        if (q->exact) {
            int32_t ix = exact_find(q, px_pack(p));
            if (ix >= 0) { dst[x] = (uint8_t)ix; continue; }
        }
        px_coords(p, c);
        if (lvl > 0) {
            for (int j = 0; j < 4; j++) {
                int32_t v = e[j] * lvl;
                int32_t adj = v >= 0 ? (v + 64) / 128 : -((-v + 64) / 128);
                c[j] = clamp255(c[j] + adj);
            }
            for (int j = 0; j < 3; j++) if (c[j] > c[3]) c[j] = c[3];
        }
        idx = find_cached(q, c);
        dst[x] = (uint8_t)idx;
        if (lvl > 0) {
            int32_t *fw = cur + (size_t)((int32_t)x + 1 + dir) * 4u;
            int32_t *bb = nxt + (size_t)((int32_t)x + 1 - dir) * 4u;
            int32_t *bm = nxt + (size_t)(x + 1u) * 4u;
            int32_t *bf = nxt + (size_t)((int32_t)x + 1 + dir) * 4u;
            for (int j = 0; j < 4; j++) {
                int32_t d = c[j] - q->pal_c[idx][j];
                fw[j] += 7 * d;
                bb[j] += 3 * d;
                bm[j] += 5 * d;
                bf[j] += d;
            }
        }
    }
    q->row++;
}

pc_status pc_quant_image(const pc_px32 *px, uint32_t w, uint32_t h, size_t stride,
                         uint32_t max_colors, pc_quant_algo algo, int32_t dither,
                         uint8_t *idx, pc_px32 *pal, uint32_t *n_pal,
                         int32_t *transparent)
{
    pc_quant *q = NULL;
    pc_status st;
    if (!px || !idx || !pal || w == 0u || h == 0u || stride < w) return PC_ERR_ARG;
    st = pc_quant_create(&q);
    if (st != PC_OK) return st;
    for (uint32_t y = 0; y < h && st == PC_OK; y++)
        st = pc_quant_add(q, px + (size_t)y * stride, w);
    if (st == PC_OK) st = pc_quant_build(q, max_colors, algo);
    if (st == PC_OK) st = pc_quant_remap_begin(q, w, dither);
    if (st == PC_OK) {
        for (uint32_t y = 0; y < h; y++)
            pc_quant_remap_row(q, px + (size_t)y * stride, idx + (size_t)y * w);
        {
            uint32_t n = pc_quant_palette(q, pal, transparent);
            if (n_pal) *n_pal = n;
        }
    }
    pc_quant_destroy(q);
    return st;
}

/* ---- save pipeline ------------------------------------------------------------------ */
void pc_quant_stats_init(pc_quant_stats *s)
{
    memset(s, 0, sizeof *s);
    s->all_opaque = true;
    s->binary_alpha = true;
}

void pc_quant_stats_add(pc_quant_stats *s, const pc_px32 *px, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_px32 p = px[i];
        if (p.a != 255) {
            s->all_opaque = false;
            if (p.a != 0) s->binary_alpha = false;
            continue;
        }
        if (s->n_opaque_colors <= 256u) {
            uint32_t key = px_pack(p);           /* alpha 255: never 0 */
            uint32_t h = hash32(key) & (PC_QUANT_STATS_SET - 1u);
            while (s->set[h] && s->set[h] != key) h = (h + 1u) & (PC_QUANT_STATS_SET - 1u);
            if (!s->set[h]) { s->set[h] = key; s->n_opaque_colors++; }
        }
    }
    s->pixels += n;
}

uint32_t pc_quant_choose_depth(const pc_quant_stats *s, uint32_t allowed)
{
    if (s->all_opaque) {
        uint32_t n = s->n_opaque_colors;
        if ((allowed & PC_QD_1) && n <= 2u) return 1;
        if ((allowed & PC_QD_2) && n <= 4u) return 2;
        if ((allowed & PC_QD_4) && n <= 16u) return 4;
        if ((allowed & PC_QD_8) && n <= 256u) return 8;
        if (allowed & PC_QD_24) return 24;
    }
    if (allowed & PC_QD_32) return 32;
    if (allowed & PC_QD_24) return 24;
    if (allowed & PC_QD_8) return 8;
    if (allowed & PC_QD_4) return 4;
    if (allowed & PC_QD_2) return 2;
    return 1;
}

void pc_quant_prepare_row(pc_px32 *row, size_t n, int32_t threshold)
{
    static const pc_px32 white = { 255, 255, 255, 255 };
    for (size_t i = 0; i < n; i++) {
        pc_px32 p = row[i], acc = white;
        if (threshold > 0 && (int32_t)p.a < threshold) {
            memset(&row[i], 0, sizeof row[i]);
            continue;
        }
        if (p.a == 255) continue;
        pc_composite_span(&acc, &p, 1u, PC_BLEND_NORMAL, 255u);
        row[i] = acc;
    }
}

/* ---- band flattener ------------------------------------------------------------------ */
pc_status pc_flat_init(pc_flat *f, const pc_doc *d, const pc_par *par)
{
    size_t n;
    memset(f, 0, sizeof *f);
    if (!d || d->w == 0u || d->h == 0u) return PC_ERR_ARG;
    if (!pc_mul_size(d->w, PC_TILE_DIM, &n)) return PC_ERR_LIMIT;
    f->band = (pc_px32 *)malloc(n * sizeof *f->band);
    if (!f->band) return PC_ERR_NOMEM;
    f->d = d;
    f->par = par;
    f->y0 = -1;
    return PC_OK;
}

pc_px32 *pc_flat_row(pc_flat *f, uint32_t y)
{
    int32_t by;
    if (!f->band || y >= f->d->h) return NULL;
    by = (int32_t)(y & ~(PC_TILE_DIM - 1u));
    if (by != f->y0) {
        uint32_t rows = f->d->h - (uint32_t)by;
        if (rows > PC_TILE_DIM) rows = PC_TILE_DIM;
        if (pc_comp_rect(f->d, pc_rect_make(0, by, (int32_t)f->d->w, (int32_t)rows), f->band,
                         f->d->w, f->par) != PC_OK)
            return NULL;
        f->y0 = by;
    }
    return f->band + (size_t)(y - (uint32_t)by) * f->d->w;
}

void pc_flat_free(pc_flat *f)
{
    if (!f) return;
    free(f->band);
    memset(f, 0, sizeof *f);
}

/* ---- row sink ------------------------------------------------------------------------ */
static bool rs_transposed(const pc_rowsink *s) { return s->orient >= 5u; }
static pc_doc *rs_bdoc(const pc_rowsink *s) { return s->stage ? s->stage : s->doc; }
static pc_layer *rs_blayer(const pc_rowsink *s) { return s->stage ? s->slayer : s->layer; }

pc_status pc_rowsink_init(pc_rowsink *s, const pc_codec_limits *lim, uint32_t sw,
                          uint32_t sh, uint32_t orient)
{
    pc_codec_limits dl;
    pc_status st;
    size_t n;
    memset(s, 0, sizeof *s);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    st = pc_codec_check_size(lim, sw, sh, 1u);
    if (st != PC_OK) return st;
    s->orient = (orient >= 1u && orient <= 8u) ? orient : 1u;
    s->sw = sw; s->sh = sh;
    s->dw = rs_transposed(s) ? sh : sw;
    s->dh = rs_transposed(s) ? sw : sh;
    s->band_y0 = -1;
    if (!pc_mul_size(sw, PC_TILE_DIM, &n)) return PC_ERR_LIMIT;
    s->doc = pc_doc_create(s->dw, s->dh);
    if (!s->doc) goto nomem;
    s->layer = pc_layer_create(s->doc, "Background");
    if (!s->layer) goto nomem;
    if (rs_transposed(s)) {
        s->stage = pc_doc_create(sw, sh);
        if (!s->stage) goto nomem;
        s->slayer = pc_layer_create(s->stage, "stage");
        if (!s->slayer) goto nomem;
    }
    s->band = (pc_px32 *)malloc(n * sizeof *s->band);
    s->stored = (uint8_t *)calloc(rs_bdoc(s)->tiles_y, 1u);
    if (!s->band || !s->stored) goto nomem;
    return PC_OK;
nomem:
    pc_rowsink_abort(s);
    return PC_ERR_NOMEM;
}

static pc_status rs_flush(pc_rowsink *s)
{
    uint32_t rows;
    pc_status st;
    if (s->band_y0 < 0 || !s->dirty) return PC_OK;
    rows = rs_bdoc(s)->h - (uint32_t)s->band_y0;
    if (rows > PC_TILE_DIM) rows = PC_TILE_DIM;
    st = pc_layer_store_rect(rs_bdoc(s), rs_blayer(s),
                             pc_rect_make(0, s->band_y0, (int32_t)s->sw, (int32_t)rows),
                             s->band, s->sw);
    s->stored[(uint32_t)s->band_y0 >> PC_TILE_SHIFT] = 1u;
    s->dirty = false;
    return st;
}

pc_status pc_rowsink_put(pc_rowsink *s, uint32_t sy, const pc_px32 *row)
{
    uint32_t y;
    int32_t by;
    pc_px32 *d;
    if (!s->doc || sy >= s->sh) return PC_OK;
    y = (s->orient == 3u || s->orient == 4u) ? s->sh - 1u - sy : sy;
    by = (int32_t)(y & ~(PC_TILE_DIM - 1u));
    if (by != s->band_y0) {
        pc_status st = rs_flush(s);
        uint32_t rows = rs_bdoc(s)->h - (uint32_t)by;
        if (st != PC_OK) return st;
        if (rows > PC_TILE_DIM) rows = PC_TILE_DIM;
        if (s->stored[(uint32_t)by >> PC_TILE_SHIFT])
            pc_layer_read_rect(rs_bdoc(s), rs_blayer(s),
                               pc_rect_make(0, by, (int32_t)s->sw, (int32_t)rows), s->band, s->sw);
        else
            memset(s->band, 0, (size_t)rows * s->sw * sizeof *s->band);
        s->band_y0 = by;
    }
    d = s->band + (size_t)(y - (uint32_t)by) * s->sw;
    if (s->orient == 2u || s->orient == 3u) {
        for (uint32_t x = 0; x < s->sw; x++) d[s->sw - 1u - x] = row[x];
    } else {
        memcpy(d, row, (size_t)s->sw * sizeof *d);
    }
    s->dirty = true;
    return PC_OK;
}

/* Transpose the staged layer into the result, one 64-row result band at a
 * time. Result row y is staged column c, result column x is staged row r. */
static pc_status rs_transpose(pc_rowsink *s)
{
    pc_px32 *col = NULL, *out = NULL;
    size_t nc, no;
    pc_status st = PC_OK;
    bool flip_c = s->orient == 7u || s->orient == 8u;   /* c = sw - 1 - y */
    bool flip_r = s->orient == 6u || s->orient == 7u;   /* r = sh - 1 - x */
    if (!pc_mul_size(s->sh, PC_TILE_DIM, &nc) || !pc_mul_size(s->dw, PC_TILE_DIM, &no))
        return PC_ERR_LIMIT;
    col = (pc_px32 *)malloc(nc * sizeof *col);
    out = (pc_px32 *)malloc(no * sizeof *out);
    if (!col || !out) { st = PC_ERR_NOMEM; goto done; }
    for (uint32_t y0 = 0; y0 < s->dh && st == PC_OK; y0 += PC_TILE_DIM) {
        uint32_t rows = s->dh - y0 < PC_TILE_DIM ? s->dh - y0 : PC_TILE_DIM;
        uint32_t c0 = flip_c ? s->sw - y0 - rows : y0;            /* staged columns */
        uint32_t tx0 = c0 >> PC_TILE_SHIFT, tx1 = (c0 + rows - 1u) >> PC_TILE_SHIFT;
        bool any = false;
        for (uint32_t ty = 0; ty < s->slayer->tiles_y && !any; ty++)
            for (uint32_t tx = tx0; tx <= tx1 && !any; tx++)
                any = s->slayer->grid[(size_t)ty * s->slayer->tiles_x + tx] != NULL;
        if (!any) continue;                                         /* all transparent */
        pc_layer_read_rect(s->stage, s->slayer,
                           pc_rect_make((int32_t)c0, 0, (int32_t)rows, (int32_t)s->sh), col, rows);
        for (uint32_t j = 0; j < rows; j++) {
            uint32_t c = flip_c ? (s->sw - 1u - (y0 + j)) - c0 : (y0 + j) - c0;
            pc_px32 *o = out + (size_t)j * s->dw;
            for (uint32_t x = 0; x < s->dw; x++) {
                uint32_t r = flip_r ? s->sh - 1u - x : x;
                o[x] = col[(size_t)r * rows + c];
            }
        }
        st = pc_layer_store_rect(s->doc, s->layer,
                                 pc_rect_make(0, (int32_t)y0, (int32_t)s->dw, (int32_t)rows),
                                 out, s->dw);
    }
done:
    free(col);
    free(out);
    return st;
}

static pc_status rs_force_opaque(pc_rowsink *s)
{
    pc_layer *l = s->layer;
    for (uint32_t ty = 0; ty < l->tiles_y; ty++) {
        for (uint32_t tx = 0; tx < l->tiles_x; tx++) {
            pc_tile **slot = &l->grid[(size_t)ty * l->tiles_x + tx];
            uint32_t w = s->dw - tx * PC_TILE_DIM, h = s->dh - ty * PC_TILE_DIM;
            pc_px32 *p;
            if (w > PC_TILE_DIM) w = PC_TILE_DIM;
            if (h > PC_TILE_DIM) h = PC_TILE_DIM;
            if (!*slot) {
                *slot = pc_tile_new_zero(4u);
                if (!*slot) return PC_ERR_NOMEM;
            }
            PC_ASSERT(pc_tile_refs(*slot) == 1u);
            p = (pc_px32 *)(void *)(*slot)->data;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) p[y * PC_TILE_DIM + x].a = 255u;
        }
    }
    l->gen++;
    return PC_OK;
}

pc_status pc_rowsink_finish(pc_rowsink *s, pc_doc **out)
{
    pc_status st;
    if (out) *out = NULL;
    if (!s->doc || !out) { pc_rowsink_abort(s); return PC_ERR_ARG; }
    st = rs_flush(s);
    if (st == PC_OK && s->stage) st = rs_transpose(s);
    if (st == PC_OK && s->force_opaque) st = rs_force_opaque(s);
    if (st == PC_OK) st = pc_doc_insert_layer(s->doc, s->layer, 0u);
    if (st != PC_OK) { pc_rowsink_abort(s); return st; }
    *out = s->doc;
    s->doc = NULL;
    s->layer = NULL;
    pc_rowsink_abort(s);                  /* frees the stage and the buffers */
    return PC_OK;
}

void pc_rowsink_abort(pc_rowsink *s)
{
    if (!s) return;
    pc_layer_destroy(s->layer);
    pc_doc_destroy(s->doc);
    pc_layer_destroy(s->slayer);
    pc_doc_destroy(s->stage);
    free(s->band);
    free(s->stored);
    memset(s, 0, sizeof *s);
}
