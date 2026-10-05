/* test_f_large.c - lane F: effects stay responsive on an 8192 x 8192 image
 * (the task's large-image requirement). Opening a dialog returns at once
 * (the snapshot is copied on a worker), no frame waits for the render
 * (frames are timed while the workers render and the preview is blended),
 * a parameter change returns at once and the next job starts within a few
 * frames, Cancel returns at once, and OK commits one history item.
 * Frame limits are generous so that sanitizer builds pass; the INFO lines
 * print the measured times. */
#include "pc_test.h"
#include "f_test_util.h"

static double now_ms(void) { return (double)SDL_GetTicksNS() / 1e6; }

/* Run frames without waiting for background work; returns the longest. */
static double frames_max(app *a, int n, double *total)
{
    double worst = 0.0;
    for (int i = 0; i < n; i++) {
        double t0 = now_ms(), dt;
        (void)app_frame(a, true);
        dt = now_ms() - t0;
        if (dt > worst) worst = dt;
        if (total) *total += dt;
    }
    return worst;
}

/* Instrumented or unoptimized builds get generous limits; the INFO lines
 * still show the times. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(__OPTIMIZE__)
#  define F_SLOW_BUILD 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define F_SLOW_BUILD 1
#  endif
#endif

static void t_large(void)
{
    const uint32_t n = 8192u;
#if defined(F_SLOW_BUILD)
    const double frame_limit = 5000.0, call_limit = 1000.0;
#else
    const double frame_limit = 250.0, call_limit = 60.0;
#endif
    app *a = at_app(1280, 800);
    app_doc *d;
    afx_session *s;
    const fx_effect *fx;
    double t0, dt, worst, total = 0.0;
    uint32_t runs;
    int frames = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, n, n, app_px_make(40, 120, 200, 255));
    CHECK(d != NULL && app_add_doc(a, d));
    if (!d) {
        app_destroy(a);
        return;
    }
    at_frames(a, 3);
    fx = fx_registry_find(a->fx, "org.paintc.adjust.hue_saturation");
    CHECK(fx != NULL);
    /* opening: the snapshot of 64 M pixels is copied on a worker */
    t0 = now_ms();
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.hue_saturation"));
    dt = now_ms() - t0;
    INFO("open: %.1f ms", dt);
    CHECK(dt < call_limit);
    s = afx_active(a);
    CHECK(s != NULL);
    if (!s || !fx) {
        app_destroy(a);
        return;
    }
    worst = frames_max(a, 3, &total);
    INFO("first frames: worst %.1f ms", worst);
    CHECK(worst < frame_limit);
    /* render and blend while frames keep coming */
    for (frames = 0; frames < 20000 && !afx_session_preview_done(s); frames++) {
        dt = frames_max(a, 1, &total);
        if (dt > worst) worst = dt;
        if (frames > 4000) SDL_Delay(1);
    }
    CHECK(afx_session_preview_done(s));
    INFO("preview: %d frames, worst %.1f ms, mean %.1f ms", frames, worst,
         frames ? total / (double)(frames + 3) : 0.0);
    CHECK(worst < frame_limit);
    /* a change while the next render runs: returns at once, restarts soon */
    runs = afx_session_runs(s);
    CHECK(fx_param_set(fx, afx_session_params(s), "hue", 90.0) == PC_OK);
    t0 = now_ms();
    afx_session_changed(a, s);
    dt = now_ms() - t0;
    INFO("change: %.2f ms", dt);
    CHECK(dt < call_limit);
    CHECK(fx_param_set(fx, afx_session_params(s), "hue", -45.0) == PC_OK);
    (void)frames_max(a, 1, NULL);
    t0 = now_ms();
    afx_session_changed(a, s);              /* cancels the running job */
    dt = now_ms() - t0;
    CHECK(dt < call_limit);
    worst = 0.0;
    for (frames = 0; frames < 2000 && afx_session_runs(s) < runs + 2u; frames++) {
        dt = frames_max(a, 1, NULL);
        if (dt > worst) worst = dt;
        SDL_Delay(1);
    }
    INFO("restart after cancel: %d frames, worst %.1f ms", frames, worst);
    CHECK(afx_session_runs(s) >= runs + 2u);
    CHECK(frames < 200);
    CHECK(worst < frame_limit);
    /* Cancel returns at once and restores */
    t0 = now_ms();
    afx_session_cancel(a, s);
    (void)app_frame(a, true);
    dt = now_ms() - t0;
    INFO("cancel: %.1f ms", dt);
    CHECK(dt < frame_limit);
    CHECK(d->txn == NULL && !app_dialog_active(a));
    /* OK on a dialog-less adjustment: one history item, frames stay short */
    worst = 0.0;
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.invert_colors"));
    for (frames = 0; frames < 20000 && app_dialog_active(a); frames++) {
        dt = frames_max(a, 1, NULL);
        if (dt > worst) worst = dt;
        if (frames > 4000) SDL_Delay(1);
    }
    INFO("invert 8K: %d frames, worst %.1f ms", frames, worst);
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(strcmp(d->hist->cur->label, "Invert Colors") == 0);
    {
        pc_px32 p;
        CHECK(pc_comp_rect(d->doc, pc_rect_make((int32_t)n - 1, (int32_t)n - 1, 1, 1), &p, 1u,
                           NULL) == PC_OK);
        CHECK(px_eq(p, 215, 135, 55, 255));
    }
    app_tasks_wait(a);
    app_destroy(a);
}

/* An image whose working copies cannot fit is refused with a message
 * instead of allocating (X-26: Linux overcommits). */
static void t_too_large(void)
{
    const uint32_t n = 65535u;
    uint64_t ram = pal_ram_bytes(), need = (uint64_t)n * n * 12u;
    app *a;
    app_doc *d;
    if (ram == 0u || need <= ram / 2u) {
        INFO("skipped: %llu MiB of RAM would fit a %u x %u run",
             (unsigned long long)(ram >> 20), (unsigned)n, (unsigned)n);
        return;
    }
    a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, n, n, app_px_make(1, 2, 3, 255));
    CHECK(d != NULL && app_add_doc(a, d));
    if (d) {
        int depth = app_dialog_depth(a);
        CHECK(!afx_open(a, fx_registry_find(a->fx, "org.paintc.adjust.invert_colors")));
        CHECK(afx_active(a) == NULL && d->txn == NULL);
        CHECK(app_dialog_depth(a) == depth + 1);        /* the error message */
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_large);
    RUN(t_too_large);
    at_quit();
    return pc_test_finish();
}
