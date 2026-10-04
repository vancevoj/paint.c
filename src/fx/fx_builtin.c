/* fx_builtin.c - registers every built-in effect module (generated list). */
#include "fx/fx_builtin.h"

#define FX_MODULE(name) int name(const fx_host *host, int (*reg)(const fx_effect *fx));
#include "fx_module_list.inc"
#undef FX_MODULE

int fx_builtin_register(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    int total = 0, r = 0;
    (void)r; (void)host; (void)reg;
#define FX_MODULE(name) r = name(host, reg); if (r > 0) total += r;
#include "fx_module_list.inc"
#undef FX_MODULE
    return total;
}
