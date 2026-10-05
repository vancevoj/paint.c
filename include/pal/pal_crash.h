/* pal_crash.h - crash logs (lane SHELL, wave 3b; additive to pal v1, the
 * frozen pal.h is unchanged).
 *
 * Settings > Diagnostics "Open Crash Log Folder" opens the folder these
 * logs go to (Paint.NET keeps the 20 most recent, HelpMenu docs). When the
 * process crashes (POSIX: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT;
 * Windows: an unhandled exception) the handler writes
 * <dir>/crash-<unix time>-<process id>.txt (mode 0600 on POSIX) with the
 * app information given at install time, the signal or exception code,
 * the faulting address and a stack trace where the C library offers one
 * (glibc and macOS backtrace(); Windows return addresses with the module
 * base), then lets the crash proceed (core dumps and the system crash
 * reporter still work).
 *
 * Thread rules: install and prune from the main thread before other
 * threads start using the process in earnest; the handler itself only
 * uses async-signal-safe calls. Ownership: dir and info are copied.
 */
#ifndef PAL_CRASH_H
#define PAL_CRASH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_CRASH_KEEP 20

/* Install the crash handler once per process (later calls only update dir
 * and info). dir must exist (UTF-8, at most 900 bytes); info is a short
 * text (version, platform; at most 1000 bytes, longer text is cut).
 * false when the handler could not be installed. */
bool pal_crash_install(const char *dir, const char *info);

/* Delete the oldest crash-*.txt files of dir beyond keep. Returns the
 * number of files removed. */
int  pal_crash_prune(const char *dir, int keep);

/* Number of crash-*.txt files in dir. */
int  pal_crash_count(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* PAL_CRASH_H */
