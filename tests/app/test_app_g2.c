/* test_app_g2.c - gate G2 end to end through the real app, headless: open
 * a PNG (worker decode), paint a Pencil stroke with synthetic mouse events,
 * read the rendered canvas back, undo and redo with Ctrl+Z / Ctrl+Y, save
 * through the Save Configuration dialog and through the Flatten prompt,
 * then reload the file and compare every pixel. */
#include "pc_test.h"
#include "app_test_util.h"

#define W 120
#define H 90

static pc_px32 src_px(int32_t x, int32_t y)
{
    pc_px32 p;
    p.r = (uint8_t)(x * 2);
    p.g = (uint8_t)(y * 2);
    p.b = 90;
    p.a = (uint8_t)(x < 100 ? 255 : 255 - (x - 100) * 12);
    if (p.a == 0u) p.r = p.g = p.b = 0;
    return p;
}

static bool write_png(const char *path)
{
    const pc_codec *png = pc_codec_by_id("png");
    pc_doc *d = pc_doc_create(W, H);
    pc_layer *l = d ? pc_layer_create(d, "Background") : NULL;
    pc_surf s;
    pc_buf out;
    void *params;
    bool ok = false;
    memset(&out, 0, sizeof out);
    if (!png || !l || pc_surf_alloc(&s, W, H) != PC_OK) return false;
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++)
            s.px[(size_t)y * (size_t)s.stride + (size_t)x] = src_px(x, y);
    params = malloc(png->params_size ? png->params_size : 1u);
    if (params &&
        pc_layer_store_rect(d, l, pc_rect_make(0, 0, W, H), s.px, (size_t)s.stride) == PC_OK &&
        pc_doc_reserve_layers(d, 1u) == PC_OK && pc_doc_insert_layer(d, l, 0u) == PC_OK) {
        l = NULL;
        pc_codec_default_params(png, params);
        ok = png->save(d, NULL, params, NULL, &out) == PC_OK &&
             pal_write_file_atomic(path, out.p, out.n) == PC_OK;
    }
    pc_layer_destroy(l);
    pc_buf_free(&out);
    free(params);
    pc_surf_free(&s);
    pc_doc_destroy(d);
    return ok;
}

/* Independent Bresenham over the stroke's sampled points (the oracle). */
static void bres(uint8_t *mask, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < W && y0 < H) mask[y0 * W + x0] = 1;
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
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
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

static bool g_saved;
static void save_done(app *a, app_doc *d, bool ok, void *ud)
{
    (void)a;
    (void)d;
    (void)ud;
    g_saved = ok;
}

static void t_g2(void)
{
    char in_path[1024], out_path[1024];
    app *a;
    app_doc *d;
    uint8_t mask[W * H];
    int32_t pts[12][2];
    int npts = 0;
    at_out_path(in_path, sizeof in_path, "test_app_g2_in.png");
    at_out_path(out_path, sizeof out_path, "test_app_g2_out.png");
    CHECK(write_png(in_path));
    a = at_app(1200, 800);
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 2);

    /* open: decoded on a worker, then fitted (at most 100 %) */
    CHECK(app_open_path(a, in_path));
    for (int i = 0; i < 50 && app_doc_count(a) == 0; i++) at_frames(a, 1);
    CHECK(app_doc_count(a) == 1);
    d = app_active_doc(a);
    if (!d) { app_destroy(a); return; }
    at_frames(a, 2);
    CHECK(d->doc->w == W && d->doc->h == H && !app_doc_dirty(d));
    CHECK(strcmp(d->name, "test_app_g2_in.png") == 0 && d->codec == pc_codec_by_id("png"));
    CHECK(d->view.zoom == 1.0);
    for (int32_t y = 0; y < H; y += 7)
        for (int32_t x = 0; x < W; x += 5) {
            pc_px32 p = at_doc_px(a, x, y), s = src_px(x, y);
            CHECK(memcmp(&p, &s, 4) == 0);
        }

    /* the Pencil stroke (left = primary) */
    CHECK(app_tool_select(a, "pencil"));
    app_set_primary(a, app_px_make(0, 0, 255, 255));
    memset(mask, 0, sizeof mask);
    {
        double x0 = 5.5, y0 = 5.5, x1 = 60.5, y1 = 40.5;
        int steps = 11;
        at_drag(a, x0, y0, x1, y1, steps, SDL_BUTTON_LEFT);
        for (int i = 0; i <= steps; i++) {
            double t = (double)i / (double)steps;
            pts[npts][0] = (int32_t)(x0 + (x1 - x0) * t);
            pts[npts][1] = (int32_t)(y0 + (y1 - y0) * t);
            npts++;
        }
        for (int i = 1; i < npts; i++)
            bres(mask, pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1]);
    }
    CHECK(app_doc_dirty(d));
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);
    CHECK(strcmp(d->hist->cur->label, "Pencil") == 0);
    {
        int wrong = 0;
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                pc_px32 p = at_doc_px(a, x, y), s = src_px(x, y);
                if (mask[y * W + x]) wrong += !px_eq(p, 0, 0, 255, 255);
                else wrong += memcmp(&p, &s, 4) != 0;
            }
        CHECK(wrong == 0);
        if (wrong) INFO("%d pixels differ from the Bresenham oracle", wrong);
    }

    /* the canvas shows it (nearest at 100 %, opaque pixels exact) */
    at_frames(a, 2);
    {
        float sx = 0.0f, sy = 0.0f;
        CHECK(at_screen(a, 5.5, 5.5, &sx, &sy));
        CHECK(at_pixel(a, (int)sx, (int)sy) == 0x0000FFu);
        CHECK(at_screen(a, 50.5, 70.5, &sx, &sy));
        CHECK(at_pixel(a, (int)sx, (int)sy) ==
              (((uint32_t)100 << 16) | ((uint32_t)140 << 8) | 90u));
    }

    /* undo and redo from the keyboard */
    key(a, SDLK_Z, AT_KMOD_PRIMARY);
    CHECK(!app_doc_dirty(d));
    CHECK(px_eq(at_doc_px(a, 5, 5), 10, 10, 90, 255));
    {
        float sx = 0.0f, sy = 0.0f;
        CHECK(at_screen(a, 5.5, 5.5, &sx, &sy));
        CHECK(at_pixel(a, (int)sx, (int)sy) == ((10u << 16) | (10u << 8) | 90u));
    }
    key(a, SDLK_Y, AT_KMOD_PRIMARY);
    CHECK(app_doc_dirty(d));
    CHECK(px_eq(at_doc_px(a, 5, 5), 0, 0, 255, 255));

    /* Save: the first save of a PNG shows Save Configuration; Enter = OK */
    CHECK(app_doc_set_file(d, out_path, d->codec, d->save_params));
    d->save_configured = false;
    g_saved = false;
    app_save_doc(a, d, false, save_done, NULL);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    for (int i = 0; i < 20; i++) at_frames(a, 1);        /* preview encode + file size */
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 3);
    CHECK(!app_dialog_active(a) && g_saved && !app_doc_dirty(d));
    CHECK(pal_file_exists(out_path));

    /* a second layer: Save asks to flatten (Enter = Flatten), one undoable step */
    CHECK(app_cmd_exec(a, "layers.add_new"));
    CHECK(d->doc->n_layers == 2u);
    g_saved = false;
    app_save_doc(a, d, false, save_done, NULL);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 3);
    CHECK(!app_dialog_active(a) && g_saved);
    CHECK(d->doc->n_layers == 1u && strcmp(d->hist->cur->label, "Flatten") == 0);
    CHECK(!app_doc_dirty(d));
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->n_layers == 2u && app_doc_dirty(d));

    /* reload the saved file: identical composite */
    {
        pc_surf want;
        CHECK(pc_surf_alloc(&want, W, H) == PC_OK);
        CHECK(app_cmd_exec(a, "edit.redo"));
        pc_comp_rect(d->doc, pc_rect_make(0, 0, W, H), want.px, (size_t)want.stride, NULL);
        app_close_doc_now(a, d);
        CHECK(app_doc_count(a) == 0);
        CHECK(app_open_path(a, out_path));
        for (int i = 0; i < 50 && app_doc_count(a) == 0; i++) at_frames(a, 1);
        d = app_active_doc(a);
        CHECK(d != NULL);
        if (d) {
            int diff = 0;
            pc_surf got;
            CHECK(pc_surf_alloc(&got, W, H) == PC_OK);
            pc_comp_rect(d->doc, pc_rect_make(0, 0, W, H), got.px, (size_t)got.stride, NULL);
            for (int32_t i = 0; i < W * H; i++) diff += memcmp(&got.px[i], &want.px[i], 4) != 0;
            CHECK(diff == 0);
            CHECK(!app_doc_dirty(d));
            pc_surf_free(&got);
        }
        pc_surf_free(&want);
    }
    /* recent files remember both */
    CHECK(a->nrecent >= 2 && strcmp(a->recent[0], out_path) == 0);
    app_destroy(a);
    (void)pal_remove(in_path);
    (void)pal_remove(out_path);
}

/* Opening a missing or broken file reports an error and opens nothing. */
static void t_open_errors(void)
{
    char path[1024];
    app *a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    at_out_path(path, sizeof path, "test_app_g2_broken.png");
    CHECK(pal_write_file_atomic(path, "\x89PNG\r\n\x1a\nbroken", 15) == PC_OK);
    CHECK(app_open_path(a, path));
    at_frames(a, 4);
    CHECK(app_doc_count(a) == 0 && app_dialog_active(a));   /* error box */
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    at_out_path(path, sizeof path, "test_app_g2_missing.png");
    (void)pal_remove(path);
    CHECK(app_open_path(a, path));
    at_frames(a, 4);
    CHECK(app_doc_count(a) == 0 && app_dialog_active(a));
    app_destroy(a);
    at_out_path(path, sizeof path, "test_app_g2_broken.png");
    (void)pal_remove(path);
}

/* Closing a modified image prompts: Don't Save closes, Cancel keeps it. */
static void t_close_prompt(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 32, 32, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_cmd_exec(a, "layers.add_new") && app_doc_dirty(d));
    CHECK(app_cmd_exec(a, "file.close"));
    at_frames(a, 2);
    CHECK(app_dialog_active(a) && app_doc_count(a) == 1);
    key(a, SDLK_ESCAPE, SDL_KMOD_NONE);                  /* Cancel */
    CHECK(!app_dialog_active(a) && app_doc_count(a) == 1);
    CHECK(app_cmd_exec(a, "file.close"));
    at_frames(a, 2);
    /* the dialog focuses "Save"; Tab moves to "Don't Save", Space presses it */
    key(a, SDLK_TAB, SDL_KMOD_NONE);
    key(a, SDLK_SPACE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && app_doc_count(a) == 0);
    /* quitting with an unmodified image needs no prompt */
    d = app_doc_new_image(a, 32, 32, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    app_quit(a);
    CHECK(!app_frame(a, true));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_g2);
    RUN(t_open_errors);
    RUN(t_close_prompt);
    at_quit();
    return pc_test_finish();
}
