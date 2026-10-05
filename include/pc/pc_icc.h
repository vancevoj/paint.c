/* pc_icc.h - ICC color management for import and export (lane L6B,
 * Little-CMS 2 underneath).
 *
 * Policy (ADR-008, OD-7, FL-ICC): codecs hand the embedded profile over in
 * pc_image_meta.icc. Opening a file keeps it with the image (Paint.NET 5.1
 * color management) after pc_icc_meta_validate dropped unusable ones;
 * pasting and importing convert to sRGB with pc_icc_import. Codecs never
 * convert by themselves, except CMYK (JPEG and TIFF), whose samples exist
 * only inside the decoder: with an embedded CMYK profile they convert to
 * Adobe RGB (1998) and tag the image with that profile (FL-CMYK).
 *
 * Robustness: profiles are untrusted input. Every function validates the
 * header first (size, signature, tag table bounds, a 64 MiB cap) and turns
 * any Little-CMS failure into a status. When a conversion fails, pixels
 * are left exactly as they were.
 *
 * Threads: every function is reentrant. Each call uses its own Little-CMS
 * context (no global state, no global error handler), so calls may run
 * concurrently on any threads.
 */
#ifndef PC_ICC_H
#define PC_ICC_H

#include "pc_codec.h"

/* Profiles larger than this are rejected (PC_ERR_LIMIT). */
#define PC_ICC_MAX_BYTES ((size_t)64 << 20)

typedef enum pc_icc_space {
    PC_ICC_SPACE_OTHER = 0,     /* Lab, XYZ, multichannel, ... (not converted) */
    PC_ICC_SPACE_RGB,
    PC_ICC_SPACE_GRAY,
    PC_ICC_SPACE_CMYK
} pc_icc_space;

typedef struct pc_icc_info {
    pc_icc_space space;         /* data color space */
    uint32_t     version;       /* header version field, e.g. 0x04300000 */
    uint32_t     device_class;  /* header class signature, e.g. 'mntr' */
    bool         is_srgb;       /* converting to sRGB would change nothing
                                   (every probe color within 1 code value) */
    char         desc[128];     /* description, UTF-8/ASCII, may be empty */
} pc_icc_info;

/* Validate and describe a profile. icc/len are borrowed. Returns PC_OK,
 * PC_ERR_ARG (NULL), PC_ERR_LIMIT (too large) or PC_ERR_FORMAT (malformed:
 * info is zeroed). */
pc_status pc_icc_inspect(const uint8_t *icc, size_t len, pc_icc_info *info);

/* Convert w x h straight-alpha BGRA pixels (stride in pixels, borrowed,
 * modified in place) from the profile to sRGB, perceptual intent, 8-bit.
 * RGB profiles transform the color channels; gray profiles map the green
 * channel (gray images have R == G == B); alpha is kept and pixels with
 * alpha 0 stay all zero. CMYK and other spaces return PC_ERR_UNSUPPORTED.
 * On any error the pixels are untouched. */
pc_status pc_icc_to_srgb_px(const uint8_t *icc, size_t len, pc_px32 *px, int32_t w,
                            int32_t h, size_t stride);

/* Same conversion for every layer of a document that is not published yet
 * (a loader result before it reaches history: every tile must have
 * refs == 1, INV-TILE-IMMUTABLE). Tiles are converted with par (may be
 * NULL); the result does not depend on the thread count. Pixels outside
 * the document stay zero (INV-TILE-EDGE). Untouched on error. */
pc_status pc_icc_to_srgb_doc(pc_doc *d, const uint8_t *icc, size_t len, const pc_par *par);

/* Import step for the app: when meta->icc is present, convert d to sRGB
 * (skipped when the profile already is sRGB), then free meta->icc (the
 * pixels are sRGB now) and describe what happened in meta->note. On
 * failure d and meta->icc are unchanged and meta->note says why; the
 * caller keeps the unconverted pixels. PC_OK when there is nothing to do.
 * d must be unpublished as for pc_icc_to_srgb_doc. */
pc_status pc_icc_import(pc_doc *d, pc_image_meta *meta, const pc_par *par);

/* A deterministic sRGB IEC61966-2.1 profile (ICC v4 matrix/TRC, made with
 * Little-CMS, creation date zeroed) for embedding on export. *out is
 * malloc'ed; the caller frees it with free(). */
pc_status pc_icc_srgb_profile(uint8_t **out, size_t *len);

/* Export helper: replace meta->icc with the sRGB profile. */
pc_status pc_icc_meta_set_srgb(pc_image_meta *meta);

/* ---- additions of wave 3b (lane CODEC) ------------------------------------------- */

/* A deterministic Adobe RGB (1998) profile (ICC v4 matrix/TRC: D65 white,
 * the published primaries, gamma 563/256; made with Little-CMS, creation
 * date zeroed). CMYK images are converted to it on load and tagged with it
 * (FL-CMYK). *out is malloc'ed; the caller frees it with free(). */
pc_status pc_icc_adobe_rgb_profile(uint8_t **out, size_t *len);

/* Open-time check of an embedded profile (FL-ICC): when meta->icc is
 * present but malformed (pc_icc_inspect fails), not usable as an image
 * profile (device link, abstract or named color class, or a color space
 * other than RGB and gray), a CMYK profile on RGB pixels, or a gray
 * profile while some pixel of d is not gray, the profile is freed and
 * meta->note says why (appended to an existing note when it fits). d (may
 * be NULL: gray profiles are then kept) is only read. Returns true when
 * the profile was dropped. Any thread; d must not change during the call. */
bool      pc_icc_meta_validate(pc_image_meta *meta, const pc_doc *d);

#endif /* PC_ICC_H */
