/* text_font.c - font catalog and pc_font_face backend of the Text tool
 * (lane C, see text_font.h).
 *
 * The backend reads glyph data from the toolkit's validated faces through
 * src/ui/ui_font_internal.h (cmap, advances, kerning, metrics and the
 * stb_truetype outline of glyphs that passed validation); ui.h has no
 * glyph-level API yet (requested in the lane C report). */
#include "text_font.h"
#include "text_sfnt.h"
#include "../app_internal.h"

#include "pc/pc_codec.h"
#include "../../ui/ui_font_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TF_MAX_FACES_PER_FILE 64
#define TF_CACHE_MAX_BYTES ((uint64_t)16u << 20)
#define TF_MAX_SETS 256u
#define TF_MAX_FALLBACKS 15u

/* ---- loaded faces -------------------------------------------------------------------- */
/* Decoded color bitmaps of a face (lane TOOLB). */
#define TF_BMP_CACHE_ENTRIES 512u
#define TF_BMP_CACHE_BYTES ((size_t)48u << 20)
#define TF_BMP_MAX_SIDE 2048u

typedef struct tf_bmp {
    const uint8_t *key;         /* the encoded image inside the font bytes */
    pc_px32       *px;          /* owned, NULL when the image could not be decoded */
    int32_t        w, h;
} tf_bmp;

typedef struct tf_face {
    pc_font_face pf;            /* pf.ud points at this record */
    ui_font     *f;             /* owned; NULL until loaded */
    char         path[1024];    /* empty for built-in faces */
    int32_t      index;
    bool         builtin;
    bool         tried;         /* a load was attempted */
    /* color tables (lane TOOLB): over f's bytes, or over own bytes for
     * fonts without outlines (sfnt_only) */
    text_sfnt    cs;
    bool         has_cs;
    bool         sfnt_only;
    uint8_t     *bytes;         /* owned file bytes of an sfnt_only face */
    tf_bmp      *bmp;           /* decoded bitmaps (owned) */
    size_t       nbmp, capbmp, bmp_bytes;
} tf_face;

typedef struct tf_set {
    char                family[96];
    bool                bold, italic;
    const pc_font_face *faces[TF_MAX_FALLBACKS + 1u];
    size_t              n;
} tf_set;

#define TF_PREVIEWS 96
#define TF_PREVIEW_MAX_FILE ((uint64_t)8u << 20)
#define TF_PREVIEW_LOADS_PER_FRAME 2

typedef struct tf_preview {
    char     family[96];
    ui_font *f;                 /* owned, NULL = cannot preview */
    uint32_t last;              /* frame of the last use */
} tf_preview;

struct text_fonts {
    tf_preview      prev[TF_PREVIEWS];
    int32_t         nprev;
    uint32_t        prev_frame, prev_loads;
    app            *a;
    text_face_info *sys;        /* scanned faces (owned) */
    size_t          nsys;
    char          **fam;        /* sorted unique family names (owned) */
    int32_t         nfam;
    uint32_t        gen;
    tf_face       **faces;      /* every face record (owned) */
    size_t          nfaces, capfaces;
    tf_set         *sets;       /* resolved family + style sets (owned) */
    size_t          nsets;
    bool            scanning;
    struct scan_job *job;       /* the running scan (owned by its task) */
    char            cache_path[1024];
};

/* Fonts the toolkit rejects for lack of outlines may still be bitmap color
 * fonts: read them with text_sfnt alone (lane TOOLB). */
static bool load_sfnt_only(tf_face *t)
{
    size_t len = 0;
    if (t->builtin || pal_read_file(t->path, UI_FONT_MAX_FILE, &t->bytes, &len) != PC_OK)
        return false;
    if (text_sfnt_open(&t->cs, t->bytes, len, t->index) == PC_OK &&
        text_sfnt_has_bitmaps(&t->cs) && !text_sfnt_has_outlines(&t->cs)) {
        t->has_cs = true;
        t->sfnt_only = true;
        return true;
    }
    text_sfnt_close(&t->cs);
    free(t->bytes);
    t->bytes = NULL;
    return false;
}

static bool ensure(tf_face *t)
{
    if (t->f || t->sfnt_only) return true;
    if (t->tried) return false;
    t->tried = true;
    if (t->builtin) {
        t->f = ui_font_load_builtin((ui_font_builtin)t->index);
    } else if (ui_font_load_file(t->path, t->index, &t->f) != PC_OK) {
        t->f = NULL;
        if (load_sfnt_only(t)) {
            t->pf.color = true;
            return true;
        }
    }
    if (t->f && !t->builtin && text_sfnt_open(&t->cs, t->f->s.d, t->f->s.n, t->index) == PC_OK) {
        t->has_cs = text_sfnt_has_color(&t->cs);
        if (!t->has_cs) text_sfnt_close(&t->cs);
        else t->pf.color = true;
    }
    if (!t->f) pal_log(PAL_LOG_WARN, "text: cannot load font %s (%d)", t->path, (int)t->index);
    return t->f != NULL;
}

static void face_free(tf_face *t)
{
    for (size_t i = 0; i < t->nbmp; i++) free(t->bmp[i].px);
    free(t->bmp);
    text_sfnt_close(&t->cs);
    ui_font_free(t->f);
    free(t->bytes);
    free(t);
}

static double scale_of(const ui_font *f, double em)
{
    return f->upem > 0.0f ? em / (double)f->upem : em / 1000.0;
}

static double face_scale(const tf_face *t, double em)
{
    if (t->sfnt_only) return t->cs.upem ? em / (double)t->cs.upem : em / 1000.0;
    return scale_of(t->f, em);
}

static uint32_t cb_glyph(void *ud, uint32_t cp)
{
    tf_face *t = (tf_face *)ud;
    if (!ensure(t)) return 0u;
    return t->sfnt_only ? text_sfnt_cmap(&t->cs, cp) : ui_font_cmap(t->f, cp);
}

static bool cb_has(void *ud, uint32_t cp)
{
    tf_face *t = (tf_face *)ud;
    if (!ensure(t)) return false;
    return t->sfnt_only ? text_sfnt_cmap(&t->cs, cp) != 0u : ui_font_has_glyph(t->f, cp);
}

static void cb_metrics(void *ud, double em, pc_font_metrics *o)
{
    tf_face *t = (tf_face *)ud;
    double s;
    memset(o, 0, sizeof *o);
    if (!ensure(t)) return;
    s = face_scale(t, em);
    if (t->sfnt_only) {
        o->ascent = (double)t->cs.ascent * s;
        o->descent = (double)t->cs.descent * s;
        o->line_gap = (double)t->cs.line_gap * s;
        o->x_height = (double)t->cs.x_height * s;
        o->cap_height = (double)t->cs.cap_height * s;
        return;
    }
    o->ascent = (double)t->f->ascent * s;
    o->descent = (double)t->f->descent * s;
    o->line_gap = (double)t->f->line_gap * s;
    if (o->descent < 0.0) o->descent = 0.0;
    if (o->line_gap < 0.0) o->line_gap = 0.0;
    if (t->f->ul_size > 0 && t->f->ul_pos > 0) {
        o->underline_offset = (double)t->f->ul_pos * s;    /* stored positive = down */
        o->underline_thickness = (double)t->f->ul_size * s;
    }
    if (t->f->st_size > 0 && t->f->st_pos > 0) {
        o->strike_offset = -(double)t->f->st_pos * s;
        o->strike_thickness = (double)t->f->st_size * s;
    }
    /* alignment zones for the Sharp modes (lane TOOLB) */
    if (t->f->x_height > 0) o->x_height = (double)t->f->x_height * s;
    if (t->f->cap_height > 0) o->cap_height = (double)t->f->cap_height * s;
}

static double cb_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    tf_face *t = (tf_face *)ud;
    (void)mode;
    if (!ensure(t)) return em * 0.5;
    if (t->sfnt_only) return (double)text_sfnt_advance(&t->cs, gid) * face_scale(t, em);
    return (double)ui_font_advance(t->f, gid) * scale_of(t->f, em);
}

static double cb_kerning(void *ud, uint32_t l, uint32_t r, double em)
{
    tf_face *t = (tf_face *)ud;
    if (!ensure(t) || t->sfnt_only || !l || !r) return 0.0;
    return (double)ui_kern_cached(t->f, l, r) * scale_of(t->f, em);
}

/* The outline in pixels, y down, unhinted (the engine grid-fits the Sharp
 * modes itself, pc_text_hint_outline). */
static pc_status cb_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    tf_face *t = (tf_face *)ud;
    stbtt_vertex *v = NULL;
    double s;
    int n;
    bool open = false;
    pc_status st = PC_OK;
    (void)mode;
    if (!ensure(t) || t->sfnt_only || !ui_font_glyph_ok(t->f, gid)) return PC_OK;
    s = scale_of(t->f, em);
    n = stbtt_GetGlyphShape(&t->f->info, (int)gid, &v);
    for (int i = 0; i < n && st == PC_OK; i++) {
        double x = (double)v[i].x * s, y = -(double)v[i].y * s;
        switch (v[i].type) {
        case STBTT_vmove:
            if (open) st = pc_path_close(out);
            if (st == PC_OK) st = pc_path_move_to(out, x, y);
            open = true;
            break;
        case STBTT_vline:
            st = pc_path_line_to(out, x, y);
            break;
        case STBTT_vcurve:
            st = pc_path_quad_to(out, (double)v[i].cx * s, -(double)v[i].cy * s, x, y);
            break;
        case STBTT_vcubic:
            st = pc_path_cubic_to(out, (double)v[i].cx * s, -(double)v[i].cy * s,
                                  (double)v[i].cx1 * s, -(double)v[i].cy1 * s, x, y);
            break;
        default:
            break;
        }
    }
    if (open && st == PC_OK) st = pc_path_close(out);
    stbtt_FreeShape(&t->f->info, v);
    return st;
}

/* ---- color glyphs (lane TOOLB) ------------------------------------------------------------ */
static size_t cb_color_layers(void *ud, uint32_t gid, pc_font_color_layer *out, size_t cap)
{
    tf_face *t = (tf_face *)ud;
    if (!ensure(t) || !t->has_cs) return 0u;
    return text_sfnt_colr_layers(&t->cs, gid, out, cap);
}

/* Decode an embedded image (PNG, or JPEG for sbix) to straight BGRA.
 * Bounded by TF_BMP_MAX_SIDE before anything is allocated (P-08). */
static pc_px32 *decode_image(const text_sfnt_image *im, int32_t *w, int32_t *h)
{
    const pc_codec *c = NULL;
    pc_codec_limits lim;
    pc_image_meta meta;
    pc_doc *doc = NULL;
    pc_px32 *px = NULL;
    size_t n;
    if (memcmp(im->type, "png ", 4) == 0) c = pc_codec_by_id("png");
    else if (memcmp(im->type, "jpg ", 4) == 0) c = pc_codec_by_id("jpeg");
    if (!c || !c->load || !c->sniff || !c->sniff(im->data, im->len)) return NULL;
    pc_codec_limits_default(&lim);
    lim.max_w = TF_BMP_MAX_SIDE;
    lim.max_h = TF_BMP_MAX_SIDE;
    lim.max_pixels = (uint64_t)TF_BMP_MAX_SIDE * TF_BMP_MAX_SIDE;
    lim.max_mem = (uint64_t)64u << 20;
    lim.max_layers = 1u;
    memset(&meta, 0, sizeof meta);
    if (c->load(im->data, im->len, &lim, &doc, &meta) != PC_OK || !doc) {
        pc_meta_free(&meta);
        return NULL;
    }
    pc_meta_free(&meta);
    if (doc->n_layers >= 1u && doc->w >= 1u && doc->h >= 1u && doc->w <= TF_BMP_MAX_SIDE &&
        doc->h <= TF_BMP_MAX_SIDE && pc_mul_size((size_t)doc->w, (size_t)doc->h, &n)) {
        px = (pc_px32 *)malloc(n * sizeof *px);
        if (px) {
            pc_layer_read_rect(doc, doc->stack[0], pc_doc_rect(doc), px, (size_t)doc->w);
            *w = (int32_t)doc->w;
            *h = (int32_t)doc->h;
        }
    }
    pc_doc_destroy(doc);
    return px;
}

static const tf_bmp *bitmap_of(tf_face *t, const text_sfnt_image *im)
{
    tf_bmp *b;
    size_t bytes = 0;
    for (size_t i = 0; i < t->nbmp; i++)
        if (t->bmp[i].key == im->data) return &t->bmp[i];
    if (t->nbmp >= TF_BMP_CACHE_ENTRIES || t->bmp_bytes > TF_BMP_CACHE_BYTES) {
        /* full: start over (callers copy what they need right away) */
        for (size_t i = 0; i < t->nbmp; i++) free(t->bmp[i].px);
        t->nbmp = 0;
        t->bmp_bytes = 0;
    }
    if (t->nbmp == t->capbmp) {
        size_t nc = t->capbmp ? t->capbmp * 2u : 16u;
        tf_bmp *nb = (tf_bmp *)realloc(t->bmp, nc * sizeof *nb);
        if (!nb) return NULL;
        t->bmp = nb;
        t->capbmp = nc;
    }
    b = &t->bmp[t->nbmp];
    memset(b, 0, sizeof *b);
    b->key = im->data;
    b->px = decode_image(im, &b->w, &b->h);
    if (b->px) bytes = (size_t)b->w * (size_t)b->h * sizeof *b->px;
    t->bmp_bytes += bytes;
    t->nbmp++;
    return b;
}

static pc_status cb_color_bitmap(void *ud, uint32_t gid, double em, pc_font_bitmap *out)
{
    tf_face *t = (tf_face *)ud;
    text_sfnt_image im;
    const tf_bmp *b;
    double k;
    if (!ensure(t) || !t->has_cs || !text_sfnt_has_bitmaps(&t->cs)) return PC_ERR_UNSUPPORTED;
    if (!text_sfnt_image_of(&t->cs, gid, em, &im) || !(im.ppem > 0.0)) return PC_ERR_UNSUPPORTED;
    b = bitmap_of(t, &im);
    if (!b) return PC_ERR_NOMEM;
    if (!b->px) return PC_ERR_UNSUPPORTED;            /* undecodable: the outline */
    k = em / im.ppem;
    out->px = b->px;
    out->w = b->w;
    out->h = b->h;
    out->scale = k;
    out->left = im.x * k;
    /* CBDT: bearing to the top edge; sbix: origin offset of the bottom edge */
    out->top = im.bottom_origin ? -(im.y * k) - (double)b->h * k : -(im.y * k);
    return PC_OK;
}

static size_t cb_substitute(void *ud, uint32_t *gids, size_t n)
{
    tf_face *t = (tf_face *)ud;
    /* only color faces ligate clusters: emoji sequences (text faces keep
     * the one-glyph-per-character model of ADR-004) */
    if (!ensure(t) || !t->has_cs || !t->pf.color) return n;
    return text_sfnt_ligate(&t->cs, gids, n);
}

static tf_face *face_record(text_fonts *tf, const char *path, int32_t index, bool builtin,
                            uint16_t weight, bool italic, bool color)
{
    tf_face *t;
    for (size_t i = 0; i < tf->nfaces; i++) {
        t = tf->faces[i];
        if (t->builtin == builtin && t->index == index && strcmp(t->path, path) == 0) return t;
    }
    if (tf->nfaces == tf->capfaces) {
        size_t nc = tf->capfaces ? tf->capfaces * 2u : 16u;
        tf_face **nf = (tf_face **)realloc(tf->faces, nc * sizeof *nf);
        if (!nf) return NULL;
        tf->faces = nf;
        tf->capfaces = nc;
    }
    t = (tf_face *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    app_copy_str(t->path, sizeof t->path, path);
    t->index = index;
    t->builtin = builtin;
    t->pf.ud = t;
    t->pf.glyph = cb_glyph;
    t->pf.has_glyph = cb_has;
    t->pf.metrics = cb_metrics;
    t->pf.advance = cb_advance;
    t->pf.kerning = cb_kerning;
    t->pf.outline = cb_outline;
    t->pf.bold = weight >= 600u;
    t->pf.italic = italic;
    t->pf.color = color;
    t->pf.color_layers = cb_color_layers;
    t->pf.color_bitmap = cb_color_bitmap;
    t->pf.substitute = cb_substitute;
    tf->faces[tf->nfaces++] = t;
    return t;
}

/* ---- families -------------------------------------------------------------------------- */
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

static int fam_qsort(const void *x, const void *y)
{
    int c = ci_cmp(*(const char *const *)x, *(const char *const *)y);
    return c ? c : strcmp(*(const char *const *)x, *(const char *const *)y);
}

static void free_fams(text_fonts *tf)
{
    for (int32_t i = 0; i < tf->nfam; i++) free(tf->fam[i]);
    free(tf->fam);
    tf->fam = NULL;
    tf->nfam = 0;
}

static pc_status rebuild_families(text_fonts *tf)
{
    size_t cap = tf->nsys + 1u, n = 0;
    char **f = (char **)calloc(cap, sizeof *f);
    if (!f) return PC_ERR_NOMEM;
    f[n++] = app_strdup(TEXT_DEFAULT_FAMILY);
    for (size_t i = 0; i < tf->nsys; i++)
        if (tf->sys[i].family[0]) f[n++] = app_strdup(tf->sys[i].family);
    for (size_t i = 0; i < n; i++)
        if (!f[i]) {
            for (size_t k = 0; k < n; k++) free(f[k]);
            free(f);
            return PC_ERR_NOMEM;
        }
    qsort(f, n, sizeof *f, fam_qsort);
    {
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            if (w && ci_cmp(f[w - 1u], f[i]) == 0) {
                free(f[i]);
                continue;
            }
            f[w++] = f[i];
        }
        n = w;
    }
    free_fams(tf);
    tf->fam = f;
    tf->nfam = (int32_t)n;
    tf->gen++;
    return PC_OK;
}

pc_status text_fonts_set_faces(text_fonts *tf, const text_face_info *f, size_t n)
{
    text_face_info *c = NULL;
    if (!tf) return PC_ERR_ARG;
    if (n) {
        size_t bytes;
        if (!pc_mul_size(n, sizeof *c, &bytes)) return PC_ERR_LIMIT;
        c = (text_face_info *)malloc(bytes);
        if (!c) return PC_ERR_NOMEM;
        memcpy(c, f, bytes);
    }
    free(tf->sys);
    tf->sys = c;
    tf->nsys = n;
    tf->nsets = 0;              /* resolve families again (faces stay loaded) */
    return rebuild_families(tf);
}

bool text_fonts_scanning(const text_fonts *tf) { return tf && tf->scanning; }
int32_t text_fonts_family_count(const text_fonts *tf) { return tf ? tf->nfam : 0; }
uint32_t text_fonts_gen(const text_fonts *tf) { return tf ? tf->gen : 0u; }

const char *text_fonts_family(const text_fonts *tf, int32_t i)
{
    return tf && i >= 0 && i < tf->nfam ? tf->fam[i] : "";
}

int32_t text_fonts_find_family(const text_fonts *tf, const char *name)
{
    if (!tf || !name) return -1;
    for (int32_t i = 0; i < tf->nfam; i++)
        if (ci_cmp(tf->fam[i], name) == 0) return i;
    return -1;
}

/* ---- face sets ----------------------------------------------------------------------- */
static const char *const k_fallback_families[] = {
    "DejaVu Sans", "Noto Sans", "Segoe UI", "Arial", "Helvetica Neue", "Liberation Sans",
    /* color emoji (lane TOOLB): preferred for emoji presentation */
    "Segoe UI Emoji", "Apple Color Emoji", "Noto Color Emoji", "Twemoji", "Twemoji Mozilla",
    "JoyPixels", "EmojiOne Color",
    "Noto Sans CJK SC", "Noto Sans CJK JP", "Microsoft YaHei", "PingFang SC",
    "Noto Sans Symbols", "Noto Sans Symbols 2", "Segoe UI Symbol", "Apple Symbols",
};

/* Best face of family for the style, or NULL. */
static const text_face_info *pick(const text_fonts *tf, const char *family, bool bold, bool italic)
{
    const text_face_info *best = NULL;
    int best_score = 0;
    int target = bold ? 700 : 400;
    for (size_t i = 0; i < tf->nsys; i++) {
        const text_face_info *f = &tf->sys[i];
        int score;
        if (ci_cmp(f->family, family) != 0) continue;
        score = abs((int)f->weight - target) + (f->italic != italic ? 1000 : 0);
        if (!best || score < best_score) {
            best = f;
            best_score = score;
        }
    }
    return best;
}

static tf_face *builtin_face(text_fonts *tf, bool bold)
{
    return face_record(tf, "", bold ? (int32_t)UI_FONT_SEMIBOLD : (int32_t)UI_FONT_REGULAR, true,
                       bold ? 600u : 400u, false, false);
}

pc_status text_fonts_faces(text_fonts *tf, const char *family, bool bold, bool italic,
                           const pc_font_face *const **faces, size_t *n)
{
    tf_set *s;
    tf_face *prim = NULL;
    const text_face_info *fi;
    if (!tf || !faces || !n) return PC_ERR_ARG;
    if (!family || !*family) family = TEXT_DEFAULT_FAMILY;
    for (size_t i = 0; i < tf->nsets; i++) {
        s = &tf->sets[i];
        if (s->bold == bold && s->italic == italic && strcmp(s->family, family) == 0) {
            *faces = s->faces;
            *n = s->n;
            return PC_OK;
        }
    }
    if (tf->nsets >= TF_MAX_SETS) tf->nsets = 0;
    if (!tf->sets) {
        tf->sets = (tf_set *)calloc(TF_MAX_SETS, sizeof *tf->sets);
        if (!tf->sets) return PC_ERR_NOMEM;
    }
    fi = ci_cmp(family, TEXT_DEFAULT_FAMILY) == 0 ? NULL : pick(tf, family, bold, italic);
    if (fi) {
        prim = face_record(tf, fi->path, fi->index, false, fi->weight, fi->italic, fi->color);
        if (prim && !ensure(prim)) prim = NULL;
    }
    if (!prim) {
        prim = builtin_face(tf, bold);
        if (!prim || !ensure(prim)) return PC_ERR_NOMEM;
    }
    s = &tf->sets[tf->nsets];
    memset(s, 0, sizeof *s);
    app_copy_str(s->family, sizeof s->family, family);
    s->bold = bold;
    s->italic = italic;
    s->faces[s->n++] = &prim->pf;
    /* fallbacks: the built-in face, then broad-coverage families */
    if (!prim->builtin) {
        tf_face *b = builtin_face(tf, false);
        if (b) s->faces[s->n++] = &b->pf;
    }
    for (size_t k = 0; k < sizeof k_fallback_families / sizeof k_fallback_families[0] &&
                       s->n < TF_MAX_FALLBACKS + 1u; k++) {
        const text_face_info *ff = pick(tf, k_fallback_families[k], false, false);
        tf_face *t;
        if (!ff || ci_cmp(k_fallback_families[k], family) == 0) continue;
        t = face_record(tf, ff->path, ff->index, false, ff->weight, ff->italic, ff->color);
        if (t) s->faces[s->n++] = &t->pf;
    }
    tf->nsets++;
    *faces = s->faces;
    *n = s->n;
    return PC_OK;
}

/* ---- previews ---------------------------------------------------------------------------- */
static bool shows_own_name(const ui_font *f, const char *name)
{
    size_t n = strlen(name), i = 0;
    while (i < n) {
        uint32_t cp = ui_utf8_decode(name, n, &i);
        if (cp > 0x20u && !ui_font_has_glyph(f, cp)) return false;   /* symbol fonts */
    }
    return true;
}

ui_font *text_fonts_preview(text_fonts *tf, int32_t i, uint32_t frame)
{
    const char *fam;
    const text_face_info *fi;
    tf_preview *slot = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!tf || i < 0 || i >= tf->nfam) return NULL;
    fam = tf->fam[i];
    if (tf->prev_frame != frame) {
        tf->prev_frame = frame;
        tf->prev_loads = 0;
    }
    for (int32_t k = 0; k < tf->nprev; k++)
        if (strcmp(tf->prev[k].family, fam) == 0) {
            tf->prev[k].last = frame;
            return tf->prev[k].f;
        }
    if (tf->prev_loads >= TF_PREVIEW_LOADS_PER_FRAME) return NULL;
    tf->prev_loads++;
    if (tf->nprev < TF_PREVIEWS) {
        slot = &tf->prev[tf->nprev++];
    } else {
        /* evict the least recently drawn face (not one drawn this frame) */
        for (int32_t k = 0; k < tf->nprev; k++)
            if (tf->prev[k].last != frame && (!slot || tf->prev[k].last < slot->last))
                slot = &tf->prev[k];
        if (!slot) return NULL;
        ui_font_free(slot->f);
    }
    memset(slot, 0, sizeof *slot);
    app_copy_str(slot->family, sizeof slot->family, fam);
    slot->last = frame;
    if (ci_cmp(fam, TEXT_DEFAULT_FAMILY) == 0) {
        slot->f = ui_font_load_builtin(UI_FONT_REGULAR);
    } else if ((fi = pick(tf, fam, false, false)) != NULL &&
               pal_read_file(fi->path, TF_PREVIEW_MAX_FILE, &data, &len) == PC_OK) {
        if (ui_font_load_mem(data, len, fi->index, UI_FONT_COPY, &slot->f) != PC_OK) slot->f = NULL;
        free(data);
    }
    if (slot->f && !shows_own_name(slot->f, fam)) {
        ui_font_free(slot->f);
        slot->f = NULL;
    }
    return slot->f;
}

/* ---- scanning --------------------------------------------------------------------------- */
static bool font_ext(const char *name)
{
    const char *e = pal_path_ext(name);
    return ci_cmp(e, "ttf") == 0 || ci_cmp(e, "otf") == 0 || ci_cmp(e, "ttc") == 0 ||
           ci_cmp(e, "otc") == 0;
}

typedef struct tf_vec {
    text_face_info *v;
    size_t          n, cap;
} tf_vec;

static pc_status vec_push(tf_vec *v, const text_face_info *f)
{
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2u : 64u, bytes;
        text_face_info *nv;
        if (!pc_mul_size(nc, sizeof *nv, &bytes)) return PC_ERR_LIMIT;
        nv = (text_face_info *)realloc(v->v, bytes);
        if (!nv) return PC_ERR_NOMEM;
        v->v = nv;
        v->cap = nc;
    }
    v->v[v->n++] = *f;
    return PC_OK;
}

static int info_path_cmp(const void *x, const void *y)
{
    const text_face_info *a = (const text_face_info *)x, *b = (const text_face_info *)y;
    int c = strcmp(a->path, b->path);
    return c ? c : (a->index > b->index) - (a->index < b->index);
}

/* First index of path in the sorted list, or n. */
static size_t find_path(const text_face_info *v, size_t n, const char *path)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (strcmp(v[mid].path, path) < 0) lo = mid + 1u;
        else hi = mid;
    }
    return lo < n && strcmp(v[lo].path, path) == 0 ? lo : n;
}

static pc_status describe_file(const char *path, uint64_t mtime, tf_vec *out)
{
    uint8_t *data = NULL;
    size_t len = 0;
    int nf;
    pc_status st = pal_read_file(path, UI_FONT_MAX_FILE, &data, &len);
    if (st != PC_OK) return PC_OK;            /* unreadable: skipped */
    nf = ui_font_face_count(data, len);
    if (nf > TF_MAX_FACES_PER_FILE) nf = TF_MAX_FACES_PER_FILE;
    for (int k = 0; k < nf && st == PC_OK; k++) {
        ui_font_desc d;
        text_face_info fi;
        if (ui_font_describe(data, len, k, &d) != PC_OK || !d.family[0]) continue;
        memset(&fi, 0, sizeof fi);
        app_copy_str(fi.path, sizeof fi.path, path);
        fi.index = k;
        fi.mtime = mtime;
        app_copy_str(fi.family, sizeof fi.family, d.family);
        app_copy_str(fi.style, sizeof fi.style, d.style);
        fi.weight = d.weight;
        fi.italic = d.italic;
        {
            text_sfnt cs;
            if (text_sfnt_open(&cs, data, len, k) == PC_OK) {
                fi.color = text_sfnt_has_color(&cs);
                text_sfnt_close(&cs);
            }
        }
        st = vec_push(out, &fi);
    }
    free(data);
    return st;
}

pc_status text_fonts_scan_dirs(const char *const *dirs, const text_face_info *prev, size_t nprev,
                               pc_atomic_u32 *cancel, text_face_info **out, size_t *n)
{
    typedef struct dir_item { char *path; uint32_t depth; } dir_item;
    dir_item *stack = NULL;
    size_t sn = 0, scap = 0, files = 0;
    text_face_info *sorted = NULL;
    tf_vec v;
    pc_status st = PC_OK;
    memset(&v, 0, sizeof v);
    *out = NULL;
    *n = 0;
    if (nprev) {
        size_t bytes;
        if (!pc_mul_size(nprev, sizeof *sorted, &bytes)) return PC_ERR_LIMIT;
        sorted = (text_face_info *)malloc(bytes);
        if (!sorted) return PC_ERR_NOMEM;
        memcpy(sorted, prev, bytes);
        qsort(sorted, nprev, sizeof *sorted, info_path_cmp);
    }
    /* iterative walk (P-07): an explicit stack of directories */
    for (size_t i = 0; dirs && dirs[i] && st == PC_OK; i++) {
        if (sn == scap) {
            size_t nc = scap ? scap * 2u : 16u;
            dir_item *ns = (dir_item *)realloc(stack, nc * sizeof *ns);
            if (!ns) { st = PC_ERR_NOMEM; break; }
            stack = ns;
            scap = nc;
        }
        stack[sn].path = app_strdup(dirs[i]);
        stack[sn].depth = 0;
        if (!stack[sn].path) { st = PC_ERR_NOMEM; break; }
        sn++;
    }
    while (sn > 0 && st == PC_OK && files < TEXT_SCAN_MAX_FILES) {
        dir_item d = stack[--sn];
        if (cancel && pc_atomic_load(cancel)) {
            free(d.path);
            st = PC_ERR_CANCELLED;
            break;
        }
        char **names = NULL;
        int nn = pal_list_dir(d.path, NULL, &names);
        for (int i = 0; i < nn && st == PC_OK && files < TEXT_SCAN_MAX_FILES; i++) {
            char path[1024];
            if (cancel && pc_atomic_load(cancel)) {
                st = PC_ERR_CANCELLED;
                break;
            }
            if (!names[i] || names[i][0] == '.') continue;
            pal_path_join(path, sizeof path, d.path, names[i]);
            if (pal_is_dir(path)) {
                if (d.depth + 1u >= TEXT_SCAN_MAX_DEPTH) continue;
                if (sn == scap) {
                    size_t nc = scap * 2u;
                    dir_item *ns = (dir_item *)realloc(stack, nc * sizeof *ns);
                    if (!ns) { st = PC_ERR_NOMEM; break; }
                    stack = ns;
                    scap = nc;
                }
                stack[sn].path = app_strdup(path);
                stack[sn].depth = d.depth + 1u;
                if (!stack[sn].path) { st = PC_ERR_NOMEM; break; }
                sn++;
            } else if (font_ext(names[i]) && strchr(path, '\t') == NULL &&
                       strchr(path, '\n') == NULL) {
                uint64_t mt = pal_file_mtime(path);
                size_t k = sorted ? find_path(sorted, nprev, path) : nprev;
                files++;
                if (k < nprev && sorted[k].mtime == mt) {
                    for (; k < nprev && strcmp(sorted[k].path, path) == 0 && st == PC_OK; k++)
                        st = vec_push(&v, &sorted[k]);
                } else {
                    st = describe_file(path, mt, &v);
                }
            }
        }
        pal_free_names(names, nn);
        free(d.path);
    }
    while (sn > 0) free(stack[--sn].path);
    free(stack);
    free(sorted);
    if (st != PC_OK) {
        free(v.v);
        return st;
    }
    *out = v.v;
    *n = v.n;
    return PC_OK;
}

/* ---- the cache file ----------------------------------------------------------------------- */
pc_status text_fonts_cache_write(const char *path, const text_face_info *f, size_t n)
{
    size_t cap = 64u, len = 0;
    char *buf;
    pc_status st;
    for (size_t i = 0; i < n; i++) {
        size_t add = strlen(f[i].path) + strlen(f[i].family) + strlen(f[i].style) + 64u;
        if (!pc_add_size(cap, add, &cap)) return PC_ERR_LIMIT;
    }
    buf = (char *)malloc(cap);
    if (!buf) return PC_ERR_NOMEM;
    len += (size_t)snprintf(buf, cap, "paintc-fonts 2\n");
    for (size_t i = 0; i < n; i++) {
        int w;
        if (strpbrk(f[i].path, "\t\n\r") || strpbrk(f[i].family, "\t\n\r") ||
            strpbrk(f[i].style, "\t\n\r"))
            continue;
        w = snprintf(buf + len, cap - len, "%s\t%d\t%llu\t%u\t%d\t%s\t%s\t%d\n", f[i].path,
                     (int)f[i].index, (unsigned long long)f[i].mtime, (unsigned)f[i].weight,
                     f[i].italic ? 1 : 0, f[i].family, f[i].style, f[i].color ? 1 : 0);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
    st = pal_write_file_atomic(path, buf, len);
    free(buf);
    return st;
}

/* Next tab-separated field of line (in place). */
static char *field(char **p)
{
    char *s = *p, *t;
    if (!s) return NULL;
    t = strchr(s, '\t');
    if (t) {
        *t = '\0';
        *p = t + 1;
    } else {
        *p = NULL;
    }
    return s;
}

pc_status text_fonts_cache_read(const char *path, text_face_info **out, size_t *n)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char *p, *end;
    tf_vec v;
    pc_status st;
    *out = NULL;
    *n = 0;
    st = pal_read_file(path, TF_CACHE_MAX_BYTES, &data, &len);
    if (st != PC_OK) return st;
    memset(&v, 0, sizeof v);
    p = (char *)data;
    end = p + len;
    /* version 2 added the color field (lane TOOLB); version 1 caches are
     * rescanned */
    if (len < 15u || memcmp(p, "paintc-fonts 2\n", 15u) != 0) {
        free(data);
        return PC_ERR_FORMAT;
    }
    p += 15;
    while (p < end && st == PC_OK && v.n < (size_t)TEXT_SCAN_MAX_FILES * 4u) {
        char *nl = memchr(p, '\n', (size_t)(end - p)), *line = p, *cur;
        char *fp, *fi, *fm, *fw, *fit, *ff, *fs, *fc;
        text_face_info info;
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
        cur = line;
        fp = field(&cur);
        fi = field(&cur);
        fm = field(&cur);
        fw = field(&cur);
        fit = field(&cur);
        ff = field(&cur);
        fs = field(&cur);
        fc = field(&cur);
        if (!fp || !fi || !fm || !fw || !fit || !ff || !fs || !fc || !*fp || !*ff) continue;
        if (strlen(fp) >= sizeof info.path || strlen(ff) >= sizeof info.family ||
            strlen(fs) >= sizeof info.style)
            continue;
        memset(&info, 0, sizeof info);
        memcpy(info.path, fp, strlen(fp) + 1u);
        memcpy(info.family, ff, strlen(ff) + 1u);
        memcpy(info.style, fs, strlen(fs) + 1u);
        info.index = (int32_t)strtol(fi, NULL, 10);
        info.mtime = (uint64_t)strtoull(fm, NULL, 10);
        {
            long w = strtol(fw, NULL, 10);
            info.weight = (uint16_t)(w < 1 ? 400 : w > 1000 ? 1000 : w);
        }
        info.italic = fit[0] == '1';
        info.color = fc[0] == '1';
        if (info.index < 0 || info.index >= TF_MAX_FACES_PER_FILE) continue;
        st = vec_push(&v, &info);
    }
    free(data);
    if (st != PC_OK) {
        free(v.v);
        return st;
    }
    *out = v.v;
    *n = v.n;
    return PC_OK;
}

/* ---- the catalog --------------------------------------------------------------------------- */
typedef struct scan_job {
    pc_atomic_u32   cancel;     /* set on quit, polled by the worker */
    char          **dirs;       /* owned, NULL terminated */
    text_face_info *prev;       /* owned */
    size_t          nprev;
    text_face_info *out;
    size_t          nout;
    pc_status       st;
    char            cache[1024];
} scan_job;

static void scan_work(void *ud)
{
    scan_job *j = (scan_job *)ud;
    j->st = text_fonts_scan_dirs((const char *const *)j->dirs, j->prev, j->nprev, &j->cancel,
                                 &j->out, &j->nout);
    if (j->st == PC_OK && j->cache[0]) (void)text_fonts_cache_write(j->cache, j->out, j->nout);
}

static void scan_free(scan_job *j)
{
    if (!j) return;
    for (size_t i = 0; j->dirs && j->dirs[i]; i++) free(j->dirs[i]);
    free(j->dirs);
    free(j->prev);
    free(j->out);
    free(j);
}

static void scan_done(app *a, void *ud)
{
    scan_job *j = (scan_job *)ud;
    text_fonts *tf = (text_fonts *)app_ext_get(a, "text.fonts");
    if (tf) {
        tf->scanning = false;
        tf->job = NULL;
        if (j->st == PC_OK) (void)text_fonts_set_faces(tf, j->out, j->nout);
        app_tool_settings_changed(a);       /* the font list changed */
    }
    scan_free(j);
}

static void tf_destroy(void *p)
{
    text_fonts *tf = (text_fonts *)p;
    if (!tf) return;
    for (int32_t i = 0; i < tf->nprev; i++) ui_font_free(tf->prev[i].f);
    for (size_t i = 0; i < tf->nfaces; i++) face_free(tf->faces[i]);
    free(tf->faces);
    free(tf->sets);
    free_fams(tf);
    free(tf->sys);
    free(tf);
}

static void start_scan(app *a, text_fonts *tf)
{
    const char *const *dirs = pal_font_dirs();
    scan_job *j;
    size_t nd = 0;
    while (dirs && dirs[nd]) nd++;
    j = (scan_job *)calloc(1u, sizeof *j);
    if (!j) return;
    j->dirs = (char **)calloc(nd + 1u, sizeof *j->dirs);
    if (!j->dirs) { scan_free(j); return; }
    for (size_t i = 0; i < nd; i++) {
        j->dirs[i] = app_strdup(dirs[i]);
        if (!j->dirs[i]) { scan_free(j); return; }
    }
    if (tf->nsys) {
        j->prev = (text_face_info *)malloc(tf->nsys * sizeof *j->prev);
        if (!j->prev) { scan_free(j); return; }
        memcpy(j->prev, tf->sys, tf->nsys * sizeof *j->prev);
        j->nprev = tf->nsys;
    }
    app_copy_str(j->cache, sizeof j->cache, tf->cache_path);
    tf->scanning = true;
    tf->job = j;
    if (!app_task(a, scan_work, scan_done, j)) {
        tf->scanning = false;
        tf->job = NULL;
        scan_free(j);
    }
}

/* Quitting must not wait for a first scan of a large font folder. */
static void hook_quit(app *a, app_doc *d, void *ud)
{
    text_fonts *tf = (text_fonts *)app_ext_get(a, "text.fonts");
    (void)d;
    (void)ud;
    if (tf && tf->job) pc_atomic_store(&tf->job->cancel, 1u);
}

text_fonts *text_fonts_get(app *a)
{
    text_fonts *tf = (text_fonts *)app_ext_get(a, "text.fonts");
    char path[1024];
    if (tf) return tf;
    tf = (text_fonts *)calloc(1u, sizeof *tf);
    if (!tf) return NULL;
    tf->a = a;
    if (rebuild_families(tf) != PC_OK || !app_ext_set(a, "text.fonts", tf, tf_destroy)) {
        tf_destroy(tf);
        return NULL;
    }
    if (app_config_path(a, "fonts.cache", path, sizeof path)) {
        text_face_info *f = NULL;
        size_t n = 0;
        app_copy_str(tf->cache_path, sizeof tf->cache_path, path);
        if (text_fonts_cache_read(path, &f, &n) == PC_OK) {
            (void)text_fonts_set_faces(tf, f, n);
            free(f);
        }
        (void)app_hook_add(a, APP_HOOK_QUIT, hook_quit, NULL);
        start_scan(a, tf);
    }
    return tf;
}
