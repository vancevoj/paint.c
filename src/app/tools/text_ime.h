/* text_ime.h - keyboard text input for the Text tool through the UI
 * toolkit (lane C, TOOLS.md 11.1 T-TEXT-EDIT, K-TEXT-*).
 *
 * The toolkit collects SDL_EVENT_TEXT_INPUT and SDL_EVENT_TEXT_EDITING per
 * frame and starts SDL text input (and places the IME candidate window)
 * for whoever asks during a frame. Its public header does not expose that
 * yet, so this adapter is the only place that touches pc_ui internals
 * (src/ui/ui_internal.h); see the lane C report for the requested public
 * API (ui_text_input_request, ui_text_take, ui_ime_composition).
 *
 * Thread rules: main thread, between ui_begin_frame and ui_end_frame.
 * Ownership: strings are copied into caller buffers.
 */
#ifndef TEXT_IME_H
#define TEXT_IME_H

#include "ui/ui.h"

/* Keep SDL text input on for the next frames and put the IME candidate
 * window next to caret (window pixels). */
void   text_ime_request(ui_ctx *ui, ui_rect caret);
/* Take the text typed since the last frame (UTF-8, not NUL-terminated in
 * the count; out is NUL-terminated when cap > 0). Returns the byte count;
 * the toolkit's text fields will not see it afterwards. */
size_t text_ime_take(ui_ctx *ui, char *out, size_t cap);
/* The IME composition (preedit) string, if one is active: copied to out,
 * *cursor (may be NULL) receives the composition caret in code points. */
bool   text_ime_composition(const ui_ctx *ui, char *out, size_t cap, int32_t *cursor);

#endif /* TEXT_IME_H */
