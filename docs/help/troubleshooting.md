# Settings and troubleshooting

## Where paint.c keeps its files

| What | Where on this computer |
|---|---|
| Settings (settings.ini) | `{{config_dir}}` |
| Plugins | `{{plugin_dir}}` |
| This help | `{{help_dir}}` |

Recovery copies of unsaved images and crash logs live in the per-user state
folder; Settings > Diagnostics shows every folder and has buttons to open
the crash log folder and to copy a report for bug reports.

## When something goes wrong

- **The program does not start or the canvas stays black**: start it once
  with hardware acceleration off: `paintc --set gfx.software=1` (or
  `paintc --software` for one run). Settings > Graphics turns it back on.
- **A window is lost off screen**: {{ctrl}}+Shift+F5 to F8 put the Tools,
  History, Layers or Colors window back at its default place.
  `paintc --reset-windows` resets all of them at start.
- **A plugin misbehaves**: start with `paintc --disable-plugins` and check
  Effects > Plugin Errors.
- **paint.c stopped unexpectedly**: start it again. It offers to restore
  every image that had unsaved changes. Settings > Diagnostics shows how
  many crash logs exist.
- **Something else**: `paintc --diagnostics` prints the version, the
  folders, the processors, the memory and the available graphics drivers,
  even when no window can open.

## Command line

| Option | Effect |
|---|---|
| `paintc FILE ...` | Opens the files; a running paint.c opens them in its window |
| `--set KEY=VALUE` | Changes a setting (repeatable); a running paint.c applies it at once |
| `--config-dir DIR` | Keeps the settings in DIR; each settings folder has its own window |
| `--software` | Software rendering for this run |
| `--reset-windows` | Puts the utility windows back at their default places |
| `--disable-plugins` | Loads no plugins |
| `--diagnostics` | Prints a report and exits |
| `--version` | Prints the version |

Keys you can change with `--set` include `gfx.software` (1 = no hardware
acceleration), `gfx.workers` (number of worker threads, 0 = automatic),
`ui.theme` (0 = follow the system, 1 = light, 2 = dark, 3 = blue) and
`history.limit_mb` (History memory per image, 0 = automatic).

## Getting help and reporting problems

{{getting_help}}

This guide is part of paint.c and always available offline.
