/* recent.c - File > Open Recent (MENUS.md File 3, F-MENU-FILE-RECENT*):
 * at most 10 files, newest first, each with a thumbnail and its full path
 * as tooltip; Clear List; a file that is gone shows an error and loses its
 * entry. Thumbnails are kept in memory and in the user's cache folder
 * (recent-thumbs/<hash>.thumb, raw RGBA written by the open and save
 * workers); the cached files are read by a worker on the first frame.
 * Textures exist only while the submenu is shown.
 *
 * Thread rules: main thread, except the preload worker, which only sees
 * its own job. */
#include "io_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECENT_KEY "lane_i.recent"

typedef struct recent_thumb {
    char        *path;          /* owned */
    uint8_t     *rgba;          /* owned, w x h */
    int32_t      w, h;
    SDL_Texture *tex;           /* while the submenu is open */
} recent_thumb;

typedef struct recent_state {
    recent_thumb *t;
    int32_t       n, cap;
    bool          preload_started;
    uint64_t      menu_frame;   /* last frame the submenu was drawn */
} recent_state;

typedef struct preload_job {
    char    *paths[APP_MAX_RECENT];
    char    *files[APP_MAX_RECENT];
    uint8_t *rgba[APP_MAX_RECENT];
    int32_t  w[APP_MAX_RECENT], h[APP_MAX_RECENT];
    int      n;
} preload_job;

static void thumb_clear(recent_thumb *t)
{
    if (t->tex) SDL_DestroyTexture(t->tex);
    free(t->rgba);
    free(t->path);
    memset(t, 0, sizeof *t);
}

static void state_free(void *p)
{
    recent_state *s = (recent_state *)p;
    if (!s) return;
    for (int32_t i = 0; i < s->n; i++) thumb_clear(&s->t[i]);
    free(s->t);
    free(s);
}

static recent_state *rstate(app *a)
{
    recent_state *s = (recent_state *)app_ext_get(a, RECENT_KEY);
    if (s) return s;
    s = (recent_state *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    if (!app_ext_set(a, RECENT_KEY, s, state_free)) {
        free(s);
        return NULL;
    }
    return s;
}

static recent_thumb *find_thumb(recent_state *s, const char *path)
{
    for (int32_t i = 0; i < s->n; i++)
        if (strcmp(s->t[i].path, path) == 0) return &s->t[i];
    return NULL;
}

bool io_recent_thumb_path(const app *a, const char *path, char *out, size_t cap)
{
    char dir[1024], name[40];
    if (!path || !io_user_dir(a, PAL_DIR_CACHE, "recent-thumbs", dir, sizeof dir)) return false;
    snprintf(name, sizeof name, "%016llx.thumb", (unsigned long long)io_hash(path));
    pal_path_join(out, cap, dir, name);
    return true;
}

void io_recent_thumb(app *a, const char *path, uint8_t *rgba, int32_t w, int32_t h)
{
    recent_state *s = rstate(a);
    recent_thumb *t;
    if (!s || !path) { free(rgba); return; }
    t = find_thumb(s, path);
    if (!rgba) {                                   /* forget it (memory and cache) */
        char file[1100];
        if (t) {
            thumb_clear(t);
            *t = s->t[--s->n];
        }
        if (io_recent_thumb_path(a, path, file, sizeof file)) (void)pal_remove(file);
        return;
    }
    if (!t) {
        if (s->n == s->cap) {
            int32_t nc = s->cap ? s->cap * 2 : 16;
            recent_thumb *nt = (recent_thumb *)realloc(s->t, (size_t)nc * sizeof *nt);
            if (!nt) { free(rgba); return; }
            s->t = nt;
            s->cap = nc;
        }
        t = &s->t[s->n];
        memset(t, 0, sizeof *t);
        t->path = app_strdup(path);
        if (!t->path) { free(rgba); return; }
        s->n++;
    }
    if (t->tex) SDL_DestroyTexture(t->tex);
    t->tex = NULL;
    free(t->rgba);
    t->rgba = rgba;
    t->w = w;
    t->h = h;
}

/* ---- preload of the cached thumbnails ----------------------------------------------- */
static void preload_work(void *ud)
{
    preload_job *j = (preload_job *)ud;
    for (int i = 0; i < j->n; i++)
        if (j->files[i]) j->rgba[i] = io_thumb_read(j->files[i], IO_THUMB_MAX, &j->w[i], &j->h[i]);
}

static void preload_done(app *a, void *ud)
{
    preload_job *j = (preload_job *)ud;
    recent_state *s = rstate(a);
    for (int i = 0; i < j->n; i++) {
        /* a newer thumbnail (opened meanwhile) wins */
        if (s && j->rgba[i] && !find_thumb(s, j->paths[i])) {
            io_recent_thumb(a, j->paths[i], j->rgba[i], j->w[i], j->h[i]);
            j->rgba[i] = NULL;
        }
        free(j->rgba[i]);
        free(j->paths[i]);
        free(j->files[i]);
    }
    free(j);
    app_request_frame(a);
}

static void preload(app *a, recent_state *s)
{
    preload_job *j;
    s->preload_started = true;
    if (a->nrecent == 0) return;
    j = (preload_job *)calloc(1u, sizeof *j);
    if (!j) return;
    for (int32_t i = 0; i < a->nrecent && j->n < APP_MAX_RECENT; i++) {
        char file[1100];
        if (!io_recent_thumb_path(a, a->recent[i], file, sizeof file)) continue;
        j->paths[j->n] = app_strdup(a->recent[i]);
        j->files[j->n] = app_strdup(file);
        if (!j->paths[j->n] || !j->files[j->n]) {
            free(j->paths[j->n]);
            free(j->files[j->n]);
            break;
        }
        j->n++;
    }
    if (j->n == 0 || !app_task(a, preload_work, preload_done, j)) {
        for (int i = 0; i < j->n; i++) {
            free(j->paths[i]);
            free(j->files[i]);
        }
        free(j);
    }
}

/* Every frame: start the preload once; drop textures when the submenu is
 * closed (they are recreated on demand, which also covers device resets). */
static void recent_frame(app *a, app_doc *d, void *ud)
{
    recent_state *s = rstate(a);
    (void)d;
    (void)ud;
    if (!s) return;
    if (!s->preload_started) preload(a, s);
    if (s->menu_frame + 2u < a->frame_no)
        for (int32_t i = 0; i < s->n; i++)
            if (s->t[i].tex) {
                SDL_DestroyTexture(s->t[i].tex);
                s->t[i].tex = NULL;
            }
}

void io_recent_init(app *a);
void io_recent_init(app *a)
{
    if (rstate(a)) (void)app_hook_add(a, APP_HOOK_FRAME, recent_frame, NULL);
}

/* ---- the list ------------------------------------------------------------------------ */
int app_recent_count(const app *a) { return (int)a->nrecent; }

const char *app_recent_at(const app *a, int i)
{
    return i >= 0 && i < (int)a->nrecent ? a->recent[i] : NULL;
}

void app_recent_clear(app *a)
{
    for (int32_t i = 0; i < a->nrecent; i++) {
        io_recent_thumb(a, a->recent[i], NULL, 0, 0);
        free(a->recent[i]);
    }
    a->nrecent = 0;
    app_recent_store(a);
    app_request_frame(a);
}

static void remove_entry(app *a, int i)
{
    io_recent_thumb(a, a->recent[i], NULL, 0, 0);
    free(a->recent[i]);
    memmove(&a->recent[i], &a->recent[i + 1], (size_t)(a->nrecent - i - 1) * sizeof *a->recent);
    a->nrecent--;
    app_recent_store(a);
}

bool app_recent_open(app *a, int i)
{
    char *p;
    bool ok;
    if (i < 0 || i >= (int)a->nrecent) return false;
    p = app_strdup(a->recent[i]);
    if (!p) return false;
    if (!pal_file_exists(p)) {                     /* F-MENU-FILE-RECENT-MISSING */
        remove_entry(a, i);
        app_error(a, "Could not open \"%s\": the file does not exist. It was removed from the "
                  "recent images.", p);
        free(p);
        return false;
    }
    ok = app_open_path(a, p);
    free(p);
    return ok;
}

/* ---- the submenu ------------------------------------------------------------------------ */
static void draw_thumb(app *a, recent_thumb *t, ui_rect row)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t slot = ui_px(ui, 26.0f), pad = ui_px(ui, 2.0f);
    ui_rect box = ui_rect_make(row.x + ui_px(ui, 4.0f), row.y + pad, slot, row.h - 2 * pad);
    float s;
    ui_rect img;
    if (!t || !t->rgba || t->w < 1 || t->h < 1) return;
    if (!t->tex) {
        t->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, t->w,
                                   t->h);
        if (!t->tex) return;
        SDL_SetTextureBlendMode(t->tex, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(t->tex, NULL, t->rgba, t->w * 4);
    }
    s = (float)box.w / (float)t->w;
    if ((float)box.h / (float)t->h < s) s = (float)box.h / (float)t->h;
    img = ui_rect_center(box, (int32_t)((float)t->w * s + 0.5f), (int32_t)((float)t->h * s + 0.5f));
    if (img.w < 1) img.w = 1;
    if (img.h < 1) img.h = 1;
    ui_draw_checker(ui, img, ui_px(ui, 3.0f), p->checker_a, p->checker_b);
    ui_draw_image(ui, t->tex, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
    ui_draw_rect_outline(ui, img, 1, p->border);
}

void app_recent_menu_items(app *a)
{
    ui_ctx *ui = a->ui;
    recent_state *s = rstate(a);
    int pick = -1;
    if (s) s->menu_frame = a->frame_no;
    if (a->nrecent == 0) {
        ui_menu_item(ui, "No recent images", NULL, false);
        return;
    }
    for (int32_t i = 0; i < a->nrecent; i++) {
        char label[320];
        snprintf(label, sizeof label, "%d   %s##recent", (int)(i + 1),
                 pal_path_basename(a->recent[i]));
        ui_push_id_int(ui, i);
        if (ui_menu_item(ui, label, NULL, true)) pick = (int)i;
        draw_thumb(a, s ? find_thumb(s, a->recent[i]) : NULL, ui_last_rect(ui));
        ui_tooltip(ui, a->recent[i]);                 /* F-MENU-FILE-RECENT-TOOLTIP */
        ui_pop_id(ui);
    }
    ui_menu_separator(ui);
    if (ui_menu_item(ui, "Clear List", NULL, true)) app_recent_clear(a);
    if (pick >= 0) (void)app_recent_open(a, pick);
}
