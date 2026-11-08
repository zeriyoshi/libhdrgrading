/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <setjmp.h>

#include <unity.h>

/* the native jpegli API, for JPEGs of a chosen subsampling and their raw planes */
#include "lib/jpegli/decode.h"
#include "lib/jpegli/encode.h"

#include "../test_util.h"
#include "core/yuv.h"
#include "io/io.h"
#include "hdrgrading/foundation/pool.h"

/*
 * JPEG decoding: a 4:2:0 / 4:2:2 JPEG's chroma is upsampled
 * guided by the luma, closer to the source at colored edges than jpegli's
 * own upsampling; the strips it converts give the bytes of the whole
 * picture's conversion (tall, odd-sized, progressive), for any worker count.
 */
#define TD_EDGE_W 64u
#define TD_EDGE_H 48u
#define TD_TALL_W 41u
#define TD_TALL_H 301u

static hdrg_allocator_t g_alloc;

typedef struct {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
} td_error_t;

static void td_error_exit(j_common_ptr cinfo)
{
    longjmp(((td_error_t *)(void *)cinfo->err)->jump, 1);
}

/* a JPEG of the 8-bit R'G'B' pixels with luma sampled h x v (malloc'ed) */
static uint8_t *td_encode(const uint8_t *rgb, uint32_t width, uint32_t height, int h, int v,
                          bool progressive, unsigned long *size)
{
    struct jpeg_compress_struct cinfo;
    td_error_t error;
    JSAMPROW row;
    uint32_t y;
    uint8_t *out;

    out = NULL;
    *size = 0;
    cinfo.err = jpegli_std_error(&error.mgr);
    error.mgr.error_exit = td_error_exit;

    if (setjmp(error.jump)) {
        TEST_FAIL_MESSAGE("jpegli encode");
    }

    jpegli_create_compress(&cinfo);
    jpegli_mem_dest(&cinfo, &out, size);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpegli_set_defaults(&cinfo);
    jpegli_set_quality(&cinfo, 92, TRUE);
    cinfo.comp_info[0].h_samp_factor = h;
    cinfo.comp_info[0].v_samp_factor = v;

    if (progressive) {
        jpegli_simple_progression(&cinfo);
    }

    jpegli_start_compress(&cinfo, TRUE);

    for (y = 0; y < height; y++) {
        row = (JSAMPROW)(uintptr_t)(rgb + (size_t)y * width * 3u);
        (void)jpegli_write_scanlines(&cinfo, &row, 1);
    }

    jpegli_finish_compress(&cinfo);
    jpegli_destroy_compress(&cinfo);

    return out;
}

/* jpegli's own R'G'B' (its upsampling), malloc'ed */
static uint8_t *td_jpegli_rgb(const uint8_t *jpeg, unsigned long size)
{
    struct jpeg_decompress_struct cinfo;
    td_error_t error;
    JSAMPROW row;
    uint8_t *rgb;

    cinfo.err = jpegli_std_error(&error.mgr);
    error.mgr.error_exit = td_error_exit;

    if (setjmp(error.jump)) {
        TEST_FAIL_MESSAGE("jpegli decode");
    }

    jpegli_create_decompress(&cinfo);
    jpegli_mem_src(&cinfo, jpeg, size);
    (void)jpegli_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    (void)jpegli_start_decompress(&cinfo);
    rgb = (uint8_t *)malloc((size_t)cinfo.output_width * cinfo.output_height * 3u);
    TEST_ASSERT_NOT_NULL(rgb);

    while (cinfo.output_scanline < cinfo.output_height) {
        row = rgb + (size_t)cinfo.output_scanline * cinfo.output_width * 3u;
        (void)jpegli_read_scanlines(&cinfo, &row, 1);
    }

    (void)jpegli_finish_decompress(&cinfo);
    jpegli_destroy_decompress(&cinfo);

    return rgb;
}

/*
 * the whole picture's conversion: every raw plane at 16 bits at once, then
 * core/yuv (fit) with the JFIF siting and one rounding to 8 bits (malloc'ed)
 */
static uint8_t *td_whole(const uint8_t *jpeg, unsigned long size, uint8_t fit)
{
    struct jpeg_decompress_struct cinfo;
    td_error_t error;
    hdrg_yuv_t yuv;
    JSAMPROW rows[3][16];
    JSAMPARRAY planes[3];
    uint32_t imcu, rows_total, i, done;
    uint16_t *data[3], *rgb;
    uint8_t c, *out;
    size_t strides[3], n;
    float *scratch;

    cinfo.err = jpegli_std_error(&error.mgr);
    error.mgr.error_exit = td_error_exit;

    if (setjmp(error.jump)) {
        TEST_FAIL_MESSAGE("jpegli raw decode");
    }

    jpegli_create_decompress(&cinfo);
    jpegli_mem_src(&cinfo, jpeg, size);
    (void)jpegli_read_header(&cinfo, TRUE);
    cinfo.raw_data_out = TRUE;
    jpegli_set_output_format(&cinfo, JPEGLI_TYPE_UINT16, JPEGLI_NATIVE_ENDIAN);
    (void)jpegli_start_decompress(&cinfo);

    imcu = (uint32_t)cinfo.max_v_samp_factor * DCTSIZE;
    rows_total = (cinfo.output_height + imcu - 1u) / imcu * imcu;

    for (c = 0; c < 3u; c++) {
        strides[c] = (size_t)cinfo.comp_info[c].width_in_blocks * DCTSIZE;
        data[c] = (uint16_t *)malloc(strides[c] * rows_total * sizeof(uint16_t));
        TEST_ASSERT_NOT_NULL(data[c]);
    }

    for (done = 0; done < rows_total; done += imcu) {
        for (c = 0; c < 3u; c++) {
            for (i = 0; i < (uint32_t)cinfo.comp_info[c].v_samp_factor * DCTSIZE; i++) {
                rows[c][i] =
                    (JSAMPROW)(void *)(data[c] +
                                       ((size_t)done * (uint32_t)cinfo.comp_info[c].v_samp_factor /
                                            (uint32_t)cinfo.max_v_samp_factor +
                                        i) *
                                           strides[c]);
            }

            planes[c] = rows[c];
        }

        TEST_ASSERT_EQUAL_UINT32(imcu, jpegli_read_raw_data(&cinfo, planes, imcu));
    }

    memset(&yuv, 0, sizeof(yuv));

    for (c = 0; c < 3u; c++) {
        yuv.planes[c] = data[c];
        yuv.strides[c] = strides[c] * sizeof(uint16_t);
    }

    yuv.width = cinfo.output_width;
    yuv.height = cinfo.output_height;
    yuv.layout = (cinfo.max_v_samp_factor == 2) ? HDRG_YUV_420 : HDRG_YUV_422;
    yuv.matrix = HDRG_MATRIX_BT601;
    yuv.bits = 16;
    yuv.siting = HDRG_CHROMA_CENTER;
    yuv.full_range = true;
    yuv.jfif = true;
    yuv.fit = fit;
    n = (size_t)yuv.width * yuv.height * 3u;
    rgb = (uint16_t *)malloc(n * sizeof(uint16_t));
    scratch = (float *)malloc(hdrg_yuv_scratch(&yuv) * sizeof(float));
    out = (uint8_t *)malloc(n);
    TEST_ASSERT_TRUE(rgb != NULL && scratch != NULL && out != NULL);

    hdrg_yuv_to_rgb16(NULL, &yuv, scratch, rgb, (size_t)yuv.width * 6u);

    for (i = 0; i < n; i++) {
        out[i] = (uint8_t)(((uint32_t)rgb[i] * 255u + 32767u) / 65535u);
    }

    jpegli_abort_decompress(&cinfo);
    jpegli_destroy_decompress(&cinfo);

    for (c = 0; c < 3u; c++) {
        free(data[c]);
    }

    free(rgb);
    free(scratch);

    return out;
}

/*
 * an orange field and a blue one meeting on an odd column (the chroma
 * straddles it), crossed by a 2-pixel dark line
 */
static uint8_t *td_edges(void)
{
    uint32_t x, y;
    uint8_t *rgb, *p;

    rgb = (uint8_t *)malloc((size_t)TD_EDGE_W * TD_EDGE_H * 3u);
    TEST_ASSERT_NOT_NULL(rgb);

    for (y = 0; y < TD_EDGE_H; y++) {
        for (x = 0; x < TD_EDGE_W; x++) {
            p = rgb + ((size_t)y * TD_EDGE_W + x) * 3u;
            p[0] = (x < 33u) ? 235u : 40u;
            p[1] = (x < 33u) ? 130u : 90u;
            p[2] = (x < 33u) ? 50u : 220u;

            if (x == 15u || x == 16u || y == 23u || y == 24u) {
                p[0] = 20;
                p[1] = 18;
                p[2] = 24;
            }
        }
    }

    return rgb;
}

/* the chroma (BT.601) error of b against a within 2 pixels of a luma edge of a, squared, summed */
static double td_edge_chroma(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height)
{
    const uint8_t *p, *q;
    uint32_t x, y, dx, dy;
    double sum, ya, yb, lo, hi, v;

    sum = 0.0;

    for (y = 2; y + 2u < height; y++) {
        for (x = 2; x + 2u < width; x++) {
            lo = 255.0;
            hi = 0.0;

            for (dy = 0; dy < 5u; dy++) {
                for (dx = 0; dx < 5u; dx++) {
                    p = a + ((size_t)(y + dy - 2u) * width + x + dx - 2u) * 3u;
                    v = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    lo = (v < lo) ? v : lo;
                    hi = (v > hi) ? v : hi;
                }
            }

            if (hi - lo < 32.0) {
                continue;
            }

            p = a + ((size_t)y * width + x) * 3u;
            q = b + ((size_t)y * width + x) * 3u;
            ya = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            yb = 0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2];
            sum += ((p[2] - ya) - (q[2] - yb)) * ((p[2] - ya) - (q[2] - yb)) / (1.772 * 1.772);
            sum += ((p[0] - ya) - (q[0] - yb)) * ((p[0] - ya) - (q[0] - yb)) / (1.402 * 1.402);
        }
    }

    return sum;
}

static void td_decode(const uint8_t *jpeg, unsigned long size, const hdrg_exec_t *exec, uint8_t fit,
                      hdrg_decoded_t *out)
{
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_decode_any(&g_alloc, exec, jpeg, size, UINT64_MAX, fit, out));
    TEST_ASSERT_EQUAL_UINT32(8, out->bit_depth);
}

/*
 * colored edges: the chroma from luma beats jpegli's own upsampling (4:2:0
 * and 4:2:2), and the segmented fit the straight line where the dark lines
 * cross the fields
 */
static void test_edges(void)
{
    static const int v[2] = {2, 1};
    hdrg_decoded_t decoded;
    unsigned long size;
    uint8_t i, *src, *jpeg, *own;
    double ours, theirs, bent;

    src = td_edges();

    for (i = 0; i < 2u; i++) {
        jpeg = td_encode(src, TD_EDGE_W, TD_EDGE_H, 2, v[i], false, &size);
        own = td_jpegli_rgb(jpeg, size);
        td_decode(jpeg, size, NULL, HDRG_CHROMA_FIT_LINEAR, &decoded);
        ours = td_edge_chroma(src, decoded.pixels, TD_EDGE_W, TD_EDGE_H);
        hdrg_decoded_free(&g_alloc, &decoded);
        td_decode(jpeg, size, NULL, HDRG_CHROMA_FIT_SEGMENTED, &decoded);
        bent = td_edge_chroma(src, decoded.pixels, TD_EDGE_W, TD_EDGE_H);
        theirs = td_edge_chroma(src, own, TD_EDGE_W, TD_EDGE_H);
        printf("4:2:%u edge chroma error: %.0f (segmented) / %.0f (chroma from luma) / %.0f "
               "(jpegli)\n",
               (v[i] == 2) ? 0u : 2u, bent, ours, theirs);

        TEST_ASSERT_TRUE(ours < 0.8 * theirs);
        TEST_ASSERT_TRUE(bent < 0.8 * ours);

        hdrg_decoded_free(&g_alloc, &decoded);
        free(own);
        free(jpeg);
    }

    free(src);
}

/*
 * a tall, odd-sized picture (several strips), baseline and progressive,
 * 4:2:0 and 4:2:2, either fit: the bytes of the whole picture's
 * conversion, with no workers or seven
 */
static void test_strips(void)
{
    static const int v[2] = {2, 1};
    hdrg_decoded_t decoded;
    hdrg_exec_t exec;
    unsigned long size;
    uint32_t x, y;
    uint8_t i, p, f, *src, *jpeg, *whole;

    src = (uint8_t *)malloc((size_t)TD_TALL_W * TD_TALL_H * 3u);
    TEST_ASSERT_NOT_NULL(src);

    for (y = 0; y < TD_TALL_H; y++) {
        for (x = 0; x < TD_TALL_W; x++) {
            src[((size_t)y * TD_TALL_W + x) * 3u] = (uint8_t)((x * 37u + y * 11u) % 251u);
            src[((size_t)y * TD_TALL_W + x) * 3u + 1u] = (uint8_t)(((x / 5u + y / 7u) % 2u) * 180u);
            src[((size_t)y * TD_TALL_W + x) * 3u + 2u] = (uint8_t)((x * y) % 253u);
        }
    }

    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    for (i = 0; i < 4u; i++) {
        jpeg = td_encode(src, TD_TALL_W, TD_TALL_H, 2, v[i % 2u], i >= 2u, &size);

        for (f = 0; f <= HDRG_CHROMA_FIT_SEGMENTED; f++) {
            whole = td_whole(jpeg, size, f);

            for (p = 0; p < 2u; p++) {
                td_decode(jpeg, size, (p == 0) ? NULL : &exec, f, &decoded);
                TEST_ASSERT_EQUAL_UINT32(TD_TALL_W, decoded.width);
                TEST_ASSERT_EQUAL_UINT32(TD_TALL_H, decoded.height);

                TEST_ASSERT_EQUAL_MEMORY(whole, decoded.pixels, (size_t)TD_TALL_W * TD_TALL_H * 3u);

                hdrg_decoded_free(&g_alloc, &decoded);
            }

            free(whole);
        }

        free(jpeg);
    }

    hdrg_pool_destroy(exec.pool);
    free(src);
}

/* 4:4:4 keeps jpegli's own R'G'B' */
static void test_full_chroma(void)
{
    hdrg_decoded_t decoded;
    unsigned long size;
    uint8_t *src, *jpeg, *own;

    src = td_edges();
    jpeg = td_encode(src, TD_EDGE_W, TD_EDGE_H, 1, 1, false, &size);
    own = td_jpegli_rgb(jpeg, size);
    td_decode(jpeg, size, NULL, HDRG_CHROMA_FIT_SEGMENTED, &decoded);

    TEST_ASSERT_EQUAL_MEMORY(own, decoded.pixels, (size_t)TD_EDGE_W * TD_EDGE_H * 3u);

    hdrg_decoded_free(&g_alloc, &decoded);
    free(own);
    free(jpeg);
    free(src);
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
    RUN_TEST(test_edges);
    RUN_TEST(test_full_chroma);
    RUN_TEST(test_strips);

    return UNITY_END();
}
