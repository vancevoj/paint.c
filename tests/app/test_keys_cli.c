/* test_keys_cli.c - lane KEYS: the paintc command line. --diagnostics
 * prints its report even when no video driver can start (K-CLI-DIAG,
 * SDL_VIDEO_DRIVER=nosuchdriver) and reports the driver when one can;
 * --set KEY=VALUE writes the setting before the app starts (K-CLI-SET),
 * repeatable, and malformed overrides are rejected with the usage text.
 * Starts the real executable (PC_PAINTC_EXE or PAINTC_EXE). */
#include "keys_util.h"

#ifndef PC_PAINTC_EXE
#define PC_PAINTC_EXE ""
#endif

static bool find_paintc(char *out, size_t cap)
{
    const char *env = SDL_getenv("PAINTC_EXE");
    if (env && *env && pal_file_exists(env)) {
        app_copy_str(out, cap, env);
        return true;
    }
    if (PC_PAINTC_EXE[0] && pal_file_exists(PC_PAINTC_EXE)) {
        app_copy_str(out, cap, PC_PAINTC_EXE);
        return true;
    }
    return false;
}

/* Run paintc with args and the video driver driver (NULL = inherit);
 * *out receives stdout (malloc'ed, NUL terminated). Returns the exit code
 * or -1 when it could not start. */
static int run(const char *const *args, const char *driver, char **out)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_Process *p;
    size_t n = 0;
    int code = -1;
    *out = NULL;
    if (!props || !env) return -1;
    if (driver) SDL_SetEnvironmentVariable(env, "SDL_VIDEO_DRIVER", driver, true);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    p = SDL_CreateProcessWithProperties(props);
    if (p) {
        void *data = SDL_ReadProcess(p, &n, &code);
        if (data) {
            *out = (char *)malloc(n + 1u);
            if (*out) {
                memcpy(*out, data, n);
                (*out)[n] = '\0';
            }
            SDL_free(data);
        }
        SDL_DestroyProcess(p);
    }
    SDL_DestroyEnvironment(env);
    SDL_DestroyProperties(props);
    return code;
}

static void t_diagnostics(void)
{
    char exe[1024], *out = NULL;
    const char *args[4];
    int rc;
    if (!find_paintc(exe, sizeof exe)) {
        INFO("paintc not found (PC_PAINTC_EXE, PAINTC_EXE); skipping");
        CHECK(true);
        return;
    }
    args[0] = exe;
    args[1] = "--diagnostics";
    args[2] = NULL;
    /* no video driver can start: the report still comes, exit code 0 */
    rc = run(args, "nosuchdriver", &out);
    CHECK(rc == 0 && out != NULL);
    if (out) {
        CHECK(strstr(out, "paint.c ") != NULL && strstr(out, "logical processors:") != NULL);
        CHECK(strstr(out, "memory:") != NULL && strstr(out, "render driver 0:") != NULL);
        CHECK(strstr(out, "settings folder:") != NULL);
        CHECK(strstr(out, "video: unavailable") != NULL);
        CHECK(strstr(out, "SDL_Init") == NULL);
    }
    free(out);
    /* with a driver: which one, and the displays */
    rc = run(args, "dummy", &out);
    CHECK(rc == 0 && out && strstr(out, "video driver: dummy") != NULL);
    CHECK(out && strstr(out, "display 0:") != NULL);
    free(out);
}

static void t_set(void)
{
    char exe[1024], dir[1024], path[1100], *out = NULL;
    const char *args[16];
    uint8_t *text = NULL;
    size_t len = 0;
    int rc, k = 0;
    if (!find_paintc(exe, sizeof exe)) {
        INFO("paintc not found; skipping");
        CHECK(true);
        return;
    }
    at_out_path(dir, sizeof dir, "test_keys_cli_cfg");
    pal_path_join(path, sizeof path, dir, "settings.ini");
    (void)pal_remove(path);
    args[k++] = exe;
    args[k++] = "--headless";
    args[k++] = "--config-dir";
    args[k++] = dir;
    args[k++] = "--set";
    args[k++] = "gfx.software=1";
    args[k++] = "--set";
    args[k++] = "keys.test-value=hello world";
    args[k++] = "--self-test";
    args[k] = NULL;
    rc = run(args, "dummy", &out);
    CHECK(rc == 0);
    free(out);
    CHECK(pal_read_file(path, 1u << 20, &text, &len) == PC_OK);
    if (text) {
        CHECK(strstr((const char *)text, "gfx.software=1") != NULL);
        CHECK(strstr((const char *)text, "keys.test-value=hello world") != NULL);
        free(text);
    }
    /* malformed: usage text, exit code 2, nothing written */
    (void)pal_remove(path);
    args[5] = "bad key=1";
    rc = run(args, "dummy", &out);
    CHECK(rc == 2 && out && strstr(out, "--set KEY=VALUE") != NULL);
    CHECK(!pal_file_exists(path));
    free(out);
    args[5] = "=1";
    rc = run(args, "dummy", &out);
    CHECK(rc == 2);
    free(out);
    args[4] = "--set";
    args[5] = NULL;                       /* --set without a value */
    rc = run(args, "dummy", &out);
    CHECK(rc == 2);
    free(out);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_diagnostics);
    RUN(t_set);
    at_quit();
    return pc_test_finish();
}
