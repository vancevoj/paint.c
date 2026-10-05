/* test_m_layers.c - lane M: the Layers menu. Enable states, active layer
 * rules, names, one history step per command, the Move Layer keys,
 * Import From File (names, canvas growth, shared tiles, selection, one
 * step, undo), Rotate / Zoom (engine mapping, live preview transaction,
 * OK commits one step, Cancel leaves no trace, session memory) and the
 * Layer Properties stand-in. */
#include "pc_test.h"
#include "app_test_util.h"

#include "edit/m_import.h"
#include "edit/m_rotzoom.h"
#include "pc/pc_layerops.h"

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

static size_t hist_len(app_doc *d)
{
    size_t cur = 0;
    (void)app_doc_history_list(d, NULL, 0, &cur);
    return cur + 1u;
}

static const char *lname(app_doc *d, uint32_t i) { return d->doc->stack[i]->name; }

/* ---- the plain commands ---------------------------------------------------------------- */
static void t_commands(void)
{
    app *a = with_image(64, 48, app_px_make(255, 255, 255, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(!app_cmd_enabled(a, "layers.delete") && !app_cmd_enabled(a, "layers.merge_down"));
    CHECK(!app_cmd_enabled(a, "layers.move_up") && !app_cmd_enabled(a, "layers.go_up"));
    CHECK(app_cmd_enabled(a, "layers.add_new") && app_cmd_enabled(a, "layers.duplicate"));
    CHECK(app_cmd_enabled(a, "layers.rotate_zoom") && app_cmd_enabled(a, "layers.properties"));
    n0 = hist_len(d);
    /* Add New Layer: "Layer 2" above, active */
    tap(a, SDLK_N, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1 &&
          strcmp(lname(d, 1), "Layer 2") == 0);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Add New Layer") == 0);
    /* Duplicate: "<name> copy" above, active */
    tap(a, SDLK_D, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(d->doc->n_layers == 3u && app_doc_layer_index(d) == 2 &&
          strcmp(lname(d, 2), "Layer 2 copy") == 0);
    /* Go to (no history) */
    n0 = hist_len(d);
    tap(a, SDLK_PAGEDOWN, SDL_KMOD_LALT);
    CHECK(app_doc_layer_index(d) == 1 && hist_len(d) == n0);
    tap(a, SDLK_PAGEDOWN, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    CHECK(app_doc_layer_index(d) == 0 && !app_cmd_enabled(a, "layers.go_down"));
    tap(a, SDLK_PAGEUP, SDL_KMOD_LCTRL | SDL_KMOD_LALT);
    CHECK(app_doc_layer_index(d) == 2 && !app_cmd_enabled(a, "layers.go_top"));
    tap(a, SDLK_PAGEUP, SDL_KMOD_LALT);
    CHECK(app_doc_layer_index(d) == 2 && hist_len(d) == n0);
    /* Move Layer keys (OBSERVED 11): the layer moves, stays active */
    tap(a, SDLK_PAGEDOWN, SDL_KMOD_LALT | SDL_KMOD_LSHIFT);
    CHECK(app_doc_layer_index(d) == 1 && strcmp(lname(d, 1), "Layer 2 copy") == 0);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Move Layer Down") == 0);
    tap(a, SDLK_PAGEDOWN, SDL_KMOD_LCTRL | SDL_KMOD_LALT | SDL_KMOD_LSHIFT);
    CHECK(app_doc_layer_index(d) == 0 && strcmp(lname(d, 0), "Layer 2 copy") == 0);
    tap(a, SDLK_PAGEUP, SDL_KMOD_LCTRL | SDL_KMOD_LALT | SDL_KMOD_LSHIFT);
    CHECK(app_doc_layer_index(d) == 2 && strcmp(lname(d, 2), "Layer 2 copy") == 0);
    tap(a, SDLK_PAGEUP, SDL_KMOD_LALT | SDL_KMOD_LSHIFT);      /* already on top: nothing */
    CHECK(app_doc_layer_index(d) == 2);
    /* Toggle visibility keeps the layer active; one step */
    n0 = hist_len(d);
    tap(a, SDLK_COMMA, SDL_KMOD_LCTRL);
    CHECK(!d->doc->stack[2]->visible && app_doc_layer_index(d) == 2 && hist_len(d) == n0 + 1u);
    tap(a, SDLK_COMMA, SDL_KMOD_LCTRL);
    CHECK(d->doc->stack[2]->visible);
    /* Merge Down: the lower layer stays active and keeps its name */
    tap(a, SDLK_M, SDL_KMOD_LCTRL);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1);
    /* Delete: the layer below becomes active; disabled with one layer */
    tap(a, SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(d->doc->n_layers == 1u && app_doc_layer_index(d) == 0);
    CHECK(!app_cmd_enabled(a, "layers.delete"));
    n0 = hist_len(d);
    tap(a, SDLK_DELETE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(d->doc->n_layers == 1u && hist_len(d) == n0);
    /* Flip and Rotate 180 of the layer: one step each */
    CHECK(pc_layerop_fill(d->hist, d->layer_id, pc_rect_make(0, 0, 8, 8),
                          app_px_make(255, 0, 0, 255), false, NULL, "f") == PC_OK);
    app_doc_history_changed(a, d);
    CHECK(app_cmd_exec(a, "layers.flip_h"));
    CHECK(pc_layer_get_px(d->doc->stack[0], 60, 2).r == 255u &&
          pc_layer_get_px(d->doc->stack[0], 2, 2).g == 255u);
    CHECK(strcmp(d->hist->cur->label, "Flip Layer Horizontal") == 0);
    CHECK(app_cmd_exec(a, "layers.flip_v") && pc_layer_get_px(d->doc->stack[0], 60, 45).g == 0u);
    CHECK(app_cmd_exec(a, "layers.rotate_180") && pc_layer_get_px(d->doc->stack[0], 2, 2).g == 0u);
    app_destroy(a);
}

/* ---- Import From File --------------------------------------------------------------------- */
static bool write_png(const char *path, uint32_t w, uint32_t h, pc_px32 c)
{
    const pc_codec *png = pc_codec_by_id("png");
    pc_doc *doc = pc_doc_create(w, h);
    pc_layer *l = doc ? pc_layer_create(doc, "Background") : NULL;
    pc_surf s;
    pc_buf buf;
    void *params;
    bool ok = false;
    memset(&buf, 0, sizeof buf);
    if (!png || !l || pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return false;
    }
    for (int32_t y = 0; y < s.h; y++)
        for (int32_t x = 0; x < s.w; x++) pc_surf_row(&s, y)[x] = c;
    pc_surf_row(&s, 0)[0] = app_px_make(1, 2, 3, 255);
    (void)pc_layer_store_rect(doc, l, pc_rect_make(0, 0, s.w, s.h), s.px, (size_t)s.stride);
    pc_surf_free(&s);
    (void)pc_doc_reserve_layers(doc, 1u);
    (void)pc_doc_insert_layer(doc, l, 0u);
    params = malloc(png->params_size ? png->params_size : 1u);
    if (params) {
        pc_codec_default_params(png, params);
        ok = png->save(doc, NULL, params, NULL, &buf) == PC_OK &&
             pal_write_file_atomic(path, buf.p, buf.n) == PC_OK;
    }
    free(params);
    pc_buf_free(&buf);
    pc_doc_destroy(doc);
    return ok;
}

static void t_import(void)
{
    app *a = with_image(80, 60, app_px_make(255, 255, 255, 255));
    app_doc *d;
    char p1[1024], p2[1024], p3[1024];
    const char *paths[3];
    size_t n0;
    uint64_t f0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    at_out_path(p1, sizeof p1, "m_import_small.png");
    at_out_path(p2, sizeof p2, "m_import_big.png");
    at_out_path(p3, sizeof p3, "m_import_missing.png");
    CHECK(write_png(p1, 30, 20, app_px_make(0, 0, 255, 255)));
    CHECK(write_png(p2, 130, 70, app_px_make(0, 255, 0, 255)));
    (void)pal_remove(p3);
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(1, 1, 5, 5), PC_SEL_REPLACE, "S") == PC_OK);
    app_doc_history_changed(a, d);
    n0 = hist_len(d);
    f0 = pc_doc_fingerprint(d->doc);
    paths[0] = p1;
    paths[1] = p3;            /* reported and skipped */
    paths[2] = p2;
    m_import_paths(a, d->id, paths, 3);
    at_frames(a, 3);
    if (app_dialog_active(a)) tap(a, SDLK_RETURN, SDL_KMOD_NONE);   /* the error box */
    CHECK(d->doc->n_layers == 3u && hist_len(d) == n0 + 1u);
    CHECK(strcmp(d->hist->cur->label, "Import From File") == 0);
    CHECK(strcmp(lname(d, 1), "m_import_small:Background") == 0);
    CHECK(strcmp(lname(d, 2), "m_import_big:Background") == 0 && app_doc_layer_index(d) == 2);
    CHECK(d->doc->w == 130u && d->doc->h == 70u);
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[0], 100, 65), 0, 0, 0, 0));   /* grown: transparent */
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[1], 0, 0), 1, 2, 3, 255));
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[1], 29, 19), 0, 0, 255, 255));
    CHECK(pc_layer_get_px(d->doc->stack[1], 30, 19).a == 0u);
    CHECK(px_eq(pc_layer_get_px(d->doc->stack[2], 129, 69), 0, 255, 0, 255));
    CHECK(pc_sel_is_active(d->doc) && pc_sel_bounds(d->doc).w == 130 &&
          pc_sel_bounds(d->doc).h == 70);
    CHECK(pc_doc_edge_padding_is_zero(d->doc));
    /* one undo restores everything (size, layers, selection) */
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(d->doc->n_layers == 1u && d->doc->w == 80u && pc_doc_fingerprint(d->doc) == f0);
    CHECK(pc_sel_is_active(d->doc) && pc_sel_bounds(d->doc).w == 5);
    CHECK(app_cmd_exec(a, "edit.redo") && d->doc->n_layers == 3u && d->doc->w == 130u);
    (void)pal_remove(p1);
    (void)pal_remove(p2);
    app_destroy(a);
}

/* ---- Rotate / Zoom ------------------------------------------------------------------------ */
static void t_rotzoom(void)
{
    app *a = with_image(64, 64, app_px_make(255, 255, 255, 255));
    app_doc *d;
    m_rz_values v, *mem;
    pc_rotzoom rz;
    size_t n0;
    uint64_t f0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* the mapping of the dialog values */
    m_rz_values_default(&v);
    CHECK(v.zoom == 1.0 && v.quality == 1.0 && v.tiling == 0 && v.sampling == 1 && v.tilt == 0.0);
    v.tilt = 90.0;
    v.tiling = 2;
    v.sampling = 0;
    v.quality = 8.0;
    v.zoom = 0.0;
    m_rz_to_rotzoom(&v, &rz);
    CHECK(rz.tilt <= 89.9 && rz.tiling == PC_WRAP_MIRROR && rz.sampling == PC_SAMPLE_NEAREST);
    CHECK(rz.quality == 8u && rz.zoom > 0.0);
    /* a red square in the top left corner */
    CHECK(pc_layerop_fill(d->hist, d->layer_id, pc_rect_make(0, 0, 16, 16),
                          app_px_make(255, 0, 0, 255), false, NULL, "f") == PC_OK);
    app_doc_history_changed(a, d);
    n0 = hist_len(d);
    f0 = pc_doc_fingerprint(d->doc);
    /* Cancel: preview transaction while open, nothing afterwards */
    mem = m_rotzoom_memory(a);
    CHECK(mem != NULL);
    if (!mem) {
        app_destroy(a);
        return;
    }
    mem->angle = 180.0;
    tap(a, SDLK_Z, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    at_frames(a, 2);
    CHECK(app_dialog_active(a) && d->txn != NULL);
    if (d->txn) {                                    /* previewed in the transaction */
        pc_px32 p;
        CHECK(pc_txn_read_rect(d->txn, d->layer_id, pc_rect_make(56, 56, 1, 1), &p, 1u) == PC_OK);
        CHECK(p.r == 255u && p.g == 0u && at_doc_px(a, 56, 56).g == 255u);
    }
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && d->txn == NULL && hist_len(d) == n0);
    CHECK(pc_doc_fingerprint(d->doc) == f0 && at_doc_px(a, 56, 56).g == 255u);
    /* OK: one step; the values are remembered */
    tap(a, SDLK_Z, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    at_frames(a, 2);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && d->txn == NULL && hist_len(d) == n0 + 1u);
    CHECK(strcmp(d->hist->cur->label, "Rotate / Zoom") == 0);
    CHECK(pc_layer_get_px(d->doc->stack[0], 56, 56).r == 255u &&
          pc_layer_get_px(d->doc->stack[0], 56, 56).g == 0u);
    CHECK(pc_layer_get_px(d->doc->stack[0], 4, 4).g == 255u);
    CHECK(m_rotzoom_memory(a)->angle == 180.0);
    CHECK(app_cmd_exec(a, "edit.undo") && pc_doc_fingerprint(d->doc) == f0);
    /* zoom out with tiling Repeat: the corner shows up again elsewhere */
    m_rz_values_default(mem);
    mem->zoom = 0.5;
    mem->tiling = 1;
    CHECK(app_cmd_exec(a, "layers.rotate_zoom"));
    at_frames(a, 2);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(hist_len(d) == n0 + 1u);
    {
        int red = 0;
        for (int32_t y = 0; y < 64; y += 2)
            for (int32_t x = 0; x < 64; x += 2) {
                pc_px32 p = pc_layer_get_px(d->doc->stack[0], (uint32_t)x, (uint32_t)y);
                if (p.r > 200 && p.g < 60) red++;
            }
        CHECK(red > 40);          /* four copies of an 8 x 8 corner, sampled every 2 px */
    }
    app_destroy(a);
}

/* ---- Layer Properties stand-in --------------------------------------------------------------- */
static void t_properties(void)
{
    app *a = with_image(32, 32, app_px_make(255, 255, 255, 255));
    app_doc *d;
    const app_cmd *c;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    c = app_cmd_find(a, "layers.properties");
    CHECK(c && (c->flags & APP_CMD_WEAK));       /* the Layers window lane may replace it */
    tap(a, SDLK_F4, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(app_dialog_active(a));
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    text_ev(a, "Sky");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && strcmp(lname(d, 0), "Sky") == 0);
    CHECK(strcmp(d->hist->cur->label, "Layer Properties") == 0);
    CHECK(app_cmd_exec(a, "edit.undo") && strcmp(lname(d, 0), "Background") == 0);
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
    RUN(t_commands);
    RUN(t_import);
    RUN(t_rotzoom);
    RUN(t_properties);
    at_quit();
    return pc_test_finish();
}
