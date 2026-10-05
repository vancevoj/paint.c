/* sel_marquee.c - Rectangle, Ellipse and Lasso Select drag logic (see
 * sel_marquee.h). The press / move / release state machine, the "click
 * deselects" truth table and the shape rules (Shift square from the
 * shorter side, Shift circle on the click-pointer diameter, fixed ratio)
 * follow the Paint.NET 3.36 SelectionTool, RectangleSelectTool and
 * EllipseSelectTool (MIT, docs/notice/a.md). Main thread. */
#include "sel_marquee.h"
#include "paint_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUICK_NS      50000000ull   /* 3.36: a drag under 50 ms ... */
#define QUICK_PX      8.0f          /* ... that moved less than this is a click */
#define LASSO_MAX_PTS (1u << 22)

/* ---- options persistence ---------------------------------------------------------- */
static void load_opts(app *a, sel_marquee *m)
{
    const app_settings *s = app_settings_of(a);
    if (m->opts_loaded) return;
    m->opts_loaded = true;
    if (m->shape != SEL_SHAPE_RECT || !s) return;
    m->draw_mode = (int)app_settings_int(s, "tool.rect_select.draw_mode", SEL_DRAW_ANY);
    if (m->draw_mode < SEL_DRAW_ANY || m->draw_mode > SEL_DRAW_SIZE) m->draw_mode = SEL_DRAW_ANY;
    m->ratio_w = app_settings_double(s, "tool.rect_select.ratio_w", 4.0);
    m->ratio_h = app_settings_double(s, "tool.rect_select.ratio_h", 3.0);
    m->size_w = app_settings_double(s, "tool.rect_select.size_w", 400.0);
    m->size_h = app_settings_double(s, "tool.rect_select.size_h", 300.0);
    /* lane SHELL (V-UNITS-WHERE): until the user picks units, the fixed
     * size uses the View units. Lane TOOLS (wave 4 item 3): units count as
     * picked only when chosen in the toolbar (the _set marker) or given as
     * an explicit default on the Settings > Tools page; a value an earlier
     * version stored with every option change is ignored. */
    m->size_units_set = app_settings_get(s, "tool.rect_select.size_units") != NULL &&
                        (app_settings_bool(s, "tool.rect_select.size_units_set", false) ||
                         app_settings_int(s, "tooldef.rect_select.size_units", -1) >= 0);
    m->size_units = m->size_units_set
                        ? (int)app_settings_int(s, "tool.rect_select.size_units", APP_UNITS_PX)
                        : (int)app_get_units(a);
    if (!(m->ratio_w > 0.0) || m->ratio_w > 65535.0) m->ratio_w = 4.0;
    if (!(m->ratio_h > 0.0) || m->ratio_h > 65535.0) m->ratio_h = 3.0;
    if (!(m->size_w > 0.0) || m->size_w > 65535.0) m->size_w = 400.0;
    if (!(m->size_h > 0.0) || m->size_h > 65535.0) m->size_h = 300.0;
    if (m->size_units < APP_UNITS_PX || m->size_units > APP_UNITS_CM) m->size_units = APP_UNITS_PX;
}

static void store_opts(app *a, const sel_marquee *m)
{
    app_settings *s = app_settings_of(a);
    if (!s || m->shape != SEL_SHAPE_RECT) return;
    app_settings_set_int(s, "tool.rect_select.draw_mode", m->draw_mode);
    app_settings_set_double(s, "tool.rect_select.ratio_w", m->ratio_w);
    app_settings_set_double(s, "tool.rect_select.ratio_h", m->ratio_h);
    app_settings_set_double(s, "tool.rect_select.size_w", m->size_w);
    app_settings_set_double(s, "tool.rect_select.size_h", m->size_h);
    /* lane TOOLS: the units only when the user picked them */
    if (m->size_units_set) {
        app_settings_set_int(s, "tool.rect_select.size_units", m->size_units);
        app_settings_set_bool(s, "tool.rect_select.size_units_set", true);
    } else {
        (void)app_settings_remove(s, "tool.rect_select.size_units");
        (void)app_settings_remove(s, "tool.rect_select.size_units_set");
    }
}

/* The units of the fixed size: the picked ones, else the View units (read
 * every time, so a loaded tool follows View > Units). */
static int size_units(const app *a, const sel_marquee *m)
{
    int u = m->size_units_set ? m->size_units : (int)app_get_units(a);
    return u < APP_UNITS_PX || u > APP_UNITS_CM ? APP_UNITS_PX : u;
}

void sel_marquee_init(sel_marquee *m, sel_shape shape, const char *label)
{
    memset(m, 0, sizeof *m);
    m->shape = shape;
    m->label = label;
    m->draw_mode = SEL_DRAW_ANY;
    m->ratio_w = 4.0;
    m->ratio_h = 3.0;
    m->size_w = 400.0;
    m->size_h = 300.0;
    m->size_units = APP_UNITS_PX;
    pc_poly_init(&m->poly);
    pc_poly_init(&m->preview);
}

void sel_marquee_fini(app *a, sel_marquee *m)
{
    (void)a;
    free(m->pts);
    m->pts = NULL;
    m->n = m->cap = 0;
    pc_poly_free(&m->poly);
    pc_poly_free(&m->preview);
}

/* ---- trace ------------------------------------------------------------------------- */
static bool push_pt(sel_marquee *m, double x, double y)
{
    if (m->n == m->cap) {
        size_t nc = m->cap ? m->cap * 2u : 64u;
        pc_pt *np;
        if (nc > LASSO_MAX_PTS) return false;
        np = (pc_pt *)realloc(m->pts, nc * sizeof *np);
        if (!np) return false;
        m->pts = np;
        m->cap = nc;
    }
    m->pts[m->n++] = pc_pt_make(x, y);
    return true;
}

static void translate_pts(sel_marquee *m, double dx, double dy)
{
    for (size_t i = 0; i < m->n; i++) {
        m->pts[i].x = sel_clampd(m->pts[i].x + dx);
        m->pts[i].y = sel_clampd(m->pts[i].y + dy);
    }
}

/* ---- shapes ------------------------------------------------------------------------- */
static double doc_dpi(const app_doc *d, bool y)
{
    double v = y ? d->meta.dpi_y : d->meta.dpi_x;
    return v > 0.0 ? v : 96.0;
}

static double to_px(double v, int units, double dpi)
{
    if (units == APP_UNITS_IN) return v * dpi;
    if (units == APP_UNITS_CM) return v * dpi / 2.54;
    return v;
}

static double clampv(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The rectangle in pixel corners, (x0, y0) - (x1, y1) with x0 <= x1. */
static void rect_shape(app *a, sel_marquee *m, const app_doc *d, double *rx0, double *ry0,
                       double *rx1, double *ry1)
{
    pc_pt pa = m->pts[0], pb = m->pts[m->n - 1u];
    double W = (double)d->doc->w, H = (double)d->doc->h;
    double ax = sel_round(pa.x), ay = sel_round(pa.y), bx, by, x0, y0, x1, y1;
    uint32_t mods = ui_mods(a->ui);
    switch (m->draw_mode) {
    case SEL_DRAW_SIZE: {
        double w = sel_round(to_px(m->size_w, size_units(a, m), doc_dpi(d, false)));
        double h = sel_round(to_px(m->size_h, size_units(a, m), doc_dpi(d, true)));
        if (w < 1.0) w = 1.0;
        if (h < 1.0) h = 1.0;
        /* the rectangle hangs from the pointer and stays inside the canvas
         * when it fits (docs: clamped to the edge) */
        x0 = sel_round(pb.x);
        y0 = sel_round(pb.y);
        if (w <= W) x0 = clampv(x0, 0.0, W - w);
        if (h <= H) y0 = clampv(y0, 0.0, H - h);
        x1 = x0 + w;
        y1 = y0 + h;
        break;
    }
    case SEL_DRAW_RATIO: {
        /* 3.36 FixedRatio: the side that is shorter relative to the ratio
         * decides; the pointer is clamped to the canvas */
        double aspect = m->ratio_w / m->ratio_h, dw, dh, sw, sh;
        bx = sel_round(clampv(pb.x, 0.0, W));
        by = sel_round(clampv(pb.y, 0.0, H));
        dw = bx - ax;
        dh = by - ay;
        sw = dw < 0.0 ? -1.0 : 1.0;
        sh = dh < 0.0 ? -1.0 : 1.0;
        if (fabs(dw) / m->ratio_w < fabs(dh) / m->ratio_h) {
            bx = ax + dw;
            by = ay + sh * sel_round(fabs(dw) / aspect);
        } else {
            bx = ax + sw * sel_round(fabs(dh) * aspect);
            by = ay + dh;
        }
        x0 = fmin(ax, bx);
        x1 = fmax(ax, bx);
        y0 = fmin(ay, by);
        y1 = fmax(ay, by);
        break;
    }
    default:
        bx = sel_round(pb.x);
        by = sel_round(pb.y);
        if (mods & UI_MOD_SHIFT) {
            /* 3.36 PointsToConstrainedRectangle: the shorter side, anchored
             * at the press and growing towards the pointer */
            double s = fmin(fabs(bx - ax), fabs(by - ay));
            bx = ax + (bx < ax ? -s : s);
            by = ay + (by < ay ? -s : s);
        }
        x0 = fmin(ax, bx);
        x1 = fmax(ax, bx);
        y0 = fmin(ay, by);
        y1 = fmax(ay, by);
        break;
    }
    *rx0 = x0;
    *ry0 = y0;
    *rx1 = x1;
    *ry1 = y1;
}

static bool to_rect(double x0, double y0, double x1, double y1, pc_rect *r)
{
    if (!(x1 - x0 >= 1.0) || !(y1 - y0 >= 1.0)) return false;
    *r = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
    return true;
}

/* Build m->poly (ellipse, lasso) or *r (rect). false when the shape is
 * empty. *bx0.. receive the shape bounds for the status bar. */
static bool build(app *a, sel_marquee *m, const app_doc *d, pc_rect *r, double *bx0,
                  double *by0, double *bx1, double *by1)
{
    pc_poly_clear(&m->poly);
    *r = pc_rect_make(0, 0, 0, 0);
    if (m->n == 0u) return false;
    if (m->shape == SEL_SHAPE_RECT) {
        rect_shape(a, m, d, bx0, by0, bx1, by1);
        return to_rect(*bx0, *by0, *bx1, *by1, r);
    }
    if (m->shape == SEL_SHAPE_ELLIPSE) {
        pc_pt pa = m->pts[0], pb = m->pts[m->n - 1u];
        double x0, y0, x1, y1;
        pc_path path;
        pc_status st;
        if (ui_mods(a->ui) & UI_MOD_SHIFT) {
            /* the click and the pointer span the circle's diameter (docs);
             * 3.36 truncates the bounding square to whole pixels */
            double cx = (pa.x + pb.x) * 0.5, cy = (pa.y + pb.y) * 0.5;
            double rad = hypot(pb.x - pa.x, pb.y - pa.y) * 0.5;
            x0 = floor(cx - rad);
            y0 = floor(cy - rad);
            x1 = x0 + floor(2.0 * rad);
            y1 = y0 + floor(2.0 * rad);
        } else {
            double ax = sel_round(pa.x), ay = sel_round(pa.y);
            double qx = sel_round(pb.x), qy = sel_round(pb.y);
            x0 = fmin(ax, qx);
            x1 = fmax(ax, qx);
            y0 = fmin(ay, qy);
            y1 = fmax(ay, qy);
        }
        *bx0 = x0;
        *by0 = y0;
        *bx1 = x1;
        *by1 = y1;
        if (!(x1 - x0 >= 1.0) || !(y1 - y0 >= 1.0)) return false;
        pc_path_init(&path);
        st = pc_path_add_ellipse(&path, (x0 + x1) * 0.5, (y0 + y1) * 0.5, (x1 - x0) * 0.5,
                                 (y1 - y0) * 0.5);
        /* fine tessellation keeps small circles round (R 4.2.14) */
        if (st == PC_OK) st = pc_path_flatten(&path, NULL, 0.02, &m->poly);
        pc_path_free(&path);
        return st == PC_OK && m->poly.n_contours > 0u;
    }
    /* lasso: the trace, closed back to the start (docs) */
    if (m->n < 3u) return false;
    {
        pc_pt mn = m->pts[0], mx = m->pts[0];
        for (size_t i = 0; i < m->n; i++) {
            if (pc_poly_add(&m->poly, m->pts[i], 0u) != PC_OK) return false;
            mn.x = fmin(mn.x, m->pts[i].x);
            mn.y = fmin(mn.y, m->pts[i].y);
            mx.x = fmax(mx.x, m->pts[i].x);
            mx.y = fmax(mx.y, m->pts[i].y);
        }
        if (pc_poly_end(&m->poly, true) != PC_OK) return false;
        *bx0 = mn.x;
        *by0 = mn.y;
        *bx1 = mx.x;
        *by1 = mx.y;
        return fabs(pc_poly_area(&m->poly)) > 1e-9;
    }
}

bool sel_marquee_shape(app *a, sel_marquee *m, app_doc *d, pc_rect *r)
{
    double x0, y0, x1, y1;
    pc_rect rr;
    bool ok = build(a, m, d, &rr, &x0, &y0, &x1, &y1);
    if (r) *r = rr;
    return ok;
}

/* Lasso self-intersections use the even-odd rule (3.36: GDI+ alternate
 * fill of the selection path); ellipses are simple. */
static pc_fill_rule rule_of(const sel_marquee *m)
{
    return m->shape == SEL_SHAPE_LASSO ? PC_FILL_EVENODD : PC_FILL_NONZERO;
}

/* ---- preview ------------------------------------------------------------------------ */
/* The selected area of the shape inside the image (status bar). */
static double shape_area(const sel_marquee *m, const app_doc *d, bool ok, pc_rect r,
                         const sel_ss *ss, bool have_ss)
{
    if (!ok) return 0.0;
    if (m->shape == SEL_SHAPE_RECT) {
        pc_rect c = pc_rect_intersect(r, pc_doc_rect(d->doc));
        return pc_rect_is_empty(c) ? 0.0 : (double)c.w * (double)c.h;
    }
    return have_ss ? ss->area : -1.0;
}

static void preview(app *a, sel_marquee *m, app_doc *d)
{
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    pc_rect r;
    bool ok = build(a, m, d, &r, &x0, &y0, &x1, &y1);
    pc_status st = PC_OK;
    sel_ss ss;
    bool have_ss = false;
    memset(&ss, 0, sizeof ss);
    pc_poly_clear(&m->preview);
    m->mods = ui_mods(a->ui);
    m->dirty = false;
    if (ok && m->shape != SEL_SHAPE_RECT)
        have_ss = sel_ss_build(&ss, &m->poly, rule_of(m), d->doc) == PC_OK;
    if (m->n)
        sel_status_rect(a, d, x0, y0, x1 - x0, y1 - y0, shape_area(m, d, ok, r, &ss, have_ss));
    if (!ok) {
        /* an empty shape combines to nothing (Replace, Intersect) or to the
         * current selection (the other modes) */
        if (m->mode == PC_SEL_REPLACE || m->mode == PC_SEL_INTERSECT)
            (void)app_doc_ants_preview(d, &m->preview);
        else
            (void)app_doc_ants_preview(d, NULL);
        return;
    }
    if (m->shape == SEL_SHAPE_RECT) {
        pc_sel_src src;
        if (m->mode == PC_SEL_REPLACE) {
            pc_rect c = pc_rect_intersect(r, pc_doc_rect(d->doc));
            if (!pc_rect_is_empty(c)) {
                st = pc_poly_add(&m->preview, pc_pt_make(c.x, c.y), 0u);
                if (st == PC_OK) st = pc_poly_add(&m->preview, pc_pt_make(c.x, c.y + c.h), 0u);
                if (st == PC_OK)
                    st = pc_poly_add(&m->preview, pc_pt_make(c.x + c.w, c.y + c.h), 0u);
                if (st == PC_OK) st = pc_poly_add(&m->preview, pc_pt_make(c.x + c.w, c.y), 0u);
                if (st == PC_OK) st = pc_poly_end(&m->preview, true);
            }
        } else {
            pc_sel_src_rect(&src, r);
            st = pc_sel_contour_preview_src(d->doc, &src, m->mode, 0.0, &m->preview);
        }
    } else if (a->ts.sel_clip_aa && have_ss) {
        /* T-SEL-QUALITY: antialiased shapes are 4 x 4 supersampled */
        pc_sel_src src;
        sel_ss_src(&src, &ss);
        st = pc_sel_contour_preview_src(d->doc, &src, m->mode, 0.0, &m->preview);
    } else {
        pc_sel_state pst;
        pc_sel_src src;
        st = pc_sel_state_from_poly(d->doc, &m->poly, rule_of(m), a->ts.sel_clip_aa, &pst);
        if (st == PC_OK) {
            pc_sel_src_state(&src, &pst);
            st = pc_sel_contour_preview_src(d->doc, &src, m->mode, 0.0, &m->preview);
            pc_sel_state_free(&pst);
        }
    }
    sel_ss_free(&ss);
    if (st == PC_OK) (void)app_doc_ants_preview(d, &m->preview);
    else (void)app_doc_ants_preview(d, NULL);
}

/* ---- the drag ----------------------------------------------------------------------- */
static void stop(app *a, sel_marquee *m, app_doc *d)
{
    m->tracking = false;
    m->move_origin = false;
    m->n = 0;
    if (d) (void)app_doc_ants_preview(d, NULL);
    app_status(a, NULL);
    app_request_frame(a);
}

void sel_marquee_abort(app *a, sel_marquee *m)
{
    if (!m->tracking) return;
    stop(a, m, sel_doc_by_id(a, m->doc_id));
}

static void report(app *a, app_doc *d, pc_status st, const char *what)
{
    if (st == PC_OK) app_doc_history_changed(a, d);
    else if (st != PC_ERR_STATE) app_error(a, "%s failed: %s.", what, pc_status_str(st));
}

/* Release: the 3.36 truth table (Clear / Emit / Reset). */
static void done(app *a, sel_marquee *m, app_doc *d, uint64_t t_ns)
{
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    pc_rect r;
    bool ok = build(a, m, d, &r, &x0, &y0, &x1, &y1);
    bool append = m->mode != PC_SEL_REPLACE;
    bool quick = m->t0_ns && t_ns && t_ns >= m->t0_ns && t_ns - m->t0_ns <= QUICK_NS &&
                 m->max_disp < sel_dip(a, QUICK_PX);
    bool clipped = !ok || pc_rect_is_empty(pc_rect_intersect(
                              pc_rect_make((int32_t)floor(sel_clampd(x0)),
                                           (int32_t)floor(sel_clampd(y0)),
                                           (int32_t)ceil(sel_clampd(x1) - floor(sel_clampd(x0))),
                                           (int32_t)ceil(sel_clampd(y1) - floor(sel_clampd(y0)))),
                              pc_doc_rect(d->doc)));
    enum { CLEAR, EMIT, RESET } what;
    if (m->press_outside && !m->moved) what = CLEAR;             /* T-SEL-OFFCANVAS */
    else if (append) what = (!m->moved || clipped) ? RESET : EMIT;
    else what = (m->moved && !quick && !clipped) ? EMIT : CLEAR;
    stop(a, m, d);
    if (what == CLEAR) {
        if (pc_sel_is_active(d->doc))
            report(a, d, pc_sel_deselect(d->hist, "Deselect"), "Deselect");
    } else if (what == EMIT) {
        pc_status st = PC_ERR_STATE;
        sel_ss ss;
        if (m->shape == SEL_SHAPE_RECT) {
            st = pc_sel_apply_rect(d->hist, r, m->mode, m->label);
        } else {
            /* T-SEL-QUALITY: antialiased shapes are 4 x 4 supersampled; the
             * analytic rasterizer covers pixelated shapes (and shapes too
             * complex for the sample table) */
            bool done_ss = false;
            if (a->ts.sel_clip_aa && sel_ss_build(&ss, &m->poly, rule_of(m), d->doc) == PC_OK) {
                pc_sel_src src;
                sel_ss_src(&src, &ss);
                st = pc_sel_apply_src(d->hist, &src, m->mode, m->label);
                sel_ss_free(&ss);
                done_ss = true;
            }
            if (!done_ss)
                st = pc_sel_apply_poly(d->hist, &m->poly, rule_of(m), a->ts.sel_clip_aa, m->mode,
                                       m->label);
        }
        report(a, d, st, m->label);
    }
}

void sel_marquee_pointer(app *a, sel_marquee *m, const app_pointer *ev)
{
    app_doc *d = app_active_doc(a);
    double x = sel_clampd(ev->x), y = sel_clampd(ev->y);
    if (!d) return;
    if (m->tracking && m->doc_id != d->id) {
        sel_marquee_abort(a, m);
        return;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (m->tracking) {
            /* T-SEL-BOTH: the other button moves the shape while held */
            if (ev->button != m->button && (ev->button == APP_BTN_LEFT ||
                                            ev->button == APP_BTN_RIGHT)) {
                m->move_origin = true;
                m->lx = x;
                m->ly = y;
            }
            break;
        }
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        if (d->txn) break;
        load_opts(a, m);
        m->tracking = true;
        m->move_origin = false;
        m->button = ev->button;
        m->doc_id = d->id;
        m->mode = sel_mode_for(a, ev->button, ev->mods);
        m->was_active = pc_sel_is_active(d->doc);
        m->press_outside = x < 0.0 || y < 0.0 || x >= (double)d->doc->w ||
                           y >= (double)d->doc->h;
        m->moved = false;
        m->t0_ns = ev->time_ns;
        m->sx0 = ev->sx;
        m->sy0 = ev->sy;
        m->max_disp = 0.0f;
        m->n = 0;
        m->nx = 0.0;
        m->ny = 0.0;
        if (!push_pt(m, x, y)) {
            m->tracking = false;
            break;
        }
        m->dirty = true;
        break;
    case APP_PTR_MOVE:
    case APP_PTR_UP: {
        float dsx = ev->sx - m->sx0, dsy = ev->sy - m->sy0, dd;
        if (!m->tracking) break;
        dd = sqrtf(dsx * dsx + dsy * dsy);
        if (dd > m->max_disp) m->max_disp = dd;
        x = sel_clampd(x + m->nx);
        y = sel_clampd(y + m->ny);
        if (m->move_origin) {
            translate_pts(m, x - m->lx, y - m->ly);
            m->lx = x;
            m->ly = y;
        } else if (m->shape == SEL_SHAPE_LASSO) {
            const pc_pt *l = &m->pts[m->n - 1u];
            if (l->x != x || l->y != y) {
                if (!push_pt(m, x, y)) m->pts[m->n - 1u] = pc_pt_make(x, y);
            }
        } else if (m->n == 1u) {
            if (!push_pt(m, x, y)) break;
        } else {
            m->pts[m->n - 1u] = pc_pt_make(x, y);
        }
        if (m->max_disp > 0.0f) m->moved = true;
        m->dirty = true;
        if (ev->kind == APP_PTR_UP) {
            if (m->move_origin) m->move_origin = false;
            else done(a, m, d, ev->time_ns);
        }
        app_request_frame(a);
        break;
    }
    case APP_PTR_CANCEL:
        sel_marquee_abort(a, m);
        break;
    default:
        break;
    }
}

bool sel_marquee_key(app *a, sel_marquee *m, int32_t key, uint32_t mods)
{
    double step = sel_mods_ctrl(mods) ? 10.0 : 1.0, dx = 0.0, dy = 0.0;
    if (!m->tracking) return app_tool_nudge_pointer(a, key, mods);   /* T-FW-ARROWS */
    switch (key) {
    case SDLK_LEFT: dx = -step; break;
    case SDLK_RIGHT: dx = step; break;
    case SDLK_UP: dy = -step; break;
    case SDLK_DOWN: dy = step; break;
    case SDLK_ESCAPE:
        sel_marquee_abort(a, m);
        return true;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        m->dirty = true;
        app_request_frame(a);
        return false;
    default:
        return false;
    }
    /* the whole shape moves; later pointer positions keep the offset */
    translate_pts(m, dx, dy);
    if (m->move_origin) {
        m->lx += dx;
        m->ly += dy;
    }
    m->nx += dx;
    m->ny += dy;
    m->moved = true;
    m->dirty = true;
    app_request_frame(a);
    return true;
}

void sel_marquee_overlay(app *a, sel_marquee *m, app_overlay *o)
{
    app_doc *d = app_active_doc(a);
    if (!d) return;
    if (m->tracking) {
        if (m->doc_id != d->id) {
            sel_marquee_abort(a, m);
        } else {
            if (ui_mods(a->ui) != m->mods) m->dirty = true;     /* Shift released */
            if (m->dirty) preview(a, m, d);
            sel_animate_ants(a);
        }
    }
    sel_tint_draw(a, d, o);
}

/* ---- cursor (lane TOOLA) ------------------------------------------------------------- */
app_cursor sel_marquee_cursor(app *a, const sel_marquee *m, uint32_t mods)
{
    app_cursor base = m->shape == SEL_SHAPE_LASSO ? APP_CURSOR_LASSO : APP_CURSOR_SEL_REPLACE;
    int mode = m->tracking ? (int)m->mode : (int)sel_mode_for(a, APP_BTN_LEFT, mods);
    return app_cursor_sel_mode(base, mode);
}

/* ---- options ------------------------------------------------------------------------ */
static bool num_field(app *a, const char *label, const char *id, double *v, double lo,
                      double hi, int decimals)
{
    ui_ctx *ui = a->ui;
    double t = *v;
    app_opt_label(a, label);
    (void)app_opt_next(a, 78.0f);
    if (ui_number_double(ui, id, &t, lo, hi, 1.0, decimals, 0u) && t != *v) {
        *v = t;
        return true;
    }
    return false;
}

void sel_marquee_options(app *a, sel_marquee *m)
{
    ui_ctx *ui = a->ui;
    load_opts(a, m);
    sel_opt_mode(a);
    if (m->shape == SEL_SHAPE_RECT) {
        static const char *const modes[3] = { "Any Size", "Fixed Ratio", "Fixed Size" };
        static const char *const units[3] = { "Pixels", "Inches", "Centimeters" };
        int v = m->draw_mode;
        bool ch = false;
        (void)app_opt_next(a, 104.0f);
        if (ui_combo(ui, "##rectsel_mode", &v, modes, 3)) {
            m->draw_mode = v;
            ch = true;
        }
        paint_widget_note(a, "##rectsel_mode", ui_last_rect(ui));    /* tests */
        ui_tooltip(ui, "Selection draw mode");
        if (m->draw_mode == SEL_DRAW_RATIO) {
            ch |= num_field(a, "Width:", "##rectsel_rw", &m->ratio_w, 0.01, 65535.0, 2);
            ch |= num_field(a, "Height:", "##rectsel_rh", &m->ratio_h, 0.01, 65535.0, 2);
        } else if (m->draw_mode == SEL_DRAW_SIZE) {
            int u = size_units(a, m);
            int dec = u == APP_UNITS_PX ? 0 : 2;
            ch |= num_field(a, "Width:", "##rectsel_sw", &m->size_w, 0.01, 65535.0, dec);
            ch |= num_field(a, "Height:", "##rectsel_sh", &m->size_h, 0.01, 65535.0, dec);
            (void)app_opt_next(a, 104.0f);
            if (ui_combo(ui, "##rectsel_units", &u, units, 3) && u != size_units(a, m)) {
                m->size_units = u;
                m->size_units_set = true;
                ch = true;
            }
            paint_widget_note(a, "##rectsel_units", ui_last_rect(ui));   /* tests */
            ui_tooltip(ui, "Units of the fixed size");
        }
        app_opt_separator(a);
        if (ch) {
            store_opts(a, m);
            m->dirty = true;
            app_request_frame(a);
        }
    }
    sel_opt_quality(a);
}
