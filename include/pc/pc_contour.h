/* pc_contour.h - outlines of coverage fields (marching ants) and the
 * selection polygon text format (lane L1a).
 *
 * The outline is the 50% iso-line of the coverage (a pixel is inside when
 * its value is >= 128), traced with marching squares over pixel centers:
 *  - Crossing points are linearly interpolated between the two pixel
 *    values, so antialiased edges give smooth sub-pixel outlines.
 *  - Where all four pixels of a cell are hard (0 or 255) the outline goes
 *    through the shared pixel corner, so hard masks give outlines that lie
 *    exactly on pixel edges. Collinear runs are merged, so a rectangle is
 *    exactly 4 points.
 *  - Inside pixels connect through edges only (4-connectivity); diagonal
 *    neighbors form separate loops that touch at a corner. Antialiased
 *    saddles use the cell average.
 *  - Every contour is closed, with the inside on its left in y-down
 *    coordinates: outer boundaries run counter-clockwise on screen
 *    (negative pc_poly_area), holes clockwise. Filling the result with
 *    either fill rule reproduces the inside region.
 * Work is done in 64 x 64 blocks; blocks whose neighborhood is uniformly
 * inside or outside are skipped, so the cost follows the outline length.
 *
 * Thread rules: reentrant. The field callbacks are called on the calling
 * thread only. Ownership: inputs are borrowed; results are appended to a
 * caller-owned pc_poly (on error it is truncated back to its previous
 * contour count).
 */
#ifndef PC_CONTOUR_H
#define PC_CONTOUR_H

#include "pc_raster.h"

/* A coverage field over a document area, read in 64 x 64 blocks aligned
 * to area.x / area.y. Coverage outside area is 0. */
typedef struct pc_cov_field {
    pc_rect area;
    /* Return block (bx, by) as 64 rows of 64 bytes (stride 64), or NULL
     * with *uniform set to the value shared by all of the block's pixels
     * inside area. scratch holds 4096 bytes the callback may fill and
     * return. Pixels outside area are ignored. Any other returned pointer
     * must keep its content until pc_contour_field returns (block classes
     * are cached by pointer, which makes shared selection tiles cheap). */
    const uint8_t *(*block)(void *ud, int32_t bx, int32_t by, uint8_t *scratch,
                            uint8_t *uniform);
    void *ud;
} pc_cov_field;

/* simplify: 0 merges exactly collinear points only; > 0 also drops points
 * within that distance of the line through their neighbors (display use). */
pc_status pc_contour_field(const pc_cov_field *f, double simplify, pc_poly *out);
pc_status pc_contour_mask(const pc_mask *m, double simplify, pc_poly *out);

/* ---- selection polygon text ------------------------------------------------
 * The JSON shape Paint.NET uses for Edit > Copy Selection:
 *   {"polygonList": ["3,4,9,4,9,19,3,19,3,4", ...]}
 * one string per contour, x,y pairs, first point repeated at the end.
 * Numbers are written in plain decimal (up to 6 fraction digits, no
 * locale dependence). */
/* *out receives a NUL-terminated malloc'ed string (release with free()),
 * *len its length without the NUL (len may be NULL). */
pc_status pc_poly_to_json(const pc_poly *p, char **out, size_t *len);
/* Parse text (n bytes, need not be NUL-terminated) and append its
 * polygons to out as closed contours. Unknown keys are ignored. Hardened
 * for clipboard input: PC_ERR_FORMAT for malformed text, PC_ERR_LIMIT
 * beyond PC_GEOM_MAX_POINTS points or for |coordinates| > 1e9. */
pc_status pc_poly_from_json(const char *text, size_t n, pc_poly *out);

#endif /* PC_CONTOUR_H */
