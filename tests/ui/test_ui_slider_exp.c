/* test_ui_slider_exp.c - lane W3B-FXCORE: the quadratic ("exponential
 * scale") slider mapping UI_SLIDER_EXP that effect dialogs use for radius
 * sliders (OBSERVED O-UI-NONLIN). Checks the drag mapping (value from the
 * pointer position) and the drawn thumb position against the thumb centers
 * measured on Paint.NET 5.2 dialogs:
 *   Gaussian Radius 2.0 of 0..300 at 8.2 % of the track, Bokeh Radius 25
 *   at 29 %, Square Radius 6 at 14 %, Turbulence Period 100 of 0.1..1024 at
 *   31 %, Vignette Radius 0.5 of 0.1..4 at 32 %, Polar Inversion Scale 1 of
 *   -8..8 at 68 %, Gamma Boost 0 and 1.23 of -0.99..2 at 33 % and 86 %.
 * Plus the unchanged log and linear mappings, keys and clamping. */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct sstate {
    double  v[4];
    double  lo[4], hi[4];
    uint32_t flags[4];
    ui_rect r[4];
} sstate;

static sstate S;

static void s_scene(ui_ctx *ctx, void *ud)
{
    (void)ud;
    ui_layout_push(ctx, ui_rect_make(10, 10, 360, 300), 0.0f);
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof id, "##s%d", i);
        ui_slider_double(ctx, id, &S.v[i], S.lo[i], S.hi[i], 0.0, S.flags[i]);
        S.r[i] = ui_last_rect(ctx);
    }
    ui_layout_pop(ctx);
}

static void setup(int i, double lo, double hi, double v, uint32_t flags)
{
    S.lo[i] = lo;
    S.hi[i] = hi;
    S.v[i] = v;
    S.flags[i] = flags;
}

/* Track ends of slider i (thumb centers at min and max). */
static void track(ut_env *e, int i, float *x0, float *x1)
{
    float half = (float)ui_px(e->ctx, ui_get_theme(e->ctx)->m.slider_thumb) * 0.5f;
    *x0 = (float)S.r[i].x + half;
    *x1 = (float)(S.r[i].x + S.r[i].w) - half;
}

/* Press and release at fraction t of slider i's track. */
static void press_at(ut_env *e, int i, double t)
{
    float x0, x1, x, y = ut_cy(S.r[i]);
    track(e, i, &x0, &x1);
    x = x0 + (float)t * (x1 - x0);
    ut_move(e, x, y);
    ut_button(e, SDL_BUTTON_LEFT, true, x, y, 1);
    ut_frame(e, s_scene, NULL);
    ut_button(e, SDL_BUTTON_LEFT, false, x, y, 1);
    ut_frame(e, s_scene, NULL);
}

/* Fraction of the track where slider i's thumb is drawn: the midpoint of
 * the thumb face (the raised color) on the slider's center row. */
static double thumb_t(ut_env *e, int i)
{
    const ui_palette *p = ui_pal(e->ctx);
    uint32_t face = ((uint32_t)p->raised.r << 16) | ((uint32_t)p->raised.g << 8) | p->raised.b;
    int y = (int)ut_cy(S.r[i]), lo = -1, hi = -1;
    float x0, x1;
    ut_render(e);
    for (int x = S.r[i].x; x < S.r[i].x + S.r[i].w; x++) {
        uint32_t px = ut_pixel(e, x, y);
        int ok = 1;
        for (int c = 0; c < 3; c++)
            if (abs(ut_chan(px, c) - ut_chan(face, c)) > 6) ok = 0;
        if (!ok) continue;
        if (lo < 0) lo = x;
        hi = x;
    }
    if (lo < 0) return -1.0;
    track(e, i, &x0, &x1);
    return (((double)lo + (double)hi + 1.0) * 0.5 - (double)x0) / (double)(x1 - x0);
}

static void t_drag_mapping(void)
{
    ut_env e;
    double ts[5] = { 0.1, 0.25, 0.5, 0.75, 0.9 };
    memset(&S, 0, sizeof S);
    setup(0, 0.0, 300.0, 2.0, UI_SLIDER_EXP);          /* Gaussian Radius */
    setup(1, 0.1, 1024.0, 100.0, UI_SLIDER_EXP);       /* Turbulence Period */
    setup(2, -8.0, 8.0, 1.0, UI_SLIDER_EXP);           /* Polar Inversion Scale */
    setup(3, -0.99, 2.0, 0.0, UI_SLIDER_EXP);          /* Gamma Boost */
    if (!ut_open(&e, 400, 320, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_scene, NULL);
    for (int k = 0; k < 5; k++) {
        double t = ts[k];
        float x0, x1;
        double tol;
        track(&e, 0, &x0, &x1);
        tol = 1.5 / (double)(x1 - x0);                 /* a pixel and a half of t */
        press_at(&e, 0, t);
        /* min >= 0: value = min + span * t^2 */
        CHECK(fabs(sqrt(S.v[0] / 300.0) - t) < tol);
        press_at(&e, 1, t);
        CHECK(fabs(sqrt((S.v[1] - 0.1) / 1023.9) - t) < tol);
        press_at(&e, 2, t);
        /* split range: zero at the linear position, quadratic away from it */
        {
            double u = t >= 0.5 ? (t - 0.5) / 0.5 : (0.5 - t) / 0.5, want = 8.0 * u * u;
            if (t < 0.5) want = -want;
            CHECK(fabs(S.v[2] - want) < 16.0 * 2.0 * u * tol / 0.5 + 0.02);
        }
        press_at(&e, 3, t);
        {
            double t0 = 0.99 / 2.99, want;
            if (t >= t0) {
                double u = (t - t0) / (1.0 - t0);
                want = 2.0 * u * u;
            } else {
                double u = (t0 - t) / t0;
                want = -0.99 * u * u;
            }
            CHECK(fabs(S.v[3] - want) < 0.05);
        }
    }
    /* small radii get most of the track: 0..10 of 0..300 spans 18 % of it,
     * where a linear slider gives it 3 % */
    press_at(&e, 0, 0.18);
    CHECK(S.v[0] > 8.5 && S.v[0] < 11.0);
    /* ends and keys */
    press_at(&e, 0, 0.0);
    CHECK(S.v[0] == 0.0);
    press_at(&e, 0, 1.0);
    CHECK(S.v[0] == 300.0);
    ut_key(&e, SDLK_HOME, SDL_KMOD_NONE);
    ut_frame(&e, s_scene, NULL);
    CHECK(S.v[0] == 0.0);
    ut_close(&e);
}

static void t_thumb_positions(void)
{
    /* value, range, measured thumb fraction on the Paint.NET 5.2 dialogs */
    static const struct { double v, lo, hi, t; } k[] = {
        { 2.0, 0.0, 300.0, 0.082 },      /* Gaussian Radius */
        { 25.0, 0.0, 300.0, 0.290 },     /* Bokeh Radius */
        { 6.0, 0.0, 300.0, 0.140 },      /* Square Blur Radius */
        { 1.2, 0.0, 300.0, 0.063 },      /* Bokeh Radius typed 1.2 */
        { 10.0, 0.0, 100.0, 0.314 },     /* Drop Shadow Radius */
        { 3.0, 0.0, 500.0, 0.077 },      /* Frosted Glass Maximum */
        { 25.0, 0.0, 200.0, 0.353 },     /* Dents Scale */
        { 50.0, 0.0, 200.0, 0.502 },     /* Dents Refraction */
        { 100.0, 0.1, 1024.0, 0.314 },   /* Turbulence Period */
        { 0.5, 0.1, 4.0, 0.319 },        /* Vignette Radius */
        { 10.0, 1.0, 500.0, 0.135 },     /* Motion Blur Distance */
        { 40.0, 1.0, 1600.0, 0.155 },    /* Tile Reflection Tile Size */
        { 1.0, -8.0, 8.0, 0.676 },       /* Polar Inversion Scale */
        { 0.0, -0.99, 2.0, 0.333 },      /* Gamma Boost default */
        { 1.23, -0.99, 2.0, 0.855 },     /* Gamma Boost typed 1.23 */
    };
    ut_env e;
    memset(&S, 0, sizeof S);
    if (!ut_open(&e, 400, 320, 1.0f)) { CHECK(0); ut_close(&e); return; }
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        double t;
        setup(0, k[i].lo, k[i].hi, k[i].v, UI_SLIDER_EXP);
        setup(1, k[i].lo, k[i].hi, k[i].v, 0u);        /* linear reference */
        ut_frame(&e, s_scene, NULL);
        t = thumb_t(&e, 0);
        /* the measurement resolution was one pixel of a 207 px track */
        if (fabs(t - k[i].t) >= 0.012)
            INFO("case %u: thumb at %.3f, measured %.3f", (unsigned)i, t, k[i].t);
        CHECK(fabs(t - k[i].t) < 0.012);
        if (k[i].lo >= 0.0 && k[i].v > k[i].lo)        /* linear would sit elsewhere */
            CHECK(fabs(thumb_t(&e, 1) - (k[i].v - k[i].lo) / (k[i].hi - k[i].lo)) < 0.012);
    }
    ut_close(&e);
}

static void t_other_mappings(void)
{
    ut_env e;
    memset(&S, 0, sizeof S);
    setup(0, 1.0, 100.0, 1.0, UI_SLIDER_LOG);          /* log stays log */
    setup(1, 0.0, 100.0, 0.0, 0u);                     /* linear stays linear */
    setup(2, 1.0, 100.0, 1.0, UI_SLIDER_LOG | UI_SLIDER_EXP);   /* EXP wins */
    setup(3, -10.0, -2.0, -10.0, UI_SLIDER_EXP);       /* all-negative range */
    if (!ut_open(&e, 400, 320, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_scene, NULL);
    press_at(&e, 0, 0.5);
    CHECK(fabs(S.v[0] - 10.0) < 0.3);
    press_at(&e, 1, 0.5);
    CHECK(fabs(S.v[1] - 50.0) < 0.5);
    press_at(&e, 2, 0.5);
    CHECK(fabs(S.v[2] - (1.0 + 99.0 * 0.25)) < 0.8);
    /* mirrored: fine steps near the end closest to zero */
    press_at(&e, 3, 0.5);
    CHECK(fabs(S.v[3] - (-2.0 - 8.0 * 0.25)) < 0.2);
    press_at(&e, 3, 1.0);
    CHECK(S.v[3] == -2.0);
    press_at(&e, 3, 0.0);
    CHECK(S.v[3] == -10.0);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!ut_sdl_init()) INFO("SDL video unavailable; running without it");
    RUN(t_drag_mapping);
    RUN(t_thumb_positions);
    RUN(t_other_mappings);
    SDL_Quit();
    return pc_test_finish();
}
