/* ui_gallery.h - the pc_ui widget gallery (lane L3).
 *
 * A small editor-like window that shows every widget, used interactively
 * (paintc_ui_gallery) and for offscreen screenshot tests (tests/ui). Main
 * thread only.
 */
#ifndef UI_GALLERY_H
#define UI_GALLERY_H

#include "ui/ui.h"

struct SDL_Renderer;

typedef struct ui_gallery ui_gallery;

enum {
    UI_GALLERY_SCENE_MAIN = 0,    /* everything at rest */
    UI_GALLERY_SCENE_MENU = 1,    /* File menu with a submenu open */
    UI_GALLERY_SCENE_DIALOG = 2,  /* modal dialog over the window */
    UI_GALLERY_SCENE_ICONS = 3,   /* the icon set at several sizes */
    UI_GALLERY_SCENE_COUNT
};

/* Creates thumbnail textures with r (borrowed). NULL on OOM. */
ui_gallery *ui_gallery_create(struct SDL_Renderer *r);
void        ui_gallery_destroy(ui_gallery *g);           /* NULL-safe */
void        ui_gallery_set_scene(ui_gallery *g, int scene);
/* True when the user picked a different theme in the gallery's View menu;
 * *dark receives the choice. */
bool        ui_gallery_theme_request(ui_gallery *g, bool *dark);
/* Declare one frame of the gallery UI (between ui_begin_frame and
 * ui_end_frame). */
void        ui_gallery_frame(ui_gallery *g, ui_ctx *ctx);

/* Render scene at w x h pixels with the given scale and theme into a BMP
 * using the software renderer (no window, no video driver needed besides
 * SDL's surface functions). Runs a few frames so popups measure themselves.
 * Writes the 32-bit pixels' FNV-1a hash to *hash when not NULL. */
bool ui_gallery_render_file(const char *path, int w, int h, float scale, bool dark, int scene,
                            uint64_t *hash);

#endif /* UI_GALLERY_H */
