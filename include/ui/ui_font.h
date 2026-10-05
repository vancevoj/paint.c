/* ui_font.h - font faces, text measurement and A8 text rasterization (L3).
 *
 * Faces are TrueType or OpenType (glyf or CFF outlines) files and
 * collections (TTC/OTC). Parsing uses stb_truetype behind a structural
 * validator (src/ui/ui_font_check.c): a face is fully checked when it is
 * loaded, glyphs that fail are rendered as empty, and kerning (GPOS pair
 * adjustment, legacy kern) is read by bounds-checked code of our own.
 * Complex-script shaping is out of scope (ADR-004): text is laid out as a
 * sequence of nominal glyphs with pair kerning.
 *
 * Sizes are em sizes in pixels (CSS font-size). Missing glyphs are looked up
 * in the fallback chain, then drawn as a box.
 *
 * Thread rules. A face is immutable after loading, plus small caches that
 * the measurement functions update. ui_text_width, ui_text_fit, ui_text_hit,
 * ui_text_caret_x and drawing through ui_draw.h are main-thread functions
 * (more precisely: never concurrently on the same face). The ui_text_*
 * layout and raster functions for the Text tool (section "A8 text") do not
 * touch the caches and may run on any thread, concurrently with each other
 * and with the main thread, as long as the face and its fallbacks are not
 * freed meanwhile.
 */
#ifndef UI_FONT_H
#define UI_FONT_H

#include "ui_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_font ui_font;

/* Hard limits applied before allocating (P-08). */
#define UI_FONT_MAX_FILE   (256u * 1024u * 1024u)  /* bytes per font file */
#define UI_FONT_MAX_SIZE   2048.0f                 /* em size in pixels */
#define UI_TEXT_MAX_PIXELS (64u * 1024u * 1024u)   /* A8 output buffer */
#define UI_FONT_MAX_FALLBACKS 8

/* ---- loading ------------------------------------------------------------- */
typedef enum ui_font_builtin {
    UI_FONT_REGULAR = 0,    /* Inter Regular, embedded */
    UI_FONT_SEMIBOLD = 1,   /* Inter SemiBold, embedded */
    UI_FONT_BUILTIN_COUNT
} ui_font_builtin;

/* New face over the embedded static data. Returns NULL only on OOM. The
 * caller owns the face (ui_font_free). Any thread. */
ui_font *ui_font_load_builtin(ui_font_builtin which);

#define UI_FONT_COPY 1u   /* copy data; otherwise data must outlive the face */

/* Load face `face` (0 for plain TTF/OTF files) from memory. On success *out
 * receives a new face owned by the caller. Errors: PC_ERR_ARG (bad
 * arguments or face index), PC_ERR_FORMAT (not a font, or failed
 * validation), PC_ERR_UNSUPPORTED (valid but unusable, e.g. no Unicode cmap
 * or Type 1 outlines), PC_ERR_LIMIT, PC_ERR_NOMEM. Any thread. */
pc_status ui_font_load_mem(const void *data, size_t len, int face, uint32_t flags,
                           ui_font **out);
/* Read a whole file (UTF-8 path, at most UI_FONT_MAX_FILE bytes) and load
 * face `face` from it; the face owns the bytes. PC_ERR_IO when unreadable.
 * Any thread. */
pc_status ui_font_load_file(const char *path, int face, ui_font **out);
/* NULL-safe. The face must not be used, and must not be a fallback of a
 * face still in use, afterwards. */
void      ui_font_free(ui_font *f);

/* Append a fallback face (borrowed: it must outlive f). Glyphs missing in f
 * are taken from the first fallback that has them (fallbacks of fallbacks
 * are not followed). PC_ERR_LIMIT beyond UI_FONT_MAX_FALLBACKS. Not
 * concurrently with other uses of f. */
pc_status ui_font_add_fallback(ui_font *f, const ui_font *fallback);

/* ---- describing files (font lists for the Text tool) --------------------- */
/* Faces in a TTF/OTF (1) or collection (n); 0 when data is not a font. */
int ui_font_face_count(const void *data, size_t len);

typedef struct ui_font_desc {
    char     family[96];       /* typographic family (name id 16, else 1), UTF-8 */
    char     style[64];        /* typographic subfamily (17, else 2) */
    char     full_name[128];   /* name id 4 */
    uint16_t weight;           /* OS/2 usWeightClass, 400 if absent */
    bool     italic;           /* OS/2 fsSelection or head macStyle */
    bool     cff;              /* CFF outlines (otherwise glyf) */
} ui_font_desc;

/* Read names and style bits of one face without a full load. Validation is
 * limited to what is read. Any thread. */
pc_status ui_font_describe(const void *data, size_t len, int face, ui_font_desc *out);
/* Description of a loaded face. Any thread. */
void      ui_font_get_desc(const ui_font *f, ui_font_desc *out);

/* ---- metrics ------------------------------------------------------------- */
typedef struct ui_font_metrics {
    float ascent;        /* above the baseline, positive */
    float descent;       /* below the baseline, positive */
    float line_gap;
    float line_height;   /* ascent + descent + line_gap */
    float cap_height;
    float x_height;
    float underline_pos;   /* below the baseline, positive = down */
    float underline_size;
    float strike_pos;      /* above the baseline, positive = up */
    float strike_size;
} ui_font_metrics;

/* Metrics at em size size_px (clamped to (0, UI_FONT_MAX_SIZE]). Any thread. */
void ui_font_get_metrics(const ui_font *f, float size_px, ui_font_metrics *m);
/* True when f itself (not its fallbacks) maps cp to a non-empty glyph id.
 * Any thread. */
bool ui_font_has_glyph(const ui_font *f, uint32_t cp);

/* ---- single-line measurement (UI text) -------------------------------------
 * Positions match ui_draw_text exactly: advances and kerning in float pixels,
 * glyphs snapped to quarter pixels. Control characters have zero width. */
float  ui_text_width(ui_font *f, float size_px, const char *s, size_t len);
/* Bytes of s that fit in max_w, ending on a code point boundary. */
size_t ui_text_fit(ui_font *f, float size_px, const char *s, size_t len, float max_w);
/* Caret boundary (byte offset) nearest to x (x = 0 at the text start). */
size_t ui_text_hit(ui_font *f, float size_px, const char *s, size_t len, float x);
/* X of the caret before byte pos (pos is clamped to a boundary). */
float  ui_text_caret_x(ui_font *f, float size_px, const char *s, size_t len, size_t pos);

/* ---- A8 text (Text tool) -------------------------------------------------
 * Multi-line text ('\n' and "\r\n" break lines) laid out from the layout
 * origin, which is the top left of the first line box. Line i has its
 * baseline at ascent + i * line_advance, where line_advance is
 * line_height * line_spacing. Lines are aligned inside the box whose width
 * is the widest line. Coverage is exact-area antialiasing (or pixel-center
 * sampling without UI_TEXT_AA), nonzero winding, combined over glyphs. */
#define UI_TEXT_AA        1u
#define UI_TEXT_BOLD      2u   /* synthetic: outline dilated by ~size/30 */
#define UI_TEXT_ITALIC    4u   /* synthetic: 12 degree shear */
#define UI_TEXT_UNDERLINE 8u
#define UI_TEXT_STRIKE    16u
#define UI_TEXT_NO_KERN   32u

enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_RIGHT = 2 };

typedef struct ui_text_style {
    float    size_px;        /* em size, (0, UI_FONT_MAX_SIZE] */
    uint32_t flags;          /* UI_TEXT_* */
    int      align;          /* UI_ALIGN_* */
    float    line_spacing;   /* multiplier of line_height; 0 means 1 */
} ui_text_style;

typedef struct ui_text_box {
    float w, h;              /* layout box: widest line x line count */
    float ascent, line_advance;
    int   lines;
} ui_text_box;

typedef struct ui_a8 {
    uint8_t *px;             /* owned, malloc'ed; free with ui_a8_free */
    int32_t  w, h, stride;   /* stride in bytes */
    int32_t  x, y;           /* buffer top left in layout coordinates */
} ui_a8;

pc_status ui_text_layout(const ui_font *f, const char *s, size_t len,
                         const ui_text_style *st, ui_text_box *box);
/* Rasterize into a new buffer that covers the ink (and at least one pixel).
 * Empty text yields a 1 x 1 zero buffer. PC_ERR_LIMIT when the buffer would
 * exceed UI_TEXT_MAX_PIXELS. On failure *out is zeroed. */
pc_status ui_text_raster(const ui_font *f, const char *s, size_t len,
                         const ui_text_style *st, ui_a8 *out);
void      ui_a8_free(ui_a8 *a);   /* NULL-safe; zeroes *a */
/* Caret before byte pos in layout coordinates: top x, y and height. */
pc_status ui_text_caret(const ui_font *f, const char *s, size_t len, const ui_text_style *st,
                        size_t pos, float *x, float *y, float *h);
/* Byte offset of the caret position nearest to (x, y). */
size_t    ui_text_hit_point(const ui_font *f, const char *s, size_t len,
                            const ui_text_style *st, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONT_H */
