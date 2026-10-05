# Packaging

Lane I. How the packages are built and what they contain: docs/PACKAGING.md.

| Path | Purpose |
|---|---|
| PcPackaging.cmake | icon and version resources, macOS bundle resources, install rules, CPack (included by src/app/CMakeLists.txt) |
| icons/make_icons.py | generates assets/icons (SVG, PNG set, .ico, .icns) and src/app/io/icon_data.c |
| licenses/ | third-party license texts; every package ships them with LICENSE and NOTICE |
| linux/org.paintc.paintc.desktop | desktop entry (app id from ADR-001), every MIME type paint.c opens |
| linux/org.paintc.paintc.xml | shared-mime-info type for .pdn (image/x-paintnet) |
| linux/org.paintc.paintc.metainfo.xml | AppStream metadata |
| linux/build-appimage.sh | AppImage from a build tree (pinned linuxdeploy) |
| flatpak/org.paintc.paintc.yml | Flatpak manifest (offline build, dependency archives listed) |
| windows/paintc.manifest | long paths, UTF-8 code page, per-monitor DPI |
| windows/paintc.nsi | NSIS installer with file associations |
| windows/README-portable.txt | README.txt of the portable zip |
| macos/make-dmg.sh | ad-hoc signed paint.c.app in a .dmg |

The bundle metadata of paint.c.app is src/app/platform/Info.plist.in; the
Windows icon and version resources are src/app/platform/paintc_version.rc.in.
