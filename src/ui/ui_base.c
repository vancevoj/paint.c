/* ui_base.c - rectangles, colors, hashing and UTF-8 helpers. */
#include "ui/ui_base.h"

#include <math.h>

/* ---- rectangles ---------------------------------------------------------- */
static int64_t imin64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t imax64(int64_t a, int64_t b) { return a > b ? a : b; }

bool ui_rect_contains(ui_rect r, float x, float y)
{
    return x >= (float)r.x && y >= (float)r.y && x < (float)r.x + (float)r.w &&
           y < (float)r.y + (float)r.h;
}

ui_rect ui_rect_intersect(ui_rect a, ui_rect b)
{
    int64_t x0 = imax64(a.x, b.x), y0 = imax64(a.y, b.y);
    int64_t x1 = imin64((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    int64_t y1 = imin64((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    if (ui_rect_empty(a) || ui_rect_empty(b) || x1 <= x0 || y1 <= y0)
        return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

ui_rect ui_rect_union(ui_rect a, ui_rect b)
{
    int64_t x0, y0, x1, y1;
    if (ui_rect_empty(a)) return ui_rect_empty(b) ? ui_rect_make(0, 0, 0, 0) : b;
    if (ui_rect_empty(b)) return a;
    x0 = imin64(a.x, b.x); y0 = imin64(a.y, b.y);
    x1 = imax64((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    y1 = imax64((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    if (x1 - x0 > INT32_MAX) x1 = x0 + INT32_MAX;
    if (y1 - y0 > INT32_MAX) y1 = y0 + INT32_MAX;
    return ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

ui_rect ui_rect_inset(ui_rect r, int32_t dx, int32_t dy)
{
    r.x += dx; r.y += dy;
    r.w -= 2 * dx; r.h -= 2 * dy;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

ui_rect ui_rect_offset(ui_rect r, int32_t dx, int32_t dy)
{
    r.x += dx; r.y += dy;
    return r;
}

ui_rect ui_rect_center(ui_rect r, int32_t w, int32_t h)
{
    return ui_rect_make(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
}

static int32_t clampn(int32_t n, int32_t avail)
{
    if (n < 0) return 0;
    return n > avail ? (avail > 0 ? avail : 0) : n;
}

ui_rect ui_cut_left(ui_rect *r, int32_t n)
{
    ui_rect out;
    n = clampn(n, r->w);
    out = ui_rect_make(r->x, r->y, n, r->h);
    r->x += n; r->w -= n;
    return out;
}

ui_rect ui_cut_right(ui_rect *r, int32_t n)
{
    n = clampn(n, r->w);
    r->w -= n;
    return ui_rect_make(r->x + r->w, r->y, n, r->h);
}

ui_rect ui_cut_top(ui_rect *r, int32_t n)
{
    ui_rect out;
    n = clampn(n, r->h);
    out = ui_rect_make(r->x, r->y, r->w, n);
    r->y += n; r->h -= n;
    return out;
}

ui_rect ui_cut_bottom(ui_rect *r, int32_t n)
{
    n = clampn(n, r->h);
    r->h -= n;
    return ui_rect_make(r->x, r->y + r->h, r->w, n);
}

/* ---- colors -------------------------------------------------------------- */
static uint8_t lerp8(uint8_t a, uint8_t b, float t)
{
    float v = (float)a + ((float)b - (float)a) * t;
    return (uint8_t)(v + 0.5f);
}

ui_color ui_color_lerp(ui_color a, ui_color b, float t)
{
    if (!(t > 0.0f)) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return ui_rgba(lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t),
                   lerp8(a.a, b.a, t));
}

ui_color ui_color_fade(ui_color c, float f)
{
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    c.a = (uint8_t)((float)c.a * f + 0.5f);
    return c;
}

ui_color ui_color_over(ui_color c, ui_color under)
{
    float t = (float)c.a / 255.0f;
    ui_color o = ui_color_lerp(under, c, t);
    o.a = 255u;
    return o;
}

ui_hsv ui_rgb_to_hsv(ui_color c)
{
    float r = (float)c.r / 255.0f, g = (float)c.g / 255.0f, b = (float)c.b / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    ui_hsv o;
    o.v = mx;
    o.s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        o.h = 0.0f;
    } else if (mx == r) {
        o.h = 60.0f * fmodf((g - b) / d, 6.0f);
    } else if (mx == g) {
        o.h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        o.h = 60.0f * ((r - g) / d + 4.0f);
    }
    if (o.h < 0.0f) o.h += 360.0f;
    if (o.h >= 360.0f) o.h -= 360.0f;
    return o;
}

static uint8_t unit8(float v)
{
    if (!(v > 0.0f)) return 0u;
    if (v >= 1.0f) return 255u;
    return (uint8_t)(v * 255.0f + 0.5f);
}

ui_color ui_hsv_to_rgb(ui_hsv hsv, uint8_t a)
{
    float h = fmodf(hsv.h, 360.0f), s = hsv.s, v = hsv.v, c, x, m, r, g, b;
    int sector;
    if (h < 0.0f) h += 360.0f;
    if (!(s > 0.0f)) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    c = v * s;
    x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    m = v - c;
    sector = (int)(h / 60.0f);
    switch (sector) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    return ui_rgba(unit8(r + m), unit8(g + m), unit8(b + m), a);
}

/* ---- ids ----------------------------------------------------------------- */
ui_id ui_hash(const void *data, ptrdiff_t len, ui_id seed)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t h = 2166136261u ^ (seed * 16777619u);
    if (len < 0) {
        while (*p) { h ^= *p++; h *= 16777619u; }
    } else {
        for (ptrdiff_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    }
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return h ? h : 1u;
}

/* ---- UTF-8 --------------------------------------------------------------- */
uint32_t ui_utf8_decode(const char *str, size_t len, size_t *i)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t k = *i;
    uint32_t c, cp;
    unsigned need, lo = 0x80u, hi = 0xBFu;
    if (k >= len) { *i = len; return UI_UTF8_REPLACEMENT; }
    c = s[k];
    if (c < 0x80u) { *i = k + 1; return c; }
    if (c >= 0xC2u && c <= 0xDFu) { need = 1; cp = c & 0x1Fu; }
    else if (c >= 0xE0u && c <= 0xEFu) {
        need = 2; cp = c & 0x0Fu;
        if (c == 0xE0u) lo = 0xA0u;         /* no overlongs */
        if (c == 0xEDu) hi = 0x9Fu;         /* no surrogates */
    } else if (c >= 0xF0u && c <= 0xF4u) {
        need = 3; cp = c & 0x07u;
        if (c == 0xF0u) lo = 0x90u;
        if (c == 0xF4u) hi = 0x8Fu;         /* <= U+10FFFF */
    } else {
        *i = k + 1;                          /* stray continuation or invalid lead */
        return UI_UTF8_REPLACEMENT;
    }
    k++;
    for (unsigned n = 0; n < need; n++) {
        unsigned b;
        if (k >= len) { *i = k; return UI_UTF8_REPLACEMENT; }
        b = s[k];
        if (b < lo || b > hi) { *i = k; return UI_UTF8_REPLACEMENT; }
        lo = 0x80u; hi = 0xBFu;
        cp = (cp << 6) | (b & 0x3Fu);
        k++;
    }
    *i = k;
    return cp;
}

int ui_utf8_encode(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = UI_UTF8_REPLACEMENT;
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

size_t ui_utf8_next(const char *s, size_t len, size_t i)
{
    if (i >= len) return len;
    (void)ui_utf8_decode(s, len, &i);
    return i;
}

/* Moves back to the start of the code point that ends at i, consistent with
 * the forward decoder (malformed bytes are single units). */
size_t ui_utf8_prev(const char *s, size_t i)
{
    size_t start, k;
    if (i == 0) return 0;
    start = i > 4 ? i - 4 : 0;
    /* Decode forward from a safe point and keep the last boundary < i. */
    for (;;) {
        size_t last = start;
        k = start;
        while (k < i) {
            last = k;
            (void)ui_utf8_decode(s, i, &k);
        }
        if (k == i) return last;
        /* start was inside a sequence that overran i; retry one byte later */
        start++;
        if (start >= i) return i - 1;
    }
}

size_t ui_utf8_count(const char *s, size_t len)
{
    size_t i = 0, n = 0;
    while (i < len) { (void)ui_utf8_decode(s, len, &i); n++; }
    return n;
}

size_t ui_utf8_floor(const char *s, size_t len, size_t cap)
{
    size_t i = 0, last = 0;
    if (cap >= len) return len;
    while (i < len) {
        size_t k = i;
        (void)ui_utf8_decode(s, len, &k);
        if (k > cap) break;
        last = k;
        i = k;
    }
    return last;
}
