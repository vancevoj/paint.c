/* pal.h - platform abstraction layer, v1 (lane L0).
 *
 * The only OS-facing API besides SDL3 windowing/events in src/app. Paths are
 * UTF-8 everywhere; Windows conversions to UTF-16, long-path prefixes and
 * similar details stay inside src/pal. Implemented over SDL3 plus small
 * native shims. Thread rules are stated per function.
 */
#ifndef PAL_H
#define PAL_H

#include "pc/pc_base.h"
#include "pc/pc_par.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
typedef struct SDL_Window pal_window;

/* ---- lifecycle ---------------------------------------------------------- */
/* After SDL_Init. app_id is reverse-DNS ("org.paintc.paintc"), org/app name
 * select the per-user directories. Main thread. */
bool pal_init(const char *app_id, const char *org, const char *app);
void pal_quit(void);

/* Deliver finished async results (file dialogs) as callbacks on the main
 * thread. Call once per frame. Main thread. */
void pal_pump(void);

/* ---- logging and time (any thread) -------------------------------------- */
enum { PAL_LOG_DEBUG = 0, PAL_LOG_INFO = 1, PAL_LOG_WARN = 2, PAL_LOG_ERROR = 3 };
void     pal_log(int level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
uint64_t pal_ticks_ns(void);                  /* monotonic */
uint32_t pal_cpu_count(void);                 /* logical cores, >= 1 */
uint64_t pal_ram_bytes(void);                 /* physical RAM, 0 if unknown */
uint32_t pal_cpu_features(void);              /* PAL_CPU_* bits */
#define PAL_CPU_SSE41 1u
#define PAL_CPU_AVX2  2u
#define PAL_CPU_NEON  4u

/* ---- worker pool -------------------------------------------------------- */
typedef struct pal_pool pal_pool;
typedef struct pal_task pal_task;

/* workers == 0 picks pal_cpu_count() - 1 (at least 1). Main thread. */
pal_pool *pal_pool_create(uint32_t workers);
void      pal_pool_destroy(pal_pool *p);     /* waits for queued tasks */
/* Adapter for the core: blocking parallel-for in which the caller also
 * works. Any thread except a pool worker (no nested use). */
pc_par    pal_pool_par(pal_pool *p);

/* Fire-and-forget background task on a worker. fn must not call SDL video,
 * render or GPU APIs. Poll pal_task_done from the main thread, then free. */
pal_task *pal_task_submit(pal_pool *p, void (*fn)(void *ud), void *ud);
bool      pal_task_done(const pal_task *t);  /* any thread */
void      pal_task_wait(pal_task *t);        /* blocks; not on a pool worker */
void      pal_task_free(pal_task *t);        /* only after done; NULL-safe */

/* Minimal mutex for app-level shared queues. Any thread. */
typedef struct pal_mutex pal_mutex;
pal_mutex *pal_mutex_create(void);
void       pal_mutex_destroy(pal_mutex *m);
void       pal_mutex_lock(pal_mutex *m);
void       pal_mutex_unlock(pal_mutex *m);

/* ---- files (any thread) --------------------------------------------------- */
/* Read a whole file. Fails with PC_ERR_LIMIT when larger than max_bytes.
 * *data is malloc'ed (free with free()) and NUL-terminated past *len. */
pc_status pal_read_file(const char *path, uint64_t max_bytes, uint8_t **data,
                        size_t *len);
/* Atomic save: write a sibling temp file (mode 0600 until renamed, X-23),
 * flush it, then replace path in one step. On failure path is untouched
 * and the temp file is removed. */
pc_status pal_write_file_atomic(const char *path, const void *data, size_t len);
bool      pal_file_exists(const char *path);
bool      pal_is_dir(const char *path);
bool      pal_remove(const char *path);
bool      pal_mkdirs(const char *path);       /* like mkdir -p */
uint64_t  pal_file_mtime(const char *path);   /* seconds since epoch, 0 if absent */

/* Names (not paths) of entries in dir matching a simple glob ("*.txt"; NULL
 * = all), sorted case-insensitively. Returns count; *names is an array of
 * malloc'ed strings freed with pal_free_names. */
int  pal_list_dir(const char *dir, const char *glob, char ***names);
void pal_free_names(char **names, int n);

/* Path helpers on UTF-8 strings (pure functions, any separator style).
 * Results are written to out (NUL-terminated, truncated to cap). */
void        pal_path_join(char *out, size_t cap, const char *a, const char *b);
const char *pal_path_basename(const char *path);       /* pointer into path */
void        pal_path_dirname(char *out, size_t cap, const char *path);
const char *pal_path_ext(const char *path);            /* after the dot, or "" */
char        pal_path_sep(void);                        /* '\\' or '/' */

/* ---- well-known directories ------------------------------------------------- */
typedef enum pal_dir_kind {
    PAL_DIR_CONFIG = 0,   /* settings, palettes subfolder, keymaps */
    PAL_DIR_DATA,         /* plugins subfolder, user resources */
    PAL_DIR_CACHE,        /* font cache, thumbnails */
    PAL_DIR_STATE,        /* autosave and recovery, history swap (0700) */
    PAL_DIR_DOCUMENTS,    /* user's documents folder */
    PAL_DIR_PICTURES,     /* user's pictures folder (default open/save) */
    PAL_DIR_EXE,          /* directory containing the executable */
    PAL_DIR_COUNT
} pal_dir_kind;
/* Absolute path with a trailing separator, created on demand for the
 * per-user kinds. The pointer stays valid until pal_quit. Any thread after
 * pal_init. */
const char *pal_dir(pal_dir_kind k);

/* Font directories to scan for the Text tool (system and user), NULL
 * terminated, valid until pal_quit. */
const char *const *pal_font_dirs(void);

/* ---- native file dialogs (main thread; results via pal_pump) ------------- */
typedef struct pal_filter {
    const char *name;     /* "PNG" */
    const char *pattern;  /* "png;apng" - extensions without dots, or "*" */
} pal_filter;

/* paths is NULL and n is 0 when the user cancelled. filter is the index of
 * the selected filter, or -1 when unknown. Called on the main thread. */
typedef void (*pal_paths_fn)(void *ud, const char *const *paths, int n, int filter);

void pal_dialog_open(pal_window *w, const pal_filter *f, int nf,
                     const char *default_dir, bool multi, pal_paths_fn cb, void *ud);
void pal_dialog_save(pal_window *w, const pal_filter *f, int nf,
                     const char *default_path, pal_paths_fn cb, void *ud);
void pal_dialog_folder(pal_window *w, const char *default_dir,
                       pal_paths_fn cb, void *ud);

/* ---- clipboard (main thread) ---------------------------------------------- */
/* True when the clipboard holds an image in any format pal can return. */
bool pal_clip_has_image(void);
/* Encoded image bytes, preferring image/png, then BMP/DIB. *mime receives
 * the MIME type ("image/png", "image/bmp"). *data is malloc'ed. */
bool pal_clip_get_image(uint8_t **data, size_t *len, char *mime, size_t mime_cap);
/* Offer an image given as PNG bytes. pal also exposes the platform-native
 * flavours (Windows CF_DIBV5, macOS public.png/tiff, Linux image/png and
 * image/bmp) from it. Copies the bytes. */
bool pal_clip_set_image_png(const uint8_t *png, size_t len);
bool pal_clip_set_text(const char *utf8);
char *pal_clip_get_text(void);                 /* malloc'ed or NULL */

/* ---- dynamic libraries (plugins; main thread) ----------------------------- */
typedef struct pal_lib pal_lib;
/* Windows: LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
 * LOAD_LIBRARY_SEARCH_SYSTEM32 (X-18). POSIX: dlopen RTLD_NOW|RTLD_LOCAL. */
pal_lib *pal_lib_open(const char *path);
void    *pal_lib_sym(pal_lib *l, const char *name);
void     pal_lib_close(pal_lib *l);
const char *pal_lib_suffix(void);             /* ".dll", ".dylib", ".so" */

/* ---- shell integration (main thread) --------------------------------------- */
bool pal_open_url(const char *url);           /* browser / file manager */
bool pal_reveal_file(const char *path);       /* show in file manager */

/* Single instance: returns true when this process is the first instance.
 * Otherwise forwards paths to the running instance and returns false (the
 * caller should exit). The first instance receives forwarded paths through
 * cb from pal_pump. May be a no-op returning true where unsupported. */
bool pal_single_instance(const char *app_id, int argc, const char *const *paths,
                         pal_paths_fn cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* PAL_H */
