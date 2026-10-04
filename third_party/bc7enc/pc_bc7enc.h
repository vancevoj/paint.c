/* pc_bc7enc.h - minimal C17 interface to the vendored bc7enc.c (MIT or
 * public domain, see LICENSE), so first-party code never includes
 * bc7enc.h (its inline helpers and K&R prototypes do not build cleanly
 * under the project warning flags).
 *
 * Threads: pc_bc7enc_init fills global tables and must complete once
 * before any pc_bc7enc_block call (the caller serializes it). After that,
 * pc_bc7enc_block is reentrant (no shared mutable state). */
#ifndef PC_BC7ENC_H
#define PC_BC7ENC_H

#include <stdint.h>

void pc_bc7enc_init(void);

/* Encode one 4x4 block of RGBA8 pixels (R first, row-major, 64 bytes,
 * borrowed) into out[16]. level 0 = fast, 1 = medium, 2 = slow.
 * perceptual != 0 weighs errors in YCbCr space. */
void pc_bc7enc_block(uint8_t out[16], const uint8_t rgba[64], int level, int perceptual);

#endif
