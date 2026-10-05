/* test_png_options.c - PNG save options added by lane CODEC (OBSERVED 3.3,
 * FS-AUTO, FS-QUANT): 4, 2 and 1-bit palettes (exact and quantized, with
 * and without a transparent entry), Auto-detect choosing sub-byte depths
 * when they are smaller and lossless, the Octree / Median Cut choice,
 * dithering, and Adam7 interlacing for every pixel layout. */
#include "pc_test.h"
#include "lib_test_util.h"

static const pc_codec *png(void) { return pc_codec_by_id("png"); }

typedef struct png_params_t {
    int32_t bit_depth, dither, threshold, palette, interlace;
} png_params_t;

enum { D_AUTO = 0, D_32, D_24, D_8, D_4, D_2, D_1 };

static int ihdr_depth(const pc_buf *b) { return b->n > 24 ? b->p[24] : -1; }
static int ihdr_type(const pc_buf *b) { return b->n > 25 ? b->p[25] : -1; }
static int ihdr_interlace(const pc_buf *b) { return b->n > 28 ? b->p[28] : -1; }

static pc_px32 *reload_px(const pc_buf *b, uint32_t w, uint32_t h)
{
    pc_doc *d = NULL;
    pc_image_meta m;
    pc_px32 *px = NULL;
    CHECK(png()->load(b->p, b->n, NULL, &d, &m) == PC_OK);
    if (d) {
        CHECK(d->w == w && d->h == h);
        px = tu_layer_px(d, d->stack[0]);
        pc_doc_destroy(d);
        pc_meta_free(&m);
    }
    return px;
}

static pc_status save_with(const pc_doc *d, int depth, int algo, int dither, int interlace,
                           pc_buf *out)
{
    png_params_t p;
    pc_codec_default_params(png(), &p);
    p.bit_depth = depth;
    p.palette = algo;
    p.dither = dither;
    p.interlace = interlace;
    memset(out, 0, sizeof *out);
    return png()->save(d, NULL, &p, NULL, out);
}

static uint32_t distinct(const pc_px32 *px, size_t n)
{
    uint32_t k = 0;
    pc_px32 seen[300];
    for (size_t i = 0; i < n && k < 300u; i++) {
        bool f = false;
        for (uint32_t j = 0; j < k && !f; j++) f = tu_px_eq(seen[j], px[i]);
        if (!f) seen[k++] = px[i];
    }
    return k;
}

/* Images with exactly n opaque colors. */
static pc_px32 *few_colors(uint32_t w, uint32_t h, uint32_t n)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t c = (uint32_t)((i * 7u + i / w) % n);
        px[i] = tu_px((uint8_t)(c * 37u), (uint8_t)(255u - c * 11u), (uint8_t)(c * 5u), 255);
    }
    return px;
}

static void t_exact_subbyte(void)
{
    static const struct { int depth; uint32_t colors; int bits; } cases[] = {
        { D_1, 2, 1 }, { D_2, 4, 2 }, { D_2, 3, 2 }, { D_4, 16, 4 }, { D_4, 9, 4 }, { D_8, 5, 8 },
    };
    const uint32_t W = 37, H = 21;     /* odd width: partial bytes at row ends */
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        pc_px32 *a = few_colors(W, H, cases[k].colors), *r;
        pc_doc *d = tu_doc_from_px(W, H, a);
        pc_buf out;
        CHECK(save_with(d, cases[k].depth, 0, 7, 0, &out) == PC_OK);
        CHECK(ihdr_type(&out) == 3 && ihdr_depth(&out) == cases[k].bits);
        r = reload_px(&out, W, H);
        CHECK(r && tu_diff(r, a, (size_t)W * H) == 0);          /* exact: not dithered */
        free(r);
        pc_buf_free(&out);
        pc_doc_destroy(d);
        free(a);
    }
}

static void t_quantized_subbyte(void)
{
    const uint32_t W = 64, H = 48;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a);
    for (int depth = D_8; depth <= D_1; depth++) {
        uint32_t cap = 1u << (depth == D_8 ? 8 : depth == D_4 ? 4 : depth == D_2 ? 2 : 1);
        pc_buf oct, med, nod;
        pc_px32 *ro, *rm, *rn;
        CHECK(save_with(d, depth, 0, 7, 0, &oct) == PC_OK);
        CHECK(save_with(d, depth, 1, 7, 0, &med) == PC_OK);
        CHECK(save_with(d, depth, 0, 0, 0, &nod) == PC_OK);
        CHECK(ihdr_type(&oct) == 3);
        CHECK(ihdr_depth(&oct) == (depth == D_8 ? 8 : depth == D_4 ? 4 : depth == D_2 ? 2 : 1));
        ro = reload_px(&oct, W, H);
        rm = reload_px(&med, W, H);
        rn = reload_px(&nod, W, H);
        if (ro && rm && rn) {
            CHECK(distinct(ro, (size_t)W * H) <= cap && distinct(rm, (size_t)W * H) <= cap);
            CHECK(tu_diff(ro, rm, (size_t)W * H) > 0);           /* the algorithm matters */
            CHECK(tu_diff(ro, rn, (size_t)W * H) > 0);           /* and so does dithering */
            INFO("%u colors: octree %.1f dB, median cut %.1f dB, undithered %.1f dB", cap,
                 tu_psnr(ro, a, (size_t)W * H), tu_psnr(rm, a, (size_t)W * H),
                 tu_psnr(rn, a, (size_t)W * H));
            CHECK(tu_psnr(ro, a, (size_t)W * H) >
                  (cap == 256u ? 28.0 : cap == 16u ? 17.0 : cap == 4u ? 12.0 : 9.0));
        }
        free(ro); free(rm); free(rn);
        pc_buf_free(&oct); pc_buf_free(&med); pc_buf_free(&nod);
    }
    pc_doc_destroy(d);
    free(a);
}

/* 1-bit with transparency: one color plus the transparent entry. */
static void t_transparent_1bit(void)
{
    const uint32_t W = 19, H = 9;
    pc_px32 *a = (pc_px32 *)malloc((size_t)W * H * sizeof *a), *r;
    pc_doc *d;
    pc_buf out;
    for (size_t i = 0; i < (size_t)W * H; i++)
        a[i] = (i % 3u) ? tu_px(10, 200, 30, 255) : tu_px(0, 0, 0, (uint8_t)((i % 2u) ? 60 : 0));
    d = tu_doc_from_px(W, H, a);
    CHECK(save_with(d, D_1, 0, 0, 0, &out) == PC_OK);
    CHECK(ihdr_depth(&out) == 1 && ihdr_type(&out) == 3);
    r = reload_px(&out, W, H);
    if (r) {
        size_t bad = 0;
        for (size_t i = 0; i < (size_t)W * H; i++) {
            pc_px32 e = a[i].a < 128 ? tu_px(0, 0, 0, 0) : a[i];   /* threshold 128 */
            if (!tu_px_eq(r[i], e)) bad++;
        }
        CHECK(bad == 0);
    }
    free(r);
    pc_buf_free(&out);
    pc_doc_destroy(d);
    free(a);
}

/* Auto-detect is lossless and never larger than the explicit candidates. */
static void t_auto(void)
{
    static const uint32_t colors[] = { 2, 3, 4, 11, 16, 17, 200 };
    const uint32_t W = 120, H = 40;
    for (size_t k = 0; k < sizeof colors / sizeof colors[0]; k++) {
        pc_px32 *a = few_colors(W, H, colors[k]), *r;
        pc_doc *d = tu_doc_from_px(W, H, a);
        pc_buf au, c8, cs;
        int sub = colors[k] <= 2u ? D_1 : colors[k] <= 4u ? D_2 : colors[k] <= 16u ? D_4 : D_8;
        CHECK(save_with(d, D_AUTO, 0, 7, 0, &au) == PC_OK);
        CHECK(save_with(d, D_8, 0, 7, 0, &c8) == PC_OK);
        CHECK(save_with(d, sub, 0, 7, 0, &cs) == PC_OK);
        r = reload_px(&au, W, H);
        CHECK(r && tu_diff(r, a, (size_t)W * H) == 0);
        CHECK(au.n <= c8.n && au.n <= cs.n);
        if (colors[k] <= 16u) CHECK(ihdr_type(&au) == 3);     /* RGB may win with more */
        if (cs.n < c8.n) CHECK(ihdr_depth(&au) == ihdr_depth(&cs));
        INFO("%3u colors: auto %zu bytes at %d bits (8-bit %zu, %d-bit %zu)", colors[k], au.n,
             ihdr_depth(&au), c8.n, ihdr_depth(&cs), cs.n);
        free(r);
        pc_buf_free(&au); pc_buf_free(&c8); pc_buf_free(&cs);
        pc_doc_destroy(d);
        free(a);
    }
    {   /* two colors plus fully transparent pixels: a 2-bit palette with tRNS */
        pc_px32 *a = few_colors(W, H, 2), *r;
        pc_doc *d;
        pc_buf au;
        for (size_t i = 0; i < (size_t)W * H; i += 5) a[i] = tu_px(0, 0, 0, 0);
        d = tu_doc_from_px(W, H, a);
        CHECK(save_with(d, D_AUTO, 0, 7, 0, &au) == PC_OK);
        CHECK(ihdr_type(&au) == 3 && ihdr_depth(&au) <= 2);
        r = reload_px(&au, W, H);
        CHECK(r && tu_diff(r, a, (size_t)W * H) == 0);
        free(r);
        pc_buf_free(&au);
        pc_doc_destroy(d);
        free(a);
    }
}

static void t_interlace(void)
{
    const uint32_t W = 300, H = 129;    /* crosses tile bands and odd Adam7 passes */
    pc_px32 *photo = tu_photo(W, H, true), *pal = few_colors(W, H, 3);
    const pc_px32 *srcs[2] = { photo, pal };
    for (int s = 0; s < 2; s++) {
        pc_doc *d = tu_doc_from_px(W, H, srcs[s]);
        for (int depth = D_AUTO; depth <= D_1; depth++) {
            pc_buf a, b;
            pc_px32 *ra, *rb;
            if (s == 0 && depth >= D_8) continue;       /* quantized: covered above */
            CHECK(save_with(d, depth, 0, 7, 0, &a) == PC_OK);
            CHECK(save_with(d, depth, 0, 7, 1, &b) == PC_OK);
            CHECK(ihdr_interlace(&a) == 0 && ihdr_interlace(&b) == 1);
            CHECK(ihdr_depth(&a) == ihdr_depth(&b) && ihdr_type(&a) == ihdr_type(&b));
            ra = reload_px(&a, W, H);
            rb = reload_px(&b, W, H);
            CHECK(ra && rb && tu_diff(ra, rb, (size_t)W * H) == 0);
            free(ra);
            free(rb);
            pc_buf_free(&a);
            pc_buf_free(&b);
        }
        pc_doc_destroy(d);
    }
    {   /* quantized and interlaced: same pixels as quantized progressive */
        pc_doc *d = tu_doc_from_px(W, H, photo);
        pc_buf a, b;
        pc_px32 *ra, *rb;
        CHECK(save_with(d, D_4, 1, 8, 0, &a) == PC_OK);
        CHECK(save_with(d, D_4, 1, 8, 1, &b) == PC_OK);
        ra = reload_px(&a, W, H);
        rb = reload_px(&b, W, H);
        CHECK(ra && rb && tu_diff(ra, rb, (size_t)W * H) == 0 && ihdr_interlace(&b) == 1);
        free(ra);
        free(rb);
        pc_buf_free(&a);
        pc_buf_free(&b);
        pc_doc_destroy(d);
    }
    free(photo);
    free(pal);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_exact_subbyte);
    RUN(t_quantized_subbyte);
    RUN(t_transparent_1bit);
    RUN(t_auto);
    RUN(t_interlace);
    return pc_test_finish();
}
