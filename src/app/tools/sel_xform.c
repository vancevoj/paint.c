/* sel_xform.c - transform frame, zones and drag math of the move tools
 * (see sel_xform.h). The nub layout, the scale-against-opposite-nub rule,
 * the aspect constraint (shorter ratio) and the 15 degree angle snap
 * follow the Paint.NET 3.36 MoveToolBase (MIT, docs/notice/a.md); the
 * math is paint.c's own (local box coordinates instead of 3.36's
 * incremental path transforms). Main thread. */
#include "sel_xform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

#define NUB_HIT_DIP     7.0f
#define ANCHOR_HIT_DIP  8.0f
#define ICON_HIT_DIP    9.0f
#define ICON_OFF_DIP    18.0f
#define CORRIDOR_DIP    22.0f
#define MIN_EXTENT      0.01        /* smallest scaled side in pixels */

static const double k_nu[SEL_NUBS] = { 0.0, 0.5, 1.0, 1.0, 1.0, 0.5, 0.0, 0.0 };
static const double k_nv[SEL_NUBS] = { 0.0, 0.0, 0.0, 0.5, 1.0, 1.0, 1.0, 0.5 };

void sel_box_set(sel_box *b, double x0, double y0, double x1, double y1)
{
    b->x0 = x0;
    b->y0 = y0;
    b->x1 = x1;
    b->y1 = y1;
    b->m = pc_affine_identity();
    b->ax = (x0 + x1) * 0.5;
    b->ay = (y0 + y1) * 0.5;
    b->angle = 0.0;
}

static pc_pt local_nub(const sel_box *b, int i)
{
    return pc_pt_make(b->x0 + k_nu[i] * (b->x1 - b->x0), b->y0 + k_nv[i] * (b->y1 - b->y0));
}

pc_pt sel_box_nub(const sel_box *b, int i) { return pc_affine_apply(&b->m, local_nub(b, i)); }

pc_pt sel_box_anchor(const sel_box *b)
{
    return pc_affine_apply(&b->m, pc_pt_make(b->ax, b->ay));
}

void sel_box_size(const sel_box *b, double *w, double *h)
{
    pc_pt u = pc_affine_apply_vec(&b->m, pc_pt_make(b->x1 - b->x0, 0.0));
    pc_pt v = pc_affine_apply_vec(&b->m, pc_pt_make(0.0, b->y1 - b->y0));
    *w = hypot(u.x, u.y);
    *h = hypot(v.x, v.y);
}

pc_rect sel_box_bounds(const sel_box *b)
{
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    for (int i = 0; i < SEL_NUBS; i += 2) {
        pc_pt p = sel_box_nub(b, i);
        mnx = fmin(mnx, p.x);
        mny = fmin(mny, p.y);
        mxx = fmax(mxx, p.x);
        mxy = fmax(mxy, p.y);
    }
    mnx = floor(sel_clampd(mnx));
    mny = floor(sel_clampd(mny));
    mxx = ceil(sel_clampd(mxx));
    mxy = ceil(sel_clampd(mxy));
    return pc_rect_make((int32_t)mnx, (int32_t)mny, (int32_t)(mxx - mnx), (int32_t)(mxy - mny));
}

/* ---- zones ------------------------------------------------------------------------------- */
static void to_scr(const gfx_view *v, pc_pt p, double *x, double *y)
{
    gfx_view_to_screen(v, p.x, p.y, x, y);
}

static double seg_dist(double px, double py, double ax, double ay, double bx, double by)
{
    double vx = bx - ax, vy = by - ay, l = vx * vx + vy * vy, t;
    if (l <= 0.0) return hypot(px - ax, py - ay);
    t = ((px - ax) * vx + (py - ay) * vy) / l;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return hypot(px - (ax + t * vx), py - (ay + t * vy));
}

/* Screen position of the four-way move icon: diagonally out from the
 * corner nearest the screen's bottom right. */
static void icon_pos(const app *a, const sel_box *b, const gfx_view *v, double *x, double *y)
{
    double bx = 0.0, by = 0.0, best = -1e300, off = (double)sel_dip(a, ICON_OFF_DIP);
    for (int i = 0; i < SEL_NUBS; i += 2) {
        double sx, sy;
        to_scr(v, sel_box_nub(b, i), &sx, &sy);
        if (sx + sy > best) {
            best = sx + sy;
            bx = sx;
            by = sy;
        }
    }
    *x = bx + off;
    *y = by + off;
}

int sel_box_zone(const app *a, const sel_box *b, const gfx_view *v, double sx, double sy,
                 bool corridor)
{
    double cx[4], cy[4], best = 1e300, ix, iy;
    int nub = -1, sgn = 0;
    bool inside = true;
    {
        double ax, ay;
        to_scr(v, sel_box_anchor(b), &ax, &ay);
        if (hypot(sx - ax, sy - ay) <= (double)sel_dip(a, ANCHOR_HIT_DIP)) return SEL_ZONE_ANCHOR;
    }
    icon_pos(a, b, v, &ix, &iy);
    if (fabs(sx - ix) <= (double)sel_dip(a, ICON_HIT_DIP) &&
        fabs(sy - iy) <= (double)sel_dip(a, ICON_HIT_DIP))
        return SEL_ZONE_ICON;
    for (int i = 0; i < SEL_NUBS; i++) {
        double nx, ny, dd;
        to_scr(v, sel_box_nub(b, i), &nx, &ny);
        dd = hypot(sx - nx, sy - ny);
        if (dd <= (double)sel_dip(a, NUB_HIT_DIP) && dd < best) {
            best = dd;
            nub = i;
        }
    }
    if (nub >= 0) return SEL_ZONE_NUB + nub;
    for (int i = 0; i < 4; i++) to_scr(v, sel_box_nub(b, 2 * i), &cx[i], &cy[i]);
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        double cr = (cx[j] - cx[i]) * (sy - cy[i]) - (cy[j] - cy[i]) * (sx - cx[i]);
        int s = cr > 0.0 ? 1 : (cr < 0.0 ? -1 : 0);
        if (s != 0) {
            if (sgn == 0) sgn = s;
            else if (s != sgn) inside = false;
        }
    }
    if (inside) return SEL_ZONE_MOVE;
    if (corridor) {
        double dmin = 1e300;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            dmin = fmin(dmin, seg_dist(sx, sy, cx[i], cy[i], cx[j], cy[j]));
        }
        if (dmin <= (double)sel_dip(a, CORRIDOR_DIP)) return SEL_ZONE_ROTATE;
    }
    return SEL_ZONE_MOVE;
}

app_cursor sel_zone_cursor(int zone)
{
    if (zone >= SEL_ZONE_NUB && zone < SEL_ZONE_NUB + SEL_NUBS) return APP_CURSOR_HAND;
    if (zone == SEL_ZONE_ROTATE) return APP_CURSOR_ROTATE;
    return APP_CURSOR_MOVE;
}

/* ---- drags ------------------------------------------------------------------------------- */
void sel_drag_begin(sel_box *b, sel_drag *g, sel_drag_kind kind, int nub, double x, double y)
{
    pc_pt c = sel_box_anchor(b);
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->nub = nub;
    g->m0 = b->m;
    g->angle0 = b->angle;
    g->px0 = x;
    g->py0 = y;
    g->cx = c.x;
    g->cy = c.y;
    g->gx = c.x - x;
    g->gy = c.y - y;
}

/* 3.36 ConstrainAngle: nearest multiple of 15 degrees, in (-180, 180]. */
static double snap15(double deg)
{
    double a = fmod(deg, 360.0), r;
    if (a < 0.0) a += 360.0;
    r = floor(a / 15.0 + 0.5) * 15.0;
    if (r > 180.0) r -= 360.0;
    return r;
}

static double clamp_scale(double s, double extent)
{
    double ext = fabs(extent);
    if (!(s == s)) return 1.0;
    if (ext <= 0.0) return 1.0;
    if (fabs(s) * ext < MIN_EXTENT) s = (s < 0.0 ? -MIN_EXTENT : MIN_EXTENT) / ext;
    if (fabs(s) > 1e5) s = s < 0.0 ? -1e5 : 1e5;
    return s;
}

bool sel_drag_update(sel_box *b, sel_drag *g, double x, double y, uint32_t mods)
{
    pc_affine old = b->m;
    x = sel_clampd(x);
    y = sel_clampd(y);
    switch (g->kind) {
    case SEL_DRAG_MOVE: {
        pc_affine t;
        g->dx = sel_round(x - g->px0);
        g->dy = sel_round(y - g->py0);
        t = pc_affine_translate(g->dx, g->dy);
        b->m = pc_affine_compose(&t, &g->m0);
        break;
    }
    case SEL_DRAG_ROTATE: {
        double t0 = atan2(g->py0 - g->cy, g->px0 - g->cx), t1 = atan2(y - g->cy, x - g->cx);
        double deg = (y == g->cy && x == g->cx) ? 0.0 : (t1 - t0) * 180.0 / PI;
        double total = g->angle0 + deg;
        pc_affine r;
        if (mods & UI_MOD_SHIFT) {
            total = snap15(total);
            deg = total - g->angle0;
        }
        b->angle = g->angle0 + deg;
        r = pc_affine_rotate_about(deg * PI / 180.0, g->cx, g->cy);
        b->m = pc_affine_compose(&r, &g->m0);
        break;
    }
    case SEL_DRAG_SCALE: {
        pc_affine inv, s1, s2, s3, tmp;
        pc_pt d, n = local_nub(b, g->nub), an, q;
        bool corner = (g->nub % 2) == 0, center = (mods & UI_MOD_ALT) != 0u;
        double sx = 1.0, sy = 1.0;
        if (!pc_affine_invert(&g->m0, &inv)) return false;
        d = pc_affine_apply_vec(&inv, pc_pt_make(x - g->px0, y - g->py0));
        q = pc_pt_make(n.x + d.x, n.y + d.y);
        an = center ? pc_pt_make((b->x0 + b->x1) * 0.5, (b->y0 + b->y1) * 0.5)
                    : local_nub(b, (g->nub + 4) % SEL_NUBS);
        if (k_nu[g->nub] != 0.5 && n.x != an.x) sx = (q.x - an.x) / (n.x - an.x);
        if (k_nv[g->nub] != 0.5 && n.y != an.y) sy = (q.y - an.y) / (n.y - an.y);
        if (corner && (mods & UI_MOD_SHIFT)) {
            double m = fmin(fabs(sx), fabs(sy));
            sx = sx < 0.0 ? -m : m;
            sy = sy < 0.0 ? -m : m;
        }
        sx = clamp_scale(sx, b->x1 - b->x0);
        sy = clamp_scale(sy, b->y1 - b->y0);
        s1 = pc_affine_translate(-an.x, -an.y);
        s2 = pc_affine_scale(sx, sy);
        s3 = pc_affine_translate(an.x, an.y);
        tmp = pc_affine_compose(&s2, &s1);
        tmp = pc_affine_compose(&s3, &tmp);
        b->m = pc_affine_compose(&g->m0, &tmp);
        break;
    }
    case SEL_DRAG_ANCHOR: {
        pc_affine inv;
        if (pc_affine_invert(&b->m, &inv)) {
            pc_pt l = pc_affine_apply(&inv, pc_pt_make(x + g->gx, y + g->gy));
            b->ax = l.x;
            b->ay = l.y;
        }
        return false;
    }
    }
    if (!pc_affine_is_finite(&b->m)) b->m = old;
    return memcmp(&old, &b->m, sizeof old) != 0;
}

void sel_box_nudge(sel_box *b, double dx, double dy)
{
    pc_affine t = pc_affine_translate(dx, dy);
    b->m = pc_affine_compose(&t, &b->m);
}

void sel_drag_status(app *a, const app_doc *d, const sel_box *b, const sel_drag *g)
{
    char buf[160], s0[32], s1[32];
    double dpi = d && d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    switch (g->kind) {
    case SEL_DRAG_MOVE:
        app_format_len(a, g->dx, dpi, s0, sizeof s0);
        app_format_len(a, g->dy, dpi, s1, sizeof s1);
        snprintf(buf, sizeof buf, "Offset %s, %s", s0, s1);
        break;
    case SEL_DRAG_ROTATE: {
        /* shown counterclockwise positive, like an angle dial */
        double deg = -b->angle;
        deg = fmod(deg, 360.0);
        if (deg <= -180.0) deg += 360.0;
        if (deg > 180.0) deg -= 360.0;
        if (fabs(deg) < 0.005) deg = 0.0;
        snprintf(buf, sizeof buf, "Angle %.2f\xC2\xB0", deg);
        break;
    }
    case SEL_DRAG_SCALE: {
        double w, h;
        sel_box_size(b, &w, &h);
        app_format_len(a, w, dpi, s0, sizeof s0);
        app_format_len(a, h, dpi, s1, sizeof s1);
        snprintf(buf, sizeof buf, "Size %s \xC3\x97 %s", s0, s1);
        break;
    }
    default:
        return;
    }
    app_status(a, buf);
}

/* ---- drawing ------------------------------------------------------------------------------- */
static void four_arrows(app_overlay *o, double sx, double sy, double r, double t, ui_color c)
{
    app_ov_line(o, sx - r, sy, sx + r, sy, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx, sy - r, sx, sy + r, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx - r, sy, sx - r + t, sy - t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx - r, sy, sx - r + t, sy + t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx + r, sy, sx + r - t, sy - t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx + r, sy, sx + r - t, sy + t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx, sy - r, sx - t, sy - r + t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx, sy - r, sx + t, sy - r + t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx, sy + r, sx - t, sy + r - t, 1.0f, c, APP_OV_SCREEN);
    app_ov_line(o, sx, sy + r, sx + t, sy + r - t, 1.0f, c, APP_OV_SCREEN);
}

void sel_draw_move_nub(const app *a, app_overlay *o, double sx, double sy)
{
    double s = (double)sel_dip(a, 15.0f), h = s * 0.5;
    ui_color dark = ui_rgba(0, 0, 0, 220), white = ui_rgba(255, 255, 255, 255);
    sx = floor(sx) + 0.5;
    sy = floor(sy) + 0.5;
    app_ov_fill_rect(o, sx - h - 1.0, sy - h - 1.0, s + 2.0, s + 2.0, dark, APP_OV_SCREEN);
    app_ov_fill_rect(o, sx - h, sy - h, s, s, white, APP_OV_SCREEN);
    four_arrows(o, sx, sy, h - 2.0, (double)sel_dip(a, 3.0f), dark);
}

void sel_box_draw(app *a, app_overlay *o, const sel_box *b, bool nubs, bool show_anchor,
                  bool icon)
{
    ui_ctx *ui = o->ui;
    ui_color dark = ui_rgba(0, 0, 0, 230), white = ui_rgba(255, 255, 255, 255);
    if (nubs) {
        for (int i = 0; i < SEL_NUBS; i++) {
            double sx, sy;
            app_ov_to_screen(o, sel_box_nub(b, i).x, sel_box_nub(b, i).y, &sx, &sy);
            if (i % 2 == 0) {
                float r = sel_dip(a, 4.5f);
                ui_vec2 c = ui_vec2_make((float)sx, (float)sy);
                ui_draw_circle(ui, c, r + 1.0f, dark);
                ui_draw_circle(ui, c, r, white);
            } else {
                double h = (double)sel_dip(a, 4.0f);
                app_ov_fill_rect(o, sx - h - 1.0, sy - h - 1.0, 2.0 * h + 2.0, 2.0 * h + 2.0, dark,
                                 APP_OV_SCREEN);
                app_ov_fill_rect(o, sx - h, sy - h, 2.0 * h, 2.0 * h, white, APP_OV_SCREEN);
            }
        }
    }
    if (show_anchor) {
        pc_pt c = sel_box_anchor(b);
        double sx, sy, r = (double)sel_dip(a, 6.0f);
        ui_vec2 v;
        app_ov_to_screen(o, c.x, c.y, &sx, &sy);
        v = ui_vec2_make((float)sx, (float)sy);
        ui_draw_circle_outline(ui, v, (float)r, 3.0f, dark);
        ui_draw_circle_outline(ui, v, (float)r, 1.2f, white);
        app_ov_line(o, sx - r - 3.0, sy, sx + r + 3.0, sy, 3.0f, dark, APP_OV_SCREEN);
        app_ov_line(o, sx, sy - r - 3.0, sx, sy + r + 3.0, 3.0f, dark, APP_OV_SCREEN);
        app_ov_line(o, sx - r - 2.0, sy, sx + r + 2.0, sy, 1.0f, white, APP_OV_SCREEN);
        app_ov_line(o, sx, sy - r - 2.0, sx, sy + r + 2.0, 1.0f, white, APP_OV_SCREEN);
    }
    if (icon) {
        double ix, iy;
        icon_pos(a, b, &o->v, &ix, &iy);
        sel_draw_move_nub(a, o, ix, iy);
    }
}
