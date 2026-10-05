/* ui_layout.c - rows, columns, nested containers and scroll regions. */
#include "ui_internal.h"

#include <math.h>
#include <string.h>

ui_layout *ui_layout_top(ui_ctx *ctx) { return &ctx->lay[ctx->lay_depth - 1]; }

static ui_layout *push(ui_ctx *ctx, ui_rect r, int32_t pad, int32_t kind, ui_id id)
{
    ui_layout *l;
    if (ctx->lay_depth >= UI_MAX_LAYOUT) ctx->lay_depth = UI_MAX_LAYOUT - 1;
    l = &ctx->lay[ctx->lay_depth++];
    memset(l, 0, sizeof *l);
    l->rect = ui_rect_inset(r, pad, pad);
    l->cx = l->rect.x;
    l->cy = l->rect.y;
    l->row_y = l->cy;
    l->spacing = ctx->px.spacing;
    l->max_x = l->rect.x;
    l->max_y = l->rect.y;
    l->kind = kind;
    l->pad = pad;
    l->id = id;
    return l;
}

static ui_id child_id(ui_ctx *ctx)
{
    ui_layout *p = ui_layout_top(ctx);
    uint32_t k = p->children++;
    return ui_hash(&k, (ptrdiff_t)sizeof k, p->id);
}

void ui_layout_root(ui_ctx *ctx, ui_rect r, int32_t pad, int32_t kind, ui_id id)
{
    (void)push(ctx, r, pad, kind, id);
}

void ui_layout_close(ui_ctx *ctx)
{
    if (ctx->lay_depth > 1) ctx->lay_depth--;
}

static void extend(ui_layout *l, ui_rect r)
{
    if (r.x + r.w > l->max_x) l->max_x = r.x + r.w;
    if (r.y + r.h > l->max_y) l->max_y = r.y + r.h;
}

void ui_layout_extend(ui_ctx *ctx, ui_rect r) { extend(ui_layout_top(ctx), r); }

/* Finish a partially filled row so the cursor moves below it. */
static void finish_row(ui_layout *l)
{
    if (l->ncells > 0 && l->cell > 0) {
        l->cy = l->row_y + l->row_h + l->spacing;
        l->row_y = l->cy;
        l->row_h = 0;
        l->cell = 0;
        l->cx = l->rect.x;
    }
}

static ui_id memo_id(const ui_layout *l, int i)
{
    uint32_t k[2];
    k[0] = l->row_index;
    k[1] = (uint32_t)i;
    return ui_hash(k, (ptrdiff_t)sizeof k, l->id ^ 0x5A5Au);
}

static void compute_widths(ui_ctx *ctx, ui_layout *l)
{
    int32_t avail = l->rect.w - l->spacing * (l->ncells - 1), fixed = 0, given = 0;
    float frs = 0.0f, acc = 0.0f;
    int last_fr = -1;
    for (int i = 0; i < l->ncells; i++) {
        const ui_size *c = &l->cells[i];
        if (c->kind == UI_SIZE_PX) {
            l->widths[i] = ui_px(ctx, c->value);
            fixed += l->widths[i];
        } else if (c->kind == UI_SIZE_AUTO) {
            ui_state *st = ui_state_find(ctx, memo_id(l, i));
            l->widths[i] = st ? st->i[0] : ctx->px.control_h * 3;
            fixed += l->widths[i];
        } else {
            frs += c->value > 0.0f ? c->value : 0.0f;
            last_fr = i;
        }
    }
    avail -= fixed;
    if (avail < 0) avail = 0;
    for (int i = 0; i < l->ncells; i++) {
        const ui_size *c = &l->cells[i];
        if (c->kind != UI_SIZE_FR) continue;
        if (i == last_fr) {
            l->widths[i] = avail - given;
        } else {
            acc += frs > 0.0f ? (float)avail * (c->value > 0.0f ? c->value : 0.0f) / frs : 0.0f;
            l->widths[i] = (int32_t)floorf(acc + 0.5f) - given;
        }
        if (l->widths[i] < 0) l->widths[i] = 0;
        given += l->widths[i];
    }
}

void ui_layout_row(ui_ctx *ctx, float height_dip, int n, const ui_size *cells)
{
    ui_layout *l = ui_layout_top(ctx);
    finish_row(l);
    if (n < 1) n = 1;
    if (n > UI_MAX_CELLS) n = UI_MAX_CELLS;
    l->ncells = n;
    memcpy(l->cells, cells, (size_t)n * sizeof(ui_size));
    l->row_fixed_h = height_dip > 0.0f ? ui_px(ctx, height_dip) : 0;
    l->cell = 0;
    l->row_y = l->cy;
    l->row_h = 0;
    l->cx = l->rect.x;
    l->row_index++;
    compute_widths(ctx, l);
}

void ui_layout_column(ui_ctx *ctx)
{
    ui_layout *l = ui_layout_top(ctx);
    finish_row(l);
    l->ncells = 0;
    l->cell = 0;
}

/* Geometry of the next cell without claiming it. */
static ui_rect peek(ui_ctx *ctx, ui_layout *l, int32_t pref_w, int32_t pref_h, bool commit)
{
    ui_rect r;
    if (l->ncells > 0) {
        int32_t w;
        if (l->cell >= l->ncells) {
            l->cy = l->row_y + l->row_h + l->spacing;
            l->row_y = l->cy;
            l->row_h = 0;
            l->cell = 0;
            l->cx = l->rect.x;
        }
        w = l->widths[l->cell];
        if (l->cells[l->cell].kind == UI_SIZE_AUTO && commit) {
            ui_state *st = ui_state_get(ctx, memo_id(l, l->cell));
            if (st && st->i[0] != pref_w) {
                st->i[0] = pref_w;
                ctx->want_frame = true;
                compute_widths(ctx, l);
            }
            w = pref_w;
        }
        r = ui_rect_make(l->cx, l->row_y, w, l->row_fixed_h ? l->row_fixed_h : pref_h);
        if (commit) {
            l->cx += w + l->spacing;
            if (r.h > l->row_h) l->row_h = r.h;
            l->cell++;
        }
    } else {
        r = ui_rect_make(l->rect.x, l->cy, l->rect.w, pref_h);
        if (commit) l->cy += pref_h + l->spacing;
    }
    if (commit) extend(l, r);
    return r;
}

ui_rect ui_layout_next(ui_ctx *ctx, int32_t pref_w, int32_t pref_h)
{
    ui_layout *l = ui_layout_top(ctx);
    if (l->has_next) {
        l->has_next = false;
        return l->next;
    }
    return peek(ctx, l, pref_w, pref_h, true);
}

ui_rect ui_layout_next_natural(ui_ctx *ctx, int32_t pref_w, int32_t pref_h)
{
    ui_layout *l = ui_layout_top(ctx);
    bool column = l->ncells == 0 && !l->has_next;
    ui_rect r = ui_layout_next(ctx, pref_w, pref_h);
    if (column && pref_w > 0 && pref_w < r.w) r.w = pref_w;
    return r;
}

void ui_layout_set_next(ui_ctx *ctx, ui_rect r)
{
    ui_layout *l = ui_layout_top(ctx);
    l->has_next = true;
    l->next = r;
}

void ui_layout_space(ui_ctx *ctx, float dip)
{
    ui_layout *l = ui_layout_top(ctx);
    finish_row(l);
    l->cy += ui_px(ctx, dip);
    l->row_y = l->cy;
}

void ui_layout_set_spacing(ui_ctx *ctx, float dip)
{
    ui_layout *l = ui_layout_top(ctx);
    l->spacing = ui_px(ctx, dip);
    if (l->ncells > 0) compute_widths(ctx, l);
}

ui_rect ui_layout_rest(const ui_ctx *ctx)
{
    const ui_layout *l = &ctx->lay[ctx->lay_depth - 1];
    int32_t y = l->cy, h;
    if (l->ncells > 0 && l->cell > 0) y = l->row_y + l->row_h + l->spacing;
    h = l->rect.y + l->rect.h - y;
    return ui_rect_make(l->rect.x, y, l->rect.w, h > 0 ? h : 0);
}

ui_rect ui_layout_content(const ui_ctx *ctx) { return ctx->lay[ctx->lay_depth - 1].rect; }

int32_t ui_layout_avail_w(const ui_ctx *ctx)
{
    const ui_layout *l = &ctx->lay[ctx->lay_depth - 1];
    if (l->has_next) return l->next.w;
    if (l->ncells > 0) return l->widths[l->cell < l->ncells ? l->cell : 0];
    return l->rect.w;
}

void ui_layout_push(ui_ctx *ctx, ui_rect r, float pad_dip)
{
    ui_id id = child_id(ctx);
    (void)push(ctx, r, ui_px(ctx, pad_dip), UI_LAY_PUSH, id);
}

void ui_layout_pop(ui_ctx *ctx)
{
    ui_layout *c;
    if (ctx->lay_depth <= 1) return;
    c = &ctx->lay[--ctx->lay_depth];
    extend(ui_layout_top(ctx), ui_rect_make(c->rect.x, c->rect.y, c->max_x - c->rect.x + c->pad,
                                            c->max_y - c->rect.y + c->pad));
}

ui_rect ui_layout_begin(ui_ctx *ctx, float pad_dip)
{
    ui_layout *p = ui_layout_top(ctx), *c;
    ui_id id = child_id(ctx);
    int32_t pad = ui_px(ctx, pad_dip);
    ui_rect cell;
    if (p->has_next) {
        cell = p->next;
    } else {
        cell = peek(ctx, p, 0, 0, false);
        if (!p->row_fixed_h || p->ncells == 0) cell.h = p->rect.y + p->rect.h - cell.y;
    }
    c = push(ctx, cell, pad, UI_LAY_CELL, id);
    c->next = cell;                  /* remembered for ui_layout_end */
    return cell;
}

void ui_layout_end(ui_ctx *ctx)
{
    ui_layout *c, *p;
    int32_t used_h, used_w;
    ui_rect cell;
    if (ctx->lay_depth <= 1) return;
    c = &ctx->lay[ctx->lay_depth - 1];
    cell = c->next;
    used_h = (c->max_y > c->rect.y ? c->max_y - c->rect.y : 0) + 2 * c->pad;
    used_w = (c->max_x > c->rect.x ? c->max_x - c->rect.x : 0) + 2 * c->pad;
    ctx->lay_depth--;
    p = ui_layout_top(ctx);
    if (p->has_next) {
        p->has_next = false;
        extend(p, ui_rect_make(cell.x, cell.y, cell.w, used_h));
    } else {
        (void)peek(ctx, p, used_w, used_h, true);
    }
}
