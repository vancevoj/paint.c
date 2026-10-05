/* test_m_image.c - lane M: the Image menu. The size model of Resize and
 * Canvas Size (percent, aspect lock, resolution units, print size,
 * validity, memory estimate text), the dialogs driven by the keyboard
 * (observed defaults, disabled OK, one history step, resolution-only
 * changes), Canvas Size anchors and fill, Crop to Selection (deselects,
 * one step), Flip / Rotate (selection follows) and Flatten. */
#include "pc_test.h"
#include "app_test_util.h"

#include "edit/m_size.h"

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 2);
}

static void text_ev(app *a, const char *t)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = t;
    app_event(a, &e);
    at_frames(a, 1);
}

static app *with_image(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1280, 860);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

/* Applied steps (root..current), not counting the redo chain. */
static size_t hist_len(app_doc *d)
{
    size_t cur = 0;
    (void)app_doc_history_list(d, NULL, 0, &cur);
    return cur + 1u;
}

static bool near(double a, double b, double eps) { return a > b - eps && a < b + eps; }

/* ---- the size model ------------------------------------------------------------------------- */
static void t_size_model(void)
{
    m_size s;
    char buf[64], why[96];
    m_size_init(&s, 800, 600, 0.0, true, false);
    CHECK(s.by == 1 && near(s.pct, 100.0, 1e-9) && near(s.dpi, 96.0, 1e-9));
    CHECK(m_size_px(&s, false) == 800 && m_size_px(&s, true) == 600);
    CHECK(near(m_size_print(&s, false), 8.333, 0.001) && near(m_size_print(&s, true), 6.25, 1e-9));
    CHECK(m_size_valid(&s, why, sizeof why) && why[0] == '\0');
    /* aspect lock keeps the exact ratio (no rounding drift) */
    m_size_set_w(&s, 333.0);
    CHECK(m_size_px(&s, false) == 333 && m_size_px(&s, true) == 250);   /* 249.75 */
    m_size_set_h(&s, 100.0);
    CHECK(m_size_px(&s, false) == 133 && m_size_px(&s, true) == 100);
    m_size_set_keep(&s, false);
    m_size_set_w(&s, 50.0);
    CHECK(m_size_px(&s, false) == 50 && m_size_px(&s, true) == 100);
    m_size_set_keep(&s, true);                         /* height follows the width */
    CHECK(m_size_px(&s, true) == 38);
    /* by percentage, clamped at 2000 */
    m_size_set_by(&s, 0);
    CHECK(m_size_px(&s, false) == 800 && m_size_px(&s, true) == 600);
    m_size_set_pct(&s, 50.0);
    CHECK(m_size_px(&s, false) == 400 && m_size_px(&s, true) == 300);
    m_size_set_pct(&s, 99999.0);
    CHECK(near(s.pct, 2000.0, 1e-9) && m_size_px(&s, false) == 16000);
    m_size_set_pct(&s, 0.0);
    CHECK(!m_size_valid(&s, why, sizeof why) && why[0] != '\0');
    m_size_set_pct(&s, -5.0);
    CHECK(near(s.pct, 0.0, 1e-9));
    /* absolute again keeps the last computed size */
    m_size_set_pct(&s, 10.0);
    m_size_set_by(&s, 1);
    CHECK(s.by == 1 && m_size_px(&s, false) == 80 && m_size_px(&s, true) == 60);
    /* zero and oversize are invalid */
    m_size_set_keep(&s, false);
    m_size_set_w(&s, 0.0);
    CHECK(!m_size_valid(&s, NULL, 0));
    m_size_set_w(&s, 70000.0);
    CHECK(!m_size_valid(&s, why, sizeof why) && strstr(why, "65535") != NULL);
    m_size_set_w(&s, 1e9);
    CHECK(m_size_px(&s, false) == M_SIZE_MAX_EDIT);
    /* resolution: changing it keeps the pixels and changes the print size */
    m_size_init(&s, 960, 480, 96.0, false, false);
    m_size_set_res(&s, 192.0);
    CHECK(m_size_px(&s, false) == 960 && near(m_size_print(&s, false), 5.0, 1e-9));
    m_size_set_res_unit(&s, 1);                         /* pixels/cm: same dpi */
    CHECK(near(m_size_res(&s), 192.0 / 2.54, 1e-9) && near(s.dpi, 192.0, 1e-9));
    m_size_set_res(&s, 100.0);                          /* 100 px/cm = 254 dpi */
    CHECK(near(s.dpi, 254.0, 1e-9));
    s.print_unit = 1;
    CHECK(near(m_size_print(&s, false), 9.6, 1e-9));    /* 960 px / 100 px/cm */
    m_size_set_print(&s, false, 2.0);                   /* 2 cm at 100 px/cm */
    CHECK(m_size_px(&s, false) == 200 && m_size_px(&s, true) == 480);
    m_size_set_res(&s, 0.0);
    CHECK(m_size_res(&s) >= M_SIZE_MIN_RES - 1e-12);
    m_size_set_res(&s, 1e9);
    CHECK(near(m_size_res(&s), M_SIZE_MAX_RES, 1e-6));
    m_size_init(&s, 10, 10, 72.0, false, true);
    CHECK(s.res_unit == 1 && s.print_unit == 1 && near(m_size_res(&s), 72.0 / 2.54, 1e-9));
    /* memory estimate in binary units (OBSERVED 2) */
    m_size_init(&s, 800, 600, 96.0, false, false);
    m_size_format_bytes(m_size_bytes(&s, 1), buf, sizeof buf);
    CHECK(strcmp(buf, "1.8 MB") == 0);
    m_size_format_bytes(2400, buf, sizeof buf);
    CHECK(strcmp(buf, "2.3 KB") == 0);
    m_size_format_bytes(65536ull * 600ull * 4ull, buf, sizeof buf);
    CHECK(strcmp(buf, "150.0 MB") == 0);
    m_size_format_bytes(16000ull * 12000ull * 4ull, buf, sizeof buf);
    CHECK(strcmp(buf, "732.4 MB") == 0);
    m_size_format_bytes(262144ull * 196608ull * 4ull, buf, sizeof buf);
    CHECK(strcmp(buf, "192.0 GB") == 0);
    m_size_format_bytes(12, buf, sizeof buf);
    CHECK(strcmp(buf, "12 bytes") == 0);
    CHECK(m_size_bytes(&s, 3) == 800ull * 600ull * 4ull * 3ull);
}

/* ---- the dialogs ------------------------------------------------------------------------- */
static void t_resize_dialog(void)
{
    app *a = with_image(200, 120, app_px_make(255, 0, 0, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = hist_len(d);
    /* the width box has the focus; the height follows (aspect on by default) */
    CHECK(app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "50");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 50u && d->doc->h == 30u);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Resize") == 0);
    CHECK(px_eq(at_doc_px(a, 25, 15), 255, 0, 0, 255));
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->w == 200u);
    /* zero: OK stays disabled, Enter does nothing, Escape cancels */
    CHECK(app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "0");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(app_dialog_active(a));
    /* the first Escape may only leave the focused box (toolkit rule) */
    for (int i = 0; i < 3 && app_dialog_active(a); i++) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && d->doc->w == 200u && hist_len(d) == n0);
    /* Ctrl+R opens it; unchanged OK records nothing */
    tap(a, SDLK_R, AT_KMOD_PRIMARY);
    CHECK(app_dialog_active(a));
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && hist_len(d) == n0);
    app_destroy(a);
}

static void t_resolution_only(void)
{
    app *a = with_image(100, 80, app_px_make(0, 0, 0, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    d->meta.dpi_x = d->meta.dpi_y = 96.0;
    n0 = hist_len(d);
    CHECK(m_doc_set_dpi(d, 96.0, 96.0, "x") == PC_ERR_STATE);
    CHECK(m_doc_set_dpi(d, 300.0, 300.0, "Resolution") == PC_OK);
    app_doc_history_changed(a, d);
    CHECK(near(d->meta.dpi_x, 300.0, 1e-9) && hist_len(d) == n0 + 1u && app_doc_dirty(d));
    CHECK(app_cmd_exec(a, "edit.undo") && near(d->meta.dpi_x, 96.0, 1e-9) && !app_doc_dirty(d));
    CHECK(app_cmd_exec(a, "edit.redo") && near(d->meta.dpi_y, 300.0, 1e-9));
    CHECK(app_cmd_exec(a, "edit.undo"));
    /* through the dialog: width box -> height -> resolution */
    CHECK(app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "150");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 100u);
    CHECK(near(d->meta.dpi_x, 150.0, 1e-6) && strcmp(d->hist->cur->label, "Resize") == 0);
    CHECK(app_cmd_exec(a, "edit.undo") && near(d->meta.dpi_x, 96.0, 1e-9));
    app_destroy(a);
}

static void t_canvas_size(void)
{
    app *a = with_image(200, 120, app_px_make(0, 0, 255, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* default anchor Top Left, transparent fill; aspect off: only the width */
    CHECK(app_cmd_exec(a, "image.canvas_size"));
    at_frames(a, 3);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "300");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(d->doc->w == 300u && d->doc->h == 120u &&
          strcmp(d->hist->cur->label, "Canvas Size") == 0);
    CHECK(px_eq(at_doc_px(a, 5, 5), 0, 0, 255, 255) && px_eq(at_doc_px(a, 250, 60), 0, 0, 0, 0));
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->w == 200u);
    /* the remembered anchor and fill apply: Middle, Secondary Color fills
     * the new area of the bottom layer; other layers get transparent */
    {
        m_size_memory *m = m_size_memory_get(a);
        CHECK(m && m->anchor == 0 && m->fill == 0 && m->keep && m->gamma && m->resample == 0);
        if (m) {
            m->anchor = 4;
            m->fill = 2;
        }
    }
    app_set_secondary(a, app_px_make(0, 255, 0, 255));
    CHECK(app_cmd_exec(a, "layers.add_new"));
    CHECK(app_cmd_exec(a, "image.canvas_size"));
    at_frames(a, 3);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "400");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "160");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(d->doc->w == 400u && d->doc->h == 160u);
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[0], 5, 5), 0, 255, 0, 255));
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[0], 200, 80), 0, 0, 255, 255));
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[0], 99, 19), 0, 255, 0, 255));
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[0], 100, 20), 0, 0, 255, 255));
    CHECK(pc_layer_get_px(d->doc->stack[1], 5, 5).a == 0u);
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->w == 200u && d->doc->h == 120u);
    app_destroy(a);
}

/* ---- Crop, Flip, Rotate, Flatten ------------------------------------------------------------ */
static void t_geometry(void)
{
    app *a = with_image(100, 60, app_px_make(255, 255, 255, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(!app_cmd_enabled(a, "image.crop_to_selection") && !app_cmd_enabled(a, "image.flatten"));
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(10, 20, 30, 15), PC_SEL_REPLACE, "S") == PC_OK);
    app_doc_history_changed(a, d);
    n0 = hist_len(d);
    tap(a, SDLK_X, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT);
    CHECK(d->doc->w == 30u && d->doc->h == 15u && !pc_sel_is_active(d->doc));
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Crop to Selection") == 0);
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->w == 100u && pc_sel_is_active(d->doc));
    CHECK(pc_sel_bounds(d->doc).x == 10 && pc_sel_bounds(d->doc).w == 30);
    /* rotate 90: dimensions swap, the selection follows, one step each */
    tap(a, SDLK_H, AT_KMOD_PRIMARY);
    CHECK(d->doc->w == 60u && d->doc->h == 100u && strcmp(d->hist->cur->label,
                                                         "Rotate 90\xC2\xB0 Clockwise") == 0);
    CHECK(pc_sel_bounds(d->doc).w == 15 && pc_sel_bounds(d->doc).h == 30);
    tap(a, SDLK_G, AT_KMOD_PRIMARY);
    CHECK(d->doc->w == 100u && pc_sel_bounds(d->doc).x == 10);
    CHECK(app_cmd_exec(a, "image.rotate_180") && pc_sel_bounds(d->doc).x == 60);
    CHECK(app_cmd_exec(a, "image.flip_h") && pc_sel_bounds(d->doc).x == 10);
    CHECK(app_cmd_exec(a, "image.flip_v") && pc_sel_bounds(d->doc).y == 20);
    /* Flatten: enabled with two layers, one step */
    CHECK(app_cmd_exec(a, "layers.add_new") && app_cmd_enabled(a, "image.flatten"));
    n0 = hist_len(d);
    tap(a, SDLK_F, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT);
    CHECK(d->doc->n_layers == 1u && hist_len(d) == n0 + 1u && !app_cmd_enabled(a, "image.flatten"));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_size_model);
    RUN(t_resize_dialog);
    RUN(t_resolution_only);
    RUN(t_canvas_size);
    RUN(t_geometry);
    at_quit();
    return pc_test_finish();
}
