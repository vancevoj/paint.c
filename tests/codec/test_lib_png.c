/* test_lib_png.c - PNG codec (libspng): decoding of every color type, bit
 * depth, filter and interlace mode against an independent encoder written
 * here, metadata, save modes and round trips, limits, mutation fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "spng.h"

static const pc_codec *png(void) { return pc_codec_by_id("png"); }

/* ---- independent fixture encoder ---------------------------------------------- */
typedef struct fx_png {
    uint32_t w, h;
    int      depth, ct, il;
    uint16_t *s;           /* samples: w*h*channels, raw values */
    int      ch;           /* channels per pixel in the file */
    uint8_t  plte[256][3];
    int      n_plte;
    int      trns;         /* 1: tRNS chunk present */
    uint8_t  trns_pal[256];
    int      n_trns_pal;
    uint16_t trns_key[3];  /* gray (key[0]) or rgb */
} fx_png;

static int ct_channels(int ct)
{
    switch (ct) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    default: return 4;
    }
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

/* Pack one row of a sub-image (pixel columns x0 + i*dx of image row y). */
static size_t pack_row(const fx_png *f, uint32_t y, uint32_t x0, uint32_t dx, uint32_t pw,
                       uint8_t *out)
{
    size_t bits = (size_t)pw * (size_t)f->ch * (size_t)f->depth, nbytes = (bits + 7u) / 8u;
    size_t bitpos = 0;
    memset(out, 0, nbytes);
    for (uint32_t i = 0; i < pw; i++) {
        uint32_t x = x0 + i * dx;
        for (int c = 0; c < f->ch; c++) {
            uint16_t v = f->s[((size_t)y * f->w + x) * (size_t)f->ch + (size_t)c];
            if (f->depth == 16) {
                out[bitpos / 8u] = (uint8_t)(v >> 8);
                out[bitpos / 8u + 1u] = (uint8_t)v;
            } else if (f->depth == 8) {
                out[bitpos / 8u] = (uint8_t)v;
            } else {
                size_t shift = 8u - (size_t)f->depth - (bitpos % 8u);
                out[bitpos / 8u] |= (uint8_t)(v << shift);
            }
            bitpos += (size_t)f->depth;
        }
    }
    return nbytes;
}

static void filter_row(uint8_t *dst, const uint8_t *cur, const uint8_t *prev, size_t n,
                       size_t bpp, int type)
{
    for (size_t i = 0; i < n; i++) {
        int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        int x = cur[i], v;
        switch (type) {
        case 1: v = x - a; break;
        case 2: v = x - b; break;
        case 3: v = x - ((a + b) >> 1); break;
        case 4: v = x - paeth(a, b, c); break;
        default: v = x; break;
        }
        dst[i] = (uint8_t)v;
    }
}

static void build_png(const fx_png *f, pc_buf *out, bool with_meta, const uint8_t *icc,
                      size_t icc_len)
{
    static const uint32_t ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const uint32_t adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    pc_buf raw, z;
    size_t rowmax = ((size_t)f->w * (size_t)f->ch * (size_t)f->depth + 7u) / 8u;
    size_t bpp = ((size_t)f->ch * (size_t)f->depth + 7u) / 8u;
    uint8_t *cur = (uint8_t *)malloc(rowmax + 1u), *prev = (uint8_t *)malloc(rowmax + 1u);
    uint8_t *flt = (uint8_t *)malloc(rowmax + 1u);
    memset(&raw, 0, sizeof raw);
    memset(&z, 0, sizeof z);
    for (int pass = 0; pass < (f->il ? 7 : 1); pass++) {
        uint32_t x0 = f->il ? ax0[pass] : 0, y0 = f->il ? ay0[pass] : 0;
        uint32_t dx = f->il ? adx[pass] : 1, dy = f->il ? ady[pass] : 1;
        uint32_t pw = f->w > x0 ? (f->w - x0 + dx - 1u) / dx : 0u;
        bool have_prev = false;
        if (pw == 0u || f->h <= y0) continue;
        for (uint32_t y = y0; y < f->h; y += dy) {
            size_t nb = pack_row(f, y, x0, dx, pw, cur);
            int type = (int)rndu(5);
            filter_row(flt, cur, have_prev ? prev : NULL, nb, bpp, type);
            pc_buf_put_u8(&raw, (uint8_t)type);
            pc_buf_append(&raw, flt, nb);
            memcpy(prev, cur, nb);
            have_prev = true;
        }
    }
    tu_png_sig(out);
    tu_png_ihdr(out, f->w, f->h, f->depth, f->ct, f->il);
    if (with_meta) {
        uint8_t g[4] = { 0, 0, 0x27, 0x10 };                 /* gAMA 0.1: must be ignored */
        uint8_t phys[9] = { 0, 0, 0x0E, 0xC4, 0, 0, 0x0B, 0xB8, 1 };   /* 3780 x 3000 ppm */
        tu_png_chunk(out, "gAMA", g, 4);
        tu_png_chunk(out, "pHYs", phys, 9);
        if (icc) {
            pc_buf ic;
            memset(&ic, 0, sizeof ic);
            pc_buf_append(&ic, "test icc", 9);               /* name + NUL */
            pc_buf_put_u8(&ic, 0);                           /* compression method */
            tu_zlib_stored(&ic, icc, icc_len);
            tu_png_chunk(out, "iCCP", ic.p, ic.n);
            pc_buf_free(&ic);
        }
    }
    if (f->ct == 3) {
        uint8_t pl[768];
        for (int i = 0; i < f->n_plte; i++) memcpy(pl + 3 * i, f->plte[i], 3);
        tu_png_chunk(out, "PLTE", pl, (size_t)f->n_plte * 3u);
    }
    if (f->trns) {
        uint8_t t[256];
        size_t tn = 0;
        if (f->ct == 3) {
            memcpy(t, f->trns_pal, (size_t)f->n_trns_pal);
            tn = (size_t)f->n_trns_pal;
        }
        else {
            for (int c = 0; c < (f->ct == 0 ? 1 : 3); c++) {
                t[tn++] = (uint8_t)(f->trns_key[c] >> 8);
                t[tn++] = (uint8_t)f->trns_key[c];
            }
        }
        tu_png_chunk(out, "tRNS", t, tn);
    }
    /* split IDAT in two chunks to exercise chunk boundaries */
    tu_zlib_stored(&z, raw.p, raw.n);
    tu_png_chunk(out, "IDAT", z.p, z.n / 2u);
    tu_png_chunk(out, "IDAT", z.p + z.n / 2u, z.n - z.n / 2u);
    tu_png_chunk(out, "IEND", NULL, 0);
    pc_buf_free(&raw);
    pc_buf_free(&z);
    free(cur); free(prev); free(flt);
}

static uint8_t scale_to8(uint16_t v, int depth)
{
    if (depth == 16) return (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
    return (uint8_t)((uint32_t)v * 255u / ((1u << depth) - 1u));
}

static pc_px32 expect_px(const fx_png *f, uint32_t x, uint32_t y)
{
    const uint16_t *s = f->s + ((size_t)y * f->w + x) * (size_t)f->ch;
    pc_px32 p;
    switch (f->ct) {
    case 0:
        p.r = p.g = p.b = scale_to8(s[0], f->depth);
        p.a = (f->trns && s[0] == f->trns_key[0]) ? 0u : 255u;
        break;
    case 2:
        p.r = scale_to8(s[0], f->depth); p.g = scale_to8(s[1], f->depth);
        p.b = scale_to8(s[2], f->depth);
        p.a = (f->trns && s[0] == f->trns_key[0] && s[1] == f->trns_key[1] &&
               s[2] == f->trns_key[2]) ? 0u : 255u;
        break;
    case 3:
        p.r = f->plte[s[0]][0]; p.g = f->plte[s[0]][1]; p.b = f->plte[s[0]][2];
        p.a = (f->trns && s[0] < f->n_trns_pal) ? f->trns_pal[s[0]] : 255u;
        break;
    case 4:
        p.r = p.g = p.b = scale_to8(s[0], f->depth);
        p.a = scale_to8(s[1], f->depth);
        break;
    default:
        p.r = scale_to8(s[0], f->depth); p.g = scale_to8(s[1], f->depth);
        p.b = scale_to8(s[2], f->depth); p.a = scale_to8(s[3], f->depth);
        break;
    }
    return p;
}

static void make_fixture(fx_png *f, uint32_t w, uint32_t h, int depth, int ct, int il,
                         bool trns)
{
    uint32_t maxv = depth == 16 ? 65535u : ((1u << depth) - 1u);
    memset(f, 0, sizeof *f);
    f->w = w; f->h = h; f->depth = depth; f->ct = ct; f->il = il;
    f->ch = ct_channels(ct);
    f->s = (uint16_t *)malloc((size_t)w * h * (size_t)f->ch * sizeof *f->s);
    if (ct == 3) {
        f->n_plte = 1 + (int)rndu(maxv + 1u);
        for (int i = 0; i < f->n_plte; i++)
            for (int c = 0; c < 3; c++) f->plte[i][c] = rnd8();
        maxv = (uint32_t)f->n_plte - 1u;
    }
    for (size_t i = 0; i < (size_t)w * h * (size_t)f->ch; i++)
        f->s[i] = (uint16_t)(depth == 16 ? (rnd() >> 48) : rndu(maxv + 1u));
    if (trns && ct != 4 && ct != 6) {
        f->trns = 1;
        if (ct == 3) {
            f->n_trns_pal = 1 + (int)rndu((uint32_t)f->n_plte);
            for (int i = 0; i < f->n_trns_pal; i++) f->trns_pal[i] = rnd8();
        } else {
            /* use the value of pixel 0 as the key so the key really occurs */
            for (int c = 0; c < (ct == 0 ? 1 : 3); c++) f->trns_key[c] = f->s[c];
        }
    }
}

static void check_decode(const fx_png *f, const uint8_t *data, size_t n)
{
    pc_doc *d = NULL;
    pc_image_meta meta;
    pc_status st = png()->load(data, n, NULL, &d, &meta);
    CHECK(st == PC_OK);
    if (st != PC_OK) {
        INFO("decode failed ct=%d depth=%d il=%d %ux%u: %s", f->ct, f->depth, f->il, f->w, f->h,
             pc_status_str(st));
        return;
    }
    CHECK(d->w == f->w && d->h == f->h && d->n_layers == 1u);
    CHECK(strcmp(d->stack[0]->name, "Background") == 0);
    {
        pc_px32 *px = tu_layer_px(d, d->stack[0]);
        size_t bad = 0;
        for (uint32_t y = 0; y < f->h; y++)
            for (uint32_t x = 0; x < f->w; x++)
                if (!tu_px_eq(px[(size_t)y * f->w + x], expect_px(f, x, y))) bad++;
        CHECK(bad == 0);
        if (bad) INFO("ct=%d depth=%d il=%d trns=%d %ux%u: %zu bad pixels", f->ct, f->depth,
                      f->il, f->trns, f->w, f->h, bad);
        free(px);
    }
    CHECK(meta.src_bits == (uint32_t)f->depth);
    CHECK(meta.had_alpha == (f->ct == 4 || f->ct == 6 || f->trns));
    CHECK(pc_doc_edge_padding_is_zero(d));
    pc_doc_destroy(d);
    pc_meta_free(&meta);
}

static void t_sniff(void)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static const uint8_t jpg[4] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    CHECK(png() != NULL);
    CHECK(png()->sniff(sig, 8));
    CHECK(!png()->sniff(sig, 7));
    CHECK(!png()->sniff(jpg, 4));
    CHECK(!png()->sniff(NULL, 0));
    CHECK(pc_codec_sniff(sig, 8) == png());
    CHECK(pc_codec_by_ext("PNG") == png());
    CHECK((png()->flags & (PC_CODEC_LOAD | PC_CODEC_SAVE)) == (PC_CODEC_LOAD | PC_CODEC_SAVE));
}

static void t_decode_matrix(void)
{
    static const int combos[][2] = {   /* color type, depth */
        { 0, 1 }, { 0, 2 }, { 0, 4 }, { 0, 8 }, { 0, 16 },
        { 2, 8 }, { 2, 16 },
        { 3, 1 }, { 3, 2 }, { 3, 4 }, { 3, 8 },
        { 4, 8 }, { 4, 16 },
        { 6, 8 }, { 6, 16 },
    };
    static const uint32_t sizes[][2] = { { 1, 1 }, { 7, 5 }, { 33, 17 }, { 70, 130 }, { 9, 1 } };
    for (size_t c = 0; c < sizeof combos / sizeof combos[0]; c++)
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
            for (int il = 0; il < 2; il++)
                for (int trns = 0; trns < 2; trns++) {
                    fx_png f;
                    pc_buf b;
                    if (trns && (combos[c][0] == 4 || combos[c][0] == 6)) continue;
                    memset(&b, 0, sizeof b);
                    make_fixture(&f, sizes[s][0], sizes[s][1], combos[c][1], combos[c][0], il,
                                 trns != 0);
                    build_png(&f, &b, false, NULL, 0);
                    check_decode(&f, b.p, b.n);
                    pc_buf_free(&b);
                    free(f.s);
                }
}

static void t_decode_meta(void)
{
    fx_png f;
    pc_buf b;
    uint8_t icc[300];
    pc_doc *d = NULL;
    pc_image_meta meta;
    for (int i = 0; i < 300; i++) icc[i] = (uint8_t)(i * 7);
    memset(&b, 0, sizeof b);
    make_fixture(&f, 20, 10, 8, 2, 0, false);
    build_png(&f, &b, true, icc, sizeof icc);
    CHECK(png()->load(b.p, b.n, NULL, &d, &meta) == PC_OK);
    CHECK(d != NULL);
    CHECK(fabs(meta.dpi_x - 96.012) < 1e-6 && fabs(meta.dpi_y - 76.2) < 1e-6);
    CHECK(meta.icc_len == sizeof icc && meta.icc && memcmp(meta.icc, icc, sizeof icc) == 0);
    if (d) {   /* gAMA must not change pixels */
        pc_px32 *px = tu_layer_px(d, d->stack[0]);
        CHECK(tu_px_eq(px[5], expect_px(&f, 5, 0)));
        free(px);
    }
    pc_doc_destroy(d);
    pc_meta_free(&meta);
    pc_buf_free(&b);
    free(f.s);
}

static void t_decode_errors(void)
{
    fx_png f;
    pc_buf b;
    pc_doc *d = (pc_doc *)1;
    pc_image_meta meta;
    pc_codec_limits lim;
    memset(&b, 0, sizeof b);
    make_fixture(&f, 100, 80, 8, 6, 0, false);
    build_png(&f, &b, true, (const uint8_t *)"abcdefgh", 8);
    /* dimension limit */
    pc_codec_limits_default(&lim);
    lim.max_w = 99;
    CHECK(png()->load(b.p, b.n, &lim, &d, &meta) == PC_ERR_LIMIT && d == NULL);
    CHECK(meta.icc == NULL);
    /* memory budget smaller than the image */
    pc_codec_limits_default(&lim);
    lim.max_mem = 100u * 80u * 4u - 1u;
    CHECK(png()->load(b.p, b.n, &lim, &d, &meta) == PC_ERR_LIMIT && d == NULL);
    /* truncated data */
    CHECK(png()->load(b.p, b.n / 2u, NULL, &d, &meta) != PC_OK && d == NULL);
    CHECK(meta.icc == NULL);
    /* corrupted critical chunk CRC (last byte of IHDR CRC) */
    b.p[8 + 8 + 13 + 3] ^= 0x55u;
    CHECK(png()->load(b.p, b.n, NULL, &d, &meta) == PC_ERR_FORMAT && d == NULL);
    pc_buf_free(&b);
    free(f.s);
    /* huge declared size with no data: rejected before allocating */
    memset(&b, 0, sizeof b);
    tu_png_sig(&b);
    tu_png_ihdr(&b, 65535, 65535, 8, 6, 0);
    tu_png_chunk(&b, "IEND", NULL, 0);
    CHECK(png()->load(b.p, b.n, NULL, &d, &meta) == PC_ERR_LIMIT && d == NULL);
    pc_buf_free(&b);
    memset(&b, 0, sizeof b);
    tu_png_sig(&b);
    tu_png_ihdr(&b, 70000, 1, 8, 6, 0);
    tu_png_chunk(&b, "IEND", NULL, 0);
    CHECK(png()->load(b.p, b.n, NULL, &d, &meta) == PC_ERR_LIMIT && d == NULL);
    pc_buf_free(&b);
    CHECK(png()->load(NULL, 0, NULL, &d, &meta) == PC_ERR_ARG);
}

/* ---- saving -------------------------------------------------------------------- */
typedef struct png_params_t {
    int32_t bit_depth, dither, threshold, palette, interlace;
} png_params_t;

static pc_doc *reload(const pc_buf *b, pc_image_meta *meta)
{
    pc_doc *d = NULL;
    pc_status st = png()->load(b->p, b->n, NULL, &d, meta);
    CHECK(st == PC_OK);
    return d;
}

static int ihdr_color_type(const pc_buf *b) { return b->n > 25 ? b->p[25] : -1; }

static void t_params(void)
{
    png_params_t p;
    CHECK(png()->params_size == sizeof p);
    CHECK(png()->n_props == 5u);
    pc_codec_default_params(png(), &p);
    CHECK(p.bit_depth == 0 && p.dither == 7 && p.threshold == 128);
    CHECK(p.palette == 0 && p.interlace == 0);          /* Octree, not interlaced */
}

static void t_save_modes(void)
{
    const uint32_t W = 77, H = 70;
    pc_px32 *a = tu_noise(W, H, 1), *b = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    pc_px32 *flat, *white;
    pc_image_meta meta;
    png_params_t p;
    tu_doc_add_layer(d, b, PC_BLEND_MULTIPLY, 200, true, "top");
    flat = tu_flatten(d);
    white = (pc_px32 *)malloc((size_t)W * H * sizeof *white);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        pc_px32 w = tu_px(255, 255, 255, 255);
        pc_composite_span(&w, &flat[i], 1, PC_BLEND_NORMAL, 255);
        white[i] = w;
    }
    memset(&meta, 0, sizeof meta);
    meta.dpi_x = 300.0; meta.dpi_y = 150.0;
    for (int depth = 0; depth < 3; depth++) {
        pc_buf out;
        pc_doc *r;
        pc_image_meta m2;
        memset(&out, 0, sizeof out);
        pc_codec_default_params(png(), &p);
        p.bit_depth = depth;          /* auto, 32, 24 */
        CHECK(png()->save(d, &meta, &p, NULL, &out) == PC_OK);
        r = reload(&out, &m2);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK(tu_diff(px, depth == 2 ? white : flat, (size_t)W * H) == 0);
            CHECK(fabs(m2.dpi_x - 300.0) < 0.02 && fabs(m2.dpi_y - 150.0) < 0.02);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m2);
        }
        CHECK(ihdr_color_type(&out) == (depth == 2 ? 2 : 6));
        pc_buf_free(&out);
    }
    free(a); free(b); free(flat); free(white);
    pc_doc_destroy(d);
}

static void t_save_palette(void)
{
    const uint32_t W = 66, H = 67;
    pc_px32 pal[40], *a = (pc_px32 *)malloc((size_t)W * H * sizeof *a);
    png_params_t p;
    for (int i = 0; i < 40; i++) pal[i] = tu_px(rnd8(), rnd8(), rnd8(), 255);
    for (size_t i = 0; i < (size_t)W * H; i++) a[i] = pal[rndu(40)];
    /* opaque, 40 colors: Auto picks the (smaller) palette encoding, exact */
    {
        pc_doc *d = tu_doc_from_px(W, H, a), *r;
        pc_buf out;
        pc_image_meta m;
        memset(&out, 0, sizeof out);
        pc_codec_default_params(png(), &p);
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 3);
        r = reload(&out, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK(tu_diff(px, a, (size_t)W * H) == 0);
            CHECK(fabs(m.dpi_x - 96.0) < 0.02);          /* default resolution */
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
        /* explicit 8-bit: same result */
        p.bit_depth = 3;
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 3);
        pc_buf_free(&out);
        pc_doc_destroy(d);
    }
    /* binary transparency, 8-bit with threshold: pixels below the threshold
     * become transparent, the rest composite onto white */
    {
        pc_doc *d, *r;
        pc_buf out;
        pc_image_meta m;
        for (size_t i = 0; i < (size_t)W * H; i++) {
            a[i] = pal[rndu(20)];
            a[i].a = (uint8_t)(rndu(3) == 0 ? 0 : (rndu(2) ? 255 : 100));
            if (a[i].a == 0) a[i].r = a[i].g = a[i].b = 0;
        }
        d = tu_doc_from_px(W, H, a);
        memset(&out, 0, sizeof out);
        pc_codec_default_params(png(), &p);
        p.bit_depth = 3;
        p.threshold = 128;
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 3);
        r = reload(&out, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            size_t bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                pc_px32 e = a[i];
                if (e.a < 128) e = tu_px(0, 0, 0, 0);
                else {
                    pc_px32 w = tu_px(255, 255, 255, 255);
                    pc_composite_span(&w, &e, 1, PC_BLEND_NORMAL, 255);
                    e = w;
                }
                if (!tu_px_eq(px[i], e)) bad++;
            }
            CHECK(bad == 0);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
        /* Auto with alpha 100 present: not 0/255 only, so 32-bit exact */
        p.bit_depth = 0;
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 6);
        pc_buf_free(&out);
        pc_doc_destroy(d);
    }
    /* 0/255 alpha with < 256 colors: Auto writes a palette with tRNS, exact */
    {
        pc_doc *d, *r;
        pc_buf out;
        pc_image_meta m;
        for (size_t i = 0; i < (size_t)W * H; i++) {
            a[i] = pal[rndu(30)];
            if (rndu(4) == 0) a[i] = tu_px(0, 0, 0, 0);
        }
        d = tu_doc_from_px(W, H, a);
        memset(&out, 0, sizeof out);
        pc_codec_default_params(png(), &p);
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 3);
        r = reload(&out, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK(tu_diff(px, a, (size_t)W * H) == 0);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
        pc_doc_destroy(d);
    }
    /* more than 256 colors in 8-bit: octree quantizer + dithering (quant.h) */
    {
        pc_px32 *n = tu_noise(W, H, 0);
        pc_doc *d = tu_doc_from_px(W, H, n);
        pc_buf out;
        memset(&out, 0, sizeof out);
        pc_codec_default_params(png(), &p);
        p.bit_depth = 3;
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 3);                 /* palette */
        {
            pc_image_meta m;
            pc_doc *r = reload(&out, &m);
            CHECK(r != NULL);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                uint32_t distinct = 0;
                double err = 0;
                for (uint32_t i = 0; i < W * H; i++) {
                    bool seen = false;
                    for (uint32_t k = 0; k < i && !seen && distinct <= 256u; k++)
                        seen = memcmp(&px[k], &px[i], 4) == 0;
                    if (!seen) distinct++;
                    err += abs(px[i].r - n[i].r) + abs(px[i].g - n[i].g) + abs(px[i].b - n[i].b);
                }
                CHECK(distinct <= 256u);
                INFO("8-bit noise: %u colors, mean abs channel error %.2f", distinct,
                     err / (3.0 * W * H));
                CHECK(err / (3.0 * W * H) < 40.0);       /* uniform noise is the worst case */
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        pc_buf_free(&out);
        /* Auto on opaque noise: 24 or 32-bit, lossless */
        p.bit_depth = 0;
        CHECK(png()->save(d, NULL, &p, NULL, &out) == PC_OK);
        CHECK(ihdr_color_type(&out) == 2 || ihdr_color_type(&out) == 6);
        {
            pc_image_meta m;
            pc_doc *r = reload(&out, &m);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                CHECK(tu_diff(px, n, (size_t)W * H) == 0);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        pc_buf_free(&out);
        pc_doc_destroy(d);
        free(n);
    }
    free(a);
}

static void t_save_icc_and_big(void)
{
    /* ICC passes through; sizes crossing many tiles; NULL params = defaults */
    const uint32_t W = 300, H = 129;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    pc_image_meta meta, m2;
    pc_buf out;
    uint8_t icc[1000];
    for (int i = 0; i < 1000; i++) icc[i] = (uint8_t)rnd8();
    memset(&meta, 0, sizeof meta);
    meta.icc = icc;
    meta.icc_len = sizeof icc;
    memset(&out, 0, sizeof out);
    CHECK(png()->save(d, &meta, NULL, NULL, &out) == PC_OK);
    r = reload(&out, &m2);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        CHECK(tu_diff(px, a, (size_t)W * H) == 0);
        CHECK(m2.icc_len == sizeof icc && memcmp(m2.icc, icc, sizeof icc) == 0);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m2);
    }
    pc_buf_free(&out);
    pc_doc_destroy(d);
    free(a);
}

/* Fixtures encoded by libspng itself: 16-bit RGBA and 8-bit gray + alpha. */
static void t_spng_fixtures(void)
{
    for (int k = 0; k < 2; k++) {
        const uint32_t W = 19, H = 11;
        struct spng_ihdr ih;
        spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
        uint16_t img16[19 * 11 * 4];
        uint8_t img8[19 * 11 * 2];
        size_t len = 0;
        int err = 0;
        void *png_buf;
        pc_doc *d = NULL;
        pc_image_meta m;
        memset(&ih, 0, sizeof ih);
        ih.width = W; ih.height = H;
        ih.bit_depth = (uint8_t)(k == 0 ? 16 : 8);
        ih.color_type = (uint8_t)(k == 0 ? SPNG_COLOR_TYPE_TRUECOLOR_ALPHA
                                         : SPNG_COLOR_TYPE_GRAYSCALE_ALPHA);
        for (size_t i = 0; i < sizeof img16 / 2; i++) img16[i] = (uint16_t)(rnd() >> 48);
        for (size_t i = 0; i < sizeof img8; i++) img8[i] = rnd8();
        /* SPNG_FMT_PNG takes 16-bit samples in host byte order */
        CHECK(spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1) == 0);
        CHECK(spng_set_ihdr(ctx, &ih) == 0);
        CHECK(spng_encode_image(ctx, k == 0 ? (const void *)img16 : (const void *)img8,
                                k == 0 ? sizeof img16 : sizeof img8, SPNG_FMT_PNG,
                                SPNG_ENCODE_FINALIZE) == 0);
        png_buf = spng_get_png_buffer(ctx, &len, &err);
        CHECK(png_buf != NULL && err == 0);
        spng_ctx_free(ctx);
        if (!png_buf) continue;
        CHECK(png()->load((const uint8_t *)png_buf, len, NULL, &d, &m) == PC_OK);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            size_t bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                pc_px32 e;
                if (k == 0) {
                    const uint16_t *c = img16 + 4 * i;
                    e = tu_px(scale_to8(c[0], 16), scale_to8(c[1], 16), scale_to8(c[2], 16),
                              scale_to8(c[3], 16));
                } else {
                    e = tu_px(img8[2 * i], img8[2 * i], img8[2 * i], img8[2 * i + 1]);
                }
                if (!tu_px_eq(px[i], e)) bad++;
            }
            CHECK(bad == 0);
            CHECK(m.src_bits == (k == 0 ? 16u : 8u) && m.had_alpha);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        free(png_buf);
    }
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 1500u : 20000u, ok = 0;
    int kinds[4][3] = { { 6, 8, 0 }, { 3, 4, 1 }, { 0, 16, 0 }, { 2, 8, 1 } };
    for (int k = 0; k < 4; k++) {
        fx_png f;
        pc_buf b;
        memset(&b, 0, sizeof b);
        make_fixture(&f, 37, 21, kinds[k][1], kinds[k][0], kinds[k][2], kinds[k][0] != 6);
        build_png(&f, &b, true, (const uint8_t *)"0123456789abcdef", 16);
        ok += tu_fuzz_codec(png(), b.p, b.n, iters, NULL);
        ok += tu_fuzz_codec(png(), b.p, b.n, iters, tu_png_fix_crcs);
        pc_buf_free(&b);
        free(f.s);
    }
    /* compressed IDAT from our own encoder (Huffman paths, filters) */
    {
        pc_px32 *a = tu_photo(45, 33, true);
        pc_doc *d = tu_doc_from_px(45, 33, a);
        pc_buf b;
        memset(&b, 0, sizeof b);
        CHECK(png()->save(d, NULL, NULL, NULL, &b) == PC_OK);
        ok += tu_fuzz_codec(png(), b.p, b.n, 2u * iters, tu_png_fix_crcs);
        pc_buf_free(&b);
        pc_doc_destroy(d);
        free(a);
    }
    INFO("png fuzz: %u of %u mutated files decoded", ok, 10u * iters);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_sniff);
    RUN(t_params);
    RUN(t_decode_matrix);
    RUN(t_decode_meta);
    RUN(t_spng_fixtures);
    RUN(t_decode_errors);
    RUN(t_save_modes);
    RUN(t_save_palette);
    RUN(t_save_icc_and_big);
    RUN(t_fuzz);
    return pc_test_finish();
}
