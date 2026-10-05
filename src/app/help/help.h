/* help.h - lane UIB (wave 4 items 4 to 6): the user guide that ships inside
 * paint.c.
 *
 * The guide's sources (the Markdown pages in docs/help, the plugin example
 * and the plugin headers) are embedded at build time (src/app/CMakeLists.txt).
 * When the user opens a Help item, every page is rendered to HTML on the main thread
 * (help_md.h; directives fill in this copy's key map, tools, effects, file
 * formats and folders), a worker writes the files into the help folder
 * (unchanged files are not rewritten), and the page opens in the browser
 * through its file:// URL (pal_open_url). Every file holds the whole guide
 * with its own page shown (help_pages.c), so links between pages work even
 * when a sandbox hands the browser only that one file. Nothing needs the
 * network, so the Help menu never leads to a dead page.
 *
 * Help folder: <settings folder>/help with --config-dir, otherwise the
 * per-user cache folder's "help". Headless apps without a settings folder
 * (tests) only record the URL (m_last_url) and write nothing, like
 * m_open_url never starts a browser for them.
 *
 * Thread rules: app_help_build, app_help_open, app_help_dir and the page
 * list run on the main thread; app_help_write and app_help_file_url on any
 * thread. Ownership: an app_help_set owns its files (app_help_set_free);
 * every other argument is borrowed.
 */
#ifndef APP_HELP_H
#define APP_HELP_H

#include <stdbool.h>
#include <stddef.h>

#include "app/app.h"
#include "help_md.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_help_file {
    char     name[64];      /* file name inside the help folder */
    help_buf data;
} app_help_file;

typedef struct app_help_set {
    app_help_file *f;
    int            n, cap;
} app_help_set;

/* Guide pages ("index", "tools", ...; file name without ".html") in
 * navigation order. */
int         app_help_page_count(void);
const char *app_help_page_name(int i);          /* NULL when out of range */

/* Every file of the guide for a: one page per source, search.html and the
 * plugin example files. false on OOM (out may hold some files; free it). */
bool app_help_build(app *a, app_help_set *out);
void app_help_set_free(app_help_set *s);

/* Write the files into dir (created when missing). Files whose content is
 * already there are left alone. false when a file cannot be written. */
bool app_help_write(const app_help_set *s, const char *dir);

/* The help folder of a; false when there is none. */
bool app_help_dir(const app *a, char *out, size_t cap);

/* "file:///..." URL of an absolute path (percent-encoded, '/' separators,
 * "file:///C:/..." for Windows drive paths). false when it does not fit. */
bool app_help_file_url(const char *path, char *out, size_t cap);

/* Open page (a name of the list above, or "search") in the browser:
 * build now, write on a worker, open when written (m_open_url; an error box
 * when the folder cannot be written). false when nothing was started. */
bool app_help_open(app *a, const char *page);

#ifdef __cplusplus
}
#endif

#endif /* APP_HELP_H */
