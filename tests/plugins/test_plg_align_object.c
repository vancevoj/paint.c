/* test_plg_align_object.c - the built Align Object plugin (plugins/
 * align_object) through pc_plugin_test and plg_test_util.h: loaded by the
 * real loader from PC_PLUGIN_PATH, every one of the 16 positions (every
 * third with --quick) passes the generic checks (determinism, tiling and
 * threads, ROI-only writes, cancellation) on the photo, selection,
 * transparent and object images,
 * and on the object image the object's bounding box lands where the
 * position says, with the pixels it leaves transparent. */
#include "plg_test_util.h"
#include "fx/fx_widgets.h"

/* Bounding box of alpha > 0 (w = 0 when empty). */
static fx_rect alpha_box(const fx_img *im)
{
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++)
            if (fxt_at(im, x, y)[3] > 0) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    return x1 < 0 ? fxt_rect(0, 0, 0, 0) : fxt_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* Expected start of a box of size n in [0, total) for an axis part. */
static int32_t place(int32_t part, int32_t now, int32_t n, int32_t total)
{
    if (part == FX_POS_AXIS_START) return 0;
    if (part == FX_POS_AXIS_MID) return (total - n) / 2;
    if (part == FX_POS_AXIS_END) return total - n;
    return now;
}

static void t_paths(void)
{
    CHECK(strstr(PC_PLUGIN_PATH, PC_PLUGIN_SLUG) != NULL);
    CHECK(strstr(PC_PLUGIN_SOURCE_DIR, "plugins") != NULL);
}

static void t_positions(void)
{
    plg_set s;
    if (plg_open(&s, PC_PLUGIN_PATH)) {
        const fx_effect *fx = plg_find(&s, "org.paintc.object.align");
        fx_img src = plg_image(PLG_OBJECT, 4), dst = fxt_img_new(src.r, 4);
        fx_rect before = alpha_box(&src);
        fx_env env = plg_env(src.r);
        CHECK(before.w > 0 && src.px && dst.px);
        for (int32_t pos = 0; fx && src.px && dst.px && pos < FX_POS_COUNT; pos++) {
            void *p = plg_params(fx);
            char label[32];
            int32_t h = FX_POS_AXIS_KEEP, v = FX_POS_AXIS_KEEP;
            fx_rect after;
            if (!p) continue;
            snprintf(label, sizeof label, "position %d", (int)pos);
            CHECK(fx_param_set(fx, p, "position", pos) == PC_OK);
            /* --quick (CTest, sanitizers): the generic checks on a third */
            if (!g_quick || pos % 3 == 0) (void)plg_check_effect(fx, p, label);
            CHECK(fx_run_sync(fx, p, &src, &dst, &env, src.r, NULL) == PC_OK);
            after = alpha_box(&dst);
            fx_pos_axes(pos, &h, &v);
            CHECK(after.w == before.w && after.h == before.h);
            CHECK(after.x == place(h, before.x, before.w, PLG_W));
            CHECK(after.y == place(v, before.y, before.h, PLG_H));
            /* the place the object left (outside its new box) is transparent */
            if (pos != FX_POS_NONE) {
                const uint8_t *q = fxt_at(&dst, before.x + before.w / 2, before.y + before.h / 2);
                bool inside = fxt_in(after, before.x + before.w / 2, before.y + before.h / 2);
                CHECK(inside || q[3] == 0);
            }
            fx_params_free(p);
        }
        fxt_img_free(&src);
        fxt_img_free(&dst);
    }
    plg_close(&s);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_paths);
    RUN(t_positions);
    return pc_test_finish();
}
