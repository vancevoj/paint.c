/* sel_cursor.c - cursors the tool framework draws itself (lane TOOLA, see
 * app_tool.h app_tool_cursor_make): the closed hand of the Pan tool while
 * dragging (TOOLS.md 1: "open hand, closed hand while dragging") and the
 * selection tool cursors with the selection mode glyph (TOOLS.md 1:
 * "crosshair with selection-mode glyph"; the lasso and the wand show the
 * glyph for the combine modes too, as the 3.36 tools did with their plus
 * and minus cursors). The drawings are paint.c's own: shapes from signed
 * distance functions sampled 4 x 4 per pixel, a white halo around dark
 * strokes so the cursor shows on any image, and the toolbar's selection
 * mode icons (ui_icons) as glyphs. Lane TOOLS (wave 4): the Paint Bucket
 * cursor, whose hotspot is the paint drop.
 *
 * Thread rules: app_tool_cursor_rgba and app_cursor_sel_mode run on any
 * thread; app_tool_cursor_make on the main thread (SDL cursors).
 * Ownership: rgba buffers are the caller's; app_tool_cursor_make returns
 * an owned SDL cursor (SDL_DestroyCursor). */
#include "../app_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CUR_MAX 128

/* ---- tiny straight-alpha canvas ------------------------------------------------------- */
static void put_over(uint8_t *px, uint8_t r, uint8_t g, uint8_t b, float cov)
{
    float sa = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov), da = (float)px[3] / 255.0f;
    float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) return;
    px[0] = (uint8_t)lroundf(((float)r * sa + (float)px[0] * da * (1.0f - sa)) / oa);
    px[1] = (uint8_t)lroundf(((float)g * sa + (float)px[1] * da * (1.0f - sa)) / oa);
    px[2] = (uint8_t)lroundf(((float)b * sa + (float)px[2] * da * (1.0f - sa)) / oa);
    px[3] = (uint8_t)lroundf(oa * 255.0f);
}

/* Signed distances (negative inside) in drawing units. */
static float sd_disk(float x, float y, float cx, float cy, float r)
{
    return hypotf(x - cx, y - cy) - r;
}

static float sd_capsule(float x, float y, float ax, float ay, float bx, float by, float r)
{
    float px = x - ax, py = y - ay, vx = bx - ax, vy = by - ay;
    float vv = vx * vx + vy * vy, t = vv > 0.0f ? (px * vx + py * vy) / vv : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return hypotf(px - vx * t, py - vy * t) - r;
}

static float sd_rrect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
    float in = qx > qy ? qx : qy;
    return hypotf(ox, oy) + (in < 0.0f ? in : 0.0f) - r;
}

/* The closed hand in a 16 x 16 design grid: palm, four folded fingers and
 * the thumb (union), plus the finger gaps as strokes. */
static float sd_fist(float x, float y)
{
    static const float knuckle_x[4] = { 5.0f, 7.5f, 10.0f, 12.4f };
    float d = sd_rrect(x, y, 8.6f, 10.6f, 4.9f, 3.9f, 2.6f);
    for (int i = 0; i < 4; i++) {
        float k = sd_disk(x, y, knuckle_x[i], 6.9f, 1.55f);
        if (k < d) d = k;
    }
    {
        float t = sd_capsule(x, y, 2.7f, 9.2f, 5.0f, 12.0f, 1.3f);
        if (t < d) d = t;
    }
    return d;
}

static float sd_fist_gaps(float x, float y)
{
    float d = 1e9f;
    for (int i = 0; i < 3; i++) {
        float gx = 6.25f + 2.47f * (float)i;
        float g = sd_capsule(x, y, gx, 6.4f, gx, 8.7f, 0.22f);
        if (g < d) d = g;
    }
    return d;
}

/* 4 x 4 samples per pixel of the hand: fill, outline and halo coverage. */
static void draw_fist(uint8_t *rgba, int size)
{
    float k = 16.0f / (float)size;           /* design units per pixel */
    float line = 0.95f, halo = 0.9f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int nf = 0, nl = 0, nh = 0;
            uint8_t *px = rgba + ((size_t)y * (size_t)size + (size_t)x) * 4u;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float ux = ((float)x + ((float)sx + 0.5f) / 4.0f) * k;
                    float uy = ((float)y + ((float)sy + 0.5f) / 4.0f) * k;
                    float d = sd_fist(ux, uy), g = sd_fist_gaps(ux, uy);
                    if (d <= 0.0f && g > 0.0f) nf++;
                    else if (d <= line * k * 1.25f || g <= 0.0f) nl++;
                    else if (d <= (line + halo) * k * 1.25f) nh++;
                }
            put_over(px, 255, 255, 255, (float)nh / 16.0f * 0.85f);
            put_over(px, 255, 255, 255, (float)nf / 16.0f);
            put_over(px, 24, 24, 24, (float)nl / 16.0f);
        }
}

/* Thin crosshair with a gap at the hotspot (c, c), dark over a white halo. */
static void draw_cross(uint8_t *rgba, int size, int c)
{
    int arm = size * 9 / 24, gap = size >= 32 ? 2 : 1;
    uint8_t *mask = (uint8_t *)calloc((size_t)size * (size_t)size, 1u);
    if (!mask) return;
    for (int i = c - arm; i <= c + arm; i++) {
        if (i < 0 || i >= size || abs(i - c) <= gap) continue;
        mask[(size_t)c * (size_t)size + (size_t)i] = 1u;
        mask[(size_t)i * (size_t)size + (size_t)c] = 1u;
    }
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            bool by_line = false;
            if (mask[(size_t)y * (size_t)size + (size_t)x]) continue;
            for (int dy = -1; dy <= 1 && !by_line; dy++)
                for (int dx = -1; dx <= 1 && !by_line; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < size && yy < size &&
                        mask[(size_t)yy * (size_t)size + (size_t)xx])
                        by_line = true;
                }
            if (by_line) put_over(rgba + ((size_t)y * (size_t)size + (size_t)x) * 4u, 255, 255, 255,
                               0.9f);
        }
    for (size_t i = 0; i < (size_t)size * (size_t)size; i++)
        if (mask[i]) put_over(rgba + i * 4u, 16, 16, 16, 1.0f);
    free(mask);
}

/* An icon (dark strokes, accent parts in accent) composited over a white
 * halo, as the canvas draws its icon cursors. */
static bool draw_icon_halo_ex(uint8_t *rgba, int size, ui_icon icon, ui_color accent)
{
    uint8_t *src = (uint8_t *)malloc((size_t)size * (size_t)size * 4u);
    if (!src) return false;
    if (ui_icon_raster_rgba(icon, size, ui_rgba(16, 16, 16, 255), accent,
                            ui_rgba(255, 255, 255, 200), src) != PC_OK) {
        free(src);
        return false;
    }
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint8_t amax = 0;
            uint8_t *o = rgba + ((size_t)y * (size_t)size + (size_t)x) * 4u;
            const uint8_t *s = src + ((size_t)y * (size_t)size + (size_t)x) * 4u;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    uint8_t al;
                    if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
                    al = src[((size_t)yy * (size_t)size + (size_t)xx) * 4u + 3u];
                    if (al > amax) amax = al;
                }
            put_over(o, 255, 255, 255, (float)amax / 255.0f);
            put_over(o, s[0], s[1], s[2], (float)s[3] / 255.0f);
        }
    free(src);
    return true;
}

static bool draw_icon_halo(uint8_t *rgba, int size, ui_icon icon)
{
    return draw_icon_halo_ex(rgba, size, icon, ui_rgba(255, 255, 255, 255));
}

/* The selection mode glyph (the toolbar icon of the mode) on a white
 * rounded backing in the bottom right corner. */
static bool draw_mode_glyph(uint8_t *rgba, int size, int mode)
{
    static const ui_icon icons[5] = {
        UI_ICON_SEL_REPLACE, UI_ICON_SEL_UNION, UI_ICON_SEL_EXCLUDE, UI_ICON_SEL_INTERSECT,
        UI_ICON_SEL_XOR
    };
    int gs = size / 2, x0 = size - gs, y0 = size - gs;
    uint8_t *g;
    if (mode < 0 || mode > 4) return false;
    g = (uint8_t *)malloc((size_t)gs * (size_t)gs * 4u);
    if (!g) return false;
    if (ui_icon_raster_rgba(icons[mode], gs, ui_rgba(16, 16, 16, 255), ui_rgba(0, 102, 204, 255),
                            ui_rgba(0, 102, 204, 90), g) != PC_OK) {
        free(g);
        return false;
    }
    for (int y = 0; y < gs; y++)
        for (int x = 0; x < gs; x++) {
            uint8_t *o = rgba + ((size_t)(y0 + y) * (size_t)size + (size_t)(x0 + x)) * 4u;
            const uint8_t *s = g + ((size_t)y * (size_t)gs + (size_t)x) * 4u;
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f, h = (float)gs * 0.5f;
            float d = sd_rrect(fx, fy, h, h, h - 0.5f, h - 0.5f, (float)gs * 0.22f);
            if (d <= 0.0f) put_over(o, 255, 255, 255, d < -1.0f ? 0.92f : 0.92f * -d);
            put_over(o, s[0], s[1], s[2], (float)s[3] / 255.0f);
        }
    free(g);
    return true;
}

/* ---- public ------------------------------------------------------------------------ */
app_cursor app_cursor_sel_mode(app_cursor base, int mode)
{
    if (mode < 0 || mode > 4) mode = 0;
    if (base == APP_CURSOR_CROSSHAIR || (base >= APP_CURSOR_SEL_REPLACE &&
                                         base <= APP_CURSOR_SEL_XOR))
        return (app_cursor)((int)APP_CURSOR_SEL_REPLACE + mode);
    if (base == APP_CURSOR_LASSO || (base >= APP_CURSOR_LASSO_UNION &&
                                     base <= APP_CURSOR_LASSO_XOR))
        return mode == 0 ? APP_CURSOR_LASSO : (app_cursor)((int)APP_CURSOR_LASSO_UNION + mode - 1);
    if (base == APP_CURSOR_WAND || (base >= APP_CURSOR_WAND_UNION && base <= APP_CURSOR_WAND_XOR))
        return mode == 0 ? APP_CURSOR_WAND : (app_cursor)((int)APP_CURSOR_WAND_UNION + mode - 1);
    return base;
}

bool app_tool_cursor_rgba(app_cursor k, int size, uint8_t *rgba, int *hot_x, int *hot_y)
{
    int hx = size / 2, hy = size / 2;
    bool ok = true;
    if (!rgba || size < 16 || size > CUR_MAX) return false;
    memset(rgba, 0, (size_t)size * (size_t)size * 4u);
    if (k == APP_CURSOR_GRAB) {
        draw_fist(rgba, size);
    } else if (k >= APP_CURSOR_SEL_REPLACE && k <= APP_CURSOR_SEL_XOR) {
        hx = hy = size / 2 - 1;
        draw_cross(rgba, size, hx);
        ok = draw_mode_glyph(rgba, size, (int)k - (int)APP_CURSOR_SEL_REPLACE);
    } else if (k >= APP_CURSOR_LASSO_UNION && k <= APP_CURSOR_LASSO_XOR) {
        hx = size * 2 / 16;
        hy = size * 15 / 16 - 1;
        ok = draw_icon_halo(rgba, size, UI_ICON_TOOL_LASSO_SELECT) &&
             draw_mode_glyph(rgba, size, (int)k - (int)APP_CURSOR_LASSO_UNION + 1);
    } else if (k >= APP_CURSOR_WAND_UNION && k <= APP_CURSOR_WAND_XOR) {
        hx = size * 12 / 16;
        hy = size * 4 / 16;
        ok = draw_icon_halo(rgba, size, UI_ICON_TOOL_MAGIC_WAND) &&
             draw_mode_glyph(rgba, size, (int)k - (int)APP_CURSOR_WAND_UNION + 1);
    } else if (k == APP_CURSOR_BUCKET) {
        /* lane TOOLS (wave 4 item 22): the fill starts where the paint
         * drop lands, so the hotspot is the bottom of the drop (icon units
         * 14, 13 of 16; the drop is a circle of radius 1.6 around 14, 12.2)
         * and the drop is drawn in a color that shows on white images */
        hx = size * 14 / 16;
        hy = size * 13 / 16;
        ok = draw_icon_halo_ex(rgba, size, UI_ICON_TOOL_PAINT_BUCKET, ui_rgba(0, 102, 204, 255));
    } else {
        return false;
    }
    if (hot_x) *hot_x = hx;
    if (hot_y) *hot_y = hy;
    return ok;
}

struct SDL_Cursor *app_tool_cursor_make(app *a, app_cursor k, int size)
{
    uint8_t *rgba;
    int hx = 0, hy = 0;
    SDL_Surface *s;
    SDL_Cursor *c = NULL;
    (void)a;
    if (size < 16 || size > CUR_MAX) return NULL;
    rgba = (uint8_t *)malloc((size_t)size * (size_t)size * 4u);
    if (!rgba) return NULL;
    if (!app_tool_cursor_rgba(k, size, rgba, &hx, &hy)) {
        free(rgba);
        return NULL;
    }
    s = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, rgba, size * 4);
    if (s) {
        c = SDL_CreateColorCursor(s, hx, hy);
        SDL_DestroySurface(s);
    }
    free(rgba);
    return c;
}
