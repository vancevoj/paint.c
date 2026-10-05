/* test_codec_progress.c - W4-SAVECFG (ADR-023): encode progress and
 * cancellation through pc_codec_save_ex.
 *   t_contract      every codec that saves reports 0 first, 1 last, strictly
 *                   increasing thousandths with intermediate steps, and
 *                   writes the same bytes as save()
 *   t_cancel        returning false at the first, second, a middle and the
 *                   last intermediate report stops the encode with
 *                   PC_ERR_CANCELLED, no later report, and out keeps its
 *                   previous bytes (sanitizer builds check the cleanup)
 *   t_fallback      a codec without save_ex reports only 0 and 1; a cancel
 *                   at 0 never starts its save()
 *   t_args          argument and capability errors */
#include "pc_test.h"
#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

#include <math.h>

/* ---- test images -------------------------------------------------------------- */
static pc_px32 px_at(uint32_t x, uint32_t y, uint32_t salt)
{
    pc_px32 p;
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (salt * 83492791u);
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    /* 32 levels per channel: many colors for the quantizers, a histogram
     * small enough for quick sanitizer runs */
    p.b = (uint8_t)(((x * 255u) / 320u + (h & 15u)) & 0xF8u);
    p.g = (uint8_t)(((y * 255u) / 520u + ((h >> 4) & 15u)) & 0xF8u);
    p.r = (uint8_t)((h >> 8) & 0xF8u);
    p.a = (uint8_t)((x + y) % 7u == 0u ? 0u : (y < 260u ? 255u : 128u + (h >> 25)));
    return p;
}

static pc_doc *make_doc(uint32_t w, uint32_t h, uint32_t layers)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    if (!d || !px) { free(px); pc_doc_destroy(d); return NULL; }
    for (uint32_t k = 0; k < layers; k++) {
        pc_layer *l = pc_layer_create(d, k ? "Top" : "Background");
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) px[(size_t)y * w + x] = px_at(x, y, k);
        if (!l ||
            pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, w) !=
                PC_OK ||
            pc_doc_insert_layer(d, l, k) != PC_OK) {
            pc_layer_destroy(l);
            free(px);
            pc_doc_destroy(d);
            return NULL;
        }
    }
    free(px);
    return d;
}

/* ---- options by key ------------------------------------------------------------- */
static void set_opt(const pc_codec *c, void *params, const char *key, int32_t v)
{
    bool found = false;
    for (uint32_t i = 0; i < c->n_props && !found; i++)
        if (strcmp(c->props[i].key, key) == 0) {
            memcpy((uint8_t *)params + c->props[i].offset, &v, sizeof v);
            found = true;
        }
    CHECK(found);
}

typedef struct opt_case {
    const char *id;
    const char *key[3];
    int32_t     val[3];
    uint32_t    layers;
    uint32_t    min_steps;      /* intermediate reports at least */
} opt_case;

static const opt_case k_cases[] = {
    { "png",  { NULL }, { 0 }, 1, 6 },
    { "png",  { "bit_depth" }, { 1 }, 1, 6 },
    { "png",  { "bit_depth" }, { 2 }, 1, 6 },
    { "png",  { "bit_depth", "dither" }, { 3, 8 }, 1, 6 },
    { "png",  { "bit_depth", "interlace" }, { 1, 1 }, 1, 6 },
    { "jpeg", { NULL }, { 0 }, 1, 6 },
    { "webp", { NULL }, { 0 }, 1, 6 },
    { "webp", { "lossless", "effort" }, { 1, 2 }, 1, 6 },
    { "tiff", { NULL }, { 0 }, 1, 6 },
    { "tiff", { "bit_depth" }, { 3 }, 1, 6 },
    { "bmp",  { NULL }, { 0 }, 1, 6 },
    { "bmp",  { "bit_depth" }, { 3 }, 1, 6 },
    { "tga",  { NULL }, { 0 }, 1, 6 },
    { "gif",  { NULL }, { 0 }, 1, 6 },
    { "dds",  { NULL }, { 0 }, 1, 6 },
    { "dds",  { "format", "mipmaps" }, { 10, 1 }, 1, 6 },
    { "dds",  { "format", "dither" }, { 20, 1 }, 1, 6 },
    { "ora",  { NULL }, { 0 }, 2, 6 },
    { "pdn",  { NULL }, { 0 }, 2, 2 },      /* one batch of chunks per layer here */
    { "avif", { NULL }, { 0 }, 1, 4 },
    { "jxl",  { NULL }, { 0 }, 1, 4 },
};
#define N_CASES (sizeof k_cases / sizeof k_cases[0])

/* ---- recorder ---------------------------------------------------------------------- */
typedef struct rec {
    double   v[4096];
    uint32_t n;
    uint32_t cancel_at;         /* 1-based call that returns false, 0 = never */
    uint32_t after_cancel;      /* calls after the one that returned false */
} rec;

static bool rec_report(void *ud, double done)
{
    rec *r = (rec *)ud;
    if (r->cancel_at && r->n >= r->cancel_at) {
        r->after_cancel++;
        return false;
    }
    if (r->n < sizeof r->v / sizeof r->v[0]) r->v[r->n] = done;
    r->n++;
    return !(r->cancel_at && r->n == r->cancel_at);
}

static uint32_t intermediates(const rec *r)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < r->n; i++) k += r->v[i] > 0.0 && r->v[i] < 1.0;
    return k;
}

static void prepare(const opt_case *oc, const pc_codec **c, void **params, pc_doc **d)
{
    *c = pc_codec_by_id(oc->id);
    *params = NULL;
    *d = NULL;
    if (!*c || !((*c)->flags & PC_CODEC_SAVE)) return;      /* library absent */
    if ((*c)->params_size) {
        *params = calloc(1u, (*c)->params_size);
        if (!*params) return;
        pc_codec_default_params(*c, *params);
        for (int k = 0; k < 3 && oc->key[k]; k++) set_opt(*c, *params, oc->key[k], oc->val[k]);
    }
    *d = make_doc(g_quick ? 128u : 320u, g_quick ? 448u : 520u, oc->layers);
}

static void t_contract(void)
{
    for (size_t i = 0; i < N_CASES; i++) {
        const opt_case *oc = &k_cases[i];
        const pc_codec *c;
        void *params;
        pc_doc *d;
        pc_buf ref, got;
        rec *r = (rec *)calloc(1u, sizeof *r);
        pc_codec_progress pg;
        pc_status st;
        bool mono = true;
        prepare(oc, &c, &params, &d);
        if (!c || !(c->flags & PC_CODEC_SAVE)) {
            INFO("%s: not built, skipped", oc->id);
            free(r);
            continue;
        }
        CHECK(d != NULL && r != NULL);
        if (!d || !r) { free(params); free(r); pc_doc_destroy(d); continue; }
        CHECK(c->save_ex != NULL);                         /* every saving codec reports */
        memset(&ref, 0, sizeof ref);
        memset(&got, 0, sizeof got);
        CHECK(c->save(d, NULL, params, NULL, &ref) == PC_OK);
        pg.report = rec_report;
        pg.ud = r;
        st = pc_codec_save_ex(c, d, NULL, params, NULL, &pg, &got);
        CHECK(st == PC_OK);
        CHECK(r->n >= 2u && r->v[0] == 0.0 && r->v[r->n - 1u] == 1.0);
        for (uint32_t k = 1; k < r->n && k < 4096u; k++) {
            double step = r->v[k] - r->v[k - 1u];
            if (!(step >= 0.000999)) mono = false;         /* strictly up, in thousandths */
            if (fabs(r->v[k] * 1000.0 - floor(r->v[k] * 1000.0 + 0.5)) > 1e-9) mono = false;
        }
        CHECK(mono);
        CHECK(intermediates(r) >= oc->min_steps);
        if (intermediates(r) < oc->min_steps)
            INFO("%s case %u: %u intermediate reports", oc->id, (unsigned)i, intermediates(r));
        /* the bytes never depend on the observer */
        CHECK(got.n == ref.n && (ref.n == 0u || memcmp(got.p, ref.p, ref.n) == 0));
        pc_buf_free(&ref);
        pc_buf_free(&got);
        free(params);
        free(r);
        pc_doc_destroy(d);
    }
}

static void t_cancel(void)
{
    static const uint8_t k_prefix[5] = { 'K', 'E', 'E', 'P', '!' };
    for (size_t i = 0; i < N_CASES; i++) {
        const opt_case *oc = &k_cases[i];
        const pc_codec *c;
        void *params;
        pc_doc *d;
        rec *r = (rec *)calloc(1u, sizeof *r);
        uint32_t calls, at[4];
        pc_codec_progress pg;
        prepare(oc, &c, &params, &d);
        if (!c || !(c->flags & PC_CODEC_SAVE) || !d || !r) {
            free(params);
            free(r);
            pc_doc_destroy(d);
            continue;
        }
        /* how many reports a full encode makes */
        pg.report = rec_report;
        pg.ud = r;
        {
            pc_buf b;
            memset(&b, 0, sizeof b);
            CHECK(pc_codec_save_ex(c, d, NULL, params, NULL, &pg, &b) == PC_OK);
            pc_buf_free(&b);
        }
        calls = r->n;
        at[0] = 1u;                 /* the 0 before any work */
        at[1] = 2u;                 /* the first step of the encoder */
        at[2] = calls / 2u;
        at[3] = calls > 2u ? calls - 1u : 1u;      /* the last before 1 */
        for (int k = 0; k < 4; k++) {
            pc_buf b;
            pc_status st;
            memset(&b, 0, sizeof b);
            CHECK(pc_buf_append(&b, k_prefix, sizeof k_prefix) == PC_OK);
            memset(r, 0, sizeof *r);
            r->cancel_at = at[k] ? at[k] : 1u;
            st = pc_codec_save_ex(c, d, NULL, params, NULL, &pg, &b);
            CHECK(st == PC_ERR_CANCELLED);
            if (st != PC_ERR_CANCELLED)
                INFO("%s case %u: cancel at %u of %u gave %s", oc->id, (unsigned)i,
                     (unsigned)r->cancel_at, (unsigned)calls, pc_status_str(st));
            CHECK(r->n == r->cancel_at);                   /* nothing after the cancel */
            CHECK(r->after_cancel == 0u);
            CHECK(b.n == sizeof k_prefix && memcmp(b.p, k_prefix, sizeof k_prefix) == 0);
            pc_buf_free(&b);
        }
        free(params);
        free(r);
        pc_doc_destroy(d);
    }
}

/* ---- a codec without save_ex ----------------------------------------------------- */
static int g_plain_calls;

static pc_status plain_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                            const pc_par *par, pc_buf *out)
{
    (void)d;
    (void)meta;
    (void)params;
    (void)par;
    g_plain_calls++;
    return pc_buf_append(out, "plain", 5u);
}

static const pc_codec k_plain = {
    "plain", "Plain", "plain", PC_CODEC_SAVE, NULL, NULL, NULL, 0u, 0u, plain_save, NULL
};

static void t_fallback(void)
{
    pc_doc *d = make_doc(8u, 8u, 1u);
    rec *r = (rec *)calloc(1u, sizeof *r);
    pc_codec_progress pg;
    pc_buf b;
    CHECK(d != NULL && r != NULL);
    if (!d || !r) { free(r); pc_doc_destroy(d); return; }
    pg.report = rec_report;
    pg.ud = r;
    memset(&b, 0, sizeof b);
    g_plain_calls = 0;
    CHECK(pc_codec_save_ex(&k_plain, d, NULL, NULL, NULL, &pg, &b) == PC_OK);
    CHECK(g_plain_calls == 1 && b.n == 5u);
    CHECK(r->n == 2u && r->v[0] == 0.0 && r->v[1] == 1.0);
    /* a cancel at 0 never starts the save */
    memset(r, 0, sizeof *r);
    r->cancel_at = 1u;
    CHECK(pc_codec_save_ex(&k_plain, d, NULL, NULL, NULL, &pg, &b) == PC_ERR_CANCELLED);
    CHECK(g_plain_calls == 1 && b.n == 5u);
    /* a cancel at 1: the save ran, its bytes are dropped */
    memset(r, 0, sizeof *r);
    r->cancel_at = 2u;
    CHECK(pc_codec_save_ex(&k_plain, d, NULL, NULL, NULL, &pg, &b) == PC_ERR_CANCELLED);
    CHECK(g_plain_calls == 2 && b.n == 5u);
    /* no observer */
    CHECK(pc_codec_save_ex(&k_plain, d, NULL, NULL, NULL, NULL, &b) == PC_OK);
    CHECK(g_plain_calls == 3 && b.n == 10u);
    pc_buf_free(&b);
    free(r);
    pc_doc_destroy(d);
}

static void t_args(void)
{
    static const pc_codec k_noflag = {
        "noflag", "No flag", "nf", 0u, NULL, NULL, NULL, 0u, 0u, plain_save, NULL
    };
    pc_doc *d = make_doc(4u, 4u, 1u);
    pc_buf b;
    memset(&b, 0, sizeof b);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(pc_codec_save_ex(NULL, d, NULL, NULL, NULL, NULL, &b) == PC_ERR_ARG);
    CHECK(pc_codec_save_ex(&k_plain, NULL, NULL, NULL, NULL, NULL, &b) == PC_ERR_ARG);
    CHECK(pc_codec_save_ex(&k_plain, d, NULL, NULL, NULL, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_codec_save_ex(&k_noflag, d, NULL, NULL, NULL, NULL, &b) == PC_ERR_UNSUPPORTED);
    CHECK(b.n == 0u);
    pc_doc_destroy(d);
}

int main(int argc, char **argv)
{
    (void)rndu;                         /* pc_test.h helpers this test does not use */
    (void)rnd8;
    pc_test_init(argc, argv);
    RUN(t_contract);
    RUN(t_cancel);
    RUN(t_fallback);
    RUN(t_args);
    return pc_test_finish();
}
