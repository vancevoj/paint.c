/* doc_trc.c - lane W3B-FXCORE: per-image transfer curves (doc_trc.h). */
#include "doc_trc.h"

#include "pc/pc_icc.h"

#include <stdlib.h>
#include <string.h>

#define TRC_SLOTS 4

typedef struct trc_slot {
    uint32_t doc_id;
    uint64_t hash;           /* FNV-1a of the profile bytes */
    size_t   len;
    pc_trc  *trc;            /* owned, NULL = sRGB */
    bool     used;
} trc_slot;

typedef struct trc_cache { trc_slot s[TRC_SLOTS]; } trc_cache;

static void cache_free(void *p)
{
    trc_cache *c = (trc_cache *)p;
    if (!c) return;
    for (int i = 0; i < TRC_SLOTS; i++) pc_trc_free(c->s[i].trc);
    free(c);
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* The curve of a profile, NULL for sRGB or unusable profiles (owned). */
static pc_trc *curve_of(const uint8_t *icc, size_t len)
{
    pc_icc_info info;
    pc_trc *t = NULL;
    if (!icc || len == 0u) return NULL;
    if (pc_icc_inspect(icc, len, &info) != PC_OK) return NULL;
    if (info.is_srgb) return NULL;                 /* the built-in exact curve */
    if (pc_trc_new_icc(icc, len, &t) != PC_OK) return NULL;
    return t;
}

const pc_trc *app_doc_trc(app *a, const app_doc *d)
{
    trc_cache *c;
    trc_slot *s = NULL;
    uint64_t h;
    if (!a || !d || !d->meta.icc || d->meta.icc_len == 0u) return NULL;
    h = fnv(d->meta.icc, d->meta.icc_len);
    c = (trc_cache *)app_ext_get(a, "doc_trc");
    if (!c) {
        c = (trc_cache *)calloc(1u, sizeof *c);
        if (!c) return curve_of(NULL, 0u);
        if (!app_ext_set(a, "doc_trc", c, cache_free)) {
            free(c);
            return NULL;
        }
    }
    for (int i = 0; i < TRC_SLOTS; i++)
        if (c->s[i].used && c->s[i].doc_id == d->id && c->s[i].hash == h &&
            c->s[i].len == d->meta.icc_len)
            return c->s[i].trc;
    /* a new entry replaces this document's old one, else the last slot */
    for (int i = 0; i < TRC_SLOTS && !s; i++)
        if (c->s[i].used && c->s[i].doc_id == d->id) s = &c->s[i];
    for (int i = 0; i < TRC_SLOTS && !s; i++)
        if (!c->s[i].used) s = &c->s[i];
    if (!s) {
        pc_trc_free(c->s[TRC_SLOTS - 1].trc);
        memmove(&c->s[1], &c->s[0], (size_t)(TRC_SLOTS - 1) * sizeof c->s[0]);
        memset(&c->s[0], 0, sizeof c->s[0]);
        s = &c->s[0];
    } else {
        pc_trc_free(s->trc);
    }
    s->used = true;
    s->doc_id = d->id;
    s->hash = h;
    s->len = d->meta.icc_len;
    s->trc = curve_of(d->meta.icc, d->meta.icc_len);
    return s->trc;
}
