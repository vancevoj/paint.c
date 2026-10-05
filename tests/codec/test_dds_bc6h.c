/* test_dds_bc6h.c - lane CODEC: the BC6H (unsigned) block encoder
 * (bc6h_enc.c) checked against the bcdec decoder: every mode and every
 * partition decodes to exactly the values the encoder predicts, half float
 * conversions are exact, and encoded LDR images stay close to the source. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/bc6h_enc.h"
#include "bcdec.h"

#include <math.h>

static void t_half(void)
{
    /* every 8-bit level and a few powers of two round-trip exactly */
    for (int v = 0; v < 256; v++) {
        float f = (float)v / 255.0f;
        uint16_t h = bc6h_float_to_half(f);
        float back = bc6h_half_to_float(h);
        CHECK(fabsf(back - f) <= f * (1.0f / 1024.0f) + 1e-7f);
        CHECK((int)floorf(back * 255.0f + 0.5f) == v);
    }
    CHECK(bc6h_float_to_half(1.0f) == 0x3C00u && bc6h_float_to_half(0.5f) == 0x3800u);
    CHECK(bc6h_float_to_half(65504.0f) == 0x7BFFu && bc6h_float_to_half(1e9f) == 0x7BFFu);
    CHECK(bc6h_float_to_half(-1.0f) == 0u && bc6h_float_to_half(0.0f) == 0u);
    CHECK(bc6h_float_to_half(5.96046448e-8f) == 1u);                     /* smallest subnormal */
    CHECK(bc6h_half_to_float(0x0001u) == 5.96046448e-8f && bc6h_half_to_float(0x3C00u) == 1.0f);
}

static void random_block(uint16_t *rgb, int kind)
{
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            float f;
            if (kind == 0) f = (float)rnd8() / 255.0f;                  /* LDR noise */
            else if (kind == 1) f = (float)(rnd() % 100000u) / 1000.0f;  /* HDR 0..100 */
            else f = (float)((i % 4) * 40 + (i / 4) * 20 + c * 7) / 255.0f;  /* gradient */
            rgb[3 * i + c] = bc6h_float_to_half(f);
        }
}

/* Each mode and partition: bcdec decodes exactly what the encoder predicts. */
static void t_modes_exact(void)
{
    int reps = g_quick ? 3 : 20;
    for (int mode = 0; mode < 14; mode++)
        for (int part = 0; part < (mode < 10 ? 32 : 1); part++)
            for (int r = 0; r < reps; r++) {
                uint16_t rgb[48], want[48], got[48];
                uint8_t blk[16];
                bool same = true;
                random_block(rgb, r % 3);
                bc6h_encode_forced(blk, rgb, mode, part, want);
                bcdec_bc6h_half(blk, got, 4 * 3, 0);
                for (int i = 0; i < 16 && same; i++)
                    for (int c = 0; c < 3; c++) same = same && got[i * 3 + c] == want[i * 3 + c];
                CHECK(same);
                if (!same) INFO("mode %d partition %d differs", mode + 1, part);
            }
}

/* Whole-image quality on an LDR photo for each effort. */
static void t_quality(void)
{
    const uint32_t W = g_quick ? 64 : 256, H = g_quick ? 64 : 256;
    pc_px32 *px = tu_photo(W, H, false);
    double last = 0;
    for (int effort = 0; effort < 3; effort++) {
        double se = 0, t0 = pc_test_now();
        int worst = 0;
        for (uint32_t by = 0; by < H; by += 4)
            for (uint32_t bx = 0; bx < W; bx += 4) {
                uint16_t rgb[48], dec[48];
                uint8_t blk[16];
                for (int i = 0; i < 16; i++) {
                    pc_px32 p = px[(by + (uint32_t)i / 4u) * W + bx + (uint32_t)i % 4u];
                    rgb[3 * i] = bc6h_float_to_half(p.r / 255.0f);
                    rgb[3 * i + 1] = bc6h_float_to_half(p.g / 255.0f);
                    rgb[3 * i + 2] = bc6h_float_to_half(p.b / 255.0f);
                }
                bc6h_encode_block(blk, rgb, effort);
                bcdec_bc6h_half(blk, dec, 4 * 3, 0);
                for (int i = 0; i < 16; i++) {
                    pc_px32 p = px[(by + (uint32_t)i / 4u) * W + bx + (uint32_t)i % 4u];
                    int src[3] = { p.r, p.g, p.b };
                    for (int c = 0; c < 3; c++) {
                        float f = bc6h_half_to_float(dec[i * 3 + c]);
                        int v = (int)floorf((f > 1.0f ? 1.0f : f) * 255.0f + 0.5f);
                        int d = abs(v - src[c]);
                        se += (double)d * d;
                        if (d > worst) worst = d;
                    }
                }
            }
        {
            double psnr = 10.0 * log10(255.0 * 255.0 * 3.0 * W * H / (se > 0 ? se : 1e-9));
            INFO("BC6H effort %d: PSNR %.2f dB, worst channel error %d, %.1f us per block",
                 effort, psnr, worst, (pc_test_now() - t0) * 1e6 / ((double)W * H / 16.0));
            CHECK(psnr > (effort == 0 ? 38.0 : 39.5));
            CHECK(psnr >= last - 0.01);                  /* more effort never hurts */
            last = psnr;
        }
    }
    free(px);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_half);
    RUN(t_modes_exact);
    RUN(t_quality);
    return pc_test_finish();
}
