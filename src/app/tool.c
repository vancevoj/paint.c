/* tool.c - tool registry and switching, hotkey cycling (K-TOOLSEL-CYCLE),
 * the shared tool settings and the tool options bar (see app_tool.h).
 *
 * Lane TOOLA (wave 3b) added the kinds of Finish (explicit Finish History
 * items, live edits that survive layer property changes), the Paint.NET
 * style brush size box for every tool, the DPI scaled default width, the
 * toolbar overflow chevron, the tool chooser with icons and Alt+T,
 * starting with the default tool and arrow key pointer nudges. The
 * keyboard acceleration of the nudges follows the Paint.NET 3.36 Tool
 * class (MIT, docs/notice/toola.md). Auto-scroll at the view edge is the
 * canvas's (lane SHELL). Main thread. */
#include "app_internal.h"
#include "tools/paint_common.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in tools: generated from src/app/tools/tool_<name>.c. */
#define APP_TOOL(n) extern const app_tool app_tool_##n;
#include "app_tool_list.inc"
#undef APP_TOOL

static const app_tool *const k_builtin_tools[] = {
#define APP_TOOL(n) &app_tool_##n,
#include "app_tool_list.inc"
#undef APP_TOOL
    NULL
};

#define CYCLE_WINDOW_MS 1000u   /* docs: pressing the letter again within a second */

/* ---- per-app framework state (lane TOOLA) ------------------------------------------- */
#define FW_KEY          "toola.fw"
#define OPT_MAX_SLOTS   160
#define OPT_SINK_X      (-1000000)
#define OVF_POPUP       "##opt_overflow"
#define TOOL_POPUP      "##tool_choice"  /* also the toolkit open request id (Alt+T) */
#define OVF_DIP         26.0f       /* chevron button */
#define BAR_PAD_DIP     8.0f
#define SLOT_GAP_DIP    4.0f
#define SEP_DIP         10.0f

enum { OPT_BAR = 0, OPT_POPUP = 1, OPT_SINK = 2 };

typedef struct opt_slot {
    int32_t x, w;               /* x relative to the start of the tool's options (px) */
    int32_t group;
} opt_slot;

typedef struct opt_layout {
    const app_tool *tool;
    float           scale;
    int32_t         n;          /* slots declared */
    int32_t         ngroups;    /* separators + 1 */
    int32_t         total;      /* width of the tool's options (px) */
    opt_slot        s[OPT_MAX_SLOTS];
} opt_layout;

typedef struct tool_fw {
    app_finish_kind finishing;
    /* options bar: the layout of the previous frame plans this one */
    opt_layout prev, cur;
    bool       declaring;       /* inside the tool's options() */
    int32_t    mode;            /* OPT_BAR, OPT_POPUP, OPT_SINK */
    int32_t    plan_group;      /* first overflowed group of this frame, -1 none */
    int32_t    content_x;       /* window x where the tool's options start */
    int32_t    row_h;
    int32_t    pop_w, pop_h;    /* popup content size (px) */
    int32_t    nrows;
    int32_t    grow[OPT_MAX_SLOTS];   /* popup row of each planned group */
    int32_t    gx0;             /* x of the first slot of the current group */
    int32_t    gcur;            /* group of gx0 */
    ui_rect    pop;             /* popup content block of this frame */
    bool       ovf_open_prev;   /* the overflow popup was open last frame */
    bool       tool_open_prev;
    /* results of the last frame (tests and diagnostics) */
    ui_rect    rects[OPT_MAX_SLOTS];
    bool       placed[OPT_MAX_SLOTS];
    int32_t    last_first, last_n;
    ui_rect    chevron, tool_btn;
    bool       chevron_vis;
    bool       menu_req;        /* Alt+T: open the tool chooser next frame */
    /* pointer nudges (3.36 keyboard acceleration) */
    uint64_t   nudge_t;
    int32_t    nudge_key, nudge_repeats, nudge_speed;
} tool_fw;

static tool_fw *fw(const app *a)
{
    return (tool_fw *)app_ext_get((app *)a, FW_KEY);
}

static tool_fw *fw_make(app *a)
{
    tool_fw *f = fw(a);
    if (f) return f;
    f = (tool_fw *)calloc(1u, sizeof *f);
    if (!f) return NULL;
    if (!app_ext_set(a, FW_KEY, f, free)) {
        free(f);
        return NULL;
    }
    f->plan_group = -1;
    f->last_first = -1;
    f->nudge_speed = 1;
    return f;
}

/* ---- registry ---------------------------------------------------------------------- */
bool app_tool_register(app *a, const app_tool *t)
{
    int32_t at;
    void *st = NULL;
    if (!a || !t || !t->id || !t->name) return false;
    if (app_tool_find(a, t->id)) {
        pal_log(PAL_LOG_WARN, "tool rejected: duplicate id %s", t->id);
        return false;
    }
    if (a->ntools == a->cap_tools) {
        int32_t nc = a->cap_tools ? a->cap_tools * 2 : 32;
        const app_tool **nt = (const app_tool **)realloc((void *)a->tools,
                                                         (size_t)nc * sizeof *nt);
        void **ns;
        if (!nt) return false;
        a->tools = nt;
        ns = (void **)realloc(a->tool_state, (size_t)nc * sizeof *ns);
        if (!ns) return false;
        a->tool_state = ns;
        a->cap_tools = nc;
    }
    if (t->state_size) {
        st = calloc(1u, t->state_size);
        if (!st) return false;
    }
    /* stable insert by order */
    at = a->ntools;
    while (at > 0 && a->tools[at - 1]->order > t->order) at--;
    memmove((void *)&a->tools[at + 1], (const void *)&a->tools[at],
            (size_t)(a->ntools - at) * sizeof *a->tools);
    memmove(&a->tool_state[at + 1], &a->tool_state[at],
            (size_t)(a->ntools - at) * sizeof *a->tool_state);
    a->tools[at] = t;
    a->tool_state[at] = st;
    a->ntools++;
    if (a->tool >= at && a->ntools > 1) a->tool++;
    if (a->tool_prev >= at && a->ntools > 1) a->tool_prev++;
    if (t->init) t->init(a, st);
    return true;
}

/* Default brush width scale: the UI scale of the first app (O-WIDTH: the
 * default width is 2 at 100 % and follows the display DPI). Written once
 * per app creation on the main thread. */
static float g_width_scale = 1.0f;

float app_tool_default_width(void)
{
    float w = 2.0f * g_width_scale;
    if (w < 1.0f) w = 1.0f;
    if (w > 2000.0f) w = 2000.0f;
    return w;
}

bool app_tools_init(app *a)
{
    float s = 1.0f;
    a->tool = -1;
    a->tool_prev = -1;
    /* the display scale (no frame has run yet, so not ui_scale) */
    if (a->opts.headless) s = a->opts.scale;
    else if (a->win) s = SDL_GetWindowDisplayScale(a->win);
    g_width_scale = s > 0.25f && s < 16.0f ? s : 1.0f;
    app_tool_settings_reset(&a->ts);
    (void)fw_make(a);
    for (size_t i = 0; k_builtin_tools[i]; i++)
        if (!app_tool_register(a, k_builtin_tools[i])) return false;
    return true;
}

void app_tools_free(app *a)
{
    for (int32_t i = 0; i < a->ntools; i++) {
        const app_tool *t = a->tools[i];
        if (t->fini) t->fini(a, a->tool_state[i]);
        free(a->tool_state[i]);
    }
    free((void *)a->tools);
    free(a->tool_state);
    a->tools = NULL;
    a->tool_state = NULL;
    a->ntools = a->cap_tools = 0;
}

int32_t app_tool_count(const app *a) { return a->ntools; }

const app_tool *app_tool_at(const app *a, int32_t i)
{
    return i >= 0 && i < a->ntools ? a->tools[i] : NULL;
}

static int32_t tool_index(const app *a, const char *id)
{
    if (!id) return -1;
    for (int32_t i = 0; i < a->ntools; i++)
        if (strcmp(a->tools[i]->id, id) == 0) return i;
    return -1;
}

const app_tool *app_tool_find(const app *a, const char *id)
{
    int32_t i = tool_index(a, id);
    return i >= 0 ? a->tools[i] : NULL;
}

const app_tool *app_tool_current(const app *a)
{
    return a->tool >= 0 && a->tool < a->ntools ? a->tools[a->tool] : NULL;
}

void *app_tool_state(const app *a, const app_tool *t)
{
    for (int32_t i = 0; i < a->ntools; i++)
        if (a->tools[i] == t) return a->tool_state[i];
    return NULL;
}

static void select_index(app *a, int32_t idx)
{
    const app_tool *old = app_tool_current(a);
    if (idx == a->tool || idx < 0 || idx >= a->ntools) return;
    if (a->cv.captured) app_canvas_lost_capture(a);
    if (old) {
        (void)app_tool_finish(a);
        if (old->deactivate) old->deactivate(a, a->tool_state[a->tool]);
    }
    a->tool_prev = a->tool;
    a->tool = idx;
    app_status(a, NULL);
    if (a->tools[idx]->activate) a->tools[idx]->activate(a, a->tool_state[idx]);
    app_request_frame(a);
}

bool app_tool_select(app *a, const char *id)
{
    int32_t i = tool_index(a, id);
    if (i < 0) return false;
    select_index(a, i);
    return true;
}

bool app_tool_live(app *a)
{
    const app_tool *t = app_tool_current(a);
    return t && t->live && t->live(a, a->tool_state[a->tool]);
}

/* ---- finishing (lane TOOLA kinds) ---------------------------------------------------- */
static bool commit_as(app *a, const app_tool *t, app_finish_kind kind)
{
    tool_fw *f = fw(a);
    app_finish_kind old = f ? f->finishing : APP_FINISH_IMPLICIT;
    bool r = false;
    if (f) f->finishing = kind;
    if (t->commit) r = t->commit(a, a->tool_state[a->tool]);
    if (f) f->finishing = old;
    return r;
}

bool app_tool_finish_as(app *a, app_finish_kind kind)
{
    const app_tool *t = app_tool_current(a);
    bool r;
    if (!t) return false;
    /* T-MOVEPX-FINISH: layer property changes leave these live edits alone */
    if (kind == APP_FINISH_LAYER_PROPS && (t->flags & APP_TOOL_KEEPS_LIVE)) return false;
    if (!app_tool_live(a)) return false;
    r = commit_as(a, t, kind == APP_FINISH_EXPLICIT ? APP_FINISH_EXPLICIT : APP_FINISH_IMPLICIT);
    app_request_frame(a);
    return r;
}

bool app_tool_finish(app *a) { return app_tool_finish_as(a, APP_FINISH_IMPLICIT); }

bool app_tool_finish_explicit(app *a) { return app_tool_finish_as(a, APP_FINISH_EXPLICIT); }

app_finish_kind app_tool_finishing(const app *a)
{
    const tool_fw *f = fw(a);
    return f ? f->finishing : APP_FINISH_IMPLICIT;
}

void app_tool_cancel(app *a)
{
    const app_tool *t = app_tool_current(a);
    if (!t || !app_tool_live(a)) return;
    if (t->cancel) t->cancel(a, a->tool_state[a->tool]);
    else (void)commit_as(a, t, APP_FINISH_EXPLICIT);      /* Esc is the user's Finish */
    app_request_frame(a);
}

void app_tool_dispatch(app *a, const app_pointer *ev)
{
    const app_tool *t = app_tool_current(a);
    if (t && t->pointer) t->pointer(a, a->tool_state[a->tool], ev);
}

void app_tool_overlay(app *a, app_overlay *o)
{
    const app_tool *t = app_tool_current(a);
    if (t && t->overlay) t->overlay(a, a->tool_state[a->tool], o);
}

/* 3.36 Tool.OnKeyPress: Shift searches backwards; within the window the
 * search continues after the current tool, else it starts over. A letter
 * pressed while a mouse button is down is swallowed (K-TOOLSEL-MOUSEDOWN). */
bool app_tool_letter(app *a, int32_t key, uint32_t mods)
{
    char letter;
    int32_t n = a->ntools, start = -1, found = -1;
    bool reverse = (mods & UI_MOD_SHIFT) != 0, any = false;
    const app_tool *cur = app_tool_current(a);
    if (key < 'a' || key > 'z' || (mods & ~UI_MOD_SHIFT)) return false;
    letter = (char)(key - 'a' + 'A');
    for (int32_t i = 0; i < n; i++)
        if (a->tools[i]->letter == letter) any = true;
    if (!any) return false;
    if (a->cv.captured) return true;
    if (cur && cur->letter == letter && a->now - a->cycle_ms <= CYCLE_WINDOW_MS)
        start = reverse ? n - 1 - a->tool : a->tool;
    for (int32_t k = 0; k < n; k++) {
        int32_t pos = (k + start + 1) % n;
        int32_t idx = reverse ? n - 1 - pos : pos;
        if (a->tools[idx]->letter == letter) { found = idx; break; }
    }
    if (found < 0) return false;
    select_index(a, found);
    a->cycle_ms = a->now;
    a->cycle_key = letter;
    return true;
}

/* ---- pointer nudges (T-FW-ARROWS) ---------------------------------------------------- */
bool app_tool_nudge_pointer(app *a, int32_t key, uint32_t mods)
{
    tool_fw *f = fw(a);
    app_doc *d = app_active_doc(a);
    int32_t dx = 0, dy = 0, step;
    double zoom;
    float ppp, nx, ny;
    SDL_Event e;
    switch (key) {
    case SDLK_LEFT: dx = -1; break;
    case SDLK_RIGHT: dx = 1; break;
    case SDLK_UP: dy = -1; break;
    case SDLK_DOWN: dy = 1; break;
    default: return false;
    }
    if (!d || !(a->cv.hovered || a->cv.captured) || !a->cv.mouse_in) return false;
    if (a->cv.space_down) return false;          /* Space + arrows pan the view */
    /* 3.36: holding a key speeds up after 15 quick repeats, one more pixel
     * every fourth repeat; a pause (or another key) starts over */
    if (f) {
        if (key != f->nudge_key || a->now - f->nudge_t > 250u) {
            f->nudge_repeats = 0;
            f->nudge_speed = 1;
        } else if (++f->nudge_repeats > 15 && f->nudge_repeats % 4 == 0) {
            f->nudge_speed++;
        }
        f->nudge_key = key;
        f->nudge_t = a->now;
    }
    zoom = d->view.zoom > 0.0 ? d->view.zoom : 1.0;
    step = (int32_t)ceil(zoom - 1e-9) * (f ? f->nudge_speed : 1);
    if (step < 1) step = 1;
    if (step > 4096) step = 4096;
    if ((mods & (UI_MOD_CTRL | UI_MOD_GUI)) != 0u) step *= 10;      /* K-NAV-TOOLMOVE-10 */
    ppp = a->fi.px_per_point > 0.0f ? a->fi.px_per_point : 1.0f;
    nx = a->cv.mx + (float)(dx * step);
    ny = a->cv.my + (float)(dy * step);
    if (a->win && !a->opts.headless) SDL_WarpMouseInWindow(a->win, nx / ppp, ny / ppp);
    /* the canvas sees the motion either way (warping is not allowed on
     * every platform, for example Wayland without pointer constraints) */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = nx / ppp;
    e.motion.y = ny / ppp;
    e.motion.which = 1;
    e.motion.timestamp = SDL_GetTicksNS();
    app_canvas_event(a, &e);
    app_request_frame(a);
    return true;
}

/* ---- tool settings --------------------------------------------------------------- */
void app_tool_settings_reset(app_tool_settings *s)
{
    memset(s, 0, sizeof *s);
    s->width = app_tool_default_width();      /* O-WIDTH: DPI scaled */
    s->pressure = true;
    s->hardness = 75;
    s->spacing = 15;
    s->smoothing = true;
    s->fill = 0;
    s->antialias = true;
    s->blend = (int32_t)PC_BLEND_NORMAL;
    s->sel_clip_aa = true;
    s->sel_mode = (int32_t)PC_SEL_REPLACE;
    s->flood_global = false;
    s->tolerance = 50;
    s->tol_straight = false;
    s->sampling = 0;
}

app_tool_settings *app_tool_settings_get(app *a) { return &a->ts; }

void app_tool_settings_changed(app *a)
{
    const app_tool *t = app_tool_current(a);
    if (t && t->settings_changed) t->settings_changed(a, a->tool_state[a->tool]);
    app_request_frame(a);
}

enum { F_FLOAT, F_INT, F_BOOL };
typedef struct ts_field { const char *key; int type; size_t off; double lo, hi; } ts_field;
static const ts_field k_fields[] = {
    { "tool.width", F_FLOAT, offsetof(app_tool_settings, width), 1.0, 2000.0 },
    { "tool.pressure", F_BOOL, offsetof(app_tool_settings, pressure), 0, 1 },
    { "tool.hardness", F_INT, offsetof(app_tool_settings, hardness), 0, 100 },
    { "tool.spacing", F_INT, offsetof(app_tool_settings, spacing), 1, 1000 },
    { "tool.smoothing", F_BOOL, offsetof(app_tool_settings, smoothing), 0, 1 },
    { "tool.fill", F_INT, offsetof(app_tool_settings, fill), 0, 53 },
    { "tool.antialias", F_BOOL, offsetof(app_tool_settings, antialias), 0, 1 },
    { "tool.blend", F_INT, offsetof(app_tool_settings, blend), 0, (double)PC_BLEND_COUNT },
    { "tool.sel_clip_aa", F_BOOL, offsetof(app_tool_settings, sel_clip_aa), 0, 1 },
    { "tool.sel_mode", F_INT, offsetof(app_tool_settings, sel_mode), 0,
      (double)(PC_SEL_MODE_COUNT - 1) },
    { "tool.flood_global", F_BOOL, offsetof(app_tool_settings, flood_global), 0, 1 },
    { "tool.tolerance", F_INT, offsetof(app_tool_settings, tolerance), 0, 100 },
    { "tool.tol_straight", F_BOOL, offsetof(app_tool_settings, tol_straight), 0, 1 },
    { "tool.sampling", F_INT, offsetof(app_tool_settings, sampling), 0, 1 },
};

void app_tools_load(app *a)
{
    const app_settings *s = a->settings;
    uint8_t *base = (uint8_t *)&a->ts;
    const char *def;
    for (size_t i = 0; i < sizeof k_fields / sizeof k_fields[0]; i++) {
        const ts_field *f = &k_fields[i];
        if (!app_settings_get(s, f->key)) continue;
        if (f->type == F_FLOAT) {
            double v = app_settings_double(s, f->key, 2.0);
            if (v < f->lo) v = f->lo;
            if (v > f->hi) v = f->hi;
            *(float *)(void *)(base + f->off) = (float)v;
        } else if (f->type == F_INT) {
            int64_t v = app_settings_int(s, f->key, 0);
            if (v < (int64_t)f->lo) v = (int64_t)f->lo;
            if (v > (int64_t)f->hi) v = (int64_t)f->hi;
            *(int32_t *)(void *)(base + f->off) = (int32_t)v;
        } else {
            *(bool *)(void *)(base + f->off) = app_settings_bool(s, f->key, false);
        }
    }
    /* F-TOOL-DEFAULT-BRUSH: every start uses the default tool of Settings >
     * Tools (tooldef.tool), Paintbrush until one is chosen; the last used
     * tool is not restored (D: "Paintbrush, or the default tool") */
    def = app_settings_get(s, "tooldef.tool");
    if (!def || !app_tool_select(a, def)) {
        if (!app_tool_select(a, "paintbrush") && a->ntools > 0) select_index(a, 0);
    }
}

void app_tools_store(app *a)
{
    app_settings *s = a->settings;
    const uint8_t *base = (const uint8_t *)&a->ts;
    for (size_t i = 0; i < sizeof k_fields / sizeof k_fields[0]; i++) {
        const ts_field *f = &k_fields[i];
        if (f->type == F_FLOAT)
            app_settings_set_double(s, f->key,
                                    (double)*(const float *)(const void *)(base + f->off));
        else if (f->type == F_INT)
            app_settings_set_int(s, f->key, *(const int32_t *)(const void *)(base + f->off));
        else
            app_settings_set_bool(s, f->key, *(const bool *)(const void *)(base + f->off));
    }
    if (app_tool_current(a)) app_settings_set(s, "tool.current", app_tool_current(a)->id);
}

void app_tool_paint_opts(const app *a, pc_paint_opts *o)
{
    *o = pc_paint_opts_default();
    if (a->ts.blend >= APP_BLEND_OVERWRITE) {
        o->mode = PC_PAINT_OVERWRITE;
    } else {
        o->mode = PC_PAINT_BLEND;
        o->blend = (pc_blend_mode)(a->ts.blend < 0 ? 0 : a->ts.blend);
    }
    o->clip_to_selection = true;
    o->clip_pixelated = !a->ts.sel_clip_aa;      /* O-SELCLIP pixelated */
}

/* O-WIDTH-PRESETS: 1..15 by 1, 20..100 by 5, 125..500 by 25, then coarser
 * steps up to 2000. */
static const float k_width_presets[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60,
    65, 70, 75, 80, 85, 90, 95, 100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375,
    400, 425, 450, 475, 500, 600, 700, 800, 900, 1000, 1250, 1500, 1750, 2000
};

float app_width_step(float w, int dir)
{
    size_t n = sizeof k_width_presets / sizeof k_width_presets[0];
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (k_width_presets[i] > w + 1e-3f) return k_width_presets[i];
        return k_width_presets[n - 1u];
    }
    for (size_t i = n; i > 0; i--)
        if (k_width_presets[i - 1u] < w - 1e-3f) return k_width_presets[i - 1u];
    return k_width_presets[0];
}

void app_set_cursor(app *a, app_cursor c)
{
    a->cv.cursor = c;
    a->cv.cursor_set = true;
}

/* ---- options bar: slots, groups and the overflow (lane TOOLA) ------------------------ */
static int32_t gap_px(const app *a) { return ui_px(a->ui, SLOT_GAP_DIP); }

/* Enter the overflow part of the bar: the popup when it is open, else a
 * sink far off screen where the widgets are declared but never seen or
 * reached. Called between widgets (at a separator or before the first
 * one), never inside one. */
static void enter_overflow(app *a, tool_fw *f)
{
    ui_ctx *ui = a->ui;
    if (f->mode != OPT_BAR) return;
    if (ui_popup_begin(ui, OVF_POPUP)) {
        float s = ui_scale(ui) > 0.0f ? ui_scale(ui) : 1.0f;
        ui_size cell = ui_size_px((float)f->pop_w / s + 1.0f);
        f->mode = OPT_POPUP;
        /* claim the content block so the popup measures itself */
        ui_layout_row(ui, (float)f->pop_h / s + 1.0f, 1, &cell);
        f->pop = ui_layout_next(ui, f->pop_w, f->pop_h);
        ui_layout_column(ui);
    } else {
        f->mode = OPT_SINK;
    }
}

static void leave_overflow(app *a, tool_fw *f)
{
    if (f->mode == OPT_POPUP) ui_popup_end(a->ui);
    f->mode = OPT_BAR;
}

/* Plan this frame from the previous frame's layout of the same tool: the
 * first group that does not fit before the chevron and everything after
 * it overflow, one group per popup row. */
static void opt_plan(app *a, tool_fw *f, const app_tool *t, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const opt_layout *p = &f->prev;
    int32_t right = bar.x + bar.w - ui_px(ui, BAR_PAD_DIP), limit, gap = gap_px(a);
    f->plan_group = -1;
    f->nrows = 0;
    f->pop_w = f->pop_h = 0;
    f->row_h = ui_px(ui, ui_get_theme(ui)->m.control_h);
    if (p->tool != t || p->n <= 0 || p->scale != ui_scale(ui)) return;
    if (f->content_x + p->total <= right) return;
    limit = right - ui_px(ui, OVF_DIP) - gap;
    for (int32_t i = 0; i < p->n; i++) {
        if (f->content_x + p->s[i].x + p->s[i].w > limit) {
            f->plan_group = p->s[i].group;
            break;
        }
    }
    if (f->plan_group < 0) return;
    /* rows: one per non-empty overflowed group */
    for (int32_t g = 0; g < OPT_MAX_SLOTS; g++) f->grow[g] = -1;
    for (int32_t i = 0; i < p->n; i++) {
        int32_t g = p->s[i].group, gx = p->s[i].x, w;
        if (g < f->plan_group || g >= OPT_MAX_SLOTS) continue;
        if (f->grow[g] < 0) f->grow[g] = f->nrows++;
        /* width of the group up to this slot */
        for (int32_t k = i; k >= 0 && p->s[k].group == g; k--) gx = p->s[k].x;
        w = p->s[i].x + p->s[i].w - gx;
        if (w > f->pop_w) f->pop_w = w;
    }
    f->pop_h = f->nrows > 0 ? f->nrows * f->row_h + (f->nrows - 1) * gap : 0;
}

/* Record a slot of the current frame; returns its index or -1. */
static int32_t slot_add(tool_fw *f, int32_t x, int32_t w)
{
    int32_t i;
    if (f->cur.n >= OPT_MAX_SLOTS) return -1;
    i = f->cur.n++;
    f->cur.s[i].x = x;
    f->cur.s[i].w = w;
    f->cur.s[i].group = f->cur.ngroups - 1;
    return i;
}

ui_rect app_opt_next(app *a, float w_dip)
{
    ui_ctx *ui = a->ui;
    tool_fw *f = fw(a);
    int32_t w = ui_px(ui, w_dip), h = ui_px(ui, ui_get_theme(ui)->m.control_h);
    ui_rect r = ui_rect_make((int32_t)a->opt_x, a->opt_bar.y + (a->opt_bar.h - h) / 2, w, h);
    if (f && f->declaring) {
        int32_t rel = (int32_t)a->opt_x - f->content_x, i = slot_add(f, rel, w);
        int32_t g = f->cur.ngroups - 1;
        if (g != f->gcur) {
            f->gcur = g;
            f->gx0 = rel;
        }
        if (f->mode == OPT_POPUP) {
            int32_t row = g < OPT_MAX_SLOTS && f->grow[g] >= 0 ? f->grow[g] : f->nrows;
            r = ui_rect_make(f->pop.x + rel - f->gx0, f->pop.y + row * (f->row_h + gap_px(a)),
                             w, h);
        } else if (f->mode == OPT_SINK) {
            r = ui_rect_make(OPT_SINK_X + rel, OPT_SINK_X, w, h);
        }
        if (i >= 0) {
            f->rects[i] = r;
            f->placed[i] = f->mode != OPT_SINK;
        }
    }
    a->opt_x += (float)(w + gap_px(a));
    ui_layout_set_next(ui, r);
    return r;
}

void app_opt_separator(app *a)
{
    ui_ctx *ui = a->ui;
    tool_fw *f = fw(a);
    int32_t x = (int32_t)a->opt_x + ui_px(ui, 3.0f), h = a->opt_bar.h - ui_px(ui, 14.0f);
    if (f && f->declaring) {
        if (f->cur.ngroups < OPT_MAX_SLOTS) f->cur.ngroups++;
        if (f->mode == OPT_BAR && f->plan_group >= 0 && f->cur.ngroups - 1 >= f->plan_group) {
            enter_overflow(a, f);
            a->opt_x += (float)ui_px(ui, SEP_DIP);
            return;
        }
        if (f->mode != OPT_BAR) {
            a->opt_x += (float)ui_px(ui, SEP_DIP);
            return;
        }
    }
    ui_draw_rect(ui, ui_rect_make(x, a->opt_bar.y + (a->opt_bar.h - h) / 2,
                                  ui_px_line(ui, 1.0f), h), ui_pal(ui)->separator);
    a->opt_x += (float)ui_px(ui, SEP_DIP);
}

void app_opt_label(app *a, const char *text)
{
    ui_ctx *ui = a->ui;
    float w = ui_text_width(ui_font_regular(ui), ui_font_px(ui), text, strlen(text));
    (void)app_opt_next(a, w / ui_scale(ui) + 2.0f);
    ui_label_ex(ui, text, UI_LABEL_DIM);
}

/* O-WIDTH (F-TOOL-OPT-WIDTH): one width box for every tool that has one,
 * Paint.NET's "Brush size" combo with -/+ buttons, decimals kept, the
 * preset list, the wheel and Up/Down stepping through the presets and
 * invalid values marked red (lane B's widget, tools/paint_ui.c). */
void app_opt_width(app *a) { paint_opt_width(a); }

void app_opt_hardness(app *a)
{
    ui_ctx *ui = a->ui;
    int32_t v = a->ts.hardness;
    app_opt_label(a, "Hardness:");
    (void)app_opt_next(a, 110.0f);
    if (ui_slider_int(ui, "##opt_hardness", &v, 0, 100, UI_SLIDER_PERCENT)) {
        a->ts.hardness = v;
        app_tool_settings_changed(a);
    }
}

void app_opt_antialias(app *a)
{
    ui_ctx *ui = a->ui;
    int r;
    (void)app_opt_next(a, 42.0f);
    r = ui_split_button(ui, "##opt_aa", a->ts.antialias ? UI_ICON_AA_ON : UI_ICON_AA_OFF,
                        false, a->ts.antialias ? "Antialiasing enabled" : "Antialiasing disabled");
    if (r == 1) {
        a->ts.antialias = !a->ts.antialias;
        app_tool_settings_changed(a);
    }
    if (r == 2) ui_popup_open(ui, "##opt_aa_menu", ui_last_rect(ui), UI_POPUP_BELOW);
    if (ui_popup_begin(ui, "##opt_aa_menu")) {
        if (ui_menu_radio(ui, "Antialiasing Enabled", NULL, a->ts.antialias, true)) {
            a->ts.antialias = true;
            app_tool_settings_changed(a);
        }
        if (ui_menu_radio(ui, "Antialiasing Disabled", NULL, !a->ts.antialias, true)) {
            a->ts.antialias = false;
            app_tool_settings_changed(a);
        }
        ui_popup_end(ui);
    }
}

static const char *const k_blend_names[] = {
    "Normal",   "Multiply",   "Additive", "Color Burn", "Color Dodge", "Reflect",
    "Glow",     "Overlay",    "Difference", "Negation", "Lighten",    "Darken",
    "Screen",   "Xor",        "Overwrite"
};

void app_opt_blend(app *a)
{
    ui_ctx *ui = a->ui;
    int v = (int)a->ts.blend;
    (void)app_opt_next(a, 120.0f);
    if (ui_combo(ui, "##opt_blend", &v, k_blend_names, (int)PC_BLEND_COUNT + 1)) {
        a->ts.blend = (int32_t)v;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Blend mode");
}

void app_opt_sel_clip(app *a)
{
    ui_ctx *ui = a->ui;
    int r;
    (void)app_opt_next(a, 42.0f);
    r = ui_split_button(ui, "##opt_selclip", UI_ICON_SELECT_ALL, false,
                        a->ts.sel_clip_aa ? "Selection clipping: antialiased"
                                          : "Selection clipping: pixelated");
    if (r == 1) {
        a->ts.sel_clip_aa = !a->ts.sel_clip_aa;
        app_tool_settings_changed(a);
    }
    if (r == 2) ui_popup_open(ui, "##opt_selclip_menu", ui_last_rect(ui), UI_POPUP_BELOW);
    if (ui_popup_begin(ui, "##opt_selclip_menu")) {
        if (ui_menu_radio(ui, "Antialiased Selection Clipping", NULL, a->ts.sel_clip_aa, true)) {
            a->ts.sel_clip_aa = true;
            app_tool_settings_changed(a);
        }
        if (ui_menu_radio(ui, "Pixelated Selection Clipping", NULL, !a->ts.sel_clip_aa, true)) {
            a->ts.sel_clip_aa = false;
            app_tool_settings_changed(a);
        }
        ui_popup_end(ui);
    }
}

void app_opt_finish(app *a)
{
    ui_ctx *ui = a->ui;
    bool live = app_tool_live(a);
    (void)app_opt_next(a, 84.0f);
    if (ui_button_ex(ui, "Finish##opt_finish", UI_ICON_CHECK, live ? 0u : UI_DISABLED))
        (void)app_tool_finish_explicit(a);         /* the user's Finish: a History item */
}

/* ---- the tool chooser (F-TOOL-ORDER) ---------------------------------------------------- */
/* "Magic Wand (S, 4 times)": the letter and how many presses select the
 * tool in Tools window order (K-TOOLSEL-TOOLTIP); one tool per letter
 * shows just the letter. */
static void tool_tip(const app *a, const app_tool *t, char *out, size_t cap)
{
    int32_t same = 0, pos = 0;
    if (!t->letter) {
        snprintf(out, cap, "%s", t->name);
        return;
    }
    for (int32_t i = 0; i < a->ntools; i++) {
        if (a->tools[i]->letter != t->letter) continue;
        same++;
        if (a->tools[i] == t) pos = same;
    }
    if (same > 1 && pos > 0)
        snprintf(out, cap, "%s (%c, %d %s)", t->name, t->letter, (int)pos,
                 pos == 1 ? "time" : "times");
    else
        snprintf(out, cap, "%s (%c)", t->name, t->letter);
}

void app_tool_menu_open(app *a)
{
    tool_fw *f = fw(a);
    if (!f) return;
    f->menu_req = true;
    app_request_frame(a);
}

static void tool_chooser(app *a, tool_fw *f, const app_tool *cur)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float tw = 0.0f, fs = ui_font_px(ui);
    int32_t isz = ui_px(ui, 16.0f), pad = ui_px(ui, 6.0f), aw = ui_px(ui, 16.0f);
    float rad = (float)ui_px(ui, 4.0f);
    ui_rect r, ir, tr, ar;
    ui_interaction in;
    bool open_now = ui_popup_is_open(ui, TOOL_POPUP);
    char tip[160];
    for (int32_t i = 0; i < a->ntools; i++) {
        float w = ui_text_width(ui_font_regular(ui), fs, a->tools[i]->name,
                                strlen(a->tools[i]->name));
        if (w > tw) tw = w;
    }
    /* wide enough for the longest name ("Move Selected Pixels") */
    r = app_opt_next(a, (tw + (float)(isz + 3 * pad + aw)) / ui_scale(ui) + 4.0f);
    (void)ui_layout_next(ui, r.w, r.h);          /* drawn by hand: consume the slot */
    if (f) f->tool_btn = r;
    in = ui_interact(ui, ui_get_id(ui, "##tool_choice_btn"), r, 0u);
    ui_draw_rrect(ui, r, rad, in.hovered ? p->field_hover : p->field);
    ui_draw_rrect_outline(ui, r, rad, ui_px_line(ui, 1.0f),
                          in.hovered || open_now ? p->border_strong : p->border);
    ir = ui_rect_make(r.x + pad, r.y, isz, r.h);
    tr = ui_rect_make(ir.x + isz + pad, r.y, r.w - (isz + 3 * pad + aw), r.h);
    ar = ui_rect_make(r.x + r.w - aw - pad / 2, r.y, aw, r.h);
    if (cur) {
        ui_draw_icon(ui, cur->icon, ir, isz, p->icon, p->icon_accent);
        ui_draw_text_box(ui, ui_font_regular(ui), fs, tr, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text,
                         cur->name, strlen(cur->name));
    }
    ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN, ar, ui_px(ui, 10.0f), p->text_dim, p->text_dim);
    ui_tooltip(ui, "Choose a tool (Alt+T)");
    /* a press toggles the list (a press outside it closed it this frame) */
    if ((in.pressed && !(f && f->tool_open_prev)) || (f && f->menu_req)) {
        ui_popup_open(ui, TOOL_POPUP, r, UI_POPUP_BELOW);
        if (f) f->menu_req = false;
    }
    if (ui_popup_begin(ui, TOOL_POPUP)) {
        for (int32_t i = 0; i < a->ntools; i++) {
            const app_tool *t = a->tools[i];
            char sc[2];
            sc[0] = t->letter;
            sc[1] = '\0';
            ui_push_id_int(ui, i);
            if (ui_menu_item_icon(ui, t->icon, t->name, t->letter ? sc : NULL, true) &&
                i != a->tool)
                select_index(a, i);
            tool_tip(a, t, tip, sizeof tip);
            ui_tooltip(ui, tip);
            ui_pop_id(ui);
        }
        ui_popup_end(ui);
    }
    if (f) f->tool_open_prev = ui_popup_is_open(ui, TOOL_POPUP);
}

/* The chevron at the right end of the bar (shown while options overflow). */
static void overflow_button(app *a, tool_fw *f, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t w = ui_px(ui, OVF_DIP), h = ui_px(ui, ui_get_theme(ui)->m.control_h);
    ui_rect r = ui_rect_make(bar.x + bar.w - ui_px(ui, BAR_PAD_DIP) - w,
                             bar.y + (bar.h - h) / 2, w, h);
    float rad = (float)ui_px(ui, 4.0f);
    bool open_now;
    ui_interaction in;
    f->chevron = r;
    f->chevron_vis = true;
    in = ui_interact(ui, ui_get_id(ui, "##opt_overflow_btn"), r, 0u);
    open_now = ui_popup_is_open(ui, OVF_POPUP);
    if (in.hovered || open_now)
        ui_draw_rrect(ui, r, rad, open_now ? p->selection : p->hover);
    ui_draw_icon(ui, UI_ICON_CHEVRON_RIGHT,
                 ui_rect_make(r.x + ui_px(ui, 2.0f), r.y, r.w / 2, r.h), ui_px(ui, 10.0f),
                 p->text, p->text);
    ui_draw_icon(ui, UI_ICON_CHEVRON_RIGHT,
                 ui_rect_make(r.x + r.w / 2 - ui_px(ui, 2.0f), r.y, r.w / 2, r.h),
                 ui_px(ui, 10.0f), p->text, p->text);
    ui_tooltip(ui, "More options");
    if (in.pressed && !f->ovf_open_prev) {
        ui_rect anchor = ui_rect_make(r.x + r.w - 1, r.y, 1, r.h);   /* right aligned */
        ui_popup_open(ui, OVF_POPUP, anchor, UI_POPUP_BELOW);
    }
}

void app_options_bar(app *a, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const app_tool *cur = app_tool_current(a);
    tool_fw *f = fw(a);
    a->opt_bar = bar;
    a->opt_x = (float)(bar.x + ui_px(ui, BAR_PAD_DIP));
    tool_chooser(a, f, cur);
    cur = app_tool_current(a);                   /* the chooser may have switched tools */
    app_opt_separator(a);
    if (!f) {
        if (cur && cur->options) cur->options(a, a->tool_state[a->tool]);
        return;
    }
    f->content_x = (int32_t)a->opt_x;
    f->chevron_vis = false;
    opt_plan(a, f, cur, bar);
    memset(&f->cur, 0, sizeof f->cur);
    f->cur.tool = cur;
    f->cur.scale = ui_scale(ui);
    f->cur.ngroups = 1;
    f->gcur = -1;
    f->gx0 = 0;
    f->mode = OPT_BAR;
    memset(f->placed, 0, sizeof f->placed);
    f->declaring = true;
    if (f->plan_group == 0) enter_overflow(a, f);
    if (cur && cur->options) cur->options(a, a->tool_state[a->tool]);
    leave_overflow(a, f);
    f->declaring = false;
    f->cur.total = (int32_t)a->opt_x - f->content_x;
    f->last_first = -1;
    if (f->plan_group >= 0) {
        for (int32_t i = 0; i < f->cur.n; i++)
            if (f->cur.s[i].group >= f->plan_group) {
                f->last_first = i;
                break;
            }
        overflow_button(a, f, bar);
    }
    f->last_n = f->cur.n;
    f->ovf_open_prev = f->plan_group >= 0 && ui_popup_is_open(ui, OVF_POPUP);
    /* a different layout than the plan assumed: plan again next frame */
    if (f->cur.n != f->prev.n || f->cur.total != f->prev.total ||
        f->cur.ngroups != f->prev.ngroups || f->cur.tool != f->prev.tool ||
        f->cur.scale != f->prev.scale)
        app_request_frame(a);
    f->prev = f->cur;
}

int32_t app_opt_overflow_first(const app *a)
{
    const tool_fw *f = fw(a);
    return f ? f->last_first : -1;
}

int32_t app_opt_slot_count(const app *a)
{
    const tool_fw *f = fw(a);
    return f ? f->last_n : 0;
}

bool app_opt_slot_rect(const app *a, int32_t i, ui_rect *r)
{
    const tool_fw *f = fw(a);
    if (!f || i < 0 || i >= f->last_n || i >= OPT_MAX_SLOTS || !f->placed[i]) return false;
    if (r) *r = f->rects[i];
    return true;
}

bool app_opt_overflow_button(const app *a, ui_rect *r)
{
    const tool_fw *f = fw(a);
    if (!f || !f->chevron_vis) return false;
    if (r) *r = f->chevron;
    return true;
}

bool app_opt_tool_button(const app *a, ui_rect *r)
{
    const tool_fw *f = fw(a);
    if (!f || ui_rect_empty(f->tool_btn)) return false;
    if (r) *r = f->tool_btn;
    return true;
}
