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

typedef struct pc_text_gent {
    uint32_t face, gid;
    bool     used;
    pc_poly  poly;        /* glyph coverage at pen (0, 0), synthetic styles applied */
} pc_text_gent;

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
};

/* pc_text_render.c */
void pc_text_cache_clear(pc_text *t);
void pc_text_cache_free(pc_text *t);

#endif /* PC_TEXT_INT_H */
