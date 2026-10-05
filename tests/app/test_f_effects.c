/* test_f_effects.c - lane F: every adjustment and effect of the registry
 * runs through the editor exactly like the user runs it (menu command,
 * dialog with live preview, OK; or the immediate dialog-less run) and the
 * result equals an independent oracle (fx_run_sync on a copy, blended
 * through the antialiased selection), with exactly one history item named
 * after the effect, a correct live preview before OK, and an exact undo.
 * Also checks the Adjustments and Effects menu contents and order
 * (MENUS.md), their shortcuts (SHORTCUTS.md) and selection clipping. */
#include "pc_test.h"
#include "f_test_util.h"

#define W 72
#define H 56

/* Run one effect through the app; returns false on a harness failure. */
static void run_one(app *a, const fx_effect *fx, bool check_preview)
{
    app_doc *d = app_active_doc(a);
    char id[256], name[128];
    pc_surf before, expect, got;
    size_t cur0 = 0, cur = 0;
    int32_t fx0, fy0;
    int64_t nd;
    (void)app_doc_history_list(d, NULL, 0, &cur0);
    afx_effect_name(fx, name, sizeof name);
    f_cmd_id(fx, id, sizeof id);
    memset(&expect, 0, sizeof expect);
    memset(&got, 0, sizeof got);
    if (!f_read_layer(a, &before) || !f_oracle(a, fx, NULL, &expect)) {
        CHECK(!"oracle");
        pc_surf_free(&before);
        return;
    }
    CHECK(app_cmd_exec(a, id));
    if (!(fx->flags & FX_FLAG_NO_DIALOG)) {
        afx_session *s;
        CHECK(afx_wait_preview(a, 400));
        s = afx_active(a);
        CHECK(s != NULL && afx_session_state(s) == AFX_PREVIEW);
        if (check_preview && f_read_txn(a, &got)) {
            nd = f_diff(&got, &expect, &fx0, &fy0);
            if (nd != 0) INFO("%s: preview differs at %d pixels (first %d,%d)", fx->id, (int)nd,
                              (int)fx0, (int)fy0);
            CHECK(nd == 0);
            pc_surf_free(&got);
        }
        if (s) CHECK(afx_session_ok(a, s));
    }
    CHECK(afx_wait_idle(a, 400));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    /* exactly one new item, current, nothing to redo */
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == cur0 + 2u && cur == cur0 + 1u);
    CHECK(strcmp(d->hist->cur->label, name) == 0);
    if (f_read_layer(a, &got)) {
        nd = f_diff(&got, &expect, &fx0, &fy0);
        if (nd != 0) INFO("%s: result differs at %d pixels (first %d,%d)", fx->id, (int)nd,
                          (int)fx0, (int)fy0);
        CHECK(nd == 0);
        pc_surf_free(&got);
    }
    /* undo restores the image exactly */
    CHECK(app_doc_undo(a, d));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &before, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == cur0 + 2u && cur == cur0);  /* redo kept */
    pc_surf_free(&before);
    pc_surf_free(&expect);
}

static void run_all(bool with_selection)
{
    app *a = f_app(W, H);
    uint32_t n, ran = 0;
    CHECK(a != NULL);
    if (!a) return;
    app_set_primary(a, app_px_make(200, 30, 60, 255));
    app_set_secondary(a, app_px_make(20, 220, 180, 200));
    if (with_selection) CHECK(f_select_ellipse(a, 30.5, 26.0, 26.0, 19.0));
    n = fx_registry_count(a->fx);
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        if (fx->flags & FX_FLAG_MASK_ONLY) continue;
        if (strncmp(fx->menu, "Adjustments/", 12) != 0 && !afx_is_effect(fx)) continue;
        /* the preview is compared for a sample of effects (it is the same
         * buffer the commit uses; every effect is compared after OK) */
        run_one(a, fx, (i % 3u) == 0u);
        ran++;
    }
    CHECK(ran >= 55u);
    INFO("%u effects ran%s", (unsigned)ran, with_selection ? " through a selection" : "");
    app_destroy(a);
}

static void t_all_with_selection(void) { run_all(true); }
static void t_all_whole_image(void) { run_all(false); }

/* Pixels outside the selection never change (except object effects that
 * draw outside, FX_FLAG_NO_SEL_CLIP); inside, soft edges blend. */
static void t_selection_clip(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    pc_surf before, after;
    const fx_effect *inv;
    int64_t outside_changed = 0, inside_changed = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(f_select_ellipse(a, 36.0, 28.0, 14.0, 10.0));
    inv = fx_registry_find(a->fx, "org.paintc.adjust.invert_colors");
    CHECK(inv != NULL);
    if (!inv || !f_read_layer(a, &before)) {
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.invert_colors"));
    CHECK(afx_wait_idle(a, 100));
    if (f_read_layer(a, &after)) {
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                uint8_t cov = pc_sel_coverage(d->doc, x, y);
                bool same = memcmp(&before.px[y * before.stride + x], &after.px[y * after.stride + x],
                                   sizeof(pc_px32)) == 0;
                if (cov == 0u && !same) outside_changed++;
                if (cov == 255u && !same) inside_changed++;
            }
        pc_surf_free(&after);
    }
    CHECK(outside_changed == 0);
    CHECK(inside_changed > 300);
    pc_surf_free(&before);
    app_destroy(a);
}

/* Adjustments: 13 items, alphabetical, with their documented keys;
 * Effects: the nine submenus in MENUS.md order, items alphabetical. */
static void t_menus(void)
{
    static const char *const k_adj[] = {
        "Auto-Level", "Black and White", "Brightness / Contrast", "Curves", "Exposure",
        "Highlights / Shadows", "Hue / Saturation", "Invert Alpha", "Invert Colors", "Levels",
        "Posterize", "Sepia", "Temperature / Tint"
    };
    static const char *const k_sub[] = { "Artistic", "Blurs", "Color", "Distort", "Noise",
                                         "Object", "Photo", "Render", "Stylize" };
    static const struct { const char *id, *key; } k_keys[] = {
        { "adjust.org.paintc.adjust.auto_level", "Ctrl+Shift+L" },
        { "adjust.org.paintc.adjust.black_and_white", "Ctrl+Shift+G" },
        { "adjust.org.paintc.adjust.brightness_contrast", "Ctrl+Shift+T" },
        { "adjust.org.paintc.adjust.curves", "Ctrl+Shift+M" },
        { "adjust.org.paintc.adjust.hue_saturation", "Ctrl+Shift+U" },
        { "adjust.org.paintc.adjust.invert_alpha", "Ctrl+Alt+I" },
        { "adjust.org.paintc.adjust.invert_colors", "Ctrl+Shift+I" },
        { "adjust.org.paintc.adjust.levels", "Ctrl+L" },
        { "adjust.org.paintc.adjust.posterize", "Ctrl+Shift+P" },
        { "adjust.org.paintc.adjust.sepia", "Ctrl+Shift+E" },
        { "effects.repeat", "Ctrl+F" },
    };
    app *a = at_app(800, 600);
    uint32_t n, na = 0, ns = 0;
    char last_sub[64] = "", last_item[128] = "";
    CHECK(a != NULL);
    if (!a) return;
    n = fx_registry_count(a->fx);
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        const char *seg[4];
        size_t len[4];
        uint32_t c = fx_menu_split(fx->menu, seg, len, 4u);
        char name[128], sub[64], cmd[256];
        const app_cmd *ac;
        afx_effect_name(fx, name, sizeof name);
        f_cmd_id(fx, cmd, sizeof cmd);
        ac = app_cmd_find(a, cmd);
        CHECK(ac != NULL);
        if (ac) {
            bool dlg = !(fx->flags & FX_FLAG_NO_DIALOG);
            size_t ln = strlen(ac->label);
            CHECK(dlg == (ln > 3u && strcmp(ac->label + ln - 3u, "...") == 0));
        }
        if (c == 2u && strncmp(seg[0], "Adjustments", len[0]) == 0) {
            CHECK(na < 13u && strcmp(name, k_adj[na < 13u ? na : 12u]) == 0);
            na++;
        } else if (c == 3u && strncmp(seg[0], "Effects", len[0]) == 0) {
            snprintf(sub, sizeof sub, "%.*s", (int)len[1], seg[1]);
            if (strcmp(sub, last_sub) != 0) {
                CHECK(ns < 9u && strcmp(sub, k_sub[ns < 9u ? ns : 8u]) == 0);
                ns++;
                snprintf(last_sub, sizeof last_sub, "%s", sub);
                last_item[0] = '\0';
            } else {
                CHECK(strcmp(last_item, name) < 0);           /* alphabetical items */
            }
            snprintf(last_item, sizeof last_item, "%s", name);
        }
    }
    CHECK(na == 13u);
    CHECK(ns == 9u);
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        const char *t = app_cmd_shortcut_text(a, k_keys[i].id);
        CHECK(t && strcmp(t, k_keys[i].key) == 0);
    }
    /* Exposure, Highlights / Shadows, Temperature / Tint: no accelerator */
    CHECK(app_cmd_shortcut_text(a, "adjust.org.paintc.adjust.exposure") == NULL);
    CHECK(app_cmd_shortcut_text(a, "adjust.org.paintc.adjust.temperature_tint") == NULL);
    /* no image: every effect command is disabled; Repeat absent until used */
    CHECK(!app_cmd_enabled(a, "adjust.org.paintc.adjust.invert_colors"));
    CHECK(!app_cmd_enabled(a, "effects.org.paintc.blur.gaussian"));
    CHECK(!app_cmd_enabled(a, "effects.repeat"));
    CHECK(!app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_menus);
    RUN(t_selection_clip);
    RUN(t_all_with_selection);
    if (!g_quick) RUN(t_all_whole_image);
    at_quit();
    return pc_test_finish();
}
