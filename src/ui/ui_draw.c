/* ui_draw.c - draw lists, primitives, coverage sprites, text and replay. */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TEXEL (1.0f / (float)UI_ATLAS_DIM)

/* ---- draw list storage --------------------------------------------------- */
void ui_dl_reset(ui_dl *dl)
{
    dl->nv = 0;
    dl->ni = 0;
    dl->nc = 0;
}

void ui_dl_free(ui_dl *dl)
{
    free(dl->v);
    free(dl->ix);
    free(dl->cmd);
    memset(dl, 0, sizeof *dl);
}

static bool grow(void **p, int32_t *cap, int32_t need, size_t elem)
{
    int32_t ncap;
    size_t bytes;
    void *n;
    if (need <= *cap) return true;
    if (need > INT32_MAX / 2) return false;
    ncap = *cap ? *cap : 256;
    while (ncap < need) ncap *= 2;
    if (!pc_mul_size((size_t)ncap, elem, &bytes)) return false;
    n = realloc(*p, bytes);
    if (!n) return false;
    *p = n;
    *cap = ncap;
    return true;
}

static ui_dl *cur_dl(ui_ctx *ctx) { return &ctx->roots[ctx->cur_root].dl; }

static bool rect_eq(ui_rect a, ui_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* Reserve nv vertices and ni indices in a command for (tex, filter, clip).
 * Indices written by the caller are relative to *base. */
static SDL_Vertex *prim(ui_ctx *ctx, SDL_Texture *tex, int32_t filter, int32_t nv, int32_t ni,
                        int **ix, int32_t *base)
{
    ui_dl *dl = cur_dl(ctx);
    ui_rect clip = ctx->clip[ctx->clip_depth - 1];
    ui_cmd *c = dl->nc ? &dl->cmd[dl->nc - 1] : NULL;
    SDL_Vertex *v;
    if (ui_rect_empty(clip) || !tex) return NULL;
    if (!c || c->fn || c->tex != tex || c->filter != filter || !rect_eq(c->clip, clip)) {
        if (!grow((void **)&dl->cmd, &dl->cc, dl->nc + 1, sizeof(ui_cmd))) return NULL;
        c = &dl->cmd[dl->nc++];
        memset(c, 0, sizeof *c);
        c->tex = tex;
        c->filter = filter;
        c->clip = clip;
        c->v0 = dl->nv;
        c->i0 = dl->ni;
    }
    if (!grow((void **)&dl->v, &dl->cv, dl->nv + nv, sizeof(SDL_Vertex)) ||
        !grow((void **)&dl->ix, &dl->ci, dl->ni + ni, sizeof(int)))
        return NULL;
    v = dl->v + dl->nv;
    *ix = dl->ix + dl->ni;
    *base = dl->nv - c->v0;
    dl->nv += nv;
    dl->ni += ni;
    c->nv += nv;
    c->ni += ni;
    return v;
}

static SDL_FColor fcol(ui_color c)
{
    SDL_FColor f;
    f.r = (float)c.r / 255.0f;
    f.g = (float)c.g / 255.0f;
    f.b = (float)c.b / 255.0f;
    f.a = (float)c.a / 255.0f;
    return f;
}

static void vtx(SDL_Vertex *v, float x, float y, SDL_FColor c, float u, float w)
{
    v->position.x = x;
    v->position.y = y;
    v->color = c;
    v->tex_coord.x = u;
    v->tex_coord.y = w;
}

/* Atlas page to use for untextured shapes: the current batch's page when it
 * is an atlas page (every page has the white block), else page 0. */
static SDL_Texture *solid_tex(ui_ctx *ctx)
{
    ui_dl *dl = cur_dl(ctx);
    if (dl->nc) {
        SDL_Texture *t = dl->cmd[dl->nc - 1].tex;
        for (int32_t i = 0; i < ctx->npages; i++)
            if (ctx->pages[i].tex == t && t) return t;
    }
    return ctx->pages[0].tex;
}

static void quad_tex(ui_ctx *ctx, SDL_Texture *tex, int32_t filter, float x0, float y0, float x1,
                     float y1, float u0, float v0, float u1, float v1, ui_color c)
{
    int *ix;
    int32_t b;
    SDL_FColor fc;
    SDL_Vertex *v;
    if (c.a == 0 || x1 <= x0 || y1 <= y0) return;
    v = prim(ctx, tex, filter, 4, 6, &ix, &b);
    if (!v) return;
    fc = fcol(c);
    vtx(&v[0], x0, y0, fc, u0, v0);
    vtx(&v[1], x1, y0, fc, u1, v0);
    vtx(&v[2], x1, y1, fc, u1, v1);
    vtx(&v[3], x0, y1, fc, u0, v1);
    ix[0] = b; ix[1] = b + 1; ix[2] = b + 2;
    ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
}

static void quad_solid(ui_ctx *ctx, float x0, float y0, float x1, float y1, ui_color c)
{
    /* a two-texel span keeps the software renderer's rectangle fast path
     * from seeing a degenerate source rectangle */
    quad_tex(ctx, solid_tex(ctx), -1, x0, y0, x1, y1, TEXEL, TEXEL, 3.0f * TEXEL, 3.0f * TEXEL, c);
}

void ui_draw_sprite_flip(ui_ctx *ctx, const ui_sprite *sp, float x, float y, bool fx, bool fy,
                         ui_color c)
{
    float u0, v0, u1, v1;
    if (!sp->w || sp->page >= (uint16_t)ctx->npages) return;
    u0 = (float)sp->x * TEXEL; v0 = (float)sp->y * TEXEL;
    u1 = (float)(sp->x + sp->w) * TEXEL; v1 = (float)(sp->y + sp->h) * TEXEL;
    if (fx) { float t = u0; u0 = u1; u1 = t; }
    if (fy) { float t = v0; v0 = v1; v1 = t; }
    quad_tex(ctx, ctx->pages[sp->page].tex, -1, x, y, x + (float)sp->w, y + (float)sp->h, u0, v0,
             u1, v1, c);
}

void ui_draw_sprite(ui_ctx *ctx, const ui_sprite *sp, float x, float y, ui_color c)
{
    ui_draw_sprite_flip(ctx, sp, x, y, false, false, c);
}

/* Part of a sprite (sx, sy, sw, sh in sprite texels) stretched to a rect. */
static void sprite_part(ui_ctx *ctx, const ui_sprite *sp, int32_t sx, int32_t sy, int32_t sw,
                        int32_t sh, float x0, float y0, float x1, float y1, ui_color c)
{
    float u0 = (float)(sp->x + sx) * TEXEL, v0 = (float)(sp->y + sy) * TEXEL;
    quad_tex(ctx, ctx->pages[sp->page].tex, -1, x0, y0, x1, y1, u0, v0,
             u0 + (float)sw * TEXEL, v0 + (float)sh * TEXEL, c);
}

/* ---- clipping ------------------------------------------------------------ */
void ui_clip_push_raw(ui_ctx *ctx, ui_rect r)
{
    if (ctx->clip_depth >= UI_MAX_CLIP) return;
    ctx->clip[ctx->clip_depth++] = r;
}

void ui_push_clip(ui_ctx *ctx, ui_rect r)
{
    ui_rect c = ctx->clip_depth ? ctx->clip[ctx->clip_depth - 1] : r;
    ui_clip_push_raw(ctx, ui_rect_intersect(c, r));
}

void ui_pop_clip(ui_ctx *ctx)
{
    if (ctx->clip_depth > 1) ctx->clip_depth--;
}

ui_rect ui_current_clip(const ui_ctx *ctx)
{
    return ctx->clip_depth ? ctx->clip[ctx->clip_depth - 1] : ui_rect_make(0, 0, 0, 0);
}

/* ---- coverage sprites ---------------------------------------------------- */
static bool make_sprite(ui_ctx *ctx, uint64_t key, const uint8_t *cov, int32_t w, int32_t h,
                        ui_sprite *sp)
{
    uint8_t *dst;
    if (!ui_atlas_alloc(ctx, w, h, sp, &dst)) return false;
    for (int32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)y * UI_ATLAS_DIM, cov + (size_t)y * (size_t)w, (size_t)w);
    sp->ox = 0;
    sp->oy = 0;
    ui_cache_put(ctx, key, sp);
    return true;
}

/* Top-left corner of a rounded rectangle with radius r, outline thickness t
 * (0 = filled). The sprite is R x R with R = max(ceil(r), t). */
static bool corner_sprite(ui_ctx *ctx, float r, int32_t t, ui_sprite *sp)
{
    uint32_t rq = (uint32_t)(r * 4.0f + 0.5f);
    uint64_t key = ((uint64_t)UI_KEY_CORNER << 56) | ((uint64_t)rq << 16) | (uint64_t)(t & 0xFFFF);
    int32_t R;
    uint8_t *cov;
    ui_path *p = &ctx->scratch_path;
    if (ui_cache_get(ctx, key, sp)) return sp->w > 0;
    r = (float)rq * 0.25f;
    R = ui_maxi((int32_t)ceilf(r), t);
    if (R <= 0) return false;
    cov = ui_scratch(ctx, (size_t)R * (size_t)R);
    if (!cov) return false;
    ui_path_reset(p);
    if (r > 0.0f) {
        ui_path_move(p, 0.0f, r);
        ui_path_arc(p, r, r, 0.0f, false, true, r, 0.0f);
        ui_path_line(p, (float)R, 0.0f);
        ui_path_line(p, (float)R, (float)R);
        ui_path_line(p, 0.0f, (float)R);
        ui_path_close(p);
    } else {
        ui_path_rect(p, 0.0f, 0.0f, (float)R, (float)R);
    }
    if (ui_raster_fill(p, UI_FILL_NONZERO, true, cov, R, R, R, UI_RASTER_SET) != PC_OK)
        return false;
    if (t > 0) {
        float ri = r - (float)t, ft = (float)t;
        ui_path_reset(p);
        if (ri > 0.0f) {
            ui_path_move(p, ft, ft + ri);
            ui_path_arc(p, ri, ri, 0.0f, false, true, ft + ri, ft);
            ui_path_line(p, (float)R, ft);
            ui_path_line(p, (float)R, (float)R);
            ui_path_line(p, ft, (float)R);
            ui_path_close(p);
        } else if (R > t) {
            ui_path_rect(p, ft, ft, (float)(R - t), (float)(R - t));
        }
        if (p->n &&
            ui_raster_fill(p, UI_FILL_NONZERO, true, cov, R, R, R, UI_RASTER_ERASE) != PC_OK)
            return false;
    }
    return make_sprite(ctx, key, cov, R, R, sp);
}

/* Disc (t == 0) or ring of radius r centered at a quarter-pixel position.
 * *ix, *iy receive the integer position to draw the sprite at. */
static bool disc_sprite(ui_ctx *ctx, float cx, float cy, float r, float t, ui_sprite *sp,
                        int32_t *ix, int32_t *iy)
{
    float qx = floorf(cx * 4.0f + 0.5f) * 0.25f, qy = floorf(cy * 4.0f + 0.5f) * 0.25f;
    uint32_t rq = (uint32_t)(r * 4.0f + 0.5f), tq = (uint32_t)(t * 4.0f + 0.5f);
    float x0, y0, lx, ly;
    uint32_t fxq, fyq;
    uint64_t key;
    int32_t w, h;
    uint8_t *cov;
    ui_path *p = &ctx->scratch_path;
    if (rq == 0 || rq > 4095u) return false;
    r = (float)rq * 0.25f;
    if (tq > rq) tq = rq;
    t = (float)tq * 0.25f;
    x0 = floorf(qx - r - 1.0f);
    y0 = floorf(qy - r - 1.0f);
    lx = qx - x0; ly = qy - y0;
    fxq = (uint32_t)((lx - floorf(lx)) * 4.0f + 0.5f) & 3u;
    fyq = (uint32_t)((ly - floorf(ly)) * 4.0f + 0.5f) & 3u;
    *ix = (int32_t)x0;
    *iy = (int32_t)y0;
    key = ((uint64_t)(tq ? UI_KEY_RING : UI_KEY_DISC) << 56) | ((uint64_t)rq << 24) |
          ((uint64_t)tq << 8) | (fxq << 2) | fyq;
    if (ui_cache_get(ctx, key, sp)) return sp->w > 0;
    w = (int32_t)ceilf(lx + r + 1.0f);
    h = (int32_t)ceilf(ly + r + 1.0f);
    cov = ui_scratch(ctx, (size_t)w * (size_t)h);
    if (!cov) return false;
    ui_path_reset(p);
    ui_path_ellipse(p, lx, ly, r, r);
    if (ui_raster_fill(p, UI_FILL_NONZERO, true, cov, w, h, w, UI_RASTER_SET) != PC_OK)
        return false;
    if (tq) {
        ui_path_reset(p);
        if (r - t > 0.0f) {
            ui_path_ellipse(p, lx, ly, r - t, r - t);
            if (ui_raster_fill(p, UI_FILL_NONZERO, true, cov, w, h, w, UI_RASTER_ERASE) != PC_OK)
                return false;
        }
    }
    return make_sprite(ctx, key, cov, w, h, sp);
}

/* ---- rectangles ---------------------------------------------------------- */
void ui_draw_rect(ui_ctx *ctx, ui_rect r, ui_color c)
{
    if (ui_rect_empty(r)) return;
    quad_solid(ctx, (float)r.x, (float)r.y, (float)(r.x + r.w), (float)(r.y + r.h), c);
}

void ui_draw_rect_outline(ui_ctx *ctx, ui_rect r, int32_t t, ui_color c)
{
    if (ui_rect_empty(r)) return;
    if (t < 1) t = 1;
    if (2 * t >= r.w || 2 * t >= r.h) { ui_draw_rect(ctx, r, c); return; }
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y, r.w, t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y + r.h - t, r.w, t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y + t, t, r.h - 2 * t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x + r.w - t, r.y + t, t, r.h - 2 * t), c);
}

static float clamp_radius(ui_rect r, float rad)
{
    float m = floorf((float)ui_mini(r.w, r.h) * 0.5f);
    if (!(rad > 0.0f)) return 0.0f;
    return rad > m ? m : rad;
}

void ui_draw_rrect_ex(ui_ctx *ctx, ui_rect r, ui_corners rad, ui_color c)
{
    float rr[4];
    int32_t R[4], xs[6], ys[6];
    ui_sprite sp[4];
    bool ok[4];
    if (ui_rect_empty(r) || c.a == 0) return;
    rr[0] = clamp_radius(r, rad.tl); rr[1] = clamp_radius(r, rad.tr);
    rr[2] = clamp_radius(r, rad.br); rr[3] = clamp_radius(r, rad.bl);
    for (int i = 0; i < 4; i++) {
        ok[i] = rr[i] > 0.0f && corner_sprite(ctx, rr[i], 0, &sp[i]);
        R[i] = ok[i] ? (int32_t)sp[i].w : 0;
    }
    if (!ok[0] && !ok[1] && !ok[2] && !ok[3]) { ui_draw_rect(ctx, r, c); return; }
    if (ok[0]) ui_draw_sprite_flip(ctx, &sp[0], (float)r.x, (float)r.y, false, false, c);
    if (ok[1])
        ui_draw_sprite_flip(ctx, &sp[1], (float)(r.x + r.w - R[1]), (float)r.y, true, false, c);
    if (ok[2])
        ui_draw_sprite_flip(ctx, &sp[2], (float)(r.x + r.w - R[2]), (float)(r.y + r.h - R[2]), true,
                            true, c);
    if (ok[3])
        ui_draw_sprite_flip(ctx, &sp[3], (float)r.x, (float)(r.y + r.h - R[3]), false, true, c);
    /* fill everything outside the four corner squares with a grid of rects */
    xs[0] = r.x; xs[1] = r.x + R[0]; xs[2] = r.x + R[3];
    xs[3] = r.x + r.w - R[1]; xs[4] = r.x + r.w - R[2]; xs[5] = r.x + r.w;
    ys[0] = r.y; ys[1] = r.y + R[0]; ys[2] = r.y + R[1];
    ys[3] = r.y + r.h - R[3]; ys[4] = r.y + r.h - R[2]; ys[5] = r.y + r.h;
    for (int i = 1; i < 6; i++)               /* sort (tiny insertion sorts) */
        for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) {
            int32_t t = xs[j];
            xs[j] = xs[j - 1];
            xs[j - 1] = t;
        }
    for (int i = 1; i < 6; i++)
        for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) {
            int32_t t = ys[j];
            ys[j] = ys[j - 1];
            ys[j - 1] = t;
        }
    for (int yi = 0; yi < 5; yi++) {
        int32_t y0 = ys[yi], y1 = ys[yi + 1], run0 = -1;
        if (y1 <= y0) continue;
        for (int xi = 0; xi <= 5; xi++) {
            bool inside = false;
            if (xi < 5) {
                int32_t x0 = xs[xi], x1 = xs[xi + 1];
                if (x1 > x0) {
                    inside = true;
                    if (x0 < r.x + R[0] && y0 < r.y + R[0]) inside = false;
                    if (x1 > r.x + r.w - R[1] && y0 < r.y + R[1]) inside = false;
                    if (x1 > r.x + r.w - R[2] && y1 > r.y + r.h - R[2]) inside = false;
                    if (x0 < r.x + R[3] && y1 > r.y + r.h - R[3]) inside = false;
                } else {
                    continue;
                }
            }
            if (inside && run0 < 0) run0 = xs[xi];
            if (!inside && run0 >= 0) {
                quad_solid(ctx, (float)run0, (float)y0, (float)xs[xi], (float)y1, c);
                run0 = -1;
            }
        }
    }
}

void ui_draw_rrect(ui_ctx *ctx, ui_rect r, float radius, ui_color c)
{
    ui_draw_rrect_ex(ctx, r, ui_corners_all(radius), c);
}

void ui_draw_rrect_outline_ex(ui_ctx *ctx, ui_rect r, ui_corners rad, int32_t t, ui_color c)
{
    float rr[4];
    int32_t R[4];
    ui_sprite sp[4];
    if (ui_rect_empty(r) || c.a == 0) return;
    if (t < 1) t = 1;
    if (2 * t >= r.w || 2 * t >= r.h) { ui_draw_rrect_ex(ctx, r, rad, c); return; }
    rr[0] = clamp_radius(r, rad.tl); rr[1] = clamp_radius(r, rad.tr);
    rr[2] = clamp_radius(r, rad.br); rr[3] = clamp_radius(r, rad.bl);
    for (int i = 0; i < 4; i++) {
        if (!corner_sprite(ctx, rr[i], t, &sp[i])) { sp[i].w = 0; R[i] = t; }
        else R[i] = sp[i].w;
    }
    if (sp[0].w) ui_draw_sprite_flip(ctx, &sp[0], (float)r.x, (float)r.y, false, false, c);
    else ui_draw_rect(ctx, ui_rect_make(r.x, r.y, t, t), c);
    if (sp[1].w)
        ui_draw_sprite_flip(ctx, &sp[1], (float)(r.x + r.w - R[1]), (float)r.y, true, false, c);
    else ui_draw_rect(ctx, ui_rect_make(r.x + r.w - t, r.y, t, t), c);
    if (sp[2].w)
        ui_draw_sprite_flip(ctx, &sp[2], (float)(r.x + r.w - R[2]), (float)(r.y + r.h - R[2]), true,
                            true, c);
    else ui_draw_rect(ctx, ui_rect_make(r.x + r.w - t, r.y + r.h - t, t, t), c);
    if (sp[3].w)
        ui_draw_sprite_flip(ctx, &sp[3], (float)r.x, (float)(r.y + r.h - R[3]), false, true, c);
    else ui_draw_rect(ctx, ui_rect_make(r.x, r.y + r.h - t, t, t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x + R[0], r.y, r.w - R[0] - R[1], t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x + R[3], r.y + r.h - t, r.w - R[3] - R[2], t), c);
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y + R[0], t, r.h - R[0] - R[3]), c);
    ui_draw_rect(ctx, ui_rect_make(r.x + r.w - t, r.y + R[1], t, r.h - R[1] - R[2]), c);
}

void ui_draw_rrect_outline(ui_ctx *ctx, ui_rect r, float radius, int32_t t, ui_color c)
{
    ui_draw_rrect_outline_ex(ctx, r, ui_corners_all(radius), t, c);
}

/* ---- shadows ------------------------------------------------------------- */
/* Three box blur passes (close to a Gaussian) over a w x h float image,
 * zero outside. tmp holds max(w, h) floats. */
static void box_blur(float *a, float *tmp, int32_t w, int32_t h, int32_t rad)
{
    float inv = 1.0f / (float)(2 * rad + 1);
    for (int pass = 0; pass < 3; pass++) {
        for (int32_t y = 0; y < h; y++) {           /* horizontal */
            float *row = a + (size_t)y * (size_t)w, acc = 0.0f;
            for (int32_t x = -rad; x <= rad; x++) acc += (x >= 0 && x < w) ? row[x] : 0.0f;
            for (int32_t x = 0; x < w; x++) {
                int32_t xa = x + rad + 1, xr = x - rad;
                tmp[x] = acc * inv;
                if (xa < w) acc += row[xa];
                if (xr >= 0) acc -= row[xr];
            }
            memcpy(row, tmp, (size_t)w * sizeof(float));
        }
        for (int32_t x = 0; x < w; x++) {           /* vertical */
            float acc = 0.0f;
            for (int32_t y = -rad; y <= rad; y++)
                acc += (y >= 0 && y < h) ? a[(size_t)y * (size_t)w + (size_t)x] : 0.0f;
            for (int32_t y = 0; y < h; y++) {
                int32_t ya = y + rad + 1, yr = y - rad;
                tmp[y] = acc * inv;
                if (ya < h) acc += a[(size_t)ya * (size_t)w + (size_t)x];
                if (yr >= 0) acc -= a[(size_t)yr * (size_t)w + (size_t)x];
            }
            for (int32_t y = 0; y < h; y++) a[(size_t)y * (size_t)w + (size_t)x] = tmp[y];
        }
    }
}

/* Blurred rounded rectangle of size sw x sh (radius in px) placed 2B pixels
 * inside a (sw + 4B) x (sh + 4B) sprite. The blur is three box passes of
 * radius rad, whose combined reach (3 rad) stays inside the 2B margin. */
static bool shadow_sprite(ui_ctx *ctx, uint64_t key, float radius, int32_t B, int32_t rad,
                          int32_t sw, int32_t sh, ui_sprite *sp)
{
    int32_t W = sw + 4 * B, H = sh + 4 * B;
    float *a, *tmp;
    uint8_t *cov;
    ui_path *p = &ctx->scratch_path;
    bool ok;
    if (ui_cache_get(ctx, key, sp)) return sp->w > 0;
    if (W > UI_ATLAS_DIM - 1 || H > UI_ATLAS_DIM - 1) return false;
    cov = ui_scratch(ctx, (size_t)W * (size_t)H);
    a = (float *)malloc((size_t)W * (size_t)H * sizeof(float));
    tmp = (float *)malloc((size_t)ui_maxi(W, H) * sizeof(float));
    if (!cov || !a || !tmp) { free(a); free(tmp); return false; }
    ui_path_reset(p);
    ui_path_rrect(p, (float)(2 * B), (float)(2 * B), (float)sw, (float)sh, radius);
    ok = ui_raster_fill(p, UI_FILL_NONZERO, true, cov, W, H, W, UI_RASTER_SET) == PC_OK;
    if (ok) {
        for (int32_t i = 0; i < W * H; i++) a[i] = (float)cov[i] / 255.0f;
        box_blur(a, tmp, W, H, rad);
        for (int32_t i = 0; i < W * H; i++) {
            float v = a[i] * 255.0f + 0.5f;
            cov[i] = (uint8_t)(v >= 255.0f ? 255.0f : v);
        }
        ok = make_sprite(ctx, key, cov, W, H, sp);
    }
    free(a);
    free(tmp);
    return ok;
}

/* One axis of a shadow nine-slice: either the sprite covers the extent 1:1
 * (exact) or it splits into two corner parts of c texels and one middle
 * texel stretched across the rest. */
typedef struct shadow_axis {
    int32_t n, src[3], srcw[3], dst[3], dstw[3];
} shadow_axis;

static void shadow_axis_make(shadow_axis *a, int32_t lo, int32_t len, int32_t S, int32_t c,
                             bool exact)
{
    if (exact) {
        a->n = 1;
        a->src[0] = 0; a->srcw[0] = S; a->dst[0] = lo; a->dstw[0] = len;
        return;
    }
    a->n = 3;
    a->src[0] = 0; a->srcw[0] = c; a->dst[0] = lo; a->dstw[0] = c;
    a->src[1] = c; a->srcw[1] = 1; a->dst[1] = lo + c; a->dstw[1] = len - 2 * c;
    a->src[2] = S - c; a->srcw[2] = c; a->dst[2] = lo + len - c; a->dstw[2] = c;
}

void ui_draw_shadow(ui_ctx *ctx, ui_rect r, float radius, float blur, ui_color c)
{
    ui_sprite sp;
    int32_t B = (int32_t)ceilf(blur * 0.5f), rad, R, C, N, ext_w, ext_h, sw, sh;
    bool exact_x, exact_y;
    uint32_t rq;
    uint64_t key;
    shadow_axis ax, ay;
    if (ui_rect_empty(r) || c.a == 0 || B < 1) return;
    if (B > 64) B = 64;
    rad = ui_maxi(1, (B + 1) / 2);
    radius = clamp_radius(r, radius);
    R = (int32_t)ceilf(radius);
    rq = (uint32_t)(radius * 4.0f + 0.5f) & 0x3FFFu;
    /* Nine-slice sprite: the corner part C reaches from 2B outside the shape
     * to where the blurred profile is flat inside it, so the stretched
     * middle texels match an infinitely long edge and the center is solid.
     * An axis shorter than the nine-slice uses a sprite of its exact size. */
    C = 2 * B + R + 3 * rad;
    N = 2 * C + 1;
    ext_w = r.w + 4 * B;
    ext_h = r.h + 4 * B;
    exact_x = ext_w < N;
    exact_y = ext_h < N;
    sw = exact_x ? r.w : N - 4 * B;
    sh = exact_y ? r.h : N - 4 * B;
    if (sw > 0x1FFFF || sh > 0x1FFFF) return;
    key = ((uint64_t)UI_KEY_SHADOW << 56) | ((uint64_t)rq << 41) | ((uint64_t)B << 34) |
          ((uint64_t)sw << 17) | (uint64_t)sh;
    if (!shadow_sprite(ctx, key, radius, B, rad, sw, sh, &sp)) return;
    shadow_axis_make(&ax, r.x - 2 * B, ext_w, sw + 4 * B, C, exact_x);
    shadow_axis_make(&ay, r.y - 2 * B, ext_h, sh + 4 * B, C, exact_y);
    for (int32_t j = 0; j < ay.n; j++)
        for (int32_t i = 0; i < ax.n; i++)
            sprite_part(ctx, &sp, ax.src[i], ay.src[j], ax.srcw[i], ay.srcw[j], (float)ax.dst[i],
                        (float)ay.dst[j], (float)(ax.dst[i] + ax.dstw[i]),
                        (float)(ay.dst[j] + ay.dstw[j]), c);
}

/* Two-layer drop shadow for floating surfaces: a tight contact shadow plus
 * a soft key shadow that grows with the elevation level (1 panels and
 * tooltips, 2 popups, 3 dialogs). */
void ui_draw_elevation(ui_ctx *ctx, ui_rect r, float radius, int level)
{
    ui_color sh = ctx->theme.pal.shadow, key = sh;
    float lv = (float)(level < 1 ? 1 : (level > 3 ? 3 : level));
    float a = (float)sh.a * (0.75f + 0.25f * lv);
    key.a = (uint8_t)(a > 255.0f ? 255.0f : a);
    ui_draw_shadow(ctx, ui_rect_offset(r, 0, ui_px(ctx, 1.0f)), radius, (float)ui_px(ctx, 3.0f),
                   ui_color_fade(sh, 0.7f));
    ui_draw_shadow(ctx, ui_rect_offset(r, 0, ui_px(ctx, 2.0f * lv)), radius,
                   ctx->px.shadow * (0.5f + 0.5f * lv), key);
}

/* ---- gradients and checkerboard ------------------------------------------ */
void ui_draw_gradient(ui_ctx *ctx, ui_rect r, ui_color tl, ui_color tr, ui_color br, ui_color bl)
{
    int *ix;
    int32_t b;
    SDL_Vertex *v;
    float x0 = (float)r.x, y0 = (float)r.y, x1 = (float)(r.x + r.w), y1 = (float)(r.y + r.h);
    if (ui_rect_empty(r)) return;
    v = prim(ctx, solid_tex(ctx), -1, 4, 6, &ix, &b);
    if (!v) return;
    vtx(&v[0], x0, y0, fcol(tl), 2.0f * TEXEL, 2.0f * TEXEL);
    vtx(&v[1], x1, y0, fcol(tr), 2.0f * TEXEL, 2.0f * TEXEL);
    vtx(&v[2], x1, y1, fcol(br), 2.0f * TEXEL, 2.0f * TEXEL);
    vtx(&v[3], x0, y1, fcol(bl), 2.0f * TEXEL, 2.0f * TEXEL);
    ix[0] = b; ix[1] = b + 1; ix[2] = b + 2;
    ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
}

static SDL_Texture *checker_tex(ui_ctx *ctx, ui_color a, ui_color b)
{
    uint8_t px[16];
    int32_t slot;
    for (int32_t i = 0; i < 4; i++)
        if (ctx->checker_tex[i] && ui_color_eq(ctx->checker_col[i][0], a) &&
            ui_color_eq(ctx->checker_col[i][1], b))
            return ctx->checker_tex[i];
    slot = ctx->checker_next;
    ctx->checker_next = (ctx->checker_next + 1) % 4;
    if (ctx->checker_tex[slot]) SDL_DestroyTexture(ctx->checker_tex[slot]);
    ctx->checker_tex[slot] = SDL_CreateTexture(ctx->r, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STATIC, 2, 2);
    if (!ctx->checker_tex[slot]) return NULL;
    for (int i = 0; i < 4; i++) {
        ui_color c = (i == 0 || i == 3) ? a : b;
        px[4 * i] = c.r; px[4 * i + 1] = c.g; px[4 * i + 2] = c.b; px[4 * i + 3] = c.a;
    }
    SDL_UpdateTexture(ctx->checker_tex[slot], NULL, px, 8);
    SDL_SetTextureScaleMode(ctx->checker_tex[slot], SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(ctx->checker_tex[slot], SDL_BLENDMODE_BLEND);
    ctx->checker_col[slot][0] = a;
    ctx->checker_col[slot][1] = b;
    return ctx->checker_tex[slot];
}

void ui_draw_checker(ui_ctx *ctx, ui_rect r, int32_t cell, ui_color a, ui_color b)
{
    int32_t nx, ny;
    if (ui_rect_empty(r)) return;
    if (cell < 1) cell = 1;
    nx = (r.w + cell - 1) / cell;
    ny = (r.h + cell - 1) / cell;
    if ((int64_t)nx * (int64_t)ny > 8192) {
        /* very large areas: one quad with wrapping texture coordinates */
        SDL_Texture *t = checker_tex(ctx, a, b);
        if (!t) { ui_draw_rect(ctx, r, a); return; }
        quad_tex(ctx, t, (int32_t)SDL_SCALEMODE_NEAREST, (float)r.x, (float)r.y,
                 (float)(r.x + r.w), (float)(r.y + r.h), 0.0f, 0.0f,
                 (float)r.w / (float)(2 * cell), (float)r.h / (float)(2 * cell),
                 ui_rgba(255, 255, 255, 255));
        return;
    }
    /* Solid cells cut to r: exact on every backend (no texture wrapping or
     * sub-texel source rectangles) and batched with the other shapes. */
    for (int32_t j = 0; j < ny; j++) {
        int32_t y0 = r.y + j * cell, y1 = ui_mini(y0 + cell, r.y + r.h);
        for (int32_t i = 0; i < nx; i++) {
            int32_t x0 = r.x + i * cell, x1 = ui_mini(x0 + cell, r.x + r.w);
            quad_solid(ctx, (float)x0, (float)y0, (float)x1, (float)y1, ((i + j) & 1) ? b : a);
        }
    }
}

/* ---- antialiased strokes and polygons ------------------------------------ */
void ui_draw_polyline(ui_ctx *ctx, const ui_vec2 *p, int n, bool closed, float width, ui_color c)
{
    int32_t segs, nv, ni, b;
    int *ix;
    SDL_Vertex *v;
    SDL_FColor fc, fz;
    float inner, outer;
    if (n < 2 || c.a == 0 || !(width > 0.0f)) return;
    if (width < 1.0f) { c = ui_color_fade(c, width); width = 1.0f; }
    inner = width * 0.5f - 0.5f;
    outer = width * 0.5f + 0.5f;
    segs = closed ? n : n - 1;
    nv = n * 4;
    ni = segs * 18;
    v = prim(ctx, solid_tex(ctx), -1, nv, ni, &ix, &b);
    if (!v) return;
    fc = fcol(c);
    fz = fc;
    fz.a = 0.0f;
    for (int i = 0; i < n; i++) {
        int ip = i > 0 ? i - 1 : (closed ? n - 1 : 0),
            in = i < n - 1 ? i + 1 : (closed ? 0 : n - 1);
        float nx0 = 0, ny0 = 0, nx1 = 0, ny1 = 0, dx, dy, l, mx, my, ml2;
        if (ip != i) {
            dx = p[i].x - p[ip].x; dy = p[i].y - p[ip].y; l = sqrtf(dx * dx + dy * dy);
            if (l > 1e-6f) { nx0 = -dy / l; ny0 = dx / l; }
        }
        if (in != i) {
            dx = p[in].x - p[i].x; dy = p[in].y - p[i].y; l = sqrtf(dx * dx + dy * dy);
            if (l > 1e-6f) { nx1 = -dy / l; ny1 = dx / l; }
        }
        if (ip == i) { nx0 = nx1; ny0 = ny1; }
        if (in == i) { nx1 = nx0; ny1 = ny0; }
        mx = (nx0 + nx1) * 0.5f; my = (ny0 + ny1) * 0.5f;
        ml2 = mx * mx + my * my;
        if (ml2 > 1e-6f) {
            float sc = 1.0f / ml2;
            if (sc > 16.0f) sc = 16.0f;
            mx *= sc; my *= sc;
        }
        vtx(&v[4 * i + 0], p[i].x + mx * outer, p[i].y + my * outer, fz, 2.0f * TEXEL,
            2.0f * TEXEL);
        vtx(&v[4 * i + 1], p[i].x + mx * inner, p[i].y + my * inner, fc, 2.0f * TEXEL,
            2.0f * TEXEL);
        vtx(&v[4 * i + 2], p[i].x - mx * inner, p[i].y - my * inner, fc, 2.0f * TEXEL,
            2.0f * TEXEL);
        vtx(&v[4 * i + 3], p[i].x - mx * outer, p[i].y - my * outer, fz, 2.0f * TEXEL,
            2.0f * TEXEL);
    }
    for (int s = 0; s < segs; s++) {
        int a = b + 4 * s, bb = b + 4 * ((s + 1) % n);
        int *q = ix + 18 * s;
        for (int k = 0; k < 3; k++) {
            q[6 * k + 0] = a + k; q[6 * k + 1] = a + k + 1; q[6 * k + 2] = bb + k + 1;
            q[6 * k + 3] = a + k; q[6 * k + 4] = bb + k + 1; q[6 * k + 5] = bb + k;
        }
    }
}

void ui_draw_line(ui_ctx *ctx, ui_vec2 a, ui_vec2 b, float width, ui_color c)
{
    ui_vec2 p[2];
    /* axis-aligned lines on whole pixels become crisp rectangles */
    float iw = roundf(width);
    if (fabsf(width - iw) < 1e-3f && iw >= 1.0f) {
        if (a.y == b.y && fabsf(a.y * 2.0f - floorf(a.y * 2.0f)) < 1e-3f) {
            float y0 = a.y - iw * 0.5f;
            if (fabsf(y0 - roundf(y0)) < 1e-3f) {
                float x0 = ui_minf(a.x, b.x), x1 = ui_maxf(a.x, b.x);
                quad_solid(ctx, x0, roundf(y0), x1, roundf(y0) + iw, c);
                return;
            }
        }
        if (a.x == b.x) {
            float x0 = a.x - iw * 0.5f;
            if (fabsf(x0 - roundf(x0)) < 1e-3f) {
                float y0 = ui_minf(a.y, b.y), y1 = ui_maxf(a.y, b.y);
                quad_solid(ctx, roundf(x0), y0, roundf(x0) + iw, y1, c);
                return;
            }
        }
    }
    p[0] = a;
    p[1] = b;
    ui_draw_polyline(ctx, p, 2, false, width, c);
}

void ui_draw_convex(ui_ctx *ctx, const ui_vec2 *p, int n, ui_color c)
{
    int32_t nv, ni, b;
    int *ix;
    SDL_Vertex *v;
    SDL_FColor fc, fz;
    double area = 0.0;
    float sgn;
    if (n < 3 || c.a == 0) return;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (double)p[i].x * p[j].y - (double)p[j].x * p[i].y;
    }
    if (area == 0.0) return;
    sgn = area > 0.0 ? 1.0f : -1.0f;     /* outward normal = sgn * (dy, -dx) */
    nv = 2 * n;
    ni = 3 * (n - 2) + 6 * n;
    v = prim(ctx, solid_tex(ctx), -1, nv, ni, &ix, &b);
    if (!v) return;
    fc = fcol(c);
    fz = fc;
    fz.a = 0.0f;
    for (int i = 0; i < n; i++) {
        int ip = (i + n - 1) % n, in = (i + 1) % n;
        float dx0 = p[i].x - p[ip].x, dy0 = p[i].y - p[ip].y;
        float dx1 = p[in].x - p[i].x, dy1 = p[in].y - p[i].y;
        float l0 = sqrtf(dx0 * dx0 + dy0 * dy0), l1 = sqrtf(dx1 * dx1 + dy1 * dy1);
        float nx = 0, ny = 0, ml2;
        if (l0 > 1e-6f) { nx += sgn * dy0 / l0; ny -= sgn * dx0 / l0; }
        if (l1 > 1e-6f) { nx += sgn * dy1 / l1; ny -= sgn * dx1 / l1; }
        nx *= 0.5f; ny *= 0.5f;
        ml2 = nx * nx + ny * ny;
        if (ml2 > 1e-6f) {
            float sc = 1.0f / ml2;
            if (sc > 16.0f) sc = 16.0f;
            nx *= sc; ny *= sc;
        }
        vtx(&v[2 * i], p[i].x - nx * 0.5f, p[i].y - ny * 0.5f, fc, 2.0f * TEXEL, 2.0f * TEXEL);
        vtx(&v[2 * i + 1], p[i].x + nx * 0.5f, p[i].y + ny * 0.5f, fz, 2.0f * TEXEL, 2.0f * TEXEL);
    }
    for (int i = 2; i < n; i++) {
        *ix++ = b; *ix++ = b + 2 * (i - 1); *ix++ = b + 2 * i;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        *ix++ = b + 2 * i; *ix++ = b + 2 * j; *ix++ = b + 2 * j + 1;
        *ix++ = b + 2 * i; *ix++ = b + 2 * j + 1; *ix++ = b + 2 * i + 1;
    }
}

void ui_draw_triangle(ui_ctx *ctx, ui_vec2 a, ui_vec2 b, ui_vec2 c, ui_color col)
{
    ui_vec2 p[3];
    p[0] = a; p[1] = b; p[2] = c;
    ui_draw_convex(ctx, p, 3, col);
}

static void circle_geom(ui_ctx *ctx, ui_vec2 c, float r, float width, bool fill, ui_color col)
{
    ui_vec2 pts[256];
    int n = (int)ceilf(2.0f * UI_PI * r / 3.0f);
    if (n < 12) n = 12;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        float a = 2.0f * UI_PI * (float)i / (float)n;
        pts[i].x = c.x + r * cosf(a);
        pts[i].y = c.y + r * sinf(a);
    }
    if (fill) ui_draw_convex(ctx, pts, n, col);
    else ui_draw_polyline(ctx, pts, n, true, width, col);
}

void ui_draw_circle(ui_ctx *ctx, ui_vec2 center, float radius, ui_color c)
{
    ui_sprite sp;
    int32_t x, y;
    if (!(radius > 0.0f) || c.a == 0) return;
    if (radius <= 96.0f && disc_sprite(ctx, center.x, center.y, radius, 0.0f, &sp, &x, &y)) {
        ui_draw_sprite(ctx, &sp, (float)x, (float)y, c);
        return;
    }
    circle_geom(ctx, center, radius, 0.0f, true, c);
}

void ui_draw_circle_outline(ui_ctx *ctx, ui_vec2 center, float radius, float width, ui_color c)
{
    ui_sprite sp;
    int32_t x, y;
    if (!(radius > 0.0f) || !(width > 0.0f) || c.a == 0) return;
    /* the ring sprite covers [radius - width/2, radius + width/2] */
    if (radius + width * 0.5f <= 96.0f &&
        disc_sprite(ctx, center.x, center.y, radius + width * 0.5f, width, &sp, &x, &y)) {
        ui_draw_sprite(ctx, &sp, (float)x, (float)y, c);
        return;
    }
    circle_geom(ctx, center, radius, width, false, c);
}

void ui_draw_arc(ui_ctx *ctx, ui_vec2 c, float r, float a0, float a1, float width, ui_color col)
{
    ui_vec2 pts[257];
    float sweep = a1 - a0;
    int n = (int)ceilf(fabsf(sweep) * r / 3.0f);
    if (!(r > 0.0f)) return;
    if (n < 4) n = 4;
    if (n > 256) n = 256;
    for (int i = 0; i <= n; i++) {
        float a = a0 + sweep * (float)i / (float)n;
        pts[i].x = c.x + r * cosf(a);
        pts[i].y = c.y + r * sinf(a);
    }
    ui_draw_polyline(ctx, pts, n + 1, false, width, col);
}

/* ---- images -------------------------------------------------------------- */
void ui_draw_image(ui_ctx *ctx, SDL_Texture *tex, const ui_rect *src, ui_rect dst, ui_filter f,
                   ui_color tint)
{
    float tw = 1.0f, th = 1.0f, u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (!tex || ui_rect_empty(dst)) return;
    if (src) {
        if (!SDL_GetTextureSize(tex, &tw, &th) || tw <= 0.0f || th <= 0.0f) return;
        u0 = (float)src->x / tw; v0 = (float)src->y / th;
        u1 = (float)(src->x + src->w) / tw; v1 = (float)(src->y + src->h) / th;
    }
    quad_tex(ctx, tex, f == UI_FILTER_LINEAR ? (int32_t)SDL_SCALEMODE_LINEAR
                                              : (int32_t)SDL_SCALEMODE_NEAREST,
             (float)dst.x, (float)dst.y, (float)(dst.x + dst.w), (float)(dst.y + dst.h), u0, v0, u1,
             v1, tint);
}

void ui_draw_callback(ui_ctx *ctx, ui_draw_fn fn, void *ud)
{
    ui_dl *dl = cur_dl(ctx);
    ui_cmd *c;
    if (!fn || !grow((void **)&dl->cmd, &dl->cc, dl->nc + 1, sizeof(ui_cmd))) return;
    c = &dl->cmd[dl->nc++];
    memset(c, 0, sizeof *c);
    c->fn = fn;
    c->ud = ud;
    c->clip = ctx->clip[ctx->clip_depth - 1];
    c->filter = -1;
    c->v0 = dl->nv;
    c->i0 = dl->ni;
}

/* ---- glyphs and text ----------------------------------------------------- */
/* Coverage gamma for glyphs: slightly heavier stems, closer to the weight
 * of hinted system text at small sizes. Filled once per context. */
void ui_gamma_init(uint8_t table[256])
{
    for (int i = 0; i < 256; i++) {
        float v = powf((float)i / 255.0f, 0.82f) * 255.0f + 0.5f;
        table[i] = (uint8_t)(v >= 255.0f ? 255.0f : v);
    }
}

bool ui_glyph_sprite(ui_ctx *ctx, const ui_font *face, float size, uint32_t gid, int sub,
                     ui_sprite *sp)
{
    uint32_t sq = (uint32_t)(size * 4.0f + 0.5f) & 0x3FFFu;
    uint64_t key = ((uint64_t)UI_KEY_GLYPH << 56) | ((uint64_t)(face->serial & 0xFFFFFu) << 36) |
                   ((uint64_t)sq << 22) | ((uint64_t)(sub & 3) << 20) | (uint64_t)(gid & 0xFFFFFu);
    ui_path *p = &ctx->scratch_path;
    ui_ymap ym;
    float x0, y0, x1, y1, sc = size / face->upem;
    int32_t bx, by, w, h;
    uint8_t *cov;
    if (ui_cache_get(ctx, key, sp)) return sp->w > 0;
    memset(sp, 0, sizeof *sp);
    ui_path_reset(p);
    ui_ymap_init(&ym, face, sc, size <= 40.0f);
    if (!ui_font_glyph_path(face, gid, sc, &ym, (float)sub * 0.25f, 0.0f, 0.0f, p) || p->n == 0) {
        ui_cache_put(ctx, key, sp);     /* empty glyph */
        return false;
    }
    ui_path_bounds(p, &x0, &y0, &x1, &y1);
    bx = (int32_t)floorf(x0);
    by = (int32_t)floorf(y0);
    w = (int32_t)ceilf(x1) - bx;
    h = (int32_t)ceilf(y1) - by;
    if (w <= 0 || h <= 0) { ui_cache_put(ctx, key, sp); return false; }
    if (w > 512 || h > 512) return false;
    for (int32_t i = 0; i < p->n; i++) {
        p->xy[2 * i] -= (float)bx;
        p->xy[2 * i + 1] -= (float)by;
    }
    cov = ui_scratch(ctx, (size_t)w * (size_t)h);
    if (!cov || ui_raster_fill(p, UI_FILL_NONZERO, true, cov, w, h, w, UI_RASTER_SET) != PC_OK)
        return false;
    for (int32_t i = 0; i < w * h; i++) cov[i] = ctx->gamma[cov[i]];
    if (!make_sprite(ctx, key, cov, w, h, sp)) return false;
    sp->ox = (int16_t)bx;
    sp->oy = (int16_t)by;
    ui_cache_put(ctx, key, sp);
    return true;
}

static void missing_box(ui_ctx *ctx, float size, int32_t x, int32_t base, ui_color c)
{
    int32_t w = (int32_t)(size * 0.5f + 0.5f), h = (int32_t)(size * 0.7f + 0.5f);
    ui_draw_rect_outline(ctx, ui_rect_make(x + (int32_t)(size * 0.05f + 0.5f), base - h, w, h), 1,
                         c);
}

float ui_draw_text(ui_ctx *ctx, ui_font *font, float size, float x, float baseline, ui_color c,
                   const char *s, size_t len)
{
    const ui_font *prev_face = NULL;
    uint32_t prev = 0;
    size_t i = 0;
    int32_t base = (int32_t)floorf(baseline + 0.5f);
    ui_rect clip = ui_current_clip(ctx);
    if (!font || !s || c.a == 0) return x;
    if (!(size > 0.0f)) return x;
    if (size > 512.0f) size = 512.0f;
    while (i < len) {
        const ui_font *face;
        uint32_t cp = ui_utf8_decode(s, len, &i), gid;
        float sc, adv;
        if (cp < 0x20u || cp == 0x7Fu) continue;
        gid = ui_font_resolve_cached(font, cp, &face);
        sc = size / face->upem;
        if (prev_face == face && prev && gid)
            x += (float)ui_kern_cached((ui_font *)face, prev, gid) * sc;
        adv = gid ? (float)ui_font_advance(face, gid) * sc : size * 0.6f;
        if (x > (float)(clip.x + clip.w)) break;
        if (x + adv >= (float)clip.x) {
            int32_t q = (int32_t)floorf(x * 4.0f + 0.5f), px = q >> 2, sub = q & 3;
            if (!gid) {
                missing_box(ctx, size, px, base, c);
            } else {
                ui_sprite sp;
                if (ui_glyph_sprite(ctx, face, size, gid, sub, &sp))
                    ui_draw_sprite(ctx, &sp, (float)(px + sp.ox), (float)(base + sp.oy), c);
            }
        }
        x += adv;
        prev_face = face;
        prev = gid;
    }
    return x;
}

int32_t ui_text_baseline(const ui_ctx *ctx, ui_font *f, float size, ui_rect r)
{
    ui_font_metrics m;
    (void)ctx;
    ui_font_get_metrics(f, size, &m);
    return r.y + (int32_t)floorf(((float)r.h + m.cap_height) * 0.5f + 0.5f);
}

void ui_draw_text_box(ui_ctx *ctx, ui_font *font, float size, ui_rect r, int align, uint32_t flags,
                      ui_color c, const char *s, size_t len)
{
    float w, x;
    int32_t base;
    bool clip = false;
    if (!font || !s || ui_rect_empty(r)) return;
    w = ui_text_width(font, size, s, len);
    if (flags & UI_TEXT_TOP) {
        ui_font_metrics m;
        ui_font_get_metrics(font, size, &m);
        base = r.y + (int32_t)floorf(m.ascent + 0.5f);
    } else {
        base = ui_text_baseline(ctx, font, size, r);
    }
    if (w > (float)r.w + 0.5f) {
        if (flags & UI_TEXT_ELLIPSIS) {
            static const char ell[] = "\xE2\x80\xA6";
            float ew = ui_text_width(font, size, ell, 3);
            size_t fit = ui_text_fit(font, size, s, len, (float)r.w - ew);
            float x2 = ui_draw_text(ctx, font, size, (float)r.x, (float)base, c, s, fit);
            if ((float)r.w >= ew) ui_draw_text(ctx, font, size, x2, (float)base, c, ell, 3);
            return;
        }
        clip = true;
        align = UI_ALIGN_LEFT;
    }
    if (align == UI_ALIGN_CENTER) x = (float)r.x + floorf(((float)r.w - w) * 0.5f + 0.5f);
    else if (align == UI_ALIGN_RIGHT) x = (float)(r.x + r.w) - w;
    else x = (float)r.x;
    if (clip) ui_push_clip(ctx, r);
    ui_draw_text(ctx, font, size, x, (float)base, c, s, len);
    if (clip) ui_pop_clip(ctx);
}

/* ---- icons --------------------------------------------------------------- */
void ui_draw_icon_ex(ui_ctx *ctx, ui_icon icon, ui_rect r, int32_t size, ui_color line,
                     ui_color accent, ui_color soft)
{
    ui_sprite sp[UI_ICON_LAYERS];
    int32_t x, y;
    if (icon <= UI_ICON_NONE || icon >= UI_ICON_COUNT || size < UI_ICON_MIN_PX) return;
    if (size > UI_ICON_MAX_PX) size = UI_ICON_MAX_PX;
    if (!ui_icon_sprites(ctx, icon, size, sp)) return;
    x = r.x + (r.w - size) / 2;
    y = r.y + (r.h - size) / 2;
    if (sp[UI_ICON_LAYER_SOFT].w)
        ui_draw_sprite(ctx, &sp[UI_ICON_LAYER_SOFT], (float)(x + sp[0].ox), (float)(y + sp[0].oy),
                       soft);
    if (sp[UI_ICON_LAYER_ACCENT].w)
        ui_draw_sprite(ctx, &sp[UI_ICON_LAYER_ACCENT], (float)(x + sp[1].ox), (float)(y + sp[1].oy),
                       accent);
    if (sp[UI_ICON_LAYER_LINE].w)
        ui_draw_sprite(ctx, &sp[UI_ICON_LAYER_LINE], (float)(x + sp[2].ox), (float)(y + sp[2].oy),
                       line);
}

void ui_draw_icon(ui_ctx *ctx, ui_icon icon, ui_rect r, int32_t size, ui_color line,
                  ui_color accent)
{
    ui_draw_icon_ex(ctx, icon, r, size, line, accent, ui_color_fade(accent, 0.30f));
}

/* ---- replay -------------------------------------------------------------- */
void ui_replay_root(ui_ctx *ctx, ui_root *root)
{
    ui_dl *dl = &root->dl;
    ui_rect last = ui_rect_make(-1, -1, -1, -1);
    for (int32_t i = 0; i < dl->nc; i++) {
        ui_cmd *c = &dl->cmd[i];
        SDL_Rect cr;
        if (!rect_eq(c->clip, last) || c->fn) {
            cr.x = c->clip.x; cr.y = c->clip.y; cr.w = c->clip.w; cr.h = c->clip.h;
            SDL_SetRenderClipRect(ctx->r, &cr);
            last = c->clip;
        }
        if (c->fn) {
            c->fn(ctx->r, c->clip, c->ud);
            SDL_SetRenderDrawBlendMode(ctx->r, SDL_BLENDMODE_BLEND);
            last = ui_rect_make(-1, -1, -1, -1);
            continue;
        }
        if (c->ni == 0) continue;
        if (c->filter >= 0) SDL_SetTextureScaleMode(c->tex, (SDL_ScaleMode)c->filter);
        SDL_RenderGeometry(ctx->r, c->tex, dl->v + c->v0, c->nv, dl->ix + c->i0, c->ni);
    }
}
