/* app_settings.h - persistent settings: a flat key/value store saved as a
 * small INI-like UTF-8 text file in PAL_DIR_CONFIG ("settings.ini").
 *
 * File format: one "key=value" per line, keys are [A-Za-z0-9_.-]+, values
 * run to the end of the line (no escapes; values never contain newlines).
 * Lines starting with '#' or ';' and blank lines are ignored; "[section]"
 * lines prefix the following keys with "section." so hand-edited files may
 * use sections. Unknown keys are kept and written back. The parser is
 * hardened for damaged files: at most 1 MiB, 4096 keys, 256-byte keys and
 * 8 KiB values; anything else is skipped.
 *
 * Thread rules: an app_settings object is used from one thread at a time
 * (the main thread). Ownership: strings returned by get are borrowed until
 * the next set or remove of that key; inputs are copied.
 */
#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include "pc/pc_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_settings app_settings;

#define APP_SETTINGS_MAX_FILE   (1u << 20)
#define APP_SETTINGS_MAX_KEYS   4096u
#define APP_SETTINGS_MAX_KEY    256u
#define APP_SETTINGS_MAX_VALUE  8192u

app_settings *app_settings_create(void);                 /* NULL on OOM */
void          app_settings_destroy(app_settings *s);     /* NULL-safe */

/* Replace the content with the parsed text (n bytes). Returns the number
 * of entries accepted. */
size_t        app_settings_parse(app_settings *s, const char *text, size_t n);
/* Serialize (sorted by key). *out is malloc'ed and NUL-terminated (free()),
 * *len excludes the NUL. PC_ERR_NOMEM. */
pc_status     app_settings_serialize(const app_settings *s, char **out, size_t *len);
/* File helpers over pal (atomic save). A missing file is not an error. */
pc_status     app_settings_load(app_settings *s, const char *path);
pc_status     app_settings_save(const app_settings *s, const char *path);

const char   *app_settings_get(const app_settings *s, const char *key);   /* NULL if absent */
bool          app_settings_set(app_settings *s, const char *key, const char *value);
bool          app_settings_remove(app_settings *s, const char *key);
size_t        app_settings_count(const app_settings *s);
/* i-th entry in key order (both borrowed). */
bool          app_settings_at(const app_settings *s, size_t i, const char **key,
                              const char **value);
bool          app_settings_dirty(const app_settings *s);   /* changed since parse/save */

/* Typed helpers; malformed or missing values give def. */
int64_t       app_settings_int(const app_settings *s, const char *key, int64_t def);
double        app_settings_double(const app_settings *s, const char *key, double def);
bool          app_settings_bool(const app_settings *s, const char *key, bool def);
bool          app_settings_set_int(app_settings *s, const char *key, int64_t v);
bool          app_settings_set_double(app_settings *s, const char *key, double v);
bool          app_settings_set_bool(app_settings *s, const char *key, bool v);

/* The app's store (borrowed). */
struct app;
app_settings *app_settings_of(struct app *a);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H */
