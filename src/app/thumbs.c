/* thumbs.c - image list and Layers window thumbnails (lane P, W-IMG-THUMB,
 * W-LAY-THUMBS).
 *
 * Quality: an area-weighted box filter in linear light with premultiplied
 * alpha, so thumbnails keep correct alpha and gamma (R 5.0.4), thin strokes
 * still show and transparent pixels never darken their neighbors.
 *
 * Two stages keep it cheap. Stage 1 reduces the document by a power of two
 * g into a "reduced image" of at most about twice the thumbnail size. The
 * reduced image is cached per cell of c x c tiles (c = max(1, g / 64))
 * together with a key of the cell's content (tile serials, transaction
 * versions, composite signatures from pc_comp_tile_sig), so an edit only
 * recomputes the cells it touched. Blocks of up to 8 x 8 pixels read every
 * pixel; larger blocks average 4 x 4 stratified samples. Stage 2 box-filters
 * the reduced image to the thumbnail size and uploads a texture.
 *
 * Scheduling: lazy (a source is walked only when its content stamp
 * changed), throttled (THROTTLE_MS between two uploads of one thumbnail
 * while edits keep coming) and time sliced (BUDGET_NS of stage 1 work per
 * frame for all documents), so huge images never stall a frame.
 *
 * Live edits: the main thread owns the document and its open transaction,
 * so the transaction's private tiles are read directly (pc_txn_lookup,
 * app_doc_comp_opts) and the thumbnails follow a stroke while it is drawn.
 *
 * Thread rules: main thread only (textures, documents). The cache records
 * live in the app extension "pnl.thumbs" (owned by the app) and are swept
 * when their document closed; textures stay in app_doc (thumb, lthumbs) as
 * before, so app_thumbs_free still drops them on device resets. */
#include "app_internal.h"
#include "panels/pnl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DOC_THUMB_MAX   112      /* px, longest side (image list, its popup) */
#define LAYER_THUMB_MAX 72       /* px, Layers window rows (2x DPI included) */
#define THROTTLE_MS     250u
#define BUDGET_NS       ((uint64_t)3000000u)
#define FULL_MAX_G      8u       /* blocks up to 8 x 8 read every pixel */
#define SAMPLES         4u       /* larger blocks: 4 x 4 stratified samples */

/* ---- cache records ---------------------------------------------------------------- */
typedef struct th_src {
    uint32_t  layer_id;          /* 0 = composite of the visible layers */
    int32_t   max_px;
    uint32_t  dw, dh;            /* document size the geometry was built for */
    uint32_t  g, c, r;           /* block px, cell side in tiles, reduced px per cell side */
    uint32_t  cx, cy;            /* cells */
    uint32_t  rw, rh;            /* reduced image */
    uint16_t *red;               /* rw * rh * 4: premultiplied linear B, G, R, then alpha */
    uint64_t *key;               /* cx * cy cell keys, 0 = unknown */
    uint32_t  cursor;            /* next cell of the current walk */
    bool      walking;
    uint64_t  walk_stamp;
    bool      done_valid;
    uint64_t  done_stamp;        /* stamp of the last completed walk */
    bool      need_upload;
    uint64_t  upload_ms;
    bool      seen;              /* still present (layers) */
} th_src;

typedef struct th_doc {
    uint32_t doc_id;
    th_src   comp;
    th_src  *lay;                /* owned array */
    uint32_t nlay, caplay;
} th_doc;

typedef struct th_state {
    th_doc  *docs;               /* owned array */
    uint32_t n, cap;
    uint16_t lin[256];           /* sRGB byte -> linear 0..65535 */
    uint8_t  srgb[4097];         /* linear / 16 -> sRGB byte */
    uint64_t frame;              /* frame number of the current budget (+1) */
    uint64_t spent_ns;
    bool     unlimited;          /* pnl_thumbs_sync, pnl_thumb_render */
    uint64_t recomputed;
} th_state;

static void src_free(th_src *s)
{
    free(s->red);
    free(s->key);
    s->red = NULL;
    s->key = NULL;
    s->dw = s->dh = 0;
}

static void state_free(void *p)
{
    th_state *t = (th_state *)p;
    if (!t) return;
    for (uint32_t i = 0; i < t->n; i++) {
        src_free(&t->docs[i].comp);
        for (uint32_t k = 0; k < t->docs[i].nlay; k++) src_free(&t->docs[i].lay[k]);
        free(t->docs[i].lay);
    }
    free(t->docs);
    free(t);
}

static th_state *state(app *a)
{
    th_state *t = (th_state *)app_ext_get(a, "pnl.thumbs");
    if (t) return t;
    t = (th_state *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    for (int i = 0; i < 256; i++) {
        double c = (double)i / 255.0;
        double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        t->lin[i] = (uint16_t)(l * 65535.0 + 0.5);
    }
    for (int i = 0; i <= 4096; i++) {
        double l = (double)i / 4096.0, c;
        c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        c = c * 255.0 + 0.5;
        t->srgb[i] = (uint8_t)(c < 0.0 ? 0.0 : c > 255.0 ? 255.0 : c);
    }
    if (!app_ext_set(a, "pnl.thumbs", t, state_free)) {
        free(t);
        return NULL;
    }
    return t;
}

static th_doc *doc_rec(th_state *t, uint32_t id)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (t->docs[i].doc_id == id) return &t->docs[i];
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2u : 4u;
        th_doc *nd = (th_doc *)realloc(t->docs, (size_t)nc * sizeof *nd);
        if (!nd) return NULL;
        t->docs = nd;
        t->cap = nc;
    }
    memset(&t->docs[t->n], 0, sizeof t->docs[0]);
    t->docs[t->n].doc_id = id;
    t->docs[t->n].comp.max_px = DOC_THUMB_MAX;
    return &t->docs[t->n++];
}

static th_src *layer_rec(th_doc *r, uint32_t layer_id)
{
    for (uint32_t i = 0; i < r->nlay; i++)
        if (r->lay[i].layer_id == layer_id) return &r->lay[i];
    if (r->nlay == r->caplay) {
        uint32_t nc = r->caplay ? r->caplay * 2u : 8u;
        th_src *ns = (th_src *)realloc(r->lay, (size_t)nc * sizeof *ns);
        if (!ns) return NULL;
        r->lay = ns;
        r->caplay = nc;
    }
    memset(&r->lay[r->nlay], 0, sizeof r->lay[0]);
    r->lay[r->nlay].layer_id = layer_id;
    r->lay[r->nlay].max_px = LAYER_THUMB_MAX;
    return &r->lay[r->nlay++];
}

/* Free records of documents that are no longer open. */
static void sweep(app *a, th_state *t)
{
    for (uint32_t i = 0; i < t->n;) {
        bool open = false;
        for (int32_t k = 0; k < a->ndocs; k++)
            if (a->docs[k]->id == t->docs[i].doc_id) open = true;
        if (open) {
            i++;
            continue;
        }
        src_free(&t->docs[i].comp);
        for (uint32_t k = 0; k < t->docs[i].nlay; k++) src_free(&t->docs[i].lay[k]);
        free(t->docs[i].lay);
        t->docs[i] = t->docs[--t->n];
    }
}

/* ---- geometry --------------------------------------------------------------------- */
/* Fit w x h into max x max keeping the aspect ratio, at least 1 x 1. */
static void fit(uint32_t w, uint32_t h, int32_t max, int32_t *tw, int32_t *th)
{
    if (w >= h) {
        *tw = (int32_t)(w < (uint32_t)max ? w : (uint32_t)max);
        *th = (int32_t)(((uint64_t)h * (uint64_t)*tw + w / 2u) / w);
    } else {
        *th = (int32_t)(h < (uint32_t)max ? h : (uint32_t)max);
        *tw = (int32_t)(((uint64_t)w * (uint64_t)*th + h / 2u) / h);
    }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* (Re)build the cache geometry for the document size. false on OOM. */
static bool src_geometry(th_src *s, const pc_doc *d)
{
    uint32_t big = d->w > d->h ? d->w : d->h, target = (uint32_t)s->max_px * 2u, g = 1;
    size_t nred, nkey;
    if (s->red && s->dw == d->w && s->dh == d->h) return true;
    src_free(s);
    if (d->w == 0u || d->h == 0u) return false;
    while (big / g > target && g < (1u << 20)) g *= 2u;
    s->g = g;
    s->c = g > PC_TILE_DIM ? g / PC_TILE_DIM : 1u;
    s->r = g >= PC_TILE_DIM ? 1u : PC_TILE_DIM / g;
    s->rw = (d->w + g - 1u) / g;
    s->rh = (d->h + g - 1u) / g;
    s->cx = (d->tiles_x + s->c - 1u) / s->c;
    s->cy = (d->tiles_y + s->c - 1u) / s->c;
    /* P-08: bounded by the document limits, still checked */
    if (!pc_mul_size((size_t)s->rw * 4u, (size_t)s->rh, &nred) ||
        !pc_mul_size((size_t)s->cx, (size_t)s->cy, &nkey))
        return false;
    s->red = (uint16_t *)calloc(nred, sizeof *s->red);
    s->key = (uint64_t *)calloc(nkey, sizeof *s->key);
    if (!s->red || !s->key) {
        src_free(s);
        return false;
    }
    s->dw = d->w;
    s->dh = d->h;
    s->cursor = 0;
    s->walking = false;
    s->done_valid = false;
    return true;
}

/* ---- pixel access -------------------------------------------------------------------- */
static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 29);
}

/* Tile data of a layer as the canvas shows it (open transaction included);
 * NULL = transparent. *ver (may be NULL) receives a content key. */
static const uint8_t *layer_tile(const app_doc *d, const pc_layer *l, uint32_t ti, uint64_t *ver)
{
    const uint8_t *data = NULL;
    uint64_t v = 0;
    const pc_tile *t;
    if (d->txn && pc_txn_lookup(d->txn, l->id, ti, &data, &v)) {
        if (ver) *ver = v ? v : 0x5A5A5A5A5A5A5A5Aull;
        return data;
    }
    t = l->grid[ti];
    if (ver) *ver = t ? t->serial : 0x3C3C3C3C3C3C3C3Cull;
    return t && t->bpp == 4u ? t->data : NULL;
}

static pc_px32 tile_px(const uint8_t *data, uint32_t lx, uint32_t ly)
{
    pc_px32 p;
    if (!data) {
        memset(&p, 0, sizeof p);
        return p;
    }
    memcpy(&p, data + ((size_t)ly * PC_TILE_DIM + lx) * 4u, 4u);
    return p;
}

/* Composite of the visible layers at one pixel (sampled blocks); the same
 * per-layer pc_composite_span sequence as pc_comp. */
static pc_px32 comp_px(const app_doc *d, uint32_t x, uint32_t y)
{
    const pc_doc *doc = d->doc;
    uint32_t ti = (y >> PC_TILE_SHIFT) * doc->tiles_x + (x >> PC_TILE_SHIFT);
    uint32_t lx = x & (PC_TILE_DIM - 1u), ly = y & (PC_TILE_DIM - 1u);
    pc_px32 acc;
    memset(&acc, 0, sizeof acc);
    for (uint32_t i = 0; i < doc->n_layers; i++) {
        const pc_layer *l = doc->stack[i];
        const uint8_t *data;
        pc_px32 s;
        if (!l->visible || l->opacity == 0u) continue;
        data = layer_tile(d, l, ti, NULL);
        if (!data) continue;
        s = tile_px(data, lx, ly);
        pc_composite_span(&acc, &s, 1u, l->mode, l->opacity);
    }
    return acc;
}

/* Content key of a cell; never 0. */
static uint64_t cell_key(const app_doc *d, const th_src *s, const pc_layer *l, uint32_t cxi,
                         uint32_t cyi, const pc_comp_opts *o)
{
    const pc_doc *doc = d->doc;
    uint64_t h = 0x243F6A8885A308D3ull;
    uint32_t tx0 = cxi * s->c, ty0 = cyi * s->c;
    for (uint32_t ty = ty0; ty < ty0 + s->c && ty < doc->tiles_y; ty++)
        for (uint32_t tx = tx0; tx < tx0 + s->c && tx < doc->tiles_x; tx++) {
            uint64_t v = 0;
            if (l) (void)layer_tile(d, l, ty * doc->tiles_x + tx, &v);
            else v = pc_comp_tile_sig(doc, tx, ty, o);
            h = mix(h, v);
        }
    return h ? h : 1u;
}

/* ---- stage 1: one cell --------------------------------------------------------------- */
typedef struct acc4 { uint64_t c[3]; uint32_t a, n; } acc4;

static void acc_px(const th_state *t, acc4 *ac, pc_px32 p)
{
    ac->c[0] += (uint64_t)p.a * t->lin[p.b];
    ac->c[1] += (uint64_t)p.a * t->lin[p.g];
    ac->c[2] += (uint64_t)p.a * t->lin[p.r];
    ac->a += p.a;
    ac->n++;
}

static void acc_store(th_src *s, uint32_t rx, uint32_t ry, const acc4 *ac)
{
    uint16_t *o = s->red + ((size_t)ry * s->rw + rx) * 4u;
    uint64_t n = ac->n ? ac->n : 1u;
    for (int k = 0; k < 3; k++) o[k] = (uint16_t)((ac->c[k] + n * 255u / 2u) / (n * 255u));
    o[3] = (uint16_t)(((uint64_t)ac->a * 257u + n / 2u) / n);
}

static void cell_full(const th_state *t, const app_doc *d, th_src *s, const uint8_t *data,
                      uint32_t tx, uint32_t ty)
{
    const pc_doc *doc = d->doc;
    uint32_t g = s->g;
    for (uint32_t by = 0; by < s->r; by++) {
        uint32_t ry = ty * s->r + by, y0 = by * g, ny;
        if (ry >= s->rh) break;
        ny = doc->h - (ty * PC_TILE_DIM + y0);
        if (ny > g) ny = g;
        for (uint32_t bx = 0; bx < s->r; bx++) {
            uint32_t rx = tx * s->r + bx, x0 = bx * g, nx;
            acc4 ac;
            if (rx >= s->rw) break;
            memset(&ac, 0, sizeof ac);
            nx = doc->w - (tx * PC_TILE_DIM + x0);
            if (nx > g) nx = g;
            if (!data) {
                ac.n = nx * ny;                 /* transparent */
            } else {
                for (uint32_t y = 0; y < ny; y++)
                    for (uint32_t x = 0; x < nx; x++)
                        acc_px(t, &ac, tile_px(data, x0 + x, y0 + y));
            }
            acc_store(s, rx, ry, &ac);
        }
    }
}

static void cell_compute(const th_state *t, const app_doc *d, th_src *s, const pc_layer *l,
                         uint32_t cxi, uint32_t cyi, const pc_comp_opts *o, pc_px32 *buf)
{
    const pc_doc *doc = d->doc;
    uint32_t g = s->g;
    if (g <= FULL_MAX_G) {
        /* one tile per cell (c == 1), every pixel of every block */
        const uint8_t *data = NULL;
        if (l) data = layer_tile(d, l, cyi * doc->tiles_x + cxi, NULL);
        else if (pc_comp_tile(doc, cxi, cyi, buf, o) == PC_OK) data = (const uint8_t *)buf;
        cell_full(t, d, s, data, cxi, cyi);
        return;
    }
    /* sampled blocks: r x r blocks per cell (r = 1 when a block spans tiles) */
    for (uint32_t by = 0; by < s->r; by++) {
        uint32_t ry = cyi * s->r + by;
        if (ry >= s->rh) break;
        for (uint32_t bx = 0; bx < s->r; bx++) {
            uint32_t rx = cxi * s->r + bx, x0 = rx * g, y0 = ry * g, ex, ey;
            acc4 ac;
            if (rx >= s->rw) break;
            memset(&ac, 0, sizeof ac);
            ex = doc->w - x0 < g ? doc->w - x0 : g;
            ey = doc->h - y0 < g ? doc->h - y0 : g;
            for (uint32_t j = 0; j < SAMPLES; j++) {
                uint32_t y = y0 + (uint32_t)(((uint64_t)(2u * j + 1u) * ey) / (2u * SAMPLES));
                for (uint32_t i = 0; i < SAMPLES; i++) {
                    uint32_t x = x0 + (uint32_t)(((uint64_t)(2u * i + 1u) * ex) / (2u * SAMPLES));
                    uint32_t lx = x & (PC_TILE_DIM - 1u), ly = y & (PC_TILE_DIM - 1u);
                    pc_px32 p;
                    if (l) {
                        uint32_t ti = (y >> PC_TILE_SHIFT) * doc->tiles_x + (x >> PC_TILE_SHIFT);
                        p = tile_px(layer_tile(d, l, ti, NULL), lx, ly);
                    } else if (o->overlay) {
                        /* a floating overlay: composite its tile (rare, exact) */
                        if (pc_comp_tile(doc, x >> PC_TILE_SHIFT, y >> PC_TILE_SHIFT, buf, o) ==
                            PC_OK)
                            p = buf[ly * PC_TILE_DIM + lx];
                        else
                            memset(&p, 0, sizeof p);
                    } else {
                        p = comp_px(d, x, y);
                    }
                    acc_px(t, &ac, p);
                }
            }
            acc_store(s, rx, ry, &ac);
        }
    }
}

/* Walk the cells of s from its cursor until done or out of budget. true
 * when the walk completed. */
static bool src_walk(th_state *t, const app_doc *d, th_src *s, const pc_layer *l, uint64_t t0)
{
    pc_comp_opts o = app_doc_comp_opts(d);
    uint32_t total = s->cx * s->cy;
    pc_px32 *buf = (pc_px32 *)malloc(PC_TILE_PX * sizeof *buf);
    if (!buf) return false;
    while (s->cursor < total) {
        uint32_t cxi = s->cursor % s->cx, cyi = s->cursor / s->cx;
        uint64_t k = cell_key(d, s, l, cxi, cyi, &o);
        if (k != s->key[s->cursor]) {
            cell_compute(t, d, s, l, cxi, cyi, &o, buf);
            s->key[s->cursor] = k;
            s->need_upload = true;
            t->recomputed++;
        }
        s->cursor++;
        if (!t->unlimited && (s->cursor & 15u) == 0u &&
            t->spent_ns + (pal_ticks_ns() - t0) > BUDGET_NS)
            break;
    }
    free(buf);
    return s->cursor >= total;
}

/* ---- stage 2 ------------------------------------------------------------------------- */
/* Box filter the reduced image to tw x th straight sRGB RGBA bytes. */
static bool src_output(const th_state *t, const th_src *s, int32_t tw, int32_t th, uint8_t *rgba)
{
    double sx = (double)s->dw / (double)tw, sy = (double)s->dh / (double)th, g = (double)s->g;
    double *row, *tmp;
    size_t ntmp;
    if (!pc_mul_size((size_t)tw * 4u, (size_t)s->rh, &ntmp)) return false;
    row = (double *)calloc((size_t)tw * 4u, sizeof *row);
    tmp = (double *)calloc(ntmp, sizeof *tmp);
    if (!row || !tmp) {
        free(row);
        free(tmp);
        return false;
    }
    /* horizontal: every reduced row to tw columns, weights = covered doc px */
    for (uint32_t ry = 0; ry < s->rh; ry++) {
        const uint16_t *src = s->red + (size_t)ry * s->rw * 4u;
        double *dst = tmp + (size_t)ry * (size_t)tw * 4u;
        for (int32_t u = 0; u < tw; u++) {
            double x0 = (double)u * sx, x1 = (double)(u + 1) * sx;
            uint32_t b0 = (uint32_t)(x0 / g), b1 = (uint32_t)ceil(x1 / g);
            if (b1 > s->rw) b1 = s->rw;
            for (uint32_t bx = b0; bx < b1; bx++) {
                double l0 = (double)bx * g, l1 = l0 + g, w;
                if (l1 > (double)s->dw) l1 = (double)s->dw;
                w = (x1 < l1 ? x1 : l1) - (x0 > l0 ? x0 : l0);
                if (w <= 0.0) continue;
                for (uint32_t k = 0; k < 4u; k++)
                    dst[(size_t)u * 4u + k] += w * (double)src[bx * 4u + k];
            }
        }
    }
    /* vertical, then unpremultiply and back to sRGB */
    for (int32_t v = 0; v < th; v++) {
        double y0 = (double)v * sy, y1 = (double)(v + 1) * sy, area = sx * sy;
        uint32_t b0 = (uint32_t)(y0 / g), b1 = (uint32_t)ceil(y1 / g);
        if (b1 > s->rh) b1 = s->rh;
        memset(row, 0, (size_t)tw * 4u * sizeof *row);
        for (uint32_t by = b0; by < b1; by++) {
            double l0 = (double)by * g, l1 = l0 + g, w;
            const double *src = tmp + (size_t)by * (size_t)tw * 4u;
            if (l1 > (double)s->dh) l1 = (double)s->dh;
            w = (y1 < l1 ? y1 : l1) - (y0 > l0 ? y0 : l0);
            if (w <= 0.0) continue;
            for (size_t i = 0; i < (size_t)tw * 4u; i++) row[i] += w * src[i];
        }
        for (int32_t u = 0; u < tw; u++) {
            uint8_t *o = rgba + ((size_t)v * (size_t)tw + (size_t)u) * 4u;
            double al = row[(size_t)u * 4u + 3u] / area;     /* 0..65535 */
            if (al < 128.0) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            for (uint32_t k = 0; k < 3u; k++) {
                /* premultiplied linear B, G, R over the alpha fraction */
                double lin = row[(size_t)u * 4u + k] / area * 65535.0 / al;
                int32_t idx = (int32_t)(lin / 16.0 + 0.5);
                if (idx < 0) idx = 0;
                if (idx > 4096) idx = 4096;
                o[2u - k] = t->srgb[idx];
            }
            o[3] = (uint8_t)(al / 257.0 + 0.5);
        }
    }
    free(tmp);
    free(row);
    return true;
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

/* ---- stamps -------------------------------------------------------------------------- */
static uint64_t comp_stamp(const app_doc *d)
{
    uint64_t h = mix(d->doc->gen, ((uint64_t)d->doc->w << 32) | d->doc->h);
    pc_comp_opts o = app_doc_comp_opts(d);
    h = mix(h, d->txn ? pc_txn_clock(d->txn) : 0u);
    h = mix(h, (uint64_t)(uintptr_t)o.overlay);
    if (o.overlay) h = mix(h, o.overlay->version);
    for (uint32_t i = 0; i < d->doc->n_layers; i++) {
        const pc_layer *l = d->doc->stack[i];
        /* layer properties too: the Layer Properties preview changes them
         * without a new generation */
        h = mix(h, ((uint64_t)l->id << 32) | ((uint64_t)l->mode << 16) |
                       ((uint64_t)l->opacity << 1) | (l->visible ? 1u : 0u));
        h = mix(h, l->gen);
    }
    return h;
}

static uint64_t layer_stamp(const app_doc *d, const pc_layer *l)
{
    uint64_t h = mix(l->gen, ((uint64_t)d->doc->w << 32) | d->doc->h);
    return mix(h, d->txn ? pc_txn_clock(d->txn) : 0u);
}

/* Advance one source; true when its texture should be (re)uploaded now. */
static bool src_step(app *a, th_state *t, const app_doc *d, th_src *s, const pc_layer *l,
                     uint64_t stamp, bool have_tex)
{
    uint64_t t0;
    if (!src_geometry(s, d->doc)) return false;
    if (!s->walking) {
        bool changed = !s->done_valid || s->done_stamp != stamp;
        if (!changed) return s->need_upload || !have_tex;
        /* throttle while edits keep coming (the first build is immediate) */
        if (have_tex && !t->unlimited && a->now - s->upload_ms < THROTTLE_MS) {
            app_request_frame_at(a, s->upload_ms + THROTTLE_MS);
            return false;
        }
        s->walking = true;
        s->cursor = 0;
        s->walk_stamp = stamp;
    }
    if (!t->unlimited && t->spent_ns > BUDGET_NS) {
        app_request_frame(a);
        return false;
    }
    t0 = pal_ticks_ns();
    if (!src_walk(t, d, s, l, t0)) {
        t->spent_ns += pal_ticks_ns() - t0;
        app_request_frame(a);
        return false;
    }
    t->spent_ns += pal_ticks_ns() - t0;
    s->walking = false;
    s->done_valid = true;
    s->done_stamp = s->walk_stamp;
    if (s->walk_stamp != stamp) app_request_frame(a);   /* it changed during the walk */
    return s->need_upload || !have_tex;
}

static bool src_texture(app *a, const th_state *t, th_src *s, SDL_Texture **tex, int32_t *cw,
                        int32_t *ch)
{
    int32_t tw, th;
    uint8_t *buf;
    fit(s->dw, s->dh, s->max_px, &tw, &th);
    buf = (uint8_t *)malloc((size_t)tw * (size_t)th * 4u);
    if (!buf) return false;
    if (src_output(t, s, tw, th, buf)) {
        *tex = upload(a, *tex, cw, ch, tw, th, buf);
        s->need_upload = false;
        s->upload_ms = a->now;
        /* the UI of this frame was declared before the upload: show the new
         * thumbnail in the next one (rendering is on demand) */
        app_request_frame(a);
    }
    free(buf);
    return *tex != NULL;
}

static app_layer_thumb *lthumb(app_doc *d, uint32_t id)
{
    for (uint32_t i = 0; i < d->n_lthumbs; i++)
        if (d->lthumbs[i].layer_id == id) return &d->lthumbs[i];
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

static void update_doc(app *a, th_state *t, app_doc *d, bool layers)
{
    th_doc *r;
    if (!d->doc || d->doc->w == 0u) return;
    r = doc_rec(t, d->id);
    if (!r) return;
    /* composite (image list) */
    if (src_step(a, t, d, &r->comp, NULL, comp_stamp(d), d->thumb != NULL) &&
        src_texture(a, t, &r->comp, &d->thumb, &d->thumb_w, &d->thumb_h)) {
        d->thumb_gen = d->doc->gen;
        d->thumb_time = a->now;
    }
    if (!layers) return;
    /* layers, top rows first */
    for (uint32_t i = 0; i < r->nlay; i++) r->lay[i].seen = false;
    for (uint32_t i = 0; i < d->doc->n_layers; i++) {
        const pc_layer *l = d->doc->stack[d->doc->n_layers - 1u - i];
        th_src *s = layer_rec(r, l->id);
        app_layer_thumb *lt = lthumb(d, l->id);
        if (!s || !lt) continue;
        s->seen = true;
        if (src_step(a, t, d, s, l, layer_stamp(d, l), lt->tex != NULL)) {
            int32_t cw = lt->tex ? lt->w : 0, chh = lt->tex ? lt->h : 0;
            if (src_texture(a, t, s, &lt->tex, &cw, &chh)) {
                lt->w = cw;
                lt->h = chh;
                lt->gen = l->gen;
                d->lthumb_time = a->now;
            }
        }
    }
    /* drop records and textures of layers that left the document */
    for (uint32_t i = 0; i < r->nlay;) {
        if (r->lay[i].seen) {
            i++;
            continue;
        }
        src_free(&r->lay[i]);
        r->lay[i] = r->lay[--r->nlay];
    }
    for (uint32_t i = 0; i < d->n_lthumbs;) {
        if (!pc_doc_layer_by_id(d->doc, d->lthumbs[i].layer_id)) {
            if (d->lthumbs[i].tex) SDL_DestroyTexture(d->lthumbs[i].tex);
            d->lthumbs[i] = d->lthumbs[--d->n_lthumbs];
        } else {
            i++;
        }
    }
}

void app_thumbs_update(app *a, app_doc *d, bool layers)
{
    th_state *t = state(a);
    if (!t || !d) return;
    if (t->frame != a->frame_no + 1u) {      /* a new frame: new budget, sweep closed docs */
        t->frame = a->frame_no + 1u;
        t->spent_ns = 0;
        sweep(a, t);
    }
    /* layer thumbnails only matter while the Layers window shows them */
    update_doc(a, t, d, layers && app_panel_open(a, "layers"));
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

/* ---- lane API (pnl.h) ---------------------------------------------------------------- */
void pnl_thumbs_sync(app *a)
{
    th_state *t = state(a);
    if (!t) return;
    t->unlimited = true;
    sweep(a, t);
    for (int32_t i = 0; i < a->ndocs; i++)
        update_doc(a, t, a->docs[i], i == a->active && app_panel_open(a, "layers"));
    t->unlimited = false;
}

uint64_t pnl_thumbs_recomputed(const app *a)
{
    const th_state *t = (const th_state *)app_ext_get(a, "pnl.thumbs");
    return t ? t->recomputed : 0u;
}

bool pnl_thumb_render(app *a, app_doc *d, uint32_t layer_id, int32_t max_px, uint8_t **rgba,
                      int32_t *w, int32_t *h)
{
    th_state *t = state(a);
    th_src s;
    const pc_layer *l = NULL;
    bool ok = false;
    *rgba = NULL;
    *w = *h = 0;
    if (!t || !d || max_px < 1) return false;
    if (layer_id) {
        l = pc_doc_layer_by_id(d->doc, layer_id);
        if (!l) return false;
    }
    memset(&s, 0, sizeof s);
    s.layer_id = layer_id;
    s.max_px = max_px;
    if (!src_geometry(&s, d->doc)) return false;
    t->unlimited = true;
    if (src_walk(t, d, &s, l, pal_ticks_ns())) {
        fit(s.dw, s.dh, max_px, w, h);
        *rgba = (uint8_t *)malloc((size_t)*w * (size_t)*h * 4u);
        ok = *rgba && src_output(t, &s, *w, *h, *rgba);
    }
    t->unlimited = false;
    src_free(&s);
    if (!ok) {
        free(*rgba);
        *rgba = NULL;
    }
    return ok;
}
