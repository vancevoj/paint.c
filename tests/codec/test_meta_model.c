/* test_meta_model.c - the codec metadata model of lane CODEC (cmeta.h):
 * item helpers, UTF-8 and Latin-1 text, EXIF parsing in both byte orders
 * (sub-IFDs kept, IFD1 thumbnail and structure tags dropped, BE values and
 * UNICODE UserComment converted), serialization round trips, text tags,
 * the saver normalization (orientation, pixel dimensions, resolution,
 * MakerNote fallback), XMP orientation reset, Photoshop IPTC resources,
 * the PNG text and GIF comment mappings, and mutation fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "meta_test_util.h"
#include "../../src/codec/cmeta.h"

#include <stdio.h>

static void t_items(void)
{
    pc_image_meta m;
    uint8_t blob[300], *back = NULL;
    size_t n = 0;
    memset(&m, 0, sizeof m);
    CHECK(cm_set(&m, "a", "1") == PC_OK && cm_set(&m, "b", "2") == PC_OK);
    CHECK(cm_set(&m, "a", "3") == PC_OK);
    CHECK(m.n_items == 2u && strcmp(pc_meta_get(&m, "a"), "3") == 0);
    cm_remove(&m, "a");
    CHECK(m.n_items == 1u && !pc_meta_get(&m, "a") && strcmp(pc_meta_get(&m, "b"), "2") == 0);
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)(i * 7u + 3u);
    for (size_t len = 1; len <= 7u; len++) {         /* every padding case */
        CHECK(cm_set_blob(&m, "x", blob, len) == PC_OK);
        CHECK(cm_get_blob(&m, "x", &back, &n) == PC_OK && n == len && back &&
              memcmp(back, blob, len) == 0);
        free(back);
    }
    CHECK(cm_set_blob(&m, "x", blob, sizeof blob) == PC_OK);
    CHECK(cm_get_blob(&m, "x", &back, &n) == PC_OK && n == sizeof blob && back &&
          memcmp(back, blob, n) == 0);
    free(back);
    CHECK(cm_set(&m, "bad", "@@@@") == PC_OK);
    CHECK(cm_get_blob(&m, "bad", &back, &n) == PC_OK && back == NULL && n == 0);
    CHECK(cm_get_blob(&m, "missing", &back, &n) == PC_OK && back == NULL);
    cm_remove(&m, "b");
    cm_remove(&m, "x");
    cm_remove(&m, "bad");
    CHECK(m.n_items == 0u && m.items == NULL);
    pc_meta_free(&m);
}

static void t_text(void)
{
    static const uint8_t ok1[] = "plain", ok2[] = "\xC3\xA9t\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
    static const uint8_t bad1[] = "\xC0\xAF", bad2[] = "\xED\xA0\x80", bad3[] = "\xE2\x82";
    static const uint8_t lat[] = { 'c', 'a', 'f', 0xE9, 0 };
    uint8_t out[16];
    size_t n = 0;
    char *s;
    CHECK(cm_utf8_valid(ok1, 5) && cm_utf8_valid(ok2, sizeof ok2 - 1u));
    CHECK(!cm_utf8_valid(bad1, 2) && !cm_utf8_valid(bad2, 3) && !cm_utf8_valid(bad3, 2));
    CHECK(!cm_utf8_valid((const uint8_t *)"a\0b", 3));
    s = cm_latin1_to_utf8(lat, 4);
    CHECK(s && strcmp(s, "caf\xC3\xA9") == 0);
    CHECK(s && cm_utf8_to_latin1(s, strlen(s), out, sizeof out, &n) && n == 4 &&
          memcmp(out, lat, 4) == 0);
    free(s);
    s = cm_text_to_utf8(lat, 4);                     /* not UTF-8: read as Latin-1 */
    CHECK(s && strcmp(s, "caf\xC3\xA9") == 0);
    free(s);
    s = cm_text_to_utf8(ok2, sizeof ok2 - 1u);
    CHECK(s && strcmp(s, (const char *)ok2) == 0);
    free(s);
    CHECK(!cm_utf8_to_latin1((const char *)ok2, sizeof ok2 - 1u, out, sizeof out, &n));
    CHECK(!cm_utf8_to_latin1("abcdef", 6, out, 3, &n));
}

/* A camera-like EXIF block: IFD0 with structure and text tags, an Exif IFD
 * with a UNICODE UserComment and a MakerNote, GPS, and an IFD1 thumbnail. */
static void camera_exif(pc_buf *b, bool be, uint32_t orientation)
{
    static const uint8_t make[] = "Canon", artist[] = "Ann Example";
    static const uint8_t dto[] = "2024:05:06 07:08:09";
    static const uint8_t gpsver[] = { 2, 3, 0, 0 }, latref[] = "N";
    static const uint8_t exifver[] = { '0', '2', '3', '2' }, mk[40] = { 1, 2, 3, 4, 5 };
    uint8_t uc[8 + 10];
    tx_ent i0[7], ex[5], gp[3], i1[3];
    memcpy(uc, "UNICODE\0", 8u);
    /* "h\xE9llo" as UTF-16 in the block's byte order */
    {
        static const uint16_t u[5] = { 'h', 0xE9, 'l', 'l', 'o' };
        for (int i = 0; i < 5; i++) tx_put(uc + 8 + 2 * i, u[i], 2u, be);
    }
    memset(i0, 0, sizeof i0); memset(ex, 0, sizeof ex); memset(gp, 0, sizeof gp);
    memset(i1, 0, sizeof i1);
    i0[0].tag = 256; i0[0].type = 4; i0[0].count = 1; i0[0].nums[0] = 4000;   /* dropped */
    i0[1].tag = 271; i0[1].type = 2; i0[1].count = 6; i0[1].bytes = make;
    i0[2].tag = 273; i0[2].type = 4; i0[2].count = 2; i0[2].nums[0] = 9; i0[2].nums[1] = 10;
    i0[3].tag = 274; i0[3].type = 3; i0[3].count = 1; i0[3].nums[0] = orientation;
    i0[4].tag = 282; i0[4].type = 5; i0[4].count = 1; i0[4].nums[0] = 300; i0[4].nums[1] = 1;
    i0[5].tag = 296; i0[5].type = 3; i0[5].count = 1; i0[5].nums[0] = 2;
    i0[6].tag = 315; i0[6].type = 2; i0[6].count = 12; i0[6].bytes = artist;
    ex[0].tag = 33434; ex[0].type = 5; ex[0].count = 1; ex[0].nums[0] = 1; ex[0].nums[1] = 125;
    ex[1].tag = 36864; ex[1].type = 7; ex[1].count = 4; ex[1].bytes = exifver;
    ex[2].tag = 36867; ex[2].type = 2; ex[2].count = 20; ex[2].bytes = dto;
    ex[3].tag = 37500; ex[3].type = 7; ex[3].count = 40; ex[3].bytes = mk;
    ex[4].tag = 37510; ex[4].type = 7; ex[4].count = 18; ex[4].bytes = uc;
    gp[0].tag = 0; gp[0].type = 1; gp[0].count = 4; gp[0].bytes = gpsver;
    gp[1].tag = 1; gp[1].type = 2; gp[1].count = 2; gp[1].bytes = latref;
    gp[2].tag = 2; gp[2].type = 5; gp[2].count = 3;
    gp[2].nums[0] = 52; gp[2].nums[1] = 1; gp[2].nums[2] = 31; gp[2].nums[3] = 1;
    gp[2].nums[4] = 1234; gp[2].nums[5] = 100;
    i1[0].tag = 259; i1[0].type = 3; i1[0].count = 1; i1[0].nums[0] = 6;
    i1[1].tag = 513; i1[1].type = 4; i1[1].count = 1; i1[1].nums[0] = 8;
    i1[2].tag = 514; i1[2].type = 4; i1[2].count = 1; i1[2].nums[0] = 100;
    tx_build(b, be, i0, 7, ex, 5, gp, 3, i1, 3);
}

static uint32_t le32_of(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void check_camera_set(const cm_exif *e)
{
    const cm_tag *t;
    char *s;
    double x = 0, y = 0;
    CHECK(cm_exif_find(e, 256) == NULL && cm_exif_find(e, 273) == NULL);   /* structure */
    CHECK(cm_exif_find(e, 259) == NULL && cm_exif_find(e, 513) == NULL);   /* IFD1 */
    CHECK(cm_exif_find(e, CM_TAG_EXIF_IFD) == NULL && cm_exif_find(e, CM_TAG_GPS_IFD) == NULL);
    CHECK(cm_exif_count(e, CM_IFD0) == 5u);           /* Make, Orientation, XRes, Unit, Artist */
    CHECK(cm_exif_count(e, CM_IFD_EXIF) == 5u && cm_exif_count(e, CM_IFD_GPS) == 3u);
    s = cm_exif_get_text(e, 271);
    CHECK(s && strcmp(s, "Canon") == 0);
    free(s);
    s = cm_exif_get_text(e, CM_TAG_ARTIST);
    CHECK(s && strcmp(s, "Ann Example") == 0);
    free(s);
    s = cm_exif_get_text(e, CM_TAG_USERCOMMENT);
    CHECK(s && strcmp(s, "h\xC3\xA9llo") == 0);
    free(s);
    t = cm_exif_find(e, 33434);
    CHECK(t && t->ifd == CM_IFD_EXIF && t->type == 5u && t->len == 8u && le32_of(t->val) == 1u &&
          le32_of(t->val + 4) == 125u);
    t = cm_exif_find(e, 2);
    CHECK(t && t->ifd == CM_IFD_GPS && t->count == 3u && le32_of(t->val + 16) == 1234u);
    t = cm_exif_find(e, CM_TAG_MAKERNOTE);
    CHECK(t && t->len == 40u && t->val[0] == 1 && t->val[4] == 5);
    CHECK(!cm_exif_resolution(e, &x, &y));             /* YResolution missing */
}

static void t_exif_parse(void)
{
    for (int be = 0; be < 2; be++) {
        pc_buf b, ser;
        cm_exif e, e2;
        memset(&b, 0, sizeof b);
        memset(&ser, 0, sizeof ser);
        camera_exif(&b, be != 0, 6);
        cm_exif_init(&e);
        CHECK(cm_exif_parse(b.p, b.n, 0u, &e) == PC_OK);
        check_camera_set(&e);
        CHECK(cm_exif_orientation(&e) == 6);
        CHECK(cm_exif_serialize(&e, &ser) == PC_OK && ser.n > 8u);
        CHECK(ser.n > 4u && memcmp(ser.p, "II*\0", 4u) == 0);
        cm_exif_init(&e2);
        CHECK(cm_exif_parse(ser.p, ser.n, 0u, &e2) == PC_OK);
        check_camera_set(&e2);
        CHECK(e2.n == e.n);
        for (size_t i = 0; i < e.n; i++) {
            const cm_tag *a = &e.t[i], *c = NULL;
            for (size_t j = 0; j < e2.n; j++)
                if (e2.t[j].tag == a->tag && e2.t[j].ifd == a->ifd) c = &e2.t[j];
            CHECK(c && c->type == a->type && c->count == a->count && c->len == a->len &&
                  memcmp(c->val, a->val, a->len) == 0);
        }
        {   /* the serialized block is a well-formed TIFF: sorted tags, pointers */
            tx_rd r;
            uint32_t i0, exo, gpo;
            CHECK(tx_open(&r, ser.p, ser.n));
            i0 = tx_ifd0(&r);
            exo = tx_num(&r, i0, 34665, 0);
            gpo = tx_num(&r, i0, 34853, 0);
            CHECK(exo && gpo && tx_next(&r, i0) == 0u);
            CHECK(tx_str_is(&r, i0, 271, "Canon") && tx_num(&r, i0, 274, 0) == 6u);
            CHECK(tx_entries(&r, exo) == 5u && tx_entries(&r, gpo) == 3u);
            for (uint32_t k = 1; k < tx_entries(&r, i0); k++)
                CHECK(tx_get(&r, i0 + 2u + 12u * k, 2u) > tx_get(&r, i0 + 2u + 12u * (k - 1u), 2u));
        }
        cm_exif_free(&e2);
        cm_exif_free(&e);
        pc_buf_free(&b);
        pc_buf_free(&ser);
    }
    {   /* not TIFF, empty, broken offsets */
        cm_exif e;
        uint8_t junk[16] = { 'X', 'X', 42, 0 };
        uint8_t bad[] = { 'I', 'I', 42, 0, 0xF0, 0, 0, 0 };
        cm_exif_init(&e);
        CHECK(cm_exif_parse(junk, sizeof junk, 0u, &e) == PC_ERR_FORMAT);
        CHECK(cm_exif_parse(junk, 4, 0u, &e) == PC_ERR_FORMAT);
        CHECK(cm_exif_parse(bad, sizeof bad, 0u, &e) == PC_OK && e.n == 0u);
        cm_exif_free(&e);
    }
}

static void t_exif_text(void)
{
    cm_exif e;
    pc_buf ser;
    char *s;
    const cm_tag *t;
    cm_exif_init(&e);
    memset(&ser, 0, sizeof ser);
    CHECK(cm_exif_set_text(&e, CM_TAG_ARTIST, "Ana Mar\xC3\xAD" "a") == PC_OK);
    CHECK(cm_exif_set_text(&e, CM_TAG_USERCOMMENT, "plain ascii") == PC_OK);
    t = cm_exif_find(&e, CM_TAG_USERCOMMENT);
    CHECK(t && t->ifd == CM_IFD_EXIF && t->type == 7u && memcmp(t->val, "ASCII\0\0\0", 8) == 0);
    CHECK(cm_exif_set_text(&e, CM_TAG_DESCRIPTION, "x") == PC_OK);
    t = cm_exif_find(&e, CM_TAG_ARTIST);
    CHECK(t && t->ifd == CM_IFD0 && t->type == 2u && t->count == 11u);
    s = cm_exif_get_text(&e, CM_TAG_ARTIST);
    CHECK(s && strcmp(s, "Ana Mar\xC3\xAD" "a") == 0);
    free(s);
    CHECK(cm_exif_set_text(&e, CM_TAG_USERCOMMENT, "\xE2\x82\xAC \xF0\x9F\x98\x80") == PC_OK);
    t = cm_exif_find(&e, CM_TAG_USERCOMMENT);
    CHECK(t && memcmp(t->val, "UNICODE\0", 8) == 0 && t->len == 8u + 2u + 2u + 4u);
    CHECK(cm_exif_serialize(&e, &ser) == PC_OK);
    cm_exif_free(&e);
    cm_exif_init(&e);
    CHECK(cm_exif_parse(ser.p, ser.n, 0u, &e) == PC_OK);
    s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    CHECK(s && strcmp(s, "\xE2\x82\xAC \xF0\x9F\x98\x80") == 0);
    free(s);
    CHECK(cm_exif_set_text(&e, CM_TAG_DESCRIPTION, "") == PC_OK);
    CHECK(cm_exif_find(&e, CM_TAG_DESCRIPTION) == NULL);
    CHECK(cm_exif_set_text(&e, CM_TAG_ARTIST, "\xC0\xAF") == PC_ERR_ARG);   /* not UTF-8 */
    {   /* JIS UserComment: not text */
        uint8_t jis[12] = { 'J', 'I', 'S', 0, 0, 0, 0, 0, 1, 2, 3, 4 };
        CHECK(cm_exif_set(&e, CM_IFD_EXIF, CM_TAG_USERCOMMENT, 7, 12, jis, 12) == PC_OK);
        CHECK(cm_exif_get_text(&e, CM_TAG_USERCOMMENT) == NULL);
    }
    CHECK(cm_exif_set(&e, CM_IFD0, 271, 3, 2, "ab", 3) == PC_ERR_ARG);    /* len mismatch */
    cm_exif_free(&e);
    pc_buf_free(&ser);
}

static void t_exif_save(void)
{
    pc_image_meta m;
    pc_buf b;
    uint8_t *out = NULL;
    size_t n = 0;
    cm_exif e;
    double x = 0, y = 0;
    int o = 0;
    memset(&m, 0, sizeof m);
    memset(&b, 0, sizeof b);
    camera_exif(&b, true, 6);
    CHECK(cm_meta_load_exif(&m, b.p, b.n, true, &o) == PC_OK && o == 6);
    CHECK(cm_meta_get_exif(&m, &e) == PC_OK && cm_exif_orientation(&e) == 1);   /* upright */
    {   /* PixelXDimension as SHORT, to be updated (and widened) on save */
        uint8_t v[2] = { 100, 0 };
        CHECK(cm_exif_set(&e, CM_IFD_EXIF, CM_TAG_PIXEL_X, 3, 1, v, 2) == PC_OK);
        CHECK(cm_exif_set(&e, CM_IFD_EXIF, CM_TAG_PIXEL_Y, 3, 1, v, 2) == PC_OK);
        CHECK(cm_meta_put_exif(&m, &e) == PC_OK);
    }
    cm_exif_free(&e);
    m.dpi_x = 150.0;
    m.dpi_y = 72.5;
    CHECK(cm_exif_for_save(&m, 70000u, 50u, NULL, 0, 1u << 20, &out, &n) == PC_OK && out);
    cm_exif_init(&e);
    CHECK(out && cm_exif_parse(out, n, 0u, &e) == PC_OK);
    {
        const cm_tag *px = cm_exif_find(&e, CM_TAG_PIXEL_X), *py = cm_exif_find(&e, CM_TAG_PIXEL_Y);
        CHECK(px && px->type == 4u && le32_of(px->val) == 70000u);
        CHECK(py && py->type == 3u && py->val[0] == 50u && py->val[1] == 0u);
    }
    CHECK(cm_exif_resolution(&e, &x, &y) && x > 149.99 && x < 150.01 && y > 72.49 && y < 72.51);
    CHECK(cm_exif_orientation(&e) == 1);
    cm_exif_free(&e);
    free(out);
    /* drop list, and a size limit that only fits without the MakerNote */
    {
        static const uint16_t drop[] = { CM_TAG_ARTIST, 2 };
        size_t full;
        CHECK(cm_exif_for_save(&m, 10, 10, drop, 2, 1u << 20, &out, &n) == PC_OK && out);
        cm_exif_init(&e);
        CHECK(out && cm_exif_parse(out, n, 0u, &e) == PC_OK);
        CHECK(!cm_exif_find(&e, CM_TAG_ARTIST) && !cm_exif_find(&e, 2) && cm_exif_find(&e, 1));
        cm_exif_free(&e);
        free(out);
        CHECK(cm_exif_for_save(&m, 10, 10, NULL, 0, 1u << 20, &out, &full) == PC_OK && out);
        free(out);
        CHECK(cm_exif_for_save(&m, 10, 10, NULL, 0, full - 20u, &out, &n) == PC_OK && out &&
              n < full);
        cm_exif_init(&e);
        CHECK(out && cm_exif_parse(out, n, 0u, &e) == PC_OK && !cm_exif_find(&e, 37500));
        cm_exif_free(&e);
        free(out);
        CHECK(cm_exif_for_save(&m, 10, 10, NULL, 0, 40u, &out, &n) == PC_OK && out == NULL);
    }
    pc_meta_free(&m);
    pc_buf_free(&b);
    /* a block holding only what the container restates is not written */
    {
        tx_ent i0[3];
        memset(i0, 0, sizeof i0);
        i0[0].tag = 274; i0[0].type = 3; i0[0].count = 1; i0[0].nums[0] = 1;
        i0[1].tag = 282; i0[1].type = 5; i0[1].count = 1; i0[1].nums[0] = 72; i0[1].nums[1] = 1;
        i0[2].tag = 296; i0[2].type = 3; i0[2].count = 1; i0[2].nums[0] = 2;
        memset(&m, 0, sizeof m);
        tx_build(&b, false, i0, 3, NULL, 0, NULL, 0, NULL, 0);
        CHECK(cm_meta_load_exif(&m, b.p, b.n, false, &o) == PC_OK && o == 1);
        CHECK(pc_meta_get(&m, CM_KEY_EXIF) != NULL);
        CHECK(cm_exif_for_save(&m, 5, 5, NULL, 0, 1u << 20, &out, &n) == PC_OK && !out);
        pc_meta_free(&m);
        pc_buf_free(&b);
    }
    /* "Exif\0\0" prefix accepted, merge keeps existing entries, broken ignored */
    {
        pc_buf p2;
        char *s;
        memset(&p2, 0, sizeof p2);
        memset(&m, 0, sizeof m);
        pc_buf_append(&p2, "Exif\0\0", 6);
        camera_exif(&p2, false, 3);
        CHECK(cm_meta_load_exif(&m, p2.p, p2.n, false, &o) == PC_OK && o == 3);
        CHECK(cm_meta_get_exif(&m, &e) == PC_OK && cm_exif_orientation(&e) == 3);  /* kept */
        CHECK(cm_exif_set_text(&e, CM_TAG_ARTIST, "Other") == PC_OK);
        CHECK(cm_meta_put_exif(&m, &e) == PC_OK);
        cm_exif_free(&e);
        CHECK(cm_meta_load_exif(&m, p2.p, p2.n, true, &o) == PC_OK);
        CHECK(cm_meta_get_exif(&m, &e) == PC_OK);
        s = cm_exif_get_text(&e, CM_TAG_ARTIST);
        CHECK(s && strcmp(s, "Other") == 0);
        free(s);
        cm_exif_free(&e);
        CHECK(cm_meta_load_exif(&m, (const uint8_t *)"Exif\0\0junk", 10, true, &o) == PC_OK &&
              o == 1);
        pc_meta_free(&m);
        pc_buf_free(&p2);
    }
}

static void t_xmp(void)
{
    pc_image_meta m;
    const char *x;
    static const char k_attr[] =
        "<x:xmpmeta><rdf:Description tiff:Orientation=\"6\" exif:Foo=\"6\"/>"
        "<rdf:Description><tiff:Orientation>8</tiff:Orientation></rdf:Description>"
        "<rdf:Description tiff:Orientation = '3'/></x:xmpmeta>";
    memset(&m, 0, sizeof m);
    CHECK(cm_meta_load_xmp(&m, (const uint8_t *)"\xC0\xAF", 2) == PC_OK && !cm_meta_xmp(&m, NULL));
    CHECK(cm_meta_load_xmp(&m, (const uint8_t *)k_attr, sizeof k_attr) == PC_OK);  /* + NUL */
    x = cm_meta_xmp(&m, NULL);
    CHECK(x && strlen(x) == sizeof k_attr - 1u);
    CHECK(cm_meta_load_xmp(&m, (const uint8_t *)"other", 5) == PC_OK);   /* first one kept */
    CHECK(cm_meta_xmp_reset_orientation(&m) == PC_OK);
    x = cm_meta_xmp(&m, NULL);
    CHECK(x && strstr(x, "tiff:Orientation=\"1\"") && strstr(x, "exif:Foo=\"6\"") &&
          strstr(x, "<tiff:Orientation>1</tiff:Orientation>") &&
          strstr(x, "tiff:Orientation = '1'"));
    pc_meta_free(&m);
    /* loading EXIF with an orientation also resets an XMP loaded before it */
    {
        pc_buf b;
        int o = 0;
        memset(&b, 0, sizeof b);
        memset(&m, 0, sizeof m);
        CHECK(cm_meta_load_xmp(&m, (const uint8_t *)"<a tiff:Orientation=\"6\"/>", 25) == PC_OK);
        camera_exif(&b, false, 6);
        CHECK(cm_meta_load_exif(&m, b.p, b.n, true, &o) == PC_OK && o == 6);
        CHECK(strstr(cm_meta_xmp(&m, NULL), "tiff:Orientation=\"1\"") != NULL);
        pc_meta_free(&m);
        pc_buf_free(&b);
    }
}

static void t_iptc(void)
{
    pc_buf b;
    const uint8_t *iim = NULL;
    size_t n = 0;
    static const uint8_t data[] = { 0x1C, 2, 0x50, 0, 5, 'A', 'u', 't', 'h', 'r' };
    memset(&b, 0, sizeof b);
    /* an unrelated resource with a 3-byte name and odd size comes first */
    pc_buf_append(&b, "8BIM\x03\xED\x03" "abc" "\0\0\0\x03" "xyz" "\0", 18);
    CHECK(cm_irb_put_iptc(&b, data, sizeof data) == PC_OK);
    CHECK(cm_irb_find_iptc(b.p, b.n, &iim, &n) && n == sizeof data && memcmp(iim, data, n) == 0);
    CHECK(!cm_irb_find_iptc(b.p, 17, &iim, &n));          /* truncated */
    b.p[18] = 'X';                                        /* second signature broken */
    CHECK(!cm_irb_find_iptc(b.p, b.n, &iim, &n));
    pc_buf_free(&b);
    {
        pc_image_meta m;
        uint8_t *back = NULL;
        memset(&m, 0, sizeof m);
        CHECK(cm_meta_load_iptc(&m, data, sizeof data) == PC_OK);
        CHECK(cm_get_blob(&m, CM_KEY_IPTC, &back, &n) == PC_OK && n == sizeof data && back &&
              memcmp(back, data, n) == 0);
        free(back);
        pc_meta_free(&m);
    }
}

static void t_png_text_and_comment(void)
{
    pc_image_meta m;
    cm_exif e;
    uint16_t tag = 0;
    char *s;
    CHECK(cm_png_text_tag("Author") == CM_TAG_ARTIST && cm_png_text_tag("Comment") == 37510u);
    CHECK(cm_png_text_tag("author") == 0u && cm_png_text_tag("Title") == 0u);
    CHECK(cm_png_text_map(0, &tag) && tag == CM_TAG_ARTIST && cm_png_text_map(4, &tag) == NULL);
    memset(&m, 0, sizeof m);
    cm_exif_init(&e);
    CHECK(cm_meta_load_png_text(&m, &e, "Author", "Bea") == PC_OK);
    CHECK(cm_meta_load_png_text(&m, &e, "Title", "Sunset") == PC_OK);
    CHECK(cm_meta_load_png_text(&m, &e, "Title", "Second") == PC_OK);       /* first wins */
    CHECK(cm_meta_load_png_text(&m, &e, "Comment", "caf\xC3\xA9") == PC_OK);
    CHECK(strcmp(pc_meta_get(&m, "png.text.Title"), "Sunset") == 0);
    s = cm_exif_get_text(&e, CM_TAG_ARTIST);
    CHECK(s && strcmp(s, "Bea") == 0);
    free(s);
    s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    CHECK(s && strcmp(s, "caf\xC3\xA9") == 0);
    free(s);
    cm_exif_free(&e);
    /* GIF comments: UTF-8 or Latin-1, appended with a newline */
    CHECK(cm_meta_load_comment(&m, (const uint8_t *)"first", 5) == PC_OK);
    CHECK(cm_meta_load_comment(&m, (const uint8_t *)"caf\xE9  ", 6) == PC_OK);
    CHECK(cm_meta_get_exif(&m, &e) == PC_OK);
    s = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    CHECK(s && strcmp(s, "first\ncaf\xC3\xA9") == 0);
    free(s);
    cm_exif_free(&e);
    pc_meta_free(&m);
}

/* The entry cap: a full set refuses new tags, callers keep going. */
static void t_entry_cap(void)
{
    cm_exif e;
    pc_image_meta m;
    pc_buf b;
    uint8_t *out = NULL;
    size_t n = 0;
    uint8_t v[2] = { 1, 0 };
    cm_exif_init(&e);
    memset(&m, 0, sizeof m);
    memset(&b, 0, sizeof b);
    for (uint32_t i = 0; i < CM_EXIF_MAX_ENTRIES; i++)
        CHECK(cm_exif_set(&e, (uint8_t)(i % 4u), (uint16_t)(1000u + i / 4u), 3, 1, v, 2) ==
              PC_OK);
    CHECK(e.n == CM_EXIF_MAX_ENTRIES);
    CHECK(cm_exif_set(&e, CM_IFD0, 60000, 3, 1, v, 2) == PC_ERR_LIMIT);
    CHECK(cm_exif_set(&e, CM_IFD0, 1002, 3, 1, v, 2) == PC_OK);        /* replacing works */
    CHECK(cm_meta_load_png_text(&m, &e, "Author", "x") == PC_OK);      /* skipped, no error */
    CHECK(cm_exif_find(&e, CM_TAG_ARTIST) == NULL);
    CHECK(cm_meta_put_exif(&m, &e) == PC_OK && pc_meta_get(&m, CM_KEY_EXIF));
    m.dpi_x = m.dpi_y = 300.0;
    CHECK(cm_exif_for_save(&m, 10, 10, NULL, 0, (size_t)1 << 24, &out, &n) == PC_OK && out);
    free(out);
    CHECK(cm_meta_load_comment(&m, (const uint8_t *)"c", 1) == PC_OK);
    cm_exif_free(&e);
    pc_meta_free(&m);
    pc_buf_free(&b);
}

/* Mutated EXIF blocks: parsing never crashes and whatever is parsed
 * serializes into a block that parses to the same entries. */
static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 3000u : 30000u, parsed = 0;
    pc_buf seeds[2];
    memset(seeds, 0, sizeof seeds);
    camera_exif(&seeds[0], true, 6);
    camera_exif(&seeds[1], false, 2);
    for (uint32_t i = 0; i < iters; i++) {
        const pc_buf *sd = &seeds[i & 1u];
        uint8_t *buf = (uint8_t *)malloc(sd->n + 64u);
        size_t len = tu_mutate(sd->p, sd->n, buf, sd->n + 64u);
        cm_exif e, e2;
        pc_buf ser;
        cm_exif_init(&e);
        memset(&ser, 0, sizeof ser);
        if (cm_exif_parse(buf, len, 0u, &e) == PC_OK) {
            parsed++;
            (void)cm_exif_orientation(&e);
            free(cm_exif_get_text(&e, CM_TAG_USERCOMMENT));
            free(cm_exif_get_text(&e, 271));
            CHECK(cm_exif_serialize(&e, &ser) == PC_OK);
            cm_exif_init(&e2);
            if (ser.n) {
                CHECK(cm_exif_parse(ser.p, ser.n, 0u, &e2) == PC_OK);
                CHECK(e2.n == e.n);
            }
            cm_exif_free(&e2);
        }
        {   /* the same bytes as an IRB and as an XMP packet */
            const uint8_t *iim;
            size_t n;
            pc_image_meta m;
            (void)cm_irb_find_iptc(buf, len, &iim, &n);
            memset(&m, 0, sizeof m);
            CHECK(cm_meta_load_xmp(&m, buf, len) == PC_OK);
            CHECK(cm_meta_xmp_reset_orientation(&m) == PC_OK);
            pc_meta_free(&m);
        }
        cm_exif_free(&e);
        pc_buf_free(&ser);
        free(buf);
    }
    INFO("exif fuzz: %u of %u mutated blocks parsed", parsed, iters);
    pc_buf_free(&seeds[0]);
    pc_buf_free(&seeds[1]);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_items);
    RUN(t_text);
    RUN(t_exif_parse);
    RUN(t_exif_text);
    RUN(t_exif_save);
    RUN(t_xmp);
    RUN(t_iptc);
    RUN(t_png_text_and_comment);
    RUN(t_entry_cap);
    RUN(t_fuzz);
    return pc_test_finish();
}
