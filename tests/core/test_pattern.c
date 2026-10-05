/* test_pattern.c - fill styles: names, bitmaps, alignment, paint sources and
 * previews (lane E2). */
#include "pc_test.h"
#include "pc/pc_pattern.h"
#include "pc/pc_sel.h"

static pc_px32 px(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    pc_px32 p;
    p.b = b; p.g = g; p.r = r; p.a = a;
    return p;
}

static bool px_eq(pc_px32 a, pc_px32 b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

static void t_names(void)
{
    CHECK(strcmp(pc_fill_style_name(PC_FILL_SOLID), "Solid Color") == 0);
    CHECK(strcmp(pc_fill_style_name(PC_FILL_HORIZONTAL), "Horizontal") == 0);
    CHECK(strcmp(pc_fill_style_name(PC_FILL_PERCENT05), "Percent 05") == 0);
    CHECK(strcmp(pc_fill_style_name(PC_FILL_ZIG_ZAG), "Zig Zag") == 0);
    CHECK(strcmp(pc_fill_style_name(PC_FILL_SOLID_DIAMOND), "Solid Diamond") == 0);
    CHECK(pc_fill_style_name(PC_FILL_STYLE_COUNT) == NULL);
    CHECK(pc_fill_style_name((pc_fill_style)-1) == NULL);
    CHECK((unsigned)PC_FILL_STYLE_COUNT == PC_PATTERN_COUNT + 1u);
    for (int i = 0; i < (int)PC_FILL_STYLE_COUNT; i++) {
        const char *a = pc_fill_style_name((pc_fill_style)i);
        CHECK(a != NULL && a[0] != '\0');
        for (int j = 0; j < i; j++) CHECK(strcmp(a, pc_fill_style_name((pc_fill_style)j)) != 0);
    }
    CHECK(pc_pattern_bits(PC_FILL_SOLID) == NULL);
    CHECK(pc_pattern_bits(PC_FILL_STYLE_COUNT) == NULL);
}

/* tile as 64 bools shifted by (dx, dy) */
static void tile_of(pc_fill_style s, int dx, int dy, uint8_t out[64])
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) out[y * 8 + x] = pc_pattern_at(s, x + dx, y + dy) ? 1u : 0u;
}

static bool same_up_to_shift(const uint8_t a[64], pc_fill_style s, bool mirror_x, bool transpose)
{
    for (int dy = 0; dy < 8; dy++)
        for (int dx = 0; dx < 8; dx++) {
            bool same = true;
            for (int y = 0; y < 8 && same; y++)
                for (int x = 0; x < 8 && same; x++) {
                    int sx = mirror_x ? 7 - x : x, sy = y;
                    if (transpose) { int t = sx; sx = sy; sy = t; }
                    same = (a[y * 8 + x] != 0) == pc_pattern_at(s, sx + dx, sy + dy);
                }
            if (same) return true;
        }
    return false;
}

static void t_bitmaps(void)
{
    static const struct { pc_fill_style s; uint32_t pct; } pcts[] = {
        { PC_FILL_PERCENT05, 5 }, { PC_FILL_PERCENT10, 10 }, { PC_FILL_PERCENT20, 20 },
        { PC_FILL_PERCENT25, 25 }, { PC_FILL_PERCENT30, 30 }, { PC_FILL_PERCENT40, 40 },
        { PC_FILL_PERCENT50, 50 }, { PC_FILL_PERCENT60, 60 }, { PC_FILL_PERCENT70, 70 },
        { PC_FILL_PERCENT75, 75 }, { PC_FILL_PERCENT80, 80 }, { PC_FILL_PERCENT90, 90 }
    };
    static const pc_fill_style mirrors[][2] = {
        { PC_FILL_FORWARD_DIAGONAL, PC_FILL_BACKWARD_DIAGONAL },
        { PC_FILL_LIGHT_DOWNWARD_DIAGONAL, PC_FILL_LIGHT_UPWARD_DIAGONAL },
        { PC_FILL_DARK_DOWNWARD_DIAGONAL, PC_FILL_DARK_UPWARD_DIAGONAL },
        { PC_FILL_WIDE_DOWNWARD_DIAGONAL, PC_FILL_WIDE_UPWARD_DIAGONAL },
        { PC_FILL_DASHED_DOWNWARD_DIAGONAL, PC_FILL_DASHED_UPWARD_DIAGONAL }
    };
    static const pc_fill_style transposes[][2] = {
        { PC_FILL_HORIZONTAL, PC_FILL_VERTICAL },
        { PC_FILL_LIGHT_HORIZONTAL, PC_FILL_LIGHT_VERTICAL },
        { PC_FILL_NARROW_HORIZONTAL, PC_FILL_NARROW_VERTICAL },
        { PC_FILL_DARK_HORIZONTAL, PC_FILL_DARK_VERTICAL },
        { PC_FILL_DASHED_HORIZONTAL, PC_FILL_DASHED_VERTICAL }
    };
    uint8_t a[64];
    CHECK(pc_pattern_coverage(PC_FILL_SOLID) == 64u);
    /* every pattern is a real two-color pattern, distinct from all others
     * even after any shift */
    for (int i = 1; i < (int)PC_FILL_STYLE_COUNT; i++) {
        uint32_t c = pc_pattern_coverage((pc_fill_style)i);
        CHECK(c > 0u && c < 64u);
        tile_of((pc_fill_style)i, 0, 0, a);
        for (int j = 1; j < i; j++) CHECK(!same_up_to_shift(a, (pc_fill_style)j, false, false));
    }
    /* Percent screens: exact counts and nesting */
    for (size_t i = 0; i < sizeof pcts / sizeof pcts[0]; i++) {
        uint32_t want = (pcts[i].pct * 64u + 50u) / 100u;
        CHECK(pc_pattern_coverage(pcts[i].s) == want);
        if (i > 0) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (pc_pattern_at(pcts[i - 1].s, x, y)) CHECK(pc_pattern_at(pcts[i].s, x, y));
        }
    }
    CHECK(pc_pattern_coverage(PC_FILL_PERCENT50) == 32u);
    for (int y = 0; y < 8; y++)          /* Percent 50 is the one-pixel checker */
        for (int x = 0; x < 8; x++)
            CHECK(pc_pattern_at(PC_FILL_PERCENT50, x, y) == (((x + y) & 1) == 0));
    for (size_t i = 0; i < sizeof mirrors / sizeof mirrors[0]; i++) {
        tile_of(mirrors[i][0], 0, 0, a);
        CHECK(same_up_to_shift(a, mirrors[i][1], true, false));
    }
    for (size_t i = 0; i < sizeof transposes / sizeof transposes[0]; i++) {
        tile_of(transposes[i][0], 0, 0, a);
        CHECK(same_up_to_shift(a, transposes[i][1], false, true));
    }
    /* a few exact drawings */
    for (int x = 0; x < 8; x++) {
        CHECK(pc_pattern_at(PC_FILL_HORIZONTAL, x, 0));
        CHECK(!pc_pattern_at(PC_FILL_HORIZONTAL, x, 3));
        CHECK(pc_pattern_at(PC_FILL_FORWARD_DIAGONAL, x, x));
        CHECK(pc_pattern_at(PC_FILL_BACKWARD_DIAGONAL, x, 7 - x));
        for (int y = 0; y < 8; y++) {
            CHECK(pc_pattern_at(PC_FILL_SMALL_CHECKER_BOARD, x,
                                y) == ((((x >> 1) + (y >> 1)) & 1) == 0));
            CHECK(pc_pattern_at(PC_FILL_LARGE_CHECKER_BOARD, x,
                                y) == ((((x >> 2) + (y >> 2)) & 1) == 0));
        }
    }
}

static void t_alignment(void)
{
    int n = g_quick ? 20000 : 200000;
    pc_px32 fg = px(rnd8(), rnd8(), rnd8(), 200), bg = px(200, 100, 50, 77);
    pc_px32 row[200];
    for (int i = 0; i < n; i++) {
        pc_fill_style s = (pc_fill_style)rndu(PC_FILL_STYLE_COUNT);
        int32_t x = (int32_t)rndu(200000) - 100000, y = (int32_t)rndu(200000) - 100000;
        int32_t kx = (int32_t)rndu(1000) - 500, ky = (int32_t)rndu(1000) - 500;
        bool v = pc_pattern_at(s, x, y);
        CHECK(pc_pattern_at(s, x + 8 * kx, y + 8 * ky) == v);       /* 8 x 8 periodic */
        CHECK(pc_pattern_at(s, x, y) == pc_pattern_at(s, ((x % 8) + 8) % 8, ((y % 8) + 8) % 8));
        if (i % 50 == 0) {
            int32_t len = 1 + (int32_t)rndu(200);
            pc_pattern_row(s, fg, bg, x, y, len, row);
            for (int32_t k = 0; k < len; k++)
                CHECK(px_eq(row[k], pc_pattern_at(s, x + k, y) ? fg : bg));
        }
    }
    /* extremes do not overflow */
    CHECK(pc_pattern_at(PC_FILL_FORWARD_DIAGONAL, INT32_MIN, INT32_MIN));
    CHECK(pc_pattern_at(PC_FILL_FORWARD_DIAGONAL, INT32_MAX, INT32_MAX));
    pc_pattern_row(PC_FILL_STYLE_COUNT, fg, bg, 3, 4, 5, row);   /* invalid: fg */
    for (int k = 0; k < 5; k++) CHECK(px_eq(row[k], fg));
}

/* Painting through pc_paint_apply: two separate fills of neighboring rects
 * line up to one pattern anchored at the document origin, translucent
 * colors are composited with straight alpha, and undo restores. */
static void t_paint(void)
{
    pc_doc *d = pc_doc_create(150, 90);
    pc_hist *h = pc_hist_create(d);
    pc_layer *l = pc_layer_create(d, "L");
    pc_px32 base = px(40, 80, 120, 255);
    pc_px32 fg = px(0, 0, 255, 255), bg = px(255, 255, 255, 128);
    pc_surf s;
    pc_mask m1, m2;
    pc_fill_src fs;
    pc_paint_src src;
    pc_paint_opts op = pc_paint_opts_default();
    pc_txn *t;
    uint64_t fp0;
    pc_rect dirty;
    CHECK(pc_surf_alloc(&s, 150, 90) == PC_OK);
    for (int32_t i = 0; i < 150 * 90; i++) s.px[i] = base;
    CHECK(pc_layer_store_rect(d, l, pc_doc_rect(d), s.px, (size_t)s.stride) == PC_OK);
    CHECK(pc_hist_add_layer(h, l, 0, "add") == PC_OK);
    fp0 = pc_doc_fingerprint(d);
    CHECK(pc_mask_alloc(&m1, pc_rect_make(-5, -3, 80, 100)) == PC_OK);
    CHECK(pc_mask_alloc(&m2, pc_rect_make(75, 7, 90, 50)) == PC_OK);
    memset(m1.px, 255, (size_t)m1.stride * (size_t)m1.h);
    memset(m2.px, 255, (size_t)m2.stride * (size_t)m2.h);
    pc_fill_src_init(&fs, PC_FILL_DIAGONAL_BRICK, fg, bg);
    src = pc_fill_src_paint(&fs);
    CHECK(src.row != NULL);
    t = pc_txn_begin(d, "Fill");
    CHECK(pc_paint_apply(t, l->id, &m1, &src, &op, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 0 && dirty.y == 0 && dirty.w == 75 && dirty.h == 90);
    CHECK(pc_paint_apply(t, l->id, &m2, &src, &op, NULL, NULL) == PC_OK);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    {
        pc_px32 half = base;
        pc_composite_span(&half, &bg, 1, PC_BLEND_NORMAL, 255);
        for (int32_t y = 0; y < 90; y++)
            for (int32_t x = 0; x < 150; x++) {
                pc_px32 got = pc_layer_get_px(l, (uint32_t)x, (uint32_t)y);
                bool in = x < 75 || (y >= 7 && y < 57);
                pc_px32 want = !in ? base
                             : pc_pattern_at(PC_FILL_DIAGONAL_BRICK, x, y) ? fg : half;
                CHECK(px_eq(got, want));
            }
    }
    CHECK(pc_hist_undo(h));
    CHECK(pc_doc_fingerprint(d) == fp0);
    /* solid uses the fast path */
    pc_fill_src_init(&fs, PC_FILL_SOLID, fg, bg);
    src = pc_fill_src_paint(&fs);
    CHECK(src.row == NULL && px_eq(src.solid, fg));
    src = pc_fill_src_paint(NULL);
    CHECK(src.row == NULL && src.solid.a == 255u);
    pc_mask_free(&m1);
    pc_mask_free(&m2);
    pc_surf_free(&s);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

static void t_thumb(void)
{
    pc_px32 fg = px(1, 2, 3, 255), bg = px(9, 8, 7, 6);
    pc_px32 buf[40 * 30];
    for (int i = 0; i < (int)PC_FILL_STYLE_COUNT; i++) {
        pc_fill_style s = (pc_fill_style)i;
        int32_t scale = 1 + (int32_t)(i % 4);
        memset(buf, 0xAB, sizeof buf);
        pc_pattern_thumb(s, fg, bg, scale, 37, 30, buf, 40);
        for (int32_t y = 0; y < 30; y++) {
            for (int32_t x = 0; x < 37; x++) {
                bool f = pc_pattern_at(s, x / scale, y / scale);
                CHECK(px_eq(buf[y * 40 + x], f ? fg : bg));
            }
            for (int32_t x = 37; x < 40; x++) CHECK(buf[y * 40 + x].a == 0xABu);   /* untouched */
        }
    }
    pc_pattern_thumb(PC_FILL_WEAVE, fg, bg, 0, 8, 8, buf, 8);    /* scale < 1 counts as 1 */
    for (int32_t y = 0; y < 8; y++)
        for (int32_t x = 0; x < 8; x++)
            CHECK(px_eq(buf[y * 8 + x], pc_pattern_at(PC_FILL_WEAVE, x, y) ? fg : bg));
    pc_pattern_thumb(PC_FILL_WEAVE, fg, bg, 1, 8, 8, NULL, 8);   /* no-ops */
    pc_pattern_thumb(PC_FILL_WEAVE, fg, bg, 1, 0, 8, buf, 8);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    RUN(t_names);
    RUN(t_bitmaps);
    RUN(t_alignment);
    RUN(t_paint);
    RUN(t_thumb);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
