/* vec_custom.h - custom shapes for the Shapes tool (lane C, TOOLS.md 3.5
 * "Custom": user shape files from the Shapes folder, sorted by name, the
 * tooltip shows the file location).
 *
 * Format decision: paint.c reads the geometry subset of the XAML shape
 * files the Paint.NET community shares (user content, not Paint.NET
 * assets): the root's DisplayName and Geometry attributes, PathGeometry
 * Figures, Path Data, EllipseGeometry (Center, RadiusX, RadiusY) and
 * RectangleGeometry (Rect, RadiusX, RadiusY) elements, and FillRule
 * (EvenOdd / Nonzero, or the F0 / F1 prefix of path data). Path data is
 * the usual mini-language: M L H V C S Q T A Z, absolute and relative,
 * implicit repeats. Verbose PathFigure / segment elements are not read.
 * The geometry is scaled into the unit square (pc_shape.custom).
 *
 * Files are untrusted (P-08): at most 1 MiB each, 256 files, bounded
 * point counts (pc_path limits), finite numbers only; a file that fails
 * any check is skipped.
 *
 * Thread rules: the parsers are pure (any thread); the catalog is main
 * thread. Ownership: the catalog (app_ext "vec.custom") owns the shapes
 * until the app is destroyed, so pc_shape.custom pointers into it stay
 * valid for every live object and history step.
 */
#ifndef VEC_CUSTOM_H
#define VEC_CUSTOM_H

#include "app/app.h"
#include "pc/pc_path.h"
#include "pc/pc_raster.h"

#define VEC_CUSTOM_MAX_FILE  ((size_t)1u << 20)
#define VEC_CUSTOM_MAX_FILES 256

typedef struct vec_custom_shape {
    char         name[128];      /* DisplayName, else the file name */
    char         file[1024];     /* where it came from (tooltip) */
    pc_path      path;           /* normalized to [0,1]^2 */
    pc_fill_rule rule;
    double       aspect;         /* width / height before normalizing */
} vec_custom_shape;

/* Parse path mini-language text (n bytes) and append it to out; *rule
 * receives the F0 / F1 prefix (unchanged when absent). PC_ERR_FORMAT on a
 * syntax error, PC_ERR_ARG on non-finite numbers, PC_ERR_LIMIT, NOMEM. */
pc_status vec_path_data_parse(const char *s, size_t n, pc_path *out, pc_fill_rule *rule);

/* Parse one XAML shape file into s (initialized by the call; release with
 * vec_custom_free). The name defaults to fallback_name. */
pc_status vec_custom_parse(const char *xaml, size_t n, const char *fallback_name,
                           vec_custom_shape *s);
void      vec_custom_free(vec_custom_shape *s);

/* The app's custom shapes, loaded once from the Shapes folder: inside the
 * settings folder given with --config-dir (portable setups), else in the
 * per-user data folder; apps without a settings folder (tests) load a
 * folder explicitly. */
int32_t                 vec_custom_count(app *a);
const vec_custom_shape *vec_custom_at(app *a, int32_t i);
int32_t                 vec_custom_find(app *a, const char *name);   /* -1 */
/* Replace the catalog with the *.xaml files of dir, sorted by name.
 * Returns the number of shapes loaded. */
int32_t                 vec_custom_load_dir(app *a, const char *dir);

#endif /* VEC_CUSTOM_H */
