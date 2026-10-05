/* pc_test.h - shared single-header test harness (C17, libc only).
 *
 * Usage in a test_*.c file:
 *   #include "pc_test.h"
 *   static void t_something(void) { CHECK(1 + 1 == 2); }
 *   int main(int argc, char **argv) {
 *       pc_test_init(argc, argv);
 *       RUN(t_something);
 *       return pc_test_finish();
 *   }
 * --quick (always passed by CTest) must keep the run under ~30 s even under
 * sanitizers. Print informational lines with INFO(...). Fixed seeds only.
 */
#ifndef PC_TEST_H
#define PC_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_quick = 0;
static unsigned long g_checks = 0, g_fails = 0;

static void pc_test_fail_at(const char *file, int line, const char *expr)
{
    if (g_fails < 25u) fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
    g_fails++;
}
#define CHECK(c) do { g_checks++; if (!(c)) pc_test_fail_at(__FILE__, __LINE__, #c); } while (0)
#define INFO(...) do { printf("  info: "); printf(__VA_ARGS__); printf("\n"); } while (0)

static double pc_test_now(void)
{
#if defined(TIME_UTC)
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    /* C11 timespec_get is missing from msvcrt.dll based MinGW runtimes */
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

/* xorshift64* with a fixed default seed, so logs stay comparable. */
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static uint32_t rndu(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0u; }
static uint8_t rnd8(void) { return (uint8_t)(rnd() >> 56); }

static void pc_test_run(const char *name, void (*f)(void))
{
    unsigned long c0 = g_checks, f0 = g_fails;
    double t0 = pc_test_now();
    f();
    printf("%-28s %11lu checks %5lu fails %7.2f s\n", name,
           g_checks - c0, g_fails - f0, pc_test_now() - t0);
    fflush(stdout);
}
#define RUN(f) pc_test_run(#f, f)

static void pc_test_init(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--quick") == 0) g_quick = 1;
}

static int pc_test_finish(void)
{
    printf("TOTAL %lu checks, %lu failures\n", g_checks, g_fails);
    printf(g_fails ? "TESTS FAILED\n" : "ALL TESTS PASSED\n");
    return g_fails ? 1 : 0;
}

#endif /* PC_TEST_H */
