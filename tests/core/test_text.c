/* test_text.c - the text engine with a synthetic font backend (box
 * glyphs, a hole, kerning, a combining mark, .notdef, a fallback face):
 * units, normalization, layout metrics and alignment, caret stops and
 * clusters, caret mapping round trips, word movement and deletion,
 * selection boxes, synthetic bold and italic, decorations, rendering
 * through transactions, commit + undo fingerprints and leaks. */
#include "test_shapes_util.h"
#include "pc/pc_text.h"

/* ---- synthetic font ------------------------------------------------------------- */

typedef struct box_font {
    bool ccw;            /* emit outer contours counter-clockwise (CFF style) */
    int  outline_calls;
    bool fail;           /* outline returns PC_ERR_FORMAT */
} box_font;

static uint32_t a_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    if (cp >= 0x20u && cp <= 0x7Eu) return cp;
    if (cp == 0x301u) return 300u;
    return 0u;
}

static void a_metrics(void *ud, double em, pc_font_metrics *m)
{
    (void)ud;
    memset(m, 0, sizeof *m);
    m->ascent = 0.8 * em;
    m->descent = 0.2 * em;
    m->line_gap = 0.1 * em;
}

static double a_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    (void)ud;
    (void)mode;
    if (gid == 0u) return 0.5 * em;
    if (gid == ' ') return 0.3 * em;
    if (gid == 300u) return 0.0;
    if (gid == 'W' || gid == 'M') return 0.9 * em;
    return 0.6 * em;
}

static double a_kern(void *ud, uint32_t l, uint32_t r, double em)
{
    (void)ud;
    if ((l == 'A' && r == 'V') || (l == 'V' && r == 'A')) return -0.1 * em;
    return 0.0;
}

/* rectangle in em units, clockwise on screen unless ccw */
static pc_status rect_em(pc_path *p, double em, double x0, double y0, double x1, double y1,
                         bool ccw)
{
    pc_status st = pc_path_move_to(p, x0 * em, y0 * em);
    if (!ccw) {
        if (st == PC_OK) st = pc_path_line_to(p, x1 * em, y0 * em);
        if (st == PC_OK) st = pc_path_line_to(p, x1 * em, y1 * em);
        if (st == PC_OK) st = pc_path_line_to(p, x0 * em, y1 * em);
    } else {
        if (st == PC_OK) st = pc_path_line_to(p, x0 * em, y1 * em);
        if (st == PC_OK) st = pc_path_line_to(p, x1 * em, y1 * em);
        if (st == PC_OK) st = pc_path_line_to(p, x1 * em, y0 * em);
    }
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

static pc_status a_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    box_font *f = (box_font *)ud;
    bool ccw = f->ccw;
    (void)mode;
    f->outline_calls++;
    if (f->fail) return PC_ERR_FORMAT;
    if (gid == ' ') return PC_OK;
    if (gid == 0u) {                              /* hollow .notdef box */
        pc_status st = rect_em(out, em, 0.05, -0.7, 0.45, 0.0, ccw);
        if (st == PC_OK) st = rect_em(out, em, 0.1, -0.65, 0.4, -0.05, !ccw);
        return st;
    }
    if (gid == 300u) return rect_em(out, em, -0.4, -0.9, -0.2, -0.8, ccw);
    if (gid == 'I') return rect_em(out, em, 0.25, -0.7, 0.35, 0.0, ccw);
    if (gid == 'O') {
        pc_status st = rect_em(out, em, 0.1, -0.7, 0.5, 0.0, ccw);
        if (st == PC_OK) st = rect_em(out, em, 0.2, -0.6, 0.4, -0.1, !ccw);
        return st;
    }
    if (gid >= 'a' && gid <= 'z') return rect_em(out, em, 0.1, -0.5, 0.5, 0.0, ccw);
    if ((gid >= 'A' && gid <= 'Z') || (gid >= '0' && gid <= '9'))
        return rect_em(out, em, 0.1, -0.7, 0.5, 0.0, ccw);
    return rect_em(out, em, 0.2, -0.2, 0.4, 0.0, ccw);   /* punctuation */
}

/* fallback face: only U+0416 and U+20AC, triangles, different metrics */
static uint32_t b_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    return cp == 0x416u ? 1u : (cp == 0x20ACu ? 2u : 0u);
}

static void b_metrics(void *ud, double em, pc_font_metrics *m)
{
    (void)ud;
    memset(m, 0, sizeof *m);
    m->ascent = 2.0 * em;
    m->descent = 1.0 * em;
}

static double b_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    (void)ud;
    (void)gid;
    (void)mode;
    return 0.7 * em;
}

static pc_status b_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    pc_status st;
    (void)ud;
    (void)gid;
    (void)mode;
    st = pc_path_move_to(out, 0.1 * em, 0.0);
    if (st == PC_OK) st = pc_path_line_to(out, 0.35 * em, -0.7 * em);
    if (st == PC_OK) st = pc_path_line_to(out, 0.6 * em, 0.0);
    if (st == PC_OK) st = pc_path_close(out);
    return st;
}

static box_font g_fa, g_fa_ccw;
static pc_font_face g_face_a, g_face_a_ccw, g_face_b;

static void init_fonts(void)
{
    memset(&g_face_a, 0, sizeof g_face_a);
    g_face_a.ud = &g_fa;
    g_face_a.glyph = a_glyph;
    g_face_a.metrics = a_metrics;
    g_face_a.advance = a_advance;
    g_face_a.kerning = a_kern;
    g_face_a.outline = a_outline;
    g_face_a_ccw = g_face_a;
    g_face_a_ccw.ud = &g_fa_ccw;
    g_fa_ccw.ccw = true;
    memset(&g_face_b, 0, sizeof g_face_b);
    g_face_b.glyph = b_glyph;
    g_face_b.metrics = b_metrics;
    g_face_b.advance = b_advance;
    g_face_b.outline = b_outline;
}

/* Text with em 20 px (15 pt fixed), snap off, anchor top, origin (100, 50). */
static pc_text *make_text(const char *s, pc_text_align al)
{
    pc_text *t = pc_text_create();
    const pc_font_face *faces[2] = { &g_face_a, &g_face_b };
    pc_text_style st;
    if (!t) return NULL;
    pc_text_style_default(&st);
    st.size = 15.0;
    st.unit = PC_TEXT_FIXED96;
    st.snap = false;
    st.anchor = PC_TEXT_ANCHOR_TOP;
    st.align = al;
    CHECK(pc_text_set_fonts(t, faces, 2) == PC_OK);
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(100, 50));
    CHECK(pc_text_set_utf8(t, s, strlen(s)) == PC_OK);
    return t;
}

static bool same_str(const pc_text *t, const char *want)
{
    size_t n;
    const char *s = pc_text_utf8(t, &n);
    return n == strlen(want) && memcmp(s, want, n) == 0 && s[n] == '\0';
}

/* ---- tests ---------------------------------------------------------------------------- */

static void t_units(void)
{
    pc_text_style st;
    pc_text_style_default(&st);
    CHECK(st.size == 12.0 && st.unit == PC_TEXT_POINTS && st.dpi == 96.0 && st.snap);
    CHECK(e3_near(pc_text_em_pixels(&st), 16.0, 1e-12));
    st.dpi = 72.0;
    CHECK(e3_near(pc_text_em_pixels(&st), 12.0, 1e-12));
    st.dpi = 300.0;
    CHECK(e3_near(pc_text_em_pixels(&st), 50.0, 1e-12));
    st.unit = PC_TEXT_FIXED96;
    CHECK(e3_near(pc_text_em_pixels(&st), 16.0, 1e-12));
    st.size = 18.3;
    CHECK(e3_near(pc_text_em_pixels(&st), 18.3 * 4.0 / 3.0, 1e-12));
    st.size = -1.0;
    CHECK(pc_text_em_pixels(&st) == 0.0);
    st.size = 12.0;
    st.unit = PC_TEXT_POINTS;
    st.dpi = 0.0;
    CHECK(pc_text_em_pixels(&st) == 0.0 && pc_text_em_pixels(NULL) == 0.0);
}

static void t_normalize(void)
{
    pc_text *t = pc_text_create();
    static const char in[] = "a\r\nb\rc\td\x01" "e\x7f" "f\xc2\x85g";
    CHECK(t != NULL);
    CHECK(pc_text_set_utf8(t, in, sizeof in - 1u) == PC_OK);
    CHECK(same_str(t, "a\nb\nc defg"));
    CHECK(pc_text_set_utf8(t, "\xff" "A" "\xc0\xaf" "\xed\xa0\x80" "\xf0\x9f\x98\x80", 11) ==
          PC_OK);
    CHECK(same_str(t, "\xef\xbf\xbd" "A" "\xef\xbf\xbd\xef\xbf\xbd"
                      "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" "\xf0\x9f\x98\x80"));
    CHECK(pc_text_set_utf8(t, "\xe2\x82", 2) == PC_OK);     /* truncated sequence */
    CHECK(same_str(t, "\xef\xbf\xbd\xef\xbf\xbd"));
    CHECK(pc_text_set_utf8(t, "\xf5\x80\x80\x80", 4) == PC_OK);
    CHECK(same_str(t, "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));
    CHECK(pc_text_set_utf8(t, NULL, 0) == PC_OK && pc_text_is_empty(t));
    CHECK(pc_text_set_utf8(t, NULL, 3) == PC_ERR_ARG);
    {
        size_t big = PC_TEXT_MAX_BYTES + 1u;
        char *b = (char *)malloc(big);
        CHECK(b != NULL);
        if (b) {
            memset(b, 'x', big);
            CHECK(pc_text_set_utf8(t, b, big) == PC_ERR_LIMIT);
            CHECK(pc_text_set_utf8(t, b, big - 1u) == PC_OK);
            CHECK(pc_text_insert(t, "y", 1) == PC_ERR_LIMIT);
            CHECK(pc_text_backspace(t, false) == PC_OK);
            CHECK(pc_text_insert(t, "y", 1) == PC_OK);
            CHECK(pc_text_utf8(t, NULL)[PC_TEXT_MAX_BYTES - 1u] == 'y');
            free(b);
        }
    }
    pc_text_destroy(t);
}

static void t_layout(void)
{
    pc_text *t = make_text("AB\nC", PC_TEXT_LEFT);
    const pc_text_line *L;
    const pc_text_glyph *g;
    pc_text_style st;
    size_t n, ng;
    pc_box b;
    L = pc_text_lines(t, &n);
    g = pc_text_glyphs(t, &ng);
    CHECK(n == 2u && ng == 3u);
    CHECK(e3_near(pc_text_line_height(t), 22.0, 1e-12));
    CHECK(L[0].byte_start == 0u && L[0].byte_end == 2u);
    CHECK(L[1].byte_start == 3u && L[1].byte_end == 4u);
    CHECK(e3_near(L[0].x, 100.0, 1e-12) && e3_near(L[0].width, 24.0, 1e-12));
    CHECK(e3_near(L[0].top, 50.0, 1e-12) && e3_near(L[0].baseline, 66.0, 1e-12));
    CHECK(e3_near(L[0].bottom, 72.0, 1e-12) && e3_near(L[1].baseline, 88.0, 1e-12));
    CHECK(e3_near(g[1].x, 112.0, 1e-12) && e3_near(g[2].x, 100.0, 1e-12) && g[2].y == 88.0);
    pc_text_bounds(t, &b);
    CHECK(b.x0 == 100.0 && b.x1 == 124.0 && b.y0 == 50.0 && b.y1 == 94.0);
    /* alignment relative to the origin */
    st = *pc_text_get_style(t);
    st.align = PC_TEXT_CENTER;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    L = pc_text_lines(t, NULL);
    CHECK(e3_near(L[0].x, 88.0, 1e-12) && e3_near(L[1].x, 94.0, 1e-12));
    st.align = PC_TEXT_RIGHT;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    L = pc_text_lines(t, NULL);
    CHECK(e3_near(L[0].x, 76.0, 1e-12) && e3_near(L[1].x, 88.0, 1e-12));
    /* vertical anchors */
    st.align = PC_TEXT_LEFT;
    st.anchor = PC_TEXT_ANCHOR_LINE_CENTER;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].top, 39.0, 1e-12));
    st.anchor = PC_TEXT_ANCHOR_BASELINE;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].baseline, 50.0, 1e-12));
    /* snapping rounds line starts and baselines */
    st.snap = true;
    st.align = PC_TEXT_CENTER;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(100.4, 50.3));
    L = pc_text_lines(t, NULL);
    CHECK(L[0].x == 88.0 && L[1].x == 94.0 && L[0].baseline == 50.0 && L[1].baseline == 72.0);
    /* kerning, wide glyphs, spaces and synthetic bold advances */
    st.snap = false;
    st.align = PC_TEXT_LEFT;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(pc_text_set_utf8(t, "AVA W", 5) == PC_OK);
    L = pc_text_lines(t, NULL);
    g = pc_text_glyphs(t, NULL);
    CHECK(e3_near(L[0].width, 10.0 + 10.0 + 12.0 + 6.0 + 18.0, 1e-12));
    CHECK(e3_near(g[1].x - g[0].x, 10.0, 1e-12) && e3_near(g[0].advance, 10.0, 1e-12));
    st.bold = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].width, 56.0 + 5.0 * 20.0 / 24.0, 1e-12));
    st.bold = false;
    /* fallback faces, .notdef and the primary metrics */
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(pc_text_set_utf8(t, "A\xd0\x96\xe2\x82\xac\xe4\xb8\x80", 9) == PC_OK);
    g = pc_text_glyphs(t, &ng);
    CHECK(ng == 4u && g[0].face == 0u && g[1].face == 1u && g[1].gid == 1u);
    CHECK(g[2].face == 1u && g[2].gid == 2u && g[3].face == 0u && g[3].gid == 0u);
    CHECK(g[1].byte == 1u && g[2].byte == 3u && g[3].byte == 6u);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].width, 12.0 + 14.0 + 14.0 + 10.0, 1e-12));
    CHECK(e3_near(pc_text_line_height(t), 22.0, 1e-12));
    /* Sharp modes: whole-pixel glyph positions (em 20.5 px: advances 12.3) */
    st.size = 20.5 * 0.75;
    st.mode = PC_TEXT_SHARP_CLASSIC;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(pc_text_set_utf8(t, "ABCD", 4) == PC_OK);
    g = pc_text_glyphs(t, NULL);
    L = pc_text_lines(t, NULL);
    CHECK(g[1].x - g[0].x == 12.0 && g[3].x - g[0].x == 36.0 && L[0].width == 48.0);
    st.mode = PC_TEXT_SHARP_MODERN;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    g = pc_text_glyphs(t, NULL);
    L = pc_text_lines(t, NULL);
    CHECK(g[1].x - g[0].x == 12.0 && g[2].x - g[0].x == 25.0 && g[3].x - g[0].x == 37.0);
    CHECK(L[0].width == 49.0 && g[2].advance == 12.0 && g[1].advance == 13.0);
    st.mode = PC_TEXT_SMOOTH;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(pc_text_lines(t, NULL)[0].width, 4.0 * 12.3, 1e-9));
    st.size = 15.0;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    /* trailing newline: an empty last line */
    CHECK(pc_text_set_utf8(t, "A\n", 2) == PC_OK);
    L = pc_text_lines(t, &n);
    CHECK(n == 2u && L[1].byte_start == 2u && L[1].byte_end == 2u && L[1].width == 0.0);
    /* style validation */
    st.size = 0.01;
    CHECK(pc_text_set_style(t, &st) == PC_ERR_ARG);
    st.size = 15.0;
    st.align = (pc_text_align)7;
    CHECK(pc_text_set_style(t, &st) == PC_ERR_ARG);
    CHECK(pc_text_get_style(t)->size == 15.0 && pc_text_get_style(t)->align == PC_TEXT_LEFT);
    {
        const pc_font_face *bad[1] = { NULL };
        pc_font_face nf = g_face_a;
        const pc_font_face *bad2[1] = { &nf };
        nf.advance = NULL;
        CHECK(pc_text_set_fonts(t, bad, 1) == PC_ERR_ARG);
        CHECK(pc_text_set_fonts(t, bad2, 1) == PC_ERR_ARG);
        CHECK(pc_text_set_fonts(t, NULL, 0) == PC_OK);
        CHECK(pc_text_lines(t, NULL)[0].width == 0.0 && pc_text_line_height(t) == 20.0);
    }
    pc_text_destroy(t);
}

static void t_clusters(void)
{
    pc_text *t = make_text("e\xcc\x81x", PC_TEXT_LEFT);   /* e + U+0301 + x */
    const pc_text_glyph *g;
    size_t ng;
    g = pc_text_glyphs(t, &ng);
    CHECK(ng == 3u && g[0].cluster_start && !g[1].cluster_start && g[2].cluster_start);
    CHECK(g[1].advance == 0.0 && e3_near(g[2].x, 112.0, 1e-12));
    pc_text_set_caret(t, 0, false);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 3u);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 4u);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 4u);
    pc_text_set_caret(t, 2, false);                          /* inside the cluster */
    CHECK(pc_text_caret(t) == 0u);
    pc_text_set_caret(t, 3, false);
    CHECK(pc_text_backspace(t, false) == PC_OK && same_str(t, "x"));
    /* flags (regional indicator pairs) and ZWJ sequences are one stop each */
    CHECK(pc_text_set_utf8(t, "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xf0\x9f\x87\xab"
                              "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb!", 24) == PC_OK);
    pc_text_set_caret(t, 0, false);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 8u);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 12u);                          /* a lone indicator */
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 23u);
    pc_text_move_caret(t, PC_TEXT_MOVE_LEFT, false);
    CHECK(pc_text_caret(t) == 12u);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOC_END, false);
    CHECK(pc_text_delete(t, false) == PC_OK && pc_text_caret(t) == 24u);
    pc_text_move_caret(t, PC_TEXT_MOVE_LEFT, false);
    pc_text_move_caret(t, PC_TEXT_MOVE_LEFT, false);
    CHECK(pc_text_delete(t, false) == PC_OK && pc_text_caret(t) == 12u);
    CHECK(same_str(t, "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xf0\x9f\x87\xab!"));
    pc_text_destroy(t);
}

static void t_caret_mapping(void)
{
    static const char txt[] = "Hello AV\nWorld\n\nxe\xcc\x81y";
    pc_text *t = make_text(txt, PC_TEXT_CENTER);
    size_t len = strlen(txt), stops = 0;
    pc_box b;
    /* round trip for every caret stop */
    for (size_t i = 0; i <= len; i++) {
        size_t back;
        pc_text_set_caret(t, i, false);
        if (pc_text_caret(t) != i) continue;
        stops++;
        pc_text_caret_box(t, i, &b);
        CHECK(e3_near(b.y1 - b.y0, 22.0, 1e-12));
        back = pc_text_hit_index(t, pc_pt_make(b.x0, 0.5 * (b.y0 + b.y1)));
        CHECK(back == i);
    }
    CHECK(stops == 20u);
    /* midpoint rule inside a glyph, line ends, outside lines */
    pc_text_caret_box(t, 1, &b);                 /* between H and e: x = start + 12 */
    CHECK(pc_text_hit_index(t, pc_pt_make(b.x0 + 5.9, b.y0 + 1)) == 1u);
    CHECK(pc_text_hit_index(t, pc_pt_make(b.x0 + 6.1, b.y0 + 1)) == 2u);
    CHECK(pc_text_hit_index(t, pc_pt_make(1e6, b.y0 + 1)) == 8u);
    CHECK(pc_text_hit_index(t, pc_pt_make(-1e6, b.y0 + 1)) == 0u);
    CHECK(pc_text_hit_index(t, pc_pt_make(100, -1e6)) == pc_text_hit_index(t, pc_pt_make(100, 51)));
    CHECK(pc_text_hit_index(t, pc_pt_make(-1e6, 1e6)) == 16u);
    CHECK(pc_text_hit_index(t, pc_pt_make(100, 50 + 2.5 * 22)) == 15u);   /* empty line */
    /* the empty line's caret is at the origin (centered) */
    pc_text_caret_box(t, 15, &b);
    CHECK(b.x0 == 100.0 && b.y0 == 50.0 + 2.0 * 22.0);
    /* move handle and hit testing */
    {
        pc_handle_metrics m = pc_handle_metrics_for_zoom(1.0);
        pc_pt h;
        pc_text_set_caret(t, 3, false);
        pc_text_caret_box(t, 3, &b);
        h = pc_text_handle_pos(t, m.handle_offset);
        CHECK(h.x > b.x1 && h.y > b.y1);
        CHECK(pc_text_hit_test(t, h, &m) == PC_TEXT_PART_HANDLE);
        CHECK(pc_text_hit_test(t, pc_pt_make(100, 60), NULL) == PC_TEXT_PART_INSIDE);
        CHECK(pc_text_hit_test(t, pc_pt_make(100, 400), NULL) == PC_TEXT_PART_NONE);
    }
    pc_text_destroy(t);
}

static void t_editing(void)
{
    pc_text *t = make_text("ABCDEFGH\nAB\nABCDEFGH", PC_TEXT_LEFT);
    pc_box boxes[4];
    size_t nb;
    /* Up / Down keep the column */
    pc_text_set_caret(t, 6, false);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOWN, false);
    CHECK(pc_text_caret(t) == 11u);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOWN, false);
    CHECK(pc_text_caret(t) == 18u);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOWN, false);
    CHECK(pc_text_caret(t) == 18u);
    pc_text_move_caret(t, PC_TEXT_MOVE_UP, true);
    CHECK(pc_text_caret(t) == 11u && pc_text_sel_anchor(t) == 18u && pc_text_has_selection(t));
    pc_text_move_caret(t, PC_TEXT_MOVE_UP, true);
    CHECK(pc_text_caret(t) == 6u);
    nb = pc_text_selection_boxes(t, boxes, 4);
    CHECK(nb == 3u);
    CHECK(boxes[0].x0 == 172.0 && boxes[0].x1 == 196.0 + 5.0);    /* with the newline */
    CHECK(boxes[1].x0 == 100.0 && boxes[1].x1 == 124.0 + 5.0);
    CHECK(boxes[2].x0 == 100.0 && boxes[2].x1 == 172.0 && boxes[2].y0 == 50.0 + 44.0);
    CHECK(pc_text_selection_boxes(t, boxes, 1) == 3u);
    pc_text_move_caret(t, PC_TEXT_MOVE_UP, false);              /* first line: stays */
    CHECK(pc_text_caret(t) == 6u && !pc_text_has_selection(t));
    pc_text_move_caret(t, PC_TEXT_MOVE_END, false);
    CHECK(pc_text_caret(t) == 8u);
    pc_text_move_caret(t, PC_TEXT_MOVE_RIGHT, false);
    CHECK(pc_text_caret(t) == 9u);
    pc_text_move_caret(t, PC_TEXT_MOVE_HOME, false);
    CHECK(pc_text_caret(t) == 9u);
    pc_text_move_caret(t, PC_TEXT_MOVE_LEFT, false);
    CHECK(pc_text_caret(t) == 8u);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOC_END, true);
    CHECK(pc_text_caret(t) == 20u && pc_text_sel_anchor(t) == 8u);
    pc_text_move_caret(t, PC_TEXT_MOVE_LEFT, false);            /* collapses to the start */
    CHECK(pc_text_caret(t) == 8u && !pc_text_has_selection(t));
    CHECK(pc_text_delete(t, false) == PC_OK && same_str(t, "ABCDEFGHAB\nABCDEFGH"));
    CHECK(pc_text_insert(t, "\n", 1) == PC_OK && same_str(t, "ABCDEFGH\nAB\nABCDEFGH"));
    CHECK(pc_text_caret(t) == 9u);
    CHECK(pc_text_backspace(t, false) == PC_OK && pc_text_caret(t) == 8u);
    pc_text_move_caret(t, PC_TEXT_MOVE_DOC_START, false);
    CHECK(pc_text_backspace(t, false) == PC_OK && pc_text_caret(t) == 0u);
    pc_text_select_all(t);
    CHECK(pc_text_insert(t, "x", 1) == PC_OK && same_str(t, "x") && pc_text_caret(t) == 1u);

    /* words */
    CHECK(pc_text_set_utf8(t, "hello world, foo\nbar", 20) == PC_OK);
    {
        static const size_t right[] = { 6, 11, 13, 16, 17, 20, 20 };
        static const size_t left[] = { 17, 16, 13, 11, 6, 0, 0 };
        pc_text_set_caret(t, 0, false);
        for (size_t i = 0; i < sizeof right / sizeof right[0]; i++) {
            pc_text_move_caret(t, PC_TEXT_MOVE_WORD_RIGHT, false);
            CHECK(pc_text_caret(t) == right[i]);
        }
        for (size_t i = 0; i < sizeof left / sizeof left[0]; i++) {
            pc_text_move_caret(t, PC_TEXT_MOVE_WORD_LEFT, false);
            CHECK(pc_text_caret(t) == left[i]);
        }
        pc_text_set_caret(t, 8, false);                          /* inside "world" */
        pc_text_move_caret(t, PC_TEXT_MOVE_WORD_LEFT, false);
        CHECK(pc_text_caret(t) == 6u);
    }
    pc_text_set_caret(t, 11, false);
    CHECK(pc_text_backspace(t, true) == PC_OK && same_str(t, "hello , foo\nbar"));
    CHECK(pc_text_backspace(t, true) == PC_OK && same_str(t, ", foo\nbar"));
    CHECK(pc_text_caret(t) == 0u);
    CHECK(pc_text_delete(t, true) == PC_OK && same_str(t, "foo\nbar"));
    CHECK(pc_text_delete(t, true) == PC_OK && same_str(t, "\nbar"));
    CHECK(pc_text_delete(t, true) == PC_OK && same_str(t, "bar"));
    pc_text_move_caret(t, PC_TEXT_MOVE_DOC_END, false);
    CHECK(pc_text_delete(t, true) == PC_OK && same_str(t, "bar"));
    pc_text_set_caret(t, 1, false);
    pc_text_set_caret(t, 3, true);
    CHECK(pc_text_backspace(t, true) == PC_OK && same_str(t, "b"));   /* selection first */
    CHECK(pc_text_insert(t, "\xc3\xa9t\xc3\xa9 caf\xc3\xa9", 11) == PC_OK);
    CHECK(pc_text_backspace(t, true) == PC_OK && same_str(t, "b\xc3\xa9t\xc3\xa9 "));
    pc_text_destroy(t);
}

/* ---- geometry ------------------------------------------------------------------------- */

static double build_area(pc_text *t, pc_rect r, pc_mask *keep, pc_pt *mn, pc_pt *mx)
{
    pc_poly p;
    pc_vlayer l;
    pc_mask m;
    double a = -1.0;
    pc_poly_init(&p);
    if (pc_text_build(t, &p) == PC_OK) {
        if (mn && mx) CHECK(pc_poly_bounds(&p, mn, mx));
        l.fill = &p; l.rule = PC_FILL_NONZERO; l.thin = NULL; l.src = NULL;
        if (e3_coverage(&l, 1, true, r, &m) == PC_OK) {
            a = e3_mask_area(&m);
            if (keep) *keep = m;
            else pc_mask_free(&m);
        }
    }
    pc_poly_free(&p);
    return a;
}

static void t_geometry(void)
{
    pc_text *t = make_text("I", PC_TEXT_LEFT);
    pc_rect r = pc_rect_make(0, 0, 300, 160);
    pc_text_style st = *pc_text_get_style(t);
    pc_pt mn, mx, mn2, mx2;
    pc_mask m;
    pc_font_face it;
    double a, bold_a;
    /* 'I' = 2 x 14 px bar at x 105..107, baseline 66 */
    CHECK(e3_near(build_area(t, r, NULL, &mn, &mx), 28.0, 1e-9));
    CHECK(mn.x == 105.0 && mx.x == 107.0 && mn.y == 52.0 && mx.y == 66.0);
    /* synthetic italic shears by 0.2: same area, the top moves right */
    st.italic = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(build_area(t, r, NULL, &mn2, &mx2), 28.0, 0.15));   /* 8-bit coverage */
    CHECK(e3_near(mx2.x, 107.0 + 0.2 * 14.0, 1e-9) && mn2.x == 105.0);
    /* an italic face is not slanted again (faces are borrowed: `it` lives
     * as long as t uses it) */
    it = g_face_a;
    it.italic = true;
    {
        const pc_font_face *faces[1] = { &it };
        CHECK(pc_text_set_fonts(t, faces, 1) == PC_OK);
        CHECK(e3_near(build_area(t, r, NULL, &mn2, &mx2), 28.0, 1e-9) && mx2.x == 107.0);
    }
    /* synthetic bold: stroke em/24 = 0.833 px around the bar (round joins) */
    st.italic = false;
    st.bold = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    {
        double w = 20.0 / 24.0;
        double want = (2.0 + w) * (14.0 + w) - (4.0 - 3.14159265358979) * w * w / 4.0;
        bold_a = build_area(t, r, NULL, NULL, NULL);
        CHECK(e3_near(bold_a, want, 0.1));
    }
    /* the same with a font whose outer contours run the other way */
    {
        const pc_font_face *faces[1] = { &g_face_a_ccw };
        CHECK(pc_text_set_fonts(t, faces, 1) == PC_OK);
        CHECK(e3_near(build_area(t, r, NULL, NULL, NULL), bold_a, 1e-6));
        st.bold = false;
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        CHECK(e3_near(build_area(t, r, NULL, NULL, NULL), 28.0, 1e-9));
    }
    /* a hole: 'O' = 8 x 14 minus 4 x 10; bold shrinks the hole */
    {
        const pc_font_face *faces[1] = { &g_face_a };
        CHECK(pc_text_set_fonts(t, faces, 1) == PC_OK);
        CHECK(pc_text_set_utf8(t, "O", 1) == PC_OK);
        CHECK(e3_near(build_area(t, r, &m, NULL, NULL), 72.0, 1e-9));
        CHECK(pc_mask_at(&m, 105, 59) == 0u && pc_mask_at(&m, 103, 59) == 255u);
        pc_mask_free(&m);
        st.bold = true;
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        a = build_area(t, r, &m, NULL, NULL);
        CHECK(a > 98.0 && a < 104.0);
        CHECK(pc_mask_at(&m, 105, 59) == 0u && pc_mask_at(&m, 104, 59) > 0u);
        pc_mask_free(&m);
        st.bold = false;
    }
    /* underline and strikeout bars span the advance width */
    CHECK(pc_text_set_utf8(t, "AB", 2) == PC_OK);
    st.snap = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    a = build_area(t, r, NULL, NULL, NULL);
    CHECK(e3_near(a, 2.0 * 8.0 * 14.0, 1e-9));
    st.underline = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(build_area(t, r, &m, NULL, NULL), a + 24.0, 1e-9));
    CHECK(pc_mask_at(&m, 100, 68) == 255u && pc_mask_at(&m, 123, 68) == 255u);
    CHECK(pc_mask_at(&m, 100, 67) == 0u && pc_mask_at(&m, 124, 68) == 0u);
    pc_mask_free(&m);
    st.strikeout = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(e3_near(build_area(t, r, &m, NULL, NULL), a + 24.0 + 8.0, 1e-9));   /* gaps only */
    CHECK(pc_mask_at(&m, 100, 60) == 255u);     /* 6 px above the baseline at 66 */
    pc_mask_free(&m);
    /* .notdef for unknown characters, the combining mark over its base */
    st.underline = st.strikeout = false;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    CHECK(pc_text_set_utf8(t, "\xe4\xb8\x80", 3) == PC_OK);
    CHECK(e3_near(build_area(t, r, NULL, NULL, NULL), 8.0 * 14.0 - 6.0 * 12.0, 1e-9));
    CHECK(pc_text_set_utf8(t, "e\xcc\x81", 3) == PC_OK);
    CHECK(e3_near(build_area(t, r, &m, NULL, NULL), 8.0 * 10.0 + 4.0 * 2.0, 1e-9));
    CHECK(pc_mask_at(&m, 104, 48) == 255u);     /* accent above the 'e' */
    pc_mask_free(&m);
    /* glyphs are cached: the backend is asked once per glyph and style */
    {
        int calls;
        CHECK(pc_text_set_utf8(t, "abcabcabc", 9) == PC_OK);
        g_fa.outline_calls = 0;
        CHECK(build_area(t, r, NULL, NULL, NULL) > 0.0);
        calls = g_fa.outline_calls;
        CHECK(calls == 3);
        CHECK(build_area(t, r, NULL, NULL, NULL) > 0.0 && g_fa.outline_calls == calls);
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        CHECK(build_area(t, r, NULL, NULL, NULL) > 0.0 && g_fa.outline_calls == 2 * calls);
        g_fa.fail = true;
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        {
            pc_poly p;
            pc_poly_init(&p);
            CHECK(pc_text_build(t, &p) == PC_ERR_FORMAT);
            pc_poly_free(&p);
        }
        g_fa.fail = false;
    }
    pc_text_destroy(t);
}

/* Many distinct glyphs: the cache grows and flushes without changing output. */
static void t_cache_stress(void)
{
    pc_text *t = make_text("", PC_TEXT_LEFT);
    char buf[4096];
    size_t n = 0;
    double a1, a2;
    pc_rect r = pc_rect_make(0, 0, 4000, 200);
    for (int i = 0; i < 300; i++) buf[n++] = (char)(0x21 + i % 94);
    CHECK(pc_text_set_utf8(t, buf, n) == PC_OK);
    a1 = build_area(t, r, NULL, NULL, NULL);
    a2 = build_area(t, r, NULL, NULL, NULL);
    CHECK(a1 > 0.0 && a1 == a2);
    pc_text_destroy(t);
}

/* ---- rendering -------------------------------------------------------------------------- */

static void t_render(void)
{
    e3_doc e = e3_doc_make(240, 140, e3_px(250, 250, 250, 255));
    pc_text *t = make_text("Hi", PC_TEXT_LEFT), *t2;
    pc_vrender *vr = pc_vrender_create(), *vr2 = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 ink = e3_px(20, 30, 200, 255);
    pc_paint_src src = e3_solid(ink);
    pc_text_style st = *pc_text_get_style(t);
    pc_txn *x;
    pc_surf a, b;
    pc_rect dirty;
    uint64_t f0 = pc_doc_fingerprint(e.d), f1;
    st.snap = true;
    CHECK(pc_text_set_style(t, &st) == PC_OK);
    x = pc_txn_begin(e.d, "Text");
    CHECK(pc_text_render(t, vr, x, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 102 && dirty.y == 52 && dirty.w == 20 && dirty.h == 14);
    a = e3_read(&e, x);
    CHECK(e3_eq(e3_at(&a, 103, 60), ink) && e3_eq(e3_at(&a, 101, 60), e3_px(250, 250, 250, 255)));
    pc_surf_free(&a);
    /* typing re-renders from the original */
    CHECK(pc_text_insert(t, "!", 1) == PC_OK);
    pc_text_set_caret(t, 0, false);
    CHECK(pc_text_insert(t, "\n", 1) == PC_OK);
    CHECK(pc_text_render(t, vr, x, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    a = e3_read(&e, x);
    CHECK(e3_eq(e3_at(&a, 103, 60), e3_px(250, 250, 250, 255)));
    CHECK(e3_eq(e3_at(&a, 103, 82), ink));
    CHECK(pc_txn_commit(x, e.h) == PC_OK);
    pc_vrender_reset(vr);
    f1 = pc_doc_fingerprint(e.d);
    CHECK(f1 != f0);
    CHECK(pc_hist_undo(e.h) && pc_doc_fingerprint(e.d) == f0);
    /* a fresh text with the same content renders the same pixels */
    t2 = make_text("\nHi!", PC_TEXT_LEFT);
    CHECK(pc_text_set_style(t2, &st) == PC_OK);
    x = pc_txn_begin(e.d, "Text");
    CHECK(pc_text_render(t2, vr2, x, e.layer, &src, &o, NULL, NULL) == PC_OK);
    b = e3_read(&e, x);
    CHECK(e3_same(&a, &b));
    pc_txn_cancel(x);
    pc_vrender_reset(vr2);
    CHECK(pc_hist_redo(e.h) && pc_doc_fingerprint(e.d) == f1);
    pc_surf_free(&a);
    pc_surf_free(&b);
    /* aliased + move: only whole pixels; empty text clears */
    o.antialias = false;
    o.paint.blend = PC_BLEND_DIFFERENCE;
    x = pc_txn_begin(e.d, "Text");
    pc_text_set_origin(t2, pc_pt_make(20.3, 20.6));
    st.italic = true;
    CHECK(pc_text_set_style(t2, &st) == PC_OK);
    CHECK(pc_text_render(t2, vr2, x, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    a = e3_read(&e, x);
    b = e3_read(&e, NULL);
    {
        pc_px32 want = e3_at(&b, 25, 40), p = ink;
        size_t changed = 0, odd = 0;
        pc_composite_span(&want, &p, 1, PC_BLEND_DIFFERENCE, 255);
        for (int32_t y = 0; y < a.h; y++)
            for (int32_t xx = 0; xx < a.w; xx++)
                if (!e3_eq(e3_at(&a, xx, y), e3_at(&b, xx, y))) {
                    pc_px32 w2 = e3_at(&b, xx, y), p2 = ink;
                    pc_composite_span(&w2, &p2, 1, PC_BLEND_DIFFERENCE, 255);
                    changed++;
                    if (!e3_eq(e3_at(&a, xx, y), w2)) odd++;
                }
        CHECK(changed > 50u && odd == 0u);
        (void)want;
    }
    pc_surf_free(&a);
    pc_surf_free(&b);
    CHECK(pc_text_set_utf8(t2, "", 0) == PC_OK);
    CHECK(pc_text_render(t2, vr2, x, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    CHECK(!pc_rect_is_empty(dirty) && pc_rect_is_empty(pc_vrender_painted(vr2)));
    CHECK(pc_txn_commit(x, e.h) == PC_OK);
    CHECK(pc_doc_fingerprint(e.d) == f1);
    pc_vrender_reset(vr2);
    /* errors */
    x = pc_txn_begin(e.d, "Text");
    CHECK(pc_text_render(t, vr, x, 12345u, &src, &o, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_text_render(t, NULL, x, e.layer, &src, &o, NULL, NULL) == PC_ERR_ARG);
    pc_txn_cancel(x);
    pc_vrender_destroy(vr);
    pc_vrender_destroy(vr2);
    pc_text_destroy(t);
    pc_text_destroy(t2);
    e3_doc_free(&e);
}

/* Random typing and caret moves keep the text valid UTF-8 and the caret
 * on a stop; layout invariants hold. */
static void t_random_edits(void)
{
    static const char *const pieces[] = {
        "a", "B", " ", "\n", "e\xcc\x81", "\xd0\x96", "\xe2\x82\xac", "\xf0\x9f\x87\xa9",
        "\xf0\x9f\x98\x80", "\xe2\x80\x8d", ",", "\xff", "\r\n", "\t", "AV"
    };
    int iters = g_quick ? 400 : 4000;
    pc_text *t = make_text("", PC_TEXT_CENTER);
    for (int it = 0; it < iters; it++) {
        uint32_t op = rndu(10);
        size_t n, nl, ng, len;
        const char *s;
        const pc_text_line *L;
        const pc_text_glyph *g;
        if (op < 4) {
            const char *p = pieces[rnd8() % (sizeof pieces / sizeof pieces[0])];
            CHECK(pc_text_insert(t, p, strlen(p)) == PC_OK);
        } else if (op < 5) {
            CHECK(pc_text_backspace(t, rndu(3) == 0u) == PC_OK);
        } else if (op < 6) {
            CHECK(pc_text_delete(t, rndu(3) == 0u) == PC_OK);
        } else if (op < 9) {
            bool ext = rndu(3) == 0u;
            pc_text_move_caret(t, (pc_text_move)rndu(10), ext);
        } else {
            pc_box b;
            pc_text_caret_box(t, rndu(200), &b);
            pc_text_set_caret(t, pc_text_hit_index(t, pc_pt_make(b.x0 + (double)rndu(40) - 20.0,
                                                                 b.y0 + (double)rndu(40))),
                              rndu(2) != 0u);
        }
        s = pc_text_utf8(t, &len);
        CHECK(s[len] == '\0' && strlen(s) == len);
        CHECK(pc_text_caret(t) <= len && pc_text_sel_anchor(t) <= len);
        {
            size_t c = pc_text_caret(t);
            pc_text_set_caret(t, c, true);
            CHECK(pc_text_caret(t) == c);                 /* already a stop */
        }
        L = pc_text_lines(t, &nl);
        g = pc_text_glyphs(t, &ng);
        n = 1;
        for (size_t i = 0; i < len; i++) n += s[i] == '\n';
        CHECK(nl == n && L[0].byte_start == 0u && L[nl - 1u].byte_end == len);
        for (size_t i = 0; i + 1u < nl; i++) {
            CHECK(L[i + 1u].byte_start == L[i].byte_end + 1u);
            CHECK(e3_near(L[i + 1u].baseline - L[i].baseline, 22.0, 1e-9));
            CHECK(e3_near(L[i].x + 0.5 * L[i].width, 100.0, 0.5));
        }
        CHECK(ng == 0u || (g[0].byte == 0u || s[0] == '\n'));
    }
    pc_text_destroy(t);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    init_fonts();
    pc_tile_stats(&t0, &b0);
    RUN(t_units);
    RUN(t_normalize);
    RUN(t_layout);
    RUN(t_clusters);
    RUN(t_caret_mapping);
    RUN(t_editing);
    RUN(t_geometry);
    RUN(t_cache_stress);
    RUN(t_render);
    RUN(t_random_edits);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
