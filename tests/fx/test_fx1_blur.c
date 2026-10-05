/* test_fx1_blur.c - properties of the ten Blurs: identities, constant
 * images, mean preservation, kernel shapes against brute-force references,
 * direction of motion, salt-noise removal, edge preservation, alpha
 * (premultiplied blurring), and a timing run of Gaussian Blur at radius 100.
 */
#include "fx1_util.h"
#include "../src/fx/blur/fx1_lib.h"          /* engine internals (cache path) */

/* ---- helpers --------------------------------------------------------------- */
static fx_img t_render(const char *id, void *p, const fx_img *src, fx_rect sel, int *st)
{
    const fx_effect *fx = t_find(id);
    fx_img dst = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    int s = fx ? t_run1(fx, p, src, &dst, sel) : FX_ERROR;
    if (st) *st = s;
    return dst;
}
static fx_rect t_all(const fx_img *im) { return im->r; }

static int t_is_const(const fx_img *im, fx_px c)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++)
            if (!t_px_eq(fx_row(im, y)[x], c)) return 0;
    return 1;
}

static double t_mean(const fx_img *im, int ch)
{
    double s = 0;
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            fx_px p = fx_row(im, y)[x];
            s += ch == 0 ? p.b : ch == 1 ? p.g : ch == 2 ? p.r : p.a;
        }
    return s / ((double)im->r.w * im->r.h);
}

static double t_var(const fx_img *im, int ch)
{
    double m = t_mean(im, ch), s = 0;
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            fx_px p = fx_row(im, y)[x];
            double v = ch == 0 ? p.b : ch == 1 ? p.g : p.r;
            s += (v - m) * (v - m);
        }
    return s / ((double)im->r.w * im->r.h);
}

/* Opaque black image with one white pixel at (cx, cy). */
static fx_img t_dot(int32_t w, int32_t h, int32_t cx, int32_t cy)
{
    fx_img im = t_img_new(0, 0, w, h);
    t_img_fill(&im, fx_px_make(0, 0, 0, 255));
    fx_row(&im, cy)[cx] = fx_px_make(255, 255, 255, 255);
    return im;
}

/* Every effect with blur semantics keeps opaque constant images constant. */
static void t_constant(void)
{
    static const char *const ids[] = {
        "org.paintc.blur.bokeh", "org.paintc.blur.fragment", "org.paintc.blur.gaussian",
        "org.paintc.blur.median", "org.paintc.blur.motion", "org.paintc.blur.radial",
        "org.paintc.blur.sketch", "org.paintc.blur.square", "org.paintc.blur.surface",
        "org.paintc.blur.zoom" };
    const fx_px c = fx_px_make(201, 77, 13, 255);
    size_t i;
    int q;
    for (i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const fx_effect *fx = t_find(ids[i]);
        fx_img src = t_img_new(0, 0, 35, 27);
        void *p = t_params_new(fx);
        fx_img d;
        t_img_fill(&src, c);
        d = t_render(ids[i], p, &src, t_all(&src), NULL);
        CHECK(t_is_const(&d, c));
        if (!t_is_const(&d, c)) INFO("%s changes a constant image", ids[i]);
        t_img_free(&d);
        /* gamma boost and other qualities */
        if (t_prop(fx, "gamma_boost")) {
            for (q = 0; q < 3; q++) {
                t_set(fx, p, "gamma_boost", q == 0 ? -1.0 : (q == 1 ? 0.6 : 2.0));
                if (t_prop(fx, "quality")) t_set(fx, p, "quality", 1 + q);
                d = t_render(ids[i], p, &src, t_all(&src), NULL);
                CHECK(t_is_const(&d, c));
                if (!t_is_const(&d, c))
                    INFO("%s gamma case %d: (%d,%d,%d,%d)", ids[i], q, fx_row(&d, 0)[0].r,
                         fx_row(&d, 0)[0].g, fx_row(&d, 0)[0].b, fx_row(&d, 0)[0].a);
                t_img_free(&d);
            }
        }
        if (strstr(ids[i], "motion")) {          /* also with every edge behavior but Transparent */
            for (q = 0; q < 3; q++) {
                t_set(fx, p, "edge_behavior", q);
                t_set(fx, p, "centered", q & 1);
                d = t_render(ids[i], p, &src, t_all(&src), NULL);
                CHECK(t_is_const(&d, c));
                t_img_free(&d);
            }
        }
        free(p);
        t_img_free(&src);
    }
}

/* Neutral parameters return the source unchanged. */
static void t_identity(void)
{
    struct { const char *id, *key; double v; } cases[] = {
        { "org.paintc.blur.gaussian", "radius", 0.0 },
        { "org.paintc.blur.square", "radius", 0.0 },
        { "org.paintc.blur.bokeh", "radius", 0.4 },
        { "org.paintc.blur.fragment", "distance", 0.0 },
        { "org.paintc.blur.radial", "angle", 0.0 },
        { "org.paintc.blur.sketch", "radius", 0.0 },
        { "org.paintc.blur.zoom", "distance", 0.0 },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const fx_effect *fx = t_find(cases[i].id);
        fx_img src = t_img_new(0, 0, 30, 22), d;
        void *p = t_params_new(fx);
        t_img_random(&src, 1);
        t_set(fx, p, cases[i].key, cases[i].v);
        d = t_render(cases[i].id, p, &src, t_all(&src), NULL);
        CHECK(t_img_eq(&src, &d, t_all(&src)));
        if (!t_img_eq(&src, &d, t_all(&src))) INFO("%s is not neutral", cases[i].id);
        free(p);
        t_img_free(&src);
        t_img_free(&d);
    }
}

/* ---- Gaussian ------------------------------------------------------------- */
/* Exact 2D Gaussian with sigma^2 = r (r + 2) / 6, renormalized at the image
 * border, on an opaque image (double precision reference). */
static void t_gauss_ref(const fx_img *src, double radius, fx_img *out)
{
    double var = radius * (radius + 2.0) / 6.0;
    int32_t k = (int32_t)ceil(4.0 * sqrt(var)), x, y, i, c;
    int32_t w = src->r.w, h = src->r.h;
    double *wk = (double *)malloc(sizeof(double) * (size_t)(2 * k + 1));
    double *tmp = (double *)malloc(sizeof(double) * (size_t)w * (size_t)h * 3u);
    for (i = -k; i <= k; i++) wk[i + k] = exp(-(double)(i * i) / (2.0 * var));
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            for (c = 0; c < 3; c++) {
                double s = 0, ws = 0;
                for (i = -k; i <= k; i++) {
                    fx_px p;
                    if (x + i < 0 || x + i >= w) continue;
                    p = fx_row(src, y)[x + i];
                    s += wk[i + k] * (c == 0 ? p.b : c == 1 ? p.g : p.r);
                    ws += wk[i + k];
                }
                tmp[((size_t)y * (size_t)w + (size_t)x) * 3u + (size_t)c] = s / ws;
            }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double v[3];
            for (c = 0; c < 3; c++) {
                double s = 0, ws = 0;
                for (i = -k; i <= k; i++) {
                    if (y + i < 0 || y + i >= h) continue;
                    s += wk[i + k] *
                         tmp[((size_t)(y + i) * (size_t)w + (size_t)x) * 3u + (size_t)c];
                    ws += wk[i + k];
                }
                v[c] = s / ws;
            }
            fx_row(out, y)[x] = fx_px_make(fx_u8(v[2]), fx_u8(v[1]), fx_u8(v[0]), 255);
        }
    free(wk);
    free(tmp);
}

static void t_gaussian_reference(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.gaussian");
    double radii[] = { 1.0, 2.0, 4.5, 12.0, 30.0 };
    int maxd[4][5];
    size_t i;
    int q;
    fx_img src = t_img_new(0, 0, 70, 52);
    t_img_scene(&src);
    for (i = 0; i < 5; i++) {
        fx_img ref = t_img_new(0, 0, 70, 52);
        t_gauss_ref(&src, radii[i], &ref);
        for (q = 1; q <= 4; q++) {
            void *p = t_params_new(fx);
            fx_img d;
            t_set(fx, p, "radius", radii[i]);
            t_set(fx, p, "quality", q);
            d = t_render(fx->id, p, &src, t_all(&src), NULL);
            maxd[q - 1][i] = t_img_maxdiff(&d, &ref, t_all(&src));
            free(p);
            t_img_free(&d);
        }
        t_img_free(&ref);
    }
    for (i = 0; i < 5; i++) {
        INFO("gaussian r=%.1f max diff vs exact: q1 %d q2 %d q3 %d q4 %d", radii[i], maxd[0][i],
             maxd[1][i], maxd[2][i], maxd[3][i]);
        CHECK(maxd[3][i] <= (radii[i] <= 12.0 ? 2 : 4));   /* exact kernel or 5 boxes */
        CHECK(maxd[2][i] <= 6);
        CHECK(maxd[1][i] <= 8);
        CHECK(maxd[0][i] <= 14);                             /* tent: coarse but close */
    }
    t_img_free(&src);
}

static void t_gaussian_props(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.gaussian");
    fx_img src = t_img_new(0, 0, 64, 64), d;
    void *p = t_params_new(fx);
    int32_t x, y;
    double m0, m1, v0, v1, sum = 0, var = 0;
    /* mean preserved, variance reduced on noise */
    t_img_random(&src, 0);
    t_set(fx, p, "radius", 6.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (x = 0; x < 3; x++) {
        m0 = t_mean(&src, x);
        m1 = t_mean(&d, x);
        v0 = t_var(&src, x);
        v1 = t_var(&d, x);
        CHECK(fabs(m0 - m1) < 1.0);
        CHECK(v1 < v0 * 0.1);
    }
    t_img_free(&d);
    t_img_free(&src);
    /* a dot spreads into a symmetric, monotone bell */
    src = t_dot(61, 61, 30, 30);
    t_set(fx, p, "radius", 8.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 61; y++)
        for (x = 0; x < 61; x++) {
            fx_px a = fx_row(&d, y)[x];
            CHECK(t_px_eq(a, fx_row(&d, x)[y]));             /* transpose symmetric */
            CHECK(t_px_eq(a, fx_row(&d, y)[60 - x]));        /* mirror symmetric */
            sum += a.g;
        }
    CHECK(fx_row(&d, 30)[30].g >= fx_row(&d, 30)[31].g);
    CHECK(fx_row(&d, 30)[31].g >= fx_row(&d, 30)[33].g);
    CHECK(fabs(sum - 255.0) < 40.0);                       /* energy, up to rounding */
    t_img_free(&d);
    t_img_free(&src);
    /* a line's profile has the variance of the 3.36 tent: r (r + 2) / 6 */
    src = t_img_new(0, 0, 61, 9);
    t_img_fill(&src, fx_px_make(0, 0, 0, 255));
    for (y = 0; y < 9; y++) fx_row(&src, y)[30] = fx_px_make(255, 255, 255, 255);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    sum = 0;
    for (x = 0; x < 61; x++) {
        double g = fx_row(&d, 4)[x].g;
        sum += g;
        var += g * (double)((x - 30) * (x - 30));
    }
    var /= sum;
    INFO("line profile variance %.2f, expected %.2f", var, 8.0 * 10.0 / 6.0);
    CHECK(fabs(var - 80.0 / 6.0) < 1.3);
    t_img_free(&d);
    t_img_free(&src);
    /* gamma boost: high key brightens a checkerboard, low key darkens it */
    src = t_img_new(0, 0, 32, 32);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            fx_row(&src, y)[x] = ((x + y) & 1) ? fx_px_make(255, 255, 255, 255)
                                               : fx_px_make(0, 0, 0, 255);
    t_set(fx, p, "radius", 3.0);
    t_set(fx, p, "gamma_boost", 0.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    m0 = t_mean(&d, 1);
    t_img_free(&d);
    t_set(fx, p, "gamma_boost", 1.5);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    m1 = t_mean(&d, 1);
    t_img_free(&d);
    t_set(fx, p, "gamma_boost", -1.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    v0 = t_mean(&d, 1);
    t_img_free(&d);
    INFO("checkerboard mean: boost 0 %.1f, +1.5 %.1f, -1 %.1f", m0, m1, v0);
    CHECK(m0 > 120 && m0 < 135);
    CHECK(m1 > m0 + 40);
    CHECK(v0 < m0 - 30);
    t_img_free(&src);
    free(p);
}

/* Premultiplied blurring: an opaque red shape on a transparent background
 * keeps its color in the soft edge (no dark halo). */
static void t_alpha_halo(void)
{
    static const char *const ids[] = {
        "org.paintc.blur.gaussian", "org.paintc.blur.square", "org.paintc.blur.bokeh",
        "org.paintc.blur.motion", "org.paintc.blur.radial", "org.paintc.blur.zoom",
        "org.paintc.blur.fragment", "org.paintc.blur.median", "org.paintc.blur.sketch",
        "org.paintc.blur.surface" };
    size_t i;
    for (i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const fx_effect *fx = t_find(ids[i]);
        fx_img src = t_img_new(0, 0, 40, 40), d;
        void *p = t_params_new(fx);
        int32_t x, y, ok = 1, soft = 0;
        t_img_fill(&src, fx_px_make(0, 0, 0, 0));
        for (y = 12; y < 28; y++)
            for (x = 10; x < 30; x++) fx_row(&src, y)[x] = fx_px_make(250, 20, 10, 255);
        if (t_prop(fx, "radius") && !strstr(ids[i], "median") && !strstr(ids[i], "surface"))
            t_set(fx, p, "radius", 5.0);
        d = t_render(ids[i], p, &src, t_all(&src), NULL);
        for (y = 0; y < 40; y++)
            for (x = 0; x < 40; x++) {
                fx_px q = fx_row(&d, y)[x];
                if (q.a == 0) continue;
                if (q.a < 255) soft++;
                if (q.a >= 8) ok &= abs(q.r - 250) <= 2 && abs(q.g - 20) <= 2 && abs(q.b - 10) <= 2;
            }
        CHECK(ok);
        if (!ok) INFO("%s darkens or tints transparent edges", ids[i]);
        if (!strstr(ids[i], "median") && !strstr(ids[i], "surface")) CHECK(soft > 0);
        free(p);
        t_img_free(&src);
        t_img_free(&d);
    }
}

/* ---- Square and Bokeh --------------------------------------------------- */
static void t_square_shape(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.square");
    fx_img src = t_dot(31, 31, 15, 15), d;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    t_set(fx, p, "radius", 3.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 31; y++)
        for (x = 0; x < 31; x++) {
            int in = abs(x - 15) <= 3 && abs(y - 15) <= 3;
            uint8_t v = fx_row(&d, y)[x].g;
            ok &= in ? (v == 5 || v == 6) : v == 0;     /* 255 / 49 = 5.2 */
        }
    CHECK(ok);
    t_img_free(&d);
    /* fractional radius: the outer ring gets partial weight */
    t_set(fx, p, "radius", 3.5);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 15)[19].g > 0 && fx_row(&d, 15)[19].g < fx_row(&d, 15)[18].g);
    CHECK(fx_row(&d, 15)[20].g == 0);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

/* Brute-force anti-aliased disk with border renormalization. */
static void t_bokeh_ref(const fx_img *src, double R, fx_img *out)
{
    int32_t re = (int32_t)ceil(R + 1), x, y, u, v, w = src->r.w, h = src->r.h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double s[3] = { 0, 0, 0 }, ws = 0;
            for (v = -re; v <= re; v++)
                for (u = -re; u <= re; u++) {
                    double cov = R + 0.5 - sqrt((double)(u * u + v * v));
                    fx_px p;
                    if (cov <= 0 || x + u < 0 || y + v < 0 || x + u >= w || y + v >= h) continue;
                    if (cov > 1) cov = 1;
                    p = fx_row(src, y + v)[x + u];
                    s[0] += cov * p.b; s[1] += cov * p.g; s[2] += cov * p.r;
                    ws += cov;
                }
            fx_row(out, y)[x] = fx_px_make(fx_u8(s[2] / ws), fx_u8(s[1] / ws), fx_u8(s[0] / ws),
                                           255);
        }
}

static void t_bokeh(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.bokeh");
    fx_img src = t_img_new(0, 0, 48, 40), ref = t_img_new(0, 0, 48, 40), d;
    void *p = t_params_new(fx);
    double radii[] = { 1.0, 3.7, 9.0 };
    size_t i;
    int32_t x, y, ok = 1;
    t_img_scene(&src);
    for (i = 0; i < 3; i++) {
        int m;
        t_bokeh_ref(&src, radii[i], &ref);
        t_set(fx, p, "radius", radii[i]);
        t_set(fx, p, "quality", 10);
        d = t_render(fx->id, p, &src, t_all(&src), NULL);
        m = t_img_maxdiff(&d, &ref, t_all(&src));
        INFO("bokeh r=%.1f q10 max diff vs brute force %d", radii[i], m);
        CHECK(m <= 1);
        t_img_free(&d);
        t_set(fx, p, "quality", 1);
        d = t_render(fx->id, p, &src, t_all(&src), NULL);
        m = t_img_maxdiff(&d, &ref, t_all(&src));
        INFO("bokeh r=%.1f q1 max diff vs brute force %d", radii[i], m);
        CHECK(m <= 24);
        t_img_free(&d);
    }
    t_img_free(&src);
    t_img_free(&ref);
    /* a dot becomes a flat disk */
    src = t_dot(41, 41, 20, 20);
    t_set(fx, p, "radius", 8.0);
    t_set(fx, p, "quality", 10);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 41; y++)
        for (x = 0; x < 41; x++) {
            double r = sqrt((double)((x - 20) * (x - 20) + (y - 20) * (y - 20)));
            uint8_t v = fx_row(&d, y)[x].g;
            if (r <= 7.0) ok &= v == fx_row(&d, 20)[20].g;
            if (r >= 9.0) ok &= v == 0;
        }
    CHECK(ok);
    CHECK(fx_row(&d, 20)[20].g > 0);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

/* ---- Fragment, Motion, Radial, Zoom ------------------------------------------ */
static int32_t t_count_lit(const fx_img *d, int32_t *minx, int32_t *maxx, int32_t *miny,
                           int32_t *maxy)
{
    int32_t x, y, n = 0;
    *minx = *miny = 1 << 30;
    *maxx = *maxy = -1;
    for (y = d->r.y; y < d->r.y + d->r.h; y++)
        for (x = d->r.x; x < d->r.x + d->r.w; x++)
            if (fx_row(d, y)[x].g > 0) {
                n++;
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
                if (y < *miny) *miny = y;
                if (y > *maxy) *maxy = y;
            }
    return n;
}

static void t_fragment(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.fragment");
    fx_img src = t_dot(41, 41, 20, 20), d;
    void *p = t_params_new(fx);
    t_set(fx, p, "fragment_count", 4);
    t_set(fx, p, "distance", 6);
    t_set(fx, p, "rotation", 0.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    /* four copies of the dot at distance 6 on the axes, each a quarter */
    CHECK(fx_row(&d, 20)[26].g == 64 && fx_row(&d, 20)[14].g == 64);
    CHECK(fx_row(&d, 26)[20].g == 64 && fx_row(&d, 14)[20].g == 64);
    CHECK(fx_row(&d, 20)[20].g == 0);
    {
        int32_t a, b, c, e;
        CHECK(t_count_lit(&d, &a, &b, &c, &e) == 4);
    }
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_motion(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.motion");
    void *p = t_params_new(fx);
    fx_img src = t_dot(61, 41, 30, 20), d;
    int32_t x0, x1, y0, y1, n;
    /* angle 0, not centered: the trail extends to the right of the dot */
    t_set(fx, p, "angle", 0.0);
    t_set(fx, p, "distance", 10);
    t_set(fx, p, "centered", 0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    n = t_count_lit(&d, &x0, &x1, &y0, &y1);
    CHECK(n >= 9 && y0 == 20 && y1 == 20 && x0 == 30 && x1 >= 39 && x1 <= 40);
    CHECK(fx_row(&d, 20)[30].g > fx_row(&d, 20)[38].g);      /* half Gaussian decay */
    t_img_free(&d);
    /* centered: symmetric around the dot */
    t_set(fx, p, "centered", 1);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    n = t_count_lit(&d, &x0, &x1, &y0, &y1);
    CHECK(y0 == 20 && y1 == 20 && x0 == 25 && x1 == 35);
    for (n = 1; n <= 5; n++) CHECK(fx_row(&d, 20)[30 - n].g == fx_row(&d, 20)[30 + n].g);
    t_img_free(&d);
    /* angle 90: vertical, upward when not centered */
    t_set(fx, p, "angle", 90.0);
    t_set(fx, p, "centered", 0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    n = t_count_lit(&d, &x0, &x1, &y0, &y1);
    CHECK(x0 == 30 && x1 == 30 && y1 == 20 && y0 <= 11 && y0 >= 10);
    t_img_free(&d);
    /* 45 degrees: lit pixels lie on the diagonal up and to the right */
    t_set(fx, p, "angle", 45.0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    n = t_count_lit(&d, &x0, &x1, &y0, &y1);
    CHECK(x0 == 30 && y1 == 20 && x1 >= 36 && y0 <= 14);
    CHECK(fx_row(&d, 15)[35].g > 0 && fx_row(&d, 15)[25].g == 0 && fx_row(&d, 25)[35].g == 0);
    t_img_free(&d);
    t_img_free(&src);
    /* edge behaviors near the right border */
    src = t_dot(30, 9, 28, 4);
    t_set(fx, p, "angle", 180.0);           /* trail to the left: samples to the right */
    t_set(fx, p, "centered", 1);
    t_set(fx, p, "edge_behavior", 1);       /* wrap */
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 4)[1].g > 0);          /* wrapped around to the left edge */
    t_img_free(&d);
    t_set(fx, p, "edge_behavior", 0);       /* clamp */
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 4)[1].g == 0);
    t_img_free(&d);
    t_img_free(&src);
    /* Transparent edge behavior fades opaque images at the border only */
    src = t_img_new(0, 0, 30, 9);
    t_img_fill(&src, fx_px_make(90, 90, 90, 255));
    t_set(fx, p, "angle", 0.0);
    t_set(fx, p, "edge_behavior", 3);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 4)[0].a < 255 && fx_row(&d, 4)[29].a < 255 && fx_row(&d, 4)[15].a == 255);
    CHECK(fx_row(&d, 4)[0].g == 90);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_radial_zoom(void)
{
    const fx_effect *rad = t_find("org.paintc.blur.radial"), *zoom = t_find("org.paintc.blur.zoom");
    void *pr = t_params_new(rad), *pz = t_params_new(zoom);
    fx_img src = t_dot(61, 61, 45, 30), d, rnd;
    int32_t x0, x1, y0, y1, x, y, ok = 1;
    /* radial: the dot at radius 15 spreads along its circle */
    t_set(rad, pr, "angle", 20.0);
    t_set(rad, pr, "quality", 4);
    d = t_render(rad->id, pr, &src, t_all(&src), NULL);
    (void)t_count_lit(&d, &x0, &x1, &y0, &y1);
    for (y = 0; y < 61; y++)
        for (x = 0; x < 61; x++)
            if (fx_row(&d, y)[x].g > 0) {
                double r = sqrt((double)((x - 30) * (x - 30) + (y - 30) * (y - 30)));
                ok &= fabs(r - 15.0) <= 1.5;
            }
    CHECK(ok);
    CHECK(y0 <= 26 && y1 >= 34 && x0 >= 43);
    t_img_free(&d);
    /* zoom: the dot at radius 15 streaks outward along its ray */
    t_set(zoom, pz, "distance", 4.0);
    t_set(zoom, pz, "focus", 0.0);
    t_set(zoom, pz, "quality", 4);
    d = t_render(zoom->id, pz, &src, t_all(&src), NULL);
    (void)t_count_lit(&d, &x0, &x1, &y0, &y1);
    /* bilinear taps let the neighbor rows catch the ray, but the streak
     * only extends outward, to about 15 / (1 - 0.4) = 25 px from the center */
    CHECK(y0 >= 29 && y1 <= 31 && x0 == 45 && x1 >= 54 && x1 <= 56);
    for (x = 46; x <= 54; x++)
        CHECK(fx_row(&d, 30)[x].g >= fx_row(&d, 29)[x].g && fx_row(&d, 30)[x].g > 0);
    t_img_free(&d);
    /* focus concentrates the weight near the pixel: shorter visible streak */
    t_set(zoom, pz, "focus", 6.0);
    d = t_render(zoom->id, pz, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 30)[53].g < 2 && fx_row(&d, 30)[46].g > 0);
    t_img_free(&d);
    t_img_free(&src);
    /* the center pixel itself never moves */
    rnd = t_img_new(0, 0, 61, 61);
    t_img_random(&rnd, 0);
    t_set(rad, pr, "angle", 90.0);
    d = t_render(rad->id, pr, &rnd, t_all(&rnd), NULL);
    CHECK(t_px_eq(fx_row(&d, 30)[30], fx_row(&rnd, 30)[30]));
    t_img_free(&d);
    d = t_render(zoom->id, pz, &rnd, t_all(&rnd), NULL);
    CHECK(t_px_eq(fx_row(&d, 30)[30], fx_row(&rnd, 30)[30]));
    t_img_free(&d);
    /* Center moves the fixed point: offset 20/61 puts it on pixel (40, 30) */
    t_set2(zoom, pz, "center", 20.0 / 61.0, 0.0);
    d = t_render(zoom->id, pz, &rnd, t_all(&rnd), NULL);
    CHECK(t_px_eq(fx_row(&d, 30)[40], fx_row(&rnd, 30)[40]));
    CHECK(!t_px_eq(fx_row(&d, 30)[30], fx_row(&rnd, 30)[30]));
    t_img_free(&d);
    t_set2(rad, pr, "center", 20.0 / 61.0, 0.0);
    d = t_render(rad->id, pr, &rnd, t_all(&rnd), NULL);
    CHECK(t_px_eq(fx_row(&d, 30)[40], fx_row(&rnd, 30)[40]));
    CHECK(!t_px_eq(fx_row(&d, 30)[30], fx_row(&rnd, 30)[30]));
    t_img_free(&d);
    t_img_free(&rnd);
    free(pr);
    free(pz);
}

/* ---- Median, Sketch, Surface --------------------------------------------- */
static int32_t t_wpct(const int32_t *h, int64_t total, int32_t pct)
{
    int64_t t = (total * pct + 99) / 100, cum = 0;
    int32_t i;
    if (t < 1) t = 1;
    for (i = 0; i < 256; i++) {
        cum += h[i];
        if (cum >= t) return i;
    }
    return 255;
}

static void t_median_reference(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.median");
    fx_img src = t_img_new(0, 0, 33, 27), d;
    void *p = t_params_new(fx);
    int32_t x, y, u, v, ok = 1, r = 3, pct = 35;
    int32_t cutoff = ((2 * r + 1) * (2 * r + 1) + 2) / 4;
    t_img_random(&src, 1);
    t_set(fx, p, "radius", r);
    t_set(fx, p, "percentile", pct);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 27; y++)
        for (x = 0; x < 33; x++) {
            static int32_t hb[256], hg[256], hr[256], ha[256];
            int64_t ws = 0, n = 0;
            fx_px e;
            memset(hb, 0, sizeof hb); memset(hg, 0, sizeof hg);
            memset(hr, 0, sizeof hr); memset(ha, 0, sizeof ha);
            for (v = -r; v <= r; v++)
                for (u = -r; u <= r; u++) {
                    fx_px q;
                    if (u * u + v * v > cutoff || x + u < 0 || y + v < 0 || x + u >= 33 ||
                        y + v >= 27) continue;
                    q = fx_row(&src, y + v)[x + u];
                    hb[q.b] += q.a; hg[q.g] += q.a; hr[q.r] += q.a; ha[q.a]++;
                    ws += q.a;
                    n++;
                }
            e.a = (uint8_t)t_wpct(ha, n, pct);
            if (e.a == 0 || ws == 0) e = fx_px_make(0, 0, 0, 0);
            else {
                e.b = (uint8_t)t_wpct(hb, ws, pct);
                e.g = (uint8_t)t_wpct(hg, ws, pct);
                e.r = (uint8_t)t_wpct(hr, ws, pct);
            }
            ok &= t_px_eq(e, fx_row(&d, y)[x]);
        }
    CHECK(ok);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_median_props(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.median");
    fx_img src = t_img_new(0, 0, 40, 30), d, d2;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    /* salt and pepper noise disappears */
    t_img_fill(&src, fx_px_make(120, 120, 120, 255));
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) {
            if (x % 5 == 0 && y % 5 == 0) fx_row(&src, y)[x] = fx_px_make(255, 255, 255, 255);
            if (x % 5 == 2 && y % 5 == 3) fx_row(&src, y)[x] = fx_px_make(0, 0, 0, 255);
        }
    t_set(fx, p, "radius", 2);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(t_is_const(&d, fx_px_make(120, 120, 120, 255)));
    /* quality 9 equals quality 8 (exact for 8-bit) */
    t_set(fx, p, "quality", 9);
    d2 = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(t_img_eq(&d, &d2, t_all(&src)));
    t_img_free(&d);
    t_img_free(&d2);
    /* quality 1 posterizes to two levels */
    t_img_random(&src, 0);
    t_set(fx, p, "quality", 1);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) {
            fx_px q = fx_row(&d, y)[x];
            ok &= (q.r == 0 || q.r == 255) && (q.g == 0 || q.g == 255) && q.a == 255;
        }
    CHECK(ok);
    t_img_free(&d);
    t_img_free(&src);
    /* percentile 0 erodes a dot away, percentile 100 dilates it into a disk */
    src = t_dot(21, 21, 10, 10);
    t_set(fx, p, "quality", 8);
    t_set(fx, p, "radius", 3);
    t_set(fx, p, "percentile", 0);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(t_is_const(&d, fx_px_make(0, 0, 0, 255)));
    t_img_free(&d);
    t_set(fx, p, "percentile", 100);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    CHECK(fx_row(&d, 10)[13].g == 255 && fx_row(&d, 13)[10].g == 255);
    CHECK(fx_row(&d, 10)[14].g == 0 && fx_row(&d, 13)[13].g == 0);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_sketch(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.sketch");
    fx_img src = t_img_new(0, 0, 40, 30), lo, hi, mid;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    t_img_random(&src, 0);
    t_set(fx, p, "radius", 5.0);
    t_set(fx, p, "percentile", 0);
    lo = t_render(fx->id, p, &src, t_all(&src), NULL);
    t_set(fx, p, "percentile", 100);
    hi = t_render(fx->id, p, &src, t_all(&src), NULL);
    t_set(fx, p, "percentile", 50);
    mid = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) {
            fx_px a = fx_row(&lo, y)[x], b = fx_row(&mid, y)[x], c = fx_row(&hi, y)[x];
            ok &= a.r <= b.r && b.r <= c.r && a.g <= b.g && b.g <= c.g && a.b <= b.b && b.b <= c.b;
        }
    CHECK(ok);
    CHECK(t_mean(&lo, 1) < t_mean(&mid, 1) - 40 && t_mean(&hi, 1) > t_mean(&mid, 1) + 40);
    t_img_free(&lo); t_img_free(&hi); t_img_free(&mid);
    /* isolated specks are removed at the median */
    t_img_fill(&src, fx_px_make(100, 100, 100, 255));
    for (y = 3; y < 30; y += 9)
        for (x = 3; x < 40; x += 9) fx_row(&src, y)[x] = fx_px_make(255, 255, 255, 255);
    t_set(fx, p, "radius", 4.0);
    mid = t_render(fx->id, p, &src, t_all(&src), NULL);
    ok = 1;
    for (y = 3; y < 30; y += 9)
        for (x = 3; x < 40; x += 9) ok &= fx_row(&mid, y)[x].g < 128;
    CHECK(ok);
    /* more smoothness, smoother result: fewer distinct output levels on noise */
    t_img_free(&mid);
    t_img_free(&src);
    free(p);
}

static void t_surface(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.surface");
    fx_img src = t_img_new(0, 0, 40, 24), d;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    double v0, v1;
    for (y = 0; y < 24; y++)
        for (x = 0; x < 40; x++) {
            uint8_t base = x < 20 ? 40 : 200, n = (uint8_t)(base - 4 + (int)rndu(9));
            fx_row(&src, y)[x] = fx_px_make(n, n, n, 255);
        }
    t_set(fx, p, "radius", 4);
    d = t_render(fx->id, p, &src, t_all(&src), NULL);
    for (y = 0; y < 24; y++)
        for (x = 0; x < 40; x++) {
            uint8_t g = fx_row(&d, y)[x].g;
            ok &= x < 20 ? (g >= 36 && g <= 44) : (g >= 196 && g <= 204);
        }
    CHECK(ok);                                            /* edge kept sharp */
    v0 = t_var(&src, 1);
    v1 = t_var(&d, 1);
    CHECK(v1 < v0);                                       /* noise reduced */
    {
        fx_img l = t_img_new(0, 0, 20, 24), l2 = t_img_new(0, 0, 20, 24);
        for (y = 0; y < 24; y++) {
            memcpy(fx_row(&l, y), fx_row(&src, y), 20 * 4);
            memcpy(fx_row(&l2, y), fx_row(&d, y), 20 * 4);
        }
        INFO("surface blur noise variance %.2f -> %.2f", t_var(&l, 1), t_var(&l2, 1));
        CHECK(t_var(&l2, 1) < t_var(&l, 1) * 0.5);
        t_img_free(&l);
        t_img_free(&l2);
    }
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

/* The vertical-pass cache built in prepare() and the per-ROI path must give
 * identical bytes, for boxes and exact kernels, with gamma and alpha. */
static void t_sep_cache(void)
{
    fx_img src = t_img_new(0, 0, 90, 70), a = t_img_new(0, 0, 90, 70), b = t_img_new(0, 0, 90, 70);
    fx_rect sel = t_rect(5, 4, 80, 60), rois[T_MAX_ROI];
    int k, n, i;
    t_img_random(&src, 1);
    for (k = 0; k < 4; k++) {
        fx1_sep s;
        fx1_vcache c;
        if (k == 0) fx1_sep_gaussian(&s, 40.0, 3, 0.7);
        else if (k == 1) fx1_sep_gaussian(&s, 9.0, 4, -0.5);      /* exact kernel */
        else if (k == 2) fx1_sep_box(&s, 31.5, 0.0);
        else fx1_sep_gaussian(&s, 25.0, 1, 2.0);
        CHECK(fx1_sep_cache_build(&s, &src, sel, &c, &g_t_host, NULL) == FX_OK);
        CHECK(c.v != NULL || s.ext < 24);
        t_img_sentinel(&a);
        t_img_sentinel(&b);
        n = t_split_random(sel, rois);
        for (i = 0; i < n; i++) {
            CHECK(fx1_sep_render_c(&s, &c, &src, &a, rois[i], &g_t_host, NULL) == FX_OK);
            CHECK(fx1_sep_render(&s, &src, &b, rois[i], &g_t_host, NULL) == FX_OK);
        }
        CHECK(t_img_eq(&a, &b, sel));
        fx1_sep_cache_free(&c, &g_t_host);
    }
    t_img_free(&src);
    t_img_free(&a);
    t_img_free(&b);
}

/* ---- performance ------------------------------------------------------------ */
static void t_gaussian_speed(void)
{
    const fx_effect *fx = t_find("org.paintc.blur.gaussian");
    int32_t n = g_quick ? 768 : 4096;
    fx_img src = t_img_new(0, 0, n, n), dst = t_img_new(0, 0, n, n);
    void *p = t_params_new(fx);
    fx_rect rois[64];
    int k, nr;
    double t0, t1;
    t_img_random(&src, 0);
    t_set(fx, p, "radius", 100.0);
    nr = t_split_grid(src.r, n, (n + 63) / 64, rois);
    t0 = pc_test_now();
    {
        fx_env e = t_env(&src, src.r);
        CHECK(t_run(fx, p, &src, &dst, &e, rois, nr) == FX_OK);
    }
    t1 = pc_test_now();
    INFO("gaussian r=100 on %dx%d, one thread, %d bands: %.2f s (%.1f Mpx/s)", n, n, nr,
         t1 - t0, (double)n * n / (t1 - t0) / 1e6);
    /* a smooth region far from the noise statistics: the output is near the mean */
    k = fx_row(&dst, n / 2)[n / 2].g;
    CHECK(k > 110 && k < 145);
    free(p);
    t_img_free(&src);
    t_img_free(&dst);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_constant);
    RUN(t_identity);
    RUN(t_gaussian_reference);
    RUN(t_gaussian_props);
    RUN(t_alpha_halo);
    RUN(t_square_shape);
    RUN(t_bokeh);
    RUN(t_fragment);
    RUN(t_motion);
    RUN(t_radial_zoom);
    RUN(t_median_reference);
    RUN(t_median_props);
    RUN(t_sketch);
    RUN(t_surface);
    RUN(t_sep_cache);
    RUN(t_gaussian_speed);
    return pc_test_finish();
}
