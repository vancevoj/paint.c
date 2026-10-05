/* test_i_packaging.c - lane I: the application icon and the packaging
 * metadata stay consistent with the code.
 *  - the embedded window icon decodes at every size (transparent corners,
 *    opaque tile, the colored c and the white dot);
 *  - assets/icons: the PNG set, the .ico (PNG entries) and the .icns are
 *    well formed and agree in size;
 *  - every extension the codec registry opens is associated by the Windows
 *    installer, every type is in the .desktop MimeType list, the AppStream
 *    media types and the macOS document types;
 *  - the licenses folder lists exactly the files it has.
 * Runs in tests/app (CTest working directory); repository files are read
 * relative to it. */
#include "app_test_util.h"
#include "app/app_io.h"

#define ROOT "../../"

static char *slurp(const char *rel, size_t *len)
{
    uint8_t *d = NULL;
    size_t n = 0;
    if (pal_read_file(rel, 16u << 20, &d, &n) != PC_OK) return NULL;
    if (len) *len = n;
    return (char *)d;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* PNG width from the IHDR chunk (0 when it is not a PNG). */
static uint32_t png_width(const uint8_t *p, size_t n)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n < 24u || memcmp(p, sig, 8u) != 0 || memcmp(p + 12, "IHDR", 4u) != 0) return 0;
    return be32(p + 16);
}

/* ---- the window icon ------------------------------------------------------------------------- */
static void t_icon_rgba(void)
{
    static const int32_t sizes[] = { 16, 32, 48, 64, 128, 256 };
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int32_t s = sizes[k];
        uint8_t *px = app_icon_rgba(s);
        int colored = 0, white = 0;
        CHECK(px != NULL);
        if (!px) continue;
        CHECK(px[3] == 0u);                      /* rounded corner */
        CHECK(px[((size_t)(s / 2) * (size_t)s + (size_t)(s / 8)) * 4u + 3u] == 255u);
        for (int32_t i = 0; i < s * s; i++) {
            const uint8_t *p = px + (size_t)i * 4u;
            int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            if (p[3] == 255u && mx > 150 && mx - mn > 80) colored++;
            if (p[3] == 255u && mn > 200) white++;
        }
        CHECK(colored > s * s / 20);             /* the c */
        CHECK(white > s * s / 400);              /* the dot (2 px wide at 16 px) */
        free(px);
    }
    CHECK(app_icon_rgba(0) == NULL && app_icon_rgba(5000) == NULL);
}

/* ---- icon files ------------------------------------------------------------------------------ */
static void t_icon_files(void)
{
    static const int sizes[] = { 16, 24, 32, 48, 64, 128, 256, 512, 1024 };
    size_t n = 0;
    uint8_t *d;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        char path[128];
        snprintf(path, sizeof path, ROOT "assets/icons/png/paintc-%d.png", sizes[k]);
        d = (uint8_t *)slurp(path, &n);
        CHECK(d && png_width(d, n) == (uint32_t)sizes[k]);
        free(d);
    }
    /* .ico: PNG entries 16..256 */
    d = (uint8_t *)slurp(ROOT "assets/icons/paintc.ico", &n);
    CHECK(d && n > 6u);
    if (d && n > 6u) {
        int count = d[4] | (d[5] << 8);
        CHECK(d[0] == 0 && d[1] == 0 && d[2] == 1 && count == 7);
        for (int i = 0; i < count && 6u + 16u * (size_t)(i + 1) <= n; i++) {
            const uint8_t *e = d + 6 + 16 * i;
            uint32_t w = e[0] ? e[0] : 256u, sz = le32(e + 8), off = le32(e + 12);
            CHECK((size_t)off + sz <= n && png_width(d + off, sz) == w);
        }
    }
    free(d);
    /* .icns: total length, PNG payloads with the right sizes */
    d = (uint8_t *)slurp(ROOT "assets/icons/paintc.icns", &n);
    CHECK(d && n > 8u && memcmp(d, "icns", 4u) == 0 && be32(d + 4) == (uint32_t)n);
    if (d && n > 8u) {
        size_t at = 8u;
        int entries = 0;
        bool have_1024 = false;
        while (at + 8u <= n) {
            uint32_t len = be32(d + at + 4);
            if (len < 8u || at + len > n) break;
            if (memcmp(d + at, "ic10", 4u) == 0)
                have_1024 = png_width(d + at + 8, len - 8u) == 1024u;
            if (memcmp(d + at, "icp4", 4u) == 0) CHECK(png_width(d + at + 8, len - 8u) == 16u);
            if (memcmp(d + at, "ic08", 4u) == 0) CHECK(png_width(d + at + 8, len - 8u) == 256u);
            entries++;
            at += len;
        }
        CHECK(at == n && entries == 11 && have_1024);
    }
    free(d);
}

/* ---- associations ---------------------------------------------------------------------------- */
typedef struct type_map { const char *id, *mime, *uti; } type_map;
static const type_map k_types[] = {
    { "pdn", "image/x-paintnet", "org.paintc.pdn" },
    { "png", "image/png", "public.png" },
    { "jpeg", "image/jpeg", "public.jpeg" },
    { "bmp", "image/bmp", "com.microsoft.bmp" },
    { "gif", "image/gif", "com.compuserve.gif" },
    { "tiff", "image/tiff", "public.tiff" },
    { "webp", "image/webp", "org.webmproject.webp" },
    { "tga", "image/x-tga", "com.truevision.tga-image" },
    { "dds", "image/x-dds", "com.microsoft.dds" },
    { "ora", "image/openraster", "org.openraster.ora" }
};

static void t_associations(void)
{
    char *nsi = slurp(ROOT "packaging/windows/paintc.nsi", NULL);
    char *desk = slurp(ROOT "packaging/linux/org.paintc.paintc.desktop", NULL);
    char *meta = slurp(ROOT "packaging/linux/org.paintc.paintc.metainfo.xml", NULL);
    char *mime = slurp(ROOT "packaging/linux/org.paintc.paintc.xml", NULL);
    char *plist = slurp(ROOT "src/app/platform/Info.plist.in", NULL);
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    CHECK(nsi && desk && meta && mime && plist);
    if (!nsi || !desk || !meta || !mime || !plist) goto out;
    CHECK(strstr(desk, "Icon=org.paintc.paintc\n") && strstr(desk, "Exec=paintc %F\n"));
    CHECK(strstr(plist, "<string>org.paintc.paintc</string>") &&
          strstr(plist, "<string>paintc.icns</string>"));
    CHECK(strstr(mime, "<glob pattern=\"*.pdn\"/>") && strstr(mime, "value=\"PDN3\""));
    for (size_t i = 0; i < n; i++) {
        const pc_codec *c = list[i];
        const type_map *t = NULL;
        const char *e = c->exts;
        char needle[96];
        if (!(c->flags & PC_CODEC_LOAD)) continue;
        for (size_t k = 0; k < sizeof k_types / sizeof k_types[0]; k++)
            if (strcmp(k_types[k].id, c->id) == 0) t = &k_types[k];
        CHECK(t != NULL);                        /* a new codec needs a row above */
        if (!t) {
            printf("  codec %s has no packaging metadata\n", c->id);
            continue;
        }
        snprintf(needle, sizeof needle, "%s;", t->mime);
        CHECK(strstr(desk, needle) != NULL);
        snprintf(needle, sizeof needle, "<mediatype>%s</mediatype>", t->mime);
        CHECK(strstr(meta, needle) != NULL);
        snprintf(needle, sizeof needle, "<string>%s</string>", t->uti);
        CHECK(strstr(plist, needle) != NULL);
        /* every extension in the installer (.pdn has its own program id) */
        while (*e) {
            char ext[16];
            size_t k = 0;
            while (*e && *e != ';' && k + 1u < sizeof ext) ext[k++] = *e++;
            ext[k] = '\0';
            if (*e == ';') e++;
            if (strcmp(ext, "pdn") == 0) {
                CHECK(strstr(nsi, "\"Software\\Classes\\.pdn\\OpenWithProgids\"") != NULL);
                continue;
            }
            snprintf(needle, sizeof needle, "!insertmacro ${MACRO} %s\n", ext);
            CHECK(strstr(nsi, needle) != NULL);
            if (!strstr(nsi, needle)) printf("  installer lacks .%s\n", ext);
        }
    }
out:
    free(nsi);
    free(desk);
    free(meta);
    free(mime);
    free(plist);
}

/* ---- licenses -------------------------------------------------------------------------------- */
static void t_licenses(void)
{
    char *index = slurp(ROOT "packaging/licenses/README.txt", NULL);
    char **names = NULL;
    int n = pal_list_dir(ROOT "packaging/licenses", NULL, &names);
    CHECK(pal_file_exists(ROOT "LICENSE") && pal_file_exists(ROOT "NOTICE"));
    CHECK(index != NULL && n >= 10);
    for (int i = 0; index && i < n; i++) {
        char needle[128];
        if (strcmp(names[i], "README.txt") == 0) continue;
        snprintf(needle, sizeof needle, "\n%s ", names[i]);
        CHECK(strstr(index, needle) != NULL);
        if (!strstr(index, needle))
            printf("  licenses/README.txt does not list %s\n", names[i]);
    }
    /* the components NOTICE names all have their text */
    {
        static const char *const need[] = { "SDL3.txt", "zlib.txt", "libspng.txt",
                                             "libjpeg-turbo.md", "libwebp.txt", "little-cms.txt",
                                             "bcdec.txt", "bc7enc.txt", "stb.txt",
                                             "Inter-OFL.txt" };
        for (size_t k = 0; k < sizeof need / sizeof need[0]; k++) {
            char path[128];
            snprintf(path, sizeof path, ROOT "packaging/licenses/%s", need[k]);
            CHECK(pal_file_exists(path));
        }
    }
    pal_free_names(names, n);
    free(index);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_icon_rgba);
    RUN(t_icon_files);
    RUN(t_associations);
    RUN(t_licenses);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
