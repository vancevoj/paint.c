/* test_text_color.c - color fonts in the text engine (lane TOOLB,
 * F-T-TEXT-COLORFONT): the vector renderer's image layer (pc_vimage), face
 * choice by emoji presentation, default ignorables, cluster ligatures,
 * COLR-style layered glyphs (palette and text colors), bitmap glyphs
 * (scaling, italic), blend / Overwrite / antialiasing / selection
 * clipping, the color image cache, huge glyphs, errors, and history
 * round trips without leaks. Synthetic faces, fixed data. */
#include "test_shapes_util.h"
#include "pc/pc_text.h"

/* ---- pc_vimage ------------------------------------------------------------------------------ */

typedef struct grad_img {
    pc_rect r;
} grad_img;

/* Inside r: a gradient color with alpha 0 at x % 16 == 0, 255 at
 * x % 16 == 15, (x % 16) * 16 between; outside transparent. */
static void grad_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const grad_img *g = (const grad_img *)ud;
    for (int32_t i = 0; i < n; i++) {
        int32_t px = x + i;
        if (px >= g->r.x && px < g->r.x + g->r.w && y >= g->r.y && y < g->r.y + g->r.h)
            out[i] = e3_px((uint8_t)(px * 7), (uint8_t)(y * 5), 90u,
                           px % 16 == 15 ? 255u : (uint8_t)((px % 16) * 16));
        else
            memset(&out[i], 0, sizeof out[i]);
    }
}

static void t_vimage(void)
{
    pc_px32 bg = e3_px(200, 180, 40, 255);
    e3_doc e = e3_doc_make(96, 80, bg);
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    grad_img gi;
    pc_vimage img;
    pc_txn *x;
    pc_surf a, b;
    pc_rect dirty;
    gi.r = pc_rect_make(10, 12, 50, 30);
    img.row = grad_row;
    img.ud = &gi;
    img.bounds = gi.r;
    CHECK(vr != NULL);
    /* BLEND: identical to applying the image as a source with full coverage */
    x = pc_txn_begin(e.d, "img");
    CHECK(pc_vrender_draw_image(vr, x, e.layer, NULL, 0, &img, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x <= 10 && dirty.y <= 12 && dirty.x + dirty.w >= 60 && dirty.y + dirty.h >= 42);
    a = e3_read(&e, x);
    pc_txn_cancel(x);
    pc_vrender_reset(vr);
    {
        pc_mask m;
        pc_paint_src src;
        pc_paint_opts po = pc_paint_opts_default();
        CHECK(pc_mask_alloc(&m, gi.r) == PC_OK);
        memset(m.px, 255, (size_t)m.w * (size_t)m.h);
        memset(&src, 0, sizeof src);
        src.row = grad_row;
        src.ud = &gi;
        x = pc_txn_begin(e.d, "ref");
        CHECK(pc_paint_apply(x, e.layer, &m, &src, &po, NULL, NULL) == PC_OK);
        b = e3_read(&e, x);
        pc_txn_cancel(x);
        pc_mask_free(&m);
    }
    CHECK(e3_same(&a, &b));
    pc_surf_free(&b);
    /* Overwrite: alpha 0 keeps the pixel, alpha 255 replaces it */
    o.paint.mode = PC_PAINT_OVERWRITE;
    x = pc_txn_begin(e.d, "img");
    CHECK(pc_vrender_draw_image(vr, x, e.layer, NULL, 0, &img, &o, NULL, NULL) == PC_OK);
    b = e3_read(&e, x);
    {
        pc_px32 p0 = e3_at(&b, 15, 20);          /* alpha 255: replaced */
        pc_px32 p1 = e3_at(&b, 30, 20);          /* alpha 224: mostly replaced */
        pc_px32 p2 = e3_at(&b, 5, 20);           /* outside: kept */
        size_t zero = 0;
        CHECK(e3_eq(p0, e3_px((uint8_t)(15 * 7), 100, 90, 255)));
        CHECK(p1.a == 255u && p1.b > 80u && p1.r > 170u);
        CHECK(e3_eq(p2, bg));
        for (int32_t xx = 10; xx < 60; xx++)
            if (xx % 16 == 0) {
                CHECK(e3_eq(e3_at(&b, xx, 20), bg));     /* alpha 0: untouched */
                zero++;
            }
        CHECK(zero == 3u);
    }
    pc_surf_free(&b);
    pc_txn_cancel(x);
    pc_vrender_reset(vr);
    /* an image above a vector layer: the over composite of both */
    o = pc_vdraw_opts_default();
    {
        pc_poly sq;
        pc_vlayer l;
        pc_paint_src blue = e3_solid(e3_px(0, 0, 255, 255));
        pc_poly_init(&sq);
        CHECK(pc_poly_add(&sq, pc_pt_make(0, 0), 0u) == PC_OK);
        CHECK(pc_poly_add(&sq, pc_pt_make(96, 0), 0u) == PC_OK);
        CHECK(pc_poly_add(&sq, pc_pt_make(96, 80), 0u) == PC_OK);
        CHECK(pc_poly_add(&sq, pc_pt_make(0, 80), 0u) == PC_OK);
        CHECK(pc_poly_end(&sq, true) == PC_OK);
        l.fill = &sq;
        l.rule = PC_FILL_NONZERO;
        l.thin = NULL;
        l.src = &blue;
        x = pc_txn_begin(e.d, "img");
        CHECK(pc_vrender_draw_image(vr, x, e.layer, &l, 1, &img, &o, NULL, NULL) == PC_OK);
        b = e3_read(&e, x);
        CHECK(e3_eq(e3_at(&b, 5, 5), e3_px(0, 0, 255, 255)));
        CHECK(e3_eq(e3_at(&b, 15, 20), e3_px((uint8_t)(15 * 7), 100, 90, 255)));
        {
            pc_px32 p = e3_at(&b, 20, 20);       /* alpha 64 over blue */
            CHECK(p.a == 255u && p.b > 150u && p.r > 25u && p.r < 45u);
        }
        pc_surf_free(&b);
        pc_txn_cancel(x);
        pc_vrender_reset(vr);
        pc_poly_free(&sq);
    }
    /* errors */
    x = pc_txn_begin(e.d, "img");
    CHECK(pc_vrender_draw_image(vr, x, e.layer, NULL, 0, NULL, &o, NULL, NULL) == PC_ERR_ARG);
    img.row = NULL;
    CHECK(pc_vrender_draw_image(vr, x, e.layer, NULL, 0, &img, &o, NULL, NULL) == PC_ERR_ARG);
    img.row = grad_row;
    CHECK(pc_vrender_draw_image(vr, x, 999u, NULL, 0, &img, &o, NULL, NULL) == PC_ERR_ARG);
    /* empty bounds: nothing painted */
    img.bounds = pc_rect_make(0, 0, 0, 0);
    CHECK(pc_vrender_draw_image(vr, x, e.layer, NULL, 0, &img, &o, NULL, &dirty) == PC_OK);
    CHECK(pc_rect_is_empty(pc_vrender_painted(vr)));
    pc_txn_cancel(x);
    pc_surf_free(&a);
    pc_vrender_destroy(vr);
    e3_doc_free(&e);
}

/* ---- synthetic faces ------------------------------------------------------------------------ */

/* Face M (primary, monochrome): ASCII boxes 0.6 em wide. */
static uint32_t m_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    return cp >= 0x20u && cp <= 0x7Eu ? cp : 0u;
}

static void m_metrics(void *ud, double em, pc_font_metrics *m)
{
    (void)ud;
    memset(m, 0, sizeof *m);
    m->ascent = 0.8 * em;
    m->descent = 0.2 * em;
}

static double m_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    (void)ud;
    (void)mode;
    return gid == ' ' ? 0.3 * em : 0.6 * em;
}

static pc_status rect_px(pc_path *p, double x0, double y0, double x1, double y1)
{
    pc_status st = pc_path_move_to(p, x0, y0);
    if (st == PC_OK) st = pc_path_line_to(p, x1, y0);
    if (st == PC_OK) st = pc_path_line_to(p, x1, y1);
    if (st == PC_OK) st = pc_path_line_to(p, x0, y1);
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

static pc_status m_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    (void)ud;
    (void)mode;
    if (gid == ' ' || gid == 0u) return PC_OK;
    return rect_px(out, 0.1 * em, -0.7 * em, 0.5 * em, 0.0);
}

/* Face D (fallback, monochrome): U+2764 and U+1F600 as solid boxes. */
static uint32_t d_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    return cp == 0x2764u ? 1u : (cp == 0x1F600u ? 2u : 0u);
}

/* Face C (fallback, color). Glyphs:
 *  10 U+1F600 layers: 20 (box 0..1 em, red) then 21 (inner box, text color)
 *  11 U+1F601 bitmap 4 x 4 at scale 1 em/16... (see c_bitmap)
 *  12 U+2764  layers: 20 in blue
 *  13 U+200D  zero advance, no outline
 *  14 U+1F602 bitmap with a bad strike (error test)
 *  30 ligature of 10 13 11: bitmap
 *  40 / 41 U+1F1E6 / U+1F1E7, ligature 42 (layers: 20 in green) */
typedef struct cface {
    int    bitmap_calls;
    int    layer_calls;
    bool   fail;
    pc_px32 px[16 * 16];
} cface;

static uint32_t c_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    switch (cp) {
    case 0x1F600u: return 10u;
    case 0x1F601u: return 11u;
    case 0x2764u: return 12u;
    case 0x200Du: return 13u;
    case 0x1F602u: return 14u;
    case 0x1F1E6u: return 40u;
    case 0x1F1E7u: return 41u;
    default: return 0u;
    }
}

static double c_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    (void)ud;
    (void)mode;
    return gid == 13u ? 0.0 : em;
}

static pc_status c_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    (void)ud;
    (void)mode;
    if (gid == 20u) return rect_px(out, 0.0, -0.8 * em, em, 0.2 * em);
    if (gid == 21u) return rect_px(out, 0.25 * em, -0.55 * em, 0.75 * em, -0.05 * em);
    if (gid == 10u || gid == 12u) return rect_px(out, 0.4 * em, -0.4 * em, 0.6 * em, -0.2 * em);
    return PC_OK;
}

static size_t c_layers(void *ud, uint32_t gid, pc_font_color_layer *out, size_t cap)
{
    cface *f = (cface *)ud;
    pc_font_color_layer l[2];
    size_t n = 0;
    memset(l, 0, sizeof l);
    f->layer_calls++;
    if (gid == 10u) {
        l[0].gid = 20u;
        l[0].color = e3_px(255, 0, 0, 255);
        l[1].gid = 21u;
        l[1].foreground = true;
        n = 2u;
    } else if (gid == 12u || gid == 42u) {
        l[0].gid = 20u;
        l[0].color = gid == 12u ? e3_px(0, 0, 255, 255) : e3_px(0, 255, 0, 255);
        n = 1u;
    }
    for (size_t i = 0; i < n && i < cap; i++) out[i] = l[i];
    return n;
}

/* Bitmaps: 16 x 16 at scale em / 16 (one bitmap pixel per document pixel
 * at em 16): left half red, right half blue, top-left pixel transparent;
 * the ligature (30) is a 16 x 16 gray square. Placed at left 0, top -0.75
 * em. */
static pc_status c_bitmap(void *ud, uint32_t gid, double em, pc_font_bitmap *out)
{
    cface *f = (cface *)ud;
    f->bitmap_calls++;
    if (gid == 14u) return f->fail ? PC_ERR_FORMAT : PC_ERR_UNSUPPORTED;
    if (gid != 11u && gid != 30u) return PC_ERR_UNSUPPORTED;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            pc_px32 p = x < 8 ? e3_px(255, 0, 0, 255) : e3_px(0, 0, 255, 255);
            if (gid == 30u) p = e3_px(128, 128, 128, 255);
            if (x == 0 && y == 0) memset(&p, 0, sizeof p);
            f->px[y * 16 + x] = p;
        }
    out->px = f->px;
    out->w = 16;
    out->h = 16;
    out->scale = em / 16.0;
    out->left = 0.0;
    out->top = -0.75 * em;
    return PC_OK;
}

static size_t c_subst(void *ud, uint32_t *g, size_t n)
{
    (void)ud;
    if (n == 3u && g[0] == 10u && g[1] == 13u && g[2] == 11u) {
        g[0] = 30u;
        return 1u;
    }
    if (n == 2u && g[0] == 40u && g[1] == 41u) {
        g[0] = 42u;
        return 1u;
    }
    return n;
}

static cface g_cf;
static pc_font_face g_m, g_d, g_c;

static void init_faces(void)
{
    memset(&g_m, 0, sizeof g_m);
    g_m.glyph = m_glyph;
    g_m.metrics = m_metrics;
    g_m.advance = m_advance;
    g_m.outline = m_outline;
    g_d = g_m;
    g_d.glyph = d_glyph;
    memset(&g_c, 0, sizeof g_c);
    g_c.ud = &g_cf;
    g_c.glyph = c_glyph;
    g_c.advance = c_advance;
    g_c.outline = c_outline;
    g_c.color = true;
    g_c.color_layers = c_layers;
    g_c.color_bitmap = c_bitmap;
    g_c.substitute = c_subst;
}

/* em 16 px (12 pt), baseline anchor at (8, 40), no snapping. */
static pc_text *make(const char *s)
{
    const pc_font_face *faces[3] = { &g_m, &g_d, &g_c };
    pc_text *t = pc_text_create();
    pc_text_style st;
    if (!t) return NULL;
    pc_text_style_default(&st);
    st.anchor = PC_TEXT_ANCHOR_BASELINE;
    st.snap = false;
    st.size = 12.0;
    CHECK(pc_text_set_fonts(t, faces, 3) == PC_OK);
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(8.0, 40.0));
    CHECK(pc_text_set_utf8(t, s, strlen(s)) == PC_OK);
    return t;
}

/* ---- layout: presentation, ignorables, ligatures --------------------------------------------- */

static void t_presentation(void)
{
    const pc_text_glyph *g;
    size_t n;
    pc_text *t;
    /* U+1F600 has emoji presentation: the color face even though D comes
     * first; U+2764 is text by default (D), emoji with U+FE0F (C, which
     * lacks U+FE0F: hidden, no advance) */
    t = make("A\xf0\x9f\x98\x80\xe2\x9d\xa4\xe2\x9d\xa4\xef\xb8\x8f");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 5u);
    CHECK(g[0].face == 0u && g[1].face == 2u && g[1].gid == 10u);
    CHECK(g[2].face == 1u && g[2].gid == 1u);
    CHECK(g[3].face == 2u && g[3].gid == 12u && !g[3].hidden);
    CHECK(g[4].hidden && g[4].advance == 0.0 && !g[4].cluster_start && g[4].cp == 0xFE0Fu);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].width, 9.6 + 16.0 + 9.6 + 16.0, 1e-9));
    CHECK(pc_text_color_glyph_count(t) == 2u);
    pc_text_destroy(t);
    /* U+FE0E asks for text: the monochrome face */
    t = make("\xf0\x9f\x98\x80\xef\xb8\x8e");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 2u && g[0].face == 1u && g[1].hidden);
    CHECK(pc_text_color_glyph_count(t) == 0u);
    pc_text_destroy(t);
    /* a primary face with monochrome emoji (D first): emoji presentation
     * still comes from the color face, text presentation from D */
    {
        const pc_font_face *faces[3] = { &g_d, &g_m, &g_c };
        static const char s[] = "\xf0\x9f\x98\x80\xe2\x9d\xa4\xe2\x9d\xa4\xef\xb8\x8f";
        t = make("");
        CHECK(pc_text_set_fonts(t, faces, 3) == PC_OK);
        CHECK(pc_text_set_utf8(t, s, sizeof s - 1u) == PC_OK);
        g = pc_text_glyphs(t, &n);
        CHECK(n == 4u && g[0].face == 2u && g[1].face == 0u && g[2].face == 2u && g[3].hidden);
        pc_text_destroy(t);
    }
    /* an ignorable nobody has, alone: no box, no width (ZWSP) */
    t = make("A\xe2\x80\x8b" "B");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 3u && g[1].hidden && g[1].cluster_start && e3_near(g[2].x - g[0].x, 9.6, 1e-9));
    pc_text_destroy(t);
}

static void t_ligatures(void)
{
    const pc_text_glyph *g;
    size_t n;
    pc_box b;
    /* U+1F600 ZWJ U+1F601: one cluster, one ligature glyph */
    pc_text *t = make("\xf0\x9f\x98\x80\xe2\x80\x8d\xf0\x9f\x98\x81!");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 4u);
    CHECK(g[0].gid == 30u && g[0].face == 2u && !g[0].hidden && g[0].cluster_start);
    CHECK(g[1].hidden && g[2].hidden && g[1].advance == 0.0 && g[2].advance == 0.0);
    CHECK(g[3].face == 0u && e3_near(g[3].x - g[0].x, 16.0, 1e-9));
    pc_text_caret_box(t, 11, &b);                       /* after the cluster */
    CHECK(e3_near(b.x0, g[3].x, 1e-9));
    pc_text_set_caret(t, 0, false);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 11u);
    CHECK(pc_text_color_glyph_count(t) == 1u);
    pc_text_destroy(t);
    /* a regional indicator pair */
    t = make("\xf0\x9f\x87\xa6\xf0\x9f\x87\xa7");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 2u && g[0].gid == 42u && g[1].hidden);
    pc_text_destroy(t);
    /* no ligature for this sequence: both glyphs stay */
    t = make("\xf0\x9f\x98\x81\xe2\x80\x8d\xf0\x9f\x98\x80");
    g = pc_text_glyphs(t, &n);
    CHECK(n == 3u && g[0].gid == 11u && g[2].gid == 10u && !g[2].hidden);
    CHECK(e3_near(g[2].x - g[0].x, 16.0, 1e-9));
    pc_text_destroy(t);
}

/* ---- rendering ------------------------------------------------------------------------------- */

static pc_surf render(pc_text *t, e3_doc *e, const pc_vdraw_opts *o, pc_px32 ink, pc_status *st)
{
    pc_vrender *vr = pc_vrender_create();
    pc_paint_src src = e3_solid(ink);
    pc_txn *x = pc_txn_begin(e->d, "Text");
    pc_surf s;
    *st = pc_text_render(t, vr, x, e->layer, &src, o, NULL, NULL);
    s = e3_read(e, x);
    pc_txn_cancel(x);
    pc_vrender_destroy(vr);
    return s;
}

static void t_render_layers(void)
{
    e3_doc e = e3_doc_make(80, 60, e3_px(0, 0, 0, 0));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 ink = e3_px(0, 200, 0, 255);
    pc_text *t = make("\xf0\x9f\x98\x80");            /* pen (8, 40), em 16 */
    pc_status st;
    pc_surf s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    /* layer 20: x 8..24, y 27.2..43.2 red; layer 21 (text color): x 12..20,
     * y 31.2..39.2 */
    CHECK(e3_eq(e3_at(&s, 9, 30), e3_px(255, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&s, 15, 35), ink));
    CHECK(e3_at(&s, 7, 35).a == 0u && e3_at(&s, 25, 35).a == 0u);
    CHECK(e3_at(&s, 9, 27).a > 0u && e3_at(&s, 9, 27).a < 255u);   /* 0.8 coverage */
    {
        pc_px32 p = e3_at(&s, 9, 27);
        CHECK(p.r == 255u && p.g == 0u && e3_near((double)p.a, 0.8 * 255.0, 1.0));
    }
    pc_surf_free(&s);
    /* the text color follows the source; antialiasing off gives hard edges */
    o.antialias = false;
    s = render(t, &e, &o, e3_px(10, 20, 30, 255), &st);
    CHECK(st == PC_OK && e3_eq(e3_at(&s, 15, 35), e3_px(10, 20, 30, 255)));
    for (int32_t y = 20; y < 50; y++)
        for (int32_t x = 0; x < 40; x++)
            CHECK(e3_at(&s, x, y).a == 0u || e3_at(&s, x, y).a == 255u);
    pc_surf_free(&s);
    pc_text_destroy(t);
    e3_doc_free(&e);
}

static void t_render_bitmap(void)
{
    e3_doc e = e3_doc_make(120, 80, e3_px(255, 255, 255, 255));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 ink = e3_px(0, 0, 0, 255);
    pc_text *t = make("x\xf0\x9f\x98\x81");           /* bitmap pen at (17.6, 40) */
    pc_text_style stl;
    pc_status st;
    pc_surf s;
    int calls;
    /* fractional pen: quarter-pixel images; at em 16 one bitmap pixel per
     * document pixel; the left half red, the right half blue */
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    CHECK(e3_eq(e3_at(&s, 20, 35), e3_px(255, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&s, 30, 35), e3_px(0, 0, 255, 255)));
    CHECK(e3_eq(e3_at(&s, 40, 35), e3_px(255, 255, 255, 255)));
    /* mono glyph 'x' in the primary color */
    CHECK(e3_eq(e3_at(&s, 12, 35), ink));
    pc_surf_free(&s);
    /* the image cache: rendering again makes no new image */
    calls = g_cf.bitmap_calls;
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK && g_cf.bitmap_calls == calls);
    pc_surf_free(&s);
    /* Overwrite: the transparent top-left bitmap pixel keeps the canvas */
    pc_text_set_origin(t, pc_pt_make(8.4, 40.0));       /* bitmap at x 18.0 */
    o.paint.mode = PC_PAINT_OVERWRITE;
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    CHECK(e3_eq(e3_at(&s, 18, 28), e3_px(255, 255, 255, 255)));
    CHECK(e3_eq(e3_at(&s, 19, 28), e3_px(255, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&s, 33, 28), e3_px(0, 0, 255, 255)));
    pc_surf_free(&s);
    o.paint.mode = PC_PAINT_BLEND;
    /* shrinking (em 8): every document pixel averages 2 x 2 bitmap pixels */
    stl = *pc_text_get_style(t);
    stl.size = 6.0;
    CHECK(pc_text_set_style(t, &stl) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(8.2, 40.0));        /* x 4.8 + 8.2 = 13.0 */
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    CHECK(e3_eq(e3_at(&s, 15, 36), e3_px(255, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&s, 19, 36), e3_px(0, 0, 255, 255)));
    {
        pc_px32 p = e3_at(&s, 13, 34);                     /* 3 of 4 opaque */
        CHECK(p.r == 255u && p.g > 50u && p.g < 80u);      /* red over white at 75% */
    }
    pc_surf_free(&s);
    /* synthetic italic shears the bitmap: the red / blue boundary moves
     * right towards the top */
    stl.size = 12.0;
    stl.italic = true;
    CHECK(pc_text_set_style(t, &stl) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(8.0, 40.0));
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    {
        int32_t edge_top = -1, edge_low = -1;
        for (int32_t x = 18; x < 50; x++) {
            if (edge_top < 0 && e3_at(&s, x, 30).b > 200u && e3_at(&s, x, 30).r < 60u)
                edge_top = x;
            if (edge_low < 0 && e3_at(&s, x, 40).b > 200u && e3_at(&s, x, 40).r < 60u)
                edge_low = x;
        }
        CHECK(edge_top > 0 && edge_low > 0 && edge_top >= edge_low + 1);
    }
    pc_surf_free(&s);
    pc_text_destroy(t);
    e3_doc_free(&e);
}

static void t_render_mixed(void)
{
    e3_doc e = e3_doc_make(160, 80, e3_px(0, 0, 0, 0));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 ink = e3_px(0, 0, 0, 255);
    pc_text *t = make("A\xf0\x9f\x98\x80\xe2\x80\x8d\xf0\x9f\x98\x81"
                      "B\xf0\x9f\x87\xa6\xf0\x9f\x87\xa7");
    pc_text_style stl = *pc_text_get_style(t);
    pc_status st;
    pc_surf s;
    /* underline under color glyphs; the ligature is gray, the flag green */
    stl.underline = true;
    CHECK(pc_text_set_style(t, &stl) == PC_OK);
    s = render(t, &e, &o, ink, &st);
    CHECK(st == PC_OK);
    CHECK(e3_eq(e3_at(&s, 22, 35), e3_px(128, 128, 128, 255)));    /* x 17.6..33.6 */
    CHECK(e3_eq(e3_at(&s, 50, 35), e3_px(0, 255, 0, 255)));        /* flag x 43.2..59.2 */
    {
        /* the bitmap ends at y 40 + 0.25 em = 44: the underline below it
         * (y 41.6) is ink, drawn under the image */
        pc_px32 p = e3_at(&s, 22, 41);
        CHECK(p.a == 255u && p.r == 128u);
        CHECK(e3_at(&s, 40, 41).a > 200u && e3_at(&s, 40, 41).r == 0u);
    }
    pc_surf_free(&s);
    /* selection clipping: outside the selection nothing changes */
    {
        pc_poly sel;
        pc_poly_init(&sel);
        CHECK(pc_poly_add(&sel, pc_pt_make(0, 0), 0u) == PC_OK);
        CHECK(pc_poly_add(&sel, pc_pt_make(40, 0), 0u) == PC_OK);
        CHECK(pc_poly_add(&sel, pc_pt_make(40, 80), 0u) == PC_OK);
        CHECK(pc_poly_add(&sel, pc_pt_make(0, 80), 0u) == PC_OK);
        CHECK(pc_poly_end(&sel, true) == PC_OK);
        CHECK(pc_sel_apply_poly(e.h, &sel, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "sel") == PC_OK);
        s = render(t, &e, &o, ink, &st);
        CHECK(st == PC_OK);
        CHECK(e3_at(&s, 22, 35).a == 255u && e3_at(&s, 50, 35).a == 0u);
        pc_surf_free(&s);
        pc_poly_free(&sel);
        CHECK(pc_hist_undo(e.h));
    }
    pc_text_destroy(t);
    e3_doc_free(&e);
}

/* Huge glyphs render at a reduced image resolution; errors propagate;
 * commit and undo restore the document. */
static void t_huge_errors_history(void)
{
    e3_doc e = e3_doc_make(2400, 2400, e3_px(0, 0, 0, 0));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_text *t = make("\xf0\x9f\x98\x81");
    pc_text_style stl = *pc_text_get_style(t);
    pc_vrender *vr = pc_vrender_create();
    pc_paint_src src = e3_solid(e3_px(0, 0, 0, 255));
    uint64_t f0 = pc_doc_fingerprint(e.d), f1;
    pc_txn *x;
    pc_status st;
    stl.size = 1650.0;                                  /* em 2200 px: 4.8 Mpx image */
    CHECK(pc_text_set_style(t, &stl) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(10.0, 1700.0));
    x = pc_txn_begin(e.d, "Text");
    CHECK(pc_text_render(t, vr, x, e.layer, &src, &o, NULL, NULL) == PC_OK);
    {
        pc_px32 p;
        CHECK(pc_txn_read_rect(x, e.layer, pc_rect_make(300, 900, 1, 1), &p, 1) == PC_OK);
        CHECK(e3_eq(p, e3_px(255, 0, 0, 255)));
        CHECK(pc_txn_read_rect(x, e.layer, pc_rect_make(1900, 900, 1, 1), &p, 1) == PC_OK);
        CHECK(e3_eq(p, e3_px(0, 0, 255, 255)));
    }
    CHECK(pc_txn_commit(x, e.h) == PC_OK);
    pc_vrender_reset(vr);
    f1 = pc_doc_fingerprint(e.d);
    CHECK(f1 != f0);
    CHECK(pc_hist_undo(e.h) && pc_doc_fingerprint(e.d) == f0);
    CHECK(pc_hist_redo(e.h) && pc_doc_fingerprint(e.d) == f1);
    /* a backend error aborts the render */
    g_cf.fail = true;
    CHECK(pc_text_set_utf8(t, "\xf0\x9f\x98\x82", 4) == PC_OK);
    x = pc_txn_begin(e.d, "Text");
    st = pc_text_render(t, vr, x, e.layer, &src, &o, NULL, NULL);
    CHECK(st == PC_ERR_FORMAT);
    {
        pc_poly p;
        pc_poly_init(&p);
        CHECK(pc_text_build(t, &p) == PC_ERR_FORMAT);
        pc_poly_free(&p);
    }
    g_cf.fail = false;
    pc_txn_cancel(x);
    pc_vrender_destroy(vr);
    pc_text_destroy(t);
    e3_doc_free(&e);
}

/* ---- extreme but valid backend answers ------------------------------------------------------- */

static int g_mode;
static pc_px32 g_px4[4];

static size_t x_layers(void *ud, uint32_t gid, pc_font_color_layer *out, size_t cap)
{
    (void)ud;
    if (gid != 10u || g_mode != 0) return 0u;
    /* more layers than the engine draws, glyphs the face does not have */
    for (size_t i = 0; i < cap; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].gid = i % 3u == 0u ? 20u : 0xFFFFFFu;
        out[i].color = e3_px(9, 9, 9, 255);
    }
    return PC_TEXT_MAX_COLOR_LAYERS + 5u;
}

static pc_status x_bitmap(void *ud, uint32_t gid, double em, pc_font_bitmap *out)
{
    (void)ud;
    (void)em;
    if (gid != 11u) return PC_ERR_UNSUPPORTED;
    for (int i = 0; i < 4; i++) g_px4[i] = e3_px(200, 10, 10, 255);
    out->px = g_px4;
    out->w = 2;
    out->h = 2;
    out->left = 0.0;
    out->top = -10.0;
    switch (g_mode) {
    case 1: out->scale = 1e-4; break;                      /* vanishing */
    case 2: out->scale = 1500.0; break;                    /* 3000 px image */
    case 3: out->scale = 1.0; out->left = 1e6; break;      /* far outside */
    case 4: out->scale = (double)NAN; break;              /* NaN: rejected */
    default: out->scale = 4.0; break;
    }
    return PC_OK;
}

static void t_extreme_backend(void)
{
    pc_font_face xf = g_c;
    const pc_font_face *faces[2] = { &g_m, &xf };
    e3_doc e = e3_doc_make(400, 300, e3_px(0, 0, 0, 0));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src src = e3_solid(e3_px(0, 0, 0, 255));
    xf.color_layers = x_layers;
    xf.color_bitmap = x_bitmap;
    for (g_mode = 0; g_mode < 5; g_mode++) {
        pc_text *t = pc_text_create();
        pc_vrender *vr = pc_vrender_create();
        pc_text_style st;
        pc_txn *x;
        pc_status rs;
        pc_text_style_default(&st);
        st.anchor = PC_TEXT_ANCHOR_BASELINE;
        CHECK(t && vr && pc_text_set_fonts(t, faces, 2) == PC_OK);
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        pc_text_set_origin(t, pc_pt_make(20, 200));
        CHECK(pc_text_set_utf8(t, "\xf0\x9f\x98\x80\xf0\x9f\x98\x81", 8) == PC_OK);
        x = pc_txn_begin(e.d, "Text");
        rs = pc_text_render(t, vr, x, e.layer, &src, &o, NULL, NULL);
        CHECK(rs == PC_OK);
        INFO("mode %d: %s", g_mode, pc_status_str(rs));
        if (g_mode == 2) {
            pc_px32 p;
            CHECK(pc_txn_read_rect(x, e.layer, pc_rect_make(200, 250, 1, 1), &p, 1) == PC_OK);
            CHECK(p.r == 200u && p.a == 255u);              /* the upscaled bitmap */
        }
        pc_txn_cancel(x);
        pc_vrender_destroy(vr);
        pc_text_destroy(t);
    }
    e3_doc_free(&e);
}

/* Random edits with color glyphs keep the layout invariants. */
static void t_random(void)
{
    static const char *const pieces[] = {
        "a", " ", "\n", "\xf0\x9f\x98\x80", "\xf0\x9f\x98\x81", "\xe2\x80\x8d", "\xef\xb8\x8f",
        "\xef\xb8\x8e", "\xe2\x9d\xa4", "\xf0\x9f\x87\xa6", "\xf0\x9f\x87\xa7", "\xe2\x80\x8b"
    };
    e3_doc e = e3_doc_make(200, 200, e3_px(0, 0, 0, 0));
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_text *t = make("");
    pc_vrender *vr = pc_vrender_create();
    pc_paint_src src = e3_solid(e3_px(0, 0, 0, 255));
    pc_txn *x = pc_txn_begin(e.d, "Text");
    int iters = g_quick ? 200 : 2000;
    for (int it = 0; it < iters; it++) {
        const char *p = pieces[rndu((uint32_t)(sizeof pieces / sizeof pieces[0]))];
        const pc_text_glyph *g;
        size_t n, len, cps = 0;
        const char *s;
        if (rndu(5) == 0u) CHECK(pc_text_backspace(t, false) == PC_OK);
        else CHECK(pc_text_insert(t, p, strlen(p)) == PC_OK);
        s = pc_text_utf8(t, &len);
        for (size_t i = 0; i < len; i++) cps += ((unsigned char)s[i] & 0xC0u) != 0x80u;
        g = pc_text_glyphs(t, &n);
        {
            size_t nl = 0;
            for (size_t i = 0; i < len; i++) nl += s[i] == '\n';
            CHECK(n == cps - nl);
        }
        for (size_t i = 0; i < n; i++) {
            CHECK(!g[i].hidden || g[i].advance == 0.0);
            CHECK(g[i].face < 3u);
        }
        if (it % 16 == 0) CHECK(pc_text_render(t, vr, x, e.layer, &src, &o, NULL, NULL) == PC_OK);
    }
    (void)rnd8();
    pc_txn_cancel(x);
    pc_vrender_destroy(vr);
    pc_text_destroy(t);
    e3_doc_free(&e);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    init_faces();
    pc_tile_stats(&t0, &b0);
    RUN(t_vimage);
    RUN(t_presentation);
    RUN(t_ligatures);
    RUN(t_render_layers);
    RUN(t_render_bitmap);
    RUN(t_render_mixed);
    RUN(t_huge_errors_history);
    RUN(t_extreme_backend);
    RUN(t_random);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
