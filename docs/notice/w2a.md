# Lane W2A (editor shell, wave 2a) attribution notes

The editor shell (`src/app`, `src/gfx`, `include/app`, `tests/app`) contains
no code copied, ported or transliterated from any Paint.NET release. Window
layout, menu structure, command names, shortcuts, zoom presets, tool order and
hotkeys follow the Paint.NET 5.1 documentation as recorded in
docs/inventory (MENUS, TOOLS, WINDOWS, SHORTCUTS, FILES, VIEW); every longer
text (tooltips, status hints, dialog wording) is paint.c's own (ADR-013).
Where the documentation is silent, a few behaviors were derived from reading
the MIT-licensed Paint.NET 3.36 source (OpenPDN mirror, "Paint.NET 3.36,
Copyright (C) dotPDN LLC, Rick Brewster, Tom Jackson, and contributors", MIT
License; see NOTICE). The implementations are original C.

| 3.36 file | What was derived | Where |
|---|---|---|
| src/Core/ScaleFactor.cs (GetNextLarger, GetNextSmaller) | Stepping rule between zoom presets: the next larger preset is the first one not below the zoom plus 0.005, the next smaller one the preset before the first one not below the zoom minus 0.005. The preset values themselves are the 5.1 list from VIEW.md. | src/gfx/gfx_view.c: gfx_zoom_next_in, gfx_zoom_next_out |
| src/Tool.cs (OnKeyPress, toolSwitchReset) | Tool hotkey cycling: Shift searches the tool list backwards; within the cycle window and while the current tool has the pressed letter the search continues after the current tool, otherwise it starts over; a letter pressed while a mouse button is down is consumed without switching. The window is the documented one second instead of 3.36's two. | src/app/tool.c: app_tool_letter |
| src/PaletteCollection.cs (default palette) | The 96 default palette colors, as listed in docs/inventory/WINDOWS.md 7.2. Colors are data; the list is reproduced from the inventory. | src/app/panels/mod_panels.c: k_palette |

No Paint.NET icons, images, resource strings or other assets are used; the
icons come from the project's own set (src/ui/ui_icons_data.c).
