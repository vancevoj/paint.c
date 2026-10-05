/* pdn_util.h - shared helpers for the .pdn tests (lane L6C): file I/O,
 * synthetic documents and the in-repo sample list. Header-only, test code. */
#ifndef PDN_UTIL_H
#define PDN_UTIL_H

#include "pc_test.h"
#include "../../src/codec/nrbf.h"
#include "../../src/codec/pdn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Samples shipped in tests/codec/samples/pdn (see SOURCES.md there). The
 * tests run with the source directory tests/codec as working directory. */
#define PDN_SAMPLE_DIR "samples/pdn/"

static const char *const k_pdn_samples[] = {
    "bevy_bar_border.pdn",          /* 5.0.3, 3 layers, duplicated layer name */
    "bevy_health_bar_fill.pdn",     /* 5.0.3, 2 layers */
    "bevy_posture_bar_fill.pdn",    /* 5.0.3, 2 layers */
    "bevy_posture_bar_top.pdn",     /* 5.0.3, 2 layers */
    "pypdn_Untitled.pdn",           /* 4.0.21, 1 layer */
    "pypdn_Untitled2.pdn",          /* 4.0.21, 2 layers */
    "pypdn_Untitled3.pdn",          /* 4.0.21, 2 layers, Additive, opacity */
    "pypdn_oldPDN3510.pdn",         /* 3.5.10, legacy blend op classes */
    "pypdn_FlattenBlendTest.pdn",   /* 4.0.21, 14 layers, all blend modes */
    NULL
};

static inline uint8_t *pdn_read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = NULL;
    long len;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        p = (uint8_t *)malloc((size_t)len + 1u);
        if (p && fread(p, 1u, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
        if (p) *n = (size_t)len;
    }
    fclose(f);
    return p;
}

static inline int pdn_write_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(p, 1u, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* Random straight-alpha pixel; some transparent pixels keep stray color
 * like real Paint.NET layers do. */
static inline pc_px32 pdn_rpx(void)
{
    pc_px32 p;
    uint32_t k = rndu(8);
    p.b = rnd8(); p.g = rnd8(); p.r = rnd8();
    p.a = k == 0 ? 0 : (k == 1 ? 255 : rnd8());
    if (k == 0 && rndu(2)) p.b = p.g = p.r = 0;
    return p;
}

/* Random layer content: noise, solid blocks and empty areas (NULL tiles). */
static inline void pdn_fill_layer(pc_doc *d, pc_layer *l)
{
    pc_surf s;
    uint32_t kind = rndu(4);
    if (pc_surf_alloc(&s, (int32_t)d->w, (int32_t)d->h) != PC_OK) return;
    for (int32_t y = 0; y < s.h; y++)
        for (int32_t x = 0; x < s.w; x++) {
            pc_px32 *o = &s.px[(size_t)y * (size_t)s.stride + (size_t)x];
            switch (kind) {
            case 0: *o = pdn_rpx(); break;
            case 1: if (x < s.w / 2) *o = pdn_rpx(); break;
            case 2:
                o->b = (uint8_t)x; o->g = (uint8_t)y; o->r = (uint8_t)(x ^ y); o->a = 200;
                break;
            default: if (((x >> 3) + (y >> 3)) & 1) *o = pdn_rpx(); break;
            }
        }
    (void)pc_layer_store_rect(d, l, pc_doc_rect(d), s.px, (size_t)s.stride);
    pc_surf_free(&s);
}

/* Document with n random layers, random names (some UTF-8, some equal),
 * blend modes, opacities and visibility. */
static inline pc_doc *pdn_random_doc(uint32_t w, uint32_t h, uint32_t n)
{
    static const char *const names[] = {
        "Background", "Layer 2", "Ebene \xc3\xa4\xc3\xb6\xc3\xbc", "\xe8\x83\x8c\xe6\x99\xaf",
        "Emoji \xf0\x9f\x8e\xa8", "", "Layer 2", "a \"quoted\" <name> & more"
    };
    pc_doc *d = pc_doc_create(w, h);
    if (!d) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        char nm[PC_LAYER_NAME_MAX];
        pc_layer *l;
        snprintf(nm, sizeof nm, "%s", names[rndu((uint32_t)(sizeof names / sizeof names[0]))]);
        l = pc_layer_create(d, nm);
        if (!l) break;
        pdn_fill_layer(d, l);
        l->mode = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        l->opacity = (uint8_t)(rndu(3) == 0 ? 255u : rnd8());
        l->visible = rndu(5) != 0;
        if (pc_doc_insert_layer(d, l, d->n_layers) != PC_OK) { pc_layer_destroy(l); break; }
    }
    return d;
}

/* Exact equality of size, layer order, properties and every pixel. */
static inline int pdn_docs_equal(const pc_doc *a, const pc_doc *b)
{
    if (a->w != b->w || a->h != b->h || a->n_layers != b->n_layers) return 0;
    for (uint32_t i = 0; i < a->n_layers; i++) {
        const pc_layer *x = a->stack[i], *y = b->stack[i];
        if (strcmp(x->name, y->name) != 0 || x->mode != y->mode || x->opacity != y->opacity ||
            x->visible != y->visible)
            return 0;
        for (uint32_t py = 0; py < a->h; py++)
            for (uint32_t px = 0; px < a->w; px++) {
                pc_px32 p = pc_layer_get_px(x, px, py), q = pc_layer_get_px(y, px, py);
                if (memcmp(&p, &q, 4u) != 0) return 0;
            }
    }
    return 1;
}

#endif /* PDN_UTIL_H */
