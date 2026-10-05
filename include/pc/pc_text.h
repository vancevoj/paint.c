/* pc_text.h - font-backend-agnostic text layout, editing and rendering for
 * the Text tool (lane E3).
 *
 * Fonts come from the app through pc_font_face, a table of callbacks
 * (glyph lookup, has-glyph query, metrics, advances, kerning, outlines as
 * pc_path), so the core never parses font files (P-06). The app connects
 * stb_truetype (ADR-004) or any other backend; tests use a synthetic face.
 *
 * Layout model (TOOLS.md 11.1, 3.3; docs TextTool):
 *  - Lines break only at '\n' (Enter); there is no word wrap.
 *  - Each line is aligned relative to the origin (the click point): Left
 *    extends right, Center both ways, Right extends left.
 *  - Line height = ascent + descent + line gap of the primary face. The
 *    vertical anchor defaults to the 3.36 rule: the first line's box is
 *    centered on the origin (PC_TEXT_ANCHOR_LINE_CENTER).
 *  - Em size in pixels: Points = size * image DPI / 72, Fixed (96 DPI) =
 *    size * 96 / 72 (identical at 96 DPI).
 *  - Codepoints the primary face lacks come from the first fallback face
 *    that has them, else the primary face's glyph 0 (.notdef). Kerning
 *    applies between neighbors of the same face. No complex-script shaping
 *    (ADR-004): one glyph per codepoint, combining marks drawn at the pen
 *    position with their own advance.
 *  - Bold and italic are synthesized when the face lacks them: the outline
 *    is sheared by 0.2 (about 11.3 degrees) and emboldened by stroking it
 *    with em/24 (each glyph advance grows by em/24). Underline and
 *    strikeout span each line's advance width.
 *  - Rendering: the glyph outlines (nonzero) plus decorations become one
 *    coverage layer painted with pc_vrender (primary color, blend mode,
 *    antialiasing, selection clipping). Fill styles are not supported for
 *    text (docs), but any pc_paint_src works.
 *  - Color fonts (T-TEXT-COLORFONT, lane TOOLB; docs: "Text tool supports
 *    colored fonts"): a face may describe glyphs as color layers (COLR
 *    version 0: outlines filled with palette colors or the text color) or
 *    as color bitmaps (CBDT / sbix strikes, decoded by the backend). Such
 *    glyphs are painted in their own colors through an image layer on top
 *    of the monochrome coverage (pc_vrender_draw_image); faces without the
 *    callbacks, and glyphs without color data, stay monochrome outlines.
 *    Clusters (emoji ZWJ sequences, flags, keycaps, skin tones) go through
 *    the face's substitute callback, so one ligature glyph can stand for
 *    the whole cluster. Faces are chosen by presentation: an emoji
 *    presentation character (Unicode Emoji_Presentation, or any base
 *    followed by U+FE0F) comes from the first color face that has it, so
 *    emoji show in color even when the chosen font has a monochrome glyph
 *    (as emoji are shown in color everywhere); other characters come from
 *    the primary face, else the first monochrome fallback (U+FE0E asks for
 *    text). The rest of a cluster prefers the base character's face.
 *    Default ignorable code
 *    points (ZWJ, variation selectors, tags...) that the chosen face lacks
 *    take no space and draw nothing.
 *  - Sharp modes are hinted by the engine itself (pc_text_hint.c): both
 *    fit the outline vertically to the pixel grid (baseline, x-height and
 *    cap-height zones, horizontal stems); Sharp (Classic) also fits
 *    vertical stems horizontally, like GDI hinting. See pc_text_hint_outline.
 *
 * Editing model: the text is UTF-8 (always valid after insertion) with a
 * caret and a selection anchor as byte offsets on caret stops. Caret stops
 * are cluster boundaries: a base character plus following combining
 * marks, variation selectors, emoji modifiers, ZWJ sequences, and pairs
 * of regional indicators. Word movement and deletion follow the usual
 * word processor rules (Ctrl+Left/Right, Ctrl+Backspace/Delete).
 *
 * Thread rules: a pc_text belongs to one thread (the main thread); its
 * font callbacks run on that thread, synchronously inside pc_text calls,
 * never concurrently. Rendering hands pure per-tile work to par workers
 * through pc_paint_apply (pc_shapes.h).
 * Ownership: pc_text owns its text, layout and glyph cache. Font faces
 * (and their ud) are borrowed and must outlive the pc_text or the next
 * pc_text_set_fonts. Pointers returned by queries are borrowed until the
 * next mutating call.
 */
#ifndef PC_TEXT_H
#define PC_TEXT_H

#include "pc_shapes.h"

/* Hard cap on the text size (P-08). */
#define PC_TEXT_MAX_BYTES ((size_t)1u << 20)

/* ---- font backend ---------------------------------------------------------------- */

/* Text rendering modes (TOOLS.md 3.3). The engine passes the mode to the
 * backend, which may hint outlines and advances for the Sharp modes. The
 * layout itself puts glyphs on whole pixels in the Sharp modes: Classic
 * rounds every advance (GDI style), Modern rounds each pen position of the
 * fractional layout. */
typedef enum pc_text_mode {
    PC_TEXT_SMOOTH = 0,          /* "Smooth": unhinted outlines */
    PC_TEXT_SHARP_MODERN = 1,    /* "Sharp (Modern)": natural symmetric hinting */
    PC_TEXT_SHARP_CLASSIC = 2,   /* "Sharp (Classic)": GDI-like hinting */
    PC_TEXT_MODE_COUNT = 3
} pc_text_mode;

/* Face metrics in pixels for a given em size. Zero decoration fields ask
 * for the engine's defaults (underline 0.1 em below the baseline,
 * strikeout 0.3 em above it, thickness max(1, em / 14)). */
typedef struct pc_font_metrics {
    double ascent;               /* > 0: baseline to the line top */
    double descent;              /* >= 0: baseline to the line bottom */
    double line_gap;             /* >= 0: extra space between lines */
    double underline_offset;     /* baseline to the underline center, > 0 below */
    double underline_thickness;
    double strike_offset;        /* baseline to the strikeout center, < 0 above */
    double strike_thickness;
    /* Alignment zones for the Sharp modes (lane TOOLB), pixels above the
     * baseline, 0 = unknown (the engine then measures 'x' and 'H'). */
    double x_height;
    double cap_height;
} pc_font_metrics;

/* One layer of a layered color glyph (OpenType COLR version 0): the
 * outline of gid (same face) filled with color. Lane TOOLB. */
typedef struct pc_font_color_layer {
    uint32_t gid;
    pc_px32  color;              /* straight alpha */
    bool     foreground;         /* use the text color (CPAL index 0xFFFF) */
} pc_font_color_layer;

/* A bitmap color glyph (CBDT or sbix strike, PNG decoded by the backend).
 * Lane TOOLB. */
typedef struct pc_font_bitmap {
    const pc_px32 *px;           /* w x h straight alpha, row stride w; borrowed
                                    until the next call on the face */
    int32_t        w, h;
    double         scale;        /* pixels at the requested em per bitmap pixel */
    double         left;         /* left edge relative to the pen, pixels */
    double         top;          /* top edge relative to the baseline, pixels,
                                    y down (negative above the baseline) */
} pc_font_bitmap;

/* Most layers of one color glyph the engine draws (more are ignored). */
#define PC_TEXT_MAX_COLOR_LAYERS 1024u

typedef struct pc_font_face {
    void *ud;
    /* Glyph index of codepoint cp, 0 when the face has no glyph for it. */
    uint32_t  (*glyph)(void *ud, uint32_t cp);
    /* Optional "has glyph" query used for fallback; NULL means
     * glyph(ud, cp) != 0. */
    bool      (*has_glyph)(void *ud, uint32_t cp);
    /* Metrics at em pixels per em. NULL or ascent <= 0 gives ascent
     * 0.8 em, descent 0.2 em, no line gap. */
    void      (*metrics)(void *ud, double em, pc_font_metrics *out);
    /* Horizontal advance of glyph gid in pixels. */
    double    (*advance)(void *ud, uint32_t gid, double em, pc_text_mode mode);
    /* Optional kerning adjustment between two glyphs in pixels (added to
     * the left glyph's advance). */
    double    (*kerning)(void *ud, uint32_t left, uint32_t right, double em);
    /* Append the outline of gid to out: pen at (0, 0) on the baseline, y
     * down, pixels, closed subpaths, filled with the nonzero rule. May be
     * NULL (nothing drawn). Errors abort the render with that status. */
    pc_status (*outline)(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out);
    bool bold;                   /* the face is bold: no synthetic emboldening */
    bool italic;                 /* the face is italic: no synthetic slant */
    /* ---- optional color font support (lane TOOLB); a zeroed tail keeps a
     * face monochrome ---- */
    bool color;                  /* the face has color glyphs (fallback choice) */
    /* COLR: write up to cap layers of gid (bottom first) to out and return
     * how many the glyph has; 0 = not a layered color glyph. */
    size_t    (*color_layers)(void *ud, uint32_t gid, pc_font_color_layer *out, size_t cap);
    /* Color bitmap of gid for em pixels per em: PC_OK with *out filled,
     * PC_ERR_UNSUPPORTED when gid has none (the outline is used), any
     * other error aborts the render with that status. */
    pc_status (*color_bitmap)(void *ud, uint32_t gid, double em, pc_font_bitmap *out);
    /* Substitute the glyphs of one cluster (OpenType ligatures: emoji ZWJ
     * sequences, flags, keycaps, skin tones) in place; returns the new
     * count (1..n; any other value means no change). n >= 2. */
    size_t    (*substitute)(void *ud, uint32_t *gids, size_t n);
} pc_font_face;

/* ---- style ---------------------------------------------------------------------------- */

typedef enum pc_text_align {
    PC_TEXT_LEFT = 0,
    PC_TEXT_CENTER = 1,
    PC_TEXT_RIGHT = 2
} pc_text_align;

typedef enum pc_text_unit {
    PC_TEXT_POINTS = 0,          /* "Points (image DPI)" */
    PC_TEXT_FIXED96 = 1          /* "Fixed (96 DPI)" */
} pc_text_unit;

typedef enum pc_text_anchor {
    PC_TEXT_ANCHOR_LINE_CENTER = 0,  /* first line's box centered on the origin */
    PC_TEXT_ANCHOR_TOP = 1,          /* first line's top at the origin */
    PC_TEXT_ANCHOR_BASELINE = 2      /* first baseline at the origin */
} pc_text_anchor;

typedef struct pc_text_style {
    double         size;         /* font size in `unit` (decimals allowed) */
    pc_text_unit   unit;
    double         dpi;          /* image DPI (PC_TEXT_POINTS) */
    bool           bold, italic, underline, strikeout;
    pc_text_align  align;
    pc_text_mode   mode;
    pc_text_anchor anchor;
    bool           snap;         /* round each line's start and baseline to
                                    whole pixels */
} pc_text_style;

/* 12, Points, 96 DPI, no styles, Left, Smooth, line-center anchor, snap. */
void      pc_text_style_default(pc_text_style *st);
/* Em size in pixels for st, 0 when st is invalid. Any thread. */
double    pc_text_em_pixels(const pc_text_style *st);

/* ---- layout results -------------------------------------------------------------------- */

typedef struct pc_text_line {
    size_t byte_start, byte_end;     /* [start, end), the '\n' excluded */
    size_t glyph_start, glyph_end;   /* into pc_text_glyphs */
    double x;                        /* pen start (left edge of the advance box) */
    double width;                    /* advance width */
    double top, baseline, bottom;
} pc_text_line;

typedef struct pc_text_glyph {
    uint32_t cp;                     /* codepoint */
    uint32_t gid;                    /* glyph index in its face */
    uint32_t face;                   /* index into the face list */
    bool     cluster_start;          /* false for marks joined to the glyph before */
    size_t   byte;                   /* byte offset of cp */
    double   x, y;                   /* pen position: left, on the baseline */
    double   advance;                /* including kerning and synthetic bold */
    bool     hidden;                 /* draws nothing, advance 0: a default
                                        ignorable the face lacks, or a code
                                        point merged into a ligature (TOOLB) */
} pc_text_glyph;

/* ---- the text object ------------------------------------------------------------------ */

typedef struct pc_text pc_text;

pc_text  *pc_text_create(void);                 /* empty, default style; NULL on OOM */
void      pc_text_destroy(pc_text *t);          /* NULL-safe */

/* faces[0] is the primary face, faces[1..n) fallbacks in order (n <= 16).
 * The array is copied; the faces are borrowed. n == 0 clears the fonts
 * (nothing is drawn). PC_ERR_ARG for NULL faces or missing glyph or
 * advance callbacks. Clears the glyph cache and re-lays the text out. */
pc_status pc_text_set_fonts(pc_text *t, const pc_font_face *const *faces, size_t n);
/* PC_ERR_ARG when the em size is not in [0.1, 10000] px or an enum is out
 * of range (the style is then unchanged). Re-lays the text out. */
pc_status pc_text_set_style(pc_text *t, const pc_text_style *st);
const pc_text_style *pc_text_get_style(const pc_text *t);
/* Move the text block (T-TEXT-NUB); non-finite points are ignored. */
void      pc_text_set_origin(pc_text *t, pc_pt origin);
pc_pt     pc_text_origin(const pc_text *t);

/* Replace the whole text (normalized like pc_text_insert); caret at the
 * end. PC_ERR_LIMIT beyond PC_TEXT_MAX_BYTES, PC_ERR_NOMEM (unchanged). */
pc_status pc_text_set_utf8(pc_text *t, const char *s, size_t n);
/* The text, NUL-terminated (len may be NULL). */
const char *pc_text_utf8(const pc_text *t, size_t *len);
bool      pc_text_is_empty(const pc_text *t);

/* ---- caret, selection and editing -------------------------------------------------------- */

size_t    pc_text_caret(const pc_text *t);
size_t    pc_text_sel_anchor(const pc_text *t); /* equals the caret when nothing is selected */
bool      pc_text_has_selection(const pc_text *t);
/* Move the caret to the caret stop at or before index (clamped to the
 * text); extend keeps the anchor (Shift), otherwise the selection is
 * cleared. */
void      pc_text_set_caret(pc_text *t, size_t index, bool extend);
void      pc_text_select_all(pc_text *t);

/* Insert UTF-8 at the caret, replacing the selection; the caret ends after
 * the insertion. Normalization: invalid sequences become U+FFFD, CR LF
 * and lone CR become '\n', a tab becomes a space, other C0/C1 controls
 * and DEL are dropped. PC_ERR_LIMIT when the result would exceed
 * PC_TEXT_MAX_BYTES, PC_ERR_NOMEM; the text is unchanged on error. */
pc_status pc_text_insert(pc_text *t, const char *utf8, size_t n);
/* Backspace / Delete: remove the selection, or the cluster before / after
 * the caret, or with word the span to the previous word start / next word
 * start (Ctrl). Return PC_OK also when there was nothing to remove. */
pc_status pc_text_backspace(pc_text *t, bool word);
pc_status pc_text_delete(pc_text *t, bool word);

typedef enum pc_text_move {
    PC_TEXT_MOVE_LEFT = 0,
    PC_TEXT_MOVE_RIGHT,
    PC_TEXT_MOVE_WORD_LEFT,          /* Ctrl+Left: previous word start */
    PC_TEXT_MOVE_WORD_RIGHT,         /* Ctrl+Right: next word start */
    PC_TEXT_MOVE_HOME,               /* line start */
    PC_TEXT_MOVE_END,                /* line end */
    PC_TEXT_MOVE_UP,                 /* previous line at the remembered x */
    PC_TEXT_MOVE_DOWN,
    PC_TEXT_MOVE_DOC_START,          /* Ctrl+Home */
    PC_TEXT_MOVE_DOC_END             /* Ctrl+End */
} pc_text_move;

/* Caret movement; extend (Shift) keeps the anchor. Left / Right with a
 * selection and no extend collapse to the selection's edge. Up on the first
 * line and Down on the last line do not move (3.36 behavior). */
void      pc_text_move_caret(pc_text *t, pc_text_move m, bool extend);

/* ---- layout queries ------------------------------------------------------------------------ */

const pc_text_line  *pc_text_lines(const pc_text *t, size_t *n);    /* n >= 1 */
const pc_text_glyph *pc_text_glyphs(const pc_text *t, size_t *n);
double    pc_text_line_height(const pc_text *t);
/* Box of all line advance boxes (empty lines count with zero width). */
void      pc_text_bounds(const pc_text *t, pc_box *out);
/* Caret at byte index (snapped to a stop): x0 == x1 = caret x, y0 / y1 =
 * the line's top and bottom. */
void      pc_text_caret_box(const pc_text *t, size_t index, pc_box *out);
/* Caret stop nearest to document point p: the line under p (clamped to
 * the first and last line), then the stop whose x is nearest (a click on
 * the right half of a character lands after it). */
size_t    pc_text_hit_index(const pc_text *t, pc_pt p);
/* Selection highlight boxes, one per line touched by [anchor, caret)
 * (lines selected through their end get a small extra width for the
 * newline). Writes at most cap boxes, returns the number needed. */
size_t    pc_text_selection_boxes(const pc_text *t, pc_box *out, size_t cap);
/* Move handle (T-TEXT-NUB): offset below and right of the caret's bottom. */
pc_pt     pc_text_handle_pos(const pc_text *t, double offset);

typedef enum pc_text_part {
    PC_TEXT_PART_NONE = 0,           /* elsewhere: a click commits the text */
    PC_TEXT_PART_HANDLE,             /* the move handle */
    PC_TEXT_PART_INSIDE              /* inside the text bounds grown by the
                                        hit radius: place the caret */
} pc_text_part;
pc_text_part pc_text_hit_test(const pc_text *t, pc_pt p, const pc_handle_metrics *m);

/* ---- geometry and rendering ---------------------------------------------------------------- */

/* Append the coverage geometry (glyph outlines with synthetic styles,
 * underline, strikeout) in document coordinates to out, for the nonzero
 * rule. Glyphs drawn in color (pc_text_render) are not part of it. Glyph
 * outlines are cached per face and glyph until the fonts or the style
 * change. Backend outline and color errors are returned. */
pc_status pc_text_build(pc_text *t, pc_poly *out);

/* Number of glyphs of the current layout that render in color (layers or
 * a bitmap). Backend errors give 0. Main thread. */
size_t    pc_text_color_glyph_count(pc_text *t);

/* Render through vr into layer_id of tx with src (the primary color; NULL
 * = opaque black). Color glyphs paint their own colors above the outline
 * coverage (layers with the foreground flag use src: its solid color, or
 * for a row source the color at the glyph's pen position). Empty text
 * clears what vr painted before. dirty as pc_vrender_draw. */
pc_status pc_text_render(pc_text *t, pc_vrender *vr, pc_txn *tx, uint32_t layer_id,
                         const pc_paint_src *src, const pc_vdraw_opts *o, const pc_par *par,
                         pc_rect *dirty);

/* Grid-fit a glyph outline for a Sharp mode (lane TOOLB, pc_text_hint.c;
 * the engine calls it for every outline of a non-color face). p is in
 * pixels at em pixels per em, y down, pen at the origin, baseline y = 0.
 * x_height and cap_height are the alignment zones in pixels above the
 * baseline (0 = none). PC_TEXT_SHARP_MODERN fits y only (natural symmetric
 * hinting), PC_TEXT_SHARP_CLASSIC x and y (GDI-like), PC_TEXT_SMOOTH
 * leaves p unchanged. Paths with arcs are left unchanged, and so is p on
 * OOM. Any thread (pure; p is modified in place). */
void      pc_text_hint_outline(pc_path *p, pc_text_mode mode, double em, double x_height,
                               double cap_height);

#endif /* PC_TEXT_H */
