# paint.c 0.1.3

A packaging fix release of
[paint.c 0.1.2](https://github.com/vancevoj/paint.c/releases/tag/v0.1.2)
(its notes: `packaging/RELEASE_NOTES_0.1.2.md`). The program itself is
unchanged.

paint.c is an independent clean-room project. It is **not affiliated with
or endorsed by dotPDN LLC**; Paint.NET is their trademark.

## Install

Linux with Flatpak: [install paint.c](https://vancevoj.github.io/paintc-flatpak/io.github.vancevoj.paintc.flatpakref)
(open it with GNOME Software or KDE Discover; updates then arrive with your
other apps), or
`flatpak install --user https://vancevoj.github.io/paintc-flatpak/io.github.vancevoj.paintc.flatpakref`.
Installed copies of 0.1.2 from that repository update to 0.1.3 on their own.

## Downloads

| File | For |
|---|---|
| `paintc-0.1.3-linux-x86_64.flatpak` | Linux x86_64 with Flatpak, without the repository (no automatic updates); the runtime (org.freedesktop.Platform 26.08) comes from Flathub |
| `paintc-0.1.3-linux-x86_64.AppImage` | Linux x86_64 with glibc 2.31 or newer (Ubuntu 20.04, Debian 11, Fedora 32 and later), Wayland or X11 |
| `paintc-0.1.3-windows-x64-setup.exe` | Windows 10 22H2 or Windows 11, x64: installer with Start menu entry and file associations |
| `paintc-0.1.3-windows-x64-portable.zip` | the same program without installation |
| `paintc-0.1.3-source.tar.gz` | source code; macOS users build from it (docs/BUILDING.md, section macOS) |
| `SHA256SUMS.txt` | SHA-256 of every file above |

## What changed

* The desktop entry no longer has the `SingleMainWindow` key, which
  desktop-file-validate before 0.28 rejects (Ubuntu 24.04 and older, and
  the AppImage catalog's checks).
* The AppImage's own icon (`.DirIcon`) is the 256 x 256 PNG instead of the
  SVG, so file managers can show it as a thumbnail. The scalable icon stays
  in the Flatpak and in `cmake --install`.

## Platforms and how they were verified

Same platforms as 0.1.0. Every CTest entry passes in the AppImage's Ubuntu
20.04 build container and under Wine for the Windows build. The AppImage
passes the AppImage project's `appdir-lint.sh`.

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
