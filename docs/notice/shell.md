# Lane SHELL attribution notes (wave 3b: view, canvas, windows, dialogs)

No code was copied or translated from any Paint.NET release. The lane read
the MIT-licensed Paint.NET 3.36.7 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the behaviors and parameters listed here; Paint.NET
5.1 behavior comes from its documentation and the observations in
docs/inventory/OBSERVED.md.

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Core/ScaleFactor.cs | The zoom stepping rule (next preset above z + 0.005, below z - 0.005), already used by wave 2; this lane only changed the preset list to the reciprocal ladder that the rule turns into the observed 5.x steps. | src/gfx/gfx_view.c k_presets |
| src/MainForm.cs (FloaterOpacityTimer_Tick) | Behavior of translucent utility windows: opaque under the pointer (unless the canvas has the mouse captured), while they hold the mouse, or when they do not cover the visible image; otherwise fading to 0.75 opacity; fade steps +0.125 and -0.0625 every 25 ms. | src/app/shell_panels.c |
| src/SelectionRenderer.cs | The marching ants speed: the dash offset advances one pixel every 60 ms (kept, but now advanced continuously at the display refresh rate). | src/app/canvas.c ants_phase |
| src/Tool.cs (ScrollIfNecessary, PanTool autoScroll = false) | Behavior: dragging past the view edge scrolls the view toward the pointer, except with the Pan tool. The time-based speed, the edge zone and the overscroll limit are this project's own (5.x behavior per the ViewTools documentation and release notes). | src/app/canvas.c auto_scroll |

Everything else (pinch zoom from SDL finger events, Space + arrow
panning, the first-presentation hold, the sharp-bilinear magnification,
the linear-light mip and Rotate / Zoom sampling, the display color
transform, the Settings pages, the per-tool defaults store, crash logs and
the graphics adapter query) is original work of this project.
