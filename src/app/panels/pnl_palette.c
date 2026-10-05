/* pnl_palette.c - palette files of the Colors window (lane P, W-COL-PALETTE,
 * W-COL-PALMENU, WINDOWS.md 7.1 and 7.2).
 *
 * Format (Paint.NET compatible, behavior from the MIT 3.36 source, see
 * docs/notice/p.md): UTF-8 text, one color per line, ';' starts a comment,
 * blank and unparsable lines are skipped, a color is a hexadecimal number of
 * up to 8 digits read as AARRGGBB (so 6 digits mean alpha 00), a palette has
 * 96 colors: fewer are padded with white, extra ones are ignored. Files live
 * in the per-user palettes folder (<config dir>/palettes).
 *
 * Thread rules: the parser, formatter and name check are pure (any thread);
 * the folder functions run on the main thread. Ownership as in pnl.h. */
#include "pnl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_FILE_MAX ((uint64_t)1u << 20)

/* WINDOWS.md 7.2: 6 rows of 16, AARRGGBB. */
const uint32_t pnl_default_palette[PNL_PALETTE_N] = {
    0xFF000000u, 0xFF404040u, 0xFFFF0000u, 0xFFFF6A00u, 0xFFFFD800u, 0xFFB6FF00u, 0xFF4CFF00u,
    0xFF00FF21u, 0xFF00FF90u, 0xFF00FFFFu, 0xFF0094FFu, 0xFF0026FFu, 0xFF4800FFu, 0xFFB200FFu,
    0xFFFF00DCu, 0xFFFF006Eu, 0xFFFFFFFFu, 0xFF808080u, 0xFF7F0000u, 0xFF7F3300u, 0xFF7F6A00u,
    0xFF5B7F00u, 0xFF267F00u, 0xFF007F0Eu, 0xFF007F46u, 0xFF007F7Fu, 0xFF004A7Fu, 0xFF00137Fu,
    0xFF21007Fu, 0xFF57007Fu, 0xFF7F006Eu, 0xFF7F0037u, 0xFFA0A0A0u, 0xFF303030u, 0xFFFF7F7Fu,
    0xFFFFB27Fu, 0xFFFFE97Fu, 0xFFDAFF7Fu, 0xFFA5FF7Fu, 0xFF7FFF8Eu, 0xFF7FFFC5u, 0xFF7FFFFFu,
    0xFF7FC9FFu, 0xFF7F92FFu, 0xFFA17FFFu, 0xFFD67FFFu, 0xFFFF7FEDu, 0xFFFF7FB6u, 0xFFC0C0C0u,
    0xFF606060u, 0xFF7F3F3Fu, 0xFF7F593Fu, 0xFF7F743Fu, 0xFF6D7F3Fu, 0xFF527F3Fu, 0xFF3F7F47u,
    0xFF3F7F62u, 0xFF3F7F7Fu, 0xFF3F647Fu, 0xFF3F497Fu, 0xFF503F7Fu, 0xFF6B3F7Fu, 0xFF7F3F76u,
    0xFF7F3F5Bu, 0x80000000u, 0x80404040u, 0x80FF0000u, 0x80FF6A00u, 0x80FFD800u, 0x80B6FF00u,
    0x804CFF00u, 0x8000FF21u, 0x8000FF90u, 0x8000FFFFu, 0x800094FFu, 0x800026FFu, 0x804800FFu,
    0x80B200FFu, 0x80FF00DCu, 0x80FF006Eu, 0x80FFFFFFu, 0x80808080u, 0x807F0000u, 0x807F3300u,
    0x807F6A00u, 0x805B7F00u, 0x80267F00u, 0x80007F0Eu, 0x80007F46u, 0x80007F7Fu, 0x80004A7Fu,
    0x8000137Fu, 0x8021007Fu, 0x8057007Fu, 0x807F006Eu, 0x807F0037u,
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* One line (no newline): comment cut, trimmed, then 1..8 hex digits with an
 * optional 0x prefix. */
static bool parse_line(const char *s, size_t n, uint32_t *out)
{
    size_t b = 0, e;
    uint32_t v = 0;
    for (e = 0; e < n && s[e] != ';'; e++) {
    }
    while (b < e && is_space(s[b])) b++;
    while (e > b && is_space(s[e - 1u])) e--;
    if (e - b >= 2u && s[b] == '0' && (s[b + 1u] == 'x' || s[b + 1u] == 'X')) b += 2u;
    if (e <= b || e - b > 8u) return false;
    for (size_t i = b; i < e; i++) {
        int h = hexval(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

size_t pnl_palette_parse(const char *text, size_t n, uint32_t out[PNL_PALETTE_N])
{
    size_t count = 0, i = 0;
    for (size_t k = 0; k < PNL_PALETTE_N; k++) out[k] = 0xFFFFFFFFu;
    if (!text) return 0;
    if (n >= 3u && (uint8_t)text[0] == 0xEFu && (uint8_t)text[1] == 0xBBu &&
        (uint8_t)text[2] == 0xBFu)
        i = 3u;                                     /* UTF-8 byte order mark */
    while (i < n && count < PNL_PALETTE_N) {
        size_t s = i;
        uint32_t v;
        while (i < n && text[i] != '\n') i++;
        if (parse_line(text + s, i - s, &v)) out[count++] = v;
        if (i < n) i++;                             /* the newline */
    }
    return count;
}

char *pnl_palette_format(const uint32_t pal[PNL_PALETTE_N], size_t *len)
{
    static const char header[] =
        "; paint.c palette file\n"
        "; One color per line as an eight digit hexadecimal number AARRGGBB:\n"
        "; alpha, red, green and blue (alpha FF is opaque, 00 transparent).\n"
        "; Text after a semicolon is ignored. A palette has 96 colors; missing\n"
        "; ones become white and extra ones are ignored.\n";
    size_t hl = sizeof header - 1u, total = hl + (size_t)PNL_PALETTE_N * 9u;
    char *buf = (char *)malloc(total + 1u), *p;
    if (!buf) return NULL;
    memcpy(buf, header, hl);
    p = buf + hl;
    for (size_t i = 0; i < PNL_PALETTE_N; i++) {
        snprintf(p, 10u, "%08X\n", (unsigned)pal[i]);
        p += 9;
    }
    *p = '\0';
    if (len) *len = total;
    return buf;
}

bool pnl_palette_name_valid(const char *name)
{
    static const char *const reserved[] = { "con", "prn", "aux", "nul" };
    size_t n;
    char low[8];
    if (!name) return false;
    n = strlen(name);
    if (n == 0u || n > 120u) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (name[0] == ' ' || name[n - 1u] == ' ' || name[n - 1u] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7Fu || strchr("/\\:*?\"<>|", (int)c)) return false;
    }
    /* Windows device names, with or without an extension */
    {
        size_t k = 0;
        while (k < n && k < sizeof low - 1u && name[k] != '.') {
            char c = name[k];
            low[k] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            k++;
        }
        low[k] = '\0';
        if (k < n && name[k] != '.') return true;    /* longer than any device name */
        for (size_t r = 0; r < sizeof reserved / sizeof reserved[0]; r++)
            if (strcmp(low, reserved[r]) == 0) return false;
        if (k == 4u && (strncmp(low, "com", 3) == 0 || strncmp(low, "lpt", 3) == 0) &&
            low[3] >= '1' && low[3] <= '9')
            return false;
    }
    return true;
}

bool pnl_palettes_dir(const app *a, char *out, size_t cap)
{
    return app_config_path(a, "palettes", out, cap) != NULL;
}

int pnl_palette_list(const app *a, char ***names)
{
    char dir[1024];
    int n;
    *names = NULL;
    if (!pnl_palettes_dir(a, dir, sizeof dir) || !pal_is_dir(dir)) return 0;
    n = pal_list_dir(dir, "*.txt", names);
    for (int i = 0; i < n; i++) {
        size_t l = strlen((*names)[i]);
        if (l >= 4u) (*names)[i][l - 4u] = '\0';    /* drop ".txt" (any case) */
    }
    return n;
}

static bool palette_path(const app *a, const char *name, char *out, size_t cap)
{
    char dir[1024], file[512];
    if (!pnl_palette_name_valid(name) || !pnl_palettes_dir(a, dir, sizeof dir)) return false;
    snprintf(file, sizeof file, "%s.txt", name);
    pal_path_join(out, cap, dir, file);
    return true;
}

pc_status pnl_palette_load(app *a, const char *name)
{
    char path[1600];
    uint8_t *data = NULL;
    size_t len = 0;
    uint32_t pal[PNL_PALETTE_N];
    pc_status st;
    if (!palette_path(a, name, path, sizeof path)) return PC_ERR_ARG;
    st = pal_read_file(path, PALETTE_FILE_MAX, &data, &len);
    if (st != PC_OK) return st;
    (void)pnl_palette_parse((const char *)data, len, pal);
    free(data);
    pnl_colors_set_palette(a, pal);
    return PC_OK;
}

pc_status pnl_palette_save(app *a, const char *name)
{
    char path[1600], dir[1024];
    uint32_t pal[PNL_PALETTE_N];
    size_t len = 0;
    char *text;
    pc_status st;
    if (!palette_path(a, name, path, sizeof path) || !pnl_palettes_dir(a, dir, sizeof dir))
        return PC_ERR_ARG;
    if (!pal_is_dir(dir) && !pal_mkdirs(dir)) return PC_ERR_IO;
    pnl_colors_get_palette(a, pal);
    text = pnl_palette_format(pal, &len);
    if (!text) return PC_ERR_NOMEM;
    st = pal_write_file_atomic(path, text, len);
    free(text);
    return st;
}
