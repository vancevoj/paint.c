/* bc6h_enc.h - BC6H (unsigned half float) block encoder (lane CODEC,
 * F-FILE-DDS-SAVE-FORMAT-BC6H). Private to src/codec and its tests.
 *
 * Written from the BC6H format specification (Direct3D 11 functional
 * specification, "BC6H Format"; Khronos Data Format Specification 1.3,
 * section "BC6H"); the per-mode bit layouts and the partition table were
 * transcribed with a script from the decoder in third_party/bcdec (MIT),
 * which also checks the encoder in the tests. All 14 modes are supported:
 * every effort tries the four one-region modes and the ten two-region modes
 * on the most promising partitions (1, 4 or 12 for fast, medium and slow),
 * with 1, 2 or 3 least-squares refinement passes.
 *
 * Threads: reentrant, no global mutable state.
 */
#ifndef PC_BC6H_ENC_H
#define PC_BC6H_ENC_H

#include <stdint.h>

enum { BC6H_FAST = 0, BC6H_MEDIUM = 1, BC6H_SLOW = 2 };

/* Encode one 4x4 block (row-major pixels; rgb[3 * i + c] are IEEE half
 * floats of pixel i, channel r, g, b, with the sign bit clear, at most
 * 0x7BFF; 48 values, borrowed) into 16 bytes of BC6H_UF16. effort is
 * BC6H_FAST, BC6H_MEDIUM or BC6H_SLOW. Deterministic. */
void bc6h_encode_block(uint8_t out[16], const uint16_t *rgb, int effort);

/* Test hook: encode with one mode (0..13 in the order of the specification,
 * mode 1 = index 0) and partition (0..31, two-region modes), and return the
 * half floats a conforming decoder must produce in dec. */
void bc6h_encode_forced(uint8_t out[16], const uint16_t *rgb, int mode, int part,
                        uint16_t *dec);

/* IEEE half of a float >= 0 (round to nearest even; NaN and negatives give
 * 0, values above 65504 give 0x7BFF) and back. */
uint16_t bc6h_float_to_half(float f);
float    bc6h_half_to_float(uint16_t h);

#endif /* PC_BC6H_ENC_H */
