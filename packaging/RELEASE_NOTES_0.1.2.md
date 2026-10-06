# paint.c 0.1.2

paint.c as a Flatpak. It changes the app id to
`io.github.vancevoj.paintc`, makes paint.c behave inside the Flatpak
sandbox and adds a Flatpak bundle to the downloads. Features, platforms
and known gaps are otherwise those of
[paint.c 0.1.1](https://github.com/vancevoj/paint.c/releases/tag/v0.1.1)
and [0.1.0](https://github.com/vancevoj/paint.c/releases/tag/v0.1.0)
(their notes: `packaging/RELEASE_NOTES_0.1.1.md`, `RELEASE_NOTES_0.1.0.md`).

paint.c is an independent clean-room project. It is **not affiliated with
or endorsed by dotPDN LLC**; Paint.NET is their trademark.

## Downloads

| File | For |
|---|---|
| `paintc-0.1.2-linux-x86_64.flatpak` | Linux x86_64 with Flatpak: open it in GNOME Software or KDE Discover, or `flatpak install --user paintc-0.1.2-linux-x86_64.flatpak`; the runtime (org.freedesktop.Platform 26.08) comes from Flathub |
| `paintc-0.1.2-linux-x86_64.AppImage` | Linux x86_64 with glibc 2.31 or newer (Ubuntu 20.04, Debian 11, Fedora 32 and later), Wayland or X11 |
| `paintc-0.1.2-windows-x64-setup.exe` | Windows 10 22H2 or Windows 11, x64: installer with Start menu entry and file associations |
| `paintc-0.1.2-windows-x64-portable.zip` | the same program without installation |
| `paintc-0.1.2-source.tar.gz` | source code; macOS users build from it (docs/BUILDING.md, section macOS) |
| `SHA256SUMS.txt` | SHA-256 of every file above |

Installing and running otherwise work as in 0.1.0. Settings, recent files
and plugins of earlier versions are kept: their folders are named `paintc`
and do not depend on the app id. The Flatpak keeps its own settings under
`~/.var/app/io.github.vancevoj.paintc/`.

## What changed

### New app id

The app id is now `io.github.vancevoj.paintc` (ADR-025) everywhere it is
used: the desktop entry, AppStream metadata, icons and the MIME package on
Linux, the Wayland app id and X11 window class, the single instance name,
the macOS bundle identifier and the Windows assembly name. Effect ids keep
their `org.paintc.` prefix, so saved effect settings and plugins are
unaffected.

* Linux packages installed by hand (`cmake --install`) leave the old
  `org.paintc.paintc` desktop entry, icons and MIME file behind; remove
  them from the install prefix when you upgrade.
* A paint.c 0.1.1 that is still running does not receive files opened with
  0.1.2 (and the other way round), because the single instance name
  changed.

### Flatpak

* Opening a file while paint.c runs in the Flatpak now reaches the open
  window. The single instance socket lives in the sandbox's shared runtime
  folder (`$XDG_RUNTIME_DIR/app/io.github.vancevoj.paintc`): abstract
  sockets, which paint.c uses elsewhere on Linux, do not cross the network
  namespaces of separate sandboxes.
* The Text tool lists the fonts installed on the host system and in the
  user's font folder, which Flatpak exposes under `/run/host`.
* The Flatpak manifest (`packaging/flatpak/io.github.vancevoj.paintc.yml`)
  targets the org.freedesktop.Platform 26.08 runtime and takes SDL3,
  libavif and libjxl from it. The Flatpak sees the Pictures folder
  directly and everything else through the file chooser portal, whose
  files stay available to recent files after a restart. The optional
  plugins go to
  `~/.var/app/io.github.vancevoj.paintc/data/paintc/plugins/`.

### Other

* Builds with GCC 16 without warnings, and a new CMake option
  `PC_WERROR` (default ON) lets distribution builds keep warnings from
  failing the build (ADR-026).
* The Text tool logs how many font faces it found, or why the font scan
  failed, to the log file.

* AppStream metadata with screenshots, links and branding colors for
  software centers (`packaging/screenshots/` and
  `packaging/screenshots/make-screenshots.sh`, which renders them).
* The script command list in `src/app/script.c` names `fit` as a command of
  its own (`zoom fit` zoomed to 1%).

## Optional plugins

The ten optional plugins of the
[plugins-v1.0.0 release](https://github.com/vancevoj/paint.c/releases/tag/plugins-v1.0.0)
work with 0.1.2 unchanged. Install them as described in docs/PLUGINS.md;
the Flatpak's plugins folder is named above.

## Platforms and how they were verified

Same platforms and system requirements as 0.1.0. Packages are built with
the local release pipeline (docs/PACKAGING.md, Local releases). Every
CTest entry passes on Linux in a GCC 14 Release build and in a GCC 16
build inside the Flatpak SDK 26.08 against its SDL 3.4.14. The Flatpak was
built with org.flatpak.Builder and checked in its sandbox: the self test,
a window on Wayland, opening a file into the running instance, saving over
a file handed in by the document portal and the host fonts in the Text
tool. macOS remains unverified on a Mac since the last green
macOS CI run (ADR-021).

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
