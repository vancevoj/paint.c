/* ui_theme.c - the light and dark palettes and the default metrics.
 *
 * Original palette: cool neutral greys with one accent hue, in the spirit of
 * a modern desktop editor. Every accent shade is derived from the accent
 * color passed in, so apps can recolor the whole UI with one value. */
#include "ui/ui_theme.h"

static ui_color mix(ui_color a, ui_color b, float t)
{
    ui_color c = ui_color_lerp(a, b, t);
    c.a = 255u;
    return c;
}

static ui_color hexc(uint32_t rgb) { return ui_rgb_hex(rgb); }

static ui_color with_alpha(ui_color c, uint8_t a)
{
    c.a = a;
    return c;
}

ui_color ui_theme_default_accent(void) { return hexc(0x2D6CDF); }

static void metrics(ui_metrics *m)
{
    m->font_size = 13.0f;
    m->font_size_small = 12.0f;
    m->font_size_title = 13.0f;
    m->control_h = 28.0f;
    m->menubar_h = 30.0f;
    m->menu_item_h = 28.0f;
    m->row_h = 28.0f;
    m->tab_h = 32.0f;
    m->title_h = 32.0f;
    m->pad = 10.0f;
    m->pad_small = 6.0f;
    m->spacing = 6.0f;
    m->radius = 4.0f;
    m->radius_large = 8.0f;
    m->border = 1.0f;
    m->scrollbar = 12.0f;
    m->icon = 16.0f;
    m->slider_thumb = 16.0f;
    m->check = 16.0f;
    m->shadow = 16.0f;
    m->focus = 2.0f;
    m->snap = 10.0f;
    m->drag_threshold = 4.0f;
    m->tooltip_delay_ms = 550.0f;
    m->caret_blink_ms = 530.0f;
}

void ui_theme_init(ui_theme *t, ui_theme_kind kind, ui_color accent)
{
    ui_palette *p = &t->pal;
    ui_color white = hexc(0xFFFFFF), black = hexc(0x000000);
    accent.a = 255u;
    t->kind = kind;
    metrics(&t->m);
    if (kind == UI_THEME_DARK) {
        p->window = hexc(0x1B1D21);
        p->workspace = hexc(0x141518);
        p->panel = hexc(0x24272C);
        p->panel_header = hexc(0x2A2D33);
        p->raised = hexc(0x33373D);
        p->raised_hover = hexc(0x3B4047);
        p->raised_active = hexc(0x2C3035);
        p->field = hexc(0x1C1E22);
        p->field_hover = hexc(0x202227);
        p->border = hexc(0x3D4249);
        p->border_strong = hexc(0x555B65);
        p->separator = hexc(0x34383E);
        p->text = hexc(0xE7E9EC);
        p->text_dim = hexc(0xA1A7B0);
        p->text_disabled = hexc(0x656B74);
        p->text_on_accent = white;
        p->accent = accent;
        p->accent_hover = mix(accent, white, 0.10f);
        p->accent_active = mix(accent, black, 0.15f);
        p->accent_text = mix(accent, white, 0.38f);
        p->selection = mix(p->panel, accent, 0.32f);
        p->selection_text = hexc(0xF4F6F8);
        p->hover = with_alpha(white, 16u);
        p->text_select = with_alpha(accent, 140u);
        p->focus = mix(accent, white, 0.45f);
        p->scrollbar = hexc(0x4B5059);
        p->scrollbar_hover = hexc(0x606772);
        p->tooltip = hexc(0x30343A);
        p->tooltip_text = hexc(0xE7E9EC);
        p->shadow = ui_rgba(0, 0, 0, 120u);
        p->backdrop = ui_rgba(0, 0, 0, 110u);
        p->danger = hexc(0xE5605E);
        p->warning = hexc(0xE9A23B);
        p->success = hexc(0x46B57A);
        p->modified = hexc(0xF0954A);
        p->checker_a = hexc(0xFFFFFF);
        p->checker_b = hexc(0xD6D9DE);
        p->icon = hexc(0xD3D7DC);
        p->icon_accent = mix(accent, white, 0.30f);
    } else {
        p->window = hexc(0xECEEF1);
        p->workspace = hexc(0xDFE2E7);
        p->panel = hexc(0xF8F9FA);
        p->panel_header = hexc(0xF0F2F4);
        p->raised = hexc(0xFFFFFF);
        p->raised_hover = hexc(0xF4F6F9);
        p->raised_active = hexc(0xEAEDF1);
        p->field = hexc(0xFFFFFF);
        p->field_hover = hexc(0xFBFCFD);
        p->border = hexc(0xD3D8DE);
        p->border_strong = hexc(0xB4BBC5);
        p->separator = hexc(0xE1E4E8);
        p->text = hexc(0x1A1D21);
        p->text_dim = hexc(0x5E6570);
        p->text_disabled = hexc(0xA4AAB2);
        p->text_on_accent = white;
        p->accent = accent;
        p->accent_hover = mix(accent, white, 0.10f);
        p->accent_active = mix(accent, black, 0.14f);
        p->accent_text = mix(accent, black, 0.12f);
        p->selection = mix(p->panel, accent, 0.15f);
        p->selection_text = hexc(0x14171B);
        p->hover = with_alpha(black, 13u);
        p->text_select = with_alpha(accent, 80u);
        p->focus = mix(accent, black, 0.10f);
        p->scrollbar = hexc(0xC2C8D0);
        p->scrollbar_hover = hexc(0xA3ABB6);
        p->tooltip = hexc(0xFFFFFF);
        p->tooltip_text = hexc(0x1A1D21);
        p->shadow = ui_rgba(16, 24, 40, 46u);
        p->backdrop = ui_rgba(12, 16, 24, 84u);
        p->danger = hexc(0xCF3B3B);
        p->warning = hexc(0xC77D06);
        p->success = hexc(0x23894E);
        p->modified = hexc(0xE5772A);
        p->checker_a = hexc(0xFFFFFF);
        p->checker_b = hexc(0xD6D9DE);
        p->icon = hexc(0x3B414A);
        p->icon_accent = accent;
    }
}
