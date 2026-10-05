/* shell_panels.c - lane SHELL (wave 3b): translucent utility windows
 * (Settings > User Interface "Translucent windows", default on; WINDOWS.md
 * utility windows, OBSERVED 10).
 *
 * As in Paint.NET 3.36 (MainForm floater opacity, MIT, docs/notice/shell.md):
 * a utility window that overlaps the visible part of the image fades to
 * 75 % opacity while the pointer is elsewhere, and becomes opaque again
 * when the pointer enters it (unless a canvas drag is in progress), while
 * one of its widgets holds the mouse, or when it does not cover the image
 * at all. Fading in runs at 5 per second, fading out at 2.5 per second
 * (3.36: +0.125 / -0.0625 every 25 ms).
 *
 * Thread rules: main thread. Ownership: the per-panel opacities live in
 * app_ext "shell.panels" (freed with the app). */
#include "app_internal.h"
#include "shell_ext.h"

#include <stdlib.h>
#include <string.h>

#define PANEL_MAX 64
#define PANEL_MIN_ALPHA 0.75f
#define FADE_IN_PER_S 5.0f
#define FADE_OUT_PER_S 2.5f

typedef struct shell_panels {
    bool     off;                     /* setting off (default on) */
    float    alpha[PANEL_MAX];
    uint64_t ms[PANEL_MAX];           /* last update of each panel */
} shell_panels;

static shell_panels *state(const app *a)
{
    shell_panels *s = (shell_panels *)app_ext_get(a, "shell.panels");
    if (!s) {
        s = (shell_panels *)calloc(1u, sizeof *s);
        if (!s) return NULL;
        for (int i = 0; i < PANEL_MAX; i++) s->alpha[i] = 1.0f;
        if (!app_ext_set((app *)(uintptr_t)a, "shell.panels", s, free)) {
            free(s);
            return NULL;
        }
    }
    return s;
}

void app_panels_set_translucent(app *a, bool on)
{
    shell_panels *s = state(a);
    if (s) s->off = !on;
    app_request_frame(a);
}

bool app_panels_translucent(const app *a)
{
    shell_panels *s = state(a);
    return !s || !s->off;
}

/* The image's visible rectangle on screen (empty without an image). */
static ui_rect doc_on_screen(app *a)
{
    app_doc *d = app_active_doc(a);
    gfx_view v;
    double x0, y0, x1, y1;
    ui_rect r;
    if (!d) return ui_rect_make(0, 0, 0, 0);
    v = app_doc_gview(a, d);
    gfx_view_doc_rect(&v, &x0, &y0, &x1, &y1);
    if (x0 < -1e6) x0 = -1e6;
    if (y0 < -1e6) y0 = -1e6;
    if (x1 > 1e6) x1 = 1e6;
    if (y1 > 1e6) y1 = 1e6;
    r = ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
    return ui_rect_intersect(r, a->cv.view);
}

float app_panel_alpha(app *a, int32_t index, ui_rect pr, bool held)
{
    shell_panels *s = state(a);
    ui_ctx *ui = a->ui;
    ui_rect dr;
    ui_vec2 m;
    float target = 1.0f, cur, dt;
    uint64_t now = a->now;
    if (!s || index < 0 || index >= PANEL_MAX) return 1.0f;
    dr = doc_on_screen(a);
    m = ui_mouse_pos(ui);
    if (!s->off && !app_dialog_active(a) && !ui_rect_empty(ui_rect_intersect(pr, dr)) && !held &&
        !(ui_rect_contains(pr, m.x, m.y) && !a->cv.captured))
        target = PANEL_MIN_ALPHA;
    /* lane UIA (wave 4): a window over another window stays opaque, so
     * the one below does not show through it (only the image may) */
    for (int32_t i = 0; i < a->npanels && target < 1.0f; i++) {
        ui_rect o;
        if (i == index || !a->panels[i].st.open) continue;
        o = ui_panel_rect(ui, a->panels[i].title);
        if (!ui_rect_empty(o) && !ui_rect_empty(ui_rect_intersect(pr, o))) target = 1.0f;
    }
    cur = s->alpha[index];
    dt = s->ms[index] && now > s->ms[index] ? (float)(now - s->ms[index]) * 0.001f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    s->ms[index] = now ? now : 1u;
    if (cur < target) {
        cur += FADE_IN_PER_S * dt;
        if (cur > target) cur = target;
    } else if (cur > target) {
        cur -= FADE_OUT_PER_S * dt;
        if (cur < target) cur = target;
    }
    s->alpha[index] = cur;
    if (cur != target) app_request_frame_at(a, now + 16u);
    return cur;
}
