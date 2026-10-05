# Portability patches for files lane L0 does not own

Apply with `git apply docs/patches/<file>` (each was checked with
`git apply --check` against the wave-1 base).

| Patch | Files (owner) | Why | Status |
|---|---|---|---|
| 0001-tests-timespec_get-fallback.patch | tests/pc_test.h (orchestrator), tests/core/pc_tests.c (L1) | C11 `timespec_get` and `TIME_UTC` are missing from msvcrt.dll based MinGW runtimes (Debian's mingw-w64 default), so every test fails to compile there. Falls back to `clock()` when `TIME_UTC` is undefined. | Not needed for the supported toolchains: MSVC, clang-cl and the mingw toolchain file (UCRT via `-mcrtdll=ucrt`) all have it. Recommended for robustness. |
| 0002-core-explicit-narrowing-casts.patch | src/core/pc_doc.c, src/core/pc_fill.c (L1) | `uint8_t x = cond ? 1u : 0u` style assignments are clean under GCC and Clang `-Wconversion` (value-range analysis) but are the pattern MSVC reports as C4244 at /W4, which /WX turns into an error. | Precautionary: MSVC was not available locally; CI (windows-cl) decides. |

Other findings from the review of include/pc and src/core (no change
needed):

- `pc_base.h`: the MSVC atomics use Interlocked intrinsics, so no
  `/experimental:c11atomics`; clang-cl takes the C11 `<stdatomic.h>` path
  (its own header in C mode). `__builtin_mul_overflow` is guarded.
- `pc_mem.c`: `_aligned_malloc` / `_aligned_free` on every Windows
  toolchain; `aligned_alloc` elsewhere needs macOS 10.15 (floor is 13).
- `pc_txn.c` uses a C99 flexible array member, which MSVC reports as C4200;
  suppressed in cmake/PcCommon.cmake with that justification.
- `printf("%zu")` in tests is fine with UCRT (MSVC, mingw with
  `-mcrtdll=ucrt`) and with msvcrt mingw (strict mode enables the mingw
  ANSI stdio).
- Every core and pal source compiles warning-free with the project flags
  for x86_64 and aarch64 Windows (GNU ABI), macOS (arm64, x86_64), FreeBSD,
  musl and glibc (zig cc 0.17), and the whole tree runs its tests under
  Wine.
