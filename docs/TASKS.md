# TASKS

Execution plan for paint.c. Supersedes the lane table of the handoff where
they differ (see docs/DECISIONS.md ADR-001..012). IDs are stable.

## Ownership (lanes edit only these paths plus their own tests/<lane>/)
| Lane | Owns |
|---|---|
| L0 Foundation | cmake/PcDeps.cmake (SDL3, zlib), cmake/PcCommon.cmake, src/pal, include/pal (frozen v1, plus pal_clip_raw.h), .github/, cmake/toolchains/, packaging/, docs/BUILDING.md, docs/PAL.md, docs/patches/ |
| L1a Core geometry | src/core/pc_sel*, pc_raster*, pc_path*, pc_contour*, include/pc/pc_sel.h, pc_raster.h, pc_path.h, pc_doc.h/.c (selection fields only), tests/core/test_sel*, test_raster*, test_path* |
| L1b Core ops | src/core/pc_comp*, pc_mip*, pc_geom*, pc_layerops*, pc_txn.c, pc_resample*, pc_quant? (no: L6a), include/pc/pc_comp.h, pc_mip.h, pc_geom.h, pc_layerops.h, pc_txn.h (additive), tests/core/test_comp*, test_geom*, test_layerops*, test_txn* |
| L5a FX host + adjustments | include/fx/fx_run.h, src/fx/host/, src/fx/adjust/, tests/fx/ (host + adjust tests) |
| L5b Effects 1 | src/fx/blur/, src/fx/noise/, src/fx/photo/, src/fx/artistic/, tests/fx/test_fx1_* |
| L5c Effects 2 | src/fx/distort/, src/fx/render/, src/fx/stylize/, src/fx/object/, tests/fx/test_fx2_* |
| L6a Codecs (own formats) | src/codec/fmt_bmp.c, fmt_tga.c, fmt_gif.c, fmt_tiff.c, quant.c/.h, tests/codec/test_own_* |
| L6b Codecs (libraries) | cmake/PcCodecDeps.cmake, src/codec/fmt_png.c, fmt_jpeg.c, fmt_webp.c, fmt_dds.c, fmt_ora.c, zip.c/.h, icc.c, include/pc/pc_icc.h, third_party/bcdec, third_party/stb_dxt, tests/codec/test_lib_* |
| L6c PDN | src/codec/fmt_pdn.c, nrbf.c/.h, tests/codec/test_pdn*, tests/codec/samples/pdn |
| L3 UI toolkit | src/ui, include/ui (if any), third_party/stb_truetype, assets/fonts, tests/ui |
| L2/L4 App (wave 2+) | src/gfx, src/app |
| L7 Parity | docs/inventory, tests/golden, tools/goldencmp |

Shared files (root CMakeLists.txt, tests/CMakeLists.txt, tests/pc_test.h,
include/pc/pc_base.h, pc_par.h, pc_surf.h, pc_codec.h, include/fx/fx_abi.h,
fx_util.h, fx_builtin.h, include/pal/pal.h, src/codec/codec.c,
src/codec/CMakeLists.txt, src/fx/CMakeLists.txt, src/fx/fx_builtin.c,
docs/STATUS.md, docs/TASKS.md, docs/DECISIONS.md) are edited only by the
orchestrator. A lane that needs a change to one of them records the request
in its final report.

## Wave 1 (parallel)
- W1-L0: pal.h v1 over SDL3 (+ Win32/POSIX shims), mingw-w64 toolchain + Wine test run, GitHub Actions matrix (Linux GCC/Clang, Windows MSVC + clang-cl, macOS arm64 + x64), MSVC fixes in core.
- W1-L1a: selection model (A8 tile grid in pc_doc, history ops, combine modes, invert, select all/none, transform), AA polygon rasterizer (nonzero/even-odd), path building, flattening, stroking (joins, caps, dashes), marching-squares contours for marching ants.
- W1-L1b: multithreaded compositor with transaction override, tile-level API, premultiplied mip pyramid, txn upgrades (hash lookup, peek, write rect, masked write, A8 tiles), image ops (resize with all resampling modes, canvas size with anchor, crop, rotate 90/180, flip), layer ops (move, merge down, flatten, duplicate, flip, rotate/zoom) as history operations, history byte budget.
- W1-L5a: fx runner (ROI split, pool, cancellation, prepare), all Adjustments.
- W1-L5b, W1-L5c: every Paint.NET 5.1 effect in their categories.
- W1-L6a, W1-L6b, W1-L6c: every Paint.NET 5.1 file type plus ORA.
- W1-L3: UI toolkit, theme, icons, widget gallery with offscreen screenshot tests.

## Wave 2
- App shell: window, canvas view (gfx), documents and tabs, command system, menus, keymap, panels (Tools, History, Layers, Colors), status bar, dialogs, effect dialogs generated from fx_prop, settings, clipboard, drag and drop.
- Feature inventory of Paint.NET 5.1 menus, tools and shortcuts (docs/inventory).

## Wave 3
- Tools: selection tools and Move tools; brush engine, Paintbrush, Pencil, Eraser, Clone Stamp, Recolor; Paint Bucket, Magic Wand, Gradient, Color Picker; Line/Curve, Shapes, Text; Zoom and Pan.

## Wave 4
- Packaging (Windows zip/installer, macOS .app bundle, Linux AppImage/Flatpak manifest, .desktop), autosave and recovery, plugin loader, polish, parity audit, Wayland and X11 verification, CI green on all three OSes.

## Ownership additions recorded at the wave 1 merge
- L1a: tests/core/test_contour*, docs/notice/l1a.md.
- L1b: src/core/pc_tile.c, pc_hist.c (byte budget), pc_geom_int.h, tests/core/test_mip*, test_resample*, test_comp_mt.c, l1b_testutil.h.
- L5c: src/fx/color/, tests/fx/fx2_util.h.
- L6b: src/codec/lib_codec.c/.h, tests/codec/lib_test_util.h, third_party/bc7enc/, docs/DEPENDENCIES.md.
- L6c: src/codec/pdn_*.c, pdn.h, tests/codec/pdn_util.h, tests/codec/pdn_pypdn_check.py, docs/codecs/pdn.md.
- Every lane: docs/notice/<lane>.md.

## Wave 2 ownership (recorded at the wave 2b merge)
- W2A shell: src/app core files (app.c, app_internal.h, cmd.c, menu.c, doc.c, canvas.c, overlay.c, tool.c, dlg.c), src/gfx, include/app, docs/app/ARCHITECTURE.md (shared after wave 2b: small additive changes marked by lane).
- W2B-P panels: src/app/panels/, panels.c, shell.c, thumbs.c, docs/app/panels.md.
- W2B-M menus and dialogs: src/app/mods/mod_edit.c, mod_view.c, mod_image*.c, mod_layers.c, mod_help.c, src/app/edit/.
- W2B-F effects UI: src/app/mods/mod_effects.c, propdlg.c, src/app/fx/, tests/plugins/, docs/app/EFFECTS.md.
- W2B-A selection and move tools: src/app/tools/tool_{rect,lasso,ellipse}_select.c, tool_magic_wand.c, tool_move_*.c, sel_*, include/app/app_float.h.
- W2B-B painting tools: src/app/tools/tool_{paintbrush,pencil,eraser,clone_stamp,recolor,paint_bucket,gradient,color_picker}.c, stroke.*, paint_*.
- W2B-C vector tools: src/app/tools/tool_{line_curve,shapes,text}.c, vec_*, text_*.
- W2B-I integration: src/app/main.c, fileio.c, settings.c, script.c, platform/, src/app/io/, include/app/app_io.h, packaging/, assets/icons/, LICENSE, docs/PACKAGING.md, docs/app/io.md.
