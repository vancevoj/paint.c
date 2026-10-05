/* mod_help.c - About dialog (MENUS.md About), the Settings dialog
 * (WINDOWS.md section 8, the pages paint.c supports so far) and shell
 * commands: brush width keys (K-TB-WIDTH-*). The online Help items need a
 * project website and stay unregistered (disabled) until there is one. */
#include "../app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- About ---------------------------------------------------------------------- */
static const char k_credits[] =
    "paint.c reproduces the Paint.NET 5.1 workflow as an independent, clean-room "
    "implementation. Paint.NET is a trademark of dotPDN LLC; paint.c is not affiliated "
    "with it.\n\n"
    "Blend math and several algorithms are derived from the MIT-licensed Paint.NET 3.36 "
    "source, Copyright (C) dotPDN LLC, Rick Brewster, Chris Crosetto, Tom Jackson, Michael "
    "Kelsey, Brandon Ortiz, Craig Taylor, Chris Trevino and Luke Walker; portions Copyright "
    "(C) Microsoft Corporation and (C) 2006-2008 Ed Harvey. See NOTICE.\n\n"
    "Third-party components: SDL 3 (zlib), zlib (zlib), libspng (BSD-2-Clause), "
    "libjpeg-turbo (IJG, BSD-3-Clause, zlib), libwebp (BSD-3-Clause), Little-CMS (MIT), "
    "bcdec, stb_dxt and bc7enc (MIT or public domain), stb_truetype (MIT or public domain), "
    "Inter font (SIL OFL 1.1).";

static bool about_frame(app *a, void *st)
{
    ui_ctx *ui = a->ui;
    char line[160];
    uint32_t r;
    (void)st;
    ui_dialog_begin(ui, "About paint.c##about", 520.0f, 0.0f);
    ui_heading(ui, APP_NAME);
    snprintf(line, sizeof line, "Version %s  (renderer: %s)", APP_VERSION,
             SDL_GetRendererName(a->ren) ? SDL_GetRendererName(a->ren) : "?");
    ui_label_ex(ui, line, UI_LABEL_DIM);
    ui_layout_space(ui, 6.0f);
    ui_text_wrapped(ui, k_credits, 0);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    if (r) a->about_open = false;
    return r == 0;
}

static void cmd_about(app *a, const app_cmd *c)
{
    (void)c;
    if (a->about_open) return;
    a->about_open = app_dialog_push(a, about_frame, NULL, NULL);
}

/* ---- Settings ------------------------------------------------------------------------ */
typedef struct settings_dlg { int page; } settings_dlg;

static void page_ui(app *a)
{
    ui_ctx *ui = a->ui;
    static const char *const themes[] = { "Automatic (follow the system)", "Light", "Dark" };
    int t = (int)a->theme;
    bool os = a->overscroll;
    ui_heading(ui, "Color scheme");
    if (ui_radio_group(ui, "##theme", &t, themes, 3, false)) app_set_theme(a, (app_theme_pref)t);
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Canvas");
    if (ui_checkbox(ui, "Allow scrolling past the edge of the image##os", &os)) {
        a->overscroll = os;
        app_request_frame(a);
    }
}

static void page_tools(app *a)
{
    ui_ctx *ui = a->ui;
    ui_text_wrapped(ui, "Tool options are remembered between sessions. Reset puts every "
                        "shared option back to its default.", UI_LABEL_DIM);
    ui_layout_space(ui, 6.0f);
    if (ui_button_ex(ui, "Reset Tool Options##tsreset", UI_ICON_RESET, 0)) {
        app_tool_settings_reset(&a->ts);
        app_tool_settings_changed(a);
    }
}

static void page_diag(app *a)
{
    ui_ctx *ui = a->ui;
    char line[1200];
    snprintf(line, sizeof line, "%s %s", APP_NAME, APP_VERSION);
    ui_label(ui, line);
    snprintf(line, sizeof line, "Video driver: %s",
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    ui_label(ui, line);
    snprintf(line, sizeof line, "Renderer: %s",
             SDL_GetRendererName(a->ren) ? SDL_GetRendererName(a->ren) : "?");
    ui_label(ui, line);
    snprintf(line, sizeof line, "Logical processors: %u, memory: %.1f GiB",
             (unsigned)pal_cpu_count(), (double)pal_ram_bytes() / (1024.0 * 1024.0 * 1024.0));
    ui_label(ui, line);
    snprintf(line, sizeof line, "Effects: %u", (unsigned)fx_registry_count(a->fx));
    ui_label(ui, line);
    snprintf(line, sizeof line, "Settings: %s",
             a->settings_enabled ? a->settings_path : "(not saved)");
    ui_text_wrapped(ui, line, UI_LABEL_DIM);
}

static bool settings_frame(app *a, void *st)
{
    settings_dlg *s = (settings_dlg *)st;
    ui_ctx *ui = a->ui;
    static const char *const pages[] = { "User Interface", "Tools", "Diagnostics" };
    uint32_t r;
    ui_dialog_begin(ui, "Settings##settings", 560.0f, 0.0f);
    ui_tabs(ui, "##settings_pages", &s->page, pages, 3);
    ui_layout_space(ui, 6.0f);
    if (s->page == 0) page_ui(a);
    else if (s->page == 1) page_tools(a);
    else page_diag(a);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    return r == 0;
}

static void cmd_settings(app *a, const app_cmd *c)
{
    settings_dlg *s;
    (void)c;
    if (app_dialog_active(a)) return;
    s = (settings_dlg *)calloc(1u, sizeof *s);
    if (s) (void)app_dialog_push(a, settings_frame, s, free);
}

/* ---- brush width keys ---------------------------------------------------------------- */
static bool uses_width(app *a, const app_cmd *c)
{
    const app_tool *t = app_tool_current(a);
    (void)c;
    return t && (t->flags & APP_TOOL_USES_WIDTH);
}

static void cmd_width(app *a, const app_cmd *c)
{
    float w = a->ts.width + (float)c->arg;
    if (w < 1.0f) w = 1.0f;
    if (w > 2000.0f) w = 2000.0f;
    a->ts.width = w;
    app_tool_settings_changed(a);
}

static void cmd_reset_windows(app *a, const app_cmd *c)
{
    (void)c;
    app_panels_reset_all(a);
}

static void reg(app *a, const char *id, const char *label, ui_icon icon, uint32_t flags,
                app_cmd_fn run, app_cmd_pred en, intptr_t arg)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = flags;
    d.run = run;
    d.enabled = en;
    d.arg = arg;
    (void)app_cmd_register(a, &d);
}

void mod_help(app *a)
{
    reg(a, "help.about", "About", UI_ICON_INFO, APP_CMD_NO_COMMIT, cmd_about, NULL, 0);
    reg(a, "app.settings", "Settings", UI_ICON_SETTINGS, APP_CMD_NO_COMMIT, cmd_settings, NULL, 0);
    reg(a, "window.reset_all", "Reset Windows", UI_ICON_RESET, APP_CMD_NO_COMMIT,
        cmd_reset_windows, NULL, 0);
    reg(a, "tool.width_dec", "Decrease Brush Width", UI_ICON_MINUS,
        APP_CMD_NO_COMMIT | APP_CMD_REPEAT, cmd_width, uses_width, -1);
    reg(a, "tool.width_inc", "Increase Brush Width", UI_ICON_PLUS,
        APP_CMD_NO_COMMIT | APP_CMD_REPEAT, cmd_width, uses_width, 1);
    reg(a, "tool.width_dec5", "Decrease Brush Width by 5", UI_ICON_MINUS,
        APP_CMD_NO_COMMIT | APP_CMD_REPEAT, cmd_width, uses_width, -5);
    reg(a, "tool.width_inc5", "Increase Brush Width by 5", UI_ICON_PLUS,
        APP_CMD_NO_COMMIT | APP_CMD_REPEAT, cmd_width, uses_width, 5);
}
