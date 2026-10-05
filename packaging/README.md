# Packaging (stub)

Wave 4 work. The files here are starting points for the app lane:

| Path | Purpose |
|---|---|
| linux/org.paintc.paintc.desktop | desktop entry (app id from ADR-001) |
| macos/Info.plist.in | bundle metadata for `paint.c.app`, configured by CMake |
| windows/paintc.manifest | long paths, UTF-8 code page, per-monitor DPI |

Planned: Windows zip and installer, macOS .app bundle (ad-hoc signed, see
ADR-008 for the plugin entitlement), Linux AppImage and a Flatpak manifest.
