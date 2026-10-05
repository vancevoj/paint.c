/* test_fxc_rules.c - lane W3B-FXCORE: parameter schema parity of the effect
 * dialogs.
 *  - property rules (fx_run.h): "link:" groups (Posterize levels, Morphology
 *    width and height) with the last-edited member winning, and the soft
 *    "minmax:" pair of Frosted Glass, in both directions, plus validation of
 *    malformed rule hints;
 *  - the set of non-linear radius sliders (FXP_F_SLIDER_LOG) is exactly the
 *    one measured on the Paint.NET 5.2 dialogs (O-UI-NONLIN);
 *  - Drop Shadow's Color is RGB only (FXP_F_COLOR_NO_ALPHA) and the effect
 *    draws outside the selection (FX_FLAG_NO_SEL_CLIP).
 */
#include "pc_test.h"
#include "fx/fx_abi_ext.h"
#include "fx/fx_run.h"

#include <math.h>

static fx_registry *g_reg;

static const fx_effect *fx_of(const char *suffix)
{
    char id[128];
    snprintf(id, sizeof id, "org.paintc.%s", suffix);
    return fx_registry_find(g_reg, id);
}

static uint32_t idx_of(const fx_effect *fx, const char *key)
{
    const fx_prop *p = fx_prop_find(fx, key);
    return p ? (uint32_t)(p - fx->props) : FX_RULE_NONE;
}

static double get(const fx_effect *fx, const void *params, const char *key)
{
    double v = -12345.0;
    CHECK(fx_param_get(fx, params, key, &v) == PC_OK);
    return v;
}

/* Sets key and runs the rules as the dialog does after an edit. */
static void edit(const fx_effect *fx, void *params, const char *key, double v, uint32_t *last)
{
    CHECK(fx_param_set(fx, params, key, v) == PC_OK);
    (void)fx_props_rules(fx->props, fx->n_props, params, idx_of(fx, key), last);
}

static void t_link_posterize(void)
{
    const fx_effect *fx = fx_of("adjust.posterize");
    uint32_t last[FX_MAX_PROPS];
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    memset(last, 0, sizeof last);
    p = fx_params_new(fx, NULL);
    CHECK(get(fx, p, "linked") == 1.0);
    /* every level is linked by the Linked check box; the check boxes are not */
    CHECK(fx_prop_link_source(fx->props, fx->n_props, idx_of(fx, "red")) == idx_of(fx, "linked"));
    CHECK(fx_prop_link_source(fx->props, fx->n_props, idx_of(fx, "alpha")) ==
          idx_of(fx, "linked"));
    CHECK(fx_prop_link_source(fx->props, fx->n_props, idx_of(fx, "red_on")) == FX_RULE_NONE);
    CHECK(fx_prop_link_source(fx->props, fx->n_props, idx_of(fx, "linked")) == FX_RULE_NONE);
    /* linked: an edit of any member moves all four (O52) */
    edit(fx, p, "blue", 5.0, last);
    CHECK(get(fx, p, "red") == 5.0 && get(fx, p, "green") == 5.0);
    CHECK(get(fx, p, "blue") == 5.0 && get(fx, p, "alpha") == 5.0);
    edit(fx, p, "alpha", 40.0, last);
    CHECK(get(fx, p, "red") == 40.0 && get(fx, p, "green") == 40.0 && get(fx, p, "blue") == 40.0);
    /* clamped values propagate clamped */
    edit(fx, p, "red", 99.0, last);
    CHECK(get(fx, p, "red") == 64.0 && get(fx, p, "alpha") == 64.0);
    /* unlinked: members are independent and keep their values */
    edit(fx, p, "linked", 0.0, last);
    edit(fx, p, "green", 9.0, last);
    edit(fx, p, "red", 3.0, last);
    CHECK(get(fx, p, "green") == 9.0 && get(fx, p, "red") == 3.0 && get(fx, p, "blue") == 64.0);
    edit(fx, p, "blue", 20.0, last);
    /* linking again syncs to the member edited last (3.36 rule semantics) */
    edit(fx, p, "linked", 1.0, last);
    CHECK(get(fx, p, "red") == 20.0 && get(fx, p, "green") == 20.0);
    CHECK(get(fx, p, "blue") == 20.0 && get(fx, p, "alpha") == 20.0);
    /* without caller state (scripts, presets): the first member wins */
    CHECK(fx_param_set(fx, p, "red", 7.0) == PC_OK);
    CHECK(fx_param_set(fx, p, "green", 11.0) == PC_OK);
    CHECK(fx_params_apply_rules(fx, p) == 3u);
    CHECK(get(fx, p, "green") == 7.0 && get(fx, p, "alpha") == 7.0);
    CHECK(fx_params_apply_rules(fx, p) == 0u);          /* idempotent */
    /* the check boxes are untouched by the rules */
    CHECK(get(fx, p, "red_on") == 1.0 && get(fx, p, "alpha_on") == 1.0);
    fx_params_free(p);
}

static void t_link_morphology(void)
{
    const fx_effect *fx = fx_of("distort.morphology");
    uint32_t last[FX_MAX_PROPS];
    const fx_prop *h;
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    memset(last, 0, sizeof last);
    h = fx_prop_find(fx, "height");
    CHECK(h && h->enabled_if == NULL);                 /* stays editable (D51, 5.2 dialog) */
    p = fx_params_new(fx, NULL);
    edit(fx, p, "width", 20.0, last);
    CHECK(get(fx, p, "height") == 20.0);               /* D51: forced to the same value */
    edit(fx, p, "height", 3.0, last);
    CHECK(get(fx, p, "width") == 3.0);
    edit(fx, p, "linked", 0.0, last);
    edit(fx, p, "width", 50.0, last);
    CHECK(get(fx, p, "height") == 3.0);
    edit(fx, p, "linked", 1.0, last);
    CHECK(get(fx, p, "height") == 50.0);               /* width was edited last */
    fx_params_free(p);
}

static void t_minmax_frosted(void)
{
    const fx_effect *fx = fx_of("distort.frosted_glass");
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    CHECK(fx_prop_minmax_partner(fx->props, fx->n_props, idx_of(fx, "min_radius")) ==
          idx_of(fx, "max_radius"));
    CHECK(fx_prop_minmax_partner(fx->props, fx->n_props, idx_of(fx, "max_radius")) ==
          FX_RULE_NONE);
    p = fx_params_new(fx, NULL);
    CHECK(get(fx, p, "max_radius") == 3.0 && get(fx, p, "min_radius") == 0.0);
    /* raising Minimum above Maximum pushes Maximum up (O52) */
    edit(fx, p, "min_radius", 10.0, NULL);
    CHECK(get(fx, p, "min_radius") == 10.0 && get(fx, p, "max_radius") == 10.0);
    edit(fx, p, "max_radius", 25.5, NULL);
    CHECK(get(fx, p, "min_radius") == 10.0 && get(fx, p, "max_radius") == 25.5);
    /* lowering Maximum below Minimum pulls Minimum down */
    edit(fx, p, "max_radius", 4.25, NULL);
    CHECK(get(fx, p, "min_radius") == 4.25 && get(fx, p, "max_radius") == 4.25);
    /* within range nothing moves */
    edit(fx, p, "min_radius", 1.0, NULL);
    CHECK(get(fx, p, "max_radius") == 4.25);
    /* a broken pair from a script: the initial sync keeps Minimum */
    CHECK(fx_param_set(fx, p, "min_radius", 30.0) == PC_OK);
    CHECK(fx_param_set(fx, p, "max_radius", 2.0) == PC_OK);
    CHECK(fx_params_apply_rules(fx, p) == 1u);
    CHECK(get(fx, p, "max_radius") == 30.0);
    fx_params_free(p);
}

/* Rule hints that name missing or unfit partners are rejected. */
static int dummy_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)state; (void)src; (void)dst; (void)roi; (void)env; (void)host;
    (void)job;
    return FX_OK;
}

typedef struct rp { int32_t a, b, on; int32_t pad; double r; } rp;

static void t_rule_validation(void)
{
    fx_prop props[4] = {
        { "a", "A", FXP_INT, 0u, 0.0, 10.0, 1.0, 1.0, NULL, "link:on", 0u, 0u, NULL },
        { "b", "B", FXP_INT, 4u, 0.0, 10.0, 1.0, 1.0, NULL, "link:on", 0u, 0u, NULL },
        { "on", "On", FXP_BOOL, 8u, 0.0, 1.0, 1.0, 0.0, NULL, NULL, 0u, 0u, NULL },
        { "r", "R", FXP_REAL, 16u, 0.0, 10.0, 1.0, 0.1, NULL, NULL, 0u, 0u, NULL },
    };
    fx_effect fx;
    memset(&fx, 0, sizeof fx);
    fx.size = (uint32_t)sizeof fx;
    fx.id = "test.rules";
    fx.menu = "Effects/Test/Rules";
    fx.props = props;
    fx.n_props = 4u;
    fx.params_size = (uint32_t)sizeof(rp);
    fx.render = dummy_render;
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_OK);
    props[1].hint = "link:nope";                       /* unknown source */
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_ERR_ARG);
    props[1].hint = "link:r";                          /* source is not a BOOL */
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_ERR_ARG);
    props[1].hint = "minmax:r";                        /* partner of another kind */
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_ERR_ARG);
    props[1].hint = "minmax:a";
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_OK);
    props[1].hint = "minmax:b";                        /* itself */
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_ERR_ARG);
    props[1].hint = "someone else's widget";           /* unknown hints are ignored */
    CHECK(fx_effect_validate(&fx, NULL, 0u) == PC_OK);
    props[1].hint = "link:on";
    /* a lone member links with nobody but is still synced harmlessly */
    {
        rp v;
        memset(&v, 0, sizeof v);
        v.a = 4;
        v.b = 9;
        v.on = 1;
        CHECK(fx_props_rules(props, 4u, &v, FX_RULE_NONE, NULL) == 1u);
        CHECK(v.b == 4);
        v.on = 0;
        v.a = 2;
        CHECK(fx_props_rules(props, 4u, &v, 0u, NULL) == 0u);
        CHECK(v.b == 4);
        CHECK(fx_props_rules(NULL, 4u, &v, 0u, NULL) == 0u);
        CHECK(fx_props_rules(props, 4u, NULL, 0u, NULL) == 0u);
        CHECK(fx_props_rules(props, 4u, &v, 99u, NULL) == 0u);   /* out of range index */
    }
}

/* The non-linear sliders are exactly those measured non-linear on the 5.2
 * dialogs (tests/ui/test_ui_slider_exp.c checks the mapping itself). */
static void t_nonlinear_set(void)
{
    static const char *const want[] = {
        "blur.bokeh/radius", "blur.bokeh/gamma_boost", "blur.gaussian/radius",
        "blur.gaussian/gamma_boost", "blur.square/radius", "blur.square/gamma_boost",
        "blur.motion/distance", "distort.dents/scale", "distort.dents/refraction",
        "distort.dents/turbulence", "distort.frosted_glass/max_radius",
        "distort.frosted_glass/min_radius", "distort.polar_inversion/amount",
        "distort.tile_reflection/tile_size", "object.drop_shadow/radius",
        "photo.vignette/radius", "render.turbulence/period",
    };
    size_t nwant = sizeof want / sizeof want[0], found = 0;
    for (uint32_t i = 0; i < fx_registry_count(g_reg); i++) {
        const fx_effect *fx = fx_registry_at(g_reg, i);
        for (uint32_t k = 0; k < fx->n_props; k++) {
            char name[256];
            bool listed = false;
            if (strncmp(fx->id, "org.paintc.", 11) != 0) continue;
            snprintf(name, sizeof name, "%s/%s", fx->id + 11, fx->props[k].key);
            for (size_t j = 0; j < nwant; j++)
                if (strcmp(want[j], name) == 0) listed = true;
            if (listed != ((fx->props[k].flags & FXP_F_SLIDER_LOG) != 0u)) {
                INFO("non-linear flag mismatch: %s", name);
                CHECK(0);
            }
            found += listed;
        }
    }
    CHECK(found == nwant);
}

static void t_drop_shadow_schema(void)
{
    const fx_effect *fx = fx_of("object.drop_shadow");
    const fx_prop *c;
    CHECK(fx != NULL);
    if (!fx) return;
    CHECK((fx->flags & FX_FLAG_NO_SEL_CLIP) != 0u);
    c = fx_prop_find(fx, "color");
    CHECK(c && c->kind == FXP_COLOR && (c->flags & FXP_F_COLOR_NO_ALPHA) != 0u);
    CHECK(c && (uint32_t)c->def == 0xFF000000u);
    /* no other color prop claims to be RGB only */
    for (uint32_t i = 0; i < fx_registry_count(g_reg); i++) {
        const fx_effect *e = fx_registry_at(g_reg, i);
        for (uint32_t k = 0; k < e->n_props; k++)
            if ((e->props[k].flags & FXP_F_COLOR_NO_ALPHA) && e != fx) CHECK(0);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rndu(2u);                                    /* harness helpers unused here */
    (void)rnd8();
    g_reg = fx_registry_create();
    CHECK(g_reg != NULL);
    if (!g_reg) return pc_test_finish();
    CHECK(fx_registry_add_builtins(g_reg) >= 55);           /* 5.1 effects and adjustments */
    RUN(t_link_posterize);
    RUN(t_link_morphology);
    RUN(t_minmax_frosted);
    RUN(t_rule_validation);
    RUN(t_nonlinear_set);
    RUN(t_drop_shadow_schema);
    fx_registry_destroy(g_reg);
    return pc_test_finish();
}
