/* test_shell_crash.c - lane SHELL (wave 3b): crash logs (pal_crash.h) for
 * Settings > Diagnostics "Open Crash Log Folder".
 *   t_prune    only the newest PAL_CRASH_KEEP logs stay
 *   t_handler  (POSIX) a crashing child process writes one log with the
 *              app information and the signal, and still dies of it */
#include "pc_test.h"
#include "app_test_util.h"
#include "pal/pal_crash.h"

#if !defined(_WIN32)
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

static void clean(const char *dir)
{
    char **names = NULL;
    int n = pal_list_dir(dir, "crash-*.txt", &names);
    for (int i = 0; i < n; i++) {
        char p[1200];
        pal_path_join(p, sizeof p, dir, names[i]);
        (void)pal_remove(p);
    }
    if (n > 0) pal_free_names(names, n);
}

static void t_prune(void)
{
    char dir[1024];
    at_out_path(dir, sizeof dir, "shell_crash_prune");
    (void)pal_mkdirs(dir);
    clean(dir);
    for (int i = 0; i < 25; i++) {
        char name[64], p[1200];
        snprintf(name, sizeof name, "crash-%d-1.txt", 1700000000 + i);
        pal_path_join(p, sizeof p, dir, name);
        CHECK(pal_write_file_atomic(p, "x", 1u) == PC_OK);
    }
    CHECK(pal_crash_count(dir) == 25);
    CHECK(pal_crash_prune(dir, PAL_CRASH_KEEP) == 5);
    CHECK(pal_crash_count(dir) == PAL_CRASH_KEEP);
    {
        char p[1200];
        pal_path_join(p, sizeof p, dir, "crash-1700000004-1.txt");
        CHECK(!pal_file_exists(p));                 /* the oldest are gone */
        pal_path_join(p, sizeof p, dir, "crash-1700000005-1.txt");
        CHECK(pal_file_exists(p));
    }
    CHECK(pal_crash_prune(dir, PAL_CRASH_KEEP) == 0);
    clean(dir);
    CHECK(pal_crash_count(dir) == 0);
    CHECK(!pal_crash_install(NULL, "x") && !pal_crash_install("", "x"));
}

static void t_handler(void)
{
#if !defined(_WIN32)
    char dir[1024];
    pid_t pid;
    int status = 0;
    at_out_path(dir, sizeof dir, "shell_crash_handler");
    (void)pal_mkdirs(dir);
    clean(dir);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* the child installs the handler and crashes */
        if (!pal_crash_install(dir, "paint.c unit test\nPlatform: test")) _exit(3);
        (void)raise(SIGILL);
        _exit(4);                                   /* not reached */
    }
    if (pid > 0) {
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
        CHECK(pal_crash_count(dir) == 1);
        {
            char **names = NULL;
            int n = pal_list_dir(dir, "crash-*.txt", &names);
            if (n == 1) {
                char p[1200];
                uint8_t *data = NULL;
                size_t len = 0;
                pal_path_join(p, sizeof p, dir, names[0]);
                CHECK(pal_read_file(p, 1u << 20, &data, &len) == PC_OK);
                if (data) {
                    char *t = (char *)realloc(data, len + 1u);
                    if (t) {
                        t[len] = '\0';
                        CHECK(strstr(t, "paint.c crashed") != NULL);
                        CHECK(strstr(t, "paint.c unit test") != NULL);
                        CHECK(strstr(t, "SIGILL") != NULL);
                        free(t);
                    } else {
                        free(data);
                    }
                }
            }
            if (n > 0) pal_free_names(names, n);
        }
        clean(dir);
    }
#endif
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    /* the fork happens before SDL starts any thread */
    RUN(t_handler);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_prune);
    at_quit();
    return pc_test_finish();
}
