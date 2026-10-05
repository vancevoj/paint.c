/* test_b_picker.c - lane B: Color Picker through the real input path:
 * left sets the primary and right the secondary color, alpha included
 * (TOOLS.md 10.1); dragging keeps picking; clicks off the image pick
 * nothing; the sample sizes average with alpha weighting (3.36 rule) and
 * clip to the image; Layer vs Image sampling and Ctrl for Image
 * (K-PICKER-IMAGE); the after-click options switch to the previous tool or
 * the Pencil; sample size and after-click menus persist their values. */
#include "pc_test.h"
#include "b_test_util.h"

static pc_px32 img(int32_t x, int32_t y)
{
    return app_px_make((uint8_t)(x * 4), (uint8_t)(y * 4), 77, (uint8_t)(255 - ((x + y) % 4) * 60));
}

static app *setup(void)
{
    app *a = b_image(60, 50, b_px(255, 255, 255, 255));
    if (!a) return NULL;
    CHECK(b_fill_layer(a, img));
    CHECK(app_tool_select(a, "color_picker"));
    a->ts.sampling = 0;
    at_frames(a, 2);
    return a;
}

/* 3.36 ColorBgra.Blend over the clipped square. */
static pc_px32 avg(int32_t cx, int32_t cy, int32_t n, int32_t w, int32_t h)
{
    uint64_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
    pc_px32 o;
    memset(&o, 0, sizeof o);
    for (int32_t y = cy - n / 2; y <= cy + n / 2; y++)
        for (int32_t x = cx - n / 2; x <= cx + n / 2; x++) {
            pc_px32 p;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            p = img(x, y);
            cnt++;
            sa += p.a;
            sr += (uint64_t)p.r * p.a;
            sg += (uint64_t)p.g * p.a;
            sb += (uint64_t)p.b * p.a;
        }
    if (!cnt) return o;
    o.a = (uint8_t)(sa / cnt);
    if (sa) {
        o.r = (uint8_t)(sr / sa);
        o.g = (uint8_t)(sg / sa);
        o.b = (uint8_t)(sb / sa);
    }
    return o;
}

static bool choose(app *a, const char *menu, const char *item)
{
    char name[64];
    snprintf(name, sizeof name, "%sv", menu);
    if (!b_widget(a, name, 0.5f, 0.5f)) return false;
    at_frames(a, 2);
    snprintf(name, sizeof name, "##menu/%s", item);
    return b_widget(a, name, 0.5f, 0.5f);
}

static void t_single_and_drag(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    b_click(a, 10.5, 20.5, SDL_BUTTON_LEFT);
    CHECK(b_eq(app_primary(a), img(10, 20)));        /* alpha included */
    b_click(a, 33.5, 7.5, SDL_BUTTON_RIGHT);
    CHECK(b_eq(app_secondary(a), img(33, 7)));
    CHECK(b_eq(app_primary(a), img(10, 20)));
    at_drag(a, 5.5, 5.5, 40.5, 30.5, 5, SDL_BUTTON_LEFT);
    CHECK(b_eq(app_primary(a), img(40, 30)));        /* the last pixel of the drag */
    b_click(a, -5.5, 20.5, SDL_BUTTON_LEFT);           /* off the image: nothing */
    CHECK(b_eq(app_primary(a), img(40, 30)));
    CHECK(b_history(a) == 2u);                        /* picking records nothing */
    app_destroy(a);
}

static void t_sizes(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    CHECK(choose(a, "##pick_size", "3 \xC3\x97 3 pixels"));
    CHECK(app_settings_int(app_settings_of(a), "tool.color_picker.size", 0) == 1);
    b_click(a, 20.5, 20.5, SDL_BUTTON_LEFT);
    CHECK(b_eq(app_primary(a), avg(20, 20, 3, 60, 50)));
    CHECK(choose(a, "##pick_size", "51 \xC3\x97 51 pixels"));
    b_click(a, 2.5, 3.5, SDL_BUTTON_RIGHT);           /* clipped at the corner */
    CHECK(b_eq(app_secondary(a), avg(2, 3, 51, 60, 50)));
    CHECK(choose(a, "##pick_size", "Single Pixel"));
    b_click(a, 2.5, 3.5, SDL_BUTTON_RIGHT);
    CHECK(b_eq(app_secondary(a), img(2, 3)));
    app_destroy(a);
}

/* Layer vs Image sampling, Ctrl for Image. */
static void t_layer_image(void)
{
    app *a = setup();
    app_doc *d;
    pc_px32 comp;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "layers.add_new"));        /* transparent and active */
    at_frames(a, 1);
    CHECK(d->doc->n_layers == 2u);
    b_click(a, 12.5, 12.5, SDL_BUTTON_LEFT);
    CHECK(b_eq(app_primary(a), b_px(0, 0, 0, 0)));   /* the empty layer */
    comp = at_doc_px(a, 12, 12);
    b_mods(a, SDL_KMOD_LCTRL);
    b_click(a, 12.5, 12.5, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
    CHECK(b_eq(app_primary(a), comp));
    CHECK(choose(a, "##sampling", "Image"));
    CHECK(a->ts.sampling == 1);
    b_click(a, 13.5, 12.5, SDL_BUTTON_RIGHT);
    CHECK(b_eq(app_secondary(a), at_doc_px(a, 13, 12)));
    /* hidden layers do not count */
    CHECK(app_cmd_exec(a, "layers.go_down"));
    CHECK(app_cmd_exec(a, "layers.toggle_visibility"));
    at_frames(a, 1);
    b_click(a, 13.5, 12.5, SDL_BUTTON_RIGHT);
    CHECK(b_eq(app_secondary(a), b_px(0, 0, 0, 0)));
    app_destroy(a);
}

static void t_after_click(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "gradient"));
    CHECK(app_tool_select(a, "color_picker"));
    at_frames(a, 2);
    CHECK(choose(a, "##pick_after", "Switch to previous tool"));
    CHECK(app_settings_int(app_settings_of(a), "tool.color_picker.after", 0) == 1);
    b_click(a, 10.5, 10.5, SDL_BUTTON_LEFT);
    CHECK(strcmp(app_tool_current(a)->id, "gradient") == 0);
    CHECK(b_eq(app_primary(a), img(10, 10)));
    CHECK(app_tool_select(a, "color_picker"));
    at_frames(a, 2);
    CHECK(choose(a, "##pick_after", "Switch to Pencil tool"));
    b_click(a, 11.5, 10.5, SDL_BUTTON_RIGHT);
    CHECK(strcmp(app_tool_current(a)->id, "pencil") == 0);
    CHECK(b_eq(app_secondary(a), img(11, 10)));
    CHECK(app_tool_select(a, "color_picker"));
    at_frames(a, 2);
    CHECK(choose(a, "##pick_after", "Do not switch tool"));
    b_click(a, 12.5, 10.5, SDL_BUTTON_LEFT);
    CHECK(strcmp(app_tool_current(a)->id, "color_picker") == 0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_single_and_drag);
    RUN(t_sizes);
    RUN(t_layer_image);
    RUN(t_after_click);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
