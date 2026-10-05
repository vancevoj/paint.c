/* fx_abi_ext.h - pending additions to fx_abi.h (lane W3B-FXCORE).
 *
 * fx_abi.h is frozen (X-21); these definitions are requested for it and
 * live here until the orchestrator moves them there. They only use bits of
 * existing fields that ABI v1 hosts ignore, so effects that set them still
 * load and run everywhere; a v1 host just shows the plain control.
 *
 * Thread rules and ownership: definitions only.
 */
#ifndef FX_ABI_EXT_H
#define FX_ABI_EXT_H

#include "fx_abi.h"

/* fx_prop.flags of an FXP_COLOR prop: the color has no alpha channel. The
 * host offers no alpha control (Paint.NET's RGB color property, as in Drop
 * Shadow's Color) and stores alpha 255; effects should ignore the stored
 * alpha anyway, since presets and scripts may carry any value. */
#ifndef FXP_F_COLOR_NO_ALPHA
#  define FXP_F_COLOR_NO_ALPHA 8u
#endif

#endif /* FX_ABI_EXT_H */
