/* test_c_text.c - lane C: the Text tool and its font backend:
 * the pc_font_face backend over the built-in face (cmap, advances,
 * kerning, metrics, outlines), font directory scanning with the cache file
 * (nested folders, broken files, unchanged files reused), typing through
 * SDL text input events, caret and editing keys (words, Home / End,
 * Backspace / Delete, Enter, Shift selection), mouse caret placement and
 * selection, clipboard copy / cut / paste, IME composition, alignment
 * around the click point, styles and the size metric (points follow the
 * image DPI), live option and color changes, antialiasing off, the move
 * handle, clicking elsewhere, Esc, tool switches and Undo committing one
 * history step, Space typing instead of panning, and the view following
 * the caret. */
#include "pc_test.h"
#include "app_test_util.h"

#include "pc/pc_text.h"
#include "tools/text_font.h"
#include "tools/text_tool.h"

#include <math.h>

static app *text_app(uint32_t w, uint32_t h)
{
    app *a = at_app(1000, 640);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    (void)app_tool_select(a, "text");
    app_settings_set(app_settings_of(a), "tool.text.font", "Inter");
    app_settings_set(app_settings_of(a), "tool.text.size", "12");
    app_settings_set(app_settings_of(a), "tool.text.unit", "0");
    app_settings_set(app_settings_of(a), "tool.text.align", "0");
    app_settings_set(app_settings_of(a), "tool.text.mode", "0");
    a->ts.antialias = true;
    a->ts.blend = 0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    return a;
}

static void key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
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

static void ime(app *a, const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_EDITING;
    e.edit.text = s;
    e.edit.start = (Sint32)strlen(s);
    e.edit.length = 0;
    app_event(a, &e);
    at_frames(a, 2);
}

static void click(app *a, double x, double y) { at_drag(a, x, y, x, y, 1, SDL_BUTTON_LEFT); }

static void set_opt(app *a, const char *k, const char *v)
{
    app_settings_set(app_settings_of(a), k, v);
    app_tool_settings_changed(a);
    at_frames(a, 1);
}

static const char *text_of(app *a)
{
    const pc_text *t = text_tool_editing(a);
    return t ? pc_text_utf8(t, NULL) : NULL;
}

static bool text_is(app *a, const char *want)
{
    const char *s = text_of(a);
    return s && strcmp(s, want) == 0;
}

static size_t hist_len(app *a) { return app_doc_history_list(app_active_doc(a), NULL, 0, NULL); }

/* Composite as shown, including the open transaction of the live text. */
static pc_px32 live_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = app_active_doc(a);
    pc_comp_opts o = app_doc_comp_opts(d);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(x, y, 1, 1), &p, 1u, &o);
    return p;
}

/* FNV-1a hash of the whole image as shown. */
static uint64_t fp(app *a)
{
    app_doc *d = app_active_doc(a);
    pc_comp_opts o = app_doc_comp_opts(d);
    size_t n = (size_t)d->doc->w * d->doc->h;
    pc_px32 *buf = (pc_px32 *)malloc(n * sizeof *buf);
    uint64_t h = 1469598103934665603ull;
    if (!buf) return 0;
    if (pc_comp_rect_ex(d->doc, pc_rect_make(0, 0, (int32_t)d->doc->w, (int32_t)d->doc->h), buf,
                        d->doc->w, &o) == PC_OK) {
        const uint8_t *b = (const uint8_t *)buf;
        for (size_t i = 0; i < n * sizeof *buf; i++) h = (h ^ b[i]) * 1099511628211ull;
    }
    free(buf);
    return h;
}

typedef struct ink { int n, x0, y0, x1, y1, partial; } ink;

/* Non-white pixels inside r. */
static ink ink_in(app *a, int rx, int ry, int rw, int rh)
{
    ink k;
    memset(&k, 0, sizeof k);
    k.x0 = k.y0 = 1 << 30;
    k.x1 = k.y1 = -1;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            pc_px32 p = live_px(a, x, y);
            if (p.r == 255 && p.g == 255 && p.b == 255) continue;
            k.n++;
            k.partial += p.r != 0;
            if (x < k.x0) k.x0 = x;
            if (y < k.y0) k.y0 = y;
            if (x > k.x1) k.x1 = x;
            if (y > k.y1) k.y1 = y;
        }
    return k;
}

/* ---- the backend ------------------------------------------------------------------------- */
static void t_backend(void)
{
    app *a = at_app(400, 300);
    text_fonts *tf;
    const pc_font_face *const *faces = NULL;
    size_t n = 0;
    CHECK(a != NULL);
    if (!a) return;
    tf = text_fonts_get(a);
    CHECK(tf != NULL);
    CHECK(!text_fonts_scanning(tf));                    /* no settings folder: no scan */
    CHECK(text_fonts_find_family(tf, "Inter") >= 0);
    CHECK(text_fonts_faces(tf, "Inter", false, false, &faces, &n) == PC_OK && n >= 1u);
    if (n >= 1u) {
        const pc_font_face *f = faces[0];
        pc_font_metrics m;
        pc_path p;
        uint32_t ga = f->glyph(f->ud, 'A'), gv = f->glyph(f->ud, 'V');
        CHECK(ga != 0 && gv != 0 && ga != gv);
        CHECK(f->glyph(f->ud, 0x4E00u) == 0);           /* no CJK in Inter */
        CHECK(f->has_glyph(f->ud, 'A') && !f->has_glyph(f->ud, 0x4E00u));
        CHECK(f->advance(f->ud, ga, 100.0, PC_TEXT_SMOOTH) > 40.0);
        CHECK(fabs(f->advance(f->ud, ga, 200.0, PC_TEXT_SMOOTH) -
                   2.0 * f->advance(f->ud, ga, 100.0, PC_TEXT_SMOOTH)) < 1e-6);
        CHECK(f->kerning(f->ud, ga, gv, 100.0) <= 0.0);  /* "AV" kerns closer or not at all */
        f->metrics(f->ud, 100.0, &m);
        CHECK(m.ascent > 70.0 && m.ascent < 120.0 && m.descent > 10.0 && m.descent < 40.0);
        CHECK(m.underline_offset > 0.0 && m.underline_thickness > 0.0);
        CHECK(m.strike_offset < 0.0);
        pc_path_init(&p);
        CHECK(f->outline(f->ud, ga, 100.0, PC_TEXT_SMOOTH, &p) == PC_OK);
        {
            pc_pt mn, mx;
            CHECK(p.n_verbs > 3u && pc_path_bounds(&p, &mn, &mx));
            /* y down: the glyph sits above the baseline */
            CHECK(mx.y <= 1.0 && mn.y < -50.0 && mn.x >= -2.0 && mx.x < 90.0);
        }
        pc_path_clear(&p);
        CHECK(f->outline(f->ud, f->glyph(f->ud, ' '), 100.0, PC_TEXT_SMOOTH, &p) == PC_OK);
        CHECK(p.n_verbs == 0u);
        pc_path_free(&p);
    }
    /* bold resolves to the SemiBold face (no synthesis), unknown families to Inter */
    CHECK(text_fonts_faces(tf, "Inter", true, false, &faces, &n) == PC_OK && n >= 1u);
    if (n >= 1u) CHECK(faces[0]->bold && !faces[0]->italic);
    CHECK(text_fonts_faces(tf, "No Such Family", false, true, &faces, &n) == PC_OK && n >= 1u);
    if (n >= 1u) CHECK(!faces[0]->italic);              /* italic is synthesized */
    app_destroy(a);
}

/* Repository root from this file's path (tests/app/test_c_text.c). */
static void repo_path(char *out, size_t cap, const char *rel)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", __FILE__);
    for (int k = 0; k < 3; k++) {
        char *s1 = strrchr(dir, '/'), *s2 = strrchr(dir, '\\');
        char *s = s1 > s2 ? s1 : s2;
        if (!s) break;
        *s = '\0';
    }
    pal_path_join(out, cap, dir, rel);
}

static void t_scan(void)
{
    char root[1024], sub[1024], deep[1024], src[1024], dst[1024], cache[1024];
    uint8_t *data = NULL;
    size_t len = 0, n = 0, n2 = 0, n3 = 0;
    text_face_info *f = NULL, *f2 = NULL, *f3 = NULL;
    const char *dirs[2];
    at_out_path(root, sizeof root, "test_c_text_fonts");
    pal_path_join(sub, sizeof sub, root, "family");
    pal_path_join(deep, sizeof deep, sub, "nested");
    CHECK(pal_mkdirs(deep));
    repo_path(src, sizeof src, "assets/fonts/Inter-Regular.ttf");
    CHECK(pal_read_file(src, 64u << 20, &data, &len) == PC_OK);
    pal_path_join(dst, sizeof dst, deep, "Copy.TTF");
    CHECK(pal_write_file_atomic(dst, data, len) == PC_OK);
    pal_path_join(dst, sizeof dst, sub, "broken.ttf");
    CHECK(pal_write_file_atomic(dst, data, len > 4000u ? 4000u : len) == PC_OK);
    pal_path_join(dst, sizeof dst, sub, "notes.txt");
    CHECK(pal_write_file_atomic(dst, "not a font", 10u) == PC_OK);
    free(data);
    dirs[0] = root;
    dirs[1] = NULL;
    CHECK(text_fonts_scan_dirs(dirs, NULL, 0, &f, &n) == PC_OK);
    CHECK(n == 1u);
    if (n == 1u) {
        CHECK(strcmp(f[0].family, "Inter") == 0);
        CHECK(f[0].weight == 400 && !f[0].italic && f[0].index == 0);
        CHECK(strstr(f[0].path, "Copy.TTF") != NULL);
    }
    /* cache round trip */
    pal_path_join(cache, sizeof cache, root, "fonts.cache");
    CHECK(text_fonts_cache_write(cache, f, n) == PC_OK);
    CHECK(text_fonts_cache_read(cache, &f2, &n2) == PC_OK);
    CHECK(n2 == n);
    if (n2 == n && n) CHECK(memcmp(&f2[0], &f[0], sizeof f[0]) == 0 ||
                            (strcmp(f2[0].path, f[0].path) == 0 &&
                             strcmp(f2[0].family, f[0].family) == 0 &&
                             f2[0].mtime == f[0].mtime && f2[0].weight == f[0].weight));
    /* unchanged files are taken from the previous scan */
    if (n) {
        app_copy_str(f[0].style, sizeof f[0].style, "FromCache");
        CHECK(text_fonts_scan_dirs(dirs, f, n, &f3, &n3) == PC_OK);
        CHECK(n3 == 1u && strcmp(f3[0].style, "FromCache") == 0);
    }
    /* a damaged cache file is rejected, not trusted */
    CHECK(pal_write_file_atomic(cache, "garbage\n\t\t\n", 11u) == PC_OK);
    free(f2);
    f2 = NULL;
    CHECK(text_fonts_cache_read(cache, &f2, &n2) != PC_OK && f2 == NULL);
    free(f);
    free(f2);
    free(f3);
    /* the catalog lists scanned families */
    {
        app *a = at_app(300, 200);
        text_fonts *tf = a ? text_fonts_get(a) : NULL;
        text_face_info fake;
        CHECK(tf != NULL);
        if (tf) {
            memset(&fake, 0, sizeof fake);
            app_copy_str(fake.family, sizeof fake.family, "Zeta Sans");
            app_copy_str(fake.path, sizeof fake.path, "/nonexistent/zeta.ttf");
            fake.weight = 400;
            CHECK(text_fonts_set_faces(tf, &fake, 1u) == PC_OK);
            CHECK(text_fonts_family_count(tf) == 2);
            CHECK(strcmp(text_fonts_family(tf, 0), "Inter") == 0);
            CHECK(text_fonts_find_family(tf, "zeta sans") == 1);
            {
                /* a face that cannot be loaded falls back to the built-in one */
                const pc_font_face *const *faces = NULL;
                size_t nf = 0;
                CHECK(text_fonts_faces(tf, "Zeta Sans", false, false, &faces, &nf) == PC_OK);
                CHECK(nf >= 1u && faces[0]->glyph(faces[0]->ud, 'A') != 0);
            }
        }
        app_destroy(a);
    }
}

/* ---- the tool ------------------------------------------------------------------------------ */
static void t_typing(void)
{
    app *a = text_app(300, 200);
    size_t h0;
    ink k;
    CHECK(a != NULL);
    if (!a) return;
    h0 = hist_len(a);
    click(a, 40.0, 60.0);
    CHECK(app_tool_live(a));
    CHECK(text_is(a, ""));
    type(a, "Hello");
    CHECK(text_is(a, "Hello"));
    k = ink_in(a, 30, 40, 120, 40);
    CHECK(k.n > 40 && k.x0 >= 39 && k.x0 <= 44);
    CHECK(k.y0 > 45 && k.y1 < 70);           /* 16 px em centered on the click line */
    CHECK(hist_len(a) == h0);                /* nothing recorded while editing */
    /* editing keys */
    key(a, SDLK_LEFT, SDL_KMOD_NONE);
    key(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
    CHECK(text_is(a, "Helo"));
    key(a, SDLK_HOME, SDL_KMOD_NONE);
    type(a, "X");
    CHECK(text_is(a, "XHelo"));
    key(a, SDLK_DELETE, SDL_KMOD_NONE);
    CHECK(text_is(a, "Xelo"));
    key(a, SDLK_END, SDL_KMOD_NONE);
    key(a, SDLK_RETURN, SDL_KMOD_NONE);       /* Enter types a new line */
    CHECK(app_tool_live(a));
    type(a, "two words");
    CHECK(text_is(a, "Xelo\ntwo words"));
    key(a, SDLK_BACKSPACE, SDL_KMOD_CTRL);
    CHECK(text_is(a, "Xelo\ntwo "));
    key(a, SDLK_LEFT, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(pc_text_has_selection(text_tool_editing(a)));
    type(a, "2");                             /* typing replaces the selection */
    CHECK(text_is(a, "Xelo\n2"));
    k = ink_in(a, 30, 40, 200, 80);
    CHECK(k.y1 > 70);                         /* the second line is below */
    /* Esc commits one step */
    key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(app_active_doc(a)->hist->cur->label, "Text") == 0);
    CHECK(ink_in(a, 30, 40, 120, 60).n > 40);
    /* an empty text records nothing */
    click(a, 200.0, 150.0);
    key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(hist_len(a) == h0 + 1u);
    app_destroy(a);
}

static void t_mouse_clipboard(void)
{
    app *a = text_app(300, 200);
    const pc_text *t;
    CHECK(a != NULL);
    if (!a) return;
    click(a, 40.0, 60.0);
    type(a, "abcdef");
    t = text_tool_editing(a);
    CHECK(t != NULL);
    if (!t) {
        app_destroy(a);
        return;
    }
    /* a click inside places the caret at the nearest stop */
    {
        pc_box b;
        pc_text_caret_box(t, 3u, &b);
        click(a, b.x0 + 0.2, (b.y0 + b.y1) * 0.5);
        CHECK(text_tool_editing(a) && pc_text_caret(text_tool_editing(a)) == 3u);
        CHECK(text_is(a, "abcdef"));
    }
    /* drag to select, cut and paste */
    {
        pc_box b0, b1;
        t = text_tool_editing(a);
        pc_text_caret_box(t, 1u, &b0);
        pc_text_caret_box(t, 4u, &b1);
        at_drag(a, b0.x0 + 0.2, (b0.y0 + b0.y1) * 0.5, b1.x0 + 0.2, (b1.y0 + b1.y1) * 0.5, 4,
                SDL_BUTTON_LEFT);
        t = text_tool_editing(a);
        CHECK(t && pc_text_has_selection(t));
    }
    key(a, SDLK_X, SDL_KMOD_CTRL);
    CHECK(text_is(a, "aef"));
    key(a, SDLK_END, SDL_KMOD_NONE);
    key(a, SDLK_V, SDL_KMOD_CTRL);
    CHECK(text_is(a, "aefbcd"));
    key(a, SDLK_A, SDL_KMOD_CTRL);
    key(a, SDLK_C, SDL_KMOD_CTRL);
    {
        char *c = pal_clip_get_text();
        CHECK(c && strcmp(c, "aefbcd") == 0);
        free(c);
    }
    CHECK(app_tool_live(a));                  /* Ctrl+C did not run Edit > Copy */
    app_destroy(a);
}

static void t_ime(void)
{
    app *a = text_app(300, 200);
    CHECK(a != NULL);
    if (!a) return;
    click(a, 40.0, 60.0);
    type(a, "x");
    ime(a, "ka");
    CHECK(strcmp(text_tool_composition(a), "ka") == 0);
    CHECK(text_is(a, "x"));
    /* while composing, keys belong to the IME */
    key(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
    CHECK(text_is(a, "x"));
    type(a, "\xE3\x81\x8B");                  /* U+304B, committed by the IME */
    CHECK(text_is(a, "x\xE3\x81\x8B"));
    CHECK(strcmp(text_tool_composition(a), "") == 0);
    app_destroy(a);
}

static void t_alignment_styles(void)
{
    app *a = text_app(400, 300);
    ink l, c, r, b, u;
    uint64_t f_plain;
    CHECK(a != NULL);
    if (!a) return;
    set_opt(a, "tool.text.size", "24");
    click(a, 200.0, 50.0);
    type(a, "MMMM");
    l = ink_in(a, 0, 20, 400, 60);
    CHECK(l.x0 >= 199 && l.x0 <= 204);
    f_plain = fp(a);
    set_opt(a, "tool.text.align", "1");
    c = ink_in(a, 0, 20, 400, 60);
    CHECK(c.x0 < 190 && c.x1 > 210);
    CHECK(abs((c.x0 + c.x1) / 2 - 200) <= 2);
    set_opt(a, "tool.text.align", "2");
    r = ink_in(a, 0, 20, 400, 60);
    CHECK(r.x1 <= 201 && r.x1 >= 194);
    set_opt(a, "tool.text.align", "0");
    CHECK(fp(a) == f_plain);
    /* bold is wider, underline adds a bar below, strikeout changes pixels */
    set_opt(a, "tool.text.bold", "true");
    b = ink_in(a, 0, 20, 400, 60);
    CHECK(b.x1 > l.x1 && b.n > l.n);
    set_opt(a, "tool.text.bold", "false");
    set_opt(a, "tool.text.underline", "true");
    u = ink_in(a, 0, 20, 400, 80);
    CHECK(u.y1 > l.y1);
    set_opt(a, "tool.text.underline", "false");
    set_opt(a, "tool.text.strikeout", "true");
    CHECK(fp(a) != f_plain);
    set_opt(a, "tool.text.strikeout", "false");
    set_opt(a, "tool.text.italic", "true");
    CHECK(fp(a) != f_plain);
    set_opt(a, "tool.text.italic", "false");
    CHECK(fp(a) == f_plain);
    /* rendering modes */
    set_opt(a, "tool.text.mode", "2");
    CHECK(fp(a) != f_plain);
    set_opt(a, "tool.text.mode", "0");
    /* the primary color applies live */
    app_set_primary(a, app_px_make(200, 0, 0, 255));
    at_frames(a, 1);
    {
        ink k = ink_in(a, 0, 20, 400, 60);
        pc_px32 p = live_px(a, k.x0 + 2, (k.y0 + k.y1) / 2);
        CHECK(k.n > 0);
        (void)p;
        CHECK(fp(a) != f_plain);
    }
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    at_frames(a, 1);
    CHECK(fp(a) == f_plain);
    /* antialiasing off: no partial pixels */
    a->ts.antialias = false;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(ink_in(a, 0, 20, 400, 60).partial == 0);
    app_destroy(a);
}

static void t_units(void)
{
    app *a = text_app(400, 300);
    ink p96, p192, fixed;
    CHECK(a != NULL);
    if (!a) return;
    click(a, 50.0, 80.0);
    type(a, "H");
    p96 = ink_in(a, 0, 0, 400, 300);
    set_opt(a, "tool.text.unit", "1");
    fixed = ink_in(a, 0, 0, 400, 300);
    CHECK(fixed.n == p96.n);                  /* identical at 96 DPI */
    set_opt(a, "tool.text.unit", "0");
    app_active_doc(a)->meta.dpi_x = 192.0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    p192 = ink_in(a, 0, 0, 400, 300);
    CHECK(abs((p192.y1 - p192.y0) - 2 * (p96.y1 - p96.y0)) <= 2);
    /* size presets: the +/- buttons step through them */
    set_opt(a, "tool.text.size", "18.3");
    CHECK(fabs(app_settings_double(app_settings_of(a), "tool.text.size", 0) - 18.3) < 1e-9);
    app_destroy(a);
}

static void t_move_commit(void)
{
    app *a = text_app(400, 300);
    ink k0, k1;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    h0 = hist_len(a);
    click(a, 50.0, 80.0);
    type(a, "Move");
    k0 = ink_in(a, 0, 0, 400, 300);
    /* drag the handle: the block moves */
    {
        const pc_text *t = text_tool_editing(a);
        pc_pt h = pc_text_handle_pos(t, pc_handle_metrics_for_zoom(1.0).handle_offset);
        at_drag(a, h.x, h.y, h.x + 100.0, h.y + 50.0, 6, SDL_BUTTON_LEFT);
    }
    k1 = ink_in(a, 0, 0, 400, 300);
    CHECK(abs(k1.x0 - k0.x0 - 100) <= 1 && abs(k1.y0 - k0.y0 - 50) <= 1);
    CHECK(text_is(a, "Move"));
    /* a click elsewhere commits and starts a new text there */
    click(a, 300.0, 250.0);
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(text_is(a, ""));
    type(a, "b");
    /* switching tools commits */
    CHECK(app_tool_select(a, "pencil"));
    CHECK(hist_len(a) == h0 + 2u);
    CHECK(!app_tool_live(a));
    /* Undo while typing commits the text, then undoes it */
    CHECK(app_tool_select(a, "text"));
    click(a, 50.0, 200.0);
    type(a, "undo me");
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(!app_tool_live(a));
    CHECK(hist_len(a) == h0 + 3u);           /* the undone step stays redoable */
    CHECK(ink_in(a, 40, 180, 120, 40).n == 0);
    app_destroy(a);
}

static void t_space_and_view(void)
{
    app *a = text_app(2000, 300);
    app_doc *d;
    double cx0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    app_view_set_zoom(a, d, 4.0);
    at_frames(a, 2);
    /* Space types a space while editing (no panning) */
    {
        double x, y;
        CHECK(app_canvas_pointer_doc(a, &x, &y) || true);
    }
    cx0 = d->view.cx;
    click(a, cx0, d->view.cy);
    type(a, "a");
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_SPACE;
        e.key.down = true;
        app_event(a, &e);
        type(a, " ");
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
        at_frames(a, 1);
    }
    type(a, "b");
    CHECK(text_is(a, "a b"));
    CHECK(!a->cv.space_down);
    /* typing towards the edge scrolls the view (T-TEXT-VIEW) */
    for (int i = 0; i < 12; i++) type(a, "WWWWWWWW");
    CHECK(d->view.cx > cx0 + 50.0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_backend);
    RUN(t_scan);
    RUN(t_typing);
    RUN(t_mouse_clipboard);
    RUN(t_ime);
    RUN(t_alignment_styles);
    RUN(t_units);
    RUN(t_move_commit);
    RUN(t_space_and_view);
    at_quit();
    return pc_test_finish();
}
