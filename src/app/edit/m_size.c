/* m_size.c - lane M: size model of the Resize and Canvas Size dialogs and
 * the undoable resolution change (m_size.h). */
#include "m_size.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double clampd(double v, double lo, double hi)
{
    if (!(v >= lo)) return lo;           /* also catches NaN */
    if (v > hi) return hi;
    return v;
}

static void apply_pct(m_size *s)
{
    s->w = (double)s->ow * s->pct / 100.0;
    s->h = (double)s->oh * s->pct / 100.0;
}

void m_size_init(m_size *s, uint32_t w, uint32_t h, double dpi, bool keep, bool cm)
{
    memset(s, 0, sizeof *s);
    s->ow = w ? w : 1u;
    s->oh = h ? h : 1u;
    s->by = 1;
    s->pct = 100.0;
    s->keep = keep;
    s->w = (double)s->ow;
    s->h = (double)s->oh;
    s->dpi = dpi > 0.0 ? clampd(dpi, M_SIZE_MIN_RES, M_SIZE_MAX_RES * 2.54) : 96.0;
    s->res_unit = cm ? 1 : 0;
    s->print_unit = cm ? 1 : 0;
}

void m_size_set_by(m_size *s, int by)
{
    s->by = by ? 1 : 0;
    if (s->by == 0) apply_pct(s);
}

void m_size_set_pct(m_size *s, double pct)
{
    s->pct = clampd(pct, 0.0, M_SIZE_MAX_PCT);
    if (s->by == 0) apply_pct(s);
}

void m_size_set_keep(m_size *s, bool keep)
{
    s->keep = keep;
    if (keep) s->h = s->w * (double)s->oh / (double)s->ow;
}

void m_size_set_w(m_size *s, double w)
{
    s->w = clampd(w, 0.0, (double)M_SIZE_MAX_EDIT);
    if (s->keep) s->h = s->w * (double)s->oh / (double)s->ow;
}

void m_size_set_h(m_size *s, double h)
{
    s->h = clampd(h, 0.0, (double)M_SIZE_MAX_EDIT);
    if (s->keep) s->w = s->h * (double)s->ow / (double)s->oh;
}

double m_size_res(const m_size *s) { return s->res_unit == 1 ? s->dpi / 2.54 : s->dpi; }

void m_size_set_res(m_size *s, double res)
{
    res = clampd(res, M_SIZE_MIN_RES, M_SIZE_MAX_RES);
    s->dpi = s->res_unit == 1 ? res * 2.54 : res;
}

void m_size_set_res_unit(m_size *s, int unit) { s->res_unit = unit ? 1 : 0; }

double m_size_print(const m_size *s, bool height)
{
    double inches = (height ? s->h : s->w) / s->dpi;
    return s->print_unit == 1 ? inches * 2.54 : inches;
}

void m_size_set_print(m_size *s, bool height, double v)
{
    double inches = s->print_unit == 1 ? v / 2.54 : v;
    double px = inches * s->dpi;
    if (height) m_size_set_h(s, px);
    else m_size_set_w(s, px);
}

int32_t m_size_px(const m_size *s, bool height)
{
    double v = floor((height ? s->h : s->w) + 0.5);
    if (!(v >= 0.0)) return 0;
    if (v > (double)INT32_MAX) return INT32_MAX;
    return (int32_t)v;
}

bool m_size_valid(const m_size *s, char *why, size_t cap)
{
    int32_t w = m_size_px(s, false), h = m_size_px(s, true);
    const char *msg = NULL;
    if (s->by == 0 && !(s->pct > 0.0)) msg = "The percentage must be greater than zero.";
    else if (w < 1 || h < 1) msg = "The width and height must be at least 1 pixel.";
    else if ((uint32_t)w > PC_MAX_DIM || (uint32_t)h > PC_MAX_DIM)
        msg = "paint.c supports at most 65535 pixels per side.";
    if (why && cap) {
        if (msg) snprintf(why, cap, "%s", msg);
        else why[0] = '\0';
    }
    return msg == NULL;
}

uint64_t m_size_bytes(const m_size *s, uint32_t layers)
{
    uint64_t w = (uint64_t)m_size_px(s, false), h = (uint64_t)m_size_px(s, true);
    return w * h * 4u * (uint64_t)(layers ? layers : 1u);
}

void m_size_format_bytes(uint64_t bytes, char *out, size_t cap)
{
    static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int u = -1;
    if (bytes < 1024u) {
        snprintf(out, cap, "%u bytes", (unsigned)bytes);
        return;
    }
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, cap, "%.1f %s", v, units[u]);
}

/* ---- undoable resolution --------------------------------------------------------------- */
typedef struct dpi_payload {
    app_doc *d;          /* borrowed: the history belongs to this document */
    double   x, y;       /* the other state */
} dpi_payload;

static void dpi_swap(pc_doc *doc, void *p)
{
    dpi_payload *v = (dpi_payload *)p;
    double tx = v->d->meta.dpi_x, ty = v->d->meta.dpi_y;
    (void)doc;
    v->d->meta.dpi_x = v->x;
    v->d->meta.dpi_y = v->y;
    v->x = tx;
    v->y = ty;
}

static void dpi_destroy(void *p) { free(p); }

static size_t dpi_bytes(const void *p)
{
    (void)p;
    return sizeof(dpi_payload);
}

static const pc_hist_ops k_dpi_ops = { dpi_swap, dpi_destroy, dpi_bytes };

pc_status m_doc_set_dpi(app_doc *d, double dpi_x, double dpi_y, const char *label)
{
    dpi_payload *v;
    pc_hist_node *n;
    if (!d || !(dpi_x > 0.0) || !(dpi_y > 0.0)) return PC_ERR_ARG;
    if (d->doc->open_txns) return PC_ERR_STATE;
    if (fabs(d->meta.dpi_x - dpi_x) < 1e-9 && fabs(d->meta.dpi_y - dpi_y) < 1e-9)
        return PC_ERR_STATE;
    v = (dpi_payload *)malloc(sizeof *v);
    n = pc_hist_node_new(label ? label : "Resolution");
    if (!v || !n) {
        free(v);
        pc_hist_node_free_unlinked(n);
        return PC_ERR_NOMEM;
    }
    v->d = d;
    v->x = dpi_x;
    v->y = dpi_y;
    dpi_swap(d->doc, v);                /* apply == swap */
    pc_hist_link(d->hist, n, &k_dpi_ops, v);
    return PC_OK;
}
