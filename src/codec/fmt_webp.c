/* fmt_webp.c - WebP through libwebp 1.6 (lane L6B).
 *
 * Load: the canvas size is checked (WebP itself caps sides at 16383)
 * before any pixel memory is allocated. Still images decode into one BGRA
 * buffer (libwebp has no public row-streaming output), then go to the
 * layer in bands. Animated files load their first frame, composited on
 * the canvas by WebPAnimDecoder. The ICCP chunk goes to meta.icc, the EXIF
 * and XMP chunks become the "exif" and "xmp" items (cmeta.h); the EXIF
 * orientation is applied to the pixels and reset to 1.
 *
 * Save (options of the WebP file type bundled with Paint.NET 5.1): preset
 * (Default, Picture, Photo, Drawing, Icon, Text; default Photo), quality
 * 0..100 (default 95, unused when lossless), effort 0..9 (default 7; the
 * libwebp method for lossy, the lossless preset level for lossless) and a
 * lossless switch. Lossless output keeps exact RGB under transparency. The
 * embedded profile and the EXIF and XMP items are muxed in as ICCP, EXIF
 * and XMP chunks.
 *
 * Threads: reentrant; libwebp's own worker threads are not used.
 */
#include "lib_codec.h"
#include "cmeta.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "webp/decode.h"
#include "webp/demux.h"
#include "webp/encode.h"
#include "webp/mux.h"

#define WEBP_MAX_SIDE 16383u

static bool webp_sniff(const uint8_t *p, size_t n)
{
    return p && n >= 12u && memcmp(p, "RIFF", 4u) == 0 && memcmp(p + 8, "WEBP", 4u) == 0;
}

/* ---- decoding ----------------------------------------------------------------------- */
static pc_status store_bands(pc_doc *d, pc_layer *l, const uint8_t *bgra, size_t stride_bytes)
{
    pc_status st = PC_OK;
    for (int32_t y0 = 0; y0 < (int32_t)d->h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        st = pc_layer_store_rect(d, l, pc_rect_make(0, y0, (int32_t)d->w, nb),
                                 (const pc_px32 *)(const void *)(bgra + (size_t)y0 * stride_bytes),
                                 stride_bytes / 4u);
    }
    return st;
}

static pc_status webp_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                           pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    WebPBitstreamFeatures feat;
    WebPData data;
    WebPDemuxer *dmx;
    pc_doc *d = NULL;
    pc_layer *layer = NULL;
    pc_status st;
    uint32_t cw, ch;
    int orient = 1;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (WebPGetFeatures(p, n, &feat) != VP8_STATUS_OK) return PC_ERR_FORMAT;
    data.bytes = p;
    data.size = n;
    dmx = WebPDemux(&data);
    if (!dmx) return PC_ERR_FORMAT;
    cw = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_WIDTH);
    ch = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_HEIGHT);
    if (cw > WEBP_MAX_SIDE || ch > WEBP_MAX_SIDE) { WebPDemuxDelete(dmx); return PC_ERR_FORMAT; }
    st = pc_codec_check_size(lim, cw, ch, 1u);
    if (st == PC_OK && (WebPDemuxGetI(dmx, WEBP_FF_FORMAT_FLAGS) & XMP_FLAG)) {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(dmx, "XMP ", 1, &it)) {
            st = cm_meta_load_xmp(meta, it.chunk.bytes, it.chunk.size);
            WebPDemuxReleaseChunkIterator(&it);
        }
    }
    if (st == PC_OK && (WebPDemuxGetI(dmx, WEBP_FF_FORMAT_FLAGS) & EXIF_FLAG)) {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(dmx, "EXIF", 1, &it)) {
            st = cm_meta_load_exif(meta, it.chunk.bytes, it.chunk.size, true, &orient);
            WebPDemuxReleaseChunkIterator(&it);
        }
    }
    if (st == PC_OK && (WebPDemuxGetI(dmx, WEBP_FF_FORMAT_FLAGS) & ICCP_FLAG)) {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(dmx, "ICCP", 1, &it)) {
            if (it.chunk.size) {
                meta->icc = (uint8_t *)malloc(it.chunk.size);
                if (meta->icc) {
                    memcpy(meta->icc, it.chunk.bytes, it.chunk.size);
                    meta->icc_len = it.chunk.size;
                } else {
                    st = PC_ERR_NOMEM;
                }
            }
            WebPDemuxReleaseChunkIterator(&it);
        }
    }
    WebPDemuxDelete(dmx);
    if (st != PC_OK) goto fail;
    meta->src_bits = 8u;
    meta->had_alpha = feat.has_alpha != 0;
    if (feat.has_animation) {
        /* The animation decoder keeps two canvases besides ours. */
        WebPAnimDecoderOptions opt;
        WebPAnimDecoder *ad;
        WebPAnimInfo info;
        uint8_t *buf = NULL;
        int ts = 0;
        if ((uint64_t)cw * ch * 4u * 3u > lim->max_mem) { st = PC_ERR_LIMIT; goto fail; }
        if (!WebPAnimDecoderOptionsInit(&opt)) { st = PC_ERR_STATE; goto fail; }
        opt.color_mode = MODE_BGRA;
        opt.use_threads = 0;
        ad = WebPAnimDecoderNew(&data, &opt);
        if (!ad) { st = PC_ERR_FORMAT; goto fail; }
        if (!WebPAnimDecoderGetInfo(ad, &info) || info.canvas_width != cw ||
            info.canvas_height != ch || !WebPAnimDecoderGetNext(ad, &buf, &ts) || !buf) {
            WebPAnimDecoderDelete(ad);
            st = PC_ERR_FORMAT;
            goto fail;
        }
        st = lc_doc_new(lim, cw, ch, 1u, &d, &layer);
        if (st == PC_OK) st = store_bands(d, layer, buf, (size_t)cw * 4u);
        WebPAnimDecoderDelete(ad);
        if (st != PC_OK) goto fail;
        lc_note(meta, "Animated WebP: only the first frame was loaded");
    } else {
        WebPDecoderConfig cfg;
        uint8_t *buf;
        if ((uint32_t)feat.width != cw || (uint32_t)feat.height != ch) {
            st = PC_ERR_FORMAT;
            goto fail;
        }
        buf = (uint8_t *)lc_alloc((size_t)cw * ch, 4u, lim, &st);
        if (!buf) goto fail;
        if (!WebPInitDecoderConfig(&cfg)) { free(buf); st = PC_ERR_STATE; goto fail; }
        cfg.output.colorspace = MODE_BGRA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = buf;
        cfg.output.u.RGBA.stride = (int)(cw * 4u);
        cfg.output.u.RGBA.size = (size_t)cw * ch * 4u;
        cfg.options.use_threads = 0;
        if (WebPDecode(p, n, &cfg) != VP8_STATUS_OK) {
            WebPFreeDecBuffer(&cfg.output);
            free(buf);
            st = PC_ERR_FORMAT;
            goto fail;
        }
        WebPFreeDecBuffer(&cfg.output);
        st = lc_doc_new(lim, cw, ch, 1u, &d, &layer);
        if (st == PC_OK) st = store_bands(d, layer, buf, (size_t)cw * 4u);
        free(buf);
        if (st != PC_OK) goto fail;
    }
    st = lc_doc_orient(&d, lim, orient);
    if (st != PC_OK) goto fail;
    *out = d;
    return PC_OK;
fail:
    pc_doc_destroy(d);
    pc_meta_free(meta);
    return st;
}

/* ---- encoding ------------------------------------------------------------------------- */
typedef struct webp_params {
    int32_t preset;      /* WebPPreset order */
    int32_t quality;     /* 0..100 */
    int32_t effort;      /* 0..9 */
    int32_t lossless;    /* bool */
} webp_params;

static const char *const k_webp_presets[] = {
    "Default", "Picture", "Photo", "Drawing", "Icon", "Text", NULL
};

static const fx_prop k_webp_props[] = {
    { "preset", "Preset", FXP_CHOICE, (uint32_t)offsetof(webp_params, preset),
      0, 5, WEBP_PRESET_PHOTO, 0, k_webp_presets, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(webp_params, quality),
      0, 100, 95, 1, NULL, NULL, 0, 0, "lossless=0" },
    { "effort", "Effort", FXP_INT, (uint32_t)offsetof(webp_params, effort),
      0, 9, 7, 1, NULL, NULL, 0, 0, NULL },
    { "lossless", "Lossless", FXP_BOOL, (uint32_t)offsetof(webp_params, lossless),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
};

static int webp_writer(const uint8_t *data, size_t size, const WebPPicture *pic)
{
    return pc_buf_append((pc_buf *)pic->custom_ptr, data, size) == PC_OK;
}

static pc_status enc_err(WebPEncodingError e)
{
    switch (e) {
    case VP8_ENC_ERROR_OUT_OF_MEMORY: case VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY:
        return PC_ERR_NOMEM;
    case VP8_ENC_ERROR_BAD_DIMENSION: case VP8_ENC_ERROR_PARTITION0_OVERFLOW:
    case VP8_ENC_ERROR_PARTITION_OVERFLOW: case VP8_ENC_ERROR_FILE_TOO_BIG:
        return PC_ERR_LIMIT;
    case VP8_ENC_ERROR_BAD_WRITE: return PC_ERR_NOMEM;     /* pc_buf_append failed */
    case VP8_ENC_ERROR_USER_ABORT: return PC_ERR_CANCELLED;
    default: return PC_ERR_ARG;
    }
}

static pc_status webp_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                           const pc_par *par, pc_buf *out)
{
    webp_params prm;
    WebPConfig cfg;
    WebPPicture pic;
    pc_buf bits;
    pc_px32 *band = NULL;
    pc_status st = PC_OK;
    lc_flat flat;
    if (!d || !out) return PC_ERR_ARG;
    if (d->w > WEBP_MAX_SIDE || d->h > WEBP_MAX_SIDE) return PC_ERR_LIMIT;
    prm.preset = WEBP_PRESET_PHOTO;
    prm.quality = 95;
    prm.effort = 7;
    prm.lossless = 0;
    if (params) memcpy(&prm, params, sizeof prm);
    if (prm.preset < 0 || prm.preset > 5) prm.preset = WEBP_PRESET_PHOTO;
    prm.quality = prm.quality < 0 ? 0 : (prm.quality > 100 ? 100 : prm.quality);
    prm.effort = prm.effort < 0 ? 0 : (prm.effort > 9 ? 9 : prm.effort);
    if (!WebPConfigPreset(&cfg, (WebPPreset)prm.preset, (float)prm.quality)) return PC_ERR_STATE;
    if (prm.lossless) {
        if (!WebPConfigLosslessPreset(&cfg, prm.effort)) return PC_ERR_STATE;
        cfg.exact = 1;
    } else {
        cfg.method = (prm.effort * 6 + 4) / 9;
    }
    cfg.thread_level = 0;
    if (!WebPValidateConfig(&cfg)) return PC_ERR_ARG;
    if (!WebPPictureInit(&pic)) return PC_ERR_STATE;
    pic.use_argb = 1;
    pic.width = (int)d->w;
    pic.height = (int)d->h;
    if (!WebPPictureAlloc(&pic)) return PC_ERR_NOMEM;
    band = (pc_px32 *)malloc((size_t)d->w * (size_t)LC_BAND * sizeof *band);
    if (!band) { WebPPictureFree(&pic); return PC_ERR_NOMEM; }
    flat.d = d;
    flat.par = par;
    flat.over_white = false;
    for (int32_t y0 = 0; y0 < (int32_t)d->h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        st = lc_src_flatten(&flat, y0, nb, band);
        for (int32_t r = 0; r < nb && st == PC_OK; r++) {
            uint32_t *dst = pic.argb + (size_t)(y0 + r) * (size_t)pic.argb_stride;
            const pc_px32 *s = band + (size_t)r * d->w;
            for (uint32_t x = 0; x < d->w; x++)
                dst[x] = ((uint32_t)s[x].a << 24) | ((uint32_t)s[x].r << 16) |
                         ((uint32_t)s[x].g << 8) | s[x].b;
        }
    }
    free(band);
    if (st != PC_OK) { WebPPictureFree(&pic); return st; }
    memset(&bits, 0, sizeof bits);
    pic.writer = webp_writer;
    pic.custom_ptr = &bits;
    if (!WebPEncode(&cfg, &pic)) {
        st = enc_err(pic.error_code);
        WebPPictureFree(&pic);
        pc_buf_free(&bits);
        return st;
    }
    WebPPictureFree(&pic);
    {
        uint8_t *ex = NULL;
        size_t ex_n = 0, xmp_n = 0;
        const char *xmp = meta ? cm_meta_xmp(meta, &xmp_n) : NULL;
        st = cm_exif_for_save(meta, d->w, d->h, NULL, 0u, CM_EXIF_MAX, &ex, &ex_n);
        if (st != PC_OK) { pc_buf_free(&bits); return st; }
        if ((meta && meta->icc && meta->icc_len) || ex || xmp) {
            WebPData img, chunk, asm_out;
            WebPMux *mux;
            bool ok;
            img.bytes = bits.p;
            img.size = bits.n;
            WebPDataInit(&asm_out);
            mux = WebPMuxCreate(&img, 0);
            if (!mux) { free(ex); pc_buf_free(&bits); return PC_ERR_NOMEM; }
            ok = true;
            if (meta && meta->icc && meta->icc_len) {
                chunk.bytes = meta->icc;
                chunk.size = meta->icc_len;
                ok = WebPMuxSetChunk(mux, "ICCP", &chunk, 0) == WEBP_MUX_OK;
            }
            if (ok && ex) {
                chunk.bytes = ex;
                chunk.size = ex_n;
                ok = WebPMuxSetChunk(mux, "EXIF", &chunk, 0) == WEBP_MUX_OK;
            }
            if (ok && xmp) {
                chunk.bytes = (const uint8_t *)xmp;
                chunk.size = xmp_n;
                ok = WebPMuxSetChunk(mux, "XMP ", &chunk, 0) == WEBP_MUX_OK;
            }
            if (ok) ok = WebPMuxAssemble(mux, &asm_out) == WEBP_MUX_OK;
            WebPMuxDelete(mux);
            free(ex);
            if (!ok) { pc_buf_free(&bits); return PC_ERR_NOMEM; }
            st = pc_buf_append(out, asm_out.bytes, asm_out.size);
            WebPDataClear(&asm_out);
        } else {
            st = pc_buf_append(out, bits.p, bits.n);
        }
    }
    pc_buf_free(&bits);
    return st;
}

const pc_codec pc_codec_webp = {
    "webp", "WebP", "webp", PC_CODEC_LOAD | PC_CODEC_SAVE,
    webp_sniff, webp_load,
    k_webp_props, (uint32_t)(sizeof k_webp_props / sizeof k_webp_props[0]),
    (uint32_t)sizeof(webp_params),
    webp_save
};
