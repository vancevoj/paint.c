# Panels and shell (lane P, wave 2b)

The four utility windows, the Layer Properties dialog, the image list, the
toolbar row, the status bar and the thumbnails. Behavior follows
docs/inventory/WINDOWS.md sections 1 to 7 (with OBSERVED.md where it is the
better evidence, ADR-016). Everything runs on the main thread.

## Files

| File | Contents |
|---|---|
| `src/app/panels/pnl.h` | Lane internals shared by the files below and the `test_p_*` tests. |
| `src/app/panels/mod_panels.c` | Registers the windows (default places: Tools top left, Colors bottom left, History top right, Layers bottom right), `colors.swap` (X), `colors.toggle_slot` (C), `colors.reset`, `docs.context_menu` (Alt+Minus) and the View menu item Reset Window Layout (`window.reset_all`). |
| `src/app/panels/mod_layer_props.c` | Registers `layers.properties` (F4); the file name sorts before `mods/mod_layers.c`, so this registration wins over the wave 2a stand-in. |
| `src/app/panels/pnl_common.c` | Named rectangle registry and the list widget (`pnl_list_do`: virtualized rows, own scroll bar, wheel, drag reordering with a drop indicator, ensure-visible). |
| `src/app/panels/pnl_tools.c` | Tools window: 2 x 10 grid in TOOLS.md order, tooltips "Name (S, 4 times)", disabled slots for tools not registered yet. |
| `src/app/panels/pnl_history.c` | History window: icons per entry (the producing command's icon by label, then the tool, then fixed labels, else the effects icon), jumps, the toggle on the current entry, gray undone rows, four buttons, current row kept visible. |
| `src/app/panels/pnl_layers.c` | Layers window: thumbnail, name, visibility check box (history step "Hide Layer" / "Show Layer"), activation, double click, drag reorder ("Move Layer"), seven buttons with Ctrl+click top / bottom, active row kept visible. |
| `src/app/panels/pnl_layerprops.c` | Layer Properties: Name, Opacity, Blend Mode, Visible; live preview by setting the layer fields while the modal dialog is open, restored before the single "Layer Properties" history step. |
| `src/app/panels/pnl_colors.c` | Colors window (compact 252 x 232 DIPs, expanded 508 x 306), 3.36 integer HSV, wheel, channel bars and boxes, hex box, palette grid, Add Color, palette menu, Save Palette As dialog. |
| `src/app/panels/pnl_palette.c` | Palette file parsing and writing, names, the folder (`<config dir>/palettes`). |
| `src/app/panels/pnl_imagelist.c` | The image list: thumbnails, unsaved marker, close X, middle click, drag reordering, overflow arrows with auto repeat, list popup, context menu. |
| `src/app/shell.c` | Window layout, toolbar row, window toggles, status bar. |
| `src/app/thumbs.c` | Thumbnail engine. |
| `src/app/panels.c` | Panel registry, persistence (`panel.<id>`, `colors.more`, `colors.palette`), resets. |

## Notes for other lanes

* Tests and scripts find widgets through `pnl_rect(a, "layers.add")` and
  the other names recorded with `pnl_rect_set` (see the `test_p_*` files),
  so layout changes do not break them.
* History labels choose their icon by matching command labels, so a lane
  that records history with its command's label (without "...") gets its
  command icon in the History window for free.
* Thumbnails read the document and its open transaction on the main
  thread. Work is cached per cell of tiles and keyed by
  `pc_comp_tile_sig` (composite) or tile serials and transaction versions
  (layers); a frame spends at most about 3 ms on it and an image thumbnail
  is refreshed at most every 250 ms while edits continue.
  `pnl_thumbs_sync` forces everything (tests, screenshots).
* The Colors window keeps H, S and V beside the active color so hue and
  saturation survive black and gray while sliders move; a wheel pick sets
  value 100 like 3.36 (the inventory's "value fixed at the current V" is
  inferred, ADR-016 ranks 3.36 higher).
