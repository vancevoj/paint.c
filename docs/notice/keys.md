# Lane KEYS attribution notes (keyboard, menus, clipboard; wave 3b)

No code was copied or translated from any Paint.NET release. The lane read
the MIT-licensed Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the behaviors listed here, and implemented them in its
own C code. No Paint.NET menu text, resources, cursors or icons were used:
the access key letters of paint.c's menus are paint.c's own choice, except
the ones the Paint.NET 5.1 documentation names (Alt + F, E, V, I, L, A, C
for the menus, Alt+F then R, Q and X, Alt+H, Alt+T).

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Data/DocumentView.cs (Panel_KeyDown) | Home scrolls to the left edge and, when the view is already there, to the top; End scrolls to the right edge and, when the right edge is already shown, to the bottom (the "Home twice" and "End twice" of the 5.1 shortcut table). | edge_key in src/app/cmd.c |
| src/Tool.cs (OnKeyPress) | Arrow keys that no tool uses move the mouse pointer by ceil(zoom) screen pixels and replay the move as a pointer motion with the last button, so a held tool keeps working (a held Pan tool pans). paint.c adds the documented Ctrl x10 and leaves out 3.36's acceleration after fast repeats. | nudge_pointer in src/app/cmd.c |

paint.c's own work: the toolkit's menu keyboard (access key parsing,
underlines, the lone Alt menu bar focus, type-ahead in dropdowns, scrolled
tall menus, open requests), the typed character of key presses for the
[ ] , . / shortcuts on non-US layouts, Space + arrow panning by a fixed
screen distance, the floating paste wiring, the color profile conversion
of pastes, the command line diagnostics and setting overrides, the macOS
native menu key equivalent clean-up and the diagnostic cleanup command.
