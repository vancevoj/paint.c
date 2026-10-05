/* test_jxl.c - JPEG XL codec (fmt_jxl.c over libjxl, lane AVIFJXL):
 * registry and save options, lossless exact round trips (RGBA, RGB, gray,
 * gray + alpha) with the channel layout checked in the file, lossy quality
 * ordering (quality to distance), effort, ICC, Exif and XMP boxes (also
 * Brotli-compressed), the 8 codestream orientations, animations (first
 * frame), 16-bit and HDR (PQ) input, the pc_par parallel runner (output
 * identical to the serial one), limits and mutation fuzzing. Fixtures
 * with features paint.c never writes are made with libjxl. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/avifjxl_meta.h"
#include "pc/pc_icc.h"

#include <math.h>

static const pc_codec *jx(void) { return pc_codec_by_id("jxl"); }

typedef struct jxl_params_t { int32_t quality, lossless, effort; } jxl_params_t;

static void t_registry(void)
{
    static const uint8_t bare[2] = { 0xFF, 0x0A };
    static const uint8_t box[12] = { 0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A };
    jxl_params_t p;
    const pc_codec *c = jx();
    CHECK(c != NULL);
    if (!c) return;
    CHECK(strcmp(c->name, "JPEG XL") == 0 && strcmp(c->exts, "jxl") == 0);
    CHECK(pc_codec_by_ext("JXL") == c);
    CHECK(c->sniff(bare, 2) && c->sniff(box, 12) && !c->sniff(box, 11) && !c->sniff(bare, 1));
    CHECK(c->n_props == 3u && c->params_size == sizeof p);
    pc_codec_default_params(c, &p);
    CHECK(p.quality == 90 && p.lossless == 0 && p.effort == 7);
    CHECK(strcmp(c->props[0].enabled_if, "lossless=0") == 0);
    CHECK(c->props[2].min == 1.0 && c->props[2].max == 9.0);
#if defined(PC_HAVE_JXL)
    CHECK(c->flags == (PC_CODEC_LOAD | PC_CODEC_SAVE) && c->load && c->save);
    CHECK(pc_codec_sniff(box, 12) == c);
#else
    CHECK(c->flags == 0u && c->load == NULL && c->save == NULL);
    INFO("libjxl not available: JPEG XL load/save tests skipped");
#endif
}

#if defined(PC_HAVE_JXL)
#include "jxl/decode.h"
#include "jxl/encode.h"
#include "jxl/version.h"
#include "lcms2.h"

static void par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, i, (i * 7u) % 4u);
}
static const pc_par k_par4 = { par_run, NULL, 4u };

static jxl_params_t defaults(void)
{
    jxl_params_t p;
    pc_codec_default_params(jx(), &p);
    return p;
}

static bool save_doc(const pc_doc *d, const pc_image_meta *m, const jxl_params_t *p,
                     const pc_par *par, pc_buf *out)
{
    pc_status st;
    memset(out, 0, sizeof *out);
    st = jx()->save(d, m, p, par, out);
    if (st != PC_OK) INFO("jxl save: %s", pc_status_str(st));
    return st == PC_OK;
}

static pc_doc *load_ok(const uint8_t *p, size_t n, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_status st = jx()->load(p, n, NULL, &d, m);
    CHECK(st == PC_OK);
    if (st != PC_OK) INFO("jxl load: %s", pc_status_str(st));
    return d;
}

static pc_px32 *doc_px(const pc_doc *d) { return tu_layer_px(d, d->stack[0]); }

static uint8_t u16_to_u8(uint32_t v) { return (uint8_t)((v * 255u + 32767u) / 65535u); }

static void bgra_to_rgba(uint8_t *dst, const pc_px32 *src, size_t n)
{
    for (size_t i = 0; i < n; i++, dst += 4) {
        dst[0] = src[i].r; dst[1] = src[i].g; dst[2] = src[i].b; dst[3] = src[i].a;
    }
}

/* Basic info of a file, straight from libjxl. */
static bool basic_info(const uint8_t *p, size_t n, JxlBasicInfo *bi)
{
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    bool ok = false;
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO);
    JxlDecoderSetInput(dec, p, n);
    JxlDecoderCloseInput(dec);
    if (JxlDecoderProcessInput(dec) == JXL_DEC_BASIC_INFO)
        ok = JxlDecoderGetBasicInfo(dec, bi) == JXL_DEC_SUCCESS;
    JxlDecoderDestroy(dec);
    return ok;
}

static void lossless_case(const pc_px32 *px, uint32_t w, uint32_t h, uint32_t want_color,
                          uint32_t want_alpha_bits, int effort)
{
    pc_doc *d = tu_doc_from_px(w, h, px), *r;
    jxl_params_t p = defaults();
    pc_buf out;
    pc_image_meta m;
    JxlBasicInfo bi;
    p.lossless = 1;
    p.quality = 3;                       /* ignored when lossless */
    p.effort = effort;
    if (save_doc(d, NULL, &p, NULL, &out)) {
        CHECK(basic_info(out.p, out.n, &bi));
        CHECK(bi.num_color_channels == want_color && bi.alpha_bits == want_alpha_bits);
        CHECK(bi.uses_original_profile == JXL_TRUE);
        r = load_ok(out.p, out.n, &m);
        if (r) {
            pc_px32 *q = doc_px(r);
            CHECK(r->w == w && r->h == h);
            CHECK(tu_diff(q, px, (size_t)w * h) == 0);
            CHECK(m.had_alpha == (want_alpha_bits != 0) && m.src_bits == 8u && m.icc == NULL);
            CHECK(pc_doc_edge_padding_is_zero(r));
            free(q);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
}

static void t_lossless(void)
{
    const uint32_t W = 67, H = 45;
    pc_px32 *rgba = tu_noise(W, H, 1), *rgb = tu_noise(W, H, 0), *g = tu_noise(W, H, 1);
    pc_px32 *g1 = tu_noise(W, H, 0);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        g[i].g = g[i].b = g[i].r;
        g1[i].g = g1[i].b = g1[i].r;
    }
    lossless_case(rgba, W, H, 3u, 8u, 7);
    lossless_case(rgb, W, H, 3u, 0u, 1);
    lossless_case(g, W, H, 1u, 8u, 9);
    lossless_case(g1, W, H, 1u, 0u, 3);
    free(rgba); free(rgb); free(g); free(g1);
    /* multi-layer document: the flattened image is written */
    {
        pc_px32 *a = tu_photo(40, 30, true), *b = tu_noise(40, 30, 2), *flat;
        pc_doc *d = tu_doc_from_px(40, 30, a);
        jxl_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        tu_doc_add_layer(d, b, PC_BLEND_OVERLAY, 128, true, "o");
        flat = tu_flatten(d);
        p.lossless = 1;
        if (save_doc(d, NULL, &p, NULL, &out)) {
            pc_doc *r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *q = doc_px(r);
                CHECK(tu_diff(q, flat, 40u * 30u) == 0);
                free(q);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
        free(a); free(b); free(flat);
        pc_doc_destroy(d);
    }
}

static void t_lossy(void)
{
    const uint32_t W = 96, H = 64;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int qs[] = { 5, 25, 60, 90, 100 };
    size_t sizes[5] = { 0, 0, 0, 0, 0 };
    double psnr[5] = { 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        jxl_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        JxlBasicInfo bi;
        p.quality = qs[i];
        if (!save_doc(d, NULL, &p, NULL, &out)) continue;
        CHECK(basic_info(out.p, out.n, &bi) && bi.uses_original_profile == JXL_FALSE &&
              bi.alpha_bits == 0u);
        {
            pc_doc *r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *q = doc_px(r);
                double ps = tu_psnr(q, a, (size_t)W * H);
                INFO("quality %3d: %6zu bytes, PSNR %.1f dB", qs[i], out.n, ps);
                if (qs[i] >= 90) CHECK(ps > 36.0);
                if (qs[i] == 100) CHECK(ps < 99.0);       /* quality 100 is not lossless */
                CHECK(m.icc == NULL);                      /* sRGB: no profile */
                sizes[i] = out.n;
                psnr[i] = ps;
                free(q);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        pc_buf_free(&out);
    }
    /* higher quality: larger files, better fidelity (tiny images vary a
     * little between neighbouring qualities, so compare distant ones) */
    CHECK(sizes[4] > sizes[3] && sizes[3] > sizes[2] && sizes[3] > sizes[0]);
    CHECK(psnr[4] > psnr[3] && psnr[3] > psnr[1] + 3.0 && psnr[2] > psnr[0]);
    /* effort: every level writes a decodable file */
    for (int e = 1; e <= 9; e += 4) {
        jxl_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        p.effort = e;
        if (save_doc(d, NULL, &p, NULL, &out)) {
            pc_doc *r = load_ok(out.p, out.n, &m);
            if (r) { pc_doc_destroy(r); pc_meta_free(&m); }
            pc_buf_free(&out);
        }
    }
    free(a);
    pc_doc_destroy(d);
}

/* libjxl's output does not depend on the thread count: the pc_par runner
 * must produce the same bytes as the serial encoder. */
static void t_parallel_runner(void)
{
    pc_px32 *a = tu_photo(300, 200, true);
    pc_doc *d = tu_doc_from_px(300, 200, a);
    for (int lossless = 0; lossless < 2; lossless++) {
        jxl_params_t p = defaults();
        pc_buf s1, s4;
        p.lossless = lossless;
        p.effort = 5;
        if (save_doc(d, NULL, &p, NULL, &s1) && save_doc(d, NULL, &p, &k_par4, &s4)) {
            CHECK(s1.n == s4.n && memcmp(s1.p, s4.p, s1.n) == 0);
        }
        pc_buf_free(&s1);
        pc_buf_free(&s4);
    }
    free(a);
    pc_doc_destroy(d);
}

/* ---- metadata ---------------------------------------------------------------------------- */
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

static size_t exif_block(uint8_t *t, uint32_t orient)
{
    static const uint8_t k[] = {
        'I', 'I', 42, 0, 8, 0, 0, 0,  4, 0,
        0x12, 0x01, 3, 0, 1, 0, 0, 0, 1, 0, 0, 0,
        0x1A, 0x01, 5, 0, 1, 0, 0, 0, 62, 0, 0, 0,
        0x1B, 0x01, 5, 0, 1, 0, 0, 0, 70, 0, 0, 0,
        0x28, 0x01, 3, 0, 1, 0, 0, 0, 3, 0, 0, 0,          /* centimeters */
        0, 0, 0, 0,
        100, 0, 0, 0, 1, 0, 0, 0,  100, 0, 0, 0, 1, 0, 0, 0 };
    memcpy(t, k, sizeof k);
    t[18] = (uint8_t)orient;
    return sizeof k;
}

static void t_metadata(void)
{
    const uint32_t W = 40, H = 30;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    pc_image_meta in;
    size_t icc_len = 0, tl;
    uint8_t *icc = p3_profile(&icc_len), tiff[80];
    char *b64 = NULL;
    static const char xmp[] = "<?xpacket begin=\"\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
    memset(&in, 0, sizeof in);
    in.icc = icc;
    in.icc_len = icc_len;
    tl = exif_block(tiff, 8);
    axj_b64_encode(tiff, tl, &b64);
    pc_meta_add(&in, "exif", b64);
    pc_meta_add(&in, "xmp", xmp);
    free(b64);
    for (int lossless = 1; lossless >= 0; lossless--) {
        jxl_params_t p = defaults();
        pc_buf out;
        pc_image_meta m;
        p.lossless = lossless;
        if (!save_doc(d, &in, &p, NULL, &out)) continue;
        {
            pc_doc *r = load_ok(out.p, out.n, &m);
            if (r) {
                uint8_t *e;
                size_t el = 0;
                pc_px32 *q = doc_px(r);
                if (lossless) {
                    CHECK(tu_diff(q, a, (size_t)W * H) == 0);
                    CHECK(m.icc_len == icc_len && memcmp(m.icc, icc, icc_len) == 0);
                } else {
#if defined(PC_JXL_HAVE_CMS)
                    /* lossy (XYB): the CMS delivers the pixels in the P3 space,
                     * tagged with the profile */
                    pc_icc_info info;
                    CHECK(m.icc != NULL);
                    CHECK(m.icc && pc_icc_inspect(m.icc, m.icc_len, &info) == PC_OK &&
                          info.space == PC_ICC_SPACE_RGB && !info.is_srgb);
                    CHECK(tu_psnr(q, a, (size_t)W * H) > 30.0);
#else
                    /* libjxl without a CMS (before 0.9): sRGB pixels, no profile */
                    CHECK(m.icc == NULL && strstr(m.note, "sRGB") != NULL);
#endif
                }
                CHECK(pc_meta_get(&m, "xmp") && strcmp(pc_meta_get(&m, "xmp"), xmp) == 0);
                e = axj_meta_get_exif(&m, &el);
                CHECK(e && el == tl && e[18] == 1 && memcmp(e + 19, tiff + 19, tl - 19) == 0);
                CHECK(fabs(m.dpi_x - 254.0) < 1e-6 && fabs(m.dpi_y - 254.0) < 1e-6);
                free(e);
                free(q);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        pc_buf_free(&out);
    }
    pc_meta_free(&in);
    free(a);
    pc_doc_destroy(d);
}

/* ---- fixtures made with libjxl -------------------------------------------------------------- */
typedef struct fx_opts {
    uint32_t orientation;      /* 1..8 */
    int      frames;           /* > 1: animation */
    bool     u16;              /* 16-bit samples (pixel values from px16) */
    bool     pq;               /* BT.2100 PQ color encoding */
    bool     boxes;            /* Exif + xml boxes, Brotli-compressed */
    bool     lossy;
} fx_opts;

static uint8_t *make_jxl(const pc_px32 *px, const uint16_t *px16, uint32_t w, uint32_t h,
                         const fx_opts *o, size_t *n)
{
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlBasicInfo bi;
    JxlColorEncoding ce;
    JxlEncoderFrameSettings *fs;
    JxlPixelFormat pf = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    uint8_t *rgba = NULL, *outp = NULL;
    size_t cap = (size_t)65536u, used = 0;
    bool ok = true;
    *n = 0;
    if (o->boxes) JxlEncoderUseBoxes(enc);
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = w;
    bi.ysize = h;
    bi.bits_per_sample = o->u16 ? 16u : 8u;
    bi.num_color_channels = 3;
    bi.num_extra_channels = 1;
    bi.alpha_bits = o->u16 ? 16u : 8u;
    bi.uses_original_profile = o->lossy ? JXL_FALSE : JXL_TRUE;
    bi.orientation = (JxlOrientation)o->orientation;
    if (o->frames > 1) {
        bi.have_animation = JXL_TRUE;
        bi.animation.tps_numerator = 10;
        bi.animation.tps_denominator = 1;
        bi.animation.num_loops = 0;
    }
    ok = JxlEncoderSetBasicInfo(enc, &bi) == JXL_ENC_SUCCESS;
    memset(&ce, 0, sizeof ce);
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (o->pq) {
        ce.primaries = JXL_PRIMARIES_2100;
        ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
        ce.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
    }
    ok = ok && JxlEncoderSetColorEncoding(enc, &ce) == JXL_ENC_SUCCESS;
    if (o->boxes) {
        uint8_t ex[84];
        static const char xmp[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><b/></x:xmpmeta>";
        memset(ex, 0, 4);
        exif_block(ex + 4, 6);
        ok = ok && JxlEncoderAddBox(enc, "Exif", ex, 82, JXL_TRUE) == JXL_ENC_SUCCESS;
        ok = ok && JxlEncoderAddBox(enc, "xml ", (const uint8_t *)xmp, sizeof xmp - 1u,
                                    JXL_TRUE) == JXL_ENC_SUCCESS;
        JxlEncoderCloseBoxes(enc);
    }
    fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (o->lossy) ok = ok && JxlEncoderSetFrameDistance(fs, 1.0f) == JXL_ENC_SUCCESS;
    else ok = ok && JxlEncoderSetFrameLossless(fs, JXL_TRUE) == JXL_ENC_SUCCESS;
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 3);
    if (o->u16) {
        pf.data_type = JXL_TYPE_UINT16;
    } else {
        rgba = (uint8_t *)malloc((size_t)w * h * 4u);
        bgra_to_rgba(rgba, px, (size_t)w * h);
    }
    for (int f = 0; f < (o->frames > 1 ? o->frames : 1) && ok; f++) {
        if (o->frames > 1) {
            JxlFrameHeader fh;
            JxlEncoderInitFrameHeader(&fh);
            fh.duration = 1;
            ok = JxlEncoderSetFrameHeader(fs, &fh) == JXL_ENC_SUCCESS;
            if (f > 0 && rgba)                       /* later frames: inverted colors */
                for (size_t i = 0; i < (size_t)w * h * 4u; i++)
                    if (i % 4u != 3u) rgba[i] = (uint8_t)(255u - rgba[i]);
        }
        if (o->u16)
            ok = ok && JxlEncoderAddImageFrame(fs, &pf, px16, (size_t)w * h * 8u) ==
                           JXL_ENC_SUCCESS;
        else
            ok = ok && JxlEncoderAddImageFrame(fs, &pf, rgba, (size_t)w * h * 4u) ==
                           JXL_ENC_SUCCESS;
    }
    JxlEncoderCloseInput(enc);
    outp = (uint8_t *)malloc(cap);
    while (ok) {
        uint8_t *next = outp + used;
        size_t avail = cap - used;
        JxlEncoderStatus s = JxlEncoderProcessOutput(enc, &next, &avail);
        used = (size_t)(next - outp);
        if (s == JXL_ENC_SUCCESS) break;
        if (s != JXL_ENC_NEED_MORE_OUTPUT) { ok = false; break; }
        cap *= 2u;
        outp = (uint8_t *)realloc(outp, cap);
    }
    JxlEncoderDestroy(enc);
    free(rgba);
    if (!ok) { free(outp); return NULL; }
    *n = used;
    return outp;
}

/* Source pixel shown at (x, y) for Exif/JPEG XL orientation o of a w x h
 * stored image (independent table of the eight cases). */
static pc_px32 oriented(const pc_px32 *src, uint32_t w, uint32_t h, uint32_t o, uint32_t x,
                        uint32_t y)
{
    uint32_t sx, sy;
    switch (o) {
    case 2: sx = w - 1u - x; sy = y; break;                 /* mirrored horizontally */
    case 3: sx = w - 1u - x; sy = h - 1u - y; break;         /* rotated 180 */
    case 4: sx = x; sy = h - 1u - y; break;                  /* mirrored vertically */
    case 5: sx = y; sy = x; break;                           /* transposed */
    case 6: sx = y; sy = h - 1u - x; break;                  /* rotated 90 clockwise */
    case 7: sx = w - 1u - y; sy = h - 1u - x; break;         /* transversed */
    case 8: sx = w - 1u - y; sy = x; break;                  /* rotated 90 anti-clockwise */
    default: sx = x; sy = y; break;
    }
    return src[(size_t)sy * w + sx];
}

static void t_orientation(void)
{
    const uint32_t W = 13, H = 7;
    pc_px32 *src = tu_noise(W, H, 1);
    for (uint32_t o = 1; o <= 8; o++) {
        fx_opts opt;
        size_t n;
        uint8_t *f;
        pc_image_meta m;
        memset(&opt, 0, sizeof opt);
        opt.orientation = o;
        f = make_jxl(src, NULL, W, H, &opt, &n);
        CHECK(f != NULL);
        if (!f) continue;
        {
            pc_doc *r = load_ok(f, n, &m);
            uint32_t ow = o > 4 ? H : W, oh = o > 4 ? W : H;
            if (r) {
                pc_px32 *q = doc_px(r);
                size_t bad = 0;
                CHECK(r->w == ow && r->h == oh);
                if (r->w == ow && r->h == oh)
                    for (uint32_t y = 0; y < oh; y++)
                        for (uint32_t x = 0; x < ow; x++)
                            if (!tu_px_eq(q[(size_t)y * ow + x], oriented(src, W, H, o, x, y)))
                                bad++;
                CHECK(bad == 0u);
                if (bad) INFO("orientation %u: %zu wrong", o, bad);
                free(q);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        free(f);
    }
    free(src);
}

static void t_animation_boxes(void)
{
    const uint32_t W = 24, H = 16;
    pc_px32 *src = tu_noise(W, H, 0);
    fx_opts opt;
    size_t n;
    uint8_t *f;
    pc_image_meta m;
    memset(&opt, 0, sizeof opt);
    opt.orientation = 1;
    opt.frames = 3;
    opt.boxes = true;
    f = make_jxl(src, NULL, W, H, &opt, &n);
    CHECK(f != NULL);
    if (f) {
        pc_doc *r = load_ok(f, n, &m);
        if (r) {
            pc_px32 *q = doc_px(r);
            uint8_t *e;
            size_t el = 0;
            CHECK(tu_diff(q, src, (size_t)W * H) == 0);              /* frame 0 */
            CHECK(strstr(m.note, "first frame") != NULL);
            /* Brotli-compressed Exif and XMP boxes */
            e = axj_meta_get_exif(&m, &el);
            CHECK(e && el == 78u && e[18] == 1);
            CHECK(pc_meta_get(&m, "xmp") && strstr(pc_meta_get(&m, "xmp"), "<b/>") != NULL);
            CHECK(fabs(m.dpi_x - 254.0) < 1e-6);
            free(e);
            free(q);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(f);
    }
    free(src);
}

static uint16_t pq_signal(double nits)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    double y = pow(nits / 10000.0, m1);
    return (uint16_t)(pow((c1 + c2 * y) / (1.0 + c3 * y), m2) * 65535.0 + 0.5);
}

static void t_high_bit_depth(void)
{
    const uint32_t W = 16, H = 4;
    uint16_t *px16 = (uint16_t *)malloc((size_t)W * H * 8u);
    fx_opts opt;
    size_t n;
    uint8_t *f;
    pc_image_meta m;
    for (uint32_t i = 0; i < W * H; i++) {
        uint16_t v = (uint16_t)(i * 65535u / (W * H - 1u));
        px16[i * 4] = v;
        px16[i * 4 + 1] = (uint16_t)(65535u - v);
        px16[i * 4 + 2] = (uint16_t)(v / 2u);
        px16[i * 4 + 3] = 65535u;
    }
    memset(&opt, 0, sizeof opt);
    opt.orientation = 1;
    opt.u16 = true;
    f = make_jxl(NULL, px16, W, H, &opt, &n);
    CHECK(f != NULL);
    if (f) {
        pc_doc *r = load_ok(f, n, &m);
        if (r) {
            pc_px32 *q = doc_px(r);
            int worst = 0;
            for (uint32_t i = 0; i < W * H; i++) {
                int d0 = abs((int)q[i].r - (int)u16_to_u8(px16[i * 4]));
                int d1 = abs((int)q[i].g - (int)u16_to_u8(px16[i * 4 + 1]));
                int d2 = abs((int)q[i].b - (int)u16_to_u8(px16[i * 4 + 2]));
                if (d0 > worst) worst = d0;
                if (d1 > worst) worst = d1;
                if (d2 > worst) worst = d2;
            }
            CHECK(worst <= 1);
            CHECK(m.src_bits == 16u && m.icc == NULL && m.note[0] == '\0');
            free(q);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(f);
    }
    /* PQ, lossless and lossy (XYB): 203 cd/m2 is near white, 0 is black */
    for (int lossy = 0; lossy < 2; lossy++) {
        for (uint32_t i = 0; i < W * H; i++) {
            uint16_t v = i < 8u ? 0u : pq_signal(203.0);
            px16[i * 4] = px16[i * 4 + 1] = px16[i * 4 + 2] = v;
            px16[i * 4 + 3] = 65535u;
        }
        opt.pq = true;
        opt.lossy = lossy != 0;
        f = make_jxl(NULL, px16, W, H, &opt, &n);
        CHECK(f != NULL);
        if (!f) continue;
        {
            pc_doc *r = load_ok(f, n, &m);
            if (r) {
                pc_px32 *q = doc_px(r);
                CHECK(q[0].r <= 2 && q[0].g <= 2);
                CHECK(q[40].r >= 232 && q[40].r <= 252);
                CHECK(abs((int)q[40].r - (int)q[40].b) <= 2);
                CHECK(strstr(m.note, "HDR") != NULL && m.icc == NULL);
                INFO("PQ %s: black %u, diffuse white %u", lossy ? "lossy" : "lossless",
                     q[0].r, q[40].r);
                free(q);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
        }
        free(f);
    }
    free(px16);
}

static void t_limits_fuzz(void)
{
    pc_px32 *a = tu_photo(64, 48, true);
    pc_doc *d = tu_doc_from_px(64, 48, a), *r = NULL;
    jxl_params_t p = defaults();
    pc_codec_limits lim;
    pc_image_meta m, in;
    pc_buf out;
    uint8_t tiff[80];
    char *b64 = NULL;
    memset(&in, 0, sizeof in);
    axj_b64_encode(tiff, exif_block(tiff, 1), &b64);
    pc_meta_add(&in, "exif", b64);
    free(b64);
    p.quality = 70;
    if (save_doc(d, &in, &p, NULL, &out)) {
        pc_codec_limits_default(&lim);
        lim.max_h = 47;
        CHECK(jx()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL);
        pc_codec_limits_default(&lim);
        lim.max_mem = 30000;           /* buffers and document do not fit */
        CHECK(jx()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL);
        /* they fit, but libjxl's working memory (counted by the memory
         * manager; libjxl before 0.9 allocates most of it elsewhere) does not */
        lim.max_mem = 64u * 48u * 12u + 64u;
        {
            pc_status st = jx()->load(out.p, out.n, &lim, &r, &m);
#if JPEGXL_MAJOR_VERSION > 0 || JPEGXL_MINOR_VERSION >= 9
            CHECK(st == PC_ERR_LIMIT && r == NULL);
#else
            CHECK((st == PC_ERR_LIMIT && r == NULL) || (st == PC_OK && r != NULL));
#endif
            if (r) { pc_doc_destroy(r); pc_meta_free(&m); r = NULL; }
        }
        lim.max_mem = (uint64_t)64 << 20;
        CHECK(jx()->load(out.p, out.n, &lim, &r, &m) == PC_OK && r != NULL);
        if (r) { pc_doc_destroy(r); pc_meta_free(&m); r = NULL; }
        CHECK(jx()->load(out.p, out.n / 2u, NULL, &r, &m) != PC_OK && r == NULL);
        CHECK(jx()->load(out.p, 13, NULL, &r, &m) != PC_OK && r == NULL);
        {
            uint32_t ok = tu_fuzz_codec(jx(), out.p, out.n, g_quick ? 150u : 1500u, NULL);
            INFO("fuzz: %u of %u mutations decoded", ok, g_quick ? 150u : 1500u);
        }
        pc_buf_free(&out);
    }
    /* a bare codestream: limits apply from its basic info */
    {
        JxlEncoder *enc = JxlEncoderCreate(NULL);
        JxlBasicInfo bi;
        JxlColorEncoding ce;
        JxlEncoderFrameSettings *fs;
        JxlPixelFormat pf = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
        uint8_t buf[4096], *next = buf, rgba[4];
        size_t avail = sizeof buf;
        double t0 = pc_test_now();
        JxlEncoderInitBasicInfo(&bi);
        bi.xsize = 1; bi.ysize = 1; bi.bits_per_sample = 8; bi.num_color_channels = 3;
        bi.num_extra_channels = 1; bi.alpha_bits = 8; bi.uses_original_profile = JXL_TRUE;
        JxlEncoderSetBasicInfo(enc, &bi);
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
        JxlEncoderSetColorEncoding(enc, &ce);
        fs = JxlEncoderFrameSettingsCreate(enc, NULL);
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
        rgba[0] = 1; rgba[1] = 2; rgba[2] = 3; rgba[3] = 255;
        JxlEncoderAddImageFrame(fs, &pf, rgba, 4);
        JxlEncoderCloseInput(enc);
        CHECK(JxlEncoderProcessOutput(enc, &next, &avail) == JXL_ENC_SUCCESS);
        JxlEncoderDestroy(enc);
        {
            pc_image_meta mm;
            pc_doc *dd = NULL;
            size_t len = (size_t)(next - buf);
            CHECK(jx()->load(buf, len, NULL, &dd, &mm) == PC_OK);
            if (dd) { pc_doc_destroy(dd); pc_meta_free(&mm); }
            pc_codec_limits_default(&lim);
            lim.max_pixels = 0;            /* nothing fits */
            CHECK(jx()->load(buf, len, &lim, &dd, &mm) == PC_ERR_LIMIT && dd == NULL);
        }
        CHECK(pc_test_now() - t0 < 5.0);
    }
    pc_meta_free(&in);
    free(a);
    pc_doc_destroy(d);
}
#endif /* PC_HAVE_JXL */

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_registry);
#if defined(PC_HAVE_JXL)
    RUN(t_lossless);
    RUN(t_lossy);
    RUN(t_parallel_runner);
    RUN(t_metadata);
    RUN(t_orientation);
    RUN(t_animation_boxes);
    RUN(t_high_bit_depth);
    RUN(t_limits_fuzz);
#endif
    return pc_test_finish();
}
