/* ui_stb.c - the stb_truetype implementation translation unit (lane L3).
 *
 * Compiled without the project's strict warning flags because it contains
 * third-party code (target pc_ui_stb). STBTT_assert is disabled: every
 * assertion in the paths we use guards data that ui_font_check.c validates
 * before a face is set up, and stb's CFF reader is bounds-checked once its
 * buffer has the real table length (set below). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ui_stb.h"   /* declarations first; the implementation follows once */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"

/* stb's INDEX reader does signed arithmetic on offsets read from the file,
 * so every INDEX it will ever touch is parsed here first with the same
 * semantics and full validation: offsets start at 1, never decrease and stay
 * inside the data. A font with any bad INDEX is rejected. */
static stbtt__buf safe_index(stbtt__buf *b, int *ok)
{
    int start = b->cursor, count, offsize, head;
    stbtt_uint32 prev = 1, room;
    if (b->size - start < 2) { *ok = 0; return stbtt__new_buf(NULL, 0); }
    count = (int)stbtt__buf_get16(b);
    if (count == 0) return stbtt__buf_range(b, start, 2);
    if (b->size - b->cursor < 1) { *ok = 0; return stbtt__new_buf(NULL, 0); }
    offsize = stbtt__buf_get8(b);
    if (offsize < 1 || offsize > 4) { *ok = 0; return stbtt__new_buf(NULL, 0); }
    head = 3 + (count + 1) * offsize;
    if (head > b->size - start) { *ok = 0; return stbtt__new_buf(NULL, 0); }
    room = (stbtt_uint32)(b->size - start - head);
    for (int i = 0; i <= count; i++) {
        stbtt_uint32 o = stbtt__buf_get(b, offsize);
        if ((i == 0 && o != 1) || o < prev || o - 1 > room) {
            *ok = 0;
            return stbtt__new_buf(NULL, 0);
        }
        prev = o;
    }
    stbtt__buf_seek(b, start + head + (int)(prev - 1));
    return stbtt__buf_range(b, start, head + (int)(prev - 1));
}

/* stbtt__get_subrs with a validated INDEX. */
static stbtt__buf safe_subrs(stbtt__buf cff, stbtt__buf fontdict, int *ok)
{
    stbtt_uint32 subrsoff = 0, private_loc[2] = { 0, 0 };
    stbtt__buf pdict;
    stbtt__dict_get_ints(&fontdict, 18, 2, private_loc);
    if (!private_loc[1] || !private_loc[0]) return stbtt__new_buf(NULL, 0);
    pdict = stbtt__buf_range(&cff, (int)private_loc[1], (int)private_loc[0]);
    stbtt__dict_get_ints(&pdict, 19, 1, &subrsoff);
    if (!subrsoff) return stbtt__new_buf(NULL, 0);
    stbtt__buf_seek(&cff, (int)(private_loc[1] + subrsoff));
    return safe_index(&cff, ok);
}

int ui_stb_setup(stbtt_fontinfo *info, const unsigned char *data, const ui_stb_tables *t)
{
    memset(info, 0, sizeof *info);
    info->data = (unsigned char *)data;   /* stb never writes through it */
    info->fontstart = (int)t->fontstart;
    info->head = (int)t->head;
    info->hhea = (int)t->hhea;
    info->hmtx = (int)t->hmtx;
    info->loca = (int)t->loca;
    info->glyf = (int)t->glyf;
    info->kern = 0;     /* kerning is read by ui_font_kern.c */
    info->gpos = 0;
    info->svg = 0;      /* SVG glyphs are not used */
    info->index_map = 0;
    info->numGlyphs = t->num_glyphs;
    info->indexToLocFormat = t->loca_format;
    info->cff = stbtt__new_buf(NULL, 0);
    info->fontdicts = stbtt__new_buf(NULL, 0);
    info->fdselect = stbtt__new_buf(NULL, 0);
    if (!t->cff) return t->num_glyphs;

    /* Same steps as stbtt_InitFont_internal, over the real table size and
     * with validated INDEX structures. */
    {
        stbtt__buf b, topdict, topdictidx;
        stbtt_uint32 cstype = 2, charstrings = 0, fdarrayoff = 0, fdselectoff = 0;
        int ok = 1, hdr;
        if (t->cff_len >= 0x40000000u) return -1;
        info->cff = stbtt__new_buf(data + t->cff, t->cff_len);
        b = info->cff;
        stbtt__buf_skip(&b, 2);
        hdr = stbtt__buf_get8(&b);
        if (hdr < 4 || hdr >= b.size) return -1;
        stbtt__buf_seek(&b, hdr);
        (void)safe_index(&b, &ok);                   /* name INDEX */
        topdictidx = safe_index(&b, &ok);
        if (!ok || stbtt__cff_index_count(&topdictidx) < 1) return -1;
        topdict = stbtt__cff_index_get(topdictidx, 0);
        (void)safe_index(&b, &ok);                   /* string INDEX */
        info->gsubrs = safe_index(&b, &ok);
        if (!ok) return -1;
        stbtt__dict_get_ints(&topdict, 17, 1, &charstrings);
        stbtt__dict_get_ints(&topdict, 0x100 | 6, 1, &cstype);
        stbtt__dict_get_ints(&topdict, 0x100 | 36, 1, &fdarrayoff);
        stbtt__dict_get_ints(&topdict, 0x100 | 37, 1, &fdselectoff);
        info->subrs = safe_subrs(b, topdict, &ok);
        if (!ok || cstype != 2 || charstrings == 0) return -1;
        if (fdarrayoff) {
            int nfd;
            if (!fdselectoff || fdselectoff >= (stbtt_uint32)b.size) return -1;
            if (fdarrayoff >= (stbtt_uint32)b.size) return -1;
            stbtt__buf_seek(&b, (int)fdarrayoff);
            info->fontdicts = safe_index(&b, &ok);
            if (!ok) return -1;
            info->fdselect = stbtt__buf_range(&b, (int)fdselectoff, b.size - (int)fdselectoff);
            nfd = stbtt__cff_index_count(&info->fontdicts);
            for (int i = 0; i < nfd && ok; i++)
                (void)safe_subrs(info->cff, stbtt__cff_index_get(info->fontdicts, i), &ok);
            if (!ok) return -1;
        }
        if (charstrings >= (stbtt_uint32)b.size) return -1;
        stbtt__buf_seek(&b, (int)charstrings);
        info->charstrings = safe_index(&b, &ok);
        if (!ok || info->charstrings.size < 3) return -1;
        return stbtt__cff_index_count(&info->charstrings);
    }
}
