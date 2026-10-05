# Effects and adjustments in the editor (lane F)

How paint.c runs the effects of the registry (`include/fx/fx_run.h`, lanes
L5a/L5b/L5c and plugins) from the Adjustments and Effects menus, with the
Paint.NET 5.1 workflow: dialogs generated from `fx_prop`, live preview on
the canvas, progress, OK as one history step, Cancel, Repeat, remembered
parameters, the Curves and Levels editors and effect plugins.

| File | Role |
|---|---|
| `src/app/mods/mod_effects.c` | Commands `adjust.<id>` / `effects.<id>` per registry effect, `effects.repeat` (Ctrl+F), `effects.plugin_errors`, plugin folders at startup |
| `src/app/fx/afx.h` | Lane-private API (sessions, builder extras, editors, plugin loader, test hooks) |
| `src/app/fx/afx_session.c` | Sessions: snapshot, background jobs, preview, apply, commit, Repeat, memory, the effect dialog |
| `src/app/propdlg.c` | The generic `fx_prop` dialog builder (`app_props_ui`, also used by Save Configuration) |
| `src/app/fx/afx_curves.c` | The `curves` custom widget and its pointer logic; the Levels histogram helper |
| `src/app/fx/afx_levels.c` | The `levels` custom widget |
| `src/app/fx/afx_plugins.c` | Plugin loader and the Plugin Errors list and dialog |
| `tests/plugins/` | Sample plugin and malformed fixtures |

The menus themselves come from `menu.c` (shared): Adjustments lists every
`Adjustments/...` effect, Effects shows `Repeat <name>` (after an effect
ran), `Plugin Errors...` (when a plugin failed) and the submenus in registry
order, which sorts menu paths segment by segment, case-insensitively:
Artistic, Blurs, Color, Distort, Noise, Object, Photo, Render, Stylize, with
plugin submenus sorted in between. Items whose effect has a dialog end in
"...". Plugin effects carry their own icon and a tooltip with the effect
name, author, version and library file.

## Sessions

A session is one run of one effect on the active layer of the active image.
Everything happens on the main thread except the copy and the render.

1. **Start** (`afx_open`, `afx_repeat`, `afx_run_now`): the active layer and
   the selection are captured in a one-layer snapshot document that shares
   their immutable tiles (a few hundred microseconds, even at 8K). The
   document transaction opens with the session as owner and the effect's
   display name as label; the session joins the dialog stack, so the canvas
   and shortcuts are inactive while it lives.
2. **Loading** (worker, `app_task`): the snapshot becomes the contiguous
   `src` image of the full-source model (ADR-005), `dst` starts as a copy,
   the selection coverage over its bounds becomes `fx_env.sel_mask` (v1.1),
   a selection-area thumbnail is made for the pan pads, and for Levels the
   input histogram of the pixels selected at 50 % or more. Allocation sizes
   are checked by `pc_surf_alloc` (P-08).
3. **Render** (`afx_run`): one `app_task` runs `fx_job_work` itself and pool
   helper tasks join the same job (one per pool thread). ROIs are 64 x 64
   cells handed out nearest to the viewport center first. When parameters
   change, the job is cancelled (workers stop within a row) and the next
   job starts as soon as the cancelled one drained, from the done callback,
   so neither the UI thread nor input ever waits. A job's buffers are
   reference counted, so a session may close while its job drains.
4. **Preview**: each frame the finished ROIs are blended into the
   transaction with `pc_txn_blend_rect_masked` through the selection
   coverage (no coverage for `FX_FLAG_NO_SEL_CLIP` effects such as Drop
   Shadow, which render over the whole layer), at most 6 ms per frame. When
   the job is done and many ROIs are still waiting, the rest is blended in
   parallel bands of 256 rows, still within the budget. Blending always
   starts from the original pixels, so repeated previews never compound.
   The status bar progress follows the render and hides when the preview
   is complete.
5. **Apply**: OK (or a dialog-less run) waits for the job of the final
   parameters (restarting it if a `FXP_F_NO_PREVIEW` value changed),
   blends the rest (25 ms per frame budget) and commits the transaction as
   one history item named after the effect. When nothing changed an empty
   history item is still recorded, so OK always adds exactly one. While
   this takes longer than 300 ms a small modal box shows the progress and a
   Cancel button. Cancel, Esc and the close button restore the image
   exactly (the transaction is dropped).

Repeat (Ctrl+F, `effects.repeat`) re-runs the last effect from the Effects
menu with its last parameters, without a dialog. Adjustments neither set
nor use it (5.1 docs "Repeat last effect", MENUS.md, OBSERVED O-UI-ADJREPEAT,
3.36). Parameters are remembered per effect for the session only after OK
(Paint.NET keeps them until restart; nothing is persisted). Color defaults
of `FX_COLOR_PRIMARY` / `FX_COLOR_SECONDARY` resolve against the palette when
the effect first runs and are then remembered like every other value.

Effect dialogs do not dim the canvas (the preview is the point): while one
is the topmost dialog the theme's backdrop is transparent. A transaction
owned by something else (a live tool edit) blocks a new session; commands
finish live tool edits before they run.

Large images: on 8192 x 8192 (tests/app/test_f_large.c) opening a dialog
takes well under a millisecond on the UI thread, frames stay at the
renderer's own cost plus the blend budget while 64 M pixels render, a
parameter change returns in about a millisecond and the next job starts
within two frames, Cancel returns at once.

## The dialog and the property builder

`app_props_ui` (and `afx_props_ui`, the same with a per-prop filter) lays out
one control per `fx_prop` in schema order, like Paint.NET's property dialogs
(OBSERVED.md section 1):

| Kind | Control |
|---|---|
| INT, REAL | label; slider, numeric up/down (typing, arrows, wheel, drag) and reset button; `FXP_F_SLIDER_LOG`, `FXP_F_PERCENT`, decimals from `step` |
| BOOL | check box |
| CHOICE | `Label:` and a drop-down on one row |
| COLOR | header; color wheel, R G B A channel bars with numbers, swatch, hex entry and reset (to the palette color for `FX_COLOR_PRIMARY` / `FX_COLOR_SECONDARY`) |
| ANGLE | header; dial with numeric box (Shift snaps to 15 degrees) and reset |
| POINT | header; pan pad over a thumbnail of the selection area, X and Y sliders with numeric boxes and reset buttons |
| SEED | a button with the prop's label ("Randomize") that draws a new seed; other values never reseed |
| CUSTOM | the widget registered for the prop's hint, hidden otherwise |

`enabled_if` disables and dims a control while its condition is false.
Effects with two or more color props and other props get two tabs, the
effect name and "Colors" (Clouds). Dialog widths: 380 DIPs, Curves 340,
Levels 540. Enter is OK (also from a numeric field, which commits first),
Esc is Cancel.

### Curves (`curves` hint, `fx_curves.h`)

Transfer Map drop-down (Luminosity, RGB), a square graph with a dashed 4 x 4
grid and the identity diagonal, guide lines and an `(input, output)`
readout under the pointer, the channel check boxes in RGB mode (a disabled
checked Luminosity box otherwise), a tip line and Reset. Clicking adds a
point under the pointer on the edited curves, dragging moves a point (a
point the drag covers comes back when the drag moves on), a right click
removes a point, the end points at input 0 and 255 can only move
vertically. Pointer rules: Paint.NET 3.36 CurveControl (docs/notice/f.md),
testable on their own (`afx_curve_press/move/release`).

### Levels (`levels` hint, `fx_levels.h`)

Input histogram, input white and black points (numeric boxes and swatches),
two gradient bars with draggable arrows (input black and white, output
black, gray and white), output white point, gray point (gamma 0.10 .. 10.00)
and black point, output histogram (the input mapped through the current
levels), R G B check boxes choosing the edited channels (all controls are
disabled when none is checked), Auto (`fx_levels_auto` of the input
histogram, the Auto-Level logic) and Reset. Double-clicking a swatch opens
a color picker that sets that point per channel. Numeric and arrow edits go through
`fx_levels_edit` (3.36 per-mask averaging).

## Plugins

Folders scanned at startup, files first, then one level of subfolders, in
name order: `PAL_DIR_DATA/plugins` (per user, ADR-008 OD-9) and a portable
`plugins` folder next to the executable. Runs without a settings folder
(tests, `--script`, `--screenshot`, `config_dir ""`) skip the per-user folder;
`paintc --disable-plugins` (K-CLI-NOPLUGINS) skips both. Windows loads with
`LoadLibraryExW` and explicit search flags (pal, X-18).

A plugin is a shared library named `<name><suffix>` (`.dll`, `.dylib`, `.so`;
no `lib` prefix needed) exporting `fx_entry` (`fx_abi.h`). paint.c also
understands two optional exports:

```c
FX_EXPORT uint32_t fx_abi_version(void);               /* return FX_ABI_VERSION */
FX_EXPORT const char *fx_plugin_info(const char *key); /* "author", "version" */
```

A library whose `fx_abi_version` differs from the host's ABI is rejected
before `fx_entry` runs. Loading is all or nothing per file: `fx_entry`
registers into a scratch registry first; each effect must pass
`fx_effect_validate` and have a plausible `size` (at least the ABI v1
`sizeof(fx_effect)`, at most 64 KiB); only when `fx_entry` returns a
non-negative count are the effects moved into the app's registry, where an
id already in use is rejected. A library that contributes nothing is
unloaded again. Every problem is listed under Effects > Plugin Errors...
(file and one-line reason); the same list is available to the Settings
dialog as `afx_plugin_errors_ui`. Libraries stay loaded until the app
exits; plugins allocate through `fx_host` (X-17). Code that crashes while
running cannot be contained in-process (as in Paint.NET).

`tests/plugins/sample_plugin.c` is a complete sample (Effects > Samples >
Tint with color, int, choice and bool props); `tests/plugins/CMakeLists.txt`
shows how to build one.

## Testing

| Test | Covers |
|---|---|
| `test_f_effects` | All 55 adjustments and effects through the app (command, dialog, preview, OK) against an independent oracle (`fx_run_sync` on a copy, blended through the selection), through an antialiased selection and on the whole image; one history item each, exact undo; menu contents, order and shortcuts |
| `test_f_dialog` | Preview and Cancel / Esc, OK at identity, restarts, remembered parameters, Repeat, progress, mouse on Reseed / check box / angle / pan pad and resets, enabled_if, palette colors, closing the image or the app mid-render, undimmed canvas, busy documents |
| `test_f_curves`, `test_f_levels` | The editors' rules and the widgets driven by mouse events in the real dialogs; Auto equals Auto-Level |
| `test_f_plugins` | Loader against the fixtures: valid and nested plugins, wrong ABI, short struct, invalid props, failing entry, duplicate ids, empty, not a library; plugin commands, tooltip, Repeat, Plugin Errors dialog, `--disable-plugins` |
| `test_f_large` | 8192 x 8192: open, frame times while rendering, change, restart, cancel, commit |

Lane-private hooks for tests: `afx_active`, `afx_session_*`, `afx_wait_idle`,
`afx_wait_preview`, `afx_prop_hit` (where a prop's controls are),
`afx_curves_graph_rect`, `afx_levels_rect`, `afx_levels_axis`.
