/* drop.c - files dropped on the window (F-WIN-IMG-DND-OPEN) and documents
 * handed over by the OS (macOS Finder "Open With" arrives as drop events
 * without a window).
 *
 * Paint.NET 3.36 (MainForm.OnDragDrop, MIT) asks what to do with dropped
 * files: Open (each file in its own image), Add layers (Import From File
 * into the active image; a new image is created when none is open) or
 * Cancel; folders are ignored. paint.c keeps that question. Adding layers
 * follows MENUS.md Layers > Import From File: each layer of each file goes
 * above the active layer, named "<file name>:<layer name>" (OBSERVED 4.2),
 * the canvas grows to fit larger images (anchored top left, new area
 * transparent) and the last imported layer's bounds end up selected.
 *
 * Thread rules: main thread; the decode job owns its paths and results. */
#include "io_internal.h"
#include "pc/pc_geom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DROP_KEY       "lane_i.drop"
#define DROP_MAX_FILES 256
typedef struct drop_state {
    char  **paths;              /* files of the drop in progress (owned) */
    int     n, cap;
    bool    os_open;            /* no window: documents opened by the OS */
} drop_state;

static void drop_reset(drop_state *s)
{
    for (int i = 0; i < s->n; i++) free(s->paths[i]);
    s->n = 0;
}

static void drop_state_free(void *p)
{
    drop_state *s = (drop_state *)p;
    if (!s) return;
    drop_reset(s);
    free(s->paths);
    free(s);
}

static drop_state *dstate(app *a)
{
    drop_state *s = (drop_state *)app_ext_get(a, DROP_KEY);
    if (s) return s;
    s = (drop_state *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    if (!app_ext_set(a, DROP_KEY, s, drop_state_free)) {
        free(s);
        return NULL;
    }
    return s;
}

/* ---- import as layers ------------------------------------------------------------------ */
typedef struct import_file {
    char     *path;
    pc_doc   *doc;              /* decoded (owned) */
    pc_status st;
    app_load_info info;         /* lane CODEC: what the load found */
} import_file;

typedef struct import_job {
    uint32_t     doc_id;        /* 0 = create a new image first */
    import_file *files;
    int          n;
} import_job;

static void import_free(import_job *j)
{
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        free(j->files[i].path);
        pc_doc_destroy(j->files[i].doc);
    }
    free(j->files);
    free(j);
}

static void import_work(void *ud)
{
    import_job *j = (import_job *)ud;
    for (int i = 0; i < j->n; i++) {
        import_file *f = &j->files[i];
        pc_image_meta m;
        /* lane CODEC (wave 4): the limits of File > Open */
        f->st = app_load_file(f->path, NULL, &f->doc, &m, NULL, &f->info);
        pc_meta_free(&m);
    }
}

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == id) return a->docs[i];
    return NULL;
}

/* "<file name without extension>:<layer name>" within the layer name limit. */
static void layer_name(const char *path, const char *layer, char out[PC_LAYER_NAME_MAX])
{
    const char *base = pal_path_basename(path), *dot = strrchr(base, '.');
    int bl = (int)(dot && dot != base ? (size_t)(dot - base) : strlen(base));
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%.*s:%s", bl, base, layer && *layer ? layer : "Layer");
    app_copy_str(out, PC_LAYER_NAME_MAX, tmp);
}

/* Add every layer of src above the active layer of d. Returns the bounds of
 * the last added layer's image (empty on failure). */
static pc_rect import_one(app *a, app_doc *d, const char *path, const pc_doc *src)
{
    pc_rect r = pc_rect_make(0, 0, 0, 0);
    pc_surf s;
    pc_status st;
    /* grow the canvas to fit (top left, new area transparent) */
    if (src->w > d->doc->w || src->h > d->doc->h) {
        pc_px32 clear;
        memset(&clear, 0, sizeof clear);
        st = pc_geom_canvas_size(d->hist, src->w > d->doc->w ? src->w : d->doc->w,
                                 src->h > d->doc->h ? src->h : d->doc->h, PC_ANCHOR_TOP_LEFT,
                                 clear, &a->par, "Canvas Size");
        if (st != PC_OK) {
            app_error(a, "Could not import \"%s\": %s.", path, pc_status_str(st));
            return r;
        }
        app_doc_history_changed(a, d);
    }
    r = pc_rect_intersect(pc_rect_make(0, 0, (int32_t)src->w, (int32_t)src->h),
                          pc_doc_rect(d->doc));
    if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) {
        app_error(a, "Could not import \"%s\": %s.", path, pc_status_str(PC_ERR_NOMEM));
        return pc_rect_make(0, 0, 0, 0);
    }
    for (uint32_t k = 0; k < src->n_layers; k++) {
        const pc_layer *sl = src->stack[k];
        char name[PC_LAYER_NAME_MAX];
        pc_layer *l;
        int32_t at = app_doc_layer_index(d) + 1;
        layer_name(path, sl->name, name);
        l = pc_layer_create(d->doc, name);
        st = l ? PC_OK : PC_ERR_NOMEM;
        if (st == PC_OK) {
            l->mode = sl->mode;
            l->opacity = sl->opacity;
            l->visible = sl->visible;
            pc_layer_read_rect(src, sl, r, s.px, (size_t)s.stride);
            st = pc_layer_store_rect(d->doc, l, r, s.px, (size_t)s.stride);
        }
        if (st == PC_OK) {
            uint32_t id = l->id;
            if (at < 0) at = (int32_t)d->doc->n_layers;
            st = pc_hist_add_layer(d->hist, l, (uint32_t)at, "Import From File");
            if (st == PC_OK) {
                l = NULL;                       /* owned by the document now */
                app_doc_history_changed(a, d);
                app_doc_set_layer(d, id);
            }
        }
        pc_layer_destroy(l);
        if (st != PC_OK) {
            app_error(a, "Could not import \"%s\": %s.", path, pc_status_str(st));
            r = pc_rect_make(0, 0, 0, 0);
            break;
        }
    }
    pc_surf_free(&s);
    return r;
}

static void import_done(app *a, void *ud)
{
    import_job *j = (import_job *)ud;
    app_doc *d = j->doc_id ? find_doc(a, j->doc_id) : NULL;
    pc_rect last = pc_rect_make(0, 0, 0, 0);
    if (!j->doc_id) {
        /* no image open: a new image of the default size (3.36) */
        float ds = a->win ? SDL_GetWindowDisplayScale(a->win) : 1.0f;
        if (!(ds > 0.0f)) ds = 1.0f;
        d = app_doc_new_image(a, (uint32_t)(800.0f * ds + 0.5f), (uint32_t)(600.0f * ds + 0.5f),
                              app_px_make(255, 255, 255, 255));
        if (d) {
            app_doc_set_untitled(a, d);
            if (!app_add_doc(a, d)) d = NULL;
        }
    }
    if (!d) {
        import_free(j);
        return;
    }
    if (app_active_doc(a) == d) (void)app_tool_finish(a);
    if (d->txn) {
        app_error(a, "Could not import: the image is being edited.");
        import_free(j);
        return;
    }
    for (int i = 0; i < j->n; i++) {
        import_file *f = &j->files[i];
        if (f->st != PC_OK || !f->doc) {
            char msg[1400];
            app_load_error_text(msg, sizeof msg, "import", f->path,
                                f->st == PC_OK ? PC_ERR_FORMAT : f->st, &f->info);
            app_error(a, "%s", msg);
            continue;
        }
        last = import_one(a, d, f->path, f->doc);
    }
    if (!pc_rect_is_empty(last)) {
        /* the imported pixels end up selected, ready for Move Selected Pixels */
        if (pc_sel_apply_rect(d->hist, last, PC_SEL_REPLACE, "Import From File") == PC_OK)
            app_doc_history_changed(a, d);
        if (app_active_doc(a) == d) (void)app_tool_select(a, "move_pixels");
    }
    app_request_frame(a);
    import_free(j);
}

bool app_import_layers(app *a, app_doc *d, const char *const *paths, int n)
{
    import_job *j;
    if (n <= 0) return true;
    j = (import_job *)calloc(1u, sizeof *j);
    if (!j) return false;
    j->doc_id = d ? d->id : 0u;
    j->files = (import_file *)calloc((size_t)n, sizeof *j->files);
    if (!j->files) { free(j); return false; }
    for (int i = 0; i < n; i++) {
        j->files[i].path = app_strdup(paths[i]);
        if (!j->files[i].path) { j->n = i; import_free(j); return false; }
        j->n = i + 1;
    }
    if (!app_task(a, import_work, import_done, j)) {
        import_free(j);
        return false;
    }
    return true;
}

/* ---- the question --------------------------------------------------------------------------- */
typedef struct drop_ask {
    char **paths;
    int    n;
} drop_ask;

static void ask_free(drop_ask *q)
{
    if (!q) return;
    for (int i = 0; i < q->n; i++) free(q->paths[i]);
    free(q->paths);
    free(q);
}

static void ask_done(app *a, int pick, void *ud)
{
    drop_ask *q = (drop_ask *)ud;
    if (pick == 0) app_drop_files(a, (const char *const *)q->paths, q->n, APP_DROP_OPEN);
    else if (pick == 1) app_drop_files(a, (const char *const *)q->paths, q->n, APP_DROP_LAYERS);
    ask_free(q);
}

void app_drop_files(app *a, const char *const *paths, int n, app_drop_action act)
{
    const char *files[DROP_MAX_FILES];
    int k = 0;
    /* folders are ignored (3.36 PruneDirectories) */
    for (int i = 0; i < n && k < DROP_MAX_FILES; i++)
        if (paths[i] && *paths[i] && !pal_is_dir(paths[i])) files[k++] = paths[i];
    if (k == 0) return;
    if (act == APP_DROP_OPEN) {
        app_open_paths(a, files, k);
        if (a->win) SDL_RaiseWindow(a->win);
        return;
    }
    if (act == APP_DROP_LAYERS) {
        if (!app_import_layers(a, app_active_doc(a), files, k))
            app_error(a, "Could not import the files: %s.", pc_status_str(PC_ERR_NOMEM));
        return;
    }
    {
        drop_ask *q = (drop_ask *)calloc(1u, sizeof *q);
        char text[600];
        if (!q) return;
        q->paths = (char **)calloc((size_t)k, sizeof *q->paths);
        if (!q->paths) { free(q); return; }
        for (int i = 0; i < k; i++) {
            q->paths[i] = app_strdup(files[i]);
            if (!q->paths[i]) { ask_free(q); return; }
            q->n = i + 1;
        }
        if (k == 1)
            snprintf(text, sizeof text, "Open \"%s\" as a new image, or add it to %s as new "
                     "layers?", pal_path_basename(files[0]),
                     app_active_doc(a) ? "the current image" : "a new image");
        else
            snprintf(text, sizeof text, "Open the %d dropped files as new images, or add them "
                     "to %s as new layers?", k,
                     app_active_doc(a) ? "the current image" : "a new image");
        app_choice(a, "Open or Add Layers", text, UI_ICON_OPEN, "Open", "Add Layers", "Cancel", 0,
                   2, 0, ask_done, q);
    }
}

/* SDL: DROP_BEGIN, DROP_FILE..., DROP_COMPLETE per drop. A file without a
 * window (windowID 0) is a document the OS asked us to open. */
void app_drop_event(app *a, const SDL_Event *e)
{
    drop_state *s = dstate(a);
    if (!s) return;
    switch (e->type) {
    case SDL_EVENT_DROP_BEGIN:
        drop_reset(s);
        s->os_open = e->drop.windowID == 0;
        break;
    case SDL_EVENT_DROP_FILE:
        if (!e->drop.data || !*e->drop.data) break;
        if (s->n == 0) s->os_open = e->drop.windowID == 0;
        if (s->n == s->cap) {
            int nc = s->cap ? s->cap * 2 : 8;
            char **np = (char **)realloc(s->paths, (size_t)nc * sizeof *np);
            if (!np) break;
            s->paths = np;
            s->cap = nc;
        }
        s->paths[s->n] = app_strdup(e->drop.data);
        if (s->paths[s->n]) s->n++;
        break;
    case SDL_EVENT_DROP_COMPLETE:
        if (s->n > 0)
            app_drop_files(a, (const char *const *)s->paths, s->n,
                           s->os_open ? APP_DROP_OPEN : APP_DROP_ASK);
        drop_reset(s);
        break;
    default:
        break;
    }
}
