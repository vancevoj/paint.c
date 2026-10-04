/* fx_builtin.h - registration of the effects compiled into paint.c.
 * Same calling convention as a plugin's fx_entry. Main thread. */
#ifndef FX_BUILTIN_H
#define FX_BUILTIN_H

#include "fx_abi.h"

/* Calls every fxm_* module; returns the number of effects registered. */
int fx_builtin_register(const fx_host *host, int (*reg)(const fx_effect *fx));

#endif /* FX_BUILTIN_H */
