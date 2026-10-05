/* test_own_tga.c - TGA reader and writer (src/codec/fmt_tga.c): every image
 * type, pixel and map depth, origin and alpha rule from hand-built files,
 * RLE packets that cross scanlines, round trips for every save option,
 * corrupt input, mutation fuzzing. Also a standalone fuzz driver. */
#include "test_own_common.h"

extern const pc_codec pc_codec_tga;
#define C (&pc_codec_tga)

typedef struct tga_params { int32_t depth, rle; } tga_params;

/* ---- hand-built files ------------------------------------------------------- */
typedef struct tspec {
    uint32_t       type, cmap_type, cmap_first, cmap_len, cmap_bits;
    uint32_t       w, h, bpp, desc, id_len;
    const uint8_t *cmap;          /* cmap_len entries of (cmap_bits + 7) / 8 bytes */
    const uint8_t *px;            /* w * h stored pixel values, file order */
    int            ext_attr;      /* -1: no TGA 2.0 footer */
    bool           rle;           /* encode px with packets crossing rows */
} tspec;

static void put_packets(pc_buf *b, const uint8_t *px, uint32_t count, uint32_t bytes)
{
    uint32_t i = 0;
    while (i < count) {
        uint32_t run = 1;
        while (i + run < count && run < 128u && memcmp(px + (size_t)(i + run) * bytes,
                                                       px + (size_t)i * bytes, bytes) == 0)
            run++;
        if (run > 1u) {
            bb_u8(b, 0x80u | (run - 1u));
            bb_bytes(b, px + (size_t)i * bytes, bytes);
        } else {
            uint32_t k = 1u + rndu(7);              /* raw packets of random length */
            if (i + k > count) k = count - i;
            bb_u8(b, k - 1u);
            bb_bytes(b, px + (size_t)i * bytes, (size_t)k * bytes);
            run = k;
        }
        i += run;
    }
}

static void build(pc_buf *b, const tspec *s)
{
    uint32_t bytes = (s->bpp + 7u) / 8u;
    memset(b, 0, sizeof *b);
    bb_u8(b, s->id_len); bb_u8(b, s->cmap_type); bb_u8(b, s->type);
    bb_le16(b, s->cmap_first); bb_le16(b, s->cmap_len); bb_u8(b, s->cmap_bits);
    bb_le16(b, 0); bb_le16(b, 0); bb_le16(b, s->w); bb_le16(b, s->h);
    bb_u8(b, s->bpp); bb_u8(b, s->desc);
    for (uint32_t i = 0; i < s->id_len; i++) bb_u8(b, 'i');
    if (s->cmap_type) bb_bytes(b, s->cmap, (size_t)s->cmap_len * ((s->cmap_bits + 7u) / 8u));
    if (s->rle) put_packets(b, s->px, s->w * s->h, bytes);
    else bb_bytes(b, s->px, (size_t)s->w * s->h * bytes);
    if (s->ext_attr >= 0) {
        size_t ext = b->n;
        bb_le16(b, 495);
        bb_zero(b, 492);
        bb_u8(b, (uint32_t)s->ext_attr);
        bb_le32(b, (uint32_t)ext); bb_le32(b, 0);
        bb_bytes(b, "TRUEVISION-XFILE.", 18);
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
    if (m) *m = mm; else pc_meta_free(&mm);
    pc_doc_destroy(d);
    return px;
}

static uint8_t s5(uint32_t v) { return (uint8_t)((v * 255u + 15u) / 31u); }

/* Decode a stored value of `bits` the way the TGA spec defines it. */
static pc_px32 ref_px(const uint8_t *v, uint32_t bits, bool gray, bool alpha16)
{
    if (gray) return mkpx(v[0], v[0], v[0], bits == 16u ? v[1] : 255u);
    if (bits == 15u || bits == 16u) {
        uint32_t x = v[0] | ((uint32_t)v[1] << 8);
        return mkpx(s5((x >> 10) & 31u), s5((x >> 5) & 31u), s5(x & 31u),
                    alpha16 ? ((x & 0x8000u) ? 255u : 0u) : 255u);
    }
    if (bits == 24u) return mkpx(v[2], v[1], v[0], 255);
    return mkpx(v[2], v[1], v[0], v[3]);
}

static void t_types(void)
{
    const uint32_t W = 21, H = 9;
    static const uint32_t tbits[4] = { 15, 16, 24, 32 };
    uint8_t px[21 * 9 * 4], cmap[300 * 4];
    for (int rle = 0; rle < 2; rle++) {
        /* truecolor */
        for (int k = 0; k < 4; k++) {
            uint32_t bytes = (tbits[k] + 7u) / 8u;
            for (int ab = 0; ab < 2; ab++) {
                tspec s;
                pc_buf b;
                pc_px32 ref[21 * 9], *got;
                pc_image_meta m;
                for (uint32_t i = 0; i < W * H * bytes; i++) px[i] = rnd8();
                if (rle) for (uint32_t i = 0; i < 40 * bytes; i++) px[i] = px[i % bytes];
                 /* runs */
                memset(&s, 0, sizeof s);
                s.type = rle ? 10u : 2u; s.w = W; s.h = H; s.bpp = tbits[k];
                s.desc = 0x20u | (ab ? (tbits[k] == 32u ? 8u : 1u) : 0u);   /* top-down */
                s.px = px; s.ext_attr = -1; s.rle = rle != 0;
                build(&b, &s);
                for (uint32_t i = 0; i < W * H; i++)
                    ref[i] = ref_px(px + i * bytes, tbits[k], false, tbits[k] == 16u && ab);
                got = load_px(&b, W, H, &m, NULL);
                CHECK(got && memcmp(got, ref, sizeof ref) == 0);
                CHECK(m.src_bits == (tbits[k] <= 16u ? 5u : 8u));
                pc_meta_free(&m);
                free(got);
                pc_buf_free(&b);
            }
        }
        /* grayscale 8 and 16 (gray + alpha) */
        for (uint32_t gb = 8; gb <= 16; gb += 8) {
            tspec s;
            pc_buf b;
            pc_px32 ref[21 * 9], *got;
            for (uint32_t i = 0; i < W * H * (gb / 8u); i++) px[i] = rnd8();
            memset(&s, 0, sizeof s);
            s.type = rle ? 11u : 3u; s.w = W; s.h = H; s.bpp = gb; s.desc = 0x20;
            s.px = px; s.ext_attr = -1; s.rle = rle != 0; s.id_len = 7;
            build(&b, &s);
            for (uint32_t i = 0; i < W * H; i++)
                ref[i] = ref_px(px + i * (gb / 8u), gb, true, false);
            got = load_px(&b, W, H, NULL, NULL);
            CHECK(got && memcmp(got, ref, sizeof ref) == 0);
            free(got);
            pc_buf_free(&b);
        }
        /* color-mapped: 8 and 16-bit indices, every map depth, first entry offset */
        for (int k = 0; k < 4; k++)
            for (uint32_t ib = 8; ib <= 16; ib += 8) {
                uint32_t eb = (tbits[k] + 7u) / 8u, n = 300, first = 5;
                tspec s;
                pc_buf b;
                pc_px32 ref[21 * 9], *got;
                for (uint32_t i = 0; i < n * eb; i++) cmap[i] = rnd8();
                for (uint32_t i = 0; i < W * H; i++) {
                    uint32_t v = (ib == 8u) ? rndu(256) : rndu(320);
                    px[i * (ib / 8u)] = (uint8_t)v;
                    if (ib == 16u) px[i * 2u + 1u] = (uint8_t)(v >> 8);
                }
                memset(&s, 0, sizeof s);
                s.type = rle ? 9u : 1u; s.cmap_type = 1; s.cmap_first = first;
                s.cmap_len = ib == 8u ? 200u : n; s.cmap_bits = tbits[k];
                s.w = W; s.h = H; s.bpp = ib; s.desc = 0x20u | (tbits[k] == 16u ? 1u : 0u);
                s.cmap = cmap; s.px = px; s.ext_attr = -1; s.rle = rle != 0;
                build(&b, &s);
                for (uint32_t i = 0; i < W * H; i++) {
                    uint32_t v = ib == 8u ? px[i] : (uint32_t)(px[2 * i] | (px[2 * i + 1] << 8));
                    if (v < first || v - first >= s.cmap_len) ref[i] = mkpx(0, 0, 0, 255);
                    else ref[i] = ref_px(cmap + (v - first) * eb, tbits[k], false, tbits[k] == 16u);
                }
                {   /* the all-zero alpha rule applies to maps as well */
                    bool any = false;
                    for (uint32_t i = 0; i < W * H; i++) any = any || ref[i].a != 0;
                    if (!any) for (uint32_t i = 0; i < W * H; i++) ref[i].a = 255;
                }
                got = load_px(&b, W, H, NULL, NULL);
                CHECK(got && memcmp(got, ref, sizeof ref) == 0);
                free(got);
                pc_buf_free(&b);
            }
    }
}

static void t_origins(void)
{
    const uint32_t W = 7, H = 4;
    uint8_t px[7 * 4 * 3];
    for (uint32_t i = 0; i < sizeof px; i++) px[i] = rnd8();
    for (uint32_t o = 0; o < 4; o++) {
        tspec s;
        pc_buf b;
        pc_px32 *got;
        bool ok = true;
        memset(&s, 0, sizeof s);
        s.type = 2; s.w = W; s.h = H; s.bpp = 24; s.desc = o << 4; s.px = px; s.ext_attr = -1;
        build(&b, &s);
        got = load_px(&b, W, H, NULL, NULL);
        CHECK(got != NULL);
        for (uint32_t y = 0; y < H && got; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint32_t sx = (o & 1u) ? W - 1 - x : x;           /* bit 4: right to left */
                uint32_t sy = (o & 2u) ? y : H - 1 - y;           /* bit 5: top to bottom */
                ok = ok && px_same(got[y * W + x],
                                   ref_px(px + (sy * W + sx) * 3u, 24, false, false));
            }
        CHECK(ok);
        free(got);
        pc_buf_free(&b);
    }
}

static void t_alpha_rules(void)
{
    uint8_t px[4 * 4];
    struct { int attr; int zero; uint32_t want_a0; bool meta_alpha; } cases[] = {
        { -1, 0, 0x40, true },      /* no footer: alpha used */
        { -1, 1, 255, false },      /* no footer, all zero: opaque */
        { 3, 1, 0, false },         /* useful alpha, even when all zero */
        { 0, 0, 255, false },       /* no alpha */
        { 2, 0, 255, false },       /* undefined, retain: not shown */
        { 4, 0, 0x40, true },       /* premultiplied */
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        tspec s;
        pc_buf b;
        pc_px32 *got;
        pc_image_meta m;
        /* pixel 0: B=0x20 G=0x30 R=0x10 A=0x40; pixel 3: A=0 with color */
        static const uint8_t base[16] = { 0x20, 0x30, 0x10, 0x40, 1, 2, 3, 255,
                                          9, 9, 9, 128, 7, 7, 7, 0 };
        memcpy(px, base, 16);
        if (cases[k].zero) for (int i = 3; i < 16; i += 4) px[i] = 0;
        memset(&s, 0, sizeof s);
        s.type = 2; s.w = 4; s.h = 1; s.bpp = 32; s.desc = 0x28; s.px = px;
        s.ext_attr = cases[k].attr;
        build(&b, &s);
        got = load_px(&b, 4, 1, &m, NULL);
        CHECK(got != NULL);
        if (got) {
            CHECK(got[0].a == cases[k].want_a0);
            CHECK(m.had_alpha == cases[k].meta_alpha);
            if (cases[k].attr == 4) {       /* 0x10 * 255 / 0x40 = 63.75 -> 64 */
                CHECK(got[0].r == 64 && got[0].b == 128 && got[0].g == 191);
                CHECK(px_same(got[3], mkpx(0, 0, 0, 0)));
                CHECK(px_same(got[1], mkpx(3, 2, 1, 255)));
            } else if (got[0].a == 0x40) {
                CHECK(got[0].r == 0x10 && got[0].g == 0x30 && got[0].b == 0x20);
            }
        }
        pc_meta_free(&m);
        free(got);
        pc_buf_free(&b);
    }
}

static void t_bad(void)
{
    uint8_t px[64];
    memset(px, 7, sizeof px);
    struct {
        uint32_t type, cmap_type, cmap_bits, cmap_len, bpp, desc, w, h;
        pc_status want;
    } cases[] = {
        { 0, 0, 0, 0, 24, 0, 2, 2, PC_ERR_FORMAT },
        { 32, 0, 0, 0, 8, 0, 2, 2, PC_ERR_UNSUPPORTED },
        { 33, 0, 0, 0, 8, 0, 2, 2, PC_ERR_UNSUPPORTED },
        { 2, 0, 0, 0, 24, 0x40, 2, 2, PC_ERR_UNSUPPORTED },     /* interleaved */
        { 2, 2, 0, 0, 24, 0, 2, 2, PC_ERR_FORMAT },
        { 2, 0, 0, 0, 12, 0, 2, 2, PC_ERR_FORMAT },
        { 3, 0, 0, 0, 24, 0, 2, 2, PC_ERR_FORMAT },
        { 1, 0, 24, 4, 8, 0, 2, 2, PC_ERR_FORMAT },             /* mapped without a map */
        { 1, 1, 12, 4, 8, 0, 2, 2, PC_ERR_FORMAT },
        { 1, 1, 24, 0, 8, 0, 2, 2, PC_ERR_FORMAT },
        { 1, 1, 24, 4, 24, 0, 2, 2, PC_ERR_FORMAT },
        { 2, 0, 0, 0, 24, 0, 0, 2, PC_ERR_FORMAT },
        { 2, 0, 0, 0, 24, 0, 30, 30, PC_ERR_FORMAT },           /* truncated raw */
        { 10, 0, 0, 0, 24, 0, 30, 30, PC_ERR_FORMAT },          /* truncated RLE */
        { 2, 0, 0, 0, 32, 0, 65535, 65535, PC_ERR_FORMAT },     /* no data: before allocating */
        { 10, 0, 0, 0, 32, 0, 65535, 65535, PC_ERR_LIMIT },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_buf b;
        pc_doc *d = (pc_doc *)(uintptr_t)1;
        pc_image_meta m;
        pc_status st;
        memset(&b, 0, sizeof b);
        bb_u8(&b, 0); bb_u8(&b, cases[i].cmap_type); bb_u8(&b, cases[i].type);
        bb_le16(&b, 0); bb_le16(&b, cases[i].cmap_len); bb_u8(&b, cases[i].cmap_bits);
        bb_le32(&b, 0); bb_le16(&b, cases[i].w); bb_le16(&b, cases[i].h);
        bb_u8(&b, cases[i].bpp); bb_u8(&b, cases[i].desc);
        bb_bytes(&b, px, 40);
        st = codec_load(C, b.p, b.n, &d, &m);
        if (st != cases[i].want) INFO("bad case %u: status %d", (unsigned)i, (int)st);
        CHECK(st == cases[i].want && d == NULL);
        pc_buf_free(&b);
    }
    {
        pc_doc *d = NULL;
        pc_image_meta m;
        CHECK(C->load(px, 17, NULL, &d, &m) == PC_ERR_FORMAT);
        px[0] = 200;                                             /* id runs past the end */
        px[1] = 0; px[2] = 2; px[12] = 1; px[14] = 1; px[16] = 24; px[17] = 0;
        CHECK(C->load(px, 64, NULL, &d, &m) == PC_ERR_FORMAT && d == NULL);
        CHECK(C->load(NULL, 64, NULL, &d, &m) == PC_ERR_ARG);
    }
}

/* ---- writer ------------------------------------------------------------------- */
static void t_roundtrip(void)
{
    static const pat_fn pats[] = { pat_rgba, pat_rgb, pat_few };
    const uint32_t W = 133, H = 70;
    for (size_t pi = 0; pi < 3; pi++) {
        pc_doc *d = doc_pat(pats[pi], W, H);
        pc_px32 *flat = doc_flat(d), *white = (pc_px32 *)malloc(W * H * sizeof *white);
        for (uint32_t i = 0; i < W * H; i++) white[i] = over_white(flat[i]);
        for (int32_t dep = 0; dep <= 2; dep++)
            for (int32_t rle = 0; rle <= 1; rle++) {
                tga_params p = { dep, rle };
                pc_buf b;
                pc_px32 *got;
                uint32_t bits;
                pc_image_meta m;
                CHECK(codec_save(C, d, &p, NULL, &b) == PC_OK);
                bits = b.p[16];
                CHECK(bits == (dep == 1 ? 32u : dep == 2 ? 24u : (pi == 0 ? 32u : 24u)));
                CHECK(b.p[2] == (rle ? 10u : 2u) && b.p[17] == (bits == 32u ? 8u : 0u));
                CHECK(memcmp(b.p + b.n - 18, "TRUEVISION-XFILE.", 18) == 0);
                CHECK(C->sniff(b.p, b.n));
                got = load_px(&b, W, H, &m, NULL);
                CHECK(got && memcmp(got, bits == 32u ? flat : white, W * H * sizeof *got) == 0);
                CHECK(m.had_alpha == (bits == 32u));       /* 32-bit files carry alpha */
                pc_meta_free(&m);
                free(got);
                pc_buf_free(&b);
            }
        free(flat);
        free(white);
        pc_doc_destroy(d);
    }
}

static pc_px32 pat_clear(uint32_t x, uint32_t y) { (void)x; (void)y; return mkpx(0, 0, 0, 0); }
static pc_px32 pat_flat(uint32_t x, uint32_t y) { (void)x; (void)y; return mkpx(1, 2, 3, 255); }

static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, 0);
}

static void t_writer_details(void)
{
    pc_doc *d = doc_pat(pat_clear, 70, 66), *f = doc_pat(pat_flat, 300, 200);
    tga_params p = { 1, 1 }, raw = { 2, 0 }, rle = { 2, 1 };
    pc_buf b1, b2;
    pc_par par;
    pc_px32 *got;
    /* a fully transparent 32-bit image keeps its alpha (extension area says so) */
    CHECK(codec_save(C, d, &p, NULL, &b1) == PC_OK);
    got = load_px(&b1, 70, 66, NULL, NULL);
    CHECK(got && got[0].a == 0 && got[70 * 66 - 1].a == 0);
    CHECK(b1.p[b1.n - 26 - 1] == 3);                        /* attribute type: alpha */
    free(got);
    pc_buf_free(&b1);
    /* RLE compresses flat areas; 24-bit raw has the exact size */
    CHECK(codec_save(C, f, &raw, NULL, &b1) == PC_OK);
    CHECK(codec_save(C, f, &rle, NULL, &b2) == PC_OK);
    CHECK(b1.n == 18u + 300u * 200u * 3u + 495u + 26u);
    CHECK(b2.n < b1.n / 20u);
    pc_buf_free(&b1);
    pc_buf_free(&b2);
    /* determinism and pc_par pass-through */
    par.run = rev_run; par.self = NULL; par.threads = 4;
    pc_doc_destroy(d);
    d = doc_pat(pat_rgba, 150, 130);
    CHECK(codec_save(C, d, NULL, NULL, &b1) == PC_OK);
    memset(&b2, 0, sizeof b2);
    CHECK(C->save(d, NULL, NULL, &par, &b2) == PC_OK);
    CHECK(b1.n == b2.n && memcmp(b1.p, b2.p, b1.n) == 0);
    {
        tga_params bad = { 3, 1 }, bad2 = { 0, 2 };
        CHECK(C->save(d, NULL, &bad, NULL, &b2) == PC_ERR_ARG);
        CHECK(C->save(d, NULL, &bad2, NULL, &b2) == PC_ERR_ARG);
    }
    pc_buf_free(&b1);
    pc_buf_free(&b2);
    pc_doc_destroy(d);
    pc_doc_destroy(f);
}

/* ---- fuzzing ------------------------------------------------------------------- */
static void make_seeds(seedset *ss)
{
    pc_doc *d = doc_pat(pat_rgba, 23, 11);
    uint8_t px[12 * 5 * 4], cmap[32 * 3];
    for (int32_t dep = 0; dep <= 2; dep++)
        for (int32_t rle = 0; rle <= 1; rle++) {
            tga_params p = { dep, rle };
            pc_buf b;
            if (codec_save(C, d, &p, NULL, &b) == PC_OK) seeds_add_buf(ss, &b);
        }
    for (uint32_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i % 5u ? rnd8() : 0);
    for (uint32_t i = 0; i < sizeof cmap; i++) cmap[i] = rnd8();
    for (int rle = 0; rle < 2; rle++) {
        tspec s;
        pc_buf b;
        memset(&s, 0, sizeof s);
        s.type = rle ? 9u : 1u; s.cmap_type = 1; s.cmap_len = 32; s.cmap_bits = 24; s.w = 12;
        s.h = 5;
        s.bpp = 8; s.cmap = cmap; s.px = px; s.ext_attr = rle ? 3 : -1; s.rle = rle != 0;
        for (uint32_t i = 0; i < 60; i++) px[i] &= 31u;
        build(&b, &s); seeds_add_buf(ss, &b);
        memset(&s, 0, sizeof s);
        s.type = rle ? 11u : 3u; s.w = 12; s.h = 5; s.bpp = 16; s.px = px; s.ext_attr = -1;
        s.rle = rle != 0; s.desc = 0x30;
        build(&b, &s); seeds_add_buf(ss, &b);
        memset(&s, 0, sizeof s);
        s.type = rle ? 10u : 2u; s.w = 12; s.h = 5; s.bpp = 16; s.px = px; s.ext_attr = 4;
        s.rle = rle != 0; s.desc = 1;
        build(&b, &s); seeds_add_buf(ss, &b);
    }
    pc_doc_destroy(d);
}

#ifndef PC_LIBFUZZER
static void t_fuzz(void)
{
    seedset s;
    memset(&s, 0, sizeof s);
    make_seeds(&s);
    CHECK(s.count >= 10u);
    fuzz_seeds(C, &s, g_quick ? 4000u : 60000u);
    seeds_free(&s);
}

static void tests(void)
{
    RUN(t_types);
    RUN(t_origins);
    RUN(t_alpha_rules);
    RUN(t_bad);
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
