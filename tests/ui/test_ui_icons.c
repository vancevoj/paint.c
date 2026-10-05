/* test_ui_icons.c - the original icon set: names, every icon at every
 * common size, pixel snapping (crisp horizontal and vertical strokes),
 * consistent stroke weight across sizes, RGBA compositing, argument
 * checks, thread safety of the pure rasterizer and drawing through the
 * atlas cache. */
#include "pc_test.h"
#include "ui_test_util.h"

static const int k_sizes[] = { 16, 20, 24, 32 };

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void t_names(void)
{
    int tools = 0;
    for (int i = 1; i < UI_ICON_COUNT; i++) {
        const char *n = ui_icon_name((ui_icon)i);
        CHECK(n && *n);
        CHECK(ui_icon_from_name(n) == (ui_icon)i);
        for (int j = 1; j < i; j++) CHECK(strcmp(n, ui_icon_name((ui_icon)j)) != 0);
        if (strncmp(n, "tool.", 5) == 0) tools++;
    }
    CHECK(tools == 19);                       /* every Paint.NET 5.1 tool */
    CHECK(*ui_icon_name(UI_ICON_NONE) == '\0' && *ui_icon_name(UI_ICON_COUNT) == '\0');
    CHECK(ui_icon_from_name("no.such.icon") == UI_ICON_NONE);
    CHECK(ui_icon_from_name(NULL) == UI_ICON_NONE);
    CHECK(ui_icon_from_name("tool.paintbrush") == UI_ICON_TOOL_PAINTBRUSH);
}

/* Coverage statistics of one layer. */
typedef struct cov_stats { uint64_t sum; int inked, full, partial; } cov_stats;

static cov_stats stats(const uint8_t *c, int size)
{
    cov_stats s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < size * size; i++) {
        s.sum += c[i];
        if (c[i]) s.inked++;
        if (c[i] == 255) s.full++;
        else if (c[i]) s.partial++;
    }
    return s;
}

static void t_all_icons(void)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)UI_ICON_LAYERS * 256u * 256u + 64u);
    if (!buf) { CHECK(0); return; }
    for (int i = 1; i < UI_ICON_COUNT; i++) {
        double density[4];
        for (size_t k = 0; k < 4; k++) {
            int s = k_sizes[k];
            size_t n = (size_t)s * (size_t)s;
            cov_stats all;
            memset(buf + n * UI_ICON_LAYERS, 0xA5, 64);          /* guard */
            CHECK(ui_icon_raster((ui_icon)i, s, buf) == PC_OK);
            for (int g = 0; g < 64; g++) CHECK(buf[n * UI_ICON_LAYERS + (size_t)g] == 0xA5);
            memset(&all, 0, sizeof all);
            for (int l = 0; l < UI_ICON_LAYERS; l++) {
                cov_stats st = stats(buf + (size_t)l * n, s);
                all.sum += st.sum;
                all.inked += st.inked;
                all.full += st.full;
                all.partial += st.partial;
            }
            /* every icon draws a visible shape that does not fill the square;
             * diagonal-only glyphs may have no fully covered pixel at 16 px */
            CHECK(all.inked >= s / 2 && all.sum >= (uint64_t)s * 255u / 2u);
            CHECK(all.inked < (int)n * UI_ICON_LAYERS);
            CHECK(all.full > 0 || s < 24);
            density[k] = (double)all.sum / 255.0 / (double)n;
        }
        /* The ink fraction is similar at every size. Stroke widths snap to
         * whole pixels, so a 1.25 unit stroke is 1 px at 16 and 2 px at 20:
         * allow that step but nothing wilder. */
        for (size_t k = 1; k < 4; k++)
            CHECK(density[k] > density[0] * 0.5 && density[k] < density[0] * 1.8);
    }
    /* the largest and smallest sizes work too */
    CHECK(ui_icon_raster(UI_ICON_TOOL_PAINTBRUSH, UI_ICON_MAX_PX, buf) == PC_OK);
    CHECK(ui_icon_raster(UI_ICON_TOOL_PAINTBRUSH, UI_ICON_MIN_PX, buf) == PC_OK);
    free(buf);
}

/* Horizontal and vertical strokes land on whole pixels: the minus sign is a
 * band of fully covered rows with empty rows around it. */
static void t_crisp(void)
{
    static const int sizes[] = { 16, 20, 24, 32, 40, 48 };
    uint8_t buf[UI_ICON_LAYERS * 48 * 48];
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int s = sizes[k], full_rows = 0, partial_rows = 0;
        const uint8_t *line = buf + (size_t)UI_ICON_LAYER_LINE * (size_t)s * (size_t)s;
        CHECK(ui_icon_raster(UI_ICON_MINUS, s, buf) == PC_OK);
        for (int y = 0; y < s; y++) {
            int mid = line[(size_t)y * (size_t)s + (size_t)s / 2];
            if (mid == 255) full_rows++;
            else if (mid) partial_rows++;
        }
        CHECK(full_rows >= 1 && partial_rows == 0);
        /* the plus has a crisp vertical bar as well */
        CHECK(ui_icon_raster(UI_ICON_PLUS, s, buf) == PC_OK);
        {
            int full_cols = 0, partial_cols = 0;
            for (int x = 0; x < s; x++) {
                int v = line[(size_t)(s / 4) * (size_t)s + (size_t)x];
                if (v == 255) full_cols++;
                else if (v) partial_cols++;
            }
            CHECK(full_cols >= 1 && partial_cols == 0);
        }
        /* grid lines: rows are either empty, partial-free or full */
        CHECK(ui_icon_raster(UI_ICON_GRID, s, buf) == PC_OK);
        {
            int partial = 0;
            for (int y = 0; y < s; y++) {
                int v = line[(size_t)y * (size_t)s + (size_t)s / 2 + 1];
                if (v && v != 255) partial++;
            }
            CHECK(partial <= 2);
        }
    }
}

static void t_rgba(void)
{
    uint8_t cov[UI_ICON_LAYERS * 24 * 24], rgba[24 * 24 * 4];
    ui_color line = ui_rgba(200, 10, 20, 255), acc = ui_rgba(0, 0, 255, 255);
    ui_color soft = ui_rgba(0, 0, 255, 77);
    int checked = 0;
    CHECK(ui_icon_raster(UI_ICON_SAVE, 24, cov) == PC_OK);
    CHECK(ui_icon_raster_rgba(UI_ICON_SAVE, 24, line, acc, soft, rgba) == PC_OK);
    for (int i = 0; i < 24 * 24; i++) {
        int cl = cov[UI_ICON_LAYER_LINE * 576 + i];
        if (cl == 255) {
            /* full line coverage: exactly the line color, opaque */
            CHECK(rgba[4 * i] == 200 && rgba[4 * i + 1] == 10 && rgba[4 * i + 3] == 255);
            checked++;
        }
        if (!cl && !cov[UI_ICON_LAYER_ACCENT * 576 + i] && !cov[i]) CHECK(rgba[4 * i + 3] == 0);
    }
    CHECK(checked > 20);
    CHECK(ui_icon_raster_rgba(UI_ICON_NONE, 24, line, acc, soft, rgba) == PC_ERR_ARG);
    CHECK(ui_icon_raster(UI_ICON_SAVE, 7, cov) == PC_ERR_ARG);
    CHECK(ui_icon_raster(UI_ICON_SAVE, UI_ICON_MAX_PX + 1, cov) == PC_ERR_ARG);
    CHECK(ui_icon_raster(UI_ICON_COUNT, 16, cov) == PC_ERR_ARG);
    CHECK(ui_icon_raster(UI_ICON_SAVE, 16, NULL) == PC_ERR_ARG);
}

/* ---- the rasterizer is pure: four threads produce the same bytes ---------------- */
typedef struct job { uint64_t hash; } job;

static uint64_t hash_all(int size)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)UI_ICON_LAYERS * (size_t)size * (size_t)size);
    uint64_t h = 1469598103934665603ull;
    if (!buf) return 0;
    for (int i = 1; i < UI_ICON_COUNT; i++)
        if (ui_icon_raster((ui_icon)i, size, buf) == PC_OK)
            h = fnv(buf, (size_t)UI_ICON_LAYERS * (size_t)size * (size_t)size, h);
    free(buf);
    return h;
}

static int SDLCALL worker(void *ud)
{
    job *j = (job *)ud;
    j->hash = hash_all(20);
    return 0;
}

static void t_threads(void)
{
    SDL_Thread *th[4];
    job jobs[4];
    uint64_t ref = hash_all(20);
    CHECK(ref != 0 && ref == hash_all(20));
    for (int i = 0; i < 4; i++) {
        jobs[i].hash = 0;
        th[i] = SDL_CreateThread(worker, "icons", &jobs[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (th[i]) SDL_WaitThread(th[i], NULL);
        else worker(&jobs[i]);
        CHECK(jobs[i].hash == ref);
    }
}

/* ---- drawing through the atlas --------------------------------------------------- */
static void s_icons(ui_ctx *ctx, void *ud)
{
    int s = *(const int *)ud, x = 0, y = 0;
    for (int i = 1; i < UI_ICON_COUNT; i++) {
        ui_draw_icon(ctx, (ui_icon)i, ui_rect_make(x, y, s, s), s, ui_rgba(255, 255, 255, 255),
                     ui_rgba(255, 255, 255, 255));
        x += s;
        if (x + s > 640) { x = 0; y += s; }
    }
}

static void t_draw(void)
{
    ut_env e;
    uint64_t h1, h2;
    int s = 24;
    if (!ut_open(&e, 640, 200, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_icons, &s);
    ut_render(&e);
    h1 = ut_hash(&e);
    ut_frame(&e, s_icons, &s);          /* cached sprites draw identically */
    ut_render(&e);
    h2 = ut_hash(&e);
    CHECK(h1 == h2);
    /* the drawn coverage matches the rasterized layers (white tint) */
    {
        uint8_t cov[UI_ICON_LAYERS * 24 * 24];
        int diff = 0;
        CHECK(ui_icon_raster((ui_icon)1, 24, cov) == PC_OK);
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 24; x++) {
                int i = y * 24 + x, a = 0;
                for (int l = 0; l < UI_ICON_LAYERS; l++) {
                    int c = cov[l * 576 + i] * (l == UI_ICON_LAYER_SOFT ? 77 : 255) / 255;
                    a = a + c - a * c / 255;
                }
                if (abs(ut_chan(ut_pixel(&e, x, y), 0) - a) > 3) diff++;
            }
        CHECK(diff <= 4);
    }
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_names);
    RUN(t_all_icons);
    RUN(t_crisp);
    RUN(t_rgba);
    RUN(t_threads);
    RUN(t_draw);
    SDL_Quit();
    return pc_test_finish();
}
