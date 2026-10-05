/* pal_internal.h - private interface of the platform layer (lane L0).
 *
 * Shared between the portable files (pal_common.c, pal_pool.c, pal_path.c,
 * pal_clip.c) and exactly one OS file (pal_posix.c or pal_win32.c). Never
 * included by the app; white-box tests in tests/pal may include it.
 *
 * Naming: pal__* functions are internal. pal__os_* functions are the OS
 * hooks; each OS file implements all of them.
 */
#ifndef PAL_INTERNAL_H
#define PAL_INTERNAL_H

#include "pal/pal.h"
#include "pal/pal_clip_raw.h"

#include <stddef.h>
#include <stdint.h>

/* ---- lock without lifetime (pal_common.c) ---------------------------------- */
/* Ticket spinlock on pc_atomic_u32 (C11 atomics or Interlocked), so it is
 * usable before pal_init and after pal_quit (static, zero-initialized) and
 * visible to ThreadSanitizer. Only for short critical sections. */
typedef struct pal__spin { pc_atomic_u32 next, serving; } pal__spin;
void pal__spin_lock(pal__spin *s);
void pal__spin_unlock(pal__spin *s);

/* ---- small helpers (pal_common.c) ----------------------------------------- */
/* malloc'ed copy of s, or NULL when s is NULL or memory runs out. */
char *pal__strdup(const char *s);
/* malloc'ed concatenation a + b + c (NULL parts count as ""), or NULL. */
char *pal__concat3(const char *a, const char *b, const char *c);
/* Unformatted pal_log. Any thread. */
void  pal__log_str(int level, const char *msg);
/* 64 bits that differ between calls and processes (temp names, test ids).
 * Not cryptographic. Any thread. */
uint64_t pal__rand64(void);
/* True for '/' and '\\' (path helpers accept both on every OS). */
bool  pal__is_sep(char c);

/* ---- path and glob helpers (pal_path.c) ------------------------------------- */
/* Case-insensitive (ASCII) glob match of name against one or more patterns
 * separated by ';'. '*' matches any run of bytes, '?' one byte. A NULL or
 * empty glob matches everything. Iterative (P-07). */
bool pal__glob_match(const char *glob, const char *name);
/* Case-insensitive (ASCII) ordering used by pal_list_dir, ties broken by
 * plain byte order so the result is total and deterministic. */
int  pal__name_cmp(const char *a, const char *b);

/* ---- test hook (pal_common.c) --------------------------------------------------- */
/* Runs the file dialog completion exactly as SDL does when a dialog ends
 * (list NULL = error, list[0] NULL = cancelled), on the calling thread, so
 * tests can exercise the cross-thread queue without showing a dialog. */
void pal__dialog_simulate(pal_paths_fn cb, void *ud, const pal_filter *f, int nf,
                          const char *const *list, int filter);

/* ---- single instance wire format (pal_common.c) ----------------------------- */
#define PAL__SI_MAX_MSG   (1u << 20)   /* bytes per forwarded message */
#define PAL__SI_MAX_PATHS 4096u
#define PAL__SI_MAX_PATH  32768u       /* bytes per path */
enum { PAL__SI_FAILED = 0, PAL__SI_PRIMARY = 1, PAL__SI_FORWARDED = 2 };
/* Encode n paths into a malloc'ed message. False on OOM or limits. */
bool pal__si_encode(const char *const *paths, int n, uint8_t **msg, size_t *len);
/* Decode a message into a malloc'ed array of n malloc'ed strings. False for
 * any malformed message (nothing allocated then). */
bool pal__si_decode(const uint8_t *msg, size_t len, char ***paths, int *n);
/* Called by OS listener threads with one received message. Validates it and
 * queues it for pal_pump. Any thread. */
void pal__si_received(const uint8_t *msg, size_t len);

/* ---- image flavour encoders (pal_clip.c, pure, any thread) ------------------- */
/* 32 bpp BI_BITFIELDS bitmap with a BITMAPV5HEADER (alpha mask set, sRGB),
 * bottom-up. With file_header the 14-byte BITMAPFILEHEADER is prepended
 * (a .bmp file); without it the result is a packed DIB for CF_DIBV5.
 * malloc'ed, NULL on bad arguments, size limits or OOM. */
uint8_t *pal__enc_bmp(const uint8_t *bgra, int32_t w, int32_t h, size_t stride,
                      bool file_header, size_t *out_len);
/* Baseline little-endian TIFF: one strip, uncompressed RGBA8 with
 * unassociated alpha. malloc'ed, NULL on bad arguments, limits or OOM. */
uint8_t *pal__enc_tiff(const uint8_t *bgra, int32_t w, int32_t h, size_t stride,
                       size_t *out_len);
/* Packed DIB from another application (CF_DIB or CF_DIBV5 contents, n
 * bytes, untrusted) to a .bmp file: validates the header against n and
 * prepends a BITMAPFILEHEADER with the correct pixel offset. malloc'ed,
 * NULL when the DIB is malformed. */
uint8_t *pal__dib_to_bmp(const uint8_t *dib, size_t n, size_t *out_len);

/* ---- OS hooks (pal_posix.c or pal_win32.c) ------------------------------------ */
/* Called first in pal_init and last in pal_quit. Main thread. */
bool  pal__os_init(void);
void  pal__os_quit(void);
/* Per-user directories for the folder names lower ("paintc") and display
 * ("paint.c"): out[0..3] receive malloc'ed absolute paths with a trailing
 * separator for PAL_DIR_CONFIG, DATA, CACHE and STATE. */
bool  pal__os_user_dirs(const char *lower, const char *display, char *out[4]);
/* Home directory with a trailing separator, malloc'ed, or NULL. */
char *pal__os_home(void);
/* NULL-terminated malloc'ed array of malloc'ed font directories. */
char **pal__os_font_dirs(void);
/* mkdir -p. private_mode creates missing components with mode 0700 on
 * POSIX (the Windows profile ACLs already restrict them). */
bool  pal__os_mkdirs(const char *path, bool private_mode);
/* Extra log sink (OutputDebugString on Windows). Any thread. */
void  pal__os_log_native(const char *line);
/* Append-only log file that other programs may read while it is open
 * (SDL_IOFromFile opens files exclusively on Windows). The handle is
 * opaque; NULL on failure. Writes are serialized by the caller. */
void *pal__os_log_open(const char *path);
void  pal__os_log_write(void *h, const char *data, size_t n);
void  pal__os_log_close(void *h);
/* malloc'ed absolute form of path (the file need not exist), or NULL. */
char *pal__os_abspath(const char *path);
/* Clamp a logical CPU count to the CPUs this process may run on. */
uint32_t pal__os_cpu_limit(uint32_t n);
/* Calls add(ctx, name) for each entry of dir except "." and "..". Returns
 * false when dir cannot be opened or add returned false (out of memory). */
bool  pal__os_list_dir(const char *dir, bool (*add)(void *ctx, const char *name),
                       void *ctx);
bool  pal__os_reveal(const char *path);
/* Become the primary instance (start a listener thread that calls
 * pal__si_received) or forward msg to the running one. Returns one of
 * PAL__SI_*. app_id is already sanitized to [A-Za-z0-9._-]. */
int   pal__os_single_instance(const char *app_id, const uint8_t *msg, size_t len);
void  pal__os_single_instance_stop(void);
/* Per-frame OS work (reaping helper processes). Main thread. */
void  pal__os_pump(void);

#endif /* PAL_INTERNAL_H */
