/* pal_win32.c - OS hooks for Windows 10 and later (MSVC, clang-cl, MinGW).
 *
 * Every path crosses the boundary as UTF-8 and is converted to UTF-16 here.
 * File APIs get full paths with the \\?\ prefix once a path is long enough
 * to hit the legacy MAX_PATH limit, so deep folders work without the
 * process-wide long-path opt-in. Clipboard images are handled natively
 * (registered "PNG", CF_DIBV5, CF_DIB) so pasting interoperates with what
 * Windows applications put there, independent of the SDL version.
 */
#if defined(_WIN32)

#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00         /* Windows 10 (ADR-008) */
#endif
#ifndef WINVER
#  define WINVER 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "pal_internal.h"

#include <SDL3/SDL.h>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Paths at least this long get the \\?\ prefix (CreateDirectoryW's legacy
 * limit is MAX_PATH - 12). */
#define LONG_PATH_THRESHOLD 240u

/* ---- UTF-8 <-> UTF-16 ------------------------------------------------------------ */
static wchar_t *w_from_utf8(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

/* strict: fail on unpaired surrogates (file names), else replace them. */
static char *utf8_from_w_n(const wchar_t *w, int wlen, bool strict)
{
    DWORD flags = strict ? WC_ERR_INVALID_CHARS : 0u;
    int n;
    char *s;
    if (!w) return NULL;
    if (wlen == 0) return pal__strdup("");
    n = WideCharToMultiByte(CP_UTF8, flags, w, wlen, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n + 1u);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, flags, w, wlen, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    if (wlen < 0) return s;                 /* includes the terminator */
    s[n] = '\0';
    return s;
}

static char *utf8_from_w(const wchar_t *w)
{
    return utf8_from_w_n(w, -1, true);
}

static bool w_has_prefix(const wchar_t *w)
{
    return w[0] == L'\\' && w[1] == L'\\' && (w[2] == L'?' || w[2] == L'.') && w[3] == L'\\';
}

/* Full path of a UTF-8 path (GetFullPathNameW: '/' becomes '\\', "." and
 * ".." resolve against the current directory). long_ok adds \\?\ (or
 * \\?\UNC\) when the result is long. malloc'ed, NULL on failure. */
static wchar_t *w_full(const char *utf8, bool long_ok)
{
    wchar_t *w = w_from_utf8(utf8), *full;
    DWORD need, got;
    size_t off;
    if (!w) return NULL;
    if (!*w) {
        free(w);
        return NULL;
    }
    if (w_has_prefix(w)) return w;
    need = GetFullPathNameW(w, 0, NULL, NULL);
    if (need == 0u) {
        free(w);
        return NULL;
    }
    full = (wchar_t *)malloc(((size_t)need + 8u) * sizeof *full);
    if (!full) {
        free(w);
        return NULL;
    }
    off = 8u;                               /* room for \\?\UNC\ in front */
    got = GetFullPathNameW(w, need, full + off, NULL);
    free(w);
    if (got == 0u || got >= need) {
        free(full);
        return NULL;
    }
    if (long_ok && got >= LONG_PATH_THRESHOLD) {
        wchar_t *p = full + off;
        if (p[0] == L'\\' && p[1] == L'\\') {        /* \\server\share -> \\?\UNC\server\share */
            off = 8u - 6u;
            memcpy(full + off, L"\\\\?\\UNC", 7u * sizeof *full);
            /* full[off + 7] is p[1] == '\\', which becomes the separator */
            memmove(full + off + 7, p + 1, ((size_t)got) * sizeof *full);
        } else {
            off -= 4u;
            memcpy(full + off, L"\\\\?\\", 4u * sizeof *full);
        }
    }
    if (off) memmove(full, full + off, (wcslen(full + off) + 1u) * sizeof *full);
    return full;
}

static wchar_t *w_path(const char *utf8) { return w_full(utf8, true); }

/* Concatenate two wide strings into a new malloc'ed one. */
static wchar_t *w_cat(const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);
    wchar_t *r = (wchar_t *)malloc((la + lb + 1u) * sizeof *r);
    if (!r) return NULL;
    memcpy(r, a, la * sizeof *r);
    memcpy(r + la, b, (lb + 1u) * sizeof *r);
    return r;
}

static wchar_t *env_w(const wchar_t *name)
{
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    wchar_t *v;
    if (n == 0u) return NULL;
    v = (wchar_t *)malloc((size_t)n * sizeof *v);
    if (!v) return NULL;
    if (GetEnvironmentVariableW(name, v, n) != n - 1u || !v[0]) {
        free(v);
        return NULL;
    }
    return v;
}

/* UTF-8 folder from an environment variable or a CSIDL, with trailing '\'. */
static char *known_folder(const wchar_t *env, int csidl)
{
    wchar_t *v = env_w(env);
    char *u = NULL, *r;
    if (v) {
        u = utf8_from_w(v);
        free(v);
    }
    if (!u) {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_CREATE, NULL, 0, buf)))
            u = utf8_from_w(buf);
    }
    if (!u) return NULL;
    if (*u && !pal__is_sep(u[strlen(u) - 1u])) {
        r = pal__concat3(u, "\\", NULL);
        free(u);
        return r;
    }
    return u;
}

/* ---- lifecycle ------------------------------------------------------------------- */
bool pal__os_init(void)
{
    /* Drop the current directory from the DLL search order (X-18). */
    (void)SetDllDirectoryW(L"");
    return true;
}

/* ---- files ----------------------------------------------------------------------- */
pc_status pal_read_file(const char *path, uint64_t max_bytes, uint8_t **data, size_t *len)
{
    wchar_t *w;
    HANDLE h;
    LARGE_INTEGER sz = {0};
    size_t cap, alloc, n = 0;
    uint8_t *buf;
    bool disk;
    if (data) *data = NULL;
    if (len) *len = 0;
    if (!path || !data || !len) return PC_ERR_ARG;
    w = w_path(path);
    if (!w) return PC_ERR_IO;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return PC_ERR_IO;       /* also directories */
    cap = max_bytes >= (uint64_t)(SIZE_MAX - 1u) ? SIZE_MAX - 1u : (size_t)max_bytes;
    disk = GetFileType(h) == FILE_TYPE_DISK && GetFileSizeEx(h, &sz);
    if (disk && (uint64_t)sz.QuadPart > (uint64_t)cap) {
        CloseHandle(h);
        return PC_ERR_LIMIT;                /* before allocating (P-08) */
    }
    alloc = disk ? (size_t)sz.QuadPart : (size_t)65536u;
    if (alloc > cap) alloc = cap;
    buf = (uint8_t *)malloc(alloc + 1u);
    if (!buf) {
        CloseHandle(h);
        return PC_ERR_NOMEM;
    }
    for (;;) {
        DWORD got = 0, want;
        if (n == alloc) {
            uint8_t probe;
            size_t grow;
            uint8_t *nb;
            if (alloc >= cap) {
                if (!ReadFile(h, &probe, 1u, &got, NULL) && GetLastError() != ERROR_BROKEN_PIPE)
                    goto io_fail;
                if (got > 0u) {
                    free(buf);
                    CloseHandle(h);
                    return PC_ERR_LIMIT;
                }
                break;
            }
            grow = alloc < 65536u ? 65536u : alloc;
            if (grow > cap - alloc) grow = cap - alloc;
            nb = (uint8_t *)realloc(buf, alloc + grow + 1u);
            if (!nb) {
                free(buf);
                CloseHandle(h);
                return PC_ERR_NOMEM;
            }
            buf = nb;
            alloc += grow;
        }
        want = (alloc - n) > (size_t)(1u << 30) ? (DWORD)(1u << 30) : (DWORD)(alloc - n);
        if (!ReadFile(h, buf + n, want, &got, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            goto io_fail;
        }
        if (got == 0u) break;
        n += got;
    }
    CloseHandle(h);
    buf[n] = 0u;
    *data = buf;
    *len = n;
    return PC_OK;
io_fail:
    free(buf);
    CloseHandle(h);
    return PC_ERR_IO;
}

pc_status pal_write_file_atomic(const char *path, const void *data, size_t len)
{
    wchar_t *wt = NULL, *wtmp = NULL;
    char dir[4096];
    char shortbase[72];
    const char *base;
    size_t bl;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD attr;
    pc_status rc = PC_ERR_IO;
    if (!path || !*path || (!data && len)) return PC_ERR_ARG;
    base = pal_path_basename(path);
    if (!*base) return PC_ERR_ARG;
    wt = w_path(path);
    if (!wt) return PC_ERR_IO;
    attr = GetFileAttributesW(wt);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        free(wt);
        return PC_ERR_ARG;
    }
    pal_path_dirname(dir, sizeof dir, path);
    bl = strlen(base);
    if (bl > 64u) {
        bl = 64u;
        while (bl > 0u && ((unsigned char)base[bl] & 0xC0u) == 0x80u) bl--;
    }
    memcpy(shortbase, base, bl);
    shortbase[bl] = '\0';
    for (int attempt = 0; attempt < 16 && h == INVALID_HANDLE_VALUE; attempt++) {
        char name[128], tmp[4300];
        DWORD err;
        (void)snprintf(name, sizeof name, ".%s.%016llx.tmp", shortbase,
                       (unsigned long long)pal__rand64());
        pal_path_join(tmp, sizeof tmp, dir[0] ? dir : ".", name);
        free(wtmp);
        wtmp = w_path(tmp);
        if (!wtmp) goto done;
        /* The temp file inherits the folder's ACL, which is private to the
         * user inside the profile (X-23). FILE_ATTRIBUTE_TEMPORARY is not
         * used: the attribute would survive the rename. */
        h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0u;
        if (h == INVALID_HANDLE_VALUE && err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            goto done;
    }
    if (h == INVALID_HANDLE_VALUE) goto done;
    {
        const uint8_t *p = (const uint8_t *)data;
        size_t left = len;
        while (left > 0u) {
            DWORD chunk = left > (size_t)(1u << 30) ? (DWORD)(1u << 30) : (DWORD)left, wr = 0;
            if (!WriteFile(h, p, chunk, &wr, NULL) || wr == 0u) goto fail_delete;
            p += wr;
            left -= wr;
        }
    }
    if (!FlushFileBuffers(h)) goto fail_delete;
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
    /* ReplaceFileW keeps the identity, attributes and ACL of an existing
     * file; MoveFileExW covers new files and file systems where
     * ReplaceFileW fails. Virus scanners and indexers often hold a fresh
     * file for a moment, so sharing errors are retried briefly. */
    for (int attempt = 0; attempt < 5; attempt++) {
        DWORD err;
        if (attempt > 0) Sleep(10u << attempt);            /* 20 .. 160 ms */
        if (attr != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(wt) != INVALID_FILE_ATTRIBUTES &&
            ReplaceFileW(wt, wtmp, NULL,
                         REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                         NULL, NULL)) {
            rc = PC_OK;
            goto done;
        }
        if (MoveFileExW(wtmp, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            rc = PC_OK;
            goto done;
        }
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED &&
            err != ERROR_LOCK_VIOLATION)
            break;
    }
    if (attr != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(wt) == INVALID_FILE_ATTRIBUTES) {
        /* ReplaceFileW removed the original but the rename failed: the
         * temp file now holds the only copy of the data, keep it. */
        pal_log(PAL_LOG_ERROR, "pal_write_file_atomic: %s is gone; the data is kept in "
                "a temporary file next to it (error %lu)", path, (unsigned long)GetLastError());
        goto done;
    }
fail_delete:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
    (void)DeleteFileW(wtmp);
done:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(wtmp);
    free(wt);
    return rc;
}

static DWORD attrs_of(const char *path)
{
    wchar_t *w;
    DWORD a;
    if (!path || !*path) return INVALID_FILE_ATTRIBUTES;
    w = w_path(path);
    if (!w) return INVALID_FILE_ATTRIBUTES;
    a = GetFileAttributesW(w);
    free(w);
    return a;
}

bool pal_file_exists(const char *path)
{
    return attrs_of(path) != INVALID_FILE_ATTRIBUTES;
}

bool pal_is_dir(const char *path)
{
    DWORD a = attrs_of(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0u;
}

bool pal_remove(const char *path)
{
    wchar_t *w;
    DWORD a;
    bool ok;
    if (!path || !*path) return false;
    w = w_path(path);
    if (!w) return false;
    a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) {
        free(w);
        return false;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        ok = RemoveDirectoryW(w) != 0;
    } else {
        ok = DeleteFileW(w) != 0;
        if (!ok && (a & FILE_ATTRIBUTE_READONLY)) {    /* like unlink on POSIX */
            (void)SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            ok = DeleteFileW(w) != 0;
        }
    }
    free(w);
    return ok;
}

bool pal_mkdirs(const char *path)
{
    return pal__os_mkdirs(path, false);
}

uint64_t pal_file_mtime(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t *w;
    uint64_t t;
    BOOL ok;
    if (!path || !*path) return 0u;
    w = w_path(path);
    if (!w) return 0u;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
    free(w);
    if (!ok) return 0u;
    t = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    if (t < 116444736000000000ull) return 0u;           /* before 1970 */
    return (t - 116444736000000000ull) / 10000000u;
}

/* Index just past the root of a full wide path: "C:\", "\\?\C:\",
 * "\\server\share\", "\\?\UNC\server\share\". */
static size_t w_root_end(const wchar_t *w)
{
    size_t i = 0;
    int parts = 0;
    if (w_has_prefix(w)) {
        i = 4u;
        if ((w[4] == L'U' || w[4] == L'u') && wcsncmp(w + 5, L"NC\\", 3u) == 0) {
            i = 8u;
            parts = 0;
            goto unc;
        }
    } else if (w[0] == L'\\' && w[1] == L'\\') {
        i = 2u;
        goto unc;
    }
    if (w[i] && w[i + 1] == L':') return w[i + 2] == L'\\' ? i + 3u : i + 2u;
    return i;
unc:
    while (w[i] && parts < 2) {
        while (w[i] && w[i] != L'\\') i++;
        parts++;
        if (w[i]) i++;
    }
    return i;
}

bool pal__os_mkdirs(const char *path, bool private_mode)
{
    wchar_t *w;
    size_t n, root;
    DWORD a;
    bool ok;
    (void)private_mode;                     /* profile ACLs are per user already */
    if (!path || !*path) return false;
    w = w_path(path);
    if (!w) return false;
    n = wcslen(w);
    while (n > 0u && w[n - 1u] == L'\\') w[--n] = L'\0';
    root = w_root_end(w);
    for (size_t i = root; i <= n; i++) {
        if (i == n || w[i] == L'\\') {
            wchar_t c = w[i];
            if (i > root) {
                w[i] = L'\0';
                (void)CreateDirectoryW(w, NULL);
                w[i] = c;
            }
        }
    }
    a = GetFileAttributesW(w);
    ok = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    free(w);
    return ok;
}

bool pal__os_list_dir(const char *dir, bool (*add)(void *ctx, const char *name), void *ctx)
{
    wchar_t *w = w_path(dir), *pat;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t n;
    if (!w) return false;
    n = wcslen(w);
    pat = w_cat(w, (n > 0u && w[n - 1u] == L'\\') ? L"*" : L"\\*");
    free(w);
    if (!pat) return false;
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                         FIND_FIRST_EX_LARGE_FETCH);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char *name;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        name = utf8_from_w(fd.cFileName);
        if (!name) continue;                /* not representable in UTF-8: skip */
        if (!add(ctx, name)) {
            free(name);
            FindClose(h);
            return false;
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

char *pal__os_abspath(const char *path)
{
    wchar_t *w = w_full(path, false);
    char *r;
    if (!w) return NULL;
    r = utf8_from_w(w);
    free(w);
    return r;
}

/* ---- machine --------------------------------------------------------------------- */
uint32_t pal__os_cpu_limit(uint32_t n)
{
    DWORD_PTR proc = 0, sys = 0;
    uint32_t c = 0;
    if (n > 64u || !GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return n;
    for (; proc; proc &= proc - 1u) c++;
    return (c > 0u && c < n) ? c : n;
}

void pal__os_log_native(const char *line)
{
    wchar_t *w = w_from_utf8(line);
    if (w) {
        OutputDebugStringW(w);
        free(w);
    }
}

void *pal__os_log_open(const char *path)
{
    wchar_t *w = w_path(path);
    HANDLE h;
    if (!w) return NULL;
    /* Shared, so the log can be opened in an editor while the app runs. */
    h = CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return h == INVALID_HANDLE_VALUE ? NULL : (void *)h;
}

void pal__os_log_write(void *h, const char *data, size_t n)
{
    DWORD wr = 0;
    if (h && n <= 0xFFFFFFFFu) (void)WriteFile((HANDLE)h, data, (DWORD)n, &wr, NULL);
}

void pal__os_log_close(void *h)
{
    if (h) CloseHandle((HANDLE)h);
}

/* ---- directories ----------------------------------------------------------------- */
char *pal__os_home(void)
{
    return known_folder(L"USERPROFILE", CSIDL_PROFILE);
}

bool pal__os_user_dirs(const char *lower, const char *display, char *out[4])
{
    char *roaming = known_folder(L"APPDATA", CSIDL_APPDATA);
    char *local = known_folder(L"LOCALAPPDATA", CSIDL_LOCAL_APPDATA);
    bool ok;
    (void)display;
    if (roaming) {
        out[0] = pal__concat3(roaming, lower, "\\");
        out[1] = pal__strdup(out[0]);
    }
    if (local) {
        char *app = pal__concat3(local, lower, "\\");
        out[2] = app ? pal__concat3(app, "Cache\\", NULL) : NULL;
        out[3] = app ? pal__concat3(app, "State\\", NULL) : NULL;
        free(app);
    }
    ok = out[0] && out[1] && out[2] && out[3];
    free(roaming);
    free(local);
    return ok;
}

char **pal__os_font_dirs(void)
{
    char **v = (char **)calloc(3u, sizeof *v);
    wchar_t win[MAX_PATH];
    UINT n;
    size_t k = 0;
    char *local;
    if (!v) return NULL;
    n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n > 0u && n < MAX_PATH) {
        char *u = utf8_from_w(win);
        if (u) {
            v[k] = pal__concat3(u, pal__is_sep(u[strlen(u) - 1u]) ? "" : "\\", "Fonts\\");
            if (v[k]) k++;
            free(u);
        }
    }
    local = known_folder(L"LOCALAPPDATA", CSIDL_LOCAL_APPDATA);
    if (local) {
        v[k] = pal__concat3(local, "Microsoft\\Windows\\Fonts\\", NULL);
        if (v[k]) k++;
        free(local);
    }
    v[k] = NULL;
    return v;
}

/* ---- dynamic libraries ----------------------------------------------------------- */
pal_lib *pal_lib_open(const char *path)
{
    /* Fully qualified path (required by LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR),
     * dependencies only from the plugin's folder and System32 (X-18). */
    wchar_t *w = w_full(path, true);
    HMODULE m;
    DWORD old = 0;
    if (!w) return NULL;
    (void)SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old);
    m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    (void)SetThreadErrorMode(old, NULL);
    if (!m)
        pal_log(PAL_LOG_WARN, "pal_lib_open(%s): error %lu", path, (unsigned long)GetLastError());
    free(w);
    return (pal_lib *)(void *)m;
}

void *pal_lib_sym(pal_lib *l, const char *name)
{
    FARPROC f;
    void *p = NULL;
    if (!l || !name) return NULL;
    f = GetProcAddress((HMODULE)(void *)l, name);
    /* ISO C has no function <-> object pointer cast; copy the bits. */
    _Static_assert(sizeof f == sizeof p, "function and data pointers differ in size");
    if (f) memcpy(&p, &f, sizeof p);
    return p;
}

void pal_lib_close(pal_lib *l)
{
    if (l) (void)FreeLibrary((HMODULE)(void *)l);
}

const char *pal_lib_suffix(void) { return ".dll"; }

/* ---- shell ----------------------------------------------------------------------- */
bool pal__os_reveal(const char *path)
{
    wchar_t *w = w_full(path, false);
    HRESULT co;
    bool ok = false;
    if (!w) return false;
    co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w);
        if (pidl) {
            ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, NULL, 0));
            ILFree(pidl);
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) {
        wchar_t *args = w_cat(L"/select,\"", w), *args2 = args ? w_cat(args, L"\"") : NULL;
        if (args2)
            ok = (INT_PTR)ShellExecuteW(NULL, L"open", L"explorer.exe", args2, NULL,
                                        SW_SHOWNORMAL) > 32;
        free(args2);
        free(args);
    }
    free(w);
    return ok;
}

void pal__os_pump(void) {}

/* ---- clipboard ------------------------------------------------------------------- */
static HWND g_clip_hwnd;
static ATOM g_clip_class;
static const wchar_t k_clip_class[] = L"paintc.pal.clipboard";

static UINT fmt_png(void)
{
    static UINT f;
    if (!f) f = RegisterClipboardFormatW(L"PNG");
    return f;
}

/* Hidden message-only window that owns what we put on the clipboard
 * (OpenClipboard(NULL) would make SetClipboardData fail). Created on the
 * main thread, so SDL's message pump also services it. */
static bool clip_open(void)
{
    if (!g_clip_hwnd) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = k_clip_class;
        if (!g_clip_class) g_clip_class = RegisterClassExW(&wc);
        if (!g_clip_class) return false;
        g_clip_hwnd = CreateWindowExW(0, k_clip_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                      GetModuleHandleW(NULL), NULL);
        if (!g_clip_hwnd) return false;
    }
    for (int i = 0; i < 10; i++) {          /* another app may hold it briefly */
        if (OpenClipboard(g_clip_hwnd)) return true;
        Sleep(5);
    }
    pal__log_str(PAL_LOG_WARN, "clipboard: OpenClipboard failed");
    return false;
}

static bool clip_put(UINT fmt, const void *data, size_t n)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1u);
    void *p;
    if (!g) return false;
    p = GlobalLock(g);
    if (!p) {
        GlobalFree(g);
        return false;
    }
    if (n) memcpy(p, data, n);
    GlobalUnlock(g);
    if (!SetClipboardData(fmt, g)) {        /* on success the system owns g */
        GlobalFree(g);
        return false;
    }
    return true;
}

bool pal_clip_has_image(void)
{
    return IsClipboardFormatAvailable(fmt_png()) || IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB);
}

bool pal_clip_get_image(uint8_t **data, size_t *len, char *mime, size_t mime_cap)
{
    static const UINT dib_fmts[2] = { CF_DIBV5, CF_DIB };
    bool ok = false;
    HANDLE hm;
    if (data) *data = NULL;
    if (len) *len = 0;
    if (mime && mime_cap) mime[0] = '\0';
    if (!data || !len || !pal_clip_has_image() || !clip_open()) return false;
    hm = GetClipboardData(fmt_png());
    if (hm) {
        SIZE_T n = GlobalSize(hm);
        const void *p = GlobalLock(hm);
        if (p && n > 0u && n <= ((SIZE_T)1 << 31)) {
            *data = (uint8_t *)malloc((size_t)n + 1u);
            if (*data) {
                memcpy(*data, p, (size_t)n);
                (*data)[n] = 0u;
                *len = (size_t)n;
                if (mime && mime_cap) SDL_strlcpy(mime, "image/png", mime_cap);
                ok = true;
            }
        }
        if (p) GlobalUnlock(hm);
    }
    for (int i = 0; i < 2 && !ok; i++) {
        hm = GetClipboardData(dib_fmts[i]);
        if (hm) {
            SIZE_T n = GlobalSize(hm);
            const uint8_t *p = (const uint8_t *)GlobalLock(hm);
            if (p && n <= ((SIZE_T)1 << 31)) {
                *data = pal__dib_to_bmp(p, (size_t)n, len);     /* validates */
                if (*data) {
                    if (mime && mime_cap) SDL_strlcpy(mime, "image/bmp", mime_cap);
                    ok = true;
                }
            }
            if (p) GlobalUnlock(hm);
        }
    }
    CloseClipboard();
    return ok;
}

bool pal_clip_set_image_png(const uint8_t *png, size_t len)
{
    bool ok;
    if (!png || len == 0u || !clip_open()) return false;
    ok = EmptyClipboard() && clip_put(fmt_png(), png, len);
    CloseClipboard();
    return ok;
}

bool pal_clip_set_image_bgra(const uint8_t *png, size_t len, const uint8_t *bgra,
                             int32_t w, int32_t h, size_t stride)
{
    size_t dib_n = 0;
    uint8_t *dib = pal__enc_bmp(bgra, w, h, stride, false, &dib_n);
    bool ok;
    if (!dib) return false;
    if (!clip_open()) {
        free(dib);
        return false;
    }
    /* PNG first: applications take the first format they understand. */
    ok = EmptyClipboard() != 0;
    if (ok && png && len) ok = clip_put(fmt_png(), png, len);
    if (ok) ok = clip_put(CF_DIBV5, dib, dib_n);   /* Windows synthesizes CF_DIB */
    CloseClipboard();
    free(dib);
    return ok;
}

bool pal_clip_set_text(const char *utf8)
{
    wchar_t *w, *crlf;
    size_t n, extra = 0, k = 0;
    bool ok;
    if (!utf8) return false;
    w = w_from_utf8(utf8);
    if (!w) return false;
    n = wcslen(w);
    for (size_t i = 0; i < n; i++)
        if (w[i] == L'\n' && (i == 0u || w[i - 1u] != L'\r')) extra++;
    crlf = (wchar_t *)malloc((n + extra + 1u) * sizeof *crlf);
    if (!crlf) {
        free(w);
        return false;
    }
    for (size_t i = 0; i < n; i++) {        /* Windows text uses CRLF */
        if (w[i] == L'\n' && (i == 0u || w[i - 1u] != L'\r')) crlf[k++] = L'\r';
        crlf[k++] = w[i];
    }
    crlf[k] = L'\0';
    free(w);
    if (!clip_open()) {
        free(crlf);
        return false;
    }
    ok = EmptyClipboard() && clip_put(CF_UNICODETEXT, crlf, (k + 1u) * sizeof *crlf);
    CloseClipboard();
    free(crlf);
    return ok;
}

char *pal_clip_get_text(void)
{
    HANDLE hm;
    char *r = NULL;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !clip_open()) return NULL;
    hm = GetClipboardData(CF_UNICODETEXT);
    if (hm) {
        SIZE_T bytes = GlobalSize(hm);
        const wchar_t *p = (const wchar_t *)GlobalLock(hm);
        if (p) {
            size_t max = (size_t)(bytes / sizeof(wchar_t)), n = 0, k = 0;
            wchar_t *lf;
            while (n < max && p[n]) n++;    /* bounded: the NUL may be missing */
            lf = (wchar_t *)malloc((n + 1u) * sizeof *lf);
            if (lf) {
                for (size_t i = 0; i < n; i++)
                    if (!(p[i] == L'\r' && i + 1u < n && p[i + 1u] == L'\n')) lf[k++] = p[i];
                lf[k] = L'\0';
                r = utf8_from_w_n(lf, (int)k, false);
                free(lf);
            }
            GlobalUnlock(hm);
        }
    }
    CloseClipboard();
    return r;
}

/* ---- single instance ------------------------------------------------------------- */
/* Named mutex "Local\<app_id>.si" decides who is first (per session); the
 * first instance serves a named pipe "\\.\pipe\<app_id>.si.<session>" on a
 * listener thread. The default pipe DACL gives other users read access
 * only, so they cannot write to an inbound pipe; remote clients are
 * rejected and FILE_FLAG_FIRST_PIPE_INSTANCE refuses a squatted name. */
static struct {
    HANDLE      mutex;
    HANDLE      stop;
    HANDLE      pipe;               /* instance waiting for the next client */
    SDL_Thread *thread;
    wchar_t     name[200];
} g_si;

static HANDLE si_create_pipe(void)
{
    return CreateNamedPipeW(g_si.name,
                            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                                FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                PIPE_REJECT_REMOTE_CLIENTS,
                            1, 0, 65536, 0, NULL);
}

/* Wait for an overlapped operation or the stop event. */
static bool si_wait(HANDLE pipe, OVERLAPPED *ov, DWORD timeout, DWORD *got)
{
    HANDLE hs[2];
    DWORD r;
    hs[0] = ov->hEvent;
    hs[1] = g_si.stop;
    r = WaitForMultipleObjects(2, hs, FALSE, timeout);
    if (r != WAIT_OBJECT_0) {
        CancelIo(pipe);
        (void)GetOverlappedResult(pipe, ov, got, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, ov, got, FALSE) != 0;
}

static void si_serve(HANDLE pipe, HANDLE ev)
{
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        OVERLAPPED ov;
        DWORD got = 0;
        BOOL r;
        if (n == cap) {
            size_t nc = cap ? cap * 2u : 4096u;
            uint8_t *nb;
            if (nc > PAL__SI_MAX_MSG) nc = PAL__SI_MAX_MSG;
            if (nc == cap) goto drop;
            nb = (uint8_t *)realloc(buf, nc);
            if (!nb) goto drop;
            buf = nb;
            cap = nc;
        }
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        r = ReadFile(pipe, buf + n, (DWORD)(cap - n), &got, &ov);
        if (!r) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE) break;               /* client done */
            if (e != ERROR_IO_PENDING) goto drop;
            if (!si_wait(pipe, &ov, 2000u, &got)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) break;
                goto drop;
            }
        }
        if (got == 0u) break;               /* end of stream */
        n += got;
    }
    pal__si_received(buf, n);
drop:
    free(buf);
}

static int SDLCALL si_thread(void *ud)
{
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    (void)ud;
    if (!ev) return 0;
    while (g_si.pipe != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov;
        DWORD got = 0;
        bool connected;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        if (ConnectNamedPipe(g_si.pipe, &ov)) {
            connected = true;
        } else {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) connected = si_wait(g_si.pipe, &ov, INFINITE, &got);
            else connected = false;
        }
        if (WaitForSingleObject(g_si.stop, 0) == WAIT_OBJECT_0) break;
        if (connected) si_serve(g_si.pipe, ev);
        (void)DisconnectNamedPipe(g_si.pipe);
        CloseHandle(g_si.pipe);
        g_si.pipe = si_create_pipe();       /* INVALID ends the loop */
    }
    CloseHandle(ev);
    return 0;
}

static bool si_forward(const uint8_t *msg, size_t len)
{
    uint64_t until = SDL_GetTicks() + 3000u;
    for (;;) {
        HANDLE h = CreateFileW(g_si.name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov;
            DWORD wr = 0;
            bool ok = false;
            memset(&ov, 0, sizeof ov);
            ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (ov.hEvent) {
                if (WriteFile(h, msg, (DWORD)len, &wr, &ov)) {
                    ok = wr == (DWORD)len;
                } else if (GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(ov.hEvent, 3000u) == WAIT_OBJECT_0)
                        ok = GetOverlappedResult(h, &ov, &wr, FALSE) && wr == (DWORD)len;
                    else
                        CancelIo(h);
                }
                if (!ok) (void)GetOverlappedResult(h, &ov, &wr, TRUE);
                CloseHandle(ov.hEvent);
            }
            CloseHandle(h);
            return ok;
        }
        if (SDL_GetTicks() >= until) return false;
        if (GetLastError() == ERROR_PIPE_BUSY) (void)WaitNamedPipeW(g_si.name, 500u);
        else Sleep(20);                     /* primary still starting */
    }
}

int pal__os_single_instance(const char *app_id, const uint8_t *msg, size_t len)
{
    wchar_t *wid = w_from_utf8(app_id);
    wchar_t mname[200];
    DWORD session = 0;
    HANDLE m;
    if (g_si.thread) {
        free(wid);
        return PAL__SI_PRIMARY;
    }
    if (!wid || wcslen(wid) > 100u) {
        free(wid);
        return PAL__SI_FAILED;
    }
    (void)ProcessIdToSessionId(GetCurrentProcessId(), &session);
    (void)_snwprintf(mname, sizeof mname / sizeof mname[0], L"Local\\%ls.si", wid);
    mname[sizeof mname / sizeof mname[0] - 1u] = L'\0';
    (void)_snwprintf(g_si.name, sizeof g_si.name / sizeof g_si.name[0],
                     L"\\\\.\\pipe\\%ls.si.%lu", wid, (unsigned long)session);
    g_si.name[sizeof g_si.name / sizeof g_si.name[0] - 1u] = L'\0';
    free(wid);
    m = CreateMutexW(NULL, FALSE, mname);
    if (!m) return PAL__SI_FAILED;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return si_forward(msg, len) ? PAL__SI_FORWARDED : PAL__SI_FAILED;
    }
    g_si.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_si.pipe = g_si.stop ? si_create_pipe() : INVALID_HANDLE_VALUE;
    if (g_si.pipe != INVALID_HANDLE_VALUE)
        g_si.thread = SDL_CreateThread(si_thread, "pal-single-instance", NULL);
    if (!g_si.thread) {
        if (g_si.pipe != INVALID_HANDLE_VALUE) CloseHandle(g_si.pipe);
        if (g_si.stop) CloseHandle(g_si.stop);
        g_si.pipe = INVALID_HANDLE_VALUE;
        g_si.stop = NULL;
        CloseHandle(m);
        return PAL__SI_FAILED;
    }
    g_si.mutex = m;                         /* held for the process lifetime */
    return PAL__SI_PRIMARY;
}

void pal__os_single_instance_stop(void)
{
    if (g_si.thread) {
        SetEvent(g_si.stop);
        SDL_WaitThread(g_si.thread, NULL);
        g_si.thread = NULL;
    }
    if (g_si.pipe && g_si.pipe != INVALID_HANDLE_VALUE) CloseHandle(g_si.pipe);
    g_si.pipe = NULL;
    if (g_si.stop) CloseHandle(g_si.stop);
    g_si.stop = NULL;
    if (g_si.mutex) CloseHandle(g_si.mutex);
    g_si.mutex = NULL;
}

void pal__os_quit(void)
{
    if (g_clip_hwnd) DestroyWindow(g_clip_hwnd);
    g_clip_hwnd = NULL;
    if (g_clip_class) UnregisterClassW(k_clip_class, GetModuleHandleW(NULL));
    g_clip_class = 0;
}

#else  /* !_WIN32 */
/* ISO C forbids an empty translation unit. */
typedef int pal_win32_unused;
#endif
