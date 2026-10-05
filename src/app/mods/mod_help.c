/* mod_help.c - the Help menu ("?" button, MENUS.md "Help menu") and the
 * About dialog, lane M, plus small shell commands (Reset Windows, brush
 * width keys K-TB-WIDTH-*). The Settings dialog lives in mod_m_settings.c.
 *
 * Help items open pages in the browser (paint.c's own, never Paint.NET's).
 * lane UIB (wave 4 items 4 to 6): Documentation (F1), Search (Ctrl+E),
 * Tutorials and Plugins open the user guide that ships inside paint.c
 * (src/app/help/help.h: the Markdown pages in docs/help rendered to local
 * HTML files and opened through file:// URLs), so they work offline and
 * never lead to a missing page. The project repository is private, its wiki is empty and
 * Discussions are off, so the old wiki and discussion links were dead.
 * Forum opens the project's issue list and exists only in builds of a
 * public repository (cmake -DPC_PROJECT_PUBLIC=ON); otherwise it stays
 * hidden and the guide's troubleshooting page says why. Website and Send
 * Feedback or Bug Report (a new issue prefilled with a template and the
 * Diagnostics text) still lead to the repository, which its members can
 * open. About shows the credits. Donate has no paint.c counterpart and
 * stays hidden. The links are defined once below (M_URL_*). Headless apps
 * only record the URL (m_last_url) so tests never start a browser.
 *
 * About (MENUS.md "About dialog", F-DLG-ABOUT): product name, version and
 * build, renderer, license, the not-affiliated statement and the complete
 * NOTICE text (the Paint.NET 3.36 MIT attribution, the Ed Harvey notice
 * and every third-party component with its license) in a scrolling area.
 *
 * Thread rules: main thread. */
#include "../app_internal.h"
#include "../edit/m_help.h"
#include "../edit/m_settings.h"
#include "../edit/m_ui.h"
#include "../help/help.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_URL_HOME     "https://github.com/vancevoj/paint.c"
#define M_URL_FEEDBACK M_URL_HOME "/issues/new"
#define M_URL_FORUM    M_URL_HOME "/issues"          /* lane UIB: only when public */

#ifndef PC_PROJECT_PUBLIC
#define PC_PROJECT_PUBLIC 0
#endif

/* The NOTICE file of the source tree, embedded at build time by
 * cmake/PcEmbed.cmake (src/app/CMakeLists.txt), so About can never drift from
 * it; test_m_help compares the two. */
extern const unsigned char pc_notice_txt[];
extern const size_t pc_notice_txt_size;

/* Next line of the embedded NOTICE starting at *pos into out (CR stripped).
 * Returns false at the end. */
static bool notice_line(size_t *pos, char *out, size_t cap)
{
    size_t p = *pos, n = 0;
    if (p >= pc_notice_txt_size) return false;
    while (p < pc_notice_txt_size && pc_notice_txt[p] != '\n') {
        if (pc_notice_txt[p] != '\r' && n + 1u < cap) out[n++] = (char)pc_notice_txt[p];
        p++;
    }
    out[n] = '\0';
    *pos = p < pc_notice_txt_size ? p + 1u : p;
    return true;
}

/* ---- About ---------------------------------------------------------------------- */
static void build_line(app *a, char *out, size_t cap)
{
    int v = SDL_GetVersion();
#if defined(__clang__)
    const char *cc = "Clang " __clang_version__;
#elif defined(__GNUC__)
    const char *cc = "GCC " __VERSION__;
#elif defined(_MSC_VER)
    const char *cc = "MSVC";
#else
    const char *cc = "C compiler";
#endif
    snprintf(out, cap, "Version %s (%s, %s, SDL %d.%d.%d, renderer %s)", APP_VERSION,
             SDL_GetPlatform(), cc, SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
             SDL_VERSIONNUM_MICRO(v),
             a->ren && SDL_GetRendererName(a->ren) ? SDL_GetRendererName(a->ren) : "?");
}

size_t m_about_text(app *a, char *out, size_t cap)
{
    size_t k = 0;
    char line[512];
    build_line(a, line, sizeof line);
#define ADD(s)                                                                      \
    do {                                                                            \
        int w_ = snprintf(out + k, k < cap ? cap - k : 0u, "%s\n", (s));            \
        if (w_ > 0) k += (size_t)w_;                                                \
        if (k >= cap) k = cap ? cap - 1u : 0u;                                      \
    } while (0)
    if (cap) out[0] = '\0';
    ADD(APP_NAME);
    ADD(line);
    {
        size_t pos = 0;
        char nl[512];
        while (notice_line(&pos, nl, sizeof nl)) ADD(nl);
    }
#undef ADD
    return k;
}

static bool about_frame(app *a, void *st)
{
    ui_ctx *ui = a->ui;
    char line[512];
    ui_rect box;
    uint32_t r;
    (void)st;
    ui_dialog_begin(ui, "About paint.c##about", 640.0f, 0.0f);
    ui_heading(ui, APP_NAME);
    build_line(a, line, sizeof line);
    ui_label_ex(ui, line, UI_LABEL_DIM);
    ui_layout_space(ui, 4.0f);
    ui_text_wrapped(ui, "An independent, clean-room editor that reproduces the Paint.NET 5.1 "
                        "workflow. Paint.NET is a trademark of dotPDN LLC; paint.c is not "
                        "affiliated with or endorsed by dotPDN LLC.",
                    0);
    ui_text_wrapped(ui, "paint.c is free software under the MIT License. The credits and the "
                        "licenses of the components it includes follow.",
                    UI_LABEL_DIM);
    ui_layout_space(ui, 6.0f);
    box = ui_layout_next(ui, 0, ui_px(ui, 300.0f));
    ui_scroll_begin(ui, "##about_credits", box, 0);
    ui_layout_push(ui, ui_layout_content(ui), 8.0f);
    ui_layout_set_spacing(ui, 0.0f);
    {
        size_t pos = 0;
        char nl[512];
        while (notice_line(&pos, nl, sizeof nl)) {
            if (!nl[0]) ui_layout_space(ui, 6.0f);
            else ui_text_wrapped(ui, nl, UI_LABEL_SMALL);
        }
    }
    ui_layout_pop(ui);
    ui_scroll_end(ui);
    ui_dialog_buttons(ui, UI_DLG_CLOSE, UI_DLG_CLOSE);
    r = ui_dialog_end(ui);
    if (r) a->about_open = false;
    return r == 0;
}

static void cmd_about(app *a, const app_cmd *c)
{
    (void)c;
    if (a->about_open) return;
    a->about_open = app_dialog_push(a, about_frame, NULL, NULL);
}

/* ---- links ----------------------------------------------------------------------- */
static void url_append(char *out, size_t cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = strlen(out);
    for (; *s && k + 4u < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[k++] = (char)c;
        } else {
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15u];
        }
    }
    out[k] = '\0';
}

/* lane UIB: a page of the bundled guide (help.h); c->arg indexes k_pages. */
static void cmd_guide(app *a, const app_cmd *c)
{
    static const char *const k_pages[] = { "index", "search", "tutorials", "plugins" };
    size_t i = (size_t)c->arg;
    (void)app_help_open(a, i < sizeof k_pages / sizeof k_pages[0] ? k_pages[i] : "index");
}

static void cmd_link(app *a, const app_cmd *c)
{
    static const char *const urls[] = { M_URL_HOME, M_URL_FORUM };
    size_t i = (size_t)c->arg;
    m_open_url(a, i < sizeof urls / sizeof urls[0] ? urls[i] : M_URL_HOME);
}

static void cmd_feedback(app *a, const app_cmd *c)
{
    char diag[2048], body[2600], url[8192];
    (void)c;
    m_settings_diagnostics(a, diag, sizeof diag);
    snprintf(body, sizeof body,
             "What happened:\n\nSteps to reproduce:\n1.\n2.\n\nWhat you expected:\n\n"
             "Diagnostics:\n```\n%s```\n",
             diag);
    snprintf(url, sizeof url, "%s?body=", M_URL_FEEDBACK);
    url_append(url, sizeof url, body);
    m_open_url(a, url);
}

/* ---- brush width keys and windows --------------------------------------------------- */
static bool uses_width(app *a, const app_cmd *c)
{
    const app_tool *t = app_tool_current(a);
    (void)c;
    return t && (t->flags & APP_TOOL_USES_WIDTH);
}

static void cmd_width(app *a, const app_cmd *c)
{
    float w = a->ts.width + (float)c->arg;
    if (w < 1.0f) w = 1.0f;
    if (w > 2000.0f) w = 2000.0f;
    a->ts.width = w;
    app_tool_settings_changed(a);
}

static void cmd_reset_windows(app *a, const app_cmd *c)
{
    (void)c;
    app_panels_reset_all(a);
}

static void reg(app *a, const char *id, const char *label, ui_icon icon, uint32_t flags,
                app_cmd_fn run, app_cmd_pred en, intptr_t arg)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = flags;
    d.run = run;
    d.enabled = en;
    d.arg = arg;
    (void)app_cmd_register(a, &d);
}

void mod_help(app *a)
{
    const uint32_t nc = APP_CMD_NO_COMMIT;
    reg(a, "help.docs", "Documentation", UI_ICON_HELP, nc, cmd_guide, NULL, 0);
    reg(a, "help.website", "Website", UI_ICON_NONE, nc, cmd_link, NULL, 0);
    reg(a, "help.search", "Search", UI_ICON_NONE, nc, cmd_guide, NULL, 1);
#if PC_PROJECT_PUBLIC
    reg(a, "help.forum", "Forum", UI_ICON_NONE, nc, cmd_link, NULL, 1);         /* lane UIB */
#endif
    reg(a, "help.tutorials", "Tutorials", UI_ICON_NONE, nc, cmd_guide, NULL, 2);
    reg(a, "help.plugins", "Plugins", UI_ICON_NONE, nc, cmd_guide, NULL, 3);
    reg(a, "help.feedback", "Send Feedback or Bug Report", UI_ICON_NONE, nc, cmd_feedback, NULL,
        0);
    reg(a, "help.about", "About", UI_ICON_INFO, nc, cmd_about, NULL, 0);
    reg(a, "window.reset_all", "Reset Windows", UI_ICON_RESET, nc, cmd_reset_windows, NULL, 0);
    reg(a, "tool.width_dec", "Decrease Brush Width", UI_ICON_MINUS, nc | APP_CMD_REPEAT,
        cmd_width, uses_width, -1);
    reg(a, "tool.width_inc", "Increase Brush Width", UI_ICON_PLUS, nc | APP_CMD_REPEAT, cmd_width,
        uses_width, 1);
    reg(a, "tool.width_dec5", "Decrease Brush Width by 5", UI_ICON_MINUS, nc | APP_CMD_REPEAT,
        cmd_width, uses_width, -5);
    reg(a, "tool.width_inc5", "Increase Brush Width by 5", UI_ICON_PLUS, nc | APP_CMD_REPEAT,
        cmd_width, uses_width, 5);
}
