/* fmt_pdn.c - Paint.NET .pdn documents: loader, dump tool and the codec
 * descriptor (lane L6C). The writer lives in pdn_write.c.
 *
 * Layout (docs/codecs/pdn.md):
 *   "PDN3", uint24 LE header length, UTF-8 XML header (<pdnImage ...>),
 *   0x00 0x01, MS-NRBF stream with root PaintDotNet.Document, then the
 *   data of every deferred PaintDotNet.MemoryBlock in serialization order:
 *   format byte (0 gzip chunks, 1 raw chunks), uint32 BE chunk size, and
 *   ceil(length / chunk size) chunks of (uint32 BE number, uint32 BE size,
 *   bytes) in any order.
 * The deferred block layout follows the MIT-licensed Paint.NET 3.36 source
 * (MemoryBlock.cs); see docs/notice/l6c.md.
 */
#include "nrbf.h"
#include "pdn.h"
#include "cmeta.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"

#define NONE32 0xFFFFFFFFu
#define PDN_PIECE (256u * 1024u)    /* inflate output piece, multiple of 4 */

/* ---- whitelists (X-19): everything a 3.x .. 5.x document contains ---------- */
static const char *const k_classes[] = {
    "PaintDotNet.Document",
    "PaintDotNet.LayerList",
    "PaintDotNet.BitmapLayer",
    "PaintDotNet.BitmapLayer+BitmapLayerProperties",
    "PaintDotNet.Layer+LayerProperties",
    "PaintDotNet.LayerBlendMode",
    "PaintDotNet.Surface",
    "PaintDotNet.MemoryBlock",
    "PaintDotNet.UserBlendOps+NormalBlendOp",
    "PaintDotNet.UserBlendOps+MultiplyBlendOp",
    "PaintDotNet.UserBlendOps+AdditiveBlendOp",
    "PaintDotNet.UserBlendOps+ColorBurnBlendOp",
    "PaintDotNet.UserBlendOps+ColorDodgeBlendOp",
    "PaintDotNet.UserBlendOps+ReflectBlendOp",
    "PaintDotNet.UserBlendOps+GlowBlendOp",
    "PaintDotNet.UserBlendOps+OverlayBlendOp",
    "PaintDotNet.UserBlendOps+DifferenceBlendOp",
    "PaintDotNet.UserBlendOps+NegationBlendOp",
    "PaintDotNet.UserBlendOps+LightenBlendOp",
    "PaintDotNet.UserBlendOps+DarkenBlendOp",
    "PaintDotNet.UserBlendOps+ScreenBlendOp",
    "PaintDotNet.UserBlendOps+XorBlendOp",
    "System.Version",
    "System.Collections.ArrayList",
    /* Paint.NET 3.x userMetaData and its .NET Framework internals */
    "System.Collections.Specialized.NameValueCollection",
    "System.Collections.CaseInsensitiveHashCodeProvider",
    "System.Collections.CaseInsensitiveComparer",
    "System.Globalization.TextInfo",
    "System.Globalization.CompareInfo",
    /* 4.x / 5.x userMetadataItems: KeyValuePair<string, string> */
    "System.Collections.Generic.KeyValuePair`2[[System.String, *",
    NULL
};

static const char *const k_libs[] = {
    "PaintDotNet.", "PdnLib,", "System,", "mscorlib,", "System.Private.CoreLib,", NULL
};

/* Index = pc_blend_mode = LayerBlendMode value. */
static const char *const k_blend_ops[PC_BLEND_COUNT] = {
    "Normal", "Multiply", "Additive", "ColorBurn", "ColorDodge", "Reflect", "Glow",
    "Overlay", "Difference", "Negation", "Lighten", "Darken", "Screen", "Xor"
};

static void pdn_opts(nrbf_opts *o)
{
    nrbf_opts_default(o);
    o->classes = k_classes;
    o->libs = k_libs;
}

/* ---- small helpers -------------------------------------------------------------- */
static bool cls_is(const nrbf_doc *d, const nrbf_obj *o, const char *name)
{
    const nrbf_class *c = nrbf_class_of(d, o);
    return c && nrbf_str_eq(c->name, name);
}

static const nrbf_obj *member_obj(const nrbf_doc *d, const nrbf_obj *o, const char *name)
{
    return nrbf_deref(d, nrbf_member_value(d, o, name));
}

static bool member_i64(const nrbf_doc *d, const nrbf_obj *o, const char *name, int64_t *v)
{
    return nrbf_as_i64(nrbf_member_value(d, o, name), v);
}

static uint32_t obj_index(const nrbf_doc *d, const nrbf_obj *o)
{
    return (uint32_t)(o - d->objs);
}

/* "A.B.C.D" -> major; false when s does not start with digits. */
static bool parse_major(const char *s, size_t n, uint32_t *major)
{
    uint32_t v = 0;
    size_t i = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && v < 100000u)
        v = v * 10u + (uint32_t)(s[i++] - '0');
    if (i == 0) return false;
    *major = v;
    return true;
}

/* Value of attribute `name` inside the first tag of xml[0..n). */
static bool xml_attr(const char *xml, size_t n, const char *name, const char **val, size_t *len)
{
    size_t nl = strlen(name), end = 0;
    while (end < n && xml[end] != '>') end++;
    for (size_t i = 0; i + nl + 2u <= end; i++) {
        if ((i == 0 || xml[i - 1] == ' ') && memcmp(xml + i, name, nl) == 0 &&
            xml[i + nl] == '=' && xml[i + nl + 1u] == '"') {
            size_t s = i + nl + 2u, e = s;
            while (e < end && xml[e] != '"') e++;
            if (e >= end) return false;
            *val = xml + s;
            *len = e - s;
            return true;
        }
    }
    return false;
}

/* Copy a UTF-8 name into dst (cap bytes incl. NUL): invalid sequences and
 * control characters become '?', truncation never splits a sequence.
 * Returns true when the name was truncated. */
static bool copy_name(char *dst, size_t cap, nrbf_str s)
{
    size_t o = 0, i = 0;
    bool trunc = false;
    while (i < s.n) {
        unsigned char c = (unsigned char)s.p[i];
        size_t len = 1;
        bool ok = true;
        if (c < 0x80u) ok = c >= 0x20u && c != 0x7Fu;
        else if ((c & 0xE0u) == 0xC0u && c >= 0xC2u) len = 2;
        else if ((c & 0xF0u) == 0xE0u) len = 3;
        else if ((c & 0xF8u) == 0xF0u && c <= 0xF4u) len = 4;
        else ok = false;
        if (ok && len > 1u) {
            uint32_t cp = c & (0x7Fu >> len);
            if (i + len > s.n) ok = false;
            for (size_t k = 1; ok && k < len; k++) {
                unsigned char cc = (unsigned char)s.p[i + k];
                if ((cc & 0xC0u) != 0x80u) ok = false;
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            if (ok && ((len == 3u && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) ||
                       (len == 4u && (cp < 0x10000u || cp > 0x10FFFFu))))
                ok = false;
        }
        if (!ok) len = 1;
        if (o + (ok ? len : 1u) + 1u > cap) { trunc = true; break; }
        if (ok) memcpy(dst + o, s.p + i, len);
        else dst[o] = '?';
        o += ok ? len : 1u;
        i += len;
    }
    dst[o] = '\0';
    return trunc;
}

/* ---- typed model ------------------------------------------------------------------ */
typedef struct pdn_lay {
    nrbf_str      name;
    bool          visible, is_bg;
    uint8_t       opacity;
    pc_blend_mode mode;
    uint32_t      mb;           /* objs index of the MemoryBlock */
    bool          deferred;
    const nrbf_obj *inline_data;    /* pointerData byte array when not deferred */
} pdn_lay;

typedef struct pdn_model {
    uint32_t  w, h, n;
    pdn_lay  *lay;
    double    dpi_x, dpi_y;
    uint8_t  *icc;
    size_t    icc_len;
    bool      unknown_blend;
    const nrbf_obj *meta_arr;   /* userMetadataItems, or NULL */
} pdn_model;

static void model_free(pdn_model *m)
{
    free(m->lay);
    free(m->icc);
    memset(m, 0, sizeof *m);
}

static pc_status blend_from_op(const nrbf_doc *d, const nrbf_obj *op, pc_blend_mode *mode)
{
    const nrbf_class *c = nrbf_class_of(d, op);
    static const char pre[] = "PaintDotNet.UserBlendOps+";
    size_t pl = sizeof pre - 1u;
    if (!c || c->name.n <= pl || memcmp(c->name.p, pre, pl) != 0) return PC_ERR_FORMAT;
    for (int i = 0; i < (int)PC_BLEND_COUNT; i++) {
        size_t nl = strlen(k_blend_ops[i]);
        if (c->name.n == pl + nl + 7u && memcmp(c->name.p + pl, k_blend_ops[i], nl) == 0 &&
            memcmp(c->name.p + pl + nl, "BlendOp", 7u) == 0) {
            *mode = (pc_blend_mode)i;
            return PC_OK;
        }
    }
    return PC_ERR_FORMAT;
}

/* One "$exif.tagN[i]" value: <exif id="282" len="8" type="5" value="..." />. */
static void exif_item(pdn_model *m, nrbf_str v, double res[2], int *unit)
{
    const char *a;
    size_t al;
    uint32_t id = 0, type = 0;
    uint8_t buf[16];
    size_t blen = 0;
    if (v.n < 6u || memcmp(v.p, "<exif ", 6u) != 0) return;
    if (!xml_attr(v.p + 1, v.n - 1u, "id", &a, &al) || !parse_major(a, al, &id)) return;
    if (!xml_attr(v.p + 1, v.n - 1u, "type", &a, &al) || !parse_major(a, al, &type)) return;
    if (!xml_attr(v.p + 1, v.n - 1u, "value", &a, &al)) return;
    if (id == 34675u && (type == 7u || type == 1u) && !m->icc && al >= 4u) {
        size_t cap = al / 4u * 3u;
        uint8_t *icc = (uint8_t *)malloc(cap);
        if (icc && pdn_base64_decode(a, al, icc, cap, &blen) && blen >= 128u) {
            m->icc = icc;
            m->icc_len = blen;
        } else {
            free(icc);
        }
        return;
    }
    if (!pdn_base64_decode(a, al, buf, sizeof buf, &blen)) return;
    if ((id == 282u || id == 283u) && type == 5u && blen == 8u) {
        uint32_t num = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
                       ((uint32_t)buf[3] << 24);
        uint32_t den = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
                       ((uint32_t)buf[7] << 24);
        if (den) res[id == 283u] = (double)num / (double)den;
    } else if (id == 296u && type == 3u && blen == 2u) {
        *unit = (int)buf[0] | ((int)buf[1] << 8);
    }
}

static void read_metadata(const nrbf_doc *d, const nrbf_obj *doc, pdn_model *m)
{
    const nrbf_obj *arr = member_obj(d, doc, "userMetadataItems");
    double res[2] = { 0.0, 0.0 };
    int unit = 2;
    if (!arr || arr->kind != NRBF_O_ARRAY) return;
    m->meta_arr = arr;
    for (uint32_t i = 0; i < arr->n; i++) {
        const nrbf_obj *kv = nrbf_deref(d, nrbf_elem(d, arr, i));
        const nrbf_obj *k = member_obj(d, kv, "key"), *v = member_obj(d, kv, "value");
        if (!k || !v || k->kind != NRBF_O_STRING || v->kind != NRBF_O_STRING) continue;
        if (k->str.n >= 6u && memcmp(k->str.p, "$exif.", 6u) == 0) exif_item(m, v->str, res, &unit);
    }
    if (res[0] > 0.0 && res[1] > 0.0 && (unit == 2 || unit == 3)) {
        double f = unit == 3 ? 2.54 : 1.0;
        m->dpi_x = res[0] * f;
        m->dpi_y = res[1] * f;
    }
}

/* NRBF string as a NUL-terminated UTF-8 copy (malloc), NULL when it is not
 * valid UTF-8 or too long. */
static char *str_copy(nrbf_str v, size_t max)
{
    char *c;
    if (v.n == 0u || v.n > max || !cm_utf8_valid((const uint8_t *)v.p, v.n)) return NULL;
    c = (char *)malloc(v.n + 1u);
    if (!c) return NULL;
    memcpy(c, v.p, v.n);
    c[v.n] = '\0';
    return c;
}

/* The IFD of a flattened tag: GPS ids 0..31, except the two
 * Interoperability tags (index "R98" as 4 ASCII bytes, version UNDEFINED). */
static uint8_t flat_ifd(uint32_t id, uint32_t type, size_t len)
{
    if ((id == 1u && type == 2u && len == 4u) || (id == 2u && type == 7u))
        return CM_IFD_INTEROP;
    return cm_exif_ifd_of((uint16_t)id);
}

/* One "$exif" value into the EXIF set e (or the IPTC item). Malformed
 * entries are skipped. */
static pc_status exif_entry(nrbf_str v, cm_exif *e, pc_image_meta *meta)
{
    const char *a;
    size_t al, blen = 0;
    uint32_t id = 0, type = 0, ts;
    uint8_t *buf;
    pc_status st = PC_OK;
    uint8_t ifd;
    if (v.n < 6u || memcmp(v.p, "<exif ", 6u) != 0) return PC_OK;
    if (!xml_attr(v.p + 1, v.n - 1u, "id", &a, &al) || !parse_major(a, al, &id)) return PC_OK;
    if (!xml_attr(v.p + 1, v.n - 1u, "type", &a, &al) || !parse_major(a, al, &type)) return PC_OK;
    if (!xml_attr(v.p + 1, v.n - 1u, "value", &a, &al)) return PC_OK;
    ts = cm_type_size(type);
    if (id > 0xFFFFu || !ts || al == 0u || al % 4u || al / 4u * 3u > CM_EXIF_MAX) return PC_OK;
    if (id == CM_TAG_ICC) return PC_OK;                  /* meta.icc (read_metadata) */
    buf = (uint8_t *)malloc(al / 4u * 3u);
    if (!buf) return PC_ERR_NOMEM;
    if (pdn_base64_decode(a, al, buf, al / 4u * 3u, &blen) && blen && blen % ts == 0u) {
        if (id == CM_TAG_IPTC) {
            st = cm_meta_load_iptc(meta, buf, blen);
        } else {
            ifd = flat_ifd(id, type, blen);
            if (!cm_exif_tag_dropped(ifd, (uint16_t)id) && !cm_exif_find(e, (uint16_t)id))
                st = cm_exif_set(e, ifd, (uint16_t)id, (uint16_t)type, (uint32_t)(blen / ts),
                                 buf, (uint32_t)blen);
            if (st == PC_ERR_ARG) st = PC_OK;
        }
    }
    free(buf);
    return st;
}

/* Every userMetadataItem into meta's items (cmeta.h key scheme): "$exif"
 * tags into the "exif" item (and tag 33723 into "iptc"), the first
 * "$xmp.packetN" into "xmp", "$paintc.<key>" back into <key>, and any other
 * "$<section>.<name>" into "pdn.<section>.<name>". */
static pc_status read_items(const nrbf_doc *d, const nrbf_obj *arr, pc_image_meta *meta)
{
    cm_exif e;
    pc_status st = PC_OK;
    bool content = false;
    if (!arr || arr->kind != NRBF_O_ARRAY) return PC_OK;
    cm_exif_init(&e);
    for (uint32_t i = 0; i < arr->n && st == PC_OK; i++) {
        const nrbf_obj *kv = nrbf_deref(d, nrbf_elem(d, arr, i));
        const nrbf_obj *k = member_obj(d, kv, "key"), *v = member_obj(d, kv, "value");
        char *key, *val;
        if (!k || !v || k->kind != NRBF_O_STRING || v->kind != NRBF_O_STRING) continue;
        if (k->str.n >= 6u && memcmp(k->str.p, "$exif.", 6u) == 0) {
            st = exif_entry(v->str, &e, meta);
            continue;
        }
        if (k->str.n >= 5u && memcmp(k->str.p, "$xmp.", 5u) == 0) {
            st = cm_meta_load_xmp(meta, (const uint8_t *)v->str.p, v->str.n);
            continue;
        }
        if (k->str.n < 2u || k->str.p[0] != '$') continue;
        key = str_copy(k->str, 4096u);
        val = str_copy(v->str, CM_XMP_MAX);
        if (key && val) {
            if (strncmp(key, "$paintc.", 8u) == 0 && key[8]) {
                if (!pc_meta_get(meta, key + 8)) st = pc_meta_add(meta, key + 8, val);
            } else {
                size_t kl = strlen(key);
                char *nk = (char *)malloc(kl + 4u);
                if (!nk) {
                    st = PC_ERR_NOMEM;
                } else {
                    memcpy(nk, CM_KEY_PDN, 4u);          /* "pdn." + key without '$' */
                    memcpy(nk + 4, key + 1, kl);
                    if (!pc_meta_get(meta, nk)) st = pc_meta_add(meta, nk, val);
                    free(nk);
                }
            }
        }
        free(key);
        free(val);
    }
    /* Software and the resolution alone are what every .pdn carries */
    for (size_t i = 0; i < e.n && !content; i++) {
        uint16_t t = e.t[i].tag;
        content = t != CM_TAG_SOFTWARE && t != CM_TAG_XRES && t != CM_TAG_YRES &&
                  t != CM_TAG_RESUNIT;
    }
    if (st == PC_OK && content) st = cm_meta_put_exif(meta, &e);
    cm_exif_free(&e);
    return st;
}

static pc_status read_layer(const nrbf_doc *d, const nrbf_obj *bl, pdn_model *m, uint32_t i,
                            uint8_t *seen, pdn_info *info)
{
    pdn_lay *L = &m->lay[i];
    const nrbf_obj *props, *bprops, *surf, *mb, *name, *bmo;
    int64_t v, sw, sh, stride, len;
    bool b;
    if (!cls_is(d, bl, "PaintDotNet.BitmapLayer")) return PC_ERR_FORMAT;
    if (seen[obj_index(d, bl)]) return PC_ERR_FORMAT;   /* a layer listed twice */
    seen[obj_index(d, bl)] = 1u;
    if (member_i64(d, bl, "Layer+width", &v) && v != (int64_t)m->w) return PC_ERR_FORMAT;
    if (member_i64(d, bl, "Layer+height", &v) && v != (int64_t)m->h) return PC_ERR_FORMAT;

    props = member_obj(d, bl, "Layer+properties");
    if (!cls_is(d, props, "PaintDotNet.Layer+LayerProperties")) return PC_ERR_FORMAT;
    name = member_obj(d, props, "name");
    if (name && name->kind == NRBF_O_STRING) L->name = name->str;
    L->visible = true;
    if (nrbf_member_value(d, props, "visible")) {
        if (!nrbf_as_bool(nrbf_member_value(d, props, "visible"), &b)) return PC_ERR_FORMAT;
        L->visible = b;
    }
    if (nrbf_as_bool(nrbf_member_value(d, props, "isBackground"), &b)) L->is_bg = b;
    if (!member_i64(d, props, "opacity", &v) || v < 0 || v > 255) return PC_ERR_FORMAT;
    L->opacity = (uint8_t)v;

    L->mode = PC_BLEND_NORMAL;
    bmo = member_obj(d, props, "blendMode");
    bprops = member_obj(d, bl, "properties");
    if (bmo) {
        if (!cls_is(d, bmo, "PaintDotNet.LayerBlendMode") || !member_i64(d, bmo, "value__", &v))
            return PC_ERR_FORMAT;
        if (v >= 0 && v < (int64_t)PC_BLEND_COUNT) L->mode = (pc_blend_mode)v;
        else m->unknown_blend = true;
    } else if (bprops) {
        const nrbf_obj *op;
        if (!cls_is(d, bprops, "PaintDotNet.BitmapLayer+BitmapLayerProperties"))
            return PC_ERR_FORMAT;
        op = member_obj(d, bprops, "blendOp");
        if (op && blend_from_op(d, op, &L->mode) != PC_OK) return PC_ERR_FORMAT;
        if (info) info->legacy_blend = true;
    }

    surf = member_obj(d, bl, "surface");
    if (!cls_is(d, surf, "PaintDotNet.Surface")) return PC_ERR_FORMAT;
    if (seen[obj_index(d, surf)]) return PC_ERR_FORMAT;
    seen[obj_index(d, surf)] = 1u;
    if (!member_i64(d, surf, "width", &sw) || !member_i64(d, surf, "height", &sh) ||
        !member_i64(d, surf, "stride", &stride))
        return PC_ERR_FORMAT;
    if (sw != (int64_t)m->w || sh != (int64_t)m->h) return PC_ERR_FORMAT;
    if (stride != (int64_t)m->w * 4) {
        if (stride == (int64_t)m->w * 3) return PC_ERR_UNSUPPORTED;    /* 24-bit surface */
        return PC_ERR_FORMAT;
    }
    mb = member_obj(d, surf, "scan0");
    if (!cls_is(d, mb, "PaintDotNet.MemoryBlock")) return PC_ERR_FORMAT;
    if (seen[obj_index(d, mb)]) return PC_ERR_FORMAT;   /* two surfaces sharing memory */
    seen[obj_index(d, mb)] = 1u;
    if (!member_i64(d, mb, "length64", &len) || len != stride * sh) return PC_ERR_FORMAT;
    if (nrbf_as_bool(nrbf_member_value(d, mb, "hasParent"), &b) && b) return PC_ERR_UNSUPPORTED;
    L->mb = obj_index(d, mb);
    L->deferred = nrbf_as_bool(nrbf_member_value(d, mb, "deferred"), &b) && b;
    if (!L->deferred) {
        const nrbf_obj *pd = member_obj(d, mb, "pointerData");
        if (!pd || pd->kind != NRBF_O_PRIM_ARRAY || pd->prim != NRBF_P_BYTE ||
            (uint64_t)pd->data_len != (uint64_t)len)
            return PC_ERR_FORMAT;
        L->inline_data = pd;
    }
    if (info && i < 32u && L->is_bg) info->bg_mask |= 1u << i;
    return PC_OK;
}

static pc_status build_model(const nrbf_doc *d, const pc_codec_limits *lim, pdn_model *m,
                             pdn_info *info)
{
    const nrbf_obj *doc = nrbf_get(d, d->root_id), *list, *items, *ver;
    int64_t w, h, size;
    uint8_t *seen;
    pc_status st;
    if (!cls_is(d, doc, "PaintDotNet.Document")) return PC_ERR_FORMAT;
    ver = member_obj(d, doc, "savedWith");
    if (ver && cls_is(d, ver, "System.Version")) {
        int64_t a = -1, b = -1, c = -1, e = -1;
        (void)member_i64(d, ver, "_Major", &a);
        (void)member_i64(d, ver, "_Minor", &b);
        (void)member_i64(d, ver, "_Build", &c);
        (void)member_i64(d, ver, "_Revision", &e);
        if (a >= 6) return PC_ERR_UNSUPPORTED;      /* Paint.NET 6 and later */
        if (info && a >= 0) {
            snprintf(info->saved_with, sizeof info->saved_with, "%lld.%lld.%lld.%lld",
                     (long long)a, (long long)b, (long long)c, (long long)e);
            info->version_major = (uint32_t)a;
        }
    }
    if (!member_i64(d, doc, "width", &w) || !member_i64(d, doc, "height", &h))
        return PC_ERR_FORMAT;
    if (w <= 0 || h <= 0) return PC_ERR_FORMAT;
    if (w > (int64_t)PC_MAX_DIM || h > (int64_t)PC_MAX_DIM) return PC_ERR_LIMIT;
    m->w = (uint32_t)w;
    m->h = (uint32_t)h;
    list = member_obj(d, doc, "layers");
    if (!cls_is(d, list, "PaintDotNet.LayerList")) return PC_ERR_FORMAT;
    items = member_obj(d, list, "ArrayList+_items");
    if (!items) items = member_obj(d, list, "_items");
    if (!member_i64(d, list, "ArrayList+_size", &size) && !member_i64(d, list, "_size", &size))
        return PC_ERR_FORMAT;
    if (!items || items->kind != NRBF_O_ARRAY || size < 1 || size > (int64_t)items->n)
        return PC_ERR_FORMAT;
    if (size > (int64_t)lim->max_layers) return PC_ERR_LIMIT;
    m->n = (uint32_t)size;
    st = pc_codec_check_size(lim, m->w, m->h, m->n);
    if (st != PC_OK) return st;
    m->lay = (pdn_lay *)calloc(m->n, sizeof *m->lay);
    seen = (uint8_t *)calloc(d->n_objs, 1u);
    if (!m->lay || !seen) { free(seen); return PC_ERR_NOMEM; }
    st = PC_OK;
    for (uint32_t i = 0; i < m->n && st == PC_OK; i++)
        st = read_layer(d, nrbf_deref(d, nrbf_elem(d, items, i)), m, i, seen, info);
    free(seen);
    if (st != PC_OK) return st;
    if (info) {
        info->list_capacity = items->n;
        for (uint32_t i = 0; i < m->n; i++)
            for (uint32_t j = 0; j < i; j++) {
                if (m->lay[i].name.p == m->lay[j].name.p && m->lay[i].name.n)
                    info->names_shared = true;
                if (m->lay[i].name.n == m->lay[j].name.n &&
                    (m->lay[i].name.n == 0u ||
                     memcmp(m->lay[i].name.p, m->lay[j].name.p, m->lay[i].name.n) == 0))
                    info->names_equal = true;
            }
    }
    read_metadata(d, doc, m);
    return PC_OK;
}

/* ---- pixel sink: chunk bytes -> layer tiles ------------------------------------------ */
typedef struct sink {
    const pc_doc *doc;
    pc_layer     *l;            /* NULL: validate and discard */
    uint32_t      w;
    uint64_t      len;          /* block length in bytes */
    uint32_t      cs;           /* chunk size */
    uint8_t      *straddle;     /* 5 bytes per chunk boundary when cs % 4 != 0 */
    /* current chunk */
    uint64_t      pos, start;
    uint8_t       part[4];
} sink;

static pc_status store_px(sink *s, uint64_t p0, uint64_t count, const uint8_t *src)
{
    while (count) {
        uint32_t y = (uint32_t)(p0 / s->w), x = (uint32_t)(p0 % s->w);
        uint64_t k;
        pc_status st;
        if (x == 0u && count >= s->w) {
            uint64_t rows = count / s->w;
            if (rows > (uint64_t)(s->doc->h - y)) rows = s->doc->h - y;
            k = rows * s->w;
            st = pc_layer_store_rect(s->doc, s->l, pc_rect_make(0, (int32_t)y, (int32_t)s->w,
                                     (int32_t)rows), (const pc_px32 *)(const void *)src, s->w);
        } else {
            k = s->w - x;
            if (k > count) k = count;
            st = pc_layer_store_rect(s->doc, s->l, pc_rect_make((int32_t)x, (int32_t)y,
                                     (int32_t)k, 1), (const pc_px32 *)(const void *)src, s->w);
        }
        if (st != PC_OK) return st;
        p0 += k;
        count -= k;
        src += k * 4u;
    }
    return PC_OK;
}

/* Bytes [lo, hi) of the pixel that straddles chunk boundary B. */
static pc_status straddle_merge(sink *s, uint64_t B, const uint8_t *bytes, uint32_t lo,
                                uint32_t hi)
{
    uint8_t *rec = s->straddle + (size_t)(B / s->cs) * 5u;
    for (uint32_t i = lo; i < hi; i++) {
        rec[i] = bytes[i];
        rec[4] = (uint8_t)(rec[4] | (1u << i));
    }
    if (rec[4] == 0x0Fu) return store_px(s, B / 4u, 1u, rec);
    return PC_OK;
}

static pc_status sink_feed(sink *s, const uint8_t *src, size_t n)
{
    pc_status st;
    if (!s->l) { s->pos += n; return PC_OK; }
    while (n && (s->pos & 3u)) {                /* finish a partial pixel */
        s->part[s->pos & 3u] = *src++;
        s->pos++;
        n--;
        if ((s->pos & 3u) == 0u) {
            uint64_t px0 = s->pos - 4u;
            if (px0 >= s->start) st = store_px(s, px0 / 4u, 1u, s->part);
            else st = straddle_merge(s, s->start, s->part, (uint32_t)(s->start & 3u), 4u);
            if (st != PC_OK) return st;
        }
    }
    if (n >= 4u) {
        size_t k = n & ~(size_t)3u;
        st = store_px(s, s->pos / 4u, k / 4u, src);
        if (st != PC_OK) return st;
        s->pos += k;
        src += k;
        n -= k;
    }
    while (n) {
        s->part[s->pos & 3u] = *src++;
        s->pos++;
        n--;
    }
    return PC_OK;
}

static pc_status sink_end_chunk(sink *s)
{
    if (!s->l || (s->pos & 3u) == 0u) return PC_OK;
    return straddle_merge(s, s->pos, s->part, 0u, (uint32_t)(s->pos & 3u));
}

/* Inflate one gzip chunk of exactly clen bytes into the sink. */
static pc_status inflate_chunk(z_stream *zs, uint8_t *piece, const uint8_t *data, uint32_t size,
                               uint64_t clen, sink *s)
{
    int r = Z_OK;
    uint64_t left = clen;
    pc_status st;
    if (inflateReset(zs) != Z_OK) return PC_ERR_FORMAT;
    zs->next_in = (Bytef *)(uintptr_t)data;
    zs->avail_in = size;
    while (left) {
        uInt want = left < PDN_PIECE ? (uInt)left : (uInt)PDN_PIECE, got;
        zs->next_out = piece;
        zs->avail_out = want;
        r = inflate(zs, Z_NO_FLUSH);
        got = want - zs->avail_out;
        st = sink_feed(s, piece, got);
        if (st != PC_OK) return st;
        left -= got;
        if (r == Z_STREAM_END) break;
        if (r == Z_MEM_ERROR) return PC_ERR_NOMEM;
        if (r != Z_OK) return PC_ERR_FORMAT;
    }
    if (left) return PC_ERR_FORMAT;             /* stream ended early */
    if (r != Z_STREAM_END) {                    /* must end exactly here */
        uint8_t extra;
        zs->next_out = &extra;
        zs->avail_out = 1u;
        r = inflate(zs, Z_NO_FLUSH);
        if (r != Z_STREAM_END || zs->avail_out == 0u) return PC_ERR_FORMAT;
    }
    if (zs->avail_in) return PC_ERR_FORMAT;     /* trailing bytes inside the chunk */
    return sink_end_chunk(s);
}

typedef struct block_ctx {
    z_stream  zs;
    bool      zs_ready;
    uint8_t  *piece;
    pc_buf   *dump;             /* pdn_dump: per-block summary */
    uint32_t  dump_flags;
    pdn_info *info;
} block_ctx;

static void bprintf(block_ctx *c, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;
    if (!c->dump) return;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)pc_buf_append(c->dump, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1u);
}

/* Parse (and decode into l when not NULL) one deferred block of len bytes. */
static pc_status read_block(block_ctx *c, pc_rd *rd, uint64_t len, const pc_doc *doc,
                            pc_layer *l, uint32_t index)
{
    uint32_t fmt = pc_rd_u8(rd), cs = pc_rd_be32(rd);
    uint64_t count;
    uint8_t *found = NULL;
    sink s;
    pc_status st = PC_OK;
    bool ordered = true;
    uint32_t prev = 0;
    if (rd->err) return PC_ERR_FORMAT;
    if (fmt > 1u || cs == 0u) return PC_ERR_FORMAT;
    if (cs < 4u) return PC_ERR_UNSUPPORTED;
    if (c->info && c->info->block_format == 0xFFu) {
        c->info->block_format = (uint8_t)fmt;
        c->info->chunk_size = cs;
    }
    count = (len + cs - 1u) / cs;
    if (count > pc_rd_left(rd) / 8u) return PC_ERR_FORMAT;     /* 8 header bytes per chunk */
    found = (uint8_t *)calloc((size_t)((count + 7u) / 8u) + 1u, 1u);
    if (!found) return PC_ERR_NOMEM;
    memset(&s, 0, sizeof s);
    s.doc = doc;
    s.l = l;
    s.w = doc ? doc->w : 1u;
    s.len = len;
    s.cs = cs;
    if (l && (cs & 3u)) {
        s.straddle = (uint8_t *)calloc((size_t)count, 5u);
        if (!s.straddle) { free(found); return PC_ERR_NOMEM; }
    }
    if (fmt == 0u && l && !c->zs_ready) {
        memset(&c->zs, 0, sizeof c->zs);
        if (inflateInit2(&c->zs, 15 + 16) != Z_OK) { st = PC_ERR_NOMEM; goto done; }
        c->zs_ready = true;
    }
    if (l && !c->piece) {
        c->piece = (uint8_t *)malloc(PDN_PIECE);
        if (!c->piece) { st = PC_ERR_NOMEM; goto done; }
    }
    if (c->dump)
        bprintf(c, "  block %u format=%u chunk_size=%u chunks=%llu\n", index, fmt, cs,
                (unsigned long long)count);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t num = pc_rd_be32(rd), size = pc_rd_be32(rd);
        uint64_t off, clen;
        const uint8_t *data;
        if (rd->err || num >= count || (found[num >> 3] & (1u << (num & 7u))) ||
            size > pc_rd_left(rd)) { st = PC_ERR_FORMAT; goto done; }
        found[num >> 3] = (uint8_t)(found[num >> 3] | (1u << (num & 7u)));
        if (i && num < prev) ordered = false;
        prev = num;
        off = (uint64_t)num * cs;
        clen = len - off < cs ? len - off : cs;
        data = rd->p + rd->pos;
        (void)pc_rd_skip(rd, size);
        if (c->dump && !(c->dump_flags & NRBF_DUMP_STRUCT))
            bprintf(c, "    chunk %u size=%u%s\n", num, size,
                    fmt == 0u && size >= 10u && data[0] == 0x1Fu && data[1] == 0x8Bu ? "" :
                    (fmt == 0u ? " (no gzip magic)" : ""));
        if (!l) continue;
        s.pos = s.start = off;
        if (fmt == 1u) {
            if ((uint64_t)size != clen) { st = PC_ERR_FORMAT; goto done; }
            st = sink_feed(&s, data, size);
            if (st == PC_OK) st = sink_end_chunk(&s);
        } else {
            st = inflate_chunk(&c->zs, c->piece, data, size, clen, &s);
        }
        if (st != PC_OK) goto done;
    }
    if (!ordered && c->info) c->info->out_of_order = true;
    if (c->dump && !ordered) bprintf(c, "  block %u chunks out of order\n", index);
done:
    free(found);
    free(s.straddle);
    return st;
}

static void block_ctx_free(block_ctx *c)
{
    if (c->zs_ready) inflateEnd(&c->zs);
    free(c->piece);
    memset(c, 0, sizeof *c);
}

/* ---- container --------------------------------------------------------------------- */
typedef struct pdn_container {
    const char *xml;
    size_t      xml_len;
    size_t      nrbf_off;
} pdn_container;

static pc_status read_container(const uint8_t *p, size_t n, pdn_container *c, pdn_info *info)
{
    size_t hl, i = 0;
    const char *v;
    size_t vl;
    uint32_t major;
    if (!p || n < 4u) return PC_ERR_FORMAT;
    if (memcmp(p, "PDN", 3u) != 0 || p[3] != '3') return PC_ERR_UNSUPPORTED;
    if (n < 7u) return PC_ERR_FORMAT;
    hl = (size_t)p[4] | ((size_t)p[5] << 8) | ((size_t)p[6] << 16);
    if (hl > n - 7u || n - 7u - hl < 2u) return PC_ERR_FORMAT;
    c->xml = (const char *)p + 7;
    c->xml_len = hl;
    if (hl >= 3u && memcmp(c->xml, "\xEF\xBB\xBF", 3u) == 0) i = 3u;
    while (i < hl && (c->xml[i] == ' ' || c->xml[i] == '\t' || c->xml[i] == '\r' ||
                      c->xml[i] == '\n'))
        i++;
    if (hl - i < 9u || memcmp(c->xml + i, "<pdnImage", 9u) != 0) return PC_ERR_FORMAT;
    if (xml_attr(c->xml + i + 1u, hl - i - 1u, "savedWithVersion", &v, &vl)) {
        if (parse_major(v, vl, &major) && major >= 6u) return PC_ERR_UNSUPPORTED;
        if (info && vl < sizeof info->saved_with) {
            memcpy(info->saved_with, v, vl);
            info->saved_with[vl] = '\0';
            if (parse_major(v, vl, &major)) info->version_major = major;
        }
    }
    c->nrbf_off = 7u + hl;
    if (p[c->nrbf_off] == 0x00u && p[c->nrbf_off + 1u] == 0x01u) {
        c->nrbf_off += 2u;
        return PC_OK;
    }
    /* 0x1F 0x8B: Paint.NET 2.x gzip-wrapped serialization; anything else is
     * a container this reader does not know. */
    return PC_ERR_UNSUPPORTED;
}

/* ---- load ------------------------------------------------------------------------------ */
pc_status pdn_load_ex(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                      pc_doc **out, pc_image_meta *meta, pdn_info *info)
{
    pdn_container c;
    pc_codec_limits dl;
    nrbf_opts no;
    nrbf_doc nd;
    pdn_model m;
    block_ctx bc;
    pc_doc *doc = NULL;
    pc_layer **layers = NULL;
    uint32_t *mb_layer = NULL;
    pc_rd rd;
    pc_status st;
    bool trunc = false;

    if (out) *out = NULL;
    if (meta) memset(meta, 0, sizeof *meta);
    if (info) {
        memset(info, 0, sizeof *info);
        info->block_format = 0xFFu;
    }
    if (!p || !out || !meta) return PC_ERR_ARG;
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    memset(&m, 0, sizeof m);
    memset(&bc, 0, sizeof bc);
    memset(&nd, 0, sizeof nd);

    st = read_container(p, n, &c, info);
    if (st != PC_OK) return st;
    pdn_opts(&no);
    st = nrbf_parse(p + c.nrbf_off, n - c.nrbf_off, &no, &nd);
    if (st != PC_OK) return st;
    st = build_model(&nd, lim, &m, info);
    if (st != PC_OK) goto fail;

    doc = pc_doc_create(m.w, m.h);
    layers = (pc_layer **)calloc(m.n, sizeof *layers);
    mb_layer = (uint32_t *)malloc((size_t)nd.n_objs * sizeof *mb_layer);
    if (!doc || !layers || !mb_layer) { st = PC_ERR_NOMEM; goto fail; }
    st = pc_doc_reserve_layers(doc, m.n);
    if (st != PC_OK) goto fail;
    memset(mb_layer, 0xFF, (size_t)nd.n_objs * sizeof *mb_layer);
    for (uint32_t i = 0; i < m.n; i++) {
        char name[PC_LAYER_NAME_MAX];
        if (copy_name(name, sizeof name, m.lay[i].name)) trunc = true;
        layers[i] = pc_layer_create(doc, name);
        if (!layers[i]) { st = PC_ERR_NOMEM; goto fail; }
        layers[i]->visible = m.lay[i].visible;
        layers[i]->opacity = m.lay[i].opacity;
        layers[i]->mode = m.lay[i].mode;
        mb_layer[m.lay[i].mb] = i;
        if (!m.lay[i].deferred) {       /* Paint.NET 2.x style inline pixels */
            sink s;
            memset(&s, 0, sizeof s);
            s.doc = doc;
            s.l = layers[i];
            s.w = m.w;
            st = store_px(&s, 0u, (uint64_t)m.w * m.h, m.lay[i].inline_data->data);
            if (st != PC_OK) goto fail;
        }
    }

    /* deferred data follows MessageEnd in the order the blocks were serialized */
    rd = pc_rd_make(p + c.nrbf_off + nd.end, n - c.nrbf_off - nd.end);
    bc.info = info;
    {
        uint32_t index = 0;
        for (uint32_t k = 0; k < nd.n_objs; k++) {
            const nrbf_obj *o = &nd.objs[k];
            int64_t len;
            bool deferred;
            uint32_t li;
            if (!cls_is(&nd, o, "PaintDotNet.MemoryBlock")) continue;
            if (!nrbf_as_bool(nrbf_member_value(&nd, o, "deferred"), &deferred) || !deferred)
                continue;
            if (!member_i64(&nd, o, "length64", &len) || len <= 0) {
                st = PC_ERR_FORMAT;
                goto fail;
            }
            li = mb_layer[k];
            st = read_block(&bc, &rd, (uint64_t)len, doc, li == NONE32 ? NULL : layers[li],
                            index++);
            if (st != PC_OK) goto fail;
        }
    }
    for (uint32_t i = 0; i < m.n; i++) {
        st = pc_doc_insert_layer(doc, layers[i], i);    /* cannot fail: reserved */
        if (st != PC_OK) goto fail;
        layers[i] = NULL;
    }
    meta->dpi_x = m.dpi_x;
    meta->dpi_y = m.dpi_y;
    meta->icc = m.icc;
    meta->icc_len = m.icc_len;
    m.icc = NULL;
    meta->src_bits = 8u;
    meta->had_alpha = true;
    st = read_items(&nd, m.meta_arr, meta);
    if (st != PC_OK) {
        pc_meta_free(meta);
        goto fail;
    }
    if (m.unknown_blend)
        snprintf(meta->note, sizeof meta->note, "Unknown layer blend modes were set to Normal.");
    else if (trunc)
        snprintf(meta->note, sizeof meta->note, "Long layer names were shortened.");
    if (info) {
        info->n_layers = m.n;
        info->names_truncated = trunc;
    }
    *out = doc;
    doc = NULL;
    st = PC_OK;
fail:
    if (layers) {
        for (uint32_t i = 0; i < m.n; i++) pc_layer_destroy(layers[i]);
        free(layers);
    }
    pc_doc_destroy(doc);
    free(mb_layer);
    block_ctx_free(&bc);
    model_free(&m);
    nrbf_free(&nd);
    return st;
}

/* ---- dump ---------------------------------------------------------------------------------- */
pc_status pdn_dump(const uint8_t *p, size_t n, uint32_t flags, pc_buf *out)
{
    pdn_container c;
    nrbf_opts no;
    nrbf_doc nd;
    block_ctx bc;
    pc_rd rd;
    pc_status st;
    char tmp[96];
    if (!out) return PC_ERR_ARG;
    memset(&nd, 0, sizeof nd);
    st = read_container(p, n, &c, NULL);
    if (st != PC_OK) {
        snprintf(tmp, sizeof tmp, "!! container: %s\n", pc_status_str(st));
        return pc_buf_append(out, tmp, strlen(tmp)) == PC_OK ? st : PC_ERR_NOMEM;
    }
    /* header XML: thumbnail elided, struct dumps also elide savedWithVersion */
    (void)pc_buf_append(out, "PDN3 header: ", 13u);
    for (size_t i = 0; i < c.xml_len;) {
        bool png = c.xml_len - i >= 5u && memcmp(c.xml + i, "png=\"", 5u) == 0;
        bool ver = (flags & NRBF_DUMP_STRUCT) && c.xml_len - i >= 18u &&
                   memcmp(c.xml + i, "savedWithVersion=\"", 18u) == 0;
        if (png || ver) {
            size_t s0 = i + (png ? 5u : 18u), e = s0;
            while (e < c.xml_len && c.xml[e] != '"') e++;
            (void)pc_buf_append(out, c.xml + i, s0 - i);
            if (ver) snprintf(tmp, sizeof tmp, "*");
            else if (flags & NRBF_DUMP_STRUCT) snprintf(tmp, sizeof tmp, "(png)");
            else snprintf(tmp, sizeof tmp, "(%zu chars)", e - s0);
            (void)pc_buf_append(out, tmp, strlen(tmp));
            i = e;
        } else {
            (void)pc_buf_append(out, c.xml + i, 1u);
            i++;
        }
    }
    (void)pc_buf_append(out, "\n", 1u);
    nrbf_opts_default(&no);
    no.libs = NULL;
    no.classes = NULL;
    no.dump = out;
    no.dump_flags = flags;
    st = nrbf_parse(p + c.nrbf_off, n - c.nrbf_off, &no, &nd);
    if (st != PC_OK) return st;
    memset(&bc, 0, sizeof bc);
    bc.dump = out;
    bc.dump_flags = flags;
    rd = pc_rd_make(p + c.nrbf_off + nd.end, n - c.nrbf_off - nd.end);
    {
        uint32_t index = 0;
        for (uint32_t k = 0; k < nd.n_objs && st == PC_OK; k++) {
            const nrbf_obj *o = &nd.objs[k];
            int64_t len;
            bool deferred;
            if (!cls_is(&nd, o, "PaintDotNet.MemoryBlock")) continue;
            if (!nrbf_as_bool(nrbf_member_value(&nd, o, "deferred"), &deferred) || !deferred)
                continue;
            if (!member_i64(&nd, o, "length64", &len) || len <= 0) { st = PC_ERR_FORMAT; break; }
            st = read_block(&bc, &rd, (uint64_t)len, NULL, NULL, index++);
        }
    }
    if (st == PC_OK) {
        snprintf(tmp, sizeof tmp, "trailing bytes: %zu\n", pc_rd_left(&rd));
        (void)pc_buf_append(out, tmp, strlen(tmp));
    } else {
        snprintf(tmp, sizeof tmp, "!! blocks: %s\n", pc_status_str(st));
        (void)pc_buf_append(out, tmp, strlen(tmp));
    }
    block_ctx_free(&bc);
    nrbf_free(&nd);
    return st;
}

/* ---- codec descriptor ----------------------------------------------------------------- */
static bool pdn_sniff(const uint8_t *p, size_t n)
{
    /* "PDN3" is the format this codec reads; "PDN" + another digit is
     * claimed too, so a newer revision reports PC_ERR_UNSUPPORTED instead of
     * "unknown file type". */
    return n >= 4u && memcmp(p, "PDN", 3u) == 0 && p[3] >= '0' && p[3] <= '9';
}

static pc_status pdn_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    return pdn_load_ex(p, n, lim, out, meta, NULL);
}

static pc_status pdn_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    (void)params;
    return pdn_save_ex(d, meta, NULL, par, out);
}

const pc_codec pc_codec_pdn = {
    "pdn", "Paint.NET image", "pdn",
    PC_CODEC_LOAD | PC_CODEC_SAVE | PC_CODEC_LAYERED,
    pdn_sniff, pdn_load,
    NULL, 0u, 0u,
    pdn_save
};
