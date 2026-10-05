/* test_lib_ora.c - OpenRaster: layered round trip (3 layers, blend modes,
 * hidden layer, opacity, offsets, escaped names), every blend mode, hand
 * written stack.xml variants (nested stacks, entities, unknown ops, non-PNG
 * sources), strict XML rejection, limits, archive and XML fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/zip.h"

static const pc_codec *ora(void) { return pc_codec_by_id("ora"); }
static const pc_codec *png(void) { return pc_codec_by_id("png"); }

static void t_sniff(void)
{
    pc_buf b;
    pc_zipw w;
    CHECK(ora() && (ora()->flags & PC_CODEC_LAYERED));
    memset(&b, 0, sizeof b);
    pc_zipw_init(&w, &b);
    pc_zipw_add(&w, "mimetype", "image/openraster", 16, false);
    pc_zipw_finish(&w);
    pc_zipw_free(&w);
    CHECK(ora()->sniff(b.p, b.n));
    CHECK(pc_codec_sniff(b.p, b.n) == ora());
    b.p[40] = 'X';
    CHECK(!ora()->sniff(b.p, b.n));
    CHECK(!ora()->sniff(b.p, 20));
    CHECK(pc_codec_by_ext("ORA") == ora());
    pc_buf_free(&b);
}

/* Read one archive member and decode it as PNG. */
static pc_doc *member_png(const pc_buf *b, const char *name)
{
    pc_zip z;
    pc_doc *d = NULL;
    const pc_zip_entry *e;
    uint8_t *data;
    size_t len;
    pc_image_meta m;
    if (pc_zip_open(&z, b->p, b->n, NULL) != PC_OK) return NULL;
    e = pc_zip_find(&z, name);
    if (e && pc_zip_read(&z, e, &data, &len) == PC_OK) {
        if (png()->load(data, len, NULL, &d, &m) == PC_OK) pc_meta_free(&m);
        free(data);
    }
    pc_zip_close(&z);
    return d;
}

static void t_roundtrip(void)
{
    const uint32_t W = 150, H = 100;
    pc_px32 *bg = tu_photo(W, H, false), *mid = tu_noise(W, H, 1), *top;
    pc_doc *d = tu_doc_from_px(W, H, bg), *r;
    pc_layer *l1, *l2;
    pc_image_meta meta, m2;
    pc_buf out;
    /* top layer: content only in a small region so the PNG gets cropped */
    top = (pc_px32 *)calloc((size_t)W * H, sizeof *top);
    for (uint32_t y = 70; y < 90; y++)
        for (uint32_t x = 100; x < 140; x++) top[y * W + x] = tu_px(10, 200, 30, 180);
    l1 = tu_doc_add_layer(d, mid, PC_BLEND_MULTIPLY, 128, false, "Mid & <hidden> \"q\" 'a'");
    l2 = tu_doc_add_layer(d, top, PC_BLEND_REFLECT, 77, true, "Top \xc3\xa9t\xc3\xa9");
    (void)l1; (void)l2;
    strcpy(d->stack[0]->name, "Background");
    memset(&meta, 0, sizeof meta);
    meta.dpi_x = 300; meta.dpi_y = 300;
    memset(&out, 0, sizeof out);
    CHECK(ora()->save(d, &meta, NULL, NULL, &out) == PC_OK);
    CHECK(memcmp(out.p + 30, "mimetypeimage/openraster", 24) == 0);
    r = NULL;
    CHECK(ora()->load(out.p, out.n, NULL, &r, &m2) == PC_OK);
    if (r) {
        CHECK(r->w == W && r->h == H && r->n_layers == 3);
        CHECK(fabs(m2.dpi_x - 300) < 1e-9 && fabs(m2.dpi_y - 300) < 1e-9);
        for (uint32_t i = 0; i < 3 && r->n_layers == 3; i++) {
            const pc_layer *a = d->stack[i], *b = r->stack[i];
            pc_px32 *pa = tu_layer_px(d, a), *pb = tu_layer_px(r, b);
            CHECK(strcmp(a->name, b->name) == 0);
            CHECK(a->mode == b->mode && a->opacity == b->opacity && a->visible == b->visible);
            CHECK(tu_diff(pa, pb, (size_t)W * H) == 0);
            free(pa); free(pb);
        }
        pc_doc_destroy(r);
        pc_meta_free(&m2);
    }
    /* the cropped layer has an offset in stack.xml; merged image and thumbnail */
    {
        pc_doc *mg = member_png(&out, "mergedimage.png"), *th = member_png(&out, "Thumbnails/thumbnail.png");
        pc_doc *l2png = member_png(&out, "data/layer2.png");
        CHECK(mg && mg->w == W && mg->h == H);
        if (mg) {
            pc_px32 *a = tu_flatten(d), *b = tu_layer_px(mg, mg->stack[0]);
            CHECK(tu_diff(a, b, (size_t)W * H) == 0);
            free(a); free(b);
        }
        CHECK(th && th->w <= 256 && th->h <= 256);
        CHECK(l2png && l2png->w == 86 && l2png->h == 36);       /* tiles x 64..150, y 64..100 */
        pc_doc_destroy(mg); pc_doc_destroy(th); pc_doc_destroy(l2png);
    }
    pc_buf_free(&out);
    /* large document: thumbnail is scaled to 256 on the long side */
    {
        pc_doc *big = pc_doc_create(1000, 300);
        pc_layer *l = pc_layer_create(big, "L");
        pc_doc *th;
        pc_doc_reserve_layers(big, 1);
        pc_doc_insert_layer(big, l, 0);
        CHECK(ora()->save(big, NULL, NULL, NULL, &out) == PC_OK);
        th = member_png(&out, "Thumbnails/thumbnail.png");
        CHECK(th && th->w == 256 && th->h == 77);
        pc_doc_destroy(th);
        r = NULL;
        CHECK(ora()->load(out.p, out.n, NULL, &r, &m2) == PC_OK);
        CHECK(r && r->n_layers == 1 && fabs(m2.dpi_x - 96.0) < 1e-9);
        pc_doc_destroy(r);
        pc_meta_free(&m2);
        pc_buf_free(&out);
        pc_doc_destroy(big);
    }
    free(bg); free(mid); free(top);
    pc_doc_destroy(d);
}

static void t_all_modes(void)
{
    const uint32_t W = 20, H = 20;
    pc_px32 *a = tu_noise(W, H, 1);
    pc_doc *d = tu_doc_from_px(W, H, a), *r = NULL;
    pc_buf out;
    pc_image_meta m;
    for (int k = 1; k < PC_BLEND_COUNT; k++)
        tu_doc_add_layer(d, a, (pc_blend_mode)k, (uint8_t)(k * 18), (k & 1) != 0, "L");
    memset(&out, 0, sizeof out);
    CHECK(ora()->save(d, NULL, NULL, NULL, &out) == PC_OK);
    CHECK(ora()->load(out.p, out.n, NULL, &r, &m) == PC_OK);
    if (r) {
        CHECK(r->n_layers == (uint32_t)PC_BLEND_COUNT);
        for (uint32_t i = 0; i < r->n_layers && i < d->n_layers; i++) {
            CHECK(r->stack[i]->mode == d->stack[i]->mode);
            CHECK(r->stack[i]->opacity == d->stack[i]->opacity);
            CHECK(r->stack[i]->visible == d->stack[i]->visible);
        }
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    pc_buf_free(&out);
    /* every opacity value survives the 3-decimal text form */
    for (uint32_t op = 0; op < 256; op++) {
        if (op % 17 != 0 && op != 254 && op != 1 && op != 128 && op != 127) continue;
        d->stack[1]->opacity = (uint8_t)op;
        memset(&out, 0, sizeof out);
        CHECK(ora()->save(d, NULL, NULL, NULL, &out) == PC_OK);
        r = NULL;
        CHECK(ora()->load(out.p, out.n, NULL, &r, &m) == PC_OK);
        CHECK(r && r->stack[1]->opacity == op);
        pc_doc_destroy(r);
        pc_meta_free(&m);
        pc_buf_free(&out);
    }
    free(a);
    pc_doc_destroy(d);
}

/* ---- hand-written archives --------------------------------------------------------------- */
static pc_buf g_png_red;      /* 4x3 opaque red */

static void make_red_png(void)
{
    pc_px32 px[12];
    pc_doc *d;
    for (int i = 0; i < 12; i++) px[i] = tu_px(255, 0, 0, 255);
    d = tu_doc_from_px(4, 3, px);
    memset(&g_png_red, 0, sizeof g_png_red);
    CHECK(png()->save(d, NULL, NULL, NULL, &g_png_red) == PC_OK);
    pc_doc_destroy(d);
}

static void make_ora(pc_buf *out, const char *mime, const char *xml, bool with_png)
{
    pc_zipw w;
    memset(out, 0, sizeof *out);
    pc_zipw_init(&w, out);
    if (mime) pc_zipw_add(&w, "mimetype", mime, strlen(mime), false);
    if (xml) pc_zipw_add(&w, "stack.xml", xml, strlen(xml), true);
    if (with_png) {
        pc_zipw_add(&w, "a.png", g_png_red.p, g_png_red.n, false);
        pc_zipw_add(&w, "v.svg", "<svg/>", 6, false);
    }
    pc_zipw_finish(&w);
    pc_zipw_free(&w);
}

static pc_status load_xml(const char *xml, pc_doc **d, pc_image_meta *m)
{
    pc_buf b;
    pc_status st;
    make_ora(&b, "image/openraster", xml, true);
    *d = NULL;
    st = ora()->load(b.p, b.n, NULL, d, m);
    pc_buf_free(&b);
    return st;
}

static void t_handwritten(void)
{
    pc_doc *d;
    pc_image_meta m;
    /* nested stacks, entities, unknown op, offsets partly outside, svg source */
    const char *xml =
        "<?xml version=\"1.0\"?>\n<!-- comment -->\n"
        "<image version=\"0.0.3\" w=\"10\" h=\"8\" xres='150' yres=\"150\">\n"
        " <stack>\n"
        "  <layer name=\"A &amp; &lt;B&gt; &#x263A; &#65;\" src=\"a.png\" x=\"-2\" y=\"6\""
        " composite-op=\"svg:hard-light\" opacity=\"0.5\"/>\n"
        "  <stack opacity=\"0.5\" visibility=\"hidden\" x=\"3\" y=\"1\">\n"
        "   <layer name='inner' src='a.png' composite-op='svg:screen' opacity='1.0e0'></layer>\n"
        "  </stack>\n"
        "  <layer name=\"vector\" src=\"v.svg\"/>\n"
        "  <text>ignored</text>\n"
        " </stack>\n"
        "</image>\n";
    CHECK(load_xml(xml, &d, &m) == PC_OK);
    if (d) {
        CHECK(d->w == 10 && d->h == 8 && d->n_layers == 3);
        if (d->n_layers == 3) {
            const pc_layer *top = d->stack[2], *inner = d->stack[1], *vec = d->stack[0];
            CHECK(strcmp(top->name, "A & <B> \xe2\x98\xba A") == 0);
            CHECK(top->mode == PC_BLEND_NORMAL && top->opacity == 128 && top->visible);
            CHECK(inner->mode == PC_BLEND_SCREEN && inner->opacity == 128 && !inner->visible);
            CHECK(strcmp(vec->name, "vector") == 0);
            /* top: red at x -2..1, y 6..8 -> visible x 0..1, y 6..7 */
            CHECK(pc_layer_get_px(top, 0, 6).r == 255 && pc_layer_get_px(top, 1, 7).r == 255);
            CHECK(pc_layer_get_px(top, 2, 6).a == 0 && pc_layer_get_px(top, 0, 5).a == 0);
            /* inner: stack offset 3,1 */
            CHECK(pc_layer_get_px(inner, 3, 1).r == 255 && pc_layer_get_px(inner, 6, 3).r == 255);
            CHECK(pc_layer_get_px(inner, 2, 1).a == 0 && pc_layer_get_px(inner, 7, 1).a == 0);
            CHECK(pc_layer_get_px(vec, 0, 0).a == 0);
        }
        CHECK(fabs(m.dpi_x - 150) < 1e-9);
        CHECK(strstr(m.note, "not PNG") != NULL);
        CHECK(pc_doc_edge_padding_is_zero(d));
        pc_doc_destroy(d);
        pc_meta_free(&m);
    }
    /* empty stack: one empty layer */
    CHECK(load_xml("<image w='5' h='5'><stack/></image>", &d, &m) == PC_OK);
    CHECK(d && d->n_layers == 1);
    pc_doc_destroy(d);
    pc_meta_free(&m);
    /* malformed or outside the subset */
    {
        static const char *bad[] = {
            "<image w='5' h='5'><stack></image>",                          /* mismatched */
            "<image w='5' h='5'><stack>",                                  /* unterminated */
            "<!DOCTYPE x [<!ENTITY e 'boom'>]><image w='5' h='5'/>",       /* DTD */
            "<image w='5' h='5'><stack><layer name='&e;' src='a.png'/></stack></image>",
            "<image w='5' h='5'><![CDATA[x]]></image>",
            "<image w='5' h='5' w='6'/>",                                  /* duplicate attr */
            "<image w='5' h='5'/><image w='5' h='5'/>",                    /* two roots */
            "<image w='5' h='5'><stack><layer src='a.png' name='x/></stack></image>",
            "<image w='5'h='5'/>",                                         /* missing space */
            "<image w='0' h='5'/>",
            "<image w='abc' h='5'/>",
            "text<image w='5' h='5'/>",
            "<stack/>",                                                    /* root not image */
            "<image w='5' h='5'><stack><layer name='&#0;' src='a.png'/></stack></image>",
            "<image w='5' h='5'><stack><layer name='&#xD800;' src='a.png'/></stack></image>",
            "<image w='5' h='5'><stack><layer name='&#99999999;' src='a.png'/></stack></image>",
            "",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            pc_status st = load_xml(bad[i], &d, &m);
            CHECK(st == PC_ERR_FORMAT && d == NULL);
            if (st != PC_ERR_FORMAT) INFO("bad xml %zu accepted: %s", i, pc_status_str(st));
        }
    }
    /* limits */
    CHECK(load_xml("<image w='70000' h='5'/>", &d, &m) == PC_ERR_LIMIT);
    {
        pc_buf deep;
        memset(&deep, 0, sizeof deep);
        pc_buf_append(&deep, "<image w='5' h='5'>", 19);
        for (int i = 0; i < 70; i++) pc_buf_append(&deep, "<stack>", 7);
        for (int i = 0; i < 70; i++) pc_buf_append(&deep, "</stack>", 8);
        pc_buf_append(&deep, "</image>", 9);
        CHECK(load_xml((const char *)deep.p, &d, &m) == PC_ERR_LIMIT);
        pc_buf_free(&deep);
    }
    {
        pc_buf many;
        pc_codec_limits lim;
        pc_buf b;
        memset(&many, 0, sizeof many);
        pc_buf_append(&many, "<image w='5' h='5'><stack>", 26);
        for (int i = 0; i < 20; i++) pc_buf_append(&many, "<layer src='a.png'/>", 20);
        pc_buf_append(&many, "</stack></image>", 17);
        make_ora(&b, "image/openraster", (const char *)many.p, true);
        pc_codec_limits_default(&lim);
        lim.max_layers = 10;
        d = NULL;
        CHECK(ora()->load(b.p, b.n, &lim, &d, &m) == PC_ERR_LIMIT && d == NULL);
        lim.max_layers = 20;
        CHECK(ora()->load(b.p, b.n, &lim, &d, &m) == PC_OK && d && d->n_layers == 20);
        pc_doc_destroy(d);
        pc_meta_free(&m);
        pc_buf_free(&b);
        pc_buf_free(&many);
    }
    /* mimetype missing or wrong, stack.xml missing */
    {
        pc_buf b;
        make_ora(&b, NULL, "<image w='5' h='5'/>", false);
        CHECK(ora()->load(b.p, b.n, NULL, &d, &m) == PC_ERR_FORMAT);
        pc_buf_free(&b);
        make_ora(&b, "image/png", "<image w='5' h='5'/>", false);
        CHECK(ora()->load(b.p, b.n, NULL, &d, &m) == PC_ERR_FORMAT);
        pc_buf_free(&b);
        make_ora(&b, "image/openraster\n", NULL, false);
        CHECK(ora()->load(b.p, b.n, NULL, &d, &m) == PC_ERR_FORMAT);
        pc_buf_free(&b);
    }
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 1500u : 20000u, ok = 0;
    const uint32_t W = 24, H = 20;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    pc_buf out;
    const char *xml =
        "<?xml version='1.0'?><image w='10' h='8'><stack>"
        "<layer name='A &amp; B' src='a.png' x='-2' y='6' composite-op='svg:multiply' opacity='0.5'/>"
        "<stack opacity='0.5'><layer name='&#65;' src='a.png' visibility='hidden'/></stack>"
        "</stack></image>";
    size_t xn = strlen(xml);
    uint8_t *mx = (uint8_t *)malloc(xn + 65);
    tu_doc_add_layer(d, a, PC_BLEND_OVERLAY, 99, true, "x");
    memset(&out, 0, sizeof out);
    CHECK(ora()->save(d, NULL, NULL, NULL, &out) == PC_OK);
    ok += tu_fuzz_codec(ora(), out.p, out.n, iters, NULL);
    /* XML parser: mutate stack.xml, repack (valid zip) and load */
    for (uint32_t i = 0; i < iters; i++) {
        size_t n = tu_mutate((const uint8_t *)xml, xn, mx, xn + 64);
        pc_doc *r = NULL;
        pc_image_meta m;
        mx[n] = 0;
        if (strlen((const char *)mx) != n) continue;    /* embedded NUL: zip names fine, skip */
        if (load_xml((const char *)mx, &r, &m) == PC_OK) {
            ok++;
            CHECK(r && r->n_layers >= 1 && pc_doc_edge_padding_is_zero(r));
            pc_doc_destroy(r);
            pc_meta_free(&m);
        } else {
            CHECK(r == NULL);
        }
    }
    INFO("ora fuzz: %u loads succeeded", ok);
    free(mx);
    pc_buf_free(&out);
    pc_doc_destroy(d);
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    make_red_png();
    RUN(t_sniff);
    RUN(t_roundtrip);
    RUN(t_all_modes);
    RUN(t_handwritten);
    RUN(t_fuzz);
    pc_buf_free(&g_png_red);
    return pc_test_finish();
}
