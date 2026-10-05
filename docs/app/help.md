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
3. Every page gets the same frame (navigation in source order, a search box,
   light and dark colors from `prefers-color-scheme`, no external resources).
   `search.html` carries an index of every section and a small script that
   searches it; it works on `file://` pages.
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
