/* fileio.c - File menu flows (MENUS.md File, FILES.md), lane I:
 *  - Open: native dialog with Paint.NET's type list (multi select), decoding
 *    on a worker (content sniffing, FL-SNIFF), errors with the path and
 *    pc_status_str (FL-ERR), already open files are activated;
 *  - Save / Save As / Save All: native save dialog, the file type from the
 *    typed extension or the type filter, Save Configuration (FS-CONFIG:
 *    options from the codec's fx_prop list, live preview of the re-opened
 *    result with zoom and pan, file size, options remembered per image and
 *    per type), then the Flatten prompt (FS-FLATTEN, an undoable history
 *    step; OBSERVED 3.3 shows it after the options), then encoding of a
 *    retained snapshot on a worker and an atomic replace (FS-ATOMIC);
 *  - Close with the unsaved changes prompt; Close All and Exit with the
 *    list of unsaved images (3.36 UnsavedChangesDialog semantics);
 *  - New Image (sizes, resolution, print size, remembered choices).
 *
 * Thread rules: main thread; workers only see owned job data (snapshots
 * share immutable tiles). Ownership: flows own their state until their done
 * callback ran. */
#include "app_internal.h"
#include "edit/m_size.h"
#include "edit/m_ui.h"
#include "io/io_internal.h"
#include "pc/pc_icc.h"
#include "pc/pc_layerops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_MAX_SIDE 2048
#define FILE_STATE_KEY   "lane_i.fileio"

/* ---- per-app state ----------------------------------------------------------------- */
typedef struct file_state {
    char  **opening;            /* paths being decoded (owned strings) */
    int32_t nopening, cap_opening;
} file_state;

static void file_state_free(void *p)
{
    file_state *s = (file_state *)p;
    if (!s) return;
    for (int32_t i = 0; i < s->nopening; i++) free(s->opening[i]);
    free(s->opening);
    free(s);
}

static file_state *fstate(app *a)
{
    file_state *s = (file_state *)app_ext_get(a, FILE_STATE_KEY);
    if (s) return s;
    s = (file_state *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    if (!app_ext_set(a, FILE_STATE_KEY, s, file_state_free)) {
        free(s);
        return NULL;
    }
    return s;
}

static bool opening_has(app *a, const char *path)
{
    file_state *s = fstate(a);
    if (!s) return false;
    for (int32_t i = 0; i < s->nopening; i++)
        if (strcmp(s->opening[i], path) == 0) return true;
    return false;
}

static bool opening_add(app *a, const char *path)
{
    file_state *s = fstate(a);
    char *p;
    if (!s) return false;
    if (s->nopening == s->cap_opening) {
        int32_t nc = s->cap_opening ? s->cap_opening * 2 : 8;
        char **n = (char **)realloc(s->opening, (size_t)nc * sizeof *n);
        if (!n) return false;
        s->opening = n;
        s->cap_opening = nc;
    }
    p = app_strdup(path);
    if (!p) return false;
    s->opening[s->nopening++] = p;
    return true;
}

static void opening_remove(app *a, const char *path)
{
    file_state *s = fstate(a);
    if (!s) return;
    for (int32_t i = 0; i < s->nopening; i++)
        if (strcmp(s->opening[i], path) == 0) {
            free(s->opening[i]);
            memmove(&s->opening[i], &s->opening[i + 1],
                    (size_t)(s->nopening - i - 1) * sizeof *s->opening);
            s->nopening--;
            return;
        }
}

int app_opening_count(const app *a)
{
    const file_state *s = (const file_state *)app_ext_get(a, FILE_STATE_KEY);
    return s ? (int)s->nopening : 0;
}

/* ---- file types ----------------------------------------------------------------------- */
/* Paint.NET's Save As type order (OBSERVED 3.3), then paint.c extras. */
static const char *const k_type_order[] = { "pdn", "png", "jpeg", "jxl", "avif", "heic", "webp",
                                            "dds", "tiff", "gif", "bmp", "tga", "jxr", "ora" };
static const struct { const char *id, *label; } k_type_label[] = {
    { "pdn", "Paint.NET" }, { "png", "PNG" }, { "jpeg", "JPEG" }, { "jxl", "JPEG XL" },
    { "avif", "AV1 (AVIF)" }, { "heic", "HEIC" }, { "webp", "WebP" },
    { "dds", "DirectDraw Surface (DDS)" }, { "tiff", "TIFF" }, { "gif", "GIF" }, { "bmp", "BMP" },
    { "tga", "TGA" }, { "jxr", "JPEG XR" }, { "ora", "OpenRaster" }
};

static const char *type_label(const pc_codec *c)
{
    for (size_t i = 0; i < sizeof k_type_label / sizeof k_type_label[0]; i++)
        if (strcmp(k_type_label[i].id, c->id) == 0) return k_type_label[i].label;
    return c->name;
}

/* Codecs with the needed flag in display order. Returns the count. */
static size_t ordered_codecs(const pc_codec **out, size_t cap, uint32_t flag)
{
    size_t n = 0, k = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    for (size_t o = 0; o < sizeof k_type_order / sizeof k_type_order[0]; o++)
        for (size_t i = 0; i < n; i++)
            if (strcmp(list[i]->id, k_type_order[o]) == 0 && (list[i]->flags & flag) &&
                k < cap)
                out[k++] = list[i];
    for (size_t i = 0; i < n; i++) {
        bool known = false;
        for (size_t o = 0; o < sizeof k_type_order / sizeof k_type_order[0]; o++)
            if (strcmp(list[i]->id, k_type_order[o]) == 0) known = true;
        if (!known && (list[i]->flags & flag) && k < cap) out[k++] = list[i];
    }
    return k;
}

/* "JPEG (*.jpg; *.jpeg; *.jpe)" */
static void filter_label(const pc_codec *c, char *out, size_t cap)
{
    size_t at;
    const char *e = c->exts;
    snprintf(out, cap, "%s (", type_label(c));
    at = strlen(out);
    while (*e && at + 8u < cap) {
        if (e != c->exts) { out[at++] = ';'; out[at++] = ' '; }
        out[at++] = '*';
        out[at++] = '.';
        while (*e && *e != ';' && at + 3u < cap) out[at++] = *e++;
        if (*e == ';') e++;
    }
    out[at++] = ')';
    out[at] = '\0';
}

void io_build_filters(io_filters *fs, bool save)
{
    const pc_codec *cs[32];
    size_t n = ordered_codecs(cs, 32u, save ? PC_CODEC_SAVE : PC_CODEC_LOAD);
    memset(fs, 0, sizeof *fs);
    if (!save) {
        for (size_t i = 0; i < n; i++) {
            if (fs->all[0]) strncat(fs->all, ";", sizeof fs->all - strlen(fs->all) - 1u);
            strncat(fs->all, cs[i]->exts, sizeof fs->all - strlen(fs->all) - 1u);
        }
        app_copy_str(fs->label[0], sizeof fs->label[0], "All images");
        fs->f[0].name = fs->label[0];
        fs->f[0].pattern = fs->all;
        fs->n = 1;
    }
    for (size_t i = 0; i < n && fs->n < 38; i++) {
        filter_label(cs[i], fs->label[fs->n], sizeof fs->label[0]);
        fs->f[fs->n].name = fs->label[fs->n];
        fs->f[fs->n].pattern = cs[i]->exts;
        fs->codec[fs->n] = cs[i];
        fs->n++;
    }
    if (!save) {
        app_copy_str(fs->label[fs->n], sizeof fs->label[0], "All files");
        fs->f[fs->n].name = fs->label[fs->n];
        fs->f[fs->n].pattern = "*";
        fs->n++;
    }
}

/* First (default) extension of a codec. */
static void default_ext(const pc_codec *c, char *out, size_t cap)
{
    size_t k = 0;
    while (c->exts[k] && c->exts[k] != ';' && k + 1u < cap) { out[k] = c->exts[k]; k++; }
    out[k] = '\0';
}

const pc_codec *io_codec_for_path(const char *path, const pc_codec *fallback)
{
    const pc_codec *c = path ? pc_codec_by_ext(pal_path_ext(path)) : NULL;
    if (c && (c->flags & PC_CODEC_SAVE)) return c;
    return fallback && (fallback->flags & PC_CODEC_SAVE) ? fallback : NULL;
}

/* ---- remembered save options ------------------------------------------------------------ */
static bool param_key(const pc_codec *c, const fx_prop *p, char *key, size_t cap)
{
    if (!p->key || p->kind == FXP_POINT || p->kind == FXP_CUSTOM || p->kind == FXP_SEED)
        return false;
    snprintf(key, cap, "file.save.%s.%s", c->id, p->key);
    return true;
}

void io_params_load(app *a, const pc_codec *c, void *params)
{
    if (!c || !params || !c->params_size) return;
    pc_codec_default_params(c, params);
    for (uint32_t i = 0; i < c->n_props; i++) {
        char key[160];
        const char *v;
        if (!param_key(c, &c->props[i], key, sizeof key)) continue;
        v = app_settings_get(a->settings, key);
        if (v && *v) app_prop_set(&c->props[i], params, app_settings_double(a->settings, key,
                                                                          c->props[i].def));
    }
}

void io_params_store(app *a, const pc_codec *c, const void *params)
{
    if (!c || !params || !c->params_size) return;
    for (uint32_t i = 0; i < c->n_props; i++) {
        char key[160];
        if (!param_key(c, &c->props[i], key, sizeof key)) continue;
        (void)app_settings_set_double(a->settings, key, app_prop_get(&c->props[i], params));
    }
}

/* ---- recent files -------------------------------------------------------------------- */
void app_recent_add(app *a, const char *path)
{
    char *p;
    int32_t at = -1;
    if (!path || !*path) return;
    for (int32_t i = 0; i < a->nrecent; i++)
        if (strcmp(a->recent[i], path) == 0) at = i;
    if (at >= 0) {
        p = a->recent[at];
        memmove(&a->recent[1], &a->recent[0], (size_t)at * sizeof *a->recent);
        a->recent[0] = p;
        return;
    }
    p = app_strdup(path);
    if (!p) return;
    if (a->nrecent == APP_MAX_RECENT) {
        io_recent_thumb(a, a->recent[a->nrecent - 1], NULL, 0, 0);
        free(a->recent[--a->nrecent]);
    }
    memmove(&a->recent[1], &a->recent[0], (size_t)a->nrecent * sizeof *a->recent);
    a->recent[0] = p;
    a->nrecent++;
}

void app_recent_load(app *a)
{
    for (int32_t i = APP_MAX_RECENT - 1; i >= 0; i--) {
        char key[32];
        const char *v;
        snprintf(key, sizeof key, "recent.%d", (int)i);
        v = app_settings_get(a->settings, key);
        if (v && *v) app_recent_add(a, v);
    }
    {
        const char *d = app_settings_get(a->settings, "file.open_dir");
        if (d) app_copy_str(a->last_open_dir, sizeof a->last_open_dir, d);
        d = app_settings_get(a->settings, "file.save_dir");
        if (d) app_copy_str(a->last_save_dir, sizeof a->last_save_dir, d);
    }
}

void app_recent_store(app *a)
{
    for (int32_t i = 0; i < APP_MAX_RECENT; i++) {
        char key[32];
        snprintf(key, sizeof key, "recent.%d", (int)i);
        if (i < a->nrecent) app_settings_set(a->settings, key, a->recent[i]);
        else app_settings_remove(a->settings, key);
    }
    if (a->last_open_dir[0]) app_settings_set(a->settings, "file.open_dir", a->last_open_dir);
    if (a->last_save_dir[0]) app_settings_set(a->settings, "file.save_dir", a->last_save_dir);
}

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == id) return a->docs[i];
    return NULL;
}

/* ---- open ---------------------------------------------------------------------------- */
typedef struct open_job {
    char           *path;
    pc_doc         *doc;
    pc_image_meta   meta;
    const pc_codec *codec;
    pc_status       st;
    app_load_info   info;           /* lane CODEC (FL-BIG): what the load found */
    uint8_t        *thumb;          /* recent list thumbnail (owned) */
    int32_t         tw, th;
    char           *thumb_file;     /* its cache file, NULL = none (owned) */
} open_job;

/* lane CODEC (FL-BIG, wave 4): the limits and the byte budget are the
 * shared ones of every load (app_load_file, src/app/io/load.c): any size
 * up to PC_MAX_DIM per side, the file and the decoded pixels within three
 * quarters of the RAM. No fixed byte cap any more (it was 3 GiB, which
 * refused a 32768 x 32768 24-bit BMP). */
static void open_work(void *ud)
{
    open_job *j = (open_job *)ud;
    j->st = app_load_file(j->path, NULL, &j->doc, &j->meta, &j->codec, &j->info);
    /* lane CODEC (FL-ICC): a damaged profile, one Little-CMS cannot convert
     * with (wave 4), or one that does not match the pixels (CMYK or gray on
     * color, device link) is dropped with a note */
    if (j->st == PC_OK && j->doc) (void)pc_icc_meta_validate(&j->meta, j->doc);
    if (j->st == PC_OK && j->doc) j->thumb = io_thumb_rgba(j->doc, IO_THUMB_MAX, &j->tw, &j->th);
    if (j->thumb && j->thumb_file) (void)io_thumb_write(j->thumb_file, j->thumb, j->tw, j->th);
}

static bool startup_untouched(app *a, app_doc *d)
{
    return a->startup_doc && d && d->id == a->startup_doc_id && !d->path && !app_doc_dirty(d) &&
           d->hist->cur == d->hist->root && d->hist->count == 1u;
}

static void open_error(app *a, const char *path, pc_status st, const app_load_info *info)
{
    /* lane CODEC (FL-BIG): a file over the byte budget and an image over the
     * decode limits get different texts, each with the limits that applied */
    char msg[1400];
    app_load_error_text(msg, sizeof msg, "open", path, st, info);
    app_error(a, "%s", msg);
}

static void open_done(app *a, void *ud)
{
    open_job *j = (open_job *)ud;
    opening_remove(a, j->path);
    if (j->st != PC_OK || !j->doc) {
        open_error(a, j->path, j->st, &j->info);
    } else {
        app_doc *old = NULL;
        app_doc *d;
        /* load by content (FL-SNIFF), save by name: a WebP file called .png
         * opens as WebP and Save writes PNG into the .png */
        const pc_codec *save_codec = io_codec_for_path(j->path, j->codec);
        for (int32_t i = 0; i < a->ndocs; i++)
            if (startup_untouched(a, a->docs[i])) old = a->docs[i];
        d = app_doc_create(a, j->doc, j->path, save_codec ? save_codec : j->codec, &j->meta,
                           "Open Image");
        j->doc = NULL;
        if (!d) {
            app_error(a, "Could not open \"%s\": %s.", j->path, pc_status_str(PC_ERR_NOMEM));
        } else if (app_add_doc(a, d)) {
            app_recent_add(a, j->path);
            io_recent_thumb(a, j->path, j->thumb, j->tw, j->th);
            j->thumb = NULL;
            pal_path_dirname(a->last_open_dir, sizeof a->last_open_dir, j->path);
            /* an untouched startup image is replaced by the first opened one */
            if (old) app_close_doc_now(a, old);
        }
    }
    pc_doc_destroy(j->doc);
    pc_meta_free(&j->meta);
    free(j->thumb);
    free(j->thumb_file);
    free(j->path);
    free(j);
}

/* The recent thumbnail cache file of path, owned (NULL when there is none). */
static char *thumb_file_of(app *a, const char *path)
{
    char file[1100];
    return io_recent_thumb_path(a, path, file, sizeof file) ? app_strdup(file) : NULL;
}

bool app_open_path(app *a, const char *path)
{
    open_job *j;
    if (!path || !*path) return false;
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->path && strcmp(a->docs[i]->path, path) == 0) {
            app_set_active_doc(a, a->docs[i]);         /* F-MENU-FILE-OPEN-ALREADY */
            return true;
        }
    if (opening_has(a, path)) return true;             /* already being decoded */
    j = (open_job *)calloc(1u, sizeof *j);
    if (!j) return false;
    j->path = app_strdup(path);
    j->thumb_file = thumb_file_of(a, path);
    if (!j->path || !opening_add(a, path)) {
        free(j->thumb_file);
        free(j->path);
        free(j);
        return false;
    }
    if (!app_task(a, open_work, open_done, j)) {
        opening_remove(a, path);
        free(j->thumb_file);
        free(j->path);
        free(j);
        return false;
    }
    app_status(a, NULL);
    return true;
}

void app_open_paths(app *a, const char *const *paths, int n)
{
    for (int i = 0; i < n; i++)
        if (paths[i] && *paths[i] && !app_open_path(a, paths[i]))
            app_error(a, "Could not open \"%s\": %s.", paths[i], pc_status_str(PC_ERR_NOMEM));
}

static void open_dialog_cb(app *a, const char *const *paths, int n, int filter, void *ud)
{
    (void)filter;
    (void)ud;
    if (paths && n > 0) app_open_paths(a, paths, n);
}

void app_cmd_open_dialog(app *a)
{
    io_filters fs;
    const char *dir = a->last_open_dir[0] ? a->last_open_dir : pal_dir(PAL_DIR_PICTURES);
    if (!a->win) return;
    io_build_filters(&fs, false);
    app_filedlg(a, APP_FILEDLG_OPEN_MULTI, fs.f, fs.n, dir, open_dialog_cb, NULL);
}

/* ---- save ---------------------------------------------------------------------------- */
typedef struct save_flow {
    uint32_t         doc_id;
    bool             save_as;
    char            *path;
    const pc_codec  *codec;
    void            *params;          /* owned, codec->params_size bytes */
    bool             configured;      /* the options dialog ran (or there are no options) */
    app_save_done_fn done;
    void            *ud;
} save_flow;

static void flow_free(save_flow *f)
{
    if (!f) return;
    free(f->path);
    free(f->params);
    free(f);
}

static void flow_finish(app *a, save_flow *f, bool ok)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    if (f->done) f->done(a, d, ok, f->ud);
    flow_free(f);
}

/* Use codec c for the flow: params from the image when it already saves
 * as c with chosen options, else the remembered options of the type. */
static bool flow_set_codec(app *a, save_flow *f, app_doc *d, const pc_codec *c)
{
    void *p = NULL;
    if (c && c->params_size) {
        p = malloc(c->params_size);
        if (!p) return false;
        if (d && d->codec == c && d->save_params && d->save_configured)
            memcpy(p, d->save_params, c->params_size);
        else
            io_params_load(a, c, p);
    }
    free(f->params);
    f->params = p;
    f->codec = c;
    return true;
}

typedef struct write_job {
    uint32_t        doc_id;
    pc_doc         *snap;
    pc_image_meta   meta;
    const pc_codec *codec;
    void           *params;          /* owned copy */
    char           *path;
    uint64_t        seq;
    bool            configured;
    pc_status       st;
    uint8_t        *thumb;
    int32_t         tw, th;
    char           *thumb_file;      /* recent thumbnail cache file (owned, may be NULL) */
    save_flow      *flow;            /* NULL for direct saves */
} write_job;

static pc_status encode_and_write(const pc_doc *snap, const pc_image_meta *meta,
                                  const pc_codec *codec, const void *params, const char *path)
{
    pc_buf out;
    pc_status st;
    memset(&out, 0, sizeof out);
    st = codec->save(snap, meta, params, NULL, &out);
    if (st == PC_OK) st = pal_write_file_atomic(path, out.p, out.n);
    pc_buf_free(&out);
    return st;
}

static void write_work(void *ud)
{
    write_job *j = (write_job *)ud;
    j->st = encode_and_write(j->snap, &j->meta, j->codec, j->params, j->path);
    if (j->st == PC_OK) j->thumb = io_thumb_rgba(j->snap, IO_THUMB_MAX, &j->tw, &j->th);
    if (j->thumb && j->thumb_file) (void)io_thumb_write(j->thumb_file, j->thumb, j->tw, j->th);
}

static void write_finish(app *a, write_job *j)
{
    app_doc *d = doc_by_id(a, j->doc_id);
    if (j->st == PC_OK && d) {
        (void)app_doc_set_file(d, j->path, j->codec, j->params);
        d->saved_seq = j->seq;
        d->save_configured = j->configured;
        app_recent_add(a, j->path);
        io_recent_thumb(a, j->path, j->thumb, j->tw, j->th);
        j->thumb = NULL;
        pal_path_dirname(a->last_save_dir, sizeof a->last_save_dir, j->path);
        app_request_frame(a);
    } else if (j->st != PC_OK) {
        app_error(a, "Could not save \"%s\": %s.", j->path, pc_status_str(j->st));
    }
}

static void write_done(app *a, void *ud)
{
    write_job *j = (write_job *)ud;
    write_finish(a, j);
    pc_doc_destroy(j->snap);
    pc_meta_free(&j->meta);
    if (j->flow) flow_finish(a, j->flow, j->st == PC_OK);
    free(j->params);
    free(j->thumb);
    free(j->thumb_file);
    free(j->path);
    free(j);
}

static write_job *make_write_job(app_doc *d, const char *path, const pc_codec *codec,
                                 const void *params)
{
    write_job *j = (write_job *)calloc(1u, sizeof *j);
    if (!j) return NULL;
    j->doc_id = d->id;
    j->codec = codec;
    j->path = app_strdup(path);
    j->snap = app_doc_snapshot(d);
    j->seq = d->hist->cur->seq;
    if (codec->params_size) {
        j->params = malloc(codec->params_size);
        if (j->params) {
            if (params) memcpy(j->params, params, codec->params_size);
            else pc_codec_default_params(codec, j->params);
        }
    }
    if (!j->path || !j->snap || (codec->params_size && !j->params) ||
        io_meta_copy(&d->meta, &j->meta) != PC_OK) {
        pc_doc_destroy(j->snap);
        free(j->params);
        free(j->path);
        free(j);
        return NULL;
    }
    return j;
}

static void step_write(app *a, save_flow *f)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    write_job *j;
    if (!d) { flow_finish(a, f, false); return; }
    j = make_write_job(d, f->path, f->codec, f->params);
    if (!j) {
        app_error(a, "Could not save \"%s\": %s.", f->path, pc_status_str(PC_ERR_NOMEM));
        flow_finish(a, f, false);
        return;
    }
    j->configured = f->configured;
    j->thumb_file = thumb_file_of(a, f->path);
    j->flow = f;
    if (!app_task(a, write_work, write_done, j)) {
        write_work(j);                      /* no worker: save synchronously */
        write_done(a, j);
    }
}

/* FS-FLATTEN: after the options (OBSERVED 3.3), before writing. */
static void flatten_choice(app *a, int pick, void *ud)
{
    save_flow *f = (save_flow *)ud;
    app_doc *d = doc_by_id(a, f->doc_id);
    pc_status st;
    if (pick != 0 || !d) { flow_finish(a, f, false); return; }
    st = pc_layerop_flatten(d->hist, &a->par, "Flatten");
    if (st != PC_OK) {
        app_error(a, "Could not flatten the image: %s.", pc_status_str(st));
        flow_finish(a, f, false);
        return;
    }
    app_doc_history_changed(a, d);
    step_write(a, f);
}

static void step_flatten(app *a, save_flow *f)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    if (!d) { flow_finish(a, f, false); return; }
    if (!(f->codec->flags & PC_CODEC_LAYERED) && d->doc->n_layers > 1u) {
        char text[512];
        snprintf(text, sizeof text,
                 "The %s file type keeps a single layer, so the image is flattened before it "
                 "is saved. The flattening is a step in the History window and can be undone "
                 "after saving.", type_label(f->codec));
        app_choice(a, "Flatten Image", text, UI_ICON_LAYER_MERGE, "Flatten", "Cancel", NULL, 0, 1,
                   d->id, flatten_choice, f);
        return;
    }
    step_write(a, f);
}

/* ---- Save Configuration (FS-CONFIG): options + preview + file size -------------------- */
typedef struct cfg_dlg {
    app          *a;
    save_flow    *flow;                /* owned until OK, Cancel or teardown ends it */
    void         *params;              /* working copy */
    char          title[96];
    pc_doc       *snap;                /* preview source (shared tiles) */
    pc_image_meta meta;
    SDL_Texture  *tex;
    int32_t       tw, th;              /* texture size */
    uint32_t      img_w, img_h;        /* decoded image size */
    char          info[128];
    uint32_t      gen;                 /* bumps with every change */
    bool          running, dirty;
    bool          ended;               /* the dialog closed while a job ran */
    bool          have_result;
    /* preview view: zoom (screen px per image px, 0 = fit) and center */
    double        zoom, cx, cy;
    bool          panning;
    float         pan_x, pan_y;
    pc_status     err_shown;           /* lane SHELL: last encode error raised as a dialog */
} cfg_dlg;

typedef struct cfg_job {
    cfg_dlg        *dlg;
    uint32_t        gen;
    pc_doc         *snap;              /* borrowed from the dialog (alive until done) */
    pc_image_meta  *meta;
    const pc_codec *codec;
    void           *params;            /* owned copy */
    size_t          bytes;
    pc_status       st;
    uint8_t        *rgba;              /* owned preview, tw x th */
    int32_t         tw, th;
    uint32_t        img_w, img_h;
} cfg_job;

static void cfg_free(void *p)
{
    cfg_dlg *c = (cfg_dlg *)p;
    if (!c) return;
    /* lane W4-MODAL: closed without OK or Cancel (app_dialogs_free at exit,
     * a refused push): the save flow ends as cancelled, exactly once */
    if (c->flow) {
        save_flow *f = c->flow;
        c->flow = NULL;
        flow_finish(c->a, f, false);
    }
    if (c->running) {                  /* the worker still uses snap: free in its done */
        c->ended = true;
        return;
    }
    if (c->tex) SDL_DestroyTexture(c->tex);
    pc_doc_destroy(c->snap);
    pc_meta_free(&c->meta);
    free(c->params);
    free(c);
}

/* The re-opened image as RGBA, longest side at most PREVIEW_MAX_SIDE. */
static uint8_t *preview_rgba(const pc_doc *d, int32_t *tw, int32_t *th)
{
    int32_t w, h;
    uint8_t *out;
    pc_px32 *row;
    double s = 1.0;
    if (d->w > PREVIEW_MAX_SIDE || d->h > PREVIEW_MAX_SIDE) {
        s = (double)PREVIEW_MAX_SIDE / (double)(d->w > d->h ? d->w : d->h);
    }
    w = (int32_t)((double)d->w * s + 0.5);
    h = (int32_t)((double)d->h * s + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    out = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    row = (pc_px32 *)malloc((size_t)d->w * sizeof *row);
    if (!out || !row) { free(out); free(row); return NULL; }
    for (int32_t y = 0; y < h; y++) {
        int32_t sy = (int32_t)(((double)y + 0.5) / s);
        if (sy >= (int32_t)d->h) sy = (int32_t)d->h - 1;
        (void)pc_comp_rect(d, pc_rect_make(0, sy, (int32_t)d->w, 1), row, d->w, NULL);
        for (int32_t x = 0; x < w; x++) {
            int32_t sx = (int32_t)(((double)x + 0.5) / s);
            uint8_t *o = out + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            if (sx >= (int32_t)d->w) sx = (int32_t)d->w - 1;
            o[0] = row[sx].r; o[1] = row[sx].g; o[2] = row[sx].b; o[3] = row[sx].a;
        }
    }
    free(row);
    *tw = w;
    *th = h;
    return out;
}

static void cfg_work(void *ud)
{
    cfg_job *j = (cfg_job *)ud;
    pc_buf out;
    memset(&out, 0, sizeof out);
    j->st = j->codec->save(j->snap, j->meta, j->params, NULL, &out);
    if (j->st == PC_OK) {
        j->bytes = out.n;
        if ((j->codec->flags & PC_CODEC_LOAD) && j->codec->load) {
            pc_doc *back = NULL;
            pc_image_meta m;
            /* lane CODEC (FL-BIG): the limits of File > Open, so every image
             * that saves also gets its preview */
            if (app_load_bytes(out.p, out.n, NULL, j->codec, &back, &m, NULL, NULL) == PC_OK &&
                back) {
                j->rgba = preview_rgba(back, &j->tw, &j->th);
                j->img_w = back->w;
                j->img_h = back->h;
                pc_doc_destroy(back);
            }
            pc_meta_free(&m);
        }
    }
    pc_buf_free(&out);
}

static void cfg_start(app *a, cfg_dlg *c);

static void format_size(double bytes, char *out, size_t cap)
{
    if (bytes < 1024.0) snprintf(out, cap, "%.0f bytes", bytes);
    else if (bytes < 1024.0 * 1024.0) snprintf(out, cap, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024.0 * 1024.0 * 1024.0)
        snprintf(out, cap, "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(out, cap, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

static void cfg_done(app *a, void *ud)
{
    cfg_job *j = (cfg_job *)ud;
    cfg_dlg *c = j->dlg;
    c->running = false;
    if (c->ended) {
        free(j->params);
        free(j->rgba);
        free(j);
        cfg_free(c);
        return;
    }
    if (j->gen == c->gen) {
        if (j->st == PC_OK) {
            char sz[48];
            format_size((double)j->bytes, sz, sizeof sz);
            snprintf(c->info, sizeof c->info, "File size: %s", sz);
        } else {
            snprintf(c->info, sizeof c->info, "File size: error (%s)", pc_status_str(j->st));
            /* lane SHELL (F-DLG-SAVECFG-FILESIZE): the standard error dialog,
             * once per kind of failure while the dialog is open */
            if (c->err_shown != j->st) {
                c->err_shown = j->st;
                app_error(a, "These options cannot be saved as %s: %s.", type_label(j->codec),
                          pc_status_str(j->st));
            }
        }
        if (j->rgba) {
            if (c->tex && (c->tw != j->tw || c->th != j->th)) {
                SDL_DestroyTexture(c->tex);
                c->tex = NULL;
            }
            if (!c->tex) {
                c->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                           j->tw, j->th);
                if (c->tex) SDL_SetTextureBlendMode(c->tex, SDL_BLENDMODE_BLEND);
            }
            if (c->tex) SDL_UpdateTexture(c->tex, NULL, j->rgba, j->tw * 4);
            c->tw = j->tw;
            c->th = j->th;
            c->img_w = j->img_w;
            c->img_h = j->img_h;
            c->have_result = true;
        }
    }
    free(j->params);
    free(j->rgba);
    free(j);
    if (c->dirty) cfg_start(a, c);
    app_request_frame(a);
}

static void cfg_start(app *a, cfg_dlg *c)
{
    cfg_job *j;
    const pc_codec *codec = c->flow->codec;
    c->dirty = false;
    if (!c->snap) return;
    j = (cfg_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->dlg = c;
    j->gen = c->gen;
    j->snap = c->snap;
    j->meta = &c->meta;
    j->codec = codec;
    if (codec->params_size) {
        j->params = malloc(codec->params_size);
        if (!j->params) { free(j); return; }
        memcpy(j->params, c->params, codec->params_size);
    }
    snprintf(c->info, sizeof c->info, "File size: computing...");
    if (app_task(a, cfg_work, cfg_done, j)) {
        c->running = true;
    } else {
        free(j->params);
        free(j);
    }
}

static void cfg_changed(app *a, cfg_dlg *c)
{
    c->gen++;
    if (c->running) c->dirty = true;
    else cfg_start(a, c);
}

/* Fit zoom of the preview box (never above 100 %). */
static double cfg_fit(const cfg_dlg *c, ui_rect box)
{
    double s;
    if (c->img_w == 0u || c->img_h == 0u) return 1.0;
    s = (double)(box.w - 12) / (double)c->img_w;
    if ((double)(box.h - 12) / (double)c->img_h < s) s = (double)(box.h - 12) / (double)c->img_h;
    return s > 1.0 ? 1.0 : s;
}

/* F-DLG-SAVECFG-ZOOMPAN: wheel zooms at the pointer, drag pans, double
 * click returns to the fitted view. */
static void cfg_preview(app *a, cfg_dlg *c, ui_rect box)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, "##cfgpreview"), box, 0u);
    double fit = cfg_fit(c, box), z = c->zoom > 0.0 ? c->zoom : fit;
    ui_vec2 wheel = ui_wheel_take(ui, box);
    ui_draw_rrect(ui, box, 4.0f, p->field);
    ui_draw_rrect_outline(ui, box, 4.0f, 1, p->border);
    if (!c->tex || !c->have_result || c->img_w == 0u) return;
    if (c->zoom <= 0.0) {
        c->cx = (double)c->img_w * 0.5;
        c->cy = (double)c->img_h * 0.5;
    }
    if (in.hovered && wheel.y != 0.0f) {
        double mx = (double)in.mouse.x, my = (double)in.mouse.y;
        double bx = (double)box.x + (double)box.w * 0.5, by = (double)box.y + (double)box.h * 0.5;
        double ix = c->cx + (mx - bx) / z, iy = c->cy + (my - by) / z;
        double nz = z * pow(1.25, (double)wheel.y);
        if (nz < fit) nz = fit;
        if (nz > 32.0) nz = 32.0;
        c->cx = ix - (mx - bx) / nz;
        c->cy = iy - (my - by) / nz;
        c->zoom = nz <= fit ? 0.0 : nz;
        z = nz;
    }
    if (in.double_clicked) c->zoom = 0.0;
    if (in.pressed) {
        c->panning = true;
        c->pan_x = in.mouse.x;
        c->pan_y = in.mouse.y;
    }
    if (c->panning && in.held && c->zoom > 0.0) {
        c->cx -= (double)(in.mouse.x - c->pan_x) / z;
        c->cy -= (double)(in.mouse.y - c->pan_y) / z;
        c->pan_x = in.mouse.x;
        c->pan_y = in.mouse.y;
    }
    if (!in.held) c->panning = false;
    if (c->zoom <= 0.0) {
        z = fit;
        c->cx = (double)c->img_w * 0.5;
        c->cy = (double)c->img_h * 0.5;
    } else {
        /* keep the image inside the box when it is larger */
        double hw = (double)box.w * 0.5 / z, hh = (double)box.h * 0.5 / z;
        if (hw * 2.0 < (double)c->img_w) {
            if (c->cx < hw) c->cx = hw;
            if (c->cx > (double)c->img_w - hw) c->cx = (double)c->img_w - hw;
        } else {
            c->cx = (double)c->img_w * 0.5;
        }
        if (hh * 2.0 < (double)c->img_h) {
            if (c->cy < hh) c->cy = hh;
            if (c->cy > (double)c->img_h - hh) c->cy = (double)c->img_h - hh;
        } else {
            c->cy = (double)c->img_h * 0.5;
        }
    }
    {
        double bx = (double)box.x + (double)box.w * 0.5, by = (double)box.y + (double)box.h * 0.5;
        ui_rect img;
        img.x = (int32_t)floor(bx - c->cx * z + 0.5);
        img.y = (int32_t)floor(by - c->cy * z + 0.5);
        img.w = (int32_t)((double)c->img_w * z + 0.5);
        img.h = (int32_t)((double)c->img_h * z + 0.5);
        if (img.w < 1) img.w = 1;
        if (img.h < 1) img.h = 1;
        ui_push_clip(ui, ui_rect_inset(box, 1, 1));
        ui_draw_checker(ui, img, ui_px(ui, 6.0f), p->checker_a, p->checker_b);
        ui_draw_image(ui, c->tex, NULL, img, z >= 1.0 ? UI_FILTER_NEAREST : UI_FILTER_LINEAR,
                      ui_rgba(255, 255, 255, 255));
        ui_pop_clip(ui);
    }
    if (in.hovered) ui_set_cursor(ui, c->zoom > 0.0 ? UI_CURSOR_MOVE : UI_CURSOR_DEFAULT);
}

static bool cfg_frame(app *a, void *st)
{
    cfg_dlg *c = (cfg_dlg *)st;
    ui_ctx *ui = a->ui;
    const pc_codec *codec = c->flow->codec;
    ui_size cells[2];
    app_props_ctx pctx;
    uint32_t r;
    bool ok = false, cancel = false, enter;
    ui_dialog_begin(ui, c->title, 760.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_px(420.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_layout_begin(ui, 0.0f);
    memset(&pctx, 0, sizeof pctx);
    pctx.id = "##saveopts";
    if (app_props_ui(a, codec->props, codec->n_props, c->params, &pctx) & APP_PROPS_CHANGED)
        cfg_changed(a, c);
    ui_layout_space(ui, 6.0f);
    if (ui_button_ex(ui, "Defaults##cfgdef", UI_ICON_RESET, 0)) {
        pc_codec_default_params(codec, c->params);
        cfg_changed(a, c);
    }
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    {
        ui_rect box = ui_layout_next(ui, ui_px(ui, 412.0f), ui_px(ui, 300.0f));
        char zl[48];
        cfg_preview(a, c, box);
        ui_label_ex(ui, c->info, UI_LABEL_DIM);
        if (c->have_result) {
            double z = c->zoom > 0.0 ? c->zoom : cfg_fit(c, box);
            snprintf(zl, sizeof zl, "Preview %.0f%%  (wheel: zoom, drag: pan)", z * 100.0);
            ui_label_ex(ui, zl, UI_LABEL_DIM | UI_LABEL_SMALL);
        }
        if (c->running) app_request_frame_at(a, a->now + 50u);
    }
    ui_layout_end(ui);
    ui_layout_column(ui);
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (r == UI_DLG_OK) ok = true;
    else if (r) cancel = true;
    if (!ok && !cancel) return true;
    if (ok) {
        memcpy(c->flow->params, c->params, codec->params_size);
        io_params_store(a, codec, c->params);         /* last used options per type */
        c->flow->configured = true;
        step_flatten(a, c->flow);
    } else {
        flow_finish(a, c->flow, false);
    }
    c->flow = NULL;
    return false;
}

static uint32_t cfg_seq;

static void step_config(app *a, save_flow *f)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    cfg_dlg *c;
    if (!d) { flow_finish(a, f, false); return; }
    if (!f->codec->n_props || !f->codec->params_size) {
        f->configured = true;
        step_flatten(a, f);
        return;
    }
    /* Save reuses the options chosen this session (MENUS.md Save) */
    if (!f->save_as && d->save_configured && d->codec == f->codec) {
        f->configured = true;
        step_flatten(a, f);
        return;
    }
    c = (cfg_dlg *)calloc(1u, sizeof *c);
    if (!c) { flow_finish(a, f, false); return; }
    c->a = a;
    c->flow = f;
    /* lane W4-MODAL: one ui id per dialog, so two can never share state */
    snprintf(c->title, sizeof c->title, "Save Configuration: %s##savecfg%u", type_label(f->codec),
             (unsigned)++cfg_seq);
    c->params = malloc(f->codec->params_size);
    c->snap = app_doc_snapshot(d);
    if (!c->params || !c->snap || io_meta_copy(&d->meta, &c->meta) != PC_OK) {
        pc_doc_destroy(c->snap);
        free(c->params);
        free(c);
        flow_finish(a, f, false);
        return;
    }
    memcpy(c->params, f->params, f->codec->params_size);
    c->img_w = d->doc->w;
    c->img_h = d->doc->h;
    if (!app_dialog_push(a, cfg_frame, c, cfg_free)) return;   /* cfg_free ended the flow */
    cfg_start(a, c);
}

/* Resolve the chosen path and type filter into a codec and a path with an
 * extension: a typed extension of a known type decides (the native dialogs
 * on Linux and macOS do not keep the filter and the name in sync), else
 * the selected filter, else the flow's type; its default extension is
 * appended then. */
static void save_dialog_cb(app *a, const char *const *paths, int n, int filter, void *ud)
{
    save_flow *f = (save_flow *)ud;
    io_filters fs;
    const pc_codec *c;
    char path[2048];
    app_doc *d;
    if (!paths || n < 1 || !paths[0] || !*paths[0]) { flow_finish(a, f, false); return; }
    d = doc_by_id(a, f->doc_id);
    if (!d) { flow_finish(a, f, false); return; }
    io_build_filters(&fs, true);
    app_copy_str(path, sizeof path, paths[0]);
    c = pc_codec_by_ext(pal_path_ext(path));
    if (!c || !(c->flags & PC_CODEC_SAVE)) {
        const pc_codec *fc = filter >= 0 && filter < fs.n && fs.codec[filter] ? fs.codec[filter]
                                                                              : f->codec;
        char ext[16];
        if (!fc) fc = pc_codec_by_id("png");
        if (!fc) { flow_finish(a, f, false); return; }
        default_ext(fc, ext, sizeof ext);
        if (strlen(path) + strlen(ext) + 2u < sizeof path) {
            size_t pl = strlen(path);
            if (pl > 0u && path[pl - 1u] == '.') path[--pl] = '\0';
            strcat(path, ".");
            strcat(path, ext);
        }
        c = fc;
    }
    free(f->path);
    f->path = app_strdup(path);
    if (!f->path || !flow_set_codec(a, f, d, c)) {
        flow_finish(a, f, false);
        return;
    }
    step_config(a, f);
}

/* Default Save As type: .pdn for layered images, else the current type,
 * else PNG (MENUS.md Save As, 3.36 DoSaveAs). */
static const pc_codec *default_save_codec(const app_doc *d)
{
    const pc_codec *c = d->codec;
    if (d->doc->n_layers > 1u && (!c || !(c->flags & PC_CODEC_LAYERED)))
        c = pc_codec_by_id("pdn");
    if (!c || !(c->flags & PC_CODEC_SAVE)) c = pc_codec_by_id("png");
    return c;
}

static void ask_path(app *a, save_flow *f, app_doc *d)
{
    io_filters fs;
    char def[2048], ext[16];
    const pc_codec *c = default_save_codec(d);
    if (!c) { flow_finish(a, f, false); return; }
    default_ext(c, ext, sizeof ext);
    if (d->path) {
        char dir[1024];
        const char *base = pal_path_basename(d->path);
        const char *dot = strrchr(base, '.');
        size_t bl = dot && dot != base ? (size_t)(dot - base) : strlen(base);
        char name[512];
        pal_path_dirname(dir, sizeof dir, d->path);
        snprintf(name, sizeof name, "%.*s.%s", (int)bl, base, ext);
        pal_path_join(def, sizeof def, dir, name);
    } else {
        char name[300];
        const char *dir = a->last_save_dir[0] ? a->last_save_dir : pal_dir(PAL_DIR_PICTURES);
        snprintf(name, sizeof name, "%s.%s", d->name, ext);
        pal_path_join(def, sizeof def, dir ? dir : "", name);
    }
    if (!flow_set_codec(a, f, d, c)) {
        flow_finish(a, f, false);
        return;
    }
    io_build_filters(&fs, true);
    /* the default type first so the dialog preselects it */
    for (int i = 1; i < fs.n; i++)
        if (fs.codec[i] == c) {
            pal_filter tf = fs.f[i];
            const pc_codec *tc = fs.codec[i];
            memmove(&fs.f[1], &fs.f[0], (size_t)i * sizeof fs.f[0]);
            memmove((void *)&fs.codec[1], (const void *)&fs.codec[0],
                    (size_t)i * sizeof fs.codec[0]);
            fs.f[0] = tf;
            fs.codec[0] = tc;
            break;
        }
    /* fs.f names point into fs.label, which pal copies before returning;
     * without a window save_dialog_cb runs at once as a cancel, and at exit
     * the open dialog is answered as cancelled (filedlg.c) */
    app_filedlg(a, APP_FILEDLG_SAVE, fs.f, fs.n, def, save_dialog_cb, f);
}

void app_save_doc(app *a, app_doc *d, bool save_as, app_save_done_fn done, void *ud)
{
    save_flow *f;
    const pc_codec *c;
    if (!d) { if (done) done(a, d, false, ud); return; }
    if (app_doc_index(a, d) == a->active) {
        if (a->cv.captured) app_canvas_lost_capture(a);
        (void)app_tool_finish(a);
    }
    f = (save_flow *)calloc(1u, sizeof *f);
    if (!f) { if (done) done(a, d, false, ud); return; }
    f->doc_id = d->id;
    f->save_as = save_as;
    f->done = done;
    f->ud = ud;
    c = d->path ? io_codec_for_path(d->path, d->codec) : NULL;
    if (!save_as && d->path && c) {
        f->path = app_strdup(d->path);
        if (!f->path || !flow_set_codec(a, f, d, c)) {
            flow_finish(a, f, false);
            return;
        }
        step_config(a, f);
        return;
    }
    ask_path(a, f, d);
}

pc_status app_save_doc_to(app *a, app_doc *d, const char *path, const pc_codec *codec,
                          const void *params, bool sync)
{
    write_job *j;
    void *p = NULL;
    if (!d || !path) return PC_ERR_ARG;
    if (!codec) codec = io_codec_for_path(path, NULL);
    if (!codec || !(codec->flags & PC_CODEC_SAVE)) return PC_ERR_UNSUPPORTED;
    if (app_doc_index(a, d) == a->active) {
        if (a->cv.captured) app_canvas_lost_capture(a);
        (void)app_tool_finish(a);
    }
    if (!(codec->flags & PC_CODEC_LAYERED) && d->doc->n_layers > 1u) {
        pc_status st = pc_layerop_flatten(d->hist, &a->par, "Flatten");
        if (st != PC_OK) return st;
        app_doc_history_changed(a, d);
    }
    if (!params && codec->params_size) {
        p = malloc(codec->params_size);
        if (!p) return PC_ERR_NOMEM;
        if (d->codec == codec && d->save_params && d->save_configured)
            memcpy(p, d->save_params, codec->params_size);
        else
            io_params_load(a, codec, p);
        params = p;
    }
    j = make_write_job(d, path, codec, params);
    free(p);
    if (!j) return PC_ERR_NOMEM;
    j->configured = true;
    j->thumb_file = thumb_file_of(a, path);
    if (sync || !app_task(a, write_work, write_done, j)) {
        pc_status st;
        write_work(j);
        st = j->st;
        write_done(a, j);
        return st;
    }
    return PC_OK;
}

/* ---- close --------------------------------------------------------------------------- */
typedef struct close_flow {
    uint32_t          doc_id;
    app_close_done_fn done;
    void             *ud;
} close_flow;

static void close_finish(app *a, close_flow *c, bool closed)
{
    if (c->done) c->done(a, closed, c->ud);
    free(c);
}

static void close_after_save(app *a, app_doc *d, bool ok, void *ud)
{
    close_flow *c = (close_flow *)ud;
    if (ok && d && !app_doc_dirty(d)) {
        app_close_doc_now(a, d);
        close_finish(a, c, true);
    } else {
        close_finish(a, c, false);                 /* F-DLG-UNSAVED-CHAIN */
    }
}

static void close_choice(app *a, int pick, void *ud)
{
    close_flow *c = (close_flow *)ud;
    app_doc *d = doc_by_id(a, c->doc_id);
    if (!d) { close_finish(a, c, true); return; }
    if (pick == 0) {
        app_save_doc(a, d, false, close_after_save, c);
    } else if (pick == 1) {
        app_close_doc_now(a, d);
        close_finish(a, c, true);
    } else {
        close_finish(a, c, false);
    }
}

void app_close_doc(app *a, app_doc *d, app_close_done_fn done, void *ud)
{
    close_flow *c;
    char text[600];
    if (!d) { if (done) done(a, true, ud); return; }
    if (app_doc_index(a, d) == a->active) {
        if (a->cv.captured) app_canvas_lost_capture(a);
        (void)app_tool_finish(a);
    }
    if (!app_doc_dirty(d)) {
        app_close_doc_now(a, d);
        if (done) done(a, true, ud);
        return;
    }
    c = (close_flow *)calloc(1u, sizeof *c);
    if (!c) { if (done) done(a, false, ud); return; }
    c->doc_id = d->id;
    c->done = done;
    c->ud = ud;
    snprintf(text, sizeof text, "\"%s\" has changes that were not saved. Save them before "
             "closing the image?", d->name);
    app_choice(a, "Unsaved Changes", text, UI_ICON_WARNING, "Save", "Don't Save", "Cancel", 0, 2,
               d->id, close_choice, c);
}

/* ---- Close All / Exit: the list of unsaved images ----------------------------------- */
typedef struct close_all {
    app_close_done_fn done;
    void             *ud;
    uint32_t         *ids;           /* unsaved images when the dialog opened */
    int32_t           n;
    int32_t           next;          /* Save: next image to save */
    uint32_t          orig_active;   /* image to return to on Cancel */
} close_all;

static void close_all_free(close_all *c)
{
    if (!c) return;
    free(c->ids);
    free(c);
}

static void close_all_end(app *a, close_all *c, bool closed)
{
    if (closed) {
        while (a->ndocs > 0) app_close_doc_now(a, a->docs[a->ndocs - 1]);
    } else {
        app_doc *d = doc_by_id(a, c->orig_active);
        if (d) app_set_active_doc(a, d);
    }
    if (c->done) c->done(a, closed, c->ud);
    close_all_free(c);
}

static void close_all_save_next(app *a, close_all *c);

static void close_all_saved(app *a, app_doc *d, bool ok, void *ud)
{
    close_all *c = (close_all *)ud;
    if (!ok || (d && app_doc_dirty(d))) {          /* cancelled or failed: stop */
        close_all_end(a, c, false);
        return;
    }
    close_all_save_next(a, c);
}

static void close_all_save_next(app *a, close_all *c)
{
    while (c->next < c->n) {
        app_doc *d = doc_by_id(a, c->ids[c->next++]);
        if (d && app_doc_dirty(d)) {
            app_set_active_doc(a, d);
            app_save_doc(a, d, false, close_all_saved, c);
            return;
        }
    }
    close_all_end(a, c, true);
}

typedef struct close_all_dlg { app *a; close_all *c; } close_all_dlg;

static void close_all_dlg_free(void *p)
{
    close_all_dlg *g = (close_all_dlg *)p;
    if (!g) return;
    /* still owned: the dialog closed without an answer (app_dialogs_free at
     * exit, a refused push), which ends the flow like Cancel (lane W4-MODAL) */
    if (g->c) close_all_end(g->a, g->c, false);
    free(g);
}

static bool close_all_frame(app *a, void *st)
{
    close_all_dlg *g = (close_all_dlg *)st;
    close_all *c = g->c;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int pick = -2;
    uint32_t r;
    ui_size cells[4];
    ui_dialog_begin(ui, "Unsaved Changes##closeall", 560.0f, 0.0f);
    ui_text_wrapped(ui, "These images have changes that were not saved. Click an image to look "
                    "at it.", 0);
    ui_layout_space(ui, 6.0f);
    /* thumbnail strip of the unsaved images (3.36 document strip) */
    {
        int32_t cell = ui_px(ui, 88.0f), gap = ui_px(ui, 8.0f);
        ui_rect strip = ui_layout_next(ui, 0, cell + ui_px(ui, 22.0f));
        int32_t x = strip.x;
        ui_draw_rrect(ui, strip, 4.0f, p->field);
        ui_push_clip(ui, strip);
        for (int32_t i = 0; i < c->n; i++) {
            app_doc *d = doc_by_id(a, c->ids[i]);
            ui_rect box = ui_rect_make(x + gap / 2, strip.y + ui_px(ui, 4.0f), cell - gap,
                                       cell - gap);
            ui_interaction in;
            if (!d) continue;
            ui_push_id_int(ui, (int64_t)d->id);
            in = ui_interact(ui, ui_get_id(ui, "##thumb"), box, 0u);
            ui_pop_id(ui);
            if (app_active_doc(a) == d) ui_draw_rrect(ui, ui_rect_inset(box, -3, -3), 4.0f,
                                                      p->selection);
            else if (in.hovered) ui_draw_rrect(ui, ui_rect_inset(box, -3, -3), 4.0f, p->hover);
            if (d->thumb && d->thumb_w > 0 && d->thumb_h > 0) {
                float sx = (float)box.w / (float)d->thumb_w, sy = (float)box.h / (float)d->thumb_h;
                float s = sx < sy ? sx : sy;
                ui_rect img = ui_rect_center(box, (int32_t)((float)d->thumb_w * s),
                                             (int32_t)((float)d->thumb_h * s));
                ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
                ui_draw_image(ui, d->thumb, NULL, img, UI_FILTER_LINEAR,
                              ui_rgba(255, 255, 255, 255));
            }
            ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                             ui_rect_make(x, box.y + box.h + ui_px(ui, 2.0f), cell,
                                          ui_px(ui, 16.0f)),
                             UI_ALIGN_CENTER, UI_TEXT_ELLIPSIS, p->text, d->name, strlen(d->name));
            if (in.clicked) app_set_active_doc(a, d);
            x += cell;
        }
        ui_pop_clip(ui);
    }
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    cells[2] = ui_size_auto();
    cells[3] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 4, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, ui_get_theme(ui)->m.control_h));
    if (ui_button_ex(ui, "Save All##ca_save", UI_ICON_SAVE, UI_BUTTON_PRIMARY)) pick = 0;
    if (ui_button_ex(ui, "Don't Save##ca_discard", UI_ICON_NONE, 0)) pick = 1;
    if (ui_button_ex(ui, "Cancel##ca_cancel", UI_ICON_NONE, 0)) pick = 2;
    ui_layout_column(ui);
    /* Enter saves; Alt+S / Alt+N are the Save All / Don't Save mnemonics */
    if (pick == -2 && a->dlg_top && !ui_text_input_active(ui)) {
        if (ui_key_take(ui, SDLK_RETURN, 0) || ui_key_take(ui, SDLK_KP_ENTER, 0) ||
            ui_key_take(ui, SDLK_S, UI_MOD_ALT))
            pick = 0;
        else if (ui_key_take(ui, SDLK_N, UI_MOD_ALT))
            pick = 1;
    }
    r = ui_dialog_end(ui);
    if (pick == -2 && r) pick = 2;
    if (pick == -2) return true;
    g->c = NULL;                       /* ownership moves to the flow */
    if (pick == 0) {
        c->next = 0;
        close_all_save_next(a, c);
    } else {
        close_all_end(a, c, pick == 1);
    }
    return false;
}

static void close_all_single(app *a, bool closed, void *ud)
{
    close_all *c = (close_all *)ud;
    close_all_end(a, c, closed);
}

void app_close_all(app *a, app_close_done_fn done, void *ud)
{
    close_all *c = (close_all *)calloc(1u, sizeof *c);
    int32_t n = 0;
    if (!c) { if (done) done(a, false, ud); return; }
    if (a->cv.captured) app_canvas_lost_capture(a);
    (void)app_tool_finish(a);
    c->done = done;
    c->ud = ud;
    c->orig_active = app_active_doc(a) ? app_active_doc(a)->id : 0u;
    c->ids = (uint32_t *)calloc((size_t)(a->ndocs > 0 ? a->ndocs : 1), sizeof *c->ids);
    if (!c->ids) { close_all_free(c); if (done) done(a, false, ud); return; }
    for (int32_t i = 0; i < a->ndocs; i++)
        if (app_doc_dirty(a->docs[i])) c->ids[n++] = a->docs[i]->id;
    c->n = n;
    if (n == 0) {
        close_all_end(a, c, true);
    } else if (n == 1) {
        app_doc *d = doc_by_id(a, c->ids[0]);
        app_set_active_doc(a, d);
        app_close_doc(a, d, close_all_single, c);
    } else {
        close_all_dlg *g = (close_all_dlg *)calloc(1u, sizeof *g);
        app_doc *act = app_active_doc(a);
        if (!g) { close_all_end(a, c, false); return; }
        g->a = a;
        g->c = c;
        if (act && !app_doc_dirty(act)) app_set_active_doc(a, doc_by_id(a, c->ids[0]));
        (void)app_dialog_push(a, close_all_frame, g, close_all_dlg_free);
    }
}

static void quit_after_close_all(app *a, bool closed, void *ud)
{
    (void)ud;
    if (closed) {
        a->quit_done = true;
        app_request_frame(a);
    } else {
        a->quit_req = false;
    }
}

bool app_quit_unsaved(app *a)
{
    int32_t n = 0;
    for (int32_t i = 0; i < a->ndocs; i++)
        if (app_doc_dirty(a->docs[i])) n++;
    if (n < 2) return false;           /* one image: the normal close prompt */
    app_close_all(a, quit_after_close_all, NULL);
    return true;
}

/* ---- New Image dialog (MENUS.md New Image, OBSERVED 2) -------------------------------- */
/* Lane SHELL (wave 3b): the dialog uses the size model of Resize and
 * Canvas Size (edit/m_size.h). The pixel boxes accept 0 .. 262144 like
 * Paint.NET's (no silent clamp to the valid range), and OK is disabled
 * with the reason shown while the size is not a valid image (zero, or
 * above paint.c's 65535 per side, ADR-014); Enter does nothing then.
 * Values typed out of a box's range are clamped when the box loses the
 * focus (O-UI-CLAMP), so the dialog never creates another size than the
 * one shown. The print size has its own unit choice (Inches,
 * Centimeters), independent of the resolution unit; maintain aspect
 * ratio keeps the ratio the size had when it was checked. The aspect
 * lock and both units are remembered. */
typedef struct new_dlg {
    m_size s;
} new_dlg;

static bool new_frame(app *a, void *st)
{
    new_dlg *n = (new_dlg *)st;
    m_size *s = &n->s;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_size cells[3];
    uint32_t r;
    char est[96], sz[48], why[96];
    static const char *const res_units[] = { "Pixels/inch", "Pixels/centimeter" };
    static const char *const print_units[] = { "Inches", "Centimeters" };
    bool enter, valid;
    ui_dialog_begin(ui, "New Image##newimg", 420.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    m_size_format_bytes(m_size_bytes(s, 1u), sz, sizeof sz);
    snprintf(est, sizeof est, "New image size: %s", sz);
    ui_label_ex(ui, est, UI_LABEL_DIM);
    {
        bool keep = s->keep;
        if (ui_checkbox(ui, "Maintain aspect ratio##keep", &keep)) {
            /* the ratio of the size as it is now */
            int32_t w = m_size_px(s, false), h = m_size_px(s, true);
            s->ow = w > 0 ? (uint32_t)w : 1u;
            s->oh = h > 0 ? (uint32_t)h : 1u;
            m_size_set_keep(s, keep);
        }
    }
    ui_heading(ui, "Pixel size");
    cells[0] = ui_size_px(110.0f);
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_px(150.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Width", UI_LABEL_DIM);
    {
        int32_t w = m_size_px(s, false);
        if (ui_number_int(ui, "##nw", &w, 0, M_SIZE_MAX_EDIT, 1, 0)) m_size_set_w(s, (double)w);
    }
    ui_label_ex(ui, "pixels", UI_LABEL_DIM);
    ui_label_ex(ui, "Height", UI_LABEL_DIM);
    {
        int32_t h = m_size_px(s, true);     /* read after the width may have changed it */
        if (ui_number_int(ui, "##nh", &h, 0, M_SIZE_MAX_EDIT, 1, 0)) m_size_set_h(s, (double)h);
    }
    ui_label_ex(ui, "pixels", UI_LABEL_DIM);
    ui_layout_column(ui);
    ui_heading(ui, "Resolution");
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Resolution", UI_LABEL_DIM);
    {
        double res = m_size_res(s);
        int u = s->res_unit;
        if (ui_number_double(ui, "##nres", &res, M_SIZE_MIN_RES, M_SIZE_MAX_RES, 1.0, 2, 0))
            m_size_set_res(s, res);
        if (ui_combo(ui, "##nresu", &u, res_units, 2)) m_size_set_res_unit(s, u);
    }
    ui_layout_column(ui);
    ui_heading(ui, "Print size");
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Width", UI_LABEL_DIM);
    {
        double pw = m_size_print(s, false);
        int pu = s->print_unit;
        if (ui_number_double(ui, "##npw", &pw, 0.0, 1.0e7, 0.1, 2, 0))
            m_size_set_print(s, false, pw);
        if (ui_combo(ui, "##npu", &pu, print_units, 2)) s->print_unit = pu ? 1 : 0;
    }
    ui_label_ex(ui, "Height", UI_LABEL_DIM);
    {
        double ph = m_size_print(s, true);
        if (ui_number_double(ui, "##nph", &ph, 0.0, 1.0e7, 0.1, 2, 0))
            m_size_set_print(s, true, ph);
    }
    ui_label_ex(ui, print_units[s->print_unit], UI_LABEL_DIM);
    ui_layout_column(ui);
    valid = m_size_valid(s, why, sizeof why);
    if (!valid) {
        ui_rect lr;
        ui_layout_space(ui, 4.0f);
        lr = ui_layout_next(ui, 0, ui_px(ui, 18.0f));
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), lr, UI_ALIGN_LEFT,
                         UI_TEXT_ELLIPSIS, p->danger, why, strlen(why));
    }
    r = m_dlg_footer(a, valid);
    {
        uint32_t r2 = ui_dialog_end(ui);
        if (!r) r = r2;
    }
    if (enter && !r && valid) r = UI_DLG_OK;
    if (r == UI_DLG_OK && !valid) r = 0;           /* Enter in a box while invalid */
    if (!r) return true;
    if (r == UI_DLG_OK) {
        int32_t w = m_size_px(s, false), h = m_size_px(s, true);
        app_doc *d = app_doc_new_image(a, (uint32_t)w, (uint32_t)h,
                                       app_px_make(255, 255, 255, 255));
        if (!d) {
            app_error(a, "Could not create a %d x %d image: %s.", (int)w, (int)h,
                      pc_status_str(PC_ERR_NOMEM));
        } else {
            d->meta.dpi_x = d->meta.dpi_y = s->dpi;
            app_doc_set_untitled(a, d);
            (void)app_add_doc(a, d);
        }
    }
    /* remembered: aspect lock and units (MENUS.md New Image) */
    (void)app_settings_set_bool(a->settings, "file.new.keep_aspect", s->keep);
    (void)app_settings_set_int(a->settings, "file.new.res_unit", s->res_unit);
    (void)app_settings_set_int(a->settings, "file.new.print_unit", s->print_unit);
    return false;
}

/* Default size: the clipboard image (CB-NEWIMAGE-SIZE), else 800 x 600
 * scaled by the display scale. */
static void new_default_size(app *a, int32_t *w, int32_t *h)
{
    float ds = a->win ? SDL_GetWindowDisplayScale(a->win) : 1.0f;
    if (!(ds > 0.0f)) ds = 1.0f;
    *w = (int32_t)(800.0f * ds + 0.5f);
    *h = (int32_t)(600.0f * ds + 0.5f);
    if (a->win && pal_clip_has_image()) {
        uint8_t *data = NULL;
        size_t len = 0;
        char mime[64];
        if (pal_clip_get_image(&data, &len, mime, sizeof mime)) {
            pc_doc *doc = NULL;
            pc_image_meta m;
            pc_codec_limits lim;
            memset(&m, 0, sizeof m);
            pc_codec_limits_default(&lim);
            if (pc_codec_load_any(data, len, NULL, &lim, &doc, &m, NULL) == PC_OK && doc) {
                *w = (int32_t)doc->w;
                *h = (int32_t)doc->h;
                pc_doc_destroy(doc);
            }
            pc_meta_free(&m);
            free(data);
        }
    }
}

void app_new_image_dialog(app *a)
{
    new_dlg *n = (new_dlg *)calloc(1u, sizeof *n);
    int32_t w = 800, h = 600;
    bool keep, cm;
    if (!n) return;
    new_default_size(a, &w, &h);
    keep = app_settings_bool(a->settings, "file.new.keep_aspect", false);
    cm = app_settings_int(a->settings, "file.new.res_unit", 0) == 1;
    m_size_init(&n->s, (uint32_t)(w > 0 ? w : 1), (uint32_t)(h > 0 ? h : 1), 96.0, keep, cm);
    n->s.print_unit = (int)app_settings_int(a->settings, "file.new.print_unit", cm ? 1 : 0) == 1;
    (void)app_dialog_push(a, new_frame, n, free);
}
