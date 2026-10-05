# Lane L1a attribution notes (selection, rasterizer, paths, contours)

No code was copied or translated from any Paint.NET release. The lane read
the MIT-licensed Paint.NET 3.36.7 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the behaviors and parameters listed here.

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Core/Selection.cs | Behavior: an empty selection clips to the whole canvas. | pc_sel.h model (sel_active == false, pc_sel_coverage returns 255, pc_sel_extent returns the canvas) |
| src/HistoryFunctions/InvertSelectionFunction.cs | Behavior: Invert Selection does nothing when the selection is empty, and inverts against the canvas rectangle. | pc_sel_invert |
| src/PenInfo.cs, src/LineCap2.cs | The cap set (flat, arrow, filled arrow, rounded) and the arrowhead size: 5 x cap scale times the pen width, long and wide, with the cap scale clamped to [1, 5]. | pc_cap, pc_arrowhead, pc_stroke.arrow_scale |
| src/PenInfo.cs | Dash styles come from the pen's System.Drawing DashStyle (solid, dash, dot, dash-dot, dash-dot-dot). The pattern lengths are the documented GDI+ values in pen widths (3 1, 1 1, 3 1 1 1, 3 1 1 1 1 1). | pc_dash_preset |
| src/tools/LineTool.cs | Behavior: the Spline curve type passes through every control point (GraphicsPath.AddCurve, a cardinal spline). The tension factor follows the documented GDI+ AddCurve convention (default 0.5). | pc_path_add_spline |

Everything else (the exact-area scanline rasterizer, the stroker, the
marching-squares outline tracer, the tile-based selection combine engine,
the bilinear selection transform with edge re-sharpening, and the
polygon-list text parser) is original work of this project. The GPC
polygon clipper of 3.36 was not used (X-03).
