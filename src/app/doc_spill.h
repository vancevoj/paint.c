/* doc_spill.h - lane W3B-FXCORE: history beyond the RAM budget for open
 * images (PARITY F-CORE-HIST-BUDGET, T-L1-08, X-23). When an image's history
 * passes the budget (a->hist_budget, OD-10), its history-only tiles are
 * packed into a per-image swap file instead of old steps being dropped
 * (pc_hist_spill.h).
 *
 * The swap file lives in the per-user state directory (PAL_DIR_STATE,
 * created 0700), or in <config dir>/state with a private --config-dir; an
 * empty config dir (no files, as in tests) packs into memory instead. On
 * POSIX the file is unlinked right after it is opened, so it disappears with
 * the process even after a crash; on Windows it is removed when the image
 * closes, and leftovers of a crashed session are swept at the next start
 * (files still open by another instance cannot be removed and are skipped).
 *
 * Thread rules: main thread.
 */
#ifndef DOC_SPILL_H
#define DOC_SPILL_H

#include "app_internal.h"

/* Keeps d's history within a->hist_budget: spills when over it (creating
 * the store on first need), prunes only when spilling cannot help. Called
 * by app_doc_history_changed. No-op without a budget or while an edit is
 * open. */
void app_doc_spill_fit(app *a, app_doc *d);

/* The message for an undo/redo/jump that failed because spilled history
 * could not be read back (NULL when the history has no store). */
const char *app_doc_spill_error(const app_doc *d);

#endif /* DOC_SPILL_H */
