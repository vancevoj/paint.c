/* menu.c - the menu bar, built every frame from a declarative table of
 * command ids that mirrors docs/inventory/MENUS.md (order and separators).
 * Items whose command is not registered show disabled (with their
 * documented shortcut), so feature lanes only register commands. The
 * Adjustments and Effects menus are generated from the effect registry
 * (menu paths), Open Recent from the recent file list. */
#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MI_END = 0,
    MI_CMD,          /* command item */
    MI_OPT,          /* command item hidden while the command is not registered */
    MI_SEP,          /* separator */
    MI_SUB,          /* submenu: label; items follow until MI_SUB_END */
    MI_SUB_OPT,      /* submenu hidden when its first item is not registered */
    MI_SUB_END,
    MI_RECENT,       /* Open Recent submenu */
    MI_ADJUST,       /* every adjustment from the effect registry */
    MI_EFFECTS       /* Repeat, Plugin Errors, effect submenus */
};

typedef struct menu_item {
    int         kind;
    const char *id;
    const char *label;
} menu_item;

typedef struct menu_def {
    const char      *title;
    const menu_item *items;
} menu_def;

/* ---- the table (MENUS.md) --------------------------------------------------------- */
static const menu_item k_file[] = {
    { MI_CMD, "file.new", "New..." },
    { MI_CMD, "file.open", "Open..." },
    { MI_RECENT, NULL, "Open Recent" },
    { MI_SUB_OPT, NULL, "Acquire" },
    { MI_CMD, "file.acquire.scanner", "From Scanner or Camera..." },
    { MI_SUB_END, NULL, NULL },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "file.save", "Save" },
    { MI_CMD, "file.save_as", "Save As..." },
    { MI_CMD, "file.save_all", "Save All" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "file.print", "Print..." },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "file.close", "Close" },
    { MI_CMD, "file.exit", "Exit" },
    { MI_END, NULL, NULL }
};

static const menu_item k_edit[] = {
    { MI_CMD, "edit.undo", "Undo" },
    { MI_CMD, "edit.redo", "Redo" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "edit.cut", "Cut" },
    { MI_CMD, "edit.copy", "Copy" },
    { MI_CMD, "edit.copy_merged", "Copy Merged" },
    { MI_CMD, "edit.paste", "Paste" },
    { MI_CMD, "edit.paste_layer", "Paste into New Layer" },
    { MI_CMD, "edit.paste_image", "Paste into New Image" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "edit.copy_selection", "Copy Selection" },
    { MI_SUB, NULL, "Paste Selection" },
    { MI_CMD, "edit.paste_selection.replace", "Replace" },
    { MI_CMD, "edit.paste_selection.union", "Add (union)" },
    { MI_CMD, "edit.paste_selection.exclude", "Subtract" },
    { MI_CMD, "edit.paste_selection.intersect", "Intersect" },
    { MI_CMD, "edit.paste_selection.xor", "Invert (xor)" },
    { MI_SUB_END, NULL, NULL },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "edit.erase_selection", "Erase Selection" },
    { MI_CMD, "edit.fill_selection", "Fill Selection" },
    { MI_CMD, "edit.invert_selection", "Invert Selection" },
    { MI_CMD, "edit.select_all", "Select All" },
    { MI_CMD, "edit.deselect", "Deselect" },
    { MI_END, NULL, NULL }
};

static const menu_item k_view[] = {
    { MI_CMD, "view.zoom_in", "Zoom In" },
    { MI_CMD, "view.zoom_out", "Zoom Out" },
    { MI_CMD, "view.zoom_window", "Zoom to Window" },
    { MI_CMD, "view.zoom_selection", "Zoom to Selection" },
    { MI_CMD, "view.actual_size", "Actual Size" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "view.pixel_grid", "Pixel Grid" },
    { MI_CMD, "view.rulers", "Rulers" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "view.units.px", "Pixels" },
    { MI_CMD, "view.units.in", "Inches" },
    { MI_CMD, "view.units.cm", "Centimeters" },
    { MI_END, NULL, NULL }
};

static const menu_item k_image[] = {
    { MI_CMD, "image.crop_to_selection", "Crop to Selection" },
    { MI_CMD, "image.resize", "Resize..." },
    { MI_CMD, "image.canvas_size", "Canvas Size..." },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "image.flip_h", "Flip Horizontal" },
    { MI_CMD, "image.flip_v", "Flip Vertical" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "image.rotate_cw", "Rotate 90\xC2\xB0 Clockwise" },
    { MI_CMD, "image.rotate_ccw", "Rotate 90\xC2\xB0 Counter-clockwise" },
    { MI_CMD, "image.rotate_180", "Rotate 180\xC2\xB0" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "image.color_profile", "Color Profile..." },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "image.flatten", "Flatten" },
    { MI_END, NULL, NULL }
};

/* Separators after items 6, 10, 14 and 18 (MENUS.md Layers note). */
static const menu_item k_layers[] = {
    { MI_CMD, "layers.add_new", "Add New Layer" },
    { MI_CMD, "layers.delete", "Delete Layer" },
    { MI_CMD, "layers.duplicate", "Duplicate Layer" },
    { MI_CMD, "layers.merge_down", "Merge Layer Down" },
    { MI_CMD, "layers.toggle_visibility", "Toggle Layer Visibility" },
    { MI_CMD, "layers.import", "Import From File..." },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "layers.flip_h", "Flip Horizontal" },
    { MI_CMD, "layers.flip_v", "Flip Vertical" },
    { MI_CMD, "layers.rotate_180", "Rotate 180\xC2\xB0" },
    { MI_CMD, "layers.rotate_zoom", "Rotate / Zoom..." },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "layers.go_top", "Go to Top Layer" },
    { MI_CMD, "layers.go_up", "Go to Layer Above" },
    { MI_CMD, "layers.go_down", "Go to Layer Below" },
    { MI_CMD, "layers.go_bottom", "Go to Bottom Layer" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "layers.move_top", "Move Layer to Top" },
    { MI_CMD, "layers.move_up", "Move Layer Up" },
    { MI_CMD, "layers.move_down", "Move Layer Down" },
    { MI_CMD, "layers.move_bottom", "Move Layer to Bottom" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "layers.properties", "Layer Properties..." },
    { MI_END, NULL, NULL }
};

static const menu_item k_adjust[] = {
    { MI_ADJUST, NULL, NULL },
    { MI_END, NULL, NULL }
};

static const menu_item k_effects[] = {
    { MI_EFFECTS, NULL, NULL },
    { MI_END, NULL, NULL }
};

static const menu_def k_menus[] = {
    { "File", k_file },     { "Edit", k_edit },         { "View", k_view },
    { "Image", k_image },   { "Layers", k_layers },     { "Adjustments", k_adjust },
    { "Effects", k_effects }
};

/* The Help menu behind the "?" button on the right of the menu bar. */
static const menu_item k_help[] = {
    { MI_CMD, "help.docs", "Documentation" },
    { MI_CMD, "help.website", "Website" },
    { MI_CMD, "help.search", "Search" },
    { MI_OPT, "help.donate", "Donate" },
    { MI_OPT, "help.forum", "Forum" },
    { MI_OPT, "help.tutorials", "Tutorials" },
    { MI_OPT, "help.plugins", "Plugins" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "help.feedback", "Send Feedback or Bug Report" },
    { MI_SEP, NULL, NULL },
    { MI_CMD, "help.about", "About" },
    { MI_END, NULL, NULL }
};

/* ---- items --------------------------------------------------------------------------- */
static void cmd_item(app *a, const char *id, const char *label)
{
    ui_ctx *ui = a->ui;
    const app_cmd *c = app_cmd_find(a, id);
    bool en = c && app_cmd_enabled(a, id);
    char sc[64];
    const char *s = app_cmd_shortcut_text(a, id);
    bool chosen;
    if (s) app_copy_str(sc, sizeof sc, s);
    ui_push_id(ui, id);
    if (c && c->checked) {
        bool chk = c->checked(a, c);
        if (c->flags & APP_CMD_RADIO) chosen = ui_menu_radio(ui, label, s ? sc : NULL, chk, en);
        else chosen = ui_menu_check(ui, label, s ? sc : NULL, &chk, en);
    } else {
        chosen = ui_menu_item_icon(ui, c ? c->icon : UI_ICON_NONE, label, s ? sc : NULL, en);
    }
    ui_pop_id(ui);
    if (chosen) (void)app_cmd_exec(a, id);
}

static void recent_menu(app *a)
{
    ui_ctx *ui = a->ui;
    if (!ui_menu_begin(ui, "Open Recent")) return;
    app_recent_menu_items(a);           /* lane I (io/recent.c): thumbnails, tooltips */
    ui_menu_end(ui);
}

/* Segment k of an effect menu path into out ("" when missing). */
static uint32_t path_segs(const char *menu, char seg[4][128])
{
    const char *s[4];
    size_t n[4];
    uint32_t c = fx_menu_split(menu, s, n, 4u);
    for (uint32_t i = 0; i < 4u; i++) {
        size_t k = i < c ? n[i] : 0u;
        if (k >= 128u) k = 127u;
        if (k) memcpy(seg[i], s[i], k);
        seg[i][k] = '\0';
    }
    return c;
}

static void fx_label(const fx_effect *fx, const char *name, char *out, size_t cap)
{
    bool dialog = !(fx->flags & FX_FLAG_NO_DIALOG);
    snprintf(out, cap, "%s%s", name, dialog ? "..." : "");
}

static void adjust_items(app *a)
{
    uint32_t n = a->fx ? fx_registry_count(a->fx) : 0u;
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        char seg[4][128], label[200], id[200];
        uint32_t c = path_segs(fx->menu, seg);
        if (c < 2u || strcmp(seg[0], "Adjustments") != 0) continue;
        fx_label(fx, seg[c - 1u], label, sizeof label);
        snprintf(id, sizeof id, "adjust.%s", fx->id);
        cmd_item(a, id, label);
        any = true;
    }
    if (!any) ui_menu_item(a->ui, "No adjustments", NULL, false);
}

static void effect_items(app *a)
{
    ui_ctx *ui = a->ui;
    uint32_t n = a->fx ? fx_registry_count(a->fx) : 0u;
    char last_sub[128];
    const app_cmd *rep = app_cmd_find(a, "effects.repeat");
    last_sub[0] = '\0';
    if (rep && a->last_effect) {
        const fx_effect *last = fx_registry_find(a->fx, a->last_effect);
        char seg[4][128], label[200];
        uint32_t c = last ? path_segs(last->menu, seg) : 0u;
        if (c > 0u) snprintf(label, sizeof label, "Repeat %s", seg[c - 1u]);
        else app_copy_str(label, sizeof label, rep->label);
        cmd_item(a, "effects.repeat", label);
        ui_menu_separator(ui);
    }
    if (app_cmd_exists(a, "effects.plugin_errors") && app_cmd_enabled(a, "effects.plugin_errors")) {
        cmd_item(a, "effects.plugin_errors", "Plugin Errors...");
        ui_menu_separator(ui);
    }
    /* submenus in registry order (alphabetical), each listing its effects */
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        char seg[4][128];
        uint32_t c = path_segs(fx->menu, seg);
        if (c < 3u || strcmp(seg[0], "Effects") != 0 || strcmp(seg[1], last_sub) == 0) continue;
        app_copy_str(last_sub, sizeof last_sub, seg[1]);
        if (ui_menu_begin(ui, seg[1])) {
            for (uint32_t j = i; j < n; j++) {
                const fx_effect *e = fx_registry_at(a->fx, j);
                char s2[4][128], label[200], id[200];
                uint32_t c2 = path_segs(e->menu, s2);
                if (c2 < 3u || strcmp(s2[0], "Effects") != 0) continue;
                if (strcmp(s2[1], seg[1]) != 0) break;
                fx_label(e, s2[c2 - 1u], label, sizeof label);
                snprintf(id, sizeof id, "effects.%s", e->id);
                cmd_item(a, id, label);
            }
            ui_menu_end(ui);
        }
    }
    /* effects directly under Effects/ */
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        char seg[4][128], label[200], id[200];
        uint32_t c = path_segs(fx->menu, seg);
        if (c != 2u || strcmp(seg[0], "Effects") != 0) continue;
        fx_label(fx, seg[1], label, sizeof label);
        snprintf(id, sizeof id, "effects.%s", fx->id);
        cmd_item(a, id, label);
    }
}

static void extras(app *a, const char *menu)
{
    bool sep = false;
    for (int32_t i = 0; i < a->nmextra; i++) {
        const app_menu_extra_rec *e = &a->mextra[i];
        const app_cmd *c;
        if (strcmp(e->menu, menu) != 0) continue;
        c = app_cmd_find(a, e->cmd);
        if (!sep) { ui_menu_separator(a->ui); sep = true; }
        cmd_item(a, e->cmd, e->label ? e->label : (c ? c->label : e->cmd));
    }
}

static const menu_item *items(app *a, const menu_item *it);

/* Declares items until MI_END or MI_SUB_END; returns the item after. */
static const menu_item *items(app *a, const menu_item *it)
{
    ui_ctx *ui = a->ui;
    for (; it->kind != MI_END; it++) {
        switch (it->kind) {
        case MI_CMD: cmd_item(a, it->id, it->label); break;
        case MI_OPT:
            if (app_cmd_exists(a, it->id)) cmd_item(a, it->id, it->label);
            break;
        case MI_SEP: ui_menu_separator(ui); break;
        case MI_SUB:
        case MI_SUB_OPT: {
            const menu_item *first = it + 1;
            bool show = it->kind == MI_SUB || (first->kind == MI_CMD && first->id &&
                                               app_cmd_exists(a, first->id));
            if (show && ui_menu_begin(ui, it->label)) {
                it = items(a, it + 1);
                ui_menu_end(ui);
            } else {
                /* skip to the matching end */
                int depth = 1;
                while (depth > 0 && it->kind != MI_END) {
                    it++;
                    if (it->kind == MI_SUB || it->kind == MI_SUB_OPT) depth++;
                    if (it->kind == MI_SUB_END) depth--;
                }
            }
            break;
        }
        case MI_SUB_END: return it;
        case MI_RECENT: recent_menu(a); break;
        case MI_ADJUST: adjust_items(a); break;
        case MI_EFFECTS: effect_items(a); break;
        default: break;
        }
    }
    return it;
}

void app_menubar(app *a, ui_rect bar, int32_t *end_x)
{
    ui_ctx *ui = a->ui;
    int32_t x = bar.x + ui_px(ui, 4.0f);
    ui_menubar_begin(ui, bar);
    for (size_t m = 0; m < sizeof k_menus / sizeof k_menus[0]; m++) {
        const char *t = k_menus[m].title;
        x += (int32_t)(ui_text_width(ui_font_regular(ui), ui_font_px(ui), t, strlen(t)) + 0.999f) +
             ui_px(ui, 20.0f);
        if (ui_menu_begin(ui, t)) {
            (void)items(a, k_menus[m].items);
            extras(a, t);
            ui_menu_end(ui);
        }
    }
    ui_menubar_end(ui);
    if (end_x) *end_x = x;
}

void app_help_menu(app *a)
{
    (void)items(a, k_help);
    extras(a, "Help");
}

size_t app_menu_ids(const char **out, size_t cap)
{
    size_t k = 0;
    for (size_t m = 0; m < sizeof k_menus / sizeof k_menus[0]; m++)
        for (const menu_item *it = k_menus[m].items; it->kind != MI_END; it++)
            if (it->id) {
                if (out && k < cap) out[k] = it->id;
                k++;
            }
    return k;
}

bool app_menu_extra(app *a, const char *menu, const char *cmd_id, const char *label)
{
    app_menu_extra_rec *e;
    if (!a || !menu || !cmd_id) return false;
    if (a->nmextra == a->cap_mextra) {
        int32_t nc = a->cap_mextra ? a->cap_mextra * 2 : 8;
        app_menu_extra_rec *n = (app_menu_extra_rec *)realloc(a->mextra, (size_t)nc * sizeof *n);
        if (!n) return false;
        a->mextra = n;
        a->cap_mextra = nc;
    }
    e = &a->mextra[a->nmextra];
    e->menu = app_strdup(menu);
    e->cmd = app_strdup(cmd_id);
    e->label = label ? app_strdup(label) : NULL;
    if (!e->menu || !e->cmd || (label && !e->label)) {
        free(e->menu);
        free(e->cmd);
        free(e->label);
        return false;
    }
    a->nmextra++;
    return true;
}

void app_menu_free(app *a)
{
    for (int32_t i = 0; i < a->nmextra; i++) {
        free(a->mextra[i].menu);
        free(a->mextra[i].cmd);
        free(a->mextra[i].label);
    }
    free(a->mextra);
    a->mextra = NULL;
    a->nmextra = a->cap_mextra = 0;
}
