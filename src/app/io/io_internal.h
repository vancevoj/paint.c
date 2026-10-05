/* io_internal.h - helpers shared by the lane I files (src/app/fileio.c,
 * the files in src/app/io, src/app/mods/mod_file.c). Not a public interface.
 *
 * Thread rules: functions marked "any thread" are pure or only touch their
 * arguments; everything else is main thread. Ownership is stated per
 * function.
 */
#ifndef IO_INTERNAL_H
#define IO_INTERNAL_H

#include "../app_internal.h"
#include "app/app_io.h"

/* Recent file thumbnails: longest side in pixels. */
#define IO_THUMB_MAX 48

/* Copy dpi, ICC profile and metadata items. dst is overwritten (not freed
 * first). PC_ERR_NOMEM leaves dst empty. Any thread. */
pc_status io_meta_copy(const pc_image_meta *src, pc_image_meta *dst);

/* Point-sampled composite thumbnail of d (straight RGBA, longest side at
 * most max, at least 1 x 1). Owned: free(). NULL on OOM. Any thread while
 * nobody mutates d (snapshots and freshly decoded documents). */
uint8_t *io_thumb_rgba(const pc_doc *d, int32_t max, int32_t *w, int32_t *h);

/* Seconds since 1970 (UTC). Any thread. */
int64_t  io_unix_now(void);

/* Per-user folder for lane I data of kind (PAL_DIR_CACHE, PAL_DIR_STATE):
 * pal_dir(kind) normally; <config dir>/<sub> when the app runs with its own
 * config dir; false (no folder) when settings are disabled (config dir "").
 * The folder is created. */
bool     io_user_dir(const app *a, pal_dir_kind kind, const char *sub, char *out, size_t cap);

/* FNV-1a 64 of a string (file names for per-path data). Any thread. */
uint64_t io_hash(const char *s);

/* Write a small raw RGBA image file ("PCTH", w, h, pixels) / read it back
 * with strict validation (at most max x max). Any thread. */
pc_status io_thumb_write(const char *path, const uint8_t *rgba, int32_t w, int32_t h);
uint8_t *io_thumb_read(const char *path, int32_t max, int32_t *w, int32_t *h);

/* ---- drop.c --------------------------------------------------------------------------- */
/* SDL drop events (files dropped on the window, documents the OS opens). */
void     app_drop_event(app *a, const SDL_Event *e);

/* ---- recent.c ------------------------------------------------------------------------- */
/* A fresh thumbnail for path (opened or saved just now). rgba ownership
 * moves to the recent list (may be NULL: forget the old one). */
void     io_recent_thumb(app *a, const char *path, uint8_t *rgba, int32_t w, int32_t h);
/* Cache file for path's thumbnail (false when there is no cache folder). */
bool     io_recent_thumb_path(const app *a, const char *path, char *out, size_t cap);

/* ---- fileio.c ---------------------------------------------------------------------------- */
/* File type filters in Paint.NET's order (OBSERVED 3.3) with "Name (*.ext)"
 * labels; the open variant starts with "All images" and ends with
 * "All files". */
typedef struct io_filters {
    pal_filter      f[40];
    const pc_codec *codec[40];        /* NULL for "All images" / "All files" */
    char            label[40][96];
    int             n;
    char            all[512];
} io_filters;
void     io_build_filters(io_filters *fs, bool save);

/* The codec that saves to path: its extension when that names a type that
 * can save, else fallback (may be NULL). */
const pc_codec *io_codec_for_path(const char *path, const pc_codec *fallback);

/* Remembered save options of a codec (settings file.save.<id>.<prop>),
 * written over the defaults in params / stored from params. */
void     io_params_load(app *a, const pc_codec *c, void *params);
void     io_params_store(app *a, const pc_codec *c, const void *params);

#endif /* IO_INTERNAL_H */
