/* autosave.c - autosave and crash recovery (T-L8-02, app_io.h).
 *
 * Layout under the recovery root (<state>/recovery):
 *     <session>/session.ini      app, start time, pid and process start time
 *                                (Linux), written when the session starts
 *     <session>/heartbeat        "time=<unix seconds>", rewritten every
 *                                heartbeat period while the app runs
 *     <session>/doc-<n>.pdn      the image (our .pdn writer keeps layers,
 *                                names, blend modes, opacity, visibility,
 *                                resolution, ICC profile and metadata)
 *     <session>/doc-<n>.ini      manifest: name, original path and type,
 *                                time, size, layer count, pdn byte count
 * The pdn is written before its manifest, each atomically, so a manifest
 * always describes a complete image. A normal exit removes the session.
 *
 * A session belongs to a dead process when (1) this process is the only
 * possible paint.c for the root (single instance primary), or (2) on Linux
 * its pid is gone, a zombie, or reused (start time differs), or (3) its
 * heartbeat is older than three periods. Sessions that are alive are left
 * alone and looked at again later.
 *
 * Thread rules: the module state is main-thread only. Workers get owned
 * job records: a document snapshot (immutable shared tiles), copied
 * metadata and paths; they encode and write files and nothing else.
 * Ownership: jobs are freed on the main thread after their task finished. */
#include "io_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AS_KEY             "lane_i.autosave"
#define AS_DEFAULT_SECS    120.0
#define AS_HEARTBEAT_SECS  10.0
#define AS_STALE_PERIODS   3.0
#define AS_SETTINGS_SECS   30.0
#define AS_MANIFEST_MAX    ((uint64_t)64u << 10)

/* settings.c (lane I internals): .bak + atomic write of serialized text,
 * and the dirty flag after a background write. */
pc_status app_settings_write_text(const char *path, const char *text, size_t n);
void      app_settings_mark(app_settings *s, bool dirty);

/* ---- state ---------------------------------------------------------------------------- */
typedef struct as_doc {
    uint32_t doc_id;
    uint32_t file_no;            /* doc-<file_no>.* */
    uint64_t disk_seq;           /* history seq on disk (valid when on_disk) */
    uint64_t changed_at;         /* SDL ms when the unsaved state was first seen, 0 = none */
    bool     on_disk, busy, closed, alive;
    bool     due_now;            /* recovered image: write it in this session at once */
    char    *adopt[2];           /* recovered files to delete after the first autosave */
} as_doc;

typedef enum job_kind { JOB_DOC = 0, JOB_HEARTBEAT, JOB_SETTINGS, JOB_RESTORE, JOB_SCAN } job_kind;

typedef struct rec_item {
    app_recovery_info info;
    char  dir[1100];             /* session folder */
    char  base[32];              /* "doc-3" */
    bool  busy;                  /* being restored */
} rec_item;

typedef struct as_job {
    job_kind      kind;
    pal_task     *task;
    pc_status     st;
    /* JOB_DOC / JOB_RESTORE */
    uint32_t      doc_id;
    uint64_t      seq;
    pc_doc       *doc;           /* snapshot (DOC) or decoded image (RESTORE), owned */
    pc_image_meta meta;
    char         *pdn, *ini;     /* owned paths */
    char         *text;          /* manifest / heartbeat / settings text (owned) */
    size_t        text_len;
    rec_item      item;          /* JOB_RESTORE */
    app_load_info load;          /* JOB_RESTORE: what the load found (lane CODEC) */
    /* JOB_SCAN */
    rec_item     *items;
    int           nitems;
} as_job;

typedef struct as_state {
    bool      enabled;
    char      root[1024];        /* <root>/recovery */
    char      session[1100];     /* <root>/recovery/<sid> */
    char      sid[64];
    bool      session_made;
    double    interval_s;        /* < 0: settings value */
    double    heartbeat_s;
    bool      exclusive, prompt;
    int64_t   self_pid, self_pstart;
    as_doc   *docs;
    int32_t   ndocs, cap_docs;
    uint32_t  next_file_no;
    as_job  **jobs;
    int32_t   njobs, cap_jobs;
    uint64_t  next_heartbeat, next_settings, next_scan;
    bool      hb_busy, settings_busy, scan_busy;
    rec_item *items;
    int32_t   nitems;
    bool      prompt_open, startup_prompted;
} as_state;

static as_state *astate(const app *a) { return (as_state *)app_ext_get(a, AS_KEY); }

/* ---- small helpers --------------------------------------------------------------------- */
static void join(char *out, size_t cap, const char *dir, const char *name)
{
    pal_path_join(out, cap, dir, name);
}

static char *dup_join(const char *dir, const char *name)
{
    char buf[1200];
    join(buf, sizeof buf, dir, name);
    return app_strdup(buf);
}

/* Linux: pid and process start time from /proc/<pid>/stat (field 22).
 * Returns 0 when unknown, -1 when the process is gone or a zombie. */
static int proc_stat(const char *pid_or_self, int64_t *pid, int64_t *pstart)
{
#if defined(__linux__)
    char path[64];
    uint8_t *data = NULL;
    size_t len = 0;
    const char *p, *rp;
    int field;
    snprintf(path, sizeof path, "/proc/%s/stat", pid_or_self);
    if (!pal_is_dir("/proc/self")) return 0;
    if (pal_read_file(path, 4096u, &data, &len) != PC_OK) return -1;
    p = (const char *)data;
    if (pid) *pid = strtoll(p, NULL, 10);
    rp = strrchr(p, ')');
    if (!rp || rp[1] != ' ') { free(data); return 0; }
    p = rp + 2;                                  /* field 3: state */
    if (*p == 'Z' || *p == 'X' || *p == 'x') { free(data); return -1; }
    for (field = 3; field < 22 && *p; field++) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    if (pstart) *pstart = strtoll(p, NULL, 10);
    free(data);
    return 1;
#else
    (void)pid_or_self;
    (void)pid;
    (void)pstart;
    return 0;
#endif
}

/* Small key=value files (session.ini, manifests) through the settings
 * parser. Owned result, NULL when missing or unreadable. Any thread. */
static app_settings *read_kv(const char *path)
{
    uint8_t *data = NULL;
    size_t len = 0;
    app_settings *s;
    if (pal_read_file(path, AS_MANIFEST_MAX, &data, &len) != PC_OK) return NULL;
    s = app_settings_create();
    if (s) app_settings_parse(s, (const char *)data, len);
    free(data);
    return s;
}

static char *kv_text(app_settings *s, size_t *len)
{
    char *t = NULL;
    if (app_settings_serialize(s, &t, len) != PC_OK) return NULL;
    return t;
}

static double interval_of(const app *a, const as_state *s)
{
    double v = s->interval_s;
    if (v < 0.0) v = app_settings_double(a->settings, "file.autosave_interval", AS_DEFAULT_SECS);
    return v > 0.0 ? v : 0.0;
}

/* ---- jobs ---------------------------------------------------------------------------------- */
static void job_free(as_job *j)
{
    if (!j) return;
    pc_doc_destroy(j->doc);
    pc_meta_free(&j->meta);
    free(j->pdn);
    free(j->ini);
    free(j->text);
    free(j->items);
    free(j);
}

static bool job_submit(app *a, as_state *s, as_job *j, void (*fn)(void *))
{
    if (s->njobs == s->cap_jobs) {
        int32_t nc = s->cap_jobs ? s->cap_jobs * 2 : 8;
        as_job **n = (as_job **)realloc(s->jobs, (size_t)nc * sizeof *n);
        if (!n) return false;
        s->jobs = n;
        s->cap_jobs = nc;
    }
    j->task = pal_task_submit(app_pool(a), fn, j);
    if (!j->task) return false;
    s->jobs[s->njobs++] = j;
    app_request_frame_at(a, SDL_GetTicks() + 30u);
    return true;
}

/* Write the pdn, then the manifest. */
static void doc_work(void *ud)
{
    as_job *j = (as_job *)ud;
    const pc_codec *pdn = pc_codec_by_id("pdn");
    pc_buf out;
    memset(&out, 0, sizeof out);
    if (!pdn || !pdn->save) { j->st = PC_ERR_UNSUPPORTED; return; }
    j->st = pdn->save(j->doc, &j->meta, NULL, NULL, &out);
    if (j->st == PC_OK) j->st = pal_write_file_atomic(j->pdn, out.p, out.n);
    if (j->st == PC_OK) {
        app_settings *m = app_settings_create();
        char num[32];
        if (!m) {
            j->st = PC_ERR_NOMEM;
        } else {
            app_settings_parse(m, j->text, j->text_len);       /* the prepared fields */
            snprintf(num, sizeof num, "%llu", (unsigned long long)out.n);
            (void)app_settings_set(m, "bytes", num);
            free(j->text);
            j->text = kv_text(m, &j->text_len);
            app_settings_destroy(m);
            j->st = j->text ? pal_write_file_atomic(j->ini, j->text, j->text_len) : PC_ERR_NOMEM;
        }
    }
    pc_buf_free(&out);
}

static void text_work(void *ud)
{
    as_job *j = (as_job *)ud;
    if (j->kind == JOB_SETTINGS) j->st = app_settings_write_text(j->pdn, j->text, j->text_len);
    else j->st = pal_write_file_atomic(j->pdn, j->text, j->text_len);
}

/* ---- session ------------------------------------------------------------------------------ */
static bool make_session(app *a, as_state *s)
{
    app_settings *kv;
    char *text, path[1200], num[32];
    size_t len = 0;
    pc_status st;
    if (s->session_made) return true;
    if (!s->enabled) return false;
    if (!pal_is_dir(s->session) && !pal_mkdirs(s->session)) {
        pal_log(PAL_LOG_WARN, "autosave: cannot create %s", s->session);
        return false;
    }
    kv = app_settings_create();
    if (!kv) return false;
    (void)app_settings_set_int(kv, "version", 1);
    (void)app_settings_set(kv, "app", APP_NAME " " APP_VERSION);
    (void)app_settings_set_int(kv, "start", io_unix_now());
    snprintf(num, sizeof num, "%lld", (long long)s->self_pid);
    (void)app_settings_set(kv, "pid", num);
    snprintf(num, sizeof num, "%lld", (long long)s->self_pstart);
    (void)app_settings_set(kv, "pstart", num);
    text = kv_text(kv, &len);
    app_settings_destroy(kv);
    if (!text) return false;
    join(path, sizeof path, s->session, "session.ini");
    st = pal_write_file_atomic(path, text, len);           /* once per session, tiny */
    free(text);
    if (st != PC_OK) return false;
    s->session_made = true;
    s->next_heartbeat = 0;                                  /* stamp at the next frame */
    (void)a;
    return true;
}

static void heartbeat(app *a, as_state *s, uint64_t now)
{
    as_job *j;
    char text[64];
    if (!s->session_made || s->hb_busy || now < s->next_heartbeat) return;
    s->next_heartbeat = now + (uint64_t)(s->heartbeat_s * 1000.0);
    j = (as_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->kind = JOB_HEARTBEAT;
    snprintf(text, sizeof text, "time=%lld\n", (long long)io_unix_now());
    j->text = app_strdup(text);
    j->text_len = strlen(text);
    j->pdn = dup_join(s->session, "heartbeat");
    if (!j->text || !j->pdn || !job_submit(a, s, j, text_work)) { job_free(j); return; }
    s->hb_busy = true;
}

/* Settings survive a crash too: flushed in the background when changed. */
static void settings_flush(app *a, as_state *s, uint64_t now)
{
    as_job *j;
    if (!a->settings_enabled || s->settings_busy || now < s->next_settings) return;
    s->next_settings = now + (uint64_t)(AS_SETTINGS_SECS * 1000.0);
    if (!a->ui) return;
    app_settings_store_ui(a);
    if (!app_settings_dirty(a->settings)) return;
    j = (as_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->kind = JOB_SETTINGS;
    j->pdn = app_strdup(a->settings_path);
    if (j->pdn && app_settings_serialize(a->settings, &j->text, &j->text_len) == PC_OK &&
        job_submit(a, s, j, text_work)) {
        app_settings_mark(a->settings, false);
        s->settings_busy = true;
        return;
    }
    job_free(j);
}

/* ---- per-document bookkeeping ------------------------------------------------------------- */
static as_doc *find_doc(as_state *s, uint32_t id)
{
    for (int32_t i = 0; i < s->ndocs; i++)
        if (s->docs[i].doc_id == id) return &s->docs[i];
    return NULL;
}

static as_doc *get_doc(as_state *s, uint32_t id)
{
    as_doc *d = find_doc(s, id);
    if (d) return d;
    if (s->ndocs == s->cap_docs) {
        int32_t nc = s->cap_docs ? s->cap_docs * 2 : 8;
        as_doc *n = (as_doc *)realloc(s->docs, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        s->docs = n;
        s->cap_docs = nc;
    }
    d = &s->docs[s->ndocs++];
    memset(d, 0, sizeof *d);
    d->doc_id = id;
    d->file_no = ++s->next_file_no;
    return d;
}

static void doc_files(const as_state *s, const as_doc *d, char *pdn, char *ini, size_t cap)
{
    char name[48];
    snprintf(name, sizeof name, "doc-%u.pdn", (unsigned)d->file_no);
    join(pdn, cap, s->session, name);
    snprintf(name, sizeof name, "doc-%u.ini", (unsigned)d->file_no);
    join(ini, cap, s->session, name);
}

/* Delete the files of a session when no image is left in it. */
static void prune_session_dir(const char *dir)
{
    char **names = NULL;
    int n = pal_list_dir(dir, "doc-*.ini", &names);
    char path[1200];
    pal_free_names(names, n);
    if (n > 0) return;
    n = pal_list_dir(dir, NULL, &names);
    for (int i = 0; i < n; i++) {
        join(path, sizeof path, dir, names[i]);
        (void)pal_remove(path);
    }
    pal_free_names(names, n);
    (void)pal_remove(dir);
}

static void drop_adopted(as_doc *d)
{
    char dir[1100];
    bool any = false;
    for (int k = 0; k < 2; k++)
        if (d->adopt[k]) {
            (void)pal_remove(d->adopt[k]);
            pal_path_dirname(dir, sizeof dir, d->adopt[k]);
            free(d->adopt[k]);
            d->adopt[k] = NULL;
            any = true;
        }
    if (any) prune_session_dir(dir);
}

static void remove_doc_files(as_state *s, as_doc *d)
{
    char pdn[1200], ini[1200];
    doc_files(s, d, pdn, ini, sizeof pdn);
    (void)pal_remove(ini);                       /* manifest first: never a dangling one */
    (void)pal_remove(pdn);
    d->on_disk = false;
    d->disk_seq = 0;
}

static void forget_doc(as_state *s, as_doc *d)
{
    drop_adopted(d);
    *d = s->docs[--s->ndocs];
}

/* Start writing d's current state. */
static bool start_doc_job(app *a, as_state *s, app_doc *doc, as_doc *ad)
{
    as_job *j;
    app_settings *m;
    char pdn[1200], ini[1200];
    if (!make_session(a, s)) return false;
    j = (as_job *)calloc(1u, sizeof *j);
    m = app_settings_create();
    if (!j || !m) { free(j); app_settings_destroy(m); return false; }
    j->kind = JOB_DOC;
    j->doc_id = doc->id;
    j->seq = doc->hist->cur->seq;
    j->doc = app_doc_snapshot(doc);
    doc_files(s, ad, pdn, ini, sizeof pdn);
    j->pdn = app_strdup(pdn);
    j->ini = app_strdup(ini);
    (void)app_settings_set_int(m, "version", 1);
    (void)app_settings_set(m, "name", doc->name);
    if (doc->path) (void)app_settings_set(m, "path", doc->path);
    if (doc->codec) (void)app_settings_set(m, "codec", doc->codec->id);
    (void)app_settings_set_int(m, "time", io_unix_now());
    (void)app_settings_set_int(m, "width", (int64_t)doc->doc->w);
    (void)app_settings_set_int(m, "height", (int64_t)doc->doc->h);
    (void)app_settings_set_int(m, "layers", (int64_t)doc->doc->n_layers);
    j->text = kv_text(m, &j->text_len);
    app_settings_destroy(m);
    if (!j->doc || !j->pdn || !j->ini || !j->text || io_meta_copy(&doc->meta, &j->meta) != PC_OK ||
        !job_submit(a, s, j, doc_work)) {
        job_free(j);
        return false;
    }
    ad->busy = true;
    return true;
}

static void finish_job(app *a, as_state *s, as_job *j)
{
    switch (j->kind) {
    case JOB_DOC: {
        as_doc *ad = find_doc(s, j->doc_id);
        if (!ad) {                               /* forgotten meanwhile */
            (void)pal_remove(j->ini);
            (void)pal_remove(j->pdn);
            break;
        }
        ad->busy = false;
        if (j->st != PC_OK) {
            pal_log(PAL_LOG_WARN, "autosave of %s failed: %s", j->pdn, pc_status_str(j->st));
            ad->changed_at = SDL_GetTicks();     /* retry after another interval */
            break;
        }
        ad->on_disk = true;
        ad->disk_seq = j->seq;
        drop_adopted(ad);                        /* recovered files are superseded */
        if (ad->closed) {                        /* closed while writing */
            remove_doc_files(s, ad);
            forget_doc(s, ad);
        }
        break;
    }
    case JOB_HEARTBEAT: s->hb_busy = false; break;
    case JOB_SETTINGS:
        s->settings_busy = false;
        if (j->st != PC_OK) app_settings_mark(a->settings, true);
        break;
    default: break;
    }
}

static void poll_jobs(app *a, as_state *s, bool wait);

/* ---- recovery: scanning ------------------------------------------------------------------- */
typedef struct scan_in {
    char    root[1024];
    char    own[64];
    bool    exclusive;
    double  stale_s;
    int64_t now;
} scan_in;

static bool session_dead(const scan_in *in, const char *dir)
{
    char path[1200];
    app_settings *kv;
    int64_t t = 0;
    if (in->exclusive) return true;
    join(path, sizeof path, dir, "session.ini");
    kv = read_kv(path);
    if (kv) {
        int64_t pid = app_settings_int(kv, "pid", 0), pstart = app_settings_int(kv, "pstart", 0);
        app_settings_destroy(kv);
        if (pid > 0) {
            char ps[32];
            int64_t got_start = 0;
            int r;
            snprintf(ps, sizeof ps, "%lld", (long long)pid);
            r = proc_stat(ps, NULL, &got_start);
            if (r < 0) return true;              /* gone or a zombie */
            if (r > 0) return pstart != 0 && got_start != pstart;   /* pid reused */
        }
    }
    join(path, sizeof path, dir, "heartbeat");
    kv = read_kv(path);
    if (kv) {
        t = app_settings_int(kv, "time", 0);
        app_settings_destroy(kv);
    } else {
        join(path, sizeof path, dir, "session.ini");
        kv = read_kv(path);
        if (kv) {
            t = app_settings_int(kv, "start", 0);
            app_settings_destroy(kv);
        }
    }
    return (double)(in->now - t) > in->stale_s;
}

static bool add_item(rec_item **items, int *n, int *cap, const rec_item *it)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        rec_item *ni = (rec_item *)realloc(*items, (size_t)nc * sizeof *ni);
        if (!ni) return false;
        *items = ni;
        *cap = nc;
    }
    (*items)[(*n)++] = *it;
    return true;
}

/* Items of every dead session under in->root. Any thread. */
static void scan_dirs(const scan_in *in, rec_item **items, int *nitems)
{
    char **names = NULL;
    int n = pal_list_dir(in->root, NULL, &names), cap = 0;
    *items = NULL;
    *nitems = 0;
    for (int i = 0; i < n; i++) {
        char dir[1100], **docs = NULL;
        int nd, found = 0;
        if (strcmp(names[i], in->own) == 0 || names[i][0] == '.') continue;
        join(dir, sizeof dir, in->root, names[i]);
        if (!pal_is_dir(dir) || !session_dead(in, dir)) continue;
        nd = pal_list_dir(dir, "doc-*.ini", &docs);
        for (int k = 0; k < nd; k++) {
            char ini[1200], pdn[1200];
            app_settings *m;
            rec_item it;
            size_t bl = strlen(docs[k]);
            memset(&it, 0, sizeof it);
            if (bl < 5u || bl - 4u >= sizeof it.base) continue;
            memcpy(it.base, docs[k], bl - 4u);
            join(ini, sizeof ini, dir, docs[k]);
            snprintf(pdn, sizeof pdn, "%.*s.pdn", (int)(strlen(ini) - 4u), ini);
            m = read_kv(ini);
            if (!m) continue;
            if (pal_file_exists(pdn) && app_settings_get(m, "bytes")) {
                const char *v;
                app_copy_str(it.dir, sizeof it.dir, dir);
                v = app_settings_get(m, "name");
                app_copy_str(it.info.name, sizeof it.info.name, v ? v : "Untitled");
                v = app_settings_get(m, "path");
                app_copy_str(it.info.path, sizeof it.info.path, v ? v : "");
                v = app_settings_get(m, "codec");
                app_copy_str(it.info.codec, sizeof it.info.codec, v ? v : "");
                it.info.time = app_settings_int(m, "time", 0);
                it.info.w = (uint32_t)app_settings_int(m, "width", 0);
                it.info.h = (uint32_t)app_settings_int(m, "height", 0);
                it.info.layers = (uint32_t)app_settings_int(m, "layers", 0);
                if (add_item(items, nitems, &cap, &it)) found++;
            }
            app_settings_destroy(m);
        }
        pal_free_names(docs, nd);
        if (found == 0) prune_session_dir(dir);  /* nothing to recover: remove it */
    }
    pal_free_names(names, n);
}

static void scan_prepare(const as_state *s, scan_in *in)
{
    memset(in, 0, sizeof *in);
    app_copy_str(in->root, sizeof in->root, s->root);
    app_copy_str(in->own, sizeof in->own, s->sid);
    in->exclusive = s->exclusive;
    in->stale_s = s->heartbeat_s * AS_STALE_PERIODS;
    in->now = io_unix_now();
}

static void scan_work(void *ud)
{
    as_job *j = (as_job *)ud;
    scan_in in;
    memcpy(&in, j->text, sizeof in);
    scan_dirs(&in, &j->items, &j->nitems);
}

/* True when item it is a recovered image this session still holds on to. */
static bool adopted(const as_state *s, const rec_item *it)
{
    char pdn[1300], base[1200];
    join(base, sizeof base, it->dir, it->base);
    snprintf(pdn, sizeof pdn, "%s.pdn", base);
    for (int32_t i = 0; i < s->ndocs; i++)
        if (s->docs[i].adopt[0] && strcmp(s->docs[i].adopt[0], pdn) == 0) return true;
    return false;
}

/* Merge a scan result: keep items being restored, add new ones. */
static int merge_items(as_state *s, rec_item *items, int n)
{
    int added = 0;
    rec_item *all = (rec_item *)malloc(((size_t)n + (size_t)s->nitems + 1u) * sizeof *all);
    int k = 0;
    if (!all) return 0;
    for (int32_t i = 0; i < s->nitems; i++)
        if (s->items[i].busy) all[k++] = s->items[i];
    for (int i = 0; i < n; i++) {
        bool dup = adopted(s, &items[i]);
        for (int q = 0; q < k; q++)
            if (strcmp(all[q].dir, items[i].dir) == 0 && strcmp(all[q].base, items[i].base) == 0)
                dup = true;
        if (!dup) {
            bool known = false;
            for (int32_t q = 0; q < s->nitems; q++)
                if (strcmp(s->items[q].dir, items[i].dir) == 0 &&
                    strcmp(s->items[q].base, items[i].base) == 0)
                    known = true;
            if (!known) added++;
            all[k++] = items[i];
        }
    }
    free(s->items);
    s->items = all;
    s->nitems = k;
    return added;
}

int app_recovery_scan(app *a)
{
    as_state *s = astate(a);
    scan_in in;
    rec_item *items = NULL;
    int n = 0;
    if (!s || !s->enabled) return 0;
    scan_prepare(s, &in);
    scan_dirs(&in, &items, &n);
    (void)merge_items(s, items, n);
    free(items);
    return app_recovery_count(a);
}

int app_recovery_count(const app *a)
{
    const as_state *s = astate(a);
    int n = 0;
    if (!s) return 0;
    for (int32_t i = 0; i < s->nitems; i++)
        if (!s->items[i].busy) n++;
    return n;
}

/* i-th item that is not being restored. */
static rec_item *item_at(as_state *s, int i)
{
    for (int32_t k = 0; s && k < s->nitems; k++)
        if (!s->items[k].busy && i-- == 0) return &s->items[k];
    return NULL;
}

bool app_recovery_get(const app *a, int i, app_recovery_info *out)
{
    rec_item *it = item_at(astate(a), i);
    if (!it || !out) return false;
    *out = it->info;
    return true;
}

static void remove_item(as_state *s, rec_item *it)
{
    int32_t k = (int32_t)(it - s->items);
    memmove(&s->items[k], &s->items[k + 1], (size_t)(s->nitems - k - 1) * sizeof *s->items);
    s->nitems--;
}

int app_recovery_discard(app *a, int i)
{
    as_state *s = astate(a);
    int done = 0;
    if (!s) return 0;
    for (;;) {
        rec_item *it = item_at(s, i < 0 ? 0 : i);
        char path[1200], dir[1100];
        if (!it) break;
        join(path, sizeof path, it->dir, it->base);
        app_copy_str(dir, sizeof dir, it->dir);
        {
            char f[1300];
            snprintf(f, sizeof f, "%s.ini", path);
            (void)pal_remove(f);
            snprintf(f, sizeof f, "%s.pdn", path);
            (void)pal_remove(f);
        }
        remove_item(s, it);
        prune_session_dir(dir);
        done++;
        if (i >= 0) break;
    }
    return done;
}

/* ---- recovery: restoring ------------------------------------------------------------------ */
static void restore_work(void *ud)
{
    as_job *j = (as_job *)ud;
    const pc_codec *pdn = pc_codec_by_id("pdn");
    /* lane CODEC (wave 4): the limits of File > Open (app_load_file), so
     * every image that opens can also be recovered */
    if (!pdn) {
        j->st = PC_ERR_UNSUPPORTED;
        return;
    }
    j->st = app_load_file(j->pdn, pdn, &j->doc, &j->meta, NULL, &j->load);
}

static void restore_done(app *a, as_state *s, as_job *j)
{
    rec_item *it = NULL;
    app_doc *d;
    const pc_codec *codec;
    for (int32_t k = 0; k < s->nitems; k++)
        if (s->items[k].busy && strcmp(s->items[k].dir, j->item.dir) == 0 &&
            strcmp(s->items[k].base, j->item.base) == 0)
            it = &s->items[k];
    if (it) remove_item(s, it);
    if (j->st != PC_OK || !j->doc) {
        char msg[1400];
        app_load_error_text(msg, sizeof msg, "recover", j->item.info.name,
                            j->st == PC_OK ? PC_ERR_FORMAT : j->st, &j->load);
        app_error(a, "%s", msg);
        return;
    }
    codec = j->item.info.codec[0] ? pc_codec_by_id(j->item.info.codec) : NULL;
    d = app_doc_create(a, j->doc, j->item.info.path[0] ? j->item.info.path : NULL, codec,
                       &j->meta, "Recovered Image");
    j->doc = NULL;
    if (!d) {
        app_error(a, "Could not recover \"%s\": %s.", j->item.info.name,
                  pc_status_str(PC_ERR_NOMEM));
        return;
    }
    app_copy_str(d->name, sizeof d->name, j->item.info.name);
    d->saved_seq = UINT64_MAX;                   /* unsaved: never equal to a history seq */
    {
        /* like opening a file: an untouched startup image makes room */
        app_doc *startup = NULL;
        for (int32_t k = 0; k < a->ndocs; k++) {
            app_doc *o = a->docs[k];
            if (a->startup_doc && o->id == a->startup_doc_id && !o->path && !app_doc_dirty(o) &&
                o->hist->cur == o->hist->root && o->hist->count == 1u)
                startup = o;
        }
        if (!app_add_doc(a, d)) return;
        if (startup) app_close_doc_now(a, startup);
    }
    {
        as_doc *ad = get_doc(s, d->id);
        if (ad) {
            ad->adopt[0] = j->pdn;
            ad->adopt[1] = j->ini;
            j->pdn = NULL;
            j->ini = NULL;
            ad->due_now = true;                  /* rewrite it in this session at once */
        }
    }
}

int app_recovery_restore(app *a, int i)
{
    as_state *s = astate(a);
    int started = 0;
    if (!s) return 0;
    for (;;) {
        rec_item *it = item_at(s, i < 0 ? 0 : i);
        as_job *j;
        char path[1200];
        if (!it) break;
        j = (as_job *)calloc(1u, sizeof *j);
        if (!j) break;
        j->kind = JOB_RESTORE;
        j->item = *it;
        join(path, sizeof path, it->dir, it->base);
        {
            char f[1300];
            snprintf(f, sizeof f, "%s.pdn", path);
            j->pdn = app_strdup(f);
            snprintf(f, sizeof f, "%s.ini", path);
            j->ini = app_strdup(f);
        }
        if (!j->pdn || !j->ini || !job_submit(a, s, j, restore_work)) {
            job_free(j);
            break;
        }
        it->busy = true;
        started++;
        if (i >= 0) break;
    }
    return started;
}

/* ---- the recovery dialog ------------------------------------------------------------------- */
typedef struct rec_dlg {
    bool *checked;               /* per item when the dialog opened */
    int   n;
} rec_dlg;

static void rec_dlg_free(void *p)
{
    rec_dlg *r = (rec_dlg *)p;
    if (!r) return;
    free(r->checked);
    free(r);
}

static void format_time(int64_t t, char *out, size_t cap)
{
    SDL_DateTime dt;
    if (t <= 0 || !SDL_TimeToDateTime((SDL_Time)t * SDL_NS_PER_SECOND, &dt, true)) {
        snprintf(out, cap, "unknown time");
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour,
             dt.minute);
}

static bool rec_frame(app *a, void *st)
{
    rec_dlg *r = (rec_dlg *)st;
    as_state *s = astate(a);
    ui_ctx *ui = a->ui;
    int n = app_recovery_count(a), pick = -2, nsel = 0;
    uint32_t res;
    ui_size cells[4];
    if (!s || n == 0) {
        if (s) s->prompt_open = false;
        return false;
    }
    if (r->n != n) {                             /* the list changed (rescan) */
        bool *c = (bool *)realloc(r->checked, (size_t)n * sizeof *c);
        if (!c) return true;
        for (int i = r->n; i < n; i++) c[i] = true;
        r->checked = c;
        r->n = n;
    }
    ui_dialog_begin(ui, "Recover Images##recovery", 560.0f, 0.0f);
    ui_text_wrapped(ui, "paint.c did not close normally last time. These images had changes "
                    "that were not saved, and copies of them were kept:", 0);
    ui_layout_space(ui, 6.0f);
    for (int i = 0; i < n; i++) {
        app_recovery_info info;
        char label[400], line[1400], when[48];
        if (!app_recovery_get(a, i, &info)) continue;
        format_time(info.time, when, sizeof when);
        snprintf(label, sizeof label, "%s##rec%d", info.name, i);
        ui_checkbox(ui, label, &r->checked[i]);
        snprintf(line, sizeof line, "%u x %u, %u layer%s, kept %s%s%s", (unsigned)info.w,
                 (unsigned)info.h, (unsigned)info.layers, info.layers == 1u ? "" : "s", when,
                 info.path[0] ? ", from " : ", never saved", info.path);
        ui_label_ex(ui, line, UI_LABEL_DIM | UI_LABEL_SMALL);
        if (r->checked[i]) nsel++;
    }
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    cells[2] = ui_size_auto();
    cells[3] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 4, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, ui_get_theme(ui)->m.control_h));
    if (ui_button_ex(ui, "Recover##rec_ok", UI_ICON_OPEN,
                     UI_BUTTON_PRIMARY | (nsel ? 0u : UI_DISABLED)))
        pick = 0;
    if (ui_button_ex(ui, "Discard##rec_del", UI_ICON_CLOSE, nsel ? 0u : UI_DISABLED)) pick = 1;
    if (ui_button_ex(ui, "Not Now##rec_later", UI_ICON_NONE, 0)) pick = 2;
    ui_layout_column(ui);
    /* Enter or Alt+R recovers, Alt+D discards, Escape is Not Now */
    if (pick == -2 && a->dlg_top && nsel) {
        if (ui_key_take(ui, SDLK_RETURN, 0) || ui_key_take(ui, SDLK_KP_ENTER, 0) ||
            ui_key_take(ui, SDLK_R, UI_MOD_ALT))
            pick = 0;
        else if (ui_key_take(ui, SDLK_D, UI_MOD_ALT))
            pick = 1;
    }
    res = ui_dialog_end(ui);
    if (pick == -2 && res) pick = 2;
    if (pick == -2) return true;
    /* act on the checked items, last first so indexes stay valid */
    if (pick == 0 || pick == 1)
        for (int i = n - 1; i >= 0; i--)
            if (r->checked[i]) {
                if (pick == 0) (void)app_recovery_restore(a, i);
                else (void)app_recovery_discard(a, i);
            }
    s->prompt_open = false;
    return false;
}

void app_recovery_prompt(app *a)
{
    as_state *s = astate(a);
    rec_dlg *r;
    if (!s || s->prompt_open || app_recovery_count(a) == 0) return;
    r = (rec_dlg *)calloc(1u, sizeof *r);
    if (!r) return;
    s->prompt_open = true;
    if (!app_dialog_push(a, rec_frame, r, rec_dlg_free)) s->prompt_open = false;
}

/* ---- the frame hook ------------------------------------------------------------------------- */
static void poll_jobs(app *a, as_state *s, bool wait)
{
    for (int32_t i = 0; i < s->njobs;) {
        as_job *j = s->jobs[i];
        if (!wait && !pal_task_done(j->task)) { i++; continue; }
        if (wait) pal_task_wait(j->task);
        pal_task_free(j->task);
        j->task = NULL;
        s->jobs[i] = s->jobs[--s->njobs];
        if (j->kind == JOB_RESTORE) {
            restore_done(a, s, j);
        } else if (j->kind == JOB_SCAN) {
            s->scan_busy = false;
            if (merge_items(s, j->items, j->nitems) > 0 && !s->prompt_open && s->prompt)
                app_recovery_prompt(a);
            j->nitems = 0;
        } else {
            finish_job(a, s, j);
        }
        job_free(j);
        app_request_frame(a);
        i = 0;
    }
}

static void start_scan(app *a, as_state *s)
{
    as_job *j;
    scan_in in;
    if (s->scan_busy) return;
    j = (as_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->kind = JOB_SCAN;
    scan_prepare(s, &in);
    j->text = (char *)malloc(sizeof in);
    if (!j->text) { job_free(j); return; }
    memcpy(j->text, &in, sizeof in);
    if (!job_submit(a, s, j, scan_work)) { job_free(j); return; }
    s->scan_busy = true;
}

static void as_frame(app *a, app_doc *unused, void *ud)
{
    as_state *s = astate(a);
    uint64_t now = SDL_GetTicks();
    double interval;
    (void)unused;
    (void)ud;
    if (!s) return;
    poll_jobs(a, s, false);
    if (s->njobs > 0) app_request_frame_at(a, now + 30u);
    settings_flush(a, s, now);
    if (!s->enabled) return;
    /* recovery: first frame, then now and then while sessions may still be
     * alive (no exclusivity) */
    if (!s->startup_prompted) {
        s->startup_prompted = true;
        s->next_scan = now;
    }
    if (now >= s->next_scan && !s->scan_busy) {
        start_scan(a, s);
        s->next_scan = s->exclusive ? UINT64_MAX
                                    : now + (uint64_t)(s->heartbeat_s * AS_STALE_PERIODS * 1000.0);
    }
    heartbeat(a, s, now);
    if (s->session_made) app_request_frame_at(a, s->next_heartbeat);
    if (!s->exclusive && s->next_scan != UINT64_MAX) app_request_frame_at(a, s->next_scan);
    interval = interval_of(a, s);
    for (int32_t i = 0; i < s->ndocs; i++) s->docs[i].alive = false;
    for (int32_t i = 0; i < a->ndocs; i++) {
        app_doc *d = a->docs[i];
        as_doc *ad = get_doc(s, d->id);
        uint64_t due;
        if (!ad) continue;
        ad->alive = true;
        if (ad->busy) continue;
        if (!app_doc_dirty(d)) {                 /* saved or back at the saved state */
            if (ad->on_disk) remove_doc_files(s, ad);
            drop_adopted(ad);
            ad->changed_at = 0;
            continue;
        }
        if (ad->on_disk && ad->disk_seq == d->hist->cur->seq) {
            ad->changed_at = 0;
            continue;
        }
        if (ad->changed_at == 0) ad->changed_at = now;
        if (interval <= 0.0 && !ad->due_now) continue;
        due = ad->due_now ? 0u : ad->changed_at + (uint64_t)(interval * 1000.0);
        if (now >= due) {
            if (start_doc_job(a, s, d, ad)) ad->due_now = false;
            else ad->changed_at = now;           /* retry after another interval */
        } else {
            app_request_frame_at(a, due);
        }
    }
    /* documents that went away without the closing hook (should not happen) */
    for (int32_t i = 0; i < s->ndocs;) {
        as_doc *ad = &s->docs[i];
        if (!ad->alive && !ad->busy && !ad->closed) {
            if (ad->on_disk) remove_doc_files(s, ad);
            forget_doc(s, ad);
            continue;
        }
        i++;
    }
}

static void as_closing(app *a, app_doc *d, void *ud)
{
    as_state *s = astate(a);
    as_doc *ad;
    (void)ud;
    if (!s || !d) return;
    ad = find_doc(s, d->id);
    if (!ad) return;
    if (ad->busy) {                              /* finish_job removes the files */
        ad->closed = true;
        return;
    }
    if (ad->on_disk) remove_doc_files(s, ad);
    forget_doc(s, ad);
}

/* Normal exit: wait for the writers, then remove this session. */
static void as_quit(app *a, app_doc *d, void *ud)
{
    as_state *s = astate(a);
    (void)d;
    (void)ud;
    if (!s) return;
    poll_jobs(a, s, true);
    for (int32_t i = 0; i < s->ndocs; i++) {
        if (s->docs[i].on_disk) remove_doc_files(s, &s->docs[i]);
        drop_adopted(&s->docs[i]);
    }
    s->ndocs = 0;
    if (s->session_made) prune_session_dir(s->session);
    s->session_made = false;
}

static void as_free(void *p)
{
    as_state *s = (as_state *)p;
    if (!s) return;
    for (int32_t i = 0; i < s->njobs; i++) {     /* only reached when the hook did not run */
        pal_task_wait(s->jobs[i]->task);
        pal_task_free(s->jobs[i]->task);
        job_free(s->jobs[i]);
    }
    for (int32_t i = 0; i < s->ndocs; i++)
        for (int k = 0; k < 2; k++) free(s->docs[i].adopt[k]);
    free(s->jobs);
    free(s->docs);
    free(s->items);
    free(s);
}

/* ---- configuration -------------------------------------------------------------------------- */
void app_autosave_cfg_default(app_autosave_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->interval_s = -1.0;
    c->prompt = true;
}

static void new_sid(as_state *s)
{
    static uint32_t counter;
    uint64_t t = SDL_GetTicksNS();
    snprintf(s->sid, sizeof s->sid, "s%lld-%llx-%u", (long long)io_unix_now(),
             (unsigned long long)(t & 0xFFFFFFFFull), (unsigned)++counter);
}

bool app_autosave_configure(app *a, const app_autosave_cfg *c)
{
    as_state *s = astate(a);
    app_autosave_cfg def;
    char base[1024];
    if (!s) return false;
    if (!c) {
        app_autosave_cfg_default(&def);
        c = &def;
    }
    /* finish the old session first */
    if (s->session_made) as_quit(a, NULL, NULL);
    s->enabled = false;
    s->root[0] = '\0';
    s->interval_s = c->interval_s;
    s->heartbeat_s = c->heartbeat_s > 0.0 ? c->heartbeat_s : AS_HEARTBEAT_SECS;
    s->exclusive = c->exclusive;
    s->prompt = c->prompt;
    if (c->root && !*c->root) return true;                     /* off */
    if (c->root) {
        app_copy_str(base, sizeof base, c->root);
    } else if (a->opts.config_dir) {
        /* a private config dir (tests, portable use) keeps its state next to it;
         * "" (no settings file) means no autosave */
        if (!io_user_dir(a, PAL_DIR_STATE, "state", base, sizeof base)) return true;
    } else {
        const char *st = pal_dir(PAL_DIR_STATE);
        if (!st) return true;
        app_copy_str(base, sizeof base, st);
    }
    join(s->root, sizeof s->root, base, "recovery");
    if (!pal_is_dir(s->root) && !pal_mkdirs(s->root)) {
        pal_log(PAL_LOG_WARN, "autosave: cannot create %s", s->root);
        s->root[0] = '\0';
        return false;
    }
    new_sid(s);
    join(s->session, sizeof s->session, s->root, s->sid);
    s->enabled = true;
    s->startup_prompted = false;
    s->next_scan = 0;
    return true;
}

const char *app_autosave_root(const app *a)
{
    const as_state *s = astate(a);
    return s && s->enabled ? s->root : NULL;
}

const char *app_autosave_session_dir(const app *a)
{
    const as_state *s = astate(a);
    return s && s->enabled && s->session_made ? s->session : NULL;
}

int app_autosave_now(app *a, bool wait)
{
    as_state *s = astate(a);
    int started = 0;
    if (!s || !s->enabled) return 0;
    poll_jobs(a, s, false);
    for (int32_t i = 0; i < a->ndocs; i++) {
        app_doc *d = a->docs[i];
        as_doc *ad = get_doc(s, d->id);
        if (!ad || ad->busy || !app_doc_dirty(d)) continue;
        if (ad->on_disk && ad->disk_seq == d->hist->cur->seq) continue;
        if (start_doc_job(a, s, d, ad)) started++;
    }
    if (wait) poll_jobs(a, s, true);
    return started;
}

int app_autosave_saved_count(const app *a)
{
    const as_state *s = astate(a);
    int n = 0;
    if (!s) return 0;
    for (int32_t i = 0; i < s->ndocs; i++)
        if (s->docs[i].on_disk) n++;
    return n;
}

void io_autosave_init(app *a);
void io_autosave_init(app *a)
{
    as_state *s = (as_state *)calloc(1u, sizeof *s);
    if (!s) return;
    if (!app_ext_set(a, AS_KEY, s, as_free)) {
        free(s);
        return;
    }
    if (proc_stat("self", &s->self_pid, &s->self_pstart) <= 0) {
        s->self_pid = 0;
        s->self_pstart = 0;
    }
    (void)app_hook_add(a, APP_HOOK_FRAME, as_frame, NULL);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, as_closing, NULL);
    (void)app_hook_add(a, APP_HOOK_QUIT, as_quit, NULL);
    /* defaults until main (or a test) configures: on with a settings file,
     * off for scripted and test runs without one */
    {
        app_autosave_cfg c;
        app_autosave_cfg_default(&c);
        if (a->opts.config_dir && !*a->opts.config_dir) c.root = "";
        (void)app_autosave_configure(a, &c);
    }
}
