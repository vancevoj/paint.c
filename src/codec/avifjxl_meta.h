/* avifjxl_meta.h - private helpers shared by the AVIF and JPEG XL codecs
 * (lane AVIFJXL: fmt_avif.c, fmt_jxl.c). Not a public header: only files in
 * src/codec include it. Nothing here depends on libavif or libjxl, so it
 * builds (and is unit tested) even when neither library is available.
 *
 * Metadata item scheme (pc_image_meta items, see docs/codecs/avif_jxl.md):
 *   "exif"  base64 (RFC 4648, no line breaks) of the TIFF-structured Exif
 *           block, starting at its byte order mark "II*\0" or "MM\0*"
 *           (no "Exif\0\0" prefix, no HEIF 4-byte offset). The IFD0
 *           Orientation tag is rewritten to 1 on load because the pixels
 *           are delivered already oriented.
 *   "xmp"   the XMP packet as UTF-8 text.
 * Readers (axj_meta_get_*) also accept an "exif" value that carries the
 * "Exif\0\0" prefix and an "xmp" value that is base64, so items written by
 * other codecs with a slightly different convention still round trip.
 *
 * Threads: every function is reentrant (no global state). Ownership is
 * stated per function; untrusted byte ranges are borrowed and only read
 * within the call, with every offset bounds-checked.
 */
#ifndef PC_AVIFJXL_META_H
#define PC_AVIFJXL_META_H

#include "pc/pc_codec.h"

/* Largest Exif or XMP payload taken from a file or written to one. */
#define AXJ_META_MAX ((size_t)16 << 20)

/* ---- base64 ---------------------------------------------------------------- */
/* Encode p[0..n) (borrowed). *out is a malloc'ed NUL-terminated string the
 * caller frees with free(). PC_ERR_LIMIT on size overflow, PC_ERR_NOMEM. */
pc_status axj_b64_encode(const uint8_t *p, size_t n, char **out);
/* Decode s (borrowed, NUL-terminated; ASCII whitespace ignored, '='
 * padding optional). On success *out is malloc'ed (caller frees, may be
 * non-NULL even for *len == 0) and true is returned; false on any invalid
 * character or allocation failure (*out NULL). */
bool axj_b64_decode(const char *s, uint8_t **out, size_t *len);

/* ---- Exif ------------------------------------------------------------------ */
/* Offset of the TIFF header inside an Exif payload that may start with
 * "Exif\0\0" or a big-endian 4-byte offset (HEIF and JPEG XL boxes) or
 * directly with the header. Returns n when no valid header is found.
 * p is borrowed. */
size_t axj_exif_tiff_offset(const uint8_t *p, size_t n);
/* Rewrite the IFD0 Orientation tag of a TIFF-structured Exif block
 * (borrowed, modified in place) to 1. Returns the previous value (1..8),
 * or 0 when the block has no valid Orientation tag (nothing written). */
uint32_t axj_exif_reset_orientation(uint8_t *tiff, size_t n);
/* XResolution, YResolution and ResolutionUnit of IFD0 converted to pixels
 * per inch. False (outputs untouched) when absent or outside 1..1e6. */
bool axj_exif_dpi(const uint8_t *tiff, size_t n, double *dpi_x, double *dpi_y);

/* ---- pc_image_meta items ----------------------------------------------------- */
/* Store an Exif payload (borrowed; any of the layouts above) as item
 * "exif" with Orientation reset to 1, and fill m->dpi_x/dpi_y from it when
 * they are still 0. Payloads without a TIFF header or over AXJ_META_MAX are
 * skipped (PC_OK). PC_ERR_NOMEM leaves m without the item. */
pc_status axj_meta_put_exif(pc_image_meta *m, const uint8_t *p, size_t n);
/* Store an XMP packet (borrowed) as item "xmp" when it is non-empty valid
 * UTF-8 without NUL bytes (trailing NULs are dropped); other packets are
 * skipped (PC_OK). */
pc_status axj_meta_put_xmp(pc_image_meta *m, const uint8_t *p, size_t n);
/* The "exif" item of m as a TIFF-structured block with Orientation 1, or
 * NULL when absent or invalid. Caller frees the result with free(). */
uint8_t *axj_meta_get_exif(const pc_image_meta *m, size_t *len);
/* The "xmp" item of m as bytes (raw text, or decoded when the value is
 * base64 of an XML packet), or NULL. Caller frees with free(). */
uint8_t *axj_meta_get_xmp(const pc_image_meta *m, size_t *len);

/* ---- HDR to SDR (PQ and HLG transfer functions) ------------------------------- */
typedef enum axj_hdr_tf {
    AXJ_TF_PQ = 0,      /* SMPTE ST 2084 */
    AXJ_TF_HLG          /* ARIB STD-B67 / BT.2100 HLG */
} axj_hdr_tf;

typedef enum axj_hdr_prim {
    AXJ_PRIM_BT709 = 0, /* also sRGB */
    AXJ_PRIM_BT2020,
    AXJ_PRIM_P3         /* Display P3 (D65) */
} axj_hdr_prim;

/* Tone map n pixels of 16-bit RGBA (0..65535, the signal values of the
 * transfer function tf in primaries prim, borrowed) to 8-bit sRGB BGRA
 * (dst, n pixels). Light is scaled so that diffuse white (203 cd/m2,
 * BT.2408) is 1.0; the gamut is converted to BT.709 and clipped; the
 * largest channel stays linear up to 0.8 and then rolls off smoothly so
 * that peak_nits (<= 0: 10000 for PQ, 1000 for HLG) reaches sRGB white,
 * scaling all three channels alike (hue preserving). Alpha is rounded to 8
 * bits. Deterministic, no state, any thread. */
void axj_hdr_to_srgb8(const uint16_t *rgba16, pc_px32 *dst, size_t n, axj_hdr_tf tf,
                      axj_hdr_prim prim, double peak_nits);

#endif /* PC_AVIFJXL_META_H */
