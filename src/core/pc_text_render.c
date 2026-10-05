/* pc_text_render.c - glyph outline cache, synthetic italic and bold,
 * underline and strikeout geometry, and rendering of pc_text through
 * pc_vrender (lane E3). Color glyphs (COLR layers and bitmap strikes),
 * their image cache and the Sharp mode hinting hook were added by lane
 * TOOLB. */
#include "pc_text_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_TOL 0.02           /* flattening tolerance, pixels */
#define CACHE_MAX 4096u          /* entries before the cache is flushed */

/* Color glyph images (P-08: every size below is checked before use). */
#define CIMG_MAX_ENTRIES 512u                /* cached images */
#define CIMG_BUDGET ((size_t)64u << 20)      /* cached image bytes kept between renders */
#define CIMG_RENDER_MAX ((size_t)256u << 20) /* image bytes one render may create */
#define CIMG_MAX_PX ((size_t)4u << 20)       /* pixels of one image at full resolution */
#define CIMG_SMALL_PX ((size_t)256u * 256u)  /* pixels per image once a render is over budget */
#define CIMG_MAX_SIDE 65536.0                /* document extent of one glyph image */
#define COORD_CLAMP 536870912.0              /* 2^29, as pc_shapes_render.c */
#define ROW_CANDIDATES 256u                  /* images sorted per row; more keep y order */

/* ---- caches ----------------------------------------------------------------------------- */

static void poly_cache_clear(pc_text *t)
{
    for (size_t i = 0; i < t->cache_cap; i++)
        if (t->cache[i].used) {
            pc_poly_free(&t->cache[i].poly);
            t->cache[i].used = false;
        }
    t->cache_n = 0;
}

static void cimg_free(pc_text_cimg *c)
{
    if (!c) return;
    free(c->px);
    free(c);
}

static void cimg_clear(pc_text *t)
{
    for (size_t i = 0; i < t->n_cimg; i++) cimg_free(t->cimg[i]);
    t->n_cimg = 0;
    t->cimg_bytes = 0;
}

void pc_text_cache_clear(pc_text *t)
{
    poly_cache_clear(t);
    cimg_clear(t);
    t->hint_zones = false;
}

void pc_text_cache_free(pc_text *t)
{
    pc_text_cache_clear(t);
    free(t->cache);
    t->cache = NULL;
    t->cache_cap = 0;
    free((void *)t->cimg);
    t->cimg = NULL;
    t->cap_cimg = 0;
    pc_raster_destroy(t->ras);
    t->ras = NULL;
}

static size_t slot_of(uint32_t face, uint32_t gid, size_t cap)
{
    uint32_t h = face * 0x9E3779B1u ^ gid * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (size_t)h & (cap - 1u);
}

static pc_text_gent *cache_find(pc_text *t, uint32_t face, uint32_t gid)
{
    size_t i;
    if (!t->cache_cap) return NULL;
    i = slot_of(face, gid, t->cache_cap);
    while (t->cache[i].used) {
        if (t->cache[i].face == face && t->cache[i].gid == gid) return &t->cache[i];
        i = (i + 1u) & (t->cache_cap - 1u);
    }
    return NULL;
}

/* Grow (rehash) so one more entry keeps the load at or below one half. */
static pc_status cache_reserve(pc_text *t)
{
    size_t ncap, bytes;
    pc_text_gent *nc;
    if (t->cache_n >= CACHE_MAX) poly_cache_clear(t);
    if (t->cache_cap && (t->cache_n + 1u) * 2u <= t->cache_cap) return PC_OK;
    ncap = t->cache_cap ? t->cache_cap * 2u : 64u;
    if (!pc_mul_size(ncap, sizeof *nc, &bytes)) return PC_ERR_LIMIT;
    nc = (pc_text_gent *)calloc(ncap, sizeof *nc);
    if (!nc) return PC_ERR_NOMEM;
    for (size_t i = 0; i < t->cache_cap; i++) {
        if (t->cache[i].used) {
            size_t j = slot_of(t->cache[i].face, t->cache[i].gid, ncap);
            while (nc[j].used) j = (j + 1u) & (ncap - 1u);
            nc[j] = t->cache[i];
        }
    }
    free(t->cache);
    t->cache = nc;
    t->cache_cap = ncap;
    return PC_OK;
}

static void reverse_contours(pc_poly *p)
{
    for (size_t c = 0; c < p->n_contours; c++) {
        size_t s = pc_poly_contour_start(p, c), e = p->ends[c];
        while (s + 1u < e) {
            pc_pt tp = p->pts[s];
            uint8_t tf = p->flags[s];
            e--;
            p->pts[s] = p->pts[e];
            p->flags[s] = p->flags[e];
            p->pts[e] = tp;
            p->flags[e] = tf;
            s++;
        }
    }
}

/* ---- Sharp mode zones (lane TOOLB) ---------------------------------------------------------- */

/* Top of a glyph's outline above the baseline in pixels, 0 when none. */
static double glyph_top(const pc_text *t, const pc_font_face *f, uint32_t cp)
{
    pc_path p;
    double top = 0.0;
    uint32_t gid;
    if (!f->outline) return 0.0;
    gid = f->glyph(f->ud, cp);
    if (!gid) return 0.0;
    pc_path_init(&p);
    if (f->outline(f->ud, gid, t->em, PC_TEXT_SMOOTH, &p) == PC_OK) {
        for (size_t i = 0; i < p.n_pts; i++)
            if (isfinite(p.pts[i].y) && -p.pts[i].y > top) top = -p.pts[i].y;
    }
    pc_path_free(&p);
    return top < t->em * 2.0 ? top : 0.0;
}

/* x-height and cap-height of the primary face for the hinter: the face's
 * metrics, else measured from 'x' and 'H'. */
static void hint_zones(pc_text *t)
{
    if (t->hint_zones) return;
    t->hint_xh = t->fm.x_height > 0.0 && isfinite(t->fm.x_height) ? t->fm.x_height : 0.0;
    t->hint_cap = t->fm.cap_height > 0.0 && isfinite(t->fm.cap_height) ? t->fm.cap_height : 0.0;
    if (t->n_faces) {
        if (t->hint_xh == 0.0) t->hint_xh = glyph_top(t, t->faces[0], 'x');
        if (t->hint_cap == 0.0) t->hint_cap = glyph_top(t, t->faces[0], 'H');
    }
    t->hint_zones = true;
}

/* Coverage polygon of one glyph at pen (0, 0): outline (grid-fitted in the
 * Sharp modes, except in color faces whose layers must stay aligned),
 * synthetic slant, outer contours turned counter-clockwise
 * on screen (the orientation of every stroke ring, so the emboldening
 * stroke unions under nonzero), and the emboldening stroke. */
static pc_status build_glyph(pc_text *t, const pc_font_face *f, uint32_t gid, pc_poly *dst)
{
    pc_path path;
    pc_status st = PC_OK;
    if (!f->outline) return PC_OK;
    pc_path_init(&path);
    st = f->outline(f->ud, gid, t->em, t->style.mode, &path);
    if (st == PC_OK && t->style.mode != PC_TEXT_SMOOTH && path.n_pts && !f->color) {
        hint_zones(t);
        pc_text_hint_outline(&path, t->style.mode, t->em, t->hint_xh, t->hint_cap);
    }
    if (st == PC_OK && t->style.italic && !f->italic) {
        pc_affine sh;
        sh.a = 1.0; sh.b = 0.0; sh.c = -PC_TEXT_SLANT; sh.d = 1.0; sh.e = 0.0; sh.f = 0.0;
        pc_path_transform(&path, &sh);
    }
    if (st == PC_OK) st = pc_path_flatten(&path, NULL, GLYPH_TOL, dst);
    if (st == PC_OK && pc_poly_area(dst) > 0.0) reverse_contours(dst);
    if (st == PC_OK && t->style.bold && !f->bold && dst->n_pts) {
        pc_poly ring;
        pc_stroke sk;
        pc_poly_init(&ring);
        pc_stroke_default(&sk);
        sk.width = t->em * PC_TEXT_BOLD_FRAC;
        sk.join = PC_JOIN_ROUND;
        st = pc_poly_stroke(dst, &sk, GLYPH_TOL, &ring);
        if (st == PC_OK) st = pc_poly_append(dst, &ring, NULL);
        pc_poly_free(&ring);
    }
    pc_path_free(&path);
    return st;
}

static bool bitmap_ok(const pc_font_bitmap *b)
{
    return b->px && b->w > 0 && b->h > 0 && b->w <= 16384 && b->h <= 16384 &&
           b->scale > 0.0 && isfinite(b->scale) && isfinite(b->left) && isfinite(b->top) &&
           fabs(b->left) < 1e7 && fabs(b->top) < 1e7 && b->scale < 1e5;
}

/* Kind of a glyph (lane TOOLB): layers, bitmap or a plain outline. */
static pc_status classify(pc_text *t, const pc_font_face *f, uint32_t gid, uint8_t *kind,
                          bool *fg)
{
    *kind = PC_GK_MONO;
    *fg = false;
    if (f->color_layers) {
        size_t n = f->color_layers(f->ud, gid, NULL, 0u);
        if (n > PC_TEXT_MAX_COLOR_LAYERS) n = PC_TEXT_MAX_COLOR_LAYERS;
        if (n) {
            pc_font_color_layer *l = (pc_font_color_layer *)calloc(n, sizeof *l);
            size_t got;
            if (!l) return PC_ERR_NOMEM;
            got = f->color_layers(f->ud, gid, l, n);
            if (got > n) got = n;
            for (size_t i = 0; i < got; i++)
                if (l[i].foreground) *fg = true;
            free(l);
            if (got) {
                *kind = PC_GK_LAYERS;
                return PC_OK;
            }
        }
    }
    if (f->color_bitmap) {
        pc_font_bitmap bm;
        pc_status st;
        memset(&bm, 0, sizeof bm);
        st = f->color_bitmap(f->ud, gid, t->em, &bm);
        if (st == PC_OK && bitmap_ok(&bm)) *kind = PC_GK_BITMAP;
        else if (st != PC_OK && st != PC_ERR_UNSUPPORTED) return st;
    }
    return PC_OK;
}

/* The cache entry of (face, gid), created (outline, kind) on first use.
 * The pointer is valid until the next glyph_entry call. */
static pc_status glyph_entry(pc_text *t, uint32_t face, uint32_t gid, pc_text_gent **out)
{
    pc_text_gent *e = cache_find(t, face, gid);
    pc_status st;
    uint8_t kind;
    bool fg;
    size_t i;
    if (e) {
        *out = e;
        return PC_OK;
    }
    st = classify(t, t->faces[face], gid, &kind, &fg);
    if (st != PC_OK) return st;
    st = cache_reserve(t);
    if (st != PC_OK) return st;
    i = slot_of(face, gid, t->cache_cap);
    while (t->cache[i].used) i = (i + 1u) & (t->cache_cap - 1u);
    e = &t->cache[i];
    pc_poly_init(&e->poly);
    if (kind == PC_GK_MONO) st = build_glyph(t, t->faces[face], gid, &e->poly);
    if (st != PC_OK) {
        pc_poly_free(&e->poly);
        return st;
    }
    e->face = face;
    e->gid = gid;
    e->kind = kind;
    e->fg = fg;
    e->used = true;
    t->cache_n++;
    *out = e;
    return PC_OK;
}

/* Outline polygon of a layer glyph (always the outline, whatever its own
 * kind). Valid until the next glyph_entry or layer_poly call. */
static pc_status layer_poly(pc_text *t, uint32_t face, uint32_t gid, const pc_poly **out)
{
    pc_text_gent *e;
    pc_status st = glyph_entry(t, face, gid, &e);
    if (st != PC_OK) return st;
    if (e->kind != PC_GK_MONO && !e->poly.n_pts) {
        /* a color glyph used as a layer: its plain outline */
        st = build_glyph(t, t->faces[face], gid, &e->poly);
        if (st != PC_OK) {
            pc_poly_clear(&e->poly);
            return st;
        }
    }
    *out = &e->poly;
    return PC_OK;
}

/* Rectangle from x0 to x1 centered on y with thickness th, counter-
 * clockwise on screen; snapped to whole pixel rows when the style snaps. */
static pc_status add_bar(const pc_text *t, pc_poly *out, double x0, double x1, double y, double th)
{
    double y0 = y - 0.5 * th, y1 = y + 0.5 * th;
    pc_status st;
    if (t->style.snap) {
        double h = floor(th + 0.5);
        if (h < 1.0) h = 1.0;
        y0 = floor(y - 0.5 * h + 0.5);
        y1 = y0 + h;
    }
    st = pc_poly_add(out, pc_pt_make(x0, y0), 0u);
    if (st == PC_OK) st = pc_poly_add(out, pc_pt_make(x0, y1), 0u);
    if (st == PC_OK) st = pc_poly_add(out, pc_pt_make(x1, y1), 0u);
    if (st == PC_OK) st = pc_poly_add(out, pc_pt_make(x1, y0), 0u);
    if (st == PC_OK) st = pc_poly_end(out, true);
    return st;
}

pc_status pc_text_build(pc_text *t, pc_poly *out)
{
    pc_status st = PC_OK;
    if (!t || !out) return PC_ERR_ARG;
    if (t->n_faces) {
        for (size_t i = 0; i < t->n_glyphs && st == PC_OK; i++) {
            const pc_text_glyph *g = &t->glyphs[i];
            pc_text_gent *e;
            if (g->hidden) continue;
            st = glyph_entry(t, g->face, g->gid, &e);
            if (st == PC_OK && e->kind == PC_GK_MONO && e->poly.n_pts) {
                pc_affine m = pc_affine_translate(g->x, g->y);
                st = pc_poly_append(out, &e->poly, &m);
            }
        }
    }
    for (size_t i = 0; i < t->n_lines && st == PC_OK; i++) {
        const pc_text_line *L = &t->lines[i];
        if (!(L->width > 0.0)) continue;
        if (t->style.underline)
            st = add_bar(t, out, L->x, L->x + L->width, L->baseline + t->fm.underline_offset,
                         t->fm.underline_thickness);
        if (st == PC_OK && t->style.strikeout)
            st = add_bar(t, out, L->x, L->x + L->width, L->baseline + t->fm.strike_offset,
                         t->fm.strike_thickness);
    }
    return st;
}

size_t pc_text_color_glyph_count(pc_text *t)
{
    size_t n = 0;
    if (!t) return 0u;
    for (size_t i = 0; i < t->n_glyphs && t->n_faces; i++) {
        const pc_text_glyph *g = &t->glyphs[i];
        pc_text_gent *e;
        if (g->hidden) continue;
        if (glyph_entry(t, g->face, g->gid, &e) != PC_OK) return 0u;
        if (e->kind != PC_GK_MONO) n++;
    }
    return n;
}

/* ---- color glyph images (lane TOOLB) --------------------------------------------------------- */

static int32_t clamp_i32(double v)
{
    if (!(v > -COORD_CLAMP)) return -(int32_t)COORD_CLAMP;
    if (v > COORD_CLAMP) return (int32_t)COORD_CLAMP;
    return (int32_t)v;
}

static uint8_t to_u8(double v)
{
    if (!(v > 0.0)) return 0u;
    if (v >= 255.0) return 255u;
    return (uint8_t)(v + 0.5);
}

/* Premultiplied accumulator (doubles, 0..1) to straight 8-bit pixels. */
static void acc_to_px(const double *acc, pc_px32 *px, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const double *p = acc + 4u * i;
        double a = p[3];
        pc_px32 o;
        memset(&o, 0, sizeof o);
        if (a > 0.0) {
            o.a = to_u8(a * 255.0);
            if (o.a) {
                o.b = to_u8(p[0] / a * 255.0);
                o.g = to_u8(p[1] / a * 255.0);
                o.r = to_u8(p[2] / a * 255.0);
            }
        }
        px[i] = o;
    }
}

/* Image geometry from a document-space extent: resolution reduced so the
 * image has at most max_px pixels. False when the extent is unusable. */
static bool image_geom(pc_text_cimg *c, double x0, double y0, double x1, double y1, size_t max_px)
{
    double dw, dh, k = 1.0;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) || x1 <= x0 ||
        y1 <= y0 || fabs(x0) > COORD_CLAMP || fabs(y0) > COORD_CLAMP)
        return false;
    c->ox = clamp_i32(floor(x0));
    c->oy = clamp_i32(floor(y0));
    dw = ceil(x1) - (double)c->ox;
    dh = ceil(y1) - (double)c->oy;
    if (!(dw >= 1.0) || !(dh >= 1.0) || dw > CIMG_MAX_SIDE || dh > CIMG_MAX_SIDE) return false;
    if (dw * dh > (double)max_px) k = sqrt(dw * dh / (double)max_px);
    c->dw = (int32_t)dw;
    c->dh = (int32_t)dh;
    c->k = k;
    c->w = (int32_t)ceil(dw / k);
    c->h = (int32_t)ceil(dh / k);
    if (c->w < 1) c->w = 1;
    if (c->h < 1) c->h = 1;
    return true;
}

/* Allocate px and the premultiplied accumulator for c. */
static pc_status image_alloc(pc_text_cimg *c, double **acc)
{
    size_t n, bytes, abytes;
    *acc = NULL;
    if (!pc_mul_size((size_t)c->w, (size_t)c->h, &n) || !pc_mul_size(n, sizeof *c->px, &bytes) ||
        !pc_mul_size(n, 4u * sizeof(double), &abytes))
        return PC_ERR_LIMIT;
    c->px = (pc_px32 *)malloc(bytes);
    *acc = (double *)calloc(n, 4u * sizeof(double));
    if (!c->px || !*acc) {
        free(c->px);
        c->px = NULL;
        free(*acc);
        *acc = NULL;
        return PC_ERR_NOMEM;
    }
    c->bytes = bytes;
    return PC_OK;
}

/* COLR: the layers rasterized and composited bottom first. */
static pc_status make_layers(pc_text *t, pc_text_cimg *c, size_t max_px)
{
    const pc_font_face *f = t->faces[c->face];
    pc_font_color_layer *l;
    size_t n, got;
    double qx = (double)c->qx * 0.25, qy = (double)c->qy * 0.25;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0, *acc = NULL;
    bool any = false;
    uint8_t *mask = NULL;
    pc_status st = PC_OK;
    n = f->color_layers(f->ud, c->gid, NULL, 0u);
    if (n > PC_TEXT_MAX_COLOR_LAYERS) n = PC_TEXT_MAX_COLOR_LAYERS;
    if (!n) return PC_OK;
    l = (pc_font_color_layer *)calloc(n, sizeof *l);
    if (!l) return PC_ERR_NOMEM;
    got = f->color_layers(f->ud, c->gid, l, n);
    if (got > n) got = n;
    for (size_t i = 0; i < got && st == PC_OK; i++) {
        const pc_poly *p;
        pc_pt mn, mx;
        st = layer_poly(t, c->face, l[i].gid, &p);
        if (st != PC_OK || !pc_poly_bounds(p, &mn, &mx)) continue;
        if (!any) {
            x0 = mn.x; y0 = mn.y; x1 = mx.x; y1 = mx.y;
            any = true;
        } else {
            if (mn.x < x0) x0 = mn.x;
            if (mn.y < y0) y0 = mn.y;
            if (mx.x > x1) x1 = mx.x;
            if (mx.y > y1) y1 = mx.y;
        }
    }
    if (st != PC_OK || !any || !image_geom(c, x0 + qx, y0 + qy, x1 + qx, y1 + qy, max_px)) {
        free(l);
        return st;
    }
    st = image_alloc(c, &acc);
    if (st == PC_OK) {
        mask = (uint8_t *)malloc((size_t)c->w * (size_t)c->h);
        if (!mask) st = PC_ERR_NOMEM;
    }
    if (st == PC_OK && !t->ras) {
        t->ras = pc_raster_create();
        if (!t->ras) st = PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < got && st == PC_OK; i++) {
        const pc_poly *p;
        pc_affine m;
        pc_mask mk;
        pc_px32 col = l[i].foreground ? c->fg : l[i].color;
        double ca = (double)col.a / 255.0, cb, cg, cr;
        size_t count = (size_t)c->w * (size_t)c->h;
        st = layer_poly(t, c->face, l[i].gid, &p);
        if (st != PC_OK || !p->n_pts || col.a == 0u) continue;
        m.a = 1.0 / c->k; m.b = 0.0; m.c = 0.0; m.d = 1.0 / c->k;
        m.e = (qx - (double)c->ox) / c->k;
        m.f = (qy - (double)c->oy) / c->k;
        pc_raster_reset(t->ras);
        st = pc_raster_add_poly(t->ras, p, &m);
        mk.px = mask;
        mk.x = 0;
        mk.y = 0;
        mk.w = c->w;
        mk.h = c->h;
        mk.stride = c->w;
        if (st == PC_OK) st = pc_raster_fill(t->ras, &mk, PC_FILL_NONZERO, c->aa);
        if (st != PC_OK) break;
        cb = (double)col.b / 255.0;
        cg = (double)col.g / 255.0;
        cr = (double)col.r / 255.0;
        for (size_t k = 0; k < count; k++) {
            double a, inv;
            double *o;
            if (!mask[k]) continue;
            a = ca * (double)mask[k] / 255.0;
            inv = 1.0 - a;
            o = acc + 4u * k;
            o[0] = cb * a + o[0] * inv;
            o[1] = cg * a + o[1] * inv;
            o[2] = cr * a + o[2] * inv;
            o[3] = a + o[3] * inv;
        }
    }
    if (st == PC_OK && c->px) acc_to_px(acc, c->px, (size_t)c->w * (size_t)c->h);
    if (st != PC_OK) {
        free(c->px);
        c->px = NULL;
        c->bytes = 0;
    }
    free(mask);
    free(acc);
    free(l);
    return st;
}

/* Premultiplied sample of bitmap b at source pixel (x, y), 0 outside. */
static void bm_texel(const pc_font_bitmap *b, int64_t x, int64_t y, double w, double *o)
{
    const pc_px32 *p;
    double a;
    if (x < 0 || y < 0 || x >= b->w || y >= b->h || w == 0.0) return;
    p = &b->px[(size_t)y * (size_t)b->w + (size_t)x];
    if (!p->a) return;
    a = (double)p->a / 255.0 * w;
    o[0] += (double)p->b / 255.0 * a;
    o[1] += (double)p->g / 255.0 * a;
    o[2] += (double)p->r / 255.0 * a;
    o[3] += a;
}

/* Bitmap strike resampled to the image: an area average when shrinking
 * (outside the bitmap counts as transparent, so its edges are
 * antialiased), bilinear when enlarging (edge texels repeat up to the
 * bitmap's box); synthetic italic shears the sampling. */
static pc_status make_bitmap(pc_text *t, pc_text_cimg *c, size_t max_px)
{
    const pc_font_face *f = t->faces[c->face];
    pc_font_bitmap b;
    double qx = (double)c->qx * 0.25, qy = (double)c->qy * 0.25, slant, *acc = NULL;
    double x0, x1, y0, y1, sh0, sh1, fw;
    pc_status st;
    memset(&b, 0, sizeof b);
    st = f->color_bitmap(f->ud, c->gid, t->em, &b);
    if (st == PC_ERR_UNSUPPORTED || (st == PC_OK && !bitmap_ok(&b))) return PC_OK;
    if (st != PC_OK) return st;
    slant = t->style.italic && !f->italic ? PC_TEXT_SLANT : 0.0;
    y0 = b.top;
    y1 = b.top + (double)b.h * b.scale;
    sh0 = -slant * y0;
    sh1 = -slant * y1;
    x0 = b.left + (sh0 < sh1 ? sh0 : sh1);
    x1 = b.left + (double)b.w * b.scale + (sh0 < sh1 ? sh1 : sh0);
    if (!image_geom(c, x0 + qx, y0 + qy, x1 + qx, y1 + qy, max_px)) return PC_OK;
    st = image_alloc(c, &acc);
    if (st != PC_OK) return st;
    fw = c->k / b.scale;                 /* source pixels per image pixel */
    for (int32_t j = 0; j < c->h; j++) {
        double ly = (double)c->oy + ((double)j + 0.5) * c->k - qy;   /* pen relative */
        double sy = (ly - b.top) / b.scale;
        for (int32_t i = 0; i < c->w; i++) {
            double lx = (double)c->ox + ((double)i + 0.5) * c->k - qx + slant * ly;
            double sx = (lx - b.left) / b.scale;
            double *o = acc + 4u * ((size_t)j * (size_t)c->w + (size_t)i);
            if (fw > 1.0) {
                /* area average over the footprint (outside = transparent) */
                double ax = sx - 0.5 * fw, bx = sx + 0.5 * fw;
                double ay = sy - 0.5 * fw, by = sy + 0.5 * fw, inv = 1.0 / (fw * fw);
                double fcx0, fcx1, fcy0, fcy1;
                int64_t cx0, cx1, cy0, cy1;
                if (bx <= 0.0 || by <= 0.0 || ax >= (double)b.w || ay >= (double)b.h) continue;
                fcx0 = floor(ax);
                fcx1 = ceil(bx);
                fcy0 = floor(ay);
                fcy1 = ceil(by);
                cx0 = fcx0 < 0.0 ? 0 : (int64_t)fcx0;
                cy0 = fcy0 < 0.0 ? 0 : (int64_t)fcy0;
                cx1 = fcx1 > (double)b.w ? b.w : (int64_t)fcx1;
                cy1 = fcy1 > (double)b.h ? b.h : (int64_t)fcy1;
                for (int64_t yy = cy0; yy < cy1; yy++) {
                    double wy = fmin(by, (double)yy + 1.0) - fmax(ay, (double)yy);
                    if (wy <= 0.0) continue;
                    for (int64_t xx = cx0; xx < cx1; xx++) {
                        double wx = fmin(bx, (double)xx + 1.0) - fmax(ax, (double)xx);
                        if (wx > 0.0) bm_texel(&b, xx, yy, wx * wy * inv, o);
                    }
                }
            } else {
                /* bilinear, edge texels repeated inside the bitmap's box */
                double px = sx - 0.5, py = sy - 0.5, fx, fy;
                int64_t ix, iy, ix1, iy1;
                if (sx < 0.0 || sy < 0.0 || sx >= (double)b.w || sy >= (double)b.h) continue;
                ix = (int64_t)floor(px);
                iy = (int64_t)floor(py);
                fx = px - (double)ix;
                fy = py - (double)iy;
                ix1 = ix + 1 < b.w ? ix + 1 : b.w - 1;
                iy1 = iy + 1 < b.h ? iy + 1 : b.h - 1;
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                bm_texel(&b, ix, iy, (1.0 - fx) * (1.0 - fy), o);
                bm_texel(&b, ix1, iy, fx * (1.0 - fy), o);
                bm_texel(&b, ix, iy1, (1.0 - fx) * fy, o);
                bm_texel(&b, ix1, iy1, fx * fy, o);
            }
        }
    }
    acc_to_px(acc, c->px, (size_t)c->w * (size_t)c->h);
    free(acc);
    return PC_OK;
}

/* One color glyph instance of a render. */
typedef struct cinst {
    const pc_text_cimg *img;
    int32_t x, y;                 /* document position of the image's top-left */
    size_t  order;                /* glyph order (later glyphs on top) */
} cinst;

typedef struct crender {
    cinst          *v;            /* sorted by y */
    size_t          n, cap;
    int32_t         maxh;
    pc_text_cimg  **tmp;          /* images not kept in the cache (owned) */
    size_t          ntmp, captmp;
    size_t          made;         /* image bytes created by this render */
} crender;

static void crender_free(crender *r)
{
    for (size_t i = 0; i < r->ntmp; i++) cimg_free(r->tmp[i]);
    free((void *)r->tmp);
    free(r->v);
    memset(r, 0, sizeof *r);
}

static pc_px32 fg_color(const pc_paint_src *src, int32_t x, int32_t y)
{
    pc_px32 c;
    if (!src) {
        c.b = 0u; c.g = 0u; c.r = 0u; c.a = 255u;
        return c;
    }
    if (!src->row) return src->solid;
    src->row(src->ud, x, y, 1, &c);
    return c;
}

static bool same_px(pc_px32 a, pc_px32 b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

/* The cached image for a key, else NULL. */
static const pc_text_cimg *cimg_find(const pc_text *t, const pc_text_cimg *key)
{
    for (size_t i = 0; i < t->n_cimg; i++) {
        const pc_text_cimg *c = t->cimg[i];
        if (c->face == key->face && c->gid == key->gid && c->qx == key->qx &&
            c->qy == key->qy && c->aa == key->aa && same_px(c->fg, key->fg))
            return c;
    }
    return NULL;
}

/* Make the image for key (kind), keep it in the cache when it fits, else
 * in the render's temporaries. */
static pc_status cimg_make(pc_text *t, crender *r, const pc_text_cimg *key, uint8_t kind,
                           const pc_text_cimg **out)
{
    pc_text_cimg *c = (pc_text_cimg *)calloc(1u, sizeof *c);
    size_t max_px = r->made > CIMG_RENDER_MAX ? CIMG_SMALL_PX : CIMG_MAX_PX;
    pc_status st;
    *out = NULL;
    if (!c) return PC_ERR_NOMEM;
    *c = *key;
    c->px = NULL;
    c->bytes = 0;
    c->w = c->h = 0;
    st = kind == PC_GK_LAYERS ? make_layers(t, c, max_px) : make_bitmap(t, c, max_px);
    if (st != PC_OK) {
        cimg_free(c);
        return st;
    }
    r->made += c->bytes;
    /* reduced-budget images are not kept: the next render may afford more */
    if (max_px == CIMG_MAX_PX && t->n_cimg < CIMG_MAX_ENTRIES &&
        t->cimg_bytes + c->bytes <= 2u * CIMG_BUDGET) {
        if (t->n_cimg == t->cap_cimg) {
            size_t nc = t->cap_cimg ? t->cap_cimg * 2u : 16u;
            pc_text_cimg **nv = (pc_text_cimg **)realloc((void *)t->cimg, nc * sizeof *nv);
            if (!nv) {
                cimg_free(c);
                return PC_ERR_NOMEM;
            }
            t->cimg = nv;
            t->cap_cimg = nc;
        }
        t->cimg[t->n_cimg++] = c;
        t->cimg_bytes += c->bytes;
    } else {
        if (r->ntmp == r->captmp) {
            size_t nc = r->captmp ? r->captmp * 2u : 16u;
            pc_text_cimg **nv = (pc_text_cimg **)realloc((void *)r->tmp, nc * sizeof *nv);
            if (!nv) {
                cimg_free(c);
                return PC_ERR_NOMEM;
            }
            r->tmp = nv;
            r->captmp = nc;
        }
        r->tmp[r->ntmp++] = c;
    }
    *out = c;
    return PC_OK;
}

static int cinst_cmp(const void *x, const void *y)
{
    const cinst *a = (const cinst *)x, *b = (const cinst *)y;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

/* Every color glyph of the layout as an image instance. */
static pc_status collect_color(pc_text *t, const pc_paint_src *src, bool aa, crender *r)
{
    pc_status st = PC_OK;
    if (!t->n_faces) return PC_OK;
    if (t->cimg_bytes > CIMG_BUDGET || t->n_cimg >= CIMG_MAX_ENTRIES) cimg_clear(t);
    for (size_t i = 0; i < t->n_glyphs && st == PC_OK; i++) {
        const pc_text_glyph *g = &t->glyphs[i];
        pc_text_gent *e;
        pc_text_cimg key;
        const pc_text_cimg *img;
        double fx, fy, px, py;
        int32_t ix, iy;
        uint8_t kind;
        bool fg;
        if (g->hidden || !isfinite(g->x) || !isfinite(g->y)) continue;
        st = glyph_entry(t, g->face, g->gid, &e);
        if (st != PC_OK) break;
        if (e->kind == PC_GK_MONO) continue;
        kind = e->kind;
        fg = e->fg;
        px = g->x < -COORD_CLAMP ? -COORD_CLAMP : (g->x > COORD_CLAMP ? COORD_CLAMP : g->x);
        py = g->y < -COORD_CLAMP ? -COORD_CLAMP : (g->y > COORD_CLAMP ? COORD_CLAMP : g->y);
        fx = floor(px);
        fy = floor(py);
        memset(&key, 0, sizeof key);
        key.face = g->face;
        key.gid = g->gid;
        key.qx = (uint8_t)floor((px - fx) * 4.0 + 0.5);
        key.qy = (uint8_t)floor((py - fy) * 4.0 + 0.5);
        if (key.qx >= 4u) { key.qx = 0u; fx += 1.0; }
        if (key.qy >= 4u) { key.qy = 0u; fy += 1.0; }
        ix = clamp_i32(fx);
        iy = clamp_i32(fy);
        key.aa = kind == PC_GK_LAYERS ? aa : true;
        if (kind == PC_GK_LAYERS && fg) key.fg = fg_color(src, ix, iy);
        img = cimg_find(t, &key);
        if (!img) st = cimg_make(t, r, &key, kind, &img);
        if (st != PC_OK || !img || !img->px) continue;
        if (r->n == r->cap) {
            size_t nc = r->cap ? r->cap * 2u : 16u, bytes;
            cinst *nv;
            if (!pc_mul_size(nc, sizeof *nv, &bytes)) {
                st = PC_ERR_LIMIT;
                break;
            }
            nv = (cinst *)realloc(r->v, bytes);
            if (!nv) {
                st = PC_ERR_NOMEM;
                break;
            }
            r->v = nv;
            r->cap = nc;
        }
        r->v[r->n].img = img;
        r->v[r->n].x = clamp_i32((double)ix + (double)img->ox);
        r->v[r->n].y = clamp_i32((double)iy + (double)img->oy);
        r->v[r->n].order = r->n;
        if (img->dh > r->maxh) r->maxh = img->dh;
        r->n++;
    }
    if (st == PC_OK && r->n > 1u) qsort(r->v, r->n, sizeof *r->v, cinst_cmp);
    return st;
}

/* Straight-alpha sample of an image at document offset (dx, dy) from its
 * top-left (inside its extent). */
static pc_px32 img_sample(const pc_text_cimg *c, int32_t dx, int32_t dy)
{
    pc_px32 o;
    double px, py, fx, fy, acc[4] = { 0.0, 0.0, 0.0, 0.0 };
    int64_t ix, iy;
    if (c->k == 1.0) {
        if (dx < c->w && dy < c->h) return c->px[(size_t)dy * (size_t)c->w + (size_t)dx];
        memset(&o, 0, sizeof o);
        return o;
    }
    px = ((double)dx + 0.5) / c->k - 0.5;
    py = ((double)dy + 0.5) / c->k - 0.5;
    ix = (int64_t)floor(px);
    iy = (int64_t)floor(py);
    fx = px - (double)ix;
    fy = py - (double)iy;
    for (int k = 0; k < 4; k++) {
        int64_t x = ix + (k & 1), y = iy + (k >> 1);
        double w = ((k & 1) ? fx : 1.0 - fx) * ((k >> 1) ? fy : 1.0 - fy), a;
        const pc_px32 *p;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= c->w) x = c->w - 1;
        if (y >= c->h) y = c->h - 1;
        p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
        a = (double)p->a * w;
        acc[0] += (double)p->b * a;
        acc[1] += (double)p->g * a;
        acc[2] += (double)p->r * a;
        acc[3] += a;
    }
    memset(&o, 0, sizeof o);
    o.a = to_u8(acc[3]);
    if (o.a) {
        o.b = to_u8(acc[0] / acc[3]);
        o.g = to_u8(acc[1] / acc[3]);
        o.r = to_u8(acc[2] / acc[3]);
    }
    return o;
}

/* s over d, straight alpha. */
static pc_px32 over(pc_px32 d, pc_px32 s)
{
    double sa, da, oa;
    pc_px32 o;
    if (s.a == 255u || d.a == 0u) return s;
    if (s.a == 0u) return d;
    sa = (double)s.a / 255.0;
    da = (double)d.a / 255.0 * (1.0 - sa);
    oa = sa + da;
    o.a = to_u8(oa * 255.0);
    o.b = to_u8(((double)s.b * sa + (double)d.b * da) / oa);
    o.g = to_u8(((double)s.g * sa + (double)d.g * da) / oa);
    o.r = to_u8(((double)s.r * sa + (double)d.r * da) / oa);
    return o;
}

static void paint_inst(const cinst *in, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const pc_text_cimg *c = in->img;
    int64_t a = in->x > x ? in->x : x;
    int64_t b = (int64_t)in->x + c->dw < (int64_t)x + n ? (int64_t)in->x + c->dw
                                                        : (int64_t)x + n;
    int32_t dy = (int32_t)((int64_t)y - in->y);
    for (int64_t px = a; px < b; px++) {
        pc_px32 s = img_sample(c, (int32_t)(px - in->x), dy);
        if (s.a) out[px - x] = over(out[px - x], s);
    }
}

/* pc_vimage row: the instances crossing row y, composited in glyph order.
 * Pure: reads the immutable instance list and images (par workers). */
static void color_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const crender *r = (const crender *)ud;
    size_t lo = 0, hi = r->n, cand[ROW_CANDIDATES], nc = 0;
    bool overflow = false;
    memset(out, 0, (size_t)n * sizeof *out);
    /* first instance with v.y > y */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (r->v[mid].y <= y) lo = mid + 1u;
        else hi = mid;
    }
    for (size_t i = lo; i-- > 0u;) {
        const cinst *in = &r->v[i];
        if ((int64_t)in->y + r->maxh <= (int64_t)y) break;
        if ((int64_t)in->y + in->img->dh <= (int64_t)y) continue;
        if ((int64_t)in->x >= (int64_t)x + n || (int64_t)in->x + in->img->dw <= (int64_t)x)
            continue;
        if (nc < ROW_CANDIDATES) cand[nc++] = i;
        else overflow = true;
    }
    if (overflow) {
        /* very dense rows: y order instead of glyph order */
        for (size_t i = 0; i < lo; i++) {
            const cinst *in = &r->v[i];
            if ((int64_t)in->y + in->img->dh <= (int64_t)y) continue;
            paint_inst(in, x, y, n, out);
        }
        return;
    }
    /* glyph order: insertion sort of the few candidates */
    for (size_t i = 1; i < nc; i++) {
        size_t v = cand[i], j = i;
        while (j > 0u && r->v[cand[j - 1u]].order > r->v[v].order) {
            cand[j] = cand[j - 1u];
            j--;
        }
        cand[j] = v;
    }
    for (size_t i = 0; i < nc; i++) paint_inst(&r->v[cand[i]], x, y, n, out);
}

pc_status pc_text_render(pc_text *t, pc_vrender *vr, pc_txn *tx, uint32_t layer_id,
                         const pc_paint_src *src, const pc_vdraw_opts *o, const pc_par *par,
                         pc_rect *dirty)
{
    pc_vlayer l;
    pc_vimage img;
    crender cr;
    pc_status st;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!t || !vr || !tx || !o) return PC_ERR_ARG;
    pc_poly_clear(&t->scratch);
    st = pc_text_build(t, &t->scratch);
    if (st != PC_OK) return st;
    memset(&cr, 0, sizeof cr);
    st = collect_color(t, src, o->antialias, &cr);
    if (st != PC_OK) {
        crender_free(&cr);
        return st;
    }
    if (!t->scratch.n_pts && !cr.n) {
        crender_free(&cr);
        return pc_vrender_clear(vr, tx, dirty);
    }
    l.fill = &t->scratch;
    l.rule = PC_FILL_NONZERO;
    l.thin = NULL;
    l.src = src;
    memset(&img, 0, sizeof img);
    if (cr.n) {
        int64_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (size_t i = 0; i < cr.n; i++) {
            const cinst *in = &cr.v[i];
            if (in->x < x0) x0 = in->x;
            if (in->y < y0) y0 = in->y;
            if ((int64_t)in->x + in->img->dw > x1) x1 = (int64_t)in->x + in->img->dw;
            if ((int64_t)in->y + in->img->dh > y1) y1 = (int64_t)in->y + in->img->dh;
        }
        if (x1 - x0 > INT32_MAX) x1 = x0 + INT32_MAX;
        if (y1 - y0 > INT32_MAX) y1 = y0 + INT32_MAX;
        img.row = color_row;
        img.ud = &cr;
        img.bounds = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0),
                                  (int32_t)(y1 - y0));
    }
    st = pc_vrender_draw_image(vr, tx, layer_id, t->scratch.n_pts ? &l : NULL,
                               t->scratch.n_pts ? 1u : 0u, cr.n ? &img : NULL, o, par, dirty);
    crender_free(&cr);
    return st;
}
