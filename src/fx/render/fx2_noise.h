/* fx2_noise.h - seeded gradient noise for the lane L5C effects (Clouds,
 * Turbulence, Dents). Private to src/fx; not part of the plugin ABI.
 *
 * The gradient noise is Ken Perlin's improved noise reduced to two dimensions,
 * in the form used by the MIT-licensed Paint.NET 3.36 CloudsEffect and
 * PerlinNoise2D (attribution: docs/notice/l5c.md). Instead of a fixed
 * permutation XORed with a byte seed, every invocation builds its own
 * permutation from the full 32-bit seed (prepare()), so all seeds differ.
 *
 * Thread rules: fx2_perm_init writes only its output; the noise functions only
 * read the table, so any number of threads may share one table.
 */
#ifndef FX2_NOISE_H
#define FX2_NOISE_H

#include <stdint.h>

typedef struct fx2_perm { uint8_t p[512]; } fx2_perm;   /* p[i + 256] == p[i] */

/* Deterministic permutation of 0..255 for (seed, salt), duplicated to 512. */
void fx2_perm_init(fx2_perm *perm, uint32_t seed, uint32_t salt);

/* Gradient noise at integer lattice cell (ix, iy) (taken modulo 256) and
 * fractional position (fx, fy) in [0, 1). off (0..255) rotates the hash so one
 * table yields independent octaves. Range about [-1, 1]. */
double fx2_noise_cell(const fx2_perm *perm, int32_t ix, int32_t iy, double fx, double fy,
                      uint8_t off);

/* Gradient noise at a continuous position (0 for non-finite input); positions
 * beyond +-2^30 wrap like the lattice does. */
double fx2_noise(const fx2_perm *perm, double x, double y, uint8_t off);

/* Fractal sum in the form of Paint.NET 3.36 PerlinNoise2D.Noise: ceil(detail)
 * octaves (the last one weighted by the fractional part), each rotated by
 * 137.2 degrees and offset by prime numbers, amplitude multiplied by roughness
 * per octave. */
double fx2_noise_fractal(const fx2_perm *perm, double x, double y, double detail,
                         double roughness);

#endif /* FX2_NOISE_H */
