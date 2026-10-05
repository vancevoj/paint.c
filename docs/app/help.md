# The user guide inside paint.c (lane UIB, wave 4)

Help > Documentation (F1), Search (Ctrl+E), Tutorials and Plugins open a
user guide that ships inside the executable. The project repository is
private, its wiki is empty and Discussions are off, so the earlier links to
the wiki and the discussions led to missing pages (final verification items
4 to 6).

| Part | Where |
|---|---|
| Guide sources (Markdown, paint.c's own wording) | `docs/help/*.md` |
| Plugin example shown and linked by the Plugins page | `docs/help/fade_plugin.c` |
| Embedding (with `include/fx/fx_abi.h` and `fx_util.h`) | `src/app/CMakeLists.txt`, lane UIB block |
| Markdown subset to HTML | `src/app/help/help_md.c` / `.h` |
| Pages, search page, directives, writing, opening | `src/app/help/help_pages.c`, `help.h` |
| Help commands | `src/app/mods/mod_help.c` |
| Tests | `tests/app/test_uib_help.c` |

## How a page opens

1. The command calls `app_help_open(a, page)`.
2. On the main thread every Markdown source is rendered to HTML. Directives
   fill in what depends on the running program: `{{shortcuts}}` (the key
   map, from the command registry), `{{tool_keys}}`, `{{adjustments}}`,
   `{{effects}}` (the effect registry, plugins included), `{{formats}}` (the
   codec registry), `{{blend_modes}}`, `{{plugin_example}}`,
   `{{getting_help}}`, and inline `{{ctrl}}` / `{{alt}}` (Cmd / Option on
   macOS), `{{version}}`, `{{plugin_dir}}`, `{{exe_plugin_dir}}`,
   `{{config_dir}}`, `{{help_dir}}`.
3. Every file holds the whole guide: one `<section class="page">` per source
   page plus the search page, with the file's own page marked `current` (the
   only one shown, no script needed). Links to other pages are plain links
   to their files; a small script switches sections in place instead (and
   searches an index of every section), so the guide keeps working when a
   sandbox such as the Flatpak document portal hands the browser only the
   one file that was opened, and no URL needs a `#fragment` (Windows drops
   fragments of `file://` URLs it opens). Heading ids carry the page name
   (`help_md_render_page`: "tools-selection-tools-s"), so they stay unique.
   Light and dark colors follow `prefers-color-scheme`; nothing is loaded
   from the network.
4. A worker writes the files into the help folder: `<--config-dir>/help`, or
   the per-user cache folder's `help`. Files that are already up to date are
   not rewritten.
5. The page opens through its `file://` URL (`m_open_url`, `pal_open_url`).
   Headless apps without a settings folder (tests) only record the URL.

## Adding or changing a page

Add or edit a `.md` file in `docs/help`; CMake embeds every file there and
the navigation follows `k_order` in `help_pages.c` (unlisted pages come
last, by name). Links between pages use the `.md` names (`[Tools](tools.md)`)
and become `.html` links. Supported Markdown: ATX headings, paragraphs,
one-level lists, quotes, fenced code, pipe tables, rules, `code`,
**strong**, *emphasis*, links and autolinks. `test_uib_help` checks that
every page builds, that every local link and anchor resolves and that no
directive is left unresolved.

## Forum

`-DPC_PROJECT_PUBLIC=ON` (CMake option, default OFF) registers Help > Forum,
which opens the repository's issue list. While the repository is private
the command is not registered, so the item stays hidden (menu.c lists it as
`MI_OPT`), and the troubleshooting page says why.
