/* test_m_edit.c - lane M: the Edit menu. History fusing (m_hist_fuse),
 * Cut / Erase Selection / Fill Selection values and single history steps,
 * the selection commands and their enable states, Copy content (selected
 * pixels only, partial coverage), Paste placement in the visible area,
 * Paste into New Layer / New Image, the Expand Canvas prompt (expand, keep,
 * cancel), the float hook, pasting image files and data URIs from text,
 * Copy Selection / Paste Selection. Headless, dummy video driver. */
#include "pc_test.h"
#include "app_test_util.h"

#include "edit/m_hist.h"
#include "edit/m_paste.h"
#include "pc/pc_layerops.h"

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 2);
}

static app *with_image(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1024, 768);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

static pc_px32 layer_px(app_doc *d, uint32_t layer_index, uint32_t x, uint32_t y)
{
    return pc_layer_get_px(d->doc->stack[layer_index], x, y);
}

/* Applied steps (root..current), not counting the redo chain. */
static size_t hist_len(app_doc *d)
{
    size_t cur = 0;
    (void)app_doc_history_list(d, NULL, 0, &cur);
    return cur + 1u;
}

/* An image with a recognizable pattern (doc pixel = x, y, 7, 255). */
static pc_doc *pattern_doc(uint32_t w, uint32_t h)
{
    pc_doc *doc = pc_doc_create(w, h);
    pc_layer *l = doc ? pc_layer_create(doc, "Background") : NULL;
    pc_surf s;
    if (!l || pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return NULL;
    }
    for (int32_t y = 0; y < s.h; y++)
        for (int32_t x = 0; x < s.w; x++) {
            pc_px32 *p = &pc_surf_row(&s, y)[x];
            p->r = (uint8_t)x;
            p->g = (uint8_t)y;
            p->b = 7;
            p->a = 255;
        }
    (void)pc_layer_store_rect(doc, l, pc_rect_make(0, 0, s.w, s.h), s.px, (size_t)s.stride);
    pc_surf_free(&s);
    (void)pc_doc_reserve_layers(doc, 1u);
    (void)pc_doc_insert_layer(doc, l, 0u);
    return doc;
}

/* ---- m_hist_fuse on a bare document -------------------------------------------------------- */
static void t_hist_fuse(void)
{
    pc_doc *doc = pc_doc_create(100, 80);
    pc_hist *h = doc ? pc_hist_create(doc) : NULL;
    pc_layer *l = doc ? pc_layer_create(doc, "Background") : NULL;
    pc_hist_node *base;
    uint64_t f0, f1;
    uint32_t nid = 0;
    size_t count0;
    CHECK(doc && h && l);
    if (!doc || !h || !l) return;
    CHECK(pc_hist_add_layer(h, l, 0u, "Add") == PC_OK);
    f0 = pc_doc_fingerprint(doc);
    count0 = h->count;
    /* nothing to fuse, and one node is only relabeled */
    base = m_hist_mark(h);
    CHECK(m_hist_fuse(h, base, "X") == PC_OK && h->cur == base && m_hist_depth_from(h, base) == 0);
    CHECK(pc_sel_select_all(h, "Select All") == PC_OK);
    CHECK(m_hist_fuse(h, base, "Renamed") == PC_OK && strcmp(h->cur->label, "Renamed") == 0);
    CHECK(pc_hist_undo(h));
    /* three different operations become one involution step */
    base = m_hist_mark(h);
    CHECK(pc_layerop_fill(h, l->id, pc_rect_make(10, 10, 20, 20), app_px_make(255, 0, 0, 255),
                          false, NULL, "Fill") == PC_OK);
    CHECK(pc_layerop_add_new(h, l->id, &nid, "Add New Layer") == PC_OK);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(5, 5, 30, 30), PC_SEL_REPLACE, "Sel") == PC_OK);
    CHECK(m_hist_depth_from(h, base) == 3);
    f1 = pc_doc_fingerprint(doc);
    CHECK(m_hist_fuse(h, base, "Compound") == PC_OK);
    CHECK(m_hist_depth_from(h, base) == 1 && strcmp(h->cur->label, "Compound") == 0);
    CHECK(h->count == count0 + 1u + 1u);       /* the undone Select All branch is still there */
    CHECK(pc_doc_fingerprint(doc) == f1 && doc->n_layers == 2u && doc->sel_active);
    for (int i = 0; i < 3; i++) {
        CHECK(pc_hist_undo(h));
        CHECK(pc_doc_fingerprint(doc) == f0 && doc->n_layers == 1u && !doc->sel_active);
        CHECK(pc_layer_get_px(l, 15, 15).a == 0u);
        CHECK(pc_hist_redo(h));
        CHECK(pc_doc_fingerprint(doc) == f1 && doc->n_layers == 2u && doc->sel_active);
        CHECK(pc_layer_get_px(l, 15, 15).r == 255u);
    }
    CHECK(pc_hist_bytes(h) > 0u);
    /* a chain with a side branch is refused and left alone */
    base = m_hist_mark(h);
    CHECK(pc_sel_deselect(h, "A") == PC_OK);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(1, 1, 5, 5), PC_SEL_REPLACE, "B") == PC_OK);
    CHECK(pc_hist_undo(h));
    CHECK(pc_sel_select_all(h, "C") == PC_OK);  /* sibling of B under A */
    {
        size_t cnt = h->count;
        pc_hist_node *cur = h->cur;
        CHECK(m_hist_fuse(h, base, "Bad") == PC_ERR_STATE);
        CHECK(h->count == cnt && h->cur == cur && strcmp(cur->label, "C") == 0);
    }
    CHECK(m_hist_fuse(h, NULL, "x") == PC_ERR_ARG);
    pc_hist_destroy(h);
    pc_doc_destroy(doc);
}

/* ---- Cut, Erase, Fill, selection commands ------------------------------------------------ */
static void t_erase_cut_fill(void)
{
    app *a = with_image(80, 60, app_px_make(10, 20, 30, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* enable states without a selection */
    CHECK(!app_cmd_enabled(a, "edit.erase_selection") && !app_cmd_enabled(a, "edit.deselect"));
    CHECK(!app_cmd_enabled(a, "edit.fill_selection") &&
          !app_cmd_enabled(a, "edit.invert_selection"));
    CHECK(!app_cmd_enabled(a, "edit.copy_selection") &&
          !app_cmd_enabled(a, "image.crop_to_selection"));
    CHECK(app_cmd_enabled(a, "edit.select_all") && app_cmd_enabled(a, "edit.cut"));
    CHECK(app_cmd_enabled(a, "edit.copy") && app_cmd_enabled(a, "edit.copy_merged"));
    CHECK(app_cmd_enabled(a, "edit.paste_image") == m_paste_available(a));
    /* Erase Selection: transparent black, selection removed, one step */
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(10, 10, 20, 10), PC_SEL_REPLACE, "Sel") == PC_OK);
    app_doc_history_changed(a, d);
    n0 = hist_len(d);
    CHECK(app_cmd_enabled(a, "edit.erase_selection"));
    CHECK(app_cmd_exec(a, "edit.erase_selection"));
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Erase Selection") == 0);
    CHECK(px_eq(layer_px(d, 0, 15, 15), 0, 0, 0, 0) &&
          px_eq(layer_px(d, 0, 5, 5), 10, 20, 30, 255));
    CHECK(!pc_sel_is_active(d->doc));
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(px_eq(layer_px(d, 0, 15, 15), 10, 20, 30, 255) && pc_sel_is_active(d->doc));
    /* Cut: transparent white (EditMenu docs), selection removed, one step */
    CHECK(app_cmd_exec(a, "edit.cut"));
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Cut") == 0);
    CHECK(px_eq(layer_px(d, 0, 15, 15), 255, 255, 255, 0) && !pc_sel_is_active(d->doc));
    CHECK(px_eq(layer_px(d, 0, 9, 9), 10, 20, 30, 255) &&
          px_eq(layer_px(d, 0, 30, 20), 10, 20, 30, 255));
    CHECK(app_cmd_exec(a, "edit.undo") && px_eq(layer_px(d, 0, 15, 15), 10, 20, 30, 255));
    CHECK(app_cmd_exec(a, "edit.redo") && px_eq(layer_px(d, 0, 15, 15), 255, 255, 255, 0));
    CHECK(app_cmd_exec(a, "edit.undo") && pc_sel_is_active(d->doc));
    /* Fill Selection with primary (Backspace) and secondary (Shift+Backspace) */
    app_set_primary(a, app_px_make(200, 0, 0, 255));
    app_set_secondary(a, app_px_make(0, 0, 200, 255));
    tap(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
    CHECK(px_eq(layer_px(d, 0, 15, 15), 200, 0, 0, 255) && pc_sel_is_active(d->doc));
    CHECK(strcmp(d->hist->cur->label, "Fill Selection") == 0);
    tap(a, SDLK_BACKSPACE, SDL_KMOD_LSHIFT);
    CHECK(px_eq(layer_px(d, 0, 15, 15), 0, 0, 200, 255));
    /* Invert, Select All, Deselect */
    CHECK(app_cmd_exec(a, "edit.invert_selection"));
    CHECK(pc_sel_coverage(d->doc, 15, 15) == 0u && pc_sel_coverage(d->doc, 2, 2) == 255u);
    tap(a, SDLK_A, SDL_KMOD_LCTRL);
    CHECK(pc_sel_coverage(d->doc, 15, 15) == 255u &&
          strcmp(d->hist->cur->label, "Select All") == 0);
    tap(a, SDLK_D, SDL_KMOD_LCTRL);
    CHECK(!pc_sel_is_active(d->doc) && strcmp(d->hist->cur->label, "Deselect") == 0);
    /* Delete without a selection does nothing (disabled) */
    n0 = hist_len(d);
    tap(a, SDLK_DELETE, SDL_KMOD_NONE);
    CHECK(hist_len(d) == n0);
    /* Cut without a selection: the whole layer */
    CHECK(app_cmd_exec(a, "edit.cut"));
    CHECK(px_eq(layer_px(d, 0, 79, 59), 255, 255, 255, 0) &&
          px_eq(layer_px(d, 0, 0, 0), 255, 255, 255, 0));
    app_destroy(a);
}

/* ---- paste placement with a decoded image (no clipboard needed) ------------------------- */
static void t_paste_image(void)
{
    app *a = with_image(200, 150, app_px_make(255, 255, 255, 255));
    app_doc *d;
    size_t n0;
    int32_t x = -1, y = -1;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* origin visible: 0, 0 */
    m_paste_position(a, d, 20, 10, &x, &y);
    CHECK(x == 0 && y == 0);
    n0 = hist_len(d);
    m_paste_image(a, 0, pattern_doc(30, 20));
    at_frames(a, 3);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Paste") == 0);
    CHECK(px_eq(layer_px(d, 0, 3, 4), 3, 4, 7, 255) &&
          px_eq(layer_px(d, 0, 30, 20), 255, 255, 255, 255));
    CHECK(pc_sel_is_active(d->doc) && pc_sel_coverage(d->doc, 29, 19) == 255u &&
          pc_sel_coverage(d->doc, 30, 19) == 0u);
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(px_eq(layer_px(d, 0, 3, 4), 255, 255, 255, 255) && !pc_sel_is_active(d->doc));
    /* scrolled: the top left of the visible part */
    app_view_set_zoom(a, d, 8.0);
    at_frames(a, 2);
    {
        gfx_view v = app_doc_gview(a, d);
        double vx0, vy0, vx1, vy1;
        gfx_view_visible(&v, &vx0, &vy0, &vx1, &vy1);
        CHECK(vx0 > 1.0 && vy0 > 1.0);
        m_paste_position(a, d, 4, 4, &x, &y);
        CHECK(x >= (int32_t)vx0 && x <= (int32_t)vx0 + 1 && y >= (int32_t)vy0 &&
              y <= (int32_t)vy0 + 1);
        m_paste_image(a, 0, pattern_doc(4, 4));
        at_frames(a, 3);
        CHECK(px_eq(layer_px(d, 0, (uint32_t)x + 1u, (uint32_t)y + 2u), 1, 2, 7, 255));
        /* a large image placed near the right edge stays inside the canvas */
        m_paste_position(a, d, 190, 140, &x, &y);
        CHECK(x + 190 <= 200 && y + 140 <= 150 && x >= 0 && y >= 0);
    }
    app_view_fit_toggle(a, d);
    at_frames(a, 2);
    /* Paste into New Layer: one step, above the active layer, selected */
    n0 = hist_len(d);
    m_paste_image(a, 1, pattern_doc(10, 10));
    at_frames(a, 3);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Paste into New Layer") == 0);
    CHECK(px_eq(layer_px(d, 1, 5, 6), 5, 6, 7, 255) && px_eq(layer_px(d, 1, 50, 50), 0, 0, 0, 0));
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->n_layers == 1u);
    /* Paste into New Image: exact size, one layer, no selection */
    m_paste_image(a, 2, pattern_doc(33, 17));
    at_frames(a, 3);
    CHECK(app_doc_count(a) == 2);
    {
        app_doc *n = app_active_doc(a);
        CHECK(n != d && n->doc->w == 33u && n->doc->h == 17u && n->doc->n_layers == 1u);
        CHECK(strcmp(n->doc->stack[0]->name, "Background") == 0 && !pc_sel_is_active(n->doc));
        CHECK(px_eq(layer_px(n, 0, 32, 16), 32, 16, 7, 255) && !app_doc_can_undo(n));
    }
    app_destroy(a);
}

/* ---- Expand Canvas prompt -------------------------------------------------------------- */
static void t_expand_prompt(void)
{
    app *a = with_image(40, 30, app_px_make(255, 255, 255, 255));
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = hist_len(d);
    /* Escape cancels: nothing changes */
    m_paste_image(a, 0, pattern_doc(60, 20));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    at_frames(a, 2);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a) && hist_len(d) == n0 && d->doc->w == 40u);
    /* Enter expands (Canvas Size step, transparent new area), then pastes */
    m_paste_image(a, 0, pattern_doc(60, 20));
    at_frames(a, 3);
    at_frames(a, 2);
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 60u && d->doc->h == 30u);
    CHECK(hist_len(d) == n0 + 2u && strcmp(d->hist->cur->label, "Paste") == 0);
    CHECK(px_eq(layer_px(d, 0, 50, 10), 50, 10, 7, 255) &&
          px_eq(layer_px(d, 0, 50, 25), 0, 0, 0, 0));
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo") && d->doc->w == 40u);
    /* Keep canvas size (Tab to the second button, Space): clipped paste */
    m_paste_image(a, 0, pattern_doc(60, 20));
    at_frames(a, 3);
    at_frames(a, 2);
    tap(a, SDLK_TAB, SDL_KMOD_NONE);
    tap(a, SDLK_SPACE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 40u);
    CHECK(px_eq(layer_px(d, 0, 39, 19), 39, 19, 7, 255) &&
          strcmp(d->hist->cur->label, "Paste") == 0);
    app_destroy(a);
}

/* ---- float hook ------------------------------------------------------------------------ */
typedef struct hook_rec {
    int      calls;
    bool     take;
    int32_t  x, y, w, h;
    uint32_t layer_id;
    char     label[64];
} hook_rec;

static bool hook_paste(app *a, app_doc *d, uint32_t layer_id, pc_surf *px, int32_t x, int32_t y,
                       const char *label, void *ud)
{
    hook_rec *r = (hook_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->x = x;
    r->y = y;
    r->w = px->w;
    r->h = px->h;
    r->layer_id = layer_id;
    snprintf(r->label, sizeof r->label, "%s", label);
    if (r->take) pc_surf_free(px);          /* the hook owns the pixels now */
    return r->take;
}

static void t_float_hook(void)
{
    app *a = with_image(100, 80, app_px_make(255, 255, 255, 255));
    app_doc *d;
    hook_rec rec;
    m_float_hook hk;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    memset(&rec, 0, sizeof rec);
    rec.take = true;
    hk.paste = hook_paste;
    hk.ud = &rec;
    CHECK(m_paste_float_hook(a) == NULL);
    CHECK(m_paste_set_float_hook(a, &hk) && m_paste_float_hook(a) != NULL);
    n0 = hist_len(d);
    m_paste_image(a, 0, pattern_doc(12, 9));
    at_frames(a, 3);
    CHECK(rec.calls == 1 && rec.w == 12 && rec.h == 9 && rec.x == 0 && rec.y == 0);
    CHECK(strcmp(rec.label, "Paste") == 0 && rec.layer_id == d->layer_id);
    CHECK(hist_len(d) == n0 && px_eq(layer_px(d, 0, 2, 2), 255, 255, 255, 255));
    /* Paste into New Layer adds the layer first and hands it over */
    m_paste_image(a, 1, pattern_doc(5, 5));
    at_frames(a, 3);
    CHECK(rec.calls == 2 && d->doc->n_layers == 2u && rec.layer_id == d->doc->stack[1]->id);
    CHECK(strcmp(rec.label, "Paste into New Layer") == 0);
    /* a hook that declines: the fallback places the pixels */
    rec.take = false;
    m_paste_image(a, 0, pattern_doc(5, 5));
    at_frames(a, 3);
    CHECK(rec.calls == 3 && px_eq(layer_px(d, 1, 4, 4), 4, 4, 7, 255));
    CHECK(m_paste_set_float_hook(a, NULL) && m_paste_float_hook(a) == NULL);
    app_destroy(a);
}

/* ---- clipboard: copy content, paste from the clipboard, text flavours ------------------ */
static void b64(const uint8_t *p, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[k++] = t[(v >> 18) & 63u];
        out[k++] = t[(v >> 12) & 63u];
        out[k++] = i + 1 < n ? t[(v >> 6) & 63u] : '=';
        out[k++] = i + 2 < n ? t[v & 63u] : '=';
    }
    out[k] = '\0';
}

static void t_clipboard(void)
{
    app *a = with_image(64, 48, app_px_make(255, 255, 255, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    app_set_primary(a, app_px_make(0, 128, 0, 255));
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(0, 0, 64, 48), PC_SEL_REPLACE, "S") == PC_OK);
    CHECK(app_cmd_exec(a, "edit.fill_selection"));
    /* an ellipse-like selection: copy has zeros outside, alpha fades on the edge */
    {
        pc_mask m;
        CHECK(pc_mask_alloc(&m, pc_rect_make(10, 10, 20, 20)) == PC_OK);
        for (int32_t y = 0; y < 20; y++)
            for (int32_t x = 0; x < 20; x++)
                m.px[(size_t)y * (size_t)m.stride + (size_t)x] =
                    (uint8_t)(x < 10 ? 255 : (x < 15 ? 128 : 0));
        CHECK(pc_sel_apply(d->hist, &m, PC_SEL_REPLACE, "Mask") == PC_OK);
        pc_mask_free(&m);
        app_doc_history_changed(a, d);
    }
    CHECK(app_cmd_exec(a, "edit.copy"));
    if (!pal_clip_has_image()) {
        INFO("no clipboard image support in this environment; skipping clipboard checks");
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_enabled(a, "edit.paste") && app_cmd_enabled(a, "edit.paste_image"));
    CHECK(app_cmd_exec(a, "edit.paste_image"));
    at_frames(a, 3);
    CHECK(app_doc_count(a) == 2);
    {
        app_doc *n = app_active_doc(a);
        CHECK(n->doc->w == 15u && n->doc->h == 20u);         /* selection bounds */
        CHECK(px_eq(layer_px(n, 0, 2, 2), 0, 128, 0, 255));
        CHECK(layer_px(n, 0, 12, 2).a == 128u && layer_px(n, 0, 12, 2).g == 128u);
    }
    /* Copy Selection writes the polygon list; Paste Selection restores it */
    app_set_active_doc(a, d);
    at_frames(a, 1);
    CHECK(app_cmd_exec(a, "edit.copy_selection"));
    {
        char *t = pal_clip_get_text();
        CHECK(t && strstr(t, "polygonList") != NULL);
        free(t);
    }
    CHECK(app_cmd_exec(a, "edit.deselect") && !pc_sel_is_active(d->doc));
    at_frames(a, 2);
    CHECK(app_cmd_enabled(a, "edit.paste_selection.replace"));
    CHECK(app_cmd_exec(a, "edit.paste_selection.replace"));
    CHECK(pc_sel_is_active(d->doc) && pc_sel_coverage(d->doc, 12, 12) > 200u &&
          pc_sel_coverage(d->doc, 40, 40) == 0u);
    CHECK(app_cmd_exec(a, "edit.paste_selection.xor"));
    /* the same shape again cancels out: nothing stays selected */
    CHECK(!pc_sel_is_active(d->doc));
    /* CB-PASTE-BASE64: a data URI on the clipboard pastes as an image */
    {
        pc_doc *img = pattern_doc(6, 5);
        const pc_codec *png = pc_codec_by_id("png");
        pc_buf buf;
        void *params = png ? malloc(png->params_size ? png->params_size : 1u) : NULL;
        memset(&buf, 0, sizeof buf);
        if (png && params && img) {
            char *text;
            pc_codec_default_params(png, params);
            CHECK(png->save(img, NULL, params, NULL, &buf) == PC_OK);
            text = (char *)malloc(buf.n * 2u + 64u);
            if (text) {
                strcpy(text, "data:image/png;base64,");
                b64(buf.p, buf.n, text + strlen(text));
                CHECK(pal_clip_set_text(text));
                free(text);
                at_frames(a, 2);
                app_set_active_doc(a, d);
                m_paste_start(a, 2);
                at_frames(a, 3);
                CHECK(app_active_doc(a)->doc->w == 6u && app_active_doc(a)->doc->h == 5u);
                CHECK(px_eq(layer_px(app_active_doc(a), 0, 5, 4), 5, 4, 7, 255));
            }
            /* CB-PASTE-FILES: a file name / file URI pastes that file */
            {
                char path[1024], uri[1200];
                at_out_path(path, sizeof path, "test_m_edit_clip.png");
                CHECK(pal_write_file_atomic(path, buf.p, buf.n) == PC_OK);
                snprintf(uri, sizeof uri, "file://%s", path);
                CHECK(pal_clip_set_text(uri));
                at_frames(a, 2);
                m_paste_start(a, 2);
                at_frames(a, 3);
                CHECK(app_doc_count(a) == 4 && app_active_doc(a)->doc->w == 6u);
                (void)pal_remove(path);
            }
            /* text that is no image: an error, nothing pasted */
            CHECK(pal_clip_set_text("hello"));
            {
                int32_t before = app_doc_count(a);
                m_paste_start(a, 2);
                at_frames(a, 3);
                CHECK(app_doc_count(a) == before && app_dialog_active(a));
                tap(a, SDLK_RETURN, SDL_KMOD_NONE);
            }
        }
        pc_buf_free(&buf);
        free(params);
        pc_doc_destroy(img);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_hist_fuse);
    RUN(t_erase_cut_fill);
    RUN(t_paste_image);
    RUN(t_expand_prompt);
    RUN(t_float_hook);
    RUN(t_clipboard);
    at_quit();
    return pc_test_finish();
}
