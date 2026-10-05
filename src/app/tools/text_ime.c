/* text_ime.c - Text tool keyboard input through pc_ui internals (lane C,
 * see text_ime.h). */
#include "text_ime.h"

#include "../../ui/ui_internal.h"

#include <string.h>

void text_ime_request(ui_ctx *ui, ui_rect caret) { ui_text_input_request(ui, caret); }

size_t text_ime_take(ui_ctx *ui, char *out, size_t cap)
{
    size_t n = ui->fin.ntext > 0 ? (size_t)ui->fin.ntext : 0u;
    if (cap == 0u) return 0u;
    if (n > cap - 1u) n = ui_utf8_floor(ui->fin.text, n, cap - 1u);
    if (n) memcpy(out, ui->fin.text, n);
    out[n] = '\0';
    ui->fin.ntext = 0;
    ui->fin.text[0] = '\0';
    return n;
}

bool text_ime_composition(const ui_ctx *ui, char *out, size_t cap, int32_t *cursor)
{
    size_t n;
    if (cursor) *cursor = 0;
    if (cap) out[0] = '\0';
    if (!ui->fin.comp_active || cap == 0u) return false;
    n = strlen(ui->fin.comp);
    if (n > cap - 1u) n = ui_utf8_floor(ui->fin.comp, n, cap - 1u);
    memcpy(out, ui->fin.comp, n);
    out[n] = '\0';
    if (cursor) *cursor = ui->fin.comp_cursor;
    return n > 0u;
}
