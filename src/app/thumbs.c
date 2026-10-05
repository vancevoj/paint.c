/* thumbs.c - image list and Layers window thumbnails: cheap point-sampled
 * downsamples (2 x 2 samples per thumbnail pixel, composited per sample
 * with pc_composite_span), refreshed lazily and throttled (W-LAY-THUMBS).
 * Main thread (textures). */
#include "app_internal.h"

#include <stdlib.h>
#include <string.h>

#define DOC_THUMB_MAX   112      /* px, longest side */
#define LAYER_THUMB_MAX 72
#define THROTTLE_MS     250u

static pc_px32 layer_px(const pc_doc *d, const pc_layer *l, uint32_t x, uint32_t y)
{
    const pc_tile *t;
    pc_px32 z;
    memset(&z, 0, sizeof z);
    if (x >= d->w || y >= d->h) return z;
    t = l->grid[(size_t)(y >> PC_TILE_SHIFT) * l->tiles_x + (x >> PC_TILE_SHIFT)];
    if (!t || t->bpp != 4u) return z;
    memcpy(&z, t->data + ((size_t)(y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                          (x & (PC_TILE_DIM - 1u))) * 4u, 4u);
    return z;
}

/* Fit w x h into max x max keeping the aspect ratio, at least 1 x 1. */
static void fit(uint32_t w, uint32_t h, int32_t max, int32_t *tw, int32_t *th)
{
    if (w >= h) {
        *tw = (int32_t)(w < (uint32_t)max ? w : (uint32_t)max);
        *th = (int32_t)((uint64_t)h * (uint64_t)*tw / w);
    } else {
        *th = (int32_t)(h < (uint32_t)max ? h : (uint32_t)max);
        *tw = (int32_t)((uint64_t)w * (uint64_t)*th / h);
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* Render into rgba (tw x th, straight RGBA bytes). layer NULL = composite
 * of the visible layers. */
static void render(const pc_doc *d, const pc_layer *only, int32_t tw, int32_t th, uint8_t *rgba)
{
    for (int32_t v = 0; v < th; v++) {
        for (int32_t u = 0; u < tw; u++) {
            uint32_t acc[4] = { 0, 0, 0, 0 };
            for (int s = 0; s < 4; s++) {
                double fx = ((double)u + 0.25 + 0.5 * (double)(s & 1)) / (double)tw;
                double fy = ((double)v + 0.25 + 0.5 * (double)(s >> 1)) / (double)th;
                uint32_t x = (uint32_t)(fx * (double)d->w), y = (uint32_t)(fy * (double)d->h);
                pc_px32 px;
                memset(&px, 0, sizeof px);
                if (only) {
                    px = layer_px(d, only, x, y);
                } else {
                    for (uint32_t i = 0; i < d->n_layers; i++) {
                        const pc_layer *l = d->stack[i];
                        pc_px32 sp;
                        if (!l->visible || l->opacity == 0u) continue;
                        sp = layer_px(d, l, x, y);
                        pc_composite_span(&px, &sp, 1u, l->mode, l->opacity);
                    }
                }
                acc[0] += (uint32_t)px.r * px.a;
                acc[1] += (uint32_t)px.g * px.a;
                acc[2] += (uint32_t)px.b * px.a;
                acc[3] += px.a;
            }
            {
                uint8_t *o = rgba + ((size_t)v * (size_t)tw + (size_t)u) * 4u;
                if (acc[3]) {
                    o[0] = (uint8_t)((acc[0] + acc[3] / 2u) / acc[3]);
                    o[1] = (uint8_t)((acc[1] + acc[3] / 2u) / acc[3]);
                    o[2] = (uint8_t)((acc[2] + acc[3] / 2u) / acc[3]);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = (uint8_t)((acc[3] + 2u) / 4u);
            }
        }
    }
}

static SDL_Texture *upload(app *a, SDL_Texture *tex, int32_t *cw, int32_t *ch, int32_t tw,
                           int32_t th, const uint8_t *rgba)
{
    if (tex && (*cw != tw || *ch != th)) {
        SDL_DestroyTexture(tex);
        tex = NULL;
    }
    if (!tex) {
        tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (!tex) return NULL;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    SDL_UpdateTexture(tex, NULL, rgba, tw * 4);
    *cw = tw;
    *ch = th;
    return tex;
}

static app_layer_thumb *lthumb(app_doc *d, uint32_t id, bool create)
{
    for (uint32_t i = 0; i < d->n_lthumbs; i++)
        if (d->lthumbs[i].layer_id == id) return &d->lthumbs[i];
    if (!create) return NULL;
    if (d->n_lthumbs == d->cap_lthumbs) {
        uint32_t nc = d->cap_lthumbs ? d->cap_lthumbs * 2u : 8u;
        app_layer_thumb *n = (app_layer_thumb *)realloc(d->lthumbs, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        d->lthumbs = n;
        d->cap_lthumbs = nc;
    }
    memset(&d->lthumbs[d->n_lthumbs], 0, sizeof d->lthumbs[0]);
    d->lthumbs[d->n_lthumbs].layer_id = id;
    return &d->lthumbs[d->n_lthumbs++];
}

void app_thumbs_update(app *a, app_doc *d, bool layers)
{
    uint8_t *buf = NULL;
    int32_t tw, th;
    bool due = a->now - d->thumb_time >= THROTTLE_MS;
    if (!d->doc) return;
    if (!d->thumb || (d->thumb_gen != d->doc->gen && due)) {
        fit(d->doc->w, d->doc->h, DOC_THUMB_MAX, &tw, &th);
        buf = (uint8_t *)malloc((size_t)tw * (size_t)th * 4u);
        if (buf) {
            render(d->doc, NULL, tw, th, buf);
            d->thumb = upload(a, d->thumb, &d->thumb_w, &d->thumb_h, tw, th, buf);
            free(buf);
            buf = NULL;
        }
        d->thumb_gen = d->doc->gen;
        d->thumb_time = a->now;
    } else if (d->thumb_gen != d->doc->gen) {
        app_request_frame_at(a, d->thumb_time + THROTTLE_MS);
    }
    if (!layers) return;
    /* drop thumbnails of layers that left the document */
    for (uint32_t i = 0; i < d->n_lthumbs;) {
        if (!pc_doc_layer_by_id(d->doc, d->lthumbs[i].layer_id)) {
            if (d->lthumbs[i].tex) SDL_DestroyTexture(d->lthumbs[i].tex);
            d->lthumbs[i] = d->lthumbs[--d->n_lthumbs];
        } else {
            i++;
        }
    }
    for (uint32_t i = 0; i < d->doc->n_layers; i++) {
        const pc_layer *l = d->doc->stack[i];
        app_layer_thumb *t = lthumb(d, l->id, true);
        if (!t) continue;
        if (t->tex && t->gen == l->gen) continue;
        if (t->tex && a->now - d->lthumb_time < THROTTLE_MS) {
            app_request_frame_at(a, d->lthumb_time + THROTTLE_MS);
            continue;
        }
        fit(d->doc->w, d->doc->h, LAYER_THUMB_MAX, &tw, &th);
        buf = (uint8_t *)malloc((size_t)tw * (size_t)th * 4u);
        if (!buf) continue;
        render(d->doc, l, tw, th, buf);
        {
            int32_t cw = t->tex ? t->w : 0, chh = t->tex ? t->h : 0;
            t->tex = upload(a, t->tex, &cw, &chh, tw, th, buf);
            t->w = cw;
            t->h = chh;
        }
        free(buf);
        t->gen = l->gen;
        d->lthumb_time = a->now;
    }
}

void app_thumbs_free(app_doc *d)
{
    if (!d) return;
    if (d->thumb) SDL_DestroyTexture(d->thumb);
    d->thumb = NULL;
    d->thumb_gen = 0;
    for (uint32_t i = 0; i < d->n_lthumbs; i++)
        if (d->lthumbs[i].tex) SDL_DestroyTexture(d->lthumbs[i].tex);
    free(d->lthumbs);
    d->lthumbs = NULL;
    d->n_lthumbs = d->cap_lthumbs = 0;
}
