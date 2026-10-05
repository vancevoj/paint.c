/* test_fxc_history.c - lane W3B-FXCORE: history beyond the RAM budget in the
 * editor (PARITY F-CORE-HIST-BUDGET, T-L1-08, X-23).
 *  - with a small budget, 24 large paint steps all stay undoable: the RAM
 *    stays within the budget, tiles go to the swap file, undo and redo
 *    through the app reproduce every step exactly;
 *  - the swap file lives in the private state directory and leaves no file
 *    behind (unlinked at once on POSIX, removed on close elsewhere); a stale
 *    swap file of a crashed session is swept;
 *  - with files disabled (empty config dir) the same works by packing in
 *    memory;
 *  - a swap file that cannot be read makes Undo fail with a message and the
 *    image unchanged.
 */
#include "pc_test.h"
#include "f_test_util.h"
#include "doc_spill.h"
#include "pc/pc_hist_spill.h"

#define IW 512
#define IH 384
#define STEPS 24

static app *make_app(const char *cfg)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = 900;
    o.height = 700;
    o.workers = 3;
    o.config_dir = cfg;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    return app_create(&o);
}

static app_doc *add_image(app *a)
{
    app_doc *d = app_doc_new_image(a, IW, IH, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) return NULL;
    at_frames(a, 2);
    return d;
}

/* A large edit of about 1.2 MiB, committed like a tool does: noise (which
 * no packer shrinks) or smooth bands (which pack well). */
static bool g_noise = true;
static void paint(app *a, app_doc *d, uint32_t seed)
{
    pc_layer *l = app_doc_layer(d);
    pc_txn *t = pc_txn_begin(d->doc, "Paintbrush");
    pc_surf s;
    if (!t || !l || pc_surf_alloc(&s, 300, 250) != PC_OK) {
        if (t) pc_txn_cancel(t);
        CHECK(!"paint setup");
        return;
    }
    for (int32_t i = 0; i < s.w * s.h; i++) {
        uint32_t h = (uint32_t)i * 2654435761u ^ seed * 0x9E3779B9u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        if (!g_noise) h = (uint32_t)(i / 300) * 3u + seed * 11u + (uint32_t)(i % 300) / 40u;
        s.px[i].r = (uint8_t)h;
        s.px[i].g = (uint8_t)(h >> 8);
        s.px[i].b = (uint8_t)(h >> 16);
        s.px[i].a = 255;
    }
    CHECK(pc_txn_write_rect(t, l->id, pc_rect_make((int32_t)(seed * 37u % 200u),
                                                   (int32_t)(seed * 53u % 130u), s.w, s.h),
                            s.px, (size_t)s.stride) == PC_OK);
    CHECK(pc_txn_commit(t, d->hist) == PC_OK);
    app_doc_history_changed(a, d);
    pc_surf_free(&s);
}

static int count_swaps(const char *dir)
{
    char **names = NULL;
    int n = pal_list_dir(dir, "hswap-*.tmp", &names);
    pal_free_names(names, n);
    return n < 0 ? 0 : n;
}

static void run(const char *cfg, bool expect_file)
{
    app *a = make_app(cfg);
    app_doc *d = a ? add_image(a) : NULL;
    uint64_t fp[STEPS + 1];
    size_t cur = 0, total;
    pc_hist_spill_stats st;
    long bad = 0;
    CHECK(d != NULL);
    if (!d) {
        if (a) app_destroy(a);
        return;
    }
    a->hist_budget = (size_t)4 << 20;                     /* 4 MiB */
    fp[0] = pc_doc_fingerprint(d->doc);
    for (int i = 1; i <= STEPS; i++) {
        paint(a, d, (uint32_t)i);
        fp[i] = pc_doc_fingerprint(d->doc);
    }
    total = app_doc_history_list(d, NULL, 0, &cur);
    CHECK(total == STEPS + 1u && cur == STEPS);           /* nothing dropped */
    CHECK(pc_hist_resident_bytes(d->hist) <= a->hist_budget);
    pc_hist_spill_stats_get(d->hist, &st);
    INFO("%s: %u tiles spilled (%.1f MiB packed, file %.1f MiB), RAM %.1f MiB",
         expect_file ? "file" : "memory", (unsigned)st.tiles,
         (double)st.packed_bytes / 1048576.0, (double)st.file_bytes / 1048576.0,
         (double)pc_hist_resident_bytes(d->hist) / 1048576.0);
    CHECK(st.tiles > 100u);
    CHECK(expect_file == (st.file_bytes > 0u));
    for (int i = STEPS; i > 0; i--) {
        CHECK(app_doc_undo(a, d));
        if (pc_doc_fingerprint(d->doc) != fp[i - 1]) bad++;
    }
    CHECK(!app_doc_can_undo(d));
    for (int i = 1; i <= STEPS; i++) {
        CHECK(app_doc_redo(a, d));
        if (pc_doc_fingerprint(d->doc) != fp[i]) bad++;
    }
    CHECK(bad == 0);
    {
        pc_hist_node *list[STEPS + 1];
        (void)app_doc_history_list(d, list, STEPS + 1u, &cur);
        CHECK(app_doc_history_jump(a, d, list[3]) == PC_OK);
        CHECK(pc_doc_fingerprint(d->doc) == fp[3]);
        CHECK(app_doc_history_jump(a, d, list[STEPS]) == PC_OK);
        CHECK(pc_doc_fingerprint(d->doc) == fp[STEPS]);
    }
    CHECK(pc_hist_resident_bytes(d->hist) <= a->hist_budget);
    app_destroy(a);
}

static void t_swap_file(void)
{
    char cfg[1024], state[1100], stale[1200];
    snprintf(cfg, sizeof cfg, "%s/fxc_hist_cfg", PC_APP_TEST_OUT_DIR);
    snprintf(state, sizeof state, "%s/state", cfg);
    CHECK(pal_mkdirs(state));
    /* a leftover of a crashed session */
    snprintf(stale, sizeof stale, "%s/hswap-7-1-deadbeef.tmp", state);
    CHECK(pal_write_file_atomic(stale, "x", 1u) == PC_OK);
    CHECK(count_swaps(state) == 1);
    run(cfg, true);
    CHECK(count_swaps(state) == 0);                        /* swept, nothing left */
}

static void t_memory(void)
{
    /* in memory the packed bytes still count: content that packs well */
    g_noise = false;
    run("", false);
    g_noise = true;
}

/* Undo of a step whose tiles cannot come back: an error, image unchanged. */
static bool fail_read(void *self, uint64_t off, void *p, size_t n)
{
    (void)self; (void)off; (void)p; (void)n;
    return false;
}
static bool ok_write(void *self, uint64_t off, const void *p, size_t n)
{
    (void)self; (void)off; (void)p; (void)n;
    return true;
}

static void t_read_error(void)
{
    app *a = make_app("");
    app_doc *d = a ? add_image(a) : NULL;
    pc_spill_io io;
    uint64_t fp;
    CHECK(d != NULL);
    if (!d) {
        if (a) app_destroy(a);
        return;
    }
    io.write = ok_write;
    io.read = fail_read;
    io.close = NULL;
    io.self = NULL;
    CHECK(pc_hist_spill_enable(d->hist, &io, &a->par) == PC_OK);
    a->hist_budget = (size_t)2 << 20;
    for (int i = 1; i <= 8; i++) paint(a, d, (uint32_t)(100 + i));
    fp = pc_doc_fingerprint(d->doc);
    /* the most recent steps may still be in RAM: undo until one needs the file */
    for (int i = 0; i < 8 && app_doc_undo(a, d); i++) fp = pc_doc_fingerprint(d->doc);
    CHECK(app_doc_can_undo(d));                            /* stopped by the error */
    CHECK(pc_doc_fingerprint(d->doc) == fp);
    CHECK(app_doc_spill_error(d) != NULL);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_swap_file);
    RUN(t_memory);
    RUN(t_read_error);
    at_quit();
    return pc_test_finish();
}
