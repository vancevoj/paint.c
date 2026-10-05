/* pc_text.c - text buffer, UTF-8 normalization, caret stops, editing and
 * layout of the Text tool (lane E3). Rendering is in pc_text_render.c. */
#include "pc_text_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SQRT_HALF 0.70710678118654752440
#define EM_MIN 0.1
#define EM_MAX 10000.0

/* ---- style -------------------------------------------------------------------------- */

void pc_text_style_default(pc_text_style *st)
{
    memset(st, 0, sizeof *st);
    st->size = 12.0;
    st->unit = PC_TEXT_POINTS;
    st->dpi = 96.0;
    st->align = PC_TEXT_LEFT;
    st->mode = PC_TEXT_SMOOTH;
    st->anchor = PC_TEXT_ANCHOR_LINE_CENTER;
    st->snap = true;
}

double pc_text_em_pixels(const pc_text_style *st)
{
    double em;
    if (!st || !(st->size > 0.0) || !isfinite(st->size)) return 0.0;
    if (st->unit == PC_TEXT_FIXED96) {
        em = st->size * 96.0 / 72.0;
    } else if (st->unit == PC_TEXT_POINTS) {
        if (!(st->dpi > 0.0) || !isfinite(st->dpi)) return 0.0;
        em = st->size * st->dpi / 72.0;
    } else {
        return 0.0;
    }
    return isfinite(em) ? em : 0.0;
}

/* ---- UTF-8 ------------------------------------------------------------------------------ */

/* Decode the codepoint at i of a VALID buffer; *next gets the following
 * offset. */
static uint32_t dec(const char *s, size_t len, size_t i, size_t *next)
{
    const unsigned char *u = (const unsigned char *)s;
    uint32_t c = u[i];
    size_t n = 1u;
    if (c >= 0xF0u && i + 3u < len) {
        c = ((c & 0x07u) << 18) | ((uint32_t)(u[i + 1] & 0x3Fu) << 12) |
            ((uint32_t)(u[i + 2] & 0x3Fu) << 6) | (uint32_t)(u[i + 3] & 0x3Fu);
        n = 4u;
    } else if (c >= 0xE0u && i + 2u < len) {
        c = ((c & 0x0Fu) << 12) | ((uint32_t)(u[i + 1] & 0x3Fu) << 6) |
            (uint32_t)(u[i + 2] & 0x3Fu);
        n = 3u;
    } else if (c >= 0xC0u && i + 1u < len) {
        c = ((c & 0x1Fu) << 6) | (uint32_t)(u[i + 1] & 0x3Fu);
        n = 2u;
    }
    if (next) *next = i + n > len ? len : i + n;
    return c;
}

static size_t enc(uint32_t c, char *o)
{
    unsigned char *u = (unsigned char *)o;
    if (c < 0x80u) {
        u[0] = (unsigned char)c;
        return 1u;
    }
    if (c < 0x800u) {
        u[0] = (unsigned char)(0xC0u | (c >> 6));
        u[1] = (unsigned char)(0x80u | (c & 0x3Fu));
        return 2u;
    }
    if (c < 0x10000u) {
        u[0] = (unsigned char)(0xE0u | (c >> 12));
        u[1] = (unsigned char)(0x80u | ((c >> 6) & 0x3Fu));
        u[2] = (unsigned char)(0x80u | (c & 0x3Fu));
        return 3u;
    }
    u[0] = (unsigned char)(0xF0u | (c >> 18));
    u[1] = (unsigned char)(0x80u | ((c >> 12) & 0x3Fu));
    u[2] = (unsigned char)(0x80u | ((c >> 6) & 0x3Fu));
    u[3] = (unsigned char)(0x80u | (c & 0x3Fu));
    return 4u;
}

/* Strict decoding of untrusted input: returns the codepoint and its byte
 * length, or 0xFFFD with length 1 for an invalid sequence. */
static uint32_t dec_strict(const unsigned char *s, size_t n, size_t *used)
{
    uint32_t c = s[0], min;
    size_t k;
    if (c < 0x80u) {
        *used = 1u;
        return c;
    }
    if (c >= 0xC2u && c <= 0xDFu) { k = 2u; c &= 0x1Fu; min = 0x80u; }
    else if (c >= 0xE0u && c <= 0xEFu) { k = 3u; c &= 0x0Fu; min = 0x800u; }
    else if (c >= 0xF0u && c <= 0xF4u) { k = 4u; c &= 0x07u; min = 0x10000u; }
    else { *used = 1u; return 0xFFFDu; }
    if (n < k) { *used = 1u; return 0xFFFDu; }
    for (size_t i = 1; i < k; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { *used = 1u; return 0xFFFDu; }
        c = (c << 6) | (uint32_t)(s[i] & 0x3Fu);
    }
    if (c < min || c > 0x10FFFFu || (c >= 0xD800u && c <= 0xDFFFu)) {
        *used = 1u;
        return 0xFFFDu;
    }
    *used = k;
    return c;
}

/* Normalized copy of untrusted input (see pc_text_insert). *out is
 * malloc'ed (NUL-terminated) and owned by the caller. */
static pc_status normalize(const char *in, size_t n, char **out, size_t *out_len)
{
    size_t cap, o = 0, i = 0;
    char *b;
    *out = NULL;
    *out_len = 0;
    if (n > PC_TEXT_MAX_BYTES) return PC_ERR_LIMIT;
    if (!pc_mul_size(n, 3u, &cap) || !pc_add_size(cap, 1u, &cap)) return PC_ERR_LIMIT;
    b = (char *)malloc(cap);
    if (!b) return PC_ERR_NOMEM;
    while (i < n) {
        size_t used;
        uint32_t c = dec_strict((const unsigned char *)in + i, n - i, &used);
        i += used;
        if (c == '\r') {
            if (i < n && in[i] == '\n') i++;
            c = '\n';
        } else if (c == '\t') {
            c = ' ';
        }
        if (c != '\n' && (c < 0x20u || c == 0x7Fu || (c >= 0x80u && c <= 0x9Fu))) continue;
        o += enc(c, b + o);
    }
    b[o] = '\0';
    *out = b;
    *out_len = o;
    return PC_OK;
}

/* ---- clusters and word classes ------------------------------------------------------------- */

static bool is_mark(uint32_t c)
{
    return (c >= 0x0300u && c <= 0x036Fu) || (c >= 0x0483u && c <= 0x0489u) ||
           (c >= 0x0591u && c <= 0x05BDu) || (c >= 0x0610u && c <= 0x061Au) ||
           (c >= 0x064Bu && c <= 0x065Fu) || c == 0x0670u ||
           (c >= 0x1AB0u && c <= 0x1AFFu) || (c >= 0x1DC0u && c <= 0x1DFFu) ||
           (c >= 0x20D0u && c <= 0x20FFu) || (c >= 0xFE00u && c <= 0xFE0Fu) ||
           (c >= 0xFE20u && c <= 0xFE2Fu) || (c >= 0x1F3FBu && c <= 0x1F3FFu) ||
           (c >= 0xE0020u && c <= 0xE007Fu) || (c >= 0xE0100u && c <= 0xE01EFu) ||
           c == 0x200Cu;
}

static bool is_ri(uint32_t c) { return c >= 0x1F1E6u && c <= 0x1F1FFu; }

/* End of the cluster starting at i. */
static size_t next_stop(const pc_text *t, size_t i)
{
    size_t j, k;
    uint32_t c;
    if (i >= t->len) return t->len;
    c = dec(t->buf, t->len, i, &j);
    if (c == '\n') return j;
    if (is_ri(c) && j < t->len && is_ri(dec(t->buf, t->len, j, &k))) j = k;
    while (j < t->len) {
        uint32_t c2 = dec(t->buf, t->len, j, &k);
        if (is_mark(c2)) {
            j = k;
        } else if (c2 == 0x200Du) {
            j = k;
            if (j < t->len && dec(t->buf, t->len, j, &k) != '\n') j = k;
        } else {
            break;
        }
    }
    return j;
}

static size_t line_start_of(const pc_text *t, size_t i)
{
    while (i > 0u && t->buf[i - 1u] != '\n') i--;
    return i;
}

/* Largest caret stop <= i. */
static size_t snap_stop(const pc_text *t, size_t i)
{
    size_t s, prev;
    if (i >= t->len) return t->len;
    while (i > 0u && ((unsigned char)t->buf[i] & 0xC0u) == 0x80u) i--;
    s = line_start_of(t, i);
    prev = s;
    while (s < i) {
        prev = s;
        s = next_stop(t, s);
        if (s > i) return prev;
    }
    return s;
}

/* Largest caret stop < i (i > 0). */
static size_t prev_stop(const pc_text *t, size_t i)
{
    size_t s, prev;
    if (i == 0u) return 0u;
    if (i > t->len) i = t->len;
    if (t->buf[i - 1u] == '\n') return i - 1u;
    s = line_start_of(t, i - 1u);
    prev = s;
    while (s < i) {
        prev = s;
        s = next_stop(t, s);
    }
    return prev;
}

enum { CLS_SPACE = 0, CLS_PUNCT = 1, CLS_WORD = 2, CLS_NL = 3 };

static int char_class(uint32_t c)
{
    if (c == '\n') return CLS_NL;
    if (c == ' ' || c == 0x00A0u || c == 0x1680u || (c >= 0x2000u && c <= 0x200Au) ||
        c == 0x202Fu || c == 0x205Fu || c == 0x3000u)
        return CLS_SPACE;
    if ((c >= 0x21u && c <= 0x2Fu) || (c >= 0x3Au && c <= 0x40u) || (c >= 0x5Bu && c <= 0x5Eu) ||
        c == 0x60u || (c >= 0x7Bu && c <= 0x7Eu) || (c >= 0x00A1u && c <= 0x00BFu) ||
        c == 0x00D7u || c == 0x00F7u || (c >= 0x2010u && c <= 0x2027u) ||
        (c >= 0x2030u && c <= 0x205Eu) || (c >= 0x3001u && c <= 0x3003u) ||
        (c >= 0x3008u && c <= 0x3011u) || (c >= 0xFF01u && c <= 0xFF0Fu) ||
        (c >= 0xFF1Au && c <= 0xFF20u))
        return CLS_PUNCT;
    return CLS_WORD;
}

static int class_at(const pc_text *t, size_t i)
{
    return char_class(dec(t->buf, t->len, i, NULL));
}

/* Ctrl+Right: the start of the next word. */
static size_t word_right(const pc_text *t, size_t i)
{
    int c;
    if (i >= t->len) return t->len;
    c = class_at(t, i);
    if (c == CLS_NL) return next_stop(t, i);
    if (c != CLS_SPACE)
        while (i < t->len && class_at(t, i) == c) i = next_stop(t, i);
    while (i < t->len && class_at(t, i) == CLS_SPACE) i = next_stop(t, i);
    return i;
}

/* Ctrl+Left: the start of the previous word. */
static size_t word_left(const pc_text *t, size_t i)
{
    size_t j;
    int c;
    if (i == 0u) return 0u;
    j = prev_stop(t, i);
    c = class_at(t, j);
    if (c == CLS_NL) return j;
    while (c == CLS_SPACE) {
        i = j;
        if (i == 0u) return 0u;
        j = prev_stop(t, i);
        c = class_at(t, j);
        if (c == CLS_NL) return i;
    }
    i = j;
    while (i > 0u) {
        j = prev_stop(t, i);
        if (class_at(t, j) != c) break;
        i = j;
    }
    return i;
}

/* ---- layout --------------------------------------------------------------------------------- */

static bool face_has(const pc_font_face *f, uint32_t cp)
{
    if (f->has_glyph) return f->has_glyph(f->ud, cp);
    return f->glyph(f->ud, cp) != 0u;
}

/* ---- presentation and ignorables (lane TOOLB) ---------------------------------------------- */

/* Unicode 15.1 Emoji_Presentation=Yes (emoji-data.txt), merged ranges. */
static const uint32_t k_emoji_pres[][2] = {
    { 0x231Au, 0x231Bu }, { 0x23E9u, 0x23ECu }, { 0x23F0u, 0x23F0u }, { 0x23F3u, 0x23F3u },
    { 0x25FDu, 0x25FEu }, { 0x2614u, 0x2615u }, { 0x2648u, 0x2653u }, { 0x267Fu, 0x267Fu },
    { 0x2693u, 0x2693u }, { 0x26A1u, 0x26A1u }, { 0x26AAu, 0x26ABu }, { 0x26BDu, 0x26BEu },
    { 0x26C4u, 0x26C5u }, { 0x26CEu, 0x26CEu }, { 0x26D4u, 0x26D4u }, { 0x26EAu, 0x26EAu },
    { 0x26F2u, 0x26F3u }, { 0x26F5u, 0x26F5u }, { 0x26FAu, 0x26FAu }, { 0x26FDu, 0x26FDu },
    { 0x2705u, 0x2705u }, { 0x270Au, 0x270Bu }, { 0x2728u, 0x2728u }, { 0x274Cu, 0x274Cu },
    { 0x274Eu, 0x274Eu }, { 0x2753u, 0x2755u }, { 0x2757u, 0x2757u }, { 0x2795u, 0x2797u },
    { 0x27B0u, 0x27B0u }, { 0x27BFu, 0x27BFu }, { 0x2B1Bu, 0x2B1Cu }, { 0x2B50u, 0x2B50u },
    { 0x2B55u, 0x2B55u }, { 0x1F004u, 0x1F004u }, { 0x1F0CFu, 0x1F0CFu },
    { 0x1F18Eu, 0x1F18Eu }, { 0x1F191u, 0x1F19Au }, { 0x1F1E6u, 0x1F1FFu },
    { 0x1F201u, 0x1F201u }, { 0x1F21Au, 0x1F21Au }, { 0x1F22Fu, 0x1F22Fu },
    { 0x1F232u, 0x1F236u }, { 0x1F238u, 0x1F23Au }, { 0x1F250u, 0x1F251u },
    { 0x1F300u, 0x1F320u }, { 0x1F32Du, 0x1F335u }, { 0x1F337u, 0x1F37Cu },
    { 0x1F37Eu, 0x1F393u }, { 0x1F3A0u, 0x1F3CAu }, { 0x1F3CFu, 0x1F3D3u },
    { 0x1F3E0u, 0x1F3F0u }, { 0x1F3F4u, 0x1F3F4u }, { 0x1F3F8u, 0x1F43Eu },
    { 0x1F440u, 0x1F440u }, { 0x1F442u, 0x1F4FCu }, { 0x1F4FFu, 0x1F53Du },
    { 0x1F54Bu, 0x1F54Eu }, { 0x1F550u, 0x1F567u }, { 0x1F57Au, 0x1F57Au },
    { 0x1F595u, 0x1F596u }, { 0x1F5A4u, 0x1F5A4u }, { 0x1F5FBu, 0x1F64Fu },
    { 0x1F680u, 0x1F6C5u }, { 0x1F6CCu, 0x1F6CCu }, { 0x1F6D0u, 0x1F6D2u },
    { 0x1F6D5u, 0x1F6D7u }, { 0x1F6DCu, 0x1F6DFu }, { 0x1F6EBu, 0x1F6ECu },
    { 0x1F6F4u, 0x1F6FCu }, { 0x1F7E0u, 0x1F7EBu }, { 0x1F7F0u, 0x1F7F0u },
    { 0x1F90Cu, 0x1F93Au }, { 0x1F93Cu, 0x1F945u }, { 0x1F947u, 0x1F9FFu },
    { 0x1FA70u, 0x1FA7Cu }, { 0x1FA80u, 0x1FA88u }, { 0x1FA90u, 0x1FABDu },
    { 0x1FABFu, 0x1FAC5u }, { 0x1FACEu, 0x1FADBu }, { 0x1FAE0u, 0x1FAE8u },
    { 0x1FAF0u, 0x1FAF8u }
};

static bool is_emoji_pres(uint32_t c)
{
    size_t lo = 0, hi = sizeof k_emoji_pres / sizeof k_emoji_pres[0];
    if (c < 0x231Au) return false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (c < k_emoji_pres[mid][0]) hi = mid;
        else if (c > k_emoji_pres[mid][1]) lo = mid + 1u;
        else return true;
    }
    return false;
}

/* Unicode Default_Ignorable_Code_Point (DerivedCoreProperties.txt). */
static bool is_ignorable(uint32_t c)
{
    if (c < 0x00ADu) return false;
    return c == 0x00ADu || c == 0x034Fu || c == 0x061Cu || c == 0x115Fu || c == 0x1160u ||
           c == 0x17B4u || c == 0x17B5u || (c >= 0x180Bu && c <= 0x180Fu) ||
           (c >= 0x200Bu && c <= 0x200Fu) || (c >= 0x202Au && c <= 0x202Eu) ||
           (c >= 0x2060u && c <= 0x206Fu) || c == 0x3164u || (c >= 0xFE00u && c <= 0xFE0Fu) ||
           c == 0xFEFFu || c == 0xFFA0u || (c >= 0xFFF0u && c <= 0xFFF8u) ||
           (c >= 0x1BCA0u && c <= 0x1BCA3u) || (c >= 0x1D173u && c <= 0x1D17Au) ||
           (c >= 0xE0000u && c <= 0xE0FFFu);
}

/* Face for cp. Emoji presentation: the first color face that has it
 * (the primary face included), else the primary face, else any fallback.
 * Text presentation: the primary face, else the first monochrome
 * fallback, else any fallback. Nothing: the primary face (its .notdef). */
static uint32_t pick_face(const pc_text *t, uint32_t cp, bool want_color)
{
    if (want_color)
        for (size_t f = 0; f < t->n_faces; f++)
            if (t->faces[f]->color && face_has(t->faces[f], cp)) return (uint32_t)f;
    if (face_has(t->faces[0], cp)) return 0u;
    for (int pass = 0; pass < 2; pass++)
        for (size_t f = 1; f < t->n_faces; f++) {
            if (pass == 0 && t->faces[f]->color) continue;
            if (face_has(t->faces[f], cp)) return (uint32_t)f;
        }
    return 0u;
}

static void resolve_metrics(pc_text *t)
{
    pc_font_metrics m;
    double em = t->em;
    memset(&m, 0, sizeof m);
    if (t->n_faces && t->faces[0]->metrics) t->faces[0]->metrics(t->faces[0]->ud, em, &m);
    if (!(m.ascent > 0.0) || !isfinite(m.ascent)) {
        m.ascent = 0.8 * em;
        m.descent = 0.2 * em;
        m.line_gap = 0.0;
    }
    if (!(m.descent >= 0.0) || !isfinite(m.descent)) m.descent = 0.0;
    if (!(m.line_gap >= 0.0) || !isfinite(m.line_gap)) m.line_gap = 0.0;
    if (!(m.underline_offset != 0.0) || !isfinite(m.underline_offset))
        m.underline_offset = 0.1 * em;
    if (!(m.underline_thickness > 0.0) || !isfinite(m.underline_thickness))
        m.underline_thickness = em / 14.0 > 1.0 ? em / 14.0 : 1.0;
    if (!(m.strike_offset != 0.0) || !isfinite(m.strike_offset)) m.strike_offset = -0.3 * em;
    if (!(m.strike_thickness > 0.0) || !isfinite(m.strike_thickness))
        m.strike_thickness = m.underline_thickness;
    t->fm = m;
    t->line_h = m.ascent + m.descent + m.line_gap;
}

static double snapv(const pc_text *t, double v)
{
    return t->style.snap ? floor(v + 0.5) : v;
}

/* Sharp modes place glyphs on whole pixels (the backend may hint the
 * outlines and advances themselves): Classic rounds every advance like
 * GDI, Modern rounds each pen position and keeps the fractional sum.
 * Positions are relative to the line start; returns the new width. */
static double sharpen(pc_text *t, size_t g0, size_t g1)
{
    double pen = 0.0, acc = 0.0;
    if (t->style.mode == PC_TEXT_SHARP_CLASSIC) {
        for (size_t k = g0; k < g1; k++) {
            double a = floor(t->glyphs[k].advance + 0.5);
            t->glyphs[k].x = pen;
            t->glyphs[k].advance = a;
            pen += a;
        }
        return pen;
    }
    for (size_t k = g0; k < g1; k++) {
        double x = floor(acc + 0.5);
        acc += t->glyphs[k].advance;
        t->glyphs[k].x = x;
        t->glyphs[k].advance = floor(acc + 0.5) - x;
    }
    return floor(acc + 0.5);
}

/* Kerning state of a line: the previous visible glyph. */
typedef struct kern_state {
    bool   have;
    size_t idx;
} kern_state;

#define CLUSTER_MAX 32u          /* code points of a cluster that can ligate */

/* Glyphs of the cluster [i, cend) appended at *pen (one per code point;
 * see the color font notes in pc_text.h for faces, presentation,
 * ignorables and ligatures). Never allocates. */
static void layout_cluster(pc_text *t, size_t i, size_t cend, double *pen, kern_state *ks)
{
    uint32_t base_cp, fb = 0;
    bool want_color = false, vs15 = false, vs16 = false, lig_ok = true;
    size_t g0 = t->n_glyphs, nvis = 0;
    uint32_t gids[CLUSTER_MAX];
    size_t slot[CLUSTER_MAX];
    base_cp = dec(t->buf, t->len, i, NULL);
    for (size_t j = i; j < cend;) {
        size_t nx;
        uint32_t c = dec(t->buf, t->len, j, &nx);
        if (c == 0xFE0Eu) vs15 = true;
        if (c == 0xFE0Fu) vs16 = true;
        j = nx;
    }
    want_color = vs16 || (is_emoji_pres(base_cp) && !vs15);
    if (t->n_faces) fb = pick_face(t, base_cp, want_color);
    /* faces and glyphs per code point */
    for (size_t j = i; j < cend;) {
        size_t nx;
        uint32_t cp = dec(t->buf, t->len, j, &nx), face = fb, gid = 0;
        pc_text_glyph *g = &t->glyphs[t->n_glyphs++];
        bool hidden = false;
        if (t->n_faces) {
            if (j != i && !face_has(t->faces[fb], cp))
                face = is_ignorable(cp) ? fb : pick_face(t, cp, want_color);
            if (is_ignorable(cp) && !face_has(t->faces[face], cp)) hidden = true;
            else gid = t->faces[face]->glyph(t->faces[face]->ud, cp);
        } else {
            hidden = is_ignorable(cp);
        }
        memset(g, 0, sizeof *g);
        g->cp = cp;
        g->gid = gid;
        g->face = face;
        g->cluster_start = j == i;
        g->byte = j;
        g->hidden = hidden;
        if (!hidden) {
            if (face != fb || nvis >= CLUSTER_MAX) lig_ok = false;
            else {
                gids[nvis] = gid;
                slot[nvis] = t->n_glyphs - 1u;
                nvis++;
            }
        }
        j = nx;
    }
    /* ligatures of the cluster (all its visible glyphs from one face) */
    if (t->n_faces && lig_ok && nvis >= 2u && t->faces[fb]->substitute) {
        uint32_t sub[CLUSTER_MAX];
        size_t m;
        memcpy(sub, gids, nvis * sizeof sub[0]);
        m = t->faces[fb]->substitute(t->faces[fb]->ud, sub, nvis);
        if (m >= 1u && m <= nvis) {
            for (size_t k = 0; k < nvis; k++) {
                pc_text_glyph *g = &t->glyphs[slot[k]];
                if (k < m) g->gid = sub[k];
                else g->hidden = true;
            }
        }
    }
    /* advances and kerning */
    for (size_t k = g0; k < t->n_glyphs; k++) {
        pc_text_glyph *g = &t->glyphs[k];
        const pc_font_face *f;
        double adv;
        g->x = *pen;                       /* made absolute by the caller */
        g->advance = 0.0;
        if (g->hidden || !t->n_faces) continue;
        f = t->faces[g->face];
        adv = f->advance(f->ud, g->gid, t->em, t->style.mode);
        if (!isfinite(adv)) adv = 0.0;
        if (adv > 0.0 && t->style.bold && !f->bold) adv += t->em * PC_TEXT_BOLD_FRAC;
        if (ks->have && t->glyphs[ks->idx].face == g->face && f->kerning) {
            double kv = f->kerning(f->ud, t->glyphs[ks->idx].gid, g->gid, t->em);
            if (isfinite(kv)) {
                t->glyphs[ks->idx].advance += kv;
                *pen += kv;
                g->x = *pen;
            }
        }
        g->advance = adv;
        *pen += adv;
        ks->have = true;
        ks->idx = k;
    }
}

/* Recompute lines and glyphs. Never allocates: the arrays were reserved
 * for the current text by reserve_layout. */
static void layout(pc_text *t)
{
    size_t i = 0, line = 0;
    double top0;
    t->em = pc_text_em_pixels(&t->style);
    resolve_metrics(t);
    t->n_glyphs = 0;
    t->n_lines = 0;
    switch (t->style.anchor) {
    case PC_TEXT_ANCHOR_TOP: top0 = t->origin.y; break;
    case PC_TEXT_ANCHOR_BASELINE: top0 = t->origin.y - t->fm.ascent; break;
    default: top0 = t->origin.y - 0.5 * t->line_h; break;
    }
    for (;;) {
        pc_text_line *L = &t->lines[t->n_lines++];
        double pen = 0.0, base;
        kern_state ks;
        ks.have = false;
        ks.idx = 0;
        L->byte_start = i;
        L->glyph_start = t->n_glyphs;
        while (i < t->len && t->buf[i] != '\n') {
            size_t cend = next_stop(t, i);
            layout_cluster(t, i, cend, &pen, &ks);
            i = cend;
        }
        L->byte_end = i;
        L->glyph_end = t->n_glyphs;
        if (t->style.mode != PC_TEXT_SMOOTH) pen = sharpen(t, L->glyph_start, L->glyph_end);
        L->width = pen;
        switch (t->style.align) {
        case PC_TEXT_CENTER: L->x = t->origin.x - 0.5 * pen; break;
        case PC_TEXT_RIGHT: L->x = t->origin.x - pen; break;
        default: L->x = t->origin.x; break;
        }
        L->x = snapv(t, L->x);
        base = snapv(t, top0 + (double)line * t->line_h + t->fm.ascent);
        L->baseline = base;
        L->top = base - t->fm.ascent;
        L->bottom = L->top + t->line_h;
        for (size_t k = L->glyph_start; k < L->glyph_end; k++) {
            t->glyphs[k].x += L->x;
            t->glyphs[k].y = base;
        }
        line++;
        if (i >= t->len) break;
        i++;                               /* the '\n' */
    }
}

/* Make room for the layout of a text with cps codepoints and nl newlines. */
static pc_status reserve_layout(pc_text *t, size_t cps, size_t nl)
{
    size_t nlines = nl + 1u, bytes;
    if (cps > t->cap_glyphs) {
        size_t cap = t->cap_glyphs ? t->cap_glyphs : 64u;
        pc_text_glyph *g;
        while (cap < cps) cap *= 2u;
        if (!pc_mul_size(cap, sizeof *g, &bytes)) return PC_ERR_LIMIT;
        g = (pc_text_glyph *)realloc(t->glyphs, bytes);
        if (!g) return PC_ERR_NOMEM;
        t->glyphs = g;
        t->cap_glyphs = cap;
    }
    if (nlines > t->cap_lines) {
        size_t cap = t->cap_lines ? t->cap_lines : 8u;
        pc_text_line *l;
        while (cap < nlines) cap *= 2u;
        if (!pc_mul_size(cap, sizeof *l, &bytes)) return PC_ERR_LIMIT;
        l = (pc_text_line *)realloc(t->lines, bytes);
        if (!l) return PC_ERR_NOMEM;
        t->lines = l;
        t->cap_lines = cap;
    }
    return PC_OK;
}

static void count_text(const char *s, size_t n, size_t *cps, size_t *nl)
{
    size_t c = 0, l = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0u) != 0x80u) c++;
        if (b == '\n') l++;
    }
    *cps = c;
    *nl = l;
}

/* ---- object ---------------------------------------------------------------------------------- */

pc_text *pc_text_create(void)
{
    pc_text *t = (pc_text *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    t->buf = (char *)malloc(16u);
    if (!t->buf || reserve_layout(t, 1u, 0u) != PC_OK) {
        free(t->buf);
        free(t->glyphs);
        free(t->lines);
        free(t);
        return NULL;
    }
    t->buf[0] = '\0';
    t->cap = 16u;
    pc_text_style_default(&t->style);
    pc_poly_init(&t->scratch);
    layout(t);
    return t;
}

void pc_text_destroy(pc_text *t)
{
    if (!t) return;
    pc_text_cache_free(t);
    pc_poly_free(&t->scratch);
    free(t->buf);
    free(t->glyphs);
    free(t->lines);
    free(t);
}

pc_status pc_text_set_fonts(pc_text *t, const pc_font_face *const *faces, size_t n)
{
    if (!t || n > PC_TEXT_MAX_FACES || (n && !faces)) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!faces[i] || !faces[i]->glyph || !faces[i]->advance) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++) t->faces[i] = faces[i];
    t->n_faces = n;
    pc_text_cache_clear(t);
    layout(t);
    return PC_OK;
}

pc_status pc_text_set_style(pc_text *t, const pc_text_style *st)
{
    double em;
    if (!t || !st) return PC_ERR_ARG;
    em = pc_text_em_pixels(st);
    if (!(em >= EM_MIN && em <= EM_MAX)) return PC_ERR_ARG;
    if ((unsigned)st->align > 2u || (unsigned)st->mode >= (unsigned)PC_TEXT_MODE_COUNT ||
        (unsigned)st->anchor > 2u)
        return PC_ERR_ARG;
    t->style = *st;
    pc_text_cache_clear(t);
    layout(t);
    return PC_OK;
}

const pc_text_style *pc_text_get_style(const pc_text *t) { return &t->style; }

void pc_text_set_origin(pc_text *t, pc_pt origin)
{
    if (!isfinite(origin.x) || !isfinite(origin.y)) return;
    t->origin = origin;
    layout(t);
}

pc_pt pc_text_origin(const pc_text *t) { return t->origin; }

const char *pc_text_utf8(const pc_text *t, size_t *len)
{
    if (len) *len = t->len;
    return t->buf;
}

bool pc_text_is_empty(const pc_text *t) { return t->len == 0u; }

/* Replace [a, b) with normalized ins; caret and anchor after it. */
static pc_status replace(pc_text *t, size_t a, size_t b, const char *ins, size_t n)
{
    size_t nlen, cps, nl;
    char *nb;
    pc_status st;
    if (a > b || b > t->len) return PC_ERR_ARG;
    if (t->len - (b - a) > PC_TEXT_MAX_BYTES || n > PC_TEXT_MAX_BYTES - (t->len - (b - a)))
        return PC_ERR_LIMIT;
    nlen = t->len - (b - a) + n;
    nb = (char *)malloc(nlen + 1u);
    if (!nb) return PC_ERR_NOMEM;
    memcpy(nb, t->buf, a);
    if (n) memcpy(nb + a, ins, n);
    memcpy(nb + a + n, t->buf + b, t->len - b);
    nb[nlen] = '\0';
    count_text(nb, nlen, &cps, &nl);
    st = reserve_layout(t, cps, nl);
    if (st != PC_OK) {
        free(nb);
        return st;
    }
    free(t->buf);
    t->buf = nb;
    t->len = nlen;
    t->cap = nlen + 1u;
    /* the caret goes after the insertion; when the edit joined it into a
     * cluster (a mark or ZWJ next to it), after that cluster */
    {
        size_t c = a + n, s = snap_stop(t, c);
        if (s != c) c = next_stop(t, s);
        t->caret = t->anchor = c;
    }
    t->has_goal = false;
    layout(t);
    return PC_OK;
}

pc_status pc_text_set_utf8(pc_text *t, const char *s, size_t n)
{
    char *norm;
    size_t nlen;
    pc_status st;
    if (!t || (!s && n)) return PC_ERR_ARG;
    st = normalize(s ? s : "", n, &norm, &nlen);
    if (st != PC_OK) return st;
    st = replace(t, 0u, t->len, norm, nlen);
    free(norm);
    return st;
}

pc_status pc_text_insert(pc_text *t, const char *utf8, size_t n)
{
    char *norm;
    size_t nlen, a, b;
    pc_status st;
    if (!t || (!utf8 && n)) return PC_ERR_ARG;
    st = normalize(utf8 ? utf8 : "", n, &norm, &nlen);
    if (st != PC_OK) return st;
    a = t->caret < t->anchor ? t->caret : t->anchor;
    b = t->caret < t->anchor ? t->anchor : t->caret;
    st = replace(t, a, b, norm, nlen);
    free(norm);
    return st;
}

size_t pc_text_caret(const pc_text *t) { return t->caret; }
size_t pc_text_sel_anchor(const pc_text *t) { return t->anchor; }
bool pc_text_has_selection(const pc_text *t) { return t->caret != t->anchor; }

void pc_text_set_caret(pc_text *t, size_t index, bool extend)
{
    t->caret = snap_stop(t, index);
    if (!extend) t->anchor = t->caret;
    t->has_goal = false;
}

void pc_text_select_all(pc_text *t)
{
    t->anchor = 0u;
    t->caret = t->len;
    t->has_goal = false;
}

static pc_status remove_range(pc_text *t, size_t a, size_t b)
{
    if (a == b) {
        t->caret = t->anchor = a;
        return PC_OK;
    }
    return replace(t, a, b, NULL, 0u);
}

pc_status pc_text_backspace(pc_text *t, bool word)
{
    if (!t) return PC_ERR_ARG;
    if (t->caret != t->anchor) {
        size_t a = t->caret < t->anchor ? t->caret : t->anchor;
        size_t b = t->caret < t->anchor ? t->anchor : t->caret;
        return remove_range(t, a, b);
    }
    if (t->caret == 0u) return PC_OK;
    return remove_range(t, word ? word_left(t, t->caret) : prev_stop(t, t->caret), t->caret);
}

pc_status pc_text_delete(pc_text *t, bool word)
{
    if (!t) return PC_ERR_ARG;
    if (t->caret != t->anchor) {
        size_t a = t->caret < t->anchor ? t->caret : t->anchor;
        size_t b = t->caret < t->anchor ? t->anchor : t->caret;
        return remove_range(t, a, b);
    }
    if (t->caret >= t->len) return PC_OK;
    return remove_range(t, t->caret, word ? word_right(t, t->caret) : next_stop(t, t->caret));
}

/* ---- queries ------------------------------------------------------------------------------- */

static size_t line_of(const pc_text *t, size_t index)
{
    size_t lo = 0, hi = t->n_lines - 1u;
    while (lo < hi) {
        size_t mid = (lo + hi + 1u) / 2u;
        if (t->lines[mid].byte_start <= index) lo = mid;
        else hi = mid - 1u;
    }
    return lo;
}

static double caret_x(const pc_text *t, size_t index)
{
    const pc_text_line *L = &t->lines[line_of(t, index)];
    for (size_t k = L->glyph_start; k < L->glyph_end; k++)
        if (t->glyphs[k].byte >= index) return t->glyphs[k].x;
    return L->x + L->width;
}

/* The caret stop of line li nearest to x. */
static size_t hit_in_line(const pc_text *t, size_t li, double x)
{
    const pc_text_line *L = &t->lines[li];
    size_t k = L->glyph_start;
    while (k < L->glyph_end) {
        size_t e = k + 1u;
        double x0 = t->glyphs[k].x, x1;
        while (e < L->glyph_end && !t->glyphs[e].cluster_start) e++;
        x1 = e < L->glyph_end ? t->glyphs[e].x : L->x + L->width;
        if (x < 0.5 * (x0 + x1)) return t->glyphs[k].byte;
        k = e;
    }
    return L->byte_end;
}

void pc_text_move_caret(pc_text *t, pc_text_move m, bool extend)
{
    size_t c = t->caret, li;
    double gx;
    bool sel = t->caret != t->anchor;
    size_t lo = t->caret < t->anchor ? t->caret : t->anchor;
    size_t hi = t->caret < t->anchor ? t->anchor : t->caret;
    switch (m) {
    case PC_TEXT_MOVE_LEFT:
        c = (sel && !extend) ? lo : prev_stop(t, c);
        break;
    case PC_TEXT_MOVE_RIGHT:
        c = (sel && !extend) ? hi : next_stop(t, c);
        break;
    case PC_TEXT_MOVE_WORD_LEFT: c = word_left(t, c); break;
    case PC_TEXT_MOVE_WORD_RIGHT: c = word_right(t, c); break;
    case PC_TEXT_MOVE_HOME: c = t->lines[line_of(t, c)].byte_start; break;
    case PC_TEXT_MOVE_END: c = t->lines[line_of(t, c)].byte_end; break;
    case PC_TEXT_MOVE_DOC_START: c = 0u; break;
    case PC_TEXT_MOVE_DOC_END: c = t->len; break;
    case PC_TEXT_MOVE_UP:
    case PC_TEXT_MOVE_DOWN:
        li = line_of(t, c);
        if ((m == PC_TEXT_MOVE_UP && li == 0u) ||
            (m == PC_TEXT_MOVE_DOWN && li + 1u >= t->n_lines)) {
            if (!extend) t->anchor = t->caret;
            return;
        }
        gx = t->has_goal ? t->goal_x : caret_x(t, c);
        c = hit_in_line(t, m == PC_TEXT_MOVE_UP ? li - 1u : li + 1u, gx);
        t->caret = c;
        if (!extend) t->anchor = c;
        t->has_goal = true;
        t->goal_x = gx;
        return;
    default:
        return;
    }
    pc_text_set_caret(t, c, extend);
}

const pc_text_line *pc_text_lines(const pc_text *t, size_t *n)
{
    if (n) *n = t->n_lines;
    return t->lines;
}

const pc_text_glyph *pc_text_glyphs(const pc_text *t, size_t *n)
{
    if (n) *n = t->n_glyphs;
    return t->glyphs;
}

double pc_text_line_height(const pc_text *t) { return t->line_h; }

void pc_text_bounds(const pc_text *t, pc_box *out)
{
    const pc_text_line *L = &t->lines[0];
    out->x0 = L->x;
    out->x1 = L->x + L->width;
    out->y0 = L->top;
    out->y1 = t->lines[t->n_lines - 1u].bottom;
    for (size_t i = 1; i < t->n_lines; i++) {
        L = &t->lines[i];
        if (L->x < out->x0) out->x0 = L->x;
        if (L->x + L->width > out->x1) out->x1 = L->x + L->width;
    }
}

void pc_text_caret_box(const pc_text *t, size_t index, pc_box *out)
{
    size_t i = snap_stop(t, index);
    const pc_text_line *L = &t->lines[line_of(t, i)];
    out->x0 = out->x1 = caret_x(t, i);
    out->y0 = L->top;
    out->y1 = L->bottom;
}

size_t pc_text_hit_index(const pc_text *t, pc_pt p)
{
    double rel;
    size_t li = 0;
    if (!isfinite(p.x) || !isfinite(p.y)) return t->caret;
    rel = (p.y - t->lines[0].top) / (t->line_h > 0.0 ? t->line_h : 1.0);
    if (rel >= (double)t->n_lines) li = t->n_lines - 1u;
    else if (rel > 0.0) li = (size_t)rel;
    return hit_in_line(t, li, p.x);
}

size_t pc_text_selection_boxes(const pc_text *t, pc_box *out, size_t cap)
{
    size_t s = t->caret < t->anchor ? t->caret : t->anchor;
    size_t e = t->caret < t->anchor ? t->anchor : t->caret, n = 0;
    if (s == e) return 0u;
    for (size_t i = 0; i < t->n_lines; i++) {
        const pc_text_line *L = &t->lines[i];
        size_t a = s > L->byte_start ? s : L->byte_start;
        size_t b = e < L->byte_end ? e : L->byte_end;
        bool nl = e > L->byte_end && s <= L->byte_end && i + 1u < t->n_lines;
        if (a > b || (a == b && !nl)) continue;
        if (n < cap) {
            out[n].x0 = caret_x(t, a);
            out[n].x1 = caret_x(t, b) + (nl ? 0.25 * t->em : 0.0);
            out[n].y0 = L->top;
            out[n].y1 = L->bottom;
        }
        n++;
    }
    return n;
}

pc_pt pc_text_handle_pos(const pc_text *t, double offset)
{
    pc_box b;
    pc_text_caret_box(t, t->caret, &b);
    return pc_pt_make(b.x1 + offset * SQRT_HALF, b.y1 + offset * SQRT_HALF);
}

pc_text_part pc_text_hit_test(const pc_text *t, pc_pt p, const pc_handle_metrics *m)
{
    pc_handle_metrics dm;
    pc_pt h;
    pc_box b;
    double r;
    if (!m) {
        dm = pc_handle_metrics_for_zoom(1.0);
        m = &dm;
    }
    r = m->nub_radius;
    h = pc_text_handle_pos(t, m->handle_offset);
    if ((p.x - h.x) * (p.x - h.x) + (p.y - h.y) * (p.y - h.y) <= r * r) return PC_TEXT_PART_HANDLE;
    pc_text_bounds(t, &b);
    if (p.x >= b.x0 - r && p.x <= b.x1 + r && p.y >= b.y0 - r && p.y <= b.y1 + r)
        return PC_TEXT_PART_INSIDE;
    return PC_TEXT_PART_NONE;
}
