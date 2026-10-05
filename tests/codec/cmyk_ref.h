/* cmyk_ref.h - reference for CMYK decodes without a usable embedded profile
 * (lane CODEC, wave 4, FL-CMYK): Little-CMS converts the 8-bit samples
 * through the default CMYK profile (pc_icc_cmyk_default_profile, SWOP
 * TR003 Coated) to Adobe RGB (1998) (pc_icc_adobe_rgb_profile), perceptual,
 * unoptimized, independently of the decoders' own transform code. Include
 * after pc_test.h. Single-threaded tests only: the two transforms are made
 * once and stay reachable from static pointers until exit. */
#ifndef CMYK_REF_H
#define CMYK_REF_H

#include "pc/pc_icc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lcms2.h"

static cmsHTRANSFORM g_cmyk_ref_xf[2];

static inline cmsHTRANSFORM cmyk_ref_xf(bool inverted)
{
    size_t pn = 0, an = 0;
    const uint8_t *prof;
    uint8_t *adobe = NULL;
    cmsHPROFILE in, o;
    int k = inverted ? 1 : 0;
    if (g_cmyk_ref_xf[k]) return g_cmyk_ref_xf[k];
    prof = pc_icc_cmyk_default_profile(&pn);
    if (!prof || pn < 132u || pc_icc_adobe_rgb_profile(&adobe, &an) != PC_OK) return NULL;
    in = cmsOpenProfileFromMem(prof, (cmsUInt32Number)pn);
    o = cmsOpenProfileFromMem(adobe, (cmsUInt32Number)an);
    if (in && o)
        g_cmyk_ref_xf[k] = cmsCreateTransform(in, inverted ? TYPE_CMYK_8_REV : TYPE_CMYK_8, o,
                                              TYPE_BGR_8, INTENT_PERCEPTUAL,
                                              cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE);
    if (in) cmsCloseProfile(in);
    if (o) cmsCloseProfile(o);
    free(adobe);
    return g_cmyk_ref_xf[k];
}

/* n pixels of 4 samples (inverted: 255 = no ink, Adobe JPEG style) to
 * opaque BGRA in out. false when Little-CMS could not build the transform. */
static inline bool cmyk_ref(const uint8_t *cmyk, size_t n, bool inverted, pc_px32 *out)
{
    cmsHTRANSFORM t = cmyk_ref_xf(inverted);
    uint8_t bgr[3 * 64];
    if (!t) return false;
    while (n) {
        size_t k = n < 64u ? n : 64u;
        cmsDoTransform(t, cmyk, bgr, (cmsUInt32Number)k);
        for (size_t i = 0; i < k; i++) {
            out[i].b = bgr[3u * i];
            out[i].g = bgr[3u * i + 1u];
            out[i].r = bgr[3u * i + 2u];
            out[i].a = 255u;
        }
        cmyk += 4u * k;
        out += k;
        n -= k;
    }
    return true;
}

/* True when meta carries the Adobe RGB (1998) profile the CMYK decoders tag
 * their results with. */
static inline bool cmyk_ref_is_adobe(const pc_image_meta *m)
{
    pc_icc_info info;
    return m->icc && pc_icc_inspect(m->icc, m->icc_len, &info) == PC_OK &&
           info.space == PC_ICC_SPACE_RGB && strcmp(info.desc, "Adobe RGB (1998)") == 0;
}

#endif /* CMYK_REF_H */
