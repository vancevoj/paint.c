/* tool_color_picker.c - Color Picker (TOOLS.md 10.1 and 3.7, lane B).
 *
 *  - A left click sets the primary color, a right click the secondary,
 *    alpha included; dragging keeps picking (3.36 ColorPickerTool picks on
 *    press and on every move while the button is down). Clicks outside the
 *    image pick nothing.
 *  - Sampling: Layer (the active layer) or Image (the visible layers
 *    flattened; hidden layers are ignored at once); Ctrl held samples the
 *    image for that click (K-PICKER-IMAGE). The option is shared with the
 *    Paint Bucket and the Magic Wand (5.1 documentation).
 *  - Sample size: Single Pixel, 3 x 3, 5 x 5, 11 x 11, 31 x 31 or 51 x 51
 *    pixels around the pixel under the pointer, clipped to the image and
 *    averaged with alpha weighting (the 3.36 ColorBgra.Blend rule: alpha =
 *    sum(a) / n, color = sum(c a) / sum(a), truncated; all transparent
 *    gives #00000000).
 *  - After click (on release): Do not switch tool, Switch to previous tool
 *    (the tool that was active before the Color Picker), Switch to Pencil
 *    tool.
 *
 * Settings: tool.color_picker.size (0..5) and tool.color_picker.after
 * (0..2). Options bar (5.1 documentation toolbar image): Sampling, Sample
 * size, After click, Selection clipping. */
#include "paint_common.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_SIZE  "tool.color_picker.size"
#define KEY_AFTER "tool.color_picker.after"

static const int32_t k_sizes[6] = { 1, 3, 5, 11, 31, 51 };
static const char *const k_size_names[6] = {
    "Single Pixel", "3 \xC3\x97 3 pixels", "5 \xC3\x97 5 pixels", "11 \xC3\x97 11 pixels",
    "31 \xC3\x97 31 pixels", "51 \xC3\x97 51 pixels"
};
static const char *const k_after_names[3] = {
    "Do not switch tool", "Switch to previous tool", "Switch to Pencil tool"
};

typedef struct picker_state {
    int32_t size, after;
    char    prev[64];       /* tool active before the picker ("" = none) */
    bool    picking;
    int     button;
    bool    hover;
    double  hx, hy;
} picker_state;

/* Average of n straight-alpha pixels (3.36 ColorBgra.Blend). */
static pc_px32 average(const pc_px32 *px, size_t n)
{
    uint64_t sa = 0, sb = 0, sg = 0, sr = 0;
    pc_px32 o;
    memset(&o, 0, sizeof o);
    if (n == 0) return o;
    for (size_t i = 0; i < n; i++) sa += px[i].a;
    o.a = (uint8_t)(sa / (uint64_t)n);
    if (sa == 0) return o;
    for (size_t i = 0; i < n; i++) {
        sb += (uint64_t)px[i].b * px[i].a;
        sg += (uint64_t)px[i].g * px[i].a;
        sr += (uint64_t)px[i].r * px[i].a;
    }
    o.b = (uint8_t)(sb / sa);
    o.g = (uint8_t)(sg / sa);
    o.r = (uint8_t)(sr / sa);
    return o;
}

static void pick(app *a, picker_state *ps, const app_pointer *ev)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    int32_t x, y, n = k_sizes[ps->size], h = n / 2;
    pc_rect r;
    pc_px32 buf[51 * 51], c;
    bool image = a->ts.sampling == 1 || paint_ctrl(ev->mods);
    if (!d || !l) return;
    paint_doc_pixel(a, ev, &x, &y);
    if (x < 0 || y < 0 || x >= (int32_t)d->doc->w || y >= (int32_t)d->doc->h) return;
    r = pc_rect_intersect(pc_rect_make(x - h, y - h, n, n), pc_doc_rect(d->doc));
    if (pc_rect_is_empty(r)) return;
    if (image) {
        if (pc_comp_rect(d->doc, r, buf, (size_t)r.w, &a->par) != PC_OK) return;
    } else {
        pc_layer_read_rect(d->doc, l, r, buf, (size_t)r.w);
    }
    c = average(buf, (size_t)r.w * (size_t)r.h);
    if (ps->button == APP_BTN_LEFT) app_set_primary(a, c);
    else app_set_secondary(a, c);
}

static void after_click(app *a, picker_state *ps)
{
    if (ps->after == 1 && ps->prev[0]) (void)app_tool_select(a, ps->prev);
    else if (ps->after == 2) (void)app_tool_select(a, "pencil");
}

static void picker_pointer(app *a, void *st, const app_pointer *ev)
{
    picker_state *ps = (picker_state *)st;
    if (ev->kind != APP_PTR_CANCEL) {
        paint_doc_pos(a, ev, &ps->hx, &ps->hy);
        ps->hover = true;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (ps->picking) break;
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        ps->picking = true;
        ps->button = ev->button;
        pick(a, ps, ev);
        break;
    case APP_PTR_MOVE:
        if (ps->picking) pick(a, ps, ev);
        break;
    case APP_PTR_UP:
        if (ps->picking && ev->button == ps->button) {
            ps->picking = false;
            after_click(a, ps);
        }
        break;
    case APP_PTR_CANCEL:
        ps->picking = false;
        break;
    default:
        break;
    }
}

static void picker_overlay(app *a, void *st, app_overlay *o)
{
    picker_state *ps = (picker_state *)st;
    int32_t n = k_sizes[ps->size], x, y;
    if (!ps->hover || !app_canvas_over(a) || n <= 1) return;
    x = (int32_t)floor(ps->hx) - n / 2;
    y = (int32_t)floor(ps->hy) - n / 2;
    /* the sampled square, visible on any background */
    app_ov_rect(o, (double)x, (double)y, (double)n, (double)n, 3.0f, ui_rgba(0, 0, 0, 140), 0);
    app_ov_rect(o, (double)x, (double)y, (double)n, (double)n, 1.0f,
                ui_rgba(255, 255, 255, 230), 0);
}

static void picker_activate(app *a, void *st)
{
    picker_state *ps = (picker_state *)st;
    const app_tool *prev = app_tool_at(a, a->tool_prev), *self = app_tool_current(a);
    if (prev && prev != self) snprintf(ps->prev, sizeof ps->prev, "%s", prev->id);
}

static void picker_deactivate(app *a, void *st)
{
    picker_state *ps = (picker_state *)st;
    (void)a;
    ps->picking = false;
    ps->hover = false;
}

static void picker_init(app *a, void *st)
{
    picker_state *ps = (picker_state *)st;
    ps->size = paint_setting_get(a, KEY_SIZE, 0, 0, 5);
    ps->after = paint_setting_get(a, KEY_AFTER, 0, 0, 2);
}

static void picker_options(app *a, void *st)
{
    picker_state *ps = (picker_state *)st;
    static const paint_glyph sg[6] = { PG_SIZE_1, PG_SIZE_3, PG_SIZE_5, PG_SIZE_11, PG_SIZE_31,
                                       PG_SIZE_51 };
    static const ui_icon ai[3] = { UI_ICON_TOOL_COLOR_PICKER, UI_ICON_UNDO,
                                   UI_ICON_TOOL_PENCIL };
    ui_rect br;
    int r;
    paint_opt_sampling(a);
    r = paint_split_button(a, "##pick_size", sg[ps->size], UI_ICON_NONE,
                           k_size_names[ps->size], true, "Sample size", &br);
    if (r == 2) ui_popup_open(a->ui, "##pick_size_menu", br, UI_POPUP_BELOW);
    if (ui_popup_begin(a->ui, "##pick_size_menu")) {
        int32_t nv = ps->size;
        for (int32_t i = 0; i < 6; i++)
            if (paint_menu_item(a, k_size_names[i], ps->size == i)) nv = i;
        ui_popup_end(a->ui);
        if (nv != ps->size) {
            ps->size = nv;
            paint_setting_set(a, KEY_SIZE, nv);
        }
    }
    app_opt_separator(a);
    app_opt_label(a, "After click:");
    r = paint_split_button(a, "##pick_after", PG_NONE, ai[ps->after], k_after_names[ps->after],
                           true, "What happens after a color was picked", &br);
    if (r == 2) ui_popup_open(a->ui, "##pick_after_menu", br, UI_POPUP_BELOW);
    if (ui_popup_begin(a->ui, "##pick_after_menu")) {
        int32_t nv = ps->after;
        for (int32_t i = 0; i < 3; i++)
            if (paint_menu_item(a, k_after_names[i], ps->after == i)) nv = i;
        ui_popup_end(a->ui);
        if (nv != ps->after) {
            ps->after = nv;
            paint_setting_set(a, KEY_AFTER, nv);
        }
    }
    app_opt_separator(a);
    app_opt_sel_clip(a);
}

const app_tool app_tool_color_picker = {
    "color_picker",
    "Color Picker",
    "Left click to set the primary color from the image, right click for the secondary "
    "color. Hold Ctrl to sample the whole image.",
    'K',
    14,
    UI_ICON_TOOL_COLOR_PICKER,
    APP_CURSOR_PICKER,
    0u,
    sizeof(picker_state),
    picker_init,
    NULL,                     /* fini */
    picker_activate,
    picker_deactivate,
    picker_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    picker_options,
    picker_overlay,
    NULL,                     /* live */
    NULL,                     /* commit */
    NULL,                     /* cancel */
    NULL,                     /* cursor_at */
    NULL                      /* settings_changed */
};
