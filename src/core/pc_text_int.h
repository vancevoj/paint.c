/* pc_text_int.h - private state shared by pc_text.c (layout, editing) and
 * pc_text_render.c (glyph cache, geometry). Not a public header. */
#ifndef PC_TEXT_INT_H
#define PC_TEXT_INT_H

#include "pc/pc_text.h"

#define PC_TEXT_MAX_FACES 16u

/* Synthetic styles: slant (x shift per pixel above the baseline) and the
 * emboldening stroke width as a fraction of the em size. */
#define PC_TEXT_SLANT 0.2
#define PC_TEXT_BOLD_FRAC (1.0 / 24.0)

/* Glyph kinds for color fonts (lane TOOLB). */
enum { PC_GK_UNKNOWN = 0, PC_GK_MONO, PC_GK_LAYERS, PC_GK_BITMAP };

typedef struct pc_text_gent {
    uint32_t face, gid;
    bool     used;
    uint8_t  kind;        /* PC_GK_*: classified on first use */
    bool     fg;          /* PC_GK_LAYERS: a layer uses the text color */
    pc_poly  poly;        /* glyph coverage at pen (0, 0), synthetic styles applied */
} pc_text_gent;

/* A color glyph image (lane TOOLB): the glyph rendered for a pen at a
 * quarter-pixel offset (qx / 4, qy / 4) from a whole pixel, straight
 * alpha. Image pixel (i, j) covers document pixels from (pen pixel + ox +
 * i * k, pen pixel + oy + j * k); k > 1 when a huge glyph was rendered at
 * a reduced resolution. */
typedef struct pc_text_cimg {
    uint32_t face, gid;
    uint8_t  qx, qy;
    bool     aa;
    pc_px32  fg;          /* foreground color baked in (layers with fg only) */
    int32_t  ox, oy;      /* document offset of the image's top-left */
    int32_t  dw, dh;      /* document extent in pixels */
    int32_t  w, h;        /* image size */
    double   k;           /* document pixels per image pixel (>= 1) */
    pc_px32 *px;
    size_t   bytes;
} pc_text_cimg;

struct pc_text {
    char          *buf;            /* NUL-terminated UTF-8, always valid */
    size_t         len, cap;
    size_t         caret, anchor;
    double         goal_x;         /* remembered x for Up / Down */
    bool           has_goal;
    pc_text_style  style;
    const pc_font_face *faces[PC_TEXT_MAX_FACES];
    size_t         n_faces;
    pc_pt          origin;

    /* layout (arrays are reserved before every edit, so layout never fails) */
    pc_text_line  *lines;
    size_t         n_lines, cap_lines;
    pc_text_glyph *glyphs;
    size_t         n_glyphs, cap_glyphs;
    double         em, line_h;
    pc_font_metrics fm;            /* primary face metrics, defaults resolved */

    /* glyph cache (pc_text_render.c) */
    pc_text_gent  *cache;
    size_t         cache_cap, cache_n;
    pc_poly        scratch;

    /* color glyph images (pc_text_render.c, lane TOOLB) */
    pc_text_cimg **cimg;           /* owned entries */
    size_t         n_cimg, cap_cimg, cimg_bytes;
    pc_raster     *ras;            /* reused for color layers */
    /* Sharp mode zones (pc_text_render.c, lane TOOLB) */
    bool           hint_zones;     /* hint_xh / hint_cap are valid */
    double         hint_xh, hint_cap;
};

/* pc_text_render.c */
void pc_text_cache_clear(pc_text *t);   /* glyph polys and color images */
void pc_text_cache_free(pc_text *t);

#endif /* PC_TEXT_INT_H */
