/* test_lib_icc.c - pc_icc.h: sRGB profile, inspection, RGB and gray
 * conversion checked against independent matrix math, documents (edge
 * padding, thread-count independence), the import step, malformed and
 * fuzzed profiles, and CMYK JPEGs with an embedded CMYK profile. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "pc/pc_icc.h"

#include <stdio.h>
#include "lcms2.h"
#include "jpeglib.h"

static uint8_t *save_profile(cmsHPROFILE h, size_t *len)
{
    cmsUInt32Number n = 0;
    uint8_t *p = NULL;
    bool ok = cmsSaveProfileToMem(h, NULL, &n) && n > 0;
    CHECK(ok);
    if (ok) {
        p = (uint8_t *)malloc(n);
        ok = cmsSaveProfileToMem(h, p, &n) != 0;
        CHECK(ok);
        if (!ok) { free(p); p = NULL; }
    }
    *len = ok ? n : 0;
    cmsCloseProfile(h);
    return p;
}

/* Adobe RGB (1998)-like profile: D65, gamma 563/256. */
static uint8_t *make_adobe(size_t *len)
{
    cmsCIExyY wp = { 0.3127, 0.3290, 1.0 };
    cmsCIExyYTRIPLE pri = { { 0.64, 0.33, 1.0 }, { 0.21, 0.71, 1.0 }, { 0.15, 0.06, 1.0 } };
    cmsToneCurve *g = cmsBuildGamma(NULL, 563.0 / 256.0);
    cmsToneCurve *c3[3];
    cmsHPROFILE h;
    c3[0] = c3[1] = c3[2] = g;
    h = cmsCreateRGBProfile(&wp, &pri, c3);
    cmsFreeToneCurve(g);
    return save_profile(h, len);
}

static double srgb_enc(double v)
{
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/* Independent reference: Adobe RGB -> XYZ (D65) -> linear sRGB. */
static void adobe_ref(const uint8_t in[3], double out[3])
{
    static const double A[3][3] = {
        { 0.5767309, 0.1855540, 0.1881852 },
        { 0.2973769, 0.6273491, 0.0752741 },
        { 0.0270343, 0.0706872, 0.9911085 } };
    static const double S[3][3] = {
        { 3.2404542, -1.5371385, -0.4985314 },
        { -0.9692660, 1.8760108, 0.0415560 },
        { 0.0556434, -0.2040259, 1.0572252 } };
    double lin[3], xyz[3];
    for (int c = 0; c < 3; c++) lin[c] = pow(in[c] / 255.0, 563.0 / 256.0);
    for (int r = 0; r < 3; r++) xyz[r] = A[r][0] * lin[0] + A[r][1] * lin[1] + A[r][2] * lin[2];
    for (int r = 0; r < 3; r++)
        out[r] = 255.0 * srgb_enc(S[r][0] * xyz[0] + S[r][1] * xyz[1] + S[r][2] * xyz[2]);
}

static void t_srgb_profile(void)
{
    uint8_t *a = NULL, *b = NULL;
    size_t an = 0, bn = 0;
    pc_icc_info info;
    pc_image_meta m;
    CHECK(pc_icc_srgb_profile(&a, &an) == PC_OK && a && an > 132);
    CHECK(pc_icc_srgb_profile(&b, &bn) == PC_OK && an == bn && memcmp(a, b, an) == 0);
    CHECK(pc_icc_inspect(a, an, &info) == PC_OK);
    CHECK(info.space == PC_ICC_SPACE_RGB && info.is_srgb && info.desc[0] != 0);
    CHECK(info.device_class == 0x6D6E7472u);                /* 'mntr' */
    memset(&m, 0, sizeof m);
    CHECK(pc_icc_meta_set_srgb(&m) == PC_OK && m.icc_len == an && memcmp(m.icc, a, an) == 0);
    pc_meta_free(&m);
    free(a);
    free(b);
}

static void t_rgb_conversion(void)
{
    size_t n;
    uint8_t *adobe = make_adobe(&n);
    pc_icc_info info;
    pc_px32 px[512];
    int worst = 0;
    CHECK(pc_icc_inspect(adobe, n, &info) == PC_OK);
    CHECK(info.space == PC_ICC_SPACE_RGB && !info.is_srgb);
    for (int i = 0; i < 512; i++) {
        px[i] = tu_px(rnd8(), rnd8(), rnd8(), (uint8_t)(i % 7 == 0 ? 0 : rnd8()));
        if (px[i].a == 0) px[i].r = 9;        /* hidden color must come out zero */
    }
    {
        pc_px32 orig[512];
        memcpy(orig, px, sizeof px);
        CHECK(pc_icc_to_srgb_px(adobe, n, px, 32, 16, 32) == PC_OK);
        for (int i = 0; i < 512; i++) {
            uint8_t in[3];
            double ref[3];
            CHECK(px[i].a == orig[i].a);
            if (orig[i].a == 0) { CHECK(px[i].r == 0 && px[i].g == 0 && px[i].b == 0); continue; }
            in[0] = orig[i].r; in[1] = orig[i].g; in[2] = orig[i].b;
            adobe_ref(in, ref);
            {
                int d0 = abs((int)px[i].r - (int)(ref[0] + 0.5));
                int d1 = abs((int)px[i].g - (int)(ref[1] + 0.5));
                int d2 = abs((int)px[i].b - (int)(ref[2] + 0.5));
                int dm = d0 > d1 ? d0 : d1;
                if (d2 > dm) dm = d2;
                if (dm > worst) worst = dm;
            }
        }
    }
    CHECK(worst <= 3);
    INFO("adobe -> sRGB worst difference to the matrix reference: %d", worst);
    free(adobe);
}

static void t_gray(void)
{
    cmsToneCurve *lin = cmsBuildGamma(NULL, 1.0);
    cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), lin);
    size_t n;
    uint8_t *gray = save_profile(h, &n);
    pc_icc_info info;
    pc_px32 px[256];
    cmsFreeToneCurve(lin);
    CHECK(pc_icc_inspect(gray, n, &info) == PC_OK && info.space == PC_ICC_SPACE_GRAY);
    CHECK(!info.is_srgb);
    for (int i = 0; i < 256; i++) px[i] = tu_px((uint8_t)i, (uint8_t)i, (uint8_t)i, 255);
    CHECK(pc_icc_to_srgb_px(gray, n, px, 256, 1, 256) == PC_OK);
    {
        int worst = 0;
        for (int i = 0; i < 256; i++) {
            int e = (int)(255.0 * srgb_enc(i / 255.0) + 0.5), d = abs((int)px[i].g - e);
            CHECK(px[i].r == px[i].g && px[i].g == px[i].b);
            if (d > worst) worst = d;
        }
        CHECK(worst <= 2);
    }
    free(gray);
}

/* pc_par that runs jobs backwards with fake worker ids. */
static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, i % 3u);
}

static void t_document_and_import(void)
{
    size_t n;
    uint8_t *adobe = make_adobe(&n);
    const uint32_t W = 130, H = 70;
    pc_px32 *a = tu_photo(W, H, true), *b = tu_noise(W, H, 1);
    pc_doc *d1 = tu_doc_from_px(W, H, a), *d2 = tu_doc_from_px(W, H, a);
    pc_par par;
    pc_image_meta m;
    tu_doc_add_layer(d1, b, PC_BLEND_SCREEN, 200, true, "x");
    tu_doc_add_layer(d2, b, PC_BLEND_SCREEN, 200, true, "x");
    par.run = rev_run; par.self = NULL; par.threads = 3;
    CHECK(pc_icc_to_srgb_doc(d1, adobe, n, NULL) == PC_OK);
    CHECK(pc_icc_to_srgb_doc(d2, adobe, n, &par) == PC_OK);
    CHECK(pc_doc_fingerprint(d1) == pc_doc_fingerprint(d2));
    CHECK(pc_doc_edge_padding_is_zero(d1));
    {   /* converted pixels equal the per-pixel API */
        pc_px32 *l0 = tu_layer_px(d1, d1->stack[0]), *ref = (pc_px32 *)malloc((size_t)W * H * 4);
        memcpy(ref, a, (size_t)W * H * 4);
        CHECK(pc_icc_to_srgb_px(adobe, n, ref, (int32_t)W, (int32_t)H, W) == PC_OK);
        CHECK(tu_diff(l0, ref, (size_t)W * H) == 0);
        free(l0); free(ref);
    }
    pc_doc_destroy(d1);
    pc_doc_destroy(d2);
    /* published tiles (refs > 1) are refused, untouched */
    d1 = tu_doc_from_px(W, H, a);
    {
        pc_layer *dup = pc_layer_duplicate(d1, d1->stack[0]);
        uint64_t fp = pc_doc_fingerprint(d1);
        CHECK(pc_icc_to_srgb_doc(d1, adobe, n, NULL) == PC_ERR_STATE);
        CHECK(pc_doc_fingerprint(d1) == fp);
        pc_layer_destroy(dup);
    }
    /* import: sRGB profile is dropped without touching pixels */
    {
        uint64_t fp = pc_doc_fingerprint(d1);
        memset(&m, 0, sizeof m);
        CHECK(pc_icc_meta_set_srgb(&m) == PC_OK);
        CHECK(pc_icc_import(d1, &m, NULL) == PC_OK);
        CHECK(m.icc == NULL && pc_doc_fingerprint(d1) == fp);
        /* Adobe: converted, profile dropped, note set */
        m.icc = (uint8_t *)malloc(n);
        memcpy(m.icc, adobe, n);
        m.icc_len = n;
        CHECK(pc_icc_import(d1, &m, NULL) == PC_OK);
        CHECK(m.icc == NULL && pc_doc_fingerprint(d1) != fp && strstr(m.note, "Converted") != NULL);
        /* damaged: error, pixels and profile kept */
        fp = pc_doc_fingerprint(d1);
        m.icc = (uint8_t *)malloc(n);
        memcpy(m.icc, adobe, n);
        m.icc[36] = 'x';                                  /* 'acsp' signature */
        m.icc_len = n;
        CHECK(pc_icc_import(d1, &m, NULL) == PC_ERR_FORMAT);
        CHECK(m.icc != NULL && pc_doc_fingerprint(d1) == fp && m.note[0] != 0);
        pc_meta_free(&m);
        CHECK(pc_icc_import(d1, &m, NULL) == PC_OK);      /* nothing to do */
    }
    pc_doc_destroy(d1);
    free(a); free(b);
    free(adobe);
}

static void t_malformed(void)
{
    size_t n;
    uint8_t *adobe = make_adobe(&n), *buf = (uint8_t *)malloc(n + 64);
    pc_icc_info info;
    pc_px32 px[64], orig[64];
    uint32_t ok = 0, iters = g_quick ? 2000u : 20000u;
    uint8_t small[132];
    memset(small, 0, sizeof small);
    CHECK(pc_icc_inspect(NULL, 0, &info) == PC_ERR_ARG);
    CHECK(pc_icc_inspect(adobe, 100, &info) == PC_ERR_FORMAT);
    CHECK(pc_icc_inspect(small, PC_ICC_MAX_BYTES + 1u, &info) == PC_ERR_LIMIT);
    memcpy(buf, adobe, n);
    buf[0] = 0x7F;                                       /* size beyond the buffer */
    CHECK(pc_icc_inspect(buf, n, &info) == PC_ERR_FORMAT);
    memcpy(buf, adobe, n);
    buf[128] = 0x10;                                     /* tag count far too large */
    CHECK(pc_icc_inspect(buf, n, &info) == PC_ERR_FORMAT);
    memcpy(buf, adobe, n);
    buf[132 + 4] = 0x7F;                                 /* first tag offset out of range */
    CHECK(pc_icc_inspect(buf, n, &info) == PC_ERR_FORMAT);
    for (int i = 0; i < 64; i++) orig[i] = tu_px(rnd8(), rnd8(), rnd8(), 255);
    for (uint32_t it = 0; it < iters; it++) {
        size_t len = tu_mutate(adobe, n, buf, n + 64);
        pc_status st;
        memcpy(px, orig, sizeof px);
        (void)pc_icc_inspect(buf, len, &info);
        st = pc_icc_to_srgb_px(buf, len, px, 8, 8, 8);
        if (st == PC_OK) ok++;
        else CHECK(tu_diff(px, orig, 64) == 0);           /* untouched on failure */
    }
    INFO("icc fuzz: %u of %u mutated profiles converted", ok, iters);
    free(buf);
    free(adobe);
}

/* CMYK profile whose AToB0 table is the naive CMYK -> sRGB formula, so the
 * managed and the naive decodes of a CMYK JPEG must agree closely. */
static int naive_sampler(CMSREGISTER const cmsUInt16Number in[], CMSREGISTER cmsUInt16Number out[],
                         CMSREGISTER void *cargo)
{
    cmsHTRANSFORM srgb_to_lab = (cmsHTRANSFORM)cargo;
    double c = in[0] / 65535.0, m = in[1] / 65535.0, y = in[2] / 65535.0, k = in[3] / 65535.0;
    uint8_t rgb[3];
    cmsCIELab lab;
    rgb[0] = (uint8_t)((1 - c) * (1 - k) * 255.0 + 0.5);
    rgb[1] = (uint8_t)((1 - m) * (1 - k) * 255.0 + 0.5);
    rgb[2] = (uint8_t)((1 - y) * (1 - k) * 255.0 + 0.5);
    cmsDoTransform(srgb_to_lab, rgb, &lab, 1);
    cmsFloat2LabEncoded(out, &lab);
    return 1;
}

static uint8_t *make_cmyk(size_t *len)
{
    cmsHPROFILE p = cmsCreateProfilePlaceholder(NULL), srgb = cmsCreate_sRGBProfile();
    cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
    cmsHTRANSFORM t = cmsCreateTransform(srgb, TYPE_RGB_8, lab, TYPE_Lab_DBL, INTENT_PERCEPTUAL, 0);
    cmsPipeline *pl = cmsPipelineAlloc(NULL, 4, 3);
    cmsStage *clut = cmsStageAllocCLut16bit(NULL, 9, 4, 3, NULL);
    cmsMLU *desc = cmsMLUalloc(NULL, 1);
    cmsSetProfileVersion(p, 2.1);       /* lut16: a bare CLUT is enough */
    cmsSetDeviceClass(p, cmsSigOutputClass);
    cmsSetColorSpace(p, cmsSigCmykData);
    cmsSetPCS(p, cmsSigLabData);
    CHECK(cmsStageSampleCLut16bit(clut, naive_sampler, t, 0));
    CHECK(cmsPipelineInsertStage(pl, cmsAT_END, clut));
    CHECK(cmsWriteTag(p, cmsSigAToB0Tag, pl));
    cmsMLUsetASCII(desc, "en", "US", "naive cmyk");
    cmsWriteTag(p, cmsSigProfileDescriptionTag, desc);
    cmsWriteTag(p, cmsSigMediaWhitePointTag, cmsD50_XYZ());
    cmsMLUfree(desc);
    cmsPipelineFree(pl);
    cmsDeleteTransform(t);
    cmsCloseProfile(lab);
    cmsCloseProfile(srgb);
    return save_profile(p, len);
}

static uint8_t *cmyk_jpeg(const uint8_t *cmyk, uint32_t w, uint32_t h, const uint8_t *icc,
                          size_t icc_len, bool adobe, unsigned long *len)
{
    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr em;
    unsigned char *buf = NULL;
    ci.err = jpeg_std_error(&em);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &buf, len);
    ci.image_width = w; ci.image_height = h; ci.input_components = 4;
    ci.in_color_space = JCS_CMYK;
    jpeg_set_defaults(&ci);
    jpeg_set_colorspace(&ci, JCS_YCCK);
    jpeg_set_quality(&ci, 100, TRUE);
    for (int c = 0; c < 4; c++) ci.comp_info[c].h_samp_factor = ci.comp_info[c].v_samp_factor = 1;
    ci.write_Adobe_marker = adobe ? TRUE : FALSE;
    jpeg_start_compress(&ci, TRUE);
    if (icc) jpeg_write_icc_profile(&ci, icc, (unsigned)icc_len);
    while (ci.next_scanline < h) {
        JSAMPROW row = (JSAMPROW)(cmyk + (size_t)ci.next_scanline * w * 4);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    return buf;
}

static void t_cmyk_jpeg(void)
{
    const uint32_t W = 32, H = 32;
    size_t pn;
    uint8_t *prof = make_cmyk(&pn), *cmyk = (uint8_t *)malloc((size_t)W * H * 4);
    const pc_codec *jpg = pc_codec_by_id("jpeg");
    pc_icc_info info;
    CHECK(prof != NULL && pn > 132);
    if (!prof || pn <= 132) { free(prof); free(cmyk); return; }
    CHECK(pc_icc_inspect(prof, pn, &info) == PC_OK && info.space == PC_ICC_SPACE_CMYK);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = cmyk + ((size_t)y * W + x) * 4;
            /* Adobe files store inverted samples: 255 = no ink */
            p[0] = (uint8_t)(255 - x * 6); p[1] = (uint8_t)(255 - y * 6);
            p[2] = (uint8_t)(200 + (x ^ y)); p[3] = (uint8_t)(255 - (x + y));
        }
    {
        unsigned long n1 = 0, n2 = 0;
        uint8_t *with = cmyk_jpeg(cmyk, W, H, prof, pn, true, &n1);
        uint8_t *without = cmyk_jpeg(cmyk, W, H, NULL, 0, true, &n2);
        pc_doc *a = NULL, *b = NULL;
        pc_image_meta ma, mb;
        CHECK(jpg->load(with, n1, NULL, &a, &ma) == PC_OK);
        CHECK(jpg->load(without, n2, NULL, &b, &mb) == PC_OK);
        if (a && b) {
            /* FL-CMYK: the managed decode is Adobe RGB (1998) and tagged with
             * it; seen through that profile it matches the naive decode */
            pc_px32 *pa = tu_layer_px(a, a->stack[0]), *pb = tu_layer_px(b, b->stack[0]);
            int md;
            CHECK(ma.icc != NULL && strstr(ma.note, "Adobe RGB") != NULL);
            CHECK(ma.icc && pc_icc_inspect(ma.icc, ma.icc_len, &info) == PC_OK &&
                  info.space == PC_ICC_SPACE_RGB && strcmp(info.desc, "Adobe RGB (1998)") == 0);
            if (ma.icc)
                CHECK(pc_icc_to_srgb_px(ma.icc, ma.icc_len, pa, (int32_t)W, (int32_t)H, W) ==
                      PC_OK);
            md = tu_max_abs_diff(pa, pb, (size_t)W * H);
            CHECK(md <= 8);
            INFO("managed (via Adobe RGB) vs naive CMYK decode: max difference %d", md);
            CHECK(mb.icc == NULL && strstr(mb.note, "without") != NULL);
            free(pa); free(pb);
        }
        pc_doc_destroy(a); pc_doc_destroy(b);
        pc_meta_free(&ma); pc_meta_free(&mb);
        free(with); free(without);
    }
    free(cmyk);
    free(prof);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_srgb_profile);
    RUN(t_rgb_conversion);
    RUN(t_gray);
    RUN(t_document_and_import);
    RUN(t_malformed);
    RUN(t_cmyk_jpeg);
    return pc_test_finish();
}
