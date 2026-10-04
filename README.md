# PortableCanvas reference core v0.1

Portable C17 core of a Paint.NET-class raster editor: tiles, documents, transactions,
a branching undo tree, Paint.NET-compatible blending and flood fill. Headless and fully
tested. Platform, GPU, UI and codec layers are not written yet.

Build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/pc_tests
```

Plans and status live in docs/.
