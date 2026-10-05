/* test_lib_resample.c - private helpers of the library codecs
 * (src/codec/lib_codec.h, white box): the streaming resampler used for DDS
 * mipmaps and ORA thumbnails, over-white compositing, UTF-8 truncation,
 * limits in lc_doc_new and lc_alloc. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/lib_codec.h"

static void t_constant_and_identity(void)
{
    pc_surf s;
    pc_px32 out[40 * 30];
    CHECK(pc_surf_alloc(&s, 37, 23) == PC_OK);
    for (int32_t i = 0; i < 37 * 23; i++) s.px[i] = tu_px(200, 100, 50, 180);
    for (int f = 0; f < LC_FILTER_COUNT; f++)
        for (int g = 0; g < 2; g++) {
            static const int32_t sizes[][2] = { { 10, 7 }, { 37, 23 }, { 40, 30 }, { 1, 1 } };
            for (size_t k = 0; k < 4; k++) {
                int32_t dw = sizes[k][0], dh = sizes[k][1];
                int bad = 0;
                CHECK(lc_resample(37, 23, lc_src_surf, &s, out, dw, dh, (size_t)dw, (lc_filter)f,
                                  g != 0) == PC_OK);
                for (int32_t i = 0; i < dw * dh; i++)
                    if (abs((int)out[i].r - 200) > 1 || abs((int)out[i].g - 100) > 1 ||
                        abs((int)out[i].b - 50) > 1 || abs((int)out[i].a - 180) > 1) bad++;
                CHECK(bad == 0);
            }
        }
    /* identity size with nearest is exact on arbitrary content */
    for (int32_t i = 0; i < 37 * 23; i++) s.px[i] = tu_px(rnd8(), rnd8(), rnd8(), 255);
    CHECK(lc_resample(37, 23, lc_src_surf, &s, out, 37, 23, 37, LC_FILTER_NEAREST, false) == PC_OK);
    CHECK(tu_diff(out, s.px, 37 * 23) == 0);
    /* 2:1 Fant is the 2x2 average */
    CHECK(lc_resample(37, 23, lc_src_surf, &s, out, 18, 11, 18, LC_FILTER_FANT, false) == PC_OK);
    {
        int bad = 0;
        for (int32_t y = 0; y < 11; y++)
            for (int32_t x = 0; x < 18; x++) {
                double sx = 37.0 / 18.0, sy = 23.0 / 11.0, acc = 0, wsum = 0;
                for (int32_t yy = 0; yy < 23; yy++)
                    for (int32_t xx = 0; xx < 37; xx++) {
                        double ox = fmin(xx + 1.0, (x + 1) * sx) - fmax((double)xx, x * sx);
                        double oy = fmin(yy + 1.0, (y + 1) * sy) - fmax((double)yy, y * sy);
                        if (ox <= 0 || oy <= 0) continue;
                        acc += ox * oy * s.px[yy * 37 + xx].g;
                        wsum += ox * oy;
                    }
                if (abs((int)out[y * 18 + x].g - (int)(acc / wsum + 0.5)) > 1) bad++;
            }
        CHECK(bad == 0);
    }
    /* transparent pixels do not bleed color (premultiplied filtering) */
    for (int32_t i = 0; i < 37 * 23; i++)
        s.px[i] = (i % 2) ? tu_px(255, 0, 0, 255) : tu_px(0, 255, 0, 0);
    CHECK(lc_resample(37, 23, lc_src_surf, &s, out, 9, 5, 9, LC_FILTER_BICUBIC, false) == PC_OK);
    for (int32_t i = 0; i < 9 * 5; i++) CHECK(out[i].g == 0 && (out[i].a == 0 || out[i].r == 255));
    CHECK(lc_resample(0, 23, lc_src_surf, &s, out, 9, 5, 9, LC_FILTER_FANT, false) == PC_ERR_ARG);
    CHECK(lc_resample(37, 23, lc_src_surf, &s, out, 9, 5, 8, LC_FILTER_FANT, false) == PC_ERR_ARG);
    pc_surf_free(&s);
}

static pc_status counting_src(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    int32_t *next = (int32_t *)ud;
    CHECK(y0 == *next);                 /* rows are pulled once, in order */
    *next = y0 + n;
    for (int32_t i = 0; i < 1000 * n; i++) dst[i] = tu_px((uint8_t)(y0 + i / 1000), 0, 0, 255);
    return PC_OK;
}

static void t_streaming(void)
{
    /* a tall image thumbnails without a full copy; rows come in order */
    pc_px32 out[64 * 256];
    int32_t next = 0;
    CHECK(lc_resample(1000, 5000, counting_src, &next, out, 64, 256, 64, LC_FILTER_LANCZOS,
                      true) == PC_OK);
    CHECK(next == 5000);
}

static void t_misc(void)
{
    char buf[8];
    pc_px32 p[3];
    pc_codec_limits lim;
    pc_doc *d = (pc_doc *)1;
    pc_layer *l = (pc_layer *)1;
    pc_status st = PC_OK;
    lc_utf8_copy(buf, 7, "abc\xc3\xa9\xc3\xa9", 7);    /* 6 bytes fit: cuts inside a char */
    CHECK(strcmp(buf, "abc\xc3\xa9") == 0);
    lc_utf8_copy(buf, sizeof buf, "ab\0cd", 5);
    CHECK(strcmp(buf, "ab") == 0);
    p[0] = tu_px(10, 20, 30, 0);
    p[1] = tu_px(10, 20, 30, 255);
    p[2] = tu_px(0, 0, 0, 128);
    lc_over_white(p, 3);
    CHECK(tu_px_eq(p[0], tu_px(255, 255, 255, 255)) && tu_px_eq(p[1], tu_px(10, 20, 30, 255)));
    CHECK(p[2].a == 255 && p[2].r == 127);
    pc_codec_limits_default(&lim);
    lim.max_layers = 2;
    CHECK(lc_doc_new(&lim, 10, 10, 3, &d, &l) == PC_ERR_LIMIT && d == NULL);
    CHECK(lc_doc_new(NULL, 0, 10, 1, &d, &l) == PC_ERR_FORMAT && d == NULL);
    CHECK(lc_doc_new(NULL, 5, 6, 1, &d, &l) == PC_OK && d && l == d->stack[0]);
    CHECK(strcmp(l->name, "Background") == 0);
    pc_doc_destroy(d);
    lim.max_mem = 100;
    CHECK(lc_alloc(101, 1, &lim, &st) == NULL && st == PC_ERR_LIMIT);
    CHECK(lc_alloc(SIZE_MAX / 2u, 4, NULL, &st) == NULL && st == PC_ERR_LIMIT);
    {
        void *q = lc_alloc(100, 1, &lim, &st);
        CHECK(q != NULL && st == PC_OK);
        free(q);
    }
    CHECK(lc_u16_to_u8(65535) == 255 && lc_u16_to_u8(0) == 0 && lc_u16_to_u8(32896) == 128);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_constant_and_identity);
    RUN(t_streaming);
    RUN(t_misc);
    return pc_test_finish();
}
