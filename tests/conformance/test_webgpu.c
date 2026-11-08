/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>

#include "../test_gpu_module.h"
#include "../test_stage.h"
#include "../test_util.h"
#include "core/noise.h"
#include "hdrgrading/gpu/webgpu.h"
#include "hdrgrading/filter.h"
#include "hdrgrading/video.h"
#include "host/stage.h"
#include "priv.h"
#include "hdrgrading/foundation/hash.h"

/*
 * The WebGPU filters (HDRG_WITH_WEBGPU) against their CPU
 * filters on the same frames: within the tolerance a GPU is given
 * (L2), measured and printed; hdrg.webgpu.defringe, hdrg.webgpu.deblock and
 * hdrg.webgpu.dering the same bytes as hdrg.defringe, hdrg.deblock and
 * hdrg.dering (the deblocking and deringing in a row in a session too, a frame
 * handed from one to the next), hdrg.webgpu.denoise within the tolerance of
 * hdrg.denoise.spatial. A session given the app's WGPUBuffers
 * (hdrg_device_frame_t: padded pitches, offsets, the planes in one buffer)
 * gives what one of host memory on the same device gives, bit for bit.
 * Without an adapter (no GPU, no lavapipe) the tests are ignored.
 */
#define TW_W      257u /* odd: the shader's last pixel of a row alone */
#define TW_H      131u
#define TW_KEY    UINT64_C(0x77656267707521)
#define TW_FRAMES 6u /* the session's */
#define TW_DEV_N  4u /* tw_dev_run's frames */

/* a session of the app's frames on its GPU: the size, the preset, the output */
typedef struct {
    uint32_t width, height;
    uint8_t preset;
    bool yuv_out; /* Y'CbCr 4:2:0 10-bit, else 16-bit R'G'B' */
} tw_dev_case_t;

/* tw_dev_run's planes: in (8-bit Y'CbCr 4:2:0) and out, offsets and pitches in bytes */
typedef struct {
    size_t in_at[3], in_pitch[3], out_at[3], out_pitch[3], out_row[3];
    size_t in_size, out_size;
    uint32_t out_rows[3];
    uint8_t out_planes;
} tw_dev_layout_t;

/* tw_dev_run's buffers, on the device's thread: made, written, read back, released */
typedef struct {
    WGPUBuffer in, out;
    const uint8_t *src; /* the input's bytes */
    uint8_t *dst;       /* the output's */
    size_t in_size, out_size;
    bool ok;
} tw_dev_io_t;

static hdrg_allocator_t g_alloc;
static bool g_ringing;    /* tw_run's noise side data says the frame rings (the defringing works) */
static bool g_blocky;     /* ... and that it has blocks (the deblocking works) */
static uint8_t g_latency; /* tw_session's HDRG_LATENCY_* (PLAYBACK) */
static uint32_t g_out_w;  /* tw_run as a SCALE stage to g_out_w x g_out_h (0: PREFILTER) ... */
static uint32_t g_out_h;
static const char *g_taps; /* ... with these options (NULL: the defaults) */
static const char *g_antiring;
#if defined(HDRG_TEST_GPU_MODULE)
static const char *g_between; /* tw_session's filter between id and next (the module's) */
#endif

/*
 * id over pixels (RGB16, TW_W x TW_H) as a PREFILTER stage of the whole
 * frame with the frame's noise (strength NULL: the default), or a SCALE
 * stage (g_out_w), into out; HDRG_ERR_UNSUPPORTED when the filter has no
 * device
 */
static hdrg_status_t tw_run(const char *id, const uint16_t *pixels, const char *strength,
                            uint16_t *out, bool *passthrough)
{
    const hdrg_class_t *cls;
    hdrg_filter_entry_t entry;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_stage_t stage;
    hdrg_noise_t noise;
    hdrg_options_t *text;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t out_w, out_h;
    float data[HDRG_SD_NOISE_FLOATS];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)pixels;
    raw.width = TW_W;
    raw.height = TW_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TW_W * 6u;
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_noise_estimate(&g_alloc, NULL, &src, &noise));
    hdrg_noise_side_data(&noise, data);

    if (g_ringing) {
        data[13] = 3.0f;
        data[14] = 2.0f / 255.0f;
    }

    if (g_blocky) {
        data[11] = 2.0f;
        data[12] = 2.0f / 255.0f;
    }

    cls = hdrg_host_find_class(&ctx->host, id, strlen(id));
    TEST_ASSERT_NOT_NULL(cls);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &text));

    if (strength != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, "strength", strength));
    }

    if (g_taps != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, "taps", g_taps));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, "antiring", g_antiring));
    }

    entry.cls = cls;
    entry.stage = (g_out_w != 0) ? HDRG_STAGE_SCALE : HDRG_STAGE_PREFILTER;
    out_w = (g_out_w != 0) ? g_out_w : TW_W;
    out_h = (g_out_w != 0) ? g_out_h : TW_H;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_host_resolve_options(&ctx->host, cls, text, &entry.options));
    status =
        hdrg_test_stage_open(&g_alloc, &entry, &src, out_w, out_h, out_h, data, &ctx->host, &stage);

    if (status == HDRG_OK) {
        status = hdrg_stage_rows(&stage, NULL, 0, out_h, out, &ctx->host);
        *passthrough = stage.passthrough;
        hdrg_stage_close(&stage);
    }

    hdrg_options_destroy(entry.options);
    hdrg_options_destroy(text);
    hdrg_source_free(&g_alloc, &src);
    hdrg_color_free(&g_alloc, &color);
    hdrg_context_destroy(ctx);

    return status;
}

/*
 * A frame for the debanding: 8-bit steps of a colored gradient across both
 * axes, a dark line, a step edge, a grain of 3 8-bit codes, a light grain
 * of 0.6 codes over the steps
 */
static void tw_frame(uint16_t *pixels)
{
    uint32_t x, y;
    uint8_t c;
    double v, u1, u2, n;

    for (y = 0; y < TW_H; y++) {
        for (x = 0; x < TW_W; x++) {
            u1 = (double)hdrg_hash_uniform(TW_KEY, x, y, 0u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TW_KEY, x, y, 1u);
            n = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);

            for (c = 0; c < 3u; c++) {
                v = 60.0 + 0.4 * (double)x + 0.25 * (double)y + 30.0 * (double)c;

                if (x >= 170u && x < 210u) {
                    v = (x < 190u) ? 200.0 - 20.0 * c : 40.0 + 10.0 * c;
                } else if (x >= 210u) {
                    v += 3.0 * n;
                } else if (y >= 90u) {
                    v += 0.6 * n;
                }

                if (x == 80u || x == 81u) {
                    v = 10.0;
                }

                pixels[((size_t)y * TW_W + x) * 3u + c] =
                    (uint16_t)(fmin(fmax(floor(v + 0.5), 0.0), 255.0) * 257.0);
            }
        }
    }
}

/*
 * A frame for the deblocking: a colored gradient whose 8 x 8 blocks are
 * moved by up to 3 8-bit codes each under a grain of 0.5 codes, a dark line
 * on a block boundary and a step edge
 */
static void tw_blocks(uint16_t *pixels)
{
    uint32_t x, y;
    uint8_t c;
    double v, u1, u2, n, offset;

    for (y = 0; y < TW_H; y++) {
        for (x = 0; x < TW_W; x++) {
            u1 = (double)hdrg_hash_uniform(TW_KEY, x, y, 0u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TW_KEY, x, y, 1u);
            n = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
            offset = 6.0 * ((double)hdrg_hash_uniform(TW_KEY, x / 8u, y / 8u, 2u) - 0.5);

            for (c = 0; c < 3u; c++) {
                v = 70.0 + 0.3 * (double)x + 0.2 * (double)y + 25.0 * (double)c + offset + 0.5 * n;

                if (x >= 170u && x < 210u) {
                    v = (x < 190u) ? 200.0 - 20.0 * c : 40.0 + 10.0 * c;
                }

                if (x == 80u || x == 81u) {
                    v = 10.0;
                }

                pixels[((size_t)y * TW_W + x) * 3u + c] =
                    (uint16_t)(fmin(fmax(floor(v + 0.5), 0.0), 255.0) * 257.0);
            }
        }
    }
}

/*
 * A frame for the deringing in a session: the blocks frame with a dark line
 * every 32 pixels on the grid and a swing of 4 8-bit codes changing sign
 * every pixel 3 to 6 pixels after each (inside the line's block)
 */
static void tw_rings(uint16_t *pixels)
{
    uint32_t x, y, k;
    uint16_t *s;
    uint8_t c;
    int32_t swing;

    tw_blocks(pixels);

    for (y = 0; y < TW_H; y++) {
        for (x = 0; x < TW_W; x++) {
            k = x % 32u;
            s = pixels + ((size_t)y * TW_W + x) * 3u;
            swing = ((x + y) % 2u == 0) ? 1028 : -1028;

            for (c = 0; c < 3u; c++) {
                if (k < 2u) {
                    s[c] = (uint16_t)(10u * 257u);
                } else if (k >= 4u && k < 8u) {
                    s[c] = (uint16_t)HDRG_CLAMP((int32_t)s[c] + swing, 0, 65535);
                }
            }
        }
    }
}

/* the test image of test_util widened to 16 bits */
static void tw_image(uint16_t *pixels)
{
    uint8_t *image;
    size_t i;

    image = hdrg_test_image(TW_W, TW_H);
    TEST_ASSERT_NOT_NULL(image);

    for (i = 0; i < (size_t)TW_W * TW_H * 3u; i++) {
        pixels[i] = (uint16_t)(image[i] * 257u);
    }

    free(image);
}

/*
 * hdrg.webgpu.deband against hdrg.deband: the samples the CPU filter
 * changes (enough of them to show it works), the GPU's largest difference
 * from the CPU's and the samples apart by more than 2 (16-bit) codes, on the
 * steps frame and the test image; strength 0 passes on both
 */
static void test_deband(void)
{
    const size_t samples = (size_t)TW_W * TW_H * 3u;
    uint32_t round, worst, apart, changed;
    uint16_t *in, *cpu, *gpu;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[160];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_frame(in);

    if (tw_run("hdrg.webgpu.deband", in, "1", gpu, &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_image(in);
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.deband", in, "1", cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.deband", in, "1", gpu, &gpu_pass));
        TEST_ASSERT_TRUE(cpu_pass == gpu_pass);

        worst = 0;
        apart = 0;
        changed = 0;

        for (i = 0; i < samples; i++) {
            worst = HDRG_MAX(worst, (uint32_t)abs((int32_t)gpu[i] - (int32_t)cpu[i]));
            apart += (abs((int32_t)gpu[i] - (int32_t)cpu[i]) > 2) ? 1u : 0u;
            changed += (cpu[i] != in[i]) ? 1u : 0u;
        }

        snprintf(line, sizeof(line),
                 "deband (%s): the CPU changes %.1f%% of the samples; the GPU is at most %u "
                 "codes from it, %u samples more than 2",
                 (round == 0) ? "steps" : "image", 100.0 * (double)changed / (double)samples, worst,
                 apart);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(round == 1u || changed > samples / 4u);
        TEST_ASSERT_TRUE(worst <= 16u);
        TEST_ASSERT_TRUE((double)apart <= 0.001 * (double)samples);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.deband", in, "0", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * TW_FRAMES frames of the steps frame moving a column a frame through a
 * session with the PREFILTER filter id, into out (16-bit PQ, TW_FRAMES
 * frames); HDRG_ERR_UNSUPPORTED when it has no device
 */
/*
 * hdrg.webgpu.defringe against hdrg.defringe on frames said to ring: the same
 * bytes (the same integers), on the test image samples changed (the steps
 * frame has no chroma to defringe); without the ringing and with strength 0
 * it passes
 */
static void test_defringe(void)
{
    const size_t samples = (size_t)TW_W * TW_H * 3u;
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_image(in);
    g_ringing = true;

    if (tw_run("hdrg.webgpu.defringe", in, "3", gpu, &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        g_ringing = false;
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_frame(in);
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.defringe", in, "3", cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.defringe", in, "3", gpu, &gpu_pass));
        TEST_ASSERT_FALSE(cpu_pass);
        TEST_ASSERT_FALSE(gpu_pass);

        changed = 0;
        for (i = 0; i < samples; i++) {
            changed += (cpu[i] != in[i]) ? 1u : 0u;
        }

        snprintf(line, sizeof(line), "defringe (%s): the CPU changes %.1f%% of the samples",
                 (round == 0) ? "image" : "steps", 100.0 * (double)changed / (double)samples);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(round == 1u || changed > samples / 100u);
        TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.defringe", in, "0", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_ringing = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.defringe", in, "3", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.webgpu.deblock against hdrg.deblock: the same bytes (the same
 * integers) on the blocks frame (its own noise: blocks found, samples
 * changed) and on the test image said to have blocks; with strength 0 it
 * passes
 */
static void test_deblock(void)
{
    const size_t samples = (size_t)TW_W * TW_H * 3u;
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_blocks(in);

    if (tw_run("hdrg.webgpu.deblock", in, "1", gpu, &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_image(in);
            g_blocky = true;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.deblock", in, "1", cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.deblock", in, "1", gpu, &gpu_pass));
        TEST_ASSERT_FALSE(cpu_pass);
        TEST_ASSERT_FALSE(gpu_pass);

        changed = 0;
        for (i = 0; i < samples; i++) {
            changed += (cpu[i] != in[i]) ? 1u : 0u;
        }

        snprintf(line, sizeof(line), "deblock (%s): the CPU changes %.1f%% of the samples",
                 (round == 0) ? "blocks" : "image", 100.0 * (double)changed / (double)samples);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(changed > samples / 20u);
        TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.deblock", in, "0", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_blocky = false;

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.webgpu.denoise against hdrg.denoise.spatial on the steps frame and
 * the blocks frame (their own noise) at strengths 1 and 3: the samples the CPU
 * filter changes, the GPU's largest difference from the CPU's and the
 * samples apart by more than 2 (16-bit) codes; strength 0 passes
 */
static void test_denoise(void)
{
    const size_t samples = (size_t)TW_W * TW_H * 3u;
    const char *const strengths[2] = {"1", "3"};
    uint32_t round, worst, apart, changed;
    uint16_t *in, *cpu, *gpu;
    uint8_t k;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[160];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_frame(in);

    if (tw_run("hdrg.webgpu.denoise", in, "1", gpu, &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_blocks(in);
        }

        for (k = 0; k < 2u; k++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  tw_run("hdrg.denoise.spatial", in, strengths[k], cpu, &cpu_pass));
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  tw_run("hdrg.webgpu.denoise", in, strengths[k], gpu, &gpu_pass));
            TEST_ASSERT_FALSE(cpu_pass);
            TEST_ASSERT_FALSE(gpu_pass);

            worst = 0;
            apart = 0;
            changed = 0;

            for (i = 0; i < samples; i++) {
                worst = HDRG_MAX(worst, (uint32_t)abs((int32_t)gpu[i] - (int32_t)cpu[i]));
                apart += (abs((int32_t)gpu[i] - (int32_t)cpu[i]) > 2) ? 1u : 0u;
                changed += (cpu[i] != in[i]) ? 1u : 0u;
            }

            snprintf(line, sizeof(line),
                     "denoise %s (%s): the CPU changes %.1f%% of the samples; the GPU is at most "
                     "%u codes from it, %u samples more than 2",
                     strengths[k], (round == 0) ? "steps" : "blocks",
                     100.0 * (double)changed / (double)samples, worst, apart);
            TEST_MESSAGE(line);

            TEST_ASSERT_TRUE(changed > samples / 4u);
            TEST_ASSERT_TRUE(worst <= 16u);
            TEST_ASSERT_TRUE((double)apart <= 0.001 * (double)samples);
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.denoise", in, "0", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.webgpu.dering against hdrg.dering on frames said to ring: the same
 * bytes (the same integers) at strengths 1, 2 and 3 (windows of radius 3, 5
 * and 7) on the test image and the steps frame, samples changed; with
 * strength 0 and without the ringing it passes
 */
static void test_dering(void)
{
    const size_t samples = (size_t)TW_W * TW_H * 3u;
    const char *const strengths[3] = {"1", "2", "3"};
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    uint8_t k;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_image(in);
    g_ringing = true;

    if (tw_run("hdrg.webgpu.dering", in, "1", gpu, &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        g_ringing = false;
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_frame(in);
        }

        for (k = 0; k < 3u; k++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.dering", in, strengths[k], cpu, &cpu_pass));
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  tw_run("hdrg.webgpu.dering", in, strengths[k], gpu, &gpu_pass));
            TEST_ASSERT_FALSE(cpu_pass);
            TEST_ASSERT_FALSE(gpu_pass);

            changed = 0;
            for (i = 0; i < samples; i++) {
                changed += (cpu[i] != in[i]) ? 1u : 0u;
            }

            snprintf(line, sizeof(line), "dering %s (%s): the CPU changes %.1f%% of the samples",
                     strengths[k], (round == 0) ? "image" : "steps",
                     100.0 * (double)changed / (double)samples);
            TEST_MESSAGE(line);

            TEST_ASSERT_TRUE(changed > samples / 100u);
            TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.dering", in, "0", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_ringing = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_run("hdrg.webgpu.dering", in, "1", gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.webgpu.lanczos against hdrg.scale.lanczos: the same bytes (the CPU's
 * tables and integers) on the test image and the steps frame, upscaled 2x
 * and by 1.6 / 1.5 and downscaled by 2 and 2.57 / 2.62, at 2, 3 and 4 lobes
 * with and without anti-ringing
 */
static void test_lanczos(void)
{
    const uint32_t sizes[4][2] = {{TW_W * 2u, TW_H * 2u}, {411, 197}, {128, 65}, {100, 50}};
    const size_t in_samples = (size_t)TW_W * TW_H * 3u;
    const char *const lobes[3] = {"2", "3", "4"}, *const antirings[2] = {"1", "0"};
    uint32_t round, s, k, a;
    uint16_t *in, *cpu, *gpu;
    bool passthrough;

    in = (uint16_t *)malloc(in_samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc((size_t)TW_W * TW_H * 12u * sizeof(uint16_t));
    gpu = (uint16_t *)malloc((size_t)TW_W * TW_H * 12u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tw_image(in);
    g_out_w = sizes[0][0];
    g_out_h = sizes[0][1];

    if (tw_run("hdrg.webgpu.lanczos", in, NULL, gpu, &passthrough) == HDRG_ERR_UNSUPPORTED) {
        g_out_w = 0;
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tw_frame(in);
        }

        for (s = 0; s < 4u; s++) {
            g_out_w = sizes[s][0];
            g_out_h = sizes[s][1];

            for (k = 0; k < 3u; k++) {
                for (a = 0; a < 2u; a++) {
                    g_taps = lobes[k];
                    g_antiring = antirings[a];
                    TEST_ASSERT_EQUAL_INT(
                        HDRG_OK, tw_run("hdrg.scale.lanczos", in, NULL, cpu, &passthrough));
                    TEST_ASSERT_EQUAL_INT(
                        HDRG_OK, tw_run("hdrg.webgpu.lanczos", in, NULL, gpu, &passthrough));
                    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu,
                                             (size_t)g_out_w * g_out_h * 3u * sizeof(uint16_t));
                }
            }
        }
    }

    g_out_w = 0;
    g_taps = NULL;
    g_antiring = NULL;

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * TW_FRAMES frames through a session with the PREFILTER filter id, and next
 * after it (NULL: none), both of strength 1, into out (16-bit PQ, TW_FRAMES
 * frames); HDRG_ERR_UNSUPPORTED when one has no device
 */
static hdrg_status_t tw_session(const char *id, const char *next, const uint16_t *frames,
                                uint16_t *out, uint32_t *latency)
{
    const size_t frame_samples = (size_t)TW_W * TW_H * 3u;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_options_t *options;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, got;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "strength", "1"));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, id, options));

#if defined(HDRG_TEST_GPU_MODULE)
    if (g_between != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_load_plugin(ctx, HDRG_TEST_GPU_MODULE));
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, g_between, NULL));
    }
#endif

    if (next != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, next, options));
    }

    hdrg_options_destroy(options);

    memset(&format, 0, sizeof(format));
    format.width = TW_W;
    format.height = TW_H;
    format.bit_depth = 16;
    format.stride = (size_t)TW_W * 6u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    params.mctf_strength = 0.0f; /* the filters under test alone */
    params.grain_policy = HDRG_GRAIN_PRESERVE;
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.proxy = 0;
    config.future = 0; /* the filters' delays alone */
    config.latency = g_latency;

    status = hdrg_session_create(ctx, &format, &params, NULL, &config, &session);
    if (status != HDRG_OK) {
        hdrg_context_destroy(ctx);

        return status;
    }

    *latency = hdrg_session_latency(session);
    memset(&in, 0, sizeof(in));
    in.strides[0] = format.stride;
    sent = 0;
    got = 0;

    for (;;) {
        memset(&info, 0, sizeof(info));
        info.planes[0] = (uint8_t *)(out + frame_samples * got);
        info.strides[0] = (size_t)TW_W * 6u;

        status = hdrg_session_receive_frame(session, &info);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = (const uint8_t *)(const void *)(frames + frame_samples * sent);
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TW_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status != HDRG_OK) {
            break;
        }

        TEST_ASSERT_TRUE(info.pts == (int64_t)got);
        got++;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_EOF, status);
    TEST_ASSERT_EQUAL_UINT32(TW_FRAMES, got);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return HDRG_OK;
}

/*
 * A video session with hdrg.webgpu.deband (ASYNC, two frames in flight: a
 * frame of latency) against one with hdrg.deband: every frame in order,
 * each within the tolerance in 16-bit PQ codes (printed); with
 * hdrg.webgpu.deblock and hdrg.webgpu.dering in a row (a frame handed from
 * one to the next, a frame of latency each; in LOW kept on the device
 * between them, no latency) the bytes of the CPU's, the deringing at work
 */
static void test_session(void)
{
    const size_t frame_samples = (size_t)TW_W * TW_H * 3u;
    uint32_t t, x, y, latency, cpu_latency, worst, apart, step;
    uint16_t *still, *frames, *cpu, *gpu;
    size_t i;
    char line[160];

    still = (uint16_t *)malloc(frame_samples * sizeof(uint16_t));
    frames = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(still != NULL && frames != NULL && cpu != NULL && gpu != NULL);

    tw_frame(still);
    step = 1;

    for (t = 0; t < TW_FRAMES; t++) {
        for (y = 0; y < TW_H; y++) {
            for (x = 0; x < TW_W; x++) {
                memcpy(frames + frame_samples * t + ((size_t)y * TW_W + x) * 3u,
                       still + ((size_t)y * TW_W + (x + step * t) % TW_W) * 3u,
                       3u * sizeof(uint16_t));
            }
        }
    }

    if (tw_session("hdrg.webgpu.deband", NULL, frames, gpu, &latency) == HDRG_ERR_UNSUPPORTED) {
        free(still);
        free(frames);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_session("hdrg.deband", NULL, frames, cpu, &cpu_latency));
    worst = 0;
    apart = 0;

    for (i = 0; i < frame_samples * TW_FRAMES; i++) {
        worst = HDRG_MAX(worst, (uint32_t)abs((int32_t)gpu[i] - (int32_t)cpu[i]));
        apart += (abs((int32_t)gpu[i] - (int32_t)cpu[i]) > 4) ? 1u : 0u;
    }

    snprintf(line, sizeof(line),
             "session: the GPU's frames at most %u PQ codes from the CPU's, %u samples more "
             "than 4; latency %u frames (CPU %u)",
             worst, apart, latency, cpu_latency);
    TEST_MESSAGE(line);

    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 1u, latency);
    TEST_ASSERT_TRUE(worst <= 64u);
    TEST_ASSERT_TRUE((double)apart <= 0.001 * (double)(frame_samples * TW_FRAMES));

    /* the deblocking and the deringing in a row over the rings, a block further each frame */
    tw_rings(still);
    step = 8;

    for (t = 0; t < TW_FRAMES; t++) {
        for (y = 0; y < TW_H; y++) {
            for (x = 0; x < TW_W; x++) {
                memcpy(frames + frame_samples * t + ((size_t)y * TW_W + x) * 3u,
                       still + ((size_t)y * TW_W + (x + step * t) % TW_W) * 3u,
                       3u * sizeof(uint16_t));
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tw_session("hdrg.deblock", "hdrg.dering", frames, cpu, &cpu_latency));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tw_session("hdrg.webgpu.deblock", "hdrg.webgpu.dering", frames, gpu, &latency));
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, frame_samples * TW_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 2u, latency);

    /* ... in LOW too (a frame in flight at most: the frame kept on the device between them) */
    g_latency = HDRG_LATENCY_LOW;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tw_session("hdrg.webgpu.deblock", "hdrg.webgpu.dering", frames, gpu, &latency));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tw_session("hdrg.deblock", "hdrg.dering", frames, cpu, &cpu_latency));
    g_latency = HDRG_LATENCY_PLAYBACK;
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, frame_samples * TW_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency, latency);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_session("hdrg.deblock", NULL, frames, gpu, &latency));
    apart = 0;

    for (i = 0; i < frame_samples * TW_FRAMES; i++) {
        apart += (cpu[i] != gpu[i]) ? 1u : 0u;
    }

    snprintf(line, sizeof(line), "session: the deringing changes %.1f%% of the samples",
             100.0 * (double)apart / (double)(frame_samples * TW_FRAMES));
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(apart > frame_samples * TW_FRAMES / 100u);

    free(still);
    free(frames);
    free(cpu);
    free(gpu);
}

#if defined(HDRG_TEST_GPU_MODULE)
/*
 * a module's copy on the device (tests/modules/gpu_copy.c: a GPU layer of its own, its work on
 * the library's device thread) between hdrg.webgpu.deblock and hdrg.webgpu.dering in LOW: frames
 * taken and given on the device, the output and latency those of the two alone
 */
static void test_module_frames(void)
{
    const size_t frame_samples = (size_t)TW_W * TW_H * 3u;
    uint32_t t, x, y, latency, alone_latency, in, out, in_after, out_after;
    uint16_t *still, *frames, *alone, *between;
    void *module;

    still = (uint16_t *)malloc(frame_samples * sizeof(uint16_t));
    frames = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    alone = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    between = (uint16_t *)malloc(frame_samples * TW_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(still != NULL && frames != NULL && alone != NULL && between != NULL);

    tw_rings(still);

    for (t = 0; t < TW_FRAMES; t++) {
        for (y = 0; y < TW_H; y++) {
            for (x = 0; x < TW_W; x++) {
                memcpy(frames + frame_samples * t + ((size_t)y * TW_W + x) * 3u,
                       still + ((size_t)y * TW_W + (x + 8u * t) % TW_W) * 3u,
                       3u * sizeof(uint16_t));
            }
        }
    }

    g_latency = HDRG_LATENCY_LOW;

    if (tw_session("hdrg.webgpu.deblock", "hdrg.webgpu.dering", frames, alone, &alone_latency) ==
        HDRG_ERR_UNSUPPORTED) {
        g_latency = HDRG_LATENCY_PLAYBACK;
        free(still);
        free(frames);
        free(alone);
        free(between);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    g_between = "org.test.gpu.webgpu.copy";
    module = hdrg_test_gpu_module_open(HDRG_TEST_GPU_MODULE);
    TEST_ASSERT_NOT_NULL(module);
    hdrg_test_gpu_module_jobs(module, &in, &out);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tw_session("hdrg.webgpu.deblock", "hdrg.webgpu.dering", frames,
                                              between, &latency));
    hdrg_test_gpu_module_jobs(module, &in_after, &out_after);
    hdrg_test_gpu_module_close(module);
    g_between = NULL;
    g_latency = HDRG_LATENCY_PLAYBACK;
    TEST_ASSERT_EQUAL_MEMORY(alone, between, frame_samples * TW_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(alone_latency, latency);
    TEST_ASSERT_TRUE(in_after > in);
    TEST_ASSERT_TRUE(out_after > out);

    free(still);
    free(frames);
    free(alone);
    free(between);
}
#endif

/*
 * the context's own device: the same one each time it is asked, and one
 * hdrg_gpu_t (one thread) for its filters and its core
 */
static void test_shared_device(void)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_gpu_t *a, *b;
    void *device, *queue, *again, *again_queue;

    hdrg_context_desc_init(&desc);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    if (!hdrg_host_device(&ctx->host, HDRG_DEVICE_WEBGPU, &device, &queue)) {
        hdrg_context_destroy(ctx);
        TEST_IGNORE_MESSAGE("no WebGPU adapter");
    }

    TEST_ASSERT_TRUE(hdrg_host_device(&ctx->host, HDRG_DEVICE_WEBGPU, &again, &again_queue));
    TEST_ASSERT_EQUAL_PTR(device, again);
    TEST_ASSERT_TRUE(hdrg_gpu_create(&ctx->host.api, (WGPUDevice)device, &a));
    TEST_ASSERT_TRUE(hdrg_gpu_create(&ctx->host.api, (WGPUDevice)device, &b));
    TEST_ASSERT_EQUAL_PTR(a, b);

    hdrg_gpu_destroy(a);
    hdrg_gpu_destroy(b);
    hdrg_context_destroy(ctx);
}

/* the planes of a case: whole words, padded pitches, the planes after offsets */
static void tw_dev_layout(const tw_dev_case_t *c, tw_dev_layout_t *l)
{
    const size_t cw = (c->width + 1u) / 2u, ch = (c->height + 1u) / 2u;
    uint8_t p;

    memset(l, 0, sizeof(*l));
    l->in_pitch[0] = ((c->width + 67u) / 64u) * 64u;
    l->in_pitch[1] = ((cw + 35u) / 32u) * 32u;
    l->in_pitch[2] = l->in_pitch[1];
    l->in_at[0] = 256;
    l->in_at[1] = l->in_at[0] + l->in_pitch[0] * c->height + 4u;
    l->in_at[2] = l->in_at[1] + l->in_pitch[1] * ch + 8u;
    l->in_size = l->in_at[2] + l->in_pitch[2] * ch;
    l->out_planes = c->yuv_out ? 3u : 1u;
    l->out_row[0] = c->yuv_out ? (size_t)c->width * 2u : (size_t)c->width * 6u;
    l->out_row[1] = cw * 2u;
    l->out_row[2] = cw * 2u;
    l->out_rows[0] = c->height;
    l->out_rows[1] = (uint32_t)ch;
    l->out_rows[2] = (uint32_t)ch;
    l->out_at[0] = 512;

    for (p = 0; p < l->out_planes; p++) {
        l->out_pitch[p] = ((l->out_row[p] + 63u) / 64u) * 64u + 64u;
        l->out_at[p] = (p == 0) ? l->out_at[0]
                                : l->out_at[p - 1u] + l->out_pitch[p - 1u] * l->out_rows[p - 1u];
    }

    l->out_size = l->out_at[l->out_planes - 1u] +
                  l->out_pitch[l->out_planes - 1u] * l->out_rows[l->out_planes - 1u];
}

/* frame t's planes into in (a ramp, chroma steps and hash noise) */
static void tw_dev_planes(const tw_dev_case_t *c, const tw_dev_layout_t *l, uint32_t t, uint8_t *in)
{
    const uint32_t cw = (c->width + 1u) / 2u, ch = (c->height + 1u) / 2u;
    uint32_t x, y, h;

    for (y = 0; y < c->height; y++) {
        for (x = 0; x < c->width; x++) {
            h = hdrg_hash_u32(TW_KEY, x, y, t);
            in[l->in_at[0] + y * l->in_pitch[0] + x] =
                (uint8_t)(16u + ((x * 3u + y + t * 7u) % 200u) + h % 9u);
        }
    }

    for (y = 0; y < ch; y++) {
        for (x = 0; x < cw; x++) {
            h = hdrg_hash_u32(TW_KEY, x, y, t + 100u);
            in[l->in_at[1] + y * l->in_pitch[1] + x] =
                (uint8_t)(108u + ((x + t) / 8u % 40u) + h % 5u);
            in[l->in_at[2] + y * l->in_pitch[2] + x] =
                (uint8_t)(98u + ((y * 2u) / 8u % 50u) + h % 3u);
        }
    }
}

static void tw_dev_make(hdrg_gpu_t *gpu, void *arg)
{
    const WGPUBufferUsage usage =
        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    tw_dev_io_t *io = (tw_dev_io_t *)arg;

    io->in = hdrg_gpu_buffer(gpu, io->in_size, usage);
    io->out = hdrg_gpu_buffer(gpu, io->out_size, usage);
    io->ok = io->in != NULL && io->out != NULL;
}

static void tw_dev_write(hdrg_gpu_t *gpu, void *arg)
{
    tw_dev_io_t *io = (tw_dev_io_t *)arg;

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), io->in, 0, io->src, io->in_size);
    io->ok = hdrg_gpu_finish(gpu);
}

static void tw_dev_read(hdrg_gpu_t *gpu, void *arg)
{
    const void *mapped;
    tw_dev_io_t *io = (tw_dev_io_t *)arg;
    WGPUBuffer readback;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;

    io->ok = false;
    readback =
        hdrg_gpu_buffer(gpu, io->out_size, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
    if (readback == NULL) {
        return;
    }

    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, io->out, 0, readback, 0, io->out_size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);

    mapped = hdrg_gpu_map_read(gpu, readback, io->out_size);

    if (mapped != NULL) {
        memcpy(io->dst, mapped, io->out_size);
        wgpuBufferUnmap(readback);
        io->ok = true;
    }

    wgpuBufferRelease(readback);
}

static void tw_dev_free(hdrg_gpu_t *gpu, void *arg)
{
    tw_dev_io_t *io = (tw_dev_io_t *)arg;

    (void)gpu;

    if (io->in != NULL) {
        wgpuBufferRelease(io->in);
    }

    if (io->out != NULL) {
        wgpuBufferRelease(io->out);
    }
}

/*
 * The case's session on the app's WebGPU device (one of its own): its
 * frames as the app's WGPUBuffers (device) or in host memory (kept on the
 * GPU too, the core on WebGPU), the outputs' rows packed into out. false: no
 * adapter
 */
static bool tw_dev_run(const tw_dev_case_t *c, bool device, uint8_t *out)
{
    tw_dev_layout_t l;
    tw_dev_io_t io;
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t in;
    hdrg_session_output_t o;
    hdrg_device_frame_t in_frame, out_frame;
    hdrg_status_t status;
    hdrg_gpu_t *gpu;
    uint32_t sent, received, y;
    uint8_t *in_bytes, *out_bytes, p;
    size_t at;

    tw_dev_layout(c, &l);
    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_frames = HDRG_GPU_FRAMES_GPU;
    ctx->gpu_kind = HDRG_DEVICE_WEBGPU;

    if (!hdrg_gpu_create(&ctx->host.api, NULL, &gpu)) {
        hdrg_context_destroy(ctx);

        return false;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_device(ctx, HDRG_DEVICE_WEBGPU,
                                                           (void *)hdrg_gpu_device(gpu), NULL));

    in_bytes = (uint8_t *)calloc(1, l.in_size);
    out_bytes = (uint8_t *)calloc(1, l.out_size);
    TEST_ASSERT_TRUE(in_bytes != NULL && out_bytes != NULL);

    memset(&io, 0, sizeof(io));
    io.src = in_bytes;
    io.dst = out_bytes;
    io.in_size = l.in_size;
    io.out_size = l.out_size;
    TEST_ASSERT_TRUE(hdrg_gpu_call(gpu, tw_dev_make, &io) && io.ok);

    memset(&format, 0, sizeof(format));
    format.width = c->width;
    format.height = c->height;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = (size_t)c->width * 3u;

    hdrg_params_init(&params, c->preset);

    hdrg_display_caps_init(&caps);
    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    config.output.bits = 16;
    config.device = device ? HDRG_DEVICE_WEBGPU : 0u;

    if (c->yuv_out) {
        config.output.layout = HDRG_YUV_420;
        config.output.matrix = HDRG_MATRIX_BT2020;
        config.output.bits = 10;
        caps.gamut = HDRG_GAMUT_BT2020;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, &caps, &config, &session));

    memset(&in_frame, 0, sizeof(in_frame));
    memset(&out_frame, 0, sizeof(out_frame));

    for (p = 0; p < 3u; p++) {
        in_frame.handles[p] = (void *)io.in;
        in_frame.offsets[p] = l.in_at[p];
        in_frame.pitches[p] = (uint32_t)l.in_pitch[p];
        out_frame.handles[p] = (void *)io.out;
        out_frame.offsets[p] = l.out_at[p];
        out_frame.pitches[p] = (uint32_t)l.out_pitch[p];
    }

    sent = 0;
    received = 0;
    at = 0;

    for (;;) {
        memset(&o, 0, sizeof(o));
        o.device = device ? &out_frame : NULL;

        for (p = 0; !device && p < l.out_planes; p++) {
            o.planes[p] = out_bytes + l.out_at[p];
            o.strides[p] = l.out_pitch[p];
        }

        status = hdrg_session_receive_frame(session, &o);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.device = device ? &in_frame : NULL;
            in.pts = sent;

            for (p = 0; p < 3u; p++) {
                in.planes[p] = in_bytes + l.in_at[p];
                in.strides[p] = l.in_pitch[p];
            }

            if (sent < TW_DEV_N) {
                tw_dev_planes(c, &l, sent, in_bytes);
            }

            if (sent < TW_DEV_N && device) {
                TEST_ASSERT_TRUE(hdrg_gpu_call(gpu, tw_dev_write, &io) && io.ok);
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < TW_DEV_N) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

        if (device) {
            TEST_ASSERT_TRUE(hdrg_gpu_call(gpu, tw_dev_read, &io) && io.ok);
        }

        for (p = 0; p < l.out_planes; p++) {
            for (y = 0; y < l.out_rows[p]; y++) {
                memcpy(out + at, out_bytes + l.out_at[p] + (size_t)y * l.out_pitch[p],
                       l.out_row[p]);
                at += l.out_row[p];
            }
        }

        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TW_DEV_N, received);

    hdrg_session_destroy(session);
    (void)hdrg_gpu_call(gpu, tw_dev_free, &io);
    hdrg_gpu_destroy(gpu);
    hdrg_context_destroy(ctx);
    free(in_bytes);
    free(out_bytes);

    return true;
}

/* the app's WGPUBuffers in and out give what host memory gives on the same device */
static void test_device_frames(void)
{
    static const tw_dev_case_t cases[] = {
        {1280, 720, HDRG_PRESET_ANIME, true},
        {99, 67, HDRG_PRESET_PHOTO, false},
    };
    tw_dev_layout_t l;
    uint8_t *device, *host, i;
    size_t bytes;

    for (i = 0; i < 2u; i++) {
        tw_dev_layout(&cases[i], &l);
        bytes = (l.out_row[0] * l.out_rows[0] + l.out_row[1] * l.out_rows[1] +
                 l.out_row[2] * l.out_rows[2]) *
                TW_DEV_N;
        device = (uint8_t *)calloc(1, bytes);
        host = (uint8_t *)calloc(1, bytes);
        TEST_ASSERT_TRUE(device != NULL && host != NULL);

        if (!tw_dev_run(&cases[i], true, device)) {
            free(device);
            free(host);
            TEST_IGNORE_MESSAGE("no WebGPU adapter");
        }

        TEST_ASSERT_TRUE(tw_dev_run(&cases[i], false, host));
        TEST_ASSERT_EQUAL_MEMORY(host, device, bytes);

        free(device);
        free(host);
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
    g_alloc = hdrg_test_allocator();

    UNITY_BEGIN();
    RUN_TEST(test_deband);
    RUN_TEST(test_defringe);
    RUN_TEST(test_deblock);
    RUN_TEST(test_dering);
    RUN_TEST(test_denoise);
    RUN_TEST(test_lanczos);
    RUN_TEST(test_session);
    RUN_TEST(test_shared_device);
    RUN_TEST(test_device_frames);
#if defined(HDRG_TEST_GPU_MODULE)
    RUN_TEST(test_module_frames);
#endif

    return UNITY_END();
}
