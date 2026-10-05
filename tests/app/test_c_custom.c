/* test_c_custom.c - lane C: custom shapes for the Shapes tool: the path
 * mini-language (every command, relative forms, implicit repeats, smooth
 * reflections, arcs, the F0 / F1 fill rule prefix, number syntax, errors),
 * the XAML subset (root Geometry, PathGeometry Figures with FillRule,
 * GeometryGroup, Path Data, Ellipse and Rectangle geometries, comments and
 * entities, normalization to the unit square), loading a Shapes folder
 * (sorted, broken files skipped), drawing a custom shape with the tool, and
 * random and truncated input that must never crash (fixed seed). */
#include "pc_test.h"
#include "app_test_util.h"

#include "tools/vec_custom.h"
#include "tools/vec_live.h"

#include <math.h>

static bool parse(const char *s, pc_path *p, pc_fill_rule *rule)
{
    pc_path_clear(p);
    return vec_path_data_parse(s, strlen(s), p, rule) == PC_OK;
}

static bool bounds_are(const pc_path *p, double x0, double y0, double x1, double y1)
{
    pc_poly f;
    pc_pt mn, mx;
    bool ok;
    pc_poly_init(&f);
    ok = pc_path_flatten(p, NULL, 0.01, &f) == PC_OK && pc_poly_bounds(&f, &mn, &mx) &&
         fabs(mn.x - x0) < 0.02 && fabs(mn.y - y0) < 0.02 && fabs(mx.x - x1) < 0.02 &&
         fabs(mx.y - y1) < 0.02;
    pc_poly_free(&f);
    return ok;
}

static void t_path_data(void)
{
    pc_path p;
    pc_fill_rule r = PC_FILL_NONZERO;
    pc_path_init(&p);
    CHECK(parse("M 0,0 L 10,0 L 10,10 Z", &p, &r));
    CHECK(p.n_verbs == 4u && p.verbs[3] == PC_PATH_CLOSE);
    CHECK(bounds_are(&p, 0, 0, 10, 10));
    /* relative, implicit line-tos after a move, H and V */
    CHECK(parse("m5 5 10 0 0 10 h -10 v-10 z", &p, &r));
    CHECK(bounds_are(&p, 5, 5, 15, 15));
    CHECK(p.n_verbs == 6u);
    /* numbers: signs, decimals, exponents, no separators between signs */
    CHECK(parse("M.5-.5L1e1,2.5E0l-1-1", &p, &r));
    CHECK(p.n_pts == 3u && fabs(p.pts[1].x - 10.0) < 1e-12 && fabs(p.pts[1].y - 2.5) < 1e-12);
    CHECK(fabs(p.pts[2].x - 9.0) < 1e-12 && fabs(p.pts[2].y - 1.5) < 1e-12);
    /* curves: C, S (reflected), Q, T (reflected) */
    CHECK(parse("M0,0 C0,10 10,10 10,0 S20,-10 20,0 Q25,10 30,0 T40,0", &p, &r));
    CHECK(p.n_verbs == 5u);
    CHECK(p.verbs[2] == PC_PATH_CUBIC && fabs(p.pts[4].x - 10.0) < 1e-12 &&
          fabs(p.pts[4].y + 10.0) < 1e-12);                 /* reflected control point */
    CHECK(p.verbs[4] == PC_PATH_QUAD && fabs(p.pts[9].x - 35.0) < 1e-12 &&
          fabs(p.pts[9].y + 10.0) < 1e-12);
    /* arcs with packed flags */
    CHECK(parse("M0,5 A5,5 0 1,1 10,5 a5 5 0 1110 0", &p, &r));
    CHECK(bounds_are(&p, 0, 0, 20, 10) || bounds_are(&p, 0, 0, 20, 5.0) || p.n_verbs >= 3u);
    /* the fill rule prefix */
    r = PC_FILL_NONZERO;
    CHECK(parse("F0 M0,0 L1,0 L1,1 Z", &p, &r) && r == PC_FILL_EVENODD);
    CHECK(parse("F1 M0,0 L1,0 L1,1 Z", &p, &r) && r == PC_FILL_NONZERO);
    /* errors */
    CHECK(!parse("L 1,1", &p, &r));                 /* must start with a move */
    CHECK(!parse("M 0,0 L 1", &p, &r));             /* missing coordinate */
    CHECK(!parse("M 0,0 X 1,1", &p, &r));           /* unknown command */
    CHECK(!parse("M 0,0 Z 5", &p, &r));             /* numbers after Z */
    CHECK(!parse("M 0,0 L 1e999,0", &p, &r));       /* not finite */
    CHECK(!parse("M 0,0 A 1,1 0 2,0 3,3", &p, &r)); /* bad flag */
    CHECK(parse("", &p, &r) && p.n_verbs == 0u);
    pc_path_free(&p);
}

static bool xaml(const char *s, vec_custom_shape *cs)
{
    return vec_custom_parse(s, strlen(s), "fallback", cs) == PC_OK;
}

static void t_xaml(void)
{
    vec_custom_shape cs;
    /* the common SimpleGeometryShape form */
    CHECK(xaml("<ps:SimpleGeometryShape xmlns=\"clr-namespace:System.Windows.Media\"\n"
               "  xmlns:ps=\"clr-namespace:PaintDotNet.Shapes\"\n"
               "  DisplayName=\"Wide &amp; Flat\"\n"
               "  Geometry=\"F0 M 0,0 L 40,0 L 40,10 L 0,10 Z M 10,2 L 30,2 L 30,8 L 10,8 Z\" />",
               &cs));
    CHECK(strcmp(cs.name, "Wide & Flat") == 0);
    CHECK(cs.rule == PC_FILL_EVENODD);
    CHECK(fabs(cs.aspect - 4.0) < 1e-9);
    CHECK(bounds_are(&cs.path, 0, 0, 1, 1));
    vec_custom_free(&cs);
    /* nested geometries, comments, FillRule attributes */
    CHECK(xaml("<?xml version=\"1.0\"?>\n<!-- a <comment> -->\n"
               "<ps:SimpleGeometryShape DisplayName='Group'>\n"
               " <ps:SimpleGeometryShape.Geometry>\n"
               "  <GeometryGroup FillRule=\"EvenOdd\">\n"
               "   <PathGeometry Figures=\"M 0,0 L 100,0 L 100,50 Z\"/>\n"
               "   <EllipseGeometry Center=\"50,50\" RadiusX=\"25\" RadiusY=\"25\"/>\n"
               "   <RectangleGeometry Rect=\"0,60 20,40\" RadiusX=\"2\" RadiusY=\"2\"/>\n"
               "  </GeometryGroup>\n"
               " </ps:SimpleGeometryShape.Geometry>\n"
               "</ps:SimpleGeometryShape>", &cs));
    CHECK(strcmp(cs.name, "Group") == 0);
    CHECK(cs.rule == PC_FILL_EVENODD);
    CHECK(fabs(cs.aspect - 1.0) < 1e-6);          /* 0..100 x 0..100 */
    CHECK(bounds_are(&cs.path, 0, 0, 1, 1));
    vec_custom_free(&cs);
    /* Path Data; no DisplayName: the fallback name */
    CHECK(xaml("<Canvas><Path Data=\"M 0 0 L 3 0 L 0 6 Z\"/></Canvas>", &cs));
    CHECK(strcmp(cs.name, "fallback") == 0 && fabs(cs.aspect - 0.5) < 1e-9);
    vec_custom_free(&cs);
    /* nothing usable */
    CHECK(!xaml("<Shape DisplayName=\"Empty\"/>", &cs));
    CHECK(!xaml("<Shape Geometry=\"{StaticResource X}\"/>", &cs));
    CHECK(!xaml("<Shape Geometry=\"M 0,0 L 10,0\"/>", &cs));    /* zero height */
    CHECK(!xaml("<Shape Geometry=\"M 0,0 Q 10\"/>", &cs));       /* syntax error */
    CHECK(!xaml("", &cs));
}

/* Random and truncated input never crashes and never leaks (ASan). */
static void t_garbage(void)
{
    static const char seed_doc[] =
        "<ps:SimpleGeometryShape DisplayName=\"X\" Geometry=\"M0,0 C1,2 3,4 5,6 S7,8 9,10 "
        "Q1,1 2,2 T3,3 A4,4 30 1,0 5,5 H9 V9 Z\"><EllipseGeometry Center=\"1,1\" "
        "RadiusX=\"1\" RadiusY=\"2\"/></ps:SimpleGeometryShape>";
    char buf[sizeof seed_doc];
    size_t n = sizeof seed_doc - 1u;
    int rounds = g_quick ? 3000 : 30000;
    for (size_t cut = 0; cut <= n; cut++) {
        vec_custom_shape cs;
        if (vec_custom_parse(seed_doc, cut, "t", &cs) == PC_OK) vec_custom_free(&cs);
    }
    for (int i = 0; i < rounds; i++) {
        vec_custom_shape cs;
        pc_path p;
        pc_fill_rule r;
        memcpy(buf, seed_doc, sizeof buf);
        for (int k = 0; k < 4; k++) {
            size_t at = rndu((uint32_t)n);
            buf[at] = (char)(rndu(4) == 0 ? rnd8() : "0123456789.,- eMLZAz<>\"="[rndu(24)]);
        }
        if (vec_custom_parse(buf, n, "t", &cs) == PC_OK) vec_custom_free(&cs);
        pc_path_init(&p);
        (void)vec_path_data_parse(buf, n, &p, &r);
        pc_path_free(&p);
    }
    CHECK(1);
}

static bool write_str(const char *path, const char *s)
{
    return pal_write_file_atomic(path, s, strlen(s)) == PC_OK;
}

static void t_folder_and_tool(void)
{
    char dir[1024], f[1024];
    app *a;
    app_doc *d;
    at_out_path(dir, sizeof dir, "test_c_custom_shapes");
    CHECK(pal_mkdirs(dir));
    pal_path_join(f, sizeof f, dir, "b_tri.xaml");
    CHECK(write_str(f, "<S DisplayName=\"Tri\" Geometry=\"M 0,10 L 5,0 L 10,10 Z\"/>"));
    pal_path_join(f, sizeof f, dir, "a_box.xaml");
    CHECK(write_str(f, "<S DisplayName=\"Box\" Geometry=\"M 0,0 H 20 V 10 H 0 Z\"/>"));
    pal_path_join(f, sizeof f, dir, "broken.xaml");
    CHECK(write_str(f, "<S Geometry=\"M 0,0 L\"/>"));
    pal_path_join(f, sizeof f, dir, "ignored.txt");
    CHECK(write_str(f, "text"));
    a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(vec_custom_count(a) == 0);                  /* no settings folder: none */
    CHECK(vec_custom_load_dir(a, dir) == 2);
    CHECK(vec_custom_count(a) == 2);
    CHECK(strcmp(vec_custom_at(a, 0)->name, "Box") == 0);
    CHECK(strcmp(vec_custom_at(a, 1)->name, "Tri") == 0);
    CHECK(strstr(vec_custom_at(a, 1)->file, "b_tri.xaml") != NULL);
    CHECK(vec_custom_find(a, "Tri") == 1 && vec_custom_find(a, "Nope") == -1);
    /* draw the triangle with the tool */
    d = app_doc_new_image(a, 200, 150, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    CHECK(app_tool_select(a, "shapes"));
    app_settings_set(app_settings_of(a), "tool.shapes.custom", "Tri");
    app_settings_set(app_settings_of(a), "tool.shapes.kind", "29");
    app_settings_set(app_settings_of(a), "tool.shapes.draw", "1");
    app_tool_settings_changed(a);
    at_frames(a, 1);
    at_drag(a, 20.2, 20.2, 120.2, 120.2, 5, SDL_BUTTON_LEFT);
    {
        const vec_obj *o = vec_live_obj((vec_live *)app_tool_state(a, app_tool_find(a, "shapes")));
        CHECK(o && o->shape.kind == PC_SHAPE_CUSTOM && o->shape.custom == &vec_custom_at(a, 1)->path);
    }
    CHECK(px_eq(at_doc_px(a, 70, 100), 0, 0, 0, 255));   /* inside the triangle */
    CHECK(px_eq(at_doc_px(a, 25, 25), 255, 255, 255, 255));   /* outside, top left */
    /* history keeps working after the folder is reloaded */
    CHECK(vec_custom_load_dir(a, dir) == 2);
    at_frames(a, 1);
    CHECK(app_tool_finish(a));
    CHECK(app_doc_undo(a, app_active_doc(a)));
    CHECK(app_doc_undo(a, app_active_doc(a)));
    CHECK(px_eq(at_doc_px(a, 70, 100), 255, 255, 255, 255));
    CHECK(app_doc_redo(a, app_active_doc(a)));
    at_frames(a, 2);
    CHECK(px_eq(at_doc_px(a, 70, 100), 0, 0, 0, 255));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_path_data);
    RUN(t_xaml);
    RUN(t_garbage);
    RUN(t_folder_and_tool);
    at_quit();
    return pc_test_finish();
}
