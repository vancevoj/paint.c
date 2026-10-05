# Effect host runtime (lane L5a)

Public API: `include/fx/fx_run.h`. Implementation: `src/fx/host/` (part of
`pc_fx`). Pure C17 over `fx_abi.h` and the `pc_core` atomics; no SDL, no pal,
no OS headers. The app supplies threads.

## Pieces

| Piece | Functions | Thread rules |
|---|---|---|
| Host services | `fx_run_host`, `fx_run_set_log`, `fx_run_log` | host table: any thread. Set the log hook once on the main thread before jobs run. |
| Validation | `fx_effect_validate`, `fx_prop_value_size` | pure |
| Menu paths | `fx_menu_split`, `fx_menu_compare` | pure |
| Registry | `fx_registry_create/destroy/add/add_entry/add_builtins/count/at/find/find_menu` | build on one thread; read-only use from any thread afterwards |
| Parameters | `fx_params_init/new/free`, `fx_prop_find`, `fx_param_get/set`, `fx_param_get_point/set_point`, `fx_params_clamp/valid`, `fx_color_default` | any thread on its own blob |
| Presets | `fx_preset_save`, `fx_preset_load` | any thread on its own blob |
| Jobs | `fx_job_*`, `fx_run_sync`, `fx_run_sync_tiled` | see below |

### Validation

`fx_effect_validate` rejects descriptors that would make the host or the
dialog generator misbehave: struct smaller than ABI v1, id not
`[A-Za-z0-9_.-]{1,128}`, menu path without a category, empty levels or
padding spaces, missing `render`, `FX_FLAG_GPU` (reserved for ABI v2), more
than 256 props or a params blob over 1 MiB, bad keys or duplicates, unknown
kinds, values outside `params_size`, misaligned 4 and 8 byte values,
overlapping props, defaults outside their range, non-integral int ranges,
empty or overlong choice lists, custom props without size or hint, and
`enabled_if` strings that name an unknown key or a non-integer value. The
registry and `fx_job_create` both run it.

### Menu paths

Levels are separated by `/`. A `/` with a space on either side belongs to a
name, so `Adjustments/Brightness / Contrast` has the two levels
`Adjustments` and `Brightness / Contrast`. The registry keeps effects sorted
level by level (ASCII case-insensitive first, then case-sensitive, then by
id), which reproduces the alphabetical order of the Paint.NET menus.

### Registry and plugins

`fx_registry_add_entry(reg, entry)` calls a plugin's `fx_entry` export (or
`fx_builtin_register`) with `fx_run_host()` and a `reg` callback that
validates and inserts. The callback returns 0 when accepted and a negative
`pc_status` otherwise; rejected effects are reported through the log hook.
The `fx_entry_fn` callback has no user pointer, so the target registry is a
process-wide variable during the call: one entry call at a time (main
thread). Effect descriptors are borrowed and must outlive the registry
(keep plugin libraries loaded until the registry is destroyed).

### Parameters and presets

`fx_params_new(fx, &env)` returns an initialized blob: zeroed, every
non-custom default written (`FX_COLOR_PRIMARY` and `FX_COLOR_SECONDARY`
resolve against `env`, or black and white without one), then
`init_params` for custom blobs. `fx_param_set` clamps to the prop range,
rounds ints and choices half away from zero, maps bools to 0 or 1 and
rejects NaN. `fx_params_clamp` repairs any blob (NaN becomes the default)
and is applied to the job's private copy, so effects always see in-range
values. Custom blobs are never touched by the host; effects must treat them
as untrusted (Curves and Levels sanitize theirs).

Preset text is `key=value;key=value`:

| Kind | Text |
|---|---|
| INT, CHOICE (index), SEED | decimal integer |
| BOOL | `0` or `1` (`true` and `false` accepted) |
| REAL, ANGLE | shortest decimal that round-trips exactly, `.` as the point whatever the C locale |
| COLOR | `#AARRGGBB` (`#RRGGBB` accepted as opaque) |
| POINT | `x,y` |
| CUSTOM | lowercase hex of the blob bytes (little-endian platforms) |

The parser reads exactly `len` bytes, rejects NUL bytes, inputs over 4 MiB,
malformed entries and malformed values for known keys; it ignores unknown
keys, lets later duplicates win, and clamps like `fx_param_set`. It parses
into a scratch copy and only copies back on success, so a failed load leaves
the caller's params untouched. Numbers are validated by a strict grammar
before `strtod`, so `inf`, `nan`, hex floats and trailing garbage are
rejected.

## Jobs

```
fx_job_create(fx, params, &src, &dst, &env, region, tile, prio_xy, &job)
    -> any number of workers: fx_job_work(job, i) until FX_WORK_FINISHED
       (FX_WORK_AGAIN: prepare() is running elsewhere, yield and retry)
    -> main thread each frame: fx_job_take_done(job, rects, n), fx_job_state(job)
    -> fx_job_cancel(job) at any time
    -> fx_job_destroy(job) once every fx_job_work call has returned
```

* The rendered area is `region` clipped to `env->sel`, `src->r` and
  `dst->r`. ROIs are the cells of a `tile x tile` grid anchored at the
  document origin (64 by default, matching layer tiles), clipped to that
  area. The grid is coarsened automatically above 2^20 cells.
  `FX_FLAG_SINGLE_THREAD` effects get one ROI covering the whole area.
* With a priority point (for example the viewport center) ROIs are handed
  out nearest first: squared distance between ROI center and point, ties in
  row-major order. The order never changes the pixels.
* `prepare()` runs once, on whichever thread calls `fx_job_work` or
  `fx_job_prepare` first. Workers arriving during it get `FX_WORK_AGAIN`
  immediately instead of blocking (the core has no blocking primitives).
* ROIs come from one atomic counter, so any number of workers share the
  queue without locks. A render returning `FX_CANCELLED` or `FX_ERROR`
  stops the job (state CANCELLED or FAILED).
* `fx_job_take_done` is single-consumer: it returns each finished ROI
  exactly once, after the ROI's pixels are visible to the caller (release
  store per completion slot, acquire load in the consumer). `fx_job_state`
  returns DONE only when every ROI finished; an acquire load of the
  remaining-ROI counter makes all pixels visible.
* `host->cancelled(job)` reads the job's cancel flag. For tests,
  `fx_job_set_cancel_after(job, n)` makes the n-th poll raise the flag and
  `fx_job_polls` counts polls.
* The job owns a clamped copy of the params blob (`fx_job_params`) and
  copies of the `fx_img` descriptors and `fx_env`; the pixels of src and dst
  are borrowed until `fx_job_destroy`. `release()` runs in
  `fx_job_destroy`.

`fx_run_sync` is the blocking form for final renders, dialog-less
adjustments and tests: it prepares on the calling thread and renders with
`pc_par_for` (NULL `par` = serial). `fx_run_sync_tiled` adds tile size and
priority point. FX_ERROR maps to `PC_ERR_NOMEM`, an effect that cancels
itself to `PC_ERR_CANCELLED`.

## App integration recipe

1. Startup: `reg = fx_registry_create(); fx_registry_add_builtins(reg);`,
   then plugins from the per-user folder: `pal_lib_open`, `pal_lib_sym(lib,
   FX_ENTRY_NAME)`, `fx_registry_add_entry(reg, fn)`. Build the Effects and
   Adjustments menus by walking `fx_registry_at` and splitting paths with
   `fx_menu_split`.
2. Invocation: snapshot the active layer into a contiguous BGRA `fx_img`
   (`pc_layer_read_rect`), allocate dst with the same rect, fill an `fx_env`
   (document size, selection bounds or the whole canvas, palette colors).
   `params = fx_params_new(fx, &env)`; restore the last used values with
   `fx_preset_load`. `FX_FLAG_NO_DIALOG` effects run at once with defaults.
3. Preview: on every parameter change cancel the running job, then create a
   new one with the viewport center as priority point and submit N pal tasks
   that loop `while (fx_job_work(job, i) == FX_WORK_AGAIN) SDL_Delay(0);`.
   Each frame, drain `fx_job_take_done` and upload those rects (blended
   through the selection coverage) to the canvas cache. Destroy old jobs only
   after their tasks report `pal_task_done`.
4. Commit: when `fx_job_state` is DONE (or after `fx_run_sync`), blend dst
   over the original through the selection mask and commit with a
   transaction. Save the params with `fx_preset_save` for the next run.

## Test helper for effect lanes

`tests/fx/fx_test_util.h` (header-only) gives every effect lane the same
checks: `fxt_check_effect(fx, params, w, h, seed)` renders a seeded noise
image with an unaligned selection and a smooth full-canvas image, and checks
byte equality across four tilings and thread counts plus `fx_run_sync`,
canary-protected ROI-only writes (also one ROI at a time), exact tiling of
the reported ROIs, and cancellation (pre-cancelled, mid-run, and a single
large ROI that must notice `host->cancelled` within a few polls). Real
threads come from C11 `<threads.h>` (pthreads in ThreadSanitizer builds);
elsewhere workers are interleaved on one thread. `fxt_registry()` returns a
registry with all built-ins. `test_fx_host.c` also validates every
built-in descriptor from every lane (`t_builtins_valid`).
