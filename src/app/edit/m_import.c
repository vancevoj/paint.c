/* m_import.c - lane M: Layers > Import From File (m_import.h). */
#include "m_import.h"

#include "../app_internal.h"
#include "m_hist.h"
#include "pc/pc_geom.h"
#include "pc/pc_icc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct import_file {
    char          *path;      /* owned */
    pc_doc        *doc;       /* decoded (owned) */
    pc_image_meta  meta;
    pc_status      st;
} import_file;

typedef struct import_job {
    uint32_t     doc_id;
    bool         to_srgb;
    int          n;
    import_file *f;           /* owned, n entries */
} import_job;

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static void import_free(import_job *j)
{
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        free(j->f[i].path);
        pc_doc_destroy(j->f[i].doc);
        pc_meta_free(&j->f[i].meta);
    }
    free(j->f);
    free(j);
}

/* Worker: read and decode every file (touches no app state). */
static void import_work(void *ud)
{
    import_job *j = (import_job *)ud;
    for (int i = 0; i < j->n; i++) {
        import_file *f = &j->f[i];
        uint8_t *data = NULL;
        size_t len = 0;
        pc_codec_limits lim;
        if (f->st != PC_OK) continue;
        f->st = pal_read_file(f->path, (uint64_t)3u << 30, &data, &len);
        if (f->st != PC_OK) continue;
        pc_codec_limits_default(&lim);
        f->st = pc_codec_load_any(data, len, f->path, &lim, &f->doc, &f->meta, NULL);
        free(data);
        if (f->st == PC_OK && (!f->doc || f->doc->n_layers == 0u)) f->st = PC_ERR_FORMAT;
        if (f->st == PC_OK && j->to_srgb && f->meta.icc)
            (void)pc_icc_import(f->doc, &f->meta, NULL);
    }
}

/* "<file name without extension>:<layer name>", cut at a UTF-8 boundary. */
static void import_name(const char *path, const char *layer, char out[PC_LAYER_NAME_MAX])
{
    char buf[512];
    const char *base = pal_path_basename(path);
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n > 300u) n = 300u;
    snprintf(buf, sizeof buf, "%.*s:%s", (int)n, base, layer);
    n = strlen(buf);
    if (n >= PC_LAYER_NAME_MAX) {
        n = PC_LAYER_NAME_MAX - 1u;
        while (n > 0u && ((unsigned char)buf[n] & 0xC0u) == 0x80u) n--;
    }
    memcpy(out, buf, n);
    out[n] = '\0';
}

/* A new layer of d holding the tiles of src. src's document is anchored
 * top left inside d and d is at least as large, so tile (tx, ty) maps to
 * tile (tx, ty) and the zero padding of src's edge tiles stays valid
 * (INV-TILE-EDGE). Tiles are shared (immutable, refcounted). */
static pc_layer *import_layer(app_doc *d, const pc_doc *sdoc, const pc_layer *src,
                              const char *name)
{
    pc_layer *l = pc_layer_create(d->doc, name);
    if (!l) return NULL;
    l->mode = src->mode;
    l->opacity = src->opacity;
    l->visible = src->visible;
    for (uint32_t ty = 0; ty < sdoc->tiles_y && ty < l->tiles_y; ty++)
        for (uint32_t tx = 0; tx < sdoc->tiles_x && tx < l->tiles_x; tx++) {
            pc_tile *t = src->grid[(size_t)ty * sdoc->tiles_x + tx];
            if (!t || t->bpp != 4u) continue;
            pc_tile_retain(t);
            l->grid[(size_t)ty * l->tiles_x + tx] = t;
        }
    return l;
}

static void import_done(app *a, void *ud)
{
    import_job *j = (import_job *)ud;
    app_doc *d = find_doc(a, j->doc_id);
    pc_hist_node *base;
    pc_rect last = pc_rect_make(0, 0, 0, 0);
    bool grown = false, any = false;
    pc_status st = PC_OK;
    for (int i = 0; i < j->n; i++)
        if (j->f[i].st != PC_OK)
            app_error(a, "Could not import \"%s\": %s.", j->f[i].path ? j->f[i].path : "?",
                      pc_status_str(j->f[i].st));
    if (!d) {
        import_free(j);
        return;
    }
    if (d->txn) (void)app_tool_finish(a);
    if (d->txn) {
        app_error(a, "Import From File failed: the image is busy.");
        import_free(j);
        return;
    }
    base = m_hist_mark(d->hist);
    for (int i = 0; i < j->n && st == PC_OK; i++) {
        const import_file *f = &j->f[i];
        if (f->st != PC_OK) continue;
        if (!any && pc_sel_is_active(d->doc)) {
            st = pc_sel_deselect(d->hist, "Deselect");
            if (st != PC_OK) break;
        }
        if (f->doc->w > d->doc->w || f->doc->h > d->doc->h) {
            pc_px32 none;
            memset(&none, 0, sizeof none);
            st = pc_geom_canvas_size(d->hist, d->doc->w > f->doc->w ? d->doc->w : f->doc->w,
                                     d->doc->h > f->doc->h ? d->doc->h : f->doc->h,
                                     PC_ANCHOR_TOP_LEFT, none, &a->par, "Canvas Size");
            if (st != PC_OK) break;
            grown = true;
        }
        for (uint32_t k = 0; k < f->doc->n_layers && st == PC_OK; k++) {
            char name[PC_LAYER_NAME_MAX];
            pc_layer *l;
            uint32_t id;
            import_name(f->path, f->doc->stack[k]->name, name);
            l = import_layer(d, f->doc, f->doc->stack[k], name);
            if (!l) {
                st = PC_ERR_NOMEM;
                break;
            }
            id = l->id;
            st = pc_hist_add_layer(d->hist, l, (uint32_t)(app_doc_layer_index(d) + 1),
                                   "Import From File");
            if (st != PC_OK) {
                pc_layer_destroy(l);
                break;
            }
            app_doc_set_layer(d, id);
            any = true;
        }
        last = pc_rect_make(0, 0, (int32_t)f->doc->w, (int32_t)f->doc->h);
    }
    if (any && st == PC_OK)
        st = pc_sel_apply_rect(d->hist, pc_rect_intersect(last, pc_doc_rect(d->doc)),
                               PC_SEL_REPLACE, "Import From File");
    (void)m_hist_fuse(d->hist, base, "Import From File");
    if (m_hist_depth_from(d->hist, base) > 0) app_doc_history_changed(a, d);
    if (grown) d->view.need_fit = true;
    if (st != PC_OK) app_error(a, "Import From File failed: %s.", pc_status_str(st));
    if (any && app_tool_find(a, "move_pixels")) (void)app_tool_select(a, "move_pixels");
    import_free(j);
}

void m_import_paths(app *a, uint32_t doc_id, const char *const *paths, int n)
{
    app_doc *d = find_doc(a, doc_id);
    import_job *j;
    if (!d || !paths || n <= 0) return;
    j = (import_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->f = (import_file *)calloc((size_t)n, sizeof *j->f);
    if (!j->f) {
        free(j);
        return;
    }
    j->doc_id = doc_id;
    j->to_srgb = d->meta.icc == NULL;
    for (int i = 0; i < n; i++) {
        j->f[i].path = app_strdup(paths[i] ? paths[i] : "");
        j->f[i].st = j->f[i].path ? PC_OK : PC_ERR_NOMEM;
        j->n++;
    }
    if (!app_task(a, import_work, import_done, j)) {
        app_error(a, "Import From File failed: %s.", pc_status_str(PC_ERR_NOMEM));
        import_free(j);
    }
}
