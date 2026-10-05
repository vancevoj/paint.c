/* doc_ants.c - marching ants outline of an open image: traced on the main
 * thread for simple selections, on a worker for complex ones, kept as a
 * prepared gfx_ants for drawing (lane TOOLS, wave 4 item 28; see
 * doc_ants.h and app_doc.h). */
#include "doc_ants.h"
#include "app_internal.h"

#include <stdlib.h>
#include <string.h>

#define LOD_MIN_POINTS 100000u          /* outlines this long get occupancy levels */
#define CACHE_KEY      "tools.ants_draw"

static uint32_t g_sync_tiles = DOC_ANTS_SYNC_TILES;
static uint64_t g_sync_edges = DOC_ANTS_SYNC_EDGES;

void app_doc_ants_set_sync_tiles(uint32_t n) { g_sync_tiles = n; }
void app_doc_ants_set_sync_edges(uint64_t n) { g_sync_edges = n; }

typedef struct ants_job ants_job;

struct app_doc_ants_rt {
    app      *a;                 /* borrowed */
    app_doc  *d;                 /* the owning document */
    gfx_ants *sel;               /* outline of the selection at sel_gen (owned), NULL = none */
    uint64_t  sel_gen;
    bool      sel_valid;         /* sel describes d's selection at sel_gen */
    gfx_ants *prev;              /* tool preview (owned) */
    bool      has_prev;
    uint64_t  prev_gen;          /* sel_gen when the preview was set */
    uint64_t  version;
    ants_job *job;               /* running background trace (owned by its task) */
    pc_poly   empty;
    uint64_t  complex_gen;       /* app_doc_sel_complex cache */
    bool      complex_known, complex;
};

/* A background trace of one selection state. The worker reads only snap
 * and cancel and writes only out, st and done. */
struct ants_job {
    app_doc_ants_rt *rt;         /* NULL once the document is gone */
    pc_sel_snap      snap;       /* owned (retained tiles) */
    uint64_t         gen;
    SDL_AtomicInt    cancel;
    SDL_AtomicInt    done;
    gfx_ants        *out;        /* owned until installed */
    pc_status        st;
};

/* ---- life time ----------------------------------------------------------------------- */
app_doc_ants_rt *app_doc_ants_rt_create(app *a, app_doc *d)
{
    app_doc_ants_rt *rt = (app_doc_ants_rt *)calloc(1u, sizeof *rt);
    if (!rt) return NULL;
    rt->a = a;
    rt->d = d;
    pc_poly_init(&rt->empty);
    return rt;
}

void app_doc_ants_rt_free(app_doc_ants_rt *rt)
{
    if (!rt) return;
    if (rt->job) {
        /* the done callback frees the detached job */
        SDL_SetAtomicInt(&rt->job->cancel, 1);
        rt->job->rt = NULL;
        rt->job = NULL;
    }
    gfx_ants_free(rt->sel);
    gfx_ants_free(rt->prev);
    pc_poly_free(&rt->empty);
    free(rt);
}

static void job_free(ants_job *j)
{
    if (!j) return;
    gfx_ants_free(j->out);
    pc_sel_snap_free(&j->snap);
    free(j);
}

/* ---- tracing ------------------------------------------------------------------------- */
/* Crossings of the 50 % level between neighboring pixels of one tile's
 * vw x vh area, rows and columns (about the length of the outline inside). */
static uint32_t tile_edges(const uint8_t *px, uint32_t vw, uint32_t vh)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < vh; y++) {
        const uint8_t *row = px + (size_t)y * PC_TILE_DIM;
        const uint8_t *up = y ? row - PC_TILE_DIM : NULL;
        for (uint32_t x = 0; x < vw; x++) {
            bool in = row[x] >= 128u;
            if (x && in != (row[x - 1u] >= 128u)) n++;
            if (up && in != (up[x] >= 128u)) n++;
        }
    }
    return n;
}

/* Whether tracing d's selection outline is too slow for a frame: more
 * partially selected tiles than tile_limit and more than edge_limit
 * crossings inside them (counting stops at the answer; fully selected
 * tiles share pointers, so one check covers a whole Select All). */
static bool sel_is_complex(const pc_doc *d, uint32_t tile_limit, uint64_t edge_limit)
{
    static const uint8_t full_row[PC_TILE_DIM] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
    const pc_tile *last_full = NULL;
    uint32_t n = 0;
    uint64_t edges = 0;
    if (!d->sel_active || !d->sel_grid) return false;
    for (uint32_t ty = 0; ty < d->tiles_y; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
            const pc_tile *t = d->sel_grid[(size_t)ty * d->tiles_x + tx];
            uint32_t vw = d->w - tx * PC_TILE_DIM, vh = d->h - ty * PC_TILE_DIM;
            bool full = true;
            if (!t || t == last_full) continue;
            if (vw > PC_TILE_DIM) vw = PC_TILE_DIM;
            if (vh > PC_TILE_DIM) vh = PC_TILE_DIM;
            for (uint32_t y = 0; y < vh && full; y++)
                full = memcmp(t->data + (size_t)y * PC_TILE_DIM, full_row, vw) == 0;
            if (full) {
                last_full = t;
                continue;
            }
            n++;
            edges += tile_edges(t->data, vw, vh);
            if (n > tile_limit && edges > edge_limit) return true;
        }
    return false;
}

static const uint8_t *snap_block(void *ud, int32_t bx, int32_t by, uint8_t *scratch,
                                 uint8_t *uniform)
{
    ants_job *j = (ants_job *)ud;
    const pc_tile *t;
    (void)scratch;
    *uniform = 0u;
    /* a cancelled trace reads nothing more, so it ends quickly */
    if (SDL_GetAtomicInt(&j->cancel) != 0) return NULL;
    if (!j->snap.active || !j->snap.grid || bx < 0 || by < 0 ||
        (uint32_t)bx >= j->snap.tiles_x || (uint32_t)by >= j->snap.tiles_y)
        return NULL;
    t = j->snap.grid[(size_t)by * j->snap.tiles_x + (size_t)bx];
    return t ? t->data : NULL;
}

static uint32_t create_flags(const pc_poly *p)
{
    return p->n_pts >= LOD_MIN_POINTS ? GFX_ANTS_WITH_LOD : 0u;
}

/* Worker: trace the snapshot and prepare the outline. */
static void job_work(void *ud)
{
    ants_job *j = (ants_job *)ud;
    pc_cov_field f;
    pc_poly p;
    pc_poly_init(&p);
    f.area = pc_rect_make(0, 0, (int32_t)j->snap.w, (int32_t)j->snap.h);
    f.block = snap_block;
    f.ud = j;
    j->st = pc_contour_field(&f, 0.0, &p);
    if (j->st == PC_OK && SDL_GetAtomicInt(&j->cancel) != 0) j->st = PC_ERR_CANCELLED;
    if (j->st == PC_OK) j->st = gfx_ants_create(&p, create_flags(&p), &j->out);
    pc_poly_free(&p);
    SDL_SetAtomicInt(&j->done, 1);
}

static void install_sel(app_doc_ants_rt *rt, gfx_ants *g, uint64_t gen)
{
    gfx_ants_free(rt->sel);
    rt->sel = g;
    rt->sel_gen = gen;
    rt->sel_valid = true;
    rt->version++;
}

/* Main thread: a trace finished (or was detached). */
static void job_done(app *a, void *ud)
{
    ants_job *j = (ants_job *)ud;
    app_doc_ants_rt *rt = j->rt;
    if (rt) {
        if (rt->job == j) rt->job = NULL;
        if (j->st == PC_OK && j->out && !rt->sel_valid && SDL_GetAtomicInt(&j->cancel) == 0 &&
            rt->d->doc->sel_gen == j->gen && pc_sel_is_active(rt->d->doc)) {
            install_sel(rt, j->out, j->gen);
            j->out = NULL;
        }
    }
    job_free(j);
    app_request_frame(a);
}

static void trace_now(app_doc_ants_rt *rt, uint64_t gen)
{
    pc_poly p;
    gfx_ants *g = NULL;
    pc_poly_init(&p);
    if (pc_sel_contour(rt->d->doc, 0.0, &p) != PC_OK ||
        gfx_ants_create(&p, create_flags(&p), &g) != PC_OK)
        g = NULL;                    /* no outline rather than a wrong one */
    pc_poly_free(&p);
    install_sel(rt, g, gen);
}

static void start_job(app_doc_ants_rt *rt, uint64_t gen)
{
    ants_job *j = (ants_job *)calloc(1u, sizeof *j);
    if (j && pc_sel_snap_take(rt->d->doc, &j->snap) == PC_OK) {
        j->rt = rt;
        j->gen = gen;
        j->st = PC_ERR_STATE;
        if (app_task(rt->a, job_work, job_done, j)) {
            rt->job = j;
            return;
        }
    }
    job_free(j);
    trace_now(rt, gen);               /* no worker: trace here */
}

/* Bring the displayed outline up to date with the selection. */
static void update(app_doc_ants_rt *rt)
{
    const pc_doc *doc = rt->d->doc;
    uint64_t gen = doc->sel_gen;
    if (rt->has_prev && rt->prev_gen != gen) {
        gfx_ants_free(rt->prev);
        rt->prev = NULL;
        rt->has_prev = false;
        rt->version++;
    }
    if (rt->sel_valid && rt->sel_gen == gen) return;
    if (rt->sel_valid) {
        gfx_ants_free(rt->sel);
        rt->sel = NULL;
        rt->sel_valid = false;
        rt->version++;
    }
    if (!pc_sel_is_active(doc)) {
        install_sel(rt, NULL, gen);
        return;
    }
    if (rt->job) {
        /* one trace at a time: an outdated one is cancelled, the next
         * update after it ends starts the current one */
        if (rt->job->gen != gen) SDL_SetAtomicInt(&rt->job->cancel, 1);
        return;
    }
    if (!rt->a || !app_doc_sel_complex(rt->d)) trace_now(rt, gen);
    else start_job(rt, gen);
}

static const gfx_ants *shown(const app_doc_ants_rt *rt)
{
    if (rt->has_prev) return rt->prev;
    return rt->sel_valid ? rt->sel : NULL;
}

/* ---- app_doc.h ------------------------------------------------------------------------ */
const pc_poly *app_doc_ants(app_doc *d)
{
    static const pc_poly none = { 0 };
    const gfx_ants *g;
    if (!d) return NULL;
    if (!d->ants_rt) return &none;
    update(d->ants_rt);
    g = shown(d->ants_rt);
    return g ? gfx_ants_poly(g) : &d->ants_rt->empty;
}

const gfx_ants *app_doc_ants_geom(app_doc *d)
{
    if (!d || !d->ants_rt) return NULL;
    update(d->ants_rt);
    return shown(d->ants_rt);
}

uint64_t app_doc_ants_version(app_doc *d)
{
    if (!d || !d->ants_rt) return 0u;
    update(d->ants_rt);
    return d->ants_rt->version;
}

bool app_doc_ants_pending(app_doc *d)
{
    app_doc_ants_rt *rt = d ? d->ants_rt : NULL;
    if (!rt) return false;
    update(rt);
    return !rt->has_prev && !rt->sel_valid;
}

bool app_doc_ants_is_preview(app_doc *d)
{
    if (!d || !d->ants_rt) return false;
    update(d->ants_rt);
    return d->ants_rt->has_prev;
}

static void set_preview(app_doc_ants_rt *rt, gfx_ants *g)
{
    gfx_ants_free(rt->prev);
    rt->prev = g;
    rt->has_prev = true;
    rt->prev_gen = rt->d->doc->sel_gen;
    rt->version++;
}

static void drop_preview(app_doc_ants_rt *rt)
{
    if (!rt->has_prev) return;
    gfx_ants_free(rt->prev);
    rt->prev = NULL;
    rt->has_prev = false;
    rt->version++;
}

pc_status app_doc_ants_preview(app_doc *d, const pc_poly *p)
{
    pc_poly copy;
    gfx_ants *g = NULL;
    pc_status st;
    if (!d) return PC_ERR_ARG;
    if (!d->ants_rt) return PC_ERR_STATE;
    if (!p) {
        drop_preview(d->ants_rt);
        return PC_OK;
    }
    pc_poly_init(&copy);
    st = pc_poly_append(&copy, p, NULL);
    if (st == PC_OK) st = gfx_ants_create(&copy, create_flags(&copy), &g);
    pc_poly_free(&copy);
    if (st != PC_OK) {
        drop_preview(d->ants_rt);       /* falls back to the selection outline */
        return st;
    }
    set_preview(d->ants_rt, g);
    return PC_OK;
}

void app_doc_ants_preview_take(app_doc *d, gfx_ants *g)
{
    if (!d || !d->ants_rt) {
        gfx_ants_free(g);
        return;
    }
    if (!g) drop_preview(d->ants_rt);
    else set_preview(d->ants_rt, g);
}

bool app_doc_sel_complex(app_doc *d)
{
    app_doc_ants_rt *rt = d ? d->ants_rt : NULL;
    if (!d || !pc_sel_is_active(d->doc)) return false;
    if (!rt) return sel_is_complex(d->doc, g_sync_tiles, g_sync_edges);
    if (!rt->complex_known || rt->complex_gen != d->doc->sel_gen) {
        rt->complex = sel_is_complex(d->doc, g_sync_tiles, g_sync_edges);
        rt->complex_gen = d->doc->sel_gen;
        rt->complex_known = true;
    }
    return rt->complex;
}

pc_status app_doc_sel_outline(app_doc *d, pc_poly *out)
{
    app_doc_ants_rt *rt;
    uint64_t gen;
    if (!d || !out) return PC_ERR_ARG;
    rt = d->ants_rt;
    if (!pc_sel_is_active(d->doc)) return PC_OK;
    if (!rt) return pc_sel_contour(d->doc, 0.0, out);
    update(rt);
    gen = d->doc->sel_gen;
    if (rt->job && rt->job->gen == gen) {
        /* a trace of this very state is queued or running: wait for its
         * task (pal_task_wait runs a task that has not started yet here,
         * so busy workers cannot hold this up) */
        ants_job *j = rt->job;
        bool waited = false;
        for (int32_t i = 0; i < rt->a->ntasks && !waited; i++)
            if (rt->a->tasks[i].ud == j) {
                pal_task_wait(rt->a->tasks[i].task);
                waited = true;
            }
        while (SDL_GetAtomicInt(&j->done) == 0) SDL_Delay(1);
        if (j->st == PC_OK && j->out && !rt->sel_valid) {
            install_sel(rt, j->out, gen);
            j->out = NULL;
        }
    }
    if (rt->sel_valid && rt->sel_gen == gen && rt->sel)
        return pc_poly_append(out, gfx_ants_poly(rt->sel), NULL);
    return pc_sel_contour(d->doc, 0.0, out);
}

/* ---- drawing ---------------------------------------------------------------------------- */
typedef struct ants_draw {
    gfx_ants_cache *cache;
    gfx_ants_info   last;
    bool            has_last;
} ants_draw;

static void draw_free(void *p)
{
    ants_draw *x = (ants_draw *)p;
    if (!x) return;
    gfx_ants_cache_destroy(x->cache);
    free(x);
}

static ants_draw *draw_state(app *a)
{
    ants_draw *x = (ants_draw *)app_ext_get(a, CACHE_KEY);
    if (x) return x;
    x = (ants_draw *)calloc(1u, sizeof *x);
    if (!x) return NULL;
    x->cache = gfx_ants_cache_create();
    if (!x->cache || !app_ext_set(a, CACHE_KEY, x, draw_free)) {
        gfx_ants_cache_destroy(x->cache);
        free(x);
        return NULL;
    }
    return x;
}

void app_doc_ants_draw(app *a, app_doc *d, const gfx_view *v, double phase, double dash,
                       pc_rect clip)
{
    const gfx_ants *g = app_doc_ants_geom(d);
    ants_draw *x;
    if (!a || !a->ren || !g) return;
    x = draw_state(a);
    if (!x) return;
    gfx_ants_draw(a->ren, x->cache, v, g, phase, dash, clip, app_par(a), &x->last);
    x->has_last = true;
}

bool app_doc_ants_last_draw(app *a, gfx_ants_info *out)
{
    ants_draw *x = a ? (ants_draw *)app_ext_get(a, CACHE_KEY) : NULL;
    if (!x || !x->has_last) return false;
    if (out) *out = x->last;
    return true;
}
