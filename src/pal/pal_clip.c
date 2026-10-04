/* pal_clip.c - clipboard image flavours.
 *
 * The encoders (BMP with a BITMAPV5HEADER, baseline TIFF) and the DIB
 * validator are pure functions used on every OS. The public clipboard API
 * below them is the SDL3 MIME clipboard used on macOS, X11 and Wayland;
 * Windows implements the same functions natively in pal_win32.c so that
 * the registered "PNG" format and CF_DIBV5 / CF_DIB work with every
 * Windows application regardless of the SDL version in use.
 */
#include "pal_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- little-endian writers ---------------------------------------------------- */
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Pixel bytes of a w x h 32-bit image, bounded so every encoded size still
 * fits a 32-bit file field. */
static bool raw_bytes(const uint8_t *bgra, int32_t w, int32_t h, size_t stride, size_t extra,
                      size_t *pixels, size_t *total)
{
    size_t row;
    if (!bgra || w <= 0 || h <= 0 || (uint32_t)w > PC_MAX_DIM || (uint32_t)h > PC_MAX_DIM)
        return false;
    if (!pc_mul_size((size_t)w, 4u, &row) || stride < row) return false;
    if (!pc_mul_size(row, (size_t)h, pixels) || !pc_add_size(*pixels, extra, total)) return false;
    return (uint64_t)*total <= 0xFFFFFFFFull;
}

/* ---- BMP / DIB ------------------------------------------------------------------- */
#define BMP_FILE_HDR 14u
#define BMP_V5_HDR   124u

uint8_t *pal__enc_bmp(const uint8_t *bgra, int32_t w, int32_t h, size_t stride,
                      bool file_header, size_t *out_len)
{
    size_t pixels, total, hdr = (file_header ? BMP_FILE_HDR : 0u) + BMP_V5_HDR, row;
    uint8_t *b, *v5, *px;
    if (out_len) *out_len = 0;
    if (!out_len || !raw_bytes(bgra, w, h, stride, hdr, &pixels, &total)) return NULL;
    b = (uint8_t *)calloc(1u, total);
    if (!b) return NULL;
    if (file_header) {
        b[0] = 'B';
        b[1] = 'M';
        w32(b + 2, (uint32_t)total);
        w32(b + 10, (uint32_t)hdr);              /* pixel data offset */
    }
    v5 = b + (file_header ? BMP_FILE_HDR : 0u);
    w32(v5 + 0, BMP_V5_HDR);                    /* bV5Size */
    w32(v5 + 4, (uint32_t)w);                   /* bV5Width */
    w32(v5 + 8, (uint32_t)h);                   /* bV5Height > 0: bottom-up */
    w16(v5 + 12, 1u);                           /* bV5Planes */
    w16(v5 + 14, 32u);                          /* bV5BitCount */
    w32(v5 + 16, 3u);                           /* BI_BITFIELDS */
    w32(v5 + 20, (uint32_t)pixels);             /* bV5SizeImage */
    w32(v5 + 24, 2835u);                        /* 72 dpi in pixels per metre */
    w32(v5 + 28, 2835u);
    w32(v5 + 40, 0x00FF0000u);                  /* red mask */
    w32(v5 + 44, 0x0000FF00u);                  /* green mask */
    w32(v5 + 48, 0x000000FFu);                  /* blue mask */
    w32(v5 + 52, 0xFF000000u);                  /* alpha mask */
    memcpy(v5 + 56, "BGRs", 4u);                /* LCS_sRGB, stored little-endian */
    w32(v5 + 108, 4u);                          /* LCS_GM_IMAGES */
    px = v5 + BMP_V5_HDR;
    row = (size_t)w * 4u;
    for (int32_t y = 0; y < h; y++)
        memcpy(px + (size_t)(h - 1 - y) * row, bgra + (size_t)y * stride, row);
    *out_len = total;
    return b;
}

uint8_t *pal__dib_to_bmp(const uint8_t *dib, size_t n, size_t *out_len)
{
    uint32_t hsize, bits, comp, used, masks = 0, table = 0;
    int32_t width, height;
    uint64_t stride, image, offset, need;
    uint8_t *b;
    if (out_len) *out_len = 0;
    if (!dib || !out_len || n < 40u || n > 0xFFFFFFFFu - BMP_FILE_HDR) return NULL;
    hsize = r32(dib);
    if (hsize < 40u || hsize > n) return NULL;
    width = (int32_t)r32(dib + 4);
    height = (int32_t)r32(dib + 8);
    bits = r16(dib + 14);
    comp = r32(dib + 16);
    used = r32(dib + 32);
    if (width <= 0 || height == 0 || height == INT32_MIN || r16(dib + 12) != 1u) return NULL;
    if (bits != 1u && bits != 4u && bits != 8u && bits != 16u && bits != 24u && bits != 32u)
        return NULL;
    if (comp == 3u || comp == 6u) {             /* BI_BITFIELDS, BI_ALPHABITFIELDS */
        if (bits != 16u && bits != 32u) return NULL;
        /* A 40-byte header keeps the masks after it; V2+ headers include
         * them. Some writers append masks after a V5 header too; that is
         * detected from the total size below. */
        if (hsize == 40u) masks = comp == 6u ? 16u : 12u;
    } else if (comp != 0u && comp != 1u && comp != 2u) {
        return NULL;                            /* JPEG / PNG inside a DIB: no */
    }
    if (bits <= 8u) {
        uint32_t max = 1u << bits;
        table = (used == 0u || used > max) ? max : used;
    } else if (used <= 256u) {
        table = used;                           /* optional optimization palette */
    } else {
        return NULL;
    }
    stride = (((uint64_t)(uint32_t)width * bits + 31u) / 32u) * 4u;
    image = stride * (uint64_t)(height < 0 ? -(int64_t)height : height);
    offset = (uint64_t)hsize + masks + (uint64_t)table * 4u;
    if (comp == 1u || comp == 2u) {             /* RLE: size comes from the header */
        image = r32(dib + 20);
        if (image == 0u || height < 0) return NULL;
    }
    if (comp == 3u && hsize > 40u && masks == 0u && offset + 12u + image == (uint64_t)n)
        offset += 12u;                          /* masks after a V4/V5 header */
    need = offset + image;
    if (need > (uint64_t)n) return NULL;
    b = (uint8_t *)malloc(BMP_FILE_HDR + n);
    if (!b) return NULL;
    b[0] = 'B';
    b[1] = 'M';
    w32(b + 2, (uint32_t)(BMP_FILE_HDR + n));
    w32(b + 6, 0u);
    w32(b + 10, (uint32_t)(BMP_FILE_HDR + offset));
    memcpy(b + BMP_FILE_HDR, dib, n);
    *out_len = BMP_FILE_HDR + n;
    return b;
}

/* ---- TIFF ------------------------------------------------------------------------ */
uint8_t *pal__enc_tiff(const uint8_t *bgra, int32_t w, int32_t h, size_t stride,
                       size_t *out_len)
{
    /* Layout: 8-byte header, IFD with NTAGS entries, BitsPerSample values,
     * X and Y resolution rationals, pixel strip. Every offset is even. */
    enum { NTAGS = 14 };
    const size_t ifd = 8u, ifd_size = 2u + (size_t)NTAGS * 12u + 4u;
    const size_t bps_off = ifd + ifd_size, res_off = bps_off + 8u, data_off = res_off + 16u;
    size_t pixels, total;
    uint8_t *t, *e, *px;
    if (out_len) *out_len = 0;
    if (!out_len || !raw_bytes(bgra, w, h, stride, data_off, &pixels, &total)) return NULL;
    t = (uint8_t *)calloc(1u, total);
    if (!t) return NULL;
    t[0] = 'I';
    t[1] = 'I';
    w16(t + 2, 42u);
    w32(t + 4, (uint32_t)ifd);
    w16(t + ifd, (uint32_t)NTAGS);
    e = t + ifd + 2u;
    /* Entries sorted by tag id. SHORT values sit in the low half of the
     * value field (little-endian), so w32 stores them correctly. */
#define TAG(id, type, count, value)                                                         \
    do { w16(e, (id)); w16(e + 2, (type)); w32(e + 4, (count)); w32(e + 8, (value)); e += 12; } \
    while (0)
    TAG(256u, 4u, 1u, (uint32_t)w);                  /* ImageWidth, LONG */
    TAG(257u, 4u, 1u, (uint32_t)h);                  /* ImageLength */
    TAG(258u, 3u, 4u, (uint32_t)bps_off);            /* BitsPerSample 8,8,8,8 */
    TAG(259u, 3u, 1u, 1u);                           /* Compression: none */
    TAG(262u, 3u, 1u, 2u);                           /* PhotometricInterpretation: RGB */
    TAG(273u, 4u, 1u, (uint32_t)data_off);           /* StripOffsets */
    TAG(277u, 3u, 1u, 4u);                           /* SamplesPerPixel */
    TAG(278u, 4u, 1u, (uint32_t)h);                  /* RowsPerStrip */
    TAG(279u, 4u, 1u, (uint32_t)pixels);             /* StripByteCounts */
    TAG(282u, 5u, 1u, (uint32_t)res_off);            /* XResolution 72/1 */
    TAG(283u, 5u, 1u, (uint32_t)(res_off + 8u));     /* YResolution 72/1 */
    TAG(284u, 3u, 1u, 1u);                           /* PlanarConfiguration: chunky */
    TAG(296u, 3u, 1u, 2u);                           /* ResolutionUnit: inch */
    TAG(338u, 3u, 1u, 2u);                           /* ExtraSamples: unassociated alpha */
#undef TAG
    w32(e, 0u);                                      /* no next IFD */
    for (size_t i = 0; i < 4u; i++) w16(t + bps_off + 2u * i, 8u);
    w32(t + res_off, 72u);
    w32(t + res_off + 4u, 1u);
    w32(t + res_off + 8u, 72u);
    w32(t + res_off + 12u, 1u);
    px = t + data_off;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *s = bgra + (size_t)y * stride;
        uint8_t *d = px + (size_t)y * (size_t)w * 4u;
        for (int32_t x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    *out_len = total;
    return t;
}

/* ---- SDL3 MIME clipboard (macOS, X11, Wayland) ----------------------------------- */
#if !defined(_WIN32)
#include <SDL3/SDL.h>

/* Flavours offered by pal_clip_set_image_*: PNG always, plus BMP on X11 and
 * Wayland or TIFF on macOS when pixels were given. Owned by SDL from the
 * successful SDL_SetClipboardData call until clip_cleanup. */
typedef struct clip_offer {
    uint8_t *png;   size_t png_len;
    uint8_t *alt;   size_t alt_len;     /* BMP file or TIFF */
    const char *alt_mime;
} clip_offer;

#if defined(__APPLE__)
#  define CLIP_ALT_MIME "image/tiff"
#else
#  define CLIP_ALT_MIME "image/bmp"
#endif

/* Accepted on paste, best first. */
static const char *const k_paste_mimes[] = {
    "image/png", "image/bmp", "image/x-bmp", "image/x-MS-bmp", "image/tiff",
    "image/webp", "image/jpeg", "image/gif", NULL
};

static const void *SDLCALL clip_provide(void *ud, const char *mime, size_t *size)
{
    const clip_offer *o = (const clip_offer *)ud;
    *size = 0;
    if (!mime) return NULL;
    if (o->png && SDL_strcmp(mime, "image/png") == 0) {
        *size = o->png_len;
        return o->png;
    }
    if (o->alt && o->alt_mime && SDL_strcmp(mime, o->alt_mime) == 0) {
        *size = o->alt_len;
        return o->alt;
    }
    return NULL;
}

static void clip_offer_free(clip_offer *o)
{
    if (!o) return;
    free(o->png);
    free(o->alt);
    free(o);
}

static void SDLCALL clip_cleanup(void *ud)
{
    clip_offer_free((clip_offer *)ud);
}

static bool clip_ready(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        pal__log_str(PAL_LOG_DEBUG, "clipboard: SDL video is not initialized");
        return false;
    }
    return true;
}

static bool clip_offer_set(clip_offer *o)
{
    const char *mimes[2];
    size_t n = 0;
    if (o->png) mimes[n++] = "image/png";
    if (o->alt) mimes[n++] = o->alt_mime;
    if (n == 0u) {
        clip_offer_free(o);
        return false;
    }
    /* From here on SDL owns o and frees it through clip_cleanup. */
    if (!SDL_SetClipboardData(clip_provide, clip_cleanup, o, mimes, n)) {
        pal_log(PAL_LOG_WARN, "clipboard: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool pal_clip_has_image(void)
{
    if (!clip_ready()) return false;
    for (size_t i = 0; k_paste_mimes[i]; i++)
        if (SDL_HasClipboardData(k_paste_mimes[i])) return true;
    return false;
}

bool pal_clip_get_image(uint8_t **data, size_t *len, char *mime, size_t mime_cap)
{
    if (data) *data = NULL;
    if (len) *len = 0;
    if (mime && mime_cap) mime[0] = '\0';
    if (!data || !len || !clip_ready()) return false;
    for (size_t i = 0; k_paste_mimes[i]; i++) {
        size_t n = 0;
        void *p;
        if (!SDL_HasClipboardData(k_paste_mimes[i])) continue;
        p = SDL_GetClipboardData(k_paste_mimes[i], &n);
        if (!p) continue;
        if (n == 0u || n > ((size_t)1 << 31)) {     /* empty or absurd: next flavour */
            SDL_free(p);
            continue;
        }
        *data = (uint8_t *)malloc(n + 1u);          /* callers free() it, not SDL_free */
        if (!*data) {
            SDL_free(p);
            return false;
        }
        memcpy(*data, p, n);
        (*data)[n] = 0u;
        SDL_free(p);
        *len = n;
        if (mime && mime_cap) {
            /* report canonical names for the BMP aliases */
            const char *m = (i >= 1u && i <= 3u) ? "image/bmp" : k_paste_mimes[i];
            SDL_strlcpy(mime, m, mime_cap);
        }
        return true;
    }
    return false;
}

bool pal_clip_set_image_png(const uint8_t *png, size_t len)
{
    clip_offer *o;
    if (!png || len == 0u || !clip_ready()) return false;
    o = (clip_offer *)calloc(1u, sizeof *o);
    if (!o) return false;
    o->png = (uint8_t *)malloc(len);
    if (!o->png) {
        free(o);
        return false;
    }
    memcpy(o->png, png, len);
    o->png_len = len;
    return clip_offer_set(o);
}

bool pal_clip_set_image_bgra(const uint8_t *png, size_t len, const uint8_t *bgra,
                             int32_t w, int32_t h, size_t stride)
{
    clip_offer *o;
    if (!bgra || !clip_ready()) return false;
    o = (clip_offer *)calloc(1u, sizeof *o);
    if (!o) return false;
#if defined(__APPLE__)
    o->alt = pal__enc_tiff(bgra, w, h, stride, &o->alt_len);
#else
    o->alt = pal__enc_bmp(bgra, w, h, stride, true, &o->alt_len);
#endif
    o->alt_mime = CLIP_ALT_MIME;
    if (!o->alt) {
        clip_offer_free(o);
        return false;
    }
    if (png && len) {
        o->png = (uint8_t *)malloc(len);
        if (!o->png) {
            clip_offer_free(o);
            return false;
        }
        memcpy(o->png, png, len);
        o->png_len = len;
    }
    return clip_offer_set(o);
}

bool pal_clip_set_text(const char *utf8)
{
    if (!utf8 || !clip_ready()) return false;
    if (!SDL_SetClipboardText(utf8)) {
        pal_log(PAL_LOG_WARN, "clipboard: %s", SDL_GetError());
        return false;
    }
    return true;
}

char *pal_clip_get_text(void)
{
    char *s, *copy;
    if (!clip_ready() || !SDL_HasClipboardText()) return NULL;
    s = SDL_GetClipboardText();
    if (!s) return NULL;
    copy = pal__strdup(s);                  /* malloc'ed for the caller */
    SDL_free(s);
    return copy;
}
#endif /* !_WIN32 */
