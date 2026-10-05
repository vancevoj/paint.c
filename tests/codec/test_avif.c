/* test_avif.c - AV1 Image File Format codec (fmt_avif.c over libavif, lane
 * AVIFJXL): registry and save options, lossless exact round trips (RGBA,
 * gray 4:0:0), lossy quality ordering, chroma subsampling and matrix in the
 * written file, encoder presets, lossless and premultiplied alpha, image
 * grids (automatic per preset, preserved layout), irot / imir / clap
 * transforms, image sequences (first frame), ICC, Exif and XMP, CICP to
 * ICC, 10/12-bit and HDR (PQ) input, limits and mutation fuzzing.
 * Fixtures with features paint.c never writes are made with libavif. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/avifjxl_meta.h"
#include "pc/pc_icc.h"

#include <math.h>

static const pc_codec *av(void) { return pc_codec_by_id("avif"); }

typedef struct avif_params_t {
    int32_t quality, lossless, lossless_alpha, preset, chroma, keep_tiles, premultiplied;
} avif_params_t;

static void t_registry(void)
{
    static const uint8_t ftyp_avif[24] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f',
                                           0, 0, 0, 0, 'm', 'i', 'f', '1', 'm', 'i', 'a', 'f' };
    static const uint8_t ftyp_compat[24] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'm', 'i', 'f', '1',
                                             0, 0, 0, 0, 'm', 'i', 'a', 'f', 'a', 'v', 'i', 's' };
    static const uint8_t ftyp_heic[24] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c',
                                           0, 0, 0, 0, 'm', 'i', 'f', '1', 'h', 'e', 'i', 'c' };
    avif_params_t p;
    const pc_codec *c = av();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(strcmp(c->name, "AV1 (AVIF)") == 0 && strcmp(c->exts, "avif") == 0);
    CHECK(pc_codec_by_ext("AVIF") == c && pc_codec_by_ext(".avif") == c);
    CHECK(c->sniff(ftyp_avif, 24) && c->sniff(ftyp_compat, 24) && !c->sniff(ftyp_heic, 24));
    CHECK(!c->sniff(ftyp_avif, 15));
    CHECK(c->n_props == 7u && c->params_size == sizeof p);
    pc_codec_default_params(c, &p);
    CHECK(p.quality == 85 && p.lossless == 0 && p.lossless_alpha == 1 && p.preset == 0 &&
          p.chroma == 1 && p.keep_tiles == 1 && p.premultiplied == 0);
    /* Quality, alpha compression, chroma and premultiplied are disabled while Lossless */
    CHECK(strcmp(c->props[0].key, "quality") == 0 &&
          strcmp(c->props[0].enabled_if, "lossless=0") == 0);
    CHECK(strcmp(c->props[2].enabled_if, "lossless=0") == 0);
    CHECK(strcmp(c->props[4].enabled_if, "lossless=0") == 0);
    CHECK(strcmp(c->props[6].enabled_if, "lossless=0") == 0);
    CHECK(c->props[1].enabled_if == NULL && c->props[3].enabled_if == NULL &&
          c->props[5].enabled_if == NULL);
    CHECK(strcmp(c->props[3].choices[3], "Very Slow") == 0 && c->props[3].choices[4] == NULL);
    CHECK(strcmp(c->props[4].choices[1], "4:2:2") == 0);
#if defined(PC_HAVE_AVIF)
    CHECK(c->flags == (PC_CODEC_LOAD | PC_CODEC_SAVE) && c->load && c->save);
    CHECK(pc_codec_sniff(ftyp_avif, 24) == c);
#else
    CHECK(c->flags == 0u && c->load == NULL && c->save == NULL);
    CHECK(pc_codec_sniff(ftyp_avif, 24) == NULL);
    INFO("libavif not available: AVIF load/save tests skipped");
#endif
}

#if defined(PC_HAVE_AVIF)
#include "avif/avif.h"
#include "lcms2.h"

/* Serial pc_par that reports 4 workers, to exercise maxThreads > 1. */
static void par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, i, i % 4u);
}
static const pc_par k_par4 = { par_run, NULL, 4u };

static avif_params_t defaults(void)
{
    avif_params_t p;
    pc_codec_default_params(av(), &p);
    return p;
}

static bool save_doc(const pc_doc *d, const pc_image_meta *m, const avif_params_t *p, pc_buf *out)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    st = av()->save(d, m, p, NULL, out);
    if (st != PC_OK) INFO("avif save: %s", pc_status_str(st));
    return st == PC_OK;
}

static pc_doc *load_ok(const uint8_t *p, size_t n, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_status st = av()->load(p, n, NULL, &d, m);
    CHECK(st == PC_OK);
    if (st != PC_OK) INFO("avif load: %s", pc_status_str(st));
    return d;
}

/* Decode p with libavif itself (container facts). Caller destroys. */
static avifImage *raw_decode(const uint8_t *p, size_t n)
{
    avifDecoder *dec = avifDecoderCreate();
    avifImage *im = avifImageCreateEmpty();
    avifResult r = avifDecoderReadMemory(dec, im, p, n);
    avifDecoderDestroy(dec);
    if (r != AVIF_RESULT_OK) { avifImageDestroy(im); return NULL; }
    return im;
}

static pc_px32 *doc_px(const pc_doc *d) { return tu_layer_px(d, d->stack[0]); }

static void t_lossless(void)
{
    const uint32_t W = 70, H = 50;
    pc_px32 *a = tu_noise(W, H, 1), *b = tu_photo(W, H, true), *flat;
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    avif_params_t p = defaults();
    pc_buf out;
    pc_image_meta m;
    tu_doc_add_layer(d, b, PC_BLEND_MULTIPLY, 200, true, "m");
    flat = tu_flatten(d);
    p.lossless = 1;
    p.lossless_alpha = 0;           /* ignored when lossless */
    p.chroma = 0;                   /* ignored: lossless is RGB 4:4:4 */
    p.premultiplied = 1;            /* ignored when lossless */
    for (int preset = 0; preset < 4; preset += 3) {
        p.preset = preset;
        if (!save_doc(d, NULL, &p, &out)) continue;
        r = load_ok(out.p, out.n, &m);
        if (r) {
            pc_px32 *px = doc_px(r);
            CHECK(r->w == W && r->h == H && r->n_layers == 1u);
            CHECK(tu_diff(px, flat, (size_t)W * H) == 0);
            CHECK(m.had_alpha && m.src_bits == 8u && m.icc == NULL);
            CHECK(pc_doc_edge_padding_is_zero(r));
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        {
            avifImage *im = raw_decode(out.p, out.n);
            CHECK(im && im->yuvFormat == AVIF_PIXEL_FORMAT_YUV444 &&
                  im->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_IDENTITY &&
                  im->alphaPlane != NULL && !im->alphaPremultiplied);
            if (im) avifImageDestroy(im);
        }
        pc_buf_free(&out);
    }
    free(flat);
    free(a);
    free(b);
    pc_doc_destroy(d);
    /* gray and opaque: 4:0:0, no alpha plane, still exact */
    {
        pc_px32 *g = tu_noise(33, 17, 0);
        for (size_t i = 0; i < 33u * 17u; i++) g[i].g = g[i].b = g[i].r;
        d = tu_doc_from_px(33, 17, g);
        if (save_doc(d, NULL, &p, &out)) {
            avifImage *im = raw_decode(out.p, out.n);
            CHECK(im && im->yuvFormat == AVIF_PIXEL_FORMAT_YUV400 && im->alphaPlane == NULL);
            if (im) avifImageDestroy(im);
            r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                CHECK(tu_diff(px, g, 33u * 17u) == 0);
                CHECK(!m.had_alpha);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
        free(g);
        pc_doc_destroy(d);
    }
}

static void t_lossy(void)
{
    const uint32_t W = 96, H = 64;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int qs[] = { 10, 50, 85, 100 };
    size_t prev_size = 0;
    double prev_psnr = 0.0;
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        avif_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        pc_doc *r;
        p.quality = qs[i];
        p.chroma = 2;
        if (!save_doc(d, NULL, &p, &out)) continue;
        r = load_ok(out.p, out.n, &m);
        if (r) {
            pc_px32 *px = doc_px(r);
            double ps = tu_psnr(px, a, (size_t)W * H);
            INFO("quality %3d: %6zu bytes, PSNR %.1f dB", qs[i], out.n, ps);
            CHECK(out.n > prev_size);
            CHECK(ps > prev_psnr);
            CHECK(ps > (qs[i] >= 85 ? 33.0 : 20.0));
            CHECK(!m.had_alpha);
            prev_size = out.n;
            prev_psnr = ps;
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
    }
    free(a);
    pc_doc_destroy(d);
}

static void t_chroma_presets(void)
{
    const uint32_t W = 64, H = 48;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const avifPixelFormat k_fmt[3] = { AVIF_PIXEL_FORMAT_YUV420, AVIF_PIXEL_FORMAT_YUV422,
                                              AVIF_PIXEL_FORMAT_YUV444 };
    for (int c = 0; c < 3; c++) {
        avif_params_t p = defaults();
        pc_buf out;
        p.chroma = c;
        if (save_doc(d, NULL, &p, &out)) {
            avifImage *im = raw_decode(out.p, out.n);
            CHECK(im && im->yuvFormat == k_fmt[c]);
            CHECK(im && im->colorPrimaries == AVIF_COLOR_PRIMARIES_BT709 &&
                  im->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_SRGB &&
                  im->yuvRange == AVIF_RANGE_FULL);
            if (im) avifImageDestroy(im);
            pc_buf_free(&out);
        }
    }
    /* every preset produces a decodable file; slower presets are not larger
     * by much (cpu-used 8 / 4 / 0 / 0) */
    for (int pr = 0; pr < 4; pr++) {
        avif_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        double t0 = pc_test_now();
        p.preset = pr;
        if (save_doc(d, NULL, &p, &out)) {
            pc_doc *r = load_ok(out.p, out.n, &m);
            INFO("preset %d: %zu bytes in %.2f s", pr, out.n, pc_test_now() - t0);
            if (r) {
                pc_px32 *px = doc_px(r);
                CHECK(tu_psnr(px, a, (size_t)W * H) > 30.0);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
    }
    /* thread count does not change validity */
    {
        avif_params_t p = defaults();
        pc_buf out;
        memset(&out, 0, sizeof out);
        CHECK(av()->save(d, NULL, &p, &k_par4, &out) == PC_OK);
        CHECK(out.n > 0u);
        pc_buf_free(&out);
    }
    free(a);
    pc_doc_destroy(d);
}

static void t_alpha_options(void)
{
    const uint32_t W = 64, H = 64;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    for (int mode = 0; mode < 3; mode++) {
        avif_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        pc_doc *r;
        p.quality = 70;
        p.lossless_alpha = mode == 0;
        p.premultiplied = mode == 2;
        if (!save_doc(d, NULL, &p, &out)) continue;
        {
            avifImage *im = raw_decode(out.p, out.n);
            CHECK(im && im->alphaPlane && (im->alphaPremultiplied != 0) == (mode == 2));
            if (im) avifImageDestroy(im);
        }
        r = load_ok(out.p, out.n, &m);
        if (r) {
            pc_px32 *px = doc_px(r);
            int amax = 0;
            size_t far = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                int da = abs((int)px[i].a - a[i].a);
                if (da > amax) amax = da;
                if (a[i].a > 128 && (abs((int)px[i].r - a[i].r) > 40 ||
                                     abs((int)px[i].g - a[i].g) > 40)) far++;
            }
            if (mode == 0) CHECK(amax == 0);           /* lossless alpha compression */
            else CHECK(amax <= 24);
            CHECK(far == 0u);
            CHECK(m.had_alpha);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
    }
    free(a);
    pc_doc_destroy(d);
}

static const char *grid_of(const pc_doc *d, const avif_params_t *p, const pc_image_meta *in,
                           char *buf, size_t cap)
{
    pc_buf out;
    pc_image_meta m;
    pc_doc *r;
    buf[0] = '\0';
    if (!save_doc(d, in, p, &out)) return "error";
    r = load_ok(out.p, out.n, &m);
    if (r) {
        const char *g = pc_meta_get(&m, "avif.grid");
        snprintf(buf, cap, "%s", g ? g : "none");
        CHECK(r->w == d->w && r->h == d->h);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    pc_buf_free(&out);
    return buf;
}

static void t_grid(void)
{
    char g[64];
    avif_params_t p = defaults();
    pc_image_meta keep;
    pc_px32 *a = tu_photo(1024, 512, true);
    pc_doc *d = tu_doc_from_px(1024, 512, a);
    /* Fast splits sides over 512 into the fewest even divisors <= 512 */
    p.quality = 40;
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "2,1,512,512") == 0);
    INFO("1024x512 Fast: %s", g);
    {   /* pixels survive the grid: lossless grid round trip */
        avif_params_t q = defaults();
        pc_buf out;
        pc_image_meta m;
        pc_doc *r;
        q.lossless = 1;
        if (save_doc(d, NULL, &q, &out)) {
            r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                CHECK(tu_diff(px, a, 1024u * 512u) == 0);
                CHECK(pc_meta_get(&m, "avif.grid") &&
                      strcmp(pc_meta_get(&m, "avif.grid"), "2,1,512,512") == 0);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
    }
    p.preset = 1;                                         /* Medium: 1280 */
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "none") == 0);
    p.preset = 3;                                         /* Very Slow: never */
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "none") == 0);
    free(a);
    pc_doc_destroy(d);
    /* odd sides are never split */
    a = tu_photo(1026, 513, false);
    d = tu_doc_from_px(1026, 513, a);
    p.preset = 0;
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "none") == 0);
    free(a);
    pc_doc_destroy(d);
    /* Medium splits 2600 into 4 x 650 (1300 is over 1280) */
    a = tu_photo(2600, 64, false);
    d = tu_doc_from_px(2600, 64, a);
    p.preset = 1;
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "4,1,650,64") == 0);
    INFO("2600x64 Medium: %s", g);
    free(a);
    pc_doc_destroy(d);
    /* Preserve existing tile size */
    a = tu_photo(640, 640, false);
    d = tu_doc_from_px(640, 640, a);
    memset(&keep, 0, sizeof keep);
    pc_meta_add(&keep, "avif.grid", "5,5,128,128");
    p = defaults();
    p.quality = 30;
    CHECK(strcmp(grid_of(d, &p, NULL, g, sizeof g), "2,2,320,320") == 0);   /* square */
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "5,5,128,128") == 0);
    p.preset = 1;                         /* kept even where Medium would not split */
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "5,5,128,128") == 0);
    p.preset = 0;
    p.keep_tiles = 0;
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "2,2,320,320") == 0);
    p.keep_tiles = 1;
    p.preset = 3;
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "none") == 0);
    pc_meta_free(&keep);
    /* a layout that does not fit the image falls back to the automatic one */
    memset(&keep, 0, sizeof keep);
    pc_meta_add(&keep, "avif.grid", "3,3,200,200");
    p.preset = 0;
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "2,2,320,320") == 0);
    pc_meta_free(&keep);
    memset(&keep, 0, sizeof keep);
    pc_meta_add(&keep, "avif.grid", "10,10,64,64,1");     /* malformed */
    CHECK(strcmp(grid_of(d, &p, &keep, g, sizeof g), "2,2,320,320") == 0);
    pc_meta_free(&keep);
    free(a);
    pc_doc_destroy(d);
}

/* ---- fixtures made with libavif ---------------------------------------------------------- */
/* Lossless 4:4:4 identity AVIF of BGRA pixels; prepare() may set
 * transforms, metadata or CICP on the image before encoding. */
typedef void (*prep_fn)(avifImage *im, void *ud);

static uint8_t *make_avif(const pc_px32 *px, uint32_t w, uint32_t h, uint32_t depth,
                          avifPixelFormat fmt, bool lossless, prep_fn prep, void *ud, size_t *n)
{
    avifImage *im = avifImageCreate(w, h, depth, fmt);
    avifRGBImage rgb;
    avifEncoder *enc;
    avifRWData data = AVIF_DATA_EMPTY;
    uint8_t *outp = NULL;
    uint16_t *px16 = NULL;
    *n = 0;
    im->yuvRange = AVIF_RANGE_FULL;
    im->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    im->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    im->matrixCoefficients = lossless ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
                                      : AVIF_MATRIX_COEFFICIENTS_BT601;
    avifRGBImageSetDefaults(&rgb, im);
    if (depth == 8u) {
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.pixels = (uint8_t *)(uintptr_t)px;
        rgb.rowBytes = w * 4u;
    } else {
        /* px carries 16-bit samples packed as r = high byte, g = low byte
         * of a single gray level per pixel (see callers); here: expand */
        px16 = (uint16_t *)malloc((size_t)w * h * 8u);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint16_t v = (uint16_t)((px[i].r << 8) | px[i].g);
            px16[i * 4] = px16[i * 4 + 1] = px16[i * 4 + 2] = v;
            px16[i * 4 + 3] = (uint16_t)((1u << depth) - 1u);
        }
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = depth;
        rgb.pixels = (uint8_t *)px16;
        rgb.rowBytes = w * 8u;
    }
    if (avifImageRGBToYUV(im, &rgb) != AVIF_RESULT_OK) goto out;
    if (prep) prep(im, ud);
    enc = avifEncoderCreate();
    enc->quality = lossless ? AVIF_QUALITY_LOSSLESS : 90;
    enc->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    enc->speed = 9;
    if (avifEncoderWrite(enc, im, &data) == AVIF_RESULT_OK) {
        outp = (uint8_t *)malloc(data.size);
        memcpy(outp, data.data, data.size);
        *n = data.size;
    }
    avifRWDataFree(&data);
    avifEncoderDestroy(enc);
out:
    free(px16);
    avifImageDestroy(im);
    return outp;
}

typedef struct xf_case {
    int crop;            /* apply a clap crop */
    int rot;             /* irot angle, -1 none */
    int mir;             /* imir axis, -1 none */
} xf_case;

static void prep_xf(avifImage *im, void *ud)
{
    const xf_case *c = (const xf_case *)ud;
    if (c->crop) {
        /* crop (3, 2, 11, 7) of the 20 x 12 image: offsets relative to the center */
        int32_t cw = 11, ch = 7, cx = 3, cy = 2;
        im->transformFlags |= AVIF_TRANSFORM_CLAP;
        im->clap.widthN = (uint32_t)cw; im->clap.widthD = 1;
        im->clap.heightN = (uint32_t)ch; im->clap.heightD = 1;
        im->clap.horizOffN = (uint32_t)(2 * cx + cw - 20); im->clap.horizOffD = 2;
        im->clap.vertOffN = (uint32_t)(2 * cy + ch - 12); im->clap.vertOffD = 2;
    }
    if (c->rot >= 0) {
        im->transformFlags |= AVIF_TRANSFORM_IROT;
        im->irot.angle = (uint8_t)c->rot;
    }
    if (c->mir >= 0) { im->transformFlags |= AVIF_TRANSFORM_IMIR; im->imir.axis = (uint8_t)c->mir; }
}

/* Reference: the source pixel shown at output (ox, oy), undoing mirror,
 * then the anti-clockwise rotation, then the crop. */
static pc_px32 ref_px(const pc_px32 *src, uint32_t sw, const xf_case *c, uint32_t ox, uint32_t oy,
                      uint32_t ow, uint32_t oh)
{
    uint32_t cx = c->crop ? 3u : 0u, cy = c->crop ? 2u : 0u;
    uint32_t cw = c->crop ? 11u : 20u, ch = c->crop ? 7u : 12u, x, y;
    if (c->mir == 0) oy = oh - 1u - oy;                  /* top and bottom exchanged */
    if (c->mir == 1) ox = ow - 1u - ox;                  /* left and right exchanged */
    switch (c->rot) {
    case 1: x = cw - 1u - oy; y = ox; break;             /* 90 anti-clockwise */
    case 2: x = cw - 1u - ox; y = ch - 1u - oy; break;
    case 3: x = oy; y = ch - 1u - ox; break;             /* 270 anti-clockwise */
    default: x = ox; y = oy; break;
    }
    return src[(size_t)(cy + y) * sw + cx + x];
}

static void t_transforms(void)
{
    const uint32_t W = 20, H = 12;
    pc_px32 *src = tu_noise(W, H, 0);
    /* anchor: a 2 x 1 image [A B] turned 90 degrees anti-clockwise is B over A */
    {
        pc_px32 two[2];
        xf_case c = { 0, 1, -1 };
        size_t n;
        uint8_t *f;
        pc_image_meta m;
        two[0] = tu_px(255, 0, 0, 255);
        two[1] = tu_px(0, 0, 255, 255);
        f = make_avif(two, 2, 1, 8, AVIF_PIXEL_FORMAT_YUV444, true, prep_xf, &c, &n);
        CHECK(f != NULL);
        if (f) {
            pc_doc *r = load_ok(f, n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                CHECK(r->w == 1 && r->h == 2);
                CHECK(px[0].b == 255 && px[0].r == 0 && px[1].r == 255 && px[1].b == 0);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            free(f);
        }
    }
    for (int crop = 0; crop < 2; crop++)
        for (int rot = -1; rot < 4; rot++)
            for (int mir = -1; mir < 2; mir++) {
                xf_case c;
                size_t n;
                uint8_t *f;
                pc_image_meta m;
                c.crop = crop; c.rot = rot; c.mir = mir;
                f = make_avif(src, W, H, 8, AVIF_PIXEL_FORMAT_YUV444, true, prep_xf, &c, &n);
                CHECK(f != NULL);
                if (!f) continue;
                {
                    pc_doc *r = load_ok(f, n, &m);
                    uint32_t cw = crop ? 11u : W, ch = crop ? 7u : H;
                    uint32_t ow = (rot == 1 || rot == 3) ? ch : cw;
                    uint32_t oh = (rot == 1 || rot == 3) ? cw : ch;
                    if (r) {
                        pc_px32 *px = doc_px(r);
                        size_t bad = 0;
                        CHECK(r->w == ow && r->h == oh);
                        if (r->w == ow && r->h == oh)
                            for (uint32_t y = 0; y < oh; y++)
                                for (uint32_t x = 0; x < ow; x++)
                                    if (!tu_px_eq(px[(size_t)y * ow + x],
                                                  ref_px(src, W, &c, x, y, ow, oh)))
                                        bad++;
                        CHECK(bad == 0u);
                        if (bad) INFO("crop %d rot %d mir %d: %zu wrong", crop, rot, mir, bad);
                        CHECK(pc_meta_get(&m, "avif.grid") == NULL);
                        free(px);
                        pc_doc_destroy(r);
                        pc_meta_free(&m);
                    }
                }
                free(f);
            }
    free(src);
}

/* Image sequence: the first frame is loaded and the note says so. */
static void t_sequence(void)
{
    const uint32_t W = 32, H = 24;
    avifEncoder *enc = avifEncoderCreate();
    avifRWData data = AVIF_DATA_EMPTY;
    avifResult r = AVIF_RESULT_OK;
    pc_image_meta m;
    enc->quality = AVIF_QUALITY_LOSSLESS;
    enc->speed = 10;
    enc->timescale = 10;
    for (int f = 0; f < 3 && r == AVIF_RESULT_OK; f++) {
        avifImage *im = avifImageCreate(W, H, 8, AVIF_PIXEL_FORMAT_YUV444);
        avifRGBImage rgb;
        pc_px32 *px = (pc_px32 *)malloc((size_t)W * H * sizeof *px);
        for (size_t i = 0; i < (size_t)W * H; i++) px[i] = tu_px((uint8_t)(f * 100), 50, 200, 255);
        im->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
        im->yuvRange = AVIF_RANGE_FULL;
        avifRGBImageSetDefaults(&rgb, im);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.pixels = (uint8_t *)px;
        rgb.rowBytes = W * 4u;
        r = avifImageRGBToYUV(im, &rgb);
        if (r == AVIF_RESULT_OK) r = avifEncoderAddImage(enc, im, 1, AVIF_ADD_IMAGE_FLAG_NONE);
        avifImageDestroy(im);
        free(px);
    }
    if (r == AVIF_RESULT_OK) r = avifEncoderFinish(enc, &data);
    CHECK(r == AVIF_RESULT_OK);
    if (r == AVIF_RESULT_OK) {
        pc_doc *d = load_ok(data.data, data.size, &m);
        if (d) {
            pc_px32 *px = doc_px(d);
            CHECK(d->w == W && d->h == H);
            CHECK(px[0].r == 0 && px[0].g == 50 && px[0].b == 200);
            CHECK(strstr(m.note, "first frame") != NULL);
            INFO("note: %s", m.note);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
    }
    avifRWDataFree(&data);
    avifEncoderDestroy(enc);
}

/* ---- metadata ------------------------------------------------------------------------------- */
static uint8_t *p3_profile(size_t *len)
{
    cmsContext ctx = cmsCreateContext(NULL, NULL);
    cmsCIExyY wp = { 0.3127, 0.3290, 1.0 };
    cmsCIExyYTRIPLE pr = { { 0.680, 0.320, 1.0 }, { 0.265, 0.690, 1.0 }, { 0.150, 0.060, 1.0 } };
    cmsToneCurve *g = cmsBuildGamma(ctx, 2.2);
    cmsToneCurve *c3[3];
    cmsHPROFILE h;
    cmsUInt32Number n = 0;
    uint8_t *p = NULL;
    c3[0] = c3[1] = c3[2] = g;
    h = cmsCreateRGBProfileTHR(ctx, &wp, &pr, c3);
    if (h && cmsSaveProfileToMem(h, NULL, &n)) {
        p = (uint8_t *)malloc(n);
        cmsSaveProfileToMem(h, p, &n);
        *len = n;
    }
    if (h) cmsCloseProfile(h);
    cmsFreeToneCurve(g);
    cmsDeleteContext(ctx);
    return p;
}

/* Minimal big-endian TIFF with Orientation and a 300 dpi resolution. */
static size_t exif_block(uint8_t *t, uint32_t orient)
{
    static const uint8_t k[] = {
        'M', 'M', 0, 42, 0, 0, 0, 8,  0, 4,
        0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 1, 0, 0,
        0x01, 0x1A, 0, 5, 0, 0, 0, 1, 0, 0, 0, 62,
        0x01, 0x1B, 0, 5, 0, 0, 0, 1, 0, 0, 0, 70,
        0x01, 0x28, 0, 3, 0, 0, 0, 1, 0, 2, 0, 0,
        0, 0, 0, 0,
        0, 0, 1, 44, 0, 0, 0, 1,  0, 0, 1, 44, 0, 0, 0, 1 };
    memcpy(t, k, sizeof k);
    t[19] = (uint8_t)orient;
    return sizeof k;
}

static void t_metadata(void)
{
    const uint32_t W = 40, H = 30;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    pc_image_meta in, m;
    size_t icc_len = 0, tl;
    uint8_t *icc = p3_profile(&icc_len), tiff[80];
    char *b64 = NULL;
    static const char xmp[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><t>paint.c</t></x:xmpmeta>";
    avif_params_t p = defaults();
    pc_buf out;
    memset(&in, 0, sizeof in);
    in.icc = icc;
    in.icc_len = icc_len;
    tl = exif_block(tiff, 6);
    axj_b64_encode(tiff, tl, &b64);
    pc_meta_add(&in, "exif", b64);
    pc_meta_add(&in, "xmp", xmp);
    free(b64);
    if (save_doc(d, &in, &p, &out)) {
        avifImage *im = raw_decode(out.p, out.n);
        CHECK(im && im->icc.size == icc_len && memcmp(im->icc.data, icc, icc_len) == 0);
        CHECK(im && im->transformFlags == AVIF_TRANSFORM_NONE);     /* pixels are upright */
        CHECK(im && im->xmp.size == strlen(xmp));
        CHECK(im && im->exif.size == tl && im->exif.data[19] == 1);  /* Orientation 1 */
        if (im) avifImageDestroy(im);
        r = load_ok(out.p, out.n, &m);
        if (r) {
            uint8_t *e;
            size_t el = 0;
            CHECK(m.icc_len == icc_len && memcmp(m.icc, icc, icc_len) == 0);
            CHECK(pc_meta_get(&m, "xmp") && strcmp(pc_meta_get(&m, "xmp"), xmp) == 0);
            e = axj_meta_get_exif(&m, &el);
            CHECK(e && el == tl && e[19] == 1 && memcmp(e + 20, tiff + 20, tl - 20) == 0);
            CHECK(fabs(m.dpi_x - 300.0) < 1e-6 && fabs(m.dpi_y - 300.0) < 1e-6);
            free(e);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
    }
    /* a gray image with an RGB profile stays RGB (4:2:2), keeps the profile */
    {
        pc_px32 *g = tu_noise(W, H, 0);
        pc_doc *gd;
        for (size_t i = 0; i < (size_t)W * H; i++) g[i].g = g[i].b = g[i].r;
        gd = tu_doc_from_px(W, H, g);
        if (save_doc(gd, &in, &p, &out)) {
            avifImage *im = raw_decode(out.p, out.n);
            CHECK(im && im->yuvFormat == AVIF_PIXEL_FORMAT_YUV422 && im->icc.size == icc_len);
            if (im) avifImageDestroy(im);
            pc_buf_free(&out);
        }
        free(g);
        pc_doc_destroy(gd);
    }
    pc_meta_free(&in);
    free(a);
    pc_doc_destroy(d);
}

typedef struct cicp_case { uint16_t prim, tc; } cicp_case;

static void prep_cicp(avifImage *im, void *ud)
{
    const cicp_case *c = (const cicp_case *)ud;
    im->colorPrimaries = c->prim;
    im->transferCharacteristics = c->tc;
}

static void prep_exif_rot(avifImage *im, void *ud)
{
    uint8_t t[80];
    size_t n = exif_block(t, 6);
    (void)ud;
    (void)avifImageSetMetadataExif(im, t, n);
    im->transformFlags = AVIF_TRANSFORM_IROT;             /* Exif 6 = irot 3 */
    im->irot.angle = 3;
}

static void t_color(void)
{
    const uint32_t W = 16, H = 8;
    pc_px32 *a = tu_noise(W, H, 0);
    static const cicp_case cases[] = { { 1, 13 }, { 2, 2 }, { 1, 1 }, { 12, 13 }, { 9, 1 },
                                       { 1, 8 } };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t n;
        uint8_t *f = make_avif(a, W, H, 8, AVIF_PIXEL_FORMAT_YUV444, true, prep_cicp,
                               (void *)(uintptr_t)&cases[i], &n);
        pc_image_meta m;
        pc_doc *r;
        if (!f) { CHECK(f != NULL); continue; }
        r = load_ok(f, n, &m);
        if (r) {
            bool srgb = i < 3;
            pc_px32 *px = doc_px(r);
            CHECK(tu_diff(px, a, (size_t)W * H) == 0);          /* pixels never converted */
            CHECK((m.icc == NULL) == srgb);
            if (m.icc) {
                pc_icc_info info;
                CHECK(pc_icc_inspect(m.icc, m.icc_len, &info) == PC_OK);
                CHECK(info.space == PC_ICC_SPACE_RGB && !info.is_srgb);
                CHECK(strstr(info.desc, "CICP") != NULL);
            }
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(f);
    }
    free(a);
    /* Exif Orientation 6 with the matching irot: rotated once, Exif reset to 1 */
    {
        pc_px32 two[2];
        size_t n;
        uint8_t *f;
        pc_image_meta m;
        two[0] = tu_px(255, 0, 0, 255);
        two[1] = tu_px(0, 0, 255, 255);
        f = make_avif(two, 2, 1, 8, AVIF_PIXEL_FORMAT_YUV444, true, prep_exif_rot, NULL, &n);
        if (f) {
            pc_doc *r = load_ok(f, n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                uint8_t *e;
                size_t el = 0;
                /* 90 degrees clockwise: A over B */
                CHECK(r->w == 1 && r->h == 2 && px[0].r == 255 && px[1].b == 255);
                e = axj_meta_get_exif(&m, &el);
                CHECK(e && axj_exif_reset_orientation(e, el) == 1u);
                free(e);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            free(f);
        }
    }
}

static void prep_pq(avifImage *im, void *ud)
{
    (void)ud;
    im->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
    im->transferCharacteristics = (avifTransferCharacteristics)16;   /* PQ */
}

static void t_high_bit_depth(void)
{
    const uint32_t W = 8, H = 4;
    pc_px32 lv[32];
    static const uint32_t depths[2] = { 10, 12 };
    for (int k = 0; k < 2; k++) {
        uint32_t maxv = (1u << depths[k]) - 1u;
        size_t n;
        uint8_t *f;
        pc_image_meta m;
        for (uint32_t i = 0; i < W * H; i++) {
            uint32_t v = i * maxv / (W * H - 1u);
            lv[i].r = (uint8_t)(v >> 8); lv[i].g = (uint8_t)v; lv[i].b = 0; lv[i].a = 255;
        }
        f = make_avif(lv, W, H, depths[k], AVIF_PIXEL_FORMAT_YUV444, true, NULL, NULL, &n);
        CHECK(f != NULL);
        if (!f) continue;
        {
            pc_doc *r = load_ok(f, n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                int worst = 0;
                for (uint32_t i = 0; i < W * H; i++) {
                    uint32_t v = i * maxv / (W * H - 1u);
                    int want = (int)((v * 255u + maxv / 2u) / maxv);
                    int dd = abs((int)px[i].g - want);
                    if (dd > worst) worst = dd;
                    CHECK(px[i].r == px[i].g && px[i].g == px[i].b);
                }
                CHECK(worst <= 1);
                CHECK(m.src_bits == depths[k] && m.icc == NULL && m.note[0] == '\0');
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        free(f);
    }
    /* PQ (HDR): diffuse white 203 cd/m2 is near sRGB white, black stays black */
    {
        size_t n;
        uint8_t *f;
        pc_image_meta m;
        for (uint32_t i = 0; i < W * H; i++) {
            uint32_t v = i < 16u ? 0u : 594u;            /* PQ code of 203 cd/m2, 10 bit */
            lv[i].r = (uint8_t)(v >> 8); lv[i].g = (uint8_t)v; lv[i].b = 0; lv[i].a = 255;
        }
        f = make_avif(lv, W, H, 10, AVIF_PIXEL_FORMAT_YUV444, true, prep_pq, NULL, &n);
        CHECK(f != NULL);
        if (f) {
            pc_doc *r = load_ok(f, n, &m);
            if (r) {
                pc_px32 *px = doc_px(r);
                CHECK(px[0].r == 0 && px[0].g == 0);
                CHECK(px[20].r >= 236 && px[20].r <= 250 && px[20].r == px[20].b);
                CHECK(strstr(m.note, "HDR") != NULL && m.icc == NULL);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            free(f);
        }
    }
}

static void t_limits_fuzz(void)
{
    pc_px32 *a = tu_photo(64, 64, true);
    pc_doc *d = tu_doc_from_px(64, 64, a), *r = NULL;
    avif_params_t p = defaults();
    pc_codec_limits lim;
    pc_image_meta m;
    pc_buf out;
    p.quality = 50;
    if (save_doc(d, NULL, &p, &out)) {
        pc_codec_limits_default(&lim);
        lim.max_w = 63;
        CHECK(av()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL);
        pc_codec_limits_default(&lim);
        lim.max_pixels = 64u * 64u - 1u;
        CHECK(av()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL);
        pc_codec_limits_default(&lim);
        lim.max_mem = 20000;
        CHECK(av()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL);
        /* a declared 60000 x 60000 'ispe' is refused before decoding */
        {
            uint8_t *big = (uint8_t *)malloc(out.n);
            size_t hits = 0;
            memcpy(big, out.p, out.n);
            for (size_t i = 0; i + 20 <= out.n; i++)
                if (memcmp(big + i + 4, "ispe", 4) == 0) {
                    big[i + 12] = 0; big[i + 13] = 0; big[i + 14] = 0xEA; big[i + 15] = 0x60;
                    big[i + 16] = 0; big[i + 17] = 0; big[i + 18] = 0xEA; big[i + 19] = 0x60;
                    hits++;
                }
            CHECK(hits >= 1u);
            CHECK(av()->load(big, out.n, NULL, &r, &m) != PC_OK && r == NULL);
            free(big);
        }
        /* truncation and garbage */
        CHECK(av()->load(out.p, out.n / 2u, NULL, &r, &m) != PC_OK && r == NULL);
        CHECK(av()->load(out.p, 20, NULL, &r, &m) != PC_OK && r == NULL);
        {
            uint32_t ok = tu_fuzz_codec(av(), out.p, out.n, g_quick ? 150u : 1500u, NULL);
            INFO("fuzz: %u of %u mutations decoded", ok, g_quick ? 150u : 1500u);
        }
        pc_buf_free(&out);
    }
    /* odd and tiny sizes in every chroma mode */
    {
        static const uint32_t sz[4][2] = { { 1, 1 }, { 1, 2 }, { 3, 5 }, { 17, 1 } };
        for (int i = 0; i < 4; i++)
            for (int c = 0; c < 3; c++) {
                pc_px32 *b = tu_photo(sz[i][0], sz[i][1], true);
                pc_doc *bd = tu_doc_from_px(sz[i][0], sz[i][1], b);
                avif_params_t q = defaults();
                q.chroma = c;
                if (save_doc(bd, NULL, &q, &out)) {
                    pc_doc *rr = load_ok(out.p, out.n, &m);
                    if (rr) {
                        CHECK(rr->w == sz[i][0] && rr->h == sz[i][1]);
                        pc_doc_destroy(rr);
                        pc_meta_free(&m);
                    }
                    pc_buf_free(&out);
                } else {
                    CHECK(0);
                }
                free(b);
                pc_doc_destroy(bd);
            }
    }
    free(a);
    pc_doc_destroy(d);
}
#endif /* PC_HAVE_AVIF */

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_registry);
#if defined(PC_HAVE_AVIF)
    RUN(t_lossless);
    RUN(t_lossy);
    RUN(t_chroma_presets);
    RUN(t_alpha_options);
    RUN(t_grid);
    RUN(t_transforms);
    RUN(t_sequence);
    RUN(t_metadata);
    RUN(t_color);
    RUN(t_high_bit_depth);
    RUN(t_limits_fuzz);
#endif
    return pc_test_finish();
}
