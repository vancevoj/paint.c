/* wake.c - lane I event hooks (app_io_event, first in app_event):
 *  - file drops go to drop.c;
 *  - loop wake-ups: pal delivers paths forwarded by a second paintc (single
 *    instance) and native dialog results from pal_pump, which runs at the
 *    start of every app_frame. An idle editor sleeps in SDL_WaitEvent, so a
 *    timer posts a private event a few times per second while the app runs
 *    as the single instance; the event wakes the loop, app_frame pumps pal
 *    and returns without drawing unless something arrived (the event never
 *    reaches the UI, which would redraw for it).
 * Thread rules: the timer callback runs on SDL's timer thread and only
 * pushes an event (guarded by an atomic flag so at most one is queued);
 * everything else is main thread. */
#include "io_internal.h"

#include <string.h>

/* One per process (the editor has one app). Never freed: SDL may still be
 * running a timer callback right after SDL_RemoveTimer returns. */
typedef struct wake_state {
    SDL_TimerID   timer;
    Uint32        type;            /* registered event type, 0 = none */
    pc_atomic_u32 queued;          /* a wake event is in the queue */
    const app    *owner;
    const app    *hooked;          /* app that got the quit hook */
} wake_state;

static wake_state g_wake;

static Uint32 SDLCALL wake_tick(void *ud, SDL_TimerID id, Uint32 interval)
{
    wake_state *w = (wake_state *)ud;
    (void)id;
    if (pc_atomic_load(&w->queued) == 0u) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = w->type;
        pc_atomic_store(&w->queued, 1u);
        if (!SDL_PushEvent(&e)) pc_atomic_store(&w->queued, 0u);
    }
    return interval;
}

static void wake_quit(app *a, app_doc *d, void *ud)
{
    (void)d;
    (void)ud;
    app_wake_poll(a, 0u);
}

void app_wake_poll(app *a, uint32_t ms)
{
    wake_state *w = &g_wake;
    if (w->timer) {
        SDL_RemoveTimer(w->timer);
        w->timer = 0;
    }
    w->owner = NULL;
    if (ms == 0u || !a) return;
    if (!w->type) w->type = SDL_RegisterEvents(1);
    if (!w->type) return;
    w->timer = SDL_AddTimer(ms, wake_tick, w);
    if (w->timer) {
        if (w->hooked != a) (void)app_hook_add(a, APP_HOOK_QUIT, wake_quit, NULL);
        w->hooked = a;
        w->owner = a;
    }
}

bool app_io_event(app *a, const SDL_Event *e)
{
    const wake_state *w;
    switch (e->type) {
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_COMPLETE:
        app_drop_event(a, e);
        return true;
    default:
        break;
    }
    w = &g_wake;
    if (w->type && e->type == w->type) {
        pc_atomic_store(&g_wake.queued, 0u);
        return true;
    }
    return false;
}
