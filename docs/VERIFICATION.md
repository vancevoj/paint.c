# VERIFICATION (2026-10-04)

## Test results
| Group | Checks | Result | Notable |
|---|---|---|---|
| mul255_exhaustive | 65,537 | pass | Rounding formula exact for every product 0 to 65662 |
| blend_channel_vs_3.36 | 1,836,288 | pass | All 14 modes x 65,536 input pairs equal a literal transliteration of the 3.36 macros |
| composite_vs_3.36 | 6,422,528 | pass | Every backdrop alpha x source alpha pair, 7 opacities, 14 modes, bit-exact |
| composite_error_analysis | 1 plus statistics | pass | Against the ideal real-valued W3C result, alpha error at most 1 LSB. Color error at most 3 LSB at output alpha 64 to 255, 8 at 16 to 63, up to 75 at 1 to 15. This is inherent in Paint.NET's integer weights. |
| tiles_refcount_accounting | 9 | pass | Alignment, cloning, refcounts and counters return to baseline |
| doc_limits_sparsity | 7 | pass | A blank 65535 x 65535 layer allocates 0 tiles |
| history_property | 56,352 | pass | 25,000 random operations including 2,505 jumps and 1,271 prunes, no leaks |
| tolerance_metric | 7,902,137 | pass | Tolerance 0 is exact, 100 percent accepts everything, symmetric, monotonic |
| flood_vs_bfs_random | 4,205,894 | pass | 1,500 random images give masks identical to a BFS reference |
| flood_stress | 4 | pass | 4096 x 4096 at 112 Mpx/s. A 2049 x 2049 serpentine runs with stack high water 1 |
| bench_scalar | 1 | info | Scalar oracle on one thread. Normal 111, Multiply 59, Overlay 49 Mpx/s |

- Full suite (GCC 13.3, -O2, Ubuntu 24.04, one vCPU Xeon at 2.8 GHz). 20,488,758 checks, 0 failures.
- ASan plus UBSan quick suite with leak detection. 4,454,881 checks, 0 failures.
- CMake 4.4 configure, build and CTest. 1 of 1 tests passed.
- zig cc compile of every source with -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Werror for x86_64-windows-gnu, aarch64-windows-gnu, aarch64-macos, x86_64-macos and x86_64-linux-musl. All OK.
- A static x86_64-linux-musl binary built by zig ran the quick suite and passed.
- -Wconversion -Wsign-conversion over src/ gives 0 warnings.
- Not tested yet. MSVC (cl.exe), real macOS and Windows hardware, big-endian targets, and multithreaded use (the core is single-writer by design).

## Performance baselines
| Measurement | Value | Conditions |
|---|---|---|
| Scalar composite, Normal | 110.9 Mpx/s | Reference machine, one thread, -O2 |
| Scalar composite, Multiply | 58.9 Mpx/s | Same |
| Scalar composite, Overlay | 49.0 Mpx/s | Same |
| Contiguous flood, uniform 4096 x 4096 | 111.9 Mpx/s | Same |
| History property test, 25,000 steps | 18.2 s | Dominated by fingerprint hashing, not history |

## Verified external facts
| Fact | Source |
|---|---|
| SDL3 has SDL_ShowOpenFileDialog, SDL_ShowSaveFileDialog, SDL_ShowOpenFolderDialog and SDL_ShowFileDialogWithProperties. The callback may run on a thread other than the caller's. | SDL main, include/SDL3/SDL_dialog.h |
| A window may be redrawn directly from an event watcher on SDL_EVENT_WINDOW_EXPOSED, and data1 is 1 for live-resize exposes. | SDL_events.h |
| The pen API has SDL_EVENT_PEN_DOWN, UP, MOTION and AXIS, SDL_PEN_AXIS_PRESSURE in the range 0 to 1, and SDL_PEN_INPUT_ERASER_TIP. | SDL_pen.h, SDL_events.h |
| SDL_EVENT_DROP_FILE also carries system file-open requests, such as Finder opening a document. | SDL_events.h |
| The clipboard works by MIME type through SDL_SetClipboardData, SDL_GetClipboardData, SDL_HasClipboardData and SDL_GetClipboardMimeTypes. | SDL_clipboard.h |
| SDL_SetWindowsMessageHook, SDL_RunOnMainThread(callback, userdata, wait_complete), SDL_IsMainThread, and the main-callback model (SDL_MAIN_USE_CALLBACKS, SDL_AppIterate, SDL_AppEvent) exist. | SDL_system.h, SDL_init.h, SDL_main.h |
| SDL_GPU backends are Vulkan 1.0 with listed extensions, D3D12 and Metal. Shader formats are SPIRV, DXBC, DXIL, MSL and METALLIB. | SDL_gpu.h |
| SDL_MapGPUTransferBuffer(device, transfer_buffer, cycle) exists, and cycling turns resources into internal ring buffers. | SDL_gpu.h |
| Command buffers may only be used and submitted on the thread that acquired them, and swapchain textures may only be acquired on the thread that created the window. | SDL_gpu.h |
| The SDL main branch reports version 3.5.0, a development version. Builds must pin a released 3.x tag. | SDL_version.h |
| SDL_shadercross takes HLSL or SPIR-V and outputs DXBC, DXIL, SPIR-V, MSL or HLSL. It has an offline CLI, a zlib license, and depends on SPIRV-Cross and DXC. | SDL_shadercross README.txt |
| SDL3_ttf has TTF_CreateGPUTextEngine(SDL_GPUDevice *). HarfBuzz is optional, and TTF_GetHarfBuzzVersion reports 0.0.0 without it. | SDL3_ttf SDL_ttf.h |
| libspng has spng_set_image_limits, spng_set_chunk_limits, SPNG_DECODE_PROGRESSIVE and spng_decode_row. | libspng spng/spng.h |
| TurboJPEG 3 has TJPARAM_SCANLIMIT, TJPARAM_MAXMEMORY and TJPARAM_MAXPIXELS. | libjpeg-turbo src/turbojpeg.h |
| stb_image documents denial-of-service risk on untrusted data. STBI_MAX_DIMENSIONS defaults to 1 shl 24. | stb_image.h |
| bcdec decodes BC1 to BC5, BC6H (float) and BC7. | bcdec.h |
| stb_dxt encodes BC1 and BC3 (stb_compress_dxt_block), BC4 and BC5. | stb_dxt.h |
| Clay works as a single .h include from C99. | Clay README |
| LZ4 has LZ4_compress_default and LZ4_decompress_safe. LittleCMS master is version 2.19. | lz4.h, lcms2.h |
| Pinta is MIT, uses GTK4, libadwaita and .NET 10, and uses Paint.NET 3.36 code under MIT. | Pinta readme.md |
| The 3.36 license is MIT with three exceptions. Logo and icon art are CC BY-NC-ND 2.5. Resource assets (.resources, .resx, .png, menu and status text) are CC BY-NC-ND 2.5. GPC needs a commercial license from the University of Manchester. | OpenPDN src/Resources/Files/License.txt |
| The current Paint.NET license forbids modifying, adapting or creating derivative works. Its FAQ prohibits using Paint.NET in other software, and Paint.NET is a registered trademark of dotPDN LLC. | getpaint.net/license.html |
| The 3.36 blend ops use INT_SCALE (identical to pc_mul255), weights y, x and z, and table-driven division. The table was proved equal to floor division for every divisor 1 to 255 and every numerator 0 to 65025. | OpenPDN src/Data/UserBlendOps.Generated.H.cs |
| A legacy .pdn starts with PDN3, a 3-byte little-endian header length, a UTF-8 XML header, the bytes 00 01 and an MS-NRBF document. Layer pixel chunks follow. The blend enum runs Normal = 0 to Xor = 13. | pypdn 1.0.6 reader (MIT). Writers newer than pypdn's samples are VERIFY |
| Stable is 5.1.12 (2026-03-08). The 5.2 betas have an FP32 engine, 6.0 brings a new .pdn format, and experimental Wine support exists. | Wikipedia, and a deskmodder.de report of the release notes (2026-10-03). Secondary source, re-check forums.paint.net |
| MSVC C11 atomics need /std:c11 or later plus /experimental:c11atomics, Visual Studio 2022 17.5 or later. | Microsoft C++ team blog |
