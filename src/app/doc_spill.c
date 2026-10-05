/* doc_spill.c - lane W3B-FXCORE: per-image history swap files (doc_spill.h). */
#include "doc_spill.h"

#include "pc/pc_hist_spill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWAP_PREFIX "hswap-"
#define SWAP_EXT    ".tmp"

typedef struct swap_file {
    SDL_IOStream *io;
    char          path[1024];
    bool          unlinked;          /* already removed from the directory */
} swap_file;

static bool sw_seek(SDL_IOStream *io, uint64_t off)
{
    if (off > (uint64_t)INT64_MAX) return false;
    return SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) == (Sint64)off;
}

static bool sw_write(void *self, uint64_t off, const void *p, size_t n)
{
    swap_file *f = (swap_file *)self;
    return sw_seek(f->io, off) && SDL_WriteIO(f->io, p, n) == n;
}

static bool sw_read(void *self, uint64_t off, void *p, size_t n)
{
    swap_file *f = (swap_file *)self;
    return sw_seek(f->io, off) && SDL_ReadIO(f->io, p, n) == n;
}

static void sw_close(void *self)
{
    swap_file *f = (swap_file *)self;
    if (!f) return;
    if (f->io) (void)SDL_CloseIO(f->io);
    if (!f->unlinked) (void)pal_remove(f->path);      /* X-23: delete on close */
    free(f);
}

/* Directory for swap files ("" when files are disabled). */
static bool swap_dir(const app *a, char *out, size_t cap)
{
    const char *cfg = a->opts.config_dir;
    out[0] = '\0';
    if (cfg && !*cfg) return false;                    /* no files at all */
    if (cfg) {
        pal_path_join(out, cap, cfg, "state");
    } else {
        const char *st = pal_dir(PAL_DIR_STATE);
        if (!st) return false;
        app_copy_str(out, cap, st);
    }
    if (!pal_is_dir(out) && !pal_mkdirs(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Removes swap files left by crashed sessions (once per process). Files a
 * running instance holds open cannot be removed on Windows and stay. */
static void sweep(const char *dir)
{
    static bool done;
    char **names = NULL;
    int n;
    if (done) return;
    done = true;
    n = pal_list_dir(dir, SWAP_PREFIX "*" SWAP_EXT, &names);
    for (int i = 0; i < n; i++) {
        char p[1024];
        pal_path_join(p, sizeof p, dir, names[i]);
        (void)pal_remove(p);
    }
    pal_free_names(names, n);
}

static swap_file *swap_open(app *a, const app_doc *d)
{
    static uint32_t counter;
    char dir[900];
    swap_file *f;
    if (!swap_dir(a, dir, sizeof dir)) return NULL;
    sweep(dir);
    f = (swap_file *)calloc(1u, sizeof *f);
    if (!f) return NULL;
    for (int tries = 0; tries < 8 && !f->io; tries++) {
        char name[96];
        uint64_t t = pal_ticks_ns();
        snprintf(name, sizeof name, SWAP_PREFIX "%u-%u-%08x%s", (unsigned)d->id,
                 (unsigned)++counter, (unsigned)(t ^ (t >> 32)), SWAP_EXT);
        pal_path_join(f->path, sizeof f->path, dir, name);
        if (pal_file_exists(f->path)) continue;
        f->io = SDL_IOFromFile(f->path, "w+b");
    }
    if (!f->io) {
        free(f);
        return NULL;
    }
#if !defined(_WIN32)
    /* POSIX: the open handle keeps the data; no name, nothing left behind */
    f->unlinked = pal_remove(f->path);
#endif
    return f;
}

static void enable(app *a, app_doc *d)
{
    swap_file *f = swap_open(a, d);
    pc_status st;
    if (f) {
        pc_spill_io io;
        io.write = sw_write;
        io.read = sw_read;
        io.close = sw_close;
        io.self = f;
        st = pc_hist_spill_enable(d->hist, &io, &a->par);    /* owns f from here */
    } else {
        pal_log(PAL_LOG_WARN, "history: no swap file, packing in memory");
        st = pc_hist_spill_enable(d->hist, NULL, &a->par);
    }
    if (st != PC_OK) pal_log(PAL_LOG_WARN, "history: spill store failed: %s", pc_status_str(st));
}

void app_doc_spill_fit(app *a, app_doc *d)
{
    pc_status st;
    if (!a || !d || !d->hist || !a->hist_budget || d->doc->open_txns) return;
    if (!pc_hist_spill_enabled(d->hist)) {
        if (pc_hist_resident_bytes(d->hist) <= a->hist_budget) return;
        enable(a, d);
    }
    st = pc_hist_spill_fit(d->hist, a->hist_budget);
    if (st != PC_OK)
        pal_log(PAL_LOG_WARN, "history: spilling failed (%s); old steps were dropped",
                pc_status_str(st));
}

const char *app_doc_spill_error(const app_doc *d)
{
    pc_hist_spill_stats s;
    if (!d || !d->hist || !pc_hist_spill_enabled(d->hist)) return NULL;
    pc_hist_spill_stats_get(d->hist, &s);
    switch (s.last_error) {
    case PC_ERR_NOMEM: return "not enough memory to bring the step back from the swap file";
    case PC_ERR_FORMAT: return "the history swap file is damaged";
    default: return "the history swap file could not be read";
    }
}
