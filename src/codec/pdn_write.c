/* pdn_write.c - Paint.NET .pdn writer (lane L6C, ADR-007).
 *
 * Mirrors the record structure of files saved by Paint.NET 5.1: the same
 * classes, libraries, member names and types, record kinds, object id
 * assignment (the .NET ObjectWriter consumes one id per object lookup,
 * including lookups of objects it has already seen, and gives value types
 * negative ids), metadata reuse through ClassWithId, the userMetadataItems
 * EXIF entries, the header XML with a PNG thumbnail, and gzip chunks of
 * 256 KiB with the header bytes .NET writes (mtime 0, XFL 0, OS 10).
 * docs/codecs/pdn.md lists the exact record sequence.
 */
#include "nrbf.h"
#include "pdn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib.h"

#define KVP_TYPE "System.Collections.Generic.KeyValuePair`2[[System.String, mscorlib, " \
    "Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089],[System.String, " \
    "mscorlib, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089]]"
#define KVP_ARRAY_TYPE KVP_TYPE "[]"

static const char *const k_op_names[PC_BLEND_COUNT] = {
    "PaintDotNet.UserBlendOps+NormalBlendOp", "PaintDotNet.UserBlendOps+MultiplyBlendOp",
    "PaintDotNet.UserBlendOps+AdditiveBlendOp", "PaintDotNet.UserBlendOps+ColorBurnBlendOp",
    "PaintDotNet.UserBlendOps+ColorDodgeBlendOp", "PaintDotNet.UserBlendOps+ReflectBlendOp",
    "PaintDotNet.UserBlendOps+GlowBlendOp", "PaintDotNet.UserBlendOps+OverlayBlendOp",
    "PaintDotNet.UserBlendOps+DifferenceBlendOp", "PaintDotNet.UserBlendOps+NegationBlendOp",
    "PaintDotNet.UserBlendOps+LightenBlendOp", "PaintDotNet.UserBlendOps+DarkenBlendOp",
    "PaintDotNet.UserBlendOps+ScreenBlendOp", "PaintDotNet.UserBlendOps+XorBlendOp"
};

void pdn_save_opts_default(pdn_save_opts *o)
{
    memset(o, 0, sizeof *o);
    o->version = PDN_COMPAT_VERSION;
    o->software = PDN_SOFTWARE;
    o->chunk_size = PDN_CHUNK_SIZE;
    o->level = PDN_GZIP_LEVEL;
    o->thumbnail = true;
    o->intern_names = true;
}

/* ---- id assignment (.NET ObjectWriter.InternalGetId) --------------------------- */
typedef struct idgen {
    int32_t        next;        /* m_currentId */
    const int32_t *prev;        /* previousObj, keyed by the id slot */
    int32_t        prev_id;
} idgen;

/* Reference object whose id lives in *slot (0 = not assigned yet). */
static int32_t id_ref(idgen *g, int32_t *slot)
{
    int32_t cur;
    if (slot == g->prev) return g->prev_id;
    cur = g->next++;
    if (*slot == 0) *slot = cur;
    g->prev = slot;
    g->prev_id = *slot;
    return *slot;
}

/* Value type instance: negative id, previousObj unchanged. */
static int32_t id_value(idgen *g) { return -(g->next++); }

/* ---- metadata items ------------------------------------------------------------ */
typedef struct kv_item {
    char   *key;
    char   *value;
    int32_t key_id, value_id;
} kv_item;

static pc_status exif_value(uint32_t id, uint32_t type, const uint8_t *data, size_t n,
                            char **out)
{
    pc_buf b;
    char head[96];
    pc_status st;
    memset(&b, 0, sizeof b);
    snprintf(head, sizeof head, "<exif id=\"%u\" len=\"%zu\" type=\"%u\" value=\"", id, n, type);
    st = pc_buf_append(&b, head, strlen(head));
    if (st == PC_OK) st = pdn_base64_encode(data, n, &b);
    if (st == PC_OK) st = pc_buf_append(&b, "\" />", 5u);     /* includes the NUL */
    if (st != PC_OK) { pc_buf_free(&b); return st; }
    *out = (char *)b.p;
    return PC_OK;
}

/* EXIF RATIONAL closest to dpi with a denominator up to 10^6 (continued
 * fractions), so resolutions read from a file are written back exactly. */
static void rational(double dpi, uint8_t out[8])
{
    uint32_t num = 96u, den = 1u;
    if (!(dpi > 0.0) || dpi > 4.0e6) dpi = 96.0;
    {
        double x = dpi;
        uint64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;   /* convergents h/k */
        for (int i = 0; i < 32; i++) {
            double a = floor(x);
            uint64_t ai = (uint64_t)a, h2 = ai * h1 + h0, k2 = ai * k1 + k0;
            if (k2 > 1000000u || h2 > 0xFFFFFFFFu) break;
            h0 = h1; h1 = h2; k0 = k1; k1 = k2;
            if (fabs((double)h1 / (double)k1 - dpi) <= dpi * 1e-15 || x - a < 1e-12) break;
            x = 1.0 / (x - a);
        }
        if (k1) { num = (uint32_t)h1; den = (uint32_t)k1; }
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(num >> (8 * i));
        out[4 + i] = (uint8_t)(den >> (8 * i));
    }
}

static void items_free(kv_item *it, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { free(it[i].key); free(it[i].value); }
}

/* Software, ResolutionUnit, XResolution, YResolution and the optional ICC
 * profile, keyed $exif.tagN[0] like a new Paint.NET 5.1 document. */
static pc_status build_items(const pc_image_meta *meta, const pdn_save_opts *o,
                             kv_item *it, uint32_t *n)
{
    uint8_t buf[8];
    pc_status st = PC_OK;
    uint32_t k = 0;
    double dx = meta && meta->dpi_x > 0.0 ? meta->dpi_x : 96.0;
    double dy = meta && meta->dpi_y > 0.0 ? meta->dpi_y : 96.0;
    *n = 0;
    if (o->software) {
        size_t sl = strlen(o->software);
        if (sl > 4096u) return PC_ERR_ARG;
        st = exif_value(305u, 2u, (const uint8_t *)o->software, sl + 1u, &it[k].value);
        if (st != PC_OK) return st;
        k++;
    }
    buf[0] = 2u; buf[1] = 0u;      /* inches */
    st = exif_value(296u, 3u, buf, 2u, &it[k].value);
    if (st == PC_OK) { k++; rational(dx, buf); st = exif_value(282u, 5u, buf, 8u, &it[k].value); }
    if (st == PC_OK) { k++; rational(dy, buf); st = exif_value(283u, 5u, buf, 8u, &it[k].value); }
    if (st == PC_OK) k++;
    if (st == PC_OK && meta && meta->icc && meta->icc_len) {
        if (meta->icc_len > (16u << 20)) st = PC_ERR_LIMIT;
        else st = exif_value(34675u, 7u, meta->icc, meta->icc_len, &it[k].value);
        if (st == PC_OK) k++;
    }
    for (uint32_t i = 0; i < k && st == PC_OK; i++) {
        it[i].key = (char *)malloc(32u);
        if (!it[i].key) { st = PC_ERR_NOMEM; break; }
        snprintf(it[i].key, 32u, "$exif.tag%u[0]", i);
    }
    *n = k;
    return st;
}

/* ---- NRBF document ----------------------------------------------------------------- */
typedef struct lay_ids {
    int32_t bl, bp, sf, lp, op, mb, name;
    uint32_t name_owner;        /* layer index whose name string is shared */
    uint32_t op_owner;          /* layer index whose blend op object is shared */
} lay_ids;

static pc_status put_bool(pc_buf *b, bool v) { return pc_buf_put_u8(b, v ? 1u : 0u); }

static pc_status write_nrbf(const pc_doc *d, const pdn_save_opts *o, kv_item *items,
                            uint32_t n_items, pc_buf *b)
{
    const uint32_t n = d->n_layers;
    char lib_data[160], lib_core[160];
    idgen g;
    int32_t doc = 0, lib_d = 0, lib_c = 0, list = 0, ver = 0, meta_arr = 0, arr = 0;
    int32_t empty = 0, kvp_meta = 0, bp_meta = 0, bm_meta = 0;
    int32_t op_meta[PC_BLEND_COUNT];
    uint32_t cap = 4u, major = 0, minor = 0, build = 0, rev = 0;
    pc_blend_mode bp_mode = PC_BLEND_NORMAL;
    lay_ids *L;
    pc_status st = PC_OK;
#define TRY(x) do { st = (x); if (st != PC_OK) goto done; } while (0)

    if (sscanf(o->version, "%u.%u.%u.%u", &major, &minor, &build, &rev) != 4 ||
        major > 0x7FFFFFFFu || minor > 0x7FFFFFFFu || build > 0x7FFFFFFFu || rev > 0x7FFFFFFFu)
        return PC_ERR_ARG;
    snprintf(lib_data, sizeof lib_data,
             "PaintDotNet.Data, Version=%s, Culture=neutral, PublicKeyToken=null", o->version);
    snprintf(lib_core, sizeof lib_core,
             "PaintDotNet.Core, Version=%s, Culture=neutral, PublicKeyToken=null", o->version);
    while (cap < n) cap *= 2u;      /* ArrayList growth from an empty list */
    if (o->list_capacity) {
        if (o->list_capacity < n || o->list_capacity > (1u << 20)) return PC_ERR_ARG;
        cap = o->list_capacity;
    }
    memset(op_meta, 0, sizeof op_meta);
    L = (lay_ids *)calloc(n, sizeof *L);
    if (!L) return PC_ERR_NOMEM;
    /* A layer duplicated in Paint.NET shares the name string and, while the
     * blend mode is unchanged, the blend op object of its source layer. */
    for (uint32_t i = 0; i < n; i++) {
        L[i].name_owner = i;
        L[i].op_owner = i;
        if (!o->intern_names) continue;
        for (uint32_t j = 0; j < i; j++) {
            if (strcmp(d->stack[j]->name, d->stack[i]->name) != 0) continue;
            L[i].name_owner = j;
            if (d->stack[j]->mode == d->stack[i]->mode) L[i].op_owner = L[j].op_owner;
            break;
        }
    }
    memset(&g, 0, sizeof g);
    g.next = 1;

    /* root: PaintDotNet.Document */
    (void)id_ref(&g, &doc);
    TRY(nrbf_put_header(b, doc, -1));
    TRY(nrbf_put_library(b, id_ref(&g, &lib_d), lib_data));
    {
        const nrbf_wmember m[6] = {
            { "isDisposed", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
            { "layers", NRBF_BT_CLASS, 0, "PaintDotNet.LayerList", lib_d },
            { "width", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "height", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "savedWith", NRBF_BT_SYSTEM_CLASS, 0, "System.Version", 0 },
            { "userMetadataItems", NRBF_BT_SYSTEM_CLASS, 0, KVP_ARRAY_TYPE, 0 },
        };
        TRY(nrbf_put_class(b, doc, "PaintDotNet.Document", m, 6u, lib_d));
    }
    TRY(put_bool(b, false));
    TRY(nrbf_put_ref(b, id_ref(&g, &list)));
    TRY(nrbf_put_i32(b, (int32_t)d->w));
    TRY(nrbf_put_i32(b, (int32_t)d->h));
    TRY(nrbf_put_ref(b, id_ref(&g, &ver)));
    TRY(nrbf_put_ref(b, id_ref(&g, &meta_arr)));

    /* PaintDotNet.LayerList (an ArrayList) */
    {
        const nrbf_wmember m[4] = {
            { "parent", NRBF_BT_CLASS, 0, "PaintDotNet.Document", lib_d },
            { "ArrayList+_items", NRBF_BT_OBJECT_ARRAY, 0, NULL, 0 },
            { "ArrayList+_size", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "ArrayList+_version", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
        };
        TRY(nrbf_put_class(b, list, "PaintDotNet.LayerList", m, 4u, lib_d));
    }
    TRY(nrbf_put_ref(b, id_ref(&g, &doc)));
    TRY(nrbf_put_ref(b, id_ref(&g, &arr)));
    TRY(nrbf_put_i32(b, (int32_t)n));
    TRY(nrbf_put_i32(b, (int32_t)n));

    /* System.Version */
    {
        const nrbf_wmember m[4] = {
            { "_Major", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "_Minor", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "_Build", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
            { "_Revision", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
        };
        TRY(nrbf_put_class(b, ver, "System.Version", m, 4u, 0));
        TRY(nrbf_put_i32(b, (int32_t)major));
        TRY(nrbf_put_i32(b, (int32_t)minor));
        TRY(nrbf_put_i32(b, (int32_t)build));
        TRY(nrbf_put_i32(b, (int32_t)rev));
    }

    /* userMetadataItems: KeyValuePair<string, string>[] */
    TRY(nrbf_put_sysclass_array(b, meta_arr, n_items, KVP_TYPE));
    for (uint32_t i = 0; i < n_items; i++) {
        int32_t id = id_value(&g);
        if (i == 0) {
            const nrbf_wmember m[2] = {
                { "key", NRBF_BT_STRING, 0, NULL, 0 },
                { "value", NRBF_BT_STRING, 0, NULL, 0 },
            };
            kvp_meta = id;
            TRY(nrbf_put_class(b, id, KVP_TYPE, m, 2u, 0));
        } else {
            TRY(nrbf_put_class_with_id(b, id, kvp_meta));
        }
        TRY(nrbf_put_string(b, id_ref(&g, &items[i].key_id), items[i].key, strlen(items[i].key)));
        TRY(nrbf_put_string(b, id_ref(&g, &items[i].value_id), items[i].value,
                            strlen(items[i].value)));
    }

    /* ArrayList._items */
    TRY(nrbf_put_object_array(b, arr, cap));
    for (uint32_t i = 0; i < n; i++) TRY(nrbf_put_ref(b, id_ref(&g, &L[i].bl)));
    TRY(nrbf_put_nulls(b, cap - n));

    /* PaintDotNet.BitmapLayer */
    for (uint32_t i = 0; i < n; i++) {
        if (i == 0) {
            nrbf_wmember m[6] = {
                { "properties", NRBF_BT_CLASS, 0, "PaintDotNet.BitmapLayer+BitmapLayerProperties",
                  0 },
                { "surface", NRBF_BT_CLASS, 0, "PaintDotNet.Surface", 0 },
                { "Layer+isDisposed", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
                { "Layer+width", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
                { "Layer+height", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
                { "Layer+properties", NRBF_BT_CLASS, 0, "PaintDotNet.Layer+LayerProperties", 0 },
            };
            TRY(nrbf_put_library(b, id_ref(&g, &lib_c), lib_core));
            m[0].type_lib = lib_d;
            m[1].type_lib = lib_c;
            m[5].type_lib = lib_d;
            TRY(nrbf_put_class(b, L[i].bl, "PaintDotNet.BitmapLayer", m, 6u, lib_d));
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].bl, L[0].bl));
        }
        TRY(nrbf_put_ref(b, id_ref(&g, &L[i].bp)));
        TRY(nrbf_put_ref(b, id_ref(&g, &L[i].sf)));
        TRY(put_bool(b, false));
        TRY(nrbf_put_i32(b, (int32_t)d->w));
        TRY(nrbf_put_i32(b, (int32_t)d->h));
        TRY(nrbf_put_ref(b, id_ref(&g, &L[i].lp)));
    }

    /* per layer: BitmapLayerProperties, Surface, LayerProperties */
    for (uint32_t i = 0; i < n; i++) {
        const pc_layer *l = d->stack[i];
        pc_blend_mode mode = (uint32_t)l->mode < PC_BLEND_COUNT ? l->mode : PC_BLEND_NORMAL;
        lay_ids *own = &L[L[i].name_owner];
        int32_t bm;
        /* .NET caches the first metadata of a type and reuses it only while
         * the runtime member types match (the blendOp class varies). */
        if (i == 0 || mode != bp_mode) {
            nrbf_wmember m[1] = { { "blendOp", NRBF_BT_CLASS, 0, k_op_names[mode], 0 } };
            m[0].type_lib = lib_d;
            TRY(nrbf_put_class(b, L[i].bp, "PaintDotNet.BitmapLayer+BitmapLayerProperties", m, 1u,
                               lib_d));
            if (i == 0) { bp_meta = L[i].bp; bp_mode = mode; }
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].bp, bp_meta));
        }
        TRY(nrbf_put_ref(b, id_ref(&g, &L[L[i].op_owner].op)));

        if (i == 0) {
            nrbf_wmember m[4] = {
                { "width", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
                { "height", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
                { "stride", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
                { "scan0", NRBF_BT_CLASS, 0, "PaintDotNet.MemoryBlock", 0 },
            };
            m[3].type_lib = lib_c;
            TRY(nrbf_put_class(b, L[i].sf, "PaintDotNet.Surface", m, 4u, lib_c));
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].sf, L[0].sf));
        }
        TRY(nrbf_put_i32(b, (int32_t)d->w));
        TRY(nrbf_put_i32(b, (int32_t)d->h));
        TRY(nrbf_put_i32(b, (int32_t)(d->w * 4u)));
        TRY(nrbf_put_ref(b, id_ref(&g, &L[i].mb)));

        if (i == 0) {
            nrbf_wmember m[6] = {
                { "name", NRBF_BT_STRING, 0, NULL, 0 },
                { "userMetadataItems", NRBF_BT_SYSTEM_CLASS, 0, KVP_ARRAY_TYPE, 0 },
                { "visible", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
                { "isBackground", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
                { "opacity", NRBF_BT_PRIMITIVE, NRBF_P_BYTE, NULL, 0 },
                { "blendMode", NRBF_BT_CLASS, 0, "PaintDotNet.LayerBlendMode", 0 },
            };
            m[5].type_lib = lib_d;
            TRY(nrbf_put_class(b, L[i].lp, "PaintDotNet.Layer+LayerProperties", m, 6u, lib_d));
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].lp, L[0].lp));
        }
        if (own->name == 0) {
            int32_t id = id_ref(&g, &own->name);
            TRY(nrbf_put_string(b, id, l->name, strlen(l->name)));
        } else {
            TRY(nrbf_put_ref(b, id_ref(&g, &own->name)));
        }
        TRY(nrbf_put_ref(b, id_ref(&g, &empty)));
        TRY(put_bool(b, l->visible));
        TRY(put_bool(b, i == 0));
        TRY(pc_buf_put_u8(b, l->opacity));
        bm = id_value(&g);
        if (i == 0) {
            const nrbf_wmember m[1] = { { "value__", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 } };
            bm_meta = bm;
            TRY(nrbf_put_class(b, bm, "PaintDotNet.LayerBlendMode", m, 1u, lib_d));
        } else {
            TRY(nrbf_put_class_with_id(b, bm, bm_meta));
        }
        TRY(nrbf_put_i32(b, (int32_t)mode));
    }

    /* objects scheduled by the loop above, in queue order */
    for (uint32_t i = 0; i < n; i++) {
        const pc_layer *l = d->stack[i];
        pc_blend_mode mode = (uint32_t)l->mode < PC_BLEND_COUNT ? l->mode : PC_BLEND_NORMAL;
        if (L[i].op_owner != i) {
            /* shared blend op: written once, with its owner */
        } else if (op_meta[mode] == 0) {
            op_meta[mode] = L[i].op;
            TRY(nrbf_put_class(b, L[i].op, k_op_names[mode], NULL, 0u, lib_d));
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].op, op_meta[mode]));
        }
        if (i == 0) {
            nrbf_wmember m[3] = {
                { "length64", NRBF_BT_PRIMITIVE, NRBF_P_INT64, NULL, 0 },
                { "hasParent", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
                { "deferred", NRBF_BT_PRIMITIVE, NRBF_P_BOOLEAN, NULL, 0 },
            };
            TRY(nrbf_put_class(b, L[i].mb, "PaintDotNet.MemoryBlock", m, 3u, lib_c));
        } else {
            TRY(nrbf_put_class_with_id(b, L[i].mb, L[0].mb));
        }
        TRY(nrbf_put_i64(b, (int64_t)d->w * (int64_t)d->h * 4));
        TRY(put_bool(b, false));
        TRY(put_bool(b, true));
        if (i == 0) TRY(nrbf_put_sysclass_array(b, empty, 0u, KVP_TYPE));
    }
    TRY(nrbf_put_end(b));
done:
#undef TRY
    free(L);
    return st;
}

/* ---- deferred pixel data --------------------------------------------------------------- */
typedef struct cjob {
    const pc_doc   *d;
    const pc_layer *l;
    uint64_t        len;
    uint32_t        cs, first;
    int             level;
    uint8_t       **scratch;    /* per worker, cs + 8 bytes */
    uint8_t       **out;        /* per chunk of the batch */
    uint32_t       *out_len;
    pc_status      *st;
} cjob;

/* Copy bytes [off, off + n) of the layer's BGRA image to dst (n + 8 bytes
 * of room); returns the offset of the first requested byte in dst. */
static size_t read_bytes(const pc_doc *d, const pc_layer *l, uint64_t off, uint64_t n,
                         uint8_t *dst)
{
    uint64_t p = off / 4u, end = (off + n + 3u) / 4u;
    uint8_t *o = dst;
    while (p < end) {
        uint32_t y = (uint32_t)(p / d->w), x = (uint32_t)(p % d->w);
        uint64_t k;
        if (x == 0u && end - p >= d->w) {
            uint64_t rows = (end - p) / d->w;
            pc_layer_read_rect(d, l, pc_rect_make(0, (int32_t)y, (int32_t)d->w, (int32_t)rows),
                               (pc_px32 *)(void *)o, d->w);
            k = rows * d->w;
        } else {
            k = d->w - x;
            if (k > end - p) k = end - p;
            pc_layer_read_rect(d, l, pc_rect_make((int32_t)x, (int32_t)y, (int32_t)k, 1),
                               (pc_px32 *)(void *)o, d->w);
        }
        p += k;
        o += k * 4u;
    }
    return (size_t)(off & 3u);
}

static pc_status gzip_chunk(const uint8_t *src, uint32_t n, int level, uint8_t **out,
                            uint32_t *out_len)
{
    z_stream zs;
    gz_header gh;
    uLong bound;
    uint8_t *buf;
    int r;
    memset(&zs, 0, sizeof zs);
    memset(&gh, 0, sizeof gh);
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return PC_ERR_NOMEM;
    gh.os = 10;                 /* what .NET's zlib writes (OS_CODE of Windows builds) */
    if (deflateSetHeader(&zs, &gh) != Z_OK) { deflateEnd(&zs); return PC_ERR_NOMEM; }
    bound = deflateBound(&zs, n);
    if (bound > 0xFFFFFFFFu) { deflateEnd(&zs); return PC_ERR_LIMIT; }
    buf = (uint8_t *)malloc((size_t)bound);
    if (!buf) { deflateEnd(&zs); return PC_ERR_NOMEM; }
    zs.next_in = (Bytef *)(uintptr_t)src;
    zs.avail_in = n;
    zs.next_out = buf;
    zs.avail_out = (uInt)bound;
    r = deflate(&zs, Z_FINISH);
    *out_len = (uint32_t)(bound - zs.avail_out);
    deflateEnd(&zs);
    if (r != Z_STREAM_END) { free(buf); return PC_ERR_NOMEM; }
    *out = buf;
    return PC_OK;
}

static void chunk_job(void *ud, uint32_t index, uint32_t worker)
{
    cjob *j = (cjob *)ud;
    uint64_t off = (uint64_t)(j->first + index) * j->cs;
    uint32_t n = (uint32_t)(j->len - off < j->cs ? j->len - off : j->cs);
    uint8_t *raw = j->scratch[worker];
    size_t at = read_bytes(j->d, j->l, off, n, raw);
    if (j->level < 0) {
        uint8_t *copy = (uint8_t *)malloc(n);
        if (!copy) { j->st[index] = PC_ERR_NOMEM; return; }
        memcpy(copy, raw + at, n);
        j->out[index] = copy;
        j->out_len[index] = n;
        j->st[index] = PC_OK;
        return;
    }
    j->st[index] = gzip_chunk(raw + at, n, j->level, &j->out[index], &j->out_len[index]);
}

static pc_status write_block(const pc_doc *d, const pc_layer *l, const pdn_save_opts *o,
                             const pc_par *par, pc_buf *b)
{
    uint64_t len = (uint64_t)d->w * d->h * 4u;
    uint32_t cs = o->chunk_size, threads = pc_par_threads(par), batch;
    uint64_t count = (len + cs - 1u) / cs;
    uint8_t **scratch, **outs;
    uint32_t *lens;
    pc_status *sts, st;
    cjob j;
    batch = (uint32_t)((32u << 20) / cs);
    if (batch < 1u) batch = 1u;
    if (batch > 64u) batch = 64u;
    if ((uint64_t)batch > count) batch = (uint32_t)count;
    if (count > 0xFFFFFFFFu) return PC_ERR_LIMIT;
    st = pc_buf_put_u8(b, o->level < 0 ? 1u : 0u);
    if (st == PC_OK) st = pc_buf_put_be32(b, cs);
    if (st != PC_OK) return st;
    scratch = (uint8_t **)calloc(threads, sizeof *scratch);
    outs = (uint8_t **)calloc(batch, sizeof *outs);
    lens = (uint32_t *)calloc(batch, sizeof *lens);
    sts = (pc_status *)calloc(batch, sizeof *sts);
    if (!scratch || !outs || !lens || !sts) { st = PC_ERR_NOMEM; goto done; }
    for (uint32_t t = 0; t < threads; t++) {
        scratch[t] = (uint8_t *)malloc((size_t)cs + 8u);
        if (!scratch[t]) { st = PC_ERR_NOMEM; goto done; }
    }
    memset(&j, 0, sizeof j);
    j.d = d; j.l = l; j.len = len; j.cs = cs; j.level = o->level;
    j.scratch = scratch; j.out = outs; j.out_len = lens; j.st = sts;
    for (uint64_t c0 = 0; c0 < count; c0 += batch) {
        uint32_t nb = (uint32_t)(count - c0 < batch ? count - c0 : batch);
        /* reverse_chunks (tests): batches and chunks in descending order */
        uint64_t first = o->reverse_chunks ? count - c0 - nb : c0;
        j.first = (uint32_t)first;
        for (uint32_t k = 0; k < nb; k++) { outs[k] = NULL; sts[k] = PC_ERR_NOMEM; }
        pc_par_for(par, chunk_job, &j, nb);
        for (uint32_t k = 0; k < nb; k++) {
            uint32_t idx = o->reverse_chunks ? nb - 1u - k : k;
            if (st == PC_OK) st = sts[idx];
            if (st == PC_OK) st = pc_buf_put_be32(b, (uint32_t)first + idx);
            if (st == PC_OK) st = pc_buf_put_be32(b, lens[idx]);
            if (st == PC_OK) st = pc_buf_append(b, outs[idx], lens[idx]);
        }
        for (uint32_t k = 0; k < nb; k++) { free(outs[k]); outs[k] = NULL; }
        if (st != PC_OK) goto done;
    }
done:
    if (scratch)
        for (uint32_t t = 0; t < threads; t++) free(scratch[t]);
    free(scratch); free(outs); free(lens); free(sts);
    return st;
}

/* ---- file ---------------------------------------------------------------------------- */
pc_status pdn_save_ex(const pc_doc *d, const pc_image_meta *meta, const pdn_save_opts *opt,
                      const pc_par *par, pc_buf *out)
{
    pdn_save_opts o;
    kv_item items[6];
    uint32_t n_items = 0;
    pc_buf hdr, png;
    char head[256];
    pc_status st;
    if (!d || !out || d->n_layers == 0u || d->w == 0u || d->h == 0u ||
        d->w > PC_MAX_DIM || d->h > PC_MAX_DIM)
        return PC_ERR_ARG;
    if (opt) o = *opt;
    else pdn_save_opts_default(&o);
    if (!o.version) o.version = PDN_COMPAT_VERSION;
    if (o.chunk_size < 4u || o.chunk_size > (64u << 20) || o.level > 9 || o.level < -1)
        return PC_ERR_ARG;
    if (strlen(o.version) > 40u) return PC_ERR_ARG;
    if (d->n_layers > 0x3FFFFFFFu) return PC_ERR_LIMIT;
    memset(items, 0, sizeof items);
    memset(&hdr, 0, sizeof hdr);
    memset(&png, 0, sizeof png);

    st = build_items(meta, &o, items, &n_items);
    if (st != PC_OK) goto done;
    /* header XML */
    snprintf(head, sizeof head,
             "<pdnImage width=\"%u\" height=\"%u\" layers=\"%u\" savedWithVersion=\"%s\">"
             "<custom>", d->w, d->h, d->n_layers, o.version);
    st = pc_buf_append(&hdr, head, strlen(head));
    if (st == PC_OK && o.thumbnail) {
        st = pdn_thumbnail_png(d, par, &png);
        if (st == PC_OK) st = pc_buf_append(&hdr, "<thumb png=\"", 12u);
        if (st == PC_OK) st = pdn_base64_encode(png.p, png.n, &hdr);
        if (st == PC_OK) st = pc_buf_append(&hdr, "\" />", 4u);
    }
    if (st == PC_OK) st = pc_buf_append(&hdr, "</custom></pdnImage>", 20u);
    if (st != PC_OK) goto done;
    if (hdr.n > 0xFFFFFFu) { st = PC_ERR_LIMIT; goto done; }

    st = pc_buf_append(out, "PDN3", 4u);
    if (st == PC_OK) st = pc_buf_put_u8(out, (uint8_t)hdr.n);
    if (st == PC_OK) st = pc_buf_put_u8(out, (uint8_t)(hdr.n >> 8));
    if (st == PC_OK) st = pc_buf_put_u8(out, (uint8_t)(hdr.n >> 16));
    if (st == PC_OK) st = pc_buf_append(out, hdr.p, hdr.n);
    if (st == PC_OK) st = pc_buf_put_u8(out, 0x00u);
    if (st == PC_OK) st = pc_buf_put_u8(out, 0x01u);
    if (st == PC_OK) st = write_nrbf(d, &o, items, n_items, out);
    for (uint32_t i = 0; i < d->n_layers && st == PC_OK; i++)
        st = write_block(d, d->stack[i], &o, par, out);
done:
    items_free(items, n_items < 6u ? 6u : n_items);
    pc_buf_free(&hdr);
    pc_buf_free(&png);
    return st;
}
