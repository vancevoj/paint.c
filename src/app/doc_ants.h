/* doc_ants.h - the marching ants outline of an open image (lane TOOLS,
 * wave 4 item 28), behind the app_doc_ants* functions of app_doc.h.
 *
 * Each document keeps its displayed outline as a prepared gfx_ants: the
 * selection's own outline (rebuilt when sel_gen changes) or a tool's
 * preview. Simple selections are traced at once on the main thread, as
 * before. Selections with more partially selected tiles than the sync
 * limit are traced on a worker (app_task) from a snapshot of the selection
 * tiles (immutable, retained), so a global Magic Wand on a noisy 4K image
 * no longer freezes the window for seconds: the frame shows no ants (and
 * app_doc_ants_pending is true) until the result lands. A newer selection
 * cancels the running trace; a finished trace is used only when the
 * selection is still the one it was made from. Outlines with many points
 * get the coarse occupancy levels for drawing zoomed out.
 *
 * Thread rules: main thread, except the job's work function. Ownership:
 * the document owns its app_doc_ants_rt (created and destroyed with it); a
 * running job is detached when its document goes away and freed by its
 * done callback.
 */
#ifndef DOC_ANTS_H
#define DOC_ANTS_H

#include "app/app_doc.h"
#include "gfx.h"

typedef struct app_doc_ants_rt app_doc_ants_rt;

/* NULL on OOM. a is borrowed (the app outlives its documents). */
app_doc_ants_rt *app_doc_ants_rt_create(app *a, app_doc *d);
void             app_doc_ants_rt_free(app_doc_ants_rt *rt);    /* NULL-safe */

/* Draw d's outline for view v (the canvas draw callback): visible chunks
 * only, a screen raster or the occupancy levels for complex outlines
 * (gfx_ants_draw with the app's drawing cache). */
void app_doc_ants_draw(app *a, app_doc *d, const gfx_view *v, double phase, double dash,
                       pc_rect clip);
/* What the last app_doc_ants_draw did (tests, diagnostics); false before
 * the first draw. */
bool app_doc_ants_last_draw(app *a, gfx_ants_info *out);

/* Tests: selections with at most n partially selected tiles are traced
 * synchronously (default DOC_ANTS_SYNC_TILES; UINT32_MAX always, 0 never). */
#define DOC_ANTS_SYNC_TILES 48u
void app_doc_ants_set_sync_tiles(uint32_t n);

#endif /* DOC_ANTS_H */
