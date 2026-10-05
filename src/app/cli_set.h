/* cli_set.h - lane UIB (wave 4 item 9): `paintc --set KEY=VALUE` while
 * another paintc already runs with the same settings folder.
 *
 * The launch that will forward to the running instance first writes its
 * overrides to a pending file in the settings folder
 * ("settings.forward.<random>.ini", one per launch, so concurrent launches
 * never lose each other's values), then forwards as usual. The running
 * instance takes every pending file when a forwarded message arrives (and
 * a new primary takes leftovers at startup), applies the values to its
 * settings store, reloads the state the running app keeps outside the
 * store (theme, view toggles, units, palette colors, tool settings, panel
 * layout, recent files, window geometry, Settings dialog preferences) and
 * saves the settings file at once. Without that reload the exit would
 * write the old live state over the override.
 *
 * Instance identity: one single instance per settings folder. A launch
 * with another --config-dir is another profile with its own settings and
 * autosave state, so it gets its own instance id and never forwards into a
 * window that uses a different folder.
 *
 * Thread rules: app_cli_apply_settings runs on the main thread; the other
 * functions touch no app state and may run on any thread. Ownership: every
 * argument is borrowed; nothing is retained after a call returns.
 */
#ifndef APP_CLI_SET_H
#define APP_CLI_SET_H

#include <stdbool.h>
#include <stddef.h>

#include "app/app.h"
#include "app/app_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Glob of the pending files inside the settings folder. */
#define APP_CLI_PENDING_GLOB "settings.forward.*.ini"

/* Write sets[0..n) ("KEY=VALUE", already validated by the caller) as a new
 * pending file in dir (created when missing). false when n <= 0 is not
 * the case and the file cannot be written; the reason is logged. */
bool app_cli_queue_settings(const char *dir, const char *const *sets, int n);

/* Move every pending file of dir into out (files in name order, later
 * values win) and delete them. Returns the number of files taken. Damaged
 * or oversized files are deleted and skipped. */
int  app_cli_take_pending(const char *dir, app_settings *out);

/* Apply every key of overrides to the running app a (see above) and save
 * the settings file when the app has one. Returns the number of keys. */
int  app_cli_apply_settings(app *a, const app_settings *overrides);

/* The running app's settings folder (its --config-dir, else the per-user
 * folder), or NULL when it has none. Borrowed until pal_quit or app_destroy. */
const char *app_cli_config_dir(const app *a);

/* The single instance id for a settings folder: APP_ID for the per-user
 * folder (config_dir NULL or empty or the same folder), otherwise APP_ID
 * plus a hash of the absolute folder path. out receives at most cap bytes
 * (64 are always enough). */
void app_cli_instance_id(const char *config_dir, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLI_SET_H */
