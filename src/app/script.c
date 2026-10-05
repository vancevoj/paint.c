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
 *   key COMBO                 a key press ("Ctrl+Z", "F7", "B")
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
 *   expect pixel X Y #AARRGGBB   composite of the visible layers
 *   expect dirty 0|1 | layers N | size W H | docs N | history N | tool ID
 */
#include "app_internal.h"

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
        SDL_Event e;
#if defined(__APPLE__)
        bool mac = true;
#else
        bool mac = false;
#endif
        if (app_key_parse(tok[1], mac, &k, 1) != 1) return fail(err, cap, ln, "bad key");
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = (SDL_Keycode)k.key;
        e.key.mod = (SDL_Keymod)(((k.mods & UI_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) |
                                 ((k.mods & UI_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) |
                                 ((k.mods & UI_MOD_ALT) ? SDL_KMOD_LALT : 0) |
                                 ((k.mods & UI_MOD_GUI) ? SDL_KMOD_LGUI : 0));
        e.key.down = true;
        e.key.timestamp = SDL_GetTicksNS();
        app_event(a, &e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
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
