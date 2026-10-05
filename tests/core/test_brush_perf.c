/* test_brush_perf.c - interactivity of the stroke engine: a width 300
 * brush stroke across a 4096 x 4096 layer, one input event per ~8 px of
 * pointer travel, measured per event on one core (par = NULL). The target
 * is < 4 ms per typical event. Timing is only enforced (with headroom for
 * a loaded machine) in optimized builds without sanitizers; other builds
 * just report it. */
#include "pc_test.h"
#include "test_brush_util.h"

#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#  define PERF_ENFORCE 1
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    undef PERF_ENFORCE
#  endif
#endif

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* layer filled with shared opaque tiles (cheap for a big canvas) */
static void big_doc(tdoc *td, uint32_t w, uint32_t h)
{
    pc_layer *l;
    pc_px32 c = pxc(200, 180, 160, 255);
    td->d = pc_doc_create(w, h);
    td->h = pc_hist_create(td->d);
    l = pc_layer_create(td->d, "L");
    td->lid = l->id;
    for (uint32_t ty = 0; ty < td->d->tiles_y; ty++)
        for (uint32_t tx = 0; tx < td->d->tiles_x; tx++) {
            uint32_t tw = w - tx * PC_TILE_DIM, th = h - ty * PC_TILE_DIM;
            l->grid[ty * td->d->tiles_x + tx] = pc_tile_new_fill(4u, &c, tw, th);
        }
    CHECK(pc_hist_add_layer(td->h, l, 0, "add") == PC_OK);
}

static void run_perf(const char *name, double width, double hardness, double spacing,
                     bool soft_overlap, int events, double limit_ms)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    double *ms = (double *)malloc((size_t)events * sizeof *ms), total = 0.0;
    big_doc(&td, 4096, 4096);
    p.width = width;
    p.hardness = hardness;
    p.spacing = spacing;
    p.accum = soft_overlap ? PC_BRUSH_ACCUM_BUILDUP : PC_BRUSH_ACCUM_MAX;
    pc_brush_paint_color(pxc(20, 60, 200, 230), PC_BLEND_NORMAL, true, &src, &o);
    t = pc_txn_begin(td.d, "perf");
    {
        pc_brush_sample s0 = smp(200.5, 300.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
    }
    for (int i = 1; i <= events; i++) {
        /* a gentle curve: ~8 px per event, like a 1 kHz mouse moving fast */
        double u = (double)i;
        pc_brush_sample si = smp(200.5 + u * 7.5, 300.5 + u * 2.0 + 60.0 * sin(u * 0.01), 1);
        double t0 = pc_test_now();
        CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        ms[i - 1] = (pc_test_now() - t0) * 1000.0;
        total += ms[i - 1];
    }
    CHECK(pc_brush_end(b, NULL) == PC_OK);
    qsort(ms, (size_t)events, sizeof *ms, cmp_d);
    INFO("%s: %d events, median %.3f ms, p95 %.3f ms, max %.3f ms, mean %.3f ms, %zu dabs",
         name, events, ms[events / 2], ms[(events * 95) / 100], ms[events - 1],
         total / events, pc_brush_dab_count(b));
#if defined(PERF_ENFORCE)
    /* p95 (events that stamp dabs), 2x headroom for other jobs on the machine */
    CHECK(ms[(events * 95) / 100] < 2.0 * limit_ms);
#else
    (void)limit_ms;
#endif
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    CHECK(pc_hist_undo(td.h));
    free(ms);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_perf_300(void)
{
    int n = g_quick ? 200 : 480;
    run_perf("w300 h75 sp15 buildup", 300.0, 0.75, 0.15, true, n, 4.0);
    run_perf("w300 h100 sp15 max", 300.0, 1.0, 0.15, false, n, 4.0);
    run_perf("w300 h0 sp15 buildup", 300.0, 0.0, 0.15, true, n, 4.0);
    /* worst case spacing: a dab every 3 px, several per event */
    run_perf("w300 h75 sp1 buildup", 300.0, 0.75, 0.01, true, n / 2, 4.0);
}

int main(int argc, char **argv)
{
    size_t t0;
    pc_test_init(argc, argv);
    t0 = tiles_live();
    RUN(t_perf_300);
    CHECK(tiles_live() == t0);
    return pc_test_finish();
}
