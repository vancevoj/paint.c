/* text_sfnt.h - hardened reader for the OpenType tables of color fonts
 * (lane TOOLB, T-TEXT-COLORFONT): COLR version 0 with CPAL palettes, CBLC
 * and CBDT bitmap strikes, sbix strikes, GSUB ligature lookups, and the
 * basics (cmap, hmtx, hhea, head, maxp, OS/2) for fonts that have no
 * outlines at all (Noto Color Emoji), which the UI toolkit's loader
 * rejects.
 *
 * Every read is bounds checked against its table and every count is
 * capped before it is used (P-08); malformed tables read as absent. Only
 * the documented subset is read: COLR version 1 paint graphs, EBDT
 * monochrome strikes and GSUB lookup types other than 4 (ligatures, also
 * inside extension lookups) are ignored, so such glyphs fall back to
 * their outlines.
 *
 * Thread rules: a text_sfnt is immutable after text_sfnt_open; every query
 * is pure and may run on any thread. Ownership: the font bytes are
 * borrowed and must outlive the text_sfnt; the lookup list is owned and
 * released by text_sfnt_close.
 */
#ifndef TEXT_SFNT_H
#define TEXT_SFNT_H

#include "pc/pc_text.h"

typedef struct text_tbl { uint32_t off, len; } text_tbl;

typedef struct text_sfnt {
    const uint8_t *d;
    size_t         n;
    text_tbl head, hhea, hmtx, maxp, cmap, os2, glyf, cff, colr, cpal, cblc, cbdt, sbix, gsub;
    uint32_t upem, num_glyphs, num_hmetrics;
    int32_t  ascent, descent, line_gap;     /* font units, descent > 0 below */
    int32_t  x_height, cap_height;          /* font units, 0 = unknown */
    uint32_t cmap_sub, cmap_fmt;            /* chosen cmap subtable (offset in cmap) */
    /* COLR v0 */
    uint32_t colr_base, colr_nbase, colr_layers, colr_nlayers;
    /* CPAL palette 0 */
    uint32_t cpal_first, cpal_entries, cpal_records, cpal_off;
    /* GSUB ligature subtables (offsets in the GSUB table), lookup order */
    uint32_t *lig;
    size_t    n_lig;
} text_sfnt;

/* Locate face `face` of a font file or collection. PC_ERR_FORMAT when the
 * file is not an sfnt or lacks head / hhea / hmtx / maxp / cmap with a
 * Unicode subtable, PC_ERR_ARG for a bad face index, PC_ERR_NOMEM. */
pc_status text_sfnt_open(text_sfnt *s, const uint8_t *data, size_t len, int face);
void      text_sfnt_close(text_sfnt *s);                 /* NULL-safe; zeroes *s */

bool      text_sfnt_has_outlines(const text_sfnt *s);   /* glyf or CFF */
bool      text_sfnt_has_colr(const text_sfnt *s);
bool      text_sfnt_has_bitmaps(const text_sfnt *s);    /* CBDT or sbix */
static inline bool text_sfnt_has_color(const text_sfnt *s)
{
    return text_sfnt_has_colr(s) || text_sfnt_has_bitmaps(s);
}

uint32_t  text_sfnt_cmap(const text_sfnt *s, uint32_t cp);       /* 0 = missing */
uint32_t  text_sfnt_advance(const text_sfnt *s, uint32_t gid);   /* font units */

/* COLR layers of gid with their CPAL palette 0 colors (index 0xFFFF and
 * indices outside the palette: the text color). Returns the layer count
 * (at most PC_TEXT_MAX_COLOR_LAYERS), writing up to cap of them. */
size_t    text_sfnt_colr_layers(const text_sfnt *s, uint32_t gid, pc_font_color_layer *out,
                                size_t cap);

/* An embedded bitmap glyph image (still encoded). */
typedef struct text_sfnt_image {
    const uint8_t *data;         /* borrowed from the font bytes */
    size_t         len;
    char           type[5];      /* "png ", "jpg ", ... NUL terminated */
    double         ppem;         /* strike size */
    /* placement in strike pixels, y up from the baseline: CBDT gives the
     * top edge (bottom_origin false), sbix the bottom edge */
    double         x, y;
    bool           bottom_origin;
} text_sfnt_image;

/* The bitmap of gid from the strike best suited for ppem (the smallest
 * one at least that big, else the largest). False when there is none. */
bool      text_sfnt_image_of(const text_sfnt *s, uint32_t gid, double ppem,
                             text_sfnt_image *out);

/* Apply the GSUB ligature lookups of the ccmp, liga, clig and rlig
 * features to gids[0..n) in place; returns the new count. */
size_t    text_sfnt_ligate(const text_sfnt *s, uint32_t *gids, size_t n);

#endif /* TEXT_SFNT_H */
