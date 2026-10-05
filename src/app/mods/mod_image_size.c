/* mod_image_size.c - Image > Resize (Ctrl+R) and Image > Canvas Size
 * (Ctrl+Shift+R), lane M (MENUS.md Image, OBSERVED.md 3.1 and 3.2).
 *
 * Both dialogs share the size model of edit/m_size.h: a memory estimate
 * of the new size ("New size: 1.8 MB", binary units), By percentage (two
 * decimals, clamps at 2000) or By absolute size (the default), Maintain
 * aspect ratio (Resize: on, Canvas Size: off; disabled while By
 * percentage), the pixel size, the resolution in pixels per inch or per
 * centimeter and the print size in inches or centimeters. OK is disabled
 * while the result is not a valid image size (zero, or above paint.c's
 * 65535 per side, ADR-014).
 *   Resize adds the resampling list in MENUS.md order with a reset button
 *   (default Bicubic) and Use gamma correction (default on).
 *   Canvas Size adds the anchor (dropdown plus 3 x 3 grid, default Top
 *   Left as observed) and the fill of the new area (Transparent, Primary
 *   Color, Secondary Color, White, Black; default Transparent; it fills
 *   the bottom layer, the others get transparent, pc_geom_canvas_size).
 * OK applies one history step ("Resize" / "Canvas Size") that also holds a
 * changed resolution (m_doc_set_dpi, fused). The resampling, gamma and
 * aspect choices of Resize and the anchor and fill of Canvas Size are
 * remembered for the session.
 *
 * Thread rules: main thread. Ownership: the dialog state is owned by the
 * dialog stack; the session memory by the app (app_ext). */
#include "../app_internal.h"
#include "../edit/m_hist.h"
#include "../edit/m_size.h"
#include "../edit/m_ui.h"
#include "pc/pc_geom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MENUS.md / OBSERVED.md order of the resampling list. */
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
static const char *const k_anchor_names[] = { "Top Left", "Top", "Top Right", "Left", "Middle",
                                              "Right", "Bottom Left", "Bottom", "Bottom Right" };
static const char *const k_res_units[] = { "Pixels/inch", "Pixels/centimeter" };
static const char *const k_print_units[] = { "Inches", "Centimeters" };

#define MEM_KEY "lane_m.size_memory"

typedef struct size_memory {
    int  resample;           /* index into k_resample_names */
    bool gamma, keep;        /* Resize */
    int  anchor, fill;       /* Canvas Size */
} size_memory;

static size_memory *memory(app *a)
{
    size_memory *m = (size_memory *)app_ext_get(a, MEM_KEY);
    if (m) return m;
    m = (size_memory *)calloc(1u, sizeof *m);
    if (!m) return NULL;
    m->resample = 0;
    m->gamma = true;
    m->keep = true;
    m->anchor = PC_ANCHOR_TOP_LEFT;
    m->fill = 0;
    if (!app_ext_set(a, MEM_KEY, m, free)) {
        free(m);
        return NULL;
    }
    return m;
}

typedef struct size_dlg {
    bool     canvas;             /* Canvas Size (true) or Resize */
    uint32_t doc_id;
    m_size   s;
    double   dpi0;               /* resolution shown when the dialog opened */
    bool     focused;            /* the initial focus was placed */
    int      resample;
    bool     gamma;
    int      anchor;             /* pc_anchor */
    int      fill;
    char     title[64];
} size_dlg;

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static pc_px32 fill_color(app *a, int fill)
{
    pc_px32 c;
    memset(&c, 0, sizeof c);
    if (fill == 1) c = app_primary(a);
    else if (fill == 2) c = app_secondary(a);
    else if (fill == 3) c = app_px_make(255, 255, 255, 255);
    else if (fill == 4) c = app_px_make(0, 0, 0, 255);
    return c;
}

/* Apply the dialog's result: one history step (size and/or resolution). */
static void apply(app *a, app_doc *d, size_dlg *g)
{
    const char *label = g->canvas ? "Canvas Size" : "Resize";
    uint32_t w = (uint32_t)m_size_px(&g->s, false), h = (uint32_t)m_size_px(&g->s, true);
    bool resized = w != d->doc->w || h != d->doc->h;
    bool new_dpi = fabs(g->s.dpi - g->dpi0) > 1e-9;
    pc_hist_node *base;
    pc_status st = PC_OK;
    if (!resized && !new_dpi) return;
    (void)app_tool_finish(a);
    if (d->txn) {
        app_error(a, "%s failed: the image is busy.", label);
        return;
    }
    base = m_hist_mark(d->hist);
    if (resized) {
        if (g->canvas)
            st = pc_geom_canvas_size(d->hist, w, h, (pc_anchor)g->anchor, fill_color(a, g->fill),
                                     &a->par, label);
        else
            st = pc_geom_resize(d->hist, w, h, k_resample_modes[g->resample],
                                g->gamma ? PC_RESAMPLE_GAMMA : 0u, &a->par, label);
    }
    if (st == PC_OK && new_dpi) {
        pc_status s2 = m_doc_set_dpi(d, g->s.dpi, g->s.dpi, label);
        if (s2 != PC_OK && s2 != PC_ERR_STATE) st = s2;
    }
    (void)m_hist_fuse(d->hist, base, label);
    if (m_hist_depth_from(d->hist, base) > 0) app_doc_history_changed(a, d);
    if (st != PC_OK) {
        app_error(a, "%s failed: %s.", label, pc_status_str(st));
        return;
    }
    if (resized) d->view.need_fit = true;
}

static bool size_frame(app *a, void *st)
{
    size_dlg *g = (size_dlg *)st;
    m_size *s = &g->s;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    app_doc *d = doc_by_id(a, g->doc_id);
    ui_size cells[3];
    char text[160], est[48], why[96];
    uint32_t r;
    bool enter, valid;
    int by = s->by;
    if (!d) return false;
    ui_dialog_begin(ui, g->title, 440.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    m_size_format_bytes(m_size_bytes(s, d->doc->n_layers), est, sizeof est);
    snprintf(text, sizeof text, "New size: %s", est);
    ui_label(ui, text);
    ui_layout_space(ui, 4.0f);

    /* By percentage / By absolute size */
    cells[0] = ui_size_px(150.0f);
    cells[1] = ui_size_px(110.0f);
    cells[2] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    if (ui_radio(ui, "By percentage:##bypct", &by, 0)) m_size_set_by(s, 0);
    {
        double pct = s->pct;
        if (ui_number_double(ui, "##pct", &pct, 0.0, M_SIZE_MAX_PCT, 1.0, 2,
                             s->by == 0 ? 0u : UI_DISABLED))
            m_size_set_pct(s, pct);
    }
    ui_label_ex(ui, "%", s->by == 0 ? 0u : UI_LABEL_DIM | UI_DISABLED);
    ui_layout_column(ui);
    if (ui_radio(ui, "By absolute size:##byabs", &by, 1)) m_size_set_by(s, 1);
    cells[0] = ui_size_px(24.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, 28.0f));
    {
        bool keep = s->by == 0 ? true : s->keep;
        if (s->by == 0) {
            ui_label_ex(ui, "Maintain aspect ratio", UI_LABEL_DIM | UI_DISABLED);
        } else if (ui_checkbox(ui, "Maintain aspect ratio##keep", &keep)) {
            m_size_set_keep(s, keep);
        }
    }
    ui_layout_column(ui);

    /* pixel size, resolution, print size */
    {
        uint32_t dis = s->by == 0 ? UI_DISABLED : 0u;
        int32_t w = m_size_px(s, false), h = m_size_px(s, true);
        double res = m_size_res(s), pw = m_size_print(s, false), ph = m_size_print(s, true);
        int ru = s->res_unit, pu = s->print_unit;
        ui_heading(ui, "Pixel size");
        cells[0] = ui_size_px(110.0f);
        cells[1] = ui_size_fr(1.0f);
        cells[2] = ui_size_px(150.0f);
        ui_layout_row(ui, 0.0f, 3, cells);
        ui_label_ex(ui, "Width:", UI_LABEL_DIM | dis);
        if (!g->focused) {
            /* O-UI-FOCUS: the first enabled numeric box starts focused
             * (its editor id is the widget id ^ 0xED17, ui_slider.c) */
            ui_set_focus(ui, ui_get_id(ui, s->by == 0 ? "##pct" : "##w") ^ 0xED17u);
            g->focused = true;
        }
        if (ui_number_int(ui, "##w", &w, 0, M_SIZE_MAX_EDIT, 1, dis) && !dis)
            m_size_set_w(s, (double)w);
        ui_label_ex(ui, "pixels", UI_LABEL_DIM | dis);
        ui_label_ex(ui, "Height:", UI_LABEL_DIM | dis);
        if (ui_number_int(ui, "##h", &h, 0, M_SIZE_MAX_EDIT, 1, dis) && !dis)
            m_size_set_h(s, (double)h);
        ui_label_ex(ui, "pixels", UI_LABEL_DIM | dis);
        ui_label_ex(ui, "Resolution:", UI_LABEL_DIM);
        if (ui_number_double(ui, "##res", &res, M_SIZE_MIN_RES, M_SIZE_MAX_RES, 1.0, 2, 0))
            m_size_set_res(s, res);
        if (ui_combo(ui, "##resu", &ru, k_res_units, 2)) m_size_set_res_unit(s, ru);
        ui_layout_column(ui);
        ui_heading(ui, "Print size");
        ui_layout_row(ui, 0.0f, 3, cells);
        ui_label_ex(ui, "Width:", UI_LABEL_DIM | dis);
        if (ui_number_double(ui, "##pw", &pw, 0.0, 1e7, 0.1, 2, dis) && !dis)
            m_size_set_print(s, false, pw);
        if (ui_combo(ui, "##pu", &pu, k_print_units, 2)) s->print_unit = pu;
        ui_label_ex(ui, "Height:", UI_LABEL_DIM | dis);
        if (ui_number_double(ui, "##ph", &ph, 0.0, 1e7, 0.1, 2, dis) && !dis)
            m_size_set_print(s, true, ph);
        ui_label_ex(ui, k_print_units[s->print_unit], UI_LABEL_DIM | dis);
        ui_layout_column(ui);
    }

    if (g->canvas) {
        ui_heading(ui, "Anchor");
        cells[0] = ui_size_px(110.0f);
        cells[1] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 2, cells);
        ui_layout_begin(ui, 0.0f);
        (void)m_anchor_grid(a, "##anchorgrid", &g->anchor, 32.0f);
        ui_layout_end(ui);
        ui_layout_begin(ui, 0.0f);
        (void)ui_combo(ui, "##anchor", &g->anchor, k_anchor_names, 9);
        ui_layout_space(ui, 6.0f);
        ui_label_ex(ui, "Fill:", UI_LABEL_DIM);
        (void)ui_combo(ui, "##fill", &g->fill, k_fill_names, 5);
        ui_layout_end(ui);
        ui_layout_column(ui);
    } else {
        ui_heading(ui, "Resampling");
        cells[0] = ui_size_fr(1.0f);
        cells[1] = ui_size_auto();
        ui_layout_row(ui, 0.0f, 2, cells);
        (void)ui_combo(ui, "##resample", &g->resample, k_resample_names, 8);
        if (ui_icon_button(ui, "##resample_reset", UI_ICON_RESET, "Reset")) g->resample = 0;
        ui_layout_column(ui);
        (void)ui_checkbox(ui, "Use gamma correction##gamma", &g->gamma);
    }
    valid = m_size_valid(s, why, sizeof why);
    if (!valid) {
        ui_rect lr;
        ui_layout_space(ui, 4.0f);
        lr = ui_layout_next(ui, 0, ui_px(ui, 18.0f));
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), lr, UI_ALIGN_LEFT,
                         UI_TEXT_ELLIPSIS, p->danger, why, strlen(why));
    }
    r = m_dlg_footer(a, valid);
    {
        uint32_t r2 = ui_dialog_end(ui);
        if (!r) r = r2;
    }
    if (enter && !r && valid) r = UI_DLG_OK;
    if (r == UI_DLG_OK && !valid) r = 0;
    if (!r) return true;
    if (r == UI_DLG_OK) {
        size_memory *m = memory(a);
        if (m) {
            if (g->canvas) {
                m->anchor = g->anchor;
                m->fill = g->fill;
            } else {
                m->resample = g->resample;
                m->gamma = g->gamma;
                m->keep = s->keep;
            }
        }
        apply(a, d, g);
    }
    return false;
}

static void open_dialog(app *a, bool canvas)
{
    app_doc *d = app_active_doc(a);
    size_memory *m = memory(a);
    size_dlg *g;
    if (!d) return;
    g = (size_dlg *)calloc(1u, sizeof *g);
    if (!g) return;
    g->canvas = canvas;
    g->doc_id = d->id;
    m_size_init(&g->s, d->doc->w, d->doc->h, d->meta.dpi_x, canvas ? false : (m ? m->keep : true),
                app_get_units(a) == APP_UNITS_CM);
    g->dpi0 = g->s.dpi;
    g->resample = m ? m->resample : 0;
    g->gamma = m ? m->gamma : true;
    g->anchor = m ? m->anchor : PC_ANCHOR_TOP_LEFT;
    g->fill = m ? m->fill : 0;
    snprintf(g->title, sizeof g->title, "%s##sizedlg", canvas ? "Canvas Size" : "Resize Image");
    (void)app_dialog_push(a, size_frame, g, free);
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
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = cmd_resize;
    (void)app_cmd_register(a, &d);
    d.id = "image.canvas_size";
    d.label = "Canvas Size...";
    d.icon = UI_ICON_CANVAS_SIZE;
    d.run = cmd_canvas;
    (void)app_cmd_register(a, &d);
}
