# paint.c 0.1.1

A maintenance release of paint.c 0.1.0: native Wayland on more Linux
desktops, a Display server setting, the plugin host features that the
optional plugins use, and a fix for a canvas that kept part of a closed
image on screen. Features, platforms and known gaps are otherwise those of
[paint.c 0.1.0](https://github.com/vancevoj/paint.c/releases/tag/v0.1.0)
(its notes: `packaging/RELEASE_NOTES_0.1.0.md`).

paint.c is an independent clean-room project. It is **not affiliated with
or endorsed by dotPDN LLC**; Paint.NET is their trademark.

## Downloads

| File | For |
|---|---|
| `paintc-0.1.1-linux-x86_64.AppImage` | Linux x86_64 with glibc 2.31 or newer (Ubuntu 20.04, Debian 11, Fedora 32 and later), Wayland or X11 |
| `paintc-0.1.1-windows-x64-setup.exe` | Windows 10 22H2 or Windows 11, x64: installer with Start menu entry and file associations |
| `paintc-0.1.1-windows-x64-portable.zip` | the same program without installation |
| `paintc-0.1.1-source.tar.gz` | source code; macOS users build from it (docs/BUILDING.md, section macOS) |
| `SHA256SUMS.txt` | SHA-256 of every file above |

Installing and running work as in 0.1.0: `chmod +x` and run the AppImage
(`--appimage-extract-and-run` without FUSE); the Windows installer and
executable are not code signed, so SmartScreen warns on first start (More
info, Run anyway). Settings, recent files and plugins of 0.1.0 are kept.

## What changed

### Linux: native Wayland on more compositors

* SDL 3.2 and later run a program through XWayland when the Wayland
  compositor lacks the `wp_fifo_v1` protocol, to protect the frame pacing
  of games. That affected KDE Plasma (KWin before 6.4) and other
  compositors without the protocol: paint.c 0.1.0 ran there as an X11
  window. paint.c redraws only on demand, so on a Wayland session it now
  asks for the native Wayland driver first, which brings fractional
  scaling, pen input through the tablet protocol and crisp text. When
  Wayland cannot be used it falls back to X11 as before.
* **Settings > Graphics > Display server** chooses between **Native
  Wayland when available** (the default) and **X11 (XWayland on Wayland
  sessions)**. The choice is stored as `gfx.video_driver` (`wayland` or
  `x11`) and takes effect at the next start. The `SDL_VIDEO_DRIVER`
  environment variable still overrides both. Windows and macOS are
  unchanged.

### Plugin host: what the optional plugins need (effect ABI v1.2)

The effect interface grew by additions only (ADR-024): `FX_ABI_VERSION`
stays 1, plugins built for 0.1.0 keep working, and plugins that use the
new parts still load in 0.1.0, with the fallbacks named below.

* **Effect notices**: an effect can say why it left the image unchanged
  (`fx_host.notice`), and paint.c shows the message in a message box, for
  example Align Object with nothing to align, Bevel Object without an
  object or Content Aware Fill without a selection. paint.c 0.1.0 leaves
  the image unchanged without a message.
* **Position grid widget** (`include/fx/fx_widgets.h`, hint
  `position-grid`): a 3 x 3 grid of position buttons, optional rows of
  horizontal-only and vertical-only positions and a Reset position button,
  usable by mouse and keyboard. Hosts without it, 0.1.0 included, show a
  drop-down with the same choices.
* **Tooltips** on check boxes, drop-downs and seeds through `tip:` hints,
  and values that apply to the live preview only (`FXP_F_PREVIEW_ONLY`):
  they are reset for OK, Repeat and runs without a dialog.
* The plugin guide (Help > Plugins, docs/PLUGINS.md) documents
  `fx_widgets.h` with the other plugin headers.

### Fixed: canvas showing part of a closed image

After closing one image and opening another in the same session, part of
the new image's first 64 pixel tile row could stay transparent on screen,
or show pixels of the closed image, until those tiles were drawn again (an
edit there, or another zoom level). The image itself was never affected
(saving gave the right pixels). The page
textures that mirror the display cache told caches apart by their memory
address, and the new image's cache could be created at the address of the
closed one with tile stamps numbered the same way. Every display cache now
has an identity and stamps that are never reused in the process, and the
canvas keys its textures on them. Switching between open images and
undoing Image > Resize are covered by the same tests
(`tests/app/test_canvas_docs.c`, `tests/core/test_mip.c`).

## Optional plugins

Ten effect plugins are available as separate downloads, not part of the
paint.c packages: AA's Assistant, Align Object, Bevel Object, Content
Aware Fill, Gradient Mapping, Grid / Checkerboard, Grim Color Reaper,
Perspective, Shape3D and Water Reflection, each a native re-creation of a
popular Paint.NET plugin that credits its original author.

* Download: the
  [plugins-v1.0.0 release](https://github.com/vancevoj/paint.c/releases/tag/plugins-v1.0.0)
  (one zip per plugin and system, or `paintc-plugins-all-1.0.0-<system>.zip`,
  with `SHA256SUMS`).
* Install: unzip into the plugins folder (Linux
  `~/.local/share/paintc/plugins/`, Windows `%APPDATA%\paintc\plugins\`)
  and restart paint.c; details, the list with menus and versions, and how
  to build them from source are in docs/PLUGINS.md.
* They run best on paint.c 0.1.1, which has the notices and the position
  grid widget described above.

## Platforms and how they were verified

Same platforms and system requirements as 0.1.0: Linux x86_64 (glibc 2.31
or newer, Wayland or X11), Windows 10 22H2 and Windows 11 x64, and macOS
13 or newer built from source. Packages are built with the local release
pipeline of 0.1.0 (docs/PACKAGING.md, Local releases). Every CTest entry
passes on Linux in a GCC Release build, a Clang build and an AddressSanitizer
plus UndefinedBehaviorSanitizer build (leak checks on), with no compiler
warnings. macOS remains unverified on a Mac since the last green macOS CI
run (ADR-021).

## Checksums (SHA-256)

```
@CHECKSUMS@
```

`SHA256SUMS.txt` in the release has the same lines; check with
`sha256sum -c SHA256SUMS.txt` (Linux) or `shasum -a 256 -c SHA256SUMS.txt`
(macOS).

## License and attribution

paint.c is MIT licensed (`LICENSE`). `NOTICE` carries the MIT attribution
of the algorithms re-implemented from Paint.NET 3.36. Every package has a
`licenses` folder with paint.c's license, the NOTICE and the license texts
of every bundled component, as in 0.1.0.
