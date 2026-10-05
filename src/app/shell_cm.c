/* shell_cm.c - lane SHELL (wave 3b): display color management for the
 * canvas (V-RENDER-CM), the Colors window (W-COL-CM), Settings > Color
 * Management and Image > Color Profile (the display's profile as a
 * choice). See shell_ext.h.
 *
 * paint.c keeps an image's pixels in the image's own profile (the
 * meta.icc of the document; none means sRGB, edit/m_profile.h). The view
 * converts them for the screen: to sRGB (Paint.NET's "sRGB mode" on SDR
 * displays), or to the display's own profile when Settings > Color
 * Management asks for it and the platform reports one
 * (SDL_GetWindowICCProfile: Windows GetICMProfile, macOS ColorSync, X11
 * _ICC_PROFILE; Wayland and headless report none).
 *
 * The transform is a 33 x 33 x 33 lookup table built once per (image
 * profile, destination) pair: Little-CMS converts the grid to sRGB
 * (pc_icc_to_srgb_px, any RGB or gray profile), and a matrix/TRC display
 * profile is reached from there with edit/m_icc.h (or directly from a
 * matrix/TRC image profile, so wide gamut images keep their gamut on wide
 * gamut displays). Pixels are converted with trilinear interpolation; an
 * image whose profile equals the destination is shown unchanged (no
 * table, key 0).
 *
 * Thread rules: main thread for the state; app_cm_xform_bgra_premul only
 * reads an immutable table and may run anywhere while the table lives.
 * Ownership: state in app_ext "shell.cm" (freed with the app); the display
 * profile bytes are owned by the state. */
#include "app_internal.h"
#include "edit/m_icc.h"
#include "pc/pc_icc.h"
#include "shell_ext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CM_N 33u                          /* grid points per axis */
#define CM_DISPLAY_POLL_MS 2000u          /* re-read the display profile at most this often */

typedef struct shell_cm {
    bool      use_display;                /* Settings > Color Management */
    /* display profile */
    uint8_t  *disp;                       /* owned */
    size_t    disp_len;
    uint64_t  disp_hash;
    bool      disp_override;              /* tests */
    bool      disp_dirty;                 /* re-read before the next use */
    uint64_t  disp_ms;
    bool      disp_srgb;                  /* the display profile is equivalent to sRGB */
    /* the last image profile seen (inspecting a profile is not free) */
    const uint8_t *src_ptr;
    size_t    src_len;
    uint64_t  src_head;                   /* hash of its first bytes */
    uint64_t  src_h;                      /* 0: none, unreadable or equivalent to sRGB */
    /* the lookup table of the last image profile */
    uint64_t  key;                        /* 0 = identity */
    uint64_t  src_hash, dst_hash;         /* inputs the table was built for */
    bool      built;
    bool      failed;                     /* the image profile could not be converted */
    uint8_t  *lut;                        /* CM_N^3 x (b, g, r), owned */
    /* per-channel grid index and weight of every code value */
    uint8_t   idx[256];
    float     frac[256];
} shell_cm;

static void cm_free(void *p)
{
    shell_cm *c = (shell_cm *)p;
    if (!c) return;
    free(c->disp);
    free(c->lut);
    free(c);
}

static shell_cm *cm_state(const app *a)
{
    shell_cm *c = (shell_cm *)app_ext_get(a, "shell.cm");
    if (!c) {
        c = (shell_cm *)calloc(1u, sizeof *c);
        if (!c) return NULL;
        c->disp_dirty = true;
        for (uint32_t v = 0; v < 256u; v++) {
            double f = (double)v * (double)(CM_N - 1u) / 255.0;
            uint32_t i = (uint32_t)f;
            if (i >= CM_N - 1u) i = CM_N - 2u;
            c->idx[v] = (uint8_t)i;
            c->frac[v] = (float)(f - (double)i);
        }
        if (!app_ext_set((app *)(uintptr_t)a, "shell.cm", c, cm_free)) {
            free(c);
            return NULL;
        }
    }
    return c;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1u;
}

/* ---- display profile ------------------------------------------------------------------------ */
static void display_refresh(app *a, shell_cm *c)
{
    void *p;
    size_t n = 0;
    if (c->disp_override) return;
    if (!c->disp_dirty && a->now - c->disp_ms < CM_DISPLAY_POLL_MS) return;
    c->disp_dirty = false;
    c->disp_ms = a->now;
    p = a->win ? SDL_GetWindowICCProfile(a->win, &n) : NULL;
    if (p && n > 0u && n <= PC_ICC_MAX_BYTES) {
        uint64_t h = fnv((const uint8_t *)p, n);
        if (h != c->disp_hash || !c->disp) {
            uint8_t *copy = (uint8_t *)malloc(n);
            if (copy) {
                pc_icc_info info;
                memcpy(copy, p, n);
                if (pc_icc_inspect(copy, n, &info) == PC_OK && info.space == PC_ICC_SPACE_RGB) {
                    free(c->disp);
                    c->disp = copy;
                    c->disp_len = n;
                    c->disp_hash = h;
                    c->disp_srgb = info.is_srgb;
                } else {
                    free(copy);
                }
            }
        }
    } else if (c->disp) {
        free(c->disp);
        c->disp = NULL;
        c->disp_len = 0;
        c->disp_hash = 0;
    }
    if (p) SDL_free(p);
}

void app_cm_display_changed(app *a)
{
    shell_cm *c = cm_state(a);
    if (c) c->disp_dirty = true;
    app_request_frame(a);
}

const uint8_t *app_cm_display_profile(app *a, size_t *len)
{
    shell_cm *c = cm_state(a);
    if (len) *len = 0;
    if (!c) return NULL;
    display_refresh(a, c);
    if (len) *len = c->disp ? c->disp_len : 0u;
    return c->disp;
}

void app_cm_set_display_profile_override(app *a, const uint8_t *icc, size_t len)
{
    shell_cm *c = cm_state(a);
    if (!c) return;
    free(c->disp);
    c->disp = NULL;
    c->disp_len = 0;
    c->disp_hash = 0;
    c->disp_override = icc != NULL;
    c->disp_dirty = true;
    if (icc && len) {
        pc_icc_info info;
        c->disp = (uint8_t *)malloc(len);
        if (c->disp) {
            memcpy(c->disp, icc, len);
            c->disp_len = len;
            c->disp_hash = fnv(icc, len);
            c->disp_srgb = pc_icc_inspect(icc, len, &info) == PC_OK && info.is_srgb;
        }
    }
    app_request_frame(a);
}

void app_cm_display_describe(app *a, char *out, size_t cap)
{
    size_t n = 0;
    const uint8_t *p = app_cm_display_profile(a, &n);
    pc_icc_info info;
    if (cap) out[0] = '\0';
    if (!p) return;
    if (pc_icc_inspect(p, n, &info) == PC_OK)
        snprintf(out, cap, "%s", info.desc[0] ? info.desc : "Unnamed display profile");
}

void app_cm_set_use_display(app *a, bool on)
{
    shell_cm *c = cm_state(a);
    if (c && c->use_display != on) {
        c->use_display = on;
        app_request_frame(a);
    }
}

bool app_cm_use_display(const app *a)
{
    shell_cm *c = cm_state(a);
    return c && c->use_display;
}

/* ---- the lookup table ------------------------------------------------------------------------ */
/* Destination: the display profile when it is used and present, else sRGB
 * (hash 0). A display profile equivalent to sRGB counts as sRGB. */
static uint64_t dest_hash(app *a, shell_cm *c)
{
    if (!c->use_display) return 0u;
    display_refresh(a, c);
    if (!c->disp || c->disp_srgb) return 0u;
    return c->disp_hash;
}

static uint64_t src_hash(shell_cm *c, const app_doc *d)
{
    pc_icc_info info;
    uint64_t head;
    if (!d || !d->meta.icc || !d->meta.icc_len) return 0u;
    head = fnv(d->meta.icc, d->meta.icc_len < 160u ? d->meta.icc_len : 160u);
    if (d->meta.icc == c->src_ptr && d->meta.icc_len == c->src_len && head == c->src_head)
        return c->src_h;
    c->src_ptr = d->meta.icc;
    c->src_len = d->meta.icc_len;
    c->src_head = head;
    c->src_h = 0u;
    if (pc_icc_inspect(d->meta.icc, d->meta.icc_len, &info) == PC_OK && !info.is_srgb)
        c->src_h = fnv(d->meta.icc, d->meta.icc_len);
    return c->src_h;
}

/* Build the table from the image profile (src, may be NULL = sRGB) to the
 * destination (dst, NULL = sRGB). false when nothing could be converted. */
static bool build_lut(shell_cm *c, const uint8_t *src, size_t slen, const uint8_t *dst,
                      size_t dlen)
{
    size_t n = (size_t)CM_N * CM_N * CM_N;
    pc_px32 *g = (pc_px32 *)malloc(n * sizeof *g);
    m_icc_rgb rs, rd;
    m_icc_xform *x = NULL;
    bool direct = false, ok = true;
    if (!g) return false;
    if (!c->lut) c->lut = (uint8_t *)malloc(n * 3u);
    if (!c->lut) {
        free(g);
        return false;
    }
    for (uint32_t b = 0; b < CM_N; b++)
        for (uint32_t gg = 0; gg < CM_N; gg++)
            for (uint32_t r = 0; r < CM_N; r++) {
                pc_px32 *p = &g[((size_t)b * CM_N + gg) * CM_N + r];
                p->r = (uint8_t)((r * 255u + (CM_N - 1u) / 2u) / (CM_N - 1u));
                p->g = (uint8_t)((gg * 255u + (CM_N - 1u) / 2u) / (CM_N - 1u));
                p->b = (uint8_t)((b * 255u + (CM_N - 1u) / 2u) / (CM_N - 1u));
                p->a = 255u;
            }
    if (dst && m_icc_parse(dst, dlen, &rd) == PC_OK) {
        x = (m_icc_xform *)malloc(sizeof *x);
        if (src && m_icc_parse(src, slen, &rs) == PC_OK) direct = true;
        else m_icc_builtin_rgb(M_ICC_SRGB, &rs);
        if (!x || !m_icc_xform_init(x, &rs, &rd)) {
            free(x);
            x = NULL;
            direct = false;
        }
    }
    if (src && !direct) {
        /* the image profile to sRGB with Little-CMS (one call, the grid as
         * a CM_N^2 x CM_N image) */
        if (pc_icc_to_srgb_px(src, slen, g, (int32_t)(CM_N * CM_N), (int32_t)CM_N,
                              (size_t)CM_N * CM_N) != PC_OK)
            ok = false;
    }
    if (ok && x) m_icc_xform_px(x, g, n);
    free(x);
    if (ok) {
        for (size_t i = 0; i < n; i++) {
            c->lut[3u * i] = g[i].b;
            c->lut[3u * i + 1u] = g[i].g;
            c->lut[3u * i + 2u] = g[i].r;
        }
    }
    free(g);
    return ok;
}

/* Bring the table up to date for d; returns its key (0 = identity). */
static uint64_t cm_prepare(app *a, shell_cm *c, const app_doc *d)
{
    uint64_t s = src_hash(c, d), t = dest_hash(a, c);
    if (s == t) return 0;                /* same profile on both sides (or both sRGB) */
    if (c->built && c->src_hash == s && c->dst_hash == t) return c->failed ? 0u : c->key;
    c->built = true;
    c->src_hash = s;
    c->dst_hash = t;
    c->failed = !build_lut(c, s ? d->meta.icc : NULL, s ? d->meta.icc_len : 0u,
                           t ? c->disp : NULL, t ? c->disp_len : 0u);
    c->key = c->failed ? 0u : (s * 31u) ^ (t * 0x9E3779B97F4A7C15ull) ^ 0x5Au;
    if (!c->key && !c->failed) c->key = 1u;
    return c->failed ? 0u : c->key;
}

uint64_t app_cm_view_key(app *a, const app_doc *d)
{
    shell_cm *c = cm_state(a);
    return c && d ? cm_prepare(a, c, d) : 0u;
}

static void lut_px(const shell_cm *c, uint8_t *b, uint8_t *g, uint8_t *r)
{
    uint32_t ib = c->idx[*b], ig = c->idx[*g], ir = c->idx[*r];
    float fb = c->frac[*b], fg = c->frac[*g], fr = c->frac[*r];
    const uint8_t *t = c->lut;
    size_t s1 = 3u, s2 = 3u * CM_N, s3 = 3u * CM_N * CM_N;
    size_t o = (size_t)ib * s3 + (size_t)ig * s2 + (size_t)ir * s1;
    for (int ch = 0; ch < 3; ch++) {
        const uint8_t *q = t + o + (size_t)ch;
        float c00 = (float)q[0] + ((float)q[s1] - (float)q[0]) * fr;
        float c01 = (float)q[s2] + ((float)q[s2 + s1] - (float)q[s2]) * fr;
        float c10 = (float)q[s3] + ((float)q[s3 + s1] - (float)q[s3]) * fr;
        float c11 = (float)q[s3 + s2] + ((float)q[s3 + s2 + s1] - (float)q[s3 + s2]) * fr;
        float c0 = c00 + (c01 - c00) * fg, c1 = c10 + (c11 - c10) * fg;
        float v = c0 + (c1 - c0) * fb;
        uint8_t o8 = (uint8_t)(v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (int)(v + 0.5f)));
        if (ch == 0) *b = o8;
        else if (ch == 1) *g = o8;
        else *r = o8;
    }
}

static uint8_t unpremul(uint32_t v, uint32_t al)
{
    uint32_t s = (v * 255u + al / 2u) / al;
    return (uint8_t)(s > 255u ? 255u : s);
}

/* gfx display transform callback: premultiplied BGRA in place. */
static void cm_xform_premul(void *ud, uint8_t *px, size_t n)
{
    const shell_cm *c = (const shell_cm *)ud;
    for (size_t i = 0; i < n; i++, px += 4) {
        uint32_t al = px[3];
        if (al == 0u) continue;
        if (al == 255u) {
            lut_px(c, &px[0], &px[1], &px[2]);
        } else {
            uint8_t b = unpremul(px[0], al), g = unpremul(px[1], al), r = unpremul(px[2], al);
            lut_px(c, &b, &g, &r);
            px[0] = (uint8_t)pc_mul255(b, al);
            px[1] = (uint8_t)pc_mul255(g, al);
            px[2] = (uint8_t)pc_mul255(r, al);
        }
    }
}

bool app_cm_gfx_style(app *a, const app_doc *d, gfx_style *st)
{
    shell_cm *c = cm_state(a);
    uint64_t k = c && d ? cm_prepare(a, c, d) : 0u;
    st->xf = k ? cm_xform_premul : NULL;
    st->xf_ud = k ? c : NULL;
    st->xf_key = k;
    return k != 0u;
}

void app_cm_to_display(app *a, const app_doc *d, pc_px32 *px, size_t n)
{
    shell_cm *c = cm_state(a);
    if (!c || !d || !cm_prepare(a, c, d)) return;
    for (size_t i = 0; i < n; i++) lut_px(c, &px[i].b, &px[i].g, &px[i].r);
}

void app_cm_status(app *a, char *out, size_t cap)
{
    shell_cm *c = cm_state(a);
    char desc[160];
    app_cm_display_describe(a, desc, sizeof desc);
    if (!c) {
        if (cap) out[0] = '\0';
        return;
    }
    if (c->use_display && desc[0])
        snprintf(out, cap, "Images are shown converted to the display profile \"%s\".", desc);
    else if (c->use_display)
        snprintf(out, cap, "The display reports no color profile, so images are shown "
                           "converted to sRGB.");
    else
        snprintf(out, cap, "Images are shown converted to sRGB (standard dynamic range).");
}
