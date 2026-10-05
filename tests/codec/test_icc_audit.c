/* test_icc_audit.c - lane CODEC, wave 4: regressions of the final
 * verification items 13 to 17 (handoff/final/w4_items.md), written against
 * the codec API that existed before the fix (save, load, pc_icc_inspect,
 * pc_icc_meta_validate, pc_icc_to_srgb_px), so the same file fails on the
 * old code and passes now.
 *
 *  - 13 F-FILE-FS-ICC: a gray profile is never embedded in RGB or palette
 *    data (PNG at every bit depth, JPEG, WebP, TIFF, BMP, PDN); AVIF and
 *    JPEG XL keep it with gray output; CMYK, damaged and junk profiles are
 *    never written; colored pixels of an image with a gray profile stay
 *    colored when converted;
 *  - 14 F-FILE-FL-ICC: a profile with a damaged PCS that still parses is
 *    dropped when the file opens and never re-embedded; JPEG XL saves with
 *    it (it failed with "Operation not allowed right now");
 *  - 15, 16, 17 F-FILE-FL-CMYK, F-FILE-JPEG-LOAD-CMYK, F-CORE-CM-CMYK: CMYK
 *    JPEG, 8-bit TIFF and CMYK64 TIFF without a profile give the audit's
 *    Little-CMS reference (131, 162, 202) tagged Adobe RGB (1998), not the
 *    naive (114, 183, 229) untagged.
 * Fixed content; profiles are made with Little-CMS. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "icc_test_util.h"
#include "meta_test_util.h"

#include <stdio.h>
#include "jpeglib.h"

static const pc_codec *codec(const char *id) { return pc_codec_by_id(id); }

static bool can_save(const char *id)
{
    const pc_codec *c = codec(id);
    return c && (c->flags & PC_CODEC_SAVE) && c->save && (c->flags & PC_CODEC_LOAD) && c->load;
}

static pc_doc *make_doc(uint32_t w, uint32_t h, bool color)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    pc_doc *d;
    if (!px) return NULL;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = (uint8_t)((x * 255u) / (w - 1u));
            px[(size_t)y * w + x] = color ? tu_px(v, (uint8_t)(y * 9u), (uint8_t)(255u - v), 255)
                                          : tu_px(v, v, v, 255);
        }
    d = tu_doc_from_px(w, h, px);
    free(px);
    return d;
}

#define EMB_NONE   (-1)        /* no profile in the file */
#define EMB_BROKEN 100         /* a profile pc_icc_inspect rejects */

/* Save d with meta through codec id (depth: the first int32 option, -1 =
 * defaults) and load the file back: the data color space of the profile in
 * the file (EMB_NONE, EMB_BROKEN or a pc_icc_space), *ok, the PNG color
 * type and, for a profile, whether it is the damaged one (PCS 'SYZ '). */
static int save_space(const char *id, const pc_doc *d, const pc_image_meta *meta, int depth,
                      int *png_type, bool *syz, bool *ok)
{
    const pc_codec *c = codec(id);
    void *prm = NULL;
    pc_buf out;
    pc_doc *r = NULL;
    pc_image_meta m;
    int sp = EMB_NONE;
    *ok = false;
    if (syz) *syz = false;
    if (!c) return sp;
    memset(&out, 0, sizeof out);
    memset(&m, 0, sizeof m);
    if (c->params_size) {
        prm = calloc(1u, c->params_size);
        if (!prm) return sp;
        pc_codec_default_params(c, prm);
        if (depth >= 0) memcpy(prm, &depth, sizeof depth);
    }
    if (c->save(d, meta, prm, NULL, &out) == PC_OK &&
        c->load(out.p, out.n, NULL, &r, &m) == PC_OK) {
        pc_icc_info info;
        *ok = true;
        if (m.icc) {
            sp = pc_icc_inspect(m.icc, m.icc_len, &info) == PC_OK ? (int)info.space
                                                                  : EMB_BROKEN;
            if (syz && m.icc_len > 24u) *syz = memcmp(m.icc + 20, "SYZ ", 4u) == 0;
        }
        if (png_type) *png_type = out.n > 25u ? out.p[25] : -1;
    }
    pc_doc_destroy(r);
    pc_meta_free(&m);
    pc_buf_free(&out);
    free(prm);
    return sp;
}

typedef struct enc_case { const char *id; int depth; bool gray_out; } enc_case;

/* Item 13: the audit's gray profile on a gray image, through every writer,
 * plus the profiles no writer may embed (item 14: the damaged one). */
static void t_encoders(void)
{
    /* gray_out: the writer stores gray samples for a gray image (AVIF 4:0:0,
     * JPEG XL one channel); the others store RGB or palette data */
    static const enc_case k_cases[] = {
        { "png", 0, false }, { "png", 1, false }, { "png", 2, false }, { "png", 3, false },
        { "jpeg", -1, false }, { "webp", -1, false },
        { "tiff", 0, false }, { "tiff", 2, false }, { "tiff", 3, false },
        { "bmp", 0, false }, { "bmp", 2, false }, { "bmp", 3, false },
        { "pdn", -1, false }, { "avif", -1, true }, { "jxl", -1, true },
    };
    size_t gn = 0, an = 0, bn = 0, cn = 0;
    uint8_t *gray = itu_gray(2.2, "audit gray", &gn), *adobe = NULL, *bad = itu_bad_pcs(&bn);
    uint8_t *junk = itu_junk(500u, 4u), *cmyk = itu_cmyk(&cn);
    pc_doc *gdoc = make_doc(32, 8, false), *cdoc = make_doc(32, 8, true);
    pc_image_meta m;
    int tested = 0;
    CHECK(pc_icc_adobe_rgb_profile(&adobe, &an) == PC_OK);
    CHECK(gray && bad && junk && cmyk && gdoc && cdoc);
    if (!gray || !bad || !junk || !cmyk || !gdoc || !cdoc || !adobe) goto done;
    memset(&m, 0, sizeof m);
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        const enc_case *k = &k_cases[i];
        bool ok, syz;
        int pt = -1, sp, want;
        if (!can_save(k->id)) {
            INFO("%s: not built, skipped", k->id);
            continue;
        }
        tested++;
        /* a gray profile on a gray image */
        m.icc = gray; m.icc_len = gn;
        sp = save_space(k->id, gdoc, &m, k->depth, &pt, NULL, &ok);
        want = k->gray_out ? (int)PC_ICC_SPACE_GRAY : (int)PC_ICC_SPACE_RGB;
        CHECK(ok && sp == want);
        if (!ok || sp != want)
            INFO("%s depth %d: gray image, gray profile -> %d", k->id, k->depth, sp);
        if (strcmp(k->id, "png") == 0) CHECK(pt == 2 || pt == 3 || pt == 6);   /* RGB data */
        /* a gray profile on a color image: an RGB profile */
        sp = save_space(k->id, cdoc, &m, k->depth, NULL, NULL, &ok);
        CHECK(ok && sp == (int)PC_ICC_SPACE_RGB);
        /* an RGB profile is kept, also for a gray image */
        m.icc = adobe; m.icc_len = an;
        sp = save_space(k->id, gdoc, &m, k->depth, NULL, NULL, &ok);
        CHECK(ok && sp == (int)PC_ICC_SPACE_RGB);
        /* never a CMYK, damaged or junk profile */
        m.icc = cmyk; m.icc_len = cn;
        CHECK(save_space(k->id, cdoc, &m, k->depth, NULL, NULL, &ok) == EMB_NONE && ok);
        m.icc = bad; m.icc_len = bn;
        sp = save_space(k->id, cdoc, &m, k->depth, NULL, &syz, &ok);
        CHECK(ok && sp == EMB_NONE && !syz);
        if (!ok) INFO("%s depth %d: save or reload with the damaged profile failed", k->id,
                      k->depth);
        m.icc = junk; m.icc_len = 500u;
        CHECK(save_space(k->id, cdoc, &m, k->depth, NULL, NULL, &ok) == EMB_NONE && ok);
    }
    CHECK(tested >= 13);
done:
    pc_doc_destroy(gdoc);
    pc_doc_destroy(cdoc);
    free(gray); free(adobe); free(bad); free(junk); free(cmyk);
}

/* Item 14: the damaged profile is dropped when the file opens, and JPEG XL
 * saves even when it got one. */
static void t_damaged_profile(void)
{
    size_t bn = 0;
    uint8_t *bad = itu_bad_pcs(&bn);
    pc_image_meta m;
    pc_icc_info info;
    pc_doc *d = make_doc(16, 16, true);
    CHECK(bad && pc_icc_inspect(bad, bn, &info) == PC_OK);      /* it parses */
    memset(&m, 0, sizeof m);
    m.icc = (uint8_t *)malloc(bn);
    if (m.icc && bad) {
        memcpy(m.icc, bad, bn);
        m.icc_len = bn;
    }
    CHECK(pc_icc_meta_validate(&m, d));
    CHECK(m.icc == NULL && m.icc_len == 0u && m.note[0] != '\0');
    pc_meta_free(&m);
    if (can_save("jxl") && bad && d) {
        pc_buf out;
        pc_status st;
        memset(&m, 0, sizeof m);
        memset(&out, 0, sizeof out);
        m.icc = bad;
        m.icc_len = bn;
        st = codec("jxl")->save(d, &m, NULL, NULL, &out);
        if (st != PC_OK) INFO("jxl save with a damaged profile: %s", pc_status_str(st));
        CHECK(st == PC_OK && out.n > 0u);
        pc_buf_free(&out);
    } else {
        INFO("jxl: not built, skipped");
    }
    pc_doc_destroy(d);
    free(bad);
}

/* Item 13: colored pixels of an image with a gray profile keep their color
 * when converted (screen, Image > Color Profile, import); they used to be
 * replaced by their green channel. Gray pixels are unchanged by the fix. */
static void t_colored_pixels(void)
{
    size_t gn = 0;
    uint8_t *gray = itu_gray(1.8, "gray 1.8", &gn);
    pc_px32 px[2];
    px[0] = tu_px(200, 40, 40, 255);
    px[1] = tu_px(90, 90, 90, 255);
    CHECK(gray && pc_icc_to_srgb_px(gray, gn, px, 2, 1, 2u) == PC_OK);
    CHECK(px[0].r > px[0].g + 60 && px[0].r > px[0].b + 60);       /* still red */
    CHECK(px[1].r == px[1].g && px[1].g == px[1].b);
    free(gray);
}

/* ---- items 15 to 17: CMYK without a profile -------------------------------------------- */
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

/* Separated CMYK TIFF, 8 or 16 bits per sample (v * 257), no profile. */
static void tiff_cmyk_file(pc_buf *b, const uint8_t *cmyk, uint32_t w, uint32_t h, int bits)
{
    tx_ent e[10];
    size_t bpp = (size_t)bits / 2u, sz;
    uint8_t *data = (uint8_t *)malloc((size_t)w * h * bpp);
    memset(b, 0, sizeof *b);
    if (!data) return;
    for (size_t i = 0; i < (size_t)w * h * 4u; i++) {
        if (bits == 8) data[i] = cmyk[i];
        else { data[2 * i] = cmyk[i]; data[2 * i + 1] = cmyk[i]; }
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
    tx_build(b, false, e, 10u, NULL, 0, NULL, 0, NULL, 0);
    sz = b->n;
    e[5].nums[0] = (uint32_t)sz;
    b->n = 0;
    tx_build(b, false, e, 10u, NULL, 0, NULL, 0, NULL, 0);
    pc_buf_append(b, data, (size_t)w * h * bpp);
    free(data);
}

/* The decoded color must be the SWOP to Adobe RGB (1998) reference of the
 * audit, tagged with Adobe RGB (1998). */
static void check_cmyk_result(const char *what, pc_doc *d, pc_image_meta *m)
{
    pc_icc_info info;
    pc_px32 *px = d ? tu_layer_px(d, d->stack[0]) : NULL;
    CHECK(px != NULL);
    if (px) {
        bool near = abs((int)px[7].r - 131) <= 1 && abs((int)px[7].g - 162) <= 1 &&
                    abs((int)px[7].b - 202) <= 1 && px[7].a == 255u;
        if (!near) INFO("%s: %u, %u, %u", what, px[7].r, px[7].g, px[7].b);
        CHECK(near);
    }
    CHECK(m->icc && pc_icc_inspect(m->icc, m->icc_len, &info) == PC_OK &&
          strcmp(info.desc, "Adobe RGB (1998)") == 0);
    free(px);
}

static void t_cmyk_without_profile(void)
{
    /* the audit's sample: C 50%, M 20%, Y 0%, K 10% (Pillow: 128, 51, 0, 26) */
    static const uint8_t k_px[4] = { 128, 51, 0, 26 };
    const uint32_t W = 40, H = 40;
    uint8_t *cmyk = (uint8_t *)malloc((size_t)W * H * 4u);
    if (!cmyk) { CHECK(false); return; }
    for (size_t i = 0; i < (size_t)W * H; i++) memcpy(cmyk + 4u * i, k_px, 4u);
    {
        unsigned long n = 0;
        uint8_t *file = jpeg_cmyk_file(cmyk, W, H, &n);
        pc_doc *d = NULL;
        pc_image_meta m;
        CHECK(file && codec("jpeg")->load(file, n, NULL, &d, &m) == PC_OK);
        if (d) {
            check_cmyk_result("jpeg", d, &m);
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
            check_cmyk_result(bits == 8 ? "tiff cmyk32" : "tiff cmyk64", d, &m);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        pc_buf_free(&b);
    }
    free(cmyk);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_encoders);
    RUN(t_damaged_profile);
    RUN(t_colored_pixels);
    RUN(t_cmyk_without_profile);
    return pc_test_finish();
}
