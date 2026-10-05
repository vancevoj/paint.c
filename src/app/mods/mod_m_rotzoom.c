/* mod_m_rotzoom.c - Layers > Rotate / Zoom (Ctrl+Shift+Z), lane M
 * (MENUS.md Layers 10, OBSERVED.md 4.3, RotateZoom docs).
 *
 * Controls (ranges and defaults as observed):
 *   Roll / Rotate  a globe: dragging the outer ring sets the angle (Shift
 *                  snaps to 15 degrees), dragging inside sets the roll
 *                  direction (0 east, 90 south, -90 north) and the tilt
 *                  (center 0, rim 90); three unlabeled slider rows below it:
 *                  angle -180..180, roll direction -180..180, tilt 0..90
 *                  (two decimals, default 0, each with a reset button)
 *   Pan            a pad over a thumbnail of the layer with a crosshair, and
 *                  X / Y rows -10..10 (0 = centered, +-1 = the layer edge)
 *   Zoom           0.06 (1/16) .. 16, logarithmic slider, default 1
 *   Quality        1..8 supersamples, default 1
 *   Tiling Mode    None, Repeat, Mirror (default None)
 *   Sampling       Nearest Neighbor, Bilinear (default Bilinear)
 * The active layer previews live through the document transaction
 * (pc_layerop_rotate_zoom_txn renders the original layer every time a
 * value changes, through the selection like any effect); OK commits one
 * history step "Rotate / Zoom", Cancel and Escape drop it. The last values
 * are remembered for the session.
 *
 * Thread rules: main thread. Ownership: the dialog state (and its
 * thumbnail texture) is owned by the dialog stack. */
#include "../app_internal.h"
#include "../edit/m_ui.h"
#include "pc/pc_layerops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RZ_MEM_KEY "lane_m.rotzoom_memory"
#define RZ_THUMB   160
#define M_PI_VALUE 3.14159265358979323846

static const char *const k_tiling[] = { "None", "Repeat", "Mirror" };
static const char *const k_sampling[] = { "Nearest Neighbor", "Bilinear" };

/* The dialog values in dialog units. */
typedef struct rz_values {
    double angle, roll, tilt;     /* degrees */
    double pan_x, pan_y;          /* -10..10 */
    double zoom;                  /* 1/16..16 */
    double quality;               /* 1..8 */
    int    tiling;                /* index into k_tiling */
    int    sampling;              /* index into k_sampling */
} rz_values;

typedef struct rz_dlg {
    uint32_t     doc_id, layer_id;
    rz_values    v, rendered;     /* current values, values of the preview */
    bool         have_preview;
    bool         failed;          /* preview could not be rendered */
    int          drag;            /* globe drag: 0 none, 1 ring (angle), 2 roll */
    pc_surf      thumb;           /* premultiplied thumbnail of the original layer */
    SDL_Texture *tex;             /* owned */
    app         *a;
} rz_dlg;

static void values_default(rz_values *v)
{
    memset(v, 0, sizeof *v);
    v->zoom = 1.0;
    v->quality = 1.0;
    v->tiling = 0;
    v->sampling = 1;
}

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

/* Dialog values -> engine settings (pc_layerops.h). */
static void to_rotzoom(const rz_values *v, pc_rotzoom *rz)
{
    pc_rotzoom_default(rz);
    rz->angle = v->angle;
    rz->tilt_dir = v->roll;
    rz->tilt = v->tilt > 89.9 ? 89.9 : (v->tilt < 0.0 ? 0.0 : v->tilt);
    rz->pan_x = v->pan_x;
    rz->pan_y = v->pan_y;
    rz->zoom = v->zoom > 0.0 ? v->zoom : 1.0 / 16.0;
    rz->quality = (uint32_t)(v->quality < 1.0 ? 1.0 : (v->quality > 8.0 ? 8.0 : v->quality));
    rz->tiling = v->tiling == 1 ? PC_WRAP_REPEAT : (v->tiling == 2 ? PC_WRAP_MIRROR : PC_WRAP_NONE);
    rz->sampling = v->sampling == 0 ? PC_SAMPLE_NEAREST : PC_SAMPLE_BILINEAR;
}

static void rz_free(void *p)
{
    rz_dlg *g = (rz_dlg *)p;
    if (!g) return;
    /* a dialog dropped without OK (app shutdown) leaves no trace */
    {
        app_doc *d = find_doc(g->a, g->doc_id);
        if (d && d->txn && d->txn_owner == g) app_doc_txn_cancel(g->a, d);
    }
    if (g->tex) SDL_DestroyTexture(g->tex);
    pc_surf_free(&g->thumb);
    free(g);
}

/* Thumbnail of the published layer by nearest sampling (cheap for huge
 * layers), premultiplied for linear filtering. */
static void make_thumb(const pc_doc *doc, const pc_layer *l, pc_surf *out)
{
    double k = (double)RZ_THUMB / (double)(doc->w > doc->h ? doc->w : doc->h);
    int32_t tw, th;
    if (k > 1.0) k = 1.0;
    tw = (int32_t)((double)doc->w * k + 0.5);
    th = (int32_t)((double)doc->h * k + 0.5);
    if (pc_surf_alloc(out, tw < 1 ? 1 : tw, th < 1 ? 1 : th) != PC_OK) return;
    for (int32_t y = 0; y < out->h; y++) {
        uint32_t sy = (uint32_t)(((double)y + 0.5) / k);
        if (sy >= doc->h) sy = doc->h - 1u;
        for (int32_t x = 0; x < out->w; x++) {
            uint32_t sx = (uint32_t)(((double)x + 0.5) / k);
            pc_px32 c;
            if (sx >= doc->w) sx = doc->w - 1u;
            c = pc_layer_get_px(l, sx, sy);
            c.r = (uint8_t)pc_mul255(c.r, c.a);
            c.g = (uint8_t)pc_mul255(c.g, c.a);
            c.b = (uint8_t)pc_mul255(c.b, c.a);
            pc_surf_row(out, y)[x] = c;
        }
    }
}

/* ---- the Roll / Rotate globe ------------------------------------------------------------ */
static void globe(app *a, rz_dlg *g, float size_dip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t size = ui_px(ui, size_dip);
    ui_rect r = ui_layout_next(ui, size, size);
    ui_interaction in;
    float cx, cy, ro, ri, ring;
    double ar;
    r.w = r.h = size;
    in = ui_interact(ui, ui_get_id(ui, "##globe"), r, UI_INTERACT_FOCUSABLE);
    cx = (float)r.x + (float)size * 0.5f;
    cy = (float)r.y + (float)size * 0.5f;
    ro = (float)size * 0.5f - 1.0f;
    ring = (float)ui_px(ui, 12.0f);
    ri = ro - ring;
    if (in.pressed) {
        float dx = in.mouse.x - cx, dy = in.mouse.y - cy;
        g->drag = sqrtf(dx * dx + dy * dy) > ri ? 1 : 2;
    }
    if (in.held && g->drag) {
        double dx = (double)(in.mouse.x - cx), dy = (double)(in.mouse.y - cy);
        if (g->drag == 1) {
            double ang = atan2(-dy, dx) * 180.0 / M_PI_VALUE;
            if (ui_mods(ui) & UI_MOD_SHIFT) ang = floor(ang / 15.0 + 0.5) * 15.0;
            if (ang > 180.0) ang -= 360.0;
            if (ang < -180.0) ang += 360.0;
            g->v.angle = floor(ang * 100.0 + 0.5) / 100.0;
        } else {
            double dist = sqrt(dx * dx + dy * dy), t = dist / (double)ri * 90.0;
            g->v.roll = floor(atan2(dy, dx) * 180.0 / M_PI_VALUE * 100.0 + 0.5) / 100.0;
            g->v.tilt = floor((t > 90.0 ? 90.0 : t) * 100.0 + 0.5) / 100.0;
        }
        ui_set_cursor(ui, UI_CURSOR_CROSSHAIR);
    }
    if (!in.held) g->drag = 0;
    /* outer ring */
    ui_draw_circle(ui, ui_vec2_make(cx, cy), ro, in.hovered ? p->raised_hover : p->raised);
    ui_draw_circle_outline(ui, ui_vec2_make(cx, cy), ro, 1.0f, p->border_strong);
    /* inner disc and the tilted plane: an ellipse squashed along the roll
     * direction by cos(tilt) */
    ui_draw_circle(ui, ui_vec2_make(cx, cy), ri, p->field);
    ui_draw_circle_outline(ui, ui_vec2_make(cx, cy), ri, 1.0f, p->border);
    {
        ui_vec2 pts[48];
        double dir = g->v.roll * M_PI_VALUE / 180.0, c = cos(g->v.tilt * M_PI_VALUE / 180.0);
        for (int i = 0; i < 48; i++) {
            double t = (double)i / 48.0 * 2.0 * M_PI_VALUE;
            double u = cos(t) * (double)ri * 0.92 * c, w = sin(t) * (double)ri * 0.92;
            pts[i].x = cx + (float)(u * cos(dir) - w * sin(dir));
            pts[i].y = cy + (float)(u * sin(dir) + w * cos(dir));
        }
        ui_draw_polyline(ui, pts, 48, true, 1.5f, p->accent);
    }
    /* reticle at the roll direction / tilt */
    ar = g->v.roll * M_PI_VALUE / 180.0;
    {
        float rr = ri * (float)(g->v.tilt / 90.0);
        ui_vec2 q = ui_vec2_make(cx + rr * (float)cos(ar), cy + rr * (float)sin(ar));
        ui_draw_line(ui, ui_vec2_make(cx, cy), q, 1.0f, p->text_dim);
        ui_draw_circle(ui, q, (float)ui_px(ui, 4.0f), p->accent);
    }
    /* angle marker on the ring (0 east, counter-clockwise positive) */
    {
        double an = g->v.angle * M_PI_VALUE / 180.0;
        float rm = ri + ring * 0.5f;
        ui_vec2 m = ui_vec2_make(cx + rm * (float)cos(an), cy - rm * (float)sin(an));
        ui_draw_line(ui, ui_vec2_make(cx + ri * (float)cos(an), cy - ri * (float)sin(an)),
                     ui_vec2_make(cx + ro * (float)cos(an), cy - ro * (float)sin(an)), 2.0f,
                     p->accent);
        ui_draw_circle(ui, m, (float)ui_px(ui, 3.5f), p->accent);
    }
    if (in.hovered && !in.held)
        ui_tooltip(ui, "Drag the ring to rotate (Shift snaps to 15 degrees); drag inside to "
                       "tilt the layer.");
}

/* ---- the dialog -------------------------------------------------------------------------- */
static bool same(const rz_values *x, const rz_values *y)
{
    return memcmp(x, y, sizeof *x) == 0;
}

static void preview(app *a, rz_dlg *g, app_doc *d)
{
    pc_rotzoom rz;
    pc_status st;
    if (g->have_preview && same(&g->v, &g->rendered)) return;
    if (!d->txn) {
        if (!app_doc_txn_begin(a, d, g, "Rotate / Zoom")) return;
    } else if (d->txn_owner != g) {
        return;
    }
    to_rotzoom(&g->v, &rz);
    st = pc_layerop_rotate_zoom_txn(d->txn, g->layer_id, &rz, &a->par);
    g->failed = st != PC_OK;
    g->rendered = g->v;
    g->have_preview = true;
    app_request_frame(a);
}

static bool rz_frame(app *a, void *st)
{
    rz_dlg *g = (rz_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = find_doc(a, g->doc_id);
    ui_size cells[2];
    uint32_t r;
    bool enter;
    if (!d || !pc_doc_layer_by_id(d->doc, g->layer_id)) return false;
    ui_dialog_begin(ui, "Rotate / Zoom##rotzoom", 520.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    cells[0] = ui_size_px(170.0f);
    cells[1] = ui_size_fr(1.0f);

    ui_heading(ui, "Roll / Rotate");
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_layout_begin(ui, 0.0f);
    globe(a, g, 150.0f);
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    ui_layout_space(ui, 14.0f);
    (void)m_slider_row(a, "##angle", NULL, &g->v.angle, -180.0, 180.0, 0.0, 1.0, 2, 0);
    (void)m_slider_row(a, "##roll", NULL, &g->v.roll, -180.0, 180.0, 0.0, 1.0, 2, 0);
    (void)m_slider_row(a, "##tilt", NULL, &g->v.tilt, 0.0, 90.0, 0.0, 1.0, 2, 0);
    ui_layout_end(ui);
    ui_layout_column(ui);

    ui_heading(ui, "Pan");
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_layout_begin(ui, 0.0f);
    {
        ui_vec2 pt;
        float hd = 150.0f;
        if (!g->tex && g->thumb.px && a->ren) {
            g->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC,
                                       g->thumb.w, g->thumb.h);
            if (g->tex) {
                SDL_UpdateTexture(g->tex, NULL, g->thumb.px, g->thumb.stride * 4);
                SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            }
        }
        if (g->thumb.w > 0 && g->thumb.h > 0) {
            hd = 150.0f * (float)g->thumb.h / (float)g->thumb.w;
            if (hd > 150.0f) hd = 150.0f;
            if (hd < 40.0f) hd = 40.0f;
        }
        pt.x = (float)(g->v.pan_x < -1.0 ? -1.0 : (g->v.pan_x > 1.0 ? 1.0 : g->v.pan_x));
        pt.y = (float)(g->v.pan_y < -1.0 ? -1.0 : (g->v.pan_y > 1.0 ? 1.0 : g->v.pan_y));
        if (ui_point_picker(ui, "##panpad", &pt, g->tex, hd)) {
            g->v.pan_x = floor((double)pt.x * 100.0 + 0.5) / 100.0;
            g->v.pan_y = floor((double)pt.y * 100.0 + 0.5) / 100.0;
        }
    }
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    ui_layout_space(ui, 28.0f);
    (void)m_slider_row(a, "##panx", "X", &g->v.pan_x, -10.0, 10.0, 0.0, 0.01, 2, 0);
    (void)m_slider_row(a, "##pany", "Y", &g->v.pan_y, -10.0, 10.0, 0.0, 0.01, 2, 0);
    ui_layout_end(ui);
    ui_layout_column(ui);

    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Zoom", UI_LABEL_DIM);
    ui_layout_begin(ui, 0.0f);
    (void)m_slider_row(a, "##zoom", NULL, &g->v.zoom, 1.0 / 16.0, 16.0, 1.0, 0.01, 2,
                       UI_SLIDER_LOG);
    ui_layout_end(ui);
    ui_label_ex(ui, "Quality", UI_LABEL_DIM);
    ui_layout_begin(ui, 0.0f);
    if (m_slider_row(a, "##quality", NULL, &g->v.quality, 1.0, 8.0, 1.0, 1.0, 0, 0))
        g->v.quality = floor(g->v.quality + 0.5);
    ui_layout_end(ui);
    ui_label_ex(ui, "Tiling Mode", UI_LABEL_DIM);
    (void)ui_combo(ui, "##tiling", &g->v.tiling, k_tiling, 3);
    ui_label_ex(ui, "Sampling", UI_LABEL_DIM);
    (void)ui_combo(ui, "##sampling", &g->v.sampling, k_sampling, 2);
    ui_layout_column(ui);
    if (g->failed) ui_label_ex(ui, "The preview could not be rendered (out of memory).", 0);

    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) {
        preview(a, g, d);
        return true;
    }
    {
        rz_values *mem = (rz_values *)app_ext_get(a, RZ_MEM_KEY);
        if (r == UI_DLG_OK) {
            preview(a, g, d);
            if (mem) *mem = g->v;
            if (d->txn && d->txn_owner == g) {
                pc_status s2 = app_doc_txn_commit(a, d);
                if (s2 != PC_OK)
                    app_error(a, "Rotate / Zoom failed: %s.", pc_status_str(s2));
            }
        } else if (d->txn && d->txn_owner == g) {
            app_doc_txn_cancel(a, d);
        }
    }
    return false;
}

static void cmd_rotzoom(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    rz_values *mem = (rz_values *)app_ext_get(a, RZ_MEM_KEY);
    rz_dlg *g;
    (void)c;
    if (!l || d->txn) return;
    if (!mem) {
        mem = (rz_values *)malloc(sizeof *mem);
        if (mem) {
            values_default(mem);
            if (!app_ext_set(a, RZ_MEM_KEY, mem, free)) {
                free(mem);
                mem = NULL;
            }
        }
    }
    g = (rz_dlg *)calloc(1u, sizeof *g);
    if (!g) return;
    g->a = a;
    g->doc_id = d->id;
    g->layer_id = l->id;
    if (mem) g->v = *mem;
    else values_default(&g->v);
    make_thumb(d->doc, l, &g->thumb);
    if (!app_dialog_push(a, rz_frame, g, rz_free)) return;
    preview(a, g, d);
}

void mod_m_rotzoom(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "layers.rotate_zoom";
    d.label = "Rotate / Zoom...";
    d.icon = UI_ICON_ROTATE_CW;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = cmd_rotzoom;
    (void)app_cmd_register(a, &d);
}
