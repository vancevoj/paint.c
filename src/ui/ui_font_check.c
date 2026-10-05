/* ui_font_check.c - font file validation (lane L3).
 *
 * stb_truetype reads TrueType glyph data with unchecked offsets, so a face
 * is validated before any glyph reaches it:
 *  - the TTC header and table directory: every table used lies in the file;
 *  - head, hhea, maxp sizes and values (units per em, loca format);
 *  - loca: every entry pair is ordered and inside glyf;
 *  - every glyf record, mirroring how stb parses it: contour end points
 *    strictly increasing, instructions, flag runs and coordinate bytes
 *    inside the record, contours starting off-curve have a second point,
 *    composite records inside the glyph with x/y offsets (point matching
 *    is rejected);
 *  - the composite graph, walked iteratively (P-07): cycles, nesting deeper
 *    than UI_GLYPH_MAX_DEPTH and expansions above UI_GLYPH_MAX_POINTS points
 *    make the glyph and every glyph that uses it invalid.
 * Invalid glyphs render as empty. CFF data is handed to stb with a buffer
 * bounded by the real table length (ui_stb.c), where its reader checks
 * every access. cmap, hmtx, kern, GPOS, OS/2, post and name are read by our
 * own bounds-checked code (ui_rd16 and friends).
 */
#include "ui_font_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UI_GLYPH_MAX_DEPTH 12
#define UI_GLYPH_MAX_POINTS 200000u
#define UI_GLYPH_MAX_COMPONENTS 255u

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

pc_status ui_sfnt_locate(ui_sfnt *s, const uint8_t *d, size_t n, int face)
{
    uint32_t start = 0, ver, ntab;
    memset(s, 0, sizeof *s);
    if (!d || n < 12u || face < 0) return d && n >= 12u ? PC_ERR_ARG : PC_ERR_FORMAT;
    if (n > UI_FONT_MAX_FILE) return PC_ERR_LIMIT;
    if (memcmp(d, "ttcf", 4) == 0) {
        uint32_t cv = be32(d + 4), nf = be32(d + 8);
        if (cv != 0x00010000u && cv != 0x00020000u) return PC_ERR_FORMAT;
        if ((uint32_t)face >= nf) return PC_ERR_ARG;
        if ((uint64_t)12u + 4u * ((uint64_t)face + 1u) > n) return PC_ERR_FORMAT;
        start = be32(d + 12 + 4u * (uint32_t)face);
    } else if (face != 0) {
        return PC_ERR_ARG;
    }
    if ((uint64_t)start + 12u > n) return PC_ERR_FORMAT;
    ver = be32(d + start);
    if (memcmp(d + start, "typ1", 4) == 0) return PC_ERR_UNSUPPORTED;
    if (ver != 0x00010000u && memcmp(d + start, "OTTO", 4) != 0 &&
        memcmp(d + start, "true", 4) != 0)
        return PC_ERR_FORMAT;
    ntab = be16(d + start + 4);
    if ((uint64_t)start + 12u + 16u * (uint64_t)ntab > n) return PC_ERR_FORMAT;
    s->d = d;
    s->n = n;
    s->start = start;
    for (uint32_t i = 0; i < ntab; i++) {
        const uint8_t *rec = d + start + 12u + 16u * i;
        uint32_t off = be32(rec + 8), len = be32(rec + 12);
        ui_tbl *t = NULL;
        if ((uint64_t)off + len > n) continue;          /* unusable table */
        if (memcmp(rec, "cmap", 4) == 0) t = &s->cmap;
        else if (memcmp(rec, "head", 4) == 0) t = &s->head;
        else if (memcmp(rec, "hhea", 4) == 0) t = &s->hhea;
        else if (memcmp(rec, "hmtx", 4) == 0) t = &s->hmtx;
        else if (memcmp(rec, "maxp", 4) == 0) t = &s->maxp;
        else if (memcmp(rec, "loca", 4) == 0) t = &s->loca;
        else if (memcmp(rec, "glyf", 4) == 0) t = &s->glyf;
        else if (memcmp(rec, "CFF ", 4) == 0) t = &s->cff;
        else if (memcmp(rec, "kern", 4) == 0) t = &s->kern;
        else if (memcmp(rec, "GPOS", 4) == 0) t = &s->gpos;
        else if (memcmp(rec, "OS/2", 4) == 0) t = &s->os2;
        else if (memcmp(rec, "post", 4) == 0) t = &s->post;
        else if (memcmp(rec, "name", 4) == 0) t = &s->name;
        if (t && t->off == 0 && off != 0) { t->off = off; t->len = len; }
    }
    return PC_OK;
}

/* ---- names --------------------------------------------------------------- */
static size_t put_cp(char *out, size_t cap, size_t k, uint32_t cp)
{
    char b[4];
    int m = ui_utf8_encode(cp, b);
    if (k + (size_t)m >= cap) return k;
    memcpy(out + k, b, (size_t)m);
    return k + (size_t)m;
}

bool ui_sfnt_name(const ui_sfnt *s, uint32_t name_id, char *out, size_t cap)
{
    ui_tbl t = s->name;
    uint32_t count, sbase, best = UINT32_MAX;
    int best_score = -1;
    if (!cap) return false;
    out[0] = '\0';
    if (t.len < 6u) return false;
    count = ui_rd16(s, t, 2);
    sbase = ui_rd16(s, t, 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = 6u + 12u * i, pid, eid, lang;
        int score = -1;
        if (!ui_tbl_has(t, r, 12u)) break;
        if (ui_rd16(s, t, r + 6) != name_id) continue;
        pid = ui_rd16(s, t, r); eid = ui_rd16(s, t, r + 2); lang = ui_rd16(s, t, r + 4);
        if (pid == 3 && (eid == 1 || eid == 10)) score = lang == 0x409 ? 5 : 4;
        else if (pid == 3 && eid == 0) score = 3;
        else if (pid == 0) score = 2;
        else if (pid == 1 && eid == 0) score = lang == 0 ? 1 : 0;
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best == UINT32_MAX) return false;
    {
        uint32_t r = 6u + 12u * best, pid = ui_rd16(s, t, r);
        uint32_t len = ui_rd16(s, t, r + 8), off = sbase + ui_rd16(s, t, r + 10);
        size_t k = 0;
        if (!ui_tbl_has(t, off, len)) return false;
        if (pid == 1) {
            for (uint32_t i = 0; i < len; i++) {
                uint32_t c = ui_rd8(s, t, off + i);
                k = put_cp(out, cap, k, c < 128u ? c : (uint32_t)'?');
            }
        } else {
            for (uint32_t i = 0; i + 1 < len; i += 2) {
                uint32_t c = ui_rd16(s, t, off + i);
                if (c >= 0xD800u && c < 0xDC00u && i + 3 < len) {
                    uint32_t lo = ui_rd16(s, t, off + i + 2);
                    if (lo >= 0xDC00u && lo < 0xE000u) {
                        c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 2;
                    } else {
                        c = UI_UTF8_REPLACEMENT;
                    }
                } else if (c >= 0xD800u && c < 0xE000u) {
                    c = UI_UTF8_REPLACEMENT;
                }
                if (c == 0) break;
                k = put_cp(out, cap, k, c);
            }
        }
        out[k] = '\0';
        return k > 0;
    }
}

/* ---- glyf validation ----------------------------------------------------- */
typedef struct gxform { int16_t a, b, c, d, e, f; } gxform;   /* F2Dot14 matrix, offsets */
typedef struct gbox { int32_t x0, y0, x1, y1; bool empty; } gbox;

typedef struct gcheck {
    uint32_t *comp_start;    /* num_glyphs + 1 */
    uint16_t *comps;         /* component glyph ids */
    gxform   *xf;            /* per component */
    uint32_t  ncomps, ccap;
    uint8_t  *own_ok;        /* record parsed fine */
    uint32_t *points;        /* points of simple glyphs, expanded points later */
    gbox     *box;           /* outline bounds as stb will produce them */
} gcheck;

static bool glyph_span(const ui_font *f, uint32_t g, uint32_t *off, uint32_t *len)
{
    const ui_sfnt *s = &f->s;
    uint32_t a, b;
    if (f->info.indexToLocFormat == 0) {
        a = ui_rd16(s, s->loca, 2u * g) * 2u;
        b = ui_rd16(s, s->loca, 2u * g + 2u) * 2u;
    } else {
        a = ui_rd32(s, s->loca, 4u * g);
        b = ui_rd32(s, s->loca, 4u * g + 4u);
    }
    if (a > b || b > s->glyf.len) return false;
    *off = a;
    *len = b - a;
    return true;
}

static bool add_comp(gcheck *gc, uint32_t gid, const gxform *x)
{
    if (gc->ncomps == gc->ccap) {
        uint32_t ncap = gc->ccap ? gc->ccap * 2u : 256u;
        uint16_t *n = (uint16_t *)realloc(gc->comps, (size_t)ncap * sizeof(uint16_t));
        gxform *nx;
        if (!n) return false;
        gc->comps = n;
        nx = (gxform *)realloc(gc->xf, (size_t)ncap * sizeof(gxform));
        if (!nx) return false;
        gc->xf = nx;
        gc->ccap = ncap;
    }
    gc->xf[gc->ncomps] = *x;
    gc->comps[gc->ncomps++] = (uint16_t)gid;
    return true;
}

static void box_add(gbox *b, int32_t x, int32_t y)
{
    if (b->empty) { b->x0 = b->x1 = x; b->y0 = b->y1 = y; b->empty = false; return; }
    if (x < b->x0) b->x0 = x;
    if (x > b->x1) b->x1 = x;
    if (y < b->y0) b->y0 = y;
    if (y > b->y1) b->y1 = y;
}

/* Decode the coordinates of a validated simple glyph the way stb does
 * (int32 accumulation stored as int16) to get the bounds it will emit. */
static void simple_box(const ui_sfnt *s, ui_tbl t, uint32_t nc, uint32_t n, uint32_t fpos,
                       gbox *box)
{
    uint32_t xpos, ypos, fp, flags = 0, cnt = 0, xb = 0;
    int32_t x = 0, y = 0;
    int16_t *xs = NULL;
    box->empty = true;
    (void)nc;
    /* pass 1: length of the flag run, to find the coordinate arrays */
    fp = fpos;
    for (uint32_t i = 0; i < n; i++) {
        if (cnt == 0) { flags = ui_rd8(s, t, fp++); if (flags & 8u) cnt = ui_rd8(s, t, fp++); }
        else cnt--;
        xb += (flags & 2u) ? 1u : ((flags & 16u) ? 0u : 2u);
    }
    xpos = fp;
    ypos = fp + xb;
    xs = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    if (!xs) { box->empty = false; box->x0 = box->y0 = -32768; box->x1 = box->y1 = 32767; return; }
    fp = fpos; cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cnt == 0) { flags = ui_rd8(s, t, fp++); if (flags & 8u) cnt = ui_rd8(s, t, fp++); }
        else cnt--;
        if (flags & 2u) {
            int32_t d = (int32_t)ui_rd8(s, t, xpos++);
            x += (flags & 16u) ? d : -d;
        } else if (!(flags & 16u)) {
            x += ui_rds16(s, t, xpos);
            xpos += 2u;
        }
        xs[i] = (int16_t)(uint16_t)(uint32_t)x;
    }
    fp = fpos; cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cnt == 0) { flags = ui_rd8(s, t, fp++); if (flags & 8u) cnt = ui_rd8(s, t, fp++); }
        else cnt--;
        if (flags & 4u) {
            int32_t d = (int32_t)ui_rd8(s, t, ypos++);
            y += (flags & 32u) ? d : -d;
        } else if (!(flags & 32u)) {
            y += ui_rds16(s, t, ypos);
            ypos += 2u;
        }
        box_add(box, xs[i], (int16_t)(uint16_t)(uint32_t)y);
    }
    free(xs);
}

/* Parse one glyf record the way stb does. Returns false on OOM only;
 * *ok tells whether the record itself is acceptable. */
static bool check_record(const ui_font *f, gcheck *gc, uint32_t g, bool *ok)
{
    const ui_sfnt *s = &f->s;
    ui_tbl t;
    uint32_t off, len;
    int32_t nc;
    *ok = false;
    gc->points[g] = 0;
    gc->box[g].empty = true;
    if (!glyph_span(f, g, &off, &len)) return true;
    if (len == 0) { *ok = true; return true; }
    if (len < 10u) return true;
    t.off = s->glyf.off + off;
    t.len = len;
    nc = ui_rds16(s, t, 0);
    if (nc > 0) {
        uint32_t pos, fpos, n, ins, prev = 0, flags = 0, flagcount = 0, xb = 0, yb = 0, j = 0;
        uint32_t next_start = 0;
        if (!ui_tbl_has(t, 10u, 2u * (uint32_t)nc + 2u)) return true;
        for (int32_t c = 0; c < nc; c++) {
            uint32_t e = ui_rd16(s, t, 10u + 2u * (uint32_t)c);
            if (c > 0 && e <= prev) return true;     /* strictly increasing */
            prev = e;
        }
        n = prev + 1u;
        ins = ui_rd16(s, t, 10u + 2u * (uint32_t)nc);
        pos = 12u + 2u * (uint32_t)nc + ins;
        fpos = pos;
        if (pos > len) return true;
        for (uint32_t i = 0; i < n; i++) {
            if (flagcount == 0) {
                if (pos >= len) return true;
                flags = ui_rd8(s, t, pos++);
                if (flags & 8u) {
                    if (pos >= len) return true;
                    flagcount = ui_rd8(s, t, pos++);
                }
            } else {
                flagcount--;
            }
            if (i == next_start) {
                uint32_t end = ui_rd16(s, t, 10u + 2u * j);
                /* stb peeks at point i + 1 when a contour starts off-curve */
                if (!(flags & 1u) && end <= i) return true;
                next_start = end + 1u;
                j++;
            }
            xb += (flags & 2u) ? 1u : ((flags & 16u) ? 0u : 2u);
            yb += (flags & 4u) ? 1u : ((flags & 32u) ? 0u : 2u);
        }
        if ((uint64_t)pos + xb + yb > len) return true;
        gc->points[g] = n + 2u * (uint32_t)nc;
        simple_box(s, t, (uint32_t)nc, n, fpos, &gc->box[g]);
        *ok = true;
        return true;
    }
    if (nc == 0) { *ok = true; return true; }
    {
        uint32_t pos = 10u, ncomp = 0, more = 1;
        while (more) {
            uint32_t fl, gid;
            gxform x;
            if (!ui_tbl_has(t, pos, 4u)) return true;
            fl = ui_rd16(s, t, pos);
            gid = ui_rd16(s, t, pos + 2u);
            pos += 4u;
            if (!(fl & 2u)) return true;                /* point matching */
            memset(&x, 0, sizeof x);
            x.a = 16384;
            x.d = 16384;
            if (fl & 1u) {
                x.e = (int16_t)ui_rds16(s, t, pos);
                x.f = (int16_t)ui_rds16(s, t, pos + 2u);
                pos += 4u;
            } else {
                x.e = (int16_t)(int8_t)(uint8_t)ui_rd8(s, t, pos);
                x.f = (int16_t)(int8_t)(uint8_t)ui_rd8(s, t, pos + 1u);
                pos += 2u;
            }
            if (fl & 8u) {
                x.a = x.d = (int16_t)ui_rds16(s, t, pos);
                pos += 2u;
            } else if (fl & 0x40u) {
                x.a = (int16_t)ui_rds16(s, t, pos);
                x.d = (int16_t)ui_rds16(s, t, pos + 2u);
                pos += 4u;
            } else if (fl & 0x80u) {
                x.a = (int16_t)ui_rds16(s, t, pos);
                x.b = (int16_t)ui_rds16(s, t, pos + 2u);
                x.c = (int16_t)ui_rds16(s, t, pos + 4u);
                x.d = (int16_t)ui_rds16(s, t, pos + 6u);
                pos += 8u;
            }
            if (pos > len) return true;
            if (++ncomp > UI_GLYPH_MAX_COMPONENTS) return true;
            if (!add_comp(gc, gid, &x)) return false;
            more = fl & 0x20u;
        }
        *ok = true;
        return true;
    }
}

/* Bounds of a component after stb's transform; false when a coordinate
 * would leave the int16 range stb stores it in. */
static bool xform_box(const gxform *x, const gbox *in, gbox *out)
{
    double a = x->a / 16384.0, b = x->b / 16384.0, c = x->c / 16384.0, d = x->d / 16384.0;
    double m = sqrt(a * a + b * b), n = sqrt(c * c + d * d);
    if (in->empty) return true;
    for (int k = 0; k < 4; k++) {
        double px = (k & 1) ? in->x1 : in->x0, py = (k & 2) ? in->y1 : in->y0;
        double tx = m * (a * px + c * py + x->e), ty = n * (b * px + d * py + x->f);
        if (tx > 32767.0 || tx < -32768.0 || ty > 32767.0 || ty < -32768.0) return false;
        box_add(out, (int32_t)tx, (int32_t)ty);
    }
    return true;
}

static pc_status check_glyf(ui_font *f)
{
    const uint32_t ng = (uint32_t)f->num_glyphs;
    gcheck gc;
    uint8_t *state = NULL, *depth = NULL;
    uint32_t *stack = NULL, *cursor = NULL;
    pc_status st = PC_ERR_NOMEM;
    size_t need = (size_t)ng + 1u;
    memset(&gc, 0, sizeof gc);
    if (!ui_tbl_has(f->s.loca, 0, (f->info.indexToLocFormat ? 4u : 2u) * (ng + 1u)))
        return PC_ERR_FORMAT;
    gc.comp_start = (uint32_t *)malloc(need * sizeof(uint32_t));
    gc.own_ok = (uint8_t *)calloc(need, 1u);
    gc.points = (uint32_t *)calloc(need, sizeof(uint32_t));
    gc.box = (gbox *)calloc(need, sizeof(gbox));
    state = (uint8_t *)calloc(need, 1u);
    depth = (uint8_t *)calloc(need, 1u);
    stack = (uint32_t *)malloc(need * sizeof(uint32_t));
    cursor = (uint32_t *)malloc(need * sizeof(uint32_t));
    if (!gc.comp_start || !gc.own_ok || !gc.points || !gc.box || !state || !depth || !stack ||
        !cursor)
        goto out;
    for (uint32_t g = 0; g < ng; g++) {
        bool ok;
        gc.comp_start[g] = gc.ncomps;
        if (!check_record(f, &gc, g, &ok)) goto out;
        gc.own_ok[g] = ok ? 1u : 0u;
    }
    gc.comp_start[ng] = gc.ncomps;
    /* state: 0 new, 1 on stack, 2 valid, 3 invalid. Iterative DFS. */
    for (uint32_t root = 0; root < ng; root++) {
        uint32_t sp = 0;
        if (state[root]) continue;
        stack[sp] = root; cursor[sp] = gc.comp_start[root]; sp++;
        state[root] = 1;
        while (sp) {
            uint32_t g = stack[sp - 1], k = cursor[sp - 1];
            if (gc.own_ok[g] && k < gc.comp_start[g + 1]) {
                uint32_t c = gc.comps[k];
                cursor[sp - 1] = k + 1;
                if (c >= ng) continue;                  /* stb treats as empty */
                if (state[c] == 1) { gc.own_ok[g] = 0; continue; }     /* cycle */
                if (state[c] == 0) {
                    state[c] = 1;
                    stack[sp] = c; cursor[sp] = gc.comp_start[c]; sp++;
                }
                continue;
            }
            /* all components visited: finalize g */
            {
                bool ok = gc.own_ok[g] != 0;
                uint32_t pts = gc.points[g], dmax = 0;
                if (ok && gc.comp_start[g + 1] > gc.comp_start[g]) {
                    gbox b;
                    b.empty = true;
                    b.x0 = b.y0 = b.x1 = b.y1 = 0;
                    pts = 0;
                    for (uint32_t i = gc.comp_start[g]; i < gc.comp_start[g + 1]; i++) {
                        uint32_t c = gc.comps[i];
                        if (c >= ng) continue;
                        if (state[c] != 2) { ok = false; break; }
                        pts += gc.points[c];
                        if (pts > UI_GLYPH_MAX_POINTS) { ok = false; break; }
                        if (depth[c] > dmax) dmax = depth[c];
                        if (!xform_box(&gc.xf[i], &gc.box[c], &b)) { ok = false; break; }
                    }
                    if (dmax + 1u > UI_GLYPH_MAX_DEPTH) ok = false;
                    gc.box[g] = b;
                }
                state[g] = ok ? 2u : 3u;
                gc.points[g] = pts;
                depth[g] = (uint8_t)(dmax + 1u);
                if (ok) f->glyph_ok[g >> 3] |= (uint8_t)(1u << (g & 7u));
                sp--;
            }
        }
    }
    st = PC_OK;
out:
    free(gc.comp_start);
    free(gc.comps);
    free(gc.xf);
    free(gc.own_ok);
    free(gc.points);
    free(gc.box);
    free(state); free(depth); free(stack); free(cursor);
    return st;
}

/* ---- cmap selection ------------------------------------------------------ */
static bool choose_cmap(ui_font *f)
{
    const ui_sfnt *s = &f->s;
    ui_tbl t = s->cmap;
    uint32_t n = ui_rd16(s, t, 2);
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = 4u + 8u * i, pid, eid, off, fmt;
        int score = 0;
        if (!ui_tbl_has(t, r, 8u)) break;
        pid = ui_rd16(s, t, r); eid = ui_rd16(s, t, r + 2); off = ui_rd32(s, t, r + 4);
        if (!ui_tbl_has(t, off, 4u)) continue;
        fmt = ui_rd16(s, t, off);
        if (fmt == 12 && ((pid == 3 && eid == 10) || pid == 0)) score = 6;
        else if (fmt == 4 && pid == 3 && eid == 1) score = 5;
        else if (fmt == 4 && pid == 0) score = 4;
        else if ((fmt == 6 || fmt == 0) && pid == 0) score = 3;
        else if (fmt == 4 && pid == 3 && eid == 0) score = 2;
        else if (fmt == 13 && pid == 0) score = 1;
        if (score > best) {
            best = score;
            f->cmap_sub = off;
            f->cmap_fmt = fmt;
            f->cmap_symbol = (score == 2);
        }
    }
    return best > 0;
}

/* ---- face setup ---------------------------------------------------------- */
static void read_metrics(ui_font *f)
{
    const ui_sfnt *s = &f->s;
    uint32_t fs = 0;
    f->ascent = ui_rds16(s, s->hhea, 4);
    f->descent = -ui_rds16(s, s->hhea, 6);
    f->line_gap = ui_rds16(s, s->hhea, 8);
    if (s->os2.len >= 78u) {
        fs = ui_rd16(s, s->os2, 62);
        if ((fs & 0x80u) || (f->ascent == 0 && f->descent == 0)) {
            f->ascent = ui_rds16(s, s->os2, 68);
            f->descent = -ui_rds16(s, s->os2, 70);
            f->line_gap = ui_rds16(s, s->os2, 72);
        }
        if (f->ascent == 0 && f->descent == 0) {
            f->ascent = (int32_t)ui_rd16(s, s->os2, 74);
            f->descent = (int32_t)ui_rd16(s, s->os2, 76);
        }
    }
    if (f->ascent <= 0) f->ascent = (int32_t)(f->upem * 0.8f);
    if (f->descent < 0) f->descent = -f->descent;
    if (f->line_gap < 0) f->line_gap = 0;
    f->x_height = 0;
    f->cap_height = 0;
    if (s->os2.len >= 90u && ui_rd16(s, s->os2, 0) >= 2u) {
        f->x_height = ui_rds16(s, s->os2, 86);
        f->cap_height = ui_rds16(s, s->os2, 88);
    }
    if (s->post.len >= 12u) {
        f->ul_pos = -ui_rds16(s, s->post, 8);
        f->ul_size = ui_rds16(s, s->post, 10);
    }
    if (s->os2.len >= 30u) {
        f->st_size = ui_rds16(s, s->os2, 26);
        f->st_pos = ui_rds16(s, s->os2, 28);
    }
    f->desc.weight = (uint16_t)(s->os2.len >= 6u ? ui_rd16(s, s->os2, 4) : 400u);
    if (f->desc.weight == 0) f->desc.weight = 400u;
    f->desc.italic = (fs & 1u) != 0 || (ui_rd16(s, s->head, 44) & 2u) != 0;
}

/* Outline bounds of a glyph in font units (y up). */
static bool glyph_box(const ui_font *f, uint32_t gid, int32_t *y0, int32_t *y1)
{
    stbtt_vertex *v = NULL;
    int n;
    if (!ui_font_glyph_ok(f, gid)) return false;
    n = stbtt_GetGlyphShape(&f->info, (int)gid, &v);
    if (n <= 0) { stbtt_FreeShape(&f->info, v); return false; }
    *y0 = v[0].y; *y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].y < *y0) *y0 = v[i].y;
        if (v[i].y > *y1) *y1 = v[i].y;
    }
    stbtt_FreeShape(&f->info, v);
    return true;
}

static void fill_derived(ui_font *f)
{
    int32_t a, b;
    if (f->x_height <= 0) {
        f->x_height = (int32_t)(f->upem * 0.5f);
        if (glyph_box(f, ui_font_cmap(f, 'x'), &a, &b) && b > 0) f->x_height = b;
    }
    if (f->cap_height <= 0) {
        f->cap_height = (int32_t)(f->upem * 0.7f);
        if (glyph_box(f, ui_font_cmap(f, 'H'), &a, &b) && b > 0) f->cap_height = b;
    }
    if (f->ul_size <= 0) f->ul_size = (int32_t)(f->upem / 20.0f + 0.5f);
    if (f->ul_pos <= 0) f->ul_pos = (int32_t)(f->upem / 10.0f + 0.5f);
    if (f->st_size <= 0) f->st_size = f->ul_size;
    if (f->st_pos <= 0) f->st_pos = f->x_height / 2 + f->st_size / 2;
}

pc_status ui_font_open(ui_font *f, const uint8_t *data, size_t len, int face)
{
    ui_sfnt *s = &f->s;
    ui_stb_tables tb;
    pc_status st = ui_sfnt_locate(s, data, len, face);
    int32_t ng, count;
    size_t bits;
    if (st != PC_OK) return st;
    if (s->head.len < 54u || s->hhea.len < 36u || s->maxp.len < 6u || s->cmap.len < 4u ||
        s->hmtx.len < 4u)
        return PC_ERR_FORMAT;
    f->upem = (float)ui_rd16(s, s->head, 18);
    if (f->upem < 16.0f || f->upem > 16384.0f) return PC_ERR_FORMAT;
    ng = (int32_t)ui_rd16(s, s->maxp, 4);
    if (ng < 1) return PC_ERR_FORMAT;
    f->num_hmetrics = (int32_t)ui_rd16(s, s->hhea, 34);
    if (f->num_hmetrics < 1) return PC_ERR_FORMAT;
    if (!choose_cmap(f)) return PC_ERR_UNSUPPORTED;
    memset(&tb, 0, sizeof tb);
    tb.fontstart = s->start;
    tb.head = s->head.off;
    tb.hhea = s->hhea.off;
    tb.hmtx = s->hmtx.off;
    tb.num_glyphs = ng;
    if (s->glyf.off && s->loca.off) {
        int32_t lf = ui_rds16(s, s->head, 50);
        if (lf != 0 && lf != 1) return PC_ERR_FORMAT;
        tb.loca = s->loca.off;
        tb.glyf = s->glyf.off;
        tb.loca_format = lf;
    } else if (s->cff.off && s->cff.len >= 4u) {
        tb.cff = s->cff.off;
        tb.cff_len = s->cff.len;
        f->cff = true;
    } else {
        return PC_ERR_UNSUPPORTED;
    }
    count = ui_stb_setup(&f->info, data, &tb);
    if (count < 1) return PC_ERR_FORMAT;
    if (count < ng) ng = count;
    f->num_glyphs = ng;
    f->info.numGlyphs = ng;
    bits = ((size_t)ng + 7u) / 8u;
    f->glyph_ok = (uint8_t *)calloc(bits, 1u);
    if (!f->glyph_ok) return PC_ERR_NOMEM;
    if (f->cff) {
        memset(f->glyph_ok, 0xFF, bits);
    } else {
        st = check_glyf(f);
        if (st != PC_OK) return st;
    }
    read_metrics(f);
    fill_derived(f);
    f->desc.cff = f->cff;
    if (!ui_sfnt_name(s, 16, f->desc.family, sizeof f->desc.family))
        (void)ui_sfnt_name(s, 1, f->desc.family, sizeof f->desc.family);
    if (!ui_sfnt_name(s, 17, f->desc.style, sizeof f->desc.style))
        (void)ui_sfnt_name(s, 2, f->desc.style, sizeof f->desc.style);
    (void)ui_sfnt_name(s, 4, f->desc.full_name, sizeof f->desc.full_name);
    ui_kern_init(f);
    return PC_OK;
}
