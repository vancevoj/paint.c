/* keys_os.h - lane KEYS: platform keyboard integration (F-KEY-OS-1).
 *
 * macOS: SDL creates a default Cocoa menu bar (application menu and Window
 * menu) whose key equivalents are handled by AppKit before the key reaches
 * the window: Cmd+H (Hide), Cmd+M (Minimize), Cmd+W (Close window) and
 * Cmd+, (Preferences) would take Rotate 90 Clockwise, Merge Layer Down,
 * Close and Toggle Layer Visibility (Ctrl means Cmd on macOS, K-OS-1).
 * app_keys_os_menus clears the key equivalents of the native items that
 * collide with a paint.c binding; the others (Cmd+Q Quit, Ctrl+Cmd+F full
 * screen) keep working. It talks to the Objective-C runtime through
 * SDL_LoadObject, so nothing links against AppKit and other platforms
 * compile the same code without using it.
 *
 * Thread rules: main thread. Ownership: nothing is retained.
 */
#ifndef KEYS_OS_H
#define KEYS_OS_H

#include "app_internal.h"

/* NSEventModifierFlags bits used by menu key equivalents. */
#define APP_NSMOD_SHIFT   (1ul << 17)
#define APP_NSMOD_CONTROL (1ul << 18)
#define APP_NSMOD_OPTION  (1ul << 19)
#define APP_NSMOD_COMMAND (1ul << 20)

/* True when the menu key equivalent equiv (UTF-8, one character; an upper
 * case letter implies Shift) with the modifier mask is a shortcut of a
 * registered paint.c command. Command maps to the platform's primary
 * modifier (ui_mod_primary), so the check is testable everywhere. a and
 * equiv are borrowed. */
bool app_keys_os_conflict(const app *a, const char *equiv, unsigned long mask);

/* macOS: clear the conflicting key equivalents of the native menus (call
 * after app_create; SDL_Init(SDL_INIT_VIDEO) created the menus). Returns
 * the number of items changed; 0 elsewhere or when the runtime is not
 * reachable. */
int  app_keys_os_menus(app *a);

#endif /* KEYS_OS_H */
