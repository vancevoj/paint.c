/* test_i_settings.c - lane I: settings robustness (src/app/settings.c).
 * Atomic saves with a backup of the previous file, damaged files moved
 * aside with a fallback to the backup, format versioning in the header,
 * unknown keys kept, and random files that never crash the loader.
 * Fixed seeds; files go to the build tree. */
#include "app_test_util.h"

pc_status app_settings_write_text(const char *path, const char *text, size_t n);

static void paths(const char *name, char *dir, char *file, char *bak, char *bad, size_t cap)
{
    at_out_path(dir, cap, name);
    (void)pal_mkdirs(dir);
    pal_path_join(file, cap, dir, "settings.ini");
    snprintf(bak, cap, "%.900s.bak", file);
    snprintf(bad, cap, "%.900s.corrupt", file);
    (void)pal_remove(file);
    (void)pal_remove(bak);
    (void)pal_remove(bad);
}

static bool write_raw(const char *path, const void *data, size_t n)
{
    return pal_write_file_atomic(path, data, n) == PC_OK;
}

static char *read_all(const char *path)
{
    uint8_t *d = NULL;
    size_t n = 0;
    if (pal_read_file(path, 4u << 20, &d, &n) != PC_OK) return NULL;
    return (char *)d;
}

/* Saving keeps the previous file as .bak and leaves no temporary files. */
static void t_atomic_backup(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100];
    app_settings *s = app_settings_create();
    char **names = NULL;
    int n;
    char *text;
    paths("i_settings_atomic", dir, file, bak, bad, sizeof dir);
    app_settings_set(s, "a.one", "1");
    CHECK(app_settings_save(s, file) == PC_OK);
    CHECK(pal_file_exists(file) && !pal_file_exists(bak));
    app_settings_set(s, "a.two", "2");
    CHECK(app_settings_save(s, file) == PC_OK);
    CHECK(pal_file_exists(bak));
    text = read_all(bak);
    CHECK(text && strstr(text, "a.one=1") && !strstr(text, "a.two"));
    free(text);
    text = read_all(file);
    CHECK(text && strstr(text, "a.two=2"));
    free(text);
    /* the same content again does not touch the backup */
    CHECK(app_settings_save(s, file) == PC_OK);
    text = read_all(bak);
    CHECK(text && !strstr(text, "a.two"));
    free(text);
    n = pal_list_dir(dir, NULL, &names);
    CHECK(n == 2);                                  /* settings.ini, settings.ini.bak */
    for (int i = 0; i < n; i++)
        CHECK(strcmp(names[i], "settings.ini") == 0 || strcmp(names[i], "settings.ini.bak") == 0);
    pal_free_names(names, n);
    app_settings_destroy(s);
}

/* A damaged file is moved aside and the backup is used. */
static void t_corrupt_fallback(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100];
    static const char garbage[] = "colors.primary=FF00\0\x01\x02 binary junk\xff\xfe\n";
    app_settings *s = app_settings_create();
    paths("i_settings_corrupt", dir, file, bak, bad, sizeof dir);
    CHECK(write_raw(bak, "# old\nview.rulers=1\nui.theme=2\n", 31u));
    CHECK(write_raw(file, garbage, sizeof garbage - 1u));
    CHECK(app_settings_load(s, file) == PC_OK);
    CHECK(app_settings_bool(s, "view.rulers", false));
    CHECK(app_settings_int(s, "ui.theme", 0) == 2);
    CHECK(app_settings_get(s, "colors.primary") == NULL);
    CHECK(pal_file_exists(bad) && !pal_file_exists(file));
    CHECK(app_settings_dirty(s));                   /* the main file gets rewritten */
    CHECK(app_settings_save(s, file) == PC_OK);
    app_settings_destroy(s);
    s = app_settings_create();
    CHECK(app_settings_load(s, file) == PC_OK && app_settings_bool(s, "view.rulers", false));
    app_settings_destroy(s);
}

/* Without a backup a damaged file gives defaults (and no crash). */
static void t_corrupt_no_backup(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100];
    uint8_t junk[512];
    app_settings *s = app_settings_create();
    paths("i_settings_nobak", dir, file, bak, bad, sizeof dir);
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37u + 11u);
    junk[3] = 0;
    CHECK(write_raw(file, junk, sizeof junk));
    CHECK(app_settings_load(s, file) == PC_OK);
    CHECK(app_settings_count(s) == 0u);
    CHECK(pal_file_exists(bad));
    app_settings_destroy(s);
}

/* Larger than the limit, or not UTF-8: damaged too. */
static void t_limits(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100];
    size_t big = APP_SETTINGS_MAX_FILE + 4096u;
    char *buf = (char *)malloc(big);
    app_settings *s = app_settings_create();
    paths("i_settings_limits", dir, file, bak, bad, sizeof dir);
    if (!buf || !s) { free(buf); app_settings_destroy(s); CHECK(false); return; }
    for (size_t i = 0; i < big; i++) buf[i] = (char)(i % 64u == 63u ? '\n' : 'k');
    CHECK(write_raw(file, buf, big));
    CHECK(write_raw(bak, "x.ok=1\n", 7u));
    CHECK(app_settings_load(s, file) == PC_OK && app_settings_bool(s, "x.ok", false));
    CHECK(pal_file_exists(bad));
    (void)pal_remove(bad);
    /* invalid UTF-8 (a lone continuation byte, an overlong lead) */
    CHECK(write_raw(file, "a.b=\x80\xC0\xAF\n", 8u));
    CHECK(app_settings_load(s, file) == PC_OK && app_settings_bool(s, "x.ok", false));
    CHECK(pal_file_exists(bad));
    /* valid multi-byte UTF-8 is fine */
    CHECK(write_raw(file, "a.name=caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xA8\n", 22u));
    CHECK(app_settings_load(s, file) == PC_OK);
    CHECK(app_settings_get(s, "a.name") &&
          strcmp(app_settings_get(s, "a.name"), "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xA8") == 0);
    free(buf);
    app_settings_destroy(s);
}

/* Format versions: written, kept for newer files, upgraded for older ones. */
static void t_versions(void)
{
    app_settings *s = app_settings_create();
    char *out = NULL;
    size_t len = 0;
    static const char newer[] = "# paint.c settings, format 7 (future)\nfuture.key=x\nui.theme=1\n";
    static const char older[] = "ui.theme=2\nrecent.0=/tmp/a.png\n";
    app_settings_set(s, "k.v", "1");
    CHECK(app_settings_serialize(s, &out, &len) == PC_OK);
    CHECK(out && strncmp(out, "# paint.c settings, format 1 ", 29u) == 0);
    free(out);
    CHECK(app_settings_parse(s, newer, sizeof newer - 1u) == 2u);
    CHECK(app_settings_get(s, "future.key") != NULL);  /* unknown keys are kept */
    CHECK(app_settings_serialize(s, &out, &len) == PC_OK);
    CHECK(out && strncmp(out, "# paint.c settings, format 7 ", 29u) == 0 &&
          strstr(out, "future.key=x"));
    free(out);
    CHECK(app_settings_parse(s, older, sizeof older - 1u) == 2u);
    CHECK(app_settings_serialize(s, &out, &len) == PC_OK);
    CHECK(out && strncmp(out, "# paint.c settings, format 1 ", 29u) == 0 &&
          strstr(out, "recent.0=/tmp/a.png"));
    free(out);
    app_settings_destroy(s);
}

/* BOM, CRLF line ends, sections, spaces around '=' and comments. */
static void t_syntax(void)
{
    static const char text[] = "\xEF\xBB\xBF# c\r\n; c2\r\n[view]\r\n  rulers = 1 \r\n"
                               "[]\r\nui.theme=2\r\nbad line\r\n=novalue\r\n";
    app_settings *s = app_settings_create();
    CHECK(app_settings_parse(s, text, sizeof text - 1u) == 2u);
    CHECK(app_settings_bool(s, "view.rulers", false));
    CHECK(app_settings_int(s, "ui.theme", 0) == 2);
    app_settings_destroy(s);
}

/* The background writer (autosave module) keeps the same guarantees. */
static void t_write_text(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100], sub[1100];
    char *text;
    paths("i_settings_text", dir, file, bak, bad, sizeof dir);
    pal_path_join(sub, sizeof sub, dir, "nested/deeper/settings.ini");
    CHECK(app_settings_write_text(sub, "a=1\n", 4u) == PC_OK);   /* creates folders */
    CHECK(app_settings_write_text(file, "a=1\n", 4u) == PC_OK);
    CHECK(app_settings_write_text(file, "a=2\n", 4u) == PC_OK);
    text = read_all(bak);
    CHECK(text && strcmp(text, "a=1\n") == 0);
    free(text);
    CHECK(app_settings_write_text(NULL, "x", 1u) == PC_ERR_ARG);
}

/* Random files: the loader never crashes and the limits hold. */
static void t_fuzz_files(void)
{
    char dir[1024], file[1024], bak[1100], bad[1100];
    int rounds = g_quick ? 60 : 600;
    uint8_t *buf = (uint8_t *)malloc(20000u);
    paths("i_settings_fuzz", dir, file, bak, bad, sizeof dir);
    if (!buf) { CHECK(false); return; }
    g_rng = 0x1234567887654321ull;
    for (int r = 0; r < rounds; r++) {
        size_t n = rndu(20000u);
        app_settings *s = app_settings_create();
        for (size_t i = 0; i < n; i++) {
            uint32_t k = rndu(16);
            buf[i] = k < 2 ? (uint8_t)'\n' : k < 4 ? (uint8_t)'=' : k < 5 ? (uint8_t)'[' :
                     k < 10 ? (uint8_t)('a' + rndu(26)) : rnd8();
        }
        CHECK(write_raw(file, buf, n));
        if (r % 3 == 0) CHECK(write_raw(bak, buf, n / 2u));
        CHECK(app_settings_load(s, file) == PC_OK);
        CHECK(app_settings_count(s) <= APP_SETTINGS_MAX_KEYS);
        app_settings_destroy(s);
        (void)pal_remove(bad);
        (void)pal_remove(bak);
    }
    free(buf);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_atomic_backup);
    RUN(t_corrupt_fallback);
    RUN(t_corrupt_no_backup);
    RUN(t_limits);
    RUN(t_versions);
    RUN(t_syntax);
    RUN(t_write_text);
    RUN(t_fuzz_files);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
