/* test_plg_shape3d.c - the optional Shape3D plugin (plugins/shape3d),
 * loaded as the built library through the editor's plugin loader.
 *
 * Covers: the exports and the schema (46 props, menu, defaults,
 * enabled_if); the default sphere's silhouette (opaque center, clear
 * corners, the perspective radius); the plane map with a nearly
 * orthographic camera (center and rim texels); a 90 degree rotation about
 * Y bringing u = 0.25 of the full sphere map to the front; antialiasing
 * (no interior pixel at alpha 254, partial alpha only on the rim);
 * see-through rendering (front at 50 % over the inner back wall); a box
 * with the front face off showing the mirrored inside of the rear face,
 * and no faces at all; every face of the dice map, and the float dice map;
 * the half sphere map hiding the lower half; the scalable plane map being
 * transparent outside the image; a zero light direction equal to (0, 0, 1);
 * pan (1, 0) centering the object on the right edge; rounded edges
 * changing shading only; cylinder ends; and the generic determinism,
 * ROI-only and cancellation checks of fx_test_util.h on several settings. */
#include "plg1_util.h"

#define FX_ID "org.paintc.render.shape3d"
#define PI 3.14159265358979323846

static plg_env g_env;

/* Params from a preset string over the defaults ("key=value;..."). */
static void *params(const char *preset)
{
    void *p = fx_params_new(g_env.fx, NULL);
    CHECK(p != NULL);
    if (!p) return NULL;
    if (preset && *preset) {
        pc_status st = fx_preset_load(g_env.fx, p, preset, strlen(preset));
        CHECK(st == PC_OK);
        if (st != PC_OK) fprintf(stderr, "  bad preset: %s\n", preset);
    }
    return p;
}

/* Renders the whole image (no selection) with params preset. */
static void render(const char *preset, const fx_img *src, fx_img *dst)
{
    void *p = params(preset);
    fx_env env = fxt_env(src->r.x + src->r.w, src->r.y + src->r.h, src->r);
    fxt_fill_canary(dst);
    if (p) CHECK(fx_run_sync(g_env.fx, p, src, dst, &env, src->r, NULL) == PC_OK);
    fx_params_free(p);
}

/* Opaque gradient texture: r = 255 x / (w - 1), g = 255 y / (h - 1). */
static void fill_gradient(fx_img *im)
{
    for (int32_t y = 0; y < im->r.h; y++)
        for (int32_t x = 0; x < im->r.w; x++)
            *plg_px(im, x, y) = fx_px_make(fx_u8(255.0 * x / (im->r.w - 1)),
                                           fx_u8(255.0 * y / (im->r.h - 1)), 128, 255);
}

static void fill_checker(fx_img *im, int32_t cell)
{
    for (int32_t y = 0; y < im->r.h; y++)
        for (int32_t x = 0; x < im->r.w; x++)
            *plg_px(im, x, y) = ((x / cell + y / cell) & 1)
                                    ? fx_px_make(240, 240, 240, 255) : fx_px_make(30, 60, 200, 255);
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* Projected radius (pixels) of a unit sphere at the center, default camera
 * angle cam degrees, for a w x h selection. */
static double sphere_radius_px(double cam, int32_t w, int32_t h)
{
    double r0 = 0.45 * (w < h ? w : h), d = 1.0 / tan(cam * PI / 360.0);
    return r0 * d * tan(asin(1.0 / d));
}

static void t_schema(void)
{
    const fx_effect *fx = g_env.fx;
    const fx_prop *p;
    CHECK(strcmp(fx->menu, "Effects/Render/Shape3D") == 0);
    CHECK(fx->n_props == 46u);
    CHECK((fx->flags & (FX_FLAG_NO_DIALOG | FX_FLAG_SINGLE_THREAD | FX_FLAG_NO_SEL_CLIP)) == 0u);
    CHECK(plg_def(fx, "shape") == 0.0);
    CHECK(plg_def(fx, "scale") == 1.0);
    CHECK(plg_def(fx, "camera") == 23.0);
    CHECK(plg_def(fx, "ends") == 1.0);
    CHECK(plg_def(fx, "rot1_axis") == 0.0 && plg_def(fx, "rot2_axis") == 1.0 &&
          plg_def(fx, "rot3_axis") == 2.0);
    CHECK(plg_def(fx, "transp_alpha") == 208.0);
    CHECK(plg_def(fx, "ambient") == 0.15 && plg_def(fx, "light_x") == -0.75);
    CHECK(plg_def(fx, "phong") == 50.0 && plg_def(fx, "ior") == 1.4 && plg_def(fx, "luster") == 1.0);
    p = fx_prop_find(fx, "map");
    CHECK(p && p->kind == FXP_CHOICE && p->choices && strcmp(p->choices[9], "Dice map") == 0 &&
          p->choices[11] == NULL);
    p = fx_prop_find(fx, "pan");
    CHECK(p && p->kind == FXP_POINT && p->min == -2.0 && p->max == 2.0);
    p = fx_prop_find(fx, "rot2");
    CHECK(p && p->kind == FXP_ANGLE);
    p = fx_prop_find(fx, "light_color");
    CHECK(p && p->kind == FXP_COLOR && (p->flags & 8u) != 0u && p->def == 4294967295.0);
    p = fx_prop_find(fx, "face_left");
    CHECK(p && p->kind == FXP_BOOL && p->enabled_if && strcmp(p->enabled_if, "shape=2") == 0);
    p = fx_prop_find(fx, "aa_level");
    CHECK(p && p->kind == FXP_INT && p->min == 1.0 && p->max == 5.0 && p->enabled_if &&
          strcmp(p->enabled_if, "aa") == 0);
    p = fx_prop_find(fx, "rough_x");
    CHECK(p && p->enabled_if && strcmp(p->enabled_if, "luster=1") == 0);
}

static void t_default_sphere(void)
{
    const int32_t n = 200;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    double r = sphere_radius_px(23.0, n, n);
    int32_t left = -1, right = -1, top = -1;
    fill_checker(&src, 20);
    render("", &src, &dst);
    CHECK(plg_px(&dst, 0, 0)->a == 0u && plg_px(&dst, n - 1, 0)->a == 0u);
    CHECK(plg_px(&dst, 0, n - 1)->a == 0u && plg_px(&dst, n - 1, n - 1)->a == 0u);
    CHECK(plg_px(&dst, 100, 100)->a == 255u && plg_px(&dst, 99, 99)->a == 255u);
    for (int32_t x = 0; x < n; x++)
        if (plg_px(&dst, x, 100)->a >= 128u) {
            if (left < 0) left = x;
            right = x;
        }
    for (int32_t y = 0; y < n && top < 0; y++)
        if (plg_px(&dst, 100, y)->a >= 128u) top = y;
    INFO("default sphere: radius %.2f px, row 100 covers %d..%d, column 100 from %d", r,
         (int)left, (int)right, (int)top);
    CHECK(fabs((100.0 - left) - r) <= 1.5);
    CHECK(fabs((right + 1.0 - 100.0) - r) <= 1.5);
    CHECK(fabs((100.0 - top) - r) <= 1.5);
    /* lit from the upper left: the upper left is brighter than the lower right */
    {
        fx_px ul = *plg_px(&dst, 70, 70), lr = *plg_px(&dst, 130, 130);
        CHECK(ul.r + ul.g + ul.b > lr.r + lr.g + lr.b || (ul.b > lr.b));
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_plane_ortho(void)
{
    const int32_t n = 200;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    fx_px c, s;
    int32_t left = -1;
    fill_gradient(&src);
    render("map=6;light=0;camera=1", &src, &dst);
    c = *plg_px(&dst, 100, 100);
    s = *plg_px(&src, 100, 100);
    CHECK(c.a == 255u);
    CHECK(iabs(c.r - s.r) <= 1 && iabs(c.g - s.g) <= 1 && c.b == 128u);
    /* the leftmost opaque pixel of the middle row samples the left border */
    for (int32_t x = 0; x < n && left < 0; x++)
        if (plg_px(&dst, x, 100)->a == 255u) left = x;
    CHECK(left > 0 && left < 20);
    if (left > 0) CHECK(plg_px(&dst, left, 100)->r <= 25u);
    /* nearly orthographic: the image is not stretched along the middle row */
    for (int32_t x = 60; x <= 140; x += 20) {
        double want = 255.0 * (0.5 + (x + 0.5 - 100.0) / 90.0 * 0.5);
        CHECK(fabs(plg_px(&dst, x, 100)->r - want) <= 3.0);
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_rotation(void)
{
    const int32_t n = 160;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    fx_px c;
    fill_gradient(&src);
    /* no rotation: the texture's center column faces the viewer */
    render("light=0", &src, &dst);
    c = *plg_px(&dst, 80, 80);
    CHECK(iabs(c.r - 128) <= 3 && iabs(c.g - 128) <= 3);
    /* 90 degrees about Y (rotation 1): u = 0.25 comes to the front */
    render("light=0;rot1_axis=1;rot1=90", &src, &dst);
    c = *plg_px(&dst, 80, 80);
    INFO("rotated center r=%d (u = 0.25 is %d)", c.r, fx_u8(255.0 * 0.25 * n / (n - 1) - 0.5 *
                                                              255.0 / (n - 1)));
    CHECK(iabs(c.r - 64) <= 3 && iabs(c.g - 128) <= 3);
    /* the same rotation as rotation 3 about Y */
    render("light=0;rot3_axis=1;rot3=90", &src, &dst);
    CHECK(iabs(plg_px(&dst, 80, 80)->r - 64) <= 3);
    /* -90 about Y: u = 0.75 */
    render("light=0;rot2=-90", &src, &dst);
    CHECK(iabs(plg_px(&dst, 80, 80)->r - 191) <= 3);
    /* 90 about X brings the top (v = 0, the north pole) toward the viewer:
     * the center samples the top row */
    render("light=0;rot1=90", &src, &dst);
    CHECK(plg_px(&dst, 80, 80)->g <= 4u);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_antialias(void)
{
    const int32_t n = 120;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    int partial = 0, interior254 = 0;
    fill_checker(&src, 15);
    render("aa=1;aa_level=3;shape=1;rot1=30;rot2=20", &src, &dst);
    for (int32_t y = 1; y < n - 1; y++)
        for (int32_t x = 1; x < n - 1; x++) {
            uint8_t a = plg_px(&dst, x, y)->a;
            bool inner = true;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (plg_px(&dst, x + dx, y + dy)->a == 0u) inner = false;
            if (a > 0u && a < 255u) {
                partial++;
                /* partial alpha only on the rim, next to a clear pixel within 2 px */
                {
                    bool near_clear = false;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int32_t xx = x + dx, yy = y + dy;
                            if (xx >= 0 && yy >= 0 && xx < n && yy < n &&
                                plg_px(&dst, xx, yy)->a < a)
                                near_clear = true;
                        }
                    CHECK(near_clear);
                }
            }
            if (a == 254u && inner) interior254++;
        }
    CHECK(partial > 20);
    CHECK(interior254 == 0);
    /* without antialiasing every pixel is 0 or 255 */
    render("shape=1;rot1=30;rot2=20", &src, &dst);
    partial = 0;
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) {
            uint8_t a = plg_px(&dst, x, y)->a;
            partial += a > 0u && a < 255u;
        }
    CHECK(partial == 0);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_transparency(void)
{
    const int32_t n = 160;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    fx_px c;
    /* red in the middle half of u (the front), blue in the outer quarters (the back) */
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++)
            *plg_px(&src, x, y) = (x >= n / 4 && x < 3 * n / 4) ? fx_px_make(255, 0, 0, 255)
                                                                : fx_px_make(0, 0, 255, 255);
    render("light=0;transp=1;transp_alpha=128", &src, &dst);
    c = *plg_px(&dst, 80, 80);
    INFO("see-through center: %d %d %d %d", c.r, c.g, c.b, c.a);
    CHECK(c.a == 255u);
    CHECK(iabs(c.r - 128) <= 2 && iabs(c.b - 127) <= 2 && c.g == 0u);
    /* opaque without transparency */
    render("light=0", &src, &dst);
    c = *plg_px(&dst, 80, 80);
    CHECK(c.r == 255u && c.b == 0u && c.a == 255u);
    /* alpha 0 front: only the lit inside of the back shows */
    render("transp=1;transp_alpha=0", &src, &dst);
    c = *plg_px(&dst, 80, 80);
    CHECK(c.a == 255u && c.b > c.r);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_box_faces(void)
{
    const int32_t n = 160;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    fx_px c;
    /* left half green, right half magenta */
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++)
            *plg_px(&src, x, y) = x < n / 2 ? fx_px_make(0, 200, 0, 255)
                                            : fx_px_make(200, 0, 200, 255);
    render("shape=2;map=8;light=0;size_x=0.6;size_y=0.6;size_z=0.6", &src, &dst);
    c = *plg_px(&dst, 95, 80);                   /* right of center: right half */
    CHECK(c.r == 200u && c.g == 0u && c.a == 255u);
    render("shape=2;map=8;light=0;size_x=0.6;size_y=0.6;size_z=0.6;face_front=0", &src, &dst);
    c = *plg_px(&dst, 95, 80);                   /* the rear face from inside: mirrored */
    CHECK(c.g == 200u && c.r == 0u && c.a == 255u);
    c = *plg_px(&dst, 65, 80);
    CHECK(c.r == 200u && c.g == 0u);
    /* lit: the inside of the rear face is shaded like a surface facing the viewer */
    render("shape=2;map=8;size_x=0.6;size_y=0.6;size_z=0.6;face_front=0;spec=0", &src, &dst);
    c = *plg_px(&dst, 95, 80);
    CHECK(c.a == 255u && c.g > 0u);
    /* no faces: nothing */
    render("shape=2;face_front=0;face_rear=0;face_top=0;face_bottom=0;face_left=0;face_right=0",
           &src, &dst);
    for (int32_t y = 0; y < n; y += 3)
        for (int32_t x = 0; x < n; x += 3) CHECK(plg_px(&dst, x, y)->a == 0u);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static fx_px dice_color(int col, int row)
{
    return fx_px_make((uint8_t)(40 + 60 * col), (uint8_t)(50 + 80 * row), (uint8_t)(200 - 30 * col),
                      255);
}

static void t_dice(void)
{
    static const struct { const char *preset; int col, row; } k[] = {
        { "", 1, 1 },                              /* front */
        { "rot1=90", 1, 0 },                       /* top */
        { "rot1=-90", 1, 2 },                      /* bottom */
        { "rot2=90", 0, 1 },                       /* left */
        { "rot2=-90", 2, 1 },                      /* right */
        { "rot2=180", 3, 1 },                      /* rear */
    };
    const int32_t cw = 30;
    fx_img src = fxt_img_new(fxt_rect(0, 0, 4 * cw, 3 * cw), 4), dst = fxt_img_new(src.r, 4);
    char preset[256];
    for (int32_t y = 0; y < 3 * cw; y++)
        for (int32_t x = 0; x < 4 * cw; x++) *plg_px(&src, x, y) = dice_color(x / cw, y / cw);
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        for (int fl = 0; fl < 2; fl++) {
            fx_px c, want = dice_color(k[i].col, k[i].row);
            snprintf(preset, sizeof preset, "shape=2;map=%d;light=0;size_x=0.5;size_y=0.5;"
                     "size_z=0.5%s%s", fl ? 10 : 9, k[i].preset[0] ? ";" : "", k[i].preset);
            render(preset, &src, &dst);
            c = *plg_px(&dst, 2 * cw, 3 * cw / 2);
            if (!plg_px_eq(c, want))
                INFO("dice %s: got %d %d %d, want %d %d %d", preset, c.r, c.g, c.b, want.r,
                     want.g, want.b);
            CHECK(plg_px_eq(c, want));
        }
    /* float dice on a wide box: the front still samples the front cell */
    render("shape=2;map=10;light=0;size_x=1.2;size_y=0.4;size_z=0.4", &src, &dst);
    CHECK(plg_px_eq(*plg_px(&dst, 2 * cw, 3 * cw / 2), dice_color(1, 1)));
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_maps_hidden(void)
{
    const int32_t n = 160;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    fill_gradient(&src);
    /* half sphere map: the lower half is not drawn */
    render("map=1;light=0", &src, &dst);
    CHECK(plg_px(&dst, 80, 50)->a == 255u);
    CHECK(plg_px(&dst, 80, 110)->a == 0u);
    /* repeat: drawn, mirrored */
    render("map=2;light=0", &src, &dst);
    CHECK(plg_px(&dst, 80, 110)->a == 255u);
    CHECK(iabs(plg_px(&dst, 80, 110)->g - plg_px(&dst, 80, 49)->g) <= 8);
    /* half cylinder map: the back half is hidden, so see-through shows nothing behind */
    render("shape=1;map=4;light=0;transp=1;transp_alpha=128", &src, &dst);
    CHECK(plg_px(&dst, 80, 80)->a == 128u);
    render("shape=1;map=5;light=0;transp=1;transp_alpha=128", &src, &dst);
    CHECK(plg_px(&dst, 80, 80)->a == 255u);
    /* scalable plane map: transparent outside the image */
    render("map=7;light=0;tex_scale=0.5", &src, &dst);
    CHECK(plg_px(&dst, 80, 80)->a == 255u);
    CHECK(plg_px(&dst, 80 + 55, 80)->a == 0u);
    render("map=7;light=0;tex_scale=1", &src, &dst);
    CHECK(plg_px(&dst, 80 + 55, 80)->a == 255u);
    /* texture rotation: 180 degrees swaps left and right */
    render("map=6;light=0;camera=1;tex_rot=2", &src, &dst);
    CHECK(plg_px(&dst, 50, 80)->r > plg_px(&dst, 110, 80)->r);
    render("map=6;light=0;camera=1;tex_rot=1", &src, &dst);
    CHECK(plg_px(&dst, 80, 50)->r < plg_px(&dst, 80, 110)->r);  /* left edge on top */
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_light_and_pan(void)
{
    const int32_t w = 180, h = 140;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), a = fxt_img_new(src.r, 4);
    fx_img b = fxt_img_new(src.r, 4);
    int32_t left = -1, right = -1;
    double r = sphere_radius_px(23.0, w, h);
    fill_checker(&src, 12);
    render("light_x=0;light_y=0;light_z=0", &src, &a);
    render("light_x=0;light_y=0;light_z=1", &src, &b);
    CHECK(fxt_equal_in(&a, &b, src.r));
    render("light_x=0.5;light_y=0;light_z=0", &src, &b);
    CHECK(!fxt_equal_in(&a, &b, src.r));
    /* pan (1, 0): the center projects onto the right edge of the selection */
    render("pan=1,0", &src, &a);
    for (int32_t x = 0; x < w; x++)
        if (plg_px(&a, x, h / 2)->a >= 128u) {
            if (left < 0) left = x;
            right = x;
        }
    INFO("pan 1,0: row covers %d..%d (radius about %.1f)", (int)left, (int)right, r);
    CHECK(right == w - 1);
    CHECK(left > 0 && fabs((double)(w - left) - r) <= 0.08 * r + 2.0);
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

static void t_shapes(void)
{
    const int32_t n = 140;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), a = fxt_img_new(src.r, 4);
    fx_img b = fxt_img_new(src.r, 4);
    int diff_rgb = 0, diff_a = 0;
    fill_checker(&src, 14);
    /* rounded edges bend normals only: same alpha, other colors near the edges */
    render("shape=2;rot1=25;rot2=35;size_x=0.6;size_y=0.6;size_z=0.6", &src, &a);
    render("shape=2;rot1=25;rot2=35;size_x=0.6;size_y=0.6;size_z=0.6;round=8", &src, &b);
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) {
            fx_px p = *plg_px(&a, x, y), q = *plg_px(&b, x, y);
            diff_a += p.a != q.a;
            diff_rgb += p.r != q.r || p.g != q.g || p.b != q.b;
        }
    CHECK(diff_a == 0);
    CHECK(diff_rgb > 50);
    /* a cylinder: the side is as tall as the height, ball ends make it taller */
    {
        int32_t top_flat = -1, top_ball = -1;
        render("shape=1;size_x=0.5;size_y=0.6;size_z=0.5;camera=1;light=0", &src, &a);
        render("shape=1;size_x=0.5;size_y=0.6;size_z=0.5;camera=1;light=0;ends=2;ball_h=0.3",
               &src, &b);
        for (int32_t y = 0; y < n && top_flat < 0; y++)
            if (plg_px(&a, n / 2, y)->a >= 128u) top_flat = y;
        for (int32_t y = 0; y < n && top_ball < 0; y++)
            if (plg_px(&b, n / 2, y)->a >= 128u) top_ball = y;
        /* 0.6 units * 63 px per unit above the center; the dome adds 0.3 units */
        CHECK(iabs(top_flat - (int32_t)(70 - 0.6 * 63)) <= 1);
        CHECK(iabs(top_ball - (int32_t)(70 - 0.9 * 63)) <= 1);
    }
    /* tilted 60 degrees toward the viewer, the middle ray runs through an
     * open tube without touching it; flat ends close it */
    render("shape=1;ends=0;rot1=60;light=0;map=3", &src, &a);
    render("shape=1;ends=1;rot1=60;light=0;map=3", &src, &b);
    CHECK(plg_px(&a, n / 2, n / 2)->a == 0u);
    CHECK(plg_px(&b, n / 2, n / 2)->a == 255u);
    CHECK(plg_px(&a, n / 2, n / 2 - 50)->a == 255u);    /* the tube wall */
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

static void t_cook_torrance(void)
{
    const int32_t n = 120;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), a = fxt_img_new(src.r, 4);
    fx_img b = fxt_img_new(src.r, 4);
    uint32_t peak_a = 0, peak_b = 0;
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) *plg_px(&src, x, y) = fx_px_make(90, 90, 90, 255);
    render("spec=0", &src, &a);
    render("spec_model=1;luster=0;spec_rate=4", &src, &b);
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) {
            uint32_t va = plg_px(&a, x, y)->r, vb = plg_px(&b, x, y)->r;
            CHECK(vb >= va);                      /* a highlight only adds light */
            if (va > peak_a) peak_a = va;
            if (vb > peak_b) peak_b = vb;
        }
    CHECK(peak_b > peak_a + 10u);
    render("spec_model=1;luster=1;spec_rate=4", &src, &a);
    CHECK(!fxt_equal_in(&a, &b, src.r));
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

static void t_invariance(void)
{
    static const char *const presets[] = {
        "",
        "shape=1;ends=2;aa=1;aa_level=2;rot1=35;rot2=-20;map=3;pan=0.2,-0.1",
        "shape=2;map=9;transp=1;spec_model=1;round=5;rot1=20;rot2=30;rot3=10",
        "shape=1;ends=0;map=5;transp=1;transp_alpha=90;spec_model=1;luster=0;camera=60",
    };
    for (size_t k = 0; k < sizeof presets / sizeof presets[0]; k++) {
        void *p = params(presets[k]);
        if (p) fxt_check_effect(g_env.fx, p, 71, 53, 300u + (uint32_t)k);
        fx_params_free(p);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!plg_load(&g_env, FX_ID, "paint.c port of Shape3D by MKT")) {
        plg_unload(&g_env);
        return pc_test_finish();
    }
    RUN(t_schema);
    RUN(t_default_sphere);
    RUN(t_plane_ortho);
    RUN(t_rotation);
    RUN(t_antialias);
    RUN(t_transparency);
    RUN(t_box_faces);
    RUN(t_dice);
    RUN(t_maps_hidden);
    RUN(t_light_and_pan);
    RUN(t_shapes);
    RUN(t_cook_torrance);
    RUN(t_invariance);
    plg_unload(&g_env);
    return pc_test_finish();
}
