/* test_plg_all.c - every optional plugin of plugins/ as it is built and
 * shipped (tests/plugins/CMakeLists.txt, docs/PLUGINS.md):
 *   - discovery: each folder of <build>/plugins/out holds <slug><suffix>
 *     and README.md, and every plugin target of this build has its folder;
 *   - all plugins loaded together from that folder through the real loader
 *     (afx_plugins_scan, as paint.c loads its plugins folder) next to the
 *     built-in effects: no Plugin Errors entry, no id clash;
 *   - each plugin loaded on its own (afx_plugins_load_file): author and
 *     version for the menu tooltip (the version equal to the one in the
 *     name of its release zip: the README card or release.conf), the
 *     fx_abi_version and fx_plugin_info exports, and every
 *     effect it registers run with default parameters through the effect
 *     runner on 256 x 256 photo-like, selected, transparent and object
 *     images (plg_test_util.h: no crash, deterministic, tiling and thread
 *     invariant, ROI-only writes, cancellation, time limit);
 *   - unload and load again: the same pixels;
 *   - the minimal plugin of docs/PLUGINS.md (tests/plugins/minimal_plugin.c)
 *     loads and passes the same checks.
 * PC_PLUGIN_OUT_DIR, PC_PLUGIN_SLUGS (comma separated), PC_PLUGIN_SOURCE_ROOT,
 * PC_PLUGIN_RELEASE_CONF and PC_PLUGIN_EXAMPLE come from CMake. */
#include "plg_test_util.h"

#ifndef PC_PLUGIN_OUT_DIR
#  define PC_PLUGIN_OUT_DIR "."
#endif
#ifndef PC_PLUGIN_SLUGS
#  define PC_PLUGIN_SLUGS ""
#endif
#ifndef PC_PLUGIN_SOURCE_ROOT
#  define PC_PLUGIN_SOURCE_ROOT "."
#endif
#ifndef PC_PLUGIN_RELEASE_CONF
#  define PC_PLUGIN_RELEASE_CONF "release.conf"
#endif
#ifndef PC_PLUGIN_EXAMPLE
#  define PC_PLUGIN_EXAMPLE "darken"
#endif

#define MAX_SLUGS 128

static char g_slug[MAX_SLUGS][64];
static int  g_nslug;

static void parse_slugs(void)
{
    const char *s = PC_PLUGIN_SLUGS;
    g_nslug = 0;
    while (*s && g_nslug < MAX_SLUGS) {
        size_t n = strcspn(s, ",");
        if (n > 0u && n < sizeof g_slug[0]) {
            memcpy(g_slug[g_nslug], s, n);
            g_slug[g_nslug][n] = '\0';
            g_nslug++;
        }
        s += n;
        if (*s == ',') s++;
    }
}

/* <out>/<slug>/<slug><suffix> */
static void plugin_path(char *buf, size_t cap, const char *slug)
{
    char dir[1024], file[128];
    size_t n = strlen(slug), k = strlen(pal_lib_suffix());
    file[0] = '\0';
    if (n + k < sizeof file) {
        memcpy(file, slug, n);
        memcpy(file + n, pal_lib_suffix(), k + 1u);
    }
    pal_path_join(dir, sizeof dir, PC_PLUGIN_OUT_DIR, slug);
    pal_path_join(buf, cap, dir, file);
}

static void folder_file(char *buf, size_t cap, const char *root, const char *slug,
                        const char *name)
{
    char dir[1024];
    pal_path_join(dir, sizeof dir, root, slug);
    pal_path_join(buf, cap, dir, name);
}

/* ---- the README card (packaging/plugins/plugin_meta.py) ------------------------------ */
static bool read_head(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return false;
    n = fread(buf, 1u, cap - 1u, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

/* The value of "key<sep>" at the start of a line of text[0..end), trimmed. */
static bool line_value(const char *text, const char *end, const char *key, char sep, char *out,
                       size_t cap)
{
    size_t k = strlen(key);
    for (const char *l = text; l && l < end; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
        const char *p = l, *v, *e;
        size_t n;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, k) != 0) continue;
        p += k;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != sep) continue;
        v = p + 1;
        while (*v == ' ' || *v == '\t') v++;
        e = v;
        while (*e && *e != '\n' && *e != '\r') e++;
        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
        n = (size_t)(e - v);
        if (n == 0u || n >= cap) return false;
        memcpy(out, v, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

/* The version the release zip of a plugin carries: the card's "version:"
 * line, else the plugin set version of release.conf. */
static void zip_version(const char *slug, char *out, size_t cap)
{
    static char text[16384];
    char path[1200];
    out[0] = '\0';
    folder_file(path, sizeof path, PC_PLUGIN_SOURCE_ROOT, slug, "README.md");
    if (read_head(path, text, sizeof text)) {
        const char *c = strstr(text, "<!-- paintc-plugin"), *e = c ? strstr(c, "-->") : NULL;
        if (c && e && line_value(c, e, "version", ':', out, cap)) return;
    }
    if (read_head(PC_PLUGIN_RELEASE_CONF, text, sizeof text))
        (void)line_value(text, text + strlen(text), "version", '=', out, cap);
}

/* Equal versions, where 1.0 equals 1.0.0 (numeric parts padded with 0). */
static bool same_version(const char *a, const char *b)
{
    for (;;) {
        char *ea, *eb;
        unsigned long x = 0, y = 0;
        if (*a) {
            x = strtoul(a, &ea, 10);
            if (ea == a || (*ea && *ea != '.')) return strcmp(a, b) == 0;
            a = *ea ? ea + 1 : ea;
        }
        if (*b) {
            y = strtoul(b, &eb, 10);
            if (eb == b || (*eb && *eb != '.')) return false;
            b = *eb ? eb + 1 : eb;
        }
        if (x != y) return false;
        if (!*a && !*b) return true;
    }
}

/* ---- discovery ------------------------------------------------------------------- */
static void t_discover(void)
{
    char **names = NULL, path[1200];
    int n = pal_list_dir(PC_PLUGIN_OUT_DIR, NULL, &names), found = 0;
    CHECK(g_nslug > 0);
    CHECK(n >= g_nslug);
    for (int i = 0; i < n; i++) {
        bool known = false;
        pal_path_join(path, sizeof path, PC_PLUGIN_OUT_DIR, names[i]);
        if (!pal_is_dir(path)) continue;
        for (int k = 0; k < g_nslug; k++) known = known || strcmp(names[i], g_slug[k]) == 0;
        if (!known) {
            INFO("%s is not a plugin of this build (left over from an older one?)", names[i]);
            continue;
        }
        found++;
    }
    CHECK(found == g_nslug);
    for (int k = 0; k < g_nslug; k++) {
        plugin_path(path, sizeof path, g_slug[k]);
        CHECK(pal_file_exists(path));
        if (!pal_file_exists(path)) fprintf(stderr, "    missing %s\n", path);
        folder_file(path, sizeof path, PC_PLUGIN_OUT_DIR, g_slug[k], "README.md");
        CHECK(pal_file_exists(path));
        folder_file(path, sizeof path, PC_PLUGIN_SOURCE_ROOT, g_slug[k], "README.md");
        CHECK(pal_file_exists(path));
        folder_file(path, sizeof path, PC_PLUGIN_SOURCE_ROOT, g_slug[k], "screenshot.png");
        if (!pal_file_exists(path))
            INFO("plugins/%s has no screenshot.png yet (the release zip needs one)", g_slug[k]);
    }
    pal_free_names(names, n);
    INFO("%d plugins: %s", g_nslug, PC_PLUGIN_SLUGS);
}

/* ---- all together, as from a plugins folder ------------------------------------------ */
static void t_load_together(void)
{
    plg_set s;
    int added = plg_open_dir(&s, PC_PLUGIN_OUT_DIR);
    CHECK(added > 0);
    CHECK(afx_plugins_error_count(s.plugins) == 0u);
    if (afx_plugins_error_count(s.plugins) != 0u) plg_print_errors(s.plugins);
    CHECK(afx_plugins_lib_count(s.plugins) >= (size_t)g_nslug);
    for (int k = 0; k < g_nslug; k++) {
        char want[1200];
        int effects = 0;
        plugin_path(want, sizeof want, g_slug[k]);
        for (uint32_t i = 0; i < s.nfx; i++) {
            const afx_plugin_info *info = afx_plugins_info(s.plugins, s.fx[i]);
            if (info && strcmp(pal_path_basename(info->path), pal_path_basename(want)) == 0)
                effects++;
        }
        CHECK(effects > 0);
        if (effects == 0) fprintf(stderr, "    %s added no effect\n", g_slug[k]);
    }
    plg_close(&s);
}

/* ---- each plugin on its own, every effect -------------------------------------------- */
static void t_effects(void)
{
    for (int k = 0; k < g_nslug; k++) {
        char path[1200];
        plg_set s;
        plugin_path(path, sizeof path, g_slug[k]);
        if (plg_open(&s, path)) {
            const afx_plugin_info *info = afx_plugins_info(s.plugins, s.fx[0]);
            pal_lib *lib = pal_lib_open(path);
            CHECK(info != NULL);
            if (info) {
                char zv[64];
                CHECK(info->author[0] != '\0');      /* fx_plugin_info("author") */
                CHECK(info->version[0] != '\0');     /* fx_plugin_info("version") */
                INFO("%s %s by %s: %u effect(s)", g_slug[k], info->version, info->author,
                     (unsigned)s.nfx);
                /* the tooltip and the download name tell the same version */
                zip_version(g_slug[k], zv, sizeof zv);
                CHECK(same_version(info->version, zv));
                if (!same_version(info->version, zv))
                    fprintf(stderr, "    %s reports version %s, its README card (or "
                            "release.conf) says %s\n", g_slug[k], info->version, zv);
            }
            CHECK(lib != NULL);
            if (lib) {
                CHECK(pal_lib_sym(lib, FX_ABI_VERSION_NAME) != NULL);
                CHECK(pal_lib_sym(lib, FX_INFO_NAME) != NULL);
                pal_lib_close(lib);
            }
            for (uint32_t i = 0; i < s.nfx; i++) {
                const fx_effect *fx = s.fx[i];
                void *p = plg_params(fx);
                double t0 = pc_test_now(), worst;
                printf("  %-20s %s (%s)\n", g_slug[k], fx->id, fx->menu);
                fflush(stdout);
                CHECK(p != NULL);
                if (!p) continue;
                CHECK(fx_params_valid(fx, p));
                worst = plg_check_effect(fx, p, NULL);
                INFO("%s: slowest reference render %.3f s, all checks %.2f s", fx->id, worst,
                     pc_test_now() - t0);
                fx_params_free(p);
            }
        } else {
            fprintf(stderr, "    %s did not load\n", path);
        }
        plg_close(&s);
    }
}

/* ---- unload and load again ------------------------------------------------------------ */
static void render_first(const char *path, fx_img *dst, char *id, size_t cap)
{
    plg_set s;
    if (plg_open(&s, path) && s.nfx > 0u) {
        const fx_effect *fx = s.fx[0];
        fx_img src = plg_image(PLG_OBJECT, (fx->flags & FX_FLAG_MASK_ONLY) ? 1 : 4);
        fx_env env = plg_env(fxt_rect(0, 0, PLG_W, PLG_H));
        void *p = plg_params(fx);
        *dst = fxt_img_new(src.r, src.chans);
        snprintf(id, cap, "%s", fx->id);
        if (src.px && dst->px && p)
            CHECK(fx_run_sync(fx, p, &src, dst, &env, src.r, NULL) == PC_OK);
        fx_params_free(p);
        fxt_img_free(&src);
    }
    plg_close(&s);
}

static void t_reload(void)
{
    for (int k = 0; k < g_nslug; k++) {
        char path[1200], id1[FX_MAX_ID_LEN + 1u] = "", id2[FX_MAX_ID_LEN + 1u] = "";
        fx_img a = { NULL, 0, 0, { 0, 0, 0, 0 } }, b = a;
        plugin_path(path, sizeof path, g_slug[k]);
        render_first(path, &a, id1, sizeof id1);
        render_first(path, &b, id2, sizeof id2);
        CHECK(a.px != NULL && b.px != NULL);
        CHECK(strcmp(id1, id2) == 0);
        if (a.px && b.px) {
            CHECK(fxt_equal_in(&a, &b, a.r));
            if (!fxt_equal_in(&a, &b, a.r))
                fprintf(stderr, "    %s renders differently after a reload\n", id1);
        }
        fxt_img_free(&a);
        fxt_img_free(&b);
    }
}

/* ---- the documentation's example -------------------------------------------------- */
static void t_doc_example(void)
{
    plg_set s;
    if (plg_open(&s, PC_PLUGIN_EXAMPLE)) {
        const fx_effect *fx = plg_find(&s, "com.example.darken");
        const afx_plugin_info *info = fx ? afx_plugins_info(s.plugins, fx) : NULL;
        void *p = fx ? plg_params(fx) : NULL;
        CHECK(info && strcmp(info->author, "Your Name") == 0 &&
              strcmp(info->version, "1.0.0") == 0);
        if (fx && p) {
            fx_img src = plg_image(PLG_OBJECT, 4), dst = fxt_img_new(src.r, 4);
            fx_env env = plg_env(src.r);
            (void)plg_check_effect(fx, p, NULL);
            /* 50 %: every channel halved (rounded), alpha kept */
            if (src.px && dst.px &&
                fx_run_sync(fx, p, &src, &dst, &env, src.r, NULL) == PC_OK) {
                const uint8_t *a = fxt_at(&src, 101, 117), *b = fxt_at(&dst, 101, 117);
                CHECK(b[0] == (a[0] * 50 + 50) / 100 && b[1] == (a[1] * 50 + 50) / 100 &&
                      b[2] == (a[2] * 50 + 50) / 100 && b[3] == a[3] && a[3] == 255);
            } else {
                CHECK(!"darken did not run");
            }
            fxt_img_free(&src);
            fxt_img_free(&dst);
        }
        fx_params_free(p);
    }
    plg_close(&s);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    parse_slugs();
    RUN(t_discover);
    RUN(t_load_together);
    RUN(t_effects);
    RUN(t_reload);
    RUN(t_doc_example);
    return pc_test_finish();
}
