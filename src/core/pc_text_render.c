/* pc_text_render.c - glyph outline cache, synthetic italic and bold,
 * underline and strikeout geometry, and rendering of pc_text through
 * pc_vrender (lane E3). */
#include "pc_text_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_TOL 0.02           /* flattening tolerance, pixels */
#define CACHE_MAX 4096u          /* entries before the cache is flushed */

void pc_text_cache_clear(pc_text *t)
{
    for (size_t i = 0; i < t->cache_cap; i++)
        if (t->cache[i].used) {
            pc_poly_free(&t->cache[i].poly);
            t->cache[i].used = false;
        }
    t->cache_n = 0;
}

void pc_text_cache_free(pc_text *t)
{
    pc_text_cache_clear(t);
    free(t->cache);
    t->cache = NULL;
    t->cache_cap = 0;
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
    if (t->cache_n >= CACHE_MAX) pc_text_cache_clear(t);
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

/* Coverage polygon of one glyph at pen (0, 0): outline, synthetic slant,
 * outer contours turned counter-clockwise on screen (the orientation of
 * every stroke ring, so the emboldening stroke unions under nonzero), and
 * the emboldening stroke. */
static pc_status build_glyph(const pc_text *t, const pc_font_face *f, uint32_t gid, pc_poly *dst)
{
    pc_path path;
    pc_status st = PC_OK;
    if (!f->outline) return PC_OK;
    pc_path_init(&path);
    st = f->outline(f->ud, gid, t->em, t->style.mode, &path);
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

static pc_status glyph_poly(pc_text *t, uint32_t face, uint32_t gid, const pc_poly **out)
{
    pc_text_gent *e = cache_find(t, face, gid);
    pc_status st;
    size_t i;
    if (e) {
        *out = &e->poly;
        return PC_OK;
    }
    st = cache_reserve(t);
    if (st != PC_OK) return st;
    i = slot_of(face, gid, t->cache_cap);
    while (t->cache[i].used) i = (i + 1u) & (t->cache_cap - 1u);
    e = &t->cache[i];
    pc_poly_init(&e->poly);
    st = build_glyph(t, t->faces[face], gid, &e->poly);
    if (st != PC_OK) {
        pc_poly_free(&e->poly);
        return st;
    }
    e->face = face;
    e->gid = gid;
    e->used = true;
    t->cache_n++;
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
            const pc_poly *gp;
            st = glyph_poly(t, g->face, g->gid, &gp);
            if (st == PC_OK && gp->n_pts) {
                pc_affine m = pc_affine_translate(g->x, g->y);
                st = pc_poly_append(out, gp, &m);
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

pc_status pc_text_render(pc_text *t, pc_vrender *vr, pc_txn *tx, uint32_t layer_id,
                         const pc_paint_src *src, const pc_vdraw_opts *o, const pc_par *par,
                         pc_rect *dirty)
{
    pc_vlayer l;
    pc_status st;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!t || !vr || !tx || !o) return PC_ERR_ARG;
    pc_poly_clear(&t->scratch);
    st = pc_text_build(t, &t->scratch);
    if (st != PC_OK) return st;
    if (!t->scratch.n_pts) return pc_vrender_clear(vr, tx, dirty);
    l.fill = &t->scratch;
    l.rule = PC_FILL_NONZERO;
    l.thin = NULL;
    l.src = src;
    return pc_vrender_draw(vr, tx, layer_id, &l, 1, o, par, dirty);
}
