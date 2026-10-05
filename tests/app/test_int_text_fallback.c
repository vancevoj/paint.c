/* test_int_text_fallback.c - integration: the Text tool always draws with a
 * usable face. A font file that loads but cannot draw its characters must
 * not leave the text invisible (a "Text" History item with no pixels, seen
 * in the Windows build under Wine):
 *   - "Blank Outline": every letter maps to an empty outline, like the
 *     bitmap-only TrueType files Wine installs (Courier, MS Sans Serif,
 *     Small Fonts, System: EBDT strikes over empty glyf outlines);
 *   - "Broken Outline": every letter maps to a glyph whose outline fails
 *     validation.
 * Both families are found by a scan of a font folder like any installed
 * font. The characters fall back to the built-in Inter (each set ends in
 * it), and the font list does not preview such a family in its own (empty)
 * face. Synthetic fonts built from the OpenType specification
 * (toolb_test_fonts.h helpers). Headless, single threaded. */
#include "pc_test.h"
#include "app_test_util.h"
#include "toolb_test_fonts.h"

#include "pc/pc_text.h"
#include "tools/text_font.h"
#include "tools/text_tool.h"

static void key(app *a, SDL_Keycode k)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    app_event(a, &e);
    at_frames(a, 1);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 1);
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

/* Non-white pixels of the image as shown inside r. */
static int ink_in(app *a, int rx, int ry, int rw, int rh)
{
    app_doc *d = app_active_doc(a);
    pc_comp_opts o = app_doc_comp_opts(d);
    pc_px32 *buf = (pc_px32 *)calloc((size_t)rw * (size_t)rh, sizeof *buf);
    int n = 0;
    if (!buf) return 0;
    if (pc_comp_rect_ex(d->doc, pc_rect_make(rx, ry, rw, rh), buf, (size_t)rw, &o) == PC_OK)
        for (size_t i = 0; i < (size_t)rw * (size_t)rh; i++)
            if (buf[i].r != 255 || buf[i].g != 255 || buf[i].b != 255) n++;
    free(buf);
    return n;
}

/* A font named family with a square .notdef (glyph 0) whose cmap maps
 * A-Z, a-z and the digits to glyph 1: an empty outline (broken false) or
 * a simple glyph that claims 60001 points in a 24 byte record (broken
 * true, fails validation). */
static bool make_font(const char *family, bool broken, pc_buf *out)
{
    static const uint16_t adv[2] = { 500, 600 };
    tf_map map[62];
    size_t n = 0;
    tfb f;
    pc_buf *g, *l;
    uint32_t end0, end1;
    memset(&f, 0, sizeof f);
    for (uint32_t c = '0'; c <= 'z'; c++) {
        if ((c > '9' && c < 'A') || (c > 'Z' && c < 'a')) continue;
        map[n].cp = c;
        map[n++].gid = 1u;
    }
    tf_head(&f, true);
    tf_hhea_hmtx_maxp(&f, 2u, adv);
    tf_name_os2(&f, family);
    tf_cmap(&f, map, n, false);
    g = tfb_add(&f, "glyf");
    /* gid 0: the square (50, 0) - (450, 700), four on-curve points */
    tbs16(g, 1); tbs16(g, 50); tbs16(g, 0); tbs16(g, 450); tbs16(g, 700);
    tb16(g, 3); tb16(g, 0);
    for (int k = 0; k < 4; k++) tb8(g, 1);
    tbs16(g, 50); tbs16(g, 0); tbs16(g, 400); tbs16(g, 0);
    tbs16(g, 0); tbs16(g, 700); tbs16(g, 0); tbs16(g, -700);
    tbpad4(g);
    end0 = (uint32_t)g->n;
    if (broken) {
        tbs16(g, 1);                                   /* one contour */
        tbs16(g, 0); tbs16(g, 0); tbs16(g, 500); tbs16(g, 700);
        tb16(g, 60000);                                /* endPts: 60001 points */
        tb16(g, 0);                                    /* no instructions */
        for (int k = 0; k < 10; k++) tb8(g, 1);        /* far too few flags */
        tbpad4(g);
    }
    end1 = (uint32_t)g->n;
    l = tfb_add(&f, "loca");
    tb32(l, 0);
    tb32(l, end0);
    tb32(l, end1);                                     /* end0 == end1: gid 1 empty */
    tfb_finish(&f, false, out);
    tfb_free(&f);
    return out->n > 0u && out->p != NULL;
}

static app *text_app(void)
{
    app *a = at_app(1000, 640);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 300, 200, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    (void)app_tool_select(a, "text");
    app_settings_set(app_settings_of(a), "tool.text.size", "12");
    app_settings_set(app_settings_of(a), "tool.text.unit", "0");
    app_settings_set(app_settings_of(a), "tool.text.mode", "0");
    a->ts.antialias = true;
    a->ts.blend = 0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    return a;
}

static void t_unusable_faces(void)
{
    static const char *const fams[2] = { "Blank Outline", "Broken Outline" };
    char root[1024], path[1024];
    const char *dirs[2];
    text_face_info *found = NULL;
    size_t nfound = 0;
    app *a;
    text_fonts *tf;
    at_out_path(root, sizeof root, "test_int_text_fonts");
    CHECK(pal_mkdirs(root));
    for (int i = 0; i < 2; i++) {
        pc_buf b;
        memset(&b, 0, sizeof b);
        CHECK(make_font(fams[i], i == 1, &b));
        pal_path_join(path, sizeof path, root, i == 0 ? "blank.ttf" : "broken.ttf");
        CHECK(pal_write_file_atomic(path, b.p, b.n) == PC_OK);
        pc_buf_free(&b);
    }
    /* both load and are listed like any installed font */
    dirs[0] = root;
    dirs[1] = NULL;
    CHECK(text_fonts_scan_dirs(dirs, NULL, 0, NULL, &found, &nfound) == PC_OK);
    CHECK(nfound == 2u);
    a = text_app();
    CHECK(a != NULL);
    if (!a) {
        free(found);
        return;
    }
    tf = text_fonts_get(a);
    CHECK(tf != NULL && text_fonts_set_faces(tf, found, nfound) == PC_OK);
    for (int i = 0; i < 2 && tf; i++) {
        const pc_font_face *const *faces = NULL;
        size_t nf = 0;
        size_t h0 = app_doc_history_list(app_active_doc(a), NULL, 0, NULL);
        int32_t fi = text_fonts_find_family(tf, fams[i]);
        CHECK(fi >= 0);
        /* the family's own face comes first (it loads), but it does not
         * claim characters it cannot draw; the built-in face follows */
        CHECK(text_fonts_faces(tf, fams[i], false, false, &faces, &nf) == PC_OK && nf >= 2u);
        if (nf >= 2u) {
            CHECK(faces[0]->glyph(faces[0]->ud, 'H') == 1u);
            CHECK(!faces[0]->has_glyph(faces[0]->ud, 'H'));
            CHECK(faces[1]->has_glyph(faces[1]->ud, 'H'));
        }
        /* the font list shows the name in the UI font instead */
        for (uint32_t fr = 1; fr <= 4u && fi >= 0; fr++)
            CHECK(text_fonts_preview(tf, fi, fr) == NULL);
        /* typed with that family, the text is visible and committed */
        app_settings_set(app_settings_of(a), "tool.text.font", fams[i]);
        app_tool_settings_changed(a);
        at_frames(a, 1);
        at_drag(a, 40.0, 60.0 + 70.0 * i, 40.0, 60.0 + 70.0 * i, 1, SDL_BUTTON_LEFT);
        CHECK(app_tool_live(a));
        type(a, "Hello 42");
        CHECK(ink_in(a, 30, 40 + 70 * i, 200, 40) > 40);
        key(a, SDLK_ESCAPE);
        CHECK(!app_tool_live(a));
        CHECK(app_doc_history_list(app_active_doc(a), NULL, 0, NULL) == h0 + 1u);
        CHECK(ink_in(a, 30, 40 + 70 * i, 200, 40) > 40);
    }
    free(found);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_unusable_faces);
    at_quit();
    return pc_test_finish();
}
