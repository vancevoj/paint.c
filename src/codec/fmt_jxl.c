/* fmt_jxl.c - JPEG XL through libjxl (lane AVIFJXL, ADR-019).
 *
 * Built against libjxl >= 0.7 when PC_HAVE_JXL is defined (cmake/PcAvifJxl.cmake:
 * system package or the pinned bundled build). Without it the codec stays
 * registered with no load/save flags, so the app hides the file type.
 *
 * Load (FL-FIRSTFRAME, FL-ICC, FL-META):
 *  - Limits: the canvas size from the basic info (after orientation) is
 *    checked before any frame is decoded, and every allocation libjxl
 *    makes goes through a counting memory manager capped at
 *    pc_codec_limits.max_mem (P-08).
 *  - The first displayed frame is decoded (animations: coalesced frame 0,
 *    meta.note says so); layers are composited by libjxl (coalescing).
 *  - The codestream orientation is applied by libjxl; premultiplied alpha
 *    is unpremultiplied.
 *  - High bit depth and float images are rounded to 8 bits. Lossy (XYB)
 *    images are decoded into their original color space; the profile of
 *    the delivered pixels goes to meta.icc unless it is sRGB. PQ and HLG
 *    (HDR) images are decoded at 16 bits and tone mapped to 8-bit sRGB
 *    (axj_hdr_to_srgb8), the same mapping the AVIF codec uses.
 *  - Exif and XMP boxes (also Brotli-compressed 'brob' boxes) become meta
 *    items "exif" and "xmp" (avifjxl_meta.h); Exif resolution fills
 *    meta.dpi.
 *
 * Save (options of the JPEG XL file type bundled with Paint.NET 5.1,
 * docs/inventory/FILES.md): Quality 0..100 (90; disabled when Lossless),
 * Lossless (off), Effort 1..9 (7). Quality maps to the Butteraugli
 * distance like libjxl's GIMP plugin (q >= 30: 0.1 + (100 - q) * 0.09, so
 * 90 is distance 1.0; below 30 a steeper curve up to 15). Lossless keeps
 * the original profile (no XYB). Gray images are written with one color
 * channel and opaque images without alpha. The image profile (meta.icc)
 * is embedded, else the image is tagged sRGB. Exif goes into an 'Exif'
 * box (Orientation 1) and XMP into an 'xml ' box.
 *
 * Threads: load and save are reentrant. Save runs libjxl's parallel work
 * on pc_par (a JxlParallelRunner over pc_par_for); the output does not
 * depend on the thread count. Load runs on the calling thread only.
 */
#include "lib_codec.h"
#include "avifjxl_meta.h"
#include "pc/pc_icc.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t k_jxl_container[12] = {
    0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
};

static bool jxl_sniff(const uint8_t *p, size_t n)
{
    if (!p) return false;
    if (n >= 2u && p[0] == 0xFFu && p[1] == 0x0Au) return true;       /* bare codestream */
    return n >= 12u && memcmp(p, k_jxl_container, 12u) == 0;
}

/* ---- save options ---------------------------------------------------------------------- */
typedef struct jxl_params {
    int32_t quality;      /* 0..100 */
    int32_t lossless;     /* bool */
    int32_t effort;       /* 1..9 */
} jxl_params;

static const fx_prop k_jxl_props[] = {
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(jxl_params, quality),
      0, 100, 90, 1, NULL, NULL, 0, 0, "lossless=0" },
    { "lossless", "Lossless", FXP_BOOL, (uint32_t)offsetof(jxl_params, lossless),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
    { "effort", "Effort", FXP_INT, (uint32_t)offsetof(jxl_params, effort),
      1, 9, 7, 1, NULL, NULL, 0, 0, NULL },
};
#define N_JXL_PROPS ((uint32_t)(sizeof k_jxl_props / sizeof k_jxl_props[0]))

#if defined(PC_HAVE_JXL)
/* ======================================================================================= */
#include "jxl/decode.h"
#include "jxl/encode.h"
#include "jxl/parallel_runner.h"
#include "jxl/version.h"
#if defined(PC_JXL_HAVE_CMS)
#include "jxl/cms.h"
#endif

#define AXJ_JXL_VER (JPEGXL_MAJOR_VERSION * 10000 + JPEGXL_MINOR_VERSION * 100 + \
                     JPEGXL_PATCH_VERSION)

/* libjxl 0.9 dropped the pixel format argument of the color queries. */
#if AXJ_JXL_VER >= 900
#define JXL_GET_ENCODED(dec, tgt, enc) JxlDecoderGetColorAsEncodedProfile(dec, tgt, enc)
#define JXL_ICC_SIZE(dec, tgt, sz) JxlDecoderGetICCProfileSize(dec, tgt, sz)
#define JXL_ICC_GET(dec, tgt, p, n) JxlDecoderGetColorAsICCProfile(dec, tgt, p, n)
#else
#define JXL_GET_ENCODED(dec, tgt, enc) JxlDecoderGetColorAsEncodedProfile(dec, NULL, tgt, enc)
#define JXL_ICC_SIZE(dec, tgt, sz) JxlDecoderGetICCProfileSize(dec, NULL, tgt, sz)
#define JXL_ICC_GET(dec, tgt, p, n) JxlDecoderGetColorAsICCProfile(dec, NULL, tgt, p, n)
#endif

/* ---- counting memory manager (decoder) --------------------------------------------------- */
#define MM_HDR 16u

typedef struct jxl_mm {
    uint64_t used, budget;
    bool     over;           /* an allocation was refused by the budget */
    bool     oom;            /* malloc failed */
} jxl_mm;

static void *mm_alloc(void *opaque, size_t size)
{
    jxl_mm *m = (jxl_mm *)opaque;
    size_t total;
    uint8_t *p;
    if (!pc_add_size(size, MM_HDR, &total) || (uint64_t)total > m->budget - m->used) {
        m->over = true;
        return NULL;
    }
    p = (uint8_t *)malloc(total);
    if (!p) { m->oom = true; return NULL; }
    memcpy(p, &total, sizeof total);
    m->used += total;
    return p + MM_HDR;
}

static void mm_free(void *opaque, void *address)
{
    jxl_mm *m = (jxl_mm *)opaque;
    uint8_t *p;
    size_t total;
    if (!address) return;
    p = (uint8_t *)address - MM_HDR;
    memcpy(&total, p, sizeof total);
    m->used -= total;
    free(p);
}

/* ---- decoding ------------------------------------------------------------------------------ */
static bool enc_is_srgb(const JxlColorEncoding *e)
{
    if (e->transfer_function != JXL_TRANSFER_FUNCTION_SRGB) return false;
    if (e->color_space == JXL_COLOR_SPACE_GRAY) return e->white_point == JXL_WHITE_POINT_D65;
    return e->color_space == JXL_COLOR_SPACE_RGB && e->white_point == JXL_WHITE_POINT_D65 &&
           e->primaries == JXL_PRIMARIES_SRGB;
}

typedef struct jxl_meta_box {
    uint8_t *buf;
    size_t   cap;            /* allocated bytes */
    size_t   set_at;         /* offset of the buffer last given to libjxl */
    int      kind;           /* 0 none, 1 Exif, 2 xml */
} jxl_meta_box;

/* Bytes of the current box written so far (releases the buffer). */
static size_t box_release(JxlDecoder *dec, const jxl_meta_box *b)
{
    size_t left = JxlDecoderReleaseBoxBuffer(dec);
    size_t given = b->cap - b->set_at;
    return b->set_at + (left <= given ? given - left : 0u);
}

static pc_status box_flush(JxlDecoder *dec, jxl_meta_box *b, pc_image_meta *meta, bool *got_exif,
                           bool *got_xmp)
{
    pc_status st = PC_OK;
    if (b->kind && b->buf) {
        size_t len = box_release(dec, b);
        if (b->kind == 1 && !*got_exif) {
            st = axj_meta_put_exif(meta, b->buf, len);
            *got_exif = true;
        } else if (b->kind == 2 && !*got_xmp) {
            st = axj_meta_put_xmp(meta, b->buf, len);
            *got_xmp = true;
        }
    }
    free(b->buf);
    memset(b, 0, sizeof *b);
    return st;
}

/* Collect the first Exif and XMP boxes of a container. Only box events are
 * subscribed, so libjxl skips the codestream without decoding pixels. */
static pc_status jxl_read_boxes(const uint8_t *p, size_t n, jxl_mm *mm, pc_image_meta *meta)
{
    JxlMemoryManager jm;
    JxlDecoder *dec;
    jxl_meta_box b;
    bool got_exif = false, got_xmp = false;
    pc_status st = PC_OK;
    jm.opaque = mm;
    jm.alloc = mm_alloc;
    jm.free = mm_free;
    dec = JxlDecoderCreate(&jm);
    if (!dec) return mm->over ? PC_ERR_LIMIT : PC_ERR_NOMEM;
    memset(&b, 0, sizeof b);
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BOX) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, p, n) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return PC_ERR_FORMAT;
    }
    (void)JxlDecoderSetDecompressBoxes(dec, JXL_TRUE);
    JxlDecoderCloseInput(dec);
    while (st == PC_OK) {
        JxlDecoderStatus s = JxlDecoderProcessInput(dec);
        if (s == JXL_DEC_BOX) {
            JxlBoxType type;
            st = box_flush(dec, &b, meta, &got_exif, &got_xmp);
            if (st != PC_OK || JxlDecoderGetBoxType(dec, type, JXL_TRUE) != JXL_DEC_SUCCESS)
                continue;
            if (memcmp(type, "Exif", 4u) == 0 && !got_exif) b.kind = 1;
            else if (memcmp(type, "xml ", 4u) == 0 && !got_xmp) b.kind = 2;
            if (!b.kind) continue;
            b.cap = 65536u;
            b.buf = (uint8_t *)malloc(b.cap);
            if (!b.buf) { st = PC_ERR_NOMEM; continue; }
            if (JxlDecoderSetBoxBuffer(dec, b.buf, b.cap) != JXL_DEC_SUCCESS) {
                free(b.buf);
                memset(&b, 0, sizeof b);
            }
        } else if (s == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            size_t used, ncap;
            uint8_t *nb;
            if (!b.kind || !b.buf) continue;
            used = box_release(dec, &b);
            if (b.cap >= AXJ_META_MAX) {                  /* too large: skip the rest */
                free(b.buf);
                memset(&b, 0, sizeof b);
                continue;
            }
            ncap = b.cap * 2u > AXJ_META_MAX ? AXJ_META_MAX : b.cap * 2u;
            nb = (uint8_t *)realloc(b.buf, ncap);
            if (!nb) { st = PC_ERR_NOMEM; continue; }
            b.buf = nb;
            b.cap = ncap;
            b.set_at = used;
            if (JxlDecoderSetBoxBuffer(dec, b.buf + used, ncap - used) != JXL_DEC_SUCCESS) {
                free(b.buf);
                memset(&b, 0, sizeof b);
            }
        } else {
            /* JXL_DEC_SUCCESS, or an error that the pixel pass reports */
            st = box_flush(dec, &b, meta, &got_exif, &got_xmp);
            break;
        }
    }
    free(b.buf);
    JxlDecoderDestroy(dec);
    return st;
}

typedef struct jxl_dec_state {
    JxlBasicInfo info;
    uint32_t     w, h;
    bool         hdr;
    axj_hdr_tf   tf;
    axj_hdr_prim prim;
    uint32_t     nch;          /* channels in the output buffer: 2 (gray + alpha) or 4 */
    bool         u16;          /* 16-bit samples (HDR path) */
} jxl_dec_state;

/* Color decisions at JXL_DEC_COLOR_ENCODING; fills meta->icc. */
static pc_status jxl_color(JxlDecoder *dec, jxl_dec_state *s, pc_image_meta *meta)
{
    JxlColorEncoding orig, data;
    bool have_orig = JXL_GET_ENCODED(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &orig) ==
                     JXL_DEC_SUCCESS;
    bool xyb = !s->info.uses_original_profile;
    size_t sz = 0;
    if (have_orig && (orig.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                      orig.transfer_function == JXL_TRANSFER_FUNCTION_HLG) &&
        (orig.color_space == JXL_COLOR_SPACE_RGB || orig.color_space == JXL_COLOR_SPACE_GRAY)) {
        /* HDR: deliver the original signal at 16 bits and tone map it */
        if (xyb && JxlDecoderSetPreferredColorProfile(dec, &orig) != JXL_DEC_SUCCESS)
            return PC_ERR_UNSUPPORTED;
        s->hdr = true;
        s->u16 = true;
        s->tf = orig.transfer_function == JXL_TRANSFER_FUNCTION_PQ ? AXJ_TF_PQ : AXJ_TF_HLG;
        s->prim = orig.primaries == JXL_PRIMARIES_2100 ? AXJ_PRIM_BT2020
                  : (orig.primaries == JXL_PRIMARIES_P3 ? AXJ_PRIM_P3 : AXJ_PRIM_BT709);
        lc_note(meta, "HDR image (PQ or HLG) tone mapped to 8-bit sRGB");
        return PC_OK;
    }
    if (xyb && have_orig) {
        (void)JxlDecoderSetPreferredColorProfile(dec, &orig);
    } else if (xyb) {
        /* the original profile is an ICC profile */
        bool in_icc = false;
#if defined(PC_JXL_HAVE_CMS) && AXJ_JXL_VER >= 900
        if (JXL_ICC_SIZE(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &sz) == JXL_DEC_SUCCESS &&
            sz > 0u && sz <= ((size_t)64 << 20)) {
            /* let the CMS deliver the pixels in that profile */
            uint8_t *icc = (uint8_t *)malloc(sz);
            if (!icc) return PC_ERR_NOMEM;
            in_icc = JXL_ICC_GET(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, icc, sz) ==
                         JXL_DEC_SUCCESS &&
                     JxlDecoderSetCms(dec, *JxlGetDefaultCms()) == JXL_DEC_SUCCESS &&
                     JxlDecoderSetOutputColorProfile(dec, NULL, icc, sz) == JXL_DEC_SUCCESS;
            free(icc);
        }
#endif
        if (!in_icc) {
            /* Without a color management system libjxl delivers lossy (XYB)
             * pixels only in a structured encoding: take sRGB, no profile. */
            JxlColorEncoding srgb;
            JxlColorEncodingSetToSRGB(&srgb, s->info.num_color_channels == 1u ? JXL_TRUE
                                                                               : JXL_FALSE);
            if (JxlDecoderSetPreferredColorProfile(dec, &srgb) == JXL_DEC_SUCCESS) {
                lc_note(meta, "Lossy JPEG XL with an ICC profile: converted to sRGB");
                return PC_OK;
            }
        }
    }
    /* the profile of the delivered pixels */
    if (JXL_GET_ENCODED(dec, JXL_COLOR_PROFILE_TARGET_DATA, &data) == JXL_DEC_SUCCESS &&
        enc_is_srgb(&data))
        return PC_OK;
    sz = 0;
    if (JXL_ICC_SIZE(dec, JXL_COLOR_PROFILE_TARGET_DATA, &sz) == JXL_DEC_SUCCESS && sz > 0u &&
        sz <= ((size_t)64 << 20)) {
        meta->icc = (uint8_t *)malloc(sz);
        if (!meta->icc) return PC_ERR_NOMEM;
        if (JXL_ICC_GET(dec, JXL_COLOR_PROFILE_TARGET_DATA, meta->icc, sz) == JXL_DEC_SUCCESS) {
            meta->icc_len = sz;
        } else {
            free(meta->icc);
            meta->icc = NULL;
        }
    }
    return PC_OK;
}

/* Output buffer rows [y0, y0 + n) as BGRA. */
static void jxl_rows_to_bgra(const jxl_dec_state *s, const uint8_t *buf, uint32_t y0,
                             uint32_t n, pc_px32 *dst, uint16_t *tmp16)
{
    size_t k = (size_t)s->w * n;
    if (s->u16) {
        const uint16_t *src = (const uint16_t *)(const void *)buf + (size_t)y0 * s->w * s->nch;
        if (s->nch == 4u) {
            memcpy(tmp16, src, k * 8u);
        } else {
            for (size_t i = 0; i < k; i++) {
                tmp16[i * 4] = tmp16[i * 4 + 1] = tmp16[i * 4 + 2] = src[i * 2];
                tmp16[i * 4 + 3] = src[i * 2 + 1];
            }
        }
        if (s->hdr) {
            axj_hdr_to_srgb8(tmp16, dst, k, s->tf, s->prim, 0.0);
        } else {
            for (size_t i = 0; i < k; i++) {
                dst[i].r = lc_u16_to_u8(tmp16[i * 4]);
                dst[i].g = lc_u16_to_u8(tmp16[i * 4 + 1]);
                dst[i].b = lc_u16_to_u8(tmp16[i * 4 + 2]);
                dst[i].a = lc_u16_to_u8(tmp16[i * 4 + 3]);
            }
        }
        return;
    }
    {
        const uint8_t *src = buf + (size_t)y0 * s->w * s->nch;
        if (s->nch == 4u) {
            lc_rgba_to_bgra(dst, src, k);
        } else {
            for (size_t i = 0; i < k; i++) {
                dst[i].r = dst[i].g = dst[i].b = src[i * 2];
                dst[i].a = src[i * 2 + 1];
            }
        }
    }
}

static pc_status dec_fail(const jxl_mm *mm)
{
    if (mm->over) return PC_ERR_LIMIT;
    if (mm->oom) return PC_ERR_NOMEM;
    return PC_ERR_FORMAT;
}

static pc_status jxl_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    JxlMemoryManager jm;
    JxlDecoder *dec = NULL;
    jxl_mm mm;
    jxl_dec_state s;
    pc_doc *d = NULL;
    pc_layer *layer = NULL;
    uint8_t *buf = NULL;
    pc_px32 *band = NULL;
    uint16_t *tmp16 = NULL;
    size_t buf_size = 0;
    bool done = false;
    pc_status st = PC_OK;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    memset(&s, 0, sizeof s);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (!jxl_sniff(p, n)) return PC_ERR_FORMAT;
    memset(&mm, 0, sizeof mm);
    mm.budget = lim->max_mem;
    if (n >= 12u && memcmp(p, k_jxl_container, 12u) == 0) {
        st = jxl_read_boxes(p, n, &mm, meta);
        if (st != PC_OK) goto fail;
        mm.over = mm.oom = false;
    }
    jm.opaque = &mm;
    jm.alloc = mm_alloc;
    jm.free = mm_free;
    dec = JxlDecoderCreate(&jm);
    if (!dec) { st = mm.over ? PC_ERR_LIMIT : PC_ERR_NOMEM; goto fail; }
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                       JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetKeepOrientation(dec, JXL_FALSE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, p, n) != JXL_DEC_SUCCESS) {
        st = PC_ERR_STATE;
        goto fail;
    }
    JxlDecoderCloseInput(dec);
    while (!done && st == PC_OK) {
        JxlDecoderStatus ds = JxlDecoderProcessInput(dec);
        switch (ds) {
        case JXL_DEC_BASIC_INFO: {
            uint64_t need;
            if (JxlDecoderGetBasicInfo(dec, &s.info) != JXL_DEC_SUCCESS) {
                st = PC_ERR_FORMAT;
                break;
            }
            s.w = s.info.orientation > 4 ? s.info.ysize : s.info.xsize;
            s.h = s.info.orientation > 4 ? s.info.xsize : s.info.ysize;
            st = pc_codec_check_size(lim, s.w, s.h, 1u);
            if (st != PC_OK) break;
            /* output buffer (up to 16-bit RGBA) plus the document */
            need = (uint64_t)s.w * s.h * 12u;
            if (need > lim->max_mem) { st = PC_ERR_LIMIT; break; }
            meta->src_bits = s.info.bits_per_sample;
            meta->had_alpha = s.info.alpha_bits > 0u;
            if (s.info.have_animation)
                lc_note(meta, "Animated JPEG XL: only the first frame was loaded");
            break;
        }
        case JXL_DEC_COLOR_ENCODING:
            st = jxl_color(dec, &s, meta);
            break;
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            JxlPixelFormat pf;
            size_t want;
            if (buf) { st = PC_ERR_FORMAT; break; }        /* one frame only */
            s.nch = s.info.num_color_channels == 1u ? 2u : 4u;
            pf.num_channels = s.nch;
            pf.data_type = s.u16 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
            pf.endianness = JXL_NATIVE_ENDIAN;
            pf.align = 0;
            if (JxlDecoderImageOutBufferSize(dec, &pf, &buf_size) != JXL_DEC_SUCCESS) {
                st = PC_ERR_FORMAT;
                break;
            }
            want = (size_t)s.w * s.h * s.nch * (s.u16 ? 2u : 1u);
            if (buf_size != want) { st = PC_ERR_FORMAT; break; }
            buf = (uint8_t *)lc_alloc(buf_size, 1u, lim, &st);
            if (!buf) break;
            if (JxlDecoderSetImageOutBuffer(dec, &pf, buf, buf_size) != JXL_DEC_SUCCESS)
                st = PC_ERR_FORMAT;
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            done = buf != NULL;
            if (!done) st = PC_ERR_FORMAT;
            break;
        case JXL_DEC_SUCCESS:
            if (!buf) st = PC_ERR_FORMAT;
            done = true;
            break;
        case JXL_DEC_NEED_MORE_INPUT:         /* truncated (input is closed) */
            st = PC_ERR_FORMAT;
            break;
        case JXL_DEC_ERROR:
            st = dec_fail(&mm);
            break;
        default:
            break;
        }
    }
    if (st != PC_OK) goto fail;
    JxlDecoderDestroy(dec);
    dec = NULL;
    st = lc_doc_new(lim, s.w, s.h, 1u, &d, &layer);
    if (st != PC_OK) goto fail;
    band = (pc_px32 *)lc_alloc((size_t)s.w * (size_t)LC_BAND, sizeof *band, lim, &st);
    if (band && s.u16)
        tmp16 = (uint16_t *)lc_alloc((size_t)s.w * (size_t)LC_BAND * 4u, 2u, lim, &st);
    for (uint32_t y0 = 0; y0 < s.h && st == PC_OK; y0 += (uint32_t)LC_BAND) {
        uint32_t nb = s.h - y0 < (uint32_t)LC_BAND ? s.h - y0 : (uint32_t)LC_BAND;
        jxl_rows_to_bgra(&s, buf, y0, nb, band, tmp16);
        st = pc_layer_store_rect(d, layer, pc_rect_make(0, (int32_t)y0, (int32_t)s.w,
                                                        (int32_t)nb), band, s.w);
    }
    if (st != PC_OK) goto fail;
    free(band);
    free(tmp16);
    free(buf);
    *out = d;
    return PC_OK;
fail:
    if (dec) JxlDecoderDestroy(dec);
    free(band);
    free(tmp16);
    free(buf);
    pc_doc_destroy(d);
    pc_meta_free(meta);
    return st;
}

/* ---- encoding ------------------------------------------------------------------------------ */
/* Butteraugli distance for a 0..100 quality, as libjxl's GIMP plugin maps it. */
static float jxl_distance(int32_t q)
{
    double dist;
    if (q >= 100) return 0.1f;
    if (q >= 30) {
        dist = 0.1 + (double)(100 - q) * 0.09;
    } else if (q <= 8) {
        dist = 15.0;
    } else {
        dist = 6.4 + pow(2.5, (double)(30 - q) / 5.0) / 6.25;
        if (dist > 15.0) dist = 15.0;
    }
    return (float)dist;
}

typedef struct jxl_par_job {
    JxlParallelRunFunction fn;
    void                  *opaque;
    uint32_t               start;
} jxl_par_job;

static void jxl_par_job_fn(void *ud, uint32_t index, uint32_t worker)
{
    const jxl_par_job *j = (const jxl_par_job *)ud;
    j->fn(j->opaque, j->start + index, (size_t)worker);
}

/* JxlParallelRunner over pc_par (runner_opaque = const pc_par *). */
static JxlParallelRetCode jxl_par_runner(void *runner_opaque, void *jpegxl_opaque,
                                         JxlParallelRunInit init, JxlParallelRunFunction func,
                                         uint32_t start_range, uint32_t end_range)
{
    const pc_par *par = (const pc_par *)runner_opaque;
    jxl_par_job job;
    JxlParallelRetCode rc = init(jpegxl_opaque, (size_t)pc_par_threads(par));
    if (rc != 0) return rc;
    if (end_range <= start_range) return 0;
    job.fn = func;
    job.opaque = jpegxl_opaque;
    job.start = start_range;
    pc_par_for(par, jxl_par_job_fn, &job, end_range - start_range);
    return 0;
}

static pc_status jxl_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    jxl_params prm;
    JxlEncoder *enc = NULL;
    JxlEncoderFrameSettings *fs;
    JxlBasicInfo bi;
    JxlPixelFormat pf;
    uint8_t *px = NULL, *exif = NULL, *xmp = NULL;
    pc_px32 *band = NULL;
    size_t total, exif_len = 0, xmp_len = 0, nout;
    bool alpha = false, gray = true, use_icc = false;
    uint32_t nc, ch;
    pc_status st = PC_OK;
    lc_flat flat;
    if (!d || !out) return PC_ERR_ARG;
    if (!d->w || !d->h) return PC_ERR_LIMIT;
    prm.quality = 90;
    prm.lossless = 0;
    prm.effort = 7;
    if (params) memcpy(&prm, params, sizeof prm);
    prm.quality = prm.quality < 0 ? 0 : (prm.quality > 100 ? 100 : prm.quality);
    prm.effort = prm.effort < 1 ? 1 : (prm.effort > 9 ? 9 : prm.effort);
    /* flatten into RGBA, noting alpha and gray */
    if (!pc_mul_size((size_t)d->w * d->h, 4u, &total)) return PC_ERR_LIMIT;
    px = (uint8_t *)malloc(total);
    band = (pc_px32 *)malloc((size_t)d->w * (size_t)LC_BAND * sizeof *band);
    if (!px || !band) { st = PC_ERR_NOMEM; goto done; }
    flat.d = d;
    flat.par = par;
    flat.over_white = false;
    for (int32_t y0 = 0; y0 < (int32_t)d->h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        size_t k = (size_t)d->w * (size_t)nb;
        st = lc_src_flatten(&flat, y0, nb, band);
        if (st != PC_OK) break;
        for (size_t i = 0; i < k; i++) {
            if (band[i].a != 255u) alpha = true;
            if (band[i].r != band[i].g || band[i].g != band[i].b) gray = false;
        }
        lc_bgra_to_rgba(px + (size_t)y0 * d->w * 4u, band, k);
    }
    free(band);
    band = NULL;
    if (st != PC_OK) goto done;
    if (meta && meta->icc && meta->icc_len) {
        pc_icc_info info;
        if (pc_icc_inspect(meta->icc, meta->icc_len, &info) == PC_OK) {
            if (info.space == PC_ICC_SPACE_RGB) { use_icc = true; gray = false; }
            else if (info.space == PC_ICC_SPACE_GRAY && gray) use_icc = true;
        }
    }
    /* compact in place to the channels actually written */
    nc = gray ? 1u : 3u;
    ch = nc + (alpha ? 1u : 0u);
    if (ch != 4u) {
        size_t npx = (size_t)d->w * d->h;
        for (size_t i = 0; i < npx; i++) {
            const uint8_t *s = px + i * 4u;
            uint8_t *t = px + i * ch;
            uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
            if (gray) { t[0] = g; if (alpha) t[1] = a; }
            else { t[0] = r; t[1] = g; t[2] = b; }
        }
    }
    nout = (size_t)d->w * d->h * ch;
    enc = JxlEncoderCreate(NULL);
    if (!enc) { st = PC_ERR_NOMEM; goto done; }
    if (pc_par_threads(par) > 1u &&
        JxlEncoderSetParallelRunner(enc, jxl_par_runner, (void *)(uintptr_t)par) !=
            JXL_ENC_SUCCESS) {
        st = PC_ERR_STATE;
        goto done;
    }
    if (meta) {
        exif = axj_meta_get_exif(meta, &exif_len);
        xmp = axj_meta_get_xmp(meta, &xmp_len);
    }
    if ((exif || xmp) && JxlEncoderUseBoxes(enc) != JXL_ENC_SUCCESS) {
        st = PC_ERR_STATE;
        goto done;
    }
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = d->w;
    bi.ysize = d->h;
    bi.bits_per_sample = 8;
    bi.exponent_bits_per_sample = 0;
    bi.num_color_channels = nc;
    bi.num_extra_channels = alpha ? 1u : 0u;
    bi.alpha_bits = alpha ? 8u : 0u;
    bi.alpha_exponent_bits = 0;
    bi.alpha_premultiplied = JXL_FALSE;
    bi.uses_original_profile = prm.lossless ? JXL_TRUE : JXL_FALSE;
    bi.orientation = JXL_ORIENT_IDENTITY;
    if (JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS) { st = PC_ERR_ARG; goto done; }
    if (use_icc) {
        if (JxlEncoderSetICCProfile(enc, meta->icc, meta->icc_len) != JXL_ENC_SUCCESS)
            use_icc = false;
    }
    if (!use_icc) {
        JxlColorEncoding ce;
        JxlColorEncodingSetToSRGB(&ce, gray ? JXL_TRUE : JXL_FALSE);
        if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) { st = PC_ERR_ARG; goto done; }
    }
    if (exif && exif_len) {
        /* Exif box payload: 4-byte offset of the TIFF header, then the block */
        uint8_t *box = (uint8_t *)malloc(exif_len + 4u);
        if (!box) { st = PC_ERR_NOMEM; goto done; }
        memset(box, 0, 4u);
        memcpy(box + 4, exif, exif_len);
        if (JxlEncoderAddBox(enc, "Exif", box, exif_len + 4u, JXL_FALSE) != JXL_ENC_SUCCESS)
            st = PC_ERR_ARG;
        free(box);
        if (st != PC_OK) goto done;
    }
    if (xmp && xmp_len &&
        JxlEncoderAddBox(enc, "xml ", xmp, xmp_len, JXL_FALSE) != JXL_ENC_SUCCESS) {
        st = PC_ERR_ARG;
        goto done;
    }
    if (exif || xmp) JxlEncoderCloseBoxes(enc);
    fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    if (!fs ||
        JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, prm.effort) !=
            JXL_ENC_SUCCESS) {
        st = PC_ERR_ARG;
        goto done;
    }
    if (prm.lossless) {
        if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS) {
            st = PC_ERR_ARG;
            goto done;
        }
    } else if (JxlEncoderSetFrameDistance(fs, jxl_distance(prm.quality)) != JXL_ENC_SUCCESS) {
        st = PC_ERR_ARG;
        goto done;
    }
    pf.num_channels = ch;
    pf.data_type = JXL_TYPE_UINT8;
    pf.endianness = JXL_NATIVE_ENDIAN;
    pf.align = 0;
    if (JxlEncoderAddImageFrame(fs, &pf, px, nout) != JXL_ENC_SUCCESS) {
        st = PC_ERR_ARG;
        goto done;
    }
    JxlEncoderCloseInput(enc);
    free(px);
    px = NULL;
    for (;;) {
        uint8_t *next;
        size_t avail;
        JxlEncoderStatus es;
        st = pc_buf_reserve(out, (size_t)65536u);
        if (st != PC_OK) break;
        next = out->p + out->n;
        avail = out->cap - out->n;
        es = JxlEncoderProcessOutput(enc, &next, &avail);
        out->n = (size_t)(next - out->p);
        if (es == JXL_ENC_SUCCESS) break;
        if (es != JXL_ENC_NEED_MORE_OUTPUT) { st = PC_ERR_STATE; break; }
    }
done:
    if (enc) JxlEncoderDestroy(enc);
    free(exif);
    free(xmp);
    free(band);
    free(px);
    return st;
}

#define JXL_FLAGS (PC_CODEC_LOAD | PC_CODEC_SAVE)
#define JXL_LOAD_FN jxl_load
#define JXL_SAVE_FN jxl_save

#else /* !PC_HAVE_JXL ======================================================================== */

/* Library absent: registered without flags, so the app hides the type. */
#define JXL_FLAGS 0u
#define JXL_LOAD_FN NULL
#define JXL_SAVE_FN NULL
#endif

const pc_codec pc_codec_jxl = {
    "jxl", "JPEG XL", "jxl", JXL_FLAGS,
    jxl_sniff, JXL_LOAD_FN,
    k_jxl_props, N_JXL_PROPS, (uint32_t)sizeof(jxl_params),
    JXL_SAVE_FN
};
