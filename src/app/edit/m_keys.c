/* m_keys.c - lane KEYS: small pure helpers for keyboard modifiers in
 * dialogs (declarations in the headers named per function).
 *
 * Thread rules: pure functions, any thread. */
#include "m_rotzoom.h"

#include <math.h>

double m_rz_roll_from_drag(double dx, double dy, bool snap)
{
    double r = atan2(dy, dx) * 180.0 / 3.14159265358979323846;
    if (!isfinite(r)) r = 0.0;
    if (snap) r = floor(r / 15.0 + 0.5) * 15.0;
    if (r > 180.0) r -= 360.0;
    if (r < -180.0) r += 360.0;
    return floor(r * 100.0 + 0.5) / 100.0;
}
