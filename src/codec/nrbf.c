/* nrbf.c - bounded, whitelisted MS-NRBF reader and record writer (lane L6C).
 *
 * Format reference: [MS-NRBF] .NET Remoting: Binary Format Data Structure
 * (Microsoft Open Specifications). Written from the specification; see
 * docs/codecs/pdn.md for the subset a Paint.NET document uses.
 */
#include "nrbf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NRBF_NONE 0xFFFFFFFFu

/* ---- options ------------------------------------------------------------- */
void nrbf_limits_default(nrbf_limits *l)
{
    l->max_objects = 1u << 18;
    l->max_values = 1u << 21;
    l->max_depth = 32u;
    l->max_members = 256u;
    l->max_classes = 1u << 14;
    l->max_libs = 64u;
    l->max_string = 16u << 20;
    l->max_array = 1u << 20;
    l->max_bytes = SIZE_MAX;
}

void nrbf_opts_default(nrbf_opts *o)
{
    memset(o, 0, sizeof *o);
    nrbf_limits_default(&o->lim);
}

/* ---- small helpers -------------------------------------------------------- */
static pc_status grow(void **p, uint32_t *cap, uint32_t need, size_t elem, uint32_t hard)
{
    uint32_t nc;
    size_t bytes;
    void *q;
    if (need <= *cap) return PC_OK;
    if (need > hard) return PC_ERR_LIMIT;
    nc = *cap ? *cap : 16u;
    while (nc < need) nc = (nc > hard / 2u) ? hard : nc * 2u;
    if (nc < need) nc = need;
    if (!pc_mul_size(nc, elem, &bytes)) return PC_ERR_LIMIT;
    q = realloc(*p, bytes);
    if (!q) return PC_ERR_NOMEM;
    *p = q;
    *cap = nc;
    return PC_OK;
}

static uint32_t hash_id(int32_t id, uint32_t mask)
{
    return ((uint32_t)id * 2654435761u) & mask;
}

bool nrbf_str_eq(nrbf_str s, const char *z)
{
    size_t n = strlen(z);
    return n == s.n && (n == 0u || memcmp(s.p, z, n) == 0);
}

static bool str_prefix(nrbf_str s, const char *z, size_t n)
{
    return s.n >= n && memcmp(s.p, z, n) == 0;
}

static bool name_allowed(const char *const *list, nrbf_str s)
{
    if (!list) return true;
    for (size_t i = 0; list[i]; i++) {
        size_t n = strlen(list[i]);
        if (n > 0u && list[i][n - 1u] == '*') {
            if (str_prefix(s, list[i], n - 1u)) return true;
        } else if (nrbf_str_eq(s, list[i])) {
            return true;
        }
    }
    return false;
}

static bool lib_allowed(const char *const *list, nrbf_str s)
{
    if (!list) return true;
    for (size_t i = 0; list[i]; i++)
        if (str_prefix(s, list[i], strlen(list[i]))) return true;
    return false;
}

/* ---- lookups ----------------------------------------------------------------- */
static uint32_t find_index(const nrbf_doc *d, int32_t id)
{
    uint32_t mask, h;
    if (!d->hash_cap) return NRBF_NONE;
    mask = d->hash_cap - 1u;
    for (h = hash_id(id, mask);; h = (h + 1u) & mask) {
        uint32_t e = d->hash[h];
        if (e == 0u) return NRBF_NONE;
        if (d->objs[e - 1u].id == id) return e - 1u;
    }
}

const nrbf_obj *nrbf_get(const nrbf_doc *d, int32_t id)
{
    uint32_t i = find_index(d, id);
    return i == NRBF_NONE ? NULL : &d->objs[i];
}

const nrbf_class *nrbf_class_of(const nrbf_doc *d, const nrbf_obj *o)
{
    if (!o || o->kind != NRBF_O_CLASS || o->cls >= d->n_classes) return NULL;
    return &d->classes[o->cls];
}

const nrbf_value *nrbf_member_value(const nrbf_doc *d, const nrbf_obj *o, const char *name)
{
    const nrbf_class *c = nrbf_class_of(d, o);
    if (!c) return NULL;
    for (uint32_t i = 0; i < c->n_members; i++)
        if (nrbf_str_eq(d->members[c->first_member + i].name, name))
            return &d->vals[o->first + i];
    return NULL;
}

const nrbf_value *nrbf_elem(const nrbf_doc *d, const nrbf_obj *o, uint32_t i)
{
    if (!o || o->kind != NRBF_O_ARRAY || i >= o->n) return NULL;
    return &d->vals[o->first + i];
}

const nrbf_obj *nrbf_deref(const nrbf_doc *d, const nrbf_value *v)
{
    if (!v || v->kind != NRBF_V_OBJ) return NULL;
    return nrbf_get(d, v->id);
}

bool nrbf_as_i64(const nrbf_value *v, int64_t *out)
{
    if (!v || v->kind != NRBF_V_PRIM) return false;
    switch (v->prim) {
    case NRBF_P_BYTE: case NRBF_P_SBYTE: case NRBF_P_INT16: case NRBF_P_UINT16:
    case NRBF_P_INT32: case NRBF_P_UINT32: case NRBF_P_INT64:
        *out = v->v.i;
        return true;
    case NRBF_P_UINT64:
        if (v->v.u > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)v->v.u;
        return true;
    default:
        return false;
    }
}

bool nrbf_as_bool(const nrbf_value *v, bool *out)
{
    if (!v || v->kind != NRBF_V_PRIM || v->prim != NRBF_P_BOOLEAN) return false;
    *out = v->v.u != 0u;
    return true;
}

void nrbf_free(nrbf_doc *d)
{
    if (!d) return;
    free(d->objs);
    free(d->vals);
    free(d->classes);
    free(d->members);
    free(d->libs);
    free(d->hash);
    memset(d, 0, sizeof *d);
}

/* ---- parser ----------------------------------------------------------------- */
typedef struct frame {
    uint32_t obj;       /* objs index being filled */
    uint32_t base;      /* first value slot */
    uint32_t next, count;
    uint32_t cls;       /* class index, NRBF_NONE for arrays */
} frame;

typedef struct parser {
    pc_rd            rd;
    const nrbf_opts *o;
    nrbf_doc        *d;
    frame           *stack;
    uint32_t         sp, stack_cap;
    /* dump state */
    pc_buf          *dump;
    uint32_t         dflags;
    pc_status        dump_st;
    int32_t         *nid_keys;  /* id normalization map (struct dumps) */
    uint32_t        *nid_vals;
    uint32_t         nid_cap, nid_n;
} parser;

enum { CTX_TOP = 0, CTX_MEMBER = 1, CTX_ELEM = 2 };

static pc_status rd_check(parser *P)
{
    return P->rd.err ? PC_ERR_FORMAT : PC_OK;
}

static int32_t rd_i32(parser *P) { return (int32_t)pc_rd_le32(&P->rd); }

/* ---- dump output ---- */
static void dputs(parser *P, const char *s)
{
    if (P->dump && P->dump_st == PC_OK) P->dump_st = pc_buf_append(P->dump, s, strlen(s));
}

static void dprintf_(parser *P, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;
    if (!P->dump || P->dump_st != PC_OK) return;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    P->dump_st = pc_buf_append(P->dump, tmp, (size_t)n);
}

static void dindent(parser *P)
{
    for (uint32_t i = 0; i < P->sp; i++) dputs(P, "  ");
}

static void dstr(parser *P, nrbf_str s, uint32_t max)
{
    uint32_t n = s.n < max ? s.n : max;
    char esc[8];
    dputs(P, "\"");
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; esc[2] = 0; dputs(P, esc); }
        else if (c < 0x20u || c == 0x7Fu) {
            snprintf(esc, sizeof esc, "\\x%02x", c);
            dputs(P, esc);
        }
        else { esc[0] = (char)c; esc[1] = 0; dputs(P, esc); }
    }
    dputs(P, n < s.n ? "\"..." : "\"");
}

/* Library and type names: struct dumps keep only the assembly simple name. */
static void dname(parser *P, nrbf_str s, bool is_lib)
{
    uint32_t n = s.n;
    if ((P->dflags & NRBF_DUMP_STRUCT) && is_lib) {
        for (uint32_t i = 0; i < s.n; i++)
            if (s.p[i] == ',') { n = i; break; }
    }
    if (P->dump && P->dump_st == PC_OK) P->dump_st = pc_buf_append(P->dump, s.p, n);
}

static uint32_t nid_lookup(parser *P, int32_t id)
{
    uint32_t mask, h;
    if (P->nid_n * 2u + 2u > P->nid_cap) {     /* grow */
        uint32_t nc = P->nid_cap ? P->nid_cap * 2u : 256u;
        int32_t *k = (int32_t *)calloc(nc, sizeof *k);
        uint32_t *v = (uint32_t *)calloc(nc, sizeof *v);
        if (!k || !v) { free(k); free(v); P->dump_st = PC_ERR_NOMEM; return 0u; }
        for (uint32_t i = 0; i < P->nid_cap; i++) {
            if (!P->nid_vals[i]) continue;
            for (h = hash_id(P->nid_keys[i], nc - 1u); v[h]; h = (h + 1u) & (nc - 1u)) {}
            k[h] = P->nid_keys[i];
            v[h] = P->nid_vals[i];
        }
        free(P->nid_keys); free(P->nid_vals);
        P->nid_keys = k; P->nid_vals = v; P->nid_cap = nc;
    }
    mask = P->nid_cap - 1u;
    for (h = hash_id(id, mask); P->nid_vals[h]; h = (h + 1u) & mask)
        if (P->nid_keys[h] == id) return P->nid_vals[h];
    P->nid_keys[h] = id;
    P->nid_vals[h] = ++P->nid_n;
    return P->nid_n;
}

static void did(parser *P, int32_t id)
{
    if (!P->dump) return;
    if ((P->dflags & NRBF_DUMP_STRUCT) && !(P->dflags & NRBF_DUMP_KEEP_IDS)) {
        uint32_t k = nid_lookup(P, id);
        dprintf_(P, "#%s%u", id < 0 ? "-" : "", k);
    } else {
        dprintf_(P, "#%d", (int)id);
    }
}

static const char *prim_name(uint8_t p)
{
    static const char *const names[19] = {
        "?", "Boolean", "Byte", "Char", "?", "Decimal", "Double", "Int16", "Int32",
        "Int64", "SByte", "Single", "TimeSpan", "DateTime", "UInt16", "UInt32",
        "UInt64", "Null", "String"
    };
    return p < 19u ? names[p] : "?";
}

static void dvalue_prim(parser *P, const nrbf_value *v)
{
    if (!P->dump) return;
    dputs(P, prim_name(v->prim));
    if (P->dflags & NRBF_DUMP_STRUCT) return;
    switch (v->prim) {
    case NRBF_P_BOOLEAN: dputs(P, v->v.u ? " true" : " false"); break;
    case NRBF_P_DOUBLE: case NRBF_P_SINGLE: dprintf_(P, " %.17g", v->v.f); break;
    case NRBF_P_UINT64: case NRBF_P_DATETIME: case NRBF_P_UINT32: case NRBF_P_UINT16:
    case NRBF_P_BYTE: case NRBF_P_CHAR:
        dprintf_(P, " %llu", (unsigned long long)v->v.u); break;
    case NRBF_P_DECIMAL: break;
    default: dprintf_(P, " %lld", (long long)v->v.i); break;
    }
}

static void dslot_prefix(parser *P, uint32_t ctx)
{
    const frame *f;
    if (!P->dump || ctx == CTX_TOP) return;
    f = &P->stack[P->sp - 1u];
    dindent(P);
    if (f->cls != NRBF_NONE) {
        const nrbf_member *m = &P->d->members[P->d->classes[f->cls].first_member + f->next];
        dputs(P, ".");
        dname(P, m->name, false);
        dputs(P, " = ");
    } else {
        dprintf_(P, "[%u] = ", f->next);
    }
}

static void dclass_members(parser *P, const nrbf_class *c)
{
    dputs(P, " {");
    for (uint32_t i = 0; i < c->n_members; i++) {
        const nrbf_member *m = &P->d->members[c->first_member + i];
        if (i) dputs(P, ", ");
        dname(P, m->name, false);
        if (!c->has_types) continue;
        switch (m->btype) {
        case NRBF_BT_PRIMITIVE: dprintf_(P, ":%s", prim_name(m->prim)); break;
        case NRBF_BT_STRING: dputs(P, ":String"); break;
        case NRBF_BT_OBJECT: dputs(P, ":Object"); break;
        case NRBF_BT_SYSTEM_CLASS: dputs(P, ":SystemClass("); dname(P, m->type_name, false);
            dputs(P, ")"); break;
        case NRBF_BT_CLASS: dputs(P, ":Class("); dname(P, m->type_name, false); dputs(P, ",");
            did(P, m->type_lib); dputs(P, ")"); break;
        case NRBF_BT_OBJECT_ARRAY: dputs(P, ":Object[]"); break;
        case NRBF_BT_STRING_ARRAY: dputs(P, ":String[]"); break;
        case NRBF_BT_PRIMITIVE_ARRAY: dprintf_(P, ":%s[]", prim_name(m->prim)); break;
        default: dputs(P, ":?"); break;
        }
    }
    dputs(P, "}");
}

/* ---- primitive values ---- */
static pc_status rd_lps(parser *P, nrbf_str *s)
{
    uint32_t len = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t b = pc_rd_u8(&P->rd);
        if (P->rd.err) return PC_ERR_FORMAT;
        if (i == 4u && (b & 0xF8u)) return PC_ERR_FORMAT;   /* > INT32_MAX */
        len |= (b & 0x7Fu) << (7u * i);
        if (!(b & 0x80u)) break;
        if (i == 4u) return PC_ERR_FORMAT;
    }
    if (len > P->o->lim.max_string) return PC_ERR_LIMIT;
    if (len > pc_rd_left(&P->rd)) return PC_ERR_FORMAT;
    s->p = (const char *)(P->rd.p + P->rd.pos);
    s->n = len;
    (void)pc_rd_skip(&P->rd, len);
    return PC_OK;
}

static size_t prim_size(uint8_t p)
{
    switch (p) {
    case NRBF_P_BOOLEAN: case NRBF_P_BYTE: case NRBF_P_SBYTE: return 1u;
    case NRBF_P_INT16: case NRBF_P_UINT16: return 2u;
    case NRBF_P_INT32: case NRBF_P_UINT32: case NRBF_P_SINGLE: return 4u;
    case NRBF_P_INT64: case NRBF_P_UINT64: case NRBF_P_DOUBLE: case NRBF_P_TIMESPAN:
    case NRBF_P_DATETIME: return 8u;
    default: return 0u;     /* Char, Decimal: variable; others invalid */
    }
}

static bool prim_valid(uint8_t p)
{
    return p >= 1u && p <= 16u && p != 4u;
}

static pc_status rd_prim(parser *P, uint8_t p, nrbf_value *v)
{
    pc_rd *r = &P->rd;
    memset(v, 0, sizeof *v);
    v->kind = NRBF_V_PRIM;
    v->prim = p;
    switch (p) {
    case NRBF_P_BOOLEAN: case NRBF_P_BYTE: v->v.u = pc_rd_u8(r); break;
    case NRBF_P_SBYTE: v->v.i = (int8_t)pc_rd_u8(r); break;
    case NRBF_P_INT16: v->v.i = (int16_t)pc_rd_le16(r); break;
    case NRBF_P_UINT16: v->v.u = pc_rd_le16(r); break;
    case NRBF_P_INT32: v->v.i = (int32_t)pc_rd_le32(r); break;
    case NRBF_P_UINT32: v->v.u = pc_rd_le32(r); break;
    case NRBF_P_INT64: case NRBF_P_TIMESPAN: v->v.i = (int64_t)pc_rd_le64(r); break;
    case NRBF_P_UINT64: case NRBF_P_DATETIME: v->v.u = pc_rd_le64(r); break;
    case NRBF_P_SINGLE: {
        uint32_t u = pc_rd_le32(r);
        float f;
        memcpy(&f, &u, sizeof f);
        v->v.f = (double)f;
        break;
    }
    case NRBF_P_DOUBLE: {
        uint64_t u = pc_rd_le64(r);
        memcpy(&v->v.f, &u, sizeof v->v.f);
        break;
    }
    case NRBF_P_CHAR: {     /* one UTF-8 encoded character */
        uint32_t b = pc_rd_u8(r), extra, cp;
        if (b < 0x80u) { extra = 0u; cp = b; }
        else if ((b & 0xE0u) == 0xC0u) { extra = 1u; cp = b & 0x1Fu; }
        else if ((b & 0xF0u) == 0xE0u) { extra = 2u; cp = b & 0x0Fu; }
        else if ((b & 0xF8u) == 0xF0u) { extra = 3u; cp = b & 0x07u; }
        else return PC_ERR_FORMAT;
        for (uint32_t i = 0; i < extra; i++) {
            uint32_t c = pc_rd_u8(r);
            if ((c & 0xC0u) != 0x80u) return PC_ERR_FORMAT;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        v->v.u = cp;
        break;
    }
    case NRBF_P_DECIMAL: {
        nrbf_str s;
        pc_status st = rd_lps(P, &s);
        if (st != PC_OK) return st;
        break;
    }
    default:
        return PC_ERR_FORMAT;
    }
    return rd_check(P);
}

/* ---- object table ---- */
static pc_status hash_insert(nrbf_doc *d, uint32_t index)
{
    uint32_t mask, h;
    if ((d->n_objs + 1u) * 2u > d->hash_cap) {
        uint32_t nc = d->hash_cap ? d->hash_cap * 2u : 64u;
        uint32_t *nh = (uint32_t *)calloc(nc, sizeof *nh);
        if (!nh) return PC_ERR_NOMEM;
        for (uint32_t i = 0; i < d->hash_cap; i++) {
            uint32_t e = d->hash[i];
            if (!e) continue;
            for (h = hash_id(d->objs[e - 1u].id, nc - 1u); nh[h]; h = (h + 1u) & (nc - 1u)) {}
            nh[h] = e;
        }
        free(d->hash);
        d->hash = nh;
        d->hash_cap = nc;
    }
    mask = d->hash_cap - 1u;
    for (h = hash_id(d->objs[index].id, mask); d->hash[h]; h = (h + 1u) & mask) {}
    d->hash[h] = index + 1u;
    return PC_OK;
}

/* New object with id and kind; value slots reserved when slots > 0. */
static pc_status new_obj(parser *P, int32_t id, uint8_t kind, uint32_t slots, uint32_t *out)
{
    nrbf_doc *d = P->d;
    pc_status st;
    nrbf_obj *o;
    if (id == 0 || find_index(d, id) != NRBF_NONE) return PC_ERR_FORMAT;
    st = grow((void **)&d->objs, &d->cap_objs, d->n_objs + 1u, sizeof *d->objs,
              P->o->lim.max_objects);
    if (st != PC_OK) return st;
    if (slots) {
        if (slots > P->o->lim.max_values - d->n_vals) return PC_ERR_LIMIT;
        st = grow((void **)&d->vals, &d->cap_vals, d->n_vals + slots, sizeof *d->vals,
                  P->o->lim.max_values);
        if (st != PC_OK) return st;
    }
    o = &d->objs[d->n_objs];
    memset(o, 0, sizeof *o);
    o->id = id;
    o->kind = kind;
    o->cls = NRBF_NONE;
    o->first = d->n_vals;
    o->n = slots;
    if (slots) memset(&d->vals[d->n_vals], 0, (size_t)slots * sizeof *d->vals);
    d->n_vals += slots;
    st = hash_insert(d, d->n_objs);
    if (st != PC_OK) return st;
    *out = d->n_objs++;
    return PC_OK;
}

static pc_status push_frame(parser *P, uint32_t obj, uint32_t cls)
{
    pc_status st;
    frame *f;
    const nrbf_obj *o = &P->d->objs[obj];
    if (o->n == 0u) return PC_OK;
    if (P->sp >= P->o->lim.max_depth) return PC_ERR_LIMIT;
    st = grow((void **)&P->stack, &P->stack_cap, P->sp + 1u, sizeof *P->stack, 1u << 16);
    if (st != PC_OK) return st;
    f = &P->stack[P->sp++];
    f->obj = obj;
    f->base = o->first;
    f->next = 0;
    f->count = o->n;
    f->cls = cls;
    return PC_OK;
}

static const nrbf_lib *find_lib(const nrbf_doc *d, int32_t id)
{
    for (uint32_t i = 0; i < d->n_libs; i++)
        if (d->libs[i].id == id) return &d->libs[i];
    return NULL;
}

static pc_status rd_library(parser *P)
{
    nrbf_doc *d = P->d;
    int32_t id = rd_i32(P);
    nrbf_str name;
    pc_status st = rd_lps(P, &name);
    if (st != PC_OK) return st;
    if (P->rd.err || id == 0 || find_lib(d, id)) return PC_ERR_FORMAT;
    if (!lib_allowed(P->o->libs, name)) return PC_ERR_UNSUPPORTED;
    st = grow((void **)&d->libs, &d->cap_libs, d->n_libs + 1u, sizeof *d->libs,
              P->o->lim.max_libs);
    if (st != PC_OK) return st;
    d->libs[d->n_libs].id = id;
    d->libs[d->n_libs].name = name;
    d->n_libs++;
    if (P->dump) {
        dindent(P);
        dputs(P, "Library ");
        did(P, id);
        dputs(P, " ");
        dname(P, name, true);
        dputs(P, "\n");
    }
    return PC_OK;
}

/* ClassWithMembersAndTypes and friends. Creates the class metadata and the
 * object, pushes its frame. */
static pc_status rd_class(parser *P, uint8_t rt, uint32_t *obj_out)
{
    nrbf_doc *d = P->d;
    bool typed = rt == NRBF_REC_CLASS_TYPES || rt == NRBF_REC_SYS_CLASS_TYPES;
    bool system = rt == NRBF_REC_SYS_CLASS_TYPES || rt == NRBF_REC_SYS_CLASS_MEMBERS;
    int32_t id = rd_i32(P), count;
    nrbf_str name;
    nrbf_class *c;
    uint32_t first, obj;
    pc_status st = rd_lps(P, &name);
    if (st != PC_OK) return st;
    count = rd_i32(P);
    if (P->rd.err || count < 0) return PC_ERR_FORMAT;
    if ((uint32_t)count > P->o->lim.max_members) return PC_ERR_LIMIT;
    if (!name_allowed(P->o->classes, name)) return PC_ERR_UNSUPPORTED;
    st = grow((void **)&d->members, &d->cap_members, d->n_members + (uint32_t)count,
              sizeof *d->members, P->o->lim.max_values);
    if (st != PC_OK) return st;
    st = grow((void **)&d->classes, &d->cap_classes, d->n_classes + 1u, sizeof *d->classes,
              P->o->lim.max_classes);
    if (st != PC_OK) return st;
    first = d->n_members;
    for (int32_t i = 0; i < count; i++) {
        nrbf_member *m = &d->members[first + (uint32_t)i];
        memset(m, 0, sizeof *m);
        m->btype = 0xFFu;
        st = rd_lps(P, &m->name);
        if (st != PC_OK) return st;
    }
    if (typed) {
        for (int32_t i = 0; i < count; i++) {
            uint32_t bt = pc_rd_u8(&P->rd);
            if (bt > NRBF_BT_PRIMITIVE_ARRAY) return PC_ERR_FORMAT;
            d->members[first + (uint32_t)i].btype = (uint8_t)bt;
        }
        for (int32_t i = 0; i < count; i++) {
            nrbf_member *m = &d->members[first + (uint32_t)i];
            switch (m->btype) {
            case NRBF_BT_PRIMITIVE: case NRBF_BT_PRIMITIVE_ARRAY:
                m->prim = pc_rd_u8(&P->rd);
                if (!prim_valid(m->prim)) return PC_ERR_FORMAT;
                break;
            case NRBF_BT_SYSTEM_CLASS:
                st = rd_lps(P, &m->type_name);
                if (st != PC_OK) return st;
                break;
            case NRBF_BT_CLASS:
                st = rd_lps(P, &m->type_name);
                if (st != PC_OK) return st;
                m->type_lib = rd_i32(P);
                if (P->rd.err || !find_lib(d, m->type_lib)) return PC_ERR_FORMAT;
                break;
            default:
                break;
            }
        }
    }
    {
        int32_t lib = 0;
        if (!system) {
            lib = rd_i32(P);
            if (P->rd.err || !find_lib(d, lib)) return PC_ERR_FORMAT;
        }
        st = rd_check(P);
        if (st != PC_OK) return st;
        st = new_obj(P, id, NRBF_O_CLASS, (uint32_t)count, &obj);
        if (st != PC_OK) return st;
        c = &d->classes[d->n_classes];
        c->name = name;
        c->lib = lib;
        c->meta_id = id;
        c->first_member = first;
        c->n_members = (uint32_t)count;
        c->has_types = typed;
        c->system = system;
        d->objs[obj].cls = d->n_classes;
        d->n_members += (uint32_t)count;
        d->n_classes++;
    }
    if (P->dump) {
        static const char *const rn[6] = { "", "", "SystemClassWithMembers", "ClassWithMembers",
                                           "SystemClassWithMembersAndTypes",
                                           "ClassWithMembersAndTypes" };
        dputs(P, rn[rt]);
        dputs(P, " ");
        did(P, id);
        dputs(P, " ");
        dname(P, name, false);
        if (!system) { dputs(P, " lib="); did(P, c->lib); }
        dclass_members(P, c);
        dputs(P, "\n");
    }
    *obj_out = obj;
    return push_frame(P, obj, d->objs[obj].cls);
}

static pc_status rd_class_with_id(parser *P, uint32_t *obj_out)
{
    nrbf_doc *d = P->d;
    int32_t id = rd_i32(P), meta = rd_i32(P);
    uint32_t mi, obj, cls;
    pc_status st;
    if (P->rd.err) return PC_ERR_FORMAT;
    mi = find_index(d, meta);
    if (mi == NRBF_NONE || d->objs[mi].kind != NRBF_O_CLASS) return PC_ERR_FORMAT;
    cls = d->objs[mi].cls;
    st = new_obj(P, id, NRBF_O_CLASS, d->classes[cls].n_members, &obj);
    if (st != PC_OK) return st;
    d->objs[obj].cls = cls;
    if (P->dump) {
        dputs(P, "ClassWithId ");
        did(P, id);
        dputs(P, " meta=");
        did(P, meta);
        dputs(P, " ");
        dname(P, d->classes[cls].name, false);
        dputs(P, "\n");
    }
    *obj_out = obj;
    return push_frame(P, obj, cls);
}

/* Primitive array data of count elements of type p: referenced in place. */
static pc_status rd_prim_data(parser *P, uint32_t obj, uint8_t p, uint32_t count)
{
    nrbf_obj *o = &P->d->objs[obj];
    size_t es = prim_size(p), bytes;
    o->prim = p;
    o->n = count;
    o->first = 0;
    o->data = P->rd.p + P->rd.pos;
    if (es) {
        if (!pc_mul_size(es, count, &bytes) || bytes > pc_rd_left(&P->rd)) return PC_ERR_FORMAT;
        (void)pc_rd_skip(&P->rd, bytes);
    } else {
        size_t start = P->rd.pos;
        for (uint32_t i = 0; i < count; i++) {
            nrbf_value v;
            pc_status st = rd_prim(P, p, &v);
            if (st != PC_OK) return st;
        }
        bytes = P->rd.pos - start;
    }
    o->data_len = bytes;
    return rd_check(P);
}

static pc_status rd_array_prim(parser *P, uint32_t *obj_out)
{
    int32_t id = rd_i32(P), len = rd_i32(P);
    uint32_t p = pc_rd_u8(&P->rd), obj;
    pc_status st;
    if (P->rd.err || len < 0 || !prim_valid((uint8_t)p)) return PC_ERR_FORMAT;
    if ((uint32_t)len > P->o->lim.max_array) return PC_ERR_LIMIT;
    st = new_obj(P, id, NRBF_O_PRIM_ARRAY, 0u, &obj);
    if (st != PC_OK) return st;
    st = rd_prim_data(P, obj, (uint8_t)p, (uint32_t)len);
    if (st != PC_OK) return st;
    if (P->dump) {
        dputs(P, "ArraySinglePrimitive ");
        did(P, id);
        dprintf_(P, " %s n=%d\n", prim_name((uint8_t)p), (int)len);
    }
    *obj_out = obj;
    return PC_OK;
}

static pc_status rd_array_single(parser *P, uint8_t rt, uint32_t *obj_out)
{
    int32_t id = rd_i32(P), len = rd_i32(P);
    uint32_t obj;
    pc_status st;
    if (P->rd.err || len < 0) return PC_ERR_FORMAT;
    if ((uint32_t)len > P->o->lim.max_array) return PC_ERR_LIMIT;
    st = new_obj(P, id, NRBF_O_ARRAY, (uint32_t)len, &obj);
    if (st != PC_OK) return st;
    P->d->objs[obj].elem_btype = rt == NRBF_REC_ARRAY_STRING ? NRBF_BT_STRING : NRBF_BT_OBJECT;
    if (P->dump) {
        dputs(P, rt == NRBF_REC_ARRAY_STRING ? "ArraySingleString " : "ArraySingleObject ");
        did(P, id);
        dprintf_(P, " n=%d\n", (int)len);
    }
    *obj_out = obj;
    return push_frame(P, obj, NRBF_NONE);
}

static pc_status rd_binary_array(parser *P, uint32_t *obj_out)
{
    int32_t id = rd_i32(P), rank, len;
    uint32_t at = pc_rd_u8(&P->rd), bt, obj;
    uint8_t prim = 0;
    nrbf_str tname = { NULL, 0u };
    pc_status st;
    rank = rd_i32(P);
    if (P->rd.err) return PC_ERR_FORMAT;
    if (at > NRBF_AT_RECT_OFFSET) return PC_ERR_FORMAT;
    if (at != NRBF_AT_SINGLE && at != NRBF_AT_JAGGED) return PC_ERR_UNSUPPORTED;
    if (rank != 1) return rank < 1 ? PC_ERR_FORMAT : PC_ERR_UNSUPPORTED;
    len = rd_i32(P);
    bt = pc_rd_u8(&P->rd);
    if (P->rd.err || len < 0 || bt > NRBF_BT_PRIMITIVE_ARRAY) return PC_ERR_FORMAT;
    if ((uint32_t)len > P->o->lim.max_array) return PC_ERR_LIMIT;
    switch (bt) {
    case NRBF_BT_PRIMITIVE: case NRBF_BT_PRIMITIVE_ARRAY:
        prim = pc_rd_u8(&P->rd);
        if (!prim_valid(prim)) return PC_ERR_FORMAT;
        break;
    case NRBF_BT_SYSTEM_CLASS:
        st = rd_lps(P, &tname);
        if (st != PC_OK) return st;
        break;
    case NRBF_BT_CLASS:
        st = rd_lps(P, &tname);
        if (st != PC_OK) return st;
        if (!find_lib(P->d, rd_i32(P))) return PC_ERR_FORMAT;
        break;
    default:
        break;
    }
    st = rd_check(P);
    if (st != PC_OK) return st;
    if (bt == NRBF_BT_PRIMITIVE) {
        st = new_obj(P, id, NRBF_O_PRIM_ARRAY, 0u, &obj);
        if (st != PC_OK) return st;
        st = rd_prim_data(P, obj, prim, (uint32_t)len);
        if (st != PC_OK) return st;
    } else {
        st = new_obj(P, id, NRBF_O_ARRAY, (uint32_t)len, &obj);
        if (st != PC_OK) return st;
        P->d->objs[obj].elem_btype = (uint8_t)bt;
        P->d->objs[obj].prim = prim;
        P->d->objs[obj].str = tname;
    }
    if (P->dump) {
        dputs(P, "BinaryArray ");
        did(P, id);
        dprintf_(P, " %s n=%d of ", at == NRBF_AT_SINGLE ? "single" : "jagged", (int)len);
        if (bt == NRBF_BT_PRIMITIVE || bt == NRBF_BT_PRIMITIVE_ARRAY)
            dprintf_(P, "%s%s", prim_name(prim), bt == NRBF_BT_PRIMITIVE ? "" : "[]");
        else if (tname.p) {
            dputs(P, bt == NRBF_BT_SYSTEM_CLASS ? "SystemClass(" : "Class(");
            dname(P, tname, false);
            dputs(P, ")");
        }
        else dprintf_(P, "bt%u", bt);
        dputs(P, "\n");
    }
    *obj_out = obj;
    return bt == NRBF_BT_PRIMITIVE ? PC_OK : push_frame(P, obj, NRBF_NONE);
}

/* Read one record in context ctx. For value contexts the slot is the next
 * slot of the top frame. *end is set by MessageEnd at top level. */
static pc_status rd_record(parser *P, uint32_t ctx, bool *end)
{
    nrbf_doc *d = P->d;
    uint32_t obj = NRBF_NONE, fi = P->sp ? P->sp - 1u : 0u;
    nrbf_value v;
    pc_status st;
    uint8_t rt;
    for (;;) {
        rt = pc_rd_u8(&P->rd);
        if (P->rd.err) return PC_ERR_FORMAT;
        if (rt != NRBF_REC_LIBRARY) break;
        st = rd_library(P);
        if (st != PC_OK) return st;
    }
    memset(&v, 0, sizeof v);
    if (ctx != CTX_TOP) dslot_prefix(P, ctx);
    else if (P->dump && rt != NRBF_REC_END) dindent(P);
    switch (rt) {
    case NRBF_REC_CLASS_TYPES: case NRBF_REC_SYS_CLASS_TYPES:
    case NRBF_REC_CLASS_MEMBERS: case NRBF_REC_SYS_CLASS_MEMBERS:
        st = rd_class(P, rt, &obj);
        break;
    case NRBF_REC_CLASS_WITH_ID:
        st = rd_class_with_id(P, &obj);
        break;
    case NRBF_REC_STRING: {
        int32_t id = rd_i32(P);
        nrbf_str s;
        st = rd_lps(P, &s);
        if (st != PC_OK) return st;
        st = new_obj(P, id, NRBF_O_STRING, 0u, &obj);
        if (st != PC_OK) return st;
        d->objs[obj].str = s;
        if (P->dump) {
            dputs(P, "String ");
            did(P, id);
            if (!(P->dflags & NRBF_DUMP_STRUCT)) { dputs(P, " "); dstr(P, s, 160u); }
            dputs(P, "\n");
        }
        break;
    }
    case NRBF_REC_ARRAY_PRIM:
        st = rd_array_prim(P, &obj);
        break;
    case NRBF_REC_ARRAY_OBJECT: case NRBF_REC_ARRAY_STRING:
        st = rd_array_single(P, rt, &obj);
        break;
    case NRBF_REC_BINARY_ARRAY:
        st = rd_binary_array(P, &obj);
        break;
    case NRBF_REC_REFERENCE:
        if (ctx == CTX_TOP) return PC_ERR_FORMAT;
        v.kind = NRBF_V_OBJ;
        v.id = rd_i32(P);
        if (P->rd.err) return PC_ERR_FORMAT;
        if (P->dump) { dputs(P, "-> "); did(P, v.id); dputs(P, "\n"); }
        P->d->vals[P->stack[fi].base + P->stack[fi].next++] = v;
        return PC_OK;
    case NRBF_REC_NULL:
        if (ctx == CTX_TOP) return PC_ERR_FORMAT;
        dputs(P, "null\n");
        P->d->vals[P->stack[fi].base + P->stack[fi].next++] = v;
        return PC_OK;
    case NRBF_REC_NULL_MULTI_256: case NRBF_REC_NULL_MULTI: {
        frame *f;
        uint32_t cnt;
        if (ctx != CTX_ELEM) return PC_ERR_FORMAT;
        if (rt == NRBF_REC_NULL_MULTI_256) {
            cnt = pc_rd_u8(&P->rd);
        } else {
            int32_t c = rd_i32(P);
            if (c < 0) return PC_ERR_FORMAT;
            cnt = (uint32_t)c;
        }
        if (P->rd.err) return PC_ERR_FORMAT;
        f = &P->stack[fi];
        if (cnt == 0u || cnt > f->count - f->next) return PC_ERR_FORMAT;
        if (P->dump) dprintf_(P, "null x%u%s\n", cnt, rt == NRBF_REC_NULL_MULTI ? " (multi)" : "");
        f->next += cnt;     /* slots are zero-initialized = null */
        return PC_OK;
    }
    case NRBF_REC_PRIM_TYPED: {
        uint8_t p = pc_rd_u8(&P->rd);
        if (ctx == CTX_TOP) return PC_ERR_FORMAT;
        if (!prim_valid(p)) return PC_ERR_FORMAT;
        st = rd_prim(P, p, &v);
        if (st != PC_OK) return st;
        dvalue_prim(P, &v);
        dputs(P, " (typed)\n");
        P->d->vals[P->stack[fi].base + P->stack[fi].next++] = v;
        return PC_OK;
    }
    case NRBF_REC_END:
        if (ctx != CTX_TOP) return PC_ERR_FORMAT;
        dputs(P, "MessageEnd\n");
        *end = true;
        return PC_OK;
    case NRBF_REC_HEADER:
        return PC_ERR_FORMAT;   /* only valid as the very first record */
    case 21: case 22:           /* MethodCall, MethodReturn: valid NRBF, not a document */
        return PC_ERR_UNSUPPORTED;
    default:
        return PC_ERR_FORMAT;
    }
    if (st != PC_OK) return st;
    if (ctx != CTX_TOP) {       /* the new object fills the slot of the parent frame */
        v.kind = NRBF_V_OBJ;
        v.id = d->objs[obj].id;
        P->d->vals[P->stack[fi].base + P->stack[fi].next++] = v;
    }
    return PC_OK;
}

static pc_status parse_body(parser *P)
{
    nrbf_doc *d = P->d;
    bool end = false;
    pc_status st;
    uint8_t rt = pc_rd_u8(&P->rd);
    if (P->rd.err || rt != NRBF_REC_HEADER) return PC_ERR_FORMAT;
    d->root_id = rd_i32(P);
    d->header_id = rd_i32(P);
    {
        int32_t major = rd_i32(P), minor = rd_i32(P);
        if (P->rd.err) return PC_ERR_FORMAT;
        if (major != 1 || minor != 0) return PC_ERR_UNSUPPORTED;
        if (P->dump) {
            dputs(P, "Header root=");
            did(P, d->root_id);
            dputs(P, " header=");
            dprintf_(P, "%d 1.0\n", (int)d->header_id);
        }
    }
    while (!end) {
        if (P->sp == 0u) {
            st = rd_record(P, CTX_TOP, &end);
        } else {
            frame *f = &P->stack[P->sp - 1u];
            if (f->next >= f->count) { P->sp--; continue; }
            if (f->cls != NRBF_NONE) {
                const nrbf_class *c = &d->classes[f->cls];
                const nrbf_member *m = &d->members[c->first_member + f->next];
                if (c->has_types && m->btype == NRBF_BT_PRIMITIVE) {
                    nrbf_value v;
                    st = rd_prim(P, m->prim, &v);
                    if (st != PC_OK) return st;
                    if (P->dump) {
                        dslot_prefix(P, CTX_MEMBER);
                        dvalue_prim(P, &v);
                        dputs(P, "\n");
                    }
                    d->vals[f->base + f->next++] = v;
                    continue;
                }
                st = rd_record(P, CTX_MEMBER, &end);
            } else {
                st = rd_record(P, CTX_ELEM, &end);
            }
        }
        if (st != PC_OK) return st;
    }
    d->end = P->rd.pos;
    /* every reference must resolve, and the root must exist */
    for (uint32_t i = 0; i < d->n_vals; i++)
        if (d->vals[i].kind == NRBF_V_OBJ && find_index(d, d->vals[i].id) == NRBF_NONE)
            return PC_ERR_FORMAT;
    if (find_index(d, d->root_id) == NRBF_NONE) return PC_ERR_FORMAT;
    return PC_OK;
}

pc_status nrbf_parse(const uint8_t *p, size_t n, const nrbf_opts *o, nrbf_doc *doc)
{
    parser P;
    nrbf_opts def;
    pc_status st;
    if (!doc) return PC_ERR_ARG;
    memset(doc, 0, sizeof *doc);
    if (!p) return PC_ERR_ARG;
    if (!o) { nrbf_opts_default(&def); o = &def; }
    memset(&P, 0, sizeof P);
    P.rd = pc_rd_make(p, n < o->lim.max_bytes ? n : o->lim.max_bytes);
    P.o = o;
    P.d = doc;
    P.dump = o->dump;
    P.dflags = o->dump_flags;
    st = parse_body(&P);
    if (st == PC_OK && P.dump_st != PC_OK) st = P.dump_st;
    if (st != PC_OK && P.dump && P.dump_st == PC_OK)
        dprintf_(&P, "!! %s at offset %zu\n", pc_status_str(st), P.rd.pos);
    free(P.stack);
    free(P.nid_keys);
    free(P.nid_vals);
    if (st != PC_OK) nrbf_free(doc);
    return st;
}

/* ---- writer -------------------------------------------------------------------- */
pc_status nrbf_put_lps(pc_buf *b, const char *s, size_t n)
{
    uint8_t t[5];
    size_t k = 0, v = n;
    pc_status st;
    if (n > (size_t)INT32_MAX) return PC_ERR_LIMIT;
    do {
        uint8_t byte = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v) byte |= 0x80u;
        t[k++] = byte;
    } while (v);
    st = pc_buf_append(b, t, k);
    return st == PC_OK ? pc_buf_append(b, s, n) : st;
}

pc_status nrbf_put_i32(pc_buf *b, int32_t v) { return pc_buf_put_le32(b, (uint32_t)v); }

pc_status nrbf_put_i64(pc_buf *b, int64_t v)
{
    pc_status st = pc_buf_put_le32(b, (uint32_t)((uint64_t)v & 0xFFFFFFFFu));
    return st == PC_OK ? pc_buf_put_le32(b, (uint32_t)((uint64_t)v >> 32)) : st;
}

pc_status nrbf_put_header(pc_buf *b, int32_t root_id, int32_t header_id)
{
    pc_status st = pc_buf_put_u8(b, NRBF_REC_HEADER);
    if (st == PC_OK) st = nrbf_put_i32(b, root_id);
    if (st == PC_OK) st = nrbf_put_i32(b, header_id);
    if (st == PC_OK) st = nrbf_put_i32(b, 1);
    if (st == PC_OK) st = nrbf_put_i32(b, 0);
    return st;
}

pc_status nrbf_put_library(pc_buf *b, int32_t id, const char *name)
{
    pc_status st = pc_buf_put_u8(b, NRBF_REC_LIBRARY);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = nrbf_put_lps(b, name, strlen(name));
    return st;
}

pc_status nrbf_put_string(pc_buf *b, int32_t id, const char *s, size_t n)
{
    pc_status st = pc_buf_put_u8(b, NRBF_REC_STRING);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = nrbf_put_lps(b, s, n);
    return st;
}

pc_status nrbf_put_ref(pc_buf *b, int32_t id)
{
    pc_status st = pc_buf_put_u8(b, NRBF_REC_REFERENCE);
    return st == PC_OK ? nrbf_put_i32(b, id) : st;
}

pc_status nrbf_put_null(pc_buf *b) { return pc_buf_put_u8(b, NRBF_REC_NULL); }

pc_status nrbf_put_nulls(pc_buf *b, uint32_t count)
{
    pc_status st;
    if (count == 0u) return PC_OK;
    if (count == 1u) return nrbf_put_null(b);
    if (count < 256u) {
        st = pc_buf_put_u8(b, NRBF_REC_NULL_MULTI_256);
        return st == PC_OK ? pc_buf_put_u8(b, (uint8_t)count) : st;
    }
    if (count > (uint32_t)INT32_MAX) return PC_ERR_LIMIT;
    st = pc_buf_put_u8(b, NRBF_REC_NULL_MULTI);
    return st == PC_OK ? nrbf_put_i32(b, (int32_t)count) : st;
}

pc_status nrbf_put_end(pc_buf *b) { return pc_buf_put_u8(b, NRBF_REC_END); }

pc_status nrbf_put_class_with_id(pc_buf *b, int32_t id, int32_t meta_id)
{
    pc_status st = pc_buf_put_u8(b, NRBF_REC_CLASS_WITH_ID);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = nrbf_put_i32(b, meta_id);
    return st;
}

pc_status nrbf_put_class(pc_buf *b, int32_t id, const char *name,
                         const nrbf_wmember *m, uint32_t n_members, int32_t lib)
{
    pc_status st = pc_buf_put_u8(b, lib ? NRBF_REC_CLASS_TYPES : NRBF_REC_SYS_CLASS_TYPES);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = nrbf_put_lps(b, name, strlen(name));
    if (st == PC_OK) st = nrbf_put_i32(b, (int32_t)n_members);
    for (uint32_t i = 0; i < n_members && st == PC_OK; i++)
        st = nrbf_put_lps(b, m[i].name, strlen(m[i].name));
    for (uint32_t i = 0; i < n_members && st == PC_OK; i++)
        st = pc_buf_put_u8(b, m[i].btype);
    for (uint32_t i = 0; i < n_members && st == PC_OK; i++) {
        switch (m[i].btype) {
        case NRBF_BT_PRIMITIVE: case NRBF_BT_PRIMITIVE_ARRAY:
            st = pc_buf_put_u8(b, m[i].prim);
            break;
        case NRBF_BT_SYSTEM_CLASS:
            st = nrbf_put_lps(b, m[i].type_name, strlen(m[i].type_name));
            break;
        case NRBF_BT_CLASS:
            st = nrbf_put_lps(b, m[i].type_name, strlen(m[i].type_name));
            if (st == PC_OK) st = nrbf_put_i32(b, m[i].type_lib);
            break;
        default:
            break;
        }
    }
    if (st == PC_OK && lib) st = nrbf_put_i32(b, lib);
    return st;
}

pc_status nrbf_put_sysclass_array(pc_buf *b, int32_t id, uint32_t length,
                                  const char *type_name)
{
    pc_status st;
    if (length > (uint32_t)INT32_MAX) return PC_ERR_LIMIT;
    st = pc_buf_put_u8(b, NRBF_REC_BINARY_ARRAY);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = pc_buf_put_u8(b, NRBF_AT_SINGLE);
    if (st == PC_OK) st = nrbf_put_i32(b, 1);
    if (st == PC_OK) st = nrbf_put_i32(b, (int32_t)length);
    if (st == PC_OK) st = pc_buf_put_u8(b, NRBF_BT_SYSTEM_CLASS);
    if (st == PC_OK) st = nrbf_put_lps(b, type_name, strlen(type_name));
    return st;
}

pc_status nrbf_put_object_array(pc_buf *b, int32_t id, uint32_t length)
{
    pc_status st;
    if (length > (uint32_t)INT32_MAX) return PC_ERR_LIMIT;
    st = pc_buf_put_u8(b, NRBF_REC_ARRAY_OBJECT);
    if (st == PC_OK) st = nrbf_put_i32(b, id);
    if (st == PC_OK) st = nrbf_put_i32(b, (int32_t)length);
    return st;
}
