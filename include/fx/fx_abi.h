/* fx_abi.h - paint.c effect and adjustment plugin ABI, version 1.
 *
 * Self-contained: depends only on <stdint.h> and <stddef.h>. Built-in
 * effects compile against this header exactly like third-party plugins
 * (plus the optional header-only helpers in fx_util.h).
 *
 * Model (full-source, Paint.NET style):
 *  - The host snapshots the active layer into src (whole document, straight
 *    BGRA8) and allocates dst with identical geometry.
 *  - render() is called for disjoint ROIs, possibly concurrently, and writes
 *    only the dst pixels inside its ROI. It may read any src pixel.
 *  - The host then blends dst over the original through the selection
 *    coverage and commits through a transaction. Effects never see tiles,
 *    selections, history or other layers.
 *  - Output must be a pure function of (params, src, env, roi pixel
 *    position): identical for any ROI split or thread count. Random effects
 *    derive their randomness from the seed parameter and pixel coordinates,
 *    never from call order.
 *
 * Versioning: structs carry their size and grow only at the end. Fields are
 * never reordered or removed. FX_ABI_VERSION bumps on incompatible changes.
 */
#ifndef FX_ABI_H
#define FX_ABI_H

#include <stddef.h>
#include <stdint.h>

#define FX_ABI_VERSION 1u

/* Return codes of render() and prepare(). */
#define FX_OK         0
#define FX_CANCELLED  1   /* host->cancelled() returned nonzero */
#define FX_ERROR      2   /* out of memory or invalid parameters */

typedef struct fx_rect { int32_t x, y, w, h; } fx_rect;

typedef struct fx_img {
    uint8_t *px;        /* BGRA8 straight alpha (chans 4) or A8 (chans 1);
                           px[0] is the pixel at document (r.x, r.y) */
    int32_t  stride;    /* bytes per row, >= r.w * chans */
    int32_t  chans;     /* 4 or 1 */
    fx_rect  r;         /* document area covered by px */
} fx_img;

/* ---- parameter schema (the host builds dialogs from it, like IndirectUI) */
typedef enum fxp_kind {
    FXP_INT    = 0,  /* int32_t                min..max, def                    */
    FXP_REAL   = 1,  /* double                 min..max, def, step = UI step    */
    FXP_BOOL   = 2,  /* int32_t 0 or 1         def                              */
    FXP_CHOICE = 3,  /* int32_t index          choices, def                     */
    FXP_COLOR  = 4,  /* uint32_t 0xAARRGGBB    def (see FX_COLOR_*)             */
    FXP_ANGLE  = 5,  /* double degrees         min..max, def (angle-dial widget)*/
    FXP_POINT  = 6,  /* double[2]              min..max per axis, def both axes:
                        offset relative to the selection bounds, -1 = left/top
                        edge, 0 = center, +1 = right/bottom edge (pan widget)  */
    FXP_SEED   = 7,  /* int32_t                "Reseed" button; host randomizes */
    FXP_CUSTOM = 8   /* opaque blob of `size` bytes; host widget named by hint,
                        e.g. "curves" or "levels"; unknown hints are hidden and
                        keep the defaults written by init_params              */
} fxp_kind;

/* FXP_COLOR defaults that follow the palette at invocation time. */
#define FX_COLOR_PRIMARY   (-1.0)
#define FX_COLOR_SECONDARY (-2.0)

/* fx_prop.flags */
#define FXP_F_SLIDER_LOG   1u   /* logarithmic slider (large radius ranges) */
#define FXP_F_PERCENT      2u   /* display value with a % sign */
#define FXP_F_NO_PREVIEW   4u   /* changing it does not restart the preview */

typedef struct fx_prop {
    const char *key;               /* stable ASCII id, used by saved presets */
    const char *label;             /* UTF-8 dialog label */
    uint32_t    kind;              /* fxp_kind */
    uint32_t    offset;            /* byte offset into the params blob */
    double      min, max, def;
    double      step;              /* UI increment; 0 = 1 for ints, 0.01 for reals */
    const char *const *choices;    /* FXP_CHOICE: NULL-terminated list */
    const char *hint;              /* FXP_CUSTOM widget name, else NULL */
    uint32_t    size;              /* FXP_CUSTOM blob bytes, else 0 */
    uint32_t    flags;             /* FXP_F_* */
    const char *enabled_if;        /* NULL, "key" (bool or nonzero) or
                                      "key=N" (int/choice equals N); the
                                      control is disabled otherwise */
} fx_prop;

/* ---- invocation environment ------------------------------------------- */
typedef struct fx_env {
    uint32_t size;                 /* sizeof(fx_env) as built by the host */
    int32_t  doc_w, doc_h;
    fx_rect  sel;                  /* selection bounds, or the whole document */
    uint32_t primary, secondary;   /* palette colors, 0xAARRGGBB */
} fx_env;

/* ---- host services ------------------------------------------------------ */
typedef struct fx_host {
    uint32_t abi;                  /* FX_ABI_VERSION of the host */
    uint32_t size;                 /* sizeof(fx_host) as built by the host */
    void  *(*alloc)(size_t n);     /* one heap across the boundary (X-17) */
    void   (*free)(void *p);
    int    (*cancelled)(const void *job);  /* poll at least once per row */
    void   (*log)(int level, const char *utf8);   /* 0 info, 1 warn, 2 error */
} fx_host;

/* fx_effect.flags */
#define FX_FLAG_GPU            1u  /* reserved for ABI v2 */
#define FX_FLAG_SINGLE_THREAD  2u  /* host calls render once with the whole
                                      selection bounds as ROI */
#define FX_FLAG_MASK_ONLY      4u  /* src/dst are A8 selection masks */
#define FX_FLAG_NO_DIALOG      8u  /* runs immediately with default params
                                      (Invert Colors, Black and White, ...) */
#define FX_FLAG_ADJUSTMENT    16u  /* listed in the Adjustments menu */

typedef struct fx_effect {
    uint32_t       size;           /* sizeof(fx_effect) the plugin was built with */
    const char    *id;             /* unique reverse-DNS id, "org.paintc.blur.gaussian" */
    const char    *menu;           /* "Effects/Blurs/Gaussian Blur",
                                      "Adjustments/Invert Colors" */
    const fx_prop *props;
    uint32_t       n_props;
    uint32_t       params_size;    /* bytes of the params blob */
    uint32_t       flags;          /* FX_FLAG_* */

    /* Optional. Write defaults for FXP_CUSTOM blobs (and anything else not
     * expressible as fx_prop.def). The host first fills every non-custom
     * prop from its def, then calls this. */
    void  (*init_params)(void *params);

    /* Optional. Build immutable state shared by all render calls of one
     * invocation (histograms, LUTs, kernels, noise tables, distance maps).
     * Runs once, on a worker thread, before any render. Return FX_OK and set
     * *state (may stay NULL), or FX_CANCELLED / FX_ERROR. Allocate with
     * host->alloc. */
    int   (*prepare)(const void *params, const fx_img *src, const fx_env *env,
                     const fx_host *host, const void *job, void **state);
    void  (*release)(void *state, const fx_host *host);   /* optional */

    /* Required. Render roi into dst (see the model above). roi lies inside
     * env->sel and inside src->r. Thread-safe for disjoint ROIs. */
    int   (*render)(const void *params, const void *state, const fx_img *src,
                    fx_img *dst, fx_rect roi, const fx_env *env,
                    const fx_host *host, const void *job);
} fx_effect;

/* A plugin library exports exactly one symbol named FX_ENTRY_NAME with this
 * type. It calls reg() once per effect and returns the number registered,
 * or a negative value on failure. Effect structs must stay valid until the
 * library is unloaded. Built-in effect modules use the same signature.
 * reg() returns >= 0 when the effect was accepted and a negative value when
 * the host rejected it (invalid struct, duplicate id). */
typedef int (*fx_entry_fn)(const fx_host *host, int (*reg)(const fx_effect *fx));
#define FX_ENTRY_NAME "fx_entry"

#if defined(_WIN32)
#  define FX_EXPORT __declspec(dllexport)
#else
#  define FX_EXPORT __attribute__((visibility("default")))
#endif

#endif /* FX_ABI_H */
