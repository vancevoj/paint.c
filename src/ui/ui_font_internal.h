/* ui_font_internal.h - face structure shared by the font sources (L3). */
#ifndef UI_FONT_INTERNAL_H
#define UI_FONT_INTERNAL_H

#include "ui/ui_font.h"
#include "ui_raster.h"
#include "ui_stb.h"

/* A located table: offset from the file start and length, both inside the
 * file (checked when the table directory is read). */
typedef struct ui_tbl { uint32_t off, len; } ui_tbl;

typedef struct ui_sfnt {
    const uint8_t *d;
    size_t         n;
    uint32_t       start;      /* offset of the face's table directory */
    ui_tbl cmap, head, hhea, hmtx, maxp, loca, glyf, cff, kern, gpos, os2, post, name;
} ui_sfnt;

/* Bounds-checked big-endian reads inside one table. They return 0 when
 * off + size exceeds the table, so callers can check ranges once and keep
 * the code readable. */
static inline uint32_t ui_rd8(const ui_sfnt *s, ui_tbl t, uint32_t off)
{
    return off < t.len ? s->d[t.off + off] : 0u;
}
static inline uint32_t ui_rd16(const ui_sfnt *s, ui_tbl t, uint32_t off)
{
    const uint8_t *p;
    if (off > t.len || t.len - off < 2u) return 0u;
    p = s->d + t.off + off;
    return ((uint32_t)p[0] << 8) | p[1];
}
static inline int32_t ui_rds16(const ui_sfnt *s, ui_tbl t, uint32_t off)
{
    return (int32_t)(int16_t)(uint16_t)ui_rd16(s, t, off);
}
static inline uint32_t ui_rd32(const ui_sfnt *s, ui_tbl t, uint32_t off)
{
    const uint8_t *p;
    if (off > t.len || t.len - off < 4u) return 0u;
    p = s->d + t.off + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline bool ui_tbl_has(ui_tbl t, uint32_t off, uint32_t size)
{
    return off <= t.len && t.len - off >= size;
}

/* Main-thread caches (see the thread rules in ui_font.h). */
#define UI_CMAP_DIRECT 0x800u
#define UI_KERN_CACHE  4096u
typedef struct ui_cmap_slot { uint16_t gid; uint8_t src; uint8_t valid; } ui_cmap_slot;
typedef struct ui_kern_slot { uint32_t key; int16_t val; uint8_t valid; uint8_t pad; } ui_kern_slot;

struct ui_font {
    ui_sfnt         s;
    stbtt_fontinfo  info;
    uint8_t        *owned;          /* file bytes when owned */
    uint32_t        serial;         /* unique per load, keys GPU caches */
    int32_t         num_glyphs;
    int32_t         num_hmetrics;
    bool            cff;
    uint8_t        *glyph_ok;       /* bitset: outline safe to hand to stb */
    float           upem;
    int32_t         ascent, descent, line_gap;   /* descent > 0 below baseline */
    int32_t         cap_height, x_height;
    int32_t         ul_pos, ul_size, st_pos, st_size;
    ui_font_desc    desc;
    /* cmap: chosen subtable inside the cmap table */
    uint32_t        cmap_sub;       /* offset inside the cmap table */
    uint32_t        cmap_fmt;
    bool            cmap_symbol;    /* (3,0) symbol encoding */
    /* kerning */
    uint32_t       *kern_subtables; /* GPOS PairPos subtable offsets inside GPOS */
    uint16_t       *kern_lookup_end;/* exclusive end index per lookup */
    int32_t         n_kern_lookups;
    bool            has_kern_table; /* legacy kern format 0 */
    /* fallbacks (borrowed) */
    const ui_font  *fallback[UI_FONT_MAX_FALLBACKS];
    int32_t         n_fallback;
    /* caches */
    ui_cmap_slot   *cmap_cache;     /* UI_CMAP_DIRECT entries, lazily allocated */
    ui_kern_slot   *kern_cache;     /* UI_KERN_CACHE entries */
};

/* ui_font_check.c: locate tables, validate, fill the face. */
pc_status ui_font_open(ui_font *f, const uint8_t *data, size_t len, int face);
pc_status ui_sfnt_locate(ui_sfnt *s, const uint8_t *data, size_t len, int face);
/* Name table string (name id, preferring Windows English) as UTF-8. */
bool      ui_sfnt_name(const ui_sfnt *s, uint32_t name_id, char *out, size_t cap);

/* ui_font_kern.c */
void    ui_kern_init(ui_font *f);
void    ui_kern_free(ui_font *f);
int32_t ui_kern_lookup(const ui_font *f, uint32_t g1, uint32_t g2);   /* font units */

/* ui_font.c */
uint32_t ui_font_cmap(const ui_font *f, uint32_t cp);                 /* 0 = missing */
int32_t  ui_font_advance(const ui_font *f, uint32_t gid);             /* font units */
static inline bool ui_font_glyph_ok(const ui_font *f, uint32_t gid)
{
    return gid < (uint32_t)f->num_glyphs && (f->glyph_ok[gid >> 3] >> (gid & 7u) & 1u);
}
/* Resolve cp through the fallback chain: *face receives the face that has
 * it (or f when none does, with gid 0). Pure (no caches). */
uint32_t ui_font_resolve(const ui_font *f, uint32_t cp, const ui_font **face);
/* Cached variant for the main thread. */
uint32_t ui_font_resolve_cached(ui_font *f, uint32_t cp, const ui_font **face);
int32_t  ui_kern_cached(ui_font *f, uint32_t g1, uint32_t g2);

/* Vertical mapping from font units (y up) to pixels (y up): plain scaling,
 * or snapped zones (baseline, x-height, cap height) for small UI text. */
typedef struct ui_ymap {
    int   n;
    float u[6], px[6];
    float scale;
} ui_ymap;
void  ui_ymap_init(ui_ymap *m, const ui_font *f, float scale, bool hinted);
float ui_ymap_apply(const ui_ymap *m, float y_units);

/* Append glyph gid as closed contours to p: x_px = ox + (x * scale) + shear *
 * y_px_up, y_px = oy - ymap(y). Returns false when the glyph has no outline. */
bool ui_font_glyph_path(const ui_font *f, uint32_t gid, float scale, const ui_ymap *ym,
                        float ox, float oy, float shear, ui_path *p);

#endif /* UI_FONT_INTERNAL_H */
