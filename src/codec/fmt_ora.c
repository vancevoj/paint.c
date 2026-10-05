/* fmt_ora.c - OpenRaster (.ora), layered (lane L6B).
 *
 * Load: the archive goes through the hardened reader in zip.c (zip-bomb
 * limits tied to lim->max_mem); "mimetype" must read "image/openraster";
 * stack.xml is parsed by a small strict XML subset parser (elements,
 * attributes, comments, processing instructions; only the five predefined
 * entities and numeric character references; any DOCTYPE or CDATA is
 * rejected; depth and counts bounded; no recursion). Layers are PNG files
 * decoded with the PNG codec's banded decoder straight into layers at
 * their x/y offsets, clipped to the canvas. Nested stacks are flattened:
 * their opacity multiplies in, a hidden stack hides its layers and stack
 * offsets add up. composite-op names map to blend modes (unknown ones
 * become Normal); xres/yres become meta.dpi. Layers whose source is not a
 * PNG (or missing) load as empty layers (meta.note says so).
 *
 * Save (PC_CODEC_LAYERED): a stored "mimetype" first, stack.xml (top layer
 * first; name, visibility, opacity, x/y, composite-op), each layer as a PNG
 * cropped to its non-empty tiles, mergedimage.png (full composite) and
 * Thumbnails/thumbnail.png (at most 256 x 256). Blend modes without an SVG
 * equivalent use "pdn-" names that this reader maps back.
 *
 * Threads: reentrant.
 */
#include "lib_codec.h"
#include "zip.h"

#include <stdlib.h>
#include <string.h>

#define ORA_MIME        "image/openraster"
#define ORA_XML_MAX     ((size_t)16 << 20)
#define ORA_XML_DEPTH   64
#define ORA_MAX_ATTRS   32
#define ORA_THUMB       256
#define ORA_POS_MIN     (-(int64_t)2147483647 - 1)
#define ORA_POS_MAX     ((int64_t)2147483647)
#define ORA_CLAMP       ((int64_t)1 << 30)

/* ---- blend mode names ------------------------------------------------------------------ */
static const char *const k_ops[PC_BLEND_COUNT] = {
    "svg:src-over", "svg:multiply", "svg:plus", "svg:color-burn", "svg:color-dodge",
    "pdn-reflect", "pdn-glow", "svg:overlay", "svg:difference", "pdn-negation",
    "svg:lighten", "svg:darken", "svg:screen", "pdn-xor"
};

static pc_blend_mode op_to_mode(const char *s)
{
    if (!s) return PC_BLEND_NORMAL;
    for (int i = 0; i < PC_BLEND_COUNT; i++)
        if (strcmp(s, k_ops[i]) == 0) return (pc_blend_mode)i;
    return PC_BLEND_NORMAL;
}

/* ---- strict XML subset ------------------------------------------------------------------ */
typedef struct xml_attr { const char *name, *value; } xml_attr;

typedef struct xml_node {
    const char *name;
    uint32_t    first_attr, n_attrs;
    int32_t     depth;
    bool        is_end;          /* closing event of an element */
} xml_node;

typedef struct xml_doc {
    char      *buf;             /* owned copy, NUL-terminated pieces */
    xml_node  *node;
    uint32_t   n_node, cap_node;
    xml_attr  *attr;
    uint32_t   n_attr, cap_attr;
} xml_doc;

static void xml_free(xml_doc *x)
{
    free(x->buf); free(x->node); free(x->attr);
    memset(x, 0, sizeof *x);
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool is_name_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80u;
}
static bool is_name_char(unsigned char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static size_t utf8_put(char *d, uint32_t cp)
{
    if (cp < 0x80u) { d[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        d[0] = (char)(0xC0u | (cp >> 6)); d[1] = (char)(0x80u | (cp & 63u));
        return 2;
    }
    if (cp < 0x10000u) {
        d[0] = (char)(0xE0u | (cp >> 12)); d[1] = (char)(0x80u | ((cp >> 6) & 63u));
        d[2] = (char)(0x80u | (cp & 63u));
        return 3;
    }
    d[0] = (char)(0xF0u | (cp >> 18)); d[1] = (char)(0x80u | ((cp >> 12) & 63u));
    d[2] = (char)(0x80u | ((cp >> 6) & 63u)); d[3] = (char)(0x80u | (cp & 63u));
    return 4;
}

/* Decode entities of s[0..n) in place (output never grows). */
static bool xml_unescape(char *s, size_t n, size_t *out_len)
{
    size_t r = 0, w = 0;
    while (r < n) {
        if (s[r] != '&') { s[w++] = s[r++]; continue; }
        {
            size_t e = r + 1;
            while (e < n && s[e] != ';' && e - r < 12u) e++;
            if (e >= n || s[e] != ';') return false;
            if (e - r == 3 && memcmp(s + r + 1, "lt", 2) == 0) s[w++] = '<';
            else if (e - r == 3 && memcmp(s + r + 1, "gt", 2) == 0) s[w++] = '>';
            else if (e - r == 4 && memcmp(s + r + 1, "amp", 3) == 0) s[w++] = '&';
            else if (e - r == 5 && memcmp(s + r + 1, "quot", 4) == 0) s[w++] = '"';
            else if (e - r == 5 && memcmp(s + r + 1, "apos", 4) == 0) s[w++] = '\'';
            else if (s[r + 1] == '#') {
                uint32_t cp = 0;
                size_t i = r + 2;
                bool hex = i < e && (s[i] == 'x' || s[i] == 'X');
                if (hex) i++;
                if (i >= e) return false;
                for (; i < e; i++) {
                    char c = s[i];
                    uint32_t d;
                    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                    else if (hex && c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                    else if (hex && c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                    else return false;
                    cp = cp * (hex ? 16u : 10u) + d;
                    if (cp > 0x10FFFFu) return false;
                }
                if (cp == 0u || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
                w += utf8_put(s + w, cp);
            } else {
                return false;            /* no other entities, no DTD */
            }
            r = e + 1;
        }
    }
    s[w] = '\0';
    *out_len = w;
    return true;
}

static bool xml_push_node(xml_doc *x, const xml_node *nd, uint32_t max_nodes)
{
    if (x->n_node == x->cap_node) {
        uint32_t nc = x->cap_node ? x->cap_node * 2u : 64u;
        xml_node *p;
        if (x->n_node >= max_nodes) return false;
        p = (xml_node *)realloc(x->node, (size_t)nc * sizeof *p);
        if (!p) return false;
        x->node = p;
        x->cap_node = nc;
    }
    x->node[x->n_node++] = *nd;
    return true;
}

static bool xml_push_attr(xml_doc *x, const char *name, const char *value)
{
    if (x->n_attr == x->cap_attr) {
        uint32_t nc = x->cap_attr ? x->cap_attr * 2u : 64u;
        xml_attr *p = (xml_attr *)realloc(x->attr, (size_t)nc * sizeof *p);
        if (!p) return false;
        x->attr = p;
        x->cap_attr = nc;
    }
    x->attr[x->n_attr].name = name;
    x->attr[x->n_attr].value = value;
    x->n_attr++;
    return true;
}

/* Parse text[0..n) into start/end events. PC_ERR_FORMAT on anything outside
 * the subset, PC_ERR_LIMIT on excessive depth or counts. */
static pc_status xml_parse(xml_doc *x, const uint8_t *text, size_t n, uint32_t max_nodes)
{
    const char *stack[ORA_XML_DEPTH];
    int32_t depth = 0;
    size_t i = 0;
    bool root_done = false;
    char *s;
    memset(x, 0, sizeof *x);
    x->buf = (char *)malloc(n + 1u);
    if (!x->buf) return PC_ERR_NOMEM;
    memcpy(x->buf, text, n);
    x->buf[n] = '\0';
    s = x->buf;
    if (n >= 3u && (uint8_t)s[0] == 0xEFu && (uint8_t)s[1] == 0xBBu && (uint8_t)s[2] == 0xBFu)
        i = 3;                                                     /* UTF-8 BOM */
    while (i < n) {
        if (s[i] != '<') {
            if (s[i] == '\0') return PC_ERR_FORMAT;
            if (depth == 0 && !is_ws(s[i])) return PC_ERR_FORMAT;   /* text outside root */
            i++;
            continue;
        }
        if (i + 1 >= n) return PC_ERR_FORMAT;
        if (s[i + 1] == '?') {                                     /* processing instruction */
            const char *e = strstr(s + i + 2, "?>");
            if (!e) return PC_ERR_FORMAT;
            i = (size_t)(e - s) + 2u;
            continue;
        }
        if (s[i + 1] == '!') {
            const char *e;
            if (strncmp(s + i, "<!--", 4) != 0) return PC_ERR_FORMAT; /* DOCTYPE, CDATA */
            e = strstr(s + i + 4, "-->");
            if (!e) return PC_ERR_FORMAT;
            i = (size_t)(e - s) + 3u;
            continue;
        }
        if (s[i + 1] == '/') {                                     /* end tag */
            size_t b = i + 2, e = b;
            xml_node nd;
            while (e < n && is_name_char((unsigned char)s[e])) e++;
            if (e == b || depth == 0) return PC_ERR_FORMAT;
            {
                size_t k = e;
                while (k < n && is_ws(s[k])) k++;
                if (k >= n || s[k] != '>') return PC_ERR_FORMAT;
                s[e] = '\0';
                if (strcmp(s + b, stack[depth - 1]) != 0) return PC_ERR_FORMAT;
                i = k + 1;
            }
            depth--;
            memset(&nd, 0, sizeof nd);
            nd.name = stack[depth];
            nd.depth = depth;
            nd.is_end = true;
            if (!xml_push_node(x, &nd, max_nodes)) return PC_ERR_LIMIT;
            if (depth == 0) root_done = true;
            continue;
        }
        {                                                          /* start tag */
            size_t b = i + 1, e = b;
            xml_node nd;
            bool self_close = false;
            if (root_done && depth == 0) return PC_ERR_FORMAT;     /* second root */
            if (!is_name_start((unsigned char)s[b])) return PC_ERR_FORMAT;
            while (e < n && is_name_char((unsigned char)s[e])) e++;
            memset(&nd, 0, sizeof nd);
            nd.name = s + b;
            nd.depth = depth;
            nd.first_attr = x->n_attr;
            i = e;
            for (;;) {
                size_t an, ae, vb, ve, vlen;
                char q;
                bool ws = false;
                while (i < n && is_ws(s[i])) { i++; ws = true; }
                if (i >= n) return PC_ERR_FORMAT;
                if (s[i] == '>') { s[e] = '\0'; i++; break; }
                if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') {
                    s[e] = '\0'; i += 2; self_close = true; break;
                }
                if (!ws || !is_name_start((unsigned char)s[i])) return PC_ERR_FORMAT;
                an = i;
                while (i < n && is_name_char((unsigned char)s[i])) i++;
                ae = i;
                while (i < n && is_ws(s[i])) i++;
                if (i >= n || s[i] != '=') return PC_ERR_FORMAT;
                i++;
                while (i < n && is_ws(s[i])) i++;
                if (i >= n || (s[i] != '"' && s[i] != '\'')) return PC_ERR_FORMAT;
                q = s[i++];
                vb = i;
                while (i < n && s[i] != q) {
                    if (s[i] == '<' || s[i] == '\0') return PC_ERR_FORMAT;
                    i++;
                }
                if (i >= n) return PC_ERR_FORMAT;
                ve = i++;
                if (nd.n_attrs >= ORA_MAX_ATTRS) return PC_ERR_LIMIT;
                s[ae] = '\0';
                if (!xml_unescape(s + vb, ve - vb, &vlen)) return PC_ERR_FORMAT;
                for (uint32_t k = nd.first_attr; k < x->n_attr; k++)
                    if (strcmp(x->attr[k].name, s + an) == 0) return PC_ERR_FORMAT;  /* dup */
                if (!xml_push_attr(x, s + an, s + vb)) return PC_ERR_NOMEM;
                nd.n_attrs++;
            }
            if (!xml_push_node(x, &nd, max_nodes)) return PC_ERR_LIMIT;
            if (self_close) {
                xml_node en = nd;
                en.is_end = true;
                en.n_attrs = 0;
                if (!xml_push_node(x, &en, max_nodes)) return PC_ERR_LIMIT;
                if (depth == 0) root_done = true;
            } else {
                if (depth >= ORA_XML_DEPTH) return PC_ERR_LIMIT;
                stack[depth++] = nd.name;
            }
        }
    }
    if (depth != 0 || !root_done) return PC_ERR_FORMAT;
    return PC_OK;
}

static const char *xml_get(const xml_doc *x, const xml_node *nd, const char *name)
{
    for (uint32_t k = 0; k < nd->n_attrs; k++)
        if (strcmp(x->attr[nd->first_attr + k].name, name) == 0)
            return x->attr[nd->first_attr + k].value;
    return NULL;
}

/* Locale-independent number parsing. */
static bool parse_int(const char *s, int64_t lo, int64_t hi, int64_t *out)
{
    int64_t v = 0;
    bool neg = false, any = false;
    if (!s) return false;
    while (is_ws(*s)) s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        any = true;
        if (v > (int64_t)1 << 40) return false;
    }
    while (is_ws(*s)) s++;
    if (!any || *s) return false;
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool parse_real(const char *s, double *out)
{
    double v = 0.0, scale = 1.0;
    bool neg = false, any = false;
    if (!s) return false;
    while (is_ws(*s)) s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++) { if (v < 1e12) v = v * 10.0 + (*s - '0'); any = true; }
    if (*s == '.') {
        s++;
        for (; *s >= '0' && *s <= '9'; s++) { scale *= 0.1; v += (*s - '0') * scale; any = true; }
    }
    if (*s == 'e' || *s == 'E') {
        int64_t ex;
        int e10;
        char buf[8];
        size_t k = 0;
        s++;
        while ((*s == '-' || *s == '+' || (*s >= '0' && *s <= '9')) && k < sizeof buf - 1u)
            buf[k++] = *s++;
        buf[k] = '\0';
        if (!parse_int(buf, -300, 300, &ex)) return false;
        for (e10 = 0; e10 < (ex < 0 ? -ex : ex); e10++) v = ex < 0 ? v / 10.0 : v * 10.0;
    }
    while (is_ws(*s)) s++;
    if (!any || *s) return false;
    *out = neg ? -v : v;
    return true;
}

/* ---- loading ------------------------------------------------------------------------------ */
typedef struct ora_layer {
    const char   *name, *src;
    int64_t       x, y;
    double        opacity;
    bool          visible;
    pc_blend_mode mode;
} ora_layer;

typedef struct ora_frame { double opacity; bool visible; int64_t x, y; } ora_frame;

typedef struct ora_png_ctx {
    const pc_doc   *d;
    lc_layer_sink   sink;
    int64_t         x, y;
} ora_png_ctx;

static pc_status ora_png_hdr(void *ud, uint32_t w, uint32_t h)
{
    ora_png_ctx *c = (ora_png_ctx *)ud;
    (void)h;
    c->sink.w = (int32_t)w;
    return PC_OK;
}

static pc_status ora_png_rows(void *ud, int32_t y0, int32_t n, const pc_px32 *rows)
{
    ora_png_ctx *c = (ora_png_ctx *)ud;
    return lc_sink_layer(&c->sink, y0, n, rows);
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool ends_with_png(const char *s)
{
    size_t n = strlen(s);
    return n >= 4u && s[n - 4] == '.' && (s[n - 3] | 0x20) == 'p' && (s[n - 2] | 0x20) == 'n' &&
           (s[n - 1] | 0x20) == 'g';
}

static pc_status ora_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    pc_codec_limits dl;
    pc_zip_limits zl;
    pc_zip z;
    xml_doc x;
    ora_layer *layers = NULL;
    uint32_t n_layers = 0;
    ora_frame frames[ORA_XML_DEPTH + 1];
    int32_t nf = 0;
    const xml_node *image = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    pc_doc *d = NULL;
    pc_status st;
    int64_t w = 0, h = 0, v;
    bool skipped = false;
    const pc_zip_entry *e;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    memset(&x, 0, sizeof x);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    pc_zip_limits_default(&zl);
    zl.max_total = lim->max_mem;
    zl.max_entries = lim->max_layers + 64u;
    st = pc_zip_open(&z, p, n, &zl);
    if (st != PC_OK) return st;
    /* mimetype */
    e = pc_zip_find(&z, "mimetype");
    if (!e || e->usize > 64u) { st = PC_ERR_FORMAT; goto done; }
    st = pc_zip_read(&z, e, &data, &len);
    if (st != PC_OK) goto done;
    while (len && is_ws((char)data[len - 1])) len--;
    if (len != sizeof ORA_MIME - 1u || memcmp(data, ORA_MIME, len) != 0) {
        st = PC_ERR_FORMAT;
        goto done;
    }
    free(data);
    data = NULL;
    /* stack.xml */
    e = pc_zip_find(&z, "stack.xml");
    if (!e) { st = PC_ERR_FORMAT; goto done; }
    if (e->usize > ORA_XML_MAX) { st = PC_ERR_LIMIT; goto done; }
    st = pc_zip_read(&z, e, &data, &len);
    if (st != PC_OK) goto done;
    st = xml_parse(&x, data, len, 2u * lim->max_layers + 4u * ORA_XML_DEPTH + 64u);
    free(data);
    data = NULL;
    if (st != PC_OK) goto done;
    if (x.n_node == 0 || x.node[0].is_end || strcmp(x.node[0].name, "image") != 0) {
        st = PC_ERR_FORMAT;
        goto done;
    }
    image = &x.node[0];
    if (!parse_int(xml_get(&x, image, "w"), 1, PC_MAX_DIM, &w) ||
        !parse_int(xml_get(&x, image, "h"), 1, PC_MAX_DIM, &h)) {
        st = parse_int(xml_get(&x, image, "w"), 1, INT32_MAX, &v) ? PC_ERR_LIMIT : PC_ERR_FORMAT;
        goto done;
    }
    if (parse_int(xml_get(&x, image, "xres"), 1, 1000000, &v)) meta->dpi_x = (double)v;
    if (parse_int(xml_get(&x, image, "yres"), 1, 1000000, &v)) meta->dpi_y = (double)v;
    if (meta->dpi_x <= 0.0 || meta->dpi_y <= 0.0) meta->dpi_x = meta->dpi_y = 0.0;
    /* collect layers in document order (top first), flattening stacks */
    layers = (ora_layer *)calloc((size_t)x.n_node, sizeof *layers);
    if (!layers) { st = PC_ERR_NOMEM; goto done; }
    frames[0].opacity = 1.0; frames[0].visible = true; frames[0].x = frames[0].y = 0;
    for (uint32_t i = 1; i < x.n_node; i++) {
        const xml_node *nd = &x.node[i];
        if (strcmp(nd->name, "stack") == 0) {
            if (nd->is_end) {
                if (nf > 0) nf--;
                continue;
            }
            if (nf >= ORA_XML_DEPTH) { st = PC_ERR_LIMIT; goto done; }
            frames[nf + 1] = frames[nf];
            nf++;
            if (nf > 1) {           /* the root stack carries no attributes */
                double o;
                const char *vis = xml_get(&x, nd, "visibility");
                if (parse_real(xml_get(&x, nd, "opacity"), &o))
                    frames[nf].opacity *= o < 0.0 ? 0.0 : (o > 1.0 ? 1.0 : o);
                if (vis && strcmp(vis, "hidden") == 0) frames[nf].visible = false;
                if (parse_int(xml_get(&x, nd, "x"), ORA_POS_MIN, ORA_POS_MAX, &v))
                    frames[nf].x += v;
                if (parse_int(xml_get(&x, nd, "y"), ORA_POS_MIN, ORA_POS_MAX, &v))
                    frames[nf].y += v;
            }
        } else if (strcmp(nd->name, "layer") == 0 && !nd->is_end) {
            ora_layer *l = &layers[n_layers];
            double o = 1.0;
            const char *vis = xml_get(&x, nd, "visibility");
            if (n_layers >= lim->max_layers) { st = PC_ERR_LIMIT; goto done; }
            l->src = xml_get(&x, nd, "src");
            l->name = xml_get(&x, nd, "name");
            l->x = frames[nf].x;
            l->y = frames[nf].y;
            if (parse_int(xml_get(&x, nd, "x"), ORA_POS_MIN, ORA_POS_MAX, &v)) l->x += v;
            if (parse_int(xml_get(&x, nd, "y"), ORA_POS_MIN, ORA_POS_MAX, &v)) l->y += v;
            if (!parse_real(xml_get(&x, nd, "opacity"), &o)) o = 1.0;
            o = o < 0.0 ? 0.0 : (o > 1.0 ? 1.0 : o);
            l->opacity = o * frames[nf].opacity;
            l->visible = frames[nf].visible && !(vis && strcmp(vis, "hidden") == 0);
            l->mode = op_to_mode(xml_get(&x, nd, "composite-op"));
            n_layers++;
        }
    }
    st = pc_codec_check_size(lim, (uint64_t)w, (uint64_t)h, n_layers ? n_layers : 1u);
    if (st != PC_OK) goto done;
    d = pc_doc_create((uint32_t)w, (uint32_t)h);
    if (!d) { st = PC_ERR_NOMEM; goto done; }
    st = pc_doc_reserve_layers(d, n_layers ? n_layers : 1u);
    if (st != PC_OK) goto done;
    for (uint32_t k = n_layers; k-- > 0;) {                  /* bottom layer first */
        const ora_layer *ol = &layers[k];
        char name[PC_LAYER_NAME_MAX];
        pc_layer *l;
        if (ol->name) lc_utf8_copy(name, sizeof name, ol->name, strlen(ol->name));
        else memcpy(name, "Layer", 6u);
        l = pc_layer_create(d, name);
        if (!l) { st = PC_ERR_NOMEM; goto done; }
        l->mode = ol->mode;
        l->opacity = (uint8_t)(ol->opacity * 255.0 + 0.5);
        l->visible = ol->visible;
        st = pc_doc_insert_layer(d, l, d->n_layers);
        if (st != PC_OK) { pc_layer_destroy(l); goto done; }
        e = (ol->src && ends_with_png(ol->src)) ? pc_zip_find(&z, ol->src) : NULL;
        if (!e) { skipped = true; continue; }
        st = pc_zip_read(&z, e, &data, &len);
        if (st == PC_OK) {
            ora_png_ctx c;
            memset(&c, 0, sizeof c);
            c.d = d;
            c.sink.d = d;
            c.sink.l = l;
            c.sink.x = (int32_t)clamp64(ol->x, -ORA_CLAMP, ORA_CLAMP);
            c.sink.y = (int32_t)clamp64(ol->y, -ORA_CLAMP, ORA_CLAMP);
            st = lc_png_decode(data, len, lim, ora_png_hdr, ora_png_rows, &c, NULL, NULL);
        }
        free(data);
        data = NULL;
        if (st != PC_OK) goto done;
    }
    if (d->n_layers == 0) {
        pc_layer *l = pc_layer_create(d, "Background");
        if (!l) { st = PC_ERR_NOMEM; goto done; }
        st = pc_doc_insert_layer(d, l, 0);
        if (st != PC_OK) { pc_layer_destroy(l); goto done; }
    }
    meta->src_bits = 8u;
    meta->had_alpha = true;
    if (skipped)
        lc_note(meta, "Some layers are not PNG images or are missing; they were loaded empty");
done:
    free(data);
    free(layers);
    xml_free(&x);
    pc_zip_close(&z);
    if (st != PC_OK) {
        pc_doc_destroy(d);
        pc_meta_free(meta);
        return st;
    }
    *out = d;
    return PC_OK;
}

static bool ora_sniff(const uint8_t *p, size_t n)
{
    /* first local header: "PK\3\4", stored file named "mimetype" */
    size_t nl, el;
    if (!p || n < 38u || memcmp(p, "PK\3\4", 4u) != 0) return false;
    nl = (size_t)p[26] | ((size_t)p[27] << 8);
    el = (size_t)p[28] | ((size_t)p[29] << 8);
    if (nl != 8u || memcmp(p + 30, "mimetype", 8u) != 0) return false;
    if (n < 38u + el + sizeof ORA_MIME - 1u) return true;   /* short prefix: plausible */
    return memcmp(p + 38u + el, ORA_MIME, sizeof ORA_MIME - 1u) == 0;
}

/* ---- saving --------------------------------------------------------------------------------- */
static pc_status xml_put(pc_buf *b, const char *s) { return pc_buf_append(b, s, strlen(s)); }

static pc_status xml_put_escaped(pc_buf *b, const char *s)
{
    pc_status st = PC_OK;
    for (; *s && st == PC_OK; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '&': st = xml_put(b, "&amp;"); break;
        case '<': st = xml_put(b, "&lt;"); break;
        case '>': st = xml_put(b, "&gt;"); break;
        case '"': st = xml_put(b, "&quot;"); break;
        case '\'': st = xml_put(b, "&apos;"); break;
        case '\t': st = xml_put(b, "&#9;"); break;
        case '\n': st = xml_put(b, "&#10;"); break;
        case '\r': st = xml_put(b, "&#13;"); break;
        default:
            if (c >= 0x20u) st = pc_buf_put_u8(b, c);      /* other controls are invalid XML */
            break;
        }
    }
    return st;
}

static pc_status xml_put_int(pc_buf *b, int64_t v)
{
    char t[24];
    size_t k = sizeof t;
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    t[--k] = '\0';
    do { t[--k] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);
    if (neg) t[--k] = '-';
    return xml_put(b, t + k);
}

static pc_status xml_put_opacity(pc_buf *b, uint8_t op)
{
    char t[8];
    uint32_t m = ((uint32_t)op * 1000u + 127u) / 255u;    /* thousandths, exact round trip */
    t[0] = (char)('0' + (int)(m / 1000u));
    t[1] = '.';
    t[2] = (char)('0' + (int)(m / 100u % 10u));
    t[3] = (char)('0' + (int)(m / 10u % 10u));
    t[4] = (char)('0' + (int)(m % 10u));
    t[5] = '\0';
    return xml_put(b, t);
}

/* Bounds of a layer's non-empty tiles, clipped to the document. */
static pc_rect layer_bounds(const pc_doc *d, const pc_layer *l)
{
    pc_rect r = pc_rect_make(0, 0, 0, 0);
    for (uint32_t ty = 0; ty < d->tiles_y; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x; tx++)
            if (l->grid[(size_t)ty * d->tiles_x + tx])
                r = pc_rect_union(r, pc_rect_make((int32_t)(tx * PC_TILE_DIM),
                                                  (int32_t)(ty * PC_TILE_DIM),
                                                  (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM));
    r = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(r)) r = pc_rect_make(0, 0, 1, 1);
    return r;
}

typedef struct ora_layer_src {
    const pc_doc   *d;
    const pc_layer *l;
    pc_rect         r;
} ora_layer_src;

static pc_status ora_src_layer(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    const ora_layer_src *s = (const ora_layer_src *)ud;
    pc_layer_read_rect(s->d, s->l, pc_rect_make(s->r.x, s->r.y + y0, s->r.w, n), dst,
                       (size_t)s->r.w);
    return PC_OK;
}

static void layer_file(char *buf, size_t cap, uint32_t index)
{
    char num[12];
    size_t k = sizeof num, pre = 10u;   /* "data/layer" */
    num[--k] = '\0';
    do { num[--k] = (char)('0' + (int)(index % 10u)); index /= 10u; } while (index);
    if (cap < pre + (sizeof num - k) + 4u) { buf[0] = '\0'; return; }
    memcpy(buf, "data/layer", pre);
    memcpy(buf + pre, num + k, sizeof num - k - 1u);
    memcpy(buf + pre + (sizeof num - k - 1u), ".png", 5u);
}

static pc_status ora_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    pc_zipw zw;
    pc_buf xml, png;
    pc_rect *rects = NULL;
    pc_status st = PC_OK;
    lc_png_opts po;
    size_t n0;
    int64_t dpi_x, dpi_y;
    (void)params;
    if (!d || !out || d->n_layers == 0) return PC_ERR_ARG;
    memset(&xml, 0, sizeof xml);
    memset(&png, 0, sizeof png);
    memset(&po, 0, sizeof po);
    po.kind = LC_PNG_RGBA;
    po.level = -1;
    n0 = out->n;
    rects = (pc_rect *)malloc((size_t)d->n_layers * sizeof *rects);
    if (!rects) return PC_ERR_NOMEM;
    for (uint32_t i = 0; i < d->n_layers; i++) rects[i] = layer_bounds(d, d->stack[i]);
    dpi_x = meta && meta->dpi_x > 0.0 ? (int64_t)(meta->dpi_x + 0.5) : 96;
    dpi_y = meta && meta->dpi_y > 0.0 ? (int64_t)(meta->dpi_y + 0.5) : 96;
    if (dpi_x < 1) dpi_x = 1;
    if (dpi_y < 1) dpi_y = 1;
    /* stack.xml, top layer first */
    st = xml_put(&xml, "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"");
    if (st == PC_OK) st = xml_put_int(&xml, d->w);
    if (st == PC_OK) st = xml_put(&xml, "\" h=\"");
    if (st == PC_OK) st = xml_put_int(&xml, d->h);
    if (st == PC_OK) st = xml_put(&xml, "\" xres=\"");
    if (st == PC_OK) st = xml_put_int(&xml, dpi_x);
    if (st == PC_OK) st = xml_put(&xml, "\" yres=\"");
    if (st == PC_OK) st = xml_put_int(&xml, dpi_y);
    if (st == PC_OK) st = xml_put(&xml, "\">\n <stack>\n");
    for (uint32_t k = d->n_layers; k-- > 0 && st == PC_OK;) {
        const pc_layer *l = d->stack[k];
        char file[40];
        layer_file(file, sizeof file, k);
        st = xml_put(&xml, "  <layer name=\"");
        if (st == PC_OK) st = xml_put_escaped(&xml, l->name);
        if (st == PC_OK) st = xml_put(&xml, "\" visibility=\"");
        if (st == PC_OK) st = xml_put(&xml, l->visible ? "visible" : "hidden");
        if (st == PC_OK) st = xml_put(&xml, "\" opacity=\"");
        if (st == PC_OK) st = xml_put_opacity(&xml, l->opacity);
        if (st == PC_OK) st = xml_put(&xml, "\" x=\"");
        if (st == PC_OK) st = xml_put_int(&xml, rects[k].x);
        if (st == PC_OK) st = xml_put(&xml, "\" y=\"");
        if (st == PC_OK) st = xml_put_int(&xml, rects[k].y);
        if (st == PC_OK) st = xml_put(&xml, "\" composite-op=\"");
        if (st == PC_OK)
            st = xml_put(&xml, k_ops[(unsigned)l->mode < PC_BLEND_COUNT ? l->mode : 0]);
        if (st == PC_OK) st = xml_put(&xml, "\" src=\"");
        if (st == PC_OK) st = xml_put(&xml, file);
        if (st == PC_OK) st = xml_put(&xml, "\"/>\n");
    }
    if (st == PC_OK) st = xml_put(&xml, " </stack>\n</image>\n");
    if (st != PC_OK) goto done;

    pc_zipw_init(&zw, out);
    st = pc_zipw_add(&zw, "mimetype", ORA_MIME, sizeof ORA_MIME - 1u, false);
    if (st == PC_OK) st = pc_zipw_add(&zw, "stack.xml", xml.p, xml.n, true);
    for (uint32_t k = 0; k < d->n_layers && st == PC_OK; k++) {
        ora_layer_src s;
        char file[40];
        s.d = d;
        s.l = d->stack[k];
        s.r = rects[k];
        layer_file(file, sizeof file, k);
        png.n = 0;
        st = lc_png_encode(&png, (uint32_t)s.r.w, (uint32_t)s.r.h, ora_src_layer, &s, &po);
        if (st == PC_OK) st = pc_zipw_add(&zw, file, png.p, png.n, false);
    }
    if (st == PC_OK) {                                   /* full composite */
        lc_flat f;
        f.d = d; f.par = par; f.over_white = false;
        png.n = 0;
        st = lc_png_encode(&png, d->w, d->h, lc_src_flatten, &f, &po);
        if (st == PC_OK) st = pc_zipw_add(&zw, "mergedimage.png", png.p, png.n, false);
    }
    if (st == PC_OK) {                                   /* thumbnail, at most 256 x 256 */
        lc_flat f;
        pc_surf th;
        int32_t tw = (int32_t)d->w, tht = (int32_t)d->h;
        if (tw > ORA_THUMB || tht > ORA_THUMB) {
            if (d->w >= d->h) {
                tw = ORA_THUMB;
                tht = (int32_t)(((uint64_t)d->h * ORA_THUMB + d->w / 2u) / d->w);
            } else {
                tht = ORA_THUMB;
                tw = (int32_t)(((uint64_t)d->w * ORA_THUMB + d->h / 2u) / d->h);
            }
            if (tw < 1) tw = 1;
            if (tht < 1) tht = 1;
        }
        f.d = d; f.par = par; f.over_white = false;
        st = pc_surf_alloc(&th, tw, tht);
        if (st == PC_OK) {
            st = lc_resample((int32_t)d->w, (int32_t)d->h, lc_src_flatten, &f, th.px, tw, tht,
                             (size_t)th.stride, LC_FILTER_FANT, false);
            png.n = 0;
            if (st == PC_OK) st = lc_png_encode(&png, (uint32_t)tw, (uint32_t)tht, lc_src_surf,
                                                &th, &po);
            if (st == PC_OK) st = pc_zipw_add(&zw, "Thumbnails/thumbnail.png", png.p, png.n, false);
            pc_surf_free(&th);
        }
    }
    if (st == PC_OK) st = pc_zipw_finish(&zw);
    pc_zipw_free(&zw);
done:
    pc_buf_free(&xml);
    pc_buf_free(&png);
    free(rects);
    if (st != PC_OK) out->n = n0;
    return st;
}

const pc_codec pc_codec_ora = {
    "ora", "OpenRaster", "ora", PC_CODEC_LOAD | PC_CODEC_SAVE | PC_CODEC_LAYERED,
    ora_sniff, ora_load,
    NULL, 0, 0,
    ora_save
};
