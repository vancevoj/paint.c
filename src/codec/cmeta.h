/* cmeta.h - image metadata shared by the codecs (lane CODEC, wave 3b):
 * the pc_image_meta item key scheme, an EXIF (TIFF IFD) model, XMP and
 * IPTC helpers, and the PNG text and GIF comment mappings. Private to
 * src/codec (and the tests in tests/codec).
 *
 * Key scheme of pc_image_meta.items (docs/codecs/meta.md):
 *   "exif"              base64 of a little-endian TIFF block ("II*\0", IFD0
 *                       at offset 8, Exif, GPS and Interoperability sub-IFDs,
 *                       no IFD1 thumbnail). Loaders store it normalized:
 *                       pointer and image-structure tags removed, the
 *                       Orientation tag reset to 1 once the pixels were
 *                       turned upright.
 *   "xmp"               the XMP packet, UTF-8 text.
 *   "iptc"              base64 of the IPTC-IIM datasets (the payload of the
 *                       Photoshop 0x0404 resource or TIFF tag 33723).
 *   "png.text.<kw>"     a PNG tEXt/zTXt/iTXt chunk without an EXIF equivalent
 *                       (keyword in UTF-8). Author, Copyright, Description
 *                       and Comment live in EXIF instead (Artist, Copyright,
 *                       ImageDescription, UserComment), like a GIF comment.
 *   "pdn.<sec>.<name>"  other .pdn userMetadataItems ("$<sec>.<name>"), for
 *                       example "pdn.user.Palette" for "$user.Palette".
 * The ICC profile and the resolution stay in pc_image_meta.icc / dpi.
 *
 * Threads: every function is reentrant (no global mutable state).
 * Ownership is stated per function; "borrowed" pointers are only read
 * during the call.
 */
#ifndef PC_CMETA_H
#define PC_CMETA_H

#include "pc/pc_codec.h"

#define CM_KEY_EXIF     "exif"
#define CM_KEY_XMP      "xmp"
#define CM_KEY_IPTC     "iptc"
#define CM_KEY_PNG_TEXT "png.text."
#define CM_KEY_PDN      "pdn."

/* Caps for untrusted metadata (bytes). Larger blocks are ignored. */
#define CM_EXIF_MAX ((size_t)4 << 20)
#define CM_XMP_MAX  ((size_t)16 << 20)
#define CM_IPTC_MAX ((size_t)4 << 20)
#define CM_TEXT_MAX ((size_t)1 << 20)       /* one PNG text chunk or GIF comment */
#define CM_EXIF_MAX_ENTRIES 8192u            /* entries of one cm_exif, all IFDs */

/* ---- items ------------------------------------------------------------------ */
/* Replace the first item with this key (or append one). Copies both
 * strings. PC_ERR_NOMEM leaves m unchanged. */
pc_status cm_set(pc_image_meta *m, const char *key, const char *value);
/* Remove every item with this key. Never fails. */
void      cm_remove(pc_image_meta *m, const char *key);
/* cm_set with value = base64(p[0..n)). */
pc_status cm_set_blob(pc_image_meta *m, const char *key, const uint8_t *p, size_t n);
/* Decode the base64 item key into *out (malloc, caller frees) and *n.
 * *out = NULL, *n = 0 (and PC_OK) when the item is absent, empty, or not
 * valid base64. PC_ERR_NOMEM. */
pc_status cm_get_blob(const pc_image_meta *m, const char *key, uint8_t **out, size_t *n);

/* ---- text ------------------------------------------------------------------- */
/* Well-formed UTF-8 (no overlongs, no surrogates) without NUL bytes. */
bool  cm_utf8_valid(const uint8_t *p, size_t n);
/* UTF-8 copy of n Latin-1 bytes (NUL bytes dropped). malloc; NULL on OOM. */
char *cm_latin1_to_utf8(const uint8_t *p, size_t n);
/* UTF-8 copy of n bytes that are UTF-8 when valid, else Latin-1. */
char *cm_text_to_utf8(const uint8_t *p, size_t n);
/* Latin-1 encoding of UTF-8 text into out (cap bytes, no NUL written);
 * false when a character is outside Latin-1 or the text does not fit. */
bool  cm_utf8_to_latin1(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len);

/* ---- EXIF model ------------------------------------------------------------------ */
enum { CM_IFD0 = 0, CM_IFD_EXIF = 1, CM_IFD_GPS = 2, CM_IFD_INTEROP = 3, CM_IFD_COUNT = 4 };

#define CM_TAG_ORIENTATION   274u
#define CM_TAG_XRES          282u
#define CM_TAG_YRES          283u
#define CM_TAG_RESUNIT       296u
#define CM_TAG_SOFTWARE      305u
#define CM_TAG_ARTIST        315u
#define CM_TAG_DESCRIPTION   270u
#define CM_TAG_XMP           700u
#define CM_TAG_COPYRIGHT     33432u
#define CM_TAG_IPTC          33723u
#define CM_TAG_EXIF_IFD      34665u
#define CM_TAG_GPS_IFD       34853u
#define CM_TAG_ICC           34675u
#define CM_TAG_MAKERNOTE     37500u
#define CM_TAG_USERCOMMENT   37510u
#define CM_TAG_PIXEL_X       40962u
#define CM_TAG_PIXEL_Y       40963u
#define CM_TAG_INTEROP_IFD   40965u

/* One entry. val holds len = count * type size bytes in little-endian
 * order (owned by the cm_exif). */
typedef struct cm_tag {
    uint16_t tag, type;
    uint32_t count;
    uint8_t  ifd;              /* CM_IFD0 .. CM_IFD_INTEROP */
    uint8_t *val;
    uint32_t len;
} cm_tag;

typedef struct cm_exif {
    cm_tag *t;                 /* owned array, unsorted */
    size_t  n, cap;
} cm_exif;

/* Bytes per value of a TIFF field type (1..13, 129 = EXIF 3.0 UTF-8), or 0. */
uint32_t  cm_type_size(uint32_t type);

void      cm_exif_init(cm_exif *e);
void      cm_exif_free(cm_exif *e);                    /* NULL-safe; re-inits */

/* Parse flags. */
#define CM_PARSE_TIFF_FILE 1u    /* p is a TIFF image file: skip values over
                                    1 MiB (image data, private blobs) */

/* Parse a TIFF structure (p[0..n), borrowed: "II*\0" or "MM\0*" first).
 * Reads IFD0 and its Exif, GPS and Interoperability sub-IFDs (no recursion,
 * each IFD at most once); IFD1 and its thumbnail are dropped, and so are
 * pointer tags, image-structure tags (dimensions, strips, tiles, color
 * maps, ...), the ICC profile (34675), XMP (700) and IPTC (33723), which
 * the codecs carry separately. Values are converted to little-endian
 * (UserComment "UNICODE" text too). Entries with unknown types, counts that
 * do not fit or out-of-range offsets are skipped. Appends to e (call
 * cm_exif_init first). PC_ERR_FORMAT when the header is not TIFF,
 * PC_ERR_NOMEM. */
pc_status cm_exif_parse(const uint8_t *p, size_t n, uint32_t flags, cm_exif *e);

/* True for tags the model never keeps (see cm_exif_parse): pointers,
 * image structure, ICC, XMP, IPTC, Photoshop resources and GDI+ private
 * PNG and thumbnail properties found in old .pdn files. */
bool      cm_exif_tag_dropped(uint8_t ifd, uint16_t tag);

/* Add or replace (same ifd and tag) one entry; le_val holds len bytes in
 * little-endian order and len must equal count * cm_type_size(type).
 * PC_ERR_ARG, PC_ERR_NOMEM, PC_ERR_LIMIT (CM_EXIF_MAX_ENTRIES reached). */
pc_status cm_exif_set(cm_exif *e, uint8_t ifd, uint16_t tag, uint16_t type, uint32_t count,
                      const void *le_val, uint32_t len);
/* First entry with this tag in any IFD, or NULL. Borrowed. */
const cm_tag *cm_exif_find(const cm_exif *e, uint16_t tag);
void      cm_exif_remove(cm_exif *e, uint16_t tag);   /* every IFD */
/* Orientation 1..8 (1 when absent or invalid). */
int       cm_exif_orientation(const cm_exif *e);
/* Resolution in pixels per inch from XResolution, YResolution and
 * ResolutionUnit (inches or centimeters); false when absent or invalid. */
bool      cm_exif_resolution(const cm_exif *e, double *x, double *y);
/* Number of entries in one IFD. */
size_t    cm_exif_count(const cm_exif *e, uint8_t ifd);
/* The IFD a tag belongs to when only its id is known (flat .pdn lists):
 * GPS for 0..31, Exif for the Exif-specific ranges, else IFD0. */
uint8_t   cm_exif_ifd_of(uint16_t tag);

/* Serialize as a little-endian TIFF block (header, IFD0, Exif, GPS,
 * Interoperability; entries sorted by tag; no IFD1), appended to out.
 * Nothing is appended when e is empty. PC_ERR_LIMIT past 4 GiB. */
pc_status cm_exif_serialize(const cm_exif *e, pc_buf *out);

/* TIFF writer support: append the sub-IFD ifd (CM_IFD_EXIF or CM_IFD_GPS;
 * the Exif IFD brings its Interoperability IFD along) at the end of out,
 * word aligned, with offsets relative to out->p + base. *off receives the
 * IFD offset (relative to base), 0 when the IFD is empty. */
pc_status cm_exif_write_subifd(const cm_exif *e, uint8_t ifd, pc_buf *out, size_t base,
                               uint32_t *off);

/* Text tags. Artist, Copyright, ImageDescription (and other ASCII tags)
 * are written as type ASCII holding UTF-8; UserComment gets the "ASCII" or
 * "UNICODE" (UTF-16LE) character code. An empty text removes the tag. */
pc_status cm_exif_set_text(cm_exif *e, uint16_t tag, const char *utf8);
/* UTF-8 text of an ASCII tag or of UserComment (malloc), or NULL when
 * absent, empty or not text. */
char     *cm_exif_get_text(const cm_exif *e, uint16_t tag);

/* ---- EXIF in pc_image_meta ---------------------------------------------------------- */
/* Parse the "exif" item into e (empty when absent or broken). PC_ERR_NOMEM. */
pc_status cm_meta_get_exif(const pc_image_meta *m, cm_exif *e);
/* Store e as the "exif" item (removes the item when e is empty). */
pc_status cm_meta_put_exif(pc_image_meta *m, const cm_exif *e);

/* Loader step for an EXIF block found in a file (p[0..n), borrowed: TIFF
 * header first; a leading "Exif\0\0" is skipped). Parses it, merges it into
 * the "exif" item (entries already present win), and returns the
 * orientation it declared in *orient (may be NULL). With upright == true
 * the caller turns the pixels upright, so Orientation is stored as 1 and an
 * XMP tiff:Orientation is reset as well (also when the XMP arrives later:
 * call cm_meta_xmp_reset_orientation again). Broken blocks are ignored
 * (PC_OK, *orient = 1). PC_ERR_NOMEM. */
pc_status cm_meta_load_exif(pc_image_meta *m, const uint8_t *p, size_t n, bool upright,
                            int *orient);

/* Saver step: the EXIF block to embed for a w x h image, or *out = NULL
 * when there is nothing to write. Orientation is 1, PixelXDimension /
 * PixelYDimension and (when present) the resolution tags are updated from
 * w, h and m->dpi. Tags listed in drop (n_drop entries, may be NULL) are
 * left out. When the block is larger than max_len the MakerNote is dropped,
 * and if it still does not fit nothing is written. *out is malloc'ed (caller
 * frees). PC_ERR_NOMEM. */
pc_status cm_exif_for_save(const pc_image_meta *m, uint32_t w, uint32_t h,
                           const uint16_t *drop, size_t n_drop, size_t max_len,
                           uint8_t **out, size_t *n);

/* ---- XMP ---------------------------------------------------------------------------- */
/* Store p[0..n) (borrowed) as the "xmp" item when it is valid UTF-8 without
 * NUL bytes (trailing NULs are trimmed) and not larger than CM_XMP_MAX;
 * otherwise nothing happens (PC_OK). An existing item is kept. */
pc_status cm_meta_load_xmp(pc_image_meta *m, const uint8_t *p, size_t n);
/* Rewrite tiff:Orientation (attribute or element form) to 1 in the "xmp"
 * item. PC_OK when there is nothing to do. */
pc_status cm_meta_xmp_reset_orientation(pc_image_meta *m);
/* The "xmp" item and its length, or NULL. Borrowed. */
const char *cm_meta_xmp(const pc_image_meta *m, size_t *n);

/* ---- IPTC ----------------------------------------------------------------------------- */
/* Find the IPTC resource (0x0404) in a Photoshop image resource block
 * (sequence of "8BIM" resources, p borrowed). *iim points into p. */
bool      cm_irb_find_iptc(const uint8_t *p, size_t n, const uint8_t **iim, size_t *len);
/* Append one "8BIM" 0x0404 resource holding iim[0..n). */
pc_status cm_irb_put_iptc(pc_buf *out, const uint8_t *iim, size_t n);
/* Store IPTC-IIM data as the "iptc" item (ignored when empty or larger
 * than CM_IPTC_MAX; an existing item is kept). */
pc_status cm_meta_load_iptc(pc_image_meta *m, const uint8_t *iim, size_t n);

/* ---- PNG text and GIF comments ------------------------------------------------------------ */
/* EXIF tag of a standard PNG keyword stored in EXIF (Author, Copyright,
 * Description, Comment), else 0. Case-sensitive like PNG keywords. */
uint16_t    cm_png_text_tag(const char *keyword);
/* The PNG keyword of index i (0..3) of that mapping and its tag; NULL past
 * the end. */
const char *cm_png_text_map(size_t i, uint16_t *tag);

/* Loader step: one text chunk (keyword and text in UTF-8, borrowed). Mapped
 * keywords go into the EXIF set e (the caller stores e afterwards), others
 * become "png.text.<keyword>" items. XMP chunks are not handled here. */
pc_status cm_meta_load_png_text(pc_image_meta *m, cm_exif *e, const char *keyword,
                                const char *text);
/* GIF comment (raw bytes, UTF-8 or Latin-1, borrowed) into the EXIF
 * UserComment of the "exif" item, appended to an existing comment with a
 * newline. Ignored when empty. */
pc_status cm_meta_load_comment(pc_image_meta *m, const uint8_t *p, size_t n);

#endif /* PC_CMETA_H */
