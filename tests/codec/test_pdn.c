/* test_pdn.c - .pdn import and export (lane L6C): synthetic round trips,
 * writer options, metadata, real samples, unknown versions, targeted
 * corruptions and a mutation loop. Fixed seeds. */
#include "pdn_util.h"

#include <math.h>

static size_t g_tiles0, g_bytes0, g_layers0;

static void leak_mark(void)
{
    pc_tile_stats(&g_tiles0, &g_bytes0);
    g_layers0 = pc_layer_live_count();
}

static void leak_check(void)
{
    size_t t, b;
    pc_tile_stats(&t, &b);
    CHECK(t == g_tiles0 && b == g_bytes0);
    CHECK(pc_layer_live_count() == g_layers0);
}

static pc_status save(const pc_doc *d, const pdn_save_opts *o, pc_buf *out)
{
    memset(out, 0, sizeof *out);
    return pdn_save_ex(d, NULL, o, NULL, out);
}

/* Save d with o, load it back, compare. Returns the loaded info. */
static void roundtrip(const pc_doc *d, const pdn_save_opts *o, pdn_info *info)
{
    pc_buf b;
    pc_doc *e = NULL;
    pc_image_meta meta;
    pdn_info tmp;
    if (!info) info = &tmp;
    CHECK(save(d, o, &b) == PC_OK);
    CHECK(pdn_load_ex(b.p, b.n, NULL, &e, &meta, info) == PC_OK);
    CHECK(e != NULL);
    if (e) {
        CHECK(pdn_docs_equal(d, e));
        CHECK(pc_doc_edge_padding_is_zero(e));
        CHECK(pc_doc_fingerprint(e) != 0u);
    }
    CHECK(meta.src_bits == 8u && meta.had_alpha);
    pc_meta_free(&meta);
    pc_doc_destroy(e);
    pc_buf_free(&b);
}

/* ---- synthetic round trips ------------------------------------------------- */
static void t_roundtrip_sizes(void)
{
    static const uint32_t sizes[][2] = {
        { 1, 1 }, { 2, 1 }, { 1, 3 }, { 63, 64 }, { 64, 64 }, { 65, 65 }, { 70, 33 },
        { 129, 7 }, { 7, 200 }, { 257, 130 }, { 300, 300 }
    };
    leak_mark();
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t n = 1u + rndu(g_quick ? 4u : 9u);
        pc_doc *d = pdn_random_doc(sizes[i][0], sizes[i][1], n);
        pdn_info info;
        CHECK(d != NULL && d->n_layers == n);
        if (!d) continue;
        roundtrip(d, NULL, &info);
        CHECK(strcmp(info.saved_with, PDN_COMPAT_VERSION) == 0);
        CHECK(info.version_major == 5u && info.n_layers == n);
        CHECK(info.block_format == 0u && info.chunk_size == PDN_CHUNK_SIZE);
        CHECK(!info.legacy_blend && !info.out_of_order);
        CHECK(info.bg_mask == 1u);
        CHECK(info.list_capacity >= n && info.list_capacity >= 4u);
        pc_doc_destroy(d);
    }
    leak_check();
}

static void t_roundtrip_random(void)
{
    uint32_t iters = g_quick ? 12u : 60u;
    leak_mark();
    for (uint32_t it = 0; it < iters; it++) {
        uint32_t w = 1u + rndu(180u), h = 1u + rndu(180u), n = 1u + rndu(12u);
        pc_doc *d = pdn_random_doc(w, h, n);
        CHECK(d != NULL);
        if (!d) continue;
        roundtrip(d, NULL, NULL);
        pc_doc_destroy(d);
    }
    leak_check();
}

static void t_all_blend_modes(void)
{
    pc_doc *d = NULL;
    pc_buf b, dump;
    leak_mark();
    d = pdn_random_doc(97, 61, PC_BLEND_COUNT);
    CHECK(d != NULL);
    if (!d) return;
    for (uint32_t i = 0; i < d->n_layers; i++) {
        d->stack[i]->mode = (pc_blend_mode)i;
        d->stack[i]->opacity = (uint8_t)(i * 18u);
        d->stack[i]->visible = (i % 3u) != 1u;
        snprintf(d->stack[i]->name, PC_LAYER_NAME_MAX, "%s", pc_blend_name((pc_blend_mode)i));
    }
    roundtrip(d, NULL, NULL);
    CHECK(save(d, NULL, &b) == PC_OK);
    memset(&dump, 0, sizeof dump);
    CHECK(pdn_dump(b.p, b.n, 0u, &dump) == PC_OK && pc_buf_put_u8(&dump, 0u) == PC_OK);
    if (dump.p) {
        static const char *const ops[PC_BLEND_COUNT] = {
            "Normal", "Multiply", "Additive", "ColorBurn", "ColorDodge", "Reflect", "Glow",
            "Overlay", "Difference", "Negation", "Lighten", "Darken", "Screen", "Xor"
        };
        for (uint32_t i = 0; i < PC_BLEND_COUNT; i++) {
            char want[96];
            snprintf(want, sizeof want, "PaintDotNet.UserBlendOps+%sBlendOp lib=#2 {}", ops[i]);
            CHECK(strstr((const char *)dump.p, want) != NULL);
        }
        CHECK(strstr((const char *)dump.p, "ArraySingleObject #7 n=16") != NULL);
    }
    pc_buf_free(&dump);
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* Replace the first BinaryObjectString whose value is `from` (LPS < 128)
 * with `to` (shorter than 128 bytes). NRBF stores no offsets, so the file
 * stays consistent. Returns a new buffer or NULL. */
static uint8_t *replace_string(const uint8_t *p, size_t n, const char *from, const char *to,
                               size_t *out_n)
{
    size_t fl = strlen(from), tl = strlen(to);
    for (size_t i = 5; i + 1u + fl <= n; i++) {
        if (p[i] == fl && memcmp(p + i + 1, from, fl) == 0 && p[i - 5] == 6u) {
            uint8_t *q = (uint8_t *)malloc(n - fl + tl + 1u);
            if (!q) return NULL;
            memcpy(q, p, i);
            q[i] = (uint8_t)tl;
            memcpy(q + i + 1, to, tl);
            memcpy(q + i + 1 + tl, p + i + 1 + fl, n - i - 1u - fl);
            *out_n = n - fl + tl;
            return q;
        }
    }
    return NULL;
}

/* Equal layer names share one string record (like a duplicated layer),
 * distinct ones do not; long and invalid names are cleaned on load. */
static void t_names(void)
{
    pc_doc *d = NULL, *e = NULL;
    pdn_save_opts o;
    pdn_info info;
    pc_image_meta meta;
    pc_buf b;
    leak_mark();
    d = pdn_random_doc(20, 10, 4);
    CHECK(d != NULL);
    if (!d) return;
    snprintf(d->stack[0]->name, PC_LAYER_NAME_MAX, "Background");
    snprintf(d->stack[1]->name, PC_LAYER_NAME_MAX, "Same");
    snprintf(d->stack[2]->name, PC_LAYER_NAME_MAX, "Same");
    /* 63 bytes: 21 three-byte characters, the longest name pc_layer keeps */
    d->stack[3]->name[0] = 0;
    for (int i = 0; i < 21; i++) strcat(d->stack[3]->name, "\xe2\x82\xac");
    roundtrip(d, NULL, &info);
    CHECK(info.names_shared && info.names_equal && !info.names_truncated);
    pdn_save_opts_default(&o);
    o.intern_names = false;
    roundtrip(d, &o, &info);
    CHECK(!info.names_shared && info.names_equal);
    CHECK(save(d, NULL, &b) == PC_OK);
    if (b.p) {
        char longname[100];
        size_t n2 = 0;
        uint8_t *q;
        longname[0] = 0;
        for (int i = 0; i < 30; i++) strcat(longname, "\xe2\x82\xac");     /* 90 bytes */
        q = replace_string(b.p, b.n, "Background", longname, &n2);
        CHECK(q != NULL);
        if (q) {
            CHECK(pdn_load_ex(q, n2, NULL, &e, &meta, &info) == PC_OK);
            CHECK(e && strlen(e->stack[0]->name) == 63u &&
                  memcmp(e->stack[0]->name, d->stack[3]->name, 63u) == 0);
            CHECK(info.names_truncated && meta.note[0] != 0);
            pc_meta_free(&meta);
            pc_doc_destroy(e);
            e = NULL;
            free(q);
        }
        q = replace_string(b.p, b.n, "Background", "A\xff\xc3" "B\x01\xed\xa0\x80" "C", &n2);
        CHECK(q != NULL);
        if (q) {
            CHECK(pdn_load_ex(q, n2, NULL, &e, &meta, &info) == PC_OK);
            CHECK(e && strcmp(e->stack[0]->name, "A??B????C") == 0);
            pc_meta_free(&meta);
            pc_doc_destroy(e);
            e = NULL;
            free(q);
        }
    }
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* Writer options: raw blocks, stored deflate, odd chunk sizes including
 * ones that split pixels, and chunks written in descending order. */
static void t_writer_options(void)
{
    static const uint32_t cs[] = { 4u, 5u, 6u, 7u, 9u, 64u, 1000u, 4099u, 65536u, 262145u };
    pc_doc *d = NULL, *small = NULL;
    leak_mark();
    d = pdn_random_doc(131, 67, 3);
    small = pdn_random_doc(31, 17, 2);      /* tiny chunks: many gzip members */
    CHECK(d != NULL && small != NULL);
    if (!d || !small) { pc_doc_destroy(d); pc_doc_destroy(small); return; }
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        const pc_doc *t = cs[i] < 64u ? small : d;
        for (int mode = 0; mode < 4; mode++) {
            pdn_save_opts o;
            pdn_info info;
            pdn_save_opts_default(&o);
            o.chunk_size = cs[i];
            o.level = mode == 0 ? -1 : (mode == 1 ? 0 : (mode == 2 ? 1 : 9));
            o.reverse_chunks = (i + (size_t)mode) % 2u == 1u;
            o.thumbnail = mode != 3;
            if (g_quick && cs[i] < 64u && mode >= 2) continue;
            roundtrip(t, &o, &info);
            CHECK(info.chunk_size == cs[i]);
            CHECK(info.block_format == (o.level < 0 ? 1u : 0u));
            if (o.reverse_chunks && (uint64_t)t->w * t->h * 4u > cs[i]) CHECK(info.out_of_order);
        }
    }
    pc_doc_destroy(small);
    {   /* invalid options */
        pdn_save_opts o;
        pc_buf b;
        pdn_save_opts_default(&o);
        o.chunk_size = 3u;
        CHECK(save(d, &o, &b) == PC_ERR_ARG);
        pc_buf_free(&b);
        pdn_save_opts_default(&o);
        o.level = 10;
        CHECK(save(d, &o, &b) == PC_ERR_ARG);
        pc_buf_free(&b);
        pdn_save_opts_default(&o);
        o.version = "five";
        CHECK(save(d, &o, &b) == PC_ERR_ARG);
        pc_buf_free(&b);
        pdn_save_opts_default(&o);
        o.list_capacity = 2u;
        CHECK(save(d, &o, &b) == PC_ERR_ARG);
        pc_buf_free(&b);
    }
    pc_doc_destroy(d);
    leak_check();
}

/* A pc_par that runs jobs backwards with rotating worker ids: output must be
 * identical to the serial save. */
static void par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    uint32_t threads = *(const uint32_t *)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, i % threads);
}

static void t_parallel_determinism(void)
{
    uint32_t threads = 3u;
    pc_par par;
    pc_doc *d = NULL;      /* 840000 bytes: 4 chunks per layer */
    pc_buf a, b;
    leak_mark();
    d = pdn_random_doc(700, 300, 2);
    par.run = par_run;
    par.self = &threads;
    par.threads = threads;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(pdn_save_ex(d, NULL, NULL, NULL, &a) == PC_OK);
    CHECK(pdn_save_ex(d, NULL, NULL, &par, &b) == PC_OK);
    CHECK(a.n == b.n && a.n > 0u && memcmp(a.p, b.p, a.n) == 0);
    roundtrip(d, NULL, NULL);
    pc_buf_free(&a);
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* ---- metadata and header ----------------------------------------------------------- */
static void t_metadata(void)
{
    static const double dpis[] = { 96.0, 300.0, 72.5, 95.9993, 1200.0, 37.795 };
    pc_doc *d = NULL;
    leak_mark();
    d = pdn_random_doc(16, 9, 1);
    CHECK(d != NULL);
    if (!d) return;
    for (size_t i = 0; i < sizeof dpis / sizeof dpis[0]; i++) {
        pc_image_meta in, out;
        pc_buf b;
        pc_doc *e = NULL;
        uint8_t icc[300];
        memset(&in, 0, sizeof in);
        memset(&b, 0, sizeof b);
        for (size_t k = 0; k < sizeof icc; k++) icc[k] = (uint8_t)(k * 7u + i);
        in.dpi_x = dpis[i];
        in.dpi_y = dpis[(i + 1u) % (sizeof dpis / sizeof dpis[0])];
        if (i & 1u) { in.icc = icc; in.icc_len = sizeof icc - i; }
        CHECK(pdn_save_ex(d, &in, NULL, NULL, &b) == PC_OK);
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &out) == PC_OK);
        CHECK(fabs(out.dpi_x - in.dpi_x) < 1e-9 * in.dpi_x);
        CHECK(fabs(out.dpi_y - in.dpi_y) < 1e-9 * in.dpi_y);
        if (i & 1u) CHECK(out.icc_len == in.icc_len && memcmp(out.icc, icc, in.icc_len) == 0);
        else CHECK(out.icc == NULL && out.icc_len == 0u);
        pc_meta_free(&out);
        pc_doc_destroy(e);
        pc_buf_free(&b);
    }
    {   /* no meta: 96 dpi and the Software tag */
        pc_buf b, dump;
        pc_image_meta out;
        pc_doc *e = NULL;
        memset(&b, 0, sizeof b);
        memset(&dump, 0, sizeof dump);
        CHECK(pdn_save_ex(d, NULL, NULL, NULL, &b) == PC_OK);
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &out) == PC_OK);
        CHECK(out.dpi_x == 96.0 && out.dpi_y == 96.0);
        CHECK(pdn_dump(b.p, b.n, 0u, &dump) == PC_OK && pc_buf_put_u8(&dump, 0u) == PC_OK);
        /* "paint.c\0" in base64 */
        CHECK(dump.p && strstr((const char *)dump.p, "id=\\\"305\\\" len=\\\"8\\\" type=\\\"2\\\" "
                                                   "value=\\\"cGFpbnQuYwA=\\\"") != NULL);
        CHECK(dump.p && strstr((const char *)dump.p, "\"$exif.tag0[0]\"") != NULL);
        pc_buf_free(&dump);
        pc_meta_free(&out);
        pc_doc_destroy(e);
        pc_buf_free(&b);
    }
    pc_doc_destroy(d);
    leak_check();
}

static uint32_t be32_at(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                                   ((uint32_t)p[2] << 8) | p[3]; }

/* Header XML and thumbnail: Paint.NET's sizes (longest side 256, floor). */
static void t_header_thumbnail(void)
{
    static const uint32_t cases[][4] = {
        { 159, 53, 159, 53 }, { 800, 600, 256, 192 }, { 742, 94, 256, 32 },
        { 523, 337, 256, 164 }, { 131, 257, 130, 256 }, { 293, 103, 256, 89 },
        { 6764, 168, 256, 6 }, { 1680, 27, 256, 4 }, { 256, 256, 256, 256 },
        { 257, 1, 256, 1 }, { 1, 1, 1, 1 }, { 2, 2, 2, 2 }
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t tw, th;
        pdn_thumb_size(cases[i][0], cases[i][1], &tw, &th);
        CHECK(tw == cases[i][2] && th == cases[i][3]);
    }
    {
        pc_doc *d;
        pc_buf b;
        leak_mark();
        d = pdn_random_doc(523, 337, 2);
        CHECK(d != NULL);
        memset(&b, 0, sizeof b);
        if (d && pdn_save_ex(d, NULL, NULL, NULL, &b) == PC_OK && b.n > 7u) {
            size_t hl = (size_t)b.p[4] | ((size_t)b.p[5] << 8) | ((size_t)b.p[6] << 16);
            const char *x = (const char *)b.p + 7;
            const char *pre = "<pdnImage width=\"523\" height=\"337\" layers=\"2\" "
                              "savedWithVersion=\"" PDN_COMPAT_VERSION "\"><custom><thumb png=\"";
            CHECK(memcmp(b.p, "PDN3", 4u) == 0);
            CHECK(hl + 9u < b.n && b.p[7u + hl] == 0u && b.p[8u + hl] == 1u);
            CHECK(hl > strlen(pre) && memcmp(x, pre, strlen(pre)) == 0);
            CHECK(memcmp(x + hl - 26u, "\" /></custom></pdnImage>", 24u) == 0 ||
                  memcmp(x + hl - 24u, "\" /></custom></pdnImage>", 24u) == 0);
            {
                const char *s = x + strlen(pre), *e = s;
                uint8_t *png = (uint8_t *)malloc(hl);
                size_t pl = 0;
                while (*e != '"') e++;
                CHECK(png && pdn_base64_decode(s, (size_t)(e - s), png, hl, &pl));
                if (png && pl > 33u) {
                    CHECK(memcmp(png, "\x89PNG\r\n\x1a\n", 8u) == 0);
                    CHECK(memcmp(png + 12, "IHDR", 4u) == 0);
                    CHECK(be32_at(png + 16) == 256u && be32_at(png + 20) == 164u);
                    CHECK(memcmp(png + 37, "sRGB", 4u) == 0);
                }
                free(png);
            }
        }
        pc_buf_free(&b);
        pc_doc_destroy(d);
        leak_check();
    }
    {   /* base64 */
        uint8_t out[16];
        size_t n = 0;
        pc_buf b;
        memset(&b, 0, sizeof b);
        CHECK(pdn_base64_encode((const uint8_t *)"paint.c", 8u, &b) == PC_OK);
        CHECK(b.n == 12u && memcmp(b.p, "cGFpbnQuYwA=", 12u) == 0);
        pc_buf_free(&b);
        CHECK(pdn_base64_decode("YAAAAAEAAAA=", 12u, out, sizeof out, &n) && n == 8u &&
              out[0] == 96u && out[4] == 1u);
        CHECK(!pdn_base64_decode("YAAAAAEAAAA", 11u, out, sizeof out, &n));
        CHECK(!pdn_base64_decode("YA=A", 4u, out, sizeof out, &n));
        CHECK(!pdn_base64_decode("Y!AA", 4u, out, sizeof out, &n));
        CHECK(!pdn_base64_decode("YAAAAAEAAAAA", 12u, out, 8u, &n));
    }
}

/* ---- registry ------------------------------------------------------------------ */
static void t_registry(void)
{
    const pc_codec *c = pc_codec_by_id("pdn"), *used = NULL;
    pc_doc *d = pdn_random_doc(33, 17, 2), *e = NULL;
    pc_image_meta meta;
    pc_buf b;
    memset(&b, 0, sizeof b);
    CHECK(c == &pc_codec_pdn);
    CHECK(pc_codec_by_ext("PDN") == &pc_codec_pdn);
    CHECK(c && (c->flags & PC_CODEC_LAYERED) && (c->flags & PC_CODEC_LOAD) &&
          (c->flags & PC_CODEC_SAVE));
    CHECK(c && c->sniff((const uint8_t *)"PDN3", 4u) && c->sniff((const uint8_t *)"PDN4xx", 6u));
    CHECK(c && !c->sniff((const uint8_t *)"PDN", 3u) && !c->sniff((const uint8_t *)"PDNx", 4u));
    CHECK(c && !c->sniff((const uint8_t *)"[Event", 6u));
    CHECK(d && c && c->save(d, NULL, NULL, NULL, &b) == PC_OK);
    CHECK(pc_codec_sniff(b.p, b.n) == &pc_codec_pdn);
    CHECK(pc_codec_load_any(b.p, b.n, "x.pdn", NULL, &e, &meta, &used) == PC_OK);
    CHECK(used == &pc_codec_pdn && e && d && pdn_docs_equal(d, e));
    pc_meta_free(&meta);
    pc_doc_destroy(e);
    pc_buf_free(&b);
    {   /* no layers / bad args */
        pc_doc *z = pc_doc_create(4, 4);
        memset(&b, 0, sizeof b);
        CHECK(z && pdn_save_ex(z, NULL, NULL, NULL, &b) == PC_ERR_ARG);
        CHECK(pdn_save_ex(NULL, NULL, NULL, NULL, &b) == PC_ERR_ARG);
        CHECK(pdn_load_ex(NULL, 0u, NULL, &e, &meta, NULL) == PC_ERR_ARG);
        pc_doc_destroy(z);
        pc_buf_free(&b);
    }
    pc_doc_destroy(d);
}

/* ---- real samples ----------------------------------------------------------------- */
typedef struct sample_expect {
    const char *file;
    uint32_t w, h, n;
    const char *version;
    bool legacy;
} sample_expect;

static void t_samples(void)
{
    static const sample_expect ex[] = {
        { "bevy_bar_border.pdn", 742, 94, 3, "5.3.8488.42200", false },
        { "bevy_health_bar_fill.pdn", 742, 94, 2, "5.3.8488.42200", false },
        { "bevy_posture_bar_fill.pdn", 742, 94, 2, "5.3.8488.42200", false },
        { "bevy_posture_bar_top.pdn", 742, 94, 2, "5.3.8488.42200", false },
        { "pypdn_Untitled.pdn", 800, 600, 1, "4.21.6589.7045", false },
        { "pypdn_Untitled2.pdn", 800, 600, 2, "4.21.6589.7045", false },
        { "pypdn_Untitled3.pdn", 800, 600, 2, "4.21.6589.7045", false },
        { "pypdn_oldPDN3510.pdn", 800, 600, 2, "3.510.4297.28964", true },
        { "pypdn_FlattenBlendTest.pdn", 800, 600, 14, "4.21.6589.7045", false },
    };
    int found = 0;
    leak_mark();
    for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        char path[256];
        size_t n;
        uint8_t *p;
        pc_doc *d = NULL, *e = NULL;
        pc_image_meta meta, meta2;
        pdn_info info;
        pc_buf b;
        snprintf(path, sizeof path, PDN_SAMPLE_DIR "%s", ex[i].file);
        p = pdn_read_file(path, &n);
        if (!p) continue;
        found++;
        memset(&b, 0, sizeof b);
        CHECK(pdn_load_ex(p, n, NULL, &d, &meta, &info) == PC_OK);
        CHECK(d && d->w == ex[i].w && d->h == ex[i].h && d->n_layers == ex[i].n);
        CHECK(strcmp(info.saved_with, ex[i].version) == 0);
        CHECK(info.legacy_blend == ex[i].legacy);
        CHECK(info.block_format == 0u);
        if (d && strstr(ex[i].file, "FlattenBlendTest")) {
            for (uint32_t k = 0; k < d->n_layers; k++) CHECK(d->stack[k]->mode == (pc_blend_mode)k);
            CHECK(strcmp(d->stack[0]->name, "Background") == 0);
            CHECK(strcmp(d->stack[13]->name, "XOR") == 0);
        }
        if (d && strstr(ex[i].file, "Untitled3")) {
            CHECK(d->stack[1]->mode == PC_BLEND_ADDITIVE && d->stack[1]->opacity == 161u);
            CHECK(meta.dpi_x == 96.0 && meta.dpi_y == 96.0);
        }
        if (d && strstr(ex[i].file, "bar_border")) {
            CHECK(info.names_shared);       /* a duplicated "Layer 2" */
            CHECK(strcmp(d->stack[1]->name, "Layer 2") == 0 &&
                  strcmp(d->stack[2]->name, "Layer 2") == 0);
        }
        /* read, write, read: identical pixels and properties (test d); the
         * 14-layer 800 x 600 sample only in full runs (sanitizer time) */
        if (d && (!g_quick || d->n_layers < 10u)) {
            CHECK(pc_codec_pdn.save(d, &meta, NULL, NULL, &b) == PC_OK);
            CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta2) == PC_OK);
            CHECK(e && pdn_docs_equal(d, e));
            if (meta.dpi_x > 0.0) CHECK(meta2.dpi_x == meta.dpi_x && meta2.dpi_y == meta.dpi_y);
            pc_meta_free(&meta2);
        }
        pc_meta_free(&meta);
        pc_doc_destroy(d);
        pc_doc_destroy(e);
        pc_buf_free(&b);
        free(p);
    }
    if (!found) INFO("no samples in %s, skipped", PDN_SAMPLE_DIR);
    else INFO("%d samples", found);
    leak_check();
}

/* ---- unknown versions and containers ------------------------------------------------- */
static void t_versions(void)
{
    pc_doc *d = NULL, *e = NULL;
    pc_image_meta meta;
    pdn_save_opts o;
    pc_buf b;
    leak_mark();
    d = pdn_random_doc(40, 30, 2);
    CHECK(d != NULL);
    if (!d) return;
    pdn_save_opts_default(&o);
    /* Paint.NET 6 and later: header and NRBF both say 6 */
    o.version = "6.0.9999.1";
    CHECK(save(d, &o, &b) == PC_OK);
    CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_UNSUPPORTED && e == NULL);
    pc_buf_free(&b);
    /* only the NRBF System.Version says 6 (header patched back to 5) */
    CHECK(save(d, &o, &b) == PC_OK);
    if (b.p) {
        char *v = strstr((char *)b.p + 7, "savedWithVersion=\"6");
        CHECK(v != NULL);
        if (v) v[18] = '5';
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_UNSUPPORTED);
    }
    pc_buf_free(&b);
    /* 5.2 (same format) loads */
    o.version = "5.200.9772.9330";
    roundtrip(d, &o, NULL);
    o.version = "4.0.0.0";
    roundtrip(d, &o, NULL);
    CHECK(save(d, NULL, &b) == PC_OK);
    if (b.p && b.n > 16u) {
        size_t hl = (size_t)b.p[4] | ((size_t)b.p[5] << 8) | ((size_t)b.p[6] << 16);
        uint8_t keep;
        b.p[3] = '4';               /* a newer container revision */
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_UNSUPPORTED);
        CHECK(pc_codec_sniff(b.p, b.n) == &pc_codec_pdn);
        b.p[3] = '3';
        keep = b.p[7u + hl];
        b.p[7u + hl] = 0x1Fu;       /* Paint.NET 2.x gzip container */
        b.p[8u + hl] = 0x8Bu;
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_UNSUPPORTED);
        b.p[7u + hl] = 0x00u;
        b.p[8u + hl] = 0x02u;       /* unknown data indicator */
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_UNSUPPORTED);
        b.p[7u + hl] = keep;
        b.p[8u + hl] = 0x01u;
        b.p[7] = '[';               /* not a pdnImage header */
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_FORMAT);
        b.p[7] = '<';
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_OK);
        pc_meta_free(&meta);
        pc_doc_destroy(e);
        e = NULL;
        b.p[6] = 0x7Fu;             /* header longer than the file */
        CHECK(pc_codec_pdn.load(b.p, b.n, NULL, &e, &meta) == PC_ERR_FORMAT);
        CHECK(pc_codec_pdn.load(b.p, 3u, NULL, &e, &meta) == PC_ERR_FORMAT);
        CHECK(pc_codec_pdn.load((const uint8_t *)"GIF89a", 6u, NULL, &e, &meta) ==
              PC_ERR_UNSUPPORTED);
    }
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* ---- limits --------------------------------------------------------------------- */
static void t_limits(void)
{
    pc_doc *d = NULL, *e = NULL;
    pc_image_meta meta;
    pc_codec_limits lim;
    pc_buf b;
    leak_mark();
    d = pdn_random_doc(90, 40, 5);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(save(d, NULL, &b) == PC_OK);
    pc_codec_limits_default(&lim);
    lim.max_w = 89;
    CHECK(pc_codec_pdn.load(b.p, b.n, &lim, &e, &meta) == PC_ERR_LIMIT && e == NULL);
    pc_codec_limits_default(&lim);
    lim.max_layers = 4;
    CHECK(pc_codec_pdn.load(b.p, b.n, &lim, &e, &meta) == PC_ERR_LIMIT);
    pc_codec_limits_default(&lim);
    lim.max_mem = (uint64_t)90u * 40u * 4u * 4u;
    CHECK(pc_codec_pdn.load(b.p, b.n, &lim, &e, &meta) == PC_ERR_LIMIT);
    lim.max_mem *= 2u;
    CHECK(pc_codec_pdn.load(b.p, b.n, &lim, &e, &meta) == PC_OK);
    pc_meta_free(&meta);
    pc_doc_destroy(e);
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* ---- targeted corruptions ------------------------------------------------------------- */
static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(p + i, pat, k) == 0) return p + i;
    return NULL;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Offset of the first deferred block (just past MessageEnd). */
static size_t blocks_offset(const uint8_t *p, size_t n)
{
    size_t hl = (size_t)p[4] | ((size_t)p[5] << 8) | ((size_t)p[6] << 16), off = 9u + hl;
    nrbf_doc nd;
    size_t end = 0;
    if (nrbf_parse(p + off, n - off, NULL, &nd) == PC_OK) end = off + nd.end;
    nrbf_free(&nd);
    return end;
}

static pc_status load_status(const uint8_t *p, size_t n)
{
    pc_doc *e = NULL;
    pc_image_meta meta;
    pc_status st = pc_codec_pdn.load(p, n, NULL, &e, &meta);
    if (st == PC_OK) { pc_meta_free(&meta); pc_doc_destroy(e); }
    else CHECK(e == NULL);
    return st;
}

static void t_corruptions(void)
{
    const uint32_t W = 77, H = 45;          /* 13860 bytes per layer */
    pc_doc *d = NULL;
    pdn_save_opts o;
    pc_buf b;
    leak_mark();
    d = pdn_random_doc(W, H, 2);
    CHECK(d != NULL);
    if (!d) return;
    pdn_save_opts_default(&o);
    o.chunk_size = 4096u;                   /* 4 chunks per layer */
    o.thumbnail = false;
    CHECK(save(d, &o, &b) == PC_OK);
    if (b.p) {
        uint8_t *c = (uint8_t *)malloc(b.n);
        size_t bo = blocks_offset(b.p, b.n);
        uint8_t sig[12], *hit;
        CHECK(c != NULL && bo > 0u);
        if (!c || !bo) { free(c); goto out; }
        CHECK(load_status(b.p, b.n) == PC_OK);
        /* block header: format byte, chunk size */
        memcpy(c, b.p, b.n); c[bo] = 2u;
        CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        memcpy(c, b.p, b.n); put_be32(c + bo + 1u, 0u);
        CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        memcpy(c, b.p, b.n); put_be32(c + bo + 1u, 2u);
        CHECK(load_status(c, b.n) == PC_ERR_UNSUPPORTED);
        memcpy(c, b.p, b.n); put_be32(c + bo + 1u, 8192u);   /* fewer chunks than written */
        CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        /* first chunk: number out of range, duplicated, size beyond the file */
        memcpy(c, b.p, b.n); put_be32(c + bo + 5u, 4u);
        CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        {
            size_t s0 = be32_at(b.p + bo + 9u), second = bo + 13u + s0;
            memcpy(c, b.p, b.n); put_be32(c + second, 0u);
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
            memcpy(c, b.p, b.n); put_be32(c + bo + 9u, (uint32_t)b.n);
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
            /* gzip payload damage: CRC, deflate data, header */
            memcpy(c, b.p, b.n); c[bo + 13u + s0 - 5u] ^= 0x40u;
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
            memcpy(c, b.p, b.n); c[bo + 13u + 12u] ^= 0xFFu;
            CHECK(load_status(c, b.n) != PC_OK);
            memcpy(c, b.p, b.n); c[bo + 13u] = 0x00u;
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        }
        CHECK(load_status(b.p, b.n - 1u) == PC_ERR_FORMAT);           /* truncated */
        CHECK(load_status(b.p, bo + 3u) == PC_ERR_FORMAT);
        /* Surface: width, height, stride */
        put_le32(sig, W); put_le32(sig + 4, H); put_le32(sig + 8, W * 4u);
        memcpy(c, b.p, b.n);
        hit = find_bytes(c, bo, sig, 12u);
        CHECK(hit != NULL);
        if (hit) {
            put_le32(hit + 8, W * 3u);
            CHECK(load_status(c, b.n) == PC_ERR_UNSUPPORTED);         /* 24-bit */
            put_le32(hit + 8, W * 4u + 4u);
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
            put_le32(hit + 8, W * 4u);
            put_le32(hit, W + 1u);
            CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
        }
        /* MemoryBlock: length64, hasParent, deferred */
        {
            uint64_t len = (uint64_t)W * H * 4u;
            for (int k = 0; k < 8; k++) sig[k] = (uint8_t)(len >> (8 * k));
            sig[8] = 0u; sig[9] = 1u;
            memcpy(c, b.p, b.n);
            hit = find_bytes(c, bo, sig, 10u);
            CHECK(hit != NULL);
            if (hit) {
                hit[8] = 1u;
                CHECK(load_status(c, b.n) == PC_ERR_UNSUPPORTED);     /* parent block */
                hit[8] = 0u;
                hit[9] = 0u;                    /* not deferred and no pointerData */
                CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
                hit[9] = 1u;
                hit[0] ^= 4u;                   /* length != stride * height */
                CHECK(load_status(c, b.n) == PC_ERR_FORMAT);
            }
        }
        /* opacity outside a byte cannot be expressed; blend mode out of range
         * falls back to Normal with a note */
        {
            pc_doc *e = NULL;
            pc_image_meta meta;
            static const uint8_t vm[] = { 'v', 'a', 'l', 'u', 'e', '_', '_', 0x00, 0x08 };
            memcpy(c, b.p, b.n);
            hit = find_bytes(c, bo, vm, sizeof vm);
            CHECK(hit != NULL);
            if (hit) {
                /* the first LayerBlendMode value follows its class record */
                uint8_t *v = hit + sizeof vm + 4u;
                put_le32(v, 77u);
                CHECK(pc_codec_pdn.load(c, b.n, NULL, &e, &meta) == PC_OK);
                CHECK(e && e->stack[0]->mode == PC_BLEND_NORMAL && meta.note[0] != 0);
                pc_meta_free(&meta);
                pc_doc_destroy(e);
            }
        }
        free(c);
    }
out:
    pc_buf_free(&b);
    pc_doc_destroy(d);
    leak_check();
}

/* ---- mutation loop ---------------------------------------------------------------------- */
static void mutate(uint8_t *p, size_t n, size_t *len)
{
    uint32_t kind = rndu(8);
    size_t at = n ? rndu((uint32_t)n) : 0u;
    switch (kind) {
    case 0: case 1:                                     /* flip bits */
        for (uint32_t k = 0, m = 1u + rndu(4); k < m; k++)
            p[rndu((uint32_t)n)] ^= (uint8_t)(1u << rndu(8));
        break;
    case 2:                                             /* random bytes */
        for (uint32_t k = 0, m = 1u + rndu(8); k < m && at + k < n; k++) p[at + k] = rnd8();
        break;
    case 3: {                                           /* interesting values */
        static const uint8_t v[] = { 0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE };
        for (uint32_t k = 0; k < 4u && at + k < n; k++) p[at + k] = v[rndu(6)];
        break;
    }
    case 4:                                             /* truncate */
        *len = at;
        break;
    case 5:                                             /* delete a span */
        if (at < n) {
            size_t k = 1u + rndu(16);
            if (at + k > n) k = n - at;
            memmove(p + at, p + at + k, n - at - k);
            *len = n - k;
        }
        break;
    case 6: {                                           /* duplicate a span forward */
        size_t k = 1u + rndu(16);
        if (at + 2u * k <= n) memcpy(p + at + k, p + at, k);
        break;
    }
    default:                                            /* big 32-bit values */
        if (at + 4u <= n) {
            uint32_t v = rndu(2) ? 0x7FFFFFFFu : (uint32_t)rnd();
            if (rndu(2)) put_be32(p + at, v);
            else put_le32(p + at, v);
        }
        break;
    }
}

static void fuzz_one(const uint8_t *src, size_t n, uint32_t iters, unsigned long *ok_count)
{
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1u);
    pc_codec_limits lim;
    pc_codec_limits_default(&lim);
    lim.max_mem = 64u << 20;            /* bounded allocation per load */
    lim.max_pixels = 1u << 22;
    if (!buf) return;
    for (uint32_t it = 0; it < iters; it++) {
        size_t len = n;
        pc_doc *e = NULL;
        pc_image_meta meta;
        pc_status st;
        memcpy(buf, src, n);
        mutate(buf, n, &len);
        if (rndu(4) == 0) mutate(buf, len, &len);
        st = pc_codec_pdn.load(buf, len, &lim, &e, &meta);
        CHECK(st == PC_OK || st == PC_ERR_FORMAT || st == PC_ERR_UNSUPPORTED ||
              st == PC_ERR_LIMIT);
        if (st == PC_OK) {
            (*ok_count)++;
            CHECK(e && e->n_layers >= 1u && pc_doc_edge_padding_is_zero(e));
            pc_meta_free(&meta);
            pc_doc_destroy(e);
        } else {
            CHECK(e == NULL && meta.icc == NULL);
        }
    }
    free(buf);
}

static void t_mutation(void)
{
    unsigned long ok = 0;
    uint32_t iters = g_quick ? 1500u : 30000u;
    pc_buf b;
    pdn_save_opts o;
    pc_doc *d;
    leak_mark();
    /* small synthetic files: gzip and raw blocks, unaligned chunks */
    d = pdn_random_doc(23, 11, 3);
    CHECK(d != NULL);
    if (d) {
        pdn_save_opts_default(&o);
        o.thumbnail = false;
        o.chunk_size = 300u;
        CHECK(save(d, &o, &b) == PC_OK);
        fuzz_one(b.p, b.n, iters, &ok);
        pc_buf_free(&b);
        o.level = -1;
        o.chunk_size = 255u;
        o.reverse_chunks = true;
        CHECK(save(d, &o, &b) == PC_OK);
        fuzz_one(b.p, b.n, iters, &ok);
        pc_buf_free(&b);
        pc_doc_destroy(d);
    }
    /* small real samples */
    {
        static const char *const files[] = { "bevy_posture_bar_top.pdn", "pypdn_Untitled3.pdn" };
        for (size_t i = 0; i < 2u; i++) {
            char path[256];
            size_t n;
            uint8_t *p;
            snprintf(path, sizeof path, PDN_SAMPLE_DIR "%s", files[i]);
            p = pdn_read_file(path, &n);
            if (!p) continue;
            fuzz_one(p, n, g_quick ? 300u : iters, &ok);
            free(p);
        }
    }
    INFO("%lu mutated files still loaded", ok);
    leak_check();
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_roundtrip_sizes);
    RUN(t_roundtrip_random);
    RUN(t_all_blend_modes);
    RUN(t_names);
    RUN(t_writer_options);
    RUN(t_parallel_determinism);
    RUN(t_metadata);
    RUN(t_header_thumbnail);
    RUN(t_registry);
    RUN(t_samples);
    RUN(t_versions);
    RUN(t_limits);
    RUN(t_corruptions);
    RUN(t_mutation);
    return pc_test_finish();
}
