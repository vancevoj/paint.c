# plugins/: the official optional plugins

Each folder here is one paint.c effect plugin that ships as a separate
download (release `plugins-v<version>`), not inside paintc. Users: see
docs/PLUGINS.md (installing, the list of plugins, writing your own). This
page is the checklist for adding or changing one.

## A plugin folder

| File | Required | What |
|---|---|---|
| `CMakeLists.txt` | yes | `pc_add_plugin(<name> <sources...>)`; `<name>` is the folder name |
| `*.c`, `*.h` | yes | C17 against `include/fx` only (`fx_abi.h`, optional `fx_util.h`, `fx_widgets.h`); no paint.c code, no other library |
| `README.md` | yes | starts with the card below; what it does, its controls, install, Credits |
| `screenshot.png` | yes, for a release | the dialog over a test image (`packaging/plugins/screenshot.sh`) |
| `LICENSE-original.txt` | when code was ported | the license text of the original source |

`pc_add_plugin` (plugins/CMakeLists.txt) builds a MODULE without the `lib`
prefix into `<build>/plugins/out/<name>/<name>.so` (`.dll`, `.dylib`),
hidden visibility, the project's warnings (`-Werror`), and copies
README.md, screenshot.png and LICENSE-original.txt next to it. That folder
is exactly what users install and what the release zip holds. Every folder
with a CMakeLists.txt is picked up automatically; `cmake --build build
--target plugins` builds them all (`PC_BUILD_PLUGINS=OFF` skips them).

## The code

* Export `fx_entry` (required), `fx_abi_version` (return `FX_ABI_VERSION`)
  and `fx_plugin_info`: `"author"` like `"paint.c, after the <Original>
  plugin by <author>"`, `"version"` the same as the card's version (1.0
  equals 1.0.0; `test_plg_all` compares them). Mark them `FX_EXPORT`;
  nothing else is exported.
* Effect ids are unique reverse-DNS names (`org.paintc.<group>.<name>`);
  menu paths follow Paint.NET's submenus (`Effects/Object/...`,
  `Effects/Render/...`, `Adjustments/...`).
* Follow the ABI rules of `fx_abi.h`: write only the ROI, the same output
  for any ROI split and thread count, randomness from the seed parameter
  and pixel coordinates, `FX_CHECK_CANCEL(host, job)` at least once per row
  (also in long `prepare` loops), memory through `host->alloc`/`host->free`.
* Use `host->notice` (after checking `host->size`) when the effect has
  nothing to do, and `FXP_F_PREVIEW_ONLY` for preview aids (ADR-024).

## The README card

The first lines of README.md, an HTML comment GitHub does not show.
`packaging/plugins/plugin_meta.py` reads it to fill the plugin tables of
docs/PLUGINS.md and the top-level README.md and to name the release zips:

```markdown
<!-- paintc-plugin
name: Align Object
version: 1.0.0
menu: Effects > Object > Align Object
summary: Moves the object on a transparent layer to an edge, a corner or the center of the canvas or the selection.
original: Align Object by xod, helped by MJW
original-url: https://forums.paint.net/topic/112095-align-object/
basis: clean room
-->
```

| Key | Required | Meaning |
|---|---|---|
| `name` | yes | display name, as in the menu |
| `summary` | yes | one sentence: what it does |
| `original` | yes | the original Paint.NET plugin and its author(s), the credit shown in the tables |
| `menu` | recommended | where it appears |
| `original-url` | recommended | the original's forum page or source |
| `version` | no | default: `version` of packaging/plugins/release.conf; the zip is `paintc-plugin-<name>-<version>-<platform>.zip` |
| `basis` | no | `clean room` (default) or `source (<license>)` for a port of permissively licensed source |

A value may continue on indented lines. Without a card the values are
guessed from the title, the first paragraph and the Credits section, with a
warning. Write no em or en dashes (house style).

## Credits and legal rules

Every README has a Credits section naming the original plugin, its
author(s) and where it was published, and says how this version was made.
Never decompile, disassemble or inspect the code of a Paint.NET plugin
binary or of Paint.NET itself (P-01, X-01); never load Paint.NET DLLs
(X-04); never reuse their icons, images or text (P-02). Work from the public
description (documentation, forum posts, screenshots, help text, videos).
Source code may be used only when its author published it under a license
that permits reuse (MIT, BSD, Apache, zlib, public domain or an explicit
permission): record the license and URL in the card (`basis`,
`original-url`) and put the license text in LICENSE-original.txt.

## Tests

`test_plg_all` (tests/plugins/test_plg_all.c) covers every plugin without
any work from its lane: it loads all of `<build>/plugins/out` through the
real plugin loader (no Plugin Errors entry, no id clash with built-in
effects or other plugins), checks the exports and the version, and runs
every effect with default parameters on 256 x 256 photo, selection,
transparent and object images: no crash, deterministic, identical for
every ROI split and thread count, writes only inside its ROI, observes
cancellation, under 2 s per render in optimized builds
(`tests/plugins/plg_test_util.h`).

Plugin-specific tests go into `tests/plugins/test_plg_<name>.c`, registered
by a file of their own, `tests/plugins/plg_<name>.cmake` (included
automatically, so no lane edits a shared CMake file):

```cmake
# tests/plugins/plg_my_plugin.cmake
pc_plugin_test(my_plugin test_plg_my_plugin.c)
```

`pc_plugin_test(<name> <test source> [<more sources>...])` builds one
executable and CTest entry (`--quick`, label `plugins`), after the plugin,
linked with the real loader (pc_app) and the effect runner (pc_fx), with
`tests/`, `tests/fx` and `tests/plugins` on the include path and these
definitions: `PC_PLUGIN_SLUG`, `PC_PLUGIN_PATH` (the built library),
`PC_PLUGIN_DIR` and `PC_PLUGIN_SOURCE_DIR` (plugins/<name>). It is skipped
in headless builds and when the plugin is not built.

```c
/* tests/plugins/test_plg_my_plugin.c */
#include "plg_test_util.h"

static void t_strong(void)
{
    plg_set s;
    if (plg_open(&s, PC_PLUGIN_PATH)) {               /* real loader, built-ins first */
        const fx_effect *fx = plg_find(&s, "org.paintc.render.my_effect");
        void *p = fx ? plg_params(fx) : NULL;         /* defaults, palette resolved */
        if (p) {
            CHECK(fx_param_set(fx, p, "radius", 40) == PC_OK);
            plg_check_effect(fx, p, "radius 40");     /* the generic checks */
            /* ...pixel checks of your own: plg_image(PLG_OBJECT, 4),
             * fx_run_sync, fxt_at (tests/fx/fx_test_util.h) */
        }
        fx_params_free(p);
    }
    plg_close(&s);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_strong);
    return pc_test_finish();
}
```

Run them with `ctest --test-dir build -L plugins`. A test may also compile
the plugin's source into itself (as tests/fx/test_fx2_align_object.c does)
to reach internal functions.

## Screenshot, docs and packaging

```sh
# screenshot.png: the dialog over a test image (object or photo), headless;
# optional extra script lines (clicks in window coordinates of 1600 x 1000)
packaging/plugins/screenshot.sh build/src/app/paintc build/plugins/out/my_plugin \
    org.paintc.render.my_effect plugins/my_plugin/screenshot.png photo
# the plugin tables of docs/PLUGINS.md and README.md (CTest test_plg_docs
# fails while they are stale)
python3 packaging/plugins/plugin_meta.py write
# the release zips (docs/PLUGINS.md, Packaging and releasing)
packaging/plugins/build-plugins.sh /tmp/plg-linux dist-plugins linux-x86_64
packaging/plugins/build-plugins.sh /tmp/plg-win dist-plugins windows-x64
```
