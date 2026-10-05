# Lane M attribution notes (Edit, View, Image, Layers and Help menus, dialogs)

No code was copied or translated from any Paint.NET release. Paint.NET
4.x, 5.x and 6.x were never decompiled or disassembled (P-01, ADR-002);
their behavior was taken from the official documentation mirror
(docs/inventory, /ai/work/paintc-research/pdn-docs), the release notes and
the black-box observations of the running application in
docs/inventory/OBSERVED.md (ADR-016). The lane read the MIT-licensed
Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN, Copyright dotPDN
LLC, Rick Brewster, Tom Jackson and contributors, MIT, see NOTICE) only for
the behaviors listed here. No 3.36 resource text, icon or image was used;
every display string is paint.c's own (P-02, ADR-013).

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/ResizeDialog.cs (ResizeConstrainer) | The size model keeps the new size as unrounded doubles, so locking the aspect ratio derives the other side from the original aspect without rounding drift; turning the lock on recomputes the height from the width; by percentage sets both sides from the original size. | src/app/edit/m_size.c |
| src/HistoryFunctions/CropToSelectionFunction.cs | Behavior: Crop to Selection replaces the document and leaves nothing selected, recorded as one compound history item. | cmd_crop in src/app/mods/mod_image.c (crop and deselect fused into one step) |
| src/Actions/ImportFromFileAction.cs | Behavior: one history item for the whole import (deselect, canvas growth anchored top left, the new layers, then the bounds of the last imported layer selected) and the Move Selected Pixels tool afterwards; layers named from the file name and the source layer name. | src/app/edit/m_import.c |
| src/Actions/PasteAction.cs, PasteInToNewLayerAction.cs, PasteInToNewImageAction.cs | Behavior: the Expand Canvas question for images larger than the canvas with Expand / Keep / Cancel and a thumbnail of the pasted image, the expansion as its own history item anchored top left, Paste into New Layer as a new layer followed by the paste, Paste into New Image as a new image of the clipboard size whose history starts at the new image. | src/app/edit/m_paste.c |
| src/Effects/RotateZoomEffect.cs | The meaning of the pan values (offsets in half frame sizes, as the pan pad's -1..1 edges), mapped onto the engine's pc_rotzoom. | src/app/mods/mod_m_rotzoom.c |

Not taken: the 3.36 canvas growth with the secondary color on paste and
import (the 5.1 inventory specifies a transparent new area), the 3.36
centering of pastes that do not fit (5.1 pastes into the visible part of
the canvas, CB-PASTE-POS) and every 3.36 dialog layout.

Color science (src/app/edit/m_icc.c) uses only published definitions: the
sRGB primaries, white point and tone curve of IEC 61966-2-1, Adobe RGB
(1998) of the Adobe RGB (1998) Color Image Encoding specification, Display
P3 of SMPTE EG 432-1 with the sRGB curve, ROMM RGB (ProPhoto) of ISO 22028-2
(written with a pure 1.8 gamma), the Bradford chromatic adaptation matrix
and the ICC.1:2022 profile format. The built-in profiles are generated at
run time; no third-party profile file is included.
