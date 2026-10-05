/* cmd.c - command registry, the default keymap (docs/inventory/SHORTCUTS.md)
 * and keyboard dispatch (see app_cmd.h). */
#include "app_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- default keymap ---------------------------------------------------------- */
typedef struct keymap_entry { const char *id, *keys; } keymap_entry;

/* Command id -> bindings. Commands registered later pick these up, so the
 * documented keys work as soon as a lane registers the command. */
static const keymap_entry k_keymap[] = {
    /* File (K-FILE-*) */
    { "file.new", "Ctrl+N" },
    { "file.open", "Ctrl+O" },
    { "file.close", "Ctrl+W, Ctrl+F4" },
    { "file.save", "Ctrl+S" },
    { "file.save_as", "Ctrl+Shift+S" },
    { "file.save_all", "Ctrl+Alt+S" },
    { "file.print", "Ctrl+P" },
    { "file.exit", "Alt+F4" },
    /* Edit (K-EDIT-*) */
    { "edit.undo", "Ctrl+Z" },
    { "edit.redo", "Ctrl+Y" },
    { "edit.cut", "Ctrl+X, Shift+Delete" },
    { "edit.copy", "Ctrl+C, Ctrl+Insert" },
    { "edit.copy_merged", "Ctrl+Shift+C" },
    { "edit.paste", "Ctrl+V, Shift+Insert" },
    { "edit.paste_layer", "Ctrl+Shift+V" },
    { "edit.paste_image", "Ctrl+Alt+V" },
    { "edit.copy_selection", "Ctrl+Alt+Shift+C" },
    { "edit.paste_selection.replace", "Ctrl+Alt+Shift+V" },
    { "edit.erase_selection", "Delete" },
    { "edit.fill_selection", "Backspace" },
    { "edit.fill_selection_secondary", "Shift+Backspace" },
    { "edit.invert_selection", "Ctrl+I" },
    { "edit.select_all", "Ctrl+A" },
    { "edit.deselect", "Ctrl+D" },
    /* View (K-VIEW-*, K-NAV-*) */
    { "view.zoom_in", "Ctrl+Plus" },
    { "view.zoom_out", "Ctrl+Minus" },
    { "view.zoom_window", "Ctrl+B" },
    { "view.zoom_selection", "Ctrl+Shift+B" },
    { "view.actual_size", "Ctrl+0, Ctrl+Shift+A, Ctrl+Alt+0" },
    { "view.scroll_up", "PgUp" },
    { "view.scroll_down", "PgDn" },
    { "view.scroll_left", "Shift+PgUp" },
    { "view.scroll_right", "Shift+PgDn" },
    { "view.home", "Home" },
    { "view.end", "End" },
    { "view.home_top_left", "Shift+Home" },
    { "view.end_bottom_right", "Shift+End" },
    { "view.center_top_left", "Ctrl+Home" },
    { "view.center_bottom_right", "Ctrl+End" },
    /* Image (K-IMAGE-*) */
    { "image.crop_to_selection", "Ctrl+Shift+X" },
    { "image.resize", "Ctrl+R" },
    { "image.canvas_size", "Ctrl+Shift+R" },
    { "image.rotate_cw", "Ctrl+H" },
    { "image.rotate_ccw", "Ctrl+G" },
    { "image.flatten", "Ctrl+Shift+F" },
    /* Layers (K-LAYER-*) */
    { "layers.add_new", "Ctrl+Shift+N" },
    { "layers.delete", "Ctrl+Shift+Delete" },
    { "layers.duplicate", "Ctrl+Shift+D" },
    { "layers.merge_down", "Ctrl+M" },
    { "layers.toggle_visibility", "Ctrl+Comma" },
    { "layers.rotate_zoom", "Ctrl+Shift+Z" },
    { "layers.go_top", "Ctrl+Alt+PgUp" },
    { "layers.go_up", "Alt+PgUp" },
    { "layers.go_down", "Alt+PgDn" },
    { "layers.go_bottom", "Ctrl+Alt+PgDn" },
    { "layers.properties", "F4" },
    /* Adjustments (K-ADJ-*) */
    { "adjust.org.paintc.adjust.auto_level", "Ctrl+Shift+L" },
    { "adjust.org.paintc.adjust.black_and_white", "Ctrl+Shift+G" },
    { "adjust.org.paintc.adjust.brightness_contrast", "Ctrl+Shift+T" },
    { "adjust.org.paintc.adjust.curves", "Ctrl+Shift+M" },
    { "adjust.org.paintc.adjust.hue_saturation", "Ctrl+Shift+U" },
    { "adjust.org.paintc.adjust.invert_alpha", "Ctrl+Alt+I" },
    { "adjust.org.paintc.adjust.invert_colors", "Ctrl+Shift+I" },
    { "adjust.org.paintc.adjust.levels", "Ctrl+L" },
    { "adjust.org.paintc.adjust.posterize", "Ctrl+Shift+P" },
    { "adjust.org.paintc.adjust.sepia", "Ctrl+Shift+E" },
    /* Effects (K-FX-*) */
    { "effects.repeat", "Ctrl+F" },
    /* Windows and app (K-UI-*) */
    { "window.tools", "F5" },
    { "window.history", "F6" },
    { "window.layers", "F7" },
    { "window.colors", "F8" },
    { "window.reset.tools", "Ctrl+Shift+F5" },
    { "window.reset.history", "Ctrl+Shift+F6" },
    { "window.reset.layers", "Ctrl+Shift+F7" },
    { "window.reset.colors", "Ctrl+Shift+F8" },
    { "app.settings", "Alt+X" },
    { "help.docs", "F1" },
    { "help.search", "Ctrl+E" },
    { "app.diag_cleanup", "Ctrl+Alt+Shift+Grave" },      /* K-UI-DIAG (lane KEYS) */
    /* Image list (K-IMG-*) */
    { "docs.next", "Ctrl+Tab, Ctrl+PgDn" },
    { "docs.prev", "Ctrl+Shift+Tab, Ctrl+PgUp" },
    { "docs.move_left", "Ctrl+Shift+PgUp" },
    { "docs.move_right", "Ctrl+Shift+PgDn" },
    { "docs.1", "Ctrl+1, Alt+1" }, { "docs.2", "Ctrl+2, Alt+2" }, { "docs.3", "Ctrl+3, Alt+3" },
    { "docs.4", "Ctrl+4, Alt+4" }, { "docs.5", "Ctrl+5, Alt+5" }, { "docs.6", "Ctrl+6, Alt+6" },
    { "docs.7", "Ctrl+7, Alt+7" }, { "docs.8", "Ctrl+8, Alt+8" }, { "docs.9", "Ctrl+9, Alt+9" },
    /* Colors and toolbar (K-COL-*, K-TB-*) */
    { "colors.swap", "X" },
    { "colors.toggle_slot", "C" },
    { "tool.width_dec", "LeftBracket" },
    { "tool.width_inc", "RightBracket" },
    { "tool.width_dec5", "Ctrl+LeftBracket" },
    { "tool.width_inc5", "Ctrl+RightBracket" },
};

const char *app_keymap_lookup(const char *id)
{
    for (size_t i = 0; i < sizeof k_keymap / sizeof k_keymap[0]; i++)
        if (strcmp(k_keymap[i].id, id) == 0) return k_keymap[i].keys;
    return NULL;
}

static bool is_mac(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

/* ---- key names -------------------------------------------------------------------- */
typedef struct key_name { const char *name; int32_t key; const char *disp; } key_name;

static const key_name k_names[] = {
    { "Plus", SDLK_PLUS, "+" },          { "Minus", SDLK_MINUS, "-" },
    { "Comma", SDLK_COMMA, "," },        { "Period", SDLK_PERIOD, "." },
    { "Slash", SDLK_SLASH, "/" },        { "Semicolon", SDLK_SEMICOLON, ";" },
    { "Quote", SDLK_APOSTROPHE, "'" },   { "LeftBracket", SDLK_LEFTBRACKET, "[" },
    { "RightBracket", SDLK_RIGHTBRACKET, "]" }, { "Backslash", SDLK_BACKSLASH, "\\" },
    { "Grave", SDLK_GRAVE, "`" },        { "Space", SDLK_SPACE, "Space" },
    { "Tab", SDLK_TAB, "Tab" },          { "Enter", SDLK_RETURN, "Enter" },
    { "Esc", SDLK_ESCAPE, "Esc" },       { "Backspace", SDLK_BACKSPACE, "Backspace" },
    { "Delete", SDLK_DELETE, "Del" },    { "Del", SDLK_DELETE, "Del" },
    { "Insert", SDLK_INSERT, "Ins" },    { "Ins", SDLK_INSERT, "Ins" },
    { "Home", SDLK_HOME, "Home" },       { "End", SDLK_END, "End" },
    { "PgUp", SDLK_PAGEUP, "PgUp" },     { "PgDn", SDLK_PAGEDOWN, "PgDn" },
    { "Left", SDLK_LEFT, "Left" },       { "Right", SDLK_RIGHT, "Right" },
    { "Up", SDLK_UP, "Up" },             { "Down", SDLK_DOWN, "Down" },
};

static bool ieq(const char *a, size_t n, const char *b)
{
    size_t i = 0;
    for (; i < n && b[i]; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return i == n && b[i] == '\0';
}

static int32_t parse_key(const char *s, size_t n)
{
    if (n == 1u) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return (int32_t)c;
        if (c >= 'A' && c <= 'Z') return (int32_t)(c - 'A' + 'a');
        if (c >= '0' && c <= '9') return (int32_t)c;
        if (c == '+') return SDLK_PLUS;
        if (c == '-') return SDLK_MINUS;
        if (c == ',') return SDLK_COMMA;
        if (c == '.') return SDLK_PERIOD;
        if (c == '/') return SDLK_SLASH;
        if (c == '[') return SDLK_LEFTBRACKET;
        if (c == ']') return SDLK_RIGHTBRACKET;
        if (c == '\'') return SDLK_APOSTROPHE;
    }
    if (n >= 2u && n <= 3u && (s[0] == 'F' || s[0] == 'f')) {
        int v = 0;
        for (size_t i = 1; i < n; i++) {
            if (s[i] < '0' || s[i] > '9') { v = -1; break; }
            v = v * 10 + (s[i] - '0');
        }
        if (v >= 1 && v <= 12) return (int32_t)(SDLK_F1 + (SDL_Keycode)(v - 1));
        if (v >= 13 && v <= 24) return (int32_t)(SDLK_F13 + (SDL_Keycode)(v - 13));
    }
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (ieq(s, n, k_names[i].name)) return k_names[i].key;
    return 0;
}

int app_key_parse(const char *s, bool mac, app_key *out, int max)
{
    int count = 0;
    const char *p = s;
    if (!s) return 0;
    while (*p && count < max) {
        uint32_t mods = 0;
        int32_t key = 0;
        const char *end;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        end = p;
        while (*end && *end != ',') end++;
        /* a binding "Ctrl+," would split at the comma: a comma right after
         * '+' belongs to the binding */
        while (*end == ',' && end > p && end[-1] == '+') {
            end++;
            while (*end && *end != ',') end++;
        }
        {
            const char *q = p;
            while (q < end) {
                const char *t = q;
                size_t n;
                while (t < end && *t != '+') t++;
                if (t == q && t < end) t++;          /* "+" as a key ("Ctrl++") */
                n = (size_t)(t - q);
                while (n > 0u && q[n - 1u] == ' ') n--;
                if (t >= end) {
                    key = parse_key(q, n);
                } else if (ieq(q, n, "Ctrl")) {
                    mods |= mac ? UI_MOD_GUI : UI_MOD_CTRL;
                } else if (ieq(q, n, "Shift")) {
                    mods |= UI_MOD_SHIFT;
                } else if (ieq(q, n, "Alt") || ieq(q, n, "Option")) {
                    mods |= UI_MOD_ALT;
                } else if (ieq(q, n, "Cmd") || ieq(q, n, "Win") || ieq(q, n, "Super")) {
                    mods |= UI_MOD_GUI;
                } else if (ieq(q, n, "RealCtrl")) {
                    mods |= UI_MOD_CTRL;             /* Ctrl even on macOS */
                } else {
                    key = parse_key(q, n);
                    if (!key) return 0;
                }
                q = t < end ? t + 1 : t;
            }
        }
        if (!key) return 0;
        out[count].key = key;
        out[count].mods = mods;
        count++;
        p = end;
    }
    return count;
}

void app_key_format(app_key k, bool mac, char *out, size_t cap)
{
    char name[16];
    const char *kn = NULL;
    if (!out || cap == 0u) return;
    if (k.key >= 'a' && k.key <= 'z') {
        name[0] = (char)(k.key - 'a' + 'A');
        name[1] = '\0';
        kn = name;
    } else if (k.key >= '0' && k.key <= '9') {
        name[0] = (char)k.key;
        name[1] = '\0';
        kn = name;
    } else if (k.key >= (int32_t)SDLK_F1 && k.key <= (int32_t)SDLK_F12) {
        snprintf(name, sizeof name, "F%d", (int)(k.key - (int32_t)SDLK_F1) + 1);
        kn = name;
    } else if (k.key >= (int32_t)SDLK_F13 && k.key <= (int32_t)SDLK_F24) {
        snprintf(name, sizeof name, "F%d", (int)(k.key - (int32_t)SDLK_F13) + 13);
        kn = name;
    } else {
        for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
            if (k_names[i].key == k.key) { kn = k_names[i].disp; break; }
    }
    snprintf(out, cap, "%s%s%s%s%s", (k.mods & UI_MOD_CTRL) ? "Ctrl+" : "",
             (k.mods & UI_MOD_GUI) ? (mac ? "Cmd+" : "Win+") : "",
             (k.mods & UI_MOD_ALT) ? (mac ? "Option+" : "Alt+") : "",
             (k.mods & UI_MOD_SHIFT) ? "Shift+" : "", kn ? kn : "?");
}

bool app_key_matches(app_key b, int32_t key, uint32_t mods)
{
    uint32_t m = mods & (UI_MOD_CTRL | UI_MOD_SHIFT | UI_MOD_ALT | UI_MOD_GUI);
    if (b.key == SDLK_PLUS) {
        /* main-row "=" (shifted or not), the "+" keycode and the keypad */
        if (key != SDLK_PLUS && key != SDLK_EQUALS && key != SDLK_KP_PLUS) return false;
        return (m & ~UI_MOD_SHIFT) == (b.mods & ~UI_MOD_SHIFT);
    }
    if (b.key == SDLK_MINUS) {
        if (key != SDLK_MINUS && key != SDLK_KP_MINUS) return false;
        return m == b.mods;
    }
    if (b.key == SDLK_RETURN && key == SDLK_KP_ENTER) return m == b.mods;
    return key == b.key && m == b.mods;
}

/* ---- registry ----------------------------------------------------------------------- */
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static bool valid_id(const char *id)
{
    size_t n = 0;
    if (!id || !*id) return false;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return false;
    }
    return n < 256u;
}

static int32_t find_index(const app *a, const char *id)
{
    uint32_t h, mask;
    if (!a->cmd_map_cap || !id) return -1;
    mask = a->cmd_map_cap - 1u;
    h = hash_str(id) & mask;
    while (a->cmd_map[h]) {
        int32_t i = a->cmd_map[h] - 1;
        if (strcmp(a->cmds[i]->id, id) == 0) return i;
        h = (h + 1u) & mask;
    }
    return -1;
}

static bool map_insert(app *a, int32_t index)
{
    uint32_t mask, h;
    if ((uint32_t)(a->ncmds + 1) * 2u > a->cmd_map_cap) {
        uint32_t nc = a->cmd_map_cap ? a->cmd_map_cap * 2u : 256u;
        int32_t *nm = (int32_t *)calloc(nc, sizeof *nm);
        if (!nm) return false;
        free(a->cmd_map);
        a->cmd_map = nm;
        a->cmd_map_cap = nc;
        for (int32_t i = 0; i < a->ncmds; i++) {
            if (i == index) continue;
            h = hash_str(a->cmds[i]->id) & (nc - 1u);
            while (a->cmd_map[h]) h = (h + 1u) & (nc - 1u);
            a->cmd_map[h] = i + 1;
        }
    }
    mask = a->cmd_map_cap - 1u;
    h = hash_str(a->cmds[index]->id) & mask;
    while (a->cmd_map[h]) h = (h + 1u) & mask;
    a->cmd_map[h] = index + 1;
    return true;
}

static void free_cmd(app_cmd *c)
{
    if (!c) return;
    free(c->id);
    free(c->label);
    free(c->shortcut);
    free(c->tip);
    free(c);
}

static app_cmd *make_cmd(const app_cmd_def *def)
{
    app_cmd *c = (app_cmd *)calloc(1u, sizeof *c);
    const char *keys;
    if (!c) return NULL;
    keys = app_keymap_lookup(def->id);
    if (!keys) keys = def->shortcut;
    c->id = app_strdup(def->id);
    c->label = app_strdup(def->label ? def->label : def->id);
    c->shortcut = keys ? app_strdup(keys) : NULL;
    c->tip = def->tip ? app_strdup(def->tip) : NULL;
    if (!c->id || !c->label || (keys && !c->shortcut) || (def->tip && !c->tip)) {
        free_cmd(c);
        return NULL;
    }
    c->icon = def->icon;
    c->flags = def->flags;
    c->enabled = def->enabled;
    c->checked = def->checked;
    c->run = def->run;
    c->ud = def->ud;
    c->arg = def->arg;
    if (keys) {
        c->nkeys = app_key_parse(keys, is_mac(), c->keys, APP_CMD_MAX_KEYS);
        if (c->nkeys == 0) pal_log(PAL_LOG_WARN, "command %s: bad shortcut '%s'", c->id, keys);
    }
    return c;
}

/* A provisional (APP_CMD_WEAK) command is superseded in place: same index,
 * so the map stays valid. */
static bool replace_cmd(app *a, int32_t index, const app_cmd_def *def)
{
    app_cmd *c = make_cmd(def);
    if (!c) return false;
    free_cmd(a->cmds[index]);
    a->cmds[index] = c;
    pal_log(PAL_LOG_INFO, "command %s: provisional version replaced", c->id);
    return true;
}

bool app_cmd_register(app *a, const app_cmd_def *def)
{
    app_cmd *c;
    if (!a || !def || !def->run || !valid_id(def->id)) {
        pal_log(PAL_LOG_WARN, "command rejected: invalid definition (%s)",
                def && def->id ? def->id : "(null)");
        return false;
    }
    {
        int32_t old = find_index(a, def->id);
        if (old >= 0 && (def->flags & APP_CMD_WEAK)) return false;  /* the real one exists */
        if (old >= 0 && !(a->cmds[old]->flags & APP_CMD_WEAK)) {
            pal_log(PAL_LOG_WARN, "command rejected: duplicate id %s", def->id);
            return false;
        }
        if (old >= 0) return replace_cmd(a, old, def);
    }
    if (a->ncmds == a->cap_cmds) {
        int32_t nc = a->cap_cmds ? a->cap_cmds * 2 : 128;
        app_cmd **na = (app_cmd **)realloc(a->cmds, (size_t)nc * sizeof *na);
        if (!na) return false;
        a->cmds = na;
        a->cap_cmds = nc;
    }
    c = make_cmd(def);
    if (!c) return false;
    a->cmds[a->ncmds] = c;
    if (!map_insert(a, a->ncmds)) {
        free_cmd(c);
        return false;
    }
    a->ncmds++;
    return true;
}

bool app_cmd_add(app *a, const char *id, const char *label, ui_icon icon, uint32_t flags,
                 app_cmd_fn run, app_cmd_pred enabled)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = flags;
    d.run = run;
    d.enabled = enabled;
    return app_cmd_register(a, &d);
}

void app_cmds_free(app *a)
{
    for (int32_t i = 0; i < a->ncmds; i++) free_cmd(a->cmds[i]);
    free(a->cmds);
    free(a->cmd_map);
    a->cmds = NULL;
    a->cmd_map = NULL;
    a->ncmds = a->cap_cmds = 0;
    a->cmd_map_cap = 0;
}

const app_cmd *app_cmd_find(const app *a, const char *id)
{
    int32_t i = find_index(a, id);
    return i >= 0 ? a->cmds[i] : NULL;
}

bool app_cmd_exists(const app *a, const char *id) { return find_index(a, id) >= 0; }

int32_t app_cmd_count(const app *a) { return a->ncmds; }

const app_cmd *app_cmd_at(const app *a, int32_t i)
{
    return i >= 0 && i < a->ncmds ? a->cmds[i] : NULL;
}

static bool cmd_enabled(app *a, const app_cmd *c)
{
    if (!c) return false;
    if ((c->flags & APP_CMD_NEEDS_DOC) && !app_active_doc(a)) return false;
    return c->enabled ? c->enabled(a, c) : true;
}

bool app_cmd_enabled(app *a, const char *id) { return cmd_enabled(a, app_cmd_find(a, id)); }

bool app_cmd_checked(app *a, const char *id)
{
    const app_cmd *c = app_cmd_find(a, id);
    return c && c->checked && c->checked(a, c);
}

/* lane C: Undo and Redo step through the edits of a live object whose
 * tool records every edit in history (APP_TOOL_HISTORY_EDITS). */
static bool keeps_live_edit(app *a, const app_cmd *c)
{
    const app_tool *t = app_tool_current(a);
    if (!t || !(t->flags & APP_TOOL_HISTORY_EDITS)) return false;
    return strcmp(c->id, "edit.undo") == 0 || strcmp(c->id, "edit.redo") == 0;
}

bool app_cmd_exec(app *a, const char *id)
{
    const app_cmd *c = app_cmd_find(a, id);
    if (!c || !cmd_enabled(a, c)) return false;
    if (!(c->flags & APP_CMD_NO_COMMIT) && !keeps_live_edit(a, c)) app_tool_finish(a);
    /* the commit may have changed what is enabled (e.g. a live wand) */
    if (!cmd_enabled(a, c)) return false;
    c->run(a, c);
    app_request_frame(a);
    return true;
}

const char *app_cmd_shortcut_text(const app *a, const char *id)
{
    const app_cmd *c = app_cmd_find(a, id);
    app_key k[1];
    const char *keys;
    app *m = (app *)a;
    if (c) {
        if (c->nkeys <= 0) return NULL;
        app_key_format(c->keys[0], is_mac(), m->key_text, sizeof m->key_text);
        return m->key_text;
    }
    keys = app_keymap_lookup(id);
    if (!keys || app_key_parse(keys, is_mac(), k, 1) != 1) return NULL;
    app_key_format(k[0], is_mac(), m->key_text, sizeof m->key_text);
    return m->key_text;
}

/* ---- dispatch ------------------------------------------------------------------------ */
static bool plain_char_key(int32_t key)
{
    return (key >= 'a' && key <= 'z') || (key >= '0' && key <= '9') || key == SDLK_SPACE ||
           key == SDLK_COMMA || key == SDLK_PERIOD || key == SDLK_SLASH ||
           key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET || key == SDLK_MINUS ||
           key == SDLK_EQUALS || key == SDLK_APOSTROPHE || key == SDLK_SEMICOLON ||
           key == SDLK_BACKSPACE || key == SDLK_DELETE || key == SDLK_HOME || key == SDLK_END ||
           key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN;
}

/* ---- lane KEYS: menus, typed characters, view keys, pointer nudge ------------------ */
/* Menu access keys live in the toolkit (ui.h "menu keyboard"; menu.c marks
 * them): Alt + F, E, V, I, L, A, C open the menus, letters choose items,
 * a lone Alt focuses the menu bar. app_menu_rights is kept for app.c and
 * does nothing unless something sets menu_rights. */
void app_menu_rights(app *a)
{
    SDL_Event e;
    if (a->menu_rights <= 0) return;
    if (a->menu_delay > 0) {
        a->menu_delay--;
        app_request_frame(a);
        return;
    }
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_RIGHT;
    e.key.down = true;
    (void)ui_event(a->ui, &e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    (void)ui_event(a->ui, &e);
    a->menu_rights--;
    app_request_frame(a);
}

/* lane UIB (wave 4 item 11): with the menu bar focused by a lone Alt, a
 * letter that is no menu title runs the command bound to Alt + that letter,
 * so a lone Alt then X opens Settings exactly like Alt+X (the access key of
 * the Settings button at the right of the menu bar, MENUS.md). */
static bool alt_letter_cmd(app *a, int32_t key)
{
    if (key < 'a' || key > 'z') return false;
    for (int32_t i = 0; i < a->ncmds; i++) {
        const app_cmd *c = a->cmds[i];
        for (int k = 0; k < c->nkeys; k++) {
            if (c->keys[k].key != key || c->keys[k].mods != UI_MOD_ALT) continue;
            ui_menubar_unfocus(a->ui);
            (void)app_cmd_exec(a, c->id);
            app_request_frame(a);
            return true;
        }
    }
    return false;
}

/* Alt+H opens the Help menu behind the "?" button (K-UI-HELPMENU), Alt+T
 * the tool dropdown of the toolbar (K-UI-TOOLDROP); with the menu bar
 * focused by a lone Alt, H and T alone do the same, and so does any other
 * Alt + letter command (alt_letter_cmd). Any other press is swallowed while
 * a menu owns the keyboard (F-KEY-UI-MENU-MNEMONIC). */
static bool menu_keys(app *a, int32_t key, uint32_t mods)
{
    ui_ctx *ui = a->ui;
    uint32_t m = mods & ~UI_MOD_SHIFT;
    bool focused = ui_menubar_focused(ui);
    if ((key == 'h' || key == 't') && (m == UI_MOD_ALT || (focused && m == 0u))) {
        ui_menubar_unfocus(ui);
        ui_popup_close(ui);                 /* outside a popup: closes every popup */
        ui_open_request(ui, key == 'h' ? "##help_menu" : "##tool_choice");
        app_request_frame(a);
        return true;
    }
    if (focused && m == 0u && alt_letter_cmd(a, key)) return true;
    return ui_menu_keyboard(ui);
}

/* K-OS-3: bindings on these characters match the typed character. */
static bool char_binding(int32_t k)
{
    return k == SDLK_LEFTBRACKET || k == SDLK_RIGHTBRACKET || k == SDLK_COMMA ||
           k == SDLK_PERIOD || k == SDLK_SLASH;
}

/* pass 0: the typed character against the character bindings (only when
 * it differs from the keycode); pass 1: the keycode as before. */
static bool binding_matches(app_key b, int pass, int32_t key, uint32_t mods, int32_t ck,
                            uint32_t cm)
{
    if (pass == 0)
        return char_binding(b.key) && ck == b.key &&
               (cm & (UI_MOD_CTRL | UI_MOD_SHIFT | UI_MOD_ALT | UI_MOD_GUI)) == b.mods;
    return app_key_matches(b, key, mods);
}

static bool is_arrow(int32_t key)
{
    return key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN;
}

static void arrow_dir(int32_t key, int *dx, int *dy)
{
    *dx = key == SDLK_LEFT ? -1 : (key == SDLK_RIGHT ? 1 : 0);
    *dy = key == SDLK_UP ? -1 : (key == SDLK_DOWN ? 1 : 0);
}

/* K-NAV-PAN-SPACE-ARROWS(-10): with Space held, arrows scroll the view by
 * 10 screen pixels (100 with Ctrl), so the step in image pixels shrinks as
 * the zoom grows (sub-pixel above 1000 %). The arrow names the direction
 * the view moves over the image. */
static bool space_pan(app *a, int32_t key, uint32_t mods)
{
    app_doc *d = app_active_doc(a);
    uint32_t m = mods & ~UI_MOD_SHIFT;
    double step;
    int dx, dy;
    if (!a->cv.space_down || !is_arrow(key) || !d) return false;
    if (m != 0u && m != UI_MOD_CTRL && m != UI_MOD_GUI) return false;
    step = (double)ui_px(a->ui, 10.0f) * (m ? 10.0 : 1.0);
    arrow_dir(key, &dx, &dy);
    app_view_pan_px(a, d, -(double)dx * step, -(double)dy * step);
    app_request_frame(a);
    return true;
}

/* K-NAV-HOME2 / K-NAV-END2 (the 3.36 rule): Home scrolls to the left edge
 * and, when that changes nothing, to the top left; End likewise to the
 * right edge, then the bottom right. */
static bool edge_key(app *a, const app_cmd *c)
{
    const char *next = strcmp(c->id, "view.home") == 0  ? "view.home_top_left"
                       : strcmp(c->id, "view.end") == 0 ? "view.end_bottom_right"
                                                        : NULL;
    app_doc *d = app_active_doc(a);
    gfx_view v0, v1;
    if (!next || !d) return false;
    v0 = app_doc_gview(a, d);
    (void)app_cmd_exec(a, c->id);
    v1 = app_doc_gview(a, d);
    if (fabs(v1.cx - v0.cx) < 1e-9 && fabs(v1.cy - v0.cy) < 1e-9) (void)app_cmd_exec(a, next);
    return true;
}

/* K-NAV-TOOLMOVE(-10), K-PAN-DRAG: arrows that no tool or command used
 * move the pointer over the canvas by one image pixel (ten with Ctrl), at
 * least one screen pixel, like 3.36. The tool sees an ordinary pointer
 * motion (a held button keeps dragging), and the system cursor follows
 * where the platform allows warping.
 * lane UIB (wave 4 item 32): the move itself is app_tool_nudge_pointer
 * (tool.c, lane TOOLA), the same function the tools with their own arrow
 * handling call, so every tool shares one 3.36 keyboard acceleration (a
 * held arrow speeds up after 15 quick repeats) and one repeat counter.
 * Only the modifier rule stays here: Alt + arrows never nudge. */
static bool nudge_pointer(app *a, int32_t key, uint32_t mods)
{
    uint32_t m = mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI);
    if (!is_arrow(key) || (m != 0u && m != UI_MOD_CTRL && m != UI_MOD_GUI)) return false;
    return app_tool_nudge_pointer(a, key, mods);
}

bool app_key_press(app *a, int32_t key, uint32_t mods, bool repeat)
{
    return app_key_press_ex(a, key, 0, mods, mods, repeat);
}

bool app_key_press_ex(app *a, int32_t key, int32_t sym, uint32_t sym_mods, uint32_t mods,
                      bool repeat)
{
    const app_tool *t = app_tool_current(a);
    bool editing = ui_text_input_active(a->ui);
    bool chord = (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) != 0;
    int32_t ck = key;
    uint32_t cm = mods;
    if (app_dialog_active(a)) return false;
    if (menu_keys(a, key, mods)) return true;
    /* letters and editing keys belong to a focused text field */
    if (editing && !chord && plain_char_key(key)) return false;
    if (!editing && space_pan(a, key, mods)) return true;
    /* K-OS-3: the typed character for [ ] , . / */
    if (sym > 0 && sym != key && char_binding(sym)) {
        ck = sym;
        cm = sym_mods;
    }
    if (t && t->key) {
        void *st = app_tool_state(a, t);
        if ((ck != key && t->key(a, st, ck, cm, true)) || t->key(a, st, key, mods, true)) {
            app_request_frame(a);
            return true;
        }
    }
    for (int pass = ck != key ? 0 : 1; pass < 2; pass++) {
        for (int32_t i = 0; i < a->ncmds; i++) {
            const app_cmd *c = a->cmds[i];
            for (int k = 0; k < c->nkeys; k++) {
                if (!binding_matches(c->keys[k], pass, key, mods, ck, cm)) continue;
                if (editing && !(c->flags & APP_CMD_IN_TEXT) && !chord) return false;
                if (repeat && !(c->flags & APP_CMD_REPEAT)) return true;
                if (!edge_key(a, c)) (void)app_cmd_exec(a, c->id);
                return true;
            }
        }
    }
    if (!chord && !repeat && app_tool_letter(a, key, mods)) return true;
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) && !chord && !repeat) {
        /* K-UI-FINISH / K-UI-DESELECT */
        if (app_tool_live(a)) {
            if (key == SDLK_ESCAPE) app_tool_cancel(a);
            else app_tool_finish(a);
            app_request_frame(a);
            return true;
        }
        return app_cmd_exec(a, "edit.deselect");
    }
    if (!editing && nudge_pointer(a, key, mods)) return true;
    return false;
}
