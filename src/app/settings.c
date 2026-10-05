/* settings.c - persistent key/value settings (see app_settings.h).
 *
 * Robustness (lane I):
 *  - saves are atomic (pal_write_file_atomic) and the previous good file is
 *    kept as "<file>.bak" before it is replaced;
 *  - a damaged file (unreadable, larger than the limit, NUL bytes, not
 *    UTF-8 text) is moved aside as "<file>.corrupt" and the backup is
 *    loaded instead; damaged lines inside a text file are skipped;
 *  - the header comment carries the format version ("# paint.c settings,
 *    format N"): older files are migrated on load, files from a newer
 *    paint.c are read as far as understood and keep their version (and
 *    unknown keys) when saved again. */
#include "app/app_settings.h"
#include "pal/pal.h"

#include <SDL3/SDL_filesystem.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_VERSION 1
#define SETTINGS_MAGIC   "# paint.c settings, format "

/* Lane I internals used by the autosave module (io/autosave.c). */
pc_status app_settings_write_text(const char *path, const char *text, size_t n);
void      app_settings_mark(app_settings *s, bool dirty);

typedef struct entry {
    char *key, *value;
} entry;

struct app_settings {
    entry  *e;
    size_t  n, cap;
    bool    dirty;
    int     version;            /* format of the parsed file, 0 = unknown / none */
};

app_settings *app_settings_create(void)
{
    return (app_settings *)calloc(1u, sizeof(app_settings));
}

static void clear(app_settings *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->e[i].key);
        free(s->e[i].value);
    }
    s->n = 0;
}

void app_settings_destroy(app_settings *s)
{
    if (!s) return;
    clear(s);
    free(s->e);
    free(s);
}

static char *dup_n(const char *p, size_t n)
{
    char *d = (char *)malloc(n + 1u);
    if (!d) return NULL;
    memcpy(d, p, n);
    d[n] = '\0';
    return d;
}

static bool key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

static bool valid_key(const char *k, size_t n)
{
    if (n == 0u || n >= APP_SETTINGS_MAX_KEY) return false;
    for (size_t i = 0; i < n; i++)
        if (!key_char(k[i])) return false;
    return true;
}

static bool valid_value(const char *v, size_t n)
{
    if (n >= APP_SETTINGS_MAX_VALUE) return false;
    for (size_t i = 0; i < n; i++)
        if (v[i] == '\n' || v[i] == '\r' || v[i] == '\0') return false;
    return true;
}

/* Index of key, or the insertion point with *found false. */
static size_t find(const app_settings *s, const char *key, size_t klen, bool *found)
{
    size_t lo = 0, hi = s->n;
    *found = false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        const char *mk = s->e[mid].key;
        size_t ml = strlen(mk), m = ml < klen ? ml : klen;
        int c = memcmp(mk, key, m);
        if (c == 0) c = ml < klen ? -1 : (ml > klen ? 1 : 0);
        if (c == 0) { *found = true; return mid; }
        if (c < 0) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

static bool set_n(app_settings *s, const char *key, size_t klen, const char *value, size_t vlen)
{
    bool found;
    size_t i;
    char *nv;
    if (!valid_key(key, klen) || !valid_value(value, vlen)) return false;
    i = find(s, key, klen, &found);
    if (found) {
        if (strlen(s->e[i].value) == vlen && memcmp(s->e[i].value, value, vlen) == 0) return true;
        nv = dup_n(value, vlen);
        if (!nv) return false;
        free(s->e[i].value);
        s->e[i].value = nv;
        s->dirty = true;
        return true;
    }
    if (s->n >= APP_SETTINGS_MAX_KEYS) return false;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2u : 64u;
        entry *ne = (entry *)realloc(s->e, nc * sizeof *ne);
        if (!ne) return false;
        s->e = ne;
        s->cap = nc;
    }
    {
        char *k = dup_n(key, klen);
        nv = dup_n(value, vlen);
        if (!k || !nv) { free(k); free(nv); return false; }
        memmove(&s->e[i + 1u], &s->e[i], (s->n - i) * sizeof *s->e);
        s->e[i].key = k;
        s->e[i].value = nv;
        s->n++;
    }
    s->dirty = true;
    return true;
}

size_t app_settings_parse(app_settings *s, const char *text, size_t n)
{
    size_t pos = 0, accepted = 0;
    char section[APP_SETTINGS_MAX_KEY];
    size_t slen = 0;
    if (!s) return 0;
    clear(s);
    if (!text) n = 0;
    if (n > APP_SETTINGS_MAX_FILE) n = APP_SETTINGS_MAX_FILE;
    /* UTF-8 byte order mark */
    if (n >= 3u && (uint8_t)text[0] == 0xEFu && (uint8_t)text[1] == 0xBBu &&
        (uint8_t)text[2] == 0xBFu)
        pos = 3u;
    section[0] = '\0';
    s->version = 0;
    if (n - pos > sizeof SETTINGS_MAGIC - 1u &&
        memcmp(text + pos, SETTINGS_MAGIC, sizeof SETTINGS_MAGIC - 1u) == 0) {
        size_t q = pos + sizeof SETTINGS_MAGIC - 1u;
        int v = 0;
        while (q < n && text[q] >= '0' && text[q] <= '9' && v < 100000) {
            v = v * 10 + (text[q] - '0');
            q++;
        }
        s->version = v;
    }
    while (pos < n) {
        size_t a = pos, b, e, eq;
        while (pos < n && text[pos] != '\n') pos++;
        e = pos;
        if (pos < n) pos++;
        if (e > a && text[e - 1u] == '\r') e--;
        while (a < e && (text[a] == ' ' || text[a] == '\t')) a++;
        if (a >= e || text[a] == '#' || text[a] == ';') continue;
        if (text[a] == '[') {
            b = a + 1u;
            while (b < e && text[b] != ']') b++;
            slen = b - (a + 1u);
            if (b >= e || slen >= sizeof section - 1u ||
                (slen && !valid_key(text + a + 1u, slen))) {
                slen = 0;
                section[0] = '\0';
                continue;
            }
            memcpy(section, text + a + 1u, slen);
            section[slen] = '\0';
            continue;
        }
        eq = a;
        while (eq < e && text[eq] != '=') eq++;
        if (eq >= e) continue;
        b = eq;
        while (b > a && (text[b - 1u] == ' ' || text[b - 1u] == '\t')) b--;
        {
            char key[APP_SETTINGS_MAX_KEY * 2u];
            size_t kl = 0, vs = eq + 1u;
            if (b - a >= APP_SETTINGS_MAX_KEY) continue;
            if (slen) {
                memcpy(key, section, slen);
                key[slen] = '.';
                kl = slen + 1u;
            }
            memcpy(key + kl, text + a, b - a);
            kl += b - a;
            while (vs < e && (text[vs] == ' ' || text[vs] == '\t')) vs++;
            {
                size_t ve = e;
                while (ve > vs && (text[ve - 1u] == ' ' || text[ve - 1u] == '\t')) ve--;
                if (memchr(text + vs, '\0', ve - vs)) continue;
                if (set_n(s, key, kl, text + vs, ve - vs)) accepted++;
            }
        }
    }
    s->dirty = false;
    return accepted;
}

pc_status app_settings_serialize(const app_settings *s, char **out, size_t *len)
{
    char header[96];
    size_t total, at;
    char *buf;
    *out = NULL;
    if (len) *len = 0;
    /* the format version; a newer file keeps its own */
    total = (size_t)snprintf(header, sizeof header, "%s%d (written by the app; edits are kept)\n",
                             SETTINGS_MAGIC, s->version > SETTINGS_VERSION ? s->version
                                                                           : SETTINGS_VERSION);
    for (size_t i = 0; i < s->n; i++) {
        size_t line;
        if (!pc_add_size(strlen(s->e[i].key), strlen(s->e[i].value), &line) ||
            !pc_add_size(line, 2u, &line) || !pc_add_size(total, line, &total))
            return PC_ERR_LIMIT;
    }
    buf = (char *)malloc(total + 1u);
    if (!buf) return PC_ERR_NOMEM;
    at = strlen(header);
    memcpy(buf, header, at);
    for (size_t i = 0; i < s->n; i++) {
        size_t kl = strlen(s->e[i].key), vl = strlen(s->e[i].value);
        memcpy(buf + at, s->e[i].key, kl);
        at += kl;
        buf[at++] = '=';
        memcpy(buf + at, s->e[i].value, vl);
        at += vl;
        buf[at++] = '\n';
    }
    buf[at] = '\0';
    *out = buf;
    if (len) *len = at;
    return PC_OK;
}

/* A settings file must be UTF-8 text without NUL bytes. */
static bool looks_like_text(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t k, need;
        if (c == 0u) return false;
        if (c < 0x80u) { i++; continue; }
        if (c >= 0xC2u && c <= 0xDFu) need = 1u;
        else if (c >= 0xE0u && c <= 0xEFu) need = 2u;
        else if (c >= 0xF0u && c <= 0xF4u) need = 3u;
        else return false;
        for (k = 1u; k <= need; k++)
            if (i + k >= n || (p[i + k] & 0xC0u) != 0x80u) return false;
        i += need + 1u;
    }
    return true;
}

/* Read and validate a settings file. PC_ERR_FORMAT for damaged content. */
static pc_status read_text(const char *path, uint8_t **data, size_t *n)
{
    pc_status st = pal_read_file(path, APP_SETTINGS_MAX_FILE, data, n);
    if (st == PC_ERR_LIMIT) return PC_ERR_FORMAT;
    if (st != PC_OK) return st;
    if (!looks_like_text(*data, *n)) {
        free(*data);
        *data = NULL;
        return PC_ERR_FORMAT;
    }
    return PC_OK;
}

static void bak_path(char *out, size_t cap, const char *path, const char *suffix)
{
    snprintf(out, cap, "%s%s", path, suffix);
}

/* Upgrade older files in place (version 0: files written before versioning
 * have the same keys, nothing to change). */
static void migrate(app_settings *s)
{
    if (s->version > SETTINGS_VERSION) {
        pal_log(PAL_LOG_INFO, "settings: written by a newer paint.c (format %d); unknown "
                "entries are kept", s->version);
        return;
    }
    /* format 0 (no header) and 1 share their keys: nothing to convert yet */
}

pc_status app_settings_load(app_settings *s, const char *path)
{
    uint8_t *data = NULL;
    size_t n = 0;
    pc_status st;
    char other[1100];
    bool from_bak = false;
    if (!s || !path) return PC_ERR_ARG;
    st = pal_file_exists(path) ? read_text(path, &data, &n) : PC_ERR_IO;
    if (st == PC_ERR_FORMAT) {
        /* keep the damaged file for diagnosis, then fall back to the backup */
        bak_path(other, sizeof other, path, ".corrupt");
        (void)pal_remove(other);
        if (!SDL_RenamePath(path, other)) (void)pal_remove(path);
        pal_log(PAL_LOG_WARN, "settings: %s is damaged; moved to %s", path, other);
    }
    if (st != PC_OK) {
        bak_path(other, sizeof other, path, ".bak");
        if (pal_file_exists(other) && read_text(other, &data, &n) == PC_OK) {
            from_bak = true;
            st = PC_OK;
            pal_log(PAL_LOG_INFO, "settings: using the backup %s", other);
        } else {
            app_settings_parse(s, "", 0u);       /* defaults */
            return PC_OK;                        /* a missing file is not an error */
        }
    }
    app_settings_parse(s, (const char *)data, n);
    free(data);
    migrate(s);
    s->dirty = from_bak;                         /* rewrite the main file from the backup */
    return st;
}

pc_status app_settings_write_text(const char *path, const char *text, size_t n)
{
    char bak[1100];
    uint8_t *old = NULL;
    size_t on = 0;
    if (!path || !text) return PC_ERR_ARG;
    /* keep the previous good file as the backup (only when it differs) */
    if (pal_file_exists(path) && read_text(path, &old, &on) == PC_OK) {
        if (on != n || memcmp(old, text, n) != 0) {
            bak_path(bak, sizeof bak, path, ".bak");
            (void)pal_write_file_atomic(bak, old, on);
        }
        free(old);
    } else {
        char dir[1100];
        pal_path_dirname(dir, sizeof dir, path);
        if (dir[0] && !pal_is_dir(dir)) (void)pal_mkdirs(dir);
    }
    return pal_write_file_atomic(path, text, n);
}

void app_settings_mark(app_settings *s, bool dirty)
{
    if (s) s->dirty = dirty;
}

pc_status app_settings_save(const app_settings *s, const char *path)
{
    char *text = NULL;
    size_t n = 0;
    pc_status st;
    if (!s || !path) return PC_ERR_ARG;
    st = app_settings_serialize(s, &text, &n);
    if (st != PC_OK) return st;
    st = app_settings_write_text(path, text, n);
    free(text);
    if (st == PC_OK) ((app_settings *)s)->dirty = false;
    return st;
}

const char *app_settings_get(const app_settings *s, const char *key)
{
    bool found;
    size_t i;
    if (!s || !key) return NULL;
    i = find(s, key, strlen(key), &found);
    return found ? s->e[i].value : NULL;
}

bool app_settings_set(app_settings *s, const char *key, const char *value)
{
    if (!s || !key || !value) return false;
    return set_n(s, key, strlen(key), value, strlen(value));
}

bool app_settings_remove(app_settings *s, const char *key)
{
    bool found;
    size_t i;
    if (!s || !key) return false;
    i = find(s, key, strlen(key), &found);
    if (!found) return false;
    free(s->e[i].key);
    free(s->e[i].value);
    memmove(&s->e[i], &s->e[i + 1u], (s->n - i - 1u) * sizeof *s->e);
    s->n--;
    s->dirty = true;
    return true;
}

size_t app_settings_count(const app_settings *s) { return s ? s->n : 0u; }

bool app_settings_at(const app_settings *s, size_t i, const char **key, const char **value)
{
    if (!s || i >= s->n) return false;
    if (key) *key = s->e[i].key;
    if (value) *value = s->e[i].value;
    return true;
}

bool app_settings_dirty(const app_settings *s) { return s && s->dirty; }

int64_t app_settings_int(const app_settings *s, const char *key, int64_t def)
{
    const char *v = app_settings_get(s, key);
    char *end;
    long long x;
    if (!v || !*v) return def;
    errno = 0;
    x = strtoll(v, &end, 10);
    if (errno || *end) return def;
    return (int64_t)x;
}

/* Locale independent decimal parse: [-]digits[.digits], at most 18
 * significant digits (exact for the values app_settings_set_double writes). */
double app_settings_double(const app_settings *s, const char *key, double def)
{
    const char *v = app_settings_get(s, key);
    uint64_t mant = 0;
    int digits = 0, frac = 0;
    bool neg = false, any = false, dot = false;
    double r;
    if (!v || !*v) return def;
    if (*v == '-') { neg = true; v++; }
    for (; *v; v++) {
        if (*v == '.' && !dot) { dot = true; continue; }
        if (*v < '0' || *v > '9') return def;
        if (digits >= 18) {
            if (!dot) return def;               /* too large */
            continue;                           /* extra fraction digits: ignored */
        }
        mant = mant * 10u + (uint64_t)(*v - '0');
        if (mant) digits++;
        if (dot) frac++;
        any = true;
    }
    if (!any) return def;
    r = (double)mant;
    {
        double p10 = 1.0;
        for (int i = 0; i < frac; i++) p10 *= 10.0;
        r /= p10;
    }
    return neg ? -r : r;
}

bool app_settings_bool(const app_settings *s, const char *key, bool def)
{
    const char *v = app_settings_get(s, key);
    if (!v) return def;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0) return true;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0) return false;
    return def;
}

bool app_settings_set_int(app_settings *s, const char *key, int64_t v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return app_settings_set(s, key, buf);
}

/* Locale independent: fixed point with up to 6 decimals, trailing zeros
 * removed. */
bool app_settings_set_double(app_settings *s, const char *key, double v)
{
    char buf[64];
    double av = v < 0.0 ? -v : v;
    long long ip;
    long long fp;
    size_t n;
    if (!(av == av) || av > 9e15) return false;
    ip = (long long)av;
    fp = (long long)((av - (double)ip) * 1000000.0 + 0.5);
    if (fp >= 1000000) { ip++; fp -= 1000000; }
    n = (size_t)snprintf(buf, sizeof buf, "%s%lld.%06lld", v < 0.0 ? "-" : "", ip, fp);
    while (n > 0u && buf[n - 1u] == '0') buf[--n] = '\0';
    if (n > 0u && buf[n - 1u] == '.') buf[--n] = '\0';
    if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
    return app_settings_set(s, key, buf);
}

bool app_settings_set_bool(app_settings *s, const char *key, bool v)
{
    return app_settings_set(s, key, v ? "1" : "0");
}
