# Attribution notice, lane W3B-FXCORE (effects and core, wave 3b)

Some behavior in the files below was derived from the MIT-licensed source code
of Paint.NET 3.36 (the last release under that license, mirrored at
https://github.com/rivy/OpenPDN). The C code is a new implementation, written
for paint.c; no Paint.NET 4.x, 5.x or 6.x code, binary, decompiled IL or
resource was used (P-01, P-02, ADR-002). No 3.36 resource assets, artwork,
menu or status text, or GPC code were used.

Where Paint.NET 5.x behavior was needed it was measured on the running
Paint.NET 5.2 beta under Wine (thumb positions read off the dialog
screenshots in /ai/work/paintc-research/wine/shots, docs/inventory/OBSERVED.md)
or taken from the public 5.0.4 release notes (the list of effects that render
with linear gamma); nothing else was copied.

The orchestrator should merge this notice into `NOTICE` (it already
reproduces the full MIT permission text for the 3.36 derivations).

## Derived files

| paint.c file | Paint.NET 3.36 source | What was derived |
|---|---|---|
| src/ui/ui_slider.c (UI_SLIDER_EXP) | Core/IndirectUI/PropertyControlUtil.cs (ToSliderValueExp, FromSliderValueExp) | The shape of the "exponential scale" slider: value = min + span * t^2 for ranges at or above zero; for ranges spanning zero, zero stays at its linear slider position and each side is quadratic away from zero. The same shape was measured on the 5.2 dialogs (tests/ui/test_ui_slider_exp.c). |
| src/fx/host/fxh_rules.c | Base/PropertySystem/LinkValuesBasedOnBooleanRule`2.cs, Base/PropertySystem/SoftMutuallyBoundMinMaxRule`2.cs | Semantics of linked values (while the boolean is on, every member takes the value of the member changed last, the first member until one changes) and of the soft minimum/maximum pair (raising the minimum above the maximum pushes the maximum up, lowering the maximum below the minimum pulls the minimum down). |
| src/fx/distort/fxm_frosted_glass.c | Effects/FrostedGlassEffect.cs | The minimum radius limited to half the image while the maximum is not (already noted by lane L5C), and the min/max rule above applied at render time. |
| src/fx/adjust/fxm_adj_posterize.c, src/fx/distort/fxm_morphology.c | Effects/PosterizeAdjustment.cs | Which values are linked (3.36 linked R, G, B; 5.x adds Alpha, and Morphology links Width and Height per the 5.1 documentation). |

## Own implementations of public formats (no third-party code)

| paint.c file | Source of the format | Note |
|---|---|---|
| src/core/pc_hist_lz4.c | The public LZ4 block format description (token, literals, 16-bit offset, length extensions, end-of-block rules) | Written from the format description; no LZ4 library code was used or vendored. |
| src/core/pc_resample_trc.c | The ICC.1 profile specification ('curv' and 'para' tone curve types, tag table) | Reads only the tone curve tags; parametric curve types 0..4 as specified. |
| src/core/pc_raster.c (pc_raster_fill_ss4) | TOOLS T-SEL-QUALITY (4 x 4 supersampled selection coverage) | Own point-sampling rasterizer on the existing edge list. |
