/* test_own_gif.c - GIF reader and writer (src/codec/fmt_gif.c): hand-built
 * streams (reference LZW encoder in test_own_common.h) for palettes,
 * interlacing, transparency, logical screens, deferred clear codes,
 * corruption and animations; round trips for every save option; mutation
 * fuzzing. Also a standalone fuzz driver. */
#include "test_own_common.h"

extern const pc_codec pc_codec_gif;
#define C (&pc_codec_gif)

typedef struct gif_params { int32_t dither, threshold, palette; } gif_params;

/* ---- hand-built files ------------------------------------------------------- */
typedef struct gframe {
    uint32_t       left, top, w, h;
    const pc_px32 *lct;
    uint32_t       lct_bits;      /* 0 = no local table */
    bool           interlace;
    int32_t        tidx;          /* transparent index, -1 = GCE without it, -2 = no GCE */
    const uint8_t *idx;           /* w * h indices, display order */
    uint32_t       mcs;
    bool           deferred, no_initial_clear;
    size_t         cut;           /* keep only this many code bytes (0 = all) */
} gframe;

static void put_table(pc_buf *b, const pc_px32 *t, uint32_t bits)
{
    for (uint32_t i = 0; i < (1u << bits); i++) {
        bb_u8(b, t[i].r);
        bb_u8(b, t[i].g);
        bb_u8(b, t[i].b);
    }
}

static void put_frame(pc_buf *b, const gframe *f)
{
    pc_buf codes;
    uint8_t *order = (uint8_t *)malloc((size_t)f->w * f->h + 1u);
    size_t n = 0, pos = 0;
    if (f->tidx > -2) {
        bb_u8(b, 0x21); bb_u8(b, 0xF9); bb_u8(b, 4);
        bb_u8(b, f->tidx >= 0 ? 1u : 0u); bb_le16(b, 0);
        bb_u8(b, f->tidx >= 0 ? (uint32_t)f->tidx : 0u);
        bb_u8(b, 0);
    }
    bb_u8(b, 0x2C);
    bb_le16(b, f->left); bb_le16(b, f->top); bb_le16(b, f->w); bb_le16(b, f->h);
    bb_u8(b, (f->lct_bits ? 0x80u | (f->lct_bits - 1u) : 0u) | (f->interlace ? 0x40u : 0u));
    if (f->lct_bits) put_table(b, f->lct, f->lct_bits);
    bb_u8(b, f->mcs);
    if (f->interlace) {
        static const uint32_t st[4] = { 0, 4, 2, 1 }, sp[4] = { 8, 8, 4, 2 };
        for (int p = 0; p < 4; p++)
            for (uint32_t y = st[p]; y < f->h; y += sp[p]) {
                memcpy(order + n, f->idx + (size_t)y * f->w, f->w);
                n += f->w;
            }
    } else {
        memcpy(order, f->idx, (size_t)f->w * f->h);
        n = (size_t)f->w * f->h;
    }
    memset(&codes, 0, sizeof codes);
    ref_lzw_encode(&codes, order, n, f->mcs, 0, f->deferred, !f->no_initial_clear);
    if (f->cut && f->cut < codes.n) codes.n = f->cut;
    while (pos < codes.n) {                     /* random sub-block sizes */
        size_t len = 1u + rndu(255);
        if (len > codes.n - pos) len = codes.n - pos;
        bb_u8(b, (uint32_t)len);
        bb_bytes(b, codes.p + pos, len);
        pos += len;
    }
    bb_u8(b, 0);
    pc_buf_free(&codes);
    free(order);
}

static void put_header(pc_buf *b, uint32_t sw, uint32_t sh, const pc_px32 *gct, uint32_t gbits)
{
    memset(b, 0, sizeof *b);
    bb_bytes(b, "GIF89a", 6);
    bb_le16(b, sw); bb_le16(b, sh);
    bb_u8(b, gbits ? 0x80u | 0x70u | (gbits - 1u) : 0x70u);
    bb_u8(b, 0); bb_u8(b, 0);
    if (gbits) put_table(b, gct, gbits);
}

static pc_px32 k_pal[256], k_pal2[256];

static void init_pals(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        k_pal[i] = mkpx((i * 37u + 5u) & 255u, (i * 101u) & 255u, (i * 13u + 77u) & 255u, 255);
        k_pal2[i] = mkpx(255u - i, i, (i * 3u) & 255u, 255);
    }
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

static uint8_t *rand_idx(uint32_t n, uint32_t range)
{
    uint8_t *p = (uint8_t *)malloc(n);
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rndu(range);
    return p;
}

static void t_basic(void)
{
    static const uint32_t gbits[] = { 1, 2, 4, 8 };
    for (int k = 0; k < 4; k++)
        for (int il = 0; il < 2; il++)
            for (uint32_t extra = 0; extra < 2; extra++) {
                const uint32_t W = 13 + (uint32_t)k, H = 37;
                uint8_t *idx = rand_idx(W * H, 1u << gbits[k]);
                gframe f;
                pc_buf b;
                pc_px32 *got;
                pc_image_meta m;
                bool ok = true;
                memset(&f, 0, sizeof f);
                f.w = W; f.h = H; f.idx = idx; f.interlace = il != 0; f.tidx = -2;
                f.mcs = (gbits[k] < 2u ? 2u : gbits[k]) + extra * (gbits[k] < 8u ? 1u : 0u);
                f.no_initial_clear = extra != 0;
                put_header(&b, W, H, k_pal, gbits[k]);
                put_frame(&b, &f);
                bb_u8(&b, 0x3B);
                CHECK(C->sniff(b.p, b.n));
                got = load_px(&b, W, H, &m, NULL);
                CHECK(got != NULL);
                for (uint32_t i = 0; got && i < W * H; i++)
                    ok = ok && px_same(got[i], k_pal[idx[i]]);
                CHECK(ok);
                CHECK(got && !m.had_alpha && m.note[0] == 0 && m.src_bits == 8u);
                pc_meta_free(&m);
                free(got);
                free(idx);
                pc_buf_free(&b);
            }
}

static void t_palettes_and_transparency(void)
{
    const uint32_t W = 20, H = 9;
    uint8_t *idx = rand_idx(W * H, 16);
    for (int mode = 0; mode < 5; mode++) {
        gframe f;
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        bool ok = true;
        memset(&f, 0, sizeof f);
        f.w = W; f.h = H; f.idx = idx; f.mcs = 4; f.tidx = -2;
        if (mode == 1 || mode == 2) { f.lct = k_pal2; f.lct_bits = 4; }
        if (mode == 3) f.tidx = 5;
        if (mode == 4) f.tidx = -1;
        put_header(&b, W, H, k_pal, mode == 2 ? 0u : (mode == 0 ? 3u : 4u));
         /* mode 0: 8 entries */
        put_frame(&b, &f);
        bb_u8(&b, 0x3B);
        got = load_px(&b, W, H, &m, NULL);
        CHECK(got != NULL);
        for (uint32_t i = 0; got && i < W * H; i++) {
            pc_px32 want;
            if (mode == 1 || mode == 2) want = k_pal2[idx[i]];
            else if (mode == 0) want = idx[i] < 8u ? k_pal[idx[i]] : mkpx(0, 0, 0, 255);
            else if (mode == 3 && idx[i] == 5u) want = mkpx(0, 0, 0, 0);
            else want = k_pal[idx[i]];
            ok = ok && px_same(got[i], want);
        }
        CHECK(ok);
        CHECK(got && m.had_alpha == (mode == 3));
        pc_meta_free(&m);
        free(got);
        pc_buf_free(&b);
    }
    {   /* no color table at all: opaque black */
        gframe f;
        pc_buf b;
        pc_px32 *got;
        memset(&f, 0, sizeof f);
        f.w = W; f.h = H; f.idx = idx; f.mcs = 4; f.tidx = -2;
        put_header(&b, W, H, NULL, 0);
        put_frame(&b, &f);
        got = load_px(&b, W, H, NULL, NULL);                /* no trailer: fine */
        CHECK(got && px_same(got[0], mkpx(0, 0, 0, 255)) &&
              px_same(got[W * H - 1], mkpx(0, 0, 0, 255)));
        free(got);
        pc_buf_free(&b);
    }
    free(idx);
}

static void t_screen(void)
{
    /* frame inside a larger screen, frame sticking out, screen of size 0 */
    struct { uint32_t sw, sh, l, t, w, h, cw, ch; } cases[] = {
        { 40, 30, 7, 5, 10, 8, 40, 30 },
        { 10, 10, 5, 6, 10, 10, 15, 16 },
        { 0, 0, 3, 2, 6, 5, 9, 7 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint8_t *idx = rand_idx(cases[c].w * cases[c].h, 4);
        gframe f;
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        bool ok = true;
        memset(&f, 0, sizeof f);
        f.left = cases[c].l; f.top = cases[c].t; f.w = cases[c].w; f.h = cases[c].h;
        f.idx = idx; f.mcs = 2; f.tidx = -2;
        put_header(&b, cases[c].sw, cases[c].sh, k_pal, 2);
        put_frame(&b, &f);
        bb_u8(&b, 0x3B);
        got = load_px(&b, cases[c].cw, cases[c].ch, &m, NULL);
        CHECK(got != NULL);
        for (uint32_t y = 0; got && y < cases[c].ch; y++)
            for (uint32_t x = 0; x < cases[c].cw; x++) {
                bool in = x >= f.left && y >= f.top && x < f.left + f.w && y < f.top + f.h;
                pc_px32 want = in ? k_pal[idx[(y - f.top) * f.w + (x - f.left)]] : mkpx(0, 0, 0, 0);
                ok = ok && px_same(got[y * cases[c].cw + x], want);
            }
        CHECK(ok);
        CHECK(got && m.had_alpha);
        pc_meta_free(&m);
        free(got);
        free(idx);
        pc_buf_free(&b);
    }
}

/* Large random frames: table resets, deferred clear (a full table without a
 * clear code), and a stream that starts without a clear code. */
static void t_lzw_table(void)
{
    const uint32_t W = 211, H = 97;
    uint8_t *idx = rand_idx(W * H, 256);
    for (int mode = 0; mode < 3; mode++) {
        gframe f;
        pc_buf b;
        pc_px32 *got;
        bool ok = true;
        memset(&f, 0, sizeof f);
        f.w = W; f.h = H; f.idx = idx; f.mcs = 8; f.tidx = -2;
        f.deferred = mode == 1;
        f.no_initial_clear = mode == 2;
        put_header(&b, W, H, k_pal, 8);
        put_frame(&b, &f);
        bb_u8(&b, 0x3B);
        got = load_px(&b, W, H, NULL, NULL);
        CHECK(got != NULL);
        for (uint32_t i = 0; got && i < W * H; i++) ok = ok && px_same(got[i], k_pal[idx[i]]);
        CHECK(ok);
        free(got);
        pc_buf_free(&b);
    }
    {   /* long runs: deep string chains */
        uint8_t *z = (uint8_t *)calloc((size_t)W * H, 1);
        gframe f;
        pc_buf b;
        pc_px32 *got;
        memset(&f, 0, sizeof f);
        f.w = W; f.h = H; f.idx = z; f.mcs = 2; f.tidx = -2; f.deferred = true;
        put_header(&b, W, H, k_pal, 2);
        put_frame(&b, &f);
        got = load_px(&b, W, H, NULL, NULL);
        CHECK(got && px_same(got[0], k_pal[0]) && px_same(got[W * H - 1], k_pal[0]));
        free(got);
        free(z);
        pc_buf_free(&b);
    }
    free(idx);
}

static void t_corrupt(void)
{
    const uint32_t W = 30, H = 20;
    uint8_t *idx = rand_idx(W * H, 16);
    gframe f;
    pc_buf b;
    pc_px32 *got;
    pc_image_meta m;
    pc_status st;
    memset(&f, 0, sizeof f);
    f.w = W; f.h = H; f.idx = idx; f.mcs = 4; f.tidx = -2; f.cut = 100;
    put_header(&b, W, H, k_pal, 4);
    put_frame(&b, &f);
    got = load_px(&b, W, H, &m, &st);                 /* truncated data: partial image */
    CHECK(st == PC_OK && got != NULL);
    if (got) {
        CHECK(px_same(got[0], k_pal[idx[0]]));
        CHECK(px_same(got[W * H - 1], mkpx(0, 0, 0, 0)));
        CHECK(m.had_alpha && strstr(m.note, "incomplete") != NULL);
        pc_meta_free(&m);
    }
    free(got);
    pc_buf_free(&b);
    {   /* a code beyond the next free entry ends the stream */
        static const uint8_t data[] = { 0x2C, 0, 0, 0, 0, 4, 0, 1, 0, 0, 2,
                                        3, 0xCC, 0x07, 0x00, 0, 0x3B };
                                         /* clear, 1, then 7 > next */
        put_header(&b, 4, 1, k_pal, 2);
        bb_bytes(&b, data, sizeof data);
        got = load_px(&b, 4, 1, &m, &st);
        CHECK(st == PC_OK && got && px_same(got[0], k_pal[1]));
        if (got) pc_meta_free(&m);
        free(got);
        pc_buf_free(&b);
    }
    {   /* structural errors */
        struct { const char *name; int what; pc_status want; } cases[] = {
            { "no image", 0, PC_ERR_FORMAT }, { "bad version", 1, PC_ERR_FORMAT },
            { "min code size 0", 2, PC_ERR_FORMAT }, { "min code size 12", 3, PC_ERR_FORMAT },
            { "zero width", 4, PC_ERR_FORMAT }, { "local table cut", 5, PC_ERR_FORMAT },
            { "global table cut", 6, PC_ERR_FORMAT }, { "junk block", 7, PC_ERR_FORMAT },
            { "huge screen", 8, PC_ERR_LIMIT },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            pc_doc *d = (pc_doc *)(uintptr_t)1;
            int w = cases[i].what;
            put_header(&b, w == 8 ? 65535u : 8u, w == 8 ? 65535u : 8u, k_pal, 2);
            if (w == 1) b.p[4] = '8';
            if (w == 7) bb_u8(&b, 0x99);
            if (w >= 2 && w != 7) {
                bb_u8(&b, 0x2C); bb_le32(&b, 0);
                bb_le16(&b, w == 4 ? 0u : 4u); bb_le16(&b, 2);
                bb_u8(&b, w == 5 ? 0x87u : 0u);
                if (w == 5) bb_zero(&b, 10);
                else bb_u8(&b, w == 2 ? 0u : (w == 3 ? 12u : 2u));
                if (w != 5) { bb_u8(&b, 2); bb_u8(&b, 0x4C); bb_u8(&b, 0x01); bb_u8(&b, 0); }
            }
            bb_u8(&b, 0x3B);
            if (w == 6) b.n = 13 + 5;
            st = codec_load(C, b.p, b.n, &d, &m);
            if (st != cases[i].want) INFO("%s: status %d", cases[i].name, (int)st);
            CHECK(st == cases[i].want && d == NULL);
            pc_buf_free(&b);
        }
    }
    free(idx);
}

static void t_animation_and_extensions(void)
{
    const uint32_t W = 9, H = 7;
    uint8_t *i1 = rand_idx(W * H, 4), *i2 = rand_idx(W * H, 4);
    gframe f;
    pc_buf b;
    pc_px32 *got;
    pc_image_meta m;
    bool ok = true;
    put_header(&b, W, H, k_pal, 2);
    /* application, comment and plain-text extensions with several sub-blocks */
    bb_u8(&b, 0x21); bb_u8(&b, 0xFF); bb_u8(&b, 11); bb_bytes(&b, "NETSCAPE2.0", 11);
    bb_u8(&b, 3); bb_u8(&b, 1); bb_le16(&b, 0); bb_u8(&b, 0);
    bb_u8(&b, 0x21); bb_u8(&b, 0xFE); bb_u8(&b, 5); bb_bytes(&b, "hello", 5);
    bb_u8(&b, 255); bb_zero(&b, 255); bb_u8(&b, 0);
    bb_u8(&b, 0x21); bb_u8(&b, 0x01); bb_u8(&b, 12); bb_zero(&b, 12); bb_u8(&b, 0);
    memset(&f, 0, sizeof f);
    f.w = W; f.h = H; f.idx = i1; f.mcs = 2; f.tidx = -2;
    put_frame(&b, &f);
    f.idx = i2; f.tidx = 1; f.interlace = true; f.lct = k_pal2; f.lct_bits = 2;
    put_frame(&b, &f);                          /* its GCE must not affect frame 1 */
    f.tidx = -2; f.lct_bits = 0;
    put_frame(&b, &f);
    bb_u8(&b, 0x3B);
    got = load_px(&b, W, H, &m, NULL);
    CHECK(got != NULL);
    for (uint32_t i = 0; got && i < W * H; i++) ok = ok && px_same(got[i], k_pal[i1[i]]);
    CHECK(ok);
    CHECK(got && strstr(m.note, "first of 3 frames") != NULL && !m.had_alpha);
    if (got) pc_meta_free(&m);
    free(got);
    free(i1);
    free(i2);
    pc_buf_free(&b);
}

/* ---- writer ------------------------------------------------------------------- */
static uint32_t gct_bits(const pc_buf *b) { return (b->p[10] & 0x80u) ? (b->p[10] & 7u) + 1u : 0u; }
static bool has_gce(const pc_buf *b) { return b->p[13 + 3 * (1u << gct_bits(b))] == 0x21; }

static void t_roundtrip_exact(void)
{
    static const pat_fn pats[] = { pat_few, pat_gray, pat_bw };
    static const uint32_t bits[] = { 4, 8, 1 };
    for (int pi = 0; pi < 3; pi++)
        for (int32_t dl = 0; dl <= 8; dl += 4)
            for (int32_t pal = 0; pal < 2; pal++) {
                pc_doc *d = doc_pat(pats[pi], 77, 41);
                pc_px32 *flat = doc_flat(d), *got;
                gif_params p = { dl, 128, pal };
                pc_buf b;
                CHECK(codec_save(C, d, &p, NULL, &b) == PC_OK);
                CHECK(memcmp(b.p, "GIF89a", 6) == 0 && b.p[b.n - 1] == 0x3B);
                CHECK(gct_bits(&b) == bits[pi] && !has_gce(&b));
                got = load_px(&b, 77, 41, NULL, NULL);
                CHECK(got && memcmp(got, flat, 77 * 41 * sizeof *got) == 0);
                free(got);
                free(flat);
                pc_buf_free(&b);
                pc_doc_destroy(d);
            }
    {   /* 256 random colors over a large area: exact, many table resets */
        const uint32_t W = 301, H = 203;
        pc_px32 *px = (pc_px32 *)malloc(W * H * sizeof *px), *got;
        pc_doc *d;
        pc_buf b;
        for (uint32_t i = 0; i < W * H; i++) px[i] = k_pal[i < 256 ? i : rndu(256)];
        for (uint32_t i = 0; i < 256; i++) px[i] = mkpx(i, 255u - i, (i * 7u) & 255u, 255);
        for (uint32_t i = 256; i < W * H; i++) px[i] = px[rndu(256)];
        d = doc_from(px, W, H);
        CHECK(codec_save(C, d, NULL, NULL, &b) == PC_OK);
        CHECK(gct_bits(&b) == 8u);
        got = load_px(&b, W, H, NULL, NULL);
        CHECK(got && memcmp(got, px, W * H * sizeof *got) == 0);
        free(got);
        free(px);
        pc_buf_free(&b);
        pc_doc_destroy(d);
    }
}

static pc_px32 pat_one(uint32_t x, uint32_t y) { (void)x; (void)y; return mkpx(9, 8, 7, 255); }
static pc_px32 pat_none(uint32_t x, uint32_t y) { (void)x; (void)y; return mkpx(0, 0, 0, 0); }

static void t_roundtrip_alpha(void)
{
    const uint32_t W = 64, H = 40;
    static const int32_t thr[] = { 0, 1, 128, 255 };
    pc_doc *d = doc_pat(pat_rgba, W, H);
    pc_px32 *flat = doc_flat(d);
    for (size_t t = 0; t < 4; t++) {
        gif_params p = { 7, thr[t], 0 };
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        CHECK(codec_save(C, d, &p, NULL, &b) == PC_OK);
        CHECK(has_gce(&b) == (thr[t] > 0));
        got = load_px(&b, W, H, &m, NULL);
        CHECK(got != NULL);
        if (got) {
            bool mask_ok = true;
            uint32_t opaque = 0;
            double se = 0.0;
            for (uint32_t i = 0; i < W * H; i++) {
                bool tr = thr[t] > 0 && (int32_t)flat[i].a < thr[t];
                mask_ok = mask_ok && (tr ? px_same(got[i], mkpx(0, 0, 0, 0)) : got[i].a == 255);
                if (!tr) {
                    pc_px32 w = over_white(flat[i]);
                    se += (double)(w.r - got[i].r) * (w.r - got[i].r) +
                          (double)(w.g - got[i].g) * (w.g - got[i].g) +
                          (double)(w.b - got[i].b) * (w.b - got[i].b);
                    opaque++;
                }
            }
            CHECK(mask_ok);
            if (opaque) CHECK(se / (opaque * 3.0) < 100.0);     /* PSNR above about 28 dB */
            CHECK(m.had_alpha == (thr[t] > 0));
            pc_meta_free(&m);
        }
        free(got);
        pc_buf_free(&b);
    }
    free(flat);
    pc_doc_destroy(d);
    {   /* one color; fully transparent; transparency with few colors is exact */
        pc_doc *o = doc_pat(pat_one, 5, 3), *z = doc_pat(pat_none, 6, 4), *f;
        pc_px32 *px = pat_image(pat_few, 30, 20), *got;
        pc_buf b;
        CHECK(codec_save(C, o, NULL, NULL, &b) == PC_OK);
        CHECK(gct_bits(&b) == 1u);
        got = load_px(&b, 5, 3, NULL, NULL);
        CHECK(got && px_same(got[14], mkpx(9, 8, 7, 255)));
        free(got);
        pc_buf_free(&b);
        CHECK(codec_save(C, z, NULL, NULL, &b) == PC_OK);
        got = load_px(&b, 6, 4, NULL, NULL);
        CHECK(got && px_same(got[0], mkpx(0, 0, 0, 0)) && px_same(got[23], mkpx(0, 0, 0, 0)));
        free(got);
        pc_buf_free(&b);
        for (uint32_t i = 0; i < 600; i += 3) px[i] = mkpx(0, 0, 0, 0);
        f = doc_from(px, 30, 20);
        CHECK(codec_save(C, f, NULL, NULL, &b) == PC_OK);
        got = load_px(&b, 30, 20, NULL, NULL);
        CHECK(got && memcmp(got, px, 600 * sizeof *got) == 0);
        free(got);
        pc_buf_free(&b);
        free(px);
        pc_doc_destroy(o);
        pc_doc_destroy(z);
        pc_doc_destroy(f);
    }
}

static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, 0);
}

static void t_writer_details(void)
{
    pc_doc *d = doc_pat(pat_rgba, 140, 90);
    pc_buf b1, b2;
    pc_par par;
    gif_params bad[3] = { { 9, 128, 0 }, { 7, 256, 0 }, { 7, 128, 2 } };
    par.run = rev_run; par.self = NULL; par.threads = 3;
    for (int32_t dl = 0; dl <= 8; dl++) {
        gif_params p = { dl, 100, dl & 1 };
        CHECK(codec_save(C, d, &p, NULL, &b1) == PC_OK);
        memset(&b2, 0, sizeof b2);
        CHECK(C->save(d, NULL, &p, &par, &b2) == PC_OK);
        CHECK(b1.n == b2.n && memcmp(b1.p, b2.p, b1.n) == 0);
        pc_buf_free(&b1);
        pc_buf_free(&b2);
    }
    memset(&b1, 0, sizeof b1);
    for (int i = 0; i < 3; i++) CHECK(C->save(d, NULL, &bad[i], NULL, &b1) == PC_ERR_ARG);
    CHECK(b1.n == 0u);
    pc_buf_free(&b1);
    pc_doc_destroy(d);
}

/* ---- fuzzing ------------------------------------------------------------------- */
static void make_seeds(seedset *ss)
{
    const uint32_t W = 21, H = 13;
    uint8_t *idx = rand_idx(W * H, 16);
    pc_doc *d = doc_pat(pat_rgba, 25, 14), *f = doc_pat(pat_few, 25, 14);
    gframe fr;
    pc_buf b;
    gif_params p = { 7, 128, 0 };
    if (codec_save(C, d, &p, NULL, &b) == PC_OK) seeds_add_buf(ss, &b);
    p.palette = 1; p.threshold = 0;
    if (codec_save(C, f, &p, NULL, &b) == PC_OK) seeds_add_buf(ss, &b);
    for (int k = 0; k < 4; k++) {
        memset(&fr, 0, sizeof fr);
        fr.w = W; fr.h = H; fr.idx = idx; fr.mcs = 4; fr.tidx = k == 1 ? 3 : -2;
        fr.interlace = k >= 2; fr.left = k == 3 ? 4u : 0u; fr.top = k == 3 ? 2u : 0u;
        if (k == 2) { fr.lct = k_pal2; fr.lct_bits = 4; }
        put_header(&b, k == 3 ? W + 8u : W, k == 3 ? H + 4u : H, k_pal, k == 2 ? 0u : 4u);
        put_frame(&b, &fr);
        if (k == 3) put_frame(&b, &fr);
        bb_u8(&b, 0x3B);
        seeds_add_buf(ss, &b);
    }
    free(idx);
    pc_doc_destroy(d);
    pc_doc_destroy(f);
}

#ifndef PC_LIBFUZZER
static void t_fuzz(void)
{
    seedset s;
    memset(&s, 0, sizeof s);
    make_seeds(&s);
    CHECK(s.count >= 6u);
    fuzz_seeds(C, &s, g_quick ? 4000u : 60000u);
    seeds_free(&s);
}

static void tests(void)
{
    RUN(t_basic);
    RUN(t_palettes_and_transparency);
    RUN(t_screen);
    RUN(t_lzw_table);
    RUN(t_corrupt);
    RUN(t_animation_and_extensions);
    RUN(t_roundtrip_exact);
    RUN(t_roundtrip_alpha);
    RUN(t_writer_details);
    RUN(t_fuzz);
}

static void seeds_main(seedset *s)
{
    init_pals();
    make_seeds(s);
}

int main(int argc, char **argv)
{
    init_pals();
    return own_main(argc, argv, C, seeds_main, tests);
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
