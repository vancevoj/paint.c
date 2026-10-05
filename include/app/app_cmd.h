/* app_cmd.h - commands, keyboard shortcuts and menu contributions.
 *
 * A command is a string id ("file.open", "edit.undo", "image.resize",
 * "layers.merge_down", "adjust.<effect id>", "effects.<effect id>",
 * "view.zoom_in") with a label, an icon, optional enabled / checked
 * predicates and a run callback. Menus, toolbar buttons, panel buttons and
 * keyboard shortcuts all execute commands through app_cmd_exec, which:
 *   1. refuses every command while a modal dialog is open (Paint.NET
 *      disables the main window then; lane W4-MODAL), except commands
 *      flagged APP_CMD_IN_DIALOG,
 *   2. refuses disabled commands (predicate, or APP_CMD_NEEDS_DOC without
 *      an open image),
 *   3. finishes the live tool edit first (TOOLS.md T-FW-FINISH) unless the
 *      command has APP_CMD_NO_COMMIT,
 *   4. runs the callback and requests a frame.
 *
 * Shortcuts: the default keymap (src/app/cmd.c, mirrors
 * docs/inventory/SHORTCUTS.md) binds ids to keys, so a module only
 * registers the command and gets the documented key automatically. A
 * definition may add its own shortcut for ids missing from the keymap.
 * Shortcut strings: "Ctrl+Shift+S", "F5", "Alt+PgUp", "Ctrl+Plus", several
 * separated by ", ". Ctrl means Cmd on macOS (K-OS-1). Key names: A..Z,
 * 0..9, F1..F24, Plus, Minus, Comma, Period, Slash, Semicolon, Quote,
 * LeftBracket ("["), RightBracket ("]"), Backslash, Grave, Space, Tab,
 * Enter, Esc, Backspace, Delete (Del), Insert (Ins), Home, End, PgUp, PgDn,
 * Left, Right, Up, Down.
 *
 * Thread rules: main thread. Ownership: definitions are copied (id, label,
 * shortcut and tooltip strings included); ud is borrowed for the app's
 * lifetime.
 */
#ifndef APP_CMD_H
#define APP_CMD_H

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CMD_NEEDS_DOC  1u   /* disabled when no image is open */
#define APP_CMD_NO_COMMIT  2u   /* does not finish the live tool edit (view, windows) */
#define APP_CMD_REPEAT     4u   /* key auto-repeat runs it again (zoom, nudges) */
#define APP_CMD_RADIO      8u   /* drawn as a radio item when checked != NULL */
#define APP_CMD_IN_TEXT   16u   /* also runs while a text field has focus */
#define APP_CMD_WEAK      32u   /* provisional implementation: a later registration
                                   of the same id replaces it (wave 2a stand-ins
                                   that feature lanes supersede by adding files) */
#define APP_CMD_IN_DIALOG 64u   /* lane W4-MODAL: may run while a modal dialog is
                                   open (touches neither the images nor the
                                   dialog stack's flows) */

typedef struct app_cmd app_cmd;
typedef void (*app_cmd_fn)(app *a, const app_cmd *c);
typedef bool (*app_cmd_pred)(app *a, const app_cmd *c);

/* A command definition (input to app_cmd_register). */
typedef struct app_cmd_def {
    const char   *id;          /* required, unique: [a-z0-9_.-] segments */
    const char   *label;       /* menu / tooltip text, UTF-8 ("Save As...") */
    const char   *shortcut;    /* used only when the keymap has no entry; may be NULL */
    const char   *tip;         /* longer tooltip / status help; may be NULL */
    ui_icon       icon;
    uint32_t      flags;       /* APP_CMD_* */
    app_cmd_pred  enabled;     /* NULL = always (subject to NEEDS_DOC) */
    app_cmd_pred  checked;     /* NULL = not a check item */
    app_cmd_fn    run;         /* required */
    void         *ud;
    intptr_t      arg;
} app_cmd_def;

/* A parsed key binding. key is an SDL_Keycode, mods are UI_MOD_*. */
typedef struct app_key { int32_t key; uint32_t mods; } app_key;
#define APP_CMD_MAX_KEYS 4

/* A registered command (read-only for callers). */
struct app_cmd {
    char         *id, *label, *shortcut, *tip;   /* owned copies; shortcut is the
                                                    effective binding text */
    ui_icon       icon;
    uint32_t      flags;
    app_cmd_pred  enabled, checked;
    app_cmd_fn    run;
    void         *ud;
    intptr_t      arg;
    app_key       keys[APP_CMD_MAX_KEYS];
    int           nkeys;
};

/* Register a command. false for an invalid id or OOM, and for a duplicate
 * id unless the registered one is APP_CMD_WEAK (then it is replaced). A
 * WEAK definition for an id that already exists is ignored (false), so
 * module order does not matter. */
bool           app_cmd_register(app *a, const app_cmd_def *def);
/* Convenience for the common case. */
bool           app_cmd_add(app *a, const char *id, const char *label, ui_icon icon,
                           uint32_t flags, app_cmd_fn run, app_cmd_pred enabled);
const app_cmd *app_cmd_find(const app *a, const char *id);
bool           app_cmd_exists(const app *a, const char *id);
bool           app_cmd_enabled(app *a, const char *id);    /* false for unknown ids */
bool           app_cmd_checked(app *a, const char *id);
/* Execute (see the rules above). false when unknown, disabled or refused
 * because a modal dialog is open. */
bool           app_cmd_exec(app *a, const char *id);
/* Number of commands and the i-th one (registration order). */
int32_t        app_cmd_count(const app *a);
const app_cmd *app_cmd_at(const app *a, int32_t i);

/* Display text of the first binding of id ("Ctrl+Shift+S", "Cmd+S" on
 * macOS), or NULL when it has none. Also works for ids that are not
 * registered yet (menus show their keys dimmed). Borrowed, valid until
 * the next call. */
const char    *app_cmd_shortcut_text(const app *a, const char *id);

/* ---- shortcut parsing (pure, any thread) ----------------------------------- */
/* Parse "Ctrl+Shift+S, Ctrl+F4" into up to max bindings; returns the count
 * (0 on a syntax error). mac selects Ctrl -> Cmd. */
int            app_key_parse(const char *s, bool mac, app_key *out, int max);
/* Display text of one binding into out ("Ctrl+Shift+S"). */
void           app_key_format(app_key k, bool mac, char *out, size_t cap);
/* True when the pressed key (SDL keycode + UI_MOD_* mods) triggers binding
 * b. Plus and Minus accept the main row and the keypad; Plus also accepts
 * "=" with or without Shift (K-NAV-ZOOM-IN). */
bool           app_key_matches(app_key b, int32_t key, uint32_t mods);

/* ---- keyboard dispatch ------------------------------------------------------ */
/* Handle one key press that no widget used. Order: the active tool's key
 * handler, command shortcuts, tool letters (K-TOOLSEL-CYCLE). Letter keys
 * without Ctrl/Alt/Cmd are ignored while a text field has focus. Returns
 * true when something consumed it. */
bool           app_key_press(app *a, int32_t key, uint32_t mods, bool repeat);
/* lane KEYS: the same with the typed character of the press (ui_key_press
 * sym and sym_mods; sym 0 = unknown). Shortcuts on the characters [ ] , . /
 * match the typed character, so they work on layouts that need Shift or
 * AltGr for them (K-OS-3); everything else matches the keycode. While a
 * menu owns the keyboard (ui_menu_keyboard) presses are swallowed, except
 * Alt+H (Help menu) and Alt+T (tool dropdown). Space + arrows pan the
 * view, Home / End pressed again at the edge go to the corner, and arrows
 * no tool or command used nudge the pointer over the canvas. */
bool           app_key_press_ex(app *a, int32_t key, int32_t sym, uint32_t sym_mods,
                                uint32_t mods, bool repeat);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H */
