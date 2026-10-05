/* pc_pattern.c - fill styles: names, the 53 own 8 x 8 hatch bitmaps, paint
 * sources and dropdown previews (lane E2).
 *
 * Every bitmap below was drawn for paint.c from the visual descriptions of
 * the classic hatch styles (TOOLS.md 3.6); none is taken from another
 * implementation. Bit x of a row is pixel x of that row ('#' in the
 * comments). The Percent styles are thresholds of the 8 x 8 Bayer matrix,
 * so each screen contains the lighter ones and spreads its dots evenly. */
#include "pc/pc_pattern.h"

static const char *const k_names[PC_FILL_STYLE_COUNT] = {
    "Solid Color",
    "Horizontal",
    "Vertical",
    "Forward Diagonal",
    "Backward Diagonal",
    "Cross",
    "Diagonal Cross",
    "Percent 05",
    "Percent 10",
    "Percent 20",
    "Percent 25",
    "Percent 30",
    "Percent 40",
    "Percent 50",
    "Percent 60",
    "Percent 70",
    "Percent 75",
    "Percent 80",
    "Percent 90",
    "Light Downward Diagonal",
    "Light Upward Diagonal",
    "Dark Downward Diagonal",
    "Dark Upward Diagonal",
    "Wide Downward Diagonal",
    "Wide Upward Diagonal",
    "Light Vertical",
    "Light Horizontal",
    "Narrow Vertical",
    "Narrow Horizontal",
    "Dark Vertical",
    "Dark Horizontal",
    "Dashed Downward Diagonal",
    "Dashed Upward Diagonal",
    "Dashed Horizontal",
    "Dashed Vertical",
    "Small Confetti",
    "Large Confetti",
    "Zig Zag",
    "Wave",
    "Diagonal Brick",
    "Horizontal Brick",
    "Weave",
    "Plaid",
    "Divot",
    "Dotted Grid",
    "Dotted Diamond",
    "Shingle",
    "Trellis",
    "Sphere",
    "Small Grid",
    "Small Checker Board",
    "Large Checker Board",
    "Outlined Diamond",
    "Solid Diamond"
};

static const uint8_t k_bits[PC_PATTERN_COUNT][8] = {
    { /*  1 Horizontal */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /*  2 Vertical */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
    },
    { /*  3 Forward Diagonal */
        0x01u, /* #....... */
        0x02u, /* .#...... */
        0x04u, /* ..#..... */
        0x08u, /* ...#.... */
        0x10u, /* ....#... */
        0x20u, /* .....#.. */
        0x40u, /* ......#. */
        0x80u, /* .......# */
    },
    { /*  4 Backward Diagonal */
        0x80u, /* .......# */
        0x40u, /* ......#. */
        0x20u, /* .....#.. */
        0x10u, /* ....#... */
        0x08u, /* ...#.... */
        0x04u, /* ..#..... */
        0x02u, /* .#...... */
        0x01u, /* #....... */
    },
    { /*  5 Cross */
        0xFFu, /* ######## */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
    },
    { /*  6 Diagonal Cross */
        0x81u, /* #......# */
        0x42u, /* .#....#. */
        0x24u, /* ..#..#.. */
        0x18u, /* ...##... */
        0x18u, /* ...##... */
        0x24u, /* ..#..#.. */
        0x42u, /* .#....#. */
        0x81u, /* #......# */
    },
    { /*  7 Percent 05 */
        0x11u, /* #...#... */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x10u, /* ....#... */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /*  8 Percent 10 */
        0x11u, /* #...#... */
        0x00u, /* ........ */
        0x04u, /* ..#..... */
        0x00u, /* ........ */
        0x11u, /* #...#... */
        0x00u, /* ........ */
        0x40u, /* ......#. */
        0x00u, /* ........ */
    },
    { /*  9 Percent 20 */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x45u, /* #.#...#. */
        0x00u, /* ........ */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x44u, /* ..#...#. */
        0x00u, /* ........ */
    },
    { /* 10 Percent 25 */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
    },
    { /* 11 Percent 30 */
        0x55u, /* #.#.#.#. */
        0x22u, /* .#...#.. */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x55u, /* #.#.#.#. */
        0x20u, /* .....#.. */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
    },
    { /* 12 Percent 40 */
        0x55u, /* #.#.#.#. */
        0x2Au, /* .#.#.#.. */
        0x55u, /* #.#.#.#. */
        0x88u, /* ...#...# */
        0x55u, /* #.#.#.#. */
        0xA2u, /* .#...#.# */
        0x55u, /* #.#.#.#. */
        0x88u, /* ...#...# */
    },
    { /* 13 Percent 50 */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
    },
    { /* 14 Percent 60 */
        0x77u, /* ###.###. */
        0xAAu, /* .#.#.#.# */
        0x5Du, /* #.###.#. */
        0xAAu, /* .#.#.#.# */
        0x77u, /* ###.###. */
        0xAAu, /* .#.#.#.# */
        0xD5u, /* #.#.#.## */
        0xAAu, /* .#.#.#.# */
    },
    { /* 15 Percent 70 */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xDFu, /* #####.## */
        0xAAu, /* .#.#.#.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xDDu, /* #.###.## */
        0xAAu, /* .#.#.#.# */
    },
    { /* 16 Percent 75 */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
    },
    { /* 17 Percent 80 */
        0xFFu, /* ######## */
        0xBBu, /* ##.###.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
        0xFFu, /* ######## */
        0xBAu, /* .#.###.# */
        0xFFu, /* ######## */
        0xAAu, /* .#.#.#.# */
    },
    { /* 18 Percent 90 */
        0xFFu, /* ######## */
        0xBFu, /* ######.# */
        0xFFu, /* ######## */
        0xEEu, /* .###.### */
        0xFFu, /* ######## */
        0xFBu, /* ##.##### */
        0xFFu, /* ######## */
        0xEEu, /* .###.### */
    },
    { /* 19 Light Downward Diagonal */
        0x11u, /* #...#... */
        0x22u, /* .#...#.. */
        0x44u, /* ..#...#. */
        0x88u, /* ...#...# */
        0x11u, /* #...#... */
        0x22u, /* .#...#.. */
        0x44u, /* ..#...#. */
        0x88u, /* ...#...# */
    },
    { /* 20 Light Upward Diagonal */
        0x88u, /* ...#...# */
        0x44u, /* ..#...#. */
        0x22u, /* .#...#.. */
        0x11u, /* #...#... */
        0x88u, /* ...#...# */
        0x44u, /* ..#...#. */
        0x22u, /* .#...#.. */
        0x11u, /* #...#... */
    },
    { /* 21 Dark Downward Diagonal */
        0x33u, /* ##..##.. */
        0x66u, /* .##..##. */
        0xCCu, /* ..##..## */
        0x99u, /* #..##..# */
        0x33u, /* ##..##.. */
        0x66u, /* .##..##. */
        0xCCu, /* ..##..## */
        0x99u, /* #..##..# */
    },
    { /* 22 Dark Upward Diagonal */
        0xCCu, /* ..##..## */
        0x66u, /* .##..##. */
        0x33u, /* ##..##.. */
        0x99u, /* #..##..# */
        0xCCu, /* ..##..## */
        0x66u, /* .##..##. */
        0x33u, /* ##..##.. */
        0x99u, /* #..##..# */
    },
    { /* 23 Wide Downward Diagonal */
        0x83u, /* ##.....# */
        0x07u, /* ###..... */
        0x0Eu, /* .###.... */
        0x1Cu, /* ..###... */
        0x38u, /* ...###.. */
        0x70u, /* ....###. */
        0xE0u, /* .....### */
        0xC1u, /* #.....## */
    },
    { /* 24 Wide Upward Diagonal */
        0xC1u, /* #.....## */
        0xE0u, /* .....### */
        0x70u, /* ....###. */
        0x38u, /* ...###.. */
        0x1Cu, /* ..###... */
        0x0Eu, /* .###.... */
        0x07u, /* ###..... */
        0x83u, /* ##.....# */
    },
    { /* 25 Light Vertical */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
    },
    { /* 26 Light Horizontal */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 27 Narrow Vertical */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
        0x55u, /* #.#.#.#. */
    },
    { /* 28 Narrow Horizontal */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0xFFu, /* ######## */
        0x00u, /* ........ */
    },
    { /* 29 Dark Vertical */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
    },
    { /* 30 Dark Horizontal */
        0xFFu, /* ######## */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0xFFu, /* ######## */
        0xFFu, /* ######## */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 31 Dashed Downward Diagonal */
        0x11u, /* #...#... */
        0x22u, /* .#...#.. */
        0x44u, /* ..#...#. */
        0x88u, /* ...#...# */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 32 Dashed Upward Diagonal */
        0x88u, /* ...#...# */
        0x44u, /* ..#...#. */
        0x22u, /* .#...#.. */
        0x11u, /* #...#... */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 33 Dashed Horizontal */
        0x0Fu, /* ####.... */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0xF0u, /* ....#### */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 34 Dashed Vertical */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x10u, /* ....#... */
        0x10u, /* ....#... */
        0x10u, /* ....#... */
        0x10u, /* ....#... */
    },
    { /* 35 Small Confetti */
        0x01u, /* #....... */
        0x10u, /* ....#... */
        0x80u, /* .......# */
        0x04u, /* ..#..... */
        0x20u, /* .....#.. */
        0x01u, /* #....... */
        0x08u, /* ...#.... */
        0x40u, /* ......#. */
    },
    { /* 36 Large Confetti */
        0x03u, /* ##...... */
        0x33u, /* ##..##.. */
        0xB0u, /* ....##.# */
        0x02u, /* .#...... */
        0xC6u, /* .##...## */
        0xC4u, /* ..#...## */
        0x40u, /* ......#. */
        0x10u, /* ....#... */
    },
    { /* 37 Zig Zag */
        0x81u, /* #......# */
        0x42u, /* .#....#. */
        0x24u, /* ..#..#.. */
        0x18u, /* ...##... */
        0x81u, /* #......# */
        0x42u, /* .#....#. */
        0x24u, /* ..#..#.. */
        0x18u, /* ...##... */
    },
    { /* 38 Wave */
        0x00u, /* ........ */
        0x0Cu, /* ..##.... */
        0x12u, /* .#..#... */
        0x21u, /* #....#.. */
        0xC0u, /* ......## */
        0x00u, /* ........ */
        0x00u, /* ........ */
        0x00u, /* ........ */
    },
    { /* 39 Diagonal Brick */
        0x80u, /* .......# */
        0x40u, /* ......#. */
        0x20u, /* .....#.. */
        0x10u, /* ....#... */
        0x08u, /* ...#.... */
        0x14u, /* ..#.#... */
        0x22u, /* .#...#.. */
        0x41u, /* #.....#. */
    },
    { /* 40 Horizontal Brick */
        0xFFu, /* ######## */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0x01u, /* #....... */
        0xFFu, /* ######## */
        0x10u, /* ....#... */
        0x10u, /* ....#... */
        0x10u, /* ....#... */
    },
    { /* 41 Weave */
        0x80u, /* .......# */
        0x42u, /* .#....#. */
        0x24u, /* ..#..#.. */
        0x08u, /* ...#.... */
        0x10u, /* ....#... */
        0x24u, /* ..#..#.. */
        0x42u, /* .#....#. */
        0x01u, /* #....... */
    },
    { /* 42 Plaid */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
        0x55u, /* #.#.#.#. */
        0xAAu, /* .#.#.#.# */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
    },
    { /* 43 Divot */
        0x00u, /* ........ */
        0x08u, /* ...#.... */
        0x10u, /* ....#... */
        0x08u, /* ...#.... */
        0x00u, /* ........ */
        0x01u, /* #....... */
        0x80u, /* .......# */
        0x01u, /* #....... */
    },
    { /* 44 Dotted Grid */
        0x55u, /* #.#.#.#. */
        0x00u, /* ........ */
        0x01u, /* #....... */
        0x00u, /* ........ */
        0x01u, /* #....... */
        0x00u, /* ........ */
        0x01u, /* #....... */
        0x00u, /* ........ */
    },
    { /* 45 Dotted Diamond */
        0x00u, /* ........ */
        0x28u, /* ...#.#.. */
        0x00u, /* ........ */
        0x82u, /* .#.....# */
        0x00u, /* ........ */
        0x82u, /* .#.....# */
        0x00u, /* ........ */
        0x28u, /* ...#.#.. */
    },
    { /* 46 Shingle */
        0x81u, /* #......# */
        0x42u, /* .#....#. */
        0x3Cu, /* ..####.. */
        0x00u, /* ........ */
        0x18u, /* ...##... */
        0x24u, /* ..#..#.. */
        0xC3u, /* ##....## */
        0x00u, /* ........ */
    },
    { /* 47 Trellis */
        0x11u, /* #...#... */
        0xAAu, /* .#.#.#.# */
        0x44u, /* ..#...#. */
        0xAAu, /* .#.#.#.# */
        0x11u, /* #...#... */
        0xAAu, /* .#.#.#.# */
        0x44u, /* ..#...#. */
        0xAAu, /* .#.#.#.# */
    },
    { /* 48 Sphere */
        0x06u, /* .##..... */
        0x0Du, /* #.##.... */
        0x0Fu, /* ####.... */
        0x06u, /* .##..... */
        0x60u, /* .....##. */
        0xD0u, /* ....#.## */
        0xF0u, /* ....#### */
        0x60u, /* .....##. */
    },
    { /* 49 Small Grid */
        0xFFu, /* ######## */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0xFFu, /* ######## */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
        0x11u, /* #...#... */
    },
    { /* 50 Small Checker Board */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0xCCu, /* ..##..## */
        0xCCu, /* ..##..## */
        0x33u, /* ##..##.. */
        0x33u, /* ##..##.. */
        0xCCu, /* ..##..## */
        0xCCu, /* ..##..## */
    },
    { /* 51 Large Checker Board */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
        0x0Fu, /* ####.... */
        0xF0u, /* ....#### */
        0xF0u, /* ....#### */
        0xF0u, /* ....#### */
        0xF0u, /* ....#### */
    },
    { /* 52 Outlined Diamond */
        0x10u, /* ....#... */
        0x28u, /* ...#.#.. */
        0x44u, /* ..#...#. */
        0x82u, /* .#.....# */
        0x01u, /* #....... */
        0x82u, /* .#.....# */
        0x44u, /* ..#...#. */
        0x28u, /* ...#.#.. */
    },
    { /* 53 Solid Diamond */
        0x00u, /* ........ */
        0x18u, /* ...##... */
        0x3Cu, /* ..####.. */
        0x7Eu, /* .######. */
        0x7Eu, /* .######. */
        0x3Cu, /* ..####.. */
        0x18u, /* ...##... */
        0x00u, /* ........ */
    },
};

const char *pc_fill_style_name(pc_fill_style s)
{
    if ((unsigned)s >= (unsigned)PC_FILL_STYLE_COUNT) return NULL;
    return k_names[s];
}

const uint8_t *pc_pattern_bits(pc_fill_style s)
{
    if ((unsigned)s == 0u || (unsigned)s >= (unsigned)PC_FILL_STYLE_COUNT) return NULL;
    return k_bits[(unsigned)s - 1u];
}

bool pc_pattern_at(pc_fill_style s, int32_t x, int32_t y)
{
    const uint8_t *b;
    if (s == PC_FILL_SOLID) return true;
    b = pc_pattern_bits(s);
    if (!b) return false;
    /* conversion to unsigned is modulo 2^32, so this is floor mod 8 */
    return ((b[(uint32_t)y & 7u] >> ((uint32_t)x & 7u)) & 1u) != 0u;
}

uint32_t pc_pattern_coverage(pc_fill_style s)
{
    const uint8_t *b = pc_pattern_bits(s);
    uint32_t n = 0;
    if (s == PC_FILL_SOLID) return 64u;
    if (!b) return 0u;
    for (unsigned r = 0; r < 8u; r++)
        for (unsigned c = 0; c < 8u; c++) n += (b[r] >> c) & 1u;
    return n;
}

void pc_pattern_row(pc_fill_style s, pc_px32 fg, pc_px32 bg, int32_t x, int32_t y,
                    int32_t n, pc_px32 *out)
{
    const uint8_t *b = pc_pattern_bits(s);
    uint32_t bits, ph;
    if (!out || n <= 0) return;
    if (!b) {
        for (int32_t i = 0; i < n; i++) out[i] = fg;
        return;
    }
    bits = b[(uint32_t)y & 7u];
    ph = (uint32_t)x & 7u;
    for (int32_t i = 0; i < n; i++) {
        out[i] = ((bits >> ph) & 1u) ? fg : bg;
        ph = (ph + 1u) & 7u;
    }
}

void pc_fill_src_init(pc_fill_src *fs, pc_fill_style style, pc_px32 fg, pc_px32 bg)
{
    if (!fs) return;
    fs->style = style;
    fs->fg = fg;
    fs->bg = bg;
}

static void fill_src_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    const pc_fill_src *fs = (const pc_fill_src *)ud;
    pc_pattern_row(fs->style, fs->fg, fs->bg, x, y, n, out);
}

pc_paint_src pc_fill_src_paint(const pc_fill_src *fs)
{
    pc_paint_src p;
    p.row = NULL;
    p.ud = NULL;
    p.solid.b = 0u; p.solid.g = 0u; p.solid.r = 0u; p.solid.a = 255u;
    if (!fs) return p;
    p.solid = fs->fg;
    if (pc_pattern_bits(fs->style)) {
        p.row = fill_src_row;
        p.ud = (void *)(uintptr_t)fs;     /* the callback only reads it */
    }
    return p;
}

void pc_pattern_thumb(pc_fill_style s, pc_px32 fg, pc_px32 bg, int32_t scale,
                      int32_t w, int32_t h, pc_px32 *dst, size_t stride)
{
    if (!dst || w <= 0 || h <= 0 || stride < (size_t)w) return;
    if (scale < 1) scale = 1;
    for (int32_t y = 0; y < h; y++) {
        pc_px32 *row = dst + (size_t)y * stride;
        int32_t py = y / scale;
        for (int32_t x = 0; x < w; x++)
            row[x] = pc_pattern_at(s, x / scale, py) || !pc_pattern_bits(s) ? fg : bg;
    }
}
