/* script.c - scripted input for integration tests, screenshots and the
 * --self-test mode (app.h app_script_run). One command per line; blank
 * lines and lines starting with '#' are ignored. Coordinates are document
 * pixels unless stated; pointer commands synthesize SDL events and run
 * frames, exactly like real input.
 *
 *   open PATH                 open an image (waits for the decode)
 *   new W H                   new image filled white
 *   save PATH                 save the active image (type from the extension)
 *   close                     close the active image without prompting
 *   tool ID                   select a tool ("pencil", "paintbrush", ...)
 *   primary #AARRGGBB         set the primary color (also: secondary)
 *   width W                   brush width
 *   blend N                   tool blend mode (0..13, 14 = overwrite)
 *   antialias 0|1
 *   cmd ID                    execute a command
 *   key COMBO                 a key press ("Ctrl+Z", "F7", "B"); its modifiers go
 *                             down before and up after it (lane UIB)
 *   down X Y [left|right|middle]  pointer press at a document position
 *   move X Y                  pointer motion
 *   up X Y [left|right|middle]
 *   stroke X0 Y0 X1 Y1 [N] [left|right]  press, N motions, release
 *   sclick X Y [left|right]   click at window coordinates (menus, panels)
 *   smove X Y                 pointer motion in window coordinates
 *   zoom PERCENT | fit        view zoom
 *   frames N                  run N frames
 *   wait                      finish background work
 *   screenshot PATH           write the frame as BMP
 *   type TEXT                 text input (SDL_EVENT_TEXT_INPUT; lane C, Text tool)
 *   ime TEXT                  IME composition string (SDL_EVENT_TEXT_EDITING; lane C)
 *   set KEY VALUE             write a settings key and notify the tool (lane C)
 *   expect pixel X Y #AARRGGBB   composite of the visible layers
 *   expect dirty 0|1 | layers N | size W H | docs N | history N | tool ID
 *
 * Lane I additions (files, autosave, recovery, drag and drop):
 *   saveas PATH               save with the type of the extension (Save As
 *                             without the dialogs; flattens when needed)
 *   autosave                  write every modified image's autosave now
 *   idle MS                   run the frame loop for MS milliseconds
 *                             (timers such as the autosave interval fire)
 *   print TEXT                write TEXT and a newline to stdout (flushed)
 *   touch PATH                write an empty file (a marker for other processes)
 *   recover [all|I]           open the images of crashed sessions
 *   discard [all|I]           delete them
 *   drop open|layers|ask PATH a file dropped on the window
 *   select N                  make image N (0 based) active
 *   wheel X Y DY              mouse wheel at window coordinates (DY > 0: away)
 *   expect autosaved N        images whose current state is autosaved
 *   expect recovery N         images found by a recovery scan
 *   expect recent N | dialogs N | name NAME | path PATH | layername I NAME
 */
#include "app_internal.h"
#include "app/app_io.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void settle(app *a, int frames)
{
    for (int i = 0; i < frames; i++) {
        app_tasks_wait(a);
        (void)app_frame(a, true);
    }
}

static int parse_btn(const char *s)
{
    if (!s || !*s || strcmp(s, "left") == 0) return SDL_BUTTON_LEFT;
    if (strcmp(s, "right") == 0) return SDL_BUTTON_RIGHT;
    if (strcmp(s, "middle") == 0) return SDL_BUTTON_MIDDLE;
    return SDL_BUTTON_LEFT;
}

static bool to_screen(app *a, double dx, double dy, float *sx, float *sy)
{
    app_doc *d = app_active_doc(a);
    gfx_view v;
    double x, y;
    float k;
    if (!d) return false;
    v = app_doc_gview(a, d);
    gfx_view_to_screen(&v, dx, dy, &x, &y);
    k = a->fi.px_per_point > 0.0f ? a->fi.px_per_point : 1.0f;
    *sx = (float)x / k;
    *sy = (float)y / k;
    return true;
}

static void mouse_event(app *a, Uint32 type, float x, float y, int button)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = 1;
        e.motion.timestamp = SDL_GetTicksNS();
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = (Uint8)button;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = 1;
        e.button.timestamp = SDL_GetTicksNS();
    }
    app_event(a, &e);
}

static void key_event(app *a, SDL_Keycode key, SDL_Scancode sc, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.scancode = sc;
    e.key.mod = mod;
    e.key.down = down;
    e.key.timestamp = SDL_GetTicksNS();
    app_event(a, &e);
}

/* "key COMBO" like a real keyboard (lane UIB, wave 4 item 10): each
 * modifier goes down first (its event carries the modifiers held so far),
 * then the key goes down and up with all of them, then the modifiers come
 * up in reverse order, the last release carrying no modifier. Before, the
 * key up carried the modifiers and no modifier was ever released, so the
 * app kept seeing Ctrl or Alt held: a later scripted drag with Move
 * Selected Pixels became a Ctrl copy, and access key underlines stayed. */
static void key_combo(app *a, const app_key *k)
{
    static const struct {
        uint32_t     ui;
        SDL_Keycode  key;
        SDL_Scancode sc;
        SDL_Keymod   mod;
    } mk[4] = {
        { UI_MOD_CTRL, SDLK_LCTRL, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL },
        { UI_MOD_SHIFT, SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT },
        { UI_MOD_ALT, SDLK_LALT, SDL_SCANCODE_LALT, SDL_KMOD_LALT },
        { UI_MOD_GUI, SDLK_LGUI, SDL_SCANCODE_LGUI, SDL_KMOD_LGUI },
    };
    SDL_Keymod held = SDL_KMOD_NONE;
    for (int i = 0; i < 4; i++) {
        if (!(k->mods & mk[i].ui)) continue;
        held = (SDL_Keymod)(held | mk[i].mod);
        key_event(a, mk[i].key, mk[i].sc, held, true);
    }
    key_event(a, (SDL_Keycode)k->key, SDL_SCANCODE_UNKNOWN, held, true);
    key_event(a, (SDL_Keycode)k->key, SDL_SCANCODE_UNKNOWN, held, false);
    for (int i = 3; i >= 0; i--) {
        if (!(k->mods & mk[i].ui)) continue;
        held = (SDL_Keymod)(held & ~mk[i].mod);
        key_event(a, mk[i].key, mk[i].sc, held, false);
    }
}

static pc_px32 parse_color(const char *s, bool *ok)
{
    unsigned long v;
    char *end;
    pc_px32 c;
    memset(&c, 0, sizeof c);
    *ok = false;
    if (!s || *s != '#') return c;
    v = strtoul(s + 1, &end, 16);
    if (*end || strlen(s + 1) != 8u) return c;
    c.a = (uint8_t)(v >> 24);
    c.r = (uint8_t)(v >> 16);
    c.g = (uint8_t)(v >> 8);
    c.b = (uint8_t)v;
    *ok = true;
    return c;
}

#define MAXTOK 8

static int tokenize(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p && n < MAXTOK) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n == MAXTOK - 1) {             /* the rest of the line (paths with spaces) */
            char *e = p + strlen(p);
            while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
            tok[n++] = p;
            break;
        }
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* Paths may contain spaces: join tokens 1.. back together. */
static const char *rest_of(char **tok, int n, char *buf, size_t cap)
{
    buf[0] = '\0';
    for (int i = 1; i < n; i++) {
        if (i > 1) strncat(buf, " ", cap - strlen(buf) - 1u);
        strncat(buf, tok[i], cap - strlen(buf) - 1u);
    }
    return buf;
}

static int fail(char *err, size_t cap, int line, const char *msg)
{
    if (err && cap) snprintf(err, cap, "line %d: %s", line, msg);
    pal_log(PAL_LOG_ERROR, "script line %d: %s", line, msg);
    return line > 0 ? line : 1;
}

static int run_line(app *a, char **tok, int n, int ln, char *err, size_t cap)
{
    const char *c = tok[0];
    char buf[2048], msg[256];
    app_doc *d = app_active_doc(a);
    if (strcmp(c, "open") == 0 && n >= 2) {
        if (!app_open_path(a, rest_of(tok, n, buf, sizeof buf))) return fail(err, cap, ln, "open");
        settle(a, 2);
        if (!app_active_doc(a) || !app_active_doc(a)->path ||
            strcmp(app_active_doc(a)->path, buf) != 0)
            return fail(err, cap, ln, "open did not produce the image");
    } else if (strcmp(c, "new") == 0 && n >= 3) {
        app_doc *nd = app_doc_new_image(a, (uint32_t)atoi(tok[1]), (uint32_t)atoi(tok[2]),
                                        app_px_make(255, 255, 255, 255));
        if (!nd) return fail(err, cap, ln, "new");
        app_doc_set_untitled(a, nd);
        if (!app_add_doc(a, nd)) return fail(err, cap, ln, "new");
        settle(a, 2);
    } else if (strcmp(c, "save") == 0 && n >= 2) {
        pc_status st;
        if (!d) return fail(err, cap, ln, "no image");
        st = app_save_doc_to(a, d, rest_of(tok, n, buf, sizeof buf), NULL, NULL, true);
        if (st != PC_OK) {
            snprintf(msg, sizeof msg, "save: %s", pc_status_str(st));
            return fail(err, cap, ln, msg);
        }
    } else if (strcmp(c, "saveas") == 0 && n >= 2) {
        pc_status st;
        if (!d) return fail(err, cap, ln, "no image");
        st = app_save_doc_to(a, d, rest_of(tok, n, buf, sizeof buf), NULL, NULL, true);
        if (st != PC_OK) {
            snprintf(msg, sizeof msg, "saveas: %s", pc_status_str(st));
            return fail(err, cap, ln, msg);
        }
        settle(a, 1);
    } else if (strcmp(c, "autosave") == 0) {
        (void)app_autosave_now(a, true);
        settle(a, 1);
    } else if (strcmp(c, "idle") == 0 && n >= 2) {
        uint64_t end = SDL_GetTicks() + (uint64_t)(atoi(tok[1]) > 0 ? atoi(tok[1]) : 0);
        while (SDL_GetTicks() < end) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) app_event(a, &e);
            if (!app_frame(a, false)) break;
            SDL_Delay(5);
        }
        settle(a, 1);
    } else if (strcmp(c, "touch") == 0 && n >= 2) {
        if (pal_write_file_atomic(rest_of(tok, n, buf, sizeof buf), "", 0u) != PC_OK)
            return fail(err, cap, ln, "touch");
    } else if (strcmp(c, "print") == 0) {
        printf("%s\n", n >= 2 ? rest_of(tok, n, buf, sizeof buf) : "");
        fflush(stdout);
    } else if ((strcmp(c, "recover") == 0 || strcmp(c, "discard") == 0)) {
        int which = n >= 2 && strcmp(tok[1], "all") != 0 ? atoi(tok[1]) : -1;
        (void)app_recovery_scan(a);
        if (c[0] == 'r') (void)app_recovery_restore(a, which);
        else (void)app_recovery_discard(a, which);
        settle(a, 3);
    } else if (strcmp(c, "drop") == 0 && n >= 3) {
        app_drop_action act = strcmp(tok[1], "open") == 0     ? APP_DROP_OPEN
                              : strcmp(tok[1], "layers") == 0 ? APP_DROP_LAYERS
                                                              : APP_DROP_ASK;
        const char *path;
        char pbuf[2048];
        pbuf[0] = '\0';
        for (int i = 2; i < n; i++) {
            if (i > 2) strncat(pbuf, " ", sizeof pbuf - strlen(pbuf) - 1u);
            strncat(pbuf, tok[i], sizeof pbuf - strlen(pbuf) - 1u);
        }
        path = pbuf;
        app_drop_files(a, &path, 1, act);
        settle(a, 3);
    } else if (strcmp(c, "wheel") == 0 && n >= 4) {
        SDL_Event e;
        float sx = (float)atof(tok[1]), sy = (float)atof(tok[2]);
        mouse_event(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        settle(a, 1);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.x = 0.0f;
        e.wheel.y = (float)atof(tok[3]);
        e.wheel.mouse_x = sx;
        e.wheel.mouse_y = sy;
        e.wheel.which = 1;
        e.wheel.timestamp = SDL_GetTicksNS();
        app_event(a, &e);
        settle(a, 2);
    } else if (strcmp(c, "select") == 0 && n >= 2) {
        app_doc *sd = app_doc_at(a, atoi(tok[1]));
        if (!sd) return fail(err, cap, ln, "no such image");
        app_set_active_doc(a, sd);
        settle(a, 1);
    } else if (strcmp(c, "close") == 0) {
        if (d) app_close_doc_now(a, d);
        settle(a, 1);
    } else if (strcmp(c, "tool") == 0 && n >= 2) {
        if (!app_tool_select(a, tok[1])) return fail(err, cap, ln, "unknown tool");
    } else if ((strcmp(c, "primary") == 0 || strcmp(c, "secondary") == 0) && n >= 2) {
        bool ok;
        pc_px32 col = parse_color(tok[1], &ok);
        if (!ok) return fail(err, cap, ln, "bad color");
        if (c[0] == 'p') app_set_primary(a, col);
        else app_set_secondary(a, col);
    } else if (strcmp(c, "width") == 0 && n >= 2) {
        a->ts.width = (float)atof(tok[1]);
        app_tool_settings_changed(a);
    } else if (strcmp(c, "blend") == 0 && n >= 2) {
        a->ts.blend = atoi(tok[1]);
    } else if (strcmp(c, "antialias") == 0 && n >= 2) {
        a->ts.antialias = atoi(tok[1]) != 0;
    } else if (strcmp(c, "cmd") == 0 && n >= 2) {
        if (!app_cmd_exec(a, tok[1])) {
            snprintf(msg, sizeof msg, "command %s is unknown or disabled", tok[1]);
            return fail(err, cap, ln, msg);
        }
        settle(a, 1);
    } else if (strcmp(c, "key") == 0 && n >= 2) {
        /* through SDL events, so widgets and dialogs see the key too */
        app_key k;
#if defined(__APPLE__)
        bool mac = true;
#else
        bool mac = false;
#endif
        if (app_key_parse(tok[1], mac, &k, 1) != 1) return fail(err, cap, ln, "bad key");
        key_combo(a, &k);
        settle(a, 2);
    } else if ((strcmp(c, "down") == 0 || strcmp(c, "up") == 0 || strcmp(c, "move") == 0) &&
               n >= 3) {
        float sx, sy;
        if (!to_screen(a, atof(tok[1]), atof(tok[2]), &sx, &sy))
            return fail(err, cap, ln, "no image");
        mouse_event(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        {
            int btn = parse_btn(n > 3 ? tok[3] : NULL);
            if (c[0] == 'd') mouse_event(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, btn);
            if (c[0] == 'u') mouse_event(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, btn);
        }
        settle(a, 1);
    } else if (strcmp(c, "stroke") == 0 && n >= 5) {
        double x0 = atof(tok[1]), y0 = atof(tok[2]), x1 = atof(tok[3]), y1 = atof(tok[4]);
        int steps = n > 5 ? atoi(tok[5]) : 8, btn = parse_btn(n > 6 ? tok[6] : NULL);
        float sx, sy;
        if (steps < 1) steps = 1;
        if (!to_screen(a, x0, y0, &sx, &sy)) return fail(err, cap, ln, "no image");
        mouse_event(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        settle(a, 1);
        mouse_event(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, btn);
        settle(a, 1);
        for (int i = 1; i <= steps; i++) {
            double t = (double)i / (double)steps;
            (void)to_screen(a, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, &sx, &sy);
            mouse_event(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        }
        settle(a, 1);
        mouse_event(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, btn);
        settle(a, 2);
    } else if ((strcmp(c, "sclick") == 0 || strcmp(c, "smove") == 0) && n >= 3) {
        float sx = (float)atof(tok[1]), sy = (float)atof(tok[2]);
        mouse_event(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        settle(a, 2);
        if (c[1] == 'c') {
            int btn = parse_btn(n > 3 ? tok[3] : NULL);
            mouse_event(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, btn);
            settle(a, 1);
            mouse_event(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, btn);
            settle(a, 2);
        }
    } else if ((strcmp(c, "type") == 0 || strcmp(c, "ime") == 0) && n >= 2) {
        /* lane C: typing for the Text tool, through SDL text events */
        SDL_Event e;
        memset(&e, 0, sizeof e);
        rest_of(tok, n, buf, sizeof buf);
        if (c[0] == 't') {
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = buf;
            e.text.timestamp = SDL_GetTicksNS();
        } else {
            e.type = SDL_EVENT_TEXT_EDITING;
            e.edit.text = buf;
            e.edit.start = (Sint32)strlen(buf);
            e.edit.length = 0;
            e.edit.timestamp = SDL_GetTicksNS();
        }
        app_event(a, &e);
        settle(a, 2);
    } else if (strcmp(c, "set") == 0 && n >= 3) {
        /* lane C: tool options kept in the settings store (values may
         * contain spaces: font names) */
        buf[0] = '\0';
        for (int i = 2; i < n; i++) {
            if (i > 2) strncat(buf, " ", sizeof buf - strlen(buf) - 1u);
            strncat(buf, tok[i], sizeof buf - strlen(buf) - 1u);
        }
        if (!app_settings_set(a->settings, tok[1], buf)) return fail(err, cap, ln, "set");
        app_tool_settings_changed(a);
        settle(a, 1);
    } else if (strcmp(c, "sleep") == 0 && n >= 2) {
        SDL_Delay((Uint32)atoi(tok[1]));
        settle(a, 1);
    } else if (strcmp(c, "zoom") == 0 && n >= 2) {
        if (!d) return fail(err, cap, ln, "no image");
        app_view_set_zoom(a, d, atof(tok[1]) / 100.0);
        settle(a, 1);
    } else if (strcmp(c, "fit") == 0) {
        if (!d) return fail(err, cap, ln, "no image");
        d->view.need_fit = true;
        settle(a, 1);
    } else if (strcmp(c, "frames") == 0 && n >= 2) {
        settle(a, atoi(tok[1]));
    } else if (strcmp(c, "wait") == 0) {
        settle(a, 1);
    } else if (strcmp(c, "screenshot") == 0 && n >= 2) {
        settle(a, 1);
        if (!app_screenshot(a, rest_of(tok, n, buf, sizeof buf)))
            return fail(err, cap, ln, "screenshot failed");
    } else if (strcmp(c, "expect") == 0 && n >= 3) {
        const char *what = tok[1];
        if (strcmp(what, "docs") == 0) {
            if (app_doc_count(a) != atoi(tok[2])) return fail(err, cap, ln, "expect docs");
            return 0;
        }
        if (strcmp(what, "autosaved") == 0) {
            if (app_autosave_saved_count(a) != atoi(tok[2])) {
                snprintf(msg, sizeof msg, "%d images autosaved, expected %s",
                         app_autosave_saved_count(a), tok[2]);
                return fail(err, cap, ln, msg);
            }
            return 0;
        }
        if (strcmp(what, "recovery") == 0) {
            int got = app_recovery_scan(a);
            if (got != atoi(tok[2])) {
                snprintf(msg, sizeof msg, "%d images to recover, expected %s", got, tok[2]);
                return fail(err, cap, ln, msg);
            }
            return 0;
        }
        if (strcmp(what, "recent") == 0) {
            if (app_recent_count(a) != atoi(tok[2])) return fail(err, cap, ln, "expect recent");
            return 0;
        }
        if (strcmp(what, "dialogs") == 0) {
            if (app_dialog_depth(a) != atoi(tok[2])) return fail(err, cap, ln, "expect dialogs");
            return 0;
        }
        if (!d) return fail(err, cap, ln, "no image");
        if (strcmp(what, "pixel") == 0 && n >= 5) {
            bool ok;
            pc_px32 want = parse_color(tok[4], &ok), got;
            int32_t x = atoi(tok[2]), y = atoi(tok[3]);
            if (!ok) return fail(err, cap, ln, "bad color");
            if (pc_comp_rect(d->doc, pc_rect_make(x, y, 1, 1), &got, 1u, NULL) != PC_OK)
                return fail(err, cap, ln, "composite failed");
            if (memcmp(&got, &want, sizeof got) != 0) {
                snprintf(msg, sizeof msg, "pixel %d,%d is #%02X%02X%02X%02X, expected %s", (int)x,
                         (int)y, got.a, got.r, got.g, got.b, tok[4]);
                return fail(err, cap, ln, msg);
            }
        } else if (strcmp(what, "dirty") == 0) {
            if (app_doc_dirty(d) != (atoi(tok[2]) != 0)) return fail(err, cap, ln, "expect dirty");
        } else if (strcmp(what, "layers") == 0) {
            if ((int)d->doc->n_layers != atoi(tok[2])) return fail(err, cap, ln, "expect layers");
        } else if (strcmp(what, "size") == 0 && n >= 4) {
            if ((int)d->doc->w != atoi(tok[2]) || (int)d->doc->h != atoi(tok[3]))
                return fail(err, cap, ln, "expect size");
        } else if (strcmp(what, "history") == 0) {
            if ((int)app_doc_history_list(d, NULL, 0, NULL) != atoi(tok[2]))
                return fail(err, cap, ln, "expect history");
        } else if (strcmp(what, "name") == 0) {
            char nb[1024];
            nb[0] = '\0';
            for (int i = 2; i < n; i++) {
                if (i > 2) strncat(nb, " ", sizeof nb - strlen(nb) - 1u);
                strncat(nb, tok[i], sizeof nb - strlen(nb) - 1u);
            }
            if (strcmp(d->name, nb) != 0) {
                snprintf(msg, sizeof msg, "name is \"%.200s\"", d->name);
                return fail(err, cap, ln, msg);
            }
        } else if (strcmp(what, "path") == 0) {
            char pb[2048];
            pb[0] = '\0';
            for (int i = 2; i < n; i++) {
                if (i > 2) strncat(pb, " ", sizeof pb - strlen(pb) - 1u);
                strncat(pb, tok[i], sizeof pb - strlen(pb) - 1u);
            }
            if (!d->path || strcmp(d->path, pb) != 0) return fail(err, cap, ln, "expect path");
        } else if (strcmp(what, "layername") == 0 && n >= 4) {
            int li = atoi(tok[2]);
            char nb[256];
            nb[0] = '\0';
            for (int i = 3; i < n; i++) {
                if (i > 3) strncat(nb, " ", sizeof nb - strlen(nb) - 1u);
                strncat(nb, tok[i], sizeof nb - strlen(nb) - 1u);
            }
            if (li < 0 || (uint32_t)li >= d->doc->n_layers ||
                strcmp(d->doc->stack[li]->name, nb) != 0)
                return fail(err, cap, ln, "expect layername");
        } else if (strcmp(what, "tool") == 0) {
            const app_tool *t = app_tool_current(a);
            if (!t || strcmp(t->id, tok[2]) != 0) return fail(err, cap, ln, "expect tool");
        } else {
            return fail(err, cap, ln, "unknown expectation");
        }
    } else {
        return fail(err, cap, ln, "unknown command");
    }
    return 0;
}

int app_script_run(app *a, const char *text, char *err, size_t cap)
{
    const char *p = text;
    int ln = 0;
    if (err && cap) err[0] = '\0';
    if (!a || !text) return 1;
    settle(a, 2);
    while (*p) {
        char line[2048], *tok[MAXTOK];
        size_t k = 0;
        int n, rc;
        ln++;
        while (*p && *p != '\n' && k + 1u < sizeof line) line[k++] = *p++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line[k] = '\0';
        if (k && line[k - 1u] == '\r') line[k - 1u] = '\0';
        n = tokenize(line, tok);
        if (n == 0 || tok[0][0] == '#') continue;
        rc = run_line(a, tok, n, ln, err, cap);
        if (rc) return rc;
    }
    settle(a, 1);
    return 0;
}
