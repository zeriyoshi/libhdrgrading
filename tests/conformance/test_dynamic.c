/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/math.h"
#include "core/apply.h"
#include "hdrgrading/video.h"
#include "hagc.h"
#include "io/io.h"

/*
 * A session's dynamic HDR metadata of its output:
 * HDR10+ (hdrg_hdr10plus_t) of a flat gray (R, G, B's largest the
 * frame's MaxCLL, every percentile inside the gray's bin), of a frame of two
 * levels (the low percentiles the dark half's, the high ones the bright
 * half's, rising and at most the MaxCLL, the mean the frame's FALL), the same whatever the
 * threads, and none for a session whose apply is the app's (the state); its
 * T.35 payload (hdrg_hdr10plus_t35) read back field by field. ST 2094-50's
 * gain curve (hdrg_st2094_50_t) giving the SDR back.
 */

#define TD_W      128u
#define TD_H      72u
#define TD_FRAMES 4u

static bool g_ramp;      /* td_frame: a ramp across in place of the two halves ... */
static bool g_scene;     /* ... or a ramp with a lamp's glow in the middle */
static uint16_t *g_kept; /* td_run: the last output's pixels into it (NULL: none) */

/* the scene's code at (x, y): a ramp of 20 to 180 across, a lamp's glow at the middle */
static uint8_t td_scene(uint32_t x, uint32_t y)
{
    const float dx = (float)x - 0.5f * (float)TD_W, dy = (float)y - 0.5f * (float)TD_H;
    float v;

    v = 20.0f + 160.0f * (float)x / (float)(TD_W - 1u) +
        255.0f * hdrg_math_expf(-(dx * dx + dy * dy) / (2.0f * 36.0f));

    return (uint8_t)((v > 255.0f) ? 255.0f : v + 0.5f);
}

/* frame of the left half at dark and the right half at bright (8-bit gray codes); a ramp */
static void td_frame(uint8_t *rgb, uint8_t dark, uint8_t bright)
{
    uint32_t x, y;
    uint8_t v;

    for (y = 0; y < TD_H; y++) {
        for (x = 0; x < TD_W; x++) {
            v = g_ramp ? (uint8_t)(x * 255u / (TD_W - 1u)) : (x < TD_W / 2u) ? dark : bright;
            v = g_scene ? td_scene(x, y) : v;
            memset(rgb + ((size_t)y * TD_W + x) * 3u, v, 3u);
        }
    }
}

/*
 * the SDR's linear light of an 8-bit code as GAME shows it (*want: gamma
 * 2.2, its sdr_eotf) as the curve gives it back at the point nearest it
 */
static double td_restored(const hdrg_st2094_50_t *curve, uint8_t code, double *want)
{
    uint8_t i;
    double best, v;

    *want = (double)hdrg_math_powf((float)code / 255.0f, 2.2f);
    best = 0.0;

    for (i = 0; i < curve->count; i++) {
        v = (double)curve->x[i] * (double)hdrg_math_exp2f(curve->y[i]);
        best = (i == 0 || fabs(v - *want) < fabs(best - *want)) ? v : best;
    }

    return best;
}

/* the frames through a session (GAME, 16-bit PQ out) of threads: the last output's into out */
static void td_run(uint8_t dark, uint8_t bright, uint8_t threads, bool state,
                   hdrg_session_output_t *out)
{
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    hdrg_session_output_t output;
    hdrg_apply_state_t apply;
    hdrg_status_t status;
    uint32_t sent;
    uint16_t *pixels;
    uint8_t *rgb;

    rgb = (uint8_t *)malloc((size_t)TD_W * TD_H * 3u);
    pixels = (uint16_t *)malloc((size_t)TD_W * TD_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(rgb != NULL && pixels != NULL);
    td_frame(rgb, dark, bright);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));

    memset(&format, 0, sizeof(format));
    format.width = TD_W;
    format.height = TD_H;
    format.bit_depth = 8;
    format.stride = (size_t)TD_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_GAME);
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.latency = HDRG_LATENCY_PLAYBACK;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));

    sent = 0;
    memset(out, 0, sizeof(*out));

    for (;;) {
        memset(&output, 0, sizeof(output));
        output.planes[0] = (uint8_t *)pixels;
        output.strides[0] = (size_t)TD_W * 6u;
        memset(&apply, 0, sizeof(apply));
        apply.struct_size = (uint32_t)sizeof(apply);

        status = state ? hdrg_session_receive_state(session, &output, &apply)
                       : hdrg_session_receive_frame(session, &output);
        if (status == HDRG_ERR_AGAIN) {
            memset(&frame, 0, sizeof(frame));
            frame.planes[0] = rgb;
            frame.pts = (int64_t)sent;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TD_FRAMES) ? &frame : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        *out = output;

        if (g_kept != NULL) {
            memcpy(g_kept, pixels, (size_t)TD_W * TD_H * 3u * sizeof(uint16_t));
        }
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(rgb);
    free(pixels);
}

/* the bin of nits' bounds hold v */
static void td_in_bin(float nits, float v)
{
    const uint32_t bin = hdrg_lightstat_bin(nits);

    TEST_ASSERT_TRUE(v >= hdrg_lightstat_lower(bin) && v <= hdrg_lightstat_lower(bin + 1u));
}

/* a flat gray: R, G, B's largest the frame's MaxCLL, the percentiles in its bin */
static void test_flat(void)
{
    hdrg_session_output_t out;
    uint8_t p, c;

    td_run(150, 150, 4, false, &out);
    TEST_ASSERT_TRUE(out.hdr10plus.valid);
    TEST_ASSERT_TRUE(out.max_nits > 50.0f);

    for (c = 0; c < 3u; c++) {
        HDRG_TEST_NEAR(1e-3 * out.max_nits, out.max_nits, out.hdr10plus.maxscl[c]);
    }

    HDRG_TEST_NEAR(1e-6, out.avg_nits, out.hdr10plus.average_maxrgb);

    for (p = 0; p < HDRG_HDR10PLUS_PERCENTILES; p++) {
        td_in_bin(out.max_nits, out.hdr10plus.percentiles[p]);
    }
}

/*
 * two levels: the low percentiles the dark half's bin, the high ones the
 * bright half's, rising; the same whatever the threads
 */
static void test_two_levels(void)
{
    hdrg_session_output_t out, other;
    uint8_t p;
    char line[160];

    td_run(30, 235, 4, false, &out);
    TEST_ASSERT_TRUE(out.hdr10plus.valid);
    snprintf(line, sizeof(line), "p1 %.2f, p50 %.2f, p99 %.2f cd/m^2 (MaxCLL %.1f)",
             (double)out.hdr10plus.percentiles[0], (double)out.hdr10plus.percentiles[4],
             (double)out.hdr10plus.percentiles[8], (double)out.max_nits);
    TEST_MESSAGE(line);

    for (p = 1; p < HDRG_HDR10PLUS_PERCENTILES; p++) {
        TEST_ASSERT_TRUE(out.hdr10plus.percentiles[p] >= out.hdr10plus.percentiles[p - 1u]);
        TEST_ASSERT_TRUE(out.hdr10plus.percentiles[p] <= out.max_nits);
    }

    TEST_ASSERT_TRUE(out.hdr10plus.percentiles[3] < 0.25f * out.max_nits);
    td_in_bin(out.max_nits, out.hdr10plus.percentiles[6]);
    td_in_bin(out.max_nits, out.hdr10plus.percentiles[8]);

    td_run(30, 235, 1, false, &other);
    TEST_ASSERT_EQUAL_MEMORY(&out.hdr10plus, &other.hdr10plus, sizeof(out.hdr10plus));
}

/* the state alone (the app's apply): none */
static void test_state(void)
{
    hdrg_session_output_t out;

    td_run(150, 150, 2, true, &out);
    TEST_ASSERT_FALSE(out.hdr10plus.valid);
    TEST_ASSERT_FALSE(out.st2094_50.valid);
}

/*
 * ST 2094-50: the curve gives back a flat gray's SDR level and each of two
 * levels' (2%), in the output's gamut (the default display's P3); the same
 * whatever the threads
 */
static void test_st2094_50_levels(void)
{
    hdrg_session_output_t out, other;
    char line[160];
    double got, want;

    td_run(150, 150, 4, false, &out);
    TEST_ASSERT_TRUE(out.st2094_50.valid);
    TEST_ASSERT_TRUE(out.st2094_50.baseline_headroom > 0.5f);
    HDRG_TEST_NEAR(1e-3, 203.0, out.st2094_50.reference_white);
    TEST_ASSERT_EQUAL_UINT8(HDRG_GAMUT_P3, out.st2094_50.gamut);
    got = td_restored(&out.st2094_50, 150, &want);
    HDRG_TEST_NEAR(0.02 * want, want, got);

    td_run(30, 235, 4, false, &out);
    TEST_ASSERT_TRUE(out.st2094_50.valid);
    TEST_ASSERT_TRUE(out.st2094_50.count >= 2u);
    got = td_restored(&out.st2094_50, 30, &want);
    snprintf(line, sizeof(line), "dark: %.4f for %.4f; %u points, baseline %.2f stops", got, want,
             out.st2094_50.count, (double)out.st2094_50.baseline_headroom);
    TEST_MESSAGE(line);
    HDRG_TEST_NEAR(0.02 * want, want, got);
    got = td_restored(&out.st2094_50, 235, &want);
    HDRG_TEST_NEAR(0.02 * want, want, got);

    td_run(30, 235, 1, false, &other);
    TEST_ASSERT_EQUAL_MEMORY(&out.st2094_50, &other.st2094_50, sizeof(out.st2094_50));
}

/* a ramp: the points rise in x, the gains within -6 and 0 stops, the output never falls */
static void test_st2094_50_ramp(void)
{
    hdrg_session_output_t out;
    uint8_t i;

    g_ramp = true;
    td_run(0, 0, 4, false, &out);
    g_ramp = false;
    TEST_ASSERT_TRUE(out.st2094_50.valid);
    TEST_ASSERT_TRUE(out.st2094_50.count >= 8u && out.st2094_50.count <= HDRG_ST2094_50_POINTS);

    for (i = 0; i < out.st2094_50.count; i++) {
        TEST_ASSERT_TRUE(out.st2094_50.y[i] <= 0.0f && out.st2094_50.y[i] >= -6.0f);
        TEST_ASSERT_TRUE(out.st2094_50.x[i] > 0.0f && out.st2094_50.x[i] <= 64.0f);

        if (i > 0) {
            TEST_ASSERT_TRUE(out.st2094_50.x[i] > out.st2094_50.x[i - 1u]);
            TEST_ASSERT_TRUE(out.st2094_50.x[i] * hdrg_math_exp2f(out.st2094_50.y[i]) >=
                             out.st2094_50.x[i - 1u] * hdrg_math_exp2f(out.st2094_50.y[i - 1u]) *
                                 0.9999f);
        }
    }
}

/*
 * the mean |difference| of the SDR's 8-bit code (gamma 2.2) the curve gives
 * back from the kept output's max(R, G, B) and the scene's (curve NULL: the
 * generic tone map the stills' HAGC had, knee 0.75)
 */
static double td_error(const hdrg_hagc_curve_t *curve)
{
    uint32_t x, y;
    uint8_t c;
    double sum;
    float m, v, back, rolled;

    sum = 0.0;

    for (y = 0; y < TD_H; y++) {
        for (x = 0; x < TD_W; x++) {
            m = 0.0f;

            for (c = 0; c < 3u; c++) {
                v = hdrg_math_pq_eotf((float)g_kept[((size_t)y * TD_W + x) * 3u + c] / 65535.0f);
                m = (v > m) ? v : m;
            }

            /* (none: the generic tone map, identity to 0.75 and a C1 approach to 1) */
            rolled = (m / 203.0f <= 0.75f)
                         ? m / 203.0f
                         : 0.75f + 0.25f * (1.0f - hdrg_math_expf(-(m / 203.0f - 0.75f) / 0.25f));
            back = (curve != NULL) ? m / 203.0f * hdrg_math_exp2f(hdrg_hagc_eval(curve, m / 203.0f))
                                   : rolled;
            back = hdrg_math_powf((back < 1.0f) ? back : 1.0f, 1.0f / 2.2f) * 255.0f;
            sum += fabs((double)back - (double)td_scene(x, y));
        }
    }

    return sum / (double)(TD_W * TD_H);
}

/*
 * a ramp with a lamp: the fitted curve gives the SDR back closer than the
 * generic tone map the stills' HAGC had (knee 0.75)
 */
static void test_st2094_50_restores(void)
{
    hdrg_session_output_t out;
    hdrg_hagc_t fitted;
    uint8_t payload[HDRG_ST2094_50_T35_MAX];
    size_t size;
    char line[160];
    double ours, theirs;

    g_kept = (uint16_t *)malloc((size_t)TD_W * TD_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(g_kept);
    g_scene = true;
    td_run(0, 0, 4, false, &out);
    g_scene = false;
    TEST_ASSERT_TRUE(out.st2094_50.valid);

    /* the fitted curve as its payload reads (PCHIP slopes) */
    size = hdrg_st2094_50_t35(&out.st2094_50, payload, sizeof(payload));
    TEST_ASSERT_TRUE(size > 0);
    TEST_ASSERT_TRUE(hdrg_hagc_parse(payload, size, &fitted));

    ours = td_error(&fitted.alt[0]);
    theirs = td_error(NULL);
    snprintf(line, sizeof(line), "SDR back: %.2f codes off (the generic tone map's %.2f)", ours,
             theirs);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(ours < theirs);
    TEST_ASSERT_TRUE(ours < 4.0);

    free(g_kept);
    g_kept = NULL;
}

/* n bits of p at bit *at, the most significant first */
static uint32_t td_bits(const uint8_t *p, uint32_t *at, uint8_t n)
{
    uint32_t v;
    uint8_t i;

    v = 0;

    for (i = 0; i < n; i++) {
        v = (v << 1) | ((p[*at >> 3] >> (7u - (*at & 7u))) & 1u);
        (*at)++;
    }

    return v;
}

/*
 * hdrg_hdr10plus_t35: the fields read back in CTA-861-H's order (one window,
 * 9 percentiles, values in 1/100000 of 10000 cd/m^2, held within 0 and 1),
 * 339 bits and zeros after; the size without a buffer or with a short one;
 * not valid: 0
 */
static void test_hdr10plus_t35(void)
{
    static const uint8_t percentages[HDRG_HDR10PLUS_PERCENTILES] = {1,  5,  10, 25, 50,
                                                                    75, 90, 95, 99};
    hdrg_hdr10plus_t plus;
    uint32_t at;
    uint8_t payload[64], c;

    memset(&plus, 0, sizeof(plus));
    plus.maxscl[0] = 1000.0f;
    plus.maxscl[1] = 20000.0f;
    plus.maxscl[2] = -5.0f;
    plus.average_maxrgb = 123.46f;

    for (c = 0; c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        plus.percentiles[c] = 10.0f * (float)(c + 1u);
    }

    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_hdr10plus_t35(&plus, payload, sizeof(payload)));
    plus.valid = true;
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_T35_BYTES,
                             (uint32_t)hdrg_hdr10plus_t35(&plus, NULL, 0));
    memset(payload, 0xA5, sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_T35_BYTES,
                             (uint32_t)hdrg_hdr10plus_t35(&plus, payload, 42));
    TEST_ASSERT_EQUAL_UINT8(0xA5, payload[0]);
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_T35_BYTES,
                             (uint32_t)hdrg_hdr10plus_t35(&plus, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT8(0xA5, payload[HDRG_HDR10PLUS_T35_BYTES]);

    at = 0;
    TEST_ASSERT_EQUAL_UINT32(1, td_bits(payload, &at, 8));
    TEST_ASSERT_EQUAL_UINT32(1, td_bits(payload, &at, 2));
    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 27));
    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 1));
    TEST_ASSERT_EQUAL_UINT32(10000, td_bits(payload, &at, 17));
    TEST_ASSERT_EQUAL_UINT32(100000, td_bits(payload, &at, 17));
    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 17));
    TEST_ASSERT_EQUAL_UINT32(1235, td_bits(payload, &at, 17));
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_PERCENTILES, td_bits(payload, &at, 4));

    for (c = 0; c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        TEST_ASSERT_EQUAL_UINT32(percentages[c], td_bits(payload, &at, 7));
        TEST_ASSERT_EQUAL_UINT32(100u * (c + 1u), td_bits(payload, &at, 17));
    }

    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 10));
    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 3));
    TEST_ASSERT_EQUAL_UINT32(339, at);
    TEST_ASSERT_EQUAL_UINT32(0, td_bits(payload, &at, 5));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)hdrg_hdr10plus_t35(NULL, payload, sizeof(payload)));
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
    RUN_TEST(test_flat);
    RUN_TEST(test_two_levels);
    RUN_TEST(test_state);
    RUN_TEST(test_hdr10plus_t35);
    RUN_TEST(test_st2094_50_levels);
    RUN_TEST(test_st2094_50_ramp);
    RUN_TEST(test_st2094_50_restores);

    return UNITY_END();
}
