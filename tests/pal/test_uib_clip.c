/* test_uib_clip.c - lane UIB (wave 4 items 36 and 43): the clipboard retry
 * policy of the Windows shim.
 *   t_schedule     pal__clip_retry_delay: short first waits, 100 ms steps,
 *                  a one second budget, clean refusals (every platform)
 *   t_contention   Windows (and Wine): another thread holds the clipboard
 *                  open for 300 ms; Copy (pal_clip_set_text) waits for it
 *                  and succeeds instead of failing after 50 ms; a hold
 *                  longer than the budget fails cleanly in about a second
 * The test takes the CTest resource lock "clipboard" (tests/pal/
 * CMakeLists.txt), so no other clipboard test runs at the same time. */
#include "pc_test.h"
#include "pal_internal.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static void t_schedule(void)
{
    uint32_t elapsed = 0;
    int n = 0, prev = 0, d;
    CHECK(pal__clip_retry_delay(0, 0u) == 1);
    CHECK(pal__clip_retry_delay(1, 1u) == 2);
    CHECK(pal__clip_retry_delay(2, 3u) == 4);
    for (;;) {
        d = pal__clip_retry_delay(n, elapsed);
        if (d < 0) break;
        CHECK(d >= 1 && d <= 100);
        CHECK(d >= prev || elapsed + (uint32_t)d == PAL__CLIP_RETRY_BUDGET_MS);
        prev = d;
        elapsed += (uint32_t)d;
        n++;
        if (n > 1000) break;
    }
    INFO("%d retries over %u ms", n, (unsigned)elapsed);
    CHECK(elapsed == PAL__CLIP_RETRY_BUDGET_MS);
    CHECK(n >= 10 && n <= 40);
    /* the budget counts wall time, not attempts: a slow Sleep ends sooner */
    CHECK(pal__clip_retry_delay(3, 999u) == 1);
    CHECK(pal__clip_retry_delay(3, 1000u) == -1);
    CHECK(pal__clip_retry_delay(3, 0xFFFFFFFFu) == -1);
    CHECK(pal__clip_retry_delay(-1, 0u) == -1);
    CHECK(pal__clip_retry_delay(1000000, 5u) == 100);
}

#if defined(_WIN32)
typedef struct holder {
    HANDLE ready;
    DWORD  hold_ms;
    bool   opened;
} holder;

/* Another "program": a thread with its own window that opens the
 * clipboard and keeps it open for hold_ms. */
static DWORD WINAPI hold_thread(LPVOID p)
{
    holder *h = (holder *)p;
    HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                             GetModuleHandleW(NULL), NULL);
    h->opened = w && OpenClipboard(w);
    SetEvent(h->ready);
    if (h->opened) {
        Sleep(h->hold_ms);
        CloseClipboard();
    }
    if (w) DestroyWindow(w);
    return 0;
}

/* Run pal_clip_set_text(text) while another thread holds the clipboard for
 * hold_ms. Returns the result; *ms receives the time the call took. */
static bool set_while_held(DWORD hold_ms, const char *text, uint64_t *ms)
{
    holder h;
    HANDLE t;
    uint64_t t0;
    bool ok;
    h.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    h.hold_ms = hold_ms;
    h.opened = false;
    t = h.ready ? CreateThread(NULL, 0, hold_thread, &h, 0, NULL) : NULL;
    CHECK(t != NULL);
    if (!t) {
        if (h.ready) CloseHandle(h.ready);
        *ms = 0;
        return false;
    }
    (void)WaitForSingleObject(h.ready, 10000u);
    CHECK(h.opened);
    t0 = SDL_GetTicks();
    ok = pal_clip_set_text(text);
    *ms = SDL_GetTicks() - t0;
    (void)WaitForSingleObject(t, 15000u);
    CloseHandle(t);
    CloseHandle(h.ready);
    return ok;
}

static void t_contention(void)
{
    uint64_t ms = 0;
    char *got;
    CHECK(pal_clip_set_text("before"));
    /* held briefly: Copy waits and succeeds (it failed after 10 x 5 ms) */
    CHECK(set_while_held(300u, "uib after a hold", &ms));
    INFO("set after a 300 ms hold took %u ms", (unsigned)ms);
    CHECK(ms >= 150u && ms < 1500u);
    got = pal_clip_get_text();
    CHECK(got && strcmp(got, "uib after a hold") == 0);
    free(got);
    /* held for longer than the budget: a clean failure after about a
     * second, and the old content stays */
    CHECK(!set_while_held(2500u, "never", &ms));
    INFO("set during a 2500 ms hold gave up after %u ms", (unsigned)ms);
    CHECK(ms >= 900u && ms < 2400u);
    got = pal_clip_get_text();
    CHECK(got && strcmp(got, "uib after a hold") == 0);
    free(got);
}
#endif

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rndu;                                     /* pc_test.h helpers unused here */
    (void)rnd8;
    if (!SDL_Init(SDL_INIT_EVENTS) || !pal_init("org.paintc.test", "paintc", "paint.c")) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_schedule);
#if defined(_WIN32)
    RUN(t_contention);
#endif
    pal_quit();
    SDL_Quit();
    return pc_test_finish();
}
