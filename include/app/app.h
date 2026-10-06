/* app.h - the paint.c editor application (lane L2/L4).
 *
 * One `app` owns the window, the renderer, the UI context, the worker pool,
 * the effect registry, the settings, the open documents (app_doc.h), the
 * command registry (app_cmd.h), the tools (app_tool.h), the panels and the
 * dialog stack (app_ui.h). Feature modules extend it without editing shared
 * files: every src/app/.../mod_<name>.c defines `void mod_<name>(app *a)`,
 * called once at startup, and every src/app/tools/tool_<name>.c defines
 * `const app_tool app_tool_<name>`. See docs/app/ARCHITECTURE.md.
 *
 * Thread rules: everything in include/app runs on the main thread (the
 * thread that called app_create) unless a function says otherwise. Worker
 * code receives plain data, never the app. Ownership words: "borrowed"
 * means the callee keeps no reference after returning; "owned" results
 * must be released with the named function.
 */
#ifndef APP_H
#define APP_H

#include "pc/pc_base.h"
#include "pc/pc_blend.h"
#include "pc/pc_par.h"
#include "ui/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Surface;
struct SDL_Texture;
union SDL_Event;

typedef struct app app;
typedef struct app_doc app_doc;
typedef struct app_tool app_tool;
typedef struct fx_registry fx_registry;
typedef struct pal_pool pal_pool;

#define APP_NAME    "paint.c"
#define APP_ID      "io.github.vancevoj.paintc"
#define APP_VERSION "0.1.2"

/* ---- creation ------------------------------------------------------------ */
typedef enum app_theme_pref {
    APP_THEME_AUTO = 0,      /* follow SDL_GetSystemTheme */
    APP_THEME_LIGHT = 1,
    APP_THEME_DARK = 2,
    APP_THEME_BLUE = 3       /* light with blue chrome (lane SHELL, WINDOWS 8.1) */
} app_theme_pref;

typedef struct app_opts {
    bool        headless;    /* software renderer on an off-screen surface, no window
                                (tests, --screenshot without a display) */
    int32_t     width, height;   /* surface size in px, or window size in window
                                    units (0 = default; lane UIA: a window then
                                    takes its saved or default DIP size, fitted
                                    to the display; a given size wins) */
    float       scale;       /* UI scale (0 = automatic: 1 headless, the display
                                scale in a window; lane UIA: a value overrides
                                the display scale in a window too) */
    uint32_t    workers;     /* worker threads, 0 = logical cores - 1 */
    const char *config_dir;  /* settings folder; NULL = PAL_DIR_CONFIG; "" = no
                                settings file at all (tests) */
    int         theme;       /* -1 = from settings, else app_theme_pref */
    bool        no_vsync;
    bool        software;    /* window mode: force the "software" renderer */
    bool        no_default_doc;  /* do not open the startup "Untitled" image */
    bool        disable_plugins; /* lane F: skip effect plugin folders (--disable-plugins) */
} app_opts;

/* Defaults: window 1440 x 900, settings from PAL_DIR_CONFIG, theme from
 * settings, vsync on, the startup image opened. */
void  app_opts_default(app_opts *o);

/* Create the application. Requires SDL_Init(SDL_INIT_VIDEO) in window mode
 * (headless needs no video driver) and pal_init. Registers built-in tools
 * and runs every mod_* module. NULL on failure (logged). Owned: destroy
 * with app_destroy. */
app  *app_create(const app_opts *o);
void  app_destroy(app *a);                    /* NULL-safe; saves settings */

/* Classic main loop until the user quits (unsaved-changes prompts
 * included). Returns the process exit code. */
int   app_run(app *a);

/* Feed one SDL event (the loop calls it; tests inject synthetic ones). */
void  app_event(app *a, const union SDL_Event *e);
/* Run one frame: background results, UI, input, rendering. force renders
 * even when nothing changed. Returns false once the app finished quitting. */
bool  app_frame(app *a, bool force);
/* True when a frame is due (input, animation, background work). */
bool  app_needs_frame(const app *a);
/* Milliseconds the loop may sleep (-1 = until input). */
int32_t app_wait_timeout(const app *a);

/* Request a redraw as soon as possible / at a time (SDL_GetTicks ms). */
void  app_request_frame(app *a);
void  app_request_frame_at(app *a, uint64_t ms);

/* Ask to quit: commits the live tool, then prompts for every document with
 * unsaved changes; quits when all were resolved. */
void  app_quit(app *a);
/* Quit without prompting (tests, fatal errors). */
void  app_quit_now(app *a);
bool  app_quitting(const app *a);

/* ---- accessors (borrowed, valid for the app's lifetime) --------------------- */
ui_ctx              *app_ui(const app *a);
struct SDL_Renderer *app_renderer(const app *a);
struct SDL_Window   *app_window(const app *a);       /* NULL when headless */
const pc_par        *app_par(const app *a);          /* worker pool parallel-for */
pal_pool            *app_pool(const app *a);
fx_registry         *app_fx(const app *a);
uint64_t             app_now_ms(const app *a);       /* time of the current frame */

/* ---- documents ---------------------------------------------------------------- */
int32_t  app_doc_count(const app *a);
app_doc *app_doc_at(const app *a, int32_t i);
int32_t  app_doc_index(const app *a, const app_doc *d);   /* -1 if not open */
app_doc *app_active_doc(const app *a);                    /* NULL when none */
/* Make d the active document (commits the live tool first). */
void     app_set_active_doc(app *a, app_doc *d);
/* Add a document created with app_doc_create (ownership moves to the app)
 * and make it active. false (d destroyed) on OOM. */
bool     app_add_doc(app *a, app_doc *d);
/* Close without prompting: the document is destroyed. */
void     app_close_doc_now(app *a, app_doc *d);

/* ---- colors (Colors window) ------------------------------------------------------ */
pc_px32  app_primary(const app *a);
pc_px32  app_secondary(const app *a);
void     app_set_primary(app *a, pc_px32 c);
void     app_set_secondary(app *a, pc_px32 c);
int      app_color_slot(const app *a);          /* 0 primary, 1 secondary active */
void     app_set_color_slot(app *a, int slot);

/* ---- view preferences --------------------------------------------------------------- */
typedef enum app_units { APP_UNITS_PX = 0, APP_UNITS_IN = 1, APP_UNITS_CM = 2 } app_units;
bool      app_pixel_grid(const app *a);
void      app_set_pixel_grid(app *a, bool on);
bool      app_rulers(const app *a);
void      app_set_rulers(app *a, bool on);
app_units app_get_units(const app *a);
void      app_set_units(app *a, app_units u);
bool      app_overscroll(const app *a);
/* Format a length of px pixels in the current units ("12", "0.13") using
 * dpi (0 = 96). */
void      app_format_len(const app *a, double px, double dpi, char *out, size_t cap);

/* Theme: the current preference and whether dark colors are in use. */
app_theme_pref app_theme(const app *a);
void           app_set_theme(app *a, app_theme_pref t);
bool           app_dark(const app *a);

/* ---- status bar -------------------------------------------------------------------- */
/* Transient tool state text ("Selection: 120 x 80"), replacing the tool
 * help text until cleared with NULL. Copied. */
void  app_status(app *a, const char *text);
/* Progress bar: fraction in [0, 1], negative = indeterminate, > 1 hides. */
void  app_progress(app *a, float fraction);

/* ---- messages -------------------------------------------------------------------- */
/* Called once when a message box closes with the UI_DLG_* result. */
typedef void (*app_msg_fn)(app *a, uint32_t result, void *ud);
/* Modal message box (copied text); done may be NULL. */
void  app_message(app *a, const char *title, const char *text, ui_icon icon, uint32_t buttons,
                  uint32_t def, app_msg_fn done, void *ud);
/* Error box with printf-style text, also logged. */
void  app_error(app *a, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* ---- background work ----------------------------------------------------------------- */
/* Run work(ud) on a pool worker (must not touch the app, SDL video or the
 * renderer), then done(a, ud) on the main thread in a later frame. While
 * tasks run the status bar shows a busy indicator. false (nothing runs)
 * on OOM. */
typedef void (*app_work_fn)(void *ud);
typedef void (*app_done_fn)(app *a, void *ud);
bool  app_task(app *a, app_work_fn work, app_done_fn done, void *ud);
int   app_tasks_pending(const app *a);
/* Block until every task finished and its done callback ran (exit, tests). */
void  app_tasks_wait(app *a);

/* ---- hooks and extension state ----------------------------------------------------- */
typedef enum app_hook_kind {
    APP_HOOK_FRAME = 0,        /* every frame, before the UI is declared */
    APP_HOOK_DOC_ACTIVATED,    /* d became active (may be NULL) */
    APP_HOOK_DOC_CLOSING,      /* d is about to be destroyed */
    APP_HOOK_QUIT,             /* the app is shutting down (d NULL) */
    APP_HOOK_COUNT
} app_hook_kind;
typedef void (*app_hook_fn)(app *a, app_doc *d, void *ud);
bool  app_hook_add(app *a, app_hook_kind k, app_hook_fn fn, void *ud);

/* Named per-app state for modules (no edits to the app struct needed).
 * set stores p (ownership moves to the app; destroy is called on replace
 * and at exit, may be NULL). get returns NULL for unknown keys. */
bool  app_ext_set(app *a, const char *key, void *p, void (*destroy)(void *p));
void *app_ext_get(const app *a, const char *key);

/* ---- screenshots ------------------------------------------------------------------- */
/* Write the last rendered frame to a BMP file. */
bool  app_screenshot(app *a, const char *path);

/* ---- script / self test (src/app/script.c) ----------------------------------------- */
/* Run a script (one command per line, see script.c) synchronously, driving
 * frames as needed. Returns 0 on success, nonzero on the first failing
 * line (message logged and copied to err when not NULL). */
int   app_script_run(app *a, const char *text, char *err, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* APP_H */
