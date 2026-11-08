/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "io/io.h"
#include "hdrgrading/foundation/math.h"

#ifdef HDRG_WITH_LCMS2
#include <lcms2.h>
#endif

/*
 * 入力の色と出力の色域: 拡張した SDR ベースは P3 でタグされ、
 * 彩度の高い色だけが P3 側へ伸びて無彩色は変わらない。Adobe RGB / BT.2020 /
 * ProPhoto の入力は sRGB へ潰れない。ICC (行列 + TRC) と EXIF の Interop "R03" を
 * 解決し、sdr_eotf は sRGB でタグされた入力の復号を変える。
 */
#define TG_SIZE 64u

static hdrg_allocator_t g_alloc;
static hdrg_context_t *g_ctx;

/* 左半分 left、右半分 right の画像を、ゲインとグレーディングを中立にして変換する */
static void convert_base(uint8_t primaries, uint8_t transfer, const uint8_t left[3],
                         const uint8_t right[3], float expand, uint8_t sdr_eotf,
                         hdrg_decoded_t *out)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    uint32_t x, y;
    uint8_t *pixels, *jpeg;
    size_t idx, jpeg_size, second;

    pixels = (uint8_t *)malloc(TG_SIZE * TG_SIZE * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TG_SIZE; y++) {
        for (x = 0; x < TG_SIZE; x++) {
            idx = ((size_t)y * TG_SIZE + x) * 3;
            memcpy(pixels + idx, (x < TG_SIZE / 2) ? left : right, 3);
        }
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_GAINMAP; /* SDR ベースの code を直接検証する */
    params.expand_strength = expand;
    params.sdr_eotf = sdr_eotf;
    /* ゲイン・グレーディングを中立化し、ベース = 色の変換のみにする */
    params.global_strength = 0.0f;
    params.contrast = 1.0f;
    params.clarity = 0.0f;
    params.saturation = 1.0f;
    params.highlight_desat = 0.0f;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = primaries;
    raw.transfer = transfer;
    raw.width = TG_SIZE;
    raw.height = TG_SIZE;
    raw.bit_depth = 8;
    raw.stride = TG_SIZE * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(g_ctx, &raw, &params, NULL, &encode, &jpeg, &jpeg_size));

    free(pixels);

    /* プライマリのみデコード (2 つ目の SOI 手前まで) */
    second = jpeg_size;
    for (idx = 2; idx + 2 < jpeg_size; idx++) {
        if (jpeg[idx] == 0xFF && jpeg[idx + 1] == 0xD8 && jpeg[idx + 2] == 0xFF) {
            second = idx;

            break;
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, jpeg, second, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, out));

    hdrg_free(jpeg);
}

/* 半分の領域 (端の 8 画素を除く) のチャネル平均 code */
static float area_mean(const hdrg_decoded_t *d, bool right, uint8_t ch)
{
    uint32_t x, y, count;
    float sum;

    sum = 0.0f;
    count = 0;
    for (y = 8; y < d->height - 8; y++) {
        for (x = right ? TG_SIZE / 2 + 8 : 8; x < (right ? TG_SIZE - 8 : TG_SIZE / 2 - 8); x++) {
            sum += (float)d->pixels[(size_t)y * d->stride + x * 3 + ch];
            count++;
        }
    }

    return sum / (float)count;
}

/* 小さな JPEG (icc / exif は任意) をデコードする */
static void decode_tagged(const uint8_t *icc, size_t icc_size, const uint8_t *exif,
                          size_t exif_size, hdrg_decoded_t *out)
{
    uint8_t pixels[16 * 16 * 3], *jpeg;
    size_t jpeg_size;

    memset(pixels, 128, sizeof(pixels));

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_test_jpeg_rgb(&g_alloc, pixels, 16, 16, 16 * 3, 90, icc, icc_size,
                                             exif, exif_size, &jpeg, &jpeg_size));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, jpeg, jpeg_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, out));

    hdrg_mem_free(&g_alloc, jpeg);
}

static void assert_color_is(const hdrg_color_t *color, uint8_t primaries)
{
    hdrg_color_t reference;
    uint8_t i;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(primaries, HDRG_TRANSFER_SRGB, &reference));

    for (i = 0; i < 9; i++) {
        HDRG_TEST_NEAR(1e-9, reference.to_xyz[i], color->to_xyz[i]);
    }
}

#ifdef HDRG_WITH_LCMS2
/* P3 の code (sRGB の曲線) を sRGB の線形値へ。sRGB の外なら負の成分を持つ */
static void p3_codes_to_srgb(const float codes[3], double out[3])
{
    static const double p3_to_srgb[9] = {
        1.2249401762805598,  -0.2249401762805598, 0.0,
        -0.0420569547096881, 1.0420569547096881,  0.0,
        -0.0196375545903344, -0.0786360455506319, 1.0982736001409663,
    };
    uint8_t i;
    double lin[3];

    for (i = 0; i < 3; i++) {
        lin[i] = (double)hdrg_math_srgb_eotf(codes[i] / 255.0f);
    }

    for (i = 0; i < 3; i++) {
        out[i] = p3_to_srgb[i * 3] * lin[0] + p3_to_srgb[i * 3 + 1] * lin[1] +
                 p3_to_srgb[i * 3 + 2] * lin[2];
    }
}
#endif

static void test_expansion_direction_and_protection(void)
{
#ifdef HDRG_WITH_LCMS2
    static const uint8_t red[3] = {230, 20, 20}, gray[3] = {128, 128, 128};
    hdrg_decoded_t expanded, colorimetric;
    float lin[3], g_pred;

    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, red, gray, 0.5f, HDRG_SDR_EOTF_AUTO,
                 &expanded);
    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, red, gray, 0.0f, HDRG_SDR_EOTF_AUTO,
                 &colorimetric);

    /* 拡張したベースは P3 でタグされる */
    TEST_ASSERT_NOT_NULL(expanded.icc);
    TEST_ASSERT_FALSE(hdrg_color_within(&expanded.color, HDRG_GAMUT_SRGB));
    assert_color_is(&expanded.color, HDRG_PRIMARIES_P3_D65);

    /* 拡張しない sRGB の入力は sRGB のまま (ICC なし、code も元のまま) */
    TEST_ASSERT_NULL(colorimetric.icc);
    assert_color_is(&colorimetric.color, HDRG_PRIMARIES_BT709);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 230.0f, area_mean(&colorimetric, false, 0));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 20.0f, area_mean(&colorimetric, false, 1));

    /* 測色的 sRGB -> P3 の G code 理論値 */
    lin[0] = hdrg_math_srgb_eotf(230.0f / 255.0f);
    lin[1] = hdrg_math_srgb_eotf(20.0f / 255.0f);
    g_pred = hdrg_math_srgb_oetf(0.033194f * lin[0] + 0.966806f * lin[1]) * 255.0f;

    /* 彩度の高い赤: G code は測色的変換 (色不変) より低い = P3 の赤へ伸びる */
    TEST_ASSERT_TRUE(area_mean(&expanded, false, 1) < g_pred - 4.0f);
    TEST_ASSERT_TRUE(area_mean(&expanded, false, 0) > 180.0f);

    /* 無彩色は保護: P3 でも同一 code (JPEG 誤差のみ) */
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 128.0f, area_mean(&expanded, true, 0));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 128.0f, area_mean(&expanded, true, 1));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 128.0f, area_mean(&colorimetric, true, 1));

    hdrg_decoded_free(&g_alloc, &expanded);
    hdrg_decoded_free(&g_alloc, &colorimetric);
#else
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

/* sRGB より広い入力は拡張なしでも P3 のベースになり、sRGB の外の色が残る */
static void test_wide_input_not_squashed(void)
{
#ifdef HDRG_WITH_LCMS2
    static const uint8_t sources[3][2] = {{HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB},
                                          {HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT709},
                                          {HDRG_PRIMARIES_PROPHOTO, HDRG_TRANSFER_PROPHOTO}},
                         green[3] = {0, 200, 0}, gray[3] = {128, 128, 128};
    hdrg_decoded_t base;
    hdrg_color_t source;
    uint8_t s, c;
    double srgb[3];
    float codes[3], pred;

    for (s = 0; s < 3; s++) {
        convert_base(sources[s][0], sources[s][1], green, gray, 0.0f, HDRG_SDR_EOTF_AUTO, &base);

        TEST_ASSERT_NOT_NULL(base.icc);
        assert_color_is(&base.color, HDRG_PRIMARIES_P3_D65);

        for (c = 0; c < 3; c++) {
            codes[c] = area_mean(&base, false, c);
        }

        p3_codes_to_srgb(codes, srgb);
        TEST_ASSERT_TRUE(srgb[0] < -0.02 || srgb[2] < -0.02);

        /* 無彩色は入力の曲線で復号した明るさのまま */
        TEST_ASSERT_TRUE(hdrg_color_from_codes(sources[s][0], sources[s][1], &source));
        pred =
            hdrg_math_srgb_oetf((float)hdrg_curve_eval(&source.curve[0], 128.0 / 255.0)) * 255.0f;
        TEST_ASSERT_FLOAT_WITHIN(2.0f, pred, area_mean(&base, true, 1));

        hdrg_decoded_free(&g_alloc, &base);
    }
#else
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

#ifdef HDRG_WITH_LCMS2
/* lcms2 が書く Adobe RGB 相当のプロファイル (D50 へ順応した原色 + para 曲線) */
static uint8_t *adobe_icc(size_t *size)
{
    static const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
    static const cmsCIExyYTRIPLE adobe = {
        {0.64, 0.33, 1.0},
        {0.21, 0.71, 1.0},
        {0.15, 0.06, 1.0},
    };
    cmsHPROFILE profile;
    cmsToneCurve *gamma, *curves[3];
    cmsUInt32Number icc_size;
    uint8_t *icc;

    gamma = cmsBuildGamma(NULL, 2.2);
    curves[0] = gamma;
    curves[1] = gamma;
    curves[2] = gamma;
    profile = cmsCreateRGBProfile(&d65, &adobe, curves);
    cmsFreeToneCurve(gamma);
    TEST_ASSERT_NOT_NULL(profile);

    TEST_ASSERT_TRUE(cmsSaveProfileToMem(profile, NULL, &icc_size));
    icc = (uint8_t *)malloc(icc_size);
    TEST_ASSERT_NOT_NULL(icc);
    TEST_ASSERT_TRUE(cmsSaveProfileToMem(profile, icc, &icc_size));
    cmsCloseProfile(profile);

    *size = icc_size;

    return icc;
}
#endif

/* JPEG の色: ICC の行列 + TRC、ICC がなければ EXIF の Interop "R03" (Adobe RGB)、既定 sRGB */
static void test_jpeg_color_resolution(void)
{
    /* "Exif\0\0" + TIFF (II): IFD0 -> Exif IFD (ColorSpace 0xFFFF) -> Interop IFD ("R03") */
    static const uint8_t exif_r03[80] = {
        'E',  'x',  'i',  'f',  0, 0,                                  /* APP1 header */
        'I',  'I',  42,   0,    8, 0, 0, 0,                            /* TIFF, IFD0 at 8 */
        1,    0,    0x69, 0x87, 4, 0, 1, 0, 0,  0, 26,   0,    0,   0, /* Exif IFD at 26 */
        0,    0,    0,    0,                                           /* no next IFD */
        2,    0,    0x01, 0xA0, 3, 0, 1, 0, 0,  0, 0xFF, 0xFF, 0,   0, /* uncalibrated */
        0x05, 0xA0, 4,    0,    1, 0, 0, 0, 56, 0, 0,    0,            /* Interop IFD at 56 */
        0,    0,    0,    0,                                           /* no next IFD */
        1,    0,    1,    0,    2, 0, 4, 0, 0,  0, 'R',  '0',  '3', 0, /* index "R03" */
        0,    0,    0,    0,                                           /* no next IFD */
    };
    hdrg_decoded_t decoded;
#ifdef HDRG_WITH_LCMS2
    uint8_t *icc;
    size_t icc_size;
#endif

    decode_tagged(NULL, 0, NULL, 0, &decoded);
    TEST_ASSERT_TRUE(decoded.color.srgb_curve);
    assert_color_is(&decoded.color, HDRG_PRIMARIES_BT709);
    hdrg_decoded_free(&g_alloc, &decoded);

    decode_tagged(NULL, 0, exif_r03, sizeof(exif_r03), &decoded);
    TEST_ASSERT_FALSE(decoded.color.srgb_curve);
    assert_color_is(&decoded.color, HDRG_PRIMARIES_ADOBE_RGB);
    HDRG_TEST_NEAR(1e-9, 563.0 / 256.0, decoded.color.curve[1].g);
    hdrg_decoded_free(&g_alloc, &decoded);

#ifdef HDRG_WITH_LCMS2
    icc = adobe_icc(&icc_size);

    /* ICC は EXIF より優先される */
    decode_tagged(icc, icc_size, exif_r03, sizeof(exif_r03), &decoded);
    free(icc);

    assert_color_is(&decoded.color, HDRG_PRIMARIES_ADOBE_RGB);
    HDRG_TEST_NEAR(1e-3, 2.2, decoded.color.curve[0].g);
    HDRG_TEST_NEAR(
        1e-6, 0.5,
        hdrg_curve_eval(&decoded.color.curve[2], hdrg_curve_inverse(&decoded.color.curve[2], 0.5)));
    hdrg_decoded_free(&g_alloc, &decoded);
#endif
}

#ifdef HDRG_WITH_LCMS2
/* a profile saved (malloc'd) */
static uint8_t *saved_icc(cmsHPROFILE profile, size_t *size)
{
    cmsUInt32Number icc_size;
    uint8_t *icc;

    TEST_ASSERT_NOT_NULL(profile);
    TEST_ASSERT_TRUE(cmsSaveProfileToMem(profile, NULL, &icc_size));
    icc = (uint8_t *)malloc(icc_size);
    TEST_ASSERT_NOT_NULL(icc);
    TEST_ASSERT_TRUE(cmsSaveProfileToMem(profile, icc, &icc_size));
    cmsCloseProfile(profile);
    *size = icc_size;

    return icc;
}

/*
 * an older lcms2's cicp (a tag of 16 bytes, written 4 bytes late) where the ICC specification has
 * it; lcms2 2.20 writes the 12 bytes of the specification
 */
static void fix_cicp(uint8_t *icc, size_t size)
{
    uint32_t count, i, at, length;

    count = ((uint32_t)icc[128] << 24) | ((uint32_t)icc[129] << 16) | ((uint32_t)icc[130] << 8) |
            icc[131];

    for (i = 0; i < count && 132u + 12u * (i + 1u) <= size; i++) {
        if (memcmp(icc + 132u + 12u * i, "cicp", 4) == 0) {
            at = ((uint32_t)icc[136u + 12u * i] << 24) | ((uint32_t)icc[137u + 12u * i] << 16) |
                 ((uint32_t)icc[138u + 12u * i] << 8) | icc[139u + 12u * i];
            length = ((uint32_t)icc[140u + 12u * i] << 24) | ((uint32_t)icc[141u + 12u * i] << 16) |
                     ((uint32_t)icc[142u + 12u * i] << 8) | icc[143u + 12u * i];
            TEST_ASSERT_TRUE(at + length <= size);

            if (length >= 16u) {
                memmove(icc + at + 8u, icc + at + 12u, 4);
            }
        }
    }
}

/* a small JPEG of the profile through the whole conversion */
static void convert_tagged(const uint8_t *icc, size_t icc_size)
{
    uint8_t pixels[16 * 16 * 3], *jpeg, *output;
    size_t jpeg_size, output_size, i;

    for (i = 0; i < sizeof(pixels); i++) {
        pixels[i] = (uint8_t)(40u + (i * 7u) % 180u);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_jpeg_rgb(&g_alloc, pixels, 16, 16, 16 * 3, 90, icc,
                                                      icc_size, NULL, 0, &jpeg, &jpeg_size));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert(g_ctx, jpeg, jpeg_size, NULL, NULL, NULL, &output, &output_size));
    hdrg_free(output);
    hdrg_mem_free(&g_alloc, jpeg);
}
#endif

/*
 * The forms of ICC profiles: v2's gamma of a curv's single entry, a sampled curv, a gray
 * profile's kTRC (no transfer code then: the gray's linear one replaced), the v4.4 cicp tag of
 * SDR code points, an A2B0 transform (lcms2 converts the pixels to BT.2020 at 2.2, 16 bits);
 * each through the whole conversion
 */
static void test_icc_forms(void)
{
#ifdef HDRG_WITH_LCMS2
    static const cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
    static const cmsCIExyYTRIPLE adobe = {
        {0.64, 0.33, 1.0},
        {0.21, 0.71, 1.0},
        {0.15, 0.06, 1.0},
    };
    static const cmsVideoSignalType p3 = {12, 13, 0, 1};
    cmsHPROFILE profile;
    cmsToneCurve *gamma, *table, *curves[3];
    cmsPipeline *lut;
    hdrg_decoded_t decoded;
    uint32_t transfer;
    uint16_t samples[256];
    uint8_t *icc;
    size_t icc_size, i;
    double xy[8];

    /* v2: a curv of one entry (the gamma) */
    gamma = cmsBuildGamma(NULL, 2.2);
    curves[0] = gamma;
    curves[1] = gamma;
    curves[2] = gamma;
    profile = cmsCreateRGBProfile(&d65, &adobe, curves);
    cmsSetProfileVersion(profile, 2.1);
    icc = saved_icc(profile, &icc_size);
    decode_tagged(icc, icc_size, NULL, 0, &decoded);
    assert_color_is(&decoded.color, HDRG_PRIMARIES_ADOBE_RGB);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_GAMMA, decoded.color.curve[0].type);
    HDRG_TEST_NEAR(1e-3, 2.2, decoded.color.curve[0].g);
    hdrg_decoded_free(&g_alloc, &decoded);
    convert_tagged(icc, icc_size);
    free(icc);

    /* a sampled curve (gamma 1.8 at 256 points) */
    for (i = 0; i < 256u; i++) {
        samples[i] = (uint16_t)((double)hdrg_math_powf((float)i / 255.0f, 1.8f) * 65535.0 + 0.5);
    }

    table = cmsBuildTabulatedToneCurve16(NULL, 256, samples);
    curves[0] = table;
    curves[1] = table;
    curves[2] = table;
    icc = saved_icc(cmsCreateRGBProfile(&d65, &adobe, curves), &icc_size);
    decode_tagged(icc, icc_size, NULL, 0, &decoded);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_TABLE, decoded.color.curve[1].type);
    HDRG_TEST_NEAR(2e-3, hdrg_math_powf(0.5f, 1.8f), hdrg_curve_eval(&decoded.color.curve[1], 0.5));
    hdrg_decoded_free(&g_alloc, &decoded);
    convert_tagged(icc, icc_size);
    free(icc);

    /* gray: its kTRC on every channel, no transfer code of its own */
    icc = saved_icc(cmsCreateGrayProfile(&d65, gamma), &icc_size);
    decode_tagged(icc, icc_size, NULL, 0, &decoded);
    HDRG_TEST_NEAR(1e-3, 2.2, decoded.color.curve[2].g);
    hdrg_color_describe(&decoded.color, xy, &transfer);
    TEST_ASSERT_EQUAL_UINT32(2, transfer);
    hdrg_decoded_free(&g_alloc, &decoded);
    convert_tagged(icc, icc_size);
    free(icc);

    /* v4.4's cicp of P3 and the sRGB curve */
    profile = cmsCreateRGBProfile(&d65, &adobe, curves);
    cmsSetProfileVersion(profile, 4.4);
    TEST_ASSERT_TRUE(cmsWriteTag(profile, cmsSigcicpTag, &p3));
    icc = saved_icc(profile, &icc_size);
    fix_cicp(icc, icc_size);
    decode_tagged(icc, icc_size, NULL, 0, &decoded);
    assert_color_is(&decoded.color, HDRG_PRIMARIES_P3_D65);
    TEST_ASSERT_TRUE(decoded.color.srgb_curve);
    hdrg_decoded_free(&g_alloc, &decoded);
    free(icc);

    /* A2B0: lcms2's transform into BT.2020 at 2.2 */
    profile = cmsCreateRGBProfile(&d65, &adobe, curves);
    lut = cmsPipelineAlloc(NULL, 3, 3);
    TEST_ASSERT_NOT_NULL(lut);
    TEST_ASSERT_TRUE(
        cmsPipelineInsertStage(lut, cmsAT_END, cmsStageAllocToneCurves(NULL, 3, curves)));
    TEST_ASSERT_TRUE(cmsWriteTag(profile, cmsSigAToB0Tag, lut));
    cmsPipelineFree(lut);
    icc = saved_icc(profile, &icc_size);
    decode_tagged(icc, icc_size, NULL, 0, &decoded);
    assert_color_is(&decoded.color, HDRG_PRIMARIES_BT2020);
    TEST_ASSERT_EQUAL_UINT32(16, decoded.bit_depth);
    hdrg_decoded_free(&g_alloc, &decoded);
    convert_tagged(icc, icc_size);
    free(icc);

    cmsFreeToneCurve(table);
    cmsFreeToneCurve(gamma);
#else
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

/* sdr_eotf: sRGB でタグされた入力を 2.2 で復号すると暗部が締まる */
static void test_sdr_eotf_decoding(void)
{
    static const uint8_t dark[3] = {40, 40, 40}, light[3] = {200, 200, 200};
    hdrg_decoded_t srgb, gamma22;
    float pred;

    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, dark, light, 0.0f, HDRG_SDR_EOTF_SRGB,
                 &srgb);
    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, dark, light, 0.0f, HDRG_SDR_EOTF_GAMMA22,
                 &gamma22);

    TEST_ASSERT_FLOAT_WITHIN(2.0f, 40.0f, area_mean(&srgb, false, 1));

    pred = hdrg_math_srgb_oetf(hdrg_math_powf(40.0f / 255.0f, 2.2f)) * 255.0f;
    TEST_ASSERT_FLOAT_WITHIN(2.0f, pred, area_mean(&gamma22, false, 1));
    TEST_ASSERT_TRUE(area_mean(&gamma22, false, 1) < area_mean(&srgb, false, 1) - 3.0f);

    hdrg_decoded_free(&g_alloc, &srgb);
    hdrg_decoded_free(&g_alloc, &gamma22);
}

#ifndef HDRG_WITH_LCMS2
/* no P3 profile can be written: the base stays sRGB, the same as without expansion */
static void test_no_expansion_without_lcms2(void)
{
    static const uint8_t red[3] = {230, 20, 20}, gray[3] = {128, 128, 128};
    hdrg_decoded_t expanded, colorimetric;

    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, red, gray, 0.5f, HDRG_SDR_EOTF_AUTO,
                 &expanded);
    convert_base(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, red, gray, 0.0f, HDRG_SDR_EOTF_AUTO,
                 &colorimetric);

    TEST_ASSERT_NULL(expanded.icc);
    assert_color_is(&expanded.color, HDRG_PRIMARIES_BT709);
    TEST_ASSERT_EQUAL_size_t((size_t)expanded.stride * expanded.height,
                             (size_t)colorimetric.stride * colorimetric.height);
    TEST_ASSERT_EQUAL_MEMORY(colorimetric.pixels, expanded.pixels,
                             (size_t)expanded.stride * expanded.height);

    hdrg_decoded_free(&g_alloc, &expanded);
    hdrg_decoded_free(&g_alloc, &colorimetric);
}
#endif

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    int failures;

    g_alloc = hdrg_test_allocator();

    if (hdrg_test_context(&g_ctx, NULL, 0) != HDRG_OK) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_expansion_direction_and_protection);
    RUN_TEST(test_wide_input_not_squashed);
    RUN_TEST(test_jpeg_color_resolution);
    RUN_TEST(test_icc_forms);
    RUN_TEST(test_sdr_eotf_decoding);
#ifndef HDRG_WITH_LCMS2
    RUN_TEST(test_no_expansion_without_lcms2);
#endif
    failures = UNITY_END();

    hdrg_context_destroy(g_ctx);

    return failures;
}
