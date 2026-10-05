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

/* ---- transforms to sRGB ------------------------------------------------------------------ */
typedef struct icc_xf {
    cmsContext    ctx;
    cmsHTRANSFORM xf;            /* RGB: BGRA -> BGRA; NULL for gray */
    uint8_t       gray[256][3];  /* gray: code -> B, G, R */
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
        for (size_t i = 0; i < n; i++) {
            const uint8_t *e = x->gray[px[i].g];
            px[i].b = e[0]; px[i].g = e[1]; px[i].r = e[2];
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
                if (x->space == PC_ICC_SPACE_GRAY && !(r == g && g == b)) continue;
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
pc_status pc_icc_inspect(const uint8_t *icc, size_t len, pc_icc_info *info)
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
    if (info->space == PC_ICC_SPACE_RGB || info->space == PC_ICC_SPACE_GRAY) {
        icc_xf x;
        if (icc_xf_make(&x, icc, len) == PC_OK) {
            info->is_srgb = icc_xf_is_identity(&x);
            icc_xf_free(&x);
        }
    }
    return PC_OK;
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

/* ---- CMYK (JPEG decoder) ---------------------------------------------------------------- */
struct lc_cmyk_xf {
    cmsContext    ctx;
    cmsHTRANSFORM xf;
    icc_log       log;
};

lc_cmyk_xf *lc_cmyk_open(const uint8_t *icc, size_t len, bool inverted)
{
    lc_cmyk_xf *x;
    cmsHPROFILE in, out;
    size_t size;
    if (icc_validate(icc, len, &size) != PC_OK || space_of(icc) != PC_ICC_SPACE_CMYK) return NULL;
    x = (lc_cmyk_xf *)calloc(1u, sizeof *x);
    if (!x) return NULL;
    x->ctx = icc_ctx(&x->log);
    if (!x->ctx) { free(x); return NULL; }
    in = cmsOpenProfileFromMemTHR(x->ctx, icc, (cmsUInt32Number)size);
    out = cmsCreate_sRGBProfileTHR(x->ctx);
    if (in && out && cmsGetColorSpace(in) == cmsSigCmykData)
        x->xf = cmsCreateTransformTHR(x->ctx, in, inverted ? TYPE_CMYK_8_REV : TYPE_CMYK_8, out,
                                      TYPE_BGR_8, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
    if (in) cmsCloseProfile(in);
    if (out) cmsCloseProfile(out);
    if (!x->xf) { lc_cmyk_close(x); return NULL; }
    return x;
}

void lc_cmyk_run(lc_cmyk_xf *x, const uint8_t *cmyk, pc_px32 *dst, size_t n)
{
    uint8_t bgr[3 * 256];
    while (n) {
        size_t k = n < 256u ? n : 256u;
        cmsDoTransform(x->xf, cmyk, bgr, (cmsUInt32Number)k);
        for (size_t i = 0; i < k; i++) {
            dst[i].b = bgr[3 * i]; dst[i].g = bgr[3 * i + 1]; dst[i].r = bgr[3 * i + 2];
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
    free(x);
}
