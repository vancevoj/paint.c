/* panels.c - floating panel registry: declaration each frame, toggles
 * ("window.<id>"), resets ("window.reset.<id>", "window.reset_all") and
 * persistence of their rectangles plus the Colors window state (palette,
 * More / Less) in the settings (app_ui.h, lane P). Main thread. */
#include "app_internal.h"
#include "shell_ext.h"
#include "panels/pnl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_panel *find(const app *a, const char *id)
{
    for (int32_t i = 0; i < a->npanels; i++)
        if (strcmp(a->panels[i].id, id) == 0) return &a->panels[i];
    return NULL;
}

static bool toggle_checked(app *a, const app_cmd *c) { return app_panel_open(a, c->id + 7); }
static void toggle_run(app *a, const app_cmd *c) { app_panel_toggle(a, c->id + 7); }
static void reset_run(app *a, const app_cmd *c) { app_panel_reset(a, c->id + 13); }

bool app_panel_register(app *a, const app_panel_def *def)
{
    app_panel *p;
    char id[160];
    app_cmd_def cd;
    if (!def || !def->id || !def->title || !def->body || find(a, def->id)) return false;
    if (a->npanels == a->cap_panels) {
        int32_t nc = a->cap_panels ? a->cap_panels * 2 : 8;
        app_panel *n = (app_panel *)realloc(a->panels, (size_t)nc * sizeof *n);
        if (!n) return false;
        a->panels = n;
        a->cap_panels = nc;
    }
    p = &a->panels[a->npanels];
    memset(p, 0, sizeof *p);
    p->id = app_strdup(def->id);
    p->title = app_strdup(def->title);
    if (!p->id || !p->title) {
        free(p->id);
        free(p->title);
        return false;
    }
    p->def = *def;
    p->def.id = p->id;
    p->def.title = p->title;
    p->st = def->def;
    p->uia_auto = true;                                   /* lane UIA */
    a->npanels++;
    /* commands: window.<id> toggles, window.reset.<id> restores the default */
    memset(&cd, 0, sizeof cd);
    snprintf(id, sizeof id, "window.%s", def->id);
    cd.id = id;
    cd.label = def->title;
    cd.icon = def->icon;
    cd.flags = APP_CMD_NO_COMMIT;
    cd.checked = toggle_checked;
    cd.run = toggle_run;
    (void)app_cmd_register(a, &cd);
    snprintf(id, sizeof id, "window.reset.%s", def->id);
    cd.label = "Reset Window";
    cd.checked = NULL;
    cd.run = reset_run;
    (void)app_cmd_register(a, &cd);
    return true;
}

ui_panel_state *app_panel_state(app *a, const char *id)
{
    app_panel *p = find(a, id);
    return p ? &p->st : NULL;
}

bool app_panel_open(const app *a, const char *id)
{
    const app_panel *p = find(a, id);
    return p && p->st.open;
}

void app_panel_toggle(app *a, const char *id)
{
    app_panel *p = find(a, id);
    if (!p) return;
    p->st.open = !p->st.open;
    app_request_frame(a);
}

void app_panel_reset(app *a, const char *id)
{
    app_panel *p = find(a, id);
    if (!p) return;
    p->st = p->def.def;
    p->st.open = true;
    p->uia_auto = true;                                   /* lane UIA */
    pnl_colors_frame(a);
    app_request_frame(a);
}

void app_panels_reset_all(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        a->panels[i].st = a->panels[i].def.def;
        a->panels[i].st.open = true;
        a->panels[i].uia_auto = true;                     /* lane UIA */
    }
    pnl_colors_frame(a);          /* the Colors window keeps its mode's size */
    app_request_frame(a);
}

/* ---- lane UIA (wave 4): the default layout ------------------------------------------
 * The four standard windows keep Paint.NET's corners (Tools top left,
 * Colors bottom left, History top right, Layers bottom right; 3.36 parks
 * each floater in its corner, AppWorkspace.ResetFloatingForm) while they
 * are where the layout put them. In a workspace too short for a column,
 * paint.c (its windows live inside the main window, unlike 3.36's owned
 * forms) avoids overlaps: Tools gets more columns, Colors moves beside
 * Tools, History and Layers share the height (down to a minimum) or sit
 * side by side, and the right column narrows before it covers the left
 * one. Sizes are DIPs. */
#define LAY_M        8.0f       /* margin to the workspace and between windows */
#define LAY_CELL     30.0f      /* a Tools button: 28 DIPs and 2 of spacing (pnl_tools.c) */
#define LAY_TOOLS    19.0f      /* buttons in the Tools window */
#define LAY_MIN_HIST 120.0f
#define LAY_MIN_LAY  150.0f
#define LAY_MIN_W    180.0f

typedef struct lay_slot {
    app_panel     *p;
    ui_panel_state st;          /* the placed state */
} lay_slot;

static void lay_set(lay_slot *l, float x, float y, float w, float h, int ax, int ay)
{
    l->st.x = x;
    l->st.y = y;
    l->st.w = w;
    l->st.h = h;
    l->st.anchor_x = (uint8_t)ax;
    l->st.anchor_y = (uint8_t)ay;
}

static void lay_pick(app *a, lay_slot *l, const char *id)
{
    app_panel *p = find(a, id);
    memset(l, 0, sizeof *l);
    if (p && p->uia_auto && p->st.open) {
        l->p = p;
        l->st = p->st;
    }
}

/* Lay out the standard windows that are still auto placed in a W x H DIP
 * workspace; out receives a placed state per panel index (set[i]). */
static void default_layout(app *a, float W, float H, ui_panel_state *out, bool *set)
{
    lay_slot t, c, h, l;
    float left = 0.0f, M = LAY_M;
    lay_pick(a, &t, "tools");
    lay_pick(a, &c, "colors");
    lay_pick(a, &h, "history");
    lay_pick(a, &l, "layers");
    if (t.p) {
        float tw = t.p->def.def.w, th = t.p->def.def.h, chrome = th - 10.0f * LAY_CELL;
        float cw = c.p ? c.p->st.w : 0.0f, ch = c.p ? c.p->st.h : 0.0f;
        bool stacked = !c.p || th + ch + 3.0f * M <= H;
        /* more columns and fewer rows: first so that Colors still fits
         * below (Tools no wider than Colors), else so that Tools fits */
        for (float cols = 3.0f; !stacked && cols <= 8.0f; cols += 1.0f) {
            float w2 = tw + (cols - 2.0f) * LAY_CELL;
            float h2 = chrome + ceilf(LAY_TOOLS / cols) * LAY_CELL;
            if (w2 > cw) break;
            if (h2 + ch + 3.0f * M <= H) {
                tw = w2;
                th = h2;
                stacked = true;
            }
        }
        if (th + 2.0f * M > H) {
            float rows = floorf((H - 2.0f * M - chrome) / LAY_CELL), cols;
            if (rows < 1.0f) rows = 1.0f;
            cols = ceilf(LAY_TOOLS / rows);
            if (cols < 2.0f) cols = 2.0f;
            if (cols > 8.0f) cols = 8.0f;
            tw = t.p->def.def.w + (cols - 2.0f) * LAY_CELL;
            th = chrome + ceilf(LAY_TOOLS / cols) * LAY_CELL;
        }
        lay_set(&t, M, M, tw, th, UI_ANCHOR_START, UI_ANCHOR_START);
        left = M + tw;
    }
    if (c.p) {
        float cw = c.p->st.w, ch = c.p->st.h;     /* the size of the current mode */
        if (!t.p || t.st.h + ch + 3.0f * M <= H) {
            lay_set(&c, M, M, cw, ch, UI_ANCHOR_START, UI_ANCHOR_END);
        } else {
            lay_set(&c, left + M, M, cw, ch, UI_ANCHOR_START, UI_ANCHOR_END);
        }
        if (c.st.x + cw > left) left = c.st.x + cw;
    }
    {
        float hw = h.p ? h.p->def.def.w : 0.0f, hh = h.p ? h.p->def.def.h : 0.0f;
        float lw = l.p ? l.p->def.def.w : 0.0f, lh = l.p ? l.p->def.def.h : 0.0f;
        float room = W - left - 2.0f * M;          /* width right of the left column */
        bool side = false;
        if (h.p && l.p && hh + lh + 3.0f * M > H) {
            float avail = H - 3.0f * M;
            float h2 = floorf(avail * hh / (hh + lh)), l2;
            if (h2 < LAY_MIN_HIST) h2 = LAY_MIN_HIST;
            l2 = avail - h2;
            if (l2 < LAY_MIN_LAY) {
                l2 = LAY_MIN_LAY;
                h2 = avail - l2 < LAY_MIN_HIST ? LAY_MIN_HIST : avail - l2;
            }
            /* too short for both: side by side, narrowed (not below
             * LAY_MIN_W) when the room needs it */
            if (h2 + l2 > avail && 2.0f * LAY_MIN_W + M <= room) side = true;
            hh = h2;
            lh = l2;
        }
        if (side) {
            if (hw + lw + M > room) {
                float k = (room - M) / (hw + lw);
                hw = floorf(hw * k);
                lw = floorf(lw * k);
                if (hw < LAY_MIN_W) hw = LAY_MIN_W;
                if (lw < LAY_MIN_W) lw = LAY_MIN_W;
            }
            hh = h.p->def.def.h < H - 2.0f * M ? h.p->def.def.h : H - 2.0f * M;
            lh = l.p->def.def.h < H - 2.0f * M ? l.p->def.def.h : H - 2.0f * M;
            lay_set(&l, M, M, lw, lh, UI_ANCHOR_END, UI_ANCHOR_END);
            lay_set(&h, 2.0f * M + lw, M, hw, hh, UI_ANCHOR_END, UI_ANCHOR_START);
        } else {
            /* one column; narrower before it covers the left column */
            if (h.p && hw > room) hw = room < LAY_MIN_W ? LAY_MIN_W : room;
            if (l.p && lw > room) lw = room < LAY_MIN_W ? LAY_MIN_W : room;
            if (h.p && !l.p && hh + 2.0f * M > H) hh = H - 2.0f * M;
            if (l.p && !h.p && lh + 2.0f * M > H) lh = H - 2.0f * M;
            if (h.p) lay_set(&h, M, M, hw, hh, UI_ANCHOR_END, UI_ANCHOR_START);
            if (l.p) lay_set(&l, M, M, lw, lh, UI_ANCHOR_END, UI_ANCHOR_END);
        }
    }
    {
        lay_slot *all[4];
        all[0] = &t;
        all[1] = &c;
        all[2] = &h;
        all[3] = &l;
        for (int k = 0; k < 4; k++) {
            int32_t i;
            if (!all[k]->p) continue;
            i = (int32_t)(all[k]->p - a->panels);
            if (all[k]->st.w < 60.0f) all[k]->st.w = 60.0f;
            if (all[k]->st.h < 40.0f) all[k]->st.h = 40.0f;
            all[k]->st.open = true;
            out[i] = all[k]->st;
            set[i] = true;
        }
    }
}

static bool same_place(const ui_panel_state *x, const ui_panel_state *y)
{
    return x->x == y->x && x->y == y->y && x->w == y->w && x->h == y->h &&
           x->anchor_x == y->anchor_x && x->anchor_y == y->anchor_y;
}

void app_panels_frame(app *a)
{
    enum { NMAX = 32 };
    ui_panel_state placed[NMAX];
    bool set[NMAX];
    float s = ui_scale(a->ui) > 0.0f ? ui_scale(a->ui) : 1.0f;
    memset(set, 0, sizeof set);
    if (a->npanels <= NMAX && a->r_work.w > 0 && a->r_work.h > 0)
        default_layout(a, (float)a->r_work.w / s, (float)a->r_work.h / s, placed, set);
    for (int32_t i = 0; i < a->npanels; i++) {
        app_panel *p = &a->panels[i];
        ui_rect pr = ui_panel_rect(a->ui, p->title);    /* lane SHELL: as shown last frame */
        bool lay = i < NMAX && set[i] && p->uia_auto;
        ui_panel_state shown = lay ? placed[i] : p->st, before = shown;
        if (ui_panel_begin(a->ui, p->title, &shown, p->def.flags)) {
            /* lane SHELL: translucent utility windows */
            ui_panel_set_alpha(a->ui, app_panel_alpha(a, i, pr, ui_panel_held(a->ui)));
            p->def.body(a, p->def.ud);
            ui_panel_end(a->ui);
        }
        /* lane UIA: a move or resize by the user ends the auto placement */
        if (!lay) p->st = shown;
        else if (!same_place(&shown, &before)) {
            p->st = shown;
            p->uia_auto = false;
        } else {
            p->st.open = shown.open;
        }
    }
}

void app_panels_free(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        free(a->panels[i].id);
        free(a->panels[i].title);
    }
    free(a->panels);
    a->panels = NULL;
    a->npanels = a->cap_panels = 0;
}

/* "panel.<id>" = "x,y,w,h,anchor_x,anchor_y,open" in DIPs. */
void app_panels_load(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        app_panel *p = &a->panels[i];
        char key[160];
        const char *v;
        double x, y, w, h;
        int ax, ay, open;
        snprintf(key, sizeof key, "panel.%s", p->id);
        v = app_settings_get(a->settings, key);
        if (!v) continue;
        /* the settings writer uses '.' decimals; sscanf %lf follows the C
         * locale, which the app never changes from "C" */
        {
            /* lane UIA: an 8th field marks the default layout; files
             * without it count as default when they hold the default */
            int au = -1, nf = sscanf(v, "%lf,%lf,%lf,%lf,%d,%d,%d,%d", &x, &y, &w, &h, &ax, &ay,
                                     &open, &au);
            if (nf < 7) continue;
            if (nf == 8) p->uia_auto = au != 0;
            else
                p->uia_auto = (float)x == p->def.def.x && (float)y == p->def.def.y &&
                              (!(p->def.flags & UI_PANEL_RESIZABLE) ||
                               ((float)w == p->def.def.w && (float)h == p->def.def.h)) &&
                              (ax ? UI_ANCHOR_END : UI_ANCHOR_START) == p->def.def.anchor_x &&
                              (ay ? UI_ANCHOR_END : UI_ANCHOR_START) == p->def.def.anchor_y;
        }
        if (!(w >= 60.0 && w <= 4000.0 && h >= 40.0 && h <= 4000.0)) continue;
        if (!(x >= -100.0 && x <= 8000.0 && y >= -100.0 && y <= 8000.0)) continue;
        p->st.x = (float)x;
        p->st.y = (float)y;
        p->st.w = (float)w;
        p->st.h = (float)h;
        p->st.anchor_x = (uint8_t)(ax ? UI_ANCHOR_END : UI_ANCHOR_START);
        p->st.anchor_y = (uint8_t)(ay ? UI_ANCHOR_END : UI_ANCHOR_START);
        p->st.open = open != 0;
    }
    pnl_colors_load(a);
}

void app_panels_store(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        const app_panel *p = &a->panels[i];
        char key[160], val[160];
        snprintf(key, sizeof key, "panel.%s", p->id);
        snprintf(val, sizeof val, "%d,%d,%d,%d,%d,%d,%d,%d", (int)p->st.x, (int)p->st.y,
                 (int)p->st.w, (int)p->st.h, (int)p->st.anchor_x, (int)p->st.anchor_y,
                 p->st.open ? 1 : 0, p->uia_auto ? 1 : 0);    /* lane UIA: 8th field */
        app_settings_set(a->settings, key, val);
    }
    pnl_colors_store(a);
}
