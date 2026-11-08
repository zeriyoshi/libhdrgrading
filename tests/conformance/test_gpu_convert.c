/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "core/gpu_convert.h"
#include "priv.h"

/*
 * The Y'CbCr input's conversion on the GPU (src/core/gpu_convert,
 * src/core/wgsl/convert.wgsl) against the CPU's hdrg_yuv_to_rgb16 on
 * pictures of flat fills with dark outlines, ramps and grain: 4:2:0 / 4:2:2
 * / 4:4:4, 8 / 10 / 16 bits, video and full range, the three matrices,
 * every chroma location, both fits, odd sizes, rows of strides wider than
 * the samples, Cb and Cr in one plane and the samples in the high bits.
 * Every sample within TGC_MAX codes (16-bit; SEGMENTED TGC_BENT) and few
 * beyond TGC_NEAR; the GPU the same twice.
 * Ignored without a GPU.
 */

#define TGC_W     77u
#define TGC_H     45u
#define TGC_NEAR  1u    /* 16-bit codes: the multiply-adds a GPU fuses (M5 Ultra: 1 at most) */
#define TGC_SHARE 0.001 /* the share of samples beyond TGC_NEAR at most */
#define TGC_MAX   2u    /* LINEAR: every sample within */
#define TGC_BENT  16u   /* SEGMENTED: a knot or a bend may flip on f32 */
#define TGC_KEY   UINT64_C(0x5447435F4752414E)

typedef struct {
    uint8_t layout, matrix, bits, siting, fit;
    bool full_range, interleaved, msb;
} tgc_case_t;

static hdrg_context_t *g_ctx;
static hdrg_gpu_core_t *g_gpu;

/* the picture's R'G'B' at (x, y) in [0, 1]: fills and outlines, a ramp, grain */
static void tgc_rgb(uint32_t x, uint32_t y, double out[3])
{
    const double dx = (double)x - 30.0, dy = (double)y - 22.0, r = dx * dx + dy * dy;
    uint32_t h;
    uint8_t c;

    out[0] = 0.85;
    out[1] = 0.55;
    out[2] = 0.30;

    if (x > 52u) {
        out[0] = (double)y / (double)TGC_H;
        out[1] = 0.2 + 0.6 * (double)(x - 52u) / 25.0;
        out[2] = 0.7;
    }

    if (r < 200.0) {
        out[0] = 0.25;
        out[1] = 0.45;
        out[2] = 0.90;
    }

    /* the outlines: a dark ring and a dark bar */
    if ((r >= 200.0 && r < 240.0) || (x >= 10u && x <= 11u && y > 4u)) {
        out[0] = 0.05;
        out[1] = 0.04;
        out[2] = 0.06;
    }

    for (c = 0; y > 34u && c < 3u; c++) {
        h = hdrg_hash_u32(TGC_KEY, x, y, c);
        out[c] += ((double)(h % 41u) - 20.0) / 255.0;
        out[c] = (out[c] < 0.0) ? 0.0 : ((out[c] > 1.0) ? 1.0 : out[c]);
    }
}

/* code v of plane at (x, y): Cb, Cr alternating in planes[1] when interleaved, msb shifted */
static void tgc_put(const hdrg_yuv_t *yuv, uint8_t plane, uint32_t x, uint32_t y, double v)
{
    const uint8_t holder = (yuv->interleaved && plane != 0u) ? 1u : plane;
    const size_t at = (yuv->interleaved && plane != 0u) ? 2u * (size_t)x + plane - 1u : x;
    const double top = (double)((1u << yuv->bits) - 1u);
    uint16_t code;
    uint8_t *row;

    v = (v < 0.0) ? 0.0 : ((v > top) ? top : v);
    code = (uint16_t)((uint32_t)(v + 0.5) << (yuv->msb ? 16u - yuv->bits : 0u));
    row = (uint8_t *)(uintptr_t)yuv->planes[holder] + (size_t)y * yuv->strides[holder];

    if (yuv->bits == 8u) {
        row[at] = (uint8_t)code;
    } else {
        memcpy(row + at * 2u, &code, sizeof(code));
    }
}

/* the picture as the case's Y'CbCr: each site the mean chroma of the pixels it covers */
static void tgc_picture(const tgc_case_t *c, hdrg_yuv_t *yuv)
{
    const uint32_t cw = (c->layout == HDRG_YUV_444) ? TGC_W : (TGC_W + 1u) / 2u,
                   ch = (c->layout == HDRG_YUV_420) ? (TGC_H + 1u) / 2u : TGC_H,
                   sx = (c->layout == HDRG_YUV_444) ? 1u : 2u,
                   sy = (c->layout == HDRG_YUV_420) ? 2u : 1u;
    const size_t bytes = (c->bits > 8u) ? 2u : 1u;
    const double scale = (double)(1u << (c->bits - 8u)), top = (double)((1u << c->bits) - 1u),
                 kr = (c->matrix == HDRG_MATRIX_BT2020)  ? 0.2627
                      : (c->matrix == HDRG_MATRIX_BT601) ? 0.299
                                                         : 0.2126,
                 kb = (c->matrix == HDRG_MATRIX_BT2020)  ? 0.0593
                      : (c->matrix == HDRG_MATRIX_BT601) ? 0.114
                                                         : 0.0722,
                 y_off = c->full_range ? 0.0 : 16.0 * scale,
                 y_range = c->full_range ? top : 219.0 * scale,
                 c_off = c->full_range ? (double)(1u << (c->bits - 1u)) : 128.0 * scale,
                 c_range = c->full_range ? top : 224.0 * scale;
    uint32_t x, y, j, k, n;
    double rgb[3], luma, cb, cr;

    memset(yuv, 0, sizeof(*yuv));
    yuv->width = TGC_W;
    yuv->height = TGC_H;
    yuv->layout = c->layout;
    yuv->matrix = c->matrix;
    yuv->bits = c->bits;
    yuv->siting = c->siting;
    yuv->fit = c->fit;
    yuv->full_range = c->full_range;
    yuv->interleaved = c->interleaved;
    yuv->msb = c->msb;
    yuv->strides[0] = (size_t)TGC_W * bytes + 6u;
    yuv->strides[1] = (size_t)cw * bytes * (c->interleaved ? 2u : 1u) + 2u;
    yuv->strides[2] = c->interleaved ? 0u : yuv->strides[1];
    yuv->planes[0] = calloc((size_t)TGC_H, yuv->strides[0]);
    yuv->planes[1] = calloc((size_t)ch, yuv->strides[1]);
    yuv->planes[2] = c->interleaved ? NULL : calloc((size_t)ch, yuv->strides[2]);
    TEST_ASSERT_NOT_NULL(yuv->planes[0]);
    TEST_ASSERT_NOT_NULL(yuv->planes[1]);
    TEST_ASSERT_TRUE(c->interleaved || yuv->planes[2] != NULL);
    TEST_ASSERT_TRUE(hdrg_yuv_valid(yuv) && hdrg_yuv_fits(yuv));

    for (y = 0; y < TGC_H; y++) {
        for (x = 0; x < TGC_W; x++) {
            tgc_rgb(x, y, rgb);
            luma = kr * rgb[0] + (1.0 - kr - kb) * rgb[1] + kb * rgb[2];
            tgc_put(yuv, 0, x, y, y_off + y_range * luma);
        }
    }

    for (k = 0; k < ch; k++) {
        for (j = 0; j < cw; j++) {
            cb = 0.0;
            cr = 0.0;
            n = 0;

            for (y = k * sy; y < HDRG_MIN((k + 1u) * sy, TGC_H); y++) {
                for (x = j * sx; x < HDRG_MIN((j + 1u) * sx, TGC_W); x++) {
                    tgc_rgb(x, y, rgb);
                    luma = kr * rgb[0] + (1.0 - kr - kb) * rgb[1] + kb * rgb[2];
                    cb += (rgb[2] - luma) / (2.0 * (1.0 - kb));
                    cr += (rgb[0] - luma) / (2.0 * (1.0 - kr));
                    n++;
                }
            }

            tgc_put(yuv, 1, j, k, c_off + c_range * cb / (double)n);
            tgc_put(yuv, 2, j, k, c_off + c_range * cr / (double)n);
        }
    }
}

static void tgc_free(hdrg_yuv_t *yuv)
{
    uint8_t p;

    for (p = 0; p < 3u; p++) {
        free((void *)(uintptr_t)yuv->planes[p]);
    }
}

/* the case on the GPU into rgb (rows of stride bytes) */
static void tgc_gpu(const tgc_case_t *c, uint16_t *rgb, size_t stride)
{
    hdrg_yuv_t yuv;

    tgc_picture(c, &yuv);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_gpu_convert_frame(g_gpu, NULL, &yuv, rgb, stride, HDRG_GC_SLOTS, NULL));
    tgc_free(&yuv);
}

/* the case on the CPU and twice on the GPU: the samples' largest difference, the share beyond */
static void tgc_check(const tgc_case_t *c)
{
    const size_t samples = (size_t)TGC_W * TGC_H * 3u, stride = (size_t)TGC_W * 6u + 10u;
    hdrg_yuv_t yuv;
    uint32_t far, worst, d, y;
    uint16_t *cpu, *gpu, *again;
    size_t i;
    char message[160];
    float *scratch;

    tgc_picture(c, &yuv);
    cpu = (uint16_t *)calloc((size_t)TGC_H, stride);
    gpu = (uint16_t *)calloc((size_t)TGC_H, stride);
    again = (uint16_t *)calloc((size_t)TGC_H, stride);
    scratch = (float *)malloc(HDRG_TEST_MAX(hdrg_yuv_scratch(&yuv), (size_t)1) * sizeof(float));
    TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_NOT_NULL(gpu);
    TEST_ASSERT_NOT_NULL(again);
    TEST_ASSERT_NOT_NULL(scratch);

    hdrg_yuv_to_rgb16(NULL, &yuv, scratch, cpu, stride);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_gpu_convert_frame(g_gpu, NULL, &yuv, gpu, stride, HDRG_GC_SLOTS, NULL));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_gpu_convert_frame(g_gpu, NULL, &yuv, again, stride, HDRG_GC_SLOTS, NULL));
    TEST_ASSERT_EQUAL_MEMORY(gpu, again, (size_t)TGC_H * stride);

    far = 0;
    worst = 0;

    for (y = 0; y < TGC_H; y++) {
        for (i = 0; i < (size_t)TGC_W * 3u; i++) {
            d = (uint32_t)abs((int32_t)cpu[y * (stride / 2u) + i] -
                              (int32_t)gpu[y * (stride / 2u) + i]);
            worst = HDRG_TEST_MAX(worst, d);
            far += (d > TGC_NEAR) ? 1u : 0u;
        }
    }

    snprintf(message, sizeof(message),
             "layout %u matrix %u bits %u full %d siting %u fit %u: worst %u, %u beyond %u",
             c->layout, c->matrix, c->bits, c->full_range ? 1 : 0, c->siting, c->fit, worst, far,
             TGC_NEAR);
    TEST_ASSERT_TRUE_MESSAGE(worst <= ((c->fit == HDRG_CHROMA_FIT_SEGMENTED) ? TGC_BENT : TGC_MAX),
                             message);
    TEST_ASSERT_TRUE_MESSAGE((double)far <= TGC_SHARE * (double)samples, message);

    free(cpu);
    free(gpu);
    free(again);
    free(scratch);
    tgc_free(&yuv);
}

/* every layout, chroma location and fit (10-bit video range, BT.709) */
static void test_sitings(void)
{
    const uint8_t layouts[3] = {HDRG_YUV_420, HDRG_YUV_422, HDRG_YUV_444};
    tgc_case_t c;
    uint8_t l, s, f;

    if (g_gpu == NULL) {
        TEST_IGNORE_MESSAGE("no GPU");
    }

    memset(&c, 0, sizeof(c));
    c.matrix = HDRG_MATRIX_BT709;
    c.bits = 10;

    for (l = 0; l < 3u; l++) {
        for (s = 0; s <= HDRG_CHROMA_BOTTOM; s++) {
            for (f = 0; f <= HDRG_CHROMA_FIT_SEGMENTED; f++) {
                c.layout = layouts[l];
                c.siting = s;
                c.fit = f;
                tgc_check(&c);
            }
        }
    }
}

/* the bit depths, ranges and matrices (4:2:0 LEFT) */
static void test_formats(void)
{
    const uint8_t bits[3] = {8, 10, 16},
                  matrices[3] = {HDRG_MATRIX_BT709, HDRG_MATRIX_BT601, HDRG_MATRIX_BT2020};
    tgc_case_t c;
    uint8_t b, m, r, f;

    if (g_gpu == NULL) {
        TEST_IGNORE_MESSAGE("no GPU");
    }

    memset(&c, 0, sizeof(c));
    c.layout = HDRG_YUV_420;

    for (b = 0; b < 3u; b++) {
        for (m = 0; m < 3u; m++) {
            for (r = 0; r < 2u; r++) {
                for (f = 0; f <= HDRG_CHROMA_FIT_SEGMENTED; f++) {
                    c.bits = bits[b];
                    c.matrix = matrices[m];
                    c.full_range = r == 1u;
                    c.fit = f;
                    tgc_check(&c);
                }
            }
        }
    }
}

/*
 * Cb and Cr alternating in one plane (NV12, P010) and the samples in the
 * high bits (P010): the CPU's within the tolerance, and the same on the GPU
 * as a plane each in the low bits
 */
static void test_packed(void)
{
    const uint8_t layouts[3] = {HDRG_YUV_420, HDRG_YUV_422, HDRG_YUV_444}, bits[2] = {8, 10};
    const size_t stride = (size_t)TGC_W * 6u + 10u;
    tgc_case_t c, packed;
    uint16_t *planar, *got;
    uint8_t l, b, f, m;

    if (g_gpu == NULL) {
        TEST_IGNORE_MESSAGE("no GPU");
    }

    planar = (uint16_t *)calloc((size_t)TGC_H, stride);
    got = (uint16_t *)calloc((size_t)TGC_H, stride);
    TEST_ASSERT_NOT_NULL(planar);
    TEST_ASSERT_NOT_NULL(got);

    memset(&c, 0, sizeof(c));
    c.matrix = HDRG_MATRIX_BT709;

    for (l = 0; l < 3u; l++) {
        for (b = 0; b < 2u; b++) {
            for (f = HDRG_CHROMA_FIT_LINEAR; f <= HDRG_CHROMA_FIT_SEGMENTED; f++) {
                c.layout = layouts[l];
                c.bits = bits[b];
                c.fit = f;
                c.full_range = b == 1u;
                tgc_gpu(&c, planar, stride);

                for (m = 0; m < ((bits[b] > 8u) ? 3u : 1u); m++) {
                    packed = c;
                    packed.interleaved = m != 1u;
                    packed.msb = m != 0u;
                    tgc_check(&packed);
                    tgc_gpu(&packed, got, stride);
                    TEST_ASSERT_EQUAL_MEMORY(planar, got, (size_t)TGC_H * stride);
                }
            }
        }
    }

    free(planar);
    free(got);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    static const uint32_t kinds[3] = {HDRG_DEVICE_METAL, HDRG_DEVICE_VULKAN, HDRG_DEVICE_WEBGPU};
    hdrg_context_desc_t desc;
    uint8_t k, ran;

    hdrg_context_desc_init(&desc);

    if (hdrg_context_create(&desc, &g_ctx) != HDRG_OK) {
        return 1;
    }

    UNITY_BEGIN();
    ran = 0;

    /* each GPU of the build's and the system's in turn (none: the tests ignored) */
    for (k = 0; k < 4u; k++) {
        if (k < 3u && hdrg_gpu_core_create(&g_ctx->host, kinds[k], &g_gpu) != HDRG_OK) {
            continue;
        }

        if (k == 3u) {
            if (ran > 0) {
                break;
            }

            g_gpu = NULL;
        }

        ran++;
        RUN_TEST(test_sitings);
        RUN_TEST(test_formats);
        RUN_TEST(test_packed);
        hdrg_gpu_core_destroy(g_gpu);
        g_gpu = NULL;
    }

    hdrg_context_destroy(g_ctx);

    return UNITY_END();
}
