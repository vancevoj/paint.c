/* pc_doc.c - documents and layers. */
#include "pc/pc_doc.h"
#include "pc/pc_sel.h"

#include <stdlib.h>
#include <string.h>

static pc_atomic_u32 g_live_layers;

size_t pc_layer_live_count(void) { return pc_atomic_load(&g_live_layers); }

pc_doc *pc_doc_create(uint32_t w, uint32_t h)
{
    pc_doc *d;
    if (w == 0u || h == 0u || w > PC_MAX_DIM || h > PC_MAX_DIM) return NULL;
    d = (pc_doc *)calloc(1u, sizeof *d);
    if (!d) return NULL;
    d->w = w;
    d->h = h;
    d->tiles_x = (w + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    d->tiles_y = (h + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    d->next_layer_id = 1u;
    /* Selection (lane L1a): sel_grid stays NULL and sel_active false until
     * the apply phase of the first selection edit allocates the grid
     * (pc_sel.c); deselect hands the grid to history and leaves NULL. */
    return d;
}

void pc_doc_destroy(pc_doc *d)
{
    if (!d) return;
    pc_sel_doc_destroyed(d);      /* drop cached selection data keyed by d */
    for (uint32_t i = 0; i < d->n_layers; i++) pc_layer_destroy(d->stack[i]);
    if (d->sel_grid) {
        size_t n = (size_t)d->tiles_x * d->tiles_y;
        for (size_t i = 0; i < n; i++) pc_tile_release(d->sel_grid[i]);
        free(d->sel_grid);
    }
    free(d->stack);
    free(d);
}

static pc_layer *layer_alloc(pc_doc *d)
{
    pc_layer *l;
    size_t n;
    if (!pc_mul_size(d->tiles_x, d->tiles_y, &n)) return NULL;
    l = (pc_layer *)calloc(1u, sizeof *l);
    if (!l) return NULL;
    l->grid = (pc_tile **)calloc(n, sizeof *l->grid);
    if (!l->grid) { free(l); return NULL; }
    l->id = d->next_layer_id++;
    l->mode = PC_BLEND_NORMAL;
    l->opacity = 255u;
    l->visible = true;
    l->tiles_x = d->tiles_x;
    l->tiles_y = d->tiles_y;
    (void)pc_atomic_inc(&g_live_layers);
    return l;
}

static void set_name(pc_layer *l, const char *name)
{
    size_t k = 0;
    if (name) {
        for (; k + 1u < PC_LAYER_NAME_MAX && name[k] != '\0'; k++) l->name[k] = name[k];
    }
    l->name[k] = '\0';
}

pc_layer *pc_layer_create(pc_doc *d, const char *name)
{
    pc_layer *l = layer_alloc(d);
    if (l) set_name(l, name);
    return l;
}

pc_layer *pc_layer_duplicate(pc_doc *d, const pc_layer *src)
{
    pc_layer *l;
    size_t n = (size_t)src->tiles_x * src->tiles_y;
    if (src->tiles_x != d->tiles_x || src->tiles_y != d->tiles_y) return NULL;
    l = layer_alloc(d);
    if (!l) return NULL;
    memcpy(l->name, src->name, sizeof l->name);
    l->mode = src->mode;
    l->opacity = src->opacity;
    l->visible = src->visible;
    for (size_t i = 0; i < n; i++) {
        l->grid[i] = src->grid[i];
        pc_tile_retain(l->grid[i]);           /* shared, copy-on-write */
    }
    return l;
}

void pc_layer_destroy(pc_layer *l)
{
    size_t n;
    if (!l) return;
    n = (size_t)l->tiles_x * l->tiles_y;
    for (size_t i = 0; i < n; i++) pc_tile_release(l->grid[i]);
    free(l->grid);
    free(l);
    (void)pc_atomic_dec(&g_live_layers);
}

pc_status pc_doc_reserve_layers(pc_doc *d, uint32_t n)
{
    uint32_t cap;
    size_t bytes;
    pc_layer **s;
    if (!d) return PC_ERR_ARG;
    if (n <= d->cap_layers) return PC_OK;
    cap = d->cap_layers ? d->cap_layers : 8u;
    while (cap < n) {
        if (cap > UINT32_MAX / 2u) return PC_ERR_LIMIT;
        cap *= 2u;
    }
    if (!pc_mul_size(cap, sizeof *s, &bytes)) return PC_ERR_LIMIT;
    s = (pc_layer **)realloc(d->stack, bytes);
    if (!s) return PC_ERR_NOMEM;
    d->stack = s;
    d->cap_layers = cap;
    return PC_OK;
}

pc_status pc_doc_insert_layer(pc_doc *d, pc_layer *l, uint32_t index)
{
    pc_status st;
    if (!d || !l || index > d->n_layers) return PC_ERR_ARG;
    if (l->tiles_x != d->tiles_x || l->tiles_y != d->tiles_y) return PC_ERR_ARG;
    if (d->n_layers == UINT32_MAX) return PC_ERR_LIMIT;
    st = pc_doc_reserve_layers(d, d->n_layers + 1u);
    if (st != PC_OK) return st;
    memmove(&d->stack[index + 1u], &d->stack[index],
            (size_t)(d->n_layers - index) * sizeof *d->stack);
    d->stack[index] = l;
    d->n_layers++;
    d->gen++;
    return PC_OK;
}

pc_layer *pc_doc_detach_layer(pc_doc *d, uint32_t index)
{
    pc_layer *l;
    if (!d || index >= d->n_layers) return NULL;
    l = d->stack[index];
    memmove(&d->stack[index], &d->stack[index + 1u],
            (size_t)(d->n_layers - index - 1u) * sizeof *d->stack);
    d->n_layers--;
    d->gen++;
    return l;
}

int32_t pc_doc_layer_index(const pc_doc *d, uint32_t id)
{
    for (uint32_t i = 0; i < d->n_layers; i++)
        if (d->stack[i]->id == id) return (int32_t)i;
    return -1;
}

pc_layer *pc_doc_layer_by_id(const pc_doc *d, uint32_t id)
{
    int32_t i = pc_doc_layer_index(d, id);
    return i < 0 ? NULL : d->stack[i];
}

pc_px32 pc_layer_get_px(const pc_layer *l, uint32_t x, uint32_t y)
{
    pc_px32 p = {0, 0, 0, 0};
    uint32_t tx = x >> PC_TILE_SHIFT, ty = y >> PC_TILE_SHIFT;
    const pc_tile *t;
    if (tx >= l->tiles_x || ty >= l->tiles_y) return p;
    t = l->grid[(size_t)ty * l->tiles_x + tx];
    if (t) {
        size_t off = ((size_t)(y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                      (x & (PC_TILE_DIM - 1u))) * 4u;
        memcpy(&p, t->data + off, 4u);
    }
    return p;
}

/* FNV-1a 64 */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t pc_doc_fingerprint(const pc_doc *d)
{
    static const uint8_t zero_row[PC_TILE_DIM * 4u];
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &d->w, sizeof d->w);
    h = fnv(h, &d->h, sizeof d->h);
    h = fnv(h, &d->n_layers, sizeof d->n_layers);
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        uint32_t mode = (uint32_t)l->mode;
        uint8_t vis = l->visible ? 1u : 0u;
        h = fnv(h, &l->id, sizeof l->id);
        h = fnv(h, l->name, sizeof l->name);
        h = fnv(h, &mode, sizeof mode);
        h = fnv(h, &l->opacity, 1u);
        h = fnv(h, &vis, 1u);
        for (uint32_t ty = 0; ty < l->tiles_y; ty++) {
            for (uint32_t tx = 0; tx < l->tiles_x; tx++) {
                const pc_tile *t = l->grid[(size_t)ty * l->tiles_x + tx];
                uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
                uint32_t cw = d->w - x0 < PC_TILE_DIM ? d->w - x0 : PC_TILE_DIM;
                uint32_t ch = d->h - y0 < PC_TILE_DIM ? d->h - y0 : PC_TILE_DIM;
                for (uint32_t r = 0; r < ch; r++) {
                    const uint8_t *row = t ? t->data + (size_t)r * PC_TILE_DIM * 4u
                                           : zero_row;
                    h = fnv(h, row, (size_t)cw * 4u);
                }
            }
        }
    }
    /* Selection (lane L1a). Only hashed when something is selected or the
     * grid holds tiles, so documents without a selection keep the
     * fingerprint they always had. Coverage of in-bounds pixels only. */
    if (d->sel_grid || d->sel_active) {
        bool any = d->sel_active;
        size_t n = (size_t)d->tiles_x * d->tiles_y;
        for (size_t i = 0; i < n && !any && d->sel_grid; i++) any = d->sel_grid[i] != NULL;
        if (any) {
            static const uint8_t zero_a8[PC_TILE_DIM];
            uint8_t act = d->sel_active ? 1u : 0u;
            h = fnv(h, "selection", 9u);
            h = fnv(h, &act, 1u);
            for (uint32_t ty = 0; ty < d->tiles_y; ty++) {
                for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
                    const pc_tile *t = d->sel_grid ? d->sel_grid[(size_t)ty * d->tiles_x + tx]
                                                   : NULL;
                    uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
                    uint32_t cw = d->w - x0 < PC_TILE_DIM ? d->w - x0 : PC_TILE_DIM;
                    uint32_t ch = d->h - y0 < PC_TILE_DIM ? d->h - y0 : PC_TILE_DIM;
                    for (uint32_t r = 0; r < ch; r++) {
                        const uint8_t *row = t ? t->data + (size_t)r * PC_TILE_DIM : zero_a8;
                        h = fnv(h, row, cw);
                    }
                }
            }
        }
    }
    return h;
}

bool pc_doc_edge_padding_is_zero(const pc_doc *d)
{
    if (d->sel_grid) {          /* A8 selection tiles (lane L1a) */
        for (uint32_t ty = 0; ty < d->tiles_y; ty++) {
            for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
                const pc_tile *t = d->sel_grid[(size_t)ty * d->tiles_x + tx];
                uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
                if (!t) continue;
                if (t->bpp != 1u) return false;
                for (uint32_t r = 0; r < PC_TILE_DIM; r++)
                    for (uint32_t c = 0; c < PC_TILE_DIM; c++)
                        if (((x0 + c) >= d->w || (y0 + r) >= d->h) &&
                            t->data[(size_t)r * PC_TILE_DIM + c])
                            return false;
            }
        }
    }
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        for (uint32_t ty = 0; ty < l->tiles_y; ty++) {
            for (uint32_t tx = 0; tx < l->tiles_x; tx++) {
                const pc_tile *t = l->grid[(size_t)ty * l->tiles_x + tx];
                uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
                if (!t) continue;
                for (uint32_t r = 0; r < PC_TILE_DIM; r++) {
                    for (uint32_t c = 0; c < PC_TILE_DIM; c++) {
                        bool inside = (x0 + c) < d->w && (y0 + r) < d->h;
                        const uint8_t *p = t->data + ((size_t)r * PC_TILE_DIM + c) * 4u;
                        if (!inside && (p[0] | p[1] | p[2] | p[3])) return false;
                    }
                }
            }
        }
    }
    return true;
}
