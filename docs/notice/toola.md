# Lane TOOLA attribution notes (wave 3b: tool framework, selection, move and view tools)

No code was copied or translated from any Paint.NET release. The lane read
the MIT-licensed Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the behaviors listed here and implemented them in its
own C code. No Paint.NET cursors, icons, strings or other resources were
used; the cursor drawings and the spinner are this project's own.

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Tool.cs (OnKeyPress, KeyPress) | Arrow keys move the pointer and the tool sees the motion: the step is the zoom ratio rounded up (one image pixel when zoomed in, one screen pixel when zoomed out) times a speed that starts at 1 and grows by one every fourth repeat after 15 quick repeats; a pause of more than a quarter second or another key starts over. paint.c adds Ctrl for ten times the step (documented in 5.1). | app_tool_nudge_pointer in src/app/tool.c |
| src/tools/PanTool.cs, src/Tool.cs | The Pan tool pans while a button is held, and arrow keys reach it as pointer motion, so a held button plus arrows pans. | tool_pan.c (pan_key) |
| src/DocumentWorkspace.cs (UpdateSelectionInfoInStatusBar) | The status bar shows the selected area besides the offset and size: pixels as a whole number, inches and centimeters squared with two decimals (paint.c's wording and layout). | sel_status_rect in src/app/tools/sel_common.c |
| src/Resources/Cursors (file names only) | The idea that each selection tool has a cursor per combine mode (plus and minus variants). The cursors themselves were not used; paint.c draws a crosshair or the tool icon with the toolbar's own selection mode icon. | src/app/tools/sel_cursor.c |

paint.c's own work: the kinds of Finish and the live-object history model
(sel_live.c, after this project's lane B observations of Paint.NET 5.2),
the adoption of layer property History items by fingerprinting tile
identity, the toolbar overflow chevron and its group planning, the tool
chooser, the 4 x 4 supersampled selection rasterizer (a scanline table of
crossings, sel_aa.c), the background Magic Wand job and spinner, and the
closed hand drawing.
