# Lane L1b attribution notes

Lane L1b (core operations: transactions, compositor, display cache,
resampling, image geometry, layer operations, history budget) contains no
code copied, ported or transliterated from any Paint.NET release. Behavior
was taken from the Paint.NET 5.1 documentation (names, parameters, menu
semantics) and, where the documentation is silent, from the MIT-licensed
Paint.NET 3.36 source (OpenPDN mirror, "Paint.NET 3.36, Copyright (C) dotPDN
LLC, Rick Brewster, Tom Jackson, and contributors", MIT License; see NOTICE).
The following behaviors were derived from reading that source. All
implementations are original C written for this project.

| 3.36 file | What was derived | Where |
|---|---|---|
| Actions/CanvasSizeAction.cs (ResizeLayer, ResizeDocument) | Anchor placement: left/top 0, right/bottom new - old, center (new - old) / 2 truncated toward zero. The bottom (background) layer receives the fill color in the new area, every other layer is transparent there. | src/core/pc_geom.c: pc_geom_anchor_offset, pc_geom_canvas_size |
| HistoryFunctions/MergeLayerDownFunction.cs | The upper layer is rendered onto the lower layer's pixels with its own blend mode and opacity (its visibility flag is not consulted), the lower layer keeps its properties, the upper layer is deleted, one history step. | src/core/pc_layerops.c: pc_layerop_merge_down |
| HistoryFunctions/FlattenFunction.cs, Data/Document.cs (Flatten, Render) | Flatten composites the visible layers only (Render skips hidden layers) into a single layer. | src/core/pc_layerops.c: pc_layerop_flatten |
| HistoryFunctions/CropToSelectionFunction.cs | Crop to the selection's bounding rectangle; pixels outside a non-rectangular selection are cleared to transparent. | src/core/pc_geom.c: pc_geom_crop_to_selection |
| HistoryFunctions/FillSelectionFunction.cs, EraseSelectionFunction.cs | Fill replaces the selected pixels with the color (no blending over them); erase clears them to transparent. | src/core/pc_layerops.c: pc_layerop_fill, pc_layerop_clear |
| HistoryFunctions/AddNewBlankLayerFunction.cs | New layer name "Layer N" with N = layer count + 1. | src/core/pc_layerops.c: pc_layerop_add_new |
| Effects/RotateZoomEffect.cs, RotateZoomEffectConfigToken.cs | Model only: a perspective camera at half the frame diagonal, pan given in half frame sizes, transparent (soft) edges outside the source when not tiling. The matrix construction is this project's own. | src/core/pc_layerops.c: pc_rotzoom_xform |
| Core/ResamplingAlgorithm.cs, Actions/ResizeAction.cs | The "Super Sampling" mode (area average when shrinking, bicubic when enlarging) kept as an extra mode next to the eight 5.1 modes. | src/core/pc_resample.c |

The resampling kernels (tent, Mitchell-Netravali cubics with B=0 C=0.5
and B=1 C=0, Lanczos-3, box area average) are the standard published
definitions; the Paint.NET 5.1 documentation only names them. Blend math
(pc_composite_span) is the existing project oracle, already attributed in
NOTICE.
