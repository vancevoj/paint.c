/* test_m_profile.c - lane M: Image > Color Profile. The built-in ICC v4
 * profiles are accepted by Little-CMS (pc_icc_inspect) and sRGB is
 * recognized as sRGB; the parser reads them back exactly and survives
 * damaged input; the matrix/TRC conversion agrees with Little-CMS; Assign
 * and Convert are one undoable history step each. Fixed seeds. */
#include "pc_test.h"
#include "app_test_util.h"

#include "edit/m_icc.h"
#include "edit/m_profile.h"
#include "pc/pc_icc.h"

static double dabs(double v) { return v < 0.0 ? -v : v; }

static void t_builtin_profiles(void)
{
    for (int b = 0; b < (int)M_ICC_BUILTIN_COUNT; b++) {
        uint8_t *p = NULL;
        size_t n = 0;
        pc_icc_info info;
        m_icc_rgb parsed, direct;
        CHECK(m_icc_builtin_profile((m_icc_builtin)b, &p, &n) == PC_OK && p && n > 300u);
        if (!p) continue;
        CHECK(pc_icc_inspect(p, n, &info) == PC_OK);
        CHECK(info.space == PC_ICC_SPACE_RGB && info.version >= 0x04000000u);
        CHECK(strcmp(info.desc, m_icc_builtin_name((m_icc_builtin)b)) == 0);
        CHECK(info.is_srgb == (b == (int)M_ICC_SRGB));
        CHECK(m_icc_parse(p, n, &parsed) == PC_OK);
        m_icc_builtin_rgb((m_icc_builtin)b, &direct);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) CHECK(dabs(parsed.m[r][c] - direct.m[r][c]) < 1e-4);
        for (int i = 0; i <= 255; i += 15) {
            double x = (double)i / 255.0;
            CHECK(dabs(m_icc_curve_eval(&parsed.trc[1], x) -
                       m_icc_curve_eval(&direct.trc[1], x)) < 1e-4);
        }
        /* deterministic bytes */
        {
            uint8_t *q = NULL;
            size_t m = 0;
            CHECK(m_icc_builtin_profile((m_icc_builtin)b, &q, &m) == PC_OK && m == n &&
                  memcmp(p, q, n) == 0);
            free(q);
        }
        free(p);
    }
    {
        uint8_t *p = NULL;
        size_t n = 0;
        CHECK(m_icc_builtin_profile(M_ICC_BUILTIN_COUNT, &p, &n) == PC_ERR_ARG && !p);
    }
    /* sRGB primaries and white: Y of white is 1, D50 adapted */
    {
        m_icc_rgb s;
        m_icc_builtin_rgb(M_ICC_SRGB, &s);
        CHECK(dabs(s.m[1][0] + s.m[1][1] + s.m[1][2] - 1.0) < 1e-6);
        CHECK(dabs(s.m[0][0] + s.m[0][1] + s.m[0][2] - 0.9642) < 1e-3);
        CHECK(dabs(s.m[0][0] - 0.4360) < 2e-3 && dabs(s.m[1][1] - 0.7152) < 2e-3);
    }
}

static void t_parser_hardening(void)
{
    uint8_t *p = NULL;
    size_t n = 0;
    m_icc_rgb r;
    CHECK(m_icc_builtin_profile(M_ICC_ADOBE_RGB, &p, &n) == PC_OK);
    if (!p) return;
    CHECK(m_icc_parse(NULL, 0, &r) == PC_ERR_FORMAT);
    CHECK(m_icc_parse(p, 100, &r) == PC_ERR_FORMAT);
    for (size_t cut = 0; cut < n; cut += 7) (void)m_icc_parse(p, cut, &r);   /* truncations */
    {
        uint8_t *q = (uint8_t *)malloc(n);
        int ok = 0;
        CHECK(q != NULL);
        for (int it = 0; q && it < (g_quick ? 3000 : 30000); it++) {
            memcpy(q, p, n);
            for (int k = 0; k < 1 + (int)rndu(6); k++) q[rndu((uint32_t)n)] = rnd8();
            if (m_icc_parse(q, n, &r) == PC_OK) {
                m_icc_xform *x = (m_icc_xform *)malloc(sizeof *x);
                if (x) {
                    pc_px32 px[4] = { { 1, 2, 3, 4 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 },
                                      { 9, 99, 199, 0 } };
                    m_icc_rgb s;
                    m_icc_builtin_rgb(M_ICC_SRGB, &s);
                    if (m_icc_xform_init(x, &r, &s)) m_icc_xform_px(x, px, 4);
                    free(x);
                }
                ok++;
            }
        }
        CHECK(ok > 0);
        free(q);
    }
    /* not an RGB matrix profile: gray or no colorants */
    {
        uint8_t *q = (uint8_t *)malloc(n);
        if (q) {
            memcpy(q, p, n);
            q[16] = 'G';
            q[17] = 'R';
            q[18] = 'A';
            q[19] = 'Y';
            CHECK(m_icc_parse(q, n, &r) == PC_ERR_FORMAT);
            free(q);
        }
    }
    free(p);
}

/* My matrix/TRC conversion agrees with Little-CMS (pc_icc_to_srgb_px). */
static void t_against_lcms(void)
{
    enum { N = 4096 };
    pc_px32 *a = (pc_px32 *)malloc(N * sizeof *a), *b = (pc_px32 *)malloc(N * sizeof *b);
    m_icc_xform *x = (m_icc_xform *)malloc(sizeof *x);
    CHECK(a && b && x);
    if (!a || !b || !x) {
        free(a);
        free(b);
        free(x);
        return;
    }
    for (int bi = 1; bi < (int)M_ICC_BUILTIN_COUNT; bi++) {
        uint8_t *p = NULL;
        size_t n = 0;
        m_icc_rgb src, dst;
        int maxd = 0;
        CHECK(m_icc_builtin_profile((m_icc_builtin)bi, &p, &n) == PC_OK);
        if (!p) continue;
        for (int i = 0; i < N; i++) {
            a[i].r = rnd8();
            a[i].g = rnd8();
            a[i].b = rnd8();
            a[i].a = 255;
        }
        a[0].r = a[0].g = a[0].b = 0;
        a[1].r = a[1].g = a[1].b = 255;
        memcpy(b, a, N * sizeof *a);
        CHECK(pc_icc_to_srgb_px(p, n, a, N, 1, N) == PC_OK);
        m_icc_builtin_rgb((m_icc_builtin)bi, &src);
        m_icc_builtin_rgb(M_ICC_SRGB, &dst);
        CHECK(m_icc_xform_init(x, &src, &dst));
        m_icc_xform_px(x, b, N);
        for (int i = 0; i < N; i++) {
            int d0 = abs((int)a[i].r - (int)b[i].r), d1 = abs((int)a[i].g - (int)b[i].g),
                d2 = abs((int)a[i].b - (int)b[i].b);
            if (d0 > maxd) maxd = d0;
            if (d1 > maxd) maxd = d1;
            if (d2 > maxd) maxd = d2;
        }
        INFO("%s -> sRGB: max difference to Little-CMS %d", m_icc_builtin_name((m_icc_builtin)bi),
             maxd);
        CHECK(maxd <= 2);
        CHECK(b[0].r == 0 && b[1].g == 255);       /* black and white map exactly */
        free(p);
    }
    /* sRGB -> Display P3 -> sRGB in linear light: bounded by half a code of
     * the 8-bit intermediate (at most 0.5 / 255 * 2.4 = 0.0047 at the top of
     * the curve) times the largest row sum of the P3 -> sRGB matrix (1.45),
     * plus half a code of the final encoding (0.0047): 0.012 */
    {
        m_icc_rgb s, p3;
        m_icc_xform *y = (m_icc_xform *)malloc(sizeof *y);
        double maxd = 0.0;
        m_icc_builtin_rgb(M_ICC_SRGB, &s);
        m_icc_builtin_rgb(M_ICC_DISPLAY_P3, &p3);
        if (y && m_icc_xform_init(x, &s, &p3) && m_icc_xform_init(y, &p3, &s)) {
            for (int i = 0; i < N; i++) {
                a[i].r = rnd8();
                a[i].g = rnd8();
                a[i].b = rnd8();
                a[i].a = (uint8_t)(1u + rndu(255));
            }
            memcpy(b, a, N * sizeof *a);
            m_icc_xform_px(x, b, N);
            m_icc_xform_px(y, b, N);
            for (int i = 0; i < N; i++) {
                const uint8_t ca[3] = { a[i].r, a[i].g, a[i].b };
                const uint8_t cb[3] = { b[i].r, b[i].g, b[i].b };
                for (int k = 0; k < 3; k++) {
                    double d = dabs(m_icc_curve_eval(&s.trc[k], (double)ca[k] / 255.0) -
                                    m_icc_curve_eval(&s.trc[k], (double)cb[k] / 255.0));
                    if (d > maxd) maxd = d;
                }
                if (a[i].a != b[i].a) maxd = 1.0;
            }
            INFO("sRGB -> Display P3 -> sRGB: max linear difference %.5f", maxd);
            CHECK(maxd < 0.012);
        }
        free(y);
    }
    free(a);
    free(b);
    free(x);
}

static void t_assign_convert(void)
{
    app *a = at_app(1024, 768);
    app_doc *d;
    uint8_t *adobe = NULL;
    size_t n = 0;
    char desc[200];
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 100, 70, app_px_make(255, 0, 0, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_cmd_enabled(a, "image.color_profile"));
    CHECK(m_icc_builtin_profile(M_ICC_ADOBE_RGB, &adobe, &n) == PC_OK);
    m_profile_describe(d, desc, sizeof desc);
    CHECK(strstr(desc, "sRGB") != NULL && strstr(desc, "assumed") != NULL);
    /* Assign: the profile changes, the pixels do not; one step */
    CHECK(m_profile_assign(a, d, adobe, n) == PC_OK);
    CHECK(d->meta.icc && d->meta.icc_len == n && px_eq(at_doc_px(a, 5, 5), 255, 0, 0, 255));
    CHECK(strcmp(d->hist->cur->label, "Assign Color Profile") == 0 && app_doc_dirty(d));
    m_profile_describe(d, desc, sizeof desc);
    CHECK(strstr(desc, "Adobe RGB") != NULL);
    CHECK(m_profile_assign(a, d, adobe, n) == PC_ERR_STATE);         /* nothing changes */
    CHECK(app_cmd_exec(a, "edit.undo") && d->meta.icc == NULL && !app_doc_dirty(d));
    CHECK(app_cmd_exec(a, "edit.redo") && d->meta.icc != NULL);
    CHECK(app_cmd_exec(a, "edit.undo"));
    {
        uint8_t junk[200];
        memset(junk, 7, sizeof junk);
        CHECK(m_profile_assign(a, d, junk, sizeof junk) != PC_OK && d->meta.icc == NULL);
    }
    /* Convert sRGB -> Adobe RGB: red moves inside the wider gamut */
    CHECK(app_cmd_exec(a, "layers.add_new"));
    CHECK(m_profile_convert(a, d, adobe, n) == PC_OK);
    CHECK(strcmp(d->hist->cur->label, "Convert Color Profile") == 0);
    CHECK(d->meta.icc && d->meta.icc_len == n);
    {
        pc_px32 p = pc_layer_get_px(d->doc->stack[0], 5, 5);
        CHECK(p.r >= 214 && p.r <= 222 && p.g <= 2 && p.b <= 2 && p.a == 255);
        CHECK(pc_layer_get_px(d->doc->stack[1], 5, 5).a == 0u);    /* empty stays empty */
    }
    CHECK(pc_doc_edge_padding_is_zero(d->doc));
    /* undo restores pixels and profile in one step */
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(d->meta.icc == NULL && px_eq(pc_layer_get_px(d->doc->stack[0], 5, 5), 255, 0, 0, 255));
    CHECK(app_cmd_exec(a, "edit.redo") && d->meta.icc != NULL);
    /* and back to sRGB (no embedded profile) */
    CHECK(m_profile_convert(a, d, NULL, 0) == PC_OK && d->meta.icc == NULL);
    {
        pc_px32 p = pc_layer_get_px(d->doc->stack[0], 5, 5);
        CHECK(p.r >= 253 && p.g <= 2 && p.b <= 2);
    }
    CHECK(m_profile_convert(a, d, NULL, 0) == PC_ERR_STATE);
    {
        uint8_t junk[300];
        memset(junk, 3, sizeof junk);
        CHECK(m_profile_convert(a, d, junk, sizeof junk) == PC_ERR_UNSUPPORTED);
    }
    /* the dialog opens and closes */
    CHECK(app_cmd_exec(a, "image.color_profile"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_ESCAPE;
        e.key.down = true;
        app_event(a, &e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
        at_frames(a, 2);
    }
    CHECK(!app_dialog_active(a));
    free(adobe);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_builtin_profiles);
    RUN(t_parser_hardening);
    RUN(t_against_lcms);
    RUN(t_assign_convert);
    at_quit();
    return pc_test_finish();
}
