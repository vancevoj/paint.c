/* panels.c - floating panel registry: declaration each frame, toggles
 * ("window.<id>"), resets ("window.reset.<id>", "window.reset_all") and
 * persistence of their rectangles plus the Colors window state (palette,
 * More / Less) in the settings (app_ui.h, lane P). Main thread. */
#include "app_internal.h"
#include "panels/pnl.h"

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
    pnl_colors_frame(a);
    app_request_frame(a);
}

void app_panels_reset_all(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        a->panels[i].st = a->panels[i].def.def;
        a->panels[i].st.open = true;
    }
    pnl_colors_frame(a);          /* the Colors window keeps its mode's size */
    app_request_frame(a);
}

void app_panels_frame(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) {
        app_panel *p = &a->panels[i];
        if (ui_panel_begin(a->ui, p->title, &p->st, p->def.flags)) {
            p->def.body(a, p->def.ud);
            ui_panel_end(a->ui);
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
        if (sscanf(v, "%lf,%lf,%lf,%lf,%d,%d,%d", &x, &y, &w, &h, &ax, &ay, &open) != 7) continue;
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
        snprintf(val, sizeof val, "%d,%d,%d,%d,%d,%d,%d", (int)p->st.x, (int)p->st.y,
                 (int)p->st.w, (int)p->st.h, (int)p->st.anchor_x, (int)p->st.anchor_y,
                 p->st.open ? 1 : 0);
        app_settings_set(a->settings, key, val);
    }
    pnl_colors_store(a);
}
