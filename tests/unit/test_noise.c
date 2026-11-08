/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <unity.h>

#include "../test_util.h"
#include "core/noise.h"
#include "pipeline.h"
#include "hdrgrading/foundation/math.h"
#include "hdrgrading/foundation/pool.h"

/*
 * Noise estimation: white Gaussian noise of known sigma reads
 * its sigma on flat frames (luma and per-channel noise, which splits into
 * Y' / Cb / Cr by the coefficients), the noise level function follows a
 * level-dependent sigma, edges, texture and clipped tiles do not inflate it,
 * clean frames read (almost) nothing, 8-pixel blocks show as blockiness, the
 * worker count does not change the result, and frames too small have none.
 * Its consumer: the coring of the apply pass lowers the noise
 * the gain adds next to an edge, keeps the edge, and stays independent of
 * the worker count (also on the SCALE stage's bands); a clean frame is not
 * cored. The estimate from the grid's windows gathered into a mosaic (the
 * GPU's) equals the frame's.
 */
#define TN_W        1024u
#define TN_H        512u
#define TN_TOL      0.05f /* relative, flat frames */
#define TN_TOL_EDGE 0.12f /* relative, with edges / texture / clipping */
#define TN_CODE     (1.0f / 255.0f)
#define TN_EDGE_W   256u
#define TN_EDGE_H   128u
#define TN_EDGE_X   128u /* gray 110 left of it, 200 from it */
#define TN_SCALED_W 512u /* an upscaled output of two bands */
#define TN_SCALED_H 300u
#define TN_MOSAIC_W 1600u /* more tiles than an estimate takes: every second one */
#define TN_MOSAIC_H 800u

static hdrg_allocator_t g_alloc;
static uint64_t g_rng;
static double g_sigma, g_ring, g_block[64][128];

static double rng_uniform(void)
{
    uint64_t z;

    g_rng += UINT64_C(0x9E3779B97F4A7C15);
    z = g_rng;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    z ^= z >> 31;

    return ((double)(z >> 11) + 0.5) / 9007199254740992.0;
}

static double rng_gauss(void)
{
    return sqrt(-2.0 * log(rng_uniform())) * cos(6.283185307179586 * rng_uniform());
}

static uint16_t to16(double v)
{
    return (uint16_t)HDRG_TEST_MAX(HDRG_MIN(lround(v * 65535.0), 65535L), 0L);
}

/* a 16-bit frame: level(x, y) plus noise of sigma(x, y) (8-bit codes), shared or per channel */
static uint16_t *frame16(uint32_t w, uint32_t h, double (*level)(uint32_t, uint32_t),
                         double (*sigma)(uint32_t, uint32_t), bool per_channel)
{
    uint32_t x, y;
    uint16_t *pixels;
    uint8_t c;
    double n, s;

    pixels = (uint16_t *)malloc((size_t)w * h * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(pixels);
    g_rng = 1;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            s = sigma(x, y) / 255.0;
            n = rng_gauss() * s;

            for (c = 0; c < 3u; c++) {
                if (per_channel && c > 0) {
                    n = rng_gauss() * s;
                }

                pixels[((size_t)y * w + x) * 3u + c] = to16(level(x, y) + n);
            }
        }
    }

    return pixels;
}

static void estimate(const void *pixels, uint32_t w, uint32_t h, uint32_t bits,
                     const hdrg_exec_t *exec, hdrg_noise_t *out)
{
    hdrg_color_t color;
    hdrg_raw_image_t raw;
    hdrg_source_t src;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)pixels;
    raw.width = w;
    raw.height = h;
    raw.bit_depth = bits;
    raw.stride = (size_t)w * 3u * (bits / 8u);

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_noise_estimate(&g_alloc, exec, &src, out));

    hdrg_source_free(&g_alloc, &src);
    hdrg_color_free(&g_alloc, &color);
}

static void assert_rel(float tol, float expected, float actual)
{
    char msg[96];

    snprintf(msg, sizeof(msg), "expected %.4f codes, got %.4f", (double)(expected / TN_CODE),
             (double)(actual / TN_CODE));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(actual - expected) <= tol * expected, msg);
}

static double level_mid(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;

    return 0.5;
}

static double sigma_const(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;

    return g_sigma;
}

/* eight 128-pixel strips at the bin centers, sigma 1 + bin / 2 (clear of the clipping) */
static double level_strips(uint32_t x, uint32_t y)
{
    (void)y;

    return ((double)(x / 128u) + 0.5) / 8.0;
}

static double sigma_strips(uint32_t x, uint32_t y)
{
    (void)y;

    return 1.0 + (double)(x / 128u) / 2.0;
}

/* 40-pixel squares of two levels with a sine texture in every third one */
static double level_checker(uint32_t x, uint32_t y)
{
    const uint32_t sx = x / 40u, sy = y / 40u;
    double v;

    v = ((sx + sy) % 2u == 0) ? 0.3 : 0.6;

    if ((sx + 2u * sy) % 3u == 0) {
        v += 0.08 * sin((double)x * 0.9) * cos((double)y * 0.7);
    }

    return v;
}

static void test_flat_luma(void)
{
    const float sigmas[3] = {1.0f, 4.0f, 16.0f};
    hdrg_noise_t noise;
    uint16_t *pixels;
    uint8_t i;

    for (i = 0; i < 3u; i++) {
        g_sigma = (double)sigmas[i];
        pixels = frame16(TN_W, TN_H, level_mid, sigma_const, false);
        estimate(pixels, TN_W, TN_H, 16, NULL, &noise);
        printf("flat sigma %.1f: %.4f (cb %.4f cr %.4f block %.3f tiles %u)\n", g_sigma,
               (double)(noise.sigma / TN_CODE), (double)(noise.sigma_cb / TN_CODE),
               (double)(noise.sigma_cr / TN_CODE), (double)noise.blockiness, noise.tiles);

        assert_rel(TN_TOL, sigmas[i] * TN_CODE, noise.sigma);
        assert_rel(TN_TOL, sigmas[i] * TN_CODE, noise.sigma_y[3]);
        assert_rel(TN_TOL, sigmas[i] * TN_CODE, noise.sigma_y[4]);
        TEST_ASSERT_TRUE(noise.sigma_cb < 0.01f * TN_CODE && noise.sigma_cr < 0.01f * TN_CODE);
        TEST_ASSERT_TRUE(noise.blockiness > 0.95f && noise.blockiness < 1.05f);
        TEST_ASSERT_TRUE(noise.sigma_y[0] < 0.0f && noise.sigma_y[7] < 0.0f);
        free(pixels);
    }
}

static void test_flat_channels(void)
{
    const double kr = 0.2126, kg = 0.7152, kb = 0.0722;
    hdrg_noise_t noise;
    uint16_t *pixels;

    g_sigma = 4.0;
    pixels = frame16(TN_W, TN_H, level_mid, sigma_const, true);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);

    assert_rel(TN_TOL, (float)(4.0 * sqrt(kr * kr + kg * kg + kb * kb)) * TN_CODE, noise.sigma);
    assert_rel(
        TN_TOL,
        (float)(4.0 * sqrt(kr * kr + kg * kg + (1.0 - kb) * (1.0 - kb)) / (2.0 * (1.0 - kb))) *
            TN_CODE,
        noise.sigma_cb);
    assert_rel(
        TN_TOL,
        (float)(4.0 * sqrt((1.0 - kr) * (1.0 - kr) + kg * kg + kb * kb) / (2.0 * (1.0 - kr))) *
            TN_CODE,
        noise.sigma_cr);

    free(pixels);
}

static void test_level_function(void)
{
    hdrg_noise_t noise;
    uint16_t *pixels;
    uint8_t b;

    pixels = frame16(TN_W, TN_H, level_strips, sigma_strips, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);

    for (b = 0; b < HDRG_NOISE_BINS; b++) {
        assert_rel(TN_TOL_EDGE, (1.0f + (float)b / 2.0f) * TN_CODE, noise.sigma_y[b]);
        assert_rel(0.001f, noise.sigma_y[b], hdrg_noise_at(&noise, ((float)b + 0.5f) / 8.0f));
    }

    /* between the centers of bins 2 and 3; held past the outer ones */
    assert_rel(0.001f, 0.5f * (noise.sigma_y[2] + noise.sigma_y[3]),
               hdrg_noise_at(&noise, 3.0f / 8.0f));
    assert_rel(0.001f, noise.sigma_y[0], hdrg_noise_at(&noise, 0.0f));
    assert_rel(0.001f, noise.sigma_y[7], hdrg_noise_at(&noise, 1.0f));

    free(pixels);
}

static void test_edges_texture(void)
{
    hdrg_noise_t noise;
    uint16_t *pixels;

    g_sigma = 3.0;
    pixels = frame16(TN_W, TN_H, level_checker, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);
    printf("checker sigma 3: %.4f\n", (double)(noise.sigma / TN_CODE));

    assert_rel(TN_TOL_EDGE, 3.0f * TN_CODE, noise.sigma);

    free(pixels);
}

/* the synthetic picture (gradient, a highlight, a white patch at 250) with 8-bit noise */
static void test_clipped_8bit(void)
{
    hdrg_noise_t noise;
    uint8_t *pixels;
    size_t i;

    pixels = hdrg_test_image(TN_W, TN_H);
    TEST_ASSERT_NOT_NULL(pixels);
    g_rng = 7;

    for (i = 0; i < (size_t)TN_W * TN_H; i++) {
        pixels[i * 3u + 0] =
            (uint8_t)HDRG_CLAMP(lround((double)pixels[i * 3u + 0] + 3.0 * rng_gauss()), 0L, 255L);
        pixels[i * 3u + 1] =
            (uint8_t)HDRG_CLAMP(lround((double)pixels[i * 3u + 1] + 3.0 * rng_gauss()), 0L, 255L);
        pixels[i * 3u + 2] =
            (uint8_t)HDRG_CLAMP(lround((double)pixels[i * 3u + 2] + 3.0 * rng_gauss()), 0L, 255L);
    }

    estimate(pixels, TN_W, TN_H, 8, NULL, &noise);
    printf("8-bit picture sigma 3/ch: %.4f (cb %.4f cr %.4f)\n", (double)(noise.sigma / TN_CODE),
           (double)(noise.sigma_cb / TN_CODE), (double)(noise.sigma_cr / TN_CODE));

    /* per-channel noise of 3 codes plus the rounding (1/12 code^2) */
    assert_rel(TN_TOL_EDGE, (float)(sqrt(9.0 + 1.0 / 12.0) * 0.7554) * TN_CODE, noise.sigma);

    free(pixels);
}

static void test_clean(void)
{
    hdrg_noise_t noise;
    uint8_t *pixels;

    pixels = hdrg_test_image(TN_W, TN_H);
    TEST_ASSERT_NOT_NULL(pixels);
    estimate(pixels, TN_W, TN_H, 8, NULL, &noise);
    printf("clean picture: %.4f\n", (double)(noise.sigma / TN_CODE));

    TEST_ASSERT_TRUE(noise.sigma >= 0.0f && noise.sigma < 0.5f * TN_CODE);

    memset(pixels, 128, (size_t)TN_W * TN_H * 3u);
    estimate(pixels, TN_W, TN_H, 8, NULL, &noise);

    TEST_ASSERT_TRUE(noise.sigma == 0.0f && noise.sigma_cb == 0.0f && noise.blockiness == 1.0f &&
                     noise.block_step == 0.0f);

    free(pixels);
}

static double level_blocks(uint32_t x, uint32_t y)
{
    return g_block[y / 8u][x / 8u];
}

/*
 * Flat 0.5 with a dark line at x = 96, 97 (the left of a block column) and,
 * in its blocks, ringing: a swing of +-g_ring changing sign every pixel at
 * x = 100..103, two pixels clear of the line
 */
static double level_ringing(uint32_t x, uint32_t y)
{
    if (x == 96u || x == 97u) {
        return 0.05;
    }

    if (x >= 100u && x <= 103u) {
        return 0.5 + (((x + y) % 2u == 0) ? g_ring : -g_ring);
    }

    return 0.5;
}

/* 64-pixel squares of two levels: steps of 77 codes on the grid, as of UI laid out on it */
static double level_grid_edges(uint32_t x, uint32_t y)
{
    return (((x / 64u) + (y / 64u)) % 2u == 0) ? 0.3 : 0.6;
}

static void test_blockiness(void)
{
    hdrg_noise_t noise;
    uint32_t i, j;
    uint16_t *pixels;

    g_rng = 3;

    for (i = 0; i < 64u; i++) {
        for (j = 0; j < 128u; j++) {
            g_block[i][j] = 0.4 + 0.02 * rng_uniform();
        }
    }

    g_sigma = 1.0;
    pixels = frame16(TN_W, TN_H, level_blocks, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);
    printf("blocks: blockiness %.3f, step %.3f codes\n", (double)noise.blockiness,
           (double)(noise.block_step / TN_CODE));

    /*
     * block levels 0.02 apart at most: |difference| of two uniform ones is
     * 1/3 of it on average, less what the noise of 1 code hides of it
     */
    TEST_ASSERT_TRUE(noise.blockiness > 1.5f);
    TEST_ASSERT_TRUE(noise.block_step > 0.3f * 0.02f / 3.0f && noise.block_step < 0.02f / 3.0f);

    free(pixels);

    /* edges on the grid are no blocks */
    pixels = frame16(TN_W, TN_H, level_grid_edges, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);

    TEST_ASSERT_TRUE(noise.blockiness < 1.05f && noise.block_step < 0.05f * TN_CODE);

    free(pixels);
}

/*
 * Ringing: swings away from an edge inside its 8 x 8 blocks read as a
 * level of about the swing over flat blocks; a clean edge and noise spread
 * over the whole frame read none
 */
static void test_ringing(void)
{
    hdrg_noise_t noise;
    uint16_t *pixels;

    g_sigma = 0.0;
    g_ring = 3.0 / 255.0;
    pixels = frame16(TN_W, TN_H, level_ringing, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);
    printf("ringing: %.2f, level %.3f codes\n", (double)noise.ringing,
           (double)(noise.ring_level / TN_CODE));

    TEST_ASSERT_TRUE(noise.ringing > 4.0f);
    TEST_ASSERT_TRUE(noise.ring_level > 1.0f * TN_CODE && noise.ring_level < 4.0f * TN_CODE);
    free(pixels);

    g_ring = 0.0;
    pixels = frame16(TN_W, TN_H, level_ringing, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);

    TEST_ASSERT_TRUE(noise.ringing == 1.0f && noise.ring_level == 0.0f);
    free(pixels);

    g_sigma = 2.0;
    pixels = frame16(TN_W, TN_H, level_ringing, sigma_const, false);
    estimate(pixels, TN_W, TN_H, 16, NULL, &noise);

    TEST_ASSERT_TRUE(noise.ringing < 1.5f);
    free(pixels);
}

static void test_workers(void)
{
    hdrg_exec_t exec;
    hdrg_noise_t one, many;
    uint16_t *pixels;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;
    pixels = frame16(TN_W, TN_H, level_checker, sigma_strips, true);

    estimate(pixels, TN_W, TN_H, 16, NULL, &one);
    estimate(pixels, TN_W, TN_H, 16, &exec, &many);

    TEST_ASSERT_EQUAL_MEMORY(&one, &many, sizeof(one));

    free(pixels);
    hdrg_pool_destroy(exec.pool);
}

static void test_small(void)
{
    hdrg_noise_t noise;
    uint16_t *pixels;

    g_sigma = 4.0;
    pixels = frame16(32, 300, level_mid, sigma_const, false);
    estimate(pixels, 32, 300, 16, NULL, &noise);

    TEST_ASSERT_TRUE(noise.sigma < 0.0f && noise.tiles == 0);
    TEST_ASSERT_TRUE(hdrg_noise_at(&noise, 0.5f) == 0.0f);

    free(pixels);
    pixels = frame16(33, 33, level_mid, sigma_const, false);
    estimate(pixels, 33, 33, 16, NULL, &noise);

    TEST_ASSERT_TRUE(noise.tiles == 1 && noise.sigma > 0.0f);

    free(pixels);
}

/* the edge with per-channel noise of 3 codes, or none */
static uint8_t *edge_frame(bool noisy)
{
    uint8_t *pixels;
    long v;
    size_t i;

    pixels = (uint8_t *)malloc((size_t)TN_EDGE_W * TN_EDGE_H * 3u);
    TEST_ASSERT_NOT_NULL(pixels);
    g_rng = 11;

    for (i = 0; i < (size_t)TN_EDGE_W * TN_EDGE_H * 3u; i++) {
        v = ((i / 3u) % TN_EDGE_W < TN_EDGE_X) ? 110 : 200;

        if (noisy) {
            v = lround((double)v + 3.0 * rng_gauss());
        }

        pixels[i] = (uint8_t)HDRG_CLAMP(v, 0L, 255L);
    }

    return pixels;
}

/*
 * a light: a white disc of radius 12 at the frame's center with a glow over
 * a gray 40 background, with per-channel noise of 3 codes or none
 */
static uint8_t *glow_frame(bool noisy)
{
    uint32_t x, y;
    uint8_t *pixels, c;
    double d, v;

    pixels = (uint8_t *)malloc((size_t)TN_EDGE_W * TN_EDGE_H * 3u);
    TEST_ASSERT_NOT_NULL(pixels);
    g_rng = 17;

    for (y = 0; y < TN_EDGE_H; y++) {
        for (x = 0; x < TN_EDGE_W; x++) {
            d = sqrt(((double)x + 0.5 - 128.0) * ((double)x + 0.5 - 128.0) +
                     ((double)y + 0.5 - 64.0) * ((double)y + 0.5 - 64.0));
            v = (d <= 12.0) ? 255.0 : 40.0 + 215.0 * 0.6 * exp(-(d - 12.0) / 24.0);

            for (c = 0; c < 3u; c++) {
                pixels[((size_t)y * TN_EDGE_W + x) * 3u + c] =
                    (uint8_t)HDRG_CLAMP(lround(v + (noisy ? 3.0 * rng_gauss() : 0.0)), 0L, 255L);
            }
        }
    }

    return pixels;
}

/*
 * The still pipeline's 16-bit PQ rows of an edge frame at out_w x out_h (0:
 * the frame's size); coring false forces the frame's noise to none (no
 * coring), cored receives whether the plan cored
 */
static void render_edge(const uint8_t *pixels, uint8_t threads, bool coring, uint32_t out_w,
                        uint32_t out_h, uint16_t *rows, bool *cored)
{
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_raw_image_t format;
    hdrg_color_t color;
    hdrg_analysis_t analysis;
    hdrg_pipeline_t pipeline;
    hdrg_context_t *ctx;
    uint32_t y0, height;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    params.mctf_strength = 0.0f;
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;
    encode.dither = false;
    encode.output_width = out_w;
    encode.output_height = out_h;
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&caps);

    memset(&format, 0, sizeof(format));
    format.width = TN_EDGE_W;
    format.height = TN_EDGE_H;
    format.bit_depth = 8;
    format.stride = (size_t)TN_EDGE_W * 3u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_open(ctx, &format, &color, &effective, &encode,
                                                      &caps, 0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_output(ctx, &pipeline, 16, false));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_analyze(&pipeline, pixels, NULL, &analysis));

    if (!coring) {
        analysis.noise.sigma = 0.0f;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_plan(ctx, &pipeline, pixels, &analysis));
    *cored = pipeline.plan.cored;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_bind(ctx, &pipeline));
    height = (out_h > 0) ? out_h : TN_EDGE_H;

    for (y0 = 0; y0 < height; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, hdrg_pipeline_rows(
                         ctx, &pipeline, y0, HDRG_MIN(HDRG_APPLY_BAND_ROWS, height - y0),
                         (uint8_t *)(rows + (size_t)y0 * ((out_w > 0) ? out_w : TN_EDGE_W) * 3u)));
    }

    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);
    hdrg_context_destroy(ctx);
}

/* mean and standard deviation of log2 Y (Display P3) of the output over columns [x0, x1) */
static void edge_region(const uint16_t *rows, uint32_t x0, uint32_t x1, double *mean, double *sd)
{
    const uint16_t *p;
    const double k[3] = {0.2289746, 0.6917385, 0.0792869};
    uint32_t x, y;
    uint8_t c;
    double s, ss, n, l;

    s = 0.0;
    ss = 0.0;
    n = 0.0;

    for (y = 8; y < TN_EDGE_H - 8u; y++) {
        for (x = x0; x < x1; x++) {
            p = rows + ((size_t)y * TN_EDGE_W + x) * 3u;
            l = 0.0;

            for (c = 0; c < 3u; c++) {
                l += k[c] * (double)hdrg_math_pq_eotf((float)p[c] / 65535.0f);
            }

            l = log2(l);
            s += l;
            ss += l * l;
            n += 1.0;
        }
    }

    *mean = s / n;
    *sd = sqrt(ss / n - *mean * *mean);
}

/*
 * the standard deviation of log2 Y (Display P3) of a over b's, over the
 * pixels [r0, r1) from the glow frame's center: the noise a adds
 */
static double glow_noise(const uint16_t *a, const uint16_t *b, double r0, double r1)
{
    const uint16_t *p, *q;
    const double k[3] = {0.2289746, 0.6917385, 0.0792869};
    uint32_t x, y;
    uint8_t c;
    double s, ss, n, la, lb, d;

    s = 0.0;
    ss = 0.0;
    n = 0.0;

    for (y = 0; y < TN_EDGE_H; y++) {
        for (x = 0; x < TN_EDGE_W; x++) {
            d = sqrt(((double)x + 0.5 - 128.0) * ((double)x + 0.5 - 128.0) +
                     ((double)y + 0.5 - 64.0) * ((double)y + 0.5 - 64.0));

            if (d < r0 || d >= r1) {
                continue;
            }

            p = a + ((size_t)y * TN_EDGE_W + x) * 3u;
            q = b + ((size_t)y * TN_EDGE_W + x) * 3u;
            la = 0.0;
            lb = 0.0;

            for (c = 0; c < 3u; c++) {
                la += k[c] * (double)hdrg_math_pq_eotf((float)p[c] / 65535.0f);
                lb += k[c] * (double)hdrg_math_pq_eotf((float)q[c] / 65535.0f);
            }

            d = log2(la) - log2(lb);
            s += d;
            ss += d * d;
            n += 1.0;
        }
    }

    return sqrt(ss / n - (s / n) * (s / n));
}

static void test_coring(void)
{
    const size_t samples = (size_t)TN_EDGE_W * TN_EDGE_H * 3u;
    uint16_t *on, *off, *clean;
    uint8_t *pixels;
    bool cored;
    double mean_on, mean_off, sd_on, sd_off, left_on, left_off, sd;

    on = (uint16_t *)malloc(samples * sizeof(uint16_t));
    off = (uint16_t *)malloc(samples * sizeof(uint16_t));
    clean = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(on != NULL && off != NULL && clean != NULL);

    /* a clean frame is not cored */
    pixels = edge_frame(false);
    render_edge(pixels, 1, true, 0, 0, on, &cored);
    TEST_ASSERT_FALSE(cored);
    free(pixels);

    pixels = edge_frame(true);
    render_edge(pixels, 1, true, 0, 0, on, &cored);
    TEST_ASSERT_TRUE(cored);
    render_edge(pixels, 1, false, 0, 0, off, &cored);
    TEST_ASSERT_FALSE(cored);

    /*
     * a diffuse surface takes one gain (no noise to amplify): coring leaves
     * its noise and the step as they are
     */
    edge_region(on, TN_EDGE_X + 4u, TN_EDGE_X + 40u, &mean_on, &sd_on);
    edge_region(off, TN_EDGE_X + 4u, TN_EDGE_X + 40u, &mean_off, &sd_off);
    edge_region(on, 16, TN_EDGE_X - 16u, &left_on, &sd);
    edge_region(off, 16, TN_EDGE_X - 16u, &left_off, &sd);
    printf("edge: log2 Y sd %.4f -> %.4f, step %.4f -> %.4f stops\n", sd_off, sd_on,
           mean_off - left_off, mean_on - left_on);

    TEST_ASSERT_TRUE(sd_on <= 1.01 * sd_off);
    TEST_ASSERT_TRUE(fabs((mean_on - left_on) - (mean_off - left_off)) < 0.03);
    free(pixels);

    /* a light's glow, where the boost follows La: coring keeps its noise from being amplified */
    pixels = glow_frame(false);
    render_edge(pixels, 1, true, 0, 0, clean, &cored);
    free(pixels);
    pixels = glow_frame(true);
    render_edge(pixels, 1, true, 0, 0, on, &cored);
    TEST_ASSERT_TRUE(cored);
    render_edge(pixels, 1, false, 0, 0, off, &cored);
    sd_on = glow_noise(on, clean, 16.0, 40.0);
    sd_off = glow_noise(off, clean, 16.0, 40.0);
    printf("glow: log2 Y noise %.4f -> %.4f\n", sd_off, sd_on);

    TEST_ASSERT_TRUE(sd_on < 0.9 * sd_off);

    free(pixels);
    free(on);
    free(off);
    free(clean);
}

static void test_coring_workers(void)
{
    const size_t samples = (size_t)TN_SCALED_W * TN_SCALED_H * 3u;
    uint16_t *one, *many;
    uint8_t *pixels;
    bool cored;

    HDRG_TEST_REQUIRE_THREADS();

    one = (uint16_t *)malloc(samples * sizeof(uint16_t));
    many = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(one != NULL && many != NULL);
    pixels = edge_frame(true);

    render_edge(pixels, 1, true, 0, 0, one, &cored);
    render_edge(pixels, 8, true, 0, 0, many, &cored);

    TEST_ASSERT_TRUE(cored);
    TEST_ASSERT_EQUAL_MEMORY(one, many, (size_t)TN_EDGE_W * TN_EDGE_H * 3u * sizeof(uint16_t));

    /* the bands of the SCALE stage: the 3x3 mean stays inside each band */
    render_edge(pixels, 1, true, TN_SCALED_W, TN_SCALED_H, one, &cored);
    render_edge(pixels, 8, true, TN_SCALED_W, TN_SCALED_H, many, &cored);

    TEST_ASSERT_TRUE(cored);
    TEST_ASSERT_EQUAL_MEMORY(one, many, samples * sizeof(uint16_t));

    free(pixels);
    free(one);
    free(many);
}

/* 8-pixel blocks of levels, an edge */
static double level_tiles(uint32_t x, uint32_t y)
{
    return 0.3 + 0.002 * (double)(((x / 8u) * 7u + (y / 8u) * 13u) % 17u) +
           ((x > 600u) ? 0.3 : 0.0);
}

static double sigma_two(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;

    return 2.0;
}

/* the grid's windows of a frame gathered as the GPU does: the same estimate, bit for bit */
static void test_mosaic(void)
{
    hdrg_color_t color;
    hdrg_raw_image_t raw;
    hdrg_source_t src, mosaic;
    hdrg_noise_grid_t grid;
    hdrg_noise_t direct, gathered;
    uint32_t r, y, x0, y0;
    uint16_t *pixels, *tiles;
    size_t row;

    pixels = frame16(TN_MOSAIC_W, TN_MOSAIC_H, level_tiles, sigma_two, true);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)pixels;
    raw.width = TN_MOSAIC_W;
    raw.height = TN_MOSAIC_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TN_MOSAIC_W * 6u;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));
    TEST_ASSERT_TRUE(hdrg_noise_grid(TN_MOSAIC_W, TN_MOSAIC_H, &grid));
    TEST_ASSERT_TRUE(grid.step > 1u);

    row = (size_t)grid.nx * HDRG_NOISE_WINDOW * 3u;
    tiles = (uint16_t *)malloc(row * grid.ny * HDRG_NOISE_WINDOW * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(tiles);

    for (r = 0; r < grid.nx * grid.ny; r++) {
        x0 = (grid.first_x + (r % grid.nx) * grid.step) * HDRG_NOISE_TILE - 1u;
        y0 = (grid.first_y + (r / grid.nx) * grid.step) * HDRG_NOISE_TILE - 1u;

        for (y = 0; y < HDRG_NOISE_WINDOW; y++) {
            memcpy(tiles + (((size_t)(r / grid.nx) * HDRG_NOISE_WINDOW + y) * row) +
                       ((size_t)(r % grid.nx) * HDRG_NOISE_WINDOW * 3u),
                   pixels + (((size_t)(y0 + y) * TN_MOSAIC_W + x0) * 3u),
                   HDRG_NOISE_WINDOW * 3u * sizeof(uint16_t));
        }
    }

    mosaic = src;
    mosaic.pixels = (const uint8_t *)tiles;
    mosaic.stride = row * sizeof(uint16_t);
    mosaic.width = grid.nx * HDRG_NOISE_WINDOW;
    mosaic.height = grid.ny * HDRG_NOISE_WINDOW;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_noise_estimate(&g_alloc, NULL, &src, &direct));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_noise_estimate_mosaic(&g_alloc, NULL, &mosaic, &grid, &gathered));
    TEST_ASSERT_TRUE(direct.sigma > 0.0f && direct.blockiness > 1.0f);
    TEST_ASSERT_EQUAL_MEMORY(&direct, &gathered, sizeof(direct));

    free(tiles);
    free(pixels);
    hdrg_source_free(&g_alloc, &src);
    hdrg_color_free(&g_alloc, &color);
}

void setUp(void)
{
    g_alloc = hdrg_test_allocator();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flat_luma);
    RUN_TEST(test_flat_channels);
    RUN_TEST(test_level_function);
    RUN_TEST(test_edges_texture);
    RUN_TEST(test_clipped_8bit);
    RUN_TEST(test_clean);
    RUN_TEST(test_blockiness);
    RUN_TEST(test_ringing);
    RUN_TEST(test_workers);
    RUN_TEST(test_small);
    RUN_TEST(test_coring);
    RUN_TEST(test_coring_workers);
    RUN_TEST(test_mosaic);

    return UNITY_END();
}
