/* test_uia_combo.c - lane UIA (wave 4): dropdown lists are as wide as their
 * longest item. A list long enough to scroll used to give its rows no room
 * for the scroll bar, so a narrow box cut "Color Dodge" to "Color Do..."
 * (w4 item 29). The open list is rendered and the rightmost text pixel of
 * the long item's row must reach the end of the whole label.
 * Single-threaded test code. */
#include "pc_test.h"
#include "ui_test_util.h"

#define NITEMS 24

static const char *const k_items[NITEMS] = {
    "Normal", "Multiply", "Color Dodge Wide Label", "Color Burn", "Reflect", "Glow",
    "Overlay", "Difference", "Negation", "Lighten", "Darken", "Screen", "Xor", "Overwrite",
    "Item 14", "Item 15", "Item 16", "Item 17", "Item 18", "Item 19", "Item 20", "Item 21",
    "Item 22", "Item 23"
};

typedef struct combo_scene {
    int     idx, n;
    ui_rect box;
} combo_scene;

static void scene(ui_ctx *ctx, void *ud)
{
    combo_scene *s = (combo_scene *)ud;
    /* a white page, so only text is dark */
    ui_draw_rect(ctx, ui_rect_make(0, 0, 600, 700), ui_rgba(255, 255, 255, 255));
    s->box = ui_rect_make(20, 20, 120, 28);
    ui_layout_set_next(ctx, s->box);
    (void)ui_combo(ctx, "##uia_combo", &s->idx, k_items, s->n);
}

/* Rightmost x of dark (text) pixels in rows y0..y1 right of x0. */
static int ink_right(ut_env *e, int x0, int y0, int y1)
{
    int best = -1;
    for (int y = y0; y < y1 && y < e->h; y++)
        for (int x = x0; x < e->w; x++) {
            uint32_t p = ut_pixel(e, x, y);
            if (ut_chan(p, 0) + ut_chan(p, 1) + ut_chan(p, 2) < 300 && x > best) best = x;
        }
    return best;
}

static void t_list_width(int n)
{
    ut_env e;
    combo_scene s;
    float tw;
    int right;
    memset(&s, 0, sizeof s);
    s.n = n;
    CHECK(ut_open(&e, 600, 700, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 2, scene, &s);
    ut_click(&e, s.box);
    ut_frames(&e, 4, scene, &s);
    CHECK(ui_menu_keyboard(e.ctx));                  /* the list is open */
    ut_move(&e, 590.0f, 690.0f);                     /* no hover highlight */
    ut_frames(&e, 2, scene, &s);
    ut_render(&e);
    tw = ui_text_width(ui_font_regular(e.ctx), ui_font_px(e.ctx), k_items[2],
                       strlen(k_items[2]));
    /* the long item is the third row; search below the box, right of the
     * box start plus the row's lead */
    right = ink_right(&e, s.box.x, s.box.y + s.box.h + 2, s.box.y + s.box.h + 2 + 3 * 30);
    INFO("%d items: text %.0f px, ink to x %d, box at %d", n, (double)tw, right, s.box.x);
    CHECK(right >= s.box.x + (int)tw + 20);
    ut_close(&e);
}

static void t_scrolled_list(void) { t_list_width(NITEMS); }
static void t_short_list(void) { t_list_width(15); }

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!ut_sdl_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_scrolled_list);
    RUN(t_short_list);
    ut_harness_refs();
    SDL_Quit();
    return pc_test_finish();
}
