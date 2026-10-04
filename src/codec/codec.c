/* codec.c - codec registry, byte buffers, bounded readers, limits. */
#include "pc/pc_codec.h"

#include <stdlib.h>
#include <string.h>

/* ---- registry ------------------------------------------------------------- */
#define PC_CODEC(id) extern const pc_codec pc_codec_##id;
#include "pc_codec_list.inc"
#undef PC_CODEC

static const pc_codec *const k_codecs[] = {
#define PC_CODEC(id) &pc_codec_##id,
#include "pc_codec_list.inc"
#undef PC_CODEC
    NULL
};
#define N_CODECS (sizeof k_codecs / sizeof k_codecs[0] - 1u)

static const pc_codec *g_sorted[N_CODECS + 1u];
static size_t g_n_sorted = (size_t)-1;

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || x == 0) return x - y;
    }
}

/* Sorted on first use. The registry is initialized from the main thread at
 * startup (pc_codec_list) before workers can call it. */
const pc_codec *const *pc_codec_list(size_t *n)
{
    if (g_n_sorted == (size_t)-1) {
        size_t k = 0;
        for (size_t i = 0; k_codecs[i]; i++) g_sorted[k++] = k_codecs[i];
        for (size_t i = 1; i < k; i++) {          /* insertion sort by name */
            const pc_codec *c = g_sorted[i];
            size_t j = i;
            while (j > 0 && ci_cmp(g_sorted[j - 1]->name, c->name) > 0) {
                g_sorted[j] = g_sorted[j - 1];
                j--;
            }
            g_sorted[j] = c;
        }
        g_sorted[k] = NULL;
        g_n_sorted = k;
    }
    if (n) *n = g_n_sorted;
    return g_sorted;
}

const pc_codec *pc_codec_by_id(const char *id)
{
    size_t n;
    const pc_codec *const *l = pc_codec_list(&n);
    if (!id) return NULL;
    for (size_t i = 0; i < n; i++)
        if (ci_cmp(l[i]->id, id) == 0) return l[i];
    return NULL;
}

static bool ext_in_list(const char *list, const char *ext)
{
    size_t el = strlen(ext);
    const char *p = list;
    while (p && *p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == el) {
            size_t i = 0;
            while (i < len && lower((unsigned char)p[i]) == lower((unsigned char)ext[i])) i++;
            if (i == len) return true;
        }
        p = e ? e + 1 : NULL;
    }
    return false;
}

const pc_codec *pc_codec_by_ext(const char *ext)
{
    size_t n;
    const pc_codec *const *l = pc_codec_list(&n);
    if (!ext) return NULL;
    if (*ext == '.') ext++;
    for (size_t i = 0; i < n; i++)
        if (ext_in_list(l[i]->exts, ext)) return l[i];
    return NULL;
}

const pc_codec *pc_codec_sniff(const uint8_t *p, size_t n)
{
    size_t k;
    const pc_codec *const *l = pc_codec_list(&k);
    for (size_t i = 0; i < k; i++)
        if (l[i]->sniff && (l[i]->flags & PC_CODEC_LOAD) && l[i]->sniff(p, n)) return l[i];
    return NULL;
}

void pc_codec_default_params(const pc_codec *c, void *params)
{
    if (!c || !params || !c->params_size) return;
    memset(params, 0, c->params_size);
    for (uint32_t i = 0; i < c->n_props; i++) {
        const fx_prop *pr = &c->props[i];
        uint8_t *dst = (uint8_t *)params + pr->offset;
        switch (pr->kind) {
        case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED: {
            int32_t v = (int32_t)pr->def;
            memcpy(dst, &v, sizeof v);
            break;
        }
        case FXP_REAL: case FXP_ANGLE: {
            double v = pr->def;
            memcpy(dst, &v, sizeof v);
            break;
        }
        case FXP_POINT: {
            double v[2];
            v[0] = pr->def; v[1] = pr->def;
            memcpy(dst, v, sizeof v);
            break;
        }
        case FXP_COLOR: {
            uint32_t v = pr->def < 0 ? 0xFF000000u : (uint32_t)pr->def;
            memcpy(dst, &v, sizeof v);
            break;
        }
        default: break;
        }
    }
}

static const char *path_ext(const char *path)
{
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') dot = NULL;
        else if (*p == '.') dot = p;
    }
    return dot ? dot + 1 : NULL;
}

pc_status pc_codec_load_any(const uint8_t *p, size_t n, const char *path_hint,
                            const pc_codec_limits *lim, pc_doc **out,
                            pc_image_meta *meta, const pc_codec **used)
{
    pc_codec_limits dl;
    const pc_codec *c = pc_codec_sniff(p, n);
    pc_status st;
    if (out) *out = NULL;
    if (used) *used = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (!c && path_hint) {
        const char *e = path_ext(path_hint);
        c = e ? pc_codec_by_ext(e) : NULL;
        if (c && !(c->flags & PC_CODEC_LOAD)) c = NULL;
    }
    if (!c) return PC_ERR_UNSUPPORTED;
    st = c->load(p, n, lim, out, meta);
    if (st == PC_OK && used) *used = c;
    return st;
}

/* ---- limits and metadata -------------------------------------------------- */
void pc_codec_limits_default(pc_codec_limits *l)
{
    l->max_w = PC_MAX_DIM;
    l->max_h = PC_MAX_DIM;
    l->max_pixels = (uint64_t)1 << 30;
    l->max_mem = (uint64_t)4 << 30;
    l->max_layers = 1024u;
}

pc_status pc_codec_check_size(const pc_codec_limits *l, uint64_t w, uint64_t h,
                              uint32_t layers)
{
    if (w == 0u || h == 0u) return PC_ERR_FORMAT;
    if (w > l->max_w || h > l->max_h || w > PC_MAX_DIM || h > PC_MAX_DIM) return PC_ERR_LIMIT;
    if (w * h > l->max_pixels) return PC_ERR_LIMIT;           /* no overflow: both <= 65535 */
    if (layers > l->max_layers) return PC_ERR_LIMIT;
    if ((w * h * 4u) * (uint64_t)(layers ? layers : 1u) > l->max_mem) return PC_ERR_LIMIT;
    return PC_OK;
}

void pc_meta_free(pc_image_meta *m)
{
    if (!m) return;
    free(m->icc);
    memset(m, 0, sizeof *m);
}

/* ---- buffers --------------------------------------------------------------- */
pc_status pc_buf_reserve(pc_buf *b, size_t extra)
{
    size_t need, cap;
    uint8_t *p;
    if (!pc_add_size(b->n, extra, &need)) return PC_ERR_LIMIT;
    if (need <= b->cap) return PC_OK;
    cap = b->cap ? b->cap : 4096u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) { cap = need; break; }
        cap *= 2u;
    }
    p = (uint8_t *)realloc(b->p, cap);
    if (!p) return PC_ERR_NOMEM;
    b->p = p;
    b->cap = cap;
    return PC_OK;
}

pc_status pc_buf_append(pc_buf *b, const void *data, size_t n)
{
    pc_status st;
    if (n == 0u) return PC_OK;
    st = pc_buf_reserve(b, n);
    if (st != PC_OK) return st;
    memcpy(b->p + b->n, data, n);
    b->n += n;
    return PC_OK;
}

pc_status pc_buf_put_u8(pc_buf *b, uint8_t v) { return pc_buf_append(b, &v, 1u); }
pc_status pc_buf_put_le16(pc_buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return pc_buf_append(b, t, 2u);
}
pc_status pc_buf_put_le32(pc_buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return pc_buf_append(b, t, 4u);
}
pc_status pc_buf_put_be16(pc_buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return pc_buf_append(b, t, 2u);
}
pc_status pc_buf_put_be32(pc_buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return pc_buf_append(b, t, 4u);
}

void pc_buf_free(pc_buf *b)
{
    if (!b) return;
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* ---- bounded reader --------------------------------------------------------- */
static const uint8_t *rd_take(pc_rd *r, size_t n)
{
    const uint8_t *p;
    if (r->err || n > r->n - r->pos) { r->err = true; return NULL; }
    p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t pc_rd_u8(pc_rd *r)
{
    const uint8_t *p = rd_take(r, 1u);
    return p ? p[0] : 0u;
}
uint16_t pc_rd_le16(pc_rd *r)
{
    const uint8_t *p = rd_take(r, 2u);
    return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0u;
}
uint32_t pc_rd_le32(pc_rd *r)
{
    const uint8_t *p = rd_take(r, 4u);
    return p ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[3] << 24)) : 0u;
}
uint64_t pc_rd_le64(pc_rd *r)
{
    uint64_t lo = pc_rd_le32(r), hi = pc_rd_le32(r);
    return lo | (hi << 32);
}
uint16_t pc_rd_be16(pc_rd *r)
{
    const uint8_t *p = rd_take(r, 2u);
    return p ? (uint16_t)((p[0] << 8) | p[1]) : 0u;
}
uint32_t pc_rd_be32(pc_rd *r)
{
    const uint8_t *p = rd_take(r, 4u);
    return p ? (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                (uint32_t)p[3]) : 0u;
}
bool pc_rd_bytes(pc_rd *r, void *dst, size_t n)
{
    const uint8_t *p = rd_take(r, n);
    if (!p) return false;
    if (n) memcpy(dst, p, n);
    return true;
}
bool pc_rd_skip(pc_rd *r, size_t n) { return rd_take(r, n) != NULL || n == 0u; }
bool pc_rd_seek(pc_rd *r, size_t pos)
{
    if (r->err || pos > r->n) { r->err = true; return false; }
    r->pos = pos;
    return true;
}
