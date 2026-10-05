/* canvas.c - the image view: pointer routing to tools (mouse and pen),
 * panning and zooming (docs/inventory/VIEW.md), scroll bars, rulers, the
 * draw callback (gfx), marching ants, tool overlays and cursors.
 *
 * Pointer routing: SDL pointer events are queued as they arrive and
 * replayed during the frame, after the canvas declared its ui_interact
 * region. The UI decides whether a left press belongs to the canvas (no
 * panel, popup or dialog above it, press not consumed by closing a menu);
 * right and middle presses need the canvas hovered in this and the
 * previous frame. Once a press is accepted the canvas captures the
 * pointer until every button is released, so strokes keep every motion
 * event at full rate. Pen events carry pressure; the mouse events SDL
 * synthesizes from pens are ignored for tools. Main thread.
 *
 * Lane SHELL (wave 3b, shell_ext.h): auto-scroll while a tool drags at
 * the view edge (V-AUTOSCROLL), two-finger pinch zoom (V-ZOOM-PINCH),
 * Space + arrows panning (V-PAN-KEYS), Zoom to Window twice re-centers
 * (V-ZOOM-RECENTER), the first presentation of an image waits for its
 * visible tiles (V-NOFLICKER), marching ants at the display refresh rate
 * with a battery saver pause (V-SEL-ANTS). */
#include "app_internal.h"
#include "shell_ext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_ID "##canvas"
#define RULER_DIP 20.0f
#define SBAR_DIP  14.0f
#define FIT_MARGIN_DIP 12.0f
#define UPDATE_BUDGET_NS 14000000ull     /* view cache work per frame */
#define FIRST_BUDGET_NS 120000000ull     /* first presentation of an image (V-NOFLICKER) */
#define ANTS_STEP_MS 60u                 /* 3.36: the dashes move one pixel per 60 ms */
#define KEY_PAN_DIP 10.0                 /* V-PAN-KEYS step on screen */
#define AS_EDGE_DIP 4.0f                 /* auto-scroll starts this close to the edge */
#define AS_MIN_SPEED 240.0               /* screen px per second at the edge */
#define AS_GAIN 14.0                     /* extra px per second per px beyond it */
#define AS_MAX_SPEED 6000.0
#define SHELL_MAX_FINGERS 10

/* ---- lane SHELL state (app_ext "shell.cv") ------------------------------------------------- */
typedef struct shell_finger {
    SDL_TouchID  touch;
    SDL_FingerID id;
    float        x, y;               /* normalized 0..1 */
} shell_finger;

typedef struct shell_cv {
    bool         autoscroll;          /* Settings > User Interface (default on) */
    uint64_t     as_ns;               /* time of the last auto-scroll step, 0 = idle */
    /* two-finger gestures */
    shell_finger fing[SHELL_MAX_FINGERS];
    int          nfing;
    bool         pinching;
    int          pinch_mode;          /* 0 undecided, 1 zoom/pan, 2 scroll (indirect) */
    bool         pinch_direct;        /* touch screen: positions map to the window */
    SDL_TouchID  pinch_touch;
    double       p_d0, p_cx0, p_cy0;  /* start distance and centroid (normalized) */
    double       p_z0, p_docx, p_docy;/* zoom at the start, image point under the centroid */
    bool         pen_seen;            /* a pen event arrived (Settings > Diagnostics) */
    bool         touch_block;         /* ignore touch-synthesized mouse until fingers lift */
    bool         touch_press;         /* the last canvas press came from a touch screen */
    uint32_t     touch_doc;           /* image and history node at that press */
    uint64_t     touch_seq;
    /* first presentation (V-NOFLICKER) */
    uint32_t     shown_doc;
    bool         shown_ok;
    uint64_t     shown_ms;            /* when the image became active */
    uint64_t     first_budget_ns;     /* 0 = FIRST_BUDGET_NS (tests lower it) */
    /* marching ants */
    double       ants_phase;          /* dash offset, screen px */
    uint64_t     ants_ms;             /* time of the last phase advance, 0 = paused */
    uint64_t     power_ms;            /* last battery check */
    bool         power_saver;
    int          power_force;         /* -1 measure, 0 off, 1 on (tests) */
} shell_cv;

static shell_cv *scv(const app *a)
{
    shell_cv *s = (shell_cv *)app_ext_get(a, "shell.cv");
    if (!s) {
        s = (shell_cv *)calloc(1u, sizeof *s);
        if (!s) return NULL;
        s->autoscroll = true;
        s->power_force = -1;
        if (!app_ext_set((app *)(uintptr_t)a, "shell.cv", s, free)) {
            free(s);
            return NULL;
        }
    }
    return s;
}

void app_canvas_set_autoscroll(app *a, bool on)
{
    shell_cv *s = scv(a);
    if (s) s->autoscroll = on;
}

bool app_canvas_autoscroll(const app *a)
{
    shell_cv *s = scv(a);
    return s ? s->autoscroll : true;
}

void app_canvas_force_power_saver(app *a, int state)
{
    shell_cv *s = scv(a);
    if (s) {
        s->power_force = state < 0 ? -1 : (state ? 1 : 0);
        s->power_ms = 0;
    }
}

void app_canvas_set_first_budget(app *a, uint64_t ns)
{
    shell_cv *s = scv(a);
    if (s) s->first_budget_ns = ns;
}

bool app_canvas_pen_seen(const app *a)
{
    shell_cv *s = scv(a);
    return s && s->pen_seen;
}

bool app_canvas_first_shown(app *a)
{
    shell_cv *s = scv(a);
    app_doc *d = app_active_doc(a);
    return s && d && s->shown_doc == d->id && s->shown_ok;
}

/* ---- views ---------------------------------------------------------------------- */
gfx_view app_doc_gview(const app *a, const app_doc *d)
{
    gfx_view v;
    memset(&v, 0, sizeof v);
    v.zoom = d ? d->view.zoom : 1.0;
    v.cx = d ? d->view.cx : 0.0;
    v.cy = d ? d->view.cy : 0.0;
    v.vx = a->cv.view.x;
    v.vy = a->cv.view.y;
    v.vw = a->cv.view.w > 1 ? a->cv.view.w : 1;
    v.vh = a->cv.view.h > 1 ? a->cv.view.h : 1;
    v.dw = d ? d->doc->w : 1u;
    v.dh = d ? d->doc->h : 1u;
    return v;
}

void app_doc_set_gview(app *a, app_doc *d, const gfx_view *v)
{
    if (!d) return;
    if (d->view.zoom != v->zoom || d->view.cx != v->cx || d->view.cy != v->cy)
        app_request_frame(a);
    d->view.zoom = v->zoom;
    d->view.cx = v->cx;
    d->view.cy = v->cy;
}

static int32_t fit_margin(const app *a) { return ui_px(a->ui, FIT_MARGIN_DIP); }

void app_view_set_zoom(app *a, app_doc *d, double z)
{
    gfx_view v;
    if (!d) return;
    v = app_doc_gview(a, d);
    gfx_view_zoom_center(&v, z, a->overscroll);
    d->view.fit_mode = false;
    app_doc_set_gview(a, d, &v);
}

void app_view_zoom_step(app *a, app_doc *d, int dir, bool at_point, double sx, double sy)
{
    gfx_view v;
    double z;
    if (!d) return;
    v = app_doc_gview(a, d);
    z = dir > 0 ? gfx_zoom_next_in(v.zoom) : gfx_zoom_next_out(v.zoom);
    if (at_point) gfx_view_zoom_at(&v, z, sx, sy, a->overscroll);
    else gfx_view_zoom_center(&v, z, a->overscroll);
    d->view.fit_mode = false;
    app_doc_set_gview(a, d, &v);
}

void app_view_fit_toggle(app *a, app_doc *d)
{
    gfx_view v;
    if (!d) return;
    v = app_doc_gview(a, d);
    if (d->view.fit_mode && d->view.has_prev) {
        /* V-ZOOM-WINDOW / V-ZOOM-RECENTER: the second invocation restores
         * the previous zoom level and keeps the image centered (the
         * ViewTools "Centering an Image" trick; 3.36 also only restored
         * the scale). prev_cx / prev_cy are kept for API compatibility. */
        v.zoom = d->view.prev_zoom;
        v.cx = (double)v.dw * 0.5;
        v.cy = (double)v.dh * 0.5;
        gfx_view_clamp(&v, a->overscroll);
        d->view.fit_mode = false;
        d->view.has_prev = false;
    } else {
        d->view.prev_zoom = v.zoom;
        d->view.prev_cx = v.cx;
        d->view.prev_cy = v.cy;
        d->view.has_prev = true;
        gfx_view_fit_window(&v, fit_margin(a), a->overscroll);
        d->view.fit_mode = true;
    }
    app_doc_set_gview(a, d, &v);
}

void app_view_actual(app *a, app_doc *d) { app_view_set_zoom(a, d, 1.0); }

void app_view_zoom_rect(app *a, app_doc *d, double x, double y, double w, double h)
{
    gfx_view v;
    if (!d) return;
    v = app_doc_gview(a, d);
    gfx_view_fit_rect(&v, x, y, w, h, fit_margin(a), a->overscroll);
    d->view.fit_mode = false;
    app_doc_set_gview(a, d, &v);
}

void app_view_pan_px(app *a, app_doc *d, double dx, double dy)
{
    gfx_view v;
    if (!d) return;
    v = app_doc_gview(a, d);
    gfx_view_pan_px(&v, dx, dy, a->overscroll);
    app_doc_set_gview(a, d, &v);
}

double app_view_key_pan_step(const app *a) { return (double)ui_px(a->ui, (float)KEY_PAN_DIP); }

void app_view_key_pan(app *a, app_doc *d, int dx, int dy, bool ten)
{
    double step;
    if (!d || (dx == 0 && dy == 0)) return;
    step = app_view_key_pan_step(a) * (ten ? 10.0 : 1.0);
    /* the arrow moves the view: Right shows what is to the right */
    app_view_pan_px(a, d, -(double)dx * step, -(double)dy * step);
}

void app_view_home(app *a, app_doc *d, int which)
{
    gfx_view v;
    double x0, x1, y0, y1, hw, hh;
    if (!d) return;
    v = app_doc_gview(a, d);
    gfx_view_range(&v, a->overscroll, &x0, &x1, &y0, &y1);
    hw = (double)v.vw / (2.0 * v.zoom);
    hh = (double)v.vh / (2.0 * v.zoom);
    switch (which) {
    case 0: v.cx = x0; break;                                   /* Home */
    case 1: v.cx = x1; break;                                   /* End */
    case 2: v.cx = hw; v.cy = hh; break;                        /* top left to view top left */
    case 3: v.cx = (double)v.dw - hw; v.cy = (double)v.dh - hh; break;
    case 4: v.cx = 0.0; v.cy = 0.0; break;                      /* Ctrl+Home */
    default: v.cx = (double)v.dw; v.cy = (double)v.dh; break;  /* Ctrl+End */
    }
    gfx_view_clamp(&v, a->overscroll);
    app_doc_set_gview(a, d, &v);
}

bool app_canvas_over(const app *a) { return a->cv.hovered; }

bool app_canvas_pointer_doc(const app *a, double *x, double *y)
{
    app_doc *d = app_active_doc(a);
    gfx_view v;
    if (!d || !a->cv.mouse_in) return false;
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, (double)a->cv.mx, (double)a->cv.my, x, y);
    return true;
}

/* ---- cursors ------------------------------------------------------------------------- */
static SDL_Cursor *make_icon_cursor(ui_icon icon, int size, int hx, int hy)
{
    uint8_t *rgba = (uint8_t *)malloc((size_t)size * (size_t)size * 4u);
    uint8_t *out = (uint8_t *)calloc((size_t)size * (size_t)size, 4u);
    SDL_Surface *s;
    SDL_Cursor *c = NULL;
    if (!rgba || !out) { free(rgba); free(out); return NULL; }
    if (ui_icon_raster_rgba(icon, size, ui_rgba(16, 16, 16, 255), ui_rgba(255, 255, 255, 255),
                            ui_rgba(255, 255, 255, 200), rgba) != PC_OK) {
        free(rgba);
        free(out);
        return NULL;
    }
    /* white halo around the dark strokes so the cursor shows on any image */
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint8_t amax = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
                    if (rgba[((size_t)yy * (size_t)size + (size_t)xx) * 4u + 3u] > amax)
                        amax = rgba[((size_t)yy * (size_t)size + (size_t)xx) * 4u + 3u];
                }
            {
                uint8_t *o = out + ((size_t)y * (size_t)size + (size_t)x) * 4u;
                const uint8_t *i = rgba + ((size_t)y * (size_t)size + (size_t)x) * 4u;
                uint32_t ia = i[3], ha = amax;
                /* composite icon over a white halo */
                uint32_t oa = ia + ha * (255u - ia) / 255u;
                o[3] = (uint8_t)oa;
                if (oa) {
                    for (int k = 0; k < 3; k++)
                        o[k] = (uint8_t)(((uint32_t)i[k] * ia + 255u * ha * (255u - ia) / 255u) /
                                         oa);
                }
            }
        }
    s = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, out, size * 4);
    if (s) {
        c = SDL_CreateColorCursor(s, hx, hy);
        SDL_DestroySurface(s);
    }
    free(rgba);
    free(out);
    return c;
}

static SDL_Cursor *cursor_for(app *a, app_cursor k)
{
    int size;
    float ds;
    if (!a->win || (int)k < 0 || k >= APP_CURSOR_COUNT) return NULL;
    if (a->cv.cursors[k]) return a->cv.cursors[k];
    ds = SDL_GetWindowDisplayScale(a->win);
    size = ds > 1.4f ? 32 : 24;
    switch (k) {
    case APP_CURSOR_HAND:
    case APP_CURSOR_GRAB:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_PAN, size, size / 2, size / 2);
        break;
    case APP_CURSOR_ZOOM_IN:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_ZOOM_IN, size, size * 5 / 12, size * 5 / 12);
        break;
    case APP_CURSOR_ZOOM_OUT:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_ZOOM_OUT, size, size * 5 / 12, size * 5 / 12);
        break;
    case APP_CURSOR_PENCIL:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_PENCIL, size, 1, size - 2);
        break;
    case APP_CURSOR_PICKER:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_COLOR_PICKER, size, 1, size - 2);
        break;
    case APP_CURSOR_BUCKET:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_PAINT_BUCKET, size, 2, size - 3);
        break;
    /* lane A: selection and move tool cursors */
    case APP_CURSOR_ROTATE:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_ROTATE_CW, size, size / 2, size / 2);
        break;
    case APP_CURSOR_LASSO:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_LASSO_SELECT, size, size * 2 / 16,
                                            size * 15 / 16 - 1);
        break;
    case APP_CURSOR_WAND:
        a->cv.cursors[k] = make_icon_cursor(UI_ICON_TOOL_MAGIC_WAND, size, size * 12 / 16,
                                            size * 4 / 16);
        break;
    case APP_CURSOR_ARROW:
        a->cv.cursors[k] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        break;
    case APP_CURSOR_TEXT:
        a->cv.cursors[k] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        break;
    case APP_CURSOR_MOVE:
        a->cv.cursors[k] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
        break;
    case APP_CURSOR_NOT_ALLOWED:
        a->cv.cursors[k] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
        break;
    default:
        a->cv.cursors[k] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        break;
    }
    return a->cv.cursors[k];
}

void app_canvas_apply_cursor(app *a)
{
    app_cursor k = a->cv.cursor;
    if (!a->win || ui_get_cursor(a->ui) != UI_CURSOR_APP) {
        a->cv.applied = APP_CURSOR_COUNT;
        return;
    }
    if (k == a->cv.applied) return;
    if (k == APP_CURSOR_HIDDEN) {
        SDL_HideCursor();
    } else {
        SDL_Cursor *c = cursor_for(a, k);
        if (a->cv.applied == APP_CURSOR_HIDDEN) SDL_ShowCursor();
        if (c) SDL_SetCursor(c);
    }
    a->cv.applied = k;
}

/* ---- lifetime ------------------------------------------------------------------------ */
bool app_canvas_init(app *a)
{
    memset(&a->cv, 0, sizeof a->cv);
    a->cv.gfx = gfx_canvas_create(a->ren, 0);
    a->cv.applied = APP_CURSOR_COUNT;
    a->cv.pen_pressure = 1.0f;
    return a->cv.gfx != NULL;
}

void app_canvas_free(app *a)
{
    gfx_canvas_destroy(a->cv.gfx);
    a->cv.gfx = NULL;
    for (int i = 0; i < APP_CURSOR_COUNT; i++)
        if (a->cv.cursors[i]) SDL_DestroyCursor(a->cv.cursors[i]);
    memset(a->cv.cursors, 0, sizeof a->cv.cursors);
}

void app_canvas_reset_doc(app *a)
{
    if (a->cv.captured) app_canvas_lost_capture(a);
    a->cv.gfx_doc = NULL;
    app_request_frame(a);
}

/* ---- event intake -------------------------------------------------------------------- */
static void push_ev(app *a, const app_qev *e)
{
    app_canvas *c = &a->cv;
    if (c->nq >= APP_PTR_QUEUE) {
        /* full: keep downs and ups, coalesce motion into the last slot */
        if (e->kind != QEV_MOVE || c->q[c->nq - 1].kind != QEV_MOVE) return;
        c->q[c->nq - 1] = *e;
        return;
    }
    if (e->kind == QEV_MOVE && c->nq > 0 && c->q[c->nq - 1].kind == QEV_MOVE && !c->captured &&
        !c->pen_down)
        c->q[c->nq - 1] = *e;         /* hover motion: only the latest matters */
    else
        c->q[c->nq++] = *e;
    app_request_frame(a);
}

static uint32_t cur_mods(const app *a) { return ui_mods(a->ui); }

static float ppp(const app *a) { return a->fi.px_per_point > 0.0f ? a->fi.px_per_point : 1.0f; }

static int map_btn(Uint8 b)
{
    if (b == SDL_BUTTON_LEFT) return APP_BTN_LEFT;
    if (b == SDL_BUTTON_RIGHT) return APP_BTN_RIGHT;
    if (b == SDL_BUTTON_MIDDLE) return APP_BTN_MIDDLE;
    return -1;
}

/* ---- two-finger pinch zoom (V-ZOOM-PINCH, lane SHELL) -------------------------------------
 * SDL 3.2 finger events. Touch screens (direct devices) report positions
 * normalized to the window: the image point under the two fingers'
 * centroid follows the centroid while the zoom follows the finger
 * distance, so pinching zooms around the gesture and moving both fingers
 * pans. Touchpads that report fingers (indirect devices, macOS trackpads)
 * report positions on the pad: the gesture zooms around the pointer once
 * the finger distance changed clearly before the fingers moved together
 * (otherwise it is a two-finger scroll, which arrives as wheel events).
 * Only the SDL 3.2 API is used (cmake/PcDeps.cmake): touchpads whose
 * pinch the platform only reports as a gesture (libinput on X11 and
 * Wayland) are not covered; Windows turns touchpad pinches into
 * Ctrl+wheel, which zooms through the presets. */
static int finger_find(const shell_cv *s, SDL_TouchID t, SDL_FingerID f)
{
    for (int i = 0; i < s->nfing; i++)
        if (s->fing[i].touch == t && s->fing[i].id == f) return i;
    return -1;
}

static bool touch_direct(SDL_TouchID t)
{
    SDL_TouchDeviceType k = SDL_GetTouchDeviceType(t);
    /* unknown devices (synthetic events) are treated as touch screens */
    return k != SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE && k != SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

/* The first two fingers of the gesture device: distance and centroid. */
static bool pinch_geom(const shell_cv *s, const app *a, double *dist, double *cx, double *cy)
{
    const shell_finger *f0 = NULL, *f1 = NULL;
    double w = s->pinch_direct ? (double)(a->fi.width > 0 ? a->fi.width : 1) : 1.0;
    double h = s->pinch_direct ? (double)(a->fi.height > 0 ? a->fi.height : 1) : 1.0;
    double dx, dy;
    for (int i = 0; i < s->nfing; i++) {
        if (s->fing[i].touch != s->pinch_touch) continue;
        if (!f0) f0 = &s->fing[i];
        else if (!f1) f1 = &s->fing[i];
    }
    if (!f0 || !f1) return false;
    dx = ((double)f1->x - (double)f0->x) * w;
    dy = ((double)f1->y - (double)f0->y) * h;
    *dist = sqrt(dx * dx + dy * dy);
    *cx = ((double)f0->x + (double)f1->x) * 0.5 * w;
    *cy = ((double)f0->y + (double)f1->y) * 0.5 * h;
    return *dist > 1e-6;
}

static void touch_undo(app *a, app_doc *d, uint64_t seq)
{
    pc_hist *h = d->hist;
    size_t depth = 0;
    const pc_hist_node *n;
    if (!h || d->txn || d->doc->open_txns || h->cur->seq == seq || !h->cur->parent ||
        h->cur->parent->seq != seq)
        return;
    if (!pc_hist_undo(h)) return;
    for (n = h->cur; n->parent; n = n->parent) depth++;
    pc_hist_prune(h, depth + 1u);                /* the undone step is not redoable */
    app_doc_history_changed(a, d);
}

static void pinch_begin(app *a, shell_cv *s)
{
    app_doc *d = app_active_doc(a);
    double dist, cx, cy;
    gfx_view v;
    s->pinching = false;
    if (!d || app_dialog_active(a) || !pinch_geom(s, a, &dist, &cx, &cy)) return;
    v = app_doc_gview(a, d);
    if (s->pinch_direct) {
        /* the gesture must start over the image view */
        if (cx < (double)v.vx || cy < (double)v.vy || cx >= (double)(v.vx + v.vw) ||
            cy >= (double)(v.vy + v.vh))
            return;
        s->p_docx = v.cx + (cx - ((double)v.vx + (double)v.vw * 0.5)) / v.zoom;
        s->p_docy = v.cy + (cy - ((double)v.vy + (double)v.vh * 0.5)) / v.zoom;
        /* a touch that started a tool drag becomes a gesture: the tool
         * gets the usual cancel, and a history step the touch made (a
         * stroke ends by committing what it painted) is undone and
         * dropped, so the gesture leaves no trace */
        if (a->cv.captured) {
            app_canvas_lost_capture(a);
            if (s->touch_press && s->touch_doc == d->id) touch_undo(a, d, s->touch_seq);
        }
        s->touch_block = true;
        s->pinch_mode = 1;
    } else {
        if (!a->cv.mouse_in || !app_canvas_over(a)) return;
        s->pinch_mode = 0;
    }
    s->pinching = true;
    s->p_d0 = dist;
    s->p_cx0 = cx;
    s->p_cy0 = cy;
    s->p_z0 = v.zoom;
}

static void pinch_update(app *a, shell_cv *s)
{
    app_doc *d = app_active_doc(a);
    double dist, cx, cy, z;
    gfx_view v;
    if (!s->pinching || !d || !pinch_geom(s, a, &dist, &cx, &cy)) return;
    if (s->pinch_mode == 0) {
        /* touchpad: decide between pinch and two-finger scroll */
        double ds = fabs(log(dist / s->p_d0));
        double mv = hypot(cx - s->p_cx0, cy - s->p_cy0);
        if (ds > 0.08 && ds > mv * 1.5) s->pinch_mode = 1;
        else if (mv > 0.05) s->pinch_mode = 2;
        if (s->pinch_mode == 1) {         /* restart from here: no jump */
            s->p_d0 = dist;
            s->p_z0 = d->view.zoom;
        }
        return;
    }
    if (s->pinch_mode != 1) return;
    z = gfx_zoom_clamp(s->p_z0 * dist / s->p_d0);
    v = app_doc_gview(a, d);
    if (s->pinch_direct) {
        v.zoom = z;
        v.cx = s->p_docx - (cx - ((double)v.vx + (double)v.vw * 0.5)) / z;
        v.cy = s->p_docy - (cy - ((double)v.vy + (double)v.vh * 0.5)) / z;
        gfx_view_clamp(&v, a->overscroll);
    } else {
        gfx_view_zoom_at(&v, z, (double)a->cv.mx, (double)a->cv.my, a->overscroll);
    }
    d->view.fit_mode = false;
    app_doc_set_gview(a, d, &v);
}

static void touch_event(app *a, const SDL_Event *e)
{
    shell_cv *s = scv(a);
    const SDL_TouchFingerEvent *t = &e->tfinger;
    int i;
    if (!s || t->touchID == SDL_MOUSE_TOUCHID) return;
    i = finger_find(s, t->touchID, t->fingerID);
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (i < 0 && s->nfing < SHELL_MAX_FINGERS) {
            i = s->nfing++;
            s->fing[i].touch = t->touchID;
            s->fing[i].id = t->fingerID;
        }
        if (i < 0) return;
        s->fing[i].x = t->x;
        s->fing[i].y = t->y;
        if (!s->pinching) {
            int same = 0;
            for (int k = 0; k < s->nfing; k++) same += s->fing[k].touch == t->touchID;
            if (same == 2) {
                s->pinch_touch = t->touchID;
                s->pinch_direct = touch_direct(t->touchID);
                pinch_begin(a, s);
            }
        }
    } else if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (i < 0) return;
        s->fing[i].x = t->x;
        s->fing[i].y = t->y;
        if (s->pinching && t->touchID == s->pinch_touch) pinch_update(a, s);
    } else {                                  /* up or canceled */
        if (i < 0) return;
        s->fing[i] = s->fing[--s->nfing];
        if (s->pinching && t->touchID == s->pinch_touch) s->pinching = false;
        if (s->nfing == 0) s->touch_block = false;
    }
    app_request_frame(a);
}

void app_canvas_event(app *a, const SDL_Event *e)
{
    app_canvas *c = &a->cv;
    app_qev q;
    SDL_Event m_mouse;                   /* lane M: pen as mouse */
    memset(&q, 0, sizeof q);
    q.pressure = 1.0f;
    /* lane SHELL: touch gestures; while two fingers pinch, the mouse
     * events SDL synthesizes from the touch screen are not for the tools */
    if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_UP ||
        e->type == SDL_EVENT_FINGER_MOTION || e->type == SDL_EVENT_FINGER_CANCELED) {
        touch_event(a, e);
        return;
    }
    if (e->type == SDL_EVENT_PEN_DOWN || e->type == SDL_EVENT_PEN_MOTION ||
        e->type == SDL_EVENT_PEN_PROXIMITY_IN) {
        shell_cv *s = scv(a);
        if (s) s->pen_seen = true;
    }
    if ((e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == SDL_TOUCH_MOUSEID) ||
        ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
         e->button.which == SDL_TOUCH_MOUSEID)) {
        shell_cv *s = scv(a);
        if (s && (s->touch_block || s->pinching)) return;
        if (s && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            app_doc *d = app_active_doc(a);
            s->touch_press = true;
            s->touch_doc = d ? d->id : 0u;
            s->touch_seq = d && d->hist ? d->hist->cur->seq : 0u;
        }
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_PEN_DOWN) {
        shell_cv *s = scv(a);
        if (s) s->touch_press = false;
    }
    /* lane M: Settings > Pen & Tablet off: pen events are ignored and the
     * mouse events SDL synthesizes from pens are used like a mouse */
    if (a->m_pen_off) {
        if (e->type == SDL_EVENT_PEN_AXIS || e->type == SDL_EVENT_PEN_MOTION ||
            e->type == SDL_EVENT_PEN_DOWN || e->type == SDL_EVENT_PEN_UP)
            return;
        if ((e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == SDL_PEN_MOUSEID) ||
            ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
             e->button.which == SDL_PEN_MOUSEID)) {
            m_mouse = *e;
            if (e->type == SDL_EVENT_MOUSE_MOTION) m_mouse.motion.which = 0;
            else m_mouse.button.which = 0;
            e = &m_mouse;
        }
    }
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        c->mx = e->motion.x * ppp(a);
        c->my = e->motion.y * ppp(a);
        c->mouse_in = true;
        c->hover_pending = true;
        if (e->motion.which == SDL_PEN_MOUSEID) break;
        q.kind = QEV_MOVE;
        q.x = c->mx;
        q.y = c->my;
        q.mods = cur_mods(a);
        q.ts = e->motion.timestamp;
        push_ev(a, &q);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        c->mx = e->button.x * ppp(a);
        c->my = e->button.y * ppp(a);
        c->mouse_in = true;
        if (e->button.which == SDL_PEN_MOUSEID) break;
        q.button = map_btn(e->button.button);
        if (q.button < 0) break;
        q.kind = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? QEV_DOWN : QEV_UP;
        q.x = c->mx;
        q.y = c->my;
        q.clicks = e->button.clicks;
        q.mods = cur_mods(a);
        q.ts = e->button.timestamp;
        push_ev(a, &q);
        break;
    case SDL_EVENT_PEN_AXIS:
        if (e->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
            float pv = e->paxis.value;
            c->pen_pressure = pv < 0.0f ? 0.0f : (pv > 1.0f ? 1.0f : pv);
            if (c->pen_down) {
                q.kind = QEV_MOVE;
                q.x = e->paxis.x * ppp(a);
                q.y = e->paxis.y * ppp(a);
                q.pen = true;
                q.eraser = c->pen_eraser;
                q.pressure = c->pen_pressure;
                q.mods = cur_mods(a);
                q.ts = e->paxis.timestamp;
                push_ev(a, &q);
            }
        }
        break;
    case SDL_EVENT_PEN_MOTION:
        c->mx = e->pmotion.x * ppp(a);
        c->my = e->pmotion.y * ppp(a);
        c->mouse_in = true;
        c->hover_pending = true;
        q.kind = QEV_MOVE;
        q.x = c->mx;
        q.y = c->my;
        q.pen = true;
        q.eraser = c->pen_eraser;
        q.pressure = c->pen_down ? c->pen_pressure : 1.0f;
        q.mods = cur_mods(a);
        q.ts = e->pmotion.timestamp;
        push_ev(a, &q);
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        c->mx = e->ptouch.x * ppp(a);
        c->my = e->ptouch.y * ppp(a);
        c->pen_eraser = e->ptouch.eraser;
        c->pen_down = e->type == SDL_EVENT_PEN_DOWN;
        q.kind = c->pen_down ? QEV_DOWN : QEV_UP;
        q.button = APP_BTN_LEFT;
        q.x = c->mx;
        q.y = c->my;
        q.clicks = 1;
        q.pen = true;
        q.eraser = e->ptouch.eraser;
        q.pressure = c->pen_down ? c->pen_pressure : 0.0f;
        q.mods = cur_mods(a);
        q.ts = e->ptouch.timestamp;
        push_ev(a, &q);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.key == SDLK_SPACE) {
            const app_tool *t = app_tool_current(a);
            bool allow = !ui_text_input_active(a->ui) && !app_dialog_active(a) &&
                         !(t && (t->flags & APP_TOOL_NO_SPACE_PAN));
            bool down = e->type == SDL_EVENT_KEY_DOWN;
            if (!down || allow) {
                if (c->space_down != down) app_request_frame(a);
                c->space_down = down;
            }
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        c->mouse_in = false;
        app_request_frame(a);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        c->space_down = false;
        if (c->captured) app_canvas_lost_capture(a);
        break;
    case SDL_EVENT_WINDOW_ICCPROF_CHANGED:      /* lane SHELL: display color management */
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        app_cm_display_changed(a);
        break;
    default:
        break;
    }
}

/* ---- dispatch to tools --------------------------------------------------------------- */
static void send(app *a, app_ptr_kind kind, const app_qev *q, int button)
{
    app_doc *d = app_active_doc(a);
    app_pointer p;
    gfx_view v;
    if (!d) return;
    memset(&p, 0, sizeof p);
    v = app_doc_gview(a, d);
    p.kind = kind;
    gfx_view_to_doc(&v, (double)q->x, (double)q->y, &p.x, &p.y);
    p.sx = q->x;
    p.sy = q->y;
    p.button = button;
    p.buttons = a->cv.buttons;
    p.pressure = q->pressure;
    p.pen = q->pen;
    p.eraser = q->eraser;
    p.mods = q->mods;
    p.clicks = q->clicks;
    p.time_ns = q->ts;
    app_tool_dispatch(a, &p);
}

void app_canvas_lost_capture(app *a)
{
    app_canvas *c = &a->cv;
    if (!c->captured) return;
    if (!c->panning) {
        app_qev q;
        memset(&q, 0, sizeof q);
        q.x = c->mx;
        q.y = c->my;
        q.pressure = 1.0f;
        q.mods = cur_mods(a);
        c->buttons = 0;
        send(a, APP_PTR_CANCEL, &q, c->first_button);
    }
    c->captured = false;
    c->panning = false;
    c->buttons = 0;
    app_request_frame(a);
}

static void pan_by(app *a, float x, float y)
{
    app_doc *d = app_active_doc(a);
    if (d) app_view_pan_px(a, d, (double)(x - a->cv.pan_x), (double)(y - a->cv.pan_y));
    a->cv.pan_x = x;
    a->cv.pan_y = y;
}

static void process_queue(app *a, const ui_interaction *in)
{
    app_canvas *c = &a->cv;
    bool left_taken = false;
    for (int i = 0; i < c->nq; i++) {
        const app_qev *q = &c->q[i];
        uint32_t bit = q->button >= 0 ? 1u << q->button : 0u;
        switch (q->kind) {
        case QEV_DOWN:
            if (!c->captured) {
                bool accept;
                if (app_dialog_active(a) || !app_active_doc(a)) break;
                if (q->button == APP_BTN_LEFT)
                    accept = in->pressed || (left_taken && in->hovered);
                else
                    accept = in->hovered && c->hovered_prev;
                if (!accept) break;
                if (q->button == APP_BTN_LEFT) left_taken = true;
                c->captured = true;
                c->first_button = q->button;
                c->buttons = bit;
                if (q->button == APP_BTN_MIDDLE || (q->button == APP_BTN_LEFT && c->space_down)) {
                    c->panning = true;
                    c->pan_x = q->x;
                    c->pan_y = q->y;
                } else {
                    send(a, APP_PTR_DOWN, q, q->button);
                }
            } else if (!(c->buttons & bit)) {
                c->buttons |= bit;
                if (!c->panning) send(a, APP_PTR_DOWN, q, q->button);
            }
            break;
        case QEV_MOVE:
            if (c->captured) {
                if (c->panning) pan_by(a, q->x, q->y);
                else send(a, APP_PTR_MOVE, q, c->first_button);
            }
            break;
        case QEV_UP:
            if (!c->captured || !(c->buttons & bit)) break;
            c->buttons &= ~bit;
            if (c->panning) {
                pan_by(a, q->x, q->y);
            } else {
                send(a, APP_PTR_UP, q, q->button);
            }
            if (c->buttons == 0u) {
                c->captured = false;
                c->panning = false;
            }
            break;
        }
    }
    c->nq = 0;
}

/* ---- auto-scroll (V-AUTOSCROLL, lane SHELL) -------------------------------------------------
 * While a tool drags (not a pan, not the Pan tool, as in 3.36) and the
 * pointer is at an edge of the view or beyond it, the view scrolls toward
 * that side by elapsed time, faster the further out the pointer is. It
 * never scrolls into the overscroll area: an axis moves only toward the
 * limit the view has without overscroll and stops there. After a step the
 * tool receives a move at the same screen position, which is now another
 * image position, so selections and strokes follow. */
static double as_speed(double depth)
{
    double v = AS_MIN_SPEED + AS_GAIN * depth;
    return v > AS_MAX_SPEED ? AS_MAX_SPEED : v;
}

static void auto_scroll(app *a, app_doc *d)
{
    app_canvas *c = &a->cv;
    shell_cv *s = scv(a);
    const app_tool *t = app_tool_current(a);
    double edge, sx = 0.0, sy = 0.0, dt, x0, x1, y0, y1, ncx, ncy, depth;
    uint64_t now;
    gfx_view v;
    if (!s) return;
    if (!s->autoscroll || !c->captured || c->panning || !d || app_dialog_active(a) ||
        (t && strcmp(t->id, "pan") == 0)) {
        s->as_ns = 0;
        return;
    }
    edge = (double)ui_px(a->ui, AS_EDGE_DIP);
    depth = (double)c->view.x + edge - (double)c->mx;
    if (depth > 0.0) sx = -as_speed(depth);
    depth = (double)c->mx - ((double)(c->view.x + c->view.w) - edge);
    if (depth > 0.0) sx = as_speed(depth);
    depth = (double)c->view.y + edge - (double)c->my;
    if (depth > 0.0) sy = -as_speed(depth);
    depth = (double)c->my - ((double)(c->view.y + c->view.h) - edge);
    if (depth > 0.0) sy = as_speed(depth);
    if (sx == 0.0 && sy == 0.0) {
        s->as_ns = 0;
        return;
    }
    now = SDL_GetTicksNS();
    if (s->as_ns == 0u || now <= s->as_ns) {
        /* the first frame at the edge only starts the clock */
        s->as_ns = now;
        app_request_frame_at(a, a->now + 8u);
        return;
    }
    dt = (double)(now - s->as_ns) * 1e-9;
    if (dt > 0.05) dt = 0.05;                 /* a stalled frame does not jump */
    s->as_ns = now;
    v = app_doc_gview(a, d);
    gfx_view_range(&v, false, &x0, &x1, &y0, &y1);
    ncx = v.cx + sx * dt / v.zoom;
    ncy = v.cy + sy * dt / v.zoom;
    if (sx > 0.0) ncx = v.cx < x1 ? (ncx < x1 ? ncx : x1) : v.cx;
    else if (sx < 0.0) ncx = v.cx > x0 ? (ncx > x0 ? ncx : x0) : v.cx;
    if (sy > 0.0) ncy = v.cy < y1 ? (ncy < y1 ? ncy : y1) : v.cy;
    else if (sy < 0.0) ncy = v.cy > y0 ? (ncy > y0 ? ncy : y0) : v.cy;
    if (ncx != v.cx || ncy != v.cy) {
        app_qev q;
        v.cx = ncx;
        v.cy = ncy;
        gfx_view_clamp(&v, a->overscroll);
        d->view.fit_mode = false;
        app_doc_set_gview(a, d, &v);
        memset(&q, 0, sizeof q);
        q.kind = QEV_MOVE;
        q.x = c->mx;
        q.y = c->my;
        q.pen = c->pen_down;
        q.eraser = c->pen_down && c->pen_eraser;
        q.pressure = c->pen_down ? c->pen_pressure : 1.0f;
        q.mods = cur_mods(a);
        q.ts = now;
        send(a, APP_PTR_MOVE, &q, c->first_button);
    }
    app_request_frame_at(a, a->now + 8u);
}

/* ---- Space + arrows (V-PAN-KEYS, lane SHELL) ------------------------------------------------
 * Taken from the frame's key presses so that tools (which would nudge
 * with the arrows) and shortcuts never see them while Space is down. */
static void key_pan(app *a, app_doc *d)
{
    static const struct { int32_t key; int dx, dy; } k[4] = {
        { SDLK_LEFT, -1, 0 }, { SDLK_RIGHT, 1, 0 }, { SDLK_UP, 0, -1 }, { SDLK_DOWN, 0, 1 }
    };
    uint32_t m = 0;
    if (!a->cv.space_down || !d) return;
    for (int i = 0; i < 4; i++)
        while (ui_key_take_any(a->ui, k[i].key, &m))
            app_view_key_pan(a, d, k[i].dx, k[i].dy, (m & (UI_MOD_CTRL | UI_MOD_GUI)) != 0u);
}

/* ---- first presentation (V-NOFLICKER, lane SHELL) -------------------------------------------
 * A newly active image is shown only once every tile of the view is in
 * the display cache: the first frame gets a larger budget, and until the
 * visible tiles exist the canvas shows the empty workspace (no
 * checkerboard flash, no half drawn image). A safety limit of two seconds
 * stops the wait for views that cannot be cached at once. */
static bool first_ready(app *a, app_doc *d, const gfx_view *v, uint64_t budget_ns)
{
    uint32_t level = gfx_view_level(v->zoom);
    pc_rect lr = gfx_view_level_rect(v, level);
    pc_comp_opts o;
    uint64_t t0 = SDL_GetTicksNS();
    int32_t band = 2 * (int32_t)PC_TILE_DIM;
    if (gfx_view_ready(v, d->vcache)) return true;
    if (pc_rect_is_empty(lr)) return true;
    o = app_doc_comp_opts(d);
    for (int32_t y = lr.y & ~((int32_t)PC_TILE_DIM - 1); y < lr.y + lr.h; y += band) {
        pc_rect r = pc_rect_intersect(lr, pc_rect_make(lr.x, y, lr.w, band));
        if (pc_rect_is_empty(r)) continue;
        if (pc_view_cache_update(d->vcache, d->doc, &o, level, r, &a->par) != PC_OK) break;
        if (SDL_GetTicksNS() - t0 > budget_ns) break;
    }
    return gfx_view_ready(v, d->vcache);
}

static bool first_hold(app *a, app_doc *d, const gfx_view *v)
{
    shell_cv *s = scv(a);
    if (!s) return false;
    if (s->shown_doc != d->id) {
        s->shown_doc = d->id;
        s->shown_ok = false;
        s->shown_ms = a->now;
    }
    if (s->shown_ok) return false;
    if (first_ready(a, d, v, s->first_budget_ns ? s->first_budget_ns : FIRST_BUDGET_NS) ||
        a->now - s->shown_ms > 2000u) {
        s->shown_ok = true;
        return false;
    }
    app_request_frame(a);
    return true;
}

/* ---- marching ants timing (V-SEL-ANTS, lane SHELL) ------------------------------------------ */
float app_canvas_ants_hz(app *a)
{
    float hz = 0.0f;
    if (a->win) {
        SDL_DisplayID id = SDL_GetDisplayForWindow(a->win);
        const SDL_DisplayMode *m = id ? SDL_GetCurrentDisplayMode(id) : NULL;
        if (m) hz = m->refresh_rate;
    }
    if (!(hz >= 20.0f)) hz = 60.0f;
    if (hz > 500.0f) hz = 500.0f;
    return hz;
}

bool app_canvas_ants_paused(app *a)
{
    shell_cv *s = scv(a);
    if (!a->focused) return true;
    if (!s) return false;
    if (s->power_force >= 0) return s->power_force == 1;
    if (s->power_ms == 0u || a->now - s->power_ms > 10000u) {
        int pct = -1;
        SDL_PowerState ps = SDL_GetPowerInfo(NULL, &pct);
        s->power_saver = ps == SDL_POWERSTATE_ON_BATTERY && pct >= 0 && pct <= 20;
        s->power_ms = a->now ? a->now : 1u;
    }
    return s->power_saver;
}

/* The dash offset in screen pixels: one pixel per 60 ms like 3.36, but
 * continuous, so every frame at the refresh rate moves the dashes (drawn
 * with antialiased ends, gfx_draw_ants). Frozen while paused. */
static double ants_phase(app *a)
{
    shell_cv *s = scv(a);
    if (!s) return 0.0;
    if (!app_canvas_ants_paused(a)) {
        if (s->ants_ms && a->now > s->ants_ms)
            s->ants_phase += (double)(a->now - s->ants_ms) / (double)ANTS_STEP_MS;
        s->ants_ms = a->now;
    } else {
        s->ants_ms = 0;
    }
    s->ants_phase = fmod(s->ants_phase, 4096.0);
    return s->ants_phase;
}

/* ---- scroll bars --------------------------------------------------------------------- */
static void scrollbar(app *a, app_doc *d, ui_rect r, bool horiz)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    gfx_view v = app_doc_gview(a, d);
    double content, visible, pos, track, tlen, tpos;
    int32_t minlen = ui_px(ui, 24.0f), pad = ui_px(ui, 3.0f);
    ui_rect thumb;
    ui_interaction tin, bin;
    if (ui_rect_empty(r)) return;
    if (!gfx_view_scrollbar(&v, a->overscroll, horiz, &content, &visible, &pos)) return;
    ui_draw_rect(ui, r, ui_color_fade(p->panel, 0.82f));
    track = horiz ? (double)(r.w - 2 * pad) : (double)(r.h - 2 * pad);
    tlen = track * visible / content;
    if (tlen < (double)minlen) tlen = (double)minlen;
    if (tlen > track) tlen = track;
    tpos = content > visible ? (track - tlen) * pos / (content - visible) : 0.0;
    if (horiz)
        thumb = ui_rect_make(r.x + pad + (int32_t)tpos, r.y + pad, (int32_t)tlen, r.h - 2 * pad);
    else
        thumb = ui_rect_make(r.x + pad, r.y + pad + (int32_t)tpos, r.w - 2 * pad, (int32_t)tlen);
    bin = ui_interact(ui, ui_get_id(ui, horiz ? "##hbar_track" : "##vbar_track"), r,
                      UI_INTERACT_OVERLAP);
    tin = ui_interact(ui, ui_get_id(ui, horiz ? "##hbar_thumb" : "##vbar_thumb"), thumb, 0);
    if (tin.held && track > tlen) {
        static double press_pos[2];       /* scroll position at the press, per axis */
        double delta = horiz ? (double)(tin.mouse.x - tin.press_pos.x)
                             : (double)(tin.mouse.y - tin.press_pos.y);
        if (tin.pressed) press_pos[horiz] = pos;
        gfx_view_set_scroll(&v, a->overscroll, horiz,
                            press_pos[horiz] + delta * (content - visible) / (track - tlen));
        app_doc_set_gview(a, d, &v);
    } else if (bin.pressed) {
        /* page towards the click */
        double m = horiz ? (double)(bin.mouse.x - (float)r.x) : (double)(bin.mouse.y - (float)r.y);
        double dir = m < tpos + (double)pad ? -1.0 : 1.0;
        gfx_view_set_scroll(&v, a->overscroll, horiz, pos + dir * visible * 0.9);
        app_doc_set_gview(a, d, &v);
    }
    ui_draw_rrect(ui, thumb, (float)(horiz ? thumb.h : thumb.w) * 0.5f,
                  tin.held || tin.hovered ? p->scrollbar_hover : p->scrollbar);
}

/* ---- rulers -------------------------------------------------------------------------- */
static double unit_px(const app *a, const app_doc *d)
{
    double dpi = d && d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    if (a->units == APP_UNITS_IN) return dpi;
    if (a->units == APP_UNITS_CM) return dpi / 2.54;
    return 1.0;
}

static void ruler(app *a, app_doc *d, ui_rect r, bool horiz)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    gfx_view v = app_doc_gview(a, d);
    double ox, oy, upx = unit_px(a, d), scr_per_unit, step = 1.0, s0, s1;
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui) * 0.92f;
    int32_t minor_n = 10;
    static const double mult[] = { 1.0, 2.0, 5.0 };
    if (ui_rect_empty(r)) return;
    ui_draw_rect(ui, r, p->panel);
    ui_draw_rect(ui, horiz ? ui_rect_make(r.x, r.y + r.h - 1, r.w, 1)
                           : ui_rect_make(r.x + r.w - 1, r.y, 1, r.h), p->border);
    gfx_view_origin(&v, &ox, &oy);
    scr_per_unit = v.zoom * upx;
    /* label spacing of at least 60 DIPs: steps 1, 2, 5 times powers of 10 */
    {
        double want = (double)ui_px(ui, 60.0f);
        double base = 0.001;
        bool done = false;
        while (!done && base < 1e9) {
            for (int i = 0; i < 3; i++) {
                if (base * mult[i] * scr_per_unit >= want) {
                    step = base * mult[i];
                    minor_n = i == 1 ? 4 : (i == 2 ? 5 : 10);
                    done = true;
                    break;
                }
            }
            base *= 10.0;
        }
    }
    ui_push_clip(ui, r);
    s0 = horiz ? (double)r.x : (double)r.y;
    s1 = horiz ? (double)(r.x + r.w) : (double)(r.y + r.h);
    {
        double o = horiz ? ox : oy;
        double u0 = (s0 - o) / scr_per_unit, u1 = (s1 - o) / scr_per_unit;
        double first = floor(u0 / step) * step;
        double minor = step / (double)minor_n;
        for (double u = first; u <= u1 + step; u += step) {
            for (int k = 0; k < minor_n; k++) {
                double uu = u + minor * (double)k;
                double s = o + uu * scr_per_unit;
                int32_t len = k == 0 ? (horiz ? r.h : r.w) * 2 / 3
                                     : (k * 2 == minor_n ? (horiz ? r.h : r.w) / 3
                                                         : (horiz ? r.h : r.w) / 5);
                if (s < s0 - 1.0 || s > s1) continue;
                if (horiz)
                    ui_draw_rect(ui, ui_rect_make((int32_t)floor(s), r.y + r.h - len, 1, len),
                                 p->text_dim);
                else
                    ui_draw_rect(ui, ui_rect_make(r.x + r.w - len, (int32_t)floor(s), len, 1),
                                 p->text_dim);
                if (k == 0) {
                    char lbl[32];
                    double val = fabs(uu) < step * 1e-6 ? 0.0 : uu;
                    if (step >= 1.0) snprintf(lbl, sizeof lbl, "%.0f", val);
                    else if (step >= 0.1) snprintf(lbl, sizeof lbl, "%.1f", val);
                    else snprintf(lbl, sizeof lbl, "%.2f", val);
                    if (horiz) {
                        ui_draw_text(ui, ui_font_regular(ui), fs, (float)floor(s) + 3.0f,
                                     (float)r.y + fs, p->text_dim, lbl, strlen(lbl));
                    } else {
                        /* vertical ruler: label to the right of its tick, top aligned */
                        ui_draw_text(ui, ui_font_regular(ui), fs, (float)r.x + 2.0f,
                                     (float)floor(s) + fs + 1.0f, p->text_dim, lbl, strlen(lbl));
                    }
                }
            }
        }
        /* selection range (V-RULER-SEL) and pointer marker (V-RULER-CURSOR) */
        if (pc_sel_is_active(d->doc)) {
            pc_rect b = pc_sel_bounds(d->doc);
            double a0 = o + (horiz ? (double)b.x : (double)b.y) * v.zoom;
            double a1 = o + (horiz ? (double)(b.x + b.w) : (double)(b.y + b.h)) * v.zoom;
            ui_color hc = ui_color_fade(p->accent, 0.25f);
            int32_t i0 = (int32_t)a0, il = (int32_t)(a1 - a0);
            if (horiz) ui_draw_rect(ui, ui_rect_make(i0, r.y, il, r.h), hc);
            else ui_draw_rect(ui, ui_rect_make(r.x, i0, r.w, il), hc);
        }
        if (a->cv.mouse_in) {
            int32_t m = (int32_t)(horiz ? a->cv.mx : a->cv.my);
            if (horiz) ui_draw_rect(ui, ui_rect_make(m, r.y, 1, r.h), p->accent);
            else ui_draw_rect(ui, ui_rect_make(r.x, m, r.w, 1), p->accent);
        }
    }
    ui_pop_clip(ui);
}

/* ---- drawing callback ---------------------------------------------------------------- */
typedef struct draw_ctx {
    app       *a;
    gfx_view   v;
    uint32_t   doc_id;      /* looked up at draw time: the frame may close it */
} draw_ctx;

static draw_ctx g_draw;    /* one canvas per app; the callback runs within ui_render */

static void draw_cb(SDL_Renderer *r, ui_rect clip, void *ud)
{
    draw_ctx *dc = (draw_ctx *)ud;
    app *a = dc->a;
    const ui_palette *p = ui_pal(a->ui);
    app_doc *d = NULL;
    gfx_style st;
    (void)r;
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == dc->doc_id) d = a->docs[i];
    memset(&st, 0, sizeof st);
    st.checker_a = gfx_rgba_make(p->checker_a.r, p->checker_a.g, p->checker_a.b, 255);
    st.checker_b = gfx_rgba_make(p->checker_b.r, p->checker_b.g, p->checker_b.b, 255);
    /* lane M: Settings > Canvas checkerboard brightness: 0.75 = the theme
     * colors, lower scales them toward black, higher lifts them toward
     * white (at most a quarter of the way, so the squares stay distinct) */
    if (a->m_cv_checker > 0.0f && (a->m_cv_checker < 0.749f || a->m_cv_checker > 0.751f)) {
        float b = a->m_cv_checker;
        float c[6] = { (float)st.checker_a.r, (float)st.checker_a.g, (float)st.checker_a.b,
                       (float)st.checker_b.r, (float)st.checker_b.g, (float)st.checker_b.b };
        for (int i = 0; i < 6; i++) {
            if (b < 0.75f) c[i] = c[i] * b / 0.75f;
            else c[i] = c[i] + (255.0f - c[i]) * (b - 0.75f);
            if (c[i] > 255.0f) c[i] = 255.0f;
        }
        st.checker_a = gfx_rgba_make((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], 255);
        st.checker_b = gfx_rgba_make((uint8_t)c[3], (uint8_t)c[4], (uint8_t)c[5], 255);
    }
    st.checker_cell = ui_px(a->ui, 8.0f);
    st.grid = a->grid;
    st.grid_color = a->dark ? gfx_rgba_make(255, 255, 255, 56) : gfx_rgba_make(0, 0, 0, 56);
    if (!d) return;
    (void)app_cm_gfx_style(a, d, &st);     /* lane SHELL: V-RENDER-CM */
    gfx_canvas_draw(a->cv.gfx, &dc->v, d->vcache, &st, &a->cv.stats);
    {
        const pc_poly *ants = app_doc_ants(d);
        if (ants && ants->n_contours) {
            gfx_draw_ants(a->ren, &dc->v, ants, ants_phase(a), (double)ui_px(a->ui, 4.0f),
                          pc_rect_make(clip.x, clip.y, clip.w, clip.h));
        }
    }
}

/* ---- the frame ----------------------------------------------------------------------- */
/* The viewport never changes size with the zoom: scroll bars overlay its
 * right and bottom edges, so zooming at the pointer stays anchored and the
 * fitted view does not jump when bars appear. */
static void layout(app *a, app_doc *d, ui_rect area)
{
    ui_ctx *ui = a->ui;
    app_canvas *c = &a->cv;
    int32_t rw = a->rulers ? ui_px(ui, RULER_DIP) : 0, sb = ui_px(ui, SBAR_DIP);
    ui_rect inner = area;
    c->hruler = c->vruler = c->hbar = c->vbar = ui_rect_make(0, 0, 0, 0);
    if (rw) {
        c->hruler = ui_rect_make(inner.x + rw, inner.y, inner.w - rw, rw);
        c->vruler = ui_rect_make(inner.x, inner.y + rw, rw, inner.h - rw);
        inner = ui_rect_make(inner.x + rw, inner.y + rw, inner.w - rw, inner.h - rw);
    }
    c->view = inner;
    if (c->view.w < 1) c->view.w = 1;
    if (c->view.h < 1) c->view.h = 1;
    if (d && !d->view.need_fit) {
        gfx_view v = app_doc_gview(a, d);
        bool hneed = gfx_view_scrollbar(&v, a->overscroll, true, NULL, NULL, NULL);
        bool vneed = gfx_view_scrollbar(&v, a->overscroll, false, NULL, NULL, NULL);
        int32_t hw = inner.w - (vneed ? sb : 0), vh = inner.h - (hneed ? sb : 0);
        if (hneed) c->hbar = ui_rect_make(inner.x, inner.y + inner.h - sb, hw, sb);
        if (vneed) c->vbar = ui_rect_make(inner.x + inner.w - sb, inner.y, sb, vh);
    }
}

static void empty_workspace(app *a, ui_rect area)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    static const char hint[] = "Open an image or drop files here";
    ui_rect r = ui_rect_center(area, area.w, ui_px(ui, 40.0f));
    int32_t ih = ui_px(ui, 48.0f);
    ui_draw_icon(ui, UI_ICON_IMAGE, ui_rect_make(r.x, r.y - ih, r.w, ih), ui_px(ui, 40.0f),
                 p->text_disabled, p->text_disabled);
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), r, UI_ALIGN_CENTER, 0, p->text_dim,
                     hint, sizeof hint - 1u);
}

void app_canvas_frame(app *a, ui_rect area)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    app_canvas *c = &a->cv;
    app_doc *d = app_active_doc(a);
    ui_interaction in;
    gfx_view v;
    c->area = area;
    c->cursor_set = false;
    ui_draw_rect(ui, area, p->workspace);
    if (a->m_cv_border_on) ui_draw_rect(ui, area, app_px_to_ui(a->m_cv_border));   /* lane M */
    layout(a, d, area);
    if (!d) {
        c->nq = 0;
        c->hovered_prev = c->hovered = false;
        empty_workspace(a, area);
        return;
    }
    /* first layout of a new or opened image: fit it (V-ZOOM-OPEN) */
    v = app_doc_gview(a, d);
    if (d->view.need_fit || d->view.fit_mode) {
        gfx_view_fit_window(&v, fit_margin(a), a->overscroll);
        if (d->view.need_fit) d->view.fit_mode = false;
        d->view.need_fit = false;
        app_doc_set_gview(a, d, &v);
        layout(a, d, area);
        v = app_doc_gview(a, d);
    }
    gfx_view_clamp(&v, a->overscroll);
    app_doc_set_gview(a, d, &v);

    /* the canvas region: presses here belong to the tools */
    in = ui_interact(ui, ui_get_id(ui, CANVAS_ID), c->view, UI_INTERACT_OVERLAP);
    c->hovered_prev = c->hovered;
    c->hovered = in.hovered || c->captured;
    process_queue(a, &in);
    if (d != app_active_doc(a)) return;       /* a tool closed or switched the image */
    auto_scroll(a, d);                        /* lane SHELL */
    if (d != app_active_doc(a)) return;
    key_pan(a, d);
    v = app_doc_gview(a, d);

    /* hover (one per frame) */
    if (c->hover_pending && !c->captured && in.hovered) {
        app_qev q;
        memset(&q, 0, sizeof q);
        q.x = c->mx;
        q.y = c->my;
        q.pressure = 1.0f;
        q.mods = cur_mods(a);
        send(a, APP_PTR_HOVER, &q, -1);
    }
    c->hover_pending = false;

    /* wheel: Ctrl zooms at the pointer, Shift scrolls sideways */
    if (in.hovered && !c->captured) {
        ui_vec2 w = ui_wheel_take(ui, c->view);
        uint32_t m = ui_mods(ui);
        if (w.x != 0.0f || w.y != 0.0f) {
            if (m & ui_mod_primary()) {
                c->wheel_zoom_acc += (double)w.y;
                while (c->wheel_zoom_acc >= 1.0) {
                    app_view_zoom_step(a, d, 1, true, (double)c->mx, (double)c->my);
                    c->wheel_zoom_acc -= 1.0;
                }
                while (c->wheel_zoom_acc <= -1.0) {
                    app_view_zoom_step(a, d, -1, true, (double)c->mx, (double)c->my);
                    c->wheel_zoom_acc += 1.0;
                }
            } else {
                double step = (double)ui_px(ui, 48.0f);
                double dx = -(double)w.x * step, dy = (double)w.y * step;
                if (m & UI_MOD_SHIFT) { dx = (double)w.y * step; dy = 0.0; }
                app_view_pan_px(a, d, dx, dy);
            }
            v = app_doc_gview(a, d);
        }
    }

    /* image drop shadow, then the image (callback) and the ants; nothing
     * until a new image's view can be shown completely (V-NOFLICKER) */
    if (!first_hold(a, d, &v)) {
        double x0, y0, x1, y1;
        gfx_view_doc_rect(&v, &x0, &y0, &x1, &y1);
        ui_push_clip(ui, c->view);
        if (x1 - x0 < 1e7 && y1 - y0 < 1e7 && !a->m_cv_no_shadow) {   /* lane M: setting */
            ui_rect ir = ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0),
                                      (int32_t)(y1 - y0));
            float blur = (float)ui_px(ui, 14.0f);
            /* Only the image edges near the view cast visible shadow: clip the
             * rect to the view grown by twice the blur so huge zoomed images do
             * not rasterize a document-sized shadow every frame. */
            int32_t grow = (int32_t)(2.0f * blur) + 2;
            ir = ui_rect_intersect(ir, ui_rect_inset(c->view, -grow, -grow));
            if (ir.w > 0 && ir.h > 0)
                ui_draw_shadow(ui, ir, 0.0f, blur, ui_rgba(0, 0, 0, a->dark ? 170 : 96));
        }
        g_draw.a = a;
        g_draw.v = v;
        g_draw.doc_id = d->id;
        ui_draw_callback(ui, draw_cb, &g_draw);
        /* tool overlay */
        {
            app_overlay o;
            o.a = a;
            o.ui = ui;
            o.v = v;
            o.clip = c->view;
            app_tool_overlay(a, &o);
        }
        ui_pop_clip(ui);
    }
    scrollbar(a, d, c->hbar, true);
    scrollbar(a, d, c->vbar, false);
    if (!ui_rect_empty(c->hbar) && !ui_rect_empty(c->vbar))
        ui_draw_rect(ui, ui_rect_make(c->vbar.x, c->hbar.y, c->vbar.w, c->hbar.h),
                     ui_color_fade(p->panel, 0.82f));
    if (a->rulers) {
        ruler(a, d, c->hruler, true);
        ruler(a, d, c->vruler, false);
        ui_draw_rect(ui, ui_rect_make(area.x, area.y, c->vruler.w, c->hruler.h), p->panel);
    }

    /* cursor */
    if (c->hovered) {
        const app_tool *t = app_tool_current(a);
        app_cursor k;
        if (c->panning) k = APP_CURSOR_GRAB;
        else if (c->space_down) k = APP_CURSOR_HAND;
        else if (c->cursor_set) k = c->cursor;
        else if (t && t->cursor_at && c->mouse_in) {
            double dx, dy;
            gfx_view_to_doc(&v, (double)c->mx, (double)c->my, &dx, &dy);
            k = t->cursor_at(a, app_tool_state(a, t), dx, dy, ui_mods(ui));
        } else {
            k = t ? t->cursor : APP_CURSOR_ARROW;
        }
        c->cursor = k;
        ui_set_cursor(ui, UI_CURSOR_APP);
    }
}

/* After the UI frame: bring the display cache up to date for the visible
 * area, a time slice per frame (huge zoomed-out views stream in). */
void app_canvas_prepare(app *a)
{
    app_canvas *c = &a->cv;
    app_doc *d = app_active_doc(a);
    gfx_view v;
    uint32_t level;
    pc_rect lr;
    pc_comp_opts o;
    uint64_t t0;
    int32_t band;
    c->need_more = false;
    if (!d) return;
    v = app_doc_gview(a, d);
    level = gfx_view_level(v.zoom);
    lr = gfx_view_level_rect(&v, level);
    if (pc_rect_is_empty(lr)) return;
    o = app_doc_comp_opts(d);
    t0 = SDL_GetTicksNS();
    band = 2 * (int32_t)PC_TILE_DIM;
    {
        int32_t y0 = lr.y & ~((int32_t)PC_TILE_DIM - 1);
        for (int32_t y = y0; y < lr.y + lr.h; y += band) {
            pc_rect r = pc_rect_intersect(lr, pc_rect_make(lr.x, y, lr.w, band));
            if (pc_rect_is_empty(r)) continue;
            if (pc_view_cache_update(d->vcache, d->doc, &o, level, r, &a->par) != PC_OK) break;
            if (SDL_GetTicksNS() - t0 > UPDATE_BUDGET_NS && y + band < lr.y + lr.h) {
                c->need_more = true;
                break;
            }
        }
    }
    if (c->need_more) app_request_frame(a);
    /* marching ants animation at the display refresh rate (V-SEL-ANTS) */
    {
        uint64_t t = app_canvas_ants_wake(a);
        if (t) app_request_frame_at(a, t);
    }
}

uint64_t app_canvas_ants_wake(app *a)
{
    app_doc *d = app_active_doc(a);
    uint64_t ms;
    if (!d || !pc_sel_is_active(d->doc) || app_canvas_ants_paused(a)) return 0u;
    ms = (uint64_t)(1000.0f / app_canvas_ants_hz(a));
    return a->now + (ms > 0u ? ms : 1u);
}
