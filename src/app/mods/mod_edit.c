/* mod_edit.c - Edit menu commands (MENUS.md Edit, lane M): Undo, Redo, the
 * clipboard (Cut, Copy, Copy Merged, Paste, Paste into New Layer, Paste
 * into New Image, Copy Selection, Paste Selection with its five combine
 * modes) and the selection commands (Erase Selection, Fill Selection with
 * the primary or secondary color, Invert Selection, Select All, Deselect).
 *
 * Every command that changes the image is exactly one history step: the
 * compound ones (Cut = erase to transparent white + deselect, Erase
 * Selection = erase to transparent black + deselect) are fused with
 * m_hist_fuse. Paste lives in edit/m_paste.c; Paste and Paste into New
 * Layer stay registered as APP_CMD_WEAK so the Move Selected Pixels owner
 * may still replace them wholesale (the preferred integration is its float
 * hook, m_paste_set_float_hook), and because tests/app/test_app_cmd.c pins
 * that contract.
 *
 * Thread rules: main thread. Ownership: commands keep no state. */
#include "../app_internal.h"
#include "../edit/m_hist.h"
#include "../edit/m_paste.h"
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

/* The selected pixels of layer_id become color with alpha 0 (Cut leaves
 * transparent white): full coverage stores (r, g, b, 0) exactly, partial
 * coverage k keeps the color and scales the alpha by (255 - k), the same
 * premultiplied lerp toward transparent that Erase uses. Tiles without
 * coverage are not touched; pixels outside the document stay zero
 * (INV-TILE-EDGE). One transaction, committed into history directly. */
static pc_status erase_to_color(app_doc *d, uint32_t layer_id, pc_px32 color, const char *label)
{
    pc_doc *doc = d->doc;
    pc_rect r = pc_rect_intersect(pc_sel_extent(doc), pc_doc_rect(doc));
    uint8_t *cov;
    pc_txn *t;
    pc_status st = PC_OK;
    if (pc_rect_is_empty(r)) return PC_ERR_STATE;
    cov = (uint8_t *)malloc(PC_TILE_PX);
    t = cov ? pc_txn_begin(doc, label) : NULL;
    if (!t) {
        free(cov);
        return PC_ERR_NOMEM;
    }
    color.a = 0;
    for (uint32_t ty = (uint32_t)r.y >> PC_TILE_SHIFT;
         st == PC_OK && ty <= (uint32_t)(r.y + r.h - 1) >> PC_TILE_SHIFT; ty++)
        for (uint32_t tx = (uint32_t)r.x >> PC_TILE_SHIFT;
             st == PC_OK && tx <= (uint32_t)(r.x + r.w - 1) >> PC_TILE_SHIFT; tx++) {
            pc_rect tr = pc_rect_intersect(
                r, pc_rect_make((int32_t)(tx * PC_TILE_DIM), (int32_t)(ty * PC_TILE_DIM),
                                (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM));
            bool any = false;
            uint8_t *px;
            if (pc_rect_is_empty(tr)) continue;
            pc_sel_read_rect(doc, tr, cov, PC_TILE_DIM, true);
            for (int32_t y = 0; y < tr.h && !any; y++)
                for (int32_t x = 0; x < tr.w; x++)
                    if (cov[(size_t)y * PC_TILE_DIM + (size_t)x]) { any = true; break; }
            if (!any) continue;
            px = pc_txn_tile_rw(t, layer_id, ty * doc->tiles_x + tx);
            if (!px) {
                st = PC_ERR_NOMEM;
                break;
            }
            for (int32_t y = 0; y < tr.h; y++) {
                uint32_t ly = (uint32_t)(tr.y + y) - ty * PC_TILE_DIM;
                pc_px32 *row = (pc_px32 *)(void *)(px + (size_t)ly * PC_TILE_DIM * 4u);
                for (int32_t x = 0; x < tr.w; x++) {
                    uint8_t k = cov[(size_t)y * PC_TILE_DIM + (size_t)x];
                    pc_px32 *p = &row[(uint32_t)(tr.x + x) - tx * PC_TILE_DIM];
                    if (k == 255u) *p = color;
                    else if (k) p->a = (uint8_t)pc_mul255(p->a, 255u - k);
                }
            }
        }
    free(cov);
    if (st != PC_OK) {
        pc_txn_cancel(t);
        return st;
    }
    return pc_txn_commit(t, d->hist);
}

/* Replace the selected pixels of the active layer by color (through the
 * selection coverage; the whole layer without a selection), then drop the
 * selection; one history step named label. */
static pc_status erase_and_deselect(app *a, app_doc *d, pc_px32 color, const char *label)
{
    pc_layer *l = app_doc_layer(d);
    pc_hist_node *base = m_hist_mark(d->hist);
    pc_status st;
    if (!l) return PC_ERR_STATE;
    if (color.a == 0u && color.r == 0u && color.g == 0u && color.b == 0u)
        st = pc_layerop_clear(d->hist, l->id, pc_rect_make(0, 0, 0, 0), true, &a->par, label);
    else
        st = erase_to_color(d, l->id, color, label);
    if (st == PC_OK && pc_sel_is_active(d->doc)) st = pc_sel_deselect(d->hist, "Deselect");
    (void)m_hist_fuse(d->hist, base, label);
    if (m_hist_depth_from(d->hist, base) > 0) {
        app_doc_history_changed(a, d);
        if (st == PC_ERR_STATE) st = PC_OK;
    }
    return st;
}

/* Edit > Erase Selection: transparent black #00000000 (R 4.3). */
static void cmd_erase(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_px32 none;
    pc_status st;
    (void)c;
    memset(&none, 0, sizeof none);
    st = erase_and_deselect(a, d, none, "Erase Selection");
    if (st != PC_OK && st != PC_ERR_STATE)
        app_error(a, "Erase Selection failed: %s.", pc_status_str(st));
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
                    if (k < 255u) p->a = (uint8_t)pc_mul255(p->a, k);
                    if (p->a == 0u) memset(p, 0, sizeof *p);
                }
            pc_mask_free(&m);
        }
    }
    if (st != PC_OK) pc_surf_free(out);
    return st;
}

static pc_status encode_png(const pc_surf *s, const pc_image_meta *meta, const pc_par *par,
                            pc_buf *out)
{
    const pc_codec *png = pc_codec_by_id("png");
    pc_image_meta m;
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
        if (!params) {
            pc_doc_destroy(doc);
            return PC_ERR_NOMEM;
        }
        pc_codec_default_params(png, params);
    }
    /* the PNG carries the image's resolution and color profile
     * (CB-COPY-FORMATS, 5.1); the profile bytes stay borrowed */
    memset(&m, 0, sizeof m);
    if (meta) {
        m.dpi_x = meta->dpi_x;
        m.dpi_y = meta->dpi_y;
        m.icc = meta->icc;
        m.icc_len = meta->icc_len;
    }
    st = png->save(doc, &m, params, par, out);
    free(params);
    pc_doc_destroy(doc);
    return st;
}

static bool video_ok(void) { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }

static bool do_copy(app *a, bool merged)
{
    app_doc *d = app_active_doc(a);
    pc_surf s;
    pc_buf png;
    pc_status st;
    bool ok;
    if (!d) return false;
    st = copy_pixels(a, d, merged, &s);
    if (st == PC_ERR_STATE) return false;          /* nothing selected */
    if (st != PC_OK) {
        app_error(a, "Copy failed: %s.", pc_status_str(st));
        return false;
    }
    memset(&png, 0, sizeof png);
    st = encode_png(&s, &d->meta, &a->par, &png);
    /* PNG plus raw pixels for the native flavours (CB-COPY-FORMATS) */
    ok = video_ok() &&
         pal_clip_set_image_bgra(st == PC_OK ? png.p : NULL, st == PC_OK ? png.n : 0u,
                                 (const uint8_t *)s.px, s.w, s.h, (size_t)s.stride * 4u);
    pc_buf_free(&png);
    pc_surf_free(&s);
    m_paste_invalidate(a);
    if (!ok) app_error(a, "Could not place the image on the clipboard.");
    return ok;
}

static void cmd_copy(app *a, const app_cmd *c) { (void)do_copy(a, c->arg != 0); }

/* Edit > Cut: copy, then the selected pixels become transparent white
 * #00FFFFFF (EditMenu docs, R 4.0.4) and the selection is removed; one
 * history step. */
static void cmd_cut(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_status st;
    (void)c;
    if (!app_doc_layer(d) || !do_copy(a, false)) return;
    st = erase_and_deselect(a, d, app_px_make(255, 255, 255, 0), "Cut");
    if (st != PC_OK && st != PC_ERR_STATE) app_error(a, "Cut failed: %s.", pc_status_str(st));
}

static bool can_paste(app *a, const app_cmd *c)
{
    (void)c;
    return m_paste_available(a);
}

static void cmd_paste(app *a, const app_cmd *c) { m_paste_start(a, (int)c->arg); }

/* ---- selection geometry (CB-SEL-JSON) ----------------------------------------------- */
typedef struct seltext_cache { uint64_t at; bool valid, ok; } seltext_cache;

static void cmd_copy_selection(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    char *text = NULL;
    (void)c;
    if (pc_sel_copy_text(d->doc, &text, NULL) == PC_OK && text) {
        if (!video_ok() || !pal_clip_set_text(text))
            app_error(a, "Could not place the selection on the clipboard.");
    }
    free(text);
    m_paste_invalidate(a);
    {
        seltext_cache *k = (seltext_cache *)app_ext_get(a, "lane_m.seltext");
        if (k) k->valid = false;
    }
}

/* Enabled while the clipboard text looks like a selection polygon list;
 * the text is fetched at most once per second. */

static bool clip_seltext(app *a, const app_cmd *c)
{
    seltext_cache *k;
    (void)c;
    /* no SDL_HasClipboardText() gate: pal_clip_get_text checks for text in
     * the clipboard pal writes (Win32 on Windows, unseen by SDL's dummy
     * video driver); the text is cached for a second below */
    if (!video_ok()) return false;
    k = (seltext_cache *)app_ext_get(a, "lane_m.seltext");
    if (!k) {
        k = (seltext_cache *)calloc(1u, sizeof *k);
        if (!k || !app_ext_set(a, "lane_m.seltext", k, free)) {
            free(k);
            return true;
        }
    }
    if (!k->valid || app_now_ms(a) < k->at || app_now_ms(a) - k->at > 1000u) {
        char *text = pal_clip_get_text();
        k->ok = text && strstr(text, "polygonList") != NULL;
        free(text);
        k->valid = true;
        k->at = app_now_ms(a);
    }
    return k->ok;
}

static void cmd_paste_selection(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    char *text = video_ok() ? pal_clip_get_text() : NULL;
    seltext_cache *k = (seltext_cache *)app_ext_get(a, "lane_m.seltext");
    pc_status st;
    if (k) k->valid = false;
    if (!text) return;
    st = pc_sel_paste_text(d->hist, text, strlen(text), a->ts.sel_clip_aa, (pc_sel_mode)c->arg,
                           "Paste Selection");
    free(text);
    if (st == PC_ERR_FORMAT) app_error(a, "The clipboard text is not a selection.");
    else report(a, d, st, "Paste Selection");
}

/* ---- registration -------------------------------------------------------------------- */
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
    reg(a, "edit.paste", "Paste", UI_ICON_PASTE, nd | APP_CMD_WEAK, cmd_paste, can_paste, 0);
    reg(a, "edit.paste_layer", "Paste into New Layer", UI_ICON_PASTE, nd | APP_CMD_WEAK,
        cmd_paste, can_paste, 1);
    reg(a, "edit.paste_image", "Paste into New Image", UI_ICON_PASTE, 0, cmd_paste, can_paste, 2);
    reg(a, "edit.copy_selection", "Copy Selection", UI_ICON_COPY, nd, cmd_copy_selection, has_sel,
        0);
    reg(a, "edit.paste_selection.replace", "Paste Selection (Replace)", UI_ICON_SEL_REPLACE,
        nd, cmd_paste_selection, clip_seltext, PC_SEL_REPLACE);
    reg(a, "edit.paste_selection.union", "Paste Selection (Add)", UI_ICON_SEL_UNION, nd,
        cmd_paste_selection, clip_seltext, PC_SEL_UNION);
    reg(a, "edit.paste_selection.exclude", "Paste Selection (Subtract)", UI_ICON_SEL_EXCLUDE,
        nd, cmd_paste_selection, clip_seltext, PC_SEL_EXCLUDE);
    reg(a, "edit.paste_selection.intersect", "Paste Selection (Intersect)",
        UI_ICON_SEL_INTERSECT, nd, cmd_paste_selection, clip_seltext, PC_SEL_INTERSECT);
    reg(a, "edit.paste_selection.xor", "Paste Selection (Invert)", UI_ICON_SEL_XOR, nd,
        cmd_paste_selection, clip_seltext, PC_SEL_XOR);
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
