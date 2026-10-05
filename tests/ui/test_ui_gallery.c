/* test_ui_gallery.c - full-window rendering smoke test: every gallery scene
 * in the light and dark themes (and at fractional scales) is rendered with
 * the software renderer and written to <build>/ui_gallery_*.bmp for visual
 * review. Checks: deterministic output, non-trivial images (many colors,
 * plausible brightness per theme), and that the menu, dialog and icon
 * scenes differ from the main scene. */
#include "pc_test.h"
#include "ui_test_util.h"

#include "ui_gallery.h"

#ifndef PC_UI_OUT_DIR
#define PC_UI_OUT_DIR "."
#endif

typedef struct shot_stats { double luma; int colors; int w, h; } shot_stats;

static bool load_stats(const char *path, shot_stats *st)
{
    SDL_Surface *s = SDL_LoadBMP(path), *c;
    uint8_t *seen;
    if (!s) return false;
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888);
    SDL_DestroySurface(s);
    if (!c) return false;
    seen = (uint8_t *)calloc(1u << 21, 1u);           /* 24-bit color bitmap */
    st->w = c->w;
    st->h = c->h;
    st->luma = 0.0;
    st->colors = 0;
    if (seen && SDL_LockSurface(c)) {
        for (int y = 0; y < c->h; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)c->pixels +
                                                     (size_t)y * (size_t)c->pitch);
            for (int x = 0; x < c->w; x++) {
                uint32_t v = row[x] & 0xFFFFFFu;
                st->luma += 0.2126 * ut_chan(v, 0) + 0.7152 * ut_chan(v, 1) +
                            0.0722 * ut_chan(v, 2);
                if (!(seen[v >> 3] & (1u << (v & 7u)))) {
                    seen[v >> 3] = (uint8_t)(seen[v >> 3] | (1u << (v & 7u)));
                    st->colors++;
                }
            }
        }
        SDL_UnlockSurface(c);
        st->luma /= (double)c->w * (double)c->h;
    }
    free(seen);
    SDL_DestroySurface(c);
    return true;
}

static const char *const k_scene[UI_GALLERY_SCENE_COUNT] = { "main", "menu", "dialog", "icons" };

static void t_scenes(void)
{
    uint64_t hash[2][UI_GALLERY_SCENE_COUNT];
    shot_stats st[2][UI_GALLERY_SCENE_COUNT];
    for (int dark = 0; dark < 2; dark++)
        for (int sc = 0; sc < UI_GALLERY_SCENE_COUNT; sc++) {
            char path[512];
            snprintf(path, sizeof path, "%s/ui_gallery_%s_%s.bmp", PC_UI_OUT_DIR,
                     dark ? "dark" : "light", k_scene[sc]);
            hash[dark][sc] = 0;
            CHECK(ui_gallery_render_file(path, 1440, 900, 1.0f, dark != 0, sc, &hash[dark][sc]));
            memset(&st[dark][sc], 0, sizeof st[dark][sc]);
            CHECK(load_stats(path, &st[dark][sc]));
            CHECK(st[dark][sc].w == 1440 && st[dark][sc].h == 900);
            CHECK(st[dark][sc].colors > 300);
            INFO("%s: %d colors, mean luma %.1f", path, st[dark][sc].colors, st[dark][sc].luma);
        }
    for (int sc = 0; sc < UI_GALLERY_SCENE_COUNT; sc++) {
        CHECK(st[0][sc].luma > 140.0);                /* light theme (dialog: dimmed) */
        CHECK(st[1][sc].luma < 90.0);                 /* dark theme */
        CHECK(hash[0][sc] != hash[1][sc]);
    }
    for (int dark = 0; dark < 2; dark++)
        for (int sc = 1; sc < UI_GALLERY_SCENE_COUNT; sc++)
            CHECK(hash[dark][sc] != hash[dark][0]);
    /* the dialog dims the window behind it */
    CHECK(st[0][UI_GALLERY_SCENE_DIALOG].luma < st[0][UI_GALLERY_SCENE_MAIN].luma - 10.0);
    {
        /* rendering is deterministic */
        uint64_t again = 0;
        CHECK(ui_gallery_render_file(NULL, 1440, 900, 1.0f, false, UI_GALLERY_SCENE_MAIN, &again));
        CHECK(again == hash[0][UI_GALLERY_SCENE_MAIN]);
    }
}

static void t_scales(void)
{
    static const float scales[] = { 1.5f, 1.25f, 2.0f, 1.75f };
    int n = g_quick ? 1 : 4;
    for (int i = 0; i < n; i++) {
        char path[512];
        uint64_t h = 0;
        shot_stats st;
        /* the gallery is laid out for 1440 x 900 DIPs */
        int w = (int)(1440.0f * scales[i]), hgt = (int)(900.0f * scales[i]);
        snprintf(path, sizeof path, "%s/ui_gallery_light_main_%03d.bmp", PC_UI_OUT_DIR,
                 (int)(scales[i] * 100.0f + 0.5f));
        CHECK(ui_gallery_render_file(path, w, hgt, scales[i], i & 1, UI_GALLERY_SCENE_MAIN, &h));
        memset(&st, 0, sizeof st);
        CHECK(load_stats(path, &st) && st.colors > 300);
        INFO("%s: %d colors", path, st.colors);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_scenes);
    RUN(t_scales);
    SDL_Quit();
    return pc_test_finish();
}
