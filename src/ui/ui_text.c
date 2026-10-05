/* ui_text.c - multi-line text layout and A8 rasterization for the Text
 * tool. Uses only the pure (cache-free) font functions, so it is safe on any
 * thread while the faces stay alive. */
#include "ui_font_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ITALIC_SHEAR 0.21255656f   /* tan(12 degrees) */

typedef struct tl_ctx {
    const ui_font      *f;
    const ui_text_style *st;
    float               size, line_h, line_adv, ascent, bold_w;
    bool                kern;
} tl_ctx;

static pc_status tl_init(tl_ctx *c, const ui_font *f, const ui_text_style *st)
{
    ui_font_metrics m;
    float spacing;
    if (!f || !st || !(st->size_px > 0.0f) || st->size_px > UI_FONT_MAX_SIZE) return PC_ERR_ARG;
    memset(c, 0, sizeof *c);
    c->f = f;
    c->st = st;
    c->size = st->size_px;
    ui_font_get_metrics(f, c->size, &m);
    spacing = st->line_spacing > 0.0f ? st->line_spacing : 1.0f;
    c->line_h = m.line_height;
    c->line_adv = m.line_height * spacing;
    c->ascent = m.ascent;
    c->bold_w = (st->flags & UI_TEXT_BOLD) ? c->size / 30.0f : 0.0f;
    c->kern = !(st->flags & UI_TEXT_NO_KERN);
    return PC_OK;
}

/* Visit the glyphs of one line [b, e). fn may be NULL (measuring). Returns
 * the line width. stop_at (byte offset or SIZE_MAX) returns the pen x
 * before that byte through *stop_x. */
typedef void (*glyph_fn)(void *ud, const ui_font *face, uint32_t gid, float x, size_t byte);

static float tl_line(const tl_ctx *c, const char *s, size_t b, size_t e, glyph_fn fn,
                     void *ud, size_t stop_at, float *stop_x)
{
    const ui_font *prev_face = NULL;
    uint32_t prev = 0;
    float x = 0.0f;
    size_t i = b;
    while (i < e) {
        const ui_font *face;
        uint32_t gid, cp;
        size_t at = i;
        float sc, adv;
        if (at == stop_at && stop_x) *stop_x = x;
        cp = ui_utf8_decode(s, e, &i);
        if (cp < 0x20u || cp == 0x7Fu) continue;
        gid = ui_font_resolve(c->f, cp, &face);
        sc = c->size / face->upem;
        if (c->kern && face == prev_face && prev && gid)
            x += (float)ui_kern_lookup(face, prev, gid) * sc;
        if (fn) fn(ud, face, gid, x, at);
        adv = gid ? (float)ui_font_advance(face, gid) * sc : c->size * 0.6f;
        x += adv + c->bold_w;
        prev_face = face;
        prev = gid;
    }
    if (stop_at == e && stop_x) *stop_x = x;
    return x;
}

static size_t line_end(const char *s, size_t len, size_t b)
{
    while (b < len && s[b] != '\n') b++;
    return b;
}

static float layout_width(const tl_ctx *c, const char *s, size_t len, int *lines)
{
    float w = 0.0f;
    size_t b = 0;
    int n = 0;
    for (;;) {
        size_t e = line_end(s, len, b);
        float lw = tl_line(c, s, b, e, NULL, NULL, SIZE_MAX, NULL);
        if (lw > w) w = lw;
        n++;
        if (e >= len) break;
        b = e + 1;
    }
    *lines = n;
    return w;
}

static float align_off(const tl_ctx *c, float box_w, float line_w)
{
    if (c->st->align == UI_ALIGN_CENTER) return floorf((box_w - line_w) * 0.5f);
    if (c->st->align == UI_ALIGN_RIGHT) return box_w - line_w;
    return 0.0f;
}

pc_status ui_text_layout(const ui_font *f, const char *s, size_t len, const ui_text_style *st,
                         ui_text_box *box)
{
    tl_ctx c;
    pc_status r = tl_init(&c, f, st);
    if (r != PC_OK) return r;
    if (!s) len = 0;
    box->w = layout_width(&c, s ? s : "", len, &box->lines);
    box->h = (float)box->lines * c.line_adv;
    box->ascent = c.ascent;
    box->line_advance = c.line_adv;
    return PC_OK;
}

typedef struct raster_ud {
    const tl_ctx *c;
    ui_path      *p;
    float         ox, base;
} raster_ud;

static void add_glyph(void *ud, const ui_font *face, uint32_t gid, float x, size_t byte)
{
    raster_ud *r = (raster_ud *)ud;
    ui_ymap ym;
    float sc = r->c->size / face->upem;
    int32_t c0 = r->p->nc;
    float area = 0.0f;
    (void)byte;
    ui_ymap_init(&ym, face, sc, false);
    if (!gid) {
        /* missing glyph: an outlined box of the x-height width */
        float w = r->c->size * 0.5f, h = r->c->size * 0.7f, t = r->c->size * 0.06f;
        float bx = r->ox + x + r->c->size * 0.05f, by = r->base - h;
        if (t < 1.0f) t = 1.0f;
        ui_path_rect(r->p, bx, by, w, h);
        ui_path_move(r->p, bx + t, by + t);
        ui_path_line(r->p, bx + t, by + h - t);
        ui_path_line(r->p, bx + w - t, by + h - t);
        ui_path_line(r->p, bx + w - t, by + t);
        ui_path_close(r->p);
        return;
    }
    if (!ui_font_glyph_path(face, gid, sc, &ym, r->ox + x, r->base,
                            (r->c->st->flags & UI_TEXT_ITALIC) ? ITALIC_SHEAR : 0.0f, r->p))
        return;
    for (int32_t k = c0; k < r->p->nc; k++) area += ui_path_contour_area(r->p, k);
    if (area < 0.0f)
        for (int32_t k = c0; k < r->p->nc; k++) ui_path_reverse_contour(r->p, k);
}

static void add_line_rect(ui_path *p, float x, float y, float w, float h, bool aa)
{
    if (!aa) {
        y = roundf(y);
        h = roundf(h);
        x = roundf(x);
        w = roundf(w);
    }
    if (h < 1.0f) h = 1.0f;
    if (w > 0.0f) ui_path_rect(p, x, y, w, h);
}

pc_status ui_text_raster(const ui_font *f, const char *s, size_t len, const ui_text_style *st,
                         ui_a8 *out)
{
    tl_ctx c;
    ui_path p, stroke;
    float box_w, x0, y0, x1, y1;
    int lines;
    size_t b = 0, npx;
    int32_t bx, by, bw, bh;
    pc_status r;
    bool aa;
    if (!out) return PC_ERR_ARG;
    memset(out, 0, sizeof *out);
    r = tl_init(&c, f, st);
    if (r != PC_OK) return r;
    if (!s) len = 0;
    aa = (st->flags & UI_TEXT_AA) != 0;
    box_w = layout_width(&c, s ? s : "", len, &lines);
    {
        /* reject oversized output before building any outline (P-08) */
        double ew = (double)box_w + 4.0 * (double)c.size, eh = (double)lines * (double)c.line_adv +
                                                                2.0 * (double)c.size;
        if (ew * eh > (double)UI_TEXT_MAX_PIXELS) return PC_ERR_LIMIT;
    }
    ui_path_init(&p, 0.2f);
    for (int li = 0; li < lines; li++) {
        size_t e = line_end(s, len, b);
        raster_ud ud;
        float lw = tl_line(&c, s, b, e, NULL, NULL, SIZE_MAX, NULL);
        ud.c = &c;
        ud.p = &p;
        ud.ox = align_off(&c, box_w, lw);
        ud.base = c.ascent + (float)li * c.line_adv;
        (void)tl_line(&c, s, b, e, add_glyph, &ud, SIZE_MAX, NULL);
        if (st->flags & (UI_TEXT_UNDERLINE | UI_TEXT_STRIKE)) {
            ui_font_metrics m;
            ui_font_get_metrics(f, c.size, &m);
            if (st->flags & UI_TEXT_UNDERLINE)
                add_line_rect(&p, ud.ox, ud.base + m.underline_pos - m.underline_size * 0.5f,
                              lw, m.underline_size, aa);
            if (st->flags & UI_TEXT_STRIKE)
                add_line_rect(&p, ud.ox, ud.base - m.strike_pos, lw, m.strike_size, aa);
        }
        b = e + 1;
    }
    if (c.bold_w > 0.0f && p.n > 0) {
        ui_path_init(&stroke, 0.2f);
        ui_path_stroke(&p, c.bold_w, UI_JOIN_ROUND, UI_CAP_ROUND, 4.0f, &stroke);
        /* merge the stroke contours into p */
        for (int32_t k = 0; k < stroke.nc; k++) {
            int32_t s0 = k == 0 ? 0 : stroke.ends[k - 1], s1 = stroke.ends[k];
            ui_path_move(&p, stroke.xy[2 * s0], stroke.xy[2 * s0 + 1]);
            for (int32_t i = s0 + 1; i < s1; i++)
                ui_path_line(&p, stroke.xy[2 * i], stroke.xy[2 * i + 1]);
            ui_path_close(&p);
        }
        if (stroke.oom) p.oom = true;
        ui_path_free(&stroke);
    }
    if (p.oom) { ui_path_free(&p); return PC_ERR_NOMEM; }
    if (p.n > 0) {
        ui_path_bounds(&p, &x0, &y0, &x1, &y1);
    } else {
        x0 = y0 = 0.0f;
        x1 = y1 = 1.0f;
    }
    if (!(x0 > -1e7f) || !(y0 > -1e7f) || !(x1 < 1e7f) || !(y1 < 1e7f)) {
        ui_path_free(&p);
        return PC_ERR_LIMIT;
    }
    bx = (int32_t)floorf(x0);
    by = (int32_t)floorf(y0);
    bw = (int32_t)ceilf(x1) - bx;
    bh = (int32_t)ceilf(y1) - by;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    if (!pc_mul_size((size_t)bw, (size_t)bh, &npx) || npx > UI_TEXT_MAX_PIXELS) {
        ui_path_free(&p);
        return PC_ERR_LIMIT;
    }
    out->px = (uint8_t *)calloc(npx, 1u);
    if (!out->px) { ui_path_free(&p); return PC_ERR_NOMEM; }
    for (int32_t i = 0; i < p.n; i++) {
        p.xy[2 * i] -= (float)bx;
        p.xy[2 * i + 1] -= (float)by;
    }
    r = ui_raster_fill(&p, UI_FILL_NONZERO, aa, out->px, bw, bh, bw, UI_RASTER_SET);
    ui_path_free(&p);
    if (r != PC_OK) { free(out->px); memset(out, 0, sizeof *out); return r; }
    out->w = bw;
    out->h = bh;
    out->stride = bw;
    out->x = bx;
    out->y = by;
    return PC_OK;
}

void ui_a8_free(ui_a8 *a)
{
    if (!a) return;
    free(a->px);
    memset(a, 0, sizeof *a);
}

pc_status ui_text_caret(const ui_font *f, const char *s, size_t len, const ui_text_style *st,
                        size_t pos, float *x, float *y, float *h)
{
    tl_ctx c;
    float box_w, cx = 0.0f;
    int lines, li = 0;
    size_t b = 0, e;
    pc_status r = tl_init(&c, f, st);
    if (r != PC_OK) return r;
    if (!s) len = 0;
    if (pos > len) pos = len;
    box_w = layout_width(&c, s ? s : "", len, &lines);
    for (;;) {
        e = line_end(s, len, b);
        if (pos <= e || e >= len) break;
        b = e + 1;
        li++;
    }
    {
        float lw = tl_line(&c, s, b, e, NULL, NULL, pos, &cx);
        cx += align_off(&c, box_w, lw);
    }
    if (x) *x = cx;
    if (y) *y = (float)li * c.line_adv;
    if (h) *h = c.line_h;
    return PC_OK;
}

size_t ui_text_hit_point(const ui_font *f, const char *s, size_t len, const ui_text_style *st,
                         float x, float y)
{
    tl_ctx c;
    float box_w;
    int lines, li;
    size_t b = 0, e, best = 0;
    if (tl_init(&c, f, st) != PC_OK || !s) return 0;
    box_w = layout_width(&c, s, len, &lines);
    li = c.line_adv > 0.0f ? (int)floorf(y / c.line_adv) : 0;
    if (li < 0) li = 0;
    if (li >= lines) li = lines - 1;
    for (int k = 0; k < li; k++) b = line_end(s, len, b) + 1;
    e = line_end(s, len, b);
    {
        float lw = tl_line(&c, s, b, e, NULL, NULL, SIZE_MAX, NULL);
        float off = align_off(&c, box_w, lw), best_d = 1e30f;
        size_t i = b;
        for (;;) {
            float px = 0.0f, d;
            (void)tl_line(&c, s, b, e, NULL, NULL, i, &px);
            d = fabsf(px + off - x);
            if (d < best_d) { best_d = d; best = i; }
            if (i >= e) break;
            i = ui_utf8_next(s, e, i);
        }
    }
    return best;
}
