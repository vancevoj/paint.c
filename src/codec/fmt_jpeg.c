/* fmt_jpeg.c - JPEG through libjpeg-turbo, libjpeg API (lane L6B).
 *
 * Load: dimensions are checked right after jpeg_read_header, before any
 * pixel memory exists; the libjpeg memory manager gets lim->max_mem as its
 * budget; a progress monitor rejects files with more than JPEG_MAX_SCANS
 * scans (progressive DoS). Scanlines are read in bands of LC_BAND rows and
 * stored into the layer. Gray, YCbCr and RGB decode straight to BGRA;
 * CMYK and YCCK (Adobe inverted samples when an Adobe marker is present)
 * convert with Little-CMS through the embedded CMYK profile, or through the
 * default CMYK profile (SWOP, pc_icc_cmyk_default_profile) when there is
 * no usable one; the result is Adobe RGB (1998) and the image is tagged
 * with that profile (FL-CMYK). The naive formula remains only for a failing
 * Little-CMS. The EXIF orientation is
 * applied and reset to 1; EXIF (APP1 "Exif"), XMP (APP1, standard packet)
 * and IPTC (APP13 Photoshop 3.0 resource 0x0404) become pc_image_meta
 * items (cmeta.h key scheme, the EXIF thumbnail is dropped); the ICC
 * profile is reassembled from APP2 chunks, and JFIF density (or EXIF
 * resolution) becomes meta.dpi. Only 8-bit precision is supported, like
 * Paint.NET (12-bit fails cleanly).
 *
 * Save: quality 0..100 (default 95, Paint.NET 3.36 JpegFileType), chroma
 * subsampling 4:2:0, 4:2:2 (default, FILES.md) or 4:4:4, optimized Huffman
 * tables, JFIF density from meta.dpi (96 when unknown), ICC embedding
 * (pc_icc_embed_for: an RGB profile as is, a gray one as its RGB form,
 * unusable and CMYK ones never), and the EXIF, XMP and IPTC items written
 * back (APP1, APP1, APP13; a block that does not fit one marker segment is
 * left out). The image is flattened onto white first (3.36 behavior).
 *
 * Threads: reentrant. Error recovery uses setjmp/longjmp confined to the
 * *_run functions; everything they allocate lives in a heap context that
 * the caller cleans up.
 */
#include "lib_codec.h"
#include "cmeta.h"
#include "pc/pc_icc.h"

#include <limits.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"
#include "jerror.h"

#define JPEG_MAX_SCANS   500u
#define JPEG_DEFAULT_DPI 96.0

/* ---- error handling ---------------------------------------------------------------- */
/* jb points at a jmp_buf local to the function that called setjmp: a
 * jmp_buf member would force 16-byte alignment padding on MSVC x64 (C4324). */
typedef struct jerr_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf              *jb;
    pc_status             st;        /* reason for the longjmp */
} jerr_mgr;

static void j_error_exit(j_common_ptr cinfo)
{
    jerr_mgr *e = (jerr_mgr *)(void *)cinfo->err;
    if (e->st == PC_OK) {
        switch (e->pub.msg_code) {
        case JERR_OUT_OF_MEMORY: e->st = PC_ERR_NOMEM; break;
        case JERR_NO_BACKING_STORE: case JERR_IMAGE_TOO_BIG: case JERR_WIDTH_OVERFLOW:
            e->st = PC_ERR_LIMIT; break;
        default: e->st = PC_ERR_FORMAT; break;
        }
    }
    longjmp(*e->jb, 1);
}

static void j_output_message(j_common_ptr cinfo) { (void)cinfo; }   /* silent */

static void j_fail(j_common_ptr cinfo, pc_status st)
{
    jerr_mgr *e = (jerr_mgr *)(void *)cinfo->err;
    e->st = st;
    longjmp(*e->jb, 1);
}

static void j_progress(j_common_ptr cinfo)
{
    if (cinfo->is_decompressor &&
        (unsigned)((j_decompress_ptr)cinfo)->input_scan_number > JPEG_MAX_SCANS)
        j_fail(cinfo, PC_ERR_LIMIT);
}

/* ---- metadata markers (APP1 EXIF and XMP, APP13 IPTC) ---------------------------------- */
#define JPEG_XMP_SIG     "http://ns.adobe.com/xap/1.0/"     /* + NUL: 29 bytes */
#define JPEG_XMP_SIG_LEN 29u
#define JPEG_PS_SIG      "Photoshop 3.0"                    /* + NUL: 14 bytes */
#define JPEG_PS_SIG_LEN  14u
#define JPEG_SEG_MAX     65533u                             /* marker payload limit */

typedef struct exif_info {
    int    orientation;          /* 1..8, 1 when absent */
    double dpi_x, dpi_y;         /* 0 when absent */
} exif_info;

/* EXIF, XMP and IPTC of the saved markers into meta; *e receives the EXIF
 * orientation (applied by the caller, so the stored tag becomes 1) and
 * resolution. Broken blocks are ignored. */
static pc_status read_meta(j_decompress_ptr ci, pc_image_meta *meta, exif_info *e)
{
    pc_buf irb;
    pc_status st = PC_OK;
    bool have_exif = false;
    e->orientation = 1;
    e->dpi_x = e->dpi_y = 0.0;
    memset(&irb, 0, sizeof irb);
    for (jpeg_saved_marker_ptr m = ci->marker_list; m && st == PC_OK; m = m->next) {
        const uint8_t *d = m->data;
        size_t n = m->data_length;
        if (m->marker == JPEG_APP0 + 1 && n >= JPEG_XMP_SIG_LEN &&
            memcmp(d, JPEG_XMP_SIG, JPEG_XMP_SIG_LEN) == 0) {
            st = cm_meta_load_xmp(meta, d + JPEG_XMP_SIG_LEN, n - JPEG_XMP_SIG_LEN);
        } else if (m->marker == JPEG_APP0 + 13 && n >= JPEG_PS_SIG_LEN &&
                   memcmp(d, JPEG_PS_SIG, JPEG_PS_SIG_LEN) == 0) {
            /* resources may continue in the next APP13 segment */
            if (irb.n + n <= CM_IPTC_MAX * 2u)
                st = pc_buf_append(&irb, d + JPEG_PS_SIG_LEN, n - JPEG_PS_SIG_LEN);
        }
    }
    for (jpeg_saved_marker_ptr m = ci->marker_list; m && st == PC_OK && !have_exif; m = m->next) {
        if (m->marker == JPEG_APP0 + 1 && m->data_length >= 6u &&
            memcmp(m->data, "Exif\0\0", 6u) == 0) {
            cm_exif ex;
            have_exif = true;              /* first EXIF block only */
            st = cm_meta_load_exif(meta, m->data, m->data_length, true, &e->orientation);
            if (st == PC_OK) st = cm_meta_get_exif(meta, &ex);
            if (st == PC_OK && !cm_exif_resolution(&ex, &e->dpi_x, &e->dpi_y))
                e->dpi_x = e->dpi_y = 0.0;
            cm_exif_free(&ex);
        }
    }
    if (st == PC_OK && irb.n) {
        const uint8_t *iim;
        size_t len;
        if (cm_irb_find_iptc(irb.p, irb.n, &iim, &len)) st = cm_meta_load_iptc(meta, iim, len);
    }
    pc_buf_free(&irb);
    return st;
}

/* Write the EXIF, XMP and IPTC items as marker segments (after the JFIF
 * header). A block that does not fit one segment is left out. */
static void write_meta(j_compress_ptr ci, const pc_image_meta *meta, uint32_t w, uint32_t h)
{
    pc_buf b;
    uint8_t *ex = NULL, *iim = NULL;
    size_t ex_n = 0, iim_n = 0, xmp_n = 0;
    const char *xmp;
    pc_status st;
    if (!meta) return;
    memset(&b, 0, sizeof b);
    st = cm_exif_for_save(meta, w, h, NULL, 0u, JPEG_SEG_MAX - 6u, &ex, &ex_n);
    if (st != PC_OK) j_fail((j_common_ptr)ci, st);
    if (ex) {
        st = pc_buf_append(&b, "Exif\0\0", 6u);
        if (st == PC_OK) st = pc_buf_append(&b, ex, ex_n);
        free(ex);
        if (st != PC_OK) { pc_buf_free(&b); j_fail((j_common_ptr)ci, st); }
        jpeg_write_marker(ci, JPEG_APP0 + 1, b.p, (unsigned int)b.n);
        b.n = 0;
    }
    xmp = cm_meta_xmp(meta, &xmp_n);
    if (xmp && xmp_n <= JPEG_SEG_MAX - JPEG_XMP_SIG_LEN) {
        st = pc_buf_append(&b, JPEG_XMP_SIG, JPEG_XMP_SIG_LEN);
        if (st == PC_OK) st = pc_buf_append(&b, xmp, xmp_n);
        if (st != PC_OK) { pc_buf_free(&b); j_fail((j_common_ptr)ci, st); }
        jpeg_write_marker(ci, JPEG_APP0 + 1, b.p, (unsigned int)b.n);
        b.n = 0;
    }
    st = cm_get_blob(meta, CM_KEY_IPTC, &iim, &iim_n);
    if (st == PC_OK && iim) {
        st = pc_buf_append(&b, JPEG_PS_SIG, JPEG_PS_SIG_LEN);
        if (st == PC_OK) st = cm_irb_put_iptc(&b, iim, iim_n);
        if (st == PC_OK && b.n <= JPEG_SEG_MAX)
            jpeg_write_marker(ci, JPEG_APP0 + 13, b.p, (unsigned int)b.n);
    }
    free(iim);
    pc_buf_free(&b);
    if (st != PC_OK) j_fail((j_common_ptr)ci, st);
}

/* ---- orientation ------------------------------------------------------------------------ */
/* Map source pixel (x, y) of a sw x sh image to the displayed image. */
static void orient_map(int o, int32_t sw, int32_t sh, int32_t x, int32_t y, int32_t *dx,
                       int32_t *dy)
{
    switch (o) {
    case 2: *dx = sw - 1 - x; *dy = y; break;
    case 3: *dx = sw - 1 - x; *dy = sh - 1 - y; break;
    case 4: *dx = x; *dy = sh - 1 - y; break;
    case 5: *dx = y; *dy = x; break;
    case 6: *dx = sh - 1 - y; *dy = x; break;
    case 7: *dx = sh - 1 - y; *dy = sw - 1 - x; break;
    case 8: *dx = y; *dy = sw - 1 - x; break;
    default: *dx = x; *dy = y; break;
    }
}

/* ---- decoding ---------------------------------------------------------------------------- */
typedef struct jdec {
    struct jpeg_decompress_struct ci;
    jerr_mgr                      err;
    struct jpeg_progress_mgr      prog;
    bool                          created;
    pc_doc                       *d;
    pc_px32                      *band;    /* sw * LC_BAND */
    pc_px32                      *tmp;     /* oriented copy of a band */
    uint8_t                      *cmyk;    /* sw * LC_BAND * 4 */
    lc_cmyk_xf                   *xf;
} jdec;

static void cmyk_naive(const uint8_t *s, pc_px32 *d, size_t n, bool inverted)
{
    for (size_t i = 0; i < n; i++, s += 4) {
        uint32_t c = s[0], m = s[1], y = s[2], k = s[3];
        if (!inverted) { c = 255u - c; m = 255u - m; y = 255u - y; k = 255u - k; }
        /* c, m, y, k now hold 255 - ink */
        d[i].r = (uint8_t)pc_mul255(c, k);
        d[i].g = (uint8_t)pc_mul255(m, k);
        d[i].b = (uint8_t)pc_mul255(y, k);
        d[i].a = 255u;
    }
}

static pc_status jdec_run(jdec *j, const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_image_meta *meta)
{
    struct jpeg_decompress_struct *ci = &j->ci;
    exif_info ex;
    pc_layer *layer;
    pc_status st;
    int32_t sw, sh, dw, dh;
    bool cmyk;
    JSAMPROW rows[LC_BAND];
    jmp_buf jb;

    j->err.jb = &jb;
    if (setjmp(jb)) return j->err.st;
    jpeg_create_decompress(ci);
    j->created = true;
    ci->mem->max_memory_to_use = lim->max_mem > (uint64_t)LONG_MAX ? LONG_MAX : (long)lim->max_mem;
    j->prog.progress_monitor = j_progress;
    ci->progress = &j->prog;
    jpeg_mem_src(ci, p, (unsigned long)n);
    jpeg_save_markers(ci, JPEG_APP0 + 1, 0xFFFF);
    jpeg_save_markers(ci, JPEG_APP0 + 2, 0xFFFF);
    jpeg_save_markers(ci, JPEG_APP0 + 13, 0xFFFF);
    if (jpeg_read_header(ci, TRUE) != JPEG_HEADER_OK) return PC_ERR_FORMAT;
    st = pc_codec_check_size(lim, ci->image_width, ci->image_height, 1u);
    if (st != PC_OK) return st;
    if (ci->data_precision != 8) return PC_ERR_UNSUPPORTED;
    sw = (int32_t)ci->image_width;
    sh = (int32_t)ci->image_height;

    st = read_meta(ci, meta, &ex);
    if (st != PC_OK) return st;
    if (ci->saw_JFIF_marker && (ci->density_unit == 1 || ci->density_unit == 2) &&
        ci->X_density && ci->Y_density) {
        double k = ci->density_unit == 2 ? 2.54 : 1.0;
        meta->dpi_x = ci->X_density * k;
        meta->dpi_y = ci->Y_density * k;
    } else if (ex.dpi_x > 0.0 && ex.dpi_y > 0.0) {
        meta->dpi_x = ex.dpi_x;
        meta->dpi_y = ex.dpi_y;
    }
    {
        JOCTET *icc = NULL;
        unsigned int icc_len = 0;
        if (jpeg_read_icc_profile(ci, &icc, &icc_len) && icc && icc_len) {
            meta->icc = icc;
            meta->icc_len = icc_len;
        } else {
            free(icc);
        }
    }
    meta->src_bits = 8u;
    meta->had_alpha = false;

    switch (ci->jpeg_color_space) {
    case JCS_GRAYSCALE: case JCS_YCbCr: case JCS_RGB:
        ci->out_color_space = JCS_EXT_BGRA;
        cmyk = false;
        break;
    case JCS_CMYK: case JCS_YCCK:
        ci->out_color_space = JCS_CMYK;
        cmyk = true;
        break;
    default:
        return PC_ERR_UNSUPPORTED;
    }
    if (cmyk) {
        /* FL-CMYK: through the embedded CMYK profile, else (none, or not
         * usable) through the default CMYK profile (wave 4); the pixels
         * become Adobe RGB (1998) and carry that profile */
        bool inverted = ci->saw_Adobe_marker != 0;
        const char *note = LC_NOTE_CMYK_EMBEDDED;
        uint8_t *adobe = NULL;
        size_t adobe_n = 0;
        st = pc_icc_adobe_rgb_profile(&adobe, &adobe_n);
        if (st != PC_OK) return st;
        if (meta->icc) j->xf = lc_cmyk_open(meta->icc, meta->icc_len, inverted, adobe, adobe_n);
        if (!j->xf) {
            note = meta->icc ? LC_NOTE_CMYK_BAD : LC_NOTE_CMYK_DEFAULT;
            j->xf = lc_cmyk_open_default(inverted, adobe, adobe_n);
        }
        free(meta->icc);
        meta->icc = NULL;
        meta->icc_len = 0;
        if (j->xf) {
            meta->icc = adobe;
            meta->icc_len = adobe_n;
        } else {
            free(adobe);
            note = LC_NOTE_CMYK_NAIVE;           /* only when Little-CMS failed */
        }
        lc_note(meta, note);
    }

    jpeg_start_decompress(ci);
    if ((int32_t)ci->output_width != sw || (int32_t)ci->output_height != sh)
        return PC_ERR_FORMAT;
    dw = ex.orientation >= 5 ? sh : sw;
    dh = ex.orientation >= 5 ? sw : sh;
    st = lc_doc_new(lim, (uint32_t)dw, (uint32_t)dh, 1u, &j->d, &layer);
    if (st != PC_OK) return st;
    j->band = (pc_px32 *)lc_alloc((size_t)sw * (size_t)LC_BAND, sizeof(pc_px32), lim, &st);
    if (!j->band) return st;
    if (ex.orientation != 1) {
        j->tmp = (pc_px32 *)lc_alloc((size_t)sw * (size_t)LC_BAND, sizeof(pc_px32), lim, &st);
        if (!j->tmp) return st;
    }
    if (cmyk) {
        j->cmyk = (uint8_t *)lc_alloc((size_t)sw * (size_t)LC_BAND, 4u, lim, &st);
        if (!j->cmyk) return st;
    }
    for (int32_t y0 = 0; y0 < sh; y0 += LC_BAND) {
        int32_t nb = sh - y0 < LC_BAND ? sh - y0 : LC_BAND;
        for (int32_t r = 0; r < nb; r++)
            rows[r] = cmyk ? j->cmyk + (size_t)r * (size_t)sw * 4u
                           : (JSAMPROW)(void *)(j->band + (size_t)r * (size_t)sw);
        while ((int32_t)ci->output_scanline < y0 + nb) {
            int32_t at = (int32_t)ci->output_scanline - y0;
            JDIMENSION got = jpeg_read_scanlines(ci, rows + at, (JDIMENSION)(nb - at));
            if (got == 0) return PC_ERR_FORMAT;
        }
        if (cmyk) {
            size_t cnt = (size_t)sw * (size_t)nb;
            if (j->xf) lc_cmyk_run(j->xf, j->cmyk, j->band, cnt);
            else cmyk_naive(j->cmyk, j->band, cnt, ci->saw_Adobe_marker != 0);
        }
        if (ex.orientation == 1) {
            st = pc_layer_store_rect(j->d, layer, pc_rect_make(0, y0, sw, nb), j->band,
                                     (size_t)sw);
        } else {
            /* destination rectangle of this band, then scatter */
            int32_t ax, ay, bx, by, rx, ry, rw, rh;
            orient_map(ex.orientation, sw, sh, 0, y0, &ax, &ay);
            orient_map(ex.orientation, sw, sh, sw - 1, y0 + nb - 1, &bx, &by);
            rx = ax < bx ? ax : bx; ry = ay < by ? ay : by;
            rw = (ax > bx ? ax : bx) - rx + 1; rh = (ay > by ? ay : by) - ry + 1;
            for (int32_t y = 0; y < nb; y++)
                for (int32_t x = 0; x < sw; x++) {
                    int32_t dx, dy;
                    orient_map(ex.orientation, sw, sh, x, y0 + y, &dx, &dy);
                    j->tmp[(size_t)(dy - ry) * (size_t)rw + (size_t)(dx - rx)] =
                        j->band[(size_t)y * (size_t)sw + (size_t)x];
                }
            st = pc_layer_store_rect(j->d, layer, pc_rect_make(rx, ry, rw, rh), j->tmp,
                                     (size_t)rw);
        }
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

static pc_status jpeg_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                           pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    jdec *j;
    pc_status st;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (n > (size_t)ULONG_MAX) return PC_ERR_LIMIT;
    j = (jdec *)calloc(1u, sizeof *j);
    if (!j) return PC_ERR_NOMEM;
    j->ci.err = jpeg_std_error(&j->err.pub);
    j->err.pub.error_exit = j_error_exit;
    j->err.pub.output_message = j_output_message;
    j->err.st = PC_OK;
    st = jdec_run(j, p, n, lim, meta);
    if (j->created) jpeg_destroy_decompress(&j->ci);
    lc_cmyk_close(j->xf);
    free(j->band);
    free(j->tmp);
    free(j->cmyk);
    if (st == PC_OK) {
        *out = j->d;
    } else {
        pc_doc_destroy(j->d);
        pc_meta_free(meta);
    }
    free(j);
    return st;
}

static bool jpeg_sniff(const uint8_t *p, size_t n)
{
    return p && n >= 3u && p[0] == 0xFFu && p[1] == 0xD8u && p[2] == 0xFFu;
}

/* ---- encoding ------------------------------------------------------------------------------ */
typedef struct jpeg_params {
    int32_t quality;        /* 0..100 */
    int32_t subsampling;    /* JPEG_SUB_* */
} jpeg_params;

enum { JPEG_SUB_420 = 0, JPEG_SUB_422 = 1, JPEG_SUB_444 = 2 };

static const char *const k_jpeg_sub[] = {
    "4:2:0 (best compression)", "4:2:2", "4:4:4 (best quality)", NULL
};

static const fx_prop k_jpeg_props[] = {
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(jpeg_params, quality),
      0, 100, 95, 1, NULL, NULL, 0, 0, NULL },
    { "subsampling", "Chroma subsampling", FXP_CHOICE,
      (uint32_t)offsetof(jpeg_params, subsampling), 0, 2, JPEG_SUB_422, 0, k_jpeg_sub, NULL,
      0, 0, NULL },
};

#define JDST_BUF 65536u

typedef struct jdst {
    struct jpeg_destination_mgr pub;
    pc_buf                     *out;
    JOCTET                      buf[JDST_BUF];
} jdst;

static void jd_init(j_compress_ptr ci)
{
    jdst *d = (jdst *)(void *)ci->dest;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = JDST_BUF;
}

static boolean jd_empty(j_compress_ptr ci)
{
    jdst *d = (jdst *)(void *)ci->dest;
    pc_status st = pc_buf_append(d->out, d->buf, JDST_BUF);
    if (st != PC_OK) j_fail((j_common_ptr)ci, st);
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = JDST_BUF;
    return TRUE;
}

static void jd_term(j_compress_ptr ci)
{
    jdst *d = (jdst *)(void *)ci->dest;
    pc_status st = pc_buf_append(d->out, d->buf, JDST_BUF - d->pub.free_in_buffer);
    if (st != PC_OK) j_fail((j_common_ptr)ci, st);
}

typedef struct jenc {
    struct jpeg_compress_struct ci;
    jerr_mgr                    err;
    jdst                        dst;
    bool                        created;
    pc_px32                    *band;
} jenc;

static pc_status jenc_run(jenc *j, const pc_doc *d, const pc_image_meta *meta,
                          const pc_icc_embed *icc, const jpeg_params *prm, const pc_par *par)
{
    struct jpeg_compress_struct *ci = &j->ci;
    lc_flat flat;
    double dpi_x, dpi_y;
    int hs, vs;
    JSAMPROW rows[LC_BAND];
    jmp_buf jb;
    j->err.jb = &jb;
    if (setjmp(jb)) return j->err.st;
    jpeg_create_compress(ci);
    j->created = true;
    ci->dest = &j->dst.pub;
    j->dst.pub.init_destination = jd_init;
    j->dst.pub.empty_output_buffer = jd_empty;
    j->dst.pub.term_destination = jd_term;
    ci->image_width = d->w;
    ci->image_height = d->h;
    ci->input_components = 4;
    ci->in_color_space = JCS_EXT_BGRA;
    jpeg_set_defaults(ci);
    jpeg_set_quality(ci, prm->quality < 1 ? 1 : (prm->quality > 100 ? 100 : prm->quality), TRUE);
    hs = prm->subsampling == JPEG_SUB_444 ? 1 : 2;
    vs = prm->subsampling == JPEG_SUB_420 ? 2 : 1;
    ci->comp_info[0].h_samp_factor = hs;
    ci->comp_info[0].v_samp_factor = vs;
    ci->comp_info[1].h_samp_factor = ci->comp_info[1].v_samp_factor = 1;
    ci->comp_info[2].h_samp_factor = ci->comp_info[2].v_samp_factor = 1;
    ci->optimize_coding = TRUE;
    dpi_x = meta && meta->dpi_x > 0.0 ? meta->dpi_x : JPEG_DEFAULT_DPI;
    dpi_y = meta && meta->dpi_y > 0.0 ? meta->dpi_y : JPEG_DEFAULT_DPI;
    ci->write_JFIF_header = TRUE;
    ci->density_unit = 1;
    ci->X_density = (UINT16)(dpi_x >= 65535.0 ? 65535 : (dpi_x < 1.0 ? 1 : (int)(dpi_x + 0.5)));
    ci->Y_density = (UINT16)(dpi_y >= 65535.0 ? 65535 : (dpi_y < 1.0 ? 1 : (int)(dpi_y + 0.5)));
    jpeg_start_compress(ci, TRUE);
    write_meta(ci, meta, d->w, d->h);
    if (icc->icc && icc->len) {
        if (icc->len > 65519u * 255u) return PC_ERR_LIMIT;
        jpeg_write_icc_profile(ci, icc->icc, (unsigned int)icc->len);
    }
    j->band = (pc_px32 *)malloc((size_t)d->w * (size_t)LC_BAND * sizeof *j->band);
    if (!j->band) return PC_ERR_NOMEM;
    flat.d = d;
    flat.par = par;
    flat.over_white = true;
    for (int32_t y0 = 0; y0 < (int32_t)d->h; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        int32_t done = 0;
        pc_status st = lc_src_flatten(&flat, y0, nb, j->band);
        if (st != PC_OK) return st;
        for (int32_t r = 0; r < nb; r++)
            rows[r] = (JSAMPROW)(void *)(j->band + (size_t)r * d->w);
        while (done < nb) {
            JDIMENSION k = jpeg_write_scanlines(ci, rows + done, (JDIMENSION)(nb - done));
            if (k == 0) return PC_ERR_STATE;
            done += (int32_t)k;
        }
    }
    jpeg_finish_compress(ci);
    return PC_OK;
}

static pc_status jpeg_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                           const pc_par *par, pc_buf *out)
{
    jpeg_params prm;
    jenc *j;
    pc_icc_embed icc;
    pc_status st;
    size_t n0;
    if (!d || !out) return PC_ERR_ARG;
    if (d->w > 65500u || d->h > 65500u) return PC_ERR_LIMIT;   /* JPEG format limit */
    prm.quality = 95;
    prm.subsampling = JPEG_SUB_422;
    if (params) memcpy(&prm, params, sizeof prm);
    /* FS-ICC: the profile must describe the 3-component YCbCr (RGB) data */
    st = pc_icc_embed_for(meta, PC_ICC_SPACE_RGB, &icc);
    if (st != PC_OK) return st;
    j = (jenc *)calloc(1u, sizeof *j);
    if (!j) { pc_icc_embed_free(&icc); return PC_ERR_NOMEM; }
    j->ci.err = jpeg_std_error(&j->err.pub);
    j->err.pub.error_exit = j_error_exit;
    j->err.pub.output_message = j_output_message;
    j->err.st = PC_OK;
    j->dst.out = out;
    n0 = out->n;
    st = jenc_run(j, d, meta, &icc, &prm, par);
    if (j->created) jpeg_destroy_compress(&j->ci);
    free(j->band);
    free(j);
    pc_icc_embed_free(&icc);
    if (st != PC_OK) out->n = n0;     /* drop partial output */
    return st;
}

const pc_codec pc_codec_jpeg = {
    "jpeg", "JPEG", "jpg;jpeg;jpe;jfif;exif", PC_CODEC_LOAD | PC_CODEC_SAVE,
    jpeg_sniff, jpeg_load,
    k_jpeg_props, (uint32_t)(sizeof k_jpeg_props / sizeof k_jpeg_props[0]),
    (uint32_t)sizeof(jpeg_params),
    jpeg_save
};
