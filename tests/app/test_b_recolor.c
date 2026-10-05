/* test_b_recolor.c - lane B: Recolor through the real input path:
 * Sampling Once replaces the color under the start of the stroke with the
 * primary (left) or secondary (right) color and keeps the pixels' own
 * variation (T-RECOLOR-ONCE, T-RECOLOR-TOL); Sampling Secondary Color
 * replaces colors like the secondary with the primary and the right button
 * swaps the roles (T-RECOLOR-SEC); tolerance 0 matches exact colors only;
 * the sampling buttons of the options bar persist their choice. */
#include "pc_test.h"
#include "b_test_util.h"
#include "pc/pc_recolor.h"

/* Left half reddish with a little noise in green, right half blue. */
static pc_px32 img(int32_t x, int32_t y)
{
    if (x < 60) return app_px_make(200, (uint8_t)(40 + ((x + y) % 3)), 40, 255);
    return app_px_make(30, 60, 210, 255);
}

static app *setup(void)
{
    app *a = b_image(120, 80, b_px(255, 255, 255, 255));
    if (!a) return NULL;
    CHECK(b_fill_layer(a, img));
    CHECK(app_tool_select(a, "recolor"));
    a->ts.width = 12.0f;
    a->ts.hardness = 100;
    a->ts.tolerance = 30;
    a->ts.tol_straight = false;
    return a;
}

static void t_sampling_once(void)
{
    app *a = setup();
    pc_px32 green = b_px(40, 200, 40, 255), got, want;
    CHECK(a != NULL);
    if (!a) return;
    app_set_primary(a, green);
    /* starts on red, crosses into blue */
    at_drag(a, 20.5, 20.5, 110.5, 20.5, 12, SDL_BUTTON_LEFT);
    CHECK(strcmp(b_top_label(a), "Recolor") == 0 && b_history(a) == 3u);
    for (int32_t x = 25; x < 55; x += 7) {
        got = at_doc_px(a, x, 20);
        want = pc_recolor_pixel(img(x, 20), img(20, 20), green, 30u,
                                PC_RECOLOR_ALPHA_PREMULTIPLIED);
        CHECK(b_eq(got, want));
        CHECK(got.g >= 195 && got.r < 60);          /* recolored, variation kept */
    }
    CHECK(b_eq(at_doc_px(a, 90, 20), img(90, 20)));   /* blue is too far away */
    /* right button: secondary replaces; the target is sampled again */
    app_set_secondary(a, b_px(250, 250, 0, 255));
    at_drag(a, 100.5, 60.5, 20.5, 60.5, 12, SDL_BUTTON_RIGHT);
    CHECK(b_eq(at_doc_px(a, 90, 60), pc_recolor_pixel(img(90, 60), img(100, 60),
                                                      b_px(250, 250, 0, 255), 30u,
                                                      PC_RECOLOR_ALPHA_PREMULTIPLIED)));
    CHECK(b_eq(at_doc_px(a, 30, 60), img(30, 60)));
    app_destroy(a);
}

static void t_sampling_secondary(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    /* choose Sampling Secondary Color with its toolbar button */
    at_frames(a, 2);
    CHECK(b_widget(a, "##rc_secondary", 0.5f, 0.5f));
    CHECK(app_settings_int(app_settings_of(a), "tool.recolor.sampling", 0) == 1);
    app_set_primary(a, b_px(0, 220, 0, 255));
    app_set_secondary(a, b_px(30, 60, 210, 255));    /* the blue */
    /* the stroke starts on red: red stays, blue becomes green */
    at_drag(a, 20.5, 30.5, 110.5, 30.5, 12, SDL_BUTTON_LEFT);
    CHECK(b_eq(at_doc_px(a, 30, 30), img(30, 30)));
    CHECK(b_eq(at_doc_px(a, 90, 30), b_px(0, 220, 0, 255)));
    /* right button: target primary (now the red), replacement secondary */
    app_set_primary(a, b_px(200, 41, 40, 255));
    app_set_secondary(a, b_px(255, 255, 255, 255));
    at_drag(a, 20.5, 60.5, 50.5, 60.5, 6, SDL_BUTTON_RIGHT);
    CHECK(at_doc_px(a, 30, 60).b >= 250 && at_doc_px(a, 30, 60).r == 255);
    /* back to Sampling Once */
    CHECK(b_widget(a, "##rc_once", 0.5f, 0.5f));
    CHECK(app_settings_int(app_settings_of(a), "tool.recolor.sampling", 1) == 0);
    app_destroy(a);
}

/* Tolerance 0: only the exact target color changes. */
static void t_tolerance_zero(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    a->ts.tolerance = 0;
    app_set_primary(a, b_px(0, 0, 0, 255));
    at_drag(a, 20.5, 40.5, 50.5, 40.5, 6, SDL_BUTTON_LEFT);
    {
        int changed = 0, same_color = 0;
        for (int32_t x = 22; x < 49; x++) {
            pc_px32 o = img(x, 40), g = at_doc_px(a, x, 40);
            bool exact = b_eq(o, img(20, 40));
            if (exact) { same_color++; changed += !b_eq(g, o); }
            else CHECK(b_eq(g, o));
        }
        CHECK(same_color > 0 && changed == same_color);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_sampling_once);
    RUN(t_sampling_secondary);
    RUN(t_tolerance_zero);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
