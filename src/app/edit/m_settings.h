/* m_settings.h - lane M: the Settings dialog (mods/mod_m_settings.c) and
 * the preferences it owns. See that file for the pages and keys.
 *
 * Thread rules: main thread. Ownership: strings are copied; out buffers
 * are the caller's.
 */
#ifndef M_SETTINGS_H
#define M_SETTINGS_H

#include "app/app.h"
#include "app/app_tool.h"

/* Open the Settings dialog on page (0 User Interface ... 7 Diagnostics). */
void   m_settings_open(app *a, int page);
/* The page the open Settings dialog shows, -1 when it is closed (lane
 * SHELL; Effects > Plugin Errors opens page 6). */
int    m_settings_page(const app *a);

/* Read the preferences from the settings store into the app (canvas
 * shadow, border color, checkerboard brightness, pen input, history
 * memory limit) and apply them. */
void   m_settings_apply(app *a);

/* OD-10: a quarter of the RAM, at least 1 GiB (1 GiB cap on 32-bit). */
size_t m_settings_auto_history_budget(void);

/* Tool defaults (Settings > Tools): the factory values overlaid with the
 * stored tooldef.* keys; set stores every field (and marks that defaults
 * exist, so they apply at the next start); the default tool id
 * ("paintbrush" when unset or unknown); reset stores the factory values;
 * load_from_toolbar copies the current toolbar and tool. */
void        m_tooldef_get(app *a, app_tool_settings *out);
void        m_tooldef_set(app *a, const app_tool_settings *v);
const char *m_tooldef_tool(app *a);
void        m_tooldef_reset(app *a);
void        m_tooldef_load_from_toolbar(app *a);

/* Settings > Plugin Errors: the list a plugin loader reports (copied;
 * n <= 0 clears it). false on OOM. */
bool   m_settings_set_plugin_errors(app *a, const char *const *files,
                                    const char *const *details, int n);

/* Folder paths: which 0 = plugins (PAL_DIR_DATA/plugins), 1 = crash logs
 * (PAL_DIR_STATE/crash, or <config dir>/crash when the app runs with a
 * private --config-dir; lane SHELL). */
void   m_settings_folder(app *a, int which, char *out, size_t cap);

/* The Diagnostics page text (one item per line). */
void   m_settings_diagnostics(app *a, char *out, size_t cap);

#endif /* M_SETTINGS_H */
