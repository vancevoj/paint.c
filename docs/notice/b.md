# Lane B (wave 2b) attribution notes (painting and fill tools)

No code was copied or translated from any Paint.NET release. Paint.NET
4.x, 5.x and 6.x were never decompiled or disassembled (P-01, ADR-002).
Behavior comes from the Paint.NET 5.1 documentation mirror (text in
/ai/work/paintc-research/pdn-docs; the toolbar images of the online
documentation were looked at for the order of the options), the earlier
lanes' black-box observations of Paint.NET 5.2 under Wine
(docs/inventory/OBSERVED.md, docs/core/brush.md, docs/core/fills.md),
and the MIT-licensed Paint.NET 3.36.7 source (mirror:
github.com/rivy/OpenPDN, Copyright dotPDN LLC, Rick Brewster, Tom
Jackson and contributors, MIT, see NOTICE), which was read only for the
algorithms and behaviors listed here. The drawing engines themselves are
the core lanes' work (see e1.md and e2.md).

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/Core/ColorBgra.cs (Blend over an array) | The average of several colors for the Color Picker sample sizes: alpha = sum(a) / n, each color channel = sum(c a) / sum(a), integer division, all-transparent input gives transparent black. | average() in src/app/tools/tool_color_picker.c |
| src/tools/ColorPickerTool.cs | Behavior: a color is picked on the press and on every move while the button is held, clicks outside the image pick nothing, the tool switch of the after-click option happens on the release, "previous tool" means the tool active before the Color Picker. | tool_color_picker.c |
| src/tools/GradientTool.cs | Behavior: pressing a nub with the right button swaps the color roles and keeps dragging that nub, pressing outside the nubs commits the gradient and starts a new one, Shift constrains the dragged nub around the other one and pressing Shift during a drag re-renders at once. | tool_gradient.c |

The measured line rule of the Pencil (3.36 Utility.GetLinePoints, used by
pc_brush, see e1.md) is also the oracle of tests/app/test_app_g2.c after
the Pencil moved onto pc_brush.

Everything else is original work of this project: the options bar
widgets (brush size combo, bar sliders, split buttons, the fill style
dropdown with pattern previews), the glyphs drawn for the option buttons
(smoothing, pressure, flood mode, tolerance alpha mode, sampling, sample
size, gradient types, color mode, repeat modes, Recolor sampling modes),
the nubs and move handles on the canvas, the coalesced live re-rendering
of the Paint Bucket and the Gradient, and all user-visible wording
(help texts, tooltips, messages). Short option and menu names follow
Paint.NET's functional names (ADR-013).
