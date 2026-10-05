/* pc_bc7enc.c - paint.c wrapper around the vendored bc7enc.c (see
 * pc_bc7enc.h). Compiled without the project warning flags. */
#include "pc_bc7enc.h"
#include "bc7enc.h"

void pc_bc7enc_init(void)
{
    bc7enc_compress_block_init();
}

void pc_bc7enc_block(uint8_t out[16], const uint8_t rgba[64], int level, int perceptual)
{
    bc7enc_compress_block_params p;
    p.m_max_partitions_mode = level <= 0 ? 16u : (level == 1 ? 32u : BC7ENC_MAX_PARTITIONS1);
    p.m_uber_level = level <= 0 ? 0u : (level == 1 ? 1u : BC7ENC_MAX_UBER_LEVEL);
    p.m_try_least_squares = BC7ENC_TRUE;
    p.m_mode_partition_estimation_filterbank = level >= 2 ? BC7ENC_FALSE : BC7ENC_TRUE;
    p.m_use_mode5_for_alpha = BC7ENC_TRUE;
    p.m_use_mode7_for_alpha = BC7ENC_TRUE;
    if (perceptual) {
        p.m_perceptual = BC7ENC_TRUE;
        p.m_weights[0] = 128; p.m_weights[1] = 64; p.m_weights[2] = 16; p.m_weights[3] = 32;
    } else {
        p.m_perceptual = BC7ENC_FALSE;
        p.m_weights[0] = 1; p.m_weights[1] = 1; p.m_weights[2] = 1; p.m_weights[3] = 1;
    }
    (void)bc7enc_compress_block(out, rgba, &p);
}
