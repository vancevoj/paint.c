# STATUS

## Current state
- Wave 1 merged (except L3 UI toolkit and L6A own codecs, resumed after a pause): pal v1 over SDL3 + Win32/POSIX shims and CI yaml (L0); selection, rasterizer, paths/strokes, contours (L1A); txn upgrades, MT compositor, view cache + mips, resampling, image geometry and layer ops as history ops, byte-budget history (L1B); fx host runtime + 13 adjustments (L5A); 21 effects Blurs/Noise/Photo/Artistic (L5B); 21 effects Color/Distort/Object/Render/Stylize (L5C); PNG, JPEG, WebP, DDS, ORA, ICC (L6B); .pdn read/write, reader matches pypdn on 489 real 3.0 to 5.1.12 files (L6C).
- Core v0.1 (tiles, docs, txn, history, blend, fill) moved to src/core; suites pass.
- Contracts frozen for wave 1: pc_par.h, pc_surf.h (+ impl), pc_comp.h (reference impl), pc_codec.h (+ registry, buffers, readers), fx_abi.h, fx_util.h, fx_builtin.h, pal.h (header only).
- Build: CMake + Ninja, system or vendored SDL3 3.4.18, vendored zlib 1.3.2. Glob-based lanes.

## In progress
- Wave 1 (see docs/TASKS.md).

## Blocked
- None (owner directive, ADR-008/009).

## Next
- Wave 2: app shell and canvas.

## Log
- 2026-10-04 T-L0-01 restructure done (full suite 20,488,758 checks pass; quick suite pass).
- 2026-10-04 contracts and ADR-001..012 recorded; project renamed paint.c.
- 2026-10-04 W1-L7 inventory merged: docs/inventory (MENUS, TOOLS, WINDOWS, SHORTCUTS, FILES, VIEW, PARITY with 1469 rows). Values tagged I need a 5.1.12 check.
- 2026-10-05 wave 1 lanes L0, L1A, L1B, L5A, L5B, L5C, L6B, L6C merged; 39 CTest executables pass, full pc_tests 20,488,758 checks pass. MinGW timespec fallback and MSVC narrowing casts applied from docs/patches.
- 2026-10-05 CI green on all nine jobs (run 37266845400): Linux GCC/Clang/ASan+UBSan/TSan, mingw-w64 + Wine, Windows MSVC /W4 /WX and clang-cl, macOS 14 arm64 and macOS 15 Intel; 40/40 tests each. macOS jobs now run on dispatch, tags or [mac] commits.
- 2026-10-05 W1-L6A merged: BMP, TGA, GIF, TIFF (own, hardened) read/write + quant.h (octree/median cut + k-means, Floyd-Steinberg level 0..8); PNG 8-bit save now uses it. 46 CTest executables pass.
- 2026-10-05 W1-L3 merged: pc_ui toolkit over SDL_Renderer, embedded Inter, stb_truetype behind a font validator, 98 original icons, all widgets, gallery (paintc_ui_gallery). 57 CTest executables pass. Wave 1 complete.
- 2026-10-05 W15-E3 merged: pc_shapes.h (29 shapes + shared pc_vrender), pc_linecurve.h, pc_text.h (backend-agnostic layout); 60 CTest executables pass.
- 2026-10-05 W2-FXP merged: effect parameter parity per ADR-016 (docs/fx/parity.md), 5.2 golden test tests/golden (27 items within pre-written tolerances; Invert, Brightness/Contrast, Posterize exact).
- 2026-10-05 W15-E1 (brush, pencil, eraser, clone, recolor) and W15-E2 (53 patterns, 7 gradients, wand/bucket regions) merged; pc_paint gains clip_pixelated. 71 CTest executables pass.
- 2026-10-05 W2A merged: paintc editor shell, gate G2 passes (open PNG, paint, undo, redo, save) on X11, Wayland and headless; docs/app/ARCHITECTURE.md. 80 CTest entries pass.
- 2026-10-05 ci/fix2 merged: MSVC warnings, macOS Cmd shortcuts in app tests, -ffp-contract=off, STREAMING UI atlas (SDL RLE rounding); CI 8/9 green, last red was the timing check on macos-15-intel, now report-only on CI (ADR-017).
- 2026-10-05 W2B-A (selection and move tools, floating paste API app_float.h) and W2B-C (Line/Curve, Shapes, Text with system fonts and IME) merged; 89 CTest entries pass.
- 2026-10-05 Wave 2b merged (P panels, M menus and dialogs, F effects UI and plugins, B painting tools, I file I/O, autosave, single instance, packaging): every Paint.NET 5.1 tool, menu and window now exists; 117 CTest entries pass locally.
- 2026-10-05 Wave 3a parity audit: 1469 rows, 1289 DONE, 107 WIP (partial), 40 TODO, 33 N/A; statuses written to docs/inventory/PARITY.md. 147 gaps (5 high, 36 medium, 106 low) go to wave 3b.
- 2026-10-05 Wave 3b done (7 lanes, reports in /ai/work/paintc-research/handoff/wave3b_reports.json). Merged into master LOCALLY, NOT YET BUILT OR TESTED, NOT PUSHED: w3/keys, w3/shell, w3/toolb, w3/codec, w3/avifjxl, w3/fxcore. Left unmerged: w3/toola (conflicts in src/app/tool.c and src/app/panels/pnl_layers.c). Also unmerged: ci/fix3 (CI round 3, 7 commits, last runs on that branch failing/in progress). Next: build + ctest master, resolve w3/toola, merge origin/ci/fix3, update PARITY.md from the wave 3b reports, push with [mac], final Wayland/X11/Wine verification and a v0.1 release.
- 2026-10-05 Wave 3b integrated on master (Rotate/Zoom sRGB helper, single Home/End rule, toola conflicts resolved); 3 cross-lane test failures (keys/toola) handed to the integration lane (branch int/w3) together with CI round 4 (ci/fix4). PARITY.md after wave 3b: 1447 DONE, 20 N/A, 2 WIP (per lane reports; to be re-verified by a final audit).
- 2026-10-05 Wave 3b done (7 lanes, reports in /ai/work/paintc-research/handoff/wave3b_reports.json). Merged into master LOCALLY, NOT YET BUILT OR TESTED, NOT PUSHED: w3/keys, w3/shell, w3/toolb, w3/codec, w3/avifjxl, w3/fxcore. Left unmerged: w3/toola (conflicts in src/app/tool.c and src/app/panels/pnl_layers.c). Also unmerged: ci/fix3 (CI round 3, 7 commits, last runs on that branch failing/in progress). Next (after the integration entry below): update PARITY.md from the wave 3b reports, merge int/w3 into master, v0.1 release (tags build AVIF/JPEG XL BUNDLED on every platform, macOS included).
- 2026-10-05 Integration int/w3 (branch ci/fix4): wave 3b + w3/toola + ci/fix3 merged and verified. Three KEYS x TOOLA failures fixed: the tool chooser lost KEYS's wheel stepping (now in tool.c); app_tool_menu_open opens through the same toolkit request as Alt+T (keyboard menu: first tool highlighted, a unique first letter chooses); KEYS tests updated to TOOLA's Finish History item (T-FW-HISTORY). Also: the Text tool falls back per character to the built-in Inter when a face cannot draw it (Wine's bitmap-only TrueType fonts), cross builds ignore the build machine's pkg-config for AVIF/JPEG XL and every system library must link, MSVC C4701 in pc_text_hint, AVIF/JPEG XL status per platform in docs/codecs/avif_jxl.md. Local: Release, ASan+UBSan and Clang 157/157 with 0 warnings, mingw-w64 + Wine 157/157 (BUNDLED and OFF), paintc scripts (paste, move, Finish, undo, tool chooser, menus) on Xvfb X11, headless Weston and Wine.
- 2026-10-05 CI on ci/fix4 (965 billed minutes, then GitHub stopped starting jobs: "recent account payments have failed or your spending limit needs to be increased"; OWNER: billing). Green per job: linux-gcc, linux-clang, linux-tsan and windows-clang-cl in run 37331223253 (ef118ee); mingw-w64 + wine and linux-asan-ubsan in run 37324304799 (3d9b873); windows-cl (MSVC /W4 /WX) compiles clean and failed only the installed-emoji smoke test, fixed in ef118ee and passing on windows-clang-cl on the same image. macOS 14 and 15 Intel configure and build (Homebrew / preinstalled libavif), tests passed up to test_shell_crash, whose crashing child never ended there (run cancelled after 23 min); the test now forbids core files in the child, waits at most 60 s with a diagnostic, and every CI ctest call has --timeout 600. Not yet run on a macOS runner. Next: when Actions minutes are available, `gh workflow run ci.yml --ref ci/fix4` (all nine jobs, macOS included).
- 2026-10-05 int/w3 merged into master (157/157 locally in Release, ASan, clang and mingw+Wine; test_brush_perf trips its local timing limit only under the parallel verification load, ADR-017). GitHub Actions stopped starting jobs (spending limit / billing) after about 2000 billed minutes this month; latest green per job is in the integration report (ci/fix4).
- 2026-10-05 Wave 4 merged (UIA, UIB, TOOLS, CODEC): every item of the final verification fixed with regression tests; 176/176 CTest entries pass locally. ADR-022: vendored SDL3 3.4.18 by default. PARITY: see counts in PARITY.md.
