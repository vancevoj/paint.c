/* test_meta_formats.c - metadata through the library codecs and .pdn
 * (lane CODEC, FL-META): JPEG APP1 EXIF / APP1 XMP / APP13 IPTC read with
 * the orientation applied and reset, written back (and the 4:2:2 default);
 * PNG eXIf, tEXt, zTXt, iTXt and XMP (Author, Copyright, Description and
 * Comment through EXIF); WebP EXIF and XMP chunks; GIF comments as EXIF
 * UserComment; .pdn round trips of every item; conversions between the
 * formats; fuzzing of metadata-rich files. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "meta_test_util.h"
#include "../../src/codec/cmeta.h"
#include "icc_test_util.h"

#include <stdio.h>
#include "jpeglib.h"
#include "webp/encode.h"
#include "webp/mux.h"

static const pc_codec *codec(const char *id) { return pc_codec_by_id(id); }

/* ---- fixtures ------------------------------------------------------------------ */
static const uint8_t k_iim[] = { 0x1C, 2, 0x50, 0, 3, 'B', 'o', 'b', 0x1C, 2, 0x74, 0, 4,
                                 '(', 'c', ')', 'X' };
static const char k_xmp6[] =
    "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description "
    "xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "tiff:Orientation=\"6\"><dc:creator>Ann</dc:creator></rdf:Description></rdf:RDF>"
    "</x:xmpmeta><?xpacket end=\"w\"?>";

/* EXIF of a camera: IFD0 Make, Orientation, resolution, Artist; Exif IFD
 * ExposureTime, DateTimeOriginal, MakerNote (mk_len bytes), UserComment;
 * GPS LatitudeRef and Latitude; an IFD1 thumbnail entry. */
static void camera_exif(pc_buf *b, bool be, uint32_t orientation, uint32_t mk_len)
{
    static const uint8_t make[] = "Canon", artist[] = "Ann Example";
    static const uint8_t dto[] = "2024:05:06 07:08:09", latref[] = "N";
    static const uint8_t uc[] = { 'A', 'S', 'C', 'I', 'I', 0, 0, 0, 'h', 'i' };
    uint8_t *mk = (uint8_t *)calloc(mk_len ? mk_len : 1u, 1u);
    tx_ent i0[6], ex[4], gp[2], i1[1];
    for (uint32_t i = 0; i < mk_len; i++) mk[i] = (uint8_t)(i * 13u + 1u);
    memset(i0, 0, sizeof i0); memset(ex, 0, sizeof ex); memset(gp, 0, sizeof gp);
    memset(i1, 0, sizeof i1);
    i0[0].tag = 271; i0[0].type = 2; i0[0].count = 6; i0[0].bytes = make;
    i0[1].tag = 274; i0[1].type = 3; i0[1].count = 1; i0[1].nums[0] = orientation;
    i0[2].tag = 282; i0[2].type = 5; i0[2].count = 1; i0[2].nums[0] = 72; i0[2].nums[1] = 1;
    i0[3].tag = 283; i0[3].type = 5; i0[3].count = 1; i0[3].nums[0] = 72; i0[3].nums[1] = 1;
    i0[4].tag = 296; i0[4].type = 3; i0[4].count = 1; i0[4].nums[0] = 2;
    i0[5].tag = 315; i0[5].type = 2; i0[5].count = 12; i0[5].bytes = artist;
    ex[0].tag = 33434; ex[0].type = 5; ex[0].count = 1; ex[0].nums[0] = 1; ex[0].nums[1] = 125;
    ex[1].tag = 36867; ex[1].type = 2; ex[1].count = 20; ex[1].bytes = dto;
    ex[2].tag = 37500; ex[2].type = 7; ex[2].count = mk_len ? mk_len : 1u; ex[2].bytes = mk;
    ex[3].tag = 37510; ex[3].type = 7; ex[3].count = 10; ex[3].bytes = uc;
    gp[0].tag = 1; gp[0].type = 2; gp[0].count = 2; gp[0].bytes = latref;
    gp[1].tag = 2; gp[1].type = 5; gp[1].count = 3;
    gp[1].nums[0] = 52; gp[1].nums[1] = 1; gp[1].nums[2] = 31; gp[1].nums[3] = 1;
    gp[1].nums[4] = 1234; gp[1].nums[5] = 100;
    i1[0].tag = 513; i1[0].type = 4; i1[0].count = 1; i1[0].nums[0] = 8;
    tx_build(b, be, i0, 6, ex, 4, gp, 2, i1, 1);
    free(mk);
}

/* The camera tags are all there (Orientation 1, no thumbnail); want_maker
 * says whether the MakerNote must be present or absent. */
static void check_camera(const pc_image_meta *m, const char *what, bool want_maker)
{
    cm_exif e;
    char *s;
    const cm_tag *t;
    bool ok = true;
    CHECK(cm_meta_get_exif(m, &e) == PC_OK);
    s = cm_exif_get_text(&e, 271);
    ok = ok && s && strcmp(s, "Canon") == 0;
    free(s);
    s = cm_exif_get_text(&e, CM_TAG_ARTIST);
    ok = ok && s && strcmp(s, "Ann Example") == 0;
    free(s);
    s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    ok = ok && s && strcmp(s, "hi") == 0;
    free(s);
    s = cm_exif_get_text(&e, 36867);
    ok = ok && s && strcmp(s, "2024:05:06 07:08:09") == 0;
    free(s);
    t = cm_exif_find(&e, 2);
    ok = ok && t && t->ifd == CM_IFD_GPS && t->len == 24u && t->val[16] == (1234u & 0xFFu);
    t = cm_exif_find(&e, 33434);
    ok = ok && t && t->ifd == CM_IFD_EXIF;
    ok = ok && cm_exif_orientation(&e) == 1 && !cm_exif_find(&e, 513);
    ok = ok && (cm_exif_find(&e, CM_TAG_MAKERNOTE) != NULL) == want_maker;
    CHECK(ok);
    if (!ok) INFO("camera EXIF incomplete after %s", what);
    cm_exif_free(&e);
}

static void check_iptc(const pc_image_meta *m)
{
    uint8_t *b = NULL;
    size_t n = 0;
    CHECK(cm_get_blob(m, CM_KEY_IPTC, &b, &n) == PC_OK && n == sizeof k_iim && b &&
          memcmp(b, k_iim, n) == 0);
    free(b);
}

/* Metadata of a camera photo already loaded (orientation 1). */
static void rich_meta(pc_image_meta *m, uint32_t mk_len)
{
    pc_buf b;
    int o;
    memset(m, 0, sizeof *m);
    memset(&b, 0, sizeof b);
    camera_exif(&b, false, 1, mk_len);
    CHECK(cm_meta_load_exif(m, b.p, b.n, true, &o) == PC_OK);
    CHECK(cm_meta_load_xmp(m, (const uint8_t *)k_xmp6, sizeof k_xmp6 - 1u) == PC_OK);
    CHECK(cm_meta_xmp_reset_orientation(m) == PC_OK);
    CHECK(cm_meta_load_iptc(m, k_iim, sizeof k_iim) == PC_OK);
    CHECK(pc_meta_add(m, "png.text.Title", "Sunset \xC3\xA0 Nice") == PC_OK);
    CHECK(pc_meta_add(m, "pdn.user.Palette", "#FF0000") == PC_OK);
    m->dpi_x = m->dpi_y = 300.0;
    pc_buf_free(&b);
}

static pc_doc *gradient_doc(uint32_t w, uint32_t h)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    pc_doc *d;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[(size_t)y * w + x] = tu_px((uint8_t)(x * 255u / (w - 1u)),
                                          (uint8_t)(y * 255u / (h - 1u)), 90, 255);
    d = tu_doc_from_px(w, h, px);
    free(px);
    return d;
}

static pc_doc *load(const char *id, const pc_buf *b, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_status st = codec(id)->load(b->p, b->n, NULL, &d, m);
    CHECK(st == PC_OK);
    if (st != PC_OK) {
        INFO("%s load: %s", id, pc_status_str(st));
        memset(m, 0, sizeof *m);
    }
    return d;
}

static pc_status save(const char *id, const pc_doc *d, const pc_image_meta *m, pc_buf *out)
{
    memset(out, 0, sizeof *out);
    return codec(id)->save(d, m, NULL, NULL, out);
}

/* ---- JPEG ------------------------------------------------------------------------------ */
typedef struct jseg { int marker; size_t off, len; } jseg;

/* Marker segments before SOS (payload offsets). */
static size_t jpeg_segments(const pc_buf *b, jseg *s, size_t cap)
{
    size_t pos = 2, k = 0;
    while (pos + 4u <= b->n && k < cap) {
        int mk;
        size_t len;
        if (b->p[pos] != 0xFF) break;
        mk = b->p[pos + 1];
        len = ((size_t)b->p[pos + 2] << 8) | b->p[pos + 3];
        s[k].marker = mk;
        s[k].off = pos + 4u;
        s[k].len = len - 2u;
        k++;
        if (mk == 0xDA) break;
        pos += 2u + len;
    }
    return k;
}

static const jseg *jpeg_find(const pc_buf *b, const jseg *s, size_t n, int marker, const char *sig,
                             size_t sig_len)
{
    for (size_t i = 0; i < n; i++)
        if (s[i].marker == marker && s[i].len >= sig_len &&
            memcmp(b->p + s[i].off, sig, sig_len) == 0)
            return &s[i];
    return NULL;
}

static uint8_t *jpeg_with_markers(const uint8_t *rgb, uint32_t w, uint32_t h, const pc_buf *app1,
                                  bool xmp, bool iptc, unsigned long *len)
{
    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr em;
    unsigned char *buf = NULL;
    ci.err = jpeg_std_error(&em);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &buf, len);
    ci.image_width = w; ci.image_height = h;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, 100, TRUE);
    for (int c = 0; c < 3; c++) ci.comp_info[c].h_samp_factor = ci.comp_info[c].v_samp_factor = 1;
    jpeg_start_compress(&ci, TRUE);
    if (app1) jpeg_write_marker(&ci, JPEG_APP0 + 1, app1->p, (unsigned)app1->n);
    if (xmp) {
        pc_buf x;
        memset(&x, 0, sizeof x);
        pc_buf_append(&x, "http://ns.adobe.com/xap/1.0/", 29);
        pc_buf_append(&x, k_xmp6, sizeof k_xmp6 - 1u);
        jpeg_write_marker(&ci, JPEG_APP0 + 1, x.p, (unsigned)x.n);
        pc_buf_free(&x);
    }
    if (iptc) {
        pc_buf x;
        memset(&x, 0, sizeof x);
        pc_buf_append(&x, "Photoshop 3.0", 14);
        pc_buf_append(&x, "8BIM\x04\x0C\0\0\0\0\0\x02zz", 14);     /* thumbnail resource first */
        cm_irb_put_iptc(&x, k_iim, sizeof k_iim);
        jpeg_write_marker(&ci, JPEG_APP0 + 13, x.p, (unsigned)x.n);
        pc_buf_free(&x);
    }
    while (ci.next_scanline < h) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)ci.next_scanline * w * 3u);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    return buf;
}

static void t_jpeg(void)
{
    const uint32_t W = 32, H = 16;
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3u);
    pc_buf app1, out;
    unsigned long len = 0;
    uint8_t *file;
    pc_image_meta m, m2;
    pc_doc *d;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = rgb + ((size_t)y * W + x) * 3u;
            p[0] = (uint8_t)(x * 8u); p[1] = (uint8_t)(y * 16u); p[2] = 60;
        }
    memset(&app1, 0, sizeof app1);
    pc_buf_append(&app1, "Exif\0\0", 6);
    camera_exif(&app1, true, 6, 200);
    file = jpeg_with_markers(rgb, W, H, &app1, true, true, &len);
    {
        pc_buf fb;
        fb.p = file; fb.n = len; fb.cap = len;
        d = load("jpeg", &fb, &m);
    }
    CHECK(d && d->w == H && d->h == W);                /* orientation 6: turned */
    if (d) {
        /* source (x, y) lands at (H - 1 - y, x): the red ramp now runs down */
        pc_px32 a = pc_layer_get_px(d->stack[0], H - 1u, 0);
        pc_px32 b = pc_layer_get_px(d->stack[0], H - 1u, W - 1u);
        CHECK(a.r < 10 && b.r > 240 && a.g < 10);
        check_camera(&m, "JPEG load", true);
        CHECK(strstr(cm_meta_xmp(&m, NULL), "tiff:Orientation=\"1\"") != NULL);
        check_iptc(&m);
        CHECK(m.dpi_x > 71.9 && m.dpi_x < 72.1);       /* JFIF says 1:1, EXIF says 72 */
        /* save: APP1 EXIF, APP1 XMP, APP13, 4:2:2 by default */
        CHECK(save("jpeg", d, &m, &out) == PC_OK);
        {
            jseg sg[32];
            size_t ns = jpeg_segments(&out, sg, 32);
            const jseg *ex = jpeg_find(&out, sg, ns, 0xE1, "Exif\0\0", 6);
            const jseg *xm = jpeg_find(&out, sg, ns, 0xE1, "http://ns.adobe.com/xap/1.0/", 29);
            const jseg *ps = jpeg_find(&out, sg, ns, 0xED, "Photoshop 3.0", 14);
            const jseg *sof = jpeg_find(&out, sg, ns, 0xC0, "", 0);
            CHECK(ex && xm && ps && sof);
            if (sof) CHECK(out.p[sof->off + 7] == 0x21 && out.p[sof->off + 10] == 0x11);
            if (ex) {
                tx_rd r;
                CHECK(tx_open(&r, out.p + ex->off + 6, ex->len - 6u));
                CHECK(tx_num(&r, tx_ifd0(&r), 274, 0) == 1u);
                CHECK(tx_next(&r, tx_ifd0(&r)) == 0u);         /* no thumbnail IFD */
            }
        }
        d = (pc_doc_destroy(d), (pc_doc *)NULL);
        d = load("jpeg", &out, &m2);
        CHECK(d && d->w == H && d->h == W);            /* not turned twice */
        check_camera(&m2, "JPEG save", true);
        check_iptc(&m2);
        CHECK(cm_meta_xmp(&m2, NULL) && strcmp(cm_meta_xmp(&m2, NULL), cm_meta_xmp(&m, NULL)) == 0);
        pc_meta_free(&m2);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
    free(file);
    pc_buf_free(&app1);
    /* an EXIF block over 64 KiB loses the MakerNote; XMP over one segment is left out */
    {
        pc_image_meta big;
        char *xmp = (char *)malloc(70001u);
        rich_meta(&big, 66000u);
        memset(xmp, 'x', 70000u);
        xmp[70000] = '\0';
        CHECK(cm_set(&big, CM_KEY_XMP, xmp) == PC_OK);
        d = gradient_doc(20, 10);
        CHECK(save("jpeg", d, &big, &out) == PC_OK);
        pc_doc_destroy(d);
        d = load("jpeg", &out, &m2);
        check_camera(&m2, "JPEG with a 66000 byte MakerNote", false);
        CHECK(cm_meta_xmp(&m2, NULL) == NULL);
        pc_doc_destroy(d);
        pc_meta_free(&m2);
        pc_meta_free(&big);
        pc_buf_free(&out);
        free(xmp);
    }
    free(rgb);
}

/* ---- PNG ------------------------------------------------------------------------------- */
static bool png_has_chunk(const pc_buf *b, const char *type, const char *start, size_t n)
{
    size_t pos = 8;
    while (pos + 12u <= b->n) {
        uint32_t len = ((uint32_t)b->p[pos] << 24) | ((uint32_t)b->p[pos + 1] << 16) |
                       ((uint32_t)b->p[pos + 2] << 8) | b->p[pos + 3];
        if (memcmp(b->p + pos + 4, type, 4) == 0 && len >= n &&
            (n == 0 || memcmp(b->p + pos + 8, start, n) == 0))
            return true;
        pos += 12u + len;
    }
    return false;
}

static void png_text_chunk(pc_buf *b, const char *type, const char *kw, const char *text,
                           bool compress)
{
    pc_buf c;
    memset(&c, 0, sizeof c);
    pc_buf_append(&c, kw, strlen(kw) + 1u);
    if (strcmp(type, "zTXt") == 0) {
        pc_buf_put_u8(&c, 0);
        tu_zlib_stored(&c, (const uint8_t *)text, strlen(text));
    } else if (strcmp(type, "iTXt") == 0) {
        pc_buf_put_u8(&c, compress ? 1 : 0);
        pc_buf_put_u8(&c, 0);
        pc_buf_append(&c, "en\0\0", 4);
        if (compress) tu_zlib_stored(&c, (const uint8_t *)text, strlen(text));
        else pc_buf_append(&c, text, strlen(text));
    } else {
        pc_buf_append(&c, text, strlen(text));
    }
    tu_png_chunk(b, type, c.p, c.n);
    pc_buf_free(&c);
}

static void t_png(void)
{
    const uint32_t W = 12, H = 5;
    pc_buf b, ex, out;
    pc_image_meta m, m2;
    pc_doc *d;
    memset(&b, 0, sizeof b);
    memset(&ex, 0, sizeof ex);
    camera_exif(&ex, false, 8, 16);
    tu_png_sig(&b);
    tu_png_ihdr(&b, W, H, 8, 2, 0);
    tu_png_chunk(&b, "eXIf", ex.p, ex.n);
    png_text_chunk(&b, "tEXt", "Author", "Bea \xE9t\xE9", false);              /* Latin-1 */
    png_text_chunk(&b, "zTXt", "Copyright", "(c) 2024 Bea", false);
    png_text_chunk(&b, "iTXt", "Description", "Caf\xC3\xA9 \xE2\x82\xAC", true);
    png_text_chunk(&b, "iTXt", "XML:com.adobe.xmp", k_xmp6, false);
    {
        uint8_t raw[(3 * 12 + 1) * 5];
        pc_buf z;
        memset(&z, 0, sizeof z);
        for (uint32_t y = 0; y < H; y++) {
            raw[y * (3u * W + 1u)] = 0;
            for (uint32_t x = 0; x < W; x++) {
                uint8_t *p = raw + y * (3u * W + 1u) + 1u + 3u * x;
                p[0] = (uint8_t)(x * 20u); p[1] = (uint8_t)(y * 50u); p[2] = 7;
            }
        }
        tu_zlib_stored(&z, raw, sizeof raw);
        tu_png_chunk(&b, "IDAT", z.p, z.n);
        pc_buf_free(&z);
    }
    png_text_chunk(&b, "tEXt", "Comment", "after IDAT", false);
    png_text_chunk(&b, "tEXt", "Title", "Sunset", false);
    tu_png_chunk(&b, "IEND", NULL, 0);
    d = load("png", &b, &m);
    CHECK(d && d->w == H && d->h == W);                /* orientation 8 */
    if (d) {
        cm_exif e;
        char *s;
        /* orientation 8: source (x, y) lands at (y, W - 1 - x) */
        pc_px32 p = pc_layer_get_px(d->stack[0], 4, W - 1u - 3u);
        CHECK(p.r == 60 && p.g == 200 && p.b == 7);
        CHECK(cm_meta_get_exif(&m, &e) == PC_OK && cm_exif_orientation(&e) == 1);
        s = cm_exif_get_text(&e, CM_TAG_ARTIST);
        CHECK(s && strcmp(s, "Bea \xC3\xA9t\xC3\xA9") == 0);           /* text chunk wins */
        free(s);
        s = cm_exif_get_text(&e, CM_TAG_COPYRIGHT);
        CHECK(s && strcmp(s, "(c) 2024 Bea") == 0);
        free(s);
        s = cm_exif_get_text(&e, CM_TAG_DESCRIPTION);
        CHECK(s && strcmp(s, "Caf\xC3\xA9 \xE2\x82\xAC") == 0);
        free(s);
        s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
        CHECK(s && strcmp(s, "after IDAT") == 0);
        free(s);
        s = cm_exif_get_text(&e, 271);
        CHECK(s && strcmp(s, "Canon") == 0);
        free(s);
        cm_exif_free(&e);
        CHECK(pc_meta_get(&m, "png.text.Title") && strcmp(pc_meta_get(&m, "png.text.Title"),
                                                          "Sunset") == 0);
        CHECK(strstr(cm_meta_xmp(&m, NULL), "tiff:Orientation=\"1\"") != NULL);
        CHECK(save("png", d, &m, &out) == PC_OK);
        CHECK(png_has_chunk(&out, "tEXt", "Author\0Bea \xE9t\xE9", 14));
        CHECK(png_has_chunk(&out, "tEXt", "Copyright\0(c)", 13));
        CHECK(png_has_chunk(&out, "iTXt", "Description\0", 12));          /* not Latin-1 */
        CHECK(png_has_chunk(&out, "tEXt", "Comment\0after", 13));
        CHECK(png_has_chunk(&out, "tEXt", "Title\0Sunset", 12));
        CHECK(png_has_chunk(&out, "iTXt", "XML:com.adobe.xmp\0", 18));
        CHECK(png_has_chunk(&out, "eXIf", "II*", 3));
        pc_doc_destroy(d);
        d = load("png", &out, &m2);
        CHECK(d && d->w == H);
        if (d) {
            CHECK(cm_meta_get_exif(&m2, &e) == PC_OK);
            s = cm_exif_get_text(&e, CM_TAG_ARTIST);
            CHECK(s && strcmp(s, "Bea \xC3\xA9t\xC3\xA9") == 0);
            free(s);
            s = cm_exif_get_text(&e, 36867);
            CHECK(s && strcmp(s, "2024:05:06 07:08:09") == 0);
            free(s);
            CHECK(cm_exif_find(&e, CM_TAG_MAKERNOTE) && cm_exif_find(&e, 2));
            cm_exif_free(&e);
            CHECK(strcmp(pc_meta_get(&m2, "png.text.Title"), "Sunset") == 0);
            CHECK(strcmp(cm_meta_xmp(&m2, NULL), cm_meta_xmp(&m, NULL)) == 0);
        }
        pc_meta_free(&m2);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
    pc_buf_free(&b);
    pc_buf_free(&ex);
    /* text only: no eXIf chunk is written for the four mapped tags */
    {
        pc_image_meta t;
        cm_exif e;
        memset(&t, 0, sizeof t);
        cm_exif_init(&e);
        CHECK(cm_exif_set_text(&e, CM_TAG_ARTIST, "Only") == PC_OK);
        CHECK(cm_meta_put_exif(&t, &e) == PC_OK);
        cm_exif_free(&e);
        d = gradient_doc(4, 4);
        CHECK(save("png", d, &t, &out) == PC_OK);
        CHECK(png_has_chunk(&out, "tEXt", "Author\0Only", 11));
        CHECK(!png_has_chunk(&out, "eXIf", "", 0));
        pc_buf_free(&out);
        pc_doc_destroy(d);
        pc_meta_free(&t);
    }
}

/* ---- WebP ------------------------------------------------------------------------------- */
static bool riff_has(const pc_buf *b, const char *fourcc)
{
    size_t pos = 12;
    while (pos + 8u <= b->n) {
        uint32_t len = (uint32_t)b->p[pos + 4] | ((uint32_t)b->p[pos + 5] << 8) |
                       ((uint32_t)b->p[pos + 6] << 16) | ((uint32_t)b->p[pos + 7] << 24);
        if (memcmp(b->p + pos, fourcc, 4) == 0) return true;
        pos += 8u + len + (len & 1u);
    }
    return false;
}

static void t_webp(void)
{
    const uint32_t W = 9, H = 6;
    uint8_t *rgba = (uint8_t *)malloc((size_t)W * H * 4u), *enc = NULL;
    size_t n;
    pc_buf ex, file, out;
    WebPData img, chunk, asm_out;
    WebPMux *mux;
    pc_image_meta m, m2;
    pc_doc *d;
    for (uint32_t i = 0; i < W * H; i++) {
        rgba[4 * i] = (uint8_t)(i * 4u); rgba[4 * i + 1] = (uint8_t)(255u - i);
        rgba[4 * i + 2] = 3; rgba[4 * i + 3] = 255;
    }
    n = WebPEncodeLosslessRGBA(rgba, (int)W, (int)H, (int)W * 4, &enc);
    CHECK(n > 0);
    memset(&ex, 0, sizeof ex);
    camera_exif(&ex, true, 3, 8);
    img.bytes = enc; img.size = n;
    mux = WebPMuxCreate(&img, 1);
    chunk.bytes = ex.p; chunk.size = ex.n;
    CHECK(WebPMuxSetChunk(mux, "EXIF", &chunk, 1) == WEBP_MUX_OK);
    chunk.bytes = (const uint8_t *)k_xmp6; chunk.size = sizeof k_xmp6 - 1u;
    CHECK(WebPMuxSetChunk(mux, "XMP ", &chunk, 1) == WEBP_MUX_OK);
    WebPDataInit(&asm_out);
    CHECK(WebPMuxAssemble(mux, &asm_out) == WEBP_MUX_OK);
    WebPMuxDelete(mux);
    file.p = (uint8_t *)(uintptr_t)asm_out.bytes; file.n = asm_out.size; file.cap = file.n;
    d = load("webp", &file, &m);
    CHECK(d && d->w == W && d->h == H);
    if (d) {
        pc_px32 p = pc_layer_get_px(d->stack[0], 0, 0);   /* orientation 3: turned 180 */
        CHECK(p.r == rgba[4 * (W * H - 1u)] && p.g == rgba[4 * (W * H - 1u) + 1]);
        check_camera(&m, "WebP load", true);
        CHECK(strstr(cm_meta_xmp(&m, NULL), "tiff:Orientation=\"1\"") != NULL);
        {
            int32_t prm[4];
            pc_codec_default_params(codec("webp"), prm);
            prm[3] = 1;                                    /* lossless */
            memset(&out, 0, sizeof out);
            CHECK(codec("webp")->save(d, &m, prm, NULL, &out) == PC_OK);
        }
        CHECK(riff_has(&out, "EXIF") && riff_has(&out, "XMP "));
        pc_doc_destroy(d);
        d = load("webp", &out, &m2);
        if (d) {
            pc_px32 q = pc_layer_get_px(d->stack[0], 0, 0);
            CHECK(tu_px_eq(q, p));                             /* not turned again */
        }
        check_camera(&m2, "WebP save", true);
        pc_meta_free(&m2);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
    WebPDataClear(&asm_out);
    WebPFree(enc);
    pc_buf_free(&ex);
    free(rgba);
}

/* ---- GIF comments ------------------------------------------------------------------------ */
static void t_gif(void)
{
    static const uint8_t gif[] = {
        'G', 'I', 'F', '8', '9', 'a', 2, 0, 2, 0, 0x80, 0, 0,
        0, 0, 0, 255, 255, 255,                                    /* 2-color table */
        0x21, 0xFE, 5, 'h', 'e', 'l', 'l', 'o', 3, ' ', 'G', 'I', 0,   /* comment */
        0x2C, 0, 0, 0, 0, 2, 0, 2, 0, 0,
        2, 3, 0x44, 0x02, 0x05, 0,                                 /* LZW: 0 1 1 0 */
        0x21, 0xFE, 4, 'm', 'o', 'r', 0xE9, 0,                     /* Latin-1, after */
        0x3B };
    pc_buf b, out;
    pc_image_meta m, m2;
    pc_doc *d;
    cm_exif e;
    char *s;
    b.p = (uint8_t *)(uintptr_t)gif; b.n = sizeof gif; b.cap = b.n;
    d = load("gif", &b, &m);
    CHECK(cm_meta_get_exif(&m, &e) == PC_OK);
    s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    CHECK(s && strcmp(s, "hello GI\nmor\xC3\xA9") == 0);
    free(s);
    cm_exif_free(&e);
    if (d) {
        CHECK(save("gif", d, &m, &out) == PC_OK);
        {   /* the comment extension, written once with both comments */
            bool found = false;
            for (size_t i = 0; i + 3u < out.n && !found; i++)
                found = out.p[i] == 0x21 && out.p[i + 1] == 0xFE && out.p[i + 2] == 14 &&
                        memcmp(out.p + i + 3, "hello GI\nmor\xC3\xA9", 14) == 0;
            CHECK(found);
        }
        pc_doc_destroy(d);
        d = load("gif", &out, &m2);
        CHECK(cm_meta_get_exif(&m2, &e) == PC_OK);
        s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
        CHECK(s && strcmp(s, "hello GI\nmor\xC3\xA9") == 0);
        free(s);
        cm_exif_free(&e);
        pc_meta_free(&m2);
        pc_buf_free(&out);
        /* GIF comment into a JPEG: EXIF UserComment */
        CHECK(d && save("jpeg", d, &m, &out) == PC_OK);
        pc_doc_destroy(d);
        d = load("jpeg", &out, &m2);
        CHECK(cm_meta_get_exif(&m2, &e) == PC_OK);
        s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
        CHECK(s && strcmp(s, "hello GI\nmor\xC3\xA9") == 0);
        free(s);
        cm_exif_free(&e);
        pc_meta_free(&m2);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
}

/* ---- .pdn -------------------------------------------------------------------------------- */
/* Equal EXIF items, except Software (the .pdn writer sets paint.c) and the
 * resolution tags (savers write meta.dpi into them). */
static bool same_exif(const pc_image_meta *a, const pc_image_meta *b)
{
    static const uint16_t k_skip[] = { CM_TAG_SOFTWARE, CM_TAG_XRES, CM_TAG_YRES,
                                       CM_TAG_RESUNIT };
    cm_exif x, y;
    bool ok;
    if (cm_meta_get_exif(a, &x) != PC_OK) return false;
    if (cm_meta_get_exif(b, &y) != PC_OK) { cm_exif_free(&x); return false; }
    for (size_t i = 0; i < sizeof k_skip / sizeof k_skip[0]; i++) {
        cm_exif_remove(&x, k_skip[i]);
        cm_exif_remove(&y, k_skip[i]);
    }
    ok = x.n == y.n;
    if (!ok) INFO("EXIF entry counts differ: %zu and %zu", x.n, y.n);
    for (size_t i = 0; i < x.n && ok; i++) {
        const cm_tag *t = &x.t[i], *u = NULL;
        for (size_t j = 0; j < y.n; j++)
            if (y.t[j].tag == t->tag && y.t[j].ifd == t->ifd) u = &y.t[j];
        ok = u && u->type == t->type && u->count == t->count && u->len == t->len &&
             memcmp(u->val, t->val, t->len) == 0;
        if (!ok) INFO("EXIF tag %u (IFD %u) differs", (unsigned)t->tag, (unsigned)t->ifd);
    }
    cm_exif_free(&x);
    cm_exif_free(&y);
    return ok;
}

static void t_pdn(void)
{
    pc_image_meta m, m2;
    pc_doc *d = gradient_doc(10, 7), *r;
    pc_buf out;
    size_t icc_n = 0;
    uint8_t *icc = itu_rgb("pdn metadata", 0u, &icc_n);   /* a usable profile (FS-ICC) */
    rich_meta(&m, 33);
    CHECK(icc != NULL);
    m.icc = (uint8_t *)malloc(icc_n);
    if (m.icc && icc) memcpy(m.icc, icc, icc_n);
    m.icc_len = m.icc && icc ? icc_n : 0u;
    CHECK(save("pdn", d, &m, &out) == PC_OK);
    {   /* the file uses Paint.NET's sections */
        const char *keys[] = { "$xmp.packet0", "$user.Palette", "$paintc.png.text.Title" };
        for (size_t k = 0; k < 3; k++) {
            bool found = false;
            size_t kl = strlen(keys[k]);
            for (size_t i = 0; i + kl <= out.n && !found; i++)
                found = memcmp(out.p + i, keys[k], kl) == 0;
            CHECK(found);
        }
    }
    r = load("pdn", &out, &m2);
    CHECK(r != NULL);
    if (r) {
        CHECK(same_exif(&m, &m2));
        check_camera(&m2, "PDN", true);
        check_iptc(&m2);
        CHECK(strcmp(cm_meta_xmp(&m2, NULL), cm_meta_xmp(&m, NULL)) == 0);
        CHECK(strcmp(pc_meta_get(&m2, "pdn.user.Palette"), "#FF0000") == 0);
        CHECK(strcmp(pc_meta_get(&m2, "png.text.Title"), "Sunset \xC3\xA0 Nice") == 0);
        CHECK(icc && m2.icc_len == icc_n && memcmp(m2.icc, icc, icc_n) == 0);
        CHECK(m2.dpi_x > 299.9 && m2.dpi_x < 300.1);
        CHECK(m2.n_items == m.n_items);
        {   /* and once more: stable */
            pc_buf out2;
            pc_image_meta m3;
            pc_doc *r3;
            CHECK(save("pdn", r, &m2, &out2) == PC_OK);
            r3 = load("pdn", &out2, &m3);
            CHECK(r3 && same_exif(&m2, &m3) && m3.n_items == m2.n_items);
            pc_doc_destroy(r3);
            pc_meta_free(&m3);
            pc_buf_free(&out2);
        }
    }
    pc_doc_destroy(r);
    pc_meta_free(&m2);
    pc_buf_free(&out);
    /* a plain document gets no "exif" item from Software and resolution alone */
    {
        pc_image_meta p;
        memset(&p, 0, sizeof p);
        p.dpi_x = p.dpi_y = 120.0;
        CHECK(save("pdn", d, &p, &out) == PC_OK);
        r = load("pdn", &out, &m2);
        CHECK(r && !pc_meta_get(&m2, CM_KEY_EXIF) && m2.n_items == 0u);
        pc_doc_destroy(r);
        pc_meta_free(&m2);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
    free(icc);
}

/* ---- conversions between the formats ------------------------------------------------------ */
static void t_cross(void)
{
    static const char *const chain[] = { "jpeg", "png", "webp", "tiff", "pdn", "jpeg" };
    pc_image_meta m;
    pc_doc *d = gradient_doc(16, 12);
    rich_meta(&m, 24);
    for (size_t i = 0; i < sizeof chain / sizeof chain[0]; i++) {
        pc_buf out;
        pc_image_meta m2;
        pc_doc *r;
        CHECK(save(chain[i], d, &m, &out) == PC_OK);
        r = load(chain[i], &out, &m2);
        pc_buf_free(&out);
        CHECK(r != NULL);
        check_camera(&m2, chain[i], true);
        CHECK(cm_meta_xmp(&m2, NULL) &&
              strcmp(cm_meta_xmp(&m2, NULL), cm_meta_xmp(&m, NULL)) == 0);
        if (strcmp(chain[i], "tiff") == 0 || strcmp(chain[i], "pdn") == 0 ||
            strcmp(chain[i], "jpeg") == 0)
            check_iptc(&m2);
        pc_doc_destroy(d);
        d = r;
        pc_meta_free(&m);
        m = m2;
        if (!d) break;
        /* PNG and WebP have no IPTC: put it back for the next format */
        if (!pc_meta_get(&m, CM_KEY_IPTC))
            CHECK(cm_meta_load_iptc(&m, k_iim, sizeof k_iim) == PC_OK);
    }
    pc_doc_destroy(d);
    pc_meta_free(&m);
}

/* ---- fuzzing of metadata-rich files ---------------------------------------------------------- */
static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 400u : 5000u, ok = 0;
    static const char *const ids[] = { "jpeg", "png", "webp", "pdn", "gif" };
    pc_image_meta m;
    pc_doc *d = gradient_doc(9, 7);
    rich_meta(&m, 40);
    for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        pc_buf out;
        CHECK(save(ids[k], d, &m, &out) == PC_OK);
        ok += tu_fuzz_codec(codec(ids[k]), out.p, out.n, iters,
                            strcmp(ids[k], "png") == 0 ? tu_png_fix_crcs : NULL);
        pc_buf_free(&out);
    }
    INFO("metadata fuzz: %u of %u mutated files decoded", ok, 5u * iters);
    pc_doc_destroy(d);
    pc_meta_free(&m);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_jpeg);
    RUN(t_png);
    RUN(t_webp);
    RUN(t_gif);
    RUN(t_pdn);
    RUN(t_cross);
    RUN(t_fuzz);
    return pc_test_finish();
}
