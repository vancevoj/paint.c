/* cmeta.c - image metadata shared by the codecs (lane CODEC). See cmeta.h
 * and docs/codecs/meta.md. The TIFF and EXIF structures follow the public
 * TIFF 6.0 and Exif 2.32 specifications, the Photoshop image resource
 * layout follows Adobe's published file format specification, and the XMP
 * handling follows the XMP specification part 3. */
#include "cmeta.h"
#include "pdn.h"

#include <stdlib.h>
#include <string.h>

/* ---- items ---------------------------------------------------------------------- */
static char *str_dup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

pc_status cm_set(pc_image_meta *m, const char *key, const char *value)
{
    if (!m || !key || !value) return PC_ERR_ARG;
    for (size_t i = 0; i < m->n_items; i++)
        if (strcmp(m->items[i].key, key) == 0) {
            char *v = str_dup(value);
            if (!v) return PC_ERR_NOMEM;
            free(m->items[i].value);
            m->items[i].value = v;
            return PC_OK;
        }
    return pc_meta_add(m, key, value);
}

void cm_remove(pc_image_meta *m, const char *key)
{
    size_t o = 0;
    if (!m || !key) return;
    for (size_t i = 0; i < m->n_items; i++) {
        if (strcmp(m->items[i].key, key) == 0) {
            free(m->items[i].key);
            free(m->items[i].value);
            continue;
        }
        m->items[o++] = m->items[i];
    }
    m->n_items = o;
    if (o == 0) {
        free(m->items);
        m->items = NULL;
    }
}

pc_status cm_set_blob(pc_image_meta *m, const char *key, const uint8_t *p, size_t n)
{
    pc_buf b;
    pc_status st;
    if (!m || !key || (!p && n)) return PC_ERR_ARG;
    memset(&b, 0, sizeof b);
    st = pdn_base64_encode(p, n, &b);
    if (st == PC_OK) st = pc_buf_put_u8(&b, 0u);
    if (st == PC_OK) st = cm_set(m, key, (const char *)b.p);
    pc_buf_free(&b);
    return st;
}

pc_status cm_get_blob(const pc_image_meta *m, const char *key, uint8_t **out, size_t *n)
{
    const char *v;
    size_t len, cap;
    uint8_t *p;
    *out = NULL;
    *n = 0;
    v = pc_meta_get(m, key);
    if (!v) return PC_OK;
    len = strlen(v);
    if (len == 0u || len % 4u) return PC_OK;
    cap = len / 4u * 3u;
    p = (uint8_t *)malloc(cap);
    if (!p) return PC_ERR_NOMEM;
    if (!pdn_base64_decode(v, len, p, cap, n) || *n == 0u) {
        free(p);
        *n = 0;
        return PC_OK;
    }
    *out = p;
    return PC_OK;
}

/* ---- text ------------------------------------------------------------------------- */
bool cm_utf8_valid(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c = p[i], need, cp;
        if (c == 0u) return false;
        if (c < 0x80u) { i++; continue; }
        if (c >= 0xC2u && c <= 0xDFu) { need = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0u && c <= 0xEFu) { need = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0u && c <= 0xF4u) { need = 3; cp = c & 0x07u; }
        else return false;
        if (n - i - 1u < need) return false;
        for (uint32_t k = 1; k <= need; k++) {
            uint32_t d = p[i + k];
            if ((d & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if ((need == 2u && cp < 0x800u) || (need == 3u && (cp < 0x10000u || cp > 0x10FFFFu)) ||
            (cp >= 0xD800u && cp <= 0xDFFFu))
            return false;
        i += need + 1u;
    }
    return true;
}

char *cm_latin1_to_utf8(const uint8_t *p, size_t n)
{
    size_t cap;
    char *s;
    size_t o = 0;
    if (!pc_mul_size(n, 2u, &cap) || !pc_add_size(cap, 1u, &cap)) return NULL;
    s = (char *)malloc(cap);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == 0u) continue;
        if (c < 0x80u) {
            s[o++] = (char)c;
        } else {
            s[o++] = (char)(0xC0u | (c >> 6));
            s[o++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    s[o] = '\0';
    return s;
}

char *cm_text_to_utf8(const uint8_t *p, size_t n)
{
    char *s;
    if (cm_utf8_valid(p, n)) {
        s = (char *)malloc(n + 1u);
        if (!s) return NULL;
        memcpy(s, p, n);
        s[n] = '\0';
        return s;
    }
    return cm_latin1_to_utf8(p, n);
}

bool cm_utf8_to_latin1(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t o = 0;
    if (!cm_utf8_valid(p, n)) return false;
    for (size_t i = 0; i < n;) {
        uint32_t c = p[i];
        if (c < 0x80u) {
            i++;
        } else if (c == 0xC2u || c == 0xC3u) {
            c = ((c & 0x1Fu) << 6) | (p[i + 1u] & 0x3Fu);
            i += 2u;
        } else {
            return false;
        }
        if (o >= cap) return false;
        out[o++] = (uint8_t)c;
    }
    *len = o;
    return true;
}

/* ---- EXIF model ------------------------------------------------------------------- */
uint32_t cm_type_size(uint32_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: case 129: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

void cm_exif_init(cm_exif *e)
{
    memset(e, 0, sizeof *e);
}

void cm_exif_free(cm_exif *e)
{
    if (!e) return;
    for (size_t i = 0; i < e->n; i++) free(e->t[i].val);
    free(e->t);
    memset(e, 0, sizeof *e);
}

/* Tags the model never keeps: pointers (rebuilt on output), image
 * structure of the file they came from, and blocks carried separately. */
bool cm_exif_tag_dropped(uint8_t ifd, uint16_t tag)
{
    if (tag == CM_TAG_EXIF_IFD || tag == CM_TAG_GPS_IFD || tag == CM_TAG_INTEROP_IFD)
        return true;
    if (ifd != CM_IFD0) return false;
    if (tag >= 254u && tag <= 266u) return true;       /* subfile .. fill order */
    if (tag >= 273u && tag <= 281u && tag != CM_TAG_ORIENTATION) return true;
    if (tag >= 284u && tag <= 293u) return true;       /* planar .. T6 options */
    if (tag == 301u || tag == 317u || tag == 320u || tag == 321u) return true;
    if (tag >= 322u && tag <= 347u) return true;       /* tiles, sub IFDs, ink, samples */
    if (tag >= 512u && tag <= 530u) return true;       /* old JPEG, YCbCr layout */
    if (tag == 532u) return true;
    if (tag == CM_TAG_XMP || tag == CM_TAG_IPTC || tag == CM_TAG_ICC) return true;
    if (tag == 34377u || tag == 37724u) return true;   /* Photoshop resources, layers */
    if (tag >= 768u && tag <= 771u) return true;       /* GDI+ PNG gamma, sRGB intent */
    if (tag >= 0x5000u && tag <= 0x5FFFu) return true; /* GDI+ thumbnail and PNG data */
    return false;
}

static cm_tag *find_tag(const cm_exif *e, uint8_t ifd, uint16_t tag)
{
    for (size_t i = 0; i < e->n; i++)
        if (e->t[i].ifd == ifd && e->t[i].tag == tag) return &e->t[i];
    return NULL;
}

pc_status cm_exif_set(cm_exif *e, uint8_t ifd, uint16_t tag, uint16_t type, uint32_t count,
                      const void *le_val, uint32_t len)
{
    uint32_t ts = cm_type_size(type);
    cm_tag *t;
    uint8_t *v;
    if (!e || ifd >= CM_IFD_COUNT || !ts || count == 0u || (!le_val && len) ||
        (uint64_t)count * ts != (uint64_t)len)
        return PC_ERR_ARG;
    v = (uint8_t *)malloc(len ? len : 1u);
    if (!v) return PC_ERR_NOMEM;
    if (len) memcpy(v, le_val, len);
    t = find_tag(e, ifd, tag);
    if (!t) {
        if (e->n == e->cap) {
            size_t nc = e->cap ? e->cap * 2u : 32u, bytes;
            cm_tag *nt;
            if (!pc_mul_size(nc, sizeof *nt, &bytes)) { free(v); return PC_ERR_LIMIT; }
            nt = (cm_tag *)realloc(e->t, bytes);
            if (!nt) { free(v); return PC_ERR_NOMEM; }
            e->t = nt;
            e->cap = nc;
        }
        t = &e->t[e->n++];
    } else {
        free(t->val);
    }
    t->tag = tag;
    t->type = type;
    t->count = count;
    t->ifd = ifd;
    t->val = v;
    t->len = len;
    return PC_OK;
}

const cm_tag *cm_exif_find(const cm_exif *e, uint16_t tag)
{
    if (!e) return NULL;
    for (size_t i = 0; i < e->n; i++)
        if (e->t[i].tag == tag) return &e->t[i];
    return NULL;
}

void cm_exif_remove(cm_exif *e, uint16_t tag)
{
    size_t o = 0;
    if (!e) return;
    for (size_t i = 0; i < e->n; i++) {
        if (e->t[i].tag == tag) { free(e->t[i].val); continue; }
        e->t[o++] = e->t[i];
    }
    e->n = o;
}

static uint32_t le16v(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32v(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

int cm_exif_orientation(const cm_exif *e)
{
    const cm_tag *t = cm_exif_find(e, CM_TAG_ORIENTATION);
    uint32_t v;
    if (!t || t->ifd != CM_IFD0 || t->count < 1u) return 1;
    if (t->type == 3u) v = le16v(t->val);
    else if (t->type == 4u) v = le32v(t->val);
    else return 1;
    return v >= 1u && v <= 8u ? (int)v : 1;
}

bool cm_exif_resolution(const cm_exif *e, double *x, double *y)
{
    const cm_tag *tx = cm_exif_find(e, CM_TAG_XRES), *ty = cm_exif_find(e, CM_TAG_YRES);
    const cm_tag *tu = cm_exif_find(e, CM_TAG_RESUNIT);
    double k = 1.0, v[2] = { 0.0, 0.0 };
    uint32_t unit = 2u;
    if (tu && tu->type == 3u) unit = le16v(tu->val);
    if (unit == 3u) k = 2.54;
    else if (unit != 2u) return false;
    for (int i = 0; i < 2; i++) {
        const cm_tag *t = i ? ty : tx;
        uint32_t num, den;
        if (!t || t->type != 5u || t->len < 8u) return false;
        num = le32v(t->val);
        den = le32v(t->val + 4);
        if (!num || !den) return false;
        v[i] = (double)num / (double)den * k;
    }
    *x = v[0];
    *y = v[1];
    return true;
}

size_t cm_exif_count(const cm_exif *e, uint8_t ifd)
{
    size_t k = 0;
    for (size_t i = 0; e && i < e->n; i++) k += e->t[i].ifd == ifd;
    return k;
}

uint8_t cm_exif_ifd_of(uint16_t tag)
{
    if (tag <= 31u) return CM_IFD_GPS;
    if (tag == 33434u || tag == 33437u) return CM_IFD_EXIF;
    if (tag >= 34850u && tag <= 34869u && tag != CM_TAG_GPS_IFD) return CM_IFD_EXIF;
    if (tag >= 36864u && tag <= 37999u) return CM_IFD_EXIF;
    if (tag >= 40960u && tag <= 42999u && tag != CM_TAG_INTEROP_IFD) return CM_IFD_EXIF;
    return CM_IFD0;
}

/* ---- parsing ------------------------------------------------------------------------ */
typedef struct tparse {
    const uint8_t *p;
    size_t         n;
    bool           be;
    uint32_t       flags;
} tparse;

static uint32_t t16(const tparse *t, size_t at)
{
    const uint8_t *q = t->p + at;
    return t->be ? ((uint32_t)q[0] << 8) | q[1] : (uint32_t)q[0] | ((uint32_t)q[1] << 8);
}

static uint32_t t32(const tparse *t, size_t at)
{
    const uint8_t *q = t->p + at;
    return t->be ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]
                 : (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) |
                   ((uint32_t)q[3] << 24);
}

/* Reverse each elem-byte unit of v[0..len). */
static void swap_units(uint8_t *v, uint32_t len, uint32_t elem)
{
    if (elem < 2u) return;
    for (uint32_t i = 0; i + elem <= len; i += elem)
        for (uint32_t a = 0, b = elem - 1u; a < b; a++, b--) {
            uint8_t x = v[i + a];
            v[i + a] = v[i + b];
            v[i + b] = x;
        }
}

/* Big-endian value bytes to little-endian, by type. */
static void to_le(uint16_t tag, uint16_t type, uint8_t *v, uint32_t len)
{
    switch (type) {
    case 3: case 8: swap_units(v, len, 2u); break;
    case 4: case 9: case 11: case 13: case 5: case 10: swap_units(v, len, 4u); break;
    case 12: swap_units(v, len, 8u); break;
    case 7:
        if (tag == CM_TAG_USERCOMMENT && len >= 8u && memcmp(v, "UNICODE\0", 8u) == 0)
            swap_units(v + 8, (len - 8u) & ~1u, 2u);
        break;
    default: break;
    }
}

#define IFD_MAX_ENTRIES 2048u
#define FILE_VALUE_MAX  ((uint32_t)1 << 20)

/* Read the IFD at off into e (as ifd). Pointer values of sub-IFDs are
 * returned in ptr[] (indexed by CM_IFD_*), 0 when absent. */
static pc_status parse_ifd(const tparse *t, uint32_t off, uint8_t ifd, cm_exif *e,
                           uint32_t ptr[CM_IFD_COUNT])
{
    uint32_t count;
    if (off < 8u || (size_t)off > t->n - 2u) return PC_OK;
    count = t16(t, off);
    if ((size_t)count > (t->n - off - 2u) / 12u) count = (uint32_t)((t->n - off - 2u) / 12u);
    if (count > IFD_MAX_ENTRIES) count = IFD_MAX_ENTRIES;
    for (uint32_t i = 0; i < count; i++) {
        size_t en = (size_t)off + 2u + (size_t)i * 12u;
        uint32_t tag = t16(t, en), type = t16(t, en + 2u), cnt = t32(t, en + 4u);
        uint32_t ts = cm_type_size(type);
        uint64_t len = (uint64_t)cnt * ts;
        size_t at;
        uint8_t *v;
        pc_status st;
        if (!ts || cnt == 0u) continue;
        if ((tag == CM_TAG_EXIF_IFD || tag == CM_TAG_GPS_IFD) && ifd == CM_IFD0 &&
            (type == 4u || type == 13u)) {
            ptr[tag == CM_TAG_EXIF_IFD ? CM_IFD_EXIF : CM_IFD_GPS] = t32(t, en + 8u);
            continue;
        }
        if (tag == CM_TAG_INTEROP_IFD && ifd == CM_IFD_EXIF && (type == 4u || type == 13u)) {
            ptr[CM_IFD_INTEROP] = t32(t, en + 8u);
            continue;
        }
        if (type == 13u || cm_exif_tag_dropped(ifd, (uint16_t)tag)) continue;
        if (len > (uint64_t)t->n || len > 0xFFFFFFFFu) continue;
        if ((t->flags & CM_PARSE_TIFF_FILE) && len > FILE_VALUE_MAX) continue;
        if (len <= 4u) {
            at = en + 8u;
        } else {
            uint32_t vo = t32(t, en + 8u);
            if ((size_t)vo > t->n || len > (uint64_t)(t->n - vo)) continue;
            at = vo;
        }
        v = (uint8_t *)malloc((size_t)len);
        if (!v) return PC_ERR_NOMEM;
        memcpy(v, t->p + at, (size_t)len);
        if (t->be) to_le((uint16_t)tag, (uint16_t)type, v, (uint32_t)len);
        st = cm_exif_set(e, ifd, (uint16_t)tag, (uint16_t)type, cnt, v, (uint32_t)len);
        free(v);
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

pc_status cm_exif_parse(const uint8_t *p, size_t n, uint32_t flags, cm_exif *e)
{
    tparse t;
    uint32_t ptr[CM_IFD_COUNT] = { 0, 0, 0, 0 }, seen[CM_IFD_COUNT] = { 0, 0, 0, 0 };
    pc_status st;
    if (!p || !e) return PC_ERR_ARG;
    if (n < 8u) return PC_ERR_FORMAT;
    if (p[0] == 'I' && p[1] == 'I') t.be = false;
    else if (p[0] == 'M' && p[1] == 'M') t.be = true;
    else return PC_ERR_FORMAT;
    t.p = p;
    t.n = n;
    t.flags = flags;
    if (t16(&t, 2) != 42u) return PC_ERR_FORMAT;
    seen[CM_IFD0] = t32(&t, 4);
    st = parse_ifd(&t, seen[CM_IFD0], CM_IFD0, e, ptr);
    /* each sub-IFD once; a pointer back to an IFD already read is ignored */
    for (uint8_t k = CM_IFD_EXIF; k < CM_IFD_COUNT && st == PC_OK; k++) {
        bool dup = false;
        if (!ptr[k]) continue;
        for (uint8_t j = 0; j < k; j++) dup = dup || seen[j] == ptr[k];
        if (dup) continue;
        seen[k] = ptr[k];
        st = parse_ifd(&t, ptr[k], k, e, ptr);
    }
    return st;
}

/* ---- serialization ------------------------------------------------------------------- */
typedef struct oent {
    uint16_t       tag, type;
    uint32_t       count;
    const uint8_t *val;
    uint32_t       len;
    uint8_t        ptr[4];      /* pointer entries: value written here */
    bool           is_ptr;      /* val must point at ptr (re-aimed after sorting) */
} oent;

static int cmp_oent(const void *a, const void *b)
{
    uint16_t x = ((const oent *)a)->tag, y = ((const oent *)b)->tag;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Bytes of an IFD with these entries, including out-of-line values. */
static uint64_t ifd_bytes(const oent *o, size_t n)
{
    uint64_t s = 2u + 12u * (uint64_t)n + 4u;
    for (size_t i = 0; i < n; i++)
        if (o[i].len > 4u) s += (uint64_t)o[i].len + (o[i].len & 1u);
    return s;
}

/* Collect the entries of ifd (plus room for two pointer entries). */
static oent *collect(const cm_exif *e, uint8_t ifd, size_t *n)
{
    size_t k = 0, cnt = cm_exif_count(e, ifd);
    oent *o = (oent *)calloc(cnt + 2u, sizeof *o);
    if (!o) return NULL;
    for (size_t i = 0; i < e->n; i++) {
        const cm_tag *t = &e->t[i];
        if (t->ifd != ifd) continue;
        o[k].tag = t->tag;
        o[k].type = t->type;
        o[k].count = t->count;
        o[k].val = t->val;
        o[k].len = t->len;
        k++;
    }
    *n = k;
    return o;
}

static void add_ptr(oent *o, size_t *n, uint16_t tag, uint32_t value)
{
    o[*n].tag = tag;
    o[*n].type = 4u;
    o[*n].count = 1u;
    put32(o[*n].ptr, value);
    o[*n].val = o[*n].ptr;
    o[*n].len = 4u;
    o[*n].is_ptr = true;
    (*n)++;
}

/* Write an IFD at out->n (offset off relative to base, already even). */
static pc_status emit_ifd(pc_buf *out, uint32_t off, oent *o, size_t n)
{
    uint64_t data = (uint64_t)off + 2u + 12u * (uint64_t)n + 4u;
    pc_status st;
    qsort(o, n, sizeof *o, cmp_oent);
    for (size_t i = 0; i < n; i++)
        if (o[i].is_ptr) o[i].val = o[i].ptr;
    st = pc_buf_put_le16(out, (uint16_t)n);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        uint8_t ent[12];
        put16(ent, o[i].tag);
        put16(ent + 2, o[i].type);
        put32(ent + 4, o[i].count);
        memset(ent + 8, 0, 4u);
        if (o[i].len <= 4u) {
            memcpy(ent + 8, o[i].val, o[i].len);
        } else {
            if (data > 0xFFFFFFFFu) return PC_ERR_LIMIT;
            put32(ent + 8, (uint32_t)data);
            data += (uint64_t)o[i].len + (o[i].len & 1u);
        }
        st = pc_buf_append(out, ent, sizeof ent);
    }
    if (st == PC_OK) st = pc_buf_put_le32(out, 0u);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (o[i].len <= 4u) continue;
        st = pc_buf_append(out, o[i].val, o[i].len);
        if (st == PC_OK && (o[i].len & 1u)) st = pc_buf_put_u8(out, 0u);
    }
    return st;
}

pc_status cm_exif_serialize(const cm_exif *e, pc_buf *out)
{
    oent *o[CM_IFD_COUNT] = { NULL, NULL, NULL, NULL };
    size_t n[CM_IFD_COUNT] = { 0, 0, 0, 0 };
    uint64_t off[CM_IFD_COUNT] = { 0, 0, 0, 0 }, pos;
    pc_status st = PC_OK;
    static const uint8_t hdr[8] = { 'I', 'I', 42, 0, 8, 0, 0, 0 };
    if (!e || !out) return PC_ERR_ARG;
    if (e->n == 0u) return PC_OK;
    for (uint8_t k = 0; k < CM_IFD_COUNT; k++) {
        o[k] = collect(e, k, &n[k]);
        if (!o[k]) { st = PC_ERR_NOMEM; goto done; }
    }
    /* the Exif IFD exists when it or the Interoperability IFD has entries */
    {
        bool has_interop = n[CM_IFD_INTEROP] > 0u;
        bool has_exif = n[CM_IFD_EXIF] > 0u || has_interop;
        bool has_gps = n[CM_IFD_GPS] > 0u;
        size_t n0 = n[CM_IFD0] + (has_exif ? 1u : 0u) + (has_gps ? 1u : 0u);
        size_t ne = n[CM_IFD_EXIF] + (has_interop ? 1u : 0u);
        pos = 8u;
        off[CM_IFD0] = pos;
        pos += ifd_bytes(o[CM_IFD0], n[CM_IFD0]) + 12u * (n0 - n[CM_IFD0]);
        if (has_exif) {
            off[CM_IFD_EXIF] = pos;
            pos += ifd_bytes(o[CM_IFD_EXIF], n[CM_IFD_EXIF]) + 12u * (ne - n[CM_IFD_EXIF]);
        }
        if (has_interop) {
            off[CM_IFD_INTEROP] = pos;
            pos += ifd_bytes(o[CM_IFD_INTEROP], n[CM_IFD_INTEROP]);
        }
        if (has_gps) {
            off[CM_IFD_GPS] = pos;
            pos += ifd_bytes(o[CM_IFD_GPS], n[CM_IFD_GPS]);
        }
        if (pos > 0xFFFFFFFFu) { st = PC_ERR_LIMIT; goto done; }
        if (has_exif) add_ptr(o[CM_IFD0], &n[CM_IFD0], CM_TAG_EXIF_IFD, (uint32_t)off[1]);
        if (has_gps) add_ptr(o[CM_IFD0], &n[CM_IFD0], CM_TAG_GPS_IFD, (uint32_t)off[2]);
        if (has_interop)
            add_ptr(o[CM_IFD_EXIF], &n[CM_IFD_EXIF], CM_TAG_INTEROP_IFD, (uint32_t)off[3]);
        st = pc_buf_reserve(out, (size_t)pos);
        if (st != PC_OK) goto done;
        st = pc_buf_append(out, hdr, sizeof hdr);
        if (st == PC_OK) st = emit_ifd(out, (uint32_t)off[CM_IFD0], o[CM_IFD0], n[CM_IFD0]);
        if (st == PC_OK && has_exif)
            st = emit_ifd(out, (uint32_t)off[CM_IFD_EXIF], o[CM_IFD_EXIF], n[CM_IFD_EXIF]);
        if (st == PC_OK && has_interop)
            st = emit_ifd(out, (uint32_t)off[CM_IFD_INTEROP], o[CM_IFD_INTEROP],
                          n[CM_IFD_INTEROP]);
        if (st == PC_OK && has_gps)
            st = emit_ifd(out, (uint32_t)off[CM_IFD_GPS], o[CM_IFD_GPS], n[CM_IFD_GPS]);
    }
done:
    for (uint8_t k = 0; k < CM_IFD_COUNT; k++) free(o[k]);
    return st;
}

pc_status cm_exif_write_subifd(const cm_exif *e, uint8_t ifd, pc_buf *out, size_t base,
                               uint32_t *off)
{
    oent *o = NULL, *oi = NULL;
    size_t n = 0, ni = 0;
    uint64_t at, iat = 0;
    pc_status st = PC_OK;
    if (!e || !out || !off || (ifd != CM_IFD_EXIF && ifd != CM_IFD_GPS) || out->n < base)
        return PC_ERR_ARG;
    *off = 0;
    o = collect(e, ifd, &n);
    if (!o) return PC_ERR_NOMEM;
    if (ifd == CM_IFD_EXIF) {
        oi = collect(e, CM_IFD_INTEROP, &ni);
        if (!oi) { free(o); return PC_ERR_NOMEM; }
    }
    if (n == 0u && ni == 0u) goto done;
    if ((out->n - base) & 1u) {
        st = pc_buf_put_u8(out, 0u);
        if (st != PC_OK) goto done;
    }
    at = out->n - base;
    if (ni) {
        iat = at + ifd_bytes(o, n) + 12u;
        if (iat > 0xFFFFFFFFu) { st = PC_ERR_LIMIT; goto done; }
        add_ptr(o, &n, CM_TAG_INTEROP_IFD, (uint32_t)iat);
    }
    if (at + ifd_bytes(o, n) + ifd_bytes(oi, ni) > 0xFFFFFFFFu) { st = PC_ERR_LIMIT; goto done; }
    st = emit_ifd(out, (uint32_t)at, o, n);
    if (st == PC_OK && ni) st = emit_ifd(out, (uint32_t)iat, oi, ni);
    if (st == PC_OK) *off = (uint32_t)at;
done:
    free(o);
    free(oi);
    return st;
}

/* ---- text tags ------------------------------------------------------------------------- */
static const char k_uc_ascii[8] = { 'A', 'S', 'C', 'I', 'I', 0, 0, 0 };
static const char k_uc_unicode[8] = { 'U', 'N', 'I', 'C', 'O', 'D', 'E', 0 };

pc_status cm_exif_set_text(cm_exif *e, uint16_t tag, const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0u;
    uint8_t ifd = cm_exif_ifd_of(tag);
    pc_status st;
    if (!e) return PC_ERR_ARG;
    if (n == 0u) {
        cm_exif_remove(e, tag);
        return PC_OK;
    }
    if (n > CM_TEXT_MAX || !cm_utf8_valid((const uint8_t *)utf8, n)) return PC_ERR_ARG;
    if (tag == CM_TAG_USERCOMMENT) {
        bool ascii = true;
        uint8_t *v;
        size_t len = 8u, o = 8u;
        for (size_t i = 0; i < n && ascii; i++) ascii = (uint8_t)utf8[i] < 0x80u;
        len += ascii ? n : n * 2u;            /* UTF-16 never needs more units than bytes */
        v = (uint8_t *)malloc(len);
        if (!v) return PC_ERR_NOMEM;
        memcpy(v, ascii ? k_uc_ascii : k_uc_unicode, 8u);
        if (ascii) {
            memcpy(v + 8, utf8, n);
            o = len;
        } else {
            const uint8_t *p = (const uint8_t *)utf8;
            for (size_t i = 0; i < n;) {
                uint32_t c = p[i], cp, k;
                if (c < 0x80u) { cp = c; k = 1; }
                else if (c < 0xE0u) { cp = c & 0x1Fu; k = 2; }
                else if (c < 0xF0u) { cp = c & 0x0Fu; k = 3; }
                else { cp = c & 0x07u; k = 4; }
                for (uint32_t j = 1; j < k; j++) cp = (cp << 6) | (p[i + j] & 0x3Fu);
                i += k;
                if (cp >= 0x10000u) {
                    uint32_t s = cp - 0x10000u;
                    put16(v + o, 0xD800u | (s >> 10));
                    put16(v + o + 2, 0xDC00u | (s & 0x3FFu));
                    o += 4u;
                } else {
                    put16(v + o, cp);
                    o += 2u;
                }
            }
        }
        st = cm_exif_set(e, ifd, tag, 7u, (uint32_t)o, v, (uint32_t)o);
        free(v);
        return st;
    }
    return cm_exif_set(e, ifd, tag, 2u, (uint32_t)(n + 1u), utf8, (uint32_t)(n + 1u));
}

/* Trim trailing NUL bytes and spaces. */
static size_t trim_len(const uint8_t *p, size_t n)
{
    while (n && (p[n - 1u] == 0u || p[n - 1u] == ' ')) n--;
    return n;
}

static char *utf16le_to_utf8(const uint8_t *p, size_t n)
{
    size_t units = n / 2u, o = 0, cap;
    char *s;
    if (!pc_mul_size(units, 3u, &cap) || !pc_add_size(cap, 1u, &cap)) return NULL;
    s = (char *)malloc(cap);
    if (!s) return NULL;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = le16v(p + 2u * i);
        if (c >= 0xD800u && c <= 0xDBFFu && i + 1u < units) {
            uint32_t d = le16v(p + 2u * (i + 1u));
            if (d >= 0xDC00u && d <= 0xDFFFu) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u);
                i++;
            }
        }
        if (c == 0u) break;
        if (c >= 0xD800u && c <= 0xDFFFu) c = 0xFFFDu;      /* lone surrogate */
        if (c < 0x80u) {
            s[o++] = (char)c;
        } else if (c < 0x800u) {
            s[o++] = (char)(0xC0u | (c >> 6));
            s[o++] = (char)(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            s[o++] = (char)(0xE0u | (c >> 12));
            s[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            s[o++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            if (o + 4u >= cap) break;
            s[o++] = (char)(0xF0u | (c >> 18));
            s[o++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            s[o++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            s[o++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    s[o] = '\0';
    return s;
}

char *cm_exif_get_text(const cm_exif *e, uint16_t tag)
{
    const cm_tag *t = cm_exif_find(e, tag);
    size_t n;
    char *s;
    if (!t || !t->len) return NULL;
    if (tag == CM_TAG_USERCOMMENT) {
        if (t->type != 7u || t->len < 8u) return NULL;
        if (memcmp(t->val, k_uc_unicode, 8u) == 0) {
            s = utf16le_to_utf8(t->val + 8, t->len - 8u);
        } else if (memcmp(t->val, k_uc_ascii, 8u) == 0 ||
                   memcmp(t->val, "\0\0\0\0\0\0\0\0", 8u) == 0) {
            n = trim_len(t->val + 8, t->len - 8u);
            s = cm_text_to_utf8(t->val + 8, n);
        } else {
            return NULL;                          /* JIS or unknown character code */
        }
    } else {
        if (t->type != 2u && t->type != 129u && t->type != 1u && t->type != 7u) return NULL;
        n = 0;
        while (n < t->len && t->val[n]) n++;
        n = trim_len(t->val, n);
        s = cm_text_to_utf8(t->val, n);
    }
    if (s && !s[0]) {
        free(s);
        return NULL;
    }
    if (s) {
        size_t k = strlen(s);
        while (k && (s[k - 1u] == ' ' || s[k - 1u] == '\0')) s[--k] = '\0';
        if (!k) { free(s); return NULL; }
    }
    return s;
}

/* ---- EXIF in pc_image_meta --------------------------------------------------------------- */
pc_status cm_meta_get_exif(const pc_image_meta *m, cm_exif *e)
{
    uint8_t *p;
    size_t n;
    pc_status st;
    cm_exif_init(e);
    st = cm_get_blob(m, CM_KEY_EXIF, &p, &n);
    if (st != PC_OK || !p) return st;
    st = cm_exif_parse(p, n, 0u, e);
    free(p);
    if (st == PC_ERR_FORMAT) {
        cm_exif_free(e);
        st = PC_OK;
    }
    return st;
}

pc_status cm_meta_put_exif(pc_image_meta *m, const cm_exif *e)
{
    pc_buf b;
    pc_status st;
    if (!m || !e) return PC_ERR_ARG;
    if (e->n == 0u) {
        cm_remove(m, CM_KEY_EXIF);
        return PC_OK;
    }
    memset(&b, 0, sizeof b);
    st = cm_exif_serialize(e, &b);
    if (st == PC_OK) st = cm_set_blob(m, CM_KEY_EXIF, b.p, b.n);
    pc_buf_free(&b);
    return st;
}

static pc_status set_short(cm_exif *e, uint8_t ifd, uint16_t tag, uint32_t v)
{
    uint8_t b[2];
    put16(b, v);
    return cm_exif_set(e, ifd, tag, 3u, 1u, b, 2u);
}

pc_status cm_meta_load_exif(pc_image_meta *m, const uint8_t *p, size_t n, bool upright,
                            int *orient)
{
    cm_exif add, cur;
    pc_status st;
    int o;
    if (orient) *orient = 1;
    if (!m || (!p && n)) return PC_ERR_ARG;
    if (n >= 6u && memcmp(p, "Exif\0\0", 6u) == 0) { p += 6; n -= 6u; }
    if (n < 8u || n > CM_EXIF_MAX) return PC_OK;
    cm_exif_init(&add);
    st = cm_exif_parse(p, n, 0u, &add);
    if (st != PC_OK) {
        cm_exif_free(&add);
        return st == PC_ERR_NOMEM ? st : PC_OK;
    }
    o = cm_exif_orientation(&add);
    if (orient) *orient = o;
    if (upright && cm_exif_find(&add, CM_TAG_ORIENTATION)) {
        st = set_short(&add, CM_IFD0, CM_TAG_ORIENTATION, 1u);
        if (st == PC_OK && o != 1) st = cm_meta_xmp_reset_orientation(m);
        if (st != PC_OK) { cm_exif_free(&add); return st; }
    }
    st = cm_meta_get_exif(m, &cur);
    for (size_t i = 0; i < add.n && st == PC_OK; i++) {
        const cm_tag *t = &add.t[i];
        if (find_tag(&cur, t->ifd, t->tag)) continue;
        st = cm_exif_set(&cur, t->ifd, t->tag, t->type, t->count, t->val, t->len);
    }
    if (st == PC_OK) st = cm_meta_put_exif(m, &cur);
    cm_exif_free(&cur);
    cm_exif_free(&add);
    return st;
}

/* RATIONAL closest to v with a denominator up to 10^6 (continued fractions). */
static void put_rational(double v, uint8_t out[8])
{
    uint32_t num = 96u, den = 1u;
    double x = v;
    uint64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    if (!(v > 0.0) || v > 4.0e6) v = 96.0;
    x = v;
    for (int i = 0; i < 32; i++) {
        double a = (double)(uint64_t)x;
        uint64_t ai = (uint64_t)a, h2 = ai * h1 + h0, k2 = ai * k1 + k0;
        if (k2 > 1000000u || h2 > 0xFFFFFFFFu) break;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        if (x - a < 1e-12) break;
        {
            double q = (double)h1 / (double)k1 - v;
            if (q < 0) q = -q;
            if (q <= v * 1e-15) break;
        }
        x = 1.0 / (x - a);
    }
    if (k1) { num = (uint32_t)h1; den = (uint32_t)k1; }
    put32(out, num);
    put32(out + 4, den);
}

/* Tags that only restate what the container or the pixels say. */
static bool tag_derived(uint16_t tag)
{
    return tag == CM_TAG_ORIENTATION || tag == CM_TAG_XRES || tag == CM_TAG_YRES ||
           tag == CM_TAG_RESUNIT || tag == CM_TAG_PIXEL_X || tag == CM_TAG_PIXEL_Y;
}

pc_status cm_exif_for_save(const pc_image_meta *m, uint32_t w, uint32_t h,
                           const uint16_t *drop, size_t n_drop, size_t max_len,
                           uint8_t **out, size_t *n)
{
    cm_exif e;
    pc_buf b;
    pc_status st;
    bool content = false;
    *out = NULL;
    *n = 0;
    if (!m) return PC_OK;
    st = cm_meta_get_exif(m, &e);
    if (st != PC_OK || e.n == 0u) { cm_exif_free(&e); return st; }
    for (size_t i = 0; i < n_drop; i++) cm_exif_remove(&e, drop[i]);
    for (size_t i = 0; i < e.n && !content; i++) content = !tag_derived(e.t[i].tag);
    if (!content) { cm_exif_free(&e); return PC_OK; }
    if (cm_exif_find(&e, CM_TAG_ORIENTATION)) st = set_short(&e, CM_IFD0, CM_TAG_ORIENTATION, 1u);
    for (int k = 0; k < 2 && st == PC_OK; k++) {
        uint16_t tag = k == 0 ? CM_TAG_PIXEL_X : CM_TAG_PIXEL_Y;
        uint32_t v = k == 0 ? w : h;
        const cm_tag *t = cm_exif_find(&e, tag);
        if (!t) continue;
        if (v <= 0xFFFFu && t->type == 3u) {
            st = set_short(&e, t->ifd, tag, v);
        } else {
            uint8_t b4[4];
            uint8_t ifd = t->ifd;
            put32(b4, v);
            cm_exif_remove(&e, tag);
            st = cm_exif_set(&e, ifd, tag, 4u, 1u, b4, 4u);
        }
    }
    if (st == PC_OK && m->dpi_x > 0.0 && m->dpi_y > 0.0 &&
        (cm_exif_find(&e, CM_TAG_XRES) || cm_exif_find(&e, CM_TAG_YRES))) {
        uint8_t r[8];
        put_rational(m->dpi_x, r);
        st = cm_exif_set(&e, CM_IFD0, CM_TAG_XRES, 5u, 1u, r, 8u);
        put_rational(m->dpi_y, r);
        if (st == PC_OK) st = cm_exif_set(&e, CM_IFD0, CM_TAG_YRES, 5u, 1u, r, 8u);
        if (st == PC_OK) st = set_short(&e, CM_IFD0, CM_TAG_RESUNIT, 2u);
    }
    memset(&b, 0, sizeof b);
    if (st == PC_OK) st = cm_exif_serialize(&e, &b);
    if (st == PC_OK && b.n > max_len && cm_exif_find(&e, CM_TAG_MAKERNOTE)) {
        b.n = 0;
        cm_exif_remove(&e, CM_TAG_MAKERNOTE);
        st = cm_exif_serialize(&e, &b);
    }
    cm_exif_free(&e);
    if (st != PC_OK || b.n == 0u || b.n > max_len) {
        pc_buf_free(&b);
        return st;
    }
    *out = b.p;
    *n = b.n;
    return PC_OK;
}

/* ---- XMP --------------------------------------------------------------------------------- */
pc_status cm_meta_load_xmp(pc_image_meta *m, const uint8_t *p, size_t n)
{
    char *s;
    pc_status st;
    if (!m || (!p && n)) return PC_ERR_ARG;
    while (n && p[n - 1u] == 0u) n--;
    if (n == 0u || n > CM_XMP_MAX || pc_meta_get(m, CM_KEY_XMP)) return PC_OK;
    if (!cm_utf8_valid(p, n)) return PC_OK;
    s = (char *)malloc(n + 1u);
    if (!s) return PC_ERR_NOMEM;
    memcpy(s, p, n);
    s[n] = '\0';
    st = pc_meta_add(m, CM_KEY_XMP, s);
    free(s);
    return st;
}

const char *cm_meta_xmp(const pc_image_meta *m, size_t *n)
{
    const char *s = pc_meta_get(m, CM_KEY_XMP);
    if (n) *n = s ? strlen(s) : 0u;
    return s && *s ? s : NULL;
}

pc_status cm_meta_xmp_reset_orientation(pc_image_meta *m)
{
    static const char k_name[] = "tiff:Orientation";
    const size_t nl = sizeof k_name - 1u;
    for (size_t i = 0; m && i < m->n_items; i++) {
        char *s;
        if (strcmp(m->items[i].key, CM_KEY_XMP) != 0) continue;
        s = m->items[i].value;
        for (char *q = strstr(s, k_name); q; q = strstr(q + nl, k_name)) {
            char *v = q + nl;
            if (q > s && (q[-1] == '<' || q[-1] == '/')) {
                /* element form <tiff:Orientation>N</tiff:Orientation> */
                if (q[-1] == '/' || *v != '>') continue;
                v++;
            } else {
                /* attribute form tiff:Orientation="N" */
                while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
                if (*v != '=') continue;
                v++;
                while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
                if (*v != '"' && *v != '\'') continue;
                v++;
            }
            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
            if (*v >= '1' && *v <= '8' && (v[1] < '0' || v[1] > '9')) *v = '1';
        }
    }
    return PC_OK;
}

/* ---- IPTC --------------------------------------------------------------------------------- */
static uint32_t be16v(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32v(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool cm_irb_find_iptc(const uint8_t *p, size_t n, const uint8_t **iim, size_t *len)
{
    size_t pos = 0;
    if (!p) return false;
    while (n - pos >= 12u) {
        uint32_t id, name, size;
        size_t at;
        if (memcmp(p + pos, "8BIM", 4u) != 0) return false;
        id = be16v(p + pos + 4u);
        name = p[pos + 6u];
        at = pos + 6u + ((1u + name + 1u) & ~(size_t)1u);     /* Pascal name, even size */
        if (at > n || n - at < 4u) return false;
        size = be32v(p + at);
        at += 4u;
        if (size > n - at) return false;
        if (id == 0x0404u) {
            *iim = p + at;
            *len = size;
            return size > 0u;
        }
        pos = at + size + (size & 1u);
        if (pos > n) return false;
    }
    return false;
}

pc_status cm_irb_put_iptc(pc_buf *out, const uint8_t *iim, size_t n)
{
    static const uint8_t head[8] = { '8', 'B', 'I', 'M', 0x04, 0x04, 0, 0 };
    pc_status st;
    if (!out || (!iim && n) || n > 0xFFFFFFF0u) return PC_ERR_ARG;
    st = pc_buf_append(out, head, sizeof head);
    if (st == PC_OK) st = pc_buf_put_be32(out, (uint32_t)n);
    if (st == PC_OK) st = pc_buf_append(out, iim, n);
    if (st == PC_OK && (n & 1u)) st = pc_buf_put_u8(out, 0u);
    return st;
}

pc_status cm_meta_load_iptc(pc_image_meta *m, const uint8_t *iim, size_t n)
{
    if (!m || (!iim && n)) return PC_ERR_ARG;
    if (n == 0u || n > CM_IPTC_MAX || pc_meta_get(m, CM_KEY_IPTC)) return PC_OK;
    return cm_set_blob(m, CM_KEY_IPTC, iim, n);
}

/* ---- PNG text and GIF comments ------------------------------------------------------------ */
static const struct { const char *kw; uint16_t tag; } k_text_map[] = {
    { "Author", CM_TAG_ARTIST },
    { "Copyright", CM_TAG_COPYRIGHT },
    { "Description", CM_TAG_DESCRIPTION },
    { "Comment", CM_TAG_USERCOMMENT },
};

uint16_t cm_png_text_tag(const char *keyword)
{
    for (size_t i = 0; keyword && i < sizeof k_text_map / sizeof k_text_map[0]; i++)
        if (strcmp(keyword, k_text_map[i].kw) == 0) return k_text_map[i].tag;
    return 0;
}

const char *cm_png_text_map(size_t i, uint16_t *tag)
{
    if (i >= sizeof k_text_map / sizeof k_text_map[0]) return NULL;
    if (tag) *tag = k_text_map[i].tag;
    return k_text_map[i].kw;
}

pc_status cm_meta_load_png_text(pc_image_meta *m, cm_exif *e, const char *keyword,
                                const char *text)
{
    uint16_t tag;
    size_t kl, tl;
    if (!m || !e || !keyword || !text) return PC_ERR_ARG;
    kl = strlen(keyword);
    tl = strlen(text);
    if (kl == 0u || kl > 79u * 2u || tl == 0u || tl > CM_TEXT_MAX) return PC_OK;
    if (!cm_utf8_valid((const uint8_t *)keyword, kl) || !cm_utf8_valid((const uint8_t *)text, tl))
        return PC_OK;
    tag = cm_png_text_tag(keyword);
    if (tag) {
        pc_status st = cm_exif_set_text(e, tag, text);
        return st == PC_ERR_ARG ? PC_OK : st;
    }
    {
        char key[sizeof CM_KEY_PNG_TEXT + 160u];
        memcpy(key, CM_KEY_PNG_TEXT, sizeof CM_KEY_PNG_TEXT - 1u);
        memcpy(key + sizeof CM_KEY_PNG_TEXT - 1u, keyword, kl + 1u);
        if (pc_meta_get(m, key)) return PC_OK;            /* first chunk wins */
        return pc_meta_add(m, key, text);
    }
}

pc_status cm_meta_load_comment(pc_image_meta *m, const uint8_t *p, size_t n)
{
    cm_exif e;
    char *add, *old, *joined = NULL;
    pc_status st;
    if (!m || (!p && n)) return PC_ERR_ARG;
    n = trim_len(p, n);
    if (n == 0u || n > CM_TEXT_MAX) return PC_OK;
    add = cm_text_to_utf8(p, n);
    if (!add) return PC_ERR_NOMEM;
    st = cm_meta_get_exif(m, &e);
    if (st != PC_OK) { free(add); return st; }
    old = cm_exif_get_text(&e, CM_TAG_USERCOMMENT);
    if (old) {
        size_t a = strlen(old), b = strlen(add);
        joined = (a + b + 2u <= CM_TEXT_MAX) ? (char *)malloc(a + b + 2u) : NULL;
        if (joined) {
            memcpy(joined, old, a);
            joined[a] = '\n';
            memcpy(joined + a + 1u, add, b + 1u);
        }
    }
    st = cm_exif_set_text(&e, CM_TAG_USERCOMMENT, joined ? joined : (old ? old : add));
    if (st == PC_ERR_ARG) st = PC_OK;
    if (st == PC_OK) st = cm_meta_put_exif(m, &e);
    free(joined);
    free(old);
    free(add);
    cm_exif_free(&e);
    return st;
}
