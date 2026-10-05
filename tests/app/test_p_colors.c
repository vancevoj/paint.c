/* test_p_colors.c - lane P: the Colors window (WINDOWS.md 7) and palette
 * files (7.1, 7.2). Pure parts (3.36 HSV, wheel geometry and modifiers,
 * palette parsing, formatting, names) and the window driven like a user:
 * swatches, active slot, swap, reset, More / Less, hex box, channel boxes
 * and bars, the wheel with both buttons and modifiers, Add Color, the
 * palette menu (save, load, reset, folder) and persistence. */
#include "pc_test.h"
#include "p_test_util.h"

static pc_px32 rgb(uint8_t r, uint8_t g, uint8_t b) { return app_px_make(r, g, b, 255); }

static void t_hsv(void)
{
    pnl_hsv h;
    uint8_t r, g, b;
    h = pnl_rgb_to_hsv(255, 0, 0);
    CHECK(h.h == 0 && h.s == 100 && h.v == 100);
    h = pnl_rgb_to_hsv(128, 128, 128);
    CHECK(h.h == 0 && h.s == 0 && h.v == 50);
    h = pnl_rgb_to_hsv(0, 255, 255);
    CHECK(h.h == 180 && h.s == 100 && h.v == 100);
    h = pnl_rgb_to_hsv(255, 128, 0);
    CHECK(h.h == 30 && h.s == 100 && h.v == 100);
    h = pnl_rgb_to_hsv(0, 0, 0);
    CHECK(h.h == 0 && h.s == 0 && h.v == 0);
    h.h = 0; h.s = 100; h.v = 100;
    pnl_hsv_to_rgb(h, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 0);
    h.h = 120; h.s = 50; h.v = 50;
    pnl_hsv_to_rgb(h, &r, &g, &b);          /* 3.36 truncates: 0.25 * 255 = 63.75 -> 63 */
    CHECK(r == 63 && g == 127 && b == 63);
    h.h = 360; h.s = 100; h.v = 100;        /* 360 wraps to red */
    pnl_hsv_to_rgb(h, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 0);
    h.h = 240; h.s = 0; h.v = 50;           /* gray ignores the hue */
    pnl_hsv_to_rgb(h, &r, &g, &b);
    CHECK(r == 127 && g == 127 && b == 127);
    /* every RGB maps to an HSV inside the ranges */
    for (int i = 0; i < 4096; i++) {
        uint8_t rr = (uint8_t)(i * 37), gg = (uint8_t)(i * 91 + 7), bb = (uint8_t)(i * 13 + 200);
        h = pnl_rgb_to_hsv(rr, gg, bb);
        CHECK(h.h >= 0 && h.h < 360 && h.s >= 0 && h.s <= 100 && h.v >= 0 && h.v <= 100);
    }
}

static void t_wheel_math(void)
{
    int32_t h, s;
    pnl_hsv p, st, o;
    pnl_wheel_pick(10.0, 0.0, 100.0, &h, &s);
    CHECK(h == 0 && s == 10);
    pnl_wheel_pick(0.0, 50.0, 100.0, &h, &s);   /* below the center: clockwise 90 */
    CHECK(h == 90 && s == 50);
    pnl_wheel_pick(-30.0, 0.0, 100.0, &h, &s);
    CHECK(h == 180 && s == 30);
    pnl_wheel_pick(0.0, -100.0, 100.0, &h, &s);
    CHECK(h == 270 && s == 100);
    pnl_wheel_pick(500.0, 0.0, 100.0, &h, &s);  /* outside: full saturation */
    CHECK(s == 100);
    p.h = 37; p.s = 60; p.v = 100;
    st.h = 10; st.s = 25; st.v = 40;
    o = pnl_wheel_constrain(p, st, UI_MOD_CTRL);
    CHECK(o.h == 37 && o.s == 25);              /* same radius */
    o = pnl_wheel_constrain(p, st, UI_MOD_ALT);
    CHECK(o.h == 10 && o.s == 60);              /* same spoke */
    o = pnl_wheel_constrain(p, st, UI_MOD_SHIFT);
    CHECK(o.h == 30 && o.s == 60);              /* nearest 15 degree spoke */
    p.h = 38;
    o = pnl_wheel_constrain(p, st, UI_MOD_SHIFT);
    CHECK(o.h == 45);
    p.h = 32;
    o = pnl_wheel_constrain(p, st, UI_MOD_CTRL | UI_MOD_SHIFT);
    CHECK(o.h == 25 && o.s == 25);              /* 15 degree steps from the start hue */
    p.h = 350;
    o = pnl_wheel_constrain(p, st, UI_MOD_CTRL | UI_MOD_SHIFT);
    CHECK(o.h == 355 && o.s == 25);             /* the short way round */
    o = pnl_wheel_constrain(p, st, 0u);
    CHECK(o.h == 350 && o.s == 60);
}

static void t_palette_files(void)
{
    static const char text[] =
        "\xEF\xBB\xBF; header comment\r\n"
        "\r\n"
        "FF000000\r\n"
        "  ff112233  ; trailing comment\n"
        "FF0000\n"                      /* 6 digits: alpha 00 (3.36) */
        "GG000000\n"                    /* invalid: skipped */
        "123456789\n"                   /* 9 digits: skipped */
        "0x80FFFFFF\n"
        "\t;only a comment\n"
        "7";                            /* last line without a newline */
    uint32_t pal[PNL_PALETTE_N], back[PNL_PALETTE_N];
    size_t n = pnl_palette_parse(text, sizeof text - 1u, pal), len = 0;
    char *out;
    CHECK(n == 5u);
    CHECK(pal[0] == 0xFF000000u && pal[1] == 0xFF112233u && pal[2] == 0x00FF0000u);
    CHECK(pal[3] == 0x80FFFFFFu && pal[4] == 0x00000007u);
    CHECK(pal[5] == 0xFFFFFFFFu && pal[95] == 0xFFFFFFFFu);
    /* more than 96 entries: the rest is ignored */
    {
        char big[200 * 10];
        size_t k = 0;
        for (int i = 0; i < 200; i++) k += (size_t)snprintf(big + k, sizeof big - k, "%08X\n", i);
        CHECK(pnl_palette_parse(big, k, pal) == 96u && pal[95] == 95u);
    }
    CHECK(pnl_palette_parse(NULL, 0u, pal) == 0u && pal[0] == 0xFFFFFFFFu);
    /* formatting round trips and writes 96 uppercase lines after the header */
    out = pnl_palette_format(pnl_default_palette, &len);
    CHECK(out != NULL);
    if (out) {
        CHECK(pnl_palette_parse(out, len, back) == 96u);
        CHECK(memcmp(back, pnl_default_palette, sizeof back) == 0);
        CHECK(out[0] == ';' && strstr(out, "\nFF0026FF\n") != NULL && strstr(out, "ff") == NULL);
        free(out);
    }
    /* the default palette of WINDOWS.md 7.2 */
    CHECK(pnl_default_palette[0] == 0xFF000000u && pnl_default_palette[16] == 0xFFFFFFFFu &&
          pnl_default_palette[64] == 0x80000000u && pnl_default_palette[95] == 0x807F0037u);
    /* names */
    CHECK(pnl_palette_name_valid("Pastels") && pnl_palette_name_valid("my palette 2"));
    CHECK(pnl_palette_name_valid("console") && pnl_palette_name_valid("COM10"));
    CHECK(!pnl_palette_name_valid("") && !pnl_palette_name_valid(NULL));
    CHECK(!pnl_palette_name_valid("a/b") && !pnl_palette_name_valid("a\\b"));
    CHECK(!pnl_palette_name_valid("a:b") && !pnl_palette_name_valid("what?"));
    CHECK(!pnl_palette_name_valid(".") && !pnl_palette_name_valid(".."));
    CHECK(!pnl_palette_name_valid("x.") && !pnl_palette_name_valid(" x"));
    CHECK(!pnl_palette_name_valid("con") && !pnl_palette_name_valid("CON.txt"));
    CHECK(!pnl_palette_name_valid("lpt1") && !pnl_palette_name_valid("Nul"));
    CHECK(!pnl_palette_name_valid("tab\there"));
}

/* Swatches, slots, swap, reset (W-COL-SWATCH, SWAP, RESET, ACTIVEKEY,
 * PALETTE). */
static void t_slots(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    ui_rect pal;
    int32_t cell, gap;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pt_new_doc(a, 200, 150) != NULL);
    pal = pnl_rect(a, "colors.palette");
    CHECK(!ui_rect_empty(pal));
    cell = ui_px(a->ui, 13.0f);
    gap = ui_px_line(a->ui, 2.0f);
    /* left click: active slot (primary); right click: the other one */
    pt_click(a, (float)(pal.x + 2 * (cell + gap) + cell / 2), (float)(pal.y + cell / 2),
             SDL_BUTTON_LEFT, 1);
    CHECK(pt_px_is(app_primary(a), 255, 0, 0, 255));
    pt_click(a, (float)(pal.x + 9 * (cell + gap) + cell / 2), (float)(pal.y + cell / 2),
             SDL_BUTTON_RIGHT, 1);
    CHECK(pt_px_is(app_secondary(a), 0, 255, 255, 255) && pt_px_is(app_primary(a), 255, 0, 0, 255));
    /* C makes the secondary active: left clicks go there now */
    pt_key(a, SDLK_C, SDL_KMOD_NONE);
    CHECK(app_color_slot(a) == 1);
    pt_click(a, (float)(pal.x + cell / 2), (float)(pal.y + (cell + gap) + cell / 2),
             SDL_BUTTON_LEFT, 1);                          /* row 2: white */
    CHECK(pt_px_is(app_secondary(a), 255, 255, 255, 255));
    CHECK(pt_px_is(app_primary(a), 255, 0, 0, 255));
    pt_key(a, SDLK_C, SDL_KMOD_NONE);
    CHECK(app_color_slot(a) == 0);
    /* X swaps */
    pt_key(a, SDLK_X, SDL_KMOD_NONE);
    CHECK(pt_px_is(app_primary(a), 255, 255, 255, 255));
    CHECK(pt_px_is(app_secondary(a), 255, 0, 0, 255));
    /* the swap icon too */
    CHECK(pt_click_rect(a, "colors.swap", SDL_BUTTON_LEFT));
    CHECK(pt_px_is(app_primary(a), 255, 0, 0, 255));
    /* reset: black and white */
    CHECK(pt_click_rect(a, "colors.reset", SDL_BUTTON_LEFT));
    CHECK(pt_px_is(app_primary(a), 0, 0, 0, 255) && pt_px_is(app_secondary(a), 255, 255, 255, 255));
    /* clicking the secondary square makes it active */
    {
        ui_rect pr = pnl_rect(a, "colors.pair");
        pt_click(a, (float)(pr.x + pr.w - 6), (float)(pr.y + pr.h - 26), SDL_BUTTON_LEFT, 1);
        CHECK(app_color_slot(a) == 1);
        pt_click(a, (float)(pr.x + 8), (float)(pr.y + 20), SDL_BUTTON_LEFT, 1);
        CHECK(app_color_slot(a) == 0);
    }
    /* semi-transparent palette rows keep their alpha */
    pt_click(a, (float)(pal.x + 2 * (cell + gap) + cell / 2), (float)(pal.y + cell / 2),
             SDL_BUTTON_LEFT, 1);
    pnl_colors_set_expanded(a, true);
    at_frames(a, 3);
    pal = pnl_rect(a, "colors.palette");
    pt_click(a, (float)(pal.x + 2 * (cell + gap) + cell / 2),
             (float)(pal.y + 4 * (cell + gap) + cell / 2), SDL_BUTTON_LEFT, 1);
    CHECK(pt_px_is(app_primary(a), 255, 0, 0, 0x80));
    app_destroy(a);
}

/* More / Less, the hex box and the channel controls (W-COL-MORE, SLIDERS,
 * HEX, BACKSPACE). */
static void t_expanded(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    ui_panel_state *st;
    CHECK(a != NULL);
    if (!a) return;
    d = pt_new_doc(a, 200, 150);
    CHECK(d != NULL);
    st = app_panel_state(a, "colors");
    CHECK(st && st->w == 252.0f && !pnl_colors_expanded(a));
    CHECK(ui_rect_empty(pnl_rect(a, "colors.hex")));
    CHECK(pt_click_rect(a, "colors.more", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(pnl_colors_expanded(a) && st && st->w == 508.0f && st->h == 306.0f);
    CHECK(!ui_rect_empty(pnl_rect(a, "colors.hex")) && !ui_rect_empty(pnl_rect(a, "colors.bar.h")));
    /* hex: '#' accepted, alpha kept */
    app_set_primary(a, app_px_make(1, 2, 3, 200));
    CHECK(pt_click_rect(a, "colors.hex", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "#FF8000");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(pt_px_is(app_primary(a), 255, 128, 0, 200));
    /* Backspace edits the box and never runs Fill Selection (W-COL-BACKSPACE) */
    {
        size_t h0;
        CHECK(app_cmd_exec(a, "edit.select_all"));
        h0 = app_doc_history_list(d, NULL, 0, NULL);
        CHECK(pt_click_rect(a, "colors.hex", SDL_BUTTON_LEFT));
        pt_key(a, SDLK_END, SDL_KMOD_NONE);
        pt_key(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
        pt_key(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
        CHECK(app_doc_history_list(d, NULL, 0, NULL) == h0);
        /* "FF80" is not a color: leaving reverts the text, the color stays */
        pt_key(a, SDLK_TAB, SDL_KMOD_NONE);
        CHECK(pt_px_is(app_primary(a), 255, 128, 0, 200));
        /* control: without a focused box Backspace fills the selection */
        ui_set_focus(a->ui, 0);
        pt_key(a, SDLK_BACKSPACE, SDL_KMOD_NONE);
        CHECK(app_doc_history_list(d, NULL, 0, NULL) == h0 + 1u);
        CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.deselect"));
    }
    /* typed channel values */
    CHECK(pt_click_rect(a, "colors.num.g", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "77");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(pt_px_is(app_primary(a), 255, 77, 0, 200));
    CHECK(pt_click_rect(a, "colors.num.a", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "255");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(app_primary(a).a == 255);
    /* HSV boxes: V 0 then back keeps hue and saturation */
    app_set_primary(a, rgb(255, 0, 0));
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "colors.num.v", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "0");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(pt_px_is(app_primary(a), 0, 0, 0, 255));
    CHECK(pt_click_rect(a, "colors.num.v", SDL_BUTTON_LEFT));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "100");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(pt_px_is(app_primary(a), 255, 0, 0, 255));
    /* dragging a bar: the left end of the R bar is 0 */
    {
        ui_rect b = pnl_rect(a, "colors.bar.r");
        pt_click(a, (float)b.x, pt_cy(b), SDL_BUTTON_LEFT, 1);
        CHECK(app_primary(a).r == 0);
        b = pnl_rect(a, "colors.bar.h");
        pt_click(a, (float)(b.x + b.w - 1), pt_cy(b), SDL_BUTTON_LEFT, 1);
        CHECK(app_primary(a).r == 0);           /* gray stays gray, hue kept for later */
    }
    /* Less: back to the compact size; the state persists in the settings */
    CHECK(pt_click_rect(a, "colors.more", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(!pnl_colors_expanded(a) && st && st->w == 252.0f && st->h == 232.0f);
    app_destroy(a);
}

/* The wheel (W-COL-WHEEL, K-COL-WHEEL-*): left = active slot, right = the
 * other one, value 100 (3.36), clockwise hue, modifiers. */
static void t_wheel(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    ui_rect w;
    float cx, cy, R;
    pnl_hsv h;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pt_new_doc(a, 200, 150) != NULL);
    w = pnl_rect(a, "colors.wheel");
    CHECK(!ui_rect_empty(w));
    cx = (float)w.x + (float)w.w * 0.5f;
    cy = (float)w.y + (float)w.h * 0.5f;
    R = (float)w.w * 0.5f;
    app_set_primary(a, app_px_make(0, 0, 0, 77));
    pt_click(a, cx + R * 0.5f, cy, SDL_BUTTON_LEFT, 1);
    h = pnl_rgb_to_hsv(app_primary(a).r, app_primary(a).g, app_primary(a).b);
    CHECK(app_primary(a).r == 255 && app_primary(a).a == 77);
    CHECK((h.h <= 1 || h.h >= 359) && h.s >= 48 && h.s <= 52 && h.v == 100);
    /* right click below the center: hue 90 into the secondary */
    pt_click(a, cx, cy + R * 0.9f, SDL_BUTTON_RIGHT, 1);
    h = pnl_rgb_to_hsv(app_secondary(a).r, app_secondary(a).g, app_secondary(a).b);
    CHECK(h.h >= 88 && h.h <= 92 && h.s >= 86 && h.s <= 92 && h.v == 100);
    CHECK(app_primary(a).r == 255);
    /* Ctrl while dragging keeps the radius: a drag from s 50 to the edge */
    pt_click(a, cx + R * 0.5f, cy, SDL_BUTTON_LEFT, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, cx + R * 0.5f, cy, 0);
    at_frames(a, 1);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, cx + R * 0.5f, cy, SDL_BUTTON_LEFT, 1);
    at_frames(a, 1);
    pt_mod(a, SDLK_LCTRL, SDL_KMOD_LCTRL, true);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, cx, cy - R * 0.95f, 0);
    at_frames(a, 2);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, cx, cy - R * 0.95f, SDL_BUTTON_LEFT, 1);
    at_frames(a, 1);
    pt_mod(a, SDLK_LCTRL, SDL_KMOD_LCTRL, false);
    h = pnl_rgb_to_hsv(app_primary(a).r, app_primary(a).g, app_primary(a).b);
    INFO("ctrl drag result h %d s %d v %d", (int)h.h, (int)h.s, (int)h.v);
    CHECK(h.h >= 268 && h.h <= 271 && h.s >= 47 && h.s <= 52);
    app_destroy(a);
}

/* Add Color (W-COL-ADD): insert mode, then a palette click replaces it. */
static void t_add_color(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    ui_rect pal;
    uint32_t p[PNL_PALETTE_N];
    int32_t cell, gap;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pt_new_doc(a, 200, 150) != NULL);
    pal = pnl_rect(a, "colors.palette");
    cell = ui_px(a->ui, 13.0f);
    gap = ui_px_line(a->ui, 2.0f);
    app_set_primary(a, app_px_make(1, 2, 3, 255));
    CHECK(pt_click_rect(a, "colors.add", SDL_BUTTON_LEFT));
    CHECK(pnl_colors_add_mode(a));
    CHECK(pt_click_rect(a, "colors.add", SDL_BUTTON_LEFT));     /* cancel */
    CHECK(!pnl_colors_add_mode(a));
    CHECK(pt_click_rect(a, "colors.add", SDL_BUTTON_LEFT));
    pt_click(a, (float)(pal.x + 5 * (cell + gap) + cell / 2), (float)(pal.y + cell / 2),
             SDL_BUTTON_LEFT, 1);
    pnl_colors_get_palette(a, p);
    CHECK(p[5] == 0xFF010203u && !pnl_colors_add_mode(a));
    CHECK(pt_px_is(app_primary(a), 1, 2, 3, 255));           /* the slot did not change */
    CHECK(p[4] == pnl_default_palette[4] && p[6] == pnl_default_palette[6]);
    app_destroy(a);
}

/* The palette menu: Save Current Palette As, load, reset, folder, and the
 * remembered palette and mode (W-COL-PALMENU, W-COL-MORE). */
static void t_palette_menu(void)
{
    char cfg[1024], dir[1024], path[1400];
    app *a;
    uint32_t p[PNL_PALETTE_N];
    at_out_path(cfg, sizeof cfg, "test_p_colors_cfg");
    pal_path_join(dir, sizeof dir, cfg, "palettes");
    pt_clean_dir(dir);
    pt_clean_dir(cfg);
    CHECK(pal_mkdirs(cfg));
    a = pt_app(1280, 800, cfg, false);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pt_new_doc(a, 200, 150) != NULL);
    {
        char got[1024];
        CHECK(pnl_palettes_dir(a, got, sizeof got) && strcmp(got, dir) == 0);
    }
    /* Open Palettes Folder creates it (nothing to show headless) */
    CHECK(!pal_is_dir(dir));
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.folder", SDL_BUTTON_LEFT));
    CHECK(pal_is_dir(dir));
    /* change a swatch, then Save Current Palette As... "Test Pal" */
    pnl_colors_get_palette(a, p);
    p[0] = 0xFF123456u;
    pnl_colors_set_palette(a, p);
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.save", SDL_BUTTON_LEFT));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "Test Pal");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    pal_path_join(path, sizeof path, dir, "Test Pal.txt");
    CHECK(pal_file_exists(path));
    {
        uint8_t *data = NULL;
        size_t len = 0;
        uint32_t back[PNL_PALETTE_N];
        CHECK(pal_read_file(path, 1u << 20, &data, &len) == PC_OK);
        CHECK(pnl_palette_parse((const char *)data, len, back) == 96u && back[0] == 0xFF123456u);
        free(data);
    }
    /* an invalid name keeps the dialog open; Esc cancels it */
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.save", SDL_BUTTON_LEFT));
    at_frames(a, 3);
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "bad/name");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(app_dialog_active(a));
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    /* the same name asks before replacing; Esc keeps the file */
    p[0] = 0xFF654321u;
    pnl_colors_set_palette(a, p);
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.save", SDL_BUTTON_LEFT));
    at_frames(a, 3);
    pt_key(a, SDLK_A, SDL_KMOD_LCTRL);
    pt_text(a, "test pal");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));                      /* the replace question */
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    {
        uint8_t *data = NULL;
        size_t len = 0;
        uint32_t back[PNL_PALETTE_N];
        CHECK(pal_read_file(path, 1u << 20, &data, &len) == PC_OK);
        CHECK(pnl_palette_parse((const char *)data, len, back) == 96u && back[0] == 0xFF123456u);
        free(data);
    }
    /* Reset to Default Palette, then load "Test Pal" from the menu */
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.reset", SDL_BUTTON_LEFT));
    pnl_colors_get_palette(a, p);
    CHECK(memcmp(p, pnl_default_palette, sizeof p) == 0);
    CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
    CHECK(pt_click_rect(a, "palmenu.item0", SDL_BUTTON_LEFT));
    pnl_colors_get_palette(a, p);
    CHECK(p[0] == 0xFF123456u && p[1] == pnl_default_palette[1]);
    /* a short file pads with white */
    {
        char shortp[1400];
        static const char three[] = "FF0000FF\nFF00FF00\nFFFF0000\n";
        pal_path_join(shortp, sizeof shortp, dir, "Three.txt");
        CHECK(pal_write_file_atomic(shortp, three, sizeof three - 1u) == PC_OK);
        CHECK(pnl_palette_load(a, "Three") == PC_OK);
        pnl_colors_get_palette(a, p);
        CHECK(p[0] == 0xFF0000FFu && p[2] == 0xFFFF0000u && p[3] == 0xFFFFFFFFu &&
              p[95] == 0xFFFFFFFFu);
        CHECK(pnl_palette_load(a, "Missing") != PC_OK);
        CHECK(pnl_palette_load(a, "../evil") == PC_ERR_ARG);
        {
            char **names = NULL;
            int n = pnl_palette_list(a, &names);
            CHECK(n == 2 && strcmp(names[0], "Test Pal") == 0 && strcmp(names[1], "Three") == 0);
            pal_free_names(names, n);
        }
    }
    /* the palette and the expanded mode come back with the next start */
    pnl_colors_set_expanded(a, true);
    app_destroy(a);
    a = pt_app(1280, 800, cfg, false);
    CHECK(a != NULL);
    if (a) {
        ui_panel_state *st = app_panel_state(a, "colors");
        pnl_colors_get_palette(a, p);
        CHECK(p[0] == 0xFF0000FFu && p[3] == 0xFFFFFFFFu);
        CHECK(pnl_colors_expanded(a) && st && st->w == 508.0f);
        pnl_colors_set_expanded(a, false);
        app_destroy(a);
    }
    pt_clean_dir(dir);
    pt_clean_dir(cfg);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_hsv);
    RUN(t_wheel_math);
    RUN(t_palette_files);
    RUN(t_slots);
    RUN(t_expanded);
    RUN(t_wheel);
    RUN(t_add_color);
    RUN(t_palette_menu);
    at_quit();
    return pc_test_finish();
}
