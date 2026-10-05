/* fxm_align_object.c - the Align Object effect plugin (Effects > Object >
 * Align Object), an optional paint.c plugin (plugins/align_object/README.md,
 * docs/fx/align_object.md).
 *
 * Design after the Paint.NET plugin "Align Object" 1.0.1.9 by xod (helped by
 * MJW), reimplemented from its documented behavior only; no code was taken
 * from it (clean room, P-01 spirit).
 *
 * The object is the selected visible part of the active layer: pixels whose
 * alpha times the selection coverage is above 0 (env->sel_mask, ABI v1.1;
 * the selection rectangle when the host passes no mask), inside the
 * selection bounds clipped to the image (the whole image without a
 * selection). prepare() finds the object's bounding box and the offset that
 * puts that box at the chosen position of the selection bounds (or the
 * canvas): left / top edges meet, centers meet (the left or top margin is
 * the smaller one when the free space is odd), right / bottom edges meet.
 * Horizontal-only and vertical-only positions keep the other axis.
 * render() writes the object moved by that offset (its alpha weighted by
 * the coverage it had, so the selected part moves), transparent pixels where
 * the object was, and the source everywhere else; the host blends the
 * result through the selection, so pixels outside it never change and
 * moved pixels that land outside it are dropped.
 *
 * Nothing moves (dst = src) for "Original position", when the object is
 * already in place, when there is no object and when the object reaches the
 * edges of the target on every axis the position uses (filled or framed);
 * the last two report a notice to the user (fx_host.notice, ABI v1.2,
 * ADR-024; logged as a warning by older hosts).
 *
 * Test (FXP_F_PREVIEW_ONLY, ADR-024): object pixels with alpha below 64 are
 * shown opaque with their color, so faint pixels that count as part of the
 * object become visible. paint.c resets it for the final render, so it
 * never reaches the image.
 *
 * The position is an FXP_CHOICE with the "position-grid" hint
 * (fx_widgets.h): the paint.c dialog shows a 3 x 3 grid with axis-only
 * rows; other hosts show a drop-down of the same 16 positions.
 *
 * Output is a pure function of (params, src, env, pixel): any ROI split and
 * thread count give the same bytes. prepare() and render() poll
 * cancellation once per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments and the immutable state. Ownership: the effect
 * structs are static and stay valid until the library is unloaded; the
 * state is allocated through host->alloc and freed by release (X-17).
 */
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"
#include "fx/fx_widgets.h"

#define ALIGN_TEST_ALPHA 64u     /* Test shows object pixels below this alpha opaque */

typedef struct align_params {
    int32_t position;            /* FX_POS_*, FXP_CHOICE with the position-grid hint */
    int32_t test;                /* bool, FXP_F_PREVIEW_ONLY */
} align_params;

/* The FX_POS_* order of fx_widgets.h (fx_pos_name). */
static const char *const k_positions[] = {
    "Original position", "Top Left", "Middle Top", "Top Right", "Middle Left", "Center",
    "Middle Right", "Bottom Left", "Middle Bottom", "Bottom Right", "Left",
    "Middle Horizontal", "Right", "Top", "Middle Vertical", "Bottom", NULL
};

static const fx_prop k_props[] = {
    { "position", "Position", FXP_CHOICE, (uint32_t)offsetof(align_params, position), 0.0,
      (double)(FX_POS_COUNT - 1), 0.0, 0.0, k_positions, FX_WIDGET_POSITION_GRID, 0u, 0u, NULL },
    { "test", "Test", FXP_BOOL, (uint32_t)offsetof(align_params, test), 0.0, 1.0, 0.0, 0.0,
      NULL, FX_HINT_TIP "Reveal low opacity pixels", 0u, FXP_F_PREVIEW_ONLY, NULL },
};

typedef struct align_state {
    fx_rect obj;                 /* object bounds; w = 0 when unknown or none */
    int32_t dx, dy;              /* offset of the object */
    int32_t move;                /* nonzero: render moves the object */
} align_state;

/* ---- helpers ---------------------------------------------------------------- */
/* The selection mask of env when the host passed one (ABI v1.1), else NULL. */
static const fx_img *sel_mask_of(const fx_env *env)
{
    const fx_img *m;
    if (env == NULL || env->size < offsetof(fx_env, sel_mask) + sizeof env->sel_mask) return NULL;
    m = env->sel_mask;
    if (m == NULL || m->px == NULL || m->chans != 1 || m->r.w <= 0 || m->r.h <= 0) return NULL;
    return m;
}

static int in_rect(fx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

/* Selection coverage of pixel (x, y), 0..255. */
static uint32_t coverage(const fx_env *env, const fx_img *mask, int32_t x, int32_t y)
{
    if (mask != NULL) return in_rect(mask->r, x, y) ? fx_row8(mask, y)[x] : 0u;
    return in_rect(env->sel, x, y) ? 255u : 0u;
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w, y1 = (int64_t)a.y + a.h;
    fx_rect r = { 0, 0, 0, 0 };
    if ((int64_t)b.x + b.w < x1) x1 = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < y1) y1 = (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0) return r;
    r.x = (int32_t)x0;
    r.y = (int32_t)y0;
    r.w = (int32_t)(x1 - x0);
    r.h = (int32_t)(y1 - y0);
    return r;
}

/* Offset along one axis that puts [o0, o0 + on) at part (FX_POS_AXIS_*) of
 * [t0, t0 + tn). The object lies inside the target, so tn >= on. */
static int32_t axis_offset(int32_t part, int32_t o0, int32_t on, int32_t t0, int32_t tn)
{
    int64_t want;
    switch (part) {
    case FX_POS_AXIS_START: want = t0; break;
    case FX_POS_AXIS_MID:   want = (int64_t)t0 + ((int64_t)tn - on) / 2; break;
    case FX_POS_AXIS_END:   want = (int64_t)t0 + tn - on; break;
    default:                return 0;
    }
    return (int32_t)(want - o0);
}

/* A message for the user (ABI v1.2); hosts without notices get a log line. */
static void report(const fx_host *host, const void *job, const char *msg)
{
    if (host == NULL) return;
    if (host->size >= offsetof(fx_host, notice) + sizeof host->notice && host->notice != NULL)
        host->notice(job, msg);
    else if (host->log != NULL)
        host->log(1, msg);
}

/* ---- prepare ---------------------------------------------------------------- */
static void align_release(void *state, const fx_host *host)
{
    if (state != NULL && host != NULL && host->free != NULL) host->free(state);
}

static int align_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const align_params *p = (const align_params *)params;
    const fx_img *mask = sel_mask_of(env);
    int32_t pos = p->position, hpart, vpart, x, y;
    int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    int has_sel, full_h, full_v;
    align_state *s;
    fx_rect t;
    *state = NULL;
    if (host == NULL || host->alloc == NULL) return FX_ERROR;
    s = (align_state *)host->alloc(sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->obj.x = s->obj.y = s->obj.w = s->obj.h = 0;
    s->dx = s->dy = 0;
    s->move = 0;
    *state = s;
    fx_pos_axes(pos, &hpart, &vpart);
    if (hpart == FX_POS_AXIS_KEEP && vpart == FX_POS_AXIS_KEEP) return FX_OK;   /* original */
    t = rect_isect(env->sel, src->r);
    if (t.w <= 0 || t.h <= 0) return FX_OK;
    has_sel = mask != NULL || env->sel.x != 0 || env->sel.y != 0 || env->sel.w != env->doc_w ||
              env->sel.h != env->doc_h;
    /* the object's bounding box */
    for (y = t.y; y < t.y + t.h; y++) {
        const fx_px *row = fx_row(src, y);
        int32_t first = INT32_MAX, last = INT32_MIN;
        FX_CHECK_CANCEL(host, job);
        for (x = t.x; x < t.x + t.w; x++)
            if (row[x].a != 0u && fx_mul255(row[x].a, coverage(env, mask, x, y)) != 0u) {
                if (first == INT32_MAX) first = x;
                last = x;
            }
        if (first == INT32_MAX) continue;
        if (first < bx0) bx0 = first;
        if (last > bx1) bx1 = last;
        if (y < by0) by0 = y;
        by1 = y;
    }
    if (bx1 < bx0) {
        report(host, job, has_sel ? "There is no object in the selection, so nothing was "
                                    "aligned. Align Object moves the visible pixels of a "
                                    "layer that has transparency around them."
                                  : "There is no object on the canvas, so nothing was "
                                    "aligned. Align Object moves the visible pixels of a "
                                    "layer that has transparency around them.");
        return FX_OK;
    }
    s->obj.x = bx0;
    s->obj.y = by0;
    s->obj.w = bx1 - bx0 + 1;
    s->obj.h = by1 - by0 + 1;
    full_h = s->obj.w == t.w;
    full_v = s->obj.h == t.h;
    /* filled or framed on every axis the position uses: nothing can move */
    if ((hpart == FX_POS_AXIS_KEEP || full_h) && (vpart == FX_POS_AXIS_KEEP || full_v)) {
        const char *msg;
        if (full_h && full_v)
            msg = has_sel ? "The selection is filled or framed: the object reaches all of its "
                            "edges, so there is no room to align it. Turn on Test to reveal "
                            "faint pixels that count as part of the object."
                          : "The canvas is filled or framed: the object reaches all of its "
                            "edges, so there is no room to align it. Turn on Test to reveal "
                            "faint pixels that count as part of the object.";
        else if (full_h)
            msg = has_sel ? "The object spans the full width of the selection, so it cannot "
                            "move sideways. Turn on Test to reveal faint pixels that count as "
                            "part of the object."
                          : "The object spans the full width of the canvas, so it cannot move "
                            "sideways. Turn on Test to reveal faint pixels that count as part "
                            "of the object.";
        else
            msg = has_sel ? "The object spans the full height of the selection, so it cannot "
                            "move up or down. Turn on Test to reveal faint pixels that count "
                            "as part of the object."
                          : "The object spans the full height of the canvas, so it cannot move "
                            "up or down. Turn on Test to reveal faint pixels that count as "
                            "part of the object.";
        report(host, job, msg);
        return FX_OK;
    }
    s->dx = axis_offset(hpart, s->obj.x, s->obj.w, t.x, t.w);
    s->dy = axis_offset(vpart, s->obj.y, s->obj.h, t.y, t.h);
    s->move = s->dx != 0 || s->dy != 0;
    return FX_OK;
}

/* ---- render ----------------------------------------------------------------- */
static int align_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const align_params *p = (const align_params *)params;
    const align_state *s = (const align_state *)state;
    const fx_img *mask = sel_mask_of(env);
    const fx_px clear = { 0, 0, 0, 0 };
    int test = p->test != 0;
    int32_t x, y;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        int32_t sy = y - s->dy;
        const fx_px *orow = s->move && sy >= s->obj.y && sy - s->obj.y < s->obj.h
                                ? fx_row(src, sy) : NULL;
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px o = srow[x];
            if (!s->move) {
                if (test && o.a != 0u && o.a < ALIGN_TEST_ALPHA &&
                    coverage(env, mask, x, y) != 0u)
                    o.a = 255u;
            } else {
                int32_t sx = x - s->dx;
                uint32_t a = 0u;
                if (orow != NULL && sx >= s->obj.x && sx - s->obj.x < s->obj.w &&
                    orow[sx].a != 0u)
                    a = fx_mul255(orow[sx].a, coverage(env, mask, sx, sy));
                if (a != 0u) {
                    o = orow[sx];                     /* the moved object */
                    o.a = (uint8_t)(test && a < ALIGN_TEST_ALPHA ? 255u : a);
                } else if (o.a != 0u && coverage(env, mask, x, y) != 0u) {
                    o = clear;                        /* where the object was */
                }
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_align = {
    (uint32_t)sizeof(fx_effect), "org.paintc.object.align", "Effects/Object/Align Object",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(align_params), 0u,
    NULL, align_prepare, align_release, align_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c, after the Align Object plugin by xod and MJW";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Align Object; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_align) >= 0 ? 1 : 0;
}
