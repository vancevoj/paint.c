# Lane I services of the editor (files, recovery, platform)

Companion to docs/app/ARCHITECTURE.md. Lane I owns src/app/main.c,
fileio.c, settings.c, script.c, mods/mod_file.c, src/app/io/,
src/app/platform/ and the public header include/app/app_io.h. Packaging is
in docs/PACKAGING.md.

## File flows (fileio.c, functions in app_internal.h)

| Function | What it does |
|---|---|
| `app_open_path(a, path)` | Decodes on a worker (content sniffing); an open file is activated, a file being decoded is not decoded twice; errors show the path and `pc_status_str`; the document's save type comes from the file name when it names a type that saves (load by content, save by name). |
| `app_save_doc(a, d, save_as, done, ud)` | Save or Save As: native dialog (type list in Paint.NET's order), Save Configuration when the type has options and they were not chosen for this image yet (Save As always), then the Flatten prompt, then a worker writes a snapshot atomically. `done(ok)` after the flow, exactly once: also `done(false)` when the app is destroyed while a step waits for an answer (lane W4-MODAL). |
| `app_save_doc_to(a, d, path, codec, params, sync)` | Dialog-free save for scripts and tests (flattens as a history step when needed). |
| `app_close_doc`, `app_close_all`, `app_quit_unsaved` | Close prompts; Close All and Exit list the unsaved images when there are several. |
| `app_new_image_dialog` | File > New (clipboard size, remembered aspect lock and units). |
| `io_params_load / io_params_store` (io_internal.h) | Last used save options per type: settings keys `file.save.<codec>.<prop>`. |

## Loading image files (io/load.c, app_io.h; lane CODEC, wave 4)

Every user-facing load goes through one function and one set of limits
(FL-BIG): File > Open and Open Recent (fileio.c open_work), crash recovery
(autosave.c restore_work), drop as layers (drop.c), Layers > Import From
File (edit/m_import.c), paste of a file or clipboard bytes (edit/m_paste.c)
and the Save Configuration preview (fileio.c cfg_work).

* `app_load_limits(&lim)`: any size up to 65535 per side (ADR-014), up to
  1024 layers, one memory budget for the file bytes held while decoding plus
  the decoded pixels: three quarters of the physical RAM, never less than
  the codec default of 4 GiB (`app_load_limits_ram` for a given RAM size).
* `app_load_file(path, only, &doc, &meta, &used, &info)`: the file is
  refused before reading when it exceeds the budget (there is no fixed
  byte cap any more; it was 3 GiB and refused the 32768 x 32768 24-bit BMP
  of the FL-BIG row), the decoder gets the budget minus the file size.
  `app_load_bytes` does the same for bytes already in memory.
* `app_load_error_text(out, cap, verb, name, st, &info)`: "Could not <verb>
  "<name>": ..." naming the reason: a missing file; a file larger than the
  budget (its size and the budget); an image over the decode limits (65535 x
  65535 pixels, 1024 layers and the memory left after reading the file);
  else `pc_status_str`.
* `app_load_test_budget(bytes)`: tests replace the budget (0 = automatic).

## Autosave and recovery (io/autosave.c, app_io.h)

* `app_autosave_configure(a, &cfg)`: root folder (default PAL_DIR_STATE, or
  `<config dir>/state`; "" = off), interval (settings key
  `file.autosave_interval`, seconds, default 120), heartbeat, exclusive
  (single instance primary), prompt. main() configures it; an app created
  with `config_dir = ""` (tests, scripts) starts with autosave off.
* Session folder `<root>/recovery/<session>/`: `session.ini`, `heartbeat`,
  `doc-<n>.pdn` + `doc-<n>.ini` per modified image. Normal exit removes it.
* `app_recovery_scan / get / restore / discard / prompt`: images of dead
  sessions; restored images are modified documents with their original
  name, path and type; their files are deleted once this session has
  autosaved them. The first frame scans and prompts (interactive runs).
  Restoring decodes with `app_load_file` (the limits of File > Open), so
  every image that opens can be recovered (wave 4; it used the codec
  defaults of 1 Gpx and 4 GiB of pixels).
* `app_autosave_now(a, wait)`, `app_autosave_saved_count(a)` for scripts and
  tests. Settings are also flushed in the background every 30 s when they
  changed (crash-safe preferences).

## Other services

* Open Recent (io/recent.c): `app_recent_menu_items` draws the submenu
  (thumbnails cached in `<cache>/recent-thumbs`, path tooltips, Clear List);
  `app_recent_open`, `app_recent_clear`, command `file.recent.clear`.
* Drag and drop (io/drop.c): `app_drop_files(a, paths, n, APP_DROP_ASK |
  OPEN | LAYERS)`, `app_import_layers(a, d, paths, n)` (usable by the Layers
  lane for Import From File).
* `app_io_event` (io/wake.c) runs first in `app_event`: it takes the SDL drop
  events and the wake-up event of `app_wake_poll(a, ms)`, which keeps pal
  callbacks (forwarded opens, dialog results) flowing while the editor is
  idle.
* Window icon (io/icon.c): `app_set_window_icon`, `app_icon_rgba(size)`.
* Settings (settings.c): atomic saves with `settings.ini.bak`, damaged files
  moved to `settings.ini.corrupt`, format version in the header comment.
* Command `file.recover` scans again and shows the recovery dialog.

## Command line (main.c)

`--state-dir DIR`, `--autosave-interval SECONDS`, `--version` in addition to
the wave 2a options. Interactive runs use pal's single instance (forwarded
paths open in the running editor) and the wake poll.

Lane UIB (wave 4): the single instance is per settings folder
(`app_cli_instance_id`, src/app/cli_set.h): a launch with another
`--config-dir` starts its own window. `--set KEY=VALUE` given to a launch
that forwards is written to `settings.forward.<time><random>.ini` in the
settings folder first; the running instance takes those files when the
forwarded message arrives (a new primary takes leftovers at startup),
applies the values live (theme, view toggles, units, colors, tool settings,
panels, recent files, window geometry, Settings dialog preferences) and
saves the settings file at once.

## Script commands added by lane I (script.c)

`saveas PATH`, `autosave`, `idle MS`, `print TEXT`, `touch PATH`,
`recover [all|I]`, `discard [all|I]`, `drop open|layers|ask PATH`,
`select N`, `wheel X Y DY`, and the expectations `autosaved N`,
`recovery N`, `recent N`, `dialogs N`, `name NAME`, `path PATH`,
`layername I NAME`.
