# RULES

Mirrors Sections 0, 3.4 and 5 of the handoff PDF. IDs are stable; cite them.

## Prime directives
| ID | Directive | Why |
|---|---|---|
| P-01 | Never copy, port, transliterate or paraphrase code from Paint.NET 4.x, 5.x or 6.x binaries, decompiled IL or disassembly. Learn behavior only by running the application, and learn algorithms only from the MIT-licensed 3.36 source. | The current Paint.NET license forbids modification and derivative works. One copied routine endangers the whole project. |
| P-02 | Never reuse Paint.NET icons, logo, images, .resx or .resources files, menu or status-bar text, or the GPC polygon clipper, not even from the 3.36 source. Never ship under the name Paint.NET. | The 3.36 MIT grant explicitly excludes these (CC BY-NC-ND 2.5 and a non-commercial GPC license). Paint.NET is a registered trademark of dotPDN LLC. |
| P-03 | The CPU owns every pixel. GPU textures are disposable caches rebuilt from CPU tiles. | Device loss, driver bugs and readback stalls can then never lose user data. |
| P-04 | Published tiles are immutable. Every pixel change goes through `pc_txn` (clone on first touch, publish on commit). | This removes data races between editing, rendering and history, and makes undo nearly free. |
| P-05 | Every history operation is an involution. Apply equals swap, the payload owns the state that is not in the document, and swap never allocates and never fails. | It eliminates the applied versus unapplied double-free and leak bugs, and undo can never fail half way. |
| P-06 | Core code (`pc_*`) includes only the C standard library and its own headers. No OS, SDL, GPU or UI header may appear in it. | The document model stays portable, testable without a display, and fuzzable. |
| P-07 | No recursion over image data or history trees. Use explicit heap stacks or iterative traversal. | Recursive fills overflow the C stack on large regions. The serpentine test needs a 2.1 million pixel path. |
| P-08 | All size arithmetic goes through `pc_mul_size` and `pc_add_size`. Untrusted input is bounded before anything is allocated. | 32768 x 32768 x 4 wraps a 32-bit size to 0. Decompression bombs are a standard attack. |
| P-09 | Every change lands with tests. Never delete, weaken or skip a test to make it pass. The full suite and the sanitizer suite must pass before a task is done. | Every correctness claim in this document rests on those tests. |
| P-10 | Do not decide anything tagged OWNER. Stop, record the question in `docs/STATUS.md`, and ask. | Parity target, licensing and platform floors change scope, cost and legal exposure. |
| P-11 | `pc_composite_span` is the conformance oracle. SIMD paths must match it bit for bit and GPU paths within a documented tolerance. | Golden tests against Paint.NET only mean something if the integer semantics are exact. |
| P-12 | Update `docs/STATUS.md` at the end of every task with what changed, how it was verified and what comes next. | Sessions and parallel agents share no memory except files in the repository. |

## Invariants
| ID | Invariant | Enforced by | Tested by |
|---|---|---|---|
| INV-TILE-IMMUTABLE | A tile reachable from a layer grid or a history payload is never written. | pc_txn clones on first touch, plus review | history_property fingerprints |
| INV-TILE-OWN | Every stored tile pointer is one counted reference. | retain and release discipline | Leak checks in tiles and history tests |
| INV-TILE-EDGE | Pixels of edge tiles outside the document are zero. | Zeroed allocation, tools clip to the document | pc_doc_edge_padding_is_zero in history_property |
| INV-DOC-OWN | Each layer is owned by exactly one of the stack, a payload, or the caller. | API contracts in pc_doc.h | Layer leak check |
| INV-DOC-CAP | Layer stack capacity never shrinks and is reserved before apply. | pc_doc_reserve_layers | PC_ASSERT inside the layer swap |
| INV-DOC-ID | Other modules, history and jobs refer to layers by id, never by pointer. | API design | Review |
| INV-TXN-EXCLUSIVE | At most one open transaction per document, and no other mutation while it is open. | open_txns and PC_ERR_STATE | history_property edge cases |
| INV-HIST-SWAP | A payload owns the off-document state. Apply, undo and redo are the same swap, and swap never allocates. | Operation design | history_property |
| INV-HIST-PATH | Swap is only called on an edge adjacent to the current node. | undo, redo and jump implementation | Random jumps in history_property |
| INV-HIST-PRUNE | Pruning never touches the root-to-current path, except for root collapse. | mark_path | Random prunes in history_property |
| INV-ITER | No recursion over images or trees. | Review | flood_stress serpentine |
| INV-ORACLE | SIMD and GPU kernels equal the scalar oracle. | Tests to add in T-L1-06 and T-L5-06 | TODO |

## Never do
| ID | Do not | Why | Do instead |
|---|---|---|---|
| X-01 | Decompile or copy Paint.NET 4.x, 5.x or 6.x code | License forbids derivative works | Black-box behavior plus the 3.36 MIT source |
| X-02 | Reuse Paint.NET icons, images, .resx text, logo or name | License exceptions and trademark | Original assets sourced by the owner |
| X-03 | Use GPC or any 3.36 GPC code | Non-commercial license | A8 mask rasterizer and per-tile mask ops |
| X-04 | Link or load PaintDotNet DLLs, for example to run Paint.NET plugins | The license FAQ prohibits use in other software | The native fx ABI only |
| X-05 | Write into a tile reachable from a layer grid or a history payload | INV-TILE-IMMUTABLE | pc_txn_tile_rw |
| X-06 | Allocate or fail inside a history swap | INV-HIST-SWAP | Reserve in the apply phase, as pc_hist_add_layer does |
| X-07 | Hold raw pc_layer pointers across frames, threads or history | INV-DOC-ID | Layer ids |
| X-08 | Include SDL, OS or GPU headers in core | P-06 | pal.h function tables |
| X-09 | Recurse over pixels or trees | C stack overflow | Explicit stacks, iterative traversal |
| X-10 | Multiply sizes unchecked or allocate before validating limits | Overflow and decompression bombs | pc_mul_size and codec limits |
| X-11 | Use stb_image or other decoders not built for untrusted input in shipping paths | Documented DoS risk, history of CVEs | libspng, libjpeg-turbo, hardened own parsers. stb_image is fine in tests and tools |
| X-12 | Create document-sized GPU textures or keep authoritative pixels on the GPU | Texture size limits, device loss | Atlas pages filled from CPU tiles |
| X-13 | Use sRGB textures or linear-light blending on the 5.1 parity path | Paint.NET 5.1 blends gamma-encoded 8-bit values | UNORM formats and the integer oracle |
| X-14 | Filter straight-alpha textures | Halos at transparent edges | Premultiply before upload |
| X-15 | Call SDL_GPU from worker threads or acquire the swapchain off the window thread | SDL_GPU threading rules | The main thread owns the GPU (C-13) |
| X-16 | Block the main thread on file I/O, readback or long effects | Input latency | I/O thread, fenced async readback, workers |
| X-17 | Let a plugin free host memory or the host free plugin memory | Cross-CRT heap corruption on Windows | fx_host alloc and free |
| X-18 | Load DLLs through the default search order | DLL hijacking | LoadLibraryExW with explicit search flags |
| X-19 | Deserialize generic object graphs in the .pdn reader | NRBF can describe arbitrary .NET objects | Whitelisted records and classes only |
| X-20 | Delete, weaken or skip tests, or widen tolerances to pass | Hides regressions | Fix the code or escalate to the owner |
| X-21 | Change a frozen public header without a decision record | Parallel agents depend on it | An ADR in docs/decisions and a version bump |
| X-22 | Add a third-party dependency without a license review | Copyleft or non-commercial contamination | DEPENDENCIES.md entry, owner approval for copyleft |
| X-23 | Write user image data to world-readable temp files or leave swap files behind | Privacy | Per-user directories, mode 0600, delete-on-close |
| X-24 | Target Paint.NET 5.2 or 6.0 behavior in v1 without owner approval | Moving target | 5.1.12 parity (OD-1) |
| X-25 | Make AVX2 or AVX-512 code reachable before CPU detection | SIGILL on older CPUs | Separate translation units plus cpuid dispatch |
| X-26 | Rely on malloc returning NULL on Linux | Overcommit, then the OOM killer | Own budget accounting |
| X-27 | Put side effects inside PC_ASSERT | Vanish when PC_NO_ASSERT is defined | Evaluate first, assert on the result |
