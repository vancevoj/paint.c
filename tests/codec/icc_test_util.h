/* icc_test_util.h - real ICC profiles for codec tests (lane CODEC, wave 4).
 * Encoders embed only usable profiles whose color space matches their
 * pixels (pc_icc_embed_for), so round trips of the ICC chunk need genuine
 * profiles instead of random bytes. Include after pc_test.h. Everything is
 * static inline; profiles are malloc'ed (free with free()). */
#ifndef ICC_TEST_UTIL_H
#define ICC_TEST_UTIL_H

#include "pc/pc_icc.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lcms2.h"

static inline uint8_t *itu_save(cmsHPROFILE h, size_t *len)
{
    cmsUInt32Number n = 0;
    uint8_t *p = NULL;
    *len = 0;
    if (!h) return NULL;
    if (cmsSaveProfileToMem(h, NULL, &n) && n > 0u) {
        p = (uint8_t *)malloc(n);
        if (p && !cmsSaveProfileToMem(h, p, &n)) { free(p); p = NULL; }
    }
    cmsCloseProfile(h);
    if (p) *len = n;
    return p;
}

static inline void itu_desc(cmsHPROFILE h, const char *desc)
{
    cmsMLU *m = cmsMLUalloc(NULL, 1);
    if (!m) return;
    cmsMLUsetASCII(m, "en", "US", desc);
    cmsWriteTag(h, cmsSigProfileDescriptionTag, m);
    cmsMLUfree(m);
}

/* An RGB matrix/TRC profile (Adobe RGB like primaries) named desc. With
 * entries > 0 the three curves are different tables of that many entries,
 * so the profile grows to about 6 * entries bytes (multi-chunk tests). */
static inline uint8_t *itu_rgb(const char *desc, uint32_t entries, size_t *len)
{
    cmsCIExyY wp = { 0.3127, 0.3290, 1.0 };
    cmsCIExyYTRIPLE pri = { { 0.64, 0.33, 1.0 }, { 0.21, 0.71, 1.0 }, { 0.15, 0.06, 1.0 } };
    cmsToneCurve *c[3] = { NULL, NULL, NULL };
    cmsHPROFILE h;
    if (entries == 0u) {
        c[0] = c[1] = c[2] = cmsBuildGamma(NULL, 2.2);
    } else {
        uint16_t *v = (uint16_t *)malloc((size_t)entries * sizeof *v);
        if (!v) return NULL;
        for (int k = 0; k < 3; k++) {
            for (uint32_t i = 0; i < entries; i++) {
                double x = (double)i / (double)(entries - 1u);
                double y = pow(x, 2.0 + 0.1 * k) * 65535.0;
                v[i] = (uint16_t)(y + 0.5);
            }
            c[k] = cmsBuildTabulatedToneCurve16(NULL, entries, v);
        }
        free(v);
    }
    h = c[0] && c[1] && c[2] ? cmsCreateRGBProfile(&wp, &pri, c) : NULL;
    if (h) itu_desc(h, desc);
    cmsFreeToneCurve(c[0]);
    if (c[1] != c[0]) cmsFreeToneCurve(c[1]);
    if (c[2] != c[0]) cmsFreeToneCurve(c[2]);
    return itu_save(h, len);
}

/* A gray profile with a gamma curve (v2 style grayTRC, XYZ PCS). */
static inline uint8_t *itu_gray(double gamma, const char *desc, size_t *len)
{
    cmsToneCurve *g = cmsBuildGamma(NULL, gamma);
    cmsHPROFILE h = g ? cmsCreateGrayProfile(cmsD50_xyY(), g) : NULL;
    if (h) itu_desc(h, desc);
    if (g) cmsFreeToneCurve(g);
    return itu_save(h, len);
}

/* A small CMYK output profile (Lab PCS, AToB0 = a 9-point CLUT of a
 * simple ink model), enough to be a usable CMYK profile. */
static inline int itu_cmyk_sampler(CMSREGISTER const cmsUInt16Number in[],
                                   CMSREGISTER cmsUInt16Number out[], CMSREGISTER void *cargo)
{
    double c = in[0] / 65535.0, m = in[1] / 65535.0, y = in[2] / 65535.0, k = in[3] / 65535.0;
    cmsCIELab lab;
    (void)cargo;
    lab.L = 100.0 * (1.0 - k) * (1.0 - 0.3 * (c + m + y) / 3.0);
    lab.a = 60.0 * (m - c) * (1.0 - k);
    lab.b = 60.0 * (y - 0.5 * (c + m)) * (1.0 - k);
    cmsFloat2LabEncoded(out, &lab);
    return 1;
}

static inline uint8_t *itu_cmyk(size_t *len)
{
    cmsHPROFILE p = cmsCreateProfilePlaceholder(NULL);
    cmsPipeline *pl = cmsPipelineAlloc(NULL, 4, 3);
    cmsStage *clut = cmsStageAllocCLut16bit(NULL, 9, 4, 3, NULL);
    bool ok = p && pl && clut;
    *len = 0;
    if (ok) {
        cmsSetProfileVersion(p, 2.1);
        cmsSetDeviceClass(p, cmsSigOutputClass);
        cmsSetColorSpace(p, cmsSigCmykData);
        cmsSetPCS(p, cmsSigLabData);
        ok = cmsStageSampleCLut16bit(clut, itu_cmyk_sampler, NULL, 0) &&
             cmsPipelineInsertStage(pl, cmsAT_END, clut);
        clut = NULL;                                /* owned by pl now */
        ok = ok && cmsWriteTag(p, cmsSigAToB0Tag, pl) &&
             cmsWriteTag(p, cmsSigMediaWhitePointTag, cmsD50_XYZ());
        itu_desc(p, "test cmyk");
    }
    if (clut) cmsStageFree(clut);
    if (pl) cmsPipelineFree(pl);
    if (!ok) {
        if (p) cmsCloseProfile(p);
        return NULL;
    }
    return itu_save(p, len);
}

/* Adobe RGB like profile with its PCS field damaged to 'SYZ ' (the audit's
 * badpcs.png): the header still parses, no transform can be built. */
static inline uint8_t *itu_bad_pcs(size_t *len)
{
    uint8_t *p = itu_rgb("damaged PCS", 0u, len);
    if (p && *len > 24u) memcpy(p + 20, "SYZ ", 4u);
    return p;
}

/* Random bytes that are not a profile at all. */
static inline uint8_t *itu_junk(size_t n, uint32_t seed)
{
    uint8_t *p = (uint8_t *)malloc(n);
    for (size_t i = 0; p && i < n; i++) p[i] = (uint8_t)((i * 131u + seed * 7u) ^ (i >> 3));
    return p;
}

#endif /* ICC_TEST_UTIL_H */
