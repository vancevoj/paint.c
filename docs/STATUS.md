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
