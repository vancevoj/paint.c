# DECISIONS

## Architecture decision records (orchestrator, 2026-10-04)
The owner's directive ("name it paint.c ... do not stop until it works on Linux
(Wayland and X11), Windows and macOS ... take different approaches when you see
fit") authorizes the orchestrating agent to answer OWNER items with the defaults
below instead of blocking (overrides the blocking part of P-10). Every answer is
recorded here; lane agents still never decide OWNER items on their own.

| ID | Decision | Why |
|---|---|---|
| ADR-001 | Product name **paint.c** (OD-3). Executable `paintc`, macOS bundle `paint.c.app`, app id `org.paintc.paintc`. C prefix `pc_` is unchanged. | Owner instruction. No Paint.NET trademark use (P-02). |
| ADR-002 | Clean-room parity only. "Reverse engineer" means black-box behavior (official documentation, observing the running app) plus algorithms from the MIT 3.36 source. Never decompile or disassemble 4.x+ (P-01, X-01). | Paint.NET license forbids derivative works. |
| ADR-003 | Display backend is **SDL_Renderer** (SDL3 2D API) instead of SDL_GPU + SDL_shadercross for v1. Tile-page textures, premultiplied uploads (SDL_BLENDMODE_BLEND_PREMULTIPLIED), nearest at zoom >= 100%, CPU mips + linear below. An SDL_GPU backend may come later behind gfx.h. | CPU owns every pixel (P-03), so the GPU only scales textures and draws overlays. SDL_Renderer covers D3D11/12, Metal, Vulkan, OpenGL and software without an offline shader toolchain (DXC, SPIRV-Cross), and its software path makes headless screenshot tests possible. |
| ADR-004 | UI is a custom immediate-mode toolkit over SDL_Renderer with **stb_truetype** and an embedded OFL/permissive UI font, instead of Clay + SDL3_ttf (FreeType, HarfBuzz). | All C, two fewer heavy dependencies, deterministic offscreen rendering for tests. Complex-script shaping is deferred (OD-5 default kept: custom UI). |
| ADR-005 | Effect ABI v1 uses the **full-source model**: the host snapshots the whole layer into src, render() writes disjoint ROIs of dst, and an optional prepare()/release() pair builds immutable per-invocation state. Replaces the apron-gather design and resolves C-11 in v1. | Matches Paint.NET's effect model; distortions read arbitrarily far away and Auto-Level style effects need whole-image statistics. |
| ADR-006 | TIFF uses an **own hardened codec** (baseline, strips and tiles, none/LZW/PackBits/Deflate, 8/16-bit, gray/RGB/RGBA/palette) instead of libtiff. | Smaller attack surface, fewer dependencies, fuzzable. |
| ADR-007 | **.pdn writing is in scope** (OD-6 answered yes). The writer must reproduce the record structure of real Paint.NET 4.x/5.x files and is verified by round trips through our reader and the independent MIT pypdn reader. | A Paint.NET clone needs its native layered format; strict structural mirroring limits the risk the plan named. |
| ADR-008 | Owner defaults adopted: OD-1 5.1.12 behavior, OD-2 MIT license, OD-4 Windows 10 22H2 / macOS 13 / glibc 2.31, OD-5 custom UI, OD-7 ICC convert on import and export, OD-8 proceed, OD-9 plugins load only from the per-user plugin folder (macOS: ad-hoc signed builds disable library validation only for that folder's plugins via a documented entitlement), OD-10 25% of RAM (min 1 GiB). | Defaults from Section 10, confirmed by the owner's "do not stop" directive. |
| ADR-009 | OD-11: no Windows machine with Paint.NET is available, so the golden corpus starts from documented behavior and the 3.36 algorithms. Wine-produced outputs may be used as non-authoritative hints. Parity gaps are listed in docs/inventory/PARITY.md. | Keeps parity work unblocked and honest about what is verified. |
| ADR-010 | Codec registry and built-in effect list are **generated from file names** (src/codec/fmt_<id>.c, src/fx/**/fxm_<name>.c); tests are one executable per tests/<lane>/test_*.c. | Parallel lanes add files without touching shared CMake or registry files. |
| ADR-011 | Formats: add **GIF** (Paint.NET supports it; the plan omitted it). AVIF, HEIC and JPEG XL are optional later work behind system libraries. | Paint.NET 5.1 file-type parity. |
| ADR-012 | Status codes extended (PC_ERR_FORMAT, PC_ERR_UNSUPPORTED, PC_ERR_IO, PC_ERR_CANCELLED) and pc_status_str added to pc_base.h. New core headers pc_par.h, pc_surf.h, pc_comp.h, pc_codec.h; new include/fx/fx_abi.h, fx_util.h, fx_builtin.h; include/pal/pal.h. | Contracts frozen before the parallel waves start (X-21). |

## Owner decisions (defaults stay in effect until the owner answers)
| ID | Question | Default in effect | Affects | Owner answer |
|---|---|---|---|---|
| OD-1 | Parity target. Paint.NET 5.1.12 (8-bit) or 6.0 (FP32)? | 5.1.12 for v1. 6.0 later | Blend oracle, golden corpus, memory use |  |
| OD-2 | License of this project | MIT, compatible with the 3.36 MIT attribution (OWNER confirm) | Dependency choices, plugin ecosystem |  |
| OD-3 | Product name and visual identity | Working name PortableCanvas, C prefix pc_. Trademark check needed | Packaging, assets |  |
| OD-4 | OS floors | Windows 10 22H2, macOS 13, glibc 2.31 | API availability, CI images |  |
| OD-5 | UI approach and accessibility risk | Custom UI over SDL3 and Clay. GTK4 is the alternative | L3 scope, accessibility |  |
| OD-6 | .pdn writing | No, import only | Format risk |  |
| OD-7 | Color management scope | Convert ICC on import and export. Display management later | Codec work, rendering |  |
| OD-8 | Motivation, given official Wine support | Proceed for native macOS, performance and open code | Whether the project continues |  |
| OD-9 | macOS plugin trust model | Signed plugins only. Disabling library validation needs approval | Plugin ecosystem, security |  |
| OD-10 | Default history RAM budget | 25 percent of physical RAM, minimum 1 GiB | Memory policy |  |
| OD-11 | Who produces the golden corpus, and when | The owner on Windows with Paint.NET 5.1.12 | Parity tasks are blocked until then |  |

## Corrections to the earlier chat specification
| ID | Chat said | Now | Why |
|---|---|---|---|
| C-01 | Composite kernel with rounded division and a guard against co greater than ao | 3.36-exact kernel with rounded weights y, x and z and floor division | The chat kernel differed from Paint.NET semantics in 91.2 percent of random samples, by up to 126 LSB. |
| C-02 | A transparent source is a no-op in every mode | True, except that a result with total alpha 0 is zeroed, so RGB becomes 0 | 3.36 returns 0 when total alpha is 0. Only hidden RGB is affected. |
| C-03 | Tolerance metric with integer division by 255 | Division-free scaled metric | Division let tolerance 0 accept small differences at low alpha, for example {10,10,10,1} against {11,10,10,1}. |
| C-04 | Separate apply and swap code paths | Apply equals swap for every operation | One code path, exercised by every redo. |
| C-05 | The layer stack could reallocate during undo | Capacity is reserved before apply and never shrinks (INV-DOC-CAP) | Swap must never allocate or fail. |
| C-06 | Transactions were implicit | At most one open transaction per document. Undo, redo and layer operations refuse while one is open | Prevents deleting a layer under an open stroke. |
| C-07 | 8-bit parity target assumed to be stable | Pinned to 5.1.12, FP32 deferred | Paint.NET's engine changes in 5.2 and 6.0. |
| C-08 | The 3.36 source called MIT without caveats | MIT except artwork, resource assets and GPC | License text. |
| C-09 | Placeholder names (pdnx_, tile, px32) | Core prefix pc_, macros PC_, effect ABI fx_, PAL pal_ | Avoid trademark-adjacent names and keep namespaces consistent. |
| C-10 | SDL 3.x unpinned | Pin a released tag in T-L0-02 | Main is 3.5.0 development. Builds must be reproducible. |
| C-11 | Effect ABI v1 had no prepare pass | Prepare pass planned for ABI v2 with a read-only tile iterator | Auto-Level style effects need whole-image statistics. |
| C-12 | Uniform tile flag listed in the tile header | Only a reserved flags field exists. Uniform, packed and swapped states are TODO | Not implemented. Do not assume them. |
| C-13 | A dedicated render thread owns the GPU | The main thread owns the GPU device and all command buffers | SDL_GPU requires swapchain acquisition on the window thread and binds command buffers to their acquiring thread. |
| C-14 | Document read-write lock around compositing | No document lock. The main thread is the only writer and also takes the tile snapshot | Workers only read retained, immutable tiles, so there is nothing to lock. |
