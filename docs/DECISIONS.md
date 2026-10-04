# DECISIONS

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
