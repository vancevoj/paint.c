/* pal_crash.c - crash logs (lane SHELL, wave 3b; see include/pal/pal_crash.h).
 *
 * The handler runs in a broken process: it uses only async-signal-safe
 * calls on POSIX (open, write, close, time, getpid, raise, and glibc /
 * macOS backtrace_symbols_fd, which is preloaded at install so it does not
 * allocate later) and plain Win32 file calls on Windows. Numbers are
 * formatted by hand, no stdio. */
#if !defined(_WIN32)
#  if defined(__linux__) || defined(__GNU__) || defined(__CYGWIN__)
#    define _GNU_SOURCE 1     /* glibc hides POSIX in -std=c17: open it up */
#  endif
#endif

#include "pal/pal_crash.h"
#include "pal/pal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__GLIBC__) || defined(__APPLE__)
#    include <execinfo.h>
#    define PAL_HAVE_BACKTRACE 1
#  endif
#  ifndef O_CLOEXEC
#    define O_CLOEXEC 0
#  endif
#endif

#define DIR_MAX  900
#define INFO_MAX 1000

static char   g_dir[DIR_MAX + 1];
static char   g_info[INFO_MAX + 1];
static size_t g_info_len;
static bool   g_installed;

/* ---- formatting without stdio (handler side) ------------------------------------------ */
static size_t put_str(char *b, size_t at, size_t cap, const char *s)
{
    while (*s && at + 1u < cap) b[at++] = *s++;
    b[at] = '\0';
    return at;
}

#if !defined(_WIN32)
static size_t put_dec(char *b, size_t at, size_t cap, uint64_t v)
{
    char t[24];
    size_t n = 0;
    do {
        t[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v && n < sizeof t);
    while (n && at + 1u < cap) b[at++] = t[--n];
    b[at] = '\0';
    return at;
}

#endif

static size_t put_hex(char *b, size_t at, size_t cap, uint64_t v)
{
    static const char hx[] = "0123456789abcdef";
    char t[20];
    size_t n = 0;
    at = put_str(b, at, cap, "0x");
    do {
        t[n++] = hx[v & 15u];
        v >>= 4;
    } while (v && n < sizeof t);
    while (n && at + 1u < cap) b[at++] = t[--n];
    b[at] = '\0';
    return at;
}

/* ---- the shared part ------------------------------------------------------------------- */
static void set_strings(const char *dir, const char *info)
{
    size_t n = strlen(dir);
    if (n > DIR_MAX) n = DIR_MAX;
    memcpy(g_dir, dir, n);
    g_dir[n] = '\0';
    n = info ? strlen(info) : 0u;
    if (n > INFO_MAX) n = INFO_MAX;
    if (n) memcpy(g_info, info, n);
    g_info[n] = '\0';
    g_info_len = n;
}

int pal_crash_count(const char *dir)
{
    char **names = NULL;
    int n;
    if (!dir || !*dir) return 0;
    n = pal_list_dir(dir, "crash-*.txt", &names);
    if (n > 0) pal_free_names(names, n);
    return n > 0 ? n : 0;
}

int pal_crash_prune(const char *dir, int keep)
{
    char **names = NULL;
    int n, removed = 0;
    if (!dir || !*dir) return 0;
    if (keep < 0) keep = 0;
    n = pal_list_dir(dir, "crash-*.txt", &names);
    /* names hold the time first: name order is age order */
    for (int i = 0; i < n - keep; i++) {
        char path[2048];
        pal_path_join(path, sizeof path, dir, names[i]);
        if (pal_remove(path)) removed++;
    }
    if (n > 0) pal_free_names(names, n);
    return removed;
}

#if defined(_WIN32)
/* ---- Windows ---------------------------------------------------------------------------- */
static wchar_t g_wdir[DIR_MAX + 1];

static void wput(HANDLE h, const char *s, size_t n)
{
    DWORD w = 0;
    if (n) (void)WriteFile(h, s, (DWORD)n, &w, NULL);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    wchar_t path[DIR_MAX + 64];
    char line[256];
    size_t at;
    HANDLE h;
    {
        /* <dir>\crash-<unix time>-<process id>.txt */
        FILETIME ft;
        ULARGE_INTEGER u;
        wchar_t num[48];
        size_t k = 0, nd = 0;
        uint64_t secs, pid = (uint64_t)GetCurrentProcessId();
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        secs = (u.QuadPart / 10000000ull) - 11644473600ull;     /* unix time */
        while (g_wdir[k] && k < DIR_MAX) {
            path[k] = g_wdir[k];
            k++;
        }
        {
            static const wchar_t pre[] = L"\\crash-";
            for (size_t i = 0; pre[i]; i++) path[k++] = pre[i];
        }
        do {
            num[nd++] = (wchar_t)(L'0' + (int)(secs % 10u));
            secs /= 10u;
        } while (secs && nd < 20u);
        while (nd) path[k++] = num[--nd];
        path[k++] = L'-';
        do {
            num[nd++] = (wchar_t)(L'0' + (int)(pid % 10u));
            pid /= 10u;
        } while (pid && nd < 20u);
        while (nd) path[k++] = num[--nd];
        path[k++] = L'.';
        path[k++] = L't';
        path[k++] = L'x';
        path[k++] = L't';
        path[k] = 0;
    }
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    at = put_str(line, 0, sizeof line, "paint.c crashed\r\n");
    wput(h, line, at);
    wput(h, g_info, g_info_len);
    at = put_str(line, 0, sizeof line, "\r\nException: ");
    at = put_hex(line, at, sizeof line,
                 ep && ep->ExceptionRecord ? (uint64_t)ep->ExceptionRecord->ExceptionCode : 0u);
    at = put_str(line, at, sizeof line, " at ");
    at = put_hex(line, at, sizeof line,
                 ep && ep->ExceptionRecord
                     ? (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress
                     : 0u);
    at = put_str(line, at, sizeof line, "\r\nModule base: ");
    at = put_hex(line, at, sizeof line, (uint64_t)(uintptr_t)GetModuleHandleW(NULL));
    at = put_str(line, at, sizeof line, "\r\nStack:\r\n");
    wput(h, line, at);
    {
        void *frames[64];
        USHORT n = RtlCaptureStackBackTrace(0, 64, frames, NULL);
        for (USHORT i = 0; i < n; i++) {
            at = put_str(line, 0, sizeof line, "  ");
            at = put_hex(line, at, sizeof line, (uint64_t)(uintptr_t)frames[i]);
            at = put_str(line, at, sizeof line, "\r\n");
            wput(h, line, at);
        }
    }
    CloseHandle(h);
    return EXCEPTION_CONTINUE_SEARCH;
}

bool pal_crash_install(const char *dir, const char *info)
{
    if (!dir || !*dir) return false;
    set_strings(dir, info);
    if (MultiByteToWideChar(CP_UTF8, 0, g_dir, -1, g_wdir, DIR_MAX) <= 0) return false;
    if (!g_installed) {
        (void)SetUnhandledExceptionFilter(crash_filter);
        g_installed = true;
    }
    return true;
}

#else
/* ---- POSIX ------------------------------------------------------------------------------ */
static char g_altstack[65536];

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "signal";
    }
}

static void wr(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
    char path[DIR_MAX + 64], line[256];
    size_t at;
    int fd;
    (void)uc;
    at = put_str(path, 0, sizeof path, g_dir);
    at = put_str(path, at, sizeof path, "/crash-");
    at = put_dec(path, at, sizeof path, (uint64_t)time(NULL));
    at = put_str(path, at, sizeof path, "-");
    at = put_dec(path, at, sizeof path, (uint64_t)getpid());
    (void)put_str(path, at, sizeof path, ".txt");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
        at = put_str(line, 0, sizeof line, "paint.c crashed\n");
        wr(fd, line, at);
        wr(fd, g_info, g_info_len);
        at = put_str(line, 0, sizeof line, "\nSignal: ");
        at = put_str(line, at, sizeof line, sig_name(sig));
        at = put_str(line, at, sizeof line, " (");
        at = put_dec(line, at, sizeof line, (uint64_t)(sig < 0 ? 0 : sig));
        at = put_str(line, at, sizeof line, ")\nAddress: ");
        at = put_hex(line, at, sizeof line, si ? (uint64_t)(uintptr_t)si->si_addr : 0u);
        at = put_str(line, at, sizeof line, "\nStack:\n");
        wr(fd, line, at);
#if defined(PAL_HAVE_BACKTRACE)
        {
            void *frames[64];
            int n = backtrace(frames, 64);
            if (n > 0) backtrace_symbols_fd(frames, n, fd);
        }
#endif
        (void)close(fd);
    }
    /* SA_RESETHAND restored the default action: let the crash proceed */
    (void)raise(sig);
}

bool pal_crash_install(const char *dir, const char *info)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    struct sigaction sa;
    if (!dir || !*dir) return false;
    set_strings(dir, info);
    if (g_installed) return true;
#if defined(PAL_HAVE_BACKTRACE)
    {
        /* the first backtrace() may load the unwinder (allocates): do it now */
        void *frames[4];
        (void)backtrace(frames, 4);
    }
#endif
    {
        stack_t st;
        memset(&st, 0, sizeof st);
        st.ss_sp = g_altstack;
        st.ss_size = sizeof g_altstack;
        (void)sigaltstack(&st, NULL);       /* stack overflows still get a log */
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        if (sigaction(sigs[i], &sa, NULL) != 0) return false;
    g_installed = true;
    return true;
}
#endif
