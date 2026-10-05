/* text_tool.h - read-only view of the Text tool's editing state (lane C),
 * for tests and status displays.
 *
 * Thread rules: main thread. Ownership: the returned pc_text belongs to the
 * tool and is borrowed until the next call into the app.
 */
#ifndef TEXT_TOOL_H
#define TEXT_TOOL_H

#include "app/app.h"
#include "pc/pc_text.h"

/* The text being edited, or NULL when the Text tool is not editing. */
const pc_text *text_tool_editing(app *a);
/* The IME composition string shown at the caret ("" when none). */
const char    *text_tool_composition(app *a);

#endif /* TEXT_TOOL_H */
