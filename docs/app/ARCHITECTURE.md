# paint.c editor architecture (lane L2/L4, wave 2a)

The editor is the `paintc` executable over two libraries:

| Library | Directory | Contents |
|---|---|---|
| `pc_gfx` | `src/gfx` | Canvas rendering over SDL_Renderer (ADR-003): view math (`gfx_view.h`, pure), page textures mirrored from the `pc_view_cache` (`gfx.h`), marching ants. |
| `pc_app` | `src/app` | Everything else: application, frame loop, commands, menus, document model, canvas widget, tools, panels, dialogs, file flows, settings, scripts. Public headers in `include/app`. |
| `paintc` | `src/app/main.c` | Command line, SDL and pal setup, single instance, `--self-test`, `--script`, `--screenshot`. |

Every `.c` file under `src/app` is compiled into `pc_app` (except `main.c`),
so feature lanes extend the editor by adding files. Nothing in this document
requires editing a shared file.

## Frame loop

```
main: SDL_Init(VIDEO | EVENTS), pal_init, app_create, open argv files, app_run
app_run: SDL_WaitEventTimeout(app_wait_timeout) -> app_event for every event -> app_frame
app_frame:
  pal_pump (dialog results), finished background tasks (done callbacks)
  APP_HOOK_FRAME hooks
  ui_begin_frame
    shell: top row (menus, image list, window toggles), toolbar, tool options,
           canvas (input replay, view, overlay), status bar, panels, dialogs
  ui_end_frame
  unused key presses -> app_key_press (tool keys, command shortcuts, tool letters)
  canvas prepare: pc_view_cache_update of the visible area (time-sliced)
  thumbnails (lazy, throttled)
  SDL_RenderClear, ui_render (the canvas draws in a ui_draw_callback), present
```

Rendering is on demand: `app_wait_timeout` returns -1 when nothing is
animating, so an idle editor sleeps in `SDL_WaitEvent`. Marching ants,
progress bars, background tasks and time-sliced cache updates request frames.
On Windows and macOS an event watch renders on `SDL_EVENT_WINDOW_EXPOSED` so
the window keeps painting during live resize. Quitting (window close,
`SDL_EVENT_QUIT`, File > Exit) runs the unsaved-changes prompts first.

Thread rules: the main thread owns the app, the renderer, every document
and history. Workers (`app_task`, effect jobs, `pc_par` fan-out) only see
owned copies or immutable tiles (savers use `app_doc_snapshot`, which shares
the refcounted tiles).

## Main pieces

| File | Role |
|---|---|
| `app.c` | Lifetime, documents list, colors, preferences, tasks, hooks, extension state, event and frame dispatch, `app_run`. |
| `shell.c` | Main window layout (WINDOWS.md 1): top row, toolbars, status bar, window title. |
| `menu.c` | The menu table mirroring MENUS.md; Adjustments, Effects and Open Recent are generated. |
| `cmd.c` | Command registry, keymap mirroring SHORTCUTS.md, shortcut parsing, keyboard dispatch. |
| `doc.c` | `app_doc`: document, linear history, dirty tracking, active layer, transactions, snapshots, outline. |
| `canvas.c` | Pointer routing (mouse and pen), panning, zooming, scroll bars, rulers, the draw callback, cursors, view-cache time slicing. |
| `overlay.c` | `app_ov_*` drawing for tool handles and outlines. |
| `tool.c` | Tool registry, switching and hotkey cycling, shared tool settings, options bar helpers. |
| `panels.c`, `panels/mod_panels.c` | Panel registry; the Tools, History, Layers and Colors windows. |
| `dlg.c`, `propdlg.c` | Dialog stack, message boxes; the generic fx_prop dialog builder. |
| `fileio.c` | New, Open, Save, Save As, Save Configuration, Flatten prompt, Close, recent files. |
| `thumbs.c` | Image list and layer thumbnails. |
| `settings.c` | The INI-like settings store. |
| `script.c` | `app_script_run` for tests, `--script` and `--self-test`. |
| `tools/` | `stroke.c` (stroke accumulation for painting tools), `tool_pan.c`, `tool_zoom.c`, `tool_pencil.c`, `tool_paintbrush.c`. |
| `mods/` | `mod_file`, `mod_edit`, `mod_view`, `mod_image`, `mod_layers`, `mod_effects`, `mod_help`. |

## Registration from file names

`src/app/CMakeLists.txt` generates two lists at configure time (globs with
`CONFIGURE_DEPENDS`, so a new file is picked up by the next build):

* every `src/app/tools/tool_<name>.c` must define
  `const app_tool app_tool_<name>;` (`app_tool_list.inc`, registered by
  `app_tools_init`, sorted by the `order` field);
* every `src/app/**/mod_<name>.c` must define `void mod_<name>(app *a);`
  (`app_mod_list.inc`, called once by `app_create` in file-name order after
  the built-in tools are registered and before settings are applied).

Helper files must not use these prefixes (for example `tools/stroke.c`).

## Commands, menus and keys

* A command is a string id with a label, icon, flags, optional `enabled`
  and `checked` predicates and a `run` callback (`include/app/app_cmd.h`).
  Ids: `file.*`, `edit.*`, `view.*`, `image.*`, `layers.*`,
  `adjust.<effect id>`, `effects.<effect id>`, `window.<panel id>`,
  `docs.*`, `colors.*`, `tool.*`, `help.*`, `app.settings`.
* `app_cmd_exec` refuses disabled commands, finishes the live tool edit
  first (unless `APP_CMD_NO_COMMIT`) and requests a frame. Menus, toolbar
  buttons, panel buttons and shortcuts all go through it.
* The keymap in `cmd.c` binds documented keys to ids, so registering a
  command is enough to get its SHORTCUTS.md key. A definition's own
  `shortcut` is used only for ids missing from the keymap. Ctrl means Cmd on
  macOS. `Plus` and `Minus` accept the main row and the keypad.
* The menu table in `menu.c` lists every MENUS.md item in order with
  separators. Items whose command is not registered are shown disabled with
  their documented shortcut; a lane that implements e.g. Image > Resize only
  registers `image.resize`. `app_menu_extra` appends items that are not in
  MENUS.md (plugins, paint.c extras).
* Key dispatch (`app_key_press`), for presses no widget used: the active
  tool's `key` callback, then command shortcuts, then tool letters
  (K-TOOLSEL-CYCLE: the 3.36 algorithm, 1 s window, Shift reverses, swallowed
  while a mouse button is down), then Enter / Esc (finish or cancel the live
  tool, else Deselect). Letters, digits and editing keys without Ctrl, Alt or
  Cmd go to a focused text field instead.

## Documents

`app_doc` (`include/app/app_doc.h`) wraps a `pc_doc` and its `pc_hist`:

* Linear history: the History window shows the root..current path plus the
  redo chain; a new action after undo prunes the undone branch
  (`app_doc_history_changed`, call it after any history operation done
  outside `app_doc_txn_commit`). The history byte budget (25 % of RAM, at
  least 1 GiB, OD-10) is applied there too.
* Dirty = the current history node differs from the node at the last save
  or open (`saved_seq`), so undoing back to the saved state is clean.
* Transactions: one per document (`app_doc_txn_begin(a, d, owner, label)`),
  owned by a tool or an effect preview; the canvas composites it live.
* View state per document (zoom, center, fit mode), its own
  `pc_view_cache`, the selection outline cache and thumbnails.
* `app_doc_snapshot` gives workers an independent copy sharing tiles.

## Canvas

* `gfx_view` maps document to screen: zoom, the document point at the view
  center, snapped origin. Presets and stepping follow VIEW.md with the 3.36
  tolerance rule; Ctrl+wheel zooms at the pointer, keys and menus at the
  center; Zoom to Window toggles back; overscroll follows V-OVERSCROLL.
* Display: the visible tiles of mip level `gfx_view_level(zoom)` are brought
  up to date with `pc_view_cache_update` in 2-tile-row bands, at most about
  14 ms per frame (huge zoomed-out views stream in). `gfx_canvas_draw`
  uploads only tiles whose stamp changed into 1024 x 1024 page textures (16 x
  16 tiles), LRU-evicts pages beyond the budget (32 pages), draws the
  checkerboard in screen space aligned to the image, the pages with
  premultiplied blending (nearest at >= 100 % or exact 1:1 mip levels,
  linear otherwise), and the pixel grid at >= 200 %.
* Input: SDL pointer events are queued and replayed in the frame after the
  canvas declared its `ui_interact` region, so panels, popups and dialogs
  always win. A press on the canvas captures the pointer until all buttons
  are up; every motion event reaches the tool. The middle button and
  Space + left drag pan in every tool. Pen events carry pressure and the
  eraser flag; the mouse events SDL synthesizes from pens are ignored.

## How to add a tool

1. Create `src/app/tools/tool_<id>.c` with the canonical id from TOOLS.md
   (`rect_select`, `move_pixels`, `lasso_select`, `move_selection`,
   `ellipse_select`, `magic_wand`, `paint_bucket`, `gradient`, `eraser`,
   `color_picker`, `clone_stamp`, `recolor`, `text`, `line_curve`,
   `shapes`); the Tools window already shows a disabled slot for each.
2. Define `const app_tool app_tool_<id> = { "<id>", "Name", "help", 'L',
   order, UI_ICON_..., APP_CURSOR_..., flags, sizeof(state), ... }` with the
   callbacks you need (all optional): `pointer` (document coordinates,
   buttons, pressure), `key`, `text`, `options` (use `app_opt_*` and
   `app_opt_next` to place widgets in the options bar), `overlay`
   (`app_ov_*`), `live` / `commit` / `cancel` for editable states, and
   `settings_changed` to re-render live edits.
3. Pixels change only through the document transaction:
   `app_doc_txn_begin(a, d, owner, "Name")`, edits with `pc_paint_apply`
   (painting tools: see `tools/stroke.c`) or the `pc_txn_*` functions, then
   `app_doc_txn_commit(a, d)` (one history step) or `app_doc_txn_cancel`.
   Selection tools use the `pc_sel_*` history operations and then call
   `app_doc_history_changed`.
4. Shared options live in `app_tool_settings` (`app_tool_settings_get`);
   tool-specific options go to the settings store with keys
   `tool.<id>.<name>` (`app_settings_of(a)`).
5. Add `tests/app/test_<lane>_<tool>.c` driving it with `app_test_util.h`
   (`at_drag`, `at_doc_px`, `at_pixel`) or with an `app_script_run` script.

## How to add a command

```c
static void run(app *a, const app_cmd *c) { ... }
static bool enabled(app *a, const app_cmd *c) { ... }
void mod_myfeature(app *a)
{
    app_cmd_def d = { 0 };
    d.id = "image.resize";            /* the id the menu table and keymap use */
    d.label = "Resize...";
    d.icon = UI_ICON_RESIZE;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = run;
    d.enabled = enabled;
    app_cmd_register(a, &d);
}
```

in a new file `src/app/<area>/mod_myfeature.c`. Work that changes the
document must call `app_doc_history_changed(a, d)` afterwards (or commit a
transaction with `app_doc_txn_commit`).

## How to add a dialog

Push a frame callback with owned state; the callback declares one frame
(`ui_dialog_begin` ... `ui_dialog_end`) and returns false once closed:

```c
static bool frame(app *a, void *st)
{
    ui_ctx *ui = app_ui(a);
    bool enter;
    uint32_t r;
    ui_dialog_begin(ui, "Resize Image##resize", 420.0f, 0.0f);
    enter = app_dialog_take_enter(a);       /* Enter means OK (K-DLG-ENTER) */
    ... widgets ...
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    if (r == UI_DLG_OK) { ... apply, app_doc_history_changed ... }
    return false;
}
app_dialog_push(a, frame, state, free);
```

Parameter blocks described by `fx_prop` schemas (effects, save options) get
their widgets from `app_props_ui`; custom blob props (`FXP_CUSTOM`, e.g.
"curves", "levels") get a widget through `app_prop_widget_register`.
Message boxes: `app_message`, errors: `app_error`.

## How to add a panel

```c
static void body(app *a, void *ud) { ... widgets in the panel's layout ... }
void mod_palette_panel(app *a)
{
    app_panel_def d = { 0 };
    d.id = "palettes";                /* command window.palettes, settings key */
    d.title = "Palettes";
    d.icon = UI_ICON_PALETTE;
    d.toggle_order = 5;               /* button on the right of the menu bar */
    d.flags = UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE;
    d.def = (ui_panel_state){ 8, 8, 240, 300, UI_ANCHOR_END, UI_ANCHOR_START, true };
    d.body = body;
    app_panel_register(a, &d);
}
```

## Effects and adjustments

`mod_effects.c` registers one command per registry entry. A dialog effect
opens the generic dialog; the active layer is snapshotted, the effect renders
ROIs on pool workers, and finished ROIs are blended through the selection into
the document transaction for the live preview. OK waits for the job and
commits one history step named after the effect; Cancel drops it. Dialog-less
adjustments and Effects > Repeat run immediately with the remembered
parameters. Wave 2b refines the previews and adds the custom widgets.

## State, hooks and extension points

* `app_ext_set(a, "key", ptr, destroy)` / `app_ext_get` keep per-app module
  state without touching the app struct.
* `app_hook_add(a, APP_HOOK_FRAME | DOC_ACTIVATED | DOC_CLOSING | QUIT, ...)`.
* `app_task(a, work, done, ud)` runs work on the pool and `done` on the main
  thread (busy indicator in the status bar).
* Settings: `app_settings_of(a)` is the INI store (`settings.ini` in
  `PAL_DIR_CONFIG`), saved at exit. Keys in use: `ui.*`, `view.*`,
  `colors.*`, `window.*`, `panel.<id>`, `tool.*`, `recent.N`, `file.*`.

## Testing and scripts

`tests/app/app_test_util.h` creates a headless app (software renderer on an
off-screen surface, no settings file) and injects synthetic SDL events in
document coordinates. `app_script_run` (`src/app/script.c`) drives the same
from text: `open`, `new`, `tool`, `stroke`, `cmd`, `key`, `zoom`, `save`,
`expect pixel|dirty|layers|size|history|tool`, `screenshot`. The executable
runs scripts (`paintc --script file`), its own end-to-end check
(`paintc --self-test`) and screenshots after loading files
(`paintc --screenshot out.bmp [--headless] files...`), which is how the X11
(Xvfb) and Wayland (Weston) renderings are verified.
