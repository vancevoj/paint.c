/* ui_core.c - context, input, frames, ids, roots, state and interaction. */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UI_BASE_ROOT_ID 0x0B45E001u
#define UI_TIP_ROOT_ID  0x0717F001u
#define UI_GC_AGE       300u

/* ---- context lifetime ---------------------------------------------------- */
static void compute_px(ui_ctx *ctx)
{
    const ui_metrics *m = &ctx->theme.m;
    ui_pxm *p = &ctx->px;
    p->control_h = ui_px(ctx, m->control_h);
    p->menubar_h = ui_px(ctx, m->menubar_h);
    p->menu_item_h = ui_px(ctx, m->menu_item_h);
    p->row_h = ui_px(ctx, m->row_h);
    p->tab_h = ui_px(ctx, m->tab_h);
    p->title_h = ui_px(ctx, m->title_h);
    p->pad = ui_px(ctx, m->pad);
    p->pad_small = ui_px(ctx, m->pad_small);
    p->spacing = ui_px(ctx, m->spacing);
    p->border = ui_px_line(ctx, m->border);
    p->scrollbar = ui_px(ctx, m->scrollbar);
    p->icon = ui_px(ctx, m->icon);
    p->thumb = ui_px(ctx, m->slider_thumb);
    p->check = ui_px(ctx, m->check);
    p->focus = ui_px_line(ctx, m->focus);
    p->snap = ui_px(ctx, m->snap);
    p->drag = ui_px(ctx, m->drag_threshold);
    p->radius = (float)ui_px(ctx, m->radius);
    p->radius_large = (float)ui_px(ctx, m->radius_large);
    p->shadow = m->shadow * ctx->scale;
    p->font = floorf(m->font_size * ctx->scale * 4.0f + 0.5f) * 0.25f;
    p->font_small = floorf(m->font_size_small * ctx->scale * 4.0f + 0.5f) * 0.25f;
    p->font_title = floorf(m->font_size_title * ctx->scale * 4.0f + 0.5f) * 0.25f;
}

ui_ctx *ui_create(SDL_Renderer *r, SDL_Window *w)
{
    ui_ctx *ctx;
    if (!r) return NULL;
    ctx = (ui_ctx *)calloc(1u, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->r = r;
    ctx->win = w;
    ctx->zoom = 1.0f;
    ctx->scale = 1.0f;
    ctx->auto_cursor = w != NULL;
    ctx->applied_cursor = UI_CURSOR_COUNT;
    ui_theme_init(&ctx->theme, UI_THEME_LIGHT, ui_theme_default_accent());
    ctx->own_reg = ui_font_load_builtin(UI_FONT_REGULAR);
    ctx->own_bold = ui_font_load_builtin(UI_FONT_SEMIBOLD);
    ctx->font_reg = ctx->own_reg;
    ctx->font_bold = ctx->own_bold;
    ui_path_init(&ctx->scratch_path, 0.1f);
    ui_path_init(&ctx->scratch_stroke, 0.1f);
    ctx->in.mx = ctx->in.my = -1e6f;
    ctx->fin = ctx->in;
    ctx->fi.px_per_point = 1.0f;
    ctx->fi.scale = 1.0f;
    if (!ctx->own_reg || !ctx->own_bold || !ui_atlas_init(ctx)) {
        ui_destroy(ctx);
        return NULL;
    }
    compute_px(ctx);
    ctx->want_frame = true;
    ctx->wake_at = UINT64_MAX;
    return ctx;
}

void ui_destroy(ui_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->win && ctx->text_input_on) SDL_StopTextInput(ctx->win);
    for (int i = 0; i < UI_MAX_ROOTS; i++) ui_dl_free(&ctx->roots[i].dl);
    for (int i = 0; i < 4; i++)
        if (ctx->checker_tex[i]) SDL_DestroyTexture(ctx->checker_tex[i]);
    for (int i = 0; i < UI_WHEEL_CACHE; i++)
        if (ctx->wheels[i].tex) SDL_DestroyTexture(ctx->wheels[i].tex);
    for (int i = 0; i < UI_CURSOR_COUNT; i++)
        if (ctx->sys_cursor[i]) SDL_DestroyCursor(ctx->sys_cursor[i]);
    ui_atlas_destroy(ctx);
    ui_path_free(&ctx->scratch_path);
    ui_path_free(&ctx->scratch_stroke);
    ui_store_free(&ctx->store);
    free(ctx->edit.orig);
    free(ctx->clip_text);
    ui_font_free(ctx->own_reg);
    ui_font_free(ctx->own_bold);
    free(ctx);
}

void ui_set_theme(ui_ctx *ctx, const ui_theme *t)
{
    ctx->theme = *t;
    compute_px(ctx);
    ctx->want_frame = true;
}

const ui_theme *ui_get_theme(const ui_ctx *ctx) { return &ctx->theme; }
const ui_palette *ui_pal(const ui_ctx *ctx) { return &ctx->theme.pal; }

void ui_set_fonts(ui_ctx *ctx, ui_font *regular, ui_font *semibold)
{
    ctx->font_reg = regular ? regular : ctx->own_reg;
    ctx->font_bold = semibold ? semibold : ctx->own_bold;
    ctx->want_frame = true;
}

ui_font *ui_font_regular(const ui_ctx *ctx) { return ctx->font_reg; }
ui_font *ui_font_semibold(const ui_ctx *ctx) { return ctx->font_bold; }

void ui_set_zoom(ui_ctx *ctx, float zoom)
{
    ctx->zoom = ui_clampf(zoom, 0.5f, 4.0f);
    ctx->want_frame = true;
}

void ui_frame_info_auto(ui_ctx *ctx, ui_frame_info *fi)
{
    int w = 0, h = 0;
    float ds = 1.0f, pd = 1.0f;
    SDL_GetRenderOutputSize(ctx->r, &w, &h);
    if (ctx->win) {
        ds = SDL_GetWindowDisplayScale(ctx->win);
        pd = SDL_GetWindowPixelDensity(ctx->win);
        if (!(ds > 0.0f)) ds = 1.0f;
        if (!(pd > 0.0f)) pd = 1.0f;
    }
    fi->width = w;
    fi->height = h;
    fi->scale = ds * ctx->zoom;
    fi->px_per_point = pd;
    fi->time_ms = SDL_GetTicks();
}

/* ---- scale helpers ------------------------------------------------------- */
float ui_scale(const ui_ctx *ctx) { return ctx->scale; }
int32_t ui_px(const ui_ctx *ctx, float dip) { return (int32_t)floorf(dip * ctx->scale + 0.5f); }
int32_t ui_px_line(const ui_ctx *ctx, float dip)
{
    int32_t v = (int32_t)floorf(dip * ctx->scale + 0.25f);
    return v < 1 ? 1 : v;
}
float ui_font_px(const ui_ctx *ctx) { return ctx->px.font; }
uint64_t ui_time_ms(const ui_ctx *ctx) { return ctx->now; }
uint64_t ui_now(const ui_ctx *ctx) { return ctx->now; }
uint32_t ui_frame_count(const ui_ctx *ctx) { return ctx->frame; }

/* ---- events -------------------------------------------------------------- */
uint32_t ui_mod_primary(void)
{
#if defined(__APPLE__)
    return UI_MOD_GUI;
#else
    return UI_MOD_CTRL;
#endif
}

static uint32_t norm_mods(SDL_Keymod m)
{
    uint32_t r = 0;
    if (m & SDL_KMOD_CTRL) r |= UI_MOD_CTRL;
    if (m & SDL_KMOD_SHIFT) r |= UI_MOD_SHIFT;
    if (m & SDL_KMOD_ALT) r |= UI_MOD_ALT;
    if (m & SDL_KMOD_GUI) r |= UI_MOD_GUI;
    return r;
}

static int map_button(Uint8 b)
{
    if (b == SDL_BUTTON_LEFT) return UI_MOUSE_LEFT;
    if (b == SDL_BUTTON_RIGHT) return UI_MOUSE_RIGHT;
    if (b == SDL_BUTTON_MIDDLE) return UI_MOUSE_MIDDLE;
    return -1;
}

static void set_pos(ui_ctx *ctx, float x, float y)
{
    float k = ctx->fi.px_per_point > 0.0f ? ctx->fi.px_per_point : 1.0f;
    ctx->in.mx = x * k;
    ctx->in.my = y * k;
    ctx->in.mouse_in = true;
}

static void button(ui_ctx *ctx, int b, bool down, int clicks)
{
    if (b < 0) return;
    if (down) {
        ctx->in.down |= 1u << b;
        ctx->in.pressed |= 1u << b;
        ctx->in.clicks[b] = clicks;
        ctx->in.press_pos[b] = ui_vec2_make(ctx->in.mx, ctx->in.my);
    } else {
        ctx->in.down &= ~(1u << b);
        ctx->in.released |= 1u << b;
    }
}

static bool pen_as_mouse(void) { return SDL_GetHintBoolean(SDL_HINT_PEN_MOUSE_EVENTS, true); }

bool ui_event(ui_ctx *ctx, const SDL_Event *e)
{
    bool kb = false, mouse = false;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        set_pos(ctx, e->motion.x, e->motion.y);
        mouse = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        set_pos(ctx, e->button.x, e->button.y);
        button(ctx, map_button(e->button.button), e->type == SDL_EVENT_MOUSE_BUTTON_DOWN,
               e->button.clicks);
        mouse = true;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float x = e->wheel.x, y = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { x = -x; y = -y; }
        ctx->in.wheel_x += x;
        ctx->in.wheel_y += y;
        mouse = true;
        break;
    }
    case SDL_EVENT_PEN_MOTION:
        if (pen_as_mouse()) return false;
        set_pos(ctx, e->pmotion.x, e->pmotion.y);
        mouse = true;
        break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        if (pen_as_mouse()) return false;
        set_pos(ctx, e->ptouch.x, e->ptouch.y);
        button(ctx, UI_MOUSE_LEFT, e->type == SDL_EVENT_PEN_DOWN, 1);
        mouse = true;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        ctx->in.mouse_in = false;
        ctx->in.mx = ctx->in.my = -1e6f;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (ctx->in.down) ctx->in.released |= ctx->in.down;
        ctx->in.down = 0;
        ctx->in.mods = 0;
        break;
    case SDL_EVENT_KEY_DOWN:
        ctx->in.mods = norm_mods(e->key.mod);
        if (ctx->in.nkeys < UI_MAX_KEYS) {
            ui_key_press *k = &ctx->in.keys[ctx->in.nkeys++];
            k->key = (int32_t)e->key.key;
            k->mods = norm_mods(e->key.mod);
            k->repeat = e->key.repeat;
            k->used = false;
        }
        kb = true;
        break;
    case SDL_EVENT_KEY_UP:
        ctx->in.mods = norm_mods(e->key.mod);
        kb = true;
        break;
    case SDL_EVENT_TEXT_INPUT: {
        size_t n = e->text.text ? strlen(e->text.text) : 0;
        size_t room = sizeof ctx->in.text - 1u - (size_t)ctx->in.ntext;
        if (n > room) n = ui_utf8_floor(e->text.text, n, room);
        memcpy(ctx->in.text + ctx->in.ntext, e->text.text, n);
        ctx->in.ntext += (int32_t)n;
        ctx->in.text[ctx->in.ntext] = '\0';
        ctx->in.comp[0] = '\0';
        ctx->in.comp_active = false;
        kb = true;
        break;
    }
    case SDL_EVENT_TEXT_EDITING: {
        size_t n = e->edit.text ? strlen(e->edit.text) : 0;
        if (n > sizeof ctx->in.comp - 1u)
            n = ui_utf8_floor(e->edit.text, n, sizeof ctx->in.comp - 1u);
        if (n) memcpy(ctx->in.comp, e->edit.text, n);
        ctx->in.comp[n] = '\0';
        ctx->in.comp_active = n > 0;
        ctx->in.comp_cursor = e->edit.start;
        ctx->in.comp_len = e->edit.length;
        kb = true;
        break;
    }
    case SDL_EVENT_RENDER_DEVICE_RESET:
        ctx->atlas_reupload = true;
        break;
    default:
        break;
    }
    ctx->want_frame = true;
    if (mouse) return ui_wants_mouse(ctx);
    if (kb) return ui_wants_keyboard(ctx);
    return false;
}

/* ---- redraw scheduling --------------------------------------------------- */
void ui_request_frame(ui_ctx *ctx) { ctx->want_frame = true; }

void ui_request_frame_at(ui_ctx *ctx, uint64_t t)
{
    if (t <= ctx->now) { ctx->want_frame = true; return; }
    if (t < ctx->wake_at) ctx->wake_at = t;
}

bool ui_needs_frame(const ui_ctx *ctx, uint64_t now)
{
    return ctx->want_frame || now >= ctx->wake_at;
}

int32_t ui_wait_timeout(const ui_ctx *ctx, uint64_t now)
{
    uint64_t d;
    if (ctx->want_frame) return 0;
    if (ctx->wake_at == UINT64_MAX) return -1;
    if (now >= ctx->wake_at) return 0;
    d = ctx->wake_at - now;
    return d > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)d;
}

bool ui_wants_mouse(const ui_ctx *ctx)
{
    if (ctx->active) return true;
    if (ctx->hover_root && ctx->hover_root != UI_BASE_ROOT_ID) return true;
    return ctx->hot != 0;
}

bool ui_wants_keyboard(const ui_ctx *ctx)
{
    return ctx->focus != 0 || ctx->npopups > 0 || ctx->top_modal != 0;
}

bool ui_text_input_active(const ui_ctx *ctx) { return ctx->text_input_on; }

/* ---- ids ----------------------------------------------------------------- */
static ui_id seed(const ui_ctx *ctx)
{ return ctx->id_depth ? ctx->id_stack[ctx->id_depth - 1] : 0; }

ui_id ui_get_id(ui_ctx *ctx, const char *label)
{ return ui_hash(label ? label : "", -1, seed(ctx)); }

ui_id ui_get_id_int(ui_ctx *ctx, int64_t n) { return ui_hash(&n, (ptrdiff_t)sizeof n, seed(ctx)); }

static void push_raw(ui_ctx *ctx, ui_id id)
{
    if (ctx->id_depth < UI_MAX_ID_STACK) ctx->id_stack[ctx->id_depth++] = id;
}

void ui_push_id(ui_ctx *ctx, const char *s) { push_raw(ctx, ui_get_id(ctx, s)); }
void ui_push_id_int(ui_ctx *ctx, int64_t n) { push_raw(ctx, ui_get_id_int(ctx, n)); }
void ui_push_id_ptr(ui_ctx *ctx, const void *p)
{
    uintptr_t v = (uintptr_t)p;
    push_raw(ctx, ui_hash(&v, (ptrdiff_t)sizeof v, seed(ctx)));
}
void ui_pop_id(ui_ctx *ctx)
{
    if (ctx->id_depth > 0) ctx->id_depth--;
}

const char *ui_label_end(const char *label)
{
    const char *p = label;
    if (!p) return NULL;
    while (*p && !(p[0] == '#' && p[1] == '#')) p++;
    return p;
}

const char *ui_label_text(const char *label, size_t *len)
{
    const char *e = ui_label_end(label ? label : "");
    *len = (size_t)(e - (label ? label : e));
    return label ? label : "";
}

/* ---- retained state ------------------------------------------------------ */
static ui_state *slot_at(ui_store *s, int32_t i)
{
    return &s->blocks[i / UI_STATE_BLOCK][i % UI_STATE_BLOCK];
}

static uint32_t id_hash(ui_id id) { return (id * 2654435761u) ^ (id >> 16); }

static bool map_rebuild(ui_store *s, uint32_t cap)
{
    int32_t *m = (int32_t *)calloc(cap, sizeof(int32_t));
    if (!m) return false;
    for (int32_t i = 0; i < s->nused; i++) {
        ui_state *st = slot_at(s, i);
        uint32_t h;
        if (!st->id) continue;
        h = id_hash(st->id) & (cap - 1u);
        while (m[h]) h = (h + 1u) & (cap - 1u);
        m[h] = i + 1;
    }
    free(s->map);
    s->map = m;
    s->mapcap = cap;
    return true;
}

ui_state *ui_state_find(ui_ctx *ctx, ui_id id)
{
    ui_store *s = &ctx->store;
    uint32_t h;
    if (!s->mapcap || !id) return NULL;
    h = id_hash(id) & (s->mapcap - 1u);
    while (s->map[h]) {
        ui_state *st = slot_at(s, s->map[h] - 1);
        if (st->id == id) { st->frame = ctx->frame; return st; }
        h = (h + 1u) & (s->mapcap - 1u);
    }
    return NULL;
}

ui_state *ui_state_get(ui_ctx *ctx, ui_id id)
{
    ui_store *s = &ctx->store;
    ui_state *st = ui_state_find(ctx, id);
    int32_t idx;
    uint32_t h;
    if (st || !id) return st;
    if (s->nfree > 0) {
        idx = s->free_list[--s->nfree];
    } else {
        if (s->nused == s->nblocks * UI_STATE_BLOCK) {
            ui_state **nb = (ui_state **)realloc(s->blocks, (size_t)(s->nblocks + 1) * sizeof *nb);
            if (!nb) return NULL;
            s->blocks = nb;
            nb[s->nblocks] = (ui_state *)calloc(UI_STATE_BLOCK, sizeof(ui_state));
            if (!nb[s->nblocks]) return NULL;
            s->nblocks++;
        }
        idx = s->nused++;
    }
    st = slot_at(s, idx);
    memset(st, 0, sizeof *st);
    st->id = id;
    st->frame = ctx->frame;
    if ((uint32_t)(s->nused + 1) * 2u > s->mapcap) {
        if (!map_rebuild(s, s->mapcap ? s->mapcap * 2u : 256u)) { st->id = 0; return NULL; }
        return st;
    }
    h = id_hash(id) & (s->mapcap - 1u);
    while (s->map[h]) h = (h + 1u) & (s->mapcap - 1u);
    s->map[h] = idx + 1;
    return st;
}

bool ui_state_text(ui_state *st, size_t cap)
{
    char *n;
    if (st->text && st->text_cap >= cap) return true;
    n = (char *)realloc(st->text, cap);
    if (!n) return false;
    if (!st->text) n[0] = '\0';
    st->text = n;
    st->text_cap = cap;
    return true;
}

static void store_gc(ui_ctx *ctx)
{
    ui_store *s = &ctx->store;
    bool changed = false;
    for (int32_t i = 0; i < s->nused; i++) {
        ui_state *st = slot_at(s, i);
        if (!st->id || st->frame + UI_GC_AGE > ctx->frame) continue;
        free(st->text);
        memset(st, 0, sizeof *st);
        if (s->nfree == s->cfree) {
            int32_t nc = s->cfree ? s->cfree * 2 : 64;
            int32_t *nf = (int32_t *)realloc(s->free_list, (size_t)nc * sizeof(int32_t));
            if (!nf) break;
            s->free_list = nf;
            s->cfree = nc;
        }
        s->free_list[s->nfree++] = i;
        changed = true;
    }
    if (changed) (void)map_rebuild(s, s->mapcap);
}

void ui_store_free(ui_store *s)
{
    for (int32_t b = 0; b < s->nblocks; b++) {
        for (int32_t i = 0; i < UI_STATE_BLOCK; i++) free(s->blocks[b][i].text);
        free(s->blocks[b]);
    }
    free(s->blocks);
    free(s->free_list);
    free(s->map);
    memset(s, 0, sizeof *s);
}

/* ---- roots --------------------------------------------------------------- */
ui_root *ui_root_find(ui_ctx *ctx, ui_id id)
{
    for (int i = 0; i < UI_MAX_ROOTS; i++)
        if (ctx->roots[i].used && ctx->roots[i].id == id) return &ctx->roots[i];
    return NULL;
}

ui_root *ui_root_cur(ui_ctx *ctx) { return &ctx->roots[ctx->cur_root]; }

int32_t ui_root_begin(ui_ctx *ctx, ui_id id, int32_t kind, ui_rect hit, bool hidden)
{
    int32_t slot = -1;
    ui_root *r;
    for (int i = 0; i < UI_MAX_ROOTS; i++)
        if (ctx->roots[i].used && ctx->roots[i].id == id) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < UI_MAX_ROOTS; i++)
            if (!ctx->roots[i].used || ctx->roots[i].frame + 2u < ctx->frame) { slot = i; break; }
        if (slot < 0) slot = UI_MAX_ROOTS - 1;
        r = &ctx->roots[slot];
        r->used = true;
        r->id = id;
        r->prev_rect = ui_rect_make(0, 0, 0, 0);
        r->prev_frame = 0;
        r->frame = 0;
        ui_dl_reset(&r->dl);
    }
    r = &ctx->roots[slot];
    if (r->frame != ctx->frame) {
        if (r->frame + 1u != ctx->frame) r->order = ++ctx->open_seq;   /* newly shown */
        ui_dl_reset(&r->dl);
    }
    r->kind = kind;
    r->frame = ctx->frame;
    r->rect = hit;
    r->hidden = hidden;
    if (ctx->root_depth < UI_MAX_ROOT_STACK) ctx->root_stack[ctx->root_depth++] = ctx->cur_root;
    ctx->cur_root = slot;
    ui_clip_push_raw(ctx, ui_rect_make(0, 0, ctx->fi.width, ctx->fi.height));
    return slot;
}

void ui_root_end(ui_ctx *ctx)
{
    if (ctx->clip_depth > 0) ctx->clip_depth--;
    if (ctx->root_depth > 0) ctx->cur_root = ctx->root_stack[--ctx->root_depth];
}

bool ui_root_hovered(const ui_ctx *ctx)
{
    const ui_root *r = &ctx->roots[ctx->cur_root];
    return r->id == ctx->hover_root && !r->hidden;
}

static int root_rank(const ui_ctx *ctx, const ui_root *r)
{
    switch (r->kind) {
    case UI_ROOT_BASE: return 0;
    case UI_ROOT_PANEL:
        for (int32_t i = 0; i < ctx->npanel_z; i++)
            if (ctx->panel_z[i] == r->id) return 1000 + i;
        return 1000 + UI_MAX_PANELS;
    case UI_ROOT_MODAL: return 2000;
    case UI_ROOT_POPUP: return 3000;
    default: return 4000;
    }
}

/* Roots declared in frame f, bottom to top. Returns the count. */
static int sorted_roots(const ui_ctx *ctx, uint32_t f, int out[UI_MAX_ROOTS])
{
    int n = 0;
    for (int i = 0; i < UI_MAX_ROOTS; i++)
        if (ctx->roots[i].used && ctx->roots[i].frame == f) out[n++] = i;
    for (int i = 1; i < n; i++) {
        int t = out[i], j = i - 1;
        const ui_root *rt = &ctx->roots[t];
        int kt = root_rank(ctx, rt);
        while (j >= 0) {
            const ui_root *rj = &ctx->roots[out[j]];
            int kj = root_rank(ctx, rj);
            if (kj < kt || (kj == kt && rj->order <= rt->order)) break;
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = t;
    }
    return n;
}

/* ---- frame --------------------------------------------------------------- */
static void begin_hover(ui_ctx *ctx)
{
    int order[UI_MAX_ROOTS], n = sorted_roots(ctx, ctx->frame - 1u, order);
    float mx = ctx->fin.mx, my = ctx->fin.my;
    ctx->hover_root = 0;
    ctx->top_modal = 0;
    for (int i = 0; i < n; i++)
        if (ctx->roots[order[i]].kind == UI_ROOT_MODAL) ctx->top_modal = ctx->roots[order[i]].id;
    if (!ctx->fin.mouse_in) return;
    for (int i = n - 1; i >= 0; i--) {
        const ui_root *r = &ctx->roots[order[i]];
        if (r->kind == UI_ROOT_TOOLTIP || r->prev_hidden) continue;
        if (ui_rect_contains(r->prev_rect, mx, my)) { ctx->hover_root = r->id; return; }
    }
}

static void begin_tab(ui_ctx *ctx)
{
    uint32_t mods = 0;
    ui_id scope;
    int32_t cur = -1, cnt = 0, first = -1, last = -1, pick = -1;
    if (ctx->npopups > 0) return;
    if (!ui_key_take_any(ctx, SDLK_TAB, &mods)) return;
    scope = ctx->top_modal ? ctx->top_modal : (ctx->focus ? ctx->focus_root : UI_BASE_ROOT_ID);
    for (int32_t i = 0; i < ctx->prev_nfocus; i++) {
        if (ctx->prev_focus_roots[i] != scope) continue;
        if (first < 0) first = i;
        last = i;
        if (ctx->prev_focus_list[i] == ctx->focus) cur = i;
        cnt++;
    }
    if (!cnt) return;
    if (cur < 0) {
        pick = (mods & UI_MOD_SHIFT) ? last : first;
    } else {
        int32_t step = (mods & UI_MOD_SHIFT) ? -1 : 1, i = cur;
        for (int32_t k = 0; k < ctx->prev_nfocus; k++) {
            i += step;
            if (i < 0) i = ctx->prev_nfocus - 1;
            if (i >= ctx->prev_nfocus) i = 0;
            if (ctx->prev_focus_roots[i] == scope) { pick = i; break; }
        }
    }
    if (pick >= 0) {
        ctx->focus = ctx->prev_focus_list[pick];
        ctx->focus_root = ctx->prev_focus_roots[pick];
        ctx->focus_visible = true;
    }
}

static void begin_autofocus(ui_ctx *ctx)
{
    if (!ctx->autofocus_root) return;
    if (ctx->focus && ctx->focus_root == ctx->autofocus_root) { ctx->autofocus_root = 0; return; }
    for (int32_t i = 0; i < ctx->prev_nfocus; i++) {
        if (ctx->prev_focus_roots[i] == ctx->autofocus_root) {
            ctx->focus = ctx->prev_focus_list[i];
            ctx->focus_root = ctx->autofocus_root;
            ctx->autofocus_root = 0;
            ctx->want_frame = true;
            return;
        }
    }
}

void ui_begin_frame(ui_ctx *ctx, const ui_frame_info *fi)
{
    ctx->fi = *fi;
    if (!(ctx->fi.px_per_point > 0.0f)) ctx->fi.px_per_point = 1.0f;
    ctx->scale = ui_clampf(fi->scale > 0.0f ? fi->scale : 1.0f, 0.5f, 8.0f);
    compute_px(ctx);
    ctx->now = fi->time_ms;
    ctx->frame++;
    ctx->in_frame = true;
    ctx->want_frame = false;
    ctx->wake_at = UINT64_MAX;
    ui_atlas_frame(ctx);
    if ((ctx->frame & 63u) == 0) store_gc(ctx);

    /* snapshot the input gathered since the last frame */
    ctx->fin = ctx->in;
    ctx->in.pressed = 0;
    ctx->in.released = 0;
    ctx->in.wheel_x = ctx->in.wheel_y = 0.0f;
    ctx->in.nkeys = 0;
    ctx->in.ntext = 0;
    ctx->in.text[0] = '\0';
    memset(ctx->press_taken, 0, sizeof ctx->press_taken);

    ctx->nfocus = 0;
    ctx->focus_seen = false;
    ctx->hover_cand = 0;
    ctx->last_id = 0;
    ctx->last_hovered = false;
    ctx->last_right_clicked = false;
    ctx->cursor = UI_CURSOR_DEFAULT;
    ctx->tip_show = false;
    ctx->popup_depth = 0;
    ctx->popup_dismissed = 0;
    ctx->mb_active = false;
    ctx->root_depth = 0;
    ctx->clip_depth = 0;
    ctx->lay_depth = 0;
    ctx->id_depth = 0;
    ctx->dlg_id = 0;
    ctx->text_input_want = false;
    ctx->active_seen = false;
    for (int i = 0; i < UI_MAX_ROOTS; i++) {
        ui_root *r = &ctx->roots[i];
        if (!r->used) continue;
        r->prev_rect = r->frame + 1u == ctx->frame ? r->rect : ui_rect_make(0, 0, 0, 0);
        r->prev_hidden = r->hidden;
        r->prev_frame = r->frame;
    }
    ui_popups_frame_begin(ctx);       /* outside clicks, Escape */
    begin_hover(ctx);
    ui_panels_frame_begin(ctx);       /* raise on press */
    begin_autofocus(ctx);
    begin_tab(ctx);
    if (ctx->fin.pressed) ctx->tip_block = ctx->hot;

    ctx->cur_root = 0;
    ui_root_begin(ctx, UI_BASE_ROOT_ID, UI_ROOT_BASE, ui_rect_make(0, 0, fi->width, fi->height),
                  false);
    ctx->root_depth = 0;              /* the base root is the bottom of the stack */
    ui_layout_root(ctx, ui_rect_make(0, 0, fi->width, fi->height), 0, UI_LAY_ROOT,
                   UI_BASE_ROOT_ID);
}

static void apply_cursor(ui_ctx *ctx)
{
    static const SDL_SystemCursor map[UI_CURSOR_COUNT] = {
        SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_POINTER,
        SDL_SYSTEM_CURSOR_MOVE, SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NS_RESIZE,
        SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE,
        SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_NOT_ALLOWED, SDL_SYSTEM_CURSOR_WAIT,
        SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_DEFAULT,
    };
    ui_cursor c = ctx->cursor;
    if (!ctx->auto_cursor || !ctx->win || c == ctx->applied_cursor) return;
    if (c == UI_CURSOR_APP) { ctx->applied_cursor = c; return; }
    if (c == UI_CURSOR_HIDDEN) {
        SDL_HideCursor();
    } else {
        if (ctx->applied_cursor == UI_CURSOR_HIDDEN) SDL_ShowCursor();
        if (!ctx->sys_cursor[c]) ctx->sys_cursor[c] = SDL_CreateSystemCursor(map[c]);
        if (ctx->sys_cursor[c]) SDL_SetCursor(ctx->sys_cursor[c]);
    }
    ctx->applied_cursor = c;
}

static void apply_text_input(ui_ctx *ctx)
{
    if (!ctx->win) { ctx->text_input_on = ctx->text_input_want; return; }
    if (ctx->text_input_want) {
        SDL_Rect r;
        float k = ctx->fi.px_per_point > 0.0f ? ctx->fi.px_per_point : 1.0f;
        if (!ctx->text_input_on) SDL_StartTextInput(ctx->win);
        r.x = (int)((float)ctx->text_input_rect.x / k);
        r.y = (int)((float)ctx->text_input_rect.y / k);
        r.w = (int)((float)ctx->text_input_rect.w / k) + 1;
        r.h = (int)((float)ctx->text_input_rect.h / k) + 1;
        SDL_SetTextInputArea(ctx->win, &r, 0);
        ctx->text_input_on = true;
    } else if (ctx->text_input_on) {
        SDL_StopTextInput(ctx->win);
        ctx->text_input_on = false;
    }
}

void ui_end_frame(ui_ctx *ctx)
{
    ui_root *base;
    if (!ctx->in_frame) return;
    /* unwind anything left open (unbalanced begin/end calls) */
    while (ctx->root_depth > 0) ui_root_end(ctx);
    ctx->cur_root = 0;
    ui_popups_frame_end(ctx);
    ui_tooltip_draw(ctx);
    base = &ctx->roots[0];
    (void)base;
    if (ctx->hover_cand != ctx->hot) {
        ctx->hot = ctx->hover_cand;
        ctx->hot_since = ctx->now;
        ctx->want_frame = true;
    }
    ctx->hot_rect = ctx->hover_cand_rect;
    if (ctx->active && !ctx->active_seen) ctx->active = 0;
    if ((ctx->fin.pressed & 1u) && !ctx->press_taken[UI_MOUSE_LEFT] && ctx->focus) {
        ctx->focus = 0;
        ctx->want_frame = true;
    }
    if (ctx->focus && !ctx->focus_seen) ctx->focus = 0;
    if (!ctx->focus) ctx->edit.id = 0;
    memcpy(ctx->prev_focus_list, ctx->focus_list, (size_t)ctx->nfocus * sizeof(ui_id));
    memcpy(ctx->prev_focus_roots, ctx->focus_roots, (size_t)ctx->nfocus * sizeof(ui_id));
    ctx->prev_nfocus = ctx->nfocus;
    apply_text_input(ctx);
    apply_cursor(ctx);
    ctx->pmx = ctx->fin.mx;
    ctx->pmy = ctx->fin.my;
    if (ctx->in.pressed || ctx->in.released || ctx->in.nkeys || ctx->in.ntext)
        ctx->want_frame = true;     /* events arrived during the frame */
    ctx->in_frame = false;
}

void ui_render(ui_ctx *ctx)
{
    int order[UI_MAX_ROOTS], n;
    ui_atlas_upload(ctx);
    n = sorted_roots(ctx, ctx->frame, order);
    for (int i = 0; i < n; i++) {
        ui_root *r = &ctx->roots[order[i]];
        if (r->hidden) continue;
        ui_replay_root(ctx, r);
    }
    SDL_SetRenderClipRect(ctx->r, NULL);
}

/* ---- cursor and keys ----------------------------------------------------- */
void ui_set_cursor(ui_ctx *ctx, ui_cursor c)
{
    if ((int)c >= 0 && c < UI_CURSOR_COUNT) ctx->cursor = c;
}
ui_cursor ui_get_cursor(const ui_ctx *ctx) { return ctx->cursor; }
void ui_set_auto_cursor(ui_ctx *ctx, bool on) { ctx->auto_cursor = on && ctx->win; }
uint32_t ui_mods(const ui_ctx *ctx) { return ctx->in.mods; }

bool ui_key_take(ui_ctx *ctx, int32_t key, uint32_t mods)
{
    for (int32_t i = 0; i < ctx->fin.nkeys; i++) {
        ui_key_press *k = &ctx->fin.keys[i];
        if (!k->used && k->key == key && k->mods == mods) { k->used = true; return true; }
    }
    return false;
}

bool ui_key_take_any(ui_ctx *ctx, int32_t key, uint32_t *mods)
{
    for (int32_t i = 0; i < ctx->fin.nkeys; i++) {
        ui_key_press *k = &ctx->fin.keys[i];
        if (!k->used && k->key == key) {
            k->used = true;
            if (mods) *mods = k->mods;
            return true;
        }
    }
    return false;
}

const ui_key_press *ui_key_presses(const ui_ctx *ctx, int *n)
{
    *n = ctx->fin.nkeys;
    return ctx->fin.keys;
}

/* ---- pointer ------------------------------------------------------------- */
ui_vec2 ui_mouse_pos(const ui_ctx *ctx) { return ui_vec2_make(ctx->fin.mx, ctx->fin.my); }
bool ui_mouse_down(const ui_ctx *ctx, int b)
{ return b >= 0 && b < 3 && (ctx->fin.down >> b & 1u); }
bool ui_mouse_pressed(const ui_ctx *ctx, int b) { return (ctx->fin.pressed >> b & 1u) != 0; }
bool ui_mouse_released(const ui_ctx *ctx, int b) { return (ctx->fin.released >> b & 1u) != 0; }

bool ui_mouse_in(const ui_ctx *ctx, ui_rect r)
{
    ui_rect v = ui_rect_intersect(r, ui_current_clip(ctx));
    return ctx->fin.mouse_in && ui_rect_contains(v, ctx->fin.mx, ctx->fin.my);
}

ui_vec2 ui_wheel_take(ui_ctx *ctx, ui_rect r)
{
    ui_vec2 w = ui_vec2_make(0.0f, 0.0f);
    if (!ui_root_hovered(ctx) || !ui_mouse_in(ctx, r)) return w;
    if (ctx->npopups > 0 && ui_root_cur(ctx)->kind != UI_ROOT_POPUP) return w;
    w.x = ctx->fin.wheel_x;
    w.y = ctx->fin.wheel_y;
    ctx->fin.wheel_x = ctx->fin.wheel_y = 0.0f;
    return w;
}

/* ---- interaction --------------------------------------------------------- */
void ui_focus_register(ui_ctx *ctx, ui_id id)
{
    ui_id root = ui_root_cur(ctx)->id;
    if (ctx->nfocus < UI_MAX_FOCUS) {
        ctx->focus_list[ctx->nfocus] = id;
        ctx->focus_roots[ctx->nfocus] = root;
        ctx->nfocus++;
    }
    if (id == ctx->focus) { ctx->focus_seen = true; ctx->focus_root = root; }
}

ui_interaction ui_interact(ui_ctx *ctx, ui_id id, ui_rect r, uint32_t flags)
{
    ui_interaction res;
    ui_vec2 m = ui_vec2_make(ctx->fin.mx, ctx->fin.my);
    bool inside = ui_mouse_in(ctx, r), over;
    bool disabled = (flags & UI_INTERACT_DISABLED) != 0;
    memset(&res, 0, sizeof res);
    res.mouse = m;
    res.press_pos = ctx->fin.press_pos[UI_MOUSE_LEFT];
    over = inside && ui_root_hovered(ctx) && !disabled;
    if (over && ctx->npopups > 0 && ui_root_cur(ctx)->kind != UI_ROOT_POPUP &&
        !(flags & UI_INTERACT_MENUBAR))
        over = false;
    if (over && (flags & UI_INTERACT_OVERLAP) && ctx->hot && ctx->hot != id &&
        ui_rect_contains(ctx->hot_rect, m.x, m.y) &&
        !ui_rect_empty(ui_rect_intersect(ctx->hot_rect, r)))
        over = false;
    if (over) { ctx->hover_cand = id; ctx->hover_cand_rect = r; }
    res.hovered = over && (ctx->active == 0 || ctx->active == id);
    if (res.hovered && ui_mouse_pressed(ctx, UI_MOUSE_LEFT) && !ctx->press_taken[UI_MOUSE_LEFT]) {
        ctx->press_taken[UI_MOUSE_LEFT] = true;
        ctx->active = id;
        ctx->active_root = ui_root_cur(ctx)->id;
        ctx->active_t0 = ctx->now;
        ctx->repeat_n = 0;
        res.pressed = true;
        if (flags & UI_INTERACT_FOCUSABLE) {
            ctx->focus = id;
            ctx->focus_root = ui_root_cur(ctx)->id;
            ctx->focus_visible = false;
        } else if (!(flags & UI_INTERACT_KEEP_FOCUS) && ctx->focus != id) {
            ctx->focus = 0;
        }
        if (ctx->fin.clicks[UI_MOUSE_LEFT] >= 2) res.double_clicked = true;
        if (flags & UI_INTERACT_PRESS) res.clicked = true;
        ctx->want_frame = true;
    }
    if (ctx->active == id) {
        ctx->active_seen = true;
        if (ui_mouse_down(ctx, UI_MOUSE_LEFT)) res.held = true;
        if (ui_mouse_released(ctx, UI_MOUSE_LEFT)) {
            res.released = true;
            res.held = false;
            if (!(flags & UI_INTERACT_PRESS) && ui_rect_contains(r, m.x, m.y) && !disabled)
                res.clicked = true;
            ctx->active = 0;
            ctx->want_frame = true;
        } else if (!ui_mouse_down(ctx, UI_MOUSE_LEFT)) {
            ctx->active = 0;
        }
        if (res.held) {
            float dx = m.x - res.press_pos.x, dy = m.y - res.press_pos.y;
            float th = (float)ctx->px.drag;
            if (dx * dx + dy * dy > th * th) res.dragging = true;
            if ((flags & UI_INTERACT_REPEAT) && inside) {
                uint64_t due = ctx->active_t0 + 400u + 60u * ctx->repeat_n;
                if (ctx->now >= due) {
                    res.clicked = true;
                    ctx->repeat_n++;
                    due = ctx->active_t0 + 400u + 60u * ctx->repeat_n;
                }
                ui_request_frame_at(ctx, due);
            }
        }
    }
    if (over && ui_mouse_released(ctx, UI_MOUSE_RIGHT)) {
        res.right_clicked = true;
        ctx->last_right_clicked = true;
    }
    if (over && ui_mouse_released(ctx, UI_MOUSE_MIDDLE)) res.middle_clicked = true;
    if ((flags & UI_INTERACT_FOCUSABLE) && !disabled) {
        ui_focus_register(ctx, id);
        res.focused = ctx->focus == id;
        if (res.focused && !(flags & UI_INTERACT_NO_KEYS) &&
            (ui_key_take(ctx, SDLK_SPACE, 0) || ui_key_take(ctx, SDLK_RETURN, 0) ||
             ui_key_take(ctx, SDLK_KP_ENTER, 0))) {
            res.clicked = true;
            res.key_activated = true;
            ctx->focus_visible = true;
        }
    }
    ctx->last_id = id;
    ctx->last_rect = r;
    ctx->last_hovered = res.hovered;
    if (!res.right_clicked) ctx->last_right_clicked = false;
    return res;
}

ui_id ui_last_id(const ui_ctx *ctx) { return ctx->last_id; }
ui_rect ui_last_rect(const ui_ctx *ctx) { return ctx->last_rect; }
bool ui_last_hovered(const ui_ctx *ctx) { return ctx->last_hovered; }
bool ui_last_right_clicked(const ui_ctx *ctx) { return ctx->last_right_clicked; }
bool ui_is_focused(const ui_ctx *ctx, ui_id id) { return id && ctx->focus == id; }
ui_id ui_focus_id(const ui_ctx *ctx) { return ctx->focus; }
bool ui_is_active(const ui_ctx *ctx, ui_id id) { return id && ctx->active == id; }
bool ui_focus_visible(const ui_ctx *ctx) { return ctx->focus_visible; }

void ui_set_focus(ui_ctx *ctx, ui_id id)
{
    ctx->focus = id;
    ctx->focus_root = id ? ui_root_cur(ctx)->id : 0;
    ctx->want_frame = true;
}

void ui_draw_focus_ring(ui_ctx *ctx, ui_rect r, float radius)
{
    int32_t f = ctx->px.focus, gap = ui_px_line(ctx, 1.0f);
    if (!ctx->focus_visible) return;
    ui_draw_rrect_outline(ctx, ui_rect_inset(r, -(f + gap), -(f + gap)), radius + (float)(f + gap),
                          f, ctx->theme.pal.focus);
}

/* ---- tooltips ------------------------------------------------------------ */
void ui_tooltip(ui_ctx *ctx, const char *text)
{
    uint64_t due;
    if (!text || !*text || !ctx->last_hovered || ctx->last_id != ctx->hot) return;
    if (ctx->active || ctx->tip_block == ctx->hot) return;
    due = ctx->hot_since + (uint64_t)ctx->theme.m.tooltip_delay_ms;
    if (ctx->now < due) { ui_request_frame_at(ctx, due); return; }
    if (!ctx->tip_show) {
        size_t n = strlen(text);
        if (n >= sizeof ctx->tip) n = ui_utf8_floor(text, n, sizeof ctx->tip - 1u);
        memcpy(ctx->tip, text, n);
        ctx->tip[n] = '\0';
        ctx->tip_show = true;
        ctx->tip_pos = ui_vec2_make(ctx->fin.mx, (float)(ctx->last_rect.y + ctx->last_rect.h));
    }
}

void ui_tooltip_draw(ui_ctx *ctx)
{
    const ui_palette *p = &ctx->theme.pal;
    float fs = ctx->px.font_small;
    int32_t padx = ui_px(ctx, 8.0f), pady = ui_px(ctx, 5.0f), w, h, x, y;
    ui_font_metrics m;
    ui_rect r;
    if (!ctx->tip_show) return;
    ui_font_get_metrics(ctx->font_reg, fs, &m);
    w = (int32_t)ceilf(ui_text_width(ctx->font_reg, fs, ctx->tip, strlen(ctx->tip))) + 2 * padx;
    h = (int32_t)ceilf(m.line_height) + 2 * pady;
    x = (int32_t)ctx->tip_pos.x;
    y = (int32_t)ctx->tip_pos.y + ui_px(ctx, 6.0f);
    if (x + w > ctx->fi.width - 4) x = ctx->fi.width - 4 - w;
    if (x < 4) x = 4;
    if (y + h > ctx->fi.height - 4) y = (int32_t)ctx->tip_pos.y - h - ui_px(ctx, 30.0f);
    r = ui_rect_make(x, y, w, h);
    ui_root_begin(ctx, UI_TIP_ROOT_ID, UI_ROOT_TOOLTIP, ui_rect_make(0, 0, 0, 0), false);
    ui_draw_shadow(ctx, ui_rect_offset(r, 0, ui_px(ctx, 2.0f)), ctx->px.radius,
                   ctx->px.shadow * 0.6f, p->shadow);
    ui_draw_rrect(ctx, r, ctx->px.radius, p->tooltip);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius, ctx->px.border, p->border);
    ui_draw_text_box(ctx, ctx->font_reg, fs, ui_rect_inset(r, padx, 0), UI_ALIGN_LEFT, 0,
                     p->tooltip_text, ctx->tip, strlen(ctx->tip));
    ui_root_end(ctx);
}

/* ---- clipboard and text input -------------------------------------------- */
void ui_set_clipboard(ui_ctx *ctx, const char *s)
{
    size_t n = strlen(s);
    char *c = (char *)malloc(n + 1u);
    if (c) {
        memcpy(c, s, n + 1u);
        free(ctx->clip_text);
        ctx->clip_text = c;
    }
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_SetClipboardText(s);
}

char *ui_get_clipboard(ui_ctx *ctx)
{
    const char *src = ctx->clip_text;
    char *sdl = NULL, *out;
    size_t n;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_HasClipboardText()) {
        sdl = SDL_GetClipboardText();
        if (sdl && *sdl) src = sdl;
    }
    if (!src) { SDL_free(sdl); return NULL; }
    n = strlen(src);
    out = (char *)malloc(n + 1u);
    if (out) memcpy(out, src, n + 1u);
    SDL_free(sdl);
    return out;
}

void ui_text_input_request(ui_ctx *ctx, ui_rect caret)
{
    ctx->text_input_want = true;
    ctx->text_input_rect = caret;
}
