/* mod_m_settings.c - the Settings dialog (gear button, Alt+X; command
 * app.settings), lane M (MENUS.md "Settings dialog", WINDOWS.md section 8,
 * OBSERVED.md 10).
 *
 * Page list on the left (icon and name), the page on the right, Close at
 * the bottom; every change applies at once and is persisted through the
 * settings store (saved at exit). Pages, in Paint.NET's order, with the
 * options that apply to paint.c:
 *   User Interface    language (English only so far; changing it needs a
 *                     restart), color scheme (Default = follow the system,
 *                     Blue, Light, Dark), translucent windows, scrolling
 *                     past the edge of the image, auto-scroll when drawing
 *                     at the edge of the window (lane SHELL)
 *   Canvas            drop shadow around the canvas (on), custom border
 *                     color (off; 128, 128, 128), transparency checkerboard
 *                     brightness 0.25..1.00 (0.75) with reset
 *   Tools             default tool (Paintbrush) and the defaults of every
 *                     toolbar option, shared and per tool (selection draw
 *                     mode and fixed size, Move Selected Pixels sampling,
 *                     text, gradient, color picker, shapes, line / curve,
 *                     recolor; lane SHELL), Load from Toolbar, Reset; once
 *                     set, the defaults apply at every start (keys tooldef.*
 *                     copied over tool.* before the toolbar is loaded and
 *                     again at exit, for tools that read their options when
 *                     they are created)
 *   Pen & Tablet      enable pen input (on; off = pens act as a mouse)
 *   Graphics          hardware acceleration (on), rendering device (SDL
 *                     render driver), worker threads, history memory limit
 *                     (OD-10 default: 25 % of RAM, at least 1 GiB)
 *   Color Management  use the display's color profile (off: images are
 *                     shown as sRGB), the display profile and a status line
 *                     (lane SHELL, shell_cm.c); no HDR output
 *   Plugin Errors     the plugin loader's errors (afx_plugins.c; Effects >
 *                     Plugin Errors opens this page), errors other loaders
 *                     report through m_settings_set_plugin_errors, and a
 *                     button that opens the plugins folder
 *   Diagnostics       system and app information (renderer and GPU, pointer
 *                     devices, plugin libraries), Copy to Clipboard, Open
 *                     Crash Log Folder (crash logs: pal_crash.h)
 * The Updates page is not applicable (no updater; distribution channels
 * own updates).
 *
 * Settings keys: ui.language, ui.theme (app.c), view.overscroll (app.c),
 * ui.translucent, ui.autoscroll, cm.use_display (lane SHELL),
 * canvas.shadow, canvas.border_custom, canvas.border_color (#RRGGBB),
 * canvas.checker, pen.enabled, gfx.software, gfx.renderer, gfx.workers,
 * history.limit_mb, tooldef.<option> and tooldef.tool.
 *
 * Thread rules: main thread. Ownership: the dialog state is owned by the
 * dialog stack, the plugin error list by the app (app_ext). */
#include "../app_internal.h"
#include "../edit/m_settings.h"
#include "../edit/m_ui.h"
#include "../fx/afx.h"
#include "../shell_ext.h"
#include "../tools/text_font.h"
#include "pal/pal_crash.h"
#include "pc/pc_gradient.h"
#include "pc/pc_pattern.h"
#include "pc/pc_shapes.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tool defaults (TOOLS.md section 12) ----------------------------------------------- */
enum { TF_FLOAT, TF_INT, TF_BOOL };
typedef struct tool_field {
    const char *name;           /* suffix of tool.<name> (tool.c) and tooldef.<name> */
    int         type;
    size_t      off;
    double      lo, hi;
} tool_field;

static const tool_field k_tool_fields[] = {
    { "width", TF_FLOAT, offsetof(app_tool_settings, width), 1.0, 2000.0 },
    { "pressure", TF_BOOL, offsetof(app_tool_settings, pressure), 0, 1 },
    { "hardness", TF_INT, offsetof(app_tool_settings, hardness), 0, 100 },
    { "spacing", TF_INT, offsetof(app_tool_settings, spacing), 1, 1000 },
    { "smoothing", TF_BOOL, offsetof(app_tool_settings, smoothing), 0, 1 },
    { "fill", TF_INT, offsetof(app_tool_settings, fill), 0, 53 },
    { "antialias", TF_BOOL, offsetof(app_tool_settings, antialias), 0, 1 },
    { "blend", TF_INT, offsetof(app_tool_settings, blend), 0, (double)PC_BLEND_COUNT },
    { "sel_clip_aa", TF_BOOL, offsetof(app_tool_settings, sel_clip_aa), 0, 1 },
    { "sel_mode", TF_INT, offsetof(app_tool_settings, sel_mode), 0,
      (double)(PC_SEL_MODE_COUNT - 1) },
    { "flood_global", TF_BOOL, offsetof(app_tool_settings, flood_global), 0, 1 },
    { "tolerance", TF_INT, offsetof(app_tool_settings, tolerance), 0, 100 },
    { "tol_straight", TF_BOOL, offsetof(app_tool_settings, tol_straight), 0, 1 },
    { "sampling", TF_INT, offsetof(app_tool_settings, sampling), 0, 1 },
};
#define N_TOOL_FIELDS (sizeof k_tool_fields / sizeof k_tool_fields[0])

static void key_of(char *out, size_t cap, const char *prefix, const char *name)
{
    snprintf(out, cap, "%s.%s", prefix, name);
}

void m_tooldef_get(app *a, app_tool_settings *out)
{
    const app_settings *s = app_settings_of(a);
    uint8_t *base = (uint8_t *)out;
    app_tool_settings_reset(out);
    for (size_t i = 0; i < N_TOOL_FIELDS; i++) {
        const tool_field *f = &k_tool_fields[i];
        char key[64];
        key_of(key, sizeof key, "tooldef", f->name);
        if (!app_settings_get(s, key)) continue;
        if (f->type == TF_FLOAT) {
            double v = app_settings_double(s, key, 2.0);
            if (!(v >= f->lo)) v = f->lo;
            if (v > f->hi) v = f->hi;
            *(float *)(void *)(base + f->off) = (float)v;
        } else if (f->type == TF_INT) {
            int64_t v = app_settings_int(s, key, 0);
            if (v < (int64_t)f->lo) v = (int64_t)f->lo;
            if (v > (int64_t)f->hi) v = (int64_t)f->hi;
            *(int32_t *)(void *)(base + f->off) = (int32_t)v;
        } else {
            *(bool *)(void *)(base + f->off) = app_settings_bool(s, key, false);
        }
    }
}

void m_tooldef_set(app *a, const app_tool_settings *v)
{
    app_settings *s = app_settings_of(a);
    const uint8_t *base = (const uint8_t *)v;
    /* tooldef.tool marks that defaults exist (they apply at start) */
    if (!app_settings_get(s, "tooldef.tool"))
        (void)app_settings_set(s, "tooldef.tool", m_tooldef_tool(a));
    for (size_t i = 0; i < N_TOOL_FIELDS; i++) {
        const tool_field *f = &k_tool_fields[i];
        char key[64];
        key_of(key, sizeof key, "tooldef", f->name);
        if (f->type == TF_FLOAT)
            (void)app_settings_set_double(s, key,
                                          (double)*(const float *)(const void *)(base + f->off));
        else if (f->type == TF_INT)
            (void)app_settings_set_int(s, key, *(const int32_t *)(const void *)(base + f->off));
        else
            (void)app_settings_set_bool(s, key, *(const bool *)(const void *)(base + f->off));
    }
}

const char *m_tooldef_tool(app *a)
{
    const char *t = app_settings_get(app_settings_of(a), "tooldef.tool");
    return t && *t && app_tool_find(a, t) ? t : "paintbrush";
}

static void opts_reset(app_settings *s);
static void opts_from_toolbar(app_settings *s);

void m_tooldef_reset(app *a)
{
    app_tool_settings f;
    app_tool_settings_reset(&f);
    (void)app_settings_set(app_settings_of(a), "tooldef.tool", "paintbrush");
    m_tooldef_set(a, &f);
    opts_reset(app_settings_of(a));            /* lane SHELL: per-tool options */
}

void m_tooldef_load_from_toolbar(app *a)
{
    const app_tool *t = app_tool_current(a);
    m_tooldef_set(a, app_tool_settings_get(a));
    if (t) (void)app_settings_set(app_settings_of(a), "tooldef.tool", t->id);
    opts_from_toolbar(app_settings_of(a));     /* lane SHELL */
}

/* ---- per-tool defaults (lane SHELL, TOOLS.md 12, OBSERVED 9 and 10) ----------------------- */
/* Options a tool keeps under tool.<key> in the settings store (the tool
 * reads them; factory values as the tools use them when the key is
 * absent). Settings > Tools stores the defaults as tooldef.<key>. */
enum { TO_CHOICE, TO_BOOL, TO_NUM, TO_TEXT };
typedef struct tool_opt {
    const char         *key;
    const char         *label;
    int                 type;
    double              def, lo, hi;      /* TO_NUM range; TO_CHOICE / TO_BOOL default */
    int                 decimals;
    const char *const  *names;            /* TO_CHOICE labels, or name_fn */
    const char       *(*name_fn)(int i);
    int                 n;
    const int          *values;           /* stored value of each label (NULL = index) */
} tool_opt;

typedef struct tool_group {
    const char     *title;
    const tool_opt *opts;
    int             n;
} tool_group;

static const char *const k_draw_modes[] = { "Any Size", "Fixed Ratio", "Fixed Size" };
static const char *const k_size_units[] = { "Same as the view", "Pixels", "Inches",
                                            "Centimeters" };
static const int k_size_unit_values[] = { -1, 0, 1, 2 };
static const char *const k_mp_sampling[] = { "Nearest Neighbor", "Bilinear",
                                             "Multisample Bilinear", "Anisotropic", "Bicubic" };
static const char *const k_text_units[] = { "Points (image DPI)", "Fixed (96 DPI)" };
static const char *const k_text_modes[] = { "Smooth", "Sharp (Modern)", "Sharp (Classic)" };
static const char *const k_text_align[] = { "Left", "Center", "Right" };
static const char *const k_pick_size[] = { "Single Pixel", "3 \xC3\x97 3 pixels",
                                           "5 \xC3\x97 5 pixels", "11 \xC3\x97 11 pixels",
                                           "31 \xC3\x97 31 pixels", "51 \xC3\x97 51 pixels" };
static const char *const k_pick_after[] = { "Do not switch tool", "Switch to previous tool",
                                            "Switch to Pencil tool" };
static const char *const k_shape_draw[] = { "Draw Shape Outline", "Draw Filled Shape",
                                            "Draw Filled Shape With Outline" };
static const char *const k_curve_types[] = { "Straight", "Spline", "Bezier" };
static const char *const k_caps[] = { "Flat", "Arrow", "Filled Arrow", "Rounded" };
static const int k_cap_values[] = { 0, 3, 4, 1 };      /* PC_CAP_BUTT, ARROW, ARROW_FILLED, ROUND */
static const char *const k_dashes[] = { "Solid", "Dashes", "Dotted", "Dash, Dot",
                                        "Dash, Dot, Dot" };
static const char *const k_recolor[] = { "Sampling Once", "Sampling Secondary Color" };

static const char *grad_type(int i) { return pc_grad_type_name((pc_grad_type)i); }
static const char *grad_mode(int i) { return pc_grad_mode_name((pc_grad_mode)i); }
static const char *grad_repeat(int i) { return pc_grad_repeat_name((pc_grad_repeat)i); }
static const char *shape_name(int i) { return pc_shape_name((pc_shape_kind)i); }

#define CH(k, l, d, names, n) { k, l, TO_CHOICE, d, 0, 0, 0, names, NULL, n, NULL }
#define CF(k, l, d, fn, n) { k, l, TO_CHOICE, d, 0, 0, 0, NULL, fn, n, NULL }
#define BO(k, l, d) { k, l, TO_BOOL, d, 0, 1, 0, NULL, NULL, 0, NULL }
#define NU(k, l, d, lo, hi, dec) { k, l, TO_NUM, d, lo, hi, dec, NULL, NULL, 0, NULL }

static const tool_opt k_opts_rect[] = {
    CH("rect_select.draw_mode", "Draw mode:", 0, k_draw_modes, 3),
    NU("rect_select.ratio_w", "Fixed ratio width:", 4.0, 0.01, 65535.0, 2),
    NU("rect_select.ratio_h", "Fixed ratio height:", 3.0, 0.01, 65535.0, 2),
    NU("rect_select.size_w", "Fixed width:", 400.0, 0.01, 65535.0, 2),
    NU("rect_select.size_h", "Fixed height:", 300.0, 0.01, 65535.0, 2),
    { "rect_select.size_units", "Fixed size units:", TO_CHOICE, -1, 0, 0, 0, k_size_units, NULL,
      4, k_size_unit_values },
};
static const tool_opt k_opts_move[] = {
    CH("move_pixels.sampling", "Sampling:", 4, k_mp_sampling, 5),
    BO("move_pixels.gamma", "Gamma corrected", 1),
};
static const tool_opt k_opts_text[] = {
    { "text.font", "Font:", TO_TEXT, 0, 0, 0, 0, NULL, NULL, 0, NULL },
    NU("text.size", "Size:", 12.0, 1.0, 2000.0, 2),
    CH("text.unit", "Size unit:", 0, k_text_units, 2),
    BO("text.bold", "Bold", 0),
    BO("text.italic", "Italic", 0),
    BO("text.underline", "Underline", 0),
    BO("text.strikeout", "Strikethrough", 0),
    CH("text.mode", "Rendering:", 0, k_text_modes, 3),
    CH("text.align", "Alignment:", 0, k_text_align, 3),
};
static const tool_opt k_opts_grad[] = {
    CF("gradient.type", "Type:", 0, grad_type, PC_GRAD_TYPE_COUNT),
    CF("gradient.mode", "Mode:", 0, grad_mode, 2),
    CF("gradient.repeat", "Repeat:", 0, grad_repeat, PC_GRAD_REPEAT_COUNT),
};
static const tool_opt k_opts_pick[] = {
    CH("color_picker.size", "Sampling size:", 0, k_pick_size, 6),
    CH("color_picker.after", "After click:", 0, k_pick_after, 3),
};
static const tool_opt k_opts_shape[] = {
    CF("shapes.kind", "Shape:", 0, shape_name, PC_SHAPE_BUILTIN_COUNT),
    CH("shapes.draw", "Draw mode:", 0, k_shape_draw, 3),
    NU("shapes.corner", "Corner size:", 10.0, 0.0, 2000.0, 2),
};
static const tool_opt k_opts_line[] = {
    CH("line_curve.type", "Curve type:", 1, k_curve_types, 3),
    { "line_curve.start_cap", "Start cap:", TO_CHOICE, 0, 0, 0, 0, k_caps, NULL, 4, k_cap_values },
    { "line_curve.end_cap", "End cap:", TO_CHOICE, 0, 0, 0, 0, k_caps, NULL, 4, k_cap_values },
    CH("dash", "Dash style:", 0, k_dashes, 5),
};
static const tool_opt k_opts_recolor[] = {
    CH("recolor.sampling", "Sampling:", 0, k_recolor, 2),
};

#undef CH
#undef CF
#undef BO
#undef NU

#define NOPTS(x) ((int)(sizeof x / sizeof x[0]))
static const tool_group k_groups[] = {
    { "Rectangle Select", k_opts_rect, NOPTS(k_opts_rect) },
    { "Move Selected Pixels", k_opts_move, NOPTS(k_opts_move) },
    { "Text", k_opts_text, NOPTS(k_opts_text) },
    { "Gradient", k_opts_grad, NOPTS(k_opts_grad) },
    { "Color Picker", k_opts_pick, NOPTS(k_opts_pick) },
    { "Shapes", k_opts_shape, NOPTS(k_opts_shape) },
    { "Line / Curve and Shapes style", k_opts_line, NOPTS(k_opts_line) },
    { "Recolor", k_opts_recolor, NOPTS(k_opts_recolor) },
};
#define N_GROUPS ((int)(sizeof k_groups / sizeof k_groups[0]))

static const char *opt_label(const tool_opt *o, int i)
{
    return o->names ? o->names[i] : (o->name_fn ? o->name_fn(i) : "");
}

static int choice_value(const tool_opt *o, int i) { return o->values ? o->values[i] : i; }

static int choice_index(const tool_opt *o, int64_t v)
{
    for (int i = 0; i < o->n; i++)
        if (choice_value(o, i) == (int)v) return i;
    return -1;
}

/* Is the stored text of an option valid? (wrong values fall back to the
 * factory default) */
static bool opt_valid(const tool_opt *o, const app_settings *s, const char *key)
{
    if (!app_settings_get(s, key)) return false;
    if (o->type == TO_CHOICE) return choice_index(o, app_settings_int(s, key, -99)) >= 0;
    if (o->type == TO_NUM) {
        double v = app_settings_double(s, key, o->def);
        return v >= o->lo && v <= o->hi;
    }
    return true;
}

/* Write the factory value of o under prefix.<key>. size_units "same as
 * the view" and the default font are stored too (apply removes the tool
 * key for the view-units case). */
static void opt_store_factory(app_settings *s, const char *prefix, const tool_opt *o)
{
    char key[96];
    key_of(key, sizeof key, prefix, o->key);
    switch (o->type) {
    case TO_CHOICE: (void)app_settings_set_int(s, key, (int64_t)o->def); break;
    case TO_BOOL: (void)app_settings_set_bool(s, key, o->def != 0.0); break;
    case TO_NUM: (void)app_settings_set_double(s, key, o->def); break;
    default: (void)app_settings_set(s, key, TEXT_DEFAULT_FAMILY); break;
    }
}

static void opts_reset(app_settings *s)
{
    for (int g = 0; g < N_GROUPS; g++)
        for (int i = 0; i < k_groups[g].n; i++)
            opt_store_factory(s, "tooldef", &k_groups[g].opts[i]);
}

/* Load from Toolbar: the tool.<key> values the tools wrote (factory
 * values for options never changed). */
static void opts_from_toolbar(app_settings *s)
{
    for (int g = 0; g < N_GROUPS; g++)
        for (int i = 0; i < k_groups[g].n; i++) {
            const tool_opt *o = &k_groups[g].opts[i];
            char tkey[96], dkey[96];
            key_of(tkey, sizeof tkey, "tool", o->key);
            key_of(dkey, sizeof dkey, "tooldef", o->key);
            if (opt_valid(o, s, tkey)) (void)app_settings_set(s, dkey, app_settings_get(s, tkey));
            else if (strcmp(o->key, "rect_select.size_units") == 0)
                (void)app_settings_set_int(s, dkey, -1);
            else opt_store_factory(s, "tooldef", o);
        }
}

/* tooldef.<key> -> tool.<key> for every per-tool option (a missing or
 * invalid default, and "same as the view" units, remove the tool key so
 * the tool's own default applies). */
static void opts_apply(app_settings *s)
{
    for (int g = 0; g < N_GROUPS; g++)
        for (int i = 0; i < k_groups[g].n; i++) {
            const tool_opt *o = &k_groups[g].opts[i];
            char tkey[96], dkey[96];
            key_of(tkey, sizeof tkey, "tool", o->key);
            key_of(dkey, sizeof dkey, "tooldef", o->key);
            if (opt_valid(o, s, dkey) && !(o->values == k_size_unit_values &&
                                           app_settings_int(s, dkey, -1) < 0))
                (void)app_settings_set(s, tkey, app_settings_get(s, dkey));
            else
                (void)app_settings_remove(s, tkey);
        }
}

/* Start of the app: once tool defaults exist (tooldef.tool is set by Load
 * from Toolbar, Reset or an edit on the Tools page), the toolbar starts
 * from them as in Paint.NET, which does not carry toolbar values over a
 * restart. Until then paint.c keeps the wave 2a behavior of remembering
 * the last toolbar (tests/app/test_app_settings.c). The shell loads tool.*
 * after every module ran, so writing them here is enough. */
static void apply_tool_defaults_at_start(app *a)
{
    app_settings *s = app_settings_of(a);
    if (!app_settings_get(s, "tooldef.tool")) return;
    for (size_t i = 0; i < N_TOOL_FIELDS; i++) {
        char dkey[64], tkey[64];
        const char *v;
        key_of(dkey, sizeof dkey, "tooldef", k_tool_fields[i].name);
        key_of(tkey, sizeof tkey, "tool", k_tool_fields[i].name);
        v = app_settings_get(s, dkey);
        if (v) (void)app_settings_set(s, tkey, v);
        else (void)app_settings_remove(s, tkey);
    }
    (void)app_settings_set(s, "tool.current", m_tooldef_tool(a));
    opts_apply(s);                             /* lane SHELL: per-tool options */
}

/* Lane SHELL: tools that read their options when they are created (before
 * the modules run) see the defaults at the next start, because the exit
 * writes them over the toolbar values (the settings are saved after the
 * quit hooks). */
static void tool_defaults_at_quit(app *a, app_doc *d, void *ud)
{
    app_settings *s = app_settings_of(a);
    (void)d;
    (void)ud;
    if (s && app_settings_get(s, "tooldef.tool")) opts_apply(s);
}

/* ---- preferences -> app state ------------------------------------------------------------ */
static pc_px32 parse_rgb(const char *s, pc_px32 def)
{
    unsigned v = 0;
    if (!s) return def;
    if (*s == '#') s++;
    if (strlen(s) != 6u || sscanf(s, "%6x", &v) != 1) return def;
    return app_px_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, 255);
}

size_t m_settings_auto_history_budget(void)
{
    uint64_t ram = pal_ram_bytes(), b = ram / 4u;
    if (b < ((uint64_t)1u << 30)) b = (uint64_t)1u << 30;
#if SIZE_MAX < UINT64_MAX
    if (b > ((uint64_t)1u << 30)) b = (uint64_t)1u << 30;   /* 32-bit address space */
#endif
    return (size_t)b;
}

void m_settings_apply(app *a)
{
    const app_settings *s = app_settings_of(a);
    double ck = app_settings_double(s, "canvas.checker", 0.75);
    int64_t mb = app_settings_int(s, "history.limit_mb", 0);
    a->m_cv_no_shadow = !app_settings_bool(s, "canvas.shadow", true);
    a->m_cv_border_on = app_settings_bool(s, "canvas.border_custom", false);
    a->m_cv_border = parse_rgb(app_settings_get(s, "canvas.border_color"),
                               app_px_make(128, 128, 128, 255));
    if (!(ck >= 0.25)) ck = 0.25;
    if (ck > 1.0) ck = 1.0;
    a->m_cv_checker = (float)ck;
    a->m_pen_off = !app_settings_bool(s, "pen.enabled", true);
    /* lane SHELL: User Interface and Color Management */
    app_canvas_set_autoscroll(a, app_settings_bool(s, "ui.autoscroll", true));
    app_panels_set_translucent(a, app_settings_bool(s, "ui.translucent", true));
    app_cm_set_use_display(a, app_settings_bool(s, "cm.use_display", false));
    if (mb > 0) {
        uint64_t b = (uint64_t)mb << 20;
#if SIZE_MAX < UINT64_MAX
        if (b > (uint64_t)SIZE_MAX) b = (uint64_t)SIZE_MAX;
#endif
        a->hist_budget = (size_t)b;
    } else {
        a->hist_budget = m_settings_auto_history_budget();
    }
    for (int32_t i = 0; i < app_doc_count(a); i++) app_doc_history_changed(a, app_doc_at(a, i));
    app_request_frame(a);
}

/* ---- plugin errors ------------------------------------------------------------------------- */
#define PLUGIN_ERR_KEY "lane_m.plugin_errors"

typedef struct plugin_errors {
    int    n;
    char **file, **detail;      /* owned */
} plugin_errors;

static void plugin_errors_free(void *p)
{
    plugin_errors *e = (plugin_errors *)p;
    if (!e) return;
    for (int i = 0; i < e->n; i++) {
        free(e->file[i]);
        free(e->detail[i]);
    }
    free(e->file);
    free(e->detail);
    free(e);
}

bool m_settings_set_plugin_errors(app *a, const char *const *files, const char *const *details,
                                  int n)
{
    plugin_errors *e;
    if (n <= 0) return app_ext_set(a, PLUGIN_ERR_KEY, NULL, NULL);
    e = (plugin_errors *)calloc(1u, sizeof *e);
    if (!e) return false;
    e->file = (char **)calloc((size_t)n, sizeof *e->file);
    e->detail = (char **)calloc((size_t)n, sizeof *e->detail);
    if (!e->file || !e->detail) {
        plugin_errors_free(e);
        return false;
    }
    e->n = n;
    for (int i = 0; i < n; i++) {
        e->file[i] = app_strdup(files[i] ? files[i] : "");
        e->detail[i] = app_strdup(details && details[i] ? details[i] : "");
        if (!e->file[i] || !e->detail[i]) {
            plugin_errors_free(e);
            return false;
        }
    }
    if (!app_ext_set(a, PLUGIN_ERR_KEY, e, plugin_errors_free)) {
        plugin_errors_free(e);
        return false;
    }
    return true;
}

/* ---- folders and links ----------------------------------------------------------------------- */
void m_settings_folder(app *a, int which, char *out, size_t cap)
{
    const char *base = pal_dir(which == 0 ? PAL_DIR_DATA : PAL_DIR_STATE);
    /* lane SHELL: a private --config-dir keeps the crash logs with it */
    if (which == 1 && a && a->opts.config_dir && a->opts.config_dir[0]) base = a->opts.config_dir;
    pal_path_join(out, cap, base ? base : "", which == 0 ? "plugins" : "crash");
}

/* Open a folder in the file manager (created on demand). Headless apps
 * (tests) only record the request. */
static void open_folder(app *a, int which)
{
    char dir[1024];
    m_settings_folder(a, which, dir, sizeof dir);
    (void)pal_mkdirs(dir);
    m_open_url(a, dir);
}

/* ---- the dialog ---------------------------------------------------------------------------- */
enum { PG_UI = 0, PG_CANVAS, PG_TOOLS, PG_PEN, PG_GFX, PG_CM, PG_PLUGINS, PG_DIAG, PG_COUNT };

static const char *const k_pages[PG_COUNT] = { "User Interface", "Canvas", "Tools",
                                               "Pen & Tablet", "Graphics", "Color Management",
                                               "Plugin Errors", "Diagnostics" };
static const ui_icon k_page_icons[PG_COUNT] = {
    UI_ICON_SETTINGS, UI_ICON_IMAGE, UI_ICON_TOOL_PAINTBRUSH, UI_ICON_TOOL_PENCIL,
    UI_ICON_GRID, UI_ICON_PALETTE, UI_ICON_WARNING, UI_ICON_INFO
};

typedef struct settings_dlg {
    app          *a;             /* borrowed (lane SHELL: m_settings_page) */
    int           page;
    ui_color_edit border;        /* border color being edited */
    int           plugin_sel;    /* selected plugin error */
    char          copied[64];    /* feedback text of Copy to Clipboard */
} settings_dlg;

static void check_pref(app *a, const char *label, const char *key, bool def)
{
    bool v = app_settings_bool(app_settings_of(a), key, def);
    if (ui_checkbox(a->ui, label, &v)) {
        (void)app_settings_set_bool(app_settings_of(a), key, v);
        m_settings_apply(a);
    }
}

static void dim_text(app *a, const char *text)
{
    ui_text_wrapped(a->ui, text, UI_LABEL_DIM);
}

static void page_ui(app *a)
{
    ui_ctx *ui = a->ui;
    static const char *const langs[] = { "English" };
    /* OBSERVED 10: Default, Blue, Light, Dark (Default follows the system) */
    static const char *const themes[] = { "Default (follow the system)", "Blue", "Light",
                                          "Dark" };
    static const app_theme_pref k_theme_of[] = { APP_THEME_AUTO, APP_THEME_BLUE, APP_THEME_LIGHT,
                                                 APP_THEME_DARK };
    int lang = 0, t = 0;
    bool os = a->overscroll;
    for (int i = 0; i < 4; i++)
        if (k_theme_of[i] == app_theme(a)) t = i;
    ui_heading(ui, "Language");
    if (ui_combo(ui, "##lang", &lang, langs, 1))
        (void)app_settings_set(app_settings_of(a), "ui.language", "en");
    dim_text(a, "paint.c is available in English. Changing the language takes effect after "
                "restarting paint.c.");
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Color scheme");
    if (ui_radio_group(ui, "##theme", &t, themes, 4, false) && t >= 0 && t < 4)
        app_set_theme(a, k_theme_of[t]);
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Windows");
    /* lane SHELL: utility windows fade while the pointer is elsewhere */
    check_pref(a, "Translucent windows##translucent", "ui.translucent", true);
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Canvas navigation");
    if (ui_checkbox(ui, "Scrolling past the edge of the image (overscroll)##os", &os)) {
        a->overscroll = os;
        app_request_frame(a);
    }
    /* lane SHELL: V-AUTOSCROLL */
    check_pref(a, "Auto-scroll when drawing at the edge of the window##autoscroll",
               "ui.autoscroll", true);
}

static void page_canvas(app *a, settings_dlg *g)
{
    ui_ctx *ui = a->ui;
    app_settings *s = app_settings_of(a);
    bool custom = a->m_cv_border_on;
    double ck = a->m_cv_checker > 0.0f ? (double)a->m_cv_checker : 0.75;
    ui_size cells[2];
    check_pref(a, "Draw a shadow around the canvas##shadow", "canvas.shadow", true);
    ui_layout_space(ui, 6.0f);
    cells[0] = ui_size_auto();
    cells[1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 2, cells);
    if (ui_checkbox(ui, "Use a custom color for the canvas border##bcust", &custom)) {
        (void)app_settings_set_bool(s, "canvas.border_custom", custom);
        m_settings_apply(a);
    }
    if (ui_color_swatch(ui, "##bswatch", app_px_to_ui(a->m_cv_border),
                        UI_SWATCH_NO_ALPHA | (custom ? 0u : UI_DISABLED)) && custom) {
        ui_color_edit_set_rgba(&g->border, app_px_to_ui(a->m_cv_border));
        ui_popup_open(ui, "##bpopup", ui_last_rect(ui), UI_POPUP_BELOW);
    }
    ui_layout_column(ui);
    if (ui_popup_begin(ui, "##bpopup")) {
        if (ui_color_picker(ui, "##bpicker", &g->border, UI_PICKER_NO_ALPHA)) {
            char hex[16];
            snprintf(hex, sizeof hex, "#%02X%02X%02X", g->border.rgba.r, g->border.rgba.g,
                     g->border.rgba.b);
            (void)app_settings_set(s, "canvas.border_color", hex);
            m_settings_apply(a);
        }
        ui_popup_end(ui);
    }
    ui_layout_space(ui, 10.0f);
    ui_heading(ui, "Transparency checkerboard brightness");
    if (m_slider_row(a, "##checker", NULL, &ck, 0.25, 1.0, 0.75, 0.01, 2, 0)) {
        (void)app_settings_set_double(s, "canvas.checker", ck);
        m_settings_apply(a);
    }
}

static const char *const k_blend_names[] = { "Normal",     "Multiply",    "Additive",
                                             "Color Burn", "Color Dodge", "Reflect",
                                             "Glow",       "Overlay",     "Difference",
                                             "Negation",   "Lighten",     "Darken",
                                             "Screen",     "Xor",         "Overwrite" };
static const char *const k_sel_modes[] = { "Replace", "Add (union)", "Subtract", "Intersect",
                                           "Invert (xor)" };
static const char *const k_clip[] = { "Pixelated", "Antialiased" };
static const char *const k_flood[] = { "Contiguous", "Global" };
static const char *const k_sampling[] = { "Layer", "Image" };
static const char *const k_tol_alpha[] = { "Premultiplied", "Straight" };

/* Lane SHELL: the per-tool option defaults, grouped by tool. Editing one
 * marks that defaults exist (tooldef.tool), like the shared options. */
static void page_tool_options(app *a, ui_size cells[2])
{
    ui_ctx *ui = a->ui;
    app_settings *s = app_settings_of(a);
    for (int g = 0; g < N_GROUPS; g++) {
        ui_heading(ui, k_groups[g].title);
        ui_layout_row(ui, 0.0f, 2, cells);
        for (int i = 0; i < k_groups[g].n; i++) {
            const tool_opt *o = &k_groups[g].opts[i];
            char key[96], wid[112];
            bool changed = false;
            key_of(key, sizeof key, "tooldef", o->key);
            snprintf(wid, sizeof wid, "##td_%s", o->key);
            if (o->type == TO_BOOL) {
                bool v = opt_valid(o, s, key) ? app_settings_bool(s, key, false) : o->def != 0.0;
                (void)ui_layout_next(ui, 0, 0);
                snprintf(wid, sizeof wid, "%s##td_%s", o->label, o->key);
                if (ui_checkbox(ui, wid, &v)) {
                    (void)app_settings_set_bool(s, key, v);
                    changed = true;
                }
                if (changed && !app_settings_get(s, "tooldef.tool"))
                    (void)app_settings_set(s, "tooldef.tool", m_tooldef_tool(a));
                continue;
            }
            ui_label_ex(ui, o->label, UI_LABEL_DIM);
            if (o->type == TO_CHOICE) {
                const char *names[PC_SHAPE_BUILTIN_COUNT + 4];
                int idx = choice_index(o, opt_valid(o, s, key) ? app_settings_int(s, key, 0)
                                                                : (int64_t)o->def);
                int n = o->n < (int)(sizeof names / sizeof names[0]) ? o->n
                                                                     : (int)(sizeof names /
                                                                             sizeof names[0]);
                for (int k = 0; k < n; k++) names[k] = opt_label(o, k);
                if (idx < 0) idx = 0;
                if (ui_combo(ui, wid, &idx, names, n) && idx >= 0 && idx < n) {
                    (void)app_settings_set_int(s, key, choice_value(o, idx));
                    changed = true;
                }
            } else if (o->type == TO_NUM) {
                double v = opt_valid(o, s, key) ? app_settings_double(s, key, o->def) : o->def;
                if (ui_number_double(ui, wid, &v, o->lo, o->hi, 1.0, o->decimals, 0)) {
                    (void)app_settings_set_double(s, key, v);
                    changed = true;
                }
            } else {
                /* the font: the families paint.c found, or a typed name */
                text_fonts *tf = text_fonts_get(a);
                const char *cur = app_settings_get(s, key);
                int32_t nf = tf ? text_fonts_family_count(tf) : 0;
                if (!cur || !*cur) cur = TEXT_DEFAULT_FAMILY;
                if (nf > 0) {
                    const char **fam = (const char **)malloc((size_t)nf * sizeof *fam);
                    int idx = (int)text_fonts_find_family(tf, cur);
                    if (fam) {
                        for (int32_t k = 0; k < nf; k++) fam[k] = text_fonts_family(tf, k);
                        if (idx < 0) idx = 0;
                        if (ui_combo(ui, wid, &idx, fam, (int)nf) && idx >= 0 && idx < nf) {
                            (void)app_settings_set(s, key, fam[idx]);
                            changed = true;
                        }
                        free(fam);
                    }
                } else {
                    char buf[96];
                    app_copy_str(buf, sizeof buf, cur);
                    if (ui_text_field(ui, wid, buf, sizeof buf, 0) & UI_EDIT_CHANGED) {
                        (void)app_settings_set(s, key, buf[0] ? buf : TEXT_DEFAULT_FAMILY);
                        changed = true;
                    }
                }
            }
            if (changed && !app_settings_get(s, "tooldef.tool"))
                (void)app_settings_set(s, "tooldef.tool", m_tooldef_tool(a));
        }
        ui_layout_column(ui);
    }
}

static void page_tools(app *a)
{
    ui_ctx *ui = a->ui;
    app_tool_settings def;
    const char *names[64];
    const char *fills[PC_FILL_STYLE_COUNT];
    int ntools = 0, cur = 0;
    bool changed = false;
    ui_size cells[2];
    const char *deftool = m_tooldef_tool(a);
    m_tooldef_get(a, &def);
    dim_text(a, "These are the toolbar settings every tool starts with when paint.c starts "
                "(until defaults are set, paint.c keeps the last toolbar). Load from Toolbar "
                "copies the current toolbar; Reset restores the factory defaults.");
    ui_layout_space(ui, 4.0f);
    cells[0] = ui_size_auto();
    cells[1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 2, cells);
    if (ui_button_ex(ui, "Load from Toolbar##tdload", UI_ICON_NONE, 0)) {
        m_tooldef_load_from_toolbar(a);
        m_tooldef_get(a, &def);
        deftool = m_tooldef_tool(a);
    }
    if (ui_button_ex(ui, "Reset##tdreset", UI_ICON_RESET, 0)) {
        m_tooldef_reset(a);
        m_tooldef_get(a, &def);
        deftool = m_tooldef_tool(a);
    }
    ui_layout_column(ui);
    ui_layout_space(ui, 6.0f);
    for (int32_t i = 0; i < app_tool_count(a) && ntools < 64; i++) {
        const app_tool *t = app_tool_at(a, i);
        if (strcmp(t->id, deftool) == 0) cur = ntools;
        names[ntools++] = t->name;
    }
    cells[0] = ui_size_px(150.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Default tool:", UI_LABEL_DIM);
    if (ui_combo(ui, "##deftool", &cur, names, ntools) && cur < ntools)
        (void)app_settings_set(app_settings_of(a), "tooldef.tool", app_tool_at(a, cur)->id);
    ui_layout_column(ui);

    ui_heading(ui, "Brush and fill");
    ui_layout_row(ui, 0.0f, 2, cells);
    {
        double w = (double)def.width;
        int32_t hard = def.hardness, spacing = def.spacing, fill = def.fill;
        int blend = def.blend;
        ui_label_ex(ui, "Brush width:", UI_LABEL_DIM);
        if (ui_number_double(ui, "##tdw", &w, 1.0, 2000.0, 1.0, 0, 0)) {
            def.width = (float)w;
            changed = true;
        }
        ui_label_ex(ui, "Hardness:", UI_LABEL_DIM);
        if (ui_number_int(ui, "##tdh", &hard, 0, 100, 1, UI_SLIDER_PERCENT)) {
            def.hardness = hard;
            changed = true;
        }
        ui_label_ex(ui, "Spacing:", UI_LABEL_DIM);
        if (ui_number_int(ui, "##tdsp", &spacing, 1, 1000, 1, UI_SLIDER_PERCENT)) {
            def.spacing = spacing;
            changed = true;
        }
        ui_label_ex(ui, "Fill:", UI_LABEL_DIM);
        for (int i = 0; i < (int)PC_FILL_STYLE_COUNT; i++)
            fills[i] = pc_fill_style_name((pc_fill_style)i);
        if (ui_combo(ui, "##tdfill", &fill, fills, (int)PC_FILL_STYLE_COUNT)) {
            def.fill = fill;
            changed = true;
        }
        ui_label_ex(ui, "Blend mode:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdblend", &blend, k_blend_names, (int)PC_BLEND_COUNT + 1)) {
            def.blend = blend;
            changed = true;
        }
    }
    ui_layout_column(ui);
    if (ui_checkbox(ui, "Antialiasing##tdaa", &def.antialias)) changed = true;
    if (ui_checkbox(ui, "Pen pressure changes the brush width##tdpr", &def.pressure))
        changed = true;
    if (ui_checkbox(ui, "Smoothing##tdsm", &def.smoothing)) changed = true;

    ui_heading(ui, "Selection");
    ui_layout_row(ui, 0.0f, 2, cells);
    {
        int mode = def.sel_mode, clip = def.sel_clip_aa ? 1 : 0;
        ui_label_ex(ui, "Selection mode:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdsel", &mode, k_sel_modes, 5)) {
            def.sel_mode = mode;
            changed = true;
        }
        ui_label_ex(ui, "Selection clipping:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdclip", &clip, k_clip, 2)) {
            def.sel_clip_aa = clip == 1;
            changed = true;
        }
    }
    ui_layout_column(ui);

    ui_heading(ui, "Magic Wand and Paint Bucket");
    ui_layout_row(ui, 0.0f, 2, cells);
    {
        int flood = def.flood_global ? 1 : 0, samp = def.sampling, ta = def.tol_straight ? 1 : 0;
        int32_t tol = def.tolerance;
        ui_label_ex(ui, "Flood mode:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdflood", &flood, k_flood, 2)) {
            def.flood_global = flood == 1;
            changed = true;
        }
        ui_label_ex(ui, "Tolerance:", UI_LABEL_DIM);
        if (ui_number_int(ui, "##tdtol", &tol, 0, 100, 1, UI_SLIDER_PERCENT)) {
            def.tolerance = tol;
            changed = true;
        }
        ui_label_ex(ui, "Sampling:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdsamp", &samp, k_sampling, 2)) {
            def.sampling = samp;
            changed = true;
        }
        ui_label_ex(ui, "Tolerance alpha:", UI_LABEL_DIM);
        if (ui_combo(ui, "##tdta", &ta, k_tol_alpha, 2)) {
            def.tol_straight = ta == 1;
            changed = true;
        }
    }
    ui_layout_column(ui);
    if (changed) m_tooldef_set(a, &def);
    page_tool_options(a, cells);
}

static void page_pen(app *a)
{
    check_pref(a, "Enable pen input##pen", "pen.enabled", true);
    dim_text(a, "When pen input is off, pens and tablets act like a mouse: no pressure and no "
                "eraser tip.");
}

static void page_gfx(app *a)
{
    ui_ctx *ui = a->ui;
    app_settings *s = app_settings_of(a);
    bool hw = !app_settings_bool(s, "gfx.software", false);
    const char *names[24];
    const char *cur = app_settings_get(s, "gfx.renderer");
    int n = 0, sel = 0, nd = SDL_GetNumRenderDrivers();
    int32_t workers = (int32_t)app_settings_int(s, "gfx.workers", 0);
    int32_t mb = (int32_t)app_settings_int(s, "history.limit_mb", 0);
    ui_size cells[2];
    char line[200];
    if (ui_checkbox(ui, "Use hardware acceleration for the user interface and the canvas##hw",
                    &hw))
        (void)app_settings_set_bool(s, "gfx.software", !hw);
    names[n++] = "Default";
    for (int i = 0; i < nd && n < 24; i++) {
        const char *d = SDL_GetRenderDriver(i);
        if (!d) continue;
        if (cur && strcmp(cur, d) == 0) sel = n;
        names[n++] = d;
    }
    cells[0] = ui_size_px(190.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Rendering device:", UI_LABEL_DIM);
    if (ui_combo(ui, "##renderer", &sel, names, n))
        (void)app_settings_set(s, "gfx.renderer", sel == 0 ? "" : names[sel]);
    ui_label_ex(ui, "Worker threads:", UI_LABEL_DIM);
    if (ui_number_int(ui, "##workers", &workers, 0, (int32_t)pal_cpu_count(), 1, 0))
        (void)app_settings_set_int(s, "gfx.workers", workers);
    ui_layout_column(ui);
    snprintf(line, sizeof line,
             "In use: %s on %s, %u worker threads. Changes take effect after restarting "
             "paint.c. 0 worker threads means automatic.",
             a->ren && SDL_GetRendererName(a->ren) ? SDL_GetRendererName(a->ren) : "?",
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "no video driver",
             (unsigned)pc_par_threads(&a->par));
    dim_text(a, line);
    ui_layout_space(ui, 10.0f);
    ui_heading(ui, "History");
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Memory limit per image (MiB):", UI_LABEL_DIM);
    if (ui_number_int(ui, "##histmb", &mb, 0, 1 << 22, 64, 0)) {
        (void)app_settings_set_int(s, "history.limit_mb", mb);
        m_settings_apply(a);
    }
    ui_layout_column(ui);
    snprintf(line, sizeof line,
             "0 means automatic: a quarter of the memory, at least 1 GiB. In use now: %.0f MiB. "
             "The oldest steps are dropped when an image's history needs more.",
             (double)a->hist_budget / 1048576.0);
    dim_text(a, line);
}

static void page_cm(app *a)
{
    ui_ctx *ui = a->ui;
    char desc[160], line[400];
    bool use = app_cm_use_display(a);
    /* lane SHELL (shell_cm.c): images are converted from their profile to
     * sRGB, or to the display's profile when the platform reports one */
    if (ui_checkbox(ui, "Use the display's color profile##cmdisp", &use)) {
        (void)app_settings_set_bool(app_settings_of(a), "cm.use_display", use);
        m_settings_apply(a);
    }
    app_cm_display_describe(a, desc, sizeof desc);
    snprintf(line, sizeof line, "Display profile: %s",
             desc[0] ? desc
                     : "none reported (Wayland and some X11 setups do not provide one)");
    dim_text(a, line);
    ui_layout_space(ui, 6.0f);
    ui_heading(ui, "Status");
    app_cm_status(a, line, sizeof line);
    ui_text_wrapped(ui, line, 0);
    ui_label_ex(ui, "Advanced color (HDR and wide color gamut) output: not available",
                UI_LABEL_DIM | UI_DISABLED);
    dim_text(a, "Without the display profile, colors are accurate when the display itself is "
                "set to sRGB.");
}

static void page_plugins(app *a, settings_dlg *g)
{
    ui_ctx *ui = a->ui;
    const plugin_errors *e = (const plugin_errors *)app_ext_get(a, PLUGIN_ERR_KEY);
    size_t nload = afx_plugins_error_count(afx_app_plugins(a));
    char dir[1024];
    /* lane SHELL: the plugin loader's own errors (file list and details) */
    if (nload > 0u || !e || e->n == 0) afx_plugin_errors_ui(a, 300.0f);
    if (e && e->n > 0) {
        if (nload > 0u) ui_layout_space(ui, 8.0f);
        for (int i = 0; i < e->n; i++) {
            ui_push_id_int(ui, i);
            (void)ui_radio(ui, e->file[i], &g->plugin_sel, i);
            ui_pop_id(ui);
        }
        if (g->plugin_sel >= 0 && g->plugin_sel < e->n) {
            ui_layout_space(ui, 6.0f);
            ui_text_wrapped(ui, e->detail[g->plugin_sel], 0);
        }
    }
    ui_layout_space(ui, 10.0f);
    m_settings_folder(a, 0, dir, sizeof dir);
    {
        char line[1100];
        snprintf(line, sizeof line, "Plugins are loaded from %s", dir);
        dim_text(a, line);
    }
    if (ui_button_ex(ui, "Open Plugins Folder##plugdir", UI_ICON_OPEN, 0)) open_folder(a, 0);
}

void m_settings_diagnostics(app *a, char *out, size_t cap)
{
    int v = SDL_GetVersion();
    char dir[1024];
    size_t k = 0;
    m_settings_folder(a, 1, dir, sizeof dir);
#define LINE(...)                                                                   \
    do {                                                                            \
        int w_ = snprintf(out + k, k < cap ? cap - k : 0u, __VA_ARGS__);           \
        if (w_ > 0) k += (size_t)w_;                                                \
        if (k >= cap) k = cap ? cap - 1u : 0u;                                      \
    } while (0)
    if (cap) out[0] = '\0';
    LINE("%s %s\n", APP_NAME, APP_VERSION);
    LINE("Platform: %s\n", SDL_GetPlatform());
    LINE("SDL: %d.%d.%d\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
         SDL_VERSIONNUM_MICRO(v));
    LINE("Video driver: %s\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    LINE("Renderer: %s\n", a->ren && SDL_GetRendererName(a->ren) ? SDL_GetRendererName(a->ren)
                                                                 : "?");
    {
        /* lane SHELL: graphics adapter and driver (WINDOWS.md 8.9) */
        char gpu[640];
        gfx_renderer_describe(a->ren, gpu, sizeof gpu);
        LINE("Graphics: %s\n", gpu);
    }
    LINE("Display scale: %.2f\n", (double)ui_scale(a->ui));
    LINE("Logical processors: %u, worker threads: %u\n", (unsigned)pal_cpu_count(),
         (unsigned)pc_par_threads(&a->par));
    LINE("Memory: %.1f GiB, history limit per image: %.0f MiB\n",
         (double)pal_ram_bytes() / (1024.0 * 1024.0 * 1024.0),
         (double)a->hist_budget / 1048576.0);
    /* lane SHELL: pointer devices (mice, touch screens and touchpads that
     * report fingers; pens are listed once they were used) */
    {
        int nm = 0, nt = 0;
        SDL_MouseID *mice = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetMice(&nm) : NULL;
        SDL_TouchID *touch = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetTouchDevices(&nt) : NULL;
        LINE("Pointer devices: %d mouse device%s, %d touch device%s%s\n", nm, nm == 1 ? "" : "s",
             nt, nt == 1 ? "" : "s", app_canvas_pen_seen(a) ? ", a pen" : "");
        for (int i = 0; i < nm && i < 8; i++) {
            const char *n = SDL_GetMouseNameForID(mice[i]);
            LINE("  Mouse: %s\n", n && *n ? n : "(unnamed)");
        }
        for (int i = 0; i < nt && i < 8; i++) {
            const char *n = SDL_GetTouchDeviceName(touch[i]);
            SDL_TouchDeviceType ty = SDL_GetTouchDeviceType(touch[i]);
            LINE("  Touch: %s (%s)\n", n && *n ? n : "(unnamed)",
                 ty == SDL_TOUCH_DEVICE_DIRECT ? "touch screen" : "touchpad");
        }
        SDL_free(mice);
        SDL_free(touch);
    }
    LINE("Effects and adjustments: %u\n", (unsigned)fx_registry_count(a->fx));
    /* lane SHELL: loaded plugin libraries */
    {
        afx_plugins *pl = afx_app_plugins(a);
        size_t nl = afx_plugins_lib_count(pl);
        const char *seen[16];
        size_t ns = 0;
        LINE("Plugin libraries: %u loaded, %u failed\n", (unsigned)nl,
             (unsigned)afx_plugins_error_count(pl));
        for (uint32_t i = 0; pl && a->fx && i < fx_registry_count(a->fx) && ns < 16u; i++) {
            const afx_plugin_info *in = afx_plugins_info(pl, fx_registry_at(a->fx, i));
            bool dup = false;
            if (!in || !in->path) continue;
            for (size_t q = 0; q < ns; q++)
                if (strcmp(seen[q], in->path) == 0) dup = true;
            if (dup) continue;
            seen[ns++] = in->path;
            LINE("  %s%s%s\n", in->path, in->version && *in->version ? ", version " : "",
                 in->version ? in->version : "");
        }
    }
    LINE("Open images: %d\n", (int)app_doc_count(a));
    LINE("Settings: %s\n", a->settings_enabled ? a->settings_path : "(not saved)");
    LINE("Crash logs: %s (%d)\n", dir, pal_crash_count(dir));
#undef LINE
}

static void page_diag(app *a, settings_dlg *g)
{
    ui_ctx *ui = a->ui;
    char text[4096];
    const char *p = text;
    ui_size cells[3];
    m_settings_diagnostics(a, text, sizeof text);
    ui_layout_set_spacing(ui, 3.0f);
    while (*p) {
        const char *e = strchr(p, '\n');
        char line[1200];
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1u;
        memcpy(line, p, n);
        line[n] = '\0';
        ui_text_wrapped(ui, line, 0);
        p = e ? e + 1 : p + n;
    }
    ui_layout_set_spacing(ui, ui_get_theme(ui)->m.spacing);
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_auto();
    cells[1] = ui_size_auto();
    cells[2] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    if (ui_button_ex(ui, "Copy to Clipboard##diagcopy", UI_ICON_COPY, 0)) {
        bool ok = SDL_WasInit(SDL_INIT_VIDEO) && pal_clip_set_text(text);
        app_copy_str(g->copied, sizeof g->copied, ok ? "Copied." : "Could not copy.");
    }
    if (ui_button_ex(ui, "Open Crash Log Folder##crashdir", UI_ICON_OPEN, 0)) open_folder(a, 1);
    ui_label_ex(ui, g->copied, UI_LABEL_DIM);
    ui_layout_column(ui);
}

static void page_list(app *a, settings_dlg *g, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t rh = ui_px(ui, 34.0f), isz = ui_px(ui, 18.0f), pad = ui_px(ui, 10.0f);
    ui_draw_rect(ui, r, p->panel_header);
    for (int i = 0; i < PG_COUNT; i++) {
        ui_rect row = ui_rect_make(r.x + ui_px(ui, 4.0f), r.y + ui_px(ui, 4.0f) + i * rh,
                                   r.w - ui_px(ui, 8.0f), rh - ui_px(ui, 2.0f));
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, 7000 + i), row,
                                        UI_INTERACT_FOCUSABLE);
        bool sel = g->page == i;
        if (in.clicked) g->page = i;
        if (sel || in.hovered)
            ui_draw_rrect(ui, row, (float)ui_px(ui, 4.0f), sel ? p->selection : p->hover);
        ui_draw_icon(ui, k_page_icons[i], ui_rect_make(row.x + pad / 2, row.y, isz + pad, row.h),
                     isz, sel ? p->selection_text : p->icon, p->icon_accent);
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                         ui_rect_make(row.x + isz + pad * 2, row.y, row.w - isz - pad * 2, row.h),
                         UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, sel ? p->selection_text : p->text,
                         k_pages[i], strlen(k_pages[i]));
        if (in.focused && ui_focus_visible(ui)) ui_draw_rect_outline(ui, row, 1, p->focus);
    }
    /* Up / Down move between pages while a page entry has the focus */
    {
        ui_id f = ui_focus_id(ui);
        bool on_list = false;
        for (int i = 0; i < PG_COUNT; i++)
            if (f == ui_get_id_int(ui, 7000 + i)) on_list = true;
        if (on_list) {
            if (ui_key_take(ui, SDLK_UP, 0) && g->page > 0) g->page--;
            if (ui_key_take(ui, SDLK_DOWN, 0) && g->page + 1 < PG_COUNT) g->page++;
        }
    }
}

static bool settings_frame(app *a, void *st)
{
    settings_dlg *g = (settings_dlg *)st;
    ui_ctx *ui = a->ui;
    ui_rect body, list, page;
    int32_t lw = ui_px(ui, 190.0f), gap = ui_px(ui, 12.0f);
    uint32_t r;
    ui_dialog_begin(ui, "Settings##settings", 780.0f, 0.0f);
    body = ui_layout_next(ui, 0, ui_px(ui, 470.0f));
    list = ui_rect_make(body.x, body.y, lw, body.h);
    page = ui_rect_make(body.x + lw + gap, body.y, body.w - lw - gap, body.h);
    page_list(a, g, list);
    ui_scroll_begin(ui, "##settings_page", page, UI_SCROLL_NO_BG);
    ui_layout_push(ui, ui_layout_content(ui), 2.0f);
    ui_push_id_int(ui, g->page);
    ui_heading(ui, k_pages[g->page]);
    ui_layout_space(ui, 4.0f);
    switch (g->page) {
    case PG_UI: page_ui(a); break;
    case PG_CANVAS: page_canvas(a, g); break;
    case PG_TOOLS: page_tools(a); break;
    case PG_PEN: page_pen(a); break;
    case PG_GFX: page_gfx(a); break;
    case PG_CM: page_cm(a); break;
    case PG_PLUGINS: page_plugins(a, g); break;
    default: page_diag(a, g); break;
    }
    ui_pop_id(ui);
    ui_layout_pop(ui);
    ui_scroll_end(ui);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    return r == 0;
}

static bool settings_open_pred(app *a, const app_cmd *c)
{
    (void)c;
    return !app_dialog_active(a);
}

#define OPEN_KEY "shell.settings_open"

static void settings_free(void *p)
{
    settings_dlg *g = (settings_dlg *)p;
    if (!g) return;
    if (g->a && app_ext_get(g->a, OPEN_KEY) == (void *)g)
        (void)app_ext_set(g->a, OPEN_KEY, NULL, NULL);
    free(g);
}

void m_settings_open(app *a, int page)
{
    settings_dlg *g;
    if (app_dialog_active(a)) return;
    g = (settings_dlg *)calloc(1u, sizeof *g);
    if (!g) return;
    g->a = a;
    g->page = page >= 0 && page < PG_COUNT ? page : 0;
    if (app_dialog_push(a, settings_frame, g, settings_free))
        (void)app_ext_set(a, OPEN_KEY, g, NULL);
}

int m_settings_page(const app *a)
{
    const settings_dlg *g = (const settings_dlg *)app_ext_get(a, OPEN_KEY);
    return g ? g->page : -1;
}

static void cmd_settings(app *a, const app_cmd *c)
{
    (void)c;
    m_settings_open(a, 0);
}

void mod_m_settings(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "app.settings";
    d.label = "Settings";
    d.icon = UI_ICON_SETTINGS;
    d.flags = APP_CMD_NO_COMMIT;
    d.run = cmd_settings;
    d.enabled = settings_open_pred;
    (void)app_cmd_register(a, &d);
    apply_tool_defaults_at_start(a);
    (void)app_hook_add(a, APP_HOOK_QUIT, tool_defaults_at_quit, NULL);   /* lane SHELL */
    m_settings_apply(a);
    /* lane SHELL: crash logs for the interactive app (never for headless
     * runs and tests); the folder keeps the newest PAL_CRASH_KEEP logs */
    if (a->win && !a->opts.headless) {
        char dir[1024], info[600];
        int v = SDL_GetVersion();
        m_settings_folder(a, 1, dir, sizeof dir);
        if (pal_mkdirs(dir)) {
            snprintf(info, sizeof info, "%s %s\nPlatform: %s\nSDL: %d.%d.%d\nVideo driver: %s",
                     APP_NAME, APP_VERSION, SDL_GetPlatform(), SDL_VERSIONNUM_MAJOR(v),
                     SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
                     SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
            (void)pal_crash_prune(dir, PAL_CRASH_KEEP);
            (void)pal_crash_install(dir, info);
        }
    }
}
