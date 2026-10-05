/* test_ui_window.c - modal dialogs (measuring, autofocus, modality, Enter
 * from the dialog and from its fields, Escape, close button, dragging, Tab
 * scope), message boxes, and floating panels (dragging, snapping to edges
 * and other panels, anchors, closing, resizing, z-order, clamping to the
 * panel area, DIP state across scales). */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct dstate {
    bool     show, msg;
    uint32_t result;
    int      behind, inside;
    char     name[32];
    double   width;
    ui_rect  ok, field, behind_r, num;
    int      kind;                   /* 0 dialog, 1 message box */
} dstate;

static dstate D;

static void s_dialog(ui_ctx *ctx, void *ud)
{
    (void)ud;
    ui_layout_push(ctx, ui_rect_make(10, 10, 200, 100), 0.0f);
    if (ui_button(ctx, "Behind")) D.behind++;
    D.behind_r = ui_last_rect(ctx);
    ui_layout_pop(ctx);
    if (D.show && D.kind == 0) {
        uint32_t r;
        ui_dialog_begin(ctx, "Resize", 320.0f, 0.0f);
        ui_text_field(ctx, "##name", D.name, sizeof D.name, 0);
        D.field = ui_last_rect(ctx);
        ui_number_double(ctx, "##w", &D.width, 1.0, 1000.0, 1.0, 0, 0);
        D.num = ui_last_rect(ctx);
        if (ui_button(ctx, "Inside")) D.inside++;
        ui_dialog_buttons(ctx, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
        D.ok = ui_rect_make(0, 0, 0, 0);
        r = ui_dialog_end(ctx);
        if (r) { D.result = r; D.show = false; }
    } else if (D.show) {
        uint32_t r = ui_message_box(ctx, "Unsaved changes", "Save the image before closing?",
                                    UI_ICON_WARNING, UI_DLG_YES | UI_DLG_NO, UI_DLG_YES);
        if (r) { D.result = r; D.show = false; }
    }
}

static void open_dialog(ut_env *e, int kind)
{
    D.show = true;
    D.kind = kind;
    D.result = 0;
    ut_frames(e, 3, s_dialog, NULL);   /* measure, show, settle autofocus */
}

static void t_dialog(void)
{
    ut_env e;
    memset(&D, 0, sizeof D);
    D.width = 640.0;
    if (!ut_open(&e, 640, 480, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_dialog, NULL);
    open_dialog(&e, 0);
    /* centered horizontally, autofocused first field, modal */
    CHECK(abs((D.field.x + D.field.w / 2) - 320) <= 2);
    CHECK(ui_text_input_active(e.ctx) && ui_wants_keyboard(e.ctx));
    ut_click(&e, D.behind_r);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.behind == 0 && D.show);
    ut_move(&e, ut_cx(D.behind_r), ut_cy(D.behind_r));
    ut_frame(&e, s_dialog, NULL);
    CHECK(ui_wants_mouse(e.ctx));                      /* the backdrop takes the pointer */
    /* typing goes to the field; Enter in the field presses OK */
    ut_text(&e, "image");
    ut_frame(&e, s_dialog, NULL);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_OK && !D.show && strcmp(D.name, "image") == 0);
    ut_frame(&e, s_dialog, NULL);
    CHECK(!ui_wants_keyboard(e.ctx));
    /* the field commits before OK when Enter is pressed in a numeric field */
    open_dialog(&e, 0);
    ut_click_at(&e, (float)D.num.x + 10.0f, ut_cy(D.num));
    ut_frame(&e, s_dialog, NULL);
    ut_text(&e, "800");
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_OK && D.width == 800.0);
    /* Escape cancels (the unmodified field lets it through) */
    open_dialog(&e, 0);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_CANCEL);
    /* a modified field takes the first Escape (revert), the second closes */
    open_dialog(&e, 0);
    ut_text(&e, "x");
    ut_frame(&e, s_dialog, NULL);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.show && strcmp(D.name, "image") == 0);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(!D.show && D.result == UI_DLG_CANCEL);
    /* Tab stays inside the dialog: field, number, Inside, OK, Cancel, field */
    open_dialog(&e, 0);
    for (int i = 0; i < 2; i++) {
        ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
        ut_frame(&e, s_dialog, NULL);
    }
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.inside == 1);
    for (int i = 0; i < 3; i++) {
        ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
        ut_frame(&e, s_dialog, NULL);
    }
    ut_text(&e, "!");                  /* back in the field: Tab selected its text */
    ut_frame(&e, s_dialog, NULL);
    CHECK(strcmp(D.name, "!") == 0 && D.behind == 0);
    ut_key(&e, SDLK_TAB, SDL_KMOD_LSHIFT);             /* back to Cancel */
    ut_frame(&e, s_dialog, NULL);
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_CANCEL);
    ut_close(&e);
}

static void t_dialog_drag(void)
{
    ut_env e;
    ui_rect f0;
    int32_t th;
    float x, y;
    memset(&D, 0, sizeof D);
    D.width = 10.0;
    if (!ut_open(&e, 640, 480, 1.0f)) { CHECK(0); ut_close(&e); return; }
    th = ui_px(e.ctx, ui_get_theme(e.ctx)->m.title_h);
    open_dialog(&e, 0);
    f0 = D.field;
    /* the title bar is above the first field; drag it by (+60, -40) */
    x = (float)f0.x + 20.0f;
    y = (float)f0.y - (float)th / 2.0f - 4.0f;
    ut_move(&e, x, y);
    ut_button(&e, SDL_BUTTON_LEFT, true, x, y, 1);
    ut_frame(&e, s_dialog, NULL);
    ut_move(&e, x + 30.0f, y - 20.0f);
    ut_frame(&e, s_dialog, NULL);
    ut_move(&e, x + 60.0f, y - 40.0f);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.field.x == f0.x + 60 && D.field.y == f0.y - 40);   /* no frame of lag */
    ut_button(&e, SDL_BUTTON_LEFT, false, x + 60.0f, y - 40.0f, 1);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.show);
    /* the close button (top right of the dialog) cancels */
    {
        int32_t right = f0.x + 60 + f0.w + ui_px(e.ctx, 16.0f);
        ut_click_at(&e, (float)right - (float)th * 0.5f, (float)(f0.y - 40) - (float)th * 0.5f -
                                                             4.0f);
        ut_frame(&e, s_dialog, NULL);
        CHECK(D.result == UI_DLG_CANCEL && !D.show);
    }
    ut_close(&e);
}

static void t_message_box(void)
{
    ut_env e;
    memset(&D, 0, sizeof D);
    if (!ut_open(&e, 640, 480, 1.0f)) { CHECK(0); ut_close(&e); return; }
    open_dialog(&e, 1);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_YES);
    open_dialog(&e, 1);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);            /* no Cancel button: No */
    ut_frame(&e, s_dialog, NULL);
    CHECK(D.result == UI_DLG_NO);
    ut_close(&e);
}

/* ---- panels ------------------------------------------------------------------------ */
typedef struct pstate {
    ui_panel_state a, b;
    ui_rect        ra, rb, btn_a, btn_b;
    int            clicks_a, clicks_b;
    ui_rect        area;
    bool           use_area;
} pstate;

static pstate P;

static void s_panels(ui_ctx *ctx, void *ud)
{
    (void)ud;
    if (P.use_area) ui_panels_area(ctx, P.area);
    if (ui_panel_begin(ctx, "Tools", &P.a, UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE)) {
        if (ui_button(ctx, "A")) P.clicks_a++;
        P.btn_a = ui_last_rect(ctx);
        ui_panel_end(ctx);
    }
    P.ra = ui_panel_rect(ctx, "Tools");
    if (ui_panel_begin(ctx, "Layers", &P.b, UI_PANEL_CLOSABLE)) {
        if (ui_button(ctx, "B")) P.clicks_b++;
        P.btn_b = ui_last_rect(ctx);
        ui_panel_end(ctx);
    }
    P.rb = ui_panel_rect(ctx, "Layers");
}

static ui_panel_state pstate_make(float x, float y, float w, float h, uint8_t ax, uint8_t ay)
{
    ui_panel_state s;
    s.x = x; s.y = y; s.w = w; s.h = h;
    s.anchor_x = ax; s.anchor_y = ay;
    s.open = true;
    return s;
}

static void pdrag(ut_env *e, float x, float y, float dx, float dy)
{
    ut_move(e, x, y);
    ut_button(e, SDL_BUTTON_LEFT, true, x, y, 1);
    ut_frame(e, s_panels, NULL);
    ut_move(e, x + dx * 0.5f, y + dy * 0.5f);
    ut_frame(e, s_panels, NULL);
    ut_move(e, x + dx, y + dy);
    ut_frame(e, s_panels, NULL);
    ut_button(e, SDL_BUTTON_LEFT, false, x + dx, y + dy, 1);
    ut_frame(e, s_panels, NULL);
}

static void t_panels(void)
{
    ut_env e;
    int32_t th;
    memset(&P, 0, sizeof P);
    P.a = pstate_make(20.0f, 20.0f, 200.0f, 150.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    P.b = pstate_make(20.0f, 20.0f, 220.0f, 160.0f, UI_ANCHOR_END, UI_ANCHOR_END);
    if (!ut_open(&e, 800, 600, 1.0f)) { CHECK(0); ut_close(&e); return; }
    th = ui_px(e.ctx, 28.0f);
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(P.ra.x == 20 && P.ra.y == 20 && P.ra.w == 200 && P.ra.h == 150);
    CHECK(P.rb.x == 800 - 20 - 220 && P.rb.y == 600 - 20 - 160);   /* anchored bottom right */
    /* move by the title bar */
    pdrag(&e, 60.0f, 20.0f + (float)th * 0.5f, 100.0f, 50.0f);
    CHECK(P.ra.x == 120 && P.ra.y == 70);
    CHECK(P.a.x == 120.0f && P.a.y == 70.0f && P.a.anchor_x == UI_ANCHOR_START);
    /* dropped near the right edge it snaps there and anchors to the right */
    pdrag(&e, 160.0f, 70.0f + (float)th * 0.5f, 800.0f - 320.0f - 6.0f, 0.0f);
    CHECK(P.ra.x + P.ra.w == 800);
    CHECK(P.a.anchor_x == UI_ANCHOR_END && P.a.x == 0.0f);
    /* snapping to another panel's edge: put a's bottom near b's top */
    pdrag(&e, (float)P.ra.x + 40.0f, (float)P.ra.y + (float)th * 0.5f, 0.0f,
          (float)(P.rb.y - (P.ra.y + P.ra.h)) - 6.0f);
    CHECK(P.ra.y + P.ra.h == P.rb.y);
    /* the area clamps panels (dragging far outside) */
    pdrag(&e, (float)P.ra.x + 40.0f, (float)P.ra.y + (float)th * 0.5f, -2000.0f, -2000.0f);
    CHECK(P.ra.x == 0 && P.ra.y == 0);
    /* resize from the right edge */
    {
        float ex = (float)(P.ra.x + P.ra.w) - 2.0f, ey = (float)P.ra.y + 80.0f;
        int32_t w0 = P.ra.w;
        ut_move(&e, ex, ey);
        ut_frame(&e, s_panels, NULL);
        CHECK(ui_get_cursor(e.ctx) == UI_CURSOR_EW);
        pdrag(&e, ex, ey, 40.0f, 0.0f);
        CHECK(P.ra.w == w0 + 40 && P.a.w == (float)(w0 + 40));
        ut_move(&e, (float)(P.ra.x + P.ra.w) - 2.0f, (float)(P.ra.y + P.ra.h) - 2.0f);
        ut_frame(&e, s_panels, NULL);
        CHECK(ui_get_cursor(e.ctx) == UI_CURSOR_NWSE);
        pdrag(&e, (float)(P.ra.x + P.ra.w) - 2.0f, (float)(P.ra.y + P.ra.h) - 2.0f, -500.0f,
              -500.0f);
        CHECK(P.ra.w == ui_px(e.ctx, 120.0f) && P.ra.h == ui_px(e.ctx, 80.0f));   /* minimum */
    }
    /* z-order: overlap the panels; the most recently clicked one is on top */
    P.a = pstate_make(100.0f, 100.0f, 200.0f, 150.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    P.b = pstate_make(150.0f, 120.0f, 220.0f, 160.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    ut_frames(&e, 2, s_panels, NULL);
    ut_click(&e, P.btn_a);                             /* raises a (it was declared first) */
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(P.clicks_a == 1);
    ut_click_at(&e, (float)P.rb.x + 30.0f, (float)P.rb.y + (float)th * 0.5f);   /* b's title */
    ut_frames(&e, 2, s_panels, NULL);
    ut_click_at(&e, 200.0f, 160.0f);                   /* inside both: b is on top */
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(ui_panel_rect(e.ctx, "Layers").x == 150);
    ut_click_at(&e, 110.0f, 200.0f);                   /* a only: raise a again */
    ut_frames(&e, 2, s_panels, NULL);
    ut_move(&e, 200.0f, 160.0f);
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(ui_wants_mouse(e.ctx));
    /* the close button */
    ut_move(&e, (float)(P.ra.x + P.ra.w) - (float)th * 0.5f, (float)P.ra.y + (float)th * 0.5f);
    ut_frame(&e, s_panels, NULL);
    ut_click_at(&e, (float)(P.ra.x + P.ra.w) - (float)th * 0.5f,
                (float)P.ra.y + (float)th * 0.5f);
    ut_frame(&e, s_panels, NULL);
    CHECK(!P.a.open && !ui_rect_empty(P.ra));          /* still shown in the click frame */
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(ui_rect_empty(P.ra));
    ut_close(&e);
}

/* Panel state is stored in DIPs, so the same state lays out at any scale;
 * a custom panel area keeps panels inside it. */
static void t_panel_scale(void)
{
    ut_env e;
    memset(&P, 0, sizeof P);
    P.a = pstate_make(10.0f, 20.0f, 200.0f, 150.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    P.b = pstate_make(10.0f, 10.0f, 100.0f, 100.0f, UI_ANCHOR_END, UI_ANCHOR_START);
    if (!ut_open(&e, 800, 600, 2.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(P.ra.x == 20 && P.ra.y == 40 && P.ra.w == 400 && P.ra.h == 300);
    CHECK(P.rb.x == 800 - 20 - 200 && P.rb.w == 200);
    P.use_area = true;
    P.area = ui_rect_make(100, 100, 300, 200);
    ut_frames(&e, 2, s_panels, NULL);
    CHECK(P.ra.x >= 100 && P.ra.y >= 100 && P.ra.x + P.ra.w <= 400 && P.ra.y + P.ra.h <= 300);
    CHECK(P.rb.x + P.rb.w <= 400 && P.rb.x >= 100);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_dialog);
    RUN(t_dialog_drag);
    RUN(t_message_box);
    RUN(t_panels);
    RUN(t_panel_scale);
    SDL_Quit();
    return pc_test_finish();
}
