/* m_paste.c - lane M: Paste, Paste into New Layer, Paste into New Image and
 * the float hook for Move Selected Pixels (see m_paste.h). */
#include "m_paste.h"

#include "../app_internal.h"
#include "app/app_float.h"
#include "m_hist.h"
#include "m_icc.h"
#include "pc/pc_geom.h"
#include "pc/pc_icc.h"
#include "pc/pc_layerops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_HOOK_KEY      "lane_m.float_hook"
#define M_CLIPCACHE_KEY "lane_m.clip_cache"
#define M_MAX_FILE      ((uint64_t)3u << 30)     /* as File > Open */
#define M_MAX_TEXT      ((size_t)1u << 30)       /* data URI text we decode */

/* ---- float hook ----------------------------------------------------------------------- */
bool m_paste_set_float_hook(app *a, const m_float_hook *hook)
{
    m_float_hook *h;
    if (!hook) return app_ext_set(a, M_HOOK_KEY, NULL, NULL);
    h = (m_float_hook *)malloc(sizeof *h);
    if (!h) return false;
    *h = *hook;
    if (!app_ext_set(a, M_HOOK_KEY, h, free)) {
        free(h);
        return false;
    }
    return true;
}

const m_float_hook *m_paste_float_hook(const app *a)
{
    const m_float_hook *h = (const m_float_hook *)app_ext_get(a, M_HOOK_KEY);
    return h && h->paste ? h : NULL;
}

/* ---- position (CB-PASTE-POS) ------------------------------------------------------------ */
void m_paste_position(app *a, app_doc *d, int32_t w, int32_t h, int32_t *x, int32_t *y)
{
    gfx_view v = app_doc_gview(a, d);
    double vx0, vy0, vx1, vy1;
    int32_t px = 0, py = 0, dw = (int32_t)d->doc->w, dh = (int32_t)d->doc->h;
    gfx_view_visible(&v, &vx0, &vy0, &vx1, &vy1);
    /* the first fully visible pixel when the origin is scrolled out of view */
    if (vx1 > vx0 && vy1 > vy0) {
        if (vx0 > 0.0) px = (int32_t)ceil(vx0 - 1e-6);
        if (vy0 > 0.0) py = (int32_t)ceil(vy0 - 1e-6);
    }
    /* keep the pasted pixels inside the canvas when they fit */
    if (px + w > dw) px = dw - w;
    if (py + h > dh) py = dh - h;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    *x = px;
    *y = py;
}

/* ---- clipboard text: file names and data URIs -------------------------------------------- */
typedef enum clip_text_kind { CT_NONE = 0, CT_FILE, CT_DATA_URI } clip_text_kind;

typedef struct clip_cache {
    uint64_t       at;          /* app time of the last text check */
    bool           valid;
    clip_text_kind kind;
    uint64_t       any_at;      /* app time of the last overall answer */
    bool           any_valid, any;
} clip_cache;

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char c = *s, p = *prefix;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != p) return false;
    }
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* First line of text that names an existing file (a file:// URI or an
 * absolute path), percent-decoded into out. */
static bool text_file_path(const char *text, char *out, size_t cap)
{
    const char *p = text;
    while (p && *p) {
        const char *e = p;
        size_t n, k = 0;
        while (*e && *e != '\n' && *e != '\r') e++;
        n = (size_t)(e - p);
        while (n > 0u && (*p == ' ' || *p == '\t')) { p++; n--; }
        if (n > 0u && *p != '#') {
            const char *s = p;
            size_t m = n;
            bool uri = starts_with_ci(s, "file://");
            if (uri) {
                s += 7;
                m -= 7u;
                if (m >= 9u && starts_with_ci(s, "localhost")) { s += 9; m -= 9u; }
#if defined(_WIN32)
                if (m >= 3u && s[0] == '/' && s[2] == ':') { s++; m--; }
#endif
            }
            for (size_t i = 0; i < m && k + 1u < cap; i++) {
                int h1, h2;
                if (uri && s[i] == '%' && i + 2u < m && (h1 = hexval(s[i + 1])) >= 0 &&
                    (h2 = hexval(s[i + 2])) >= 0) {
                    out[k++] = (char)((h1 << 4) | h2);
                    i += 2u;
                } else {
                    out[k++] = s[i];
                }
            }
            out[k] = '\0';
            while (k > 0u && (out[k - 1u] == ' ' || out[k - 1u] == '\t')) out[--k] = '\0';
            if (k > 0u && pal_file_exists(out) && !pal_is_dir(out)) return true;
            return false;           /* only the first entry counts */
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Start of the base64 payload of "data:image/...;base64," (leading blanks
 * allowed), or NULL. */
static const char *data_uri_payload(const char *text)
{
    const char *p = text, *comma;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!starts_with_ci(p, "data:image/")) return NULL;
    comma = strchr(p, ',');
    if (!comma || comma - p > 128) return NULL;
    for (const char *q = p; q + 7 <= comma; q++)
        if (starts_with_ci(q, ";base64,")) return comma + 1;
    return NULL;
}

/* Decode base64 (whitespace ignored) into a malloc'ed buffer. */
static pc_status b64_decode(const char *s, size_t n, uint8_t **out, size_t *len)
{
    size_t cap = n / 4u * 3u + 3u, k = 0;
    uint32_t acc = 0;
    int bits = 0;
    uint8_t *buf;
    *out = NULL;
    *len = 0;
    if (n > M_MAX_TEXT) return PC_ERR_LIMIT;
    buf = (uint8_t *)malloc(cap ? cap : 1u);
    if (!buf) return PC_ERR_NOMEM;
    for (size_t i = 0; i < n; i++) {
        int v = b64val(s[i]);
        if (s[i] == '=') break;
        if (v < 0) {
            if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') continue;
            break;                  /* end of the payload */
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k < cap) buf[k++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    if (k == 0u) {
        free(buf);
        return PC_ERR_FORMAT;
    }
    *out = buf;
    *len = k;
    return PC_OK;
}

static clip_text_kind classify_text(const char *text, char *path, size_t cap)
{
    if (!text || !*text) return CT_NONE;
    if (data_uri_payload(text)) return CT_DATA_URI;
    if (text_file_path(text, path, cap)) return CT_FILE;
    return CT_NONE;
}

static bool video_ok(void) { return SDL_WasInit(SDL_INIT_VIDEO) != 0; }

/* Text of the uri-list flavour (file managers) or the plain text, malloc'ed. */
static char *clip_text_any(void)
{
    if (SDL_HasClipboardData("text/uri-list")) {
        size_t n = 0;
        void *p = SDL_GetClipboardData("text/uri-list", &n);
        if (p) {
            char *s = (char *)malloc(n + 1u);
            if (s) {
                memcpy(s, p, n);
                s[n] = '\0';
            }
            SDL_free(p);
            if (s) return s;
        }
    }
    /* pal checks for text itself: on Windows it reads the Win32 clipboard,
     * which SDL_HasClipboardText does not see under the dummy video driver */
    return pal_clip_get_text();
}

static clip_cache *cache_of(app *a)
{
    clip_cache *c = (clip_cache *)app_ext_get(a, M_CLIPCACHE_KEY);
    if (c) return c;
    c = (clip_cache *)calloc(1u, sizeof *c);
    if (!c || !app_ext_set(a, M_CLIPCACHE_KEY, c, free)) {
        free(c);
        return NULL;
    }
    return c;
}

void m_paste_invalidate(app *a)
{
    clip_cache *c = (clip_cache *)app_ext_get(a, M_CLIPCACHE_KEY);
    if (c) c->valid = c->any_valid = false;
}

/* The enabled state of the paste commands is asked every frame (toolbar
 * button); the clipboard is queried at most every 250 ms (text contents
 * every second), since some platforms answer through window system
 * round trips. */
bool m_paste_available(app *a)
{
    clip_cache *c;
    uint64_t now = app_now_ms(a);
    if (!video_ok()) return false;
    c = cache_of(a);
    if (!c) return pal_clip_has_image();
    if (c->any_valid && now >= c->any_at && now - c->any_at < 250u) return c->any;
    c->any_at = now;
    c->any_valid = true;
    c->any = true;
    if (pal_clip_has_image()) return true;
    if (!c->valid || now - c->at > 1000u || now < c->at) {
        char *text = clip_text_any();
        char path[1024];
        c->kind = classify_text(text, path, sizeof path);
        free(text);
        c->at = now;
        c->valid = true;
    }
    c->any = c->kind != CT_NONE;
    return c->any;
}

/* ---- the paste job -------------------------------------------------------------------------- */
typedef struct paste_job {
    int             kind;           /* 0 paste, 1 new layer, 2 new image */
    uint32_t        doc_id;
    bool            to_srgb;        /* convert an embedded profile to sRGB */
    uint8_t        *dst_icc;        /* lane KEYS: profile of the target image (owned copy) */
    size_t          dst_icc_len;
    uint8_t        *data;           /* encoded bytes (owned) or NULL */
    size_t          len;
    char           *path;           /* or a file to read (owned) */
    pc_doc         *img;            /* decoded image (owned) */
    pc_image_meta   meta;
    pc_status       st;
} paste_job;

static void job_free(paste_job *j)
{
    if (!j) return;
    free(j->data);
    free(j->dst_icc);
    free(j->path);
    pc_doc_destroy(j->img);
    pc_meta_free(&j->meta);
    free(j);
}

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

/* Convert every layer of the decoded image from the matrix/TRC profile src
 * to dst (m_icc.h, relative colorimetric). Row bands bound the scratch
 * memory (P-08). Worker thread, touches only img. */
static void convert_layers(pc_doc *img, const m_icc_rgb *src, const m_icc_rgb *dst)
{
    const int32_t rows = 64;
    size_t band_px = 0;
    m_icc_xform *x = (m_icc_xform *)malloc(sizeof *x);
    pc_px32 *buf;
    if (!x) return;
    if (!m_icc_xform_init(x, src, dst) || !pc_mul_size(img->w, (size_t)rows, &band_px)) {
        free(x);
        return;
    }
    buf = (pc_px32 *)malloc(band_px * sizeof *buf);
    if (!buf) {
        free(x);
        return;
    }
    for (uint32_t li = 0; li < img->n_layers; li++) {
        pc_layer *l = img->stack[li];
        for (int32_t y0 = 0; y0 < (int32_t)img->h; y0 += rows) {
            int32_t n = (int32_t)img->h - y0 < rows ? (int32_t)img->h - y0 : rows;
            pc_rect r = pc_rect_make(0, y0, (int32_t)img->w, n);
            pc_layer_read_rect(img, l, r, buf, (size_t)img->w);
            m_icc_xform_px(x, buf, (size_t)img->w * (size_t)n);
            if (pc_layer_store_rect(img, l, r, buf, (size_t)img->w) != PC_OK) break;
        }
    }
    free(buf);
    free(x);
}

/* lane KEYS (F-CLIP-PROFILE): the pasted pixels in the target image's
 * profile. The same profile (a copy within paint.c) needs nothing; a
 * matrix/TRC clipboard profile converts directly (no detour through the
 * sRGB gamut); any other clipboard profile goes to sRGB first (Little-CMS,
 * pc_icc_import); untagged pixels are sRGB. Targets that are sRGB-like or
 * not matrix/TRC profiles keep the sRGB pixels. Worker thread. */
static void to_target_profile(paste_job *j)
{
    pc_icc_info info;
    m_icc_rgb src, dst;
    bool dst_srgb = pc_icc_inspect(j->dst_icc, j->dst_icc_len, &info) != PC_OK || info.is_srgb;
    bool dst_ok = !dst_srgb && m_icc_parse(j->dst_icc, j->dst_icc_len, &dst) == PC_OK;
    if (j->meta.icc && j->meta.icc_len == j->dst_icc_len &&
        memcmp(j->meta.icc, j->dst_icc, j->dst_icc_len) == 0)
        return;
    if (j->meta.icc && dst_ok && m_icc_parse(j->meta.icc, j->meta.icc_len, &src) == PC_OK) {
        convert_layers(j->img, &src, &dst);
        return;
    }
    if (j->meta.icc) (void)pc_icc_import(j->img, &j->meta, NULL);
    if (!dst_ok) {
        if (!dst_srgb)
            pal_log(PAL_LOG_INFO, "paste: the image profile is not a matrix profile; "
                                  "pixels stay sRGB");
        return;
    }
    m_icc_builtin_rgb(M_ICC_SRGB, &src);
    convert_layers(j->img, &src, &dst);
}

/* Worker: read (file), decode and convert the color profile. Touches no
 * app state. */
static void decode_work(void *ud)
{
    paste_job *j = (paste_job *)ud;
    pc_codec_limits lim;
    if (j->img) {
        j->st = PC_OK;
    } else {
        if (j->path) {
            j->st = pal_read_file(j->path, M_MAX_FILE, &j->data, &j->len);
            if (j->st != PC_OK) return;
        }
        pc_codec_limits_default(&lim);
        j->st = pc_codec_load_any(j->data, j->len, j->path, &lim, &j->img, &j->meta, NULL);
        free(j->data);
        j->data = NULL;
        if (j->st == PC_OK && !j->img) j->st = PC_ERR_FORMAT;
        if (j->st != PC_OK) return;
    }
    /* CB-PROFILE: pixels for an sRGB image are converted to sRGB (failures
     * keep the unconverted pixels, as File > Open does) */
    if (j->dst_icc) to_target_profile(j);
    else if (j->to_srgb && j->meta.icc) (void)pc_icc_import(j->img, &j->meta, NULL);
}

/* The flattened clipboard image as one surface. */
static pc_status flatten(app *a, const pc_doc *img, pc_surf *out)
{
    pc_status st = pc_surf_alloc(out, (int32_t)img->w, (int32_t)img->h);
    if (st != PC_OK) return st;
    if (img->n_layers == 1u) {
        pc_layer_read_rect(img, img->stack[0], pc_rect_make(0, 0, out->w, out->h), out->px,
                           (size_t)out->stride);
        return PC_OK;
    }
    st = pc_comp_rect(img, pc_rect_make(0, 0, out->w, out->h), out->px, (size_t)out->stride,
                      &a->par);
    if (st != PC_OK) pc_surf_free(out);
    return st;
}

static void report(app *a, const char *what, pc_status st)
{
    if (st != PC_OK && st != PC_ERR_CANCELLED)
        app_error(a, "%s failed: %s.", what, pc_status_str(st));
}

/* Paste into New Image: a new image of exactly the clipboard size whose
 * only layer holds the pixels (history starts at "New Image", nothing
 * selected, as in Paint.NET 3.36). The clipboard image keeps its own color
 * profile. */
static void place_new_image(app *a, paste_job *j)
{
    pc_doc *img = j->img;
    app_doc *d;
    if (img->n_layers != 1u) {
        pc_surf s = {0};
        pc_doc *flat = pc_doc_create(img->w, img->h);
        pc_layer *l = flat ? pc_layer_create(flat, "Background") : NULL;
        pc_status st = l ? flatten(a, img, &s) : PC_ERR_NOMEM;
        if (st == PC_OK) {
            st = pc_layer_store_rect(flat, l, pc_rect_make(0, 0, s.w, s.h), s.px,
                                     (size_t)s.stride);
            pc_surf_free(&s);
        }
        if (st == PC_OK) st = pc_doc_reserve_layers(flat, 1u);
        if (st == PC_OK) st = pc_doc_insert_layer(flat, l, 0u);
        if (st != PC_OK) {
            pc_layer_destroy(l);
            pc_doc_destroy(flat);
            report(a, "Paste into New Image", st);
            return;
        }
        pc_doc_destroy(img);
        img = flat;
    } else {
        app_copy_str(img->stack[0]->name, sizeof img->stack[0]->name, "Background");
        img->stack[0]->visible = true;
        img->stack[0]->opacity = 255u;
        img->stack[0]->mode = PC_BLEND_NORMAL;
    }
    j->img = NULL;
    d = app_doc_create(a, img, NULL, NULL, &j->meta, "New Image");
    if (!d) {
        report(a, "Paste into New Image", PC_ERR_NOMEM);
        return;
    }
    app_doc_set_untitled(a, d);
    (void)app_add_doc(a, d);
}

/* Write the pixels into the layer and select them: the fallback when no
 * float hook took them. Returns the status of the pixel write. */
static pc_status write_and_select(app *a, app_doc *d, uint32_t layer_id, const pc_surf *s,
                                  int32_t x, int32_t y, const char *label)
{
    pc_rect r = pc_rect_make(x, y, s->w, s->h);
    pc_rect vis = pc_rect_intersect(r, pc_doc_rect(d->doc));
    pc_txn *t;
    pc_status st;
    (void)a;
    if (pc_rect_is_empty(vis)) return PC_OK;
    t = pc_txn_begin(d->doc, label);
    if (!t) return PC_ERR_NOMEM;
    st = pc_txn_write_rect(t, layer_id, r, s->px, (size_t)s->stride);
    if (st != PC_OK) {
        pc_txn_cancel(t);
        return st;
    }
    st = pc_txn_commit(t, d->hist);
    if (st != PC_OK) return st;
    return pc_sel_apply_rect(d->hist, vis, PC_SEL_REPLACE, label);
}

/* Place the decoded pixels into d (after the Expand Canvas question).
 * Lane KEYS (F-CLIP-PASTE-FLOAT, F-MENU-EDIT-PASTE, F-CLIP-PASTE-LARGER):
 * without an installed hook the pixels become a floating selection of
 * Move Selected Pixels (app_float_paste, include/app/app_float.h), so
 * moving them restores what they covered and the part outside the canvas
 * (Keep canvas size) stays movable until Finish. The old commit-and-select
 * path remains the fallback when the tool is not available. */
static void place(app *a, paste_job *j, bool expand)
{
    app_doc *d = doc_by_id(a, j->doc_id);
    const char *label = j->kind == 1 ? "Paste into New Layer" : "Paste";
    const m_float_hook *hook = m_paste_float_hook(a);
    pc_hist_node *base;
    uint32_t layer_id;
    pc_surf s;
    int32_t x = 0, y = 0;
    pc_status st;
    if (!d) return;
    if (d->txn) (void)app_tool_finish(a);
    if (d->txn) {
        app_error(a, "%s failed: the image is busy.", label);
        return;
    }
    st = flatten(a, j->img, &s);
    if (st != PC_OK) {
        report(a, label, st);
        return;
    }
    if (expand) {
        uint32_t w = d->doc->w > j->img->w ? d->doc->w : j->img->w;
        uint32_t h = d->doc->h > j->img->h ? d->doc->h : j->img->h;
        pc_px32 none;
        memset(&none, 0, sizeof none);
        st = pc_geom_canvas_size(d->hist, w, h, PC_ANCHOR_TOP_LEFT, none, &a->par,
                                 "Canvas Size");
        if (st != PC_OK) {
            pc_surf_free(&s);
            report(a, "Expand Canvas", st);
            return;
        }
        app_doc_history_changed(a, d);
        d->view.need_fit = true;
    } else {
        m_paste_position(a, d, s.w, s.h, &x, &y);
    }
    if (!hook) {
        /* the floating paste records its own History item (and the new
         * layer for Paste into New Layer) and activates Move Selected
         * Pixels; app_float_paste copies the pixels */
        st = app_float_paste(a, d, &s, x, y, j->kind == 1);
        if (st != PC_ERR_STATE) {
            pc_surf_free(&s);
            report(a, label, st);
            return;
        }
        if (d->txn) {
            pc_surf_free(&s);
            app_error(a, "%s failed: the image is busy.", label);
            return;
        }
    }
    base = m_hist_mark(d->hist);
    layer_id = d->layer_id;
    if (j->kind == 1) {
        uint32_t nid = 0;
        st = pc_layerop_add_new(d->hist, d->layer_id, &nid, "Add New Layer");
        if (st != PC_OK) {
            pc_surf_free(&s);
            report(a, label, st);
            return;
        }
        layer_id = nid;
        app_doc_set_layer(d, nid);
    }
    if (hook && hook->paste(a, d, layer_id, &s, x, y, label, hook->ud)) {
        app_doc_history_changed(a, d);
        return;
    }
    st = write_and_select(a, d, layer_id, &s, x, y, label);
    pc_surf_free(&s);
    (void)m_hist_fuse(d->hist, base, label);
    app_doc_history_changed(a, d);
    app_doc_set_layer(d, layer_id);
    report(a, label, st);
    if (st == PC_OK && app_tool_find(a, "move_pixels")) (void)app_tool_select(a, "move_pixels");
}

/* ---- Expand Canvas prompt (CB-PASTE-LARGER) ----------------------------------------------- */
static uint32_t g_expand_seq;     /* main thread only */

typedef struct expand_dlg {
    char         title[48];      /* unique per prompt: fresh focus and placement */
    paste_job   *job;            /* owned */
    pc_surf      thumb;          /* small preview */
    SDL_Texture *tex;            /* owned, created on first use */
} expand_dlg;

static void expand_free(void *p)
{
    expand_dlg *e = (expand_dlg *)p;
    if (!e) return;
    if (e->tex) SDL_DestroyTexture(e->tex);
    pc_surf_free(&e->thumb);
    job_free(e->job);
    free(e);
}

/* Box-free nearest sampling into a thumbnail of at most side x side. */
static void make_thumb(app *a, const pc_doc *img, int32_t side, pc_surf *out)
{
    double k = (double)side / (double)(img->w > img->h ? img->w : img->h);
    int32_t tw, th;
    pc_surf full;
    memset(out, 0, sizeof *out);
    if (k > 1.0) k = 1.0;
    tw = (int32_t)((double)img->w * k + 0.5);
    th = (int32_t)((double)img->h * k + 0.5);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (pc_surf_alloc(out, tw, th) != PC_OK) return;
    if (flatten(a, img, &full) != PC_OK) {
        pc_surf_free(out);
        return;
    }
    for (int32_t y = 0; y < th; y++) {
        int32_t sy = (int32_t)(((double)y + 0.5) / k);
        if (sy >= full.h) sy = full.h - 1;
        for (int32_t x = 0; x < tw; x++) {
            int32_t sx = (int32_t)(((double)x + 0.5) / k);
            if (sx >= full.w) sx = full.w - 1;
            pc_px32 c = pc_surf_row(&full, sy)[sx];
            /* premultiplied for linear filtering without halos (X-14) */
            c.r = (uint8_t)pc_mul255(c.r, c.a);
            c.g = (uint8_t)pc_mul255(c.g, c.a);
            c.b = (uint8_t)pc_mul255(c.b, c.a);
            pc_surf_row(out, y)[x] = c;
        }
    }
    pc_surf_free(&full);
}

static bool expand_frame(app *a, void *st)
{
    expand_dlg *e = (expand_dlg *)st;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    static const char *const labels[3] = { "Expand canvas##pexp0", "Keep canvas size##pexp1",
                                           "Cancel##pexp2" };
    ui_size cells[4];
    int pick = -1;
    uint32_t r;
    bool enter;
    char text[200];
    ui_dialog_begin(ui, e->title, 460.0f, 0.0f);
    cells[0] = ui_size_px(110.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    {
        ui_rect box = ui_layout_next(ui, ui_px(ui, 100.0f), ui_px(ui, 100.0f));
        if (!e->tex && e->thumb.px && a->ren) {
            e->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC,
                                       e->thumb.w, e->thumb.h);
            if (e->tex) {
                SDL_UpdateTexture(e->tex, NULL, e->thumb.px, e->thumb.stride * 4);
                SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            }
        }
        if (e->tex) {
            int32_t side = ui_px(ui, 96.0f);
            double k = (double)side / (double)(e->thumb.w > e->thumb.h ? e->thumb.w : e->thumb.h);
            int32_t tw = (int32_t)((double)e->thumb.w * k), th = (int32_t)((double)e->thumb.h * k);
            ui_rect ir = ui_rect_make(box.x + (box.w - tw) / 2, box.y + (box.h - th) / 2,
                                      tw < 1 ? 1 : tw, th < 1 ? 1 : th);
            ui_draw_checker(ui, ir, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
            ui_draw_image(ui, e->tex, NULL, ir, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
            ui_draw_rect_outline(ui, ir, 1, p->border);
        } else {
            ui_draw_icon(ui, UI_ICON_PASTE, box, ui_px(ui, 40.0f), p->icon, p->icon_accent);
        }
    }
    ui_layout_begin(ui, 0.0f);
    snprintf(text, sizeof text,
             "The image being pasted (%u x %u) is larger than the canvas (%u x %u).",
             (unsigned)e->job->img->w, (unsigned)e->job->img->h,
             (unsigned)(doc_by_id(a, e->job->doc_id) ? doc_by_id(a, e->job->doc_id)->doc->w : 0u),
             (unsigned)(doc_by_id(a, e->job->doc_id) ? doc_by_id(a, e->job->doc_id)->doc->h : 0u));
    ui_text_wrapped(ui, text, 0);
    ui_layout_space(ui, 4.0f);
    ui_text_wrapped(ui, "Expand the canvas so the whole image fits, or keep the canvas size "
                        "and paste anyway (the parts outside the canvas can be moved in).",
                    UI_LABEL_DIM);
    ui_layout_end(ui);
    ui_layout_column(ui);
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    cells[2] = ui_size_auto();
    cells[3] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 4, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, 28.0f));
    for (int i = 0; i < 3; i++)
        if (ui_button_ex(ui, labels[i], UI_ICON_NONE, i == 0 ? UI_BUTTON_PRIMARY : 0u)) pick = i;
    ui_layout_column(ui);
    /* Enter presses Expand canvas unless a focused button took it */
    enter = pick < 0 && app_dialog_take_enter(a);
    r = ui_dialog_end(ui);
    if (pick < 0 && enter) pick = 0;
    if (pick < 0 && r) pick = 2;               /* Escape or the close button */
    if (pick < 0) return true;
    if (pick == 0 || pick == 1) {
        paste_job *j = e->job;
        e->job = NULL;
        place(a, j, pick == 0);
        job_free(j);
    }
    return false;
}

/* ---- main-thread continuation ------------------------------------------------------------- */
static void decode_done(app *a, void *ud)
{
    paste_job *j = (paste_job *)ud;
    app_doc *d;
    if (j->st != PC_OK || !j->img) {
        if (j->st == PC_ERR_FORMAT || j->st == PC_ERR_UNSUPPORTED)
            app_error(a, "The clipboard data is not an image paint.c can read. Copy the image "
                         "again and retry.");
        else
            report(a, "Paste", j->st);
        job_free(j);
        return;
    }
    if (j->kind == 2) {
        place_new_image(a, j);
        job_free(j);
        return;
    }
    d = doc_by_id(a, j->doc_id);
    if (!d) {
        job_free(j);
        return;
    }
    if (j->img->w > d->doc->w || j->img->h > d->doc->h) {
        expand_dlg *e = (expand_dlg *)calloc(1u, sizeof *e);
        if (!e) {
            job_free(j);
            report(a, "Paste", PC_ERR_NOMEM);
            return;
        }
        e->job = j;
        snprintf(e->title, sizeof e->title, "Paste##pasteexpand%u", (unsigned)++g_expand_seq);
        make_thumb(a, j->img, 96, &e->thumb);
        (void)app_dialog_push(a, expand_frame, e, expand_free);
        return;
    }
    place(a, j, false);
    job_free(j);
}

static void start_job(app *a, paste_job *j)
{
    app_doc *d = doc_by_id(a, j->doc_id);
    /* lane KEYS: images with a profile convert through sRGB into it */
    j->to_srgb = j->kind != 2;
    if (j->kind != 2 && d && d->meta.icc && d->meta.icc_len > 0u &&
        d->meta.icc_len <= PC_ICC_MAX_BYTES) {
        j->dst_icc = (uint8_t *)malloc(d->meta.icc_len);
        if (j->dst_icc) {
            memcpy(j->dst_icc, d->meta.icc, d->meta.icc_len);
            j->dst_icc_len = d->meta.icc_len;
        }
    }
    if (!app_task(a, decode_work, decode_done, j)) {
        report(a, "Paste", PC_ERR_NOMEM);
        job_free(j);
    }
}

void m_paste_image(app *a, int kind, pc_doc *img)
{
    app_doc *d = app_active_doc(a);
    paste_job *j;
    if (!img) return;
    j = (paste_job *)calloc(1u, sizeof *j);
    if (!j || (kind != 2 && !d)) {
        free(j);
        pc_doc_destroy(img);
        return;
    }
    j->kind = kind;
    j->doc_id = d ? d->id : 0u;
    j->img = img;
    start_job(a, j);
}

void m_paste_start(app *a, int kind)
{
    app_doc *d = app_active_doc(a);
    paste_job *j;
    char mime[64];
    if (kind != 2 && !d) return;
    j = (paste_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->kind = kind;
    j->doc_id = d ? d->id : 0u;
    if (!video_ok() || !pal_clip_get_image(&j->data, &j->len, mime, sizeof mime)) {
        /* CB-PASTE-FILES / CB-PASTE-BASE64: an image file or a data URI */
        char *text = video_ok() ? clip_text_any() : NULL;
        char path[1024];
        clip_text_kind k = classify_text(text, path, sizeof path);
        pc_status st = PC_OK;
        if (k == CT_FILE) {
            j->path = app_strdup(path);
            if (!j->path) st = PC_ERR_NOMEM;
        } else if (k == CT_DATA_URI) {
            const char *pl = data_uri_payload(text);
            st = b64_decode(pl, strlen(pl), &j->data, &j->len);
        }
        free(text);
        if (k == CT_NONE) {
            app_error(a, "The clipboard does not contain an image.");
            job_free(j);
            return;
        }
        if (st != PC_OK) {
            report(a, "Paste", st);
            job_free(j);
            return;
        }
    }
    start_job(a, j);
}
