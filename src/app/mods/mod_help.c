/* mod_help.c - the Help menu ("?" button, MENUS.md "Help menu") and the
 * About dialog, lane M, plus small shell commands (Reset Windows, brush
 * width keys K-TB-WIDTH-*). The Settings dialog lives in mod_m_settings.c.
 *
 * Help items open the project's pages in the browser (paint.c's own
 * project, never Paint.NET's): Documentation (F1; a bundled docs/index.html
 * next to the executable when present), Website, Search (Ctrl+E, the
 * project search), Send Feedback or Bug Report (a new issue prefilled with
 * a template and the Diagnostics text) and About. Donate, Forum, Tutorials
 * and Plugins have no paint.c counterpart yet and stay hidden. The links
 * are defined once below (M_URL_*). Headless apps only record the URL
 * (m_last_url) so tests never start a browser.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_URL_HOME     "https://github.com/vancevoj/paint.c"
#define M_URL_DOCS     M_URL_HOME "#readme"
#define M_URL_SEARCH   M_URL_HOME "/search"
#define M_URL_FEEDBACK M_URL_HOME "/issues/new"

/* The NOTICE file of the source tree, line by line (keep in sync; the
 * test_m_help test compares the two). */
static const char *const k_notice[] = {
    "paint.c (formerly the PortableCanvas working name)",
    "",
    "This product contains code derived from Paint.NET 3.36. The blend formulas and the",
    "integer compositing semantics in src/pc_blend.c re-implement, in C, the UserBlendOps of",
    "Paint.NET 3.36, which were released under the MIT License reproduced below.",
    "",
    "Paint.NET",
    "Copyright (C) dotPDN LLC, Rick Brewster, Chris Crosetto, Tom Jackson, Michael Kelsey,",
    "Brandon Ortiz, Craig Taylor, Chris Trevino, and Luke Walker.",
    "Portions Copyright (C) Microsoft Corporation. All Rights Reserved.",
    "",
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this",
    "software and associated documentation files (the \"Software\"), to deal in the Software",
    "without restriction, including without limitation the rights to use, copy, modify,",
    "merge, publish, distribute, sublicense, and/or sell copies of the Software, and to",
    "permit persons to whom the Software is furnished to do so, subject to the following",
    "conditions:",
    "",
    "The above copyright notice and this permission notice shall be included in all copies",
    "or substantial portions of the Software.",
    "",
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,",
    "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A",
    "PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT",
    "HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF",
    "CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE",
    "OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    "",
    "The Paint.NET 3.36 license excludes its logo and icon artwork, its resource assets",
    "(.resources, .resx and .png files, menu and status text) and the GPC library from the",
    "MIT grant. None of that material is used here, and it must never be added.",
    "",
    "----------------------------------------------------------------------------------------",
    "Additional Paint.NET 3.36 derived algorithms",
    "",
    "Several adjustments and effects (src/fx/adjust, src/fx/blur, src/fx/noise, src/fx/photo,",
    "src/fx/artistic, src/fx/distort, src/fx/render, src/fx/stylize) and a few codec and core",
    "behaviors re-implement algorithms from the MIT-licensed Paint.NET 3.36 source under the",
    "license reproduced above. The save pipeline of the BMP, GIF, TGA, TIFF and PNG codecs",
    "(src/codec/quant.c: Auto-detect bit depth, flattening onto white, transparency threshold,",
    "dithering level) follows Paint.NET 3.36's InternalFileType and Quantizer classes. Per-file",
    "details are in docs/notice/*.md. Some of those 3.36",
    "files carry an additional notice, also under the MIT License reproduced above:",
    "",
    "    Copyright (c) 2006-2008 Ed Harvey",
    "    (Posterize tables, WarpEffectBase, Polar Inversion, Dents, PerlinNoise2D)",
    "",
    "----------------------------------------------------------------------------------------",
    "Third-party components (full license texts ship in licenses/ with every binary package)",
    "",
    "    SDL 3.4.18                 zlib license          https://libsdl.org",
    "    zlib 1.3.2                 zlib license          https://zlib.net",
    "    libspng 0.7.4              BSD 2-Clause          https://libspng.org",
    "    libjpeg-turbo 3.2.0        IJG, BSD 3-Clause and zlib licenses",
    "    libwebp 1.6.0              BSD 3-Clause         "
    " https://chromium.googlesource.com/webm/libwebp",
    "    Little-CMS 2.19.1          MIT                   https://littlecms.com",
    "    bcdec, stb_dxt, bc7enc     MIT or public domain  (see third_party/*/LICENSE*)",
    "    stb_truetype 1.26          MIT or public domain, Copyright (c) 2017 Sean Barrett",
    "    Inter 4.1 (UI font)        SIL OFL 1.1, Copyright 2016 The Inter Project Authors",
    "                               (assets/fonts/OFL.txt)",
    "    Test fixtures only: subsets of Inter and Noto Sans CJK JP (SIL OFL 1.1, Copyright",
    "    2014-2021 Adobe, Reserved Font Name 'Source'), see tests/ui/data/README.md",
    "",
    "This software is based in part on the work of the Independent JPEG Group.",
};

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
    for (size_t i = 0; i < sizeof k_notice / sizeof k_notice[0]; i++) ADD(k_notice[i]);
#undef ADD
    return k;
}

static bool about_frame(app *a, void *st)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
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
    for (size_t i = 0; i < sizeof k_notice / sizeof k_notice[0]; i++) {
        const char *s = k_notice[i];
        if (!s[0]) ui_layout_space(ui, 6.0f);
        else ui_text_wrapped(ui, s, UI_LABEL_SMALL);
    }
    ui_layout_pop(ui);
    ui_scroll_end(ui);
    (void)p;
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

static void cmd_docs(app *a, const app_cmd *c)
{
    const char *exe = pal_dir(PAL_DIR_EXE);
    char local[1100];
    (void)c;
    local[0] = '\0';
    if (exe) {
        char docs[1024];
        pal_path_join(docs, sizeof docs, exe, "docs");
        pal_path_join(local, sizeof local, docs, "index.html");
    }
    if (local[0] && pal_file_exists(local)) m_open_url(a, local);
    else m_open_url(a, M_URL_DOCS);
}

static void cmd_link(app *a, const app_cmd *c)
{
    m_open_url(a, c->arg == 0 ? M_URL_HOME : M_URL_SEARCH);
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
    reg(a, "help.docs", "Documentation", UI_ICON_HELP, nc, cmd_docs, NULL, 0);
    reg(a, "help.website", "Website", UI_ICON_NONE, nc, cmd_link, NULL, 0);
    reg(a, "help.search", "Search", UI_ICON_NONE, nc, cmd_link, NULL, 1);
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
