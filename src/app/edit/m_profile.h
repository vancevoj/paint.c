/* m_profile.h - lane M: Image > Color Profile operations on an open image
 * (mods/mod_m_profile.c holds the dialog).
 *
 * paint.c keeps each image's pixels in its own color profile, carried in
 * app_doc.meta.icc (NULL means sRGB, the assumed profile). Assign replaces
 * the profile only (the pixels keep their values, so the appearance
 * changes); Convert transforms the pixels of every layer from the current
 * profile to the new one (appearance kept, colors outside the new gamut
 * clipped) and assigns it. Both are one history step. The canvas shows
 * the pixel values as sRGB (no display color management yet, VIEW.md
 * V-RENDER-CM gap).
 *
 * Thread rules: main thread; Convert transforms tiles with the worker
 * pool. Ownership: profile bytes are copied.
 */
#ifndef M_PROFILE_H
#define M_PROFILE_H

#include "app/app.h"
#include "app/app_doc.h"

/* Assign profile bytes (NULL / 0 = sRGB, no embedded profile). PC_ERR_STATE
 * when nothing changes or the image is busy; PC_ERR_FORMAT for bytes
 * pc_icc_inspect rejects; PC_ERR_NOMEM. Runs app_doc_history_changed. */
pc_status m_profile_assign(app *a, app_doc *d, const uint8_t *icc, size_t len);

/* Convert to the profile (NULL / 0 = sRGB). The destination must be a
 * matrix/TRC RGB profile (PC_ERR_UNSUPPORTED otherwise); the source may
 * be any RGB or gray profile Little-CMS reads. */
pc_status m_profile_convert(app *a, app_doc *d, const uint8_t *icc, size_t len);

/* Description of the image's profile ("sRGB IEC61966-2.1 (assumed)" when
 * none is embedded). */
void      m_profile_describe(const app_doc *d, char *out, size_t cap);

#endif /* M_PROFILE_H */
