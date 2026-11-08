/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "hdrgrading/foundation/thread.h"
#include "io.h"

#ifdef HDRG_WITH_LCMS2
#include <lcms2.h>
#include <lcms2_plugin.h>
#endif

#define HDRG_ICC_MAX_TAGS     1024u
#define HDRG_ICC_MAX_CURVE    65536u
#define HDRG_TIFF_SHORT       3u
#define HDRG_TIFF_LONG        4u
#define HDRG_TIFF_ASCII       2u
#define HDRG_EXIF_ORIENTATION 0x0112u
#define HDRG_EXIF_IFD         0x8769u
#define HDRG_EXIF_COLORSPACE  0xA001u
#define HDRG_EXIF_INTEROP     0xA005u
#define HDRG_INTEROP_INDEX    0x0001u

/* a TIFF stream (EXIF payload) */
typedef struct {
    const uint8_t *data;
    size_t size;
    bool little;
} hdrg_tiff_t;

static inline uint32_t hdrg_read_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t hdrg_read_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline double hdrg_read_s15f16(const uint8_t *p)
{
    return (double)(int32_t)hdrg_read_u32be(p) / 65536.0;
}

static inline uint16_t hdrg_tiff_u16(const hdrg_tiff_t *tiff, size_t at)
{
    const uint8_t *p = tiff->data + at;

    return tiff->little ? (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) : hdrg_read_u16be(p);
}

static inline uint32_t hdrg_tiff_u32(const hdrg_tiff_t *tiff, size_t at)
{
    const uint8_t *p = tiff->data + at;

    return tiff->little ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                           ((uint32_t)p[3] << 24))
                        : hdrg_read_u32be(p);
}

/* data and size of tag sig; false when absent or out of bounds */
static bool hdrg_icc_tag(const uint8_t *icc, size_t size, const char sig[4], const uint8_t **data,
                         uint32_t *length)
{
    const uint8_t *tag;
    uint32_t count, offset, tag_size, i;

    if (size < 132) {
        return false;
    }

    count = hdrg_read_u32be(icc + 128);
    if (count > HDRG_ICC_MAX_TAGS || size < 132 + (size_t)count * 12) {
        return false;
    }

    for (i = 0; i < count; i++) {
        tag = icc + 132 + (size_t)i * 12;
        if (memcmp(tag, sig, 4) != 0) {
            continue;
        }

        offset = hdrg_read_u32be(tag + 4);
        tag_size = hdrg_read_u32be(tag + 8);
        if (tag_size < 8 || offset > size || size - offset < tag_size) {
            return false;
        }

        *data = icc + offset;
        *length = tag_size;

        return true;
    }

    return false;
}

/* the XYZNumber of an XYZType tag */
static bool hdrg_icc_xyz(const uint8_t *icc, size_t size, const char sig[4], double xyz[3])
{
    const uint8_t *data;
    uint32_t length;

    if (!hdrg_icc_tag(icc, size, sig, &data, &length) || length < 20 ||
        memcmp(data, "XYZ ", 4) != 0) {
        return false;
    }

    xyz[0] = hdrg_read_s15f16(data + 8);
    xyz[1] = hdrg_read_s15f16(data + 12);
    xyz[2] = hdrg_read_s15f16(data + 16);

    return true;
}

/* a curveType or parametricCurveType tag */
static hdrg_status_t hdrg_icc_curve(const hdrg_allocator_t *alloc, const uint8_t *icc, size_t size,
                                    const char sig[4], hdrg_curve_t *out)
{
    static const uint8_t para_counts[5] = {1, 3, 4, 5, 7};
    const uint8_t *data;
    uint32_t length, count, i;
    uint16_t function;
    double p[7];

    if (!hdrg_icc_tag(icc, size, sig, &data, &length) || length < 12) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    if (memcmp(data, "curv", 4) == 0) {
        count = hdrg_read_u32be(data + 8);
        if (count > HDRG_ICC_MAX_CURVE || length - 12u < count * 2u) {
            return HDRG_ERR_ICC_UNSUPPORTED;
        }

        if (count < 2) {
            hdrg_curve_set_gamma((count == 0) ? 1.0 : (double)hdrg_read_u16be(data + 12) / 256.0,
                                 out);

            return (out->g > 0.0) ? HDRG_OK : HDRG_ERR_ICC_UNSUPPORTED;
        }

        memset(out, 0, sizeof(*out));
        out->table = (float *)hdrg_mem_alloc(alloc, (size_t)count * sizeof(float));
        if (out->table == NULL) {
            return HDRG_ERR_NOMEM;
        }

        for (i = 0; i < count; i++) {
            out->table[i] = (float)hdrg_read_u16be(data + 12 + (size_t)i * 2) / 65535.0f;
        }

        out->type = HDRG_CURVE_TABLE;
        out->count = count;

        return HDRG_OK;
    }

    if (memcmp(data, "para", 4) != 0) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    function = hdrg_read_u16be(data + 8);
    if (function > 4 || length - 12u < (uint32_t)para_counts[function] * 4u) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    memset(p, 0, sizeof(p));

    for (i = 0; i < para_counts[function]; i++) {
        p[i] = hdrg_read_s15f16(data + 12 + (size_t)i * 4);
    }

    if (!(p[0] > 0.0)) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    /* every function as y = (a x + b)^g + e for x >= d, else c x + f */
    memset(out, 0, sizeof(*out));
    out->type = HDRG_CURVE_PARA;
    out->g = p[0];
    out->a = (function == 0) ? 1.0 : p[1];
    out->b = p[2];

    switch (function) {
    case 1:
        out->d = (p[1] != 0.0) ? -p[2] / p[1] : 0.0;

        break;
    case 2:
        out->d = (p[1] != 0.0) ? -p[2] / p[1] : 0.0;
        out->e = p[3];
        out->f = p[3];

        break;
    case 3:
        out->c = p[3];
        out->d = p[4];

        break;
    case 4:
        out->c = p[3];
        out->d = p[4];
        out->e = p[5];
        out->f = p[6];

        break;
    default:
        break;
    }

    return HDRG_OK;
}

static bool hdrg_tiff_open(const uint8_t *data, size_t size, hdrg_tiff_t *out)
{
    if (data == NULL || size < 8) {
        return false;
    }

    if (data[0] == 'I' && data[1] == 'I') {
        out->little = true;
    } else if (data[0] == 'M' && data[1] == 'M') {
        out->little = false;
    } else {
        return false;
    }

    out->data = data;
    out->size = size;

    return true;
}

/* offset of the 12-byte entry of tag in the IFD at ifd; 0 when absent */
static size_t hdrg_tiff_find(const hdrg_tiff_t *tiff, uint32_t ifd, uint16_t tag)
{
    uint32_t count, i;
    size_t entry;

    if (ifd < 8 || ifd > tiff->size || tiff->size - ifd < 2) {
        return 0;
    }

    count = hdrg_tiff_u16(tiff, ifd);
    if (tiff->size - ifd - 2 < (size_t)count * 12) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        entry = (size_t)ifd + 2 + (size_t)i * 12;
        if (hdrg_tiff_u16(tiff, entry) == tag) {
            return entry;
        }
    }

    return 0;
}

#ifdef HDRG_WITH_LCMS2
/*
 * A profile that needs a CMM: lcms2 converts the pixels into BT.2020
 * primaries with a 2.2 gamma at 16 bits, wide enough that no source color
 * is clipped on the way in.
 */
/* lcms2's allocation through the context's allocator: an lcms2 context for each call */
static void *hdrg_icc_lcms_malloc(cmsContext lcms, cmsUInt32Number size)
{
    return hdrg_mem_alloc((const hdrg_allocator_t *)cmsGetContextUserData(lcms), size);
}

static void hdrg_icc_lcms_free(cmsContext lcms, void *ptr)
{
    hdrg_mem_free((const hdrg_allocator_t *)cmsGetContextUserData(lcms), ptr);
}

static void *hdrg_icc_lcms_realloc(cmsContext lcms, void *ptr, cmsUInt32Number size)
{
    return hdrg_mem_realloc((const hdrg_allocator_t *)cmsGetContextUserData(lcms), ptr, size);
}

/*
 * lcms2's mutexes on common/thread: its own (defMtxCreate) takes no failed
 * allocation (NULL into pthread_mutex_init); a profile without one locks
 * nothing, as lcms2's own without a plugin
 */
static void *hdrg_icc_lcms_mutex(cmsContext lcms)
{
    hdrg_mutex_t *mutex;

    mutex = (hdrg_mutex_t *)hdrg_icc_lcms_malloc(lcms, sizeof(*mutex));
    if (mutex != NULL && !hdrg_mutex_init(mutex)) {
        hdrg_icc_lcms_free(lcms, mutex);
        mutex = NULL;
    }

    return mutex;
}

static void hdrg_icc_lcms_mutex_free(cmsContext lcms, void *mutex)
{
    if (mutex != NULL) {
        hdrg_mutex_destroy((hdrg_mutex_t *)mutex);
        hdrg_icc_lcms_free(lcms, mutex);
    }
}

static cmsBool hdrg_icc_lcms_lock(cmsContext lcms, void *mutex)
{
    (void)lcms;

    if (mutex != NULL) {
        hdrg_mutex_lock((hdrg_mutex_t *)mutex);
    }

    return TRUE;
}

static void hdrg_icc_lcms_unlock(cmsContext lcms, void *mutex)
{
    (void)lcms;

    if (mutex != NULL) {
        hdrg_mutex_unlock((hdrg_mutex_t *)mutex);
    }
}

static cmsContext hdrg_icc_lcms(const hdrg_allocator_t *alloc)
{
    static cmsPluginMutex mutex = {
        {cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMutexSig, NULL},
        hdrg_icc_lcms_mutex,
        hdrg_icc_lcms_mutex_free,
        hdrg_icc_lcms_lock,
        hdrg_icc_lcms_unlock,
    };
    static cmsPluginMemHandler memory = {
        {cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMemHandlerSig, &mutex.base},
        hdrg_icc_lcms_malloc,
        hdrg_icc_lcms_free,
        hdrg_icc_lcms_realloc,
        NULL,
        NULL,
        NULL,
    };

    return cmsCreateContext(&memory, (void *)(uintptr_t)alloc);
}

static hdrg_status_t hdrg_icc_p3_pq(cmsContext lcms, const hdrg_allocator_t *alloc, float lumi_nits,
                                    const uint8_t *hagc, size_t hagc_size, uint8_t **out,
                                    size_t *out_size)
{
    static const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
    static const cmsCIExyYTRIPLE p3_primaries = {
        {0.680, 0.320, 1.0},
        {0.265, 0.690, 1.0},
        {0.150, 0.060, 1.0},
    };
    const uint8_t *entry;
    cmsHPROFILE profile;
    cmsToneCurve *curves[3];
    cmsVideoSignalType signal;
    cmsCIEXYZ lumi;
    cmsMLU *mlu;
    cmsUInt32Number size;
    cmsUInt16Number table[1024];
    uint32_t i, count, off, tag_size;
    uint8_t *icc, *tag;
    size_t tag_len;
    bool ok;
    double nits, display, y;

    *out = NULL;
    *out_size = 0;

    /*
     * The curve is the SDR fallback for color-managed consumers that ignore
     * cicp: a tone-mapped PQ EOTF (linear below ~0.25 with SDR white around
     * 267 nits, C1 exponential shoulder saturating at ~1000 nits). HDR-aware
     * consumers use the cicp tag and never touch this curve.
     */
    for (i = 0; i < 1024; i++) {
        nits = (double)hdrg_math_pq_eotf((float)i / 1023.0f);
        display = nits / 267.0;
        y = (display <= 0.25)
                ? display
                : 1.0 - 0.75 * (double)hdrg_math_expf((float)(-(display - 0.25) / 0.75));
        table[i] = (cmsUInt16Number)lrint(HDRG_MIN(y, 1.0) * 65535.0);
    }

    curves[0] = cmsBuildTabulatedToneCurve16(lcms, 1024, table);
    if (curves[0] == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    curves[1] = curves[0];
    curves[2] = curves[0];
    profile = cmsCreateRGBProfileTHR(lcms, &d65, &p3_primaries, curves);

    cmsFreeToneCurve(curves[0]);

    if (profile == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&signal, 0, sizeof(signal));
    signal.ColourPrimaries = 12;
    signal.TransferCharacteristics = 16;
    signal.MatrixCoefficients = 0;
    signal.VideoFullRangeFlag = 1;

    ok = cmsWriteTag(profile, cmsSigcicpTag, &signal);

    /*
     * Optional luminance tag: Apple's own PQ profiles carry lumi (reference
     * white in cd/m2) and ColorSync may use it for EDR headroom mapping,
     * while Chrome/skcms render from cicp alone and ignore it — an
     * Apple-only tuning lever (raising it reduces the headroom Photos
     * requests, mitigating zoom dimming without changing X).
     */
    if (ok && lumi_nits > 0.0f) {
        lumi.X = 0.0;
        lumi.Y = (cmsFloat64Number)lumi_nits;
        lumi.Z = 0.0;

        ok = cmsWriteTag(profile, cmsSigLuminanceTag, &lumi);
    }

    /*
     * Headroom Adaptive Gain Curve as Apple ColorSync and skcms read it:
     * tag 'HAGC', type 'hagc', 4 reserved bytes, big-endian payload size,
     * the SMPTE ST 2094-50 payload, zero padding to 4 bytes.
     */
    if (ok && hagc != NULL && hagc_size > 0) {
        tag_len = (12 + hagc_size + 3) & ~(size_t)3;

        tag = (uint8_t *)hdrg_mem_calloc(alloc, tag_len, 1);
        if (tag == NULL) {
            cmsCloseProfile(profile);

            return HDRG_ERR_NOMEM;
        }

        memcpy(tag, "hagc", 4);
        tag[8] = (uint8_t)(hagc_size >> 24);
        tag[9] = (uint8_t)(hagc_size >> 16);
        tag[10] = (uint8_t)(hagc_size >> 8);
        tag[11] = (uint8_t)hagc_size;
        memcpy(tag + 12, hagc, hagc_size);

        ok = cmsWriteRawTag(profile, (cmsTagSignature)0x48414743, tag, (cmsUInt32Number)tag_len);

        hdrg_mem_free(alloc, tag);
    }

    /* a tag lcms2 could not take (out of memory) fails the profile: none goes out without it */
    mlu = ok ? cmsMLUalloc(lcms, 1) : NULL;
    ok = mlu != NULL && cmsMLUsetASCII(mlu, "en", "US", "P3 PQ (libhdrgrading)") &&
         cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu);

    if (mlu != NULL) {
        cmsMLUfree(mlu);
    }

    if (!ok) {
        cmsCloseProfile(profile);

        return HDRG_ERR_NOMEM;
    }

    if (!cmsSaveProfileToMem(profile, NULL, &size) || size == 0) {
        cmsCloseProfile(profile);

        return HDRG_ERR_INTERNAL;
    }

    *out = (uint8_t *)hdrg_mem_alloc(alloc, size);
    if (*out == NULL) {
        cmsCloseProfile(profile);

        return HDRG_ERR_NOMEM;
    }

    if (!cmsSaveProfileToMem(profile, *out, &size)) {
        hdrg_mem_free(alloc, *out);
        *out = NULL;
        cmsCloseProfile(profile);

        return HDRG_ERR_INTERNAL;
    }

    *out_size = size;

    cmsCloseProfile(profile);

    /*
     * Determinism: lcms2 stamps the profile creation dateTime (header bytes
     * 24..35) and a content MD5 profile ID (84..99). Zero both (valid per
     * ICC.1: zero = unknown/not computed) so identical inputs produce
     * byte-identical outputs across runs.
     */
    memset(*out + 24, 0, 12);
    memset(*out + 84, 0, 16);

    /*
     * lcms2 serializes cicpType with the four values at byte 12 (8 reserved
     * bytes); ICC.1 and every reader (skcms/Chrome) expect them at byte 8
     * after a single 4-byte reserved field. Patch the tag in place to the
     * spec layout, keeping the trailing bytes as harmless padding.
     */
    icc = *out;
    count = ((uint32_t)icc[128] << 24) | ((uint32_t)icc[129] << 16) | ((uint32_t)icc[130] << 8) |
            icc[131];
    for (i = 0; i < count; i++) {
        entry = icc + 132 + (size_t)i * 12;
        if (memcmp(entry, "cicp", 4) != 0) {
            continue;
        }

        off = ((uint32_t)entry[4] << 24) | ((uint32_t)entry[5] << 16) | ((uint32_t)entry[6] << 8) |
              entry[7];
        tag_size = ((uint32_t)entry[8] << 24) | ((uint32_t)entry[9] << 16) |
                   ((uint32_t)entry[10] << 8) | entry[11];
        if (tag_size >= 16 && off + tag_size <= *out_size) {
            icc[off + 8] = 12;                   /* Display P3 primaries */
            icc[off + 9] = 16;                   /* PQ */
            icc[off + 10] = 0;                   /* identity matrix (RGB) */
            icc[off + 11] = 1;                   /* full range */
            icc[132 + (size_t)i * 12 + 11] = 12; /* size 16 -> 12 */
        }

        break;
    }

    return HDRG_OK;
}

static hdrg_status_t hdrg_icc_p3(cmsContext lcms, const hdrg_allocator_t *alloc, uint8_t **out,
                                 size_t *out_size)
{
    static const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
    static const cmsCIExyYTRIPLE p3_primaries = {
        {0.680, 0.320, 1.0},
        {0.265, 0.690, 1.0},
        {0.150, 0.060, 1.0},
    };
    cmsHPROFILE profile;
    cmsToneCurve *curves[3];
    cmsMLU *mlu;
    cmsFloat64Number srgb_params[5];
    cmsUInt32Number size;
    bool ok;

    *out = NULL;
    *out_size = 0;

    srgb_params[0] = 2.4;
    srgb_params[1] = 1.0 / 1.055;
    srgb_params[2] = 0.055 / 1.055;
    srgb_params[3] = 1.0 / 12.92;
    srgb_params[4] = 0.04045;

    curves[0] = cmsBuildParametricToneCurve(lcms, 4, srgb_params);
    if (curves[0] == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    curves[1] = curves[0];
    curves[2] = curves[0];
    profile = cmsCreateRGBProfileTHR(lcms, &d65, &p3_primaries, curves);

    cmsFreeToneCurve(curves[0]);

    if (profile == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    mlu = cmsMLUalloc(lcms, 1);
    ok = mlu != NULL && cmsMLUsetASCII(mlu, "en", "US", "Display P3 (libhdrgrading)") &&
         cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu);

    if (mlu != NULL) {
        cmsMLUfree(mlu);
    }

    if (!ok) {
        cmsCloseProfile(profile);

        return HDRG_ERR_NOMEM;
    }

    if (!cmsSaveProfileToMem(profile, NULL, &size) || size == 0) {
        cmsCloseProfile(profile);

        return HDRG_ERR_INTERNAL;
    }

    *out = (uint8_t *)hdrg_mem_alloc(alloc, size);
    if (*out == NULL) {
        cmsCloseProfile(profile);

        return HDRG_ERR_NOMEM;
    }

    if (!cmsSaveProfileToMem(profile, *out, &size)) {
        hdrg_mem_free(alloc, *out);
        *out = NULL;
        cmsCloseProfile(profile);

        return HDRG_ERR_INTERNAL;
    }

    *out_size = size;

    cmsCloseProfile(profile);

    /* determinism: zero the creation dateTime + profile ID (see p3_pq) */
    memset(*out + 24, 0, 12);
    memset(*out + 84, 0, 16);

    return HDRG_OK;
}

static hdrg_status_t hdrg_icc_wide(cmsContext lcms, const hdrg_allocator_t *alloc,
                                   hdrg_decoded_t *decoded)
{
    static const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
    static const cmsCIExyYTRIPLE bt2020 = {
        {0.708, 0.292, 1.0},
        {0.170, 0.797, 1.0},
        {0.131, 0.046, 1.0},
    };
    cmsHPROFILE src_profile, dst_profile;
    cmsToneCurve *gamma, *curves[3];
    cmsHTRANSFORM transform;
    uint32_t y;
    uint8_t *wide;
    size_t stride;

    src_profile = cmsOpenProfileFromMemTHR(lcms, decoded->icc, (cmsUInt32Number)decoded->icc_size);
    if (src_profile == NULL) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    if (cmsGetColorSpace(src_profile) != cmsSigRgbData) {
        cmsCloseProfile(src_profile);

        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    gamma = cmsBuildGamma(lcms, 2.2);
    curves[0] = gamma;
    curves[1] = gamma;
    curves[2] = gamma;
    dst_profile = (gamma != NULL) ? cmsCreateRGBProfileTHR(lcms, &d65, &bt2020, curves) : NULL;
    cmsFreeToneCurve(gamma);

    transform = (dst_profile != NULL)
                    ? cmsCreateTransformTHR(
                          lcms, src_profile, (decoded->bit_depth == 16) ? TYPE_RGB_16 : TYPE_RGB_8,
                          dst_profile, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, 0)
                    : NULL;

    cmsCloseProfile(src_profile);
    cmsCloseProfile(dst_profile);

    if (transform == NULL) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    stride = (size_t)decoded->width * 3u * sizeof(uint16_t);
    wide = ((uint64_t)stride * decoded->height <= SIZE_MAX)
               ? (uint8_t *)hdrg_mem_alloc(alloc, stride * decoded->height)
               : NULL;
    if (wide == NULL) {
        cmsDeleteTransform(transform);

        return HDRG_ERR_NOMEM;
    }

    for (y = 0; y < decoded->height; y++) {
        cmsDoTransform(transform, decoded->pixels + (size_t)y * decoded->stride,
                       wide + (size_t)y * stride, decoded->width);
    }

    cmsDeleteTransform(transform);

    hdrg_mem_free(alloc, decoded->pixels);
    hdrg_mem_free(alloc, decoded->icc);

    /* the stale profile is dropped: the pixels are in the BT.2020 space now */
    decoded->pixels = wide;
    decoded->stride = stride;
    decoded->bit_depth = 16;
    decoded->icc = NULL;
    decoded->icc_size = 0;
    (void)hdrg_color_from_codes(HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_GAMMA22, &decoded->color);

    return HDRG_OK;
}
static hdrg_status_t hdrg_icc_transform_wide(const hdrg_allocator_t *alloc, hdrg_decoded_t *decoded)
{
    hdrg_status_t status;
    cmsContext lcms;

    lcms = hdrg_icc_lcms(alloc);
    if (lcms == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = hdrg_icc_wide(lcms, alloc, decoded);
    cmsDeleteContext(lcms);

    return status;
}
#endif

extern hdrg_status_t hdrg_icc_color(const hdrg_allocator_t *alloc, const uint8_t *icc, size_t size,
                                    hdrg_color_t *out)
{
    const uint8_t *data;
    static const char rgb_trc[3][4] = {{'r', 'T', 'R', 'C'},
                                       {'g', 'T', 'R', 'C'},
                                       {'b', 'T', 'R', 'C'}},
                      rgb_xyz[3][4] = {
                          {'r', 'X', 'Y', 'Z'}, {'g', 'X', 'Y', 'Z'}, {'b', 'X', 'Y', 'Z'}};
    hdrg_status_t status;
    uint32_t length;
    uint8_t c;
    bool gray;
    double colorants[9], xyz[3];

    memset(out, 0, sizeof(*out));

    if (icc == NULL || size < 132) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    /* v4.4 cicp: RGB, full range, code points the library knows */
    if (hdrg_icc_tag(icc, size, "cicp", &data, &length) && length >= 12 &&
        memcmp(data, "cicp", 4) == 0 && data[10] == 0 && data[11] == 1 &&
        data[8] < HDRG_PRIMARIES_ADOBE_RGB && data[9] < HDRG_TRANSFER_ADOBE_RGB &&
        hdrg_color_from_codes(data[8], data[9], out)) {
        return HDRG_OK;
    }

    gray = memcmp(icc + 16, "GRAY", 4) == 0;

    /* an A2B transform takes precedence over the matrix / TRC: it needs a CMM */
    if ((!gray && memcmp(icc + 16, "RGB ", 4) != 0) ||
        hdrg_icc_tag(icc, size, "A2B0", &data, &length) ||
        hdrg_icc_tag(icc, size, "A2B1", &data, &length)) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }

    if (gray) {
        (void)hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_LINEAR, out);
    } else {
        for (c = 0; c < 3; c++) {
            if (!hdrg_icc_xyz(icc, size, rgb_xyz[c], xyz)) {
                return HDRG_ERR_ICC_UNSUPPORTED;
            }

            colorants[c] = xyz[0];
            colorants[3 + c] = xyz[1];
            colorants[6 + c] = xyz[2];
        }

        if (!hdrg_color_set_colorants(colorants, out)) {
            return HDRG_ERR_ICC_UNSUPPORTED;
        }
    }

    for (c = 0; c < 3; c++) {
        status = hdrg_icc_curve(alloc, icc, size, gray ? "kTRC" : rgb_trc[c], &out->curve[c]);
        if (status != HDRG_OK) {
            hdrg_color_free(alloc, out);

            return status;
        }
    }

    /* the profile's curves: no transfer code (the gray's linear one replaced) */
    out->transfer = 0;
    hdrg_color_snap(alloc, out);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_decoded_icc_color(const hdrg_allocator_t *alloc, hdrg_decoded_t *decoded)
{
    hdrg_status_t status;

    status = hdrg_icc_color(alloc, decoded->icc, decoded->icc_size, &decoded->color);

#ifdef HDRG_WITH_LCMS2
    if (status == HDRG_ERR_ICC_UNSUPPORTED) {
        status = hdrg_icc_transform_wide(alloc, decoded);
    }
#endif

    return status;
}

extern uint16_t hdrg_exif_orientation(const uint8_t *exif, size_t size)
{
    hdrg_tiff_t tiff;
    uint16_t value;
    size_t entry;

    if (!hdrg_tiff_open(exif, size, &tiff)) {
        return 0;
    }

    entry = hdrg_tiff_find(&tiff, hdrg_tiff_u32(&tiff, 4), HDRG_EXIF_ORIENTATION);
    if (entry == 0 || hdrg_tiff_u16(&tiff, entry + 2) != HDRG_TIFF_SHORT) {
        return 0;
    }

    value = hdrg_tiff_u16(&tiff, entry + 8);

    return (value >= 1 && value <= 8) ? value : 0;
}

/* Patches tag 0x0112 in place; returns false when the tag is absent. */
extern bool hdrg_exif_set_orientation(uint8_t *exif, size_t size, uint16_t value)
{
    hdrg_tiff_t tiff;
    size_t entry;

    if (!hdrg_tiff_open(exif, size, &tiff)) {
        return false;
    }

    entry = hdrg_tiff_find(&tiff, hdrg_tiff_u32(&tiff, 4), HDRG_EXIF_ORIENTATION);
    if (entry == 0 || hdrg_tiff_u16(&tiff, entry + 2) != HDRG_TIFF_SHORT) {
        return false;
    }

    if (tiff.little) {
        exif[entry + 8] = (uint8_t)(value & 0xFF);
        exif[entry + 9] = (uint8_t)(value >> 8);
    } else {
        exif[entry + 8] = (uint8_t)(value >> 8);
        exif[entry + 9] = (uint8_t)(value & 0xFF);
    }

    return true;
}

extern bool hdrg_exif_adobe_rgb(const uint8_t *exif, size_t size)
{
    hdrg_tiff_t tiff;
    uint32_t ifd;
    uint16_t space;
    size_t entry;

    if (!hdrg_tiff_open(exif, size, &tiff)) {
        return false;
    }

    entry = hdrg_tiff_find(&tiff, hdrg_tiff_u32(&tiff, 4), HDRG_EXIF_IFD);
    if (entry == 0 || hdrg_tiff_u16(&tiff, entry + 2) != HDRG_TIFF_LONG) {
        return false;
    }

    ifd = hdrg_tiff_u32(&tiff, entry + 8);

    entry = hdrg_tiff_find(&tiff, ifd, HDRG_EXIF_COLORSPACE);
    if (entry == 0 || hdrg_tiff_u16(&tiff, entry + 2) != HDRG_TIFF_SHORT) {
        return false;
    }

    space = hdrg_tiff_u16(&tiff, entry + 8);
    if (space == 2) {
        return true;
    }

    if (space != 0xFFFF) {
        return false;
    }

    /* uncalibrated: the DCF option file marks Adobe RGB by the interop index */
    entry = hdrg_tiff_find(&tiff, ifd, HDRG_EXIF_INTEROP);
    if (entry == 0 || hdrg_tiff_u16(&tiff, entry + 2) != HDRG_TIFF_LONG) {
        return false;
    }

    entry = hdrg_tiff_find(&tiff, hdrg_tiff_u32(&tiff, entry + 8), HDRG_INTEROP_INDEX);

    return entry != 0 && hdrg_tiff_u16(&tiff, entry + 2) == HDRG_TIFF_ASCII &&
           hdrg_tiff_u32(&tiff, entry + 4) >= 3 && memcmp(exif + entry + 8, "R03", 3) == 0;
}

#ifdef HDRG_WITH_LCMS2

/*
 * "P3 PQ" profile: Display P3 primaries with a 1024-point PQ EOTF curve and
 * an embedded cicp tag {12, 16, 0, 1}. This is the one HDR signaling that
 * survives X's image re-encode (the transcoder strips every APP metadata
 * segment but preserves and re-attaches the ICC profile).
 */
extern hdrg_status_t hdrg_icc_create_p3_pq(const hdrg_allocator_t *alloc, float lumi_nits,
                                           const uint8_t *hagc, size_t hagc_size, uint8_t **out,
                                           size_t *out_size)
{
    hdrg_status_t status;
    cmsContext lcms;

    lcms = hdrg_icc_lcms(alloc);
    if (lcms == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = hdrg_icc_p3_pq(lcms, alloc, lumi_nits, hagc, hagc_size, out, out_size);
    cmsDeleteContext(lcms);

    return status;
}

extern hdrg_status_t hdrg_icc_create_p3(const hdrg_allocator_t *alloc, uint8_t **out,
                                        size_t *out_size)
{
    hdrg_status_t status;
    cmsContext lcms;

    lcms = hdrg_icc_lcms(alloc);
    if (lcms == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = hdrg_icc_p3(lcms, alloc, out, out_size);
    cmsDeleteContext(lcms);

    return status;
}

#endif /* HDRG_WITH_LCMS2 */
