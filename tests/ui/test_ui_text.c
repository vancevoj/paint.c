/* test_ui_text.c - UTF-8, font loading and validation (including fuzzed
 * fonts), metrics, kerning, measurement, fallback, collections and the A8
 * text rasterizer used by the Text tool. */
#include "pc_test.h"
#include "ui_test_util.h"

#include "ui/ui_font.h"

static uint8_t *read_all(const char *path, size_t *len)
{
    void *d = SDL_LoadFile(path, len);
    uint8_t *out;
    if (!d) return NULL;
    out = (uint8_t *)malloc(*len);
    if (out) memcpy(out, d, *len);
    SDL_free(d);
    return out;
}

/* ---- UTF-8 --------------------------------------------------------------- */
static uint32_t dec1(const char *s, size_t len, size_t *i) { return ui_utf8_decode(s, len, i); }

static void t_utf8_decode(void)
{
    size_t i = 0;
    const char *ok = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    size_t n = strlen(ok);
    CHECK(dec1(ok, n, &i) == 'a' && i == 1);
    CHECK(dec1(ok, n, &i) == 0xE9 && i == 3);
    CHECK(dec1(ok, n, &i) == 0x20AC && i == 6);
    CHECK(dec1(ok, n, &i) == 0x1F600 && i == 10);
    CHECK(ui_utf8_count(ok, n) == 4);
    {
        /* each malformed sequence: expected replacement count */
        static const struct { const char *s; size_t n; int reps; } bad[] = {
            { "\xC0\xAF", 2, 2 },               /* overlong two-byte */
            { "\xE0\x80\xAF", 3, 3 },           /* overlong three-byte */
            { "\xED\xA0\x80", 3, 3 },           /* surrogate */
            { "\xE2\x82", 2, 1 },               /* truncated */
            { "\xF4\x90\x80\x80", 4, 4 },       /* above U+10FFFF */
            { "\x80", 1, 1 },                   /* stray continuation */
            { "\xF8\x88\x80\x80\x80", 5, 5 },   /* five-byte form */
            { "\xE2\x28\xA1", 3, 3 },           /* bad continuation */
        };
        for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
            size_t j = 0;
            int reps = 0, total = 0;
            while (j < bad[k].n) {
                size_t before = j;
                uint32_t cp = dec1(bad[k].s, bad[k].n, &j);
                CHECK(j > before);
                if (cp == UI_UTF8_REPLACEMENT) reps++;
                total++;
            }
            CHECK(reps == bad[k].reps || (k == 7 && reps == 2 && total == 3));
        }
    }
}

static void t_utf8_encode_roundtrip(void)
{
    uint32_t step = g_quick ? 97u : 1u;
    for (uint32_t cp = 0; cp <= 0x10FFFFu; cp += step) {
        char b[4];
        size_t i = 0;
        int n = ui_utf8_encode(cp, b);
        uint32_t back = ui_utf8_decode(b, (size_t)n, &i);
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            CHECK(back == UI_UTF8_REPLACEMENT);
        } else {
            CHECK(back == cp && i == (size_t)n);
        }
    }
    {
        char b[4];
        CHECK(ui_utf8_encode(0x110000u, b) == 3);
    }
}

static void t_utf8_boundaries(void)
{
    /* valid text: prev and next walk the same boundaries */
    const char *s = "x\xC3\xA9y\xE2\x82\xAC\xF0\x9F\x98\x80z\xD0\x96";
    size_t n = strlen(s), bounds[32], nb = 0, i = 0;
    while (i < n) { bounds[nb++] = i; i = ui_utf8_next(s, n, i); }
    bounds[nb++] = n;
    for (size_t k = 1; k < nb; k++) CHECK(ui_utf8_prev(s, bounds[k]) == bounds[k - 1]);
    CHECK(ui_utf8_floor(s, n, 2) == 1);       /* inside the two-byte e-acute */
    CHECK(ui_utf8_floor(s, n, 3) == 3);
    CHECK(ui_utf8_floor(s, n, 100) == n);
    /* random bytes: boundaries are monotonic and stay in range */
    for (int it = 0; it < (g_quick ? 2000 : 50000); it++) {
        char buf[16];
        size_t len = 1u + rndu(15);
        for (size_t k = 0; k < len; k++) buf[k] = (char)rnd8();
        for (size_t k = 1; k <= len; k++) {
            size_t pv = ui_utf8_prev(buf, k);
            CHECK(pv < k);
            CHECK(ui_utf8_next(buf, len, pv) >= k || ui_utf8_next(buf, len, pv) > pv);
        }
    }
}

/* ---- loading and metadata ------------------------------------------------ */
static void t_builtin_fonts(void)
{
    ui_font *r = ui_font_load_builtin(UI_FONT_REGULAR), *b = ui_font_load_builtin(UI_FONT_SEMIBOLD);
    ui_font_desc d;
    ui_font_metrics m;
    CHECK(r != NULL && b != NULL);
    if (!r || !b) { ui_font_free(r); ui_font_free(b); return; }
    ui_font_get_desc(r, &d);
    CHECK(strcmp(d.family, "Inter") == 0);
    CHECK(strcmp(d.style, "Regular") == 0);
    CHECK(d.weight == 400 && !d.italic && !d.cff);
    ui_font_get_desc(b, &d);
    CHECK(strcmp(d.style, "SemiBold") == 0 && d.weight == 600);
    ui_font_get_metrics(r, 13.0f, &m);
    CHECK(fabsf(m.ascent - 13.0f * 1984.0f / 2048.0f) < 0.01f);
    CHECK(fabsf(m.descent - 13.0f * 494.0f / 2048.0f) < 0.01f);
    CHECK(m.cap_height > 9.0f && m.cap_height < 9.8f);
    CHECK(m.x_height > 6.8f && m.x_height < 7.4f);
    CHECK(m.underline_size > 0.5f && m.underline_pos > 1.0f);
    CHECK(ui_font_has_glyph(r, 'A') && ui_font_has_glyph(r, 0xE9) && ui_font_has_glyph(r, 0x416) &&
          ui_font_has_glyph(r, 0x3A9) && ui_font_has_glyph(r, 0x2026));
    CHECK(!ui_font_has_glyph(r, 0x6C34));
    ui_font_free(r);
    ui_font_free(b);
}

static void t_load_files(void)
{
    static const struct { const char *path; const char *family; bool cff; } files[] = {
        { "data/inter-ttf-subset.ttf", "Inter", false },
        { "data/inter-cff-subset.otf", "Inter", true },
        { "data/noto-cjk-subset.otf", "Noto Sans CJK JP", true },
    };
    for (size_t k = 0; k < sizeof files / sizeof files[0]; k++) {
        ui_font *f = NULL;
        ui_font_desc d;
        size_t len = 0;
        uint8_t *data;
        pc_status st = ui_font_load_file(files[k].path, 0, &f);
        CHECK(st == PC_OK);
        if (st != PC_OK) continue;
        ui_font_get_desc(f, &d);
        CHECK(strcmp(d.family, files[k].family) == 0);
        CHECK(d.cff == files[k].cff);
        CHECK(ui_text_width(f, 20.0f, "A", 1) > 5.0f);
        data = read_all(files[k].path, &len);
        CHECK(data != NULL);
        if (data) {
            ui_font_desc d2;
            CHECK(ui_font_face_count(data, len) == 1);
            CHECK(ui_font_describe(data, len, 0, &d2) == PC_OK && strcmp(d2.family, d.family) == 0);
            free(data);
        }
        ui_font_free(f);
    }
    {
        ui_font *f = (ui_font *)1;
        CHECK(ui_font_load_file("data/does-not-exist.ttf", 0, &f) == PC_ERR_IO && f == NULL);
    }
}

/* Build a two-face collection from two font files (table offsets rebased). */
static uint8_t *make_ttc(const uint8_t *a, size_t la, const uint8_t *b, size_t lb, size_t *out_len)
{
    size_t hdr = 20, off_a = hdr, off_b = (hdr + la + 3u) & ~(size_t)3u, n = off_b + lb;
    uint8_t *t = (uint8_t *)calloc(1u, n);
    const uint8_t *src[2];
    size_t base[2], lens[2];
    if (!t) return NULL;
    memcpy(t, "ttcf", 4);
    t[4] = 0; t[5] = 1; t[6] = 0; t[7] = 0;
    t[11] = 2;
    src[0] = a; src[1] = b; base[0] = off_a; base[1] = off_b; lens[0] = la; lens[1] = lb;
    for (int f = 0; f < 2; f++) {
        uint8_t *d = t + base[f];
        uint32_t ntab;
        t[12 + 4 * f] = (uint8_t)(base[f] >> 24); t[13 + 4 * f] = (uint8_t)(base[f] >> 16);
        t[14 + 4 * f] = (uint8_t)(base[f] >> 8); t[15 + 4 * f] = (uint8_t)base[f];
        memcpy(d, src[f], lens[f]);
        ntab = ((uint32_t)d[4] << 8) | d[5];
        for (uint32_t i = 0; i < ntab; i++) {
            uint8_t *rec = d + 12 + 16 * i;
            uint32_t o = ((uint32_t)rec[8] << 24) | ((uint32_t)rec[9] << 16) |
                         ((uint32_t)rec[10] << 8) | rec[11];
            o += (uint32_t)base[f];
            rec[8] = (uint8_t)(o >> 24); rec[9] = (uint8_t)(o >> 16); rec[10] = (uint8_t)(o >> 8);
            rec[11] = (uint8_t)o;
        }
    }
    *out_len = n;
    return t;
}

static void t_collections_and_errors(void)
{
    size_t la = 0, lb = 0, lt = 0;
    uint8_t *a = read_all("data/inter-ttf-subset.ttf", &la),
            *b = read_all("data/inter-cff-subset.otf", &lb);
    uint8_t *ttc = (a && b) ? make_ttc(a, la, b, lb, &lt) : NULL;
    ui_font *f = NULL;
    ui_font_desc d;
    static const uint8_t junk[64] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    CHECK(ttc != NULL);
    if (ttc) {
        CHECK(ui_font_face_count(ttc, lt) == 2);
        CHECK(ui_font_load_mem(ttc, lt, 1, UI_FONT_COPY, &f) == PC_OK);
        if (f) {
            ui_font_get_desc(f, &d);
            CHECK(d.cff);
            CHECK(ui_text_width(f, 16.0f, "Hello", 5) > 20.0f);
            ui_font_free(f);
            f = NULL;
        }
        CHECK(ui_font_load_mem(ttc, lt, 0, UI_FONT_COPY, &f) == PC_OK);
        if (f) { ui_font_get_desc(f, &d); CHECK(!d.cff); ui_font_free(f); f = NULL; }
        CHECK(ui_font_load_mem(ttc, lt, 2, UI_FONT_COPY, &f) == PC_ERR_ARG && f == NULL);
        CHECK(ui_font_describe(ttc, lt, 1, &d) == PC_OK && d.cff);
    }
    CHECK(ui_font_load_mem(junk, sizeof junk, 0, 0, &f) == PC_ERR_FORMAT && f == NULL);
    CHECK(ui_font_load_mem(junk, 0, 0, 0, &f) == PC_ERR_ARG);
    CHECK(ui_font_load_mem(NULL, 10, 0, 0, &f) == PC_ERR_ARG);
    if (a) {
        CHECK(ui_font_load_mem(a, la, 1, 0, &f) == PC_ERR_ARG);
        CHECK(ui_font_load_mem(a, 11, 0, 0, &f) == PC_ERR_FORMAT);
        CHECK(ui_font_load_mem(a, 100, 0, 0, &f) != PC_OK);   /* truncated: tables missing */
        {
            uint8_t *t1 = (uint8_t *)malloc(la);
            if (t1) {
                memcpy(t1, a, la);
                memcpy(t1, "typ1", 4);
                CHECK(ui_font_load_mem(t1, la, 0, 0, &f) == PC_ERR_UNSUPPORTED);
                free(t1);
            }
        }
    }
    CHECK(ui_font_face_count(junk, sizeof junk) == 0);
    free(a);
    free(b);
    free(ttc);
}

/* Mutate valid fonts and make sure loading, measuring and rasterizing never
 * crash (the sanitizer build turns any out-of-bounds read into a failure). */
static void fuzz_one(const uint8_t *src, size_t len, int iters)
{
    uint8_t *buf = (uint8_t *)malloc(len);
    int loaded = 0;
    if (!buf) return;
    for (int it = 0; it < iters; it++) {
        ui_font *f = NULL;
        size_t n = len;
        int muts = 1 + (int)rndu(24);
        memcpy(buf, src, len);
        for (int m = 0; m < muts; m++) {
            size_t pos;
            switch (rndu(4)) {
            case 0: pos = rndu(512) % len; break;                    /* directory, headers */
            case 1: pos = rndu((uint32_t)len); break;
            default: pos = rndu((uint32_t)len); buf[pos] ^= (uint8_t)(1u << rndu(8)); continue;
            }
            buf[pos] = rnd8();
        }
        if (rndu(8) == 0) n = 12 + rndu((uint32_t)(len - 12));     /* truncation */
        if (ui_font_load_mem(buf, n, 0, 0, &f) == PC_OK) {
            ui_text_style st;
            ui_a8 a8;
            ui_font_metrics m;
            loaded++;
            ui_font_get_metrics(f, 18.0f, &m);
            (void)ui_text_width(f, 15.0f, "AVTo kerning WAy 0123", 21);
            memset(&st, 0, sizeof st);
            st.size_px = 14.0f;
            st.flags = UI_TEXT_AA | (rndu(2) ? UI_TEXT_BOLD : 0u) | (rndu(2) ? UI_TEXT_ITALIC : 0u);
            if (ui_text_raster(f, "Hamburgefonts AV\xC3\xA9", 17, &st, &a8) == PC_OK) {
                CHECK(a8.w > 0 && a8.h > 0);
                ui_a8_free(&a8);
            }
            ui_font_free(f);
        }
    }
    INFO("fuzz: %d of %d mutated fonts loaded", loaded, iters);
    free(buf);
}

static void t_fuzz_fonts(void)
{
    static const char *const paths[] = { "data/inter-ttf-subset.ttf", "data/inter-cff-subset.otf",
                                         "data/noto-cjk-subset.otf" };
    for (size_t k = 0; k < 3; k++) {
        size_t len = 0;
        uint8_t *d = read_all(paths[k], &len);
        CHECK(d != NULL);
        if (!d) continue;
        fuzz_one(d, len, g_quick ? 120 : 4000);
        free(d);
    }
}

/* ---- measurement --------------------------------------------------------- */
static void t_measure(void)
{
    ui_font *f = ui_font_load_builtin(UI_FONT_REGULAR);
    const char *s = "Kerning: AV To Wa \xC3\xA9t\xC3\xA9 \xD0\x96\xD0\xB8\xD0\xB7\xD0\xBD\xD1\x8C";
    size_t n = strlen(s);
    float w;
    if (!f) { CHECK(f != NULL); return; }
    CHECK(ui_text_width(f, 20.0f, "AV", 2) <
          ui_text_width(f, 20.0f, "A", 1) + ui_text_width(f, 20.0f, "V", 1) - 0.5f);
    CHECK(ui_text_width(f, 20.0f, "To", 2) <
          ui_text_width(f, 20.0f, "T", 1) + ui_text_width(f, 20.0f, "o", 1) - 0.5f);
    w = ui_text_width(f, 13.0f, s, n);
    CHECK(w > 100.0f && w < 400.0f);
    CHECK(fabsf(ui_text_caret_x(f, 13.0f, s, n, n) - w) < 1e-3f);
    CHECK(ui_text_fit(f, 13.0f, s, n, w + 0.5f) == n);
    CHECK(ui_text_fit(f, 13.0f, s, n, 0.0f) == 0);
    {
        size_t prev = 0;
        for (float x = 0.0f; x < w; x += 7.0f) {
            size_t k = ui_text_fit(f, 13.0f, s, n, x);
            CHECK(k >= prev);
            CHECK(ui_text_caret_x(f, 13.0f, s, n, k) <= x + 0.02f);
            prev = k;
        }
    }
    for (size_t i = 0; i < n; i = ui_utf8_next(s, n, i)) {
        float x0 = ui_text_caret_x(f, 13.0f, s, n, i),
              x1 = ui_text_caret_x(f, 13.0f, s, n, ui_utf8_next(s, n, i));
        CHECK(x1 >= x0);
        if (x1 - x0 > 2.0f) CHECK(ui_text_hit(f, 13.0f, s, n, x0 + 0.25f * (x1 - x0)) == i);
    }
    CHECK(ui_text_hit(f, 13.0f, s, n, -5.0f) == 0);
    CHECK(ui_text_hit(f, 13.0f, s, n, w + 50.0f) == n);
    /* missing glyphs advance by a box, control characters by nothing */
    CHECK(fabsf(ui_text_width(f, 10.0f, "\xE6\xB0\xB4", 3) - 6.0f) < 1e-3f);
    CHECK(ui_text_width(f, 10.0f, "\t\n", 2) == 0.0f);
    ui_font_free(f);
}

static void t_fallback(void)
{
    ui_font *f = ui_font_load_builtin(UI_FONT_REGULAR), *cjk = NULL;
    const char *water = "\xE6\xB0\xB4";
    CHECK(ui_font_load_file("data/noto-cjk-subset.otf", 0, &cjk) == PC_OK);
    if (!f || !cjk) { ui_font_free(f); ui_font_free(cjk); return; }
    CHECK(ui_font_has_glyph(cjk, 0x6C34));
    CHECK(fabsf(ui_text_width(f, 20.0f, water, 3) - 12.0f) < 1e-3f);
    CHECK(ui_font_add_fallback(f, cjk) == PC_OK);
    CHECK(ui_font_add_fallback(f, f) == PC_ERR_ARG);
    CHECK(fabsf(ui_text_width(f, 20.0f, water, 3) - 20.0f) < 0.5f);   /* full-width ideograph */
    for (int i = 1; i < UI_FONT_MAX_FALLBACKS; i++) CHECK(ui_font_add_fallback(f, cjk) == PC_OK);
    CHECK(ui_font_add_fallback(f, cjk) == PC_ERR_LIMIT);
    {
        ui_text_style st;
        ui_a8 a;
        memset(&st, 0, sizeof st);
        st.size_px = 24.0f;
        st.flags = UI_TEXT_AA;
        CHECK(ui_text_raster(f, "A\xE6\xB0\xB4", 4, &st, &a) == PC_OK);
        CHECK(a.w > 30);
        ui_a8_free(&a);
    }
    ui_font_free(f);
    ui_font_free(cjk);
}

/* ---- A8 text ------------------------------------------------------------- */
static void a8_stats(const ui_a8 *a, uint64_t *sum, int *partial, int *rows_inked, int *x0, int *x1)
{
    *sum = 0;
    *partial = 0;
    *rows_inked = 0;
    *x0 = a->w;
    *x1 = -1;
    for (int y = 0; y < a->h; y++) {
        bool any = false;
        for (int x = 0; x < a->w; x++) {
            uint8_t v = a->px[(size_t)y * (size_t)a->stride + (size_t)x];
            *sum += v;
            if (v > 0 && v < 255) (*partial)++;
            if (v) {
                any = true;
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
            }
        }
        if (any) (*rows_inked)++;
    }
}

static void t_text_raster(void)
{
    ui_font *f = ui_font_load_builtin(UI_FONT_REGULAR);
    ui_text_style st;
    ui_a8 aa, al, bold, ital, ul;
    ui_text_box box;
    uint64_t s_aa, s_al, s_b, s_i, s_u;
    int p_aa, p_al, p_b, p_i, p_u, r_aa, r_al, r_b, r_i, r_u, x0, x1, ix0, ix1;
    float cx, cy, ch;
    if (!f) { CHECK(f != NULL); return; }
    memset(&st, 0, sizeof st);
    st.size_px = 64.0f;
    st.flags = UI_TEXT_AA;
    CHECK(ui_text_raster(f, "HIH", 3, &st, &aa) == PC_OK);
    a8_stats(&aa, &s_aa, &p_aa, &r_aa, &x0, &x1);
    CHECK(p_aa > 0);
    /* cap height of H at 64 px is about 46.6 px */
    CHECK(r_aa >= 46 && r_aa <= 48);
    CHECK(aa.y < 64 && aa.y > 0);            /* the ink starts below the line top */
    st.flags = 0;
    CHECK(ui_text_raster(f, "HIH", 3, &st, &al) == PC_OK);
    a8_stats(&al, &s_al, &p_al, &r_al, &x0, &x1);
    CHECK(p_al == 0);
    CHECK(llabs((long long)s_al - (long long)s_aa) < (long long)(s_aa / 20u));
    st.flags = UI_TEXT_AA | UI_TEXT_BOLD;
    CHECK(ui_text_raster(f, "HIH", 3, &st, &bold) == PC_OK);
    a8_stats(&bold, &s_b, &p_b, &r_b, &x0, &x1);
    CHECK(s_b > s_aa + s_aa / 10u);
    st.flags = UI_TEXT_AA | UI_TEXT_ITALIC;
    CHECK(ui_text_raster(f, "HIH", 3, &st, &ital) == PC_OK);
    a8_stats(&ital, &s_i, &p_i, &r_i, &ix0, &ix1);
    CHECK(ital.w > aa.w + 6);
    CHECK(llabs((long long)s_i - (long long)s_aa) < (long long)(s_aa / 20u));
    st.flags = UI_TEXT_AA | UI_TEXT_UNDERLINE;
    CHECK(ui_text_raster(f, "HIH", 3, &st, &ul) == PC_OK);
    a8_stats(&ul, &s_u, &p_u, &r_u, &x0, &x1);
    CHECK(ul.h > aa.h);
    CHECK(s_u > s_aa);
    {
        /* the underline row below the baseline spans the advance width */
        int row = ul.h - 1, inked = 0;
        for (int y = ul.h - 1; y >= 0 && !inked; y--) {
            int c = 0;
            for (int x = 0; x < ul.w; x++)
                c += ul.px[(size_t)y * (size_t)ul.stride + (size_t)x] > 128;
            if (c > ul.w / 2) { inked = c; row = y; }
        }
        CHECK(inked > 0 && row + ul.y > (int)(64.0f * 1984.0f / 2048.0f));
    }
    /* multi-line layout, alignment, caret and hit testing */
    st.flags = UI_TEXT_AA;
    st.align = UI_ALIGN_CENTER;
    st.size_px = 20.0f;
    CHECK(ui_text_layout(f, "ab\nlonger line\nx", 16, &st, &box) == PC_OK);
    CHECK(box.lines == 3);
    CHECK(fabsf(box.h - 3.0f * box.line_advance) < 1e-3f);
    CHECK(fabsf(box.w - ui_text_width(f, 20.0f, "longer line", 11)) < 1.5f);
    CHECK(ui_text_caret(f, "ab\nlonger line\nx", 16, &st, 3, &cx, &cy, &ch) == PC_OK);
    CHECK(fabsf(cx) < 1e-3f && fabsf(cy - box.line_advance) < 1e-3f && ch > 20.0f);
    CHECK(ui_text_caret(f, "ab\nlonger line\nx", 16, &st, 0, &cx, &cy, &ch) == PC_OK);
    CHECK(cx > 10.0f && cy == 0.0f);                       /* centered short line */
    CHECK(ui_text_hit_point(f, "ab\nlonger line\nx", 16, &st, 0.0f, box.line_advance * 1.5f) == 3);
    CHECK(ui_text_hit_point(f, "ab\nlonger line\nx", 16, &st, 1e4f, box.line_advance * 2.5f) == 16);
    st.line_spacing = 1.5f;
    CHECK(ui_text_layout(f, "a\nb", 3, &st, &box) == PC_OK);
    CHECK(box.lines == 2 && box.h > 2.9f * 20.0f);
    /* limits */
    ui_a8_free(&aa);
    st.size_px = 0.0f;
    CHECK(ui_text_raster(f, "a", 1, &st, &aa) == PC_ERR_ARG && aa.px == NULL);
    st.size_px = 5000.0f;
    CHECK(ui_text_layout(f, "a", 1, &st, &box) == PC_ERR_ARG);
    {
        size_t n = 4000;
        char *big = (char *)malloc(n + 1u);
        ui_a8 none;
        if (big) {
            memset(big, 'W', n);
            big[n] = '\0';
            st.size_px = 2000.0f;
            CHECK(ui_text_raster(f, big, n, &st, &none) == PC_ERR_LIMIT && none.px == NULL);
            free(big);
        }
    }
    st.size_px = 12.0f;
    {
        ui_a8 empty;
        CHECK(ui_text_raster(f, "", 0, &st, &empty) == PC_OK && empty.w >= 1 && empty.h >= 1);
        ui_a8_free(&empty);
        ui_a8_free(&empty);
    }
    ui_a8_free(&al);
    ui_a8_free(&bold);
    ui_a8_free(&ital);
    ui_a8_free(&ul);
    ui_font_free(f);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_utf8_decode);
    RUN(t_utf8_encode_roundtrip);
    RUN(t_utf8_boundaries);
    RUN(t_builtin_fonts);
    RUN(t_load_files);
    RUN(t_collections_and_errors);
    RUN(t_measure);
    RUN(t_fallback);
    RUN(t_text_raster);
    RUN(t_fuzz_fonts);
    SDL_Quit();
    return pc_test_finish();
}
