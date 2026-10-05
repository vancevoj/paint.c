/* test_fxc_resize.c - lane W3B-FXCORE: Image > Resize with "Use gamma
 * correction" linearizes with the image profile's transfer curve (MENUS
 * Resize "sRGB transfer, or the image profile's"; paint.c keeps pixels in
 * the image's own profile).
 *  - the core ICC curve reader (pc_trc_new_icc) agrees with the app's own
 *    matrix/TRC parser (m_icc) on the built-in Adobe RGB, Display P3 and
 *    ProPhoto profiles (two independent readings of the same bytes);
 *  - app_doc_trc: no profile and sRGB profiles use the exact built-in sRGB
 *    curve (NULL), other profiles their own curves, cached per profile;
 *  - the Resize dialog on an Adobe RGB image mixes black and white to
 *    255 * 0.5^(1/2.2) = 186 (sRGB would give 188, no gamma 128);
 *  - Move Selected Pixels with Gamma Corrected squeezes black and white
 *    rows through the same curve (tagged Adobe RGB darker than untagged).
 */
#include "pc_test.h"
#include "f_test_util.h"
#include "a_util.h"
#include "doc_trc.h"
#include "edit/m_icc.h"
#include "tools/sel_float.h"

static void text_ev(app *a, const char *t)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = t;
    app_event(a, &e);
    at_frames(a, 1);
}

static void t_cross_check(void)
{
    static const m_icc_builtin k[3] = { M_ICC_ADOBE_RGB, M_ICC_DISPLAY_P3, M_ICC_PROPHOTO };
    for (int i = 0; i < 3; i++) {
        uint8_t *icc = NULL;
        size_t len = 0;
        m_icc_rgb rgb;
        pc_trc *t = NULL;
        double worst = 0.0;
        CHECK(m_icc_builtin_profile(k[i], &icc, &len) == PC_OK);
        CHECK(m_icc_parse(icc, len, &rgb) == PC_OK);
        CHECK(pc_trc_new_icc(icc, len, &t) == PC_OK);
        if (t) {
            for (int c = 0; c < 3; c++)              /* m_icc: R, G, B; pc_trc: B, G, R */
                for (int v = 0; v < 256; v++) {
                    double a = 255.0 * m_icc_curve_eval(&rgb.trc[2 - c], (double)v / 255.0);
                    double d = fabs(a - (double)t->dec[c][v]);
                    if (d > worst) worst = d;
                }
        }
        INFO("%s: largest decode difference %.5f", m_icc_builtin_name(k[i]), worst);
        CHECK(worst < 0.01);
        pc_trc_free(t);
        free(icc);
    }
}

static void set_profile(app_doc *d, m_icc_builtin b)
{
    uint8_t *icc = NULL;
    size_t len = 0;
    free(d->meta.icc);
    d->meta.icc = NULL;
    d->meta.icc_len = 0;
    if (m_icc_builtin_profile(b, &icc, &len) == PC_OK) {
        d->meta.icc = icc;
        d->meta.icc_len = len;
    }
}

static void t_doc_trc(void)
{
    app *a = f_app(8, 8);
    app_doc *d = a ? app_active_doc(a) : NULL;
    const pc_trc *t1, *t2;
    CHECK(d != NULL);
    if (!d) {
        if (a) app_destroy(a);
        return;
    }
    CHECK(app_doc_trc(a, d) == NULL);                       /* untagged: sRGB */
    set_profile(d, M_ICC_SRGB);
    CHECK(app_doc_trc(a, d) == NULL);                       /* sRGB profile: exact sRGB */
    set_profile(d, M_ICC_ADOBE_RGB);
    t1 = app_doc_trc(a, d);
    CHECK(t1 != NULL);
    CHECK(app_doc_trc(a, d) == t1);                          /* cached */
    if (t1) CHECK(fabs(t1->dec[1][128] - 255.0 * pow(128.0 / 255.0, 563.0 / 256.0)) < 0.01);
    set_profile(d, M_ICC_PROPHOTO);                          /* a new profile: new curve */
    t2 = app_doc_trc(a, d);
    CHECK(t2 != NULL);
    if (t2) CHECK(fabs(t2->dec[1][128] - 255.0 * pow(128.0 / 255.0, 1.8)) < 0.05);
    /* garbage bytes as a profile fall back to sRGB */
    free(d->meta.icc);
    d->meta.icc = (uint8_t *)calloc(1u, 300u);
    d->meta.icc_len = d->meta.icc ? 300u : 0u;
    CHECK(app_doc_trc(a, d) == NULL);
    app_destroy(a);
}

/* 2 x 2 image, left column black, right white; Resize to width 1. */
static uint8_t resize_mix(m_icc_builtin profile, bool tagged)
{
    app *a = at_app(1280, 860);
    app_doc *d = NULL;
    uint8_t v = 0;
    if (!a) return 0;
    d = app_doc_new_image(a, 2u, 2u, app_px_make(0, 0, 0, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return 0;
    }
    at_frames(a, 3);
    {
        pc_layer *l = app_doc_layer(d);
        pc_txn *t = pc_txn_begin(d->doc, "white");
        pc_px32 w[2] = { {255, 255, 255, 255}, {255, 255, 255, 255} };
        if (t && l) {
            (void)pc_txn_write_rect(t, l->id, pc_rect_make(1, 0, 1, 2), w, 1u);
            (void)pc_txn_commit(t, d->hist);
        } else if (t) {
            pc_txn_cancel(t);
        }
        app_doc_history_changed(a, d);
    }
    if (tagged) set_profile(d, profile);
    CHECK(app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    f_key(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "1");
    f_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    if (d->doc->w == 1u && d->doc->h == 1u)
        v = pc_layer_get_px(app_doc_layer(d), 0, 0).g;
    else
        CHECK(!"resize did not happen");
    app_destroy(a);
    return v;
}

static void t_resize_dialog(void)
{
    uint8_t untagged = resize_mix(M_ICC_SRGB, false);
    uint8_t srgb = resize_mix(M_ICC_SRGB, true);
    uint8_t adobe = resize_mix(M_ICC_ADOBE_RGB, true);
    double want = floor(255.0 * pow(0.5, 256.0 / 563.0) + 0.5);
    INFO("Resize 50/50 mix: untagged %u, sRGB %u, Adobe RGB %u (want %.0f)", (unsigned)untagged,
         (unsigned)srgb, (unsigned)adobe, want);
    CHECK(untagged == 188 && srgb == 188);
    CHECK((double)adobe == want);
}

/* Move Selected Pixels, Bilinear, Gamma Corrected: 1 px black and white
 * rows squeezed to half height blend 50 / 50 in the image's linear light. */
static uint8_t move_mix(bool tagged)
{
    app *a = a_app(100, 80, app_px_make(255, 255, 255, 255));
    uint8_t v = 0;
    if (!a) return 0;
    for (int32_t y = 20; y < 60; y += 2)
        a_fill(a, pc_rect_make(20, y, 40, 1), a_px(0, 0, 0, 255));
    if (tagged) set_profile(a_doc(a), M_ICC_ADOBE_RGB);
    sel_move_pixels_quality(a, SEL_RS_BILINEAR, true);
    (void)app_tool_select(a, "rect_select");
    a_drag(a, 20, 20, 60, 60, SDL_BUTTON_LEFT, 0u);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 40, 60, 40, 40, SDL_BUTTON_LEFT, 0u);
    v = a_lpx(a, 40, 30).r;
    app_destroy(a);
    return v;
}

static void t_move_gamma(void)
{
    uint8_t plain = move_mix(false), adobe = move_mix(true);
    INFO("Move Selected Pixels 50/50 rows: untagged %u, Adobe RGB %u", (unsigned)plain,
         (unsigned)adobe);
    CHECK(plain >= 182u && plain <= 194u);              /* sRGB linear light, about 188 */
    CHECK(adobe < plain && (unsigned)(plain - adobe) <= 6u);   /* gamma 2.2: about 186 */
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_cross_check);
    RUN(t_doc_trc);
    RUN(t_resize_dialog);
    RUN(t_move_gamma);
    at_quit();
    return pc_test_finish();
}
