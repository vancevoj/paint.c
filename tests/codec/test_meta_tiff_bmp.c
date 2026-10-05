/* test_meta_tiff_bmp.c - lane CODEC: TIFF metadata (IFD0 tags, Exif and
 * GPS sub-IFDs, XMP 700, IPTC 33723, ICC 34675) written and read back, a
 * big-endian TIFF with orientation and metadata, separated CMYK through an
 * embedded profile to Adobe RGB (1998) (FL-CMYK), BMP V5 embedded
 * profiles at every depth (FS-ICC), the open-time profile check
 * pc_icc_meta_validate (FL-ICC), the Adobe RGB profile, and the
 * enabled_if rules of the indexed save options. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "meta_test_util.h"
#include "../../src/codec/cmeta.h"
#include "pc/pc_icc.h"

#include <stdio.h>
#include "lcms2.h"

static const pc_codec *codec(const char *id) { return pc_codec_by_id(id); }

static const uint8_t k_iim[] = { 0x1C, 2, 0x50, 0, 3, 'B', 'o', 'b' };
static const char k_xmp[] = "<x:xmpmeta><rdf:Description tiff:Orientation=\"8\" "
                            "dc:title=\"t\"/></x:xmpmeta>";

static pc_doc *ramp_doc(uint32_t w, uint32_t h)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    pc_doc *d;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[(size_t)y * w + x] = tu_px((uint8_t)(x * 9u), (uint8_t)(y * 17u), 40, 255);
    d = tu_doc_from_px(w, h, px);
    free(px);
    return d;
}

static uint8_t *save_profile(cmsHPROFILE h, size_t *len)
{
    cmsUInt32Number n = 0;
    uint8_t *p = NULL;
    if (cmsSaveProfileToMem(h, NULL, &n) && n) {
        p = (uint8_t *)malloc(n);
        if (!cmsSaveProfileToMem(h, p, &n)) { free(p); p = NULL; }
    }
    *len = p ? n : 0;
    cmsCloseProfile(h);
    return p;
}

/* CMYK profile whose AToB0 table is the naive CMYK -> sRGB formula. */
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
    cmsSetProfileVersion(p, 2.1);
    cmsSetDeviceClass(p, cmsSigOutputClass);
    cmsSetColorSpace(p, cmsSigCmykData);
    cmsSetPCS(p, cmsSigLabData);
    CHECK(cmsStageSampleCLut16bit(clut, naive_sampler, t, 0));
    CHECK(cmsPipelineInsertStage(pl, cmsAT_END, clut));
    CHECK(cmsWriteTag(p, cmsSigAToB0Tag, pl));
    cmsWriteTag(p, cmsSigMediaWhitePointTag, cmsD50_XYZ());
    cmsPipelineFree(pl);
    cmsDeleteTransform(t);
    cmsCloseProfile(lab);
    cmsCloseProfile(srgb);
    return save_profile(p, len);
}

static uint8_t *make_gray(size_t *len)
{
    cmsToneCurve *g = cmsBuildGamma(NULL, 2.2);
    cmsHPROFILE h = cmsCreateGrayProfile(cmsD50_xyY(), g);
    cmsFreeToneCurve(g);
    return save_profile(h, len);
}

static uint8_t *make_srgb(size_t *len)
{
    return save_profile(cmsCreate_sRGBProfile(), len);
}

static uint8_t *make_abstract(size_t *len)
{
    return save_profile(cmsCreateBCHSWabstractProfile(17, 0, 1.2, 0, 0, 0, 0), len);
}

/* ---- TIFF ---------------------------------------------------------------------------- */
static void rich_meta(pc_image_meta *m)
{
    static const uint8_t make[] = "Nikon", artist[] = "Cy", dto[] = "2023:01:02 03:04:05";
    static const uint8_t latref[] = "S", mk[9] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    tx_ent i0[3], ex[3], gp[1];
    pc_buf b;
    int o;
    memset(m, 0, sizeof *m);
    memset(&b, 0, sizeof b);
    memset(i0, 0, sizeof i0); memset(ex, 0, sizeof ex); memset(gp, 0, sizeof gp);
    i0[0].tag = 271; i0[0].type = 2; i0[0].count = 6; i0[0].bytes = make;
    i0[1].tag = 274; i0[1].type = 3; i0[1].count = 1; i0[1].nums[0] = 1;
    i0[2].tag = 315; i0[2].type = 2; i0[2].count = 3; i0[2].bytes = artist;
    ex[0].tag = 33434; ex[0].type = 5; ex[0].count = 1; ex[0].nums[0] = 1; ex[0].nums[1] = 60;
    ex[1].tag = 36867; ex[1].type = 2; ex[1].count = 20; ex[1].bytes = dto;
    ex[2].tag = 37500; ex[2].type = 7; ex[2].count = 9; ex[2].bytes = mk;
    gp[0].tag = 1; gp[0].type = 2; gp[0].count = 2; gp[0].bytes = latref;
    tx_build(&b, true, i0, 3, ex, 3, gp, 1, NULL, 0);
    CHECK(cm_meta_load_exif(m, b.p, b.n, true, &o) == PC_OK);
    CHECK(cm_meta_load_xmp(m, (const uint8_t *)k_xmp, sizeof k_xmp - 1u) == PC_OK);
    CHECK(cm_meta_load_iptc(m, k_iim, sizeof k_iim) == PC_OK);
    m->dpi_x = 200.0;
    m->dpi_y = 100.0;
    pc_buf_free(&b);
}

static void check_rich(const pc_image_meta *m, const char *what)
{
    cm_exif e;
    char *s;
    bool ok;
    uint8_t *ip = NULL;
    size_t ipn = 0;
    CHECK(cm_meta_get_exif(m, &e) == PC_OK);
    s = cm_exif_get_text(&e, 271);
    ok = s && strcmp(s, "Nikon") == 0;
    free(s);
    s = cm_exif_get_text(&e, CM_TAG_ARTIST);
    ok = ok && s && strcmp(s, "Cy") == 0;
    free(s);
    s = cm_exif_get_text(&e, 36867);
    ok = ok && s && strcmp(s, "2023:01:02 03:04:05") == 0;
    free(s);
    ok = ok && cm_exif_find(&e, 33434) && cm_exif_find(&e, 33434)->ifd == CM_IFD_EXIF;
    ok = ok && cm_exif_find(&e, CM_TAG_MAKERNOTE) && cm_exif_find(&e, CM_TAG_MAKERNOTE)->len == 9u;
    ok = ok && cm_exif_find(&e, 1) && cm_exif_find(&e, 1)->ifd == CM_IFD_GPS;
    cm_exif_free(&e);
    ok = ok && cm_meta_xmp(m, NULL) && strstr(cm_meta_xmp(m, NULL), "dc:title=\"t\"");
    ok = ok && cm_get_blob(m, CM_KEY_IPTC, &ip, &ipn) == PC_OK && ip && ipn == sizeof k_iim &&
         memcmp(ip, k_iim, ipn) == 0;
    free(ip);
    CHECK(ok);
    if (!ok) INFO("metadata incomplete after %s", what);
}

static void t_tiff_write(void)
{
    static const int32_t depths[3] = { 0, 2, 4 };     /* Auto, 24-bit, 4-bit */
    pc_doc *d = ramp_doc(20, 9);
    pc_image_meta m;
    size_t pn = 0;
    uint8_t *prof = make_srgb(&pn);
    rich_meta(&m);
    m.icc = prof;
    m.icc_len = pn;
    for (int k = 0; k < 3; k++) {
        int32_t prm[4];
        pc_buf out;
        tx_rd r;
        uint32_t i0, type, count;
        size_t at;
        pc_image_meta m2;
        pc_doc *rd = NULL;
        pc_codec_default_params(codec("tiff"), prm);
        prm[0] = depths[k];
        memset(&out, 0, sizeof out);
        CHECK(codec("tiff")->save(d, &m, prm, NULL, &out) == PC_OK);
        CHECK(tx_open(&r, out.p, out.n));
        i0 = tx_ifd0(&r);
        CHECK(tx_str_is(&r, i0, 271, "Nikon") && tx_str_is(&r, i0, 315, "Cy"));
        CHECK(tx_find(&r, i0, 700, &type, &count, &at) && type == 1u &&
              count == sizeof k_xmp - 1u && memcmp(out.p + at, k_xmp, count) == 0);
        CHECK(tx_find(&r, i0, 33723, &type, &count, &at) && count == sizeof k_iim &&
              memcmp(out.p + at, k_iim, count) == 0);
        CHECK(tx_find(&r, i0, 34675, &type, &count, &at) && type == 7u && count == pn &&
              memcmp(out.p + at, prof, pn) == 0);
        CHECK(tx_num(&r, i0, 274, 0) == 0u);           /* written upright, tag omitted */
        {
            uint32_t exo = tx_num(&r, i0, 34665, 0), gpo = tx_num(&r, i0, 34853, 0);
            CHECK(exo && tx_find(&r, exo, 33434, &type, &count, &at) && type == 5u);
            CHECK(exo && tx_str_is(&r, exo, 36867, "2023:01:02 03:04:05"));
            CHECK(gpo && tx_str_is(&r, gpo, 1, "S"));
        }
        for (uint32_t i = 1; i < tx_entries(&r, i0); i++)
            CHECK(tx_get(&r, i0 + 2u + 12u * i, 2u) > tx_get(&r, i0 + 2u + 12u * (i - 1u), 2u));
        CHECK(codec("tiff")->load(out.p, out.n, NULL, &rd, &m2) == PC_OK);
        if (rd) {
            check_rich(&m2, "TIFF");
            CHECK(m2.icc_len == pn && memcmp(m2.icc, prof, pn) == 0);
            CHECK(m2.dpi_x > 199.9 && m2.dpi_x < 200.1 && m2.dpi_y > 99.9 && m2.dpi_y < 100.1);
            pc_meta_free(&m2);
        }
        pc_doc_destroy(rd);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
}

/* A big-endian gray TIFF with orientation 6 and descriptive tags. */
static void t_tiff_read(void)
{
    static const uint8_t make[] = "Fuji", artist[] = "Dee", px[6] = { 10, 20, 30, 40, 50, 60 };
    tx_ent i0[13];
    pc_buf b;
    pc_doc *d = NULL;
    pc_image_meta m;
    size_t sz;
    memset(i0, 0, sizeof i0);
    i0[0].tag = 256; i0[0].type = 3; i0[0].count = 1; i0[0].nums[0] = 3;
    i0[1].tag = 257; i0[1].type = 3; i0[1].count = 1; i0[1].nums[0] = 2;
    i0[2].tag = 258; i0[2].type = 3; i0[2].count = 1; i0[2].nums[0] = 8;
    i0[3].tag = 262; i0[3].type = 3; i0[3].count = 1; i0[3].nums[0] = 1;
    i0[4].tag = 271; i0[4].type = 2; i0[4].count = 5; i0[4].bytes = make;
    i0[5].tag = 273; i0[5].type = 4; i0[5].count = 1; i0[5].nums[0] = 0;  /* set below */
    i0[6].tag = 274; i0[6].type = 3; i0[6].count = 1; i0[6].nums[0] = 6;
    i0[7].tag = 277; i0[7].type = 3; i0[7].count = 1; i0[7].nums[0] = 1;
    i0[8].tag = 278; i0[8].type = 3; i0[8].count = 1; i0[8].nums[0] = 2;
    i0[9].tag = 279; i0[9].type = 4; i0[9].count = 1; i0[9].nums[0] = 6;
    i0[10].tag = 315; i0[10].type = 2; i0[10].count = 4; i0[10].bytes = artist;
    i0[11].tag = 700; i0[11].type = 1; i0[11].count = (uint32_t)(sizeof k_xmp - 1u);
    i0[11].bytes = (const uint8_t *)k_xmp;
    i0[12].tag = 33723; i0[12].type = 7; i0[12].count = sizeof k_iim; i0[12].bytes = k_iim;
    memset(&b, 0, sizeof b);
    tx_build(&b, true, i0, 13, NULL, 0, NULL, 0, NULL, 0);
    sz = b.n;
    i0[5].nums[0] = (uint32_t)sz;
    b.n = 0;
    tx_build(&b, true, i0, 13, NULL, 0, NULL, 0, NULL, 0);
    CHECK(b.n == sz);
    pc_buf_append(&b, px, sizeof px);
    CHECK(codec("tiff")->load(b.p, b.n, NULL, &d, &m) == PC_OK);
    CHECK(d && d->w == 2 && d->h == 3);
    if (d) {
        cm_exif e;
        char *s;
        /* orientation 6: stored (x, y) lands at (sh - 1 - y, x) */
        CHECK(pc_layer_get_px(d->stack[0], 1, 0).r == 10 && pc_layer_get_px(d->stack[0], 0, 2).r == 60);
        CHECK(cm_meta_get_exif(&m, &e) == PC_OK);
        s = cm_exif_get_text(&e, 271);
        CHECK(s && strcmp(s, "Fuji") == 0);
        free(s);
        CHECK(cm_exif_orientation(&e) == 1 && !cm_exif_find(&e, 256) && !cm_exif_find(&e, 273));
        CHECK(!cm_exif_find(&e, 700) && !cm_exif_find(&e, 33723));
        cm_exif_free(&e);
        CHECK(strstr(cm_meta_xmp(&m, NULL), "tiff:Orientation=\"1\"") != NULL);
        {
            uint8_t *ip = NULL;
            size_t n = 0;
            CHECK(cm_get_blob(&m, CM_KEY_IPTC, &ip, &n) == PC_OK && n == sizeof k_iim);
            free(ip);
        }
        pc_meta_free(&m);
    }
    pc_doc_destroy(d);
    pc_buf_free(&b);
}

static uint8_t cmyk_sample(uint32_t i, uint32_t c)
{
    uint32_t v = (i * (c + 3u) * 7u + c * 40u) & 0xFFu;
    return (uint8_t)(c == 3 ? v / 3u : v);
}

/* Separated CMYK, 8 and 16 bits, with and without an embedded profile. The
 * managed result, seen through its Adobe RGB (1998) tag, must match Little-
 * CMS converting the same samples straight to sRGB; the naive decode is
 * the profile-less fallback. */
static void t_tiff_cmyk(void)
{
    const uint32_t W = 16, H = 8;
    size_t pn = 0;
    uint8_t *prof = make_cmyk(&pn);
    uint16_t ref[16 * 8 * 3];
    CHECK(prof != NULL);
    if (prof) {
        /* exact (unoptimized, 16-bit) CMYK -> Adobe RGB (1998) reference */
        uint8_t *adobe = NULL;
        size_t an = 0;
        cmsHPROFILE in = cmsOpenProfileFromMem(prof, (cmsUInt32Number)pn), out;
        cmsHTRANSFORM t;
        CHECK(pc_icc_adobe_rgb_profile(&adobe, &an) == PC_OK);
        out = cmsOpenProfileFromMem(adobe, (cmsUInt32Number)an);
        t = cmsCreateTransform(in, TYPE_CMYK_8, out, TYPE_RGB_16, INTENT_PERCEPTUAL,
                               cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
        free(adobe);
        uint8_t cm[16 * 8 * 4];
        for (uint32_t i = 0; i < W * H; i++)
            for (uint32_t c = 0; c < 4; c++) cm[4 * i + c] = cmyk_sample(i, c);
        cmsDoTransform(t, cm, ref, W * H);
        cmsDeleteTransform(t);
        cmsCloseProfile(in);
        cmsCloseProfile(out);
    }
    for (int bits = 8; bits <= 16 && prof; bits += 8) {
        pc_doc *dm = NULL, *dn = NULL;
        pc_image_meta mm, mn;
        for (int with = 0; with < 2; with++) {
            tx_ent i0[11];
            pc_buf b;
            size_t sz, bpp = (size_t)bits / 2u;      /* bytes per pixel: 4 samples */
            uint8_t *data = (uint8_t *)malloc((size_t)W * H * bpp);
            pc_status st;
            for (uint32_t i = 0; i < W * H; i++)
                for (uint32_t c = 0; c < 4; c++) {
                    uint8_t v = cmyk_sample(i, c);
                    if (bits == 8) data[4 * i + c] = v;
                    else { data[8 * i + 2 * c] = v; data[8 * i + 2 * c + 1] = v; }
                }
            memset(i0, 0, sizeof i0);
            i0[0].tag = 256; i0[0].type = 3; i0[0].count = 1; i0[0].nums[0] = W;
            i0[1].tag = 257; i0[1].type = 3; i0[1].count = 1; i0[1].nums[0] = H;
            i0[2].tag = 258; i0[2].type = 3; i0[2].count = 4;
            for (int c = 0; c < 4; c++) i0[2].nums[c] = (uint32_t)bits;
            i0[3].tag = 259; i0[3].type = 3; i0[3].count = 1; i0[3].nums[0] = 1;
            i0[4].tag = 262; i0[4].type = 3; i0[4].count = 1; i0[4].nums[0] = 5;
            i0[5].tag = 273; i0[5].type = 4; i0[5].count = 1;
            i0[6].tag = 277; i0[6].type = 3; i0[6].count = 1; i0[6].nums[0] = 4;
            i0[7].tag = 278; i0[7].type = 3; i0[7].count = 1; i0[7].nums[0] = H;
            i0[8].tag = 279; i0[8].type = 4; i0[8].count = 1; i0[8].nums[0] = (uint32_t)(W * H * bpp);
            i0[9].tag = 284; i0[9].type = 3; i0[9].count = 1; i0[9].nums[0] = 1;
            i0[10].tag = 34675; i0[10].type = 7; i0[10].count = (uint32_t)pn; i0[10].bytes = prof;
            memset(&b, 0, sizeof b);
            tx_build(&b, false, i0, with ? 11u : 10u, NULL, 0, NULL, 0, NULL, 0);
            sz = b.n;
            i0[5].nums[0] = (uint32_t)sz;
            b.n = 0;
            tx_build(&b, false, i0, with ? 11u : 10u, NULL, 0, NULL, 0, NULL, 0);
            pc_buf_append(&b, data, (size_t)W * H * bpp);
            st = codec("tiff")->load(b.p, b.n, NULL, with ? &dm : &dn, with ? &mm : &mn);
            CHECK(st == PC_OK);
            pc_buf_free(&b);
            free(data);
        }
        if (dm && dn) {
            pc_icc_info info;
            pc_px32 *a = tu_layer_px(dm, dm->stack[0]), *b = tu_layer_px(dn, dn->stack[0]);
            int md;
            CHECK(mm.icc && pc_icc_inspect(mm.icc, mm.icc_len, &info) == PC_OK &&
                  strcmp(info.desc, "Adobe RGB (1998)") == 0);
            CHECK(strstr(mm.note, "Adobe RGB") != NULL);
            CHECK(mn.icc == NULL && strstr(mn.note, "without") != NULL);
            md = 0;
            for (uint32_t i = 0; i < W * H; i++) {
                int dr = abs((int)a[i].r * 257 - ref[3 * i]);
                int dg = abs((int)a[i].g * 257 - ref[3 * i + 1]);
                int db = abs((int)a[i].b * 257 - ref[3 * i + 2]);
                int m3 = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
                if (m3 > md) md = m3;
            }
            INFO("%d-bit CMYK TIFF: max difference to the exact Adobe RGB values %.2f codes",
                 bits, md / 257.0);
            CHECK(md <= 257);              /* one rounding to 8 bits */
            for (uint32_t i = 0; i < W * H; i++) {   /* the naive formula, exactly */
                uint32_t c = cmyk_sample(i, 0), mg = cmyk_sample(i, 1), y = cmyk_sample(i, 2);
                uint32_t k = cmyk_sample(i, 3);
                CHECK(b[i].r == (uint8_t)pc_mul255(255u - c, 255u - k) &&
                      b[i].g == (uint8_t)pc_mul255(255u - mg, 255u - k) &&
                      b[i].b == (uint8_t)pc_mul255(255u - y, 255u - k) && b[i].a == 255u);
            }
            free(a);
            free(b);
            pc_meta_free(&mm);
            pc_meta_free(&mn);
        }
        pc_doc_destroy(dm);
        pc_doc_destroy(dn);
    }
    free(prof);
}

/* ---- BMP --------------------------------------------------------------------------------- */
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void t_bmp_icc(void)
{
    static const int32_t depths[5] = { 0, 1, 2, 3, 5 };   /* Auto, 32, 24, 8, 1 */
    pc_doc *d = ramp_doc(13, 7);
    size_t pn = 0;
    uint8_t *prof = make_srgb(&pn);
    for (int k = 0; k < 5; k++) {
        for (int with = 0; with < 2; with++) {
            int32_t prm[3];
            pc_image_meta m, m2;
            pc_buf out;
            pc_doc *r = NULL;
            memset(&m, 0, sizeof m);
            if (with) { m.icc = prof; m.icc_len = pn; }
            pc_codec_default_params(codec("bmp"), prm);
            prm[0] = depths[k];
            memset(&out, 0, sizeof out);
            CHECK(codec("bmp")->save(d, &m, prm, NULL, &out) == PC_OK);
            CHECK(out.n > 54u && rd32le(out.p + 2) == out.n);
            if (with) {
                uint32_t off = rd32le(out.p + 14 + 112), size = rd32le(out.p + 14 + 116);
                CHECK(rd32le(out.p + 14) == 124u && rd32le(out.p + 14 + 56) == 0x4D424544u);
                CHECK(size == pn && 14u + off + size == out.n &&
                      memcmp(out.p + 14 + off, prof, pn) == 0);
            } else {
                uint32_t bits = (uint32_t)out.p[28] | ((uint32_t)out.p[29] << 8);
                CHECK(rd32le(out.p + 14) == (bits == 32u ? 124u : 40u));
                if (bits == 32u) CHECK(rd32le(out.p + 14 + 56) == 0x73524742u);   /* sRGB */
            }
            CHECK(codec("bmp")->load(out.p, out.n, NULL, &r, &m2) == PC_OK);
            if (r) {
                CHECK(with ? (m2.icc_len == pn && memcmp(m2.icc, prof, pn) == 0) : m2.icc == NULL);
                pc_meta_free(&m2);
            }
            pc_doc_destroy(r);
            pc_buf_free(&out);
        }
    }
    free(prof);
    pc_doc_destroy(d);
}

/* ---- open-time profile check ---------------------------------------------------------------- */
static void t_validate(void)
{
    pc_doc *color = ramp_doc(5, 5), *gray;
    size_t n;
    {
        pc_px32 g[25];
        for (int i = 0; i < 25; i++) g[i] = tu_px((uint8_t)(i * 9), (uint8_t)(i * 9), (uint8_t)(i * 9), 255);
        gray = tu_doc_from_px(5, 5, g);
    }
    struct { const char *name; uint8_t *(*make)(size_t *); bool drop_color, drop_gray; } cases[] = {
        { "sRGB", make_srgb, false, false },
        { "gray", make_gray, true, false },
        { "CMYK", make_cmyk, true, true },
        { "abstract", make_abstract, true, true },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        uint8_t *p = cases[k].make(&n);
        for (int g = 0; g < 2; g++) {
            pc_image_meta m;
            bool dropped;
            memset(&m, 0, sizeof m);
            m.icc = (uint8_t *)malloc(n);
            memcpy(m.icc, p, n);
            m.icc_len = n;
            dropped = pc_icc_meta_validate(&m, g ? gray : color);
            CHECK(dropped == (g ? cases[k].drop_gray : cases[k].drop_color));
            CHECK(dropped ? (m.icc == NULL && m.icc_len == 0 && m.note[0]) : m.icc != NULL);
            if (dropped != (g ? cases[k].drop_gray : cases[k].drop_color))
                INFO("%s profile on a %s image: dropped=%d", cases[k].name, g ? "gray" : "color",
                     (int)dropped);
            pc_meta_free(&m);
        }
        free(p);
    }
    {   /* garbage (an unparseable iCCP), a note that is kept, NULL document */
        pc_image_meta m;
        memset(&m, 0, sizeof m);
        m.icc = (uint8_t *)malloc(260);
        for (int i = 0; i < 260; i++) m.icc[i] = (uint8_t)(i * 31 + 7);
        m.icc_len = 260;
        snprintf(m.note, sizeof m.note, "Multi-page TIFF");
        CHECK(pc_icc_meta_validate(&m, NULL) && m.icc == NULL);
        CHECK(strncmp(m.note, "Multi-page TIFF; ", 17) == 0 && strstr(m.note, "damaged"));
        pc_meta_free(&m);
        memset(&m, 0, sizeof m);
        CHECK(!pc_icc_meta_validate(&m, color));       /* nothing to check */
        m.icc = make_gray(&m.icc_len);
        CHECK(!pc_icc_meta_validate(&m, NULL) && m.icc);  /* unknown pixels: kept */
        pc_meta_free(&m);
    }
    {   /* the Adobe RGB (1998) profile */
        uint8_t *a = NULL, *b = NULL;
        size_t an = 0, bn = 0;
        pc_icc_info info;
        CHECK(pc_icc_adobe_rgb_profile(&a, &an) == PC_OK && a && an > 132u);
        CHECK(pc_icc_adobe_rgb_profile(&b, &bn) == PC_OK && an == bn && memcmp(a, b, an) == 0);
        CHECK(pc_icc_inspect(a, an, &info) == PC_OK && info.space == PC_ICC_SPACE_RGB &&
              !info.is_srgb && strcmp(info.desc, "Adobe RGB (1998)") == 0);
        {   /* green primary: pure Adobe green is outside sRGB (clipped), mid gray stays gray */
            pc_px32 px[2] = { { 0, 255, 0, 255 }, { 128, 128, 128, 255 } };
            CHECK(pc_icc_to_srgb_px(a, an, px, 2, 1, 2) == PC_OK);
            CHECK(px[0].g == 255 && px[0].r == 0 && px[0].b < 70);
            CHECK(abs((int)px[1].r - (int)px[1].g) <= 1 && abs((int)px[1].g - (int)px[1].b) <= 1);
        }
        free(a);
        free(b);
    }
    pc_doc_destroy(color);
    pc_doc_destroy(gray);
}

/* ---- enabled_if of the indexed options --------------------------------------------------------- */
/* "key" or "key=N|M|...": every key names a choice/int prop and every value
 * is one of its choices. */
static void t_enables(void)
{
    static const char *const ids[] = { "png", "bmp", "tiff", "gif", "jpeg", "webp", "dds" };
    for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        const pc_codec *c = codec(ids[k]);
        for (uint32_t i = 0; c && i < c->n_props; i++) {
            const char *e = c->props[i].enabled_if, *eq;
            const fx_prop *q = NULL;
            size_t kl;
            if (!e) continue;
            eq = strchr(e, '=');
            kl = eq ? (size_t)(eq - e) : strlen(e);
            for (uint32_t j = 0; j < c->n_props; j++)
                if (strlen(c->props[j].key) == kl && memcmp(c->props[j].key, e, kl) == 0)
                    q = &c->props[j];
            CHECK(q != NULL && q != &c->props[i]);
            if (!q || !eq) continue;
            for (const char *v = eq + 1; v; v = strchr(v, '|')) {
                long x;
                if (*v == '|') v++;
                x = strtol(v, NULL, 10);
                CHECK(x >= (long)q->min && x <= (long)q->max);
            }
        }
    }
    {   /* the indexed bit depths enable the quantizer options */
        const pc_codec *png = codec("png"), *bmp = codec("bmp"), *tif = codec("tiff");
        CHECK(png && strcmp(png->props[1].key, "palette") == 0 &&
              strcmp(png->props[1].enabled_if, "bit_depth=3|4|5|6") == 0);
        CHECK(bmp && strcmp(bmp->props[1].enabled_if, "bit_depth=3|4|5") == 0 &&
              strcmp(bmp->props[2].label, "Quantization algorithm") == 0);
        CHECK(tif && strcmp(tif->props[2].enabled_if, "bit_depth=3|4|5|6") == 0);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_tiff_write);
    RUN(t_tiff_read);
    RUN(t_tiff_cmyk);
    RUN(t_bmp_icc);
    RUN(t_validate);
    RUN(t_enables);
    return pc_test_finish();
}
