/* test_shell_dialogs.c - lane SHELL (wave 3b): dialogs.
 *   t_no_dim         Layer Properties and Rotate / Zoom leave the canvas undimmed
 *                    (their preview is the canvas), other dialogs still dim it
 *   t_new_image      New Image: 0 or 99999 disables OK (Enter does nothing),
 *                    typed values commit on Tab (never silently reverted), a
 *                    valid size creates exactly that image, the print unit is
 *                    its own choice and remembered
 *   t_clamp_focus    effect dialogs open with the first numeric box focused;
 *                    an out-of-range value typed there clamps on Tab
 *   t_rotzoom_gamma  Rotate / Zoom samples in linear light (core pc_warp_grid_ex)
 *   t_savecfg_error  a failed size computation raises the error dialog once
 *   t_help_alt_h     Alt+H opens the Help menu */
#include "pc_test.h"
#include "app_test_util.h"
#include "edit/m_rotzoom.h"
#include "fx/afx.h"
#include "pc/pc_layerops.h"
#include "pc/pc_resample.h"

#include <math.h>

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

static app *with_image(int ww, int wh, uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(ww, wh);
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

static void t_no_dim(void)
{
    app *a = with_image(1200, 800, 1000, 600, app_px_make(255, 255, 255, 255));
    app_doc *d;
    int32_t sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* a canvas pixel left of the centered dialogs, right of the Tools window */
    {
        float fx, fy;
        CHECK(at_screen(a, 150.0, 300.0, &fx, &fy));
        sx = (int32_t)fx;
        sy = (int32_t)fy;
    }
    CHECK(at_pixel(a, sx, sy) == 0xFFFFFFu);
    CHECK(app_cmd_exec(a, "layers.properties"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    CHECK(at_pixel(a, sx, sy) == 0xFFFFFFu);          /* not dimmed */
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    CHECK(app_cmd_exec(a, "layers.rotate_zoom"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    CHECK(at_pixel(a, sx, sy) == 0xFFFFFFu);
    for (int i = 0; i < 3 && app_dialog_active(a); i++) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);
    /* a plain dialog (Resize) still dims the window */
    CHECK(app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a) && at_pixel(a, sx, sy) != 0xFFFFFFu);
    for (int i = 0; i < 3 && app_dialog_active(a); i++) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    app_destroy(a);
}

static void t_new_image(void)
{
    app *a = at_app(1000, 760);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 2);
    app_new_image_dialog(a);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    CHECK(ui_text_input_active(a->ui));              /* O-UI-FOCUS: the Width box */
    /* 0: OK disabled, Enter does nothing */
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "0");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(app_dialog_active(a) && app_doc_count(a) == 0);
    /* back to the Width box: 99999 is above paint.c's limit: still disabled */
    tap(a, SDLK_TAB, SDL_KMOD_LSHIFT);
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "99999");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(app_dialog_active(a) && app_doc_count(a) == 0);
    /* a valid width typed then Tab: exactly that size */
    tap(a, SDLK_TAB, SDL_KMOD_LSHIFT);
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "640");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "123");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && app_doc_count(a) == 1);
    d = app_active_doc(a);
    CHECK(d && d->doc->w == 640u && d->doc->h == 123u);
    CHECK(d && d->meta.dpi_x > 95.99 && d->meta.dpi_x < 96.01);
    /* the print unit is remembered on its own */
    (void)app_settings_set_int(app_settings_of(a), "file.new.print_unit", 1);
    (void)app_settings_set_int(app_settings_of(a), "file.new.res_unit", 0);
    app_new_image_dialog(a);
    at_frames(a, 3);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    CHECK(app_settings_int(app_settings_of(a), "file.new.print_unit", -1) == 1);
    CHECK(app_settings_int(app_settings_of(a), "file.new.res_unit", -1) == 0);
    app_destroy(a);
}

static double param_of(const app *a, const char *key)
{
    const fx_effect *fx = a->last_effect ? fx_registry_find(a->fx, a->last_effect) : NULL;
    if (!fx || !a->last_effect_params) return NAN;
    for (uint32_t i = 0; i < fx->n_props; i++)
        if (strcmp(fx->props[i].key, key) == 0) {
            double v;
            memcpy(&v, (const uint8_t *)a->last_effect_params + fx->props[i].offset, sizeof v);
            return v;
        }
    return NAN;
}

static void t_clamp_focus(void)
{
    app *a = with_image(1000, 760, 64, 64, app_px_make(10, 20, 30, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 4);
    CHECK(app_dialog_active(a));
    CHECK(ui_text_input_active(a->ui));              /* the Radius box has the focus */
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "99999");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);                  /* commits, clamped to 300 */
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(afx_wait_idle(a, 400));
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    CHECK(param_of(a, "radius") == 300.0);
    app_destroy(a);
}

/* A black and white checker rotated 45 degrees: in linear light the mixed
 * edge pixels are brighter than the gamma-space average. */
static void t_rotzoom_gamma(void)
{
    pc_doc *d = pc_doc_create(64, 64);
    pc_layer *l;
    pc_grid g;
    pc_warp w;
    pc_tile **lin = NULL, **gam = NULL;
    double sum_lin = 0.0, sum_gam = 0.0;
    pc_px32 px[64 * 64];
    CHECK(d != NULL);
    if (!d) return;
    l = pc_layer_create(d, "L");
    CHECK(l != NULL && pc_doc_insert_layer(d, l, 0) == PC_OK);
    for (uint32_t y = 0; y < 64u; y++)
        for (uint32_t x = 0; x < 64u; x++) {
            uint8_t v = (((x / 2u) + (y / 2u)) & 1u) ? 255u : 0u;
            pc_px32 *p = &px[y * 64u + x];
            p->b = v; p->g = v; p->r = v; p->a = 255u;
        }
    CHECK(pc_layer_store_rect(d, l, pc_rect_make(0, 0, 64, 64), px, 64u) == PC_OK);
    g = pc_grid_of_layer(d, l);
    memset(&w, 0, sizeof w);
    CHECK(pc_xform_invert(pc_xform_rotate(45.0), &w.inv));
    w.sample = PC_SAMPLE_BILINEAR;
    w.wrap = PC_WRAP_REPEAT;
    w.quality = 2u;
    w.aa_edges = true;
    CHECK(pc_warp_grid_ex(&g, &w, true, 64u, 64u, NULL, &lin) == PC_OK);
    CHECK(pc_warp_grid_ex(&g, &w, false, 64u, 64u, NULL, &gam) == PC_OK);
    if (lin && gam) {
        for (uint32_t i = 0; i < PC_TILE_PX; i++) {
            sum_lin += lin[0] ? (double)lin[0]->data[4u * i + 1u] : 0.0;
            sum_gam += gam[0] ? (double)gam[0]->data[4u * i + 1u] : 0.0;
        }
        /* the gamma-space mean of a 50 % checker is about 128; the linear
         * light mean encodes to about 188 */
        CHECK(sum_gam / (double)PC_TILE_PX < 150.0);
        CHECK(sum_lin / (double)PC_TILE_PX > 165.0);
    }
    pc_grid_free(lin, 1u);
    pc_grid_free(gam, 1u);
    /* identity with gamma on is an exact copy */
    {
        pc_tile **id = NULL;
        pc_warp wi = w;
        bool same = true;
        wi.inv = pc_xform_identity();
        wi.quality = 1u;
        wi.wrap = PC_WRAP_NONE;
        CHECK(pc_warp_grid_ex(&g, &wi, true, 64u, 64u, NULL, &id) == PC_OK);
        for (uint32_t y = 0; id && y < 64u; y++)
            for (uint32_t x = 0; x < 64u; x++) {
                pc_px32 o = pc_layer_get_px(l, x, y);
                const uint8_t *q = id[0]->data + ((size_t)y * 64u + x) * 4u;
                if (q[0] != o.b || q[1] != o.g || q[2] != o.r || q[3] != o.a) same = false;
            }
        CHECK(same);
        pc_grid_free(id, 1u);
    }
    /* the Rotate / Zoom dialog asks for it */
    {
        m_rz_values v;
        pc_rotzoom rz;
        pc_rotzoom_default(&rz);
        CHECK(!rz.gamma);
        m_rz_values_default(&v);
        m_rz_to_rotzoom(&v, &rz);
        CHECK(rz.gamma);
    }
    pc_doc_destroy(d);
}

typedef struct saved_rec { int calls; bool ok; } saved_rec;

static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    saved_rec *r = (saved_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->ok = ok;
}

static void t_savecfg_error(void)
{
    /* WebP cannot hold more than 16383 pixels per side */
    app *a = with_image(1000, 720, 16400, 4, app_px_make(1, 2, 3, 255));
    app_doc *d;
    char path[1024];
    saved_rec r = { 0, false };
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    at_out_path(path, sizeof path, "shell_cfg_error.webp");
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("webp"), NULL));
    d->save_configured = false;
    app_save_doc(a, d, false, on_saved, &r);
    at_frames(a, 1);
    CHECK(app_dialog_depth(a) >= 1);                 /* Save Configuration */
    for (int i = 0; i < 20 && app_dialog_depth(a) < 2; i++) {
        app_tasks_wait(a);
        at_frames(a, 1);
    }
    CHECK(app_dialog_depth(a) == 2);                 /* the error dialog on top */
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);              /* dismiss it */
    CHECK(app_dialog_depth(a) == 1);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);              /* cancel the save */
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && r.calls == 1 && !r.ok);
    app_destroy(a);
}

static void t_help_alt_h(void)
{
    app *a = with_image(1000, 720, 64, 64, app_px_make(1, 2, 3, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true);
    tap(a, SDLK_H, SDL_KMOD_LALT);
    key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false);
    at_frames(a, 2);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pan") != 0);
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
    RUN(t_no_dim);
    RUN(t_new_image);
    RUN(t_clamp_focus);
    RUN(t_rotzoom_gamma);
    RUN(t_savecfg_error);
    RUN(t_help_alt_h);
    at_quit();
    return pc_test_finish();
}
