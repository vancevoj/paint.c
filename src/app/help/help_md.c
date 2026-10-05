/* help_md.c - lane UIB (wave 4 items 4 to 6): Markdown subset to HTML for
 * the bundled user guide (see help_md.h). */
#include "help_md.h"

#include "pc/pc_base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- buffers --------------------------------------------------------------------------- */
void help_buf_free(help_buf *b)
{
    if (!b) return;
    free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
    b->failed = false;
}

static bool buf_reserve(help_buf *b, size_t extra)
{
    size_t need, nc;
    char *ns;
    if (b->failed) return false;
    if (!pc_add_size(b->n, extra, &need) || !pc_add_size(need, 1u, &need) ||
        need > HELP_MD_MAX_OUTPUT) {
        b->failed = true;
        return false;
    }
    if (need <= b->cap) return true;
    nc = b->cap ? b->cap : 256u;
    while (nc < need) {
        if (!pc_mul_size(nc, 2u, &nc)) {
            b->failed = true;
            return false;
        }
    }
    if (nc > HELP_MD_MAX_OUTPUT + 1u) nc = HELP_MD_MAX_OUTPUT + 1u;
    ns = (char *)realloc(b->s, nc);
    if (!ns) {
        b->failed = true;
        return false;
    }
    b->s = ns;
    b->cap = nc;
    return true;
}

void help_buf_put(help_buf *b, const char *s, size_t n)
{
    if (!b || (!s && n) || !buf_reserve(b, n)) return;
    if (n) memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

void help_buf_puts(help_buf *b, const char *s)
{
    if (s) help_buf_put(b, s, strlen(s));
}

void help_buf_putc(help_buf *b, char c) { help_buf_put(b, &c, 1u); }

void help_buf_esc(help_buf *b, const char *s, size_t n)
{
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        const char *e = NULL;
        switch (s[i]) {
        case '&': e = "&amp;"; break;
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '"': e = "&quot;"; break;
        case '\'': e = "&#39;"; break;
        case '\0': e = ""; break;
        default: break;
        }
        if (!e) continue;
        help_buf_put(b, s + run, i - run);
        help_buf_puts(b, e);
        run = i + 1u;
    }
    help_buf_put(b, s + run, n - run);
}

void help_buf_escs(help_buf *b, const char *s)
{
    if (s) help_buf_esc(b, s, strlen(s));
}

/* ---- small helpers ------------------------------------------------------------------- */
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_punct(char c) { return c > 0x20 && c < 0x7F && !is_alnum(c); }

static void trim(const char **s, size_t *n)
{
    while (*n && is_space(**s)) {
        (*s)++;
        (*n)--;
    }
    while (*n && is_space((*s)[*n - 1u])) (*n)--;
}

static size_t indent_of(const char *l, size_t n)
{
    size_t k = 0, w = 0;
    while (k < n && (l[k] == ' ' || l[k] == '\t')) {
        w += l[k] == '\t' ? 4u : 1u;
        k++;
    }
    return w;
}

static size_t skip_ws(const char *l, size_t n)
{
    size_t k = 0;
    while (k < n && (l[k] == ' ' || l[k] == '\t')) k++;
    return k;
}

static bool is_blank(const char *l, size_t n) { return skip_ws(l, n) == n; }

static bool ieq_prefix(const char *s, size_t n, const char *p)
{
    size_t k = strlen(p);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != p[i]) return false;
    }
    return true;
}

/* Copy at most cap - 1 bytes of s, cut at a UTF-8 character boundary. */
static void copy_utf8(char *out, size_t cap, const char *s, size_t n)
{
    if (!cap) return;
    if (n >= cap) {
        n = cap - 1u;
        while (n > 0u && ((unsigned char)s[n] & 0xC0u) == 0x80u) n--;
    }
    if (n) memcpy(out, s, n);
    out[n] = '\0';
}

void help_md_slug(const char *text, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    bool dash = false;
    if (!out || cap == 0u) return;
    for (size_t i = 0; text && i < n && k + 1u < cap; i++) {
        char c = text[i];
        if (is_alnum(c)) {
            if (dash && k > 0u && k + 2u < cap) out[k++] = '-';
            dash = false;
            out[k++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        } else {
            dash = true;
        }
    }
    out[k] = '\0';
    if (k == 0u) copy_utf8(out, cap, "section", 7u);
}

/* ---- the converter ------------------------------------------------------------------- */
enum { B_NONE = 0, B_PARA, B_UL, B_OL, B_QUOTE, B_TABLE, B_CODE };
enum { T_STRONG = 0, T_EM, T_A };

#define MAX_TAGS 8
#define MAX_COLS 32

typedef struct tag_ent {
    int         kind;
    const char *url;        /* T_A: the link target (into the source) */
    size_t      url_n;
} tag_ent;

typedef struct conv {
    help_doc         *doc;
    help_directive_fn fn;
    void             *ud;
    help_buf          item;        /* text of the open paragraph, item or quote */
    uint64_t         *ids;         /* hashes of the ids given (open addressing) */
    size_t            nids, ids_cap;
    bool              ids_failed;
    int               block;
    bool              list_blank;
    int               cur;         /* current section */
    int               ncols;       /* open table */
    int               align[MAX_COLS];
} conv;

static help_section *section(conv *c) { return &c->doc->sec[c->cur]; }

static bool new_section(conv *c, const char *anchor, const char *title, size_t title_n, int level)
{
    help_doc *d = c->doc;
    help_section *s;
    if (d->nsec == d->cap) {
        int nc = d->cap ? d->cap * 2 : 16;
        size_t bytes;
        help_section *ns;
        if (nc > 100000 || !pc_mul_size((size_t)nc, sizeof *ns, &bytes)) return false;
        ns = (help_section *)realloc(d->sec, bytes);
        if (!ns) return false;
        d->sec = ns;
        d->cap = nc;
    }
    s = &d->sec[d->nsec];
    memset(s, 0, sizeof *s);
    copy_utf8(s->anchor, sizeof s->anchor, anchor, strlen(anchor));
    copy_utf8(s->title, sizeof s->title, title, title_n);
    s->level = level;
    c->cur = d->nsec++;
    return true;
}

/* Plain text for the search index: whitespace runs become one space. */
static void plain_put(conv *c, const char *s, size_t n)
{
    help_buf *t = &section(c)->text;
    for (size_t i = 0; i < n; i++) {
        char ch = is_space(s[i]) ? ' ' : s[i];
        if (ch == '\0') continue;
        if (ch == ' ' && (t->n == 0u || t->s[t->n - 1u] == ' ')) continue;
        help_buf_putc(t, ch);
    }
}

static void plain_sep(conv *c) { plain_put(c, " ", 1u); }

/* Plain text of HTML (tags dropped, the five entities decoded). */
static void plain_from_html(conv *c, const char *h, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (h[i] == '<') {
            while (i < n && h[i] != '>') i++;
            i++;
            plain_sep(c);
            continue;
        }
        if (h[i] == '&') {
            static const struct { const char *e; char c; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
                { "&#39;", '\'' }
            };
            bool done = false;
            for (size_t k = 0; k < sizeof ents / sizeof ents[0] && !done; k++) {
                size_t el = strlen(ents[k].e);
                if (n - i >= el && memcmp(h + i, ents[k].e, el) == 0) {
                    plain_put(c, &ents[k].c, 1u);
                    i += el;
                    done = true;
                }
            }
            if (done) continue;
        }
        plain_put(c, h + i, 1u);
        i++;
    }
}

static bool valid_name(const char *s, size_t n)
{
    if (n == 0u || n > 47u) return false;
    for (size_t i = 0; i < n; i++)
        if (!(is_digit(s[i]) || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_')) return false;
    return true;
}

/* "{{name}}" at s[0..n): the length of the whole directive and the name, or 0. */
static size_t directive_at(const char *s, size_t n, char *name, size_t cap)
{
    size_t k;
    if (n < 5u || s[0] != '{' || s[1] != '{') return 0;
    for (k = 2; k < n && k < 52u; k++) {
        if (s[k] == '}') {
            if (k + 1u < n && s[k + 1u] == '}' && valid_name(s + 2, k - 2u) && k - 2u < cap) {
                memcpy(name, s + 2, k - 2u);
                name[k - 2u] = '\0';
                return k + 2u;
            }
            return 0;
        }
    }
    return 0;
}

/* Text: escaped into html, raw into the search text. */
static void put_text(conv *c, help_buf *html, const char *s, size_t n)
{
    help_buf_esc(html, s, n);
    plain_put(c, s, n);
}

/* An inline directive's text, escaped. */
static bool put_directive(conv *c, help_buf *html, const char *name)
{
    help_buf tmp;
    bool ok;
    memset(&tmp, 0, sizeof tmp);
    ok = c->fn && c->fn(c->ud, name, false, &tmp);
    if (ok && tmp.n) put_text(c, html, tmp.s, tmp.n);
    help_buf_free(&tmp);
    return ok;
}

/* Code span text: escaped, with inline directives expanded. */
static void put_code(conv *c, help_buf *html, const char *s, size_t n)
{
    size_t i = 0, run = 0;
    while (i < n) {
        char name[48];
        size_t dl = s[i] == '{' ? directive_at(s + i, n - i, name, sizeof name) : 0u;
        if (dl) {
            put_text(c, html, s + run, i - run);
            (void)put_directive(c, html, name);
            i += dl;
            run = i;
            continue;
        }
        i++;
    }
    put_text(c, html, s + run, n - run);
}

static void put_href(help_buf *html, const char *u, size_t n)
{
    size_t k = 0, hash, path_end;
    bool scheme = false;
    trim(&u, &n);
    while (k < n && !is_space(u[k])) k++;          /* drop a "title" part */
    n = k;
    while (n && (unsigned char)u[0] <= 0x20u) {
        u++;
        n--;
    }
    if (ieq_prefix(u, n, "javascript:") || ieq_prefix(u, n, "data:") ||
        ieq_prefix(u, n, "vbscript:") || n == 0u) {
        help_buf_puts(html, "#");
        return;
    }
    for (k = 0; k < n && u[k] != '/' && u[k] != '?' && u[k] != '#'; k++)
        if (u[k] == ':') scheme = true;
    for (hash = 0; hash < n && u[hash] != '#'; hash++) {}
    path_end = hash;
    if (!scheme && path_end >= 3u && memcmp(u + path_end - 3u, ".md", 3u) == 0) {
        help_buf_esc(html, u, path_end - 3u);
        help_buf_puts(html, ".html");
        help_buf_esc(html, u + path_end, n - path_end);
        return;
    }
    help_buf_esc(html, u, n);
}

typedef struct tag_stack {
    tag_ent t[MAX_TAGS];
    int     n;
} tag_stack;

static void open_tag(help_buf *html, tag_stack *st, int kind, const char *url, size_t url_n)
{
    if (st->n >= MAX_TAGS) return;
    st->t[st->n].kind = kind;
    st->t[st->n].url = url;
    st->t[st->n].url_n = url_n;
    st->n++;
    if (kind == T_STRONG) {
        help_buf_puts(html, "<strong>");
    } else if (kind == T_EM) {
        help_buf_puts(html, "<em>");
    } else {
        help_buf_puts(html, "<a href=\"");
        put_href(html, url, url_n);
        help_buf_puts(html, "\">");
    }
}

static void emit_close(help_buf *html, int kind)
{
    help_buf_puts(html, kind == T_STRONG ? "</strong>" : kind == T_EM ? "</em>" : "</a>");
}

static bool has_tag(const tag_stack *st, int kind)
{
    for (int i = 0; i < st->n; i++)
        if (st->t[i].kind == kind) return true;
    return false;
}

/* Close kind; tags opened after it are closed first and opened again. */
static void close_tag(help_buf *html, tag_stack *st, int kind)
{
    tag_ent above[MAX_TAGS];
    int na = 0, at = -1;
    for (int i = st->n - 1; i >= 0; i--)
        if (st->t[i].kind == kind) { at = i; break; }
    if (at < 0) return;
    for (int i = st->n - 1; i > at; i--) {
        emit_close(html, st->t[i].kind);
        above[na++] = st->t[i];
    }
    emit_close(html, kind);
    st->n = at;
    for (int i = na - 1; i >= 0; i--)
        open_tag(html, st, above[i].kind, above[i].url, above[i].url_n);
}

/* Inline Markdown s[0..n) into html (and the search text). */
#define LINK_TEXT_MAX 1000u            /* a "[" without "](" this close is text */
#define LINK_URL_MAX  2000u
#define CODE_RUN_MAX  16u              /* longest backtick run with a failure cache */

static void render_inline(conv *c, help_buf *html, const char *s, size_t n)
{
    tag_stack st;
    size_t i = 0, link_close = (size_t)-1, link_end = 0;
    /* once no closing run of k backticks follows position p, none follows
     * any later position: remember it, so the pass stays linear */
    bool no_close[CODE_RUN_MAX + 1u];
    memset(no_close, 0, sizeof no_close);
    st.n = 0;
    while (i < n) {
        char ch = s[i];
        if (i == link_close) {
            close_tag(html, &st, T_A);
            link_close = (size_t)-1;
            i = link_end;
            continue;
        }
        if (ch == '\\' && i + 1u < n && is_punct(s[i + 1u])) {
            put_text(c, html, s + i + 1u, 1u);
            i += 2u;
            continue;
        }
        if (ch == '`') {
            size_t k = 0, j, close = (size_t)-1;
            while (i + k < n && s[i + k] == '`') k++;
            for (j = i + k; j < n && !(k <= CODE_RUN_MAX && no_close[k]); j++) {
                size_t r = 0;
                if (s[j] != '`') continue;
                while (j + r < n && s[j + r] == '`') r++;
                if (r == k) { close = j; break; }
                j += r - 1u;
            }
            if (close == (size_t)-1 && k <= CODE_RUN_MAX) no_close[k] = true;
            if (close != (size_t)-1 && (link_close == (size_t)-1 || close < link_close)) {
                const char *in = s + i + k;
                size_t inn = close - i - k;
                if (inn >= 2u && in[0] == ' ' && in[inn - 1u] == ' ') {
                    in++;
                    inn -= 2u;
                }
                help_buf_puts(html, "<code>");
                put_code(c, html, in, inn);
                help_buf_puts(html, "</code>");
                i = close + k;
            } else {
                put_text(c, html, s + i, k);
                i += k;
            }
            continue;
        }
        if (ch == '{') {
            char name[48];
            size_t dl = directive_at(s + i, n - i, name, sizeof name);
            if (dl && (link_close == (size_t)-1 || i + dl <= link_close)) {
                (void)put_directive(c, html, name);
                i += dl;
                continue;
            }
        }
        if (ch == '*' && i + 1u < n && s[i + 1u] == '*') {
            if (has_tag(&st, T_STRONG)) close_tag(html, &st, T_STRONG);
            else open_tag(html, &st, T_STRONG, NULL, 0u);
            i += 2u;
            continue;
        }
        if (ch == '*') {
            bool prev_sp = i == 0u || is_space(s[i - 1u]);
            bool next_sp = i + 1u >= n || is_space(s[i + 1u]);
            bool open = has_tag(&st, T_EM);
            if (open ? !prev_sp : !next_sp) {
                if (open) close_tag(html, &st, T_EM);
                else open_tag(html, &st, T_EM, NULL, 0u);
                i++;
                continue;
            }
        }
        if (ch == '[' && link_close == (size_t)-1 && !has_tag(&st, T_A)) {
            size_t j = i + 1u, k, jend = n - i > LINK_TEXT_MAX ? i + LINK_TEXT_MAX : n;
            while (j < jend && s[j] != ']') j += (s[j] == '\\' && j + 1u < jend) ? 2u : 1u;
            if (j + 1u < jend && s[j] == ']' && s[j + 1u] == '(') {
                size_t kend = n - j > LINK_URL_MAX ? j + LINK_URL_MAX : n;
                for (k = j + 2u; k < kend && s[k] != ')' && s[k] != '\n'; k++) {}
                if (k < kend && s[k] == ')') {
                    open_tag(html, &st, T_A, s + j + 2u, k - j - 2u);
                    link_close = j;
                    link_end = k + 1u;
                    i++;
                    continue;
                }
            }
        }
        if (ch == '<' && !has_tag(&st, T_A) &&
            (ieq_prefix(s + i + 1u, n - i - 1u, "https://") ||
             ieq_prefix(s + i + 1u, n - i - 1u, "http://") ||
             ieq_prefix(s + i + 1u, n - i - 1u, "mailto:"))) {
            size_t k = i + 1u;
            while (k < n && s[k] != '>' && !is_space(s[k]) && s[k] != '<') k++;
            if (k < n && s[k] == '>') {
                help_buf_puts(html, "<a href=\"");
                put_href(html, s + i + 1u, k - i - 1u);
                help_buf_puts(html, "\">");
                put_text(c, html, s + i + 1u, k - i - 1u);
                help_buf_puts(html, "</a>");
                i = k + 1u;
                continue;
            }
        }
        put_text(c, html, s + i, 1u);
        i++;
    }
    while (st.n > 0) {
        st.n--;
        emit_close(html, st.t[st.n].kind);
    }
}

/* ---- blocks -------------------------------------------------------------------------- */
static void flush_item(conv *c, const char *open, const char *close)
{
    const char *s = c->item.s ? c->item.s : "";
    size_t n = c->item.n;
    trim(&s, &n);
    if (n) {
        help_buf_puts(&c->doc->html, open);
        render_inline(c, &c->doc->html, s, n);
        help_buf_puts(&c->doc->html, close);
        plain_sep(c);
    }
    c->item.n = 0;
    if (c->item.s) c->item.s[0] = '\0';
}

static void close_block(conv *c)
{
    help_buf *h = &c->doc->html;
    switch (c->block) {
    case B_PARA: flush_item(c, "<p>", "</p>\n"); break;
    case B_UL:
        flush_item(c, "<li>", "</li>\n");
        help_buf_puts(h, "</ul>\n");
        break;
    case B_OL:
        flush_item(c, "<li>", "</li>\n");
        help_buf_puts(h, "</ol>\n");
        break;
    case B_QUOTE:
        flush_item(c, "<blockquote><p>", "</p></blockquote>\n");
        c->item.n = 0;
        break;
    case B_TABLE: help_buf_puts(h, "</tbody></table>\n"); break;
    case B_CODE: help_buf_puts(h, "</code></pre>\n"); break;
    default: break;
    }
    c->block = B_NONE;
    c->list_blank = false;
    c->item.n = 0;
}

/* Add a line to the open paragraph, item or quote (leading blanks of a
 * continuation line dropped). */
static void item_add(conv *c, const char *s, size_t n)
{
    size_t k = skip_ws(s, n);
    if (c->item.n) help_buf_putc(&c->item, '\n');
    help_buf_put(&c->item, s + k, n - k);
}

static bool fence_at(const char *l, size_t n, size_t *after)
{
    size_t k = skip_ws(l, n);
    if (indent_of(l, n) > 3u || n - k < 3u || memcmp(l + k, "```", 3u) != 0) return false;
    *after = k + 3u;
    while (*after < n && l[*after] == '`') (*after)++;
    return true;
}

static int heading_at(const char *l, size_t n, const char **text, size_t *tn)
{
    size_t k = skip_ws(l, n), h = 0;
    if (indent_of(l, n) > 3u) return 0;
    while (k + h < n && l[k + h] == '#') h++;
    if (h == 0u || h > 6u || (k + h < n && l[k + h] != ' ' && l[k + h] != '\t')) return 0;
    *text = l + k + h;
    *tn = n - k - h;
    trim(text, tn);
    while (*tn && (*text)[*tn - 1u] == '#') (*tn)--;       /* closing #s */
    trim(text, tn);
    return (int)h;
}

static bool rule_at(const char *l, size_t n)
{
    size_t count = 0;
    char mark = 0;
    if (indent_of(l, n) > 3u) return false;
    for (size_t i = 0; i < n; i++) {
        if (l[i] == ' ' || l[i] == '\t') continue;
        if (l[i] != '-' && l[i] != '*' && l[i] != '_') return false;
        if (mark && l[i] != mark) return false;
        mark = l[i];
        count++;
    }
    return count >= 3u;
}

static bool ul_at(const char *l, size_t n, const char **text, size_t *tn)
{
    size_t k = skip_ws(l, n);
    if (indent_of(l, n) > 3u || k + 1u >= n) return false;
    if ((l[k] != '-' && l[k] != '*' && l[k] != '+') || (l[k + 1u] != ' ' && l[k + 1u] != '\t'))
        return false;
    *text = l + k + 2u;
    *tn = n - k - 2u;
    return true;
}

static bool ol_at(const char *l, size_t n, const char **text, size_t *tn)
{
    size_t k = skip_ws(l, n), d = 0;
    if (indent_of(l, n) > 3u) return false;
    while (k + d < n && d < 9u && is_digit(l[k + d])) d++;
    if (d == 0u || k + d + 1u >= n || (l[k + d] != '.' && l[k + d] != ')') ||
        (l[k + d + 1u] != ' ' && l[k + d + 1u] != '\t'))
        return false;
    *text = l + k + d + 2u;
    *tn = n - k - d - 2u;
    return true;
}

static bool quote_at(const char *l, size_t n, const char **text, size_t *tn)
{
    size_t k = skip_ws(l, n);
    if (indent_of(l, n) > 3u || k >= n || l[k] != '>') return false;
    k++;
    if (k < n && l[k] == ' ') k++;
    *text = l + k;
    *tn = n - k;
    return true;
}

static bool row_at(const char *l, size_t n)
{
    size_t k = skip_ws(l, n);
    return k < n && l[k] == '|';
}

static bool sep_row(const char *l, size_t n)
{
    bool dash = false, bar = false;
    for (size_t i = 0; i < n; i++) {
        char ch = l[i];
        if (ch == '-') dash = true;
        else if (ch == '|') bar = true;
        else if (ch != ':' && ch != ' ' && ch != '\t') return false;
    }
    return dash && bar;
}

/* Cells of a table row: start and length of each (trimmed). */
static int split_cells(const char *l, size_t n, size_t *st, size_t *ln, int max)
{
    size_t i = skip_ws(l, n), start;
    int nc = 0;
    bool code = false;
    while (n > i && is_space(l[n - 1u])) n--;
    if (i < n && l[i] == '|') i++;
    start = i;
    for (; i <= n && nc < max; i++) {
        bool end = i == n;
        if (!end && l[i] == '\\' && i + 1u < n) {
            i++;
            continue;
        }
        if (!end && l[i] == '`') code = !code;
        if (end || (l[i] == '|' && !code)) {
            const char *s = l + start;
            size_t sn = i - start;
            if (end && sn == 0u) break;             /* nothing after the last bar */
            trim(&s, &sn);
            st[nc] = (size_t)(s - l);
            ln[nc] = sn;
            nc++;
            start = i + 1u;
        }
    }
    return nc;
}

static void table_cells(conv *c, const char *l, size_t n, bool head)
{
    size_t st[MAX_COLS], ln[MAX_COLS];
    int nc = split_cells(l, n, st, ln, MAX_COLS);
    help_buf *h = &c->doc->html;
    help_buf_puts(h, "<tr>");
    for (int k = 0; k < c->ncols; k++) {
        static const char *const al[4] = { "", " style=\"text-align:left\"",
                                           " style=\"text-align:center\"",
                                           " style=\"text-align:right\"" };
        help_buf_puts(h, head ? "<th" : "<td");
        help_buf_puts(h, al[c->align[k]]);
        help_buf_puts(h, ">");
        if (k < nc) {
            render_inline(c, h, l + st[k], ln[k]);
            plain_sep(c);
        }
        help_buf_puts(h, head ? "</th>" : "</td>");
    }
    help_buf_puts(h, "</tr>\n");
}

static void start_table(conv *c, const char *head, size_t hn, const char *sep, size_t sn)
{
    size_t st[MAX_COLS], ln[MAX_COLS];
    int ns = split_cells(sep, sn, st, ln, MAX_COLS), nh;
    size_t hst[MAX_COLS], hln[MAX_COLS];
    nh = split_cells(head, hn, hst, hln, MAX_COLS);
    c->ncols = nh > 0 ? nh : 1;
    for (int k = 0; k < c->ncols; k++) {
        int a = 0;
        if (k < ns && ln[k] > 0u) {
            bool left = sep[st[k]] == ':', right = sep[st[k] + ln[k] - 1u] == ':';
            a = left && right ? 2 : right ? 3 : left ? 1 : 0;
        }
        c->align[k] = a;
    }
    help_buf_puts(&c->doc->html, "<table>\n<thead>");
    table_cells(c, head, hn, true);
    help_buf_puts(&c->doc->html, "</thead>\n<tbody>\n");
    c->block = B_TABLE;
}

/* ---- heading ids -------------------------------------------------------------------- */
static uint64_t id_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ull;
    }
    return h ? h : 1u;                        /* 0 marks an empty slot */
}

static bool id_taken(const conv *c, uint64_t h)
{
    size_t mask, k;
    if (!c->ids_cap) return false;
    mask = c->ids_cap - 1u;
    for (k = (size_t)h & mask; c->ids[k]; k = (k + 1u) & mask)
        if (c->ids[k] == h) return true;
    return false;
}

static void id_add(conv *c, uint64_t h)
{
    size_t mask, k;
    if (c->ids_failed || id_taken(c, h)) return;
    if ((c->nids + 1u) * 2u > c->ids_cap) {
        size_t nc = c->ids_cap ? c->ids_cap * 2u : 64u, bytes;
        uint64_t *nt;
        if (!pc_mul_size(nc, sizeof *nt, &bytes) || !(nt = (uint64_t *)calloc(1u, bytes))) {
            c->ids_failed = true;
            return;
        }
        for (size_t i = 0; i < c->ids_cap; i++) {
            if (!c->ids[i]) continue;
            for (k = (size_t)c->ids[i] & (nc - 1u); nt[k]; k = (k + 1u) & (nc - 1u)) {}
            nt[k] = c->ids[i];
        }
        free(c->ids);
        c->ids = nt;
        c->ids_cap = nc;
    }
    mask = c->ids_cap - 1u;
    for (k = (size_t)h & mask; c->ids[k]; k = (k + 1u) & mask) {}
    c->ids[k] = h;
    c->nids++;
}

static void heading(conv *c, int level, const char *text, size_t tn)
{
    help_buf inner;
    help_section *s;
    char slug[96], uniq[112], probe[128];
    memset(&inner, 0, sizeof inner);
    if (!new_section(c, "", "", 0u, level)) {
        c->doc->html.failed = true;
        return;
    }
    render_inline(c, &inner, text, tn);       /* its plain text lands in the new section */
    s = section(c);
    help_md_slug(s->text.s ? s->text.s : "", s->text.n, slug, sizeof slug);
    copy_utf8(uniq, sizeof uniq, slug, strlen(slug));
    /* "id", "id-2", "id-3" ...: a few probes, then the section number */
    for (int k = 2; id_taken(c, id_hash(uniq)); k++) {
        if (k <= 9) snprintf(uniq, sizeof uniq, "%s-%d", slug, k);
        else snprintf(uniq, sizeof uniq, "%s-s%d-%d", slug, c->cur, k);
        if (k > 40) break;                    /* hostile input: accept a repeat */
    }
    id_add(c, id_hash(uniq));
    copy_utf8(s->anchor, sizeof s->anchor, uniq, strlen(uniq));
    copy_utf8(s->title, sizeof s->title, s->text.s ? s->text.s : "", s->text.n);
    help_buf_free(&s->text);                  /* the section text follows the heading */
    if (level == 1 && !c->doc->title[0])
        copy_utf8(c->doc->title, sizeof c->doc->title, s->title, strlen(s->title));
    snprintf(probe, sizeof probe, "<h%d id=\"", level);
    help_buf_puts(&c->doc->html, probe);
    help_buf_escs(&c->doc->html, uniq);
    help_buf_puts(&c->doc->html, "\">");
    help_buf_put(&c->doc->html, inner.s, inner.n);
    snprintf(probe, sizeof probe, "</h%d>\n", level);
    help_buf_puts(&c->doc->html, probe);
    help_buf_free(&inner);
}

static bool directive_line(const char *l, size_t n, char *name, size_t cap)
{
    const char *s = l;
    size_t sn = n;
    trim(&s, &sn);
    return directive_at(s, sn, name, cap) == sn && sn > 0u;
}

bool help_md_render(const char *md, size_t n, help_directive_fn fn, void *ud, help_doc *out)
{
    conv c;
    size_t p = 0;
    bool ok;
    if (!out) return false;
    memset(out, 0, sizeof *out);
    memset(&c, 0, sizeof c);
    c.doc = out;
    c.fn = fn;
    c.ud = ud;
    if (!md) n = 0;
    if (n > HELP_MD_MAX_INPUT) n = HELP_MD_MAX_INPUT;
    if (n >= 3u && (unsigned char)md[0] == 0xEFu && (unsigned char)md[1] == 0xBBu &&
        (unsigned char)md[2] == 0xBFu)
        p = 3u;                                         /* UTF-8 byte order mark */
    if (!new_section(&c, "", "", 0u, 0)) return false;
    help_buf_puts(&out->html, "");
    while (p < n && !out->html.failed) {
        const char *l = md + p, *text = NULL;
        size_t ln = 0, tn = 0, after = 0;
        char name[48];
        int level;
        while (p + ln < n && md[p + ln] != '\n') ln++;
        p += ln + (p + ln < n ? 1u : 0u);
        if (ln && l[ln - 1u] == '\r') ln--;
        if (c.block == B_CODE) {
            if (fence_at(l, ln, &after)) {
                close_block(&c);
            } else {
                help_buf_esc(&out->html, l, ln);
                help_buf_putc(&out->html, '\n');
                plain_put(&c, l, ln);
                plain_sep(&c);
            }
            continue;
        }
        if (is_blank(l, ln)) {
            if (c.block == B_UL || c.block == B_OL) {
                flush_item(&c, "<li>", "</li>\n");
                c.list_blank = true;
            } else if (c.block != B_NONE) {
                close_block(&c);
            }
            continue;
        }
        if (c.list_blank) {
            bool same = (c.block == B_UL && ul_at(l, ln, &text, &tn) && !rule_at(l, ln)) ||
                        (c.block == B_OL && ol_at(l, ln, &text, &tn));
            c.list_blank = false;
            if (!same) close_block(&c);
        }
        if (fence_at(l, ln, &after)) {
            const char *lang = l + after;
            size_t langn = ln - after;
            close_block(&c);
            trim(&lang, &langn);
            help_buf_puts(&out->html, "<pre><code");
            if (langn && valid_name(lang, langn)) {
                help_buf_puts(&out->html, " class=\"language-");
                help_buf_esc(&out->html, lang, langn);
                help_buf_puts(&out->html, "\"");
            }
            help_buf_puts(&out->html, ">");
            c.block = B_CODE;
            continue;
        }
        if (directive_line(l, ln, name, sizeof name)) {
            size_t before;
            close_block(&c);
            before = out->html.n;
            if (fn) (void)fn(ud, name, true, &out->html);
            if (out->html.n > before)
                plain_from_html(&c, out->html.s + before, out->html.n - before);
            continue;
        }
        level = heading_at(l, ln, &text, &tn);
        if (level > 0) {
            close_block(&c);
            heading(&c, level, text, tn);
            continue;
        }
        if (rule_at(l, ln)) {
            close_block(&c);
            help_buf_puts(&out->html, "<hr>\n");
            continue;
        }
        if (c.block == B_TABLE) {
            if (row_at(l, ln)) {
                table_cells(&c, l, ln, false);
                continue;
            }
            close_block(&c);
        }
        if (row_at(l, ln) && p < n) {
            const char *nl = md + p;
            size_t raw = 0, nn;
            while (p + raw < n && md[p + raw] != '\n') raw++;
            nn = raw;
            if (nn && nl[nn - 1u] == '\r') nn--;
            if (sep_row(nl, nn)) {
                close_block(&c);
                start_table(&c, l, ln, nl, nn);
                p += raw + (p + raw < n ? 1u : 0u);
                continue;
            }
        }
        if (ul_at(l, ln, &text, &tn)) {
            if (c.block != B_UL) {
                close_block(&c);
                help_buf_puts(&out->html, "<ul>\n");
                c.block = B_UL;
            } else {
                flush_item(&c, "<li>", "</li>\n");
            }
            item_add(&c, text, tn);
            continue;
        }
        if (ol_at(l, ln, &text, &tn)) {
            if (c.block != B_OL) {
                close_block(&c);
                help_buf_puts(&out->html, "<ol>\n");
                c.block = B_OL;
            } else {
                flush_item(&c, "<li>", "</li>\n");
            }
            item_add(&c, text, tn);
            continue;
        }
        if (quote_at(l, ln, &text, &tn)) {
            if (c.block != B_QUOTE) {
                close_block(&c);
                c.block = B_QUOTE;
            }
            item_add(&c, text, tn);
            continue;
        }
        if (c.block == B_PARA || c.block == B_QUOTE ||
            ((c.block == B_UL || c.block == B_OL) && indent_of(l, ln) >= 2u)) {
            item_add(&c, l, ln);
            continue;
        }
        close_block(&c);
        c.block = B_PARA;
        item_add(&c, l, ln);
    }
    close_block(&c);
    ok = !out->html.failed && !c.item.failed && !c.ids_failed;
    for (int i = 0; i < out->nsec; i++)
        if (out->sec[i].text.failed) ok = false;
    help_buf_free(&c.item);
    free(c.ids);
    return ok;
}

void help_doc_free(help_doc *d)
{
    if (!d) return;
    help_buf_free(&d->html);
    for (int i = 0; i < d->nsec; i++) help_buf_free(&d->sec[i].text);
    free(d->sec);
    memset(d, 0, sizeof *d);
}
