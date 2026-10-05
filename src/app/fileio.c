/* fileio.c - File menu flows (MENUS.md File, FILES.md):
 *  - Open: native dialog with filters from the codec registry (multi
 *    select), decoding on a worker, errors with pc_status_str;
 *  - Save / Save As: native save dialog, file type from the extension or
 *    filter, the Flatten prompt for flat formats (an undoable history
 *    step), the Save Configuration dialog generated from the codec's
 *    fx_prop options with a live preview and file size, encoding of a
 *    retained snapshot on a worker and pal_write_file_atomic;
 *  - Close with the unsaved-changes prompt, recent files, New Image.
 * Main thread; workers only see owned job data. */
#include "app_internal.h"
#include "pc/pc_layerops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPEN_BYTES ((uint64_t)3u << 30)

/* ---- codecs and filters ------------------------------------------------------------- */
typedef struct filter_set {
    pal_filter      f[40];
    const pc_codec *codec[40];       /* codec of filter i (NULL for "All") */
    int             n;
    char            all[512];
} filter_set;

static void build_filters(filter_set *fs, bool save)
{
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    memset(fs, 0, sizeof *fs);
    if (!save) {
        fs->all[0] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (!(list[i]->flags & PC_CODEC_LOAD)) continue;
            if (fs->all[0]) strncat(fs->all, ";", sizeof fs->all - strlen(fs->all) - 1u);
            strncat(fs->all, list[i]->exts, sizeof fs->all - strlen(fs->all) - 1u);
        }
        fs->f[fs->n].name = "All images";
        fs->f[fs->n].pattern = fs->all;
        fs->codec[fs->n] = NULL;
        fs->n++;
    }
    for (size_t i = 0; i < n && fs->n < 38; i++) {
        const pc_codec *c = list[i];
        if (!(c->flags & (save ? PC_CODEC_SAVE : PC_CODEC_LOAD))) continue;
        fs->f[fs->n].name = c->name;
        fs->f[fs->n].pattern = c->exts;
        fs->codec[fs->n] = c;
        fs->n++;
    }
    if (!save) {
        fs->f[fs->n].name = "All files";
        fs->f[fs->n].pattern = "*";
        fs->codec[fs->n] = NULL;
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

static void set_dir_from(char *dir, size_t cap, const char *path)
{
    pal_path_dirname(dir, cap, path);
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
    if (a->nrecent == APP_MAX_RECENT) free(a->recent[--a->nrecent]);
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

/* ---- metadata copies for workers ----------------------------------------------------- */
static pc_status meta_copy(const pc_image_meta *src, pc_image_meta *dst)
{
    memset(dst, 0, sizeof *dst);
    dst->dpi_x = src->dpi_x;
    dst->dpi_y = src->dpi_y;
    dst->src_bits = src->src_bits;
    dst->had_alpha = src->had_alpha;
    memcpy(dst->note, src->note, sizeof dst->note);
    if (src->icc && src->icc_len) {
        dst->icc = (uint8_t *)malloc(src->icc_len);
        if (!dst->icc) return PC_ERR_NOMEM;
        memcpy(dst->icc, src->icc, src->icc_len);
        dst->icc_len = src->icc_len;
    }
    for (size_t i = 0; i < src->n_items; i++)
        if (pc_meta_add(dst, src->items[i].key, src->items[i].value) != PC_OK) {
            pc_meta_free(dst);
            return PC_ERR_NOMEM;
        }
    return PC_OK;
}

/* ---- open ---------------------------------------------------------------------------- */
typedef struct open_job {
    char           *path;
    pc_doc         *doc;
    pc_image_meta   meta;
    const pc_codec *codec;
    pc_status       st;
} open_job;

static int g_opening;

int app_opening_count(const app *a)
{
    (void)a;
    return g_opening;
}

static void open_work(void *ud)
{
    open_job *j = (open_job *)ud;
    uint8_t *data = NULL;
    size_t len = 0;
    pc_codec_limits lim;
    j->st = pal_read_file(j->path, MAX_OPEN_BYTES, &data, &len);
    if (j->st != PC_OK) return;
    pc_codec_limits_default(&lim);
    j->st = pc_codec_load_any(data, len, j->path, &lim, &j->doc, &j->meta, &j->codec);
    free(data);
}

static bool startup_untouched(app *a, app_doc *d)
{
    return a->startup_doc && d && d->id == a->startup_doc_id && !d->path && !app_doc_dirty(d) &&
           d->hist->cur == d->hist->root && d->hist->count == 1u;
}

static void open_done(app *a, void *ud)
{
    open_job *j = (open_job *)ud;
    g_opening--;
    if (j->st != PC_OK || !j->doc) {
        app_error(a, "Could not open \"%s\": %s.", j->path, pc_status_str(j->st));
    } else {
        app_doc *old = NULL;
        app_doc *d;
        for (int32_t i = 0; i < a->ndocs; i++)
            if (startup_untouched(a, a->docs[i])) old = a->docs[i];
        d = app_doc_create(a, j->doc, j->path, j->codec, &j->meta, "Open Image");
        j->doc = NULL;
        if (!d) {
            app_error(a, "Could not open \"%s\": %s.", j->path, pc_status_str(PC_ERR_NOMEM));
        } else if (app_add_doc(a, d)) {
            app_recent_add(a, j->path);
            set_dir_from(a->last_open_dir, sizeof a->last_open_dir, j->path);
            /* an untouched startup image is replaced by the first opened one */
            if (old) app_close_doc_now(a, old);
        }
    }
    pc_doc_destroy(j->doc);
    pc_meta_free(&j->meta);
    free(j->path);
    free(j);
}

bool app_open_path(app *a, const char *path)
{
    open_job *j;
    if (!path || !*path) return false;
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->path && strcmp(a->docs[i]->path, path) == 0) {
            app_set_active_doc(a, a->docs[i]);
            return true;
        }
    j = (open_job *)calloc(1u, sizeof *j);
    if (!j) return false;
    j->path = app_strdup(path);
    if (!j->path || !app_task(a, open_work, open_done, j)) {
        free(j->path);
        free(j);
        return false;
    }
    g_opening++;
    app_status(a, NULL);
    return true;
}

void app_open_paths(app *a, const char *const *paths, int n)
{
    for (int i = 0; i < n; i++) (void)app_open_path(a, paths[i]);
}

static void open_dialog_cb(void *ud, const char *const *paths, int n, int filter)
{
    app *a = (app *)ud;
    (void)filter;
    if (paths && n > 0) app_open_paths(a, paths, n);
}

void app_cmd_open_dialog(app *a)
{
    filter_set fs;
    const char *dir = a->last_open_dir[0] ? a->last_open_dir : pal_dir(PAL_DIR_PICTURES);
    if (!a->win) return;
    build_filters(&fs, false);
    pal_dialog_open(a->win, fs.f, fs.n, dir, true, open_dialog_cb, a);
}

/* ---- choice dialogs ------------------------------------------------------------------ */
typedef void (*choice_fn)(app *a, int choice, void *ud);   /* -1 = cancelled */

typedef struct choice_dlg {
    char         title[160];
    char        *text;
    ui_icon      icon;
    const char  *labels[4];
    int          n, def, cancel;
    SDL_Texture *thumb;            /* borrowed: owned by a document that outlives it */
    uint32_t     thumb_doc;
    choice_fn    done;
    void        *ud;
} choice_dlg;

static void choice_free(void *p)
{
    choice_dlg *c = (choice_dlg *)p;
    if (!c) return;
    free(c->text);
    free(c);
}

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == id) return a->docs[i];
    return NULL;
}

static bool choice_frame(app *a, void *st)
{
    choice_dlg *c = (choice_dlg *)st;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_size cells[5];
    int pick = -2;
    uint32_t r;
    ui_dialog_begin(ui, c->title, 440.0f, 0.0f);
    cells[0] = ui_size_px(c->thumb ? 92.0f : (c->icon ? 44.0f : 0.0f));
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    {
        app_doc *d = c->thumb_doc ? doc_by_id(a, c->thumb_doc) : NULL;
        if (d && d->thumb) {
            ui_rect box = ui_layout_next(ui, ui_px(ui, 84.0f), ui_px(ui, 64.0f));
            float s = (float)box.w / (float)d->thumb_w < (float)box.h / (float)d->thumb_h
                          ? (float)box.w / (float)d->thumb_w
                          : (float)box.h / (float)d->thumb_h;
            ui_rect img = ui_rect_make(box.x, box.y, (int32_t)((float)d->thumb_w * s),
                                       (int32_t)((float)d->thumb_h * s));
            ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
            ui_draw_image(ui, d->thumb, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
            ui_draw_rect_outline(ui, ui_rect_inset(img, -1, -1), 1, p->border_strong);
        } else {
            ui_rect ir = ui_layout_next(ui, ui_px(ui, 44.0f), ui_px(ui, 36.0f));
            if (c->icon)
                ui_draw_icon(ui, c->icon,
                             ui_rect_make(ir.x, ir.y, ui_px(ui, 32.0f), ui_px(ui, 32.0f)),
                             ui_px(ui, 32.0f), p->text_on_accent,
                             c->icon == UI_ICON_WARNING ? p->warning : p->accent);
        }
    }
    ui_layout_begin(ui, 0.0f);
    ui_layout_space(ui, 4.0f);
    ui_text_wrapped(ui, c->text, 0);
    ui_layout_end(ui);
    ui_layout_column(ui);
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_fr(1.0f);
    for (int i = 0; i < c->n; i++) cells[i + 1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, c->n + 1, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, ui_get_theme(ui)->m.control_h));
    for (int i = 0; i < c->n; i++)
        if (ui_button_ex(ui, c->labels[i], UI_ICON_NONE, i == c->def ? UI_BUTTON_PRIMARY : 0u))
            pick = i;
    ui_layout_column(ui);
    if (pick == -2 && (ui_key_take(ui, SDLK_RETURN, 0) || ui_key_take(ui, SDLK_KP_ENTER, 0)))
        pick = c->def;
    r = ui_dialog_end(ui);
    if (pick == -2 && r) pick = c->cancel;           /* Escape or the close button */
    if (pick == -2) return true;
    if (c->done) c->done(a, pick == c->cancel ? -1 : pick, c->ud);
    return false;
}

static void choice(app *a, const char *title, const char *text, ui_icon icon, const char *l0,
                   const char *l1, const char *l2, int def, int cancel, uint32_t thumb_doc,
                   choice_fn done, void *ud)
{
    static uint32_t seq;
    choice_dlg *c = (choice_dlg *)calloc(1u, sizeof *c);
    if (!c) { if (done) done(a, -1, ud); return; }
    snprintf(c->title, sizeof c->title, "%s##choice%u", title, (unsigned)++seq);
    c->text = app_strdup(text);
    c->icon = icon;
    c->labels[0] = l0;
    c->labels[1] = l1;
    c->labels[2] = l2;
    c->n = l2 ? 3 : (l1 ? 2 : 1);
    c->def = def;
    c->cancel = cancel;
    c->thumb_doc = thumb_doc;
    c->thumb = NULL;
    c->done = done;
    c->ud = ud;
    if (!c->text) { choice_free(c); if (done) done(a, -1, ud); return; }
    if (!app_dialog_push(a, choice_frame, c, choice_free) && done) done(a, -1, ud);
}

/* ---- save ---------------------------------------------------------------------------- */
typedef struct save_flow {
    uint32_t         doc_id;
    bool             save_as;
    char            *path;
    const pc_codec  *codec;
    void            *params;          /* owned, codec->params_size bytes */
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

static bool flow_set_codec(save_flow *f, const pc_codec *c, const void *params)
{
    void *p = NULL;
    if (c && c->params_size) {
        p = malloc(c->params_size);
        if (!p) return false;
        if (params) memcpy(p, params, c->params_size);
        else pc_codec_default_params(c, p);
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
    void           *params;          /* borrowed from the flow (alive until done) */
    char           *path;
    uint64_t        seq;
    pc_status       st;
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
}

static void write_finish(app *a, write_job *j)
{
    app_doc *d = doc_by_id(a, j->doc_id);
    if (j->st == PC_OK && d) {
        (void)app_doc_set_file(d, j->path, j->codec, j->params);
        d->saved_seq = j->seq;
        d->save_configured = true;
        app_recent_add(a, j->path);
        set_dir_from(a->last_save_dir, sizeof a->last_save_dir, j->path);
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
    else free(j->params);
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
    if (!j->path || !j->snap || meta_copy(&d->meta, &j->meta) != PC_OK) {
        pc_doc_destroy(j->snap);
        free(j->path);
        free(j);
        return NULL;
    }
    (void)params;
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
    j->params = f->params;
    j->flow = f;
    if (!app_task(a, write_work, write_done, j)) {
        write_work(j);                      /* no worker: save synchronously */
        write_done(a, j);
    }
}

/* ---- Save Configuration (FS-CONFIG): props + preview + file size --------------------- */
typedef struct cfg_dlg {
    save_flow   *flow;
    void        *params;               /* working copy */
    void        *initial;
    char         title[96];
    pc_doc      *snap;                 /* preview source */
    pc_image_meta meta;
    SDL_Texture *tex;
    int32_t      tw, th;
    char         info[96];
    uint32_t     gen;                  /* bumps with every change */
    bool         running, dirty;
    bool         ended;                /* the dialog closed while a job ran */
} cfg_dlg;

typedef struct cfg_job {
    cfg_dlg        *dlg;
    uint32_t        gen;
    pc_doc         *snap;
    pc_image_meta  *meta;
    const pc_codec *codec;
    void           *params;            /* owned copy */
    size_t          bytes;
    pc_status       st;
    uint8_t        *rgba;              /* owned preview, tw x th */
    int32_t         tw, th;
} cfg_job;

static void cfg_free(void *p)
{
    cfg_dlg *c = (cfg_dlg *)p;
    if (!c) return;
    if (c->running) {                  /* the worker still uses snap: free in its done */
        c->ended = true;
        return;
    }
    if (c->tex) SDL_DestroyTexture(c->tex);
    pc_doc_destroy(c->snap);
    pc_meta_free(&c->meta);
    free(c->params);
    free(c->initial);
    free(c);
}

/* Composite a loaded preview document into a small RGBA thumbnail. */
static uint8_t *preview_rgba(const pc_doc *d, int32_t *tw, int32_t *th)
{
    int32_t mw = 360, mh = 260, w, h;
    uint8_t *out;
    pc_px32 *row;
    double s = (double)mw / (double)d->w;
    if ((double)mh / (double)d->h < s) s = (double)mh / (double)d->h;
    if (s > 1.0) s = 1.0;
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
        pc_comp_rect(d, pc_rect_make(0, sy, (int32_t)d->w, 1), row, d->w, NULL);
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
            pc_codec_limits lim;
            pc_doc *back = NULL;
            pc_image_meta m;
            memset(&m, 0, sizeof m);
            pc_codec_limits_default(&lim);
            if (j->codec->load(out.p, out.n, &lim, &back, &m) == PC_OK && back) {
                j->rgba = preview_rgba(back, &j->tw, &j->th);
                pc_doc_destroy(back);
            }
            pc_meta_free(&m);
        }
    }
    pc_buf_free(&out);
}

static void cfg_start(app *a, cfg_dlg *c);

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
            double kb = (double)j->bytes / 1024.0;
            if (kb < 1024.0) snprintf(c->info, sizeof c->info, "File size: %.1f KB", kb);
            else snprintf(c->info, sizeof c->info, "File size: %.2f MB", kb / 1024.0);
        } else {
            snprintf(c->info, sizeof c->info, "Preview error: %s", pc_status_str(j->st));
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

static bool cfg_frame(app *a, void *st)
{
    cfg_dlg *c = (cfg_dlg *)st;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    const pc_codec *codec = c->flow->codec;
    ui_size cells[2];
    app_props_ctx pctx;
    uint32_t r;
    bool ok = false, cancel = false, enter;
    ui_dialog_begin(ui, c->title, 700.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_px(380.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_layout_begin(ui, 0.0f);
    memset(&pctx, 0, sizeof pctx);
    pctx.id = "##saveopts";
    if (app_props_ui(a, codec->props, codec->n_props, c->params, &pctx) & APP_PROPS_CHANGED) {
        c->gen++;
        if (c->running) c->dirty = true;
        else cfg_start(a, c);
    }
    ui_layout_space(ui, 6.0f);
    if (ui_button_ex(ui, "Defaults##cfgdef", UI_ICON_RESET, 0)) {
        pc_codec_default_params(codec, c->params);
        c->gen++;
        if (c->running) c->dirty = true;
        else cfg_start(a, c);
    }
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    {
        ui_rect box = ui_layout_next(ui, ui_px(ui, 372.0f), ui_px(ui, 272.0f));
        ui_draw_rrect(ui, box, 4.0f, p->field);
        ui_draw_rrect_outline(ui, box, 4.0f, 1, p->border);
        if (c->tex && c->tw > 0 && c->th > 0) {
            float s = (float)(box.w - 12) / (float)c->tw;
            ui_rect img;
            if ((float)(box.h - 12) / (float)c->th < s) s = (float)(box.h - 12) / (float)c->th;
            if (s > (float)ui_scale(ui)) s = ui_scale(ui);
            img = ui_rect_center(box, (int32_t)((float)c->tw * s), (int32_t)((float)c->th * s));
            ui_draw_checker(ui, img, ui_px(ui, 6.0f), p->checker_a, p->checker_b);
            ui_draw_image(ui, c->tex, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
        }
        ui_label_ex(ui, c->info, UI_LABEL_DIM);
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
        app_doc *d = doc_by_id(a, c->flow->doc_id);
        memcpy(c->flow->params, c->params, codec->params_size);
        if (d) d->save_configured = true;
        step_write(a, c->flow);
    } else {
        flow_finish(a, c->flow, false);
    }
    c->flow = NULL;
    return false;
}

static void step_config(app *a, save_flow *f)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    cfg_dlg *c;
    if (!d) { flow_finish(a, f, false); return; }
    if (!f->codec->n_props || !f->codec->params_size || (!f->save_as && d->save_configured &&
                                                          d->codec == f->codec)) {
        step_write(a, f);
        return;
    }
    c = (cfg_dlg *)calloc(1u, sizeof *c);
    if (!c) { flow_finish(a, f, false); return; }
    c->flow = f;
    snprintf(c->title, sizeof c->title, "Save Configuration: %s##savecfg", f->codec->name);
    c->params = malloc(f->codec->params_size);
    c->initial = malloc(f->codec->params_size);
    c->snap = app_doc_snapshot(d);
    if (!c->params || !c->initial || !c->snap || meta_copy(&d->meta, &c->meta) != PC_OK) {
        pc_doc_destroy(c->snap);
        c->snap = NULL;
        free(c->params);
        free(c->initial);
        free(c);
        flow_finish(a, f, false);
        return;
    }
    memcpy(c->params, f->params, f->codec->params_size);
    memcpy(c->initial, f->params, f->codec->params_size);
    if (!app_dialog_push(a, cfg_frame, c, cfg_free)) { flow_finish(a, f, false); return; }
    cfg_start(a, c);
}

static void flatten_choice(app *a, int pick, void *ud)
{
    save_flow *f = (save_flow *)ud;
    app_doc *d = doc_by_id(a, f->doc_id);
    if (pick != 0 || !d) { flow_finish(a, f, false); return; }
    if (pc_layerop_flatten(d->hist, &a->par, "Flatten") != PC_OK) {
        app_error(a, "Could not flatten the image.");
        flow_finish(a, f, false);
        return;
    }
    app_doc_history_changed(a, d);
    step_config(a, f);
}

static void step_flatten(app *a, save_flow *f)
{
    app_doc *d = doc_by_id(a, f->doc_id);
    if (!d) { flow_finish(a, f, false); return; }
    if (!(f->codec->flags & PC_CODEC_LAYERED) && d->doc->n_layers > 1u) {
        char text[512];
        snprintf(text, sizeof text,
                 "The %s file type stores a single layer. Flatten the image to save it? "
                 "Flattening is recorded in the history and can be undone.", f->codec->name);
        choice(a, "Flatten Image", text, UI_ICON_WARNING, "Flatten", "Cancel", NULL, 0, 1, 0,
               flatten_choice, f);
        return;
    }
    step_config(a, f);
}

typedef struct save_cb_ctx { app *a; save_flow *f; } save_cb_ctx;

/* Resolve the chosen path and filter into a codec and a path with an
 * extension. */
static void save_dialog_cb(void *ud, const char *const *paths, int n, int filter)
{
    save_cb_ctx *cx = (save_cb_ctx *)ud;
    app *a = cx->a;
    save_flow *f = cx->f;
    filter_set fs;
    const pc_codec *c;
    char path[2048];
    free(cx);
    if (!paths || n < 1 || !paths[0] || !*paths[0]) { flow_finish(a, f, false); return; }
    build_filters(&fs, true);
    app_copy_str(path, sizeof path, paths[0]);
    c = pc_codec_by_ext(pal_path_ext(path));
    if (!c || !(c->flags & PC_CODEC_SAVE)) {
        /* no or unknown extension: the selected filter decides, else keep the type */
        const pc_codec *fc = filter >= 0 && filter < fs.n ? fs.codec[filter] : f->codec;
        char ext[16];
        if (!fc) fc = pc_codec_by_id("png");
        if (!fc) { flow_finish(a, f, false); return; }
        default_ext(fc, ext, sizeof ext);
        if (strlen(path) + strlen(ext) + 2u < sizeof path) {
            strcat(path, ".");
            strcat(path, ext);
        }
        c = fc;
    }
    free(f->path);
    f->path = app_strdup(path);
    if (!f->path || !flow_set_codec(f, c, f->codec == c ? f->params : NULL)) {
        flow_finish(a, f, false);
        return;
    }
    step_flatten(a, f);
}

static void ask_path(app *a, save_flow *f, app_doc *d)
{
    filter_set fs;
    char def[2048], ext[16];
    const pc_codec *c = f->codec;
    save_cb_ctx *cx;
    if (!a->win) { flow_finish(a, f, false); return; }
    /* default type: .pdn for layered images, else the current type, else PNG */
    if (d->doc->n_layers > 1u && pc_codec_by_id("pdn")) c = pc_codec_by_id("pdn");
    else if (!c || !(c->flags & PC_CODEC_SAVE)) c = pc_codec_by_id("png");
    if (!c) { flow_finish(a, f, false); return; }
    default_ext(c, ext, sizeof ext);
    if (d->path) {
        char dir[1024];
        const char *base = pal_path_basename(d->path);
        const char *dot = strrchr(base, '.');
        size_t bl = dot ? (size_t)(dot - base) : strlen(base);
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
    if (!flow_set_codec(f, c, f->codec == c ? f->params : NULL)) {
        flow_finish(a, f, false);
        return;
    }
    build_filters(&fs, true);
    /* put the default type first so the dialog preselects it */
    for (int i = 0; i < fs.n; i++)
        if (fs.codec[i] == c && i > 0) {
            pal_filter tf = fs.f[i];
            const pc_codec *tc = fs.codec[i];
            memmove(&fs.f[1], &fs.f[0], (size_t)i * sizeof fs.f[0]);
            memmove((void *)&fs.codec[1], (const void *)&fs.codec[0],
                    (size_t)i * sizeof fs.codec[0]);
            fs.f[0] = tf;
            fs.codec[0] = tc;
        }
    cx = (save_cb_ctx *)malloc(sizeof *cx);
    if (!cx) { flow_finish(a, f, false); return; }
    cx->a = a;
    cx->f = f;
    pal_dialog_save(a->win, fs.f, fs.n, def, save_dialog_cb, cx);
}

void app_save_doc(app *a, app_doc *d, bool save_as, app_save_done_fn done, void *ud)
{
    save_flow *f;
    if (!d) { if (done) done(a, d, false, ud); return; }
    if (a->cv.captured) app_canvas_lost_capture(a);
    (void)app_tool_finish(a);
    f = (save_flow *)calloc(1u, sizeof *f);
    if (!f) { if (done) done(a, d, false, ud); return; }
    f->doc_id = d->id;
    f->save_as = save_as;
    f->done = done;
    f->ud = ud;
    if (!save_as && d->path && d->codec && (d->codec->flags & PC_CODEC_SAVE)) {
        f->path = app_strdup(d->path);
        if (!f->path || !flow_set_codec(f, d->codec, d->save_params)) {
            flow_finish(a, f, false);
            return;
        }
        step_flatten(a, f);
        return;
    }
    if (d->codec && (d->codec->flags & PC_CODEC_SAVE) &&
        !flow_set_codec(f, d->codec, d->save_params)) {
        flow_finish(a, f, false);
        return;
    }
    ask_path(a, f, d);
}

pc_status app_save_doc_to(app *a, app_doc *d, const char *path, const pc_codec *codec,
                          const void *params, bool sync)
{
    write_job *j;
    if (!d || !path) return PC_ERR_ARG;
    if (!codec) codec = pc_codec_by_ext(pal_path_ext(path));
    if (!codec || !(codec->flags & PC_CODEC_SAVE)) return PC_ERR_UNSUPPORTED;
    if (a->cv.captured) app_canvas_lost_capture(a);
    (void)app_tool_finish(a);
    if (!(codec->flags & PC_CODEC_LAYERED) && d->doc->n_layers > 1u) {
        pc_status st = pc_layerop_flatten(d->hist, &a->par, "Flatten");
        if (st != PC_OK) return st;
        app_doc_history_changed(a, d);
    }
    j = make_write_job(d, path, codec, params);
    if (!j) return PC_ERR_NOMEM;
    if (codec->params_size) {
        j->params = malloc(codec->params_size);
        if (!j->params) {
            pc_doc_destroy(j->snap);
            pc_meta_free(&j->meta);
            free(j->path);
            free(j);
            return PC_ERR_NOMEM;
        }
        if (params) memcpy(j->params, params, codec->params_size);
        else pc_codec_default_params(codec, j->params);
    }
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
        close_finish(a, c, false);
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
    snprintf(text, sizeof text, "Save changes to \"%s\" before closing? Unsaved changes are lost "
             "otherwise.", d->name);
    choice(a, "Unsaved Changes", text, UI_ICON_WARNING, "Save", "Don't Save", "Cancel", 0, 2, d->id,
           close_choice, c);
}

/* ---- New Image dialog (MENUS.md New Image) ------------------------------------------- */
typedef struct new_dlg {
    int32_t w, h;
    bool    keep;
    double  ratio;
    double  res;                 /* pixels per unit */
    int     res_unit;            /* 0 pixels/inch, 1 pixels/centimeter */
    double  pw, ph;              /* print size in inches or centimeters */
} new_dlg;

static int32_t g_new_w, g_new_h;   /* last used size (session) */
static bool g_new_keep;

static void new_sync_print(new_dlg *n)
{
    n->pw = (double)n->w / n->res;
    n->ph = (double)n->h / n->res;
}

static bool new_frame(app *a, void *st)
{
    new_dlg *n = (new_dlg *)st;
    ui_ctx *ui = a->ui;
    ui_size cells[3];
    uint32_t r;
    char est[96];
    double bytes = (double)n->w * (double)n->h * 4.0;
    static const char *const res_units[] = { "Pixels/inch", "Pixels/centimeter" };
    static const char *const print_units[] = { "Inches", "Centimeters" };
    bool enter;
    ui_dialog_begin(ui, "New Image##newimg", 420.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    if (bytes < 1024.0 * 1024.0) snprintf(est, sizeof est, "Size: %.1f KB", bytes / 1024.0);
    else snprintf(est, sizeof est, "Size: %.1f MB", bytes / (1024.0 * 1024.0));
    ui_label_ex(ui, est, UI_LABEL_DIM);
    ui_checkbox(ui, "Maintain aspect ratio##keep", &n->keep);
    if (n->keep && !(n->ratio > 0.0)) n->ratio = (double)n->w / (double)n->h;
    ui_heading(ui, "Pixel size");
    cells[0] = ui_size_px(110.0f);
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_px(60.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Width", UI_LABEL_DIM);
    if (ui_number_int(ui, "##nw", &n->w, 1, (int32_t)PC_MAX_DIM, 1, 0)) {
        if (n->keep && n->ratio > 0.0) {
            double hh = (double)n->w / n->ratio + 0.5;
            n->h = hh < 1.0 ? 1 : (hh > (double)PC_MAX_DIM ? (int32_t)PC_MAX_DIM : (int32_t)hh);
        }
        new_sync_print(n);
    }
    ui_label_ex(ui, "pixels", UI_LABEL_DIM);
    ui_label_ex(ui, "Height", UI_LABEL_DIM);
    if (ui_number_int(ui, "##nh", &n->h, 1, (int32_t)PC_MAX_DIM, 1, 0)) {
        if (n->keep && n->ratio > 0.0) {
            double ww = (double)n->h * n->ratio + 0.5;
            n->w = ww < 1.0 ? 1 : (ww > (double)PC_MAX_DIM ? (int32_t)PC_MAX_DIM : (int32_t)ww);
        }
        new_sync_print(n);
    }
    ui_label_ex(ui, "pixels", UI_LABEL_DIM);
    ui_layout_column(ui);
    if (!n->keep) n->ratio = (double)n->w / (double)n->h;
    ui_heading(ui, "Resolution");
    cells[2] = ui_size_px(150.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Resolution", UI_LABEL_DIM);
    if (ui_number_double(ui, "##nres", &n->res, 1.0, 10000.0, 1.0, 2, 0)) new_sync_print(n);
    {
        int u = n->res_unit;
        if (ui_combo(ui, "##nresu", &u, res_units, 2) && u != n->res_unit) {
            n->res = u == 1 ? n->res / 2.54 : n->res * 2.54;
            n->res_unit = u;
            new_sync_print(n);
        }
    }
    ui_layout_column(ui);
    ui_heading(ui, "Print size");
    ui_layout_row(ui, 0.0f, 3, cells);
    ui_label_ex(ui, "Width", UI_LABEL_DIM);
    if (ui_number_double(ui, "##npw", &n->pw, 0.01, 100000.0, 0.1, 2, 0)) {
        double w = n->pw * n->res + 0.5;
        n->w = w < 1.0 ? 1 : (w > (double)PC_MAX_DIM ? (int32_t)PC_MAX_DIM : (int32_t)w);
        if (n->keep && n->ratio > 0.0) n->h = (int32_t)((double)n->w / n->ratio + 0.5);
        if (n->h < 1) n->h = 1;
        new_sync_print(n);
    }
    ui_label_ex(ui, print_units[n->res_unit], UI_LABEL_DIM);
    ui_label_ex(ui, "Height", UI_LABEL_DIM);
    if (ui_number_double(ui, "##nph", &n->ph, 0.01, 100000.0, 0.1, 2, 0)) {
        double h = n->ph * n->res + 0.5;
        n->h = h < 1.0 ? 1 : (h > (double)PC_MAX_DIM ? (int32_t)PC_MAX_DIM : (int32_t)h);
        if (n->keep && n->ratio > 0.0) n->w = (int32_t)((double)n->h * n->ratio + 0.5);
        if (n->w < 1) n->w = 1;
        new_sync_print(n);
    }
    ui_label_ex(ui, print_units[n->res_unit], UI_LABEL_DIM);
    ui_layout_column(ui);
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    if (r == UI_DLG_OK) {
        app_doc *d = app_doc_new_image(a, (uint32_t)n->w, (uint32_t)n->h,
                                       app_px_make(255, 255, 255, 255));
        if (!d) {
            app_error(a, "Could not create a %d x %d image: %s.", (int)n->w, (int)n->h,
                      pc_status_str(PC_ERR_NOMEM));
        } else {
            double dpi = n->res_unit == 1 ? n->res * 2.54 : n->res;
            d->meta.dpi_x = d->meta.dpi_y = dpi;
            app_doc_set_untitled(a, d);
            (void)app_add_doc(a, d);
            g_new_w = n->w;
            g_new_h = n->h;
        }
    }
    g_new_keep = n->keep;
    return false;
}

void app_new_image_dialog(app *a)
{
    new_dlg *n = (new_dlg *)calloc(1u, sizeof *n);
    float ds = a->win ? SDL_GetWindowDisplayScale(a->win) : 1.0f;
    if (!n) return;
    if (!(ds > 0.0f)) ds = 1.0f;
    n->w = g_new_w > 0 ? g_new_w : (int32_t)(800.0f * ds + 0.5f);
    n->h = g_new_h > 0 ? g_new_h : (int32_t)(600.0f * ds + 0.5f);
    /* CB-NEWIMAGE-SIZE: the clipboard image size when there is one */
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
                n->w = (int32_t)doc->w;
                n->h = (int32_t)doc->h;
                pc_doc_destroy(doc);
            }
            pc_meta_free(&m);
            free(data);
        }
    }
    n->keep = g_new_keep;
    n->ratio = (double)n->w / (double)n->h;
    n->res = 96.0;
    n->res_unit = 0;
    new_sync_print(n);
    (void)app_dialog_push(a, new_frame, n, free);
}
