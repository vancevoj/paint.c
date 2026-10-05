/* ui_atlas.c - the coverage atlas (glyphs, shape sprites, icons).
 *
 * Pages are UI_ATLAS_DIM square A8 buffers on the CPU (authoritative, P-03)
 * mirrored into RGBA textures (white, alpha = coverage) that are updated
 * from the dirty rectangle before each render. Every page starts with a
 * 4 x 4 white block used for untextured geometry, so shapes and text batch
 * into one draw call. Allocation is shelf packing with one texel of
 * padding. When the pages are exhausted the atlas is cleared at the start
 * of the next frame and sprites are rasterized again on demand. */
#include "ui_internal.h"

#include <stdlib.h>
#include <string.h>

static bool page_texture(ui_ctx *ctx, ui_atlas_page *pg)
{
    pg->tex = SDL_CreateTexture(ctx->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                UI_ATLAS_DIM, UI_ATLAS_DIM);
    if (!pg->tex) return false;
    SDL_SetTextureBlendMode(pg->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(pg->tex, SDL_SCALEMODE_NEAREST);
    pg->dx0 = 0; pg->dy0 = 0; pg->dx1 = UI_ATLAS_DIM; pg->dy1 = UI_ATLAS_DIM;
    return true;
}

static void page_clear(ui_atlas_page *pg)
{
    memset(pg->a8, 0, (size_t)UI_ATLAS_DIM * UI_ATLAS_DIM);
    pg->nshelves = 0;
    pg->next_y = 0;
    for (int y = 0; y < 4; y++) memset(pg->a8 + (size_t)y * UI_ATLAS_DIM, 255, 4u);
    pg->shelves[0].y = 0;
    pg->shelves[0].h = 4;
    pg->shelves[0].x = 5;
    pg->nshelves = 1;
    pg->next_y = 5;
    pg->dx0 = 0; pg->dy0 = 0; pg->dx1 = UI_ATLAS_DIM; pg->dy1 = UI_ATLAS_DIM;
}

static bool page_add(ui_ctx *ctx)
{
    ui_atlas_page *pg;
    if (ctx->npages >= UI_ATLAS_MAX_PAGES) return false;
    pg = &ctx->pages[ctx->npages];
    memset(pg, 0, sizeof *pg);
    pg->a8 = (uint8_t *)malloc((size_t)UI_ATLAS_DIM * UI_ATLAS_DIM);
    if (!pg->a8) return false;
    if (!page_texture(ctx, pg)) { free(pg->a8); pg->a8 = NULL; return false; }
    page_clear(pg);
    ctx->npages++;
    return true;
}

static void cache_clear(ui_cache *c)
{
    if (c->e) memset(c->e, 0, (size_t)c->cap * sizeof(ui_cache_entry));
    c->n = 0;
}

bool ui_atlas_init(ui_ctx *ctx)
{
    ctx->npages = 0;
    return page_add(ctx);
}

void ui_atlas_destroy(ui_ctx *ctx)
{
    for (int32_t i = 0; i < ctx->npages; i++) {
        if (ctx->pages[i].tex) SDL_DestroyTexture(ctx->pages[i].tex);
        free(ctx->pages[i].a8);
    }
    ctx->npages = 0;
    free(ctx->cache.e);
    memset(&ctx->cache, 0, sizeof ctx->cache);
    free(ctx->scratch);
    ctx->scratch = NULL;
    ctx->scratch_cap = 0;
}

void ui_atlas_frame(ui_ctx *ctx)
{
    if (ctx->atlas_reupload) {
        /* device reset: textures are gone, the CPU pages are not */
        for (int32_t i = 0; i < ctx->npages; i++) {
            if (ctx->pages[i].tex) SDL_DestroyTexture(ctx->pages[i].tex);
            ctx->pages[i].tex = NULL;
            (void)page_texture(ctx, &ctx->pages[i]);
        }
        for (int32_t i = 0; i < 4; i++) {
            if (ctx->checker_tex[i]) SDL_DestroyTexture(ctx->checker_tex[i]);
            ctx->checker_tex[i] = NULL;
        }
        for (int32_t i = 0; i < UI_WHEEL_CACHE; i++) {
            if (ctx->wheels[i].tex) SDL_DestroyTexture(ctx->wheels[i].tex);
            ctx->wheels[i].tex = NULL;
        }
        ctx->atlas_reupload = false;
    }
    if (ctx->atlas_overflow || ctx->npages > UI_ATLAS_SOFT_PAGES) {
        while (ctx->npages > 1) {
            ui_atlas_page *pg = &ctx->pages[--ctx->npages];
            if (pg->tex) SDL_DestroyTexture(pg->tex);
            free(pg->a8);
            memset(pg, 0, sizeof *pg);
        }
        page_clear(&ctx->pages[0]);
        cache_clear(&ctx->cache);
        ctx->atlas_overflow = false;
    }
}

static bool page_alloc(ui_atlas_page *pg, int32_t w, int32_t h, int32_t *ox, int32_t *oy)
{
    int32_t pw = w + 1, ph = h + 1, best = -1;
    for (int32_t i = 0; i < pg->nshelves; i++) {
        ui_shelf *s = &pg->shelves[i];
        if (s->h >= ph && s->h <= ph + ph / 2 + 2 && UI_ATLAS_DIM - s->x >= pw) {
            if (best < 0 || s->h < pg->shelves[best].h) best = i;
        }
    }
    if (best < 0) {
        int32_t sh = (ph + 3) & ~3;
        if (pg->nshelves >= UI_MAX_SHELVES || UI_ATLAS_DIM - pg->next_y < sh) return false;
        best = pg->nshelves++;
        pg->shelves[best].y = pg->next_y;
        pg->shelves[best].h = sh;
        pg->shelves[best].x = 0;
        pg->next_y += sh;
    }
    *ox = pg->shelves[best].x;
    *oy = pg->shelves[best].y;
    pg->shelves[best].x += pw;
    return true;
}

bool ui_atlas_alloc(ui_ctx *ctx, int32_t w, int32_t h, ui_sprite *sp, uint8_t **dst)
{
    int32_t x = 0, y = 0, pi;
    ui_atlas_page *pg;
    if (w <= 0 || h <= 0 || w > UI_ATLAS_DIM - 1 || h > UI_ATLAS_DIM - 1) return false;
    for (pi = 0; pi < ctx->npages; pi++)
        if (page_alloc(&ctx->pages[pi], w, h, &x, &y)) break;
    if (pi == ctx->npages) {
        if (!page_add(ctx) || !page_alloc(&ctx->pages[pi], w, h, &x, &y)) {
            ctx->atlas_overflow = true;
            ctx->want_frame = true;
            return false;
        }
    }
    pg = &ctx->pages[pi];
    sp->page = (uint16_t)pi;
    sp->x = (uint16_t)x;
    sp->y = (uint16_t)y;
    sp->w = (uint16_t)w;
    sp->h = (uint16_t)h;
    if (pg->dx1 <= pg->dx0) {
        pg->dx0 = x; pg->dy0 = y; pg->dx1 = x + w; pg->dy1 = y + h;
    } else {
        pg->dx0 = ui_mini(pg->dx0, x); pg->dy0 = ui_mini(pg->dy0, y);
        pg->dx1 = ui_maxi(pg->dx1, x + w); pg->dy1 = ui_maxi(pg->dy1, y + h);
    }
    *dst = pg->a8 + (size_t)y * UI_ATLAS_DIM + (size_t)x;
    for (int32_t r = 0; r < h; r++) memset(*dst + (size_t)r * UI_ATLAS_DIM, 0, (size_t)w);
    return true;
}

void ui_atlas_upload(ui_ctx *ctx)
{
    uint8_t *rgba = NULL;
    size_t cap = 0;
    for (int32_t i = 0; i < ctx->npages; i++) {
        ui_atlas_page *pg = &ctx->pages[i];
        int32_t w = pg->dx1 - pg->dx0, h = pg->dy1 - pg->dy0;
        size_t need;
        SDL_Rect rc;
        if (w <= 0 || h <= 0 || !pg->tex) continue;
        need = (size_t)w * (size_t)h * 4u;
        if (need > cap) {
            uint8_t *n = (uint8_t *)realloc(rgba, need);
            if (!n) break;
            rgba = n;
            cap = need;
        }
        for (int32_t y = 0; y < h; y++) {
            const uint8_t *src = pg->a8 + (size_t)(pg->dy0 + y) * UI_ATLAS_DIM + (size_t)pg->dx0;
            uint8_t *d = rgba + (size_t)y * (size_t)w * 4u;
            for (int32_t x = 0; x < w; x++) {
                d[4 * x + 0] = 255u;
                d[4 * x + 1] = 255u;
                d[4 * x + 2] = 255u;
                d[4 * x + 3] = src[x];
            }
        }
        rc.x = pg->dx0; rc.y = pg->dy0; rc.w = w; rc.h = h;
        SDL_UpdateTexture(pg->tex, &rc, rgba, w * 4);
        pg->dx0 = pg->dy0 = pg->dx1 = pg->dy1 = 0;
    }
    free(rgba);
}

/* ---- sprite cache (open addressing, key 0 = empty) ----------------------- */
static uint32_t key_hash(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return (uint32_t)k;
}

bool ui_cache_get(ui_ctx *ctx, uint64_t key, ui_sprite *sp)
{
    ui_cache *c = &ctx->cache;
    uint32_t i;
    if (!c->cap) return false;
    i = key_hash(key) & (c->cap - 1u);
    for (;;) {
        if (c->e[i].key == key) { *sp = c->e[i].sp; return true; }
        if (c->e[i].key == 0) return false;
        i = (i + 1u) & (c->cap - 1u);
    }
}

void ui_cache_put(ui_ctx *ctx, uint64_t key, const ui_sprite *sp)
{
    ui_cache *c = &ctx->cache;
    uint32_t i;
    if (key == 0) return;
    if ((c->n + 1u) * 10u >= c->cap * 7u) {
        uint32_t ncap = c->cap ? c->cap * 2u : 1024u;
        ui_cache_entry *ne = (ui_cache_entry *)calloc(ncap, sizeof(ui_cache_entry));
        if (!ne) return;
        for (uint32_t k = 0; k < c->cap; k++) {
            if (!c->e[k].key) continue;
            i = key_hash(c->e[k].key) & (ncap - 1u);
            while (ne[i].key) i = (i + 1u) & (ncap - 1u);
            ne[i] = c->e[k];
        }
        free(c->e);
        c->e = ne;
        c->cap = ncap;
    }
    i = key_hash(key) & (c->cap - 1u);
    while (c->e[i].key && c->e[i].key != key) i = (i + 1u) & (c->cap - 1u);
    if (!c->e[i].key) c->n++;
    c->e[i].key = key;
    c->e[i].sp = *sp;
}

uint8_t *ui_scratch(ui_ctx *ctx, size_t n)
{
    if (n > ctx->scratch_cap) {
        uint8_t *b = (uint8_t *)realloc(ctx->scratch, n);
        if (!b) return NULL;
        ctx->scratch = b;
        ctx->scratch_cap = n;
    }
    memset(ctx->scratch, 0, n);
    return ctx->scratch;
}
