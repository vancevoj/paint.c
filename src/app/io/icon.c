/* icon.c - the paint.c window icon (an original design, assets/icons; the
 * PNGs are embedded by src/app/io/icon_data.c, generated with
 * packaging/icons/make_icons.py). Decoding uses our PNG codec.
 *
 * Thread rules: app_icon_rgba runs on any thread; app_set_window_icon on
 * the main thread. */
#include "io_internal.h"

#include <stdlib.h>
#include <string.h>

extern const unsigned char *const io_icon_pngs[3];
extern const size_t io_icon_png_sizes[3];
extern const int io_icon_png_dims[3];

/* Decode embedded icon k into straight RGBA. */
static uint8_t *decode(int k, int32_t *dim)
{
    const pc_codec *png = pc_codec_by_id("png");
    pc_codec_limits lim;
    pc_doc *doc = NULL;
    pc_image_meta m;
    uint8_t *out = NULL;
    pc_px32 *row;
    if (!png || !png->load) return NULL;
    memset(&m, 0, sizeof m);
    pc_codec_limits_default(&lim);
    lim.max_w = lim.max_h = 1024u;
    if (png->load(io_icon_pngs[k], io_icon_png_sizes[k], &lim, &doc, &m) != PC_OK || !doc) {
        pc_meta_free(&m);
        return NULL;
    }
    pc_meta_free(&m);
    out = (uint8_t *)malloc((size_t)doc->w * (size_t)doc->h * 4u);
    row = (pc_px32 *)malloc((size_t)doc->w * sizeof *row);
    if (out && row) {
        for (uint32_t y = 0; y < doc->h; y++) {
            (void)pc_comp_rect(doc, pc_rect_make(0, (int32_t)y, (int32_t)doc->w, 1), row, doc->w,
                               NULL);
            for (uint32_t x = 0; x < doc->w; x++) {
                uint8_t *o = out + ((size_t)y * doc->w + x) * 4u;
                o[0] = row[x].r;
                o[1] = row[x].g;
                o[2] = row[x].b;
                o[3] = row[x].a;
            }
        }
        *dim = (int32_t)doc->w;
    } else {
        free(out);
        out = NULL;
    }
    free(row);
    pc_doc_destroy(doc);
    return out;
}

/* Box-filtered (premultiplied) resample of a square RGBA image. */
static uint8_t *resample(const uint8_t *src, int32_t sd, int32_t dd)
{
    uint8_t *out = (uint8_t *)malloc((size_t)dd * (size_t)dd * 4u);
    if (!out) return NULL;
    for (int32_t y = 0; y < dd; y++)
        for (int32_t x = 0; x < dd; x++) {
            int32_t x0 = x * sd / dd, x1 = (x + 1) * sd / dd;
            int32_t y0 = y * sd / dd, y1 = (y + 1) * sd / dd;
            uint64_t acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            uint8_t *o = out + ((size_t)y * (size_t)dd + (size_t)x) * 4u;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (int32_t v = y0; v < y1 && v < sd; v++)
                for (int32_t u = x0; u < x1 && u < sd; u++) {
                    const uint8_t *p = src + ((size_t)v * (size_t)sd + (size_t)u) * 4u;
                    acc[0] += (uint64_t)p[0] * p[3];
                    acc[1] += (uint64_t)p[1] * p[3];
                    acc[2] += (uint64_t)p[2] * p[3];
                    acc[3] += p[3];
                    cnt++;
                }
            for (int c = 0; c < 3; c++)
                o[c] = (uint8_t)(acc[3] ? (acc[c] + acc[3] / 2u) / acc[3] : 0u);
            o[3] = (uint8_t)(cnt ? (acc[3] + cnt / 2u) / cnt : 0u);
        }
    return out;
}

uint8_t *app_icon_rgba(int32_t size)
{
    int32_t dim = 0;
    uint8_t *big, *out;
    if (size < 1 || size > 1024) return NULL;
    for (int k = 0; k < 3; k++)
        if (io_icon_png_dims[k] == size) return decode(k, &dim);
    big = decode(2, &dim);
    if (!big) return NULL;
    out = resample(big, dim, size);
    free(big);
    return out;
}

/* An SDL-owned surface with a copy of the pixels (backends may keep a
 * reference to the icon). */
static SDL_Surface *surface_of(const uint8_t *px, int32_t d)
{
    SDL_Surface *s = SDL_CreateSurface(d, d, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    for (int32_t y = 0; y < d; y++)
        memcpy((uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch,
               px + (size_t)y * (size_t)d * 4u, (size_t)d * 4u);
    return s;
}

bool app_set_window_icon(app *a)
{
    SDL_Surface *main_s = NULL;
    uint8_t *px[3] = { NULL, NULL, NULL };
    bool ok = false;
    if (!a || !a->win) return false;
    for (int k = 0; k < 3; k++) {
        int32_t d = 0;
        px[k] = decode(k, &d);
        if (!px[k] || d != io_icon_png_dims[k]) {
            free(px[k]);
            px[k] = NULL;
        }
    }
    if (px[2]) main_s = surface_of(px[2], io_icon_png_dims[2]);
    if (main_s) {
        for (int k = 0; k < 2; k++) {
            SDL_Surface *alt = px[k] ? surface_of(px[k], io_icon_png_dims[k]) : NULL;
            if (alt) {
                (void)SDL_AddSurfaceAlternateImage(main_s, alt);
                SDL_DestroySurface(alt);
            }
        }
        ok = SDL_SetWindowIcon(a->win, main_s);
        if (!ok) pal_log(PAL_LOG_INFO, "window icon: %s", SDL_GetError());
        SDL_DestroySurface(main_s);
    }
    for (int k = 0; k < 3; k++) free(px[k]);
    return ok;
}
