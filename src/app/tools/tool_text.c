/* tool_text.c - the Text tool (lane C, TOOLS.md 3.3, 11.1; docs TextTool):
 * a click places the caret and typing renders the text in the primary
 * color with the toolbar options (font, size in points or fixed 96 DPI
 * pixels, bold, italic, underline, strikeout, alignment relative to the
 * click, rendering mode, antialiasing, blend mode, selection clipping).
 * Until it is finished the text stays editable: caret keys (Ctrl for
 * words, Shift selects), Home / End, Backspace / Delete (Ctrl for words),
 * Enter for new lines, Ctrl+A / C / X / V, IME composition shown at the
 * caret, mouse clicks and drags inside the text to place the caret and
 * select, and the pulsing four-arrow handle below right of the caret moves
 * the block (either button; arrows move it 1 px while it is held). Every
 * change re-renders from the transaction's original pixels; option and
 * color changes apply live. Esc or Finish commits (one history step
 * "Text"), and so do a click outside the text (which starts a new one),
 * switching tools and running commands. The view follows the caret
 * (T-TEXT-VIEW). Layout and rendering are pc_text.h; fonts text_font.h;
 * keyboard text and IME text_ime.h. */
#include "text_font.h"
#include "text_ime.h"
#include "text_tool.h"
#include "vec_ui.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TDRAG_NONE = 0, TDRAG_HANDLE, TDRAG_SELECT };

typedef struct text_state {
    /* toolbar options (persisted) */
    char        family[96];
    double      size;
    int32_t     unit, align, mode;
    bool        bold, italic, underline, strike;
    /* editing */
    bool        editing;
    pc_text    *t;              /* owned */
    pc_vrender *vr;             /* owned */
    uint32_t    doc_id, layer_id;
    bool        dirty;          /* re-render due */
    bool        follow;         /* keep the caret in view after the next render */
    char        comp[256];      /* IME composition */
    /* pointer */
    int         drag;
    int         button;
    pc_pt       grab;           /* origin minus pointer at a handle press */
    uint64_t    blink_t0;
    uint32_t    font_gen;
    bool        fonts_set;
    char        fonts_family[96];
    bool        fonts_bold, fonts_italic;
} text_state;

#define KEY_FONT   "tool.text.font"
#define KEY_SIZE   "tool.text.size"
#define KEY_UNIT   "tool.text.unit"
#define KEY_BOLD   "tool.text.bold"
#define KEY_ITALIC "tool.text.italic"
#define KEY_UNDER  "tool.text.underline"
#define KEY_STRIKE "tool.text.strikeout"
#define KEY_ALIGN  "tool.text.align"
#define KEY_MODE   "tool.text.mode"

/* B: the 3.36 font size list (also observed in 5.2) */
static const double k_sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72,
                                  84, 96, 108, 144, 192, 216, 288 };
#define N_SIZES (sizeof k_sizes / sizeof k_sizes[0])
#define TEXT_SIZE_MIN 1.0
#define TEXT_SIZE_MAX 2000.0

static void load_options(app *a, text_state *s)
{
    const char *f = app_settings_get(app_settings_of(a), KEY_FONT);
    app_copy_str(s->family, sizeof s->family, f && *f ? f : TEXT_DEFAULT_FAMILY);
    s->size = vec_get_double(a, KEY_SIZE, 12.0, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
    s->unit = vec_get_int(a, KEY_UNIT, PC_TEXT_POINTS, 0, 1);
    s->align = vec_get_int(a, KEY_ALIGN, PC_TEXT_LEFT, 0, 2);
    s->mode = vec_get_int(a, KEY_MODE, PC_TEXT_SMOOTH, 0, (int32_t)PC_TEXT_MODE_COUNT - 1);
    s->bold = app_settings_bool(app_settings_of(a), KEY_BOLD, false);
    s->italic = app_settings_bool(app_settings_of(a), KEY_ITALIC, false);
    s->underline = app_settings_bool(app_settings_of(a), KEY_UNDER, false);
    s->strike = app_settings_bool(app_settings_of(a), KEY_STRIKE, false);
}

static void store_options(app *a, const text_state *s)
{
    app_settings *st = app_settings_of(a);
    (void)app_settings_set(st, KEY_FONT, s->family);
    vec_set_double(a, KEY_SIZE, s->size);
    vec_set_int(a, KEY_UNIT, s->unit);
    vec_set_int(a, KEY_ALIGN, s->align);
    vec_set_int(a, KEY_MODE, s->mode);
    (void)app_settings_set_bool(st, KEY_BOLD, s->bold);
    (void)app_settings_set_bool(st, KEY_ITALIC, s->italic);
    (void)app_settings_set_bool(st, KEY_UNDER, s->underline);
    (void)app_settings_set_bool(st, KEY_STRIKE, s->strike);
}

/* ---- editing state -------------------------------------------------------------------- */
static app_doc *edit_doc(app *a, const text_state *s)
{
    app_doc *d = app_active_doc(a);
    return d && s->editing && d->id == s->doc_id && d->txn && d->txn_owner == s ? d : NULL;
}

/* The transaction went away under us (document closed, cancelled). */
static void validate(app *a, text_state *s)
{
    if (s->editing && !edit_doc(a, s)) {
        s->editing = false;
        s->drag = TDRAG_NONE;
        if (s->vr) pc_vrender_reset(s->vr);
    }
}

static double doc_dpi(const app_doc *d)
{
    return d && d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
}

/* Fonts and style from the toolbar. */
static void apply_style(app *a, text_state *s)
{
    pc_text_style st;
    app_doc *d = app_active_doc(a);
    text_fonts *tf = text_fonts_get(a);
    if (!s->t) return;
    if (tf && (!s->fonts_set || strcmp(s->fonts_family, s->family) != 0 ||
               s->fonts_bold != s->bold || s->fonts_italic != s->italic ||
               s->font_gen != text_fonts_gen(tf))) {
        const pc_font_face *const *faces = NULL;
        size_t n = 0;
        if (text_fonts_faces(tf, s->family, s->bold, s->italic, &faces, &n) == PC_OK &&
            pc_text_set_fonts(s->t, faces, n) == PC_OK) {
            s->fonts_set = true;
            app_copy_str(s->fonts_family, sizeof s->fonts_family, s->family);
            s->fonts_bold = s->bold;
            s->fonts_italic = s->italic;
            s->font_gen = text_fonts_gen(tf);
        }
    }
    pc_text_style_default(&st);
    st.size = s->size;
    st.unit = (pc_text_unit)s->unit;
    st.dpi = doc_dpi(d);
    st.bold = s->bold;
    st.italic = s->italic;
    st.underline = s->underline;
    st.strikeout = s->strike;
    st.align = (pc_text_align)s->align;
    st.mode = (pc_text_mode)s->mode;
    if (pc_text_set_style(s->t, &st) != PC_OK) {
        /* the em size is out of range for this DPI: keep a valid size */
        st.size = 12.0;
        (void)pc_text_set_style(s->t, &st);
    }
    s->dirty = true;
}

static pc_vdraw_opts draw_opts(app *a)
{
    const app_tool_settings *ts = app_tool_settings_get(a);
    pc_vdraw_opts v = pc_vdraw_opts_default();
    if (ts->blend >= APP_BLEND_OVERWRITE) {
        v.paint.mode = PC_PAINT_OVERWRITE;
    } else {
        v.paint.mode = PC_PAINT_BLEND;
        v.paint.blend = (pc_blend_mode)(ts->blend < 0 ? 0 : ts->blend);
    }
    v.paint.clip_to_selection = true;
    v.clip_pixelated = !ts->sel_clip_aa;
    v.antialias = ts->antialias;
    return v;
}

static void render(app *a, text_state *s)
{
    app_doc *d = edit_doc(a, s);
    pc_paint_src src;
    pc_vdraw_opts o;
    pc_status st;
    if (!d || !s->dirty) return;
    s->dirty = false;
    memset(&src, 0, sizeof src);
    src.solid = app_primary(a);            /* the text uses the primary color (docs) */
    o = draw_opts(a);
    st = pc_text_render(s->t, s->vr, d->txn, s->layer_id, &src, &o, app_par(a), NULL);
    if (st != PC_OK) pal_log(PAL_LOG_WARN, "text render: %s", pc_status_str(st));
    app_request_frame(a);
}

static bool start_edit(app *a, text_state *s, pc_pt origin)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l;
    if (s->editing || !d || d->txn) return false;
    l = app_doc_layer(d);
    if (!l) return false;
    if (!s->t) s->t = pc_text_create();
    if (!s->vr) s->vr = pc_vrender_create();
    if (!s->t || !s->vr) return false;
    if (!app_doc_txn_begin(a, d, s, "Text")) return false;
    (void)pc_text_set_utf8(s->t, "", 0);
    pc_vrender_reset(s->vr);
    s->editing = true;
    s->doc_id = d->id;
    s->layer_id = l->id;
    s->comp[0] = '\0';
    s->blink_t0 = app_now_ms(a);
    apply_style(a, s);
    pc_text_set_origin(s->t, origin);
    s->dirty = true;
    app_request_frame(a);
    return true;
}

/* Finish: commit non-empty text as one history step. */
static bool finish(app *a, text_state *s)
{
    app_doc *d = edit_doc(a, s);
    bool done = false;
    s->drag = TDRAG_NONE;
    if (!d) {
        validate(a, s);
        return false;
    }
    render(a, s);
    if (pc_text_is_empty(s->t)) {
        app_doc_txn_cancel(a, d);
    } else {
        pc_status st = app_doc_txn_commit(a, d);
        if (st != PC_OK) app_error(a, "Could not record the text: %s.", pc_status_str(st));
        done = st == PC_OK;
    }
    s->editing = false;
    s->comp[0] = '\0';
    pc_vrender_reset(s->vr);
    app_request_frame(a);
    return done;
}

/* ---- pointer ------------------------------------------------------------------------------ */
static pc_handle_metrics metrics(app *a)
{
    app_doc *d = app_active_doc(a);
    return pc_handle_metrics_for_zoom(d ? d->view.zoom : 1.0);
}

static void text_pointer(app *a, void *st, const app_pointer *ev)
{
    text_state *s = (text_state *)st;
    pc_pt p = pc_pt_make(ev->x, ev->y);
    if (ev->kind == APP_PTR_HOVER) return;
    validate(a, s);
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        if (s->drag != TDRAG_NONE) break;
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        if (s->editing) {
            pc_handle_metrics m = metrics(a);
            pc_text_part part = pc_text_hit_test(s->t, p, &m);
            if (part == PC_TEXT_PART_HANDLE) {
                /* T-TEXT-NUB: either button moves the block */
                pc_pt o = pc_text_origin(s->t);
                s->grab = pc_pt_make(o.x - p.x, o.y - p.y);
                s->drag = TDRAG_HANDLE;
                s->button = ev->button;
                break;
            }
            if (part == PC_TEXT_PART_INSIDE && !pc_text_is_empty(s->t)) {
                pc_text_set_caret(s->t, pc_text_hit_index(s->t, p),
                                  (ev->mods & UI_MOD_SHIFT) != 0);
                s->drag = TDRAG_SELECT;
                s->button = ev->button;
                s->blink_t0 = app_now_ms(a);
                break;
            }
            if (pc_text_is_empty(s->t)) {
                /* nothing typed yet: the click just moves the caret */
                pc_text_set_origin(s->t, p);
                s->dirty = true;
                s->blink_t0 = app_now_ms(a);
                break;
            }
            (void)finish(a, s);          /* T-TEXT-COMMIT: clicking elsewhere commits */
        }
        (void)start_edit(a, s, p);
        break;
    }
    case APP_PTR_MOVE:
        if (ev->button != s->button) break;
        if (s->drag == TDRAG_HANDLE && s->editing) {
            pc_text_set_origin(s->t, pc_pt_make(p.x + s->grab.x, p.y + s->grab.y));
            s->dirty = true;
            render(a, s);
        } else if (s->drag == TDRAG_SELECT && s->editing) {
            pc_text_set_caret(s->t, pc_text_hit_index(s->t, p), true);
            app_request_frame(a);
        }
        break;
    case APP_PTR_UP:
    case APP_PTR_CANCEL:
        if (ev->kind == APP_PTR_UP && ev->button != s->button) break;
        s->drag = TDRAG_NONE;
        break;
    default:
        break;
    }
}

/* ---- keyboard ------------------------------------------------------------------------------- */
static void copy_selection(text_state *s, bool cut)
{
    size_t len, lo = pc_text_caret(s->t), hi = pc_text_sel_anchor(s->t);
    const char *u = pc_text_utf8(s->t, &len);
    char *buf;
    if (lo > hi) {
        size_t t = lo;
        lo = hi;
        hi = t;
    }
    if (lo == hi || hi > len) return;
    buf = (char *)malloc(hi - lo + 1u);
    if (!buf) return;
    memcpy(buf, u + lo, hi - lo);
    buf[hi - lo] = '\0';
    (void)pal_clip_set_text(buf);
    free(buf);
    if (cut) (void)pc_text_backspace(s->t, false);
}

static void paste(text_state *s)
{
    char *c = pal_clip_get_text();
    if (!c) return;
    (void)pc_text_insert(s->t, c, strlen(c));
    free(c);
}

/* One editing key. Returns true when used. */
static bool edit_key(app *a, text_state *s, int32_t key, uint32_t mods)
{
    bool ctrl = (mods & (UI_MOD_CTRL | UI_MOD_GUI)) != 0, shift = (mods & UI_MOD_SHIFT) != 0;
    bool text_changed = true;
    if (mods & UI_MOD_ALT) {
        /* T-TEXT-EDIT: AltGr (Ctrl+Alt) characters, and Option characters on
         * macOS, arrive as text input; their key presses must not run
         * shortcuts such as Ctrl+Alt+I or menu mnemonics */
        bool printable = key >= 0x20 && key < 0x7F;
#if defined(__APPLE__)
        return printable && !(mods & (UI_MOD_CTRL | UI_MOD_GUI));
#else
        return printable && (mods & UI_MOD_CTRL) != 0;
#endif
    }
    if (s->drag == TDRAG_HANDLE) {
        /* arrows move the block while the handle is held */
        pc_pt o = pc_text_origin(s->t);
        double dx = key == SDLK_LEFT ? -1.0 : key == SDLK_RIGHT ? 1.0 : 0.0;
        double dy = key == SDLK_UP ? -1.0 : key == SDLK_DOWN ? 1.0 : 0.0;
        if (dx != 0.0 || dy != 0.0) {
            pc_text_set_origin(s->t, pc_pt_make(o.x + dx, o.y + dy));
            s->grab.x += dx;
            s->grab.y += dy;
            s->dirty = true;
            return true;
        }
    }
    switch (key) {
    case SDLK_BACKSPACE: (void)pc_text_backspace(s->t, ctrl); break;
    case SDLK_DELETE: (void)pc_text_delete(s->t, ctrl); break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (ctrl) return false;
        (void)pc_text_insert(s->t, "\n", 1u);        /* K-TEXT-NEWLINE */
        break;
    case SDLK_LEFT:
        pc_text_move_caret(s->t, ctrl ? PC_TEXT_MOVE_WORD_LEFT : PC_TEXT_MOVE_LEFT, shift);
        text_changed = false;
        break;
    case SDLK_RIGHT:
        pc_text_move_caret(s->t, ctrl ? PC_TEXT_MOVE_WORD_RIGHT : PC_TEXT_MOVE_RIGHT, shift);
        text_changed = false;
        break;
    case SDLK_UP:
        pc_text_move_caret(s->t, PC_TEXT_MOVE_UP, shift);
        text_changed = false;
        break;
    case SDLK_DOWN:
        pc_text_move_caret(s->t, PC_TEXT_MOVE_DOWN, shift);
        text_changed = false;
        break;
    case SDLK_HOME:
        pc_text_move_caret(s->t, ctrl ? PC_TEXT_MOVE_DOC_START : PC_TEXT_MOVE_HOME, shift);
        text_changed = false;
        break;
    case SDLK_END:
        pc_text_move_caret(s->t, ctrl ? PC_TEXT_MOVE_DOC_END : PC_TEXT_MOVE_END, shift);
        text_changed = false;
        break;
    case SDLK_A:
        if (!ctrl || shift) return false;
        pc_text_select_all(s->t);
        text_changed = false;
        break;
    case SDLK_C:
    case SDLK_INSERT:
        if (!ctrl || shift) return false;
        copy_selection(s, false);
        text_changed = false;
        break;
    case SDLK_X:
        if (!ctrl || shift) return false;
        copy_selection(s, true);
        break;
    case SDLK_V:
        if (!ctrl || shift) return false;
        paste(s);
        break;
    case SDLK_ESCAPE:
        (void)finish(a, s);                         /* K-TEXT-COMMIT */
        return true;
    default:
        return false;
    }
    s->blink_t0 = app_now_ms(a);
    s->follow = true;
    if (text_changed) s->dirty = true;
    app_request_frame(a);
    return true;
}

/* Keep the caret inside the viewport (T-TEXT-VIEW). */
static void follow_caret(app *a, text_state *s, app_overlay *o)
{
    app_doc *d = app_active_doc(a);
    pc_box b;
    double x0, y0, x1, y1, dx = 0.0, dy = 0.0, m = (double)ui_px(app_ui(a), 24.0f);
    ui_rect v = a->cv.view;
    if (!d) return;
    pc_text_caret_box(s->t, pc_text_caret(s->t), &b);
    app_ov_to_screen(o, b.x0, b.y0, &x0, &y0);
    app_ov_to_screen(o, b.x1, b.y1, &x1, &y1);
    if (x0 < (double)v.x + m) dx = x0 - ((double)v.x + m);
    else if (x1 > (double)(v.x + v.w) - m) dx = x1 - ((double)(v.x + v.w) - m);
    if (y0 < (double)v.y + m) dy = y0 - ((double)v.y + m);
    else if (y1 > (double)(v.y + v.h) - m) dy = y1 - ((double)(v.y + v.h) - m);
    if (dx != 0.0 || dy != 0.0) app_view_pan_px(a, d, -dx, -dy);   /* content moves */
}

/* Text, composition and editing keys of this frame (called inside the
 * frame, after the canvas replayed the pointer events). */
static void process_input(app *a, text_state *s, app_overlay *o)
{
    ui_ctx *ui = app_ui(a);
    char buf[512];
    size_t n;
    int nk = 0;
    const ui_key_press *k;
    pc_box cb;
    double sx0, sy0, sx1, sy1;
    if (!s->editing || app_dialog_active(a) || ui_focus_id(ui) != 0) return;
    pc_text_caret_box(s->t, pc_text_caret(s->t), &cb);
    app_ov_to_screen(o, cb.x0, cb.y0, &sx0, &sy0);
    app_ov_to_screen(o, cb.x1, cb.y1, &sx1, &sy1);
    text_ime_request(ui, ui_rect_make((int32_t)sx0, (int32_t)sy0, 2, (int32_t)(sy1 - sy0) + 1));
    n = text_ime_take(ui, buf, sizeof buf);
    if (n) {
        if (pc_text_insert(s->t, buf, n) == PC_OK) {
            s->dirty = true;
            s->follow = true;
            s->blink_t0 = app_now_ms(a);
        }
    }
    (void)text_ime_composition(ui, s->comp, sizeof s->comp, NULL);
    if (s->comp[0]) return;                 /* the IME owns the keys while composing */
    k = ui_key_presses(ui, &nk);
    for (int i = 0; i < nk && s->editing; i++) {
        int32_t key;
        uint32_t mods;
        if (k[i].used) continue;
        key = k[i].key;
        mods = k[i].mods;
        if (edit_key(a, s, key, mods)) (void)ui_key_take(ui, key, mods);
    }
}

/* ---- options bar ------------------------------------------------------------------------------- */
static double size_step(double v, int dir)
{
    if (dir > 0) {
        for (size_t i = 0; i < N_SIZES; i++)
            if (k_sizes[i] > v + 1e-6) return k_sizes[i];
        return v + 24.0 > TEXT_SIZE_MAX ? TEXT_SIZE_MAX : v + 24.0;
    }
    for (size_t i = N_SIZES; i > 0; i--)
        if (k_sizes[i - 1u] < v - 1e-6) return k_sizes[i - 1u];
    return v - 1.0 < TEXT_SIZE_MIN ? TEXT_SIZE_MIN : v - 1.0;
}

static bool opt_toggle(app *a, const char *id, ui_icon icon, bool *v, const char *tip)
{
    ui_ctx *ui = app_ui(a);
    (void)app_opt_next(a, 30.0f);
    if (ui_tool_button(ui, id, icon, *v, tip)) {
        *v = !*v;
        return true;
    }
    return false;
}

static void text_options(app *a, void *st)
{
    text_state *s = (text_state *)st;
    ui_ctx *ui = app_ui(a);
    text_fonts *tf = text_fonts_get(a);
    bool ch = false;
    validate(a, s);
    /* Font */
    app_opt_label(a, "Font:");
    if (tf) {
        int32_t n = text_fonts_family_count(tf);
        const char **names = n > 0 ? (const char **)malloc((size_t)n * sizeof *names) : NULL;
        int cur = text_fonts_find_family(tf, s->family);
        if (names) {
            for (int32_t i = 0; i < n; i++) names[i] = text_fonts_family(tf, i);
            (void)app_opt_next(a, 170.0f);
            if (cur < 0) {
                /* a family that is not installed (yet): show it, keep it */
                ui_label_ex(ui, s->family, UI_LABEL_DIM);
            } else {
                int v = cur;
                if (ui_combo(ui, "##text_font", &v, names, (int)n) && v != cur) {
                    app_copy_str(s->family, sizeof s->family, names[v]);
                    ch = true;
                    ui_set_focus(ui, 0);
                }
            }
            ui_tooltip(ui, text_fonts_scanning(tf) ? "Font (looking for installed fonts...)"
                                                   : "Font");
            free(names);
        }
    }
    /* Size, presets, - and + (R 5.1.8) */
    {
        double v = s->size;
        (void)app_opt_next(a, 64.0f);
        if (ui_number_double(ui, "##text_size", &v, TEXT_SIZE_MIN, TEXT_SIZE_MAX, 1.0, 1, 0) &&
            v != s->size) {
            s->size = v;
            ch = true;
        }
        ui_tooltip(ui, "Font size");
        (void)app_opt_next(a, 22.0f);
        if (ui_icon_button(ui, "##text_size_presets", UI_ICON_CHEVRON_DOWN, "Font sizes"))
            ui_popup_open(ui, "##text_size_pop", ui_last_rect(ui), UI_POPUP_BELOW);
        if (ui_popup_begin(ui, "##text_size_pop")) {
            for (size_t i = 0; i < N_SIZES; i++) {
                char lbl[16];
                (void)snprintf(lbl, sizeof lbl, "%g", k_sizes[i]);
                if (ui_menu_radio(ui, lbl, NULL, fabs(s->size - k_sizes[i]) < 1e-6, true)) {
                    s->size = k_sizes[i];
                    ch = true;
                }
            }
            ui_popup_end(ui);
        }
        (void)app_opt_next(a, 26.0f);
        if (ui_icon_button(ui, "##text_size_dec", UI_ICON_MINUS, "Smaller font size")) {
            s->size = size_step(s->size, -1);
            ch = true;
        }
        (void)app_opt_next(a, 26.0f);
        if (ui_icon_button(ui, "##text_size_inc", UI_ICON_PLUS, "Larger font size")) {
            s->size = size_step(s->size, 1);
            ch = true;
        }
    }
    {
        static const char *const units[] = { "Points (image DPI)", "Fixed (96 DPI)" };
        int v = s->unit;
        (void)app_opt_next(a, 140.0f);
        if (ui_combo(ui, "##text_unit", &v, units, 2) && v != s->unit) {
            s->unit = v;
            ch = true;
            ui_set_focus(ui, 0);
        }
        ui_tooltip(ui, "Font size metric");
    }
    app_opt_separator(a);
    ch |= opt_toggle(a, "##text_bold", UI_ICON_BOLD, &s->bold, "Bold");
    ch |= opt_toggle(a, "##text_italic", UI_ICON_ITALIC, &s->italic, "Italic");
    ch |= opt_toggle(a, "##text_under", UI_ICON_UNDERLINE, &s->underline, "Underline");
    ch |= opt_toggle(a, "##text_strike", UI_ICON_STRIKE, &s->strike, "Strikethrough");
    app_opt_separator(a);
    {
        static const ui_icon icons[3] = { UI_ICON_ALIGN_LEFT, UI_ICON_ALIGN_CENTER,
                                          UI_ICON_ALIGN_RIGHT };
        static const char *const tips[3] = { "Align left", "Align center", "Align right" };
        for (int32_t i = 0; i < 3; i++) {
            char id[24];
            (void)snprintf(id, sizeof id, "##text_align%d", (int)i);
            (void)app_opt_next(a, 30.0f);
            if (ui_tool_button(ui, id, icons[i], s->align == i, tips[i]) && s->align != i) {
                s->align = i;
                ch = true;
            }
        }
    }
    {
        static const char *const modes[] = { "Smooth", "Sharp (Modern)", "Sharp (Classic)" };
        int v = s->mode;
        (void)app_opt_next(a, 128.0f);
        if (ui_combo(ui, "##text_mode", &v, modes, 3) && v != s->mode) {
            s->mode = v;
            ch = true;
            ui_set_focus(ui, 0);
        }
        ui_tooltip(ui, "Rendering mode");
    }
    app_opt_separator(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
    app_opt_separator(a);
    app_opt_finish(a);
    if (tf && text_fonts_gen(tf) != s->font_gen && s->editing) ch = true;
    if (ch) {
        store_options(a, s);
        app_tool_settings_changed(a);
    }
}

static void text_settings_changed(app *a, void *st)
{
    text_state *s = (text_state *)st;
    load_options(a, s);
    validate(a, s);
    if (!s->editing) return;
    apply_style(a, s);            /* T-TEXT-LIVE: options and the primary color */
    render(a, s);
}

/* ---- overlay ----------------------------------------------------------------------------- */
static void text_overlay(app *a, void *st, app_overlay *o)
{
    text_state *s = (text_state *)st;
    ui_ctx *ui = o->ui;
    pc_box cb;
    double x0, y0, x1, y1;
    validate(a, s);
    if (!s->editing) return;
    process_input(a, s, o);
    if (!s->editing) return;               /* Esc finished it */
    render(a, s);
    if (s->follow) {
        s->follow = false;
        follow_caret(a, s, o);
    }
    /* selection */
    {
        pc_box boxes[64];
        size_t nb = pc_text_selection_boxes(s->t, boxes, 64u);
        ui_color sel = ui_pal(ui)->text_select;
        sel.a = 110;
        if (nb > 64u) nb = 64u;
        for (size_t i = 0; i < nb; i++) {
            app_ov_to_screen(o, boxes[i].x0, boxes[i].y0, &x0, &y0);
            app_ov_to_screen(o, boxes[i].x1, boxes[i].y1, &x1, &y1);
            ui_draw_rect(ui, ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)ceil(x1 - x0),
                                          (int32_t)ceil(y1 - y0)), sel);
        }
    }
    pc_text_caret_box(s->t, pc_text_caret(s->t), &cb);
    app_ov_to_screen(o, cb.x0, cb.y0, &x0, &y0);
    app_ov_to_screen(o, cb.x1, cb.y1, &x1, &y1);
    if (s->comp[0]) {
        /* IME composition: drawn at the caret, underlined, not yet in the image */
        float fs = (float)(y1 - y0) * 0.75f, w;
        ui_font *f = ui_font_regular(ui);
        if (fs < ui_font_px(ui)) fs = ui_font_px(ui);
        w = ui_text_width(f, fs, s->comp, strlen(s->comp));
        ui_draw_rect(ui, ui_rect_make((int32_t)x0, (int32_t)y0, (int32_t)ceilf(w) + 4,
                                      (int32_t)(y1 - y0)), ui_rgba(255, 255, 255, 230));
        (void)ui_draw_text(ui, f, fs, (float)x0 + 2.0f, (float)y0 + fs, ui_rgba(0, 0, 0, 255),
                           s->comp, strlen(s->comp));
        ui_draw_rect(ui, ui_rect_make((int32_t)x0 + 2, (int32_t)y1 - 3, (int32_t)ceilf(w), 2),
                     ui_rgba(0, 0, 0, 255));
    } else {
        /* blinking caret, black under white so it shows on any color */
        uint64_t blink = (uint64_t)ui_get_theme(ui)->m.caret_blink_ms;
        uint64_t t = app_now_ms(a) - s->blink_t0;
        if (blink == 0u || (t / blink) % 2u == 0u) {
            ui_draw_rect(ui, ui_rect_make((int32_t)x0 - 1, (int32_t)y0, 3, (int32_t)(y1 - y0)),
                         ui_rgba(255, 255, 255, 220));
            ui_draw_rect(ui, ui_rect_make((int32_t)x0, (int32_t)y0, 1, (int32_t)(y1 - y0)),
                         ui_rgba(0, 0, 0, 255));
        }
        if (blink) app_request_frame_at(a, app_now_ms(a) + blink - t % blink);
    }
    {
        pc_handle_metrics m = metrics(a);
        pc_pt h = pc_text_handle_pos(s->t, m.handle_offset);
        vec_ov_move_handle(o, h.x, h.y, vec_pulse(a));
    }
}

static app_cursor text_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    text_state *s = (text_state *)st;
    pc_handle_metrics m;
    (void)mods;
    if (s->drag == TDRAG_HANDLE) return APP_CURSOR_MOVE;
    if (!s->editing) return APP_CURSOR_TEXT;
    m = metrics(a);
    return pc_text_hit_test(s->t, pc_pt_make(x, y), &m) == PC_TEXT_PART_HANDLE ? APP_CURSOR_MOVE
                                                                              : APP_CURSOR_TEXT;
}

static bool text_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    text_state *s = (text_state *)st;
    (void)mods;
    if (!down) return false;
    validate(a, s);
    /* keys not taken during the frame (text input off, focus elsewhere) */
    if (!s->editing || ui_focus_id(app_ui(a)) != 0) return false;
    if (key == SDLK_ESCAPE) {
        (void)finish(a, s);
        return true;
    }
    return false;
}

static bool text_live(app *a, void *st)
{
    text_state *s = (text_state *)st;
    validate(a, s);
    return s->editing;
}

static bool text_commit(app *a, void *st) { return finish(a, (text_state *)st); }

static void text_init(app *a, void *st)
{
    load_options(a, (text_state *)st);
}

static void text_activate(app *a, void *st)
{
    text_state *s = (text_state *)st;
    load_options(a, s);
    (void)text_fonts_get(a);      /* starts looking for installed fonts */
}

static void text_deactivate(app *a, void *st) { (void)finish(a, (text_state *)st); }

static void text_fini(app *a, void *st)
{
    text_state *s = (text_state *)st;
    app_doc *d = edit_doc(a, s);
    if (d) app_doc_txn_cancel(a, d);
    pc_text_destroy(s->t);
    pc_vrender_destroy(s->vr);
    s->t = NULL;
    s->vr = NULL;
}

/* ---- inspection (text_tool.h) --------------------------------------------------------- */
static text_state *tool_state(app *a)
{
    const app_tool *t = app_tool_find(a, "text");
    return t ? (text_state *)app_tool_state(a, t) : NULL;
}

const pc_text *text_tool_editing(app *a)
{
    text_state *s = tool_state(a);
    if (!s) return NULL;
    validate(a, s);
    return s->editing ? s->t : NULL;
}

const char *text_tool_composition(app *a)
{
    text_state *s = tool_state(a);
    return s && s->editing ? s->comp : "";
}

const app_tool app_tool_text = {
    "text",
    "Text",
    "Click to place the text cursor and type. Esc or Finish commits; drag the handle to move.",
    'T',
    17,
    UI_ICON_TOOL_TEXT,
    APP_CURSOR_TEXT,
    APP_TOOL_PAINTS | APP_TOOL_TEXT_INPUT,
    sizeof(text_state),
    text_init,
    text_fini,
    text_activate,
    text_deactivate,
    text_pointer,
    text_key,
    NULL,
    text_options,
    text_overlay,
    text_live,
    text_commit,
    NULL,
    text_cursor,
    text_settings_changed
};
