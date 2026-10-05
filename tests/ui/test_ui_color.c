/* test_ui_color.c - color editing: HSV/RGB conversions that keep hue through
 * gray and black, the hue/saturation disc (with Shift, Ctrl and Alt
 * constraints), the hue ring with saturation/value square, channel sliders
 * (drag, keys, typed values), hex entry, swatches, the primary/secondary
 * pair, palette grids and the composed picker in wide and narrow cells. */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct cstate {
    ui_color_edit ce;
    int           mode;
    ui_rect       r[8];
    int           pair, pal, pal_right;
    bool          sw_click, sw_right;
    bool          changed;
} cstate;

static cstate C;

static void t_convert(void)
{
    ui_color_edit ce;
    ui_hsv h;
    int worst = 0;
    memset(&ce, 0, sizeof ce);
    /* round trips over a grid of colors */
    for (int r = 0; r < 256; r += 17)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 51) {
                ui_color c = ui_rgba((uint8_t)r, (uint8_t)g, (uint8_t)b, 200), o;
                o = ui_hsv_to_rgb(ui_rgb_to_hsv(c), 200);
                worst = abs(o.r - c.r) > worst ? abs(o.r - c.r) : worst;
                worst = abs(o.g - c.g) > worst ? abs(o.g - c.g) : worst;
                worst = abs(o.b - c.b) > worst ? abs(o.b - c.b) : worst;
                CHECK(o.a == 200);
            }
    CHECK(worst <= 1);
    h = ui_rgb_to_hsv(ui_rgb_hex(0x00FF00));
    CHECK(fabsf(h.h - 120.0f) < 1e-3f && h.s == 1.0f && h.v == 1.0f);
    h = ui_rgb_to_hsv(ui_rgb_hex(0xFF00FF));
    CHECK(fabsf(h.h - 300.0f) < 1e-3f);
    /* the edit keeps hue (and saturation) when the color goes gray or black */
    h.h = 200.0f; h.s = 0.8f; h.v = 0.6f;
    ui_color_edit_set_hsv(&ce, h);
    ui_color_edit_set_rgba(&ce, ui_rgb_hex(0x000000));
    CHECK(ce.hsv.h == 200.0f && ce.hsv.s == 0.8f && ce.hsv.v == 0.0f);
    ui_color_edit_set_rgba(&ce, ui_rgb_hex(0x808080));
    CHECK(ce.hsv.h == 200.0f && ce.hsv.s == 0.0f);
    h.h = -30.0f; h.s = 2.0f; h.v = -1.0f;                  /* wraps and clamps */
    ui_color_edit_set_hsv(&ce, h);
    CHECK(ce.hsv.h == 330.0f && ce.hsv.s == 1.0f && ce.hsv.v == 0.0f);
    CHECK(ui_color_argb32(ui_argb32(0x80112233u)) == 0x80112233u);
}

static void s_color(ui_ctx *ctx, void *ud)
{
    static const ui_color pal[12] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
                                      { 9, 9, 9, 255 },   { 1, 2, 3, 255 },   { 4, 5, 6, 255 },
                                      { 7, 8, 9, 255 },   { 0, 0, 0, 128 },   { 1, 1, 1, 1 },
                                      { 2, 2, 2, 2 },     { 3, 3, 3, 3 },     { 4, 4, 4, 4 } };
    (void)ud;
    C.changed = false;
    ui_layout_push(ctx, ui_rect_make(10, 10, 150, 700), 0.0f);
    if (C.mode == 0) {
        C.changed = ui_color_wheel(ctx, "##disc", &C.ce, 150.0f, 0);
        C.r[0] = ui_last_rect(ctx);
    } else if (C.mode == 1) {
        C.changed = ui_color_wheel(ctx, "##ring", &C.ce, 150.0f, UI_WHEEL_RING);
        C.r[0] = ui_last_rect(ctx);
    } else if (C.mode == 2) {
        ui_layout_pop(ctx);
        ui_layout_push(ctx, ui_rect_make(10, 10, 300, 700), 0.0f);
        C.changed = ui_color_channel(ctx, "##red", UI_CHAN_RED, &C.ce);
        C.r[0] = ui_layout_content(ctx);
        C.changed |= ui_color_hex(ctx, "##hex", &C.ce);
        C.r[1] = ui_last_rect(ctx);
        C.changed |= ui_color_channel(ctx, "##hue", UI_CHAN_HUE, &C.ce);
    } else if (C.mode == 3) {
        C.pair = ui_color_pair(ctx, "##pair", ui_rgb_hex(0x112233), ui_rgb_hex(0xFFFFFF), 0);
        C.r[0] = ui_last_rect(ctx);
        C.pal = ui_palette_grid(ctx, "##pal", pal, 12, 16.0f, &C.pal_right);
        C.r[1] = ui_last_rect(ctx);
        C.sw_click = ui_color_swatch(ctx, "##sw", ui_rgba(10, 20, 30, 128), 0);
        C.sw_right = ui_last_right_clicked(ctx);
        C.r[2] = ui_last_rect(ctx);
    } else {
        ui_layout_pop(ctx);
        ui_layout_push(ctx, ui_rect_make(10, 10, C.mode == 4 ? 520 : 200, 700), 0.0f);
        C.changed = ui_color_picker(ctx, "##picker", &C.ce, C.mode == 4 ? 0u : UI_PICKER_RING);
        C.r[0] = ui_layout_rest(ctx);
    }
    ui_layout_pop(ctx);
}

static void press_drag(ut_env *e, float x0, float y0, float x1, float y1)
{
    ut_move(e, x0, y0);
    ut_button(e, SDL_BUTTON_LEFT, true, x0, y0, 1);
    ut_frame(e, s_color, NULL);
    ut_move(e, x1, y1);
    ut_frame(e, s_color, NULL);
    ut_button(e, SDL_BUTTON_LEFT, false, x1, y1, 1);
    ut_frame(e, s_color, NULL);
}

static void t_disc(void)
{
    ut_env e;
    float cx, cy, R;
    memset(&C, 0, sizeof C);
    ui_color_edit_set_rgba(&C.ce, ui_rgb_hex(0x336699));
    if (!ut_open(&e, 400, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_color, NULL);
    CHECK(C.r[0].w == 150 && C.r[0].h == 150);
    cx = (float)C.r[0].x + 75.0f;
    cy = (float)C.r[0].y + 75.0f;
    R = 74.5f;
    press_drag(&e, cx, cy, cx, cy);
    CHECK(C.ce.hsv.s < 0.02f);
    press_drag(&e, cx + 0.9f * R, cy, cx + 0.9f * R, cy);
    CHECK((C.ce.hsv.h < 1.0f || C.ce.hsv.h > 359.0f) && fabsf(C.ce.hsv.s - 0.9f) < 0.02f);
    CHECK(C.ce.rgba.r > C.ce.rgba.g && C.ce.rgba.g == C.ce.rgba.b);   /* a red */
    press_drag(&e, cx, cy - 0.5f * R, cx, cy - 0.5f * R);
    CHECK(fabsf(C.ce.hsv.h - 90.0f) < 1.0f && fabsf(C.ce.hsv.s - 0.5f) < 0.02f);
    /* dragging outside the disc clamps the saturation */
    press_drag(&e, cx, cy, cx - 3.0f * R, cy);
    CHECK(fabsf(C.ce.hsv.h - 180.0f) < 1.0f && C.ce.hsv.s == 1.0f);
    /* Shift snaps the hue to 15 degree spokes */
    ut_mods(&e, SDL_KMOD_LSHIFT);
    press_drag(&e, cx + 0.6f * R * cosf(0.87f), cy - 0.6f * R * sinf(0.87f),
               cx + 0.6f * R * cosf(0.87f), cy - 0.6f * R * sinf(0.87f));      /* 49.8 deg */
    CHECK(fabsf(C.ce.hsv.h - 45.0f) < 1e-3f);
    /* Ctrl keeps the saturation from the press, Alt keeps the hue */
    ut_mods(&e, SDL_KMOD_LCTRL);
    press_drag(&e, cx + 0.3f * R, cy, cx, cy + 0.95f * R);
    CHECK(fabsf(C.ce.hsv.s - 0.6f) < 0.03f && fabsf(C.ce.hsv.h - 270.0f) < 1.0f);
    ut_mods(&e, SDL_KMOD_LALT);
    press_drag(&e, cx + 0.3f * R, cy, cx + 0.8f * R, cy);
    CHECK(fabsf(C.ce.hsv.h - 270.0f) < 1.0f && fabsf(C.ce.hsv.s - 0.8f) < 0.03f);
    ut_mods(&e, SDL_KMOD_NONE);
    /* a black color becomes visible when the disc is used */
    ui_color_edit_set_rgba(&C.ce, ui_rgb_hex(0x000000));
    press_drag(&e, cx + 0.5f * R, cy, cx + 0.5f * R, cy);
    CHECK(C.ce.hsv.v == 1.0f && C.ce.rgba.r == 255);
    ut_close(&e);
}

static void t_ring(void)
{
    ut_env e;
    float cx, cy, R, half;
    memset(&C, 0, sizeof C);
    C.mode = 1;
    ui_color_edit_set_rgba(&C.ce, ui_rgb_hex(0x336699));
    if (!ut_open(&e, 400, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_color, NULL);
    cx = (float)C.r[0].x + 75.0f;
    cy = (float)C.r[0].y + 75.0f;
    R = 74.5f;
    half = R * 0.80f * 0.68f;
    /* the square: top right is saturated and bright, bottom left black */
    press_drag(&e, cx + half - 1.0f, cy - half + 1.0f, cx + half - 1.0f, cy - half + 1.0f);
    CHECK(C.ce.hsv.s > 0.97f && C.ce.hsv.v > 0.97f);
    press_drag(&e, cx - half + 1.0f, cy + half - 1.0f, cx - 3.0f * R, cy + 3.0f * R);
    CHECK(C.ce.hsv.s == 0.0f && C.ce.hsv.v == 0.0f);
    /* the ring sets the hue only */
    ui_color_edit_set_rgba(&C.ce, ui_rgb_hex(0x80C040));
    {
        float s0 = C.ce.hsv.s, v0 = C.ce.hsv.v;
        press_drag(&e, cx - 0.9f * R, cy, cx - 0.9f * R, cy);
        CHECK(fabsf(C.ce.hsv.h - 180.0f) < 1.0f && C.ce.hsv.s == s0 && C.ce.hsv.v == v0);
    }
    /* a drag that starts in the square stays in the square */
    press_drag(&e, cx, cy, cx + 0.9f * R, cy);
    CHECK(fabsf(C.ce.hsv.h - 180.0f) < 1.0f && C.ce.hsv.s == 1.0f);
    ut_close(&e);
}

static void t_channels_hex(void)
{
    ut_env e;
    ui_rect bar;
    float y;
    SDL_Keymod pm;
    memset(&C, 0, sizeof C);
    C.mode = 2;
    ui_color_edit_set_rgba(&C.ce, ui_rgba(10, 20, 30, 200));
    if (!ut_open(&e, 400, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    pm = ui_mod_primary() == UI_MOD_GUI ? SDL_KMOD_LGUI : SDL_KMOD_LCTRL;
    ut_frame(&e, s_color, NULL);
    /* row: 16 px label, bar cell, 64 px numeric field (6 px spacing) */
    bar = ui_rect_make(10 + 16 + 6, 10, 300 - 16 - 64 - 12, ui_px(e.ctx, 28.0f));
    y = ut_cy(bar);
    press_drag(&e, (float)bar.x + 10.0f, y, (float)(bar.x + bar.w) + 50.0f, y);
    CHECK(C.ce.rgba.r == 255 && C.ce.rgba.g == 20 && C.ce.rgba.a == 200);
    press_drag(&e, (float)bar.x + 10.0f, y, (float)bar.x - 50.0f, y);
    CHECK(C.ce.rgba.r == 0);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_LSHIFT);
    ut_frame(&e, s_color, NULL);
    CHECK(C.ce.rgba.r == 11);
    /* the channel's numeric field takes typed values */
    ut_click_at(&e, (float)(10 + 300 - 64) + 10.0f, y);
    ut_frame(&e, s_color, NULL);
    ut_key(&e, SDLK_A, pm);
    ut_text(&e, "128");
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_color, NULL);
    CHECK(C.ce.rgba.r == 128);
    /* hex: six digits keep alpha, eight digits set it (AARRGGBB) */
    ut_click(&e, C.r[1]);
    ut_frame(&e, s_color, NULL);
    ut_text(&e, "FF8000");
    ut_frame(&e, s_color, NULL);
    CHECK(C.ce.rgba.r == 255 && C.ce.rgba.g == 128 && C.ce.rgba.b == 0 && C.ce.rgba.a == 200);
    CHECK(fabsf(C.ce.hsv.h - 30.1f) < 0.5f);
    ut_key(&e, SDLK_A, pm);
    ut_text(&e, "#40FF00FF");
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_color, NULL);
    CHECK(C.ce.rgba.a == 0x40 && C.ce.rgba.r == 255 && C.ce.rgba.g == 0 && C.ce.rgba.b == 255);
    ut_key(&e, SDLK_A, pm);
    ut_text(&e, "12");                                    /* incomplete: ignored */
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_color, NULL);
    CHECK(C.ce.rgba.a == 0x40 && C.ce.rgba.r == 255);
    ut_close(&e);
}

static void t_pair_palette(void)
{
    ut_env e;
    int32_t S, ic, cell, gap;
    memset(&C, 0, sizeof C);
    C.mode = 3;
    if (!ut_open(&e, 400, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_color, NULL);
    S = ui_px(e.ctx, 56.0f);
    ic = ui_px(e.ctx, 16.0f);
    CHECK(C.r[0].w == S && C.r[0].h == S);
    ut_click_at(&e, (float)(C.r[0].x + S - 6), (float)(C.r[0].y + S - 6));   /* secondary */
    ut_frame(&e, s_color, NULL);
    CHECK(C.pair == UI_PAIR_SELECT_SECONDARY);
    ut_click_at(&e, (float)(C.r[0].x + 6), (float)(C.r[0].y + 6));           /* primary */
    ut_frame(&e, s_color, NULL);
    CHECK(C.pair == UI_PAIR_SELECT_PRIMARY);
    ut_click_at(&e, (float)(C.r[0].x + S - ic / 2), (float)(C.r[0].y + ic / 2));
    ut_frame(&e, s_color, NULL);
    CHECK(C.pair == UI_PAIR_SWAP);
    ut_click_at(&e, (float)(C.r[0].x + ic / 2), (float)(C.r[0].y + S - ic / 2));
    ut_frame(&e, s_color, NULL);
    CHECK(C.pair == UI_PAIR_RESET);
    ut_frame(&e, s_color, NULL);
    CHECK(C.pair == UI_PAIR_NONE);
    /* palette: 16 px cells with 2 px gaps in a 150 px column: 8 per row */
    cell = ui_px(e.ctx, 16.0f);
    gap = 2;
    ut_click_at(&e, (float)(C.r[1].x + 5 * (cell + gap) + 4), (float)(C.r[1].y + 4));
    ut_frame(&e, s_color, NULL);
    CHECK(C.pal == 5 && C.pal_right == -1);
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, (float)(C.r[1].x + 1 * (cell + gap) + 4),
                    (float)(C.r[1].y + cell + gap + 4), 1);
    ut_frame(&e, s_color, NULL);
    CHECK(C.pal == -1 && C.pal_right == 9);
    CHECK(C.r[1].h == 2 * cell + gap);
    /* swatch: left click and right click */
    ut_click(&e, C.r[2]);
    ut_frame(&e, s_color, NULL);
    CHECK(C.sw_click && !C.sw_right);
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, ut_cx(C.r[2]), ut_cy(C.r[2]), 1);
    ut_frame(&e, s_color, NULL);
    CHECK(!C.sw_click && C.sw_right);
    ut_close(&e);
}

static void t_picker(void)
{
    ut_env e;
    memset(&C, 0, sizeof C);
    ui_color_edit_set_rgba(&C.ce, ui_rgb_hex(0x2E8BC9));
    if (!ut_open(&e, 600, 800, 1.0f)) { CHECK(0); ut_close(&e); return; }
    /* wide: wheel left, sliders right; narrow: stacked and taller */
    C.mode = 4;
    ut_frames(&e, 3, s_color, NULL);
    ut_render(&e);
    {
        int32_t wide_rest = C.r[0].y;
        C.mode = 5;
        ut_frames(&e, 3, s_color, NULL);
        ut_render(&e);
        CHECK(C.r[0].y > wide_rest + 100);
    }
    CHECK(ut_count(&e, ui_rect_make(0, 0, 600, 800), 0x000000, 0) < 600 * 800);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_convert);
    RUN(t_disc);
    RUN(t_ring);
    RUN(t_channels_hex);
    RUN(t_pair_palette);
    RUN(t_picker);
    SDL_Quit();
    return pc_test_finish();
}
