/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "core/yuv.h"
#include "hdrgrading/foundation/pool.h"

/*
 * Y'CbCr: the H.273 code ranges (video and full, 8 and 10
 * bits), neutrals on the chroma midpoint, flat pictures stay flat at odd
 * sizes of every layout, R'G'B' -> 4:2:0 -> R'G'B' within the quantization
 * on linear ramps (the filters' sitings agree), bands converted apart equal
 * one call, and the worker count does not change the result. Every H.273
 * chroma location puts its sites where it says, a picture converted in
 * strips with some rows of context equals one call (the JPEG decoder's way),
 * the JFIF chroma center is neutral, and Cb and Cr in one plane or the
 * samples in the high bits read and write the same codes.
 */
#define TY_W        37u
#define TY_H        23u
#define TY_RAMP_TOL 128 /* 10-bit video range, BT.709: half a step of Y' and C, in 16-bit codes */
#define TY_TALL     300u
#define TY_LONG     700u /* past two of the conversion's bands */
#define TY_DISC     64u
#define TY_SITE_N   32u /* the sitings' pictures */
#define TY_STRIP    32u /* rows a strip emits ... */
#define TY_MARGIN   8u  /* ... with this context on each side */

static hdrg_allocator_t g_alloc;

/* a picture of the format with its planes allocated (16-bit samples above 8 bits) */
static void yuv_alloc(uint8_t layout, uint8_t matrix, uint8_t bits, bool full_range, uint32_t width,
                      uint32_t height, hdrg_yuv_t *yuv)
{
    const uint32_t cw = (layout == HDRG_YUV_444) ? width : (width + 1u) / 2u,
                   ch = (layout == HDRG_YUV_420) ? (height + 1u) / 2u : height;
    const size_t bytes = (bits > 8u) ? 2u : 1u;
    uint8_t p;

    memset(yuv, 0, sizeof(*yuv));
    yuv->width = width;
    yuv->height = height;
    yuv->layout = layout;
    yuv->matrix = matrix;
    yuv->bits = bits;
    yuv->full_range = full_range;
    yuv->strides[0] = (size_t)width * bytes;
    yuv->strides[1] = (size_t)cw * bytes;
    yuv->strides[2] = yuv->strides[1];
    yuv->planes[0] = calloc((size_t)height, yuv->strides[0]);
    yuv->planes[1] = calloc((size_t)ch, yuv->strides[1]);
    yuv->planes[2] = calloc((size_t)ch, yuv->strides[2]);

    for (p = 0; p < 3u; p++) {
        TEST_ASSERT_NOT_NULL(yuv->planes[p]);
    }

    TEST_ASSERT_TRUE(hdrg_yuv_valid(yuv) && hdrg_yuv_fits(yuv));
}

static void yuv_free(hdrg_yuv_t *yuv)
{
    uint8_t p;

    for (p = 0; p < 3u; p++) {
        free((void *)(uintptr_t)yuv->planes[p]);
    }
}

/* R'G'B' with the chroma from luma (its scratch allocated here) */
static void to_rgb16(const hdrg_exec_t *exec, const hdrg_yuv_t *yuv, uint16_t *rgb)
{
    float *scratch;

    scratch = (float *)malloc(HDRG_TEST_MAX(hdrg_yuv_scratch(yuv), (size_t)1) * sizeof(float));
    TEST_ASSERT_NOT_NULL(scratch);

    hdrg_yuv_to_rgb16(exec, yuv, scratch, rgb, (size_t)yuv->width * 6u);

    free(scratch);
}

static uint16_t sample(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t x, uint32_t y)
{
    const uint8_t *row = (const uint8_t *)yuv->planes[plane] + (size_t)y * yuv->strides[plane];
    uint16_t v;

    if (yuv->bits == 8) {
        return row[x];
    }

    memcpy(&v, row + (size_t)x * 2u, sizeof(v));

    return v;
}

static void fill(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t rows, uint16_t value)
{
    uint8_t *data = (uint8_t *)(uintptr_t)yuv->planes[plane];
    size_t i;

    for (i = 0; i < yuv->strides[plane] * rows; i += (yuv->bits > 8u) ? 2u : 1u) {
        if (yuv->bits == 8) {
            data[i] = (uint8_t)value;
        } else {
            memcpy(data + i, &value, sizeof(value));
        }
    }
}

/* a w x h R'G'B' picture of one color */
static uint16_t *flat_rgb(uint32_t width, uint32_t height, uint16_t r, uint16_t g, uint16_t b)
{
    uint16_t *rgb;
    size_t i;

    rgb = (uint16_t *)malloc((size_t)width * height * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(rgb);

    for (i = 0; i < (size_t)width * height; i++) {
        rgb[i * 3u] = r;
        rgb[i * 3u + 1u] = g;
        rgb[i * 3u + 2u] = b;
    }

    return rgb;
}

/* the codes of black and white, and the chroma midpoint, of a 4:2:0 picture */
static void check_range(uint8_t bits, bool full_range, uint16_t black, uint16_t white, uint16_t mid)
{
    hdrg_yuv_t yuv;
    uint16_t *dark, *light, *back;

    dark = flat_rgb(2, 2, 0, 0, 0);
    light = flat_rgb(2, 2, 65535, 65535, 65535);
    back = flat_rgb(2, 2, 1, 1, 1);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT2020, bits, full_range, 2, 2, &yuv);

    hdrg_rgb16_to_yuv420(NULL, dark, 0, 2, &yuv);
    TEST_ASSERT_EQUAL_UINT16(black, sample(&yuv, 0, 1, 1));
    TEST_ASSERT_EQUAL_UINT16(mid, sample(&yuv, 1, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(mid, sample(&yuv, 2, 0, 0));
    to_rgb16(NULL, &yuv, back);
    TEST_ASSERT_EQUAL_MEMORY(dark, back, 12u * sizeof(uint16_t));

    hdrg_rgb16_to_yuv420(NULL, light, 0, 2, &yuv);
    TEST_ASSERT_EQUAL_UINT16(white, sample(&yuv, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(mid, sample(&yuv, 1, 0, 0));
    to_rgb16(NULL, &yuv, back);
    TEST_ASSERT_EQUAL_MEMORY(light, back, 12u * sizeof(uint16_t));

    yuv_free(&yuv);
    free(dark);
    free(light);
    free(back);
}

static void test_ranges(void)
{
    check_range(10, false, 64, 940, 512);
    check_range(10, true, 0, 1023, 512);
    check_range(8, false, 16, 235, 128);
    check_range(12, false, 256, 3760, 2048);
}

/* grays of any level sit on the chroma midpoint, for every matrix */
static void test_neutral(void)
{
    static const uint8_t matrices[3] = {HDRG_MATRIX_BT709, HDRG_MATRIX_BT601, HDRG_MATRIX_BT2020};
    hdrg_yuv_t yuv;
    uint32_t level, x, y;
    uint16_t *rgb;
    uint8_t m;

    for (m = 0; m < 3u; m++) {
        yuv_alloc(HDRG_YUV_420, matrices[m], 10, false, 4, 2, &yuv);

        for (level = 0; level <= 65535u; level += 4369u) {
            rgb = flat_rgb(4, 2, (uint16_t)level, (uint16_t)level, (uint16_t)level);
            hdrg_rgb16_to_yuv420(NULL, rgb, 0, 2, &yuv);

            for (x = 0; x < 2u; x++) {
                TEST_ASSERT_EQUAL_UINT16(512, sample(&yuv, 1, x, 0));
                TEST_ASSERT_EQUAL_UINT16(512, sample(&yuv, 2, x, 0));
            }

            for (y = 0; y < 2u; y++) {
                TEST_ASSERT_EQUAL_UINT16(sample(&yuv, 0, 0, 0), sample(&yuv, 0, 3, y));
            }

            free(rgb);
        }

        yuv_free(&yuv);
    }
}

/* flat planes give one color everywhere, at odd sizes of every layout; its value is H.273's */
static void test_flat_odd(void)
{
    static const uint8_t layouts[3] = {HDRG_YUV_420, HDRG_YUV_422, HDRG_YUV_444};
    const double luma = (500.0 - 64.0) / 876.0, cb = (300.0 - 512.0) / 896.0,
                 cr = (700.0 - 512.0) / 896.0, kr = 0.2126, kb = 0.0722, kg = 1.0 - kr - kb,
                 want[3] = {luma + 2.0 * (1.0 - kr) * cr,
                            luma - (2.0 * kb * (1.0 - kb) * cb + 2.0 * kr * (1.0 - kr) * cr) / kg,
                            luma + 2.0 * (1.0 - kb) * cb};
    hdrg_yuv_t yuv;
    uint16_t *rgb;
    uint8_t l, c;
    size_t i;

    rgb = (uint16_t *)malloc((size_t)7u * 5u * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(rgb);

    for (l = 0; l < 3u; l++) {
        yuv_alloc(layouts[l], HDRG_MATRIX_BT709, 10, false, 7, 5, &yuv);
        fill(&yuv, 0, 5, 500);
        fill(&yuv, 1, (layouts[l] == HDRG_YUV_420) ? 3u : 5u, 300);
        fill(&yuv, 2, (layouts[l] == HDRG_YUV_420) ? 3u : 5u, 700);

        to_rgb16(NULL, &yuv, rgb);

        for (c = 0; c < 3u; c++) {
            TEST_ASSERT_TRUE(fabs((double)rgb[c] - want[c] * 65535.0) <= 1.0);
        }

        for (i = 3; i < (size_t)7u * 5u * 3u; i++) {
            TEST_ASSERT_EQUAL_UINT16(rgb[i % 3u], rgb[i]);
        }

        yuv_free(&yuv);
    }

    free(rgb);
}

/* linear ramps through 4:2:0 and back: the filters interpolate them exactly inside the border */
static void test_ramp_roundtrip(void)
{
    hdrg_yuv_t yuv;
    uint32_t x, y;
    uint16_t *rgb, *back;
    uint8_t c;
    int32_t worst, d;
    size_t i;

    rgb = flat_rgb(TY_W, TY_H, 0, 0, 0);
    back = flat_rgb(TY_W, TY_H, 0, 0, 0);

    for (y = 0; y < TY_H; y++) {
        for (x = 0; x < TY_W; x++) {
            i = ((size_t)y * TY_W + x) * 3u;
            rgb[i] = (uint16_t)(65535u * x / (TY_W - 1u));
            rgb[i + 1u] = (uint16_t)(65535u * y / (TY_H - 1u));
            rgb[i + 2u] = (uint16_t)(65535u - 65535u * (x + y) / (TY_W + TY_H - 2u));
        }
    }

    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_W, TY_H, &yuv);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_H, &yuv);
    to_rgb16(NULL, &yuv, back);

    worst = 0;
    for (y = 2; y < TY_H - 2u; y++) {
        for (x = 2; x < TY_W - 2u; x++) {
            for (c = 0; c < 3u; c++) {
                i = ((size_t)y * TY_W + x) * 3u + c;
                d = (int32_t)back[i] - (int32_t)rgb[i];
                worst = (d < 0) ? HDRG_TEST_MAX(worst, -d) : HDRG_TEST_MAX(worst, d);
            }
        }
    }

    TEST_ASSERT_TRUE(worst <= TY_RAMP_TOL);

    yuv_free(&yuv);
    free(rgb);
    free(back);
}

/* a pseudo-random R'G'B' picture */
static uint16_t *noise_rgb(uint32_t width, uint32_t height)
{
    uint32_t state;
    uint16_t *rgb;
    size_t i;

    rgb = flat_rgb(width, height, 0, 0, 0);
    state = 12345u;

    for (i = 0; i < (size_t)width * height * 3u; i++) {
        state = state * 1664525u + 1013904223u;
        rgb[i] = (uint16_t)(state >> 16);
    }

    return rgb;
}

/* bands of even starts converted one at a time write what one call writes */
static void test_bands(void)
{
    hdrg_yuv_t whole, parts;
    uint16_t *rgb;
    uint8_t p;

    rgb = noise_rgb(TY_W, TY_H);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, false, TY_W, TY_H, &whole);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, false, TY_W, TY_H, &parts);

    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_H, &whole);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, 8, &parts);
    hdrg_rgb16_to_yuv420(NULL, rgb + (size_t)8u * TY_W * 3u, 8, TY_H - 8u, &parts);

    TEST_ASSERT_EQUAL_MEMORY(whole.planes[0], parts.planes[0], whole.strides[0] * TY_H);

    for (p = 1; p < 3u; p++) {
        TEST_ASSERT_EQUAL_MEMORY(whole.planes[p], parts.planes[p],
                                 whole.strides[p] * ((TY_H + 1u) / 2u));
    }

    yuv_free(&whole);
    yuv_free(&parts);
    free(rgb);
}

/* the conversions do not depend on the worker count */
static void test_workers_identical(void)
{
    hdrg_exec_t exec;
    hdrg_yuv_t one, many;
    uint16_t *rgb, *back_one, *back_many;
    uint8_t p;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    rgb = noise_rgb(TY_W, TY_TALL);
    back_one = flat_rgb(TY_W, TY_TALL, 0, 0, 0);
    back_many = flat_rgb(TY_W, TY_TALL, 0, 0, 0);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_W, TY_TALL, &one);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_W, TY_TALL, &many);

    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_TALL, &one);
    hdrg_rgb16_to_yuv420(&exec, rgb, 0, TY_TALL, &many);

    for (p = 0; p < 3u; p++) {
        TEST_ASSERT_EQUAL_MEMORY(one.planes[p], many.planes[p],
                                 one.strides[p] * ((p == 0) ? TY_TALL : TY_TALL / 2u));
    }

    to_rgb16(NULL, &one, back_one);
    to_rgb16(&exec, &one, back_many);
    TEST_ASSERT_EQUAL_MEMORY(back_one, back_many, (size_t)TY_W * TY_TALL * 3u * sizeof(uint16_t));

    one.fit = HDRG_CHROMA_FIT_SEGMENTED;
    to_rgb16(NULL, &one, back_one);
    to_rgb16(&exec, &one, back_many);
    TEST_ASSERT_EQUAL_MEMORY(back_one, back_many, (size_t)TY_W * TY_TALL * 3u * sizeof(uint16_t));

    yuv_free(&one);
    yuv_free(&many);
    free(rgb);
    free(back_one);
    free(back_many);
    hdrg_pool_destroy(exec.pool);
}

/* formats the conversions do not take, and planes that do not fit */
/* a disc of (r, g, b) on gray 0.5 (fractions of the full scale) */
static uint16_t *disc_rgb(double r, double g, double b)
{
    uint32_t x, y;
    uint16_t *rgb;
    size_t i;
    double dx, dy;

    rgb = flat_rgb(TY_DISC, TY_DISC, 32768, 32768, 32768);

    for (y = 0; y < TY_DISC; y++) {
        for (x = 0; x < TY_DISC; x++) {
            dx = (double)x - 31.3;
            dy = (double)y - 30.7;

            if (dx * dx + dy * dy < 17.0 * 17.0) {
                i = ((size_t)y * TY_DISC + x) * 3u;
                rgb[i] = (uint16_t)(r * 65535.0 + 0.5);
                rgb[i + 1u] = (uint16_t)(g * 65535.0 + 0.5);
                rgb[i + 2u] = (uint16_t)(b * 65535.0 + 0.5);
            }
        }
    }

    return rgb;
}

/* squared R'G'B' error of the 4:2:0 round trip, chroma from luma (fit) or bilinear (no scratch) */
static double disc_error(const uint16_t *rgb, bool cfl, uint8_t fit)
{
    hdrg_yuv_t yuv;
    uint16_t *back;
    size_t i;
    double d, sum;

    back = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_DISC, TY_DISC, &yuv);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_DISC, &yuv);
    yuv.fit = fit;

    if (cfl) {
        to_rgb16(NULL, &yuv, back);
    } else {
        hdrg_yuv_to_rgb16(NULL, &yuv, NULL, back, (size_t)yuv.width * 6u);
    }

    sum = 0.0;

    for (i = 0; i < (size_t)TY_DISC * TY_DISC * 3u; i++) {
        d = (double)back[i] - (double)rgb[i];
        sum += d * d;
    }

    yuv_free(&yuv);
    free(back);

    return sum;
}

/*
 * chroma from luma (6.6e): a colored disc (its chroma edge on the luma
 * edge) comes back closer than with the bilinear chroma; an isoluminant
 * disc (a chroma edge without luma) is no worse; a neutral picture keeps
 * the bilinear chroma exactly
 */
static void test_chroma_from_luma(void)
{
    hdrg_yuv_t yuv;
    uint16_t *rgb, *cfl, *bilinear;
    double with, without;

    rgb = disc_rgb(0.9, 0.5, 0.1);
    with = disc_error(rgb, true, HDRG_CHROMA_FIT_LINEAR);
    without = disc_error(rgb, false, HDRG_CHROMA_FIT_LINEAR);
    printf("orange disc: squared error %.3g (chroma from luma) / %.3g (bilinear)\n", with, without);
    TEST_ASSERT_TRUE(with < 0.7 * without);
    free(rgb);

    /* luma 0.5 like the gray: 0.2126 r + 0.7152 g + 0.0722 b = 0.5 */
    rgb = disc_rgb(0.7, 0.45, (0.5 - 0.2126 * 0.7 - 0.7152 * 0.45) / 0.0722);
    with = disc_error(rgb, true, HDRG_CHROMA_FIT_LINEAR);
    without = disc_error(rgb, false, HDRG_CHROMA_FIT_LINEAR);
    printf("isoluminant disc: squared error %.3g / %.3g\n", with, without);
    TEST_ASSERT_TRUE(with <= 1.02 * without);
    free(rgb);

    rgb = disc_rgb(0.8, 0.8, 0.8);
    cfl = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);
    bilinear = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_DISC, TY_DISC, &yuv);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_DISC, &yuv);
    to_rgb16(NULL, &yuv, cfl);
    hdrg_yuv_to_rgb16(NULL, &yuv, NULL, bilinear, (size_t)yuv.width * 6u);
    TEST_ASSERT_EQUAL_MEMORY(bilinear, cfl, (size_t)TY_DISC * TY_DISC * 3u * sizeof(uint16_t));

    yuv_free(&yuv);
    free(rgb);
    free(cfl);
    free(bilinear);
}

/*
 * An outline between two fills: skin on the left, blue on the right, a
 * dark line 2.5 pixels wide along x = 24 + 0.3 y between them (4 x 4
 * supersampled)
 */
static uint16_t *outline_rgb(void)
{
    static const double skin[3] = {0.98, 0.88, 0.80}, blue[3] = {0.20, 0.45, 0.75},
                        line[3] = {0.08, 0.06, 0.06};
    uint32_t x, y, sx, sy;
    uint16_t *rgb;
    uint8_t c;
    size_t i;
    double px, py, d, acc[3];

    rgb = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);

    for (y = 0; y < TY_DISC; y++) {
        for (x = 0; x < TY_DISC; x++) {
            acc[0] = 0.0;
            acc[1] = 0.0;
            acc[2] = 0.0;

            for (sy = 0; sy < 4u; sy++) {
                for (sx = 0; sx < 4u; sx++) {
                    px = (double)x + ((double)sx + 0.5) / 4.0;
                    py = (double)y + ((double)sy + 0.5) / 4.0;
                    d = px - (24.0 + 0.3 * py);

                    for (c = 0; c < 3u; c++) {
                        acc[c] += (d < -1.25) ? skin[c] : ((d > 1.25) ? blue[c] : line[c]);
                    }
                }
            }

            i = ((size_t)y * TY_DISC + x) * 3u;

            for (c = 0; c < 3u; c++) {
                rgb[i + c] = (uint16_t)(acc[c] / 16.0 * 65535.0 + 0.5);
            }
        }
    }

    return rgb;
}

/*
 * The segmented fit: an outline between two fills (three regions in a
 * window, no straight line through them) comes back closer than with the
 * linear fit; a disc on gray (two regions) is no worse; a neutral picture
 * leaves nothing to bend and keeps the bilinear chroma exactly
 */
static void test_segmented(void)
{
    hdrg_yuv_t yuv;
    uint16_t *rgb, *bent, *bilinear;
    double line, segmented;

    rgb = outline_rgb();
    line = disc_error(rgb, true, HDRG_CHROMA_FIT_LINEAR);
    segmented = disc_error(rgb, true, HDRG_CHROMA_FIT_SEGMENTED);
    printf("outline: squared error %.3g (segmented) / %.3g (line)\n", segmented, line);
    TEST_ASSERT_TRUE(segmented < line);
    free(rgb);

    rgb = disc_rgb(0.9, 0.5, 0.1);
    line = disc_error(rgb, true, HDRG_CHROMA_FIT_LINEAR);
    segmented = disc_error(rgb, true, HDRG_CHROMA_FIT_SEGMENTED);
    printf("orange disc: squared error %.3g (segmented) / %.3g (line)\n", segmented, line);
    TEST_ASSERT_TRUE(segmented <= 1.02 * line);
    free(rgb);

    rgb = disc_rgb(0.8, 0.8, 0.8);
    bent = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);
    bilinear = flat_rgb(TY_DISC, TY_DISC, 0, 0, 0);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT709, 10, false, TY_DISC, TY_DISC, &yuv);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_DISC, &yuv);
    yuv.fit = HDRG_CHROMA_FIT_SEGMENTED;
    to_rgb16(NULL, &yuv, bent);
    hdrg_yuv_to_rgb16(NULL, &yuv, NULL, bilinear, (size_t)yuv.width * 6u);
    TEST_ASSERT_EQUAL_MEMORY(bilinear, bent, (size_t)TY_DISC * TY_DISC * 3u * sizeof(uint16_t));

    yuv_free(&yuv);
    free(rgb);
    free(bent);
    free(bilinear);
}

static void put16(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t x, uint32_t y, uint16_t v)
{
    memcpy((uint8_t *)(uintptr_t)yuv->planes[plane] + (size_t)y * yuv->strides[plane] +
               (size_t)x * 2u,
           &v, sizeof(v));
}

/*
 * Each chroma location (4:2:0 and 4:2:2, 10-bit full range): Cb and Cr
 * linear in the position of their sites over flat luma come back linear at
 * every luma pixel off the borders, at the pixel's own position. A site put
 * half a luma pixel off would miss by 2 codes
 */
static void test_sitings(void)
{
    const uint32_t n = TY_SITE_N;
    static const uint8_t layouts[2] = {HDRG_YUV_420, HDRG_YUV_422};
    hdrg_yuv_t yuv;
    uint32_t x, y, j, k, cw, ch;
    uint16_t *rgb;
    uint8_t l, loc;
    double sx, sy, r, g, b, luma, cb, cr;

    rgb = flat_rgb(n, n, 0, 0, 0);

    for (l = 0; l < 2u; l++) {
        for (loc = 0; loc <= HDRG_CHROMA_BOTTOM; loc++) {
            yuv_alloc(layouts[l], HDRG_MATRIX_BT709, 10, true, n, n, &yuv);
            yuv.siting = loc;
            TEST_ASSERT_TRUE(hdrg_yuv_valid(&yuv));
            fill(&yuv, 0, n, 512);
            cw = n / 2u;
            ch = (layouts[l] == HDRG_YUV_420) ? n / 2u : n;

            for (k = 0; k < ch; k++) {
                for (j = 0; j < cw; j++) {
                    sx = 2.0 * j + ((loc % 2u == 1u) ? 0.5 : 0.0);
                    sy = (layouts[l] == HDRG_YUV_422)  ? (double)k
                         : (loc <= HDRG_CHROMA_CENTER) ? 2.0 * k + 0.5
                         : (loc <= HDRG_CHROMA_TOP)    ? 2.0 * k
                                                       : 2.0 * k + 1.0;
                    put16(&yuv, 1, j, k, (uint16_t)(512.0 + 4.0 * sx + 2.0 * sy));
                    put16(&yuv, 2, j, k, (uint16_t)(512.0 - 2.0 * sx + 2.0 * sy));
                }
            }

            to_rgb16(NULL, &yuv, rgb);

            for (y = 2; y + 2u < n; y++) {
                for (x = 2; x + 2u < n; x++) {
                    r = rgb[((size_t)y * n + x) * 3u] / 65535.0;
                    g = rgb[((size_t)y * n + x) * 3u + 1u] / 65535.0;
                    b = rgb[((size_t)y * n + x) * 3u + 2u] / 65535.0;
                    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    cb = 512.0 + 1023.0 * (b - luma) / 1.8556;
                    cr = 512.0 + 1023.0 * (r - luma) / 1.5748;

                    HDRG_TEST_NEAR(0.1, 512.0 + 4.0 * x + 2.0 * y, cb);
                    HDRG_TEST_NEAR(0.1, 512.0 - 2.0 * x + 2.0 * y, cr);
                }
            }

            yuv_free(&yuv);
        }
    }

    free(rgb);
}

/*
 * Rows [first, top) of a picture taken as a picture of their own, the rows
 * of each strip TY_MARGIN or more from its cut edges kept: the whole call's
 * bytes for 4:2:0 / 4:2:2 at the JPEG, the top-left and the MPEG-2 sitings,
 * with either fit
 */
static void test_strips(void)
{
    static const uint8_t layouts[2] = {HDRG_YUV_420, HDRG_YUV_422},
                         sitings[3] = {HDRG_CHROMA_CENTER, HDRG_CHROMA_TOPLEFT, HDRG_CHROMA_LEFT};
    const size_t row = (size_t)TY_W * 3u;
    hdrg_yuv_t whole, strip;
    uint32_t e, first, top, vf, end;
    uint16_t *src, *rgb, *part;
    uint8_t l, s, p, f;

    src = noise_rgb(TY_W, TY_TALL);
    rgb = flat_rgb(TY_W, TY_TALL, 0, 0, 0);
    part = flat_rgb(TY_W, TY_TALL, 0, 0, 0);

    for (l = 0; l < 4u; l++) {
        for (s = 0; s < 3u; s++) {
            f = (l >= 2u) ? HDRG_CHROMA_FIT_SEGMENTED : HDRG_CHROMA_FIT_LINEAR;
            yuv_alloc(layouts[l % 2u], HDRG_MATRIX_BT601, 16, true, TY_W, TY_TALL, &whole);

            /* noisy planes with a structure the chroma from luma follows */
            for (p = 0; p < 3u; p++) {
                memcpy((void *)(uintptr_t)whole.planes[p], src,
                       whole.strides[p] * ((p == 0 || layouts[l % 2u] == HDRG_YUV_422)
                                               ? TY_TALL
                                               : (TY_TALL + 1u) / 2u));
            }

            whole.siting = sitings[s];
            whole.fit = f;
            whole.jfif = sitings[s] == HDRG_CHROMA_CENTER;
            to_rgb16(NULL, &whole, rgb);
            vf = (layouts[l % 2u] == HDRG_YUV_420) ? 2u : 1u;

            for (e = 0; e < TY_TALL; e += TY_STRIP) {
                first = (e > TY_MARGIN) ? e - TY_MARGIN : 0u;
                top = (e + TY_STRIP + TY_MARGIN < TY_TALL) ? e + TY_STRIP + TY_MARGIN : TY_TALL;
                end = (e + TY_STRIP < TY_TALL) ? e + TY_STRIP : TY_TALL;
                strip = whole;
                strip.height = top - first;

                for (p = 0; p < 3u; p++) {
                    strip.planes[p] = (const uint8_t *)whole.planes[p] +
                                      (size_t)((p == 0) ? first : first / vf) * whole.strides[p];
                }

                to_rgb16(NULL, &strip, part);

                TEST_ASSERT_EQUAL_MEMORY(rgb + (size_t)e * row, part + (size_t)(e - first) * row,
                                         (size_t)(end - e) * row * sizeof(uint16_t));
            }

            yuv_free(&whole);
        }
    }

    free(src);
    free(rgb);
    free(part);
}

/*
 * A picture taller than the conversion's bands (it converts them apart, with
 * their own context): every row as a short picture around it gives, at every
 * siting, for 4:2:0 / 4:2:2 and either fit
 */
static void test_bands_tall(void)
{
    static const uint8_t layouts[2] = {HDRG_YUV_420, HDRG_YUV_422},
                         sitings[6] = {HDRG_CHROMA_LEFT,       HDRG_CHROMA_CENTER,
                                       HDRG_CHROMA_TOPLEFT,    HDRG_CHROMA_TOP,
                                       HDRG_CHROMA_BOTTOMLEFT, HDRG_CHROMA_BOTTOM};
    const size_t row = (size_t)TY_W * 3u;
    hdrg_yuv_t whole, strip;
    uint32_t e, first, top, vf, end;
    uint16_t *src, *rgb, *part;
    uint8_t l, s, p, f;

    src = noise_rgb(TY_W, TY_LONG);
    rgb = flat_rgb(TY_W, TY_LONG, 0, 0, 0);
    part = flat_rgb(TY_W, TY_LONG, 0, 0, 0);

    for (l = 0; l < 4u; l++) {
        for (s = 0; s < 6u; s++) {
            f = (l >= 2u) ? HDRG_CHROMA_FIT_SEGMENTED : HDRG_CHROMA_FIT_LINEAR;
            yuv_alloc(layouts[l % 2u], HDRG_MATRIX_BT601, 16, true, TY_W, TY_LONG, &whole);

            /* noisy planes with a structure the chroma from luma follows */
            for (p = 0; p < 3u; p++) {
                memcpy((void *)(uintptr_t)whole.planes[p], src,
                       whole.strides[p] * ((p == 0 || layouts[l % 2u] == HDRG_YUV_422)
                                               ? TY_LONG
                                               : (TY_LONG + 1u) / 2u));
            }

            whole.siting = sitings[s];
            whole.fit = f;
            whole.jfif = sitings[s] == HDRG_CHROMA_CENTER;
            to_rgb16(NULL, &whole, rgb);
            vf = (layouts[l % 2u] == HDRG_YUV_420) ? 2u : 1u;

            for (e = 0; e < TY_LONG; e += TY_STRIP) {
                first = (e > TY_MARGIN) ? e - TY_MARGIN : 0u;
                top = (e + TY_STRIP + TY_MARGIN < TY_LONG) ? e + TY_STRIP + TY_MARGIN : TY_LONG;
                end = (e + TY_STRIP < TY_LONG) ? e + TY_STRIP : TY_LONG;
                strip = whole;
                strip.height = top - first;

                for (p = 0; p < 3u; p++) {
                    strip.planes[p] = (const uint8_t *)whole.planes[p] +
                                      (size_t)((p == 0) ? first : first / vf) * whole.strides[p];
                }

                to_rgb16(NULL, &strip, part);

                TEST_ASSERT_EQUAL_MEMORY(rgb + (size_t)e * row, part + (size_t)(e - first) * row,
                                         (size_t)(end - e) * row * sizeof(uint16_t));
            }

            yuv_free(&whole);
        }
    }

    free(src);
    free(rgb);
    free(part);
}

/* JFIF: the chroma on 128 / 255 of the top is neutral at 16 bits, as 128 at 8 */
static void test_jfif(void)
{
    hdrg_yuv_t yuv;
    uint16_t *rgb;
    size_t i;

    rgb = flat_rgb(TY_W, TY_H, 0, 0, 0);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT601, 16, true, TY_W, TY_H, &yuv);
    yuv.siting = HDRG_CHROMA_CENTER;
    yuv.jfif = true;
    fill(&yuv, 0, TY_H, 40000);
    fill(&yuv, 1, (TY_H + 1u) / 2u, 32896);
    fill(&yuv, 2, (TY_H + 1u) / 2u, 32896);
    to_rgb16(NULL, &yuv, rgb);

    for (i = 0; i < (size_t)TY_W * TY_H; i++) {
        TEST_ASSERT_EQUAL_UINT16(40000, rgb[i * 3u]);
        TEST_ASSERT_EQUAL_UINT16(40000, rgb[i * 3u + 1u]);
        TEST_ASSERT_EQUAL_UINT16(40000, rgb[i * 3u + 2u]);
    }

    yuv_free(&yuv);
    free(rgb);
}

/*
 * planar's samples as a picture with Cb and Cr alternating in planes[1]
 * (interleaved) and, above 8 bits, in the samples' high bits (msb), its
 * planes allocated here
 */
static void yuv_pack(const hdrg_yuv_t *planar, bool interleaved, bool msb, hdrg_yuv_t *packed)
{
    const uint32_t cw =
                       (planar->layout == HDRG_YUV_444) ? planar->width : (planar->width + 1u) / 2u,
                   ch = (planar->layout == HDRG_YUV_420) ? (planar->height + 1u) / 2u
                                                         : planar->height;
    const uint8_t shift = msb ? (uint8_t)(16u - planar->bits) : 0u;
    const size_t bytes = (planar->bits > 8u) ? 2u : 1u;
    uint32_t x, y, n;
    uint16_t v;
    uint8_t p, *row;

    *packed = *planar;
    packed->interleaved = interleaved;
    packed->msb = msb;
    packed->strides[1] = (size_t)cw * bytes * (interleaved ? 2u : 1u);
    packed->strides[2] = interleaved ? 0u : packed->strides[1];
    packed->planes[0] = calloc((size_t)planar->height, packed->strides[0]);
    packed->planes[1] = calloc((size_t)ch, packed->strides[1]);
    packed->planes[2] = interleaved ? NULL : calloc((size_t)ch, packed->strides[2]);
    TEST_ASSERT_NOT_NULL(packed->planes[0]);
    TEST_ASSERT_NOT_NULL(packed->planes[1]);

    for (p = 0; p < 3u; p++) {
        n = (p == 0u) ? planar->width : cw;

        for (y = 0; y < ((p == 0u) ? planar->height : ch); y++) {
            row = (uint8_t *)(uintptr_t)packed->planes[(interleaved && p != 0u) ? 1u : p] +
                  (size_t)y * packed->strides[(interleaved && p != 0u) ? 1u : p];

            for (x = 0; x < n; x++) {
                v = (uint16_t)(sample(planar, p, x, y) << shift);

                if (bytes == 1u) {
                    row[(interleaved && p != 0u) ? 2u * x + p - 1u : x] = (uint8_t)v;
                } else {
                    memcpy(row + ((interleaved && p != 0u) ? 2u * x + p - 1u : x) * 2u, &v,
                           sizeof(v));
                }
            }
        }
    }

    TEST_ASSERT_TRUE(hdrg_yuv_valid(packed) && hdrg_yuv_fits(packed));
}

/* planar pseudo-random samples of each plane */
static void yuv_noise(const hdrg_yuv_t *yuv)
{
    const uint32_t cw = (yuv->layout == HDRG_YUV_444) ? yuv->width : (yuv->width + 1u) / 2u,
                   ch = (yuv->layout == HDRG_YUV_420) ? (yuv->height + 1u) / 2u : yuv->height;
    uint32_t state, x, y;
    uint16_t v;
    uint8_t p;

    state = 777u;

    for (p = 0; p < 3u; p++) {
        for (y = 0; y < ((p == 0u) ? yuv->height : ch); y++) {
            for (x = 0; x < ((p == 0u) ? yuv->width : cw); x++) {
                state = state * 1664525u + 1013904223u;
                v = (uint16_t)((state >> 8) & ((1u << yuv->bits) - 1u));

                if (yuv->bits == 8) {
                    ((uint8_t *)(uintptr_t)yuv->planes[p])[(size_t)y * yuv->strides[p] + x] =
                        (uint8_t)v;
                } else {
                    put16(yuv, p, x, y, v);
                }
            }
        }
    }
}

/*
 * Cb and Cr alternating in one plane (NV12, P010) and the samples in the
 * high bits (P010) read and write the same codes as a plane each in the low
 * bits: the R'G'B' of each layout and fit, the 4:2:0 output
 */
static void test_packed(void)
{
    static const uint8_t layouts[3] = {HDRG_YUV_420, HDRG_YUV_422, HDRG_YUV_444},
                         depths[3] = {8, 10, 12};
    hdrg_yuv_t planar, packed, out_planar, out_packed;
    uint32_t x, y;
    uint16_t *rgb, *ref, *got;
    uint8_t l, d, f, m, r, p;
    size_t size;

    size = (size_t)TY_W * TY_H * 3u * sizeof(uint16_t);
    ref = (uint16_t *)malloc(size);
    got = (uint16_t *)malloc(size);
    TEST_ASSERT_NOT_NULL(ref);
    TEST_ASSERT_NOT_NULL(got);

    for (l = 0; l < 3u; l++) {
        for (d = 0; d < 3u; d++) {
            for (r = 0; r < 2u; r++) {
                yuv_alloc(layouts[l], HDRG_MATRIX_BT709, depths[d], r == 1u, TY_W, TY_H, &planar);
                yuv_noise(&planar);

                for (f = HDRG_CHROMA_FIT_LINEAR; f <= HDRG_CHROMA_FIT_SEGMENTED; f++) {
                    planar.fit = f;
                    to_rgb16(NULL, &planar, ref);

                    for (m = 0; m < ((depths[d] > 8u) ? 3u : 1u); m++) {
                        yuv_pack(&planar, m != 1u, m != 0u, &packed);
                        to_rgb16(NULL, &packed, got);
                        TEST_ASSERT_EQUAL_MEMORY(ref, got, size);
                        yuv_free(&packed);
                    }
                }

                yuv_free(&planar);
            }
        }
    }

    rgb = noise_rgb(TY_W, TY_H);
    yuv_alloc(HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, false, TY_W, TY_H, &out_planar);
    yuv_pack(&out_planar, true, true, &out_packed);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_H, &out_planar);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TY_H, &out_packed);

    for (p = 0; p < 3u; p++) {
        for (y = 0; y < ((p == 0u) ? TY_H : (TY_H + 1u) / 2u); y++) {
            for (x = 0; x < ((p == 0u) ? TY_W : (TY_W + 1u) / 2u); x++) {
                TEST_ASSERT_EQUAL_UINT16((uint16_t)(sample(&out_planar, p, x, y) << 6),
                                         (p == 0u) ? sample(&out_packed, 0, x, y)
                                                   : sample(&out_packed, 1, 2u * x + p - 1u, y));
            }
        }
    }

    yuv_free(&out_planar);
    yuv_free(&out_packed);
    free(rgb);
    free(ref);
    free(got);
}

static void test_valid(void)
{
    hdrg_yuv_t yuv, bad;

    yuv_alloc(HDRG_YUV_422, HDRG_MATRIX_BT601, 12, true, 5, 3, &yuv);

    bad = yuv;
    bad.layout = 0;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.matrix = 2;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.bits = 17;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.width = 0;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.siting = HDRG_CHROMA_BOTTOM + 1u;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.full_range = false;
    bad.jfif = true;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));
    bad = yuv;
    bad.fit = HDRG_CHROMA_FIT_SEGMENTED + 1u;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));

    bad = yuv;
    bad.bits = 8;
    bad.msb = true;
    TEST_ASSERT_FALSE(hdrg_yuv_valid(&bad));

    bad = yuv;
    bad.planes[2] = NULL;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad.interleaved = true;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad.strides[1] *= 2u;
    TEST_ASSERT_TRUE(hdrg_yuv_fits(&bad));
    bad = yuv;
    bad.strides[1] = 5;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));

    /* each plane missing or its rows too short, planar or interleaved */
    TEST_ASSERT_TRUE(hdrg_yuv_fits(&yuv));
    bad = yuv;
    bad.planes[0] = NULL;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad = yuv;
    bad.planes[1] = NULL;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad = yuv;
    bad.strides[0] -= 1u;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad = yuv;
    bad.strides[2] -= 1u;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad = yuv;
    bad.interleaved = true;
    bad.strides[1] *= 2u;
    TEST_ASSERT_TRUE(hdrg_yuv_fits(&bad));
    bad.planes[0] = NULL;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad.planes[0] = yuv.planes[0];
    bad.planes[1] = NULL;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));
    bad.planes[1] = yuv.planes[1];
    bad.strides[0] -= 1u;
    TEST_ASSERT_FALSE(hdrg_yuv_fits(&bad));

    yuv_free(&yuv);
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
    RUN_TEST(test_ranges);
    RUN_TEST(test_neutral);
    RUN_TEST(test_flat_odd);
    RUN_TEST(test_ramp_roundtrip);
    RUN_TEST(test_bands);
    RUN_TEST(test_workers_identical);
    RUN_TEST(test_chroma_from_luma);
    RUN_TEST(test_segmented);
    RUN_TEST(test_sitings);
    RUN_TEST(test_strips);
    RUN_TEST(test_bands_tall);
    RUN_TEST(test_jfif);
    RUN_TEST(test_packed);
    RUN_TEST(test_valid);

    return UNITY_END();
}
