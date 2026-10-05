# Layers

An image is a stack of layers. The bottom layer is usually opaque; layers
above it can be partly transparent, so the lower layers show through.
Painting, adjustments and effects change the **active layer** only, the one
highlighted in the Layers window (F7).

## The Layers window

- Each row shows a thumbnail, the name and a check box that shows or hides
  the layer ({{ctrl}}+Comma toggles the active layer).
- Click a row to make that layer active. {{alt}}+PgUp and {{alt}}+PgDn go to the
  layer above or below, {{ctrl}}+{{alt}}+PgUp and {{ctrl}}+{{alt}}+PgDn to the top or
  bottom layer.
- The buttons at the bottom add, delete, duplicate and merge layers, move the
  active layer up or down, and open its properties.
- Double-click a layer to open **Layer Properties**.

## Layer Properties

Layer Properties (F4) sets the **name**, whether the layer is **visible**,
its **blend mode** and its **opacity** (0 to 255). Changes show on the
canvas while the dialog is open; Cancel restores the layer.

## Blend modes

The blend mode decides how a layer mixes with the layers below it:

{{blend_modes}}

Normal simply draws the layer over the ones below. Multiply and Darken make
the result darker, Screen, Additive and Lighten make it lighter, Overlay
raises contrast, and Difference, Negation and Xor are useful for finding
differences between two layers. Tools offer the same modes, plus Overwrite.

## The Layers menu

| Command | What it does |
|---|---|
| Add New Layer ({{ctrl}}+Shift+N) | A transparent layer above the active one |
| Delete Layer ({{ctrl}}+Shift+Delete) | Removes the active layer |
| Duplicate Layer ({{ctrl}}+Shift+D) | A copy above the active layer |
| Merge Layer Down ({{ctrl}}+M) | Combines the active layer with the one below it |
| Import From File | Adds an image file as a new layer |
| Flip Horizontal, Flip Vertical, Rotate 180 degrees | Change the active layer only |
| Rotate / Zoom ({{ctrl}}+Shift+Z) | Turns, scales and moves the active layer freely |
| Move Layer Up, Down, to Top, to Bottom | Change the order of the layers |

**Image > Flatten** ({{ctrl}}+Shift+F) merges all visible layers into one.
Saving to a format that keeps a single layer asks to flatten first; the .pdn
format keeps every layer with its name, visibility, opacity and blend mode
(see [File formats](file-formats.md)).

## Tips

- Paint new details on a layer of their own, so you can change or remove
  them later without touching the picture below.
- Lower the opacity of a layer to fade it, or hide it to compare before and
  after.
- Edit > Copy Merged ({{ctrl}}+Shift+C) copies what you see from all layers,
  while Copy uses only the active layer.
