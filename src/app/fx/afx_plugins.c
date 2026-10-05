/* afx_plugins.c - the effect plugin loader (lane F, ADR-008 OD-9, X-04,
 * X-17, X-18) and the Plugin Errors list (Effects > Plugin Errors...,
 * Settings > Plugin Errors).
 *
 * A plugin is a shared library (pal_lib_suffix) that exports the fx_abi.h
 * entry point fx_entry. Optional paint.c exports (docs/app/EFFECTS.md):
 *   uint32_t    fx_abi_version(void)          ABI the plugin was built for;
 *                                             a different major version is
 *                                             rejected before fx_entry runs
 *   const char *fx_plugin_info(const char *key)  "author", "version"
 * Loading is all or nothing per file: fx_entry registers into a scratch
 * registry first (every descriptor validated with fx_effect_validate, its
 * struct size checked); only when the entry succeeds are the effects moved
 * into the app's registry (duplicate ids rejected one by one). A file that
 * adds nothing is unloaded again. Every problem becomes a Plugin Errors
 * entry; nothing a well-formed but wrong library does at registration time
 * can crash the editor. Code that misbehaves while running (wild pointers)
 * cannot be contained in-process, as in Paint.NET.
 *
 * Thread rules: main thread (pal_lib_* and the registry are main thread
 * only). The registration callback has no user pointer, so the loader
 * keeps its context in a static for the duration of one fx_entry call. */
#include "afx.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INFO_MAX 256u

typedef struct plib {
    char           *path;
    pal_lib        *lib;
    afx_plugin_info info;      /* strings owned below */
    char           *author, *version;
} plib;

typedef struct porigin {
    const fx_effect *fx;
    uint32_t         lib;
} porigin;

typedef struct perr {
    char *path, *msg;
} perr;

struct afx_plugins {
    plib    *libs;
    uint32_t nlibs, cap_libs;
    porigin *orig;
    uint32_t norig, cap_orig;
    perr    *errs;
    uint32_t nerrs, cap_errs;
    afx_plugin_error view;     /* returned by afx_plugins_error */
};

afx_plugins *afx_plugins_create(void)
{
    return (afx_plugins *)calloc(1u, sizeof(afx_plugins));
}

void afx_plugins_destroy(afx_plugins *p)
{
    if (!p) return;
    for (uint32_t i = p->nlibs; i > 0u; i--) {
        plib *l = &p->libs[i - 1u];
        pal_lib_close(l->lib);
        free(l->path);
        free(l->author);
        free(l->version);
    }
    for (uint32_t i = 0; i < p->nerrs; i++) {
        free(p->errs[i].path);
        free(p->errs[i].msg);
    }
    free(p->libs);
    free(p->orig);
    free(p->errs);
    free(p);
}

static bool grow(void **v, uint32_t *cap, uint32_t n, size_t elem)
{
    uint32_t nc;
    void *nv;
    size_t bytes;
    if (n < *cap) return true;
    nc = *cap ? *cap * 2u : 8u;
    if (!pc_mul_size((size_t)nc, elem, &bytes)) return false;
    nv = realloc(*v, bytes);
    if (!nv) return false;
    *v = nv;
    *cap = nc;
    return true;
}

static void add_error(afx_plugins *p, const char *path, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void add_error(afx_plugins *p, const char *path, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pal_log(PAL_LOG_WARN, "plugin %s: %s", path, msg);
    if (p->nerrs >= AFX_PLUGIN_MAX_ERRORS) return;
    if (!grow((void **)&p->errs, &p->cap_errs, p->nerrs, sizeof(perr))) return;
    p->errs[p->nerrs].path = app_strdup(path);
    p->errs[p->nerrs].msg = app_strdup(msg);
    if (!p->errs[p->nerrs].path || !p->errs[p->nerrs].msg) {
        free(p->errs[p->nerrs].path);
        free(p->errs[p->nerrs].msg);
        return;
    }
    p->nerrs++;
}

size_t afx_plugins_error_count(const afx_plugins *p) { return p ? p->nerrs : 0u; }
size_t afx_plugins_lib_count(const afx_plugins *p) { return p ? p->nlibs : 0u; }

const afx_plugin_error *afx_plugins_error(const afx_plugins *p, size_t i)
{
    afx_plugins *m = (afx_plugins *)(uintptr_t)p;
    if (!p || i >= p->nerrs) return NULL;
    m->view.path = p->errs[i].path;
    m->view.message = p->errs[i].msg;
    return &m->view;
}

const afx_plugin_info *afx_plugins_info(const afx_plugins *p, const fx_effect *fx)
{
    if (!p || !fx) return NULL;
    for (uint32_t i = 0; i < p->norig; i++)
        if (p->orig[i].fx == fx) return &p->libs[p->orig[i].lib].info;
    return NULL;
}

/* ---- registration context ------------------------------------------------------ */
typedef struct load_ctx {
    afx_plugins *p;
    const char  *path;
    fx_registry *scratch;
    uint32_t     rejected;
} load_ctx;

static load_ctx *g_load;      /* set for the duration of one fx_entry call */

/* A printable id for messages: the id when it is a plain identifier. */
static const char *safe_id(const fx_effect *fx, char *buf, size_t cap)
{
    size_t n = 0;
    if (!fx || !fx->id) return "?";
    while (n < FX_MAX_ID_LEN && fx->id[n]) {
        char c = fx->id[n];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return "?";
        n++;
    }
    if (n == 0u || n >= FX_MAX_ID_LEN || n + 1u > cap) return "?";
    memcpy(buf, fx->id, n);
    buf[n] = '\0';
    return buf;
}

static int plugin_reg(const fx_effect *fx)
{
    load_ctx *c = g_load;
    char why[200], id[FX_MAX_ID_LEN + 1u];
    pc_status st;
    if (!c) return (int)PC_ERR_STATE;
    if (!fx) {
        add_error(c->p, c->path, "registered a NULL effect");
        c->rejected++;
        return (int)PC_ERR_ARG;
    }
    if (fx->size < (uint32_t)sizeof(fx_effect)) {
        add_error(c->p, c->path, "effect struct size %u is smaller than ABI v%u needs (%u)",
                  (unsigned)fx->size, (unsigned)FX_ABI_VERSION, (unsigned)sizeof(fx_effect));
        c->rejected++;
        return (int)PC_ERR_ARG;
    }
    if (fx->size > AFX_PLUGIN_MAX_FX_SIZE) {
        add_error(c->p, c->path, "effect struct size %u is not plausible", (unsigned)fx->size);
        c->rejected++;
        return (int)PC_ERR_ARG;
    }
    if (fx_effect_validate(fx, why, sizeof why) != PC_OK) {
        add_error(c->p, c->path, "effect '%s' rejected: %s", safe_id(fx, id, sizeof id), why);
        c->rejected++;
        return (int)PC_ERR_ARG;
    }
    st = fx_registry_add(c->scratch, fx);
    if (st != PC_OK) {
        if (st == PC_ERR_STATE)
            add_error(c->p, c->path, "effect id '%s' is registered twice",
                      safe_id(fx, id, sizeof id));
        else
            add_error(c->p, c->path, "effect '%s' could not be registered (%s)",
                      safe_id(fx, id, sizeof id), pc_status_str(st));
        c->rejected++;
        return (int)st;
    }
    return 0;
}

static char *copy_info(const char *s)
{
    size_t n = 0;
    char *d;
    if (!s) return NULL;
    while (n < INFO_MAX && s[n]) n++;
    d = (char *)malloc(n + 1u);
    if (!d) return NULL;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)s[i] < 0x20u ? ' ' : s[i];
    d[n] = '\0';
    return d;
}

int afx_plugins_load_file(afx_plugins *p, fx_registry *r, const char *path)
{
    pal_lib *lib;
    void *sym;
    fx_entry_fn entry;
    load_ctx ctx;
    int ret, added = 0;
    uint32_t n;
    if (!p || !r || !path) return 0;
    if (g_load) {
        add_error(p, path, "plugin loading is not reentrant");
        return 0;
    }
    lib = pal_lib_open(path);
    if (!lib) {
        add_error(p, path, "not a loadable library for this system (or a library it needs "
                           "is missing)");
        return 0;
    }
    sym = pal_lib_sym(lib, "fx_abi_version");
    if (sym) {
        uint32_t (*ver_fn)(void);
        uint32_t ver;
        memcpy(&ver_fn, &sym, sizeof ver_fn);    /* object pointer -> function pointer */
        ver = ver_fn();
        if (ver != FX_ABI_VERSION) {
            add_error(p, path, "built for effect ABI v%u; this version of paint.c supports v%u",
                      (unsigned)ver, (unsigned)FX_ABI_VERSION);
            pal_lib_close(lib);
            return 0;
        }
    }
    sym = pal_lib_sym(lib, FX_ENTRY_NAME);
    if (!sym) {
        add_error(p, path, "no %s export: not a paint.c effect plugin", FX_ENTRY_NAME);
        pal_lib_close(lib);
        return 0;
    }
    memcpy(&entry, &sym, sizeof entry);
    memset(&ctx, 0, sizeof ctx);
    ctx.p = p;
    ctx.path = path;
    ctx.scratch = fx_registry_create();
    if (!ctx.scratch) {
        add_error(p, path, "out of memory");
        pal_lib_close(lib);
        return 0;
    }
    g_load = &ctx;
    ret = entry(fx_run_host(), plugin_reg);
    g_load = NULL;
    n = fx_registry_count(ctx.scratch);
    if (ret < 0) {
        add_error(p, path, "%s reported a failure (%d); none of its effects were loaded",
                  FX_ENTRY_NAME, ret);
        n = 0;
    } else if (n == 0u && ctx.rejected == 0u) {
        add_error(p, path, "the plugin registered no effects");
    }
    if (n > 0u && grow((void **)&p->libs, &p->cap_libs, p->nlibs, sizeof(plib))) {
        uint32_t li = p->nlibs;
        plib *l = &p->libs[li];
        memset(l, 0, sizeof *l);
        l->path = app_strdup(path);
        for (uint32_t i = 0; l->path && i < n; i++) {
            const fx_effect *fx = fx_registry_at(ctx.scratch, i);
            char id[FX_MAX_ID_LEN + 1u];
            pc_status st;
            if (!grow((void **)&p->orig, &p->cap_orig, p->norig, sizeof(porigin))) break;
            st = fx_registry_add(r, fx);
            if (st == PC_OK) {
                p->orig[p->norig].fx = fx;
                p->orig[p->norig].lib = li;
                p->norig++;
                added++;
            } else if (st == PC_ERR_STATE) {
                add_error(p, path, "effect id '%s' is already used by another effect",
                          safe_id(fx, id, sizeof id));
            } else {
                add_error(p, path, "effect '%s' could not be registered (%s)",
                          safe_id(fx, id, sizeof id), pc_status_str(st));
            }
        }
        if (added > 0) {
            const char *(*info_fn)(const char *);
            sym = pal_lib_sym(lib, "fx_plugin_info");
            if (sym) {
                memcpy(&info_fn, &sym, sizeof info_fn);
                l->author = copy_info(info_fn("author"));
                l->version = copy_info(info_fn("version"));
            }
            l->lib = lib;
            l->info.path = l->path;
            l->info.author = l->author ? l->author : "";
            l->info.version = l->version ? l->version : "";
            p->nlibs++;
            lib = NULL;
        } else {
            free(l->path);
        }
    }
    fx_registry_destroy(ctx.scratch);
    if (lib) pal_lib_close(lib);                 /* nothing of it is in use */
    return added;
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), k = strlen(suffix);
    if (n <= k) return false;
    for (size_t i = 0; i < k; i++)
        if (tolower((unsigned char)name[n - k + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

int afx_plugins_scan(afx_plugins *p, fx_registry *r, const char *dir)
{
    char **names = NULL;
    int n, added = 0;
    uint32_t seen = 0;
    const char *suffix = pal_lib_suffix();
    if (!p || !r || !dir || !*dir || !pal_is_dir(dir)) return 0;
    n = pal_list_dir(dir, NULL, &names);
    /* files of the folder first, then one level of subfolders (one plugin
     * per folder with its own dependencies is a common layout) */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n && seen < AFX_PLUGIN_MAX_FILES; i++) {
            char path[2048];
            bool is_dir;
            if (!names[i] || names[i][0] == '.') continue;
            pal_path_join(path, sizeof path, dir, names[i]);
            is_dir = pal_is_dir(path);
            if (pass == 0 && !is_dir && has_suffix(names[i], suffix)) {
                seen++;
                added += afx_plugins_load_file(p, r, path);
            } else if (pass == 1 && is_dir) {
                char **sub = NULL;
                int m = pal_list_dir(path, NULL, &sub);
                for (int k = 0; k < m && seen < AFX_PLUGIN_MAX_FILES; k++) {
                    char sp[2048];
                    if (!sub[k] || sub[k][0] == '.' || !has_suffix(sub[k], suffix)) continue;
                    pal_path_join(sp, sizeof sp, path, sub[k]);
                    if (pal_is_dir(sp)) continue;
                    seen++;
                    added += afx_plugins_load_file(p, r, sp);
                }
                pal_free_names(sub, m);
            }
        }
    }
    pal_free_names(names, n);
    return added;
}

/* ==== Plugin Errors UI ============================================================ */
typedef struct perr_ui { int32_t sel; } perr_ui;

static perr_ui *perr_state(app *a)
{
    perr_ui *u = (perr_ui *)app_ext_get(a, "afx.perr");
    if (!u) {
        u = (perr_ui *)calloc(1u, sizeof *u);
        if (u && !app_ext_set(a, "afx.perr", u, free)) {
            free(u);
            u = NULL;
        }
    }
    return u;
}

static void perr_row(ui_ctx *ui, void *ud, int32_t index, ui_rect row, uint32_t state)
{
    const afx_plugins *p = (const afx_plugins *)ud;
    const afx_plugin_error *e = afx_plugins_error(p, (size_t)index);
    const ui_palette *pal = ui_pal(ui);
    const char *name;
    if (!e) return;
    name = pal_path_basename(e->path);
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                     ui_rect_make(row.x + ui_px(ui, 8.0f), row.y, row.w - ui_px(ui, 12.0f), row.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                     (state & UI_ROW_SELECTED) ? pal->text : pal->text, name, strlen(name));
}

void afx_plugin_errors_ui(app *a, float height_dip)
{
    ui_ctx *ui = a->ui;
    afx_plugins *p = afx_app_plugins(a);
    perr_ui *u = perr_state(a);
    int32_t n = (int32_t)afx_plugins_error_count(p);
    ui_rect r;
    if (!u) return;
    if (n == 0) {
        ui_text_wrapped(ui, "No problems were found while loading plugins.", UI_LABEL_DIM);
        return;
    }
    if (u->sel < 0 || u->sel >= n) u->sel = 0;
    r = ui_layout_next(ui, 0, ui_px(ui, height_dip * 0.5f));
    (void)ui_list(ui, "##perrlist", r, n, 24.0f, &u->sel, 0u, perr_row, p);
    ui_layout_space(ui, 6.0f);
    {
        const afx_plugin_error *e = afx_plugins_error(p, (size_t)u->sel);
        if (e) {
            ui_label_ex(ui, "File", UI_LABEL_BOLD);
            ui_text_wrapped(ui, e->path, UI_LABEL_DIM);
            ui_layout_space(ui, 4.0f);
            ui_label_ex(ui, "Problem", UI_LABEL_BOLD);
            ui_text_wrapped(ui, e->message, 0u);
        }
    }
}

static bool perr_frame(app *a, void *st)
{
    ui_ctx *ui = a->ui;
    uint32_t r;
    (void)st;
    ui_dialog_begin(ui, "Plugin Errors##afxperr", 560.0f, 0.0f);
    afx_plugin_errors_ui(a, 320.0f);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    return r == 0u;
}

void afx_plugin_errors_dialog(app *a)
{
    (void)app_dialog_push(a, perr_frame, NULL, NULL);
}
