# paint.c 0.1.0

paint.c is a native raster image editor written in C17 that reproduces the
workflow of Paint.NET 5.1 on Linux (Wayland and X11), Windows and macOS:
the same tools, menus, adjustments, effects, layer blend modes and keyboard
shortcuts, in a single small executable with no runtime to install.

paint.c is an independent clean-room project. It is **not affiliated with
or endorsed by dotPDN LLC**; Paint.NET is their trademark. Behavior was
reproduced from Paint.NET's documentation and from observing the running
program. The only Paint.NET source it builds on is the MIT-licensed
Paint.NET 3.36 release: the layer blend math and several adjustment,
effect and file-saving algorithms are re-implemented from it in C and
attributed in NOTICE. No later Paint.NET code and no Paint.NET assets,
icons or texts are used.

This is the first release. Parity with Paint.NET 5.1.12 is tracked row by
row in docs/inventory/PARITY.md (1469 rows: 1449 done, 20 not
applicable, none partial or open); see Known gaps below.

## Downloads

| File | For |
|---|---|
| `paintc-0.1.0-linux-x86_64.AppImage` | Linux x86_64 with glibc 2.31 or newer (Ubuntu 20.04, Debian 11, Fedora 32 and later), Wayland or X11 |
| `paintc-0.1.0-windows-x64-setup.exe` | Windows 10 22H2 or Windows 11, x64: installer with Start menu entry and file associations |
| `paintc-0.1.0-windows-x64-portable.zip` | the same program without installation |
| `paintc-0.1.0-source.tar.gz` | source code; macOS users build from it (docs/BUILDING.md, section macOS) |
| `SHA256SUMS.txt` | SHA-256 of every file above |

* **Linux**: `chmod +x paintc-0.1.0-linux-x86_64.AppImage` and run it.
  Without FUSE, run it with `--appimage-extract-and-run`. The AppImage
  holds one self-contained executable; SDL3 loads the X11 or Wayland
  libraries (and libdecor for window decorations on GNOME) of the system
  at run time.
* **Windows**: the installer is per machine (Program Files) and registers
  "Open with" entries for every supported image type; .pdn opens with
  paint.c only when no other program claims it. The installer and the
  executable are not code signed, so SmartScreen warns on first start
  (More info, Run anyway). The portable zip keeps settings in `%APPDATA%`
  unless started with `--config-dir DIR`.
* **macOS**: no prebuilt package in this release (see Platforms). Build
  from the source archive: Homebrew `cmake ninja nasm`, one CMake
  configure and build, then `packaging/macos/make-dmg.sh` or a plain
  install, both signed ad hoc (docs/BUILDING.md has the exact steps).

## Features

* **Tools (19)**: Rectangle Select, Ellipse Select, Lasso Select, Magic
  Wand, Move Selected Pixels, Move Selection, Zoom, Pan, Paint Bucket,
  Gradient (7 types), Paintbrush, Eraser, Pencil, Color Picker, Clone
  Stamp, Recolor, Text (system fonts, IME input), Line/Curve and Shapes
  (29 shapes), with the toolbar options of Paint.NET (antialiasing, blend
  mode, selection modes, tolerance, fill patterns, pen pressure).
* **Layers**: the 14 classic blend modes with Paint.NET 3.36 integer math,
  opacity, visibility, properties, duplicate, merge down, import from
  file, flip and Rotate/Zoom.
* **Adjustments (13)**: Auto-Level, Black and White, Brightness /
  Contrast, Curves, Exposure, Highlights / Shadows, Hue / Saturation,
  Invert Alpha, Invert Colors, Levels, Posterize, Sepia, Temperature /
  Tint.
* **Effects (42)**, all with live preview:
  Artistic (Ink Sketch, Oil Painting, Pencil Sketch);
  Blurs (Bokeh, Fragment, Gaussian, Median, Motion, Radial, Sketch,
  Square, Surface, Zoom);
  Color (Quantize);
  Distort (Bulge, Crystalize, Dents, Frosted Glass, Morphology, Pixelate,
  Polar Inversion, Tile Reflection, Twist);
  Noise (Add Noise, Reduce Noise);
  Object (Drop Shadow, Feather Object, Outline Object);
  Photo (Glow, Red Eye Removal, Sharpen, Soften Portrait, Straighten,
  Vignette);
  Render (Clouds, Julia Fractal, Mandelbrot Fractal, Turbulence);
  Stylize (Edge Detect, Emboss, Outline, Relief).
* **Image and edit commands**: resize, canvas size, rotate, flip, crop to
  selection, flatten, cut / copy / copy merged / paste (into a new layer
  or image), fill and erase selection, select all, invert selection.
* **History**: unlimited undo and redo within a memory budget (25 percent
  of RAM by default), History window.
* **Files**:
  * `.pdn` read **and write**: the reader opens files from Paint.NET 3.0
    to 5.1.12 (checked against the independent pypdn reader on 489 real
    files); the writer reproduces Paint.NET's layered file structure.
  * **AVIF** and **JPEG XL** read and write, with Paint.NET's save options
    (quality, lossless, chroma subsampling, encoder preset or effort),
    Exif and XMP metadata, HDR (PQ, HLG) images tone mapped to 8 bits.
  * PNG, JPEG, BMP, GIF, TIFF, WebP, TGA, DDS (BC1 to BC7) and OpenRaster
    (.ora) read and write; Save Configuration dialog with a live preview
    and the file size (with a computing percentage).
  * ICC profiles are converted on import and kept on export.
  * Autosave and crash recovery, recent files, drag and drop, single
    instance (files opened later go to the running window).
* **Plugins**: effect plugins load from the per-user plugins folder and,
  for portable installs, from a `plugins` folder next to the executable
  (`--disable-plugins` turns them off).
* **Interface**: Paint.NET's menus, floating Tools, History, Layers and
  Colors windows, image tabs, rulers, pixel grid, light and dark themes,
  UI scaling for high DPI displays, Paint.NET's keyboard shortcuts.

## Platforms and how they were verified

GitHub Actions was unavailable for the final builds (billing), so every
package of this release was built and checked locally.

* **Linux** (built in an Ubuntu 20.04 container, glibc 2.31, GCC 13;
  `packaging/linux/build-appimage-docker.sh`): all 183 tests passed
  inside the container; `objdump` shows no symbol newer than GLIBC_2.29
  (the floor is 2.31) and the executable needs only glibc (libstdc++ and
  libgcc are linked statically, SDL3 and the codecs are built in). The
  AppImage passed `--self-test --headless` inside that container and on
  Debian 13, then a scripted editing session (painting,
  layers, Clouds, Shapes, Text, Invert Colors, the Effects menu and the
  Twist dialog, saving and reopening .pdn, AVIF, JPEG XL and PNG) with
  screenshots on **X11** (Xvfb) and **Wayland** (headless Weston, window
  decorations through the system libdecor), and a normal start with a .pdn
  on both.
* **Windows** (cross built with mingw-w64 GCC 14 against the Universal
  CRT; `packaging/windows/build-mingw-release.sh`): all 183 tests passed
  under Wine 10; the installer was run silently (`/S`) in a clean Wine
  prefix and its files, Start menu entry and registry entries checked; the
  installed `paintc.exe` and the one from the portable zip passed
  `--self-test --headless`; the installed one ran the same scripted
  session with screenshots on Xvfb and opened a .pdn in a normal window;
  the uninstaller (`/S`) removed the files, the shortcut and the registry
  entries. Earlier CI runs built and tested paint.c natively with MSVC
  (`/W4 /WX`), clang-cl and MinGW + Wine (all jobs green on commit
  'ADR-017'; MinGW + Wine and clang-cl green again on the integration
  branch in runs 37324304799 and 37331223253). This release has not been
  run on a physical Windows machine.
* **macOS**: verified by the all-green CI run on commit 'ADR-017' (macOS 14
  arm64 and macOS 15 Intel, deployment target 13.0) and by run 37331223253
  (configure, build and tests up to test_shell_crash). Changes made after
  that run, including the bundled AVIF and JPEG XL build on macOS and the
  test_shell_crash fix, have **not been verified on a Mac** (ADR-021).
  Reports of macOS build results are welcome.

## Known gaps

* **Not applicable (N/A in PARITY.md)**:
  * maximum image size is 65535 pixels per side (Paint.NET: 262144;
    ADR-014), also in Resize and Canvas Size;
  * HEIC and JPEG XR are not supported: Paint.NET reaches them only
    through Windows system codecs (ADR-019);
  * opening http(s) URLs from the Open dialog (ADR-020);
  * File > Print and File > Acquire (scanners and cameras) are out of
    scope: Print is shown disabled, Acquire is hidden;
  * no update checker (the Updates settings page is not applicable),
    no Windows Advanced Color (HDR display) setting, Help > Donate hidden;
  * Red Eye Removal has Paint.NET 5.1's single Strength control, not the
    3.36 Tolerance slider.
* **Text**: no complex script shaping (ADR-004): right-to-left text and
  scripts that need shaping (Arabic, Indic scripts) are drawn character
  by character.
* **AVIF**: libavif decodes at most 16384 x 16384 pixels in total; saving
  from the app runs the encoder on one thread (Slow and Very Slow presets
  are slow on large images); HDR tone mapping is paint.c's own.
* **Reference data**: parity was checked against the Paint.NET 5.1
  documentation and Paint.NET 5.2 beta running under Wine, not against
  5.1.12 on Windows (ADR-009, ADR-016).
* **Signing**: Windows binaries are unsigned; macOS builds are signed ad
  hoc only (Gatekeeper asks on first start of a copied app).

## Checksums (SHA-256)

```
@CHECKSUMS@
```

`SHA256SUMS.txt` in the release has the same lines; check with
`sha256sum -c SHA256SUMS.txt` (Linux) or `shasum -a 256 -c SHA256SUMS.txt`
(macOS).

## License and attribution

paint.c is MIT licensed (`LICENSE`). `NOTICE` carries the MIT attribution
of the algorithms re-implemented from Paint.NET 3.36. Every package has a `licenses` folder
(Windows: next to `paintc.exe`; Linux: `usr/share/doc/paintc/licenses`
inside the AppImage; macOS: `paint.c.app/Contents/Resources/licenses`)
with paint.c's license, the NOTICE and the license texts of every bundled
component, indexed by its README.txt (source: `packaging/licenses/`):
SDL3, zlib, libspng, libjpeg-turbo, libwebp, Little-CMS, bcdec, bc7enc,
stb, the Inter font (SIL OFL 1.1), libavif, libaom (with the AOM patent
license), libjxl (with its patent grant), Highway, Brotli and skcms.
