/* test_ui_draw.c - the drawing layer rendered through the software
 * renderer: exact rectangles, the clip stack, antialiased rounded
 * rectangles, lines, circles, arcs and polygons (coverage against the
 * analytic area), gradients, checkerboards, images with nearest and linear
 * filtering, text runs, icons, shadows, custom callbacks, batching, pixel
 * snapping at fractional scales, atlas overflow and device resets. */
#include "pc_test.h"
#include "ui_test_util.h"

#include "ui_internal.h"   /* batching and atlas statistics */

#define WHITE ui_rgba(255, 255, 255, 255)

typedef void (*draw_fn)(ui_ctx *ctx, void *ud);

/* Render one frame that only draws, on a black background. */
static void draw_frame(ut_env *e, draw_fn fn, void *ud)
{
    ut_frame(e, fn, ud);
    ut_render(e);
}

static int red(ut_env *e, int x, int y) { return ut_chan(ut_pixel(e, x, y), 0); }

/* Coverage of white-on-black drawings in pixel units. */
static double coverage(ut_env *e, ui_rect r) { return ut_sum(e, r, 0) / 255.0; }

/* ---- rectangles and clipping ---------------------------------------------- */
static void d_rects(ui_ctx *ctx, void *ud)
{
    (void)ud;
    ui_draw_rect(ctx, ui_rect_make(10, 10, 20, 5), WHITE);
    ui_draw_rect_outline(ctx, ui_rect_make(40, 10, 20, 12), 2, ui_rgba(255, 0, 0, 255));
    ui_draw_rect(ctx, ui_rect_make(70, 10, 0, 5), WHITE);           /* empty: nothing */
    ui_draw_rect(ctx, ui_rect_make(70, 20, 10, 10), ui_rgba(255, 255, 255, 128));
}

static void t_rects(void)
{
    ut_env e;
    if (!ut_open(&e, 120, 60, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_rects, NULL);
    CHECK(ut_count(&e, ui_rect_make(10, 10, 20, 5), 0xFFFFFF, 0) == 100);
    CHECK(ut_pixel(&e, 9, 12) == 0 && ut_pixel(&e, 30, 12) == 0);
    CHECK(ut_pixel(&e, 15, 9) == 0 && ut_pixel(&e, 15, 15) == 0);
    /* outline: 2 px border, untouched interior */
    CHECK(ut_count(&e, ui_rect_make(40, 10, 20, 12), 0xFF0000, 0) == 20 * 12 - 16 * 8);
    CHECK(ut_count(&e, ui_rect_make(42, 12, 16, 8), 0x000000, 0) == 16 * 8);
    CHECK(ut_count(&e, ui_rect_make(70, 0, 5, 20), 0x000000, 0) == 100);
    /* straight alpha blending: half white over black */
    CHECK(abs(red(&e, 75, 25) - 128) <= 2);
    ut_close(&e);
}

typedef struct clip_rec { ui_rect c[4]; } clip_rec;

static void d_clip(ui_ctx *ctx, void *ud)
{
    clip_rec *c = (clip_rec *)ud;
    c->c[0] = ui_current_clip(ctx);
    ui_push_clip(ctx, ui_rect_make(12, 12, 5, 5));
    ui_draw_rect(ctx, ui_rect_make(0, 0, 50, 50), ui_rgba(255, 0, 0, 255));
    ui_pop_clip(ctx);
    ui_push_clip(ctx, ui_rect_make(60, 10, 10, 10));
    ui_push_clip(ctx, ui_rect_make(65, 15, 20, 20));
    c->c[1] = ui_current_clip(ctx);
    ui_draw_rect(ctx, ui_rect_make(50, 0, 50, 50), ui_rgba(0, 255, 0, 255));
    ui_pop_clip(ctx);
    c->c[2] = ui_current_clip(ctx);
    ui_pop_clip(ctx);
    ui_pop_clip(ctx);                       /* extra pops keep the root clip */
    ui_pop_clip(ctx);
    c->c[3] = ui_current_clip(ctx);
    ui_draw_rect(ctx, ui_rect_make(0, 55, 4, 4), ui_rgba(0, 0, 255, 255));
}

static void t_clip(void)
{
    ut_env e;
    clip_rec c;
    if (!ut_open(&e, 120, 60, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_clip, &c);
    CHECK(c.c[0].x == 0 && c.c[0].y == 0 && c.c[0].w == 120 && c.c[0].h == 60);
    CHECK(c.c[1].x == 65 && c.c[1].y == 15 && c.c[1].w == 5 && c.c[1].h == 5);
    CHECK(c.c[2].x == 60 && c.c[2].w == 10);
    CHECK(c.c[3].w == 120 && c.c[3].h == 60);
    CHECK(ut_count(&e, ui_rect_make(0, 0, 120, 60), 0xFF0000, 0) == 25);
    CHECK(ut_count(&e, ui_rect_make(12, 12, 5, 5), 0xFF0000, 0) == 25);
    CHECK(ut_count(&e, ui_rect_make(0, 0, 120, 60), 0x00FF00, 0) == 25);
    CHECK(ut_count(&e, ui_rect_make(65, 15, 5, 5), 0x00FF00, 0) == 25);
    CHECK(ut_count(&e, ui_rect_make(0, 55, 4, 4), 0x0000FF, 0) == 16);
    ut_close(&e);
}

/* ---- rounded rectangles --------------------------------------------------- */
static void d_rrect(ui_ctx *ctx, void *ud)
{
    (void)ud;
    ui_draw_rrect(ctx, ui_rect_make(20, 20, 40, 30), 8.0f, WHITE);
    ui_draw_rrect_outline(ctx, ui_rect_make(70, 20, 40, 30), 6.0f, 1, WHITE);
    ui_draw_rrect_ex(ctx, ui_rect_make(120, 20, 30, 30), ui_corners_make(0.0f, 10.0f, 10.0f, 10.0f),
                     WHITE);
    ui_draw_rrect(ctx, ui_rect_make(160, 20, 10, 30), 50.0f, WHITE);   /* radius clamped */
}

static void t_rrect(void)
{
    ut_env e;
    double cov;
    int partial = 0;
    bool sym = true;
    if (!ut_open(&e, 180, 70, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_rrect, NULL);
    /* analytic area: w h - (4 - pi) r^2 */
    cov = coverage(&e, ui_rect_make(18, 18, 44, 34));
    CHECK(fabs(cov - (1200.0 - (4.0 - 3.14159265) * 64.0)) < 2.0);
    CHECK(red(&e, 40, 35) == 255);
    CHECK(red(&e, 20, 20) < 20 && red(&e, 59, 49) < 20);
    CHECK(red(&e, 20, 35) == 255 && red(&e, 19, 35) == 0);        /* crisp straight edges */
    CHECK(red(&e, 40, 20) == 255 && red(&e, 40, 19) == 0);
    CHECK(red(&e, 59, 35) == 255 && red(&e, 60, 35) == 0);
    for (int y = 20; y < 28; y++)
        for (int x = 20; x < 28; x++) {
            int v = red(&e, x, y);
            if (v > 0 && v < 255) partial++;
            /* the four corners mirror each other exactly */
            if (v != red(&e, 59 - (x - 20), y) || v != red(&e, x, 49 - (y - 20)) ||
                v != red(&e, 59 - (x - 20), 49 - (y - 20)))
                sym = false;
        }
    CHECK(partial >= 6);
    CHECK(sym);
    /* outline: 1 px ring, interior untouched */
    CHECK(red(&e, 90, 35) == 0 && red(&e, 70, 35) == 255 && red(&e, 71, 35) == 0);
    CHECK(red(&e, 90, 20) == 255 && red(&e, 90, 49) == 255 && red(&e, 109, 35) == 255);
    cov = coverage(&e, ui_rect_make(68, 18, 44, 34));
    CHECK(fabs(cov - (2.0 * (40.0 + 30.0) - 4.0 - (8.0 - 2.0 * 3.14159265) * 5.5)) < 6.0);
    /* per-corner radii: square top left only */
    CHECK(red(&e, 120, 20) == 255 && red(&e, 149, 20) < 30 && red(&e, 149, 49) < 30);
    /* the oversized radius becomes a capsule of radius 5 */
    CHECK(red(&e, 165, 35) == 255 && red(&e, 160, 20) < 40);
    ut_close(&e);
}

/* ---- lines, circles, arcs, polygons --------------------------------------- */
static void d_shapes(ui_ctx *ctx, void *ud)
{
    ui_vec2 sq[4];
    int which = *(const int *)ud;
    if (which == 0) {
        ui_draw_line(ctx, ui_vec2_make(5.0f, 10.5f), ui_vec2_make(95.0f, 10.5f), 1.0f, WHITE);
        ui_draw_line(ctx, ui_vec2_make(10.5f, 20.0f), ui_vec2_make(10.5f, 90.0f), 1.0f, WHITE);
        ui_draw_line(ctx, ui_vec2_make(20.0f, 20.0f), ui_vec2_make(20.0f, 60.0f), 2.0f, WHITE);
    } else if (which == 1) {
        ui_draw_line(ctx, ui_vec2_make(20.0f, 20.0f), ui_vec2_make(80.0f, 80.0f), 2.0f, WHITE);
    } else if (which == 2) {
        ui_draw_circle(ctx, ui_vec2_make(50.0f, 50.0f), 20.0f, WHITE);
    } else if (which == 3) {
        ui_draw_circle(ctx, ui_vec2_make(150.0f, 150.0f), 120.0f, WHITE);   /* geometry path */
    } else if (which == 4) {
        ui_draw_circle_outline(ctx, ui_vec2_make(50.0f, 50.0f), 30.0f, 2.0f, WHITE);
    } else if (which == 5) {
        ui_draw_arc(ctx, ui_vec2_make(50.0f, 50.0f), 30.0f, 0.0f, UI_PI * 0.5f, 3.0f, WHITE);
    } else if (which == 6) {
        ui_draw_triangle(ctx, ui_vec2_make(10.0f, 10.0f), ui_vec2_make(60.0f, 10.0f),
                         ui_vec2_make(10.0f, 60.0f), WHITE);
    } else if (which == 7) {
        sq[0] = ui_vec2_make(20.0f, 20.0f);
        sq[1] = ui_vec2_make(70.0f, 20.0f);
        sq[2] = ui_vec2_make(70.0f, 70.0f);
        sq[3] = ui_vec2_make(20.0f, 70.0f);
        ui_draw_polyline(ctx, sq, 4, true, 2.0f, WHITE);
    } else {
        /* counter-clockwise winding and a thin line */
        sq[0] = ui_vec2_make(20.0f, 20.0f);
        sq[1] = ui_vec2_make(20.0f, 70.0f);
        sq[2] = ui_vec2_make(70.0f, 70.0f);
        sq[3] = ui_vec2_make(70.0f, 20.0f);
        ui_draw_convex(ctx, sq, 4, WHITE);
        ui_draw_line(ctx, ui_vec2_make(80.0f, 5.0f), ui_vec2_make(80.0f, 95.0f), 0.5f, WHITE);
    }
}

static void t_shapes(void)
{
    ut_env e;
    ui_rect all = ui_rect_make(0, 0, 300, 300);
    int w;
    double cov;
    if (!ut_open(&e, 300, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    w = 0;
    draw_frame(&e, d_shapes, &w);
    /* pixel-centered axis lines are crisp rectangles */
    CHECK(ut_count(&e, ui_rect_make(5, 10, 90, 1), 0xFFFFFF, 0) == 90);
    CHECK(ut_count(&e, ui_rect_make(5, 9, 90, 1), 0x000000, 0) == 90);
    CHECK(ut_count(&e, ui_rect_make(5, 11, 90, 1), 0x000000, 0) == 90);
    CHECK(ut_count(&e, ui_rect_make(10, 20, 1, 70), 0xFFFFFF, 0) == 70);
    CHECK(ut_count(&e, ui_rect_make(19, 20, 2, 40), 0xFFFFFF, 0) == 80);
    CHECK(red(&e, 18, 40) == 0 && red(&e, 21, 40) == 0);
    /* Feathered geometry: SDL's software rasterizer snaps vertices to half
     * pixels, so thin diagonal strokes are only checked loosely here (GPU
     * backends interpolate exactly). */
    w = 1;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(cov > 0.8 * 2.0 * sqrt(2.0) * 60.0 && cov < 1.5 * 2.0 * sqrt(2.0) * 60.0);
    CHECK(ut_count(&e, all, 0x000000, 0) + ut_count(&e, all, 0xFFFFFF, 0) < 300 * 300);
    CHECK(red(&e, 50, 50) == 255 && red(&e, 60, 40) == 0);
    w = 2;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 3.14159265 * 400.0) < 0.015 * 3.14159265 * 400.0);
    CHECK(red(&e, 50, 50) == 255 && red(&e, 50, 29) < 128 && red(&e, 50, 71) == 0);
    w = 3;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 3.14159265 * 14400.0) < 0.02 * 3.14159265 * 14400.0);
    w = 4;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 2.0 * 3.14159265 * 30.0 * 2.0) < 0.04 * 2.0 * 3.14159265 * 60.0);
    CHECK(red(&e, 50, 50) == 0 && red(&e, 80, 50) > 200);
    w = 5;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 0.5 * 3.14159265 * 30.0 * 3.0) < 0.12 * 0.5 * 3.14159265 * 90.0);
    /* 0 rad is +x and angles grow clockwise on screen: the quarter runs
     * through the lower right (71, 71), not the upper right (71, 28) */
    CHECK(coverage(&e, ui_rect_make(68, 68, 6, 6)) > 4.0);
    CHECK(coverage(&e, ui_rect_make(68, 25, 6, 6)) == 0.0);
    CHECK(red(&e, 20, 50) == 0 && red(&e, 50, 20) == 0);
    w = 6;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 1250.0) < 0.03 * 1250.0);
    CHECK(red(&e, 20, 20) == 255 && red(&e, 50, 50) == 0);
    w = 7;
    draw_frame(&e, d_shapes, &w);
    cov = coverage(&e, all);
    CHECK(fabs(cov - 4.0 * 50.0 * 2.0) < 0.1 * 400.0);
    CHECK(red(&e, 45, 45) == 0);
    w = 8;
    draw_frame(&e, d_shapes, &w);
    CHECK(red(&e, 45, 45) == 255);
    cov = coverage(&e, ui_rect_make(78, 0, 5, 100));
    CHECK(cov > 30.0 && cov < 60.0);                               /* 0.5 px faded line */
    ut_close(&e);
}

/* ---- gradients, checkerboards, images ------------------------------------- */
typedef struct img_rec { SDL_Texture *tex; int mode; } img_rec;

static void d_paint(ui_ctx *ctx, void *ud)
{
    img_rec *ir = (img_rec *)ud;
    ui_rect src = ui_rect_make(1, 0, 1, 1);
    if (ir->mode == 0) {
        ui_draw_gradient(ctx, ui_rect_make(0, 0, 100, 10), ui_rgba(255, 0, 0, 255),
                         ui_rgba(0, 0, 255, 255), ui_rgba(0, 0, 255, 255), ui_rgba(255, 0, 0, 255));
        ui_draw_checker(ctx, ui_rect_make(10, 20, 40, 40), 5, WHITE, ui_rgba(60, 60, 60, 255));
        ui_draw_checker(ctx, ui_rect_make(60, 20, 7, 7), 5, WHITE, ui_rgba(60, 60, 60, 255));
    } else {
        ui_draw_image(ctx, ir->tex, NULL, ui_rect_make(0, 0, 20, 20), UI_FILTER_NEAREST, WHITE);
        ui_draw_image(ctx, ir->tex, NULL, ui_rect_make(30, 0, 20, 20), UI_FILTER_LINEAR, WHITE);
        ui_draw_image(ctx, ir->tex, &src, ui_rect_make(60, 0, 10, 10), UI_FILTER_NEAREST, WHITE);
        ui_draw_image(ctx, ir->tex, NULL, ui_rect_make(0, 30, 20, 20), UI_FILTER_NEAREST,
                      ui_rgba(255, 0, 0, 255));
        ui_draw_image(ctx, NULL, NULL, ui_rect_make(80, 0, 10, 10), UI_FILTER_NEAREST, WHITE);
    }
}

static void t_paint(void)
{
    ut_env e;
    img_rec ir;
    uint8_t px[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
    int prev = 256;
    bool mono = true;
    if (!ut_open(&e, 120, 70, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ir.mode = 0;
    ir.tex = NULL;
    draw_frame(&e, d_paint, &ir);
    CHECK(red(&e, 0, 5) > 245 && ut_chan(ut_pixel(&e, 99, 5), 2) > 245);
    CHECK(abs(red(&e, 50, 5) - 128) < 10);
    for (int x = 0; x < 100; x++) {
        int r = red(&e, x, 5);
        if (r > prev) mono = false;
        prev = r;
    }
    CHECK(mono);
    /* checker cells of 5 px starting with color a at the top left */
    CHECK(ut_pixel(&e, 10, 20) == 0xFFFFFF && ut_pixel(&e, 14, 24) == 0xFFFFFF);
    CHECK(ut_pixel(&e, 15, 20) == 0x3C3C3C && ut_pixel(&e, 10, 25) == 0x3C3C3C);
    CHECK(ut_pixel(&e, 15, 25) == 0xFFFFFF && ut_pixel(&e, 49, 59) == 0xFFFFFF);
    CHECK(ut_count(&e, ui_rect_make(10, 20, 40, 40), 0xFFFFFF, 0) == 800);
    CHECK(ut_pixel(&e, 60, 20) == 0xFFFFFF && ut_pixel(&e, 66, 26) == 0xFFFFFF &&
          ut_pixel(&e, 65, 20) == 0x3C3C3C);
    /* images: 2 x 2 texture red, green / blue, white */
    ir.tex = SDL_CreateTexture(e.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 2, 2);
    CHECK(ir.tex != NULL);
    if (ir.tex) {
        SDL_UpdateTexture(ir.tex, NULL, px, 8);
        ir.mode = 1;
        draw_frame(&e, d_paint, &ir);
        CHECK(ut_pixel(&e, 2, 2) == 0xFF0000 && ut_pixel(&e, 17, 2) == 0x00FF00);
        CHECK(ut_pixel(&e, 2, 17) == 0x0000FF && ut_pixel(&e, 17, 17) == 0xFFFFFF);
        CHECK(ut_pixel(&e, 9, 9) == 0xFF0000 && ut_pixel(&e, 10, 9) == 0x00FF00);
        {
            uint32_t m = ut_pixel(&e, 40, 10);              /* linear: a blend */
            CHECK(ut_chan(m, 0) > 40 && ut_chan(m, 1) > 40 && ut_chan(m, 2) > 40);
            CHECK(ut_pixel(&e, 39, 9) != ut_pixel(&e, 32, 2));
        }
        CHECK(ut_count(&e, ui_rect_make(60, 0, 10, 10), 0x00FF00, 0) == 100);
        CHECK(ut_pixel(&e, 17, 47) == 0xFF0000 && ut_pixel(&e, 17, 32) == 0x000000);   /* tint */
        CHECK(ut_count(&e, ui_rect_make(80, 0, 10, 10), 0x000000, 0) == 100);
        SDL_DestroyTexture(ir.tex);
    }
    ut_close(&e);
}

/* ---- text runs -------------------------------------------------------------- */
typedef struct text_rec { float end, width, size; int mode; } text_rec;

static void d_text(ui_ctx *ctx, void *ud)
{
    text_rec *t = (text_rec *)ud;
    ui_font *f = ui_font_regular(ctx);
    t->size = ui_font_px(ctx);
    if (t->mode == 0) {
        t->end = ui_draw_text(ctx, f, t->size, 10.0f, 40.0f, WHITE, "Hello, world", 12);
        t->width = ui_text_width(f, t->size, "Hello, world", 12);
        ui_draw_text(ctx, f, t->size, 10.0f, 80.0f, WHITE, "\xE6\xB0\xB4", 3);   /* box */
    } else if (t->mode == 1) {
        const char *s = "A rather long label that cannot fit";
        ui_draw_text_box(ctx, f, t->size, ui_rect_make(10, 10, 80, 20), UI_ALIGN_LEFT,
                         UI_TEXT_ELLIPSIS, WHITE, s, strlen(s));
        ui_draw_text_box(ctx, f, t->size, ui_rect_make(10, 40, 80, 20), UI_ALIGN_LEFT, 0, WHITE,
                         s, strlen(s));
        ui_draw_text_box(ctx, f, t->size, ui_rect_make(100, 10, 80, 20), UI_ALIGN_CENTER, 0,
                         WHITE, "Mid", 3);
        ui_draw_text_box(ctx, f, t->size, ui_rect_make(100, 40, 80, 20), UI_ALIGN_RIGHT, 0, WHITE,
                         "Right", 5);
    }
}

/* Ink bounding box of non-black pixels in r. */
static void ink_box(ut_env *e, ui_rect r, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = INT32_MAX; *y0 = INT32_MAX; *x1 = -1; *y1 = -1;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            if (ut_pixel(e, x, y) != 0) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
}

static void t_text(void)
{
    ut_env e;
    text_rec t;
    int x0, y0, x1, y1;
    ui_font_metrics m;
    memset(&t, 0, sizeof t);
    if (!ut_open(&e, 200, 100, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_text, &t);
    CHECK(fabsf(t.end - 10.0f - t.width) < 1e-3f);
    ui_font_get_metrics(ui_font_regular(e.ctx), t.size, &m);
    ink_box(&e, ui_rect_make(0, 0, 200, 60), &x0, &y0, &x1, &y1);
    CHECK(x0 >= 9 && x0 <= 12 && x1 <= (int)ceilf(t.end) + 1 && x1 > (int)t.end - 6);
    CHECK(y0 >= 40 - (int)ceilf(m.ascent) - 1 && y1 <= 40 + (int)ceilf(m.descent) + 1);
    CHECK(y0 <= 40 - (int)floorf(m.cap_height) + 1);       /* "H" reaches the cap height */
    CHECK(ut_count(&e, ui_rect_make(0, 0, 200, 60), 0xFFFFFF, 60) > 20);
    /* a missing glyph draws a box outline */
    ink_box(&e, ui_rect_make(0, 60, 200, 40), &x0, &y0, &x1, &y1);
    CHECK(x1 > x0 && y1 > y0 && y1 <= 80);
    t.mode = 1;
    draw_frame(&e, d_text, &t);
    ink_box(&e, ui_rect_make(0, 0, 100, 35), &x0, &y0, &x1, &y1);
    CHECK(x0 >= 10 && x1 < 90 && x1 > 70);                 /* ellipsis fills the box */
    ink_box(&e, ui_rect_make(0, 36, 100, 30), &x0, &y0, &x1, &y1);
    CHECK(x0 >= 10 && x1 < 90 && x1 > 80);                 /* clipped, not cut */
    ink_box(&e, ui_rect_make(100, 0, 100, 35), &x0, &y0, &x1, &y1);
    CHECK(abs((x0 + x1) / 2 - 140) <= 2);                  /* centered */
    ink_box(&e, ui_rect_make(100, 36, 100, 30), &x0, &y0, &x1, &y1);
    CHECK(x1 >= 176 && x1 < 180);                          /* right aligned */
    ut_close(&e);
}

/* ---- icons, shadows, callbacks ------------------------------------------------ */
typedef struct misc_rec { int calls; ui_rect clip; int mode; } misc_rec;

static void cb(SDL_Renderer *r, ui_rect clip, void *ud)
{
    misc_rec *m = (misc_rec *)ud;
    SDL_FRect fr;
    m->calls++;
    m->clip = clip;
    fr.x = 0.0f; fr.y = 0.0f; fr.w = 200.0f; fr.h = 200.0f;
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    SDL_RenderFillRect(r, &fr);
}

static void d_misc(ui_ctx *ctx, void *ud)
{
    misc_rec *m = (misc_rec *)ud;
    if (m->mode == 0) {
        ui_draw_icon(ctx, UI_ICON_TOOL_PAINTBRUSH, ui_rect_make(10, 10, 40, 40), 24,
                     ui_rgba(255, 0, 0, 255), ui_rgba(0, 255, 0, 255));
        ui_draw_icon(ctx, UI_ICON_NONE, ui_rect_make(60, 10, 40, 40), 24, WHITE, WHITE);
        ui_draw_icon(ctx, UI_ICON_COUNT, ui_rect_make(60, 10, 40, 40), 24, WHITE, WHITE);
    } else if (m->mode == 1) {
        ui_draw_rect(ctx, ui_rect_make(0, 0, 160, 120), WHITE);
        ui_draw_shadow(ctx, ui_rect_make(50, 50, 40, 20), 4.0f, 16.0f, ui_rgba(0, 0, 0, 255));
    } else {
        ui_push_clip(ctx, ui_rect_make(10, 10, 50, 40));
        ui_draw_callback(ctx, cb, m);
        ui_draw_rect(ctx, ui_rect_make(30, 30, 50, 50), ui_rgba(0, 255, 0, 255));
        ui_pop_clip(ctx);
    }
}

static void t_misc(void)
{
    ut_env e;
    misc_rec m;
    int x0, y0, x1, y1;
    memset(&m, 0, sizeof m);
    if (!ut_open(&e, 160, 120, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_misc, &m);
    ink_box(&e, ui_rect_make(0, 0, 160, 120), &x0, &y0, &x1, &y1);
    CHECK(x0 >= 18 && y0 >= 18 && x1 <= 41 && y1 <= 41);  /* inside the centered 24 px */
    CHECK(ut_count(&e, ui_rect_make(18, 18, 24, 24), 0xFF0000, 30) > 10);
    CHECK(ut_count(&e, ui_rect_make(18, 18, 24, 24), 0x00FF00, 30) > 3);
    m.mode = 1;
    draw_frame(&e, d_misc, &m);
    CHECK(red(&e, 70, 60) < 60);                           /* under the box: dark */
    CHECK(red(&e, 70, 45) < 250 && red(&e, 70, 45) > red(&e, 70, 60));   /* soft edge */
    CHECK(red(&e, 70, 30) == 255 && red(&e, 5, 5) == 255); /* far away: untouched */
    CHECK(red(&e, 46, 60) < red(&e, 40, 60));               /* falls off outwards */
    m.mode = 2;
    draw_frame(&e, d_misc, &m);
    CHECK(m.calls == 1);
    CHECK(m.clip.x == 10 && m.clip.y == 10 && m.clip.w == 50 && m.clip.h == 40);
    CHECK(ut_pixel(&e, 15, 15) == 0xFF0000 && ut_pixel(&e, 5, 5) == 0x000000);
    CHECK(ut_pixel(&e, 40, 40) == 0x00FF00);                /* drawn after the callback */
    CHECK(ut_pixel(&e, 62, 40) == 0x000000);                /* clip restored for later calls */
    ut_close(&e);
}

/* ---- batching ----------------------------------------------------------------- */
static void d_batch(ui_ctx *ctx, void *ud)
{
    (void)ud;
    for (int i = 0; i < 50; i++) {
        ui_draw_rect(ctx, ui_rect_make(i * 3, 0, 2, 2), WHITE);
        ui_draw_rrect(ctx, ui_rect_make(i * 3, 10, 12, 12), 3.0f, WHITE);
        ui_draw_text(ctx, ui_font_regular(ctx), 13.0f, (float)(i * 3), 40.0f, WHITE, "ab", 2);
        ui_draw_circle(ctx, ui_vec2_make((float)i * 3.0f, 60.0f), 4.0f, WHITE);
        ui_draw_icon(ctx, UI_ICON_SAVE, ui_rect_make(i * 3, 70, 16, 16), 16, WHITE, WHITE);
        ui_draw_line(ctx, ui_vec2_make(0.0f, 90.0f), ui_vec2_make((float)i, 99.0f), 1.5f, WHITE);
    }
}

static void t_batch(void)
{
    ut_env e;
    if (!ut_open(&e, 200, 100, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, d_batch, NULL);   /* first frame rasterizes sprites */
    draw_frame(&e, d_batch, NULL);
    {
        ui_root *base = &e.ctx->roots[0];
        CHECK(e.ctx->npages == 1);
        CHECK(base->dl.nc == 1);                  /* one SDL_RenderGeometry call */
        CHECK(base->dl.nv > 1000);
        INFO("batch: %d commands, %d vertices, %d indices", (int)base->dl.nc, (int)base->dl.nv,
             (int)base->dl.ni);
    }
    ut_close(&e);
}

/* ---- pixel snapping at fractional scales ---------------------------------------- */
typedef struct snap_rec { ui_rect r; } snap_rec;

static void d_snap(ui_ctx *ctx, void *ud)
{
    snap_rec *s = (snap_rec *)ud;
    ui_draw_rect(ctx, ui_rect_make(0, 0, 400, 200), ui_pal(ctx)->panel);
    ui_layout_push(ctx, ui_rect_make(ui_px(ctx, 10.0f), ui_px(ctx, 10.0f), ui_px(ctx, 200.0f),
                                     ui_px(ctx, 100.0f)), 0.0f);
    ui_button(ctx, "Snap");
    s->r = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void t_snap(void)
{
    static const float scales[] = { 1.0f, 1.25f, 1.5f, 1.75f };
    for (size_t k = 0; k < 4; k++) {
        ut_env e;
        snap_rec s;
        uint32_t edge, top, bg;
        bool same = true;
        if (!ut_open(&e, 400, 200, scales[k])) { CHECK(0); ut_close(&e); continue; }
        ut_frame(&e, d_snap, &s);
        draw_frame(&e, d_snap, &s);
        /* the top border row is one solid color, the row above is background */
        top = ut_pixel(&e, s.r.x + s.r.w / 2, s.r.y);
        bg = ut_pixel(&e, s.r.x + s.r.w / 2, s.r.y - 1);
        for (int x = s.r.x + 8; x < s.r.x + s.r.w - 8; x++) {
            if (ut_pixel(&e, x, s.r.y) != top) same = false;
            if (ut_pixel(&e, x, s.r.y - 1) != bg) same = false;
        }
        edge = ut_pixel(&e, s.r.x, s.r.y + s.r.h / 2);
        CHECK(same);
        CHECK(top != bg);
        CHECK(edge == top);                                   /* left edge same as top */
        CHECK(ut_pixel(&e, s.r.x - 1, s.r.y + s.r.h / 2) == bg);
        CHECK(s.r.h == ui_px(e.ctx, ui_get_theme(e.ctx)->m.control_h));
        ut_close(&e);
    }
}

/* ---- atlas overflow and device reset -------------------------------------------- */
/* Many glyphs at many sizes, each drawn on its own so none is culled. */
static void d_flood(ui_ctx *ctx, void *ud)
{
    int base = *(const int *)ud;
    static const char s[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int k = 0; k < 40; k++)
        for (size_t i = 0; i + 1u < sizeof s; i++)
            ui_draw_text(ctx, ui_font_regular(ctx), (float)(base + k) * 1.25f + 8.0f, 0.0f,
                         100.0f, WHITE, s + i, 1);
}

static void d_small(ui_ctx *ctx, void *ud)
{
    (void)ud;
    ui_draw_text(ctx, ui_font_regular(ctx), 13.0f, 4.0f, 20.0f, WHITE, "Still here", 10);
    ui_draw_rrect(ctx, ui_rect_make(4, 30, 40, 20), 6.0f, ui_rgba(255, 0, 0, 255));
}

static void t_atlas(void)
{
    ut_env e;
    int base = 0;
    uint64_t h0, h1;
    bool overflowed = false;
    if (!ut_open(&e, 300, 120, 1.0f)) { CHECK(0); ut_close(&e); return; }
    draw_frame(&e, d_small, NULL);
    h0 = ut_hash(&e);
    CHECK(ut_count(&e, ui_rect_make(0, 0, 300, 28), 0xFFFFFF, 60) > 10);
    for (int i = 0; i < (g_quick ? 8 : 20); i++) {
        base = i * 40;
        draw_frame(&e, d_flood, &base);
        CHECK(e.ctx->npages <= UI_ATLAS_MAX_PAGES);
        if (e.ctx->atlas_overflow) overflowed = true;
    }
    CHECK(overflowed);
    draw_frame(&e, d_small, NULL);                 /* the atlas resets and recovers */
    draw_frame(&e, d_small, NULL);
    h1 = ut_hash(&e);
    CHECK(h0 == h1);
    CHECK(e.ctx->npages >= 1 && e.ctx->npages <= UI_ATLAS_SOFT_PAGES);
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_EVENT_RENDER_DEVICE_RESET;
        ui_event(e.ctx, &ev);
        draw_frame(&e, d_small, NULL);
        CHECK(ut_hash(&e) == h0);                  /* textures rebuilt from CPU pages */
    }
    ut_close(&e);
}

/* ---- determinism ------------------------------------------------------------------ */
static void t_determinism(void)
{
    uint64_t h[2];
    for (int k = 0; k < 2; k++) {
        ut_env e;
        if (!ut_open(&e, 200, 100, 1.25f)) { CHECK(0); ut_close(&e); return; }
        draw_frame(&e, d_batch, NULL);
        draw_frame(&e, d_batch, NULL);
        h[k] = ut_hash(&e);
        ut_close(&e);
    }
    CHECK(h[0] == h[1]);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_rects);
    RUN(t_clip);
    RUN(t_rrect);
    RUN(t_shapes);
    RUN(t_paint);
    RUN(t_text);
    RUN(t_misc);
    RUN(t_batch);
    RUN(t_snap);
    RUN(t_atlas);
    RUN(t_determinism);
    SDL_Quit();
    return pc_test_finish();
}
