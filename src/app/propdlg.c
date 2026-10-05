/* propdlg.c - the generic dialog builder for fx_prop schemas: effect and
 * adjustment dialogs and the Save Configuration options (app_ui.h).
 * One widget per prop, in schema order, honoring ranges, steps, log and
 * percent flags, enabled_if, reset-to-default buttons (property sliders),
 * color pickers, angle dials, the pan point picker and Reseed buttons.
 * Main thread, inside a dialog body. */
#include "app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- value access ------------------------------------------------------------------ */
static uint32_t choice_count(const fx_prop *p)
{
    uint32_t n = 0;
    if (!p->choices) return 0;
    while (p->choices[n] && n < FX_MAX_CHOICES) n++;
    return n;
}

static double clampd(double v, double lo, double hi)
{
    if (!(v == v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t round_i32(double v)
{
    double r = v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5);
    if (r > 2147483647.0) r = 2147483647.0;
    if (r < -2147483648.0) r = -2147483648.0;
    return (int32_t)r;
}

double app_prop_get(const fx_prop *p, const void *params)
{
    const uint8_t *b = (const uint8_t *)params + p->offset;
    switch (p->kind) {
    case FXP_INT:
    case FXP_BOOL:
    case FXP_CHOICE:
    case FXP_SEED: {
        int32_t v;
        memcpy(&v, b, sizeof v);
        return (double)v;
    }
    case FXP_REAL:
    case FXP_ANGLE: {
        double v;
        memcpy(&v, b, sizeof v);
        return v;
    }
    case FXP_COLOR: {
        uint32_t v;
        memcpy(&v, b, sizeof v);
        return (double)v;
    }
    default:
        return 0.0;
    }
}

void app_prop_set(const fx_prop *p, void *params, double v)
{
    uint8_t *b = (uint8_t *)params + p->offset;
    switch (p->kind) {
    case FXP_INT: {
        int32_t x = round_i32(clampd(v, p->min, p->max));
        memcpy(b, &x, sizeof x);
        break;
    }
    case FXP_SEED: {
        int32_t x = round_i32(v);
        memcpy(b, &x, sizeof x);
        break;
    }
    case FXP_BOOL: {
        int32_t x = v != 0.0 ? 1 : 0;
        memcpy(b, &x, sizeof x);
        break;
    }
    case FXP_CHOICE: {
        uint32_t n = choice_count(p);
        int32_t x = round_i32(clampd(v, 0.0, n ? (double)(n - 1u) : 0.0));
        memcpy(b, &x, sizeof x);
        break;
    }
    case FXP_REAL:
    case FXP_ANGLE: {
        double x = clampd(v, p->min, p->max);
        memcpy(b, &x, sizeof x);
        break;
    }
    case FXP_COLOR: {
        uint32_t x = (uint32_t)clampd(v, 0.0, 4294967295.0);
        memcpy(b, &x, sizeof x);
        break;
    }
    default:
        break;
    }
}

void app_prop_get_point(const fx_prop *p, const void *params, double xy[2])
{
    memcpy(xy, (const uint8_t *)params + p->offset, 2u * sizeof(double));
}

void app_prop_set_point(const fx_prop *p, void *params, const double xy[2])
{
    double v[2];
    v[0] = clampd(xy[0], p->min, p->max);
    v[1] = clampd(xy[1], p->min, p->max);
    memcpy((uint8_t *)params + p->offset, v, sizeof v);
}

static uint32_t color_argb(pc_px32 c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

void app_props_defaults(app *a, const fx_prop *props, uint32_t n, void *params)
{
    for (uint32_t i = 0; i < n; i++) {
        const fx_prop *p = &props[i];
        if (p->kind == FXP_CUSTOM) continue;
        if (p->kind == FXP_POINT) {
            double xy[2];
            xy[0] = xy[1] = p->def;
            app_prop_set_point(p, params, xy);
        } else if (p->kind == FXP_COLOR) {
            uint32_t c;
            if (p->def == FX_COLOR_PRIMARY) c = color_argb(a->primary);
            else if (p->def == FX_COLOR_SECONDARY) c = color_argb(a->secondary);
            else c = (uint32_t)clampd(p->def, 0.0, 4294967295.0);
            memcpy((uint8_t *)params + p->offset, &c, sizeof c);
        } else {
            app_prop_set(p, params, p->def);
        }
    }
}

bool app_prop_enabled(const fx_prop *props, uint32_t n, const fx_prop *p, const void *params)
{
    const char *e = p->enabled_if, *eq;
    size_t klen;
    if (!e || !*e) return true;
    eq = strchr(e, '=');
    klen = eq ? (size_t)(eq - e) : strlen(e);
    for (uint32_t i = 0; i < n; i++) {
        const fx_prop *q = &props[i];
        if (strlen(q->key) != klen || memcmp(q->key, e, klen) != 0) continue;
        if (q->kind == FXP_POINT || q->kind == FXP_CUSTOM) return true;
        if (eq) return round_i32(app_prop_get(q, params)) == atoi(eq + 1);
        return app_prop_get(q, params) != 0.0;
    }
    return true;
}

/* ---- custom widgets ------------------------------------------------------------------ */
bool app_prop_widget_register(app *a, const char *hint, app_prop_widget_fn fn, void *ud)
{
    if (!hint || !fn) return false;
    if (a->npwidgets == a->cap_pwidgets) {
        int32_t nc = a->cap_pwidgets ? a->cap_pwidgets * 2 : 4;
        app_prop_widget *n = (app_prop_widget *)realloc(a->pwidgets, (size_t)nc * sizeof *n);
        if (!n) return false;
        a->pwidgets = n;
        a->cap_pwidgets = nc;
    }
    a->pwidgets[a->npwidgets].hint = app_strdup(hint);
    if (!a->pwidgets[a->npwidgets].hint) return false;
    a->pwidgets[a->npwidgets].fn = fn;
    a->pwidgets[a->npwidgets].ud = ud;
    a->npwidgets++;
    return true;
}

void app_pwidgets_free(app *a)
{
    for (int32_t i = 0; i < a->npwidgets; i++) free(a->pwidgets[i].hint);
    free(a->pwidgets);
    a->pwidgets = NULL;
    a->npwidgets = a->cap_pwidgets = 0;
}

/* ---- the builder --------------------------------------------------------------------- */
/* Color edits keep their hue while the color passes through grays. */
typedef struct color_slot { const void *params; uint32_t offset; ui_color_edit ce; } color_slot;
static color_slot g_colors[16];

static ui_color_edit *color_edit(const void *params, uint32_t offset, ui_color c)
{
    color_slot *s = NULL;
    for (int i = 0; i < 16; i++)
        if (g_colors[i].params == params && g_colors[i].offset == offset) s = &g_colors[i];
    if (!s) {
        memmove(&g_colors[1], &g_colors[0], 15u * sizeof g_colors[0]);
        s = &g_colors[0];
        s->params = params;
        s->offset = offset;
        s->ce.hsv.h = 0.0f;
        ui_color_edit_set_rgba(&s->ce, c);
    }
    if (!ui_color_eq(s->ce.rgba, c)) ui_color_edit_set_rgba(&s->ce, c);
    return &s->ce;
}

static void dim_last(app *a)
{
    ui_ctx *ui = a->ui;
    ui_rect r = ui_last_rect(ui);
    if (!ui_rect_empty(r)) ui_draw_rect(ui, r, ui_color_fade(ui_pal(ui)->panel, 0.55f));
}

static int decimals_for(double step)
{
    if (step >= 1.0) return 0;
    if (step >= 0.1) return 1;
    if (step >= 0.01) return 2;
    return 3;
}

uint32_t app_props_ui(app *a, const fx_prop *props, uint32_t n, void *params,
                      const app_props_ctx *ctx)
{
    ui_ctx *ui = a->ui;
    uint32_t res = 0;
    ui_push_id(ui, ctx && ctx->id ? ctx->id : "##props");
    for (uint32_t i = 0; i < n; i++) {
        const fx_prop *p = &props[i];
        bool en = app_prop_enabled(props, n, p, params), changed = false;
        uint32_t dis = en ? 0u : UI_DISABLED;
        uint32_t sflags = ((p->flags & FXP_F_SLIDER_LOG) && p->min > 0.0 ? UI_SLIDER_LOG : 0u) |
                          ((p->flags & FXP_F_PERCENT) ? UI_SLIDER_PERCENT : 0u);
        ui_push_id(ui, p->key);
        switch (p->kind) {
        case FXP_INT: {
            int32_t v = round_i32(app_prop_get(p, params));
            int32_t lo = round_i32(p->min), hi = round_i32(p->max), def = round_i32(p->def);
            if (ui_prop_slider_int(ui, p->label, &v, lo, hi, def, sflags | dis) && en) {
                app_prop_set(p, params, (double)v);
                changed = true;
            }
            break;
        }
        case FXP_REAL: {
            double v = app_prop_get(p, params), step = p->step > 0.0 ? p->step : 0.01;
            if (ui_prop_slider_double(ui, p->label, &v, p->min, p->max, p->def, step,
                                      decimals_for(step), sflags | dis) && en) {
                app_prop_set(p, params, v);
                changed = true;
            }
            break;
        }
        case FXP_BOOL: {
            bool v = app_prop_get(p, params) != 0.0, old = v;
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##chk", p->label);
            ui_checkbox(ui, lbl, &v);
            if (!en) dim_last(a);
            if (v != old && en) {
                app_prop_set(p, params, v ? 1.0 : 0.0);
                changed = true;
            }
            break;
        }
        case FXP_CHOICE: {
            int v = round_i32(app_prop_get(p, params)), old = v;
            uint32_t cn = choice_count(p);
            ui_label_ex(ui, p->label, dis);
            if (cn > 0u) {
                ui_combo(ui, "##choice", &v, p->choices, (int)cn);
                if (!en) dim_last(a);
            }
            if (v != old && en) {
                app_prop_set(p, params, (double)v);
                changed = true;
            }
            break;
        }
        case FXP_COLOR: {
            uint32_t c = (uint32_t)app_prop_get(p, params);
            ui_color uc = ui_argb32(c);
            ui_size cells[3];
            ui_rect sr;
            cells[0] = ui_size_fr(1.0f);
            cells[1] = ui_size_px(44.0f);
            cells[2] = ui_size_px(96.0f);
            ui_layout_row(ui, 0.0f, 3, cells);
            ui_label_ex(ui, p->label, dis);
            {
                bool click = ui_color_swatch(ui, "##swatch", uc, 0);
                sr = ui_last_rect(ui);
                if (click && en) ui_popup_open(ui, "##colorpop", sr, UI_POPUP_BELOW);
            }
            {
                ui_color_edit *ce = color_edit(params, p->offset, uc);
                if (ui_color_hex(ui, "##hex", ce) && en) {
                    app_prop_set(p, params, (double)ui_color_argb32(ce->rgba));
                    changed = true;
                }
                if (!en) dim_last(a);
                ui_layout_column(ui);
                if (ui_popup_begin(ui, "##colorpop")) {
                    ui_size w1 = ui_size_px(280.0f);
                    ui_layout_row(ui, 0.0f, 1, &w1);
                    ui_layout_begin(ui, 4.0f);
                    if (ui_color_picker(ui, "##picker", ce, 0)) {
                        app_prop_set(p, params, (double)ui_color_argb32(ce->rgba));
                        changed = true;
                    }
                    ui_layout_end(ui);
                    ui_layout_column(ui);
                    ui_popup_end(ui);
                }
            }
            break;
        }
        case FXP_ANGLE: {
            double v = app_prop_get(p, params), old = v;
            ui_label_ex(ui, p->label, dis);
            ui_angle(ui, "##angle", &v, p->min, p->max);
            if (!en) dim_last(a);
            if (v != old && en) {
                app_prop_set(p, params, v);
                changed = true;
            }
            break;
        }
        case FXP_POINT: {
            double xy[2], ox, oy;
            ui_vec2 pt;
            app_prop_get_point(p, params, xy);
            ox = xy[0];
            oy = xy[1];
            ui_label_ex(ui, p->label, dis);
            pt = ui_vec2_make((float)xy[0], (float)xy[1]);
            if (ui_point_picker(ui, "##pick", &pt, ctx ? ctx->thumb : NULL, 0.0f) && en) {
                xy[0] = (double)pt.x;
                xy[1] = (double)pt.y;
            }
            if (!en) dim_last(a);
            (void)ui_prop_slider_double(ui, "X", &xy[0], p->min, p->max, p->def, 0.01, 2,
                                        UI_SLIDER_NO_RESET | dis);
            (void)ui_prop_slider_double(ui, "Y", &xy[1], p->min, p->max, p->def, 0.01, 2,
                                        UI_SLIDER_NO_RESET | dis);
            if ((xy[0] != ox || xy[1] != oy) && en) {
                app_prop_set_point(p, params, xy);
                changed = true;
            }
            break;
        }
        case FXP_SEED: {
            ui_size cells[2];
            cells[0] = ui_size_fr(1.0f);
            cells[1] = ui_size_auto();
            ui_layout_row(ui, 0.0f, 2, cells);
            ui_label_ex(ui, p->label, dis);
            if (ui_button_ex(ui, "Reseed##seed", UI_ICON_RESET, dis) && en) {
                uint32_t s = (uint32_t)SDL_GetTicksNS() ^ (uint32_t)(SDL_GetTicksNS() >> 32);
                s ^= ctx ? ctx->seed_salt : 0u;
                s = s * 2654435761u + 0x9E3779B9u;
                app_prop_set(p, params, (double)(int32_t)(s & 0x7FFFFFFFu));
                changed = true;
            }
            ui_layout_column(ui);
            break;
        }
        case FXP_CUSTOM: {
            for (int32_t k = 0; k < a->npwidgets; k++) {
                if (!p->hint || strcmp(a->pwidgets[k].hint, p->hint) != 0) continue;
                if (a->pwidgets[k].fn(a, p, (uint8_t *)params + p->offset, a->pwidgets[k].ud))
                    changed = true;
                break;
            }
            break;
        }
        default:
            break;
        }
        ui_pop_id(ui);
        if (changed) {
            res |= APP_PROPS_CHANGED;
            if (!(p->flags & FXP_F_NO_PREVIEW)) res |= APP_PROPS_PREVIEW;
        }
        ui_layout_space(ui, 2.0f);
    }
    ui_pop_id(ui);
    return res;
}
