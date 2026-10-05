/* mod_keys.c - lane KEYS: the diagnostic cleanup shortcut (K-UI-DIAG,
 * Ctrl+Alt+Shift+~ in the keymap of cmd.c).
 *
 * Paint.NET binds Ctrl+Alt+Shift+~ to a hidden cleanup (garbage collection
 * and a GPU cache dump). paint.c has no garbage collector; the closest
 * equivalent drops what is only a cache and is rebuilt on demand: the
 * display caches of the images not shown, every page texture of the
 * canvas (re-uploaded from the CPU caches, P-03) and the image and layer
 * thumbnails. It then logs the memory figures (history and display cache
 * bytes before and after) through pal_log. No document, history or
 * setting changes.
 *
 * Thread rules: main thread. Ownership: no state is kept. */
#include "../app_internal.h"

#include <string.h>

typedef struct diag_mem {
    size_t history, caches;
    int    tiles;
} diag_mem;

static void measure(app *a, diag_mem *m)
{
    memset(m, 0, sizeof *m);
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *d = app_doc_at(a, i);
        pc_view_stats st;
        m->history += pc_hist_bytes(d->hist);
        if (!d->vcache) continue;
        pc_view_cache_stats(d->vcache, &st);
        m->caches += st.bytes;
        m->tiles += (int)st.entries;
    }
}

static void cmd_diag_cleanup(app *a, const app_cmd *c)
{
    app_doc *active = app_active_doc(a);
    diag_mem before, after;
    (void)c;
    measure(a, &before);
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *d = app_doc_at(a, i);
        if (d != active && d->vcache) pc_view_cache_clear(d->vcache);
        app_thumbs_free(d);
    }
    if (a->cv.gfx) gfx_canvas_reset(a->cv.gfx);
    measure(a, &after);
    pal_log(PAL_LOG_INFO,
            "diagnostic cleanup: %d images, history %.1f MiB, display caches %.1f MiB "
            "(%d tiles) -> %.1f MiB (%d tiles), canvas textures and thumbnails dropped",
            (int)app_doc_count(a), (double)before.history / 1048576.0,
            (double)before.caches / 1048576.0, before.tiles, (double)after.caches / 1048576.0,
            after.tiles);
    app_request_frame(a);
}

void mod_keys(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "app.diag_cleanup";
    d.label = "Diagnostic Cleanup";
    d.flags = APP_CMD_NO_COMMIT;
    d.run = cmd_diag_cleanup;
    d.tip = "Drop display caches and canvas textures that are rebuilt on demand, then log "
            "the memory use";
    (void)app_cmd_register(a, &d);
}
