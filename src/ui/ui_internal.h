/* ui_internal.h - context structure and internal helpers of pc_ui (L3). */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <SDL3/SDL.h>

#include "ui/ui.h"
#include "ui_font_internal.h"
#include "ui_raster.h"

/* ---- limits -------------------------------------------------------------- */
#define UI_ATLAS_DIM        1024
#define UI_ATLAS_MAX_PAGES  8
#define UI_ATLAS_SOFT_PAGES 4
#define UI_MAX_SHELVES      192
#define UI_MAX_ROOTS        48
#define UI_MAX_ROOT_STACK   16
#define UI_MAX_CLIP         64
#define UI_MAX_LAYOUT       32
#define UI_MAX_ID_STACK     64
#define UI_MAX_KEYS         64
#define UI_MAX_FOCUS        512
#define UI_MAX_POPUPS       8
#define UI_MAX_MENU_ITEMS   128
#define UI_MAX_PANELS       32
#define UI_TEXT_BUF         512
#define UI_MAX_HITS         2048

/* ---- atlas and sprite cache ---------------------------------------------- */
typedef struct ui_shelf { int32_t y, h, x; } ui_shelf;

typedef struct ui_atlas_page {
    SDL_Texture *tex;
    uint8_t     *a8;                /* UI_ATLAS_DIM x UI_ATLAS_DIM coverage */
    ui_shelf     shelves[UI_MAX_SHELVES];
    int32_t      nshelves, next_y;
    int32_t      dx0, dy0, dx1, dy1; /* dirty rectangle, empty when dx1 <= dx0 */
} ui_atlas_page;

typedef struct ui_sprite {
    uint16_t page, x, y, w, h;      /* w == 0: empty (nothing to draw) */
    int16_t  ox, oy;                /* offset of the bitmap from the pen/origin */
} ui_sprite;

typedef struct ui_cache_entry { uint64_t key; ui_sprite sp; } ui_cache_entry;
typedef struct ui_cache { ui_cache_entry *e; uint32_t cap, n; } ui_cache;

enum { UI_KEY_GLYPH = 1, UI_KEY_CORNER = 2, UI_KEY_DISC = 3, UI_KEY_RING = 4,
       UI_KEY_SHADOW = 5, UI_KEY_ICON = 6 };

/* ---- draw lists ---------------------------------------------------------- */
typedef struct ui_cmd {
    SDL_Texture *tex;
    ui_rect      clip;
    int32_t      v0, nv, i0, ni;
    ui_draw_fn   fn;                /* callback command when non-NULL */
    void        *ud;
    int32_t      filter;            /* scale mode for user textures, -1 = atlas */
} ui_cmd;

typedef struct ui_dl {
    SDL_Vertex *v;
    int32_t     nv, cv;
    int        *ix;
    int32_t     ni, ci;
    ui_cmd     *cmd;
    int32_t     nc, cc;
} ui_dl;

/* ---- roots (layers) ------------------------------------------------------ */
enum { UI_ROOT_BASE = 0, UI_ROOT_PANEL = 1, UI_ROOT_MODAL = 2, UI_ROOT_POPUP = 3,
       UI_ROOT_TOOLTIP = 4 };

typedef struct ui_root {
    ui_id    id;
    int32_t  kind;
    bool     used;
    bool     hidden;                /* measuring frame: not drawn, not hit */
    bool     prev_hidden;
    ui_rect  rect;                  /* hit area declared this frame */
    ui_rect  prev_rect;
    uint32_t frame;                 /* last frame it was declared */
    uint32_t prev_frame;
    uint32_t order;                 /* panels: z; modals and popups: open sequence */
    ui_dl    dl;
    float    alpha;                 /* lane SHELL: opacity of the whole root, 1 by default */
} ui_root;

/* ---- layout -------------------------------------------------------------- */
enum { UI_LAY_ROOT = 0, UI_LAY_PUSH, UI_LAY_CELL, UI_LAY_SCROLL, UI_LAY_GROUP };

typedef struct ui_layout {
    ui_rect  rect;                  /* content area */
    int32_t  cx, cy;                /* cursor */
    int32_t  row_y, row_h;
    int32_t  spacing;
    int32_t  ncells, cell;          /* ncells 0: column mode */
    ui_size  cells[UI_MAX_CELLS];
    int32_t  widths[UI_MAX_CELLS];
    int32_t  row_fixed_h;
    int32_t  max_x, max_y;          /* extent of placed widgets */
    ui_id    id;
    uint32_t row_index;
    int32_t  kind;
    int32_t  pad;
    bool     has_next;
    ui_rect  next;
    uint32_t children;              /* nested containers declared (stable ids) */
    ui_rect  view;                  /* scroll regions: visible area */
    ui_id    sid;                   /* scroll regions: state id */
    uint32_t sflags;
} ui_layout;

/* ---- retained widget state ----------------------------------------------- */
typedef struct ui_state {
    ui_id    id;
    uint32_t frame;
    int32_t  i[4];
    float    f[6];
    double   d[2];
    char    *text;                  /* optional owned buffer */
    size_t   text_cap;
} ui_state;

#define UI_STATE_BLOCK 128
typedef struct ui_store {
    ui_state **blocks;
    int32_t    nblocks, nused;
    int32_t   *free_list;           /* indices of released slots */
    int32_t    nfree, cfree;
    int32_t   *map;                 /* open addressing: index + 1, 0 empty */
    uint32_t   mapcap;
} ui_store;

/* ---- input --------------------------------------------------------------- */
typedef struct ui_input {
    float        mx, my;
    bool         mouse_in;
    uint32_t     down;              /* bit per UI_MOUSE_* */
    uint32_t     pressed, released; /* edges since the last frame */
    int32_t      clicks[3];
    ui_vec2      press_pos[3];
    float        wheel_x, wheel_y;
    ui_key_press keys[UI_MAX_KEYS];
    int32_t      nkeys;
    char         text[UI_TEXT_BUF];
    int32_t      ntext;
    char         comp[UI_TEXT_BUF];
    int32_t      comp_cursor, comp_len;
    bool         comp_active;
    uint32_t     mods;
} ui_input;

/* ---- popups -------------------------------------------------------------- */
typedef struct ui_popup {
    ui_id    id;
    ui_id    owner;                 /* menubar id for top-level menus */
    ui_rect  anchor;
    int32_t  placement;
    int32_t  w, h;                  /* measured size, 0 = unknown */
    ui_rect  rect;
    int32_t  nav;                   /* highlighted item or -1 */
    int32_t  nitems;
    int32_t  prev_nitems;
    uint8_t  item_flags[UI_MAX_MENU_ITEMS];  /* last frame: 1 selectable, 2 submenu */
    uint8_t  cur_flags[UI_MAX_MENU_ITEMS];
    int32_t  sub_item;              /* item that opened the child popup */
    int32_t  hover_item;
    uint64_t hover_since;
    bool     activate;              /* Enter: activate nav this frame */
    bool     open_sub;              /* Right: open the nav submenu */
    bool     keyboard;              /* opened or navigated by keyboard */
    uint32_t open_frame;
    int32_t  pref_w;                /* widest item this frame */
    int32_t  min_w;                 /* e.g. the combo box width */
    int32_t  kind;                  /* 0 menu, 1 combo, 2 custom */
    /* lane KEYS: access keys and scrolling */
    uint32_t item_mnem[UI_MAX_MENU_ITEMS];   /* last frame: access key (lower case), 0 none */
    uint32_t cur_mnem[UI_MAX_MENU_ITEMS];
    uint32_t item_first[UI_MAX_MENU_ITEMS];  /* last frame: first character (lower case) */
    uint32_t cur_first[UI_MAX_MENU_ITEMS];
    int32_t  scroll;                /* content pixels scrolled out at the top */
    ui_rect  view;                  /* visible content when taller than the window, else empty */
} ui_popup;

/* ---- text editing -------------------------------------------------------- */
typedef struct ui_edit {
    ui_id    id;
    size_t   cursor, anchor;        /* byte offsets */
    float    scroll;
    char    *orig;                  /* text when focus arrived (Escape) */
    size_t   orig_cap;
    bool     drag;
    uint64_t blink0;
} ui_edit;

/* internal interaction flag: menu bar titles stay hoverable over popups */
#define UI_INTERACT_MENUBAR 0x10000u

/* Hit list entry: every interactive widget of a frame, in declaration
 * order, so the next frame can resolve the hovered widget at the current
 * pointer position before any widget runs (the pointer may have moved and
 * pressed within one batch of events, e.g. a pen tap). */
typedef struct ui_hit {
    ui_id   id, root;
    ui_rect r;                      /* rectangle clipped like the hit test */
    uint8_t popup;                  /* root is a popup, or menu bar title */
} ui_hit;

/* ---- color wheel textures ------------------------------------------------ */
#define UI_WHEEL_CACHE 8
typedef struct ui_wheel_tex {
    ui_id        id;
    int          kind;              /* 0 disc, 1 ring, 2 saturation/value square */
    int32_t      size;
    float        hue;
    uint32_t     frame;
    SDL_Texture *tex;
} ui_wheel_tex;

/* ---- context ------------------------------------------------------------- */
typedef struct ui_pxm {
    int32_t control_h, menubar_h, menu_item_h, row_h, tab_h, title_h;
    int32_t pad, pad_small, spacing, border, scrollbar, icon, thumb, check, focus, snap;
    int32_t drag;
    float   radius, radius_large, shadow, font, font_small, font_title;
} ui_pxm;

struct ui_ctx {
    SDL_Renderer  *r;
    SDL_Window    *win;
    ui_theme       theme;
    ui_font       *font_reg, *font_bold, *own_reg, *own_bold;
    float          zoom;
    ui_frame_info  fi;
    float          scale;
    uint32_t       frame;
    uint64_t       now;
    bool           in_frame;
    ui_pxm         px;

    ui_input       in;              /* accumulated by ui_event */
    ui_input       fin;             /* snapshot used by the current frame */
    float          pmx, pmy;        /* pointer position of the previous frame */
    bool           press_taken[3];
    bool           active_seen;
    uint64_t       active_t0;
    uint32_t       repeat_n;

    ui_id          id_stack[UI_MAX_ID_STACK];
    int32_t        id_depth;

    ui_hit         hits[2][UI_MAX_HITS];
    int32_t        nhits[2];
    int32_t        hit_cur;         /* list being recorded this frame */
    ui_id          hover_cand;      /* last hovered widget declared this frame */
    ui_rect        hover_cand_rect;
    ui_id          hot;             /* previous frame's final hover candidate */
    ui_rect        hot_rect;
    uint64_t       hot_since;
    ui_id          active;
    ui_id          active_root;
    ui_id          focus;
    ui_id          focus_root;
    bool           focus_visible;
    bool           focus_seen;
    bool           focus_scroll;    /* Tab moved focus: scroll it into view */
    int32_t        focus_move;      /* Tab requested: +1 / -1 */
    ui_id          focus_list[UI_MAX_FOCUS];
    ui_id          focus_roots[UI_MAX_FOCUS];
    int32_t        nfocus;
    ui_id          prev_focus_list[UI_MAX_FOCUS];
    ui_id          prev_focus_roots[UI_MAX_FOCUS];
    int32_t        prev_nfocus;
    ui_id          autofocus_root;  /* focus the first widget of this root */
    /* lane SHELL (O-UI-FOCUS): the first enabled text or numeric field of
     * the autofocus root this frame and last frame; autofocus prefers it */
    ui_id          autofocus_edit, prev_autofocus_edit;

    ui_id          last_id;
    ui_rect        last_rect;
    bool           last_hovered;
    bool           last_right_clicked;

    ui_root        roots[UI_MAX_ROOTS];
    int32_t        root_stack[UI_MAX_ROOT_STACK];
    int32_t        root_depth;
    int32_t        cur_root;
    ui_id          hover_root;
    ui_id          top_modal;       /* topmost modal declared last frame */
    uint32_t       open_seq;

    ui_rect        clip[UI_MAX_CLIP];
    int32_t        clip_depth;

    ui_layout      lay[UI_MAX_LAYOUT];
    int32_t        lay_depth;

    ui_popup       popups[UI_MAX_POPUPS];
    int32_t        npopups;
    int32_t        popup_depth;     /* popups being declared (nesting) */
    ui_id          popup_dismissed; /* popup closed by an outside press this frame */
    ui_rect        popup_dismiss_anchor;

    /* menu bar being declared */
    ui_id          mb_id;
    ui_rect        mb_rect;
    int32_t        mb_x;
    bool           mb_active;
    ui_id          mb_titles[32];
    ui_rect        mb_title_rects[32];
    int32_t        mb_n;
    int32_t        mb_switch;       /* -1 / +1 requested by Left/Right */
    /* lane KEYS: menu keyboard (ui_menu_mnemonics and friends, ui.h) */
    uint32_t       mb_mnem[32];     /* access key per title (lower case), 0 none */
    bool           mnem_parse;      /* '&' marks access keys in menu labels */
    bool           mnem_session;    /* keyboard menu session: access keys underlined */
    bool           mb_focus;        /* menu bar has keyboard focus, no menu open */
    int32_t        mb_focus_i;      /* the focused title */
    bool           alt_armed;       /* Alt went down alone and nothing else happened yet */
    int32_t        alt_taps;        /* lone Alt presses since the last frame (ui_event) */
    bool           alt_tap;         /* a lone Alt press arrived for this frame */
    char           open_req[64];    /* ui_open_request id, "" = none */
    uint32_t       open_req_frame;

    char           tip[256];
    ui_vec2        tip_pos;
    bool           tip_show;
    ui_id          tip_block;       /* tooltip suppressed for this hot id */

    ui_edit        edit;

    ui_cursor      cursor;
    ui_cursor      applied_cursor;
    bool           auto_cursor;
    SDL_Cursor    *sys_cursor[UI_CURSOR_COUNT];

    bool           want_frame;
    uint64_t       wake_at;

    ui_atlas_page  pages[UI_ATLAS_MAX_PAGES];
    int32_t        npages;
    bool           atlas_overflow;
    bool           atlas_reupload;
    ui_cache       cache;
    ui_path        scratch_path;
    ui_path        scratch_stroke;
    SDL_Vertex    *fade_v;          /* lane SHELL: vertices of a translucent root */
    int32_t        fade_cap;
    uint8_t       *scratch;         /* coverage scratch for sprites */
    uint8_t        gamma[256];      /* glyph coverage curve */
    size_t         scratch_cap;

    ui_wheel_tex   wheels[UI_WHEEL_CACHE];
    SDL_Texture   *checker_tex[4];
    ui_color       checker_col[4][2];
    int32_t        checker_next;

    ui_store       store;

    bool           text_input_on;
    bool           text_input_want;
    ui_rect        text_input_rect;

    ui_rect        panel_area;
    bool           panel_area_set;
    ui_id          panel_z[UI_MAX_PANELS];
    int32_t        npanel_z;
    ui_id          panel_raise;     /* raise this panel at the next frame */

    char          *clip_text;       /* clipboard fallback without video */

    /* dialog being declared */
    ui_id          edit_submit_root; /* root of a field that took Enter this frame */
    ui_id          dlg_id;
    uint32_t       dlg_result;
    uint32_t       dlg_buttons_def;
};

/* ---- helpers shared between files ---------------------------------------- */
static inline ui_color ui_dim(ui_color c, bool disabled)
{
    return disabled ? ui_color_fade(c, 0.45f) : c;
}
static inline float ui_minf(float a, float b) { return a < b ? a : b; }
static inline float ui_maxf(float a, float b) { return a > b ? a : b; }
static inline int32_t ui_mini(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t ui_maxi(int32_t a, int32_t b) { return a > b ? a : b; }
static inline float ui_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline double ui_clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ui_atlas.c */
bool     ui_atlas_init(ui_ctx *ctx);
void     ui_atlas_destroy(ui_ctx *ctx);
void     ui_atlas_frame(ui_ctx *ctx);             /* reset after overflow */
/* Reserve w x h coverage texels; *dst receives the write pointer (stride
 * UI_ATLAS_DIM, zeroed). False on overflow (nothing drawn this frame). */
bool     ui_atlas_alloc(ui_ctx *ctx, int32_t w, int32_t h, ui_sprite *sp, uint8_t **dst);
void     ui_atlas_upload(ui_ctx *ctx);
bool     ui_cache_get(ui_ctx *ctx, uint64_t key, ui_sprite *sp);
void     ui_cache_put(ui_ctx *ctx, uint64_t key, const ui_sprite *sp);
uint8_t *ui_scratch(ui_ctx *ctx, size_t n);       /* zeroed scratch buffer */

/* ui_draw.c */
void ui_gamma_init(uint8_t table[256]);
void ui_draw_elevation(ui_ctx *ctx, ui_rect r, float radius, int level);
void ui_dl_reset(ui_dl *dl);
void ui_dl_free(ui_dl *dl);
void ui_draw_sprite(ui_ctx *ctx, const ui_sprite *sp, float x, float y, ui_color c);
/* Sprite with flipped texture coordinates (corner reuse). */
void ui_draw_sprite_flip(ui_ctx *ctx, const ui_sprite *sp, float x, float y, bool fx, bool fy,
                         ui_color c);
void ui_replay_root(ui_ctx *ctx, ui_root *root);
/* Glyph sprite for (face, size, gid, quarter-pixel phase), rasterizing on
 * first use. False when the glyph is empty or the atlas is full. */
bool ui_glyph_sprite(ui_ctx *ctx, const ui_font *face, float size, uint32_t gid, int sub,
                     ui_sprite *sp);

/* ui_icons.c */
bool ui_icon_sprites(ui_ctx *ctx, ui_icon icon, int32_t size, ui_sprite sp[UI_ICON_LAYERS]);

/* ui_core.c */
ui_state *ui_state_get(ui_ctx *ctx, ui_id id);    /* creates; NULL on OOM */
ui_state *ui_state_find(ui_ctx *ctx, ui_id id);
bool      ui_state_text(ui_state *st, size_t cap);/* ensure st->text capacity */
void      ui_store_free(ui_store *s);
int32_t   ui_root_begin(ui_ctx *ctx, ui_id id, int32_t kind, ui_rect hit, bool hidden);
void      ui_root_end(ui_ctx *ctx);
ui_root  *ui_root_cur(ui_ctx *ctx);
ui_root  *ui_root_find(ui_ctx *ctx, ui_id id);
bool      ui_root_hovered(const ui_ctx *ctx);
void      ui_clip_push_raw(ui_ctx *ctx, ui_rect r);
bool      ui_mouse_pressed(const ui_ctx *ctx, int b);
bool      ui_mouse_released(const ui_ctx *ctx, int b);
bool      ui_mouse_in(const ui_ctx *ctx, ui_rect r);   /* inside r and the clip */
void      ui_focus_register(ui_ctx *ctx, ui_id id);
/* Give id the keyboard focus from inside the current root (composite
 * widgets whose parts route focus to the container). */
void      ui_focus_take(ui_ctx *ctx, ui_id id);
void      ui_draw_focus_ring(ui_ctx *ctx, ui_rect r, float radius);
const char *ui_label_text(const char *label, size_t *len);
int32_t   ui_text_baseline(const ui_ctx *ctx, ui_font *f, float size, ui_rect r);
void      ui_text_input_request(ui_ctx *ctx, ui_rect caret);
void      ui_set_clipboard(ui_ctx *ctx, const char *s);
char     *ui_get_clipboard(ui_ctx *ctx);          /* malloc'ed or NULL */
uint64_t  ui_now(const ui_ctx *ctx);

/* ui_layout.c */
void ui_layout_root(ui_ctx *ctx, ui_rect r, int32_t pad, int32_t kind, ui_id id);
void ui_layout_close(ui_ctx *ctx);                /* pop without committing */
ui_layout *ui_layout_top(ui_ctx *ctx);
void ui_layout_extend(ui_ctx *ctx, ui_rect r);    /* grow the content extent */
int32_t ui_layout_avail_w(const ui_ctx *ctx);     /* width of the next cell */
/* ui_layout_next for fixed-size widgets: in column mode they keep their
 * natural width (left aligned) instead of spanning the container. */
ui_rect ui_layout_next_natural(ui_ctx *ctx, int32_t pref_w, int32_t pref_h);

/* ui_popup.c */
void ui_popups_frame_begin(ui_ctx *ctx);
void ui_popups_frame_end(ui_ctx *ctx);
bool ui_popup_any_open(const ui_ctx *ctx);
ui_popup *ui_popup_current(ui_ctx *ctx);
void ui_popup_close_all(ui_ctx *ctx);
/* Selectable row in the current popup (menus, combo lists). Returns true
 * when chosen. kind: 0 plain, 1 check, 2 radio. */
bool ui_popup_item(ui_ctx *ctx, ui_icon icon, const char *label, const char *shortcut,
                   bool enabled, int kind, bool marked, bool submenu, bool *sub_open,
                   ui_rect *row);

/* ui_window.c */
void ui_panels_frame_begin(ui_ctx *ctx);
void ui_tooltip_draw(ui_ctx *ctx);

/* ui_widgets.c helpers */
void ui_draw_button_face(ui_ctx *ctx, ui_rect r, uint32_t flags, bool hovered, bool held,
                         bool disabled);
/* Scrollbar on track; *scroll in [0, content - view]. Returns true when
 * scrolling changed. */
bool ui_scrollbar(ui_ctx *ctx, ui_id id, ui_rect track, float content, float view,
                  float *scroll, bool horizontal);
/* ui_textfield.c: shared editor used by numeric fields. */
uint32_t ui_edit_field(ui_ctx *ctx, ui_id id, ui_rect r, char *buf, size_t cap, uint32_t flags,
                       const char *placeholder, int align);

#endif /* UI_INTERNAL_H */
