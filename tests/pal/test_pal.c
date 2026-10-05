/* test_pal.c - platform layer tests. Headless: SDL_Init(0), no window, no
 * video, no dialogs (a real dialog would pop up on the desktop). Every file
 * lives under a private temporary root next to the test executable, and
 * HOME / XDG_* / APPDATA / LOCALAPPDATA point there, so the user's real
 * directories are never touched. The root is removed at the end.
 *
 * The executable doubles as the second instance for the single-instance
 * test: "test_pal --si-child <app_id> <variant> <paths...>".
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#  define _POSIX_C_SOURCE 200809L    /* symlink, umask under -std=c17 */
#endif

#include "pc_test.h"

#include "pal/pal.h"
#include "pal/pal_clip_raw.h"
#include "pal_internal.h"

#include <SDL3/SDL.h>
/* SDL's entry point gives main() UTF-8 arguments on Windows, like the app
 * gets them (the C runtime's argv is in the ANSI code page there). MinGW
 * links it with -municode through pc_sdl_main() in CMakeLists.txt. */
#include <SDL3/SDL_main.h>

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#if defined(_WIN32)
#  define LIB_SUFFIX ".dll"
#elif defined(__APPLE__)
#  define LIB_SUFFIX ".dylib"
#else
#  define LIB_SUFFIX ".so"
#endif

static char g_root[1024];        /* temp root with trailing separator */

/* ---- helpers ----------------------------------------------------------------- */
static void P(char *out, size_t cap, const char *rel)
{
    pal_path_join(out, cap, g_root, rel);
}

static bool write_str(const char *path, const char *s)
{
    return pal_write_file_atomic(path, s, strlen(s)) == PC_OK;
}

static bool file_equals(const char *path, const void *data, size_t n)
{
    uint8_t *d = NULL;
    size_t len = 0;
    bool eq;
    if (pal_read_file(path, (uint64_t)n + 16u, &d, &len) != PC_OK) return false;
    eq = len == n && (n == 0u || memcmp(d, data, n) == 0) && d[len] == 0u;
    free(d);
    return eq;
}

static bool ends_with_sep(const char *s)
{
    size_t n = s ? strlen(s) : 0u;
    return n > 0u && (s[n - 1u] == '/' || s[n - 1u] == '\\');
}

static bool is_abs(const char *s)
{
    if (!s || !*s) return false;
#if defined(_WIN32)
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':') return true;
    if (s[0] == '\\' && s[1] == '\\') return true;
#endif
    return s[0] == '/';
}

/* Iterative recursive delete (P-07): collect every directory in DFS order,
 * delete files on the way, then remove the directories deepest first. */
static void rm_tree(const char *root)
{
    char **dirs = NULL;
    size_t n = 0, cap = 0, i = 0;
    if (!pal_is_dir(root)) {
        (void)pal_remove(root);
        return;
    }
    dirs = (char **)malloc(sizeof *dirs * 16u);
    if (!dirs) return;
    cap = 16u;
    dirs[n++] = pal__strdup(root);
    while (i < n) {
        char **names = NULL;
        int k = pal_list_dir(dirs[i], NULL, &names);
        for (int j = 0; j < k; j++) {
            char path[4096];
            pal_path_join(path, sizeof path, dirs[i], names[j]);
            if (pal_is_dir(path)) {
                if (n == cap) {
                    char **nd = (char **)realloc(dirs, sizeof *dirs * cap * 2u);
                    if (!nd) break;
                    dirs = nd;
                    cap *= 2u;
                }
                dirs[n++] = pal__strdup(path);
            } else {
                (void)pal_remove(path);
            }
        }
        pal_free_names(names, k);
        i++;
    }
    while (n > 0u) {
        n--;
        if (dirs[n]) (void)pal_remove(dirs[n]);
        free(dirs[n]);
    }
    free(dirs);
}

/* ---- path helpers ---------------------------------------------------------------- */
static void t_path_helpers(void)
{
    char out[64];
    char tiny[8];
    /* join */
    pal_path_join(out, sizeof out, "a/b", "c.txt");
    CHECK(strcmp(out, "a/b/c.txt") == 0);
    pal_path_join(out, sizeof out, "a\\b", "c.txt");
    CHECK(strcmp(out, "a\\b\\c.txt") == 0);
    pal_path_join(out, sizeof out, "a/b/", "/c");
    CHECK(strcmp(out, "/c") == 0);                 /* absolute b wins */
    pal_path_join(out, sizeof out, "a/b/", "c");
    CHECK(strcmp(out, "a/b/c") == 0);
    pal_path_join(out, sizeof out, "", "c");
    CHECK(strcmp(out, "c") == 0);
    pal_path_join(out, sizeof out, "a", "");
    CHECK(strcmp(out, "a") == 0);
    pal_path_join(out, sizeof out, "a", NULL);
    CHECK(strcmp(out, "a") == 0);
    pal_path_join(out, sizeof out, "dir", "sub/x");
    CHECK(strcmp(out, "dir/sub/x") == 0 || strcmp(out, "dir\\sub/x") == 0);
    strcpy(out, "base");
    pal_path_join(out, sizeof out, out, "leaf");   /* out aliases a */
    CHECK(strcmp(out, "base/leaf") == 0 || strcmp(out, "base\\leaf") == 0);
    /* truncation never splits a UTF-8 sequence: "ab/" + U+00E9 U+00E9 */
    pal_path_join(tiny, sizeof tiny, "ab", "\xC3\xA9\xC3\xA9");    /* 7 bytes fit in 8 */
    CHECK(strlen(tiny) == 7u && strcmp(tiny + 3, "\xC3\xA9\xC3\xA9") == 0);
    pal_path_join(tiny, 6u, "ab", "\xC3\xA9\xC3\xA9");     /* 5 bytes fit: "ab/" + one */
    CHECK(strcmp(tiny + 3, "\xC3\xA9") == 0 && strlen(tiny) == 5u);
    pal_path_join(tiny, 5u, "ab", "\xC3\xA9\xC3\xA9");     /* 4 fit: cut before the lead */
    CHECK(strlen(tiny) == 3u);
    pal_path_join(tiny, 1u, "ab", "cd");
    CHECK(tiny[0] == '\0');

    /* basename */
    CHECK(strcmp(pal_path_basename("a/b/c.png"), "c.png") == 0);
    CHECK(strcmp(pal_path_basename("a\\b\\c.png"), "c.png") == 0);
    CHECK(strcmp(pal_path_basename("c.png"), "c.png") == 0);
    CHECK(strcmp(pal_path_basename("a/b/"), "") == 0);
    CHECK(strcmp(pal_path_basename("/"), "") == 0);
    CHECK(strcmp(pal_path_basename(""), "") == 0);
    CHECK(strcmp(pal_path_basename(NULL), "") == 0);
    CHECK(strcmp(pal_path_basename("dir/\xE7\x94\xBB\xE5\x83\x8F.png"),
                 "\xE7\x94\xBB\xE5\x83\x8F.png") == 0);

    /* dirname */
    pal_path_dirname(out, sizeof out, "a/b/c.png");
    CHECK(strcmp(out, "a/b") == 0);
    pal_path_dirname(out, sizeof out, "a\\b\\c.png");
    CHECK(strcmp(out, "a\\b") == 0);
    pal_path_dirname(out, sizeof out, "a//c.png");
    CHECK(strcmp(out, "a") == 0);
    pal_path_dirname(out, sizeof out, "/c.png");
    CHECK(strcmp(out, "/") == 0);
    pal_path_dirname(out, sizeof out, "c.png");
    CHECK(strcmp(out, "") == 0);
    pal_path_dirname(out, sizeof out, "a/b/");
    CHECK(strcmp(out, "a/b") == 0);
#if defined(_WIN32)
    pal_path_dirname(out, sizeof out, "C:\\x.png");
    CHECK(strcmp(out, "C:\\") == 0);
    pal_path_dirname(out, sizeof out, "\\\\srv\\share\\x.png");
    CHECK(strcmp(out, "\\\\srv\\share\\") == 0);
    CHECK(strcmp(pal_path_basename("C:x.png"), "x.png") == 0);
    pal_path_join(out, sizeof out, "a", "D:\\abs");
    CHECK(strcmp(out, "D:\\abs") == 0);
    CHECK(pal_path_sep() == '\\');
#else
    CHECK(pal_path_sep() == '/');
#endif

    /* ext */
    CHECK(strcmp(pal_path_ext("a/b/c.PNG"), "PNG") == 0);
    CHECK(strcmp(pal_path_ext("archive.tar.gz"), "gz") == 0);
    CHECK(strcmp(pal_path_ext(".hidden"), "") == 0);
    CHECK(strcmp(pal_path_ext("noext"), "") == 0);
    CHECK(strcmp(pal_path_ext("name."), "") == 0);
    CHECK(strcmp(pal_path_ext("dir.d/file"), "") == 0);
    CHECK(strcmp(pal_path_ext("dir.d\\file"), "") == 0);
}

static void t_glob(void)
{
    CHECK(pal__glob_match(NULL, "x"));
    CHECK(pal__glob_match("", "x"));
    CHECK(pal__glob_match("*", ""));
    CHECK(pal__glob_match("*.txt", "a.txt"));
    CHECK(pal__glob_match("*.txt", "A.TXT"));
    CHECK(!pal__glob_match("*.txt", "a.txt.bak"));
    CHECK(pal__glob_match("*.png;*.jpg", "x.JPG"));
    CHECK(!pal__glob_match("*.png;*.jpg", "x.gif"));
    CHECK(pal__glob_match("?.txt", "b.txt"));
    CHECK(!pal__glob_match("?.txt", "bb.txt"));
    CHECK(pal__glob_match("a*b*c", "aXXbYYc"));
    CHECK(!pal__glob_match("a*b*c", "aXXbYY"));
    CHECK(pal__glob_match("**a", "bbba"));
    CHECK(pal__glob_match("*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
    CHECK(!pal__glob_match("*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK(pal__glob_match(";;*.x;", "q.x"));
    CHECK(!pal__glob_match("*.x", NULL));
    CHECK(pal__name_cmp("a", "B") < 0 && pal__name_cmp("B", "a") > 0);
    CHECK(pal__name_cmp("A", "a") < 0 && pal__name_cmp("a", "a") == 0);
    CHECK(pal__name_cmp("ab", "a") > 0);
}

/* ---- init and directories -------------------------------------------------------- */
static void set_env(const char *k, const char *v)
{
    if (v) (void)SDL_setenv_unsafe(k, v, 1);
    else   (void)SDL_unsetenv_unsafe(k);
}

static void t_init_dirs(void)
{
    char buf[1100], expect[1200];
    const char *const *fonts;
    char sep[2];
    sep[0] = pal_path_sep();
    sep[1] = '\0';
    P(buf, sizeof buf, "home");
    set_env("HOME", buf);
#if defined(_WIN32)
    P(buf, sizeof buf, "roaming");
    set_env("APPDATA", buf);
    P(buf, sizeof buf, "local");
    set_env("LOCALAPPDATA", buf);
#elif !defined(__APPLE__)
    P(buf, sizeof buf, "cfg");
    set_env("XDG_CONFIG_HOME", buf);
    set_env("XDG_DATA_HOME", NULL);
    set_env("XDG_CACHE_HOME", "relative/cache");     /* not absolute: ignored */
    set_env("XDG_STATE_HOME", NULL);
#endif
    set_env("PAINTC_LOG", "debug");
    CHECK(pal_dir(PAL_DIR_CONFIG) == NULL);          /* before pal_init */
    CHECK(pal_init("org.paintc.test", "paintc", "paint.c"));
    CHECK(pal_init("org.paintc.test", "paintc", "paint.c"));   /* idempotent */
    for (int k = 0; k < (int)PAL_DIR_COUNT; k++) {
        const char *d = pal_dir((pal_dir_kind)k);
        CHECK(d != NULL);
        if (!d) continue;
        CHECK(ends_with_sep(d));
        CHECK(is_abs(d));
        if (k <= (int)PAL_DIR_STATE || k == (int)PAL_DIR_EXE) CHECK(pal_is_dir(d));
        INFO("dir %d: %s", k, d);
    }
    CHECK(pal_dir(PAL_DIR_COUNT) == NULL);
#if defined(_WIN32)
    (void)snprintf(expect, sizeof expect, "%sroaming\\paintc\\", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CONFIG), expect) == 0);
    CHECK(strcmp(pal_dir(PAL_DIR_DATA), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%slocal\\paintc\\Cache\\", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CACHE), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%slocal\\paintc\\State\\", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_STATE), expect) == 0);
#elif defined(__APPLE__)
    (void)snprintf(expect, sizeof expect, "%shome/Library/Application Support/paint.c/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CONFIG), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%shome/Library/Caches/paint.c/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CACHE), expect) == 0);
#else
    (void)snprintf(expect, sizeof expect, "%scfg/paintc/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CONFIG), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%shome/.local/share/paintc/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_DATA), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%shome/.cache/paintc/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_CACHE), expect) == 0);
    (void)snprintf(expect, sizeof expect, "%shome/.local/state/paintc/", g_root);
    CHECK(strcmp(pal_dir(PAL_DIR_STATE), expect) == 0);
#endif
#if !defined(_WIN32)
    {
        struct stat st;
        CHECK(stat(pal_dir(PAL_DIR_STATE), &st) == 0 && (st.st_mode & 0777) == 0700);
        CHECK(stat(pal_dir(PAL_DIR_CONFIG), &st) == 0 && (st.st_mode & 0077) == 0);
    }
#endif
    /* removed directories come back on demand */
    {
        char probe[1200];
        (void)snprintf(probe, sizeof probe, "%s", pal_dir(PAL_DIR_CACHE));
        CHECK(pal_remove(probe));
        CHECK(!pal_is_dir(probe));
        CHECK(pal_is_dir(pal_dir(PAL_DIR_CACHE)));
    }
    fonts = pal_font_dirs();
    CHECK(fonts != NULL && fonts[0] != NULL);
    for (int i = 0; fonts && fonts[i]; i++) {
        CHECK(ends_with_sep(fonts[i]) && is_abs(fonts[i]));
        INFO("font dir: %s", fonts[i]);
    }
    CHECK(strcmp(pal_lib_suffix(), LIB_SUFFIX) == 0);
    (void)sep;
}

/* ---- machine --------------------------------------------------------------------- */
static void t_machine(void)
{
    uint64_t t0 = pal_ticks_ns(), t1;
    uint32_t f = pal_cpu_features();
    CHECK(pal_cpu_count() >= 1u);
    CHECK(pal_ram_bytes() > 0u);
    SDL_Delay(2);
    t1 = pal_ticks_ns();
    CHECK(t1 > t0 && t1 - t0 >= 1000000u);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK((f & PAL_CPU_NEON) == 0u);
    CHECK(!(f & PAL_CPU_AVX2) || (f & PAL_CPU_SSE41));   /* AVX2 parts have SSE4.1 */
#elif defined(__aarch64__) || defined(_M_ARM64)
    CHECK((f & PAL_CPU_NEON) != 0u);
#endif
    INFO("%u logical CPUs, %llu MiB, features 0x%x", (unsigned)pal_cpu_count(),
         (unsigned long long)(pal_ram_bytes() >> 20), (unsigned)f);
}

/* ---- files ----------------------------------------------------------------------- */
static const char *const k_utf8_names[] = {
    "plain.txt",
    "\xC3\x9C" "n\xC3\xAF" "c\xC3\xB8" "d\xC3\xA9 file.png",          /* Ünïcødé file.png */
    "\xCE\xB5\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC.txt", /* greek */
    "\xE7\x94\xBB\xE5\x83\x8F.pdn",                                    /* CJK */
    "\xF0\x9F\x8E\xA8 art.bin",                                        /* emoji, 4 bytes */
    NULL
};

static void t_files_utf8(void)
{
    char dir[1100], path[1200];
    char **names = NULL;
    int n;
    P(dir, sizeof dir, "utf8 d\xC3\xADr");
    CHECK(pal_mkdirs(dir));
    CHECK(pal_is_dir(dir) && pal_file_exists(dir));
    for (int i = 0; k_utf8_names[i]; i++) {
        pal_path_join(path, sizeof path, dir, k_utf8_names[i]);
        CHECK(!pal_file_exists(path));
        CHECK(pal_file_mtime(path) == 0u);
        CHECK(write_str(path, k_utf8_names[i]));
        CHECK(pal_file_exists(path) && !pal_is_dir(path));
        CHECK(file_equals(path, k_utf8_names[i], strlen(k_utf8_names[i])));
        CHECK(pal_file_mtime(path) > 1600000000u);
    }
    n = pal_list_dir(dir, NULL, &names);
    CHECK(n == 5);
    for (int i = 0; k_utf8_names[i]; i++) {
        bool found = false;
        for (int j = 0; j < n; j++) found = found || strcmp(names[j], k_utf8_names[i]) == 0;
        CHECK(found);
    }
    pal_free_names(names, n);
    for (int i = 0; k_utf8_names[i]; i++) {
        pal_path_join(path, sizeof path, dir, k_utf8_names[i]);
        CHECK(pal_remove(path));
        CHECK(!pal_file_exists(path));
        CHECK(!pal_remove(path));
    }
    CHECK(pal_list_dir(dir, NULL, &names) == 0 && names == NULL);
    CHECK(pal_remove(dir));
    CHECK(!pal_is_dir(dir));
    /* invalid arguments */
    CHECK(!pal_file_exists(NULL) && !pal_file_exists(""));
    CHECK(!pal_is_dir(NULL));
    CHECK(!pal_remove(NULL));
    CHECK(!pal_mkdirs(NULL) && !pal_mkdirs(""));
    CHECK(pal_file_mtime(NULL) == 0u);
}

static int count_entries(const char *dir)
{
    char **names = NULL;
    int n = pal_list_dir(dir, NULL, &names);
    pal_free_names(names, n);
    return n;
}

static void t_atomic_write(void)
{
    char dir[1100], path[1200], bad[1200];
    uint8_t *big;
    size_t big_n = g_quick ? ((size_t)3 << 20) + 17u : ((size_t)40 << 20) + 17u;
    P(dir, sizeof dir, "atomic");
    CHECK(pal_mkdirs(dir));
    pal_path_join(path, sizeof path, dir, "doc.pdn");
    CHECK(write_str(path, "first version, longer than the second"));
    CHECK(file_equals(path, "first version, longer than the second", 37u));
    CHECK(write_str(path, "second"));
    CHECK(file_equals(path, "second", 6u));
    CHECK(count_entries(dir) == 1);                  /* no temp file left */
    CHECK(pal_write_file_atomic(path, "", 0u) == PC_OK);
    CHECK(file_equals(path, "", 0u));
    CHECK(pal_write_file_atomic(path, NULL, 0u) == PC_OK);
    CHECK(pal_write_file_atomic(path, NULL, 5u) == PC_ERR_ARG);
    CHECK(pal_write_file_atomic(NULL, "x", 1u) == PC_ERR_ARG);
    CHECK(pal_write_file_atomic("", "x", 1u) == PC_ERR_ARG);
    /* big pseudo-random payload */
    big = (uint8_t *)malloc(big_n);
    CHECK(big != NULL);
    if (big) {
        for (size_t i = 0; i < big_n; i++) big[i] = rnd8();
        CHECK(pal_write_file_atomic(path, big, big_n) == PC_OK);
        CHECK(file_equals(path, big, big_n));
        free(big);
    }
    CHECK(count_entries(dir) == 1);
    /* failure leaves the target untouched and no temp file behind */
    pal_path_join(bad, sizeof bad, dir, "missing-subdir");
    pal_path_join(bad, sizeof bad, bad, "x.png");
    CHECK(pal_write_file_atomic(bad, "x", 1u) == PC_ERR_IO);
    CHECK(!pal_file_exists(bad));
    pal_path_join(bad, sizeof bad, dir, "sub");
    CHECK(pal_mkdirs(bad));
    CHECK(pal_write_file_atomic(bad, "x", 1u) != PC_OK);   /* a directory */
    CHECK(pal_is_dir(bad));
    CHECK(pal_remove(bad));
    CHECK(count_entries(dir) == 1);
#if !defined(_WIN32)
    {
        struct stat st;
        char link[1200], fresh[1200];
        mode_t um = umask(022);
        (void)umask(um);
        /* permissions of an existing file survive a save */
        CHECK(chmod(path, 0640) == 0);
        CHECK(write_str(path, "perm"));
        CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
        /* a new file gets 0666 & ~umask, never the temp file's 0600 */
        pal_path_join(fresh, sizeof fresh, dir, "fresh.png");
        CHECK(write_str(fresh, "new"));
        CHECK(stat(fresh, &st) == 0 && (st.st_mode & 0777) == (0666 & ~um));
        CHECK(pal_remove(fresh));
        /* saving through a symlink replaces the target, keeps the link */
        pal_path_join(link, sizeof link, dir, "link.pdn");
        CHECK(symlink("doc.pdn", link) == 0);
        CHECK(write_str(link, "via link"));
        CHECK(file_equals(path, "via link", 8u));
        CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(pal_remove(link));
    }
#endif
    CHECK(pal_remove(path));
    CHECK(count_entries(dir) == 0);
    CHECK(pal_remove(dir));
}

static void t_read_cap(void)
{
    char path[1100], dir[1100];
    char data[1000];
    uint8_t *d = (uint8_t *)1;
    size_t len = 77u;
    for (int i = 0; i < 1000; i++) data[i] = (char)('a' + i % 26);
    P(path, sizeof path, "cap.bin");
    CHECK(pal_write_file_atomic(path, data, sizeof data) == PC_OK);
    CHECK(pal_read_file(path, 999u, &d, &len) == PC_ERR_LIMIT);
    CHECK(d == NULL && len == 0u);
    CHECK(pal_read_file(path, 1000u, &d, &len) == PC_OK);
    CHECK(d != NULL && len == 1000u && memcmp(d, data, 1000u) == 0 && d[1000] == 0u);
    free(d);
    CHECK(pal_read_file(path, UINT64_MAX, &d, &len) == PC_OK && len == 1000u);
    free(d);
    CHECK(pal_write_file_atomic(path, "", 0u) == PC_OK);
    CHECK(pal_read_file(path, 0u, &d, &len) == PC_OK && len == 0u && d && d[0] == 0u);
    free(d);
    CHECK(pal_write_file_atomic(path, "x", 1u) == PC_OK);
    CHECK(pal_read_file(path, 0u, &d, &len) == PC_ERR_LIMIT && d == NULL);
    CHECK(pal_remove(path));
    CHECK(pal_read_file(path, 100u, &d, &len) == PC_ERR_IO && d == NULL);
    P(dir, sizeof dir, "");
    CHECK(pal_read_file(dir, 100u, &d, &len) == PC_ERR_IO && d == NULL);
    CHECK(pal_read_file(NULL, 100u, &d, &len) == PC_ERR_ARG);
    CHECK(pal_read_file(path, 100u, NULL, &len) == PC_ERR_ARG);
}

static void t_list_dir(void)
{
    static const char *const files[] = { "b.txt", "A.txt", "c.PNG", "a.png", "_x", "Z.txt",
                                         ".hidden", NULL };
    static const char *const all[] = { ".hidden", "_x", "a.png", "A.txt", "b.txt", "c.PNG",
                                       "sub", "Z.txt" };
    static const char *const txt[] = { "A.txt", "b.txt", "Z.txt" };
    static const char *const img[] = { "a.png", "A.txt", "b.txt", "c.PNG", "Z.txt" };
    char dir[1100], path[1200];
    char **names = NULL;
    int n;
    P(dir, sizeof dir, "list");
    CHECK(pal_mkdirs(dir));
    for (int i = 0; files[i]; i++) {
        pal_path_join(path, sizeof path, dir, files[i]);
        CHECK(write_str(path, files[i]));
    }
    pal_path_join(path, sizeof path, dir, "sub");
    CHECK(pal_mkdirs(path));
    n = pal_list_dir(dir, NULL, &names);
    CHECK(n == 8);
    for (int i = 0; i < n && i < 8; i++) CHECK(strcmp(names[i], all[i]) == 0);
    pal_free_names(names, n);
    n = pal_list_dir(dir, "*.txt", &names);
    CHECK(n == 3);
    for (int i = 0; i < n && i < 3; i++) CHECK(strcmp(names[i], txt[i]) == 0);
    pal_free_names(names, n);
    n = pal_list_dir(dir, "*.png;?.TXT", &names);
    CHECK(n == 5);
    for (int i = 0; i < n && i < 5; i++) CHECK(strcmp(names[i], img[i]) == 0);
    pal_free_names(names, n);
    n = pal_list_dir(dir, "nothing*", &names);
    CHECK(n == 0 && names == NULL);
    pal_path_join(path, sizeof path, dir, "absent");
    n = pal_list_dir(path, NULL, &names);
    CHECK(n == 0 && names == NULL);
    CHECK(pal_list_dir(NULL, NULL, &names) == 0);
    CHECK(pal_list_dir(dir, NULL, NULL) == 0);
    pal_free_names(NULL, 3);
    /* many entries stay sorted */
    {
        char many[1200];
        int count = g_quick ? 300 : 3000;
        bool sorted = true;
        pal_path_join(many, sizeof many, dir, "sub");
        for (int i = 0; i < count; i++) {
            char name[32];
            (void)snprintf(name, sizeof name, "%c%05u.dat", (i & 1) ? 'F' : 'f',
                           (unsigned)rndu(100000u));
            pal_path_join(path, sizeof path, many, name);
            CHECK(write_str(path, name));
        }
        n = pal_list_dir(many, "*.DAT", &names);
        CHECK(n > count / 2);
        for (int i = 1; i < n; i++) sorted = sorted && pal__name_cmp(names[i - 1], names[i]) < 0;
        CHECK(sorted);
        pal_free_names(names, n);
    }
    rm_tree(dir);
    CHECK(!pal_is_dir(dir));
}

static void t_long_paths(void)
{
    /* > 260 characters total: exercises the \\?\ prefix on Windows. */
    char path[2048], file[2100];
    char seg[64];
    char **names = NULL;
    int n;
    memset(seg, 'L', 50u);
    seg[50] = '\0';
    P(path, sizeof path, "long");
    for (int i = 0; i < 6; i++) {
        seg[0] = (char)('0' + i);
        pal_path_join(path, sizeof path, path, seg);
    }
    CHECK(strlen(path) > 300u);
    CHECK(pal_mkdirs(path));
    CHECK(pal_is_dir(path));
    pal_path_join(file, sizeof file, path, "\xC3\xA9t\xC3\xA9.png");
    CHECK(write_str(file, "deep"));
    CHECK(write_str(file, "deeper"));
    CHECK(file_equals(file, "deeper", 6u));
    CHECK(pal_file_mtime(file) > 0u);
    n = pal_list_dir(path, "*.PNG", &names);
    CHECK(n == 1 && strcmp(names[0], "\xC3\xA9t\xC3\xA9.png") == 0);
    pal_free_names(names, n);
    P(path, sizeof path, "long");
    rm_tree(path);
    CHECK(!pal_is_dir(path));
}

/* ---- pool ------------------------------------------------------------------------ */
typedef struct sum_job {
    uint32_t     *hits;      /* per index */
    uint64_t     *scratch;   /* per worker, unsynchronized on purpose */
    uint32_t      threads;
    pc_atomic_u32 bad_worker;
} sum_job;

static void sum_fn(void *ud, uint32_t index, uint32_t worker)
{
    sum_job *j = (sum_job *)ud;
    if (worker >= j->threads) {
        (void)pc_atomic_inc(&j->bad_worker);
        return;
    }
    j->hits[index]++;                        /* each index runs exactly once */
    j->scratch[worker] += (uint64_t)index * 2654435761u % 1000003u;
}

static uint64_t run_sum(const pc_par *par, uint32_t count, bool *ok)
{
    sum_job j;
    uint64_t total = 0;
    memset(&j, 0, sizeof j);
    j.threads = pc_par_threads(par);
    j.hits = (uint32_t *)calloc(count ? count : 1u, sizeof *j.hits);
    j.scratch = (uint64_t *)calloc(j.threads, sizeof *j.scratch);
    *ok = j.hits && j.scratch;
    if (*ok) {
        pc_par_for(par, sum_fn, &j, count);
        for (uint32_t i = 0; i < count; i++) *ok = *ok && j.hits[i] == 1u;
        for (uint32_t w = 0; w < j.threads; w++) total += j.scratch[w];
        *ok = *ok && pc_atomic_load(&j.bad_worker) == 0u;
    }
    free(j.hits);
    free(j.scratch);
    return total;
}

static void t_pool_parallel_for(void)
{
    static const uint32_t counts[] = { 0u, 1u, 2u, 3u, 17u, 64u, 1000u, 100003u };
    static const uint32_t workers[] = { 1u, 2u, 3u, 7u, 0u };
    uint32_t big = g_quick ? 1000003u : 20000003u;
    for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        bool ok;
        uint64_t ref = run_sum(NULL, counts[c], &ok);
        CHECK(ok);
        for (size_t w = 0; w < sizeof workers / sizeof workers[0]; w++) {
            pal_pool *p = pal_pool_create(workers[w]);
            pc_par par = pal_pool_par(p);
            CHECK(p != NULL);
            CHECK(par.threads == (workers[w] ? workers[w] + 1u : par.threads));
            CHECK(par.threads >= 2u);
            CHECK(run_sum(&par, counts[c], &ok) == ref);   /* determinism */
            CHECK(ok);
            pal_pool_destroy(p);
        }
    }
    {
        bool ok;
        uint64_t ref = run_sum(NULL, big, &ok);
        pal_pool *p = pal_pool_create(0u);
        pc_par par = pal_pool_par(p);
        for (int rep = 0; rep < 3; rep++) {
            CHECK(run_sum(&par, big, &ok) == ref);
            CHECK(ok);
        }
        pal_pool_destroy(p);
    }
    {
        pc_par none = pal_pool_par(NULL);
        bool ok;
        CHECK(none.threads == 1u && none.run == NULL);
        (void)run_sum(&none, 10u, &ok);
        CHECK(ok);
    }
    pal_pool_destroy(NULL);
}

/* Several threads share one pool. */
typedef struct caller_arg {
    pc_par   par;
    uint64_t ref;
    int      reps;
    bool     ok;
} caller_arg;

static int SDLCALL caller_main(void *ud)
{
    caller_arg *a = (caller_arg *)ud;
    a->ok = true;
    for (int i = 0; i < a->reps; i++) {
        bool ok;
        uint64_t s = run_sum(&a->par, 50021u, &ok);
        a->ok = a->ok && ok && s == a->ref;
    }
    return 0;
}

static void t_pool_concurrent_callers(void)
{
    pal_pool *p = pal_pool_create(3u);
    caller_arg args[4];
    SDL_Thread *th[4];
    bool ok;
    uint64_t ref = run_sum(NULL, 50021u, &ok);
    for (int i = 0; i < 4; i++) {
        args[i].par = pal_pool_par(p);
        args[i].ref = ref;
        args[i].reps = g_quick ? 20 : 200;
        args[i].ok = false;
        th[i] = SDL_CreateThread(caller_main, "caller", &args[i]);
        CHECK(th[i] != NULL);
    }
    for (int i = 0; i < 4; i++) {
        SDL_WaitThread(th[i], NULL);
        CHECK(args[i].ok);
    }
    pal_pool_destroy(p);
}

/* A job item that itself runs a parallel-for on the same pool. */
typedef struct nest_job {
    pc_par        par;
    pc_atomic_u32 inner;
} nest_job;

static void nest_inner(void *ud, uint32_t index, uint32_t worker)
{
    nest_job *j = (nest_job *)ud;
    (void)index;
    (void)worker;
    (void)pc_atomic_inc(&j->inner);
}

static void nest_outer(void *ud, uint32_t index, uint32_t worker)
{
    nest_job *j = (nest_job *)ud;
    (void)index;
    (void)worker;
    pc_par_for(&j->par, nest_inner, j, 100u);
}

static void t_pool_nested(void)
{
    pal_pool *p = pal_pool_create(2u);
    nest_job j;
    memset(&j, 0, sizeof j);
    j.par = pal_pool_par(p);
    pc_par_for(&j.par, nest_outer, &j, 50u);
    CHECK(pc_atomic_load(&j.inner) == 5000u);
    pal_pool_destroy(p);
}

/* ---- tasks ----------------------------------------------------------------------- */
typedef struct task_ctx {
    pc_atomic_u32 runs;
    pal_pool     *pool;
    pal_task     *child;
    pc_atomic_u32 child_runs;
    int           sleep_ms;
} task_ctx;

static void task_count(void *ud)
{
    task_ctx *c = (task_ctx *)ud;
    if (c->sleep_ms) SDL_Delay((Uint32)c->sleep_ms);
    (void)pc_atomic_inc(&c->runs);
}

static void task_child(void *ud)
{
    task_ctx *c = (task_ctx *)ud;
    (void)pc_atomic_inc(&c->child_runs);
}

/* Runs on a worker: submits another task to the same pool and waits. */
static void task_parent(void *ud)
{
    task_ctx *c = (task_ctx *)ud;
    pal_task *t = pal_task_submit(c->pool, task_child, c);
    pal_task_wait(t);
    if (pal_task_done(t)) (void)pc_atomic_inc(&c->runs);
    pal_task_free(t);
}

static void t_tasks(void)
{
    enum { N = 64 };
    pal_pool *p = pal_pool_create(3u);
    pal_task *t[N];
    task_ctx c;
    memset(&c, 0, sizeof c);
    c.sleep_ms = 1;
    for (int i = 0; i < N; i++) {
        t[i] = pal_task_submit(p, task_count, &c);
        CHECK(t[i] != NULL);
    }
    for (int i = 0; i < N; i += 2) {               /* wait some, poll the rest */
        pal_task_wait(t[i]);
        CHECK(pal_task_done(t[i]));
    }
    for (int i = 1; i < N; i += 2) {
        uint64_t until = SDL_GetTicks() + 10000u;
        while (!pal_task_done(t[i]) && SDL_GetTicks() < until) SDL_Delay(1);
        CHECK(pal_task_done(t[i]));
    }
    CHECK(pc_atomic_load(&c.runs) == (uint32_t)N);
    for (int i = 0; i < N; i++) pal_task_free(t[i]);
    pal_task_free(NULL);
    CHECK(pal_task_done(NULL));
    pal_task_wait(NULL);
    CHECK(pal_task_submit(NULL, task_count, &c) == NULL);
    CHECK(pal_task_submit(p, NULL, &c) == NULL);
    pal_pool_destroy(p);

    /* Nested submission and wait from inside a task: with a single worker
     * the parent occupies it, so the child must run inside the wait. */
    for (uint32_t workers = 1; workers <= 2u; workers++) {
        task_ctx n;
        pal_task *pt;
        memset(&n, 0, sizeof n);
        n.pool = pal_pool_create(workers);
        pt = pal_task_submit(n.pool, task_parent, &n);
        pal_task_wait(pt);
        CHECK(pal_task_done(pt));
        CHECK(pc_atomic_load(&n.runs) == 1u && pc_atomic_load(&n.child_runs) == 1u);
        pal_task_free(pt);
        pal_pool_destroy(n.pool);
    }

    /* Destroy drains the queue: every queued task runs before it returns. */
    {
        task_ctx d;
        pal_pool *q = pal_pool_create(2u);
        pal_task *dt[32];
        memset(&d, 0, sizeof d);
        d.sleep_ms = 1;
        for (int i = 0; i < 32; i++) dt[i] = pal_task_submit(q, task_count, &d);
        pal_pool_destroy(q);
        CHECK(pc_atomic_load(&d.runs) == 32u);
        for (int i = 0; i < 32; i++) {
            CHECK(pal_task_done(dt[i]));
            pal_task_free(dt[i]);
        }
    }

    /* A parallel-for still completes while every worker runs a long task. */
    {
        task_ctx l;
        pal_pool *q = pal_pool_create(2u);
        pal_task *lt[2];
        pc_par par = pal_pool_par(q);
        bool ok;
        uint64_t ref = run_sum(NULL, 1000u, &ok);
        memset(&l, 0, sizeof l);
        l.sleep_ms = 100;
        lt[0] = pal_task_submit(q, task_count, &l);
        lt[1] = pal_task_submit(q, task_count, &l);
        CHECK(run_sum(&par, 1000u, &ok) == ref && ok);
        pal_task_free(lt[0]);                       /* waits if still running */
        pal_task_free(lt[1]);
        CHECK(pc_atomic_load(&l.runs) == 2u);
        pal_pool_destroy(q);
    }
}

typedef struct mutex_arg {
    pal_mutex *m;
    uint64_t  *counter;
    int        reps;
} mutex_arg;

static int SDLCALL mutex_main(void *ud)
{
    mutex_arg *a = (mutex_arg *)ud;
    for (int i = 0; i < a->reps; i++) {
        pal_mutex_lock(a->m);
        (*a->counter)++;
        pal_mutex_unlock(a->m);
    }
    return 0;
}

static void t_mutex(void)
{
    uint64_t counter = 0;
    mutex_arg a;
    SDL_Thread *th[4];
    a.m = pal_mutex_create();
    a.counter = &counter;
    a.reps = g_quick ? 20000 : 200000;
    CHECK(a.m != NULL);
    for (int i = 0; i < 4; i++) th[i] = SDL_CreateThread(mutex_main, "mutex", &a);
    for (int i = 0; i < 4; i++) SDL_WaitThread(th[i], NULL);
    CHECK(counter == (uint64_t)a.reps * 4u);
    pal_mutex_destroy(a.m);
    pal_mutex_destroy(NULL);
    pal_mutex_lock(NULL);
    pal_mutex_unlock(NULL);
}

/* ---- logging --------------------------------------------------------------------- */
typedef struct log_arg { int id; } log_arg;

static int SDLCALL log_main(void *ud)
{
    const log_arg *a = (const log_arg *)ud;
    for (int i = 0; i < 50; i++) pal_log(PAL_LOG_DEBUG, "thread %d line %d", a->id, i);
    return 0;
}

static void t_logging(void)
{
    char path[1200];
    char *big = (char *)malloc(5000u);
    uint8_t *d = NULL;
    size_t len = 0;
    SDL_Thread *th[4];
    log_arg args[4];
    CHECK(big != NULL);
    if (!big) return;
    memset(big, 'x', 4999u);
    big[4999] = '\0';
    memcpy(big + 4990, "END-MARK", 8u);
    pal_log(PAL_LOG_INFO, "log test %s %d %.2f %llu", "\xC3\xA9", 42, 1.5,
            (unsigned long long)UINT64_MAX);
    pal_log(PAL_LOG_WARN, "%s", big);
    pal_log(PAL_LOG_ERROR, "trailing newline is dropped\n");
    pal_log(99, "level clamps");
    for (int i = 0; i < 4; i++) {
        args[i].id = i;
        th[i] = SDL_CreateThread(log_main, "log", &args[i]);
    }
    for (int i = 0; i < 4; i++) SDL_WaitThread(th[i], NULL);
    SDL_Log("from SDL_Log");                        /* routed into the pal log */
    pal_path_join(path, sizeof path, pal_dir(PAL_DIR_STATE), "paintc.log");
    CHECK(pal_read_file(path, (uint64_t)16 << 20, &d, &len) == PC_OK);
    if (d) {
        const char *s = (const char *)d;
        CHECK(strstr(s, "log test \xC3\xA9 42 1.50 18446744073709551615") != NULL);
        CHECK(strstr(s, "END-MARK") != NULL);
        CHECK(strstr(s, "] W xxxx") != NULL);
        CHECK(strstr(s, "trailing newline is dropped\n[") != NULL);
        CHECK(strstr(s, "] E level clamps") != NULL);
        CHECK(strstr(s, "thread 3 line 49") != NULL);
        CHECK(strstr(s, "SDL: from SDL_Log") != NULL);
        CHECK(strstr(s, "---- log opened ") != NULL);
        free(d);
    }
    free(big);
}

/* ---- dynamic libraries ----------------------------------------------------------- */
static void t_dynlib(void)
{
    char path[1200], name[64];
    pal_lib *l;
    (void)snprintf(name, sizeof name, "pal_test_plugin%s", pal_lib_suffix());
    pal_path_join(path, sizeof path, pal_dir(PAL_DIR_EXE), name);
    l = pal_lib_open(path);
    CHECK(l != NULL);
    if (l) {
        void *sym = pal_lib_sym(l, "pal_fixture_add");
        int (*add)(int, int) = NULL;
        CHECK(sym != NULL);
        if (sym) {
            memcpy(&add, &sym, sizeof add);          /* object -> function pointer */
            CHECK(add(2, 3) == 5);
        }
        CHECK(pal_lib_sym(l, "no_such_symbol") == NULL);
        CHECK(pal_lib_sym(l, NULL) == NULL);
        pal_lib_close(l);
    }
    pal_path_join(path, sizeof path, pal_dir(PAL_DIR_EXE), "no_such_plugin.bin");
    CHECK(pal_lib_open(path) == NULL);
    CHECK(pal_lib_open(NULL) == NULL);
    CHECK(pal_lib_sym(NULL, "x") == NULL);
    pal_lib_close(NULL);
}

/* ---- image flavour encoders ------------------------------------------------------ */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static void make_img(uint8_t *px, int32_t w, int32_t h, size_t stride)
{
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            uint8_t *p = px + (size_t)y * stride + (size_t)x * 4u;
            p[0] = (uint8_t)(x * 7 + y);       /* B */
            p[1] = (uint8_t)(y * 13);          /* G */
            p[2] = (uint8_t)(x ^ y);           /* R */
            p[3] = (uint8_t)(255 - x - y);     /* A */
        }
}

/* Value of a TIFF tag (SHORT or LONG, count 1, or offset for larger). */
static bool tiff_tag(const uint8_t *t, size_t n, uint32_t tag, uint32_t *val, uint32_t *count)
{
    uint32_t ifd = rd32(t + 4), entries;
    if (ifd + 2u > n) return false;
    entries = rd16(t + ifd);
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = t + ifd + 2u + i * 12u;
        if ((size_t)(e - t) + 12u > n) return false;
        if (rd16(e) == tag) {
            uint32_t type = rd16(e + 2);
            *count = rd32(e + 4);
            *val = (type == 3u && *count == 1u) ? rd16(e + 8) : rd32(e + 8);
            return true;
        }
    }
    return false;
}

static void t_encoders(void)
{
    const int32_t w = 5, h = 3;
    const size_t stride = 5u * 4u + 8u;            /* padded rows */
    uint8_t px[3 * 28];
    uint8_t *bmp, *dib, *conv, *tif;
    size_t bmp_n = 0, dib_n = 0, conv_n = 0, tif_n = 0;
    make_img(px, w, h, stride);

    bmp = pal__enc_bmp(px, w, h, stride, true, &bmp_n);
    CHECK(bmp != NULL && bmp_n == 14u + 124u + 60u);
    if (bmp) {
        CHECK(bmp[0] == 'B' && bmp[1] == 'M' && rd32(bmp + 2) == bmp_n && rd32(bmp + 10) == 138u);
        CHECK(rd32(bmp + 14) == 124u && rd32(bmp + 18) == 5u && rd32(bmp + 22) == 3u);
        CHECK(rd16(bmp + 28) == 32u && rd32(bmp + 30) == 3u && rd32(bmp + 34) == 60u);
        CHECK(rd32(bmp + 14 + 52) == 0xFF000000u);
        /* bottom-up: first stored row is image row h - 1 */
        for (int32_t y = 0; y < h; y++)
            CHECK(memcmp(bmp + 138 + (size_t)(h - 1 - y) * 20u, px + (size_t)y * stride, 20u) == 0);
    }
    dib = pal__enc_bmp(px, w, h, stride, false, &dib_n);
    CHECK(dib != NULL && dib_n == bmp_n - 14u);
    conv = dib ? pal__dib_to_bmp(dib, dib_n, &conv_n) : NULL;
    CHECK(conv != NULL && conv_n == bmp_n && bmp && memcmp(conv, bmp, bmp_n) == 0);
    free(conv);
    /* the same V5 DIB with masks appended after the header (seen in the
     * wild) is detected from its size */
    if (dib) {
        uint8_t *v = (uint8_t *)malloc(dib_n + 12u);
        if (v) {
            memcpy(v, dib, 124u);
            memcpy(v + 124, dib + 40, 12u);
            memcpy(v + 136, dib + 124, dib_n - 124u);
            conv = pal__dib_to_bmp(v, dib_n + 12u, &conv_n);
            CHECK(conv != NULL && rd32(conv + 10) == 14u + 136u);
            free(conv);
            free(v);
        }
    }
    /* classic 40-byte headers */
    {
        uint8_t d[40 + 12 + 8];                    /* 2x2 BI_BITFIELDS 16 bpp */
        uint8_t p8[40 + 1024 + 8];                 /* 3x2 8 bpp, full palette */
        uint8_t r24[40 + 16];                      /* 3x2 24 bpp: rows of 12 */
        memset(d, 0, sizeof d);
        d[0] = 40; d[4] = 2; d[8] = 2; d[12] = 1; d[14] = 16; d[16] = 3;
        conv = pal__dib_to_bmp(d, sizeof d, &conv_n);
        CHECK(conv != NULL && rd32(conv + 10) == 14u + 52u && conv_n == 14u + sizeof d);
        free(conv);
        memset(p8, 0, sizeof p8);
        p8[0] = 40; p8[4] = 3; p8[8] = 2; p8[12] = 1; p8[14] = 8;
        conv = pal__dib_to_bmp(p8, sizeof p8, &conv_n);
        CHECK(conv != NULL && rd32(conv + 10) == 14u + 40u + 1024u);
        free(conv);
        CHECK(pal__dib_to_bmp(p8, sizeof p8 - 1u, &conv_n) == NULL);   /* short pixels */
        p8[32] = 2;                                /* biClrUsed = 2 */
        conv = pal__dib_to_bmp(p8, 40u + 8u + 8u, &conv_n);
        CHECK(conv != NULL && rd32(conv + 10) == 14u + 48u);
        free(conv);
        memset(r24, 0, sizeof r24);
        r24[0] = 40; r24[4] = 1; r24[8] = 0xFE; r24[9] = 0xFF; r24[10] = 0xFF; r24[11] = 0xFF;
        r24[12] = 1; r24[14] = 24;                 /* 1 x -2: top-down */
        conv = pal__dib_to_bmp(r24, 48u, &conv_n);
        CHECK(conv != NULL && rd32(conv + 10) == 54u);
        free(conv);
        /* malformed */
        CHECK(pal__dib_to_bmp(r24, 39u, &conv_n) == NULL);
        CHECK(pal__dib_to_bmp(NULL, 48u, &conv_n) == NULL);
        r24[14] = 7;
        CHECK(pal__dib_to_bmp(r24, 48u, &conv_n) == NULL);   /* bit count */
        r24[14] = 24; r24[16] = 4;
        CHECK(pal__dib_to_bmp(r24, 48u, &conv_n) == NULL);   /* BI_JPEG */
        r24[16] = 0; r24[4] = 0;
        CHECK(pal__dib_to_bmp(r24, 48u, &conv_n) == NULL);   /* width 0 */
        r24[4] = 0xFF; r24[5] = 0xFF; r24[6] = 0xFF; r24[7] = 0x7F;
        CHECK(pal__dib_to_bmp(r24, 48u, &conv_n) == NULL);   /* huge */
    }
    /* random mutations never crash and never claim more than they have */
    if (dib) {
        int iters = g_quick ? 20000 : 300000;
        for (int it = 0; it < iters; it++) {
            uint8_t m[256];
            size_t n = 40u + rndu(200u);
            memset(m, 0, sizeof m);
            memcpy(m, dib, n < dib_n ? n : dib_n);
            for (int k = (int)rndu(6u) + 1; k > 0; k--) m[rndu((uint32_t)n)] = rnd8();
            if (rndu(2u)) { m[0] = 40; m[1] = m[2] = m[3] = 0; }
            conv = pal__dib_to_bmp(m, n, &conv_n);
            if (conv) {
                CHECK(conv_n == n + 14u && rd32(conv + 10) <= conv_n);
                free(conv);
            }
        }
    }
    free(dib);
    free(bmp);

    tif = pal__enc_tiff(px, w, h, stride, &tif_n);
    CHECK(tif != NULL);
    if (tif) {
        uint32_t v = 0, c = 0, off = 0, cnt = 0;
        CHECK(tif[0] == 'I' && tif[1] == 'I' && rd16(tif + 2) == 42u);
        CHECK(tiff_tag(tif, tif_n, 256u, &v, &c) && v == 5u);
        CHECK(tiff_tag(tif, tif_n, 257u, &v, &c) && v == 3u);
        CHECK(tiff_tag(tif, tif_n, 277u, &v, &c) && v == 4u);
        CHECK(tiff_tag(tif, tif_n, 338u, &v, &c) && v == 2u);
        CHECK(tiff_tag(tif, tif_n, 258u, &v, &c) && c == 4u && rd16(tif + v) == 8u);
        CHECK(tiff_tag(tif, tif_n, 273u, &off, &c) && (off & 1u) == 0u);
        CHECK(tiff_tag(tif, tif_n, 279u, &cnt, &c) && cnt == 60u && off + cnt == tif_n);
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                const uint8_t *s = px + (size_t)y * stride + (size_t)x * 4u;
                const uint8_t *d = tif + off + ((size_t)y * 5u + (size_t)x) * 4u;
                CHECK(d[0] == s[2] && d[1] == s[1] && d[2] == s[0] && d[3] == s[3]);
            }
        /* tags ascending (TIFF requirement) */
        {
            uint32_t ifd = rd32(tif + 4), prev = 0;
            for (uint32_t i = 0; i < rd16(tif + ifd); i++) {
                uint32_t tg = rd16(tif + ifd + 2u + i * 12u);
                CHECK(tg > prev);
                prev = tg;
            }
        }
        free(tif);
    }
    CHECK(pal__enc_bmp(px, 0, h, stride, true, &bmp_n) == NULL);
    CHECK(pal__enc_bmp(px, w, h, 4u, true, &bmp_n) == NULL);      /* stride too small */
    CHECK(pal__enc_bmp(NULL, w, h, stride, true, &bmp_n) == NULL);
    CHECK(pal__enc_tiff(px, 70000, 1, (size_t)70000 * 4u, &tif_n) == NULL);
    CHECK(pal__enc_bmp(px, 65535, 65535, (size_t)65535 * 4u, true, &bmp_n) == NULL); /* > 4 GiB */
}

/* ---- clipboard ------------------------------------------------------------------- */
static void t_clipboard(void)
{
    uint8_t px[4 * 4 * 4];
    uint8_t *d = NULL;
    size_t n = 0;
    char mime[32];
    static const uint8_t fake_png[] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 1, 2, 3 };
    make_img(px, 4, 4, 16u);
#if defined(_WIN32)
    /* Native shim: works without SDL video, so it runs headless (Wine too). */
    CHECK(pal_clip_set_text("h\xC3\xA9llo\nworld"));
    {
        char *t = pal_clip_get_text();
        CHECK(t && strcmp(t, "h\xC3\xA9llo\nworld") == 0);
        free(t);
    }
    CHECK(!pal_clip_has_image());
    CHECK(pal_clip_set_image_png(fake_png, sizeof fake_png));
    CHECK(pal_clip_has_image());
    CHECK(pal_clip_get_image(&d, &n, mime, sizeof mime));
    CHECK(d && n == sizeof fake_png && memcmp(d, fake_png, n) == 0);
    CHECK(strcmp(mime, "image/png") == 0);
    free(d);
    CHECK(pal_clip_set_image_bgra(NULL, 0u, px, 4, 4, 16u));
    CHECK(pal_clip_has_image());
    CHECK(pal_clip_get_image(&d, &n, mime, sizeof mime));
    CHECK(d && strcmp(mime, "image/bmp") == 0 && n >= 14u + 124u + 64u);
    if (d && n >= 14u + 40u) {
        uint32_t off = rd32(d + 10);
        CHECK(d[0] == 'B' && d[1] == 'M' && off + 64u <= n);
        if (off + 64u <= n)
            for (int32_t y = 0; y < 4; y++)
                CHECK(memcmp(d + off + (size_t)(3 - y) * 16u, px + (size_t)y * 16u, 16u) == 0);
    }
    free(d);
    CHECK(pal_clip_set_image_bgra(fake_png, sizeof fake_png, px, 4, 4, 16u));
    CHECK(pal_clip_get_image(&d, &n, mime, sizeof mime));
    CHECK(d && strcmp(mime, "image/png") == 0);
    free(d);
    CHECK(!pal_clip_set_image_bgra(fake_png, sizeof fake_png, px, 0, 4, 16u));
    CHECK(pal_clip_set_text(""));
    CHECK(!pal_clip_has_image());
#else
    /* No video subsystem in this headless test: every call fails cleanly
     * and never touches the desktop clipboard. */
    CHECK(!pal_clip_has_image());
    CHECK(!pal_clip_get_image(&d, &n, mime, sizeof mime) && d == NULL && n == 0u);
    CHECK(!pal_clip_set_image_png(fake_png, sizeof fake_png));
    CHECK(!pal_clip_set_image_bgra(fake_png, sizeof fake_png, px, 4, 4, 16u));
    CHECK(!pal_clip_set_text("x"));
    CHECK(pal_clip_get_text() == NULL);
#endif
    CHECK(!pal_clip_get_image(NULL, &n, mime, sizeof mime));
    CHECK(!pal_clip_set_image_png(NULL, 4u));
}

/* ---- single instance ------------------------------------------------------------- */
static void t_si_codec(void)
{
    const char *paths[3] = { "/a/b.png", "", "C:\\\xE7\x94\xBB.pdn" };
    uint8_t *m = NULL;
    size_t n = 0;
    char **out = NULL;
    int k = -1;
    CHECK(pal__si_encode(paths, 3, &m, &n));
    CHECK(pal__si_decode(m, n, &out, &k));
    CHECK(k == 3 && out && strcmp(out[0], paths[0]) == 0 && strcmp(out[1], "") == 0 &&
          strcmp(out[2], paths[2]) == 0);
    pal_free_names(out, k);
    /* every truncation and trailing garbage is rejected */
    for (size_t cut = 0; cut < n; cut++) CHECK(!pal__si_decode(m, cut, &out, &k) && out == NULL);
    if (m) {
        uint8_t *big = (uint8_t *)malloc(n + 1u);
        if (big) {
            memcpy(big, m, n);
            big[n] = 0u;
            CHECK(!pal__si_decode(big, n + 1u, &out, &k));
            big[13] = 0u;                          /* NUL inside a path */
            big[12] = 0u;
            CHECK(!pal__si_decode(big, n, &out, &k) || k == 3);
            free(big);
        }
        m[0] = 'X';
        CHECK(!pal__si_decode(m, n, &out, &k));
        m[0] = 'P';
        m[8] = 0xFF; m[9] = 0xFF;                  /* huge count */
        CHECK(!pal__si_decode(m, n, &out, &k));
    }
    free(m);
    CHECK(pal__si_encode(NULL, 0, &m, &n) && n == 12u);
    CHECK(pal__si_decode(m, n, &out, &k) && k == 0 && out == NULL);
    free(m);
    CHECK(!pal__si_encode(NULL, 2, &m, &n));
    CHECK(!pal__si_encode(paths, -1, &m, &n));
    for (int it = 0; it < (g_quick ? 20000 : 200000); it++) {
        uint8_t buf[64];
        size_t len = 12u + rndu(52u);
        for (size_t i = 0; i < len; i++) buf[i] = rnd8();
        memcpy(buf, "PCSI\x01\0\0\0", 8u);
        buf[9] = buf[10] = buf[11] = 0u;
        buf[8] = (uint8_t)rndu(4u);
        if (pal__si_decode(buf, len, &out, &k)) {
            CHECK(k == (int)buf[8]);
            pal_free_names(out, k);
        }
    }
}

typedef struct si_got {
    int  calls;
    int  n;
    char paths[4][1100];
} si_got;

static void si_cb(void *ud, const char *const *paths, int n, int filter)
{
    si_got *g = (si_got *)ud;
    g->calls++;
    g->n = n;
    CHECK(filter == -1);
    CHECK((n == 0) == (paths == NULL));
    for (int i = 0; i < n && i < 4; i++)
        (void)snprintf(g->paths[i], sizeof g->paths[i], "%s", paths[i]);
}

static int si_child(int argc, char **argv)
{
    /* argv: exe --si-child <app_id> <variant> paths... */
    const char *const *paths = (const char *const *)(argv + 4);
    bool first;
    if (argc < 4) return 3;
    if (strcmp(argv[3], "file") == 0) (void)SDL_setenv_unsafe("PAINTC_SI_FILE", "1", 1);
    if (!SDL_Init(0)) return 4;
    if (!pal_init("org.paintc.test", "paintc", "paint.c")) return 5;
    first = pal_single_instance(argv[2], argc - 4, paths, NULL, NULL);
    pal_quit();
    SDL_Quit();
    return first ? 1 : 0;
}

static void si_round(const char *variant)
{
    char app_id[64], exe[1200];
    si_got got;
    SDL_Process *proc;
    int code = -1;
    uint64_t until;
    const char *args[7];
    memset(&got, 0, sizeof got);
    (void)snprintf(app_id, sizeof app_id, "org.paintc.test.si%08x",
                   (unsigned)(pal__rand64() & 0xFFFFFFFFu));
    if (strcmp(variant, "file") == 0) (void)SDL_setenv_unsafe("PAINTC_SI_FILE", "1", 1);
    else (void)SDL_unsetenv_unsafe("PAINTC_SI_FILE");
    CHECK(pal_single_instance(app_id, 0, NULL, si_cb, &got));
    CHECK(pal_single_instance(app_id, 0, NULL, si_cb, &got));   /* already primary */
#if defined(_WIN32)
    (void)snprintf(exe, sizeof exe, "%stest_pal.exe", pal_dir(PAL_DIR_EXE));
#else
    (void)snprintf(exe, sizeof exe, "%stest_pal", pal_dir(PAL_DIR_EXE));
#endif
    args[0] = exe;
    args[1] = "--si-child";
    args[2] = app_id;
    args[3] = variant;
    args[4] = "relative \xC3\xA9.png";
    args[5] = "second.pdn";
    args[6] = NULL;
    proc = SDL_CreateProcess(args, false);
    CHECK(proc != NULL);
    if (!proc) return;
    CHECK(SDL_WaitProcess(proc, true, &code));
    SDL_DestroyProcess(proc);
#if defined(__APPLE__)
    CHECK(code == 1);                              /* no single instance on macOS */
    (void)until;
#else
    CHECK(code == 0);                              /* child forwarded and exited */
    until = SDL_GetTicks() + 10000u;
    while (got.calls == 0 && SDL_GetTicks() < until) {
        pal_pump();
        SDL_Delay(5);
    }
    CHECK(got.calls == 1 && got.n == 2);
    if (got.n == 2) {
        size_t l0 = strlen(got.paths[0]), l1 = strlen(got.paths[1]);
        INFO("forwarded: %s | %s", got.paths[0], got.paths[1]);
        CHECK(is_abs(got.paths[0]) && is_abs(got.paths[1]));
        CHECK(l0 > 15u && strcmp(got.paths[0] + l0 - 15u, "relative \xC3\xA9.png") == 0);
        CHECK(l1 > 10u && strcmp(got.paths[1] + l1 - 10u, "second.pdn") == 0);
    }
#endif
}

static void t_single_instance(void)
{
    si_round("default");
#if defined(__linux__)
    /* restart the layer so the socket-file variant gets its own primary */
    pal_quit();
    CHECK(pal_init("org.paintc.test", "paintc", "paint.c"));
    si_round("file");
#endif
    (void)SDL_unsetenv_unsafe("PAINTC_SI_FILE");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--si-child") == 0) return si_child(argc, argv);
    pc_test_init(argc, argv);
    if (!SDL_Init(0)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    {
        const char *base = SDL_GetBasePath();
        (void)snprintf(g_root, sizeof g_root, "%spal_tmp_%08x%c", base ? base : "",
                       (unsigned)(pal__rand64() & 0xFFFFFFFFu), pal_path_sep());
        if (!pal_mkdirs(g_root)) {
            fprintf(stderr, "cannot create %s\n", g_root);
            return 1;
        }
    }
    RUN(t_path_helpers);
    RUN(t_glob);
    RUN(t_init_dirs);
    RUN(t_machine);
    RUN(t_files_utf8);
    RUN(t_atomic_write);
    RUN(t_read_cap);
    RUN(t_list_dir);
    RUN(t_long_paths);
    RUN(t_pool_parallel_for);
    RUN(t_pool_concurrent_callers);
    RUN(t_pool_nested);
    RUN(t_tasks);
    RUN(t_mutex);
    RUN(t_logging);
    RUN(t_dynlib);
    RUN(t_encoders);
    RUN(t_clipboard);
    RUN(t_si_codec);
    RUN(t_single_instance);
    pal_quit();
    CHECK(pal_dir(PAL_DIR_CONFIG) == NULL);
    rm_tree(g_root);
    CHECK(!pal_is_dir(g_root));
    SDL_Quit();
    return pc_test_finish();
}
