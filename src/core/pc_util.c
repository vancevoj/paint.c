/* pc_util.c - status strings and the serial parallel-for fallback. */
#include "pc/pc_par.h"

const char *pc_status_str(pc_status s)
{
    switch (s) {
    case PC_OK:              return "OK";
    case PC_ERR_NOMEM:       return "Out of memory";
    case PC_ERR_ARG:         return "Invalid argument";
    case PC_ERR_STATE:       return "Operation not allowed right now";
    case PC_ERR_LIMIT:       return "Size limit exceeded";
    case PC_ERR_FORMAT:      return "The file is damaged or not in the expected format";
    case PC_ERR_UNSUPPORTED: return "This file uses a feature that is not supported";
    case PC_ERR_IO:          return "Could not read or write the file";
    case PC_ERR_CANCELLED:   return "Cancelled";
    }
    return "Unknown error";
}

void pc_par_for(const pc_par *par, pc_job_fn fn, void *ud, uint32_t count)
{
    if (count == 0u) return;
    if (par && par->run && par->threads > 1u && count > 1u) {
        par->run(par->self, fn, ud, count);
        return;
    }
    for (uint32_t i = 0; i < count; i++) fn(ud, i, 0u);
}

uint32_t pc_par_threads(const pc_par *par)
{
    return (par && par->run && par->threads > 1u) ? par->threads : 1u;
}
