/* pc_pattern.h - fill styles: solid color and the 53 hatch patterns of the
 * Fill dropdown (Paintbrush, Paint Bucket, Line/Curve, Shapes), lane E2.
 *
 * Model. A pattern is an 8 x 8 bitmap repeated over the whole document
 * with its origin at document pixel (0, 0), so pixel (x, y) shows bit
 * (x mod 8, y mod 8) (floor modulo, also for negative coordinates) and
 * every tool that paints with the same style lines up. Set bits show the
 * foreground color, clear bits the background color. Tools pass the
 * primary color as foreground and the secondary as background for the
 * left button, and the two swapped for the right button. Colors are
 * straight alpha and are used as given (a translucent foreground or
 * background stays translucent).
 *
 * The bitmaps are this project's own drawings of the classic hatch looks
 * (lines, grids, dot screens, bricks, confetti, ...); the Percent styles
 * are nested ordered-dither screens with round(N * 64 / 100) set pixels.
 * Order and names follow the Fill dropdown (TOOLS.md 3.6).
 *
 * Thread rules: every function is pure and reentrant; any thread.
 * Ownership: returned strings and bitmaps are static; buffers passed in
 * are borrowed for the duration of the call.
 */
#ifndef PC_PATTERN_H
#define PC_PATTERN_H

#include "pc_paint.h"

typedef enum pc_fill_style {
    PC_FILL_SOLID = 0,
    PC_FILL_HORIZONTAL,
    PC_FILL_VERTICAL,
    PC_FILL_FORWARD_DIAGONAL,
    PC_FILL_BACKWARD_DIAGONAL,
    PC_FILL_CROSS,                    /* "Cross" (large grid) */
    PC_FILL_DIAGONAL_CROSS,
    PC_FILL_PERCENT05,
    PC_FILL_PERCENT10,
    PC_FILL_PERCENT20,
    PC_FILL_PERCENT25,
    PC_FILL_PERCENT30,
    PC_FILL_PERCENT40,
    PC_FILL_PERCENT50,
    PC_FILL_PERCENT60,
    PC_FILL_PERCENT70,
    PC_FILL_PERCENT75,
    PC_FILL_PERCENT80,
    PC_FILL_PERCENT90,
    PC_FILL_LIGHT_DOWNWARD_DIAGONAL,
    PC_FILL_LIGHT_UPWARD_DIAGONAL,
    PC_FILL_DARK_DOWNWARD_DIAGONAL,
    PC_FILL_DARK_UPWARD_DIAGONAL,
    PC_FILL_WIDE_DOWNWARD_DIAGONAL,
    PC_FILL_WIDE_UPWARD_DIAGONAL,
    PC_FILL_LIGHT_VERTICAL,
    PC_FILL_LIGHT_HORIZONTAL,
    PC_FILL_NARROW_VERTICAL,
    PC_FILL_NARROW_HORIZONTAL,
    PC_FILL_DARK_VERTICAL,
    PC_FILL_DARK_HORIZONTAL,
    PC_FILL_DASHED_DOWNWARD_DIAGONAL,
    PC_FILL_DASHED_UPWARD_DIAGONAL,
    PC_FILL_DASHED_HORIZONTAL,
    PC_FILL_DASHED_VERTICAL,
    PC_FILL_SMALL_CONFETTI,
    PC_FILL_LARGE_CONFETTI,
    PC_FILL_ZIG_ZAG,
    PC_FILL_WAVE,
    PC_FILL_DIAGONAL_BRICK,
    PC_FILL_HORIZONTAL_BRICK,
    PC_FILL_WEAVE,
    PC_FILL_PLAID,
    PC_FILL_DIVOT,
    PC_FILL_DOTTED_GRID,
    PC_FILL_DOTTED_DIAMOND,
    PC_FILL_SHINGLE,
    PC_FILL_TRELLIS,
    PC_FILL_SPHERE,
    PC_FILL_SMALL_GRID,
    PC_FILL_SMALL_CHECKER_BOARD,
    PC_FILL_LARGE_CHECKER_BOARD,
    PC_FILL_OUTLINED_DIAMOND,
    PC_FILL_SOLID_DIAMOND,
    PC_FILL_STYLE_COUNT               /* 54: Solid + 53 patterns */
} pc_fill_style;

#define PC_PATTERN_COUNT 53u
#define PC_PATTERN_DIM   8

/* Display name ("Solid Color", "Horizontal", ..., "Solid Diamond"); NULL
 * for an invalid style. */
const char *pc_fill_style_name(pc_fill_style s);

/* The 8 rows of the bitmap of a pattern style: bit x (value 1 << x) of
 * row y is pixel (x, y) of the tile. NULL for PC_FILL_SOLID or an invalid
 * style. */
const uint8_t *pc_pattern_bits(pc_fill_style s);

/* True when document pixel (x, y) shows the foreground. Solid is always
 * foreground; an invalid style is never foreground. */
bool        pc_pattern_at(pc_fill_style s, int32_t x, int32_t y);

/* Number of foreground pixels in one 8 x 8 tile (64 for solid). */
uint32_t    pc_pattern_coverage(pc_fill_style s);

/* Fill n pixels of document row y starting at x with the style's colors.
 * An invalid style fills the foreground (like solid). */
void        pc_pattern_row(pc_fill_style s, pc_px32 fg, pc_px32 bg, int32_t x, int32_t y,
                           int32_t n, pc_px32 *out);

/* ---- paint sources ------------------------------------------------------------
 * Storage behind a pc_paint_src that paints a fill style. The source
 * borrows *fs, which must stay alive and unchanged while pc_paint_apply
 * runs; its row callback is pure and safe for concurrent par workers. */
typedef struct pc_fill_src {
    pc_fill_style style;
    pc_px32       fg, bg;
} pc_fill_src;

void         pc_fill_src_init(pc_fill_src *fs, pc_fill_style style, pc_px32 fg, pc_px32 bg);
/* For PC_FILL_SOLID the result has row == NULL and solid == fg (the fast
 * path of pc_paint_apply); otherwise row/ud point at the pattern. */
pc_paint_src pc_fill_src_paint(const pc_fill_src *fs);

/* ---- dropdown previews ----------------------------------------------------------
 * Render a w x h swatch (dst, stride in pixels) of style s with each
 * pattern pixel magnified to scale x scale screen pixels (scale >= 1;
 * values below 1 count as 1). The swatch starts at pattern phase (0, 0).
 * Pixels are straight alpha; callers that show translucent colors draw a
 * checkerboard behind them. Does nothing for w or h <= 0 or a NULL dst. */
void        pc_pattern_thumb(pc_fill_style s, pc_px32 fg, pc_px32 bg, int32_t scale,
                             int32_t w, int32_t h, pc_px32 *dst, size_t stride);

#endif /* PC_PATTERN_H */
