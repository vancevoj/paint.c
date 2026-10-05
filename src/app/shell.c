/* shell.c - the main window layout (docs/inventory/WINDOWS.md section 1):
 *   top row    menus, the image list (thumbnail tabs), window toggles,
 *              Settings and Help buttons
 *   toolbar 1  common commands, Pixel Grid and Rulers toggles
 *   toolbar 2  tool chooser and the active tool's options
 *   workspace  the canvas with floating panels
 *   status bar tool help, progress, image size, pointer, selection,
 *              units, zoom box, quick zoom button and zoom slider */
#include "app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_DIP     42.0f
#define TOOLBAR_DIP 38.0f
#define STATUS_DIP  30.0f

/* ---- small helpers ------------------------------------------------------------- */
static ui_rect place(app *a, int32_t *x, int32_t y_mid, float w_dip, float h_dip)
{
    int32_t w = ui_px(a->ui, w_dip), h = ui_px(a->ui, h_dip);
    ui_rect r = ui_rect_make(*x, y_mid - h / 2, w, h);
    *x += w;
    ui_layout_set_next(a->ui, r);
    return r;
}

/* Icon button bound to a command: disabled and checked state mirror it. */
static void cmd_button(app *a, int32_t *x, int32_t y_mid, const char *id, ui_icon icon)
{
    ui_ctx *ui = a->ui;
    const app_cmd *c = app_cmd_find(a, id);
    bool en = c && app_cmd_enabled(a, id);
    bool chk = c && c->checked && c->checked(a, c);
    char label[160];
    const char *sc = app_cmd_shortcut_text(a, id);
    const char *name = c ? c->label : id;
    size_t n = strlen(name);
    /* tooltips drop the trailing "..." of menu labels */
    if (n > 3u && strcmp(name + n - 3u, "...") == 0) n -= 3u;
    if (sc) snprintf(label, sizeof label, "%.*s (%s)##tb_%s", (int)n, name, sc, id);
    else snprintf(label, sizeof label, "%.*s##tb_%s", (int)n, name, id);
    place(a, x, y_mid, 32.0f, 32.0f);
    if (ui_button_ex(ui, label, icon,
                     UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (chk ? UI_BUTTON_SELECTED : 0u) |
                         (en ? 0u : UI_DISABLED)))
        (void)app_cmd_exec(a, id);
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

/* ---- top row ------------------------------------------------------------------------ */
static void doc_context_menu(app *a, int32_t idx)
{
    ui_ctx *ui = a->ui;
    app_doc *d = app_doc_at(a, idx);
    bool has_path = d && d->path;
    if (!d) return;
    if (ui_menu_item(ui, "Copy Path", NULL, has_path)) (void)pal_clip_set_text(d->path);
    if (ui_menu_item(ui, "Open Containing Folder", NULL, has_path))
        (void)pal_reveal_file(d->path);
    ui_menu_separator(ui);
    if (ui_menu_item(ui, "Save", NULL, true)) app_save_doc(a, d, false, NULL, NULL);
    if (ui_menu_item(ui, "Save As...", NULL, true)) app_save_doc(a, d, true, NULL, NULL);
    ui_menu_separator(ui);
    if (ui_menu_item(ui, "Close", NULL, true)) app_close_doc(a, d, NULL, NULL);
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
    for (int32_t ord = 1; ord <= 64; ord++) {
        for (int32_t i = 0; i < a->npanels; i++) {
            app_panel *pn = &a->panels[i];
            char id[96], tip[160];
            const char *sc;
            if (pn->def.toggle_order != ord) continue;
            snprintf(id, sizeof id, "window.%s", pn->id);
            sc = app_cmd_shortcut_text(a, id);
            if (sc) snprintf(tip, sizeof tip, "%s (%s)", pn->title, sc);
            else snprintf(tip, sizeof tip, "%s", pn->title);
            place(a, &x, r.y + r.h / 2, 32.0f, 32.0f);
            x += ui_px(ui, 2.0f);
            snprintf(id, sizeof id, "##wt_%s", pn->id);
            if (ui_tool_button(ui, id, pn->def.icon, pn->st.open, tip)) {
                if (ui_mods(ui) == (UI_MOD_CTRL | UI_MOD_SHIFT)) app_panel_reset(a, pn->id);
                else app_panel_toggle(a, pn->id);
            }
        }
    }
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
        ui_doc_tab tabs[64];
        int32_t n = a->ndocs < 64 ? a->ndocs : 64, active = a->active;
        if (x1 - x0 > ui_px(ui, 60.0f) && n > 0) {
            ui_doc_tabs_result res;
            for (int32_t i = 0; i < n; i++) {
                app_doc *d = a->docs[i];
                tabs[i].title = d->name;
                tabs[i].thumb = d->thumb;
                tabs[i].thumb_w = d->thumb_w > 0 ? d->thumb_w : (int32_t)d->doc->w;
                tabs[i].thumb_h = d->thumb_h > 0 ? d->thumb_h : (int32_t)d->doc->h;
                tabs[i].modified = app_doc_dirty(d);
            }
            res = ui_doc_tabs(ui, "##images", ui_rect_make(x0, r.y + ui_px(ui, 1.0f), x1 - x0,
                                                           r.h - ui_px(ui, 2.0f)),
                              tabs, n, &active, UI_DOCTABS_THUMBS_ONLY);
            if (res.switched && active >= 0 && active < a->ndocs)
                app_set_active_doc(a, a->docs[active]);
            if (res.reordered && res.move_from >= 0 && res.move_from < a->ndocs &&
                res.move_to >= 0 && res.move_to < a->ndocs) {
                app_doc *cur = app_active_doc(a), *m = a->docs[res.move_from];
                if (res.move_from < res.move_to)
                    memmove(&a->docs[res.move_from], &a->docs[res.move_from + 1],
                            (size_t)(res.move_to - res.move_from) * sizeof *a->docs);
                else
                    memmove(&a->docs[res.move_to + 1], &a->docs[res.move_to],
                            (size_t)(res.move_from - res.move_to) * sizeof *a->docs);
                a->docs[res.move_to] = m;
                a->active = app_doc_index(a, cur);
            }
            if (res.context_index >= 0) {
                a->ctx_doc = res.context_index;
                ui_popup_open(ui, "##doc_ctx", ui_rect_make(0, 0, 0, 0), UI_POPUP_AT);
            }
            if (ui_popup_begin(ui, "##doc_ctx")) {
                doc_context_menu(a, a->ctx_doc);
                ui_popup_end(ui);
            }
            if (res.close_index >= 0 && res.close_index < a->ndocs)
                app_close_doc(a, a->docs[res.close_index], NULL, NULL);
        }
    }
    ui_draw_rect(ui, ui_rect_make(r.x, r.y + r.h - 1, r.w, 1), p->separator);
}

/* ---- toolbars --------------------------------------------------------------------- */
static void toolbar1(app *a, ui_rect bar)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t x = bar.x + ui_px(ui, 6.0f), ym = bar.y + bar.h / 2;
    ui_draw_rect(ui, bar, p->window);
    cmd_button(a, &x, ym, "file.new", UI_ICON_NEW);
    cmd_button(a, &x, ym, "file.open", UI_ICON_OPEN);
    cmd_button(a, &x, ym, "file.save", UI_ICON_SAVE);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "edit.cut", UI_ICON_CUT);
    cmd_button(a, &x, ym, "edit.copy", UI_ICON_COPY);
    cmd_button(a, &x, ym, "edit.paste", UI_ICON_PASTE);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "image.crop_to_selection", UI_ICON_CROP);
    cmd_button(a, &x, ym, "edit.deselect", UI_ICON_DESELECT);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "edit.undo", UI_ICON_UNDO);
    cmd_button(a, &x, ym, "edit.redo", UI_ICON_REDO);
    tb_sep(a, &x, bar);
    cmd_button(a, &x, ym, "view.pixel_grid", UI_ICON_GRID);
    cmd_button(a, &x, ym, "view.rulers", UI_ICON_RULERS);
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
static int32_t status_text(app *a, int32_t x, int32_t y, int32_t h, ui_icon icon,
                           const char *text, float min_w_dip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui);
    int32_t isz = ui_px(ui, 16.0f), w;
    float tw = ui_text_width(ui_font_regular(ui), fs, text, strlen(text));
    w = (int32_t)ceilf(tw);
    if (w < ui_px(ui, min_w_dip)) w = ui_px(ui, min_w_dip);
    if (icon) {
        ui_draw_icon(ui, icon, ui_rect_make(x, y, isz, h), isz, p->text_dim, p->icon_accent);
        x += isz + ui_px(ui, 5.0f);
    }
    ui_draw_text_box(ui, ui_font_regular(ui), fs, ui_rect_make(x, y, w, h), UI_ALIGN_LEFT, 0,
                     p->text_dim, text, strlen(text));
    return x + w + ui_px(ui, 14.0f);
}

static int32_t status_width(app *a, ui_icon icon, const char *text, float min_w_dip)
{
    ui_ctx *ui = a->ui;
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui);
    int32_t w = (int32_t)ceilf(ui_text_width(ui_font_regular(ui), fs, text, strlen(text)));
    if (w < ui_px(ui, min_w_dip)) w = ui_px(ui, min_w_dip);
    return w + (icon ? ui_px(ui, 21.0f) : 0) + ui_px(ui, 14.0f);
}

static void status_bar(app *a, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    app_doc *d = app_active_doc(a);
    const app_tool *t = app_tool_current(a);
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui);
    char size_s[96], pos_s[96], sel_s[96], a0[32], a1[32];
    int32_t x, right, ym = r.y + r.h / 2, ch = ui_px(ui, 24.0f);
    ui_draw_rect(ui, r, p->window);
    ui_draw_rect(ui, ui_rect_make(r.x, r.y, r.w, 1), p->separator);
    /* right part, laid out from the right edge */
    right = r.x + r.w - ui_px(ui, 8.0f);
    if (d) {
        static const char *const units[] = { "Pixels", "Inches", "Centimeters" };
        double zoom = d->view.zoom * 100.0;
        double dpi = d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
        int u = (int)a->units;
        int32_t xr = right;
        /* zoom slider (log mapping), quick size button, zoom box */
        {
            double lz = log10(zoom);
            int32_t sw = ui_px(ui, 110.0f);
            xr -= sw;
            ui_layout_set_next(ui, ui_rect_make(xr, ym - ch / 2, sw, ch));
            if (ui_slider_double(ui, "##zoom_slider", &lz, 0.0, 4.0, 0.0, UI_SLIDER_NO_RESET))
                app_view_set_zoom(a, d, pow(10.0, lz) / 100.0);
            xr -= ui_px(ui, 6.0f);
        }
        {
            int32_t bw = ui_px(ui, 28.0f);
            xr -= bw;
            ui_layout_set_next(ui, ui_rect_make(xr, ym - bw / 2, bw, bw));
            bool at1 = fabs(d->view.zoom - 1.0) < 1e-9;
            if (ui_icon_button(ui, "##quick_zoom", at1 ? UI_ICON_ZOOM_FIT : UI_ICON_ZOOM_ACTUAL,
                               "Toggle between actual size and the window size")) {
                if (fabs(d->view.zoom - 1.0) < 1e-9) app_view_fit_toggle(a, d);
                else app_view_actual(a, d);
            }
        }
        {
            int32_t zw = ui_px(ui, 92.0f);
            xr -= zw + ui_px(ui, 4.0f);
            ui_layout_set_next(ui, ui_rect_make(xr, ym - ch / 2, zw, ch));
            if (ui_number_double(ui, "##zoom_box", &zoom, 1.0, 10000.0, 10.0, 0, UI_SLIDER_PERCENT))
                app_view_set_zoom(a, d, zoom / 100.0);
            ui_tooltip(ui, "Zoom");
        }
        {
            int32_t uw = ui_px(ui, 112.0f);
            xr -= uw + ui_px(ui, 10.0f);
            ui_layout_set_next(ui, ui_rect_make(xr, ym - ch / 2, uw, ch));
            if (ui_combo(ui, "##units", &u, units, 3)) app_set_units(a, (app_units)u);
        }
        right = xr - ui_px(ui, 12.0f);
        /* image size, pointer and selection */
        app_format_len(a, (double)d->doc->w, dpi, a0, sizeof a0);
        app_format_len(a, (double)d->doc->h, dpi, a1, sizeof a1);
        snprintf(size_s, sizeof size_s, "%s \xC3\x97 %s", a0, a1);
        pos_s[0] = '\0';
        {
            double px_, py_;
            if (app_canvas_pointer_doc(a, &px_, &py_) && a->cv.hovered) {
                app_format_len(a, floor(px_), dpi, a0, sizeof a0);
                app_format_len(a, floor(py_), dpi, a1, sizeof a1);
                snprintf(pos_s, sizeof pos_s, "%s, %s", a0, a1);
            }
        }
        if (pc_sel_is_active(d->doc)) {
            pc_rect b = pc_sel_bounds(d->doc);
            app_format_len(a, (double)b.w, dpi, a0, sizeof a0);
            app_format_len(a, (double)b.h, dpi, a1, sizeof a1);
            snprintf(sel_s, sizeof sel_s, "%s \xC3\x97 %s", a0, a1);
        } else {
            snprintf(sel_s, sizeof sel_s, "0 \xC3\x97 0");
        }
        {
            int32_t w3 = status_width(a, UI_ICON_TOOL_RECT_SELECT, sel_s, 70.0f);
            int32_t w2 = status_width(a, UI_ICON_TOOL_MOVE_SELECTION, pos_s, 70.0f);
            int32_t w1 = status_width(a, UI_ICON_IMAGE, size_s, 80.0f);
            int32_t xs = right - (w1 + w2 + w3);
            xs = status_text(a, xs, r.y, r.h, UI_ICON_IMAGE, size_s, 80.0f);
            xs = status_text(a, xs, r.y, r.h, UI_ICON_TOOL_MOVE_SELECTION, pos_s, 70.0f);
            (void)status_text(a, xs, r.y, r.h, UI_ICON_TOOL_RECT_SELECT, sel_s, 70.0f);
            right -= w1 + w2 + w3;
        }
    }
    /* left: tool help or status, then the progress bar */
    x = r.x + ui_px(ui, 10.0f);
    {
        const char *msg = a->status_set ? a->status : (t && t->help ? t->help : "");
        bool busy = app_tasks_pending(a) > 0;
        int32_t pw = ui_px(ui, 160.0f), avail = right - x;
        bool show_prog = a->progress <= 1.0f || busy;
        int32_t tw = avail - (show_prog ? pw + ui_px(ui, 12.0f) : 0);
        if (t) {
            int32_t isz = ui_px(ui, 16.0f);
            ui_draw_icon(ui, t->icon, ui_rect_make(x, r.y, isz, r.h), isz, p->icon, p->icon_accent);
            x += isz + ui_px(ui, 6.0f);
            tw -= isz + ui_px(ui, 6.0f);
        }
        if (tw > 0)
            ui_draw_text_box(ui, ui_font_regular(ui), fs, ui_rect_make(x, r.y, tw, r.h),
                             UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text_dim, msg, strlen(msg));
        if (show_prog) {
            float f = a->progress <= 1.0f ? a->progress : -1.0f;
            ui_layout_set_next(ui, ui_rect_make(right - pw, ym - ui_px(ui, 4.0f), pw,
                                                ui_px(ui, 8.0f)));
            ui_progress(ui, f);
            if (f < 0.0f) app_request_frame_at(a, a->now + 33u);
        }
    }
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
