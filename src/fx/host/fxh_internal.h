/* fxh_internal.h - private helpers shared by the fx host runtime files.
 * Not installed, not part of any ABI. */
#ifndef FXH_INTERNAL_H
#define FXH_INTERNAL_H

#include "fx/fx_run.h"

#include <math.h>
#include <string.h>

/* host->cancelled() implementation, defined in fxh_job.c. */
int fxh_job_cancelled(const void *job);

/* Number of entries of a NULL-terminated choice list, capped at
 * FX_MAX_CHOICES + 1 so callers can detect overlong lists. */
uint32_t fxh_choice_count(const fx_prop *p);

/* Sets one numeric prop from a double with the fx_param_set rules
 * (clamping, rounding). PC_ERR_ARG for NaN or for POINT and CUSTOM. */
pc_status fxh_prop_set(const fx_prop *p, void *params, double v);

/* Raw little helpers: unaligned-safe loads and stores into the blob. */
static inline int32_t fxh_rd_i32(const void *params, uint32_t off)
{
    int32_t v;
    memcpy(&v, (const uint8_t *)params + off, sizeof v);
    return v;
}
static inline uint32_t fxh_rd_u32(const void *params, uint32_t off)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)params + off, sizeof v);
    return v;
}
static inline double fxh_rd_f64(const void *params, uint32_t off)
{
    double v;
    memcpy(&v, (const uint8_t *)params + off, sizeof v);
    return v;
}
static inline void fxh_wr_i32(void *params, uint32_t off, int32_t v)
{
    memcpy((uint8_t *)params + off, &v, sizeof v);
}
static inline void fxh_wr_u32(void *params, uint32_t off, uint32_t v)
{
    memcpy((uint8_t *)params + off, &v, sizeof v);
}
static inline void fxh_wr_f64(void *params, uint32_t off, double v)
{
    memcpy((uint8_t *)params + off, &v, sizeof v);
}

/* Round half away from zero. */
static inline double fxh_round(double v)
{
    return v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5);
}

#endif /* FXH_INTERNAL_H */
