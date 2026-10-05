# Lane P attribution notes (utility windows, image list, status bar, thumbnails)

No code was copied or translated from any Paint.NET release. Paint.NET
4.x, 5.x and 6.x were never decompiled or disassembled (P-01, ADR-002);
their behavior came from the official 5.1 documentation mirror, the
inventory in docs/inventory (WINDOWS, SHORTCUTS, OBSERVED) and black-box
observation of the running Paint.NET 5.2 beta under Wine (ADR-016, for
example the status bar order and the zoom box number format). The lane
read the MIT-licensed Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the algorithms and behaviors listed here.

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Core/HsvColor.cs, src/Core/RgbColor.cs | The integer HSV model of the Colors window: hue 0..360, saturation and value 0..100 as truncated integers, the six-sector HSV to RGB conversion with byte truncation (`(int)(x * 255)`), and gray or black colors reporting hue 0. | pnl_rgb_to_hsv, pnl_hsv_to_rgb in src/app/panels/pnl_colors.c |
| src/ColorWheel.cs (GrabColor) | Wheel geometry: the angle of the pointer around the wheel center with the screen y axis pointing down (so hue grows clockwise from red at 3 o'clock), hue and saturation truncated to integers, saturation clamped at the rim, and value 100 for colors picked on the wheel. | pnl_wheel_pick, wheel_apply in pnl_colors.c |
| src/PaletteCollection.cs, src/Core/ColorBgra.cs (ParseHexString) | Palette files: UTF-8 text with the .txt extension, ';' starts a comment, lines are trimmed and blank or unparsable lines skipped, a color is a hexadecimal number read as AARRGGBB (so 6 digits mean alpha 00; an optional 0x prefix as .NET's base-16 parser accepts it), 96 colors with missing ones white and extra ones ignored, the palette name is the file name, the folder is only created when needed (saving, Open Palettes Folder), and the default 96-color palette values (also listed in WINDOWS.md 7.2). The comment header written by Save is paint.c's own text. | src/app/panels/pnl_palette.c |
| src/ColorsForm.cs (layout only) | The arrangement of the expanded window into RGB, Hex, HSV and alpha sections next to the wheel. No code. | pnl_colors_body |

Not taken: 3.36's history, layers and image list controls (the windows
were built on paint.c's own toolkit from the 5.1 documentation), its
SavePaletteDialog (paint.c has its own small dialog with a replace
question), any resource text, icon or image (P-02). The history toggle
(clicking the current entry steps back and forth) follows the 5.1
History Window documentation.

The thumbnail filter (linear-light, premultiplied area average with a
per-tile cache) and the zoom box number format are paint.c designs
from the documented and observed behavior (R 5.0.4 "correct alpha and
gamma", OBSERVED section 8).
