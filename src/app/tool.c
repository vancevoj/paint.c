/* tool.c - tool registry and switching, hotkey cycling (K-TOOLSEL-CYCLE),
 * the shared tool settings and the tool options bar (see app_tool.h). */
#include "app_internal.h"

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

bool app_tools_init(app *a)
{
    a->tool = -1;
    a->tool_prev = -1;
    app_tool_settings_reset(&a->ts);
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

bool app_tool_finish(app *a)
{
    const app_tool *t = app_tool_current(a);
    bool r = false;
    if (!t || !app_tool_live(a)) return false;
    if (t->commit) r = t->commit(a, a->tool_state[a->tool]);
    app_request_frame(a);
    return r;
}

void app_tool_cancel(app *a)
{
    const app_tool *t = app_tool_current(a);
    if (!t || !app_tool_live(a)) return;
    if (t->cancel) t->cancel(a, a->tool_state[a->tool]);
    else if (t->commit) (void)t->commit(a, a->tool_state[a->tool]);
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

/* ---- tool settings --------------------------------------------------------------- */
void app_tool_settings_reset(app_tool_settings *s)
{
    memset(s, 0, sizeof *s);
    s->width = 2.0f;
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
    const char *cur;
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
    cur = app_settings_get(s, "tool.current");
    if (!cur || !app_tool_select(a, cur)) {
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

/* ---- options bar ------------------------------------------------------------------ */
ui_rect app_opt_next(app *a, float w_dip)
{
    ui_ctx *ui = a->ui;
    int32_t w = ui_px(ui, w_dip), h = ui_px(ui, ui_get_theme(ui)->m.control_h);
    ui_rect r = ui_rect_make((int32_t)a->opt_x, a->opt_bar.y + (a->opt_bar.h - h) / 2, w, h);
    a->opt_x += (float)(w + ui_px(ui, 4.0f));
    ui_layout_set_next(ui, r);
    return r;
}

void app_opt_separator(app *a)
{
    ui_ctx *ui = a->ui;
    int32_t x = (int32_t)a->opt_x + ui_px(ui, 3.0f), h = a->opt_bar.h - ui_px(ui, 14.0f);
    ui_draw_rect(ui, ui_rect_make(x, a->opt_bar.y + (a->opt_bar.h - h) / 2,
                                  ui_px_line(ui, 1.0f), h), ui_pal(ui)->separator);
    a->opt_x += (float)ui_px(ui, 10.0f);
}

void app_opt_label(app *a, const char *text)
{
    ui_ctx *ui = a->ui;
    float w = ui_text_width(ui_font_regular(ui), ui_font_px(ui), text, strlen(text));
    (void)app_opt_next(a, w / ui_scale(ui) + 2.0f);
    ui_label_ex(ui, text, UI_LABEL_DIM);
}

void app_opt_width(app *a)
{
    ui_ctx *ui = a->ui;
    double w = (double)a->ts.width;
    app_opt_label(a, "Brush width:");
    (void)app_opt_next(a, 84.0f);
    if (ui_number_double(ui, "##opt_width", &w, 1.0, 2000.0, 1.0, 0, 0)) {
        a->ts.width = (float)w;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Brush width ([ and ] change it)");
}

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
        (void)app_tool_finish(a);
}

void app_options_bar(app *a, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const app_tool *cur = app_tool_current(a);
    const char *names[64];
    int n = 0, sel = 0;
    a->opt_bar = bar;
    a->opt_x = (float)(bar.x + ui_px(ui, 8.0f));
    app_opt_label(a, "Tool:");
    for (int32_t i = 0; i < a->ntools && n < 64; i++) {
        if (a->tools[i] == cur) sel = n;
        names[n++] = a->tools[i]->name;
    }
    (void)app_opt_next(a, 150.0f);
    if (n > 0) {
        int v = sel;
        if (ui_combo(ui, "##tool_choice", &v, names, n) && v != sel)
            select_index(a, v);
        ui_tooltip(ui, "Choose a tool");
    }
    app_opt_separator(a);
    if (cur && cur->options) cur->options(a, a->tool_state[a->tool]);
}
