/* pal_common.c - portable part of the platform layer: lifecycle, logging,
 * time and CPU queries, well-known directories, file dialogs, shell
 * helpers and the single-instance message queue. OS specifics live behind
 * the pal__os_* hooks (pal_posix.c, pal_win32.c).
 *
 * Global state is written by pal_init and pal_quit on the main thread.
 * Everything other threads may touch afterwards is either immutable until
 * pal_quit (directory strings) or guarded by a pal__spin lock (log sink,
 * dialog results, forwarded paths).
 */
#include "pal_internal.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- spin lock ------------------------------------------------------------------ */
void pal__spin_lock(pal__spin *s)
{
    uint32_t ticket = pc_atomic_inc(&s->next) - 1u;
    unsigned spins = 0;
    while (pc_atomic_load(&s->serving) != ticket) {
        if (++spins > 64u) SDL_Delay(0);     /* yield to the holder */
    }
}

void pal__spin_unlock(pal__spin *s)
{
    /* Only the holder writes serving, so load + release store is atomic
     * enough and publishes the critical section. */
    pc_atomic_store(&s->serving, pc_atomic_load(&s->serving) + 1u);
}

/* ---- small helpers --------------------------------------------------------------- */
char *pal__strdup(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s) + 1u;
    d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *pal__concat3(const char *a, const char *b, const char *c)
{
    size_t la = a ? strlen(a) : 0u, lb = b ? strlen(b) : 0u, lc = c ? strlen(c) : 0u;
    size_t total;
    char *d;
    if (!pc_add_size(la, lb, &total) || !pc_add_size(total, lc, &total) ||
        !pc_add_size(total, 1u, &total))
        return NULL;
    d = (char *)malloc(total);
    if (!d) return NULL;
    if (la) memcpy(d, a, la);
    if (lb) memcpy(d + la, b, lb);
    if (lc) memcpy(d + la + lb, c, lc);
    d[la + lb + lc] = '\0';
    return d;
}

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t pal__rand64(void)
{
    static pc_atomic_u32 counter;
    SDL_Time now = 0;
    uint64_t x;
    int local = 0;
    (void)SDL_GetCurrentTime(&now);
    x = splitmix64((uint64_t)now);
    x = splitmix64(x ^ SDL_GetTicksNS());
    x = splitmix64(x ^ (uint64_t)SDL_GetCurrentThreadID());
    x = splitmix64(x ^ (uint64_t)(uintptr_t)&local);
    return splitmix64(x ^ pc_atomic_inc(&counter));
}

/* lane UIB (wave 4 item 36): see pal_internal.h. Short waits first, so a
 * brief hold costs a millisecond or two; then 100 ms steps up to the
 * budget, the same total as the .NET clipboard default (10 x 100 ms). */
int pal__clip_retry_delay(int attempt, uint32_t elapsed_ms)
{
    int d;
    if (attempt < 0 || elapsed_ms >= PAL__CLIP_RETRY_BUDGET_MS) return -1;
    d = attempt < 7 ? 1 << attempt : 100;
    if (d > 100) d = 100;
    if ((uint32_t)d > PAL__CLIP_RETRY_BUDGET_MS - elapsed_ms)
        d = (int)(PAL__CLIP_RETRY_BUDGET_MS - elapsed_ms);
    return d;
}

/* ---- logging --------------------------------------------------------------------- */
static pal__spin     g_log_lock;
static void         *g_log_io;              /* guarded by g_log_lock */
static pc_atomic_u32 g_log_min = { PAL_LOG_INFO };
static SDL_LogOutputFunction g_prev_sdl_log;
static void         *g_prev_sdl_log_ud;

static void log_emit(int level, const char *msg)
{
    static const char k_lv[4] = { 'D', 'I', 'W', 'E' };
    char stackbuf[1024];
    char *line = stackbuf;
    size_t ml, hl, total;
    char head[40];
    int n;
    double t = (double)SDL_GetTicksNS() * 1e-9;
    if (level < PAL_LOG_DEBUG) level = PAL_LOG_DEBUG;
    if (level > PAL_LOG_ERROR) level = PAL_LOG_ERROR;
    n = snprintf(head, sizeof head, "[%10.3f] %c ", t, k_lv[level]);
    if (n < 0) return;
    hl = strlen(head);
    ml = strlen(msg);
    while (ml > 0u && (msg[ml - 1u] == '\n' || msg[ml - 1u] == '\r')) ml--;
    total = hl + ml + 2u;                   /* newline and NUL */
    if (total > sizeof stackbuf) {
        line = (char *)malloc(total);
        if (!line) {                        /* out of memory: truncate */
            line = stackbuf;
            ml = sizeof stackbuf - hl - 2u;
        }
    }
    memcpy(line, head, hl);
    memcpy(line + hl, msg, ml);
    line[hl + ml] = '\n';
    line[hl + ml + 1u] = '\0';

    pal__spin_lock(&g_log_lock);
    fputs(line, stderr);
    fflush(stderr);
    if (g_log_io) pal__os_log_write(g_log_io, line, hl + ml + 1u);
    pal__spin_unlock(&g_log_lock);
    pal__os_log_native(line);
    if (line != stackbuf) free(line);
}

void pal__log_str(int level, const char *msg)
{
    if (level < (int)pc_atomic_load(&g_log_min)) return;
    log_emit(level, msg ? msg : "");
}

void pal_log(int level, const char *fmt, ...)
{
    char buf[1024];
    char *msg = buf;
    va_list ap;
    int n;
    if (level < (int)pc_atomic_load(&g_log_min) || !fmt) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        log_emit(level, fmt);               /* bad format: log it verbatim */
        return;
    }
    if ((size_t)n >= sizeof buf) {
        char *big = (char *)malloc((size_t)n + 1u);
        if (big) {
            va_start(ap, fmt);
            (void)vsnprintf(big, (size_t)n + 1u, fmt, ap);
            va_end(ap);
            msg = big;
        }
    }
    log_emit(level, msg);
    if (msg != buf) free(msg);
}

static void SDLCALL sdl_log_cb(void *ud, int category, SDL_LogPriority prio, const char *message)
{
    int level = PAL_LOG_INFO;
    char buf[1024];
    (void)ud;
    (void)category;
    if (prio <= SDL_LOG_PRIORITY_DEBUG)     level = PAL_LOG_DEBUG;
    else if (prio == SDL_LOG_PRIORITY_INFO) level = PAL_LOG_INFO;
    else if (prio == SDL_LOG_PRIORITY_WARN) level = PAL_LOG_WARN;
    else                                    level = PAL_LOG_ERROR;
    if (level < (int)pc_atomic_load(&g_log_min)) return;
    (void)snprintf(buf, sizeof buf, "SDL: %s", message ? message : "");
    log_emit(level, buf);
}

/* Log file in PAL_DIR_STATE, rotated once at 1 MiB (one .old generation). */
static void log_open_file(const char *state_dir, const char *lower)
{
    char *path = pal__concat3(state_dir, lower, ".log");
    char *old = path ? pal__concat3(path, ".old", NULL) : NULL;
    void *io;
    if (path && old && pal_file_exists(path)) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(path, &info) && info.size > (Uint64)(1u << 20)) {
            (void)pal_remove(old);
            (void)SDL_RenamePath(path, old);
        }
    }
    io = path ? pal__os_log_open(path) : NULL;
    if (io) {
        SDL_Time now = 0;
        SDL_DateTime dt;
        char head[96];
        memset(&dt, 0, sizeof dt);
        if (SDL_GetCurrentTime(&now)) (void)SDL_TimeToDateTime(now, &dt, false);
        (void)snprintf(head, sizeof head,
                       "---- log opened %04d-%02d-%02dT%02d:%02d:%02dZ ----\n",
                       dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        pal__os_log_write(io, head, strlen(head));
        pal__spin_lock(&g_log_lock);
        g_log_io = io;
        pal__spin_unlock(&g_log_lock);
    }
    free(old);
    free(path);
}

static void log_close_file(void)
{
    void *io;
    pal__spin_lock(&g_log_lock);
    io = g_log_io;
    g_log_io = NULL;
    pal__spin_unlock(&g_log_lock);
    if (io) pal__os_log_close(io);
}

/* ---- time and machine ----------------------------------------------------------- */
static pc_atomic_u32 g_cpu_clear;              /* PAL_CPU_* bits forced off */

uint64_t pal_ticks_ns(void) { return SDL_GetTicksNS(); }

uint32_t pal_cpu_count(void)
{
    int n = SDL_GetNumLogicalCPUCores();
    uint32_t c = n > 0 ? (uint32_t)n : 1u;
    c = pal__os_cpu_limit(c);
    return c > 0u ? c : 1u;
}

uint64_t pal_ram_bytes(void)
{
    int mib = SDL_GetSystemRAM();
    return mib > 0 ? (uint64_t)mib * 1024u * 1024u : 0u;
}

uint32_t pal_cpu_features(void)
{
    uint32_t f = 0;
    if (SDL_HasSSE41()) f |= PAL_CPU_SSE41;
    if (SDL_HasAVX2())  f |= PAL_CPU_AVX2;
    if (SDL_HasNEON())  f |= PAL_CPU_NEON;
    return f & ~pc_atomic_load(&g_cpu_clear);
}

/* ---- directories ----------------------------------------------------------------- */
static pc_atomic_u32 g_inited;
static char         *g_dirs[PAL_DIR_COUNT];
static char        **g_fonts;
static const char   *const k_no_fonts[1] = { NULL };

/* Folder names from app (display) or the last app_id component. lower keeps
 * [a-z0-9_-] ("paint.c" -> "paintc") for Linux and Windows; display keeps
 * the name except path syntax and control characters, for macOS. */
static void app_names(const char *app_id, const char *app, char *lower, char *display,
                      size_t cap)
{
    const char *src = (app && *app) ? app : NULL;
    size_t nl = 0, nd = 0;
    if (!src && app_id && *app_id) {
        const char *dot = strrchr(app_id, '.');
        src = dot ? dot + 1 : app_id;
    }
    if (!src || !*src) src = "paintc";
    for (const char *p = src; *p && nd + 1u < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20u || c == 0x7Fu || c == '/' || c == '\\' || c == ':') continue;
        if (nd == 0u && (c == '.' || c == ' ')) continue;
        display[nd++] = (char)c;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') &&
            nl + 1u < cap)
            lower[nl++] = (char)c;
    }
    /* the cap may have cut a multi-byte sequence: drop the partial one */
    if (nd > 0u && ((unsigned char)display[nd - 1u] & 0x80u)) {
        size_t lead = nd - 1u, need;
        unsigned char c;
        while (lead > 0u && ((unsigned char)display[lead] & 0xC0u) == 0x80u) lead--;
        c = (unsigned char)display[lead];
        need = c >= 0xF0u ? 4u : c >= 0xE0u ? 3u : c >= 0xC0u ? 2u : 1u;
        if (nd - lead < need) nd = lead;
    }
    display[nd] = '\0';
    lower[nl] = '\0';
    if (nd == 0u) { (void)snprintf(display, cap, "%s", "paintc"); }
    if (nl == 0u) { (void)snprintf(lower, cap, "%s", "paintc"); }
}

static char *with_sep(const char *dir)
{
    size_t n;
    char sep[2];
    if (!dir || !*dir) return NULL;
    n = strlen(dir);
    if (pal__is_sep(dir[n - 1u])) return pal__strdup(dir);
    sep[0] = pal_path_sep();
    sep[1] = '\0';
    return pal__concat3(dir, sep, NULL);
}

static char *user_folder(SDL_Folder f)
{
    const char *s = SDL_GetUserFolder(f);
    return s ? with_sep(s) : NULL;
}

const char *pal_dir(pal_dir_kind k)
{
    if ((unsigned)k >= (unsigned)PAL_DIR_COUNT || !pc_atomic_load(&g_inited)) return NULL;
    /* One stat per call: the folder may have been deleted while running. */
    if (k <= PAL_DIR_STATE && !pal_is_dir(g_dirs[k])) (void)pal__os_mkdirs(g_dirs[k], true);
    return g_dirs[k];
}

const char *const *pal_font_dirs(void)
{
    if (!pc_atomic_load(&g_inited) || !g_fonts) return k_no_fonts;
    return (const char *const *)g_fonts;
}

static void free_dirs(void)
{
    for (int i = 0; i < (int)PAL_DIR_COUNT; i++) {
        free(g_dirs[i]);
        g_dirs[i] = NULL;
    }
    if (g_fonts) {
        for (size_t i = 0; g_fonts[i]; i++) free(g_fonts[i]);
        free(g_fonts);
        g_fonts = NULL;
    }
}

/* ---- dialogs --------------------------------------------------------------------- */
typedef struct dlg_req {
    struct dlg_req       *next;
    pal_paths_fn          cb;
    void                 *ud;
    SDL_DialogFileFilter *filters;     /* deep copy, valid until delivery */
    int                   nf;
    char                **paths;
    int                   n;
    int                   filter;
} dlg_req;

static pal__spin     g_dlg_lock;
static dlg_req      *g_dlg_head, *g_dlg_tail;   /* finished, guarded */
static pc_atomic_u32 g_accepting;               /* 1 between init and quit */

static void dlg_free(dlg_req *r)
{
    if (!r) return;
    pal_free_names(r->paths, r->n);
    free(r->filters);
    free(r);
}

static dlg_req *dlg_new(pal_paths_fn cb, void *ud, const pal_filter *f, int nf)
{
    dlg_req *r = (dlg_req *)calloc(1u, sizeof *r);
    size_t bytes, strings = 0;
    char *sp;
    if (!r) return NULL;
    r->cb = cb;
    r->ud = ud;
    r->filter = -1;
    if (!f || nf <= 0) return r;
    for (int i = 0; i < nf; i++) {
        strings += strlen(f[i].name ? f[i].name : "") + 1u;
        strings += strlen(f[i].pattern ? f[i].pattern : "*") + 1u;
    }
    if (!pc_mul_size((size_t)nf, sizeof *r->filters, &bytes) ||
        !pc_add_size(bytes, strings, &bytes)) {
        free(r);
        return NULL;
    }
    r->filters = (SDL_DialogFileFilter *)malloc(bytes);
    if (!r->filters) {
        free(r);
        return NULL;
    }
    sp = (char *)(r->filters + nf);
    for (int i = 0; i < nf; i++) {
        const char *name = f[i].name ? f[i].name : "";
        const char *pat = f[i].pattern ? f[i].pattern : "*";
        size_t ln = strlen(name) + 1u, lp = strlen(pat) + 1u;
        memcpy(sp, name, ln);
        r->filters[i].name = sp;
        sp += ln;
        memcpy(sp, pat, lp);
        r->filters[i].pattern = sp;
        sp += lp;
    }
    r->nf = nf;
    return r;
}

/* SDL calls this exactly once per dialog, on any thread, possibly before
 * SDL_Show*Dialog returns. */
static void SDLCALL dlg_done(void *ud, const char *const *list, int filter)
{
    dlg_req *r = (dlg_req *)ud;
    r->filter = filter;
    if (!list) {
        pal_log(PAL_LOG_WARN, "file dialog failed: %s", SDL_GetError());
    } else {
        int n = 0;
        while (list[n]) n++;
        if (n > 0) {
            r->paths = (char **)calloc((size_t)n, sizeof *r->paths);
            if (r->paths) {
                r->n = n;
                for (int i = 0; i < n; i++) {
                    r->paths[i] = pal__strdup(list[i]);
                    if (!r->paths[i]) {
                        pal_free_names(r->paths, n);
                        r->paths = NULL;
                        r->n = 0;
                        pal__log_str(PAL_LOG_ERROR, "file dialog: out of memory");
                        break;
                    }
                }
            }
        }
    }
    pal__spin_lock(&g_dlg_lock);
    if (pc_atomic_load(&g_accepting)) {
        r->next = NULL;
        if (g_dlg_tail) g_dlg_tail->next = r;
        else            g_dlg_head = r;
        g_dlg_tail = r;
        r = NULL;
    }
    pal__spin_unlock(&g_dlg_lock);
    dlg_free(r);                            /* only after pal_quit */
}

void pal_dialog_open(pal_window *w, const pal_filter *f, int nf, const char *default_dir,
                     bool multi, pal_paths_fn cb, void *ud)
{
    dlg_req *r = dlg_new(cb, ud, f, nf);
    if (!r) {
        pal__log_str(PAL_LOG_ERROR, "pal_dialog_open: out of memory");
        return;
    }
    SDL_ShowOpenFileDialog(dlg_done, r, w, r->filters, r->nf, default_dir, multi);
}

void pal_dialog_save(pal_window *w, const pal_filter *f, int nf, const char *default_path,
                     pal_paths_fn cb, void *ud)
{
    dlg_req *r = dlg_new(cb, ud, f, nf);
    if (!r) {
        pal__log_str(PAL_LOG_ERROR, "pal_dialog_save: out of memory");
        return;
    }
    SDL_ShowSaveFileDialog(dlg_done, r, w, r->filters, r->nf, default_path);
}

void pal_dialog_folder(pal_window *w, const char *default_dir, pal_paths_fn cb, void *ud)
{
    dlg_req *r = dlg_new(cb, ud, NULL, 0);
    if (!r) {
        pal__log_str(PAL_LOG_ERROR, "pal_dialog_folder: out of memory");
        return;
    }
    SDL_ShowOpenFolderDialog(dlg_done, r, w, default_dir, false);
}

void pal__dialog_simulate(pal_paths_fn cb, void *ud, const pal_filter *f, int nf,
                          const char *const *list, int filter)
{
    dlg_req *r = dlg_new(cb, ud, f, nf);
    if (r) dlg_done(r, list, filter);
}

static void dlg_deliver(void)
{
    dlg_req *r;
    pal__spin_lock(&g_dlg_lock);
    r = g_dlg_head;
    g_dlg_head = g_dlg_tail = NULL;
    pal__spin_unlock(&g_dlg_lock);
    while (r) {
        dlg_req *next = r->next;
        if (r->cb)
            r->cb(r->ud, r->n > 0 ? (const char *const *)r->paths : NULL, r->n,
                  r->n > 0 ? r->filter : -1);
        dlg_free(r);
        r = next;
    }
}

/* ---- shell ----------------------------------------------------------------------- */
bool pal_open_url(const char *url)
{
    if (!url || !*url) return false;
    if (!SDL_OpenURL(url)) {
        pal_log(PAL_LOG_WARN, "pal_open_url failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool pal_reveal_file(const char *path)
{
    if (!path || !*path) return false;
    return pal__os_reveal(path);
}

/* ---- single instance ------------------------------------------------------------- */
typedef struct si_msg {
    struct si_msg *next;
    char         **paths;
    int            n;
} si_msg;

static pal__spin    g_si_lock;
static si_msg      *g_si_head, *g_si_tail;      /* guarded by g_si_lock */
static pal_paths_fn g_si_cb;                    /* main thread */
static void        *g_si_ud;
static bool         g_si_primary;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Wire format: "PCSI", u32 version 1, u32 count, then count times
 * (u32 length, length bytes of UTF-8 without NUL). Little-endian. */
bool pal__si_encode(const char *const *paths, int n, uint8_t **msg, size_t *len)
{
    size_t total = 12u, off;
    uint8_t *m;
    if (!msg || !len || n < 0 || (uint32_t)n > PAL__SI_MAX_PATHS || (n > 0 && !paths))
        return false;
    *msg = NULL;
    *len = 0;
    for (int i = 0; i < n; i++) {
        size_t l = paths[i] ? strlen(paths[i]) : 0u;
        if (l > PAL__SI_MAX_PATH) return false;
        total += 4u + l;
        if (total > PAL__SI_MAX_MSG) return false;
    }
    m = (uint8_t *)malloc(total);
    if (!m) return false;
    memcpy(m, "PCSI", 4u);
    put_le32(m + 4, 1u);
    put_le32(m + 8, (uint32_t)n);
    off = 12u;
    for (int i = 0; i < n; i++) {
        size_t l = paths[i] ? strlen(paths[i]) : 0u;
        put_le32(m + off, (uint32_t)l);
        if (l) memcpy(m + off + 4u, paths[i], l);
        off += 4u + l;
    }
    *msg = m;
    *len = total;
    return true;
}

bool pal__si_decode(const uint8_t *msg, size_t len, char ***paths, int *n)
{
    uint32_t count;
    size_t off = 12u;
    char **v = NULL;
    if (!paths || !n) return false;
    *paths = NULL;
    *n = 0;
    if (!msg || len < 12u || len > PAL__SI_MAX_MSG || memcmp(msg, "PCSI", 4u) != 0 ||
        get_le32(msg + 4) != 1u)
        return false;
    count = get_le32(msg + 8);
    if (count > PAL__SI_MAX_PATHS) return false;
    if (count) {
        v = (char **)calloc(count, sizeof *v);
        if (!v) return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t l;
        if (len - off < 4u) goto bad;
        l = get_le32(msg + off);
        off += 4u;
        if (l > PAL__SI_MAX_PATH || l > len - off || memchr(msg + off, 0, l)) goto bad;
        v[i] = (char *)malloc((size_t)l + 1u);
        if (!v[i]) goto bad;
        memcpy(v[i], msg + off, l);
        v[i][l] = '\0';
        off += l;
    }
    if (off != len) goto bad;
    *paths = v;
    *n = (int)count;
    return true;
bad:
    pal_free_names(v, (int)count);
    return false;
}

void pal__si_received(const uint8_t *msg, size_t len)
{
    si_msg *m = (si_msg *)calloc(1u, sizeof *m);
    if (!m) return;
    if (!pal__si_decode(msg, len, &m->paths, &m->n)) {
        pal__log_str(PAL_LOG_WARN, "single instance: ignored a malformed message");
        free(m);
        return;
    }
    pal__spin_lock(&g_si_lock);
    if (g_si_tail) g_si_tail->next = m;
    else           g_si_head = m;
    g_si_tail = m;
    pal__spin_unlock(&g_si_lock);
}

static si_msg *si_take_all(void)
{
    si_msg *m;
    pal__spin_lock(&g_si_lock);
    m = g_si_head;
    g_si_head = g_si_tail = NULL;
    pal__spin_unlock(&g_si_lock);
    return m;
}

static void si_deliver(void)
{
    si_msg *m = si_take_all();
    while (m) {
        si_msg *next = m->next;
        if (g_si_cb)
            g_si_cb(g_si_ud, m->n > 0 ? (const char *const *)m->paths : NULL, m->n, -1);
        pal_free_names(m->paths, m->n);
        free(m);
        m = next;
    }
}

bool pal_single_instance(const char *app_id, int argc, const char *const *paths,
                         pal_paths_fn cb, void *ud)
{
    char name[80];
    size_t k = 0;
    char **abs = NULL;
    uint8_t *msg = NULL;
    size_t len = 0;
    int r;
    bool ok;
    g_si_cb = cb;
    g_si_ud = ud;
    if (g_si_primary) return true;
    if (!app_id || !*app_id) return true;
    for (const char *p = app_id; *p && k + 1u < sizeof name; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            name[k++] = c;
    }
    name[k] = '\0';
    if (k == 0u) return true;
    if (argc < 0 || !paths) argc = 0;
    if (argc > (int)PAL__SI_MAX_PATHS) argc = (int)PAL__SI_MAX_PATHS;
    if (argc > 0) {
        abs = (char **)calloc((size_t)argc, sizeof *abs);
        if (!abs) return true;
        for (int i = 0; i < argc; i++) {
            /* The primary has another working directory. */
            abs[i] = paths[i] ? pal__os_abspath(paths[i]) : NULL;
            if (!abs[i]) abs[i] = pal__strdup(paths[i] ? paths[i] : "");
        }
    }
    ok = pal__si_encode((const char *const *)abs, argc, &msg, &len);
    pal_free_names(abs, argc);
    if (!ok) {
        pal__log_str(PAL_LOG_WARN, "single instance: arguments too large to forward");
        return true;
    }
    r = pal__os_single_instance(name, msg, len);
    free(msg);
    if (r == PAL__SI_FORWARDED) return false;
    if (r == PAL__SI_PRIMARY) g_si_primary = true;
    return true;
}

/* ---- lifecycle ------------------------------------------------------------------- */
static void read_env(void)
{
    const char *lv = SDL_getenv("PAINTC_LOG");
    const char *mask = SDL_getenv("PAINTC_CPU_FEATURES");
    if (lv) {
        if (SDL_strcasecmp(lv, "debug") == 0)      pc_atomic_store(&g_log_min, PAL_LOG_DEBUG);
        else if (SDL_strcasecmp(lv, "warn") == 0)  pc_atomic_store(&g_log_min, PAL_LOG_WARN);
        else if (SDL_strcasecmp(lv, "error") == 0) pc_atomic_store(&g_log_min, PAL_LOG_ERROR);
    }
    if (mask && *mask) pc_atomic_store(&g_cpu_clear, ~(uint32_t)SDL_strtoul(mask, NULL, 0));
}

bool pal_init(const char *app_id, const char *org, const char *app)
{
    char lower[64], display[64];
    char *user[4] = { NULL, NULL, NULL, NULL };
    const char *base;
    const char *lv;
    (void)org;                              /* folder names come from app */
    if (pc_atomic_load(&g_inited)) return true;
    read_env();
    if (!pal__os_init()) {
        pal__log_str(PAL_LOG_ERROR, "pal_init: platform initialization failed");
        return false;
    }
    app_names(app_id, app, lower, display, sizeof lower);
    if (!pal__os_user_dirs(lower, display, user)) {
        pal__log_str(PAL_LOG_ERROR, "pal_init: cannot determine the per-user directories");
        for (int i = 0; i < 4; i++) free(user[i]);
        pal__os_quit();
        return false;
    }
    for (int i = 0; i < 4; i++) g_dirs[i] = user[i];
    g_dirs[PAL_DIR_DOCUMENTS] = user_folder(SDL_FOLDER_DOCUMENTS);
    if (!g_dirs[PAL_DIR_DOCUMENTS]) g_dirs[PAL_DIR_DOCUMENTS] = pal__os_home();
    g_dirs[PAL_DIR_PICTURES] = user_folder(SDL_FOLDER_PICTURES);
    if (!g_dirs[PAL_DIR_PICTURES])
        g_dirs[PAL_DIR_PICTURES] = pal__strdup(g_dirs[PAL_DIR_DOCUMENTS]);
    base = SDL_GetBasePath();
    g_dirs[PAL_DIR_EXE] = base ? with_sep(base) : NULL;
    if (!g_dirs[PAL_DIR_EXE]) g_dirs[PAL_DIR_EXE] = pal__os_abspath(".");
    if (g_dirs[PAL_DIR_EXE]) {
        char *s = with_sep(g_dirs[PAL_DIR_EXE]);     /* the fallback has none */
        free(g_dirs[PAL_DIR_EXE]);
        g_dirs[PAL_DIR_EXE] = s;
    }
    for (int i = 0; i < (int)PAL_DIR_COUNT; i++) {
        if (!g_dirs[i]) {
            pal__log_str(PAL_LOG_ERROR, "pal_init: out of memory");
            free_dirs();
            pal__os_quit();
            return false;
        }
    }
    for (int i = 0; i <= (int)PAL_DIR_STATE; i++) {
        if (!pal__os_mkdirs(g_dirs[i], true))
            pal_log(PAL_LOG_WARN, "pal_init: cannot create %s", g_dirs[i]);
    }
    g_fonts = pal__os_font_dirs();
    lv = SDL_getenv("PAINTC_LOG");
    if (!lv || SDL_strcasecmp(lv, "off") != 0) log_open_file(g_dirs[PAL_DIR_STATE], lower);
    SDL_GetLogOutputFunction(&g_prev_sdl_log, &g_prev_sdl_log_ud);
    SDL_SetLogOutputFunction(sdl_log_cb, NULL);
    pc_atomic_store(&g_accepting, 1u);
    pc_atomic_store(&g_inited, 1u);
    pal_log(PAL_LOG_INFO, "pal: SDL %d.%d.%d, %u logical CPUs, %llu MiB RAM, cpu 0x%x",
            SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
            SDL_VERSIONNUM_MICRO(SDL_GetVersion()), (unsigned)pal_cpu_count(),
            (unsigned long long)(pal_ram_bytes() >> 20), (unsigned)pal_cpu_features());
    return true;
}

void pal_quit(void)
{
    dlg_req *r;
    si_msg *m;
    if (!pc_atomic_load(&g_inited)) return;
    pal__os_single_instance_stop();
    g_si_primary = false;
    g_si_cb = NULL;
    g_si_ud = NULL;
    pc_atomic_store(&g_accepting, 0u);
    /* Results nobody will receive any more. */
    pal__spin_lock(&g_dlg_lock);
    r = g_dlg_head;
    g_dlg_head = g_dlg_tail = NULL;
    pal__spin_unlock(&g_dlg_lock);
    while (r) { dlg_req *next = r->next; dlg_free(r); r = next; }
    m = si_take_all();
    while (m) { si_msg *next = m->next; pal_free_names(m->paths, m->n); free(m); m = next; }
    SDL_SetLogOutputFunction(g_prev_sdl_log, g_prev_sdl_log_ud);
    log_close_file();
    pc_atomic_store(&g_inited, 0u);
    free_dirs();
    pal__os_quit();
}

void pal_pump(void)
{
    if (!pc_atomic_load(&g_inited)) return;
    dlg_deliver();
    si_deliver();
    pal__os_pump();
}
