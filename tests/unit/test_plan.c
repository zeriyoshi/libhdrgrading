/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <unity.h>

#include "../test_util.h"
#include "core/analyze.h"
#include "core/plan.h"
#include "core/predict.h"
#if HDRG_BUILTIN_PREDICT
#include "plugins/predict/parts.h"
#endif

/*
 * 解析とプラン: 周りがなだらかに落ちる明るい領域 (グロー) は光源で、
 * その上乗せは格子を引いても周りの平坦な背景へ漏れない。縁が鋭い平坦な面・文字・
 * 白飛びしただけの小さな塊は拡散面。フェードで暗くなるフレームも判定と上乗せを保つ。
 * 芯が光の閾値をまたいで明るくなる光は、上乗せが飛ばずに上がる。土手の輪郭線の近くの水面の
 * きらめき (落ち方が片側だけ) は光でない。拡散面の倍率は key
 * で決まる。プランはワーカー数によらない。 色の濃い光 (opts.colored): グローのある赤い LED
 * は光で、切り替えなしでは拡散面。
 * 輪郭のある赤い面・クリップしない淡いピンクのぼかし・白い光の色のにじみは光を
 * 増やさない。地平線のかすみの上の青でクリップした空、暗い赤の梁の上の淡いピンクの
 * ハイライト、明るい背景の上の縁のぼけた黄色い円は光ではない。
 */
#define TP_W 192u
#define TP_H 128u

static hdrg_allocator_t g_alloc;

static void build(const uint8_t *pixels, const hdrg_params_t *params, const hdrg_exec_t *exec,
                  const hdrg_analyze_opts_t *opts, hdrg_source_t *src, hdrg_analysis_t *analysis,
                  hdrg_plan_t *plan)
{
    hdrg_color_t color;
    hdrg_raw_image_t raw;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3;

    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, src));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_analyze(&g_alloc, exec, src, opts, analysis));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_plan_build(&g_alloc, exec, NULL, src, analysis, 0, params, plan));
}

static void release(hdrg_source_t *src, hdrg_analysis_t *analysis, hdrg_plan_t *plan)
{
    hdrg_plan_free(&g_alloc, plan);
    hdrg_analysis_free(&g_alloc, analysis);
    hdrg_source_free(&g_alloc, src);
}

/* the plan's boost share at (x, y), sliced at the source's La */
static float boost_at(const hdrg_source_t *src, const hdrg_plan_t *plan, uint32_t x, uint32_t y)
{
    hdrg_grid_pos_t pos;
    float rgb[3];

    if (plan->boost == NULL) {
        return 0.0f;
    }

    hdrg_source_rgb(src, hdrg_source_row(src, y), x, rgb);
    hdrg_plan_pos(plan, x, y, &pos);

    return hdrg_boost_at(plan->boost, plan->grid_w, &pos,
                         hdrg_rgb_la(hdrg_rgb_y(src->coeffs, rgb), rgb));
}

/* the largest boost share over the box [x0, x1) x [y0, y1) */
static float boost_max(const hdrg_source_t *src, const hdrg_plan_t *plan, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1)
{
    uint32_t x, y;
    float m;

    m = 0.0f;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            m = fmaxf(m, boost_at(src, plan, x, y));
        }
    }

    return m;
}

static void put(uint8_t *pixels, uint32_t x, uint32_t y, float v)
{
    const uint8_t code = (uint8_t)lrintf(fminf(fmaxf(v, 0.0f), 255.0f));

    pixels[((size_t)y * TP_W + x) * 3 + 0] = code;
    pixels[((size_t)y * TP_W + x) * 3 + 1] = code;
    pixels[((size_t)y * TP_W + x) * 3 + 2] = code;
}

/*
 * a gray picture of background code back times k: a disc of radius 10 at
 * (48, 64) with a glow (glow true) or a sharp edge, a white box outlined in
 * black at (120..170, 40..90)
 */
static uint8_t *picture(float back, bool glow, float k)
{
    uint32_t x, y;
    uint8_t *pixels;
    float d, v;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            d = sqrtf(((float)x + 0.5f - 48.0f) * ((float)x + 0.5f - 48.0f) +
                      ((float)y + 0.5f - 64.0f) * ((float)y + 0.5f - 64.0f));
            v = back;

            if (glow && d > 10.0f) {
                v += (255.0f - back) * 0.55f * expf(-(d - 10.0f) / 20.0f);
            }

            v += (255.0f - v) * fminf(fmaxf(10.5f - d, 0.0f), 1.0f);

            if (x >= 117u && x < 173u && y >= 37u && y < 93u) {
                v = 20.0f;
            }

            if (x >= 120u && x < 170u && y >= 40u && y < 90u) {
                v = 240.0f;
            }

            put(pixels, x, y, v * k);
        }
    }

    return pixels;
}

/* the glowing disc of picture() on code 110 with its core at code core (the glow scaled to it) */
static uint8_t *core_picture(float core)
{
    uint32_t x, y;
    uint8_t *pixels;
    float d, v;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            d = sqrtf(((float)x + 0.5f - 48.0f) * ((float)x + 0.5f - 48.0f) +
                      ((float)y + 0.5f - 64.0f) * ((float)y + 0.5f - 64.0f));
            v = 110.0f;

            if (d > 10.0f) {
                v += (core - 110.0f) * 0.55f * expf(-(d - 10.0f) / 20.0f);
            }

            v += (core - v) * fminf(fmaxf(10.5f - d, 0.0f), 1.0f);
            put(pixels, x, y, v);
        }
    }

    return pixels;
}

static void params_for(hdrg_params_t *params)
{
    hdrg_params_init(params, HDRG_PRESET_ANIME);
}

static void put_rgb(uint8_t *pixels, uint32_t x, uint32_t y, const float rgb[3])
{
    uint8_t c;

    for (c = 0; c < 3; c++) {
        pixels[((size_t)y * TP_W + x) * 3 + c] =
            (uint8_t)lrintf(fminf(fmaxf(rgb[c], 0.0f), 255.0f));
    }
}

/* px toward color by a (clamped to 0..1) */
static void mix(float *px, const float color[3], float a)
{
    uint8_t c;

    a = fminf(fmaxf(a, 0.0f), 1.0f);

    for (c = 0; c < 3; c++) {
        px[c] += (color[c] - px[c]) * a;
    }
}

/*
 * an anime river: grass (rows 0..39) over a black outline (rows 40, 41),
 * water darkening below it from code (132, 193, 238) to (95, 154, 212), two
 * white glints 48 pixels long (a row of code (230, 255, 255) over one of
 * (176, 227, 252)): one 8 pixels below the bank (row 50), one in the middle
 * of the water (row 90)
 */
static uint8_t *glint_picture(void)
{
    const float grass[3] = {58.0f, 128.0f, 74.0f}, ink[3] = {18.0f, 41.0f, 33.0f},
                top[3] = {132.0f, 193.0f, 238.0f}, low[3] = {95.0f, 154.0f, 212.0f},
                core[3] = {230.0f, 255.0f, 255.0f}, edge[3] = {176.0f, 227.0f, 252.0f};
    uint32_t x, y;
    uint8_t *pixels;
    float px[3];

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            memcpy(px, top, sizeof(px));
            mix(px, low, ((float)y - 42.0f) / 70.0f);

            if (y < 40u) {
                memcpy(px, grass, sizeof(px));
            } else if (y < 42u) {
                memcpy(px, ink, sizeof(px));
            }

            if (x >= 72u && x < 120u && (y == 50u || y == 90u)) {
                memcpy(px, core, sizeof(px));
            } else if (x >= 72u && x < 120u && (y == 51u || y == 91u)) {
                memcpy(px, edge, sizeof(px));
            }

            put_rgb(pixels, x, y, px);
        }
    }

    return pixels;
}

/*
 * on a dark wall: a red LED (a disc of radius 6 at (36, 40), its glow of
 * sigma 10), a red disc clipped in red with a black outline (radius 14 at
 * (100, 40)), a soft pink blob peaking at code 245 (sigma 8 at (160, 40)),
 * a white disc with a magenta glow (radius 4 at (64, 100))
 */
static uint8_t *colored_picture(void)
{
    const float wall[3] = {25.0f, 25.0f, 35.0f}, red[3] = {255.0f, 30.0f, 20.0f},
                glow[3] = {255.0f, 0.0f, 0.0f}, fill[3] = {255.0f, 40.0f, 50.0f},
                ink[3] = {10.0f, 10.0f, 15.0f}, pink[3] = {245.0f, 150.0f, 190.0f},
                white[3] = {255.0f, 250.0f, 255.0f}, magenta[3] = {255.0f, 60.0f, 200.0f};
    uint32_t x, y;
    uint8_t *pixels;
    float px[3], d;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            memcpy(px, wall, sizeof(px));

            d = hypotf((float)x + 0.5f - 36.0f, (float)y + 0.5f - 40.0f);
            mix(px, glow, (d > 6.0f) ? 0.7f * expf(-(d - 6.0f) * (d - 6.0f) / 200.0f) : 0.0f);
            mix(px, red, 6.5f - d);

            d = hypotf((float)x + 0.5f - 100.0f, (float)y + 0.5f - 40.0f);
            mix(px, ink, 16.5f - d);
            mix(px, fill, 14.5f - d);

            d = hypotf((float)x + 0.5f - 160.0f, (float)y + 0.5f - 40.0f);
            mix(px, pink, expf(-d * d / 128.0f));

            d = hypotf((float)x + 0.5f - 64.0f, (float)y + 0.5f - 100.0f);
            mix(px, magenta, (d > 4.0f) ? 0.75f * expf(-(d - 4.0f) * (d - 4.0f) / 128.0f) : 0.0f);
            mix(px, white, 4.5f - d);

            put_rgb(pixels, x, y, px);
        }
    }

    return pixels;
}

/* a disc with a glow is a light; the outlined white box and the background stay diffuse */
static void test_glow_is_light(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    pixels = picture(110.0f, true, 1.0f);
    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);

    TEST_ASSERT_TRUE(boost_at(&src, &plan, 48, 64) >= 0.9f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 124, 44, 166, 86) <= 0.01f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 4, 4, 20, 20) <= 0.01f);
    TEST_ASSERT_TRUE(analysis.light_ratio > 0.0f);

    release(&src, &analysis, &plan);
    free(pixels);
}

/* the same disc with a sharp edge (a flat fill, a sign, a UI element) stays diffuse */
static void test_sharp_is_diffuse(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    pixels = picture(110.0f, false, 1.0f);
    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);

    TEST_ASSERT_TRUE(boost_max(&src, &plan, 0, 0, TP_W, TP_H) <= 0.01f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, analysis.light_ratio);

    release(&src, &analysis, &plan);
    free(pixels);
}

/* a small clipped spot and a thin stroke without a glow stay diffuse: clipping alone is no light */
static void test_clipped_spot(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint32_t x, y;
    uint8_t *pixels;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            put(pixels, x, y, (x >= 40u && x < 46u && y >= 40u && y < 46u) ? 255.0f : 90.0f);

            if (y >= 100u && y < 102u && x >= 20u && x < 170u) {
                put(pixels, x, y, 255.0f);
            }
        }
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 42, 42) <= 0.01f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 20, 99, 170, 103) <= 0.01f);
    release(&src, &analysis, &plan);

    free(pixels);
}

/*
 * the boost slices by La: the background right next to the glowing disc's
 * edge takes none past its glow, and neither does a dark outline near a light
 */
static void test_no_leak(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    pixels = picture(110.0f, true, 1.0f);
    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);

    /* far out in the glow the background level: no boost */
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 48, 120) <= 0.02f);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 100, 64) <= 0.02f);

    release(&src, &analysis, &plan);
    free(pixels);
}

/*
 * a frame of a fade (darkened by k, analyzed with the session's bright_scale)
 * keeps the light's judgment and its boost at each pixel
 */
static void test_fade_keeps_light(void)
{
    hdrg_analyze_opts_t opts;
    hdrg_params_t params;
    hdrg_source_t src, faded_src;
    hdrg_analysis_t analysis, faded;
    hdrg_plan_t plan, faded_plan;
    uint8_t *pixels, *dim;

    params_for(&params);
    pixels = picture(110.0f, true, 1.0f);
    dim = picture(110.0f, true, 0.4f);
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = powf(0.4f, 2.2f);

    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);
    build(dim, &params, NULL, &opts, &faded_src, &faded, &faded_plan);

    TEST_ASSERT_TRUE(boost_at(&faded_src, &faded_plan, 48, 64) >= 0.9f);
    HDRG_TEST_NEAR(0.1, boost_at(&src, &plan, 48, 75), boost_at(&faded_src, &faded_plan, 48, 75));

    release(&src, &analysis, &plan);
    release(&faded_src, &faded, &faded_plan);
    free(pixels);
    free(dim);
}

/*
 * a light whose core brightens across the light threshold (La 0.85, code 238) to white: its
 * boost rises with its brightest channel, without a jump (a small light of a moving picture
 * crossing the threshold does not flicker)
 */
static void test_threshold_ramp(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint32_t code;
    uint8_t *pixels;
    float boost, last, step;

    params_for(&params);
    last = 0.0f;
    step = 0.0f;

    for (code = 230u; code <= 255u; code++) {
        pixels = core_picture((float)code);
        build(pixels, &params, NULL, NULL, &src, &analysis, &plan);
        boost = boost_at(&src, &plan, 48, 64);
        release(&src, &analysis, &plan);
        free(pixels);

        if (code < 238u) {
            TEST_ASSERT_TRUE(boost <= 0.01f);
        }

        step = fmaxf(step, fabsf(boost - last));
        last = boost;
    }

    TEST_ASSERT_TRUE(last >= 0.9f);
    TEST_ASSERT_TRUE(step <= 0.2f);
}

/* the lights of boost 0.5 and more in the analysis' list */
static uint32_t lit_count(const hdrg_analysis_t *analysis)
{
    uint32_t k, n;

    n = 0;
    for (k = 0; k < analysis->light_count; k++) {
        n += (analysis->lights[k].boost >= 0.5f) ? 1u : 0u;
    }

    return n;
}

/*
 * colored lights: the red LED is a light with them and diffuse without; the
 * outlined red disc and the pink blob stay diffuse; the white disc's magenta
 * glow is that light's, not one more
 */
static void test_colored_lights(void)
{
    hdrg_analyze_opts_t opts;
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    pixels = colored_picture();
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.lights = true;

    build(pixels, &params, NULL, &opts, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 36, 40) <= 0.01f);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 64, 100) >= 0.9f);
    TEST_ASSERT_EQUAL_UINT32(1, lit_count(&analysis));
    release(&src, &analysis, &plan);

    opts.colored = true;
    build(pixels, &params, NULL, &opts, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 36, 40) >= 0.9f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 90, 30, 110, 50) <= 0.01f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 150, 30, 170, 50) <= 0.01f);
    TEST_ASSERT_TRUE(boost_at(&src, &plan, 64, 100) >= 0.9f);
    TEST_ASSERT_EQUAL_UINT32(2, lit_count(&analysis));
    release(&src, &analysis, &plan);

    free(pixels);
}

/*
 * a sky clipped in blue over a hazy horizon falls off below like a glow, but
 * its ring is brighter than the sky: no light
 */
static void test_colored_sky(void)
{
    const float ground[3] = {60.0f, 90.0f, 50.0f};
    hdrg_analyze_opts_t opts;
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint32_t x, y;
    uint8_t *pixels;
    float px[3], t;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            t = (float)y / 80.0f;
            px[0] = 70.0f + 150.0f * t * t;
            px[1] = 150.0f + 80.0f * t * t;
            px[2] = 255.0f;

            if (y >= 80u) {
                px[0] = 220.0f;
                px[1] = 230.0f;
                px[2] = 255.0f;
                mix(px, ground, ((float)y - 80.0f) / 12.0f);
            }

            put_rgb(pixels, x, y, px);
        }
    }

    params_for(&params);
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.colored = true;

    build(pixels, &params, NULL, &opts, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 0, 0, TP_W, 80) <= 0.01f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, analysis.light_ratio);
    release(&src, &analysis, &plan);

    free(pixels);
}

/*
 * saturated surfaces whose falloff looks like a glow (girder false: a yellow
 * disc with a soft edge on a light wall, its surround bright): a pale pink
 * highlight (clipped in red) along a dark red girder on a dark ground, its
 * color pale
 */
static uint8_t *surface_picture(bool girder)
{
    const float ground[3] = {20.0f, 20.0f, 30.0f}, red[3] = {150.0f, 30.0f, 40.0f},
                pink[3] = {255.0f, 180.0f, 190.0f}, wall[3] = {200.0f, 200.0f, 190.0f},
                yellow[3] = {255.0f, 220.0f, 0.0f};
    uint32_t x, y;
    uint8_t *pixels;
    float px[3], d;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            if (girder) {
                memcpy(px, ground, sizeof(px));

                if (x >= 8u && x < 184u && y >= 24u && y < 104u) {
                    memcpy(px, red, sizeof(px));
                    d = hypotf((float)x + 0.5f - fminf(fmaxf((float)x + 0.5f, 64.0f), 128.0f),
                               (float)y + 0.5f - 64.0f);
                    mix(px, pink, (d > 3.0f) ? expf(-(d - 3.0f) * (d - 3.0f) / 32.0f) : 1.0f);
                }
            } else {
                memcpy(px, wall, sizeof(px));
                d = hypotf((float)x + 0.5f - 96.0f, (float)y + 0.5f - 64.0f);
                mix(px, yellow, (d > 12.0f) ? expf(-(d - 12.0f) * (d - 12.0f) / 72.0f) : 1.0f);
            }

            put_rgb(pixels, x, y, px);
        }
    }

    return pixels;
}

/* the pink highlight on the girder and the yellow disc on the wall stay diffuse */
static void test_colored_surfaces(void)
{
    hdrg_analyze_opts_t opts;
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.colored = true;

    pixels = surface_picture(true);
    build(pixels, &params, NULL, &opts, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 60, 56, 132, 72) <= 0.01f);
    release(&src, &analysis, &plan);
    free(pixels);

    pixels = surface_picture(false);
    build(pixels, &params, NULL, &opts, &src, &analysis, &plan);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 84, 52, 108, 76) <= 0.01f);
    release(&src, &analysis, &plan);
    free(pixels);
}

/*
 * glints on water: the one by the bank, whose rays up cross the outline into
 * the darker grass, drops on one side only and is no light, like the one in
 * the middle of the water; the water around it is not lifted
 */
static void test_glint_by_bank(void)
{
    hdrg_params_t params;
    hdrg_source_t src;
    hdrg_analysis_t analysis;
    hdrg_plan_t plan;
    uint8_t *pixels;

    params_for(&params);
    pixels = glint_picture();
    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);

    TEST_ASSERT_TRUE(boost_max(&src, &plan, 64, 44, 128, 64) <= 0.05f);
    TEST_ASSERT_TRUE(boost_max(&src, &plan, 64, 84, 128, 98) <= 0.05f);

    release(&src, &analysis, &plan);
    free(pixels);
}

/* the diffuse level rises with the key from dark to normally exposed frames */
static void test_diffuse_level(void)
{
    hdrg_params_t params;
    float prev, level, key;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, hdrg_plan_diffuse(&params, 0.03f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hdrg_plan_diffuse(&params, 0.2f));

    prev = 0.0f;
    for (key = 0.0f; key <= 0.4f; key += 0.01f) {
        level = hdrg_plan_diffuse(&params, key);
        TEST_ASSERT_TRUE(level >= prev);
        prev = level;
    }

    params.diffuse_max = 1.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, hdrg_plan_diffuse(&params, 0.2f));
}

#if HDRG_BUILTIN_PREDICT
static uint32_t g_judged;

/* the built-in predictor's chances (hdrg.analyze.predict's), each record's box within the frame */
static hdrg_status_t built_in_judge(void *user, const hdrg_source_t *src,
                                    const hdrg_light_record_t *records, uint32_t count,
                                    float *chances)
{
    uint32_t k;

    (void)user;

    for (k = 0; k < count; k++) {
        if (records[k].width == 0 || records[k].height == 0 ||
            records[k].x + records[k].width > src->width ||
            records[k].y + records[k].height > src->height) {
            return HDRG_ERR_INTERNAL;
        }

        chances[k] = (float)hdrg_predict_chance(records[k].features) / (float)HDRG_PREDICT_ONE;
    }

    g_judged += count;

    return HDRG_OK;
}

/*
 * The light predictor (src/core/predict, the built-in hdrg.analyze.predict): a judged light's
 * boost scaled by (1 - amount) + amount x its chance, its cells with it, never above the rules';
 * the list not kept unless asked for; its features and chance the same on every run
 */
static void test_light_predictor(void)
{
    static const float amounts[2] = {0.5f, 1.0f};
    hdrg_params_t params;
    hdrg_analyze_opts_t opts;
    hdrg_source_t src;
    hdrg_analysis_t rules, checked, features;
    hdrg_plan_t plan;
    uint32_t k, a;
    uint8_t *pixels;
    int32_t chance;
    size_t c;
    char line[96];
    float factor;

    params_for(&params);
    pixels = picture(110.0f, true, 1.0f);
    memset(&opts, 0, sizeof(opts));
    opts.judge = built_in_judge;
    g_judged = 0;
    opts.bright_scale = 1.0f;
    opts.cell_shift = HDRG_QUARTER_SHIFT;
    opts.lights = true;
    opts.features = true;
    build(pixels, &params, NULL, &opts, &src, &features, &plan);
    hdrg_plan_free(&g_alloc, &plan);
    hdrg_source_free(&g_alloc, &src);
    TEST_ASSERT_TRUE(features.light_count > 0u);
    TEST_ASSERT_NOT_NULL(features.light_features);

    for (a = 0; a < 2u; a++) {
        opts.features = false;
        opts.predictor = 0.0f;
        build(pixels, &params, NULL, &opts, &src, &rules, &plan);
        hdrg_plan_free(&g_alloc, &plan);
        hdrg_source_free(&g_alloc, &src);
        opts.predictor = amounts[a];
        build(pixels, &params, NULL, &opts, &src, &checked, &plan);
        hdrg_plan_free(&g_alloc, &plan);
        hdrg_source_free(&g_alloc, &src);
        TEST_ASSERT_EQUAL_UINT32(rules.light_count, checked.light_count);
        TEST_ASSERT_EQUAL_UINT32(features.light_count, checked.light_count);

        for (k = 0; k < checked.light_count; k++) {
            chance = hdrg_predict_chance(features.light_features + (size_t)k * HDRG_LIGHT_FEATURES);
            TEST_ASSERT_TRUE(chance >= 0 && chance <= HDRG_PREDICT_ONE);
            TEST_ASSERT_EQUAL_INT32(chance, hdrg_predict_chance(features.light_features +
                                                                (size_t)k * HDRG_LIGHT_FEATURES));
            factor = 1.0f - amounts[a] + amounts[a] * (float)chance / (float)HDRG_PREDICT_ONE;
            TEST_ASSERT_EQUAL_FLOAT(rules.lights[k].boost * factor, checked.lights[k].boost);
            snprintf(line, sizeof(line), "light %u: boost %.3f, chance %.3f", k,
                     (double)rules.lights[k].boost, (double)chance / HDRG_PREDICT_ONE);
            TEST_MESSAGE(line);
        }

        for (c = 0; c < (size_t)checked.light.width * checked.light.height; c++) {
            TEST_ASSERT_TRUE(checked.light.data[c] <= rules.light.data[c]);
        }

        hdrg_analysis_free(&g_alloc, &rules);
        hdrg_analysis_free(&g_alloc, &checked);
    }

    TEST_ASSERT_TRUE(g_judged > 0u);

    /* not asked for: no list kept, and amount 0 is the rules' bytes */
    opts.lights = false;
    opts.predictor = 1.0f;
    build(pixels, &params, NULL, &opts, &src, &checked, &plan);
    TEST_ASSERT_NULL(checked.lights);
    TEST_ASSERT_NULL(checked.light_of);
    TEST_ASSERT_NULL(checked.light_features);
    release(&src, &checked, &plan);
    opts.predictor = 0.0f;
    build(pixels, &params, NULL, &opts, &src, &checked, &plan);
    hdrg_plan_free(&g_alloc, &plan);
    hdrg_source_free(&g_alloc, &src);
    build(pixels, &params, NULL, NULL, &src, &rules, &plan);
    TEST_ASSERT_EQUAL_MEMORY(rules.light.data, checked.light.data,
                             (size_t)rules.light.width * rules.light.height * sizeof(float));
    hdrg_analysis_free(&g_alloc, &checked);
    release(&src, &rules, &plan);
    hdrg_analysis_free(&g_alloc, &features);
    free(pixels);
}
#endif

static void test_workers_identical(void)
{
    hdrg_params_t params;
    hdrg_exec_t exec;
    hdrg_source_t src, src2;
    hdrg_analysis_t analysis, analysis2;
    hdrg_plan_t plan, plan2;
    uint8_t *pixels;
    size_t cells;

    HDRG_TEST_REQUIRE_THREADS();

    pixels = hdrg_test_image(TP_W, TP_H);
    TEST_ASSERT_NOT_NULL(pixels);

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.clarity = 0.15f; /* the clarity base too (the default 0 has none) */

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    build(pixels, &params, NULL, NULL, &src, &analysis, &plan);
    build(pixels, &params, &exec, NULL, &src2, &analysis2, &plan2);

    /* the boost grid, the APL statistics and the clarity base are one block */
    cells = (size_t)plan.grid_w * plan.grid_h;
    TEST_ASSERT_NOT_NULL(plan.base);
    TEST_ASSERT_EQUAL_MEMORY(plan.boost, plan2.boost,
                             cells * (HDRG_PLAN_BINS + 4u) * sizeof(float));

    release(&src, &analysis, &plan);
    release(&src2, &analysis2, &plan2);
    hdrg_pool_destroy(exec.pool);
    free(pixels);
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
    RUN_TEST(test_glow_is_light);
    RUN_TEST(test_sharp_is_diffuse);
    RUN_TEST(test_clipped_spot);
    RUN_TEST(test_no_leak);
    RUN_TEST(test_fade_keeps_light);
    RUN_TEST(test_threshold_ramp);
    RUN_TEST(test_glint_by_bank);
    RUN_TEST(test_colored_lights);
    RUN_TEST(test_colored_sky);
    RUN_TEST(test_colored_surfaces);
    RUN_TEST(test_diffuse_level);
#if HDRG_BUILTIN_PREDICT
    RUN_TEST(test_light_predictor);
#endif
    RUN_TEST(test_workers_identical);

    return UNITY_END();
}
