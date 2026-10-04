# paint.c

A native C17 raster image editor with the Paint.NET 5.1 workflow, for Linux
(Wayland and X11), Windows and macOS. Layers with the 14 classic blend modes,
selections and the magic wand, brushes with pen pressure, adjustments and
effects with live preview, branching undo, plugins, and PNG, JPEG, BMP, GIF,
TGA, TIFF, WebP, DDS, OpenRaster and .pdn files.

paint.c is an independent clean-room project. It is not affiliated with or
endorsed by dotPDN LLC; Paint.NET is their trademark. See NOTICE for the MIT
attribution of the Paint.NET 3.36 blend math.

Build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
./build/pc_tests            # full reference suite
```

Plans and status live in docs/.
