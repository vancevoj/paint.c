# Platform layer (pal) contract notes

`include/pal/pal.h` v1 is frozen. This page records what the header leaves
open: error values, ownership, threads and per-OS behavior of the
implementation in `src/pal`. `include/pal/pal_clip_raw.h` is an additive
extension owned by lane L0.

## Lifecycle

| Function | Thread | Notes |
|---|---|---|
| `pal_init(app_id, org, app)` | main | After `SDL_Init`. Idempotent (a second call returns true). Folder names come from `app` (falls back to the last `app_id` component): Linux and Windows use it lowercased with only `[a-z0-9_-]` kept (`"paint.c"` gives `paintc`), macOS uses it as is. `org` is unused. Creates CONFIG, DATA, CACHE and STATE, opens the log file, routes SDL's log output into pal. False only when no home or profile folder can be determined. Call it as `pal_init("org.paintc.paintc", "paintc", "paint.c")`. |
| `pal_quit()` | main | Stops the single-instance listener, drops undelivered dialog results and forwarded paths, restores SDL's log function, frees every string returned by `pal_dir` and `pal_font_dirs`. |
| `pal_pump()` | main | Delivers dialog results and forwarded paths through their callbacks (in arrival order) and reaps helper processes. Call once per frame. |

## Logging, time, machine (any thread)

- `pal_log` writes `[seconds] L message` to stderr, to
  `<STATE>/paintc.log` (opened for append and shared, so it can be read
  while the app runs; rotated to `.old` at 1 MiB) and on Windows to
  `OutputDebugString`. Lines longer than 1 KiB are allocated, a trailing
  newline is dropped, levels outside 0..3 clamp. Default minimum level is
  INFO; `PAINTC_LOG=debug|warn|error|off` changes it (`off` disables only
  the file). Works before `pal_init` (stderr only).
- `pal_ticks_ns` is `SDL_GetTicksNS` (monotonic). `pal_cpu_count` is the
  logical CPU count clamped to the process affinity (Linux, Windows).
  `pal_ram_bytes` is `SDL_GetSystemRAM` in bytes. `pal_cpu_features` uses
  SDL's checks (AVX2 includes the OS XSAVE check);
  `PAINTC_CPU_FEATURES=<mask>` clears bits for testing scalar paths.

## Worker pool

- `pal_pool_create(0)` starts `pal_cpu_count() - 1` workers (at least 1,
  at most 1024). Returns NULL on failure. Main thread.
- `pal_pool_par(p)` returns `{ run, self = p, threads = workers + 1 }`;
  NULL gives the serial `pc_par` (threads 1). In `run` the caller is
  worker 0 and pool thread k is worker k, unique within the call. Several
  threads may run parallel-fors on one pool at the same time. Nested use
  from inside a job item also completes (the nested caller does the work
  itself), although the header asks callers not to rely on it.
- Parallel-for jobs take priority over background tasks; while all workers
  run long tasks the caller completes the job alone.
- `pal_task_submit` returns NULL (and does not run fn) on OOM or bad
  arguments. `pal_task_done(NULL)` is true. `pal_task_wait` on a task that
  has not started runs it on the calling thread, so waiting is safe from
  anywhere, including a worker. `pal_task_free` on a pending task waits
  first (and logs a warning). Results written by fn are visible to a thread
  that saw `pal_task_done` return true (acquire/release).
- `pal_pool_destroy` runs every queued task, then joins the workers. No
  `run` may be in progress. NULL-safe.
- `pal_mutex_*` wrap `SDL_Mutex` and are NULL-safe.

## Files (any thread)

- Paths are UTF-8. On Windows they become UTF-16; paths of 240 characters
  or more get the `\\?\` (or `\\?\UNC\`) prefix, so long paths work without
  the system-wide opt-in. Invalid UTF-8 fails like a missing file.
- `pal_read_file`: `PC_ERR_ARG` (NULL arguments), `PC_ERR_IO` (missing,
  directory, read error), `PC_ERR_LIMIT` (larger than `max_bytes`, checked
  before allocating and again while reading), `PC_ERR_NOMEM`. On failure
  `*data` is NULL and `*len` 0. On success the buffer is from `malloc`
  (caller frees) with a NUL after the data.
- `pal_write_file_atomic`: `PC_ERR_ARG` for an empty path, `data == NULL`
  with `len > 0`, or a path naming a directory; `PC_ERR_IO` otherwise. The
  temp file is `.<name>.<random>.tmp` in the same folder. POSIX: created
  `O_EXCL` with mode 0600, `fsync` (`F_FULLFSYNC` on macOS), `rename`, then
  the final mode (the old file's mode, or `0666 & ~umask`) through the
  descriptor, then the folder is fsynced. Saving through a symlink writes
  the link target. Windows: `CREATE_NEW`, `FlushFileBuffers`, then
  `ReplaceFileW` for existing files (keeps attributes and ACL), else
  `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; sharing errors (virus
  scanners) are retried for about 300 ms, so a read-only file fails with
  `PC_ERR_IO` after that delay. On POSIX the folder's permissions decide,
  as for every rename-based save. Not preserved: hard links, extended
  attributes and POSIX ACLs of the old file.
- `pal_file_exists` is true for any existing entry (file or folder).
  `pal_remove` removes a file or an empty folder (Windows clears the
  read-only attribute first, like `unlink`). `pal_mkdirs` returns true when
  the folder exists afterwards. `pal_file_mtime` is 0 for missing entries.
- `pal_list_dir`: every entry except `.` and `..` (hidden ones included),
  filtered by `glob` (`*`, `?`, several patterns separated by `;`, ASCII
  case-insensitive), sorted by ASCII case-insensitive order with byte
  order breaking ties. Never negative: 0 with `*names == NULL` for empty,
  missing or unreadable folders and on OOM. Windows skips names that are
  not valid UTF-16. Free with `pal_free_names` (NULL-safe).

## Path helpers (pure, any thread)

`/` and `\` both separate components on every OS; drive letters and UNC
roots are recognized in Windows builds only. Results never split a UTF-8
sequence when truncated, and `out` may alias an input.

- `pal_path_join(a, b)`: `b` when `a` is empty or `b` is absolute, `a` when
  `b` is empty; otherwise one separator in the style `a` already uses
  (native when mixed or none).
- `pal_path_basename`: after the last separator, so `"a/b/"` gives `""`.
- `pal_path_dirname`: before the last separator with trailing separators
  removed, roots kept (`"/x"` gives `"/"`, `"C:\x"` gives `"C:\"`), and
  `""` for a bare name.
- `pal_path_ext`: after the last dot of the base name; `".hidden"` and
  `"name."` give `""`.

## Folders

| Kind | Linux, BSD | Windows | macOS |
|---|---|---|---|
| CONFIG | `$XDG_CONFIG_HOME/paintc/` (`~/.config`) | `%APPDATA%\paintc\` | `~/Library/Application Support/paint.c/` |
| DATA | `$XDG_DATA_HOME/paintc/` (`~/.local/share`) | `%APPDATA%\paintc\` | same as CONFIG |
| CACHE | `$XDG_CACHE_HOME/paintc/` (`~/.cache`) | `%LOCALAPPDATA%\paintc\Cache\` | `~/Library/Caches/paint.c/` |
| STATE | `$XDG_STATE_HOME/paintc/` (`~/.local/state`) | `%LOCALAPPDATA%\paintc\State\` | `.../Application Support/paint.c/State/` |
| DOCUMENTS, PICTURES | `SDL_GetUserFolder`, falling back to home (pictures to documents) | same | same |
| EXE | `SDL_GetBasePath` | same | same |

XDG variables count only when absolute. Missing per-user folders are
created with mode 0700 (POSIX), also when they vanish while running (one
`stat` per `pal_dir` call). CACHE is never shared with other kinds, so it
can be wiped. The strings stay valid until `pal_quit`; NULL before
`pal_init` or for an invalid kind.

`pal_font_dirs` lists roots to scan recursively (subfolders are not
listed): Linux `/usr/share/fonts/`, `/usr/local/share/fonts/`,
`$XDG_DATA_HOME/fonts/`, `~/.fonts/` and `<each XDG_DATA_DIRS>/fonts/`;
Windows `%WINDIR%\Fonts\` and `%LOCALAPPDATA%\Microsoft\Windows\Fonts\`;
macOS `/System/Library/Fonts/` (and `Supplemental/`), `/Library/Fonts/`,
`~/Library/Fonts/`. Entries may not exist.

## Dialogs (main thread)

Filters and the default location are copied, so the caller's arrays may be
temporary. Every call produces exactly one callback through `pal_pump`,
including errors (reported as a cancel, with a log line). The `filter`
index is -1 when the user cancelled or the platform cannot tell.

## Clipboard (main thread)

- macOS, X11, Wayland: SDL's MIME clipboard; everything fails (returns
  false or NULL) until SDL's video subsystem is initialized. Paste accepts
  `image/png`, `image/bmp` (and the `x-bmp` aliases, reported as
  `image/bmp`), `image/tiff`, `image/webp`, `image/jpeg`, `image/gif`, in
  that order; `*mime` receives the type. `pal_clip_set_image_png` offers
  `image/png`; `pal_clip_set_image_bgra` adds `image/bmp` (X11, Wayland) or
  `image/tiff` (macOS).
- Windows: native, no SDL video needed. Paste prefers the registered "PNG"
  format, then CF_DIBV5, then CF_DIB (a validated DIB becomes a BMP file,
  `image/bmp`). Copy offers "PNG" and, with `pal_clip_set_image_bgra`,
  CF_DIBV5 with an alpha mask (Windows synthesizes CF_DIB and CF_BITMAP).
  Text uses CF_UNICODETEXT with CRLF conversion.
- Returned buffers come from `malloc` (caller frees). PNG bytes from the
  Windows clipboard may carry trailing padding after IEND.
- Note for the BMP codec: 32-bit BI_RGB DIBs from screenshots usually have
  alpha 0 everywhere; treat an all-zero alpha channel as opaque.

## Dynamic libraries (main thread)

`pal_lib_open` loads exactly the given file: Windows uses the full path
with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` and
suppresses error dialogs; POSIX uses `dlopen(RTLD_NOW | RTLD_LOCAL)` and
prefixes a bare name with `./` so no search path is consulted.
`pal_lib_sym` returns an object pointer; convert it to a function pointer
with `memcpy`. `pal_init` also removes the current folder from the Windows
DLL search order (`SetDllDirectoryW(L"")`).

## Shell (main thread)

- `pal_open_url` is `SDL_OpenURL`.
- `pal_reveal_file`: Windows `SHOpenFolderAndSelectItems` (fallback
  `explorer /select,`), macOS `open -R`, Linux and BSD the freedesktop
  `org.freedesktop.FileManager1.ShowItems` call via `dbus-send`; when that
  fails, `pal_pump` opens the parent folder instead. Helper processes are
  never waited for.

## Single instance (main thread, after `pal_init`)

`pal_single_instance(app_id, n, paths, cb, ud)`: `n`/`paths` are the file
arguments only (not argv[0] or options), UTF-8 (on Windows take them from
`SDL_main`, see BUILDING.md). Relative paths are made absolute before they
are forwarded. The first instance gets `cb(ud, paths, n, -1)` from
`pal_pump` for every later launch; `n == 0` (paths NULL) means "launched
again without files", a cue to raise the window. Calling it again in the
first instance only replaces cb and ud.

- Linux: abstract socket `@<app_id>.si.<uid>`; the peer's uid is checked
  in both directions. BSD (or Linux with `PAINTC_SI_FILE=1`): socket file
  and `fcntl` lock file in `$XDG_RUNTIME_DIR` or STATE.
- Windows: mutex `Local\<app_id>.si` plus pipe
  `\\.\pipe\<app_id>.si.<session>` (remote clients rejected, other users
  cannot write, squatting refused with FILE_FLAG_FIRST_PIPE_INSTANCE).
- macOS: always true (Finder sends documents as `SDL_EVENT_DROP_FILE`).
- Any failure (no runtime folder, unresponsive first instance after 3 s)
  returns true, so the app runs on as an independent instance.

Messages are at most 1 MiB, 4096 paths of at most 32 KiB each; malformed
messages are ignored.
