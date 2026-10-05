/* test_pdn_dump.c - .pdn dump tool and structure tests (lane L6C).
 *
 * As a test (CTest passes --quick): dumps synthetic and sample files and
 * checks that re-saving a Paint.NET 4.x/5.x sample reproduces its NRBF
 * record structure, member names and types and object ids exactly.
 *
 * As a tool:
 *   test_pdn_dump --dump [--struct] [--keep-ids] FILE...   print dumps
 *   test_pdn_dump --resave IN OUT                           load and save
 *   test_pdn_dump --compare FILE...                         re-save each file and
 *       compare structure dumps; prints MATCH (identical records and ids),
 *       LAYOUT (identical apart from the metadata items), SHARED (the
 *       original shares a blend op or name object between layers), ORDER
 *       (the original stores chunks out of order), V3 (a Paint.NET 3.x
 *       layout) or DIFF with the first differing line.
 *   test_pdn_dump --export FILE PREFIX                      write PREFIX.txt (size
 *       and layer properties) and PREFIX.bin (BGRA of every layer, bottom
 *       first) for cross-checks against other readers (pypdn).
 */
#include "pdn_util.h"

static int g_tool_fail = 0;

static int dump_file(const char *path, uint32_t flags)
{
    size_t n;
    uint8_t *p = pdn_read_file(path, &n);
    pc_buf out;
    pc_status st;
    if (!p) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    memset(&out, 0, sizeof out);
    st = pdn_dump(p, n, flags, &out);
    printf("== %s (%zu bytes): %s\n", path, n, pc_status_str(st));
    if (out.n) fwrite(out.p, 1u, out.n, stdout);
    pc_buf_free(&out);
    free(p);
    return st == PC_OK ? 0 : 1;
}

/* Structure dump (ids kept) of a buffer; NULL on error. */
static char *struct_dump(const uint8_t *p, size_t n, uint32_t extra)
{
    pc_buf out;
    memset(&out, 0, sizeof out);
    if (pdn_dump(p, n, NRBF_DUMP_STRUCT | extra, &out) != PC_OK ||
        pc_buf_put_u8(&out, 0u) != PC_OK) {
        pc_buf_free(&out);
        return NULL;
    }
    return (char *)out.p;
}

/* First line where a and b differ, or NULL. */
static const char *first_diff(const char *a, const char *b)
{
    const char *la = a;
    while (*a && *a == *b) {
        if (*a == '\n') la = a + 1;
        a++; b++;
    }
    return (*a == 0 && *b == 0) ? NULL : la;
}

/* Drops the element lines of the document metadata array (the first
 * BinaryArray of KeyValuePair) and its length, so documents with other
 * metadata items compare by layout. */
static void strip_metadata(char *s)
{
    char *a = strstr(s, "BinaryArray "), *o, *line, *eol;
    if (!a || !(eol = strchr(a, '\n'))) return;
    {
        char *nn = strstr(a, " n=");
        if (nn && nn < eol) {
            char *e = nn + 3;
            while (*e >= '0' && *e <= '9') e++;
            nn[3] = '*';
            memmove(nn + 4, e, strlen(e) + 1u);
        }
    }
    o = strchr(a, '\n') + 1;
    line = o;
    while (line[0] == ' ' && line[1] == ' ') {
        eol = strchr(line, '\n');
        if (!eol) break;
        line = eol + 1;
    }
    memmove(o, line, strlen(line) + 1u);
}

/* Renumbers "#id" tokens by first mention (after strip_metadata). Returns a
 * new string. */
static char *renumber_ids(const char *s)
{
    size_t n = strlen(s), cap = n * 2u + 16u, o = 0, nk = 0;
    char *out = (char *)malloc(cap);
    long *keys = (long *)malloc(sizeof(long) * (n / 2u + 1u));
    if (!out || !keys) { free(out); free(keys); return NULL; }
    for (size_t i = 0; i < n;) {
        if (s[i] == '#' && (s[i + 1] == '-' || (s[i + 1] >= '0' && s[i + 1] <= '9'))) {
            char *e;
            long v = strtol(s + i + 1, &e, 10);
            size_t k = 0;
            while (k < nk && keys[k] != v) k++;
            if (k == nk) keys[nk++] = v;
            o += (size_t)snprintf(out + o, cap - o, "#%s%zu", v < 0 ? "-" : "", k + 1u);
            i = (size_t)(e - s);
        } else {
            out[o++] = s[i++];
        }
    }
    out[o] = 0;
    free(keys);
    return out;
}

typedef enum {
    CMP_MATCH, CMP_LAYOUT, CMP_SHARED, CMP_ORDER, CMP_V3, CMP_DIFF, CMP_ERROR
} cmp_result;

static cmp_result compare_resave(const uint8_t *p, size_t n, char *why, size_t why_n)
{
    pc_doc *d = NULL;
    pc_image_meta meta;
    pdn_info info;
    pdn_save_opts o;
    pc_buf out;
    char *a = NULL, *b = NULL;
    cmp_result r = CMP_ERROR;
    pc_status st = pdn_load_ex(p, n, NULL, &d, &meta, &info);
    why[0] = 0;
    memset(&out, 0, sizeof out);
    if (st != PC_OK) { snprintf(why, why_n, "load: %s", pc_status_str(st)); goto done; }
    pdn_save_opts_default(&o);
    o.version = info.saved_with[0] ? info.saved_with : PDN_COMPAT_VERSION;
    o.list_capacity = info.list_capacity;           /* edit history, not structure */
    o.intern_names = info.names_shared || !info.names_equal;
    st = pdn_save_ex(d, &meta, &o, NULL, &out);
    if (st != PC_OK) { snprintf(why, why_n, "save: %s", pc_status_str(st)); goto done; }
    a = struct_dump(p, n, NRBF_DUMP_KEEP_IDS);
    b = struct_dump(out.p, out.n, NRBF_DUMP_KEEP_IDS);
    if (!a || !b) { snprintf(why, why_n, "dump failed"); goto done; }
    if (!first_diff(a, b)) { r = CMP_MATCH; goto done; }
    /* same layout apart from the metadata items: compare with renumbered ids */
    strip_metadata(a);
    strip_metadata(b);
    {
        char *ra = renumber_ids(a), *rb = renumber_ids(b);
        free(a); free(b);
        a = ra; b = rb;
    }
    if (!a || !b) { snprintf(why, why_n, "renumber failed"); goto done; }
    if (getenv("PDN_CMP_DEBUG")) {     /* tool use: keep both dumps for diff(1) */
        char path[512];
        snprintf(path, sizeof path, "%s/a.txt", getenv("PDN_CMP_DEBUG"));
        (void)pdn_write_file(path, (const uint8_t *)a, strlen(a));
        snprintf(path, sizeof path, "%s/b.txt", getenv("PDN_CMP_DEBUG"));
        (void)pdn_write_file(path, (const uint8_t *)b, strlen(b));
    }
    {
        const char *dl = first_diff(a, b);
        if (!dl) { r = CMP_LAYOUT; goto done; }
        snprintf(why, why_n, "%.*s", (int)(strchr(dl, '\n') ? strchr(dl, '\n') - dl : 80), dl);
        /* the original shares one object between layers (a blend op or a
         * name string), which depends on its edit history */
        if (strstr(why, ".blendOp = ") || strstr(why, ".name = ")) r = CMP_SHARED;
        else if (strstr(why, "chunks out of order")) r = CMP_ORDER;
        else if (strstr(why, "Library #3 System")) r = CMP_V3;
        else r = CMP_DIFF;
    }
done:
    free(a); free(b);
    pc_buf_free(&out);
    pc_meta_free(&meta);
    pc_doc_destroy(d);
    return r;
}

static int tool(int argc, char **argv)
{
    uint32_t flags = 0;
    int i = 2;
    if (strcmp(argv[1], "--dump") == 0) {
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (strcmp(argv[i], "--struct") == 0) flags |= NRBF_DUMP_STRUCT;
            else if (strcmp(argv[i], "--keep-ids") == 0) flags |= NRBF_DUMP_KEEP_IDS;
        }
        for (; i < argc; i++) g_tool_fail |= dump_file(argv[i], flags);
        return g_tool_fail;
    }
    if (strcmp(argv[1], "--resave") == 0 && argc == 4) {
        size_t n;
        uint8_t *p = pdn_read_file(argv[2], &n);
        pc_doc *d = NULL;
        pc_image_meta meta;
        pc_buf out;
        pc_status st;
        memset(&out, 0, sizeof out);
        if (!p) return 1;
        st = pc_codec_pdn.load(p, n, NULL, &d, &meta);
        if (st == PC_OK) st = pc_codec_pdn.save(d, &meta, NULL, NULL, &out);
        if (st == PC_OK && !pdn_write_file(argv[3], out.p, out.n)) st = PC_ERR_IO;
        printf("%s -> %s: %s\n", argv[2], argv[3], pc_status_str(st));
        pc_buf_free(&out);
        pc_meta_free(&meta);
        pc_doc_destroy(d);
        free(p);
        return st == PC_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "--export") == 0 && argc == 4) {
        size_t n;
        uint8_t *p = pdn_read_file(argv[2], &n);
        pc_doc *d = NULL;
        pc_image_meta meta;
        pc_status st;
        char path[512];
        FILE *f;
        if (!p) return 1;
        st = pc_codec_pdn.load(p, n, NULL, &d, &meta);
        free(p);
        if (st != PC_OK) { printf("%s: %s\n", argv[2], pc_status_str(st)); return 1; }
        snprintf(path, sizeof path, "%s.txt", argv[3]);
        f = fopen(path, "wb");
        if (f) {
            fprintf(f, "%u %u %u %g %g\n", d->w, d->h, d->n_layers, meta.dpi_x, meta.dpi_y);
            for (uint32_t li = 0; li < d->n_layers; li++)
                fprintf(f, "%d %u %u %s\n", (int)d->stack[li]->mode, d->stack[li]->opacity,
                        d->stack[li]->visible ? 1u : 0u, d->stack[li]->name);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s.bin", argv[3]);
        f = fopen(path, "wb");
        if (f) {
            pc_px32 *row = (pc_px32 *)malloc((size_t)d->w * sizeof *row);
            for (uint32_t li = 0; row && li < d->n_layers; li++)
                for (uint32_t y = 0; y < d->h; y++) {
                    pc_layer_read_rect(d, d->stack[li], pc_rect_make(0, (int32_t)y,
                                       (int32_t)d->w, 1), row, d->w);
                    fwrite(row, sizeof *row, d->w, f);
                }
            free(row);
            fclose(f);
        }
        pc_meta_free(&meta);
        pc_doc_destroy(d);
        return 0;
    }
    if (strcmp(argv[1], "--compare") == 0) {
        static const char *const names[] = { "MATCH", "LAYOUT", "SHARED", "ORDER", "V3",
                                             "DIFF", "ERROR" };
        unsigned long count[7] = { 0, 0, 0, 0, 0, 0, 0 };
        for (; i < argc; i++) {
            size_t n;
            uint8_t *p = pdn_read_file(argv[i], &n);
            char why[256];
            cmp_result r;
            if (!p) { fprintf(stderr, "cannot read %s\n", argv[i]); continue; }
            r = compare_resave(p, n, why, sizeof why);
            count[r]++;
            printf("%-6s %s %s\n", names[r], argv[i], why);
            free(p);
        }
        printf("MATCH %lu LAYOUT %lu SHARED %lu ORDER %lu V3 %lu DIFF %lu ERROR %lu\n",
               count[0], count[1], count[2], count[3], count[4], count[5], count[6]);
        return (count[5] || count[6]) ? 1 : 0;
    }
    fprintf(stderr, "usage: see the comment at the top of test_pdn_dump.c\n");
    return 2;
}

/* ---- tests ---------------------------------------------------------------------- */
static void t_dump_synthetic(void)
{
    pc_doc *d = pdn_random_doc(70, 33, 3);
    pc_buf f, out;
    memset(&f, 0, sizeof f);
    memset(&out, 0, sizeof out);
    CHECK(d != NULL);
    CHECK(pdn_save_ex(d, NULL, NULL, NULL, &f) == PC_OK);
    CHECK(pdn_dump(f.p, f.n, 0u, &out) == PC_OK);
    CHECK(pc_buf_put_u8(&out, 0u) == PC_OK);
    if (out.p) {
        const char *s = (const char *)out.p;
        CHECK(strstr(s, "ClassWithMembersAndTypes #1 PaintDotNet.Document lib=#2") != NULL);
        CHECK(strstr(s, "Library #2 PaintDotNet.Data, Version=" PDN_COMPAT_VERSION) != NULL);
        CHECK(strstr(s, "ClassWithMembersAndTypes #3 PaintDotNet.LayerList") != NULL);
        CHECK(strstr(s, ".parent = -> #1") != NULL);
        CHECK(strstr(s, ".ArrayList+_items = -> #7") != NULL);
        CHECK(strstr(s, "SystemClassWithMembersAndTypes #-8 System.Collections.Generic.") != NULL);
        CHECK(strstr(s, "ArraySingleObject #7 n=4") != NULL);
        CHECK(strstr(s, "ClassWithMembersAndTypes #20 PaintDotNet.BitmapLayer lib=#2") != NULL);
        CHECK(strstr(s, "Library #23 PaintDotNet.Core") != NULL);
        CHECK(strstr(s, "block 2 format=0 chunk_size=262144 chunks=1") != NULL);
        CHECK(strstr(s, "trailing bytes: 0") != NULL);
        CHECK(strstr(s, "MessageEnd") != NULL);
        CHECK(strstr(s, "<thumb png=\"(") != NULL);
    }
    pc_buf_free(&out);
    /* structure dumps of two saves of documents with the same layout match */
    {
        pc_doc *e = pdn_random_doc(70, 33, 3);
        pc_buf g;
        char *a, *b;
        memset(&g, 0, sizeof g);
        CHECK(e != NULL);
        for (uint32_t i = 0; e && d && i < 3; i++) {
            e->stack[i]->mode = d->stack[i]->mode;
            memcpy(e->stack[i]->name, d->stack[i]->name, sizeof e->stack[i]->name);
        }
        CHECK(pdn_save_ex(e, NULL, NULL, NULL, &g) == PC_OK);
        a = struct_dump(f.p, f.n, NRBF_DUMP_KEEP_IDS);
        b = struct_dump(g.p, g.n, NRBF_DUMP_KEEP_IDS);
        CHECK(a && b && first_diff(a, b) == NULL);
        free(a); free(b);
        pc_buf_free(&g);
        pc_doc_destroy(e);
    }
    pc_buf_free(&f);
    pc_doc_destroy(d);
}

/* Re-saving real 4.x/5.x samples reproduces their record structure. */
static void t_samples_resave_structure(void)
{
    int found = 0;
    for (size_t i = 0; k_pdn_samples[i]; i++) {
        char path[256], why[256];
        size_t n;
        uint8_t *p;
        cmp_result r;
        snprintf(path, sizeof path, PDN_SAMPLE_DIR "%s", k_pdn_samples[i]);
        p = pdn_read_file(path, &n);
        if (!p) continue;
        found++;
        r = compare_resave(p, n, why, sizeof why);
        if (strstr(path, "oldPDN3510")) {
            CHECK(r == CMP_V3);         /* 3.x layout differs by design (no blendMode) */
        } else {
            if (r != CMP_MATCH && r != CMP_LAYOUT) INFO("%s: %d %s", path, (int)r, why);
            CHECK(r == CMP_MATCH || r == CMP_LAYOUT);
        }
        free(p);
    }
    if (!found) INFO("no samples in %s, skipped", PDN_SAMPLE_DIR);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--quick") != 0) return tool(argc, argv);
    pc_test_init(argc, argv);
    (void)rnd8();
    (void)rndu(1);
    RUN(t_dump_synthetic);
    RUN(t_samples_resave_structure);
    return pc_test_finish();
}
