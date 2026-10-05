/* test_toola_wand_async.c - lane TOOLA: the Magic Wand computes long
 * regions in the background with a canvas spinner (T-WAND-BUSY):
 *   - the result equals pc_region_compute on the image (layer and image
 *     sampling, contiguous and global), so it is identical to the
 *     synchronous path used for small images;
 *   - while the job runs the UI keeps running, the spinner turns at the
 *     click after a short delay, and the selection changes only when the
 *     job lands;
 *   - an option change while the first click computes is applied after
 *     it (two History items); Undo while it computes abandons it.
 * Background jobs are held with sel_wand_test_hold so the busy phase is
 * deterministic; app_tasks_wait then lands them. */
#include "pc_test.h"
#include "a_util.h"
#include "pc/pc_wand.h"
#include "tools/sel_common.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

enum { W = 300, H = 200 };

static app *wand_app(void)
{
    app *a = a_app(W, H, WHITE);
    if (!a) return NULL;
    a_fill(a, pc_rect_make(10, 10, 60, 40), a_px(255, 0, 0, 255));
    a_fill(a, pc_rect_make(70, 10, 20, 40), a_px(255, 60, 60, 255));
    a_fill(a, pc_rect_make(150, 100, 40, 40), a_px(255, 0, 0, 255));
    (void)app_tool_select(a, "magic_wand");
    a->ts.tolerance = 50;
    a->ts.flood_global = false;
    a->ts.sampling = 0;
    a->ts.tol_straight = false;
    a->ts.sel_mode = PC_SEL_REPLACE;
    sel_wand_set_async_min(a, 0u);                 /* every computation in the background */
    at_frames(a, 2);
    return a;
}

/* Frames without waiting for background tasks. */
static void raw_frames(app *a, int n)
{
    for (int i = 0; i < n; i++) (void)app_frame(a, true);
}

static void raw_click(app *a, double x, double y)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    raw_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    raw_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    raw_frames(a, 1);
}

/* The selection equals the region of pc_region_compute with o at (x, y). */
static bool matches(app *a, int32_t x, int32_t y, const pc_wand_opts *o)
{
    app_doc *d = a_doc(a);
    pc_region *r = NULL;
    bool ok = true;
    if (pc_region_compute(d->doc, d->layer_id, x, y, o, NULL, &r) != PC_OK) return false;
    for (int32_t yy = 0; yy < H && ok; yy++)
        for (int32_t xx = 0; xx < W; xx++)
            if ((a_cov(a, xx, yy) != 0u) != pc_region_at(r, xx, yy)) {
                ok = false;
                break;
            }
    pc_region_free(r);
    return ok;
}

/* Darkest pixel of the rendered window in a ring around (cx, cy). */
static int darkest_ring(app *a, float cx, float cy, float r0, float r1)
{
    int best = 255;
    for (int dy = -(int)r1; dy <= (int)r1; dy++)
        for (int dx = -(int)r1; dx <= (int)r1; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            uint32_t p;
            int v;
            if (d < r0 || d > r1) continue;
            p = at_pixel(a, (int)cx + dx, (int)cy + dy);
            v = (int)(((p >> 16) & 255u) + ((p >> 8) & 255u) + (p & 255u)) / 3;
            if (v < best) best = v;
        }
    return best;
}

static void t_busy_and_identical(void)
{
    app *a = wand_app();
    pc_wand_opts o = pc_wand_opts_default();
    float sx, sy;
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    h = a_hist(a);
    sel_wand_test_hold(a, true);
    raw_click(a, 20.5, 20.5);
    CHECK(sel_wand_busy(a) && app_tool_live(a));
    CHECK(!a_active(a) && a_hist(a) == h);                /* nothing applied yet */
    /* the UI keeps running; after the delay the spinner turns at the click */
    (void)at_screen(a, 20.5, 20.5, &sx, &sy);
    raw_frames(a, 2);
    CHECK(darkest_ring(a, sx, sy, 7.0f, 12.0f) > 70);    /* not yet: pure red around */
    {
        uint64_t end = SDL_GetTicks() + 200u;
        while (SDL_GetTicks() < end) {
            raw_frames(a, 1);
            SDL_Delay(5);
        }
    }
    raw_frames(a, 2);
    CHECK(darkest_ring(a, sx, sy, 7.0f, 12.0f) < 60);    /* the spinner's dark spokes */
    {
        char path[1200];
        at_out_path(path, sizeof path, "toola_wand_spinner.bmp");
        if (a->surf) (void)SDL_SaveBMP(a->surf, path);
    }
    sel_wand_test_hold(a, false);
    app_tasks_wait(a);
    raw_frames(a, 2);
    CHECK(!sel_wand_busy(a) && a_hist(a) == h + 1u && strcmp(a_label(a), "Magic Wand") == 0);
    o.tolerance = 50.0;
    CHECK(matches(a, 20, 20, &o));
    /* the other options land identically too */
    a->ts.flood_global = true;
    app_tool_settings_changed(a);
    at_frames(a, 2);
    o.flood = PC_FLOOD_GLOBAL;
    CHECK(matches(a, 20, 20, &o) && a_cov(a, 160, 110) == 255u);
    a->ts.sampling = 1;
    a->ts.tolerance = 10;
    app_tool_settings_changed(a);
    at_frames(a, 2);
    o.sampling = PC_SAMPLE_IMAGE;
    o.tolerance = 10.0;
    CHECK(matches(a, 20, 20, &o) && a_cov(a, 75, 20) == 0u);
    a->ts.flood_global = false;
    a->ts.sampling = 0;
    a->ts.tolerance = 50;
    app_destroy(a);
}

static void t_queue_and_abandon(void)
{
    app *a = wand_app();
    pc_wand_opts o = pc_wand_opts_default();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    h = a_hist(a);
    /* an option change during the first computation follows it */
    sel_wand_test_hold(a, true);
    raw_click(a, 20.5, 20.5);
    CHECK(sel_wand_busy(a));
    a->ts.tolerance = 10;
    app_tool_settings_changed(a);
    raw_frames(a, 1);
    CHECK(!a_active(a));
    sel_wand_test_hold(a, false);
    app_tasks_wait(a);
    raw_frames(a, 1);
    app_tasks_wait(a);
    raw_frames(a, 2);
    CHECK(!sel_wand_busy(a) && a_hist(a) == h + 2u);
    o.tolerance = 10.0;
    CHECK(matches(a, 20, 20, &o) && a_cov(a, 75, 20) == 0u);
    /* Undo of the edit: the first evaluation, editable */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 2);
    CHECK(app_tool_live(a) && a->ts.tolerance == 50 && a_cov(a, 75, 20) == 255u);
    /* Undo while a new click computes abandons it */
    h = a_hist(a);
    sel_wand_test_hold(a, true);
    raw_click(a, 160.5, 110.5);
    CHECK(sel_wand_busy(a));
    CHECK(app_cmd_exec(a, "edit.undo"));
    raw_frames(a, 1);
    sel_wand_test_hold(a, false);
    app_tasks_wait(a);
    raw_frames(a, 2);
    CHECK(!sel_wand_busy(a) && a_cov(a, 160, 110) == 0u);
    CHECK(!app_tool_live(a));
    /* Finish while it computes: nothing lands either */
    sel_wand_test_hold(a, true);
    raw_click(a, 160.5, 110.5);
    CHECK(sel_wand_busy(a) && app_tool_live(a));
    h = a_hist(a);
    app_tool_finish(a);
    CHECK(!app_tool_live(a));
    sel_wand_test_hold(a, false);
    app_tasks_wait(a);
    raw_frames(a, 2);
    CHECK(a_hist(a) == h && a_cov(a, 160, 110) == 0u);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_busy_and_identical);
    RUN(t_queue_and_abandon);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
