/* fmt_avif.c - AV1 Image File Format through libavif (lane AVIFJXL, ADR-019).
 *
 * Built against libavif >= 1.0 when PC_HAVE_AVIF is defined (cmake/PcAvifJxl.cmake:
 * system package or the pinned bundled build with libaom). Without it the
 * codec stays registered with no load/save flags, so the app hides the
 * file type instead of offering one that fails.
 *
 * Load (FL-FIRSTFRAME, FL-ICC, FL-META):
 *  - Limits: libavif's own size and dimension limits are tightened to the
 *    caller's pc_codec_limits, and the size, the decoded YUV planes and the
 *    document are checked against them after parsing the container and
 *    before any AV1 data is decoded (P-08).
 *  - The primary item is decoded; a file that only has an image sequence
 *    track yields its first frame (meta.note says so).
 *  - The decoded YUV is converted to BGRA in bands of LC_BAND rows (views
 *    into the decoder's planes), so no full-size RGBA copy exists.
 *  - Transformative properties are applied in MIAF order: clean aperture
 *    crop, then rotation (irot, anti-clockwise), then mirror (imir).
 *  - Color: an ICC profile goes to meta.icc. Without one, CICP values that
 *    mean sRGB keep the pixels untagged; other SDR primaries or transfer
 *    curves get an equivalent ICC profile made with Little-CMS; PQ and HLG
 *    (HDR) are tone mapped to 8-bit sRGB (axj_hdr_to_srgb8). 10 and 12 bit
 *    images are rounded to 8 bits.
 *  - Premultiplied alpha ('prem') is unpremultiplied by libavif.
 *  - Exif and XMP become meta items "exif" and "xmp" (avifjxl_meta.h);
 *    Exif resolution fills meta.dpi. A primary item that is an image grid
 *    stores its layout as "avif.grid" = "cols,rows,tile_w,tile_h" for the
 *    "Preserve existing tile size" save option.
 *
 * Save (options of the AV1 (AVIF) file type bundled with Paint.NET 5.1,
 * docs/inventory/FILES.md): Quality 0..100 (85), Lossless (off), Lossless
 * alpha compression (on), Encoder preset Fast / Medium / Slow / Very Slow
 * (Fast; libaom cpu-used 8 / 4 / 0 / 0), Chroma subsampling 4:2:0 /
 * 4:2:2 / 4:4:4 (4:2:2), Preserve existing tile size (on), Premultiplied
 * alpha (off). Quality q selects the AV1 quantizer 63 - round(0.63 q).
 * Lossless writes RGB with the identity matrix (4:4:4); gray images use
 * 4:0:0. Images with even sides are split into an image grid of tiles
 * (Fast 512, Medium 1280, Slow 1920 pixels per side at most; Very Slow
 * never), unless the preserved layout from the opened file still fits.
 * Maximum 65535 x 65535 (FILES.md section 3).
 *
 * Threads: load and save are reentrant. Save passes pc_par_threads(par)
 * to libavif as maxThreads (the AV1 encoder runs its own threads for the
 * duration of the call); load decodes on the calling thread only.
 */
#include "lib_codec.h"
#include "avifjxl_meta.h"
#include "pc/pc_icc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVIF_MAX_SIDE 65535u

/* ---- sniffing (no library needed) -------------------------------------------------- */
/* True when the ISO BMFF 'ftyp' box at the start names brand (major or
 * compatible). */
static bool ftyp_has(const uint8_t *p, size_t n, const char *brand)
{
    uint32_t size;
    if (!p || n < 16u || memcmp(p + 4, "ftyp", 4u) != 0) return false;
    size = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (size < 16u) return false;
    if ((size_t)size > n) size = (uint32_t)n;
    if (memcmp(p + 8, brand, 4u) == 0) return true;
    for (uint32_t o = 16u; o + 4u <= size; o += 4u)
        if (memcmp(p + o, brand, 4u) == 0) return true;
    return false;
}

static bool avif_sniff(const uint8_t *p, size_t n)
{
    return ftyp_has(p, n, "avif") || ftyp_has(p, n, "avis");
}

/* ---- save options --------------------------------------------------------------------- */
typedef struct avif_params {
    int32_t quality;          /* 0..100 */
    int32_t lossless;         /* bool */
    int32_t lossless_alpha;   /* bool */
    int32_t preset;           /* AVIF_PRESET_* */
    int32_t chroma;           /* AVIF_CHROMA_* */
    int32_t keep_tiles;       /* bool: preserve existing tile size */
    int32_t premultiplied;    /* bool */
} avif_params;

enum { AVIF_PRESET_FAST = 0, AVIF_PRESET_MEDIUM, AVIF_PRESET_SLOW, AVIF_PRESET_VERYSLOW };
enum { AVIF_CHROMA_420 = 0, AVIF_CHROMA_422, AVIF_CHROMA_444 };

static const char *const k_avif_presets[] = { "Fast", "Medium", "Slow", "Very Slow", NULL };
static const char *const k_avif_chroma[] = {
    "4:2:0 (best compression)", "4:2:2", "4:4:4 (best quality)", NULL
};

static const fx_prop k_avif_props[] = {
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(avif_params, quality),
      0, 100, 85, 1, NULL, NULL, 0, 0, "lossless=0" },
    { "lossless", "Lossless", FXP_BOOL, (uint32_t)offsetof(avif_params, lossless),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
    { "lossless_alpha", "Lossless alpha compression", FXP_BOOL,
      (uint32_t)offsetof(avif_params, lossless_alpha), 0, 1, 1, 0, NULL, NULL, 0, 0,
      "lossless=0" },
    { "preset", "Encoder preset", FXP_CHOICE, (uint32_t)offsetof(avif_params, preset),
      0, 3, AVIF_PRESET_FAST, 0, k_avif_presets, NULL, 0, 0, NULL },
    { "chroma", "Chroma subsampling", FXP_CHOICE, (uint32_t)offsetof(avif_params, chroma),
      0, 2, AVIF_CHROMA_422, 0, k_avif_chroma, NULL, 0, 0, "lossless=0" },
    { "keep_tiles", "Preserve existing tile size", FXP_BOOL,
      (uint32_t)offsetof(avif_params, keep_tiles), 0, 1, 1, 0, NULL, NULL, 0, 0, NULL },
    { "premultiplied", "Premultiplied alpha", FXP_BOOL,
      (uint32_t)offsetof(avif_params, premultiplied), 0, 1, 0, 0, NULL, NULL, 0, 0,
      "lossless=0" },
};
#define N_AVIF_PROPS ((uint32_t)(sizeof k_avif_props / sizeof k_avif_props[0]))

#if defined(PC_HAVE_AVIF)
/* ======================================================================================= */
#include "avif/avif.h"
#include "lcms2.h"

#include <math.h>

/* ---- image grid layout ---------------------------------------------------------------- */
typedef struct avif_grid {
    uint32_t cols, rows, tile_w, tile_h;
} avif_grid;

/* chroma_x/chroma_y: tiles must have even width/height (4:2:0 both, 4:2:2 x). */
static bool grid_valid(const avif_grid *g, uint32_t w, uint32_t h, bool chroma_x,
                       bool chroma_y)
{
    if (g->cols < 1u || g->rows < 1u || g->cols > 256u || g->rows > 256u) return false;
    if (g->cols * g->rows < 2u) return false;
    if (g->tile_w < 64u || g->tile_h < 64u) return false;              /* MIAF minimum */
    if ((uint64_t)g->tile_w * g->cols != w || (uint64_t)g->tile_h * g->rows != h) return false;
    if (chroma_x && (g->tile_w & 1u)) return false;
    if (chroma_y && (g->tile_h & 1u)) return false;
    return true;
}

/* Largest even divisor tile <= max_tile along one side (fewest tiles), at
 * most 250 tiles and at least 64 pixels; the smallest valid tile when none
 * is small enough. Returns the tile count (1 = no split). */
static uint32_t grid_split(uint32_t side, uint32_t max_tile, uint32_t *tile)
{
    uint32_t best_n = 1u, best_t = side;
    if (side > max_tile) {
        for (uint32_t c = 2u; c <= 250u; c++) {
            uint32_t t = side / c;
            if (t < 64u) break;
            if ((t & 1u) == 0u && t * c == side) {
                best_n = c;
                best_t = t;
                if (t <= max_tile) break;
            }
        }
    }
    *tile = best_t;
    return best_n;
}

/* Grid for a new encode: none for Very Slow or odd sizes. */
static bool grid_auto(uint32_t w, uint32_t h, int32_t preset, avif_grid *g)
{
    static const uint32_t k_max_tile[3] = { 512u, 1280u, 1920u };
    if (preset < AVIF_PRESET_FAST || preset >= AVIF_PRESET_VERYSLOW) return false;
    if ((w & 1u) || (h & 1u)) return false;
    g->cols = grid_split(w, k_max_tile[preset], &g->tile_w);
    if (w == h) {
        g->rows = g->cols;
        g->tile_h = g->tile_w;
    } else {
        g->rows = grid_split(h, k_max_tile[preset], &g->tile_h);
    }
    return g->cols > 1u || g->rows > 1u;
}

static bool grid_parse(const char *s, avif_grid *g)
{
    unsigned long v[4];
    char *end;
    for (int i = 0; i < 4; i++) {
        v[i] = strtoul(s, &end, 10);
        if (end == s || v[i] == 0ul || v[i] > 65535ul) return false;
        s = end;
        if (i < 3) {
            if (*s != ',') return false;
            s++;
        }
    }
    if (*s) return false;
    g->cols = (uint32_t)v[0]; g->rows = (uint32_t)v[1];
    g->tile_w = (uint32_t)v[2]; g->tile_h = (uint32_t)v[3];
    return true;
}

/* ---- ISO BMFF walk for the grid layout of the primary item ------------------------------ */
typedef struct bmff_box {
    uint32_t type;
    size_t   body, end;       /* payload [body, end) */
} bmff_box;

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                            ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Next box in [*pos, end) of r. False at the end or on a malformed header. */
static bool bmff_next(pc_rd *r, size_t *pos, size_t end, bmff_box *b)
{
    uint64_t size;
    size_t hdr = 8u;
    if (*pos >= end || end - *pos < 8u || !pc_rd_seek(r, *pos)) return false;
    size = pc_rd_be32(r);
    b->type = pc_rd_be32(r);
    if (size == 1u) {
        size = ((uint64_t)pc_rd_be32(r) << 32);
        size |= pc_rd_be32(r);
        hdr = 16u;
    } else if (size == 0u) {
        size = (uint64_t)(end - *pos);
    }
    if (r->err || size < hdr || size > (uint64_t)(end - *pos)) return false;
    b->body = *pos + hdr;
    b->end = *pos + (size_t)size;
    *pos = b->end;
    return true;
}

static uint32_t rd_sized(pc_rd *r, uint32_t bytes)
{
    uint32_t v = 0;
    if (bytes == 2u) return pc_rd_be16(r);
    if (bytes == 4u) return pc_rd_be32(r);
    if (bytes == 8u) {
        uint32_t hi = pc_rd_be32(r);
        v = pc_rd_be32(r);
        if (hi) r->err = true;          /* offsets beyond 4 GiB: not for us */
        return v;
    }
    if (bytes != 0u) r->err = true;
    return 0;
}

/* Layout of the primary item when it is a 'grid' with uniform tiles of a
 * known size ('ispe' of the first tile). False otherwise. */
static bool bmff_grid_layout(const uint8_t *p, size_t n, avif_grid *g)
{
    pc_rd r = pc_rd_make(p, n);
    size_t pos = 0, meta_end = 0, mpos = 0;
    bmff_box b, m;
    uint32_t primary = 0, tile0 = 0, grid_off = 0, grid_len = 0;
    uint32_t prop_idx[32], n_prop = 0, out_w = 0, out_h = 0;
    bool is_grid = false, have_loc = false, in_idat = false;
    size_t idat_body = 0, idat_end = 0, ipco_body = 0, ipco_end = 0, ipma_body = 0;
    size_t ipma_end = 0;
    uint8_t ipma_ver = 0;
    uint32_t ipma_flags = 0;
    while (bmff_next(&r, &pos, n, &b))
        if (b.type == FOURCC('m', 'e', 't', 'a')) { meta_end = b.end; mpos = b.body + 4u; break; }
    if (!meta_end) return false;
    /* pass 1: pitm, iprp children, idat */
    for (size_t q = mpos; bmff_next(&r, &q, meta_end, &m);) {
        if (m.type == FOURCC('p', 'i', 't', 'm')) {
            uint8_t ver;
            pc_rd_seek(&r, m.body);
            ver = pc_rd_u8(&r);
            pc_rd_skip(&r, 3u);
            primary = ver == 0u ? pc_rd_be16(&r) : pc_rd_be32(&r);
        } else if (m.type == FOURCC('i', 'd', 'a', 't')) {
            idat_body = m.body;
            idat_end = m.end;
        } else if (m.type == FOURCC('i', 'p', 'r', 'p')) {
            size_t c = m.body;
            bmff_box pb;
            while (bmff_next(&r, &c, m.end, &pb)) {
                if (pb.type == FOURCC('i', 'p', 'c', 'o')) {
                    ipco_body = pb.body;
                    ipco_end = pb.end;
                }
                if (pb.type == FOURCC('i', 'p', 'm', 'a') && !ipma_end) {
                    ipma_body = pb.body;
                    ipma_end = pb.end;
                }
            }
        }
    }
    if (r.err || !primary) return false;
    /* pass 2: item type, location and the first 'dimg' reference */
    for (size_t q = mpos; bmff_next(&r, &q, meta_end, &m);) {
        if (m.type == FOURCC('i', 'i', 'n', 'f')) {
            uint8_t ver;
            size_t c;
            bmff_box ib;
            pc_rd_seek(&r, m.body);
            ver = pc_rd_u8(&r);
            pc_rd_skip(&r, 3u);
            (void)(ver == 0u ? pc_rd_be16(&r) : pc_rd_be32(&r));
            c = r.pos;
            while (!r.err && bmff_next(&r, &c, m.end, &ib)) {
                uint8_t iv;
                uint32_t id, type;
                if (ib.type != FOURCC('i', 'n', 'f', 'e')) continue;
                pc_rd_seek(&r, ib.body);
                iv = pc_rd_u8(&r);
                pc_rd_skip(&r, 3u);
                if (iv < 2u) continue;
                id = iv == 2u ? pc_rd_be16(&r) : pc_rd_be32(&r);
                pc_rd_skip(&r, 2u);
                type = pc_rd_be32(&r);
                if (!r.err && id == primary) is_grid = type == FOURCC('g', 'r', 'i', 'd');
            }
        } else if (m.type == FOURCC('i', 'l', 'o', 'c')) {
            uint8_t ver, s1, s2;
            uint32_t off_sz, len_sz, base_sz, idx_sz, count;
            pc_rd_seek(&r, m.body);
            ver = pc_rd_u8(&r);
            pc_rd_skip(&r, 3u);
            s1 = pc_rd_u8(&r);
            s2 = pc_rd_u8(&r);
            off_sz = s1 >> 4; len_sz = s1 & 15u; base_sz = s2 >> 4;
            idx_sz = (ver == 1u || ver == 2u) ? (s2 & 15u) : 0u;
            if (ver > 2u) return false;
            count = ver < 2u ? pc_rd_be16(&r) : pc_rd_be32(&r);
            for (uint32_t i = 0; i < count && !r.err; i++) {
                uint32_t id = ver < 2u ? pc_rd_be16(&r) : pc_rd_be32(&r), method = 0;
                uint32_t base, ext;
                if (ver == 1u || ver == 2u) method = pc_rd_be16(&r) & 15u;
                pc_rd_skip(&r, 2u);
                base = rd_sized(&r, base_sz);
                ext = pc_rd_be16(&r);
                for (uint32_t e = 0; e < ext && !r.err; e++) {
                    uint32_t eo, el;
                    (void)rd_sized(&r, idx_sz);
                    eo = rd_sized(&r, off_sz);
                    el = rd_sized(&r, len_sz);
                    if (id == primary && ext == 1u && method <= 1u) {
                        grid_off = base + eo;
                        grid_len = el;
                        in_idat = method == 1u;
                        have_loc = grid_off >= base;            /* no wrap */
                    }
                }
            }
        } else if (m.type == FOURCC('i', 'r', 'e', 'f')) {
            uint8_t ver;
            size_t c;
            bmff_box rb;
            pc_rd_seek(&r, m.body);
            ver = pc_rd_u8(&r);
            pc_rd_skip(&r, 3u);
            c = r.pos;
            while (!r.err && bmff_next(&r, &c, m.end, &rb)) {
                uint32_t from, cnt;
                if (rb.type != FOURCC('d', 'i', 'm', 'g')) continue;
                pc_rd_seek(&r, rb.body);
                from = ver == 0u ? pc_rd_be16(&r) : pc_rd_be32(&r);
                cnt = pc_rd_be16(&r);
                if (from == primary && cnt > 0u && !tile0)
                    tile0 = ver == 0u ? pc_rd_be16(&r) : pc_rd_be32(&r);
            }
        }
    }
    if (r.err || !is_grid || !have_loc || !tile0 || !ipco_end || !ipma_end) return false;
    /* the ImageGrid payload */
    {
        size_t at = grid_off, lim = n;
        uint8_t flags;
        if (in_idat) { at = idat_body + (size_t)grid_off; lim = idat_end; }
        if (at < (in_idat ? idat_body : 0u) || at > lim || lim - at < (size_t)grid_len ||
            grid_len < 8u || !pc_rd_seek(&r, at))
            return false;
        if (pc_rd_u8(&r) != 0u) return false;              /* version */
        flags = pc_rd_u8(&r);
        g->rows = (uint32_t)pc_rd_u8(&r) + 1u;
        g->cols = (uint32_t)pc_rd_u8(&r) + 1u;
        if (flags & 1u) {
            if (grid_len < 12u) return false;
            out_w = pc_rd_be32(&r);
            out_h = pc_rd_be32(&r);
        } else {
            out_w = pc_rd_be16(&r);
            out_h = pc_rd_be16(&r);
        }
    }
    /* properties of the first tile: find its 'ispe' */
    pc_rd_seek(&r, ipma_body);
    ipma_ver = pc_rd_u8(&r);
    ipma_flags = ((uint32_t)pc_rd_u8(&r) << 16) | pc_rd_be16(&r);
    {
        uint32_t entries = pc_rd_be32(&r);
        for (uint32_t i = 0; i < entries && !r.err && r.pos < ipma_end; i++) {
            uint32_t id = ipma_ver < 1u ? pc_rd_be16(&r) : pc_rd_be32(&r);
            uint32_t na = pc_rd_u8(&r);
            for (uint32_t k = 0; k < na && !r.err; k++) {
                uint32_t idx = (ipma_flags & 1u) ? (pc_rd_be16(&r) & 0x7FFFu)
                                                 : (uint32_t)(pc_rd_u8(&r) & 0x7Fu);
                if (id == tile0 && n_prop < 32u) prop_idx[n_prop++] = idx;
            }
        }
    }
    if (r.err) return false;
    for (uint32_t k = 0; k < n_prop; k++) {
        size_t c = ipco_body;
        uint32_t want = prop_idx[k], at = 0;
        bmff_box pb;
        while (bmff_next(&r, &c, ipco_end, &pb)) {
            if (++at != want) continue;
            if (pb.type == FOURCC('i', 's', 'p', 'e') && pb.end - pb.body >= 12u) {
                pc_rd_seek(&r, pb.body + 4u);
                g->tile_w = pc_rd_be32(&r);
                g->tile_h = pc_rd_be32(&r);
                if (r.err || !g->tile_w || !g->tile_h) return false;
                /* only uniform, exactly covering layouts can be reused */
                return (uint64_t)g->tile_w * g->cols == out_w &&
                       (uint64_t)g->tile_h * g->rows == out_h;
            }
            break;
        }
    }
    return false;
}

static pc_status res_status(avifResult r)
{
    switch (r) {
    case AVIF_RESULT_OK: return PC_OK;
    case AVIF_RESULT_OUT_OF_MEMORY: return PC_ERR_NOMEM;
    case AVIF_RESULT_NOT_IMPLEMENTED: case AVIF_RESULT_NO_CODEC_AVAILABLE:
        return PC_ERR_UNSUPPORTED;
    default: return PC_ERR_FORMAT;
    }
}

/* ---- CICP to ICC ---------------------------------------------------------------------- */
typedef struct cicp_prim { uint16_t id; double rx, ry, gx, gy, bx, by, wx, wy; } cicp_prim;

static const cicp_prim k_prims[] = {
    { 1u,  0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 },  /* BT.709 / sRGB */
    { 4u,  0.670, 0.330, 0.210, 0.710, 0.140, 0.080, 0.3100, 0.3160 },  /* BT.470 M */
    { 5u,  0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290 },  /* BT.470 BG */
    { 6u,  0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290 },  /* BT.601 */
    { 7u,  0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290 },  /* SMPTE 240 */
    { 8u,  0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.3100, 0.3160 },  /* generic film */
    { 9u,  0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 },  /* BT.2020 */
    { 11u, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3140, 0.3510 },  /* DCI-P3 */
    { 12u, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290 },  /* Display P3 */
    { 22u, 0.630, 0.340, 0.295, 0.605, 0.155, 0.077, 0.3127, 0.3290 },  /* EBU 3213 */
};

static bool tc_srgb_like(uint16_t tc)
{
    /* sRGB, unspecified, and the BT.709 / BT.601 / BT.2020 SDR curves, which
     * still images tagged by video tools use for sRGB content. */
    return tc == 13u || tc == 2u || tc == 1u || tc == 6u || tc == 14u || tc == 15u;
}

/* ICC profile equivalent to SDR CICP primaries + transfer, or NULL (no
 * profile needed, or not representable). *len set on success. */
static uint8_t *cicp_icc(uint16_t prim, uint16_t tc, size_t *len)
{
    const cicp_prim *cp = NULL;
    cmsContext ctx;
    cmsHPROFILE h;
    cmsToneCurve *curve = NULL;
    cmsCIExyY wp;
    cmsCIExyYTRIPLE pr;
    cmsToneCurve *curves[3];
    cmsUInt32Number bytes = 0;
    uint8_t *out = NULL;
    *len = 0;
    if (prim == 2u) prim = 1u;                       /* unspecified: assume BT.709 */
    if (prim == 1u && tc_srgb_like(tc)) return NULL;
    for (size_t i = 0; i < sizeof k_prims / sizeof k_prims[0]; i++)
        if (k_prims[i].id == prim) cp = &k_prims[i];
    if (!cp) return NULL;
    ctx = cmsCreateContext(NULL, NULL);
    if (!ctx) return NULL;
    if (tc == 4u || tc == 5u || tc == 8u) {
        curve = cmsBuildGamma(ctx, tc == 4u ? 2.2 : (tc == 5u ? 2.8 : 1.0));
    } else if (tc == 1u || tc == 6u || tc == 14u || tc == 15u) {
        /* inverse of the BT.709 OETF: type 4, Y = ((X + 0.099) / 1.099)^(1/0.45)
         * for X >= 0.081, else X / 4.5 */
        cmsFloat64Number par[5] = { 1.0 / 0.45, 1.0 / 1.099, 0.099 / 1.099, 1.0 / 4.5, 0.081 };
        curve = cmsBuildParametricToneCurve(ctx, 4, par);
    } else {
        cmsFloat64Number par[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
        curve = cmsBuildParametricToneCurve(ctx, 4, par);
    }
    if (!curve) { cmsDeleteContext(ctx); return NULL; }
    wp.x = cp->wx; wp.y = cp->wy; wp.Y = 1.0;
    pr.Red.x = cp->rx; pr.Red.y = cp->ry; pr.Red.Y = 1.0;
    pr.Green.x = cp->gx; pr.Green.y = cp->gy; pr.Green.Y = 1.0;
    pr.Blue.x = cp->bx; pr.Blue.y = cp->by; pr.Blue.Y = 1.0;
    curves[0] = curves[1] = curves[2] = curve;
    h = cmsCreateRGBProfileTHR(ctx, &wp, &pr, curves);
    cmsFreeToneCurve(curve);
    if (h) {
        char desc[64];
        cmsMLU *mlu = cmsMLUalloc(ctx, 1);
        snprintf(desc, sizeof desc, "CICP %u/%u (paint.c)", (unsigned)prim, (unsigned)tc);
        if (mlu && cmsMLUsetASCII(mlu, "en", "US", desc))
            (void)cmsWriteTag(h, cmsSigProfileDescriptionTag, mlu);
        if (mlu) cmsMLUfree(mlu);
        if (cmsSaveProfileToMem(h, NULL, &bytes) && bytes >= 128u) {
            out = (uint8_t *)malloc(bytes);
            if (out && cmsSaveProfileToMem(h, out, &bytes)) {
                memset(out + 24, 0, 12u);            /* creation date: deterministic */
                *len = bytes;
            } else {
                free(out);
                out = NULL;
            }
        }
        cmsCloseProfile(h);
    }
    cmsDeleteContext(ctx);
    return out;
}

/* ---- decoding ----------------------------------------------------------------------------- */
typedef struct avif_xform {
    uint32_t cx, cy, cw, ch;     /* crop rectangle in the decoded image */
    int      rot;                /* anti-clockwise quarter turns 0..3 */
    int      mirror;             /* -1 none, 0 top-bottom, 1 left-right */
    uint32_t ow, oh;             /* output (document) size */
} avif_xform;

/* Crop rectangle from a 'clap' box (ISO/IEC 14496-12 12.1.4); false when
 * the values are not integral or fall outside the image. */
static bool clap_rect(const avifCleanApertureBox *c, uint32_t w, uint32_t h, avif_xform *x)
{
    int64_t wn = (int32_t)c->widthN, wd = (int32_t)c->widthD;
    int64_t hn = (int32_t)c->heightN, hd = (int32_t)c->heightD;
    int64_t xn = (int32_t)c->horizOffN, xd = (int32_t)c->horizOffD;
    int64_t yn = (int32_t)c->vertOffN, yd = (int32_t)c->vertOffD;
    int64_t cw, ch, num, den, cx, cy;
    if (wd <= 0 || hd <= 0 || xd <= 0 || yd <= 0 || wn <= 0 || hn <= 0) return false;
    if (wn % wd || hn % hd) return false;
    cw = wn / wd;
    ch = hn / hd;
    if (cw > (int64_t)w || ch > (int64_t)h) return false;
    /* left = horizOff + (w - cw) / 2 = (2 xn + (w - cw) xd) / (2 xd) */
    num = 2 * xn + ((int64_t)w - cw) * xd;
    den = 2 * xd;
    if (num % den) return false;
    cx = num / den;
    num = 2 * yn + ((int64_t)h - ch) * yd;
    den = 2 * yd;
    if (num % den) return false;
    cy = num / den;
    if (cx < 0 || cy < 0 || cx + cw > (int64_t)w || cy + ch > (int64_t)h) return false;
    x->cx = (uint32_t)cx; x->cy = (uint32_t)cy;
    x->cw = (uint32_t)cw; x->ch = (uint32_t)ch;
    return true;
}

/* Output position of crop-relative source pixel (sx, sy). */
static void xform_pt(const avif_xform *x, uint32_t sx, uint32_t sy, uint32_t *ox, uint32_t *oy)
{
    uint32_t px, py, rw, rh;
    switch (x->rot) {
    case 1: px = sy; py = x->cw - 1u - sx; rw = x->ch; rh = x->cw; break;
    case 2: px = x->cw - 1u - sx; py = x->ch - 1u - sy; rw = x->cw; rh = x->ch; break;
    case 3: px = x->ch - 1u - sy; py = sx; rw = x->ch; rh = x->cw; break;
    default: px = sx; py = sy; rw = x->cw; rh = x->ch; break;
    }
    if (x->mirror == 0) py = rh - 1u - py;
    else if (x->mirror == 1) px = rw - 1u - px;
    *ox = px;
    *oy = py;
}

typedef enum avif_color_mode { AC_PLAIN = 0, AC_HDR } avif_color_mode;

typedef struct avif_dec_ctx {
    avifImage      *img;
    avifImage      *view;        /* reusable shallow view (avifImageCreateEmpty) */
    pc_doc         *d;
    pc_layer       *layer;
    avif_xform      x;
    avif_color_mode mode;
    axj_hdr_tf      tf;
    axj_hdr_prim    prim;
    double          peak;
    void           *band;        /* converted rows (8 or 16 bit RGBA), w * LC_BAND */
    pc_px32        *row8;        /* HDR: 8-bit result of one band */
    pc_px32        *out;         /* transformed pixels of one band */
} avif_dec_ctx;

/* Convert decoded rows [y0, y0 + nb) and store them, transformed. */
static pc_status avif_band(avif_dec_ctx *c, uint32_t y0, uint32_t nb)
{
    avifCropRect rect;
    avifRGBImage rgb;
    const pc_px32 *src;
    uint32_t w = c->img->width, ya, yb;
    uint32_t minx = UINT32_MAX, miny = UINT32_MAX, maxx = 0, maxy = 0;
    uint32_t corner_x[2], corner_y[2], dw, dh;
    avifResult ar;
    rect.x = 0;
    rect.y = y0;
    rect.width = w;
    rect.height = nb;
    ar = avifImageSetViewRect(c->view, c->img, &rect);
    if (ar != AVIF_RESULT_OK) return res_status(ar);
    avifRGBImageSetDefaults(&rgb, c->view);
    rgb.rowBytes = w * 4u * (c->mode == AC_HDR ? 2u : 1u);
    rgb.pixels = (uint8_t *)c->band;
    if (c->mode == AC_HDR) {
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 16;
    } else {
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
    }
    rgb.alphaPremultiplied = AVIF_FALSE;      /* unpremultiply 'prem' images */
    ar = avifImageYUVToRGB(c->view, &rgb);
    if (ar != AVIF_RESULT_OK) return res_status(ar);
    if (c->mode == AC_HDR) {
        axj_hdr_to_srgb8((const uint16_t *)c->band, c->row8, (size_t)w * nb, c->tf, c->prim,
                         c->peak);
        src = c->row8;
    } else {
        src = (const pc_px32 *)c->band;
    }
    /* rows of this band inside the crop */
    ya = y0 > c->x.cy ? y0 : c->x.cy;
    yb = y0 + nb < c->x.cy + c->x.ch ? y0 + nb : c->x.cy + c->x.ch;
    if (ya >= yb) return PC_OK;
    corner_x[0] = 0; corner_x[1] = c->x.cw - 1u;
    corner_y[0] = ya - c->x.cy; corner_y[1] = yb - 1u - c->x.cy;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            uint32_t ox, oy;
            xform_pt(&c->x, corner_x[i], corner_y[j], &ox, &oy);
            if (ox < minx) minx = ox;
            if (ox > maxx) maxx = ox;
            if (oy < miny) miny = oy;
            if (oy > maxy) maxy = oy;
        }
    dw = maxx - minx + 1u;
    dh = maxy - miny + 1u;
    for (uint32_t y = ya; y < yb; y++) {
        const pc_px32 *row = src + (size_t)(y - y0) * w + c->x.cx;
        for (uint32_t xx = 0; xx < c->x.cw; xx++) {
            uint32_t ox, oy;
            xform_pt(&c->x, xx, y - c->x.cy, &ox, &oy);
            c->out[(size_t)(oy - miny) * dw + (ox - minx)] = row[xx];
        }
    }
    return pc_layer_store_rect(c->d, c->layer,
                               pc_rect_make((int32_t)minx, (int32_t)miny, (int32_t)dw,
                                            (int32_t)dh),
                               c->out, dw);
}

static pc_status put_meta(avifDecoder *dec, pc_image_meta *meta, const uint8_t *p, size_t n,
                          bool sequence)
{
    const avifImage *im = dec->image;
    pc_status st = PC_OK;
    if (im->icc.size > 0u && im->icc.data) {
        meta->icc = (uint8_t *)malloc(im->icc.size);
        if (!meta->icc) return PC_ERR_NOMEM;
        memcpy(meta->icc, im->icc.data, im->icc.size);
        meta->icc_len = im->icc.size;
    }
    if (im->exif.size > 0u && im->exif.data)
        st = axj_meta_put_exif(meta, im->exif.data, im->exif.size);
    if (st == PC_OK && im->xmp.size > 0u && im->xmp.data)
        st = axj_meta_put_xmp(meta, im->xmp.data, im->xmp.size);
    if (st == PC_OK && !sequence &&
        !(im->transformFlags & (AVIF_TRANSFORM_CLAP | AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR))) {
        avif_grid g;
        if (bmff_grid_layout(p, n, &g)) {
            char v[64];
            snprintf(v, sizeof v, "%u,%u,%u,%u", (unsigned)g.cols, (unsigned)g.rows,
                     (unsigned)g.tile_w, (unsigned)g.tile_h);
            st = pc_meta_add(meta, "avif.grid", v);
        }
    }
    return st;
}

static pc_status avif_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                           pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    avifDecoder *dec = NULL;
    avifResult ar;
    avif_dec_ctx c;
    pc_status st;
    bool sequence = false;
    uint64_t need = 0, yuv = 0;
    uint32_t w = 0, h = 0, bps = 1;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    memset(&c, 0, sizeof c);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (!avif_sniff(p, n)) return PC_ERR_FORMAT;
    for (int attempt = 0; attempt < 2; attempt++) {
        dec = avifDecoderCreate();
        if (!dec) return PC_ERR_NOMEM;
        /* libavif's own caps stay at their maxima (16384 x 16384 pixels,
         * 65535 per side) so that pc_codec_check_size below can report
         * PC_ERR_LIMIT; parsing allocates nothing proportional to the size. */
        dec->maxThreads = 1;
        dec->strictFlags = AVIF_STRICT_DISABLED;
        dec->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
        dec->imageDimensionLimit = AVIF_MAX_SIDE;
        /* first the primary item, then (files with only a sequence track) the track */
        ar = avifDecoderSetSource(dec, attempt == 0 ? AVIF_DECODER_SOURCE_PRIMARY_ITEM
                                                    : AVIF_DECODER_SOURCE_TRACKS);
        if (ar == AVIF_RESULT_OK) ar = avifDecoderSetIOMemory(dec, p, n);
        if (ar == AVIF_RESULT_OK) ar = avifDecoderParse(dec);
        if (ar == AVIF_RESULT_OK) {
            sequence = attempt == 1 || dec->imageCount > 1 || ftyp_has(p, n, "avis");
#if AVIF_VERSION >= 1010000
            if (dec->imageSequenceTrackPresent) sequence = true;
#endif
            break;
        }
        avifDecoderDestroy(dec);
        dec = NULL;
        if (ar == AVIF_RESULT_OUT_OF_MEMORY) return PC_ERR_NOMEM;
        if (ar == AVIF_RESULT_NOT_IMPLEMENTED) return PC_ERR_UNSUPPORTED;
    }
    if (!dec) return PC_ERR_FORMAT;
    w = dec->image->width;
    h = dec->image->height;
    bps = dec->image->depth > 8u ? 2u : 1u;
    st = pc_codec_check_size(lim, w, h, 1u);
    if (st != PC_OK) goto done;
    /* Decoder planes (Y + 2 chroma + alpha), the document and the band
     * buffers must fit the memory budget before decoding starts. */
    yuv = (uint64_t)w * h * bps;
    switch (dec->image->yuvFormat) {
    case AVIF_PIXEL_FORMAT_YUV444: yuv *= 3u; break;
    case AVIF_PIXEL_FORMAT_YUV422: yuv *= 2u; break;
    case AVIF_PIXEL_FORMAT_YUV420: yuv = yuv * 3u / 2u + 4u * w; break;
    default: break;
    }
    if (dec->alphaPresent) yuv += (uint64_t)w * h * bps;
    need = yuv + (uint64_t)w * h * 4u + (uint64_t)w * (uint64_t)LC_BAND * 20u;
    if (need > lim->max_mem) { st = PC_ERR_LIMIT; goto done; }
    ar = avifDecoderNextImage(dec);
    if (ar != AVIF_RESULT_OK) { st = res_status(ar); goto done; }
    c.img = dec->image;
    if (c.img->width != w || c.img->height != h) { st = PC_ERR_FORMAT; goto done; }
    /* transforms */
    c.x.cx = 0; c.x.cy = 0; c.x.cw = w; c.x.ch = h;
    c.x.rot = 0;
    c.x.mirror = -1;
    if (c.img->transformFlags & AVIF_TRANSFORM_CLAP) {
        avif_xform t = c.x;
        if (clap_rect(&c.img->clap, w, h, &t)) c.x = t;
    }
    if (c.img->transformFlags & AVIF_TRANSFORM_IROT) c.x.rot = c.img->irot.angle & 3;
    if (c.img->transformFlags & AVIF_TRANSFORM_IMIR) c.x.mirror = c.img->imir.axis ? 1 : 0;
    c.x.ow = (c.x.rot & 1) ? c.x.ch : c.x.cw;
    c.x.oh = (c.x.rot & 1) ? c.x.cw : c.x.ch;
    /* color */
    st = put_meta(dec, meta, p, n, sequence);
    if (st != PC_OK) goto done;
    if (!meta->icc) {
        uint16_t prim = (uint16_t)c.img->colorPrimaries;
        uint16_t tc = (uint16_t)c.img->transferCharacteristics;
        if (tc == 16u || tc == 18u) {
            c.mode = AC_HDR;
            c.tf = tc == 16u ? AXJ_TF_PQ : AXJ_TF_HLG;
            c.prim = prim == 9u ? AXJ_PRIM_BT2020
                                : ((prim == 11u || prim == 12u) ? AXJ_PRIM_P3 : AXJ_PRIM_BT709);
            c.peak = c.img->clli.maxCLL > 0u ? (double)c.img->clli.maxCLL : 0.0;
            lc_note(meta, "HDR image (PQ or HLG) tone mapped to 8-bit sRGB");
        } else {
            size_t ilen = 0;
            uint8_t *icc = cicp_icc(prim, tc, &ilen);
            if (icc) { meta->icc = icc; meta->icc_len = ilen; }
        }
    }
    if (sequence && !meta->note[0])
        lc_note(meta, "AVIF image sequence: only the first frame was loaded");
    meta->src_bits = c.img->depth;
    meta->had_alpha = c.img->alphaPlane != NULL;
    /* pixels */
    st = lc_doc_new(lim, c.x.ow, c.x.oh, 1u, &c.d, &c.layer);
    if (st != PC_OK) goto done;
    c.view = avifImageCreateEmpty();
    if (!c.view) { st = PC_ERR_NOMEM; goto done; }
    c.band = lc_alloc((size_t)w * (size_t)LC_BAND, c.mode == AC_HDR ? 8u : 4u, lim, &st);
    if (c.band && c.mode == AC_HDR)
        c.row8 = (pc_px32 *)lc_alloc((size_t)w * (size_t)LC_BAND, sizeof(pc_px32), lim, &st);
    if (st == PC_OK)
        c.out = (pc_px32 *)lc_alloc((size_t)w * (size_t)LC_BAND, sizeof(pc_px32), lim, &st);
    for (uint32_t y0 = 0; y0 < h && st == PC_OK; y0 += (uint32_t)LC_BAND) {
        uint32_t nb = h - y0 < (uint32_t)LC_BAND ? h - y0 : (uint32_t)LC_BAND;
        if (y0 + nb <= c.x.cy || y0 >= c.x.cy + c.x.ch) continue;
        st = avif_band(&c, y0, nb);
    }
done:
    free(c.band);
    free(c.row8);
    free(c.out);
    if (c.view) avifImageDestroy(c.view);       /* a view: planes stay with dec */
    if (dec) avifDecoderDestroy(dec);
    if (st != PC_OK) {
        pc_doc_destroy(c.d);
        pc_meta_free(meta);
        return st;
    }
    *out = c.d;
    return PC_OK;
}

/* ---- encoding --------------------------------------------------------------------------- */
/* libavif quality giving the AV1 quantizer 63 - round(0.63 q). */
static int avif_quality_for(int32_t q)
{
    int qz = 63 - (int)floor((double)q * 63.0 / 100.0 + 0.5);
    for (int a = 0; a <= 100; a++)
        if (((100 - a) * 63 + 50) / 100 == qz) return a;
    return (int)q;
}

typedef struct avif_scan {
    bool alpha, gray;
} avif_scan;

static pc_status scan_doc(const pc_doc *d, const pc_par *par, pc_px32 *band, avif_scan *s)
{
    lc_flat flat;
    pc_status st = PC_OK;
    flat.d = d;
    flat.par = par;
    flat.over_white = false;
    s->alpha = false;
    s->gray = true;
    for (int32_t y0 = 0; y0 < (int32_t)d->h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        size_t k = (size_t)d->w * (size_t)nb;
        st = lc_src_flatten(&flat, y0, nb, band);
        for (size_t i = 0; i < k && st == PC_OK; i++) {
            if (band[i].a != 255u) s->alpha = true;
            if (band[i].r != band[i].g || band[i].g != band[i].b) s->gray = false;
        }
    }
    return st;
}

static pc_status enc_status(avifResult r)
{
    switch (r) {
    case AVIF_RESULT_OK: return PC_OK;
    case AVIF_RESULT_OUT_OF_MEMORY: return PC_ERR_NOMEM;
    case AVIF_RESULT_NO_CODEC_AVAILABLE: case AVIF_RESULT_NOT_IMPLEMENTED:
        return PC_ERR_UNSUPPORTED;
    case AVIF_RESULT_INVALID_IMAGE_GRID: return PC_ERR_LIMIT;
    default: return PC_ERR_STATE;
    }
}

static pc_status avif_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                           const pc_par *par, pc_buf *out)
{
    avif_params prm;
    avif_scan sc;
    avif_grid grid = { 0u, 0u, 0u, 0u };
    bool use_grid = false, use_icc = false;
    avifPixelFormat fmt;
    avifImage *img = NULL, *view = NULL;
    avifImage **cells = NULL;
    uint32_t n_cells = 0;
    avifEncoder *enc = NULL;
    avifRWData data = AVIF_DATA_EMPTY;
    avifResult ar;
    pc_px32 *band = NULL;
    pc_status st = PC_OK;
    lc_flat flat;
    uint8_t *exif = NULL, *xmp = NULL;
    size_t exif_len = 0, xmp_len = 0;
    uint32_t threads;
    if (!d || !out) return PC_ERR_ARG;
    if (d->w > AVIF_MAX_SIDE || d->h > AVIF_MAX_SIDE || !d->w || !d->h) return PC_ERR_LIMIT;
    prm.quality = 85;
    prm.lossless = 0;
    prm.lossless_alpha = 1;
    prm.preset = AVIF_PRESET_FAST;
    prm.chroma = AVIF_CHROMA_422;
    prm.keep_tiles = 1;
    prm.premultiplied = 0;
    if (params) memcpy(&prm, params, sizeof prm);
    prm.quality = prm.quality < 0 ? 0 : (prm.quality > 100 ? 100 : prm.quality);
    if (prm.preset < AVIF_PRESET_FAST || prm.preset > AVIF_PRESET_VERYSLOW)
        prm.preset = AVIF_PRESET_FAST;
    if (prm.chroma < AVIF_CHROMA_420 || prm.chroma > AVIF_CHROMA_444) prm.chroma = AVIF_CHROMA_422;
    if (prm.lossless) { prm.lossless_alpha = 1; prm.premultiplied = 0; }
    band = (pc_px32 *)malloc((size_t)d->w * (size_t)LC_BAND * sizeof *band);
    if (!band) return PC_ERR_NOMEM;
    st = scan_doc(d, par, band, &sc);
    if (st != PC_OK) goto done;
    /* profile: RGB profiles keep the image RGB; a gray profile only fits a gray image */
    if (meta && meta->icc && meta->icc_len) {
        pc_icc_info info;
        if (pc_icc_inspect(meta->icc, meta->icc_len, &info) == PC_OK) {
            if (info.space == PC_ICC_SPACE_RGB) { use_icc = true; sc.gray = false; }
            else if (info.space == PC_ICC_SPACE_GRAY && sc.gray) use_icc = true;
        }
    }
    if (sc.gray) fmt = AVIF_PIXEL_FORMAT_YUV400;
    else if (prm.lossless || prm.chroma == AVIF_CHROMA_444) fmt = AVIF_PIXEL_FORMAT_YUV444;
    else if (prm.chroma == AVIF_CHROMA_420) fmt = AVIF_PIXEL_FORMAT_YUV420;
    else fmt = AVIF_PIXEL_FORMAT_YUV422;
    img = avifImageCreate(d->w, d->h, 8u, fmt);
    if (!img) { st = PC_ERR_NOMEM; goto done; }
    img->yuvRange = AVIF_RANGE_FULL;
    img->colorPrimaries = use_icc ? AVIF_COLOR_PRIMARIES_UNSPECIFIED : AVIF_COLOR_PRIMARIES_BT709;
    img->transferCharacteristics = use_icc ? AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED
                                           : AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    img->matrixCoefficients = (prm.lossless && !sc.gray) ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
                                                         : AVIF_MATRIX_COEFFICIENTS_BT601;
    img->alphaPremultiplied = (sc.alpha && prm.premultiplied) ? AVIF_TRUE : AVIF_FALSE;
    ar = avifImageAllocatePlanes(img, sc.alpha ? AVIF_PLANES_ALL : AVIF_PLANES_YUV);
    if (ar != AVIF_RESULT_OK) { st = enc_status(ar); goto done; }
    view = avifImageCreateEmpty();
    if (!view) { st = PC_ERR_NOMEM; goto done; }
    /* RGB to YUV in bands, through views of the full image */
    flat.d = d;
    flat.par = par;
    flat.over_white = false;
    for (int32_t y0 = 0; y0 < (int32_t)d->h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)d->h - y0 < LC_BAND ? (int32_t)d->h - y0 : LC_BAND;
        avifCropRect rect;
        avifRGBImage rgb;
        st = lc_src_flatten(&flat, y0, nb, band);
        if (st != PC_OK) break;
        rect.x = 0; rect.y = (uint32_t)y0; rect.width = d->w; rect.height = (uint32_t)nb;
        ar = avifImageSetViewRect(view, img, &rect);
        if (ar != AVIF_RESULT_OK) { st = enc_status(ar); break; }
        avifRGBImageSetDefaults(&rgb, view);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.pixels = (uint8_t *)band;
        rgb.rowBytes = d->w * 4u;
        rgb.ignoreAlpha = sc.alpha ? AVIF_FALSE : AVIF_TRUE;
        rgb.alphaPremultiplied = AVIF_FALSE;
        rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_BEST_QUALITY;
        ar = avifImageRGBToYUV(view, &rgb);
        /* avifImageRGBToYUV runs avifImageAllocatePlanes, which keeps the
         * existing (borrowed) planes of the view but marks them as owned;
         * undo that, or the next view change would free img's planes. */
        view->imageOwnsYUVPlanes = AVIF_FALSE;
        view->imageOwnsAlphaPlane = AVIF_FALSE;
        if (ar != AVIF_RESULT_OK) st = enc_status(ar);
    }
    if (st != PC_OK) goto done;
    /* metadata */
    if (use_icc) {
        ar = avifImageSetProfileICC(img, meta->icc, meta->icc_len);
        if (ar != AVIF_RESULT_OK) { st = enc_status(ar); goto done; }
    }
    if (meta) {
        exif = axj_meta_get_exif(meta, &exif_len);
        xmp = axj_meta_get_xmp(meta, &xmp_len);
    }
    if (exif && exif_len) {
        ar = avifImageSetMetadataExif(img, exif, exif_len);
        if (ar == AVIF_RESULT_OUT_OF_MEMORY) { st = PC_ERR_NOMEM; goto done; }
        if (ar != AVIF_RESULT_OK) avifRWDataFree(&img->exif);      /* unusable: drop */
    }
    if (xmp && xmp_len) {
        ar = avifImageSetMetadataXMP(img, xmp, xmp_len);
        if (ar != AVIF_RESULT_OK) { st = enc_status(ar); goto done; }
    }
    /* the pixels are stored upright: no irot/imir even if Exif named one */
    img->transformFlags = AVIF_TRANSFORM_NONE;
    /* grid layout */
    {
        bool cx = fmt == AVIF_PIXEL_FORMAT_YUV420 || fmt == AVIF_PIXEL_FORMAT_YUV422;
        bool cy = fmt == AVIF_PIXEL_FORMAT_YUV420;
        const char *keep = (meta && prm.keep_tiles) ? pc_meta_get(meta, "avif.grid") : NULL;
        if (prm.preset != AVIF_PRESET_VERYSLOW && !(d->w & 1u) && !(d->h & 1u)) {
            if (keep && grid_parse(keep, &grid) && grid_valid(&grid, d->w, d->h, cx, cy))
                use_grid = true;
            else if (grid_auto(d->w, d->h, prm.preset, &grid) &&
                     grid_valid(&grid, d->w, d->h, cx, cy))
                use_grid = true;
        }
    }
    if (use_grid) {
        size_t bytes;
        if (!pc_mul_size((size_t)grid.cols * grid.rows, sizeof *cells, &bytes)) {
            st = PC_ERR_LIMIT;
            goto done;
        }
        cells = (avifImage **)calloc(1u, bytes);
        if (!cells) { st = PC_ERR_NOMEM; goto done; }
        for (uint32_t r = 0; r < grid.rows && st == PC_OK; r++)
            for (uint32_t c = 0; c < grid.cols && st == PC_OK; c++) {
                avifCropRect rect;
                avifImage *cell = avifImageCreateEmpty();
                if (!cell) { st = PC_ERR_NOMEM; break; }
                cells[n_cells++] = cell;
                rect.x = c * grid.tile_w; rect.y = r * grid.tile_h;
                rect.width = grid.tile_w; rect.height = grid.tile_h;
                ar = avifImageSetViewRect(cell, img, &rect);
                if (ar == AVIF_RESULT_OK && use_icc)
                    ar = avifImageSetProfileICC(cell, img->icc.data, img->icc.size);
                if (ar == AVIF_RESULT_OK && img->exif.size)
                    ar = avifRWDataSet(&cell->exif, img->exif.data, img->exif.size);
                if (ar == AVIF_RESULT_OK && img->xmp.size)
                    ar = avifRWDataSet(&cell->xmp, img->xmp.data, img->xmp.size);
                cell->transformFlags = AVIF_TRANSFORM_NONE;
                if (ar != AVIF_RESULT_OK) st = enc_status(ar);
            }
        if (st != PC_OK) goto done;
    }
    /* encoder */
    enc = avifEncoderCreate();
    if (!enc) { st = PC_ERR_NOMEM; goto done; }
    if (avifCodecName(AVIF_CODEC_CHOICE_AOM, AVIF_CODEC_FLAG_CAN_ENCODE))
        enc->codecChoice = AVIF_CODEC_CHOICE_AOM;
    threads = pc_par_threads(par);
    enc->maxThreads = (int)(threads < 1u ? 1u : (threads > 64u ? 64u : threads));
    enc->speed = prm.preset == AVIF_PRESET_FAST ? 8 : (prm.preset == AVIF_PRESET_MEDIUM ? 4 : 0);
    if (prm.lossless) {
        enc->quality = AVIF_QUALITY_LOSSLESS;
        enc->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    } else {
        enc->quality = avif_quality_for(prm.quality);
        enc->qualityAlpha = prm.lossless_alpha ? AVIF_QUALITY_LOSSLESS : enc->quality;
    }
    if (use_grid)
        ar = avifEncoderAddImageGrid(enc, grid.cols, grid.rows,
                                     (const avifImage *const *)cells, AVIF_ADD_IMAGE_FLAG_SINGLE);
    else
        ar = avifEncoderAddImage(enc, img, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (ar == AVIF_RESULT_OK) ar = avifEncoderFinish(enc, &data);
    if (ar != AVIF_RESULT_OK) { st = enc_status(ar); goto done; }
    st = pc_buf_append(out, data.data, data.size);
done:
    avifRWDataFree(&data);
    if (enc) avifEncoderDestroy(enc);
    for (uint32_t i = 0; i < n_cells; i++) avifImageDestroy(cells[i]);
    free(cells);
    if (view) avifImageDestroy(view);
    if (img) avifImageDestroy(img);
    free(exif);
    free(xmp);
    free(band);
    return st;
}

#define AVIF_FLAGS (PC_CODEC_LOAD | PC_CODEC_SAVE)
#define AVIF_LOAD_FN avif_load
#define AVIF_SAVE_FN avif_save

#else /* !PC_HAVE_AVIF ======================================================================= */

/* Library absent: registered without flags, so the app hides the type. */
#define AVIF_FLAGS 0u
#define AVIF_LOAD_FN NULL
#define AVIF_SAVE_FN NULL
#endif

const pc_codec pc_codec_avif = {
    "avif", "AV1 (AVIF)", "avif", AVIF_FLAGS,
    avif_sniff, AVIF_LOAD_FN,
    k_avif_props, N_AVIF_PROPS, (uint32_t)sizeof(avif_params),
    AVIF_SAVE_FN
};
