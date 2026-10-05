/* m_help.h - lane M: the About dialog text (mods/mod_help.c).
 *
 * Thread rules: main thread. Ownership: out is the caller's buffer.
 */
#ifndef M_HELP_H
#define M_HELP_H

#include "app/app.h"

/* Everything the About dialog shows as plain text (name, version and
 * build line, then the NOTICE text line by line). Returns the length
 * written (truncated to cap - 1). */
size_t m_about_text(app *a, char *out, size_t cap);

#endif /* M_HELP_H */
