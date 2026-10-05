/* test_tools_fonts.c - lane TOOLS (wave 4 items 31 and 46): the Text
 * tool's fonts.
 *   t_symbol_previews  symbol fonts (Symbol code page, PANOSE symbol class,
 *                      glyph names that are not the letters', known TeX and
 *                      dingbat families) list their names in the UI font
 *                      with a sample of their own characters; text fonts
 *                      keep the name in their own face;
 *   t_tall_rows        a face whose glyphs reach far below the baseline is
 *                      scaled into its row of the font list and nothing is
 *                      drawn into the rows below (on screen);
 *   t_cjk_fallbacks    Korean and Japanese families of Windows and macOS
 *                      are fallbacks (Hangul typed in Inter finds Malgun
 *                      Gothic), ordered by the user's language.
 * Synthetic fonts built from the OpenType specification (toolb_test_fonts.h
 * helpers). Headless, single threaded. */
#include "pc_test.h"
#include "app_test_util.h"
#include "toolb_test_fonts.h"

#include "tools/paint_common.h"
#include "tools/text_font.h"

#include <math.h>

typedef struct font_spec {
    const char *family;
    const char *file;
    uint32_t    codepage1;        /* OS/2 ulCodePageRange1 */
    uint8_t     panose0;          /* PANOSE bFamilyType */
    bool        wrong_names;      /* post format 2: the letters' glyph is "arrowleft" */
    int16_t     y0, y1;           /* the letter glyph's box */
    const uint32_t *extra;        /* more code points mapped to the glyph (sorted) */
    size_t      nextra;
} font_spec;

static void name_os2(tfb *f, const font_spec *sp)
{
    pc_buf *b = tfb_add(f, "name");
    size_t fl = strlen(sp->family) * 2u;
    tb16(b, 0); tb16(b, 3); tb16(b, 6 + 12 * 3);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 1); tb16(b, (uint32_t)fl); tb16(b, 0);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 2); tb16(b, 14); tb16(b, (uint32_t)fl);
    tb16(b, 3); tb16(b, 1); tb16(b, 0x409); tb16(b, 4); tb16(b, (uint32_t)fl);
    tb16(b, (uint32_t)fl + 14u);
    tf_utf16(b, sp->family);
    tf_utf16(b, "Regular");
    tf_utf16(b, sp->family);
    b = tfb_add(f, "OS/2");
    tb16(b, 4); tbs16(b, 500); tb16(b, 400); tb16(b, 5); tb16(b, 0);
    for (int i = 0; i < 10; i++) tbs16(b, 50);
    tbs16(b, 0);
    tb8(b, sp->panose0);
    for (int i = 1; i < 10; i++) tb8(b, 0);
    for (int i = 0; i < 4; i++) tb32(b, 0);
    (void)pc_buf_append(b, "TOOL", 4u);
    tb16(b, 0x40); tb16(b, 0x20); tb16(b, 0xFFFF);
    tbs16(b, 800); tbs16(b, -200); tbs16(b, 0); tb16(b, 800); tb16(b, 200);
    tb32(b, sp->codepage1); tb32(b, 0);
    tbs16(b, 500); tbs16(b, 700); tb16(b, 0); tb16(b, 32); tb16(b, 2);
}

static void post2(tfb *f)
{
    static const char nm[] = "arrowleft";
    pc_buf *b = tfb_add(f, "post");
    tb32(b, 0x00020000u);
    for (int i = 0; i < 7; i++) tb32(b, 0);          /* italic angle ... maxMemType1 */
    tb16(b, 2);                                       /* glyphs */
    tb16(b, 0);                                       /* .notdef */
    tb16(b, 258);                                     /* the first custom name */
    tb8(b, (uint32_t)(sizeof nm - 1u));
    (void)pc_buf_append(b, nm, sizeof nm - 1u);
}

static bool make_font(const font_spec *sp, pc_buf *out)
{
    static const uint16_t adv[2] = { 500, 600 };
    tf_map map[96];
    size_t n = 0;
    int16_t box[2][4];
    tfb f;
    memset(&f, 0, sizeof f);
    for (uint32_t c = '0'; c <= 'z'; c++) {
        if ((c > '9' && c < 'A') || (c > 'Z' && c < 'a')) continue;
        map[n].cp = c;
        map[n++].gid = 1u;
    }
    for (size_t i = 0; i < sp->nextra && n < 96u; i++) {
        map[n].cp = sp->extra[i];
        map[n++].gid = 1u;
    }
    box[0][0] = 50; box[0][1] = 0; box[0][2] = 450; box[0][3] = 700;
    box[1][0] = 50; box[1][1] = sp->y0; box[1][2] = 550; box[1][3] = sp->y1;
    tf_head(&f, true);
    tf_hhea_hmtx_maxp(&f, 2u, adv);
    name_os2(&f, sp);
    if (sp->wrong_names) post2(&f);
    tf_cmap(&f, map, n, false);
    tf_glyf(&f, (const int16_t (*)[4])box, 2u);
    tfb_finish(&f, false, out);
    tfb_free(&f);
    return out->n > 0u && out->p != NULL;
}

static const uint32_t k_hangul[2] = { 0xAE00u, 0xD55Cu };   /* sorted */
static const uint32_t k_kana[1] = { 0x3042u };
static const uint32_t k_han[2] = { 0x3042u, 0x4E2Du };

static const font_spec k_fonts[] = {
    { "Plain Test", "plain.ttf", 1u, 2u, false, 0, 700, NULL, 0 },
    { "Pi Symbols Test", "pi.ttf", 0x80000000u, 2u, false, 0, 700, NULL, 0 },
    { "Panose Test", "panose.ttf", 1u, 5u, false, 0, 700, NULL, 0 },
    { "Named Test", "named.ttf", 1u, 2u, true, 0, 700, NULL, 0 },
    { "cmsy10", "cmsy10.ttf", 1u, 2u, false, 0, 700, NULL, 0 },
    { "Tall Test", "tall.ttf", 1u, 2u, false, -2600, 900, NULL, 0 },
    { "Malgun Gothic", "malgun.ttf", 1u, 2u, false, 0, 700, k_hangul, 2 },
    { "Yu Gothic", "yugothic.ttf", 1u, 2u, false, 0, 700, k_kana, 1 },
    { "Microsoft YaHei", "yahei.ttf", 1u, 2u, false, 0, 700, k_han, 2 },
};
#define NFONTS ((int)(sizeof k_fonts / sizeof k_fonts[0]))

/* Write the fonts into a folder and install them in a's catalog. */
static text_fonts *install(app *a)
{
    char root[1024], path[1024];
    const char *dirs[2];
    text_face_info *found = NULL;
    size_t nfound = 0;
    text_fonts *tf;
    at_out_path(root, sizeof root, "test_tools_fonts");
    CHECK(pal_mkdirs(root));
    for (int i = 0; i < NFONTS; i++) {
        pc_buf b;
        memset(&b, 0, sizeof b);
        CHECK(make_font(&k_fonts[i], &b));
        pal_path_join(path, sizeof path, root, k_fonts[i].file);
        CHECK(pal_write_file_atomic(path, b.p, b.n) == PC_OK);
        pc_buf_free(&b);
    }
    dirs[0] = root;
    dirs[1] = NULL;
    CHECK(text_fonts_scan_dirs(dirs, NULL, 0, NULL, &found, &nfound) == PC_OK);
    CHECK(nfound == (size_t)NFONTS);
    tf = text_fonts_get(a);
    CHECK(tf != NULL && text_fonts_set_faces(tf, found, nfound) == PC_OK);
    free(found);
    return tf;
}

static bool info_of(text_fonts *tf, const char *family, text_preview *pv)
{
    int32_t fi = text_fonts_find_family(tf, family);
    if (fi < 0) return false;
    for (uint32_t fr = 1; fr < 20u; fr++)
        if (text_fonts_preview_info(tf, fi, fr, pv)) return true;
    return false;
}

static void t_symbol_previews(void)
{
    static const char *const symbols[] = { "Pi Symbols Test", "Panose Test", "Named Test",
                                           "cmsy10" };
    app *a = at_app(800, 600);
    text_fonts *tf;
    text_preview pv;
    CHECK(a != NULL);
    if (!a) return;
    tf = install(a);
    if (!tf) {
        app_destroy(a);
        return;
    }
    /* a text font: its name in its own face */
    CHECK(info_of(tf, "Plain Test", &pv));
    CHECK(pv.face != NULL && pv.name_in_face && pv.sample[0] == '\0');
    CHECK(text_fonts_preview(tf, text_fonts_find_family(tf, "Plain Test"), 50u) == pv.face);
    for (size_t i = 0; i < sizeof symbols / sizeof symbols[0]; i++) {
        CHECK(info_of(tf, symbols[i], &pv));
        /* the name in the UI font, a sample of its characters in it */
        CHECK(pv.face != NULL && !pv.name_in_face);
        CHECK(strcmp(pv.sample, "ABCDEF") == 0);
        CHECK(text_fonts_preview(tf, text_fonts_find_family(tf, symbols[i]), 50u) == NULL);
        if (pv.name_in_face) INFO("%s is shown in its own face", symbols[i]);
    }
    /* the built-in Inter is a text font too */
    CHECK(info_of(tf, "Inter", &pv) && pv.name_in_face);
    app_destroy(a);
}

/* ---- tall glyphs -------------------------------------------------------------------- */
static void type_text(app *a, const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = s;
    app_event(a, &e);
    at_frames(a, 2);
}

static int ink_rows(app *a, ui_rect r, int y0, int y1, uint32_t bg)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = r.x + 4; x < r.x + r.w - 20; x++) {
            uint32_t c = at_pixel(a, x, y);
            int d = abs((int)(c >> 16) - (int)(bg >> 16)) +
                    abs((int)((c >> 8) & 0xFFu) - (int)((bg >> 8) & 0xFFu)) +
                    abs((int)(c & 0xFFu) - (int)(bg & 0xFFu));
            if (d > 60) n++;
        }
    return n;
}

static void t_tall_rows(void)
{
    app *a = at_app(1200, 800);
    app_doc *d;
    text_fonts *tf;
    text_preview pv;
    ui_rect br, lr;
    int32_t rh;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 300, 200, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    tf = install(a);
    CHECK(app_tool_select(a, "text"));
    at_frames(a, 3);
    CHECK(info_of(tf, "Tall Test", &pv));
    CHECK(pv.name_in_face && fabsf(pv.ink_bottom - 2.6f) < 1e-3f &&
          fabsf(pv.ink_top - 0.9f) < 1e-3f);
    /* open the font list and search for the tall face */
    CHECK(paint_widget_rect(a, "##text_font", &br));
    if (!paint_widget_rect(a, "##text_font", &br)) {
        app_destroy(a);
        return;
    }
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(br.x + br.w / 2), (float)(br.y + br.h / 2), 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, (float)(br.x + br.w / 2), (float)(br.y + br.h / 2),
             SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(br.x + br.w / 2), (float)(br.y + br.h / 2),
             SDL_BUTTON_LEFT);
    at_frames(a, 3);
    type_text(a, "Tall");
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 5.0f, 790.0f, 0);       /* no hover highlight */
    for (int i = 0; i < 6; i++) at_frames(a, 1);                /* preview faces load */
    CHECK(paint_widget_rect(a, "##text_font_list", &lr));
    if (paint_widget_rect(a, "##text_font_list", &lr)) {
        uint32_t bg = at_pixel(a, lr.x + lr.w - 30, lr.y + lr.h - 10);
        int in_row, below;
        rh = ui_px(a->ui, 28.0f);
        in_row = ink_rows(a, lr, lr.y, lr.y + rh, bg);
        below = ink_rows(a, lr, lr.y + rh + 1, lr.y + 4 * rh, bg);
        INFO("tall face row: %d ink pixels in its row, %d in the rows below", in_row, below);
        CHECK(in_row > 30);
        CHECK(below == 0);
    }
    app_destroy(a);
}

/* ---- CJK fallbacks -------------------------------------------------------------------- */
static int pos_of(const char *const *v, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i], name) == 0) return (int)i;
    return -1;
}

static void t_cjk_fallbacks(void)
{
    const char *v[64];
    size_t n;
    app *a;
    text_fonts *tf;
    const pc_font_face *const *faces = NULL;
    size_t nf = 0;
    int hangul = -1, kana = -1;
    /* the order by language */
    n = text_fonts_fallback_order("en_US", v, 64u);
    CHECK(pos_of(v, n, "Malgun Gothic") >= 0 && pos_of(v, n, "Apple SD Gothic Neo") >= 0);
    CHECK(pos_of(v, n, "Yu Gothic") >= 0 && pos_of(v, n, "Meiryo") >= 0);
    CHECK(pos_of(v, n, "Hiragino Sans") >= 0);
    CHECK(pos_of(v, n, "Microsoft YaHei") < pos_of(v, n, "Yu Gothic"));
    CHECK(pos_of(v, n, "Yu Gothic") < pos_of(v, n, "Malgun Gothic"));
    CHECK(pos_of(v, n, "DejaVu Sans") == 0);
    CHECK(pos_of(v, n, "Apple Symbols") == (int)n - 1);
    n = text_fonts_fallback_order("ko_KR", v, 64u);
    CHECK(pos_of(v, n, "Malgun Gothic") < pos_of(v, n, "Microsoft YaHei"));
    CHECK(pos_of(v, n, "Apple SD Gothic Neo") < pos_of(v, n, "PingFang SC"));
    n = text_fonts_fallback_order("ja", v, 64u);
    CHECK(pos_of(v, n, "Yu Gothic UI") < pos_of(v, n, "Microsoft YaHei"));
    CHECK(pos_of(v, n, "Hiragino Sans") < pos_of(v, n, "PingFang SC"));
    n = text_fonts_fallback_order("zh_TW", v, 64u);
    CHECK(pos_of(v, n, "Microsoft JhengHei") < pos_of(v, n, "Microsoft YaHei"));
    n = text_fonts_fallback_order(NULL, v, 64u);
    CHECK(pos_of(v, n, "Malgun Gothic") >= 0);
    CHECK(text_fonts_fallback_order("en", v, 3u) == 3u);
    /* Hangul and kana typed in the default font find the installed faces */
    a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    tf = install(a);
    CHECK(tf && text_fonts_faces(tf, TEXT_DEFAULT_FAMILY, false, false, &faces, &nf) == PC_OK);
    for (size_t i = 0; i < nf; i++) {
        if (hangul < 0 && faces[i]->has_glyph(faces[i]->ud, 0xD55Cu)) hangul = (int)i;
        if (kana < 0 && faces[i]->has_glyph(faces[i]->ud, 0x3042u)) kana = (int)i;
    }
    INFO("faces for Inter: %zu, Hangul from face %d, kana from face %d", nf, hangul, kana);
    CHECK(hangul > 0 && kana > 0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_symbol_previews);
    RUN(t_tall_rows);
    RUN(t_cjk_fallbacks);
    at_quit();
    return pc_test_finish();
}
