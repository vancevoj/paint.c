/* mod_image_size.c - Image > Resize and Image > Canvas Size (MENUS.md),
 * provisional (APP_CMD_WEAK) versions of the dialogs: by percentage or
 * absolute size, aspect ratio lock, resolution and print size, the
 * resampling choice with gamma correction (Resize), the 3 x 3 anchor grid
 * and the fill choice (Canvas Size). Each runs one history step
 * (pc_geom_resize, pc_geom_canvas_size) and stores the resolution. The
 * dialogs lane may supersede them by registering the same command ids. */
#include "../app_internal.h"
#include "pc/pc_geom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MENUS.md order of the resampling list. */
static const char *const k_resample_names[] = {
    "Bicubic", "Bicubic (Smooth)", "Bilinear", "Bilinear (Low Quality)",
    "Adaptive (Sharp)", "Lanczos", "Fant", "Nearest Neighbor"
};
static const pc_resample k_resample_modes[] = {
    PC_RESAMPLE_BICUBIC, PC_RESAMPLE_BICUBIC_SMOOTH, PC_RESAMPLE_BILINEAR,
    PC_RESAMPLE_BILINEAR_LOW, PC_RESAMPLE_ADAPTIVE_SHARP, PC_RESAMPLE_LANCZOS3,
    PC_RESAMPLE_FANT, PC_RESAMPLE_NEAREST
};
static const char *const k_fill_names[] = { "Transparent", "Primary Color", "Secondary Color",
                                            "White", "Black" };

typedef struct size_dlg {
    bool     canvas;             /* Canvas Size (true) or Resize */
    uint32_t doc_id;
    uint32_t ow, oh;             /* current size */
    int      by;                 /* 0 by percentage, 1 by absolute size */
    double   pct;
    bool     keep;
    int32_t  w, h;
    double   res;                /* pixels per inch or per centimeter */
    int      unit;               /* 0 inches, 1 centimeters */
    double   pw, ph;             /* print size */
    int      resample;
    bool     gamma;
    int      anchor;             /* pc_anchor */
    int      fill;
    char     title[64];
} size_dlg;

/* Session memory (B: last used values). */
static int g_resample;
static bool g_gamma = true, g_keep = true;

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static int32_t clamp_dim(double v)
{
    if (!(v >= 1.0)) return 1;
    if (v > (double)PC_MAX_DIM) return (int32_t)PC_MAX_DIM;
    return (int32_t)(v + 0.5);
}

static void sync_print(size_dlg *s)
{
    s->pw = (double)s->w / s->res;
    s->ph = (double)s->h / s->res;
}

static void set_w(size_dlg *s, int32_t w)
{
    s->w = w;
    if (s->keep) s->h = clamp_dim((double)w * (double)s->oh / (double)s->ow);
    sync_print(s);
}

static void set_h(size_dlg *s, int32_t h)
{
    s->h = h;
    if (s->keep) s->w = clamp_dim((double)h * (double)s->ow / (double)s->oh);
    sync_print(s);
}

static void anchor_grid(app *a, size_dlg *s)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t cell = ui_px(ui, 30.0f);
    ui_rect r = ui_layout_next(ui, 3 * cell, 3 * cell);
    int ax = s->anchor % 3, ay = s->anchor / 3;
    for (int i = 0; i < 9; i++) {
        int cx = i % 3, cy = i / 3, dx = cx - ax, dy = cy - ay;
        ui_rect c = ui_rect_make(r.x + cx * cell, r.y + cy * cell, cell, cell);
        ui_icon icon = UI_ICON_NONE;
        char id[16];
        if (dx == 0 && dy == 0) icon = UI_ICON_DOT;
        else if (dx == 0 && dy == -1) icon = UI_ICON_CHEVRON_UP;
        else if (dx == 0 && dy == 1) icon = UI_ICON_CHEVRON_DOWN;
        else if (dx == -1 && dy == 0) icon = UI_ICON_CHEVRON_LEFT;
        else if (dx == 1 && dy == 0) icon = UI_ICON_CHEVRON_RIGHT;
        snprintf(id, sizeof id, "##anchor%d", i);
        ui_layout_set_next(ui, ui_rect_inset(c, 1, 1));
        if (ui_tool_button(ui, id, icon, i == s->anchor, NULL))
            s->anchor = i;
        ui_draw_rect_outline(ui, ui_rect_inset(c, 1, 1), 1, p->border);
    }
}

static bool size_frame(app *a, void *st)
{
    size_dlg *s = (size_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = doc_by_id(a, s->doc_id);
    static const char *const units[] = { "Pixels/inch", "Pixels/centimeter" };
    static const char *const print_units[] = { "Inches", "Centimeters" };
    ui_size cells[3];
    char est[96];
    uint32_t r;
    bool enter;
    int by = s->by;
    if (!d) return false;
    ui_dialog_begin(ui, s->title, 440.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    {
        double mb = (double)s->w * (double)s->h * 4.0 * (double)d->doc->n_layers / 1048576.0;
        snprintf(est, sizeof est, "New size: %.1f MB (now %.1f MB)", mb,
                 (double)s->ow * (double)s->oh * 4.0 * (double)d->doc->n_layers / 1048576.0);
        ui_label_ex(ui, est, UI_LABEL_DIM);
    }
    cells[0] = ui_size_px(150.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    (void)ui_radio(ui, "By percentage:##bypct", &by, 0);
    if (ui_number_double(ui, "##pct", &s->pct, 0.01, 10000.0, 1.0, 2, UI_SLIDER_PERCENT) &&
        by == 0) {
        s->w = clamp_dim((double)s->ow * s->pct / 100.0);
        s->h = clamp_dim((double)s->oh * s->pct / 100.0);
        sync_print(s);
    }
    ui_layout_column(ui);
    (void)ui_radio(ui, "By absolute size:##byabs", &by, 1);
    if (by == 0 && s->by != 0) {
        s->w = clamp_dim((double)s->ow * s->pct / 100.0);
        s->h = clamp_dim((double)s->oh * s->pct / 100.0);
        sync_print(s);
    }
    s->by = by;
    if (ui_checkbox(ui, "Maintain aspect ratio##keep", &s->keep) && s->keep) set_w(s, s->w);
    cells[0] = ui_size_px(110.0f);
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_px(110.0f);
    ui_heading(ui, "Pixel size");
    ui_layout_row(ui, 0.0f, 3, cells);
    {
        uint32_t dis = s->by == 0 ? UI_DISABLED : 0u;
        int32_t w = s->w, h = s->h;
        ui_label_ex(ui, "Width", UI_LABEL_DIM | dis);
        if (ui_number_int(ui, "##w", &w, 1, (int32_t)PC_MAX_DIM, 1, dis) && !dis) set_w(s, w);
        ui_label_ex(ui, "pixels", UI_LABEL_DIM | dis);
        ui_label_ex(ui, "Height", UI_LABEL_DIM | dis);
        if (ui_number_int(ui, "##h", &h, 1, (int32_t)PC_MAX_DIM, 1, dis) && !dis) set_h(s, h);
        ui_label_ex(ui, "pixels", UI_LABEL_DIM | dis);
        ui_label_ex(ui, "Resolution", UI_LABEL_DIM);
        if (ui_number_double(ui, "##res", &s->res, 0.01, 100000.0, 1.0, 2, 0)) sync_print(s);
        {
            int u = s->unit;
            if (ui_combo(ui, "##resu", &u, units, 2) && u != s->unit) {
                s->res = u == 1 ? s->res / 2.54 : s->res * 2.54;
                s->unit = u;
                sync_print(s);
            }
        }
        ui_layout_column(ui);
        ui_heading(ui, "Print size");
        ui_layout_row(ui, 0.0f, 3, cells);
        ui_label_ex(ui, "Width", UI_LABEL_DIM | dis);
        if (ui_number_double(ui, "##pw", &s->pw, 0.001, 100000.0, 0.1, 2, dis) && !dis)
            set_w(s, clamp_dim(s->pw * s->res));
        ui_label_ex(ui, print_units[s->unit], UI_LABEL_DIM | dis);
        ui_label_ex(ui, "Height", UI_LABEL_DIM | dis);
        if (ui_number_double(ui, "##ph", &s->ph, 0.001, 100000.0, 0.1, 2, dis) && !dis)
            set_h(s, clamp_dim(s->ph * s->res));
        ui_label_ex(ui, print_units[s->unit], UI_LABEL_DIM | dis);
        ui_layout_column(ui);
    }
    if (s->canvas) {
        cells[0] = ui_size_px(110.0f);
        cells[1] = ui_size_fr(1.0f);
        ui_heading(ui, "Anchor");
        ui_layout_row(ui, 0.0f, 2, cells);
        ui_layout_begin(ui, 0.0f);
        anchor_grid(a, s);
        ui_layout_end(ui);
        ui_layout_begin(ui, 0.0f);
        ui_label_ex(ui, "Fill new area with", UI_LABEL_DIM);
        ui_combo(ui, "##fill", &s->fill, k_fill_names, 5);
        ui_layout_end(ui);
        ui_layout_column(ui);
    } else {
        ui_heading(ui, "Options");
        cells[0] = ui_size_px(110.0f);
        cells[1] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 2, cells);
        ui_label_ex(ui, "Resampling", UI_LABEL_DIM);
        ui_combo(ui, "##resample", &s->resample, k_resample_names, 8);
        ui_layout_column(ui);
        ui_checkbox(ui, "Use gamma correction##gamma", &s->gamma);
    }
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    g_keep = s->keep;
    if (r == UI_DLG_OK && ((uint32_t)s->w != s->ow || (uint32_t)s->h != s->oh)) {
        pc_status st2;
        (void)app_tool_finish(a);
        if (s->canvas) {
            pc_px32 fill;
            memset(&fill, 0, sizeof fill);
            if (s->fill == 1) fill = app_primary(a);
            else if (s->fill == 2) fill = app_secondary(a);
            else if (s->fill == 3) fill = app_px_make(255, 255, 255, 255);
            else if (s->fill == 4) fill = app_px_make(0, 0, 0, 255);
            st2 = pc_geom_canvas_size(d->hist, (uint32_t)s->w, (uint32_t)s->h,
                                      (pc_anchor)s->anchor, fill, &a->par, "Canvas Size");
        } else {
            g_resample = s->resample;
            g_gamma = s->gamma;
            st2 = pc_geom_resize(d->hist, (uint32_t)s->w, (uint32_t)s->h,
                                 k_resample_modes[s->resample], s->gamma ? PC_RESAMPLE_GAMMA : 0u,
                                 &a->par, "Resize");
        }
        if (st2 != PC_OK) {
            app_error(a, "%s failed: %s.", s->canvas ? "Canvas Size" : "Resize",
                      pc_status_str(st2));
        } else {
            double dpi = s->unit == 1 ? s->res * 2.54 : s->res;
            d->meta.dpi_x = d->meta.dpi_y = dpi;
            app_doc_history_changed(a, d);
            d->view.need_fit = true;
        }
    } else if (r == UI_DLG_OK) {
        double dpi = s->unit == 1 ? s->res * 2.54 : s->res;
        d->meta.dpi_x = d->meta.dpi_y = dpi;
    }
    return false;
}

static void open_dialog(app *a, bool canvas)
{
    app_doc *d = app_active_doc(a);
    size_dlg *s = (size_dlg *)calloc(1u, sizeof *s);
    if (!s || !d) {
        free(s);
        return;
    }
    s->canvas = canvas;
    s->doc_id = d->id;
    s->ow = d->doc->w;
    s->oh = d->doc->h;
    s->w = (int32_t)d->doc->w;
    s->h = (int32_t)d->doc->h;
    s->by = canvas ? 1 : 0;
    s->pct = 100.0;
    s->keep = canvas ? false : g_keep;
    s->res = d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    s->unit = app_get_units(a) == APP_UNITS_CM ? 1 : 0;
    if (s->unit == 1) s->res /= 2.54;
    s->resample = g_resample;
    s->gamma = g_gamma;
    s->anchor = PC_ANCHOR_CENTER;
    s->fill = 0;
    snprintf(s->title, sizeof s->title, "%s##sizedlg", canvas ? "Canvas Size" : "Resize Image");
    sync_print(s);
    (void)app_dialog_push(a, size_frame, s, free);
}

static void cmd_resize(app *a, const app_cmd *c)
{
    (void)c;
    open_dialog(a, false);
}

static void cmd_canvas(app *a, const app_cmd *c)
{
    (void)c;
    open_dialog(a, true);
}

void mod_image_size(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "image.resize";
    d.label = "Resize...";
    d.icon = UI_ICON_RESIZE;
    d.flags = APP_CMD_NEEDS_DOC | APP_CMD_WEAK;
    d.run = cmd_resize;
    (void)app_cmd_register(a, &d);
    d.id = "image.canvas_size";
    d.label = "Canvas Size...";
    d.icon = UI_ICON_CANVAS_SIZE;
    d.run = cmd_canvas;
    (void)app_cmd_register(a, &d);
}
