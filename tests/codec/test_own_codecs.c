/* test_own_codecs.c - lane L6a codecs as a group: registry entries, save
 * option schemas and defaults, sniffing, and decoding of the fixtures in
 * data/own (written by Pillow and ImageMagick, see gen_fixtures.py). */
#include "test_own_common.h"
#include "cmyk_ref.h"

extern const pc_codec pc_codec_bmp, pc_codec_gif, pc_codec_tga, pc_codec_tiff;

static const pc_codec *const k_own[] = { &pc_codec_bmp, &pc_codec_gif, &pc_codec_tga,
                                         &pc_codec_tiff };
#define N_OWN 4

static void t_registry(void)
{
    static const char *const exts[][2] = {
        { "bmp", "bmp" }, { "BMP", "bmp" }, { "dib", "bmp" }, { ".rle", "bmp" }, { "gif", "gif" },
        { "GIF", "gif" }, { "tga", "tga" }, { "tif", "tiff" }, { "TIFF", "tiff" },
        { "tiff", "tiff" },
    };
    for (int i = 0; i < N_OWN; i++) {
        const pc_codec *c = k_own[i];
        CHECK(pc_codec_by_id(c->id) == c);
        CHECK((c->flags & (PC_CODEC_LOAD | PC_CODEC_SAVE)) == (PC_CODEC_LOAD | PC_CODEC_SAVE));
        CHECK(!(c->flags & PC_CODEC_LAYERED));
        CHECK(c->sniff && c->load && c->save && c->props && c->n_props > 0u);
        CHECK(pc_codec_by_ext(c->exts) == c || strchr(c->exts, ';') != NULL);
    }
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        const pc_codec *c = pc_codec_by_ext(exts[i][0]);
        CHECK(c != NULL && strcmp(c->id, exts[i][1]) == 0);
    }
    CHECK(strncmp(pc_codec_tiff.exts, "tif;", 4) == 0);
    CHECK(strncmp(pc_codec_bmp.exts, "bmp;", 4) == 0);
}

static void t_props(void)
{
    for (int i = 0; i < N_OWN; i++) {
        const pc_codec *c = k_own[i];
        uint8_t params[64];
        CHECK(c->params_size <= sizeof params);
        for (uint32_t k = 0; k < c->n_props; k++) {
            const fx_prop *p = &c->props[k];
            CHECK(p->key && p->label && p->offset + 4u <= c->params_size);
            CHECK(p->kind == FXP_INT || p->kind == FXP_BOOL || p->kind == FXP_CHOICE);
            CHECK(p->def >= p->min && p->def <= p->max);
            for (uint32_t j = 0; j < k; j++) CHECK(strcmp(c->props[j].key, p->key) != 0);
            if (p->kind == FXP_CHOICE) {
                uint32_t n = 0;
                while (p->choices && p->choices[n]) n++;
                CHECK(n == (uint32_t)p->max + 1u && p->min == 0.0);
            }
        }
        pc_codec_default_params(c, params);
        for (uint32_t k = 0; k < c->n_props; k++) {
            int32_t v;
            memcpy(&v, params + c->props[k].offset, 4);
            CHECK(v == (int32_t)c->props[k].def);
        }
    }
    {   /* the Paint.NET defaults */
        int32_t b[3], g[3], t[2], f[4];
        pc_codec_default_params(&pc_codec_bmp, b);
        pc_codec_default_params(&pc_codec_gif, g);
        pc_codec_default_params(&pc_codec_tga, t);
        pc_codec_default_params(&pc_codec_tiff, f);
        CHECK(b[0] == 0 && b[1] == 7 && b[2] == 0);       /* Auto-detect, dither 7, Octree */
        CHECK(g[0] == 7 && g[1] == 128 && g[2] == 0);     /* dither 7, threshold 128 */
        CHECK(t[0] == 0 && t[1] == 1);                    /* Auto-detect, RLE on */
        CHECK(f[0] == 0 && f[1] == 0 && f[2] == 7 && f[3] == 0);
    }
}

static void t_sniff(void)
{
    pc_doc *d = doc_pat(pat_rgba, 9, 5);
    pc_buf out[N_OWN];
    for (int i = 0; i < N_OWN; i++) CHECK(codec_save(k_own[i], d, NULL, NULL, &out[i]) == PC_OK);
    for (int i = 0; i < N_OWN; i++) {
        for (int j = 0; j < N_OWN; j++)
            CHECK(k_own[i]->sniff(out[j].p, out[j].n) == (i == j));
        CHECK(pc_codec_sniff(out[i].p, out[i].n) == k_own[i]);
        CHECK(!k_own[i]->sniff(out[i].p, 0));
        CHECK(!k_own[i]->sniff((const uint8_t *)"\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
        CHECK(!k_own[i]->sniff((const uint8_t *)"RIFF\0\0\0\0WEBPVP8 ", 16));
        CHECK(!k_own[i]->sniff((const uint8_t *)"\xff\xd8\xff\xe0\0\x10JFIF\0\1\1\0\0\1\0\1", 18));
    }
    /* random data rarely looks like anything; TGA has no magic, the others must reject */
    for (int it = 0; it < 2000; it++) {
        uint8_t b[32];
        for (int k = 0; k < 32; k++) b[k] = rnd8();
        CHECK(!pc_codec_bmp.sniff(b, 32) || (b[0] == 'B' && b[1] == 'M'));
        CHECK(!pc_codec_gif.sniff(b, 32));
        CHECK(!pc_codec_tiff.sniff(b, 32) || b[0] == 'I' || b[0] == 'M');
    }
    for (int i = 0; i < N_OWN; i++) pc_buf_free(&out[i]);
    pc_doc_destroy(d);
}

/* ---- fixtures ------------------------------------------------------------------
 * Tolerances: ImageMagick truncates to 5 and 4-bit channels (8 and 17), its
 * 16-bit TGA writer loses up to 16 (our decode matches Pillow within 1).
 * tif_im_miniswhite.tif holds plain gray values tagged min-is-white, so the
 * spec-conforming result is the inverted ramp. */
enum { E_RGBA, E_RGB, E_GRAY, E_GRAY_INV, E_BW, E_FEW, E_FEWT, E_FEW_OFF, E_RGBA_CW, E_NONE,
       E_CMYK8 };   /* E_CMYK8: 8-bit CMYK samples stored uncompressed at file offset 8 */

static pc_px32 expect_px(int kind, uint32_t x, uint32_t y)
{
    switch (kind) {
    case E_RGBA: return pat_rgba(x, y);
    case E_RGB:  return pat_rgb(x, y);
    case E_GRAY: return pat_gray(x, y);
    case E_GRAY_INV: {
        pc_px32 p = pat_gray(x, y);
        return mkpx(255u - p.r, 255u - p.g, 255u - p.b, 255u);
    }
    case E_BW:   return pat_bw(x, y);
    case E_FEW:  return pat_few(x, y);
    case E_FEWT: return (x + y) % 5u == 0u ? mkpx(0, 0, 0, 0) : pat_few(x, y);
    case E_FEW_OFF:
        if (x < 5u || y < 3u || x >= 5u + 19u || y >= 3u + 13u) return mkpx(0, 0, 0, 0);
        return pat_few(x - 5u, y - 3u);
    default:     return pat_rgba(y, 12u - x);       /* orientation 6 of a 19 x 13 image */
    }
}

typedef struct fixture {
    const char *file, *codec;
    pc_status   status;
    int         kind;
    uint32_t    w, h, tol;
    const char *note;
    bool        alpha;
} fixture;

static const fixture k_fix[] = {
    { "bmp_pil_rgb24.bmp", "bmp", PC_OK, E_RGB, 19, 13, 0, "", false },
    { "bmp_pil_rgba32.bmp", "bmp", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "bmp_pil_p8.bmp", "bmp", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "bmp_pil_1bit.bmp", "bmp", PC_OK, E_BW, 19, 13, 0, "", false },
    { "bmp_pil_gray.bmp", "bmp", PC_OK, E_GRAY, 19, 13, 0, "", false },
    { "bmp_im_rle8.bmp", "bmp", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "bmp_im_bmp3.bmp", "bmp", PC_OK, E_RGB, 19, 13, 0, "", false },
    { "bmp_im_v5.bmp", "bmp", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "bmp_im_565.bmp", "bmp", PC_OK, E_RGB, 19, 13, 8, "", false },
    { "bmp_im_555.bmp", "bmp", PC_OK, E_RGB, 19, 13, 8, "", false },
    { "bmp_im_4444.bmp", "bmp", PC_OK, E_RGBA, 19, 13, 17, "", true },
    { "tga_pil_rgba.tga", "tga", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tga_pil_rgba_rle.tga", "tga", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tga_pil_rgb.tga", "tga", PC_OK, E_RGB, 19, 13, 0, "", false },
    { "tga_pil_gray.tga", "tga", PC_OK, E_GRAY, 19, 13, 0, "", false },
    { "tga_pil_p.tga", "tga", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "tga_pil_p_rle.tga", "tga", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "tga_im_rle.tga", "tga", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tga_im_16.tga", "tga", PC_OK, E_RGB, 19, 13, 16, "", false },
    { "gif_pil_p.gif", "gif", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "gif_pil_interlace.gif", "gif", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "gif_im_interlace.gif", "gif", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "gif_im_trans.gif", "gif", PC_OK, E_FEWT, 19, 13, 0, "", true },
    { "gif_im_offset.gif", "gif", PC_OK, E_FEW_OFF, 30, 20, 0, "", true },
    { "gif_im_anim.gif", "gif", PC_OK, E_FEW, 19, 13, 0, "first of 2 frames", false },
    { "tif_pil_rgba_none.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_pil_rgba_lzw.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_pil_rgba_adobe.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_pil_rgba_packbits.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_pil_rgba_deflate.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_pil_rgb_none.tif", "tiff", PC_OK, E_RGB, 19, 13, 0, "", false },
    { "tif_pil_rgb_lzw.tif", "tiff", PC_OK, E_RGB, 19, 13, 0, "", false },
    { "tif_pil_gray.tif", "tiff", PC_OK, E_GRAY, 19, 13, 0, "", false },
    { "tif_pil_p.tif", "tiff", PC_OK, E_FEW, 19, 13, 0, "", false },
    { "tif_pil_1bit.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_pil_g4.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_pil_ccitt_rle.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_pil_g3_1d.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_pil_g3_2d.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_im_g4_lsb.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_im_fax.tif", "tiff", PC_OK, E_BW, 19, 13, 0, "", false },
    { "tif_pil_jpeg.tif", "tiff", PC_ERR_UNSUPPORTED, E_NONE, 0, 0, 0, "", false },
    { "tif_im_tiled.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_planar.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_be.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_be_lzw_pred.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_16_zip_pred.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_16_be_tiled.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    /* no embedded profile: the default CMYK profile (SWOP) to Adobe RGB
     * (1998), exactly (FL-CMYK, wave 4) */
    { "tif_im_cmyk.tif", "tiff", PC_OK, E_CMYK8, 19, 13, 0, "default CMYK profile", false },
    { "tif_im_miniswhite.tif", "tiff", PC_OK, E_GRAY_INV, 19, 13, 0, "", false },
    { "tif_im_orient6.tif", "tiff", PC_OK, E_RGBA_CW, 13, 19, 0, "", true },
    { "tif_im_packbits.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_strips_lzw.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_float.tif", "tiff", PC_OK, E_RGBA, 19, 13, 1, "", true },
    { "tif_im_half_pred3.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "", true },
    { "tif_im_float_pred3_tiled.tif", "tiff", PC_OK, E_RGBA, 19, 13, 1, "", true },
    { "tif_im_double_pred2.tif", "tiff", PC_OK, E_RGBA, 19, 13, 1, "", true },
    { "tif_im_multipage.tif", "tiff", PC_OK, E_RGBA, 19, 13, 0, "first of 2 pages", true },
};

static void t_fixtures(void)
{
    uint32_t found = 0;
    for (size_t i = 0; i < sizeof k_fix / sizeof k_fix[0]; i++) {
        const fixture *f = &k_fix[i];
        const pc_codec *c = pc_codec_by_id(f->codec), *used = NULL;
        size_t n;
        uint8_t *b = read_fixture(f->file, &n);
        pc_doc *d = NULL;
        pc_image_meta m;
        pc_status st;
        if (!b) { INFO("missing fixture %s", f->file); CHECK(b != NULL); continue; }
        found++;
        CHECK(c->sniff(b, n));
        st = pc_codec_load_any(b, n, f->file, NULL, &d, &m, &used);
        if (st != f->status) INFO("%s: status %d, expected %d", f->file, (int)st, (int)f->status);
        CHECK(st == f->status);
        if (st == PC_OK) {
            pc_px32 *got = doc_layer0(d), *ref;
            uint32_t diff;
            CHECK(used == c);
            CHECK(d->w == f->w && d->h == f->h);
            if (d->w == f->w && d->h == f->h) {
                ref = (pc_px32 *)malloc((size_t)f->w * f->h * sizeof *ref);
                if (f->kind == E_CMYK8) {
                    CHECK(n >= 8u + (size_t)f->w * f->h * 4u);
                    CHECK(n >= 8u + (size_t)f->w * f->h * 4u &&
                          cmyk_ref(b + 8, (size_t)f->w * f->h, false, ref));
                    CHECK(cmyk_ref_is_adobe(&m));
                } else {
                    for (uint32_t y = 0; y < f->h; y++)
                        for (uint32_t x = 0; x < f->w; x++)
                            ref[(size_t)y * f->w + x] = expect_px(f->kind, x, y);
                }
                diff = px_maxdiff(got, ref, (size_t)f->w * f->h);
                if (diff > f->tol) INFO("%s: max difference %u > %u", f->file, diff, f->tol);
                CHECK(diff <= f->tol);
                free(ref);
            }
            CHECK(strstr(m.note, f->note) != NULL);
            CHECK(m.had_alpha == f->alpha);
            free(got);
            pc_meta_free(&m);
            pc_doc_destroy(d);
        }
        free(b);
    }
    CHECK(found == sizeof k_fix / sizeof k_fix[0]);
}

static void t_fixtures_fuzz(void)
{
    /* the third-party fixtures are good seeds for every decoder */
    for (int ci = 0; ci < N_OWN; ci++) {
        seedset s;
        memset(&s, 0, sizeof s);
        for (size_t i = 0; i < sizeof k_fix / sizeof k_fix[0]; i++) {
            size_t n;
            uint8_t *b;
            if (strcmp(k_fix[i].codec, k_own[ci]->id) != 0) continue;
            b = read_fixture(k_fix[i].file, &n);
            if (b) seeds_add(&s, b, n);
            free(b);
        }
        fuzz_seeds(k_own[ci], &s, g_quick ? 1500u : 20000u);
        seeds_free(&s);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_registry);
    RUN(t_props);
    RUN(t_sniff);
    RUN(t_fixtures);
    RUN(t_fixtures_fuzz);
    return pc_test_finish();
}
