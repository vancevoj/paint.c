/* text_font.h - the Text tool's fonts (lane C, TOOLS.md 3.3, 11.1):
 * the catalog of installed font families and the pc_font_face backend over
 * the UI toolkit's validated faces (ui_font.h, stb_truetype outlines).
 *
 * Catalog. The built-in Inter (Regular and SemiBold) is always available.
 * System and user fonts come from pal_font_dirs, scanned on a worker
 * (iterative directory walk, depth and count limited) and described with
 * ui_font_describe; the result is cached in "fonts.cache" in the settings
 * folder, so later runs only read files whose modification time changed.
 * Apps without a settings folder (tests) use the built-in face only unless
 * a test scans explicitly. Families are listed by their typographic family
 * name, sorted case-insensitively.
 *
 * Faces. text_fonts_faces picks the face of a family closest to the
 * requested weight and slant (bold = 700, regular = 400; the pc_font_face
 * bold / italic flags tell pc_text whether to synthesize), loads it with
 * ui_font_load_file (structural validation, glyphs failing it draw empty),
 * and appends fallback faces for characters the family lacks: the built-in
 * Inter, then common broad-coverage families when installed (loaded only
 * when a character needs them). Faces stay loaded until the app exits.
 *
 * Backend: glyph = cmap, advance and kerning (GPOS pairs or kern) scaled
 * to the em size, metrics from hhea / OS/2, outlines as quadratic and cubic
 * pc_path segments with y pointing down. The Sharp rendering modes snap the
 * baseline, x-height and cap-height zones of the outline to whole pixels
 * (the toolkit's hinting for small UI text); Smooth keeps the outline.
 *
 * Thread rules: main thread, except text_fonts_scan_dirs (any thread, pure
 * apart from file reads). Ownership: the catalog belongs to the app
 * (app_ext "text.fonts") and owns every face; returned pointers are
 * borrowed until the app is destroyed.
 */
#ifndef TEXT_FONT_H
#define TEXT_FONT_H

#include "app/app.h"
#include "pc/pc_text.h"
#include "ui/ui_font.h"

typedef struct text_fonts text_fonts;

/* One face of a font file as found by a scan. */
typedef struct text_face_info {
    char     path[1024];        /* UTF-8; empty for the built-in faces */
    int32_t  index;             /* face in a collection; built-in: ui_font_builtin */
    uint64_t mtime;
    char     family[96];
    char     style[64];
    uint16_t weight;
    bool     italic;
} text_face_info;

#define TEXT_SCAN_MAX_FILES 20000u
#define TEXT_SCAN_MAX_DEPTH 16u

/* Scan dirs (NULL terminated) for TTF/OTF/TTC/OTC files. prev (may be
 * NULL) supplies descriptions of unchanged files (same path and mtime).
 * *out is malloc'ed (free()), *n faces. Any thread. PC_ERR_NOMEM. */
pc_status text_fonts_scan_dirs(const char *const *dirs, const text_face_info *prev, size_t nprev,
                               text_face_info **out, size_t *n);
/* Cache file text <-> face list (hardened parser, P-08). */
pc_status text_fonts_cache_write(const char *path, const text_face_info *f, size_t n);
pc_status text_fonts_cache_read(const char *path, text_face_info **out, size_t *n);

/* The app's catalog (created on first use; NULL on OOM). Starts the
 * background scan once when the app has a settings folder. */
text_fonts *text_fonts_get(app *a);
/* Replace the system part of the catalog with faces (tests, scans). */
pc_status   text_fonts_set_faces(text_fonts *tf, const text_face_info *f, size_t n);
bool        text_fonts_scanning(const text_fonts *tf);
int32_t     text_fonts_family_count(const text_fonts *tf);
const char *text_fonts_family(const text_fonts *tf, int32_t i);
int32_t     text_fonts_find_family(const text_fonts *tf, const char *name);  /* -1 */
/* Generation of the family list (changes when a scan finishes). */
uint32_t    text_fonts_gen(const text_fonts *tf);

/* A face to draw family i's name in its own font in the font list
 * (TOOLS.md 3.3: dropdown with previews). Faces load on demand, at most a
 * couple per frame (frame = ui_frame_count) and only for files up to 8 MiB;
 * NULL until loaded, or when the family cannot preview (the caller then
 * uses the UI font). At most 96 preview faces stay loaded; the least
 * recently drawn ones are released, never ones drawn in this frame. */
ui_font    *text_fonts_preview(text_fonts *tf, int32_t i, uint32_t frame);

/* The default family: "Inter", paint.c's own UI face (TOOLS.md 3.3: the
 * platform's sans UI font). */
#define TEXT_DEFAULT_FAMILY "Inter"

/* Faces for family (unknown families use the default) with the style:
 * faces[0] is the primary face, then fallbacks. The array and faces are
 * owned by the catalog and stay valid until the app is destroyed.
 * PC_ERR_NOMEM, or the load error of the primary face (the default face
 * is used instead and PC_OK returned when it loads). */
pc_status   text_fonts_faces(text_fonts *tf, const char *family, bool bold, bool italic,
                             const pc_font_face *const **faces, size_t *n);

#endif /* TEXT_FONT_H */
