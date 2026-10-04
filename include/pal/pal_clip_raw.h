/* pal_clip_raw.h - clipboard images with raw pixels (lane L0, additive
 * extension of pal.h v1; pal.h itself stays frozen).
 *
 * pal cannot encode or decode images: it must not depend on the codecs.
 * pal_clip_set_image_png therefore can only offer the PNG flavour, while
 * several platforms expect a different native flavour on the clipboard:
 * Windows applications mostly read CF_DIBV5 / CF_DIB, older macOS
 * applications read public.tiff, and some X11 applications only take
 * image/bmp. This header lets the app hand over the pixels as well, so pal
 * can produce those flavours with trivial uncompressed encoders (BMP with a
 * BITMAPV5HEADER and baseline TIFF). The app encodes the PNG itself through
 * pc_codec and passes both.
 */
#ifndef PAL_CLIP_RAW_H
#define PAL_CLIP_RAW_H

#include "pal/pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Offer one image on the clipboard in every flavour pal can produce.
 *
 *   png, len   PNG file bytes, or NULL and 0 to offer no PNG flavour.
 *   bgra       straight (not premultiplied) alpha BGRA8 pixels in pc_px32
 *              byte order, row 0 at the top.
 *   w, h       size in pixels, each in [1, PC_MAX_DIM].
 *   stride     bytes per row of bgra, >= w * 4.
 *
 * Flavours offered:
 *   Windows        registered "PNG" (when png is given) and CF_DIBV5 with an
 *                  alpha mask; Windows synthesizes CF_DIB and CF_BITMAP.
 *   macOS          public.png (when png is given) and public.tiff.
 *   X11, Wayland   image/png (when png is given) and image/bmp.
 *
 * Ownership: every byte is copied before returning; the caller keeps
 * png and bgra. Returns false and offers nothing when the arguments are
 * invalid, the image is too large for the native flavours (more than
 * 2^32 - 1 encoded bytes) or memory or the clipboard is unavailable.
 * Thread: main thread only, like the rest of the clipboard API. */
bool pal_clip_set_image_bgra(const uint8_t *png, size_t len, const uint8_t *bgra,
                             int32_t w, int32_t h, size_t stride);

#ifdef __cplusplus
}
#endif

#endif /* PAL_CLIP_RAW_H */
