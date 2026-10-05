/* pnl_imagelist.c - the image list in the top row (lane P, WINDOWS.md 2).
 *
 *   W-IMG-THUMB    one live thumbnail per open image (thumbs.c), no label;
 *                  the active one highlighted
 *   W-IMG-DIRTY    a small orange asterisk at the thumbnail's top left
 *   W-IMG-CLOSE    a red X at the top right (active or hovered thumbnail,
 *                  non-active ones too); middle click closes
 *   W-IMG-REORDER  drag a thumbnail to reorder (drop indicator)
 *   W-IMG-SCROLL   when the thumbnails overflow, arrows at both sides
 *                  (press and hold repeats); wheel and horizontal wheel
 *                  scroll the strip; the active image is kept in view
 *   W-IMG-LIST     the down arrow at the right lists every image with its
 *                  thumbnail and file name; a click switches
 *   W-IMG-CTX      right click (or Alt+Minus for the active image): Copy
 *                  Path, Open Containing Folder, Save, Save As..., Close;
 *                  the first two are disabled for never saved images
 * Closing goes through app_close_doc (unsaved changes prompt). Main thread. */
#include "pnl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct imglist_state {
    float    scroll;
    int32_t  drag_from;          /* tab being dragged, -1 */
    int32_t  press_tab;
    uint32_t last_active_id;     /* keep the active tab in view when it changes */
    int32_t  last_n;
    uint32_t ctx_doc;            /* document of the context menu */
    bool     ctx_pending;        /* open it in the next frame (Alt+Minus) */
} imglist_state;

static imglist_state *istate(app *a)
{
    imglist_state *s = (imglist_state *)app_ext_get(a, "pnl.imagelist");
    if (s) return s;
    s = (imglist_state *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    s->drag_from = -1;
    s->press_tab = -1;
    if (!app_ext_set(a, "pnl.imagelist", s, free)) {
        free(s);
        return NULL;
    }
    return s;
}

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == id) return a->docs[i];
    return NULL;
}

void pnl_image_list_context(app *a, int32_t i)
{
    imglist_state *s = istate(a);
    app_doc *d = app_doc_at(a, i);
    if (!s || !d) return;
    s->ctx_doc = d->id;
    s->ctx_pending = true;
    app_request_frame(a);
}

/* Thumbnail aspect fitted into box over a checkerboard. */
static void draw_doc_thumb(app *a, const app_doc *d, ui_rect box)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_rect img = box;
    uint32_t w = d->doc->w, h = d->doc->h;
    if (w > 0u && h > 0u) {
        float s = (float)box.w / (float)w < (float)box.h / (float)h ? (float)box.w / (float)w
                                                                     : (float)box.h / (float)h;
        int32_t iw = (int32_t)((float)w * s + 0.5f), ih = (int32_t)((float)h * s + 0.5f);
        img = ui_rect_center(box, iw > 0 ? iw : 1, ih > 0 ? ih : 1);
    }
    ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
    if (d->thumb)
        ui_draw_image(ui, d->thumb, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
    ui_draw_rect_outline(ui, ui_rect_inset(img, -1, -1), 1, ui_color_fade(p->border_strong, 0.8f));
}

/* The unsaved-changes asterisk (W-IMG-DIRTY): six spokes in orange with a
 * thin backing so it reads on any thumbnail. */
static void asterisk(app *a, float cx, float cy)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float r = (float)ui_px(ui, 4.5f), w = (float)ui_px(ui, 2.0f) > 1.0f ? (float)ui_px(ui, 2.0f)
                                                                          : 1.0f;
    ui_draw_circle(ui, ui_vec2_make(cx, cy), r + (float)ui_px(ui, 1.5f),
                   ui_color_fade(p->panel, 0.85f));
    for (int k = 0; k < 3; k++) {
        float ang = (float)k * (UI_PI / 3.0f) + UI_PI / 2.0f;
        float dx = cosf(ang) * r, dy = sinf(ang) * r;
        ui_draw_line(ui, ui_vec2_make(cx - dx, cy - dy), ui_vec2_make(cx + dx, cy + dy), w,
                     p->modified);
    }
}

static void context_menu(app *a, imglist_state *s)
{
    ui_ctx *ui = a->ui;
    app_doc *d;
    if (!ui_popup_begin(ui, "##doc_ctx")) return;
    d = doc_by_id(a, s->ctx_doc);
    if (d) {
        bool has_path = d->path != NULL;
        if (ui_menu_item(ui, "Copy Path", NULL, has_path)) (void)pal_clip_set_text(d->path);
        pnl_rect_set(a, "imgctx.copy", ui_last_rect(ui));
        if (ui_menu_item(ui, "Open Containing Folder", NULL, has_path))
            (void)pal_reveal_file(d->path);
        pnl_rect_set(a, "imgctx.folder", ui_last_rect(ui));
        ui_menu_separator(ui);
        if (ui_menu_item_icon(ui, UI_ICON_SAVE, "Save", NULL, true))
            app_save_doc(a, d, false, NULL, NULL);
        pnl_rect_set(a, "imgctx.save", ui_last_rect(ui));
        if (ui_menu_item_icon(ui, UI_ICON_SAVE_AS, "Save As...", NULL, true))
            app_save_doc(a, d, true, NULL, NULL);
        pnl_rect_set(a, "imgctx.saveas", ui_last_rect(ui));
        ui_menu_separator(ui);
        if (ui_menu_item_icon(ui, UI_ICON_CLOSE, "Close", NULL, true))
            app_close_doc(a, d, NULL, NULL);
        pnl_rect_set(a, "imgctx.close", ui_last_rect(ui));
    } else {
        ui_popup_close(ui);
    }
    ui_popup_end(ui);
}

/* The list of every image (W-IMG-LIST): thumbnail and file name rows. */
static void list_popup(app *a)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t rowh = ui_px(ui, 44.0f), w = ui_px(ui, 300.0f), maxh = ui_px(ui, 440.0f);
    ui_size cell = ui_size_px(300.0f);
    ui_rect area;
    if (!ui_popup_begin(ui, "##image_list")) return;
    /* a fixed width cell makes the popup measure itself 300 DIPs wide */
    ui_layout_row(ui, 0.0f, 1, &cell);
    area = ui_layout_next(ui, w, a->ndocs * rowh < maxh ? a->ndocs * rowh : maxh);
    ui_layout_column(ui);
    ui_scroll_begin(ui, "##image_list_scroll", area, UI_SCROLL_NO_BG);
    ui_layout_set_spacing(ui, 0.0f);
    for (int32_t i = 0; i < a->ndocs; i++) {
        app_doc *d = a->docs[i];
        ui_rect r = ui_layout_next(ui, w, rowh);
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, 0x11570 + i), r, 0);
        ui_rect tb = ui_rect_make(r.x + ui_px(ui, 6.0f), r.y + ui_px(ui, 4.0f),
                                  (rowh - ui_px(ui, 8.0f)) * 4 / 3, rowh - ui_px(ui, 8.0f));
        char name[300];
        if (i == a->active) ui_draw_rrect(ui, ui_rect_inset(r, 2, 1), 4.0f, p->selection);
        else if (in.hovered) ui_draw_rrect(ui, ui_rect_inset(r, 2, 1), 4.0f, p->hover);
        draw_doc_thumb(a, d, tb);
        if (app_doc_dirty(d)) asterisk(a, (float)tb.x + 1.0f, (float)tb.y + 1.0f);
        snprintf(name, sizeof name, "%s", d->name);
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                         ui_rect_make(tb.x + tb.w + ui_px(ui, 10.0f), r.y,
                                      r.x + r.w - (tb.x + tb.w + ui_px(ui, 16.0f)), r.h),
                         UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, name, strlen(name));
        if (d->path) ui_tooltip(ui, d->path);
        if (i == 0) pnl_rect_set(a, "imagelist.popup_row0", r);
        if (in.clicked) {
            app_set_active_doc(a, d);
            ui_popup_close(ui);
        }
    }
    ui_scroll_end(ui);
    ui_popup_end(ui);
}

/* Move document from -> to keeping the active one active. */
static void move_doc(app *a, int32_t from, int32_t to)
{
    app_doc *cur = app_active_doc(a), *m;
    if (from < 0 || from >= a->ndocs || to < 0 || to >= a->ndocs || from == to) return;
    m = a->docs[from];
    if (from < to)
        memmove(&a->docs[from], &a->docs[from + 1], (size_t)(to - from) * sizeof *a->docs);
    else
        memmove(&a->docs[to + 1], &a->docs[to], (size_t)(from - to) * sizeof *a->docs);
    a->docs[to] = m;
    a->active = app_doc_index(a, cur);
    app_request_frame(a);
}

static bool arrow_button(app *a, const char *id, ui_rect r, ui_icon icon, const char *rect_name)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, id), r, UI_INTERACT_REPEAT);
    float rad = ui_get_theme(ui)->m.radius * ui_scale(ui);
    if (in.held) ui_draw_rrect(ui, r, rad, p->raised_active);
    else if (in.hovered) ui_draw_rrect(ui, r, rad, p->hover);
    ui_draw_icon(ui, icon, r, ui_px(ui, 14.0f), p->icon, p->icon_accent);
    pnl_rect_set(a, rect_name, r);
    return in.clicked;
}

void pnl_image_list(app *a, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    imglist_state *s = istate(a);
    int32_t n = a->ndocs, vpad = ui_px(ui, 3.0f), gap = ui_px(ui, 4.0f), th, tw, total;
    int32_t ctrl = ui_px(ui, 22.0f);
    float rad = ui_get_theme(ui)->m.radius * ui_scale(ui), scroll;
    ui_rect strip, listb;
    bool overflow;
    if (!s) return;
    th = r.h - 2 * vpad;
    tw = th * 4 / 3 + ui_px(ui, 6.0f);
    total = n * (tw + gap) - (n > 0 ? gap : 0);
    /* the list button is always at the right end */
    /* the list button follows the last thumbnail, or sits at the right end
     * when the thumbnails overflow */
    strip = ui_rect_make(r.x, r.y, r.w - ctrl - gap, r.h);
    overflow = total > strip.w;
    if (!overflow) strip.w = total > 0 ? total : 0;
    listb = ui_rect_make(strip.x + strip.w + gap, r.y + (r.h - ctrl) / 2, ctrl, ctrl);
    if (overflow) {
        ui_rect lb = ui_rect_make(strip.x, r.y + (r.h - ctrl) / 2, ctrl, ctrl);
        ui_rect rb = ui_rect_make(strip.x + strip.w - ctrl, lb.y, ctrl, ctrl);
        strip.x += ctrl + gap;
        strip.w -= 2 * (ctrl + gap);
        if (arrow_button(a, "##img_left", lb, UI_ICON_CHEVRON_LEFT, "imagelist.left"))
            s->scroll -= (float)(tw + gap);
        if (arrow_button(a, "##img_right", rb, UI_ICON_CHEVRON_RIGHT, "imagelist.right"))
            s->scroll += (float)(tw + gap);
    }
    pnl_rect_set(a, "imagelist.strip", strip);
    scroll = s->scroll;
    {
        ui_vec2 w = ui_wheel_take(ui, r);
        if (w.y != 0.0f || w.x != 0.0f) scroll -= (w.y + w.x) * (float)(tw / 2);
    }
    /* keep the active image in view when it changed (keyboard, new, open) */
    if (a->active >= 0 && (a->docs[a->active]->id != s->last_active_id || n != s->last_n)) {
        float x0 = (float)(a->active * (tw + gap));
        if (x0 < scroll) scroll = x0;
        if (x0 + (float)tw > scroll + (float)strip.w) scroll = x0 + (float)tw - (float)strip.w;
    }
    if (!ui_mouse_down(ui, UI_MOUSE_LEFT)) s->press_tab = -1;
    {
        float mx = (float)(total - strip.w);
        if (scroll > mx) scroll = mx;
        if (scroll < 0.0f) scroll = 0.0f;
    }
    ui_push_clip(ui, strip);
    for (int32_t i = 0; i < n; i++) {
        app_doc *d = a->docs[i];
        ui_rect tr = ui_rect_make(strip.x + i * (tw + gap) - (int32_t)scroll, r.y + vpad, tw, th);
        ui_interaction in;
        bool sel = i == a->active, inside;
        ui_rect thumb = ui_rect_inset(tr, ui_px(ui, 4.0f), ui_px(ui, 4.0f));
        ui_rect cr = ui_rect_make(tr.x + tr.w - ui_px(ui, 15.0f), tr.y + ui_px(ui, 1.0f),
                                  ui_px(ui, 14.0f), ui_px(ui, 14.0f));
        bool vis = tr.x + tr.w >= strip.x && tr.x <= strip.x + strip.w;
        if (!vis && s->drag_from != i) continue;
        in = ui_interact(ui, ui_get_id_int(ui, 0x1A6E0 + i), tr, UI_INTERACT_OVERLAP);
        inside = ui_rect_contains(tr, ui_mouse_pos(ui).x, ui_mouse_pos(ui).y);
        if (in.pressed) {
            s->press_tab = i;
            if (!sel) {
                app_set_active_doc(a, d);
                sel = true;
            }
        }
        if (in.dragging && s->drag_from < 0 && s->press_tab == i) s->drag_from = i;
        if (in.middle_clicked) {
            ui_pop_clip(ui);
            app_close_doc(a, d, NULL, NULL);
            return;                        /* the list changed; draw it next frame */
        }
        if (in.right_clicked) {
            s->ctx_doc = d->id;
            ui_popup_open(ui, "##doc_ctx", ui_rect_make(0, 0, 0, 0), UI_POPUP_AT);
        }
        if (!vis) continue;
        if (sel) {
            pnl_rect_set(a, "imagelist.active", tr);
            ui_draw_rrect(ui, tr, rad, p->selection);
            ui_draw_rrect_outline(ui, tr, rad, ui_px_line(ui, 2.0f), p->accent);
        } else if (in.hovered || inside) {
            ui_draw_rrect(ui, tr, rad, p->hover);
        }
        draw_doc_thumb(a, d, thumb);
        if (app_doc_dirty(d)) asterisk(a, (float)thumb.x + 2.0f, (float)thumb.y + 2.0f);
        if (i < 4) {
            static const char *const tab_names[4] = { "imagelist.tab0", "imagelist.tab1",
                                                      "imagelist.tab2", "imagelist.tab3" };
            static const char *const close_names[4] = { "imagelist.close0", "imagelist.close1",
                                                        "imagelist.close2", "imagelist.close3" };
            pnl_rect_set(a, tab_names[i], tr);
            pnl_rect_set(a, close_names[i], cr);
        }
        {
            char tip[1200];
            if (d->path) snprintf(tip, sizeof tip, "%s\n%s", d->name, d->path);
            else snprintf(tip, sizeof tip, "%s", d->name);
            ui_tooltip(ui, tip);
        }
        /* close button: the active one and the one under the pointer */
        if ((sel || inside) && s->drag_from < 0) {
            ui_interaction ci = ui_interact(ui, ui_get_id_int(ui, 0x1C105 + i), cr, 0);
            ui_vec2 c = ui_vec2_make((float)cr.x + (float)cr.w * 0.5f,
                                     (float)cr.y + (float)cr.h * 0.5f);
            ui_draw_circle(ui, c, (float)cr.w * 0.5f,
                           ci.hovered ? p->danger : ui_color_fade(p->panel, 0.92f));
            ui_draw_icon(ui, UI_ICON_CLOSE, cr, ui_px(ui, 10.0f),
                         ci.hovered ? ui_rgba(255, 255, 255, 255) : p->danger, p->danger);
            ui_tooltip(ui, "Close");
            if (ci.clicked) {
                ui_pop_clip(ui);
                app_close_doc(a, d, NULL, NULL);
                return;
            }
        }
    }
    /* drag reordering */
    if (s->drag_from >= 0) {
        float mx = ui_mouse_pos(ui).x;
        int32_t slot = (int32_t)floorf((mx - (float)strip.x + scroll) / (float)(tw + gap) + 0.5f);
        if (slot < 0) slot = 0;
        if (slot > n) slot = n;
        ui_draw_rect(ui, ui_rect_make(strip.x + slot * (tw + gap) - (int32_t)scroll - gap / 2 - 1,
                                      r.y + vpad, ui_px_line(ui, 2.0f), th),
                     p->accent);
        ui_set_cursor(ui, UI_CURSOR_MOVE);
        if (mx < (float)strip.x + (float)tw * 0.3f) scroll -= (float)tw * 0.15f;
        if (mx > (float)(strip.x + strip.w) - (float)tw * 0.3f) scroll += (float)tw * 0.15f;
        if (!ui_mouse_down(ui, UI_MOUSE_LEFT)) {
            int32_t from = s->drag_from, to = slot > from ? slot - 1 : slot;
            s->drag_from = -1;
            move_doc(a, from, to);
        } else {
            app_request_frame_at(a, a->now + 16u);
        }
    }
    ui_pop_clip(ui);
    {
        float mx = (float)(total - strip.w);
        if (scroll > mx) scroll = mx;
        if (scroll < 0.0f) scroll = 0.0f;
    }
    if (scroll != s->scroll) app_request_frame(a);
    s->scroll = scroll;
    s->last_active_id = a->active >= 0 ? a->docs[a->active]->id : 0u;
    s->last_n = n;
    /* the list of all images */
    {
        ui_interaction in = ui_interact(ui, ui_get_id(ui, "##image_list_btn"), listb, 0);
        if (in.held || ui_popup_is_open(ui, "##image_list"))
            ui_draw_rrect(ui, listb, rad, p->raised_active);
        else if (in.hovered) ui_draw_rrect(ui, listb, rad, p->hover);
        ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN, listb, ui_px(ui, 14.0f), p->icon, p->icon_accent);
        ui_tooltip(ui, "Open images");
        pnl_rect_set(a, "imagelist.list", listb);
        if (in.clicked && n > 0) ui_popup_open(ui, "##image_list", listb, UI_POPUP_BELOW);
    }
    list_popup(a);
    /* Alt+Minus: the context menu of the active image under its thumbnail */
    if (s->ctx_pending) {
        int32_t i = app_doc_index(a, doc_by_id(a, s->ctx_doc));
        s->ctx_pending = false;
        if (i >= 0) {
            ui_rect tr =
                ui_rect_make(strip.x + i * (tw + gap) - (int32_t)scroll, r.y + vpad, tw, th);
            if (tr.x < strip.x) tr.x = strip.x;
            ui_popup_open(ui, "##doc_ctx", tr, UI_POPUP_BELOW);
        }
    }
    context_menu(a, s);
}
