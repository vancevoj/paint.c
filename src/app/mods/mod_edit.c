/* mod_edit.c - Edit menu commands (MENUS.md Edit): undo and redo, the
 * clipboard (Copy, Copy Merged, Cut, Paste into New Image, Copy and Paste
 * Selection) and the selection commands. Paste and Paste into New Layer
 * need floating pixels (Move Selected Pixels) and are left to the move
 * tool lane: they show disabled until registered. */
#include "../app_internal.h"
#include "pal/pal_clip_raw.h"
#include "pc/pc_layerops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- history ---------------------------------------------------------------------- */
static bool can_undo(app *a, const app_cmd *c)
{
    (void)c;
    return app_doc_can_undo(app_active_doc(a)) || app_tool_live(a);
}

static bool can_redo(app *a, const app_cmd *c)
{
    (void)c;
    return app_doc_can_redo(app_active_doc(a));
}

static void cmd_undo(app *a, const app_cmd *c)
{
    (void)c;
    (void)app_doc_undo(a, app_active_doc(a));
}

static void cmd_redo(app *a, const app_cmd *c)
{
    (void)c;
    (void)app_doc_redo(a, app_active_doc(a));
}

/* ---- selection ---------------------------------------------------------------------- */
static bool has_sel(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && pc_sel_is_active(d->doc);
}

static void report(app *a, app_doc *d, pc_status st, const char *what)
{
    if (st == PC_OK) app_doc_history_changed(a, d);
    else if (st != PC_ERR_STATE) app_error(a, "%s failed: %s.", what, pc_status_str(st));
}

static void cmd_select_all(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    report(a, d, pc_sel_select_all(d->hist, "Select All"), "Select All");
}

static void cmd_deselect(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    report(a, d, pc_sel_deselect(d->hist, "Deselect"), "Deselect");
}

static void cmd_invert(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    report(a, d, pc_sel_invert(d->hist, "Invert Selection"), "Invert Selection");
}

static void cmd_erase(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    pc_status st;
    (void)c;
    if (!l) return;
    st = pc_layerop_clear(d->hist, l->id, pc_rect_make(0, 0, 0, 0), true, &a->par,
                          "Erase Selection");
    if (st == PC_OK) st = pc_sel_deselect(d->hist, "Deselect");
    report(a, d, st, "Erase Selection");
}

static void cmd_fill(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    if (!l) return;
    report(a, d, pc_layerop_fill(d->hist, l->id, pc_rect_make(0, 0, 0, 0),
                                 c->arg ? app_secondary(a) : app_primary(a), true, &a->par,
                                 "Fill Selection"),
           "Fill Selection");
}

/* ---- clipboard ----------------------------------------------------------------------- */
/* Pixels of the selection bounds (or the whole canvas), outside-selection
 * pixels zeroed and partial coverage fading alpha (CB-COPY-CONTENT). */
static pc_status copy_pixels(app *a, app_doc *d, bool merged, pc_surf *out)
{
    pc_rect r = pc_sel_extent(d->doc);
    pc_status st;
    (void)a;
    memset(out, 0, sizeof *out);
    if (pc_rect_is_empty(r)) return PC_ERR_STATE;
    st = pc_surf_alloc(out, r.w, r.h);
    if (st != PC_OK) return st;
    if (merged) {
        st = pc_comp_rect(d->doc, r, out->px, (size_t)out->stride, &a->par);
    } else {
        pc_layer *l = app_doc_layer(d);
        if (!l) st = PC_ERR_STATE;
        else pc_layer_read_rect(d->doc, l, r, out->px, (size_t)out->stride);
    }
    if (st == PC_OK && pc_sel_is_active(d->doc)) {
        pc_mask m;
        st = pc_sel_mask(d->doc, r, false, &m);
        if (st == PC_OK) {
            for (int32_t y = 0; y < r.h; y++)
                for (int32_t x = 0; x < r.w; x++) {
                    uint8_t k = m.px[(size_t)y * (size_t)m.stride + (size_t)x];
                    pc_px32 *p = &out->px[(size_t)y * (size_t)out->stride + (size_t)x];
                    if (k == 0u) memset(p, 0, sizeof *p);
                    else if (k < 255u) p->a = (uint8_t)pc_mul255(p->a, k);
                }
            pc_mask_free(&m);
        }
    }
    if (st != PC_OK) pc_surf_free(out);
    return st;
}

static pc_status encode_png(const pc_surf *s, const pc_par *par, pc_buf *out)
{
    const pc_codec *png = pc_codec_by_id("png");
    pc_doc *doc;
    pc_layer *l;
    void *params = NULL;
    pc_status st;
    if (!png || !png->save) return PC_ERR_UNSUPPORTED;
    doc = pc_doc_create((uint32_t)s->w, (uint32_t)s->h);
    if (!doc) return PC_ERR_NOMEM;
    l = pc_layer_create(doc, "Clipboard");
    st = l ? pc_layer_store_rect(doc, l, pc_rect_make(0, 0, s->w, s->h), s->px, (size_t)s->stride)
           : PC_ERR_NOMEM;
    if (st == PC_OK) st = pc_doc_reserve_layers(doc, 1u);
    if (st == PC_OK) st = pc_doc_insert_layer(doc, l, 0u);
    if (st != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return st;
    }
    if (png->params_size) {
        params = malloc(png->params_size);
        if (!params) { pc_doc_destroy(doc); return PC_ERR_NOMEM; }
        pc_codec_default_params(png, params);
    }
    st = png->save(doc, NULL, params, par, out);
    free(params);
    pc_doc_destroy(doc);
    return st;
}

static bool do_copy(app *a, bool merged)
{
    app_doc *d = app_active_doc(a);
    pc_surf s;
    pc_buf png;
    pc_status st;
    bool ok;
    if (!d) return false;
    st = copy_pixels(a, d, merged, &s);
    if (st != PC_OK) {
        app_error(a, "Copy failed: %s.", pc_status_str(st));
        return false;
    }
    memset(&png, 0, sizeof png);
    st = encode_png(&s, &a->par, &png);
    /* PNG plus raw pixels for the native flavours (CB-COPY-FORMATS) */
    ok = pal_clip_set_image_bgra(st == PC_OK ? png.p : NULL, st == PC_OK ? png.n : 0u,
                                 (const uint8_t *)s.px, s.w, s.h, (size_t)s.stride * 4u);
    pc_buf_free(&png);
    pc_surf_free(&s);
    if (!ok) app_error(a, "Could not place the image on the clipboard.");
    return ok;
}

static void cmd_copy(app *a, const app_cmd *c) { (void)do_copy(a, c->arg != 0); }

static void cmd_cut(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    pc_status st;
    (void)c;
    if (!l || !do_copy(a, false)) return;
    st = pc_layerop_clear(d->hist, l->id, pc_rect_make(0, 0, 0, 0), true, &a->par, "Cut");
    if (st == PC_OK && pc_sel_is_active(d->doc)) st = pc_sel_deselect(d->hist, "Deselect");
    report(a, d, st, "Cut");
}

static bool clip_image(app *a, const app_cmd *c)
{
    (void)c;
    return a->win && pal_clip_has_image();
}

static void cmd_paste_new_image(app *a, const app_cmd *c)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char mime[64];
    pc_doc *doc = NULL;
    pc_image_meta m;
    pc_codec_limits lim;
    pc_status st;
    (void)c;
    memset(&m, 0, sizeof m);
    if (!pal_clip_get_image(&data, &len, mime, sizeof mime)) {
        app_error(a, "The clipboard does not contain an image.");
        return;
    }
    pc_codec_limits_default(&lim);
    st = pc_codec_load_any(data, len, NULL, &lim, &doc, &m, NULL);
    free(data);
    if (st != PC_OK || !doc) {
        app_error(a, "Could not read the clipboard image: %s.", pc_status_str(st));
        pc_meta_free(&m);
        return;
    }
    {
        app_doc *d = app_doc_create(a, doc, NULL, NULL, &m, "Paste into New Image");
        if (!d) {
            app_error(a, "Could not create the image: %s.", pc_status_str(PC_ERR_NOMEM));
            return;
        }
        app_doc_set_untitled(a, d);
        (void)app_add_doc(a, d);
    }
}

static void cmd_copy_selection(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    char *text = NULL;
    (void)c;
    if (pc_sel_copy_text(d->doc, &text, NULL) == PC_OK && text) {
        if (!pal_clip_set_text(text))
            app_error(a, "Could not place the selection on the clipboard.");
    }
    free(text);
}

static bool clip_text(app *a, const app_cmd *c)
{
    (void)c;
    return a->win && SDL_HasClipboardText();
}

static void cmd_paste_selection(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    char *text = pal_clip_get_text();
    pc_status st;
    if (!text) return;
    st = pc_sel_paste_text(d->hist, text, strlen(text), a->ts.sel_clip_aa, (pc_sel_mode)c->arg,
                           "Paste Selection");
    free(text);
    if (st == PC_ERR_FORMAT) app_error(a, "The clipboard text is not a selection.");
    else report(a, d, st, "Paste Selection");
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

void mod_edit(app *a)
{
    const uint32_t nd = APP_CMD_NEEDS_DOC;
    reg(a, "edit.undo", "Undo", UI_ICON_UNDO, nd | APP_CMD_REPEAT, cmd_undo, can_undo, 0);
    reg(a, "edit.redo", "Redo", UI_ICON_REDO, nd | APP_CMD_REPEAT, cmd_redo, can_redo, 0);
    reg(a, "edit.cut", "Cut", UI_ICON_CUT, nd, cmd_cut, NULL, 0);
    reg(a, "edit.copy", "Copy", UI_ICON_COPY, nd, cmd_copy, NULL, 0);
    reg(a, "edit.copy_merged", "Copy Merged", UI_ICON_COPY, nd, cmd_copy, NULL, 1);
    reg(a, "edit.paste_image", "Paste into New Image", UI_ICON_PASTE, 0, cmd_paste_new_image,
        clip_image, 0);
    reg(a, "edit.copy_selection", "Copy Selection", UI_ICON_COPY, nd, cmd_copy_selection, has_sel,
        0);
    reg(a, "edit.paste_selection.replace", "Paste Selection (Replace)", UI_ICON_SEL_REPLACE, nd,
        cmd_paste_selection, clip_text, PC_SEL_REPLACE);
    reg(a, "edit.paste_selection.union", "Paste Selection (Add)", UI_ICON_SEL_UNION, nd,
        cmd_paste_selection, clip_text, PC_SEL_UNION);
    reg(a, "edit.paste_selection.exclude", "Paste Selection (Subtract)", UI_ICON_SEL_EXCLUDE, nd,
        cmd_paste_selection, clip_text, PC_SEL_EXCLUDE);
    reg(a, "edit.paste_selection.intersect", "Paste Selection (Intersect)", UI_ICON_SEL_INTERSECT,
        nd, cmd_paste_selection, clip_text, PC_SEL_INTERSECT);
    reg(a, "edit.paste_selection.xor", "Paste Selection (Invert)", UI_ICON_SEL_XOR, nd,
        cmd_paste_selection, clip_text, PC_SEL_XOR);
    reg(a, "edit.erase_selection", "Erase Selection", UI_ICON_TOOL_ERASER, nd, cmd_erase, has_sel,
        0);
    reg(a, "edit.fill_selection", "Fill Selection", UI_ICON_TOOL_PAINT_BUCKET, nd, cmd_fill,
        has_sel, 0);
    reg(a, "edit.fill_selection_secondary", "Fill Selection with Secondary Color",
        UI_ICON_TOOL_PAINT_BUCKET, nd, cmd_fill, has_sel, 1);
    reg(a, "edit.invert_selection", "Invert Selection", UI_ICON_SELECT_ALL, nd, cmd_invert,
        has_sel, 0);
    reg(a, "edit.select_all", "Select All", UI_ICON_SELECT_ALL, nd, cmd_select_all, NULL, 0);
    reg(a, "edit.deselect", "Deselect", UI_ICON_DESELECT, nd, cmd_deselect, has_sel, 0);
}
