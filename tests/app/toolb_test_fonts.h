/* toolb_test_fonts.h - synthetic OpenType color fonts for the lane TOOLB
 * tests, built byte by byte from the OpenType specification:
 *
 *  "Toolb Colr": glyf outlines, COLR v0 + CPAL, GSUB ligatures (ccmp, and
 *                liga through an extension lookup), cmap format 4.
 *    gid 1 'A' (square), 2 U+2B50 (COLR: 3 red, 4 text color),
 *    5 U+2764 (COLR: 3 half transparent blue), 6 U+200D (empty, advance 0),
 *    7 ligature of 2 6 5 (COLR: 3 green), 8 U+0301 (small square),
 *    9 ligature of 1 8, 3 / 4 layer squares (no cmap entry).
 *  "Toolb Bits": no outlines, CBLC + CBDT strikes at 16 and 32 ppem with
 *                every index format (1..5) and image format (17..19), GSUB,
 *                cmap format 12.
 *    gid 1 U+1F600 red, 2 U+1F601 blue, 3 / 4 U+1F1E6 / U+1F1E7, 5 their
 *    ligature (green), 6 U+200D.
 *  "Toolb Sbix": glyf (.notdef square) + an sbix strike at 16 ppem with a
 *                PNG, a 'dupe' record and a JPEG, cmap format 12.
 *    gid 1 U+1F602 yellow (origin offset 1, -2), 2 U+1F603 = dupe of 1,
 *    3 U+1F604 teal JPEG.
 *
 * upem 1000 everywhere. Bitmaps: 8 x 8 at 16 ppem (bearing 1, 7), 16 x 16
 * at 32 ppem (bearing 2, 14), PNG encoded with the project's codec.
 * Test-only code (single threaded); returned buffers are owned by the
 * caller (pc_buf_free).
 */
#ifndef TOOLB_TEST_FONTS_H
#define TOOLB_TEST_FONTS_H

#include "pc/pc_codec.h"
#include "pc/pc_surf.h"

#include <stdlib.h>
#include <string.h>

#define TF_UPEM 1000

typedef struct tfb_table {
    char   tag[5];
    pc_buf b;
} tfb_table;

typedef struct tfb {
    tfb_table t[24];
    int       n;
} tfb;

static inline pc_buf *tfb_add(tfb *f, const char *tag)
{
    tfb_table *t = &f->t[f->n++];
    memcpy(t->tag, tag, 4);
    t->tag[4] = '\0';
    memset(&t->b, 0, sizeof t->b);
    return &t->b;
}

static inline void tfb_free(tfb *f)
{
    for (int i = 0; i < f->n; i++) pc_buf_free(&f->t[i].b);
    f->n = 0;
}

static inline void tb16(pc_buf *b, uint32_t v) { (void)pc_buf_put_be16(b, (uint16_t)v); }
static inline void tb32(pc_buf *b, uint32_t v) { (void)pc_buf_put_be32(b, v); }
static inline void tbs16(pc_buf *b, int32_t v) { tb16(b, (uint32_t)(uint16_t)(int16_t)v); }
static inline void tb8(pc_buf *b, uint32_t v) { (void)pc_buf_put_u8(b, (uint8_t)v); }
static inline void tbpad4(pc_buf *b)
{
    while (b->n & 3u) tb8(b, 0);
}
static inline void tbset16(pc_buf *b, size_t at, uint32_t v)
{
    b->p[at] = (uint8_t)(v >> 8);
    b->p[at + 1u] = (uint8_t)v;
}
static inline void tbset32(pc_buf *b, size_t at, uint32_t v)
{
    b->p[at] = (uint8_t)(v >> 24);
    b->p[at + 1u] = (uint8_t)(v >> 16);
    b->p[at + 2u] = (uint8_t)(v >> 8);
    b->p[at + 3u] = (uint8_t)v;
}

/* The sfnt: table directory (tags sorted) then the tables, 4-aligned. */
static inline void tfb_finish(tfb *f, bool otto, pc_buf *out)
{
    int order[24];
    uint32_t off;
    for (int i = 0; i < f->n; i++) order[i] = i;
    for (int i = 1; i < f->n; i++)
        for (int j = i; j > 0 && memcmp(f->t[order[j - 1]].tag, f->t[order[j]].tag, 4) > 0; j--) {
            int k = order[j];
            order[j] = order[j - 1];
            order[j - 1] = k;
        }
    memset(out, 0, sizeof *out);
    tb32(out, otto ? 0x4F54544Fu : 0x00010000u);
    tb16(out, (uint32_t)f->n);
    tb16(out, 0);
    tb16(out, 0);
    tb16(out, 0);
    off = 12u + 16u * (uint32_t)f->n;
    for (int i = 0; i < f->n; i++) {
        const tfb_table *t = &f->t[order[i]];
        (void)pc_buf_append(out, t->tag, 4u);
        tb32(out, 0);
        tb32(out, off);
        tb32(out, (uint32_t)t->b.n);
        off += ((uint32_t)t->b.n + 3u) & ~3u;
    }
    for (int i = 0; i < f->n; i++) {
        const tfb_table *t = &f->t[order[i]];
        (void)pc_buf_append(out, t->b.p, t->b.n);
        tbpad4(out);
    }
}

/* ---- common tables --------------------------------------------------------------------------- */

static inline void tf_head(tfb *f, bool long_loca)
{
    pc_buf *b = tfb_add(f, "head");
    tb16(b, 1); tb16(b, 0); tb32(b, 0x00010000u); tb32(b, 0); tb32(b, 0x5F0F3CF5u);
    tb16(b, 0x000Bu); tb16(b, TF_UPEM);
    for (int i = 0; i < 16; i++) tb8(b, 0);
    tbs16(b, 0); tbs16(b, -200); tbs16(b, 1000); tbs16(b, 800);
    tb16(b, 0); tb16(b, 8); tbs16(b, 2); tbs16(b, long_loca ? 1 : 0); tbs16(b, 0);
}

static inline void tf_hhea_hmtx_maxp(tfb *f, uint32_t ng, const uint16_t *adv)
{
    pc_buf *b = tfb_add(f, "hhea");
    tb16(b, 1); tb16(b, 0); tbs16(b, 800); tbs16(b, -200); tbs16(b, 0); tb16(b, 1000);
    tbs16(b, 0); tbs16(b, 0); tbs16(b, 1000); tbs16(b, 1); tbs16(b, 0); tbs16(b, 0);
    for (int i = 0; i < 4; i++) tbs16(b, 0);
    tbs16(b, 0); tb16(b, ng);
    b = tfb_add(f, "hmtx");
    for (uint32_t g = 0; g < ng; g++) {
        tb16(b, adv[g]);
        tbs16(b, 0);
    }
    b = tfb_add(f, "maxp");
    tb32(b, 0x00005000u);
    tb16(b, ng);
}

static inline void tf_utf16(pc_buf *b, const char *s)
{
    for (; *s; s++) tb16(b, (uint8_t)*s);
}

static inline void tf_name_os2(tfb *f, const char *family)
{
    pc_buf *b = tfb_add(f, "name");
    size_t fl = strlen(family) * 2u;
    tb16(b, 0); tb16(b, 3); tb16(b, 6 + 12 * 3);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 1); tb16(b, (uint32_t)fl); tb16(b, 0);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 2); tb16(b, 14); tb16(b, (uint32_t)fl);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 4); tb16(b, (uint32_t)fl);
    tb16(b, (uint32_t)fl + 14u);
    tf_utf16(b, family);
    tf_utf16(b, "Regular");
    tf_utf16(b, family);
    b = tfb_add(f, "OS/2");
    tb16(b, 4); tbs16(b, 500); tb16(b, 400); tb16(b, 5); tb16(b, 0);
    for (int i = 0; i < 10; i++) tbs16(b, 50);            /* sub / superscript, strikeout */
    tbs16(b, 0);                                          /* family class */
    for (int i = 0; i < 10; i++) tb8(b, 0);               /* panose */
    for (int i = 0; i < 4; i++) tb32(b, 0);               /* unicode ranges */
    (void)pc_buf_append(b, "TLBX", 4u);
    tb16(b, 0x40); tb16(b, 0x20); tb16(b, 0xFFFF);
    tbs16(b, 800); tbs16(b, -200); tbs16(b, 0); tb16(b, 800); tb16(b, 200);
    tb32(b, 1); tb32(b, 0);
    tbs16(b, 500); tbs16(b, 700); tb16(b, 0); tb16(b, 32); tb16(b, 2);
}

typedef struct tf_map { uint32_t cp, gid; } tf_map;

/* cmap with one subtable: (3,1) format 4 or (3,10) format 12. */
static inline void tf_cmap(tfb *f, const tf_map *m, size_t n, bool fmt12)
{
    pc_buf *b = tfb_add(f, "cmap");
    tb16(b, 0); tb16(b, 1);
    tb16(b, 3); tb16(b, fmt12 ? 10 : 1); tb32(b, 12);
    if (fmt12) {
        tb16(b, 12); tb16(b, 0); tb32(b, 16u + 12u * (uint32_t)n); tb32(b, 0); tb32(b, (uint32_t)n);
        for (size_t i = 0; i < n; i++) {
            tb32(b, m[i].cp);
            tb32(b, m[i].cp);
            tb32(b, m[i].gid);
        }
    } else {
        uint32_t segs = (uint32_t)n + 1u, len = 16u + 8u * segs;
        tb16(b, 4); tb16(b, len); tb16(b, 0); tb16(b, segs * 2u);
        tb16(b, 2); tb16(b, 0); tb16(b, 0);           /* search hints (unused) */
        for (size_t i = 0; i < n; i++) tb16(b, m[i].cp);
        tb16(b, 0xFFFF);
        tb16(b, 0);
        for (size_t i = 0; i < n; i++) tb16(b, m[i].cp);
        tb16(b, 0xFFFF);
        for (size_t i = 0; i < n; i++) tb16(b, (m[i].gid - m[i].cp) & 0xFFFFu);
        tb16(b, 1);
        for (size_t i = 0; i <= n; i++) tb16(b, 0);
    }
}

/* glyf + loca (long): squares (x0, y0, x1, y1) in font units, y up; a
 * zero box is an empty glyph. */
static inline void tf_glyf(tfb *f, const int16_t (*box)[4], uint32_t ng)
{
    pc_buf *g = tfb_add(f, "glyf"), *l;
    uint32_t *offs = (uint32_t *)calloc(ng + 1u, sizeof *offs);
    for (uint32_t i = 0; i < ng; i++) {
        const int16_t *q = box[i];
        offs[i] = (uint32_t)g->n;
        if (q[0] == q[2]) continue;
        tbs16(g, 1); tbs16(g, q[0]); tbs16(g, q[1]); tbs16(g, q[2]); tbs16(g, q[3]);
        tb16(g, 3); tb16(g, 0);
        for (int k = 0; k < 4; k++) tb8(g, 1);
        tbs16(g, q[0]); tbs16(g, 0); tbs16(g, q[2] - q[0]); tbs16(g, 0);
        tbs16(g, q[1]); tbs16(g, q[3] - q[1]); tbs16(g, 0); tbs16(g, q[1] - q[3]);
        tbpad4(g);
    }
    offs[ng] = (uint32_t)g->n;
    l = tfb_add(f, "loca");
    for (uint32_t i = 0; i <= ng; i++) tb32(l, offs[i]);
    free(offs);
}

/* GSUB with one script, a ccmp feature (lookup 0: plain ligature
 * subtable) and a liga feature (lookup 1: the same kind wrapped in an
 * extension lookup when lig2 is set). Ligature: first glyph, the other
 * components, the result. */
typedef struct tf_lig { uint16_t first, comp[3], ncomp, out; } tf_lig;

static inline void tf_ligsubst(pc_buf *b, const tf_lig *l, bool range_cov)
{
    size_t base = b->n;
    tb16(b, 1); tb16(b, 0); tb16(b, 1); tb16(b, 0);      /* format, coverage, 1 set, set off */
    tbset16(b, base + 6u, (uint32_t)(b->n - base));
    tb16(b, 1); tb16(b, 4);                               /* set: 1 ligature at +4 */
    tb16(b, l->out); tb16(b, (uint32_t)l->ncomp + 1u);
    for (uint16_t i = 0; i < l->ncomp; i++) tb16(b, l->comp[i]);
    tbset16(b, base + 2u, (uint32_t)(b->n - base));
    if (range_cov) {
        tb16(b, 2); tb16(b, 1); tb16(b, l->first); tb16(b, l->first); tb16(b, 0);
    } else {
        tb16(b, 1); tb16(b, 1); tb16(b, l->first);
    }
}

static inline void tf_gsub(tfb *f, const tf_lig *lig1, const tf_lig *lig2)
{
    pc_buf *b = tfb_add(f, "GSUB");
    size_t sl, fl, ll, l0, l1;
    tb16(b, 1); tb16(b, 0); tb16(b, 0); tb16(b, 0); tb16(b, 0);
    /* script list: DFLT with a default LangSys using both features */
    sl = b->n;
    tbset16(b, 4, (uint32_t)sl);
    tb16(b, 1); (void)pc_buf_append(b, "DFLT", 4u); tb16(b, 8);
    tb16(b, 4); tb16(b, 0);                               /* Script: defaultLangSys at +4 */
    tb16(b, 0); tb16(b, 0xFFFF); tb16(b, 2); tb16(b, 0); tb16(b, 1);
    /* feature list */
    fl = b->n;
    tbset16(b, 6, (uint32_t)fl);
    tb16(b, 2);
    (void)pc_buf_append(b, "ccmp", 4u); tb16(b, 14);
    (void)pc_buf_append(b, "liga", 4u); tb16(b, 20);
    tb16(b, 0); tb16(b, 1); tb16(b, 0);                   /* ccmp: lookup 0 */
    tb16(b, 0); tb16(b, 1); tb16(b, 1);                   /* liga: lookup 1 */
    /* lookup list */
    ll = b->n;
    tbset16(b, 8, (uint32_t)ll);
    tb16(b, lig2 ? 2 : 1); tb16(b, 0); tb16(b, 0);
    l0 = b->n;
    tbset16(b, ll + 2u, (uint32_t)(l0 - ll));
    tb16(b, 4); tb16(b, 0); tb16(b, 1); tb16(b, 8);
    tf_ligsubst(b, lig1, false);
    if (lig2) {
        l1 = b->n;
        tbset16(b, ll + 4u, (uint32_t)(l1 - ll));
        tb16(b, 7); tb16(b, 0); tb16(b, 1); tb16(b, 8);
        tb16(b, 1); tb16(b, 4); tb32(b, 8);               /* extension -> type 4 at +8 */
        tf_ligsubst(b, lig2, true);
    }
}

/* ---- images ---------------------------------------------------------------------------------- */

/* An image file (codec id "png", "jpeg") of a w x h square of color c, the
 * top-left pixel transparent (PNG) or black (JPEG). */
static inline bool tf_image(const char *codec, int32_t w, int32_t h, pc_px32 c, pc_buf *out)
{
    const pc_codec *cod = pc_codec_by_id(codec);
    pc_doc *d = pc_doc_create((uint32_t)w, (uint32_t)h);
    pc_layer *l = d ? pc_layer_create(d, "L") : NULL;
    pc_px32 *px = (pc_px32 *)calloc((size_t)w * (size_t)h, sizeof *px);
    uint8_t params[512];
    bool ok = false;
    memset(out, 0, sizeof *out);
    if (cod && cod->save && d && l && px && cod->params_size <= sizeof params) {
        for (int32_t i = 1; i < w * h; i++) px[i] = c;
        if (pc_layer_store_rect(d, l, pc_doc_rect(d), px, (size_t)w) == PC_OK &&
            pc_doc_insert_layer(d, l, 0) == PC_OK) {
            l = NULL;
            pc_codec_default_params(cod, params);
            ok = cod->save(d, NULL, params, NULL, out) == PC_OK;
        }
    }
    if (l) pc_layer_destroy(l);
    pc_doc_destroy(d);
    free(px);
    return ok;
}

static inline bool tf_png(int32_t w, int32_t h, pc_px32 c, pc_buf *out)
{
    return tf_image("png", w, h, c, out);
}

static inline pc_px32 tf_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}

/* ---- the three fonts ------------------------------------------------------------------------- */

static inline bool tf_font_colr(pc_buf *out)
{
    static const int16_t box[10][4] = {
        { 0, 0, 0, 0 }, { 100, 0, 600, 700 }, { 300, 300, 400, 400 }, { 0, -200, 1000, 800 },
        { 250, 50, 750, 550 }, { 300, 300, 400, 400 }, { 0, 0, 0, 0 }, { 300, 300, 400, 400 },
        { 600, 800, 700, 900 }, { 100, 0, 600, 900 }
    };
    static const uint16_t adv[10] = { 500, 700, 1000, 1000, 1000, 1000, 0, 1000, 0, 700 };
    static const tf_map map[5] = {
        { 0x41u, 1u }, { 0x301u, 8u }, { 0x200Du, 6u }, { 0x2764u, 5u }, { 0x2B50u, 2u }
    };
    tf_lig l1, l2;
    tfb f;
    pc_buf *b;
    memset(&f, 0, sizeof f);
    tf_head(&f, true);
    tf_hhea_hmtx_maxp(&f, 10u, adv);
    tf_name_os2(&f, "Toolb Colr");
    tf_cmap(&f, map, 5u, false);
    tf_glyf(&f, box, 10u);
    b = tfb_add(&f, "COLR");
    tb16(b, 0); tb16(b, 3); tb32(b, 14); tb32(b, 14 + 18); tb16(b, 4);
    tb16(b, 2); tb16(b, 0); tb16(b, 2);                   /* base 2: layers 0, 1 */
    tb16(b, 5); tb16(b, 2); tb16(b, 1);                   /* base 5: layer 2 */
    tb16(b, 7); tb16(b, 3); tb16(b, 1);                   /* base 7: layer 3 */
    tb16(b, 3); tb16(b, 0);                               /* red */
    tb16(b, 4); tb16(b, 0xFFFF);                          /* text color */
    tb16(b, 3); tb16(b, 1);                               /* blue, alpha 128 */
    tb16(b, 3); tb16(b, 2);                               /* green */
    b = tfb_add(&f, "CPAL");
    tb16(b, 0); tb16(b, 3); tb16(b, 1); tb16(b, 3); tb32(b, 14); tb16(b, 0);
    tb8(b, 0); tb8(b, 0); tb8(b, 255); tb8(b, 255);       /* BGRA red */
    tb8(b, 255); tb8(b, 0); tb8(b, 0); tb8(b, 128);       /* blue, half */
    tb8(b, 0); tb8(b, 255); tb8(b, 0); tb8(b, 255);       /* green */
    memset(&l1, 0, sizeof l1);
    l1.first = 2; l1.comp[0] = 6; l1.comp[1] = 5; l1.ncomp = 2; l1.out = 7;
    memset(&l2, 0, sizeof l2);
    l2.first = 1; l2.comp[0] = 8; l2.ncomp = 1; l2.out = 9;
    tf_gsub(&f, &l1, &l2);
    tfb_finish(&f, false, out);
    tfb_free(&f);
    return true;
}

/* Small (5) or big (8) glyph metrics. */
static inline void tf_metrics(pc_buf *b, bool big, uint32_t w, uint32_t h, int32_t bx, int32_t by)
{
    tb8(b, h); tb8(b, w); tb8(b, (uint32_t)(uint8_t)(int8_t)bx);
    tb8(b, (uint32_t)(uint8_t)(int8_t)by); tb8(b, w + 2u);
    if (big) { tb8(b, 0); tb8(b, 0); tb8(b, h); }
}

static inline bool tf_font_bits(pc_buf *out)
{
    static const uint16_t adv[7] = { 500, 1000, 1000, 1000, 1000, 1000, 0 };
    static const tf_map map[5] = {
        { 0x200Du, 6u }, { 0x1F1E6u, 3u }, { 0x1F1E7u, 4u }, { 0x1F600u, 1u }, { 0x1F601u, 2u }
    };
    pc_buf png[2][3];                    /* [strike][red, blue, green] */
    pc_px32 col[3];
    tfb f;
    pc_buf *l, *d;
    tf_lig lig;
    size_t arr0, arr1, st0, st1;
    bool ok = true;
    col[0] = tf_rgba(255, 0, 0, 255);
    col[1] = tf_rgba(0, 0, 255, 255);
    col[2] = tf_rgba(0, 255, 0, 255);
    for (int s = 0; s < 2; s++)
        for (int c = 0; c < 3; c++) ok = tf_png(8 << s, 8 << s, col[c], &png[s][c]) && ok;
    if (!ok) return false;
    memset(&f, 0, sizeof f);
    tf_head(&f, false);
    tf_hhea_hmtx_maxp(&f, 7u, adv);
    tf_name_os2(&f, "Toolb Bits");
    tf_cmap(&f, map, 5u, true);
    d = tfb_add(&f, "CBDT");
    tb16(d, 3); tb16(d, 0);
    l = tfb_add(&f, "CBLC");
    tb16(l, 3); tb16(l, 0); tb32(l, 2);
    st0 = l->n;
    for (int s = 0; s < 2; s++) {
        tb32(l, 0); tb32(l, 0); tb32(l, s ? 2 : 3); tb32(l, 0);
        for (int k = 0; k < 24; k++) tb8(l, 0);           /* hori, vert line metrics */
        tb16(l, 1); tb16(l, 5); tb8(l, 16u << s); tb8(l, 16u << s); tb8(l, 32); tb8(l, 1);
    }
    st1 = st0 + 48u;
    /* strike 0 (16 ppem): [1] fmt 1 / img 17, [2] fmt 2 / img 19, [5] fmt 4 / img 18 */
    arr0 = l->n;
    tbset32(l, st0, (uint32_t)arr0);
    tb16(l, 1); tb16(l, 1); tb32(l, 0);
    tb16(l, 2); tb16(l, 2); tb32(l, 0);
    tb16(l, 5); tb16(l, 5); tb32(l, 0);
    tbset32(l, arr0 + 4u, (uint32_t)(l->n - arr0));
    tb16(l, 1); tb16(l, 17); tb32(l, (uint32_t)d->n); tb32(l, 0);
    tb32(l, 9u + (uint32_t)png[0][0].n);
    tf_metrics(d, false, 8, 8, 1, 7); tb32(d, (uint32_t)png[0][0].n);
    (void)pc_buf_append(d, png[0][0].p, png[0][0].n);
    tbset32(l, arr0 + 12u, (uint32_t)(l->n - arr0));
    tb16(l, 2); tb16(l, 19); tb32(l, (uint32_t)d->n); tb32(l, 4u + (uint32_t)png[0][1].n);
    tf_metrics(l, true, 8, 8, 1, 7);
    tb32(d, (uint32_t)png[0][1].n);
    (void)pc_buf_append(d, png[0][1].p, png[0][1].n);
    tbset32(l, arr0 + 20u, (uint32_t)(l->n - arr0));
    tb16(l, 4); tb16(l, 18); tb32(l, (uint32_t)d->n); tb32(l, 1);
    tb16(l, 5); tb16(l, 0); tb16(l, 0); tb16(l, 12u + (uint32_t)png[0][2].n);
    tf_metrics(d, true, 8, 8, 1, 7); tb32(d, (uint32_t)png[0][2].n);
    (void)pc_buf_append(d, png[0][2].p, png[0][2].n);
    tbset32(l, st0 + 4u, (uint32_t)(l->n - arr0));
    /* strike 1 (32 ppem): [1..2] fmt 3 / img 17, [5] fmt 5 / img 19 */
    tbpad4(l);
    arr1 = l->n;
    tbset32(l, st1, (uint32_t)arr1);
    tb16(l, 1); tb16(l, 2); tb32(l, 0);
    tb16(l, 5); tb16(l, 5); tb32(l, 0);
    tbset32(l, arr1 + 4u, (uint32_t)(l->n - arr1));
    tb16(l, 3); tb16(l, 17); tb32(l, (uint32_t)d->n);
    tb16(l, 0); tb16(l, 9u + (uint32_t)png[1][0].n);
    tb16(l, 18u + (uint32_t)png[1][0].n + (uint32_t)png[1][1].n);
    tb16(l, 0);                                           /* pad */
    tf_metrics(d, false, 16, 16, 2, 14); tb32(d, (uint32_t)png[1][0].n);
    (void)pc_buf_append(d, png[1][0].p, png[1][0].n);
    tf_metrics(d, false, 16, 16, 2, 14); tb32(d, (uint32_t)png[1][1].n);
    (void)pc_buf_append(d, png[1][1].p, png[1][1].n);
    tbset32(l, arr1 + 12u, (uint32_t)(l->n - arr1));
    tb16(l, 5); tb16(l, 19); tb32(l, (uint32_t)d->n);
    tb32(l, 4u + (uint32_t)png[1][2].n);
    tf_metrics(l, true, 16, 16, 2, 14);
    tb32(l, 1); tb16(l, 5); tb16(l, 0);
    tb32(d, (uint32_t)png[1][2].n);
    (void)pc_buf_append(d, png[1][2].p, png[1][2].n);
    tbset32(l, st1 + 4u, (uint32_t)(l->n - arr1));
    memset(&lig, 0, sizeof lig);
    lig.first = 3; lig.comp[0] = 4; lig.ncomp = 1; lig.out = 5;
    tf_gsub(&f, &lig, NULL);
    tfb_finish(&f, false, out);
    tfb_free(&f);
    for (int s = 0; s < 2; s++)
        for (int c = 0; c < 3; c++) pc_buf_free(&png[s][c]);
    return true;
}

static inline bool tf_font_sbix(pc_buf *out)
{
    static const int16_t box[4][4] = {
        { 50, 0, 450, 700 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }
    };
    static const uint16_t adv[4] = { 500, 1000, 1000, 1000 };
    static const tf_map map[3] = { { 0x1F602u, 1u }, { 0x1F603u, 2u }, { 0x1F604u, 3u } };
    pc_buf png, jpg, *b;
    tfb f;
    uint32_t g1, g2, g3;
    if (!tf_png(8, 8, tf_rgba(255, 255, 0, 255), &png)) return false;
    if (!tf_image("jpeg", 8, 8, tf_rgba(0, 200, 200, 255), &jpg)) {
        pc_buf_free(&png);
        return false;
    }
    memset(&f, 0, sizeof f);
    tf_head(&f, true);
    tf_hhea_hmtx_maxp(&f, 4u, adv);
    tf_name_os2(&f, "Toolb Sbix");
    tf_cmap(&f, map, 3u, true);
    tf_glyf(&f, box, 4u);
    b = tfb_add(&f, "sbix");
    tb16(b, 1); tb16(b, 1); tb32(b, 1); tb32(b, 12);
    tb16(b, 16); tb16(b, 72);                             /* strike at 12 */
    g1 = 24u + 8u + (uint32_t)png.n;                      /* data from strike + 24 */
    g2 = g1 + 10u;
    g3 = g2 + 8u + (uint32_t)jpg.n;
    tb32(b, 24); tb32(b, 24);                             /* glyph 0: none */
    tb32(b, g1); tb32(b, g2); tb32(b, g3);
    tbs16(b, 1); tbs16(b, -2); (void)pc_buf_append(b, "png ", 4u);
    (void)pc_buf_append(b, png.p, png.n);
    tbs16(b, 0); tbs16(b, 0); (void)pc_buf_append(b, "dupe", 4u); tb16(b, 1);
    tbs16(b, 0); tbs16(b, 0); (void)pc_buf_append(b, "jpg ", 4u);
    (void)pc_buf_append(b, jpg.p, jpg.n);
    tfb_finish(&f, false, out);
    tfb_free(&f);
    pc_buf_free(&png);
    pc_buf_free(&jpg);
    return true;
}

#endif /* TOOLB_TEST_FONTS_H */
