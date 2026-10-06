# Plugins

Plugins add adjustments and effects to paint.c. A plugin is a native shared
library (a .dll on Windows, .dylib on macOS, .so on Linux) written against
paint.c's own effect interface, `fx_abi.h`. Paint.NET plugins (.NET
assemblies) do not work in paint.c.

> Plugins run as part of paint.c with the same rights as the program. Only
> install plugins from people you trust.

## Installing a plugin

1. Copy the library file into the plugin folder of your user account:
   `{{plugin_dir}}`
   (Settings > Plugin Errors has a button that opens it.) Libraries in
   subfolders one level down are found too.
2. For a portable copy of paint.c, a `plugins` folder next to the program
   works as well: `{{exe_plugin_dir}}`
3. Restart paint.c. The plugin's effects appear in the Adjustments or Effects
   menu; their tooltip names the plugin, its author and version.

paint.c has official optional plugins that are separate downloads (the
`plugins-v1.0.0` release of paint.c on GitHub, one zip per plugin or all in
one): AA's Assistant, Align Object, Bevel Object, Content Aware Fill,
Gradient Mapping, Grid / Checkerboard, Grim Color Reaper, Perspective,
Shape3D and Water Reflection. Each one re-creates a popular Paint.NET plugin
natively and its README credits the original author. Unzip the folder of a
plugin into the plugin folder. They are also in the `plugins` folder of the
paint.c source tree; each one builds into a folder of its own
(`cmake --build build --target plugins`, output in `build/plugins/out`).

If a plugin cannot be loaded, **Effects > Plugin Errors** lists the file and
the reason (for example, a library built for another version of the
interface). Starting paint.c with `--disable-plugins` skips all plugins.

On macOS every library on Apple silicon needs a signature. The linker adds
one automatically; after changing the file, `codesign --force --sign -
name.dylib` adds it again.

## How an effect works

- paint.c copies the active layer into `src`, a whole-layer image of BGRA
  pixels with straight (not premultiplied) alpha, and makes an empty `dst`
  of the same size.
- It calls your `render` function for many small rectangles (`roi`), often
  on several threads at once. `render` writes the pixels of its rectangle
  into `dst` and may read any pixel of `src`.
- paint.c then blends `dst` into the layer through the selection, shows it
  as a preview and adds one History item when the user clicks OK.
- The result must depend only on the parameters, the source pixels and the
  pixel position, never on the order of the calls. Random effects derive
  their randomness from a seed parameter and the pixel coordinates.
- `render` should call `host->cancelled(job)` at least once per row and
  return `FX_CANCELLED` when it is true, so the dialog stays responsive.
- An optional `prepare` function runs once before the first `render` and
  can build tables or statistics shared by all calls (for example a
  histogram); `release` frees them.

## The parameters

Each `fx_prop` becomes one control in the effect's dialog, laid out in
order, and stores its value at `offset` in your parameter block:

| Kind | Value | Control |
|---|---|---|
| `FXP_INT` | `int32_t` | Slider with a number box |
| `FXP_REAL` | `double` | Slider with decimals (`step` sets them) |
| `FXP_BOOL` | `int32_t` 0 or 1 | Check box |
| `FXP_CHOICE` | `int32_t` index | Drop-down list (`choices`, NULL terminated) |
| `FXP_COLOR` | `uint32_t` 0xAARRGGBB | Color wheel (see below for palette defaults) |
| `FXP_ANGLE` | `double` degrees | Dial |
| `FXP_POINT` | `double[2]` | Point picker over the selection, -1 to 1 |
| `FXP_SEED` | `int32_t` | Button for a new random seed |

A color whose default is `FX_COLOR_PRIMARY` or `FX_COLOR_SECONDARY` starts
as that palette color. `enabled_if` names another property ("key" or "key=N") that must be on for
the control to be enabled. Flags: `FXP_F_PERCENT` shows a % sign,
`FXP_F_SLIDER_LOG` uses a non-linear slider, `FXP_F_NO_PREVIEW` does not
restart the preview.

Effect flags: `FX_FLAG_ADJUSTMENT` lists it under Adjustments (the menu path
then starts with "Adjustments/"), `FX_FLAG_NO_DIALOG` runs it at once with
the defaults, `FX_FLAG_SINGLE_THREAD` gives one call for the whole selection,
and `FX_FLAG_NO_SEL_CLIP` lets an effect draw outside the selection (for
shadows).

### Dialog extras

[fx_widgets.h](fx_widgets.h) describes extras of paint.c's effect dialog.
Versions of paint.c that do not know them show the plain control, so a
plugin that uses them still works everywhere:

- A `FXP_CHOICE` with the hint `"position-grid"` and the 16 position names
  in `FX_POS_*` order becomes a 3 x 3 grid of position buttons with rows for
  horizontal-only and vertical-only positions and a Reset position button
  (otherwise: a drop-down of the same names).
- A `"tip:"` hint on a check box, drop-down or seed button is its tooltip:
  `"tip:Reveal low opacity pixels"`.
- `FXP_F_PREVIEW_ONLY` marks a value as a preview aid: paint.c puts it back
  to its default before the final render, so it never reaches the image.

### Telling the user something

When an effect has nothing to do (for example, there is no object to
work on), call `host->notice(job, "One or two sentences.")` from `prepare`
or `render`, then leave the pixels as they are. paint.c shows the first
notice of a run in a message box. Check that the host has it first:
`host->size >= offsetof(fx_host, notice) + sizeof host->notice && host->notice`.

## A complete example

This plugin adds **Effects > Examples > Fade**, which blends the image toward
a color. Save it as [fade_plugin.c](fade_plugin.c) next to
[fx_abi.h](fx_abi.h) (both files are in the folder of this help page).
[fx_util.h](fx_util.h) has optional helpers for pixel access, colors,
random numbers and cancellation.

{{plugin_example}}

## Building it

The library needs no paint.c code and no other library. Build it for the
same processor architecture as paint.c:

| System | Command |
|---|---|
| Linux | `cc -std=c17 -O2 -shared -fPIC -fvisibility=hidden -o fade.so fade_plugin.c` |
| macOS | `cc -std=c17 -O2 -dynamiclib -fvisibility=hidden -o fade.dylib fade_plugin.c` |
| Windows (Visual Studio) | `cl /std:c17 /O2 /LD fade_plugin.c /Fe:fade.dll` |
| Windows (MinGW) | `gcc -std=c17 -O2 -shared -o fade.dll fade_plugin.c` |

Copy the result into the plugin folder above and restart paint.c.

## Rules for plugin authors

- **Memory**: allocate what you hand to paint.c, or keep between calls,
  with `host->alloc` and free it with `host->free`. Never free memory you
  did not allocate.
- **Threads**: `render` runs on several threads at once. Keep it free of
  global state that changes; read only its arguments and your `prepare`
  state.
- **Ids**: give every effect a unique id in reverse domain style
  ("com.example.sharpen-more"). An id that is already taken is refused.
- **Versions**: structures carry their size and only grow at the end, so a
  plugin built for this interface keeps working with later paint.c
  versions of the same `FX_ABI_VERSION`.
- **Names**: the last part of the menu path is the effect's name. Give
  effects with a dialog plain names; paint.c adds the "..." itself.
