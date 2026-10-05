/* sel_common.c - lane A helpers shared by the selection and move tools
 * (see sel_common.h). Main thread. */
#include "sel_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- history groups --------------------------------------------------------------- */
typedef struct grp_payload {
    size_t              n;
    bool                applied;     /* the document is in the "after" state */
    const pc_hist_ops **ops;
    void              **pl;
} grp_payload;

static void grp_swap(pc_doc *d, void *p)
{
    grp_payload *g = (grp_payload *)p;
    if (g->applied) {
        for (size_t i = g->n; i > 0; i--) g->ops[i - 1u]->swap(d, g->pl[i - 1u]);
    } else {
        for (size_t i = 0; i < g->n; i++) g->ops[i]->swap(d, g->pl[i]);
    }
    g->applied = !g->applied;
}

static void grp_destroy(void *p)
{
    grp_payload *g = (grp_payload *)p;
    if (!g) return;
    for (size_t i = 0; i < g->n; i++)
        if (g->ops[i] && g->ops[i]->destroy) g->ops[i]->destroy(g->pl[i]);
    free(g);
}

static size_t grp_bytes(const void *p)
{
    const grp_payload *g = (const grp_payload *)p;
    size_t b = sizeof *g;
    for (size_t i = 0; i < g->n; i++)
        if (g->ops[i] && g->ops[i]->bytes) b += g->ops[i]->bytes(g->pl[i]);
    return b;
}

static const pc_hist_ops k_grp_ops = { grp_swap, grp_destroy, grp_bytes };

void sel_hist_group_begin(const pc_hist *h, sel_hist_group *g)
{
    g->base_seq = h && h->cur ? h->cur->seq : 0u;
}

static void set_label(pc_hist_node *n, const char *label)
{
    size_t k = 0;
    if (!label) return;
    for (; k + 1u < sizeof n->label && label[k]; k++) n->label[k] = label[k];
    n->label[k] = '\0';
}

bool sel_hist_group_end(pc_hist *h, const sel_hist_group *g, const char *label)
{
    pc_hist_node *n, *first;
    size_t k = 0, bytes, arr;
    grp_payload *gp;
    if (!h || !g) return false;
    /* the nodes created since begin: consecutive ancestors of cur with a
     * newer seq (pruning may have collapsed the root into one of them,
     * which then has no ops and is skipped) */
    for (n = h->cur; n && n->parent && n->ops && n->seq > g->base_seq; n = n->parent) k++;
    if (k == 0u) return false;
    first = h->cur;
    for (size_t i = 1; i < k; i++) first = first->parent;
    if (k == 1u) {
        set_label(first, label);
        return true;
    }
    /* the chain must be linear: every inner node has exactly one child */
    for (n = h->cur; n != first; n = n->parent)
        if (n->parent->first_child != n || n->next_sibling) {
            set_label(h->cur, label);
            return true;
        }
    if (!pc_mul_size(k, sizeof(void *) * 2u, &arr) || !pc_add_size(arr, sizeof *gp, &bytes))
        return true;
    gp = (grp_payload *)malloc(bytes);
    if (!gp) {
        set_label(h->cur, label);
        return true;
    }
    gp->n = k;
    gp->applied = true;
    gp->ops = (const pc_hist_ops **)(void *)(gp + 1);
    gp->pl = (void **)(void *)(gp->ops + k);
    n = h->cur;
    for (size_t i = k; i > 0; i--) {
        gp->ops[i - 1u] = n->ops;
        gp->pl[i - 1u] = n->payload;
        n = n->parent;
    }
    /* free the inner node shells (their payloads now belong to gp) */
    n = h->cur;
    while (n != first) {
        pc_hist_node *p = n->parent;
        pc_hist_node_free_unlinked(n);
        n = p;
    }
    first->ops = &k_grp_ops;
    first->payload = gp;
    first->first_child = NULL;
    first->redo_child = NULL;
    set_label(first, label);
    h->cur = first;
    h->count -= k - 1u;
    return true;
}

/* ---- documents ------------------------------------------------------------------------ */
app_doc *sel_doc_by_id(const app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

app_doc *sel_active_doc_if(const app *a, uint32_t id)
{
    app_doc *d = app_active_doc(a);
    return d && d->id == id ? d : NULL;
}

/* ---- combine modes ----------------------------------------------------------------------- */
bool sel_mods_ctrl(uint32_t mods) { return (mods & ui_mod_primary()) != 0u; }
bool sel_mods_alt(uint32_t mods) { return (mods & UI_MOD_ALT) != 0u; }

pc_sel_mode sel_mode_for(const app *a, int button, uint32_t mods)
{
    bool ctrl = sel_mods_ctrl(mods), alt = sel_mods_alt(mods);
    if (button == APP_BTN_LEFT && ctrl) return PC_SEL_UNION;
    if (button == APP_BTN_LEFT && alt) return PC_SEL_EXCLUDE;
    if (button == APP_BTN_RIGHT && ctrl) return PC_SEL_XOR;
    if (button == APP_BTN_RIGHT && alt) return PC_SEL_INTERSECT;
    if (a->ts.sel_mode < 0 || a->ts.sel_mode >= (int32_t)PC_SEL_MODE_COUNT) return PC_SEL_REPLACE;
    return (pc_sel_mode)a->ts.sel_mode;
}

const char *sel_mode_name(pc_sel_mode m)
{
    static const char *const names[PC_SEL_MODE_COUNT] = {
        "Replace", "Add (union)", "Subtract", "Intersect", "Invert (xor)"
    };
    return (unsigned)m < (unsigned)PC_SEL_MODE_COUNT ? names[m] : "Replace";
}

/* ---- options bar widgets ----------------------------------------------------------------- */
void sel_opt_mode(app *a)
{
    static const ui_icon icons[PC_SEL_MODE_COUNT] = {
        UI_ICON_SEL_REPLACE, UI_ICON_SEL_UNION, UI_ICON_SEL_EXCLUDE, UI_ICON_SEL_INTERSECT,
        UI_ICON_SEL_XOR
    };
    static const char *const ids[PC_SEL_MODE_COUNT] = {
        "##selmode0", "##selmode1", "##selmode2", "##selmode3", "##selmode4"
    };
    static const char *const tips[PC_SEL_MODE_COUNT] = {
        "Selection mode: Replace",
        "Selection mode: Add (union). Hold Ctrl and use the left button for one selection.",
        "Selection mode: Subtract. Hold Alt and use the left button for one selection.",
        "Selection mode: Intersect. Hold Alt and use the right button for one selection.",
        "Selection mode: Invert (xor). Hold Ctrl and use the right button for one selection."
    };
    ui_ctx *ui = a->ui;
    for (int i = 0; i < (int)PC_SEL_MODE_COUNT; i++) {
        (void)app_opt_next(a, 30.0f);
        if (ui_tool_button(ui, ids[i], icons[i], a->ts.sel_mode == i, tips[i]) &&
            a->ts.sel_mode != i) {
            a->ts.sel_mode = i;
            app_tool_settings_changed(a);
        }
    }
    app_opt_separator(a);
}

void sel_opt_quality(app *a)
{
    ui_ctx *ui = a->ui;
    int r;
    (void)app_opt_next(a, 42.0f);
    r = ui_split_button(ui, "##selquality", a->ts.sel_clip_aa ? UI_ICON_AA_ON : UI_ICON_AA_OFF,
                        false, a->ts.sel_clip_aa ? "Selection quality: antialiased"
                                                 : "Selection quality: pixelated");
    if (r == 1) {
        a->ts.sel_clip_aa = !a->ts.sel_clip_aa;
        app_tool_settings_changed(a);
    }
    if (r == 2) ui_popup_open(ui, "##selquality_menu", ui_last_rect(ui), UI_POPUP_BELOW);
    if (ui_popup_begin(ui, "##selquality_menu")) {
        if (ui_menu_radio(ui, "Antialiased", NULL, a->ts.sel_clip_aa, true) &&
            !a->ts.sel_clip_aa) {
            a->ts.sel_clip_aa = true;
            app_tool_settings_changed(a);
        }
        if (ui_menu_radio(ui, "Pixelated", NULL, !a->ts.sel_clip_aa, true) &&
            a->ts.sel_clip_aa) {
            a->ts.sel_clip_aa = false;
            app_tool_settings_changed(a);
        }
        ui_popup_end(ui);
    }
}

void sel_opt_flood(app *a)
{
    static const char *const items[2] = { "Contiguous", "Global" };
    ui_ctx *ui = a->ui;
    int v = a->ts.flood_global ? 1 : 0;
    app_opt_label(a, "Flood mode:");
    (void)app_opt_next(a, 104.0f);
    if (ui_combo(ui, "##selflood", &v, items, 2)) {
        a->ts.flood_global = v == 1;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Flood mode. Hold Shift while clicking to use the other mode.");
}

void sel_opt_tolerance(app *a)
{
    ui_ctx *ui = a->ui;
    int32_t v = a->ts.tolerance;
    app_opt_label(a, "Tolerance:");
    (void)app_opt_next(a, 130.0f);
    if (ui_slider_int(ui, "##seltol", &v, 0, 100, UI_SLIDER_PERCENT) && v != a->ts.tolerance) {
        a->ts.tolerance = v;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Tolerance: how different a color may be and still be selected");
    {
        char txt[16];
        snprintf(txt, sizeof txt, "%d%%", (int)a->ts.tolerance);
        (void)app_opt_next(a, 36.0f);
        ui_label(ui, txt);
    }
}

void sel_opt_tol_alpha(app *a)
{
    static const char *const items[2] = { "Premultiplied", "Straight" };
    ui_ctx *ui = a->ui;
    int v = a->ts.tol_straight ? 1 : 0;
    (void)app_opt_next(a, 118.0f);
    if (ui_combo(ui, "##seltolalpha", &v, items, 2)) {
        a->ts.tol_straight = v == 1;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Tolerance alpha mode. Premultiplied: all fully transparent pixels "
                   "match each other. Straight: transparent pixels match only when their "
                   "colors are equal.");
}

void sel_opt_sampling(app *a)
{
    static const char *const items[2] = { "Layer", "Image" };
    ui_ctx *ui = a->ui;
    int v = a->ts.sampling == 1 ? 1 : 0;
    app_opt_label(a, "Sampling:");
    (void)app_opt_next(a, 84.0f);
    if (ui_combo(ui, "##selsampling", &v, items, 2)) {
        a->ts.sampling = v;
        app_tool_settings_changed(a);
    }
    ui_tooltip(ui, "Sampling: the active layer, or the image as it is shown");
}

/* ---- combined contours ---------------------------------------------------------------- */
typedef struct comb_ctx {
    const pc_doc      *d;
    pc_tile *const    *grid;      /* old coverage tiles (NULL = none) */
    bool               active;
    const pc_sel_src  *src;
    pc_sel_mode        mode;
} comb_ctx;

static const uint8_t *comb_block(void *ud, int32_t bx, int32_t by, uint8_t *scratch,
                                 uint8_t *uniform)
{
    comb_ctx *c = (comb_ctx *)ud;
    const pc_tile *t = NULL;
    pc_rect r = pc_rect_intersect(pc_rect_make(bx * (int32_t)PC_TILE_DIM,
                                               by * (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM,
                                               (int32_t)PC_TILE_DIM),
                                  pc_doc_rect(c->d));
    pc_rect rs = pc_rect_intersect(r, c->src->bounds);
    int ub = -2;
    if (c->active && c->grid)
        t = c->grid[(size_t)by * c->d->tiles_x + (size_t)bx];
    if (pc_rect_is_empty(rs)) ub = 0;
    else if (c->src->uniform && rs.x == r.x && rs.y == r.y && rs.w == r.w && rs.h == r.h)
        ub = c->src->uniform(c->src->ud, rs);
    if (ub >= 0) {
        uint8_t z = pc_sel_combine(c->mode, 0u, (uint8_t)ub);
        uint8_t f = pc_sel_combine(c->mode, 255u, (uint8_t)ub);
        if (!t) {
            *uniform = z;
            return NULL;
        }
        if (z == 0u && f == 255u) return t->data;      /* combine keeps the old value */
        if (z == f) {
            *uniform = z;
            return NULL;
        }
    }
    memset(scratch, 0, PC_TILE_PX);
    if (!pc_rect_is_empty(rs)) {
        int32_t ox = rs.x - bx * (int32_t)PC_TILE_DIM, oy = rs.y - by * (int32_t)PC_TILE_DIM;
        c->src->fill(c->src->ud, rs, scratch + (size_t)oy * PC_TILE_DIM + (size_t)ox,
                     PC_TILE_DIM);
    }
    for (size_t i = 0; i < PC_TILE_PX; i++)
        scratch[i] = pc_sel_combine(c->mode, t ? t->data[i] : 0u, scratch[i]);
    return scratch;
}

pc_status sel_contour_combined(const pc_doc *d, const pc_sel_snap *snap, const pc_sel_src *src,
                               pc_sel_mode mode, pc_poly *out)
{
    comb_ctx c;
    pc_cov_field f;
    if (!d || !src || !out) return PC_ERR_ARG;
    c.d = d;
    c.grid = snap ? snap->grid : d->sel_grid;
    c.active = snap ? snap->active : d->sel_active;
    c.src = src;
    c.mode = mode;
    f.area = pc_doc_rect(d);
    f.block = comb_block;
    f.ud = &c;
    return pc_contour_field(&f, 0.0, out);
}

void sel_animate_ants(app *a)
{
    if (a->focused) app_request_frame_at(a, app_now_ms(a) + 60u);
}

/* ---- selection tint ---------------------------------------------------------------------- */
#define TINT_KEY "lane_a.tint"

typedef struct sel_tint {
    SDL_Texture *tex;
    int32_t      tex_w, tex_h;
    int32_t      w, h;
    float        x, y;              /* screen position of the texture */
    uint64_t     key;
    bool         has;
    uint32_t    *rgba;
    size_t       rgba_n;
    pc_mask      mask;
} sel_tint;

static void tint_free(void *p)
{
    sel_tint *t = (sel_tint *)p;
    if (!t) return;
    if (t->tex) SDL_DestroyTexture(t->tex);
    free(t->rgba);
    pc_mask_free(&t->mask);
    free(t);
}

static sel_tint *tint_get(app *a)
{
    sel_tint *t = (sel_tint *)app_ext_get(a, TINT_KEY);
    if (t) return t;
    t = (sel_tint *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    if (!app_ext_set(a, TINT_KEY, t, tint_free)) {
        free(t);
        return NULL;
    }
    return t;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void tint_cb(SDL_Renderer *r, ui_rect clip, void *ud)
{
    sel_tint *t = (sel_tint *)ud;
    SDL_FRect src, dst;
    (void)clip;
    if (!t->has || !t->tex) return;
    src.x = 0.0f;
    src.y = 0.0f;
    src.w = (float)t->w;
    src.h = (float)t->h;
    dst.x = t->x;
    dst.y = t->y;
    dst.w = (float)t->w;
    dst.h = (float)t->h;
    SDL_RenderTexture(r, t->tex, &src, &dst);
}

/* Tint color: a light blue wash (paint.c's own value). */
#define TINT_R 52u
#define TINT_G 132u
#define TINT_B 255u
#define TINT_A 54u

void sel_tint_draw(app *a, app_doc *d, app_overlay *o)
{
    const pc_poly *p;
    sel_tint *t;
    pc_pt mn, mx;
    double ox, oy, x0, y0, x1, y1;
    int32_t rx0, ry0, rx1, ry1, w, h;
    uint64_t key = 1469598103934665603ull;
    pc_affine m;
    if (!d || !a->ren) return;
    /* hidden while a dialog (Layer Properties, effects) shows colors */
    if (app_dialog_active(a)) return;
    p = app_doc_ants(d);
    if (!p || p->n_contours == 0u || !pc_poly_bounds(p, &mn, &mx)) return;
    t = tint_get(a);
    if (!t) return;
    gfx_view_origin(&o->v, &ox, &oy);
    x0 = ox + mn.x * o->v.zoom;
    y0 = oy + mn.y * o->v.zoom;
    x1 = ox + mx.x * o->v.zoom;
    y1 = oy + mx.y * o->v.zoom;
    if (x0 < (double)o->clip.x) x0 = (double)o->clip.x;
    if (y0 < (double)o->clip.y) y0 = (double)o->clip.y;
    if (x1 > (double)(o->clip.x + o->clip.w)) x1 = (double)(o->clip.x + o->clip.w);
    if (y1 > (double)(o->clip.y + o->clip.h)) y1 = (double)(o->clip.y + o->clip.h);
    if (!(x1 > x0) || !(y1 > y0)) return;
    rx0 = (int32_t)floor(x0);
    ry0 = (int32_t)floor(y0);
    rx1 = (int32_t)ceil(x1);
    ry1 = (int32_t)ceil(y1);
    w = rx1 - rx0;
    h = ry1 - ry0;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return;
    /* cache key: outline content and its screen mapping */
    key = fnv(key, &p->n_pts, sizeof p->n_pts);
    key = fnv(key, &p->n_contours, sizeof p->n_contours);
    if (p->n_pts) key = fnv(key, p->pts, p->n_pts * sizeof *p->pts);
    if (p->n_contours) key = fnv(key, p->ends, p->n_contours * sizeof *p->ends);
    key = fnv(key, &o->v.zoom, sizeof o->v.zoom);
    key = fnv(key, &ox, sizeof ox);
    key = fnv(key, &oy, sizeof oy);
    key = fnv(key, &rx0, sizeof rx0);
    key = fnv(key, &ry0, sizeof ry0);
    key = fnv(key, &w, sizeof w);
    key = fnv(key, &h, sizeof h);
    if (!t->has || t->key != key) {
        size_t n = (size_t)w * (size_t)h;
        t->has = false;
        if (t->mask.w != w || t->mask.h != h) {
            pc_mask_free(&t->mask);
            if (pc_mask_alloc(&t->mask, pc_rect_make(0, 0, w, h)) != PC_OK) return;
        }
        if (t->rgba_n < n) {
            uint32_t *nb = (uint32_t *)realloc(t->rgba, n * sizeof *nb);
            if (!nb) return;
            t->rgba = nb;
            t->rgba_n = n;
        }
        if (!t->tex || t->tex_w < w || t->tex_h < h) {
            int32_t tw = w > t->tex_w ? w : t->tex_w, th = h > t->tex_h ? h : t->tex_h;
            if (t->tex) SDL_DestroyTexture(t->tex);
            t->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, tw, th);
            if (!t->tex) {
                t->tex_w = t->tex_h = 0;
                return;
            }
            (void)SDL_SetTextureBlendMode(t->tex, SDL_BLENDMODE_BLEND);
            t->tex_w = tw;
            t->tex_h = th;
        }
        m.a = o->v.zoom;
        m.b = 0.0;
        m.c = 0.0;
        m.d = o->v.zoom;
        m.e = ox - (double)rx0;
        m.f = oy - (double)ry0;
        if (pc_raster_fill_poly(p, &m, PC_FILL_NONZERO, true, &t->mask) != PC_OK) return;
        for (int32_t y = 0; y < h; y++) {
            const uint8_t *src = t->mask.px + (size_t)y * (size_t)t->mask.stride;
            uint8_t *dst = (uint8_t *)(t->rgba + (size_t)y * (size_t)w);
            for (int32_t x = 0; x < w; x++) {
                dst[4 * x + 0] = (uint8_t)TINT_R;
                dst[4 * x + 1] = (uint8_t)TINT_G;
                dst[4 * x + 2] = (uint8_t)TINT_B;
                dst[4 * x + 3] = (uint8_t)pc_mul255(src[x], TINT_A);
            }
        }
        {
            SDL_Rect ur;
            ur.x = 0;
            ur.y = 0;
            ur.w = w;
            ur.h = h;
            if (!SDL_UpdateTexture(t->tex, &ur, t->rgba, w * 4)) return;
        }
        t->w = w;
        t->h = h;
        t->x = (float)rx0;
        t->y = (float)ry0;
        t->key = key;
        t->has = true;
    }
    ui_draw_callback(o->ui, tint_cb, t);
}

/* ---- status ------------------------------------------------------------------------------- */
void sel_status_rect(app *a, const app_doc *d, double x, double y, double w, double h,
                     double area_px)
{
    char sx[32], sy[32], sw[32], sh[32], sa[48], buf[200];
    double dpi = d && d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    app_format_len(a, x, dpi, sx, sizeof sx);
    app_format_len(a, y, dpi, sy, sizeof sy);
    app_format_len(a, w, dpi, sw, sizeof sw);
    app_format_len(a, h, dpi, sh, sizeof sh);
    sa[0] = '\0';
    if (area_px >= 0.0) {
        /* 3.36 shows the selected area too: square pixels, or square
         * inches / centimeters with two decimals */
        if (a->units == APP_UNITS_IN)
            snprintf(sa, sizeof sa, " \xC2\xB7 Area %.2f in\xC2\xB2", area_px / (dpi * dpi));
        else if (a->units == APP_UNITS_CM)
            snprintf(sa, sizeof sa, " \xC2\xB7 Area %.2f cm\xC2\xB2",
                     area_px / (dpi * dpi) * 2.54 * 2.54);
        else
            snprintf(sa, sizeof sa, " \xC2\xB7 Area %.0f px\xC2\xB2", floor(area_px + 0.5));
    }
    snprintf(buf, sizeof buf, "Offset %s, %s \xC2\xB7 Size %s \xC3\x97 %s%s", sx, sy, sw, sh, sa);
    app_status(a, buf);
}

/* ---- small math ---------------------------------------------------------------------------- */
double sel_round(double v) { return floor(v + 0.5); }

double sel_clampd(double v)
{
    if (!(v == v)) return 0.0;
    if (v < -1e7) return -1e7;
    if (v > 1e7) return 1e7;
    return v;
}

float sel_dip(const app *a, float dip) { return dip * ui_scale(a->ui); }
