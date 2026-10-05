/* test_tools_shapes.c - lane TOOLS (wave 4 items 27 and 33): custom shape
 * files.
 *   t_combined_modes   CombinedGeometry honors GeometryCombineMode (Union,
 *                      Intersect, Xor, Exclude) with operands given as
 *                      property elements or path data, nested too;
 *   t_verbose_figures  PathFigure elements with every segment kind, with
 *                      and without the .Figures / .Segments wrappers;
 *   t_transforms       Transform attributes and transform elements;
 *   t_garbage          mutated files never crash or leak (ASan);
 *   t_folder           the repro: a rectangle Exclude circle draws with a
 *                      hole through the Shapes tool, and the Custom group
 *                      sorts names without regard to case.
 * Headless, fixed seeds. */
#include "pc_test.h"
#include "app_test_util.h"
#include "tools/vec_custom.h"
#include "tools/vec_live.h"

#include <math.h>

#define GRID 200

static bool parse(const char *x, vec_custom_shape *cs)
{
    pc_status st = vec_custom_parse(x, strlen(x), "t", cs);
    if (st != PC_OK) INFO("parse: %s", pc_status_str(st));
    return st == PC_OK;
}

/* Coverage (0..255) of the normalized shape at (u, v) in [0, 1]^2,
 * rendered at GRID x GRID with its fill rule. */
static int cov_at(const vec_custom_shape *cs, double u, double v)
{
    pc_poly p;
    pc_mask m;
    pc_affine sc = pc_affine_scale((double)GRID, (double)GRID);
    int r = -1;
    pc_poly_init(&p);
    if (pc_path_flatten(&cs->path, &sc, 0.05, &p) == PC_OK &&
        pc_mask_alloc(&m, pc_rect_make(0, 0, GRID, GRID)) == PC_OK) {
        int x = (int)(u * GRID), y = (int)(v * GRID);
        if (x >= GRID) x = GRID - 1;
        if (y >= GRID) y = GRID - 1;
        if (pc_raster_fill_poly(&p, NULL, cs->rule, true, &m) == PC_OK)
            r = m.px[(size_t)y * (size_t)m.stride + (size_t)x];
        pc_mask_free(&m);
    }
    pc_poly_free(&p);
    return r;
}

static bool in(const vec_custom_shape *cs, double u, double v) { return cov_at(cs, u, v) > 200; }
static bool out(const vec_custom_shape *cs, double u, double v) { return cov_at(cs, u, v) < 55; }

/* Rectangle 0,0,100,100 combined with a circle of radius 30 at 50,50. */
static void combined(const char *mode, vec_custom_shape *cs, bool *ok)
{
    char x[1024];
    (void)snprintf(x, sizeof x,
                   "<ps:SimpleGeometryShape xmlns:ps=\"clr-namespace:PaintDotNet.Shapes\""
                   " DisplayName=\"Combined %s\">\n"
                   " <ps:SimpleGeometryShape.Geometry>\n"
                   "  <CombinedGeometry GeometryCombineMode=\"%s\">\n"
                   "   <CombinedGeometry.Geometry1><RectangleGeometry Rect=\"0,0,100,100\"/>"
                   "</CombinedGeometry.Geometry1>\n"
                   "   <CombinedGeometry.Geometry2><EllipseGeometry Center=\"50,50\""
                   " RadiusX=\"30\" RadiusY=\"30\"/></CombinedGeometry.Geometry2>\n"
                   "  </CombinedGeometry>\n"
                   " </ps:SimpleGeometryShape.Geometry>\n"
                   "</ps:SimpleGeometryShape>", mode, mode);
    *ok = parse(x, cs);
}

static void t_combined_modes(void)
{
    vec_custom_shape cs;
    bool ok;
    /* Exclude: the rectangle with a round hole */
    combined("Exclude", &cs, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(strcmp(cs.name, "Combined Exclude") == 0);
        CHECK(fabs(cs.aspect - 1.0) < 1e-6);
        CHECK(out(&cs, 0.5, 0.5) && out(&cs, 0.5, 0.75));      /* inside the circle */
        CHECK(in(&cs, 0.05, 0.05) && in(&cs, 0.95, 0.5));     /* the rest of the square */
        CHECK(in(&cs, 0.5, 0.18) && out(&cs, 0.5, 0.22));     /* the circle's edge at 0.2 */
        vec_custom_free(&cs);
    }
    combined("Intersect", &cs, &ok);
    CHECK(ok);
    if (ok) {
        /* only the circle: its bounds are the shape now */
        CHECK(fabs(cs.aspect - 1.0) < 1e-3);
        CHECK(in(&cs, 0.5, 0.5) && out(&cs, 0.03, 0.03) && out(&cs, 0.97, 0.97));
        vec_custom_free(&cs);
    }
    combined("Xor", &cs, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(out(&cs, 0.5, 0.5) && in(&cs, 0.05, 0.05));
        vec_custom_free(&cs);
    }
    combined("Union", &cs, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(in(&cs, 0.5, 0.5) && in(&cs, 0.05, 0.05));
        /* one outline, no inner circle left over from the merge */
        {
            pc_poly p;
            pc_poly_init(&p);
            CHECK(pc_path_flatten(&cs.path, NULL, 0.01, &p) == PC_OK && p.n_contours == 1u);
            pc_poly_free(&p);
        }
        vec_custom_free(&cs);
    }
    /* operands as path data, overlapping only partly (Exclude is not the
     * even-odd of both): two side by side squares */
    CHECK(parse("<S DisplayName=\"Bite\"><S.Geometry>"
                "<CombinedGeometry GeometryCombineMode=\"Exclude\""
                " Geometry1=\"M0,0 L100,0 L100,100 L0,100 Z\""
                " Geometry2=\"M50,50 L150,50 L150,150 L50,150 Z\"/>"
                "</S.Geometry></S>", &cs));
    if (cs.path.n_verbs) {
        /* an L: the square minus its bottom right quarter (bounds unchanged) */
        CHECK(fabs(cs.aspect - 1.0) < 1e-3);
        CHECK(in(&cs, 0.25, 0.25) && in(&cs, 0.75, 0.25) && in(&cs, 0.25, 0.75));
        CHECK(out(&cs, 0.75, 0.75));
        vec_custom_free(&cs);
    }
    /* nested: (square Xor circle) Union a small square in the hole */
    CHECK(parse("<S><S.Geometry><CombinedGeometry GeometryCombineMode=\"Union\">"
                "<CombinedGeometry.Geometry1><CombinedGeometry GeometryCombineMode=\"Xor\">"
                "<CombinedGeometry.Geometry1><RectangleGeometry Rect=\"0 0 100 100\"/>"
                "</CombinedGeometry.Geometry1><CombinedGeometry.Geometry2>"
                "<EllipseGeometry Center=\"50,50\" RadiusX=\"40\" RadiusY=\"40\"/>"
                "</CombinedGeometry.Geometry2></CombinedGeometry></CombinedGeometry.Geometry1>"
                "<CombinedGeometry.Geometry2><RectangleGeometry Rect=\"45,45,10,10\"/>"
                "</CombinedGeometry.Geometry2></CombinedGeometry></S.Geometry></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(in(&cs, 0.5, 0.5) && out(&cs, 0.3, 0.5) && in(&cs, 0.02, 0.02));
        vec_custom_free(&cs);
    }
    /* an empty result is not a usable shape */
    CHECK(!parse("<S><CombinedGeometry GeometryCombineMode=\"Intersect\""
                 " Geometry1=\"M0,0 L10,0 L10,10 Z\" Geometry2=\"M20,20 L30,20 L30,30 Z\"/></S>",
                 &cs));
    /* the repro file from the audit */
    CHECK(parse("<ps:SimpleGeometryShape xmlns=\"clr-namespace:PaintDotNet.UI.Media;"
                "assembly=PaintDotNet.Framework\"\n"
                " xmlns:ps=\"clr-namespace:PaintDotNet.Shapes;assembly=PaintDotNet.Framework\"\n"
                " DisplayName=\"Combined Exclude\">\n"
                " <ps:SimpleGeometryShape.Geometry>\n"
                "  <CombinedGeometry GeometryCombineMode=\"Exclude\">\n"
                "   <CombinedGeometry.Geometry1><RectangleGeometry Rect=\"0,0,100,100\"/>"
                "</CombinedGeometry.Geometry1>\n"
                "   <CombinedGeometry.Geometry2><EllipseGeometry Center=\"50,50\" RadiusX=\"30\""
                " RadiusY=\"30\"/></CombinedGeometry.Geometry2>\n"
                "  </CombinedGeometry>\n"
                " </ps:SimpleGeometryShape.Geometry>\n"
                "</ps:SimpleGeometryShape>", &cs));
    if (cs.path.n_verbs) {
        CHECK(out(&cs, 0.5, 0.5) && in(&cs, 0.1, 0.1));
        vec_custom_free(&cs);
    }
}

/* ---- figures -------------------------------------------------------------------------- */
static void t_verbose_figures(void)
{
    vec_custom_shape cs;
    /* the audit's file: a triangle from PathFigure and LineSegments */
    CHECK(parse("<ps:SimpleGeometryShape xmlns:ps=\"clr-namespace:PaintDotNet.Shapes\""
                " DisplayName=\"Explicit Figure\">\n"
                " <ps:SimpleGeometryShape.Geometry>\n"
                "  <PathGeometry>\n"
                "   <PathFigure StartPoint=\"0,0\" IsClosed=\"True\">\n"
                "    <LineSegment Point=\"100,0\"/>\n"
                "    <LineSegment Point=\"50,80\"/>\n"
                "   </PathFigure>\n"
                "  </PathGeometry>\n"
                " </ps:SimpleGeometryShape.Geometry>\n"
                "</ps:SimpleGeometryShape>", &cs));
    if (cs.path.n_verbs) {
        CHECK(strcmp(cs.name, "Explicit Figure") == 0);
        CHECK(fabs(cs.aspect - 100.0 / 80.0) < 1e-9);
        CHECK(in(&cs, 0.5, 0.3) && out(&cs, 0.1, 0.9) && out(&cs, 0.9, 0.9));
        vec_custom_free(&cs);
    }
    /* wrappers, every segment kind, two figures (even-odd by default) */
    CHECK(parse("<S DisplayName=\"Kinds\"><S.Geometry><PathGeometry><PathGeometry.Figures>"
                "<PathFigureCollection>"
                "<PathFigure StartPoint=\"0,0\" IsClosed=\"true\"><PathFigure.Segments>"
                "<PathSegmentCollection>"
                "<PolyLineSegment Points=\"100,0 100,50\"/>"
                "<ArcSegment Point=\"100,100\" Size=\"25,25\" SweepDirection=\"Clockwise\"/>"
                "<BezierSegment Point1=\"70,110\" Point2=\"30,110\" Point3=\"0,100\"/>"
                "<QuadraticBezierSegment Point1=\"-10,50\" Point2=\"0,0\"/>"
                "</PathSegmentCollection></PathFigure.Segments></PathFigure>"
                "<PathFigure StartPoint=\"40,40\" IsClosed=\"True\">"
                "<PolyBezierSegment Points=\"45,40 55,40 60,40 60,45 60,55 60,60\"/>"
                "<PolyQuadraticBezierSegment Points=\"50,60 40,60 40,50 40,40\"/>"
                "</PathFigure>"
                "</PathFigureCollection></PathGeometry.Figures></PathGeometry></S.Geometry></S>",
                &cs));
    if (cs.path.n_verbs) {
        pc_pt mn, mx;
        pc_poly p;
        pc_poly_init(&p);
        /* the arc bulges right to x = 125, the cubic down to y = 107.5, the
         * quadratic left to x = -5: 130 x 107.5; the inner square is a hole
         * (even-odd) */
        CHECK(pc_path_flatten(&cs.path, NULL, 0.001, &p) == PC_OK && p.n_contours == 2u);
        CHECK(pc_poly_bounds(&p, &mn, &mx) && fabs(mn.x) < 1e-3 && fabs(mx.x - 1.0) < 1e-3);
        CHECK(fabs(cs.aspect - 130.0 / 107.5) < 1e-3);
        CHECK(cs.rule == PC_FILL_EVENODD);
        CHECK(in(&cs, 0.2, 0.2) && out(&cs, 0.4, 0.4) && in(&cs, 0.95, 0.7));
        CHECK(out(&cs, 0.95, 0.15));                   /* outside the arc */
        pc_poly_free(&p);
        vec_custom_free(&cs);
    }
    /* an open figure fills as if closed; FillRule on the geometry */
    CHECK(parse("<S><PathGeometry FillRule=\"Nonzero\"><PathFigure StartPoint=\"0,0\">"
                "<LineSegment Point=\"10,0\"/><LineSegment Point=\"10,10\"/></PathFigure>"
                "</PathGeometry></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(cs.rule == PC_FILL_NONZERO && in(&cs, 0.8, 0.3) && out(&cs, 0.2, 0.8));
        vec_custom_free(&cs);
    }
    /* segments outside a figure and figures without segments are harmless */
    CHECK(!parse("<S><LineSegment Point=\"1,1\"/><PathGeometry><PathFigure/></PathGeometry></S>",
                 &cs));
}

/* ---- transforms ----------------------------------------------------------------------- */
static void t_transforms(void)
{
    vec_custom_shape cs;
    /* a square turned 45 degrees: a diamond */
    CHECK(parse("<S><RectangleGeometry Rect=\"0,0,10,10\"><RectangleGeometry.Transform>"
                "<RotateTransform Angle=\"45\" CenterX=\"5\" CenterY=\"5\"/>"
                "</RectangleGeometry.Transform></RectangleGeometry></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(fabs(cs.aspect - 1.0) < 1e-6);
        CHECK(in(&cs, 0.5, 0.5) && out(&cs, 0.08, 0.08) && out(&cs, 0.92, 0.92));
        vec_custom_free(&cs);
    }
    /* a group of transforms: scale x by 3, then move (only the aspect shows) */
    CHECK(parse("<S><EllipseGeometry Center=\"0,0\" RadiusX=\"5\" RadiusY=\"5\">"
                "<EllipseGeometry.Transform><TransformGroup>"
                "<ScaleTransform ScaleX=\"3\"/><TranslateTransform X=\"7\" Y=\"9\"/>"
                "</TransformGroup></EllipseGeometry.Transform></EllipseGeometry></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(fabs(cs.aspect - 3.0) < 1e-3);
        vec_custom_free(&cs);
    }
    /* a Transform attribute (matrix) and a skew element */
    CHECK(parse("<S><PathGeometry Figures=\"M0,0 L10,0 L10,10 L0,10 Z\""
                " Transform=\"2,0,0,1,0,0\"/></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(fabs(cs.aspect - 2.0) < 1e-9);
        vec_custom_free(&cs);
    }
    CHECK(parse("<S><RectangleGeometry Rect=\"0,0,10,10\"><RectangleGeometry.Transform>"
                "<SkewTransform AngleX=\"45\"/></RectangleGeometry.Transform>"
                "</RectangleGeometry></S>", &cs));
    if (cs.path.n_verbs) {
        CHECK(fabs(cs.aspect - 2.0) < 1e-6);
        CHECK(out(&cs, 0.9, 0.1) && in(&cs, 0.9, 0.9));
        vec_custom_free(&cs);
    }
}

/* ---- robustness ------------------------------------------------------------------------ */
static void t_garbage(void)
{
    static const char seed_doc[] =
        "<S DisplayName=\"X\"><S.Geometry><CombinedGeometry GeometryCombineMode=\"Xor\">"
        "<CombinedGeometry.Geometry1><PathGeometry><PathFigure StartPoint=\"0,0\" "
        "IsClosed=\"True\"><ArcSegment Point=\"9,9\" Size=\"5,5\"/><PolyLineSegment "
        "Points=\"1,2 3,4\"/></PathFigure></PathGeometry></CombinedGeometry.Geometry1>"
        "<CombinedGeometry.Geometry2><RectangleGeometry Rect=\"1,1,5,5\"><RectangleGeometry."
        "Transform><RotateTransform Angle=\"30\"/></RectangleGeometry.Transform>"
        "</RectangleGeometry></CombinedGeometry.Geometry2></CombinedGeometry></S.Geometry></S>";
    char buf[sizeof seed_doc];
    size_t n = sizeof seed_doc - 1u;
    /* (each parse that reaches the boolean rasterizes: ~20 ms optimized) */
    int rounds = g_quick ? 40 : 1500;
    for (size_t cut = 0; cut <= n; cut += g_quick ? 23u : 3u) {
        vec_custom_shape cs;
        if (vec_custom_parse(seed_doc, cut, "t", &cs) == PC_OK) vec_custom_free(&cs);
    }
    for (int i = 0; i < rounds; i++) {
        vec_custom_shape cs;
        memcpy(buf, seed_doc, sizeof buf);
        for (int k = 0; k < 3; k++) {
            size_t at = rndu((uint32_t)n);
            buf[at] = (char)(rndu(4) == 0 ? rnd8() : "0123456789.,- eMLZ<>/\"="[rndu(23)]);
        }
        if (vec_custom_parse(buf, n, "t", &cs) == PC_OK) vec_custom_free(&cs);
    }
    /* nesting beyond the limit and too many booleans are refused */
    {
        char *deep = (char *)malloc(64u * 4u + 64u);
        vec_custom_shape cs;
        size_t k = 0;
        if (deep) {
            for (int i = 0; i < 60; i++) {
                memcpy(deep + k, "<G>", 3u);
                k += 3u;
            }
            memcpy(deep + k, "<S Geometry=\"M0,0 L1,0 L1,1 Z\"/>", 33u);
            k += 33u;
            CHECK(vec_custom_parse(deep, k, "t", &cs) == PC_ERR_LIMIT);
            free(deep);
        }
    }
    {
        char x[8192];
        size_t k = 0;
        vec_custom_shape cs;
        k += (size_t)snprintf(x + k, sizeof x - k, "<S>");
        for (int i = 0; i < 40; i++)
            k += (size_t)snprintf(x + k, sizeof x - k,
                                  "<CombinedGeometry Geometry1=\"M0,0 L1,0 L1,1 Z\" "
                                  "Geometry2=\"M0,0 L1,1 L0,1 Z\"/>");
        k += (size_t)snprintf(x + k, sizeof x - k, "</S>");
        CHECK(vec_custom_parse(x, k, "t", &cs) == PC_ERR_LIMIT);
    }
    CHECK(1);
}

/* ---- the Shapes tool -------------------------------------------------------------------- */
static bool write_str(const char *path, const char *s)
{
    return pal_write_file_atomic(path, s, strlen(s)) == PC_OK;
}

static void t_folder(void)
{
    char dir[1024], f[1024];
    app *a;
    app_doc *d;
    int32_t idx;
    at_out_path(dir, sizeof dir, "test_tools_shapes");
    CHECK(pal_mkdirs(dir));
    pal_path_join(f, sizeof f, dir, "e_combined.xaml");
    CHECK(write_str(f, "<ps:SimpleGeometryShape xmlns:ps=\"clr-namespace:PaintDotNet.Shapes\""
                       " DisplayName=\"Combined Exclude\"><ps:SimpleGeometryShape.Geometry>"
                       "<CombinedGeometry GeometryCombineMode=\"Exclude\">"
                       "<CombinedGeometry.Geometry1><RectangleGeometry Rect=\"0,0,100,100\"/>"
                       "</CombinedGeometry.Geometry1><CombinedGeometry.Geometry2>"
                       "<EllipseGeometry Center=\"50,50\" RadiusX=\"30\" RadiusY=\"30\"/>"
                       "</CombinedGeometry.Geometry2></CombinedGeometry>"
                       "</ps:SimpleGeometryShape.Geometry></ps:SimpleGeometryShape>"));
    pal_path_join(f, sizeof f, dir, "a_geom.xaml");
    CHECK(write_str(f, "<S DisplayName=\"tetris\" Geometry=\"F1 M 0,0 L 2,0 L 2,1 L 1,1 L 1,2"
                       " L 0,2 Z\"/>"));
    pal_path_join(f, sizeof f, dir, "Zebra.xaml");
    CHECK(write_str(f, "<S DisplayName=\"Zebra\" Geometry=\"M 0,0 L 3,0 L 3,1 Z\"/>"));
    pal_path_join(f, sizeof f, dir, "UPPER.xaml");
    CHECK(write_str(f, "<S DisplayName=\"Upper Ext\" Geometry=\"M 0,0 L 1,0 L 1,3 Z\"/>"));
    a = at_app(900, 700);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(vec_custom_load_dir(a, dir) == 4);
    /* item 33: sorted without regard to case */
    CHECK(vec_custom_count(a) == 4);
    if (vec_custom_count(a) == 4) {
        CHECK(strcmp(vec_custom_at(a, 0)->name, "Combined Exclude") == 0);
        CHECK(strcmp(vec_custom_at(a, 1)->name, "tetris") == 0);
        CHECK(strcmp(vec_custom_at(a, 2)->name, "Upper Ext") == 0);
        CHECK(strcmp(vec_custom_at(a, 3)->name, "Zebra") == 0);
    }
    /* item 27, the repro: drawn filled from 50,50 to 250,250 */
    d = app_doc_new_image(a, 300, 300, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    CHECK(app_tool_select(a, "shapes"));
    idx = vec_custom_find(a, "Combined Exclude");
    CHECK(idx == 0);
    app_settings_set(app_settings_of(a), "tool.shapes.custom", "Combined Exclude");
    app_settings_set(app_settings_of(a), "tool.shapes.kind", "29");
    app_settings_set(app_settings_of(a), "tool.shapes.draw", "1");
    app_tool_settings_changed(a);
    at_frames(a, 1);
    at_drag(a, 50.0, 50.0, 250.0, 250.0, 6, SDL_BUTTON_LEFT);
    CHECK(px_eq(at_doc_px(a, 150, 150), 255, 255, 255, 255));    /* the hole */
    CHECK(px_eq(at_doc_px(a, 60, 60), 0, 0, 0, 255));            /* the square */
    CHECK(px_eq(at_doc_px(a, 150, 60), 0, 0, 0, 255));
    CHECK(px_eq(at_doc_px(a, 150, 95), 255, 255, 255, 255));     /* inside the circle */
    CHECK(px_eq(at_doc_px(a, 30, 30), 255, 255, 255, 255));      /* outside the shape */
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_combined_modes);
    RUN(t_verbose_figures);
    RUN(t_transforms);
    RUN(t_garbage);
    RUN(t_folder);
    at_quit();
    return pc_test_finish();
}
