/* test_ui_widgets.c - scripted input driving the basic widgets: buttons
 * (mouse and keyboard activation, press/release rules), check boxes, radios
 * and radio groups, switches, sliders (drag, keys, log mapping), numeric
 * fields (typing, clamping, Escape, keys, spin buttons with auto repeat and
 * dragging, wheel), the single-line text field (editing, selection, word
 * moves, clipboard, IME composition, capacity, filters), Tab navigation,
 * angle dial, point picker, property sliders, collapsing headers, tooltips,
 * redraw scheduling and cursor requests. */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct wstate {
    int     mode;
    /* buttons */
    int     clicks[4];
    ui_rect r[16];
    bool    hovered[4];
    /* values */
    bool    check, sw, toggle;
    int     radio, group;
    double  slider, logv, num;
    int32_t islider, inum, prop;
    char    text[16];
    char    hex[16];
    uint32_t edit;
    double  angle;
    ui_vec2 pt;
    bool    open;
    int     split;
} wstate;

static wstate W;

static void container(ui_ctx *ctx)
{
    ui_layout_push(ctx, ui_rect_make(10, 10, 300, 580), 0.0f);
}

/* ---- buttons ----------------------------------------------------------------- */
static void s_buttons(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    if (ui_button(ctx, "Apply")) W.clicks[0]++;
    W.r[0] = ui_last_rect(ctx);
    W.hovered[0] = ui_last_hovered(ctx);
    if (ui_button_ex(ctx, "Off", UI_ICON_NONE, UI_DISABLED)) W.clicks[1]++;
    W.r[1] = ui_last_rect(ctx);
    if (ui_icon_button(ctx, "##save", UI_ICON_SAVE, "Save")) W.clicks[2]++;
    W.r[2] = ui_last_rect(ctx);
    if (ui_toggle(ctx, "Grid", UI_ICON_GRID, &W.toggle)) W.clicks[3]++;
    W.r[3] = ui_last_rect(ctx);
    W.split = ui_split_button(ctx, "##split", UI_ICON_TOOL_SHAPES, true, NULL);
    W.r[4] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void t_buttons(void)
{
    ut_env e;
    float x, y;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.r[0].h == ui_px(e.ctx, ui_get_theme(e.ctx)->m.control_h));
    CHECK(W.r[0].w >= ui_px(e.ctx, 72.0f) && W.r[0].x == 10 && W.r[0].y == 10);
    /* hover */
    x = ut_cx(W.r[0]);
    y = ut_cy(W.r[0]);
    ut_move(&e, x, y);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.hovered[0] && ui_wants_mouse(e.ctx));
    /* a click (press and release inside) fires once */
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 1);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 1);
    /* press inside, release outside: no click */
    ut_button(&e, SDL_BUTTON_LEFT, true, x, y, 1);
    ut_frame(&e, s_buttons, NULL);
    CHECK(ui_is_active(e.ctx, ui_last_id(e.ctx)) == false);   /* last widget is the split */
    ut_move(&e, 380.0f, 280.0f);
    ut_frame(&e, s_buttons, NULL);
    ut_button(&e, SDL_BUTTON_LEFT, false, 380.0f, 280.0f, 1);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 1);
    /* press outside, release inside: no click either */
    ut_button(&e, SDL_BUTTON_LEFT, true, 380.0f, 280.0f, 1);
    ut_frame(&e, s_buttons, NULL);
    ut_move(&e, x, y);
    ut_button(&e, SDL_BUTTON_LEFT, false, x, y, 1);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 1);
    /* disabled buttons ignore clicks */
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[1] == 0);
    /* icon button and toggle */
    ut_click(&e, W.r[2]);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[2] == 1 && W.r[2].w == W.r[2].h);
    ut_click(&e, W.r[3]);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.toggle && W.clicks[3] == 1);
    ut_click(&e, W.r[3]);
    ut_frame(&e, s_buttons, NULL);
    CHECK(!W.toggle && W.clicks[3] == 2);
    /* split button: main part 1, arrow 2 */
    ut_click_at(&e, (float)W.r[4].x + 4.0f, ut_cy(W.r[4]));
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.split == 1);
    ut_click_at(&e, (float)(W.r[4].x + W.r[4].w) - 3.0f, ut_cy(W.r[4]));
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.split == 2);
    /* keyboard: Tab to the first button, Enter and Space activate it */
    ut_click_at(&e, 380.0f, 280.0f);                 /* empty area clears focus */
    ut_frame(&e, s_buttons, NULL);
    CHECK(ui_focus_id(e.ctx) == 0);
    ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    CHECK(ui_focus_visible(e.ctx) && ui_wants_keyboard(e.ctx));
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 2);
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[0] == 3);
    /* Tab skips the disabled button; Shift+Tab wraps backwards */
    ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.clicks[2] == 2 && W.clicks[1] == 0);
    ut_key(&e, SDLK_TAB, SDL_KMOD_LSHIFT);
    ut_key(&e, SDLK_TAB, SDL_KMOD_LSHIFT);
    ut_frame(&e, s_buttons, NULL);
    ut_frame(&e, s_buttons, NULL);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    CHECK(W.split == 1);                             /* wrapped to the last widget */
    ut_close(&e);
}

/* ---- events returned to the app ------------------------------------------------ */
static void t_event_routing(void)
{
    ut_env e;
    SDL_Event ev;
    const ui_key_press *keys;
    int n = 0;
    bool f5 = false, tab_used = false;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_buttons, NULL);
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = ut_cx(W.r[0]);
    ev.motion.y = ut_cy(W.r[0]);
    ui_event(e.ctx, &ev);
    ut_frame(&e, s_buttons, NULL);
    CHECK(ui_event(e.ctx, &ev));                     /* over a widget: the UI takes it */
    ev.motion.x = 390.0f;
    ev.motion.y = 290.0f;
    ui_event(e.ctx, &ev);
    ut_frame(&e, s_buttons, NULL);
    CHECK(!ui_event(e.ctx, &ev));                    /* over nothing: the app's */
    /* unused key presses stay visible to the app's shortcut handler */
    ut_key(&e, SDLK_F5, SDL_KMOD_LCTRL);
    ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
    ut_frame(&e, s_buttons, NULL);
    keys = ui_key_presses(e.ctx, &n);
    for (int i = 0; i < n; i++) {
        if (keys[i].key == SDLK_F5 && keys[i].mods == UI_MOD_CTRL && !keys[i].used) f5 = true;
        if (keys[i].key == SDLK_TAB && keys[i].used) tab_used = true;
    }
    CHECK(n == 2 && f5 && tab_used);
    CHECK(ui_mod_primary() == UI_MOD_CTRL || ui_mod_primary() == UI_MOD_GUI);
    ut_close(&e);
}

/* ---- check boxes, radios, switches ------------------------------------------------ */
static void s_choices(ui_ctx *ctx, void *ud)
{
    static const char *const items[] = { "One", "Two", "Three" };
    (void)ud;
    container(ctx);
    ui_checkbox(ctx, "Keep ratio", &W.check);
    W.r[0] = ui_last_rect(ctx);
    if (ui_radio(ctx, "Layer", &W.radio, 0)) W.clicks[0]++;
    W.r[1] = ui_last_rect(ctx);
    if (ui_radio(ctx, "Image", &W.radio, 1)) W.clicks[0]++;
    W.r[2] = ui_last_rect(ctx);
    ui_switch(ctx, "Snap", &W.sw);
    W.r[3] = ui_last_rect(ctx);
    if (ui_radio_group(ctx, "##group", &W.group, items, 3, false)) W.clicks[1]++;
    W.r[4] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void t_choices(void)
{
    ut_env e;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_choices, NULL);
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.check);
    ut_click_at(&e, (float)W.r[0].x + 3.0f, ut_cy(W.r[0]));   /* the box itself */
    ut_frame(&e, s_choices, NULL);
    CHECK(!W.check);
    ut_click(&e, W.r[2]);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.radio == 1 && W.clicks[0] == 1);
    ut_click(&e, W.r[2]);                              /* already selected: no change */
    ut_frame(&e, s_choices, NULL);
    CHECK(W.radio == 1 && W.clicks[0] == 1);
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.radio == 0 && W.clicks[0] == 2);
    ut_click(&e, W.r[3]);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.sw);
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);            /* focused switch toggles by key */
    ut_frame(&e, s_choices, NULL);
    CHECK(!W.sw);
    /* radio group: the last row is "Three"; click it, then arrows move */
    ut_click(&e, W.r[4]);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.group == 2 && W.clicks[1] == 1);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.group == 0);                               /* wraps */
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.group == 1);
    ut_key(&e, SDLK_UP, SDL_KMOD_NONE);
    ut_key(&e, SDLK_UP, SDL_KMOD_NONE);
    ut_frame(&e, s_choices, NULL);
    ut_frame(&e, s_choices, NULL);
    CHECK(W.group == 2 && W.clicks[1] == 4);
    ut_close(&e);
}

/* ---- sliders ------------------------------------------------------------------------ */
static void s_sliders(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    if (ui_slider_double(ctx, "##s", &W.slider, 0.0, 100.0, 1.0, 0)) W.clicks[0]++;
    W.r[0] = ui_last_rect(ctx);
    ui_slider_int(ctx, "##i", &W.islider, -10, 10, 0);
    W.r[1] = ui_last_rect(ctx);
    ui_slider_double(ctx, "##log", &W.logv, 1.0, 100.0, 0.0, UI_SLIDER_LOG);
    W.r[2] = ui_last_rect(ctx);
    ui_slider_double(ctx, "##dis", &W.num, 0.0, 1.0, 0.0, UI_DISABLED);
    W.r[3] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void drag(ut_env *e, ut_scene_fn fn, float x0, float y0, float x1, float y1)
{
    ut_move(e, x0, y0);
    ut_button(e, SDL_BUTTON_LEFT, true, x0, y0, 1);
    ut_frame(e, fn, NULL);
    ut_move(e, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    ut_frame(e, fn, NULL);
    ut_move(e, x1, y1);
    ut_frame(e, fn, NULL);
    ut_button(e, SDL_BUTTON_LEFT, false, x1, y1, 1);
    ut_frame(e, fn, NULL);
}

static void t_sliders(void)
{
    ut_env e;
    float half, x0, x1, y;
    memset(&W, 0, sizeof W);
    W.slider = 25.0;
    W.logv = 1.0;
    W.num = 0.5;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_sliders, NULL);
    half = (float)ui_px(e.ctx, ui_get_theme(e.ctx)->m.slider_thumb) * 0.5f;
    x0 = (float)W.r[0].x + half;
    x1 = (float)(W.r[0].x + W.r[0].w) - half;
    y = ut_cy(W.r[0]);
    /* press on the track jumps there, dragging follows, and it clamps */
    drag(&e, s_sliders, x0, y, (x0 + x1) * 0.5f, y);
    CHECK(fabs(W.slider - 50.0) < 1e-9);
    drag(&e, s_sliders, (x0 + x1) * 0.5f, y, x1 + 80.0f, y);
    CHECK(W.slider == 100.0);
    drag(&e, s_sliders, x0 + (x1 - x0) * 0.333f, y, x0 + (x1 - x0) * 0.333f, y);
    CHECK(W.slider == 33.0);                           /* snapped to the step */
    /* keys while focused */
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 34.0);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_LSHIFT);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 24.0);
    ut_key(&e, SDLK_PAGEUP, SDL_KMOD_NONE);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 34.0);
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 100.0);
    ut_key(&e, SDLK_HOME, SDL_KMOD_NONE);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 0.0);
    ut_move(&e, ut_cx(W.r[0]), y);
    ut_wheel(&e, 0.0f, 1.0f);
    ut_frame(&e, s_sliders, NULL);
    CHECK(W.slider == 1.0);
    /* integer slider rounds */
    x0 = (float)W.r[1].x + half;
    x1 = (float)(W.r[1].x + W.r[1].w) - half;
    drag(&e, s_sliders, x0, ut_cy(W.r[1]), x0 + (x1 - x0) * 0.76f, ut_cy(W.r[1]));
    CHECK(W.islider == 5);
    /* logarithmic mapping: the middle of 1..100 is 10 */
    x0 = (float)W.r[2].x + half;
    x1 = (float)(W.r[2].x + W.r[2].w) - half;
    drag(&e, s_sliders, (x0 + x1) * 0.5f, ut_cy(W.r[2]), (x0 + x1) * 0.5f, ut_cy(W.r[2]));
    CHECK(fabs(W.logv - 10.0) < 0.1);
    /* disabled sliders ignore input */
    drag(&e, s_sliders, ut_cx(W.r[3]) - 50.0f, ut_cy(W.r[3]), ut_cx(W.r[3]) + 80.0f,
         ut_cy(W.r[3]));
    CHECK(W.num == 0.5);
    ut_close(&e);
}

/* ---- numeric fields ------------------------------------------------------------------- */
static void s_number(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    if (ui_number_double(ctx, "##n", &W.num, 0.0, 100.0, 0.5, 1, 0)) W.clicks[0]++;
    W.r[0] = ui_last_rect(ctx);
    ui_number_int(ctx, "##ni", &W.inum, -5, 5, 1, 0);
    W.r[1] = ui_last_rect(ctx);
    ui_number_double(ctx, "##pct", &W.slider, 0.0, 200.0, 1.0, 1, UI_SLIDER_PERCENT);
    W.r[2] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void t_number(void)
{
    ut_env e;
    float tx, ty, sx, up_y, dn_y;
    int32_t bw;
    memset(&W, 0, sizeof W);
    W.num = 10.0;
    W.slider = 65.0;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_number, NULL);
    bw = ui_px(e.ctx, 18.0f);
    tx = (float)W.r[0].x + 20.0f;
    ty = ut_cy(W.r[0]);
    sx = (float)(W.r[0].x + W.r[0].w - bw / 2);
    up_y = (float)W.r[0].y + (float)W.r[0].h * 0.25f;
    dn_y = (float)W.r[0].y + (float)W.r[0].h * 0.75f;
    /* typing: focus selects everything, typed values in range apply live */
    ut_click_at(&e, tx, ty);
    ut_frame(&e, s_number, NULL);
    CHECK(ui_text_input_active(e.ctx));
    ut_text(&e, "42.5");
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 42.5);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 42.5);
    /* out of range input clamps on Enter */
    ut_key(&e, SDLK_A, (SDL_Keymod)(ui_mod_primary() == UI_MOD_GUI ? SDL_KMOD_LGUI
                                                                   : SDL_KMOD_LCTRL));
    ut_text(&e, "999");
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 42.5);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 100.0);
    /* Escape restores the value from when the field got focus */
    ut_click_at(&e, 380.0f, 280.0f);
    ut_frame(&e, s_number, NULL);
    ut_click_at(&e, tx, ty);
    ut_frame(&e, s_number, NULL);
    ut_text(&e, "7");
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 7.0);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 100.0);
    CHECK(!ui_text_input_active(e.ctx) || ui_focus_id(e.ctx) == 0);
    /* keys step the value while focused */
    ut_click_at(&e, tx, ty);
    ut_frame(&e, s_number, NULL);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 99.5);
    ut_key(&e, SDLK_PAGEDOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 94.5);
    ut_key(&e, SDLK_UP, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 95.0);
    /* the wheel over the field */
    ut_move(&e, tx, ty);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 94.5);
    /* spin buttons: click, hold with auto repeat, vertical drag */
    ut_click_at(&e, sx, up_y);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 95.0);
    ut_click_at(&e, sx, dn_y);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 94.5);
    ut_move(&e, sx, up_y);
    ut_button(&e, SDL_BUTTON_LEFT, true, sx, up_y, 1);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 94.5);
    ut_frame(&e, s_number, NULL);                     /* held: the next repeat is due */
    CHECK(ui_wait_timeout(e.ctx, e.t) > 0 && ui_wait_timeout(e.ctx, e.t) <= 400);
    ut_advance(&e, 400);
    ut_frame(&e, s_number, NULL);                     /* first repeat after 400 ms */
    CHECK(W.num == 95.0);
    ut_advance(&e, 60);
    ut_frame(&e, s_number, NULL);                     /* then every 60 ms */
    ut_advance(&e, 60);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 96.0);
    ut_button(&e, SDL_BUTTON_LEFT, false, sx, up_y, 1);
    ut_frame(&e, s_number, NULL);
    W.num = 50.0;
    ut_move(&e, sx, dn_y);
    ut_button(&e, SDL_BUTTON_LEFT, true, sx, dn_y, 1);
    ut_frame(&e, s_number, NULL);
    ut_move(&e, sx, dn_y - 20.0f);
    ut_frame(&e, s_number, NULL);
    ut_move(&e, sx, dn_y - 40.0f);
    ut_frame(&e, s_number, NULL);
    CHECK(W.num == 55.0);                             /* 4 px per step, 10 steps up */
    CHECK(ui_get_cursor(e.ctx) == UI_CURSOR_NS);
    ut_button(&e, SDL_BUTTON_LEFT, false, sx, dn_y - 40.0f, 1);
    ut_frame(&e, s_number, NULL);
    /* integer field */
    ut_click_at(&e, (float)W.r[1].x + 20.0f, ut_cy(W.r[1]));
    ut_frame(&e, s_number, NULL);
    ut_text(&e, "-3");
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.inum == -3);
    ut_key(&e, SDLK_PAGEDOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.inum == -5);
    /* percent: "%" is accepted in typed text */
    ut_click_at(&e, (float)W.r[2].x + 20.0f, ut_cy(W.r[2]));
    ut_frame(&e, s_number, NULL);
    ut_text(&e, "120%");
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_number, NULL);
    CHECK(W.slider == 120.0);
    ut_close(&e);
}

/* ---- text field ----------------------------------------------------------------------- */
static void s_text(ui_ctx *ctx, void *ud)
{
    uint32_t flags = ud ? *(const uint32_t *)ud : 0u;
    container(ctx);
    W.edit |= ui_text_field_ex(ctx, "##name", W.text, sizeof W.text, flags, "Name");
    W.r[0] = ui_last_rect(ctx);
    W.edit |= ui_text_field(ctx, "##hex", W.hex, sizeof W.hex, UI_EDIT_HEX) << 8;
    W.r[1] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static SDL_Keymod prim(void)
{
    return ui_mod_primary() == UI_MOD_GUI ? SDL_KMOD_LGUI : SDL_KMOD_LCTRL;
}

static void type_frame(ut_env *e, const char *s)
{
    ut_text(e, s);
    ut_frame(e, s_text, NULL);
}

static void key_frame(ut_env *e, SDL_Keycode k, SDL_Keymod m)
{
    ut_key(e, k, m);
    ut_frame(e, s_text, NULL);
}

static void t_text_field(void)
{
    ut_env e;
    uint64_t h_plain, h_comp;
    uint32_t ro = UI_EDIT_READONLY;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_text, NULL);
    ut_move(&e, ut_cx(W.r[0]), ut_cy(W.r[0]));
    ut_frame(&e, s_text, NULL);
    CHECK(ui_get_cursor(e.ctx) == UI_CURSOR_TEXT);
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_text, NULL);
    CHECK(ui_text_input_active(e.ctx) && ui_wants_keyboard(e.ctx));
    W.edit = 0;
    type_frame(&e, "Hello");
    CHECK(strcmp(W.text, "Hello") == 0 && (W.edit & UI_EDIT_CHANGED));
    key_frame(&e, SDLK_LEFT, SDL_KMOD_NONE);
    key_frame(&e, SDLK_LEFT, SDL_KMOD_NONE);
    type_frame(&e, "XY");
    CHECK(strcmp(W.text, "HelXYlo") == 0);
    /* selection by Shift+Home, replaced by typing */
    key_frame(&e, SDLK_HOME, SDL_KMOD_LSHIFT);
    type_frame(&e, "A");
    CHECK(strcmp(W.text, "Alo") == 0);
    key_frame(&e, SDLK_END, SDL_KMOD_LSHIFT);           /* select "lo" */
    key_frame(&e, SDLK_BACKSPACE, SDL_KMOD_NONE);
    CHECK(strcmp(W.text, "A") == 0);
    /* clipboard: select all, copy, paste twice at the end */
    key_frame(&e, SDLK_A, prim());
    key_frame(&e, SDLK_C, prim());
    key_frame(&e, SDLK_END, SDL_KMOD_NONE);
    key_frame(&e, SDLK_V, prim());
    key_frame(&e, SDLK_V, prim());
    CHECK(strcmp(W.text, "AAA") == 0);
    key_frame(&e, SDLK_LEFT, SDL_KMOD_LSHIFT);
    key_frame(&e, SDLK_X, prim());
    CHECK(strcmp(W.text, "AA") == 0);
    /* words: Ctrl+Backspace removes one word, Ctrl+Left jumps a word */
    key_frame(&e, SDLK_A, prim());
    type_frame(&e, "foo bar baz");
    key_frame(&e, SDLK_BACKSPACE, prim());
    CHECK(strcmp(W.text, "foo bar ") == 0);
    key_frame(&e, SDLK_LEFT, prim());
    type_frame(&e, "_");
    CHECK(strcmp(W.text, "foo _bar ") == 0);
    key_frame(&e, SDLK_DELETE, prim());
    CHECK(strcmp(W.text, "foo _") == 0);
    /* UTF-8: arrows and Backspace move over whole code points */
    key_frame(&e, SDLK_A, prim());
    type_frame(&e, "a\xC3\xA9\xE6\xB0\xB4");              /* a, e acute, CJK water */
    CHECK(strlen(W.text) == 6);
    key_frame(&e, SDLK_LEFT, SDL_KMOD_NONE);
    key_frame(&e, SDLK_BACKSPACE, SDL_KMOD_NONE);
    CHECK(strcmp(W.text, "a\xE6\xB0\xB4") == 0);
    /* IME composition is shown but not inserted until committed */
    ut_frame(&e, s_text, NULL);
    ut_render(&e);
    h_plain = ut_hash(&e);
    ut_editing(&e, "\xE3\x81\xAB\xE3\x81\xBB", 1, 0);   /* "niho" in hiragana */
    ut_frame(&e, s_text, NULL);
    ut_render(&e);
    h_comp = ut_hash(&e);
    CHECK(strcmp(W.text, "a\xE6\xB0\xB4") == 0);
    CHECK(h_plain != h_comp);
    ut_editing(&e, "", 0, 0);
    type_frame(&e, "\xE6\x97\xA5");                       /* commit */
    CHECK(strcmp(W.text, "a\xE6\x97\xA5\xE6\xB0\xB4") == 0);
    /* capacity: 15 bytes plus NUL, never splitting a code point */
    key_frame(&e, SDLK_A, prim());
    type_frame(&e, "0123456789abc\xE6\xB0\xB4\xE6\xB0\xB4");
    CHECK(strlen(W.text) == 13);
    type_frame(&e, "xy");
    CHECK(strcmp(W.text, "0123456789abcxy") == 0);
    type_frame(&e, "z");
    CHECK(strlen(W.text) == 15);
    /* Enter submits; Escape restores the text from when focus arrived */
    W.edit = 0;
    key_frame(&e, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(W.edit & UI_EDIT_SUBMIT);
    ut_click_at(&e, 380.0f, 280.0f);
    ut_frame(&e, s_text, NULL);
    ut_frame(&e, s_text, NULL);
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_text, NULL);
    key_frame(&e, SDLK_A, prim());
    type_frame(&e, "changed");
    W.edit = 0;
    key_frame(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK((W.edit & UI_EDIT_CANCEL) && strcmp(W.text, "0123456789abcxy") == 0);
    CHECK(ui_focus_id(e.ctx) == 0);
    /* focus loss reports DEACTIVATED */
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_text, NULL);
    W.edit = 0;
    ut_click_at(&e, 380.0f, 280.0f);
    ut_frame(&e, s_text, NULL);
    ut_frame(&e, s_text, NULL);
    CHECK(W.edit & UI_EDIT_DEACTIVATED);
    CHECK(!ui_text_input_active(e.ctx));
    /* double click selects a word */
    strcpy(W.text, "alpha beta");
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_text, NULL);
    {
        float x = (float)W.r[0].x + (float)ui_px(e.ctx, 8.0f) +
                  ui_text_width(ui_font_regular(e.ctx), ui_font_px(e.ctx), "alpha be", 8);
        ut_click_at_btn(&e, SDL_BUTTON_LEFT, x, ut_cy(W.r[0]), 2);
        ut_frame(&e, s_text, NULL);
        type_frame(&e, "gamma");
        CHECK(strcmp(W.text, "alpha gamma") == 0);
    }
    /* mouse drag selection from the start to the end */
    {
        float xs = (float)W.r[0].x + (float)ui_px(e.ctx, 8.0f) + 1.0f;
        float xe = (float)(W.r[0].x + W.r[0].w) - 4.0f, y = ut_cy(W.r[0]);
        ut_move(&e, xs, y);
        ut_button(&e, SDL_BUTTON_LEFT, true, xs, y, 1);
        ut_frame(&e, s_text, NULL);
        ut_move(&e, xe, y);
        ut_frame(&e, s_text, NULL);
        ut_button(&e, SDL_BUTTON_LEFT, false, xe, y, 1);
        ut_frame(&e, s_text, NULL);
        type_frame(&e, "Z");
        CHECK(strcmp(W.text, "Z") == 0);
    }
    /* the hex field filters characters */
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_text, NULL);
    type_frame(&e, "zz12abXY#9");
    CHECK(strcmp(W.hex, "12ab#9") == 0);
    /* read-only: typing and cutting do nothing, copying works */
    strcpy(W.text, "fixed");
    ut_click_at(&e, 380.0f, 280.0f);
    ut_frame(&e, s_text, &ro);
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_text, &ro);
    ut_text(&e, "x");
    ut_key(&e, SDLK_A, prim());
    ut_key(&e, SDLK_X, prim());
    ut_key(&e, SDLK_BACKSPACE, SDL_KMOD_NONE);
    ut_frame(&e, s_text, &ro);
    CHECK(strcmp(W.text, "fixed") == 0);
    ut_click_at(&e, 380.0f, 280.0f);
    ut_frame(&e, s_text, NULL);
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_text, NULL);
    ut_key(&e, SDLK_A, prim());
    ut_key(&e, SDLK_V, prim());
    ut_frame(&e, s_text, NULL);
    CHECK(strcmp(W.hex, "fed") == 0);                 /* "fixed" filtered to hex digits */
    ut_close(&e);
}

/* ---- angle dial and point picker ---------------------------------------------------------- */
static void s_angle(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    ui_angle(ctx, "##angle", &W.angle, -180.0, 180.0);
    W.r[0] = ui_last_rect(ctx);
    ui_point_picker(ctx, "##pt", &W.pt, NULL, 100.0f);
    W.r[1] = ui_last_rect(ctx);
    ui_layout_pop(ctx);
}

static void t_angle_point(void)
{
    ut_env e;
    float d, cx, cy;
    ui_rect img;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_angle, NULL);
    d = (float)ui_px(e.ctx, 64.0f);
    cx = (float)W.r[0].x + d * 0.5f;
    cy = (float)W.r[0].y + d * 0.5f;
    drag(&e, s_angle, cx + 20.0f, cy, cx + 25.0f, cy - 25.0f);
    CHECK(fabs(W.angle - 45.0) < 1.0);                 /* counter-clockwise from +x */
    drag(&e, s_angle, cx - 20.0f, cy + 0.01f, cx - 20.0f, cy + 0.01f);
    CHECK(fabs(fabs(W.angle) - 180.0) < 0.5);
    drag(&e, s_angle, cx, cy + 20.0f, cx, cy + 20.0f);
    CHECK(fabs(W.angle + 90.0) < 0.5);
    ut_mods(&e, SDL_KMOD_LSHIFT);
    drag(&e, s_angle, cx + 20.0f, cy - 17.0f, cx + 20.0f, cy - 17.0f);   /* 40.4 degrees */
    ut_mods(&e, SDL_KMOD_NONE);
    CHECK(W.angle == 45.0);                            /* Shift snaps to 15 degrees */
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    ut_frame(&e, s_angle, NULL);
    CHECK(W.angle == 44.0);
    /* point picker: normalized -1..1 over the area, clamped */
    img = ui_rect_inset(W.r[1], 1, 1);
    drag(&e, s_angle, ut_cx(img), ut_cy(img), (float)img.x - 30.0f, (float)img.y - 30.0f);
    CHECK(W.pt.x == -1.0f && W.pt.y == -1.0f);
    drag(&e, s_angle, (float)img.x, (float)img.y, (float)img.x + (float)img.w * 0.5f,
         (float)img.y + (float)img.h * 0.5f);
    CHECK(fabsf(W.pt.x) < 0.02f && fabsf(W.pt.y) < 0.02f);
    drag(&e, s_angle, ut_cx(img), ut_cy(img), (float)(img.x + img.w) + 9.0f,
         (float)img.y + (float)img.h * 0.75f);
    CHECK(W.pt.x == 1.0f && fabsf(W.pt.y - 0.5f) < 0.03f);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    ut_frame(&e, s_angle, NULL);
    CHECK(fabsf(W.pt.x - 0.99f) < 1e-4f);
    ut_close(&e);
}

/* ---- property slider, collapsing header, progress --------------------------------------- */
static void s_prop(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    if (ui_prop_slider_int(ctx, "Radius", &W.prop, 0, 100, 25, 0)) W.clicks[0]++;
    W.r[0] = ui_last_rect(ctx);                        /* the reset button */
    W.open = ui_collapsing(ctx, "Advanced", false);
    W.r[1] = ui_last_rect(ctx);
    if (W.open) ui_label(ctx, "Inside");
    ui_progress(ctx, -1.0f);
    ui_layout_pop(ctx);
}

static void t_prop(void)
{
    ut_env e;
    memset(&W, 0, sizeof W);
    W.prop = 25;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_prop, NULL);
    CHECK(W.r[0].w == W.r[0].h);                       /* square reset button */
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_prop, NULL);
    CHECK(W.prop == 25 && W.clicks[0] == 0);           /* disabled at the default */
    W.prop = 70;
    ut_frame(&e, s_prop, NULL);
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_prop, NULL);
    CHECK(W.prop == 25 && W.clicks[0] == 1);
    /* the reset button sits right of the numeric field on the slider row */
    CHECK(W.r[0].x + W.r[0].w == 310);
    CHECK(!W.open);
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_prop, NULL);
    ut_frame(&e, s_prop, NULL);
    CHECK(W.open);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);              /* focused header: Left closes */
    ut_frame(&e, s_prop, NULL);
    ut_frame(&e, s_prop, NULL);
    CHECK(!W.open);
    /* the indeterminate progress bar animates at about 30 frames per second */
    CHECK(ui_wait_timeout(e.ctx, e.t) >= 0 && ui_wait_timeout(e.ctx, e.t) <= 33);
    ut_close(&e);
}

/* ---- tooltips and redraw scheduling ------------------------------------------------------ */
static void s_tip(ui_ctx *ctx, void *ud)
{
    (void)ud;
    container(ctx);
    if (ui_icon_button(ctx, "##tip", UI_ICON_HELP, "Help topics")) W.clicks[0]++;
    W.r[0] = ui_last_rect(ctx);
    if (W.mode == 1) {
        ui_text_field(ctx, "##t", W.text, sizeof W.text, 0);
        W.r[1] = ui_last_rect(ctx);
    }
    ui_layout_pop(ctx);
}

static void t_tooltip_redraw(void)
{
    ut_env e;
    uint64_t h0, h1;
    int32_t wait;
    float delay;
    memset(&W, 0, sizeof W);
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    delay = ui_get_theme(e.ctx)->m.tooltip_delay_ms;
    ut_frames(&e, 2, s_tip, NULL);
    CHECK(!ui_needs_frame(e.ctx, e.t) && ui_wait_timeout(e.ctx, e.t) == -1);
    ut_render(&e);
    h0 = ut_hash(&e);
    ut_move(&e, ut_cx(W.r[0]), ut_cy(W.r[0]));
    CHECK(ui_needs_frame(e.ctx, e.t));                 /* input arrived */
    ut_frame(&e, s_tip, NULL);                         /* hover starts */
    ut_frame(&e, s_tip, NULL);
    wait = ui_wait_timeout(e.ctx, e.t);
    CHECK(wait > 0 && (float)wait <= delay);
    ut_advance(&e, (uint64_t)wait);
    CHECK(ui_needs_frame(e.ctx, e.t) && ui_wait_timeout(e.ctx, e.t) == 0);
    ut_frame(&e, s_tip, NULL);
    ut_render(&e);
    h1 = ut_hash(&e);
    CHECK(h0 != h1);
    {
        /* the tooltip box sits below the button */
        uint32_t bg = ut_pixel(&e, 200, 150);
        int y = W.r[0].y + W.r[0].h + ui_px(e.ctx, 14.0f);
        int x = (int)ut_cx(W.r[0]) + 10;
        CHECK(ut_pixel(&e, x, y) != bg || ut_pixel(&e, x + 10, y) != bg);
    }
    /* pressing hides the tooltip for this hover */
    ut_click(&e, W.r[0]);
    ut_frame(&e, s_tip, NULL);
    ut_advance(&e, 2000);
    ut_frame(&e, s_tip, NULL);
    ut_render(&e);
    CHECK(W.clicks[0] == 1);
    CHECK(ui_wait_timeout(e.ctx, e.t) == -1);
    /* a focused text field blinks its caret */
    W.mode = 1;
    ut_frame(&e, s_tip, NULL);
    ut_click(&e, W.r[1]);
    ut_frame(&e, s_tip, NULL);
    ut_frame(&e, s_tip, NULL);
    wait = ui_wait_timeout(e.ctx, e.t);
    CHECK(wait > 0 && (float)wait <= ui_get_theme(e.ctx)->m.caret_blink_ms);
    /* explicit requests */
    ui_request_frame(e.ctx);
    CHECK(ui_needs_frame(e.ctx, e.t) && ui_wait_timeout(e.ctx, e.t) == 0);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_buttons);
    RUN(t_event_routing);
    RUN(t_choices);
    RUN(t_sliders);
    RUN(t_number);
    RUN(t_text_field);
    RUN(t_angle_point);
    RUN(t_prop);
    RUN(t_tooltip_redraw);
    SDL_Quit();
    return pc_test_finish();
}
