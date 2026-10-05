/* fx_run.h - effect host runtime (lane L5a): registry, parameter helpers,
 * presets, the host services table and the job runner.
 *
 * Pure C17 over fx_abi.h and the pc_core atomics. No SDL, no pal, no OS
 * headers: the app supplies threads (pal tasks or a pc_par) and calls in.
 *
 * Typical use by the app:
 *   reg = fx_registry_create(); fx_registry_add_builtins(reg);   (startup)
 *   fx = fx_registry_find(reg, id);
 *   params = fx_params_new(fx, &env);          (dialog edits params)
 *   fx_job_create(fx, params, &src, &dst, &env, region, 64, view_xy, &job);
 *   submit N tasks that each run fx_job_work(job, i) until it returns
 *       FX_WORK_FINISHED (yield and retry on FX_WORK_AGAIN);
 *   every frame on the main thread: fx_job_take_done(job, rects, n) to
 *       upload finished ROIs, fx_job_state(job) to detect the end;
 *   fx_job_cancel(job) when params change; fx_job_destroy(job) after all
 *       tasks returned.
 * Final renders and dialog-less adjustments can call fx_run_sync instead.
 *
 * Ownership words used below: "borrowed" means the callee keeps no
 * reference after returning (or, where stated, keeps it until a named
 * destroy call and the caller must keep it alive that long); "owned" means
 * the caller must free the result with the named function.
 */
#ifndef FX_RUN_H
#define FX_RUN_H

#include "fx_abi.h"
#include "pc/pc_base.h"
#include "pc/pc_par.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== limits ================================================================ */
#define FX_MAX_PROPS        256u          /* fx_effect.n_props */
#define FX_MAX_PARAMS_SIZE  (1u << 20)    /* fx_effect.params_size, bytes */
#define FX_MAX_KEY_LEN      64u           /* fx_prop.key */
#define FX_MAX_ID_LEN       128u          /* fx_effect.id */
#define FX_MAX_MENU_LEN     256u          /* fx_effect.menu */
#define FX_MAX_CHOICES      256u          /* entries of an FXP_CHOICE list */
#define FX_PRESET_MAX_LEN   (4u << 20)    /* longest preset string accepted */
#define FX_JOB_MAX_ROIS     (1u << 20)    /* tile size grows to stay below */
#define FX_JOB_DEFAULT_TILE 64            /* matches the layer tile size */

/* ==== host services ========================================================= */
/* The host table every effect receives: alloc/free are malloc/free (one heap
 * for plugins and host, X-17), cancelled() reads the cancel flag of the
 * fx_job passed as `job` (NULL job: never cancelled), log() forwards to the
 * hook below. Static storage, valid for the whole process. Any thread. */
const fx_host *fx_run_host(void);

/* Log hook for messages from effects and from the registry (rejected
 * effects). level: 0 info, 1 warn, 2 error. utf8 is borrowed for the call.
 * Default: messages are dropped. Set it on the main thread before any job
 * runs; fn may then be called from any worker thread concurrently. */
typedef void (*fx_log_fn)(void *ud, int level, const char *utf8);
void fx_run_set_log(fx_log_fn fn, void *ud);
/* Sends one message (borrowed) through the hook. NULL is ignored. Any thread. */
void fx_run_log(int level, const char *utf8);

/* ==== effect validation ===================================================== */
/* Checks an effect descriptor (struct size, id and menu syntax, render
 * present, prop kinds, keys, offsets inside params_size with natural
 * alignment, no overlapping props, defaults inside ranges, choice lists,
 * custom blob sizes and hints, enabled_if references). Returns PC_OK or
 * PC_ERR_ARG and writes a short English reason into why (may be NULL,
 * always NUL-terminated when cap > 0). Pure function, any thread. */
pc_status fx_effect_validate(const fx_effect *fx, char *why, size_t cap);

/* Byte size of a prop's value in the params blob (custom: prop->size). */
uint32_t fx_prop_value_size(const fx_prop *p);

/* ==== menu paths ============================================================ */
/* Menu paths use '/' between levels. A '/' with a space on either side is
 * part of a name, so "Adjustments/Brightness / Contrast" has two segments
 * ("Adjustments", "Brightness / Contrast"). Writes up to max segments
 * (pointer into menu plus byte length) and returns the total segment count.
 * Pure function, any thread. */
uint32_t fx_menu_split(const char *menu, const char **seg, size_t *seg_len, uint32_t max);
/* Order used by the registry: segment by segment, ASCII case-insensitive,
 * then case-sensitive, shorter path first on a common prefix. */
int fx_menu_compare(const char *a, const char *b);

/* ==== registry ============================================================== */
/* A set of effects sorted by menu path. Effect descriptors are borrowed:
 * they must outlive the registry (built-ins are static; plugins stay loaded
 * until the registry is destroyed). Build and mutate on one thread (the
 * main thread); once built it may be read from any thread as long as nobody
 * mutates it. */
typedef struct fx_registry fx_registry;

fx_registry *fx_registry_create(void);               /* owned; NULL on OOM */
void         fx_registry_destroy(fx_registry *r);    /* NULL-safe */

/* Validates and inserts fx. PC_ERR_ARG when invalid (reason sent to the log
 * hook), PC_ERR_STATE when the id is already registered, PC_ERR_LIMIT past
 * 65536 effects, PC_ERR_NOMEM. */
pc_status fx_registry_add(fx_registry *r, const fx_effect *fx);

/* Runs an fx_entry function (a plugin's FX_ENTRY_NAME export, or
 * fx_builtin_register) with fx_run_host() and a reg callback that adds into
 * r. The reg callback returns 0 when the effect was accepted and a negative
 * pc_status otherwise. Returns the number of effects accepted. Not
 * reentrant: one entry call at a time per process (main thread). */
int fx_registry_add_entry(fx_registry *r, fx_entry_fn entry);
int fx_registry_add_builtins(fx_registry *r);        /* fx_builtin_register */

uint32_t         fx_registry_count(const fx_registry *r);
/* i-th effect in menu order, NULL when out of range. Borrowed. */
const fx_effect *fx_registry_at(const fx_registry *r, uint32_t i);
const fx_effect *fx_registry_find(const fx_registry *r, const char *id);
const fx_effect *fx_registry_find_menu(const fx_registry *r, const char *menu);

/* ==== parameters ============================================================ */
/* FXP_COLOR defaults FX_COLOR_PRIMARY / FX_COLOR_SECONDARY resolve against
 * env->primary / env->secondary; with env NULL they become opaque black and
 * opaque white. Other color defaults are the 0xAARRGGBB value itself. */
uint32_t fx_color_default(const fx_prop *p, const fx_env *env);

/* Zero the blob, write every non-custom default, then call init_params.
 * params must hold fx->params_size bytes (may be NULL when that is 0).
 * Any thread (pure). */
void  fx_params_init(const fx_effect *fx, void *params, const fx_env *env);
/* malloc'ed, initialized blob (owned, free with fx_params_free). NULL on
 * OOM; a valid one-byte block when params_size is 0. */
void *fx_params_new(const fx_effect *fx, const fx_env *env);
void  fx_params_free(void *params);                    /* NULL-safe */

const fx_prop *fx_prop_find(const fx_effect *fx, const char *key);

/* Numeric access by key (FXP_INT, REAL, BOOL, CHOICE, COLOR, ANGLE, SEED).
 * get returns PC_ERR_ARG for unknown keys, POINT and CUSTOM. set clamps to
 * the prop range (ints and choices round half away from zero, bools become
 * 0 or 1, colors are taken as 0xAARRGGBB in [0, 2^32-1]); NaN is rejected
 * with PC_ERR_ARG and leaves params unchanged. Any thread on its own blob. */
pc_status fx_param_get(const fx_effect *fx, const void *params, const char *key, double *out);
pc_status fx_param_set(const fx_effect *fx, void *params, const char *key, double v);
/* FXP_POINT access (x, y); set clamps both axes to [min, max]. */
pc_status fx_param_get_point(const fx_effect *fx, const void *params, const char *key,
                             double xy[2]);
pc_status fx_param_set_point(const fx_effect *fx, void *params, const char *key,
                             const double xy[2]);

/* Forces every non-custom value into its valid range (NaN becomes the
 * default, bools 0/1, choices and ints clamped). Returns the number of
 * values changed. Custom blobs are left to the effect, which must treat
 * them as untrusted. */
uint32_t fx_params_clamp(const fx_effect *fx, void *params);
/* True when fx_params_clamp would change nothing. */
bool     fx_params_valid(const fx_effect *fx, const void *params);

/* ==== presets =============================================================== */
/* Text form "key=value;key=value" (UTF-8, ASCII in practice):
 *   INT, CHOICE (index), SEED: integer      BOOL: 0 or 1
 *   REAL, ANGLE: shortest round-trip decimal with '.' (locale independent)
 *   COLOR: #AARRGGBB                        POINT: x,y
 *   CUSTOM: lowercase hex of the blob bytes (little-endian platforms)
 * Saving returns a malloc'ed NUL-terminated string (owned, free()), NULL on
 * OOM. Loading parses into a scratch copy and only on success copies it into
 * params, so params are unchanged on failure. Unknown keys are ignored,
 * later duplicates win, values are clamped like fx_param_set. Errors:
 * PC_ERR_FORMAT for syntax errors, a NUL byte, bad values or wrong custom
 * lengths; PC_ERR_LIMIT when len > FX_PRESET_MAX_LEN; PC_ERR_NOMEM. Reads
 * exactly len bytes of s, never more. Any thread on its own blob. */
char     *fx_preset_save(const fx_effect *fx, const void *params);
pc_status fx_preset_load(const fx_effect *fx, void *params, const char *s, size_t len);

/* ==== jobs ================================================================== */
typedef struct fx_job fx_job;

/* Returns of fx_job_work / fx_job_work_some. */
#define FX_WORK_FINISHED 0  /* nothing left for this caller; return the task */
#define FX_WORK_AGAIN    1  /* prepare() is running on another worker; yield
                               briefly (or resubmit) and call again */
#define FX_WORK_MORE     2  /* work_some hit its ROI budget; ROIs remain */

/* fx_job_state */
typedef enum fx_job_state_t {
    FX_JOB_RUNNING   = 0,   /* preparing or rendering */
    FX_JOB_DONE      = 1,   /* every ROI rendered */
    FX_JOB_CANCELLED = 2,   /* cancelled before completion */
    FX_JOB_FAILED    = 3    /* prepare or render returned FX_ERROR */
} fx_job_state_t;

/* Creates a job that renders `region` of dst from src.
 *  - fx: borrowed until fx_job_destroy (registry effects qualify).
 *  - params: copied (and clamped with fx_params_clamp); may be NULL when
 *    params_size is 0, otherwise NULL means defaults resolved against env.
 *  - src, dst: the descriptors are copied, the pixels are borrowed until
 *    fx_job_destroy. Both must cover the area rendered; chans must be 4
 *    (1 for FX_FLAG_MASK_ONLY) and stride >= w * chans.
 *  - env: copied. The rendered area is region clipped to env->sel, src->r
 *    and dst->r; ROIs are the cells of a tile x tile grid anchored at the
 *    document origin, clipped to that area (tile <= 0: 64). The grid is
 *    coarsened when it would exceed FX_JOB_MAX_ROIS cells.
 *    FX_FLAG_SINGLE_THREAD effects get one ROI covering the whole area.
 *  - priority_xy: NULL for row-major order, else a document point (for
 *    example the viewport center); ROIs are handed out nearest first
 *    (squared center distance, ties in row-major order).
 * Writes nothing to dst. PC_ERR_ARG on bad arguments, PC_ERR_NOMEM.
 * Any thread. */
pc_status fx_job_create(const fx_effect *fx, const void *params, const fx_img *src,
                        fx_img *dst, const fx_env *env, fx_rect region, int32_t tile,
                        const int32_t *priority_xy, fx_job **out);

/* Runs prepare() on the calling thread if no worker has claimed it yet;
 * otherwise returns immediately. PC_OK when prepared (or nothing to
 * prepare), PC_ERR_CANCELLED, PC_ERR_NOMEM when prepare returned FX_ERROR,
 * PC_ERR_STATE when another thread is preparing right now. Any thread. */
pc_status fx_job_prepare(fx_job *job);

/* Worker entry. The first caller runs prepare(); then every caller pulls
 * ROIs from a shared atomic queue and renders them until the queue is empty,
 * the job is cancelled or a render fails. worker is a caller-chosen index
 * (for diagnostics only). Callers arriving while prepare() runs elsewhere get
 * FX_WORK_AGAIN at once. Any number of threads concurrently. */
int fx_job_work(fx_job *job, uint32_t worker);
/* Same, but renders at most max_rois ROIs (FX_WORK_MORE when it stopped
 * early). Useful for time-sliced work and step-by-step tests. */
int fx_job_work_some(fx_job *job, uint32_t worker, uint32_t max_rois);

/* Requests cancellation: workers stop pulling ROIs and renders observe
 * host->cancelled(). Idempotent. Any thread. */
void fx_job_cancel(fx_job *job);

/* Copies up to max finished ROIs (each reported exactly once, in completion
 * order) into out and returns how many. Pixels of a reported ROI are fully
 * written and visible to the caller. Single consumer: one thread (the main
 * thread) at a time. */
uint32_t fx_job_take_done(fx_job *job, fx_rect *out, uint32_t max);

/* Lifecycle queries, any thread. DONE implies every ROI's pixels are
 * written and visible. CANCELLED and FAILED are final for the job, but
 * workers may still be returning: wait for your tasks before destroying. */
fx_job_state_t fx_job_state(const fx_job *job);
void     fx_job_progress(const fx_job *job, uint32_t *done, uint32_t *total);
fx_rect  fx_job_area(const fx_job *job);          /* clipped render area */
uint32_t fx_job_roi_count(const fx_job *job);
fx_rect  fx_job_roi(const fx_job *job, uint32_t i); /* queue order; empty if out of range */
uint32_t fx_job_active_workers(const fx_job *job);  /* callers inside work() now */
const void *fx_job_params(const fx_job *job);      /* the job's clamped copy */

/* Diagnostics for tests: host->cancelled() turns the cancel flag on by itself
 * at its n-th poll (0 disables). fx_job_polls counts polls so far. */
void     fx_job_set_cancel_after(fx_job *job, uint32_t n);
uint32_t fx_job_polls(const fx_job *job);

/* Releases prepare() state, the params copy and the ROI tables. Only after
 * every fx_job_work call on this job has returned. NULL-safe. */
void fx_job_destroy(fx_job *job);

/* Blocking convenience: create a job (64 pixel tiles, row-major), prepare on
 * the calling thread, then render with pc_par_for (par NULL = serial on the
 * caller). PC_OK, PC_ERR_ARG, PC_ERR_NOMEM (including FX_ERROR from the
 * effect) or PC_ERR_CANCELLED (the effect gave up by itself). Any thread
 * except a pool worker of par. */
pc_status fx_run_sync(const fx_effect *fx, const void *params, const fx_img *src,
                      fx_img *dst, const fx_env *env, fx_rect region, const pc_par *par);
/* Same with an explicit tile size and optional priority point. */
pc_status fx_run_sync_tiled(const fx_effect *fx, const void *params, const fx_img *src,
                            fx_img *dst, const fx_env *env, fx_rect region, int32_t tile,
                            const int32_t *priority_xy, const pc_par *par);

#ifdef __cplusplus
}
#endif

#endif /* FX_RUN_H */
