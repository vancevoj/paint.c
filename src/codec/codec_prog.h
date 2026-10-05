/* codec_prog.h - the encoder side of pc_codec_progress (ADR-023). Private
 * to src/codec.
 *
 * An encode is split into phases; each phase covers a share [lo, hi] of the
 * whole and counts its own units (rows, pixels, block rows, chunks). The
 * row sources (lc_flat, pc_flat) add their rows to the current phase, so
 * most encoders only declare phases. Reports are throttled to changes of a
 * thousandth; once the observer cancels, every later call returns
 * PC_ERR_CANCELLED without calling it again.
 *
 * Every function accepts g == NULL (no observer: PC_OK, nothing reported).
 * Threads: one cp_prog belongs to the encoding thread; never call it from
 * pc_par workers (report between parallel batches instead).
 */
#ifndef PC_CODEC_PROG_H
#define PC_CODEC_PROG_H

#include "pc/pc_codec.h"

typedef struct cp_prog {
    const pc_codec_progress *cb;   /* borrowed observer, NULL = silent */
    double   lo, hi;               /* the current phase's share of the encode */
    uint64_t done, total;          /* units of the current phase */
    int32_t  last;                 /* last thousandth reported, -1 = none */
    bool     cancelled;            /* sticky */
} cp_prog;

/* Start an encode: no phase yet (the whole range [0, 1]). cb may be NULL. */
void      cp_init(cp_prog *g, const pc_codec_progress *cb);
/* The following units belong to a phase covering [lo, hi] of the encode
 * (clamped to [0, 1], never below what was reported) with total units
 * (0 = none: only the phase start counts). Reports lo. */
pc_status cp_phase(cp_prog *g, double lo, double hi, uint64_t total);
/* units more of the current phase are done. */
pc_status cp_add(cp_prog *g, uint64_t units);
/* done units of the current phase are finished (absolute). */
pc_status cp_set(cp_prog *g, uint64_t done);
/* The encode is complete: reports 1. */
pc_status cp_finish(cp_prog *g);
/* True once the observer cancelled (no report). */
bool      cp_cancelled(const cp_prog *g);

#endif /* PC_CODEC_PROG_H */
