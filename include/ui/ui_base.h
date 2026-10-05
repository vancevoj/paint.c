/* ui_base.h - basic value types of the paint.c UI toolkit (lane L3).
 *
 * Plain values only: integer device-pixel rectangles, float points, straight
 * alpha sRGB colors, widget ids and UTF-8 helpers. Every function here is a
 * pure function and may be called from any thread.
 *
 * Coordinates: the toolkit lays out and draws in device pixels (the
 * renderer's output pixels). DIP values (device independent pixels, 1/96
 * inch at 100 %) are converted with ui_px()/ui_dp() in ui.h, which round to
 * whole pixels so edges stay crisp at fractional scales.
 */
#ifndef UI_BASE_H
#define UI_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pc/pc_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PI 3.14159265358979323846f

/* ---- geometry ------------------------------------------------------------ */
typedef struct ui_rect { int32_t x, y, w, h; } ui_rect;   /* half-open */
typedef struct ui_vec2 { float x, y; } ui_vec2;

static inline ui_rect ui_rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    ui_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}
static inline ui_vec2 ui_vec2_make(float x, float y)
{
    ui_vec2 v;
    v.x = x; v.y = y;
    return v;
}
static inline bool ui_rect_empty(ui_rect r) { return r.w <= 0 || r.h <= 0; }
static inline int32_t ui_rect_right(ui_rect r) { return r.x + r.w; }
static inline int32_t ui_rect_bottom(ui_rect r) { return r.y + r.h; }

bool    ui_rect_contains(ui_rect r, float x, float y);
ui_rect ui_rect_intersect(ui_rect a, ui_rect b);   /* {0,0,0,0} when disjoint */
ui_rect ui_rect_union(ui_rect a, ui_rect b);       /* ignores empty inputs */
ui_rect ui_rect_inset(ui_rect r, int32_t dx, int32_t dy);  /* negative grows */
ui_rect ui_rect_offset(ui_rect r, int32_t dx, int32_t dy);
/* Center a w x h rectangle inside r (rounded toward the top left). */
ui_rect ui_rect_center(ui_rect r, int32_t w, int32_t h);

/* Rect cutting: remove a strip of size n from one side of *r and return it.
 * n is clamped to the available size, so the result never overflows *r. */
ui_rect ui_cut_left(ui_rect *r, int32_t n);
ui_rect ui_cut_right(ui_rect *r, int32_t n);
ui_rect ui_cut_top(ui_rect *r, int32_t n);
ui_rect ui_cut_bottom(ui_rect *r, int32_t n);

/* ---- colors (sRGB, straight alpha) --------------------------------------- */
typedef struct ui_color { uint8_t r, g, b, a; } ui_color;

static inline ui_color ui_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    ui_color c;
    c.r = r; c.g = g; c.b = b; c.a = a;
    return c;
}
/* 0xRRGGBB, opaque. */
static inline ui_color ui_rgb_hex(uint32_t rgb)
{
    return ui_rgba((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255u);
}
/* 0xAARRGGBB, the layout used by fx_abi.h FXP_COLOR. */
static inline ui_color ui_argb32(uint32_t argb)
{
    return ui_rgba((uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb,
                   (uint8_t)(argb >> 24));
}
static inline uint32_t ui_color_argb32(ui_color c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}
static inline bool ui_color_eq(ui_color a, ui_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Linear blend in sRGB space, t in [0, 1] (clamped). */
ui_color ui_color_lerp(ui_color a, ui_color b, float t);
/* Multiply alpha by f in [0, 1]. */
ui_color ui_color_fade(ui_color c, float f);
/* Composite c (with its alpha) over the opaque color under, result opaque. */
ui_color ui_color_over(ui_color c, ui_color under);

/* HSV in floats: h in [0, 360), s and v in [0, 1]. */
typedef struct ui_hsv { float h, s, v; } ui_hsv;
ui_hsv   ui_rgb_to_hsv(ui_color c);           /* alpha ignored */
ui_color ui_hsv_to_rgb(ui_hsv hsv, uint8_t a);

/* ---- widget ids ---------------------------------------------------------- */
typedef uint32_t ui_id;   /* 0 means "no widget" */

/* FNV-1a over len bytes (len < 0: NUL-terminated), mixed with seed. Never
 * returns 0. */
ui_id ui_hash(const void *data, ptrdiff_t len, ui_id seed);

/* ---- UTF-8 --------------------------------------------------------------- */
#define UI_UTF8_REPLACEMENT 0xFFFDu

/* Decode one code point starting at s[*i] (i < len). Advances *i by at
 * least one byte. Malformed sequences (overlong forms, surrogates, values
 * above U+10FFFF, truncated or stray continuation bytes) yield U+FFFD and
 * consume the maximal invalid subpart, as recommended by Unicode. */
uint32_t ui_utf8_decode(const char *s, size_t len, size_t *i);
/* Encode cp (invalid values become U+FFFD) into out; returns 1..4. */
int      ui_utf8_encode(uint32_t cp, char out[4]);
/* Byte offset of the code point boundary after/before i (clamped). */
size_t   ui_utf8_next(const char *s, size_t len, size_t i);
size_t   ui_utf8_prev(const char *s, size_t i);
/* Number of code points in s[0, len). */
size_t   ui_utf8_count(const char *s, size_t len);
/* Largest boundary <= cap in s[0, len) (for truncating into buffers). */
size_t   ui_utf8_floor(const char *s, size_t len, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* UI_BASE_H */
