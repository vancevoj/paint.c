/* mod_m_profile.c - Image > Color Profile, lane M (MENUS.md Image 9 and
 * "Color Profile dialog (5.1)", ImageMenu docs).
 *
 * The dialog shows the image's current profile, offers the new profile
 * (the current image profile, the built-in sRGB, Adobe RGB (1998), Display
 * P3 and ProPhoto RGB, or an imported .icc / .icm file), and has Import,
 * Export (writes the current profile), Assign, Convert and Close. Assign
 * and Convert are one history step each (edit/m_profile.h). Lane SHELL:
 * the display's own profile is offered too when the platform reports one
 * (SDL_GetWindowICCProfile through shell_cm.c: Windows, macOS and X11;
 * Wayland reports none).
 *
 * Thread rules: main thread; Convert transforms tiles on the worker pool.
 * Ownership: dialog state is owned by the dialog stack; an imported
 * profile waiting for the dialog lives in the app (app_ext). */
#include "../app_internal.h"
#include "../edit/m_hist.h"
#include "../edit/m_icc.h"
#include "../edit/m_profile.h"
#include "../shell_ext.h"
#include "pc/pc_icc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- undoable profile tag --------------------------------------------------------------- */
typedef struct icc_payload {
    app_doc *d;               /* borrowed: the history belongs to this document */
    uint8_t *icc;             /* owned: the other state */
    size_t   len;
} icc_payload;

static void icc_swap(pc_doc *doc, void *p)
{
    icc_payload *v = (icc_payload *)p;
    uint8_t *ti = v->d->meta.icc;
    size_t tl = v->d->meta.icc_len;
    (void)doc;
    v->d->meta.icc = v->icc;
    v->d->meta.icc_len = v->len;
    v->icc = ti;
    v->len = tl;
}

static void icc_destroy(void *p)
{
    icc_payload *v = (icc_payload *)p;
    if (!v) return;
    free(v->icc);
    free(v);
}

static size_t icc_bytes(const void *p)
{
    const icc_payload *v = (const icc_payload *)p;
    return sizeof *v + v->len;
}

static const pc_hist_ops k_icc_ops = { icc_swap, icc_destroy, icc_bytes };

static bool same_profile(const app_doc *d, const uint8_t *icc, size_t len)
{
    if (!icc || !len) return !d->meta.icc || !d->meta.icc_len;
    return d->meta.icc && d->meta.icc_len == len && memcmp(d->meta.icc, icc, len) == 0;
}

/* Link a history node that sets the profile to a copy of icc. */
static pc_status set_icc(app_doc *d, const uint8_t *icc, size_t len, const char *label)
{
    icc_payload *v;
    pc_hist_node *n;
    if (d->doc->open_txns) return PC_ERR_STATE;
    v = (icc_payload *)calloc(1u, sizeof *v);
    n = pc_hist_node_new(label);
    if (v && icc && len) {
        v->icc = (uint8_t *)malloc(len);
        if (v->icc) {
            memcpy(v->icc, icc, len);
            v->len = len;
        }
    }
    if (!v || !n || (icc && len && !v->icc)) {
        if (v) free(v->icc);
        free(v);
        pc_hist_node_free_unlinked(n);
        return PC_ERR_NOMEM;
    }
    v->d = d;
    icc_swap(d->doc, v);              /* apply == swap */
    pc_hist_link(d->hist, n, &k_icc_ops, v);
    return PC_OK;
}

static bool busy(app *a, app_doc *d)
{
    if (d->txn) (void)app_tool_finish(a);
    return d->txn != NULL || d->doc->open_txns != 0u;
}

pc_status m_profile_assign(app *a, app_doc *d, const uint8_t *icc, size_t len)
{
    pc_status st;
    if (!d) return PC_ERR_ARG;
    if (icc && len) {
        pc_icc_info info;
        st = pc_icc_inspect(icc, len, &info);
        if (st != PC_OK) return st;
    }
    if (same_profile(d, icc, len)) return PC_ERR_STATE;
    if (busy(a, d)) return PC_ERR_STATE;
    st = set_icc(d, icc, len, "Assign Color Profile");
    if (st == PC_OK) app_doc_history_changed(a, d);
    return st;
}

/* ---- Convert ------------------------------------------------------------------------------- */
typedef struct conv_job {
    uint8_t      **tiles;      /* private tile data of the transaction */
    uint32_t       n;
    const m_icc_xform *x;      /* NULL: the profile step alone */
    const uint8_t *src_icc;    /* non-NULL: convert to sRGB with Little-CMS first */
    size_t         src_len;
    pc_status      st;         /* first failure of the Little-CMS step */
} conv_job;

static void conv_tile(void *ud, uint32_t i, uint32_t worker)
{
    conv_job *j = (conv_job *)ud;
    pc_px32 *px = (pc_px32 *)(void *)j->tiles[i];
    (void)worker;
    if (j->src_icc) {
        pc_status st = pc_icc_to_srgb_px(j->src_icc, j->src_len, px, (int32_t)PC_TILE_DIM,
                                         (int32_t)PC_TILE_DIM, PC_TILE_DIM);
        if (st != PC_OK) j->st = st;
    }
    if (j->x) m_icc_xform_px(j->x, px, PC_TILE_PX);
}

pc_status m_profile_convert(app *a, app_doc *d, const uint8_t *icc, size_t len)
{
    m_icc_rgb src, dst;
    m_icc_xform *x = NULL;
    conv_job job;
    pc_hist_node *base;
    pc_txn *t = NULL;
    size_t cap = 0;
    pc_status st;
    bool via_lcms = false, dst_srgb = !icc || !len;
    if (!d) return PC_ERR_ARG;
    if (same_profile(d, icc, len)) return PC_ERR_STATE;
    if (dst_srgb) m_icc_builtin_rgb(M_ICC_SRGB, &dst);
    else if (m_icc_parse(icc, len, &dst) != PC_OK) return PC_ERR_UNSUPPORTED;
    if (!d->meta.icc || !d->meta.icc_len) {
        m_icc_builtin_rgb(M_ICC_SRGB, &src);
    } else if (m_icc_parse(d->meta.icc, d->meta.icc_len, &src) != PC_OK) {
        /* LUT-based or gray source: Little-CMS to sRGB, then sRGB -> dst */
        pc_icc_info info;
        st = pc_icc_inspect(d->meta.icc, d->meta.icc_len, &info);
        if (st != PC_OK) return st;
        via_lcms = true;
        m_icc_builtin_rgb(M_ICC_SRGB, &src);
    }
    if (busy(a, d)) return PC_ERR_STATE;
    memset(&job, 0, sizeof job);
    if (!(via_lcms && dst_srgb)) {
        x = (m_icc_xform *)malloc(sizeof *x);
        if (!x) return PC_ERR_NOMEM;
        if (!m_icc_xform_init(x, &src, &dst)) {
            free(x);
            return PC_ERR_UNSUPPORTED;
        }
    }
    job.x = x;
    if (via_lcms) {
        job.src_icc = d->meta.icc;
        job.src_len = d->meta.icc_len;
    }
    base = m_hist_mark(d->hist);
    t = pc_txn_begin(d->doc, "Convert Color Profile");
    st = t ? PC_OK : PC_ERR_NOMEM;
    /* every non-empty tile of every layer (empty tiles are transparent and
     * stay transparent) */
    for (uint32_t li = 0; st == PC_OK && li < d->doc->n_layers; li++) {
        const pc_layer *l = d->doc->stack[li];
        size_t nt = (size_t)l->tiles_x * l->tiles_y;
        for (size_t ti = 0; st == PC_OK && ti < nt; ti++) {
            uint8_t *p;
            if (!l->grid[ti] || l->grid[ti]->bpp != 4u) continue;
            if (job.n == cap) {
                size_t nc = cap ? cap * 2u : 256u;
                uint8_t **nb = (uint8_t **)realloc(job.tiles, nc * sizeof *nb);
                if (!nb) {
                    st = PC_ERR_NOMEM;
                    break;
                }
                job.tiles = nb;
                cap = nc;
            }
            p = pc_txn_tile_rw(t, l->id, (uint32_t)ti);
            if (!p) st = PC_ERR_NOMEM;
            else job.tiles[job.n++] = p;
        }
    }
    if (st == PC_OK && job.n) {
        pc_par_for(&a->par, conv_tile, &job, job.n);
        st = job.st;
    }
    free(job.tiles);
    free(x);
    if (st != PC_OK) {
        if (t) pc_txn_cancel(t);
        return st;
    }
    st = pc_txn_commit(t, d->hist);
    if (st == PC_OK) st = set_icc(d, dst_srgb ? NULL : icc, dst_srgb ? 0u : len,
                                  "Convert Color Profile");
    (void)m_hist_fuse(d->hist, base, "Convert Color Profile");
    if (m_hist_depth_from(d->hist, base) > 0) app_doc_history_changed(a, d);
    return st;
}

void m_profile_describe(const app_doc *d, char *out, size_t cap)
{
    pc_icc_info info;
    if (!d || !d->meta.icc || !d->meta.icc_len) {
        snprintf(out, cap, "%s (assumed, no profile embedded)", m_icc_builtin_name(M_ICC_SRGB));
        return;
    }
    if (pc_icc_inspect(d->meta.icc, d->meta.icc_len, &info) != PC_OK) {
        snprintf(out, cap, "Unreadable embedded profile");
        return;
    }
    snprintf(out, cap, "%s%s", info.desc[0] ? info.desc : "Unnamed profile",
             info.is_srgb ? " (equivalent to sRGB)" : "");
}

/* ---- the dialog -------------------------------------------------------------------------- */
#define IMPORT_KEY "lane_m.icc_import"

typedef struct icc_blob { uint8_t *p; size_t n; char name[160]; } icc_blob;

static void blob_free(void *v)
{
    icc_blob *b = (icc_blob *)v;
    if (!b) return;
    free(b->p);
    free(b);
}

typedef struct profile_dlg {
    uint32_t doc_id;
    int      choice;          /* 0 current, 1..4 built-ins, 5 imported, 6 display */
    icc_blob imported;        /* owned bytes */
    char     msg[200];
    app     *a;               /* borrowed (lane SHELL: display profile lookups) */
} profile_dlg;

static void profile_free(void *p)
{
    profile_dlg *g = (profile_dlg *)p;
    if (!g) return;
    free(g->imported.p);
    free(g);
}

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

typedef struct io_req { app *a; uint8_t *p; size_t n; } io_req;

static void import_cb(void *ud, const char *const *paths, int n, int filter)
{
    io_req *r = (io_req *)ud;
    (void)filter;
    if (paths && n > 0) {
        icc_blob *b = (icc_blob *)calloc(1u, sizeof *b);
        pc_icc_info info;
        pc_status st = b ? pal_read_file(paths[0], PC_ICC_MAX_BYTES, &b->p, &b->n) : PC_ERR_NOMEM;
        if (st == PC_OK) st = pc_icc_inspect(b->p, b->n, &info);
        if (st == PC_OK && info.space != PC_ICC_SPACE_RGB) st = PC_ERR_UNSUPPORTED;
        if (st != PC_OK) {
            app_error(r->a, "Could not import \"%s\": %s.", paths[0], pc_status_str(st));
            blob_free(b);
        } else {
            snprintf(b->name, sizeof b->name, "%s", info.desc[0] ? info.desc
                                                                 : pal_path_basename(paths[0]));
            if (!app_ext_set(r->a, IMPORT_KEY, b, blob_free)) blob_free(b);
        }
    }
    free(r);
}

static void export_cb(void *ud, const char *const *paths, int n, int filter)
{
    io_req *r = (io_req *)ud;
    (void)filter;
    if (paths && n > 0) {
        pc_status st = pal_write_file_atomic(paths[0], r->p, r->n);
        if (st != PC_OK)
            app_error(r->a, "Could not export the profile: %s.", pc_status_str(st));
    }
    free(r->p);
    free(r);
}

/* The bytes of the selected profile (NULL = sRGB / none); built-ins are
 * generated into *tmp (free()). */
static pc_status selected_bytes(profile_dlg *g, app_doc *d, const uint8_t **icc, size_t *len,
                                uint8_t **tmp)
{
    *tmp = NULL;
    *icc = NULL;
    *len = 0;
    if (g->choice == 0) {
        *icc = d->meta.icc;
        *len = d->meta.icc_len;
        return PC_OK;
    }
    if (g->choice == 5) {
        if (!g->imported.p) return PC_ERR_STATE;
        *icc = g->imported.p;
        *len = g->imported.n;
        return PC_OK;
    }
    if (g->choice == 6) {                        /* the display's profile (lane SHELL) */
        *icc = app_cm_display_profile(g->a, len);
        return *icc ? PC_OK : PC_ERR_STATE;
    }
    if (g->choice == 1) return PC_OK;            /* sRGB: no embedded profile */
    {
        pc_status st = m_icc_builtin_profile((m_icc_builtin)(g->choice - 1), tmp, len);
        *icc = *tmp;
        return st;
    }
}

static bool profile_frame(app *a, void *st)
{
    profile_dlg *g = (profile_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = find_doc(a, g->doc_id);
    const char *items[7];
    int kinds[7];
    char cur[200], buf[240], disp_name[200], ddesc[160];
    ui_size cells[4];
    int n = 5, action = 0, sel = 0;
    uint32_t r;
    if (!d) return false;
    g->a = a;
    {
        icc_blob *b = (icc_blob *)app_ext_get(a, IMPORT_KEY);
        if (b && b->p) {          /* a profile arrived from the Import dialog */
            free(g->imported.p);
            g->imported = *b;
            b->p = NULL;
            (void)app_ext_set(a, IMPORT_KEY, NULL, NULL);
            g->choice = 5;
        }
    }
    m_profile_describe(d, cur, sizeof cur);
    snprintf(buf, sizeof buf, "Current image profile: %s", cur);
    items[0] = buf;
    kinds[0] = 0;
    for (int i = 0; i < 4; i++) {
        items[i + 1] = m_icc_builtin_name((m_icc_builtin)i);
        kinds[i + 1] = i + 1;
    }
    if (g->imported.p) {
        kinds[n] = 5;
        items[n++] = g->imported.name;
    }
    app_cm_display_describe(a, ddesc, sizeof ddesc);
    if (ddesc[0]) {
        snprintf(disp_name, sizeof disp_name, "Display: %s", ddesc);
        kinds[n] = 6;
        items[n++] = disp_name;
    } else if (g->choice == 6) {
        g->choice = 0;                           /* the display profile went away */
    }
    for (int i = 0; i < n; i++)
        if (kinds[i] == g->choice) sel = i;
    ui_dialog_begin(ui, "Color Profile##colorprofile", 560.0f, 0.0f);
    ui_heading(ui, "Current profile");
    ui_label(ui, cur);
    ui_layout_space(ui, 8.0f);
    ui_heading(ui, "Profile");
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 2, cells);
    if (ui_combo(ui, "##choice", &sel, items, n) && sel >= 0 && sel < n) g->choice = kinds[sel];
    if (ui_button_ex(ui, "Import...##iccimport", UI_ICON_OPEN, 0)) {
        io_req *rq = (io_req *)calloc(1u, sizeof *rq);
        if (rq && a->win) {
            pal_filter f[2] = { { "ICC color profiles", "icc;icm" }, { "All files", "*" } };
            rq->a = a;
            pal_dialog_open(a->win, f, 2, NULL, false, import_cb, rq);
        } else {
            free(rq);
        }
    }
    ui_layout_column(ui);
    ui_text_wrapped(ui, "Convert changes the pixels so the image keeps its appearance in the "
                        "new profile (colors outside it are clipped). Assign only replaces the "
                        "profile; the pixel values stay and the appearance changes.",
                    UI_LABEL_DIM);
    if (g->msg[0]) ui_label_ex(ui, g->msg, UI_LABEL_DIM);
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_auto();
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_auto();
    cells[3] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 4, cells);
    if (ui_button_ex(ui, "Export...##iccexport", UI_ICON_SAVE, 0)) action = 3;
    (void)ui_layout_next(ui, 0, ui_px(ui, 28.0f));
    if (ui_button_ex(ui, "Assign##iccassign", UI_ICON_NONE, 0)) action = 1;
    if (ui_button_ex(ui, "Convert##iccconvert", UI_ICON_NONE, 0)) action = 2;
    ui_layout_column(ui);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    if (action == 3) {
        /* the current profile; sRGB when none is embedded */
        io_req *rq = (io_req *)calloc(1u, sizeof *rq);
        pc_status s2 = rq ? PC_OK : PC_ERR_NOMEM;
        if (rq && d->meta.icc && d->meta.icc_len) {
            rq->p = (uint8_t *)malloc(d->meta.icc_len);
            if (rq->p) {
                memcpy(rq->p, d->meta.icc, d->meta.icc_len);
                rq->n = d->meta.icc_len;
            } else {
                s2 = PC_ERR_NOMEM;
            }
        } else if (rq) {
            s2 = m_icc_builtin_profile(M_ICC_SRGB, &rq->p, &rq->n);
        }
        if (s2 == PC_OK && a->win) {
            pal_filter f[1] = { { "ICC color profile", "icc" } };
            char def[300];
            snprintf(def, sizeof def, "%s.icc", d->name);
            rq->a = a;
            pal_dialog_save(a->win, f, 1, def, export_cb, rq);
        } else {
            if (rq) free(rq->p);
            free(rq);
            if (s2 != PC_OK) app_error(a, "Export failed: %s.", pc_status_str(s2));
        }
    } else if (action == 1 || action == 2) {
        const uint8_t *icc;
        size_t len;
        uint8_t *tmp;
        pc_status s2 = selected_bytes(g, d, &icc, &len, &tmp);
        if (s2 == PC_OK)
            s2 = action == 1 ? m_profile_assign(a, d, icc, len) : m_profile_convert(a, d, icc, len);
        free(tmp);
        if (s2 == PC_OK) {
            snprintf(g->msg, sizeof g->msg, "%s.", action == 1 ? "Assigned" : "Converted");
            g->choice = 0;
        } else if (s2 == PC_ERR_STATE) {
            snprintf(g->msg, sizeof g->msg, "The image already uses this profile.");
        } else if (s2 == PC_ERR_UNSUPPORTED) {
            snprintf(g->msg, sizeof g->msg,
                     "Only matrix-based RGB profiles can be converted to; use Assign.");
        } else {
            app_error(a, "Color Profile failed: %s.", pc_status_str(s2));
        }
    }
    return r == 0;
}

static void cmd_profile(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    profile_dlg *g;
    (void)c;
    if (!d) return;
    g = (profile_dlg *)calloc(1u, sizeof *g);
    if (!g) return;
    g->doc_id = d->id;
    (void)app_dialog_push(a, profile_frame, g, profile_free);
}

void mod_m_profile(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "image.color_profile";
    d.label = "Color Profile...";
    d.icon = UI_ICON_PALETTE;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = cmd_profile;
    (void)app_cmd_register(a, &d);
}
