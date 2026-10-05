/* app_io.h - file services of the paint.c editor beyond the File menu flows
 * declared in app_internal.h (lane I): autosave and crash recovery, recent
 * file thumbnails, drag and drop of files, the window icon.
 *
 * Thread rules: every function runs on the main thread (the app's thread)
 * unless it says otherwise. Ownership: inputs are borrowed (copied when
 * kept); results written into caller buffers.
 */
#ifndef APP_IO_H
#define APP_IO_H

#include "app.h"
#include "pc/pc_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- autosave and crash recovery (src/app/io/autosave.c) --------------------------- */
/* Every modified image is written in .pdn form to
 *     <root>/recovery/<session>/doc-<n>.pdn   (+ doc-<n>.ini, the manifest)
 * once it has unsaved changes that are older than the interval (default 2
 * minutes, settings key file.autosave_interval in seconds, 0 = off). The
 * encoding runs on a worker with a document snapshot (app_doc_snapshot), the
 * files are replaced atomically. A clean document (saved, or undone back to
 * the saved state) and a closed one lose their autosave; a normal exit
 * removes the whole session. A session left behind by a process that died
 * is offered for recovery at the next start (app_recovery_prompt). */
typedef struct app_autosave_cfg {
    const char *root;          /* NULL = default: PAL_DIR_STATE, or <config dir>/state
                                  when the app has its own config dir; "" = off */
    double      interval_s;    /* < 0: settings value; 0: no periodic autosave
                                  (app_autosave_now still works) */
    double      heartbeat_s;   /* <= 0: 10 s. The session's liveness stamp. */
    bool        exclusive;     /* this process is the only paint.c that can use root
                                  (single instance primary): any other session found
                                  there belongs to a process that is gone */
    bool        prompt;        /* show the recovery dialog when images are found
                                  (default true; scripted runs turn it off) */
} app_autosave_cfg;

void        app_autosave_cfg_default(app_autosave_cfg *c);
/* (Re)configure. Starts a new session folder lazily (first autosave).
 * Returns false when root cannot be created (autosave then stays off). */
bool        app_autosave_configure(app *a, const app_autosave_cfg *c);
/* The recovery root in use ("<root>/recovery"), or NULL when off. Borrowed. */
const char *app_autosave_root(const app *a);
/* This session's folder, or NULL before the first autosave. Borrowed. */
const char *app_autosave_session_dir(const app *a);
/* Autosave every modified image now (whatever the interval); with wait the
 * call returns after the files are written. Returns the number started. */
int         app_autosave_now(app *a, bool wait);
/* Number of images whose current state is on disk (tests, status). */
int         app_autosave_saved_count(const app *a);

typedef struct app_recovery_info {
    char     name[256];        /* display name at the time ("photo.png", "Untitled 2") */
    char     path[1024];       /* original file, "" when never saved */
    char     codec[16];        /* original file type id, "" when none */
    int64_t  time;             /* seconds since 1970 of the autosave */
    uint32_t w, h, layers;
} app_recovery_info;

/* Look for images left behind by sessions that ended abnormally. Returns
 * the number found (also kept for app_recovery_get / restore / discard).
 * Empty dead sessions are removed silently. */
int   app_recovery_scan(app *a);
int   app_recovery_count(const app *a);
bool  app_recovery_get(const app *a, int i, app_recovery_info *out);
/* Open item i (-1 = all) as modified images (decoded on workers; the
 * documents appear in later frames, app_tasks_wait finishes them). The
 * recovered files are deleted once this session has autosaved the image.
 * Returns the number of decodes started. */
int   app_recovery_restore(app *a, int i);
/* Delete item i (-1 = all) from disk. Returns the number deleted. */
int   app_recovery_discard(app *a, int i);
/* Show the recovery dialog when app_recovery_count > 0. */
void  app_recovery_prompt(app *a);

/* ---- recent files (src/app/io/recent.c) ---------------------------------------------- */
/* Remove every entry (File > Open Recent > Clear List) and the thumbnails. */
void  app_recent_clear(app *a);
int   app_recent_count(const app *a);
const char *app_recent_at(const app *a, int i);   /* newest first, borrowed */
/* Open recent entry i; a missing file shows an error and loses its entry. */
bool  app_recent_open(app *a, int i);

/* ---- drag and drop (src/app/io/drop.c) ------------------------------------------------ */
typedef enum app_drop_action {
    APP_DROP_ASK = 0,          /* the Open / Add Layers / Cancel question */
    APP_DROP_OPEN = 1,         /* each file opens as a new image */
    APP_DROP_LAYERS = 2        /* each file is added as layers of the active image */
} app_drop_action;

/* Handle files dropped on the window (paths copied). */
void  app_drop_files(app *a, const char *const *paths, int n, app_drop_action act);
/* Decode the files on workers and add their layers above the active layer
 * of d (a new image of the default size when d is NULL); the canvas grows
 * to fit larger images (anchored top left). false on OOM. */
bool  app_import_layers(app *a, app_doc *d, const char *const *paths, int n);

/* ---- event loop wake-ups (src/app/io/wake.c) --------------------------------------------- */
/* Wake the idle event loop every ms milliseconds (0 stops) so pal delivers
 * forwarded opens (single instance) and dialog results without user input;
 * a wake-up draws nothing unless something arrived. The editor's main()
 * turns it on; one app per process. */
void  app_wake_poll(app *a, uint32_t ms);

/* ---- window icon (src/app/io/icon.c) --------------------------------------------------- */
/* The paint.c icon as straight RGBA (size x size, 32, 64 or 256 px, others
 * resampled from 256). Owned: free(). NULL on OOM. Any thread. */
uint8_t *app_icon_rgba(int32_t size);
/* Set the window icon (no-op headless). */
bool  app_set_window_icon(app *a);

/* ---- loading image files (src/app/io/load.c, lane CODEC wave 4) -------------------------- */
/* Every user-facing load (File > Open, crash recovery, drop as layers,
 * Layers > Import From File, paste of a file, the Save Configuration
 * preview) uses these limits (FL-BIG): any size up to PC_MAX_DIM per side
 * (ADR-014), up to 1024 layers, and one memory budget for the file bytes
 * held while decoding plus the decoded pixels: three quarters of the
 * physical RAM, never less than the codec default of 4 GiB. Any thread. */
void      app_load_limits(pc_codec_limits *lim);
/* The same for a computer with ram bytes of memory (tests, sizing). */
void      app_load_limits_ram(uint64_t ram, pc_codec_limits *lim);
/* Take held bytes (the file being decoded) out of lim->max_mem. false (lim
 * unchanged) when they alone exceed the budget. */
bool      app_load_limits_after(pc_codec_limits *lim, uint64_t held);
/* Tests: replace the budget with bytes (0 = automatic again). Main thread,
 * while no load runs. */
void      app_load_test_budget(uint64_t bytes);

/* What a load found besides its status, for the error text. */
typedef struct app_load_info {
    uint64_t file_bytes;       /* size of the file (input bytes), 0 = unknown */
    uint64_t max_file_bytes;   /* the largest file that can be read (the budget) */
    uint64_t max_mem;          /* memory the decoder could use for the pixels */
    bool     file_too_big;     /* PC_ERR_LIMIT: the file itself is over the budget */
    bool     missing;          /* the file does not exist */
} app_load_info;

/* Read path (UTF-8, borrowed) and decode it within app_load_limits: the
 * file is refused before reading when it exceeds the budget, the decoder
 * gets the budget minus the file size. only (may be NULL) forces one codec
 * (recovery: .pdn), else content sniffing with the extension as fallback.
 * On success *doc (caller owns) and *meta (caller frees with pc_meta_free)
 * are set, and *used (may be NULL) names the codec; on failure *doc is NULL
 * and meta is empty. info (may be NULL) receives the details. Any thread. */
pc_status app_load_file(const char *path, const pc_codec *only, pc_doc **doc,
                        pc_image_meta *meta, const pc_codec **used, app_load_info *info);
/* The same for bytes already in memory (p/n borrowed; clipboard data, an
 * encoded preview); path_hint (may be NULL) only helps choose the codec. */
pc_status app_load_bytes(const uint8_t *p, size_t n, const char *path_hint, const pc_codec *only,
                         pc_doc **doc, pc_image_meta *meta, const pc_codec **used,
                         app_load_info *info);
/* "Could not <verb> "<name>": <reason>." for a failed load (out gets at most
 * cap bytes, NUL-terminated): a missing file, a file larger than the
 * budget (its size and the budget), an image over the decode limits (the
 * pixel, layer and memory limits that applied), else pc_status_str. info
 * may be NULL. Any thread. */
void      app_load_error_text(char *out, size_t cap, const char *verb, const char *name,
                              pc_status st, const app_load_info *info);

#ifdef __cplusplus
}
#endif

#endif /* APP_IO_H */
