/* m_import.h - lane M: Layers > Import From File (MENUS.md Layers 6,
 * OBSERVED.md 4.2).
 *
 * The files are read and decoded on a worker (color profiles converted to
 * sRGB when the image has none), then one history step "Import From File"
 * deselects, grows the canvas when an image is larger (anchored top left,
 * new area transparent), adds every layer of every file above the active
 * layer named "<file name without extension>:<layer name>" (the new
 * layers share the decoded tiles, nothing is copied), selects the bounds
 * of the last imported layer and activates Move Selected Pixels when that
 * tool is registered. Unreadable files are reported and skipped.
 *
 * Thread rules: main thread. Ownership: paths are copied.
 */
#ifndef M_IMPORT_H
#define M_IMPORT_H

#include "app/app.h"

/* Import paths into the document with id doc_id. Asynchronous: finishes
 * in a later frame (app_tasks_wait completes it). */
void m_import_paths(app *a, uint32_t doc_id, const char *const *paths, int n);

#endif /* M_IMPORT_H */
