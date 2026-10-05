/* test_toola_select.c - lane TOOLA: the selection tools.
 *   - T-SEL-QUALITY: antialiased ellipses and lassos are 4 x 4
 *     supersampled: every pixel equals a brute-force count of 16 sample
 *     points inside the same polygon (17 levels), pixelated stays hard;
 *   - T-SEL-STATUS: the status bar shows offset, size and area while
 *     dragging, in the current units;
 *   - F-TOOL-RECTANGLE-SELECT, F-TOOL-ELLIPSE-SELECT: crosshair cursors
 *     with the selection mode glyph (toolbar mode, Ctrl adds, Alt
 *     subtracts, fixed during a drag); the lasso and the wand show the
 *     glyph for combine modes; the cursor images exist, differ per mode
 *     and have their hotspot on the drawing; the Pan tool's closed hand
 *     (F-TOOL-PAN). The images are written to the build directory. */
#include "pc_test.h"
#include "a_util.h"
#include "pc/pc_path.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

/* Nonzero (or even-odd) winding of the polygon around (x, y). */
static int winding(const pc_poly *p, double x, double y)
{
    int w = 0;
    size_t start = 0;
    for (size_t c = 0; c < p->n_contours; c++) {
        size_t end = p->ends[c], n = end - start;
        for (size_t i = 0; i < n && n >= 2u; i++) {
            pc_pt a = p->pts[start + i], b = p->pts[start + (i + 1u) % n];
            if ((a.y <= y) != (b.y <= y)) {
                double t = (y - a.y) / (b.y - a.y), xc = a.x + (b.x - a.x) * t;
                if (xc <= x) w += b.y > a.y ? 1 : -1;
            }
        }
        start = end;
    }
    return w;
}

static uint8_t brute(const pc_poly *p, int32_t x, int32_t y, bool evenodd)
{
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int w = winding(p, (double)x + ((double)i + 0.5) / 4.0,
                            (double)y + ((double)j + 0.5) / 4.0);
            if (evenodd ? (w & 1) != 0 : w != 0) n++;
        }
    return (uint8_t)(((uint32_t)n * 255u + 8u) / 16u);
}

static bool level(uint8_t c)
{
    for (uint32_t n = 0; n <= 16u; n++)
        if ((uint8_t)((n * 255u + 8u) / 16u) == c) return true;
    return false;
}

static void t_supersampled(void)
{
    app *a = a_app(200, 150, WHITE);
    pc_poly poly;
    pc_path path;
    long bad = 0, part = 0, odd = 0;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "ellipse_select"));
    a->ts.sel_clip_aa = true;
    a_drag(a, 10, 10, 110, 70, SDL_BUTTON_LEFT, 0u);
    /* the polygon the tool builds (sel_marquee.c) */
    pc_poly_init(&poly);
    pc_path_init(&path);
    CHECK(pc_path_add_ellipse(&path, 60.0, 40.0, 50.0, 30.0) == PC_OK);
    CHECK(pc_path_flatten(&path, NULL, 0.02, &poly) == PC_OK);
    for (int32_t y = 0; y < 150; y++)
        for (int32_t x = 0; x < 200; x++) {
            uint8_t c = a_cov(a, x, y);
            if (c != brute(&poly, x, y, false)) bad++;
            if (c && c != 255u) part++;
            if (!level(c)) odd++;
        }
    CHECK(bad == 0 && part > 100 && odd == 0);
    pc_path_free(&path);
    pc_poly_free(&poly);
    /* a lasso crossing itself: even-odd, supersampled */
    CHECK(app_tool_select(a, "lasso_select"));
    {
        static const double pts[][2] = {
            { 20.3, 20.7 }, { 120.6, 30.2 }, { 30.1, 110.9 }, { 140.4, 100.2 }, { 20.3, 20.7 }
        };
        pc_poly lp;
        a_down(a, pts[0][0], pts[0][1], SDL_BUTTON_LEFT);
        for (size_t i = 1; i < 5; i++) {
            a_move(a, pts[i][0], pts[i][1]);
            at_frames(a, 1);
        }
        a_up(a, pts[4][0], pts[4][1], SDL_BUTTON_LEFT);
        pc_poly_init(&lp);
        for (size_t i = 0; i < 5; i++)
            CHECK(pc_poly_add(&lp, pc_pt_make(pts[i][0], pts[i][1]), 0u) == PC_OK);
        CHECK(pc_poly_end(&lp, true) == PC_OK);
        bad = part = 0;
        for (int32_t y = 0; y < 150; y++)
            for (int32_t x = 0; x < 200; x++) {
                uint8_t c = a_cov(a, x, y);
                if (c != brute(&lp, x, y, true)) bad++;
                if (c && c != 255u) part++;
            }
        CHECK(bad == 0 && part > 50);
        pc_poly_free(&lp);
    }
    /* pixelated: hard edges */
    CHECK(app_tool_select(a, "ellipse_select"));
    a->ts.sel_clip_aa = false;
    a_drag(a, 10, 10, 110, 70, SDL_BUTTON_LEFT, 0u);
    {
        uint64_t full, p2;
        a_count(a, pc_rect_make(0, 0, 200, 150), &full, &p2);
        CHECK(p2 == 0u && full > 4000u);
    }
    a->ts.sel_clip_aa = true;
    app_destroy(a);
}

static void t_status_area(void)
{
    app *a = a_app(200, 150, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    a_down(a, 20, 20, SDL_BUTTON_LEFT);
    a_move(a, 60, 50);
    at_frames(a, 2);
    CHECK(strstr(a->status, "Offset 20, 20") != NULL);
    CHECK(strstr(a->status, "Size 40 \xC3\x97 30") != NULL);
    CHECK(strstr(a->status, "Area 1200 px\xC2\xB2") != NULL);
    /* clipped to the image */
    a_move(a, 260, 50);
    at_frames(a, 2);
    CHECK(strstr(a->status, "Area 5400 px\xC2\xB2") != NULL);
    /* inches at 96 DPI */
    app_set_units(a, APP_UNITS_IN);
    a_move(a, 116, 116);
    at_frames(a, 2);
    CHECK(strstr(a->status, "Area 1.00 in\xC2\xB2") != NULL);
    app_set_units(a, APP_UNITS_PX);
    a_up(a, 116, 116, SDL_BUTTON_LEFT);
    /* ellipse: close to pi a b */
    CHECK(app_tool_select(a, "ellipse_select"));
    a_down(a, 10, 10, SDL_BUTTON_LEFT);
    a_move(a, 110, 70);
    at_frames(a, 2);
    {
        const char *p = strstr(a->status, "Area ");
        double v = p ? atof(p + 5) : 0.0;
        CHECK(p != NULL && fabs(v - 3.14159265 * 50.0 * 30.0) < 6.0);
    }
    a_up(a, 110, 70, SDL_BUTTON_LEFT);
    app_destroy(a);
}

/* The cursor the canvas asks for over document point (x, y). */
static app_cursor cursor_over(app *a, double x, double y)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
    return a->cv.cursor;
}

static void t_cursors(void)
{
    app *a = a_app(200, 150, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    a->ts.sel_mode = PC_SEL_REPLACE;
    CHECK(app_tool_select(a, "rect_select"));
    CHECK(cursor_over(a, 50, 50) == APP_CURSOR_SEL_REPLACE);
    a_mods(a, a_ctrl());
    CHECK(cursor_over(a, 51, 50) == APP_CURSOR_SEL_UNION);
    a_mods(a, 0u);
    a_mods(a, UI_MOD_ALT);
    CHECK(cursor_over(a, 52, 50) == APP_CURSOR_SEL_EXCLUDE);
    /* the mode of a drag stays while the modifier is released */
    a_down(a, 52, 50, SDL_BUTTON_LEFT);
    a_mods(a, 0u);
    CHECK(cursor_over(a, 70, 60) == APP_CURSOR_SEL_EXCLUDE);
    a_up(a, 70, 60, SDL_BUTTON_LEFT);
    a->ts.sel_mode = PC_SEL_INTERSECT;
    CHECK(cursor_over(a, 50, 50) == APP_CURSOR_SEL_INTERSECT);
    CHECK(app_tool_select(a, "ellipse_select"));
    CHECK(cursor_over(a, 51, 50) == APP_CURSOR_SEL_INTERSECT);
    a->ts.sel_mode = PC_SEL_REPLACE;
    CHECK(cursor_over(a, 50, 50) == APP_CURSOR_SEL_REPLACE);
    CHECK(app_tool_select(a, "lasso_select"));
    CHECK(cursor_over(a, 51, 50) == APP_CURSOR_LASSO);
    a_mods(a, a_ctrl());
    CHECK(cursor_over(a, 52, 50) == APP_CURSOR_LASSO_UNION);
    a_mods(a, 0u);
    CHECK(app_tool_select(a, "magic_wand"));
    CHECK(cursor_over(a, 51, 50) == APP_CURSOR_WAND);
    a_mods(a, UI_MOD_ALT);
    CHECK(cursor_over(a, 52, 50) == APP_CURSOR_WAND_EXCLUDE);
    a_mods(a, 0u);
    /* the mapping */
    CHECK(app_cursor_sel_mode(APP_CURSOR_CROSSHAIR, PC_SEL_XOR) == APP_CURSOR_SEL_XOR);
    CHECK(app_cursor_sel_mode(APP_CURSOR_WAND_UNION, PC_SEL_REPLACE) == APP_CURSOR_WAND);
    CHECK(app_cursor_sel_mode(APP_CURSOR_LASSO, PC_SEL_INTERSECT) == APP_CURSOR_LASSO_INTERSECT);
    CHECK(app_cursor_sel_mode(APP_CURSOR_MOVE, PC_SEL_UNION) == APP_CURSOR_MOVE);
    app_destroy(a);
}

static uint64_t img_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void save_bmp(const char *name, const uint8_t *rgba, int size)
{
    char path[1200];
    SDL_Surface *s = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32, (void *)rgba,
                                           size * 4);
    if (!s) return;
    at_out_path(path, sizeof path, name);
    (void)SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

static void t_cursor_images(void)
{
    static const app_cursor kinds[] = {
        APP_CURSOR_GRAB, APP_CURSOR_SEL_REPLACE, APP_CURSOR_SEL_UNION, APP_CURSOR_SEL_EXCLUDE,
        APP_CURSOR_SEL_INTERSECT, APP_CURSOR_SEL_XOR, APP_CURSOR_LASSO_UNION,
        APP_CURSOR_LASSO_EXCLUDE, APP_CURSOR_LASSO_INTERSECT, APP_CURSOR_LASSO_XOR,
        APP_CURSOR_WAND_UNION, APP_CURSOR_WAND_EXCLUDE, APP_CURSOR_WAND_INTERSECT,
        APP_CURSOR_WAND_XOR
    };
    enum { NK = (int)(sizeof kinds / sizeof kinds[0]) };
    static const int sizes[2] = { 24, 32 };
    uint64_t hashes[NK];
    uint8_t rgba[32 * 32 * 4];
    for (int si = 0; si < 2; si++) {
        int size = sizes[si];
        for (int k = 0; k < NK; k++) {
            int hx = -1, hy = -1, opaque = 0;
            bool hot_drawn = false;
            CHECK(app_tool_cursor_rgba(kinds[k], size, rgba, &hx, &hy));
            CHECK(hx >= 0 && hy >= 0 && hx < size && hy < size);
            for (int i = 0; i < size * size; i++)
                if (rgba[i * 4 + 3] > 128u) opaque++;
            CHECK(opaque > size);                               /* something is drawn */
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int x = hx + dx, y = hy + dy;
                    if (x >= 0 && y >= 0 && x < size && y < size &&
                        rgba[((size_t)y * (size_t)size + (size_t)x) * 4u + 3u] > 0u)
                        hot_drawn = true;
                }
            CHECK(hot_drawn);                                   /* the hotspot is on the drawing */
            hashes[k] = img_hash(rgba, (size_t)size * (size_t)size * 4u);
            if (size == 32) {
                char name[64];
                snprintf(name, sizeof name, "toola_cursor_%d.bmp", (int)kinds[k]);
                save_bmp(name, rgba, size);
            }
        }
        /* every mode looks different */
        for (int i = 0; i < NK; i++)
            for (int j = i + 1; j < NK; j++) CHECK(hashes[i] != hashes[j]);
    }
    /* the canvas draws the others itself */
    CHECK(!app_tool_cursor_rgba(APP_CURSOR_HAND, 24, rgba, NULL, NULL));
    CHECK(!app_tool_cursor_rgba(APP_CURSOR_ARROW, 24, rgba, NULL, NULL));
    CHECK(!app_tool_cursor_rgba(APP_CURSOR_GRAB, 8, rgba, NULL, NULL));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_supersampled);
    RUN(t_status_area);
    RUN(t_cursors);
    RUN(t_cursor_images);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
