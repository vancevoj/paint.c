/* test_f_levels.c - lane F: the Levels editor (MENUS.md Levels dialog,
 * F-DLG-LEVELS-*): the input histogram covers exactly the selected pixels,
 * Auto equals fx_levels_auto of that histogram, Reset restores identity,
 * the arrows on the gradient bars move the input white point and the gray
 * point, the R G B check boxes choose the edited channels, the preview and
 * the commit equal the oracle, and Auto-Level gives what Levels > Auto
 * gives on the whole image. */
#include "pc_test.h"
#include "f_test_util.h"

#define W 80
#define H 60

static bool levels_eq(const fx_levels *x, const fx_levels *y)
{
    for (int c = 0; c < 3; c++) {
        if (x->in_lo[c] != y->in_lo[c] || x->in_hi[c] != y->in_hi[c]) return false;
        if (x->out_lo[c] != y->out_lo[c] || x->out_hi[c] != y->out_hi[c]) return false;
        if (fabsf(x->gamma[c] - y->gamma[c]) > 1e-6f) return false;
    }
    return x->mask == y->mask;
}

/* A double click at window pixel (x, y): the second press carries clicks 2. */
static void dclick(app *a, float x, float y)
{
    SDL_Event e;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 1);
    for (int k = 1; k <= 2; k++) {
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.clicks = (Uint8)k;
        e.button.which = 1;
        app_event(a, &e);
        at_frames(a, 1);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.down = false;
        app_event(a, &e);
        at_frames(a, 1);
    }
}

static void t_histogram_fn(void)
{
    uint8_t px[4 * 4 * 4], m[16];
    uint64_t h[FX_LEVELS_HIST_LEN];
    fx_img src, sel;
    fx_rect r;
    for (int i = 0; i < 16; i++) {
        px[i * 4 + 0] = (uint8_t)i;          /* B */
        px[i * 4 + 1] = (uint8_t)(100 + i);  /* G */
        px[i * 4 + 2] = 200;                 /* R */
        px[i * 4 + 3] = 255;
        m[i] = (uint8_t)(i % 2 ? 255 : 127);
    }
    src.px = px;
    src.stride = 16;
    src.chans = 4;
    src.r.x = src.r.y = 0;
    src.r.w = src.r.h = 4;
    sel = src;
    sel.px = m;
    sel.stride = 4;
    sel.chans = 1;
    r = src.r;
    afx_levels_histogram(&src, NULL, r, h);
    CHECK(h[FX_CH_R * 256 + 200] == 16u && h[FX_CH_B * 256 + 3] == 1u);
    afx_levels_histogram(&src, &sel, r, h);
    CHECK(h[FX_CH_R * 256 + 200] == 8u && h[FX_CH_B * 256 + 3] == 1u && h[FX_CH_B * 256 + 2] == 0u);
    r.x = 2;
    r.w = 9;
    afx_levels_histogram(&src, NULL, r, h);
    CHECK(h[FX_CH_R * 256 + 200] == 8u);
    afx_levels_histogram(NULL, NULL, r, h);
    CHECK(h[FX_CH_R * 256 + 200] == 0u);
}

static void t_dialog(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    afx_session *s;
    const uint64_t *hist;
    uint64_t want[FX_LEVELS_HIST_LEN];
    fx_levels *lv, ref;
    const fx_effect *fx;
    pc_surf expect, got;
    float top = 0.0f, bot = 0.0f;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    fx = fx_registry_find(a->fx, "org.paintc.adjust.levels");
    CHECK(f_select_ellipse(a, 40.0, 30.0, 30.0, 22.0));
    f_key(a, SDLK_L, AT_KMOD_PRIMARY);                    /* Ctrl+L */
    s = afx_active(a);
    CHECK(s && afx_session_fx(s) == fx);
    if (!s || !fx) {
        app_destroy(a);
        return;
    }
    CHECK(afx_wait_preview(a, 100));
    at_frames(a, 2);
    /* input histogram: B, G, R of the pixels selected at >= 50 % */
    hist = afx_session_histogram(s);
    CHECK(hist != NULL);
    memset(want, 0, sizeof want);
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 p = f_pattern(x, y);
            if (pc_sel_coverage(d->doc, x, y) < 128u) continue;
            want[FX_CH_B * 256 + p.b]++;
            want[FX_CH_G * 256 + p.g]++;
            want[FX_CH_R * 256 + p.r]++;
        }
    CHECK(hist && memcmp(hist, want, sizeof want) == 0);
    lv = (fx_levels *)afx_session_params(s);
    /* Auto */
    f_click_rect(a, afx_levels_rect(a, AFX_LV_AUTO), SDL_BUTTON_LEFT);
    fx_levels_init(&ref);
    fx_levels_auto(want, &ref);
    CHECK(levels_eq(lv, &ref));
    /* Reset */
    f_click_rect(a, afx_levels_rect(a, AFX_LV_RESET), SDL_BUTTON_LEFT);
    fx_levels_init(&ref);
    CHECK(levels_eq(lv, &ref));
    /* a swatch opens its per-channel color picker on a double click only */
    {
        ui_rect sw = afx_levels_rect(a, AFX_LV_SW_IN_HI);
        CHECK(!ui_rect_empty(sw));
        f_click_rect(a, sw, SDL_BUTTON_LEFT);
        CHECK(afx_levels_picking(a) == -1);
        dclick(a, (float)sw.x + (float)sw.w * 0.5f, (float)sw.y + (float)sw.h * 0.5f);
        CHECK(afx_levels_picking(a) == 1);
        f_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);              /* closes the popup only */
        CHECK(afx_levels_picking(a) == -1 && afx_active(a) == s);
        CHECK(levels_eq(lv, &ref));
    }
    /* drag the input white arrow from 255 down to about 200 */
    CHECK(afx_levels_axis(a, &top, &bot));
    {
        ui_rect b = afx_levels_rect(a, AFX_LV_IN_BAR);
        float x = (float)b.x + (float)b.w * 0.5f;
        float y200 = bot - (200.0f / 255.0f) * (bot - top);
        f_drag(a, x, top, x, y200, 5);
        for (int c = 0; c < 3; c++) CHECK(abs((int)lv->in_hi[c] - 200) <= 2);
        CHECK(lv->in_lo[0] == 0u && lv->out_hi[0] == 255u);
    }
    /* uncheck R and G: the gray point arrow only changes blue's gamma */
    f_click(a, (float)afx_levels_rect(a, AFX_LV_CHECK_R).x + 8.0f,
            (float)afx_levels_rect(a, AFX_LV_CHECK_R).y + 10.0f, SDL_BUTTON_LEFT);
    f_click(a, (float)afx_levels_rect(a, AFX_LV_CHECK_G).x + 8.0f,
            (float)afx_levels_rect(a, AFX_LV_CHECK_G).y + 10.0f, SDL_BUTTON_LEFT);
    CHECK(lv->mask == (1u << FX_CH_B));
    {
        ui_rect b = afx_levels_rect(a, AFX_LV_OUT_BAR);
        float x = (float)b.x + (float)b.w * 0.5f;
        float ymid = bot - (127.5f / 255.0f) * (bot - top);       /* gamma 1: mid at 127.5 */
        float yup = bot - (190.0f / 255.0f) * (bot - top);
        f_drag(a, x, ymid, x, yup, 5);
        CHECK(lv->gamma[FX_CH_B] < 0.75f && lv->gamma[FX_CH_B] > 0.4f);
        CHECK(lv->gamma[FX_CH_R] == 1.0f && lv->gamma[FX_CH_G] == 1.0f);
    }
    /* preview and commit equal the oracle */
    CHECK(afx_wait_preview(a, 200));
    CHECK(f_oracle(a, fx, afx_session_params(s), &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 100));
    CHECK(strcmp(d->hist->cur->label, "Levels") == 0);
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    pc_surf_free(&expect);
    app_destroy(a);
}

/* Auto-Level (no dialog) equals Levels with Auto on the whole image. */
static void t_auto_level_matches(void)
{
    app *a = f_app(W, H);
    pc_surf by_auto, by_levels;
    afx_session *s;
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.auto_level"));
    CHECK(afx_wait_idle(a, 100));
    CHECK(strcmp(d->hist->cur->label, "Auto-Level") == 0);
    CHECK(f_read_layer(a, &by_auto));
    CHECK(app_doc_undo(a, d));
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.levels"));
    CHECK(afx_wait_preview(a, 100));
    at_frames(a, 2);
    s = afx_active(a);
    f_click_rect(a, afx_levels_rect(a, AFX_LV_AUTO), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 100));
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 100));
    CHECK(f_read_layer(a, &by_levels));
    CHECK(f_diff(&by_auto, &by_levels, NULL, NULL) == 0);
    pc_surf_free(&by_auto);
    pc_surf_free(&by_levels);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_histogram_fn);
    RUN(t_dialog);
    RUN(t_auto_level_matches);
    at_quit();
    return pc_test_finish();
}
