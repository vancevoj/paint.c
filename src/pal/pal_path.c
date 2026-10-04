/* pal_path.c - pure UTF-8 path helpers, the simple glob matcher and
 * directory listing (sorting and filtering on top of the OS hook).
 *
 * Both '/' and '\\' count as separators on every OS, so results do not
 * depend on where the code runs. Drive prefixes ("C:") are recognized only
 * in Windows builds, where they can occur.
 */
#include "pal_internal.h"

#include <stdlib.h>
#include <string.h>

bool pal__is_sep(char c) { return c == '/' || c == '\\'; }

char pal_path_sep(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

/* Length of the root prefix of p: "/" or "\\" (1), "//server/share/" style
 * UNC roots and "C:\" or "C:" drive roots (Windows builds only). */
static size_t root_len(const char *p)
{
#if defined(_WIN32)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return pal__is_sep(p[2]) ? 3u : 2u;
    if (pal__is_sep(p[0]) && pal__is_sep(p[1])) {
        /* \\server\share\ (also \\?\C:\ which is treated as a UNC-like root) */
        size_t i = 2u;
        int parts = 0;
        while (p[i] && parts < 2) {
            while (p[i] && !pal__is_sep(p[i])) i++;
            parts++;
            if (p[i]) i++;
        }
        return i;
    }
#endif
    return pal__is_sep(p[0]) ? 1u : 0u;
}

static bool is_absolute(const char *p)
{
#if defined(_WIN32)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return true;
#endif
    return pal__is_sep(p[0]);
}

/* Copy at most cap - 1 bytes of src[0..n) to out, never splitting a UTF-8
 * sequence, and NUL-terminate. */
static void put_trunc(char *out, size_t cap, const char *src, size_t n)
{
    if (!out || cap == 0u) return;
    if (n > cap - 1u) {
        n = cap - 1u;
        /* back off over continuation bytes, then drop the lead byte when its
         * sequence was cut */
        while (n > 0u && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;
    }
    if (n) memmove(out, src, n);
    out[n] = '\0';
}

void pal_path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t la, lb, n, total;
    char sep = pal_path_sep();
    char *tmp;
    if (!out || cap == 0u) return;
    if (!a) a = "";
    if (!b) b = "";
    if (!*a || is_absolute(b)) {
        put_trunc(out, cap, b, strlen(b));
        return;
    }
    while (pal__is_sep(*b)) b++;
    if (!*b) {
        put_trunc(out, cap, a, strlen(a));
        return;
    }
    /* Keep the separator style of a when it uses only one style. */
    if (strchr(a, '/') && !strchr(a, '\\')) sep = '/';
    else if (strchr(a, '\\') && !strchr(a, '/')) sep = '\\';
    la = strlen(a);
    lb = strlen(b);
    /* Built in a scratch copy because out may alias a or b. */
    tmp = (pc_add_size(la, lb, &total) && pc_add_size(total, 2u, &total))
              ? (char *)malloc(total) : NULL;
    if (!tmp) {
        out[0] = '\0';
        return;
    }
    memcpy(tmp, a, la);
    n = la;
    if (!pal__is_sep(a[la - 1u])) tmp[n++] = sep;
    memcpy(tmp + n, b, lb);
    n += lb;
    put_trunc(out, cap, tmp, n);
    free(tmp);
}

const char *pal_path_basename(const char *path)
{
    const char *base;
    if (!path) return "";
    base = path + root_len(path);
    for (const char *p = base; *p; p++)
        if (pal__is_sep(*p)) base = p + 1;
    return base;
}

void pal_path_dirname(char *out, size_t cap, const char *path)
{
    size_t root, end;
    const char *base;
    if (!out || cap == 0u) return;
    if (!path) path = "";
    root = root_len(path);
    base = pal_path_basename(path);
    end = (size_t)(base - path);
    /* drop the separators between the directory and the base name */
    while (end > root && pal__is_sep(path[end - 1u])) end--;
    put_trunc(out, cap, path, end);
}

const char *pal_path_ext(const char *path)
{
    const char *base = pal_path_basename(path), *dot = NULL;
    for (const char *p = base; *p; p++)
        if (*p == '.') dot = p;
    /* ".hidden" has no extension, "name." has an empty one */
    if (!dot || dot == base) return "";
    return dot + 1;
}

/* ---- glob ------------------------------------------------------------------------ */
static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

/* One pattern p[0..pn) against name. Classic iterative wildcard matching
 * with a single backtrack point for the most recent '*'. */
static bool match_one(const char *p, size_t pn, const char *name)
{
    size_t pi = 0, ni = 0, star_p = (size_t)-1, star_n = 0;
    size_t nn = strlen(name);
    while (ni < nn) {
        if (pi < pn && p[pi] == '*') {
            star_p = pi++;
            star_n = ni;
        } else if (pi < pn && (p[pi] == '?' ||
                               fold((unsigned char)p[pi]) == fold((unsigned char)name[ni]))) {
            pi++;
            ni++;
        } else if (star_p != (size_t)-1) {
            pi = star_p + 1u;
            ni = ++star_n;
        } else {
            return false;
        }
    }
    while (pi < pn && p[pi] == '*') pi++;
    return pi == pn;
}

bool pal__glob_match(const char *glob, const char *name)
{
    const char *p = glob;
    if (!name) return false;
    if (!glob || !*glob) return true;
    for (;;) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0u && match_one(p, len, name)) return true;
        if (!e) return false;
        p = e + 1;
    }
}

int pal__name_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (;; x++, y++) {
        int cx = fold(*x), cy = fold(*y);
        if (cx != cy) return cx < cy ? -1 : 1;
        if (cx == 0) break;
    }
    {
        int c = strcmp(a, b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
}

/* ---- directory listing ----------------------------------------------------------- */
typedef struct list_ctx {
    const char *glob;
    char      **v;
    size_t      n, cap;
} list_ctx;

static bool list_add(void *ud, const char *name)
{
    list_ctx *c = (list_ctx *)ud;
    char *s;
    if (!pal__glob_match(c->glob, name)) return true;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2u : 32u, bytes;
        char **v;
        if (cap > (size_t)INT32_MAX || !pc_mul_size(cap, sizeof *v, &bytes)) return false;
        v = (char **)realloc(c->v, bytes);
        if (!v) return false;
        c->v = v;
        c->cap = cap;
    }
    s = pal__strdup(name);
    if (!s) return false;
    c->v[c->n++] = s;
    return true;
}

static int cmp_names(const void *a, const void *b)
{
    return pal__name_cmp(*(const char *const *)a, *(const char *const *)b);
}

int pal_list_dir(const char *dir, const char *glob, char ***names)
{
    list_ctx c;
    if (names) *names = NULL;
    if (!dir || !names) return 0;
    memset(&c, 0, sizeof c);
    c.glob = glob;
    if (!pal__os_list_dir(dir, list_add, &c)) {
        pal_free_names(c.v, (int)c.n);
        return 0;
    }
    if (c.n == 0u) {
        free(c.v);
        return 0;
    }
    qsort(c.v, c.n, sizeof *c.v, cmp_names);
    *names = c.v;
    return (int)c.n;
}

void pal_free_names(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}
