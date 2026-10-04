/* test_surf.c - rect math, surfaces, masks, layer transfers, reference
 * compositor against a per-pixel model. */
#include "pc_test.h"
#include "pc/pc_comp.h"
#include "pc/pc_txn.h"

static void t_rects(void)
{
    pc_rect a = pc_rect_make(0, 0, 10, 10), b = pc_rect_make(5, 5, 10, 10);
    pc_rect i = pc_rect_intersect(a, b), u = pc_rect_union(a, b);
    CHECK(i.x == 5 && i.y == 5 && i.w == 5 && i.h == 5);
    CHECK(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);
    CHECK(pc_rect_is_empty(pc_rect_intersect(a, pc_rect_make(10, 0, 5, 5))));
    CHECK(pc_rect_contains(a, 9, 9) && !pc_rect_contains(a, 10, 9));
    u = pc_rect_union(pc_rect_make(0, 0, 0, 0), b);
    CHECK(u.x == 5 && u.w == 10);
}

static void t_alloc_limits(void)
{
    pc_surf s;
    pc_mask m;
    CHECK(pc_surf_alloc(&s, 0, 5) == PC_ERR_ARG && s.px == NULL);
    CHECK(pc_surf_alloc(&s, 70000, 5) == PC_ERR_ARG);
    CHECK(pc_surf_alloc(&s, 3, 4) == PC_OK && s.stride == 3 && s.px[11].a == 0);
    pc_surf_free(&s);
    CHECK(s.px == NULL);
    CHECK(pc_mask_alloc(&m, pc_rect_make(-5, -5, 10, 10)) == PC_OK);
    m.px[0] = 7;
    CHECK(pc_mask_at(&m, -5, -5) == 7 && pc_mask_at(&m, 5, 5) == 0);
    pc_mask_free(&m);
}

static pc_px32 rpx(void)
{
    pc_px32 p;
    p.b = rnd8(); p.g = rnd8(); p.r = rnd8();
    p.a = (rndu(4) == 0) ? 0 : rnd8();
    if (p.a == 0) p.b = p.g = p.r = 0;
    return p;
}

static void t_store_read_comp(void)
{
    const uint32_t W = 150, H = 97;
    pc_doc *d = pc_doc_create(W, H);
    pc_surf s, out, ref;
    size_t tiles0, bytes0;
    pc_tile_stats(&tiles0, &bytes0);
    CHECK(d != NULL);
    CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
    CHECK(pc_surf_alloc(&out, (int32_t)W + 8, (int32_t)H + 8) == PC_OK);
    CHECK(pc_surf_alloc(&ref, (int32_t)W + 8, (int32_t)H + 8) == PC_OK);
    for (int k = 0; k < 4; k++) {
        pc_layer *l = pc_layer_create(d, "L");
        CHECK(pc_doc_reserve_layers(d, d->n_layers + 1u) == PC_OK);
        for (int32_t i = 0; i < s.w * s.h; i++) s.px[i] = rpx();
        /* store only a sub-rect so some tiles stay NULL */
        CHECK(pc_layer_store_rect(d, l, pc_rect_make(10 * k, 3, 70, 60),
                                  s.px + 3 * s.stride + 10 * k, (size_t)s.stride) == PC_OK);
        l->mode = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        l->opacity = (uint8_t)(k == 2 ? 0 : rnd8());
        l->visible = k != 3 || rndu(2);
        CHECK(pc_doc_insert_layer(d, l, d->n_layers) == PC_OK);
        /* read back */
        {
            pc_px32 buf[70 * 60];
            pc_layer_read_rect(d, l, pc_rect_make(10 * k, 3, 70, 60), buf, 70);
            CHECK(memcmp(buf, s.px + 3 * s.stride + 10 * k, 0) == 0);
            for (int y = 0; y < 60; y++)
                CHECK(memcmp(buf + y * 70, s.px + (3 + y) * s.stride + 10 * k,
                             70 * sizeof(pc_px32)) == 0);
        }
    }
    CHECK(pc_doc_edge_padding_is_zero(d));
    /* reference: per-pixel composite, rect hanging outside the doc */
    for (int32_t y = 0; y < ref.h; y++)
        for (int32_t x = 0; x < ref.w; x++) {
            pc_px32 acc = {0, 0, 0, 0};
            int32_t dx = x - 4, dy = y - 4;
            if (dx >= 0 && dy >= 0 && dx < (int32_t)W && dy < (int32_t)H)
                for (uint32_t i = 0; i < d->n_layers; i++) {
                    const pc_layer *l = d->stack[i];
                    pc_px32 p = pc_layer_get_px(l, (uint32_t)dx, (uint32_t)dy);
                    if (!l->visible || l->opacity == 0) continue;
                    if (!l->grid[(size_t)(dy >> 6) * l->tiles_x + (size_t)(dx >> 6)]) continue;
                    pc_composite_span(&acc, &p, 1, l->mode, l->opacity);
                }
            ref.px[y * ref.stride + x] = acc;
        }
    CHECK(pc_comp_rect(d, pc_rect_make(-4, -4, out.w, out.h), out.px, (size_t)out.stride,
                       NULL) == PC_OK);
    CHECK(memcmp(out.px, ref.px, (size_t)out.w * (size_t)out.h * 4u) == 0);
    pc_surf_free(&s); pc_surf_free(&out); pc_surf_free(&ref);
    pc_doc_destroy(d);
    {
        size_t t1, b1;
        pc_tile_stats(&t1, &b1);
        CHECK(t1 == tiles0 && b1 == bytes0);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_rects);
    RUN(t_alloc_limits);
    RUN(t_store_read_comp);
    return pc_test_finish();
}
