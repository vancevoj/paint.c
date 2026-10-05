/* pc_fill.c - tolerance metric and flood fills. No recursion: a naive
 * recursive fill overflows the C stack on large connected regions. */
#include "pc/pc_fill.h"

#include <stdlib.h>
#include <string.h>

uint32_t pc_tol_limit_from_percent(uint32_t pct)
{
    uint32_t t;
    if (pct > 100u) pct = 100u;
    t = (pct * 255u + 50u) / 100u;
    return t * t;
}

bool pc_color_within(pc_px32 a, pc_px32 b, uint32_t lim)
{
    int32_t dr = (int32_t)a.r - (int32_t)b.r;
    int32_t dg = (int32_t)a.g - (int32_t)b.g;
    int32_t db = (int32_t)a.b - (int32_t)b.b;
    int32_t da = (int32_t)a.a - (int32_t)b.a;
    uint32_t w = ((uint32_t)a.a + (uint32_t)b.a + 1u) >> 1;     /* 0..255 */
    uint32_t rgb2 = (uint32_t)(dr * dr + dg * dg + db * db);  /* <= 195075 */
    uint32_t d = rgb2 * w + (uint32_t)(da * da) * 255u;       /* <= 66325500 */
    if (lim > 65025u) lim = 65025u;
    return d <= lim * 1020u;                                   /* 4 * 255 */
}

void pc_fill_stack_free(pc_fill_stack *st)
{
    if (!st) return;
    free(st->xy);
    st->xy = NULL;
    st->n = st->cap = 0u;
}

static bool st_push(pc_fill_stack *st, int32_t x, int32_t y)
{
    if (st->n == st->cap) {
        size_t cap = st->cap ? st->cap * 2u : 1024u, bytes;
        int32_t *p;
        if (cap < st->cap) return false;
        if (!pc_mul_size(cap, 2u * sizeof *p, &bytes)) return false;
        p = (int32_t *)realloc(st->xy, bytes);
        if (!p) return false;
        st->xy = p;
        st->cap = cap;
    }
    st->xy[2u * st->n] = x;
    st->xy[2u * st->n + 1u] = y;
    st->n++;
    if (st->n > st->high_water) st->high_water = st->n;
    return true;
}

static bool st_pop(pc_fill_stack *st, int32_t *x, int32_t *y)
{
    if (st->n == 0u) return false;
    st->n--;
    *x = st->xy[2u * st->n];
    *y = st->xy[2u * st->n + 1u];
    return true;
}

pc_status pc_flood_contiguous(const pc_px32 *img, int32_t w, int32_t h,
                              size_t stride_px, int32_t sx, int32_t sy,
                              uint32_t lim, uint8_t *mask, pc_fill_stack *st)
{
    pc_px32 seed;
    int32_t x, y;
    if (!img || !mask || !st || w <= 0 || h <= 0) return PC_ERR_ARG;
    if (stride_px < (size_t)w) return PC_ERR_ARG;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return PC_ERR_ARG;
    seed = img[(size_t)sy * stride_px + (size_t)sx];
    st->n = 0u;

#define AT_IMG(xx, yy)  img[(size_t)(yy) * stride_px + (size_t)(xx)]
#define AT_MASK(xx, yy) mask[(size_t)(yy) * (size_t)w + (size_t)(xx)]
#define INSIDE(xx, yy)  (!AT_MASK(xx, yy) && pc_color_within(AT_IMG(xx, yy), seed, lim))

    if (!st_push(st, sx, sy)) return PC_ERR_NOMEM;
    while (st_pop(st, &x, &y)) {
        int32_t l = x, r = x;
        if (!INSIDE(x, y)) continue;
        while (l > 0 && INSIDE(l - 1, y)) l--;
        while (r < w - 1 && INSIDE(r + 1, y)) r++;
        memset(&AT_MASK(l, y), 255, (size_t)(r - l + 1));
        for (int32_t ny = y - 1; ny <= y + 1; ny += 2) {
            if (ny < 0 || ny >= h) continue;
            for (int32_t i = l; i <= r; i++) {
                /* one seed per run of fillable pixels touching the span */
                if (INSIDE(i, ny) && (i == l || !INSIDE(i - 1, ny))) {
                    if (!st_push(st, i, ny)) return PC_ERR_NOMEM;
                }
            }
        }
    }

#undef INSIDE
#undef AT_MASK
#undef AT_IMG
    return PC_OK;
}

void pc_flood_global(const pc_px32 *img, int32_t w, int32_t h,
                     size_t stride_px, pc_px32 seed, uint32_t lim,
                     uint8_t *mask)
{
    if (!img || !mask || w <= 0 || h <= 0 || stride_px < (size_t)w) return;
    for (int32_t y = 0; y < h; y++) {
        const pc_px32 *row = img + (size_t)y * stride_px;
        uint8_t *mrow = mask + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++)
            mrow[x] = (uint8_t)(pc_color_within(row[x], seed, lim) ? 255u : 0u);
    }
}
