/* test_lib_zip.c - the ORA zip reader and writer (src/codec/zip.c, white
 * box): round trips, determinism, corrupt central directories, local
 * header mismatches, zip bombs (ratio, declared sizes, totals), ZIP64 and
 * encryption rejection, CRC checks, mutation fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/zip.h"

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t g32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Build a small archive: text (deflated), random bytes (stored), empty,
 * UTF-8 name. */
static void make_archive(pc_buf *out, uint8_t *rnd_data, size_t rn)
{
    pc_zipw w;
    static const char txt[] = "hello hello hello hello hello hello hello hello zip";
    pc_zipw_init(&w, out);
    CHECK(pc_zipw_add(&w, "mimetype", "image/openraster", 16, false) == PC_OK);
    CHECK(pc_zipw_add(&w, "a.txt", txt, sizeof txt - 1, true) == PC_OK);
    CHECK(pc_zipw_add(&w, "dir/r.bin", rnd_data, rn, true) == PC_OK);     /* stored: random */
    CHECK(pc_zipw_add(&w, "empty", "", 0, true) == PC_OK);
    CHECK(pc_zipw_add(&w, "d\xc3\xa9j\xc3\xa0.txt", "x", 1, false) == PC_OK);
    CHECK(pc_zipw_finish(&w) == PC_OK);
    pc_zipw_free(&w);
}

static void t_roundtrip(void)
{
    uint8_t rd[3000];
    pc_buf a, b;
    pc_zip z;
    const pc_zip_entry *e;
    uint8_t *data;
    size_t len;
    for (size_t i = 0; i < sizeof rd; i++) rd[i] = rnd8();
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    pc_buf_put_u8(&a, 0xAB);                  /* archive not at buffer offset 0 */
    make_archive(&a, rd, sizeof rd);
    make_archive(&b, rd, sizeof rd);
    CHECK(a.n == b.n + 1 && memcmp(a.p + 1, b.p, b.n) == 0);   /* deterministic */
    CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
    CHECK(z.count == 5);
    CHECK(strcmp(z.e[0].name, "mimetype") == 0 && z.e[0].method == 0);
    /* mimetype stored, uncompressed, right after the first local header */
    CHECK(memcmp(b.p + 30, "mimetypeimage/openraster", 24) == 0);
    e = pc_zip_find(&z, "a.txt");
    CHECK(e && e->method == 8);
    CHECK(pc_zip_read(&z, e, &data, &len) == PC_OK && len == 51 && data[len] == 0);
    CHECK(memcmp(data, "hello hello", 11) == 0);
    free(data);
    e = pc_zip_find(&z, "dir/r.bin");
    CHECK(e && e->method == 0);              /* deflate did not help */
    CHECK(pc_zip_read(&z, e, &data, &len) == PC_OK && len == sizeof rd &&
          memcmp(data, rd, len) == 0);
    free(data);
    e = pc_zip_find(&z, "empty");
    CHECK(e && pc_zip_read(&z, e, &data, &len) == PC_OK && len == 0);
    free(data);
    e = pc_zip_find(&z, "d\xc3\xa9j\xc3\xa0.txt");
    CHECK(e && (e->flags & 0x800) != 0);
    CHECK(pc_zip_find(&z, "missing") == NULL);
    pc_zip_close(&z);
    pc_buf_free(&a);
    pc_buf_free(&b);
}

static size_t find_eocd(const pc_buf *b)
{
    for (size_t i = b->n - 22; i > 0; i--) if (g32(b->p + i) == 0x06054b50u) return i;
    return 0;
}

static void t_corrupt_directory(void)
{
    uint8_t rd[100];
    pc_buf b;
    pc_zip z;
    size_t eocd, cd;
    memset(rd, 1, sizeof rd);
    memset(&b, 0, sizeof b);
    make_archive(&b, rd, sizeof rd);
    eocd = find_eocd(&b);
    cd = g32(b.p + eocd + 16);
#define MUT(expect, stmt) do { uint8_t *c = (uint8_t *)malloc(b.n); size_t cn = b.n; \
        memcpy(c, b.p, b.n); stmt; { pc_status st_ = pc_zip_open(&z, c, cn, NULL); \
        CHECK(st_ == (expect)); \
        if (st_ != (expect)) INFO("line %d: %s", __LINE__, pc_status_str(st_)); \
        if (st_ == PC_OK) pc_zip_close(&z); \
        else CHECK(z.e == NULL && z.count == 0); } free(c); } while (0)
    MUT(PC_OK, (void)0);
    MUT(PC_ERR_FORMAT, w32(c + eocd + 16, (uint32_t)b.n));           /* CD offset past end */
    MUT(PC_ERR_FORMAT, w32(c + eocd + 12, 0xFFFFFF));                /* CD size too large */
    MUT(PC_ERR_FORMAT, (w16(c + eocd + 8, 200), w16(c + eocd + 10, 200)));   /* count too big */
    MUT(PC_ERR_UNSUPPORTED, w16(c + eocd + 8, 4));                   /* entries mismatch */
    MUT(PC_ERR_UNSUPPORTED, w16(c + eocd + 4, 1));                   /* multi-disk */
    MUT(PC_ERR_UNSUPPORTED, w32(c + eocd + 16, 0xFFFFFFFFu));        /* ZIP64 marker */
    MUT(PC_ERR_FORMAT, w32(c + cd, 0x12345678u));                    /* CD signature */
    MUT(PC_ERR_FORMAT, w16(c + cd + 28, 0xFFFF));                    /* name length overflow */
    MUT(PC_ERR_FORMAT, w32(c + cd + 42, (uint32_t)cd));              /* local header offset */
    MUT(PC_ERR_FORMAT, w32(c + cd + 42, 0xFFFFFFF0u));
    MUT(PC_ERR_FORMAT, c[30] = 'X');                                 /* local name mismatch */
    MUT(PC_ERR_FORMAT, w32(c + cd + 20, 0x7FFFFFFF));                /* data beyond CD */
    MUT(PC_ERR_UNSUPPORTED, w16(c + cd + 8, 1));                     /* encrypted */
    MUT(PC_ERR_UNSUPPORTED, w32(c + cd + 24, 0xFFFFFFFFu));          /* ZIP64 size */
    MUT(PC_ERR_FORMAT, w16(c + cd + 28, 0));                         /* empty name */
    MUT(PC_ERR_FORMAT, c[cd + 46 + 3] = 0);                          /* NUL inside a name */
    MUT(PC_ERR_FORMAT, cn = eocd + 10);                              /* truncated end record */
    MUT(PC_ERR_FORMAT, cn = 10);
    MUT(PC_ERR_FORMAT, w16(c + eocd + 20, 500));                     /* comment past end */
#undef MUT
    /* ZIP64 locator in front of the end record */
    {
        pc_buf t;
        memset(&t, 0, sizeof t);
        pc_buf_append(&t, b.p, eocd);
        {
            uint8_t loc[20];
            memset(loc, 0, sizeof loc);
            w32(loc, 0x07064b50u);
            pc_buf_append(&t, loc, sizeof loc);
        }
        pc_buf_append(&t, b.p + eocd, b.n - eocd);
        CHECK(pc_zip_open(&z, t.p, t.n, NULL) == PC_ERR_UNSUPPORTED);
        pc_buf_free(&t);
    }
    /* entry limit */
    {
        pc_zip_limits l;
        pc_zip_limits_default(&l);
        l.max_entries = 4;
        CHECK(pc_zip_open(&z, b.p, b.n, &l) == PC_ERR_LIMIT);
    }
    pc_buf_free(&b);
}

/* Archive with one entry whose header fields can be patched. */
static void one_entry(pc_buf *b, const void *data, size_t n, bool deflate)
{
    pc_zipw w;
    memset(b, 0, sizeof *b);
    pc_zipw_init(&w, b);
    pc_zipw_add(&w, "x", data, n, deflate);
    pc_zipw_finish(&w);
    pc_zipw_free(&w);
}

static void patch_sizes(pc_buf *b, uint32_t usize, uint32_t crc)
{
    size_t eocd = find_eocd(b), cd = g32(b->p + eocd + 16);
    w32(b->p + 22, usize); w32(b->p + cd + 24, usize);
    w32(b->p + 14, crc); w32(b->p + cd + 16, crc);
}

static void t_bombs(void)
{
    const size_t N = (size_t)16 << 20;
    uint8_t *zeros = (uint8_t *)calloc(N, 1);
    pc_buf b;
    pc_zip z;
    uint8_t *data;
    size_t len;
    const pc_zip_entry *e;
    /* 16 MiB of zeros deflates about 1000:1: refused before allocating */
    one_entry(&b, zeros, N, true);
    CHECK(b.n < N / 200);
    CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
    e = pc_zip_find(&z, "x");
    CHECK(e && pc_zip_read(&z, e, &data, &len) == PC_ERR_LIMIT && data == NULL);
    pc_zip_close(&z);
    {   /* allowed when the caller raises the ratio cap */
        pc_zip_limits l;
        pc_zip_limits_default(&l);
        l.max_ratio = 5000;
        CHECK(pc_zip_open(&z, b.p, b.n, &l) == PC_OK);
        e = pc_zip_find(&z, "x");
        CHECK(e && pc_zip_read(&z, e, &data, &len) == PC_OK && len == N);
        free(data);
        /* total budget across reads */
        CHECK(pc_zip_read(&z, e, &data, &len) == PC_OK);
        free(data);
        pc_zip_close(&z);
        l.max_total = N + N / 2;
        CHECK(pc_zip_open(&z, b.p, b.n, &l) == PC_OK);
        e = pc_zip_find(&z, "x");
        CHECK(pc_zip_read(&z, e, &data, &len) == PC_OK);
        free(data);
        CHECK(pc_zip_read(&z, e, &data, &len) == PC_ERR_LIMIT);
        pc_zip_close(&z);
        l.max_total = (uint64_t)4 << 30;
        l.max_entry_size = N - 1;
        CHECK(pc_zip_open(&z, b.p, b.n, &l) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_LIMIT);
        pc_zip_close(&z);
    }
    pc_buf_free(&b);
    /* declared size smaller or larger than the real stream; CRC mismatch */
    {
        static const char txt[] = "abcabcabcabcabcabcabcabcabcabcabcabcabcabc";
        one_entry(&b, txt, sizeof txt - 1, true);
        patch_sizes(&b, 10, 0);
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_FORMAT);
        pc_zip_close(&z);
        patch_sizes(&b, 100, 0);
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_FORMAT);
        pc_zip_close(&z);
        patch_sizes(&b, sizeof txt - 1, 12345);
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_FORMAT);
        pc_zip_close(&z);
        /* a 4 GiB claim with a tiny stream: refused by the size limit */
        patch_sizes(&b, 0xF0000000u, 0);
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_LIMIT);
        pc_zip_close(&z);
        pc_buf_free(&b);
    }
    /* stored entry whose sizes disagree; unsupported method */
    {
        size_t eocd, cd;
        one_entry(&b, "abcdef", 6, false);
        eocd = find_eocd(&b);
        cd = g32(b.p + eocd + 16);
        w32(b.p + cd + 24, 7);
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_ERR_FORMAT);
        w32(b.p + cd + 24, 6);
        w16(b.p + cd + 10, 12);              /* bzip2 */
        CHECK(pc_zip_open(&z, b.p, b.n, NULL) == PC_OK);
        CHECK(pc_zip_read(&z, pc_zip_find(&z, "x"), &data, &len) == PC_ERR_UNSUPPORTED);
        pc_zip_close(&z);
        pc_buf_free(&b);
    }
    free(zeros);
}

static void t_writer_limits(void)
{
    pc_buf b;
    pc_zipw w;
    memset(&b, 0, sizeof b);
    pc_zipw_init(&w, &b);
    CHECK(pc_zipw_add(&w, "", "x", 1, false) == PC_ERR_ARG);
    CHECK(pc_zipw_add(&w, "y", "x", 1, false) == PC_ERR_ARG);     /* sticky */
    CHECK(pc_zipw_finish(&w) == PC_ERR_ARG);
    pc_zipw_free(&w);
    pc_buf_free(&b);
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 3000u : 30000u, opened = 0, read_ok = 0;
    uint8_t rd[200];
    pc_buf b;
    uint8_t *buf;
    for (size_t i = 0; i < sizeof rd; i++) rd[i] = (uint8_t)(i * 7);
    memset(&b, 0, sizeof b);
    make_archive(&b, rd, sizeof rd);
    buf = (uint8_t *)malloc(b.n + 64);
    for (uint32_t it = 0; it < iters; it++) {
        size_t n = tu_mutate(b.p, b.n, buf, b.n + 64);
        pc_zip z;
        pc_zip_limits l;
        pc_zip_limits_default(&l);
        l.max_total = (uint64_t)1 << 24;
        if (pc_zip_open(&z, buf, n, &l) == PC_OK) {
            opened++;
            for (uint32_t i = 0; i < z.count; i++) {
                uint8_t *data;
                size_t len;
                if (pc_zip_read(&z, &z.e[i], &data, &len) == PC_OK) {
                    read_ok++;
                    CHECK(len == z.e[i].usize);
                    free(data);
                }
            }
            pc_zip_close(&z);
        }
    }
    INFO("zip fuzz: %u of %u opened, %u entries read", opened, iters, read_ok);
    free(buf);
    pc_buf_free(&b);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_roundtrip);
    RUN(t_corrupt_directory);
    RUN(t_bombs);
    RUN(t_writer_limits);
    RUN(t_fuzz);
    return pc_test_finish();
}
