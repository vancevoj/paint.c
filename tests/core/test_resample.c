/* test_resample.c - resize modes against an independent double-precision
 * reference, identity and constant-image exactness, grid streaming equals
 * the surface path for any thread count, A8 grids, OOM; transform sampling
 * (identity, integer shifts, 90 degree rotation, wrap modes, edges,
 * supersampling) and the matrix helpers. */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_resample.h"

#include <math.h>

/* ---- independent reference ------------------------------------------------ */
static double ref_cubic(double x, double B, double C)
{
    double ax = fabs(x), x2 = ax * ax, x3 = x2 * ax;
    if (ax < 1.0)
        return ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B)) / 6;
    if (ax < 2.0) return ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * ax +
                          (8 * B + 24 * C)) / 6;
    return 0.0;
}

static double ref_sinc(double x)
{
    return x == 0.0 ? 1.0 : sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
}

/* kernel ids: 0 nearest, 1 box, 2 tent, 3 CR, 4 BS, 5 lanczos3 */
static void ref_kernel_for(pc_resample m, bool shrink, int *k, bool *widen)
{
    *widen = true;
    switch (m) {
    case PC_RESAMPLE_NEAREST: *k = 0; break;
    case PC_RESAMPLE_BILINEAR_LOW: *k = 2; *widen = false; break;
    case PC_RESAMPLE_BILINEAR: *k = 2; break;
    case PC_RESAMPLE_BICUBIC: *k = 3; break;
    case PC_RESAMPLE_BICUBIC_SMOOTH: *k = 4; break;
    case PC_RESAMPLE_LANCZOS3: *k = 5; break;
    case PC_RESAMPLE_FANT: if (shrink) *k = 1; else { *k = 2; *widen = false; } break;
    case PC_RESAMPLE_ADAPTIVE_SHARP: *k = shrink ? 5 : 3; break;
    default: *k = shrink ? 1 : 3; break;
    }
}

static double ref_k(int k, double x)
{
    switch (k) {
    case 2: return fabs(x) < 1.0 ? 1.0 - fabs(x) : 0.0;
    case 3: return ref_cubic(x, 0.0, 0.5);
    case 4: return ref_cubic(x, 1.0, 0.0);
    case 5: return fabs(x) < 3.0 ? ref_sinc(x) * ref_sinc(x / 3.0) : 0.0;
    default: return 0.0;
    }
}

/* Dense weight matrix row: weights over all n_in source pixels for output o. */
static void ref_weights(pc_resample m, uint32_t n_in, uint32_t n_out, uint32_t o, double *w)
{
    int k;
    bool widen;
    double ratio = (double)n_in / n_out, sum = 0.0;
    ref_kernel_for(m, n_out < n_in, &k, &widen);
    for (uint32_t j = 0; j < n_in; j++) w[j] = 0.0;
    if (k == 0) {
        uint32_t j = (uint32_t)(((2.0 * o + 1.0) * n_in) / (2.0 * n_out));
        if (j >= n_in) j = n_in - 1u;
        w[j] = 1.0;
        return;
    }
    if (k == 1) {
        double a = o * ratio, b = (o + 1) * ratio;
        for (uint32_t j = 0; j < n_in; j++) {
            double ov = fmin(b, j + 1.0) - fmax(a, (double)j);
            if (ov > 0) w[j] += ov;
        }
    } else {
        double fs = (widen && n_out < n_in) ? (double)n_out / n_in : 1.0;
        double c = (o + 0.5) * ratio;
        for (int64_t j = (int64_t)floor(c) - 400; j <= (int64_t)ceil(c) + 400; j++) {
            double v = ref_k(k, (j + 0.5 - c) * fs);
            int64_t jj = j < 0 ? 0 : (j >= (int64_t)n_in ? (int64_t)n_in - 1 : j);
            w[jj] += v;
        }
    }
    for (uint32_t j = 0; j < n_in; j++) sum += w[j];
    for (uint32_t j = 0; j < n_in; j++) w[j] /= sum;
}

/* Reference resize in double, premultiplied, then the same output rule. */
static void ref_resize(const pc_surf *src, pc_surf *dst, pc_resample m)
{
    uint32_t sw = (uint32_t)src->w, sh = (uint32_t)src->h, dw = (uint32_t)dst->w,
        dh = (uint32_t)dst->h;
    double *wx = (double *)malloc((size_t)dw * sw * sizeof(double));
    double *wy = (double *)malloc((size_t)dh * sh * sizeof(double));
    double *tmp = (double *)malloc((size_t)dw * sh * 4u * sizeof(double));
    if (!wx || !wy || !tmp) abort();
    for (uint32_t o = 0; o < dw; o++) ref_weights(m, sw, dw, o, wx + (size_t)o * sw);
    for (uint32_t o = 0; o < dh; o++) ref_weights(m, sh, dh, o, wy + (size_t)o * sh);
    for (uint32_t y = 0; y < sh; y++)
        for (uint32_t ox = 0; ox < dw; ox++) {
            double acc[4] = {0, 0, 0, 0};
            for (uint32_t x = 0; x < sw; x++) {
                pc_px32 p = pc_surf_row(src, (int32_t)y)[x];
                double w = wx[(size_t)ox * sw + x], a = p.a / 255.0;
                if (w == 0.0) continue;
                acc[0] += w * p.b * a; acc[1] += w * p.g * a;
                acc[2] += w * p.r * a; acc[3] += w * p.a;
            }
            memcpy(tmp + ((size_t)y * dw + ox) * 4u, acc, sizeof acc);
        }
    for (uint32_t oy = 0; oy < dh; oy++)
        for (uint32_t ox = 0; ox < dw; ox++) {
            double acc[4] = {0, 0, 0, 0};
            pc_px32 *o = &pc_surf_row(dst, (int32_t)oy)[ox];
            for (uint32_t y = 0; y < sh; y++) {
                double w = wy[(size_t)oy * sh + y];
                if (w == 0.0) continue;
                for (int c = 0; c < 4; c++)
                    acc[c] += w * tmp[((size_t)y * dw + ox) * 4u + (size_t)c];
            }
            if (acc[3] < 0.5) { memset(o, 0, 4u); continue; }
            if (acc[3] > 255.0) acc[3] = 255.0;
            for (int c = 0; c < 3; c++) {
                double v = acc[c] < 0 ? 0 : (acc[c] > acc[3] ? acc[3] : acc[c]);
                ((uint8_t *)o)[c] = (uint8_t)floor(v * 255.0 / acc[3] + 0.5);
            }
            o->a = (uint8_t)floor(acc[3] + 0.5);
        }
    free(wx); free(wy); free(tmp);
}

static int px_err(pc_px32 a, pc_px32 b)
{
    int e = abs((int)a.a - (int)b.a);
    if (a.a >= 24u && b.a >= 24u) {
        e = e > abs((int)a.r - (int)b.r) ? e : abs((int)a.r - (int)b.r);
        e = e > abs((int)a.g - (int)b.g) ? e : abs((int)a.g - (int)b.g);
        e = e > abs((int)a.b - (int)b.b) ? e : abs((int)a.b - (int)b.b);
    } else {   /* low alpha: compare premultiplied */
        int pa = (a.r * a.a + 127) / 255, pb = (b.r * b.a + 127) / 255;
        e = e > abs(pa - pb) ? e : abs(pa - pb);
    }
    return e;
}

static void fill_random_surf(pc_surf *s, int smooth)
{
    pc_px32 c0 = tu_rpx(), c1 = tu_rpx();
    for (int32_t y = 0; y < s->h; y++)
        for (int32_t x = 0; x < s->w; x++) {
            pc_px32 *p = &pc_surf_row(s, y)[x];
            if (smooth && rndu(8u)) {
                uint32_t t = (uint32_t)((x * 7 + y * 3) % 256);
                p->b = (uint8_t)((c0.b * (255u - t) + c1.b * t) / 255u);
                p->g = (uint8_t)((c0.g * (255u - t) + c1.g * t) / 255u);
                p->r = (uint8_t)((c0.r * (255u - t) + c1.r * t) / 255u);
                p->a = (uint8_t)((c0.a * (255u - t) + c1.a * t) / 255u);
                if (!p->a) p->b = p->g = p->r = 0;
            } else {
                *p = tu_rpx();
            }
        }
}

static void t_rs_vs_reference(void)
{
    int rounds = g_quick ? 3 : 12;
    int worst[PC_RESAMPLE_COUNT];
    memset(worst, 0, sizeof worst);
    for (int round = 0; round < rounds; round++)
        for (int m = 0; m < (int)PC_RESAMPLE_COUNT; m++) {
            pc_surf src, got, want;
            int32_t sw = 1 + (int32_t)rndu(90u), sh = 1 + (int32_t)rndu(70u);
            int32_t dw = 1 + (int32_t)rndu(150u), dh = 1 + (int32_t)rndu(110u);
            if (round == 0) { dw = 1 + sw / 5; dh = 1 + sh / 4; }      /* strong shrink */
            if (round == 1) { dw = sw * 3 + 1; dh = sh * 2 + 3; }      /* enlarge */
            CHECK(pc_surf_alloc(&src, sw, sh) == PC_OK);
            CHECK(pc_surf_alloc(&got, dw, dh) == PC_OK);
            CHECK(pc_surf_alloc(&want, dw, dh) == PC_OK);
            fill_random_surf(&src, (int)rndu(2u));
            CHECK(pc_resample_surf(&src, &got, (pc_resample)m, 0u, NULL) == PC_OK);
            ref_resize(&src, &want, (pc_resample)m);
            for (int32_t i = 0; i < dw * dh; i++) {
                int e = px_err(got.px[i], want.px[i]);
                if (e > worst[m]) worst[m] = e;
            }
            pc_surf_free(&src); pc_surf_free(&got); pc_surf_free(&want);
        }
    for (int m = 0; m < (int)PC_RESAMPLE_COUNT; m++) {
        INFO("%-24s max error vs double reference %d", pc_resample_name((pc_resample)m), worst[m]);
        CHECK(worst[m] <= 1);
    }
    CHECK(strcmp(pc_resample_name(PC_RESAMPLE_COUNT), "?") == 0);
}

/* Tiny sources enlarged a lot: clamped edges merge negative lobes, so the
 * contribution tables are not monotonic (regression for a footprint bug). */
static void t_rs_tiny(void)
{
    int worst = 0;
    for (int32_t sw = 1; sw <= 5; sw++)
        for (int32_t dw = 1; dw <= (g_quick ? 23 : 70); dw += (g_quick ? 2 : 1))
            for (int m = 0; m < (int)PC_RESAMPLE_COUNT; m++) {
                pc_surf src, got, want;
                int32_t sh = 1 + (int32_t)rndu(4u), dh = 1 + (int32_t)rndu(40u);
                CHECK(pc_surf_alloc(&src, sw, sh) == PC_OK);
                CHECK(pc_surf_alloc(&got, dw, dh) == PC_OK);
                CHECK(pc_surf_alloc(&want, dw, dh) == PC_OK);
                fill_random_surf(&src, 0);
                CHECK(pc_resample_surf(&src, &got, (pc_resample)m, 0u, NULL) == PC_OK);
                ref_resize(&src, &want, (pc_resample)m);
                for (int32_t i = 0; i < dw * dh; i++) {
                    int e = px_err(got.px[i], want.px[i]);
                    if (e > worst) worst = e;
                }
                pc_surf_free(&src); pc_surf_free(&got); pc_surf_free(&want);
            }
    INFO("tiny sources: max error %d", worst);
    CHECK(worst <= 1);
}

static void t_rs_identity_constant(void)
{
    for (int m = 0; m < (int)PC_RESAMPLE_COUNT; m++) {
        pc_surf src, dst;
        int32_t w = 1 + (int32_t)rndu(150u), h = 1 + (int32_t)rndu(100u);
        CHECK(pc_surf_alloc(&src, w, h) == PC_OK);
        CHECK(pc_surf_alloc(&dst, w, h) == PC_OK);
        fill_random_surf(&src, 0);
        CHECK(pc_resample_surf(&src, &dst, (pc_resample)m, 0u, NULL) == PC_OK);
        if (m != (int)PC_RESAMPLE_BICUBIC_SMOOTH)   /* B-spline is approximating */
            CHECK(memcmp(src.px, dst.px, (size_t)w * (size_t)h * 4u) == 0);
        /* gamma round trip at identity stays within 1 */
        if (m != (int)PC_RESAMPLE_BICUBIC_SMOOTH) {
            int e = 0;
            CHECK(pc_resample_surf(&src, &dst, (pc_resample)m, PC_RESAMPLE_GAMMA, NULL) == PC_OK);
            for (int32_t i = 0; i < w * h; i++) {
                int q = px_err(src.px[i], dst.px[i]);
                if (q > e) e = q;
            }
            CHECK(e <= 1);
        }
        pc_surf_free(&src); pc_surf_free(&dst);
        /* constant images stay exactly constant at any size */
        for (int k = 0; k < 3; k++) {
            pc_px32 c = tu_rpx();
            int32_t dw = 1 + (int32_t)rndu(200u), dh = 1 + (int32_t)rndu(200u);
            bool ok = true;
            if (c.a == 0u) c.a = 77u;
            CHECK(pc_surf_alloc(&src, w, h) == PC_OK);
            CHECK(pc_surf_alloc(&dst, dw, dh) == PC_OK);
            for (int32_t i = 0; i < w * h; i++) src.px[i] = c;
            CHECK(pc_resample_surf(&src, &dst, (pc_resample)m, 0u, NULL) == PC_OK);
            for (int32_t i = 0; i < dw * dh; i++) if (memcmp(&dst.px[i], &c, 4u)) ok = false;
            CHECK(ok);
            CHECK(pc_resample_surf(&src, &dst, (pc_resample)m, PC_RESAMPLE_GAMMA, NULL) == PC_OK);
            ok = true;
            for (int32_t i = 0; i < dw * dh; i++) if (px_err(dst.px[i], c) > 1) ok = false;
            CHECK(ok);
            pc_surf_free(&src); pc_surf_free(&dst);
        }
    }
    {   /* gamma correction keeps the average luminance of a fine checker */
        pc_surf src, lin, gam;
        pc_px32 black = {0, 0, 0, 255}, white = {255, 255, 255, 255};
        CHECK(pc_surf_alloc(&src, 64, 64) == PC_OK);
        CHECK(pc_surf_alloc(&lin, 8, 8) == PC_OK);
        CHECK(pc_surf_alloc(&gam, 8, 8) == PC_OK);
        for (int32_t y = 0; y < 64; y++)
            for (int32_t x = 0; x < 64; x++) src.px[y * 64 + x] = ((x + y) & 1) ? white : black;
        CHECK(pc_resample_surf(&src, &lin, PC_RESAMPLE_FANT, 0u, NULL) == PC_OK);
        CHECK(pc_resample_surf(&src, &gam, PC_RESAMPLE_FANT, PC_RESAMPLE_GAMMA, NULL) == PC_OK);
        CHECK(abs((int)lin.px[9].g - 128) <= 1);
        CHECK(abs((int)gam.px[9].g - 188) <= 1);     /* sRGB of linear 0.5 */
        pc_surf_free(&src); pc_surf_free(&lin); pc_surf_free(&gam);
    }
    {   /* bad arguments */
        pc_surf a, b;
        CHECK(pc_surf_alloc(&a, 4, 4) == PC_OK);
        CHECK(pc_surf_alloc(&b, 4, 4) == PC_OK);
        CHECK(pc_resample_surf(&a, &b, PC_RESAMPLE_COUNT, 0u, NULL) == PC_ERR_ARG);
        CHECK(pc_resample_surf(NULL, &b, PC_RESAMPLE_FANT, 0u, NULL) == PC_ERR_ARG);
        pc_surf_free(&a); pc_surf_free(&b);
    }
}

/* Grid streaming equals the surface path, any thread count; A8 grids. */
static void t_rs_grid(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 4 : 16;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(400u), H = 1u + rndu(300u);
        uint32_t DW = 1u + rndu(500u), DH = 1u + rndu(400u);
        pc_doc *d = tu_random_doc(W, H, 1u);
        pc_resample m = (pc_resample)rndu(PC_RESAMPLE_COUNT);
        uint32_t flags = rndu(2u) ? PC_RESAMPLE_GAMMA : 0u;
        pc_grid g = pc_grid_of_layer(d, d->stack[0]);
        pc_tile **out = NULL, **out2 = NULL;
        pc_surf src, ref;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 6u, 31u + (uint64_t)round);
        uint32_t dtx = (DW + 63u) / 64u, dty = (DH + 63u) / 64u;
        bool ok = true, pad = true;
        CHECK(pc_surf_alloc(&src, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&ref, (int32_t)DW, (int32_t)DH) == PC_OK);
        pc_layer_read_rect(d, d->stack[0], pc_doc_rect(d), src.px, (size_t)src.stride);
        CHECK(pc_resample_surf(&src, &ref, m, flags, NULL) == PC_OK);
        CHECK(pc_resample_grid(&g, DW, DH, m, flags, NULL, &out) == PC_OK);
        CHECK(pc_resample_grid(&g, DW, DH, m, flags, &par, &out2) == PC_OK);
        for (uint32_t i = 0; i < dtx * dty; i++) {
            if (!pc_tile_equal(out[i], out2[i])) ok = false;
            if (out[i] && pc_tile_is_zero(out[i])) ok = false;     /* sparse */
        }
        for (uint32_t y = 0; y < dty * 64u; y++)
            for (uint32_t x = 0; x < dtx * 64u; x++) {
                const pc_tile *t = out[(y / 64u) * dtx + x / 64u];
                pc_px32 p = {0, 0, 0, 0};
                if (t) memcpy(&p, t->data + ((y % 64u) * 64u + x % 64u) * 4u, 4u);
                if (x < DW && y < DH) {
                    if (memcmp(&p, &ref.px[(size_t)y * DW + x], 4u) != 0) ok = false;
                } else if (p.a | p.r | p.g | p.b) {
                    pad = false;
                }
            }
        CHECK(ok);
        CHECK(pad);
        pc_grid_free(out, (size_t)dtx * dty);
        pc_grid_free(out2, (size_t)dtx * dty);
        pc_surf_free(&src); pc_surf_free(&ref);
        fake_par_free(&fp);

        /* A8 grid: nearest is exact, others within 1 of a float reference
         * computed through the BGRA path on a gray-alpha image */
        {
            size_t n = (size_t)d->tiles_x * d->tiles_y;
            pc_tile **mg = (pc_tile **)calloc(n, sizeof *mg);
            pc_grid ag;
            pc_surf as, ar;
            int worst = 0;
            CHECK(pc_surf_alloc(&as, (int32_t)W, (int32_t)H) == PC_OK);
            CHECK(pc_surf_alloc(&ar, (int32_t)DW, (int32_t)DH) == PC_OK);
            for (size_t i = 0; i < n; i++) {
                if (rndu(3u) == 0u) continue;
                mg[i] = pc_tile_new_zero(1u);
                for (uint32_t k = 0; k < 4096u; k++) {
                    uint32_t x = (uint32_t)(i % d->tiles_x) * 64u + k % 64u;
                    uint32_t y = (uint32_t)(i / d->tiles_x) * 64u + k / 64u;
                    if (x < W && y < H) mg[i]->data[k] = rnd8();
                }
            }
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    const pc_tile *t = mg[(y / 64u) * d->tiles_x + x / 64u];
                    uint8_t v = t ? t->data[(y % 64u) * 64u + x % 64u] : 0u;
                    pc_px32 p = {255, 255, 255, v};
                    if (!v) p.b = p.g = p.r = 0;
                    as.px[(size_t)y * W + x] = p;
                }
            ag.tiles = (const pc_tile *const *)mg;
            ag.w = W; ag.h = H; ag.tiles_x = d->tiles_x; ag.tiles_y = d->tiles_y; ag.bpp = 1u;
            CHECK(pc_resample_grid(&ag, DW, DH, m, 0u, NULL, &out) == PC_OK);
            CHECK(pc_resample_surf(&as, &ar, m, 0u, NULL) == PC_OK);
            for (uint32_t y = 0; y < DH; y++)
                for (uint32_t x = 0; x < DW; x++) {
                    const pc_tile *t = out[(y / 64u) * dtx + x / 64u];
                    int v = t ? t->data[(y % 64u) * 64u + x % 64u] : 0;
                    int e = abs(v - (int)ar.px[(size_t)y * DW + x].a);
                    if (t && t->bpp != 1u) e = 99;
                    if (e > worst) worst = e;
                }
            CHECK(worst <= (m == PC_RESAMPLE_NEAREST ? 0 : 1));
            pc_grid_free(out, (size_t)dtx * dty);
            pc_grid_free(mg, n);
            pc_surf_free(&as); pc_surf_free(&ar);
        }
        pc_doc_destroy(d);
    }
    {   /* bad grids */
        pc_tile **out = (pc_tile **)1;
        pc_grid g;
        memset(&g, 0, sizeof g);
        CHECK(pc_resample_grid(&g, 10u, 10u, PC_RESAMPLE_BICUBIC, 0u, NULL, &out) == PC_ERR_ARG);
        CHECK(out == NULL);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_rs_oom(void)
{
    size_t t0, l0;
    unsigned long fails = 0;
    pc_doc *d;
    pc_grid g;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(300u, 200u, 1u);
    g = pc_grid_of_layer(d, d->stack[0]);
    for (long k = 0; k < 30; k++) {
        pc_tile **out = NULL;
        pc_status st;
        pc_fault_set(k);
        st = pc_resample_grid(&g, 333u, 111u, PC_RESAMPLE_LANCZOS3, PC_RESAMPLE_GAMMA, NULL, &out);
        pc_fault_set(-1);
        if (st != PC_OK) { CHECK(st == PC_ERR_NOMEM && out == NULL); fails++; }
        else pc_grid_free(out, 6u * 2u);
        pc_fault_set(k);
        st = pc_warp_grid(&g, &(pc_warp){pc_xform_rotate(10.0), PC_SAMPLE_BILINEAR, PC_WRAP_NONE,
                                         2u, true, {0, 0, 0, 0}}, 300u, 200u, NULL, &out);
        pc_fault_set(-1);
        if (st != PC_OK) { CHECK(st == PC_ERR_NOMEM && out == NULL); fails++; }
        else pc_grid_free(out, 5u * 4u);
    }
    CHECK(fails > 10u);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

/* ---- transforms ------------------------------------------------------------------ */
static void t_xform(void)
{
    pc_xform a = pc_xform_mul(pc_xform_translate(3.0, -2.0),
                              pc_xform_mul(pc_xform_rotate(33.0), pc_xform_scale(2.0, 0.5)));
    pc_xform inv, id;
    double x, y, u, v;
    CHECK(pc_xform_invert(a, &inv));
    id = pc_xform_mul(a, inv);
    for (int i = 0; i < 9; i++) CHECK(fabs(id.m[i] - ((i % 4 == 0) ? 1.0 : 0.0)) < 1e-12);
    CHECK(pc_xform_apply(&a, 1.0, 0.0, &x, &y));
    CHECK(pc_xform_apply(&inv, x, y, &u, &v));
    CHECK(fabs(u - 1.0) < 1e-12 && fabs(v) < 1e-12);
    /* rotate(90) maps east to north (counterclockwise on a y-down screen) */
    a = pc_xform_rotate(90.0);
    CHECK(pc_xform_apply(&a, 1.0, 0.0, &x, &y));
    CHECK(fabs(x) < 1e-12 && fabs(y + 1.0) < 1e-12);
    CHECK(!pc_xform_invert(pc_xform_scale(0.0, 1.0), &inv));
    a = pc_xform_identity();
    a.m[8] = -1.0;
    CHECK(!pc_xform_apply(&a, 1.0, 1.0, &x, &y));
}

static pc_warp mkwarp(pc_xform fwd, pc_sample s, pc_wrap w, uint32_t q, bool aa)
{
    pc_warp p;
    if (!pc_xform_invert(fwd, &p.inv)) abort();
    p.sample = s; p.wrap = w; p.quality = q; p.aa_edges = aa;
    p.src_rect = pc_rect_make(0, 0, 0, 0);
    return p;
}

static void t_warp(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 4 : 12); round++) {
        int32_t w = 2 + (int32_t)rndu(100u), h = 2 + (int32_t)rndu(80u);
        pc_surf src, dst, dst2;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 3u, 77u);
        CHECK(pc_surf_alloc(&src, w, h) == PC_OK);
        CHECK(pc_surf_alloc(&dst, w, h) == PC_OK);
        CHECK(pc_surf_alloc(&dst2, w, h) == PC_OK);
        fill_random_surf(&src, 0);
        /* identity is exact for every sampler, with and without aa */
        for (int s = 0; s < 3; s++) {
            pc_warp wp = mkwarp(pc_xform_identity(), (pc_sample)s, PC_WRAP_NONE, 1u, s == 1);
            CHECK(pc_warp_surf(&src, &wp, &dst, 0, 0, &par) == PC_OK);
            CHECK(memcmp(src.px, dst.px, (size_t)w * (size_t)h * 4u) == 0);
        }
        /* integer shift with repeat = cyclic shift; with none = shift + clear */
        {
            int32_t sx = (int32_t)rndu((uint32_t)w), sy = (int32_t)rndu((uint32_t)h);
            pc_warp wr = mkwarp(pc_xform_translate(sx, sy), PC_SAMPLE_BILINEAR, PC_WRAP_REPEAT, 1u,
                                false);
            pc_warp wn = mkwarp(pc_xform_translate(sx, sy), PC_SAMPLE_BICUBIC, PC_WRAP_NONE, 1u,
                                false);
            bool ok = true, ok2 = true;
            CHECK(pc_warp_surf(&src, &wr, &dst, 0, 0, NULL) == PC_OK);
            CHECK(pc_warp_surf(&src, &wn, &dst2, 0, 0, NULL) == PC_OK);
            for (int32_t y = 0; y < h; y++)
                for (int32_t x = 0; x < w; x++) {
                    pc_px32 e = src.px[((y - sy + h) % h) * w + (x - sx + w) % w];
                    pc_px32 z = {0, 0, 0, 0};
                    if (memcmp(&dst.px[y * w + x], &e, 4u)) ok = false;
                    if (x < sx || y < sy) e = z;
                    if (memcmp(&dst2.px[y * w + x], &e, 4u)) ok2 = false;
                }
            CHECK(ok);
            CHECK(ok2);
        }
        /* mirror: a shift by exactly one period width reflects the image */
        {
            pc_warp wm = mkwarp(pc_xform_translate(w, 0), PC_SAMPLE_NEAREST, PC_WRAP_MIRROR, 1u,
                                false);
            bool ok = true;
            CHECK(pc_warp_surf(&src, &wm, &dst, 0, 0, NULL) == PC_OK);
            for (int32_t y = 0; y < h; y++)
                for (int32_t x = 0; x < w; x++)
                    if (memcmp(&dst.px[y * w + x], &src.px[y * w + (w - 1 - x)], 4u)) ok = false;
            CHECK(ok);
        }
        /* rotate 90 around the center of a square equals the exact rotation */
        {
            pc_surf sq, out;
            int32_t n = 2 + (int32_t)rndu(60u);
            pc_xform f = pc_xform_mul(pc_xform_translate(n / 2.0, n / 2.0),
                                      pc_xform_mul(pc_xform_rotate(-90.0),
                                                   pc_xform_translate(-n / 2.0, -n / 2.0)));
            pc_warp wq;
            bool ok = true;
            CHECK(pc_surf_alloc(&sq, n, n) == PC_OK);
            CHECK(pc_surf_alloc(&out, n, n) == PC_OK);
            fill_random_surf(&sq, 0);
            for (int s = 0; s < 3; s++) {
                wq = mkwarp(f, (pc_sample)s, PC_WRAP_NONE, 1u, false);
                CHECK(pc_warp_surf(&sq, &wq, &out, 0, 0, &par) == PC_OK);
                for (int32_t y = 0; y < n; y++)
                    for (int32_t x = 0; x < n; x++)   /* clockwise: new(x,y) = old(y, n-1-x) */
                        if (memcmp(&out.px[y * n + x], &sq.px[(n - 1 - x) * n + y], 4u)) ok = false;
            }
            CHECK(ok);
            pc_surf_free(&sq); pc_surf_free(&out);
        }
        /* edges: opaque square scaled 1.5x, aa gives partial alpha only on the
         * border, hard edges give only 0 or 255; supersampling of a constant
         * stays constant */
        {
            pc_surf sq, out;
            pc_px32 c = {10, 200, 30, 255};
            pc_xform f = pc_xform_mul(pc_xform_translate(7.3, 5.1), pc_xform_scale(1.5, 1.5));
            pc_warp wa = mkwarp(f, PC_SAMPLE_BILINEAR, PC_WRAP_NONE, 1u, true);
            pc_warp wh = mkwarp(f, PC_SAMPLE_BILINEAR, PC_WRAP_NONE, 1u, false);
            pc_warp wq = mkwarp(pc_xform_scale(0.37, 0.41), PC_SAMPLE_BICUBIC, PC_WRAP_REPEAT, 4u,
                                false);
            int partial = 0, inner_ok = 1, hard_ok = 1, const_ok = 1;
            CHECK(pc_surf_alloc(&sq, 20, 20) == PC_OK);
            CHECK(pc_surf_alloc(&out, 50, 50) == PC_OK);
            for (int i = 0; i < 400; i++) sq.px[i] = c;
            CHECK(pc_warp_surf(&sq, &wa, &out, 0, 0, NULL) == PC_OK);
            for (int32_t y = 0; y < 50; y++)
                for (int32_t x = 0; x < 50; x++) {
                    pc_px32 p = out.px[y * 50 + x];
                    if (p.a > 0u && p.a < 255u) partial++;
                    if (x >= 10 && x < 34 && y >= 8 && y < 32 && memcmp(&p, &c, 4u)) inner_ok = 0;
                }
            CHECK(pc_warp_surf(&sq, &wh, &out, 0, 0, NULL) == PC_OK);
            for (int i = 0; i < 2500; i++)
                if (out.px[i].a != 0u && memcmp(&out.px[i], &c, 4u)) hard_ok = 0;
            CHECK(pc_warp_surf(&sq, &wq, &out, 0, 0, NULL) == PC_OK);
            for (int i = 0; i < 2500; i++) if (memcmp(&out.px[i], &c, 4u)) const_ok = 0;
            CHECK(partial > 20 && inner_ok && hard_ok && const_ok);
            pc_surf_free(&sq); pc_surf_free(&out);
        }
        /* grid path equals the surface path */
        {
            pc_doc *d = tu_random_doc((uint32_t)w * 3u, (uint32_t)h * 2u, 1u);
            pc_grid g = pc_grid_of_layer(d, d->stack[0]);
            pc_surf s2, o2;
            pc_tile **out = NULL;
            pc_warp wp = mkwarp(pc_xform_mul(pc_xform_translate(20.0, 3.0), pc_xform_rotate(17.0)),
                                (pc_sample)rndu(3u), (pc_wrap)rndu(3u), 1u + rndu(3u),
                                rndu(2u) != 0u);
            uint32_t DW = d->w + 30u, DH = d->h, dtx = (DW + 63u) / 64u, dty = (DH + 63u) / 64u;
            bool ok = true;
            CHECK(pc_surf_alloc(&s2, (int32_t)d->w, (int32_t)d->h) == PC_OK);
            CHECK(pc_surf_alloc(&o2, (int32_t)DW, (int32_t)DH) == PC_OK);
            pc_layer_read_rect(d, d->stack[0], pc_doc_rect(d), s2.px, (size_t)s2.stride);
            CHECK(pc_warp_surf(&s2, &wp, &o2, 0, 0, NULL) == PC_OK);
            CHECK(pc_warp_grid(&g, &wp, DW, DH, &par, &out) == PC_OK);
            for (uint32_t y = 0; y < DH; y++)
                for (uint32_t x = 0; x < DW; x++) {
                    const pc_tile *t = out[(y / 64u) * dtx + x / 64u];
                    pc_px32 p = {0, 0, 0, 0};
                    if (t) memcpy(&p, t->data + ((y % 64u) * 64u + x % 64u) * 4u, 4u);
                    if (memcmp(&p, &o2.px[(size_t)y * DW + x], 4u)) ok = false;
                }
            CHECK(ok);
            pc_grid_free(out, (size_t)dtx * dty);
            pc_surf_free(&s2); pc_surf_free(&o2);
            pc_doc_destroy(d);
        }
        fake_par_free(&fp);
        pc_surf_free(&src); pc_surf_free(&dst); pc_surf_free(&dst2);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* A large sparse layer resized in bands (informational timing). */
static void t_rs_large(void)
{
    uint32_t W = g_quick ? 3000u : 12000u, H = g_quick ? 2000u : 9000u;
    pc_doc *d = pc_doc_create(W, H);
    pc_layer *l = pc_layer_create(d, "big");
    pc_tile **out = NULL;
    pc_grid g;
    double t0;
    size_t nonnull = 0, n;
    tu_fill_random(d, l, pc_rect_make(100, 100, 700, 500), 1);
    tu_fill_random(d, l, pc_rect_make((int32_t)W - 900, (int32_t)H - 600, 800, 500), 0);
    CHECK(pc_doc_insert_layer(d, l, 0u) == PC_OK);
    g = pc_grid_of_layer(d, l);
    t0 = pc_test_now();
    CHECK(pc_resample_grid(&g, W / 3u, H / 3u, PC_RESAMPLE_BICUBIC, 0u, NULL, &out) == PC_OK);
    n = (size_t)((W / 3u + 63u) / 64u) * ((H / 3u + 63u) / 64u);
    for (size_t i = 0; i < n; i++) nonnull += out[i] != NULL;
    INFO("%ux%u -> %ux%u bicubic in %.3f s, %zu of %zu tiles non-empty", W, H, W / 3u, H / 3u,
         pc_test_now() - t0, nonnull, n);
    CHECK(nonnull > 0u && nonnull < n);
    pc_grid_free(out, n);
    pc_doc_destroy(d);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_rs_vs_reference);
    RUN(t_rs_tiny);
    RUN(t_rs_identity_constant);
    RUN(t_rs_grid);
    RUN(t_rs_oom);
    RUN(t_xform);
    RUN(t_warp);
    RUN(t_rs_large);
    return pc_test_finish();
}
