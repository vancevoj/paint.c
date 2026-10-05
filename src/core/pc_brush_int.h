/* pc_brush_int.h - internals shared by the brush engine files (lane E1).
 * Not a public header; only src/core/pc_brush*.c include it. */
#ifndef PC_BRUSH_INT_H
#define PC_BRUSH_INT_H

#include "pc/pc_brush.h"

/* Soft round dab of the antialiased brush: a disk of radius r0 blurred by
 * a Gaussian of standard deviation sigma (pixel area antialiasing folded
 * into sigma). See pc_brush_profile.c for the measured parameters. */
typedef struct pcb_soft {
    double       r0, sigma, inv_sigma;
    double       cut;        /* radius (px) beyond which the value is 0 */
    const float *row0, *row1;/* table rows bracketing rho, NULL: edge mode */
    float        wr;         /* weight of row1 */
} pcb_soft;

/* Set up the dab of diameter dia at hardness h (0..1). False when the dab
 * covers nothing (dia <= 0). Any thread. */
bool    pcb_soft_setup(pcb_soft *s, double dia, double hardness);

/* Coverage 0..255 at distance d (px) from the dab center. Any thread. */
uint8_t pcb_soft_eval(const pcb_soft *s, double d);

#endif /* PC_BRUSH_INT_H */
