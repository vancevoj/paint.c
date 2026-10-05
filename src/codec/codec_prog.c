/* codec_prog.c - encode progress and cancellation (ADR-023): the throttled
 * reporter of codec_prog.h and pc_codec_save_ex. */
#include "codec_prog.h"

#include <math.h>

static pc_status emit(cp_prog *g, double v)
{
    int32_t k;
    if (g->cancelled) return PC_ERR_CANCELLED;
    if (!g->cb || !g->cb->report) return PC_OK;
    if (!(v >= 0.0)) v = 0.0;
    if (v > 1.0) v = 1.0;
    k = (int32_t)floor(v * 1000.0 + 1e-9);      /* k / 1000.0 maps back to k */
    if (k <= g->last) return PC_OK;
    g->last = k;
    if (!g->cb->report(g->cb->ud, (double)k / 1000.0)) {
        g->cancelled = true;
        return PC_ERR_CANCELLED;
    }
    return PC_OK;
}

void cp_init(cp_prog *g, const pc_codec_progress *cb)
{
    if (!g) return;
    g->cb = cb;
    g->lo = 0.0;
    g->hi = 1.0;
    g->done = 0;
    g->total = 0;
    g->last = -1;
    g->cancelled = false;
}

pc_status cp_phase(cp_prog *g, double lo, double hi, uint64_t total)
{
    double floor_v;
    if (!g) return PC_OK;
    floor_v = g->last > 0 ? (double)g->last / 1000.0 : 0.0;
    if (!(lo >= 0.0)) lo = 0.0;
    if (lo > 1.0) lo = 1.0;
    if (!(hi >= lo)) hi = lo;
    if (hi > 1.0) hi = 1.0;
    if (lo < floor_v) lo = floor_v;
    if (hi < lo) hi = lo;
    g->lo = lo;
    g->hi = hi;
    g->done = 0;
    g->total = total;
    return emit(g, lo);
}

pc_status cp_set(cp_prog *g, uint64_t done)
{
    double f;
    if (!g) return PC_OK;
    if (g->cancelled) return PC_ERR_CANCELLED;
    g->done = done;
    if (g->total == 0u) return PC_OK;
    f = done >= g->total ? 1.0 : (double)done / (double)g->total;
    return emit(g, g->lo + (g->hi - g->lo) * f);
}

pc_status cp_add(cp_prog *g, uint64_t units)
{
    if (!g) return PC_OK;
    return cp_set(g, g->done + units);
}

pc_status cp_finish(cp_prog *g)
{
    if (!g) return PC_OK;
    return emit(g, 1.0);
}

bool cp_cancelled(const cp_prog *g) { return g && g->cancelled; }

/* pc_codec_save_ex hands the codec this relay, so the 0 and 1 it reports
 * itself and the codec's reports share one throttle and one cancel flag. */
static bool relay(void *ud, double v)
{
    return emit((cp_prog *)ud, v) == PC_OK;
}

pc_status pc_codec_save_ex(const pc_codec *c, const pc_doc *d, const pc_image_meta *meta,
                           const void *params, const pc_par *par,
                           const pc_codec_progress *prog, pc_buf *out)
{
    cp_prog g;
    pc_codec_progress fwd;
    pc_status st;
    size_t n0;
    if (!c || !d || !out) return PC_ERR_ARG;
    if (!(c->flags & PC_CODEC_SAVE) || (!c->save && !c->save_ex)) return PC_ERR_UNSUPPORTED;
    n0 = out->n;
    cp_init(&g, prog);
    st = cp_phase(&g, 0.0, 1.0, 0u);
    if (st != PC_OK) return st;
    fwd.report = relay;
    fwd.ud = &g;
    if (c->save_ex) st = c->save_ex(d, meta, params, par, prog ? &fwd : NULL, out);
    else st = c->save(d, meta, params, par, out);
    if (st == PC_OK && g.cancelled) st = PC_ERR_CANCELLED;   /* cancelled at the last report */
    if (st == PC_OK) st = cp_finish(&g);
    if (st != PC_OK) out->n = n0;
    return st;
}
