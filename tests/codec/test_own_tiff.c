/* test_own_tiff.c - own TIFF reader and writer (src/codec/fmt_tiff.c):
 * hand-built files for every photometric, depth, endianness, compression,
 * predictor, layout (strips, tiles, planes), orientation and extension the
 * reader supports, malformed and hostile files (IFD bounds and loops, huge
 * geometry with tiny data), round trips for every save option, mutation
 * fuzzing. Also a standalone fuzz driver. */
#include "test_own_common.h"

extern const pc_codec pc_codec_tiff;
#define C (&pc_codec_tiff)

typedef struct tiff_params { int32_t depth, compression, dither, palette; } tiff_params;

/* ---- file builder ----------------------------------------------------------------- */
#define MAXE 40
#define MAXSEG 1024
typedef struct tent {
    uint32_t       tag, type, count;
    uint32_t       v[64];          /* numeric values (SHORT/LONG/BYTE; RATIONAL as pairs) */
    const uint32_t *vext;          /* or a longer external value array */
    const uint8_t *raw;            /* or raw bytes (UNDEFINED) */
} tent;

typedef struct tb {
    bool           be, tiles;
    tent           e[MAXE];
    int            ne;
    const uint8_t *seg[MAXSEG];
    size_t         seglen[MAXSEG];
    int            nseg;
    uint32_t       pages;          /* extra copies of the IFD chained after it */
    bool           loop;           /* the last IFD points at the first */
    bool           no_counts;      /* omit the byte counts */
} tb;

static void tb_init(tb *t, bool be) { memset(t, 0, sizeof *t); t->be = be; }

static void tb_add(tb *t, uint32_t tag, uint32_t type, uint32_t count, const uint32_t *v)
{
    tent *e = &t->e[t->ne++];
    e->tag = tag; e->type = type; e->count = count; e->raw = NULL; e->vext = NULL;
    if (count * (type == 5u ? 2u : 1u) > 64u) { e->vext = v; return; }
    for (uint32_t i = 0; i < count * (type == 5u ? 2u : 1u); i++) e->v[i] = v[i];
}

static void tb_1(tb *t, uint32_t tag, uint32_t type, uint32_t v) { tb_add(t, tag, type, 1, &v); }

static void tb_raw(tb *t, uint32_t tag, uint32_t count, const uint8_t *raw)
{
    tent *e = &t->e[t->ne++];
    e->tag = tag; e->type = 7; e->count = count; e->raw = raw; e->vext = NULL;
}

static void tb_seg(tb *t, const uint8_t *p, size_t n)
{
    t->seg[t->nseg] = p;
    t->seglen[t->nseg++] = n;
}

static void w16(pc_buf *b, bool be, uint32_t v) { if (be) bb_be16(b, v); else bb_le16(b, v); }
static void w32(pc_buf *b, bool be, uint32_t v) { if (be) bb_be32(b, v); else bb_le32(b, v); }
static void set32(pc_buf *b, bool be, size_t at, uint32_t v)
{
    if (be) {
        b->p[at] = (uint8_t)(v >> 24);
        b->p[at + 1] = (uint8_t)(v >> 16);
        b->p[at + 2] = (uint8_t)(v >> 8);
        b->p[at + 3] = (uint8_t)v;
    }
    else bb_set_le32(b, at, v);
}

static uint32_t tsize(uint32_t type)
{
    return type == 3u ? 2u : type == 4u ? 4u : type == 5u ? 8u : 1u;
}

static void put_vals(pc_buf *b, bool be, const tent *e)
{
    const uint32_t *v = e->vext ? e->vext : e->v;
    for (uint32_t i = 0; i < e->count; i++) {
        if (e->raw) bb_u8(b, e->raw[i]);
        else if (e->type == 3u) w16(b, be, v[i]);
        else if (e->type == 4u) w32(b, be, v[i]);
        else if (e->type == 5u) { w32(b, be, v[2 * i]); w32(b, be, v[2 * i + 1]); }
        else bb_u8(b, v[i]);
    }
}

static int cmp_tent(const void *a, const void *b)
{
    uint32_t x = ((const tent *)a)->tag, y = ((const tent *)b)->tag;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void tb_build(pc_buf *b, tb *t)
{
    size_t ifd_pos, link_pos = 0, first_ifd = 0;
    uint32_t offs[MAXSEG], cnts[MAXSEG];
    memset(b, 0, sizeof *b);
    if (t->be) bb_bytes(b, "MM\0*", 4); else bb_bytes(b, "II*\0", 4);
    w32(b, t->be, 0);
    for (int i = 0; i < t->nseg; i++) {
        offs[i] = (uint32_t)b->n;
        cnts[i] = (uint32_t)t->seglen[i];
        bb_bytes(b, t->seg[i], t->seglen[i]);
        if (b->n & 1u) bb_u8(b, 0);
    }
    if (t->nseg) {
        tb_add(t, t->tiles ? 324u : 273u, 4, (uint32_t)t->nseg, offs);
        if (!t->no_counts) tb_add(t, t->tiles ? 325u : 279u, 4, (uint32_t)t->nseg, cnts);
    }
    qsort(t->e, (size_t)t->ne, sizeof t->e[0], cmp_tent);
    for (uint32_t page = 0; page <= t->pages; page++) {
        size_t ext[MAXE];
        if (b->n & 1u) bb_u8(b, 0);
        ifd_pos = b->n;
        if (page == 0) { first_ifd = ifd_pos; set32(b, t->be, 4, (uint32_t)ifd_pos); }
        else set32(b, t->be, link_pos, (uint32_t)ifd_pos);
        w16(b, t->be, (uint32_t)t->ne);
        for (int i = 0; i < t->ne; i++) {
            const tent *e = &t->e[i];
            w16(b, t->be, e->tag); w16(b, t->be, e->type); w32(b, t->be, e->count);
            if (e->count * tsize(e->type) <= 4u) {
                size_t start = b->n;
                put_vals(b, t->be, e);
                while (b->n < start + 4u) bb_u8(b, 0);
                ext[i] = 0;
            } else {
                ext[i] = b->n;
                w32(b, t->be, 0);
            }
        }
        link_pos = b->n;
        w32(b, t->be, 0);
        for (int i = 0; i < t->ne; i++)
            if (ext[i]) {
                if (b->n & 1u) bb_u8(b, 0);
                set32(b, t->be, ext[i], (uint32_t)b->n);
                put_vals(b, t->be, &t->e[i]);
            }
    }
    if (t->loop) set32(b, t->be, link_pos, (uint32_t)first_ifd);
}

/* Basic tags: size, bits, spp, photometric, compression, rows per strip. */
static void tb_basic(tb *t, uint32_t w, uint32_t h, uint32_t bps, uint32_t spp, uint32_t photo,
                     uint32_t comp, uint32_t rps)
{
    uint32_t bv[64];
    for (int i = 0; i < 64; i++) bv[i] = bps;
    tb_1(t, 256, 4, w);
    tb_1(t, 257, 3, h);
    tb_add(t, 258, 3, spp, bv);
    tb_1(t, 259, 3, comp);
    tb_1(t, 262, 3, photo);
    tb_1(t, 277, 3, spp);
    if (rps) tb_1(t, 278, 4, rps);
}

/* ---- sample data ------------------------------------------------------------------ */
static void put_sample(uint8_t *row, uint32_t i, uint32_t bps, bool be, uint32_t v)
{
    if (bps == 8u) row[i] = (uint8_t)v;
    else if (bps == 16u) {
        if (be) { row[2 * i] = (uint8_t)(v >> 8); row[2 * i + 1] = (uint8_t)v; }
        else { row[2 * i] = (uint8_t)v; row[2 * i + 1] = (uint8_t)(v >> 8); }
    } else if (bps == 32u) {
        for (int k = 0; k < 4; k++)
            row[4 * i + (uint32_t)(be ? 3 - k : k)] = (uint8_t)(v >> (8 * k));
    } else {
        uint32_t bit = i * bps;
        row[bit / 8u] |= (uint8_t)(v << (8u - bps - bit % 8u));
    }
}

/* Rows of samples (plane < 0: all samples interleaved) for rows y0..y0+rows
 * and columns x0..x0+cols of a w-wide sample grid; row length rowpx pixels. */
static size_t pack(uint8_t *out, const uint32_t *s, uint32_t w, uint32_t spp, uint32_t bps,
                   bool be, int plane, uint32_t x0, uint32_t y0, uint32_t cols, uint32_t rows,
                   uint32_t rowpx)
{
    uint32_t per = plane < 0 ? spp : 1u;
    size_t rb = ((size_t)rowpx * per * bps + 7u) / 8u;
    memset(out, 0, rb * rows);
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < cols; x++)
            for (uint32_t k = 0; k < per; k++) {
                uint32_t sk = plane < 0 ? k : (uint32_t)plane;
                put_sample(out + rb * y, x * per + k, bps, be,
                           s[((size_t)(y0 + y) * w + x0 + x) * spp + sk]);
            }
    return rb * rows;
}

static uint32_t to8(uint32_t v, uint32_t bps)
{
    if (bps == 16u) return (v * 255u + 32767u) / 65535u;
    if (bps == 32u) return ((v >> 16) * 255u + 32767u) / 65535u;
    return v * 255u / ((1u << bps) - 1u);
}

static uint32_t *rand_samples(uint32_t n, uint32_t bps)
{
    uint32_t *s = (uint32_t *)malloc(n * sizeof *s);
    for (uint32_t i = 0; i < n; i++)
        s[i] = bps >= 32u ? (uint32_t)rnd() : (uint32_t)(rnd() & ((1u << bps) - 1u));
    return s;
}

static pc_px32 unpremul(pc_px32 p)
{
    uint32_t a = p.a;
    if (a == 0u) return mkpx(0, 0, 0, 0);
    if (a == 255u) return p;
    p.r = (uint8_t)(p.r >= a ? 255u : (p.r * 255u + a / 2u) / a);
    p.g = (uint8_t)(p.g >= a ? 255u : (p.g * 255u + a / 2u) / a);
    p.b = (uint8_t)(p.b >= a ? 255u : (p.b * 255u + a / 2u) / a);
    return p;
}

static pc_px32 *load_px(const pc_buf *b, uint32_t w, uint32_t h, pc_image_meta *m, pc_status *st)
{
    pc_doc *d = NULL;
    pc_image_meta mm;
    pc_px32 *px = NULL;
    pc_status s = codec_load(C, b->p, b->n, &d, &mm);
    if (st) *st = s;
    if (s != PC_OK) return NULL;
    if (d->w == w && d->h == h) px = doc_layer0(d);
    else INFO("unexpected size %ux%u", d->w, d->h);
    if (m) *m = mm; else pc_meta_free(&mm);
    pc_doc_destroy(d);
    return px;
}

/* ---- encoders for strips ---------------------------------------------------------- */
static void packbits(pc_buf *o, const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128u && p[i + run] == p[i]) run++;
        if (rndu(5) == 0) bb_u8(o, 0x80);                    /* -128: no operation */
        if (run >= 2u) {
            bb_u8(o, (uint32_t)(257u - run) & 255u);
            bb_u8(o, p[i]);
            i += run;
        } else {
            size_t k = 1;
            while (i + k < n && k < 128u && !(i + k + 1u < n && p[i + k] == p[i + k + 1u])) k++;
            bb_u8(o, (uint32_t)(k - 1u));
            bb_bytes(o, p + i, k);
            i += k;
        }
    }
}

static void zlib_stored(pc_buf *o, const uint8_t *p, size_t n)
{
    uint32_t a = 1, b2 = 0;
    size_t i = 0;
    bb_u8(o, 0x78); bb_u8(o, 0x01);
    do {
        size_t len = n - i > 7000u ? 7000u : n - i;        /* several blocks */
        bb_u8(o, i + len == n ? 1u : 0u);
        bb_le16(o, (uint32_t)len); bb_le16(o, (uint32_t)(~len & 0xFFFFu));
        bb_bytes(o, p + i, len);
        i += len;
    } while (i < n);
    for (i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b2 = (b2 + a) % 65521u; }
    bb_be32(o, (b2 << 16) | a);
}

static void difference(uint8_t *row, size_t rb, uint32_t stride, uint32_t bps, bool be)
{
    if (bps == 8u) {
        for (size_t i = rb; i-- > stride;) row[i] = (uint8_t)(row[i] - row[i - stride]);
    } else {
        size_t ns = rb / 2u;
        for (size_t i = ns; i-- > stride;) {
            uint8_t *q = row + 2 * i, *r = row + 2 * (i - stride);
            uint32_t v = be ? (uint32_t)(q[0] << 8 | q[1]) : (uint32_t)(q[0] | q[1] << 8);
            uint32_t u = be ? (uint32_t)(r[0] << 8 | r[1]) : (uint32_t)(r[0] | r[1] << 8);
            v = (v - u) & 0xFFFFu;
            if (be) { q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; }
            else { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); }
        }
    }
}

/* comp: 1 none, 5 LZW, 50 old-style LZW (tagged 5), 32773 PackBits, 8 and 32946 Deflate */
static void encode_seg(pc_buf *o, const uint8_t *raw, size_t n, uint32_t comp)
{
    memset(o, 0, sizeof *o);
    switch (comp) {
    case 5: ref_lzw_encode(o, raw, n, 8, 1, false, true); break;
    case 50: ref_lzw_encode(o, raw, n, 8, 2, false, true); break;
    case 32773: packbits(o, raw, n); break;
    case 8: case 32946: zlib_stored(o, raw, n); break;
    default: bb_bytes(o, raw, n); break;
    }
}

/* ---- decoder tests --------------------------------------------------------------------- */
/* Generic strip image: photometric, bps, spp, extra sample type (-1 = no
 * tag), endianness, planar, compression, predictor; expected pixels are
 * computed from the samples. */
typedef struct gcase {
    uint32_t photo, bps, spp;
    int      extra;         /* ExtraSamples value, -1 = no tag */
    bool     be;
    uint32_t planar, comp, pred, rps;
    uint32_t sfmt;
} gcase;

static pc_px32 expect_px(const gcase *g, const uint32_t *s, const pc_px32 *pal)
{
    uint32_t cc = g->photo == 2u ? 3u : g->photo == 5u ? 4u : 1u;
    pc_px32 p;
    uint32_t c[4];
    bool alpha = g->spp > cc && (g->extra == 1 || g->extra == 2 ||
                                 (g->extra < 0 && g->photo == 2u && g->spp == 4u));
    for (uint32_t k = 0; k < cc; k++) c[k] = to8(s[k], g->bps);
    switch (g->photo) {
    case 0: p = mkpx(255u - c[0], 255u - c[0], 255u - c[0], 255); break;
    case 1: p = mkpx(c[0], c[0], c[0], 255); break;
    case 2: p = mkpx(c[0], c[1], c[2], 255); break;
    case 3: p = pal[s[0]]; break;
    default:
        p = mkpx(pc_mul255(255u - c[0], 255u - c[3]), pc_mul255(255u - c[1], 255u - c[3]),
                 pc_mul255(255u - c[2], 255u - c[3]), 255);
        break;
    }
    if (g->photo == 0u && g->bps == 16u) {     /* inversion happens before rounding */
        uint32_t v = (65535u - s[0]) * 255u + 32767u;
        v /= 65535u;
        p = mkpx(v, v, v, 255);
    }
    if (alpha) {
        p.a = (uint8_t)to8(s[cc], g->bps);
        if (g->extra == 1 || g->extra < 0) p = unpremul(p);
    }
    return p;
}

static void run_case(const gcase *g, uint32_t W, uint32_t H, const char *what)
{
    uint32_t *s = rand_samples(W * H * g->spp, g->bps);
    uint32_t planes = g->planar == 2u ? g->spp : 1u, rps = g->rps ? g->rps : H;
    uint32_t nstrips = (H + rps - 1u) / rps;
    pc_px32 pal[256], *ref = (pc_px32 *)malloc(W * H * sizeof *ref), *got;
    uint32_t cmap[3 * 256];
    pc_buf segs[MAXSEG], b;
    uint8_t *raw = (uint8_t *)malloc((size_t)W * H * g->spp * 4u + 64u);
    tb t;
    int ns = 0;
    tb_init(&t, g->be);
    tb_basic(&t, W, H, g->bps, g->spp, g->photo, g->comp == 50u ? 5u : g->comp, g->rps);
    if (g->planar == 2u) tb_1(&t, 284, 3, 2);
    if (g->pred) tb_1(&t, 317, 3, g->pred);
    if (g->sfmt) tb_1(&t, 339, 3, g->sfmt);
    if (g->extra >= 0) tb_1(&t, 338, 3, (uint32_t)g->extra);
    if (g->photo == 3u) {
        uint32_t n = 1u << g->bps;
        for (uint32_t i = 0; i < n; i++) {
            pal[i] = mkpx(rnd8(), rnd8(), rnd8(), 255);
            cmap[i] = pal[i].r * 257u; cmap[n + i] = pal[i].g * 257u;
            cmap[2 * n + i] = pal[i].b * 257u;
        }
        tb_add(&t, 320, 3, 3u * n, cmap);
    }
    for (uint32_t si = 0; si < nstrips * planes; si++) {
        uint32_t strip = si % nstrips, plane = si / nstrips;
        uint32_t y0 = strip * rps, rows = H - y0 < rps ? H - y0 : rps;
        size_t n = pack(raw, s, W, g->spp, g->bps, g->be, g->planar == 2u ? (int)plane : -1, 0, y0,
                        W, rows, W);
        if (g->pred == 2u) {
            size_t rb = n / rows;
            for (uint32_t r = 0; r < rows; r++)
                difference(raw + rb * r, rb, g->planar == 2u ? 1u : g->spp, g->bps, g->be);
        }
        encode_seg(&segs[ns], raw, n, g->comp);
        tb_seg(&t, segs[ns].p, segs[ns].n);
        ns++;
    }
    tb_build(&b, &t);
    for (uint32_t i = 0; i < W * H; i++) ref[i] = expect_px(g, s + (size_t)i * g->spp, pal);
    {
        pc_status st;
        got = load_px(&b, W, H, NULL, &st);
        if (!got || memcmp(got, ref, W * H * sizeof *ref) != 0) {
            INFO("case %s: photo %u bps %u spp %u extra %d be %d planar %u comp %u pred %u: %s",
                 what, g->photo, g->bps, g->spp, g->extra, g->be, g->planar, g->comp, g->pred,
                 got ? "pixels differ" : pc_status_str(st));
            if (got) INFO("  first pixel got %u,%u,%u,%u want %u,%u,%u,%u", got[0].r, got[0].g,
                          got[0].b, got[0].a, ref[0].r, ref[0].g, ref[0].b, ref[0].a);
        }
        CHECK(got && memcmp(got, ref, W * H * sizeof *ref) == 0);
    }
    free(got);
    for (int i = 0; i < ns; i++) pc_buf_free(&segs[i]);
    pc_buf_free(&b);
    free(raw);
    free(ref);
    free(s);
}

static void t_photometric(void)
{
    static const uint32_t gb[] = { 1, 2, 4, 8, 16 };
    for (int be = 0; be < 2; be++) {
        for (size_t i = 0; i < 5; i++)
            for (uint32_t ph = 0; ph < 2; ph++) {
                gcase g = { ph, gb[i], 1, -1, be != 0, 1, 1, 0, 0, 0 };
                run_case(&g, 13, 7, "gray");
                if (gb[i] >= 8u) {
                    gcase ga = { ph, gb[i], 2, 2, be != 0, 1, 1, 0, 0, 0 };
                    run_case(&ga, 13, 7, "gray+alpha");
                }
            }
        for (uint32_t bps = 8; bps <= 16; bps += 8)
            for (uint32_t planar = 1; planar <= 2; planar++) {
                gcase rgb = { 2, bps, 3, -1, be != 0, planar, 1, 0, 0, 0 };
                gcase un = { 2, bps, 4, 2, be != 0, planar, 1, 0, 0, 0 };
                gcase as = { 2, bps, 4, 1, be != 0, planar, 1, 0, 0, 0 };
                gcase nt = { 2, bps, 4, -1, be != 0, planar, 1, 0, 0, 0 };
                gcase ig = { 2, bps, 5, 0, be != 0, planar, 1, 0, 0, 0 };
                gcase ck = { 5, bps, 4, -1, be != 0, planar, 1, 0, 0, 0 };
                gcase ca = { 5, bps, 5, 2, be != 0, planar, 1, 0, 0, 0 };
                run_case(&rgb, 11, 6, "rgb");
                run_case(&un, 11, 6, "rgba unassociated");
                run_case(&as, 11, 6, "rgba associated");
                run_case(&nt, 11, 6, "rgba without ExtraSamples");
                run_case(&ig, 11, 6, "rgb + unspecified extra");
                run_case(&ck, 11, 6, "cmyk");
                run_case(&ca, 11, 6, "cmyk + alpha");
            }
        for (uint32_t bps = 1; bps <= 8; bps *= 2) {
            gcase p = { 3, bps, 1, -1, be != 0, 1, 1, 0, 0, 0 };
            gcase pa = { 3, bps, 2, 2, be != 0, 1, 1, 0, 0, 0 };
            run_case(&p, 17, 5, "palette");
            if (bps == 8u) run_case(&pa, 17, 5, "palette + alpha");
        }
        {
            gcase u32 = { 2, 32, 3, -1, be != 0, 1, 1, 0, 0, 0 };
            run_case(&u32, 9, 4, "32-bit unsigned");
        }
    }
}

static void t_float(void)
{
    const float vals[8] = { 0.0f, 1.0f, 0.5f, -3.0f, 7.0f, 0.25f, 0.999f, 0.0f };
    uint8_t data[8 * 4];
    uint32_t nan_bits = 0x7FC00000u;
    tb t;
    pc_buf b;
    pc_px32 *got;
    for (int i = 0; i < 8; i++) {
        uint32_t u;
        memcpy(&u, &vals[i], 4);
        if (i == 7) u = nan_bits;
        put_sample(data, (uint32_t)i, 32, false, u);
    }
    tb_init(&t, false);
    tb_basic(&t, 8, 1, 32, 1, 1, 1, 0);
    tb_1(&t, 339, 3, 3);
    tb_seg(&t, data, sizeof data);
    tb_build(&b, &t);
    got = load_px(&b, 8, 1, NULL, NULL);
    CHECK(got != NULL);
    if (got) {
        static const uint8_t want[8] = { 0, 255, 128, 0, 255, 64, 255, 0 };
        for (int i = 0; i < 8; i++) CHECK(got[i].g == want[i] && got[i].a == 255);
    }
    free(got);
    pc_buf_free(&b);
}

static void t_compression(void)
{
    static const uint32_t comps[] = { 1, 5, 50, 32773, 8, 32946 };
    for (size_t c = 0; c < 6; c++)
        for (uint32_t bps = 8; bps <= 16; bps += 8)
            for (uint32_t pred = 0; pred <= 2; pred += 2)
                for (int be = 0; be < 2; be++) {
                    gcase g = { 2, bps, 4, 2, be != 0, 1, comps[c], pred, 3, 0 };
                    gcase pl = { 2, bps, 3, -1, be != 0, 2, comps[c], pred, 4, 0 };
                    if (pred && (comps[c] == 1u || comps[c] == 32773u)) continue;
                    run_case(&g, 23, 10, "compressed rgba");
                    run_case(&pl, 23, 10, "compressed planar");
                }
    {   /* predictor tags on uncompressed data are ignored, like libtiff */
        gcase g = { 2, 8, 3, -1, false, 1, 1, 0, 0, 0 };
        uint32_t *s = rand_samples(5 * 3 * 3, 8);
        uint8_t raw[64];
        tb t;
        pc_buf b;
        pc_px32 *got;
        size_t n = pack(raw, s, 5, 3, 8, false, -1, 0, 0, 5, 3, 5);
        tb_init(&t, false);
        tb_basic(&t, 5, 3, 8, 3, 2, 1, 0);
        tb_1(&t, 317, 3, 2);
        tb_seg(&t, raw, n);
        tb_build(&b, &t);
        got = load_px(&b, 5, 3, NULL, NULL);
        CHECK(got && px_same(got[14], expect_px(&g, s + 14 * 3, NULL)));
        free(got);
        free(s);
        pc_buf_free(&b);
    }
}

static void t_tiles(void)
{
    struct { uint32_t w, h, tw, th, comp, planar; } cases[] = {
        { 37, 23, 16, 16, 1, 1 }, { 37, 23, 16, 16, 5, 1 }, { 37, 23, 16, 16, 8, 2 },
        { 37, 23, 64, 64, 1, 1 }, { 37, 150, 16, 160, 32773, 1 }, { 70, 70, 32, 16, 5, 2 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const uint32_t W = cases[c].w, H = cases[c].h, TW = cases[c].tw, TH = cases[c].th;
        const uint32_t spp = 4, planes = cases[c].planar == 2u ? spp : 1u;
        uint32_t *s = rand_samples(W * H * spp, 8);
        uint32_t across = (W + TW - 1) / TW, down = (H + TH - 1) / TH;
        uint8_t *raw = (uint8_t *)malloc((size_t)TW * TH * spp + 16u);
        pc_buf segs[MAXSEG], b;
        pc_px32 *got;
        gcase g = { 2, 8, 4, 2, false, cases[c].planar, cases[c].comp, 0, 0, 0 };
        bool ok = true;
        tb t;
        int ns = 0;
        tb_init(&t, false);
        t.tiles = true;
        tb_basic(&t, W, H, 8, spp, 2, cases[c].comp, 0);
        tb_1(&t, 322, 3, TW);
        tb_1(&t, 323, 3, TH);
        tb_1(&t, 338, 3, 2);
        if (planes > 1u) tb_1(&t, 284, 3, 2);
        for (uint32_t p = 0; p < planes; p++)
            for (uint32_t ty = 0; ty < down; ty++)
                for (uint32_t tx = 0; tx < across; tx++) {
                    uint32_t cols = W - tx * TW < TW ? W - tx * TW : TW;
                    uint32_t rows = H - ty * TH < TH ? H - ty * TH : TH;
                    size_t rb = (size_t)TW * (planes > 1u ? 1u : spp), n;
                    /* pad the tile to its full size (garbage outside the image) */
                    memset(raw, 0xA5, rb * TH);
                    pack(raw, s, W, spp, 8, false, planes > 1u ? (int)p : -1, tx * TW, ty * TH,
                         cols, rows, TW);
                    for (uint32_t r = rows; r < TH; r++) memset(raw + rb * r, 0x5A, rb);
                    n = rb * TH;
                    encode_seg(&segs[ns], raw, n, cases[c].comp);
                    tb_seg(&t, segs[ns].p, segs[ns].n);
                    ns++;
                }
        tb_build(&b, &t);
        got = load_px(&b, W, H, NULL, NULL);
        CHECK(got != NULL);
        for (uint32_t i = 0; got && i < W * H; i++)
            ok = ok && px_same(got[i], expect_px(&g, s + (size_t)i * spp, NULL));
        if (!ok) INFO("tile case %u differs", (unsigned)c);
        CHECK(ok);
        free(got);
        for (int i = 0; i < ns; i++) pc_buf_free(&segs[i]);
        pc_buf_free(&b);
        free(raw);
        free(s);
    }
}

static void t_orientation_and_meta(void)
{
    const uint32_t W = 7, H = 4;
    uint32_t *s = rand_samples(W * H * 3, 8);
    uint8_t raw[7 * 4 * 3];
    size_t n = pack(raw, s, W, 3, 8, false, -1, 0, 0, W, H, W);
    gcase g = { 2, 8, 3, -1, false, 1, 1, 0, 0, 0 };
    for (uint32_t o = 1; o <= 9; o++) {
        tb t;
        pc_buf b;
        pc_px32 *got;
        uint32_t dw = o >= 5u && o <= 8u ? H : W, dh = o >= 5u && o <= 8u ? W : H;
        bool ok = true;
        tb_init(&t, o & 1u);
        tb_basic(&t, W, H, 8, 3, 2, 1, 0);
        tb_1(&t, 274, 3, o);
        tb_seg(&t, raw, n);
        tb_build(&b, &t);
        got = load_px(&b, dw, dh, NULL, NULL);
        CHECK(got != NULL);
        for (uint32_t y = 0; got && y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t sx, sy;
                switch (o) {
                case 2: sx = W - 1 - x; sy = y; break;
                case 3: sx = W - 1 - x; sy = H - 1 - y; break;
                case 4: sx = x; sy = H - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = H - 1 - x; break;
                case 7: sx = W - 1 - y; sy = H - 1 - x; break;
                case 8: sx = W - 1 - y; sy = x; break;
                default: sx = x; sy = y; break;            /* 1, and 9 = invalid */
                }
                ok = ok && px_same(got[y * dw + x], expect_px(&g, s + (sy * W + sx) * 3, NULL));
            }
        CHECK(ok);
        free(got);
        pc_buf_free(&b);
    }
    {   /* resolution units, ICC, pages, loops */
        struct { uint32_t unit; uint32_t xr[2], yr[2]; double dx, dy; } rc[] = {
            { 2, { 300, 1 }, { 150, 2 }, 300.0, 75.0 },
            { 3, { 100, 1 }, { 1, 0 }, 254.0, 0.0 },
            { 1, { 72, 1 }, { 72, 1 }, 0.0, 0.0 },
        };
        static uint8_t icc[200];
        for (int i = 0; i < 200; i++) icc[i] = (uint8_t)(i ^ 0x5A);
        for (int k = 0; k < 3; k++) {
            tb t;
            pc_buf b;
            pc_image_meta m;
            pc_px32 *got;
            tb_init(&t, k == 1);
            tb_basic(&t, W, H, 8, 3, 2, 1, 0);
            tb_add(&t, 282, 5, 1, rc[k].xr);
            tb_add(&t, 283, 5, 1, rc[k].yr);
            tb_1(&t, 296, 3, rc[k].unit);
            if (k == 0) tb_raw(&t, 34675, sizeof icc, icc);
            tb_seg(&t, raw, n);
            t.pages = k == 0 ? 2u : 0u;
            t.loop = k == 1;
            tb_build(&b, &t);
            got = load_px(&b, W, H, &m, NULL);
            CHECK(got != NULL);
            if (got) {
                CHECK(fabs(m.dpi_x - rc[k].dx) < 1e-9 && fabs(m.dpi_y - rc[k].dy) < 1e-9);
                CHECK((m.icc_len == sizeof icc && memcmp(m.icc, icc, sizeof icc) == 0) == (k == 0));
                CHECK((strstr(m.note, "first of 3 pages") != NULL) == (k == 0));
                CHECK(k == 0 || m.note[0] == 0);
                CHECK(m.src_bits == 8u && !m.had_alpha);
                pc_meta_free(&m);
            }
            free(got);
            pc_buf_free(&b);
        }
    }
    {   /* fill order 2: bit-reversed bytes */
        uint8_t bits[2] = { 0x01, 0x80 };     /* reversed: 0x80 0x01 -> pixels 1 0..0 / 0..0 1 */
        tb t;
        pc_buf b;
        pc_px32 *got;
        tb_init(&t, false);
        tb_basic(&t, 8, 2, 1, 1, 1, 1, 0);
        tb_1(&t, 266, 3, 2);
        tb_seg(&t, bits, 2);
        tb_build(&b, &t);
        got = load_px(&b, 8, 2, NULL, NULL);
        CHECK(got && got[0].r == 255 && got[1].r == 0 && got[15].r == 255 && got[8].r == 0);
        free(got);
        pc_buf_free(&b);
    }
    {   /* 8-bit valued color maps (libtiff's heuristic) and CMYK drops the ICC profile */
        uint32_t cm[3 * 4] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
        uint8_t px[1] = { 0x1B };            /* 2-bit indices 0 1 2 3 */
        uint8_t cmyk[4] = { 0, 255, 0, 0 };
        tb t;
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        tb_init(&t, true);
        tb_basic(&t, 4, 1, 2, 1, 3, 1, 0);
        tb_add(&t, 320, 3, 12, cm);
        tb_seg(&t, px, 1);
        tb_build(&b, &t);
        got = load_px(&b, 4, 1, NULL, NULL);
        CHECK(got && px_same(got[0], mkpx(10, 50, 90, 255)) &&
              px_same(got[3], mkpx(40, 80, 120, 255)));
        free(got);
        pc_buf_free(&b);
        tb_init(&t, false);
        tb_basic(&t, 1, 1, 8, 4, 5, 1, 0);
        tb_raw(&t, 34675, 4, cmyk);
        tb_seg(&t, cmyk, 4);
        tb_build(&b, &t);
        got = load_px(&b, 1, 1, &m, NULL);
        CHECK(got && px_same(got[0], mkpx(255, 0, 255, 255)) && m.icc == NULL);
        if (got) pc_meta_free(&m);
        free(got);
        pc_buf_free(&b);
    }
    free(s);
}

/* Find our own (little-endian) builder's value array of a LONG tag. */
static size_t le_array_pos(const pc_buf *b, uint32_t tag)
{
    uint32_t ifd = (uint32_t)(b->p[4] | b->p[5] << 8 | b->p[6] << 16 | (uint32_t)b->p[7] << 24);
    uint32_t n = (uint32_t)(b->p[ifd] | b->p[ifd + 1] << 8);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b->p + ifd + 2 + 12 * i;
        if ((uint32_t)(e[0] | e[1] << 8) == tag)
            return (size_t)(e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24);
    }
    return 0;
}

static void t_missing_data(void)
{
    const uint32_t W = 16, H = 12, RB = 16 * 3;
    uint32_t *s = rand_samples(W * H * 3, 8);
    uint8_t raw[16 * 12 * 3];
    gcase g = { 2, 8, 3, -1, false, 1, 1, 0, 0, 0 };
    pack(raw, s, W, 3, 8, false, -1, 0, 0, W, H, W);
    for (int mode = 0; mode < 4; mode++) {
        tb t;
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        tb_init(&t, false);
        tb_basic(&t, W, H, 8, 3, 2, 1, 4);                      /* 3 strips of 4 rows */
        tb_seg(&t, raw, RB * 4u);
        tb_seg(&t, raw + RB * 4u, mode == 1 ? RB * 2u + 5u : RB * 4u);
        tb_seg(&t, raw + RB * 8u, mode == 0 ? 7u : RB * 4u);
        t.no_counts = mode == 3;
        tb_build(&b, &t);
        if (mode == 2) bb_set_le32(&b, le_array_pos(&b, 273) + 4u, 0xFFFFFF00u);
        got = load_px(&b, W, H, &m, NULL);
        CHECK(got != NULL);
        if (got) {
            /* rows with complete data decode, the others stay transparent */
            bool ok = true;
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    bool have = mode == 0 ? y < 8u : mode == 1 ? (y < 6u || y >= 8u)
                              : mode == 2 ? (y < 4u || y >= 8u) : true;
                    pc_px32 want = have ? expect_px(&g, s + (y * W + x) * 3, NULL)
                                        : mkpx(0, 0, 0, 0);
                    ok = ok && px_same(got[y * W + x], want);
                }
            if (!ok) INFO("missing data mode %d differs", mode);
            CHECK(ok);
            CHECK((strstr(m.note, "incomplete") != NULL) == (mode < 3));
            pc_meta_free(&m);
        }
        free(got);
        pc_buf_free(&b);
    }
    free(s);
}

static uint8_t bitrev(uint8_t v)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = (uint8_t)(r | (((v >> i) & 1u) << (7 - i)));
    return r;
}

/* Fill order 2 (bits of every stored byte reversed) with every compression,
 * in strips and tiles; Deflate segments larger than the decoder's staging
 * buffer exercise its refills. */
static void t_fill_order(void)
{
    static const uint32_t comps[] = { 1, 5, 50, 32773, 8, 32946 };
    const uint32_t W = 64, H = 40, TW = 32, TH = 16, SPP = 3;
    uint32_t *s = rand_samples(W * H * SPP, 8);
    uint8_t *raw = (uint8_t *)malloc((size_t)W * H * SPP);
    gcase g = { 2, 8, 3, -1, false, 1, 1, 0, 0, 0 };
    for (size_t c = 0; c < sizeof comps / sizeof comps[0]; c++) {
        for (int tiled = 0; tiled < 2; tiled++) {
            uint32_t rps = c & 1u ? 0u : 7u;          /* one strip (> 4 KiB) or 7 rows */
            uint32_t across = W / TW, down = (H + TH - 1u) / TH;
            pc_buf segs[16], b;
            pc_px32 *got;
            bool ok = true;
            int ns = 0;
            tb t;
            tb_init(&t, c == 2u);
            tb_basic(&t, W, H, 8, SPP, 2, comps[c] == 50u ? 5u : comps[c], tiled ? 0u : rps);
            tb_1(&t, 266, 3, 2);
            t.tiles = tiled != 0;
            if (tiled) {
                tb_1(&t, 322, 3, TW);
                tb_1(&t, 323, 3, TH);
                for (uint32_t ty = 0; ty < down; ty++)
                    for (uint32_t tx = 0; tx < across; tx++) {
                        uint32_t rows = H - ty * TH < TH ? H - ty * TH : TH;
                        size_t n = (size_t)TW * TH * SPP;
                        pack(raw, s, W, SPP, 8, false, -1, tx * TW, ty * TH, TW, rows, TW);
                        if (rows < TH) memset(raw + (size_t)TW * SPP * rows, 0x33,
                                              (size_t)TW * SPP * (TH - rows));
                        encode_seg(&segs[ns], raw, n, comps[c]);
                        ns++;
                    }
            } else {
                uint32_t r = rps ? rps : H;
                for (uint32_t y0 = 0; y0 < H; y0 += r) {
                    uint32_t rows = H - y0 < r ? H - y0 : r;
                    size_t n = pack(raw, s, W, SPP, 8, false, -1, 0, y0, W, rows, W);
                    encode_seg(&segs[ns], raw, n, comps[c]);
                    ns++;
                }
            }
            for (int i = 0; i < ns; i++) {
                for (size_t k = 0; k < segs[i].n; k++) segs[i].p[k] = bitrev(segs[i].p[k]);
                tb_seg(&t, segs[i].p, segs[i].n);
            }
            tb_build(&b, &t);
            got = load_px(&b, W, H, NULL, NULL);
            CHECK(got != NULL);
            for (uint32_t i = 0; got && i < W * H; i++)
                ok = ok && px_same(got[i], expect_px(&g, s + (size_t)i * SPP, NULL));
            if (!ok) INFO("fill order 2: compression %u tiled %d differs", comps[c], tiled);
            CHECK(ok);
            free(got);
            for (int i = 0; i < ns; i++) pc_buf_free(&segs[i]);
            pc_buf_free(&b);
        }
    }
    free(raw);
    free(s);
}

static pc_status load_lim(const pc_buf *b, uint64_t max_mem, pc_px32 **px)
{
    pc_codec_limits lim;
    pc_doc *d = NULL;
    pc_image_meta m;
    pc_status st;
    pc_codec_limits_default(&lim);
    lim.max_mem = max_mem;
    st = C->load(b->p, b->n, &lim, &d, &m);
    *px = NULL;
    if (st == PC_OK) {
        *px = doc_layer0(d);
        pc_meta_free(&m);
        pc_doc_destroy(d);
    } else {
        CHECK(d == NULL);
    }
    return st;
}

/* Wide tile rows: tiles up to 64 rows high are decoded one tile at a time
 * (few open decoders, so tight limits pass); taller tiles keep a decoder per
 * tile of the row open, which must fit the working-memory limit. */
static void t_tile_memory(void)
{
    const uint32_t TW = 16, ACROSS = 600, W = TW * ACROSS;
    for (int tall = 0; tall < 2; tall++) {
        const uint32_t TH = tall ? 80u : 16u, H = TH;
        uint8_t *raw = (uint8_t *)malloc((size_t)TW * TH);
        pc_buf seg, b;
        pc_px32 *got = NULL;
        bool ok = true;
        tb t;
        for (uint32_t i = 0; i < TW * TH; i++) raw[i] = (uint8_t)(i * 7u + 3u);
        encode_seg(&seg, raw, (size_t)TW * TH, 8);
        tb_init(&t, false);
        t.tiles = true;
        tb_basic(&t, W, H, 8, 1, 1, 8, 0);
        tb_1(&t, 322, 3, TW);
        tb_1(&t, 323, 3, TH);
        for (uint32_t i = 0; i < ACROSS; i++) tb_seg(&t, seg.p, seg.n);
        tb_build(&b, &t);
        /* image 9600 x H x 4 bytes is 0.6 or 3 MiB; 600 open inflaters ~29 MiB */
        CHECK(load_lim(&b, 8u << 20, &got) == (tall ? PC_ERR_LIMIT : PC_OK));
        free(got);
        CHECK(load_lim(&b, (uint64_t)4 << 30, &got) == PC_OK);
        for (uint32_t y = 0; got && y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint32_t v = raw[y * TW + x % TW];
                ok = ok && px_same(got[(size_t)y * W + x], mkpx(v, v, v, 255));
            }
        CHECK(got && ok);
        free(got);
        pc_buf_free(&b);
        pc_buf_free(&seg);
        free(raw);
    }
}

static pc_status load_status(const pc_buf *b);

/* ---- CCITT -------------------------------------------------------------------------- */
typedef struct bitw { pc_buf *b; uint32_t acc, n; } bitw;

static void bw_bits(bitw *w, const char *bits)
{
    for (const char *p = bits; *p; p++) {
        if (*p == ' ') continue;
        w->acc = (w->acc << 1) | (uint32_t)(*p == '1');
        if (++w->n == 8u) { bb_u8(w->b, w->acc); w->acc = 0; w->n = 0; }
    }
}

static void bw_align(bitw *w)
{
    if (w->n) { bb_u8(w->b, w->acc << (8u - w->n)); w->acc = 0; w->n = 0; }
}

static void t_ccitt(void)
{
    /* rows: W3 B2 W3 / B8 / W2 B6, photometric min-is-white (1 bits black) */
    static const uint8_t want[3][8] = {
        { 255, 255, 255, 0, 0, 255, 255, 255 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 255, 255, 0, 0, 0, 0, 0, 0 },
    };
    static const char *const files[] = {
        "tif_pil_g4.tif", "tif_pil_ccitt_rle.tif", "tif_pil_g3_1d.tif", "tif_pil_g3_2d.tif",
        "tif_im_g4_lsb.tif", "tif_im_fax.tif",
    };
    for (int k = 0; k < 4; k++) {
        pc_buf data, b;
        bitw w;
        tb t;
        pc_px32 *got;
        uint32_t comp = k == 0 ? 2u : k == 1 ? 3u : k == 2 ? 4u : 3u;
        memset(&data, 0, sizeof data);
        w.b = &data; w.acc = 0; w.n = 0;
        if (k == 0) {                      /* Modified Huffman, byte-aligned rows */
            bw_bits(&w, "1000 11 1000"); bw_align(&w);
            bw_bits(&w, "00110101 000101"); bw_align(&w);
            bw_bits(&w, "0111 0010"); bw_align(&w);
        } else if (k == 1) {               /* T.4 1D with EOLs and fill bits */
            bw_bits(&w, "0000 000000000001 1000 11 1000");
            bw_bits(&w, "000000000001 00110101 000101");
            bw_bits(&w, "000000000001 0111 0010 000000000001 000000000001");
        } else if (k == 2) {               /* T.6: H, V0, VL3, P, VR2 */
            bw_bits(&w, "001 1000 11 1");
            bw_bits(&w, "0000010 0001");
            bw_bits(&w, "000011 1");
        } else {                           /* T.4 2D: tag bits pick 1D and 2D rows */
            bw_bits(&w, "000000000001 1 1000 11 1000");
            bw_bits(&w, "000000000001 0 0000010 0001");
            bw_bits(&w, "000000000001 0 000011 1");
        }
        bw_align(&w);
        tb_init(&t, k & 1);
        tb_basic(&t, 8, 3, 1, 1, 0, comp, 0);
        if (k == 3) tb_1(&t, 292, 4, 1);
        tb_seg(&t, data.p, data.n);
        tb_build(&b, &t);
        got = load_px(&b, 8, 3, NULL, NULL);
        CHECK(got != NULL);
        for (int y = 0; got && y < 3; y++)
            for (int x = 0; x < 8; x++)
                CHECK(got[y * 8 + x].r == want[y][x] && got[y * 8 + x].a == 255);
        free(got);
        pc_buf_free(&b);
        /* cut the code stream: the first row survives, the rest is transparent */
        tb_init(&t, k & 1);
        tb_basic(&t, 8, 3, 1, 1, 0, comp, 0);
        if (k == 3) tb_1(&t, 292, 4, 1);
        tb_seg(&t, data.p, k == 1 ? 4u : k == 3 ? 3u : 2u);   /* first row complete */
        tb_build(&b, &t);
        {
            pc_image_meta m;
            got = load_px(&b, 8, 3, &m, NULL);
            CHECK(got && got[0].a == 255 && got[23].a == 0);
            CHECK(got && strstr(m.note, "incomplete") != NULL);
            if (got) pc_meta_free(&m);
        }
        free(got);
        pc_buf_free(&b);
        pc_buf_free(&data);
    }
    {   /* invalid codes stop the stream; CCITT needs 1-bit samples */
        static const uint8_t junk[4] = { 0x00, 0x00, 0x00, 0x00 };
        tb t;
        pc_buf b;
        pc_px32 *got;
        tb_init(&t, false);
        tb_basic(&t, 8, 2, 1, 1, 0, 4, 0);
        tb_seg(&t, junk, sizeof junk);
        tb_build(&b, &t);
        got = load_px(&b, 8, 2, NULL, NULL);
        CHECK(got && got[0].a == 0 && got[15].a == 0);
        free(got);
        pc_buf_free(&b);
        tb_init(&t, false);
        tb_basic(&t, 8, 2, 8, 1, 0, 4, 0);
        tb_seg(&t, junk, sizeof junk);
        tb_build(&b, &t);
        CHECK(load_status(&b) == PC_ERR_UNSUPPORTED);
        pc_buf_free(&b);
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        size_t n;
        uint8_t *p = read_fixture(files[i], &n);
        pc_buf b;
        pc_px32 *got;
        bool ok = true;
        CHECK(p != NULL);
        if (!p) continue;
        b.p = p; b.n = n; b.cap = n;
        got = load_px(&b, 19, 13, NULL, NULL);
        CHECK(got != NULL);
        for (uint32_t y = 0; got && y < 13; y++)
            for (uint32_t x = 0; x < 19; x++) ok = ok && px_same(got[y * 19 + x], pat_bw(x, y));
        CHECK(ok);
        free(got);
        free(p);
    }
}

static pc_status load_status(const pc_buf *b)
{
    pc_doc *d = (pc_doc *)(uintptr_t)1;
    pc_image_meta m;
    pc_status st = codec_load(C, b->p, b->n, &d, &m);
    if (st == PC_OK) { pc_meta_free(&m); pc_doc_destroy(d); }
    else CHECK(d == NULL);
    return st;
}

static void t_bad(void)
{
    uint8_t raw[256];
    memset(raw, 0x11, sizeof raw);
    struct { const char *name; int k; pc_status want; } cases[] = {
        { "jpeg", 0, PC_ERR_UNSUPPORTED }, { "ccitt", 1, PC_ERR_UNSUPPORTED },
        { "ycbcr", 2, PC_ERR_UNSUPPORTED }, { "cielab", 3, PC_ERR_UNSUPPORTED },
        { "bps mismatch", 4, PC_ERR_UNSUPPORTED }, { "bps 3", 5, PC_ERR_UNSUPPORTED },
        { "signed", 6, PC_ERR_UNSUPPORTED }, { "half float", 7, PC_ERR_UNSUPPORTED },
        { "rgb spp 2", 8, PC_ERR_FORMAT }, { "palette no map", 9, PC_ERR_FORMAT },
        { "short map", 10, PC_ERR_FORMAT }, { "planar 3", 11, PC_ERR_FORMAT },
        { "predictor 3", 12, PC_ERR_UNSUPPORTED }, { "pred 2 on 4-bit", 13, PC_ERR_UNSUPPORTED },
        { "too few offsets", 14, PC_ERR_FORMAT }, { "huge", 15, PC_ERR_LIMIT },
        { "tile width 0", 16, PC_ERR_FORMAT }, { "spp 40", 17, PC_ERR_UNSUPPORTED },
        { "no width", 18, PC_ERR_FORMAT }, { "no offsets", 19, PC_ERR_FORMAT },
        { "inkset 2", 20, PC_ERR_UNSUPPORTED }, { "palette 16-bit", 21, PC_ERR_UNSUPPORTED },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int k = cases[i].k;
        uint32_t bpsv[3] = { 8, 8, 16 };
        tb t;
        pc_buf b;
        pc_status st;
        tb_init(&t, false);
        if (k == 18) {
            tb_1(&t, 257, 3, 4); tb_1(&t, 258, 3, 8); tb_1(&t, 262, 3, 1);
        } else if (k == 4) {
            tb_1(&t, 256, 4, 4); tb_1(&t, 257, 4, 4); tb_add(&t, 258, 3, 3, bpsv);
            tb_1(&t, 259, 3, 1); tb_1(&t, 262, 3, 2); tb_1(&t, 277, 3, 3);
        } else {
            uint32_t ph = (k == 2) ? 6u : (k == 3) ? 8u : (k == 9 || k == 10 || k == 21) ? 3u
                        : (k == 4 || k == 8) ? 2u : (k == 20) ? 5u : 1u;
            uint32_t spp = k == 4 ? 3u : k == 8 ? 2u : k == 17 ? 40u : k == 20 ? 4u : 1u;
            uint32_t bps = k == 5 ? 3u : k == 7 ? 16u : (k == 13) ? 4u : k == 21 ? 16u : 8u;
            uint32_t comp = k == 0 ? 7u : k == 1 ? 3u : (k == 12 || k == 13) ? 5u : 1u;
            uint32_t dim = k == 15 ? 65535u : 4u;
            tb_basic(&t, dim, dim, bps, spp, ph, comp, k == 14 ? 1u : 0u);
            if (k == 6) tb_1(&t, 339, 3, 2);
            if (k == 7) tb_1(&t, 339, 3, 3);
            if (k == 10) { uint32_t cm[6] = { 1, 2, 3, 4, 5, 6 }; tb_add(&t, 320, 3, 6, cm); }
            if (k == 11) tb_1(&t, 284, 3, 3);
            if (k == 12) tb_1(&t, 317, 3, 3);
            if (k == 13) tb_1(&t, 317, 3, 2);
            if (k == 16) { t.tiles = true; tb_1(&t, 322, 3, 0); tb_1(&t, 323, 3, 16); }
            if (k == 20) tb_1(&t, 332, 3, 2);
        }
        if (k != 19) tb_seg(&t, raw, 64);
        tb_build(&b, &t);
        st = load_status(&b);
        if (st != cases[i].want) INFO("%s: status %d", cases[i].name, (int)st);
        CHECK(st == cases[i].want);
        pc_buf_free(&b);
    }
    {   /* header and IFD structure */
        tb t;
        pc_buf b;
        tb_init(&t, false);
        tb_basic(&t, 4, 4, 8, 1, 1, 1, 0);
        tb_seg(&t, raw, 16);
        tb_build(&b, &t);
        CHECK(load_status(&b) == PC_OK);
        b.p[2] = 43;
        CHECK(load_status(&b) == PC_ERR_UNSUPPORTED);         /* BigTIFF */
        b.p[2] = 41;
        CHECK(load_status(&b) == PC_ERR_FORMAT);
        b.p[2] = 42;
        bb_set_le32(&b, 4, (uint32_t)b.n + 10u);
        CHECK(load_status(&b) == PC_ERR_FORMAT);              /* IFD past EOF */
        bb_set_le32(&b, 4, 2);
        CHECK(load_status(&b) == PC_ERR_FORMAT);              /* IFD inside the header */
        pc_buf_free(&b);
        tb_build(&b, &t);
        {
            size_t ifd = (size_t)b.p[4] | ((size_t)b.p[5] << 8);
            b.p[ifd] = 0; b.p[ifd + 1] = 0;
            CHECK(load_status(&b) == PC_ERR_FORMAT);          /* zero entries */
            b.p[ifd] = 0x01; b.p[ifd + 1] = 0x10;              /* 4097 entries */
            CHECK(load_status(&b) == PC_ERR_FORMAT);
            b.p[ifd] = 0xFF; b.p[ifd + 1] = 0x00;              /* entries past EOF */
            CHECK(load_status(&b) == PC_ERR_FORMAT);
        }
        pc_buf_free(&b);
        CHECK(C->load(raw, 7, NULL, NULL, NULL) == PC_ERR_ARG);
    }
}

/* Tiny files that declare huge images must not allocate or spin. */
static void t_bombs(void)
{
    uint8_t tiny[32];
    pc_buf code;
    memset(tiny, 0, sizeof tiny);
    memset(&code, 0, sizeof code);
    ref_lzw_encode(&code, tiny, sizeof tiny, 8, 1, false, true);
    for (int k = 0; k < 4; k++) {
        tb t;
        pc_buf b;
        pc_doc *d = NULL;
        pc_image_meta m;
        double t0 = pc_test_now();
        tb_init(&t, false);
        tb_basic(&t, 30000, 30000, 8, 4, 2, 5, k == 0 ? 30000u : 0u);
        tb_1(&t, 338, 3, 2);
        if (k >= 1) {
            t.tiles = true;
            tb_1(&t, 322, 4, k == 3 ? 16u : 30000u);
            tb_1(&t, 323, 4, 30000);
        }
        if (k == 2) tb_1(&t, 274, 3, 6);
        tb_seg(&t, code.p, code.n);
        tb_build(&b, &t);
        if (k == 3) {
            CHECK(load_status(&b) == PC_ERR_FORMAT);          /* needs 1875 tile offsets */
        } else {
            CHECK(codec_load(C, b.p, b.n, &d, &m) == PC_OK);
            CHECK(d && ((k == 2) ? d->w == 30000u : d->h == 30000u));
            if (d) {
                size_t tiles, bytes;
                pc_tile_stats(&tiles, &bytes);
                CHECK(tiles < 2000u);                           /* only the decoded rows */
                CHECK(strstr(m.note, "incomplete") != NULL);
                pc_meta_free(&m);
                pc_doc_destroy(d);
            }
        }
        INFO("30000 x 30000 bomb %d: %.3f s", k, pc_test_now() - t0);
        CHECK(pc_test_now() - t0 < 10.0);
        pc_buf_free(&b);
    }
    pc_buf_free(&code);
}

/* ---- writer ----------------------------------------------------------------------- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Value of a tag in our (little-endian) output, or UINT32_MAX. */
static uint32_t find_tag(const pc_buf *b, uint32_t tag, uint32_t *count)
{
    uint32_t ifd = rd32(b->p + 4), n = rd16(b->p + ifd), prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b->p + ifd + 2 + 12 * i;
        uint32_t t = rd16(e), type = rd16(e + 2);
        CHECK(t > prev);                                      /* sorted */
        prev = t;
        if (t == tag) {
            if (count) *count = rd32(e + 4);
            return type == 3u && rd32(e + 4) == 1u ? rd16(e + 8) : rd32(e + 8);
        }
    }
    return UINT32_MAX;
}

static pc_px32 pat_3c(uint32_t x, uint32_t y)
{
    static const pc_px32 c[3] = { { 1, 2, 3, 255 }, { 200, 100, 50, 255 }, { 0, 255, 0, 255 } };
    return c[(x + y) % 3u];
}

static void t_roundtrip(void)
{
    static const pat_fn pats[] = { pat_rgba, pat_rgb, pat_few, pat_bw, pat_3c, pat_gray };
    static const uint32_t k_depth[7] = { 0, 32, 24, 8, 4, 2, 1 };
    const uint32_t W = 71, H = 300;
    for (size_t pi = 0; pi < sizeof pats / sizeof pats[0]; pi++) {
        pc_doc *d = doc_pat(pats[pi], W, H);
        pc_px32 *flat = doc_flat(d), *white = (pc_px32 *)malloc(W * H * sizeof *white);
        static const uint32_t auto_depth[6] = { 32, 24, 4, 1, 2, 8 };
        for (uint32_t i = 0; i < W * H; i++) white[i] = over_white(flat[i]);
        for (int32_t dep = 0; dep <= 6; dep++)
            for (int32_t comp = 0; comp <= 2; comp++) {
                tiff_params p = { dep, comp, 7, comp & 1 };
                pc_image_meta meta, m;
                pc_buf b;
                pc_px32 *got;
                uint32_t bits = dep ? k_depth[dep] : auto_depth[pi], cnt = 0;
                uint32_t spp = bits == 32u ? 4u : bits == 24u ? 3u : 1u;
                memset(&meta, 0, sizeof meta);
                meta.dpi_x = 200.0; meta.dpi_y = 72.5;
                CHECK(codec_save(C, d, &p, &meta, &b) == PC_OK);
                CHECK(memcmp(b.p, "II*\0", 4) == 0 && C->sniff(b.p, b.n));
                CHECK(find_tag(&b, 277, NULL) == spp);
                CHECK(find_tag(&b, 259, NULL) == (comp == 0 ? 5u : comp == 1 ? 8u : 1u));
                CHECK(find_tag(&b, 262, NULL) == (bits <= 8u ? 3u : 2u));
                CHECK((find_tag(&b, 317, NULL) == 2u) == (bits > 8u && comp != 2));
                CHECK((find_tag(&b, 338, NULL) == 2u) == (bits == 32u));
                CHECK(find_tag(&b, 273, &cnt) != UINT32_MAX && cnt >= 1u);
                if (bits == 32u) CHECK(cnt > 1u);                 /* about 64 KiB strips */
                if (bits <= 8u) {
                    uint32_t mc = 0;
                    CHECK(find_tag(&b, 320, &mc) != UINT32_MAX && mc == 3u << bits);
                }
                got = load_px(&b, W, H, &m, NULL);
                CHECK(got != NULL);
                if (got) {
                    const pc_px32 *ref = bits == 32u ? flat : white;
                    uint32_t ncol = pi == 2 ? 13u : pi == 3 ? 2u : pi == 4 ? 3u : 256u;
                    if (bits >= 24u || (pi >= 2 && ncol <= (1u << bits))) {
                        uint32_t md = px_maxdiff(got, ref, W * H);
                        if (md) INFO("pattern %u depth %d comp %d: max diff %u", (unsigned)pi, dep,
                                     comp, md);
                        CHECK(md == 0u);
                    } else {
                        double lim = bits == 8u ? 20.0 : bits == 4u ? 14.0 : bits == 2u ? 9.0 : 6.0;
                        CHECK(px_psnr(got, ref, W * H) >= lim);
                    }
                    CHECK(fabs(m.dpi_x - 200.0) < 1e-6 && fabs(m.dpi_y - 72.5) < 1e-6);
                    CHECK(m.src_bits == (bits >= 24u ? 8u : bits));
                    pc_meta_free(&m);
                }
                free(got);
                pc_buf_free(&b);
            }
        free(flat);
        free(white);
        pc_doc_destroy(d);
    }
}

static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, 0);
}

static void t_writer_details(void)
{
    pc_doc *d = doc_pat(pat_rgba, 200, 170);
    pc_par par;
    pc_buf b1, b2;
    tiff_params bad[4] = { { 7, 0, 7, 0 }, { 0, 3, 7, 0 }, { 0, 0, 9, 0 }, { 0, 0, 7, 2 } };
    par.run = rev_run; par.self = NULL; par.threads = 2;
    for (int32_t comp = 0; comp <= 2; comp++) {
        tiff_params p = { 0, comp, 7, 0 };
        CHECK(codec_save(C, d, &p, NULL, &b1) == PC_OK);
        memset(&b2, 0, sizeof b2);
        CHECK(C->save(d, NULL, &p, &par, &b2) == PC_OK);
        CHECK(b1.n == b2.n && memcmp(b1.p, b2.p, b1.n) == 0);
        if (comp == 2) CHECK(b1.n > 200u * 170u * 4u);
        else CHECK(b1.n < 200u * 170u * 4u);
        CHECK(find_tag(&b1, 282, NULL) != UINT32_MAX);
        pc_buf_free(&b1);
        pc_buf_free(&b2);
    }
    memset(&b1, 0, sizeof b1);
    for (int i = 0; i < 4; i++) CHECK(C->save(d, NULL, &bad[i], NULL, &b1) == PC_ERR_ARG);
    {   /* appending keeps offsets relative to the start of this file */
        pc_buf b = { 0 };
        pc_px32 *got;
        pc_doc *o = NULL;
        pc_image_meta m;
        CHECK(pc_buf_append(&b, "prefix!", 7) == PC_OK);
        CHECK(C->save(d, NULL, NULL, NULL, &b) == PC_OK);
        CHECK(codec_load(C, b.p + 7, b.n - 7, &o, &m) == PC_OK);
        if (o) {
            got = doc_layer0(o);
            {
                pc_px32 *flat = doc_flat(d);
                CHECK(memcmp(got, flat, 200 * 170 * sizeof *got) == 0);
                free(flat);
            }
            free(got);
            pc_meta_free(&m);
            pc_doc_destroy(o);
        }
        pc_buf_free(&b);
    }
    pc_buf_free(&b1);
    pc_doc_destroy(d);
}

/* ---- fuzzing ------------------------------------------------------------------------ */
static void make_seeds(seedset *ss)
{
    pc_doc *d = doc_pat(pat_rgba, 21, 13), *f = doc_pat(pat_few, 21, 13);
    for (int32_t dep = 0; dep <= 6; dep += 1)
        for (int32_t comp = 0; comp <= 2; comp++) {
            tiff_params p = { dep, comp, 7, 0 };
            pc_buf b;
            if ((dep + comp) % 2 == 0 && codec_save(C, dep >= 3 ? f : d, &p, NULL, &b) == PC_OK)
                seeds_add_buf(ss, &b);
        }
    {   /* tiled, planar, big-endian, 16-bit, PackBits and old LZW variants */
        uint32_t *s = rand_samples(20 * 12 * 4, 16);
        uint8_t raw[20 * 12 * 4 * 2];
        static const uint32_t comps[] = { 32773, 50, 5, 8 };
        for (int k = 0; k < 4; k++) {
            tb t;
            pc_buf seg[8], b;
            int ns = 0;
            uint32_t bps = k == 1 ? 8u : 16u;
            tb_init(&t, k & 1);
            if (k == 3) {
                t.tiles = true;
                tb_basic(&t, 20, 12, bps, 4, 2, comps[k], 0);
                tb_1(&t, 322, 3, 16); tb_1(&t, 323, 3, 16); tb_1(&t, 317, 3, 2);
                for (uint32_t tx = 0; tx < 2; tx++) {
                    size_t n = pack(raw, s, 20, 4, bps, false, -1, tx * 16u, 0, tx ? 4u : 16u, 12,
                                    16);
                    for (uint32_t r = 0; r < 12; r++)
                        difference(raw + r * 128u, 128u, 4, 16, false);
                    encode_seg(&seg[ns], raw, n, comps[k]);
                    tb_seg(&t, seg[ns].p, seg[ns].n);
                    ns++;
                }
            } else {
                tb_basic(&t, 20, 12, bps, 4, k == 2 ? 5u : 2u, comps[k], 6);
                tb_1(&t, 284, 3, 2);
                tb_1(&t, 274, 3, (uint32_t)(k * 3 + 1));
                for (uint32_t pl = 0; pl < 4; pl++)
                    for (uint32_t st = 0; st < 2; st++) {
                        size_t n = pack(raw, s, 20, 4, bps, k & 1, (int)pl, 0, st * 6u, 20, 6, 20);
                        encode_seg(&seg[ns], raw, n, comps[k]);
                        tb_seg(&t, seg[ns].p, seg[ns].n);
                        ns++;
                    }
            }
            tb_build(&b, &t);
            seeds_add_buf(ss, &b);
            for (int i = 0; i < ns; i++) pc_buf_free(&seg[i]);
        }
        free(s);
    }
    {   /* the third-party CCITT fixtures */
        static const char *const files[] = { "tif_pil_g4.tif", "tif_pil_g3_2d.tif",
                                             "tif_pil_ccitt_rle.tif", "tif_im_fax.tif" };
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            size_t n;
            uint8_t *p = read_fixture(files[i], &n);
            if (p) seeds_add(ss, p, n);
            free(p);
        }
    }
    pc_doc_destroy(d);
    pc_doc_destroy(f);
}

#ifndef PC_LIBFUZZER
static void t_fuzz(void)
{
    seedset s;
    memset(&s, 0, sizeof s);
    make_seeds(&s);
    CHECK(s.count >= 12u);
    fuzz_seeds(C, &s, g_quick ? 4000u : 60000u);
    seeds_free(&s);
}

static void tests(void)
{
    RUN(t_photometric);
    RUN(t_float);
    RUN(t_compression);
    RUN(t_tiles);
    RUN(t_orientation_and_meta);
    RUN(t_missing_data);
    RUN(t_fill_order);
    RUN(t_tile_memory);
    RUN(t_ccitt);
    RUN(t_bad);
    RUN(t_bombs);
    RUN(t_roundtrip);
    RUN(t_writer_details);
    RUN(t_fuzz);
}

int main(int argc, char **argv)
{
    return own_main(argc, argv, C, make_seeds, tests);
}
#else
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_one(C, data, size);
    if (g_fails) abort();               /* a failed CHECK is a finding too */
    return 0;
}
#endif
