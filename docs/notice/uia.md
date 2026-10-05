# Lane UIA attribution notes (wave 4: window, layout, dialogs, settings, view)

No code was copied or translated from any Paint.NET release. Paint.NET 5.1
behavior comes from its documentation (docs/inventory). The MIT-licensed
Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN, Copyright dotPDN
LLC, Rick Brewster, Tom Jackson and contributors, MIT, see NOTICE) was read
for behavior only:

| 3.36 file | What was looked at | Where it matters |
|---|---|---|
| src/AppWorkspace.cs (ResetFloatingForms, ResetFloatingForm) | The default corner of each utility window (Tools top left, History top right, Layers bottom right, Colors bottom left), parked there without any overlap handling (3.36's floaters are top-level owned forms, not windows inside the main window). | src/app/panels.c default_layout keeps the corners and adds its own overlap avoidance. |
| src/MainForm.cs (window bounds restore, PositionFloatingForms) | That saved main window bounds are restored as saved and that floaters found off every screen are reset. | src/app/app.c app_window_geometry goes further on its own terms (DIP sizes, fitting to the usable area). |

The following were designed in this lane without any Paint.NET code: the
DIP window geometry, the window minimum size, scrolled dialog bodies, the
option-unit overflow of the options bar and its wrapped popup, the folded
window toggles, the default panel layout solver, the whole-texel clipping
for the software renderer, the area-filtered zoom between mip levels
(linear light, alpha weighted, like the wave 3b mip levels), the SDL 3.4
pinch gesture handling and the smooth Ctrl+wheel zoom.

SDL 3.2.10 and 3.4.18 sources (zlib license) were read to find the cause of
the half-pixel shift of clipped scaled blits in the software renderer
(SDL_BlitSurfaceScaled rounds a clipped source rect) and the scale
conventions of SDL_EVENT_PINCH_UPDATE per backend. No SDL code was copied.
