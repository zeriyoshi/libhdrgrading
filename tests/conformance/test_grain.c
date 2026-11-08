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
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"
#include "core/grain.h"
#include "hdrgrading/video.h"

/*
 * grain_policy RESYNTH in a video session: the grain its
 * PREFILTER stage removed comes back in PQ as the source showed it in SDR
 * (a flat gray with white noise of sigma 4: the output's grain about 4
 * codes through the PQ slope of the SDR rendition there, white; noise
 * blurred by [1 2 1] / 4: correlated), keyed by the frame (another each
 * frame); the same bytes whatever the threads and from a seek's preroll; an
 * amount of 0 gives REDUCE's bytes; a clean frame gets almost none. A
 * codec's film grain (HDRG_SD_FILM_GRAIN, AV1's parameters) its decoder left
 * out: its levels by Y' as the specification's scaling and template give
 * them (video and full range, Cb and Cr by the luma's function or their
 * own), the grain back in a clean frame's output as the SDR rendition shows
 * it (PRESERVE without a PREFILTER stage, RESYNTH in place of the removed
 * one; REDUCE without it), correlated by its auto-regression, the same
 * bytes whatever the threads and from a preroll; malformed parameters
 * refused at the send.
 */

#define TG_W      192u
#define TG_H      128u
#define TG_FRAMES 10u
#define TG_LEVEL  128u /* the gray's 8-bit code */
#define TG_SIGMA  4.0  /* the noise, 8-bit codes */
#define TG_EDGE   16u  /* the pixels from the edges the measures skip */

static double g_white[TG_H][TG_W], g_across[TG_H][TG_W]; /* tg_frame's */
static const uint8_t *g_film;                            /* tg_run: the frames' film grain */
static uint8_t g_preset = HDRG_PRESET_PHOTO;             /* ... and the preset */

/*
 * AV1's film grain at 8 bits: the luma scaled by scaling all over (no
 * chroma), its auto-regression of lag 1 coeffs (the left and the upper ones)
 */
static void tg_film(uint8_t *data, uint8_t scaling, int8_t coeffs, bool full)
{
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t av1;

    memset(&head, 0, sizeof(head));
    memset(&av1, 0, sizeof(av1));
    head.kind = HDRG_FILM_GRAIN_AV1;
    head.bit_depth = 8;
    head.full_range = full ? 1u : 0u;
    head.subsampling_x = 1;
    head.subsampling_y = 1;
    av1.num_y_points = 1;
    av1.y_points[0][0] = 128;
    av1.y_points[0][1] = scaling;
    av1.scaling_shift = 8;
    av1.ar_coeff_lag = (coeffs != 0) ? 1u : 0u;
    av1.ar_coeffs_y[1] = coeffs;
    av1.ar_coeffs_y[3] = coeffs;
    av1.ar_coeff_shift = 7;

    memcpy(data, &head, sizeof(head));
    memcpy(data + sizeof(head), &av1, sizeof(av1));
}

/* frame k: the gray with noise of sigma (blurred: [1 2 1] / 4 each way, its sigma kept) */
static void tg_frame(uint8_t *rgb, uint32_t k, double sigma, bool blurred)
{
    uint32_t x, y, h, i;
    uint8_t c;
    double n, scale;

    for (y = 0; y < TG_H; y++) {
        for (x = 0; x < TG_W; x++) {
            h = hdrg_hash_pixel(hdrg_hash_row(UINT64_C(0x6772616E) + k, y), x);
            g_white[y][x] = ((double)(h & 0xffu) + (double)((h >> 8) & 0xffu) +
                             (double)((h >> 16) & 0xffu) + (double)(h >> 24) - 510.0) /
                            147.80054127;
        }
    }

    for (y = 0; y < TG_H; y++) {
        for (x = 0; x < TG_W; x++) {
            g_across[y][x] = blurred ? 0.25 * g_white[y][(x + TG_W - 1u) % TG_W] +
                                           0.5 * g_white[y][x] + 0.25 * g_white[y][(x + 1u) % TG_W]
                                     : g_white[y][x];
        }
    }

    scale = blurred ? 1.0 / 0.375 : 1.0; /* ([1 2 1] / 4)^2 sums to 0.375 each way */

    for (y = 0; y < TG_H; y++) {
        for (x = 0; x < TG_W; x++) {
            n = g_across[y][x];

            if (blurred) {
                n = 0.25 * g_across[(y + TG_H - 1u) % TG_H][x] + 0.5 * g_across[y][x] +
                    0.25 * g_across[(y + 1u) % TG_H][x];
            }

            for (c = 0; c < 3u; c++) {
                i = (y * TG_W + x) * 3u + c;
                rgb[i] =
                    (uint8_t)fmin(fmax((double)TG_LEVEL + sigma * n * scale + 0.5, 0.0), 255.0);
            }
        }
    }
}

/*
 * the frames [start, TG_FRAMES) through a session (PHOTO, 16-bit PQ out) of
 * grain_policy policy at amount, preroll from start: the outputs into out
 */
static void tg_run(double sigma, bool blurred, uint8_t policy, float amount, uint8_t threads,
                   uint32_t start, uint32_t preroll, uint16_t *out, uint32_t *count)
{
    const size_t samples = (size_t)TG_W * TG_H * 3u;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    hdrg_session_output_t output;
    hdrg_status_t status;
    uint32_t sent;
    uint8_t *pixels;

    pixels = (uint8_t *)malloc(samples * TG_FRAMES);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));

    memset(&format, 0, sizeof(format));
    format.width = TG_W;
    format.height = TG_H;
    format.bit_depth = 8;
    format.stride = (size_t)TG_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, g_preset);
    params.grain_policy = policy;
    params.grain_amount = amount;
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.latency = HDRG_LATENCY_PLAYBACK;
    config.rate_num = 30;
    config.rate_den = 1;
    config.start = start;
    config.preroll = preroll;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));

    sent = start;
    *count = 0;

    for (;;) {
        memset(&output, 0, sizeof(output));
        output.planes[0] = (uint8_t *)(out + (size_t)*count * samples);
        output.strides[0] = (size_t)TG_W * 6u;

        status = hdrg_session_receive_frame(session, &output);
        if (status == HDRG_ERR_AGAIN) {
            if (sent < TG_FRAMES) {
                tg_frame(pixels + (size_t)sent * samples, sent, sigma, blurred);
            }

            memset(&frame, 0, sizeof(frame));
            frame.planes[0] = pixels + (size_t)((sent < TG_FRAMES) ? sent : 0u) * samples;
            frame.pts = (int64_t)sent;
            frame.film_grain = g_film;
            frame.film_grain_size = (g_film != NULL) ? HDRG_GRAIN_FILM_BYTES : 0u;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TG_FRAMES) ? &frame : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(*count < TG_FRAMES);
        (*count)++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(pixels);
}

static uint16_t *tg_frames(void)
{
    uint16_t *out;

    out = (uint16_t *)malloc((size_t)TG_W * TG_H * 3u * TG_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(out);

    return out;
}

/*
 * output frame k's grain: the standard deviation of its PQ Y' (16-bit
 * codes, BT.2020's coefficients) inside the edges, and its lag-1
 * correlation from its first and second differences across
 */
static void tg_measure(const uint16_t *out, uint32_t k, double *sigma, double *rho)
{
    const uint16_t *p = out + (size_t)k * TG_W * TG_H * 3u;
    uint32_t x, y, n;
    double v, sum, squares, d1, d2, a, b, mean, ratio, lo, hi, mid;

    sum = 0.0;
    squares = 0.0;
    d1 = 0.0;
    d2 = 0.0;
    n = 0;

    for (y = TG_EDGE; y < TG_H - TG_EDGE; y++) {
        for (x = TG_EDGE; x < TG_W - TG_EDGE; x++) {
            v = 0.2627 * p[(y * TG_W + x) * 3u] + 0.678 * p[(y * TG_W + x) * 3u + 1u] +
                0.0593 * p[(y * TG_W + x) * 3u + 2u];
            a = 0.2627 * p[(y * TG_W + x + 1u) * 3u] + 0.678 * p[(y * TG_W + x + 1u) * 3u + 1u] +
                0.0593 * p[(y * TG_W + x + 1u) * 3u + 2u];
            b = 0.2627 * p[(y * TG_W + x + 2u) * 3u] + 0.678 * p[(y * TG_W + x + 2u) * 3u + 1u] +
                0.0593 * p[(y * TG_W + x + 2u) * 3u + 2u];
            sum += v;
            squares += v * v;
            d1 += (a - v) * (a - v);
            d2 += (b - v) * (b - v);
            n++;
        }
    }

    mean = sum / n;
    *sigma = sqrt(squares / n - mean * mean);
    ratio = d2 / d1;
    lo = 0.0;
    hi = 0.999;

    while (ratio > 1.0 && hi - lo > 1e-6) {
        mid = 0.5 * (lo + hi);

        if ((1.0 + mid) * (1.0 + mid * mid) < ratio) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    *rho = (ratio > 1.0) ? 0.5 * (lo + hi) : 0.0;
}

/* the SDR rendition's grain of sigma (8-bit codes) at the gray in 16-bit PQ codes (white 203) */
static double tg_expected(double sigma)
{
    const double y = TG_LEVEL / 255.0, d = 1.0 / 255.0;

    return sigma / 255.0 *
           ((double)hdrg_math_pq_oetf(hdrg_math_srgb_eotf((float)(y + d)) * 203.0f) -
            (double)hdrg_math_pq_oetf(hdrg_math_srgb_eotf((float)(y - d)) * 203.0f)) /
           (2.0 * d) * 65535.0;
}

/*
 * the grain's level: about the SDR rendition's (not the expansion's), white
 * for white noise and correlated for blurred noise; another each frame
 */
static void test_level(void)
{
    const size_t frame = (size_t)TG_W * TG_H * 3u;
    uint32_t count, k;
    uint16_t *out;
    uint8_t blurred;
    char line[128];
    double sigma, rho, total, rho_total, expected, raw;

    out = tg_frames();
    expected = tg_expected(TG_SIGMA);

    /* the source's noise through the expansion, without the stage (for the record) */
    tg_run(TG_SIGMA, false, HDRG_GRAIN_PRESERVE, 1.0f, 4, 0, 0, out, &count);
    tg_measure(out, TG_FRAMES / 2u, &raw, &rho);

    for (blurred = 0; blurred < 2u; blurred++) {
        tg_run(TG_SIGMA, blurred != 0u, HDRG_GRAIN_RESYNTH, 1.0f, 4, 0, 0, out, &count);
        TEST_ASSERT_EQUAL_UINT32(TG_FRAMES, count);
        total = 0.0;
        rho_total = 0.0;

        for (k = 2; k < TG_FRAMES - 2u; k++) {
            tg_measure(out, k, &sigma, &rho);
            total += sigma;
            rho_total += rho;
        }

        sigma = total / (TG_FRAMES - 4u);
        rho = rho_total / (TG_FRAMES - 4u);
        snprintf(line, sizeof(line),
                 "%s: grain %.1f codes (the SDR rendition's %.1f, PRESERVE %.1f), rho %.2f",
                 blurred ? "blurred" : "white", sigma, expected, raw, rho);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(sigma > 0.75 * expected && sigma < 1.35 * expected);
        TEST_ASSERT_TRUE(blurred ? rho > 0.45 : rho < 0.35);
    }

    /* another grain each frame */
    TEST_ASSERT_TRUE(memcmp(out + 4u * frame, out + 5u * frame, frame * sizeof(uint16_t)) != 0);

    free(out);
}

/* the same bytes whatever the threads and from a seek's preroll; an amount of 0: REDUCE's */
static void test_determinism(void)
{
    const size_t frame = (size_t)TG_W * TG_H * 3u;
    uint32_t count, n;
    uint16_t *a, *b;

    a = tg_frames();
    b = tg_frames();
    tg_run(TG_SIGMA, false, HDRG_GRAIN_RESYNTH, 1.0f, 1, 0, 0, a, &count);
    tg_run(TG_SIGMA, false, HDRG_GRAIN_RESYNTH, 1.0f, 8, 0, 0, b, &count);
    TEST_ASSERT_EQUAL_MEMORY(a, b, frame * TG_FRAMES * sizeof(uint16_t));

    /* from frame 6 with frames 2..5 as the preroll (the stage's past of 2 and more) */
    n = 6;
    tg_run(TG_SIGMA, false, HDRG_GRAIN_RESYNTH, 1.0f, 4, n - 4u, 4, b, &count);
    TEST_ASSERT_EQUAL_UINT32(TG_FRAMES - n, count);
    TEST_ASSERT_EQUAL_MEMORY(a + n * frame, b, (TG_FRAMES - n) * frame * sizeof(uint16_t));

    tg_run(TG_SIGMA, false, HDRG_GRAIN_RESYNTH, 0.0f, 4, 0, 0, a, &count);
    tg_run(TG_SIGMA, false, HDRG_GRAIN_REDUCE, 1.0f, 4, 0, 0, b, &count);
    TEST_ASSERT_EQUAL_MEMORY(a, b, frame * TG_FRAMES * sizeof(uint16_t));

    free(a);
    free(b);
}

/* a clean gray: the stage removes almost nothing, the grain stays within a few codes */
static void test_clean(void)
{
    const size_t frame = (size_t)TG_W * TG_H * 3u;
    uint32_t count, worst;
    uint16_t *a, *b;
    size_t i;

    a = tg_frames();
    b = tg_frames();
    tg_run(0.0, false, HDRG_GRAIN_RESYNTH, 1.0f, 4, 0, 0, a, &count);
    tg_run(0.0, false, HDRG_GRAIN_REDUCE, 1.0f, 4, 0, 0, b, &count);
    worst = 0;

    for (i = 0; i < frame * TG_FRAMES; i++) {
        worst = HDRG_TEST_MAX(worst, (uint32_t)abs((int32_t)a[i] - (int32_t)b[i]));
    }

    TEST_ASSERT_TRUE(worst <= 8u);

    free(a);
    free(b);
}

/*
 * the parameters' levels: sigma 4 codes from a scaling of 32 over the
 * template's 32 (the Gaussian sequence's 511.5 / 16) at 8 bits, white
 * without the auto-regression and correlated with it; video range over 219
 * codes; Cb and Cr by the luma's function, by their own at the neutral
 * chroma, none without
 */
static void test_film_levels(void)
{
    hdrg_sd_film_grain_av1_t av1;
    hdrg_grain_stats_t stats, ar;
    uint8_t data[HDRG_GRAIN_FILM_BYTES];

    tg_film(data, 32, 0, true);
    TEST_ASSERT_TRUE(hdrg_grain_film_check(data, sizeof(data)));
    hdrg_grain_film(data, &stats);
    TEST_ASSERT_TRUE(stats.valid);
    HDRG_TEST_NEAR(0.02 * 4.0 / 255.0, 4.0 / 255.0, stats.sigma[0][128]);
    HDRG_TEST_NEAR(1e-9, stats.sigma[0][128], stats.sigma[0][3]);
    TEST_ASSERT_TRUE(stats.rho < 0.05f);
    TEST_ASSERT_TRUE(stats.sigma[1][128] == 0.0f && stats.sigma[2][128] == 0.0f);

    tg_film(data, 32, 0, false);
    hdrg_grain_film(data, &stats);
    HDRG_TEST_NEAR(0.02 * 4.0 / 219.0, 4.0 / 219.0, stats.sigma[0][128]);

    tg_film(data, 32, 48, true);
    hdrg_grain_film(data, &ar);
    TEST_ASSERT_TRUE(ar.rho > 0.3f);
    TEST_ASSERT_TRUE(ar.sigma[0][128] > 1.1f * 4.0f / 255.0f);

    /* Cb by the luma's function, Cr by its own at 100: (128 x 64) >> 6 - 28 */
    tg_film(data, 32, 0, true);
    memcpy(&av1, data + sizeof(hdrg_sd_film_grain_t), sizeof(av1));
    av1.chroma_scaling_from_luma = 1;
    memcpy(data + sizeof(hdrg_sd_film_grain_t), &av1, sizeof(av1));
    hdrg_grain_film(data, &stats);
    HDRG_TEST_NEAR(0.03 * 4.0 / 255.0, 4.0 / 255.0, stats.sigma[1][200]);
    HDRG_TEST_NEAR(0.03 * 4.0 / 255.0, 4.0 / 255.0, stats.sigma[2][20]);

    av1.chroma_scaling_from_luma = 0;
    av1.num_uv_points[1] = 2;
    av1.uv_points[1][0][0] = 99;
    av1.uv_points[1][0][1] = 0;
    av1.uv_points[1][1][0] = 100;
    av1.uv_points[1][1][1] = 64;
    av1.uv_mult[1] = 64;
    av1.uv_offset[1] = -28;
    memcpy(data + sizeof(hdrg_sd_film_grain_t), &av1, sizeof(av1));
    hdrg_grain_film(data, &stats);
    TEST_ASSERT_TRUE(stats.sigma[1][128] == 0.0f);
    HDRG_TEST_NEAR(0.03 * 8.0 / 255.0, 8.0 / 255.0, stats.sigma[2][40]);
    HDRG_TEST_NEAR(0.03 * 8.0 / 255.0, 8.0 / 255.0, stats.sigma[2][230]);
}

/* a clean gray's output frames' mean grain and correlation */
static void tg_level_of(const uint16_t *out, double *sigma, double *rho)
{
    uint32_t k;
    double s, r, total, rho_total;

    total = 0.0;
    rho_total = 0.0;

    for (k = 2; k < TG_FRAMES - 2u; k++) {
        tg_measure(out, k, &s, &r);
        total += s;
        rho_total += r;
    }

    *sigma = total / (TG_FRAMES - 4u);
    *rho = rho_total / (TG_FRAMES - 4u);
}

/*
 * a clean gray given the grain of sigma 4 as AV1's parameters: the SDR
 * rendition's grain back (PRESERVE without a PREFILTER stage: GAME; RESYNTH
 * in place of the removed one), correlated by its auto-regression; REDUCE
 * leaves it out
 */
static void test_film_session(void)
{
    const size_t frame = (size_t)TG_W * TG_H * 3u;
    uint32_t count;
    uint16_t *out, *plain;
    uint8_t data[HDRG_GRAIN_FILM_BYTES], cases;
    char line[128];
    double sigma, rho, expected;

    out = tg_frames();
    plain = tg_frames();
    expected = tg_expected(TG_SIGMA);
    tg_film(data, 32, 0, true);

    for (cases = 0; cases < 2u; cases++) {
        g_preset = (cases == 0u) ? HDRG_PRESET_GAME : HDRG_PRESET_PHOTO;
        g_film = data;
        tg_run(0.0, false, (cases == 0u) ? HDRG_GRAIN_PRESERVE : HDRG_GRAIN_RESYNTH, 1.0f, 4, 0, 0,
               out, &count);
        g_film = NULL;
        TEST_ASSERT_EQUAL_UINT32(TG_FRAMES, count);
        tg_level_of(out, &sigma, &rho);
        snprintf(line, sizeof(line), "%s: grain %.1f codes (the SDR rendition's %.1f), rho %.2f",
                 (cases == 0u) ? "GAME PRESERVE" : "PHOTO RESYNTH", sigma, expected, rho);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(sigma > 0.85 * expected && sigma < 1.15 * expected);
        TEST_ASSERT_TRUE(rho < 0.35);
    }

    tg_film(data, 32, 48, true);
    g_film = data;
    tg_run(0.0, false, HDRG_GRAIN_PRESERVE, 1.0f, 4, 0, 0, out, &count);
    g_film = NULL;
    tg_level_of(out, &sigma, &rho);
    snprintf(line, sizeof(line), "auto-regressed: grain %.1f codes, rho %.2f", sigma, rho);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(rho > 0.3);

    /* REDUCE: none of it */
    tg_run(0.0, false, HDRG_GRAIN_REDUCE, 1.0f, 4, 0, 0, plain, &count);
    g_film = data;
    tg_run(0.0, false, HDRG_GRAIN_REDUCE, 1.0f, 4, 0, 0, out, &count);
    g_film = NULL;
    g_preset = HDRG_PRESET_PHOTO;
    TEST_ASSERT_EQUAL_MEMORY(plain, out, frame * TG_FRAMES * sizeof(uint16_t));

    free(out);
    free(plain);
}

/* the film grain's frames: the same bytes whatever the threads and from a seek's preroll */
static void test_film_determinism(void)
{
    const size_t frame = (size_t)TG_W * TG_H * 3u;
    uint32_t count, n;
    uint16_t *a, *b;
    uint8_t data[HDRG_GRAIN_FILM_BYTES];

    a = tg_frames();
    b = tg_frames();
    tg_film(data, 32, 48, false);
    g_film = data;
    tg_run(0.0, false, HDRG_GRAIN_RESYNTH, 1.0f, 1, 0, 0, a, &count);
    tg_run(0.0, false, HDRG_GRAIN_RESYNTH, 1.0f, 8, 0, 0, b, &count);
    TEST_ASSERT_EQUAL_MEMORY(a, b, frame * TG_FRAMES * sizeof(uint16_t));

    n = 6;
    tg_run(0.0, false, HDRG_GRAIN_RESYNTH, 1.0f, 4, n - 4u, 4, b, &count);
    g_film = NULL;
    TEST_ASSERT_EQUAL_UINT32(TG_FRAMES - n, count);
    TEST_ASSERT_EQUAL_MEMORY(a + n * frame, b, (TG_FRAMES - n) * frame * sizeof(uint16_t));

    free(a);
    free(b);
}

/* malformed parameters: refused at the send */
static void test_film_refused(void)
{
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t av1;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    uint8_t data[HDRG_GRAIN_FILM_BYTES], bad[HDRG_GRAIN_FILM_BYTES], pixels[TG_W * TG_H * 3u], i;

    tg_film(data, 32, 0, true);
    memset(pixels, TG_LEVEL, sizeof(pixels));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 2));

    memset(&format, 0, sizeof(format));
    format.width = TG_W;
    format.height = TG_H;
    format.bit_depth = 8;
    format.stride = (size_t)TG_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_GAME);
    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));

    memset(&frame, 0, sizeof(frame));
    frame.planes[0] = pixels;
    frame.film_grain = data;
    frame.film_grain_size = sizeof(data) - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &frame));
    frame.film_grain = NULL;
    frame.film_grain_size = sizeof(data);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &frame));

    /*
     * the kind, the depth, the lag, the shifts, the points' order, the offsets; the counts past the
     * arrays, the subsampling past the templates, a depth and a shift past the shifts' range
     */
    for (i = 0; i < 14u; i++) {
        memcpy(&head, data, sizeof(head));
        memcpy(&av1, data + sizeof(head), sizeof(av1));
        head.kind = (i == 0u) ? 2u : head.kind;
        head.bit_depth = (i == 1u) ? 7u : head.bit_depth;
        av1.ar_coeff_lag = (i == 2u) ? 4u : av1.ar_coeff_lag;
        av1.scaling_shift = (i == 3u) ? 7u : av1.scaling_shift;
        av1.ar_coeff_shift = (i == 4u) ? 10u : av1.ar_coeff_shift;
        av1.num_y_points = (i == 5u) ? 2u : av1.num_y_points;
        av1.y_points[1][0] = (i == 5u) ? 128u : av1.y_points[1][0];
        av1.uv_offset[0] = (i == 6u) ? 256 : av1.uv_offset[0];
        av1.num_y_points = (i == 7u) ? 15u : av1.num_y_points;
        av1.num_uv_points[0] = (i == 8u) ? 11u : av1.num_uv_points[0];
        av1.num_uv_points[1] = (i == 9u) ? 11u : av1.num_uv_points[1];
        head.subsampling_x = (i == 10u) ? 2u : head.subsampling_x;
        head.subsampling_y = (i == 11u) ? head.subsampling_x + 1u : head.subsampling_y;
        head.bit_depth = (i == 12u) ? 13u : head.bit_depth;
        av1.scaling_shift = (i == 13u) ? 12u : av1.scaling_shift;
        memcpy(bad, &head, sizeof(head));
        memcpy(bad + sizeof(head), &av1, sizeof(av1));
        TEST_ASSERT_FALSE(hdrg_grain_film_check(bad, sizeof(bad)));
        frame.film_grain = bad;
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &frame));
    }

    frame.film_grain = data;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &frame));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

void setUp(void)
{
}

void tearDown(void)
{
    /* (a test failed on the way leaves none of its own) */
    g_film = NULL;
    g_preset = HDRG_PRESET_PHOTO;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_level);
    RUN_TEST(test_determinism);
    RUN_TEST(test_clean);
    RUN_TEST(test_film_levels);
    RUN_TEST(test_film_session);
    RUN_TEST(test_film_determinism);
    RUN_TEST(test_film_refused);

    return UNITY_END();
}
