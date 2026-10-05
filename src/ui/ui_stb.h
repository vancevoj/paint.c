/* ui_stb.h - the narrow interface between paint.c and stb_truetype (L3).
 *
 * Only outline extraction is delegated to stb_truetype. ui_stb_setup fills
 * an stbtt_fontinfo from tables that ui_font_check.c has already located
 * and bounded, instead of stbtt_InitFont: the CFF buffer gets the real table
 * length (stb's own init uses a fixed 512 MB window), and the cmap, kern
 * and GPOS fields stay unset because our own bounds-checked code reads
 * those tables.
 */
#ifndef UI_STB_H
#define UI_STB_H

#include <stddef.h>
#include <stdint.h>

#include "stb_truetype.h"

typedef struct ui_stb_tables {
    uint32_t fontstart;
    uint32_t head, hhea, hmtx, loca, glyf;   /* offsets from the file start */
    uint32_t cff, cff_len;                   /* cff == 0 for glyf fonts */
    int32_t  num_glyphs;
    int32_t  loca_format;                    /* 0 short, 1 long */
} ui_stb_tables;

/* Returns the number of CFF charstrings (or num_glyphs for glyf fonts), or
 * -1 when the CFF data is unusable (not Type 2 charstrings, missing
 * CharStrings or FDSelect). */
int ui_stb_setup(stbtt_fontinfo *info, const unsigned char *data, const ui_stb_tables *t);

#endif /* UI_STB_H */
