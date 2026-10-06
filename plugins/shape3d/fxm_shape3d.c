/* fxm_shape3d.c - Shape3D (Effects > Render > Shape3D), an optional paint.c
 * effect plugin (plugins/shape3d/README.md).
 *
 * Design after the Paint.NET plugin "Shape3D" by MKT (later fixes by MJW and
 * toe_head2001), reimplemented clean room from the plugin's public manual,
 * forum posts and dialog screenshots; no code, binary or asset of the
 * original was used.
 *
 * The selection bounds S of the active layer are the texture and the render
 * area. The texture is wrapped onto a sphere (ellipsoid), a cylinder (open,
 * flat or ball ends) or a box (any of its six faces), rotated by three
 * rotations about chosen global axes, panned, and rendered with a
 * perspective camera, Lambert lighting with an ambient term, a Phong or
 * Cook-Torrance highlight, optional see-through rendering and supersampled
 * antialiasing. The output replaces S: the shape over transparency.
 *
 * Geometry (right handed: x right, y up, z toward the viewer). One unit is
 * R0 = 0.45 * min(S.w, S.h) pixels on the z = 0 plane. The camera sits at
 * (0, 0, d), d = 1 / tan(camera / 2), and looks along -z; pixel (px, py)
 * shoots the ray toward ((px - cx) / R0, -(py - cy) / R0, 0). Objects are
 * intersected analytically in object space (M^T (P - T), M = R3 R2 R1);
 * only hits in front of the camera count, and every hit along a ray is
 * composited front to back with the texture's alpha as the surface's
 * coverage (holes in the texture show the inside). Inner surfaces get the
 * normal flipped toward the viewer. Rounded edges (cylinder rims next to
 * flat ends, box edges) bend the normal only, within round / 100 units of an
 * edge, by up to 45 degrees at the edge.
 *
 * Texture maps work on the normalized hit point q = p / extents, so every
 * map works on every shape: full and half sphere maps (longitude and
 * latitude), full and half cylinder maps (longitude, and the length along
 * the surface from the top center to the bottom center), plane maps (the
 * scalable one is transparent outside the image), a cube map, and the dice
 * maps (a 4 x 3 net; "float" sizes the cells by the box proportions).
 *
 * Shading: rgb = texel * (ambient + diffuse * s * max(0, N.L) * Lc), plus
 * spec_rate * s * Lc * S where N.L > 0, with S = max(0, R.V)^phong (Phong)
 * or F D G / (4 N.V) (Cook-Torrance: exact dielectric Fresnel, Beckmann
 * distribution, isotropic or anisotropic along the texture's u direction).
 * Values are gamma-encoded 0..1 (no linearization) and clamped.
 *
 * Antialiasing level n shoots (n + 1)^2 rays per pixel on a regular grid;
 * the pixel is the mean of the premultiplied samples, rounded, so fully
 * covered pixels are exactly opaque.
 *
 * Output is a pure function of (params, src, env, pixel): any ROI split and
 * thread count give the same bytes. render() polls cancellation once per
 * row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments. Ownership: the effect structs are static and stay
 * valid until the library is unloaded; there is no state.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

#ifndef FXP_F_COLOR_NO_ALPHA
#  define FXP_F_COLOR_NO_ALPHA 8u   /* fx_abi_ext.h: color without alpha */
#endif

#define S3D_PI 3.14159265358979323846

enum { SHAPE_SPHERE = 0, SHAPE_CYLINDER = 1, SHAPE_BOX = 2 };
enum { ENDS_OPEN = 0, ENDS_FLAT = 1, ENDS_BALL = 2 };
enum {
    MAP_SPHERE = 0, MAP_HALF_SPHERE, MAP_HALF_SPHERE_REP, MAP_CYL, MAP_HALF_CYL,
    MAP_HALF_CYL_REP, MAP_PLANE, MAP_PLANE_SCALE, MAP_CUBE, MAP_DICE, MAP_DICE_FLOAT
};
/* Box faces, in the order of the face props. */
enum { FACE_FRONT = 0, FACE_REAR, FACE_TOP, FACE_BOTTOM, FACE_LEFT, FACE_RIGHT };
/* Parts of a hit. */
enum { PART_BODY = 0, PART_CAP_TOP, PART_CAP_BOT, PART_DOME_TOP, PART_DOME_BOT };

typedef struct s3d_params {
    double   scale, size_x, size_y, size_z, round, ball_h, tex_scale;
    double   rot1, rot2, rot3;
    double   pan[2];
    double   camera;
    double   light_strength, light_x, light_y, light_z;
    double   ambient, diffuse, spec_rate, ior, rough, rough_x, rough_y;
    int32_t  shape, ends;
    int32_t  face[6];
    int32_t  map, tex_rot;
    int32_t  rot1_axis, rot2_axis, rot3_axis;
    int32_t  aa, aa_level, transp, transp_alpha, light, spec, spec_model, phong, luster;
    uint32_t light_color;
} s3d_params;

static const char *const k_shapes[] = { "Sphere", "Cylinder", "Box", NULL };
static const char *const k_ends[] = { "Open", "Flat", "Ball", NULL };
static const char *const k_maps[] = {
    "Full sphere map", "Half sphere map", "Half sphere map (repeat)", "Full cylinder map",
    "Half cylinder map", "Half cylinder map (repeat)", "Plane map", "Plane map (scalable)",
    "Cube map", "Dice map", "Dice map (float)", NULL
};
static const char *const k_texrot[] = { "0 degrees", "90 degrees", "180 degrees",
                                        "270 degrees", NULL };
static const char *const k_axes[] = { "X", "Y", "Z", NULL };
static const char *const k_models[] = { "Phong", "Cook-Torrance", NULL };
static const char *const k_luster[] = { "Isotropy", "Anisotropy", NULL };

#define OFS(f) (uint32_t)offsetof(s3d_params, f)
#define REAL(k, l, f, lo, hi, d, st, en) \
    { k, l, FXP_REAL, OFS(f), lo, hi, d, st, NULL, NULL, 0u, 0u, en }
#define BOOLP(k, l, f, d, tip, en) \
    { k, l, FXP_BOOL, OFS(f), 0.0, 1.0, d, 0.0, NULL, tip, 0u, 0u, en }
#define CHOICE(k, l, f, list, n, d, tip, en) \
    { k, l, FXP_CHOICE, OFS(f), 0.0, (double)(n) - 1.0, d, 0.0, list, tip, 0u, 0u, en }

static const fx_prop k_props[] = {
    CHOICE("shape", "Shape", shape, k_shapes, 3, 0.0, "tip:The solid the image is wrapped on",
           NULL),
    REAL("scale", "Scaling", scale, 0.01, 5.0, 1.0, 0.01, NULL),
    REAL("size_x", "Width (horizontal radius)", size_x, 0.01, 5.0, 1.0, 0.01, NULL),
    REAL("size_y", "Height (vertical radius)", size_y, 0.01, 5.0, 1.0, 0.01, NULL),
    REAL("size_z", "Depth (depth radius)", size_z, 0.01, 5.0, 1.0, 0.01, NULL),
    REAL("round", "Rounded edges", round, 0.0, 10.0, 0.0, 0.1, NULL),
    CHOICE("ends", "Cylinder ends", ends, k_ends, 3, 1.0,
           "tip:Open leaves a tube, Flat closes it, Ball puts a dome on each end", "shape=1"),
    REAL("ball_h", "Height of ball", ball_h, 0.01, 5.0, 0.5, 0.01, "ends=2"),
    BOOLP("face_front", "Front face", face[FACE_FRONT], 1.0, NULL, "shape=2"),
    BOOLP("face_rear", "Rear face", face[FACE_REAR], 1.0, NULL, "shape=2"),
    BOOLP("face_top", "Top face", face[FACE_TOP], 1.0, NULL, "shape=2"),
    BOOLP("face_bottom", "Bottom face", face[FACE_BOTTOM], 1.0, NULL, "shape=2"),
    BOOLP("face_left", "Left face", face[FACE_LEFT], 1.0, NULL, "shape=2"),
    BOOLP("face_right", "Right face", face[FACE_RIGHT], 1.0, NULL, "shape=2"),
    CHOICE("map", "Texture map", map, k_maps, 11, 0.0,
           "tip:How the image is laid onto the surface", NULL),
    REAL("tex_scale", "Texture scale", tex_scale, 0.01, 10.0, 1.0, 0.01, "map=7"),
    CHOICE("tex_rot", "Texture rotate", tex_rot, k_texrot, 4, 0.0,
           "tip:Turns the image clockwise before it is mapped", NULL),
    CHOICE("rot1_axis", "Rotation 1 axis", rot1_axis, k_axes, 3, 0.0, NULL, NULL),
    { "rot1", "Rotation 1 angle", FXP_ANGLE, OFS(rot1), -180.0, 180.0, 0.0, 0.0, NULL, NULL, 0u,
      0u, NULL },
    CHOICE("rot2_axis", "Rotation 2 axis", rot2_axis, k_axes, 3, 1.0, NULL, NULL),
    { "rot2", "Rotation 2 angle", FXP_ANGLE, OFS(rot2), -180.0, 180.0, 0.0, 0.0, NULL, NULL, 0u,
      0u, NULL },
    CHOICE("rot3_axis", "Rotation 3 axis", rot3_axis, k_axes, 3, 2.0, NULL, NULL),
    { "rot3", "Rotation 3 angle", FXP_ANGLE, OFS(rot3), -180.0, 180.0, 0.0, 0.0, NULL, NULL, 0u,
      0u, NULL },
    { "pan", "Pan", FXP_POINT, OFS(pan), -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    REAL("camera", "Camera angle", camera, 1.0, 90.0, 23.0, 0.1, NULL),
    BOOLP("aa", "Antialiasing", aa, 0.0, "tip:Smooth the outline with several rays per pixel",
          NULL),
    { "aa_level", "Antialiasing level", FXP_INT, OFS(aa_level), 1.0, 5.0, 1.0, 1.0, NULL, NULL,
      0u, 0u, "aa" },
    BOOLP("transp", "Transparency", transp, 0.0,
          "tip:See through the front surface to the inside of the back", NULL),
    { "transp_alpha", "Alpha channel", FXP_INT, OFS(transp_alpha), 0.0, 255.0, 208.0, 1.0, NULL,
      NULL, 0u, 0u, "transp" },
    BOOLP("light", "Lighting", light, 1.0, "tip:Off shows the texture without shading", NULL),
    REAL("light_strength", "Strength of light", light_strength, 0.0, 10.0, 1.0, 0.01, "light"),
    REAL("light_x", "Light direction X", light_x, -1.0, 1.0, -0.75, 0.01, "light"),
    REAL("light_y", "Light direction Y", light_y, -1.0, 1.0, 1.0, 0.01, "light"),
    REAL("light_z", "Light direction Z", light_z, -1.0, 1.0, 0.5, 0.01, "light"),
    { "light_color", "Light color", FXP_COLOR, OFS(light_color), 0.0, 4294967295.0,
      4294967295.0, 0.0, NULL, NULL, 0u, FXP_F_COLOR_NO_ALPHA, "light" },
    REAL("ambient", "Ambient lighting", ambient, 0.0, 10.0, 0.15, 0.01, "light"),
    REAL("diffuse", "Diffuse reflection rate", diffuse, 0.0, 10.0, 1.0, 0.01, "light"),
    BOOLP("spec", "Specular highlight", spec, 1.0, NULL, "light"),
    REAL("spec_rate", "Specular reflection rate", spec_rate, 0.0, 10.0, 0.3, 0.01, "spec"),
    CHOICE("spec_model", "Specular model", spec_model, k_models, 2, 0.0,
           "tip:Phong is a simple highlight, Cook-Torrance a physically based one", "spec"),
    { "phong", "Phong size", FXP_INT, OFS(phong), 1.0, 1000.0, 50.0, 1.0, NULL, NULL, 0u, 0u,
      "spec_model=0" },
    REAL("ior", "Refractive index", ior, 1.0, 10.0, 1.4, 0.01, "spec_model=1"),
    CHOICE("luster", "Luster", luster, k_luster, 2, 1.0,
           "tip:Anisotropy stretches the highlight along the texture's horizontal direction",
           "spec_model=1"),
    REAL("rough", "Roughness", rough, 0.01, 1.0, 0.2, 0.01, "luster=0"),
    REAL("rough_x", "Roughness X", rough_x, 0.01, 1.0, 0.3, 0.01, "luster=1"),
    REAL("rough_y", "Roughness Y", rough_y, 0.01, 1.0, 0.15, 0.01, "luster=1"),
};

/* ---- small vector math ---------------------------------------------------------- */
typedef struct v3 { double x, y, z; } v3;

static v3 v3m(double x, double y, double z)
{
    v3 r;
    r.x = x; r.y = y; r.z = z;
    return r;
}
static v3 v3add(v3 a, v3 b) { return v3m(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 v3sub(v3 a, v3 b) { return v3m(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 v3mul(v3 a, double k) { return v3m(a.x * k, a.y * k, a.z * k); }
static double v3dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 v3cross(v3 a, v3 b)
{
    return v3m(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static v3 v3norm(v3 a)
{
    double l = sqrt(v3dot(a, a));
    return l > 0.0 ? v3mul(a, 1.0 / l) : a;
}
static double v3get(v3 a, int i) { return i == 0 ? a.x : (i == 1 ? a.y : a.z); }
static v3 v3set(v3 a, int i, double v)
{
    if (i == 0) a.x = v;
    else if (i == 1) a.y = v;
    else a.z = v;
    return a;
}

typedef struct m3 { double m[3][3]; } m3;

static v3 m3mul(const m3 *a, v3 v)
{
    return v3m(a->m[0][0] * v.x + a->m[0][1] * v.y + a->m[0][2] * v.z,
               a->m[1][0] * v.x + a->m[1][1] * v.y + a->m[1][2] * v.z,
               a->m[2][0] * v.x + a->m[2][1] * v.y + a->m[2][2] * v.z);
}
static v3 m3tmul(const m3 *a, v3 v)                    /* transpose times v */
{
    return v3m(a->m[0][0] * v.x + a->m[1][0] * v.y + a->m[2][0] * v.z,
               a->m[0][1] * v.x + a->m[1][1] * v.y + a->m[2][1] * v.z,
               a->m[0][2] * v.x + a->m[1][2] * v.y + a->m[2][2] * v.z);
}
static m3 m3prod(const m3 *a, const m3 *b)
{
    m3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] +
                        a->m[i][2] * b->m[2][j];
    return r;
}
/* Right-handed rotation by deg about axis (0 x, 1 y, 2 z): counter-clockwise
 * when looking down the axis toward the origin. */
static m3 m3rot(int axis, double deg)
{
    double a = deg * (S3D_PI / 180.0), c = cos(a), s = sin(a);
    m3 r = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    r.m[i][i] = c;
    r.m[i][j] = -s;
    r.m[j][i] = s;
    r.m[j][j] = c;
    return r;
}

/* ---- the scene ------------------------------------------------------------------ */
typedef struct scene {
    fx_rect S;                   /* texture and render area */
    double  r0, cx, cy, d;       /* pixels per unit, center, camera distance */
    m3      M;                   /* object to world rotation */
    v3      T;                   /* object center in world units */
    v3      e;                   /* extents */
    double  bh;                  /* dome height (ball ends) */
    double  rr;                  /* rounded edge width in units */
    int     shape, ends, faces, map, tex_rot;
    double  tw, th;              /* texture size after rotation, pixels */
    double  pa, pb;              /* scalable plane map half size times tex_scale */
    double  len_top, len_side, len_bot, len_all;   /* cylinder map shares */
    /* shading */
    int     light, spec, model, aniso, phong;
    v3      L, Lc;
    double  strength, ambient, diffuse, spec_rate, ior, rough, rough_x, rough_y;
    int     transp;
    double  transp_k;            /* front opacity factor */
} scene;

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int32_t v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : (int)v); }

/* Quarter of the perimeter of an ellipse with semi axes a and b (Ramanujan). */
static double quarter_arc(double a, double b)
{
    return 0.25 * S3D_PI * (3.0 * (a + b) - sqrt((3.0 * a + b) * (a + 3.0 * b)));
}

static void scene_init(scene *sc, const s3d_params *p, const fx_img *src, const fx_env *env)
{
    fx_rect s = env->sel, r = src->r;
    int32_t x0 = s.x > r.x ? s.x : r.x, y0 = s.y > r.y ? s.y : r.y;
    int64_t x1 = (int64_t)s.x + s.w, y1 = (int64_t)s.y + s.h;
    double scale = clampd(p->scale, 0.01, 5.0), cam = clampd(p->camera, 1.0, 90.0);
    m3 r1, r2, r3, t;
    if ((int64_t)r.x + r.w < x1) x1 = (int64_t)r.x + r.w;
    if ((int64_t)r.y + r.h < y1) y1 = (int64_t)r.y + r.h;
    sc->S.x = x0;
    sc->S.y = y0;
    sc->S.w = x1 > x0 ? (int32_t)(x1 - x0) : 0;
    sc->S.h = y1 > y0 ? (int32_t)(y1 - y0) : 0;
    sc->r0 = 0.45 * (double)(sc->S.w < sc->S.h ? sc->S.w : sc->S.h);
    if (sc->r0 <= 0.0) sc->r0 = 0.45;
    sc->cx = sc->S.x + sc->S.w * 0.5;
    sc->cy = sc->S.y + sc->S.h * 0.5;
    sc->d = 1.0 / tan(cam * (S3D_PI / 360.0));
    r1 = m3rot(clampi(p->rot1_axis, 0, 2), p->rot1);
    r2 = m3rot(clampi(p->rot2_axis, 0, 2), p->rot2);
    r3 = m3rot(clampi(p->rot3_axis, 0, 2), p->rot3);
    t = m3prod(&r2, &r1);
    sc->M = m3prod(&r3, &t);
    sc->T = v3m(clampd(p->pan[0], -2.0, 2.0) * sc->S.w * 0.5 / sc->r0,
                -clampd(p->pan[1], -2.0, 2.0) * sc->S.h * 0.5 / sc->r0, 0.0);
    sc->e = v3m(scale * clampd(p->size_x, 0.01, 5.0), scale * clampd(p->size_y, 0.01, 5.0),
                scale * clampd(p->size_z, 0.01, 5.0));
    sc->bh = scale * clampd(p->ball_h, 0.01, 5.0);
    sc->rr = clampd(p->round, 0.0, 10.0) / 100.0;
    sc->shape = clampi(p->shape, 0, 2);
    sc->ends = clampi(p->ends, 0, 2);
    sc->faces = 0;
    for (int i = 0; i < 6; i++)
        if (p->face[i]) sc->faces |= 1 << i;
    sc->map = clampi(p->map, 0, 10);
    sc->tex_rot = clampi(p->tex_rot, 0, 3);
    sc->tw = (sc->tex_rot & 1) ? sc->S.h : sc->S.w;
    sc->th = (sc->tex_rot & 1) ? sc->S.w : sc->S.h;
    {
        double ts = clampd(p->tex_scale, 0.01, 10.0);
        double tw = sc->tw > 0 ? sc->tw : 1.0, th = sc->th > 0 ? sc->th : 1.0;
        if (tw >= th) {
            sc->pa = scale * ts;
            sc->pb = scale * ts * th / tw;
        } else {
            sc->pb = scale * ts;
            sc->pa = scale * ts * tw / th;
        }
    }
    /* cylinder map: lengths along the surface, top center to bottom center */
    {
        double rc = 0.5 * (sc->e.x + sc->e.z), cap = 0.0;
        if (sc->shape == SHAPE_BOX) cap = rc;
        else if (sc->shape == SHAPE_CYLINDER && sc->ends == ENDS_FLAT) cap = rc;
        else if (sc->shape == SHAPE_CYLINDER && sc->ends == ENDS_BALL) cap = quarter_arc(rc, sc->bh);
        sc->len_top = cap;
        sc->len_bot = cap;
        sc->len_side = 2.0 * sc->e.y;
        sc->len_all = sc->len_top + sc->len_side + sc->len_bot;
    }
    sc->light = p->light != 0;
    sc->spec = p->spec != 0;
    sc->model = clampi(p->spec_model, 0, 1);
    sc->aniso = clampi(p->luster, 0, 1) == 1;
    sc->phong = clampi(p->phong, 1, 1000);
    sc->L = v3m(clampd(p->light_x, -1.0, 1.0), clampd(p->light_y, -1.0, 1.0),
                clampd(p->light_z, -1.0, 1.0));
    if (v3dot(sc->L, sc->L) <= 0.0) sc->L = v3m(0.0, 0.0, 1.0);
    sc->L = v3norm(sc->L);
    sc->Lc = v3m(((p->light_color >> 16) & 255u) / 255.0, ((p->light_color >> 8) & 255u) / 255.0,
                 (p->light_color & 255u) / 255.0);
    sc->strength = clampd(p->light_strength, 0.0, 10.0);
    sc->ambient = clampd(p->ambient, 0.0, 10.0);
    sc->diffuse = clampd(p->diffuse, 0.0, 10.0);
    sc->spec_rate = clampd(p->spec_rate, 0.0, 10.0);
    sc->ior = clampd(p->ior, 1.0, 10.0);
    sc->rough = clampd(p->rough, 0.01, 1.0);
    sc->rough_x = clampd(p->rough_x, 0.01, 1.0);
    sc->rough_y = clampd(p->rough_y, 0.01, 1.0);
    sc->transp = p->transp != 0;
    sc->transp_k = clampi(p->transp_alpha, 0, 255) / 255.0;
}

/* ---- intersection --------------------------------------------------------------- */
#define MAX_HITS 8
#define T_EPS 1e-9

typedef struct hit {
    double t;
    v3     p;                    /* object-space point */
    v3     n;                    /* object-space normal (not normalized) */
    int    part;                 /* PART_* */
    int    face;                 /* box face, -1 otherwise */
} hit;

typedef struct hits {
    hit h[MAX_HITS];
    int n;
} hits;

static void add_hit(hits *hs, double t, v3 o, v3 dir, int part, int face, v3 n)
{
    hit *h;
    if (!(t > T_EPS) || hs->n >= MAX_HITS) return;
    h = &hs->h[hs->n++];
    h->t = t;
    h->p = v3add(o, v3mul(dir, t));
    h->n = n;
    h->part = part;
    h->face = face;
}

/* Roots of a t^2 + b t + c = 0 in ascending order; returns their count. */
static int quadratic(double a, double b, double c, double *t0, double *t1)
{
    double disc, q;
    if (fabs(a) < 1e-300) return 0;
    disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return 0;
    q = -0.5 * (b + (b >= 0.0 ? sqrt(disc) : -sqrt(disc)));
    if (q == 0.0) {
        *t0 = *t1 = 0.0;
        return 1;
    }
    *t0 = q / a;
    *t1 = c / q;
    if (*t0 > *t1) {
        double s = *t0;
        *t0 = *t1;
        *t1 = s;
    }
    return 2;
}

/* Ellipsoid centered at (0, cy, 0) with semi axes (a, b, c). side: 0 all,
 * +1 only y >= cy, -1 only y <= cy. */
static void hit_ellipsoid(hits *hs, v3 o, v3 dir, double cy, v3 ax, int side, int part)
{
    v3 os = v3m(o.x / ax.x, (o.y - cy) / ax.y, o.z / ax.z);
    v3 ds = v3m(dir.x / ax.x, dir.y / ax.y, dir.z / ax.z);
    double t[2];
    int n = quadratic(v3dot(ds, ds), 2.0 * v3dot(os, ds), v3dot(os, os) - 1.0, &t[0], &t[1]);
    for (int i = 0; i < n; i++) {
        v3 p = v3add(o, v3mul(dir, t[i]));
        if (side > 0 && p.y < cy) continue;
        if (side < 0 && p.y > cy) continue;
        add_hit(hs, t[i], o, dir, part, -1,
                v3m(p.x / (ax.x * ax.x), (p.y - cy) / (ax.y * ax.y), p.z / (ax.z * ax.z)));
    }
}

static void intersect(const scene *sc, v3 o, v3 dir, hits *hs)
{
    v3 e = sc->e;
    hs->n = 0;
    if (sc->shape == SHAPE_SPHERE) {
        hit_ellipsoid(hs, o, dir, 0.0, e, 0, PART_BODY);
    } else if (sc->shape == SHAPE_CYLINDER) {
        double a = (dir.x / e.x) * (dir.x / e.x) + (dir.z / e.z) * (dir.z / e.z);
        double b = 2.0 * (o.x * dir.x / (e.x * e.x) + o.z * dir.z / (e.z * e.z));
        double c = (o.x / e.x) * (o.x / e.x) + (o.z / e.z) * (o.z / e.z) - 1.0, t[2];
        int n = quadratic(a, b, c, &t[0], &t[1]);
        for (int i = 0; i < n; i++) {
            v3 p = v3add(o, v3mul(dir, t[i]));
            if (fabs(p.y) <= e.y)
                add_hit(hs, t[i], o, dir, PART_BODY, -1,
                        v3m(p.x / (e.x * e.x), 0.0, p.z / (e.z * e.z)));
        }
        if (sc->ends == ENDS_FLAT && dir.y != 0.0) {
            for (int s = -1; s <= 1; s += 2) {
                double tc = (s * e.y - o.y) / dir.y;
                v3 p = v3add(o, v3mul(dir, tc));
                if ((p.x / e.x) * (p.x / e.x) + (p.z / e.z) * (p.z / e.z) <= 1.0)
                    add_hit(hs, tc, o, dir, s > 0 ? PART_CAP_TOP : PART_CAP_BOT, -1,
                            v3m(0.0, (double)s, 0.0));
            }
        } else if (sc->ends == ENDS_BALL) {
            hit_ellipsoid(hs, o, dir, e.y, v3m(e.x, sc->bh, e.z), 1, PART_DOME_TOP);
            hit_ellipsoid(hs, o, dir, -e.y, v3m(e.x, sc->bh, e.z), -1, PART_DOME_BOT);
        }
    } else {
        static const int axis[6] = { 2, 2, 1, 1, 0, 0 };
        static const int sgn[6] = { 1, -1, 1, -1, -1, 1 };
        for (int f = 0; f < 6; f++) {
            int a = axis[f], b = (a + 1) % 3, c = (a + 2) % 3;
            double da = v3get(dir, a), tc;
            v3 p;
            if (!(sc->faces & (1 << f)) || da == 0.0) continue;
            tc = (sgn[f] * v3get(e, a) - v3get(o, a)) / da;
            p = v3add(o, v3mul(dir, tc));
            if (fabs(v3get(p, b)) <= v3get(e, b) && fabs(v3get(p, c)) <= v3get(e, c))
                add_hit(hs, tc, o, dir, PART_BODY, f, v3set(v3m(0.0, 0.0, 0.0), a, (double)sgn[f]));
        }
    }
    /* sort by distance (insertion sort, stable) and drop duplicates at edges */
    for (int i = 1; i < hs->n; i++) {
        hit h = hs->h[i];
        int j = i - 1;
        while (j >= 0 && hs->h[j].t > h.t) {
            hs->h[j + 1] = hs->h[j];
            j--;
        }
        hs->h[j + 1] = h;
    }
    {
        int k = 0;
        for (int i = 0; i < hs->n; i++)
            if (k == 0 || hs->h[i].t - hs->h[k - 1].t > 1e-9 * (1.0 + hs->h[i].t)) hs->h[k++] = hs->h[i];
        hs->n = k;
    }
}

/* ---- rounded edges -------------------------------------------------------------- */
/* n (unit) bent toward nb (unit) for a point dist from the edge. */
static v3 bend(v3 acc, v3 nb, double dist, double r)
{
    double th;
    if (!(r > 0.0) || dist >= r) return acc;
    if (dist < 0.0) dist = 0.0;
    th = (S3D_PI / 4.0) * (1.0 - dist / r);
    return v3add(acc, v3mul(nb, tan(th)));
}

static v3 rounded_normal(const scene *sc, const hit *h)
{
    v3 n = v3norm(h->n), e = sc->e, p = h->p;
    double r = sc->rr;
    if (!(r > 0.0)) return n;
    if (sc->shape == SHAPE_BOX && h->face >= 0) {
        static const int axis[6] = { 2, 2, 1, 1, 0, 0 };
        int a = axis[h->face];
        v3 acc = n;
        for (int k = 1; k <= 2; k++) {
            int b = (a + k) % 3;
            double pb = v3get(p, b);
            acc = bend(acc, v3set(v3m(0.0, 0.0, 0.0), b, pb >= 0.0 ? 1.0 : -1.0),
                       v3get(e, b) - fabs(pb), r);
        }
        return v3norm(acc);
    }
    if (sc->shape == SHAPE_CYLINDER && sc->ends == ENDS_FLAT) {
        if (h->part == PART_BODY)
            return v3norm(bend(n, v3m(0.0, p.y >= 0.0 ? 1.0 : -1.0, 0.0), e.y - fabs(p.y), r));
        if (h->part == PART_CAP_TOP || h->part == PART_CAP_BOT) {
            double gx = p.x / (e.x * e.x), gz = p.z / (e.z * e.z);
            double rho = sqrt((p.x / e.x) * (p.x / e.x) + (p.z / e.z) * (p.z / e.z));
            double gl = sqrt(gx * gx + gz * gz);
            if (rho > 1e-12 && gl > 0.0)
                return v3norm(bend(n, v3norm(v3m(gx, 0.0, gz)), (1.0 - rho) * rho / gl, r));
        }
    }
    return n;
}

/* ---- texture mapping ------------------------------------------------------------ */
/* Texture coordinates of a hit. Returns 0 when the hit is hidden by the map
 * (half maps) or falls outside the image (scalable plane map). *tan is the
 * object-space direction of increasing u (for anisotropic highlights). */
static int map_uv(const scene *sc, const hit *h, double *u, double *v, v3 *tan_o)
{
    v3 p = h->p, e = sc->e;
    v3 q = v3m(p.x / e.x, p.y / e.y, p.z / e.z);
    double lon = atan2(q.x, q.z), ql = sqrt(v3dot(q, q));
    double lat = ql > 0.0 ? asin(clampd(q.y / ql, -1.0, 1.0)) : 0.0;
    v3 tlon = v3norm(v3m(cos(lon), 0.0, -sin(lon)));
    switch (sc->map) {
    case MAP_SPHERE:
        *u = 0.5 + lon / (2.0 * S3D_PI);
        *v = 0.5 - lat / S3D_PI;
        *tan_o = tlon;
        return 1;
    case MAP_HALF_SPHERE:
        if (q.y < 0.0) return 0;
        *u = 0.5 + lon / (2.0 * S3D_PI);
        *v = 1.0 - 2.0 * lat / S3D_PI;
        *tan_o = tlon;
        return 1;
    case MAP_HALF_SPHERE_REP:
        *u = 0.5 + lon / (2.0 * S3D_PI);
        *v = 1.0 - 2.0 * fabs(lat) / S3D_PI;
        *tan_o = tlon;
        return 1;
    case MAP_CYL: case MAP_HALF_CYL: case MAP_HALF_CYL_REP: {
        /* v: length along the surface from the top center */
        int part = h->part;
        double s;
        if (sc->shape == SHAPE_BOX && h->face == FACE_TOP) part = PART_CAP_TOP;
        if (sc->shape == SHAPE_BOX && h->face == FACE_BOTTOM) part = PART_CAP_BOT;
        if (part == PART_CAP_TOP || part == PART_CAP_BOT) {
            double rho = clampd(sqrt(q.x * q.x + q.z * q.z), 0.0, 1.0);
            s = part == PART_CAP_TOP ? rho * sc->len_top
                                     : sc->len_top + sc->len_side + (1.0 - rho) * sc->len_bot;
        } else if (part == PART_DOME_TOP || part == PART_DOME_BOT) {
            double yn = (p.y - (part == PART_DOME_TOP ? e.y : -e.y)) / sc->bh;
            double dx = p.x / e.x, dz = p.z / e.z, dl = sqrt(dx * dx + dz * dz + yn * yn);
            double polar = dl > 0.0 ? acos(clampd(fabs(yn) / dl, 0.0, 1.0)) : 0.0;
            double f = polar / (S3D_PI / 2.0);
            s = part == PART_DOME_TOP ? f * sc->len_top
                                      : sc->len_top + sc->len_side + (1.0 - f) * sc->len_bot;
        } else {
            s = sc->len_top + clampd((1.0 - q.y) * 0.5, 0.0, 1.0) * sc->len_side;
        }
        *v = sc->len_all > 0.0 ? s / sc->len_all : 0.5;
        *tan_o = tlon;
        if (sc->map == MAP_CYL) {
            *u = 0.5 + lon / (2.0 * S3D_PI);
        } else if (lon >= -S3D_PI / 2.0 && lon <= S3D_PI / 2.0) {
            *u = 0.5 + lon / S3D_PI;
        } else {
            if (sc->map == MAP_HALF_CYL) return 0;                 /* back half hidden */
            *u = lon > 0.0 ? 0.5 - (lon - S3D_PI) / S3D_PI : 0.5 - (lon + S3D_PI) / S3D_PI;
            *tan_o = v3mul(tlon, -1.0);
        }
        return 1;
    }
    case MAP_PLANE:
        *u = 0.5 + q.x * 0.5;
        *v = 0.5 - q.y * 0.5;
        *tan_o = v3m(1.0, 0.0, 0.0);
        return 1;
    case MAP_PLANE_SCALE:
        *u = 0.5 + p.x / (2.0 * sc->pa);
        *v = 0.5 - p.y / (2.0 * sc->pb);
        *tan_o = v3m(1.0, 0.0, 0.0);
        return *u >= 0.0 && *u <= 1.0 && *v >= 0.0 && *v <= 1.0;
    default: {
        /* cube and dice maps: the face of the dominant axis of q */
        int f = h->face;
        double fu, fv, ax = fabs(q.x), ay = fabs(q.y), az = fabs(q.z), m;
        if (sc->shape != SHAPE_BOX || f < 0) {
            if (az >= ax && az >= ay) f = q.z >= 0.0 ? FACE_FRONT : FACE_REAR;
            else if (ax >= ay) f = q.x >= 0.0 ? FACE_RIGHT : FACE_LEFT;
            else f = q.y >= 0.0 ? FACE_TOP : FACE_BOTTOM;
        }
        m = f <= FACE_REAR ? az : (f <= FACE_BOTTOM ? ay : ax);
        if (m <= 0.0) m = 1.0;
        switch (f) {
        case FACE_FRONT:  fu = 0.5 + q.x / (2 * m); fv = 0.5 - q.y / (2 * m); *tan_o = v3m(1, 0, 0); break;
        case FACE_REAR:   fu = 0.5 - q.x / (2 * m); fv = 0.5 - q.y / (2 * m); *tan_o = v3m(-1, 0, 0); break;
        case FACE_RIGHT:  fu = 0.5 - q.z / (2 * m); fv = 0.5 - q.y / (2 * m); *tan_o = v3m(0, 0, -1); break;
        case FACE_LEFT:   fu = 0.5 + q.z / (2 * m); fv = 0.5 - q.y / (2 * m); *tan_o = v3m(0, 0, 1); break;
        case FACE_TOP:    fu = 0.5 + q.x / (2 * m); fv = 0.5 + q.z / (2 * m); *tan_o = v3m(1, 0, 0); break;
        default:          fu = 0.5 + q.x / (2 * m); fv = 0.5 - q.z / (2 * m); *tan_o = v3m(1, 0, 0); break;
        }
        fu = clampd(fu, 0.0, 1.0);
        fv = clampd(fv, 0.0, 1.0);
        if (sc->map == MAP_CUBE) {
            *u = fu;
            *v = fv;
        } else {
            /* the dice net: row 1 = left, front, right, rear; top above and
             * bottom below the front */
            static const int col[6] = { 1, 3, 1, 1, 0, 2 };
            static const int row[6] = { 1, 1, 0, 2, 1, 1 };
            double cw[4], rh[3], c0 = 0.0, r0 = 0.0, wsum, hsum;
            if (sc->map == MAP_DICE_FLOAT) {
                cw[0] = e.z; cw[1] = e.x; cw[2] = e.z; cw[3] = e.x;
                rh[0] = e.z; rh[1] = e.y; rh[2] = e.z;
            } else {
                cw[0] = cw[1] = cw[2] = cw[3] = 1.0;
                rh[0] = rh[1] = rh[2] = 1.0;
            }
            wsum = cw[0] + cw[1] + cw[2] + cw[3];
            hsum = rh[0] + rh[1] + rh[2];
            for (int i = 0; i < col[f]; i++) c0 += cw[i];
            for (int i = 0; i < row[f]; i++) r0 += rh[i];
            *u = (c0 + fu * cw[col[f]]) / wsum;
            *v = (r0 + fv * rh[row[f]]) / hsum;
        }
        return 1;
    }
    }
}

/* Premultiplied texel (0..1) at texture coordinates (u, v): bilinear over
 * the pixels of S, edge clamped, after the texture rotation. */
static void texel(const scene *sc, const fx_img *src, double u, double v, double out[4])
{
    double ou, ov, x, y, tx, ty, w[4];
    int32_t x0, y0, xs[2], ys[2];
    switch (sc->tex_rot) {
    case 1:  ou = v;       ov = 1.0 - u; break;
    case 2:  ou = 1.0 - u; ov = 1.0 - v; break;
    case 3:  ou = 1.0 - v; ov = u;       break;
    default: ou = u;       ov = v;       break;
    }
    x = sc->S.x + ou * sc->S.w - 0.5;
    y = sc->S.y + ov * sc->S.h - 0.5;
    x0 = (int32_t)floor(x);
    y0 = (int32_t)floor(y);
    tx = x - x0;
    ty = y - y0;
    xs[0] = fx_clampi(x0, sc->S.x, sc->S.x + sc->S.w - 1);
    xs[1] = fx_clampi(x0 + 1, sc->S.x, sc->S.x + sc->S.w - 1);
    ys[0] = fx_clampi(y0, sc->S.y, sc->S.y + sc->S.h - 1);
    ys[1] = fx_clampi(y0 + 1, sc->S.y, sc->S.y + sc->S.h - 1);
    w[0] = (1.0 - tx) * (1.0 - ty);
    w[1] = tx * (1.0 - ty);
    w[2] = (1.0 - tx) * ty;
    w[3] = tx * ty;
    out[0] = out[1] = out[2] = out[3] = 0.0;
    for (int k = 0; k < 4; k++) {
        fx_px q = fx_get(src, xs[k & 1], ys[k >> 1]);
        double a = q.a / 255.0, wk = w[k] * a;
        out[0] += wk * (q.r / 255.0);
        out[1] += wk * (q.g / 255.0);
        out[2] += wk * (q.b / 255.0);
        out[3] += w[k] * a;
    }
}

/* ---- shading -------------------------------------------------------------------- */
/* Exact Fresnel reflectance of an unpolarized dielectric (Cook and Torrance). */
static double fresnel(double c, double n)
{
    double g2 = n * n + c * c - 1.0, g, a, b;
    if (g2 <= 0.0) return 1.0;
    g = sqrt(g2);
    a = (g - c) / (g + c);
    b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

static double specular(const scene *sc, v3 N, v3 L, v3 V, v3 T)
{
    double NL = v3dot(N, L), NV = v3dot(N, V);
    if (sc->model == 0) {
        v3 R = v3sub(v3mul(N, 2.0 * NL), L);
        double rv = v3dot(R, V);
        return rv > 0.0 ? pow(rv, (double)sc->phong) : 0.0;
    } else {
        v3 H = v3norm(v3add(L, V));
        double NH = v3dot(N, H), VH = v3dot(V, H), F, G, D, c2;
        if (NH <= 1e-6 || VH <= 1e-6) return 0.0;
        if (NV < 1e-6) NV = 1e-6;
        F = fresnel(VH, sc->ior);
        G = fmin(1.0, fmin(2.0 * NH * NV / VH, 2.0 * NH * NL / VH));
        c2 = NH * NH;
        if (!sc->aniso) {
            double m2 = sc->rough * sc->rough;
            D = exp(-((1.0 - c2) / c2) / m2) / (S3D_PI * m2 * c2 * c2);
        } else {
            v3 B = v3cross(N, T);
            double ht = v3dot(H, T) / sc->rough_x, hb = v3dot(H, B) / sc->rough_y;
            D = exp(-(ht * ht + hb * hb) / c2) / (S3D_PI * sc->rough_x * sc->rough_y * c2 * c2);
        }
        return F * D * G / (4.0 * NV);
    }
}

/* Shaded straight color (0..1) of texel color c at hit h seen along dir. */
static v3 shade(const scene *sc, const hit *h, v3 dir, v3 c, v3 tan_o)
{
    v3 N, V, T, out;
    double NL, k;
    if (!sc->light) return c;
    N = v3norm(m3mul(&sc->M, rounded_normal(sc, h)));
    V = v3mul(dir, -1.0);
    if (v3dot(N, V) < 0.0) N = v3mul(N, -1.0);           /* inner surface */
    NL = v3dot(N, sc->L);
    k = sc->diffuse * sc->strength * (NL > 0.0 ? NL : 0.0);
    out = v3m(c.x * (sc->ambient + k * sc->Lc.x), c.y * (sc->ambient + k * sc->Lc.y),
              c.z * (sc->ambient + k * sc->Lc.z));
    if (sc->spec && NL > 0.0 && sc->spec_rate > 0.0) {
        double s;
        T = m3mul(&sc->M, tan_o);
        T = v3norm(v3sub(T, v3mul(N, v3dot(T, N))));
        if (v3dot(T, T) < 0.5) T = v3norm(v3cross(N, fabs(N.x) < 0.9 ? v3m(1, 0, 0) : v3m(0, 1, 0)));
        s = sc->spec_rate * sc->strength * specular(sc, N, sc->L, V, T);
        out = v3add(out, v3m(s * sc->Lc.x, s * sc->Lc.y, s * sc->Lc.z));
    }
    return v3m(clampd(out.x, 0.0, 1.0), clampd(out.y, 0.0, 1.0), clampd(out.z, 0.0, 1.0));
}

/* One ray through document point (px, py): premultiplied RGBA in acc. */
static void trace(const scene *sc, const fx_img *src, double px, double py, double acc[4])
{
    v3 dir = v3norm(v3m((px - sc->cx) / sc->r0, -(py - sc->cy) / sc->r0, -sc->d));
    v3 o = m3tmul(&sc->M, v3sub(v3m(0.0, 0.0, sc->d), sc->T));
    v3 od = m3tmul(&sc->M, dir);
    hits hs;
    int first = 1;
    acc[0] = acc[1] = acc[2] = acc[3] = 0.0;
    intersect(sc, o, od, &hs);
    for (int i = 0; i < hs.n && acc[3] < 1.0; i++) {
        double u, v, tx[4], a, k;
        v3 tan_o, c;
        if (!map_uv(sc, &hs.h[i], &u, &v, &tan_o)) continue;
        texel(sc, src, u, v, tx);
        a = tx[3];
        if (sc->transp && first) a *= sc->transp_k;
        first = 0;
        if (a <= 0.0 || tx[3] <= 0.0) continue;
        c = shade(sc, &hs.h[i], dir, v3m(tx[0] / tx[3], tx[1] / tx[3], tx[2] / tx[3]), tan_o);
        k = (1.0 - acc[3]) * a;
        acc[0] += k * c.x;
        acc[1] += k * c.y;
        acc[2] += k * c.z;
        acc[3] += k;
    }
}

/* ---- render --------------------------------------------------------------------- */
static int s3d_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const s3d_params *p = (const s3d_params *)params;
    scene sc;
    int n = p->aa ? clampi(p->aa_level, 1, 5) + 1 : 1;
    double inv = 1.0 / (double)(n * n);
    int32_t x, y;
    (void)state;
    scene_init(&sc, p, src, env);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double sum[4] = { 0.0, 0.0, 0.0, 0.0 }, acc[4];
            fx_px o = { 0, 0, 0, 0 };
            uint8_t a8;
            if (sc.S.w > 0 && sc.S.h > 0)
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++) {
                        trace(&sc, src, x + (i + 0.5) / n, y + (j + 0.5) / n, acc);
                        sum[0] += acc[0];
                        sum[1] += acc[1];
                        sum[2] += acc[2];
                        sum[3] += acc[3];
                    }
            a8 = fx_u8(255.0 * sum[3] * inv);
            if (a8 != 0u) {
                o.r = fx_u8(255.0 * sum[0] / sum[3]);
                o.g = fx_u8(255.0 * sum[1] / sum[3]);
                o.b = fx_u8(255.0 * sum[2] / sum[3]);
                o.a = a8;
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_s3d = {
    (uint32_t)sizeof(fx_effect), "org.paintc.render.shape3d", "Effects/Render/Shape3D", k_props,
    (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(s3d_params), 0u, NULL,
    NULL, NULL, s3d_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c port of Shape3D by MKT";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Shape3D; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_s3d) >= 0 ? 1 : 0;
}
