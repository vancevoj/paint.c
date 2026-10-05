/* doc_trc.h - lane W3B-FXCORE: the transfer curve of an open image's color
 * profile, for gamma-correct resampling (Image > Resize, Move Selected
 * Pixels). Paint.NET 5.1 linearizes with the image profile's curve, and
 * paint.c keeps pixels in that profile (edit/m_profile.h).
 *
 * Thread rules: main thread. The returned curve is read-only and may be
 * shared with workers for the duration of the caller's operation.
 */
#ifndef DOC_TRC_H
#define DOC_TRC_H

#include "app_internal.h"
#include "pc/pc_resample.h"

/* The curve of d's profile, or NULL for sRGB: no embedded profile, an sRGB
 * profile, or one without usable curve tags (LUT-based, CMYK, malformed).
 * Cached per app by (document id, profile bytes), so repeated calls (live
 * previews) cost a hash of the profile. Borrowed: valid until the next
 * call of this function or app_destroy. */
const pc_trc *app_doc_trc(app *a, const app_doc *d);

#endif /* DOC_TRC_H */
