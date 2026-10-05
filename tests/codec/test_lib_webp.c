/* test_lib_webp.c - WebP codec (libwebp): lossless exact round trips,
 * lossy PSNR bounds per quality, presets and effort, ICC chunk, animated
 * first frame, fixtures from libwebp's simple API, limits, fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "icc_test_util.h"

#include "webp/decode.h"
#include "webp/encode.h"
#include "webp/mux.h"

static const pc_codec *wp(void) { return pc_codec_by_id("webp"); }

typedef struct webp_params_t { int32_t preset, quality, effort, lossless; } webp_params_t;

static pc_doc *load_ok(const uint8_t *p, size_t n, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_status st = wp()->load(p, n, NULL, &d, m);
    CHECK(st == PC_OK);
    if (st != PC_OK) INFO("webp load: %s", pc_status_str(st));
    return d;
}

static void t_sniff_params(void)
{
    static const uint8_t hdr[12] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P' };
    webp_params_t p;
    CHECK(wp() != NULL);
    CHECK(wp()->sniff(hdr, 12) && !wp()->sniff(hdr, 11));
    CHECK(pc_codec_sniff(hdr, 12) == wp());
    CHECK(wp()->params_size == sizeof p && wp()->n_props == 4u);
    pc_codec_default_params(wp(), &p);
    CHECK(p.preset == 2 && p.quality == 95 && p.effort == 7 && p.lossless == 0);
}

static void t_lossless(void)
{
    const uint32_t W = 90, H = 70;
    pc_px32 *a = tu_noise(W, H, 1), *b = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    pc_px32 *flat;
    webp_params_t p;
    pc_buf out;
    pc_image_meta m;
    tu_doc_add_layer(d, b, PC_BLEND_SCREEN, 180, true, "s");
    flat = tu_flatten(d);
    for (int effort = 0; effort <= 9; effort += 3) {
        memset(&out, 0, sizeof out);
        pc_codec_default_params(wp(), &p);
        p.lossless = 1;
        p.effort = effort;
        CHECK(wp()->save(d, NULL, &p, NULL, &out) == PC_OK);
        r = load_ok(out.p, out.n, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK(tu_diff(px, flat, (size_t)W * H) == 0);
            CHECK(m.had_alpha);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        pc_buf_free(&out);
    }
    free(flat); free(a); free(b);
    pc_doc_destroy(d);
}

static void t_lossy(void)
{
    const uint32_t W = 128, H = 96;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int qs[] = { 5, 50, 95, 100 };
    for (int preset = 0; preset < 6; preset++)
        for (size_t qi = 0; qi < sizeof qs / sizeof qs[0]; qi++) {
            webp_params_t p;
            pc_buf out;
            pc_image_meta m;
            pc_doc *r;
            if (preset != 2 && qi != 2) continue;      /* full sweep only for Photo */
            memset(&out, 0, sizeof out);
            pc_codec_default_params(wp(), &p);
            p.preset = preset;
            p.quality = qs[qi];
            p.effort = qi == 0 ? 0 : 7;
            CHECK(wp()->save(d, NULL, &p, NULL, &out) == PC_OK);
            r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                /* compare premultiplied-equivalent colors: only where opaque enough */
                double ps = tu_psnr(px, a, (size_t)W * H);
                double need = qs[qi] >= 95 ? 34.0 : (qs[qi] >= 50 ? 28.0 : 20.0);
                size_t abad = 0;
                CHECK(ps >= need);
                if (ps < need) INFO("preset %d q %d psnr %.2f", preset, qs[qi], ps);
                for (size_t i = 0; i < (size_t)W * H; i++) if (px[i].a != a[i].a) abad++;
                CHECK(abad == 0);                  /* alpha plane is lossless by default */
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
    pc_doc_destroy(d);
    free(a);
}

static void t_icc_and_limits(void)
{
    const uint32_t W = 20, H = 10;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    pc_image_meta meta, m;
    pc_buf out;
    pc_codec_limits lim;
    size_t icc_n = 0;
    uint8_t *icc = itu_rgb("webp ICCP", 120u, &icc_n);    /* usable (FS-ICC) */
    CHECK(icc != NULL);
    memset(&meta, 0, sizeof meta);
    meta.icc = icc;
    meta.icc_len = icc_n;
    memset(&out, 0, sizeof out);
    CHECK(wp()->save(d, &meta, NULL, NULL, &out) == PC_OK);
    r = load_ok(out.p, out.n, &m);
    CHECK(icc && m.icc_len == icc_n && m.icc && memcmp(m.icc, icc, icc_n) == 0);
    pc_doc_destroy(r);
    pc_meta_free(&m);
    pc_codec_limits_default(&lim);
    lim.max_w = 19;
    r = (pc_doc *)1;
    CHECK(wp()->load(out.p, out.n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL && m.icc == NULL);
    CHECK(wp()->load(out.p, out.n / 2, NULL, &r, &m) != PC_OK && r == NULL);
    pc_buf_free(&out);
    {
        pc_doc *big = pc_doc_create(16384, 1);
        CHECK(wp()->save(big, NULL, NULL, NULL, &out) == PC_ERR_LIMIT);
        pc_doc_destroy(big);
    }
    {   /* bytes that are not a usable profile never become an ICCP chunk */
        uint8_t *junk = itu_junk(777u, 5u);
        meta.icc = junk;
        meta.icc_len = 777u;
        CHECK(wp()->save(d, &meta, NULL, NULL, &out) == PC_OK);
        r = load_ok(out.p, out.n, &m);
        CHECK(r && m.icc == NULL);
        pc_doc_destroy(r);
        pc_meta_free(&m);
        pc_buf_free(&out);
        free(junk);
    }
    pc_doc_destroy(d);
    free(a);
    free(icc);
}

static void t_fixtures(void)
{
    /* simple API fixtures: lossy RGB and lossless RGBA */
    const int W = 31, H = 17;
    uint8_t rgba[31 * 17 * 4];
    uint8_t *file = NULL;
    size_t n;
    pc_image_meta m;
    pc_doc *r;
    for (size_t i = 0; i < sizeof rgba; i++) rgba[i] = rnd8();
    n = WebPEncodeLosslessRGBA(rgba, W, H, W * 4, &file);
    CHECK(n > 0);
    r = load_ok(file, n, &m);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        size_t bad = 0;
        for (int i = 0; i < W * H; i++) {
            if (rgba[4 * i + 3] == 0) continue;     /* RGB under alpha 0 may change */
            if (px[i].r != rgba[4 * i] || px[i].g != rgba[4 * i + 1] ||
                px[i].b != rgba[4 * i + 2] || px[i].a != rgba[4 * i + 3]) bad++;
        }
        CHECK(bad == 0);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    WebPFree(file);
    n = WebPEncodeRGB(rgba, W, H, W * 3, 80.0f, &file);
    CHECK(n > 0);
    r = load_ok(file, n, &m);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        CHECK(r->w == (uint32_t)W && px[0].a == 255 && !m.had_alpha);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    WebPFree(file);
}

/* Two-frame animation: frame 1 is a translucent red square on a transparent
 * canvas, frame 2 solid blue. Loading yields frame 1. */
static void make_anim(WebPData *outd, int W, int H)
{
    WebPAnimEncoderOptions ao;
    WebPAnimEncoder *enc;
    WebPConfig cfg;
    WebPPicture pic;
    CHECK(WebPAnimEncoderOptionsInit(&ao));
    enc = WebPAnimEncoderNew(W, H, &ao);
    CHECK(enc != NULL);
    CHECK(WebPConfigInit(&cfg));
    cfg.lossless = 1;
    cfg.exact = 1;
    for (int f = 0; f < 2; f++) {
        CHECK(WebPPictureInit(&pic));
        pic.use_argb = 1; pic.width = W; pic.height = H;
        CHECK(WebPPictureAlloc(&pic));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                pic.argb[y * pic.argb_stride + x] = f == 0
                    ? ((x >= 4 && x < 12 && y >= 2 && y < 9) ? 0x80FF0000u : 0u)
                    : 0xFF0000FFu;
        CHECK(WebPAnimEncoderAdd(enc, &pic, f * 100, &cfg));
        WebPPictureFree(&pic);
    }
    CHECK(WebPAnimEncoderAdd(enc, NULL, 200, NULL));
    WebPDataInit(outd);
    CHECK(WebPAnimEncoderAssemble(enc, outd));
    WebPAnimEncoderDelete(enc);
}

static void t_animated(void)
{
    const int W = 16, H = 12;
    WebPData anim;
    pc_image_meta m;
    pc_doc *r;
    make_anim(&anim, W, H);
    r = load_ok(anim.bytes, anim.size, &m);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        size_t bad = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                bool in = x >= 4 && x < 12 && y >= 2 && y < 9;
                pc_px32 e = in ? tu_px(255, 0, 0, 128) : tu_px(0, 0, 0, 0);
                pc_px32 g = px[y * W + x];
                if (g.a != e.a || (e.a && !tu_px_eq(g, e))) bad++;
            }
        CHECK(bad == 0);
        CHECK(strstr(m.note, "first frame") != NULL);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    {
        uint32_t ok = tu_fuzz_codec(wp(), anim.bytes, anim.size, g_quick ? 1500u : 15000u, NULL);
        INFO("animated webp fuzz: %u decoded", ok);
    }
    WebPDataClear(&anim);
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 1500u : 20000u, ok = 0;
    const uint32_t W = 33, H = 21;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    uint8_t icc[64];
    pc_image_meta meta;
    memset(icc, 7, sizeof icc);
    memset(&meta, 0, sizeof meta);
    for (int k = 0; k < 3; k++) {
        webp_params_t p;
        pc_buf out;
        memset(&out, 0, sizeof out);
        pc_codec_default_params(wp(), &p);
        p.lossless = k == 1;
        meta.icc = k == 2 ? icc : NULL;
        meta.icc_len = k == 2 ? sizeof icc : 0;
        CHECK(wp()->save(d, &meta, &p, NULL, &out) == PC_OK);
        ok += tu_fuzz_codec(wp(), out.p, out.n, iters, NULL);
        pc_buf_free(&out);
    }
    INFO("webp fuzz: %u of %u mutated files decoded", ok, 3u * iters);
    pc_doc_destroy(d);
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_sniff_params);
    RUN(t_lossless);
    RUN(t_lossy);
    RUN(t_icc_and_limits);
    RUN(t_fixtures);
    RUN(t_animated);
    RUN(t_fuzz);
    return pc_test_finish();
}
