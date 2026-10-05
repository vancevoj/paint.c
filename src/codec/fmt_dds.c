/* fmt_dds.c - DirectDraw Surface (lane L6B).
 *
 * Load: an own hardened parser. The header must say dwSize 124 and a pixel
 * format size of 32; the DX10 extension is validated (dimension, array
 * size, cube flag); mip, depth, array and face counts are bounded and the
 * size of the whole chain is computed with checked math. dwPitchOrLinear
 * Size is ignored: sizes come from the format. The top mip of the first
 * array element / depth slice is decoded (all six faces of a cube map): BC1..BC7 through bcdec
 * (BC6H clamped to [0, 1], no tone curve), legacy bit-mask formats (any
 * RGB / luminance / alpha masks of 8 to 32 bits), and the common DXGI
 * formats (8-bit, 10:10:10:2, 5:6:5, 5:5:5:1, 4:4:4:4, 16-bit and float).
 * Single-channel formats (R8, L8, BC4, R16, R32F) load as gray; SNORM data
 * maps [-1, 1] to [0, 255].
 *
 * Save (DDS file type bundled with Paint.NET 5.1): the formats in k_formats
 * (BC1, BC2, BC3 linear and sRGB, BC4, BC5 unsigned and signed, BC6H
 * unsigned (own encoder, bc6h_enc.c), BC7 linear and sRGB, the uncompressed
 * 32, 24 and 16-bit layouts, R8, R8G8, R32 float, and the legacy ATI1,
 * ATI2 and RXGB variants), error diffusion dithering (on by default; BC1..BC3
 * color indices and BC2 alpha within each block, and the 16-bit layouts),
 * BC6H / BC7 compression speed, error metric (perceptual or uniform;
 * BC1..BC3 index selection), cube maps from a horizontal (4:3) or vertical
 * (3:4) crossed image, and mipmap generation with a choice of resampling
 * filter (default Bicubic) and gamma correction. Each option is enabled
 * only for the formats it applies to (FILES.md, DDS rows). Complete cube
 * maps load as a horizontal cross.
 *
 * Threads: reentrant. The BC7 encoder tables are built once behind an
 * atomic flag.
 */
#include "lib_codec.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bc6h_enc.h"
#include "bcdec.h"
#include "stb_dxt.h"
#include "pc_bc7enc.h"

/* ---- file structure -------------------------------------------------------------- */
#define DDS_MAGIC          0x20534444u   /* "DDS " */
#define DDS_HDR_SIZE       124u
#define DDS_PF_SIZE        32u
#define DDS_DX10_SIZE      20u

#define DDSD_CAPS          0x1u
#define DDSD_HEIGHT        0x2u
#define DDSD_WIDTH         0x4u
#define DDSD_PITCH         0x8u
#define DDSD_PIXELFORMAT   0x1000u
#define DDSD_MIPMAPCOUNT   0x20000u
#define DDSD_LINEARSIZE    0x80000u
#define DDSD_DEPTH         0x800000u

#define DDPF_ALPHAPIXELS   0x1u
#define DDPF_ALPHA         0x2u
#define DDPF_FOURCC        0x4u
#define DDPF_RGB           0x40u
#define DDPF_YUV           0x200u
#define DDPF_LUMINANCE     0x20000u
#define DDPF_BUMPDUDV      0x80000u

#define DDSCAPS_COMPLEX    0x8u
#define DDSCAPS_TEXTURE    0x1000u
#define DDSCAPS_MIPMAP     0x400000u
#define DDSCAPS2_CUBEMAP   0x200u
#define DDSCAPS2_FACES     0xFC00u
#define DDSCAPS2_VOLUME    0x200000u

#define DX10_DIM_TEX1D     2u
#define DX10_DIM_TEX2D     3u
#define DX10_DIM_TEX3D     4u
#define DX10_MISC_CUBE     0x4u

#define DDS_MAX_ARRAY      2048u
#define DDS_MAX_DEPTH      16384u

static uint32_t fourcc(const char *s)
{
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}

/* Internal pixel layouts. */
typedef enum dds_kind {
    K_NONE = 0,
    K_BC1, K_BC2, K_BC3, K_BC4U, K_BC4S, K_BC5U, K_BC5S, K_BC6HU, K_BC6HS, K_BC7,
    K_MASK,             /* bit masks within an 8..32-bit little-endian word */
    K_RGBA8S,           /* R8G8B8A8_SNORM */
    K_RG8S,             /* R8G8_SNORM */
    K_RGBA16,           /* R16G16B16A16_UNORM */
    K_RGBA16F,          /* R16G16B16A16_FLOAT */
    K_RGBA32F,          /* R32G32B32A32_FLOAT */
    K_R16F,             /* R16_FLOAT (gray) */
    K_R32F              /* R32_FLOAT (gray) */
} dds_kind;

typedef struct dds_fmt {
    dds_kind kind;
    uint32_t bits;             /* K_MASK: 8, 16, 24 or 32 */
    uint32_t mask[4];          /* K_MASK: r, g, b, a (0 = absent) */
    bool     gray;             /* K_MASK: r mask is luminance */
    bool     premul;           /* DXT2 / DXT4 */
    bool     rxgb;             /* red stored in alpha */
} dds_fmt;

typedef struct dds_info {
    uint32_t w, h, depth, mips, layers;   /* layers = array size * faces */
    bool     cube;                        /* one complete cube map (6 faces) */
    dds_fmt  fmt;
    size_t   data_off;
    size_t   top_size;                    /* bytes of one top-level image */
    size_t   chain;                       /* bytes of one layer's mip chain */
} dds_info;

static bool is_bc(dds_kind k) { return k >= K_BC1 && k <= K_BC7; }

static uint32_t bc_block_bytes(dds_kind k)
{
    return (k == K_BC1 || k == K_BC4U || k == K_BC4S) ? 8u : 16u;
}

static uint32_t fmt_bpp(const dds_fmt *f)    /* bits per pixel, uncompressed */
{
    switch (f->kind) {
    case K_MASK: return f->bits;
    case K_RGBA8S: return 32u;
    case K_RG8S: return 16u;
    case K_RGBA16: case K_RGBA16F: return 64u;
    case K_RGBA32F: return 128u;
    case K_R16F: return 16u;
    case K_R32F: return 32u;
    default: return 0u;
    }
}

/* Bytes of one w x h x depth image of this format (checked). */
static bool image_bytes(const dds_fmt *f, uint32_t w, uint32_t h, uint32_t d, size_t *out)
{
    size_t a, b;
    if (is_bc(f->kind)) {
        size_t bw = ((size_t)w + 3u) / 4u, bh = ((size_t)h + 3u) / 4u;
        if (!pc_mul_size(bw, bh, &a) || !pc_mul_size(a, bc_block_bytes(f->kind), &b)) return false;
    } else {
        size_t pitch;
        if (!pc_mul_size(w, fmt_bpp(f), &a)) return false;
        pitch = (a + 7u) / 8u;
        if (!pc_mul_size(pitch, h, &b)) return false;
    }
    return pc_mul_size(b, d, out);
}

static void set_mask(dds_fmt *f, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    f->kind = K_MASK;
    f->bits = bits;
    f->mask[0] = r; f->mask[1] = g; f->mask[2] = b; f->mask[3] = a;
}

/* DXGI_FORMAT -> layout. False when unsupported. */
static bool dxgi_fmt(uint32_t dxgi, dds_fmt *f)
{
    memset(f, 0, sizeof *f);
    switch (dxgi) {
    case 70: case 71: case 72: f->kind = K_BC1; return true;
    case 73: case 74: case 75: f->kind = K_BC2; return true;
    case 76: case 77: case 78: f->kind = K_BC3; return true;
    case 79: case 80: f->kind = K_BC4U; return true;
    case 81: f->kind = K_BC4S; return true;
    case 82: case 83: f->kind = K_BC5U; return true;
    case 84: f->kind = K_BC5S; return true;
    case 94: case 95: f->kind = K_BC6HU; return true;
    case 96: f->kind = K_BC6HS; return true;
    case 97: case 98: case 99: f->kind = K_BC7; return true;
    case 27: case 28: case 29: case 30:
        set_mask(f, 32, 0xFFu, 0xFF00u, 0xFF0000u, 0xFF000000u); return true;
    case 31: f->kind = K_RGBA8S; return true;
    case 87: case 90: case 91:
        set_mask(f, 32, 0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u); return true;
    case 88: case 92: case 93:
        set_mask(f, 32, 0xFF0000u, 0xFF00u, 0xFFu, 0u); return true;
    case 23: case 24: case 25:
        set_mask(f, 32, 0x3FFu, 0xFFC00u, 0x3FF00000u, 0xC0000000u); return true;
    case 85: set_mask(f, 16, 0xF800u, 0x7E0u, 0x1Fu, 0u); return true;
    case 86: set_mask(f, 16, 0x7C00u, 0x3E0u, 0x1Fu, 0x8000u); return true;
    case 115: set_mask(f, 16, 0xF00u, 0xF0u, 0xFu, 0xF000u); return true;
    case 60: case 61: case 62: set_mask(f, 8, 0xFFu, 0, 0, 0); f->gray = true; return true;
    case 65: set_mask(f, 8, 0, 0, 0, 0xFFu); return true;
    case 48: case 49: case 50: set_mask(f, 16, 0xFFu, 0xFF00u, 0, 0); return true;
    case 51: f->kind = K_RG8S; return true;
    case 53: case 56: case 57: set_mask(f, 16, 0xFFFFu, 0, 0, 0); f->gray = true; return true;
    case 33: case 35: case 36: set_mask(f, 32, 0xFFFFu, 0xFFFF0000u, 0, 0); return true;
    case 9: case 11: case 12: f->kind = K_RGBA16; return true;
    case 10: f->kind = K_RGBA16F; return true;
    case 1: case 2: f->kind = K_RGBA32F; return true;
    case 54: f->kind = K_R16F; return true;
    case 39: case 41: f->kind = K_R32F; return true;
    default: return false;
    }
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t popcount32(uint32_t v)
{
    uint32_t c = 0;
    for (; v; v &= v - 1u) c++;
    return c;
}

/* Masks must be contiguous bit runs inside the pixel word. */
static bool mask_ok(uint32_t m, uint32_t bits)
{
    uint32_t lo;
    if (!m) return true;
    if (bits < 32u && (m >> bits) != 0u) return false;
    lo = m & (0u - m);                    /* lowest set bit */
    return ((m + lo) & m) == 0u;          /* contiguous */
}

static pc_status dds_parse(const uint8_t *p, size_t n, const pc_codec_limits *lim, dds_info *in)
{
    uint32_t flags, pf_flags, pf_fourcc, caps2, max_side, max_mips;
    size_t one, total = 0, chain = 0;
    memset(in, 0, sizeof *in);
    if (n < 4u + DDS_HDR_SIZE) return PC_ERR_FORMAT;
    if (rd32(p) != DDS_MAGIC || rd32(p + 4) != DDS_HDR_SIZE || rd32(p + 76) != DDS_PF_SIZE)
        return PC_ERR_FORMAT;
    flags = rd32(p + 8);
    in->h = rd32(p + 12);
    in->w = rd32(p + 16);
    in->depth = rd32(p + 24);
    in->mips = rd32(p + 28);
    pf_flags = rd32(p + 80);
    pf_fourcc = rd32(p + 84);
    caps2 = rd32(p + 112);
    in->data_off = 4u + DDS_HDR_SIZE;
    in->layers = 1u;
    if (!(flags & DDSD_DEPTH) && !(caps2 & DDSCAPS2_VOLUME)) in->depth = 1u;
    if (in->depth == 0u) in->depth = 1u;
    if (in->mips == 0u) in->mips = 1u;

    if ((pf_flags & DDPF_FOURCC) && pf_fourcc == fourcc("DX10")) {
        uint32_t dxgi, dim, misc, arr;
        if (n < 4u + DDS_HDR_SIZE + DDS_DX10_SIZE) return PC_ERR_FORMAT;
        dxgi = rd32(p + 128);
        dim = rd32(p + 132);
        misc = rd32(p + 136);
        arr = rd32(p + 140);
        in->data_off += DDS_DX10_SIZE;
        if (arr == 0u || arr > DDS_MAX_ARRAY) return PC_ERR_FORMAT;
        if (dim == DX10_DIM_TEX1D) { in->h = in->h ? in->h : 1u; in->depth = 1u; }
        else if (dim == DX10_DIM_TEX2D) { in->depth = 1u; }
        else if (dim == DX10_DIM_TEX3D) { if (arr != 1u) return PC_ERR_FORMAT; }
        else return PC_ERR_FORMAT;
        in->layers = arr * ((misc & DX10_MISC_CUBE) ? 6u : 1u);
        in->cube = (misc & DX10_MISC_CUBE) && arr == 1u && dim == DX10_DIM_TEX2D;
        if (!dxgi_fmt(dxgi, &in->fmt)) return PC_ERR_UNSUPPORTED;
    } else if (pf_flags & DDPF_FOURCC) {
        dds_fmt *f = &in->fmt;
        memset(f, 0, sizeof *f);
        if (pf_fourcc == fourcc("DXT1")) f->kind = K_BC1;
        else if (pf_fourcc == fourcc("DXT2")) { f->kind = K_BC2; f->premul = true; }
        else if (pf_fourcc == fourcc("DXT3")) f->kind = K_BC2;
        else if (pf_fourcc == fourcc("DXT4")) { f->kind = K_BC3; f->premul = true; }
        else if (pf_fourcc == fourcc("DXT5")) f->kind = K_BC3;
        else if (pf_fourcc == fourcc("RXGB")) { f->kind = K_BC3; f->rxgb = true; }
        else if (pf_fourcc == fourcc("ATI1") || pf_fourcc == fourcc("BC4U")) f->kind = K_BC4U;
        else if (pf_fourcc == fourcc("BC4S")) f->kind = K_BC4S;
        else if (pf_fourcc == fourcc("ATI2") || pf_fourcc == fourcc("BC5U")) f->kind = K_BC5U;
        else if (pf_fourcc == fourcc("BC5S")) f->kind = K_BC5S;
        else if (pf_fourcc == 36u) f->kind = K_RGBA16;
        else if (pf_fourcc == 113u) f->kind = K_RGBA16F;
        else if (pf_fourcc == 116u) f->kind = K_RGBA32F;
        else if (pf_fourcc == 111u) f->kind = K_R16F;
        else if (pf_fourcc == 114u) f->kind = K_R32F;
        else return PC_ERR_UNSUPPORTED;
    } else if (pf_flags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA)) {
        uint32_t bits = rd32(p + 88);
        dds_fmt *f = &in->fmt;
        if (pf_flags & (DDPF_YUV | DDPF_BUMPDUDV)) return PC_ERR_UNSUPPORTED;
        if (bits != 8u && bits != 16u && bits != 24u && bits != 32u) return PC_ERR_FORMAT;
        memset(f, 0, sizeof *f);
        set_mask(f, bits, rd32(p + 92), rd32(p + 96), rd32(p + 100),
                 (pf_flags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)) ? rd32(p + 104) : 0u);
        if (pf_flags & DDPF_ALPHA) { f->mask[0] = f->mask[1] = f->mask[2] = 0u; }
        if (pf_flags & DDPF_LUMINANCE) { f->gray = true; f->mask[1] = f->mask[2] = 0u; }
        for (int c = 0; c < 4; c++)
            if (!mask_ok(f->mask[c], bits)) return PC_ERR_FORMAT;
        if (!(f->mask[0] | f->mask[1] | f->mask[2] | f->mask[3])) return PC_ERR_FORMAT;
    } else {
        return PC_ERR_UNSUPPORTED;
    }
    if (!(pf_flags & DDPF_FOURCC) || pf_fourcc != fourcc("DX10")) {
        if (caps2 & DDSCAPS2_CUBEMAP) {
            uint32_t faces = popcount32(caps2 & DDSCAPS2_FACES);
            in->layers = faces ? faces : 6u;
            in->cube = in->layers == 6u;
        }
    }
    if (in->w == 0u || in->h == 0u) return PC_ERR_FORMAT;
    if (in->depth > DDS_MAX_DEPTH) return PC_ERR_FORMAT;
    {
        pc_status st = pc_codec_check_size(lim, in->w, in->h, 1u);
        if (st != PC_OK) return st;
    }
    max_side = in->w > in->h ? in->w : in->h;
    if (in->depth > max_side) max_side = in->depth;
    max_mips = 1u;
    while (max_mips < 32u && (max_side >> max_mips) != 0u) max_mips++;
    if (in->mips > max_mips) return PC_ERR_FORMAT;
    /* top image of the first layer: what we decode (depth slice 0) */
    if (!image_bytes(&in->fmt, in->w, in->h, 1u, &in->top_size)) return PC_ERR_LIMIT;
    /* whole chain with checked math (not required to be present) */
    for (uint32_t m = 0; m < in->mips; m++) {
        uint32_t mw = in->w >> m, mh = in->h >> m, md = in->depth >> m;
        if (!image_bytes(&in->fmt, mw ? mw : 1u, mh ? mh : 1u, md ? md : 1u, &one) ||
            !pc_add_size(chain, one, &chain))
            return PC_ERR_LIMIT;
    }
    if (!pc_mul_size(chain, in->layers, &total)) return PC_ERR_LIMIT;
    in->chain = chain;
    if (in->depth > 1u) in->cube = false;
    if (in->data_off > n || n - in->data_off < in->top_size) return PC_ERR_FORMAT;
    return PC_OK;
}

/* ---- pixel decoding ------------------------------------------------------------------- */
static uint8_t unorm_bits(uint32_t v, uint32_t bits)
{
    uint64_t max;
    if (bits == 0u) return 0u;
    max = bits >= 32u ? 0xFFFFFFFFu : ((uint64_t)1 << bits) - 1u;
    return (uint8_t)(((uint64_t)v * 255u + max / 2u) / max);
}

static uint8_t snorm8_to_u8(int v)
{
    if (v < -127) v = -127;
    return (uint8_t)(((v + 127) * 255 + 127) / 254);
}

static uint8_t float_to_u8(float f)
{
    if (!(f > 0.0f)) return 0u;
    if (f >= 1.0f) return 255u;
    return (uint8_t)(f * 255.0f + 0.5f);
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15), e = (uint32_t)(h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0u) v = (float)m * (1.0f / 16777216.0f);              /* subnormal: m * 2^-24 */
    else if (e == 31u) v = m ? 0.0f : 65504.0f * 2.0f;              /* NaN -> 0, inf large */
    else v = ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

static float rdf32(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

typedef struct mask_ch { uint32_t mask, shift, bits; } mask_ch;

static void decode_row_uncompressed(const dds_fmt *f, const uint8_t *s, pc_px32 *d, uint32_t w)
{
    if (f->kind == K_MASK) {
        mask_ch ch[4];
        uint32_t bytes = f->bits / 8u;
        for (int c = 0; c < 4; c++) {
            uint32_t m = f->mask[c], sh = 0;
            while (m && !(m & 1u)) { m >>= 1; sh++; }
            ch[c].mask = f->mask[c]; ch[c].shift = sh; ch[c].bits = popcount32(f->mask[c]);
        }
        for (uint32_t x = 0; x < w; x++, s += bytes) {
            uint32_t v = s[0];
            uint8_t r, g, b, a;
            if (bytes > 1u) v |= (uint32_t)s[1] << 8;
            if (bytes > 2u) v |= (uint32_t)s[2] << 16;
            if (bytes > 3u) v |= (uint32_t)s[3] << 24;
            r = unorm_bits((v & ch[0].mask) >> ch[0].shift, ch[0].bits);
            g = unorm_bits((v & ch[1].mask) >> ch[1].shift, ch[1].bits);
            b = unorm_bits((v & ch[2].mask) >> ch[2].shift, ch[2].bits);
            a = ch[3].mask ? unorm_bits((v & ch[3].mask) >> ch[3].shift, ch[3].bits) : 255u;
            if (f->gray) g = b = r;
            d[x].r = r; d[x].g = g; d[x].b = b; d[x].a = a;
        }
        return;
    }
    for (uint32_t x = 0; x < w; x++) {
        pc_px32 o;
        switch (f->kind) {
        case K_RGBA8S:
            o.r = snorm8_to_u8((int8_t)s[0]); o.g = snorm8_to_u8((int8_t)s[1]);
            o.b = snorm8_to_u8((int8_t)s[2]); o.a = snorm8_to_u8((int8_t)s[3]);
            s += 4;
            break;
        case K_RG8S:
            o.r = snorm8_to_u8((int8_t)s[0]); o.g = snorm8_to_u8((int8_t)s[1]);
            o.b = 0u; o.a = 255u;
            s += 2;
            break;
        case K_RGBA16:
            o.r = lc_u16_to_u8((uint32_t)s[0] | ((uint32_t)s[1] << 8));
            o.g = lc_u16_to_u8((uint32_t)s[2] | ((uint32_t)s[3] << 8));
            o.b = lc_u16_to_u8((uint32_t)s[4] | ((uint32_t)s[5] << 8));
            o.a = lc_u16_to_u8((uint32_t)s[6] | ((uint32_t)s[7] << 8));
            s += 8;
            break;
        case K_RGBA16F:
            o.r = float_to_u8(half_to_float((uint16_t)(s[0] | (s[1] << 8))));
            o.g = float_to_u8(half_to_float((uint16_t)(s[2] | (s[3] << 8))));
            o.b = float_to_u8(half_to_float((uint16_t)(s[4] | (s[5] << 8))));
            o.a = float_to_u8(half_to_float((uint16_t)(s[6] | (s[7] << 8))));
            s += 8;
            break;
        case K_RGBA32F:
            o.r = float_to_u8(rdf32(s)); o.g = float_to_u8(rdf32(s + 4));
            o.b = float_to_u8(rdf32(s + 8)); o.a = float_to_u8(rdf32(s + 12));
            s += 16;
            break;
        case K_R16F:
            o.r = o.g = o.b = float_to_u8(half_to_float((uint16_t)(s[0] | (s[1] << 8))));
            o.a = 255u;
            s += 2;
            break;
        default:   /* K_R32F */
            o.r = o.g = o.b = float_to_u8(rdf32(s));
            o.a = 255u;
            s += 4;
            break;
        }
        d[x] = o;
    }
}

static uint8_t unpremul(uint32_t c, uint32_t a)
{
    uint32_t v = (c * 255u + a / 2u) / a;
    return (uint8_t)(v > 255u ? 255u : v);
}

/* Decode the 4x4 block at src into px (16 pixels, row-major). */
static void decode_block(const dds_fmt *f, const uint8_t *src, pc_px32 px[16])
{
    uint8_t rgba[64];
    switch (f->kind) {
    case K_BC1: bcdec_bc1(src, rgba, 16); break;
    case K_BC2: bcdec_bc2(src, rgba, 16); break;
    case K_BC3: bcdec_bc3(src, rgba, 16); break;
    case K_BC7: bcdec_bc7(src, rgba, 16); break;
    case K_BC4U: case K_BC4S: {
        uint8_t r[16];
        bcdec_bc4(src, r, 4, f->kind == K_BC4S);
        for (int i = 0; i < 16; i++) {
            uint8_t v = f->kind == K_BC4S ? snorm8_to_u8((int8_t)r[i]) : r[i];
            px[i].r = px[i].g = px[i].b = v;
            px[i].a = 255u;
        }
        return;
    }
    case K_BC5U: case K_BC5S: {
        uint8_t rg[32];
        bcdec_bc5(src, rg, 8, f->kind == K_BC5S);
        for (int i = 0; i < 16; i++) {
            if (f->kind == K_BC5S) {
                px[i].r = snorm8_to_u8((int8_t)rg[2 * i]);
                px[i].g = snorm8_to_u8((int8_t)rg[2 * i + 1]);
            } else {
                px[i].r = rg[2 * i];
                px[i].g = rg[2 * i + 1];
            }
            px[i].b = 0u;
            px[i].a = 255u;
        }
        return;
    }
    default: {   /* BC6H */
        float rgb[48];
        bcdec_bc6h_float(src, rgb, 12, f->kind == K_BC6HS);
        for (int i = 0; i < 16; i++) {
            px[i].r = float_to_u8(rgb[3 * i]);
            px[i].g = float_to_u8(rgb[3 * i + 1]);
            px[i].b = float_to_u8(rgb[3 * i + 2]);
            px[i].a = 255u;
        }
        return;
    }
    }
    for (int i = 0; i < 16; i++) {
        pc_px32 o;
        o.r = rgba[4 * i]; o.g = rgba[4 * i + 1]; o.b = rgba[4 * i + 2]; o.a = rgba[4 * i + 3];
        if (f->rxgb) { o.r = o.a; o.a = 255u; }
        if (f->premul && o.a) {
            o.r = unpremul(o.r, o.a);
            o.g = unpremul(o.g, o.a);
            o.b = unpremul(o.b, o.a);
        }
        px[i] = o;
    }
}

/* Decode one top-level w x h image at data into layer at (ox, oy). band
 * holds w * LC_BAND pixels. */
static pc_status decode_image(pc_doc *d, pc_layer *layer, const dds_fmt *f, const uint8_t *data,
                              uint32_t w, uint32_t h, int32_t ox, int32_t oy, pc_px32 *band)
{
    pc_status st;
    for (int32_t y0 = 0; y0 < (int32_t)h; y0 += LC_BAND) {
        int32_t nb = (int32_t)h - y0 < LC_BAND ? (int32_t)h - y0 : LC_BAND;
        if (is_bc(f->kind)) {
            size_t bw = ((size_t)w + 3u) / 4u, bb = bc_block_bytes(f->kind);
            for (int32_t by = 0; by < nb; by += 4) {
                const uint8_t *row = data + ((size_t)(y0 + by) / 4u) * bw * bb;
                for (size_t bx = 0; bx < bw; bx++) {
                    pc_px32 blk[16];
                    decode_block(f, row + bx * bb, blk);
                    for (int32_t yy = 0; yy < 4 && by + yy < nb; yy++)
                        for (uint32_t xx = 0; xx < 4u && bx * 4u + xx < w; xx++)
                            band[(size_t)(by + yy) * w + bx * 4u + xx] = blk[yy * 4 + (int)xx];
                }
            }
        } else {
            size_t pitch = ((size_t)w * fmt_bpp(f) + 7u) / 8u;
            for (int32_t r = 0; r < nb; r++)
                decode_row_uncompressed(f, data + (size_t)(y0 + r) * pitch, band + (size_t)r * w,
                                        w);
        }
        st = pc_layer_store_rect(d, layer, pc_rect_make(ox, oy + y0, (int32_t)w, nb), band, w);
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

/* Horizontal cross positions (in face units) of the faces in file order
 * +X, -X, +Y, -Y, +Z, -Z:   . +Y .  .
 *                           -X +Z +X -Z
 *                           . -Y .  .                                    */
static const int32_t k_hcross[6][2] = {
    { 2, 1 }, { 0, 1 }, { 1, 0 }, { 1, 2 }, { 1, 1 }, { 3, 1 }
};
/* Vertical cross:  . +Y .  /  -X +Z +X  /  . -Y .  /  . -Z .            */
static const int32_t k_vcross[6][2] = {
    { 2, 1 }, { 0, 1 }, { 1, 0 }, { 1, 2 }, { 1, 1 }, { 1, 3 }
};

static pc_status dds_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    dds_info in;
    pc_doc *d = NULL;
    pc_layer *layer;
    pc_px32 *band = NULL;
    pc_status st;
    bool cross;
    size_t need;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    st = dds_parse(p, n, lim, &in);
    if (st != PC_OK) return st;
    /* A complete cube map loads as a horizontal cross (Paint.NET's DDS
     * file type does the same); anything else loads its first image. */
    cross = in.cube && in.w == in.h && in.w <= PC_MAX_DIM / 4u &&
            pc_mul_size(in.chain, 5u, &need) && pc_add_size(need, in.top_size, &need) &&
            need <= n - in.data_off;
    if (cross) st = lc_doc_new(lim, in.w * 4u, in.h * 3u, 1u, &d, &layer);
    else st = lc_doc_new(lim, in.w, in.h, 1u, &d, &layer);
    if (st != PC_OK) return st;
    band = (pc_px32 *)lc_alloc((size_t)in.w * (size_t)LC_BAND, sizeof *band, lim, &st);
    if (!band) goto fail;
    for (uint32_t f = 0; f < (cross ? 6u : 1u); f++) {
        int32_t ox = cross ? k_hcross[f][0] * (int32_t)in.w : 0;
        int32_t oy = cross ? k_hcross[f][1] * (int32_t)in.h : 0;
        st = decode_image(d, layer, &in.fmt, p + in.data_off + (size_t)f * in.chain, in.w, in.h,
                          ox, oy, band);
        if (st != PC_OK) goto fail;
    }
    free(band);
    meta->src_bits = 8u;
    meta->had_alpha = (in.fmt.kind == K_MASK) ? in.fmt.mask[3] != 0u
                    : !(in.fmt.kind == K_BC4U || in.fmt.kind == K_BC4S || in.fmt.kind == K_BC5U ||
                        in.fmt.kind == K_BC5S || in.fmt.kind == K_BC6HU ||
                        in.fmt.kind == K_BC6HS || in.fmt.kind == K_RG8S ||
                        in.fmt.kind == K_R16F || in.fmt.kind == K_R32F || in.fmt.rxgb);
    if (cross)
        lc_note(meta, "Cube map loaded as a horizontal cross");
    else if (in.layers > 1u || in.depth > 1u)
        lc_note(meta, "Only the first image of the texture array, cube map or volume was loaded");
    *out = d;
    return PC_OK;
fail:
    free(band);
    pc_doc_destroy(d);
    return st;
}

static bool dds_sniff(const uint8_t *p, size_t n)
{
    if (!p || n < 4u || rd32(p) != DDS_MAGIC) return false;
    return n < 8u || rd32(p + 4) == DDS_HDR_SIZE;
}

/* ---- save options --------------------------------------------------------------------- */
typedef struct dds_params {
    int32_t format;       /* index into k_formats */
    int32_t dither;       /* bool: error diffusion (16-bit layouts) */
    int32_t bc7_speed;    /* 0 fast, 1 medium, 2 slow */
    int32_t metric;       /* 0 perceptual, 1 uniform */
    int32_t cube_map;     /* bool: cube map from a horizontal or vertical cross */
    int32_t mipmaps;      /* bool */
    int32_t mip_filter;   /* index into k_filters / k_filter_of */
    int32_t gamma;        /* bool: gamma-corrected mip resampling */
} dds_params;

typedef enum enc_kind {
    E_BC1, E_BC2, E_BC3, E_BC4, E_BC5U, E_BC5S, E_BC6H, E_BC7, E_RXGB,
    E_BGRA8, E_BGRX8, E_RGBA8, E_RGBX8, E_BGR8, E_B5G5R5A1, E_B4G4R4A4, E_B5G6R5,
    E_R8, E_RG8, E_RG8S, E_R32F
} enc_kind;

/* Indexed like k_format_labels. */
typedef struct dds_out_fmt {
    enc_kind    kind;
    uint32_t    dxgi;         /* nonzero: DX10 header with this format */
    const char *cc;           /* else: legacy FourCC, or NULL for bit masks */
} dds_out_fmt;

static const dds_out_fmt k_formats[] = {
    { E_BC1, 0, "DXT1" }, /* BC1 (Linear, DXT1) */
    { E_BC1, 72, NULL }, /* BC1 (sRGB, DX 10+) */
    { E_BC2, 0, "DXT3" }, /* BC2 (Linear, DXT3) */
    { E_BC2, 75, NULL }, /* BC2 (sRGB, DX 10+) */
    { E_BC3, 0, "DXT5" }, /* BC3 (Linear, DXT5) */
    { E_BC3, 78, NULL }, /* BC3 (sRGB, DX 10+) */
    { E_BC4, 80, NULL }, /* BC4 (Linear, Unsigned) */
    { E_BC5U, 83, NULL }, /* BC5 (Linear, Unsigned) */
    { E_BC5S, 84, NULL }, /* BC5 (Linear, Signed) */
    { E_BC6H, 95, NULL }, /* BC6H (Linear, Unsigned, DX 11+) */
    { E_BC7, 98, NULL }, /* BC7 (Linear, DX 11+) */
    { E_BC7, 99, NULL }, /* BC7 (sRGB, DX 11+) */
    { E_BGRA8, 0, NULL }, /* B8G8R8A8 (Linear, A8R8G8B8) */
    { E_BGRA8, 91, NULL }, /* B8G8R8A8 (sRGB, DX 10+) */
    { E_BGRX8, 0, NULL }, /* B8G8R8X8 (Linear, X8R8G8B8) */
    { E_BGRX8, 93, NULL }, /* B8G8R8X8 (sRGB, DX 10+) */
    { E_RGBA8, 0, NULL }, /* R8G8B8A8 (Linear, A8B8G8R8) */
    { E_RGBA8, 29, NULL }, /* R8G8B8A8 (sRGB, DX 10+) */
    { E_B5G5R5A1, 0, NULL }, /* B5G5R5A1 (Linear, A1R5G5B5) */
    { E_B4G4R4A4, 0, NULL }, /* B4G4R4A4 (Linear, A4R4G4B4) */
    { E_B5G6R5, 0, NULL }, /* B5G6R5 (Linear, R5G6B5) */
    { E_R8, 61, NULL }, /* R8 (Unsigned, DX 10+) */
    { E_RG8, 49, NULL }, /* R8G8 (Unsigned, DX 10+) */
    { E_RG8S, 51, NULL }, /* R8G8 (Signed, DX 10+) */
    { E_R32F, 41, NULL }, /* R32 (Float, DX 10+) */
    { E_BGR8, 0, NULL }, /* B8G8R8 (Linear, R8G8B8) */
    { E_RGBX8, 0, NULL }, /* R8G8B8X8 (Linear, X8B8G8R8) */
    { E_BC4, 0, "ATI1" }, /* BC4 (Linear, ATI1) */
    { E_BC5U, 0, "ATI2" }, /* BC5 (Linear, ATI2) */
    { E_RXGB, 0, "RXGB" }, /* BC3 (Linear, RXGB) */
};
#define N_FORMATS ((int)(sizeof k_formats / sizeof k_formats[0]))

static const char *const k_format_labels[] = {
    "BC1 (Linear, DXT1)", "BC1 (sRGB, DX 10+)", "BC2 (Linear, DXT3)", "BC2 (sRGB, DX 10+)",
    "BC3 (Linear, DXT5)", "BC3 (sRGB, DX 10+)", "BC4 (Linear, Unsigned)",
    "BC5 (Linear, Unsigned)", "BC5 (Linear, Signed)", "BC6H (Linear, Unsigned, DX 11+)",
    "BC7 (Linear, DX 11+)",
    "BC7 (sRGB, DX 11+)", "B8G8R8A8 (Linear, A8R8G8B8)", "B8G8R8A8 (sRGB, DX 10+)",
    "B8G8R8X8 (Linear, X8R8G8B8)", "B8G8R8X8 (sRGB, DX 10+)", "R8G8B8A8 (Linear, A8B8G8R8)",
    "R8G8B8A8 (sRGB, DX 10+)", "B5G5R5A1 (Linear, A1R5G5B5)", "B4G4R4A4 (Linear, A4R4G4B4)",
    "B5G6R5 (Linear, R5G6B5)", "R8 (Unsigned, DX 10+)", "R8G8 (Unsigned, DX 10+)",
    "R8G8 (Signed, DX 10+)", "R32 (Float, DX 10+)", "B8G8R8 (Linear, R8G8B8)",
    "R8G8B8X8 (Linear, X8B8G8R8)", "BC4 (Linear, ATI1)", "BC5 (Linear, ATI2)",
    "BC3 (Linear, RXGB)", NULL
};

_Static_assert(sizeof k_format_labels / sizeof k_format_labels[0] == (size_t)N_FORMATS + 1u,
               "k_formats and k_format_labels must stay aligned");

static const char *const k_speed[] = { "Fast", "Medium", "Slow", NULL };
static const char *const k_metric[] = { "Perceptual", "Uniform", NULL };
/* Mip map resampling choices (FILES.md order) and their filters. */
static const char *const k_filters[] = {
    "Bicubic", "Bicubic (Smooth)", "Bilinear", "Bilinear (Low Quality)", "Adaptive", "Lanczos",
    "Fant", "Nearest Neighbor", NULL
};
static const lc_filter k_filter_of[] = {
    LC_FILTER_BICUBIC, LC_FILTER_BICUBIC_SMOOTH, LC_FILTER_BILINEAR, LC_FILTER_BILINEAR_LOW,
    LC_FILTER_ADAPTIVE, LC_FILTER_LANCZOS, LC_FILTER_FANT, LC_FILTER_NEAREST
};
#define N_FILTERS ((int)(sizeof k_filter_of / sizeof k_filter_of[0]))
_Static_assert(sizeof k_filters / sizeof k_filters[0] == (size_t)N_FILTERS + 1u,
               "k_filters and k_filter_of must stay aligned");

/* Formats the options apply to (k_formats indices): BC1..BC3 variants
 * (with RXGB), the 16-bit layouts, BC6H and BC7. */
#define DDS_IF_DITHER "format=0|1|2|3|4|5|18|19|20|29"
#define DDS_IF_SPEED  "format=9|10|11"
#define DDS_IF_METRIC "format=0|1|2|3|4|5|29"

static const fx_prop k_dds_props[] = {
    { "format", "DDS format", FXP_CHOICE, (uint32_t)offsetof(dds_params, format),
      0, N_FORMATS - 1, 0, 0, k_format_labels, NULL, 0, 0, NULL },
    { "dither", "Error diffusion dithering", FXP_BOOL, (uint32_t)offsetof(dds_params, dither),
      0, 1, 1, 0, NULL, NULL, 0, 0, DDS_IF_DITHER },
    { "bc7_speed", "BC6H / BC7 compression speed", FXP_CHOICE,
      (uint32_t)offsetof(dds_params, bc7_speed), 0, 2, 1, 0, k_speed, NULL, 0, 0, DDS_IF_SPEED },
    { "metric", "Error metric", FXP_CHOICE, (uint32_t)offsetof(dds_params, metric),
      0, 1, 0, 0, k_metric, NULL, 0, 0, DDS_IF_METRIC },
    { "cube_map", "Cube map from crossed image", FXP_BOOL,
      (uint32_t)offsetof(dds_params, cube_map), 0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
    { "mipmaps", "Generate mip maps", FXP_BOOL, (uint32_t)offsetof(dds_params, mipmaps),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
    { "mip_filter", "Mip map resampling", FXP_CHOICE, (uint32_t)offsetof(dds_params, mip_filter),
      0, N_FILTERS - 1, 0, 0, k_filters, NULL, 0, 0, "mipmaps" },
    { "gamma", "Use gamma correction", FXP_BOOL, (uint32_t)offsetof(dds_params, gamma),
      0, 1, 1, 0, NULL, NULL, 0, 0, "mipmaps" },
};

/* ---- block encoders ------------------------------------------------------------------- */
static pc_atomic_u32 g_bc7_ticket, g_bc7_ready;

static void bc7_init_once(void)
{
    if (pc_atomic_load(&g_bc7_ready)) return;
    if (pc_atomic_inc(&g_bc7_ticket) == 1u) {
        pc_bc7enc_init();
        pc_atomic_store(&g_bc7_ready, 1u);
    } else {
        while (!pc_atomic_load(&g_bc7_ready)) { /* another thread builds the tables */ }
    }
}

typedef struct rgb_w { int r, g, b; } rgb_w;    /* error weights (sum 256-ish) */

static void rgb565_expand(uint32_t c, int out[3])
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = (int)((r << 3) | (r >> 2));
    out[1] = (int)((g << 2) | (g >> 4));
    out[2] = (int)((b << 3) | (b >> 2));
}

static int wdist(const int a[3], const uint8_t *p, const rgb_w *w)
{
    int dr = a[0] - p[0], dg = a[1] - p[1], db = a[2] - p[2];
    return w->r * dr * dr + w->g * dg * dg + w->b * db * db;
}

/* Re-pick the 2-bit indices of an encoded color block for the given
 * weights. three_color: BC1 punch-through mode (index 3 = transparent).
 * With dither the remaining error of each pixel is diffused (Floyd-
 * Steinberg, raster order) to its neighbors inside the block. */
static void reselect_indices(uint8_t *blk, const uint8_t rgba[64], const rgb_w *w,
                             bool three_color, uint32_t transparent_mask, bool dither)
{
    int err[16][3];
    uint32_t c0 = (uint32_t)blk[0] | ((uint32_t)blk[1] << 8);
    uint32_t c1 = (uint32_t)blk[2] | ((uint32_t)blk[3] << 8);
    int pal[4][3], e0[3], e1[3], np = three_color ? 3 : 4;
    uint32_t idx = 0;
    rgb565_expand(c0, e0);
    rgb565_expand(c1, e1);
    for (int c = 0; c < 3; c++) {
        pal[0][c] = e0[c];
        pal[1][c] = e1[c];
        if (three_color) {
            pal[2][c] = (e0[c] + e1[c]) / 2;
        } else {
            pal[2][c] = (2 * e0[c] + e1[c]) / 3;
            pal[3][c] = (e0[c] + 2 * e1[c]) / 3;
        }
    }
    memset(err, 0, sizeof err);
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 0x7FFFFFFF, x = i & 3, y = i >> 2;
        uint8_t t[3];
        if (transparent_mask & (1u << i)) { idx |= 3u << (2 * i); continue; }
        for (int c = 0; c < 3; c++) {      /* error in 1/16 units */
            int v = (int)rgba[4 * i + c] + (err[i][c] >= 0 ? err[i][c] + 8 : err[i][c] - 8) / 16;
            t[c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        for (int k = 0; k < np; k++) {
            int dd = wdist(pal[k], t, w);
            if (dd < bd) { bd = dd; best = k; }
        }
        idx |= (uint32_t)best << (2 * i);
        if (dither)
            for (int c = 0; c < 3; c++) {
                int e = (int)t[c] - pal[best][c];
                if (x < 3) err[i + 1][c] += 7 * e;
                if (y < 3 && x > 0) err[i + 3][c] += 3 * e;
                if (y < 3) err[i + 4][c] += 5 * e;
                if (y < 3 && x < 3) err[i + 5][c] += e;
            }
    }
    blk[4] = (uint8_t)idx; blk[5] = (uint8_t)(idx >> 8);
    blk[6] = (uint8_t)(idx >> 16); blk[7] = (uint8_t)(idx >> 24);
}

/* Error diffusion of the block's colors to 5:6:5 precision (in place, raster
 * order inside the block), so the endpoints are fitted to colors whose local
 * averages match the source (pixels in skip keep their colors). */
static void dither_565(uint8_t rgba[64], uint32_t skip)
{
    static const int k_bits[3] = { 5, 6, 5 };
    int err[16][3];
    memset(err, 0, sizeof err);
    for (int i = 0; i < 16; i++) {
        int x = i & 3, y = i >> 2;
        if (skip & (1u << i)) continue;
        for (int c = 0; c < 3; c++) {
            int max = (1 << k_bits[c]) - 1;
            int v = (int)rgba[4 * i + c] + (err[i][c] >= 0 ? err[i][c] + 8 : err[i][c] - 8) / 16;
            int q, e;
            v = v < 0 ? 0 : (v > 255 ? 255 : v);
            q = (v * max + 127) / 255;
            q = k_bits[c] == 6 ? (q << 2) | (q >> 4) : (q << 3) | (q >> 2);
            e = v - q;
            rgba[4 * i + c] = (uint8_t)q;
            if (x < 3) err[i + 1][c] += 7 * e;
            if (y < 3 && x > 0) err[i + 3][c] += 3 * e;
            if (y < 3) err[i + 4][c] += 5 * e;
            if (y < 3 && x < 3) err[i + 5][c] += e;
        }
    }
}

static void enc_bc1(uint8_t out[8], const uint8_t rgba[64], const rgb_w *w, bool perceptual,
                    bool dither)
{
    uint8_t tmp[64];
    uint32_t tmask = 0;
    int first_opaque = -1;
    for (int i = 0; i < 16; i++) {
        if (rgba[4 * i + 3] < 128u) tmask |= 1u << i;
        else if (first_opaque < 0) first_opaque = i;
    }
    if (tmask == 0xFFFFu) {                      /* fully transparent block */
        memset(out, 0, 4);
        out[4] = out[5] = out[6] = out[7] = 0xFF;
        return;
    }
    memcpy(tmp, rgba, 64);
    for (int i = 0; i < 16; i++) {
        tmp[4 * i + 3] = 255u;
        if (tmask & (1u << i)) memcpy(tmp + 4 * i, rgba + 4 * first_opaque, 3);
    }
    if (dither) {
        uint8_t fit[64];
        memcpy(fit, tmp, 64);
        dither_565(fit, tmask);
        stb_compress_dxt_block(out, fit, 0, STB_DXT_HIGHQUAL);
    } else {
        stb_compress_dxt_block(out, tmp, 0, STB_DXT_HIGHQUAL);
    }
    if (tmask) {
        /* punch-through: order the endpoints so color0 <= color1 */
        uint32_t c0 = (uint32_t)out[0] | ((uint32_t)out[1] << 8);
        uint32_t c1 = (uint32_t)out[2] | ((uint32_t)out[3] << 8);
        if (c0 > c1) {
            out[0] = (uint8_t)c1; out[1] = (uint8_t)(c1 >> 8);
            out[2] = (uint8_t)c0; out[3] = (uint8_t)(c0 >> 8);
        }
        reselect_indices(out, tmp, w, true, tmask, dither);
    } else if (perceptual || dither) {
        uint32_t c0 = (uint32_t)out[0] | ((uint32_t)out[1] << 8);
        uint32_t c1 = (uint32_t)out[2] | ((uint32_t)out[3] << 8);
        if (c0 > c1) reselect_indices(out, tmp, w, false, 0u, dither);
    }
}

static void enc_color_opaque(uint8_t out[8], const uint8_t rgba[64], const rgb_w *w,
                             bool perceptual, bool dither)
{
    uint8_t tmp[64], fit[64];
    memcpy(tmp, rgba, 64);
    for (int i = 0; i < 16; i++) tmp[4 * i + 3] = 255u;
    memcpy(fit, tmp, 64);
    if (dither) dither_565(fit, 0u);
    stb_compress_dxt_block(out, fit, 0, STB_DXT_HIGHQUAL);
    if (perceptual || dither) {
        uint32_t c0 = (uint32_t)out[0] | ((uint32_t)out[1] << 8);
        uint32_t c1 = (uint32_t)out[2] | ((uint32_t)out[3] << 8);
        if (c0 != c1) reselect_indices(out, tmp, w, false, 0u, dither);
    }
}

/* BC2 explicit 4-bit alpha, optionally with error diffusion in the block. */
static void enc_bc2_alpha(uint8_t out[8], const uint8_t rgba[64], bool dither)
{
    int err[16];
    memset(err, 0, sizeof err);
    memset(out, 0, 8);
    for (int i = 0; i < 16; i++) {
        int x = i & 3, y = i >> 2;
        int v = (int)rgba[4 * i + 3] + (err[i] >= 0 ? err[i] + 8 : err[i] - 8) / 16;
        int q;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        q = (v * 15 + 127) / 255;
        out[i / 2] |= (uint8_t)(q << (4 * (i & 1)));
        if (dither) {
            int e = v - q * 17;
            if (x < 3) err[i + 1] += 7 * e;
            if (y < 3 && x > 0) err[i + 3] += 3 * e;
            if (y < 3) err[i + 4] += 5 * e;
            if (y < 3 && x < 3) err[i + 5] += e;
        }
    }
}

/* BC4 signed through the unsigned encoder on values shifted by +128. */
static void enc_bc4_signed(uint8_t out[8], const uint8_t s_plus_128[16])
{
    stb_compress_bc4_block(out, s_plus_128);
    for (int k = 0; k < 2; k++) {
        int v = (int)out[k] - 128;
        if (v < -127) v = -127;
        out[k] = (uint8_t)(int8_t)v;
    }
}

static uint8_t u8_to_snorm_biased(uint8_t u)   /* snorm + 128, in 1..255 */
{
    int s = (int)floor((double)u * 254.0 / 255.0 - 127.0 + 0.5);
    if (s < -127) s = -127;
    if (s > 127) s = 127;
    return (uint8_t)(s + 128);
}

/* One level of a block format: every block row is a job (pc_par), writing
 * its blocks into the reserved output at a fixed offset. */
typedef struct bc_job {
    const pc_px32     *px;
    int32_t            w, h;
    size_t             bw;
    uint32_t           bsz;
    const dds_out_fmt *of;
    const dds_params  *prm;
    rgb_w              wt;
    bool               perceptual, dither;
    uint16_t           half[256];      /* BC6H: 8-bit level to half float */
    uint8_t           *dst;            /* first block of the level */
} bc_job;

static void enc_block(const bc_job *j, size_t bx, size_t by, uint8_t blk[16])
{
    uint8_t rgba[64];
    for (int i = 0; i < 16; i++) {
        int32_t x = (int32_t)(bx * 4u) + (i & 3), y = (int32_t)(by * 4u) + (i >> 2);
        pc_px32 p;
        if (x >= j->w) x = j->w - 1;
        if (y >= j->h) y = j->h - 1;
        p = j->px[(size_t)y * (size_t)j->w + (size_t)x];
        rgba[4 * i] = p.r; rgba[4 * i + 1] = p.g; rgba[4 * i + 2] = p.b;
        rgba[4 * i + 3] = p.a;
    }
    switch (j->of->kind) {
    case E_BC1: enc_bc1(blk, rgba, &j->wt, j->perceptual, j->dither); break;
    case E_BC2:
        enc_bc2_alpha(blk, rgba, j->dither);
        enc_color_opaque(blk + 8, rgba, &j->wt, j->perceptual, j->dither);
        break;
    case E_BC3: case E_RXGB: {
        if (j->of->kind == E_RXGB)
            for (int i = 0; i < 16; i++) { rgba[4 * i + 3] = rgba[4 * i]; rgba[4 * i] = 0; }
        if (j->dither) {
            uint8_t fit[64];
            memcpy(fit, rgba, 64);
            dither_565(fit, 0u);
            stb_compress_dxt_block(blk, fit, 1, STB_DXT_HIGHQUAL);
        } else {
            stb_compress_dxt_block(blk, rgba, 1, STB_DXT_HIGHQUAL);
        }
        if (j->perceptual || j->dither) {
            uint32_t c0 = (uint32_t)blk[8] | ((uint32_t)blk[9] << 8);
            uint32_t c1 = (uint32_t)blk[10] | ((uint32_t)blk[11] << 8);
            if (c0 != c1) reselect_indices(blk + 8, rgba, &j->wt, false, 0u, j->dither);
        }
        break;
    }
    case E_BC6H: {
        uint16_t rgb[48];
        for (int i = 0; i < 16; i++) {
            rgb[3 * i] = j->half[rgba[4 * i]];
            rgb[3 * i + 1] = j->half[rgba[4 * i + 1]];
            rgb[3 * i + 2] = j->half[rgba[4 * i + 2]];
        }
        bc6h_encode_block(blk, rgb, j->prm->bc7_speed);
        break;
    }
    case E_BC4: {
        uint8_t r[16];
        for (int i = 0; i < 16; i++) r[i] = rgba[4 * i];
        stb_compress_bc4_block(blk, r);
        break;
    }
    case E_BC5U: {
        uint8_t rg[32];
        for (int i = 0; i < 16; i++) {
            rg[2 * i] = rgba[4 * i];
            rg[2 * i + 1] = rgba[4 * i + 1];
        }
        stb_compress_bc5_block(blk, rg);
        break;
    }
    case E_BC5S: {
        uint8_t r[16], g[16];
        for (int i = 0; i < 16; i++) {
            r[i] = u8_to_snorm_biased(rgba[4 * i]);
            g[i] = u8_to_snorm_biased(rgba[4 * i + 1]);
        }
        enc_bc4_signed(blk, r);
        enc_bc4_signed(blk + 8, g);
        break;
    }
    default:   /* E_BC7 */
        pc_bc7enc_block(blk, rgba, j->prm->bc7_speed, j->perceptual);
        break;
    }
}

static void bc_row_job(void *ud, uint32_t index, uint32_t worker)
{
    const bc_job *j = (const bc_job *)ud;
    uint8_t *row = j->dst + (size_t)index * j->bw * j->bsz;
    (void)worker;
    for (size_t bx = 0; bx < j->bw; bx++) {
        uint8_t blk[16];
        enc_block(j, bx, index, blk);
        memcpy(row + bx * j->bsz, blk, j->bsz);
    }
}

/* Encode one level (w x h, contiguous BGRA) of a block format; block rows
 * run on par (may be NULL); the bytes do not depend on the thread count. */
static pc_status enc_level_bc(pc_buf *out, const pc_px32 *px, int32_t w, int32_t h,
                              const dds_out_fmt *of, const dds_params *prm, const pc_par *par)
{
    bc_job *j;
    size_t bh = ((size_t)h + 3u) / 4u, need;
    j = (bc_job *)calloc(1u, sizeof *j);
    if (!j) return PC_ERR_NOMEM;
    j->px = px;
    j->w = w;
    j->h = h;
    j->bw = ((size_t)w + 3u) / 4u;
    j->bsz = (of->kind == E_BC1 || of->kind == E_BC4) ? 8u : 16u;
    j->of = of;
    j->prm = prm;
    j->perceptual = prm->metric == 0;
    j->dither = prm->dither != 0;
    if (j->perceptual) { j->wt.r = 54; j->wt.g = 183; j->wt.b = 19; }
    else { j->wt.r = j->wt.g = j->wt.b = 85; }
    if (!pc_mul_size(j->bw * bh, j->bsz, &need) || bh > UINT32_MAX) { free(j); return PC_ERR_LIMIT; }
    if (pc_buf_reserve(out, need) != PC_OK) { free(j); return PC_ERR_NOMEM; }
    if (of->kind == E_BC7) bc7_init_once();
    if (of->kind == E_BC6H)
        for (int v = 0; v < 256; v++) j->half[v] = bc6h_float_to_half((float)v / 255.0f);
    j->dst = out->p + out->n;
    pc_par_for(par, bc_row_job, j, (uint32_t)bh);
    out->n += need;
    free(j);
    return PC_OK;
}

static uint32_t pack_bits(int v, int bits)    /* 8-bit value to bits with rounding */
{
    int max = (1 << bits) - 1;
    return (uint32_t)((v * max + 127) / 255);
}

/* Encode one level of an uncompressed format. Error diffusion (Floyd-
 * Steinberg) applies to the 16-bit layouts when requested. */
static pc_status enc_level_plain(pc_buf *out, const pc_px32 *px, int32_t w, int32_t h,
                                 const dds_out_fmt *of, bool dither)
{
    size_t bpp, need;
    float *err = NULL;
    bool fs = dither && (of->kind == E_B5G5R5A1 || of->kind == E_B4G4R4A4 ||
                         of->kind == E_B5G6R5);
    int bits[4] = { 8, 8, 8, 8 };
    switch (of->kind) {
    case E_BGRA8: case E_BGRX8: case E_RGBA8: case E_RGBX8: case E_R32F: bpp = 4; break;
    case E_BGR8: bpp = 3; break;
    case E_R8: bpp = 1; break;
    default: bpp = 2; break;
    }
    if (of->kind == E_B5G5R5A1) { bits[0] = bits[1] = bits[2] = 5; bits[3] = 1; }
    if (of->kind == E_B4G4R4A4) { bits[0] = bits[1] = bits[2] = bits[3] = 4; }
    if (of->kind == E_B5G6R5) { bits[0] = 5; bits[1] = 6; bits[2] = 5; bits[3] = 0; }
    if (!pc_mul_size((size_t)w * (size_t)h, bpp, &need)) return PC_ERR_LIMIT;
    if (pc_buf_reserve(out, need) != PC_OK) return PC_ERR_NOMEM;
    if (fs) {
        err = (float *)calloc(((size_t)w + 2u) * 2u * 4u, sizeof *err);
        if (!err) return PC_ERR_NOMEM;
    }
    for (int32_t y = 0; y < h; y++) {
        float *cur = err ? err + (size_t)((y & 1) ? 0 : 1) * ((size_t)w + 2u) * 4u : NULL;
        float *nxt = err ? err + (size_t)((y & 1) ? 1 : 0) * ((size_t)w + 2u) * 4u : NULL;
        if (nxt) memset(nxt, 0, ((size_t)w + 2u) * 4u * sizeof *nxt);
        for (int32_t x = 0; x < w; x++) {
            pc_px32 p = px[(size_t)y * (size_t)w + (size_t)x];
            uint8_t *o = out->p + out->n;
            int v[4];
            v[0] = p.r; v[1] = p.g; v[2] = p.b; v[3] = p.a;
            if (fs) {
                uint32_t q[4];
                for (int c = 0; c < 4; c++) {
                    float want, got, e;
                    int maxq;
                    if (bits[c] == 0) { q[c] = 0; continue; }
                    maxq = (1 << bits[c]) - 1;
                    want = (float)v[c] + cur[(size_t)(x + 1) * 4u + (size_t)c];
                    if (want < 0.0f) want = 0.0f;
                    if (want > 255.0f) want = 255.0f;
                    q[c] = (uint32_t)(want * (float)maxq / 255.0f + 0.5f);
                    got = (float)q[c] * 255.0f / (float)maxq;
                    e = want - got;
                    cur[(size_t)(x + 2) * 4u + (size_t)c] += e * (7.0f / 16.0f);
                    nxt[(size_t)x * 4u + (size_t)c] += e * (3.0f / 16.0f);
                    nxt[(size_t)(x + 1) * 4u + (size_t)c] += e * (5.0f / 16.0f);
                    nxt[(size_t)(x + 2) * 4u + (size_t)c] += e * (1.0f / 16.0f);
                }
                for (int c = 0; c < 4; c++) v[c] = (int)q[c];
            } else if (bits[0] != 8) {
                for (int c = 0; c < 4; c++) v[c] = bits[c] ? (int)pack_bits(v[c], bits[c]) : 0;
            }
            switch (of->kind) {
            case E_BGRA8: o[0] = p.b; o[1] = p.g; o[2] = p.r; o[3] = p.a; break;
            case E_BGRX8: o[0] = p.b; o[1] = p.g; o[2] = p.r; o[3] = 255u; break;
            case E_RGBA8: o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a; break;
            case E_RGBX8: o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = 255u; break;
            case E_BGR8: o[0] = p.b; o[1] = p.g; o[2] = p.r; break;
            case E_R8: o[0] = p.r; break;
            case E_RG8: o[0] = p.r; o[1] = p.g; break;
            case E_RG8S:
                o[0] = (uint8_t)(int8_t)((int)u8_to_snorm_biased(p.r) - 128);
                o[1] = (uint8_t)(int8_t)((int)u8_to_snorm_biased(p.g) - 128);
                break;
            case E_R32F: {
                float f = (float)p.r / 255.0f;
                uint32_t u;
                memcpy(&u, &f, sizeof u);
                o[0] = (uint8_t)u; o[1] = (uint8_t)(u >> 8);
                o[2] = (uint8_t)(u >> 16); o[3] = (uint8_t)(u >> 24);
                break;
            }
            case E_B5G5R5A1: {
                uint32_t u = ((uint32_t)v[3] << 15) | ((uint32_t)v[0] << 10) |
                             ((uint32_t)v[1] << 5) | (uint32_t)v[2];
                o[0] = (uint8_t)u; o[1] = (uint8_t)(u >> 8);
                break;
            }
            case E_B4G4R4A4: {
                uint32_t u = ((uint32_t)v[3] << 12) | ((uint32_t)v[0] << 8) |
                             ((uint32_t)v[1] << 4) | (uint32_t)v[2];
                o[0] = (uint8_t)u; o[1] = (uint8_t)(u >> 8);
                break;
            }
            default: {   /* E_B5G6R5 */
                uint32_t u = ((uint32_t)v[0] << 11) | ((uint32_t)v[1] << 5) | (uint32_t)v[2];
                o[0] = (uint8_t)u; o[1] = (uint8_t)(u >> 8);
                break;
            }
            }
            out->n += bpp;
        }
    }
    free(err);
    return PC_OK;
}

/* ---- header writer ---------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool enc_is_bc(enc_kind k)
{
    return k == E_BC1 || k == E_BC2 || k == E_BC3 || k == E_BC4 || k == E_BC5U ||
           k == E_BC5S || k == E_BC6H || k == E_BC7 || k == E_RXGB;
}

static pc_status write_header(pc_buf *out, const dds_out_fmt *of, uint32_t w, uint32_t h,
                              uint32_t mips, bool cube)
{
    uint8_t hdr[4 + DDS_HDR_SIZE + DDS_DX10_SIZE];
    uint8_t *pf = hdr + 76;
    size_t len = 4 + DDS_HDR_SIZE;
    bool bc = enc_is_bc(of->kind);
    size_t top;
    uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    memset(hdr, 0, sizeof hdr);
    put32(hdr, DDS_MAGIC);
    put32(hdr + 4, DDS_HDR_SIZE);
    if (bc) {
        uint32_t bsz = (of->kind == E_BC1 || of->kind == E_BC4) ? 8u : 16u;
        size_t bw = ((size_t)w + 3u) / 4u, bh = ((size_t)h + 3u) / 4u;
        top = bw * bh * bsz;
        flags |= DDSD_LINEARSIZE;
    } else {
        uint32_t bpp = 32u;
        if (of->kind == E_BGR8) bpp = 24u;
        else if (of->kind == E_R8) bpp = 8u;
        else if (of->kind == E_B5G5R5A1 || of->kind == E_B4G4R4A4 || of->kind == E_B5G6R5 ||
                 of->kind == E_RG8 || of->kind == E_RG8S) bpp = 16u;
        top = ((size_t)w * bpp + 7u) / 8u;
        flags |= DDSD_PITCH;
    }
    if (mips > 1u) flags |= DDSD_MIPMAPCOUNT;
    put32(hdr + 8, flags);
    put32(hdr + 12, h);
    put32(hdr + 16, w);
    put32(hdr + 20, top > 0xFFFFFFFFu ? 0u : (uint32_t)top);
    put32(hdr + 28, mips);
    put32(pf, DDS_PF_SIZE);
    put32(hdr + 108, DDSCAPS_TEXTURE | (mips > 1u ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0u) |
                     (cube ? DDSCAPS_COMPLEX : 0u));
    if (cube) put32(hdr + 112, DDSCAPS2_CUBEMAP | DDSCAPS2_FACES);
    if (of->dxgi) {
        put32(pf + 4, DDPF_FOURCC);
        put32(pf + 8, fourcc("DX10"));
        put32(hdr + 128, of->dxgi);
        put32(hdr + 132, DX10_DIM_TEX2D);
        put32(hdr + 136, cube ? DX10_MISC_CUBE : 0u);
        put32(hdr + 140, 1u);
        put32(hdr + 144, 0u);
        len += DDS_DX10_SIZE;
    } else if (of->cc) {
        put32(pf + 4, DDPF_FOURCC);
        put32(pf + 8, fourcc(of->cc));
    } else {
        uint32_t f = DDPF_RGB, bits = 32, r = 0, g = 0, b = 0, a = 0;
        switch (of->kind) {
        case E_BGRA8: r = 0xFF0000u; g = 0xFF00u; b = 0xFFu; a = 0xFF000000u; break;
        case E_BGRX8: r = 0xFF0000u; g = 0xFF00u; b = 0xFFu; break;
        case E_RGBA8: r = 0xFFu; g = 0xFF00u; b = 0xFF0000u; a = 0xFF000000u; break;
        case E_RGBX8: r = 0xFFu; g = 0xFF00u; b = 0xFF0000u; break;
        case E_BGR8: bits = 24; r = 0xFF0000u; g = 0xFF00u; b = 0xFFu; break;
        case E_B5G5R5A1: bits = 16; r = 0x7C00u; g = 0x3E0u; b = 0x1Fu; a = 0x8000u; break;
        case E_B4G4R4A4: bits = 16; r = 0xF00u; g = 0xF0u; b = 0xFu; a = 0xF000u; break;
        default: bits = 16; r = 0xF800u; g = 0x7E0u; b = 0x1Fu; break;    /* E_B5G6R5 */
        }
        if (a) f |= DDPF_ALPHAPIXELS;
        put32(pf + 4, f);
        put32(pf + 12, bits);
        put32(pf + 16, r); put32(pf + 20, g); put32(pf + 24, b); put32(pf + 28, a);
    }
    return pc_buf_append(out, hdr, len);
}

/* ---- save --------------------------------------------------------------------------------- */
/* Encode cur and its mip chain (mips levels). Consumes *cur. */
static pc_status encode_chain(pc_buf *out, pc_surf *cur, uint32_t mips, const dds_out_fmt *of,
                              const dds_params *prm, const pc_par *par)
{
    pc_surf next;
    pc_status st = PC_OK;
    for (uint32_t level = 0; level < mips && st == PC_OK; level++) {
        if (enc_is_bc(of->kind)) st = enc_level_bc(out, cur->px, cur->w, cur->h, of, prm, par);
        else st = enc_level_plain(out, cur->px, cur->w, cur->h, of, prm->dither != 0);
        if (st != PC_OK || level + 1u == mips) break;
        {
            int32_t nw = cur->w > 1 ? cur->w / 2 : 1, nh = cur->h > 1 ? cur->h / 2 : 1;
            st = pc_surf_alloc(&next, nw, nh);
            if (st != PC_OK) break;
            st = lc_resample(cur->w, cur->h, lc_src_surf, cur, next.px, nw, nh,
                             (size_t)next.stride, k_filter_of[prm->mip_filter], prm->gamma != 0);
            pc_surf_free(cur);
            *cur = next;
        }
    }
    pc_surf_free(cur);
    return st;
}

static uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t m = w > h ? w : h, n = 1;
    while (m > 1u) { m >>= 1; n++; }
    return n;
}

static pc_status dds_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    dds_params prm;
    const dds_out_fmt *of;
    pc_surf full;
    pc_status st;
    size_t n0;
    (void)meta;
    if (!d || !out) return PC_ERR_ARG;
    prm.format = 0; prm.dither = 1; prm.bc7_speed = 1; prm.metric = 0; prm.cube_map = 0;
    prm.mipmaps = 0; prm.mip_filter = 0; prm.gamma = 1;
    if (params) memcpy(&prm, params, sizeof prm);
    if (prm.format < 0 || prm.format >= N_FORMATS) return PC_ERR_ARG;
    if (prm.mip_filter < 0 || prm.mip_filter >= N_FILTERS) prm.mip_filter = 0;
    if (prm.bc7_speed < 0) prm.bc7_speed = 0;
    if (prm.bc7_speed > 2) prm.bc7_speed = 2;
    of = &k_formats[prm.format];
    if (prm.cube_map && !((d->w % 4u == 0u && d->h % 3u == 0u && d->w / 4u == d->h / 3u) ||
                          (d->w % 3u == 0u && d->h % 4u == 0u && d->w / 3u == d->h / 4u)))
        return PC_ERR_ARG;      /* not a 4:3 or 3:4 cross of square faces */
    st = pc_surf_alloc(&full, (int32_t)d->w, (int32_t)d->h);
    if (st != PC_OK) return st;
    st = pc_comp_rect(d, pc_doc_rect(d), full.px, (size_t)full.stride, par);
    if (st != PC_OK) { pc_surf_free(&full); return st; }
    n0 = out->n;
    if (!prm.cube_map) {
        uint32_t mips = prm.mipmaps ? mip_count(d->w, d->h) : 1u;
        st = write_header(out, of, d->w, d->h, mips, false);
        if (st == PC_OK) st = encode_chain(out, &full, mips, of, &prm, par);
        else pc_surf_free(&full);
    } else {
        bool horizontal = d->w / 4u * 3u == d->h && d->w % 4u == 0u;
        int32_t fs = (int32_t)(horizontal ? d->w / 4u : d->w / 3u);
        uint32_t mips = prm.mipmaps ? mip_count((uint32_t)fs, (uint32_t)fs) : 1u;
        st = write_header(out, of, (uint32_t)fs, (uint32_t)fs, mips, true);
        for (int f = 0; f < 6 && st == PC_OK; f++) {
            const int32_t *pos = horizontal ? k_hcross[f] : k_vcross[f];
            pc_surf face;
            st = pc_surf_alloc(&face, fs, fs);
            if (st != PC_OK) break;
            for (int32_t y = 0; y < fs; y++)
                memcpy(pc_surf_row(&face, y),
                       pc_surf_row(&full, pos[1] * fs + y) + (size_t)(pos[0] * fs),
                       (size_t)fs * sizeof(pc_px32));
            st = encode_chain(out, &face, mips, of, &prm, par);
        }
        pc_surf_free(&full);
    }
    if (st != PC_OK) out->n = n0;
    return st;
}

const pc_codec pc_codec_dds = {
    "dds", "Direct Draw Surface", "dds", PC_CODEC_LOAD | PC_CODEC_SAVE,
    dds_sniff, dds_load,
    k_dds_props, (uint32_t)(sizeof k_dds_props / sizeof k_dds_props[0]),
    (uint32_t)sizeof(dds_params),
    dds_save
};
