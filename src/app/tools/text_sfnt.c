/* text_sfnt.c - hardened reader for color font tables (lane TOOLB, see
 * text_sfnt.h). Written from the OpenType specification (Microsoft
 * OpenType 1.9: COLR, CPAL, CBLC, CBDT, sbix, GSUB, cmap, hmtx, hhea,
 * head, maxp, OS/2). */
#include "text_sfnt.h"

#include <stdlib.h>
#include <string.h>

#define MAX_STRIKES 256u
#define MAX_SUBTABLES 65536u
#define MAX_FEATURES 2048u
#define MAX_LIG_SUBTABLES 4096u
#define MAX_LIGATURES 4096u      /* ligatures tried per set */

/* ---- bounded big-endian reads ------------------------------------------------------- */

static bool has(text_tbl t, uint64_t off, uint64_t size)
{
    return off <= t.len && (uint64_t)t.len - off >= size;
}

static uint32_t rd8(const text_sfnt *s, text_tbl t, uint64_t off)
{
    return has(t, off, 1u) ? s->d[t.off + off] : 0u;
}

static uint32_t rd16(const text_sfnt *s, text_tbl t, uint64_t off)
{
    const uint8_t *p;
    if (!has(t, off, 2u)) return 0u;
    p = s->d + t.off + off;
    return ((uint32_t)p[0] << 8) | p[1];
}

static int32_t rds16(const text_sfnt *s, text_tbl t, uint64_t off)
{
    return (int32_t)(int16_t)(uint16_t)rd16(s, t, off);
}

static uint32_t rd32(const text_sfnt *s, text_tbl t, uint64_t off)
{
    const uint8_t *p;
    if (!has(t, off, 4u)) return 0u;
    p = s->d + t.off + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- opening ------------------------------------------------------------------------- */

static pc_status locate(text_sfnt *s, const uint8_t *d, size_t n, int face)
{
    uint64_t start = 0;
    uint32_t ntab;
    if (!d || n < 12u) return PC_ERR_FORMAT;
    if (face < 0) return PC_ERR_ARG;
    if (memcmp(d, "ttcf", 4) == 0) {
        uint32_t nf = be32(d + 8);
        if ((uint32_t)face >= nf) return PC_ERR_ARG;
        if (12u + 4u * ((uint64_t)face + 1u) > n) return PC_ERR_FORMAT;
        start = be32(d + 12u + 4u * (uint32_t)face);
    } else if (face != 0) {
        return PC_ERR_ARG;
    }
    if (start + 12u > n) return PC_ERR_FORMAT;
    if (be32(d + start) != 0x00010000u && memcmp(d + start, "OTTO", 4) != 0 &&
        memcmp(d + start, "true", 4) != 0)
        return PC_ERR_FORMAT;
    ntab = ((uint32_t)d[start + 4u] << 8) | d[start + 5u];
    if (start + 12u + 16u * (uint64_t)ntab > n) return PC_ERR_FORMAT;
    s->d = d;
    s->n = n;
    for (uint32_t i = 0; i < ntab; i++) {
        const uint8_t *rec = d + start + 12u + 16u * i;
        uint32_t off = be32(rec + 8), len = be32(rec + 12);
        text_tbl *t = NULL;
        if ((uint64_t)off + len > n || off == 0u) continue;
        if (memcmp(rec, "head", 4) == 0) t = &s->head;
        else if (memcmp(rec, "hhea", 4) == 0) t = &s->hhea;
        else if (memcmp(rec, "hmtx", 4) == 0) t = &s->hmtx;
        else if (memcmp(rec, "maxp", 4) == 0) t = &s->maxp;
        else if (memcmp(rec, "cmap", 4) == 0) t = &s->cmap;
        else if (memcmp(rec, "OS/2", 4) == 0) t = &s->os2;
        else if (memcmp(rec, "glyf", 4) == 0) t = &s->glyf;
        else if (memcmp(rec, "CFF ", 4) == 0) t = &s->cff;
        else if (memcmp(rec, "CFF2", 4) == 0) t = &s->cff;
        else if (memcmp(rec, "COLR", 4) == 0) t = &s->colr;
        else if (memcmp(rec, "CPAL", 4) == 0) t = &s->cpal;
        else if (memcmp(rec, "CBLC", 4) == 0) t = &s->cblc;
        else if (memcmp(rec, "CBDT", 4) == 0) t = &s->cbdt;
        else if (memcmp(rec, "sbix", 4) == 0) t = &s->sbix;
        else if (memcmp(rec, "GSUB", 4) == 0) t = &s->gsub;
        if (t && t->off == 0u) {
            t->off = off;
            t->len = len;
        }
    }
    return PC_OK;
}

/* Unicode cmap subtable of format 4 or 12 (12 preferred). */
static bool choose_cmap(text_sfnt *s)
{
    text_tbl t = s->cmap;
    uint32_t n = rd16(s, t, 2), best = 0;
    if (n > 512u) n = 512u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = 4u + 8u * i, pid = rd16(s, t, r), eid = rd16(s, t, r + 2u);
        uint32_t off = rd32(s, t, r + 4u), fmt = rd16(s, t, off), score = 0;
        bool uni = pid == 0u || (pid == 3u && (eid == 1u || eid == 10u));
        if (!has(t, off, 16u)) continue;
        if (fmt == 12u && uni) score = 3u;
        else if (fmt == 4u && uni) score = 2u;
        else if (fmt == 4u && pid == 3u && eid == 0u) score = 1u;
        if (score > best) {
            best = score;
            s->cmap_sub = off;
            s->cmap_fmt = fmt;
        }
    }
    return best > 0u;
}

static void read_metrics(text_sfnt *s)
{
    uint32_t fs = 0;
    s->ascent = rds16(s, s->hhea, 4);
    s->descent = -rds16(s, s->hhea, 6);
    s->line_gap = rds16(s, s->hhea, 8);
    if (s->os2.len >= 78u) {
        fs = rd16(s, s->os2, 62);
        if ((fs & 0x80u) || (s->ascent == 0 && s->descent == 0)) {
            s->ascent = rds16(s, s->os2, 68);
            s->descent = -rds16(s, s->os2, 70);
            s->line_gap = rds16(s, s->os2, 72);
        }
    }
    if (s->ascent <= 0) s->ascent = (int32_t)(s->upem * 4u / 5u);
    if (s->descent < 0) s->descent = -s->descent;
    if (s->line_gap < 0) s->line_gap = 0;
    if (s->os2.len >= 90u && rd16(s, s->os2, 0) >= 2u) {
        s->x_height = rds16(s, s->os2, 86);
        s->cap_height = rds16(s, s->os2, 88);
        if (s->x_height < 0) s->x_height = 0;
        if (s->cap_height < 0) s->cap_height = 0;
    }
}

static void open_colr(text_sfnt *s)
{
    text_tbl c = s->colr, p = s->cpal;
    if (c.len >= 14u) {
        uint32_t nb = rd16(s, c, 2), bo = rd32(s, c, 4), lo = rd32(s, c, 8), nl = rd16(s, c, 12);
        if (nb && nl && has(c, bo, 6u * (uint64_t)nb) && has(c, lo, 4u * (uint64_t)nl)) {
            s->colr_base = bo;
            s->colr_nbase = nb;
            s->colr_layers = lo;
            s->colr_nlayers = nl;
        }
    }
    if (p.len >= 14u) {
        uint32_t ne = rd16(s, p, 2), np = rd16(s, p, 4), nr = rd16(s, p, 6), ro = rd32(s, p, 8);
        if (np && has(p, 12u, 2u * (uint64_t)np) && has(p, ro, 4u * (uint64_t)nr)) {
            s->cpal_entries = ne;
            s->cpal_records = nr;
            s->cpal_off = ro;
            s->cpal_first = rd16(s, p, 12);
        }
    }
}

static bool lig_tag(const uint8_t *tag)
{
    return memcmp(tag, "ccmp", 4) == 0 || memcmp(tag, "liga", 4) == 0 ||
           memcmp(tag, "clig", 4) == 0 || memcmp(tag, "rlig", 4) == 0;
}

static bool lig_push(text_sfnt *s, size_t *cap, uint32_t v)
{
    if (s->n_lig == *cap) {
        size_t nc = *cap ? *cap * 2u : 64u;
        uint32_t *nv = (uint32_t *)realloc(s->lig, nc * sizeof *nv);
        if (!nv) return false;
        s->lig = nv;
        *cap = nc;
    }
    s->lig[s->n_lig++] = v;
    return true;
}

/* Ligature subtables of the wanted features, in lookup order; a 0 entry
 * ends each lookup. */
static pc_status open_gsub(text_sfnt *s)
{
    text_tbl g = s->gsub;
    uint32_t fl, ll, nf, nl;
    size_t cap = 0;
    uint8_t *want;
    pc_status st = PC_OK;
    if (g.len < 10u || rd16(s, g, 0) != 1u) return PC_OK;
    fl = rd16(s, g, 6);
    ll = rd16(s, g, 8);
    nf = rd16(s, g, fl);
    nl = rd16(s, g, ll);
    if (!nl || !nf) return PC_OK;
    if (nf > MAX_FEATURES) nf = MAX_FEATURES;
    want = (uint8_t *)calloc(nl, 1u);
    if (!want) return PC_ERR_NOMEM;
    for (uint32_t i = 0; i < nf; i++) {
        uint64_t r = (uint64_t)fl + 2u + 6u * i;
        uint32_t fo, cnt;
        if (!has(g, r, 6u)) break;
        if (!lig_tag(s->d + g.off + r)) continue;
        fo = fl + rd16(s, g, r + 4u);
        cnt = rd16(s, g, (uint64_t)fo + 2u);
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t li;
            if (!has(g, (uint64_t)fo + 4u + 2u * k, 2u)) break;
            li = rd16(s, g, (uint64_t)fo + 4u + 2u * k);
            if (li < nl) want[li] = 1u;
        }
    }
    for (uint32_t li = 0; li < nl && st == PC_OK; li++) {
        uint32_t lo, type, nsub;
        bool any = false;
        if (!want[li]) continue;
        lo = ll + rd16(s, g, (uint64_t)ll + 2u + 2u * li);
        type = rd16(s, g, lo);
        nsub = rd16(s, g, (uint64_t)lo + 4u);
        if (type != 4u && type != 7u) continue;
        for (uint32_t k = 0; k < nsub && st == PC_OK; k++) {
            uint64_t so;
            if (!has(g, (uint64_t)lo + 6u + 2u * k, 2u)) break;
            so = (uint64_t)lo + rd16(s, g, (uint64_t)lo + 6u + 2u * k);
            if (type == 7u) {
                if (rd16(s, g, so) != 1u || rd16(s, g, so + 2u) != 4u) continue;
                so += rd32(s, g, so + 4u);
            }
            if (so == 0u || so > 0xFFFFFFFFu || !has(g, so, 6u) || rd16(s, g, so) != 1u)
                continue;
            if (s->n_lig + 2u > MAX_LIG_SUBTABLES) break;
            if (!lig_push(s, &cap, (uint32_t)so)) st = PC_ERR_NOMEM;
            any = true;
        }
        if (any && st == PC_OK && !lig_push(s, &cap, 0u)) st = PC_ERR_NOMEM;
    }
    free(want);
    return st;
}

pc_status text_sfnt_open(text_sfnt *s, const uint8_t *data, size_t len, int face)
{
    pc_status st;
    if (!s) return PC_ERR_ARG;
    memset(s, 0, sizeof *s);
    st = locate(s, data, len, face);
    if (st != PC_OK) {
        memset(s, 0, sizeof *s);
        return st;
    }
    if (s->head.len < 54u || s->hhea.len < 36u || s->maxp.len < 6u || s->hmtx.len < 4u ||
        s->cmap.len < 4u) {
        memset(s, 0, sizeof *s);
        return PC_ERR_FORMAT;
    }
    s->upem = rd16(s, s->head, 18);
    s->num_glyphs = rd16(s, s->maxp, 4);
    s->num_hmetrics = rd16(s, s->hhea, 34);
    if (s->upem < 16u || s->upem > 16384u || !s->num_glyphs || !s->num_hmetrics ||
        !has(s->hmtx, 0u, 4u * (uint64_t)s->num_hmetrics) || !choose_cmap(s)) {
        memset(s, 0, sizeof *s);
        return PC_ERR_FORMAT;
    }
    read_metrics(s);
    open_colr(s);
    st = open_gsub(s);
    if (st != PC_OK) text_sfnt_close(s);
    return st;
}

void text_sfnt_close(text_sfnt *s)
{
    if (!s) return;
    free(s->lig);
    memset(s, 0, sizeof *s);
}

bool text_sfnt_has_outlines(const text_sfnt *s) { return s->glyf.len > 0u || s->cff.len > 0u; }
bool text_sfnt_has_colr(const text_sfnt *s) { return s->colr_nbase > 0u; }
bool text_sfnt_has_bitmaps(const text_sfnt *s)
{
    return (s->cblc.len >= 8u && s->cbdt.len >= 4u) || s->sbix.len >= 8u;
}

/* ---- cmap and metrics ------------------------------------------------------------------------ */

uint32_t text_sfnt_cmap(const text_sfnt *s, uint32_t cp)
{
    text_tbl t = s->cmap;
    uint64_t b = s->cmap_sub;
    uint32_t gid = 0;
    if (!s->d) return 0u;
    if (s->cmap_fmt == 4u) {
        uint32_t segx2 = rd16(s, t, b + 6u), segs = segx2 / 2u, lo = 0, hi;
        uint64_t ends = b + 14u, starts = ends + segx2 + 2u, deltas = starts + segx2;
        uint64_t ranges = deltas + segx2;
        if (cp > 0xFFFFu || !segs || !has(t, ranges, segx2)) return 0u;
        hi = segs;
        while (lo < hi) {                          /* first segment with end >= cp */
            uint32_t mid = lo + (hi - lo) / 2u;
            if (rd16(s, t, ends + 2u * mid) < cp) lo = mid + 1u;
            else hi = mid;
        }
        if (lo >= segs) return 0u;
        {
            uint32_t start = rd16(s, t, starts + 2u * lo), delta = rd16(s, t, deltas + 2u * lo);
            uint32_t ro = rd16(s, t, ranges + 2u * lo);
            if (cp < start) return 0u;
            if (ro == 0u) {
                gid = (cp + delta) & 0xFFFFu;
            } else {
                uint32_t g = rd16(s, t, ranges + 2u * lo + ro + 2u * (cp - start));
                gid = g ? (g + delta) & 0xFFFFu : 0u;
            }
        }
    } else if (s->cmap_fmt == 12u) {
        uint32_t ng = rd32(s, t, b + 12u), lo = 0, hi;
        if (!has(t, b + 16u, 12u * (uint64_t)ng)) return 0u;
        hi = ng;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            uint64_t r = b + 16u + 12u * (uint64_t)mid;
            if (rd32(s, t, r + 4u) < cp) lo = mid + 1u;
            else hi = mid;
        }
        if (lo < ng) {
            uint64_t r = b + 16u + 12u * (uint64_t)lo;
            uint32_t start = rd32(s, t, r), sg = rd32(s, t, r + 8u);
            if (cp >= start) gid = sg + (cp - start);
        }
    }
    return gid < s->num_glyphs ? gid : 0u;
}

uint32_t text_sfnt_advance(const text_sfnt *s, uint32_t gid)
{
    uint32_t i;
    if (!s->d || !s->num_hmetrics) return 0u;
    i = gid < s->num_hmetrics ? gid : s->num_hmetrics - 1u;
    return rd16(s, s->hmtx, 4u * (uint64_t)i);
}

/* ---- COLR ------------------------------------------------------------------------------------ */

size_t text_sfnt_colr_layers(const text_sfnt *s, uint32_t gid, pc_font_color_layer *out,
                             size_t cap)
{
    text_tbl c = s->colr, p = s->cpal;
    uint32_t lo = 0, hi = s->colr_nbase, first, num;
    if (!s->colr_nbase || gid > 0xFFFFu) return 0u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (rd16(s, c, s->colr_base + 6u * (uint64_t)mid) < gid) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= s->colr_nbase || rd16(s, c, s->colr_base + 6u * (uint64_t)lo) != gid) return 0u;
    first = rd16(s, c, s->colr_base + 6u * (uint64_t)lo + 2u);
    num = rd16(s, c, s->colr_base + 6u * (uint64_t)lo + 4u);
    if ((uint64_t)first + num > s->colr_nlayers) return 0u;
    if (num > PC_TEXT_MAX_COLOR_LAYERS) num = PC_TEXT_MAX_COLOR_LAYERS;
    for (uint32_t i = 0; i < num && i < cap; i++) {
        uint64_t r = s->colr_layers + 4u * ((uint64_t)first + i);
        uint32_t lg = rd16(s, c, r), pi = rd16(s, c, r + 2u);
        pc_font_color_layer *l = &out[i];
        memset(l, 0, sizeof *l);
        l->gid = lg < s->num_glyphs ? lg : 0u;
        if (pi == 0xFFFFu || pi >= s->cpal_entries ||
            (uint64_t)s->cpal_first + pi >= s->cpal_records) {
            l->foreground = true;
        } else {
            uint64_t cr = s->cpal_off + 4u * ((uint64_t)s->cpal_first + pi);
            l->color.b = (uint8_t)rd8(s, p, cr);
            l->color.g = (uint8_t)rd8(s, p, cr + 1u);
            l->color.r = (uint8_t)rd8(s, p, cr + 2u);
            l->color.a = (uint8_t)rd8(s, p, cr + 3u);
        }
    }
    return num;
}

/* ---- bitmaps --------------------------------------------------------------------------------- */

/* Preference of a strike of size ppem for a wanted size: the smallest one
 * at least as big, then the largest smaller one. Lower is better. */
static double strike_rank(double ppem, double want)
{
    return ppem >= want ? ppem - want : 1e6 + (want - ppem);
}

/* CBDT image data of gid in strike rec. */
static bool cbdt_glyph(const text_sfnt *s, uint64_t rec, uint32_t gid, text_sfnt_image *out)
{
    text_tbl l = s->cblc, d = s->cbdt;
    uint64_t arr = rd32(s, l, rec);
    uint32_t nsub = rd32(s, l, rec + 8u);
    if (nsub > MAX_SUBTABLES) nsub = MAX_SUBTABLES;
    for (uint32_t j = 0; j < nsub; j++) {
        uint64_t e = arr + 8u * (uint64_t)j, hdr, goff = 0, glen = 0;
        uint32_t first, last, fmt, ifmt, ioff;
        int32_t bx = 0, by = 0;
        bool have_metrics = false;
        if (!has(l, e, 8u)) return false;
        first = rd16(s, l, e);
        last = rd16(s, l, e + 2u);
        if (gid < first || gid > last) continue;
        hdr = arr + rd32(s, l, e + 4u);
        if (!has(l, hdr, 8u)) return false;
        fmt = rd16(s, l, hdr);
        ifmt = rd16(s, l, hdr + 2u);
        ioff = rd32(s, l, hdr + 4u);
        switch (fmt) {
        case 1: {
            uint64_t o = hdr + 8u + 4u * (uint64_t)(gid - first);
            uint32_t a = rd32(s, l, o), b = rd32(s, l, o + 4u);
            if (!has(l, o, 8u) || b <= a) return false;
            goff = (uint64_t)ioff + a;
            glen = b - a;
            break;
        }
        case 3: {
            uint64_t o = hdr + 8u + 2u * (uint64_t)(gid - first);
            uint32_t a = rd16(s, l, o), b = rd16(s, l, o + 2u);
            if (!has(l, o, 4u) || b <= a) return false;
            goff = (uint64_t)ioff + a;
            glen = b - a;
            break;
        }
        case 2: {
            uint32_t sz = rd32(s, l, hdr + 8u);
            if (!has(l, hdr, 20u)) return false;
            goff = (uint64_t)ioff + (uint64_t)sz * (gid - first);
            glen = sz;
            bx = (int32_t)(int8_t)rd8(s, l, hdr + 14u);
            by = (int32_t)(int8_t)rd8(s, l, hdr + 15u);
            have_metrics = true;
            break;
        }
        case 4: {
            uint32_t ng = rd32(s, l, hdr + 8u), k;
            if (ng > 65536u || !has(l, hdr + 12u, 4u * ((uint64_t)ng + 1u))) return false;
            for (k = 0; k < ng; k++)
                if (rd16(s, l, hdr + 12u + 4u * (uint64_t)k) == gid) break;
            if (k == ng) return false;
            {
                uint32_t a = rd16(s, l, hdr + 14u + 4u * (uint64_t)k);
                uint32_t b = rd16(s, l, hdr + 18u + 4u * (uint64_t)k);
                if (b <= a) return false;
                goff = (uint64_t)ioff + a;
                glen = b - a;
            }
            break;
        }
        case 5: {
            uint32_t sz = rd32(s, l, hdr + 8u), ng = rd32(s, l, hdr + 20u), k;
            if (ng > 65536u || !has(l, hdr + 24u, 2u * (uint64_t)ng)) return false;
            for (k = 0; k < ng; k++)
                if (rd16(s, l, hdr + 24u + 2u * (uint64_t)k) == gid) break;
            if (k == ng) return false;
            goff = (uint64_t)ioff + (uint64_t)sz * k;
            glen = sz;
            bx = (int32_t)(int8_t)rd8(s, l, hdr + 14u);
            by = (int32_t)(int8_t)rd8(s, l, hdr + 15u);
            have_metrics = true;
            break;
        }
        default:
            return false;
        }
        if (!has(d, goff, glen)) return false;
        {
            text_tbl g;
            uint64_t dl, data;
            g.off = d.off + (uint32_t)goff;
            g.len = (uint32_t)glen;
            if (ifmt == 17u) {
                bx = (int32_t)(int8_t)rd8(s, g, 2u);
                by = (int32_t)(int8_t)rd8(s, g, 3u);
                dl = rd32(s, g, 5u);
                data = 9u;
            } else if (ifmt == 18u) {
                bx = (int32_t)(int8_t)rd8(s, g, 2u);
                by = (int32_t)(int8_t)rd8(s, g, 3u);
                dl = rd32(s, g, 8u);
                data = 12u;
            } else if (ifmt == 19u && have_metrics) {
                dl = rd32(s, g, 0u);
                data = 4u;
            } else {
                return false;
            }
            if (!dl || !has(g, data, dl)) return false;
            out->data = s->d + g.off + data;
            out->len = (size_t)dl;
            memcpy(out->type, "png ", 5u);
            out->x = (double)bx;
            out->y = (double)by;
            out->bottom_origin = false;
            return true;
        }
    }
    return false;
}

static bool cbdt_image(const text_sfnt *s, uint32_t gid, double ppem, text_sfnt_image *out)
{
    text_tbl l = s->cblc;
    uint32_t n = rd32(s, l, 4u);
    bool tried[MAX_STRIKES];
    if (rd16(s, l, 0) < 2u || !n) return false;
    if (n > MAX_STRIKES) n = MAX_STRIKES;
    memset(tried, 0, sizeof tried);
    for (;;) {
        double best = 0.0;
        uint32_t bi = n;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t rec = 8u + 48u * (uint64_t)i;
            double r;
            if (tried[i] || !has(l, rec, 48u)) continue;
            if (gid < rd16(s, l, rec + 40u) || gid > rd16(s, l, rec + 42u)) continue;
            if (!rd8(s, l, rec + 45u)) continue;
            r = strike_rank((double)rd8(s, l, rec + 45u), ppem);
            if (bi == n || r < best) {
                best = r;
                bi = i;
            }
        }
        if (bi == n) return false;
        tried[bi] = true;
        if (cbdt_glyph(s, 8u + 48u * (uint64_t)bi, gid, out)) {
            out->ppem = (double)rd8(s, l, 8u + 48u * (uint64_t)bi + 45u);
            return true;
        }
    }
}

/* sbix glyph record of gid in the strike at so; one 'dupe' hop at most. */
static bool sbix_glyph(const text_sfnt *s, uint64_t so, uint32_t gid, text_sfnt_image *out)
{
    text_tbl t = s->sbix;
    for (int hop = 0; hop < 2; hop++) {
        uint64_t go = so + 4u + 4u * (uint64_t)gid, a, b, rec;
        if (gid >= s->num_glyphs || !has(t, go, 8u)) return false;
        a = rd32(s, t, go);
        b = rd32(s, t, go + 4u);
        if (b <= a || b - a < 8u) return false;
        rec = so + a;
        if (!has(t, rec, b - a)) return false;
        if (memcmp(s->d + t.off + rec + 4u, "dupe", 4) == 0) {
            if (b - a < 10u) return false;
            gid = rd16(s, t, rec + 8u);
            continue;
        }
        out->x = (double)rds16(s, t, rec);
        out->y = (double)rds16(s, t, rec + 2u);
        out->bottom_origin = true;
        memcpy(out->type, s->d + t.off + rec + 4u, 4u);
        out->type[4] = '\0';
        out->data = s->d + t.off + rec + 8u;
        out->len = (size_t)(b - a - 8u);
        return true;
    }
    return false;
}

static bool sbix_image(const text_sfnt *s, uint32_t gid, double ppem, text_sfnt_image *out)
{
    text_tbl t = s->sbix;
    uint32_t n = rd32(s, t, 4u);
    bool tried[MAX_STRIKES];
    if (!n) return false;
    if (n > MAX_STRIKES) n = MAX_STRIKES;
    memset(tried, 0, sizeof tried);
    for (;;) {
        double best = 0.0;
        uint32_t bi = n;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t so = rd32(s, t, 8u + 4u * (uint64_t)i);
            double r;
            if (tried[i] || !has(t, 8u + 4u * (uint64_t)i, 4u) || !has(t, so, 4u)) continue;
            if (!rd16(s, t, so)) continue;
            r = strike_rank((double)rd16(s, t, so), ppem);
            if (bi == n || r < best) {
                best = r;
                bi = i;
            }
        }
        if (bi == n) return false;
        tried[bi] = true;
        {
            uint64_t so = rd32(s, t, 8u + 4u * (uint64_t)bi);
            if (sbix_glyph(s, so, gid, out)) {
                out->ppem = (double)rd16(s, t, so);
                return true;
            }
        }
    }
}

bool text_sfnt_image_of(const text_sfnt *s, uint32_t gid, double ppem, text_sfnt_image *out)
{
    if (!s->d || !out || gid >= s->num_glyphs) return false;
    memset(out, 0, sizeof *out);
    if (s->cblc.len >= 8u && s->cbdt.len >= 4u && cbdt_image(s, gid, ppem, out)) return true;
    if (s->sbix.len >= 8u && sbix_image(s, gid, ppem, out)) return true;
    return false;
}

/* ---- GSUB ligatures -------------------------------------------------------------------------- */

/* Coverage index of gid in the coverage table at off (GSUB), -1. */
static int32_t coverage(const text_sfnt *s, uint64_t off, uint32_t gid)
{
    text_tbl g = s->gsub;
    uint32_t fmt = rd16(s, g, off), n = rd16(s, g, off + 2u), lo = 0, hi = n;
    if (fmt == 1u) {
        if (!has(g, off + 4u, 2u * (uint64_t)n)) return -1;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (rd16(s, g, off + 4u + 2u * (uint64_t)mid) < gid) lo = mid + 1u;
            else hi = mid;
        }
        return lo < n && rd16(s, g, off + 4u + 2u * (uint64_t)lo) == gid ? (int32_t)lo : -1;
    }
    if (fmt == 2u) {
        if (!has(g, off + 4u, 6u * (uint64_t)n)) return -1;
        while (lo < hi) {                  /* first range with end >= gid */
            uint32_t mid = lo + (hi - lo) / 2u;
            if (rd16(s, g, off + 4u + 6u * (uint64_t)mid + 2u) < gid) lo = mid + 1u;
            else hi = mid;
        }
        if (lo < n) {
            uint64_t r = off + 4u + 6u * (uint64_t)lo;
            uint32_t start = rd16(s, g, r);
            if (gid >= start) return (int32_t)(rd16(s, g, r + 4u) + (gid - start));
        }
    }
    return -1;
}

/* Try the ligature subtable at so on gids[pos..n). Returns the number of
 * glyphs consumed (0: no match). */
static size_t lig_at(const text_sfnt *s, uint64_t so, const uint32_t *gids, size_t pos,
                     size_t n, uint32_t *lig)
{
    text_tbl g = s->gsub;
    int32_t ci = coverage(s, so + rd16(s, g, so + 2u), gids[pos]);
    uint64_t set;
    uint32_t cnt;
    if (ci < 0 || (uint32_t)ci >= rd16(s, g, so + 4u)) return 0u;
    set = so + rd16(s, g, so + 6u + 2u * (uint64_t)ci);
    cnt = rd16(s, g, set);
    if (cnt > MAX_LIGATURES) cnt = MAX_LIGATURES;
    for (uint32_t k = 0; k < cnt; k++) {
        uint64_t lo = set + rd16(s, g, set + 2u + 2u * (uint64_t)k);
        uint32_t comp = rd16(s, g, lo + 2u), m;
        if (!comp || pos + comp > n || !has(g, lo + 4u, 2u * ((uint64_t)comp - 1u))) continue;
        for (m = 1; m < comp; m++)
            if (rd16(s, g, lo + 4u + 2u * ((uint64_t)m - 1u)) != gids[pos + m]) break;
        if (m == comp) {
            *lig = rd16(s, g, lo);
            return comp;
        }
    }
    return 0u;
}

size_t text_sfnt_ligate(const text_sfnt *s, uint32_t *gids, size_t n)
{
    size_t i = 0;
    while (i < s->n_lig) {
        size_t end = i;
        while (end < s->n_lig && s->lig[end]) end++;
        /* one lookup: subtables [i, end) at every position */
        for (size_t pos = 0; pos < n; pos++) {
            for (size_t k = i; k < end; k++) {
                uint32_t lg = 0;
                size_t used = lig_at(s, s->lig[k], gids, pos, n, &lg);
                if (used) {
                    gids[pos] = lg < s->num_glyphs ? lg : 0u;
                    memmove(&gids[pos + 1u], &gids[pos + used], (n - pos - used) * sizeof *gids);
                    n -= used - 1u;
                    break;
                }
            }
        }
        i = end + 1u;
    }
    return n;
}
