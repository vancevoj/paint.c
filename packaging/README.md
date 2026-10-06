# Packaging

Lane I. How the packages are built and what they contain: docs/PACKAGING.md.

| Path | Purpose |
|---|---|
| PcPackaging.cmake | icon and version resources, macOS bundle resources, install rules, CPack (included by src/app/CMakeLists.txt) |
| icons/make_icons.py | generates assets/icons (SVG, PNG set, .ico, .icns) and src/app/io/icon_data.c |
| licenses/ | third-party license texts; every package ships them with LICENSE and NOTICE |
| linux/io.github.vancevoj.paintc.desktop | desktop entry (app id from ADR-025), every MIME type paint.c opens |
| linux/io.github.vancevoj.paintc.xml | shared-mime-info type for .pdn (image/x-paintnet) |
| linux/io.github.vancevoj.paintc.metainfo.xml | AppStream metadata (Flathub listing) |
| screenshots/ | AppStream screenshots and make-screenshots.sh, which renders them headless |
| linux/build-appimage.sh | AppImage from a build tree (pinned linuxdeploy) |
| linux/build-appimage-docker.sh | release AppImage built in Ubuntu 20.04 (glibc 2.31 floor), with checks |
| flatpak/io.github.vancevoj.paintc.yml | Flatpak manifest of this checkout (offline build, dependency archives listed); Flathub has the tagged copy |
| windows/paintc.manifest | long paths, UTF-8 code page, per-monitor DPI |
| windows/paintc.nsi | NSIS installer with file associations |
| windows/README-portable.txt | README.txt of the portable zip |
| windows/build-mingw-release.sh | portable zip and installer cross built with mingw-w64 |
| macos/make-dmg.sh | ad-hoc signed paint.c.app in a .dmg |
| RELEASE_NOTES_<version>.md | release notes; `@CHECKSUMS@` is filled in by release-dist.sh |
| release-dist.sh | source archive, SHA256SUMS.txt and the rendered notes of a local release |
| plugins/build-plugins.sh | the optional plugins of plugins/ as release zips (Linux in the AppImage's Ubuntu 20.04 container, Windows with mingw-w64), with library checks and SHA256SUMS (docs/PLUGINS.md) |
| plugins/release.conf | plugin set version, repository of the download links, glibc floor |
| plugins/plugin_meta.py | reads the plugins' README cards: zip names, the plugin tables of README.md and docs/PLUGINS.md (`write`, `check`), reproducible zips |
| plugins/screenshot.sh, plugins/shot_tool.py | a plugin's screenshot.png: its dialog over a test image, rendered headless |

The bundle metadata of paint.c.app is src/app/platform/Info.plist.in; the
Windows icon and version resources are src/app/platform/paintc_version.rc.in.
