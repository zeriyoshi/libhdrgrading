/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "core/gpu_analyze.h"
#include "core/plan.h"
#include "priv.h"

/*
 * The analysis's sums on the GPU (hdrg_context_desc_t.gpu_analysis FAST,
 * src/core/gpu_analyze, src/core/wgsl/analyze.wgsl) against the CPU's pass A
 * and pass B on a letterboxed 16-bit picture of a lamp with a glow, a white
 * panel, a ramp and grain, on 4 x 4 and 2 x 2 cells: the black bars the
 * CPU's exactly, the statistics, the lights and the plan within the GPU's
 * tolerance, the GPU's the same twice. Ignored without a
 * GPU.
 */
#define TZ_W   317u /* odd: the cells' last column */
#define TZ_H   181u
#define TZ_BAR 14u /* black rows at the top and the bottom */
#define TZ_KEY UINT64_C(0x545a5f414e414c59)

static hdrg_allocator_t g_alloc;
static hdrg_context_t *g_ctx;
static hdrg_gpu_core_t *g_gpu;     /* the core of the check running */
static hdrg_gpu_core_t *g_gpus[3]; /* Metal, Vulkan, WebGPU (NULL: none) */

/* the picture's 16-bit R'G'B' (sRGB codes x 257 and grain) */
static uint16_t *tz_picture(void)
{
    uint32_t x, y, h;
    uint16_t *rgb;
    uint8_t c;
    double d, v[3], glow;

    rgb = (uint16_t *)malloc((size_t)TZ_W * TZ_H * 6u);
    TEST_ASSERT_NOT_NULL(rgb);

    for (y = 0; y < TZ_H; y++) {
        for (x = 0; x < TZ_W; x++) {
            d = ((double)x - 90.0) * ((double)x - 90.0) + ((double)y - 80.0) * ((double)y - 80.0);
            glow = 0.85 * (1.0 / (1.0 + d / 120.0));
            v[0] = 0.20 + 0.25 * (double)x / TZ_W + glow;
            v[1] = 0.18 + 0.20 * (double)y / TZ_H + glow;
            v[2] = 0.30 + glow * 0.9;

            /* a white panel with a sharp edge */
            if (x >= 220u && x < 280u && y >= 50u && y < 120u) {
                v[0] = 1.0;
                v[1] = 1.0;
                v[2] = 1.0;
            }

            for (c = 0; c < 3u; c++) {
                h = hdrg_hash_u32(TZ_KEY, x, y, c);
                v[c] += ((double)(h % 9u) - 4.0) / 255.0;
                v[c] = (v[c] < 0.0) ? 0.0 : ((v[c] > 1.0) ? 1.0 : v[c]);

                if (y < TZ_BAR || y >= TZ_H - TZ_BAR) {
                    v[c] = 0.0;
                }

                rgb[((size_t)y * TZ_W + x) * 3u + c] = (uint16_t)(v[c] * 65535.0 + 0.5);
            }
        }
    }

    return rgb;
}

static void tz_source(const uint16_t *rgb, hdrg_source_t *src)
{
    hdrg_raw_image_t raw;
    hdrg_color_t color;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)rgb;
    raw.width = TZ_W;
    raw.height = TZ_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TZ_W * 6u;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, src));
    TEST_ASSERT_TRUE(hdrg_gpu_analyze_takes(src));
}

/* the GPU's moments for hdrg_plan_build_with (the picture uploaded) */
static hdrg_status_t tz_moments(void *arg, const hdrg_source_t *src,
                                const hdrg_analysis_t *analysis, uint8_t light_shift, uint32_t cell,
                                uint32_t grid_w, uint32_t grid_h, double *sums, float *grid)
{
    hdrg_gpu_analyze_t *work = (hdrg_gpu_analyze_t *)arg;
    hdrg_gpu_picture_t picture;

    memset(&picture, 0, sizeof(picture));
    picture.src = src;
    picture.width = src->width;
    picture.height = src->height;
    picture.src_width = src->width;
    picture.src_height = src->height;
    picture.slot = HDRG_GC_SLOTS;

    return hdrg_gpu_analyze_moments(g_gpu, NULL, work, &picture, analysis, light_shift, cell,
                                    grid_w, grid_h, sums, grid);
}

/* the largest |a - b| over n floats */
static float tz_worst(const float *a, const float *b, size_t n)
{
    size_t i;
    float worst, d;

    worst = 0.0f;

    for (i = 0; i < n; i++) {
        d = fabsf(a[i] - b[i]);
        worst = (d > worst) ? d : worst;
    }

    return worst;
}

/* pass A and pass B on the CPU and on the GPU, cells of 2^shift pixels */
static void tz_check(uint8_t shift)
{
    hdrg_analyze_opts_t opts;
    hdrg_analyze_given_t given, again;
    hdrg_gpu_analyze_t work;
    hdrg_gpu_picture_t picture;
    hdrg_analysis_t cpu, gpu;
    hdrg_source_t src;
    hdrg_params_t params;
    hdrg_plan_t cpu_plan, gpu_plan;
    uint16_t *rgb;
    uint8_t b;
    size_t cells, cells_bytes;
    char line[200];
    float worst;
    void *kept;

    rgb = tz_picture();
    tz_source(rgb, &src);
    memset(&work, 0, sizeof(work));
    memset(&picture, 0, sizeof(picture));
    picture.src = &src;
    picture.width = TZ_W;
    picture.height = TZ_H;
    picture.src_width = TZ_W;
    picture.src_height = TZ_H;
    picture.slot = HDRG_GC_SLOTS;

    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.bars = true;
    opts.lights = true;
    opts.cell_shift = shift;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_analyze(&g_alloc, NULL, &src, &opts, &cpu));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_gpu_analyze_stats(g_gpu, &g_alloc, NULL, &work, &picture, shift, &given));

    /* the GPU the same twice */
    cells = (size_t)((TZ_W + (1u << shift) - 1u) >> shift) * ((TZ_H + (1u << shift) - 1u) >> shift);
    cells_bytes = cells * HDRG_ANALYZE_CELLS * sizeof(float);
    kept = malloc(cells_bytes);
    TEST_ASSERT_NOT_NULL(kept);
    memcpy(kept, given.cells, cells_bytes);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_gpu_analyze_stats(g_gpu, &g_alloc, NULL, &work, &picture, shift, &again));
    TEST_ASSERT_EQUAL_MEMORY(kept, again.cells, cells_bytes);
    free(kept);

    opts.given = &again;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_analyze(&g_alloc, NULL, &src, &opts, &gpu));

    /* the bars exactly; the statistics and the lights within the tolerance */
    for (b = 0; b < 4u; b++) {
        TEST_ASSERT_EQUAL_UINT32(cpu.bars[b], gpu.bars[b]);
    }

    TEST_ASSERT_EQUAL_UINT32(TZ_BAR, cpu.bars[0]);
    TEST_ASSERT_EQUAL_UINT32(TZ_BAR, cpu.bars[1]);
    HDRG_TEST_NEAR(1e-3, cpu.p995_luma, gpu.p995_luma);
    HDRG_TEST_NEAR(1e-4, cpu.key_luma, gpu.key_luma);
    TEST_ASSERT_EQUAL_UINT32(cpu.light_count, gpu.light_count);
    TEST_ASSERT_TRUE(cpu.light_count > 0u);
    worst = tz_worst(cpu.light.data, gpu.light.data, (size_t)cpu.light.width * cpu.light.height);
    snprintf(line, sizeof(line), "cells of %u: the lights' boosts at most %g apart", 1u << shift,
             (double)worst);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(worst <= 1e-3f);

    /* pass B: the plan on the CPU's analysis, its moments on the CPU and on the GPU */
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_plan_build(&g_alloc, NULL, NULL, &src, &cpu, 0, &params, &cpu_plan));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_plan_build_with(&g_alloc, NULL, NULL, &src, &cpu, 0,
                                                        &params, tz_moments, &work, &gpu_plan));
    cells = (size_t)cpu_plan.grid_w * cpu_plan.grid_h;
    worst = tz_worst(cpu_plan.boost, gpu_plan.boost, cells * HDRG_PLAN_BINS);
    snprintf(line, sizeof(line), "cells of %u: the plans' boost shares at most %g apart",
             1u << shift, (double)worst);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(worst <= 1e-4f);
    TEST_ASSERT_TRUE(tz_worst(cpu_plan.apl_y, gpu_plan.apl_y, cells) <= 1e-5f);
    TEST_ASSERT_TRUE(tz_worst(cpu_plan.apl_boost, gpu_plan.apl_boost, cells) <= 1e-4f);
    TEST_ASSERT_TRUE(tz_worst(cpu_plan.boost_near, gpu_plan.boost_near, cells) <= 1e-4f);

    hdrg_plan_free(&g_alloc, &cpu_plan);
    hdrg_plan_free(&g_alloc, &gpu_plan);
    hdrg_analysis_free(&g_alloc, &cpu);
    hdrg_analysis_free(&g_alloc, &gpu);
    hdrg_gpu_analyze_free(&g_alloc, &work);
    hdrg_source_free(&g_alloc, &src);
    free(rgb);
}

static void test_cells(void)
{
    uint8_t k, ran;

    ran = 0;

    for (k = 0; k < 3u; k++) {
        g_gpu = g_gpus[k];

        if (g_gpu != NULL) {
            tz_check(HDRG_QUARTER_SHIFT);
            tz_check(HDRG_FINE_SHIFT);
            ran++;
        }
    }

    if (ran == 0u) {
        TEST_IGNORE_MESSAGE("no GPU");
    }
}

/* the bars from the rows' and the columns' maxima: hdrg_analyze's rule */
static void test_bars(void)
{
    uint32_t bars[4];
    uint8_t i;
    float rows[10], columns[8];

    for (i = 0; i < 10u; i++) {
        rows[i] = (i < 2u || i >= 9u) ? 0.0f : 0.5f;
    }

    for (i = 0; i < 8u; i++) {
        columns[i] = (i == 0u) ? 0.001f : 0.5f;
    }

    hdrg_analyze_bars(rows, columns, 8, 10, bars);
    TEST_ASSERT_EQUAL_UINT32(2, bars[0]);
    TEST_ASSERT_EQUAL_UINT32(1, bars[1]);
    TEST_ASSERT_EQUAL_UINT32(1, bars[2]);
    TEST_ASSERT_EQUAL_UINT32(0, bars[3]);

    /* under a quarter left: none */
    for (i = 0; i < 10u; i++) {
        rows[i] = (i == 5u) ? 0.5f : 0.0f;
    }

    hdrg_analyze_bars(rows, columns, 8, 10, bars);

    for (i = 0; i < 4u; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, bars[i]);
    }
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    hdrg_context_desc_t desc;

    g_alloc = hdrg_test_allocator();
    hdrg_context_desc_init(&desc);

    if (hdrg_context_create(&desc, &g_ctx) == HDRG_OK) {
        if (hdrg_gpu_core_create(&g_ctx->host, HDRG_DEVICE_METAL, &g_gpus[0]) != HDRG_OK) {
            g_gpus[0] = NULL;
        }

        if (hdrg_gpu_core_create(&g_ctx->host, HDRG_DEVICE_VULKAN, &g_gpus[1]) != HDRG_OK) {
            g_gpus[1] = NULL;
        }

        if (hdrg_gpu_core_create(&g_ctx->host, HDRG_DEVICE_WEBGPU, &g_gpus[2]) != HDRG_OK) {
            g_gpus[2] = NULL;
        }
    }

    UNITY_BEGIN();
    RUN_TEST(test_cells);
    RUN_TEST(test_bars);

    hdrg_gpu_core_destroy(g_gpus[0]);
    hdrg_gpu_core_destroy(g_gpus[1]);
    hdrg_gpu_core_destroy(g_gpus[2]);
    hdrg_context_destroy(g_ctx);

    return UNITY_END();
}
