/* help_md.h - lane UIB (wave 4 items 4 to 6): the Markdown subset of the
 * bundled user guide (the Markdown pages in docs/help) turned into HTML.
 *
 * Blocks: ATX headings (# to ######, each gets an id made from its text,
 * unique within the page), paragraphs, "-" / "*" and "1." lists (one level;
 * indented lines continue an item), "> " quotes, ``` fenced code, pipe
 * tables with a "|---|" separator row (":" sets the alignment), "---"
 * rules, and directive lines "{{name}}" whose HTML a callback supplies.
 * Inline: `code`, **strong**, *emphasis*, [text](url) links (relative
 * "page.md#anchor" targets become "page.html#anchor"), <https://...>
 * autolinks, backslash escapes and "{{name}}" directives whose plain text a
 * callback supplies (also inside code spans). Everything else is text and
 * is HTML escaped; "javascript:" and "data:" link targets are dropped.
 *
 * Besides the HTML the converter records the page title (the first level 1
 * heading) and the plain text of every section (a heading and what follows
 * it) for the search page.
 *
 * Robustness: input of any bytes is accepted (at most HELP_MD_MAX_INPUT
 * bytes); output is well formed (every element opened is closed) and
 * bounded by HELP_MD_MAX_OUTPUT. No recursion (P-07): one pass over the
 * lines, one pass per inline run. Sizes are checked (P-08).
 *
 * Thread rules: pure functions on caller memory, any thread. Ownership:
 * help_buf and help_doc own their memory (free with help_buf_free and
 * help_doc_free); inputs are borrowed.
 */
#ifndef HELP_MD_H
#define HELP_MD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HELP_MD_MAX_INPUT  ((size_t)4 << 20)
#define HELP_MD_MAX_OUTPUT ((size_t)64 << 20)

/* A growable byte buffer, always NUL terminated once it holds anything.
 * After a failed append (out of memory or over HELP_MD_MAX_OUTPUT) failed
 * stays set and later appends do nothing. */
typedef struct help_buf {
    char  *s;
    size_t n, cap;
    bool   failed;
} help_buf;

void help_buf_free(help_buf *b);
void help_buf_put(help_buf *b, const char *s, size_t n);
void help_buf_puts(help_buf *b, const char *s);
void help_buf_putc(help_buf *b, char c);
/* s[0..n) with & < > " ' escaped for HTML text and attribute values. */
void help_buf_esc(help_buf *b, const char *s, size_t n);
void help_buf_escs(help_buf *b, const char *s);

typedef struct help_section {
    char     anchor[96];    /* heading id, "" for text before the first heading */
    char     title[192];    /* plain text of the heading */
    int      level;         /* 1..6, 0 for the start of the page */
    help_buf text;          /* plain text of the section, words separated by spaces */
} help_section;

typedef struct help_doc {
    help_buf      html;     /* the body fragment */
    char          title[192];
    help_section *sec;
    int           nsec, cap;
} help_doc;

/* Directive callback. block true: name stood alone on a line; append HTML
 * to out. block false: an inline directive; append plain UTF-8 text (the
 * converter escapes it). Return false for unknown names (nothing is
 * inserted). */
typedef bool (*help_directive_fn)(void *ud, const char *name, bool block, help_buf *out);

/* Convert md[0..n) into out (zeroed by the caller or freshly freed).
 * fn may be NULL. Returns false only when memory or the limits ran out;
 * out is still well formed up to that point and must be freed. */
bool help_md_render(const char *md, size_t n, help_directive_fn fn, void *ud, help_doc *out);
/* The same for one page of a guide whose pages share one HTML document:
 * heading ids become "<page>-<id>" (so they stay unique across pages) and
 * link targets follow ("tools.md#tips" -> "tools.html#tools-tips", "#tips"
 * -> "#<page>-tips"). page: [a-z0-9_-], under 40 bytes; NULL = plain. */
bool help_md_render_page(const char *md, size_t n, const char *page, help_directive_fn fn,
                         void *ud, help_doc *out);
void help_doc_free(help_doc *d);

/* The id a heading with this plain text gets ("Getting started" ->
 * "getting-started"): ASCII letters and digits lowercased, runs of other
 * characters as one '-', at most cap - 1 bytes; "section" when empty. */
void help_md_slug(const char *text, size_t n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HELP_MD_H */
