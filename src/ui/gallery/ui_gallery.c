/* ui_gallery.c - widget gallery shared by the gallery executable and the
 * offscreen screenshot tests (lane L3). All strings are placeholders for
 * the demo, not app text. */
#include "ui_gallery.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_LAYERS 5
#define N_HISTORY 9
#define N_DOCS 4
#define N_PALETTE 48

struct ui_gallery {
    SDL_Renderer  *r;
    SDL_Texture   *thumbs[N_DOCS];
    SDL_Texture   *layer_thumbs[N_LAYERS];
    int            scene;
    int            frame_no;
    bool           theme_req, want_dark;
    /* widget values */
    int            tool;
    bool           grid, rulers, aa, check_a, check_b, sw, toggle;
    int            radio, combo, blend, tabs, layer_sel, hist_sel, active_doc, active_slot;
    char           name[64], search[64];
    int32_t        brush, width_i, radius_i;
    double         strength, zoom, angle, slider_v;
    ui_vec2        center;
    ui_color_edit  color;
    ui_color       primary, secondary;
    ui_color       palette[N_PALETTE];
    bool           layer_vis[N_LAYERS];
    int            layer_order[N_LAYERS];
    ui_panel_state p_tools, p_history, p_layers, p_colors;
    bool           show_dialog;
    char           dlg_w[16];
    int32_t        dlg_width, dlg_height;
    bool           dlg_keep;
    int            dlg_resample;
};

static const char *const k_layer_names[N_LAYERS] = { "Background", "Sky gradient", "Mountains",
                                                    "Lettering", "Highlights" };
static const char *const k_history[N_HISTORY] = {
    "Open Image", "Paintbrush", "Paintbrush",       "Rectangle Select",    "Gaussian Blur",
    "Deselect",   "Text",       "Layer Properties", "Move Selected Pixels"
};
static const ui_icon k_history_icons[N_HISTORY] = {
    UI_ICON_IMAGE, UI_ICON_TOOL_PAINTBRUSH, UI_ICON_TOOL_PAINTBRUSH, UI_ICON_TOOL_RECT_SELECT,
    UI_ICON_EFFECTS, UI_ICON_DESELECT, UI_ICON_TOOL_TEXT, UI_ICON_LAYER_PROPERTIES,
    UI_ICON_TOOL_MOVE_PIXELS };
static const char *const k_docs[N_DOCS] = { "harbor_sunset.png", "logo-draft.pdn", "scan_0042.tif",
                                           "Untitled 4" };
static const char *const k_blend[] = { "Normal",      "Multiply", "Additive", "Color Burn",
                                       "Color Dodge", "Reflect",  "Glow",     "Overlay",
                                       "Difference",  "Negation", "Lighten",  "Darken",
                                       "Screen",      "Xor" };
static const ui_icon k_tools[] = {
    UI_ICON_TOOL_MOVE_PIXELS, UI_ICON_TOOL_MOVE_SELECTION, UI_ICON_TOOL_RECT_SELECT,
    UI_ICON_TOOL_LASSO_SELECT, UI_ICON_TOOL_ELLIPSE_SELECT, UI_ICON_TOOL_MAGIC_WAND,
    UI_ICON_TOOL_ZOOM, UI_ICON_TOOL_PAN, UI_ICON_TOOL_PAINT_BUCKET, UI_ICON_TOOL_GRADIENT,
    UI_ICON_TOOL_PAINTBRUSH, UI_ICON_TOOL_ERASER, UI_ICON_TOOL_PENCIL, UI_ICON_TOOL_COLOR_PICKER,
    UI_ICON_TOOL_CLONE_STAMP, UI_ICON_TOOL_RECOLOR, UI_ICON_TOOL_TEXT, UI_ICON_TOOL_LINE_CURVE,
    UI_ICON_TOOL_SHAPES };
static const char *const k_tool_names[] = { "Move Selected Pixels (M)",
                                            "Move Selection (M)",
                                            "Rectangle Select (S)",
                                            "Lasso Select (S)",
                                            "Ellipse Select (S)",
                                            "Magic Wand (S)",
                                            "Zoom (Z)",
                                            "Pan (H)",
                                            "Paint Bucket (F)",
                                            "Gradient (G)",
                                            "Paintbrush (B)",
                                            "Eraser (E)",
                                            "Pencil (P)",
                                            "Color Picker (K)",
                                            "Clone Stamp (L)",
                                            "Recolor (R)",
                                            "Text (T)",
                                            "Line / Curve (O)",
                                            "Shapes (O)" };

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

/* ---- procedural thumbnails ----------------------------------------------- */
static SDL_Texture *make_thumb(SDL_Renderer *r, int w, int h, int kind)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    SDL_Texture *t;
    if (!px) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)x / (float)w, fy = (float)y / (float)h;
            uint8_t *d = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            float rr = 0, gg = 0, bb = 0, a = 1.0f;
            switch (kind % 6) {
            case 0: {            /* sunset over water */
                float sky = fy < 0.6f ? fy / 0.6f : 1.0f;
                rr = 0.98f - 0.2f * sky; gg = 0.55f + 0.2f * sky; bb = 0.35f + 0.45f * sky;
                if (fy > 0.62f) {
                    rr = 0.16f + 0.1f * (1 - fy);
                    gg = 0.28f;
                    bb = 0.45f + 0.2f * (fy - 0.6f);
                }
                {
                    float dx = fx - 0.62f, dy = fy - 0.52f;
                    if (dx * dx + dy * dy < 0.012f) { rr = 1.0f; gg = 0.86f; bb = 0.45f; }
                }
                if (fy > 0.45f && fy < 0.62f && fabsf(fx - 0.3f) < 0.35f - (fy - 0.45f) * 1.2f) {
                    rr = 0.2f; gg = 0.24f; bb = 0.32f;
                }
                break;
            }
            case 1: {            /* logo on transparent */
                float dx = fx - 0.5f, dy = fy - 0.5f, d2 = sqrtf(dx * dx + dy * dy);
                a = d2 < 0.36f ? 1.0f : 0.0f;
                rr = 0.18f; gg = 0.42f + 0.3f * fy; bb = 0.87f;
                if (d2 < 0.2f) { rr = 1.0f; gg = 1.0f; bb = 1.0f; }
                if (fabsf(dx) < 0.06f && fabsf(dy) < 0.16f) { rr = 0.18f; gg = 0.42f; bb = 0.87f; }
                break;
            }
            case 2: {            /* grayscale scan */
                float v = 0.85f - 0.25f * fy + 0.05f * sinf(fx * 40.0f);
                if (fx > 0.15f && fx < 0.85f && fmodf(fy * 9.0f, 1.0f) < 0.35f && fy > 0.2f &&
                    fy < 0.8f)
                    v = 0.3f;
                rr = gg = bb = v;
                break;
            }
            case 3:
                rr = 1.0f; gg = 1.0f; bb = 1.0f;
                break;
            case 4: {            /* mountains */
                float ridge = 0.55f + 0.12f * sinf(fx * 9.0f) + 0.05f * sinf(fx * 23.0f);
                a = fy > ridge ? 1.0f : 0.0f;
                rr = 0.25f; gg = 0.38f + 0.2f * (fy - ridge); bb = 0.3f;
                break;
            }
            default: {           /* lettering */
                a = (fmodf(fx * 6.0f, 1.0f) < 0.55f && fy > 0.35f && fy < 0.65f) ? 1.0f : 0.0f;
                rr = 0.95f; gg = 0.95f; bb = 0.98f;
                break;
            }
            }
            d[0] = (uint8_t)(clamp01(rr) * 255.0f);
            d[1] = (uint8_t)(clamp01(gg) * 255.0f);
            d[2] = (uint8_t)(clamp01(bb) * 255.0f);
            d[3] = (uint8_t)(a * 255.0f);
        }
    t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    free(px);
    return t;
}

ui_gallery *ui_gallery_create(SDL_Renderer *r)
{
    ui_gallery *g = (ui_gallery *)calloc(1u, sizeof *g);
    if (!g) return NULL;
    g->r = r;
    g->thumbs[0] = make_thumb(r, 96, 64, 0);
    g->thumbs[1] = make_thumb(r, 64, 64, 1);
    g->thumbs[2] = make_thumb(r, 60, 80, 2);
    g->thumbs[3] = make_thumb(r, 80, 60, 3);
    g->layer_thumbs[0] = make_thumb(r, 48, 32, 3);
    g->layer_thumbs[1] = make_thumb(r, 48, 32, 0);
    g->layer_thumbs[2] = make_thumb(r, 48, 32, 4);
    g->layer_thumbs[3] = make_thumb(r, 48, 32, 5);
    g->layer_thumbs[4] = make_thumb(r, 48, 32, 1);
    g->tool = 10;
    g->grid = true;
    g->aa = true;
    g->check_a = true;
    g->sw = true;
    g->radio = 1;
    g->combo = 0;
    g->blend = 0;
    g->layer_sel = 2;
    g->hist_sel = 6;
    g->active_doc = 0;
    snprintf(g->name, sizeof g->name, "Mountains");
    g->brush = 12;
    g->width_i = 1920;
    g->radius_i = 6;
    g->strength = 65.0;
    g->zoom = 100.0;
    g->angle = 45.0;
    g->slider_v = 0.35;
    g->center = ui_vec2_make(0.25f, -0.2f);
    g->primary = ui_rgb_hex(0x1F5FAF);
    g->secondary = ui_rgb_hex(0xFFFFFF);
    g->color.hsv.h = 0;
    ui_color_edit_set_rgba(&g->color, ui_rgba(0x2E, 0x8B, 0xC9, 0xFF));
    for (int i = 0; i < N_PALETTE; i++) {
        ui_hsv h;
        h.h = (float)(i % 12) * 30.0f;
        h.s = i < 12 ? 0.85f : (i < 24 ? 0.55f : (i < 36 ? 0.3f : 0.0f));
        h.v = i < 36 ? (i < 24 ? 0.95f : 0.85f) : (float)(i - 36) / 11.0f;
        g->palette[i] = ui_hsv_to_rgb(h, 255);
    }
    for (int i = 0; i < N_LAYERS; i++) {
        g->layer_vis[i] = i != 4;
        g->layer_order[i] = N_LAYERS - 1 - i;
    }
    g->p_tools =
        (ui_panel_state){ 8.0f, 8.0f, 196.0f, 156.0f, UI_ANCHOR_START, UI_ANCHOR_START, true };
    g->p_history =
        (ui_panel_state){ 8.0f, 172.0f, 196.0f, 300.0f, UI_ANCHOR_START, UI_ANCHOR_START, true };
    g->p_layers =
        (ui_panel_state){ 8.0f, 8.0f, 296.0f, 262.0f, UI_ANCHOR_END, UI_ANCHOR_START, true };
    g->p_colors =
        (ui_panel_state){ 8.0f, 8.0f, 296.0f, 448.0f, UI_ANCHOR_END, UI_ANCHOR_END, true };
    snprintf(g->dlg_w, sizeof g->dlg_w, "1920");
    g->dlg_width = 1920;
    g->dlg_height = 1080;
    g->dlg_keep = true;
    g->dlg_resample = 0;
    return g;
}

void ui_gallery_destroy(ui_gallery *g)
{
    if (!g) return;
    for (int i = 0; i < N_DOCS; i++) if (g->thumbs[i]) SDL_DestroyTexture(g->thumbs[i]);
    for (int i = 0; i < N_LAYERS; i++)
        if (g->layer_thumbs[i]) SDL_DestroyTexture(g->layer_thumbs[i]);
    free(g);
}

void ui_gallery_set_scene(ui_gallery *g, int scene)
{
    g->scene = scene;
    g->show_dialog = scene == UI_GALLERY_SCENE_DIALOG;
}

bool ui_gallery_theme_request(ui_gallery *g, bool *dark)
{
    bool r = g->theme_req;
    *dark = g->want_dark;
    g->theme_req = false;
    return r;
}

/* ---- pieces -------------------------------------------------------------- */
static void menus(ui_gallery *g, ui_ctx *ctx, ui_rect bar)
{
    const ui_palette *p = ui_pal(ctx);
    bool dummy_check = true;
    ui_draw_rect(ctx, bar, p->window);
    ui_menubar_begin(ctx, bar);
    if (ui_menu_begin(ctx, "File")) {
        ui_menu_item_icon(ctx, UI_ICON_NEW, "New...", "Ctrl+N", true);
        ui_menu_item_icon(ctx, UI_ICON_OPEN, "Open...", "Ctrl+O", true);
        if (ui_menu_begin(ctx, "Open Recent")) {
            for (int i = 0; i < N_DOCS; i++) ui_menu_item(ctx, k_docs[i], NULL, true);
            ui_menu_separator(ctx);
            ui_menu_item(ctx, "Clear List", NULL, true);
            ui_menu_end(ctx);
        }
        ui_menu_separator(ctx);
        ui_menu_item_icon(ctx, UI_ICON_SAVE, "Save", "Ctrl+S", true);
        ui_menu_item_icon(ctx, UI_ICON_SAVE_AS, "Save As...", "Ctrl+Shift+S", true);
        ui_menu_item(ctx, "Save All", "Ctrl+Alt+S", false);
        ui_menu_separator(ctx);
        ui_menu_item(ctx, "Close", "Ctrl+W", true);
        ui_menu_item(ctx, "Exit", "Alt+F4", true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Edit")) {
        ui_menu_item_icon(ctx, UI_ICON_UNDO, "Undo", "Ctrl+Z", true);
        ui_menu_item_icon(ctx, UI_ICON_REDO, "Redo", "Ctrl+Y", false);
        ui_menu_separator(ctx);
        ui_menu_item_icon(ctx, UI_ICON_CUT, "Cut", "Ctrl+X", true);
        ui_menu_item_icon(ctx, UI_ICON_COPY, "Copy", "Ctrl+C", true);
        ui_menu_item_icon(ctx, UI_ICON_PASTE, "Paste", "Ctrl+V", true);
        ui_menu_separator(ctx);
        ui_menu_item_icon(ctx, UI_ICON_SELECT_ALL, "Select All", "Ctrl+A", true);
        ui_menu_item_icon(ctx, UI_ICON_DESELECT, "Deselect All", "Ctrl+D", true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "View")) {
        ui_menu_item_icon(ctx, UI_ICON_ZOOM_IN, "Zoom In", "Ctrl++", true);
        ui_menu_item_icon(ctx, UI_ICON_ZOOM_OUT, "Zoom Out", "Ctrl+-", true);
        ui_menu_item_icon(ctx, UI_ICON_ZOOM_FIT, "Zoom to Window", "Ctrl+B", true);
        ui_menu_item_icon(ctx, UI_ICON_ZOOM_ACTUAL, "Actual Size", "Ctrl+0", true);
        ui_menu_separator(ctx);
        ui_menu_check(ctx, "Pixel Grid", "Ctrl+'", &g->grid, true);
        ui_menu_check(ctx, "Rulers", "Ctrl+R", &g->rulers, true);
        ui_menu_separator(ctx);
        if (ui_menu_radio(ctx, "Light Theme", NULL, ui_get_theme(ctx)->kind == UI_THEME_LIGHT,
                          true)) {
            g->theme_req = true;
            g->want_dark = false;
        }
        if (ui_menu_radio(ctx, "Dark Theme", NULL, ui_get_theme(ctx)->kind == UI_THEME_DARK,
                          true)) {
            g->theme_req = true;
            g->want_dark = true;
        }
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Image")) {
        ui_menu_item_icon(ctx, UI_ICON_CROP, "Crop to Selection", "Ctrl+Shift+X", true);
        ui_menu_item_icon(ctx, UI_ICON_RESIZE, "Resize...", "Ctrl+R", true);
        ui_menu_item_icon(ctx, UI_ICON_CANVAS_SIZE, "Canvas Size...", "Ctrl+Shift+R", true);
        ui_menu_separator(ctx);
        ui_menu_item_icon(ctx, UI_ICON_FLIP_H, "Flip Horizontal", NULL, true);
        ui_menu_item_icon(ctx, UI_ICON_FLIP_V, "Flip Vertical", NULL, true);
        ui_menu_item_icon(ctx, UI_ICON_ROTATE_CW, "Rotate 90 Clockwise", "Ctrl+H", true);
        ui_menu_item_icon(ctx, UI_ICON_ROTATE_CCW, "Rotate 90 Counter-clockwise", "Ctrl+G", true);
        ui_menu_item_icon(ctx, UI_ICON_ROTATE_180, "Rotate 180", "Ctrl+J", true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Layers")) {
        ui_menu_item_icon(ctx, UI_ICON_LAYER_ADD, "Add New Layer", "Ctrl+Shift+N", true);
        ui_menu_item_icon(ctx, UI_ICON_LAYER_DELETE, "Delete Layer", "Ctrl+Shift+Del", true);
        ui_menu_item_icon(ctx, UI_ICON_LAYER_DUPLICATE, "Duplicate Layer", "Ctrl+Shift+D", true);
        ui_menu_item_icon(ctx, UI_ICON_LAYER_MERGE, "Merge Layer Down", "Ctrl+M", true);
        ui_menu_separator(ctx);
        ui_menu_item_icon(ctx, UI_ICON_LAYER_PROPERTIES, "Layer Properties...", "F4", true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Adjustments")) {
        ui_menu_item(ctx, "Auto-Level", "Ctrl+Shift+L", true);
        ui_menu_item(ctx, "Black and White", "Ctrl+Shift+G", true);
        ui_menu_item(ctx, "Brightness / Contrast...", NULL, true);
        ui_menu_item(ctx, "Curves...", "Ctrl+Shift+M", true);
        ui_menu_item(ctx, "Hue / Saturation...", "Ctrl+Shift+U", true);
        ui_menu_item(ctx, "Invert Colors", "Ctrl+Shift+I", true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Effects")) {
        ui_menu_item(ctx, "Repeat Last", "Ctrl+F", false);
        ui_menu_separator(ctx);
        if (ui_menu_begin(ctx, "Blurs")) {
            ui_menu_item(ctx, "Gaussian Blur...", NULL, true);
            ui_menu_item(ctx, "Motion Blur...", NULL, true);
            ui_menu_item(ctx, "Radial Blur...", NULL, true);
            ui_menu_item(ctx, "Zoom Blur...", NULL, true);
            ui_menu_end(ctx);
        }
        if (ui_menu_begin(ctx, "Distort")) {
            ui_menu_item(ctx, "Bulge...", NULL, true);
            ui_menu_item(ctx, "Twist...", NULL, true);
            ui_menu_end(ctx);
        }
        ui_menu_check(ctx, "Live Preview", NULL, &dummy_check, true);
        ui_menu_end(ctx);
    }
    if (ui_menu_begin(ctx, "Help")) {
        ui_menu_item_icon(ctx, UI_ICON_HELP, "Help Topics", "F1", true);
        ui_menu_item(ctx, "About", NULL, true);
        ui_menu_end(ctx);
    }
    ui_menubar_end(ctx);
    /* panel toggles on the right of the menu bar */
    {
        ui_rect r = bar;
        static const ui_icon ic[] = { UI_ICON_WIN_TOOLS, UI_ICON_WIN_HISTORY, UI_ICON_WIN_LAYERS,
                                      UI_ICON_WIN_COLORS };
        bool *open[] = { &g->p_tools.open, &g->p_history.open, &g->p_layers.open,
                         &g->p_colors.open };
        static const char *const ids[] = { "##w_tools", "##w_history", "##w_layers", "##w_colors" };
        static const char *const tips[] = { "Tools (F5)", "History (F6)", "Layers (F7)",
                                            "Colors (F8)" };
        int32_t s = ui_px(ctx, 28.0f);
        r = ui_cut_right(&r, ui_px(ctx, 6.0f) + 6 * s);
        r.x += ui_px(ctx, 4.0f);
        for (int i = 0; i < 4; i++) {
            ui_layout_set_next(ctx, ui_rect_make(r.x + i * s, r.y + (r.h - s) / 2, s, s));
            if (ui_tool_button(ctx, ids[i], ic[i], *open[i], tips[i])) *open[i] = !*open[i];
        }
        ui_layout_set_next(ctx,
                           ui_rect_make(r.x + 4 * s + ui_px(ctx, 2.0f), r.y + (r.h - s) / 2, s, s));
        ui_icon_button(ctx, "##settings", UI_ICON_SETTINGS, "Settings");
        ui_layout_set_next(ctx,
                           ui_rect_make(r.x + 5 * s + ui_px(ctx, 2.0f), r.y + (r.h - s) / 2, s, s));
        ui_icon_button(ctx, "##help", UI_ICON_HELP, "Help");
    }
}

static void toolbar(ui_gallery *g, ui_ctx *ctx, ui_rect bar)
{
    const ui_palette *p = ui_pal(ctx);
    ui_size cells[32];
    int n = 0;
    static const ui_icon cmd[] = { UI_ICON_NEW,
                                   UI_ICON_OPEN,
                                   UI_ICON_SAVE,
                                   0,
                                   UI_ICON_CUT,
                                   UI_ICON_COPY,
                                   UI_ICON_PASTE,
                                   UI_ICON_CROP,
                                   UI_ICON_DESELECT,
                                   0,
                                   UI_ICON_UNDO,
                                   UI_ICON_REDO,
                                   0 };
    static const char *const tips[] = { "New (Ctrl+N)",
                                        "Open (Ctrl+O)",
                                        "Save (Ctrl+S)",
                                        "",
                                        "Cut (Ctrl+X)",
                                        "Copy (Ctrl+C)",
                                        "Paste (Ctrl+V)",
                                        "Crop to Selection",
                                        "Deselect (Ctrl+D)",
                                        "",
                                        "Undo (Ctrl+Z)",
                                        "Redo (Ctrl+Y)",
                                        "" };
    ui_draw_rect(ctx, bar, p->window);
    ui_draw_rect(ctx, ui_rect_make(bar.x, bar.y + bar.h - 1, bar.w, 1), p->separator);
    ui_layout_push(ctx, ui_rect_inset(bar, 0, 0), 0.0f);
    ui_layout_space(ctx, 6.0f);
    for (int i = 0; i < 13; i++) cells[n++] = ui_size_px(cmd[i] ? 28.0f : 9.0f);
    cells[n++] = ui_size_px(28.0f);
    cells[n++] = ui_size_px(28.0f);
    cells[n++] = ui_size_px(9.0f);
    cells[n++] = ui_size_auto();
    ui_layout_row(ctx, 0.0f, n, cells);
    for (int i = 0; i < 13; i++) {
        char id[16];
        if (!cmd[i]) { ui_separator(ctx); continue; }
        snprintf(id, sizeof id, "##tb%d", i);
        ui_icon_button(ctx, id, cmd[i], tips[i]);
    }
    if (ui_tool_button(ctx, "##grid", UI_ICON_GRID, g->grid, "Pixel Grid")) g->grid = !g->grid;
    if (ui_tool_button(ctx, "##rulers", UI_ICON_RULERS, g->rulers, "Rulers"))
        g->rulers = !g->rulers;
    ui_separator(ctx);
    ui_label_ex(ctx, "Tool:", UI_LABEL_DIM);
    ui_layout_column(ctx);
    ui_layout_pop(ctx);
    /* tool options: second half of the bar */
    {
        ui_rect r = bar;
        ui_size c2[12];
        int m = 0;
        r.x += ui_px(ctx, 560.0f);
        r.w -= ui_px(ctx, 560.0f);
        ui_layout_push(ctx, r, 0.0f);
        ui_layout_space(ctx, 6.0f);
        c2[m++] = ui_size_px(170.0f);
        c2[m++] = ui_size_px(9.0f);
        c2[m++] = ui_size_auto();
        c2[m++] = ui_size_px(92.0f);
        c2[m++] = ui_size_px(9.0f);
        c2[m++] = ui_size_px(42.0f);
        c2[m++] = ui_size_px(42.0f);
        c2[m++] = ui_size_px(9.0f);
        c2[m++] = ui_size_px(150.0f);
        ui_layout_row(ctx, 0.0f, m, c2);
        {
            int t = g->tool;
            const char *names[19];
            for (int i = 0; i < 19; i++) names[i] = k_tool_names[i];
            if (ui_combo(ctx, "##toolcombo", &t, names, 19)) g->tool = t;
        }
        ui_separator(ctx);
        ui_label_ex(ctx, "Brush width:", UI_LABEL_DIM);
        ui_number_int(ctx, "##brush", &g->brush, 1, 2000, 1, 0);
        ui_separator(ctx);
        {
            int r1 = ui_split_button(ctx, "##aa", g->aa ? UI_ICON_AA_ON : UI_ICON_AA_OFF, g->aa,
                                     "Antialiasing");
            if (r1 == 1) g->aa = !g->aa;
            if (r1 == 2) ui_popup_open(ctx, "##aa_menu", ui_last_rect(ctx), UI_POPUP_BELOW);
            if (ui_popup_begin(ctx, "##aa_menu")) {
                if (ui_menu_radio(ctx, "Antialiasing Enabled", NULL, g->aa, true)) g->aa = true;
                if (ui_menu_radio(ctx, "Antialiasing Disabled", NULL, !g->aa, true)) g->aa = false;
                ui_popup_end(ctx);
            }
        }
        (void)ui_split_button(ctx, "##selmode", UI_ICON_SEL_UNION, false, "Selection Mode");
        ui_separator(ctx);
        {
            int b = g->blend;
            if (ui_combo(ctx, "##blend", &b, k_blend, 14)) g->blend = b;
        }
        ui_layout_column(ctx);
        ui_layout_pop(ctx);
    }
}

static void doc_tabs(ui_gallery *g, ui_ctx *ctx, ui_rect r)
{
    const ui_palette *p = ui_pal(ctx);
    ui_doc_tab tabs[N_DOCS];
    static const int dims[N_DOCS][2] = { { 96, 64 }, { 64, 64 }, { 60, 80 }, { 80, 60 } };
    ui_doc_tabs_result res;
    ui_draw_rect(ctx, r, p->window);
    for (int i = 0; i < N_DOCS; i++) {
        tabs[i].title = k_docs[i];
        tabs[i].thumb = g->thumbs[i];
        tabs[i].thumb_w = dims[i][0];
        tabs[i].thumb_h = dims[i][1];
        tabs[i].modified = i == 1 || i == 3;
    }
    res = ui_doc_tabs(ctx, "##docs", ui_rect_inset(r, ui_px(ctx, 6.0f), 0), tabs, N_DOCS,
                      &g->active_doc, 0);
    (void)res;
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y + r.h - 1, r.w, 1), p->separator);
}

static void group_buttons(ui_gallery *g, ui_ctx *ctx)
{
    ui_size c3[3] = { ui_size_auto(), ui_size_auto(), ui_size_auto() };
    ui_size c7[8];
    ui_group_begin(ctx, "Buttons");
    ui_layout_row(ctx, 0.0f, 3, c3);
    ui_button_ex(ctx, "Apply", UI_ICON_NONE, UI_BUTTON_PRIMARY);
    ui_button(ctx, "Cancel");
    ui_button_ex(ctx, "Disabled", UI_ICON_NONE, UI_DISABLED);
    ui_layout_row(ctx, 0.0f, 2, c3);
    ui_button_ex(ctx, "Open...", UI_ICON_OPEN, 0);
    ui_toggle(ctx, "Snap", UI_ICON_GRID, &g->toggle);
    for (int i = 0; i < 8; i++) c7[i] = ui_size_px(28.0f);
    ui_layout_row(ctx, 0.0f, 8, c7);
    ui_icon_button(ctx, "##b1", UI_ICON_UNDO, "Undo");
    ui_icon_button(ctx, "##b2", UI_ICON_REDO, "Redo");
    ui_icon_button(ctx, "##b3", UI_ICON_ZOOM_IN, "Zoom In");
    ui_icon_button(ctx, "##b4", UI_ICON_ZOOM_OUT, "Zoom Out");
    if (ui_tool_button(ctx, "##b5", UI_ICON_BOLD, g->check_b, "Bold")) g->check_b = !g->check_b;
    ui_tool_button(ctx, "##b6", UI_ICON_ITALIC, true, "Italic");
    ui_tool_button(ctx, "##b7", UI_ICON_UNDERLINE, false, "Underline");
    ui_tool_button(ctx, "##b8", UI_ICON_ALIGN_CENTER, false, "Center");
    ui_layout_column(ctx);
    ui_switch(ctx, "Show selection outline", &g->sw);
    ui_group_end(ctx);
}

static void group_choices(ui_gallery *g, ui_ctx *ctx)
{
    static const char *const radios[] = { "Layer", "Image", "Selection" };
    static const char *const sizes[] = { "Single pixel",  "3 x 3 region", "5 x 5 region",
                                         "7 x 7 region",  "9 x 9 region", "11 x 11 region",
                                         "51 x 51 region" };
    ui_group_begin(ctx, "Choices");
    ui_checkbox(ctx, "Keep aspect ratio", &g->check_a);
    ui_checkbox(ctx, "Sample merged layers", &g->check_b);
    ui_radio_group(ctx, "##sample", &g->radio, radios, 3, true);
    ui_combo(ctx, "##sampling", &g->combo, sizes, 7);
    ui_group_end(ctx);
}

static void group_text(ui_gallery *g, ui_ctx *ctx)
{
    ui_size c2[2] = { ui_size_px(64.0f), ui_size_fr(1.0f) };
    ui_group_begin(ctx, "Text entry");
    ui_layout_row(ctx, 0.0f, 2, c2);
    ui_label_ex(ctx, "Name", UI_LABEL_DIM);
    ui_text_field(ctx, "##name", g->name, sizeof g->name, 0);
    ui_label_ex(ctx, "Search", UI_LABEL_DIM);
    ui_text_field_ex(ctx, "##search", g->search, sizeof g->search, 0, "Filter effects...");
    ui_label_ex(ctx, "Width", UI_LABEL_DIM);
    ui_number_int(ctx, "##width", &g->width_i, 1, 65535, 1, 0);
    ui_label_ex(ctx, "Zoom", UI_LABEL_DIM);
    ui_number_double(ctx, "##zoom", &g->zoom, 1.0, 3200.0, 10.0, 1, UI_SLIDER_PERCENT);
    ui_layout_column(ctx);
    ui_group_end(ctx);
}

static void group_sliders(ui_gallery *g, ui_ctx *ctx)
{
    ui_group_begin(ctx, "Sliders");
    ui_slider_double(ctx, "##plain", &g->slider_v, 0.0, 1.0, 0.0, 0);
    ui_prop_slider_int(ctx, "Radius", &g->radius_i, 0, 200, 2, 0);
    ui_prop_slider_double(ctx, "Strength", &g->strength, 0.0, 100.0, 50.0, 0.5, 1,
                          UI_SLIDER_PERCENT);
    ui_layout_space(ctx, 4.0f);
    ui_progress(ctx, 0.62f);
    ui_progress(ctx, -1.0f);
    ui_group_end(ctx);
}

static void group_angle(ui_gallery *g, ui_ctx *ctx)
{
    ui_group_begin(ctx, "Angle and center");
    ui_angle(ctx, "##angle", &g->angle, -180.0, 180.0);
    ui_layout_space(ctx, 4.0f);
    ui_point_picker(ctx, "##center", &g->center, g->thumbs[0], 112.0f);
    ui_group_end(ctx);
}

static void group_tabs(ui_gallery *g, ui_ctx *ctx)
{
    static const char *const labels[] = { "General", "Rendering", "Advanced" };
    ui_group_begin(ctx, "Tabs and sections");
    ui_tabs(ctx, "##tabs", &g->tabs, labels, 3);
    if (ui_collapsing(ctx, "Description", true))
        ui_text_wrapped(ctx, "Immediate-mode widgets laid out in rows and columns. Every control "
                             "renders through SDL_Renderer with crisp edges at any scale.",
                        UI_LABEL_DIM);
    (void)ui_collapsing(ctx, "Licensing", false);
    ui_group_end(ctx);
}

static void group_icons(ui_ctx *ctx)
{
    const ui_palette *p = ui_pal(ctx);
    int32_t s = ui_px(ctx, 26.0f), cols;
    ui_rect r;
    ui_group_begin(ctx, "Icons");
    r = ui_layout_rest(ctx);
    cols = imax(1, r.w / s);
    r = ui_layout_next(ctx, r.w, ((UI_ICON_COUNT - 1 + cols - 1) / cols) * s);
    for (int i = 1; i < UI_ICON_COUNT; i++) {
        ui_rect c = ui_rect_make(r.x + ((i - 1) % cols) * s, r.y + ((i - 1) / cols) * s, s, s);
        ui_interaction in = ui_interact(ctx, ui_get_id_int(ctx, i), c, 0);
        if (in.hovered) ui_draw_rrect(ctx, c, 4.0f, p->hover);
        ui_draw_icon(ctx, (ui_icon)i, c, ui_px(ctx, 16.0f), p->icon, p->icon_accent);
        ui_tooltip(ctx, ui_icon_name((ui_icon)i));
    }
    ui_group_end(ctx);
}

static void group_palette(ui_gallery *g, ui_ctx *ctx)
{
    int right = -1, k;
    ui_group_begin(ctx, "Palette and swatches");
    k = ui_palette_grid(ctx, "##pal", g->palette, N_PALETTE, 18.0f, &right);
    if (k >= 0) g->primary = g->palette[k];
    if (right >= 0) g->secondary = g->palette[right];
    {
        ui_size c6[6];
        for (int i = 0; i < 6; i++) c6[i] = ui_size_px(28.0f);
        ui_layout_row(ctx, 0.0f, 6, c6);
        ui_color_swatch(ctx, "##s1", ui_rgb_hex(0xE4572E), 0);
        ui_color_swatch(ctx, "##s2", ui_rgb_hex(0x29335C), UI_SWATCH_SELECTED);
        ui_color_swatch(ctx, "##s3", ui_rgba(0x17, 0xBE, 0xBB, 160), 0);
        ui_color_swatch(ctx, "##s4", ui_rgba(255, 201, 20, 255), 0);
        ui_color_swatch(ctx, "##s5", ui_rgba(0, 0, 0, 0), 0);
        ui_color_swatch(ctx, "##s6", ui_rgb_hex(0x76B041), 0);
        ui_layout_column(ctx);
    }
    ui_group_end(ctx);
}

/* ---- panels -------------------------------------------------------------- */
static void panel_tools(ui_gallery *g, ui_ctx *ctx)
{
    ui_size cells[6];
    if (!ui_panel_begin(ctx, "Tools", &g->p_tools, UI_PANEL_CLOSABLE)) return;
    for (int i = 0; i < 6; i++) cells[i] = ui_size_px(28.0f);
    ui_layout_set_spacing(ctx, 2.0f);
    ui_layout_row(ctx, 0.0f, 6, cells);
    for (int i = 0; i < 19; i++) {
        char id[16];
        snprintf(id, sizeof id, "##tool%d", i);
        if (ui_tool_button(ctx, id, k_tools[i], g->tool == i, k_tool_names[i])) g->tool = i;
    }
    ui_layout_column(ctx);
    ui_panel_end(ctx);
}

static void history_row(ui_ctx *ctx, void *ud, int32_t i, ui_rect row, uint32_t state)
{
    ui_gallery *g = (ui_gallery *)ud;
    const ui_palette *p = ui_pal(ctx);
    bool future = i > g->hist_sel;
    ui_color c = future ? p->text_disabled : p->text;
    (void)state;
    ui_draw_icon(ctx, k_history_icons[i],
                 ui_rect_make(row.x + ui_px(ctx, 10.0f), row.y, ui_px(ctx, 16.0f), row.h),
                 ui_px(ctx, 16.0f), future ? p->text_disabled : p->icon,
                 future ? p->text_disabled : p->icon_accent);
    ui_draw_text_box(
        ctx, ui_font_regular(ctx), ui_font_px(ctx),
        ui_rect_make(row.x + ui_px(ctx, 34.0f), row.y, row.w - ui_px(ctx, 38.0f), row.h),
        UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, c, k_history[i], strlen(k_history[i]));
}

static void panel_history(ui_gallery *g, ui_ctx *ctx)
{
    ui_rect rest, foot;
    ui_size c2[3] = { ui_size_px(28.0f), ui_size_px(28.0f), ui_size_fr(1.0f) };
    if (!ui_panel_begin(ctx, "History", &g->p_history, UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE))
        return;
    rest = ui_layout_rest(ctx);
    foot = ui_cut_bottom(&rest, ui_px(ctx, 32.0f));
    {
        int32_t sel = g->hist_sel;
        ui_list(ctx, "##hist", rest, N_HISTORY, 28.0f, &sel, 0, history_row, g);
        g->hist_sel = sel;
    }
    ui_layout_push(ctx, foot, 0.0f);
    ui_layout_space(ctx, 4.0f);
    ui_layout_row(ctx, 0.0f, 3, c2);
    if (ui_icon_button(ctx, "##rew", UI_ICON_HISTORY_REWIND, "Rewind")) g->hist_sel = 0;
    if (ui_icon_button(ctx, "##ff", UI_ICON_HISTORY_FORWARD, "Fast-forward"))
        g->hist_sel = N_HISTORY - 1;
    ui_layout_column(ctx);
    ui_layout_pop(ctx);
    ui_panel_end(ctx);
}

static void layer_row(ui_ctx *ctx, void *ud, int32_t i, ui_rect row, uint32_t state)
{
    ui_gallery *g = (ui_gallery *)ud;
    const ui_palette *p = ui_pal(ctx);
    int li = g->layer_order[i];
    int32_t th = row.h - ui_px(ctx, 10.0f), tw = th * 3 / 2;
    ui_rect thumb = ui_rect_make(row.x + ui_px(ctx, 10.0f), row.y + ui_px(ctx, 5.0f), tw, th);
    ui_rect eye =
        ui_rect_make(row.x + row.w - ui_px(ctx, 34.0f), row.y + (row.h - ui_px(ctx, 28.0f)) / 2,
                     ui_px(ctx, 28.0f), ui_px(ctx, 28.0f));
    ui_interaction in;
    (void)state;
    ui_draw_checker(ctx, thumb, ui_px(ctx, 4.0f), p->checker_a, p->checker_b);
    ui_draw_image(ctx, g->layer_thumbs[li], NULL, thumb, UI_FILTER_LINEAR,
                  ui_rgba(255, 255, 255, 255));
    ui_draw_rect_outline(ctx, ui_rect_inset(thumb, -1, -1), 1,
                         ui_color_fade(p->border_strong, 0.8f));
    ui_draw_text_box(ctx, ui_font_regular(ctx), ui_font_px(ctx),
                     ui_rect_make(thumb.x + tw + ui_px(ctx, 10.0f), row.y,
                                  eye.x - thumb.x - tw - ui_px(ctx, 12.0f), row.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, g->layer_vis[li] ? p->text : p->text_dim,
                     k_layer_names[li], strlen(k_layer_names[li]));
    in = ui_interact(ctx, ui_get_id(ctx, "##eye"), eye, 0);
    if (in.hovered) ui_draw_rrect(ctx, eye, 4.0f, p->hover);
    ui_draw_icon(ctx, g->layer_vis[li] ? UI_ICON_EYE : UI_ICON_EYE_OFF, eye, ui_px(ctx, 16.0f),
                 g->layer_vis[li] ? p->icon : p->text_disabled, p->icon_accent);
    if (in.clicked) g->layer_vis[li] = !g->layer_vis[li];
    ui_tooltip(ctx, "Visible");
}

static void panel_layers(ui_gallery *g, ui_ctx *ctx)
{
    ui_rect rest, foot;
    ui_size c7[7];
    static const ui_icon ic[] = { UI_ICON_LAYER_ADD, UI_ICON_LAYER_DELETE, UI_ICON_LAYER_DUPLICATE,
                                  UI_ICON_LAYER_MERGE, UI_ICON_LAYER_UP, UI_ICON_LAYER_DOWN,
                                  UI_ICON_LAYER_PROPERTIES };
    static const char *const tips[] = { "Add New Layer",    "Delete Layer",  "Duplicate Layer",
                                        "Merge Layer Down", "Move Layer Up", "Move Layer Down",
                                        "Layer Properties" };
    ui_list_result lr;
    if (!ui_panel_begin(ctx, "Layers", &g->p_layers, UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE))
        return;
    rest = ui_layout_rest(ctx);
    foot = ui_cut_bottom(&rest, ui_px(ctx, 34.0f));
    {
        int32_t sel = g->layer_sel;
        lr = ui_list(ctx, "##layers", rest, N_LAYERS, 44.0f, &sel, UI_LIST_REORDER, layer_row, g);
        g->layer_sel = sel;
        if (lr.reordered) {
            int moved = g->layer_order[lr.move_from];
            if (lr.move_from < lr.move_to)
                memmove(&g->layer_order[lr.move_from], &g->layer_order[lr.move_from + 1],
                        (size_t)(lr.move_to - lr.move_from) * sizeof(int));
            else
                memmove(&g->layer_order[lr.move_to + 1], &g->layer_order[lr.move_to],
                        (size_t)(lr.move_from - lr.move_to) * sizeof(int));
            g->layer_order[lr.move_to] = moved;
            g->layer_sel = lr.move_to;
        }
    }
    ui_layout_push(ctx, foot, 0.0f);
    ui_layout_space(ctx, 4.0f);
    for (int i = 0; i < 7; i++) c7[i] = ui_size_px(28.0f);
    ui_layout_row(ctx, 0.0f, 7, c7);
    for (int i = 0; i < 7; i++) {
        char id[12];
        snprintf(id, sizeof id, "##l%d", i);
        ui_icon_button(ctx, id, ic[i], tips[i]);
    }
    ui_layout_column(ctx);
    ui_layout_pop(ctx);
    ui_panel_end(ctx);
}

static void panel_colors(ui_gallery *g, ui_ctx *ctx)
{
    ui_size c2[2] = { ui_size_px(64.0f), ui_size_fr(1.0f) };
    int act;
    if (!ui_panel_begin(ctx, "Colors", &g->p_colors, UI_PANEL_CLOSABLE)) return;
    ui_layout_row(ctx, 0.0f, 2, c2);
    ui_layout_begin(ctx, 0.0f);
    act = ui_color_pair(ctx, "##pair", g->primary, g->secondary, g->active_slot);
    if (act == UI_PAIR_SWAP) {
        ui_color t = g->primary;
        g->primary = g->secondary;
        g->secondary = t;
    }
    if (act == UI_PAIR_RESET) { g->primary = ui_rgb_hex(0); g->secondary = ui_rgb_hex(0xFFFFFF); }
    if (act == UI_PAIR_SELECT_PRIMARY) g->active_slot = 0;
    if (act == UI_PAIR_SELECT_SECONDARY) g->active_slot = 1;
    ui_layout_end(ctx);
    ui_layout_begin(ctx, 0.0f);
    ui_color_wheel(ctx, "##wheel", &g->color, 150.0f, 0);
    ui_layout_end(ctx);
    ui_layout_column(ctx);
    ui_layout_set_spacing(ctx, 2.0f);
    for (int ch = 0; ch < UI_CHAN_COUNT; ch++) {
        char key[8];
        if (ch == UI_CHAN_RED || ch == UI_CHAN_ALPHA) ui_layout_space(ctx, 4.0f);
        snprintf(key, sizeof key, "##ch%d", ch);
        ui_color_channel(ctx, key, ch, &g->color);
    }
    ui_panel_end(ctx);
}

/* ---- dialog -------------------------------------------------------------- */
static void dialog(ui_gallery *g, ui_ctx *ctx)
{
    static const char *const resample[] = { "Best Quality", "Bicubic", "Bilinear",
                                            "Nearest Neighbor", "Supersampling" };
    ui_size c2[2] = { ui_size_px(120.0f), ui_size_fr(1.0f) };
    uint32_t r;
    if (!g->show_dialog) return;
    ui_dialog_begin(ctx, "Resize Image", 400.0f, 0.0f);
    ui_layout_row(ctx, 0.0f, 2, c2);
    ui_label_ex(ctx, "Resampling", UI_LABEL_DIM);
    ui_combo(ctx, "##resample", &g->dlg_resample, resample, 5);
    ui_label_ex(ctx, "Width", UI_LABEL_DIM);
    ui_number_int(ctx, "##dw", &g->dlg_width, 1, 65535, 1, 0);
    ui_label_ex(ctx, "Height", UI_LABEL_DIM);
    ui_number_int(ctx, "##dh", &g->dlg_height, 1, 65535, 1, 0);
    ui_layout_column(ctx);
    ui_checkbox(ctx, "Maintain aspect ratio", &g->dlg_keep);
    ui_text_wrapped(ctx, "New size: 7.9 MB (was 7.9 MB). The image is resampled with the selected "
                         "filter.", UI_LABEL_DIM);
    ui_dialog_buttons(ctx, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ctx);
    if (r) g->show_dialog = false;
}

/* ---- icons scene --------------------------------------------------------- */
static void icons_scene(ui_ctx *ctx)
{
    const ui_palette *p = ui_pal(ctx);
    static const float sizes[] = { 16.0f, 20.0f, 24.0f, 32.0f, 48.0f };
    ui_rect full = ui_layout_content(ctx);
    int32_t x0 = full.x + ui_px(ctx, 16.0f), y = full.y + ui_px(ctx, 16.0f);
    ui_draw_rect(ctx, full, p->panel);
    for (int si = 0; si < 5; si++) {
        int32_t s = ui_px(ctx, sizes[si]), cell = s + ui_px(ctx, 12.0f), x = x0;
        char lbl[16];
        snprintf(lbl, sizeof lbl, "%d px", (int)sizes[si]);
        ui_draw_text(ctx, ui_font_semibold(ctx), ui_font_px(ctx), (float)x,
                     (float)(y + ui_px(ctx, 14.0f)), p->text_dim, lbl, strlen(lbl));
        y += ui_px(ctx, 22.0f);
        for (int i = 1; i < UI_ICON_COUNT; i++) {
            if (x + cell > full.x + full.w - ui_px(ctx, 8.0f)) { x = x0; y += cell; }
            ui_draw_icon(ctx, (ui_icon)i, ui_rect_make(x, y, cell, cell), s, p->icon,
                         p->icon_accent);
            x += cell;
        }
        y += cell + ui_px(ctx, 10.0f);
    }
}

/* ---- frame --------------------------------------------------------------- */
void ui_gallery_frame(ui_gallery *g, ui_ctx *ctx)
{
    const ui_palette *p = ui_pal(ctx);
    ui_rect full = ui_layout_content(ctx), r = full, mb, tb, dt, sb, ws;
    g->frame_no++;
    if (g->scene == UI_GALLERY_SCENE_ICONS) { icons_scene(ctx); return; }
    ui_draw_rect(ctx, full, p->window);
    mb = ui_cut_top(&r, ui_px(ctx, ui_get_theme(ctx)->m.menubar_h));
    tb = ui_cut_top(&r, ui_px(ctx, 40.0f));
    dt = ui_cut_top(&r, ui_px(ctx, 46.0f));
    sb = ui_cut_bottom(&r, ui_px(ctx, 26.0f));
    ws = r;
    ui_draw_rect(ctx, ws, p->workspace);
    /* showcase between the left and right panel columns */
    {
        ui_rect content = ws;
        ui_size c3[3] = { ui_size_fr(1.0f), ui_size_fr(1.0f), ui_size_fr(1.0f) };
        content.x += ui_px(ctx, 212.0f);
        content.w -= ui_px(ctx, 212.0f + 312.0f);
        ui_scroll_begin(ctx, "##showcase", content, UI_SCROLL_NO_BG);
        ui_layout_push(ctx, ui_layout_content(ctx), 8.0f);
        ui_layout_set_spacing(ctx, 10.0f);
        ui_layout_row(ctx, 0.0f, 3, c3);
        ui_layout_begin(ctx, 0.0f);
        group_buttons(g, ctx);
        group_choices(g, ctx);
        group_text(g, ctx);
        ui_layout_end(ctx);
        ui_layout_begin(ctx, 0.0f);
        group_sliders(g, ctx);
        group_angle(g, ctx);
        ui_layout_end(ctx);
        ui_layout_begin(ctx, 0.0f);
        group_tabs(g, ctx);
        group_palette(g, ctx);
        group_icons(ctx);
        ui_layout_end(ctx);
        ui_layout_column(ctx);
        ui_layout_pop(ctx);
        ui_scroll_end(ctx);
    }
    menus(g, ctx, mb);
    toolbar(g, ctx, tb);
    doc_tabs(g, ctx, dt);
    /* status bar */
    {
        char buf[96];
        ui_draw_rect(ctx, sb, p->window);
        ui_draw_rect(ctx, ui_rect_make(sb.x, sb.y, sb.w, 1), p->separator);
        snprintf(buf, sizeof buf, "%s   |   1920 x 1080   |   Zoom %.0f%%", k_tool_names[g->tool],
                 g->zoom);
        ui_draw_text_box(ctx, ui_font_regular(ctx),
                         ui_get_theme(ctx)->m.font_size_small * ui_scale(ctx),
                         ui_rect_inset(sb, ui_px(ctx, 12.0f), 0), UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                         p->text_dim, buf, strlen(buf));
    }
    ui_panels_area(ctx, ui_rect_inset(ws, 0, 0));
    panel_tools(g, ctx);
    panel_history(g, ctx);
    panel_layers(g, ctx);
    panel_colors(g, ctx);
    dialog(g, ctx);
    {
        /* F2 toggles the theme in the interactive gallery */
        if (ui_key_take(ctx, SDLK_F2, 0)) {
            g->theme_req = true;
            g->want_dark = ui_get_theme(ctx)->kind != UI_THEME_DARK;
        }
        if (ui_key_take(ctx, SDLK_F3, 0)) g->show_dialog = true;
    }
}

/* ---- offscreen rendering ------------------------------------------------- */
static void push_mouse(ui_ctx *ctx, int type, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (type == 0) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.type = type > 0 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.clicks = 1;
        e.button.down = type > 0;
        e.button.x = x;
        e.button.y = y;
    }
    ui_event(ctx, &e);
}

static void run_frame(ui_ctx *ctx, ui_gallery *g, int w, int h, float scale, uint64_t t)
{
    ui_frame_info fi;
    fi.width = w;
    fi.height = h;
    fi.scale = scale;
    fi.px_per_point = 1.0f;
    fi.time_ms = t;
    ui_begin_frame(ctx, &fi);
    ui_gallery_frame(g, ctx);
    ui_end_frame(ctx);
}

bool ui_gallery_render_file(const char *path, int w, int h, float scale, bool dark, int scene,
                            uint64_t *hash)
{
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    SDL_Renderer *r = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
    ui_ctx *ctx = r ? ui_create(r, NULL) : NULL;
    ui_gallery *g = ctx ? ui_gallery_create(r) : NULL;
    ui_theme th;
    bool ok = false;
    uint64_t t = 1000;
    if (!g) goto out;
    ui_theme_init(&th, dark ? UI_THEME_DARK : UI_THEME_LIGHT, ui_theme_default_accent());
    ui_set_theme(ctx, &th);
    ui_gallery_set_scene(g, scene);
    for (int i = 0; i < 3; i++) run_frame(ctx, g, w, h, scale, t += 16);
    if (scene == UI_GALLERY_SCENE_MENU) {
        /* click "File", then rest on "Open Recent" until its submenu opens */
        float fx = (float)ui_px(ctx, 24.0f), fy = (float)ui_px(ctx, 15.0f);
        push_mouse(ctx, 0, fx, fy);
        push_mouse(ctx, 1, fx, fy);
        push_mouse(ctx, -1, fx, fy);
        for (int i = 0; i < 3; i++) run_frame(ctx, g, w, h, scale, t += 16);
        {
            float iy = fy + (float)ui_px(ctx, 15.0f) + (float)ui_px(ctx, 6.0f) +
                       2.5f * (float)ui_px(ctx, ui_get_theme(ctx)->m.menu_item_h);
            push_mouse(ctx, 0, fx + (float)ui_px(ctx, 40.0f), iy);
            for (int i = 0; i < 3; i++) run_frame(ctx, g, w, h, scale, t += 200);
            push_mouse(ctx, 0, fx + (float)ui_px(ctx, 41.0f), iy);
            for (int i = 0; i < 4; i++) run_frame(ctx, g, w, h, scale, t += 16);
        }
    } else {
        for (int i = 0; i < 2; i++) run_frame(ctx, g, w, h, scale, t += 16);
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    ui_render(ctx);
    SDL_RenderPresent(r);
    SDL_FlushRenderer(r);
    if (hash) {
        uint64_t hv = 1469598103934665603ull;
        if (SDL_LockSurface(surf)) {
            for (int y = 0; y < h; y++) {
                const uint8_t *row =
                    (const uint8_t *)surf->pixels + (size_t)y * (size_t)surf->pitch;
                for (int x = 0; x < w * 4; x++) { hv ^= row[x]; hv *= 1099511628211ull; }
            }
            SDL_UnlockSurface(surf);
        }
        *hash = hv;
    }
    ok = path ? SDL_SaveBMP(surf, path) : true;
out:
    ui_gallery_destroy(g);
    ui_destroy(ctx);
    if (r) SDL_DestroyRenderer(r);
    if (surf) SDL_DestroySurface(surf);
    return ok;
}
