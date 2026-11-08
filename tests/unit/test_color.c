/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "core/color.h"
#include "core/image.h"
#include "hdrgrading/foundation/math.h"

/*
 * 色: 原色と伝達特性の解決、Oklab の境界相対の拡張と圧縮、3D LUT。
 * 恒等写像と無彩色は LUT を通しても変わらない、拡張は彩度の高い色だけを P3 側へ
 * 伸ばす (肌・パステルは保つ)、sRGB より広いソースは sRGB へ潰れない、出力は単調。
 */
#define TC_STEP     3u   /* 8-bit の立方体を間引く刻み */
#define TC_CODE_TOL 0.05 /* 恒等写像の許容誤差 (出力コード) */
#define TC_SAMPLES  1000u

static hdrg_allocator_t g_alloc;

/* 8-bit または 16-bit のソースの EOTF 表 (本体と同じ作り方) */
static void source_of(const hdrg_color_t *color, uint32_t bit_depth, hdrg_source_t *src)
{
    hdrg_raw_image_t raw;
    uint8_t pixel[6];

    memset(&raw, 0, sizeof(raw));
    memset(pixel, 0, sizeof(pixel));
    raw.pixels = pixel;
    raw.width = 1;
    raw.height = 1;
    raw.bit_depth = bit_depth;
    raw.stride = sizeof(pixel);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, color, src));
}

static void map_of(uint8_t target, float strength, float saturation, hdrg_gamut_map_t *map)
{
    hdrg_params_t params;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.expand_strength = strength;
    params.saturation = saturation;
    hdrg_gamut_map_init(&params, target, map);
}

static void lut_of(const hdrg_color_t *color, const hdrg_source_t *src, const hdrg_gamut_map_t *map,
                   hdrg_lut_t *lut)
{
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_color_lut_build(&g_alloc, NULL, color, src->lut,
                                                        src->entries - 1u, map, lut));
}

/* rgb (gamut の線形値) の Oklab */
static void lab_of(uint8_t gamut, const float rgb[3], double lab[3])
{
    hdrg_color_t space;
    uint8_t i;
    double v[3], xyz[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes((gamut == HDRG_GAMUT_P3) ? HDRG_PRIMARIES_P3_D65
                                                                    : HDRG_PRIMARIES_BT709,
                                           HDRG_TRANSFER_LINEAR, &space));

    for (i = 0; i < 3; i++) {
        v[i] = (double)rgb[i];
    }

    for (i = 0; i < 3; i++) {
        xyz[i] = space.to_xyz[i * 3] * v[0] + space.to_xyz[i * 3 + 1] * v[1] +
                 space.to_xyz[i * 3 + 2] * v[2];
    }

    hdrg_oklab_from_xyz(xyz, lab);
}

static double chroma(const double lab[3])
{
    return sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
}

/* gamut の線形値を sRGB の線形値へ */
static void to_srgb(const hdrg_color_t *from, const float rgb[3], double out[3])
{
    static const double srgb_from_xyz[9] = {
        3.2409699419045226,  -1.5373831775700939, -0.4986107602930034,
        -0.9692436362808796, 1.8759675015077202,  0.0415550574071756,
        0.0556300796969936,  -0.2039769588889765, 1.0569715142428786,
    };
    uint8_t i;
    double xyz[3];

    for (i = 0; i < 3; i++) {
        xyz[i] = from->to_xyz[i * 3] * rgb[0] + from->to_xyz[i * 3 + 1] * rgb[1] +
                 from->to_xyz[i * 3 + 2] * rgb[2];
    }

    for (i = 0; i < 3; i++) {
        out[i] = srgb_from_xyz[i * 3] * xyz[0] + srgb_from_xyz[i * 3 + 1] * xyz[1] +
                 srgb_from_xyz[i * 3 + 2] * xyz[2];
    }
}

static void test_oklab_round_trip(void)
{
    static const double samples[4][3] = {
        {0.9505, 1.0, 1.089},
        {0.4124, 0.2126, 0.0193},
        {0.1805, 0.0722, 0.9505},
        {0.01, 0.02, 0.005},
    };
    uint8_t i, c;
    double lab[3], back[3];

    for (i = 0; i < 4; i++) {
        hdrg_oklab_from_xyz(samples[i], lab);
        hdrg_oklab_to_xyz(lab, back);

        for (c = 0; c < 3; c++) {
            HDRG_TEST_NEAR(1e-6, samples[i][c], back[c]);
        }
    }

    /* D65 の白は L = 1、無彩色 */
    hdrg_oklab_from_xyz(samples[0], lab);
    HDRG_TEST_NEAR(1e-3, 1.0, lab[0]);
    TEST_ASSERT_TRUE(chroma(lab) < 1e-3);
}

static void test_codes_and_luma(void)
{
    static const uint8_t primaries[8] = {
        HDRG_PRIMARIES_BT709,     HDRG_PRIMARIES_BT601_625, HDRG_PRIMARIES_BT601_525,
        HDRG_PRIMARIES_BT2020,    HDRG_PRIMARIES_DCI_P3,    HDRG_PRIMARIES_P3_D65,
        HDRG_PRIMARIES_ADOBE_RGB, HDRG_PRIMARIES_PROPHOTO,
    };
    hdrg_color_t color;
    uint8_t i;
    float coeffs[3];

    for (i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE(hdrg_color_from_codes(primaries[i], HDRG_TRANSFER_SRGB, &color));
        hdrg_color_luma(&color, coeffs);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, coeffs[0] + coeffs[1] + coeffs[2]);
    }

    /* BT.709 の輝度係数 */
    hdrg_gamut_luma(HDRG_GAMUT_SRGB, coeffs);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2126390f, coeffs[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.7151687f, coeffs[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0721923f, coeffs[2]);

    hdrg_gamut_luma(HDRG_GAMUT_P3, coeffs);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2289746f, coeffs[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.6917385f, coeffs[1]);

    /* 予約値・未指定・HDR の伝達特性 (PQ 16, HLG 18) は受けない */
    TEST_ASSERT_FALSE(hdrg_color_from_codes(0, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_FALSE(hdrg_color_from_codes(2, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_FALSE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, 0, &color));
    TEST_ASSERT_FALSE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, 16, &color));
    TEST_ASSERT_FALSE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, 18, &color));
}

static void test_within(void)
{
    hdrg_color_t color;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_TRUE(hdrg_color_within(&color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&color, HDRG_GAMUT_P3));

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_P3_D65, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_FALSE(hdrg_color_within(&color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_TRUE(hdrg_color_within(&color, HDRG_GAMUT_P3));

    TEST_ASSERT_TRUE(
        hdrg_color_from_codes(HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB, &color));
    TEST_ASSERT_FALSE(hdrg_color_within(&color, HDRG_GAMUT_SRGB));
    TEST_ASSERT_FALSE(hdrg_color_within(&color, HDRG_GAMUT_P3));

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT709, &color));
    TEST_ASSERT_FALSE(hdrg_color_within(&color, HDRG_GAMUT_P3));
}

static void test_curve_inverse_round_trip(void)
{
    hdrg_color_t prophoto;
    hdrg_curve_t curves[5];
    uint32_t i;
    uint8_t k;
    double x;

    hdrg_curve_set_srgb(&curves[0]);
    hdrg_curve_set_gamma(2.2, &curves[1]);
    hdrg_curve_set_gamma(1.0, &curves[2]);
    TEST_ASSERT_TRUE(
        hdrg_color_from_codes(HDRG_PRIMARIES_PROPHOTO, HDRG_TRANSFER_PROPHOTO, &prophoto));
    curves[3] = prophoto.curve[0];

    /* ICC の curv 表 (sRGB を 1024 点) */
    memset(&curves[4], 0, sizeof(curves[4]));
    curves[4].type = HDRG_CURVE_TABLE;
    curves[4].count = 1024;
    curves[4].table = (float *)malloc(1024 * sizeof(float));
    TEST_ASSERT_NOT_NULL(curves[4].table);

    for (i = 0; i < 1024; i++) {
        curves[4].table[i] = hdrg_math_srgb_eotf((float)i / 1023.0f);
    }

    for (k = 0; k < 5; k++) {
        for (i = 0; i <= TC_SAMPLES; i++) {
            x = (double)i / TC_SAMPLES;
            HDRG_TEST_NEAR(2e-5, x, hdrg_curve_inverse(&curves[k], hdrg_curve_eval(&curves[k], x)));
        }

        HDRG_TEST_NEAR(1e-9, 0.0, hdrg_curve_eval(&curves[k], 0.0));
        HDRG_TEST_NEAR(1e-6, 1.0, hdrg_curve_eval(&curves[k], 1.0));
    }

    free(curves[4].table);
}

/*
 * 拡張なし・彩度 1・同じ原色なら LUT は恒等写像: 格子をソースのコード値で持つので、
 * どの伝達特性でも補間がそのまま元の値を返す (黒近くの線形区間も含む)。純色の縁
 * (RGB の立方体の辺) も動かない
 */
static void test_identity_exact(void)
{
    static const uint8_t transfers[4] = {HDRG_TRANSFER_SRGB, HDRG_TRANSFER_GAMMA22,
                                         HDRG_TRANSFER_LINEAR, HDRG_TRANSFER_BT709};
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    uint32_t r, g, b;
    uint8_t t, c;
    double err;
    float out[3];

    map_of(HDRG_GAMUT_SRGB, 0.0f, 1.0f, &map);

    for (t = 0; t < 4; t++) {
        TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, transfers[t], &color));
        source_of(&color, 8, &src);
        lut_of(&color, &src, &map, &lut);

        err = 0.0;
        for (r = 0; r < 256; r += TC_STEP) {
            for (g = 0; g < 256; g += TC_STEP) {
                for (b = 0; b < 256; b += TC_STEP) {
                    hdrg_lut_rgb(&lut, r, g, b, out);

                    /* 出力 (sRGB で符号化) の code での誤差 */
                    for (c = 0; c < 3; c++) {
                        err = HDRG_TEST_MAX(
                            err,
                            fabs((double)hdrg_math_srgb_oetf(out[c]) -
                                 (double)hdrg_math_srgb_oetf(
                                     src.lut[c * 256u + ((c == 0) ? r : ((c == 1) ? g : b))])) *
                                255.0);
                    }
                }
            }
        }

        TEST_ASSERT_TRUE(err <= TC_CODE_TOL);

        hdrg_color_lut_free(&g_alloc, &lut);
        hdrg_source_free(&g_alloc, &src);
    }
}

/* 16-bit (SCALE の帯) でも恒等写像 */
static void test_identity_exact_16bit(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    uint32_t r, g, b;
    uint8_t c;
    double err;
    float out[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    map_of(HDRG_GAMUT_SRGB, 0.0f, 1.0f, &map);
    source_of(&color, 16, &src);
    lut_of(&color, &src, &map, &lut);

    err = 0.0;
    for (r = 0; r < 65536; r += 4111) {
        for (g = 0; g < 65536; g += 2053) {
            for (b = 0; b < 65536; b += 997) {
                hdrg_lut_rgb(&lut, r, g, b, out);

                for (c = 0; c < 3; c++) {
                    err = HDRG_TEST_MAX(err,
                                        fabs((double)hdrg_math_srgb_oetf(out[c]) * 255.0 -
                                             (double)((c == 0) ? r : ((c == 1) ? g : b)) / 257.0));
                }
            }
        }
    }

    TEST_ASSERT_TRUE(err <= TC_CODE_TOL);

    hdrg_color_lut_free(&g_alloc, &lut);
    hdrg_source_free(&g_alloc, &src);
}

/* 既定の写像 (P3 へ拡張 + 彩度) でも無彩色は厳密に無彩色で、明るさも変わらない */
static void test_neutrals_exact(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    uint32_t i;
    float out[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    map_of(HDRG_GAMUT_P3, 0.7f, 1.05f, &map);
    source_of(&color, 8, &src);
    lut_of(&color, &src, &map, &lut);

    for (i = 0; i < 256; i++) {
        hdrg_lut_rgb(&lut, i, i, i, out);
        TEST_ASSERT_EQUAL_FLOAT(out[0], out[1]);
        TEST_ASSERT_EQUAL_FLOAT(out[0], out[2]);
        TEST_ASSERT_FLOAT_WITHIN(1e-6f + 1e-5f * src.lut[i], src.lut[i], out[0]);
    }

    hdrg_color_lut_free(&g_alloc, &lut);
    hdrg_source_free(&g_alloc, &src);
}

/* 拡張 0 の sRGB -> P3 は測色的変換 (色は変わらない) */
static void test_colorimetric_p3(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    float out[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    map_of(HDRG_GAMUT_P3, 0.0f, 1.0f, &map);
    source_of(&color, 8, &src);
    lut_of(&color, &src, &map, &lut);

    hdrg_lut_rgb(&lut, 255, 0, 0, out);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.822462f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.033194f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.017083f, out[2]);

    hdrg_color_lut_free(&g_alloc, &lut);
    hdrg_source_free(&g_alloc, &src);
}

/*
 * 拡張: sRGB の境界近くの彩色は P3 側へ伸びる (L と色相はほぼ保つ)。肌と
 * パステルは測色的変換のまま
 */
static void test_expansion_direction_and_protection(void)
{
    static const uint8_t kept[3][3] = {{215, 165, 135}, {190, 170, 170}, {120, 140, 160}};
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t expanded, colorimetric;
    uint8_t i;
    double lab_e[3], lab_c[3], hue;
    float out_e[3], out_c[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    source_of(&color, 8, &src);
    map_of(HDRG_GAMUT_P3, 1.0f, 1.0f, &map);
    lut_of(&color, &src, &map, &expanded);
    map_of(HDRG_GAMUT_P3, 0.0f, 1.0f, &map);
    lut_of(&color, &src, &map, &colorimetric);

    hdrg_lut_rgb(&expanded, 230, 20, 20, out_e);
    hdrg_lut_rgb(&colorimetric, 230, 20, 20, out_c);
    lab_of(HDRG_GAMUT_P3, out_e, lab_e);
    lab_of(HDRG_GAMUT_P3, out_c, lab_c);

    TEST_ASSERT_TRUE(chroma(lab_e) > chroma(lab_c) * 1.05);
    HDRG_TEST_NEAR(0.01, lab_c[0], lab_e[0]);
    hue = (lab_e[1] * lab_c[2] - lab_e[2] * lab_c[1]) / (chroma(lab_e) * chroma(lab_c));
    HDRG_TEST_NEAR(0.03, 0.0, hue);

    /* P3 の外へは出ない */
    TEST_ASSERT_TRUE(out_e[0] <= 1.0f && out_e[1] >= 0.0f && out_e[2] >= 0.0f);

    for (i = 0; i < 3; i++) {
        hdrg_lut_rgb(&expanded, kept[i][0], kept[i][1], kept[i][2], out_e);
        hdrg_lut_rgb(&colorimetric, kept[i][0], kept[i][1], kept[i][2], out_c);
        lab_of(HDRG_GAMUT_P3, out_e, lab_e);
        lab_of(HDRG_GAMUT_P3, out_c, lab_c);
        HDRG_TEST_NEAR(0.02 * chroma(lab_c) + 1e-4, chroma(lab_c), chroma(lab_e));
    }

    hdrg_color_lut_free(&g_alloc, &expanded);
    hdrg_color_lut_free(&g_alloc, &colorimetric);
    hdrg_source_free(&g_alloc, &src);
}

/* 彩度は Oklab のクロマを L・色相を保って拡大縮小し、0 なら同じ L の無彩色 */
static void test_saturation(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t base, vivid, gray;
    double lab_b[3], lab_v[3];
    float out_b[3], out_v[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    source_of(&color, 8, &src);
    map_of(HDRG_GAMUT_SRGB, 0.0f, 1.0f, &map);
    lut_of(&color, &src, &map, &base);
    map_of(HDRG_GAMUT_SRGB, 0.0f, 1.2f, &map);
    lut_of(&color, &src, &map, &vivid);
    map_of(HDRG_GAMUT_SRGB, 0.0f, 0.0f, &map);
    lut_of(&color, &src, &map, &gray);

    hdrg_lut_rgb(&base, 150, 110, 90, out_b);
    hdrg_lut_rgb(&vivid, 150, 110, 90, out_v);
    lab_of(HDRG_GAMUT_SRGB, out_b, lab_b);
    lab_of(HDRG_GAMUT_SRGB, out_v, lab_v);
    HDRG_TEST_NEAR(0.03 * chroma(lab_b), 1.2 * chroma(lab_b), chroma(lab_v));
    HDRG_TEST_NEAR(0.005, lab_b[0], lab_v[0]);

    hdrg_lut_rgb(&gray, 150, 110, 90, out_v);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, out_v[0], out_v[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, out_v[0], out_v[2]);
    lab_of(HDRG_GAMUT_SRGB, out_v, lab_v);
    HDRG_TEST_NEAR(0.005, lab_b[0], lab_v[0]);

    hdrg_color_lut_free(&g_alloc, &base);
    hdrg_color_lut_free(&g_alloc, &vivid);
    hdrg_color_lut_free(&g_alloc, &gray);
    hdrg_source_free(&g_alloc, &src);
}

/*
 * sRGB より広いソース (Adobe RGB / BT.2020 / ProPhoto) は P3 の範囲で保たれ、
 * sRGB へ潰れない。P3 の内側の色は測色的に移る
 */
static void test_wide_sources_kept(void)
{
    static const uint8_t sources[3][2] = {
        {HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB},
        {HDRG_PRIMARIES_BT2020, HDRG_TRANSFER_BT709},
        {HDRG_PRIMARIES_PROPHOTO, HDRG_TRANSFER_PROPHOTO},
    };
    hdrg_color_t color, p3;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    uint8_t s, c;
    double srgb[3], xyz[3], in[3], expect[3];
    float out[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_P3_D65, HDRG_TRANSFER_LINEAR, &p3));
    map_of(HDRG_GAMUT_P3, 0.0f, 1.0f, &map);

    for (s = 0; s < 3; s++) {
        TEST_ASSERT_TRUE(hdrg_color_from_codes(sources[s][0], sources[s][1], &color));
        source_of(&color, 8, &src);
        lut_of(&color, &src, &map, &lut);

        /* 純色の緑は sRGB の外 (sRGB で負の成分)、P3 の内側 */
        hdrg_lut_rgb(&lut, 0, 200, 0, out);
        to_srgb(&p3, out, srgb);
        TEST_ASSERT_TRUE(srgb[0] < -0.02 || srgb[2] < -0.02);

        for (c = 0; c < 3; c++) {
            TEST_ASSERT_TRUE(out[c] >= 0.0f && out[c] <= 1.0f);
        }

        /* 中程度の彩度の色は測色的変換と一致する */
        hdrg_lut_rgb(&lut, 150, 170, 130, out);
        for (c = 0; c < 3; c++) {
            in[c] = (double)src.lut[c * src.entries + ((c == 0) ? 150u : ((c == 1) ? 170u : 130u))];
        }

        for (c = 0; c < 3; c++) {
            xyz[c] = color.to_xyz[c * 3] * in[0] + color.to_xyz[c * 3 + 1] * in[1] +
                     color.to_xyz[c * 3 + 2] * in[2];
        }

        /* XYZ -> P3 (P3 の行列を解いて確かめる代わりに、出力側を XYZ にして比べる) */
        for (c = 0; c < 3; c++) {
            expect[c] = p3.to_xyz[c * 3] * out[0] + p3.to_xyz[c * 3 + 1] * out[1] +
                        p3.to_xyz[c * 3 + 2] * out[2];
            HDRG_TEST_NEAR(3e-3, xyz[c], expect[c]);
        }

        hdrg_color_lut_free(&g_alloc, &lut);
        hdrg_source_free(&g_alloc, &src);
    }
}

/* 既定の写像で原色・無彩色の傾斜は各チャネルで単調 */
static void test_monotone_ramps(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t lut;
    uint32_t k;
    uint8_t ramp, c;
    float out[3], prev[3];

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    map_of(HDRG_GAMUT_P3, 0.7f, 1.05f, &map);
    source_of(&color, 8, &src);
    lut_of(&color, &src, &map, &lut);

    for (ramp = 0; ramp < 4; ramp++) {
        prev[0] = 0.0f;
        prev[1] = 0.0f;
        prev[2] = 0.0f;

        for (k = 0; k < 256; k++) {
            hdrg_lut_rgb(&lut, (ramp == 0 || ramp == 3) ? k : 0, (ramp == 1 || ramp == 3) ? k : 0,
                         (ramp == 2 || ramp == 3) ? k : 0, out);

            /* 傾斜の主成分は増え続ける */
            c = (ramp == 3) ? 1u : ramp;
            TEST_ASSERT_TRUE(out[c] >= prev[c] - 1e-6f);

            prev[0] = out[0];
            prev[1] = out[1];
            prev[2] = out[2];
        }
    }

    hdrg_color_lut_free(&g_alloc, &lut);
    hdrg_source_free(&g_alloc, &src);
}

/* LUT の構築はワーカー数によらず同じ */
static void test_workers_identical(void)
{
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_gamut_map_t map;
    hdrg_lut_t serial, parallel;
    hdrg_exec_t exec;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    map_of(HDRG_GAMUT_P3, 0.7f, 1.05f, &map);
    source_of(&color, 8, &src);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    lut_of(&color, &src, &map, &serial);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_color_lut_build(&g_alloc, &exec, &color, src.lut, 255, &map, &parallel));
    TEST_ASSERT_EQUAL_MEMORY(serial.nodes, parallel.nodes,
                             (size_t)HDRG_LUT_SIZE * HDRG_LUT_SIZE * HDRG_LUT_SIZE * 3 *
                                 sizeof(float));

    hdrg_color_lut_free(&g_alloc, &serial);
    hdrg_color_lut_free(&g_alloc, &parallel);
    hdrg_source_free(&g_alloc, &src);
    hdrg_pool_destroy(exec.pool);
}

/* ICC の値 (D50 へ順応した原色、curv 表) は標準の組へ吸着する */
static void test_snap(void)
{
    static const double srgb_d50[9] = {0.4360, 0.3851, 0.1431, 0.2225, 0.7169,
                                       0.0606, 0.0139, 0.0971, 0.7139},
                        singular[9] = {0.0},
                        far_white[9] = {0.9, 0.0, 0.0, 0.0, 0.9, 0.0, 0.0, 0.0, 0.05};
    hdrg_color_t color, reference;
    uint32_t i;
    uint8_t c;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &reference));

    memset(&color, 0, sizeof(color));
    TEST_ASSERT_TRUE(hdrg_color_set_colorants(srgb_d50, &color));

    for (c = 0; c < 3; c++) {
        color.curve[c].type = HDRG_CURVE_TABLE;
        color.curve[c].count = 1024;
        color.curve[c].table = (float *)malloc(1024 * sizeof(float));
        TEST_ASSERT_NOT_NULL(color.curve[c].table);

        for (i = 0; i < 1024; i++) {
            color.curve[c].table[i] = hdrg_math_srgb_eotf((float)i / 1023.0f);
        }
    }

    hdrg_color_snap(&g_alloc, &color);
    TEST_ASSERT_EQUAL_MEMORY(reference.to_xyz, color.to_xyz, sizeof(color.to_xyz));
    TEST_ASSERT_TRUE(color.srgb_curve);

    for (c = 0; c < 3; c++) {
        TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_SRGB, color.curve[c].type);
        TEST_ASSERT_NULL(color.curve[c].table);
    }

    /* 2.2 のガンマは sRGB ではない */
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_GAMMA22, &color));
    hdrg_color_snap(&g_alloc, &color);
    TEST_ASSERT_FALSE(color.srgb_curve);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_GAMMA, color.curve[0].type);

    /* D65 の白を持つ原色 (順応していない v2 のプロファイル) もそのまま使える */
    memset(&color, 0, sizeof(color));
    TEST_ASSERT_TRUE(hdrg_color_set_colorants(reference.to_xyz, &color));
    hdrg_color_snap(&g_alloc, &color);
    TEST_ASSERT_EQUAL_MEMORY(reference.to_xyz, color.to_xyz, sizeof(color.to_xyz));

    /* 使えない原色 (特異・昼光から遠い白) は拒否する */
    TEST_ASSERT_FALSE(hdrg_color_set_colorants(singular, &color));
    TEST_ASSERT_FALSE(hdrg_color_set_colorants(far_white, &color));
}

/*
 * xy の原色 (PNG の cHRM) は、使えないもの (3 つが同じ・一直線・x + y > 1・
 * y = 0・白が原色の外) を拒否し、色を書き換えない。拒否しないと逆行列が作れず、
 * 未初期化の行列で色を作っていた
 */
static void test_xy_rejected(void)
{
    static const double bt709[8] = {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290},
                        bad[5][8] = {
                            {0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3127, 0.3290},
                            {0.64, 0.33, 0.30, 0.60, 0.47, 0.465, 0.3127, 0.3290},
                            {0.64, 0.33, 0.30, 0.80, 0.15, 0.06, 0.3127, 0.3290},
                            {0.64, 0.0, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290},
                            {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.9, 0.05},
                        };
    hdrg_color_t color, reference, before;
    uint8_t i;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &reference));
    memset(&color, 0, sizeof(color));
    TEST_ASSERT_TRUE(hdrg_color_set_xy(bt709, &color));
    TEST_ASSERT_EQUAL_MEMORY(reference.to_xyz, color.to_xyz, sizeof(color.to_xyz));

    for (i = 0; i < 5u; i++) {
        before = color;
        TEST_ASSERT_FALSE(hdrg_color_set_xy(bad[i], &color));
        TEST_ASSERT_EQUAL_MEMORY(&before, &color, sizeof(color));
    }
}

/* sdr_eotf は sRGB でタグされたソースの曲線だけを置き換える */
static void test_sdr_eotf(void)
{
    hdrg_color_t color;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_color_set_sdr_eotf(&color, HDRG_SDR_EOTF_AUTO);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_SRGB, color.curve[0].type);
    hdrg_color_set_sdr_eotf(&color, HDRG_SDR_EOTF_SRGB);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_SRGB, color.curve[0].type);

    hdrg_color_set_sdr_eotf(&color, HDRG_SDR_EOTF_GAMMA22);
    TEST_ASSERT_EQUAL_UINT8(HDRG_CURVE_GAMMA, color.curve[2].type);
    HDRG_TEST_NEAR(0.0, 2.2, color.curve[2].g);
    TEST_ASSERT_FALSE(color.srgb_curve);

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_color_set_sdr_eotf(&color, HDRG_SDR_EOTF_BT1886);
    HDRG_TEST_NEAR(0.0, 2.4, color.curve[1].g);

    /* sRGB 以外 (Adobe RGB の 2.2 など) はそのまま */
    TEST_ASSERT_TRUE(
        hdrg_color_from_codes(HDRG_PRIMARIES_ADOBE_RGB, HDRG_TRANSFER_ADOBE_RGB, &color));
    hdrg_color_set_sdr_eotf(&color, HDRG_SDR_EOTF_BT1886);
    HDRG_TEST_NEAR(0.0, 563.0 / 256.0, color.curve[0].g);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    g_alloc = hdrg_test_allocator();

    UNITY_BEGIN();
    RUN_TEST(test_oklab_round_trip);
    RUN_TEST(test_codes_and_luma);
    RUN_TEST(test_within);
    RUN_TEST(test_curve_inverse_round_trip);
    RUN_TEST(test_identity_exact);
    RUN_TEST(test_identity_exact_16bit);
    RUN_TEST(test_neutrals_exact);
    RUN_TEST(test_colorimetric_p3);
    RUN_TEST(test_expansion_direction_and_protection);
    RUN_TEST(test_saturation);
    RUN_TEST(test_wide_sources_kept);
    RUN_TEST(test_monotone_ramps);
    RUN_TEST(test_workers_identical);
    RUN_TEST(test_snap);
    RUN_TEST(test_xy_rejected);
    RUN_TEST(test_sdr_eotf);

    return UNITY_END();
}
