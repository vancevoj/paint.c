/* io_util.c - small helpers shared by the lane I files (io_internal.h):
 * metadata copies for workers, composite thumbnails, time, per-user data
 * folders and the raw thumbnail file format. */
#include "io_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pc_status io_meta_copy(const pc_image_meta *src, pc_image_meta *dst)
{
    memset(dst, 0, sizeof *dst);
    if (!src) return PC_OK;
    dst->dpi_x = src->dpi_x;
    dst->dpi_y = src->dpi_y;
    dst->src_bits = src->src_bits;
    dst->had_alpha = src->had_alpha;
    memcpy(dst->note, src->note, sizeof dst->note);
    if (src->icc && src->icc_len) {
        dst->icc = (uint8_t *)malloc(src->icc_len);
        if (!dst->icc) return PC_ERR_NOMEM;
        memcpy(dst->icc, src->icc, src->icc_len);
        dst->icc_len = src->icc_len;
    }
    for (size_t i = 0; i < src->n_items; i++)
        if (pc_meta_add(dst, src->items[i].key, src->items[i].value) != PC_OK) {
            pc_meta_free(dst);
            return PC_ERR_NOMEM;
        }
    return PC_OK;
}

/* One composited pixel: every visible layer at (x, y). */
static pc_px32 comp_px(const pc_doc *d, uint32_t x, uint32_t y)
{
    pc_px32 px;
    memset(&px, 0, sizeof px);
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        pc_px32 sp;
        if (!l->visible || l->opacity == 0u) continue;
        sp = pc_layer_get_px(l, x, y);
        pc_composite_span(&px, &sp, 1u, l->mode, l->opacity);
    }
    return px;
}

uint8_t *io_thumb_rgba(const pc_doc *d, int32_t max, int32_t *w, int32_t *h)
{
    int32_t tw, th;
    uint8_t *out;
    size_t bytes;
    if (!d || d->w == 0u || d->h == 0u || max < 1) return NULL;
    if (d->w >= d->h) {
        tw = d->w < (uint32_t)max ? (int32_t)d->w : max;
        th = (int32_t)((uint64_t)d->h * (uint64_t)tw / d->w);
    } else {
        th = d->h < (uint32_t)max ? (int32_t)d->h : max;
        tw = (int32_t)((uint64_t)d->w * (uint64_t)th / d->h);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (!pc_mul_size((size_t)tw * (size_t)th, 4u, &bytes)) return NULL;
    out = (uint8_t *)malloc(bytes);
    if (!out) return NULL;
    for (int32_t v = 0; v < th; v++) {
        for (int32_t u = 0; u < tw; u++) {
            uint32_t acc[4] = { 0, 0, 0, 0 };
            uint8_t *o = out + ((size_t)v * (size_t)tw + (size_t)u) * 4u;
            for (int s = 0; s < 4; s++) {
                double fx = ((double)u + 0.25 + 0.5 * (double)(s & 1)) / (double)tw;
                double fy = ((double)v + 0.25 + 0.5 * (double)(s >> 1)) / (double)th;
                uint32_t x = (uint32_t)(fx * (double)d->w), y = (uint32_t)(fy * (double)d->h);
                pc_px32 px;
                if (x >= d->w) x = d->w - 1u;
                if (y >= d->h) y = d->h - 1u;
                px = comp_px(d, x, y);
                acc[0] += (uint32_t)px.r * px.a;
                acc[1] += (uint32_t)px.g * px.a;
                acc[2] += (uint32_t)px.b * px.a;
                acc[3] += px.a;
            }
            if (acc[3]) {
                o[0] = (uint8_t)((acc[0] + acc[3] / 2u) / acc[3]);
                o[1] = (uint8_t)((acc[1] + acc[3] / 2u) / acc[3]);
                o[2] = (uint8_t)((acc[2] + acc[3] / 2u) / acc[3]);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            o[3] = (uint8_t)((acc[3] + 2u) / 4u);
        }
    }
    *w = tw;
    *h = th;
    return out;
}

int64_t io_unix_now(void)
{
    SDL_Time t = 0;
    if (!SDL_GetCurrentTime(&t)) return 0;
    return (int64_t)(t / SDL_NS_PER_SECOND);
}

bool io_user_dir(const app *a, pal_dir_kind kind, const char *sub, char *out, size_t cap)
{
    const char *cfg = a->opts.config_dir;
    if (!out || cap == 0u) return false;
    out[0] = '\0';
    if (cfg && !*cfg) return false;                     /* settings disabled: no files */
    if (cfg) {
        pal_path_join(out, cap, cfg, sub);
    } else {
        const char *base = pal_dir(kind);
        if (!base) return false;
        pal_path_join(out, cap, base, sub);
    }
    if (!pal_is_dir(out) && !pal_mkdirs(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

uint64_t io_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; s && *s; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001b3ull;
    }
    return h;
}

/* ---- raw thumbnail files: "PCTH", u16 w, u16 h (little endian), w*h*4 bytes ---- */
pc_status io_thumb_write(const char *path, const uint8_t *rgba, int32_t w, int32_t h)
{
    size_t px, n;
    uint8_t *buf;
    pc_status st;
    if (!path || !rgba || w < 1 || h < 1 || w > 4096 || h > 4096) return PC_ERR_ARG;
    if (!pc_mul_size((size_t)w * (size_t)h, 4u, &px) || !pc_add_size(px, 8u, &n))
        return PC_ERR_LIMIT;
    buf = (uint8_t *)malloc(n);
    if (!buf) return PC_ERR_NOMEM;
    memcpy(buf, "PCTH", 4u);
    buf[4] = (uint8_t)(w & 0xFF);
    buf[5] = (uint8_t)(w >> 8);
    buf[6] = (uint8_t)(h & 0xFF);
    buf[7] = (uint8_t)(h >> 8);
    memcpy(buf + 8, rgba, px);
    st = pal_write_file_atomic(path, buf, n);
    free(buf);
    return st;
}

uint8_t *io_thumb_read(const char *path, int32_t max, int32_t *w, int32_t *h)
{
    uint8_t *data = NULL, *out;
    size_t len = 0, px;
    int32_t tw, th;
    if (!path || max < 1 || max > 4096) return NULL;
    if (pal_read_file(path, 8u + (uint64_t)max * (uint64_t)max * 4u, &data, &len) != PC_OK)
        return NULL;
    if (len < 8u || memcmp(data, "PCTH", 4u) != 0) {
        free(data);
        return NULL;
    }
    tw = (int32_t)data[4] | ((int32_t)data[5] << 8);
    th = (int32_t)data[6] | ((int32_t)data[7] << 8);
    px = (size_t)tw * (size_t)th * 4u;
    if (tw < 1 || th < 1 || tw > max || th > max || len != 8u + px) {
        free(data);
        return NULL;
    }
    out = (uint8_t *)malloc(px);
    if (out) memcpy(out, data + 8, px);
    free(data);
    if (out) {
        *w = tw;
        *h = th;
    }
    return out;
}
