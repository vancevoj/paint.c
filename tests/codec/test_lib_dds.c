/* test_lib_dds.c - DDS codec: hand-built fixtures for every decoder layout
 * (legacy bit masks, DXGI formats, all BC formats checked block by block
 * against bcdec), header validation, every save format round trip with
 * format-specific tolerances, mipmap chains, dithering, fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"

#include "bcdec.h"

static const pc_codec *dds(void) { return pc_codec_by_id("dds"); }

typedef struct dds_params_t {
    int32_t format, dither, bc7_speed, metric, cube_map, mipmaps, mip_filter, gamma;
} dds_params_t;

enum {   /* save format indices (k_formats order in fmt_dds.c) */
    F_BC1 = 0, F_BC1S, F_BC2, F_BC2S, F_BC3, F_BC3S, F_BC4, F_BC5U, F_BC5S, F_BC7, F_BC7S,
    F_BGRA, F_BGRAS, F_BGRX, F_BGRXS, F_RGBA, F_RGBAS, F_5551, F_4444, F_565, F_R8, F_RG8,
    F_RG8S, F_R32F, F_BGR8, F_RGBX, F_ATI1, F_ATI2, F_RXGB, F_COUNT
};

/* ---- fixture builder -------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t cc(const char *s)
{
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}

typedef struct hdr_spec {
    uint32_t w, h, mips, pf_flags, fourcc, bits, m[4], caps2;
    bool     dx10;
    uint32_t dxgi, dim, misc, array;
} hdr_spec;

static void build_hdr(pc_buf *b, const hdr_spec *s)
{
    uint8_t h[148];
    memset(h, 0, sizeof h);
    put32(h, 0x20534444u);
    put32(h + 4, 124);
    put32(h + 8, 0x1007u);
    put32(h + 12, s->h);
    put32(h + 16, s->w);
    put32(h + 28, s->mips);
    put32(h + 76, 32);
    put32(h + 80, s->dx10 ? 4u : s->pf_flags);
    put32(h + 84, s->dx10 ? cc("DX10") : s->fourcc);
    put32(h + 88, s->bits);
    for (int i = 0; i < 4; i++) put32(h + 92 + 4 * i, s->m[i]);
    put32(h + 108, 0x1000u);
    put32(h + 112, s->caps2);
    if (s->dx10) {
        put32(h + 128, s->dxgi);
        put32(h + 132, s->dim ? s->dim : 3u);
        put32(h + 136, s->misc);
        put32(h + 140, s->array ? s->array : 1u);
    }
    pc_buf_append(b, h, s->dx10 ? 148u : 128u);
}

static pc_doc *load_ok(const pc_buf *b, pc_image_meta *m)
{
    pc_doc *d = NULL;
    pc_status st = dds()->load(b->p, b->n, NULL, &d, m);
    CHECK(st == PC_OK);
    if (st != PC_OK) INFO("dds load: %s", pc_status_str(st));
    return d;
}

static uint8_t sc(uint32_t q, uint32_t bits)
{
    uint64_t max = bits >= 32 ? 0xFFFFFFFFu : (((uint64_t)1 << bits) - 1u);
    return bits ? (uint8_t)(((uint64_t)q * 255u + max / 2u) / max) : 0u;
}

static uint32_t popc(uint32_t v) { uint32_t c = 0; for (; v; v &= v - 1u) c++; return c; }
static uint32_t lowbit(uint32_t v)
{
    uint32_t s = 0;
    if (!v) return 0;
    while (!(v & 1u)) { v >>= 1; s++; }
    return s;
}

/* Legacy bit-mask layouts: random words, expected colors from the masks. */
static void t_masks(void)
{
    static const struct { uint32_t flags, bits, m[4]; bool gray; } L[] = {
        { 0x41, 32, { 0xFF0000, 0xFF00, 0xFF, 0xFF000000u }, false },   /* A8R8G8B8 */
        { 0x40, 32, { 0xFF0000, 0xFF00, 0xFF, 0 }, false },             /* X8R8G8B8 */
        { 0x41, 32, { 0xFF, 0xFF00, 0xFF0000, 0xFF000000u }, false },   /* A8B8G8R8 */
        { 0x40, 32, { 0xFF, 0xFF00, 0xFF0000, 0 }, false },             /* X8B8G8R8 */
        { 0x40, 16, { 0xF800, 0x7E0, 0x1F, 0 }, false },                /* R5G6B5 */
        { 0x41, 16, { 0x7C00, 0x3E0, 0x1F, 0x8000 }, false },           /* A1R5G5B5 */
        { 0x40, 16, { 0x7C00, 0x3E0, 0x1F, 0 }, false },                /* X1R5G5B5 */
        { 0x41, 16, { 0xF00, 0xF0, 0xF, 0xF000 }, false },              /* A4R4G4B4 */
        { 0x40, 24, { 0xFF0000, 0xFF00, 0xFF, 0 }, false },             /* R8G8B8 */
        { 0x41, 32, { 0x3FF00000u, 0xFFC00, 0x3FF, 0xC0000000u }, false }, /* A2R10G10B10 */
        { 0x40, 32, { 0xFFFF, 0xFFFF0000u, 0, 0 }, false },             /* G16R16 */
        { 0x40, 8, { 0xE0, 0x1C, 0x3, 0 }, false },                     /* R3G3B2 */
        { 0x20000, 8, { 0xFF, 0, 0, 0 }, true },                        /* L8 */
        { 0x20001, 16, { 0xFF, 0, 0, 0xFF00 }, true },                  /* A8L8 */
        { 0x20000, 16, { 0xFFFF, 0, 0, 0 }, true },                     /* L16 */
        { 0x2, 8, { 0, 0, 0, 0xFF }, false },                           /* A8 */
    };
    const uint32_t W = 13, H = 70;
    for (size_t k = 0; k < sizeof L / sizeof L[0]; k++) {
        hdr_spec s;
        pc_buf b;
        pc_image_meta m;
        pc_doc *d;
        uint32_t bytes = L[k].bits / 8u;
        pc_px32 *exp = (pc_px32 *)malloc((size_t)W * H * sizeof *exp);
        memset(&s, 0, sizeof s);
        memset(&b, 0, sizeof b);
        s.w = W; s.h = H; s.mips = 1; s.pf_flags = L[k].flags; s.bits = L[k].bits;
        memcpy(s.m, L[k].m, sizeof s.m);
        build_hdr(&b, &s);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            uint32_t v = (uint32_t)rnd();
            uint8_t o[4];
            if (bytes < 4) v &= (1u << (8 * bytes)) - 1u;
            for (uint32_t j = 0; j < bytes; j++) o[j] = (uint8_t)(v >> (8 * j));
            pc_buf_append(&b, o, bytes);
            {
                pc_px32 e;
                uint32_t mr = L[k].m[0], mg = L[k].m[1], mb = L[k].m[2], ma = L[k].m[3];
                bool alpha_only = L[k].flags == 0x2;
                e.r = alpha_only ? 0 : sc((v & mr) >> lowbit(mr), popc(mr));
                e.g = (alpha_only || L[k].gray) ? e.r : sc((v & mg) >> lowbit(mg), popc(mg));
                e.b = (alpha_only || L[k].gray) ? e.r : sc((v & mb) >> lowbit(mb), popc(mb));
                if (alpha_only) e.g = e.b = 0;
                e.a = ma ? sc((v & ma) >> lowbit(ma), popc(ma)) : 255u;
                exp[i] = e;
            }
        }
        d = load_ok(&b, &m);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            size_t bad = tu_diff(px, exp, (size_t)W * H);
            CHECK(bad == 0);
            if (bad) INFO("mask layout %zu: %zu bad", k, bad);
            CHECK(m.had_alpha == (L[k].m[3] != 0));
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        free(exp);
        pc_buf_free(&b);
    }
}

static uint8_t snorm_u8(int v)
{
    if (v < -127) v = -127;
    return (uint8_t)(((v + 127) * 255 + 127) / 254);
}

static float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15), e = (uint32_t)(h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = (float)m / 16777216.0f;
    else if (e == 31) v = m ? 0.0f : 131008.0f;
    else v = ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

static uint8_t f2u(float f)
{
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return (uint8_t)(f * 255.0f + 0.5f);
}

/* DXGI uncompressed formats. */
static void t_dxgi_plain(void)
{
    static const struct { uint32_t dxgi, bpp; } F[] = {
        { 28, 4 }, { 29, 4 }, { 87, 4 }, { 88, 4 }, { 24, 4 }, { 85, 2 }, { 86, 2 }, { 115, 2 },
        { 61, 1 }, { 65, 1 }, { 49, 2 }, { 11, 8 }, { 10, 8 }, { 2, 16 }, { 41, 4 }, { 54, 2 },
        { 31, 4 }, { 51, 2 }, { 56, 2 }, { 35, 4 },
    };
    const uint32_t W = 9, H = 5;
    for (size_t k = 0; k < sizeof F / sizeof F[0]; k++) {
        hdr_spec s;
        pc_buf b;
        pc_image_meta m;
        pc_doc *d;
        pc_px32 exp[45];
        memset(&s, 0, sizeof s);
        memset(&b, 0, sizeof b);
        s.w = W; s.h = H; s.mips = 1; s.dx10 = true; s.dxgi = F[k].dxgi;
        build_hdr(&b, &s);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            uint8_t o[16];
            pc_px32 e = tu_px(0, 0, 0, 255);
            for (uint32_t j = 0; j < F[k].bpp; j++) o[j] = rnd8();
            switch (F[k].dxgi) {
            case 28: case 29: e = tu_px(o[0], o[1], o[2], o[3]); break;
            case 87: e = tu_px(o[2], o[1], o[0], o[3]); break;
            case 88: e = tu_px(o[2], o[1], o[0], 255); break;
            case 24: {
                uint32_t v = (uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) |
                             ((uint32_t)o[3] << 24);
                e = tu_px(sc(v & 0x3FF, 10), sc((v >> 10) & 0x3FF, 10), sc((v >> 20) & 0x3FF, 10),
                          sc(v >> 30, 2));
                break;
            }
            case 85: { uint32_t v = (uint32_t)o[0] | ((uint32_t)o[1] << 8);
                e = tu_px(sc(v >> 11, 5), sc((v >> 5) & 63, 6), sc(v & 31, 5), 255); break; }
            case 86: { uint32_t v = (uint32_t)o[0] | ((uint32_t)o[1] << 8);
                e = tu_px(sc((v >> 10) & 31, 5), sc((v >> 5) & 31, 5), sc(v & 31, 5),
                          sc(v >> 15, 1));
                break; }
            case 115: { uint32_t v = (uint32_t)o[0] | ((uint32_t)o[1] << 8);
                e = tu_px(sc((v >> 8) & 15, 4), sc((v >> 4) & 15, 4), sc(v & 15, 4),
                          sc(v >> 12, 4));
                break; }
            case 61: e = tu_px(o[0], o[0], o[0], 255); break;
            case 65: e = tu_px(0, 0, 0, o[0]); break;
            case 49: e = tu_px(o[0], o[1], 0, 255); break;
            case 11: {
                uint16_t c[4];
                for (int j = 0; j < 4; j++) c[j] = (uint16_t)(o[2 * j] | (o[2 * j + 1] << 8));
                e = tu_px(sc(c[0], 16), sc(c[1], 16), sc(c[2], 16), sc(c[3], 16));
                break;
            }
            case 10: {
                uint16_t c[4];
                for (int j = 0; j < 4; j++) {
                    /* keep values in [0, 2] so clamping is exercised but NaN avoided */
                    o[2 * j + 1] = (uint8_t)(o[2 * j + 1] & 0x3F);
                    c[j] = (uint16_t)(o[2 * j] | (o[2 * j + 1] << 8));
                }
                e = tu_px(f2u(h2f(c[0])), f2u(h2f(c[1])), f2u(h2f(c[2])), f2u(h2f(c[3])));
                break;
            }
            case 2: {
                float f[4];
                for (int j = 0; j < 4; j++) {
                    f[j] = (float)rndu(1300) / 1000.0f - 0.1f;
                    memcpy(o + 4 * j, &f[j], 4);
                }
                e = tu_px(f2u(f[0]), f2u(f[1]), f2u(f[2]), f2u(f[3]));
                break;
            }
            case 41: {
                float f = (float)rndu(1000) / 999.0f;
                memcpy(o, &f, 4);
                e = tu_px(f2u(f), f2u(f), f2u(f), 255);
                break;
            }
            case 54: {
                uint16_t c;
                o[1] = (uint8_t)(o[1] & 0x3F);
                c = (uint16_t)(o[0] | (o[1] << 8));
                e = tu_px(f2u(h2f(c)), f2u(h2f(c)), f2u(h2f(c)), 255);
                break;
            }
            case 31:
                e = tu_px(snorm_u8((int8_t)o[0]), snorm_u8((int8_t)o[1]), snorm_u8((int8_t)o[2]),
                          snorm_u8((int8_t)o[3]));
                break;
            case 51: e = tu_px(snorm_u8((int8_t)o[0]), snorm_u8((int8_t)o[1]), 0, 255); break;
            case 56: {
                uint32_t v = (uint32_t)o[0] | ((uint32_t)o[1] << 8);
                uint8_t g = sc(v, 16);
                e = tu_px(g, g, g, 255);
                break;
            }
            default: { /* 35 */
                uint32_t r = (uint32_t)o[0] | ((uint32_t)o[1] << 8);
                uint32_t g = (uint32_t)o[2] | ((uint32_t)o[3] << 8);
                e = tu_px(sc(r, 16), sc(g, 16), 0, 255);
                break;
            }
            }
            pc_buf_append(&b, o, F[k].bpp);
            exp[i] = e;
        }
        d = load_ok(&b, &m);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            size_t bad = tu_diff(px, exp, (size_t)W * H);
            CHECK(bad == 0);
            if (bad) INFO("dxgi %u: %zu bad", F[k].dxgi, bad);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        pc_buf_free(&b);
    }
}

/* Block formats with random block data, checked against bcdec directly. */
static void t_blocks(void)
{
    static const struct { const char *cc; uint32_t dxgi; int kind; } F[] = {
        /* kind: 1 bc1, 2 bc2, 3 bc3, 4 bc4u, 5 bc4s, 6 bc5u, 7 bc5s, 8 bc6hu, 9 bc6hs, 10 bc7,
         * 11 dxt2 (premul bc2), 12 dxt4 (premul bc3), 13 rxgb */
        { "DXT1", 0, 1 }, { "DXT3", 0, 2 }, { "DXT5", 0, 3 }, { "ATI1", 0, 4 }, { "BC4U", 0, 4 },
        { "BC4S", 0, 5 }, { "ATI2", 0, 6 }, { "BC5S", 0, 7 }, { "DXT2", 0, 11 },
        { "DXT4", 0, 12 }, { "RXGB", 0, 13 },
        { NULL, 71, 1 }, { NULL, 72, 1 }, { NULL, 74, 2 }, { NULL, 77, 3 }, { NULL, 80, 4 },
        { NULL, 81, 5 }, { NULL, 83, 6 }, { NULL, 84, 7 }, { NULL, 95, 8 }, { NULL, 96, 9 },
        { NULL, 98, 10 }, { NULL, 99, 10 },
    };
    const uint32_t W = 70, H = 66;    /* partial blocks on both edges, two bands */
    const uint32_t BW = (W + 3) / 4, BH = (H + 3) / 4;
    for (size_t k = 0; k < sizeof F / sizeof F[0]; k++) {
        int kind = F[k].kind;
        uint32_t bsz = (kind == 1 || kind == 4 || kind == 5) ? 8u : 16u;
        hdr_spec s;
        pc_buf b;
        pc_image_meta m;
        pc_doc *d;
        pc_px32 *exp = (pc_px32 *)malloc((size_t)W * H * sizeof *exp);
        uint8_t *blocks = (uint8_t *)malloc((size_t)BW * BH * bsz);
        memset(&s, 0, sizeof s);
        memset(&b, 0, sizeof b);
        s.w = W; s.h = H; s.mips = 1;
        if (F[k].cc) { s.pf_flags = 4; s.fourcc = cc(F[k].cc); }
        else { s.dx10 = true; s.dxgi = F[k].dxgi; }
        build_hdr(&b, &s);
        for (size_t i = 0; i < (size_t)BW * BH * bsz; i++) blocks[i] = rnd8();
        pc_buf_append(&b, blocks, (size_t)BW * BH * bsz);
        for (uint32_t by = 0; by < BH; by++)
            for (uint32_t bx = 0; bx < BW; bx++) {
                const uint8_t *src = blocks + ((size_t)by * BW + bx) * bsz;
                pc_px32 blk[16];
                uint8_t rgba[64];
                if (kind == 1 || kind == 2 || kind == 3 || kind >= 10) {
                    if (kind == 1) bcdec_bc1(src, rgba, 16);
                    else if (kind == 2 || kind == 11) bcdec_bc2(src, rgba, 16);
                    else if (kind == 3 || kind == 12 || kind == 13) bcdec_bc3(src, rgba, 16);
                    else bcdec_bc7(src, rgba, 16);
                    for (int i = 0; i < 16; i++) {
                        pc_px32 o = tu_px(rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2],
                                          rgba[4 * i + 3]);
                        if (kind == 13) { o.r = o.a; o.a = 255; }
                        if ((kind == 11 || kind == 12) && o.a) {
                            uint32_t a = o.a, v;
                            v = ((uint32_t)o.r * 255u + a / 2u) / a;
                            o.r = (uint8_t)(v > 255 ? 255 : v);
                            v = ((uint32_t)o.g * 255u + a / 2u) / a;
                            o.g = (uint8_t)(v > 255 ? 255 : v);
                            v = ((uint32_t)o.b * 255u + a / 2u) / a;
                            o.b = (uint8_t)(v > 255 ? 255 : v);
                        }
                        blk[i] = o;
                    }
                } else if (kind == 4 || kind == 5) {
                    uint8_t r[16];
                    bcdec_bc4(src, r, 4, kind == 5);
                    for (int i = 0; i < 16; i++) {
                        uint8_t v = kind == 5 ? snorm_u8((int8_t)r[i]) : r[i];
                        blk[i] = tu_px(v, v, v, 255);
                    }
                } else if (kind == 6 || kind == 7) {
                    uint8_t rg[32];
                    bcdec_bc5(src, rg, 8, kind == 7);
                    for (int i = 0; i < 16; i++)
                        blk[i] = kind == 7 ? tu_px(snorm_u8((int8_t)rg[2 * i]),
                                                   snorm_u8((int8_t)rg[2 * i + 1]), 0, 255)
                                           : tu_px(rg[2 * i], rg[2 * i + 1], 0, 255);
                } else {
                    float f[48];
                    bcdec_bc6h_float(src, f, 12, kind == 9);
                    for (int i = 0; i < 16; i++)
                        blk[i] = tu_px(f2u(f[3 * i]), f2u(f[3 * i + 1]), f2u(f[3 * i + 2]), 255);
                }
                for (uint32_t yy = 0; yy < 4; yy++)
                    for (uint32_t xx = 0; xx < 4; xx++)
                        if (bx * 4 + xx < W && by * 4 + yy < H)
                            exp[(size_t)(by * 4 + yy) * W + bx * 4 + xx] = blk[yy * 4 + xx];
            }
        d = load_ok(&b, &m);
        if (d) {
            pc_px32 *px = tu_layer_px(d, d->stack[0]);
            size_t bad = tu_diff(px, exp, (size_t)W * H);
            CHECK(bad == 0);
            if (bad) INFO("block format %zu: %zu bad", k, bad);
            free(px);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        free(blocks);
        free(exp);
        pc_buf_free(&b);
    }
}

static void t_headers(void)
{
    hdr_spec s;
    pc_buf b;
    pc_doc *d = (pc_doc *)1;
    pc_image_meta m;
    uint8_t px[4 * 8 * 8];
    memset(px, 0x7F, sizeof px);
    memset(&s, 0, sizeof s);
    s.w = 8; s.h = 8; s.mips = 1; s.pf_flags = 0x41; s.bits = 32;
    s.m[0] = 0xFF0000; s.m[1] = 0xFF00; s.m[2] = 0xFF; s.m[3] = 0xFF000000u;
#define TRY(expect, mutate) do { \
        hdr_spec t = s; pc_buf bb; memset(&bb, 0, sizeof bb); mutate; build_hdr(&bb, &t); \
        pc_buf_append(&bb, px, sizeof px); \
        d = (pc_doc *)1; \
        { pc_status st_ = dds()->load(bb.p, bb.n, NULL, &d, &m); CHECK(st_ == (expect)); \
          if (st_ == PC_OK) { pc_doc_destroy(d); pc_meta_free(&m); } else CHECK(d == NULL); \
          if (st_ != (expect)) INFO("line %d: got %s", __LINE__, pc_status_str(st_)); } \
        pc_buf_free(&bb); } while (0)
    TRY(PC_OK, (void)0);
    TRY(PC_ERR_FORMAT, t.w = 0);
    TRY(PC_ERR_FORMAT, t.w = 9);                               /* data too short */
    TRY(PC_ERR_FORMAT, t.mips = 5);                            /* 8x8 has 4 levels */
    TRY(PC_OK, t.mips = 4);                                    /* chain may be truncated */
    TRY(PC_ERR_FORMAT, t.bits = 12);
    TRY(PC_ERR_FORMAT, t.m[0] = 0x0F0F);                       /* not contiguous */
    TRY(PC_ERR_UNSUPPORTED, (t.pf_flags = 4, t.fourcc = cc("ABCD")));
    TRY(PC_ERR_UNSUPPORTED, t.pf_flags = 0x200);               /* YUV */
    TRY(PC_ERR_LIMIT, (t.w = 70000, t.h = 70000));
    TRY(PC_ERR_FORMAT, (t.dx10 = true, t.dxgi = 28, t.array = 0xFFFFFFFFu));
    TRY(PC_ERR_FORMAT, (t.dx10 = true, t.dxgi = 28, t.dim = 7));
    TRY(PC_ERR_UNSUPPORTED, (t.dx10 = true, t.dxgi = 999));
    TRY(PC_ERR_FORMAT, (t.dx10 = true, t.dxgi = 28, t.dim = 4, t.array = 2));
#undef TRY
    /* dwSize and pixel format size must be exact */
    memset(&b, 0, sizeof b);
    build_hdr(&b, &s);
    pc_buf_append(&b, px, sizeof px);
    b.p[4] = 24;
    CHECK(dds()->load(b.p, b.n, NULL, &d, &m) == PC_ERR_FORMAT && d == NULL);
    b.p[4] = 124; b.p[76] = 31;
    CHECK(dds()->load(b.p, b.n, NULL, &d, &m) == PC_ERR_FORMAT);
    b.p[76] = 32;
    CHECK(dds()->load(b.p, 100, NULL, &d, &m) == PC_ERR_FORMAT);
    pc_buf_free(&b);
    /* cube map and texture array: first face, with a note */
    {
        hdr_spec t = s;
        pc_buf bb;
        memset(&bb, 0, sizeof bb);
        t.dx10 = true; t.dxgi = 28; t.misc = 4; t.array = 2;
        build_hdr(&bb, &t);
        for (int f = 0; f < 12; f++) {
            uint8_t face[4 * 8 * 8];
            memset(face, f * 20, sizeof face);
            pc_buf_append(&bb, face, sizeof face);
        }
        d = load_ok(&bb, &m);
        if (d) {
            pc_px32 p = pc_layer_get_px(d->stack[0], 3, 3);
            CHECK(p.r == 0 && p.a == 0);       /* first face is all zero */
            CHECK(strstr(m.note, "first image") != NULL);
            pc_doc_destroy(d);
            pc_meta_free(&m);
        }
        pc_buf_free(&bb);
        memset(&bb, 0, sizeof bb);
        t = s;
        t.caps2 = 0x200 | 0xFC00;              /* legacy cube, all faces: a cross */
        build_hdr(&bb, &t);
        for (int f = 0; f < 6; f++) pc_buf_append(&bb, px, sizeof px);
        d = load_ok(&bb, &m);
        CHECK(d && d->w == 32 && d->h == 24 && strstr(m.note, "cross") != NULL);
        pc_doc_destroy(d);
        pc_meta_free(&m);
        /* faces missing from the file: falls back to the first face */
        bb.n -= 3 * sizeof px;
        d = load_ok(&bb, &m);
        CHECK(d && d->w == 8 && d->h == 8);
        pc_doc_destroy(d);
        pc_meta_free(&m);
        pc_buf_free(&bb);
    }
    {
        static const uint8_t magic[8] = { 'D', 'D', 'S', ' ', 124, 0, 0, 0 };
        CHECK(dds()->sniff(magic, 8) && dds()->sniff(magic, 4) && !dds()->sniff(magic, 3));
        CHECK(pc_codec_sniff(magic, 8) == dds());
    }
}

/* ---- save round trips -------------------------------------------------------------------- */
static pc_doc *save_load(const pc_doc *d, const dds_params_t *p, pc_buf *keep)
{
    pc_buf out;
    pc_image_meta m;
    pc_doc *r = NULL;
    memset(&out, 0, sizeof out);
    CHECK(dds()->save(d, NULL, p, NULL, &out) == PC_OK);
    if (out.n) {
        pc_status st = dds()->load(out.p, out.n, NULL, &r, &m);
        CHECK(st == PC_OK);
        if (st == PC_OK) pc_meta_free(&m);
    }
    if (keep) *keep = out; else pc_buf_free(&out);
    return r;
}

static void t_save_formats(void)
{
    const uint32_t W = 70, H = 45;
    pc_px32 *photo = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, photo);
    for (int f = 0; f < F_COUNT; f++) {
        dds_params_t p;
        pc_buf file;
        pc_doc *r;
        pc_codec_default_params(dds(), &p);
        p.format = f;
        p.bc7_speed = 0;
        r = save_load(d, &p, &file);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            size_t n = (size_t)W * H, bad = 0;
            int tol = 0;
            double need = 0;
            CHECK(r->w == W && r->h == H);
            for (size_t i = 0; i < n; i++) {
                pc_px32 a = photo[i], g = px[i], e = a;
                switch (f) {
                case F_BGRX: case F_BGRXS: case F_RGBX: case F_BGR8: case F_565:
                    e.a = 255; break;
                case F_R8: case F_R32F: e = tu_px(a.r, a.r, a.r, 255); break;
                case F_RG8: case F_RG8S: e = tu_px(a.r, a.g, 0, 255); break;
                case F_BC4: case F_ATI1: e = tu_px(a.r, a.r, a.r, 255); break;
                case F_BC5U: case F_BC5S: case F_ATI2: e = tu_px(a.r, a.g, 0, 255); break;
                case F_RXGB: e.a = 255; break;
                case F_BC1: case F_BC1S: e.a = a.a < 128 ? 0 : 255; break;
                default: break;
                }
                switch (f) {
                case F_5551: tol = 5; e.a = a.a >= 128 ? 255 : 0; break;
                case F_565: tol = 5; break;
                case F_4444: tol = 9; break;
                case F_RG8S: tol = 1; break;
                default: break;
                }
                if (f == F_BC1 || f == F_BC1S) {   /* transparent pixels: alpha only */
                    if (g.a != e.a) bad++;
                    if (e.a == 0) { px[i] = e; }
                    continue;
                }
                if (tol || f <= F_BC7S || f >= F_ATI1) continue;
                if (!tu_px_eq(g, e)) bad++;
            }
            if (f >= F_BGRA && f <= F_RGBAS) CHECK(bad == 0);
            if (f >= F_R8 && f <= F_RGBX && f != F_RG8S) CHECK(bad == 0);
            if (f == F_BC1 || f == F_BC1S) CHECK(bad == 0);
            if (tol) {
                pc_px32 *e = (pc_px32 *)malloc(n * sizeof *e);
                for (size_t i = 0; i < n; i++) {
                    e[i] = photo[i];
                    if (f == F_565) e[i].a = 255;
                    if (f == F_5551) e[i].a = photo[i].a >= 128 ? 255 : 0;
                    if (f == F_RG8S) e[i] = tu_px(photo[i].r, photo[i].g, 0, 255);
                }
                CHECK(tu_max_abs_diff(px, e, n) <= tol);
                free(e);
            }
            if (f <= F_BC7S || f >= F_ATI1) {
                /* block formats: PSNR over the channels the format keeps */
                pc_px32 *e = (pc_px32 *)malloc(n * sizeof *e);
                pc_px32 *g = (pc_px32 *)malloc(n * sizeof *g);
                for (size_t i = 0; i < n; i++) {
                    pc_px32 a = photo[i];
                    g[i] = px[i];
                    switch (f) {
                    case F_BC4: case F_ATI1: e[i] = tu_px(a.r, a.r, a.r, 255); break;
                    case F_BC5U: case F_BC5S: case F_ATI2: e[i] = tu_px(a.r, a.g, 0, 255); break;
                    case F_RXGB: e[i] = tu_px(a.r, a.g, a.b, 255); break;
                    default: e[i] = a; break;
                    }
                    if ((f == F_BC1 || f == F_BC1S) && a.a < 128) g[i] = e[i];
                }
                need = (f == F_BC7 || f == F_BC7S) ? 34.0 : 30.0;
                CHECK(tu_psnr(g, e, n) >= need);
                if (tu_psnr(g, e, n) < need) INFO("format %d psnr %.2f", f, tu_psnr(g, e, n));
                if (f == F_BC2 || f == F_BC3 || f == F_BC7) {
                    int amax = 0;
                    for (size_t i = 0; i < n; i++) {
                        int da = abs((int)px[i].a - (int)photo[i].a);
                        if (da > amax) amax = da;
                    }
                    CHECK(amax <= (f == F_BC2 ? 9 : 12));
                }
                free(e); free(g);
            }
            /* sRGB variants use a DX10 header, linear BC1..3 a legacy FourCC */
            if (f == F_BC1S || f == F_BC7S || f == F_RGBAS) {
                CHECK(file.n > 148 && memcmp(file.p + 84, "DX10", 4) == 0);
            }
            if (f == F_BC1) CHECK(memcmp(file.p + 84, "DXT1", 4) == 0);
            if (f == F_ATI2) CHECK(memcmp(file.p + 84, "ATI2", 4) == 0);
            free(px);
            pc_doc_destroy(r);
        }
        pc_buf_free(&file);
    }
    pc_doc_destroy(d);
    free(photo);
}

static void t_mips_and_dither(void)
{
    const uint32_t W = 37, H = 20;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    dds_params_t p;
    pc_buf file;
    pc_doc *r;
    /* B8G8R8A8 with mips: 37x20 -> 6 levels, sizes 37x20 .. 1x1 */
    pc_codec_default_params(dds(), &p);
    p.format = F_BGRA;
    p.mipmaps = 1;
    p.gamma = 0;
    r = save_load(d, &p, &file);
    if (r) {
        size_t expect = 128, lw = W, lh = H;
        uint32_t levels = 0;
        for (;;) {
            expect += lw * lh * 4;
            levels++;
            if (lw == 1 && lh == 1) break;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        CHECK(levels == 6);
        CHECK(file.n == expect);
        CHECK(file.p[28] == 6);
        {   /* top level unchanged */
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK(tu_diff(px, a, (size_t)W * H) == 0);
            free(px);
        }
        {   /* level 1 (18x10) is the exact area average (Fant) in premultiplied space */
            const uint8_t *l1 = file.p + 128 + (size_t)W * H * 4;
            double sx = (double)W / 18.0;
            int bad = 0;
            for (uint32_t y = 0; y < 10; y++)
                for (uint32_t x = 0; x < 18; x++) {
                    double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
                    double x0 = x * sx, x1 = x0 + sx;
                    for (uint32_t yy = 2 * y; yy < 2 * y + 2; yy++)
                        for (uint32_t xx = (uint32_t)floor(x0); xx < (uint32_t)ceil(x1); xx++) {
                            double ov = fmin(x1, xx + 1.0) - fmax(x0, (double)xx);
                            pc_px32 q = a[(size_t)yy * W + (xx < W ? xx : W - 1)];
                            double al = q.a / 255.0 * ov;
                            if (ov <= 0) continue;
                            acc[0] += q.b / 255.0 * al; acc[1] += q.g / 255.0 * al;
                            acc[2] += q.r / 255.0 * al; acc[3] += al;
                            wsum += ov;
                        }
                    {
                        double al = acc[3] / wsum;
                        const uint8_t *o = l1 + ((size_t)y * 18 + x) * 4;
                        int ea = (int)(al * 255.0 + 0.5);
                        if (abs(o[3] - ea) > 1) bad++;
                        for (int c = 0; c < 3 && al > 0; c++) {
                            int ec = (int)(acc[c] / acc[3] * 255.0 + 0.5);
                            if (abs(o[c] - ec) > 1) bad++;
                        }
                    }
                }
            CHECK(bad == 0);
        }
        pc_doc_destroy(r);
    }
    pc_buf_free(&file);
    /* every filter and gamma setting, BC1 with mips */
    for (int filt = 0; filt < 6; filt++)
        for (int g = 0; g < 2; g++) {
            pc_codec_default_params(dds(), &p);
            p.format = F_BC1;
            p.mipmaps = 1;
            p.mip_filter = filt;
            p.gamma = g;
            r = save_load(d, &p, &file);
            CHECK(r != NULL);
            CHECK(file.n == 128 + (size_t)(10 * 5 + 5 * 3 + 3 * 2 + 1 + 1 + 1) * 8);
            pc_doc_destroy(r);
            pc_buf_free(&file);
        }
    /* error diffusion on a color 5:6:5 cannot represent: 8x8 block means
     * stay closer to the source than with plain rounding */
    {
        const uint32_t GW = 64, GH = 64;
        pc_px32 *gr = (pc_px32 *)malloc((size_t)GW * GH * sizeof *gr);
        double mean[2];
        for (size_t i = 0; i < (size_t)GW * GH; i++) gr[i] = tu_px(100, 37, 203, 255);
        for (int k = 0; k < 2; k++) {
            pc_doc *gd = tu_doc_from_px(GW, GH, gr);
            pc_codec_default_params(dds(), &p);
            p.format = F_565;
            p.dither = k;
            r = save_load(gd, &p, NULL);
            mean[k] = 0;
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                for (uint32_t by = 0; by < GH; by += 8)
                    for (uint32_t bx = 0; bx < GW; bx += 8) {
                        double s[3] = { 0, 0, 0 };
                        for (uint32_t y = by; y < by + 8; y++)
                            for (uint32_t x = bx; x < bx + 8; x++) {
                                s[0] += px[y * GW + x].r; s[1] += px[y * GW + x].g;
                                s[2] += px[y * GW + x].b;
                            }
                        mean[k] += fabs(s[0] / 64.0 - 100.0) + fabs(s[1] / 64.0 - 37.0) +
                                   fabs(s[2] / 64.0 - 203.0);
                    }
                CHECK(tu_max_abs_diff(px, gr, (size_t)GW * GH) <= 12);
                free(px);
                pc_doc_destroy(r);
            }
            pc_doc_destroy(gd);
        }
        CHECK(mean[1] < mean[0] * 0.5);
        if (!(mean[1] < mean[0] * 0.5))
            INFO("dither block error %.3f vs plain %.3f", mean[1], mean[0]);
        free(gr);
    }
    /* BC7 speeds and both metrics */
    for (int sp = 0; sp < 3; sp++)
        for (int mt = 0; mt < 2; mt++) {
            pc_codec_default_params(dds(), &p);
            p.format = F_BC7;
            p.bc7_speed = sp;
            p.metric = mt;
            r = save_load(d, &p, NULL);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                /* perceptual weighting trades RGB PSNR for luma accuracy */
                double need = mt == 0 ? 30.0 : 33.0;
                CHECK(tu_psnr(px, a, (size_t)W * H) > need);
                if (tu_psnr(px, a, (size_t)W * H) <= need)
                    INFO("bc7 speed %d metric %d: %.2f", sp, mt, tu_psnr(px, a, (size_t)W * H));
                free(px);
                pc_doc_destroy(r);
            }
        }
    pc_doc_destroy(d);
    free(a);
}

static pc_px32 face_color(int f)
{
    return tu_px((uint8_t)(40 * f), (uint8_t)(200 - 30 * f), 90, 255);
}

/* Crossed images save as cube maps and load back as a horizontal cross. */
static void t_cube_maps(void)
{
    static const int hx[6][2] = { { 2, 1 }, { 0, 1 }, { 1, 0 }, { 1, 2 }, { 1, 1 }, { 3, 1 } };
    static const int vx[6][2] = { { 2, 1 }, { 0, 1 }, { 1, 0 }, { 1, 2 }, { 1, 1 }, { 1, 3 } };
    const int S = 12;
    for (int vert = 0; vert < 2; vert++)
        for (int fmt = 0; fmt < 2; fmt++) {
            uint32_t W = vert ? 3u * S : 4u * S, H = vert ? 4u * S : 3u * S;
            pc_px32 *img = (pc_px32 *)calloc((size_t)W * H, sizeof *img);
            pc_doc *d, *r;
            dds_params_t p;
            pc_buf file;
            /* face f is a solid color with a marker pixel at its top-left */
            for (int f = 0; f < 6; f++) {
                const int *pos = vert ? vx[f] : hx[f];
                for (int y = 0; y < S; y++)
                    for (int x = 0; x < S; x++)
                        img[(size_t)(pos[1] * S + y) * W + (size_t)(pos[0] * S + x)] =
                            (x == 0 && y == 0) ? tu_px(255, 255, 255, 255) : face_color(f);
            }
            d = tu_doc_from_px(W, H, img);
            pc_codec_default_params(dds(), &p);
            p.format = fmt ? F_BGRA : F_BGRAS;
            p.cube_map = 1;
            p.mipmaps = fmt;
            r = save_load(d, &p, &file);
            /* caps2: cube map with all six faces */
            CHECK(file.n > 148 && file.p[112] == 0 && file.p[113] == 0xFE);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                int bad = 0;
                CHECK(r->w == 4u * (uint32_t)S && r->h == 3u * (uint32_t)S);
                for (int f = 0; f < 6 && r->w == 4u * (uint32_t)S; f++)
                    for (int y = 0; y < S; y++)
                        for (int x = 0; x < S; x++) {
                            pc_px32 e = (x == 0 && y == 0) ? tu_px(255, 255, 255, 255)
                                                           : face_color(f);
                            size_t at = (size_t)(hx[f][1] * S + y) * r->w +
                                        (size_t)(hx[f][0] * S + x);
                            if (!tu_px_eq(px[at], e)) bad++;
                        }
                CHECK(bad == 0);
                free(px);
                pc_doc_destroy(r);
            }
            pc_buf_free(&file);
            pc_doc_destroy(d);
            free(img);
        }
    {   /* not a cross */
        pc_px32 *img = tu_noise(20, 20, 0);
        pc_doc *d = tu_doc_from_px(20, 20, img);
        dds_params_t p;
        pc_buf file;
        memset(&file, 0, sizeof file);
        pc_codec_default_params(dds(), &p);
        p.cube_map = 1;
        CHECK(dds()->save(d, NULL, &p, NULL, &file) == PC_ERR_ARG && file.n == 0);
        pc_doc_destroy(d);
        free(img);
    }
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 1500u : 20000u, ok = 0;
    const uint32_t W = 21, H = 13;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int fmts[] = { F_BC1, F_BC3, F_BC7S, F_BGRA, F_565, F_RG8S };
    for (size_t k = 0; k < sizeof fmts / sizeof fmts[0]; k++) {
        dds_params_t p;
        pc_buf out;
        memset(&out, 0, sizeof out);
        pc_codec_default_params(dds(), &p);
        p.format = fmts[k];
        p.mipmaps = 1;
        p.bc7_speed = 0;
        CHECK(dds()->save(d, NULL, &p, NULL, &out) == PC_OK);
        ok += tu_fuzz_codec(dds(), out.p, out.n, iters, NULL);
        pc_buf_free(&out);
    }
    INFO("dds fuzz: %u of %u mutated files decoded", ok, 6u * iters);
    pc_doc_destroy(d);
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_masks);
    RUN(t_dxgi_plain);
    RUN(t_blocks);
    RUN(t_headers);
    RUN(t_save_formats);
    RUN(t_mips_and_dither);
    RUN(t_cube_maps);
    RUN(t_fuzz);
    return pc_test_finish();
}
