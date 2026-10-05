/* ui_theme.h - color palettes and size tokens (lane L3).
 *
 * A theme is a plain value: the app builds one with ui_theme_init, may
 * tweak fields, and hands it to ui_set_theme (ui.h). Metrics are in DIPs;
 * the context scales them to device pixels. Pure functions, any thread.
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "ui_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_theme_kind { UI_THEME_LIGHT = 0, UI_THEME_DARK = 1 } ui_theme_kind;

typedef struct ui_palette {
    ui_color window;          /* app background (behind panels) */
    ui_color workspace;       /* canvas surround */
    ui_color panel;           /* panels, dialogs, menus */
    ui_color panel_header;    /* panel title bars, group headers */
    ui_color raised;          /* control faces (buttons) */
    ui_color raised_hover;
    ui_color raised_active;
    ui_color field;           /* text fields, list backgrounds */
    ui_color field_hover;
    ui_color border;          /* control outlines */
    ui_color border_strong;   /* hovered outlines, slider tracks */
    ui_color separator;
    ui_color text;
    ui_color text_dim;        /* secondary text, shortcuts */
    ui_color text_disabled;
    ui_color text_on_accent;
    ui_color accent;          /* fills: default buttons, checked boxes, thumbs */
    ui_color accent_hover;
    ui_color accent_active;
    ui_color accent_text;     /* accent used as a foreground on panel colors */
    ui_color selection;       /* selected rows, menu highlight */
    ui_color selection_text;
    ui_color hover;           /* subtle hover wash for flat items */
    ui_color text_select;     /* text field selection background */
    ui_color focus;           /* keyboard focus ring */
    ui_color scrollbar;
    ui_color scrollbar_hover;
    ui_color tooltip;
    ui_color tooltip_text;
    ui_color shadow;
    ui_color backdrop;        /* modal dim */
    ui_color danger;
    ui_color warning;
    ui_color success;
    ui_color modified;        /* unsaved-changes marker */
    ui_color checker_a;       /* transparency checkerboard */
    ui_color checker_b;
    ui_color icon;            /* icon line tone */
    ui_color icon_accent;     /* icon accent tone */
} ui_palette;

typedef struct ui_metrics {    /* all in DIPs */
    float font_size;          /* body text em size */
    float font_size_small;
    float font_size_title;    /* panel and dialog titles (semibold) */
    float control_h;          /* buttons, fields, combos */
    float menubar_h;
    float menu_item_h;
    float row_h;              /* list rows */
    float tab_h;
    float title_h;            /* panel and dialog title bars */
    float pad;                /* container padding */
    float pad_small;
    float spacing;            /* gap between widgets */
    float radius;             /* controls */
    float radius_large;       /* panels, dialogs, popups */
    float border;             /* outline width */
    float scrollbar;          /* scrollbar track width */
    float icon;               /* default icon size */
    float slider_thumb;       /* slider thumb diameter */
    float check;              /* checkbox and radio box */
    float shadow;             /* popup/dialog shadow blur radius */
    float focus;              /* focus ring width */
    float snap;               /* panel snap distance */
    float drag_threshold;
    float tooltip_delay_ms;
    float caret_blink_ms;
} ui_metrics;

typedef struct ui_theme {
    ui_theme_kind kind;
    ui_palette    pal;
    ui_metrics    m;
} ui_theme;

/* The default accent (a calm cobalt blue). */
ui_color ui_theme_default_accent(void);

/* Fill t with the light or dark palette built around accent (any opaque
 * color; derived shades are computed from it) and the default metrics. */
void ui_theme_init(ui_theme *t, ui_theme_kind kind, ui_color accent);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H */
