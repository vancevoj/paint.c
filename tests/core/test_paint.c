/* test_paint.c - pc_paint_apply against a per-pixel reference model. */
#include "pc_test.h"
#include "pc/pc_paint.h"
#include "pc/pc_sel.h"

static pc_px32 rpx(void)
{
    pc_px32 p;
    p.b = rnd8(); p.g = rnd8(); p.r = rnd8(); p.a = rndu(3) ? rnd8() : (rndu(2) ? 0 : 255);
    if (!p.a) p.b = p.g = p.r = 0;
    return p;
}

/* shuffled fake pool: results must not depend on order */
static void fake_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, count - 1u - i, i % 3u);   /* reversed */
}

static void ref_px(pc_px32 *o, pc_px32 paint, uint32_t k, const pc_paint_opts *op)
{
    if (k == 0u) return;
    if (op->opacity != 255u) k = pc_mul255(k, op->opacity);
    if (op->mode == PC_PAINT_ERASE) {
        uint32_t a = pc_mul255(o->a, 255u - k);
        if (!a) memset(o, 0, sizeof *o); else o->a = (uint8_t)a;
    } else if (op->mode == PC_PAINT_OVERWRITE) {
        double ta = k / 255.0, A = o->a * (1 - ta) + paint.a * ta;
        pc_px32 r = {0, 0, 0, 0};
        if (A > 0) {
            r.a = (uint8_t)((o->a * (255u - k) + paint.a * k + 127u) / 255u);
            if (r.a) {
                uint32_t pa = o->a * (255u - k) + paint.a * k;
                r.b = (uint8_t)(((uint64_t)o->b * o->a * (255u - k) + (uint64_t)paint.b * paint.a * k + pa / 2) / pa);
                r.g = (uint8_t)(((uint64_t)o->g * o->a * (255u - k) + (uint64_t)paint.g * paint.a * k + pa / 2) / pa);
                r.r = (uint8_t)(((uint64_t)o->r * o->a * (255u - k) + (uint64_t)paint.r * paint.a * k + pa / 2) / pa);
            } else memset(&r, 0, sizeof r);
        }
        *o = r;
    } else {
        paint.a = (uint8_t)pc_mul255(paint.a, k);
        pc_composite_span(o, &paint, 1, op->blend, 255);
    }
}

static void t_paint_random(void)
{
    int iters = g_quick ? 60 : 400;
    pc_par fake = { fake_run, NULL, 3 };
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(200), H = 1 + rndu(150);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_layer *l = pc_layer_create(d, "L");
        pc_surf base, got, want;
        pc_mask cov;
        pc_paint_opts op = pc_paint_opts_default();
        pc_paint_src src;
        pc_txn *t;
        bool use_sel = rndu(2) == 0;
        pc_rect cr = pc_rect_make((int32_t)rndu(W) - 20, (int32_t)rndu(H) - 20, 1 + (int32_t)rndu(W), 1 + (int32_t)rndu(H));
        CHECK(pc_surf_alloc(&base, (int32_t)W, (int32_t)H) == PC_OK);
        for (int32_t i = 0; i < base.w * base.h; i++) base.px[i] = rpx();
        CHECK(pc_layer_store_rect(d, l, pc_doc_rect(d), base.px, (size_t)base.stride) == PC_OK);
        CHECK(pc_hist_add_layer(h, l, 0, "add") == PC_OK);
        if (use_sel) CHECK(pc_sel_apply_rect(h, pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H), 1 + (int32_t)rndu(W), 1 + (int32_t)rndu(H)), PC_SEL_REPLACE, "sel") == PC_OK);
        CHECK(pc_mask_alloc(&cov, cr) == PC_OK);
        for (int32_t i = 0; i < cov.w * cov.h; i++) cov.px[i] = rndu(3) ? rnd8() : 0;
        op.mode = (pc_paint_mode)rndu(3);
        op.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        op.opacity = rndu(2) ? 255 : rnd8();
        op.clip_to_selection = rndu(4) != 0;
        memset(&src, 0, sizeof src);
        src.solid = rpx();
        t = pc_txn_begin(d, "paint");
        CHECK(t != NULL);
        /* twice (first with the reversed fake pool alone): must not compound */
        CHECK(pc_paint_apply(t, l->id, &cov, &src, &op, &fake, NULL) == PC_OK);
        if (rndu(2)) CHECK(pc_paint_apply(t, l->id, &cov, &src, &op, NULL, NULL) == PC_OK);
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), got.px, (size_t)got.stride) == PC_OK);
        CHECK(pc_surf_alloc(&want, (int32_t)W, (int32_t)H) == PC_OK);
        memcpy(want.px, base.px, (size_t)W * H * 4u);
        for (int32_t y = 0; y < (int32_t)H; y++)
            for (int32_t x = 0; x < (int32_t)W; x++) {
                uint32_t k = pc_mask_at(&cov, x, y);
                if (op.clip_to_selection && pc_sel_is_active(d)) k = pc_mul255(k, pc_sel_coverage(d, x, y));
                ref_px(&want.px[y * want.stride + x], src.solid, k, &op);
            }
        CHECK(memcmp(got.px, want.px, (size_t)W * H * 4u) == 0);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        pc_surf_free(&base); pc_surf_free(&got); pc_surf_free(&want); pc_mask_free(&cov);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    RUN(t_paint_random);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1);
    return pc_test_finish();
}
