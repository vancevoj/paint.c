/* ui_icons.c - icon programs, rasterization and the atlas cache (L3).
 *
 * Icons are small programs (ui_icons_data.c) on a 16 unit grid:
 *   op ; op ; ...
 *   op     = header ' ' path
 *   header = layer [mode...] with layer S (soft), A (accent), L (line) and
 *            modes f (fill, nonzero, default), e (fill, even-odd),
 *            s<w> (stroke of width w units, round caps and joins),
 *            b (butt caps), q (square caps), m (miter joins),
 *            x (erase from this layer), X (erase from every layer),
 *            n (no pixel snapping), o<a> (coverage x a),
 *            g<x0>,<y0>,<x1>,<y1> (coverage ramp 0.18 .. 1 along the vector)
 *   path   = SVG path data (M L H V C S Q T A Z, absolute and relative) plus
 *            O cx cy r (circle), E cx cy rx ry (ellipse), R x y w h r
 *            (rounded rectangle).
 * Snapping keeps edges crisp at any size: fills move on-curve points to
 * pixel edges; strokes move them to pixel centers (odd pixel widths) or
 * edges (even widths); control points follow their end points. Circles,
 * ellipses and rounded rectangles snap their extreme edges.
 */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const char *const ui_icon_programs[UI_ICON_COUNT];
extern const char *const ui_icon_names[UI_ICON_COUNT];

const char *ui_icon_name(ui_icon id)
{
    if (id <= UI_ICON_NONE || id >= UI_ICON_COUNT) return "";
    return ui_icon_names[id];
}

ui_icon ui_icon_from_name(const char *name)
{
    if (!name) return UI_ICON_NONE;
    for (int i = 1; i < UI_ICON_COUNT; i++)
        if (strcmp(ui_icon_names[i], name) == 0) return (ui_icon)i;
    return UI_ICON_NONE;
}

/* ---- tiny parser --------------------------------------------------------- */
typedef struct ic_op {
    int   layer;          /* 0 soft, 1 accent, 2 line */
    bool  stroke, evenodd, erase, erase_all, snap;
    float width;          /* design units */
    int   cap, join;
    float opacity;
    bool  grad;
    float g[4];
} ic_op;

typedef struct ic_parse {
    const char *p;
    float       s;        /* pixels per unit */
    int         mode;     /* 0 none, 1 fill (pixel edges), 2 stroke odd, 3 stroke even */
} ic_parse;

static void skip_ws(ic_parse *ps)
{
    while (*ps->p == ' ' || *ps->p == ',' || *ps->p == '\n' || *ps->p == '\t') ps->p++;
}

static bool at_number(const ic_parse *ps)
{
    char c = *ps->p;
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

static float number(ic_parse *ps)
{
    float v = 0.0f, frac = 0.1f, sign = 1.0f;
    bool dot = false;
    skip_ws(ps);
    if (*ps->p == '-') { sign = -1.0f; ps->p++; }
    else if (*ps->p == '+') ps->p++;
    while ((*ps->p >= '0' && *ps->p <= '9') || (*ps->p == '.' && !dot)) {
        if (*ps->p == '.') { dot = true; ps->p++; continue; }
        if (dot) { v += (float)(*ps->p - '0') * frac; frac *= 0.1f; }
        else v = v * 10.0f + (float)(*ps->p - '0');
        ps->p++;
    }
    return v * sign;
}

static float snapv(const ic_parse *ps, float v)
{
    float x = v * ps->s;
    switch (ps->mode) {
    case 1: return roundf(x);
    case 2: return floorf(x) + 0.5f;
    case 3: return roundf(x);
    default: return x;
    }
}

/* Parse one op's path into p. */
static void parse_path(ic_parse *ps, ui_path *p)
{
    float cx = 0, cy = 0, sx = 0, sy = 0;        /* design-space current/start */
    float pcx = 0, pcy = 0;                      /* previous control point */
    float dx = 0, dy = 0;                        /* snap delta of current point */
    float s = ps->s;
    char cmd = 0, prev = 0;
    for (;;) {
        skip_ws(ps);
        if (*ps->p == '\0' || *ps->p == ';') break;
        if (!at_number(ps)) cmd = *ps->p++;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        if (!cmd) break;
        switch (cmd) {
        case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v': {
            float x = cx, y = cy, X, Y;
            bool rel = cmd >= 'a';
            char up = (char)(rel ? cmd - 32 : cmd);
            if (up == 'H') x = number(ps) + (rel ? cx : 0);
            else if (up == 'V') y = number(ps) + (rel ? cy : 0);
            else { x = number(ps) + (rel ? cx : 0); y = number(ps) + (rel ? cy : 0); }
            X = snapv(ps, x); Y = snapv(ps, y);
            if (up == 'M') { ui_path_move(p, X, Y); sx = x; sy = y; }
            else ui_path_line(p, X, Y);
            dx = X - x * s; dy = Y - y * s;
            cx = x; cy = y; pcx = x; pcy = y;
            break;
        }
        case 'C': case 'c': case 'S': case 's': {
            bool rel = cmd >= 'a', smooth = cmd == 'S' || cmd == 's';
            float x1, y1, x2, y2, x, y, X, Y;
            if (smooth) {
                bool pc = prev == 'C' || prev == 'c' || prev == 'S' || prev == 's';
                x1 = pc ? 2 * cx - pcx : cx;
                y1 = pc ? 2 * cy - pcy : cy;
            } else {
                x1 = number(ps) + (rel ? cx : 0); y1 = number(ps) + (rel ? cy : 0);
            }
            x2 = number(ps) + (rel ? cx : 0); y2 = number(ps) + (rel ? cy : 0);
            x = number(ps) + (rel ? cx : 0); y = number(ps) + (rel ? cy : 0);
            X = snapv(ps, x); Y = snapv(ps, y);
            ui_path_cubic(p, x1 * s + dx, y1 * s + dy, x2 * s + (X - x * s), y2 * s + (Y - y * s),
                          X, Y);
            dx = X - x * s; dy = Y - y * s;
            pcx = x2; pcy = y2; cx = x; cy = y;
            break;
        }
        case 'Q': case 'q': case 'T': case 't': {
            bool rel = cmd >= 'a', smooth = cmd == 'T' || cmd == 't';
            float x1, y1, x, y, X, Y, ex, ey;
            if (smooth) {
                bool pc = prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't';
                x1 = pc ? 2 * cx - pcx : cx;
                y1 = pc ? 2 * cy - pcy : cy;
            } else {
                x1 = number(ps) + (rel ? cx : 0); y1 = number(ps) + (rel ? cy : 0);
            }
            x = number(ps) + (rel ? cx : 0); y = number(ps) + (rel ? cy : 0);
            X = snapv(ps, x); Y = snapv(ps, y);
            ex = X - x * s; ey = Y - y * s;
            ui_path_quad(p, x1 * s + (dx + ex) * 0.5f, y1 * s + (dy + ey) * 0.5f, X, Y);
            dx = ex; dy = ey;
            pcx = x1; pcy = y1; cx = x; cy = y;
            break;
        }
        case 'A': case 'a': {
            bool rel = cmd == 'a';
            float rx = number(ps), ry = number(ps), rot = number(ps);
            bool large = number(ps) != 0.0f, sweep = number(ps) != 0.0f;
            float x = number(ps) + (rel ? cx : 0), y = number(ps) + (rel ? cy : 0);
            float X = snapv(ps, x), Y = snapv(ps, y);
            ui_path_arc(p, rx * s, ry * s, rot, large, sweep, X, Y);
            dx = X - x * s; dy = Y - y * s;
            cx = x; cy = y; pcx = x; pcy = y;
            break;
        }
        case 'Z': case 'z':
            ui_path_close(p);
            cx = sx; cy = sy; pcx = cx; pcy = cy;
            dx = snapv(ps, cx) - cx * s; dy = snapv(ps, cy) - cy * s;
            break;
        case 'O': case 'E': {
            float ox = number(ps), oy = number(ps), rx = number(ps);
            float ry = cmd == 'E' ? number(ps) : rx;
            float l = snapv(ps, ox - rx), r = snapv(ps, ox + rx);
            float t = snapv(ps, oy - ry), b = snapv(ps, oy + ry);
            ui_path_ellipse(p, (l + r) * 0.5f, (t + b) * 0.5f, (r - l) * 0.5f, (b - t) * 0.5f);
            break;
        }
        case 'R': {
            float x = number(ps), y = number(ps), w = number(ps), h = number(ps), rr = number(ps);
            float l = snapv(ps, x), t = snapv(ps, y), r = snapv(ps, x + w), b = snapv(ps, y + h);
            ui_path_rrect(p, l, t, r - l, b - t, rr * s);
            break;
        }
        default:
            /* unknown command: stop parsing this op */
            while (*ps->p && *ps->p != ';') ps->p++;
            return;
        }
        prev = cmd;
    }
    if (p->open) ui_path_end(p);
}

static void parse_header(ic_parse *ps, ic_op *op)
{
    memset(op, 0, sizeof *op);
    op->snap = true;
    op->opacity = 1.0f;
    op->cap = UI_CAP_ROUND;
    op->join = UI_JOIN_ROUND;
    skip_ws(ps);
    switch (*ps->p) {
    case 'S': op->layer = UI_ICON_LAYER_SOFT; break;
    case 'A': op->layer = UI_ICON_LAYER_ACCENT; break;
    default:  op->layer = UI_ICON_LAYER_LINE; break;
    }
    if (*ps->p) ps->p++;
    while (*ps->p && *ps->p != ' ' && *ps->p != ';') {
        char c = *ps->p++;
        switch (c) {
        case 'f': op->stroke = false; break;
        case 'e': op->evenodd = true; break;
        case 's': op->stroke = true; op->width = number(ps); break;
        case 'b': op->cap = UI_CAP_BUTT; break;
        case 'q': op->cap = UI_CAP_SQUARE; break;
        case 'm': op->join = UI_JOIN_MITER; break;
        case 'x': op->erase = true; break;
        case 'X': op->erase = true; op->erase_all = true; break;
        case 'n': op->snap = false; break;
        case 'o': op->opacity = number(ps); break;
        case 'g':
            op->grad = true;
            for (int i = 0; i < 4; i++) op->g[i] = number(ps);
            break;
        default: break;
        }
    }
}

static int32_t stroke_px(float width, float s)
{
    float w = width * s;
    int32_t px = (int32_t)floorf(w + 0.5f);
    return px < 1 ? 1 : px;
}

pc_status ui_icon_raster(ui_icon id, int size, uint8_t *out)
{
    const char *prog;
    ic_parse ps;
    ui_path path, stroke;
    uint8_t *tmp;
    size_t n;
    pc_status st = PC_OK;
    if (id <= UI_ICON_NONE || id >= UI_ICON_COUNT || size < UI_ICON_MIN_PX ||
        size > UI_ICON_MAX_PX || !out)
        return PC_ERR_ARG;
    n = (size_t)size * (size_t)size;
    memset(out, 0, n * UI_ICON_LAYERS);
    prog = ui_icon_programs[id];
    if (!prog) return PC_OK;
    tmp = (uint8_t *)malloc(n);
    if (!tmp) return PC_ERR_NOMEM;
    ui_path_init(&path, 0.08f);
    ui_path_init(&stroke, 0.08f);
    ps.p = prog;
    ps.s = (float)size / 16.0f;
    while (*ps.p && st == PC_OK) {
        ic_op op;
        const ui_path *fillp = &path;
        int32_t wpx = 0;
        parse_header(&ps, &op);
        if (op.stroke) {
            wpx = stroke_px(op.width, ps.s);
            ps.mode = op.snap ? ((wpx & 1) ? 2 : 3) : 0;
        } else {
            ps.mode = op.snap ? 1 : 0;
        }
        ui_path_reset(&path);
        parse_path(&ps, &path);
        if (*ps.p == ';') ps.p++;
        if (op.stroke) {
            ui_path_reset(&stroke);
            ui_path_stroke(&path, op.snap ? (float)wpx : op.width * ps.s, op.join, op.cap, 4.0f,
                           &stroke);
            fillp = &stroke;
        }
        if (path.oom || stroke.oom) { st = PC_ERR_NOMEM; break; }
        st = ui_raster_fill(fillp, op.evenodd && !op.stroke ? UI_FILL_EVENODD : UI_FILL_NONZERO,
                            true, tmp, size, size, size, UI_RASTER_SET);
        if (st != PC_OK) break;
        if (op.opacity < 1.0f || op.grad) {
            float gx0 = op.g[0] * ps.s, gy0 = op.g[1] * ps.s;
            float gdx = (op.g[2] - op.g[0]) * ps.s, gdy = (op.g[3] - op.g[1]) * ps.s;
            float gl2 = gdx * gdx + gdy * gdy;
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++) {
                    float f = op.opacity;
                    uint8_t *c = &tmp[(size_t)y * (size_t)size + (size_t)x];
                    if (!*c) continue;
                    if (op.grad && gl2 > 0.0f) {
                        float t =
                            (((float)x + 0.5f - gx0) * gdx + ((float)y + 0.5f - gy0) * gdy) / gl2;
                        t = ui_clampf(t, 0.0f, 1.0f);
                        f *= 0.18f + 0.82f * t;
                    }
                    *c = (uint8_t)((float)*c * ui_clampf(f, 0.0f, 1.0f) + 0.5f);
                }
        }
        for (int l = 0; l < UI_ICON_LAYERS; l++) {
            uint8_t *dst = out + (size_t)l * n;
            if (op.erase) {
                if (!op.erase_all && l != op.layer) continue;
                for (size_t i = 0; i < n; i++)
                    dst[i] = (uint8_t)((dst[i] * (255u - tmp[i]) + 127u) / 255u);
            } else if (l == op.layer) {
                for (size_t i = 0; i < n; i++)
                    dst[i] = (uint8_t)(dst[i] + tmp[i] - (dst[i] * tmp[i] + 127u) / 255u);
            }
        }
    }
    ui_path_free(&path);
    ui_path_free(&stroke);
    free(tmp);
    return st;
}

pc_status ui_icon_raster_rgba(ui_icon id, int size, ui_color line, ui_color accent,
                              ui_color soft, uint8_t *rgba)
{
    uint8_t *cov;
    size_t n;
    pc_status st;
    const ui_color tones[UI_ICON_LAYERS] = { soft, accent, line };
    if (size < UI_ICON_MIN_PX || size > UI_ICON_MAX_PX || !rgba) return PC_ERR_ARG;
    n = (size_t)size * (size_t)size;
    cov = (uint8_t *)malloc(n * UI_ICON_LAYERS);
    if (!cov) return PC_ERR_NOMEM;
    st = ui_icon_raster(id, size, cov);
    if (st != PC_OK) { free(cov); return st; }
    for (size_t i = 0; i < n; i++) {
        float r = 0, g = 0, b = 0, a = 0;      /* premultiplied accumulation */
        for (int l = 0; l < UI_ICON_LAYERS; l++) {
            float ca = (float)cov[(size_t)l * n + i] / 255.0f * (float)tones[l].a / 255.0f;
            r = (float)tones[l].r * ca + r * (1.0f - ca);
            g = (float)tones[l].g * ca + g * (1.0f - ca);
            b = (float)tones[l].b * ca + b * (1.0f - ca);
            a = ca + a * (1.0f - ca);
        }
        rgba[4 * i + 3] = (uint8_t)(a * 255.0f + 0.5f);
        rgba[4 * i + 0] = a > 0.0f ? (uint8_t)(r / a + 0.5f) : 0u;
        rgba[4 * i + 1] = a > 0.0f ? (uint8_t)(g / a + 0.5f) : 0u;
        rgba[4 * i + 2] = a > 0.0f ? (uint8_t)(b / a + 0.5f) : 0u;
    }
    free(cov);
    return PC_OK;
}

/* ---- atlas cache --------------------------------------------------------- */
bool ui_icon_sprites(ui_ctx *ctx, ui_icon icon, int32_t size, ui_sprite sp[UI_ICON_LAYERS])
{
    uint64_t base = ((uint64_t)UI_KEY_ICON << 56) | ((uint64_t)icon << 16) | ((uint64_t)size << 2);
    uint8_t *cov;
    size_t n = (size_t)size * (size_t)size;
    bool all = true;
    for (int l = 0; l < UI_ICON_LAYERS; l++)
        if (!ui_cache_get(ctx, base | (uint64_t)l, &sp[l])) all = false;
    if (all) return true;
    cov = (uint8_t *)malloc(n * UI_ICON_LAYERS);
    if (!cov) return false;
    if (ui_icon_raster(icon, size, cov) != PC_OK) { free(cov); return false; }
    for (int l = 0; l < UI_ICON_LAYERS; l++) {
        const uint8_t *c = cov + (size_t)l * n;
        int32_t x0 = size, y0 = size, x1 = -1, y1 = -1;
        uint8_t *dst;
        memset(&sp[l], 0, sizeof sp[l]);
        for (int32_t y = 0; y < size; y++)
            for (int32_t x = 0; x < size; x++)
                if (c[(size_t)y * (size_t)size + (size_t)x]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        if (x1 >= x0) {
            int32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
            if (!ui_atlas_alloc(ctx, w, h, &sp[l], &dst)) { free(cov); return false; }
            for (int32_t y = 0; y < h; y++)
                memcpy(dst + (size_t)y * UI_ATLAS_DIM,
                       c + (size_t)(y0 + y) * (size_t)size + (size_t)x0, (size_t)w);
            sp[l].ox = (int16_t)x0;
            sp[l].oy = (int16_t)y0;
        }
        ui_cache_put(ctx, base | (uint64_t)l, &sp[l]);
    }
    free(cov);
    return true;
}
