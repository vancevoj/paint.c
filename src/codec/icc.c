/* icc.c - ICC profile handling with Little-CMS 2 (lane L6B). See
 * include/pc/pc_icc.h for the policy. Every entry point creates its own
 * cmsContext with a private error handler, so no Little-CMS global state
 * is used and calls are reentrant. */
#include "pc/pc_icc.h"
#include "lib_codec.h"

#include <stdlib.h>
#include <string.h>

#include "lcms2.h"

/* ---- contexts and validation -------------------------------------------------------- */
typedef struct icc_log {
    char msg[96];
    int  errors;
} icc_log;

static void icc_log_fn(cmsContext ctx, cmsUInt32Number code, const char *text)
{
    icc_log *l = (icc_log *)cmsGetContextUserData(ctx);
    (void)code;
    if (!l) return;
    if (l->errors == 0 && text) lc_utf8_copy(l->msg, sizeof l->msg, text, strlen(text));
    l->errors++;
}

static cmsContext icc_ctx(icc_log *log)
{
    cmsContext ctx;
    memset(log, 0, sizeof *log);
    ctx = cmsCreateContext(NULL, log);
    if (ctx) cmsSetLogErrorHandlerTHR(ctx, icc_log_fn);
    return ctx;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Header and tag table sanity. On success *size is the declared profile
 * size (<= len). */
static pc_status icc_validate(const uint8_t *p, size_t len, size_t *size)
{
    uint32_t sz, count;
    if (!p) return PC_ERR_ARG;
    if (len > PC_ICC_MAX_BYTES) return PC_ERR_LIMIT;
    if (len < 132u) return PC_ERR_FORMAT;
    sz = be32(p);
    if (sz < 132u || (size_t)sz > len) return PC_ERR_FORMAT;
    if (memcmp(p + 36, "acsp", 4u) != 0) return PC_ERR_FORMAT;
    count = be32(p + 128);
    if (count == 0u || count > (sz - 132u) / 12u) return PC_ERR_FORMAT;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *t = p + 132u + (size_t)i * 12u;
        uint64_t off = be32(t + 4), tsz = be32(t + 8);
        if (off < 128u || off + tsz > (uint64_t)sz) return PC_ERR_FORMAT;
    }
    *size = sz;
    return PC_OK;
}

static pc_icc_space space_of(const uint8_t *p)
{
    uint32_t s = be32(p + 16);
    if (s == 0x52474220u) return PC_ICC_SPACE_RGB;      /* 'RGB ' */
    if (s == 0x47524159u) return PC_ICC_SPACE_GRAY;     /* 'GRAY' */
    if (s == 0x434D594Bu) return PC_ICC_SPACE_CMYK;     /* 'CMYK' */
    return PC_ICC_SPACE_OTHER;
}

/* ---- gray profiles as RGB (lane CODEC, wave 4) ------------------------------------------- */
/* A gray profile maps a gray value v to the PCS color TRC(v) x D50 (ICC.1
 * monochrome model). An RGB matrix/TRC profile whose three curves are that
 * TRC and whose colorants add up to D50 maps (v, v, v) to the same color,
 * so it describes gray pixels exactly and gives colored pixels a defined
 * meaning (Rec. 709 / sRGB primaries, D65 white adapted to D50: a gray
 * profile with the sRGB curve becomes sRGB itself). Used to embed a gray
 * profile in RGB or palette files (FS-ICC) and to show colored pixels of an
 * image with a gray profile. */
#define GRAY_SAMPLES 1024

/* The gray -> Y curve of a gray profile: a copy of the grayTRC tag when the
 * PCS is XYZ, else sampled from a gray -> XYZ transform (Lab PCS, LUT based
 * profiles). Caller frees with cmsFreeToneCurve. NULL on failure. */
static cmsToneCurve *gray_curve(cmsContext ctx, cmsHPROFILE g)
{
    const cmsToneCurve *t = NULL;
    cmsHPROFILE xyz;
    cmsHTRANSFORM x = NULL;
    cmsToneCurve *c = NULL;
    float *in = NULL, *y = NULL;
    cmsCIEXYZ *o = NULL;
    if (cmsGetPCS(g) == cmsSigXYZData && cmsIsTag(g, cmsSigGrayTRCTag))
        t = (const cmsToneCurve *)cmsReadTag(g, cmsSigGrayTRCTag);
    if (t) return cmsDupToneCurve(t);
    xyz = cmsCreateXYZProfileTHR(ctx);
    if (xyz)
        x = cmsCreateTransformTHR(ctx, g, TYPE_GRAY_FLT, xyz, TYPE_XYZ_DBL, INTENT_PERCEPTUAL,
                                  cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (xyz) cmsCloseProfile(xyz);
    if (!x) return NULL;
    in = (float *)malloc(GRAY_SAMPLES * sizeof *in);
    y = (float *)malloc(GRAY_SAMPLES * sizeof *y);
    o = (cmsCIEXYZ *)malloc(GRAY_SAMPLES * sizeof *o);
    if (in && y && o) {
        for (int i = 0; i < GRAY_SAMPLES; i++) in[i] = (float)i / (float)(GRAY_SAMPLES - 1);
        cmsDoTransform(x, in, o, (cmsUInt32Number)GRAY_SAMPLES);
        for (int i = 0; i < GRAY_SAMPLES; i++) {
            double v = o[i].Y;
            y[i] = (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
        c = cmsBuildTabulatedToneCurveFloat(ctx, (cmsUInt32Number)GRAY_SAMPLES, y);
    }
    free(in);
    free(y);
    free(o);
    cmsDeleteTransform(x);
    return c;
}

/* The RGB equivalent of the open gray profile g (see above), or NULL. */
static cmsHPROFILE gray_as_rgb(cmsContext ctx, cmsHPROFILE g)
{
    static const cmsCIExyY k_d65 = { 0.3127, 0.3290, 1.0 };
    static const cmsCIExyYTRIPLE k_709 = { { 0.64, 0.33, 1.0 }, { 0.30, 0.60, 1.0 },
                                           { 0.15, 0.06, 1.0 } };
    cmsToneCurve *c = gray_curve(ctx, g);
    cmsToneCurve *c3[3];
    cmsHPROFILE h;
    if (!c) return NULL;
    c3[0] = c3[1] = c3[2] = c;
    h = cmsCreateRGBProfileTHR(ctx, &k_d65, &k_709, c3);
    cmsFreeToneCurve(c);
    return h;
}

/* ---- transforms to sRGB ------------------------------------------------------------------ */
typedef struct icc_xf {
    cmsContext    ctx;
    cmsHTRANSFORM xf;            /* RGB: BGRA -> BGRA; gray: the RGB equivalent (colored
                                    pixels), NULL when it could not be built */
    uint8_t       gray[256][3];  /* gray: code -> B, G, R (pixels with R == G == B) */
    pc_icc_space  space;
    icc_log       log;
} icc_xf;

static void icc_xf_free(icc_xf *x)
{
    if (x->xf) cmsDeleteTransform(x->xf);
    if (x->ctx) cmsDeleteContext(x->ctx);
    memset(x, 0, sizeof *x);
}

static pc_status icc_xf_make(icc_xf *x, const uint8_t *icc, size_t len)
{
    cmsHPROFILE in = NULL, out = NULL;
    size_t size = 0;
    pc_status st;
    memset(x, 0, sizeof *x);
    st = icc_validate(icc, len, &size);
    if (st != PC_OK) return st;
    x->space = space_of(icc);
    if (x->space != PC_ICC_SPACE_RGB && x->space != PC_ICC_SPACE_GRAY) return PC_ERR_UNSUPPORTED;
    x->ctx = icc_ctx(&x->log);
    if (!x->ctx) return PC_ERR_NOMEM;
    in = cmsOpenProfileFromMemTHR(x->ctx, icc, (cmsUInt32Number)size);
    out = cmsCreate_sRGBProfileTHR(x->ctx);
    if (!in || !out) { st = in ? PC_ERR_NOMEM : PC_ERR_FORMAT; goto done; }
    if (x->space == PC_ICC_SPACE_RGB) {
        if (cmsGetColorSpace(in) != cmsSigRgbData) { st = PC_ERR_FORMAT; goto done; }
        x->xf = cmsCreateTransformTHR(x->ctx, in, TYPE_BGRA_8, out, TYPE_BGRA_8,
                                      INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA | cmsFLAGS_NOCACHE);
        if (!x->xf) { st = PC_ERR_FORMAT; goto done; }
    } else {
        cmsHTRANSFORM g;
        uint8_t codes[256], bgr[256 * 3];
        if (cmsGetColorSpace(in) != cmsSigGrayData) { st = PC_ERR_FORMAT; goto done; }
        /* built once per call, so skip the 8-bit optimizer: its curve
         * approximation is several codes off near black */
        g = cmsCreateTransformTHR(x->ctx, in, TYPE_GRAY_8, out, TYPE_BGR_8, INTENT_PERCEPTUAL,
                                  cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
        if (!g) { st = PC_ERR_FORMAT; goto done; }
        for (int i = 0; i < 256; i++) codes[i] = (uint8_t)i;
        cmsDoTransform(g, codes, bgr, 256);
        cmsDeleteTransform(g);
        memcpy(x->gray, bgr, sizeof bgr);
        {
            /* colored pixels go through the RGB equivalent (wave 4); without
             * it they fall back to their green channel */
            cmsHPROFILE rgb = gray_as_rgb(x->ctx, in);
            if (rgb) {
                x->xf = cmsCreateTransformTHR(x->ctx, rgb, TYPE_BGRA_8, out, TYPE_BGRA_8,
                                              INTENT_PERCEPTUAL,
                                              cmsFLAGS_COPY_ALPHA | cmsFLAGS_NOCACHE);
                cmsCloseProfile(rgb);
            }
        }
    }
    st = PC_OK;
done:
    if (in) cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    if (st != PC_OK) icc_xf_free(x);
    return st;
}

/* Convert n pixels in place. scratch holds n * 4 bytes. */
static void icc_xf_run(const icc_xf *x, pc_px32 *px, size_t n, uint8_t *scratch)
{
    if (x->space == PC_ICC_SPACE_RGB) {
        cmsDoTransform(x->xf, px, scratch, (cmsUInt32Number)n);
        memcpy(px, scratch, n * 4u);
    } else {
        bool color = false;
        for (size_t i = 0; i < n && !color; i++)
            color = px[i].r != px[i].g || px[i].g != px[i].b;
        if (color && x->xf) cmsDoTransform(x->xf, px, scratch, (cmsUInt32Number)n);
        else color = false;
        for (size_t i = 0; i < n; i++) {
            if (color && (px[i].r != px[i].g || px[i].g != px[i].b)) {
                memcpy(&px[i], scratch + 4u * i, 4u);
            } else {
                const uint8_t *e = x->gray[px[i].g];
                px[i].b = e[0]; px[i].g = e[1]; px[i].r = e[2];
            }
        }
    }
    for (size_t i = 0; i < n; i++)
        if (px[i].a == 0u) px[i].b = px[i].g = px[i].r = 0u;
}

/* True when the transform moves no probe color by more than 1. */
static bool icc_xf_is_identity(const icc_xf *x)
{
    pc_px32 probe[6 * 6 * 6 + 256], orig[6 * 6 * 6 + 256];
    uint8_t scratch[(6 * 6 * 6 + 256) * 4];
    size_t n = 0;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                /* gray profiles: colored probes only when they have an RGB form */
                if (x->space == PC_ICC_SPACE_GRAY && !x->xf && !(r == g && g == b)) continue;
                probe[n].r = (uint8_t)(r * 51); probe[n].g = (uint8_t)(g * 51);
                probe[n].b = (uint8_t)(b * 51); probe[n].a = 255u;
                n++;
            }
    for (int i = 0; i < 256; i++) {
        probe[n].r = probe[n].g = probe[n].b = (uint8_t)i;
        probe[n].a = 255u;
        n++;
    }
    memcpy(orig, probe, n * sizeof *probe);
    icc_xf_run(x, probe, n, scratch);
    for (size_t i = 0; i < n; i++) {
        if (abs((int)probe[i].r - orig[i].r) > 1 || abs((int)probe[i].g - orig[i].g) > 1 ||
            abs((int)probe[i].b - orig[i].b) > 1)
            return false;
    }
    return true;
}

/* ---- public API ------------------------------------------------------------------------------ */
/* pc_icc_inspect; with_srgb false skips the is_srgb probe (it builds an
 * optimized transform, the most expensive part). */
static pc_status icc_inspect(const uint8_t *icc, size_t len, pc_icc_info *info, bool with_srgb)
{
    size_t size;
    pc_status st;
    if (!info) return PC_ERR_ARG;
    memset(info, 0, sizeof *info);
    st = icc_validate(icc, len, &size);
    if (st != PC_OK) return st;
    {
        icc_log log;
        cmsContext ctx = icc_ctx(&log);
        cmsHPROFILE h;
        if (!ctx) return PC_ERR_NOMEM;
        h = cmsOpenProfileFromMemTHR(ctx, icc, (cmsUInt32Number)size);
        if (!h) { cmsDeleteContext(ctx); return PC_ERR_FORMAT; }
        {
            char buf[128];
            cmsUInt32Number k = cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", buf,
                                                       (cmsUInt32Number)sizeof buf);
            if (k > 0u) {
                buf[sizeof buf - 1u] = '\0';
                lc_utf8_copy(info->desc, sizeof info->desc, buf, strlen(buf));
            }
        }
        cmsCloseProfile(h);
        cmsDeleteContext(ctx);
    }
    info->space = space_of(icc);
    info->version = be32(icc + 8);
    info->device_class = be32(icc + 12);
    if (with_srgb && (info->space == PC_ICC_SPACE_RGB || info->space == PC_ICC_SPACE_GRAY)) {
        icc_xf x;
        if (icc_xf_make(&x, icc, len) == PC_OK) {
            info->is_srgb = icc_xf_is_identity(&x);
            icc_xf_free(&x);
        }
    }
    return PC_OK;
}

pc_status pc_icc_inspect(const uint8_t *icc, size_t len, pc_icc_info *info)
{
    return icc_inspect(icc, len, info, true);
}

pc_status pc_icc_to_srgb_px(const uint8_t *icc, size_t len, pc_px32 *px, int32_t w,
                            int32_t h, size_t stride)
{
    icc_xf x;
    uint8_t *scratch;
    pc_status st;
    if (!px || w <= 0 || h <= 0 || (size_t)w > stride) return PC_ERR_ARG;
    st = icc_xf_make(&x, icc, len);
    if (st != PC_OK) return st;
    scratch = (uint8_t *)malloc((size_t)w * 4u);
    if (!scratch) { icc_xf_free(&x); return PC_ERR_NOMEM; }
    for (int32_t y = 0; y < h; y++)
        icc_xf_run(&x, px + (size_t)y * stride, (size_t)w, scratch);
    free(scratch);
    icc_xf_free(&x);
    return PC_OK;
}

typedef struct icc_job {
    const icc_xf *x;
    const pc_doc *d;
} icc_job;

static void icc_tile_job(void *ud, uint32_t index, uint32_t worker)
{
    const icc_job *j = (const icc_job *)ud;
    size_t per = (size_t)j->d->tiles_x * j->d->tiles_y;
    const pc_layer *l = j->d->stack[index / per];
    size_t ti = index % per;
    pc_tile *t = l->grid[ti];
    uint32_t tx = (uint32_t)(ti % j->d->tiles_x), ty = (uint32_t)(ti / j->d->tiles_x);
    uint32_t cw = j->d->w - tx * PC_TILE_DIM, ch = j->d->h - ty * PC_TILE_DIM;
    uint8_t scratch[PC_TILE_DIM * 4u];
    (void)worker;
    if (!t) return;
    if (cw > PC_TILE_DIM) cw = PC_TILE_DIM;
    if (ch > PC_TILE_DIM) ch = PC_TILE_DIM;
    for (uint32_t y = 0; y < ch; y++)
        icc_xf_run(j->x, (pc_px32 *)(void *)t->data + (size_t)y * PC_TILE_DIM, cw, scratch);
}

pc_status pc_icc_to_srgb_doc(pc_doc *d, const uint8_t *icc, size_t len, const pc_par *par)
{
    icc_xf x;
    icc_job job;
    size_t per, total;
    pc_status st;
    if (!d) return PC_ERR_ARG;
    per = (size_t)d->tiles_x * d->tiles_y;
    if (!pc_mul_size(per, d->n_layers, &total) || total > UINT32_MAX) return PC_ERR_LIMIT;
    for (uint32_t i = 0; i < d->n_layers; i++)       /* unpublished tiles only */
        for (size_t k = 0; k < per; k++) {
            pc_tile *t = d->stack[i]->grid[k];
            if (t && (pc_tile_refs(t) != 1u || t->bpp != 4u)) return PC_ERR_STATE;
        }
    st = icc_xf_make(&x, icc, len);
    if (st != PC_OK) return st;
    job.x = &x;
    job.d = d;
    if (total) pc_par_for(par, icc_tile_job, &job, (uint32_t)total);
    for (uint32_t i = 0; i < d->n_layers; i++) d->stack[i]->gen++;
    d->gen++;
    icc_xf_free(&x);
    return PC_OK;
}

pc_status pc_icc_import(pc_doc *d, pc_image_meta *meta, const pc_par *par)
{
    pc_icc_info info;
    pc_status st;
    char note[128];
    if (!d || !meta) return PC_ERR_ARG;
    if (!meta->icc || !meta->icc_len) return PC_OK;
    st = pc_icc_inspect(meta->icc, meta->icc_len, &info);
    if (st != PC_OK) {
        lc_note(meta, "The embedded color profile is damaged; colors were not converted");
        return st;
    }
    if (info.space != PC_ICC_SPACE_RGB && info.space != PC_ICC_SPACE_GRAY) {
        lc_note(meta, "The embedded color profile does not match the image; colors were not "
                      "converted");
        return PC_ERR_UNSUPPORTED;
    }
    if (!info.is_srgb) {
        st = pc_icc_to_srgb_doc(d, meta->icc, meta->icc_len, par);
        if (st != PC_OK) {
            lc_note(meta, "The embedded color profile could not be applied; colors were not "
                          "converted");
            return st;
        }
        if (info.desc[0]) {
            static const char k_pre[] = "Converted to sRGB from ";
            memcpy(note, k_pre, sizeof k_pre);
            lc_utf8_copy(note + sizeof k_pre - 1u, sizeof note - (sizeof k_pre - 1u), info.desc,
                         strlen(info.desc));
            lc_note(meta, note);
        } else {
            lc_note(meta, "Converted to sRGB from the embedded color profile");
        }
    }
    free(meta->icc);
    meta->icc = NULL;
    meta->icc_len = 0;
    return PC_OK;
}

pc_status pc_icc_srgb_profile(uint8_t **out, size_t *len)
{
    icc_log log;
    cmsContext ctx;
    cmsHPROFILE h;
    cmsUInt32Number size = 0;
    uint8_t *buf;
    pc_status st = PC_ERR_NOMEM;
    if (!out || !len) return PC_ERR_ARG;
    *out = NULL;
    *len = 0;
    ctx = icc_ctx(&log);
    if (!ctx) return PC_ERR_NOMEM;
    h = cmsCreate_sRGBProfileTHR(ctx);
    if (!h) goto done;
    if (!cmsSaveProfileToMem(h, NULL, &size) || size < 132u) goto done;
    buf = (uint8_t *)malloc(size);
    if (!buf) goto done;
    if (!cmsSaveProfileToMem(h, buf, &size)) { free(buf); goto done; }
    memset(buf + 24, 0, 12u);        /* creation date: keep exports reproducible */
    memset(buf + 84, 0, 16u);        /* profile ID would cover the date */
    *out = buf;
    *len = size;
    st = PC_OK;
done:
    if (h) cmsCloseProfile(h);
    cmsDeleteContext(ctx);
    return st;
}

/* Serialize h into a malloc'ed buffer with the creation date and profile
 * ID zeroed, so exports stay reproducible. Closes nothing. */
static pc_status save_profile(cmsHPROFILE h, uint8_t **out, size_t *len)
{
    cmsUInt32Number size = 0;
    uint8_t *buf;
    if (!cmsSaveProfileToMem(h, NULL, &size) || size < 132u) return PC_ERR_NOMEM;
    buf = (uint8_t *)malloc(size);
    if (!buf) return PC_ERR_NOMEM;
    if (!cmsSaveProfileToMem(h, buf, &size)) { free(buf); return PC_ERR_NOMEM; }
    memset(buf + 24, 0, 12u);
    memset(buf + 84, 0, 16u);
    *out = buf;
    *len = size;
    return PC_OK;
}

pc_status pc_icc_adobe_rgb_profile(uint8_t **out, size_t *len)
{
    static const cmsCIExyY k_white = { 0.3127, 0.3290, 1.0 };
    static const cmsCIExyYTRIPLE k_prim = { { 0.64, 0.33, 1.0 }, { 0.21, 0.71, 1.0 },
                                            { 0.15, 0.06, 1.0 } };
    icc_log log;
    cmsContext ctx;
    cmsToneCurve *g = NULL;
    cmsToneCurve *c3[3];
    cmsHPROFILE h = NULL;
    cmsMLU *desc = NULL, *cprt = NULL;
    pc_status st = PC_ERR_NOMEM;
    if (!out || !len) return PC_ERR_ARG;
    *out = NULL;
    *len = 0;
    ctx = icc_ctx(&log);
    if (!ctx) return PC_ERR_NOMEM;
    g = cmsBuildGamma(ctx, 563.0 / 256.0);
    if (!g) goto done;
    c3[0] = c3[1] = c3[2] = g;
    h = cmsCreateRGBProfileTHR(ctx, &k_white, &k_prim, c3);
    desc = cmsMLUalloc(ctx, 1);
    cprt = cmsMLUalloc(ctx, 1);
    if (!h || !desc || !cprt) goto done;
    if (!cmsMLUsetASCII(desc, "en", "US", "Adobe RGB (1998)") ||
        !cmsMLUsetASCII(cprt, "en", "US", "No copyright, use freely") ||
        !cmsWriteTag(h, cmsSigProfileDescriptionTag, desc) ||
        !cmsWriteTag(h, cmsSigCopyrightTag, cprt))
        goto done;
    st = save_profile(h, out, len);
done:
    if (desc) cmsMLUfree(desc);
    if (cprt) cmsMLUfree(cprt);
    if (h) cmsCloseProfile(h);
    if (g) cmsFreeToneCurve(g);
    cmsDeleteContext(ctx);
    return st;
}

/* Append why to the note ("; " separated) when it fits, else replace it. */
static void note_add(pc_image_meta *meta, const char *why)
{
    size_t a = strlen(meta->note), b = strlen(why);
    if (a && a + 2u + b < sizeof meta->note) {
        memcpy(meta->note + a, "; ", 2u);
        memcpy(meta->note + a + 2u, why, b + 1u);
    } else {
        lc_note(meta, why);
    }
}

/* True when every visible pixel of every layer has R == G == B. */
static bool doc_is_gray(const pc_doc *d)
{
    size_t per = (size_t)d->tiles_x * d->tiles_y;
    for (uint32_t i = 0; i < d->n_layers; i++)
        for (size_t k = 0; k < per; k++) {
            const pc_tile *t = d->stack[i]->grid[k];
            const pc_px32 *px;
            if (!t || t->bpp != 4u) continue;
            px = (const pc_px32 *)(const void *)t->data;
            for (size_t j = 0; j < (size_t)PC_TILE_DIM * PC_TILE_DIM; j++)
                if (px[j].a && (px[j].r != px[j].g || px[j].g != px[j].b)) return false;
        }
    return true;
}

/* Device classes that do not describe an image ('link', 'abst', 'nmcl'). */
static bool not_image_class(uint32_t cls)
{
    return cls == 0x6C696E6Bu || cls == 0x61627374u || cls == 0x6E6D636Cu;
}

/* A transform from in (format fin) to out builds; unoptimized, which is
 * cheap to create and fails for the same profiles the optimized one does
 * (the optimizer only ever falls back to the plain pipeline). */
static bool xf_builds(cmsContext ctx, cmsHPROFILE in, cmsUInt32Number fin, cmsHPROFILE out)
{
    cmsHTRANSFORM t;
    if (!in || !out) return false;
    t = cmsCreateTransformTHR(ctx, in, fin, out, TYPE_BGR_8, INTENT_PERCEPTUAL,
                              cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (t) cmsDeleteTransform(t);
    return t != NULL;
}

/* True when Little-CMS can build the profile's transforms: RGB to sRGB;
 * gray to sRGB, both the gray table and the one of its RGB form; CMYK to
 * sRGB. */
static bool transform_builds(const uint8_t *icc, size_t len, pc_icc_space space)
{
    icc_log log;
    cmsContext ctx;
    cmsHPROFILE in, out;
    size_t size;
    bool ok = false;
    if (icc_validate(icc, len, &size) != PC_OK) return false;
    ctx = icc_ctx(&log);
    if (!ctx) return false;
    in = cmsOpenProfileFromMemTHR(ctx, icc, (cmsUInt32Number)size);
    out = cmsCreate_sRGBProfileTHR(ctx);
    if (in && out) {
        cmsColorSpaceSignature cs = cmsGetColorSpace(in);
        if (space == PC_ICC_SPACE_RGB && cs == cmsSigRgbData) {
            ok = xf_builds(ctx, in, TYPE_RGB_8, out);
        } else if (space == PC_ICC_SPACE_GRAY && cs == cmsSigGrayData) {
            cmsHPROFILE rgb = xf_builds(ctx, in, TYPE_GRAY_8, out) ? gray_as_rgb(ctx, in) : NULL;
            ok = rgb && xf_builds(ctx, rgb, TYPE_RGB_8, out);
            if (rgb) cmsCloseProfile(rgb);
        } else if (space == PC_ICC_SPACE_CMYK && cs == cmsSigCmykData) {
            ok = xf_builds(ctx, in, TYPE_CMYK_8, out);
        }
    }
    if (in) cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    cmsDeleteContext(ctx);
    return ok;
}

pc_icc_space pc_icc_usable_space(const uint8_t *icc, size_t len)
{
    pc_icc_info info;
    if (!icc || !len || icc_inspect(icc, len, &info, false) != PC_OK) return PC_ICC_SPACE_OTHER;
    if (not_image_class(info.device_class)) return PC_ICC_SPACE_OTHER;
    if (!transform_builds(icc, len, info.space)) return PC_ICC_SPACE_OTHER;
    return info.space;
}

bool pc_icc_meta_validate(pc_image_meta *meta, const pc_doc *d)
{
    pc_icc_info info;
    const char *why = NULL;
    if (!meta || !meta->icc || !meta->icc_len) return false;
    if (pc_icc_inspect(meta->icc, meta->icc_len, &info) != PC_OK)
        why = "The embedded color profile is damaged and was ignored";
    else if (not_image_class(info.device_class))
        why = "The embedded color profile is not an image profile and was removed";
    else if (info.space == PC_ICC_SPACE_CMYK)
        why = "The embedded CMYK color profile does not match the RGB image and was removed";
    else if (info.space == PC_ICC_SPACE_OTHER)
        why = "The embedded color profile does not match the image and was removed";
    else if (!transform_builds(meta->icc, meta->icc_len, info.space))
        /* parses, but Little-CMS cannot convert with it (wave 4, FL-ICC) */
        why = "The embedded color profile cannot be used and was ignored";
    else if (info.space == PC_ICC_SPACE_GRAY && d && !doc_is_gray(d))
        why = "The embedded gray color profile does not match the color image and was removed";
    if (!why) return false;
    free(meta->icc);
    meta->icc = NULL;
    meta->icc_len = 0;
    note_add(meta, why);
    return true;
}

struct pc_icc_xform {
    icc_xf x;
};

pc_status pc_icc_xform_create(const uint8_t *icc, size_t len, pc_icc_xform **out,
                              bool *is_identity)
{
    pc_icc_xform *t;
    pc_status st;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (is_identity) *is_identity = false;
    t = (pc_icc_xform *)calloc(1u, sizeof *t);
    if (!t) return PC_ERR_NOMEM;
    st = icc_xf_make(&t->x, icc, len);
    if (st != PC_OK) { free(t); return st; }
    if (is_identity) *is_identity = icc_xf_is_identity(&t->x);
    *out = t;
    return PC_OK;
}

void pc_icc_xform_run(const pc_icc_xform *x, pc_px32 *px, size_t n)
{
    uint8_t scratch[256 * 4];
    if (!x || !px) return;
    while (n) {
        size_t k = n < 256u ? n : 256u;
        icc_xf_run(&x->x, px, k, scratch);
        px += k;
        n -= k;
    }
}

void pc_icc_xform_destroy(pc_icc_xform *x)
{
    if (!x) return;
    icc_xf_free(&x->x);
    free(x);
}

pc_status pc_icc_meta_set_srgb(pc_image_meta *meta)
{
    uint8_t *p;
    size_t n;
    pc_status st;
    if (!meta) return PC_ERR_ARG;
    st = pc_icc_srgb_profile(&p, &n);
    if (st != PC_OK) return st;
    free(meta->icc);
    meta->icc = p;
    meta->icc_len = n;
    return PC_OK;
}

/* ---- gray profiles for RGB pixels, export (lane CODEC, wave 4) ---------------------------- */
pc_status pc_icc_gray_as_rgb(const uint8_t *icc, size_t len, uint8_t **out, size_t *out_len)
{
    icc_log log;
    cmsContext ctx;
    cmsHPROFILE g = NULL, h = NULL;
    cmsMLU *desc = NULL, *cprt = NULL;
    size_t size;
    pc_status st;
    char name[160], copy[160];
    if (!out || !out_len) return PC_ERR_ARG;
    *out = NULL;
    *out_len = 0;
    st = icc_validate(icc, len, &size);
    if (st != PC_OK) return st;
    if (space_of(icc) != PC_ICC_SPACE_GRAY) return PC_ERR_UNSUPPORTED;
    ctx = icc_ctx(&log);
    if (!ctx) return PC_ERR_NOMEM;
    st = PC_ERR_FORMAT;
    g = cmsOpenProfileFromMemTHR(ctx, icc, (cmsUInt32Number)size);
    if (!g || cmsGetColorSpace(g) != cmsSigGrayData) goto done;
    h = gray_as_rgb(ctx, g);
    if (!h) goto done;
    name[0] = copy[0] = '\0';
    if (cmsGetProfileInfoASCII(g, cmsInfoDescription, "en", "US", name, 120u) == 0u)
        name[0] = '\0';
    name[119] = '\0';
    if (cmsGetProfileInfoASCII(g, cmsInfoCopyright, "en", "US", copy, 150u) == 0u)
        copy[0] = '\0';
    copy[149] = '\0';
    {
        size_t k = strlen(name);
        if (k == 0u) {
            memcpy(name, "Gray", 5u);
            k = 4u;
        }
        memcpy(name + k, " (RGB)", 7u);
    }
    st = PC_ERR_NOMEM;
    desc = cmsMLUalloc(ctx, 1);
    cprt = cmsMLUalloc(ctx, 1);
    if (!desc || !cprt) goto done;
    if (!cmsMLUsetASCII(desc, "en", "US", name) ||
        !cmsMLUsetASCII(cprt, "en", "US", copy[0] ? copy : "No copyright, use freely") ||
        !cmsWriteTag(h, cmsSigProfileDescriptionTag, desc) ||
        !cmsWriteTag(h, cmsSigCopyrightTag, cprt))
        goto done;
    st = save_profile(h, out, out_len);
done:
    if (desc) cmsMLUfree(desc);
    if (cprt) cmsMLUfree(cprt);
    if (h) cmsCloseProfile(h);
    if (g) cmsCloseProfile(g);
    cmsDeleteContext(ctx);
    return st;
}

pc_status pc_icc_embed_for(const pc_image_meta *meta, pc_icc_space pixels, pc_icc_embed *e)
{
    pc_icc_space src;
    pc_status st;
    if (!e) return PC_ERR_ARG;
    memset(e, 0, sizeof *e);
    if (pixels != PC_ICC_SPACE_RGB && pixels != PC_ICC_SPACE_GRAY) return PC_ERR_ARG;
    if (!meta || !meta->icc || !meta->icc_len) return PC_OK;
    src = pc_icc_usable_space(meta->icc, meta->icc_len);
    if (src == pixels) {
        e->icc = meta->icc;
        e->len = meta->icc_len;
        return PC_OK;
    }
    if (src != PC_ICC_SPACE_GRAY || pixels != PC_ICC_SPACE_RGB) return PC_OK;   /* dropped */
    st = pc_icc_gray_as_rgb(meta->icc, meta->icc_len, &e->owned, &e->len);
    if (st == PC_ERR_NOMEM) return st;
    if (st != PC_OK) {
        e->len = 0;
        return PC_OK;
    }
    e->icc = e->owned;
    return PC_OK;
}

void pc_icc_embed_free(pc_icc_embed *e)
{
    if (!e) return;
    free(e->owned);
    memset(e, 0, sizeof *e);
}

/* ---- CMYK (JPEG and TIFF decoders) ------------------------------------------------------ */
/* Converted colors are remembered in a direct-mapped table (exact results
 * of the unoptimized pipeline), so images with repeated CMYK values do not
 * pay the full pipeline for every pixel. */
#define CMYK_CACHE_BITS 15u
#define CMYK_CACHE_SIZE ((size_t)1u << CMYK_CACHE_BITS)

struct lc_cmyk_xf {
    cmsContext    ctx;
    cmsHTRANSFORM xf;
    icc_log       log;
    uint32_t     *key;           /* CMYK_CACHE_SIZE packed CMYK samples, or NULL */
    uint32_t     *val;           /* 0 = empty, else 0x01000000 | B << 16 | G << 8 | R */
};

lc_cmyk_xf *lc_cmyk_open(const uint8_t *icc, size_t len, bool inverted, const uint8_t *dst,
                         size_t dst_len)
{
    lc_cmyk_xf *x;
    cmsHPROFILE in, out;
    size_t size, dsize = 0;
    if (icc_validate(icc, len, &size) != PC_OK || space_of(icc) != PC_ICC_SPACE_CMYK) return NULL;
    if (dst && (icc_validate(dst, dst_len, &dsize) != PC_OK || space_of(dst) != PC_ICC_SPACE_RGB))
        return NULL;
    x = (lc_cmyk_xf *)calloc(1u, sizeof *x);
    if (!x) return NULL;
    x->ctx = icc_ctx(&x->log);
    if (!x->ctx) { free(x); return NULL; }
    in = cmsOpenProfileFromMemTHR(x->ctx, icc, (cmsUInt32Number)size);
    out = dst ? cmsOpenProfileFromMemTHR(x->ctx, dst, (cmsUInt32Number)dsize)
              : cmsCreate_sRGBProfileTHR(x->ctx);
    if (in && out && cmsGetColorSpace(in) == cmsSigCmykData &&
        cmsGetColorSpace(out) == cmsSigRgbData &&
        !not_image_class((uint32_t)cmsGetDeviceClass(in)))
        /* unoptimized: the precalculated 8-bit tables are up to 8 codes off for
         * CMYK, the full pipeline rounds once (about 0.2 to 0.4 us per pixel,
         * cached per color below) */
        x->xf = cmsCreateTransformTHR(x->ctx, in, inverted ? TYPE_CMYK_8_REV : TYPE_CMYK_8, out,
                                      TYPE_BGR_8, INTENT_PERCEPTUAL,
                                      cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (in) cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    if (!x->xf) { lc_cmyk_close(x); return NULL; }
    x->key = (uint32_t *)malloc(CMYK_CACHE_SIZE * sizeof *x->key);
    x->val = (uint32_t *)calloc(CMYK_CACHE_SIZE, sizeof *x->val);
    if (!x->key || !x->val) {               /* works without the cache */
        free(x->key);
        free(x->val);
        x->key = x->val = NULL;
    }
    return x;
}

lc_cmyk_xf *lc_cmyk_open_default(bool inverted, const uint8_t *dst, size_t dst_len)
{
    return lc_cmyk_open(lc_cmyk_swop_icc, lc_cmyk_swop_icc_size, inverted, dst, dst_len);
}

const uint8_t *pc_icc_cmyk_default_profile(size_t *len)
{
    if (len) *len = lc_cmyk_swop_icc_size;
    return lc_cmyk_swop_icc;
}

static uint32_t cmyk_slot(uint32_t k)
{
    return (uint32_t)((k * 0x9E3779B1u) >> (32u - CMYK_CACHE_BITS));
}

void lc_cmyk_run(lc_cmyk_xf *x, const uint8_t *cmyk, pc_px32 *dst, size_t n)
{
    uint8_t bgr[3 * 256], miss_in[4 * 256];
    uint32_t vals[256], keys[256];
    uint16_t miss[256];
    /* cmyk may alias dst (4 bytes per pixel in place): each chunk reads all
     * of its samples before it writes */
    while (n) {
        size_t k = n < 256u ? n : 256u, nm = 0;
        if (!x->val) {
            cmsDoTransform(x->xf, cmyk, bgr, (cmsUInt32Number)k);
            for (size_t i = 0; i < k; i++) {
                const uint8_t *c = bgr + 3u * i;
                vals[i] = 0x01000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
            }
        } else {
            for (size_t i = 0; i < k; i++) {
                const uint8_t *s = cmyk + 4u * i;
                uint32_t key = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 |
                               (uint32_t)s[3] << 24;
                uint32_t slot = cmyk_slot(key);
                keys[i] = key;
                if (x->val[slot] && x->key[slot] == key) {
                    vals[i] = x->val[slot];
                } else {
                    memcpy(miss_in + 4u * nm, s, 4u);
                    miss[nm++] = (uint16_t)i;
                }
            }
            if (nm) cmsDoTransform(x->xf, miss_in, bgr, (cmsUInt32Number)nm);
            for (size_t j = 0; j < nm; j++) {
                const uint8_t *c = bgr + 3u * j;
                uint32_t v = 0x01000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
                uint32_t slot = cmyk_slot(keys[miss[j]]);
                vals[miss[j]] = v;
                x->key[slot] = keys[miss[j]];
                x->val[slot] = v;
            }
        }
        for (size_t i = 0; i < k; i++) {
            dst[i].b = (uint8_t)(vals[i] >> 16);
            dst[i].g = (uint8_t)(vals[i] >> 8);
            dst[i].r = (uint8_t)vals[i];
            dst[i].a = 255u;
        }
        cmyk += 4u * k;
        dst += k;
        n -= k;
    }
}

void lc_cmyk_close(lc_cmyk_xf *x)
{
    if (!x) return;
    if (x->xf) cmsDeleteTransform(x->xf);
    if (x->ctx) cmsDeleteContext(x->ctx);
    free(x->key);
    free(x->val);
    free(x);
}
