/* fx2_noise.c - seeded 2-D gradient noise (see fx2_noise.h).
 * Algorithm derived from the MIT-licensed Paint.NET 3.36 CloudsEffect.cs and
 * PerlinNoise2D.cs (Fade, Grad, lattice hashing, octave rotation); see
 * docs/notice/l5c.md. The permutation is generated, not copied. */
#include "fx2_noise.h"

#include <math.h>

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void fx2_perm_init(fx2_perm *perm, uint32_t seed, uint32_t salt)
{
    uint64_t st = ((uint64_t)seed << 32) ^ (uint64_t)salt ^ 0x5DEECE66Dull;
    int i;
    for (i = 0; i < 256; i++) perm->p[i] = (uint8_t)i;
    for (i = 255; i > 0; i--) {                 /* Fisher-Yates */
        uint32_t j = (uint32_t)(splitmix64(&st) % (uint64_t)(i + 1));
        uint8_t t = perm->p[i];
        perm->p[i] = perm->p[j];
        perm->p[j] = t;
    }
    for (i = 0; i < 256; i++) perm->p[256 + i] = perm->p[i];
}

static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
static double lerp(double a, double b, double t) { return a + t * (b - a); }

static double grad(int hash, double x, double y)
{
    int h = hash & 15;
    double u = h < 8 ? x : y;
    double v = h < 4 ? y : ((h == 12 || h == 14) ? x : 0.0);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

double fx2_noise_cell(const fx2_perm *perm, int32_t ix, int32_t iy, double fx, double fy,
                      uint8_t off)
{
    const uint8_t *p = perm->p;
    int x = (int)((uint32_t)ix & 255u), y = (int)((uint32_t)iy & 255u);
    double u = fade(fx), v = fade(fy);
    int a = (int)p[(x + off) & 511] + y;        /* <= 510 */
    int b = (int)p[(x + 1 + off) & 511] + y;
    int aa = p[a], ab = p[a + 1], ba = p[b], bb = p[b + 1];
    double e1 = lerp(grad(p[aa], fx, fy), grad(p[ba], fx - 1.0, fy), u);
    double e2 = lerp(grad(p[ab], fx, fy - 1.0), grad(p[bb], fx - 1.0, fy - 1.0), u);
    return lerp(e1, e2, v);
}

double fx2_noise(const fx2_perm *perm, double x, double y, uint8_t off)
{
    double xf, yf;
    if (!isfinite(x) || !isfinite(y)) return 0.0;
    /* the lattice repeats every 256 cells: fold far coordinates exactly */
    if (x > 1073741824.0 || x < -1073741824.0) x = fmod(x, 256.0);
    if (y > 1073741824.0 || y < -1073741824.0) y = fmod(y, 256.0);
    xf = floor(x);
    yf = floor(y);
    return fx2_noise_cell(perm, (int32_t)xf, (int32_t)yf, x - xf, y - yf, off);
}

double fx2_noise_wrap(const fx2_perm *perm, double x, double y, int32_t pw, int32_t ph,
                      uint8_t off)
{
    const uint8_t *p = perm->p;
    double xf, yf, fx, fy, u, v, e1, e2;
    int32_t x0, y0, x1, y1;
    int a0, a1;
    if (!isfinite(x) || !isfinite(y) || pw < 1 || ph < 1) return 0.0;
    x = fmod(x, (double)pw);
    y = fmod(y, (double)ph);
    if (x < 0.0) x += (double)pw;
    if (y < 0.0) y += (double)ph;
    xf = floor(x);
    yf = floor(y);
    fx = x - xf;
    fy = y - yf;
    x0 = (int32_t)xf % pw;
    y0 = (int32_t)yf % ph;
    x1 = (x0 + 1) % pw;
    y1 = (y0 + 1) % ph;
    u = fade(fx);
    v = fade(fy);
    a0 = (int)p[((x0 & 255) + off) & 511];
    a1 = (int)p[((x1 & 255) + off) & 511];
    e1 = lerp(grad(p[a0 + (y0 & 255)], fx, fy), grad(p[a1 + (y0 & 255)], fx - 1.0, fy), u);
    e2 = lerp(grad(p[a0 + (y1 & 255)], fx, fy - 1.0),
              grad(p[a1 + (y1 & 255)], fx - 1.0, fy - 1.0), u);
    return lerp(e1, e2, v);
}

double fx2_noise_fractal(const fx2_perm *perm, double x, double y, double detail,
                         double roughness)
{
    /* rotation by 137.2 degrees decorrelates the octaves (3.36 constant) */
    const double ang = 137.2 / 180.0 * 3.14159265358979323846;
    const double r11 = cos(ang), r12 = -sin(ang), r21 = sin(ang), r22 = cos(ang);
    double total = 0.0, frequency = 1.0, amplitude = 1.0, partial = detail;
    int octaves = (int)ceil(detail), i;
    if (octaves > 64) octaves = 64;
    for (i = 0; i < octaves; i++) {
        double xr = x * r11 + y * r12;
        double yr = x * r21 + y * r22;
        double n = fx2_noise(perm, xr * frequency, yr * frequency, 0) * amplitude;
        if (partial < 1.0) n *= partial;
        total += n;
        amplitude *= roughness;
        if (amplitude < 0.001) break;
        frequency += frequency;
        partial -= 1.0;
        x = xr + 499.0;
        y = yr + 506.0;
    }
    return total;
}
