/* test_own_bmp.c - BMP reader and writer (src/codec/fmt_bmp.c): round trips
 * for every save option, hand-built files for each header, depth and
 * compression variant, corrupt and oversized inputs, mutation fuzzing.
 * Also a standalone fuzz driver (see test_own_common.h). */
#include "test_own_common.h"

extern const pc_codec pc_codec_bmp;
#define C (&pc_codec_bmp)

typedef struct bmp_params { int32_t depth, dither, palette; } bmp_params;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8)); }

/* ---- hand-built files ------------------------------------------------------- */
typedef struct bspec {
    uint32_t       hdr;              /* info header size */
    int32_t        w, h;
    uint32_t       bpp, comp;
    uint32_t       masks[4];         /* in the header (hdr >= 52) or after it */
    uint32_t       nmasks_after;     /* hdr 40 with bit fields: 3 or 4 */
    uint32_t       clr_used;
    const pc_px32 *pal;
    uint32_t       npal;
    int32_t        ppm;
    uint32_t       cstype;
    const uint8_t *icc;
    size_t         icc_len;
    bool           zero_offbits;
    const uint8_t *data;
    size_t         ndata;
} bspec;

static void build(pc_buf *b, const bspec *s)
{
    size_t off_pos, hdr_start, data_pos, prof_pos = 0;
    uint32_t pe = s->hdr == 12u ? 3u : 4u;
    memset(b, 0, sizeof *b);
    bb_u8(b, 'B'); bb_u8(b, 'M'); bb_le32(b, 0); bb_le32(b, 0);
    off_pos = b->n;
    bb_le32(b, 0);
    hdr_start = b->n;
    bb_le32(b, s->hdr);
    if (s->hdr == 12u) {
        bb_le16(b, (uint32_t)s->w); bb_le16(b, (uint32_t)s->h); bb_le16(b, 1); bb_le16(b, s->bpp);
    } else {
        bb_le32(b, (uint32_t)s->w); bb_le32(b, (uint32_t)s->h); bb_le16(b, 1); bb_le16(b, s->bpp);
        if (s->hdr >= 40u) {
            bb_le32(b, s->comp); bb_le32(b, 0); bb_le32(b, (uint32_t)s->ppm);
            bb_le32(b, (uint32_t)s->ppm);
            bb_le32(b, s->clr_used); bb_le32(b, 0);
        }
        if (s->hdr >= 52u && s->hdr != 64u) {
            bb_le32(b, s->masks[0]); bb_le32(b, s->masks[1]); bb_le32(b, s->masks[2]);
            if (s->hdr >= 56u) bb_le32(b, s->masks[3]);
        }
        if (s->hdr >= 108u && s->hdr != 64u) {
            bb_le32(b, s->cstype);
            bb_zero(b, 36 + 12);
        }
        if (s->hdr >= 124u && s->hdr != 64u) {
            bb_le32(b, 4);
            prof_pos = b->n;
            bb_le32(b, 0); bb_le32(b, (uint32_t)s->icc_len); bb_le32(b, 0);
        }
        while (b->n < hdr_start + s->hdr) bb_u8(b, 0);
    }
    for (uint32_t i = 0; i < s->nmasks_after; i++) bb_le32(b, s->masks[i]);
    for (uint32_t i = 0; i < s->npal; i++) {
        bb_u8(b, s->pal[i].b); bb_u8(b, s->pal[i].g); bb_u8(b, s->pal[i].r);
        if (pe == 4u) bb_u8(b, 0);
    }
    data_pos = b->n;
    bb_bytes(b, s->data, s->ndata);
    if (s->icc_len) {
        bb_set_le32(b, prof_pos, (uint32_t)(b->n - hdr_start));
        bb_bytes(b, s->icc, s->icc_len);
    }
    bb_set_le32(b, off_pos, s->zero_offbits ? 0u : (uint32_t)data_pos);
    bb_set_le32(b, 2, (uint32_t)b->n);
}

/* Pack w x h pixel values of `bits` each (MSB first) into 4-byte padded rows,
 * stored in the given row order. */
static void pack(uint8_t *out, const uint32_t *v, uint32_t w, uint32_t h, uint32_t bits)
{
    uint32_t stride = ((w * bits + 31u) / 32u) * 4u;
    memset(out, 0, (size_t)stride * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *row = out + (size_t)y * stride;
            uint32_t val = v[y * w + x];
            if (bits < 8u) {
                row[(x * bits) / 8u] |= (uint8_t)(val << (8u - bits - (x * bits) % 8u));
            } else {
                for (uint32_t k = 0; k < bits / 8u; k++)
                    row[x * (bits / 8u) + k] = (uint8_t)(val >> (8u * k));
            }
        }
}

static pc_status load(const pc_buf *b, pc_doc **d, pc_image_meta *m)
{
    return codec_load(C, b->p, b->n, d, m);
}

static pc_px32 *loaded_px(const pc_buf *b, uint32_t w, uint32_t h, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_image_meta mm;
    pc_px32 *px;
    if (load(b, &d, &mm) != PC_OK) return NULL;
    if (d->w != w || d->h != h) { pc_doc_destroy(d); pc_meta_free(&mm); return NULL; }
    px = doc_layer0(d);
    if (m) *m = mm; else pc_meta_free(&mm);
    pc_doc_destroy(d);
    return px;
}

static pc_px32 k_pal16[16];

static void init_pal(void)
{
    for (uint32_t i = 0; i < 16; i++)
        k_pal16[i] = mkpx(i * 16u, 255u - i * 9u, (i * 77u) & 255u, 255);
}

static void t_palettes(void)
{
    static const uint32_t bits[4] = { 1, 2, 4, 8 };
    const uint32_t W = 13, H = 5;
    for (int bi = 0; bi < 4; bi++) {
        for (int td = 0; td < 2; td++) {
            uint32_t v[13 * 5], npal = bits[bi] == 8u ? 16u : (1u << bits[bi]);
            uint8_t data[512];
            pc_px32 *got;
            pc_buf b;
            bspec s;
            pc_px32 ref[13 * 5];
            memset(&s, 0, sizeof s);
            for (uint32_t i = 0; i < W * H; i++) v[i] = rndu(npal);
            /* stored rows: bottom-up unless top-down */
            {
                uint32_t sv[13 * 5];
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < W; x++)
                        sv[y * W + x] = v[(td ? y : H - 1 - y) * W + x];
                pack(data, sv, W, H, bits[bi]);
            }
            s.hdr = 40; s.w = (int32_t)W; s.h = td ? -(int32_t)H : (int32_t)H; s.bpp = bits[bi];
            s.pal = k_pal16; s.npal = npal; s.clr_used = bits[bi] == 8u ? 16u : 0u;
            s.data = data; s.ndata = ((W * bits[bi] + 31u) / 32u) * 4u * H;
            s.ppm = 3780;
            build(&b, &s);
            for (uint32_t i = 0; i < W * H; i++) ref[i] = k_pal16[v[i]];
            {
                pc_image_meta m;
                got = loaded_px(&b, W, H, &m);
                CHECK(got && memcmp(got, ref, sizeof ref) == 0);
                CHECK(m.src_bits == 8u && !m.had_alpha);
                CHECK(m.dpi_x > 96.0 && m.dpi_x < 96.03);
                pc_meta_free(&m);
            }
            free(got);
            pc_buf_free(&b);
        }
    }
    {   /* BITMAPCOREHEADER (3-byte palette), OS/2 2.x 16-byte header, off_bits 0 */
        static const uint32_t hdrs[3] = { 12, 16, 64 };
        for (int k = 0; k < 3; k++) {
            uint32_t v[7 * 3], W2 = 7, H2 = 3;
            uint8_t data[64];
            pc_buf b;
            bspec s;
            pc_px32 *got, ref[7 * 3];
            memset(&s, 0, sizeof s);
            for (uint32_t i = 0; i < W2 * H2; i++) v[i] = rndu(16);
            {
                uint32_t sv[7 * 3];
                for (uint32_t y = 0; y < H2; y++)
                    for (uint32_t x = 0; x < W2; x++) sv[y * W2 + x] = v[(H2 - 1 - y) * W2 + x];
                pack(data, sv, W2, H2, 4);
            }
            s.hdr = hdrs[k]; s.w = (int32_t)W2; s.h = (int32_t)H2; s.bpp = 4;
            s.pal = k_pal16; s.npal = 16; s.data = data; s.ndata = 4u * H2;
            s.zero_offbits = k == 1;
            build(&b, &s);
            for (uint32_t i = 0; i < W2 * H2; i++) ref[i] = k_pal16[v[i]];
            got = loaded_px(&b, W2, H2, NULL);
            CHECK(got && memcmp(got, ref, sizeof ref) == 0);
            free(got);
            pc_buf_free(&b);
        }
    }
    {   /* short palette: missing entries and out-of-range indices are black */
        uint32_t v[4] = { 0, 1, 2, 255 };
        uint8_t data[8];
        pc_buf b;
        bspec s;
        pc_px32 *got;
        memset(&s, 0, sizeof s);
        pack(data, v, 4, 1, 8);
        s.hdr = 40; s.w = 4; s.h = 1; s.bpp = 8; s.clr_used = 2; s.pal = k_pal16; s.npal = 2;
        s.data = data; s.ndata = 4;
        build(&b, &s);
        got = loaded_px(&b, 4, 1, NULL);
        CHECK(got && px_same(got[0], k_pal16[0]) && px_same(got[1], k_pal16[1]) &&
              px_same(got[2], mkpx(0, 0, 0, 255)) && px_same(got[3], mkpx(0, 0, 0, 255)));
        free(got);
        pc_buf_free(&b);
    }
}

static uint8_t sc(uint32_t v, uint32_t bits)
{
    return (uint8_t)((v * 255u + ((1u << bits) - 1u) / 2u) / ((1u << bits) - 1u));
}

static void t_high_color(void)
{
    const uint32_t W = 5, H = 3;
    uint32_t v[15];
    uint8_t data[256];
    pc_buf b;
    bspec s;
    pc_px32 *got, ref[15];
    /* 16-bit BI_RGB = 555 */
    for (uint32_t i = 0; i < 15; i++) v[i] = rnd8() | ((uint32_t)rnd8() << 8);
    memset(&s, 0, sizeof s);
    pack(data, v, W, H, 16);
    s.hdr = 40; s.w = (int32_t)W; s.h = -(int32_t)H; s.bpp = 16; s.data = data; s.ndata = 12u * H;
    build(&b, &s);
    for (uint32_t i = 0; i < 15; i++)
        ref[i] = mkpx(sc((v[i] >> 10) & 31u, 5), sc((v[i] >> 5) & 31u, 5), sc(v[i] & 31u, 5), 255);
    got = loaded_px(&b, W, H, NULL);
    CHECK(got && memcmp(got, ref, sizeof ref) == 0);
    free(got);
    pc_buf_free(&b);
    /* 565 with masks after a 40-byte header */
    s.comp = 3; s.masks[0] = 0xF800; s.masks[1] = 0x07E0; s.masks[2] = 0x001F; s.nmasks_after = 3;
    build(&b, &s);
    for (uint32_t i = 0; i < 15; i++)
        ref[i] = mkpx(sc((v[i] >> 11) & 31u, 5), sc((v[i] >> 5) & 63u, 6), sc(v[i] & 31u, 5), 255);
    got = loaded_px(&b, W, H, NULL);
    CHECK(got && memcmp(got, ref, sizeof ref) == 0);
    free(got);
    pc_buf_free(&b);
    /* 1555 with BI_ALPHABITFIELDS: alpha bit */
    s.comp = 6; s.masks[0] = 0x7C00; s.masks[1] = 0x03E0; s.masks[2] = 0x001F; s.masks[3] = 0x8000;
    s.nmasks_after = 4;
    build(&b, &s);
    for (uint32_t i = 0; i < 15; i++)
        ref[i] = mkpx(sc((v[i] >> 10) & 31u, 5), sc((v[i] >> 5) & 31u, 5), sc(v[i] & 31u, 5),
                      (v[i] & 0x8000u) ? 255u : 0u);
    {
        pc_image_meta m;
        got = loaded_px(&b, W, H, &m);
        CHECK(got && memcmp(got, ref, sizeof ref) == 0);
        CHECK(m.had_alpha && m.src_bits == 5u);
        pc_meta_free(&m);
    }
    free(got);
    pc_buf_free(&b);
    /* 32-bit 2:10:10:10 in a V4 header */
    for (uint32_t i = 0; i < 15; i++) v[i] = (uint32_t)rnd() ;
    memset(&s, 0, sizeof s);
    pack(data, v, W, H, 32);
    s.hdr = 108; s.w = (int32_t)W; s.h = (int32_t)H; s.bpp = 32; s.comp = 3;
    s.masks[0] = 0x3FF00000; s.masks[1] = 0x000FFC00; s.masks[2] = 0x000003FF;
    s.masks[3] = 0xC0000000;
    s.data = data; s.ndata = 20u * H;
    build(&b, &s);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t q = v[(H - 1 - y) * W + x];
            ref[y * W + x] = mkpx(sc((q >> 20) & 1023u, 10), sc((q >> 10) & 1023u, 10),
                                  sc(q & 1023u, 10), sc(q >> 30, 2));
        }
    got = loaded_px(&b, W, H, NULL);
    CHECK(got && memcmp(got, ref, sizeof ref) == 0);
    free(got);
    pc_buf_free(&b);
    /* 24-bit and 32-bit BI_RGB: the fourth byte is alpha unless all zero */
    for (int k = 0; k < 3; k++) {
        uint32_t bpp = k == 0 ? 24u : 32u;
        pc_image_meta m;
        for (uint32_t i = 0; i < 15; i++) {
            v[i] = rnd8() | ((uint32_t)rnd8() << 8) | ((uint32_t)rnd8() << 16);
            if (k == 2) v[i] |= (uint32_t)rnd8() << 24;
        }
        memset(&s, 0, sizeof s);
        pack(data, v, W, H, bpp);
        s.hdr = 40; s.w = (int32_t)W; s.h = -(int32_t)H; s.bpp = bpp; s.data = data;
        s.ndata = (size_t)((W * bpp + 31u) / 32u) * 4u * H;
        build(&b, &s);
        for (uint32_t i = 0; i < 15; i++)
            ref[i] = mkpx((v[i] >> 16) & 255u, (v[i] >> 8) & 255u, v[i] & 255u,
                          k == 2 ? v[i] >> 24 : 255u);
        got = loaded_px(&b, W, H, &m);
        CHECK(got && memcmp(got, ref, sizeof ref) == 0);
        CHECK(m.had_alpha == (k == 2));
        pc_meta_free(&m);
        free(got);
        pc_buf_free(&b);
    }
    /* V3 header with bit fields but an empty alpha mask: opaque */
    memset(&s, 0, sizeof s);
    for (uint32_t i = 0; i < 15; i++) v[i] = (uint32_t)rnd();
    pack(data, v, W, H, 32);
    s.hdr = 56; s.w = (int32_t)W; s.h = -(int32_t)H; s.bpp = 32; s.comp = 3;
    s.masks[0] = 0xFF; s.masks[1] = 0xFF00; s.masks[2] = 0xFF0000;
    s.data = data; s.ndata = 20u * H;
    build(&b, &s);
    for (uint32_t i = 0; i < 15; i++)
        ref[i] = mkpx(v[i] & 255u, (v[i] >> 8) & 255u, (v[i] >> 16) & 255u, 255);
    got = loaded_px(&b, W, H, NULL);
    CHECK(got && memcmp(got, ref, sizeof ref) == 0);
    free(got);
    pc_buf_free(&b);
}

/* RLE: escapes, deltas, absolute runs, padding, skipped pixels. */
static void t_rle(void)
{
    pc_buf b;
    bspec s;
    pc_px32 *got;
    pc_image_meta m;
    /* RLE8, 6 x 4. Stored row 0 is the bottom. */
    static const uint8_t rle8[] = {
        3, 1, 0, 3, 7, 8, 9, 0,     /* row0: 1 1 1 7 8 9 (absolute run of 3, padded) */
        0, 0,                       /* end of line */
        0, 2, 2, 1,                 /* delta +2, +1: row1 skipped, row2 at x=2 */
        2, 5,                       /* row2: x2..3 = 5 */
        0, 0,
        6, 4, 0, 1                  /* row3: all 4, end of bitmap */
    };
    memset(&s, 0, sizeof s);
    s.hdr = 40; s.w = 6; s.h = 4; s.bpp = 8; s.comp = 1; s.pal = k_pal16; s.npal = 16;
    s.clr_used = 16;
    s.data = rle8; s.ndata = sizeof rle8;
    build(&b, &s);
    got = loaded_px(&b, 6, 4, &m);
    CHECK(got != NULL);
    if (got) {
        const pc_px32 z = mkpx(0, 0, 0, 0);
        /* document rows are top-down: doc row 3 = stored row 0 */
        CHECK(px_same(got[3 * 6 + 0], k_pal16[1]) && px_same(got[3 * 6 + 2], k_pal16[1]));
        CHECK(px_same(got[3 * 6 + 3], k_pal16[7]) && px_same(got[3 * 6 + 5], k_pal16[9]));
        for (int x = 0; x < 6; x++) CHECK(px_same(got[2 * 6 + x], z));            /* skipped */
        CHECK(px_same(got[1 * 6 + 0], z) && px_same(got[1 * 6 + 1], z));
        CHECK(px_same(got[1 * 6 + 2], k_pal16[5]) && px_same(got[1 * 6 + 3], k_pal16[5]));
        CHECK(px_same(got[1 * 6 + 4], z));
        for (int x = 0; x < 6; x++) CHECK(px_same(got[x], k_pal16[4]));
        CHECK(m.had_alpha);
        pc_meta_free(&m);
    }
    free(got);
    pc_buf_free(&b);
    {   /* RLE4: alternating nibbles, odd absolute run, run into the row padding */
        static const uint8_t rle4[] = {
            5, 0x12,                /* 1 2 1 2 1 */
            0, 3, 0xAB, 0xC0,       /* absolute A B C (2 bytes, already even) */
            0, 0,
            8, 0x34,                /* 3 4 3 4 3 4 3 4: one pixel into the padding */
            0, 1
        };
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 7; s.h = 2; s.bpp = 4; s.comp = 2; s.pal = k_pal16; s.npal = 16;
        s.data = rle4; s.ndata = sizeof rle4;
        build(&b, &s);
        got = loaded_px(&b, 7, 2, &m);
        CHECK(got != NULL);
        if (got) {
            static const uint32_t r1[7] = { 1, 2, 1, 2, 1, 10, 11 };
            static const uint32_t r0[7] = { 3, 4, 3, 4, 3, 4, 3 };
            bool ok = true;
            for (int x = 0; x < 7; x++) ok = ok && px_same(got[7 + x], k_pal16[r1[x]]);
            CHECK(ok);   /* the C nibble fell into the padding of row 0 (x = 7): dropped */
            ok = true;
            for (int x = 0; x < 7; x++) ok = ok && px_same(got[x], k_pal16[r0[x]]);
            CHECK(ok);
            CHECK(!m.had_alpha);
            pc_meta_free(&m);
        }
        free(got);
        pc_buf_free(&b);
    }
    {   /* overruns past the padded row, deltas past the row, top-down RLE */
        static const uint8_t over[] = { 9, 1, 0, 1 };           /* w=7: padded width 8 */
        static const uint8_t over_abs[] = { 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 1 };
        static const uint8_t delta[] = { 0, 2, 9, 0, 0, 1 };
        static const uint8_t trunc[] = { 3, 1, 0, 0, 2, 2 };   /* data ends: partial image */
        const uint8_t *cases[4] = { over, over_abs, delta, trunc };
        size_t lens[4] = { sizeof over, sizeof over_abs, sizeof delta, sizeof trunc };
        pc_status want[4] = { PC_ERR_FORMAT, PC_ERR_FORMAT, PC_ERR_FORMAT, PC_OK };
        for (int k = 0; k < 4; k++) {
            pc_doc *d = NULL;
            memset(&s, 0, sizeof s);
            s.hdr = 40; s.w = 7; s.h = 3; s.bpp = 8; s.comp = 1; s.pal = k_pal16; s.npal = 16;
            s.clr_used = 16; s.data = cases[k]; s.ndata = lens[k];
            build(&b, &s);
            CHECK(load(&b, &d, &m) == want[k]);
            if (d) { pc_meta_free(&m); pc_doc_destroy(d); }
            pc_buf_free(&b);
        }
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 7; s.h = -3; s.bpp = 8; s.comp = 1; s.data = trunc;
        s.ndata = sizeof trunc;
        build(&b, &s);
        {
            pc_doc *d = NULL;
            CHECK(load(&b, &d, &m) == PC_ERR_FORMAT && d == NULL);
        }
        pc_buf_free(&b);
    }
}

static void t_bad_headers(void)
{
    uint8_t data[64];
    memset(data, 0x55, sizeof data);
    struct { uint32_t hdr; int32_t w, h; uint32_t bpp, comp; pc_status want; } cases[] = {
        { 40, 0, 4, 24, 0, PC_ERR_FORMAT },
        { 40, 4, 0, 24, 0, PC_ERR_FORMAT },
        { 40, -4, 4, 24, 0, PC_ERR_FORMAT },
        { 40, 4, INT32_MIN, 24, 0, PC_ERR_FORMAT },
        { 40, 4, 4, 7, 0, PC_ERR_FORMAT },
        { 40, 4, 4, 0, 4, PC_ERR_UNSUPPORTED },          /* BI_JPEG */
        { 40, 4, 4, 0, 5, PC_ERR_UNSUPPORTED },          /* BI_PNG */
        { 40, 4, 4, 64, 0, PC_ERR_UNSUPPORTED },
        { 40, 4, 4, 32, 11, PC_ERR_UNSUPPORTED },        /* CMYK */
        { 40, 4, 4, 32, 9, PC_ERR_FORMAT },
        { 40, 4, 4, 24, 3, PC_ERR_UNSUPPORTED },         /* bit fields on 24-bit */
        { 40, 4, 4, 4, 1, PC_ERR_FORMAT },               /* RLE8 needs 8 bits */
        { 40, 4, 4, 8, 2, PC_ERR_FORMAT },
        { 64, 4, 4, 1, 3, PC_ERR_UNSUPPORTED },          /* OS/2 Huffman */
        { 64, 4, 4, 24, 4, PC_ERR_UNSUPPORTED },         /* OS/2 RLE24 */
        { 40, 65535, 65535, 24, 0, PC_ERR_LIMIT },
        { 40, 40000, 20000, 24, 0, PC_ERR_FORMAT },     /* no data: fails before allocating */
        { 40, 3, 1000, 24, 0, PC_ERR_FORMAT },          /* truncated */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_buf b;
        bspec s;
        pc_doc *d = (pc_doc *)(uintptr_t)1;
        pc_image_meta m;
        pc_status st;
        memset(&s, 0, sizeof s);
        s.hdr = cases[i].hdr; s.w = cases[i].w; s.h = cases[i].h; s.bpp = cases[i].bpp;
        s.comp = cases[i].comp; s.data = data; s.ndata = 16;
        if (s.comp == 3) {
            s.nmasks_after = 3;
            s.masks[0] = 0xFF;
            s.masks[1] = 0xFF00;
            s.masks[2] = 0xFF0000;
        }
        build(&b, &s);
        st = load(&b, &d, &m);
        if (st != cases[i].want) INFO("bad header case %u: status %d", (unsigned)i, (int)st);
        CHECK(st == cases[i].want && d == NULL);
        pc_buf_free(&b);
    }
    {   /* malformed header sizes, offsets and masks */
        pc_buf b;
        bspec s;
        pc_doc *d = NULL;
        pc_image_meta m;
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 2; s.h = 2; s.bpp = 32; s.comp = 3; s.nmasks_after = 3;
        s.masks[0] = 0xF0F0; s.masks[1] = 0xFF0000; s.masks[2] = 0xFF000000; s.data = data;
        s.ndata = 16;
        build(&b, &s);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);                    /* non-contiguous mask */
        pc_buf_free(&b);
        s.masks[0] = 0x1FFFF; s.bpp = 16;
        build(&b, &s);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);                    /* mask wider than 16 */
        pc_buf_free(&b);
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 2; s.h = 2; s.bpp = 24; s.data = data; s.ndata = 16;
        build(&b, &s);
        bb_set_le32(&b, 14, 11);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);
        bb_set_le32(&b, 14, 4000);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);
        bb_set_le32(&b, 14, 40);
        bb_set_le32(&b, 10, (uint32_t)b.n + 3u);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);
        bb_set_le32(&b, 10, 20);
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT);
        b.p[0] = 'X';
        CHECK(load(&b, &d, &m) == PC_ERR_FORMAT && d == NULL);
        pc_buf_free(&b);
        CHECK(C->load(NULL, 5, NULL, &d, &m) == PC_ERR_ARG);
    }
}

static void t_empty_and_icc(void)
{
    pc_doc *d = NULL;
    pc_image_meta m;
    static const uint8_t none[1] = { 0 };
    CHECK(C->load(none, 0, NULL, &d, &m) == PC_OK);           /* Explorer's empty .bmp */
    CHECK(d && d->w == 800u && d->h == 600u);
    if (d) CHECK(px_same(pc_layer_get_px(d->stack[0], 799, 599), mkpx(255, 255, 255, 255)));
    pc_meta_free(&m);
    pc_doc_destroy(d);
    {
        uint8_t icc[300], data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        pc_buf b;
        bspec s;
        for (int i = 0; i < 300; i++) icc[i] = (uint8_t)(i * 7);
        memset(&s, 0, sizeof s);
        s.hdr = 124; s.w = 2; s.h = 1; s.bpp = 24; s.data = data; s.ndata = 8;
        s.cstype = 0x4D424544u; s.icc = icc; s.icc_len = sizeof icc;
        build(&b, &s);
        d = NULL;
        CHECK(load(&b, &d, &m) == PC_OK);
        CHECK(m.icc_len == sizeof icc && m.icc && memcmp(m.icc, icc, sizeof icc) == 0);
        pc_meta_free(&m);
        pc_doc_destroy(d);
        bb_set_le32(&b, 14 + 116, 100000);                    /* profile size past the end */
        d = NULL;
        CHECK(load(&b, &d, &m) == PC_OK && m.icc == NULL && m.icc_len == 0u);
        pc_meta_free(&m);
        pc_doc_destroy(d);
        pc_buf_free(&b);
    }
}

/* ---- writer ------------------------------------------------------------------- */
static uint32_t count_colors(const pc_px32 *px, size_t n)
{
    pc_px32 seen[300];
    uint32_t k = 0;
    for (size_t i = 0; i < n && k < 300; i++) {
        bool f = false;
        for (uint32_t j = 0; j < k && !f; j++) f = px_same(seen[j], px[i]);
        if (!f) seen[k++] = px[i];
    }
    return k;
}

static pc_px32 pat_2c(uint32_t x, uint32_t y)
{
    return ((x ^ y) & 4u) ? mkpx(10, 200, 30, 255) : mkpx(250, 250, 5, 255);
}
static pc_px32 pat_noise(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;
    return mkpx(rnd8(), rnd8(), rnd8(), 255);
}

static void t_roundtrip(void)
{
    static const pat_fn pats[] = { pat_rgba, pat_rgb, pat_few, pat_2c, pat_gray, pat_noise };
    static const char *const names[] = { "rgba", "rgb", "few", "2c", "gray", "noise" };
    static const uint32_t k_bpp[6] = { 0, 32, 24, 8, 4, 1 };
    const uint32_t W = 67, H = 45;
    for (size_t pi = 0; pi < sizeof pats / sizeof pats[0]; pi++) {
        pc_doc *d = doc_pat(pats[pi], W, H);
        pc_px32 *flat = doc_flat(d), *white = (pc_px32 *)malloc(W * H * sizeof *white);
        uint32_t ncol;
        bool opaque = true;
        for (uint32_t i = 0; i < W * H; i++) {
            white[i] = over_white(flat[i]);
            opaque = opaque && flat[i].a == 255;
        }
        ncol = count_colors(white, W * H);
        for (int32_t dep = 0; dep <= 5; dep++)
            for (int32_t pal = 0; pal < 2; pal++) {
                bmp_params p = { dep, 7, pal };
                pc_buf b;
                uint32_t bpp, want;
                pc_px32 *got;
                CHECK(codec_save(C, d, &p, NULL, &b) == PC_OK);
                bpp = rd16(b.p + 28);
                want = k_bpp[dep];
                if (dep == 0)
                    want = !opaque ? 32u : ncol <= 2u ? 1u : ncol <= 16u ? 4u
                         : ncol <= 256u ? 8u : 24u;
                CHECK(bpp == want);
                CHECK(rd32(b.p + 2) == b.n && C->sniff(b.p, b.n));
                got = loaded_px(&b, W, H, NULL);
                CHECK(got != NULL);
                if (got) {
                    const pc_px32 *ref = bpp == 32u ? flat : white;
                    uint32_t md = px_maxdiff(got, ref, W * H);
                    if (bpp >= 24u || ncol <= (1u << bpp)) {
                        if (md) INFO("%s depth %d: max diff %u", names[pi], dep, md);
                        CHECK(md == 0u);
                    } else {
                        double ps = px_psnr(got, ref, W * H);
                        double lim = bpp == 8u ? 20.0 : bpp == 4u ? 14.0 : 7.0;
                        if (ps < lim) INFO("%s depth %d: PSNR %.2f", names[pi], dep, ps);
                        CHECK(ps >= lim);
                        CHECK(rd32(b.p + 46) <= (1u << bpp));
                    }
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
    for (uint32_t i = count; i-- > 0;) fn(ud, i, i & 1u);
}

static void t_writer_details(void)
{
    pc_doc *d = doc_pat(pat_rgba, 130, 70);
    pc_layer *l = pc_layer_create(d, "top");
    pc_px32 *top = pat_image(pat_few, 130, 70), *flat;
    pc_image_meta meta;
    pc_buf b1, b2, b3;
    pc_par par;
    bmp_params p32 = { 1, 7, 0 };
    CHECK(pc_layer_store_rect(d, l, pc_rect_make(10, 5, 100, 50), top, 130) == PC_OK);
    l->mode = PC_BLEND_DIFFERENCE;
    l->opacity = 200;
    CHECK(pc_doc_insert_layer(d, l, 1) == PC_OK);
    flat = doc_flat(d);
    memset(&meta, 0, sizeof meta);
    meta.dpi_x = 150.0; meta.dpi_y = 300.0;
    CHECK(codec_save(C, d, &p32, &meta, &b1) == PC_OK);
    /* header fields of the 32-bit V5 output */
    CHECK(rd32(b1.p + 14) == 124u && rd32(b1.p + 30) == 3u);
    CHECK(rd32(b1.p + 54) == 0x00FF0000u && rd32(b1.p + 58) == 0x0000FF00u &&
          rd32(b1.p + 62) == 0x000000FFu && rd32(b1.p + 66) == 0xFF000000u);
    CHECK(rd32(b1.p + 70) == 0x73524742u);
    CHECK(rd32(b1.p + 38) == 5906u && rd32(b1.p + 42) == 11811u);
    CHECK(rd32(b1.p + 10) == 14u + 124u && rd32(b1.p + 34) == 130u * 70u * 4u);
    {
        pc_image_meta m;
        pc_px32 *got = loaded_px(&b1, 130, 70, &m);
        CHECK(got && memcmp(got, flat, 130 * 70 * sizeof *got) == 0);   /* layers flattened */
        CHECK(fabs(m.dpi_x - 150.0) < 0.02 && fabs(m.dpi_y - 300.0) < 0.02);   /* ppm rounding */
        CHECK(m.had_alpha);
        pc_meta_free(&m);
        free(got);
    }
    /* determinism and pc_par pass-through */
    par.run = rev_run; par.self = NULL; par.threads = 2;
    CHECK(C->save(d, &meta, &p32, &par, (memset(&b2, 0, sizeof b2), &b2)) == PC_OK);
    CHECK(b1.n == b2.n && memcmp(b1.p, b2.p, b1.n) == 0);
    {
        bmp_params p8 = { 3, 7, 0 };
        CHECK(codec_save(C, d, &p8, NULL, &b3) == PC_OK);
        pc_buf_free(&b2);
        CHECK(C->save(d, NULL, &p8, &par, (memset(&b2, 0, sizeof b2), &b2)) == PC_OK);
        CHECK(b3.n == b2.n && memcmp(b3.p, b2.p, b3.n) == 0);
        CHECK(rd32(b3.p + 38) == 3780u && rd32(b3.p + 14) == 40u);   /* 96 dpi default */
    }
    /* appends to existing data; bad params */
    {
        pc_buf b = { 0 };
        bmp_params bad = { 6, 7, 0 }, bad2 = { 0, 9, 0 }, bad3 = { 0, 7, 2 };
        CHECK(pc_buf_append(&b, "xyz", 3) == PC_OK);
        CHECK(C->save(d, NULL, NULL, NULL, &b) == PC_OK);
        CHECK(b.n > 3u && memcmp(b.p, "xyzBM", 5) == 0);
        CHECK(C->save(d, NULL, &bad, NULL, &b) == PC_ERR_ARG);
        CHECK(C->save(d, NULL, &bad2, NULL, &b) == PC_ERR_ARG);
        CHECK(C->save(d, NULL, &bad3, NULL, &b) == PC_ERR_ARG);
        CHECK(C->save(NULL, NULL, NULL, NULL, &b) == PC_ERR_ARG);
        pc_buf_free(&b);
    }
    pc_buf_free(&b1);
    pc_buf_free(&b2);
    pc_buf_free(&b3);
    free(top);
    free(flat);
    pc_doc_destroy(d);
}

/* ---- fuzzing ------------------------------------------------------------------- */
static void make_seeds(seedset *ss)
{
    pc_doc *d = doc_pat(pat_rgba, 23, 11), *f = doc_pat(pat_few, 19, 7);
    static const uint8_t rle8[] = { 3, 1, 0, 3, 7, 8, 9, 0, 0, 0, 0, 2, 2, 1, 2, 5, 0, 0, 6, 4, 0,
                                    1 };
    static const uint8_t rle4[] = { 5, 0x12, 0, 3, 0xAB, 0xC0, 0, 0, 7, 0x34, 0, 1 };
    for (int32_t dep = 0; dep <= 5; dep++) {
        bmp_params p = { dep, 7, dep & 1 };
        pc_buf b;
        if (codec_save(C, dep == 3 ? f : d, &p, NULL, &b) == PC_OK) seeds_add_buf(ss, &b);
    }
    {
        pc_buf b;
        bspec s;
        uint8_t data[64];
        memset(data, 0x3C, sizeof data);
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 6; s.h = 4; s.bpp = 8; s.comp = 1; s.pal = k_pal16; s.npal = 16;
        s.clr_used = 16; s.data = rle8; s.ndata = sizeof rle8;
        build(&b, &s); seeds_add_buf(ss, &b);
        s.w = 7; s.h = 2; s.bpp = 4; s.comp = 2; s.data = rle4; s.ndata = sizeof rle4;
        s.clr_used = 0;
        build(&b, &s); seeds_add_buf(ss, &b);
        memset(&s, 0, sizeof s);
        s.hdr = 12; s.w = 5; s.h = 3; s.bpp = 4; s.pal = k_pal16; s.npal = 16; s.data = data;
        s.ndata = 12;
        build(&b, &s); seeds_add_buf(ss, &b);
        memset(&s, 0, sizeof s);
        s.hdr = 40; s.w = 5; s.h = -3; s.bpp = 16; s.comp = 6; s.nmasks_after = 4;
        s.masks[0] = 0x7C00; s.masks[1] = 0x3E0; s.masks[2] = 0x1F; s.masks[3] = 0x8000;
        s.data = data; s.ndata = 36;
        build(&b, &s); seeds_add_buf(ss, &b);
        memset(&s, 0, sizeof s);
        s.hdr = 124; s.w = 3; s.h = 3; s.bpp = 32; s.comp = 3; s.cstype = 0x4D424544u;
        s.masks[0] = 0xFF0000; s.masks[1] = 0xFF00; s.masks[2] = 0xFF; s.masks[3] = 0xFF000000u;
        s.data = data; s.ndata = 36; s.icc = data; s.icc_len = 20;
        build(&b, &s); seeds_add_buf(ss, &b);
    }
    pc_doc_destroy(d);
    pc_doc_destroy(f);
}

static void t_fuzz(void)
{
    seedset s;
    memset(&s, 0, sizeof s);
    make_seeds(&s);
    CHECK(s.count >= 10u);
    fuzz_seeds(C, &s, g_quick ? 4000u : 60000u);
    seeds_free(&s);
}

#ifndef PC_LIBFUZZER
static void tests(void)
{
    init_pal();
    RUN(t_palettes);
    RUN(t_high_color);
    RUN(t_rle);
    RUN(t_bad_headers);
    RUN(t_empty_and_icc);
    RUN(t_roundtrip);
    RUN(t_writer_details);
    RUN(t_fuzz);
}

static void seeds_main(seedset *s)
{
    init_pal();
    make_seeds(s);
}

int main(int argc, char **argv)
{
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
