/* propdlg.c - the generic dialog builder for fx_prop schemas: effect and
 * adjustment dialogs (lane F) and the Save Configuration options (app_ui.h).
 *
 * One widget per prop, in schema order, laid out like Paint.NET's property
 * dialogs (OBSERVED.md section 1, docs/app/EFFECTS.md):
 *   INT, REAL    label, slider, numeric up/down, reset button (log and
 *                percent flags, step and decimals from the schema)
 *   BOOL         check box
 *   CHOICE       "Label:" and a drop-down on one row
 *   COLOR        label, color wheel, R G B A channel bars with numbers, the
 *                swatch with hex entry and a reset button; FX_COLOR_PRIMARY /
 *                FX_COLOR_SECONDARY defaults follow the palette; with
 *                FXP_F_COLOR_NO_ALPHA (fx_abi_ext.h) no alpha bar, alpha 255
 *   ANGLE        label, dial with numeric box, reset button
 *   POINT        label, pan pad over a thumbnail of the selection, X and Y
 *                sliders with numeric boxes and reset buttons
 *   SEED         a button with the prop's label ("Randomize") that reseeds
 *   CUSTOM       the widget registered for the prop's hint, hidden otherwise
 * enabled_if disables (dims) a control while its condition is false.
 *
 * Thread rules: main thread, inside a dialog body. The color editing state
 * (hue kept through grays) is per-app extension state ("propdlg.colors"). */
#include "app_internal.h"
#include "fx/afx.h"
#include "fx/fx_abi_ext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- value access ------------------------------------------------------------------ */
static uint32_t choice_count(const fx_prop *p)
{
    uint32_t n = 0;
    if (!p->choices) return 0;
    while (n < FX_MAX_CHOICES && p->choices[n]) n++;
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

void afx_prop_reset(app *a, const fx_prop *p, void *params)
{
    if (p->kind == FXP_CUSTOM) return;
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

void app_props_defaults(app *a, const fx_prop *props, uint32_t n, void *params)
{
    for (uint32_t i = 0; i < n; i++) afx_prop_reset(a, &props[i], params);
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
    for (int32_t i = 0; i < a->npwidgets; i++)
        if (strcmp(a->pwidgets[i].hint, hint) == 0) {      /* re-registration replaces */
            a->pwidgets[i].fn = fn;
            a->pwidgets[i].ud = ud;
            return true;
        }
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

/* ---- color edit state ------------------------------------------------------------------ */
/* Color edits keep their hue while the color passes through grays: the
 * HSV state per (params blob, offset) survives between frames. */
#define COLOR_SLOTS 16

typedef struct color_slot {
    const void   *params;
    uint32_t      offset;
    ui_color_edit ce;
} color_slot;

typedef struct color_cache { color_slot s[COLOR_SLOTS]; } color_cache;

static color_cache *colors_of(app *a)
{
    color_cache *c = (color_cache *)app_ext_get(a, "propdlg.colors");
    if (!c) {
        c = (color_cache *)calloc(1u, sizeof *c);
        if (c && !app_ext_set(a, "propdlg.colors", c, free)) {
            free(c);
            c = NULL;
        }
    }
    return c;
}

static ui_color_edit *color_edit(app *a, const void *params, uint32_t offset, ui_color c,
                                 ui_color_edit *fallback)
{
    color_cache *cc = colors_of(a);
    color_slot *s = NULL;
    if (!cc) {
        ui_color_edit_set_rgba(fallback, c);
        return fallback;
    }
    for (int i = 0; i < COLOR_SLOTS; i++)
        if (cc->s[i].params == params && cc->s[i].offset == offset) s = &cc->s[i];
    if (!s) {
        memmove(&cc->s[1], &cc->s[0], (size_t)(COLOR_SLOTS - 1) * sizeof cc->s[0]);
        s = &cc->s[0];
        s->params = params;
        s->offset = offset;
        s->ce.hsv.h = 0.0f;
        ui_color_edit_set_rgba(&s->ce, c);
    }
    if (!ui_color_eq(s->ce.rgba, c)) ui_color_edit_set_rgba(&s->ce, c);
    return &s->ce;
}

/* ---- property rules (W3B-FXCORE) ---------------------------------------------------------- */
/* fx_props_rules keeps, per link group, the member edited last; that state
 * lives as long as the dialog's params blob, so it is cached per blob like
 * the color edit state ("propdlg.links"). */
#define LINK_SLOTS 8
#define LINK_PROPS 64u

typedef struct link_slot {
    const void *params;
    uint32_t    n;
    uint32_t    last[LINK_PROPS];
} link_slot;

typedef struct link_cache { link_slot s[LINK_SLOTS]; } link_cache;

static uint32_t *link_state(app *a, const void *params, uint32_t n)
{
    link_cache *c = (link_cache *)app_ext_get(a, "propdlg.links");
    link_slot *s = NULL;
    if (n > LINK_PROPS) return NULL;                 /* rules then sync to the first member */
    if (!c) {
        c = (link_cache *)calloc(1u, sizeof *c);
        if (c && !app_ext_set(a, "propdlg.links", c, free)) {
            free(c);
            c = NULL;
        }
        if (!c) return NULL;
    }
    for (int i = 0; i < LINK_SLOTS; i++)
        if (c->s[i].params == params && c->s[i].n == n) s = &c->s[i];
    if (!s) {
        memmove(&c->s[1], &c->s[0], (size_t)(LINK_SLOTS - 1) * sizeof c->s[0]);
        s = &c->s[0];
        memset(s, 0, sizeof *s);
        s->params = params;
        s->n = n;
    }
    return s->last;
}

/* ---- hit rectangles (tests aim synthetic input at them) ---------------------------------- */
#define HIT_MAX 64

typedef struct prop_hit {
    char    key[FX_MAX_KEY_LEN + 1u];
    int     part;
    ui_rect r;
} prop_hit;

typedef struct hit_table {
    prop_hit h[HIT_MAX];
    int      n;
    uint64_t frame;
} hit_table;

static void hit_note(app *a, const char *key, int part, ui_rect r)
{
    hit_table *t = (hit_table *)app_ext_get(a, "propdlg.hits");
    if (!t) {
        t = (hit_table *)calloc(1u, sizeof *t);
        if (!t || !app_ext_set(a, "propdlg.hits", t, free)) {
            free(t);
            return;
        }
    }
    if (t->frame != a->frame_no) {
        t->frame = a->frame_no;
        t->n = 0;
    }
    for (int i = 0; i < t->n; i++)
        if (t->h[i].part == part && strcmp(t->h[i].key, key) == 0) {
            t->h[i].r = r;
            return;
        }
    if (t->n >= HIT_MAX) return;
    app_copy_str(t->h[t->n].key, sizeof t->h[t->n].key, key);
    t->h[t->n].part = part;
    t->h[t->n].r = r;
    t->n++;
}

ui_rect afx_prop_hit(app *a, const char *key, int part)
{
    hit_table *t = (hit_table *)app_ext_get(a, "propdlg.hits");
    for (int i = 0; t && key && i < t->n; i++)
        if (t->h[i].part == part && strcmp(t->h[i].key, key) == 0) return t->h[i].r;
    return ui_rect_make(0, 0, 0, 0);
}

/* ---- helpers ------------------------------------------------------------------------- */
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

/* The area a widget took: from the layout cursor before it (top) to the
 * cursor after it, full container width. */
static ui_rect row_span(ui_ctx *ui, ui_rect top)
{
    ui_rect now = ui_layout_rest(ui);
    return ui_rect_make(top.x, top.y, top.w, now.y - top.y);
}

/* Section header: the label followed by a thin rule (Paint.NET style). */
static void header(app *a, const char *label, uint32_t dis)
{
    ui_ctx *ui = a->ui;
    ui_size cells[2];
    ui_rect r;
    if (!label || !*label) return;
    cells[0] = ui_size_auto();
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, label, dis);
    r = ui_layout_next(ui, 0, ui_px(ui, 20.0f));
    ui_draw_rect(ui, ui_rect_make(r.x + ui_px(ui, 4.0f), r.y + r.h / 2, r.w - ui_px(ui, 4.0f), 1),
                 ui_pal(ui)->separator);
    ui_layout_column(ui);
}

/* Reset button in the current cell; true when clicked while it applies. */
static bool reset_button(app *a, bool is_default, uint32_t dis)
{
    ui_ctx *ui = a->ui;
    bool click = ui_icon_button(ui, "##reset", UI_ICON_RESET, "Reset to default");
    if (dis || is_default) dim_last(a);
    return click && !dis && !is_default;
}

/* One slider row without a label: slider, numeric box, reset (pan axes and
 * props with an empty label, such as the Posterize levels under their
 * check boxes). flags: UI_SLIDER_LOG / UI_SLIDER_EXP / UI_SLIDER_PERCENT /
 * UI_DISABLED. */
static bool slider_row(app *a, const char *id, double *v, double lo, double hi, double def,
                       double step, int dec, uint32_t flags)
{
    ui_ctx *ui = a->ui;
    ui_size cells[3];
    bool ch = false;
    uint32_t dis = flags & UI_DISABLED;
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_px(72.0f);
    cells[2] = ui_size_px(ui_get_theme(ui)->m.control_h);
    ui_push_id(ui, id);
    ui_layout_row(ui, 0.0f, 3, cells);
    ch |= ui_slider_double(ui, "##s", v, lo, hi, step, flags);
    ch |= ui_number_double(ui, "##n", v, lo, hi, step, dec,
                           flags & ~(UI_SLIDER_LOG | UI_SLIDER_EXP));
    if (reset_button(a, fabs(*v - def) < 1e-9, dis)) {
        *v = def;
        ch = true;
    }
    ui_layout_column(ui);
    ui_pop_id(ui);
    return ch && !dis;
}

static bool axis_row(app *a, const char *id, double *v, double lo, double hi, double def,
                     double step, uint32_t dis)
{
    return slider_row(a, id, v, lo, hi, def, step, decimals_for(step), dis);
}

static uint32_t reseed(const app_props_ctx *ctx)
{
    uint64_t t = SDL_GetTicksNS();
    uint32_t s = (uint32_t)t ^ (uint32_t)(t >> 32);
    s ^= ctx ? ctx->seed_salt : 0u;
    s = s * 2654435761u + 0x9E3779B9u;
    s ^= s >> 15;
    return s & 0x7FFFFFFFu;
}

/* ---- widgets per kind ------------------------------------------------------------------- */
static bool w_color(app *a, const fx_prop *p, void *params, uint32_t dis)
{
    ui_ctx *ui = a->ui;
    bool rgb = (p->flags & FXP_F_COLOR_NO_ALPHA) != 0u;      /* W3B-FXCORE */
    uint32_t c = (uint32_t)app_prop_get(p, params) | (rgb ? 0xFF000000u : 0u), def;
    ui_color_edit fallback, *ce = color_edit(a, params, p->offset, ui_argb32(c), &fallback);
    ui_size cells[2];
    bool changed = false;
    {
        uint8_t tmp[4];
        fx_prop q = *p;
        q.offset = 0;
        afx_prop_reset(a, &q, tmp);
        memcpy(&def, tmp, sizeof def);
        if (rgb) def |= 0xFF000000u;
    }
    header(a, p->label, dis);
    cells[0] = ui_size_px(124.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_layout_begin(ui, 0.0f);
    changed |= ui_color_wheel(ui, "##wheel", ce, 116.0f, 0u);
    if (dis) dim_last(a);
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    changed |= ui_color_channel(ui, "##r", UI_CHAN_RED, ce);
    if (dis) dim_last(a);
    changed |= ui_color_channel(ui, "##g", UI_CHAN_GREEN, ce);
    if (dis) dim_last(a);
    changed |= ui_color_channel(ui, "##b", UI_CHAN_BLUE, ce);
    if (dis) dim_last(a);
    if (!rgb) {
        changed |= ui_color_channel(ui, "##a", UI_CHAN_ALPHA, ce);
        if (dis) dim_last(a);
    }
    {
        ui_size row[3];
        row[0] = ui_size_auto();
        row[1] = ui_size_fr(1.0f);
        row[2] = ui_size_px(ui_get_theme(ui)->m.control_h);
        ui_layout_row(ui, 0.0f, 3, row);
        (void)ui_color_swatch(ui, "##swatch", ce->rgba, rgb ? UI_SWATCH_NO_ALPHA : 0u);
        changed |= ui_color_hex(ui, "##hex", ce);
        if (dis) dim_last(a);
        if (reset_button(a, c == def, dis)) {
            ui_color_edit_set_rgba(ce, ui_argb32(def));
            changed = true;
        }
        hit_note(a, p->key, AFX_HIT_RESET, ui_last_rect(ui));
        ui_layout_column(ui);
    }
    ui_layout_end(ui);
    ui_layout_column(ui);
    if (changed && rgb && ce->rgba.a != 255u) {      /* typed AARRGGBB: drop the alpha */
        ui_color o = ce->rgba;
        o.a = 255u;
        ui_color_edit_set_rgba(ce, o);
    }
    if (changed && !dis) {
        app_prop_set(p, params, (double)ui_color_argb32(ce->rgba));
        return true;
    }
    if (changed) ui_color_edit_set_rgba(ce, ui_argb32(c));      /* disabled: revert */
    return false;
}

static bool w_angle(app *a, const fx_prop *p, void *params, uint32_t dis)
{
    ui_ctx *ui = a->ui;
    double v = app_prop_get(p, params), old = v;
    ui_size cells[3];
    header(a, p->label, dis);
    cells[0] = ui_size_auto();
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_px(ui_get_theme(ui)->m.control_h);
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_angle(ui, "##angle", &v, p->min, p->max);
    hit_note(a, p->key, AFX_HIT_MAIN, ui_last_rect(ui));
    if (dis) dim_last(a);
    (void)ui_layout_next(ui, 0, ui_px(ui, 4.0f));
    if (reset_button(a, fabs(v - p->def) < 1e-9, dis)) v = p->def;
    hit_note(a, p->key, AFX_HIT_RESET, ui_last_rect(ui));
    ui_layout_column(ui);
    if (v != old && !dis) {
        app_prop_set(p, params, v);
        return true;
    }
    return false;
}

static bool w_point(app *a, const fx_prop *p, void *params, uint32_t dis,
                    const app_props_ctx *ctx)
{
    ui_ctx *ui = a->ui;
    double xy[2], ox, oy, step = p->step > 0.0 ? p->step : 0.01;
    ui_size cells[2];
    ui_vec2 pt;
    app_prop_get_point(p, params, xy);
    ox = xy[0];
    oy = xy[1];
    header(a, p->label, dis);
    cells[0] = ui_size_px(104.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    pt = ui_vec2_make((float)xy[0], (float)xy[1]);
    if (ui_point_picker(ui, "##pick", &pt, ctx ? ctx->thumb : NULL, 96.0f) && !dis) {
        xy[0] = (double)pt.x;
        xy[1] = (double)pt.y;
    }
    hit_note(a, p->key, AFX_HIT_MAIN, ui_last_rect(ui));
    if (dis) dim_last(a);
    ui_layout_begin(ui, 0.0f);
    ui_layout_space(ui, 16.0f);
    (void)axis_row(a, "##x", &xy[0], p->min, p->max, p->def, step, dis);
    hit_note(a, p->key, AFX_HIT_RESET, ui_last_rect(ui));
    (void)axis_row(a, "##y", &xy[1], p->min, p->max, p->def, step, dis);
    hit_note(a, p->key, AFX_HIT_RESET2, ui_last_rect(ui));
    ui_layout_end(ui);
    ui_layout_column(ui);
    if ((xy[0] != ox || xy[1] != oy) && !dis) {
        app_prop_set_point(p, params, xy);
        return true;
    }
    return false;
}

static bool w_choice(app *a, const fx_prop *p, void *params, uint32_t dis)
{
    ui_ctx *ui = a->ui;
    int v = round_i32(app_prop_get(p, params)), old = v;
    uint32_t cn = choice_count(p);
    char lbl[160];
    if (cn == 0u) return false;
    if (p->label && *p->label) {
        ui_size cells[3];
        cells[0] = ui_size_auto();
        cells[1] = ui_size_px(176.0f);
        cells[2] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 3, cells);
        snprintf(lbl, sizeof lbl, "%s:", p->label);
        ui_label_ex(ui, lbl, dis);
    }
    ui_combo(ui, "##choice", &v, p->choices, (int)cn);
    hit_note(a, p->key, AFX_HIT_MAIN, ui_last_rect(ui));
    if (dis) dim_last(a);
    ui_layout_column(ui);
    if (v != old && !dis) {
        app_prop_set(p, params, (double)v);
        return true;
    }
    return false;
}

/* ---- the builder --------------------------------------------------------------------- */
uint32_t afx_props_ui(app *a, const fx_prop *props, uint32_t n, void *params,
                      const app_props_ctx *ctx, const uint8_t *show)
{
    ui_ctx *ui = a->ui;
    uint32_t res = 0;
    ui_push_id(ui, ctx && ctx->id ? ctx->id : "##props");
    for (uint32_t i = 0; i < n; i++) {
        const fx_prop *p = &props[i];
        bool en, changed = false;
        uint32_t dis, sflags;
        if (show && !show[i]) continue;
        en = app_prop_enabled(props, n, p, params);
        dis = en ? 0u : UI_DISABLED;
        /* W3B-FXCORE: FXP_F_SLIDER_LOG is Paint.NET's non-linear radius
         * scale, which measures as a quadratic mapping for every range
         * (O-UI-NONLIN), so it maps to UI_SLIDER_EXP, min > 0 or not */
        sflags = ((p->flags & FXP_F_SLIDER_LOG) ? UI_SLIDER_EXP : 0u) |
                 ((p->flags & FXP_F_PERCENT) ? UI_SLIDER_PERCENT : 0u);
        ui_push_id(ui, p->key);
        switch (p->kind) {
        case FXP_INT: {
            int32_t v = round_i32(app_prop_get(p, params));
            ui_rect top = ui_layout_rest(ui);
            int32_t lo = round_i32(p->min), hi = round_i32(p->max), def = round_i32(p->def);
            if (!p->label || !*p->label) {
                double dv = (double)v;
                if (slider_row(a, "##row", &dv, (double)lo, (double)hi, (double)def, 1.0, 0,
                               sflags | dis) && en && round_i32(dv) != v) {
                    app_prop_set(p, params, dv);
                    changed = true;
                }
            } else if (ui_prop_slider_int(ui, p->label, &v, lo, hi, def, sflags | dis) && en) {
                app_prop_set(p, params, (double)v);
                changed = true;
            }
            hit_note(a, p->key, AFX_HIT_MAIN, row_span(ui, top));
            break;
        }
        case FXP_REAL: {
            double v = app_prop_get(p, params), step = p->step > 0.0 ? p->step : 0.01;
            ui_rect top = ui_layout_rest(ui);
            if (!p->label || !*p->label) {
                if (slider_row(a, "##row", &v, p->min, p->max, p->def, step, decimals_for(step),
                               sflags | dis) && en) {
                    app_prop_set(p, params, v);
                    changed = true;
                }
            } else if (ui_prop_slider_double(ui, p->label, &v, p->min, p->max, p->def, step,
                                             decimals_for(step), sflags | dis) && en) {
                app_prop_set(p, params, v);
                changed = true;
            }
            hit_note(a, p->key, AFX_HIT_MAIN, row_span(ui, top));
            break;
        }
        case FXP_BOOL: {
            bool v = app_prop_get(p, params) != 0.0, old = v;
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##chk", p->label);
            ui_checkbox(ui, lbl, &v);
            hit_note(a, p->key, AFX_HIT_MAIN, ui_last_rect(ui));
            if (!en) dim_last(a);
            if (v != old && en) {
                app_prop_set(p, params, v ? 1.0 : 0.0);
                changed = true;
            }
            break;
        }
        case FXP_CHOICE:
            changed = w_choice(a, p, params, dis);
            break;
        case FXP_COLOR:
            changed = w_color(a, p, params, dis);
            break;
        case FXP_ANGLE:
            changed = w_angle(a, p, params, dis);
            break;
        case FXP_POINT:
            changed = w_point(a, p, params, dis, ctx);
            break;
        case FXP_SEED: {
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##seed", p->label && *p->label ? p->label : "Reseed");
            if (ui_button_ex(ui, lbl, UI_ICON_NONE, dis) && en) {
                app_prop_set(p, params, (double)(int32_t)reseed(ctx));
                changed = true;
            }
            hit_note(a, p->key, AFX_HIT_MAIN, ui_last_rect(ui));
            break;
        }
        case FXP_CUSTOM:
            for (int32_t k = 0; k < a->npwidgets; k++) {
                if (!p->hint || strcmp(a->pwidgets[k].hint, p->hint) != 0) continue;
                if (a->pwidgets[k].fn(a, p, (uint8_t *)params + p->offset, a->pwidgets[k].ud))
                    changed = true;
                break;
            }
            break;
        default:
            break;
        }
        ui_pop_id(ui);
        if (changed) {
            /* W3B-FXCORE: linked values and soft min/max pairs follow the
             * edit (fx_run.h property rules); they show from the next frame */
            (void)fx_props_rules(props, n, params, i, link_state(a, params, n));
            res |= APP_PROPS_CHANGED;
            if (!(p->flags & FXP_F_NO_PREVIEW)) res |= APP_PROPS_PREVIEW;
        }
        ui_layout_space(ui, 4.0f);
    }
    ui_pop_id(ui);
    return res;
}

uint32_t app_props_ui(app *a, const fx_prop *props, uint32_t n, void *params,
                      const app_props_ctx *ctx)
{
    return afx_props_ui(a, props, n, params, ctx, NULL);
}
