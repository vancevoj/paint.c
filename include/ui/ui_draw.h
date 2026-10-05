/* ui_draw.h - batched 2D drawing over SDL_Renderer (lane L3).
 *
 * Draw calls append triangles to the draw list of the current layer of a
 * ui_ctx (base, floating panels, dialogs, popups, tooltip). Nothing reaches
 * the renderer until ui_render() replays the layers in z order, merging
 * consecutive triangles that share a texture and clip rectangle into one
 * SDL_RenderGeometry call. Only the SDL 3.2 renderer API is used, so every
 * backend (Direct3D, Metal, Vulkan, OpenGL, software) draws the same thing.
 *
 * Crispness: rectangles are integer device pixels. Rounded corners,
 * circles, rings and shadows are drawn from coverage sprites rasterized on
 * the CPU with exact area antialiasing and cached in the glyph atlas, so
 * they look identical on every backend, including the software renderer.
 * Lines and polygons at arbitrary angles use one pixel feathered edges.
 *
 * Thread rules: main thread (the thread that owns the SDL_Renderer), between
 * ui_begin_frame and ui_end_frame. Textures passed in are borrowed and must
 * stay alive until ui_render has run for the frame.
 */
#ifndef UI_DRAW_H
#define UI_DRAW_H

#include "ui_base.h"
#include "ui_font.h"
#include "ui_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Renderer;
struct SDL_Texture;

typedef struct ui_ctx ui_ctx;

typedef enum ui_filter { UI_FILTER_NEAREST = 0, UI_FILTER_LINEAR = 1 } ui_filter;

typedef struct ui_corners { float tl, tr, br, bl; } ui_corners;   /* radii in px */

static inline ui_corners ui_corners_all(float r)
{
    ui_corners c;
    c.tl = r; c.tr = r; c.br = r; c.bl = r;
    return c;
}

/* ---- clipping (intersecting stack, at most 64 deep) ---------------------- */
void    ui_push_clip(ui_ctx *ctx, ui_rect r);
void    ui_pop_clip(ui_ctx *ctx);
ui_rect ui_current_clip(const ui_ctx *ctx);

/* ---- rectangles ---------------------------------------------------------- */
void ui_draw_rect(ui_ctx *ctx, ui_rect r, ui_color c);
/* Outline inside r, thickness >= 1 px. */
void ui_draw_rect_outline(ui_ctx *ctx, ui_rect r, int32_t thickness, ui_color c);
/* Radii are clamped to half the shorter side. */
void ui_draw_rrect(ui_ctx *ctx, ui_rect r, float radius, ui_color c);
void ui_draw_rrect_outline(ui_ctx *ctx, ui_rect r, float radius, int32_t thickness,
                           ui_color c);
void ui_draw_rrect_ex(ui_ctx *ctx, ui_rect r, ui_corners radii, ui_color c);
void ui_draw_rrect_outline_ex(ui_ctx *ctx, ui_rect r, ui_corners radii, int32_t thickness,
                              ui_color c);
/* Soft shadow of the rounded rectangle r spreading blur px outwards. */
void ui_draw_shadow(ui_ctx *ctx, ui_rect r, float radius, float blur, ui_color c);
/* Four corner colors, interpolated across the rectangle. */
void ui_draw_gradient(ui_ctx *ctx, ui_rect r, ui_color tl, ui_color tr, ui_color br,
                      ui_color bl);
/* Checkerboard of cell x cell px squares, aligned to r's top left. */
void ui_draw_checker(ui_ctx *ctx, ui_rect r, int32_t cell, ui_color a, ui_color b);

/* ---- strokes and shapes (float pixel coordinates, pixel centers at .5) ---- */
void ui_draw_line(ui_ctx *ctx, ui_vec2 a, ui_vec2 b, float width, ui_color c);
void ui_draw_polyline(ui_ctx *ctx, const ui_vec2 *pts, int n, bool closed, float width,
                      ui_color c);
/* Antialiased convex polygon (any winding). */
void ui_draw_convex(ui_ctx *ctx, const ui_vec2 *pts, int n, ui_color c);
void ui_draw_triangle(ui_ctx *ctx, ui_vec2 a, ui_vec2 b, ui_vec2 c, ui_color col);
void ui_draw_circle(ui_ctx *ctx, ui_vec2 center, float radius, ui_color c);
void ui_draw_circle_outline(ui_ctx *ctx, ui_vec2 center, float radius, float width,
                            ui_color c);
/* Arc from angle a0 to a1 (radians, 0 = +x, increasing clockwise on screen). */
void ui_draw_arc(ui_ctx *ctx, ui_vec2 center, float radius, float a0, float a1, float width,
                 ui_color c);

/* ---- images -------------------------------------------------------------- */
/* Draw src (texel rectangle, NULL = whole texture) of tex into dst, tinted
 * (white = unchanged). The texture's blend mode is used as set by the app;
 * the scale mode is set to f while drawing. */
void ui_draw_image(ui_ctx *ctx, struct SDL_Texture *tex, const ui_rect *src, ui_rect dst,
                   ui_filter f, ui_color tint);

/* ---- text ---------------------------------------------------------------- */
/* Single line from pen position x on baseline y. Returns the pen x after
 * the text. Control characters are skipped. */
float ui_draw_text(ui_ctx *ctx, ui_font *font, float size_px, float x, float baseline,
                   ui_color c, const char *s, size_t len);

#define UI_TEXT_ELLIPSIS 1u    /* cut with "..." (U+2026) when too wide */
#define UI_TEXT_TOP      2u    /* align to the top instead of centering */
/* Single line inside r, horizontally aligned (UI_ALIGN_*) and vertically
 * centered on the cap height. Clipped to r. */
void ui_draw_text_box(ui_ctx *ctx, ui_font *font, float size_px, ui_rect r, int align,
                      uint32_t flags, ui_color c, const char *s, size_t len);

/* ---- icons --------------------------------------------------------------- */
/* Icon of size_px centered in r. soft is the accent at 30 % opacity. */
void ui_draw_icon(ui_ctx *ctx, ui_icon icon, ui_rect r, int32_t size_px, ui_color line,
                  ui_color accent);
void ui_draw_icon_ex(ui_ctx *ctx, ui_icon icon, ui_rect r, int32_t size_px, ui_color line,
                     ui_color accent, ui_color soft);

/* ---- custom rendering ---------------------------------------------------- */
/* fn runs during ui_render at this position of the draw order, with the
 * renderer's clip rectangle set to clip. It may issue any SDL render calls
 * (the canvas draws its tile textures this way) and must leave the render
 * target, viewport and scale unchanged. */
typedef void (*ui_draw_fn)(struct SDL_Renderer *r, ui_rect clip, void *ud);
void ui_draw_callback(ui_ctx *ctx, ui_draw_fn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* UI_DRAW_H */
