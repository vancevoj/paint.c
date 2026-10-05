/* ui_font_kern.c - pair kerning from GPOS (PairPos formats 1 and 2, also
 * inside Extension lookups) or the legacy kern table (format 0). Every read
 * is bounds-checked against its table (ui_rd16). Lookups referenced by any
 * 'kern' feature are applied in LookupList order and their XAdvance
 * adjustments of the first glyph are summed. */
#include "ui_font_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAX_KERN_SUBTABLES 4096
#define MAX_KERN_LOOKUPS   256

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

void ui_kern_free(ui_font *f)
{
    free(f->kern_subtables);
    free(f->kern_lookup_end);
    f->kern_subtables = NULL;
    f->kern_lookup_end = NULL;
    f->n_kern_lookups = 0;
}

void ui_kern_init(ui_font *f)
{
    const ui_sfnt *s = &f->s;
    ui_tbl g = s->gpos;
    uint16_t lookups[MAX_KERN_LOOKUPS];
    int32_t nl = 0, nsub = 0;
    uint32_t flist, llist, nfeat, nlook;
    f->has_kern_table = false;
    if (s->kern.len >= 4u && ui_rd16(s, s->kern, 0) == 0u) f->has_kern_table = true;
    if (g.len < 10u || ui_rd16(s, g, 0) != 1u) return;
    flist = ui_rd16(s, g, 6);
    llist = ui_rd16(s, g, 8);
    nfeat = ui_rd16(s, g, flist);
    nlook = ui_rd16(s, g, llist);
    for (uint32_t i = 0; i < nfeat; i++) {
        uint32_t rec = flist + 2u + 6u * i, ft, cnt;
        if (!ui_tbl_has(g, rec, 6u)) break;
        if (ui_rd32(s, g, rec) != 0x6B65726Eu) continue;      /* 'kern' */
        ft = flist + ui_rd16(s, g, rec + 4u);
        cnt = ui_rd16(s, g, ft + 2u);
        for (uint32_t k = 0; k < cnt && nl < MAX_KERN_LOOKUPS; k++) {
            uint32_t li = ui_rd16(s, g, ft + 4u + 2u * k);
            bool dup = false;
            if (li >= nlook) continue;
            for (int32_t q = 0; q < nl; q++) if (lookups[q] == li) dup = true;
            if (!dup) lookups[nl++] = (uint16_t)li;
        }
    }
    if (nl == 0) return;
    qsort(lookups, (size_t)nl, sizeof(uint16_t), cmp_u16);
    f->kern_subtables = (uint32_t *)malloc(MAX_KERN_SUBTABLES * sizeof(uint32_t));
    f->kern_lookup_end = (uint16_t *)malloc((size_t)nl * sizeof(uint16_t));
    if (!f->kern_subtables || !f->kern_lookup_end) { ui_kern_free(f); return; }
    for (int32_t i = 0; i < nl; i++) {
        uint32_t lt = llist + ui_rd16(s, g, llist + 2u + 2u * lookups[i]);
        uint32_t type = ui_rd16(s, g, lt), nst = ui_rd16(s, g, lt + 4u);
        for (uint32_t k = 0; k < nst && nsub < MAX_KERN_SUBTABLES; k++) {
            uint32_t st = lt + ui_rd16(s, g, lt + 6u + 2u * k);
            if (type == 9u) {
                if (ui_rd16(s, g, st) != 1u || ui_rd16(s, g, st + 2u) != 2u) continue;
                st += ui_rd32(s, g, st + 4u);
            } else if (type != 2u) {
                continue;
            }
            if (!ui_tbl_has(g, st, 10u)) continue;
            f->kern_subtables[nsub++] = st;
        }
        f->kern_lookup_end[i] = (uint16_t)nsub;
    }
    f->n_kern_lookups = nl;
}

static int32_t coverage(const ui_sfnt *s, ui_tbl g, uint32_t cov, uint32_t gid)
{
    uint32_t fmt = ui_rd16(s, g, cov), n = ui_rd16(s, g, cov + 2u);
    int32_t lo = 0, hi = (int32_t)n - 1;
    if (fmt == 1u) {
        if (!ui_tbl_has(g, cov + 4u, 2u * n)) return -1;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            uint32_t v = ui_rd16(s, g, cov + 4u + 2u * (uint32_t)mid);
            if (gid < v) hi = mid - 1;
            else if (gid > v) lo = mid + 1;
            else return mid;
        }
    } else if (fmt == 2u) {
        if (!ui_tbl_has(g, cov + 4u, 6u * n)) return -1;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            uint32_t r = cov + 4u + 6u * (uint32_t)mid;
            uint32_t a = ui_rd16(s, g, r), b = ui_rd16(s, g, r + 2u);
            if (gid < a) hi = mid - 1;
            else if (gid > b) lo = mid + 1;
            else return (int32_t)(ui_rd16(s, g, r + 4u) + gid - a);
        }
    }
    return -1;
}

static uint32_t class_of(const ui_sfnt *s, ui_tbl g, uint32_t cd, uint32_t gid)
{
    uint32_t fmt = ui_rd16(s, g, cd);
    if (fmt == 1u) {
        uint32_t start = ui_rd16(s, g, cd + 2u), n = ui_rd16(s, g, cd + 4u);
        if (gid >= start && gid - start < n) return ui_rd16(s, g, cd + 6u + 2u * (gid - start));
    } else if (fmt == 2u) {
        uint32_t n = ui_rd16(s, g, cd + 2u);
        int32_t lo = 0, hi = (int32_t)n - 1;
        if (!ui_tbl_has(g, cd + 4u, 6u * n)) return 0;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            uint32_t r = cd + 4u + 6u * (uint32_t)mid;
            uint32_t a = ui_rd16(s, g, r), b = ui_rd16(s, g, r + 2u);
            if (gid < a) hi = mid - 1;
            else if (gid > b) lo = mid + 1;
            else return ui_rd16(s, g, r + 4u);
        }
    }
    return 0;
}

static uint32_t popcount8(uint32_t v)
{
    uint32_t c = 0;
    for (v &= 0xFFu; v; v &= v - 1u) c++;
    return c;
}

/* Returns 1 and sets *adv when the subtable applies to the pair. */
static int pairpos(const ui_sfnt *s, ui_tbl g, uint32_t st, uint32_t g1, uint32_t g2,
                   int32_t *adv)
{
    uint32_t fmt = ui_rd16(s, g, st), vf1 = ui_rd16(s, g, st + 4u), vf2 = ui_rd16(s, g, st + 6u);
    uint32_t sz1 = 2u * popcount8(vf1), sz2 = 2u * popcount8(vf2);
    uint32_t xadv = 2u * popcount8(vf1 & 3u);
    int32_t ci = coverage(s, g, st + ui_rd16(s, g, st + 2u), g1);
    if (ci < 0) return 0;
    if (fmt == 1u) {
        uint32_t nsets = ui_rd16(s, g, st + 8u), ps, n, rec;
        int32_t lo = 0, hi;
        if ((uint32_t)ci >= nsets) return 0;
        ps = st + ui_rd16(s, g, st + 10u + 2u * (uint32_t)ci);
        n = ui_rd16(s, g, ps);
        rec = 2u + sz1 + sz2;
        if (!ui_tbl_has(g, ps + 2u, rec * n)) return 0;
        hi = (int32_t)n - 1;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            uint32_t r = ps + 2u + rec * (uint32_t)mid, v = ui_rd16(s, g, r);
            if (g2 < v) hi = mid - 1;
            else if (g2 > v) lo = mid + 1;
            else {
                *adv = (vf1 & 4u) ? ui_rds16(s, g, r + 2u + xadv) : 0;
                return 1;
            }
        }
        return 0;
    }
    if (fmt == 2u) {
        uint32_t cd1 = st + ui_rd16(s, g, st + 8u), cd2 = st + ui_rd16(s, g, st + 10u);
        uint32_t n1 = ui_rd16(s, g, st + 12u), n2 = ui_rd16(s, g, st + 14u);
        uint32_t c1 = class_of(s, g, cd1, g1), c2 = class_of(s, g, cd2, g2), r;
        if (c1 >= n1 || c2 >= n2) return 0;
        r = st + 16u + (c1 * n2 + c2) * (sz1 + sz2);
        *adv = (vf1 & 4u) ? ui_rds16(s, g, r + xadv) : 0;
        return 1;
    }
    return 0;
}

static int32_t kern_table(const ui_font *f, uint32_t g1, uint32_t g2)
{
    const ui_sfnt *s = &f->s;
    ui_tbl k = s->kern;
    uint32_t n = ui_rd16(s, k, 2), off = 4u;
    int32_t sum = 0;
    for (uint32_t i = 0; i < n && i < 64u; i++) {
        uint32_t len = ui_rd16(s, k, off + 2u), cov = ui_rd16(s, k, off + 4u);
        if (len < 6u) break;
        if ((cov >> 8) == 0u && (cov & 1u) && !(cov & 4u)) {
            uint32_t np = ui_rd16(s, k, off + 6u), key = (g1 << 16) | g2;
            int32_t lo = 0, hi = (int32_t)np - 1;
            if (ui_tbl_has(k, off + 14u, 6u * np)) {
                while (lo <= hi) {
                    int32_t mid = (lo + hi) / 2;
                    uint32_t r = off + 14u + 6u * (uint32_t)mid, v = ui_rd32(s, k, r);
                    if (key < v) hi = mid - 1;
                    else if (key > v) lo = mid + 1;
                    else { sum += ui_rds16(s, k, r + 4u); break; }
                }
            }
        }
        off += len;
    }
    return sum;
}

int32_t ui_kern_lookup(const ui_font *f, uint32_t g1, uint32_t g2)
{
    int32_t sum = 0;
    if (f->n_kern_lookups > 0) {
        int32_t sub = 0;
        for (int32_t l = 0; l < f->n_kern_lookups; l++) {
            int32_t end = f->kern_lookup_end[l];
            for (; sub < end; sub++) {
                int32_t adv = 0;
                if (pairpos(&f->s, f->s.gpos, f->kern_subtables[sub], g1, g2, &adv)) {
                    sum += adv;
                    sub = end;
                    break;
                }
            }
        }
        return sum;
    }
    if (f->has_kern_table) return kern_table(f, g1, g2);
    return 0;
}
