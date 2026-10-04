/* pc_comp.h - flattening the layer stack with the pc_composite_span oracle.
 *
 * Visible layers composite bottom to top over transparent black, each with
 * its blend mode and opacity (Section 3.5). Results are bit-identical for
 * any pc_par thread count (INV-ORACLE).
 */
#ifndef PC_COMP_H
#define PC_COMP_H

#include "pc_par.h"
#include "pc_surf.h"

/* Composite rect r of d into dst (straight BGRA, stride in pixels).
 * Pixels outside the document come out as zero. par may be NULL. Any
 * thread, as long as no one mutates d during the call. */
pc_status pc_comp_rect(const pc_doc *d, pc_rect r, pc_px32 *dst,
                       size_t dst_stride, const pc_par *par);

#endif /* PC_COMP_H */
