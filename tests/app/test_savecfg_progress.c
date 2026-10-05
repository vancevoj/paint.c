/* test_savecfg_progress.c - W4-SAVECFG (F-DLG-SAVECFG-FILESIZE): the Save
 * Configuration dialog's file size label while the preview encode runs.
 * A test codec whose encode waits at every tenth for the test makes each
 * step observable:
 *   t_percent        "File size: computing (p%)" follows the encode (the
 *                    encode is 90 % of the preview job), then the size
 *   t_cancel_change  changing an option mid-encode cancels that encode at
 *                    its next step; the label never shows its result and
 *                    the new options are encoded; no error dialog
 *   t_error_again    a failed encode raises the error dialog; after a
 *                    success the next failure raises it again
 *   t_real_codec     a real codec (DDS, BC7) reports progress through the
 *                    dialog and ends with the size save() gives
 * With SAVECFG_SHOTS=<folder> in the environment the dialog at 45 % is
 * written there as savecfg_computing.bmp (visual check). */
#include "pc_test.h"
#include "app_test_util.h"
#include "fx/afx.h"
#include "io/io_internal.h"

/* ---- the gated test codec ------------------------------------------------------ */
typedef struct gate_params {
    int32_t flag;      /* bool: writes 2000 bytes instead of 100 */
    int32_t fail;      /* bool: the encode fails with PC_ERR_LIMIT */
} gate_params;

static const fx_prop k_gate_props[] = {
    { "flag", "Bigger output", FXP_BOOL, (uint32_t)offsetof(gate_params, flag),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
    { "fail", "Fail the encode", FXP_BOOL, (uint32_t)offsetof(gate_params, fail),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
};

static SDL_AtomicInt g_allow;      /* steps the encode may take (test thread) */
static SDL_AtomicInt g_reached;    /* steps reported so far in the current encode */
static SDL_AtomicInt g_calls;      /* encodes started */
static SDL_AtomicInt g_cancels;    /* encodes that saw a cancel */
static SDL_AtomicInt g_gated;      /* 1: wait for g_allow, 0: run freely */

static pc_status gate_save_ex(const pc_doc *d, const pc_image_meta *meta, const void *params,
                              const pc_par *par, const pc_codec_progress *prog, pc_buf *out)
{
    gate_params p = { 0, 0 };
    uint8_t fill[2000];
    (void)d;
    (void)meta;
    (void)par;
    if (params) memcpy(&p, params, sizeof p);
    SDL_AddAtomicInt(&g_calls, 1);
    SDL_SetAtomicInt(&g_reached, 0);
    for (int step = 1; step <= 9; step++) {
        /* wait until the test allows this step (at most about 20 s) */
        for (int w = 0; SDL_GetAtomicInt(&g_gated) && SDL_GetAtomicInt(&g_allow) < step &&
                        w < 20000; w++)
            SDL_Delay(1);
        SDL_SetAtomicInt(&g_reached, step);
        if (prog && !prog->report(prog->ud, (double)step / 10.0)) {
            SDL_AddAtomicInt(&g_cancels, 1);
            return PC_ERR_CANCELLED;
        }
    }
    if (p.fail) return PC_ERR_LIMIT;
    memset(fill, 0x5A, sizeof fill);
    return pc_buf_append(out, fill, p.flag ? 2000u : 100u);
}

static pc_status gate_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                           const pc_par *par, pc_buf *out)
{
    return gate_save_ex(d, meta, params, par, NULL, out);
}

static const pc_codec k_gate = {
    "gate", "Gate", "gate", PC_CODEC_SAVE, NULL, NULL,
    k_gate_props, 2u, (uint32_t)sizeof(gate_params), gate_save, gate_save_ex
};

/* ---- helpers ------------------------------------------------------------------------- */
typedef struct saved_rec { int calls; bool ok; } saved_rec;

static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    saved_rec *r = (saved_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->ok = ok;
}

static void key_ev(app *a, SDL_Keycode k, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    app_event(a, &e);
}

/* Frames without waiting for tasks (the gated encode would block). */
static void frames(app *a, int n)
{
    for (int i = 0; i < n; i++) (void)app_frame(a, true);
}

static void tap(app *a, SDL_Keycode k)
{
    key_ev(a, k, true);
    key_ev(a, k, false);
    frames(a, 2);
}

static bool wait_reached(app *a, int step)
{
    for (int i = 0; i < 4000; i++) {
        if (SDL_GetAtomicInt(&g_reached) >= step) return true;
        frames(a, 1);
        SDL_Delay(1);
    }
    return false;
}

/* Frames until the dialog's job is done (no encode running). */
static bool wait_idle(app *a)
{
    io_savecfg_info in;
    for (int i = 0; i < 4000; i++) {
        frames(a, 1);
        if (io_savecfg_probe(a, &in) && !in.running) return true;
        SDL_Delay(1);
    }
    return false;
}

static void click_prop(app *a, const char *key)
{
    ui_rect r = afx_prop_hit(a, key, AFX_HIT_MAIN);
    float x = (float)r.x + 8.0f, y = (float)r.y + (float)r.h * 0.5f;
    CHECK(r.w > 0 && r.h > 0);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    frames(a, 2);
}

/* A w x h document of noise (every block costs the block encoders work). */
static pc_doc *noise_doc(uint32_t w, uint32_t h)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_layer *l = d ? pc_layer_create(d, "Background") : NULL;
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    if (!d || !l || !px) {
        free(px);
        pc_layer_destroy(l);
        pc_doc_destroy(d);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t v = (uint32_t)(i * 2654435761u) ^ (uint32_t)(i >> 7);
        px[i].b = (uint8_t)v;
        px[i].g = (uint8_t)(v >> 8);
        px[i].r = (uint8_t)(v >> 16);
        px[i].a = 255u;
    }
    if (pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, w) != PC_OK ||
        pc_doc_insert_layer(d, l, 0) != PC_OK) {
        free(px);
        pc_layer_destroy(l);
        pc_doc_destroy(d);
        return NULL;
    }
    free(px);
    return d;
}

static app *open_dialog(const pc_codec *codec, uint32_t w, uint32_t h, saved_rec *r)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    pc_doc *pd;
    char path[1024];
    if (!a) return NULL;
    pd = noise_doc(w, h);
    d = pd ? app_doc_create(a, pd, NULL, NULL, NULL, "Open Image") : NULL;
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    at_out_path(path, sizeof path, "savecfg_progress.out");
    CHECK(app_doc_set_file(d, path, codec, NULL));
    d->save_configured = false;
    app_save_doc(a, d, false, on_saved, r);
    frames(a, 4);                                       /* measured, placed and drawn */
    CHECK(app_dialog_depth(a) == 1);
    return a;
}

static void finish(app *a, saved_rec *r)
{
    SDL_SetAtomicInt(&g_gated, 0);
    (void)wait_idle(a);
    tap(a, SDLK_ESCAPE);                                /* cancel the save */
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE);
    app_tasks_wait(a);
    CHECK(!app_dialog_active(a) && r->calls == 1 && !r->ok);
    app_destroy(a);
}

static void reset_gate(bool gated)
{
    SDL_SetAtomicInt(&g_allow, 0);
    SDL_SetAtomicInt(&g_reached, 0);
    SDL_SetAtomicInt(&g_calls, 0);
    SDL_SetAtomicInt(&g_cancels, 0);
    SDL_SetAtomicInt(&g_gated, gated ? 1 : 0);
}

/* ---- tests ------------------------------------------------------------------------------ */
static void t_percent(void)
{
    saved_rec r = { 0, false };
    io_savecfg_info in;
    app *a;
    reset_gate(true);
    a = open_dialog(&k_gate, 64u, 64u, &r);
    CHECK(a != NULL);
    if (!a) return;
    frames(a, 1);
    CHECK(io_savecfg_probe(a, &in));
    CHECK(in.running && strcmp(in.info, "File size: computing (0%)") == 0);
    SDL_SetAtomicInt(&g_allow, 1);
    CHECK(wait_reached(a, 1));
    frames(a, 2);
    CHECK(io_savecfg_probe(a, &in) && in.running);
    CHECK(strcmp(in.info, "File size: computing (9%)") == 0);        /* 0.1 of 90 % */
    if (strcmp(in.info, "File size: computing (9%)") != 0) INFO("label: %s", in.info);
    SDL_SetAtomicInt(&g_allow, 5);
    CHECK(wait_reached(a, 5));
    frames(a, 2);
    CHECK(io_savecfg_probe(a, &in) && in.running);
    CHECK(strcmp(in.info, "File size: computing (45%)") == 0);
    if (getenv("SAVECFG_SHOTS")) {                      /* visual check of the label */
        char path[1024];
        pal_path_join(path, sizeof path, getenv("SAVECFG_SHOTS"), "savecfg_computing.bmp");
        CHECK(app_screenshot(a, path));
    }
    SDL_SetAtomicInt(&g_allow, 100);
    CHECK(wait_idle(a));
    CHECK(io_savecfg_probe(a, &in) && !in.running);
    CHECK(strcmp(in.info, "File size: 100 bytes") == 0);
    if (strcmp(in.info, "File size: 100 bytes") != 0) INFO("label: %s", in.info);
    CHECK(SDL_GetAtomicInt(&g_calls) == 1 && SDL_GetAtomicInt(&g_cancels) == 0);
    CHECK(app_dialog_depth(a) == 1);
    finish(a, &r);
}

static void t_cancel_change(void)
{
    saved_rec r = { 0, false };
    io_savecfg_info in;
    app *a;
    reset_gate(true);
    a = open_dialog(&k_gate, 64u, 64u, &r);
    CHECK(a != NULL);
    if (!a) return;
    SDL_SetAtomicInt(&g_allow, 3);
    CHECK(wait_reached(a, 3));
    frames(a, 2);
    CHECK(io_savecfg_probe(a, &in) && in.running);
    CHECK(strcmp(in.info, "File size: computing (27%)") == 0);
    click_prop(a, "flag");                              /* options change mid-encode */
    CHECK(io_savecfg_probe(a, &in));
    CHECK(in.params && ((const gate_params *)in.params)->flag == 1);
    SDL_SetAtomicInt(&g_allow, 4);                      /* its next step sees the cancel */
    for (int i = 0; i < 4000 && SDL_GetAtomicInt(&g_calls) < 2; i++) {
        frames(a, 1);
        SDL_Delay(1);
    }
    CHECK(SDL_GetAtomicInt(&g_cancels) == 1);
    CHECK(SDL_GetAtomicInt(&g_calls) == 2);             /* restarted with the new options */
    frames(a, 2);
    CHECK(io_savecfg_probe(a, &in) && in.running);
    CHECK(strncmp(in.info, "File size: computing (", 22) == 0);
    SDL_SetAtomicInt(&g_gated, 0);
    CHECK(wait_idle(a));
    CHECK(io_savecfg_probe(a, &in) && !in.running);
    CHECK(strcmp(in.info, "File size: 2.0 KB") == 0);   /* the result of flag = 1 */
    if (strcmp(in.info, "File size: 2.0 KB") != 0) INFO("label: %s", in.info);
    CHECK(SDL_GetAtomicInt(&g_calls) == 2 && SDL_GetAtomicInt(&g_cancels) == 1);
    CHECK(app_dialog_depth(a) == 1);                    /* a cancel is no error */
    finish(a, &r);
}

static void t_error_again(void)
{
    saved_rec r = { 0, false };
    io_savecfg_info in;
    app *a;
    reset_gate(false);
    a = open_dialog(&k_gate, 64u, 64u, &r);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(wait_idle(a));
    CHECK(io_savecfg_probe(a, &in) && strcmp(in.info, "File size: 100 bytes") == 0);
    for (int round = 0; round < 2; round++) {
        click_prop(a, "fail");                          /* fail = 1 */
        for (int i = 0; i < 2000 && app_dialog_depth(a) < 2; i++) {
            frames(a, 1);
            SDL_Delay(1);
        }
        CHECK(app_dialog_depth(a) == 2);                /* the error dialog */
        frames(a, 3);                                   /* shown, it takes the keyboard */
        tap(a, SDLK_RETURN);
        CHECK(app_dialog_depth(a) == 1);
        CHECK(io_savecfg_probe(a, &in) && strncmp(in.info, "File size: error", 16) == 0);
        click_prop(a, "fail");                          /* fail = 0: a success */
        CHECK(wait_idle(a));
        CHECK(io_savecfg_probe(a, &in) && strcmp(in.info, "File size: 100 bytes") == 0);
        CHECK(app_dialog_depth(a) == 1);
    }
    finish(a, &r);
}

static void t_real_codec(void)
{
    const pc_codec *dds = pc_codec_by_id("dds");
    saved_rec r = { 0, false };
    io_savecfg_info in;
    app *a;
    uint32_t seen_mid = 0;
    char want[64];
    CHECK(dds != NULL);
    if (!dds) return;
    reset_gate(false);
    a = open_dialog(dds, g_quick ? 256u : 512u, g_quick ? 256u : 512u, &r);
    CHECK(a != NULL);
    if (!a) return;
    /* the first preview (the dialog takes its final height with it) */
    CHECK(wait_idle(a));
    frames(a, 2);
    /* BC7, slow, with mip maps: long enough to be seen mid-way */
    CHECK(io_savecfg_probe(a, &in) && in.params != NULL);
    for (uint32_t i = 0; i < dds->n_props; i++) {
        int32_t v = -1;
        if (strcmp(dds->props[i].key, "format") == 0) v = 10;
        if (strcmp(dds->props[i].key, "bc7_speed") == 0) v = 2;
        if (strcmp(dds->props[i].key, "mipmaps") == 0) v = 0;
        if (v >= 0) memcpy((uint8_t *)in.params + dds->props[i].offset, &v, sizeof v);
    }
    click_prop(a, "mipmaps");                           /* on: restarts with these options */
    for (int i = 0; i < 20000; i++) {
        frames(a, 1);
        if (!io_savecfg_probe(a, &in) || !in.running) break;
        if (in.permille > 0u && in.permille < 900u &&
            strncmp(in.info, "File size: computing (", 22) == 0)
            seen_mid++;
        SDL_Delay(1);
    }
    CHECK(io_savecfg_probe(a, &in) && !in.running);
    CHECK(seen_mid > 0u);
    {
        app_doc *d = app_active_doc(a);
        pc_buf b;
        void *p = malloc(dds->params_size);
        memset(&b, 0, sizeof b);
        CHECK(p != NULL && d != NULL);
        if (p && d) {
            double kb;
            memcpy(p, in.params, dds->params_size);
            CHECK(dds->save(d->doc, NULL, p, NULL, &b) == PC_OK);
            kb = (double)b.n / 1024.0;
            if (b.n < 1024u) snprintf(want, sizeof want, "File size: %u bytes", (unsigned)b.n);
            else if (kb < 1024.0) snprintf(want, sizeof want, "File size: %.1f KB", kb);
            else snprintf(want, sizeof want, "File size: %.1f MB", kb / 1024.0);
            CHECK(strcmp(in.info, want) == 0);
            if (strcmp(in.info, want) != 0) INFO("label %s, want %s", in.info, want);
        }
        free(p);
        pc_buf_free(&b);
    }
    finish(a, &r);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_percent);
    RUN(t_cancel_change);
    RUN(t_error_again);
    RUN(t_real_codec);
    at_quit();
    return pc_test_finish();
}
