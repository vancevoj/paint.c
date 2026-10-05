/* test_p_panels.c - lane P: the History, Layers and Tools windows and the
 * Layer Properties dialog (WINDOWS.md 4, 5, 6; OBSERVED 4.1), driven like a
 * user on a headless app. */
#include "pc_test.h"
#include "p_test_util.h"

static bool inside(ui_rect outer, ui_rect r)
{
    return !ui_rect_empty(r) && r.x >= outer.x && r.y >= outer.y &&
           r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h;
}

static size_t hist_n(app_doc *d) { return app_doc_history_list(d, NULL, 0, NULL); }

static size_t hist_cur(app_doc *d)
{
    size_t cur = 0;
    (void)app_doc_history_list(d, NULL, 0, &cur);
    return cur;
}

/* Click history row i (rows are 28 DIPs below row 0). */
static void click_hist_row(app *a, int i)
{
    ui_rect r0 = pnl_rect(a, "history.row0");
    pt_click(a, (float)(r0.x + 60), (float)(r0.y + r0.h / 2 + i * r0.h), SDL_BUTTON_LEFT, 1);
}

/* W-HIST-LIST, STATE, CLICK, BUTTONS, TRUNCATE, SCROLL */
static void t_history(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = pt_new_doc(a, 200, 150);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    for (int i = 0; i < 3; i++) CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 2);
    CHECK(hist_n(d) == 4u && hist_cur(d) == 3u && d->doc->n_layers == 4u);
    CHECK(strcmp(d->hist->root->label, "New Image") == 0);
    /* a click moves to the state right after that entry */
    click_hist_row(a, 1);
    CHECK(hist_cur(d) == 1u && d->doc->n_layers == 2u && hist_n(d) == 4u);
    /* clicking the current entry toggles to the previous state and back */
    click_hist_row(a, 1);
    CHECK(hist_cur(d) == 0u && d->doc->n_layers == 1u);
    click_hist_row(a, 1);
    CHECK(hist_cur(d) == 1u && d->doc->n_layers == 2u);
    /* the first entry cannot be undone past */
    click_hist_row(a, 0);
    CHECK(hist_cur(d) == 0u);
    click_hist_row(a, 0);
    CHECK(hist_cur(d) == 0u && d->doc->n_layers == 1u);
    /* buttons: fast-forward, rewind, undo, redo */
    CHECK(pt_click_rect(a, "history.forward", SDL_BUTTON_LEFT));
    CHECK(hist_cur(d) == 3u && d->doc->n_layers == 4u);
    CHECK(pt_click_rect(a, "history.rewind", SDL_BUTTON_LEFT));
    CHECK(hist_cur(d) == 0u);
    CHECK(pt_click_rect(a, "history.redo", SDL_BUTTON_LEFT));
    CHECK(hist_cur(d) == 1u);
    CHECK(pt_click_rect(a, "history.redo", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "history.undo", SDL_BUTTON_LEFT));
    CHECK(hist_cur(d) == 1u);
    /* a new action discards the undone entries */
    CHECK(app_cmd_exec(a, "layers.duplicate"));
    at_frames(a, 2);
    CHECK(hist_n(d) == 3u && hist_cur(d) == 2u);
    CHECK(strcmp(d->hist->cur->label, "Duplicate Layer") == 0);
    /* many entries: the current one stays visible (W-HIST-SCROLL) */
    for (int i = 0; i < 40; i++) {
        CHECK(app_cmd_exec(a, i % 2 ? "edit.deselect" : "edit.select_all"));
    }
    at_frames(a, 3);
    CHECK(inside(pnl_rect(a, "history.list"), pnl_rect(a, "history.cur")));
    CHECK(pt_click_rect(a, "history.rewind", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(inside(pnl_rect(a, "history.list"), pnl_rect(a, "history.cur")));
    CHECK(app_cmd_exec(a, "edit.redo") && app_cmd_exec(a, "edit.redo"));
    CHECK(pt_click_rect(a, "history.forward", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(hist_cur(d) == hist_n(d) - 1u);
    CHECK(inside(pnl_rect(a, "history.list"), pnl_rect(a, "history.cur")));
    /* wheel scrolling moves the rows */
    {
        ui_rect l = pnl_rect(a, "history.list"), r0 = pnl_rect(a, "history.row0");
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = 3.0f;
        e.wheel.mouse_x = pt_cx(l);
        e.wheel.mouse_y = pt_cy(l);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(l), pt_cy(l), 0);
        at_frames(a, 1);
        app_event(a, &e);
        at_frames(a, 2);
        CHECK(pnl_rect(a, "history.row0").y > r0.y || ui_rect_empty(r0));
    }
    app_destroy(a);
}

/* Per-action icons (W-HIST-LIST). */
static void t_history_icons(void)
{
    app *a = pt_app(800, 600, NULL, false);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pnl_history_icon(a, "New Image") == UI_ICON_NEW);
    CHECK(pnl_history_icon(a, "Open Image") == UI_ICON_OPEN);
    CHECK(pnl_history_icon(a, "Add New Layer") == UI_ICON_LAYER_ADD);
    CHECK(pnl_history_icon(a, "Merge Layer Down") == UI_ICON_LAYER_MERGE);
    CHECK(pnl_history_icon(a, "Hide Layer") == UI_ICON_EYE_OFF);
    CHECK(pnl_history_icon(a, "Layer Properties") == UI_ICON_LAYER_PROPERTIES);
    CHECK(pnl_history_icon(a, "Pencil") == UI_ICON_TOOL_PENCIL);
    CHECK(pnl_history_icon(a, "Paintbrush") == UI_ICON_TOOL_PAINTBRUSH);
    CHECK(pnl_history_icon(a, "Select All") == UI_ICON_SELECT_ALL);
    CHECK(pnl_history_icon(a, "Invert Colors") == UI_ICON_ADJUSTMENTS);
    CHECK(pnl_history_icon(a, "Gaussian Blur") == UI_ICON_EFFECTS);
    CHECK(pnl_history_icon(a, "Something Else") == UI_ICON_EFFECTS);
    CHECK(pnl_history_icon(a, "Pencil") == UI_ICON_TOOL_PENCIL);     /* cached */
    app_destroy(a);
}

static uint32_t top_id(app_doc *d) { return d->doc->stack[d->doc->n_layers - 1u]->id; }

/* W-LAY-ROWS, ACTIVE, VIS, DBL, DRAG, BUTTONS, SCROLL */
static void t_layers(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    ui_rect r0;
    int32_t rh;
    CHECK(a != NULL);
    if (!a) return;
    d = pt_new_doc(a, 200, 150);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    /* with one layer Delete, Merge Down, Move Up / Down are disabled */
    CHECK(!app_cmd_enabled(a, "layers.delete") && !app_cmd_enabled(a, "layers.merge_down"));
    CHECK(pt_click_rect(a, "layers.delete", SDL_BUTTON_LEFT));
    CHECK(d->doc->n_layers == 1u);
    /* Add New Layer button, three times */
    for (int i = 0; i < 3; i++) CHECK(pt_click_rect(a, "layers.add", SDL_BUTTON_LEFT));
    CHECK(d->doc->n_layers == 4u && app_doc_layer_index(d) == 3);
    r0 = pnl_rect(a, "layers.row0");
    rh = r0.h;
    CHECK(rh == ui_px(a->ui, 44.0f));
    /* a click activates a row (top row = top layer); no history step */
    {
        size_t h0 = hist_n(d);
        pt_click(a, (float)(r0.x + 120), (float)(r0.y + 2 * rh + rh / 2), SDL_BUTTON_LEFT, 1);
        CHECK(app_doc_layer_index(d) == 1);
        CHECK(hist_n(d) == h0);
        CHECK(inside(r0.w ? pnl_rect(a, "layers.list") : r0, pnl_rect(a, "layers.active")));
    }
    /* the check box hides the layer: one history step, still active */
    {
        ui_rect cb = pnl_rect(a, "layers.check0");
        uint32_t id = top_id(d);
        CHECK(!ui_rect_empty(cb));
        pt_click(a, pt_cx(cb), pt_cy(cb), SDL_BUTTON_LEFT, 1);
        CHECK(!pc_doc_layer_by_id(d->doc, id)->visible);
        CHECK(strcmp(d->hist->cur->label, "Hide Layer") == 0);
        CHECK(app_doc_layer_index(d) == 1);          /* the active layer did not change */
        pt_click(a, pt_cx(cb), pt_cy(cb), SDL_BUTTON_LEFT, 1);
        CHECK(pc_doc_layer_by_id(d->doc, id)->visible);
        CHECK(strcmp(d->hist->cur->label, "Show Layer") == 0);
        CHECK(app_cmd_exec(a, "edit.undo") && !pc_doc_layer_by_id(d->doc, id)->visible);
        CHECK(app_cmd_exec(a, "edit.undo") && pc_doc_layer_by_id(d->doc, id)->visible);
        /* hiding the active layer keeps it active (W-LAY-VIS) */
        pt_click(a, (float)(r0.x + 120), (float)(r0.y + rh / 2), SDL_BUTTON_LEFT, 1);
        pt_click(a, pt_cx(cb), pt_cy(cb), SDL_BUTTON_LEFT, 1);
        CHECK(app_doc_layer_index(d) == 3 && !app_doc_layer(d)->visible);
        CHECK(app_cmd_exec(a, "edit.undo"));
    }
    /* drag the top row to the bottom: one "Move Layer" step, still active */
    {
        uint32_t id = top_id(d);
        size_t h0 = hist_cur(d);
        float x = (float)(r0.x + 120);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, (float)(r0.y + rh / 2), 0);
        at_frames(a, 2);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, (float)(r0.y + rh / 2), SDL_BUTTON_LEFT, 1);
        at_frames(a, 1);
        for (int k = 1; k <= 8; k++) {
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, (float)(r0.y + rh / 2 + k * rh / 2), 0);
            at_frames(a, 1);
        }
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, (float)(r0.y + 4 * rh - 2), 0);
        at_frames(a, 1);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, x, (float)(r0.y + 4 * rh - 2), SDL_BUTTON_LEFT, 1);
        at_frames(a, 2);
        CHECK(d->doc->stack[0]->id == id);
        CHECK(hist_cur(d) == h0 + 1u && hist_n(d) == h0 + 2u);   /* the redo entry is gone */
        CHECK(strcmp(d->hist->cur->label, "Move Layer") == 0);
        CHECK(d->layer_id == id);
        CHECK(app_cmd_exec(a, "edit.undo") && top_id(d) == id);
    }
    /* Move Up / Down buttons; Ctrl+click moves to the top / bottom */
    {
        uint32_t id;
        pt_click(a, (float)(r0.x + 120), (float)(r0.y + 2 * rh + rh / 2), SDL_BUTTON_LEFT, 1);
        id = d->layer_id;
        CHECK(app_doc_layer_index(d) == 1);
        CHECK(pt_click_rect(a, "layers.up", SDL_BUTTON_LEFT));
        CHECK(app_doc_layer_index(d) == 2 && d->layer_id == id);
        pt_mod(a, SDLK_LCTRL, SDL_KMOD_LCTRL, true);
        {
            ui_rect b = pnl_rect(a, "layers.down");
            pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, pt_cx(b), pt_cy(b), SDL_BUTTON_LEFT, 1);
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(b), pt_cy(b), 0);
            at_frames(a, 2);
            pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, pt_cx(b), pt_cy(b), SDL_BUTTON_LEFT, 1);
            at_frames(a, 2);
        }
        pt_mod(a, SDLK_LCTRL, SDL_KMOD_LCTRL, false);
        CHECK(app_doc_layer_index(d) == 0 && d->layer_id == id);
        CHECK(!app_cmd_enabled(a, "layers.move_down") && !app_cmd_enabled(a, "layers.merge_down"));
        CHECK(pt_click_rect(a, "layers.duplicate", SDL_BUTTON_LEFT));
        CHECK(d->doc->n_layers == 5u && app_doc_layer_index(d) == 1);
        CHECK(pt_click_rect(a, "layers.merge", SDL_BUTTON_LEFT));
        CHECK(d->doc->n_layers == 4u && app_doc_layer_index(d) == 0);
    }
    /* a double click opens Layer Properties (W-LAY-DBL) */
    pt_double_click(a, (float)(r0.x + 120), (float)(r0.y + rh / 2));
    at_frames(a, 2);
    CHECK(app_dialog_active(a));
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    /* the Properties button too */
    CHECK(pt_click_rect(a, "layers.props", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(app_dialog_active(a));
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    /* many layers: the active one stays in view (W-LAY-SCROLL) */
    for (int i = 0; i < 20; i++) CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 3);
    CHECK(inside(pnl_rect(a, "layers.list"), pnl_rect(a, "layers.active")));
    CHECK(app_cmd_exec(a, "layers.go_bottom"));
    at_frames(a, 3);
    CHECK(inside(pnl_rect(a, "layers.list"), pnl_rect(a, "layers.active")));
    CHECK(app_cmd_exec(a, "layers.go_top"));
    at_frames(a, 3);
    CHECK(inside(pnl_rect(a, "layers.list"), pnl_rect(a, "layers.active")));
    app_destroy(a);
}

/* Layer Properties (OBSERVED 4.1): control order, live preview, Cancel
 * restores, OK is one history step, empty names keep the old one. */
static void t_layer_props(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    pc_layer *l;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = pt_new_doc(a, 200, 150);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, "layers.add_new"));
    l = app_doc_layer(d);
    h0 = hist_n(d);
    pt_key(a, SDLK_F4, SDL_KMOD_NONE);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    {
        ui_rect n = pnl_rect(a, "layerprops.name"), o = pnl_rect(a, "layerprops.opacity");
        ui_rect m = pnl_rect(a, "layerprops.mode"), v = pnl_rect(a, "layerprops.visible");
        CHECK(n.y < o.y && o.y < m.y && m.y < v.y);   /* Name, Opacity, Blend Mode, Visible */
    }
    /* the name is focused with its text selected */
    pt_text(a, "Sky");
    /* live preview of the opacity */
    CHECK(pt_click_rect(a, "layerprops.opacity", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "100");
    pt_key(a, SDLK_TAB, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(l->opacity == 100);
    /* Cancel restores everything, no history */
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    CHECK(l->opacity == 255 && strcmp(l->name, "Layer 2") == 0 && hist_n(d) == h0);
    /* OK: name, opacity, mode and visibility in one step */
    CHECK(app_cmd_exec(a, "layers.properties"));
    at_frames(a, 3);
    pt_text(a, "Sky");
    CHECK(pt_click_rect(a, "layerprops.opacity", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "128");
    pt_key(a, SDLK_TAB, SDL_KMOD_NONE);              /* -> the blend mode box */
    pt_key(a, SDLK_DOWN, SDL_KMOD_NONE);             /* Normal -> Multiply */
    at_frames(a, 2);
    CHECK(l->mode == PC_BLEND_MULTIPLY);
    CHECK(pt_click_rect(a, "layerprops.visible", SDL_BUTTON_LEFT));
    CHECK(!l->visible);
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    CHECK(hist_n(d) == h0 + 1u && strcmp(d->hist->cur->label, "Layer Properties") == 0);
    CHECK(strcmp(l->name, "Sky") == 0 && l->opacity == 128 && l->mode == PC_BLEND_MULTIPLY &&
          !l->visible);
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(strcmp(l->name, "Layer 2") == 0 && l->opacity == 255 && l->mode == PC_BLEND_NORMAL &&
          l->visible);
    /* OK without changes: no step; an empty name keeps the old one */
    CHECK(app_cmd_exec(a, "layers.properties"));
    at_frames(a, 3);
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(hist_cur(d) == h0 - 1u);
    CHECK(app_cmd_exec(a, "layers.properties"));
    at_frames(a, 3);
    pt_key(a, SDLK_DELETE, SDL_KMOD_NONE);
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(strcmp(l->name, "Layer 2") == 0 && hist_cur(d) == h0 - 1u);
    app_destroy(a);
}

/* W-TOOLS-GRID: the grid selects tools; unregistered slots are inert. */
static void t_tools(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    ui_rect r1, r2;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pt_new_doc(a, 200, 150) != NULL);
    CHECK(pt_click_rect(a, "tool.pencil", SDL_BUTTON_LEFT));
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pencil") == 0);
    CHECK(pt_click_rect(a, "tool.paintbrush", SDL_BUTTON_LEFT));
    CHECK(strcmp(app_tool_current(a)->id, "paintbrush") == 0);
    /* 2 columns in TOOLS.md order */
    r1 = pnl_rect(a, "tool.rect_select");
    r2 = pnl_rect(a, "tool.move_pixels");
    CHECK(r1.y == r2.y && r2.x > r1.x);
    r2 = pnl_rect(a, "tool.lasso_select");
    CHECK(r2.x == r1.x && r2.y > r1.y);
    r2 = pnl_rect(a, "tool.shapes");
    CHECK(r2.x == r1.x && !ui_rect_empty(r2));
    if (!app_tool_find(a, "text")) {
        CHECK(pt_click_rect(a, "tool.text", SDL_BUTTON_LEFT));
        CHECK(strcmp(app_tool_current(a)->id, "paintbrush") == 0);
    }
    /* F5 hides the window; tool keys keep working (W-TOOLS-TOGGLE) */
    pt_key(a, SDLK_F5, SDL_KMOD_NONE);
    CHECK(!app_panel_open(a, "tools"));
    pt_key(a, SDLK_P, SDL_KMOD_NONE);
    CHECK(strcmp(app_tool_current(a)->id, "pencil") == 0);
    pt_key(a, SDLK_F5, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    CHECK(app_panel_open(a, "tools"));
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
    RUN(t_history);
    RUN(t_history_icons);
    RUN(t_layers);
    RUN(t_layer_props);
    RUN(t_tools);
    at_quit();
    return pc_test_finish();
}
