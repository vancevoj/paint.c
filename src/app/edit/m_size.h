/* m_size.h - lane M: the size model shared by the Resize and Canvas Size
 * dialogs (MENUS.md Image, OBSERVED.md 3.1 and 3.2) and an undoable
 * resolution change.
 *
 * The model keeps the new pixel size as doubles (rounded only for display
 * and for the result) so that toggling "Maintain aspect ratio" and editing
 * one side keeps the other exact, as Paint.NET 3.36's ResizeConstrainer
 * did. The resolution is stored in pixels per inch; the dialog shows it in
 * pixels per inch or per centimeter, and the print size in inches or
 * centimeters (print size = pixels / resolution; editing the print size
 * changes the pixels, editing the resolution changes the print size only).
 *
 * Thread rules: the model functions are pure (any thread); m_doc_set_dpi
 * runs on the main thread like every history operation. Ownership: no
 * allocations except the history payload of m_doc_set_dpi (owned by the
 * history).
 */
#ifndef M_SIZE_H
#define M_SIZE_H

#include "app/app_doc.h"

#define M_SIZE_MAX_PCT   2000.0     /* OBSERVED 3.1: the box clamps at 2000.00 */
#define M_SIZE_MIN_RES   0.01       /* OBSERVED 2: 0.01 .. 65536.00 */
#define M_SIZE_MAX_RES   65536.0
#define M_SIZE_MAX_EDIT  262144     /* largest value the pixel boxes accept */

typedef struct m_size {
    uint32_t ow, oh;      /* image size when the dialog opened (>= 1) */
    int      by;          /* 0 = by percentage, 1 = by absolute size */
    double   pct;         /* percentage box, 0 .. M_SIZE_MAX_PCT */
    bool     keep;        /* maintain aspect ratio (ignored by percentage) */
    double   w, h;        /* new pixel size, unrounded */
    double   dpi;         /* resolution, pixels per inch */
    int      res_unit;    /* 0 pixels/inch, 1 pixels/centimeter */
    int      print_unit;  /* 0 inches, 1 centimeters */
} m_size;

/* Start from the current image: absolute size selected, 100 %, the given
 * aspect lock, resolution dpi (<= 0 means 96) shown in centimeters when
 * cm is true. */
void    m_size_init(m_size *s, uint32_t w, uint32_t h, double dpi, bool keep, bool cm);

void    m_size_set_by(m_size *s, int by);
void    m_size_set_pct(m_size *s, double pct);
void    m_size_set_keep(m_size *s, bool keep);
void    m_size_set_w(m_size *s, double w);          /* pixels */
void    m_size_set_h(m_size *s, double h);
/* Resolution in the displayed unit (clamped); pixels stay. */
void    m_size_set_res(m_size *s, double res);
double  m_size_res(const m_size *s);                 /* in the displayed unit */
void    m_size_set_res_unit(m_size *s, int unit);
/* Print size in the print unit. */
double  m_size_print(const m_size *s, bool height);
void    m_size_set_print(m_size *s, bool height, double v);

/* Rounded result (0 when below 0.5; may exceed PC_MAX_DIM). */
int32_t m_size_px(const m_size *s, bool height);
/* The result can be applied: both sides in [1, PC_MAX_DIM] and, by
 * percentage, a positive percentage. why (may be NULL) gets a short reason. */
bool    m_size_valid(const m_size *s, char *why, size_t cap);
/* Memory of the image at the new size: w * h * 4 * layers bytes. */
uint64_t m_size_bytes(const m_size *s, uint32_t layers);
/* "1.8 MB" style text in binary units with one decimal (OBSERVED 2). */
void    m_size_format_bytes(uint64_t bytes, char *out, size_t cap);

/* Session memory of the dialogs (the last OK'd choices): Resize keeps
 * resampling (index into the MENUS.md list, 0 = Bicubic), gamma and the
 * aspect lock; Canvas Size keeps the anchor (pc_anchor, default Top Left)
 * and the fill (0 Transparent, 1 Primary, 2 Secondary, 3 White, 4 Black).
 * Borrowed, owned by the app; NULL on OOM. */
typedef struct m_size_memory {
    int  resample;
    bool gamma, keep;
    int  anchor, fill;
} m_size_memory;
struct app;
m_size_memory *m_size_memory_get(struct app *a);

/* Record a resolution change of d as a history step labeled label
 * (involution payload swapping d->meta.dpi_x / dpi_y). PC_ERR_STATE when
 * nothing changes or a transaction is open, PC_ERR_NOMEM. The caller runs
 * app_doc_history_changed. */
pc_status m_doc_set_dpi(app_doc *d, double dpi_x, double dpi_y, const char *label);

#endif /* M_SIZE_H */
