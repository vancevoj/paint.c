/* zip.c - minimal hardened ZIP reader and deterministic writer. See zip.h. */
#include "zip.h"

#include <stdlib.h>
#include <string.h>

#include "zlib.h"

#define SIG_LOCAL   0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD    0x06054b50u
#define SIG_Z64LOC  0x07064b50u
#define RATIO_MIN_SIZE ((uint64_t)1 << 20)

static uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void pc_zip_limits_default(pc_zip_limits *l)
{
    l->max_entries = 4096u;
    l->max_entry_size = (uint64_t)1 << 30;
    l->max_total = (uint64_t)4 << 30;
    l->max_ratio = 256u;
}

/* ---- reader --------------------------------------------------------------------------- */
pc_status pc_zip_open(pc_zip *z, const uint8_t *p, size_t n, const pc_zip_limits *lim)
{
    size_t eocd = 0, pos, names_len = 0, cd_off, cd_size, cd_end;
    uint32_t total;
    bool found = false;
    pc_status st = PC_ERR_FORMAT;
    memset(z, 0, sizeof *z);
    if (!p) return PC_ERR_ARG;
    if (lim) z->lim = *lim; else pc_zip_limits_default(&z->lim);
    if (n < 22u) return PC_ERR_FORMAT;
    /* end record: last match within the final 64 KiB + 22 bytes */
    for (size_t i = n - 22u + 1u; i-- > 0;) {
        if (n - i > 22u + 65535u) break;
        if (r32(p + i) == SIG_EOCD && i + 22u + r16(p + i + 20) <= n) {
            eocd = i;
            found = true;
            break;
        }
    }
    if (!found) return PC_ERR_FORMAT;
    if (eocd >= 20u && r32(p + eocd - 20u) == SIG_Z64LOC) return PC_ERR_UNSUPPORTED;
    if (r16(p + eocd + 4) != 0u || r16(p + eocd + 6) != 0u) return PC_ERR_UNSUPPORTED;
    total = r16(p + eocd + 10);
    if (r16(p + eocd + 8) != total) return PC_ERR_UNSUPPORTED;
    if (total == 0xFFFFu || r32(p + eocd + 12) == 0xFFFFFFFFu || r32(p + eocd + 16) == 0xFFFFFFFFu)
        return PC_ERR_UNSUPPORTED;
    if (total > z->lim.max_entries) return PC_ERR_LIMIT;
    cd_size = r32(p + eocd + 12);
    cd_off = r32(p + eocd + 16);
    if (cd_off > eocd || cd_size > eocd - cd_off) return PC_ERR_FORMAT;
    if ((size_t)total * 46u > cd_size) return PC_ERR_FORMAT;
    cd_end = cd_off + cd_size;
    /* pass 1: walk the directory, size the name arena */
    pos = cd_off;
    for (uint32_t i = 0; i < total; i++) {
        size_t nl, el, cl;
        if (cd_end - pos < 46u || r32(p + pos) != SIG_CENTRAL) return PC_ERR_FORMAT;
        nl = r16(p + pos + 28); el = r16(p + pos + 30); cl = r16(p + pos + 32);
        if (cd_end - pos - 46u < nl + el + cl) return PC_ERR_FORMAT;
        names_len += nl + 1u;
        pos += 46u + nl + el + cl;
    }
    z->e = (pc_zip_entry *)calloc(total ? total : 1u, sizeof *z->e);
    z->names = (char *)malloc(names_len ? names_len : 1u);
    if (!z->e || !z->names) { st = PC_ERR_NOMEM; goto fail; }
    /* pass 2: validate each entry and its local header */
    pos = cd_off;
    names_len = 0;
    for (uint32_t i = 0; i < total; i++) {
        const uint8_t *c = p + pos;
        pc_zip_entry *e = &z->e[i];
        size_t nl = r16(c + 28), el = r16(c + 30), cl = r16(c + 32);
        uint32_t lho = r32(c + 42);
        size_t lnl, lel, data;
        e->flags = (uint16_t)r16(c + 8);
        e->method = (uint16_t)r16(c + 10);
        e->crc = r32(c + 16);
        e->csize = r32(c + 20);
        e->usize = r32(c + 24);
        if (e->flags & 0x41u) { st = PC_ERR_UNSUPPORTED; goto fail; }   /* encrypted */
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu ||
            r16(c + 34) != 0u) {
            st = PC_ERR_UNSUPPORTED;
            goto fail;
        }
        if (nl == 0u || memchr(c + 46, 0, nl)) { st = PC_ERR_FORMAT; goto fail; }
        memcpy(z->names + names_len, c + 46, nl);
        z->names[names_len + nl] = '\0';
        e->name = z->names + names_len;
        names_len += nl + 1u;
        /* local header must precede the central directory */
        if ((size_t)lho > cd_off || cd_off - lho < 30u || r32(p + lho) != SIG_LOCAL) {
            st = PC_ERR_FORMAT;
            goto fail;
        }
        lnl = r16(p + lho + 26);
        lel = r16(p + lho + 28);
        data = (size_t)lho + 30u + lnl + lel;
        if (data > cd_off || e->csize > cd_off - data) { st = PC_ERR_FORMAT; goto fail; }
        if (lnl != nl || memcmp(p + lho + 30, c + 46, nl) != 0) { st = PC_ERR_FORMAT; goto fail; }
        e->data_off = data;
        if (e->method == 0u && e->csize != e->usize) { st = PC_ERR_FORMAT; goto fail; }
        pos += 46u + nl + el + cl;
    }
    z->p = p;
    z->n = n;
    z->count = total;
    return PC_OK;
fail:
    free(z->e);
    free(z->names);
    memset(z, 0, sizeof *z);
    return st;
}

const pc_zip_entry *pc_zip_find(const pc_zip *z, const char *name)
{
    if (!z || !name) return NULL;
    for (uint32_t i = 0; i < z->count; i++)
        if (strcmp(z->e[i].name, name) == 0) return &z->e[i];
    return NULL;
}

pc_status pc_zip_read(pc_zip *z, const pc_zip_entry *e, uint8_t **out, size_t *len)
{
    uint8_t *buf;
    size_t us;
    *out = NULL;
    *len = 0;
    if (!z || !e) return PC_ERR_ARG;
    if (e->method != 0u && e->method != 8u) return PC_ERR_UNSUPPORTED;
    if (e->usize > z->lim.max_entry_size) return PC_ERR_LIMIT;
    if (e->usize > z->lim.max_total || z->extracted > z->lim.max_total - e->usize)
        return PC_ERR_LIMIT;
    if (e->usize > RATIO_MIN_SIZE && e->usize / (e->csize ? e->csize : 1u) >= z->lim.max_ratio)
        return PC_ERR_LIMIT;
    if (e->usize >= (uint64_t)SIZE_MAX) return PC_ERR_LIMIT;
    us = (size_t)e->usize;
    buf = (uint8_t *)malloc(us + 1u);
    if (!buf) return PC_ERR_NOMEM;
    if (e->method == 0u) {
        memcpy(buf, z->p + e->data_off, us);
    } else {
        z_stream s;
        int r;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -15) != Z_OK) { free(buf); return PC_ERR_NOMEM; }
        s.next_in = (Bytef *)(uintptr_t)(z->p + e->data_off);   /* zlib reads only */
        s.avail_in = (uInt)e->csize;
        s.next_out = buf;
        s.avail_out = (uInt)us;
        r = inflate(&s, Z_FINISH);
        if (r == Z_BUF_ERROR && s.avail_out == 0u && us == 0u) r = Z_STREAM_END;
        inflateEnd(&s);
        if (r == Z_MEM_ERROR) { free(buf); return PC_ERR_NOMEM; }
        if (r != Z_STREAM_END || s.total_out != (uLong)us) { free(buf); return PC_ERR_FORMAT; }
    }
    if ((uint32_t)crc32(0L, buf, (uInt)us) != e->crc) { free(buf); return PC_ERR_FORMAT; }
    buf[us] = 0u;
    z->extracted += e->usize;
    *out = buf;
    *len = us;
    return PC_OK;
}

void pc_zip_close(pc_zip *z)
{
    if (!z) return;
    free(z->e);
    free(z->names);
    memset(z, 0, sizeof *z);
}

/* ---- writer ------------------------------------------------------------------------------ */
struct pc_zipw_ent {
    char    *name;
    uint32_t crc, csize, usize, off;
    uint16_t method, flags;
};

#define ZIP_DOS_DATE 0x0021u   /* 1980-01-01 */
#define ZIP_DOS_TIME 0x0000u

void pc_zipw_init(pc_zipw *w, pc_buf *out)
{
    memset(w, 0, sizeof *w);
    w->out = out;
    w->base = out ? out->n : 0u;
    w->st = out ? PC_OK : PC_ERR_ARG;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool name_is_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80u) return false;
    return true;
}

pc_status pc_zipw_add(pc_zipw *w, const char *name, const void *data, size_t len, bool use_deflate)
{
    pc_zipw_ent *e;
    uint8_t lh[30];
    uint8_t *comp = NULL;
    const uint8_t *payload = (const uint8_t *)data;
    size_t nl, off, plen = len;
    uint16_t method = 0;
    if (w->st != PC_OK) return w->st;
    if (!name || (!data && len)) return w->st = PC_ERR_ARG;
    nl = strlen(name);
    if (nl == 0u || nl > 0xFFFFu) return w->st = PC_ERR_ARG;
    if (len > 0xFFFFFFFEu || w->count >= 0xFFFEu) return w->st = PC_ERR_LIMIT;
    off = w->out->n - w->base;
    if (off > 0xFFFFFFFEu) return w->st = PC_ERR_LIMIT;
    if (w->count == w->cap) {
        uint32_t nc = w->cap ? w->cap * 2u : 16u;
        pc_zipw_ent *ne = (pc_zipw_ent *)realloc(w->e, (size_t)nc * sizeof *ne);
        if (!ne) return w->st = PC_ERR_NOMEM;
        w->e = ne;
        w->cap = nc;
    }
    if (use_deflate && len > 0u) {
        z_stream s;
        uLong bound;
        memset(&s, 0, sizeof s);
        if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return w->st = PC_ERR_NOMEM;
        bound = deflateBound(&s, (uLong)len);
        comp = (uint8_t *)malloc(bound);
        if (!comp) { deflateEnd(&s); return w->st = PC_ERR_NOMEM; }
        s.next_in = (Bytef *)(uintptr_t)data;
        s.avail_in = (uInt)len;
        s.next_out = comp;
        s.avail_out = (uInt)bound;
        if (deflate(&s, Z_FINISH) != Z_STREAM_END) {
            deflateEnd(&s);
            free(comp);
            return w->st = PC_ERR_STATE;
        }
        if (s.total_out < len) {
            payload = comp;
            plen = s.total_out;
            method = 8;
        }
        deflateEnd(&s);
    }
    e = &w->e[w->count];
    memset(e, 0, sizeof *e);
    e->name = (char *)malloc(nl + 1u);
    if (!e->name) { free(comp); return w->st = PC_ERR_NOMEM; }
    memcpy(e->name, name, nl + 1u);
    e->crc = (uint32_t)crc32(0L, (const Bytef *)data, (uInt)len);
    e->csize = (uint32_t)plen;
    e->usize = (uint32_t)len;
    e->off = (uint32_t)off;
    e->method = method;
    e->flags = name_is_ascii(name) ? 0u : 0x0800u;
    put32(lh, SIG_LOCAL);
    put16(lh + 4, method ? 20u : 10u);
    put16(lh + 6, e->flags);
    put16(lh + 8, method);
    put16(lh + 10, ZIP_DOS_TIME);
    put16(lh + 12, ZIP_DOS_DATE);
    put32(lh + 14, e->crc);
    put32(lh + 18, e->csize);
    put32(lh + 22, e->usize);
    put16(lh + 26, (uint32_t)nl);
    put16(lh + 28, 0u);
    w->st = pc_buf_append(w->out, lh, sizeof lh);
    if (w->st == PC_OK) w->st = pc_buf_append(w->out, name, nl);
    if (w->st == PC_OK) w->st = pc_buf_append(w->out, payload, plen);
    free(comp);
    if (w->st != PC_OK) { free(e->name); return w->st; }
    w->count++;
    return PC_OK;
}

pc_status pc_zipw_finish(pc_zipw *w)
{
    size_t cd_off, cd_size;
    uint8_t eocd[22];
    if (w->st != PC_OK) return w->st;
    cd_off = w->out->n - w->base;
    for (uint32_t i = 0; i < w->count && w->st == PC_OK; i++) {
        const pc_zipw_ent *e = &w->e[i];
        uint8_t ch[46];
        size_t nl = strlen(e->name);
        memset(ch, 0, sizeof ch);
        put32(ch, SIG_CENTRAL);
        put16(ch + 4, 20u);                       /* made by: MS-DOS, 2.0 */
        put16(ch + 6, e->method ? 20u : 10u);
        put16(ch + 8, e->flags);
        put16(ch + 10, e->method);
        put16(ch + 12, ZIP_DOS_TIME);
        put16(ch + 14, ZIP_DOS_DATE);
        put32(ch + 16, e->crc);
        put32(ch + 20, e->csize);
        put32(ch + 24, e->usize);
        put16(ch + 28, (uint32_t)nl);
        put32(ch + 42, e->off);
        w->st = pc_buf_append(w->out, ch, sizeof ch);
        if (w->st == PC_OK) w->st = pc_buf_append(w->out, e->name, nl);
    }
    if (w->st != PC_OK) return w->st;
    cd_size = w->out->n - w->base - cd_off;
    if (cd_off > 0xFFFFFFFEu || cd_size > 0xFFFFFFFEu) return w->st = PC_ERR_LIMIT;
    memset(eocd, 0, sizeof eocd);
    put32(eocd, SIG_EOCD);
    put16(eocd + 8, w->count);
    put16(eocd + 10, w->count);
    put32(eocd + 12, (uint32_t)cd_size);
    put32(eocd + 16, (uint32_t)cd_off);
    w->st = pc_buf_append(w->out, eocd, sizeof eocd);
    return w->st;
}

void pc_zipw_free(pc_zipw *w)
{
    if (!w) return;
    for (uint32_t i = 0; i < w->count; i++) free(w->e[i].name);
    free(w->e);
    w->e = NULL;
    w->count = w->cap = 0;
}
