# paint.c Help

paint.c is a raster image editor for Windows, macOS and Linux. It follows the
workflow of Paint.NET 5.1: layers, a History you can step through, live
adjustments and effects with preview, and tools that stay editable until you
finish them. This guide ships with the program (version {{version}}) and
opens without a network connection.

## Contents

- [Getting started](getting-started.md): the window, opening and saving,
  zooming, undo and History, selections and colors.
- [Tools](tools.md): every tool, its keyboard letter and its options.
- [Layers](layers.md): the Layers window, layer properties and blend modes.
- [Adjustments and effects](adjustments-effects.md): how the dialogs work and
  the complete list of what is built in.
- [File formats](file-formats.md): what paint.c opens and saves, save options,
  color profiles and the clipboard.
- [Keyboard shortcuts](keyboard.md): every shortcut of this copy of paint.c,
  generated from its key map.
- [Plugins](plugins.md): installing effect plugins and writing your own.
- [Tutorials](tutorials.md): step by step examples.
- [Settings and troubleshooting](troubleshooting.md): where paint.c keeps its
  files, command line options and what to do when something goes wrong.

Use the search box at the top of every page to find a word in the whole
guide (Help > Search, or {{ctrl}}+E in paint.c, opens it directly).

## Quick start

1. Open an image with File > Open ({{ctrl}}+O), or drag files onto the
   window. File > New ({{ctrl}}+N) makes an empty image.
2. Pick a tool in the Tools window or press its letter: B for the
   Paintbrush, S for the selection tools, T for Text.
3. Choose colors in the Colors window. The left mouse button paints with the
   primary color, the right button with the secondary color.
4. Every change becomes an item in the History window. Press {{ctrl}}+Z to
   undo and {{ctrl}}+Y to redo, or click an older item to go back to it.
5. Save with File > Save ({{ctrl}}+S). Images with several layers keep them
   in the .pdn format; most other formats store one flattened layer.

## About this guide

The pages are generated from the guide's sources inside paint.c each time
you open them, so the shortcut tables and the lists of effects and file
formats always match the program you are running. The guide describes
paint.c in its own words; it is not the Paint.NET documentation.
