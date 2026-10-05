/* gfx_ants.c - marching ants: selection contours drawn as a white outline
 * with moving black dashes, crisp in screen space (see gfx.h). */
#include "gfx.h"

#include <SDL3/SDL.h>
#include <math.h>

/* Cohen-Sutherland style trivial reject of a segment against a rect. */
static bool seg_outside(double ax, double ay, double bx, double by, double x0, double y0,
                        double x1, double y1)
{
    return (ax < x0 && bx < x0) || (ax > x1 && bx > x1) || (ay < y0 && by < y0) ||
           (ay > y1 && by > y1);
}

void gfx_draw_ants(SDL_Renderer *r, const gfx_view *v, const pc_poly *p, double phase,
                   double dash, pc_rect clip)
{
    double ox, oy, cx0, cy0, cx1, cy1, period;
    if (!r || !v || !p || p->n_contours == 0u) return;
    if (!(dash > 0.5)) dash = 4.0;
    period = 2.0 * dash;
    gfx_view_origin(v, &ox, &oy);
    cx0 = (double)clip.x - 1.0;
    cy0 = (double)clip.y - 1.0;
    cx1 = (double)clip.x + (double)clip.w + 1.0;
    cy1 = (double)clip.y + (double)clip.h + 1.0;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        else SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        for (size_t ci = 0; ci < p->n_contours; ci++) {
            size_t s = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - s;
            double run = fmod(phase, period);
            if (n < 2u) continue;
            if (run < 0.0) run += period;
            for (size_t k = 0; k < n; k++) {
                const pc_pt *a = &p->pts[s + k], *b = &p->pts[s + (k + 1u) % n];
                double ax = floor(ox + a->x * v->zoom + 0.5), ay = floor(oy + a->y * v->zoom + 0.5);
                double bx = floor(ox + b->x * v->zoom + 0.5), by = floor(oy + b->y * v->zoom + 0.5);
                double len = hypot(bx - ax, by - ay), t;
                if (len <= 0.0) continue;
                if (seg_outside(ax, ay, bx, by, cx0, cy0, cx1, cy1)) {
                    run = fmod(run + len, period);
                    continue;
                }
                if (pass == 0) {
                    SDL_RenderLine(r, (float)ax, (float)ay, (float)bx, (float)by);
                    continue;
                }
                /* black dashes: the first half of every period, offset by run */
                t = 0.0;
                while (t < len) {
                    double in_period = fmod(run + t, period), step;
                    if (in_period < dash) {
                        double end = t + (dash - in_period);
                        double t1 = end < len ? end : len;
                        double fx0 = ax + (bx - ax) * (t / len), fy0 = ay + (by - ay) * (t / len);
                        double fx1 = ax + (bx - ax) * (t1 / len),
                               fy1 = ay + (by - ay) * (t1 / len);
                        SDL_RenderLine(r, (float)fx0, (float)fy0, (float)fx1, (float)fy1);
                        step = t1 - t;
                    } else {
                        step = period - in_period;
                    }
                    if (step <= 1e-9) step = 1e-3;
                    t += step;
                }
                run = fmod(run + len, period);
            }
        }
    }
}
