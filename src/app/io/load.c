/* load.c - lane CODEC (wave 4): one set of limits and one read-and-decode
 * path for every user-facing image load (File > Open, crash recovery, drop
 * as layers, Layers > Import From File, paste of a file, the Save
 * Configuration preview), and the error text that tells a file that is too
 * large to read apart from an image that is too large to decode (FL-BIG).
 *
 * Memory model: the file is read whole (codecs work on memory, pc_codec.h)
 * and stays in memory while it is decoded, so the file bytes and the
 * decoded pixels share one budget: three quarters of the physical RAM,
 * never less than the codec default of 4 GiB. A file may use the whole
 * budget; the decoder gets what is left. A 32768 x 32768 24-bit BMP (3 GiB
 * plus 54 bytes, 4 GiB of pixels) therefore opens on a computer with at
 * least about 9.4 GiB of RAM, and the message names the real reason
 * elsewhere.
 *
 * Thread rules: every function may run on any thread (workers decode);
 * app_load_test_budget is for tests and is set on the main thread before
 * loads start (atomic). Ownership: see app_io.h. */
#include "io_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test override of the budget in KiB (0 = automatic). */
static pc_atomic_u32 g_test_budget_kib;

void app_load_test_budget(uint64_t bytes)
{
    uint64_t kib = bytes / 1024u;
    pc_atomic_store(&g_test_budget_kib, kib > UINT32_MAX ? UINT32_MAX : (uint32_t)kib);
}

void app_load_limits_ram(uint64_t ram, pc_codec_limits *lim)
{
    uint64_t budget = ram / 4u * 3u;
    pc_codec_limits_default(lim);
    lim->max_pixels = (uint64_t)PC_MAX_DIM * PC_MAX_DIM;
    if (budget > lim->max_mem) lim->max_mem = budget;
}

void app_load_limits(pc_codec_limits *lim)
{
    uint32_t kib = pc_atomic_load(&g_test_budget_kib);
    app_load_limits_ram(pal_ram_bytes(), lim);
    if (kib) lim->max_mem = (uint64_t)kib * 1024u;
}

bool app_load_limits_after(pc_codec_limits *lim, uint64_t held)
{
    if (held > lim->max_mem) return false;
    lim->max_mem -= held;
    return true;
}

pc_status app_load_bytes(const uint8_t *p, size_t n, const char *path_hint, const pc_codec *only,
                         pc_doc **doc, pc_image_meta *meta, const pc_codec **used,
                         app_load_info *info)
{
    pc_codec_limits lim;
    app_load_info li;
    pc_status st;
    memset(&li, 0, sizeof li);
    if (doc) *doc = NULL;
    if (meta) memset(meta, 0, sizeof *meta);
    app_load_limits(&lim);
    li.max_file_bytes = lim.max_mem;
    li.file_bytes = n;
    if (!p || !doc || !meta) {
        st = PC_ERR_ARG;
    } else if (!app_load_limits_after(&lim, n)) {
        li.file_too_big = true;
        st = PC_ERR_LIMIT;
    } else if (only) {
        st = only->load ? only->load(p, n, &lim, doc, meta) : PC_ERR_UNSUPPORTED;
        if (st == PC_OK && used) *used = only;
    } else {
        st = pc_codec_load_any(p, n, path_hint, &lim, doc, meta, used);
    }
    li.max_mem = lim.max_mem;
    if (info) *info = li;
    return st;
}

pc_status app_load_file(const char *path, const pc_codec *only, pc_doc **doc,
                        pc_image_meta *meta, const pc_codec **used, app_load_info *info)
{
    pc_codec_limits lim;
    app_load_info li;
    SDL_PathInfo pi;
    uint8_t *data = NULL;
    size_t len = 0;
    pc_status st;
    memset(&li, 0, sizeof li);
    if (doc) *doc = NULL;
    if (meta) memset(meta, 0, sizeof *meta);
    app_load_limits(&lim);
    li.max_file_bytes = li.max_mem = lim.max_mem;
    if (!path || !doc || !meta) {
        st = PC_ERR_ARG;
        goto out;
    }
    if (!pal_file_exists(path)) {
        li.missing = true;
        st = PC_ERR_IO;
        goto out;
    }
    if (SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_FILE) li.file_bytes = pi.size;
    if (li.file_bytes > li.max_file_bytes) {            /* before reading anything (P-08) */
        li.file_too_big = true;
        st = PC_ERR_LIMIT;
        goto out;
    }
    st = pal_read_file(path, li.max_file_bytes, &data, &len);
    if (st == PC_ERR_LIMIT) li.file_too_big = true;     /* it grew meanwhile */
    if (st != PC_OK) goto out;
    st = app_load_bytes(data, len, path, only, doc, meta, used, &li);
    free(data);
out:
    if (info) *info = li;
    return st;
}

/* "512 bytes", "12.3 MB", "3.0 GB" (binary units, like the Save
 * Configuration dialog). */
static void size_text(uint64_t bytes, char *out, size_t cap)
{
    double b = (double)bytes;
    if (b < 1024.0) snprintf(out, cap, "%.0f bytes", b);
    else if (b < 1024.0 * 1024.0) snprintf(out, cap, "%.1f KB", b / 1024.0);
    else if (b < 1024.0 * 1024.0 * 1024.0) snprintf(out, cap, "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(out, cap, "%.1f GB", b / (1024.0 * 1024.0 * 1024.0));
}

void app_load_error_text(char *out, size_t cap, const char *verb, const char *name,
                         pc_status st, const app_load_info *info)
{
    char a[32], b[32];
    if (!out || cap == 0u) return;
    if (!verb) verb = "open";
    if (!name) name = "";
    if (info && info->missing) {
        snprintf(out, cap, "Could not %s \"%s\": the file does not exist.", verb, name);
    } else if (st == PC_ERR_LIMIT && info && info->file_too_big) {
        size_text(info->file_bytes, a, sizeof a);
        size_text(info->max_file_bytes, b, sizeof b);
        if (info->file_bytes)
            snprintf(out, cap, "Could not %s \"%s\": the file is %s, more than the %s that "
                               "paint.c can read into memory on this computer.",
                     verb, name, a, b);
        else
            snprintf(out, cap, "Could not %s \"%s\": the file is larger than the %s that "
                               "paint.c can read into memory on this computer.",
                     verb, name, b);
    } else if (st == PC_ERR_LIMIT && info) {
        pc_codec_limits lim;
        pc_codec_limits_default(&lim);
        size_text(info->max_mem, a, sizeof a);
        snprintf(out, cap, "Could not %s \"%s\": the image is too large. paint.c opens images "
                           "of up to %u x %u pixels and %u layers whose pixels fit in the %s "
                           "of memory left after reading the file.",
                 verb, name, (unsigned)PC_MAX_DIM, (unsigned)PC_MAX_DIM,
                 (unsigned)lim.max_layers, a);
    } else {
        snprintf(out, cap, "Could not %s \"%s\": %s.", verb, name, pc_status_str(st));
    }
}
