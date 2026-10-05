/* help_pages.c - lane UIB (wave 4 items 4 to 6): the bundled user guide's
 * pages, search page and files, the help folder and opening a page (see
 * help.h). */
#include "app_internal.h"
#include "cli_set.h"
#include "edit/m_ui.h"
#include "help/help.h"
#include "pc/pc_blend.h"
#include "pc/pc_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PC_PROJECT_PUBLIC
#define PC_PROJECT_PUBLIC 0
#endif
#define HELP_PROJECT_URL "https://github.com/vancevoj/paint.c"

/* ---- embedded sources (cmake/PcEmbed.cmake, src/app/CMakeLists.txt) ------------------ */
#define APP_HELP_PAGE(id, name)                                                          \
    extern const unsigned char pc_help_md_##id[];                                       \
    extern const size_t pc_help_md_##id##_size;
#include "app_help_list.inc"
#undef APP_HELP_PAGE

extern const unsigned char pc_help_fade_plugin_c[];
extern const size_t pc_help_fade_plugin_c_size;
extern const unsigned char pc_help_fx_abi_h[];
extern const size_t pc_help_fx_abi_h_size;
extern const unsigned char pc_help_fx_util_h[];
extern const size_t pc_help_fx_util_h_size;
extern const unsigned char pc_help_fx_widgets_h[];
extern const size_t pc_help_fx_widgets_h_size;

typedef struct help_src {
    const char          *name;
    const unsigned char *data;
    const size_t        *size;
} help_src;

static const help_src k_src[] = {
#define APP_HELP_PAGE(id, name) { name, pc_help_md_##id, &pc_help_md_##id##_size },
#include "app_help_list.inc"
#undef APP_HELP_PAGE
    { NULL, NULL, NULL }
};

#define NSRC ((int)(sizeof k_src / sizeof k_src[0]) - 1)

/* Navigation order; pages not listed follow in name order. */
static const char *const k_order[] = { "index",        "getting-started", "tools",
                                       "layers",       "adjustments-effects",
                                       "file-formats", "keyboard",        "plugins",
                                       "tutorials",    "troubleshooting" };

static int order_of(const char *name)
{
    for (int i = 0; i < (int)(sizeof k_order / sizeof k_order[0]); i++)
        if (strcmp(k_order[i], name) == 0) return i;
    return 1000;
}

/* k_src indices in navigation order (computed once, main thread). */
static int g_nav[64];
static int g_nnav = -1;

static void nav_init(void)
{
    if (g_nnav >= 0) return;
    g_nnav = 0;
    for (int i = 0; i < NSRC && g_nnav < (int)(sizeof g_nav / sizeof g_nav[0]); i++)
        g_nav[g_nnav++] = i;
    for (int i = 1; i < g_nnav; i++) {          /* insertion sort, a dozen pages */
        int v = g_nav[i], j = i - 1;
        while (j >= 0) {
            int oa = order_of(k_src[g_nav[j]].name), ob = order_of(k_src[v].name);
            if (oa < ob || (oa == ob && strcmp(k_src[g_nav[j]].name, k_src[v].name) <= 0))
                break;
            g_nav[j + 1] = g_nav[j];
            j--;
        }
        g_nav[j + 1] = v;
    }
}

int app_help_page_count(void)
{
    nav_init();
    return g_nnav;
}

const char *app_help_page_name(int i)
{
    nav_init();
    return i >= 0 && i < g_nnav ? k_src[g_nav[i]].name : NULL;
}

/* ---- directives ---------------------------------------------------------------------- */
static bool is_mac(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

typedef struct dctx {
    app *a;
} dctx;

static void put_row2(help_buf *h, const char *c1, const char *c2)
{
    help_buf_puts(h, "<tr><td>");
    help_buf_escs(h, c1);
    help_buf_puts(h, "</td><td>");
    help_buf_escs(h, c2);
    help_buf_puts(h, "</td></tr>\n");
}

static void table_open(help_buf *h, const char *c1, const char *c2, const char *c3)
{
    help_buf_puts(h, "<table>\n<thead><tr><th>");
    help_buf_escs(h, c1);
    help_buf_puts(h, "</th><th>");
    help_buf_escs(h, c2);
    if (c3) {
        help_buf_puts(h, "</th><th>");
        help_buf_escs(h, c3);
    }
    help_buf_puts(h, "</th></tr></thead>\n<tbody>\n");
}

static void table_close(help_buf *h) { help_buf_puts(h, "</tbody></table>\n"); }

/* A command's label without access key marks and a trailing "...". */
static void clean_label(const char *in, char *out, size_t cap)
{
    size_t k = 0, n;
    for (const char *p = in ? in : ""; *p && k + 1u < cap; p++) {
        if (*p == '&' && p[1] != '&') continue;
        if (*p == '&') p++;
        out[k++] = *p;
    }
    out[k] = '\0';
    n = strlen(out);
    if (n >= 3u && strcmp(out + n - 3u, "...") == 0) out[n - 3u] = '\0';
}

static void keys_text(const app_cmd *c, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = '\0';
    for (int i = 0; i < c->nkeys && k + 1u < cap; i++) {
        char one[64];
        app_key_format(c->keys[i], is_mac(), one, sizeof one);
        k += (size_t)snprintf(out + k, cap - k, "%s%s", i ? ", " : "", one);
        if (k >= cap) k = cap - 1u;
    }
}

typedef struct key_group {
    const char *title;
    const char *prefix[3];
} key_group;

static const key_group k_groups[] = {
    { "File", { "file.", NULL, NULL } },
    { "Edit", { "edit.", NULL, NULL } },
    { "View", { "view.", NULL, NULL } },
    { "Image", { "image.", NULL, NULL } },
    { "Layers", { "layers.", NULL, NULL } },
    { "Adjustments", { "adjust.", NULL, NULL } },
    { "Effects", { "effects.", NULL, NULL } },
    { "Switching images", { "docs.", NULL, NULL } },
    { "Tools and colors", { "tool.", "colors.", NULL } },
    { "Windows, settings and help", { "window.", "app.", "help." } },
};

static int group_of(const char *id)
{
    for (int g = 0; g < (int)(sizeof k_groups / sizeof k_groups[0]); g++)
        for (int p = 0; p < 3 && k_groups[g].prefix[p]; p++)
            if (strncmp(id, k_groups[g].prefix[p], strlen(k_groups[g].prefix[p])) == 0) return g;
    return -1;
}

static void dir_shortcuts(app *a, help_buf *h)
{
    int ng = (int)(sizeof k_groups / sizeof k_groups[0]);
    for (int g = -1; g < ng; g++) {
        bool any = false;
        for (int32_t i = 0; i < app_cmd_count(a); i++) {
            const app_cmd *c = app_cmd_at(a, i);
            char label[256], keys[256];
            if (!c || c->nkeys <= 0 || group_of(c->id) != g) continue;
            if (!any) {
                help_buf_puts(h, "<h3>");
                help_buf_escs(h, g >= 0 ? k_groups[g].title : "Other");
                help_buf_puts(h, "</h3>\n");
                table_open(h, "Command", "Keys", NULL);
                any = true;
            }
            clean_label(c->label, label, sizeof label);
            keys_text(c, keys, sizeof keys);
            put_row2(h, label, keys);
        }
        if (any) table_close(h);
    }
}

static void dir_tool_keys(app *a, help_buf *h)
{
    table_open(h, "Tool", "Key", "Presses");
    for (int32_t i = 0; i < a->ntools; i++) {
        const app_tool *t = a->tools[i];
        char key[8], nth[16];
        int rank = 1;
        if (!t || !t->name) continue;
        for (int32_t j = 0; j < i; j++)
            if (t->letter && a->tools[j]->letter == t->letter) rank++;
        snprintf(key, sizeof key, "%c", t->letter ? t->letter : ' ');
        snprintf(nth, sizeof nth, "%d", rank);
        help_buf_puts(h, "<tr><td>");
        help_buf_escs(h, t->name);
        help_buf_puts(h, "</td><td>");
        help_buf_escs(h, t->letter ? key : "");
        help_buf_puts(h, "</td><td>");
        help_buf_escs(h, t->letter ? nth : "");
        help_buf_puts(h, "</td></tr>\n");
    }
    table_close(h);
}

/* menu "Adjustments/Name" or "Effects/Group/Name" */
static void dir_adjustments(app *a, help_buf *h)
{
    table_open(h, "Adjustment", "Keys", NULL);
    for (uint32_t i = 0; a->fx && i < fx_registry_count(a->fx); i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        char id[300], keys[128];
        const app_cmd *c;
        if (!fx || !fx->menu || strncmp(fx->menu, "Adjustments/", 12) != 0) continue;
        snprintf(id, sizeof id, "adjust.%s", fx->id);
        c = app_cmd_find(a, id);
        keys[0] = '\0';
        if (c) keys_text(c, keys, sizeof keys);
        put_row2(h, fx->menu + 12, keys);
    }
    table_close(h);
}

static void dir_effects(app *a, help_buf *h)
{
    char group[128];
    bool open = false, first = true;
    group[0] = '\0';
    table_open(h, "Submenu", "Effects", NULL);
    for (uint32_t i = 0; a->fx && i < fx_registry_count(a->fx); i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        const char *rest, *slash;
        size_t gl;
        if (!fx || !fx->menu || strncmp(fx->menu, "Effects/", 8) != 0) continue;
        rest = fx->menu + 8;
        slash = strrchr(rest, '/');
        gl = slash ? (size_t)(slash - rest) : 0u;
        if (gl >= sizeof group) gl = sizeof group - 1u;
        if (!open || strncmp(group, rest, gl) != 0 || group[gl] != '\0') {
            if (open) help_buf_puts(h, "</td></tr>\n");
            memcpy(group, rest, gl);
            group[gl] = '\0';
            help_buf_puts(h, "<tr><td>");
            help_buf_escs(h, gl ? group : "(top level)");
            help_buf_puts(h, "</td><td>");
            open = true;
            first = true;
        }
        if (!first) help_buf_puts(h, ", ");
        help_buf_escs(h, slash ? slash + 1 : rest);
        first = false;
    }
    if (open) help_buf_puts(h, "</td></tr>\n");
    table_close(h);
}

static void dir_formats(help_buf *h)
{
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    help_buf_puts(h, "<table>\n<thead><tr><th>Format</th><th>Extensions</th><th>Open</th>"
                     "<th>Save</th><th>Keeps layers</th></tr></thead>\n<tbody>\n");
    for (size_t i = 0; list && i < n; i++) {
        const pc_codec *c = list[i];
        char ext[256];
        size_t k = 0;
        if (!c || !c->name) continue;
        ext[0] = '\0';
        for (const char *p = c->exts ? c->exts : ""; *p && k + 4u < sizeof ext;) {
            const char *e = strchr(p, ';');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l + k + 4u >= sizeof ext) break;
            k += (size_t)snprintf(ext + k, sizeof ext - k, "%s.%.*s", k ? ", " : "", (int)l, p);
            p += l + (e ? 1u : 0u);
        }
        help_buf_puts(h, "<tr><td>");
        help_buf_escs(h, c->name);
        help_buf_puts(h, "</td><td>");
        help_buf_escs(h, ext);
        help_buf_puts(h, "</td><td>");
        help_buf_puts(h, (c->flags & PC_CODEC_LOAD) ? "Yes" : "No");
        help_buf_puts(h, "</td><td>");
        help_buf_puts(h, (c->flags & PC_CODEC_SAVE) ? "Yes" : "No");
        help_buf_puts(h, "</td><td>");
        help_buf_puts(h, (c->flags & PC_CODEC_LAYERED) ? "Yes" : "No");
        help_buf_puts(h, "</td></tr>\n");
    }
    table_close(h);
}

/* pc_blend_name gives "ColorBurn"; the menus show "Color Burn". */
static void dir_blend_modes(help_buf *h)
{
    help_buf_puts(h, "<ul class=\"cols\">\n");
    for (int m = 0; m < (int)PC_BLEND_COUNT; m++) {
        const char *name = pc_blend_name((pc_blend_mode)m);
        char shown[64];
        size_t k = 0;
        if (!name) continue;
        for (const char *p = name; *p && k + 2u < sizeof shown; p++) {
            if (p != name && *p >= 'A' && *p <= 'Z' && p[-1] >= 'a' && p[-1] <= 'z')
                shown[k++] = ' ';
            shown[k++] = *p;
        }
        shown[k] = '\0';
        help_buf_puts(h, "<li>");
        help_buf_escs(h, shown);
        help_buf_puts(h, "</li>\n");
    }
    help_buf_puts(h, "</ul>\n");
}

static void dir_getting_help(help_buf *h)
{
#if PC_PROJECT_PUBLIC
    help_buf_puts(h, "<p><strong>Help &gt; Forum</strong> opens the project's issue list, where "
                     "you can ask questions and see what others reported. <strong>Help &gt; "
                     "Send Feedback or Bug Report</strong> starts a new report with the "
                     "diagnostics of this computer filled in.</p>\n");
#else
    help_buf_puts(h, "<p><strong>Help &gt; Send Feedback or Bug Report</strong> starts a new "
                     "report in the project's repository with the diagnostics of this "
                     "computer filled in. The repository is not public yet, so only people "
                     "with access can open it. For the same reason paint.c has no community "
                     "forum yet: <strong>Help &gt; Forum</strong> is hidden until there is a "
                     "public place to discuss paint.c.</p>\n");
#endif
}

static void dir_plugin_example(help_buf *h)
{
    help_buf_puts(h, "<pre><code class=\"language-c\">");
    help_buf_esc(h, (const char *)pc_help_fade_plugin_c, pc_help_fade_plugin_c_size);
    help_buf_puts(h, "</code></pre>\n");
}

static void plugin_dir(char *out, size_t cap)
{
    const char *d = pal_dir(PAL_DIR_DATA);
    if (d) pal_path_join(out, cap, d, "plugins");
    else app_copy_str(out, cap, "(no per-user folder on this system)");
}

static bool directive(void *ud, const char *name, bool block, help_buf *out)
{
    dctx *c = (dctx *)ud;
    app *a = c->a;
    char path[1100];
    if (block) {
        if (strcmp(name, "shortcuts") == 0) dir_shortcuts(a, out);
        else if (strcmp(name, "tool_keys") == 0) dir_tool_keys(a, out);
        else if (strcmp(name, "adjustments") == 0) dir_adjustments(a, out);
        else if (strcmp(name, "effects") == 0) dir_effects(a, out);
        else if (strcmp(name, "formats") == 0) dir_formats(out);
        else if (strcmp(name, "blend_modes") == 0) dir_blend_modes(out);
        else if (strcmp(name, "plugin_example") == 0) dir_plugin_example(out);
        else if (strcmp(name, "getting_help") == 0) dir_getting_help(out);
        else return false;
        return true;
    }
    if (strcmp(name, "version") == 0) {
        help_buf_puts(out, APP_VERSION);
    } else if (strcmp(name, "ctrl") == 0) {
        help_buf_puts(out, is_mac() ? "Cmd" : "Ctrl");
    } else if (strcmp(name, "alt") == 0) {
        help_buf_puts(out, is_mac() ? "Option" : "Alt");
    } else if (strcmp(name, "plugin_dir") == 0) {
        plugin_dir(path, sizeof path);
        help_buf_puts(out, path);
    } else if (strcmp(name, "exe_plugin_dir") == 0) {
        const char *e = pal_dir(PAL_DIR_EXE);
        if (e) pal_path_join(path, sizeof path, e, "plugins");
        else app_copy_str(path, sizeof path, "plugins");
        help_buf_puts(out, path);
    } else if (strcmp(name, "config_dir") == 0) {
        const char *d = app_cli_config_dir(a);
        help_buf_puts(out, d ? d : "(none: this session saves no settings)");
    } else if (strcmp(name, "help_dir") == 0) {
        if (!app_help_dir(a, path, sizeof path)) app_copy_str(path, sizeof path, "(none)");
        help_buf_puts(out, path);
    } else if (strcmp(name, "lib_suffix") == 0) {
        help_buf_puts(out, pal_lib_suffix());
    } else {
        return false;
    }
    return true;
}

/* ---- page frame ------------------------------------------------------------------------ */
static const char k_css[] =
    ":root{--bg:#ffffff;--fg:#1d2329;--dim:#5b6570;--line:#d9dee3;--side:#f4f6f8;"
    "--accent:#1f6fd1;--code:#f1f3f5}"
    "@media (prefers-color-scheme:dark){:root{--bg:#1c1f23;--fg:#e4e7ea;--dim:#9aa4ae;"
    "--line:#353b42;--side:#24282d;--accent:#6aa8ff;--code:#2a2f35}}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,-apple-system,"
    "'Segoe UI',Roboto,'Noto Sans',sans-serif}"
    "a{color:var(--accent)}"
    "header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;justify-content:space-between;"
    "padding:10px 20px;border-bottom:1px solid var(--line)}"
    ".brand{font-weight:600;font-size:18px;text-decoration:none;color:var(--fg)}"
    "header form{display:flex;gap:6px}"
    "input[type=search]{font:inherit;padding:4px 8px;border:1px solid var(--line);"
    "border-radius:6px;background:var(--bg);color:var(--fg);min-width:200px}"
    "button{font:inherit;padding:4px 12px;border:1px solid var(--line);border-radius:6px;"
    "background:var(--side);color:var(--fg)}"
    ".wrap{display:flex;align-items:flex-start;max-width:1180px;margin:0 auto}"
    "nav{flex:0 0 230px;padding:16px 12px;position:sticky;top:0}"
    "nav ul{list-style:none;margin:0;padding:0}"
    "nav a{display:block;padding:5px 10px;border-radius:6px;text-decoration:none;color:var(--fg)}"
    "nav a:hover{background:var(--side)}"
    "nav a[aria-current]{background:var(--side);font-weight:600;color:var(--accent)}"
    "main{flex:1 1 auto;min-width:0;padding:8px 28px 40px;max-width:860px}"
    "h1{font-size:30px;margin:18px 0 12px}h2{margin-top:32px}h3{margin-top:24px}"
    "table{border-collapse:collapse;margin:12px 0;width:100%;display:block;overflow-x:auto}"
    "th,td{border:1px solid var(--line);padding:5px 10px;text-align:left;vertical-align:top}"
    "th{background:var(--side)}"
    "code{font-family:ui-monospace,'Cascadia Mono',Consolas,'DejaVu Sans Mono',monospace;"
    "font-size:.92em;background:var(--code);padding:1px 4px;border-radius:4px}"
    "pre{background:var(--code);padding:12px 14px;border-radius:8px;overflow-x:auto}"
    "pre code{padding:0;background:none}"
    "blockquote{margin:12px 0;padding:6px 16px;border-left:4px solid var(--accent);"
    "background:var(--side)}"
    "ul.cols{columns:3 180px}"
    ".page{display:none}.page.current{display:block}"
    "#results li{margin:8px 0}.snip{color:var(--dim);font-size:14px}"
    "footer{color:var(--dim);font-size:14px;text-align:center;padding:24px;"
    "border-top:1px solid var(--line)}"
    "@media (max-width:760px){.wrap{display:block}nav{position:static;padding:8px 16px}"
    "nav ul{display:flex;flex-wrap:wrap;gap:4px}main{padding:0 16px 32px}}";

/* ---- the guide document ------------------------------------------------------------------
 * Every file of the guide holds the whole guide: one <section class="page"> per source page
 * plus the search page, with the file's own page marked "current" (the only one shown; no
 * script is needed for that). Links to other pages stay plain links to their files, and the
 * script switches between the sections in place instead, so the guide also works when a
 * sandbox (a Flatpak document portal, for example) hands the browser only the one file
 * that was opened. Heading ids carry their page name (help_md_render_page), so every id is
 * unique in the document. */
static void page_head(help_buf *h, const char *title, const char *current, const help_doc *docs,
                      const int *nav, int nnav)
{
    help_buf_puts(h, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                     "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                     "<title>");
    help_buf_escs(h, title);
    help_buf_puts(h, " | paint.c Help</title>\n<style>");
    help_buf_puts(h, k_css);
    help_buf_puts(h, "</style>\n</head>\n<body>\n<header><a class=\"brand\" href=\"index.html\">"
                     "paint.c Help</a><form id=\"sform\" action=\"search.html\" method=\"get\" "
                     "role=\"search\"><input type=\"search\" name=\"q\" id=\"q\" "
                     "placeholder=\"Search the guide\" aria-label=\"Search the guide\">"
                     "<button type=\"submit\">Search</button></form></header>\n"
                     "<div class=\"wrap\">\n<nav aria-label=\"Pages\"><ul>\n");
    for (int i = 0; i < nnav; i++) {
        const char *name = k_src[nav[i]].name;
        help_buf_puts(h, "<li><a href=\"");
        help_buf_escs(h, name);
        help_buf_puts(h, ".html\" data-page=\"");
        help_buf_escs(h, name);
        help_buf_puts(h, "\"");
        if (current && strcmp(current, name) == 0) help_buf_puts(h, " aria-current=\"page\"");
        help_buf_puts(h, ">");
        help_buf_escs(h, docs[i].title[0] ? docs[i].title : name);
        help_buf_puts(h, "</a></li>\n");
    }
    help_buf_puts(h, "</ul></nav>\n<main>\n");
}

static void section_open(help_buf *h, const char *id, const char *title, bool current)
{
    help_buf_puts(h, current ? "<section class=\"page current\" id=\""
                             : "<section class=\"page\" id=\"");
    help_buf_escs(h, id);
    help_buf_puts(h, "\" data-title=\"");
    help_buf_escs(h, title);
    help_buf_puts(h, "\">\n");
}

/* A JavaScript string literal (JSON compatible, safe inside <script>). */
static void js_str(help_buf *h, const char *s, size_t n)
{
    help_buf_putc(h, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char esc[8];
        if (ch == '"' || ch == '\\') {
            help_buf_putc(h, '\\');
            help_buf_putc(h, (char)ch);
        } else if (ch < 0x20u || ch == '<' || ch == '>' || ch == '&') {
            snprintf(esc, sizeof esc, "\\u%04x", (unsigned)ch);
            help_buf_puts(h, esc);
        } else if (ch == 0xE2u && i + 2u < n && (unsigned char)s[i + 1u] == 0x80u &&
                   ((unsigned char)s[i + 2u] == 0xA8u || (unsigned char)s[i + 2u] == 0xA9u)) {
            help_buf_puts(h, (unsigned char)s[i + 2u] == 0xA8u ? "\\u2028" : "\\u2029");
            i += 2u;
        } else {
            help_buf_putc(h, (char)ch);
        }
    }
    help_buf_putc(h, '"');
}

/* The search index: one entry per section of every page. */
static void build_index(help_buf *h, const help_doc *docs, const int *nav, int nnav)
{
    help_buf_puts(h, "var IDX=[\n");
    for (int i = 0; i < nnav; i++) {
        const help_doc *d = &docs[i];
        for (int k = 0; k < d->nsec; k++) {
            const help_section *sec = &d->sec[k];
            const char *anchor = sec->anchor[0] ? sec->anchor : k_src[nav[i]].name;
            const char *title = sec->title[0] ? sec->title : d->title;
            if (sec->text.n == 0u && !sec->title[0]) continue;
            help_buf_puts(h, "{a:");
            js_str(h, anchor, strlen(anchor));
            help_buf_puts(h, ",g:");
            js_str(h, d->title, strlen(d->title));
            help_buf_puts(h, ",t:");
            js_str(h, title, strlen(title));
            help_buf_puts(h, ",x:");
            js_str(h, sec->text.s ? sec->text.s : "", sec->text.n);
            help_buf_puts(h, "},\n");
        }
    }
    help_buf_puts(h, "];\n");
}

static const char k_js[] =
    "(function(){\n"
    "var pages=document.querySelectorAll('section.page'),"
    "links=document.querySelectorAll('nav a[data-page]'),box=document.getElementById('q'),"
    "out=document.getElementById('results'),count=document.getElementById('count');\n"
    "function show(id){var el=id?document.getElementById(id):null,i;"
    "var sec=el&&el.closest?el.closest('section.page'):null;if(!sec)return false;\n"
    "for(i=0;i<pages.length;i++)pages[i].classList.toggle('current',pages[i]===sec);\n"
    "for(i=0;i<links.length;i++){if(links[i].getAttribute('data-page')===sec.id)"
    "links[i].setAttribute('aria-current','page');else links[i].removeAttribute('aria-current');}\n"
    "document.title=sec.getAttribute('data-title')+' | paint.c Help';\n"
    "if(el===sec)window.scrollTo(0,0);else el.scrollIntoView();return true;}\n"
    "function target(a){var h=a.getAttribute('href')||'';if(/^[a-z][a-z0-9+.-]*:/i.test(h))"
    "return null;var m=/^([^#?]*?)(\\.html)?(#(.*))?$/.exec(h);if(!m)return null;"
    "if(m[1]&&!m[2])return null;var id=m[4]||m[1];"
    "return id&&document.getElementById(id)?id:null;}\n"
    "document.addEventListener('click',function(e){var a=e.target&&e.target.closest?"
    "e.target.closest('a'):null;if(!a)return;var id=target(a);if(!id)return;"
    "e.preventDefault();if(location.hash==='#'+id)show(id);else location.hash=id;});\n"
    "window.addEventListener('hashchange',function(){"
    "show(decodeURIComponent(location.hash.slice(1)));});\n"
    "function low(s){return s.toLowerCase();}\n"
    "function snip(x,w){var i=low(x).indexOf(w);if(i<0)return x.slice(0,160);"
    "var s=Math.max(0,i-60);return (s?'...':'')+x.slice(s,s+180)+(s+180<x.length?'...':'');}\n"
    "function run(q){var words=low(q).split(/\\s+/).filter(function(w){return w;});"
    "out.innerHTML='';if(!words.length){count.textContent='Type words to search for.';return;}"
    "var hits=[];\n"
    "IDX.forEach(function(e){var t=low(e.t),hay=t+' '+low(e.x),score=0;"
    "var ok=words.every(function(w){if(hay.indexOf(w)<0)return false;"
    "score+=t.indexOf(w)>=0?10:1;return true;});if(ok)hits.push({e:e,s:score});});\n"
    "hits.sort(function(a,b){return b.s-a.s;});\n"
    "hits.slice(0,60).forEach(function(h){var li=document.createElement('li'),"
    "a=document.createElement('a');a.href='#'+h.e.a;"
    "a.textContent=h.e.g+(h.e.t&&h.e.t!==h.e.g?': '+h.e.t:'');li.appendChild(a);"
    "var d=document.createElement('div');d.className='snip';d.textContent=snip(h.e.x,words[0]);"
    "li.appendChild(d);out.appendChild(li);});\n"
    "count.textContent=hits.length?hits.length+(hits.length===1?' result':' results'):"
    "'Nothing found. Try fewer or shorter words.';}\n"
    "document.getElementById('sform').addEventListener('submit',function(e){"
    "e.preventDefault();run(box.value);if(location.hash==='#search')show('search');"
    "else location.hash='search';});\n"
    "box.addEventListener('input',function(){run(box.value);if(box.value)show('search');});\n"
    "var q='';try{q=new URLSearchParams(location.search).get('q')||'';}catch(e){}\n"
    "if(q){box.value=q;show('search');}run(box.value);\n"
    "if(location.hash.length>1)show(decodeURIComponent(location.hash.slice(1)));\n"
    "if(document.getElementById('search').classList.contains('current'))box.focus();\n"
    "})();\n";

/* One file of the guide with page current (a page name or "search"). */
static void build_file(help_buf *h, const char *current, const help_doc *docs, const int *nav,
                       int nnav, const help_buf *idx)
{
    const char *title = "Search";
    for (int i = 0; i < nnav; i++)
        if (strcmp(k_src[nav[i]].name, current) == 0) title = docs[i].title;
    page_head(h, title, current, docs, nav, nnav);
    for (int i = 0; i < nnav; i++) {
        section_open(h, k_src[nav[i]].name, docs[i].title,
                     strcmp(k_src[nav[i]].name, current) == 0);
        help_buf_put(h, docs[i].html.s, docs[i].html.n);
        help_buf_puts(h, "</section>\n");
    }
    section_open(h, "search", "Search", strcmp(current, "search") == 0);
    help_buf_puts(h, "<h1 id=\"search-search\">Search</h1>\n<p id=\"count\"></p>\n"
                     "<ol id=\"results\"></ol>\n<noscript><p>Searching needs JavaScript, which "
                     "is turned off in this browser. Use the list of pages instead.</p>"
                     "</noscript>\n</section>\n<script>\n");
    help_buf_put(h, idx->s, idx->n);
    help_buf_puts(h, k_js);
    help_buf_puts(h, "</script>\n</main>\n</div>\n<footer>paint.c " APP_VERSION ". This guide is "
                     "part of the program and was written for this computer; it needs no "
                     "network connection.</footer>\n</body>\n</html>\n");
}

/* ---- the set of files ------------------------------------------------------------------ */
static help_buf *add_file(app_help_set *s, const char *name)
{
    app_help_file *f;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        size_t bytes;
        app_help_file *nf;
        if (!pc_mul_size((size_t)nc, sizeof *nf, &bytes)) return NULL;
        nf = (app_help_file *)realloc(s->f, bytes);
        if (!nf) return NULL;
        s->f = nf;
        s->cap = nc;
    }
    f = &s->f[s->n++];
    memset(f, 0, sizeof *f);
    app_copy_str(f->name, sizeof f->name, name);
    return &f->data;
}

void app_help_set_free(app_help_set *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) help_buf_free(&s->f[i].data);
    free(s->f);
    memset(s, 0, sizeof *s);
}

bool app_help_build(app *a, app_help_set *out)
{
    help_doc *docs;
    help_buf idx;
    dctx ctx;
    bool ok = true;
    if (!a || !out) return false;
    memset(out, 0, sizeof *out);
    memset(&idx, 0, sizeof idx);
    nav_init();
    if (g_nnav <= 0) return false;
    docs = (help_doc *)calloc((size_t)g_nnav, sizeof *docs);
    if (!docs) return false;
    ctx.a = a;
    for (int i = 0; i < g_nnav; i++) {
        const help_src *src = &k_src[g_nav[i]];
        if (!help_md_render_page((const char *)src->data, *src->size, src->name, directive, &ctx,
                                 &docs[i]))
            ok = false;
        if (!docs[i].title[0]) app_copy_str(docs[i].title, sizeof docs[i].title, src->name);
    }
    build_index(&idx, docs, g_nav, g_nnav);
    if (idx.failed) ok = false;
    for (int i = 0; i <= g_nnav && ok; i++) {
        const char *page = i < g_nnav ? k_src[g_nav[i]].name : "search";
        char name[96];
        help_buf *h;
        snprintf(name, sizeof name, "%s.html", page);
        h = add_file(out, name);
        if (!h) {
            ok = false;
            break;
        }
        build_file(h, page, docs, g_nav, g_nnav, &idx);
        if (h->failed) ok = false;
    }
    if (ok) {
        static const struct {
            const char          *name;
            const unsigned char *data;
            const size_t        *n;
        } x[] = {
            { "fade_plugin.c", pc_help_fade_plugin_c, &pc_help_fade_plugin_c_size },
            { "fx_abi.h", pc_help_fx_abi_h, &pc_help_fx_abi_h_size },
            { "fx_util.h", pc_help_fx_util_h, &pc_help_fx_util_h_size },
            { "fx_widgets.h", pc_help_fx_widgets_h, &pc_help_fx_widgets_h_size },
        };
        for (size_t i = 0; i < sizeof x / sizeof x[0] && ok; i++) {
            help_buf *h = add_file(out, x[i].name);
            if (h) help_buf_put(h, (const char *)x[i].data, *x[i].n);
            ok = h && !h->failed;
        }
    }
    help_buf_free(&idx);
    for (int i = 0; i < g_nnav; i++) help_doc_free(&docs[i]);
    free(docs);
    return ok;
}

bool app_help_write(const app_help_set *s, const char *dir)
{
    bool ok = true;
    if (!s || !dir || !*dir) return false;
    if (!pal_is_dir(dir) && !pal_mkdirs(dir)) return false;
    for (int i = 0; i < s->n; i++) {
        const app_help_file *f = &s->f[i];
        char path[1200];
        uint8_t *old = NULL;
        size_t oldn = 0;
        pal_path_join(path, sizeof path, dir, f->name);
        if (pal_read_file(path, (uint64_t)f->data.n + 1u, &old, &oldn) == PC_OK &&
            oldn == f->data.n && (oldn == 0u || memcmp(old, f->data.s, oldn) == 0)) {
            free(old);
            continue;                                  /* already up to date */
        }
        free(old);
        if (pal_write_file_atomic(path, f->data.s ? f->data.s : "", f->data.n) != PC_OK) {
            pal_log(PAL_LOG_WARN, "help: cannot write %s", path);
            ok = false;
        }
    }
    return ok;
}

bool app_help_dir(const app *a, char *out, size_t cap)
{
    const char *base;
    if (!a || !out || cap == 0u) return false;
    out[0] = '\0';
    if (a->opts.config_dir && a->opts.config_dir[0]) base = a->opts.config_dir;
    else base = pal_dir(PAL_DIR_CACHE);
    if (!base || !*base) return false;
    pal_path_join(out, cap, base, "help");
    return out[0] != '\0';
}

bool app_help_file_url(const char *path, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k;
    if (!path || !out || cap < 9u) return false;
    k = (size_t)snprintf(out, cap, "file://");
    if (path[0] != '/' && path[0] != '\\') out[k++] = '/';     /* "C:/..." -> "/C:/..." */
    for (const char *p = path; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (k + 4u >= cap) return false;
        if (ch == '\\') ch = '/';
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
            (ch == ':' && p == path + 1)) {
            out[k++] = (char)ch;
        } else {
            out[k++] = '%';
            out[k++] = hex[ch >> 4];
            out[k++] = hex[ch & 15u];
        }
    }
    out[k] = '\0';
    return true;
}

/* ---- opening ------------------------------------------------------------------------------ */
typedef struct help_job {
    app_help_set set;
    char         dir[1024];
    char         page[96];
    bool         ok;
} help_job;

static void page_url(const char *dir, const char *page, char *url, size_t cap)
{
    char file[128], path[1200];
    snprintf(file, sizeof file, "%s.html", page);
    pal_path_join(path, sizeof path, dir, file);
    if (!app_help_file_url(path, url, cap)) url[0] = '\0';
}

static void job_work(void *ud)
{
    help_job *j = (help_job *)ud;
    j->ok = app_help_write(&j->set, j->dir);
}

static void job_done(app *a, void *ud)
{
    help_job *j = (help_job *)ud;
    char url[4096];
    if (j->ok) {
        page_url(j->dir, j->page, url, sizeof url);
        if (url[0]) m_open_url(a, url);
    } else {
        app_error(a, "The help pages could not be written to \"%s\".", j->dir);
    }
    app_help_set_free(&j->set);
    free(j);
}

static bool known_page(const char *page)
{
    if (strcmp(page, "search") == 0) return true;
    for (int i = 0; i < app_help_page_count(); i++)
        if (strcmp(app_help_page_name(i), page) == 0) return true;
    return false;
}

bool app_help_open(app *a, const char *page)
{
    help_job *j;
    if (!a || !page || !known_page(page)) return false;
    j = (help_job *)calloc(1u, sizeof *j);
    if (!j) return false;
    app_copy_str(j->page, sizeof j->page, page);
    if (!app_help_dir(a, j->dir, sizeof j->dir)) {
        app_error(a, "There is no folder for the help pages on this system.");
        free(j);
        return false;
    }
    if (a->opts.headless && a->opts.config_dir && !a->opts.config_dir[0]) {
        char url[4096];
        page_url(j->dir, j->page, url, sizeof url);   /* tests: record, write nothing */
        if (url[0]) m_open_url(a, url);
        free(j);
        return true;
    }
    if (!app_help_build(a, &j->set)) {
        app_help_set_free(&j->set);
        free(j);
        app_error(a, "The help pages could not be prepared (out of memory).");
        return false;
    }
    if (!app_task(a, job_work, job_done, j)) {
        app_help_set_free(&j->set);
        free(j);
        return false;
    }
    return true;
}
