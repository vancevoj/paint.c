# Notices for lane F (effects and adjustments integration in the editor)

## Paint.NET 3.36 (MIT)

The following files re-implement in C the behavior of parts of the
MIT-licensed Paint.NET 3.36 source (OpenPDN mirror
`https://github.com/rivy/OpenPDN`, "unmodified source of Paint.NET 3.36.7").
Only MIT-licensed code was read; no artwork, resource assets (.resources,
.resx, .png), menu or status text, dialog text or GPC code was used. Menu
item, dialog and parameter names are short functional names (ADR-013); all
longer texts (tips, messages, Plugin Errors explanations) are paint.c's own.

| Our file | 3.36 source | What was derived |
|---|---|---|
| src/app/fx/afx_curves.c (afx_curve_press, afx_curve_move, afx_curve_release, afx_curve_unit_x/y) | Core/CurveControl.cs (OnMouseDown, OnMouseMove, OnMouseUp) | the pointer rules of the curve editor: a press near a point lifts it and the drag re-inserts it under the pointer, a point the drag passes over is saved and restored when the drag moves on, hover picks the nearest point within a squared distance of 30 curve units, the pixel to curve unit rounding (0.5 + x * 255 / (w - 1)); drawing order of masked and unmasked curves, point radii |
| src/app/fx/afx_levels.c (gray point arrow, swatches, masked averages) | Effects/LevelsEffectConfigDialog.cs (gradientOutput_ValueChanged, InitDialogFromToken, MaskAvg, MaskGamma, MaskChanged) | gamma from the gray point position (log of the relative position over log 0.5, clamped to 0.1 .. 10), integer masked average of the shown values, the gray swatch as the levels applied to the input mean color, controls disabled when no channel is checked |
| src/app/fx/afx_session.c (Repeat, remembered parameters) | Menus/EffectMenuBase.cs (RunEffect, RepeatEffectMenuItem_Click) | parameters are remembered per effect only when the dialog ends with OK; Repeat applies to Effects menu items only and reuses the last token without a dialog |

Deliberate differences from 3.36 (paint.c's own design): end points of a
curve are locked to vertical moves while dragged (MENUS.md, 5.x docs);
the rendering pipeline (snapshot, pool jobs, viewport-first ROIs,
time-sliced blending into the transaction, cancel and restart) is original
work over the lane L5a runtime (fx_run.h) and the lane L1b transaction
(pc_txn_blend_rect_masked); the plugin loader is original work over pal
(no Paint.NET plugin ABI, no .NET, X-04).

The Paint.NET 3.36 copyright and MIT permission notice are reproduced in
the repository's NOTICE file and apply to the derived parts listed above.

## Other sources

None. The sample plugin (tests/plugins/sample_plugin.c) and the plugin
fixtures are original.
