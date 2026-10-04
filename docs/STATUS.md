# STATUS

## Current state
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
