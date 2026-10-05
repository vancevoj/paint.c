/* pc_icc.h - ICC color management for import and export (lane L6B,
 * Little-CMS 2 underneath).
 *
 * Policy (ADR-008, OD-7, FL-ICC): codecs hand the embedded profile over in
 * pc_image_meta.icc. Opening a file keeps it with the image (Paint.NET 5.1
 * color management) after pc_icc_meta_validate dropped unusable ones;
 * pasting and importing convert to sRGB with pc_icc_import. Codecs never
 * convert by themselves, except CMYK (JPEG and TIFF), whose samples exist
 * only inside the decoder: they convert to Adobe RGB (1998) through the
 * embedded CMYK profile, or through the default CMYK profile when there is
 * no usable one (pc_icc_cmyk_default_profile), and tag the image with
 * Adobe RGB (1998) (FL-CMYK). Encoders embed a profile only when it is
 * usable and matches their pixel data (pc_icc_embed_for, FS-ICC).
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
 * RGB profiles transform the color channels; gray profiles map gray
 * pixels (R == G == B) through the gray curve and colored ones through the
 * profile's RGB form (pc_icc_gray_as_rgb); alpha is kept and pixels with
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

/* A prepared conversion from an image's RGB or gray profile to sRGB, for
 * display color management (the view converts tile pixels before upload;
 * the document keeps its values). Creating it parses the profile once;
 * running it is cheap. */
typedef struct pc_icc_xform pc_icc_xform;

/* *out (owned by the caller, free with pc_icc_xform_destroy) converts from
 * the profile icc/len (borrowed, only read during the call) to sRGB,
 * perceptual intent. PC_ERR_ARG, PC_ERR_LIMIT, PC_ERR_FORMAT,
 * PC_ERR_UNSUPPORTED (CMYK and other spaces), PC_ERR_NOMEM. *is_identity
 * (may be NULL) is set when the profile is sRGB within one code value, so
 * the caller can skip the conversion. */
pc_status pc_icc_xform_create(const uint8_t *icc, size_t len, pc_icc_xform **out,
                              bool *is_identity);
/* Convert n straight-alpha pixels in place (alpha kept, alpha 0 stays all
 * zero). Thread-safe: one xform may run on many threads at once. */
void      pc_icc_xform_run(const pc_icc_xform *x, pc_px32 *px, size_t n);
void      pc_icc_xform_destroy(pc_icc_xform *x);                   /* NULL-safe */

/* ---- additions of wave 4 (lane CODEC) ---------------------------------------------------- */

/* Gray profiles and colored pixels: every conversion above (pixels,
 * documents, xforms, the import step) maps pixels with R == G == B through
 * the gray profile and other pixels through its RGB form
 * (pc_icc_gray_as_rgb), so gray stays exactly gray and color keeps a
 * defined meaning. */

/* The data color space of icc/len (borrowed) when it is usable as an image
 * profile: it passes pc_icc_inspect, its class is not device link,
 * abstract or named color, and Little-CMS builds its transform (to sRGB
 * for RGB and gray profiles, both the gray table and its RGB form; to sRGB
 * for CMYK). A profile that parses but cannot be converted with is not
 * usable (FL-ICC). PC_ICC_SPACE_OTHER when it is not usable, absent (NULL
 * or 0 bytes), or in another space. Any thread. */
pc_icc_space pc_icc_usable_space(const uint8_t *icc, size_t len);

/* The RGB profile equivalent to the gray profile icc/len (borrowed): its
 * three tone curves are the gray curve (gray -> Y; a copy of the grayTRC
 * tag when the PCS is XYZ, else sampled), its colorants are the Rec. 709
 * (sRGB) primaries with a D65 white adapted to D50, so (v, v, v) maps to
 * the PCS color the gray profile gives v, and a gray profile with the sRGB
 * curve becomes sRGB. Description "<gray description> (RGB)", the gray
 * profile's copyright, creation date zeroed (reproducible). *out is
 * malloc'ed (caller frees with free()). PC_ERR_ARG, PC_ERR_LIMIT,
 * PC_ERR_FORMAT, PC_ERR_UNSUPPORTED (not a gray profile), PC_ERR_NOMEM.
 * Any thread. */
pc_status pc_icc_gray_as_rgb(const uint8_t *icc, size_t len, uint8_t **out, size_t *out_len);

/* The profile an encoder embeds (FS-ICC): an image profile is written only
 * when its color space matches the pixel data of the file. */
typedef struct pc_icc_embed {
    const uint8_t *icc;         /* profile to write, NULL = none; points into meta->icc
                                   or at owned; valid until pc_icc_embed_free and while
                                   meta->icc is unchanged */
    size_t         len;
    uint8_t       *owned;       /* malloc'ed profile made for this file, or NULL */
} pc_icc_embed;

/* Decide what to embed for pixel data in space pixels (PC_ICC_SPACE_RGB:
 * RGB, RGBA, palette and BGRA layers; PC_ICC_SPACE_GRAY: gray samples):
 *  - no profile, or one that is not usable (pc_icc_usable_space): none;
 *  - a usable profile of the same space: meta's profile itself;
 *  - a gray profile for RGB pixels: its RGB form (pc_icc_gray_as_rgb);
 *  - anything else (CMYK, an RGB profile for gray pixels): none, so a
 *    writer that can store gray should store RGB when the profile is RGB.
 * meta (may be NULL) is borrowed. e is always initialized (pc_icc_embed_free
 * is safe after any return). PC_OK, PC_ERR_ARG (e NULL, pixels not RGB or
 * gray), PC_ERR_NOMEM. Any thread. */
pc_status pc_icc_embed_for(const pc_image_meta *meta, pc_icc_space pixels, pc_icc_embed *e);
void      pc_icc_embed_free(pc_icc_embed *e);                    /* NULL-safe, zeroes *e */

/* The CMYK profile used when a CMYK file (JPEG, TIFF) has no usable
 * embedded profile: "SWOP TR003 Coated" (ANSI CGATS/SWOP TR 003-2007
 * characterization data, profile CC0, third_party/icc). CMYK images are
 * converted through it to Adobe RGB (1998) and tagged with that profile
 * (FL-CMYK). Static data owned by the library, never freed. *len (may be
 * NULL) receives the size. Any thread. */
const uint8_t *pc_icc_cmyk_default_profile(size_t *len);

#endif /* PC_ICC_H */
