/* pal_posix.c - OS hooks for Linux, the BSDs and macOS.
 *
 * Paths are bytes on POSIX, so UTF-8 passes through unchanged. Only '/'
 * separates components here (a backslash is a valid file name byte).
 */
#if !defined(_WIN32)

#if defined(__linux__) || defined(__GNU__) || defined(__CYGWIN__)
#  define _GNU_SOURCE 1     /* glibc hides POSIX in -std=c17: open it up */
#endif

#include "pal_internal.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#  include <sched.h>
#endif

#ifndef O_CLOEXEC
#  define O_CLOEXEC 0
#endif

static mode_t g_umask = 022;             /* sampled in pal__os_init */

/* ---- helpers --------------------------------------------------------------------- */
static int open_retry(const char *path, int flags, mode_t mode)
{
    int fd;
    do { fd = open(path, flags | O_CLOEXEC, mode); } while (fd < 0 && errno == EINTR);
    return fd;
}

static void close_quiet(int fd)
{
    if (fd >= 0) (void)close(fd);
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0u) {
        size_t chunk = n > ((size_t)1 << 30) ? ((size_t)1 << 30) : n;
        ssize_t w = write(fd, p, chunk);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

static bool full_sync(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    /* fsync on macOS does not flush the drive cache. */
    if (fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return fsync(fd) == 0;
}

/* ---- lifecycle ------------------------------------------------------------------- */
bool pal__os_init(void)
{
    bool have = false;
#if defined(__linux__)
    /* /proc avoids the process-wide umask() toggle. */
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "Umask:", 6) == 0) {
                g_umask = (mode_t)strtoul(line + 6, NULL, 8);
                have = true;
                break;
            }
        }
        fclose(f);
    }
#endif
    if (!have) {
        /* Main thread, before any worker exists. */
        mode_t m = umask(022);
        (void)umask(m);
        g_umask = m;
    }
    return true;
}

/* ---- files ----------------------------------------------------------------------- */
pc_status pal_read_file(const char *path, uint64_t max_bytes, uint8_t **data, size_t *len)
{
    struct stat st;
    size_t cap, alloc, n = 0;
    uint8_t *buf;
    int fd;
    if (data) *data = NULL;
    if (len) *len = 0;
    if (!path || !data || !len) return PC_ERR_ARG;
    fd = open_retry(path, O_RDONLY, 0);
    if (fd < 0) return PC_ERR_IO;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        close_quiet(fd);
        return PC_ERR_IO;
    }
    cap = max_bytes >= (uint64_t)(SIZE_MAX - 1u) ? SIZE_MAX - 1u : (size_t)max_bytes;
    if (S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size > (uint64_t)cap) {
        close_quiet(fd);
        return PC_ERR_LIMIT;                /* checked before allocating (P-08) */
    }
    alloc = S_ISREG(st.st_mode) ? (size_t)st.st_size : (size_t)65536u;
    if (alloc > cap) alloc = cap;
    buf = (uint8_t *)malloc(alloc + 1u);
    if (!buf) {
        close_quiet(fd);
        return PC_ERR_NOMEM;
    }
    for (;;) {
        ssize_t r;
        if (n == alloc) {
            uint8_t probe;
            size_t grow;
            uint8_t *nb;
            if (alloc >= cap) {             /* at the cap: any more byte is too many */
                do { r = read(fd, &probe, 1u); } while (r < 0 && errno == EINTR);
                if (r < 0) goto io_fail;
                if (r > 0) {
                    free(buf);
                    close_quiet(fd);
                    return PC_ERR_LIMIT;
                }
                break;
            }
            grow = alloc < 65536u ? 65536u : alloc;
            if (grow > cap - alloc) grow = cap - alloc;
            nb = (uint8_t *)realloc(buf, alloc + grow + 1u);
            if (!nb) {
                free(buf);
                close_quiet(fd);
                return PC_ERR_NOMEM;
            }
            buf = nb;
            alloc += grow;
        }
        r = read(fd, buf + n, alloc - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            goto io_fail;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close_quiet(fd);
    buf[n] = 0u;
    *data = buf;
    *len = n;
    return PC_OK;
io_fail:
    free(buf);
    close_quiet(fd);
    return PC_ERR_IO;
}

pc_status pal_write_file_atomic(const char *path, const void *data, size_t len)
{
    struct stat st;
    char *target = NULL, *dir = NULL, *tmp = NULL;
    const char *base;
    char shortbase[72];
    mode_t mode = (mode_t)(0666 & ~g_umask);
    size_t dl, bl;
    int fd = -1;
    pc_status rc = PC_ERR_IO;
    if (!path || !*path || (!data && len)) return PC_ERR_ARG;
    /* Saving through a symlink replaces the file it points to. */
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) target = realpath(path, NULL);
    if (!target) target = pal__strdup(path);
    if (!target) return PC_ERR_NOMEM;
    if (stat(target, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            free(target);
            return PC_ERR_ARG;
        }
        mode = (mode_t)(st.st_mode & 07777);
    }
    base = strrchr(target, '/');
    base = base ? base + 1 : target;
    if (!*base) {
        free(target);
        return PC_ERR_ARG;
    }
    dl = (size_t)(base - target);
    dir = (char *)malloc(dl + 2u);
    if (!dir) {
        free(target);
        return PC_ERR_NOMEM;
    }
    if (dl) memcpy(dir, target, dl);
    else    dir[dl++] = '.';
    dir[dl] = '\0';
    /* Short base keeps the temp name inside NAME_MAX; cut at a UTF-8
     * boundary so the name stays valid. */
    bl = strlen(base);
    if (bl > 64u) {
        bl = 64u;
        while (bl > 0u && ((unsigned char)base[bl] & 0xC0u) == 0x80u) bl--;
    }
    memcpy(shortbase, base, bl);
    shortbase[bl] = '\0';
    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
        char name[128];
        (void)snprintf(name, sizeof name, ".%s.%016llx.tmp", shortbase,
                       (unsigned long long)pal__rand64());
        free(tmp);
        tmp = pal__concat3(dir, dir[strlen(dir) - 1u] == '/' ? "" : "/", name);
        if (!tmp) {
            rc = PC_ERR_NOMEM;
            goto done;
        }
        fd = open_retry(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);   /* X-23 */
        if (fd < 0 && errno != EEXIST) goto done;
    }
    if (fd < 0) goto done;
    if (!write_all(fd, (const uint8_t *)data, len) || !full_sync(fd)) goto fail_unlink;
    if (rename(tmp, target) != 0) goto fail_unlink;
    /* Final permissions only once the file has its real name; through the
     * descriptor, so no other file can be affected. */
    (void)fchmod(fd, mode);
    if (close(fd) != 0) {
        /* The complete, fsync'ed data already has its final name. */
        pal_log(PAL_LOG_WARN, "pal_write_file_atomic: close failed after saving %s", path);
    }
    fd = -1;
    {
        int dfd = open_retry(dir, O_RDONLY, 0);   /* make the rename durable */
        if (dfd >= 0) {
            (void)fsync(dfd);
            close_quiet(dfd);
        }
    }
    rc = PC_OK;
    goto done;
fail_unlink:
    close_quiet(fd);
    fd = -1;
    (void)unlink(tmp);
done:
    close_quiet(fd);
    free(tmp);
    free(dir);
    free(target);
    return rc;
}

bool pal_file_exists(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

bool pal_is_dir(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool pal_remove(const char *path)
{
    return path && *path && remove(path) == 0;
}

bool pal_mkdirs(const char *path)
{
    return pal__os_mkdirs(path, false);
}

uint64_t pal_file_mtime(const char *path)
{
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0 || st.st_mtime < 0) return 0u;
    return (uint64_t)st.st_mtime;
}

bool pal__os_mkdirs(const char *path, bool private_mode)
{
    mode_t mode = private_mode ? 0700 : 0777;
    char *buf;
    size_t n;
    bool ok;
    if (!path || !*path) return false;
    if (pal_is_dir(path)) return true;
    buf = pal__strdup(path);
    if (!buf) return false;
    n = strlen(buf);
    while (n > 1u && buf[n - 1u] == '/') buf[--n] = '\0';
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            (void)mkdir(buf, mode);         /* EEXIST is fine; the end result counts */
            buf[i] = c;
        }
    }
    ok = pal_is_dir(buf);
    free(buf);
    return ok;
}

bool pal__os_list_dir(const char *dir, bool (*add)(void *ctx, const char *name), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return false;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!add(ctx, e->d_name)) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    return true;
}

char *pal__os_abspath(const char *path)
{
    char *r, *cwd = NULL;
    size_t cap = 256;
    if (!path) return NULL;
    if (path[0] == '/') return pal__strdup(path);
    r = realpath(path, NULL);
    if (r) {
        /* realpath memory comes from malloc, as the contract requires */
        return r;
    }
    for (;;) {
        char *nb = (char *)realloc(cwd, cap);
        if (!nb) {
            free(cwd);
            return NULL;
        }
        cwd = nb;
        if (getcwd(cwd, cap)) break;
        if (errno != ERANGE || cap > ((size_t)1 << 20)) {
            free(cwd);
            return NULL;
        }
        cap *= 2u;
    }
    r = pal__concat3(cwd, strcmp(cwd, "/") == 0 ? "" : "/", path);
    free(cwd);
    return r;
}

/* ---- machine --------------------------------------------------------------------- */
uint32_t pal__os_cpu_limit(uint32_t n)
{
#if defined(__linux__) && defined(CPU_COUNT)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int c = CPU_COUNT(&set);
        if (c > 0 && (uint32_t)c < n) return (uint32_t)c;
    }
#endif
    return n;
}

void pal__os_log_native(const char *line)
{
    (void)line;
}

/* The descriptor is stored in the pointer (+1 so that fd 0 is not NULL). */
void *pal__os_log_open(const char *path)
{
    int fd = open_retry(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    return fd < 0 ? NULL : (void *)(uintptr_t)((unsigned)fd + 1u);
}

void pal__os_log_write(void *h, const char *data, size_t n)
{
    if (h) (void)write_all((int)((uintptr_t)h - 1u), (const uint8_t *)data, n);
}

void pal__os_log_close(void *h)
{
    if (h) close_quiet((int)((uintptr_t)h - 1u));
}

/* ---- directories ----------------------------------------------------------------- */
char *pal__os_home(void)
{
    const char *h = SDL_getenv("HOME");
    char *r;
    if (h && h[0] == '/') {
        size_t n = strlen(h);
        return pal__concat3(h, h[n - 1u] == '/' ? "" : "/", NULL);
    }
    {
        struct passwd pw, *res = NULL;
        char buf[4096];
        if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &res) == 0 && res && res->pw_dir &&
            res->pw_dir[0] == '/') {
            size_t n = strlen(res->pw_dir);
            r = pal__concat3(res->pw_dir, res->pw_dir[n - 1u] == '/' ? "" : "/", NULL);
            return r;
        }
    }
    return NULL;
}

#if !defined(__APPLE__)
/* XDG base directory from env (absolute only, per the specification) or
 * home + fallback. Result has a trailing '/'. */
static char *xdg_base(const char *env, const char *home, const char *fallback)
{
    const char *v = SDL_getenv(env);
    if (v && v[0] == '/') {
        size_t n = strlen(v);
        return pal__concat3(v, v[n - 1u] == '/' ? "" : "/", NULL);
    }
    return pal__concat3(home, fallback, NULL);
}
#endif

bool pal__os_user_dirs(const char *lower, const char *display, char *out[4])
{
    char *home = pal__os_home();
    bool ok = true;
    if (!home) return false;
#if defined(__APPLE__)
    (void)lower;
    {
        char *support = pal__concat3(home, "Library/Application Support/", display);
        char *caches = pal__concat3(home, "Library/Caches/", display);
        out[0] = support ? pal__concat3(support, "/", NULL) : NULL;
        out[1] = support ? pal__concat3(support, "/", NULL) : NULL;
        out[2] = caches ? pal__concat3(caches, "/", NULL) : NULL;
        out[3] = support ? pal__concat3(support, "/State/", NULL) : NULL;
        free(support);
        free(caches);
    }
#else
    (void)display;
    {
        static const char *const env[4] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                            "XDG_CACHE_HOME", "XDG_STATE_HOME" };
        static const char *const fb[4] = { ".config/", ".local/share/", ".cache/",
                                           ".local/state/" };
        for (int i = 0; i < 4; i++) {
            char *b = xdg_base(env[i], home, fb[i]);
            out[i] = b ? pal__concat3(b, lower, "/") : NULL;
            free(b);
        }
    }
#endif
    for (int i = 0; i < 4; i++) ok = ok && out[i] != NULL;
    free(home);
    return ok;
}

static bool font_add(char ***v, size_t *n, size_t *cap, char *dir)
{
    if (!dir) return false;
    for (size_t i = 0; i < *n; i++) {
        if (strcmp((*v)[i], dir) == 0) {    /* duplicate */
            free(dir);
            return true;
        }
    }
    if (*n + 1u >= *cap) {
        size_t nc = *cap ? *cap * 2u : 16u;
        char **nv = (char **)realloc(*v, nc * sizeof *nv);
        if (!nv) {
            free(dir);
            return false;
        }
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = dir;
    (*v)[*n] = NULL;
    return true;
}

char **pal__os_font_dirs(void)
{
    char **v = NULL;
    size_t n = 0, cap = 0;
    char *home = pal__os_home();
#if defined(__APPLE__)
    font_add(&v, &n, &cap, pal__strdup("/System/Library/Fonts/"));
    font_add(&v, &n, &cap, pal__strdup("/System/Library/Fonts/Supplemental/"));
    font_add(&v, &n, &cap, pal__strdup("/Library/Fonts/"));
    if (home) font_add(&v, &n, &cap, pal__concat3(home, "Library/Fonts/", NULL));
#else
    font_add(&v, &n, &cap, pal__strdup("/usr/share/fonts/"));
    font_add(&v, &n, &cap, pal__strdup("/usr/local/share/fonts/"));
    if (home) {
        char *data = xdg_base("XDG_DATA_HOME", home, ".local/share/");
        if (data) font_add(&v, &n, &cap, pal__concat3(data, "fonts/", NULL));
        free(data);
        font_add(&v, &n, &cap, pal__concat3(home, ".fonts/", NULL));
    }
    {
        /* Host fonts that Flatpak exposes inside the sandbox. */
        font_add(&v, &n, &cap, pal__strdup("/run/host/fonts/"));
        font_add(&v, &n, &cap, pal__strdup("/run/host/local-fonts/"));
        font_add(&v, &n, &cap, pal__strdup("/run/host/user-fonts/"));
    }
    {
        /* XDG_DATA_DIRS adds Flatpak, Nix and Snap font locations. */
        const char *dirs = SDL_getenv("XDG_DATA_DIRS");
        while (dirs && *dirs) {
            const char *e = strchr(dirs, ':');
            size_t l = e ? (size_t)(e - dirs) : strlen(dirs);
            if (l > 0u && dirs[0] == '/' && l < 4096u) {
                char part[4096];
                memcpy(part, dirs, l);
                while (l > 1u && part[l - 1u] == '/') l--;
                part[l] = '\0';
                font_add(&v, &n, &cap, pal__concat3(part, "/fonts/", NULL));
            }
            dirs = e ? e + 1 : NULL;
        }
    }
#endif
    free(home);
    if (!v) {
        v = (char **)calloc(1u, sizeof *v);
    }
    return v;
}

/* ---- dynamic libraries ----------------------------------------------------------- */
pal_lib *pal_lib_open(const char *path)
{
    void *h;
    char *p;
    if (!path || !*path) return NULL;
    /* A bare name would make dlopen search LD_LIBRARY_PATH and the system
     * paths: always load the exact file (X-18). */
    p = strchr(path, '/') ? pal__strdup(path) : pal__concat3("./", path, NULL);
    if (!p) return NULL;
    h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!h) pal_log(PAL_LOG_WARN, "pal_lib_open(%s): %s", p, dlerror());
    free(p);
    return (pal_lib *)h;
}

void *pal_lib_sym(pal_lib *l, const char *name)
{
    if (!l || !name) return NULL;
    return dlsym((void *)l, name);
}

void pal_lib_close(pal_lib *l)
{
    if (l) (void)dlclose((void *)l);
}

const char *pal_lib_suffix(void)
{
#if defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

/* ---- reveal in the file manager -------------------------------------------------- */
/* Helper processes are reaped from pal_pump; when one fails, the parent
 * folder is opened instead. */
typedef struct reveal_proc {
    struct reveal_proc *next;
    SDL_Process        *proc;
    char               *fallback_url;
} reveal_proc;

static reveal_proc *g_reveal;            /* main thread */

static char *file_url(const char *abs)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(abs), k = 0;
    char *u;
    size_t cap;
    if (n > ((size_t)1 << 20)) return NULL;
    cap = 7u + 3u * n + 1u;
    u = (char *)malloc(cap);
    if (!u) return NULL;
    memcpy(u, "file://", 7u);
    k = 7u;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)abs[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '.' || c == '_' || c == '~') {
            u[k++] = (char)c;
        } else {
            u[k++] = '%';
            u[k++] = hex[c >> 4];
            u[k++] = hex[c & 15u];
        }
    }
    u[k] = '\0';
    return u;
}

static bool spawn_quiet(const char *const *args, SDL_Process **out)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Process *p;
    if (!props) return false;
    (void)SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    (void)SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                                SDL_PROCESS_STDIO_NULL);
    (void)SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_NULL);
    (void)SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                                SDL_PROCESS_STDIO_NULL);
    p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    *out = p;
    return p != NULL;
}

bool pal__os_reveal(const char *path)
{
    char *abs = pal__os_abspath(path);
    char *dir_url = NULL, *item_url = NULL, *arg = NULL;
    SDL_Process *proc = NULL;
    reveal_proc *rp;
    bool ok = false;
    if (!abs) return false;
    {
        char *slash = strrchr(abs, '/');
        if (slash) {
            char keep = slash[1];
            slash[1] = '\0';
            dir_url = file_url(abs);
            slash[1] = keep;
        }
    }
#if defined(__APPLE__)
    {
        const char *args[] = { "/usr/bin/open", "-R", abs, NULL };
        ok = spawn_quiet(args, &proc);
    }
#else
    item_url = file_url(abs);
    arg = item_url ? pal__concat3("array:string:", item_url, NULL) : NULL;
    if (arg) {
        const char *args[] = { "dbus-send", "--session", "--print-reply", "--reply-timeout=5000",
                               "--dest=org.freedesktop.FileManager1", "--type=method_call",
                               "/org/freedesktop/FileManager1",
                               "org.freedesktop.FileManager1.ShowItems", arg, "string:", NULL };
        ok = spawn_quiet(args, &proc);
    }
#endif
    if (ok) {
        rp = (reveal_proc *)calloc(1u, sizeof *rp);
        if (rp) {
            rp->proc = proc;
            rp->fallback_url = dir_url;
            dir_url = NULL;
            rp->next = g_reveal;
            g_reveal = rp;
        } else {
            SDL_DestroyProcess(proc);       /* still runs; just not reaped by us */
        }
    } else if (dir_url) {
        ok = SDL_OpenURL(dir_url);
    }
    free(arg);
    free(item_url);
    free(dir_url);
    free(abs);
    return ok;
}

/* Reap finished helpers. With forget, drop every helper without waiting
 * (they keep running detached). */
static void reveal_reap(bool forget)
{
    reveal_proc **pp = &g_reveal;
    while (*pp) {
        reveal_proc *rp = *pp;
        int code = 0;
        if (forget || SDL_WaitProcess(rp->proc, false, &code)) {
            if (!forget && code != 0 && rp->fallback_url) (void)SDL_OpenURL(rp->fallback_url);
            SDL_DestroyProcess(rp->proc);
            *pp = rp->next;
            free(rp->fallback_url);
            free(rp);
            continue;
        }
        pp = &rp->next;
    }
}

void pal__os_pump(void)
{
    if (g_reveal) reveal_reap(false);
}

/* ---- single instance ------------------------------------------------------------- */
/* Linux: abstract unix socket "<app_id>.si.<uid>" (vanishes with the
 * process, no files). Other POSIX systems, Flatpak (every sandbox has its
 * own network namespace, so abstract sockets do not reach the running
 * instance), or Linux with PAINTC_SI_FILE=1 (tests): socket file plus fcntl
 * lock file in $XDG_RUNTIME_DIR (its app/$FLATPAK_ID folder, the one part
 * shared between sandboxes of the app, under Flatpak) or PAL_DIR_STATE,
 * both private to the user. Peers of another uid are
 * rejected in both directions where the OS reports peer credentials.
 * macOS: not used; Finder delivers opens as SDL_EVENT_DROP_FILE. */
#if !defined(__APPLE__)
static struct {
    int          listen_fd;
    int          wake[2];
    int          lock_fd;
    SDL_Thread  *thread;
    char        *sock_path;              /* file variant: unlinked at stop */
} g_si = { -1, { -1, -1 }, -1, NULL, NULL };

static bool peer_is_same_user(int fd)
{
#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred cr;
    socklen_t l = (socklen_t)sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0) return false;
    return cr.uid == geteuid();
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) != 0) return false;
    return uid == geteuid();
#else
    (void)fd;
    return true;                         /* socket file in a private dir */
#endif
}

static void set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static int si_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) set_cloexec(fd);
    return fd;
}

static bool si_use_abstract(void)
{
#if defined(__linux__)
    const char *f = SDL_getenv("PAINTC_SI_FILE");
    const char *fp = SDL_getenv("FLATPAK_ID");
    return !(f && f[0] == '1') && !(fp && fp[0]);
#else
    return false;
#endif
}

/* Build the address. For the file variant also return the lock path. */
static bool si_address(const char *app_id, bool abstract, struct sockaddr_un *a, socklen_t *alen,
                       char **sock_path, char **lock_path)
{
    char name[128];
    size_t n;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    *sock_path = NULL;
    *lock_path = NULL;
    if (abstract) {
        (void)snprintf(name, sizeof name, "%s.si.%lu", app_id, (unsigned long)geteuid());
        n = strlen(name);
        if (n + 1u > sizeof a->sun_path) return false;
        memcpy(a->sun_path + 1, name, n);     /* sun_path[0] = 0: abstract */
        *alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1u + n);
        return true;
    } else {
        const char *rt = SDL_getenv("XDG_RUNTIME_DIR");
        const char *fp = SDL_getenv("FLATPAK_ID");
        const char *dir = (rt && rt[0] == '/' && pal_is_dir(rt)) ? rt : pal_dir(PAL_DIR_STATE);
        char *fdir = NULL;
        const char *sep;
        if (!dir) return false;
        if (dir == rt && fp && fp[0] && !strchr(fp, '/')) {
            fdir = pal__concat3(rt, rt[strlen(rt) - 1u] == '/' ? "app/" : "/app/", fp);
            if (fdir && pal_is_dir(fdir)) dir = fdir;
        }
        sep = dir[strlen(dir) - 1u] == '/' ? "" : "/";
        (void)snprintf(name, sizeof name, "%s.sock", app_id);
        *sock_path = pal__concat3(dir, sep, name);
        (void)snprintf(name, sizeof name, "%s.lock", app_id);
        *lock_path = pal__concat3(dir, sep, name);
        free(fdir);
        if (!*sock_path || !*lock_path || strlen(*sock_path) + 1u > sizeof a->sun_path) {
            free(*sock_path);
            free(*lock_path);
            *sock_path = *lock_path = NULL;
            return false;
        }
        memcpy(a->sun_path, *sock_path, strlen(*sock_path) + 1u);
        *alen = (socklen_t)sizeof *a;
        return true;
    }
}

/* Wait for fd readable or the wake pipe; false on timeout, error or wake. */
static bool si_wait_readable(int fd, int timeout_ms)
{
    for (;;) {
        struct pollfd pf[2];
        int r;
        pf[0].fd = fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = g_si.wake[0];
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        r = poll(pf, 2, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || pf[1].revents) return false;
        return (pf[0].revents & (POLLIN | POLLHUP)) != 0;
    }
}

static void si_serve(int c)
{
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        ssize_t r;
        if (!si_wait_readable(c, 2000)) goto drop;
        if (n == cap) {
            size_t nc = cap ? cap * 2u : 4096u;
            uint8_t *nb;
            if (nc > PAL__SI_MAX_MSG) nc = PAL__SI_MAX_MSG;
            if (nc == cap) goto drop;    /* larger than any valid message */
            nb = (uint8_t *)realloc(buf, nc);
            if (!nb) goto drop;
            buf = nb;
            cap = nc;
        }
        r = read(c, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            goto drop;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    pal__si_received(buf, n);
drop:
    free(buf);
}

static int SDLCALL si_thread(void *ud)
{
    (void)ud;
    for (;;) {
        int c;
        if (!si_wait_readable(g_si.listen_fd, -1)) break;
        c = accept(g_si.listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
            break;
        }
        set_cloexec(c);
        if (peer_is_same_user(c)) si_serve(c);
        close_quiet(c);
    }
    return 0;
}

static bool si_send(int fd, const uint8_t *msg, size_t len)
{
    struct timeval tv;
    int flags = 0;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof tv);
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;               /* no SIGPIPE if the primary vanished */
#endif
    while (len > 0u) {
        ssize_t w = send(fd, msg, len, flags);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        msg += (size_t)w;
        len -= (size_t)w;
    }
    (void)shutdown(fd, SHUT_WR);
    return true;
}

/* Connect to the primary, retrying for up to retry_ms while it starts. */
static int si_connect(const struct sockaddr_un *a, socklen_t alen, int retry_ms)
{
    uint64_t until = SDL_GetTicks() + (uint64_t)(retry_ms > 0 ? retry_ms : 0);
    for (;;) {
        int fd = si_socket();
        if (fd < 0) return -1;
        if (connect(fd, (const struct sockaddr *)a, alen) == 0) {
            if (peer_is_same_user(fd)) return fd;
            close_quiet(fd);
            return -1;                   /* squatter of another user */
        }
        close_quiet(fd);
        if ((errno != ECONNREFUSED && errno != ENOENT && errno != EAGAIN) ||
            SDL_GetTicks() >= until)
            return -1;
        SDL_Delay(20);
    }
}

static bool si_start_listener(int fd)
{
    if (listen(fd, 8) != 0) return false;
    if (pipe(g_si.wake) != 0) {
        g_si.wake[0] = g_si.wake[1] = -1;
        return false;
    }
    set_cloexec(g_si.wake[0]);
    set_cloexec(g_si.wake[1]);
    g_si.listen_fd = fd;
    g_si.thread = SDL_CreateThread(si_thread, "pal-single-instance", NULL);
    if (!g_si.thread) {
        close_quiet(g_si.wake[0]);
        close_quiet(g_si.wake[1]);
        g_si.wake[0] = g_si.wake[1] = -1;
        g_si.listen_fd = -1;
        return false;
    }
    return true;
}
#endif /* !__APPLE__ */

int pal__os_single_instance(const char *app_id, const uint8_t *msg, size_t len)
{
#if defined(__APPLE__)
    (void)app_id;
    (void)msg;
    (void)len;
    return PAL__SI_FAILED;
#else
    struct sockaddr_un a;
    socklen_t alen = 0;
    char *sock_path = NULL, *lock_path = NULL;
    bool abstract = si_use_abstract();
    int fd, rc = PAL__SI_FAILED;
    if (g_si.thread) return PAL__SI_PRIMARY;
    if (!si_address(app_id, abstract, &a, &alen, &sock_path, &lock_path)) return PAL__SI_FAILED;
    if (abstract) {
        fd = si_socket();
        if (fd < 0) goto out;
        if (bind(fd, (const struct sockaddr *)&a, alen) == 0) {
            if (si_start_listener(fd)) rc = PAL__SI_PRIMARY;
            else close_quiet(fd);
            goto out;
        }
        close_quiet(fd);
        if (errno != EADDRINUSE) goto out;
        fd = si_connect(&a, alen, 500);   /* the primary may not listen yet */
    } else {
        struct flock fl;
        int lk = open_retry(lock_path, O_RDWR | O_CREAT, 0600);
        if (lk < 0) goto out;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (fcntl(lk, F_SETLK, &fl) == 0) {
            /* We hold the lock: any socket file is stale. The lock file is
             * never unlinked, which would let two processes lock two
             * different files. */
            (void)unlink(sock_path);
            fd = si_socket();
            if (fd >= 0 && bind(fd, (const struct sockaddr *)&a, alen) == 0 &&
                si_start_listener(fd)) {
                g_si.lock_fd = lk;
                g_si.sock_path = sock_path;
                sock_path = NULL;
                rc = PAL__SI_PRIMARY;
                goto out;
            }
            close_quiet(fd);
            close_quiet(lk);
            goto out;
        }
        close_quiet(lk);
        fd = si_connect(&a, alen, 2000);
    }
    if (fd >= 0) {
        if (si_send(fd, msg, len)) rc = PAL__SI_FORWARDED;
        close_quiet(fd);
    }
out:
    free(sock_path);
    free(lock_path);
    return rc;
#endif
}

void pal__os_single_instance_stop(void)
{
#if !defined(__APPLE__)
    if (g_si.thread) {
        char b = 1;
        ssize_t w;
        do { w = write(g_si.wake[1], &b, 1u); } while (w < 0 && errno == EINTR);
        SDL_WaitThread(g_si.thread, NULL);
        g_si.thread = NULL;
    }
    close_quiet(g_si.listen_fd);
    close_quiet(g_si.wake[0]);
    close_quiet(g_si.wake[1]);
    g_si.listen_fd = g_si.wake[0] = g_si.wake[1] = -1;
    if (g_si.sock_path) {
        (void)unlink(g_si.sock_path);
        free(g_si.sock_path);
        g_si.sock_path = NULL;
    }
    close_quiet(g_si.lock_fd);           /* releases the fcntl lock */
    g_si.lock_fd = -1;
#endif
}

void pal__os_quit(void)
{
    reveal_reap(true);
}

#else  /* _WIN32 */
/* ISO C forbids an empty translation unit. */
typedef int pal_posix_unused;
#endif
