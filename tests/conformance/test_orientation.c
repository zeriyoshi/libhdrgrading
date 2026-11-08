/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_jpeg.h"
#include "../test_util.h"
#include "io/io.h"

/* Minimal EXIF blob: "Exif\0\0" + little-endian TIFF with orientation = 6. */
static const uint8_t exif_orientation6[] = {
    'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static hdrg_allocator_t g_alloc;
static uint8_t *g_input;
static size_t g_input_size;

/* "Exif\0\0" + a TIFF of one entry, orientation = value, little- ("II") or big-endian ("MM") */
static void exif_of(uint16_t value, bool big, uint8_t out[32])
{
    static const uint8_t ii[32] = {'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',  0x2A, 0x00, 0x08,
                                   0x00, 0x00, 0x00, 0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                         mm[32] = {'E',  'x',  'i',  'f',  0x00, 0x00, 'M',  'M',  0x00, 0x2A, 0x00,
                                   0x00, 0x00, 0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00,
                                   0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    memcpy(out, big ? mm : ii, 32);
    out[24] = big ? (uint8_t)(value >> 8) : (uint8_t)value;
    out[25] = big ? (uint8_t)value : (uint8_t)(value >> 8);
}

/* 64x32, red top-left corner block, dark blue elsewhere */
static uint8_t *picture(void)
{
    uint32_t x, y;
    uint8_t *pixels;

    pixels = (uint8_t *)calloc(1, 64 * 32 * 3);
    TEST_ASSERT_NOT_NULL(pixels);

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 64; x++) {
            pixels[((size_t)y * 64 + x) * 3 + 2] = 60;
        }
    }

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            pixels[((size_t)y * 64 + x) * 3 + 0] = 250;
            pixels[((size_t)y * 64 + x) * 3 + 2] = 0;
        }
    }

    return pixels;
}

static void ensure_input(void)
{
    uint8_t *pixels;

    if (g_input != NULL) {
        return;
    }

    g_alloc = hdrg_test_allocator();
    pixels = picture();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_jpeg_rgb(&g_alloc, pixels, 64, 32, 64 * 3, 95, NULL, 0,
                                                      exif_orientation6, sizeof(exif_orientation6),
                                                      &g_input, &g_input_size));

    free(pixels);
}

static void test_input_carries_orientation6(void)
{
    hdrg_decoded_t decoded;

    ensure_input();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, g_input, g_input_size,
                                                   UINT64_MAX, HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT16(6, decoded.orientation);
    TEST_ASSERT_EQUAL_UINT32(64, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(32, decoded.height);

    hdrg_decoded_free(&g_alloc, &decoded);
}

static void test_output_physically_rotated_and_exif_normalized(void)
{
    hdrg_decoded_t decoded;
    hdrg_context_t *ctx;
    uint8_t *output, red;
    size_t output_size, corner;

    ensure_input();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert(ctx, g_input, g_input_size, NULL, NULL, NULL, &output, &output_size));

    /* base is rotated 90 cw: 32x64, red block top-right, EXIF orientation 1 */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, output, output_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT32(32, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(64, decoded.height);
    TEST_ASSERT_TRUE(decoded.orientation <= 1);

    /* the default output's primary is the SDR base: only the red block's position matters */
    corner = ((size_t)2 * 32 + 29) * 3;
    red = decoded.pixels[corner + 0];
    TEST_ASSERT_TRUE(red > 120);
    TEST_ASSERT_TRUE(decoded.pixels[corner + 2] + 40 < red);

    corner = ((size_t)60 * 32 + 2) * 3;
    TEST_ASSERT_TRUE(decoded.pixels[corner + 0] + 60 < red);

    hdrg_decoded_free(&g_alloc, &decoded);
    hdrg_free(output);
    hdrg_context_destroy(ctx);
}

/*
 * Every EXIF orientation, in either byte order: the output is the picture as shown (the stored
 * top-left red block at the corner the orientation takes it to, the sides swapped for 5-8), its
 * EXIF orientation 1
 */
static void test_every_orientation_normalized(void)
{
    /* the corner the stored top-left shows at: 0 top-left, 1 top-right, 2 bottom-right, 3
     * bottom-left */
    static const uint8_t corner_of[9] = {0, 0, 1, 2, 3, 0, 1, 2, 3};
    hdrg_decoded_t decoded;
    hdrg_context_t *ctx;
    uint32_t w, h, xs[4], ys[4];
    uint16_t value;
    uint8_t exif[32], *pixels, *input, *output, k, big;
    size_t input_size, output_size, at;
    char what[48];

    g_alloc = hdrg_test_allocator();
    pixels = picture();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 1));

    for (big = 0; big < 2u; big++) {
        for (value = 1; value <= 8u; value++) {
            exif_of(value, big != 0, exif);
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_test_jpeg_rgb(&g_alloc, pixels, 64, 32, 64 * 3, 95, NULL, 0,
                                                     exif, sizeof(exif), &input, &input_size));
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert(ctx, input, input_size, NULL, NULL, NULL,
                                                        &output, &output_size));
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_decode_any(&g_alloc, NULL, output, output_size, UINT64_MAX,
                                                  HDRG_CHROMA_FIT_LINEAR, &decoded));

            w = (value >= 5u) ? 32u : 64u;
            h = (value >= 5u) ? 64u : 32u;
            snprintf(what, sizeof(what), "orientation %u (%s)", (unsigned)value,
                     (big != 0) ? "MM" : "II");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(w, decoded.width, what);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(h, decoded.height, what);
            TEST_ASSERT_TRUE_MESSAGE(decoded.orientation <= 1u, what);
            xs[0] = 2;
            ys[0] = 2;
            xs[1] = w - 3u;
            ys[1] = 2;
            xs[2] = w - 3u;
            ys[2] = h - 3u;
            xs[3] = 2;
            ys[3] = h - 3u;

            /* red where the block went, dark blue at the other corners */
            for (k = 0; k < 4u; k++) {
                at = (size_t)ys[k] * decoded.stride + (size_t)xs[k] * 3u;
                TEST_ASSERT_TRUE_MESSAGE((decoded.pixels[at] > 120u) == (k == corner_of[value]),
                                         what);
            }

            hdrg_decoded_free(&g_alloc, &decoded);
            hdrg_free(output);
            hdrg_mem_free(&g_alloc, input);
        }
    }

    hdrg_context_destroy(ctx);
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
    int failures;

    UNITY_BEGIN();
    RUN_TEST(test_input_carries_orientation6);
    RUN_TEST(test_output_physically_rotated_and_exif_normalized);
    RUN_TEST(test_every_orientation_normalized);
    failures = UNITY_END();

    hdrg_mem_free(&g_alloc, g_input);

    return failures;
}
