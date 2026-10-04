/* pc_par.h - parallel-for interface supplied by the caller.
 *
 * The core never creates threads (P-06). Code that can use several cores
 * takes a const pc_par * and calls pc_par_for. The platform layer (pal.h)
 * provides an implementation backed by its worker pool; NULL runs serially
 * on the calling thread. Results must never depend on the thread count.
 */
#ifndef PC_PAR_H
#define PC_PAR_H

#include "pc_base.h"

/* One unit of work. worker is in [0, threads) and identifies the executing
 * thread for the duration of the call, so jobs can use per-worker scratch. */
typedef void (*pc_job_fn)(void *ud, uint32_t index, uint32_t worker);

typedef struct pc_par {
    /* Run fn(ud, i, w) for every i in [0, count) and return when all have
     * finished. The calling thread may participate. */
    void   (*run)(void *self, pc_job_fn fn, void *ud, uint32_t count);
    void    *self;
    uint32_t threads;   /* >= 1, upper bound (exclusive) of worker indices */
} pc_par;

/* Calls par->run, or runs serially with worker 0 when par is NULL. */
void     pc_par_for(const pc_par *par, pc_job_fn fn, void *ud, uint32_t count);
uint32_t pc_par_threads(const pc_par *par);   /* 1 when par is NULL */

#endif /* PC_PAR_H */
