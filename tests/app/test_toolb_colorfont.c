/* test_toolb_colorfont.c - lane TOOLB, F-T-TEXT-COLORFONT: color fonts in
 * the Text tool. Synthetic fonts (toolb_test_fonts.h) exercise the table
 * reader (COLR / CPAL, CBLC / CBDT index formats 1..5 and image formats
 * 17..19, sbix with 'dupe', GSUB ligatures through plain and extension
 * lookups, cmap formats 4 and 12), a fuzz pass over damaged copies, the
 * font scan's color flag and cache, the pc_font_face backend (bitmap
 * strikes decoded with the PNG codec, fonts without outlines, emoji
 * fallback by presentation), and the Text tool itself: color layers with
 * the primary color as the text color, ligatures from typed ZWJ
 * sequences, live color changes, commit and undo. When a real color emoji
 * font is installed (Noto Color Emoji, Segoe UI Emoji, Apple Color Emoji)
 * a smoke test renders emoji, a skin tone sequence and a flag with it. */
#include "pc_test.h"
#include "app_test_util.h"

#include "pc/pc_text.h"
#include "tools/text_font.h"
#include "tools/text_sfnt.h"
#include "tools/text_tool.h"
#include "toolb_test_fonts.h"

#include <math.h>

static pc_buf g_colr, g_bits, g_sbix;

static void t_sfnt_parse(void)
{
    text_sfnt s;
    pc_font_color_layer l[4];
    text_sfnt_image im, im2;
    uint32_t g[4];
    /* COLR + CPAL + GSUB */
    CHECK(text_sfnt_open(&s, g_colr.p, g_colr.n, 0) == PC_OK);
    CHECK(text_sfnt_has_outlines(&s) && text_sfnt_has_colr(&s) && !text_sfnt_has_bitmaps(&s));
    CHECK(s.upem == 1000u && s.num_glyphs == 10u && s.x_height == 500 && s.cap_height == 700);
    CHECK(s.ascent == 800 && s.descent == 200);
    CHECK(text_sfnt_cmap(&s, 'A') == 1u && text_sfnt_cmap(&s, 0x2B50u) == 2u);
    CHECK(text_sfnt_cmap(&s, 0x2764u) == 5u && text_sfnt_cmap(&s, 0x200Du) == 6u);
    CHECK(text_sfnt_cmap(&s, 'B') == 0u && text_sfnt_cmap(&s, 0x1F600u) == 0u);
    CHECK(text_sfnt_advance(&s, 2) == 1000u && text_sfnt_advance(&s, 6) == 0u);
    CHECK(text_sfnt_colr_layers(&s, 2, NULL, 0) == 2u);
    CHECK(text_sfnt_colr_layers(&s, 2, l, 4) == 2u);
    CHECK(l[0].gid == 3u && !l[0].foreground && l[0].color.r == 255u && l[0].color.a == 255u);
    CHECK(l[1].gid == 4u && l[1].foreground);
    CHECK(text_sfnt_colr_layers(&s, 5, l, 4) == 1u && l[0].color.b == 255u && l[0].color.a == 128u);
    CHECK(text_sfnt_colr_layers(&s, 1, l, 4) == 0u && text_sfnt_colr_layers(&s, 9, l, 4) == 0u);
    g[0] = 2; g[1] = 6; g[2] = 5;
    CHECK(text_sfnt_ligate(&s, g, 3) == 1u && g[0] == 7u);
    g[0] = 1; g[1] = 8;
    CHECK(text_sfnt_ligate(&s, g, 2) == 1u && g[0] == 9u);       /* through an extension */
    g[0] = 5; g[1] = 6; g[2] = 2;
    CHECK(text_sfnt_ligate(&s, g, 3) == 3u && g[0] == 5u && g[2] == 2u);
    CHECK(!text_sfnt_image_of(&s, 2, 16.0, &im));
    text_sfnt_close(&s);
    /* CBLC / CBDT without outlines */
    CHECK(text_sfnt_open(&s, g_bits.p, g_bits.n, 0) == PC_OK);
    CHECK(!text_sfnt_has_outlines(&s) && text_sfnt_has_bitmaps(&s) && !text_sfnt_has_colr(&s));
    CHECK(text_sfnt_cmap(&s, 0x1F600u) == 1u && text_sfnt_cmap(&s, 0x1F1E7u) == 4u);
    CHECK(text_sfnt_cmap(&s, 0x1F604u) == 0u && text_sfnt_cmap(&s, 'A') == 0u);
    CHECK(text_sfnt_image_of(&s, 1, 16.0, &im));                /* index 1, image 17 */
    CHECK(im.ppem == 16.0 && im.x == 1.0 && im.y == 7.0 && !im.bottom_origin);
    CHECK(strcmp(im.type, "png ") == 0 && im.len > 8u && memcmp(im.data, "\x89PNG", 4) == 0);
    CHECK(text_sfnt_image_of(&s, 1, 20.0, &im2) && im2.ppem == 32.0);   /* index 3 */
    CHECK(im2.x == 2.0 && im2.y == 14.0 && im2.data != im.data);
    CHECK(text_sfnt_image_of(&s, 1, 50.0, &im2) && im2.ppem == 32.0);
    CHECK(text_sfnt_image_of(&s, 2, 10.0, &im) && im.ppem == 16.0 && im.x == 1.0);  /* 2 / 19 */
    CHECK(memcmp(im.data, "\x89PNG", 4) == 0);
    CHECK(text_sfnt_image_of(&s, 2, 32.0, &im) && im.ppem == 32.0);
    CHECK(text_sfnt_image_of(&s, 5, 16.0, &im) && im.ppem == 16.0 && im.y == 7.0); /* 4 / 18 */
    CHECK(text_sfnt_image_of(&s, 5, 30.0, &im) && im.ppem == 32.0 && im.y == 14.0); /* 5 / 19 */
    CHECK(memcmp(im.data, "\x89PNG", 4) == 0);
    CHECK(!text_sfnt_image_of(&s, 6, 16.0, &im) && !text_sfnt_image_of(&s, 99, 16.0, &im));
    g[0] = 3; g[1] = 4;
    CHECK(text_sfnt_ligate(&s, g, 2) == 1u && g[0] == 5u);
    text_sfnt_close(&s);
    /* sbix */
    CHECK(text_sfnt_open(&s, g_sbix.p, g_sbix.n, 0) == PC_OK);
    CHECK(text_sfnt_has_outlines(&s) && text_sfnt_has_bitmaps(&s));
    CHECK(text_sfnt_image_of(&s, 1, 16.0, &im) && im.bottom_origin && im.x == 1.0 && im.y == -2.0);
    CHECK(text_sfnt_image_of(&s, 2, 100.0, &im2) && im2.data == im.data && im2.ppem == 16.0);
    CHECK(!text_sfnt_image_of(&s, 0, 16.0, &im));
    CHECK(text_sfnt_ligate(&s, g, 2) == 2u);                      /* no GSUB */
    text_sfnt_close(&s);
    /* errors */
    CHECK(text_sfnt_open(&s, NULL, 0, 0) == PC_ERR_FORMAT);
    CHECK(text_sfnt_open(&s, g_bits.p, g_bits.n, 1) == PC_ERR_ARG);
    CHECK(text_sfnt_open(&s, g_bits.p, 40, 0) == PC_ERR_FORMAT);
    CHECK(text_sfnt_open(NULL, g_bits.p, g_bits.n, 0) == PC_ERR_ARG);
    text_sfnt_close(NULL);
}

/* Damaged copies: every query stays inside the buffer (ASan builds). */
static void fuzz_one(const pc_buf *src)
{
    int iters = g_quick ? 150 : 1500;
    uint8_t *b = (uint8_t *)malloc(src->n);
    if (!b) return;
    for (int it = 0; it < iters; it++) {
        size_t n = src->n;
        text_sfnt s;
        memcpy(b, src->p, src->n);
        switch (rndu(4)) {
        case 0:
            n = (size_t)rndu((uint32_t)src->n);
            break;
        case 1:
            for (uint32_t k = 1u + rndu(8); k > 0u; k--) b[rndu((uint32_t)n)] = rnd8();
            break;
        case 2: {
            size_t at = (size_t)rndu((uint32_t)(n - 4u));
            uint32_t v = rndu(3) == 0u ? 0xFFFFFFFFu : (uint32_t)rnd();
            memcpy(b + at, &v, 4u);
            break;
        }
        default:
            for (uint32_t k = 1u + rndu(64); k > 0u; k--) b[rndu((uint32_t)n)] ^= 0xFFu;
            break;
        }
        if (text_sfnt_open(&s, b, n, 0) == PC_OK) {
            pc_font_color_layer l[8];
            text_sfnt_image im;
            uint32_t g[6];
            for (uint32_t cp = 0x20u; cp < 0x60u; cp++) (void)text_sfnt_cmap(&s, cp);
            (void)text_sfnt_cmap(&s, 0x1F600u);
            (void)text_sfnt_cmap(&s, 0x2B50u);
            for (uint32_t gid = 0; gid < 12u; gid++) {
                (void)text_sfnt_advance(&s, gid);
                (void)text_sfnt_colr_layers(&s, gid, l, 8);
                (void)text_sfnt_image_of(&s, gid, 8.0, &im);
                (void)text_sfnt_image_of(&s, gid, 24.0, &im);
                if (text_sfnt_image_of(&s, gid, 64.0, &im)) CHECK(im.data && im.len > 0u);
            }
            for (int k = 0; k < 6; k++) g[k] = rndu(12);
            CHECK(text_sfnt_ligate(&s, g, 1u + rndu(6)) >= 1u);
            text_sfnt_close(&s);
        }
    }
    free(b);
}

static void t_fuzz(void)
{
    fuzz_one(&g_colr);
    fuzz_one(&g_bits);
    fuzz_one(&g_sbix);
}

/* ---- the backend -------------------------------------------------------------------------- */

static char g_dir[1024];

static bool write_fonts(void)
{
    char p[1024];
    bool ok;
    at_out_path(g_dir, sizeof g_dir, "test_toolb_colorfont_fonts");
    ok = pal_mkdirs(g_dir);
    pal_path_join(p, sizeof p, g_dir, "colr.ttf");
    ok = ok && pal_write_file_atomic(p, g_colr.p, g_colr.n) == PC_OK;
    pal_path_join(p, sizeof p, g_dir, "bits.ttf");
    ok = ok && pal_write_file_atomic(p, g_bits.p, g_bits.n) == PC_OK;
    pal_path_join(p, sizeof p, g_dir, "sbix.ttf");
    ok = ok && pal_write_file_atomic(p, g_sbix.p, g_sbix.n) == PC_OK;
    return ok;
}

/* The scanned faces; the bitmap font is registered again as "Noto Color
 * Emoji" so it is one of the catalog's emoji fallbacks. */
static bool scanned(text_face_info **out, size_t *n)
{
    const char *dirs[2];
    text_face_info *f = NULL, *g;
    size_t nf = 0;
    dirs[0] = g_dir;
    dirs[1] = NULL;
    *out = NULL;
    *n = 0;
    if (text_fonts_scan_dirs(dirs, NULL, 0, NULL, &f, &nf) != PC_OK) return false;
    g = (text_face_info *)realloc(f, (nf + 1u) * sizeof *g);
    if (!g) {
        free(f);
        return false;
    }
    for (size_t i = 0; i < nf; i++)
        if (strcmp(g[i].family, "Toolb Bits") == 0) {
            g[nf] = g[i];
            app_copy_str(g[nf].family, sizeof g[nf].family, "Noto Color Emoji");
        }
    *out = g;
    *n = nf + 1u;
    return true;
}

static void t_scan(void)
{
    text_face_info *f = NULL, *f2 = NULL;
    size_t n = 0, n2 = 0;
    char cache[1024];
    CHECK(write_fonts());
    CHECK(scanned(&f, &n));
    CHECK(n == 4u);
    for (size_t i = 0; i < n; i++) {
        CHECK(f[i].color);
        CHECK(strncmp(f[i].family, "Toolb ", 6) == 0 ||
              strcmp(f[i].family, "Noto Color Emoji") == 0);
    }
    /* the cache keeps the color flag (format 2) */
    pal_path_join(cache, sizeof cache, g_dir, "fonts.cache");
    CHECK(text_fonts_cache_write(cache, f, n) == PC_OK);
    CHECK(text_fonts_cache_read(cache, &f2, &n2) == PC_OK && n2 == n);
    for (size_t i = 0; i < n2 && i < n; i++) CHECK(f2[i].color == f[i].color);
    free(f2);
    /* a version 1 cache (no color field) is rescanned, not trusted */
    CHECK(pal_write_file_atomic(cache, "paintc-fonts 1\n/x.ttf\t0\t1\t400\t0\tX\tR\n", 37u) ==
          PC_OK);
    f2 = NULL;
    CHECK(text_fonts_cache_read(cache, &f2, &n2) != PC_OK && f2 == NULL);
    free(f);
}

static void t_backend(void)
{
    app *a = at_app(320, 200);
    text_fonts *tf = a ? text_fonts_get(a) : NULL;
    text_face_info *f = NULL;
    size_t n = 0, nf = 0;
    const pc_font_face *const *faces = NULL;
    const pc_font_face *ff;
    pc_font_bitmap bm;
    pc_font_color_layer l[4];
    uint32_t g[3];
    CHECK(tf != NULL);
    if (!tf) {
        app_destroy(a);
        return;
    }
    CHECK(scanned(&f, &n));
    CHECK(text_fonts_set_faces(tf, f, n) == PC_OK);
    free(f);
    /* a font without outlines: cmap, metrics and bitmaps from text_sfnt */
    CHECK(text_fonts_faces(tf, "Toolb Bits", false, false, &faces, &nf) == PC_OK && nf >= 1u);
    ff = faces[0];
    CHECK(ff->color && ff->glyph(ff->ud, 0x1F600u) == 1u && ff->has_glyph(ff->ud, 0x1F601u));
    CHECK(fabs(ff->advance(ff->ud, 1, 20.0, PC_TEXT_SMOOTH) - 20.0) < 1e-9);
    memset(&bm, 0, sizeof bm);
    CHECK(ff->color_bitmap(ff->ud, 1, 16.0, &bm) == PC_OK);
    CHECK(bm.w == 8 && bm.h == 8 && bm.scale == 1.0 && bm.left == 1.0 && bm.top == -7.0);
    CHECK(bm.px[0].a == 0u && px_eq(bm.px[9], 255, 0, 0, 255));
    CHECK(ff->color_bitmap(ff->ud, 2, 24.0, &bm) == PC_OK);
    CHECK(bm.w == 16 && bm.scale == 0.75 && bm.left == 1.5 && bm.top == -10.5);
    CHECK(px_eq(bm.px[17], 0, 0, 255, 255));
    CHECK(ff->color_bitmap(ff->ud, 6, 16.0, &bm) == PC_ERR_UNSUPPORTED);
    {
        pc_path p;
        pc_path_init(&p);
        CHECK(ff->outline(ff->ud, 1, 16.0, PC_TEXT_SMOOTH, &p) == PC_OK && p.n_pts == 0u);
        pc_path_free(&p);
    }
    g[0] = 3; g[1] = 4;
    CHECK(ff->substitute(ff->ud, g, 2) == 1u && g[0] == 5u);
    /* COLR font: layers, ligatures; its outlines still work */
    CHECK(text_fonts_faces(tf, "Toolb Colr", false, false, &faces, &nf) == PC_OK && nf >= 1u);
    ff = faces[0];
    CHECK(ff->color && ff->glyph(ff->ud, 0x2B50u) == 2u);
    CHECK(ff->color_layers(ff->ud, 2, l, 4) == 2u && l[1].foreground);
    CHECK(ff->color_bitmap(ff->ud, 2, 16.0, &bm) == PC_ERR_UNSUPPORTED);
    g[0] = 2; g[1] = 6; g[2] = 5;
    CHECK(ff->substitute(ff->ud, g, 3) == 1u && g[0] == 7u);
    {
        pc_font_metrics m;
        ff->metrics(ff->ud, 100.0, &m);
        CHECK(m.ascent == 80.0 && m.x_height == 50.0 && m.cap_height == 70.0);
    }
    /* sbix: the bottom-left origin becomes the top edge */
    CHECK(text_fonts_faces(tf, "Toolb Sbix", false, false, &faces, &nf) == PC_OK && nf >= 1u);
    ff = faces[0];
    CHECK(ff->color && ff->color_bitmap(ff->ud, 2, 32.0, &bm) == PC_OK);
    CHECK(bm.w == 8 && bm.scale == 2.0 && bm.left == 2.0 && bm.top == 4.0 - 16.0);
    CHECK(ff->color_bitmap(ff->ud, 3, 16.0, &bm) == PC_OK);      /* a JPEG graphic */
    CHECK(bm.w == 8 && bm.h == 8 && bm.px[27].a == 255u);
    CHECK(abs((int)bm.px[27].r - 0) < 24 && abs((int)bm.px[27].g - 200) < 24 &&
          abs((int)bm.px[27].b - 200) < 24);
    /* the built-in face is not a color face; the emoji fallback is */
    CHECK(text_fonts_faces(tf, "Inter", false, false, &faces, &nf) == PC_OK && nf >= 2u);
    CHECK(!faces[0]->color);
    {
        bool found = false;
        for (size_t i = 1; i < nf; i++) found = found || faces[i]->color;
        CHECK(found);
    }
    app_destroy(a);
}

/* ---- the Text tool -------------------------------------------------------------------------- */

static app *text_app(void)
{
    app *a = at_app(1000, 640);
    app_doc *d;
    text_fonts *tf;
    text_face_info *f = NULL;
    size_t n = 0;
    if (!a) return NULL;
    d = app_doc_new_image(a, 320, 200, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    tf = text_fonts_get(a);
    if (!tf || !scanned(&f, &n) || text_fonts_set_faces(tf, f, n) != PC_OK) {
        free(f);
        app_destroy(a);
        return NULL;
    }
    free(f);
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    (void)app_tool_select(a, "text");
    app_settings_set(app_settings_of(a), "tool.text.font", "Toolb Colr");
    app_settings_set(app_settings_of(a), "tool.text.size", "30");      /* em 40 px */
    app_settings_set(app_settings_of(a), "tool.text.unit", "0");
    app_settings_set(app_settings_of(a), "tool.text.mode", "0");
    a->ts.antialias = true;
    a->ts.blend = 0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    return a;
}

static void type(app *a, const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = s;
    app_event(a, &e);
    at_frames(a, 2);
}

static void esc(app *a)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_ESCAPE;
    e.key.down = true;
    app_event(a, &e);
    at_frames(a, 1);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 1);
}

static pc_px32 live_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = app_active_doc(a);
    pc_comp_opts o = app_doc_comp_opts(d);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(x, y, 1, 1), &p, 1u, &o);
    return p;
}

/* Glyph i of the live text (pen position), or false (*out then points at
 * an empty glyph). */
static bool glyph_at(app *a, size_t i, const pc_text_glyph **out)
{
    static pc_text_glyph none;
    const pc_text *t = text_tool_editing(a);
    size_t n = 0;
    const pc_text_glyph *g = t ? pc_text_glyphs(t, &n) : NULL;
    *out = &none;
    if (!g || i >= n) return false;
    *out = &g[i];
    return true;
}

static void t_tool(void)
{
    app *a = text_app();
    const pc_text_glyph *g = NULL;
    size_t h0;
    int32_t x, y;
    CHECK(a != NULL);
    if (!a) return;
    h0 = app_doc_history_list(app_active_doc(a), NULL, 0, NULL);
    at_drag(a, 40, 100, 40, 100, 1, SDL_BUTTON_LEFT);
    type(a, "A\xe2\xad\x90");                                  /* A, U+2B50 */
    CHECK(glyph_at(a, 1, &g) && g->gid == 2u && g->face == 0u);
    /* layer 3 covers 0..1000 x -200..800 font units, red; layer 4 (text
     * color) 250..750 x 50..550: at em 40, x +10..+30, y -22..-2 */
    x = (int32_t)floor(g->x);
    y = (int32_t)floor(g->y);
    CHECK(px_eq(live_px(a, x + 3, y - 28), 255, 0, 0, 255));
    CHECK(px_eq(live_px(a, x + 20, y - 12), 0, 0, 0, 255));
    CHECK(px_eq(live_px(a, x + 20, y + 20), 255, 255, 255, 255));
    /* the primary color is the text color, live (T-TEXT-LIVE) */
    app_set_primary(a, app_px_make(0, 0, 255, 255));
    at_frames(a, 2);
    CHECK(px_eq(live_px(a, x + 20, y - 12), 0, 0, 255, 255));
    CHECK(px_eq(live_px(a, x + 3, y - 28), 255, 0, 0, 255));
    /* ZWJ + U+2764 joins the star into the green ligature */
    type(a, "\xe2\x80\x8d\xe2\x9d\xa4");
    CHECK(glyph_at(a, 1, &g) && g->gid == 7u);
    CHECK(glyph_at(a, 3, &g) && g->hidden);
    CHECK(px_eq(live_px(a, x + 20, y - 12), 0, 255, 0, 255));
    /* U+2764 alone: the half transparent blue layer over white */
    type(a, " \xe2\x9d\xa4");
    CHECK(glyph_at(a, 5, &g) && g->gid == 5u);
    {
        pc_px32 p = live_px(a, (int32_t)floor(g->x) + 20, (int32_t)floor(g->y) - 12);
        CHECK(p.a == 255u && p.b == 255u && p.r > 120u && p.r < 135u);
    }
    /* Esc commits one history step; undo restores the white image */
    esc(a);
    CHECK(app_doc_history_list(app_active_doc(a), NULL, 0, NULL) == h0 + 1u);
    CHECK(px_eq(at_doc_px(a, x + 3, y - 28), 0, 255, 0, 255));    /* the ligature */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(px_eq(at_doc_px(a, x + 3, y - 28), 255, 255, 255, 255));
    app_destroy(a);
}

/* The built-in font with the bitmap font as the emoji fallback; a font
 * without outlines chosen directly. */
static void t_tool_fallback(void)
{
    app *a = text_app();
    const pc_text_glyph *g = NULL;
    int32_t x, y;
    CHECK(a != NULL);
    if (!a) return;
    app_settings_set(app_settings_of(a), "tool.text.font", "Inter");
    app_settings_set(app_settings_of(a), "tool.text.size", "12");     /* em 16 px */
    app_tool_settings_changed(a);
    at_frames(a, 1);
    at_drag(a, 40, 100, 40, 100, 1, SDL_BUTTON_LEFT);
    type(a, "x\xf0\x9f\x98\x80\xef\xb8\x8f");                    /* x, U+1F600, VS16 */
    CHECK(glyph_at(a, 1, &g) && g->face != 0u && g->gid == 1u);
    x = (int32_t)floor(g->x + 0.5);
    y = (int32_t)floor(g->y + 0.5);
    /* the 16 ppem strike: 8 x 8 red at (1, -7) from the pen */
    CHECK(px_eq(live_px(a, x + 5, y - 3), 255, 0, 0, 255));
    CHECK(glyph_at(a, 2, &g) && g->hidden);
    {
        const pc_text *t = text_tool_editing(a);
        CHECK(t && pc_text_color_glyph_count((pc_text *)(uintptr_t)t) == 1u);
    }
    esc(a);
    /* the bitmap font itself */
    app_settings_set(app_settings_of(a), "tool.text.font", "Toolb Bits");
    app_tool_settings_changed(a);
    at_frames(a, 1);
    at_drag(a, 40, 160, 40, 160, 1, SDL_BUTTON_LEFT);
    type(a, "\xf0\x9f\x87\xa6\xf0\x9f\x87\xa7\xf0\x9f\x98\x81");  /* flag ligature, blue */
    CHECK(glyph_at(a, 0, &g) && g->face == 0u && g->gid == 5u);
    x = (int32_t)floor(g->x + 0.5);
    y = (int32_t)floor(g->y + 0.5);
    CHECK(px_eq(live_px(a, x + 5, y - 3), 0, 255, 0, 255));
    CHECK(glyph_at(a, 2, &g) && g->gid == 2u);
    CHECK(px_eq(live_px(a, (int32_t)floor(g->x + 0.5) + 5, y - 3), 0, 0, 255, 255));
    esc(a);
    app_destroy(a);
}

/* ---- an installed color emoji font (skipped when there is none) ---------------------------- */

static bool colorful(const pc_surf *s)
{
    size_t n = 0;
    for (int32_t y = 0; y < s->h; y++)
        for (int32_t x = 0; x < s->w; x++) {
            pc_px32 p = pc_surf_row(s, y)[x];
            int d1 = abs((int)p.r - (int)p.g), d2 = abs((int)p.g - (int)p.b);
            if (p.a > 200u && (d1 > 60 || d2 > 60)) n++;
        }
    return n > 20u;
}

static void t_installed(void)
{
    static const char *const paths[] = {
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "C:\\Windows\\Fonts\\seguiemj.ttf",
    };
    const char *path = NULL;
    app *a;
    text_fonts *tf;
    text_face_info fi;
    const pc_font_face *const *faces = NULL;
    size_t nf = 0;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !path; i++)
        if (pal_file_mtime(paths[i]) != 0u) path = paths[i];
    if (!path) {
        printf("  (no installed color emoji font: smoke test skipped)\n");
        return;
    }
    a = at_app(320, 200);
    tf = a ? text_fonts_get(a) : NULL;
    CHECK(tf != NULL);
    if (!tf) {
        app_destroy(a);
        return;
    }
    memset(&fi, 0, sizeof fi);
    app_copy_str(fi.path, sizeof fi.path, path);
    app_copy_str(fi.family, sizeof fi.family, "Installed Emoji");
    fi.weight = 400;
    fi.color = true;
    CHECK(text_fonts_set_faces(tf, &fi, 1u) == PC_OK);
    CHECK(text_fonts_faces(tf, "Installed Emoji", false, false, &faces, &nf) == PC_OK);
    if (nf >= 1u) {
        pc_text *t = pc_text_create();
        pc_doc *d = pc_doc_create(400, 120);
        pc_layer *l = d ? pc_layer_create(d, "L") : NULL;
        pc_vrender *vr = pc_vrender_create();
        pc_vdraw_opts o = pc_vdraw_opts_default();
        pc_paint_src src;
        pc_text_style st;
        const pc_text_glyph *g;
        size_t ng, vis = 0;
        /* grinning face, thumbs up + skin tone, flag (U+1F1FA U+1F1F8) */
        static const char s[] = "\xf0\x9f\x98\x80\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd"
                                "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";
        CHECK(t && d && l && vr && pc_doc_insert_layer(d, l, 0) == PC_OK);
        memset(&src, 0, sizeof src);
        src.solid = app_px_make(0, 0, 0, 255);
        pc_text_style_default(&st);
        st.size = 36.0;                                          /* em 48 px */
        st.anchor = PC_TEXT_ANCHOR_BASELINE;
        CHECK(pc_text_set_fonts(t, faces, nf) == PC_OK && pc_text_set_style(t, &st) == PC_OK);
        pc_text_set_origin(t, pc_pt_make(10, 80));
        CHECK(pc_text_set_utf8(t, s, sizeof s - 1u) == PC_OK);
        g = pc_text_glyphs(t, &ng);
        for (size_t i = 0; i < ng; i++) vis += !g[i].hidden;
        CHECK(vis == 3u);                                        /* both sequences ligate */
        CHECK(pc_text_color_glyph_count(t) == 3u);
        {
            pc_txn *x = pc_txn_begin(d, "Text");
            pc_surf sf;
            CHECK(pc_text_render(t, vr, x, l->id, &src, &o, NULL, NULL) == PC_OK);
            CHECK(pc_surf_alloc(&sf, 400, 120) == PC_OK);
            CHECK(pc_txn_read_rect(x, l->id, pc_rect_make(0, 0, 400, 120), sf.px, 400) == PC_OK);
            CHECK(colorful(&sf));
            pc_surf_free(&sf);
            pc_txn_cancel(x);
        }
        pc_vrender_destroy(vr);
        pc_doc_destroy(d);
        pc_text_destroy(t);
        printf("  (smoke test with %s)\n", path);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        printf("SDL init failed\n");
        return 1;
    }
    CHECK(tf_font_colr(&g_colr) && tf_font_bits(&g_bits) && tf_font_sbix(&g_sbix));
    RUN(t_sfnt_parse);
    RUN(t_fuzz);
    RUN(t_scan);
    RUN(t_backend);
    RUN(t_tool);
    RUN(t_tool_fallback);
    RUN(t_installed);
    pc_buf_free(&g_colr);
    pc_buf_free(&g_bits);
    pc_buf_free(&g_sbix);
    at_quit();
    return pc_test_finish();
}
