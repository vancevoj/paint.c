/* test_uib_help.c - lane UIB (wave 4 items 4 to 6): the user guide inside
 * paint.c.
 *   t_md_blocks, t_md_inline  the Markdown subset (help_md.h) gives exact
 *                             HTML: headings with unique ids, lists, quotes,
 *                             code, tables, links to .md pages, escaping,
 *                             directives, CRLF sources
 *   t_md_robust               random and hostile input (fixed seed): no
 *                             crash, balanced elements, no raw '<' from text
 *   t_file_url                file:// URLs for POSIX and Windows paths
 *   t_sources                 every Markdown page in docs/help of the source
 *                             tree is a page, in navigation order
 *   t_pages                   the built guide: every page and the search
 *                             page exist, every local link and anchor
 *                             resolves, no directive is left, no link to
 *                             the dead wiki or discussions, the generated
 *                             tables match the running app (shortcuts from
 *                             the key map, tool letters, effects, formats)
 *   t_menu                    Help > Documentation (F1), Search (Ctrl+E),
 *                             Tutorials and Plugins open local pages that
 *                             exist; Forum is hidden in a private build;
 *                             unchanged pages are not rewritten
 *   t_example_plugin          the guide's plugin example builds (CMake) and
 *                             runs through the real plugin loader
 * Headless, dummy video driver. */
#include "app_test_util.h"

#include "edit/m_ui.h"
#include "fx/afx.h"
#include "help/help.h"
#include "help/help_md.h"

#ifndef PC_PROJECT_PUBLIC
#define PC_PROJECT_PUBLIC 0
#endif
#ifndef UIB_FADE_PLUGIN
#define UIB_FADE_PLUGIN ""
#endif

/* ---- Markdown ----------------------------------------------------------------------- */
static bool dir_cb(void *ud, const char *name, bool block, help_buf *out)
{
    (void)ud;
    if (block && strcmp(name, "box") == 0) {
        help_buf_puts(out, "<div class=\"box\">Generated &amp; inserted</div>\n");
        return true;
    }
    if (!block && strcmp(name, "who") == 0) {
        help_buf_puts(out, "Tom & <Jerry>");
        return true;
    }
    return false;
}

static char *md(const char *src)
{
    help_doc d;
    char *r;
    memset(&d, 0, sizeof d);
    CHECK(help_md_render(src, strlen(src), dir_cb, NULL, &d));
    r = (char *)malloc(d.html.n + 1u);
    if (r) {
        memcpy(r, d.html.s ? d.html.s : "", d.html.n);
        r[d.html.n] = '\0';
    }
    help_doc_free(&d);
    return r;
}

static void expect_md(const char *src, const char *want)
{
    char *got = md(src);
    CHECK(got && strcmp(got, want) == 0);
    if (got && strcmp(got, want) != 0) INFO("md:\n%s\ngot:\n%s\nwant:\n%s", src, got, want);
    free(got);
}

static void t_md_blocks(void)
{
    help_doc d;
    const char *page = "# Title *one*\n\nIntro text\nsecond line.\n\n## Part A\n\nText A.\n\n"
                       "## Part A\n\n### Deep: (x)\n";
    expect_md("# Hello World\n", "<h1 id=\"hello-world\">Hello World</h1>\n");
    expect_md("## Closed ##\n", "<h2 id=\"closed\">Closed</h2>\n");
    expect_md("#NoSpace\n", "<p>#NoSpace</p>\n");
    expect_md("one\ntwo\n\nthree\n", "<p>one\ntwo</p>\n<p>three</p>\n");
    expect_md("- a\n- b\n  more\n\n- c\n\nafter\n",
              "<ul>\n<li>a</li>\n<li>b\nmore</li>\n<li>c</li>\n</ul>\n<p>after</p>\n");
    expect_md("1. x\n2. y\n", "<ol>\n<li>x</li>\n<li>y</li>\n</ol>\n");
    expect_md("> note\n> more\n", "<blockquote><p>note\nmore</p></blockquote>\n");
    expect_md("```c\nint a<b;\n\n```\n", "<pre><code class=\"language-c\">int a&lt;b;\n\n"
                                         "</code></pre>\n");
    expect_md("---\n", "<hr>\n");
    expect_md("| A | B |\n|:--|--:|\n| `x|y` | **b** |\n| only |\n",
              "<table>\n<thead><tr><th style=\"text-align:left\">A</th>"
              "<th style=\"text-align:right\">B</th></tr>\n</thead>\n<tbody>\n"
              "<tr><td style=\"text-align:left\"><code>x|y</code></td>"
              "<td style=\"text-align:right\"><strong>b</strong></td></tr>\n"
              "<tr><td style=\"text-align:left\">only</td><td style=\"text-align:right\"></td>"
              "</tr>\n</tbody></table>\n");
    expect_md("text\n{{box}}\nmore\n",
              "<p>text</p>\n<div class=\"box\">Generated &amp; inserted</div>\n<p>more</p>\n");
    expect_md("{{unknown}}\n", "");
    /* CRLF sources and a byte order mark */
    expect_md("\xEF\xBB\xBF# T\r\n\r\n| a |\r\n|---|\r\n| b |\r\n",
              "<h1 id=\"t\">T</h1>\n<table>\n<thead><tr><th>a</th></tr>\n</thead>\n<tbody>\n"
              "<tr><td>b</td></tr>\n</tbody></table>\n");
    /* title, unique ids and the sections of the search index */
    memset(&d, 0, sizeof d);
    CHECK(help_md_render(page, strlen(page), NULL, NULL, &d));
    CHECK(strcmp(d.title, "Title one") == 0);
    CHECK(d.html.s && strstr(d.html.s, "<h2 id=\"part-a\">") && strstr(d.html.s,
                                                                      "<h2 id=\"part-a-2\">"));
    CHECK(d.html.s && strstr(d.html.s, "<h3 id=\"deep-x\">Deep: (x)</h3>"));
    CHECK(d.nsec == 5);
    if (d.nsec == 5) {
        CHECK(d.sec[0].text.n == 0u && d.sec[1].level == 1);
        CHECK(strcmp(d.sec[1].anchor, "title-one") == 0);
        CHECK(d.sec[1].text.s && strcmp(d.sec[1].text.s, "Intro text second line. ") == 0);
        CHECK(strcmp(d.sec[2].title, "Part A") == 0 && d.sec[2].text.s &&
              strcmp(d.sec[2].text.s, "Text A. ") == 0);
        CHECK(strcmp(d.sec[3].anchor, "part-a-2") == 0 && d.sec[4].level == 3);
    }
    help_doc_free(&d);
    /* pages that share one document: ids and anchors carry the page */
    {
        const char *src = "# A\n\nSee [tips](layers.md#tips), [here](#a), [page](layers.md) "
                          "and [file](fade_plugin.c).\n\n## A\n";
        memset(&d, 0, sizeof d);
        CHECK(help_md_render_page(src, strlen(src), "tools", NULL, NULL, &d));
        CHECK(d.html.s && strcmp(d.html.s,
              "<h1 id=\"tools-a\">A</h1>\n<p>See <a href=\"layers.html#layers-tips\">tips</a>, "
              "<a href=\"#tools-a\">here</a>, <a href=\"layers.html\">page</a> and "
              "<a href=\"fade_plugin.c\">file</a>.</p>\n<h2 id=\"tools-a-2\">A</h2>\n") == 0);
        if (d.html.s && !strstr(d.html.s, "layers-tips")) INFO("%s", d.html.s);
        CHECK(d.nsec == 3 && strcmp(d.sec[2].anchor, "tools-a-2") == 0);
        help_doc_free(&d);
    }
    {
        char slug[16];
        help_md_slug("  Hello, World!  ", 17u, slug, sizeof slug);
        CHECK(strcmp(slug, "hello-world") == 0);
        help_md_slug("***", 3u, slug, sizeof slug);
        CHECK(strcmp(slug, "section") == 0);
        help_md_slug("abcdefghijklmnopqrstuvwxyz", 26u, slug, sizeof slug);
        CHECK(strlen(slug) == 15u);
    }
}

static void t_md_inline(void)
{
    expect_md("a **b** *c* `d<e` \\*f\\*\n",
              "<p>a <strong>b</strong> <em>c</em> <code>d&lt;e</code> *f*</p>\n");
    expect_md("x * y * z\n", "<p>x * y * z</p>\n");
    expect_md("[Tools](tools.md) and [top](tools.md#selection-tools) and [x](https://a.b/c.md)\n",
              "<p><a href=\"tools.html\">Tools</a> and <a href=\"tools.html#selection-tools\">"
              "top</a> and <a href=\"https://a.b/c.md\">x</a></p>\n");
    expect_md("[bad](javascript:alert(1)) [data](DATA:text/html,x)\n",
              "<p><a href=\"#\">bad</a>) <a href=\"#\">data</a></p>\n");
    expect_md("<https://example.org/a?b=1&c=2>\n",
              "<p><a href=\"https://example.org/a?b=1&amp;c=2\">"
              "https://example.org/a?b=1&amp;c=2</a></p>\n");
    expect_md("by {{who}} in `{{who}}` {{nope}} {{ bad }}\n",
              "<p>by Tom &amp; &lt;Jerry&gt; in <code>Tom &amp; &lt;Jerry&gt;</code>  "
              "{{ bad }}</p>\n");
    expect_md("**[bold link](a.md)** <b>raw</b> & \"q\" 'a'\n",
              "<p><strong><a href=\"a.html\">bold link</a></strong> &lt;b&gt;raw&lt;/b&gt; "
              "&amp; &quot;q&quot; &#39;a&#39;</p>\n");
    /* unbalanced markers are closed in order */
    expect_md("**open *both\n", "<p><strong>open <em>both</em></strong></p>\n");
    expect_md("[a **b](c.md) d**\n",
              "<p><a href=\"c.html\">a <strong>b</strong></a><strong> d</strong></p>\n");
    expect_md("`` a`b ``\n", "<p><code>a`b</code></p>\n");
    expect_md("`unclosed\n", "<p>`unclosed</p>\n");
}

static int count_of(const char *s, const char *what)
{
    int n = 0;
    size_t wl = strlen(what);
    for (const char *p = s; (p = strstr(p, what)) != NULL; p += wl) n++;
    return n;
}

static void t_md_robust(void)
{
    static const char *const tok[] = {
        "#", "## ", "- ", "1. ", "> ", "```", "|", "|---|", "**", "*", "`", "[", "](",
        ")", "<", ">", "&", "{{", "}}", "who", "box", "\\", "\n", "\r\n", " ", "x", "\t",
        "javascript:", ".md", "#a", "\xE2\x80\xA8", "\xC3\xA9", "---", "\n\n"
    };
    const char *tags[] = { "p", "ul", "ol", "li", "table", "thead", "tbody", "tr", "td", "th",
                           "blockquote", "pre", "code", "strong", "em", "a" };
    int rounds = g_quick ? 400 : 4000;
    for (int r = 0; r < rounds; r++) {
        char src[600];
        size_t n = 0;
        help_doc d;
        int parts = 1 + (int)rndu(60u);
        bool raw = rndu(4u) == 0u;
        for (int k = 0; k < parts && n + 16u < sizeof src; k++) {
            if (raw) {
                src[n++] = (char)rnd8();
            } else {
                const char *t = tok[rndu((uint32_t)(sizeof tok / sizeof tok[0]))];
                size_t tl = strlen(t);
                memcpy(src + n, t, tl);
                n += tl;
            }
        }
        memset(&d, 0, sizeof d);
        CHECK(help_md_render(src, n, dir_cb, NULL, &d));
        if (d.html.s) {
            const char *h = d.html.s;
            for (size_t t = 0; t < sizeof tags / sizeof tags[0]; t++) {
                char o1[24], o2[24], c[24];
                int opens;
                snprintf(o1, sizeof o1, "<%s>", tags[t]);
                snprintf(o2, sizeof o2, "<%s ", tags[t]);
                snprintf(c, sizeof c, "</%s>", tags[t]);
                opens = count_of(h, o1) + count_of(h, o2);
                if (opens != count_of(h, c)) {
                    CHECK(!"unbalanced element");
                    INFO("tag %s in output of round %d", tags[t], r);
                }
            }
            CHECK(strstr(h, "javascript:") == NULL || strstr(h, "href=\"javascript") == NULL);
            CHECK(strlen(h) == d.html.n);
        }
        help_doc_free(&d);
    }
    /* many equal headings get distinct ids quickly */
    {
        size_t reps = 20000u, k = 0;
        char *s = (char *)malloc(reps * 4u + 1u);
        help_doc d;
        CHECK(s != NULL);
        if (s) {
            for (size_t i = 0; i < reps; i++) {
                memcpy(s + k, "# a\n", 4u);
                k += 4u;
            }
            memset(&d, 0, sizeof d);
            CHECK(help_md_render(s, k, NULL, NULL, &d));
            CHECK(d.nsec == (int)reps + 1);
            if (d.nsec > 12) {
                CHECK(strcmp(d.sec[1].anchor, "a") == 0 && strcmp(d.sec[2].anchor, "a-2") == 0);
                CHECK(strcmp(d.sec[9].anchor, "a-9") == 0 && strcmp(d.sec[11].anchor, "a") != 0);
                CHECK(strcmp(d.sec[10].anchor, d.sec[11].anchor) != 0);
            }
            help_doc_free(&d);
            free(s);
        }
    }
    /* deep nesting characters and a long line stay linear and bounded */
    {
        size_t big = 200000u;
        char *s = (char *)malloc(big + 1u);
        help_doc d;
        CHECK(s != NULL);
        if (s) {
            for (size_t i = 0; i < big; i++) s[i] = "[*`>|-"[i % 6u];
            s[big] = '\0';
            memset(&d, 0, sizeof d);
            CHECK(help_md_render(s, big, NULL, NULL, &d));
            help_doc_free(&d);
            free(s);
        }
    }
}

static void t_file_url(void)
{
    char u[256];
    CHECK(app_help_file_url("/tmp/a b/x.html", u, sizeof u) &&
          strcmp(u, "file:///tmp/a%20b/x.html") == 0);
    CHECK(app_help_file_url("C:\\Users\\J\xC3\xBCrgen\\h#1.html", u, sizeof u) &&
          strcmp(u, "file:///C:/Users/J%C3%BCrgen/h%231.html") == 0);
    CHECK(!app_help_file_url("/very/long/path", u, 12u));
}

/* ---- the guide ------------------------------------------------------------------------- */
static void t_sources(void)
{
    const char *src = __FILE__;
    const char *tail = strstr(src, "tests/app/test_uib_help.c");
    char dir[1024];
    char **names = NULL;
    int n;
    CHECK(app_help_page_count() >= 10);
    CHECK(app_help_page_name(0) && strcmp(app_help_page_name(0), "index") == 0);
    CHECK(app_help_page_name(app_help_page_count()) == NULL);
    if (!tail) tail = strstr(src, "tests\\app\\test_uib_help.c");
    if (!tail) {
        INFO("source tree not found from __FILE__; skipping the comparison");
        return;
    }
    snprintf(dir, sizeof dir, "%.*sdocs/help", (int)(tail - src), src);
    n = pal_list_dir(dir, "*.md", &names);
    CHECK(n == app_help_page_count());
    for (int i = 0; i < n; i++) {
        bool found = false;
        size_t l = strlen(names[i]) - 3u;
        for (int k = 0; k < app_help_page_count(); k++)
            if (strlen(app_help_page_name(k)) == l &&
                strncmp(app_help_page_name(k), names[i], l) == 0)
                found = true;
        CHECK(found);
        if (!found) INFO("docs/help/%s is not a page", names[i]);
    }
    pal_free_names(names, n);
}

static const app_help_file *find_file(const app_help_set *s, const char *name, size_t n)
{
    for (int i = 0; i < s->n; i++)
        if (strlen(s->f[i].name) == n && strncmp(s->f[i].name, name, n) == 0) return &s->f[i];
    return NULL;
}

static bool has_id(const app_help_file *f, const char *id, size_t n)
{
    char probe[160];
    if (n + 6u >= sizeof probe) return false;
    snprintf(probe, sizeof probe, "id=\"%.*s\"", (int)n, id);
    return f->data.s && strstr(f->data.s, probe) != NULL;
}

/* Every href of every page: local pages exist, anchors exist. */
static void check_links(const app_help_set *s)
{
    int links = 0, bad = 0;
    for (int i = 0; i < s->n; i++) {
        const app_help_file *f = &s->f[i];
        const char *p = f->data.s;
        if (!p || !strstr(f->name, ".html")) continue;
        while ((p = strstr(p, "href=\"")) != NULL) {
            const char *u = p + 6, *e = strchr(u, '"'), *hash;
            const app_help_file *t = f;
            p = u;
            if (!e) break;
            links++;
            if (strncmp(u, "http", 4) == 0 || strncmp(u, "mailto:", 7) == 0) continue;
            hash = memchr(u, '#', (size_t)(e - u));
            if (hash != u) t = find_file(s, u, (size_t)((hash ? hash : e) - u));
            if (!t || (hash && !has_id(t, hash + 1, (size_t)(e - hash - 1)))) {
                bad++;
                if (bad < 5) INFO("dead link in %s: %.*s", f->name, (int)(e - u), u);
            }
        }
    }
    INFO("%d links checked", links);
    CHECK(links > 100 && bad == 0);
}

static app *cfg_app(const char *dir)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = 1280;
    o.height = 800;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    o.disable_plugins = true;
    return app_create(&o);
}

static const char *page_text(const app_help_set *s, const char *name)
{
    const app_help_file *f = find_file(s, name, strlen(name));
    return f && f->data.s ? f->data.s : "";
}

static void t_pages(void)
{
    char dir[1024];
    app *a;
    app_help_set set;
    at_out_path(dir, sizeof dir, "test_uib_help_cfg");
    (void)pal_mkdirs(dir);
    a = cfg_app(dir);
    CHECK(a != NULL);
    if (!a) return;
    memset(&set, 0, sizeof set);
    CHECK(app_help_build(a, &set));
    /* search.html, fade_plugin.c, fx_abi.h, fx_util.h, fx_widgets.h (ADR-024) */
    CHECK(set.n == app_help_page_count() + 5);
    for (int i = 0; i < app_help_page_count(); i++) {
        char name[96];
        const char *t;
        snprintf(name, sizeof name, "%s.html", app_help_page_name(i));
        t = page_text(&set, name);
        CHECK(strncmp(t, "<!DOCTYPE html>", 15) == 0 && strstr(t, "</html>") != NULL);
        CHECK(strstr(t, "<title>") && strstr(t, "<h1 id=") && strstr(t, "aria-current"));
        CHECK(strstr(t, "{{") == NULL);                /* every directive resolved */
        {
            /* the whole guide, with this file's own page shown */
            char cur[160];
            snprintf(cur, sizeof cur, "<section class=\"page current\" id=\"%s\"",
                     app_help_page_name(i));
            CHECK(strstr(t, cur) != NULL && count_of(t, "page current") == 1);
            CHECK(count_of(t, "<section class=\"page") == app_help_page_count() + 1);
            CHECK(count_of(t, "</script>") == 1);  /* no index text closes the script */
        }
        CHECK(strstr(t, "/wiki") == NULL && strstr(t, "/discussions") == NULL);
        CHECK(strstr(t, "\xE2\x80\x94") == NULL && strstr(t, "\xE2\x80\x93") == NULL);
    }
    check_links(&set);
    /* generated tables follow the running app */
    {
        const char *k = page_text(&set, "keyboard.html");
        char want[128];
        snprintf(want, sizeof want, "<tr><td>Undo</td><td>%s</td></tr>",
                 app_cmd_shortcut_text(a, "edit.undo"));
        CHECK(strstr(k, want) != NULL);
        CHECK(strstr(k, "<tr><td>Paintbrush</td><td>B</td><td>1</td></tr>") != NULL);
        CHECK(strstr(k, "<tr><td>Lasso Select</td><td>S</td><td>2</td></tr>") != NULL);
        CHECK(strstr(k, "<h3>File</h3>") && strstr(k, "<h3>Layers</h3>"));
    }
    {
        const char *t = page_text(&set, "adjustments-effects.html");
        CHECK(strstr(t, "Gaussian Blur") && strstr(t, "<td>Blurs</td>"));
        CHECK(strstr(t, "<tr><td>Curves</td>") != NULL);
        t = page_text(&set, "layers.html");
        CHECK(strstr(t, "<li>Color Dodge</li>") && strstr(t, "<li>Normal</li>"));
    }
    {
        const char *t = page_text(&set, "file-formats.html");
        CHECK(strstr(t, "<td>PNG</td><td>.png</td><td>Yes</td><td>Yes</td><td>No</td>") != NULL);
    }
    {
        const char *t = page_text(&set, "plugins.html");
        char pdir[1100];
        CHECK(strstr(t, "org.example.fade") != NULL);
        if (pal_dir(PAL_DIR_DATA)) {
            pal_path_join(pdir, sizeof pdir, pal_dir(PAL_DIR_DATA), "plugins");
            if (!strpbrk(pdir, "&<>\"'")) CHECK(strstr(t, pdir) != NULL);
        }
        CHECK(strstr(page_text(&set, "fx_abi.h"), "#define FX_ABI_VERSION") != NULL);
        CHECK(strstr(page_text(&set, "fx_widgets.h"), "FX_WIDGET_POSITION_GRID") != NULL);
        CHECK(strstr(t, "fx_widgets.h") != NULL);
        CHECK(strstr(page_text(&set, "fade_plugin.c"), "fx_entry") != NULL);
    }
    {
        const char *t = page_text(&set, "troubleshooting.html");
        CHECK(strstr(t, dir) != NULL);                 /* the real settings folder */
        CHECK(strstr(t, PC_PROJECT_PUBLIC ? "opens the project's issue list" :
                                            "is hidden until") != NULL);
        t = page_text(&set, "search.html");
        CHECK(strstr(t, "var IDX=[") && strstr(t, "{a:\"tools-selection-tools-s\",g:\"Tools\""));
        CHECK(strstr(t, "<section class=\"page current\" id=\"search\"") != NULL);
        CHECK(strstr(t, "<h2 id=\"tools-selection-tools-s\">") != NULL);
    }
    app_help_set_free(&set);
    app_destroy(a);
}

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 3);
}

/* The last URL is a file URL of page in the help folder, and the file exists. */
static bool opened(app *a, const char *dir, const char *page)
{
    char path[1200], file[96], url[2048];
    const char *u = m_last_url(a);
    snprintf(file, sizeof file, "%s.html", page);
    pal_path_join(path, sizeof path, dir, file);
    if (!app_help_file_url(path, url, sizeof url) || !u) return false;
    if (strcmp(u, url) != 0) INFO("opened %s, expected %s", u, url);
    return strcmp(u, url) == 0 && pal_file_exists(path);
}

static void t_menu(void)
{
    char cfg[1024], dir[1100], idx[1200];
    app *a;
    uint64_t t0;
    at_out_path(cfg, sizeof cfg, "test_uib_help_menu");
    (void)pal_mkdirs(cfg);
    a = cfg_app(cfg);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_help_dir(a, dir, sizeof dir));
    at_frames(a, 2);
    tap(a, SDLK_F1, SDL_KMOD_NONE);
    CHECK(opened(a, dir, "index"));
    tap(a, SDLK_E, AT_KMOD_PRIMARY);
    CHECK(opened(a, dir, "search"));
    CHECK(app_cmd_exec(a, "help.tutorials"));
    at_frames(a, 2);
    CHECK(opened(a, dir, "tutorials"));
    CHECK(app_cmd_exec(a, "help.plugins"));
    at_frames(a, 2);
    CHECK(opened(a, dir, "plugins"));
    CHECK(app_cmd_exists(a, "help.forum") == (PC_PROJECT_PUBLIC != 0));
    /* the remaining links keep working */
    CHECK(app_cmd_exec(a, "help.website") && m_last_url(a) &&
          strncmp(m_last_url(a), "https://", 8) == 0);
    /* unchanged pages are not rewritten */
    pal_path_join(idx, sizeof idx, dir, "index.html");
    t0 = pal_file_mtime(idx);
    CHECK(t0 != 0u);
    CHECK(app_cmd_exec(a, "help.docs"));
    at_frames(a, 2);
    CHECK(pal_file_mtime(idx) == t0);
    /* the Help menu from the keyboard: Alt+H, then T chooses Tutorials */
    CHECK(app_cmd_exec(a, "help.plugins"));
    at_frames(a, 2);
    key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true);
    key_ev(a, SDLK_H, SDL_KMOD_LALT, true);
    key_ev(a, SDLK_H, SDL_KMOD_LALT, false);
    key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false);
    at_frames(a, 3);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    tap(a, SDLK_T, SDL_KMOD_NONE);
    CHECK(opened(a, dir, "tutorials"));
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    app_destroy(a);
    /* tests without a settings folder record the URL and write nothing */
    a = at_app(800, 600);
    CHECK(a != NULL);
    if (a) {
        CHECK(app_cmd_exec(a, "help.tutorials"));
        at_frames(a, 2);
        CHECK(m_last_url(a) && strncmp(m_last_url(a), "file://", 7) == 0 &&
              strstr(m_last_url(a), "/help/tutorials.html") != NULL);
        app_destroy(a);
    }
}

static void t_example_plugin(void)
{
    afx_plugins *p;
    fx_registry *r;
    const fx_effect *fx;
    if (!UIB_FADE_PLUGIN[0] || !pal_file_exists(UIB_FADE_PLUGIN)) {
        INFO("the example plugin was not built (UIB_FADE_PLUGIN); skipping");
        CHECK(true);
        return;
    }
    p = afx_plugins_create();
    r = fx_registry_create();
    CHECK(p && r);
    if (!p || !r) {
        afx_plugins_destroy(p);
        fx_registry_destroy(r);
        return;
    }
    CHECK(afx_plugins_load_file(p, r, UIB_FADE_PLUGIN) == 1);
    CHECK(afx_plugins_error_count(p) == 0u);
    fx = fx_registry_find(r, "org.example.fade");
    CHECK(fx && fx->menu && strcmp(fx->menu, "Effects/Examples/Fade") == 0);
    if (fx) {
        const afx_plugin_info *info = afx_plugins_info(p, fx);
        uint8_t src_px[8 * 4 * 4], dst_px[8 * 4 * 4];
        fx_img src, dst;
        fx_env env;
        void *params;
        CHECK(info && strcmp(info->author, "paint.c help example") == 0 &&
              strcmp(info->version, "1.0") == 0);
        for (int i = 0; i < 8 * 4; i++) {
            src_px[i * 4 + 0] = 0;           /* B */
            src_px[i * 4 + 1] = 100;         /* G */
            src_px[i * 4 + 2] = 200;         /* R */
            src_px[i * 4 + 3] = (uint8_t)(i * 7);
        }
        memset(dst_px, 0, sizeof dst_px);
        memset(&src, 0, sizeof src);
        src.px = src_px;
        src.stride = 8 * 4;
        src.chans = 4;
        src.r.w = 8;
        src.r.h = 4;
        dst = src;
        dst.px = dst_px;
        memset(&env, 0, sizeof env);
        env.size = (uint32_t)sizeof env;
        env.doc_w = 8;
        env.doc_h = 4;
        env.sel.w = 8;
        env.sel.h = 4;
        env.primary = 0xFF000000u;
        env.secondary = 0xFFFFFFFFu;          /* the default color: secondary, white */
        params = fx_params_new(fx, &env);
        CHECK(params != NULL);
        if (params) {
            CHECK(fx_run_sync(fx, params, &src, &dst, &env, src.r, NULL) == PC_OK);
            /* Amount 50 %: halfway to white, alpha kept */
            CHECK(dst_px[0] == 128 && dst_px[1] == 178 && dst_px[2] == 228);
            CHECK(dst_px[4 * 31 + 3] == (uint8_t)(31 * 7) && dst_px[4 * 31 + 1] == 178);
            fx_params_free(params);
        }
    }
    fx_registry_destroy(r);
    afx_plugins_destroy(p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_md_blocks);
    RUN(t_md_inline);
    RUN(t_md_robust);
    RUN(t_file_url);
    RUN(t_sources);
    RUN(t_pages);
    RUN(t_menu);
    RUN(t_example_plugin);
    at_quit();
    return pc_test_finish();
}
