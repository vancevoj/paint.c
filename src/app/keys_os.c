/* keys_os.c - lane KEYS: native menu key equivalents on macOS (see
 * keys_os.h). The Objective-C runtime is reached with SDL_LoadObject and
 * called through exactly typed function pointers (objc_msgSend must be
 * cast to the method's signature on arm64). Main thread. */
#include "keys_os.h"

#include <string.h>

static int32_t equiv_key(const char *equiv, uint32_t *mods)
{
    unsigned char c = (unsigned char)equiv[0];
    if (!c || equiv[1]) return 0;                     /* one ASCII character only */
    if (c >= 'A' && c <= 'Z') {
        *mods |= UI_MOD_SHIFT;
        return (int32_t)(c - 'A' + 'a');
    }
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return (int32_t)c;
    switch (c) {
    case ',': return SDLK_COMMA;
    case '.': return SDLK_PERIOD;
    case '/': return SDLK_SLASH;
    case '[': return SDLK_LEFTBRACKET;
    case ']': return SDLK_RIGHTBRACKET;
    case '-': return SDLK_MINUS;
    case '=': return SDLK_EQUALS;
    case '+': return SDLK_PLUS;
    case ';': return SDLK_SEMICOLON;
    case '`': return SDLK_GRAVE;
    case '\\': return SDLK_BACKSLASH;
    case '\'': return SDLK_APOSTROPHE;
    default: return 0;
    }
}

bool app_keys_os_conflict(const app *a, const char *equiv, unsigned long mask)
{
    uint32_t mods = 0;
    int32_t key;
    if (!a || !equiv) return false;
    if (mask & APP_NSMOD_COMMAND) mods |= ui_mod_primary();
    if (mask & APP_NSMOD_CONTROL) mods |= UI_MOD_CTRL;
    if (mask & APP_NSMOD_OPTION) mods |= UI_MOD_ALT;
    if (mask & APP_NSMOD_SHIFT) mods |= UI_MOD_SHIFT;
    key = equiv_key(equiv, &mods);
    if (!key) return false;
    for (int32_t i = 0; i < app_cmd_count(a); i++) {
        const app_cmd *c = app_cmd_at(a, i);
        for (int k = 0; k < c->nkeys; k++)
            if (app_key_matches(c->keys[k], key, mods)) return true;
    }
    return false;
}

/* ---- the Objective-C runtime ---------------------------------------------------------- */
typedef void *(*objc_lookup_fn)(const char *name);
typedef void *(*msg_id_fn)(void *self, void *sel);
typedef void *(*msg_id_long_fn)(void *self, void *sel, long i);
typedef long (*msg_long_fn)(void *self, void *sel);
typedef unsigned long (*msg_ulong_fn)(void *self, void *sel);
typedef const char *(*msg_cstr_fn)(void *self, void *sel);
typedef void (*msg_set_fn)(void *self, void *sel, void *arg);
typedef void (*msg_void_fn)(void *self, void *sel);

typedef struct objc_rt {
    objc_lookup_fn cls, sel;
    SDL_FunctionPointer send;
} objc_rt;

static void *sel(const objc_rt *rt, const char *name) { return rt->sel(name); }

/* Clear the conflicting key equivalents of the items of menu (one level). */
static int fix_menu(app *a, const objc_rt *rt, void *menu, void *empty)
{
    msg_long_fn count = (msg_long_fn)rt->send;
    msg_id_long_fn item_at = (msg_id_long_fn)rt->send;
    msg_id_fn get = (msg_id_fn)rt->send;
    msg_ulong_fn mask_of = (msg_ulong_fn)rt->send;
    msg_cstr_fn cstr = (msg_cstr_fn)rt->send;
    msg_set_fn set = (msg_set_fn)rt->send;
    long n = menu ? count(menu, sel(rt, "numberOfItems")) : 0;
    int changed = 0;
    for (long i = 0; i < n && i < 256; i++) {
        void *item = item_at(menu, sel(rt, "itemAtIndex:"), i);
        void *key = item ? get(item, sel(rt, "keyEquivalent")) : NULL;
        const char *k = key ? cstr(key, sel(rt, "UTF8String")) : NULL;
        unsigned long m;
        if (!k || !*k) continue;
        m = mask_of(item, sel(rt, "keyEquivalentModifierMask"));
        if (!app_keys_os_conflict(a, k, m)) continue;
        pal_log(PAL_LOG_INFO, "macOS menu: key equivalent '%s' (mask %lx) left to paint.c", k, m);
        set(item, sel(rt, "setKeyEquivalent:"), empty);
        changed++;
    }
    return changed;
}

static int walk_menus(app *a, const char *lib)
{
    SDL_SharedObject *so = SDL_LoadObject(lib);
    objc_rt rt;
    msg_id_fn get;
    msg_long_fn count;
    msg_id_long_fn item_at;
    void *nsapp, *bar, *empty, *pool;
    long n;
    int changed = 0;
    if (!so) return 0;
    rt.cls = (objc_lookup_fn)SDL_LoadFunction(so, "objc_getClass");
    rt.sel = (objc_lookup_fn)SDL_LoadFunction(so, "sel_registerName");
    rt.send = SDL_LoadFunction(so, "objc_msgSend");
    if (!rt.cls || !rt.sel || !rt.send || !rt.cls("NSApplication") || !rt.cls("NSString")) {
        SDL_UnloadObject(so);
        return 0;
    }
    get = (msg_id_fn)rt.send;
    count = (msg_long_fn)rt.send;
    item_at = (msg_id_long_fn)rt.send;
    /* the strings below are autoreleased: give them a pool */
    pool = rt.cls("NSAutoreleasePool")
               ? get(get(rt.cls("NSAutoreleasePool"), sel(&rt, "alloc")), sel(&rt, "init"))
               : NULL;
    nsapp = get(rt.cls("NSApplication"), sel(&rt, "sharedApplication"));
    bar = nsapp ? get(nsapp, sel(&rt, "mainMenu")) : NULL;
    empty = get(rt.cls("NSString"), sel(&rt, "string"));
    n = bar ? count(bar, sel(&rt, "numberOfItems")) : 0;
    for (long i = 0; i < n && i < 64 && empty; i++) {
        void *top = item_at(bar, sel(&rt, "itemAtIndex:"), i);
        void *sub = top ? get(top, sel(&rt, "submenu")) : NULL;
        changed += fix_menu(a, &rt, sub, empty);
    }
    if (pool) ((msg_void_fn)rt.send)(pool, sel(&rt, "drain"));
    /* libobjc stays loaded for the process anyway; keep the handle balanced */
    SDL_UnloadObject(so);
    return changed;
}

int app_keys_os_menus(app *a)
{
#if defined(__APPLE__)
    return a && a->win ? walk_menus(a, "/usr/lib/libobjc.A.dylib") : 0;
#else
    (void)walk_menus;
    (void)a;
    return 0;
#endif
}
