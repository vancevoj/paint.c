/* nrbf.h - bounded, whitelisted MS-NRBF (.NET BinaryFormatter) reader and
 * record writer used by the .pdn codec (lane L6C). Internal to src/codec.
 *
 * Reader (X-19): only the record types a Paint.NET document needs are
 * accepted, every class name must be on a caller supplied whitelist, every
 * library name must start with an allowed prefix, and object count, value
 * slots, nesting depth, member count, string length, array length and the
 * consumed byte count are capped before anything is allocated (P-08). The
 * parser is iterative (explicit frame stack, P-07) and never resolves the
 * object graph by itself: it produces a flat table of objects keyed by id,
 * and the caller walks only the members it expects. References are plain
 * ids, so a cyclic graph cannot make the parser loop.
 *
 * Strings and primitive arrays point into the input buffer (borrowed); the
 * input must outlive the nrbf_doc.
 *
 * Writer: helpers that append individual records to a pc_buf. Object ids,
 * record order and metadata reuse are the caller's business (fmt_pdn.c
 * mirrors the .NET ObjectWriter behavior).
 *
 * Thread rules: all functions are reentrant; an nrbf_doc is used by one
 * thread at a time.
 */
#ifndef PC_NRBF_H
#define PC_NRBF_H

#include "pc/pc_codec.h"

/* ---- enumerations from [MS-NRBF] 2.1.2 ------------------------------------ */
enum {
    NRBF_REC_HEADER = 0, NRBF_REC_CLASS_WITH_ID = 1, NRBF_REC_SYS_CLASS_MEMBERS = 2,
    NRBF_REC_CLASS_MEMBERS = 3, NRBF_REC_SYS_CLASS_TYPES = 4, NRBF_REC_CLASS_TYPES = 5,
    NRBF_REC_STRING = 6, NRBF_REC_BINARY_ARRAY = 7, NRBF_REC_PRIM_TYPED = 8,
    NRBF_REC_REFERENCE = 9, NRBF_REC_NULL = 10, NRBF_REC_END = 11, NRBF_REC_LIBRARY = 12,
    NRBF_REC_NULL_MULTI_256 = 13, NRBF_REC_NULL_MULTI = 14, NRBF_REC_ARRAY_PRIM = 15,
    NRBF_REC_ARRAY_OBJECT = 16, NRBF_REC_ARRAY_STRING = 17
};

enum {  /* BinaryTypeEnumeration */
    NRBF_BT_PRIMITIVE = 0, NRBF_BT_STRING = 1, NRBF_BT_OBJECT = 2, NRBF_BT_SYSTEM_CLASS = 3,
    NRBF_BT_CLASS = 4, NRBF_BT_OBJECT_ARRAY = 5, NRBF_BT_STRING_ARRAY = 6,
    NRBF_BT_PRIMITIVE_ARRAY = 7
};

enum {  /* PrimitiveTypeEnumeration */
    NRBF_P_BOOLEAN = 1, NRBF_P_BYTE = 2, NRBF_P_CHAR = 3, NRBF_P_DECIMAL = 5,
    NRBF_P_DOUBLE = 6, NRBF_P_INT16 = 7, NRBF_P_INT32 = 8, NRBF_P_INT64 = 9,
    NRBF_P_SBYTE = 10, NRBF_P_SINGLE = 11, NRBF_P_TIMESPAN = 12, NRBF_P_DATETIME = 13,
    NRBF_P_UINT16 = 14, NRBF_P_UINT32 = 15, NRBF_P_UINT64 = 16, NRBF_P_NULL = 17,
    NRBF_P_STRING = 18
};

enum {  /* BinaryArrayTypeEnumeration */
    NRBF_AT_SINGLE = 0, NRBF_AT_JAGGED = 1, NRBF_AT_RECTANGULAR = 2,
    NRBF_AT_SINGLE_OFFSET = 3, NRBF_AT_JAGGED_OFFSET = 4, NRBF_AT_RECT_OFFSET = 5
};

/* ---- parsed model ---------------------------------------------------------- */
typedef struct nrbf_str {
    const char *p;          /* borrowed from the input, NOT NUL-terminated */
    uint32_t    n;
} nrbf_str;

typedef struct nrbf_member {
    nrbf_str name;
    uint8_t  btype;         /* NRBF_BT_*, 0xFF when the record carried no types */
    uint8_t  prim;          /* NRBF_P_* for PRIMITIVE and PRIMITIVE_ARRAY */
    nrbf_str type_name;     /* SYSTEM_CLASS / CLASS type name, else empty */
    int32_t  type_lib;      /* CLASS library id, else 0 */
} nrbf_member;

typedef struct nrbf_class {
    nrbf_str name;
    int32_t  lib;           /* library id, 0 for system classes */
    int32_t  meta_id;       /* id of the object whose record defined it */
    uint32_t first_member;  /* index into nrbf_doc.members */
    uint32_t n_members;
    bool     has_types;     /* *WithMembersAndTypes */
    bool     system;        /* System*Class record */
} nrbf_class;

enum { NRBF_V_NULL = 0, NRBF_V_PRIM = 1, NRBF_V_OBJ = 2 };

typedef struct nrbf_value {
    uint8_t kind;           /* NRBF_V_* */
    uint8_t prim;           /* NRBF_P_* when kind == NRBF_V_PRIM */
    int32_t id;             /* object id when kind == NRBF_V_OBJ */
    union { int64_t i; uint64_t u; double f; } v;   /* integers sign/zero-extended */
} nrbf_value;

enum { NRBF_O_CLASS = 0, NRBF_O_STRING = 1, NRBF_O_ARRAY = 2, NRBF_O_PRIM_ARRAY = 3 };

typedef struct nrbf_obj {
    int32_t  id;
    uint8_t  kind;          /* NRBF_O_* */
    uint8_t  prim;          /* NRBF_O_PRIM_ARRAY element type */
    uint8_t  elem_btype;    /* NRBF_O_ARRAY: BinaryType of the elements */
    uint32_t cls;           /* NRBF_O_CLASS: index into nrbf_doc.classes */
    uint32_t first, n;      /* CLASS / ARRAY: value slots; PRIM_ARRAY: n = count */
    nrbf_str str;           /* STRING contents; ARRAY element type name */
    const uint8_t *data;    /* PRIM_ARRAY raw little-endian data (borrowed) */
    size_t   data_len;
} nrbf_obj;

typedef struct nrbf_lib {
    int32_t  id;
    nrbf_str name;
} nrbf_lib;

typedef struct nrbf_doc {
    nrbf_obj    *objs;      /* in stream order (record appearance) */
    uint32_t     n_objs, cap_objs;
    nrbf_value  *vals;
    uint32_t     n_vals, cap_vals;
    nrbf_class  *classes;
    uint32_t     n_classes, cap_classes;
    nrbf_member *members;
    uint32_t     n_members, cap_members;
    nrbf_lib    *libs;
    uint32_t     n_libs, cap_libs;
    uint32_t    *hash;      /* open addressing id -> objs index + 1, 0 = empty */
    uint32_t     hash_cap;  /* power of two */
    int32_t      root_id, header_id;
    size_t       end;       /* offset just past MessageEnd */
} nrbf_doc;

/* ---- options ---------------------------------------------------------------- */
typedef struct nrbf_limits {
    uint32_t max_objects;   /* default 1 << 18 */
    uint32_t max_values;    /* member and element slots, default 1 << 21 */
    uint32_t max_depth;     /* nested inline records, default 32 */
    uint32_t max_members;   /* per class, default 256 */
    uint32_t max_classes;   /* metadata records, default 1 << 14 */
    uint32_t max_libs;      /* default 64 */
    uint32_t max_string;    /* bytes per string, default 16 MiB */
    uint32_t max_array;     /* elements per array, default 1 << 20 */
    size_t   max_bytes;     /* NRBF bytes consumed, default SIZE_MAX */
} nrbf_limits;

void nrbf_limits_default(nrbf_limits *l);

/* Dump flags. NRBF_DUMP_STRUCT omits primitive values and string contents,
 * shortens library names to the assembly name and renumbers object ids by
 * first mention, so dumps of files with equal record structure compare equal
 * byte for byte. */
#define NRBF_DUMP_STRUCT   1u
#define NRBF_DUMP_KEEP_IDS 2u   /* with STRUCT: keep raw ids (checks id mirroring) */

typedef struct nrbf_opts {
    nrbf_limits        lim;
    /* NULL-terminated class name whitelist. An entry ending in '*' matches
     * any name with that prefix. NULL accepts every class (dump tool only). */
    const char *const *classes;
    /* NULL-terminated library name prefixes ("PaintDotNet.", ...). NULL
     * accepts every library. */
    const char *const *libs;
    pc_buf            *dump;        /* optional text dump (UTF-8), appended */
    uint32_t           dump_flags;  /* NRBF_DUMP_* */
} nrbf_opts;

/* Default limits, no whitelist (accept all), no dump. */
void nrbf_opts_default(nrbf_opts *o);

/* Parse the NRBF stream at p[0..n). On PC_OK *doc holds the model (caller
 * frees with nrbf_free) and doc->end is the offset after MessageEnd. Errors:
 * PC_ERR_FORMAT malformed or truncated stream, PC_ERR_UNSUPPORTED a valid
 * record or class that is not whitelisted, PC_ERR_LIMIT a cap was hit,
 * PC_ERR_NOMEM. On failure *doc is left empty (nothing to free).
 * p is borrowed and must outlive *doc. Any thread. */
pc_status nrbf_parse(const uint8_t *p, size_t n, const nrbf_opts *o, nrbf_doc *doc);
void      nrbf_free(nrbf_doc *doc);     /* NULL-safe, zeroes *doc */

/* Lookup helpers (borrowed pointers into doc, valid until nrbf_free). */
const nrbf_obj   *nrbf_get(const nrbf_doc *doc, int32_t id);
const nrbf_class *nrbf_class_of(const nrbf_doc *doc, const nrbf_obj *o); /* NULL if not CLASS */
bool              nrbf_str_eq(nrbf_str s, const char *z);
/* Value of member `name` of class object o, or NULL when o is not a class
 * object or has no such member. */
const nrbf_value *nrbf_member_value(const nrbf_doc *doc, const nrbf_obj *o, const char *name);
/* Element i of an object array, or NULL. */
const nrbf_value *nrbf_elem(const nrbf_doc *doc, const nrbf_obj *o, uint32_t i);
/* Object a value refers to (NULL for null or primitive values). */
const nrbf_obj   *nrbf_deref(const nrbf_doc *doc, const nrbf_value *v);
/* Integer and boolean views: false when v is missing or not an integer. */
bool nrbf_as_i64(const nrbf_value *v, int64_t *out);
bool nrbf_as_bool(const nrbf_value *v, bool *out);

/* ---- writer helpers (append to b; return PC_OK or PC_ERR_NOMEM/LIMIT) ------ */
pc_status nrbf_put_lps(pc_buf *b, const char *s, size_t n);    /* LengthPrefixedString */
pc_status nrbf_put_i32(pc_buf *b, int32_t v);
pc_status nrbf_put_i64(pc_buf *b, int64_t v);
pc_status nrbf_put_header(pc_buf *b, int32_t root_id, int32_t header_id);
pc_status nrbf_put_library(pc_buf *b, int32_t id, const char *name);
pc_status nrbf_put_string(pc_buf *b, int32_t id, const char *s, size_t n);
pc_status nrbf_put_ref(pc_buf *b, int32_t id);
pc_status nrbf_put_null(pc_buf *b);
/* ObjectNull / ObjectNullMultiple256 / ObjectNullMultiple as .NET picks them. */
pc_status nrbf_put_nulls(pc_buf *b, uint32_t count);
pc_status nrbf_put_end(pc_buf *b);
pc_status nrbf_put_class_with_id(pc_buf *b, int32_t id, int32_t meta_id);

/* One member of a ClassWithMembersAndTypes record being written. */
typedef struct nrbf_wmember {
    const char *name;
    uint8_t     btype;      /* NRBF_BT_* */
    uint8_t     prim;       /* PRIMITIVE / PRIMITIVE_ARRAY */
    const char *type_name;  /* SYSTEM_CLASS / CLASS */
    int32_t     type_lib;   /* CLASS */
} nrbf_wmember;

/* ClassWithMembersAndTypes (lib > 0) or SystemClassWithMembersAndTypes
 * (lib == 0). Member values follow and are appended by the caller. */
pc_status nrbf_put_class(pc_buf *b, int32_t id, const char *name,
                         const nrbf_wmember *m, uint32_t n_members, int32_t lib);
/* BinaryArray, single, rank 1, elements of SystemClass type_name. */
pc_status nrbf_put_sysclass_array(pc_buf *b, int32_t id, uint32_t length,
                                  const char *type_name);
pc_status nrbf_put_object_array(pc_buf *b, int32_t id, uint32_t length);

#endif /* PC_NRBF_H */
