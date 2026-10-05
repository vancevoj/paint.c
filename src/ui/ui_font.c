/* ui_font.c - face loading, cmap, metrics, outlines and line measurement. */
#include "ui_font_internal.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char ui_font_data_inter_regular[];
extern const size_t ui_font_data_inter_regular_size;
extern const unsigned char ui_font_data_inter_semibold[];
extern const size_t ui_font_data_inter_semibold_size;

static SDL_AtomicInt g_serial;

/* ---- loading ------------------------------------------------------------- */
static void font_destroy(ui_font *f)
{
    if (!f) return;
    ui_kern_free(f);
    free(f->glyph_ok);
    free(f->cmap_cache);
    free(f->kern_cache);
    free(f->owned);
    free(f);
}

pc_status ui_font_load_mem(const void *data, size_t len, int face, uint32_t flags,
                           ui_font **out)
{
    ui_font *f;
    const uint8_t *d = (const uint8_t *)data;
    pc_status st;
    if (out) *out = NULL;
    if (!out || !data || len == 0) return PC_ERR_ARG;
    if (len > UI_FONT_MAX_FILE) return PC_ERR_LIMIT;
    f = (ui_font *)calloc(1u, sizeof *f);
    if (!f) return PC_ERR_NOMEM;
    if (flags & UI_FONT_COPY) {
        f->owned = (uint8_t *)malloc(len);
        if (!f->owned) { free(f); return PC_ERR_NOMEM; }
        memcpy(f->owned, data, len);
        d = f->owned;
    }
    st = ui_font_open(f, d, len, face);
    if (st != PC_OK) { font_destroy(f); return st; }
    f->serial = (uint32_t)SDL_AddAtomicInt(&g_serial, 1) + 1u;
    *out = f;
    return PC_OK;
}

ui_font *ui_font_load_builtin(ui_font_builtin which)
{
    ui_font *f = NULL;
    const unsigned char *d = which == UI_FONT_SEMIBOLD ? ui_font_data_inter_semibold
                                                       : ui_font_data_inter_regular;
    size_t n = which == UI_FONT_SEMIBOLD ? ui_font_data_inter_semibold_size
                                         : ui_font_data_inter_regular_size;
    if (ui_font_load_mem(d, n, 0, 0, &f) != PC_OK) return NULL;
    return f;
}

pc_status ui_font_load_file(const char *path, int face, ui_font **out)
{
    SDL_IOStream *io;
    Sint64 size;
    uint8_t *buf;
    pc_status st;
    if (out) *out = NULL;
    if (!path || !out) return PC_ERR_ARG;
    io = SDL_IOFromFile(path, "rb");
    if (!io) return PC_ERR_IO;
    size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return size == 0 ? PC_ERR_FORMAT : PC_ERR_IO; }
    if ((uint64_t)size > UI_FONT_MAX_FILE) { SDL_CloseIO(io); return PC_ERR_LIMIT; }
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return PC_ERR_NOMEM; }
    if (SDL_ReadIO(io, buf, (size_t)size) != (size_t)size) {
        SDL_CloseIO(io);
        free(buf);
        return PC_ERR_IO;
    }
    SDL_CloseIO(io);
    st = ui_font_load_mem(buf, (size_t)size, face, 0, out);
    if (st != PC_OK) { free(buf); return st; }
    (*out)->owned = buf;
    return PC_OK;
}

void ui_font_free(ui_font *f) { font_destroy(f); }

pc_status ui_font_add_fallback(ui_font *f, const ui_font *fallback)
{
    if (!f || !fallback || fallback == f) return PC_ERR_ARG;
    if (f->n_fallback >= UI_FONT_MAX_FALLBACKS) return PC_ERR_LIMIT;
    f->fallback[f->n_fallback++] = fallback;
    if (f->cmap_cache) memset(f->cmap_cache, 0, UI_CMAP_DIRECT * sizeof(ui_cmap_slot));
    return PC_OK;
}

int ui_font_face_count(const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    ui_sfnt s;
    if (!d || len < 12u) return 0;
    if (memcmp(d, "ttcf", 4) == 0) {
        uint32_t n =
            ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
        if (n > 4096u || 12u + 4u * (size_t)n > len) return 0;
        return (int)n;
    }
    return ui_sfnt_locate(&s, d, len, 0) == PC_OK ? 1 : 0;
}

pc_status ui_font_describe(const void *data, size_t len, int face, ui_font_desc *out)
{
    ui_sfnt s;
    pc_status st;
    uint32_t fs = 0;
    if (!out) return PC_ERR_ARG;
    memset(out, 0, sizeof *out);
    st = ui_sfnt_locate(&s, (const uint8_t *)data, len, face);
    if (st != PC_OK) return st;
    if (!ui_sfnt_name(&s, 16, out->family, sizeof out->family))
        (void)ui_sfnt_name(&s, 1, out->family, sizeof out->family);
    if (!ui_sfnt_name(&s, 17, out->style, sizeof out->style))
        (void)ui_sfnt_name(&s, 2, out->style, sizeof out->style);
    (void)ui_sfnt_name(&s, 4, out->full_name, sizeof out->full_name);
    out->weight = (uint16_t)(s.os2.len >= 6u ? ui_rd16(&s, s.os2, 4) : 400u);
    if (out->weight == 0) out->weight = 400u;
    if (s.os2.len >= 64u) fs = ui_rd16(&s, s.os2, 62);
    out->italic = (fs & 1u) != 0 || (s.head.len >= 46u && (ui_rd16(&s, s.head, 44) & 2u));
    out->cff = s.cff.off != 0 && s.glyf.off == 0;
    if (!out->family[0]) return PC_ERR_FORMAT;
    return PC_OK;
}

void ui_font_get_desc(const ui_font *f, ui_font_desc *out) { *out = f->desc; }

/* ---- cmap and metrics ---------------------------------------------------- */
static uint32_t cmap_raw(const ui_font *f, uint32_t cp)
{
    const ui_sfnt *s = &f->s;
    ui_tbl t = s->cmap;
    uint32_t b = f->cmap_sub;
    switch (f->cmap_fmt) {
    case 0:
        return cp < 256u ? ui_rd8(s, t, b + 6u + cp) : 0u;
    case 6: {
        uint32_t first = ui_rd16(s, t, b + 6u), n = ui_rd16(s, t, b + 8u);
        if (cp < first || cp - first >= n) return 0;
        return ui_rd16(s, t, b + 10u + 2u * (cp - first));
    }
    case 4: {
        uint32_t seg2 = ui_rd16(s, t, b + 6u), segs = seg2 / 2u;
        int32_t lo = 0, hi = (int32_t)segs - 1;
        if (cp > 0xFFFFu || segs == 0) return 0;
        while (lo < hi) {                     /* first segment with end >= cp */
            int32_t mid = (lo + hi) / 2;
            if (ui_rd16(s, t, b + 14u + 2u * (uint32_t)mid) < cp) lo = mid + 1;
            else hi = mid;
        }
        {
            uint32_t i = (uint32_t)lo;
            uint32_t end = ui_rd16(s, t, b + 14u + 2u * i);
            uint32_t start = ui_rd16(s, t, b + 16u + seg2 + 2u * i);
            uint32_t delta = ui_rd16(s, t, b + 16u + 2u * seg2 + 2u * i);
            uint32_t ro_pos = b + 16u + 3u * seg2 + 2u * i;
            uint32_t ro = ui_rd16(s, t, ro_pos), gid;
            if (cp > end || cp < start) return 0;
            if (ro == 0) return (cp + delta) & 0xFFFFu;
            gid = ui_rd16(s, t, ro_pos + ro + 2u * (cp - start));
            return gid ? (gid + delta) & 0xFFFFu : 0u;
        }
    }
    case 12:
    case 13: {
        uint32_t n = ui_rd32(s, t, b + 12u);
        int32_t lo = 0, hi;
        if (!ui_tbl_has(t, b + 16u, 12u) || n > (t.len - b - 16u) / 12u) return 0;
        hi = (int32_t)n - 1;
        while (lo <= hi) {
            int32_t mid = lo + (hi - lo) / 2;
            uint32_t r = b + 16u + 12u * (uint32_t)mid;
            uint32_t a = ui_rd32(s, t, r), e = ui_rd32(s, t, r + 4u);
            if (cp < a) hi = mid - 1;
            else if (cp > e) lo = mid + 1;
            else {
                uint32_t g0 = ui_rd32(s, t, r + 8u);
                return f->cmap_fmt == 12u ? g0 + (cp - a) : g0;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

uint32_t ui_font_cmap(const ui_font *f, uint32_t cp)
{
    uint32_t g = cmap_raw(f, cp);
    if (!g && f->cmap_symbol && cp < 0x100u) g = cmap_raw(f, 0xF000u + cp);
    return g < (uint32_t)f->num_glyphs ? g : 0u;
}

int32_t ui_font_advance(const ui_font *f, uint32_t gid)
{
    uint32_t i = gid < (uint32_t)f->num_hmetrics ? gid : (uint32_t)f->num_hmetrics - 1u;
    return (int32_t)ui_rd16(&f->s, f->s.hmtx, 4u * i);
}

bool ui_font_has_glyph(const ui_font *f, uint32_t cp)
{
    return f && ui_font_cmap(f, cp) != 0;
}

uint32_t ui_font_resolve(const ui_font *f, uint32_t cp, const ui_font **face)
{
    uint32_t g = ui_font_cmap(f, cp);
    *face = f;
    if (g) return g;
    for (int32_t i = 0; i < f->n_fallback; i++) {
        g = ui_font_cmap(f->fallback[i], cp);
        if (g) { *face = f->fallback[i]; return g; }
    }
    return 0;
}

uint32_t ui_font_resolve_cached(ui_font *f, uint32_t cp, const ui_font **face)
{
    ui_cmap_slot *sl;
    if (cp >= UI_CMAP_DIRECT) return ui_font_resolve(f, cp, face);
    if (!f->cmap_cache) {
        f->cmap_cache = (ui_cmap_slot *)calloc(UI_CMAP_DIRECT, sizeof(ui_cmap_slot));
        if (!f->cmap_cache) return ui_font_resolve(f, cp, face);
    }
    sl = &f->cmap_cache[cp];
    if (!sl->valid) {
        const ui_font *src;
        uint32_t g = ui_font_resolve(f, cp, &src);
        uint8_t idx = 0;
        for (int32_t i = 0; i < f->n_fallback; i++)
            if (f->fallback[i] == src) idx = (uint8_t)(i + 1);
        sl->gid = (uint16_t)g;
        sl->src = idx;
        sl->valid = 1;
    }
    *face = sl->src ? f->fallback[sl->src - 1u] : f;
    return sl->gid;
}

int32_t ui_kern_cached(ui_font *f, uint32_t g1, uint32_t g2)
{
    uint32_t key = (g1 << 16) | (g2 & 0xFFFFu), h;
    ui_kern_slot *sl;
    if (f->n_kern_lookups == 0 && !f->has_kern_table) return 0;
    if (!f->kern_cache) {
        f->kern_cache = (ui_kern_slot *)calloc(UI_KERN_CACHE, sizeof(ui_kern_slot));
        if (!f->kern_cache) return ui_kern_lookup(f, g1, g2);
    }
    h = (key * 2654435761u) >> 20;
    sl = &f->kern_cache[h & (UI_KERN_CACHE - 1u)];
    if (!sl->valid || sl->key != key) {
        int32_t v = ui_kern_lookup(f, g1, g2);
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        sl->key = key;
        sl->val = (int16_t)v;
        sl->valid = 1;
    }
    return sl->val;
}

static float clamp_size(float size)
{
    if (!(size > 0.0f)) return 1.0f;
    return size > UI_FONT_MAX_SIZE ? UI_FONT_MAX_SIZE : size;
}

void ui_font_get_metrics(const ui_font *f, float size_px, ui_font_metrics *m)
{
    float s = clamp_size(size_px) / f->upem;
    m->ascent = (float)f->ascent * s;
    m->descent = (float)f->descent * s;
    m->line_gap = (float)f->line_gap * s;
    m->line_height = m->ascent + m->descent + m->line_gap;
    m->cap_height = (float)f->cap_height * s;
    m->x_height = (float)f->x_height * s;
    m->underline_pos = (float)f->ul_pos * s;
    m->underline_size = (float)f->ul_size * s;
    m->strike_pos = (float)f->st_pos * s;
    m->strike_size = (float)f->st_size * s;
}

/* ---- vertical zone snapping ---------------------------------------------- */
void ui_ymap_init(ui_ymap *m, const ui_font *f, float scale, bool hinted)
{
    m->scale = scale;
    m->n = 0;
    if (!hinted) return;
    {
        float zu[5];
        int n = 0;
        zu[n++] = 0.0f;
        if (f->x_height > 0) zu[n++] = (float)f->x_height;
        if (f->cap_height > f->x_height) zu[n++] = (float)f->cap_height;
        if ((float)f->ascent > zu[n - 1]) zu[n++] = (float)f->ascent;
        m->u[0] = -(float)f->descent;
        m->px[0] = -roundf((float)f->descent * scale);
        m->n = 1;
        if (!(m->u[0] < 0.0f)) m->n = 0;
        for (int i = 0; i < n; i++) {
            float p = roundf(zu[i] * scale);
            if (m->n > 0 && (zu[i] <= m->u[m->n - 1] || p < m->px[m->n - 1])) continue;
            m->u[m->n] = zu[i];
            m->px[m->n] = p;
            m->n++;
        }
    }
}

float ui_ymap_apply(const ui_ymap *m, float y)
{
    if (m->n < 2) return y * m->scale;
    if (y <= m->u[0]) return m->px[0] + (y - m->u[0]) * m->scale;
    for (int i = 1; i < m->n; i++) {
        if (y <= m->u[i]) {
            float t = (y - m->u[i - 1]) / (m->u[i] - m->u[i - 1]);
            return m->px[i - 1] + t * (m->px[i] - m->px[i - 1]);
        }
    }
    return m->px[m->n - 1] + (y - m->u[m->n - 1]) * m->scale;
}

bool ui_font_glyph_path(const ui_font *f, uint32_t gid, float scale, const ui_ymap *ym,
                        float ox, float oy, float shear, ui_path *p)
{
    stbtt_vertex *v = NULL;
    int n;
    bool any = false;
    if (!ui_font_glyph_ok(f, gid)) return false;
    n = stbtt_GetGlyphShape(&f->info, (int)gid, &v);
    if (n <= 0 || !v) { stbtt_FreeShape(&f->info, v); return false; }
    for (int i = 0; i < n; i++) {
        float yu = ui_ymap_apply(ym, (float)v[i].y);
        float x = ox + (float)v[i].x * scale + shear * yu, y = oy - yu;
        switch (v[i].type) {
        case STBTT_vmove:
            if (any) ui_path_close(p);
            ui_path_move(p, x, y);
            any = true;
            break;
        case STBTT_vline:
            ui_path_line(p, x, y);
            break;
        case STBTT_vcurve: {
            float cyu = ui_ymap_apply(ym, (float)v[i].cy);
            ui_path_quad(p, ox + (float)v[i].cx * scale + shear * cyu, oy - cyu, x, y);
            break;
        }
        case STBTT_vcubic: {
            float c1 = ui_ymap_apply(ym, (float)v[i].cy), c2 = ui_ymap_apply(ym, (float)v[i].cy1);
            ui_path_cubic(p, ox + (float)v[i].cx * scale + shear * c1, oy - c1,
                          ox + (float)v[i].cx1 * scale + shear * c2, oy - c2, x, y);
            break;
        }
        default:
            break;
        }
    }
    if (any) ui_path_close(p);
    stbtt_FreeShape(&f->info, v);
    return any;
}

/* ---- single-line measurement --------------------------------------------- */
/* Iterates glyph pen positions exactly as ui_draw_text places them. */
typedef struct line_iter {
    ui_font       *f;
    float          size, x;
    const ui_font *prev_face;
    uint32_t       prev_gid;
} line_iter;

static float glyph_step(line_iter *it, uint32_t cp, const ui_font **face, uint32_t *gid)
{
    float adv, s;
    if (cp < 0x20u || cp == 0x7Fu) { *gid = 0; *face = NULL; return 0.0f; }
    *gid = ui_font_resolve_cached(it->f, cp, face);
    s = it->size / (*face)->upem;
    if (it->prev_face == *face && *gid && it->prev_gid)
        it->x += (float)ui_kern_cached((ui_font *)*face, it->prev_gid, *gid) * s;
    adv = *gid || *face != it->f ? (float)ui_font_advance(*face, *gid) * s
                                 : it->size * 0.6f;   /* box for missing glyphs */
    it->prev_face = *face;
    it->prev_gid = *gid;
    return adv;
}

float ui_text_width(ui_font *f, float size_px, const char *s, size_t len)
{
    line_iter it;
    size_t i = 0;
    if (!f || !s) return 0.0f;
    memset(&it, 0, sizeof it);
    it.f = f;
    it.size = clamp_size(size_px);
    while (i < len) {
        const ui_font *face;
        uint32_t gid, cp = ui_utf8_decode(s, len, &i);
        float adv = glyph_step(&it, cp, &face, &gid);
        it.x += adv;
    }
    return it.x;
}

size_t ui_text_fit(ui_font *f, float size_px, const char *s, size_t len, float max_w)
{
    line_iter it;
    size_t i = 0;
    if (!f || !s) return 0;
    memset(&it, 0, sizeof it);
    it.f = f;
    it.size = clamp_size(size_px);
    while (i < len) {
        const ui_font *face;
        uint32_t gid;
        size_t k = i;
        uint32_t cp = ui_utf8_decode(s, len, &k);
        float adv = glyph_step(&it, cp, &face, &gid);
        if (it.x + adv > max_w + 0.01f) return i;
        it.x += adv;
        i = k;
    }
    return len;
}

size_t ui_text_hit(ui_font *f, float size_px, const char *s, size_t len, float x)
{
    line_iter it;
    size_t i = 0;
    if (!f || !s || x <= 0.0f) return 0;
    memset(&it, 0, sizeof it);
    it.f = f;
    it.size = clamp_size(size_px);
    while (i < len) {
        const ui_font *face;
        uint32_t gid;
        size_t k = i;
        uint32_t cp = ui_utf8_decode(s, len, &k);
        float adv = glyph_step(&it, cp, &face, &gid);
        if (x < it.x + adv * 0.5f) return i;
        it.x += adv;
        i = k;
    }
    return len;
}

float ui_text_caret_x(ui_font *f, float size_px, const char *s, size_t len, size_t pos)
{
    line_iter it;
    size_t i = 0;
    if (!f || !s) return 0.0f;
    if (pos > len) pos = len;
    memset(&it, 0, sizeof it);
    it.f = f;
    it.size = clamp_size(size_px);
    while (i < pos) {
        const ui_font *face;
        uint32_t gid;
        uint32_t cp = ui_utf8_decode(s, len, &i);
        float adv = glyph_step(&it, cp, &face, &gid);
        it.x += adv;
    }
    return it.x;
}
