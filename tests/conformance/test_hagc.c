/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../test_util.h"
#include "hdrgrading/foundation/math.h"
#include "hdrgrading/video.h"
#include "hagc.h"
#include "io/io.h"

/*
 * ICC HAGC タグ (SMPTE ST 2094-50): ペイロードの往復 (測った形の曲線の点と
 * PCHIP の評価、出力が下がらない)、既知のバイト列 (読み取りと hdrg_st2094_50_t35 の
 * 書き出し)、不正入力の拒否、
 * 単層 PQ とハイブリッドの出力が、適用で測った元の SDR を戻す曲線を運ぶこと
 * (一様な灰色の SDR を 2% 以内で戻す、代替は 1 つ)。
 */

/* 出力 JPEG の APP2 ICC チャンク (単一チャンク前提) から HAGC タグを取り出す */
#ifdef HDRG_WITH_LCMS2
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool find_hagc(const uint8_t *jpeg, size_t size, hdrg_hagc_t *out)
{
    const uint8_t *icc, *entry;
    uint32_t count, i, offset, tag_size;
    size_t pos, icc_size;

    for (pos = 2; pos + 18 < size; pos++) {
        if (jpeg[pos] != 0xFF || jpeg[pos + 1] != 0xE2 ||
            memcmp(jpeg + pos + 4, "ICC_PROFILE", 12) != 0) {
            continue;
        }

        icc = jpeg + pos + 4 + 14;
        icc_size = (((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3]) - 2 - 14;
        count = be32(icc + 128);
        for (i = 0; i < count; i++) {
            entry = icc + 132 + (size_t)i * 12;
            offset = be32(entry + 4);
            tag_size = be32(entry + 8);

            if (memcmp(entry, "HAGC", 4) != 0 || offset + tag_size > icc_size) {
                continue;
            }

            if (memcmp(icc + offset, "hagc", 4) != 0) {
                return false;
            }

            return hdrg_hagc_parse(icc + offset + 12, be32(icc + offset + 8), out);
        }

        return false;
    }

    return false;
}
#endif

/* 測った形の曲線 (暗部は 0、明部ほど下げる) の往復と PCHIP の評価 */
static void test_roundtrip_curve(void)
{
    static const float xs[6] = {0.01f, 0.1f, 0.5f, 1.0f, 2.0f, 4.0f},
                       ys[6] = {0.0f, -0.05f, -0.4f, -0.9f, -1.6f, -2.3f};
    hdrg_st2094_50_t built;
    hdrg_hagc_t parsed;
    uint8_t payload[HDRG_ST2094_50_T35_MAX], i;
    size_t size;
    float x, prev, g;

    memset(&built, 0, sizeof(built));
    built.reference_white = 203.0f;
    built.baseline_headroom = 2.3f;
    built.gamut = HDRG_GAMUT_BT2020;
    built.count = 6;
    built.valid = true;

    for (i = 0; i < 6u; i++) {
        built.x[i] = xs[i];
        built.y[i] = ys[i];
    }

    size = hdrg_st2094_50_t35(&built, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(9u + 6u * 4u, (uint32_t)size);
    TEST_ASSERT_TRUE(hdrg_hagc_parse(payload, size, &parsed));

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.3f, parsed.baseline);
    TEST_ASSERT_EQUAL_UINT8(1, parsed.alt_count);
    TEST_ASSERT_EQUAL_UINT8(2, parsed.chromaticities);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, parsed.alt[0].headroom);
    TEST_ASSERT_EQUAL_UINT8(0, parsed.alt[0].mix_type);
    TEST_ASSERT_EQUAL_UINT8(1, parsed.alt[0].pchip);
    TEST_ASSERT_EQUAL_UINT8(6, parsed.alt[0].count);

    for (i = 0; i < 6u; i++) {
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, xs[i], parsed.alt[0].x[i]);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, ys[i], hdrg_hagc_eval(&parsed.alt[0], xs[i]));
    }

    /* 点の間も出力 (x 2^gain) は下がらない */
    prev = 0.0f;

    for (x = 0.01f; x <= 4.0f; x += 0.01f) {
        g = hdrg_hagc_eval(&parsed.alt[0], x);
        TEST_ASSERT_TRUE(x * exp2f(g) >= prev * 0.999f);

        prev = x * exp2f(g);
    }
}

static void test_known_bytes(void)
{
    /* baseline 2.0、headroom 0 の alternate 1 本、制御点 1 個 (x=0, y=0, PCHIP) */
    static const uint8_t explicit_curve[] = {0x00, 0x40, 0x4E, 0x20, 0x10, 0x00, 0x00,
                                             0x00, 0x04, 0x00, 0x00, 0x00, 0x00},
                         rwtmo[] = {0x00, 0x40, 0x4E, 0x20, 0x80};
    hdrg_hagc_t hagc;

    TEST_ASSERT_TRUE(hdrg_hagc_parse(explicit_curve, sizeof(explicit_curve), &hagc));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, hagc.baseline);
    TEST_ASSERT_EQUAL_UINT8(1, hagc.alt_count);
    TEST_ASSERT_EQUAL_UINT8(1, hagc.alt[0].count);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, hdrg_hagc_eval(&hagc.alt[0], 0.0f));

    TEST_ASSERT_TRUE(hdrg_hagc_parse(rwtmo, sizeof(rwtmo), &hagc));
    TEST_ASSERT_TRUE(hagc.rwtmo);
}

/*
 * hdrg_st2094_50_t35: 制御点 1 個の曲線が既知のバイト列になる、203 でない基準の白は
 * baseline の前に入る、バッファなし・短いバッファでは大きさだけを返して書かない、
 * 有効でない曲線は 0
 */
static void test_t35_writes(void)
{
    static const uint8_t explicit_curve[] = {0x00, 0x40, 0x4E, 0x20, 0x10, 0x00, 0x00,
                                             0x00, 0x04, 0x00, 0x00, 0x00, 0x00},
                         white_head[] = {0x00, 0xC0, 0x05, 0xDC, 0x4E, 0x20, 0x14};
    hdrg_st2094_50_t curve;
    hdrg_hagc_t hagc;
    uint8_t payload[HDRG_ST2094_50_T35_MAX];

    memset(&curve, 0, sizeof(curve));
    curve.reference_white = 203.0f;
    curve.baseline_headroom = 2.0f;
    curve.gamut = HDRG_GAMUT_SRGB;
    curve.count = 1;
    curve.valid = true;
    TEST_ASSERT_EQUAL_UINT32(sizeof(explicit_curve),
                             (uint32_t)hdrg_st2094_50_t35(&curve, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(explicit_curve, payload, sizeof(explicit_curve));
    TEST_ASSERT_EQUAL_UINT32(sizeof(explicit_curve), (uint32_t)hdrg_st2094_50_t35(&curve, NULL, 0));

    memset(payload, 0xA5, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(sizeof(explicit_curve),
                             (uint32_t)hdrg_st2094_50_t35(&curve, payload, 4));
    TEST_ASSERT_EQUAL_UINT8(0xA5, payload[0]);

    curve.reference_white = 300.0f;
    curve.gamut = HDRG_GAMUT_P3;
    TEST_ASSERT_EQUAL_UINT32(sizeof(explicit_curve) + 2u,
                             (uint32_t)hdrg_st2094_50_t35(&curve, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(white_head, payload, sizeof(white_head));
    TEST_ASSERT_TRUE(hdrg_hagc_parse(payload, sizeof(explicit_curve) + 2u, &hagc));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 300.0f, hagc.reference_white);
    TEST_ASSERT_EQUAL_UINT8(1, hagc.chromaticities);

    curve.valid = false;
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_st2094_50_t35(&curve, payload, sizeof(payload)));
    curve.valid = true;
    curve.count = 0;
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_st2094_50_t35(&curve, payload, sizeof(payload)));
    curve.count = HDRG_ST2094_50_POINTS + 1u;
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_st2094_50_t35(&curve, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_st2094_50_t35(NULL, payload, sizeof(payload)));
}

static void test_rejects_malformed(void)
{
    static const uint8_t reserved_set[] = {0x00, 0x41}, min_version[] = {0x04, 0x00},
                         truncated[] = {0x00, 0x40, 0x4E, 0x20, 0x10, 0x00},
                         same_headroom[] = {0x00, 0x40, 0x4E, 0x20, 0x10, 0x4E, 0x20,
                                            0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    hdrg_hagc_t hagc;

    TEST_ASSERT_FALSE(hdrg_hagc_parse(reserved_set, sizeof(reserved_set), &hagc));
    TEST_ASSERT_FALSE(hdrg_hagc_parse(min_version, sizeof(min_version), &hagc));
    TEST_ASSERT_FALSE(hdrg_hagc_parse(truncated, sizeof(truncated), &hagc));
    TEST_ASSERT_FALSE(hdrg_hagc_parse(same_headroom, sizeof(same_headroom), &hagc));
}

/*
 * 単層 PQ とハイブリッドの出力: 一様な灰色 (GAME = gamma 2.2) の HAGC の代替
 * 1 つが、適用で測った元の SDR を 2% 以内で戻す。hagc = false ではタグを
 * 書かない
 */
static void test_outputs_carry_fitted_hagc(void)
{
#ifdef HDRG_WITH_LCMS2
    static const uint8_t modes[2] = {HDRG_OUTPUT_PQ, HDRG_OUTPUT_HYBRID};
    static const char *const no_hagc[] = {"hagc=0"};
    hdrg_display_caps_t display;
    hdrg_context_t *ctx;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_hagc_t hagc;
    uint32_t x, y;
    uint8_t *pixels, *out, m;
    size_t out_size;
    float want, got, dx, dy, level;

    pixels = (uint8_t *)malloc(128u * 96u * 3u);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(pixels, 150, 128u * 96u * 3u);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 128;
    raw.height = 96;
    raw.bit_depth = 8;
    raw.stride = 128 * 3;
    hdrg_params_init(&params, HDRG_PRESET_GAME);
    want = powf(150.0f / 255.0f, 2.2f);

    for (m = 0; m < 2u; m++) {
        hdrg_encode_params_init(&encode);
        encode.output_mode = modes[m];
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, &out, &out_size));
        TEST_ASSERT_TRUE(find_hagc(out, out_size, &hagc));
        TEST_ASSERT_EQUAL_UINT8(1, hagc.alt_count);
        TEST_ASSERT_TRUE(hagc.alt[0].count >= 1u);
        TEST_ASSERT_TRUE(hagc.baseline > 0.5f);
        got = hagc.alt[0].x[0] * exp2f(hdrg_hagc_eval(&hagc.alt[0], hagc.alt[0].x[0]));
        TEST_ASSERT_FLOAT_WITHIN(0.02f * want, want, got);
        hdrg_free(out);

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(ctx, "hdrg.jpeg", no_hagc, 1));
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, &out, &out_size));
        TEST_ASSERT_FALSE(find_hagc(out, out_size, &hagc));
        hdrg_free(out);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_writer(ctx, NULL, NULL));
    }

    /*
     * a lamp's glow on a ramp: carried up to a 1000-nit display, not to a 10000 over 100 nits
     * (its light past 6 stops over the SDR white: ST 2094-50's limit)
     */
    for (y = 0; y < 96u; y++) {
        for (x = 0; x < 128u; x++) {
            dx = (float)x - 64.0f;
            dy = (float)y - 48.0f;
            level =
                20.0f + 160.0f * (float)x / 127.0f + 255.0f * expf(-(dx * dx + dy * dy) / 72.0f);
            memset(pixels + ((size_t)y * 128u + x) * 3u,
                   (level > 255.0f) ? 255 : (int)(level + 0.5f), 3u);
        }
    }

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;
    hdrg_display_caps_init(&display);
    display.peak_nits = 1000.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, &display, &encode, &out, &out_size));
    TEST_ASSERT_TRUE(find_hagc(out, out_size, &hagc));
    hdrg_free(out);

    display.peak_nits = 10000.0f;
    display.sdr_white_nits = 100.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, &display, &encode, &out, &out_size));
    TEST_ASSERT_FALSE(find_hagc(out, out_size, &hagc));
    hdrg_free(out);

    hdrg_context_destroy(ctx);
    free(pixels);
#else
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

/*
 * kodim23 (PHOTO, the SDR shown with gamma 2.2) as single-layer PQ: the
 * file's HAGC gives the source back from the decoded PQ (max(R, G, B) in
 * the output's P3, 8-bit gamma 2.2 codes) closer than the generic tone map
 * the stills' HAGC had (knee 0.75: identity to it, a C1 approach to 1)
 */
static void test_kodim_restores(void)
{
#ifdef HDRG_WITH_LCMS2
    const hdrg_allocator_t alloc = hdrg_test_allocator();
    static const char *const best[] = {"quality=100"};
    static const float p3[9] = {0.8225f, 0.1774f, 0.0f,    0.0332f, 0.9669f,
                                0.0f,    0.0171f, 0.0724f, 0.9108f}; /* BT.709 -> P3 */
    FILE *fp;
    hdrg_context_t *ctx;
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_decoded_t source, output;
    hdrg_hagc_t hagc;
    uint32_t x, y;
    uint8_t c, *data, *out;
    long size;
    size_t out_size;
    char line[160];
    double ours, theirs;
    float lin[3], m, s, v, back, generic, target;

    HDRG_TEST_REQUIRE_BUILTIN(PNG);

    fp = fopen(HDRG_TEST_ASSETS_DIR "/kodim23.png", "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));

    size = ftell(fp);
    TEST_ASSERT_TRUE(size > 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_SET));

    data = (uint8_t *)malloc((size_t)size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_size_t((size_t)size, fread(data, 1, (size_t)size, fp));

    (void)fclose(fp);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.sdr_eotf = HDRG_SDR_EOTF_GAMMA22;
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_writer(ctx, "hdrg.jpeg", best, 1));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert(ctx, data, (size_t)size, &params, NULL, &encode, &out, &out_size));
    TEST_ASSERT_TRUE(find_hagc(out, out_size, &hagc));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, data, (size_t)size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &source));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&alloc, NULL, out, out_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &output));
    TEST_ASSERT_EQUAL_UINT32(source.width, output.width);

    ours = 0.0;
    theirs = 0.0;

    for (y = 0; y < source.height; y++) {
        for (x = 0; x < source.width; x++) {
            m = 0.0f;
            target = 0.0f;

            for (c = 0; c < 3u; c++) {
                v = hdrg_math_pq_eotf((float)output.pixels[(size_t)y * output.stride + x * 3u + c] /
                                      255.0f) /
                    203.0f;
                m = (v > m) ? v : m;
                lin[c] = powf((float)source.pixels[(size_t)y * source.stride + x * 3u + c] / 255.0f,
                              2.2f);
            }

            for (c = 0; c < 3u; c++) {
                s = p3[c * 3u] * lin[0] + p3[c * 3u + 1u] * lin[1] + p3[c * 3u + 2u] * lin[2];
                target = (s > target) ? s : target;
            }

            back = m * exp2f(hdrg_hagc_eval(&hagc.alt[0], m));
            generic = (m <= 0.75f) ? m : 0.75f + 0.25f * (1.0f - expf(-(m - 0.75f) / 0.25f));
            target = powf(target, 1.0f / 2.2f) * 255.0f;
            ours += fabs((double)(powf((back < 1.0f) ? back : 1.0f, 1.0f / 2.2f) * 255.0f) -
                         (double)target);
            theirs += fabs((double)(powf(generic, 1.0f / 2.2f) * 255.0f) - (double)target);
        }
    }

    ours /= (double)source.width * (double)source.height;
    theirs /= (double)source.width * (double)source.height;
    snprintf(line, sizeof(line), "kodim23 SDR back: %.2f codes off (the generic tone map's %.2f)",
             ours, theirs);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(ours < theirs);

    hdrg_decoded_free(&alloc, &source);
    hdrg_decoded_free(&alloc, &output);
    hdrg_free(out);
    hdrg_context_destroy(ctx);
    free(data);
#else
    TEST_IGNORE_MESSAGE("lcms2 disabled");
#endif
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_curve);
    RUN_TEST(test_known_bytes);
    RUN_TEST(test_t35_writes);
    RUN_TEST(test_rejects_malformed);
    RUN_TEST(test_outputs_carry_fitted_hagc);
    RUN_TEST(test_kodim_restores);

    return UNITY_END();
}
