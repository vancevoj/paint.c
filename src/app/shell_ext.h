/* shell_ext.h - lane SHELL (wave 3b): additions shared by the view,
 * canvas, window and dialog files this lane owns (canvas.c, src/gfx,
 * shell.c, dlg.c, panels, mods/mod_m_settings.c, mods/mod_view.c, the
 * Color Profile dialog).
 *
 *  - auto-scroll while dragging at the view edge (V-AUTOSCROLL) and its
 *    Settings > User Interface preference;
 *  - two-finger pinch zoom from touch screens and touchpads (V-ZOOM-PINCH);
 *  - Space + arrow keys panning (V-PAN-KEYS);
 *  - display color management of the canvas and the Colors window
 *    (V-RENDER-CM, W-COL-CM): the image's profile converted to sRGB or to
 *    the display's profile (Settings > Color Management);
 *  - translucent utility windows (Settings > User Interface).
 *
 * Thread rules: main thread for everything. Ownership: the per-app state
 * lives in the app's extension store (app_ext key "shell.cv") and is freed
 * with the app; nothing returned here is owned by the caller unless
 * stated.
 */
#ifndef SHELL_EXT_H
#define SHELL_EXT_H

#include "app_internal.h"

/* ---- canvas preferences (canvas.c) ----------------------------------------------------- */
/* Settings > User Interface "Auto-scroll when drawing at the edge of the
 * window" (default on). */
void     app_canvas_set_autoscroll(app *a, bool on);
bool     app_canvas_autoscroll(const app *a);

/* V-PAN-KEYS: one Space + arrow step. dx and dy are -1, 0 or 1; ten
 * multiplies the step by ten (Space + Ctrl). The step is 10 DIPs on
 * screen, so it is a fraction of an image pixel above 1000 % and many
 * image pixels when zoomed out. */
void     app_view_key_pan(app *a, app_doc *d, int dx, int dy, bool ten);
/* Screen pixels per Space + arrow step (without the factor ten). */
double   app_view_key_pan_step(const app *a);

/* V-SEL-ANTS: frames per second of the marching ants (the display's
 * refresh rate, 60 when unknown) and whether they are paused (window not
 * focused, or the battery saver heuristic: running on battery at 20 % or
 * less, Windows' default battery saver threshold; SDL reports no OS
 * battery saver flag). */
float    app_canvas_ants_hz(app *a);
bool     app_canvas_ants_paused(app *a);
/* When the ants want the next frame (ms, a->now based), 0 when they do
 * not animate (no selection, or paused). */
uint64_t app_canvas_ants_wake(app *a);
/* Testing hook: force the battery saver state (-1 = measure, 0 off, 1 on). */
void     app_canvas_force_power_saver(app *a, int state);

/* A pen sent events in this session (Settings > Diagnostics). */
bool     app_canvas_pen_seen(const app *a);

/* V-NOFLICKER: true once the active image has been presented completely
 * at least once since it became active (until then neither the
 * checkerboard nor a partial image is shown). */
bool     app_canvas_first_shown(app *a);
/* Testing hook: work budget of the first presentation in nanoseconds
 * (0 = the default 120 ms). */
void     app_canvas_set_first_budget(app *a, uint64_t ns);

/* ---- display color management (canvas.c) ------------------------------------------------ */
/* Settings > Color Management "Use the display's color profile" (default
 * off: images are shown converted to sRGB). */
void     app_cm_set_use_display(app *a, bool on);
bool     app_cm_use_display(const app *a);
/* The display profile of the window (SDL_GetWindowICCProfile: Windows,
 * macOS and X11 _ICC_PROFILE; NULL on Wayland and headless). Borrowed,
 * valid until the next call; *len receives its size. The test hook
 * overrides it (NULL clears the override). */
const uint8_t *app_cm_display_profile(app *a, size_t *len);
void     app_cm_set_display_profile_override(app *a, const uint8_t *icc, size_t len);
/* Short description of the display profile ("" when none). */
void     app_cm_display_describe(app *a, char *out, size_t cap);
/* The view transform of d: 0 when the canvas shows d's pixel values
 * unchanged (no profile or sRGB shown on an sRGB display), else a key that
 * changes whenever the transform changes. */
uint64_t app_cm_view_key(app *a, const app_doc *d);
/* Convert n straight-alpha colors of d's working space for display (the
 * same transform the canvas uses). No-op when app_cm_view_key is 0. */
void     app_cm_to_display(app *a, const app_doc *d, pc_px32 *px, size_t n);
/* Fill the display transform of st for d (canvas draw); false when the
 * pixels are shown unchanged. The callback data stays valid until the
 * next app_cm_* call. */
bool     app_cm_gfx_style(app *a, const app_doc *d, gfx_style *st);
/* The window's display or its profile changed (SDL window events). */
void     app_cm_display_changed(app *a);
/* One-line status for Settings > Color Management. */
void     app_cm_status(app *a, char *out, size_t cap);

/* ---- translucent utility windows (panels.c / shell.c) ------------------------------------- */
/* Settings > User Interface "Translucent windows" (default on). */
void     app_panels_set_translucent(app *a, bool on);
bool     app_panels_translucent(const app *a);
/* The opacity panel number index should have this frame (0.75 .. 1),
 * animated toward its target (frames are requested while it moves). pr is
 * the panel's rectangle as shown last frame, held whether one of its
 * widgets holds the mouse (ui_panel_held). */
float    app_panel_alpha(app *a, int32_t index, ui_rect pr, bool held);

#endif /* SHELL_EXT_H */
