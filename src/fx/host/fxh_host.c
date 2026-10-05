/* fxh_host.c - the fx_host services table and the log hook. */
#include "fxh_internal.h"

#include <stdlib.h>

static fx_log_fn g_log_fn = NULL;
static void     *g_log_ud = NULL;

static void *h_alloc(size_t n)
{
    return malloc(n ? n : 1u);
}

static void h_free(void *p)
{
    free(p);
}

static int h_cancelled(const void *job)
{
    return job ? fxh_job_cancelled(job) : 0;
}

static void h_log(int level, const char *utf8)
{
    fx_run_log(level, utf8);
}

static const fx_host g_host = {
    FX_ABI_VERSION, (uint32_t)sizeof(fx_host), h_alloc, h_free, h_cancelled, h_log
};

const fx_host *fx_run_host(void)
{
    return &g_host;
}

void fx_run_set_log(fx_log_fn fn, void *ud)
{
    g_log_fn = fn;
    g_log_ud = ud;
}

void fx_run_log(int level, const char *utf8)
{
    fx_log_fn fn = g_log_fn;
    if (fn && utf8) fn(g_log_ud, level, utf8);
}
