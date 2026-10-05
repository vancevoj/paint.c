/* test_pdn_nrbf.c - the bounded MS-NRBF reader and record writer used by
 * the .pdn codec (lane L6C): record coverage, caps, whitelists, malformed
 * streams, reference loops, truncation at every byte and the dump text. */
#include "pdn_util.h"

static const char *const k_wl[] = { "Test.Node", "Test.Pair", "Prefix.*", NULL };
static const char *const k_libs[] = { "TestLib,", NULL };

static void opts(nrbf_opts *o)
{
    nrbf_opts_default(o);
    o->classes = k_wl;
    o->libs = k_libs;
}

/* Node { int32 v; Node next; string s } chained n times through references,
 * the last next pointing back to the first (a cycle). */
static void build_chain(pc_buf *b, uint32_t n)
{
    const nrbf_wmember m[3] = {
        { "v", NRBF_BT_PRIMITIVE, NRBF_P_INT32, NULL, 0 },
        { "next", NRBF_BT_CLASS, 0, "Test.Node", 2 },
        { "s", NRBF_BT_STRING, 0, NULL, 0 },
    };
    memset(b, 0, sizeof *b);
    (void)nrbf_put_header(b, 1, -1);
    (void)nrbf_put_library(b, 2, "TestLib, Version=1.0.0.0");
    for (uint32_t i = 0; i < n; i++) {
        int32_t id = i == 0 ? 1 : (int32_t)(100u + i);
        int32_t next = i + 1u == n ? 1 : (int32_t)(101u + i);
        char s[16];
        if (i == 0) (void)nrbf_put_class(b, id, "Test.Node", m, 3u, 2);
        else (void)nrbf_put_class_with_id(b, id, 1);
        (void)nrbf_put_i32(b, (int32_t)i * 7);
        (void)nrbf_put_ref(b, next);
        snprintf(s, sizeof s, "n%u", i);
        (void)nrbf_put_string(b, (int32_t)(10000u + i), s, strlen(s));
    }
    (void)nrbf_put_end(b);
}

static void t_writer_bytes(void)
{
    pc_buf b;
    char big[300];
    memset(&b, 0, sizeof b);
    CHECK(nrbf_put_nulls(&b, 0u) == PC_OK && b.n == 0u);
    CHECK(nrbf_put_nulls(&b, 1u) == PC_OK && b.n == 1u && b.p[0] == 0x0Au);
    CHECK(nrbf_put_nulls(&b, 2u) == PC_OK && b.n == 3u && b.p[1] == 0x0Du && b.p[2] == 2u);
    CHECK(nrbf_put_nulls(&b, 256u) == PC_OK && b.n == 8u && b.p[3] == 0x0Eu &&
          b.p[4] == 0u && b.p[5] == 1u && b.p[6] == 0u && b.p[7] == 0u);
    pc_buf_free(&b);
    memset(big, 'x', sizeof big);
    CHECK(nrbf_put_lps(&b, big, 127u) == PC_OK && b.n == 128u && b.p[0] == 127u);
    pc_buf_free(&b);
    CHECK(nrbf_put_lps(&b, big, 300u) == PC_OK && b.n == 302u && b.p[0] == 0xACu &&
          b.p[1] == 0x02u);
    pc_buf_free(&b);
    CHECK(nrbf_put_header(&b, 1, -1) == PC_OK && b.n == 17u && b.p[0] == 0u && b.p[1] == 1u &&
          b.p[5] == 0xFFu && b.p[9] == 1u && b.p[13] == 0u);
    pc_buf_free(&b);
}

static void t_parse_chain_and_cycle(void)
{
    pc_buf b;
    nrbf_opts o;
    nrbf_doc d;
    const nrbf_obj *root, *cur;
    uint32_t steps = 0;
    build_chain(&b, 50u);
    opts(&o);
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
    CHECK(d.end == b.n && d.root_id == 1 && d.n_libs == 1u);
    root = nrbf_get(&d, 1);
    CHECK(root != NULL && nrbf_class_of(&d, root) != NULL);
    /* walking the cycle is bounded by the caller, the parser never loops */
    for (cur = root; cur && steps < 120u; steps++) {
        int64_t v;
        const nrbf_obj *s = nrbf_deref(&d, nrbf_member_value(&d, cur, "s"));
        CHECK(nrbf_as_i64(nrbf_member_value(&d, cur, "v"), &v));
        CHECK(s && s->kind == NRBF_O_STRING && s->str.n >= 2u && s->str.p[0] == 'n');
        cur = nrbf_deref(&d, nrbf_member_value(&d, cur, "next"));
    }
    CHECK(steps == 120u && cur != NULL);
    CHECK(nrbf_member_value(&d, root, "missing") == NULL);
    CHECK(nrbf_get(&d, 424242) == NULL);
    nrbf_free(&d);
    CHECK(d.objs == NULL);
    /* truncation at every byte: always an error, never a crash */
    for (size_t k = 0; k < b.n; k++) {
        pc_status st = nrbf_parse(b.p, k, &o, &d);
        CHECK(st == PC_ERR_FORMAT);
        CHECK(d.objs == NULL && d.n_objs == 0u);
    }
    pc_buf_free(&b);
}

static void t_whitelists(void)
{
    pc_buf b;
    nrbf_opts o;
    nrbf_doc d;
    build_chain(&b, 3u);
    nrbf_opts_default(&o);
    o.classes = k_wl + 1;               /* Test.Node not allowed */
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_UNSUPPORTED);
    opts(&o);
    o.libs = k_wl;                      /* TestLib not allowed */
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_UNSUPPORTED);
    pc_buf_free(&b);
    {   /* prefix entries */
        const nrbf_wmember m[1] = { { "x", NRBF_BT_PRIMITIVE, NRBF_P_BYTE, NULL, 0 } };
        memset(&b, 0, sizeof b);
        (void)nrbf_put_header(&b, 1, -1);
        (void)nrbf_put_class(&b, 1, "Prefix.Anything`2[[x]]", m, 1u, 0);
        (void)pc_buf_put_u8(&b, 9u);
        (void)nrbf_put_end(&b);
        opts(&o);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
        nrbf_free(&d);
        b.p[27] = 'Q';                  /* "Prefix" -> "PrefQx" */
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_UNSUPPORTED);
        pc_buf_free(&b);
    }
}

static void t_caps(void)
{
    pc_buf b;
    nrbf_opts o;
    nrbf_doc d;
    /* object count */
    build_chain(&b, 40u);
    opts(&o);
    o.lim.max_objects = 30u;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
    opts(&o);
    o.lim.max_values = 50u;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
    opts(&o);
    o.lim.max_string = 2u;              /* "n10" is 3 bytes */
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
    opts(&o);
    o.lim.max_bytes = b.n - 1u;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_FORMAT);
    opts(&o);
    o.lim.max_members = 2u;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
    opts(&o);
    o.lim.max_libs = 0u;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
    pc_buf_free(&b);

    /* nesting: value-type classes inline inside each other */
    {
        const nrbf_wmember m[1] = { { "inner", NRBF_BT_CLASS, 0, "Test.Pair", 2 } };
        memset(&b, 0, sizeof b);
        (void)nrbf_put_header(&b, -1, -1);
        (void)nrbf_put_library(&b, 2, "TestLib, Version=1");
        (void)nrbf_put_class(&b, -1, "Test.Pair", m, 1u, 2);
        for (int32_t i = 2; i < 60; i++) (void)nrbf_put_class_with_id(&b, -i, -1);
        (void)nrbf_put_null(&b);
        (void)nrbf_put_end(&b);
        opts(&o);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);        /* depth 32 */
        o.lim.max_depth = 64u;
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
        nrbf_free(&d);
        pc_buf_free(&b);
    }
    /* array length: huge object array described by a few bytes */
    {
        memset(&b, 0, sizeof b);
        (void)nrbf_put_header(&b, 1, -1);
        (void)nrbf_put_object_array(&b, 1, 0x7FFFFFF0u);
        (void)nrbf_put_nulls(&b, 0x7FFFFFF0u);
        (void)nrbf_put_end(&b);
        opts(&o);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_LIMIT);
        pc_buf_free(&b);
        /* within the cap: null runs fill exactly, overruns are rejected */
        (void)nrbf_put_header(&b, 1, -1);
        (void)nrbf_put_object_array(&b, 1, 1000u);
        (void)nrbf_put_nulls(&b, 999u);
        (void)nrbf_put_null(&b);
        (void)nrbf_put_end(&b);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
        CHECK(d.n_vals == 1000u);
        nrbf_free(&d);
        pc_buf_free(&b);
        (void)nrbf_put_header(&b, 1, -1);
        (void)nrbf_put_object_array(&b, 1, 1000u);
        (void)nrbf_put_nulls(&b, 1001u);
        (void)nrbf_put_end(&b);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_FORMAT);
        pc_buf_free(&b);
    }
    /* primitive arrays are referenced in place and must fit the input */
    {
        static const uint8_t prim[] = {
            0x00, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0, 0, 0,
            0x0F, 1, 0, 0, 0, 5, 0, 0, 0, 2, 'a', 'b', 'c', 'd', 'e', 0x0B
        };
        uint8_t bad[sizeof prim];
        CHECK(nrbf_parse(prim, sizeof prim, NULL, &d) == PC_OK);
        CHECK(d.n_objs == 1u && d.objs[0].kind == NRBF_O_PRIM_ARRAY && d.objs[0].n == 5u &&
              d.objs[0].data_len == 5u && memcmp(d.objs[0].data, "abcde", 5u) == 0);
        nrbf_free(&d);
        memcpy(bad, prim, sizeof bad);
        bad[25] = 0x40;                 /* length 0x40000005 */
        CHECK(nrbf_parse(bad, sizeof bad, NULL, &d) == PC_ERR_LIMIT);
        bad[25] = 0x00; bad[22] = 6;    /* one byte too long */
        CHECK(nrbf_parse(bad, sizeof bad, NULL, &d) == PC_ERR_FORMAT);
    }
}

static void t_malformed(void)
{
    pc_buf b;
    nrbf_opts o;
    nrbf_doc d;
    opts(&o);
    /* missing header, wrong version, record types outside the subset */
    {
        static const uint8_t no_header[] = { 0x0B };
        static const uint8_t v2[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 2, 0, 0, 0, 0, 0, 0, 0,
                                      0x0B };
        static const uint8_t method[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0,
                                          0, 0, 21, 0, 0, 0, 0 };
        static const uint8_t unknown[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0,
                                           0, 0, 99 };
        static const uint8_t top_ref[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0,
                                           0, 0, 9, 1, 0, 0, 0, 0x0B };
        static const uint8_t two_headers[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0,
                                               0, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1,
                                               0, 0, 0, 0, 0, 0, 0, 0x0B };
        static const uint8_t no_root[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0,
                                           0, 0, 0x0B };
        CHECK(nrbf_parse(no_header, sizeof no_header, &o, &d) == PC_ERR_FORMAT);
        CHECK(nrbf_parse(v2, sizeof v2, &o, &d) == PC_ERR_UNSUPPORTED);
        CHECK(nrbf_parse(method, sizeof method, &o, &d) == PC_ERR_UNSUPPORTED);
        CHECK(nrbf_parse(unknown, sizeof unknown, &o, &d) == PC_ERR_FORMAT);
        CHECK(nrbf_parse(top_ref, sizeof top_ref, &o, &d) == PC_ERR_FORMAT);
        CHECK(nrbf_parse(two_headers, sizeof two_headers, &o, &d) == PC_ERR_FORMAT);
        CHECK(nrbf_parse(no_root, sizeof no_root, &o, &d) == PC_ERR_FORMAT);
        CHECK(nrbf_parse(NULL, 0u, &o, &d) == PC_ERR_ARG);
    }
    /* duplicate id, dangling reference, unknown metadata id, unknown library */
    build_chain(&b, 2u);
    {
        uint8_t *q = (uint8_t *)malloc(b.n);
        nrbf_doc dd;
        size_t at = 0;
        CHECK(nrbf_parse(b.p, b.n, &o, &dd) == PC_OK);
        nrbf_free(&dd);
        if (q) {
            /* ClassWithId record: 0x01, id 101, meta 1 */
            for (size_t i = 0; i + 9u <= b.n; i++)
                if (b.p[i] == 1u && b.p[i + 1] == 101u && b.p[i + 5] == 1u) { at = i; break; }
            CHECK(at != 0u);
            memcpy(q, b.p, b.n);
            q[at + 1] = 1u;                 /* id 1 again */
            CHECK(nrbf_parse(q, b.n, &o, &d) == PC_ERR_FORMAT);
            memcpy(q, b.p, b.n);
            q[at + 5] = 77u;                /* metadata of an unknown object */
            CHECK(nrbf_parse(q, b.n, &o, &d) == PC_ERR_FORMAT);
            memcpy(q, b.p, b.n);
            q[at + 14] = 99u;               /* next -> 99 instead of 1: dangling */
            CHECK(nrbf_parse(q, b.n, &o, &d) == PC_ERR_FORMAT);
            memcpy(q, b.p, b.n);
            q[17 + 1] = 3u;                 /* library id 3, class still uses 2 */
            CHECK(nrbf_parse(q, b.n, &o, &d) == PC_ERR_FORMAT);
            memcpy(q, b.p, b.n);
            q[17 + 5] = 0x85u;              /* LPS with a continuation into the name */
            CHECK(nrbf_parse(q, b.n, &o, &d) != PC_OK);
            free(q);
        }
    }
    pc_buf_free(&b);
    {   /* five-byte LPS beyond INT32_MAX and null runs outside arrays */
        static const uint8_t lps[] = { 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0, 0, 0,
                                       0, 6, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x0B };
        CHECK(nrbf_parse(lps, sizeof lps, &o, &d) == PC_ERR_FORMAT);
    }
    {
        const nrbf_wmember m[1] = { { "x", NRBF_BT_OBJECT, 0, NULL, 0 } };
        memset(&b, 0, sizeof b);
        (void)nrbf_put_header(&b, 1, -1);
        (void)nrbf_put_library(&b, 2, "TestLib, Version=1");
        (void)nrbf_put_class(&b, 1, "Test.Pair", m, 1u, 2);
        (void)nrbf_put_nulls(&b, 2u);
        (void)nrbf_put_end(&b);
        CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_ERR_FORMAT);
        pc_buf_free(&b);
    }
}

/* Random mutations of a valid stream: never a crash or a leak. */
static void t_mutation(void)
{
    pc_buf b;
    nrbf_opts o;
    uint32_t iters = g_quick ? 4000u : 40000u, ok = 0;
    uint8_t *q;
    build_chain(&b, 12u);
    opts(&o);
    q = (uint8_t *)malloc(b.n);
    for (uint32_t it = 0; q && it < iters; it++) {
        nrbf_doc d;
        pc_status st;
        size_t len = b.n;
        memcpy(q, b.p, b.n);
        for (uint32_t k = 0, m = 1u + rndu(3); k < m; k++) q[rndu((uint32_t)b.n)] = rnd8();
        if (rndu(5) == 0) len = rndu((uint32_t)b.n);
        st = nrbf_parse(q, len, &o, &d);
        CHECK(st == PC_OK || st == PC_ERR_FORMAT || st == PC_ERR_UNSUPPORTED ||
              st == PC_ERR_LIMIT);
        if (st == PC_OK) { ok++; CHECK(nrbf_get(&d, d.root_id) != NULL); }
        nrbf_free(&d);
    }
    INFO("%u of %u mutated streams parsed", ok, iters);
    free(q);
    pc_buf_free(&b);
}

static void t_dump(void)
{
    pc_buf b, out;
    nrbf_opts o;
    nrbf_doc d;
    static const char want[] =
        "Header root=#1 header=-1 1.0\n"
        "Library #2 TestLib, Version=1.0.0.0\n"
        "ClassWithMembersAndTypes #1 Test.Node lib=#2 {v:Int32, next:Class(Test.Node,#2), "
        "s:String}\n"
        "  .v = Int32 0\n"
        "  .next = -> #101\n"
        "  .s = String #10000 \"n0\"\n"
        "ClassWithId #101 meta=#1 Test.Node\n"
        "  .v = Int32 7\n"
        "  .next = -> #1\n"
        "  .s = String #10001 \"n1\"\n"
        "MessageEnd\n";
    static const char want_struct[] =
        "Header root=#1 header=-1 1.0\n"
        "Library #2 TestLib\n"
        "ClassWithMembersAndTypes #1 Test.Node lib=#2 {v:Int32, next:Class(Test.Node,#2), "
        "s:String}\n"
        "  .v = Int32\n"
        "  .next = -> #3\n"
        "  .s = String #4\n"
        "ClassWithId #3 meta=#1 Test.Node\n"
        "  .v = Int32\n"
        "  .next = -> #1\n"
        "  .s = String #5\n"
        "MessageEnd\n";
    build_chain(&b, 2u);
    opts(&o);
    memset(&out, 0, sizeof out);
    o.dump = &out;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
    CHECK(out.n == sizeof want - 1u && memcmp(out.p, want, out.n) == 0);
    if (out.n != sizeof want - 1u || memcmp(out.p, want, out.n) != 0)
        fwrite(out.p, 1u, out.n, stdout);
    nrbf_free(&d);
    pc_buf_free(&out);
    o.dump_flags = NRBF_DUMP_STRUCT;
    CHECK(nrbf_parse(b.p, b.n, &o, &d) == PC_OK);
    CHECK(out.n == sizeof want_struct - 1u && memcmp(out.p, want_struct, out.n) == 0);
    if (out.n != sizeof want_struct - 1u || memcmp(out.p, want_struct, out.n) != 0)
        fwrite(out.p, 1u, out.n, stdout);
    nrbf_free(&d);
    pc_buf_free(&out);
    /* errors end the dump with a "!!" line */
    CHECK(nrbf_parse(b.p, b.n - 1u, &o, &d) == PC_ERR_FORMAT);
    CHECK(out.n > 3u && strstr((const char *)out.p, "!! ") != NULL);
    pc_buf_free(&out);
    pc_buf_free(&b);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_writer_bytes);
    RUN(t_parse_chain_and_cycle);
    RUN(t_whitelists);
    RUN(t_caps);
    RUN(t_malformed);
    RUN(t_mutation);
    RUN(t_dump);
    return pc_test_finish();
}
