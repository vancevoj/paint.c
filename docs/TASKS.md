# TASKS

Mirrors Section 4 of the handoff PDF. Every task is TODO until docs/STATUS.md says otherwise.

## Lanes
| Lane | Scope | Starts | Owns |
|---|---|---|---|
| L0 Foundation | Build, CI, repo layout, PAL, SDL3 app skeleton, menus | T-L0-01 first, alone. Then serial | CMakeLists.txt (orchestrator), cmake/, .github/, src/pal, include/pal, src/app/main.c |
| L1 Core | Allocator, uniform tiles, selection, rasterizer, compositor, SIMD, mips, history budget | Wave 1 | src/core, include/pc, tests/core |
| L2 Canvas and GFX | SDL_GPU renderer, atlas, canvas view, fallback, device loss | After G0 | src/gfx, shaders/ |
| L3 UI shell | Layout, widgets, panels, menus, docking, dialogs | After G0 | src/ui, src/app (shell code) |
| L4 Tools | Tool framework, brush engine, selection tools, wand, move, others | After G1 and G2 | src/app/tools |
| L5 Effects | fx ABI host, scheduler, adjustments, effects, plugin loader, GPU effects | After G1 | include/fx, fx/, src/core/fx_host* |
| L6 Codecs | Registry, PNG, JPEG, BMP, TGA, DDS, WebP, TIFF, ORA, .pdn, ICC, fuzzing | Wave 1 for core-only parts | src/codec, fuzz/ |
| L7 Parity | Feature inventory, golden corpus, comparison tool | Wave 1 (owner-assisted) | tests/golden, tools/goldencmp, docs/inventory |
| L8 Release | Packaging, signing, autosave, settings | After G3 | packaging/, src/app/settings* |

## Gates
| Gate | Condition |
|---|---|
| G0 | L0 done. A window with a GPU clear color runs on all three OSes in CI. pal.h v1 is frozen. |
| G1 | Multithreaded compositor, selection masks and byte-budget history merged. All suites green, TSan clean. |
| G2 | MVP canvas. Open a PNG, paint with a brush, undo, redo and save a PNG on all three OSes. |
| G3 | v1 feature complete per the Phase 7 criteria of Section 4.2 and the golden corpus passing. |

## Waves
| Wave | Contents |
|---|---|
| Wave 0 | T-L0-01 alone. It moves files, so nothing may run in parallel with it. |
| Wave 1 | Lane L0 serial (T-L0-02, T-L0-03, T-L0-04). In parallel L1 (T-L1-01, T-L1-03, T-L1-09), L6 (T-L6-01) and L7 (T-L7-03, plus T-L7-01 with the owner). |
| Wave 2 | After G0, L2 and L3 in parallel, and the rest of L1 (T-L1-02, T-L1-04 to T-L1-10). |
| Wave 3 | After G1 and G2, L4 and L5 in parallel, and L6 codecs that need vendored libraries. |
| Wave 4 | After G3, L8, the remaining L6 work and polish. |

## Rules for parallel work
- One agent per task. An agent edits only the directories its lane owns plus its own tests.
- Shared files (root CMakeLists.txt, public headers in include/, docs/STATUS.md, docs/TASKS.md) are edited only by the orchestrating agent, after merging lane results.
- Each workflow phase ends with the full suite and the sanitizer suite. Threaded code also runs under TSan.
- Merge lane results one at a time and re-run every suite after each merge.
- Never run two agents on tasks that share an owned directory in the same wave.

## Tasks
| ID | Task and key steps | Depends on | Done when | Status |
|---|---|---|---|---|
| T-L0-01 | Restructure the repo to the Section 3.2 layout. Move the core to src/core without changing any API. The root CMakeLists.txt only calls add_subdirectory, so lanes never edit it. | none | Full and sanitizer suites still pass. git mv preserves history. No public header changed. | TODO |
| T-L0-02 | Pin dependencies through CMake FetchContent or submodules at release tags for SDL3, SDL3_ttf, Clay, libspng, zlib-ng, libjpeg-turbo, LZ4, LittleCMS2, bcdec, stb_dxt. Record tag and license in docs/DEPENDENCIES.md. | T-L0-01 | Offline reconfigure works after the first fetch. Every dependency has a tag and a license entry. Copyleft needs OWNER approval. | TODO |
| T-L0-03 | CI matrix with MSVC and clang-cl on Windows, Apple Clang on macOS arm64, GCC and Clang on Linux. Jobs for release tests, ASan and UBSan quick, and TSan for threaded code. | T-L0-01 | All jobs green. MSVC builds the core with /W4 /WX after fixing MSVC-only issues such as the atomics path. | TODO |
| T-L0-04 | Implement pal.h v1 (Section 3.11) over SDL3 with native shims. | T-L0-02 | Unit tests for UTF-8 file round trips, including non-ASCII names and paths longer than 260 characters on Windows. Manual dialog smoke test recorded in STATUS.md. | TODO |
| T-L0-05 | App skeleton on SDL main callbacks. Window with SDL_WINDOW_HIGH_PIXEL_DENSITY, SDL_GPU device with SDL_Renderer fallback, EXPOSED redraw during live resize, quit veto. | T-L0-04 | Manual check per OS. Resize keeps redrawing, and closing with unsaved changes prompts. | TODO |
| T-L0-06 | pal_menu API, the macOS NSMenu shim, and the in-window menu for Windows and Linux. | T-L0-05 | The same command ids fire from menus on all three OSes. | TODO |
| T-L1-01 | Slab tile allocator (2 MiB chunks, 16 KiB and 4 KiB classes) behind the pc_tile API, allocator-based byte accounting, and a fault-injection hook that fails the Nth allocation. | none | history_property re-run with random fault injection shows no leaks or corruption and surfaces PC_ERR_NOMEM. | TODO |
| T-L1-02 | Uniform tiles that store one value, keep transparent as NULL, materialize on clone and leave fingerprints unchanged. | T-L1-01 | One solid color on a 16K x 16K layer uses less than 2 MiB of tile data. | TODO |
| T-L1-03 | Selection masks as an A8 tile grid with replace, union, exclude, intersect, xor, invert, select all and select none, all through transactions and history. | none | Combine ops match a per-pixel reference on random masks. Fingerprints include the selection. | TODO |
| T-L1-04 | Antialiased polygon rasterizer to A8 coverage with nonzero and even-odd rules for rectangle, ellipse and lasso. | T-L1-03 | Coverage matches analytic area with mean error at most 1/255 and max error at most 2/255 per pixel on random polygons. | TODO |
| T-L1-05 | Multithreaded compositor over a snapshot (Section 3.6) using a pool interface supplied by the caller. | T-L1-01 | Bit-identical to the single-threaded oracle on random documents. TSan clean. | TODO |
| T-L1-06 | SIMD kernels (SSE4.1, AVX2, NEON) for pc_composite_span per mode. One translation unit per ISA, runtime dispatch. | T-L1-05 | Exhaustive equality with the scalar oracle over the composite_vs_3.36 sweep on each ISA in CI. At least 4x scalar throughput for Normal on AVX2. | TODO |
| T-L1-07 | Mip pyramid per view with a premultiplied 2x2 box per dirty tile, down to 1/64. | T-L1-05 | Matches a reference downsample. Incremental updates touch only the ancestors of dirty tiles. | TODO |
| T-L1-08 | History memory policy with a byte budget from the allocator, LZ4 packing of history-only tiles on workers, swap file (0600, O_EXCL, delete-on-close on Windows), prune by bytes. | T-L1-01 | Soak test with 10,000 strokes on an 8K canvas stays under budget. Undo through spilled tiles reproduces fingerprints. | TODO |
| T-L1-09 | Canvas resize, crop, rotate and flip as history operations with whole-grid payloads. | none | Fingerprint tests including undo, redo and jump. | TODO |
| T-L1-10 | Layer reorder, merge down, flatten and duplicate as history operations using the compositor. | T-L1-05 | Merge and flatten equal the oracle. Undo restores fingerprints. | TODO |
| T-L2-01 | gfx.h backend interface and the SDL_GPU backend (device, swapchain, shaders compiled by SDL_shadercross, pipelines for tile quads, checkerboard and overlays). | G0 | A test document renders on all three OSes. Screenshots attached in STATUS.md. | TODO |
| T-L2-02 | Tile atlas with 2048 x 2048 UNORM pages, LRU residency and cycling transfer-buffer uploads of premultiplied tiles. | T-L2-01, T-L1-05 | A sparse 64K x 64K document pans smoothly and the logged page count stays bounded. | TODO |
| T-L2-03 | Canvas view with Paint.NET zoom steps (from the inventory), nearest filtering at 100 percent and above, mips below, pixel grid and rulers. | T-L2-02, T-L1-07 | Screenshot tests within tolerance. 120 Hz panning on a mid-range GPU. | TODO |
| T-L2-04 | SDL_Renderer display-only fallback backend. | T-L2-01 | A forced-fallback run shows the same CPU-composited pixels. | TODO |
| T-L2-05 | Device-loss handling that recreates the device and re-uploads visible tiles. | T-L2-02 | Simulated loss mid-session causes no data loss and a correct redraw. | TODO |
| T-L3-01 | Clay integration, SDL3_ttf GPU text engine, light and dark theme tokens, DPI scaling. | G0 | A widget gallery renders crisply at 100, 150 and 200 percent scale. | TODO |
| T-L3-02 | Widget set with button, slider, numeric field, color wheel, dropdown, checkbox, thumbnail list, tabs, tooltips, IME-capable text field. Evaluate AccessKit. | T-L3-01 | Gallery with keyboard navigation. IME composition visible. Accessibility evaluation in DECISIONS.md. | TODO |
| T-L3-03 | Shell with image tabs and thumbnails, toolbar, tool options bar, docked Tools, History (tree), Layers and Colors panels, status bar. | T-L3-02, T-L2-03 | Two documents open, tabs switch, panels reflect state. | TODO |
| T-L3-04 | Command system and keymap with Paint.NET-like shortcuts from the inventory, customizable, wired to pal_menu. | T-L0-06 | Every command has an id. The shortcut table is in docs. | TODO |
| T-L3-05 | Auto-generated effect dialogs from the fx_prop schema with live preview. | T-L5-02 | Every built-in effect works without bespoke UI code. | TODO |
| T-L4-01 | Tool framework (tool_vt, tool_ctx), unified pointer events from SDL3 pen and mouse, commit on tool switch, Esc cancels through txn_cancel. | T-L3-03 | Replayable input scripts drive tools headlessly. | TODO |
| T-L4-02 | Brush engine with round dabs, spacing, antialiasing, pressure to size and opacity, writing through transactions. | T-L4-01 | A stroke across tile boundaries equals a single-buffer reference render. | TODO |
| T-L4-03 | Pencil, Paintbrush, Eraser. | T-L4-02 | Golden comparisons against recorded Paint.NET strokes (Section 7.4). | TODO |
| T-L4-04 | Rectangle, Ellipse and Lasso Select with combine modes, and Move Selection. | T-L1-03, T-L1-04 | Golden masks within tolerance. Every selection edit undoes. | TODO |
| T-L4-05 | Magic Wand and Paint Bucket on pc_fill, layer or image sampling, global mode, Paint.NET tolerance curve fitted by golden tests. | T-L1-03 | Wand masks match owner-generated Paint.NET exports on at least 99.9 percent of pixels. | TODO |
| T-L4-06 | Move Selected Pixels with transform handles and nearest, bilinear and bicubic resampling. | T-L4-04 | Round-trip and golden tests. | TODO |
| T-L4-07 | Gradient, Color Picker, Pan, Zoom, Recolor, Clone Stamp, Line/Curve, Shapes, Text (FreeType and HarfBuzz through SDL3_ttf). | T-L4-02 | Each tool has replay tests. | TODO |
| T-L5-01 | Freeze fx_abi.h v1 (Section 3.10). Host gathers ROI plus apron, scatters, blends through the selection mask, and handles cancellation. | G1 | Conformance tests with one test effect built both in and as a plugin. | TODO |
| T-L5-02 | Effect scheduler with viewport-first progressive preview, generation-based cancel and commit through transactions. | T-L5-01 | Cancel lands within one frame and preview never blocks input. | TODO |
| T-L5-03 | Adjustments Invert, Black and White, Brightness/Contrast, Hue/Saturation, Levels, Curves, Posterize, Sepia and Auto-Level (prepare pass). | T-L5-02 | Golden parity per Section 7.4. | TODO |
| T-L5-04 | Effects from the inventory, starting with Gaussian Blur, Sharpen, Motion Blur, Noise and Emboss. | T-L5-02 | Golden parity per Section 7.4. | TODO |
| T-L5-05 | Plugin loader using pal_lib_open with safe flags, fx_entry, ABI and size checks, per-arch binaries, per-OS plugin directory. | T-L5-01 | Malformed plugin fixtures are rejected without crashing. | TODO |
| T-L5-06 | Optional GPU effect path with compute shaders that share the apron contract. The CPU stays authoritative. | T-L2-01, T-L5-04 | GPU output within 1 LSB of the CPU on the test corpus. | TODO |
| T-L6-01 | Codec registry, limits struct (max dimensions, bytes, time) and streaming decode into tiles. Never decode into one contiguous image. | none | A 30000 x 30000 PNG decodes with peak memory near tile data plus one row band. | TODO |
| T-L6-02 | PNG load and save through libspng (limits first, progressive rows). | T-L6-01, T-L0-02 | PngSuite passes. Fuzz target added. | TODO |
| T-L6-03 | JPEG load and save through libjpeg-turbo with scan limits, EXIF orientation, CMYK and quality options. | T-L6-01 | Conformance images pass. Fuzz target added. | TODO |
| T-L6-04 | BMP and TGA, own hardened code. | T-L6-01 | 24 h fuzzing clean. | TODO |
| T-L6-05 | DDS with an own header parser, bcdec decoding, stb_dxt encoding, uncompressed formats, mips and cube maps. | T-L6-01 | 24 h fuzzing clean. Round-trip tests. | TODO |
| T-L6-06 | WebP (libwebp), TIFF (libtiff), OpenRaster layered load and save. | T-L6-02 | Round-trip tests. Fuzz targets. | TODO |
| T-L6-07 | Legacy .pdn import per Section 3.14. | T-L6-01 | Reads owner-provided samples. 24 h fuzzing clean. Unknown versions rejected cleanly. | TODO |
| T-L6-08 | ICC handling through LittleCMS2 on import and export. | T-L6-02 | Profiles round-trip. Display scope per OD-7. | TODO |
| T-L6-09 | Fuzz infrastructure with libFuzzer targets for every decoder and the NRBF parser, corpora in fuzz/corpus, short CI runs. | T-L6-02 | Each target runs 10 minutes in CI with no findings. | TODO |
| T-L7-01 | Black-box feature inventory of Paint.NET 5.1.12 covering menus, tools, shortcuts, effect parameters, defaults and ranges, zoom steps, file options. The owner runs the app and the agent structures docs/inventory. | none (OWNER time) | Owner reviews the inventory. | TODO |
| T-L7-02 | Golden corpus specification and inputs. The owner produces outputs with Paint.NET 5.1.12 on Windows. | T-L7-01 | Corpus committed with a manifest of version, OS, date and settings. | TODO |
| T-L7-03 | goldencmp tool that compares images while ignoring RGB where alpha is 0, reports per-channel max and mean error, and writes a diff heatmap. | none | Used by the tests/golden CTest targets. | TODO |
| T-L8-01 | Packaging as a signed Windows installer or MSIX, a universal macOS .app with hardened runtime and notarization, and Flatpak and AppImage builds. | G3 | Installable artifacts for each OS. | TODO |
| T-L8-02 | Autosave and crash recovery. | T-L1-08 | kill -9 during edits recovers the last autosave. | TODO |
| T-L8-03 | Settings persistence and recent files (portal paths on Flatpak, security-scoped bookmarks on sandboxed macOS). | T-L0-04 | Settings and recents survive restarts on every OS. | TODO |

## Suggested first prompt (ultracode session)
```
Read docs/RULES.md, docs/STATUS.md and docs/TASKS.md.
Run Wave 0 (T-L0-01) by itself and verify it. Then plan Wave 1 as a workflow.
One lane runs T-L0-02, T-L0-03 and T-L0-04 in order; parallel lanes run
T-L1-01, T-L1-03, T-L1-09, T-L6-01 and T-L7-03. Each lane edits only the
directories it owns. After each phase run the full suite and the sanitizer
suite. Update docs/STATUS.md. Stop and ask me before any OWNER decision,
any change to a frozen header, or any new dependency.
```
