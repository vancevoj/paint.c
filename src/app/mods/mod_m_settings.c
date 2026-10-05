/* mod_m_settings.c - the Settings dialog (gear button, Alt+X; command
 * app.settings), lane M (MENUS.md "Settings dialog", WINDOWS.md section 8,
 * OBSERVED.md 10).
 *
 * Page list on the left (icon and name), the page on the right, Close at
 * the bottom; every change applies at once and is persisted through the
 * settings store (saved at exit). Pages, in Paint.NET's order, with the
 * options that apply to paint.c:
 *   User Interface    language (English only so far; changing it needs a
 *                     restart), color scheme (Automatic, Light, Dark),
 *                     scrolling past the edge of the image
 *   Canvas            drop shadow around the canvas (on), custom border
 *                     color (off; 128, 128, 128), transparency checkerboard
 *                     brightness 0.25..1.00 (0.75) with reset
 *   Tools             default tool (Paintbrush) and the defaults of every
 *                     shared toolbar option, Load from Toolbar, Reset; once
 *                     set, the defaults apply at every start (keys tooldef.*
 *                     copied over tool.* before the toolbar is loaded)
 *   Pen & Tablet      enable pen input (on; off = pens act as a mouse)
 *   Graphics          hardware acceleration (on), rendering device (SDL
 *                     render driver), worker threads, history memory limit
 *                     (OD-10 default: 25 % of RAM, at least 1 GiB)
 *   Color Management  status (paint.c shows images as sRGB; no HDR output)
 *   Plugin Errors     plugin load errors (none until a loader reports some
 *                     through m_settings_set_plugin_errors) and a button
 *                     that opens the plugins folder
 *   Diagnostics       system and app information, Copy to Clipboard, Open
 *                     Crash Log Folder
 * The Updates page is not applicable (no updater; distribution channels
 * own updates).
 *
 * Settings keys: ui.language, ui.theme (app.c), view.overscroll (app.c),
 * canvas.shadow, canvas.border_custom, canvas.border_color (#RRGGBB),
 * canvas.checker, pen.enabled, gfx.software, gfx.renderer, gfx.workers,
 * history.limit_mb, tooldef.<option> and tooldef.tool.
 *
 * Thread rules: main thread. Ownership: the dialog state is owned by the
 * dialog stack, the plugin error list by the app (app_ext). */
#include "../app_internal.h"
#include "../edit/m_settings.h"
#include "../edit/m_ui.h"
#include "pc/pc_pattern.h"

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

void m_tooldef_reset(app *a)
{
    app_tool_settings f;
    app_tool_settings_reset(&f);
    (void)app_settings_set(app_settings_of(a), "tooldef.tool", "paintbrush");
    m_tooldef_set(a, &f);
}

void m_tooldef_load_from_toolbar(app *a)
{
    const app_tool *t = app_tool_current(a);
    m_tooldef_set(a, app_tool_settings_get(a));
    if (t) (void)app_settings_set(app_settings_of(a), "tooldef.tool", t->id);
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
    (void)a;
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
    static const char *const themes[] = { "Automatic (follow the system)", "Light", "Dark" };
    int lang = 0, t = (int)app_theme(a);
    bool os = a->overscroll;
    ui_heading(ui, "Language");
    if (ui_combo(ui, "##lang", &lang, langs, 1))
        (void)app_settings_set(app_settings_of(a), "ui.language", "en");
    dim_text(a, "paint.c is available in English. Changing the language takes effect after "
                "restarting paint.c.");
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Color scheme");
    if (ui_radio_group(ui, "##theme", &t, themes, 3, false)) app_set_theme(a, (app_theme_pref)t);
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Canvas navigation");
    if (ui_checkbox(ui, "Allow scrolling past the edge of the image##os", &os)) {
        a->overscroll = os;
        app_request_frame(a);
    }
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
    ui_label_ex(a->ui, "Advanced color (HDR and wide color gamut) output: not available",
                UI_LABEL_DIM | UI_DISABLED);
    dim_text(a, "Status: standard dynamic range. paint.c shows images as sRGB; images with "
                "another color profile are shown without conversion. Advanced color output "
                "is not available.");
}

static void page_plugins(app *a, settings_dlg *g)
{
    ui_ctx *ui = a->ui;
    const plugin_errors *e = (const plugin_errors *)app_ext_get(a, PLUGIN_ERR_KEY);
    char dir[1024];
    if (!e || e->n == 0) {
        ui_label(ui, "There were no errors while loading plugins.");
    } else {
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
    LINE("Display scale: %.2f\n", (double)ui_scale(a->ui));
    LINE("Logical processors: %u, worker threads: %u\n", (unsigned)pal_cpu_count(),
         (unsigned)pc_par_threads(&a->par));
    LINE("Memory: %.1f GiB, history limit per image: %.0f MiB\n",
         (double)pal_ram_bytes() / (1024.0 * 1024.0 * 1024.0),
         (double)a->hist_budget / 1048576.0);
    LINE("Effects and adjustments: %u\n", (unsigned)fx_registry_count(a->fx));
    LINE("Open images: %d\n", (int)app_doc_count(a));
    LINE("Settings: %s\n", a->settings_enabled ? a->settings_path : "(not saved)");
    LINE("Crash logs: %s\n", dir);
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

void m_settings_open(app *a, int page)
{
    settings_dlg *g;
    if (app_dialog_active(a)) return;
    g = (settings_dlg *)calloc(1u, sizeof *g);
    if (!g) return;
    g->page = page >= 0 && page < PG_COUNT ? page : 0;
    (void)app_dialog_push(a, settings_frame, g, free);
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
    m_settings_apply(a);
}
