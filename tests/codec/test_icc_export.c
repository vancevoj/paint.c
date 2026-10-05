/* test_icc_export.c - lane CODEC, wave 4: the color profile functions
 * behind F-FILE-FS-ICC, F-FILE-FL-ICC and F-FILE-FL-CMYK (the end to end
 * regressions of those audit items are in test_icc_audit.c).
 *
 *  - pc_icc_usable_space: real profiles, junk, a damaged PCS ('SYZ ') that
 *    still parses, a device link; pc_icc_meta_validate drops the damaged
 *    one with a note;
 *  - pc_icc_gray_as_rgb: gray pixels convert exactly like through the gray
 *    profile (XYZ PCS with a copied curve, Lab PCS with a sampled one), the
 *    sRGB curve gives sRGB, description and copyright;
 *  - colored pixels of an image with a gray profile go through the RGB form
 *    (pc_icc_to_srgb_px and the xform), gray ones keep the gray table;
 *  - pc_icc_embed_for decisions;
 *  - the default CMYK profile (pc_icc_cmyk_default_profile) through JPEG,
 *    8-bit TIFF and CMYK64 TIFF against cmyk_ref.h, and the per-color cache
 *    of the CMYK transform against the reference on images with few and
 *    with many colors;
 *  - mutated gray, RGB and CMYK profiles through all of these (fuzz).
 * Fixed seeds; profiles are made with Little-CMS. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "icc_test_util.h"
#include "cmyk_ref.h"
#include "meta_test_util.h"

#include <stdio.h>
#include "jpeglib.h"

static const pc_codec *codec(const char *id) { return pc_codec_by_id(id); }


static pc_icc_space space_of(const uint8_t *icc, size_t len)
{
    pc_icc_info info;
    if (!icc || pc_icc_inspect(icc, len, &info) != PC_OK) return PC_ICC_SPACE_OTHER;
    return info.space;
}

/* ---- usability --------------------------------------------------------------------------- */
static void t_usable_space(void)
{
    size_t n = 0, cn = 0;
    uint8_t *srgb = NULL, *adobe = NULL, *gray = itu_gray(2.2, "gray 2.2", &n);
    uint8_t *bad, *junk = itu_junk(600u, 1u);
    const uint8_t *cmyk = pc_icc_cmyk_default_profile(&cn);
    size_t sn = 0, an = 0, bn = 0;
    pc_icc_info info;
    pc_image_meta m;
    CHECK(pc_icc_srgb_profile(&srgb, &sn) == PC_OK);
    CHECK(pc_icc_adobe_rgb_profile(&adobe, &an) == PC_OK);
    CHECK(pc_icc_usable_space(srgb, sn) == PC_ICC_SPACE_RGB);
    CHECK(pc_icc_usable_space(adobe, an) == PC_ICC_SPACE_RGB);
    CHECK(gray && pc_icc_usable_space(gray, n) == PC_ICC_SPACE_GRAY);
    CHECK(cmyk && cn > 100000u && pc_icc_usable_space(cmyk, cn) == PC_ICC_SPACE_CMYK);
    CHECK(pc_icc_usable_space(junk, 600u) == PC_ICC_SPACE_OTHER);
    CHECK(pc_icc_usable_space(NULL, 0u) == PC_ICC_SPACE_OTHER);
    CHECK(pc_icc_usable_space(srgb, 0u) == PC_ICC_SPACE_OTHER);
    /* a damaged PCS passes the structural check but is not usable */
    bad = itu_bad_pcs(&bn);
    CHECK(bad && pc_icc_inspect(bad, bn, &info) == PC_OK && info.space == PC_ICC_SPACE_RGB);
    CHECK(bad && pc_icc_usable_space(bad, bn) == PC_ICC_SPACE_OTHER);
    /* the open-time check drops it with a note (it used to stay, FL-ICC) */
    memset(&m, 0, sizeof m);
    m.icc = (uint8_t *)malloc(bn);
    if (m.icc && bad) {
        memcpy(m.icc, bad, bn);
        m.icc_len = bn;
    }
    CHECK(pc_icc_meta_validate(&m, NULL));
    CHECK(m.icc == NULL && m.icc_len == 0u && strstr(m.note, "cannot be used") != NULL);
    pc_meta_free(&m);
    /* a usable profile stays */
    memset(&m, 0, sizeof m);
    m.icc = (uint8_t *)malloc(an);
    if (m.icc) {
        memcpy(m.icc, adobe, an);
        m.icc_len = an;
    }
    CHECK(!pc_icc_meta_validate(&m, NULL) && m.icc != NULL);
    pc_meta_free(&m);
    {   /* a device link is not an image profile */
        uint8_t *link = adobe ? (uint8_t *)malloc(an) : NULL;
        if (link) {
            memcpy(link, adobe, an);
            memcpy(link + 12, "link", 4u);
            CHECK(pc_icc_usable_space(link, an) == PC_ICC_SPACE_OTHER);
        }
        free(link);
    }
    free(srgb); free(adobe); free(gray); free(bad); free(junk);
}

/* ---- gray profiles as RGB ------------------------------------------------------------------- */
/* Largest difference (16-bit sRGB) between v through the gray profile and
 * (v, v, v) through the RGB form, both converted by Little-CMS unoptimized. */
static int gray_rgb_maxdiff(const uint8_t *gray, size_t gn, const uint8_t *rgb, size_t rn)
{
    cmsHPROFILE g = cmsOpenProfileFromMem(gray, (cmsUInt32Number)gn);
    cmsHPROFILE r = cmsOpenProfileFromMem(rgb, (cmsUInt32Number)rn);
    cmsHPROFILE s1 = cmsCreate_sRGBProfile(), s2 = cmsCreate_sRGBProfile();
    cmsHTRANSFORM tg = NULL, tr = NULL;
    uint8_t in[256], in3[256 * 3];
    uint16_t og[256 * 3], orr[256 * 3];
    int md = 1 << 20;
    if (g && s1)
        tg = cmsCreateTransform(g, TYPE_GRAY_8, s1, TYPE_RGB_16, INTENT_PERCEPTUAL,
                                cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (r && s2)
        tr = cmsCreateTransform(r, TYPE_RGB_8, s2, TYPE_RGB_16, INTENT_PERCEPTUAL,
                                cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (tg && tr) {
        for (int i = 0; i < 256; i++) {
            in[i] = (uint8_t)i;
            in3[3 * i] = in3[3 * i + 1] = in3[3 * i + 2] = (uint8_t)i;
        }
        cmsDoTransform(tg, in, og, 256);
        cmsDoTransform(tr, in3, orr, 256);
        md = 0;
        for (int i = 0; i < 256 * 3; i++) {
            int d = abs((int)og[i] - (int)orr[i]);
            if (d > md) md = d;
        }
    }
    if (tg) cmsDeleteTransform(tg);
    if (tr) cmsDeleteTransform(tr);
    if (g) cmsCloseProfile(g);
    if (r) cmsCloseProfile(r);
    if (s1) cmsCloseProfile(s1);
    if (s2) cmsCloseProfile(s2);
    return md;
}

static void t_gray_as_rgb(void)
{
    size_t gn = 0, rn = 0, rn2 = 0;
    uint8_t *gray = itu_gray(1.8, "Dot gray 1.8", &gn), *rgb = NULL, *rgb2 = NULL;
    pc_icc_info info;
    int md;
    CHECK(gray != NULL);
    if (!gray) return;
    CHECK(pc_icc_gray_as_rgb(gray, gn, &rgb, &rn) == PC_OK && rgb && rn > 132u);
    CHECK(rgb && pc_icc_inspect(rgb, rn, &info) == PC_OK && info.space == PC_ICC_SPACE_RGB);
    CHECK(strcmp(info.desc, "Dot gray 1.8 (RGB)") == 0);
    CHECK(rgb && pc_icc_usable_space(rgb, rn) == PC_ICC_SPACE_RGB);
    /* deterministic (creation date zeroed) */
    CHECK(pc_icc_gray_as_rgb(gray, gn, &rgb2, &rn2) == PC_OK && rn2 == rn &&
          rgb2 && memcmp(rgb, rgb2, rn) == 0);
    md = rgb ? gray_rgb_maxdiff(gray, gn, rgb, rn) : 99999;
    INFO("gray 1.8: gray profile vs its RGB form, max 16-bit difference %d", md);
    CHECK(md <= 16);                         /* well below one 8-bit code (257) */
    free(rgb); free(rgb2);
    rgb = NULL;
    {   /* a gray profile with the sRGB curve becomes sRGB */
        cmsFloat64Number par[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
        cmsToneCurve *c = cmsBuildParametricToneCurve(NULL, 4, par);
        cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), c);
        size_t sn = 0;
        uint8_t *sg;
        if (h) cmsWriteTag(h, cmsSigProfileDescriptionTag, NULL);   /* no description */
        sg = itu_save(h, &sn);
        cmsFreeToneCurve(c);
        CHECK(sg && pc_icc_gray_as_rgb(sg, sn, &rgb, &rn) == PC_OK);
        CHECK(rgb && pc_icc_inspect(rgb, rn, &info) == PC_OK && info.is_srgb);
        CHECK(strcmp(info.desc, "Gray (RGB)") == 0);           /* no description */
        free(sg); free(rgb);
        rgb = NULL;
    }
    {   /* Lab PCS: the curve is L*, so the RGB form samples the transform */
        cmsToneCurve *c = cmsBuildGamma(NULL, 1.0);
        cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), c);
        size_t ln = 0;
        uint8_t *lg;
        cmsSetPCS(h, cmsSigLabData);
        lg = itu_save(h, &ln);
        cmsFreeToneCurve(c);
        CHECK(lg && pc_icc_usable_space(lg, ln) == PC_ICC_SPACE_GRAY);
        CHECK(lg && pc_icc_gray_as_rgb(lg, ln, &rgb, &rn) == PC_OK);
        md = rgb ? gray_rgb_maxdiff(lg, ln, rgb, rn) : 99999;
        INFO("Lab PCS gray: gray profile vs its RGB form, max 16-bit difference %d", md);
        CHECK(md <= 128);                    /* half an 8-bit code */
        free(lg); free(rgb);
        rgb = NULL;
    }
    CHECK(pc_icc_gray_as_rgb(gray, gn, NULL, &rn) == PC_ERR_ARG);
    {
        uint8_t *adobe = NULL;
        size_t an = 0;
        CHECK(pc_icc_adobe_rgb_profile(&adobe, &an) == PC_OK);
        CHECK(pc_icc_gray_as_rgb(adobe, an, &rgb, &rn) == PC_ERR_UNSUPPORTED && rgb == NULL);
        free(adobe);
    }
    free(gray);
}

/* Colored pixels of an image with a gray profile: the RGB form, not their
 * green channel (they used to turn gray); gray pixels: the gray table. */
static void t_gray_colored_pixels(void)
{
    size_t gn = 0, rn = 0;
    uint8_t *gray = itu_gray(1.8, "gray 1.8", &gn), *rgb = NULL;
    pc_px32 px[4], ref[4], g8[4];
    CHECK(gray && pc_icc_gray_as_rgb(gray, gn, &rgb, &rn) == PC_OK);
    if (!gray || !rgb) { free(gray); free(rgb); return; }
    px[0] = tu_px(200, 40, 40, 255);
    px[1] = tu_px(30, 160, 220, 128);
    px[2] = tu_px(90, 90, 90, 255);
    px[3] = tu_px(10, 250, 10, 0);
    memcpy(ref, px, sizeof px);
    memcpy(g8, px, sizeof px);
    CHECK(pc_icc_to_srgb_px(gray, gn, px, 4, 1, 4u) == PC_OK);
    CHECK(pc_icc_to_srgb_px(rgb, rn, ref, 4, 1, 4u) == PC_OK);
    for (int i = 0; i < 2; i++) {
        CHECK(px[i].r != px[i].g);                       /* still colored */
        CHECK(abs((int)px[i].r - ref[i].r) <= 1 && abs((int)px[i].g - ref[i].g) <= 1 &&
              abs((int)px[i].b - ref[i].b) <= 1 && px[i].a == ref[i].a);
    }
    {   /* the gray pixel: exactly the gray profile's own table */
        cmsHPROFILE g = cmsOpenProfileFromMem(gray, (cmsUInt32Number)gn);
        cmsHPROFILE s = cmsCreate_sRGBProfile();
        cmsHTRANSFORM t = cmsCreateTransform(g, TYPE_GRAY_8, s, TYPE_RGB_8, INTENT_PERCEPTUAL,
                                             cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
        uint8_t in = 90, out[3] = { 0, 0, 0 };
        if (t) cmsDoTransform(t, &in, out, 1);
        CHECK(t && px[2].r == out[0] && px[2].g == out[1] && px[2].b == out[2]);
        if (t) cmsDeleteTransform(t);
        cmsCloseProfile(g);
        cmsCloseProfile(s);
    }
    CHECK(px[3].r == 0 && px[3].g == 0 && px[3].b == 0 && px[3].a == 0);
    {   /* the display transform agrees */
        pc_icc_xform *x = NULL;
        bool ident = true;
        CHECK(pc_icc_xform_create(gray, gn, &x, &ident) == PC_OK && x && !ident);
        if (x) {
            pc_icc_xform_run(x, g8, 4u);
            CHECK(memcmp(g8, px, sizeof px) == 0);
        }
        pc_icc_xform_destroy(x);
    }
    free(gray);
    free(rgb);
}

/* ---- what to embed ------------------------------------------------------------------------- */
static void t_embed_for(void)
{
    size_t gn = 0, an = 0, bn = 0, cn = 0;
    uint8_t *gray = itu_gray(2.0, "embed gray", &gn), *adobe = NULL, *bad = itu_bad_pcs(&bn);
    uint8_t *junk = itu_junk(300u, 2u);
    const uint8_t *cmyk = pc_icc_cmyk_default_profile(&cn);
    pc_image_meta m;
    pc_icc_embed e;
    CHECK(pc_icc_adobe_rgb_profile(&adobe, &an) == PC_OK);
    memset(&m, 0, sizeof m);
    CHECK(pc_icc_embed_for(NULL, PC_ICC_SPACE_RGB, &e) == PC_OK && !e.icc && !e.owned);
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && !e.icc);
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_CMYK, &e) == PC_ERR_ARG && !e.icc);
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, NULL) == PC_ERR_ARG);
    /* RGB profile: itself for RGB pixels, nothing for gray samples */
    m.icc = adobe; m.icc_len = an;
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && e.icc == adobe &&
          e.len == an && !e.owned);
    pc_icc_embed_free(&e);
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_GRAY, &e) == PC_OK && !e.icc);
    /* gray profile: itself for gray samples, its RGB form for RGB pixels */
    m.icc = gray; m.icc_len = gn;
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_GRAY, &e) == PC_OK && e.icc == gray && !e.owned);
    pc_icc_embed_free(&e);
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && e.icc && e.owned &&
          e.icc == e.owned && space_of(e.icc, e.len) == PC_ICC_SPACE_RGB);
    {
        uint8_t *rgb = NULL;
        size_t rn = 0;
        CHECK(pc_icc_gray_as_rgb(gray, gn, &rgb, &rn) == PC_OK);
        CHECK(rgb && e.len == rn && memcmp(e.icc, rgb, rn) == 0);
        free(rgb);
    }
    pc_icc_embed_free(&e);
    CHECK(!e.icc && !e.owned && e.len == 0u);
    pc_icc_embed_free(NULL);
    /* never: CMYK, damaged, junk */
    m.icc = (uint8_t *)(uintptr_t)cmyk; m.icc_len = cn;
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && !e.icc);
    m.icc = bad; m.icc_len = bn;
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && !e.icc);
    m.icc = junk; m.icc_len = 300u;
    CHECK(pc_icc_embed_for(&m, PC_ICC_SPACE_RGB, &e) == PC_OK && !e.icc);
    free(gray); free(adobe); free(bad); free(junk);
}

/* ---- the default CMYK profile ------------------------------------------------------------ */
static uint8_t *jpeg_cmyk_file(const uint8_t *cmyk, uint32_t w, uint32_t h, unsigned long *len)
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
    jpeg_set_colorspace(&ci, JCS_CMYK);
    jpeg_set_quality(&ci, 100, TRUE);
    for (int c = 0; c < 4; c++) ci.comp_info[c].h_samp_factor = ci.comp_info[c].v_samp_factor = 1;
    ci.write_Adobe_marker = FALSE;           /* like Pillow: plain samples, no marker */
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < h) {
        JSAMPROW row = (JSAMPROW)(cmyk + (size_t)ci.next_scanline * w * 4);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    return buf;
}

/* Separated CMYK TIFF, 8 or 16 bits per sample, no profile. */
static void tiff_cmyk_file(pc_buf *b, const uint8_t *cmyk, uint32_t w, uint32_t h, int bits)
{
    tx_ent e[10];
    size_t bpp = (size_t)bits / 2u, sz;
    uint8_t *data = (uint8_t *)malloc((size_t)w * h * bpp);
    if (!data) return;
    for (size_t i = 0; i < (size_t)w * h * 4u; i++) {
        if (bits == 8) data[i] = cmyk[i];
        else { data[2 * i] = cmyk[i]; data[2 * i + 1] = cmyk[i]; }    /* v * 257, LE */
    }
    memset(e, 0, sizeof e);
    e[0].tag = 256; e[0].type = 4; e[0].count = 1; e[0].nums[0] = w;
    e[1].tag = 257; e[1].type = 4; e[1].count = 1; e[1].nums[0] = h;
    e[2].tag = 258; e[2].type = 3; e[2].count = 4;
    for (int c = 0; c < 4; c++) e[2].nums[c] = (uint32_t)bits;
    e[3].tag = 259; e[3].type = 3; e[3].count = 1; e[3].nums[0] = 1;
    e[4].tag = 262; e[4].type = 3; e[4].count = 1; e[4].nums[0] = 5;
    e[5].tag = 273; e[5].type = 4; e[5].count = 1;
    e[6].tag = 277; e[6].type = 3; e[6].count = 1; e[6].nums[0] = 4;
    e[7].tag = 278; e[7].type = 4; e[7].count = 1; e[7].nums[0] = h;
    e[8].tag = 279; e[8].type = 4; e[8].count = 1; e[8].nums[0] = (uint32_t)(w * h * bpp);
    e[9].tag = 284; e[9].type = 3; e[9].count = 1; e[9].nums[0] = 1;
    memset(b, 0, sizeof *b);
    tx_build(b, false, e, 10u, NULL, 0, NULL, 0, NULL, 0);
    sz = b->n;
    e[5].nums[0] = (uint32_t)sz;
    b->n = 0;
    tx_build(b, false, e, 10u, NULL, 0, NULL, 0, NULL, 0);
    pc_buf_append(b, data, (size_t)w * h * bpp);
    free(data);
}

static void t_cmyk_default(void)
{
    /* the audit's sample: C 50%, M 20%, Y 0%, K 10% (Pillow: 128, 51, 0, 26) */
    static const uint8_t k_px[4] = { 128, 51, 0, 26 };
    const uint32_t W = 40, H = 40;
    uint8_t *cmyk = (uint8_t *)malloc((size_t)W * H * 4u);
    pc_px32 want = { 0, 0, 0, 0 };
    size_t pn = 0;
    const uint8_t *prof = pc_icc_cmyk_default_profile(&pn);
    pc_icc_info info;
    CHECK(prof && pn == 121212u && pc_icc_inspect(prof, pn, &info) == PC_OK);
    CHECK(info.space == PC_ICC_SPACE_CMYK && strstr(info.desc, "SWOP") != NULL);
    CHECK(pc_icc_cmyk_default_profile(NULL) == prof);
    if (!cmyk) return;
    for (size_t i = 0; i < (size_t)W * H; i++) memcpy(cmyk + 4u * i, k_px, 4u);
    CHECK(cmyk_ref(k_px, 1u, false, &want));
    /* SWOP TR003 to Adobe RGB (1998), perceptual: what Little-CMS (also
     * through Pillow) gives; the naive formula gave 114, 183, 229 */
    INFO("default CMYK of (128, 51, 0, 26): %u, %u, %u", want.r, want.g, want.b);
    CHECK(abs((int)want.r - 131) <= 1 && abs((int)want.g - 162) <= 1 &&
          abs((int)want.b - 202) <= 1);
    {
        unsigned long n = 0;
        uint8_t *file = jpeg_cmyk_file(cmyk, W, H, &n);
        pc_doc *d = NULL;
        pc_image_meta m;
        CHECK(file && codec("jpeg")->load(file, n, NULL, &d, &m) == PC_OK);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            /* quality 100 keeps a flat color exactly */
            CHECK(px && px[0].r == want.r && px[0].g == want.g && px[0].b == want.b);
            CHECK(px && px[W * H - 1].r == want.r);
            CHECK(cmyk_ref_is_adobe(&m) && strstr(m.note, "default CMYK profile") != NULL);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        free(file);
    }
    for (int bits = 8; bits <= 16; bits += 8) {
        pc_buf b;
        pc_doc *d = NULL;
        pc_image_meta m;
        tiff_cmyk_file(&b, cmyk, W, H, bits);
        CHECK(codec("tiff")->load(b.p, b.n, NULL, &d, &m) == PC_OK);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            CHECK(px && px[5].r == want.r && px[5].g == want.g && px[5].b == want.b &&
                  px[5].a == 255u);
            CHECK(cmyk_ref_is_adobe(&m) && strstr(m.note, "default CMYK profile") != NULL);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        pc_buf_free(&b);
    }
    free(cmyk);
}

/* The per-color cache in the CMYK transform returns exactly the pipeline's
 * results: few colors (all hits after the first row), many colors (mostly
 * misses and slot collisions), in place (TIFF). */
static void t_cmyk_cache_exact(void)
{
    const uint32_t W = g_quick ? 160u : 512u, H = g_quick ? 120u : 384u;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *cmyk = (uint8_t *)malloc((size_t)W * H * 4u);
        pc_px32 *want = (pc_px32 *)malloc((size_t)W * H * sizeof *want);
        pc_buf b;
        pc_doc *d = NULL;
        pc_image_meta m;
        double t0;
        if (!cmyk || !want) { free(cmyk); free(want); CHECK(false); return; }
        for (size_t i = 0; i < (size_t)W * H; i++) {
            if (pass == 0) {
                uint32_t k = rndu(37u);
                cmyk[4 * i] = (uint8_t)(k * 7u); cmyk[4 * i + 1] = (uint8_t)(k * 13u);
                cmyk[4 * i + 2] = (uint8_t)(k * 29u); cmyk[4 * i + 3] = (uint8_t)(k * 3u);
            } else {
                for (int c = 0; c < 4; c++) cmyk[4 * i + (size_t)c] = rnd8();
            }
        }
        CHECK(cmyk_ref(cmyk, (size_t)W * H, false, want));
        tiff_cmyk_file(&b, cmyk, W, H, 8);
        t0 = pc_test_now();
        CHECK(codec("tiff")->load(b.p, b.n, NULL, &d, &m) == PC_OK);
        INFO("%s CMYK TIFF %u x %u: decoded in %.3f s", pass ? "random" : "37-color",
             (unsigned)W, (unsigned)H, pc_test_now() - t0);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            CHECK(px && tu_max_abs_diff(px, want, (size_t)W * H) == 0);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        pc_buf_free(&b);
        free(cmyk);
        free(want);
    }
}

/* Mutated gray, RGB and CMYK profiles through every new entry point: no
 * crash, no leak, and whatever is embedded is usable and matches. */
static void t_fuzz(void)
{
    const int iters = g_quick ? 300 : 6000;
    size_t n[3] = { 0, 0, 0 };
    uint8_t *base[3];
    int usable = 0, rgb_forms = 0;
    base[0] = itu_gray(2.2, "fuzz gray", &n[0]);
    base[1] = itu_rgb("fuzz rgb", 64u, &n[1]);
    base[2] = itu_cmyk(&n[2]);
    CHECK(base[0] && base[1] && base[2]);
    if (!base[0] || !base[1] || !base[2]) { free(base[0]); free(base[1]); free(base[2]); return; }
    for (int it = 0; it < iters; it++) {
        int k = (int)rndu(3u);
        uint8_t *p = (uint8_t *)malloc(n[k]);
        pc_image_meta m;
        pc_icc_embed e;
        pc_px32 px[3];
        int flips = 1 + (int)rndu(6u);
        if (!p) break;
        memcpy(p, base[k], n[k]);
        for (int f = 0; f < flips; f++) {
            size_t at = rndu(4u) == 0u ? rndu(132u) : rndu((uint32_t)n[k]);
            p[at] = rnd8();
        }
        if (pc_icc_usable_space(p, n[k]) != PC_ICC_SPACE_OTHER) usable++;
        memset(&m, 0, sizeof m);
        m.icc = p;
        m.icc_len = n[k];
        for (int sp = 0; sp < 2; sp++) {
            pc_icc_space want = sp ? PC_ICC_SPACE_GRAY : PC_ICC_SPACE_RGB;
            if (pc_icc_embed_for(&m, want, &e) == PC_OK && e.icc) {
                CHECK(pc_icc_usable_space(e.icc, e.len) == want);
                if (e.owned) rgb_forms++;
            }
            pc_icc_embed_free(&e);
        }
        {
            uint8_t *rgb = NULL;
            size_t rn = 0;
            if (pc_icc_gray_as_rgb(p, n[k], &rgb, &rn) == PC_OK)
                CHECK(rgb && pc_icc_usable_space(rgb, rn) == PC_ICC_SPACE_RGB);
            free(rgb);
        }
        px[0] = tu_px(10, 200, 30, 255);
        px[1] = tu_px(77, 77, 77, 255);
        px[2] = tu_px(1, 2, 3, 0);
        (void)pc_icc_to_srgb_px(p, n[k], px, 3, 1, 3u);
        (void)pc_icc_meta_validate(&m, NULL);       /* may free m.icc (== p) */
        if (m.icc) free(m.icc);
        m.icc = NULL;
        pc_meta_free(&m);
    }
    INFO("icc fuzz: %d of %d mutated profiles usable, %d RGB forms embedded", usable, iters,
         rgb_forms);
    free(base[0]); free(base[1]); free(base[2]);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_usable_space);
    RUN(t_gray_as_rgb);
    RUN(t_gray_colored_pixels);
    RUN(t_embed_for);
    RUN(t_cmyk_default);
    RUN(t_cmyk_cache_exact);
    RUN(t_fuzz);
    return pc_test_finish();
}
