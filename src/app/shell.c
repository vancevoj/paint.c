/* shell.c - the main window layout (docs/inventory/WINDOWS.md section 1,
 * lane P):
 *   top row    menus, the image list (panels/pnl_imagelist.c), the window
 *              toggles (Ctrl+Shift+click resets), Settings and Help
 *   toolbar 1  New, Open, Save, Print | Cut, Copy, Paste | Crop to
 *              Selection, Deselect | Undo, Redo | Pixel Grid, Rulers; every
 *              button mirrors its command's enabled and checked state
 *   toolbar 2  tool chooser and the active tool's options (tool.c)
 *   workspace  the canvas with the floating windows
 *   status bar tool help or status, progress, image size, pointer
 *              position, selection size, units menu, zoom box, quick
 *              zoom, zoom out, zoom slider, zoom in (WINDOWS.md 3)
 * Main thread. */
#include "app_internal.h"
#include "panels/pnl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_DIP     42.0f
#define TOOLBAR_DIP 38.0f
#define STATUS_DIP  30.0f

typedef struct status_state {
    char     fields[PNL_SF_COUNT][96];   /* texts shown in the last frame */
    bool     zoom_edit;          /* the zoom box is being typed into */
    bool     zoom_focus;         /* focus the field in this frame */
    char     zoom_buf[32];
    double   last_x, last_y;     /* last pointer position over the canvas */
    bool     has_pos;
    uint32_t pos_doc;
} status_state;

static status_state *sstate(app *a)
{
    status_state *s = (status_state *)app_ext_get(a, "pnl.status");
    if (s) return s;
    s = (status_state *)calloc(1u, sizeof *s);
    if (s && !app_ext_set(a, "pnl.status", s, free)) {
        free(s);
        s = NULL;
    }
    return s;
}

/* ---- small helpers ------------------------------------------------------------- */
static ui_rect place(app *a, int32_t *x, int32_t y_mid, float w_dip, float h_dip)
{
    int32_t w = ui_px(a->ui, w_dip), h = ui_px(a->ui, h_dip);
    ui_rect r = ui_rect_make(*x, y_mid - h / 2, w, h);
    *x += w;
    ui_layout_set_next(a->ui, r);
    return r;
}

/* Tooltip text of a command: label without "...", plus its key. */
static void cmd_tip(app *a, const char *id, char *out, size_t cap, const char *suffix)
{
    const app_cmd *c = app_cmd_find(a, id);
    const char *sc = app_cmd_shortcut_text(a, id);
    const char *name = c ? c->label : id;
    size_t n = strlen(name);
    if (n > 3u && strcmp(name + n - 3u, "...") == 0) n -= 3u;
    if (sc) snprintf(out, cap, "%.*s (%s)%s", (int)n, name, sc, suffix);
    else snprintf(out, cap, "%.*s%s", (int)n, name, suffix);
}

/* Icon button bound to a command: disabled and checked state mirror it. */
static ui_rect cmd_button(app *a, int32_t *x, int32_t y_mid, const char *id, ui_icon icon)
{
    ui_ctx *ui = a->ui;
    const app_cmd *c = app_cmd_find(a, id);
    bool en = c && app_cmd_enabled(a, id);
    bool chk = c && c->checked && c->checked(a, c);
    char label[200], suffix[96];
    ui_rect r;
    snprintf(suffix, sizeof suffix, "##tb_%s", id);
    cmd_tip(a, id, label, sizeof label, suffix);
    r = place(a, x, y_mid, 32.0f, 32.0f);
    if (ui_button_ex(ui, label, icon,
                     UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (chk ? UI_BUTTON_SELECTED : 0u) |
                         (en ? 0u : UI_DISABLED)))
        (void)app_cmd_exec(a, id);
    return r;
}

static void tb_sep(app *a, int32_t *x, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    int32_t h = bar.h - ui_px(ui, 16.0f);
    *x += ui_px(ui, 5.0f);
    ui_draw_rect(ui, ui_rect_make(*x, bar.y + (bar.h - h) / 2, ui_px_line(ui, 1.0f), h),
                 ui_pal(ui)->separator);
    *x += ui_px(ui, 6.0f);
}

/* A printer glyph (the toolkit has no print icon): paper, body, tray. */
static void print_glyph(app *a, ui_rect r, bool enabled)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_color line = enabled ? p->icon : p->text_disabled;
    ui_color acc = enabled ? p->icon_accent : p->text_disabled;
    int32_t s = ui_px(ui, 16.0f), t = ui_px_line(ui, 1.0f);
    ui_rect g = ui_rect_center(r, s, s);
    ui_rect paper = ui_rect_make(g.x + s * 4 / 16, g.y + s * 1 / 16, s * 8 / 16, s * 6 / 16);
    ui_rect body = ui_rect_make(g.x + s * 1 / 16, g.y + s * 6 / 16, s * 14 / 16, s * 6 / 16);
    ui_rect tray = ui_rect_make(g.x + s * 4 / 16, g.y + s * 10 / 16, s * 8 / 16, s * 5 / 16);
    ui_draw_rect_outline(ui, paper, t, line);
    ui_draw_rect(ui, body, ui_color_fade(acc, 0.35f));
    ui_draw_rect_outline(ui, body, t, line);
    ui_draw_rect(ui, tray, ui_pal(ui)->window);
    ui_draw_rect_outline(ui, tray, t, line);
}

/* ---- top row ------------------------------------------------------------------------ */
static void window_buttons(app *a, ui_rect r, int32_t *x)
{
    ui_ctx *ui = a->ui;
    static const char *const names[4] = { "top.window1", "top.window2", "top.window3",
                                          "top.window4" };
    for (int32_t ord = 1; ord <= 64; ord++) {
        for (int32_t i = 0; i < a->npanels; i++) {
            app_panel *pn = &a->panels[i];
            char id[96], tip[160];
            const char *sc;
            ui_rect br;
            if (pn->def.toggle_order != ord) continue;
            snprintf(id, sizeof id, "window.%s", pn->id);
            sc = app_cmd_shortcut_text(a, id);
            if (sc) snprintf(tip, sizeof tip, "%s (%s)", pn->title, sc);
            else snprintf(tip, sizeof tip, "%s", pn->title);
            br = place(a, x, r.y + r.h / 2, 32.0f, 32.0f);
            *x += ui_px(ui, 2.0f);
            snprintf(id, sizeof id, "##wt_%s", pn->id);
            /* Ctrl+Shift+click resets the window (K-UI-WIN-RESET) */
            if (ui_tool_button(ui, id, pn->def.icon, pn->st.open, tip)) {
                uint32_t m = ui_mods(ui) & (UI_MOD_CTRL | UI_MOD_SHIFT | UI_MOD_ALT | UI_MOD_GUI);
                if (m == (UI_MOD_CTRL | UI_MOD_SHIFT) || m == (UI_MOD_GUI | UI_MOD_SHIFT))
                    app_panel_reset(a, pn->id);
                else app_panel_toggle(a, pn->id);
            }
            if (ord <= 4) pnl_rect_set(a, names[ord - 1], br);
        }
    }
}

static void top_row(app *a, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t menus_end = r.x, mh = ui_px(ui, ui_get_theme(ui)->m.menubar_h);
    int32_t bsz = ui_px(ui, 32.0f), nb = 0, x, right;
    ui_rect bar = ui_rect_make(r.x, r.y + (r.h - mh) / 2, r.w, mh);
    ui_draw_rect(ui, r, p->window);
    app_menubar(a, bar, &menus_end);
    /* right side: window toggles (panels with toggle_order), Settings, Help */
    for (int32_t i = 0; i < a->npanels; i++)
        if (a->panels[i].def.toggle_order > 0) nb++;
    right = r.x + r.w - ui_px(ui, 6.0f) - (nb + 2) * bsz - nb * ui_px(ui, 2.0f) -
            ui_px(ui, 8.0f);
    x = right;
    window_buttons(a, r, &x);
    x += ui_px(ui, 8.0f);
    place(a, &x, r.y + r.h / 2, 32.0f, 32.0f);
    if (ui_icon_button(ui, "##settings", UI_ICON_SETTINGS, "Settings (Alt+X)"))
        (void)app_cmd_exec(a, "app.settings");
    {
        ui_rect hb = place(a, &x, r.y + r.h / 2, 32.0f, 32.0f);
        if (ui_icon_button(ui, "##help", UI_ICON_HELP, "Help"))
            ui_popup_open(ui, "##help_menu", hb, UI_POPUP_BELOW);
        if (ui_popup_begin(ui, "##help_menu")) {
            app_help_menu(a);
            ui_popup_end(ui);
        }
    }
    /* the image list between the menus and the buttons */
    {
        int32_t x0 = menus_end + ui_px(ui, 12.0f), x1 = right - ui_px(ui, 10.0f);
        if (x1 - x0 > ui_px(ui, 60.0f))
            pnl_image_list(a, ui_rect_make(x0, r.y + ui_px(ui, 1.0f), x1 - x0,
                                           r.h - ui_px(ui, 2.0f)));
    }
    ui_draw_rect(ui, ui_rect_make(r.x, r.y + r.h - 1, r.w, 1), p->separator);
}

/* ---- toolbars --------------------------------------------------------------------- */
static void print_button(app *a, int32_t *x, int32_t ym)
{
    ui_ctx *ui = a->ui;
    /* Print: an optional command, shown disabled until a lane registers it */
    bool en = app_cmd_enabled(a, "file.print");
    char label[160];
    ui_rect r;
    if (app_cmd_find(a, "file.print")) cmd_tip(a, "file.print", label, sizeof label, "##tb_print");
    else app_copy_str(label, sizeof label, "Print##tb_print");
    r = place(a, x, ym, 32.0f, 32.0f);
    if (ui_button_ex(ui, label, UI_ICON_NONE,
                     UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (en ? 0u : UI_DISABLED)))
        (void)app_cmd_exec(a, "file.print");
    print_glyph(a, r, en);
    pnl_rect_set(a, "toolbar.print", r);
}

static void toolbar1(app *a, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t x = bar.x + ui_px(ui, 6.0f), ym = bar.y + bar.h / 2;
    ui_draw_rect(ui, bar, p->window);
    cmd_button(a, &x, ym, "file.new", UI_ICON_NEW);
    cmd_button(a, &x, ym, "file.open", UI_ICON_OPEN);
    cmd_button(a, &x, ym, "file.save", UI_ICON_SAVE);
    print_button(a, &x, ym);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "edit.cut", UI_ICON_CUT);
    cmd_button(a, &x, ym, "edit.copy", UI_ICON_COPY);
    cmd_button(a, &x, ym, "edit.paste", UI_ICON_PASTE);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "image.crop_to_selection", UI_ICON_CROP);
    cmd_button(a, &x, ym, "edit.deselect", UI_ICON_DESELECT);
    tb_sep(a, &x, bar);
    pnl_rect_set(a, "toolbar.undo", cmd_button(a, &x, ym, "edit.undo", UI_ICON_UNDO));
    pnl_rect_set(a, "toolbar.redo", cmd_button(a, &x, ym, "edit.redo", UI_ICON_REDO));
    tb_sep(a, &x, bar);
    pnl_rect_set(a, "toolbar.grid", cmd_button(a, &x, ym, "view.pixel_grid", UI_ICON_GRID));
    pnl_rect_set(a, "toolbar.rulers", cmd_button(a, &x, ym, "view.rulers", UI_ICON_RULERS));
    ui_draw_rect(ui, ui_rect_make(bar.x, bar.y + bar.h - 1, bar.w, 1), p->separator);
}

static void toolbar2(app *a, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_draw_rect(ui, bar, p->window);
    ui_push_clip(ui, bar);
    app_options_bar(a, bar);
    ui_pop_clip(ui);
    ui_draw_rect(ui, ui_rect_make(bar.x, bar.y + bar.h - 1, bar.w, 1), p->separator);
}

/* ---- status bar -------------------------------------------------------------------- */
/* Zoom percentage as Paint.NET displays it (OBSERVED 8): integers from 100 %,
 * one decimal (rounded) from 10 %, two decimals (truncated) below. */
void pnl_format_zoom(double pct, char *out, size_t cap)
{
    char tmp[32];
    size_t n;
    if (pct >= 99.995) {
        snprintf(out, cap, "%.0f%%", pct);
        return;
    }
    if (pct >= 10.0) snprintf(tmp, sizeof tmp, "%.1f", pct);
    else snprintf(tmp, sizeof tmp, "%.2f", floor(pct * 100.0 + 1e-6) / 100.0);
    n = strlen(tmp);
    if (strchr(tmp, '.')) {
        while (n > 0u && tmp[n - 1u] == '0') tmp[--n] = '\0';
        if (n > 0u && tmp[n - 1u] == '.') tmp[--n] = '\0';
    }
    snprintf(out, cap, "%s%%", tmp);
}

static float small_font(app *a)
{
    return ui_get_theme(a->ui)->m.font_size_small * ui_scale(a->ui);
}

/* One "icon + text" field placed right to left; returns the next right edge. */
static int32_t field(app *a, int32_t xr, ui_rect bar, ui_icon icon, const char *text,
                     float min_w_dip, const char *rect_name)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t isz = ui_px(ui, 16.0f), x;
    int32_t w =
        (int32_t)ceilf(ui_text_width(ui_font_regular(ui), small_font(a), text, strlen(text)));
    if (w < ui_px(ui, min_w_dip)) w = ui_px(ui, min_w_dip);
    x = xr - w - isz - ui_px(ui, 5.0f);
    ui_draw_icon(ui, icon, ui_rect_make(x, bar.y, isz, bar.h), isz, p->text_dim, p->icon_accent);
    ui_draw_text_box(ui, ui_font_regular(ui), small_font(a),
                     ui_rect_make(x + isz + ui_px(ui, 5.0f), bar.y, w, bar.h), UI_ALIGN_LEFT, 0,
                     p->text, text, strlen(text));
    pnl_rect_set(a, rect_name, ui_rect_make(x, bar.y, xr - x, bar.h));
    return x - ui_px(ui, 16.0f);
}

/* Log-scale zoom slider 1 % .. 10000 % (W-SB-ZOOMSLIDER). */
static void zoom_slider(app *a, app_doc *d, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, "##zoom_slider"), r, 0);
    int32_t th = ui_px(ui, 3.0f), knob = ui_px(ui, 12.0f), x0 = r.x + knob / 2, len = r.w - knob;
    double t = log10(d->view.zoom * 100.0) / 4.0;
    ui_rect track = ui_rect_make(x0, r.y + (r.h - th) / 2, len, th);
    if (in.held && len > 0) {
        double nt = ((double)in.mouse.x - (double)x0) / (double)len, z;
        if (nt < 0.0) nt = 0.0;
        if (nt > 1.0) nt = 1.0;
        z = pow(10.0, nt * 4.0) / 100.0;
        if (fabs(z - d->view.zoom) > 1e-9) app_view_set_zoom(a, d, z);
        t = log10(d->view.zoom * 100.0) / 4.0;
    }
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    ui_draw_rrect(ui, track, (float)th * 0.5f, p->border_strong);
    ui_draw_rrect(ui, ui_rect_make(x0, track.y, (int32_t)((double)len * t), th), (float)th * 0.5f,
                  p->accent);
    {
        ui_vec2 c = ui_vec2_make((float)x0 + (float)((double)len * t),
                                 (float)r.y + (float)r.h * 0.5f);
        ui_draw_circle(ui, c, (float)knob * 0.5f + 1.0f, ui_color_fade(p->shadow, 0.6f));
        ui_draw_circle(ui, c, (float)knob * 0.5f,
                       in.held || in.hovered ? p->accent_hover : p->accent);
        ui_draw_circle(ui, c, (float)knob * 0.22f, p->text_on_accent);
    }
    ui_tooltip(ui, "Zoom");
    pnl_rect_set(a, "status.zoom_slider", r);
}

/* The zoom box: shows the zoom; a click turns it into a text field where a
 * percentage is typed; Enter applies (1..10000 %), Esc or leaving cancels
 * (W-SB-ZOOMBOX, OBSERVED 8: no preset dropdown). */
static void zoom_box(app *a, status_state *s, app_doc *d, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    char txt[32];
    ui_id fid;
    uint32_t res;
    pnl_format_zoom(d->view.zoom * 100.0, txt, sizeof txt);
    app_copy_str(s->fields[PNL_SF_ZOOM], sizeof s->fields[PNL_SF_ZOOM], txt);
    pnl_rect_set(a, "status.zoom_box", r);
    if (!s->zoom_edit) {
        ui_interaction in = ui_interact(ui, ui_get_id(ui, "##zoom_box"), r, 0);
        float rad = ui_get_theme(ui)->m.radius * ui_scale(ui);
        if (in.hovered) ui_draw_rrect(ui, r, rad, p->hover);
        ui_draw_text_box(ui, ui_font_regular(ui), small_font(a), r, UI_ALIGN_CENTER, 0, p->text,
                         txt, strlen(txt));
        ui_tooltip(ui, "Zoom (click to type a percentage)");
        if (in.clicked) {
            size_t n;
            app_copy_str(s->zoom_buf, sizeof s->zoom_buf, txt);
            n = strlen(s->zoom_buf);
            if (n && s->zoom_buf[n - 1u] == '%') s->zoom_buf[n - 1u] = '\0';
            s->zoom_edit = true;
            s->zoom_focus = true;
            app_request_frame(a);
        }
        return;
    }
    fid = ui_get_id(ui, "##zoom_edit");
    if (s->zoom_focus) ui_set_focus(ui, fid);
    ui_layout_set_next(ui, r);
    res = ui_text_field(ui, "##zoom_edit", s->zoom_buf, sizeof s->zoom_buf,
                        UI_EDIT_NUMERIC | UI_EDIT_SELECT_ALL);
    if (res & UI_EDIT_SUBMIT) {
        char *end = NULL;
        double v;
        for (char *q = s->zoom_buf; *q; q++)
            if (*q == ',') *q = '.';
        v = strtod(s->zoom_buf, &end);
        if (end != s->zoom_buf && v == v) {
            if (v < 1.0) v = 1.0;
            if (v > 10000.0) v = 10000.0;
            app_view_set_zoom(a, d, v / 100.0);
        }
        s->zoom_edit = false;
        ui_set_focus(ui, 0);
    } else if ((res & UI_EDIT_CANCEL) ||
               ((res & UI_EDIT_DEACTIVATED) && !ui_is_focused(ui, fid))) {
        /* a DEACTIVATED report while the field holds the focus is the
         * previous edit session ending (the toolkit reports it late) */
        s->zoom_edit = false;
        ui_set_focus(ui, 0);
    } else if (!s->zoom_focus && !ui_is_focused(ui, fid)) {
        s->zoom_edit = false;            /* focus went elsewhere */
    }
    s->zoom_focus = false;
    app_request_frame(a);
}

/* Units: "px", "in" or "cm" with an arrow; the menu holds the three units
 * as radio items (the View menu's state, W-SB-UNITS, OBSERVED 8). */
static void units_menu(app *a, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    static const char *const abbr[3] = { "px", "in", "cm" };
    static const char *const names[3] = { "Pixels", "Inches", "Centimeters" };
    ui_interaction in = ui_interact(ui, ui_get_id(ui, "##units"), r, 0);
    float rad = ui_get_theme(ui)->m.radius * ui_scale(ui);
    int u = (int)app_get_units(a);
    int32_t isz = ui_px(ui, 12.0f);
    if (u < 0 || u > 2) u = 0;
    if (in.hovered || ui_popup_is_open(ui, "##units_menu")) ui_draw_rrect(ui, r, rad, p->hover);
    ui_draw_text_box(ui, ui_font_regular(ui), small_font(a),
                     ui_rect_make(r.x + ui_px(ui, 6.0f), r.y, r.w - isz - ui_px(ui, 10.0f), r.h),
                     UI_ALIGN_LEFT, 0, p->text, abbr[u], 2);
    ui_draw_icon(ui, UI_ICON_CHEVRON_UP,
                 ui_rect_make(r.x + r.w - isz - ui_px(ui, 4.0f), r.y, isz, r.h), isz, p->text_dim,
                 p->icon_accent);
    ui_tooltip(ui, "Units");
    pnl_rect_set(a, "status.units", r);
    if (in.clicked) ui_popup_open(ui, "##units_menu", r, UI_POPUP_ABOVE);
    if (ui_popup_begin(ui, "##units_menu")) {
        for (int i = 0; i < 3; i++) {
            static const char *const rn[3] = { "status.units_px", "status.units_in",
                                               "status.units_cm" };
            if (ui_menu_radio(ui, names[i], NULL, i == u, true)) app_set_units(a, (app_units)i);
            pnl_rect_set(a, rn[i], ui_last_rect(ui));
        }
        ui_popup_end(ui);
    }
}

/* W-SB-PROGRESS: green, filling left to right; busy work sweeps a block. */
static void progress_bar(app *a, ui_rect r, float f)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float rad = (float)r.h * 0.5f;
    ui_draw_rrect(ui, r, rad, p->field);
    ui_draw_rrect_outline(ui, r, rad, ui_px_line(ui, 1.0f), p->border);
    if (f >= 0.0f) {
        int32_t w = (int32_t)((float)r.w * (f > 1.0f ? 1.0f : f));
        if (w > 0) ui_draw_rrect(ui, ui_rect_make(r.x, r.y, w, r.h), rad, p->success);
    } else {
        int32_t bw = r.w / 4, span = r.w - bw, ph = (int32_t)(a->now % 1200u);
        ui_draw_rrect(ui, ui_rect_make(r.x + span * ph / 1200, r.y, bw, r.h), rad, p->success);
        app_request_frame_at(a, a->now + 33u);
    }
}

/* The right part of the status bar; returns its left edge. */
static int32_t status_right(app *a, status_state *s, app_doc *d, ui_rect r, int32_t right)
{
    ui_ctx *ui = a->ui;
    double dpi = d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    char a0[32], a1[32], buf[96];
    int32_t xr = right, bsz = ui_px(ui, 24.0f), ym = r.y + r.h / 2, ch = ui_px(ui, 24.0f);
    ui_rect b;
    /* zoom in, slider, zoom out, quick zoom, zoom box, units */
    b = ui_rect_make(xr - bsz, ym - bsz / 2, bsz, bsz);
    ui_layout_set_next(ui, b);
    if (ui_icon_button(ui, "##zoom_in_btn", UI_ICON_ZOOM_IN, "Zoom In"))
        (void)app_cmd_exec(a, "view.zoom_in");
    pnl_rect_set(a, "status.zoom_in", b);
    xr -= bsz + ui_px(ui, 2.0f);
    zoom_slider(a, d, ui_rect_make(xr - ui_px(ui, 110.0f), ym - ch / 2, ui_px(ui, 110.0f), ch));
    xr -= ui_px(ui, 110.0f) + ui_px(ui, 2.0f);
    b = ui_rect_make(xr - bsz, ym - bsz / 2, bsz, bsz);
    ui_layout_set_next(ui, b);
    if (ui_icon_button(ui, "##zoom_out_btn", UI_ICON_ZOOM_OUT, "Zoom Out"))
        (void)app_cmd_exec(a, "view.zoom_out");
    pnl_rect_set(a, "status.zoom_out", b);
    xr -= bsz + ui_px(ui, 6.0f);
    {
        bool at1 = fabs(d->view.zoom - 1.0) < 1e-9;
        b = ui_rect_make(xr - bsz, ym - bsz / 2, bsz, bsz);
        ui_layout_set_next(ui, b);
        /* W-SB-QUICKZOOM: 100 % <-> the window size */
        if (ui_icon_button(ui, "##quick_zoom", at1 ? UI_ICON_ZOOM_FIT : UI_ICON_ZOOM_ACTUAL,
                           "Toggle between actual size and the window size")) {
            if (at1) app_view_fit_toggle(a, d);
            else app_view_actual(a, d);
        }
        pnl_rect_set(a, "status.quick_zoom", b);
        xr -= bsz + ui_px(ui, 4.0f);
    }
    zoom_box(a, s, d, ui_rect_make(xr - ui_px(ui, 64.0f), ym - ch / 2, ui_px(ui, 64.0f), ch));
    xr -= ui_px(ui, 64.0f) + ui_px(ui, 6.0f);
    units_menu(a, ui_rect_make(xr - ui_px(ui, 44.0f), ym - ch / 2, ui_px(ui, 44.0f), ch));
    xr -= ui_px(ui, 44.0f) + ui_px(ui, 14.0f);
    /* selection size (only with a selection), pointer, image size */
    if (pc_sel_is_active(d->doc)) {
        pc_rect sb = pc_sel_bounds(d->doc);
        app_format_len(a, (double)sb.w, dpi, a0, sizeof a0);
        app_format_len(a, (double)sb.h, dpi, a1, sizeof a1);
        snprintf(buf, sizeof buf, "%s \xC3\x97 %s", a0, a1);
        app_copy_str(s->fields[PNL_SF_SELECTION], sizeof s->fields[0], buf);
        xr = field(a, xr, r, UI_ICON_TOOL_RECT_SELECT, buf, 60.0f, "status.selection");
    }
    {
        double px_, py_;
        if (s->pos_doc != d->id) s->has_pos = false;
        if (app_canvas_pointer_doc(a, &px_, &py_) && a->cv.hovered) {
            s->last_x = px_;
            s->last_y = py_;
            s->has_pos = true;
            s->pos_doc = d->id;
        }
        buf[0] = '\0';
        if (s->has_pos) {
            app_format_len(a, floor(s->last_x), dpi, a0, sizeof a0);
            app_format_len(a, floor(s->last_y), dpi, a1, sizeof a1);
            snprintf(buf, sizeof buf, "%s, %s", a0, a1);
        }
        app_copy_str(s->fields[PNL_SF_CURSOR], sizeof s->fields[0], buf);
        xr = field(a, xr, r, UI_ICON_TOOL_MOVE_SELECTION, buf, 60.0f, "status.cursor");
    }
    app_format_len(a, (double)d->doc->w, dpi, a0, sizeof a0);
    app_format_len(a, (double)d->doc->h, dpi, a1, sizeof a1);
    snprintf(buf, sizeof buf, "%s \xC3\x97 %s", a0, a1);
    app_copy_str(s->fields[PNL_SF_SIZE], sizeof s->fields[0], buf);
    return field(a, xr, r, UI_ICON_IMAGE, buf, 70.0f, "status.size");
}

static void status_bar(app *a, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    status_state *s = sstate(a);
    app_doc *d = app_active_doc(a);
    const app_tool *t = app_tool_current(a);
    int32_t x, right, ym = r.y + r.h / 2;
    ui_draw_rect(ui, r, p->window);
    ui_draw_rect(ui, ui_rect_make(r.x, r.y, r.w, 1), p->separator);
    right = r.x + r.w - ui_px(ui, 8.0f);
    if (s) memset(s->fields, 0, sizeof s->fields);
    if (d && s) right = status_right(a, s, d, r, right);
    else if (s) s->zoom_edit = false;
    /* left: tool help or status text (W-SB-HELP), then the progress bar */
    x = r.x + ui_px(ui, 10.0f);
    {
        const char *msg = a->status_set ? a->status : (t && t->help ? t->help : "");
        bool busy = app_tasks_pending(a) > 0;
        int32_t pw = ui_px(ui, 160.0f), avail = right - x;
        bool show_prog = a->progress <= 1.0f || busy;
        int32_t tw = avail - (show_prog ? pw + ui_px(ui, 12.0f) : 0);
        if (t) {
            int32_t isz = ui_px(ui, 16.0f);
            ui_draw_icon(ui, t->icon, ui_rect_make(x, r.y, isz, r.h), isz, p->icon,
                         p->icon_accent);
            x += isz + ui_px(ui, 6.0f);
            tw -= isz + ui_px(ui, 6.0f);
        }
        if (tw > 0)
            ui_draw_text_box(ui, ui_font_regular(ui), small_font(a), ui_rect_make(x, r.y, tw, r.h),
                             UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text_dim, msg, strlen(msg));
        pnl_rect_set(a, "status.help", ui_rect_make(x, r.y, tw > 0 ? tw : 0, r.h));
        if (show_prog) {
            ui_rect pr = ui_rect_make(right - pw, ym - ui_px(ui, 5.0f), pw, ui_px(ui, 10.0f));
            progress_bar(a, pr, a->progress <= 1.0f ? a->progress : -1.0f);
            pnl_rect_set(a, "status.progress", pr);
        }
    }
}

const char *pnl_status_field(const app *a, int which)
{
    status_state *s = (status_state *)app_ext_get(a, "pnl.status");
    if (!s || which < 0 || which >= PNL_SF_COUNT) return "";
    return s->fields[which];
}

/* ---- window title -------------------------------------------------------------------- */
static void update_title(app *a)
{
    char title[512];
    app_doc *d = app_active_doc(a);
    if (!a->win) return;
    if (d)
        snprintf(title, sizeof title, "%s%s - %s", app_doc_dirty(d) ? "*" : "", d->name, APP_NAME);
    else snprintf(title, sizeof title, "%s", APP_NAME);
    if (strcmp(title, a->title) != 0) {
        app_copy_str(a->title, sizeof a->title, title);
        SDL_SetWindowTitle(a->win, title);
    }
}

/* ---- the frame ----------------------------------------------------------------------- */
void app_shell_frame(app *a)
{
    ui_ctx *ui = a->ui;
    ui_rect full = ui_layout_content(ui), r = full;
    a->r_top = ui_cut_top(&r, ui_px(ui, TOP_DIP));
    a->r_tb1 = ui_cut_top(&r, ui_px(ui, TOOLBAR_DIP));
    a->r_tb2 = ui_cut_top(&r, ui_px(ui, TOOLBAR_DIP));
    a->r_status = ui_cut_bottom(&r, ui_px(ui, STATUS_DIP));
    a->r_work = r;
    ui_draw_rect(ui, full, ui_pal(ui)->window);
    app_canvas_frame(a, a->r_work);
    top_row(a, a->r_top);
    toolbar1(a, a->r_tb1);
    toolbar2(a, a->r_tb2);
    status_bar(a, a->r_status);
    ui_panels_area(ui, a->r_work);
    app_panels_frame(a);
    app_dialogs_frame(a);
}

void app_shell_after_frame(app *a)
{
    update_title(a);
}
