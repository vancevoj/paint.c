/* test_savecfg_layout.c - W4-SAVECFG (F-FILE-DDS-SAVE-BC7SPEED): the Save
 * Configuration options column never lets a control overflow, at 100 % and
 * 200 % UI scale, in the light and dark themes, for every file type with
 * options.
 *   t_wide    a window with room: every control stays inside the options
 *             column (left of the preview, inside the dialog), drop-downs
 *             are as wide as their longest item, check box labels are not
 *             cut, and the DDS column fits "BC6H / BC7 compression speed"
 *             with its drop-down on one row
 *   t_narrow  a window too narrow for that (200 %): the preview shrinks
 *             first, then drop-downs move below their labels; still no
 *             control crosses into the preview or out of the dialog
 * With SAVECFG_SHOTS=<folder> in the environment the DDS and PNG dialogs
 * are also written there as BMP screenshots (visual checks). */
#include "pc_test.h"
#include "app_test_util.h"
#include "fx/afx.h"
#include "io/io_internal.h"

#include <math.h>

typedef struct saved_rec { int calls; bool ok; } saved_rec;

static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    saved_rec *r = (saved_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->ok = ok;
}

static void tap(app *a, SDL_Keycode k)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    app_event(a, &e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

static app *make_app(int w, int h, float scale, int theme)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.workers = 2;
    o.config_dir = "";
    o.theme = theme;
    o.scale = scale;
    o.no_default_doc = true;
    return app_create(&o);
}

static int32_t text_w(app *a, const char *s)
{
    return (int32_t)ceilf(ui_text_width(ui_font_regular(a->ui), ui_font_px(a->ui), s, strlen(s)));
}

/* Check every control of the open dialog; wide: there was room for the
 * natural widths. Returns false when the dialog is missing. */
static bool check_dialog(app *a, const pc_codec *c, bool wide, const char *what)
{
    io_savecfg_info in;
    int32_t opt_r, fails0 = (int32_t)g_fails;
    if (!io_savecfg_probe(a, &in)) {
        CHECK(false);
        INFO("%s %s: no Save Configuration dialog", c->id, what);
        return false;
    }
    opt_r = in.options.x + in.options.w;
    CHECK(in.options.w > 0 && in.preview.w > 0);
    CHECK(opt_r <= in.preview.x);                           /* columns do not overlap */
    CHECK(in.preview.x + in.preview.w <= in.dialog.x + in.dialog.w);
    CHECK(in.options.x >= in.dialog.x);
    for (uint32_t i = 0; i < c->n_props; i++) {
        const fx_prop *p = &c->props[i];
        ui_rect r = afx_prop_hit(a, p->key, AFX_HIT_MAIN);
        if (p->kind == FXP_CUSTOM) continue;
        CHECK(r.w > 0 && r.h > 0);                          /* every option is shown */
        CHECK(r.x >= in.options.x && r.x + r.w <= opt_r);  /* inside the column */
        CHECK(r.x + r.w <= in.preview.x);
        CHECK(r.y >= in.dialog.y && r.y + r.h <= in.dialog.y + in.dialog.h);
        if (p->kind == FXP_CHOICE && p->choices) {
            int32_t wmax = 0;
            for (uint32_t k = 0; p->choices[k]; k++)
                if (text_w(a, p->choices[k]) > wmax) wmax = text_w(a, p->choices[k]);
            /* a drop-down shows its longest item uncut: text, padding, arrow */
            CHECK(r.w >= wmax + 2 * ui_px(a->ui, 10.0f) + ui_px(a->ui, 24.0f) ||
                  (!wide && r.w == in.options.w));
        }
        if (p->kind == FXP_BOOL && wide) {
            int32_t need = ui_px(a->ui, ui_get_theme(a->ui)->m.check) + ui_px(a->ui, 8.0f) +
                           text_w(a, p->label);
            CHECK(r.w >= need);                             /* the label is not cut */
        }
    }
    if ((int32_t)g_fails != fails0)
        INFO("%s %s: options x %d w %d, preview x %d w %d, dialog x %d w %d", c->id, what,
             in.options.x, in.options.w, in.preview.x, in.preview.w, in.dialog.x, in.dialog.w);
    return true;
}

static app *new_app(int ww, int wh, float scale, int theme)
{
    app *a = make_app(ww, wh, scale, theme);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 96u, 96u, app_px_make(40, 90, 200, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

/* Open Save Configuration for the image with type c. */
static bool open_cfg(app *a, const pc_codec *c, saved_rec *r)
{
    app_doc *d = app_active_doc(a);
    char path[1024];
    if (!d) return false;
    at_out_path(path, sizeof path, "savecfg_layout.out");
    CHECK(app_doc_set_file(d, path, c, NULL));
    d->save_configured = false;
    app_save_doc(a, d, false, on_saved, r);
    at_frames(a, 4);                     /* measured, placed, preview done */
    CHECK(app_dialog_depth(a) == 1);
    return app_dialog_depth(a) == 1;
}

static void close_cfg(app *a, saved_rec *r)
{
    tap(a, SDLK_ESCAPE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE);
    CHECK(!app_dialog_active(a) && r->calls == 1 && !r->ok);
}

static void shot(app *a, const char *id, const char *what)
{
    const char *dir = getenv("SAVECFG_SHOTS");
    char name[128], path[1024];
    if (!dir || !*dir) return;
    snprintf(name, sizeof name, "savecfg_%s_%s.bmp", id, what);
    pal_path_join(path, sizeof path, dir, name);
    CHECK(app_screenshot(a, path));
}

static void t_wide(void)
{
    static const struct { int w, h; float scale; int theme; const char *what; } k_cfg[] = {
        { 1280, 800, 1.0f, APP_THEME_LIGHT, "100_light" },
        { 1280, 800, 1.0f, APP_THEME_DARK, "100_dark" },
        { 1800, 1060, 2.0f, APP_THEME_LIGHT, "200_light" },
        { 1800, 1060, 2.0f, APP_THEME_DARK, "200_dark" },
    };
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    for (size_t k = 0; k < sizeof k_cfg / sizeof k_cfg[0]; k++) {
        app *a = new_app(k_cfg[k].w, k_cfg[k].h, k_cfg[k].scale, k_cfg[k].theme);
        CHECK(a != NULL);
        if (!a) continue;
        for (size_t i = 0; i < n; i++) {
            const pc_codec *c = list[i];
            saved_rec r = { 0, false };
            bool dds = strcmp(c->id, "dds") == 0;
            if (!(c->flags & PC_CODEC_SAVE) || !c->n_props || !c->params_size) continue;
            /* the themes share the geometry: the dark runs only need screenshots */
            if (k_cfg[k].theme == APP_THEME_DARK && !dds && !getenv("SAVECFG_SHOTS")) continue;
            /* quick runs (sanitizers): 200 % frames are slow, so the types with
             * drop-downs only; the layout math does not depend on the scale */
            if (g_quick && k_cfg[k].scale > 1.0f && !strstr("dds png avif jpeg webp tiff", c->id))
                continue;
            if (!open_cfg(a, c, &r)) continue;
            if (check_dialog(a, c, true, k_cfg[k].what) && dds) {
                /* the speed drop-down sits on the row of its label */
                ui_rect sp = afx_prop_hit(a, "bc7_speed", AFX_HIT_MAIN);
                io_savecfg_info in;
                CHECK(io_savecfg_probe(a, &in));
                CHECK(sp.x > in.options.x + text_w(a, "BC6H / BC7 compression speed:"));
            }
            if (dds || strcmp(c->id, "png") == 0 || strcmp(c->id, "avif") == 0)
                shot(a, c->id, k_cfg[k].what);
            close_cfg(a, &r);
        }
        app_destroy(a);
    }
}

static void t_narrow(void)
{
    static const char *const k_ids[] = { "dds", "png", "avif", "webp", "tiff" };
    app *a = new_app(1000, 900, 2.0f, APP_THEME_LIGHT);
    CHECK(a != NULL);
    if (!a) return;
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; i++) {
        const pc_codec *c = pc_codec_by_id(k_ids[i]);
        saved_rec r = { 0, false };
        io_savecfg_info in;
        if (!c || !(c->flags & PC_CODEC_SAVE)) continue;
        /* 200 %: 984 px of dialog cannot hold a 300 DIP column and the
         * 420 DIP preview */
        if (!open_cfg(a, c, &r)) continue;
        (void)check_dialog(a, c, false, "narrow_200");
        CHECK(io_savecfg_probe(a, &in));
        CHECK(in.preview.w >= ui_px(a->ui, 200.0f) - 1);   /* the preview keeps its minimum */
        if (strcmp(c->id, "dds") == 0) {
            /* the label above, the drop-down below it at the column's left */
            ui_rect sp = afx_prop_hit(a, "bc7_speed", AFX_HIT_MAIN);
            CHECK(sp.x == in.options.x);
            shot(a, c->id, "narrow_200");
        }
        close_cfg(a, &r);
    }
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
    RUN(t_wide);
    RUN(t_narrow);
    at_quit();
    return pc_test_finish();
}
