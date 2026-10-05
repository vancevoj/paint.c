# pc_ui: the paint.c UI toolkit (lane L3)

An immediate-mode toolkit over SDL_Renderer (ADR-003, ADR-004). One static
library, `pc_ui`, that links `SDL3::SDL3` and the header-only parts of
`pc_core` (status codes, checked size math). No pal dependency. Public
headers live in `include/ui/`:

| Header | Contents |
|---|---|
| `ui.h` | context, frames, input, ids, interaction, layout, scroll regions, every widget, menus, popups, dialogs, panels |
| `ui_draw.h` | batched drawing: rects, rounded rects, shadows, gradients, checkerboards, AA lines/polygons/circles/arcs, images, text runs, icons, custom render callbacks, clip stack |
| `ui_font.h` | font loading (TTF/OTF/TTC, glyf and CFF), metrics, measurement, fallback chains, and the A8 text rasterizer for the Text tool |
| `ui_icons.h` | the original vector icon set (tools and commands) |
| `ui_theme.h` | light and dark palettes around one accent color, DIP size tokens |
| `ui_base.h` | rects, colors, HSV, ids, UTF-8 helpers (pure functions) |

## Frame loop

```c
SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");   /* before the window */
ui_ctx *ui = ui_create(renderer, window);                   /* both borrowed */
for (;;) {
    int32_t wait = ui_wait_timeout(ui, SDL_GetTicks());     /* -1: only on input */
    if (wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait)) {
        do {
            bool ui_took = ui_event(ui, &e);   /* app may still look at every event */
            /* canvas input when !ui_took ... */
        } while (SDL_PollEvent(&e));
    }
    if (!ui_needs_frame(ui, SDL_GetTicks()) && !app_dirty) continue;
    ui_frame_info fi;
    ui_frame_info_auto(ui, &fi);        /* output size, display scale x pixel density */
    ui_begin_frame(ui, &fi);
    build_ui(ui);                       /* widgets, menus, panels, dialogs */
    ui_end_frame(ui);
    run_shortcuts(ui_key_presses(ui, &n));   /* presses with used == false */
    SDL_RenderClear(renderer);
    ui_render(ui);                      /* replays the layers in z order */
    SDL_RenderPresent(renderer);
}
ui_destroy(ui);
```

Rendering is on demand: input, hover changes, the caret blink, tooltips,
popup measuring and animations request frames; an idle UI sleeps.

## Units and DPI

Widgets lay out in device pixels. Design sizes (theme metrics, layout
cells, dialog widths, panel rects) are DIPs (1/96 inch at 100 %), converted
with the frame scale (`SDL_GetWindowDisplayScale` times `ui_set_zoom`) and
rounded to whole pixels, so edges stay crisp at 125 %, 150 % and 175 %.
Mouse coordinates are converted with the window's pixel density.

## Canvas integration

* Draw the document with `ui_draw_callback` at the right point of the base
  layer: the callback receives the renderer and the current clip rectangle
  and may issue any SDL render calls (tile textures, overlays). Popups,
  panels and dialogs are drawn on top of it.
* `ui_event` returns false for pointer events that no UI layer wants: those
  belong to the canvas. `ui_wants_mouse` and `ui_wants_keyboard` give the
  same answer between frames.
* `ui_set_cursor(ui, UI_CURSOR_APP)` while the pointer is over the canvas
  leaves the cursor to the app (tool cursors).
* `ui_panels_area` sets the region floating panels live in (the workspace
  below the toolbar). `ui_panel_state` is plain data in DIPs with edge
  anchors; persist it to remember window layouts across runs and scales.

## Text tool

`ui_font_load_file` loads system fonts (collections through the face
index, `ui_font_face_count` and `ui_font_describe` list them). A face is
validated structurally when loaded; glyphs that fail validation draw as
empty. `ui_text_raster` turns UTF-8 text (multi-line, aligned, synthetic
bold, italic, underline, strikethrough, antialiased or not) into an owned
A8 coverage buffer (`ui_a8_free`); `ui_text_caret` and `ui_text_hit_point`
place the caret. These functions use no caches and may run on worker
threads while the faces stay alive. Complex-script shaping is out of scope
(ADR-004): text is nominal glyphs plus pair kerning.

## Widgets in short

Labels, wrapped text, buttons (primary, flat, icon, toggle, tool, split),
check boxes, radios and radio groups, switches, sliders (int, double, log),
numeric fields (typed entry, spin buttons with auto repeat and dragging,
wheel, keys), property sliders with reset (effect dialogs), text fields
(selection, word moves, clipboard, IME composition, filters), combo boxes,
virtualized list views with custom rows and drag reordering (Layers,
History), tab strips, document tabs (thumbnail, modified marker, close
button, reordering, overflow list), color swatches, the color wheel (hue
and saturation disc, or hue ring with a saturation/value square), channel
sliders, hex entry, the composed color picker, the primary/secondary pair,
palette grids, angle dial, point picker, progress bars, separators,
collapsing headers, group boxes, a menu bar with submenus, check and radio
items, context menus, modal dialogs and message boxes, floating panels.

`src/ui/gallery` shows all of them: `paintc_ui_gallery` opens a window
(F2 switches the theme, F3 opens a dialog), `--screenshot out.bmp
[--dark] [--scale s] [--scene 0..3]` renders headless, and `--frames N
[--capture out.bmp]` runs a window for N frames and reads the last one back
from the GPU renderer. `tests/ui/test_ui_gallery` writes every scene in both
themes to `<build>/ui_gallery_*.bmp`.

## Menu keyboard (lane KEYS)

`ui_menu_mnemonics(ctx, true)` makes '&' in the following menu labels an
access key ("&&" is a literal ampersand); it is off at the start of every
frame, so file and plugin names are never parsed. Alt + a title's key
opens that menu, letters choose items in an open menu (unmarked items by
their first character), a lone Alt press focuses the menu bar (not on
macOS, not AltGr), and the keys are underlined while Alt is held or the
menu is used from the keyboard. `ui_menu_keyboard` tells the app that a
menu owns the keys; `ui_open_request` opens a popup or combo by id for the
keyboard (Alt+H, Alt+T in the app). Menus taller than the window scroll
(wheel, arrow bands, keyboard). The wheel over a closed combo steps through
its items (except inside scrolled areas). `ui_key_press.sym` is the
character a press types with the layout's Shift / AltGr state.

Wave 4 (lane UIA): a modal dialog taller than the window is as tall as the
window and its body scrolls, buttons included (`ui_dialog_scrolled`); the
scroll region adds no id level, so widget ids and focus survive a resize.
Inside scrolled areas a number box takes the wheel only while it has the
keyboard focus (like combos). Dropdown lists reserve room for their scroll
bar and up to 16 items show without one. `ui_id_scope` /
`ui_push_id_scope` let code inside another widget's id scope declare ids
in an outer scope (the options bar opens its overflow popup this way).

## Implementation notes

* Draw calls append triangles to per-layer draw lists (base, panels,
  modals, popups, tooltip); `ui_render` merges runs that share a texture and
  clip rectangle into one `SDL_RenderGeometry` call. Glyphs, rounded
  corners, discs, rings, shadows and icons are coverage sprites rasterized
  on the CPU (exact area antialiasing, `ui_raster.c`) into A8 atlas pages
  that are mirrored into textures; the CPU copy survives device resets.
* stb_truetype only extracts outlines; `ui_font_check.c` validates every
  table it reads first, and kerning (GPOS pair adjustment, legacy kern) is
  read by bounds-checked code of our own.
* No recursion over paths or glyph data (P-07); sizes derived from fonts
  or text are checked before allocating (P-08).
* The software renderer (tests, fallback) snaps geometry vertices to half
  pixels and truncates sub-texel source rectangles, so the toolkit draws
  everything that must be exact (checkerboards, sprites) with whole texels.
