/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if defined(TM_VULKAN) && defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* memfd_create */
#endif

#include <unity.h>

#if defined(TM_VULKAN)
#include <sys/mman.h>
#include <unistd.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#endif
#include <math.h>

#include "../test_gpu_module.h"
#include "../test_stage.h"
#include "../test_util.h"
#include "core/noise.h"
#if defined(TM_VULKAN)
#include "hdrgrading/gpu/vulkan.h"
#else
#include "hdrgrading/gpu/metal.h"
#endif
#include "hdrgrading/filter.h"
#include "hdrgrading/video.h"
#include "host/stage.h"
#include "priv.h"
#include "hdrgrading/foundation/hash.h"

/*
 * The Metal filters (HDRG_WITH_METAL): hdrg.metal.deband and
 * hdrg.metal.denoise against hdrg.deband and hdrg.denoise.spatial on the
 * same frames, within the tolerance a GPU is given (L2), measured and
 * printed, on the plugin's device and on the app's (the built-in debanding
 * and noise reduction under HDRG_GPU_FILTERS the Metal filters'); hdrg.metal.defringe,
 * hdrg.metal.deblock and hdrg.metal.dering equal to hdrg.defringe, hdrg.deblock and hdrg.dering
 * (the built-in deblocking and deringing under HDRG_GPU_FILTERS too, a frame handed from one to the
 * next); hdrg.metal.metalfx (MetalFX's spatial scaler) for what a scaler of its own must do.
 * A session given the app's MTLBuffers (hdrg_device_frame_t: padded pitches,
 * offsets, the planes in one buffer) or CVPixelBuffers (a decoder's NV12 and
 * P010, an encoder's P010) gives what one of host memory gives, bit for bit.
 * Without a Metal device (or MetalFX) the tests are ignored. Built with
 * TM_VULKAN (test_vulkan) the same tests run the Vulkan filters
 * (hdrg.vulkan.*, HDRG_WITH_VULKAN) on the plugin's device and on an app's
 * (a device of the library's own given as the app's), the app's VkBuffers
 * and dma-bufs (memfds lavapipe takes as dma-bufs) in place of MTLBuffers
 * and CVPixelBuffers, MetalFX's tests left out.
 */
#define TM_W       257u /* odd: the shader's last pixel of a row alone */
#define TM_H       131u
#define TM_KEY     UINT64_C(0x77656267707521)
#define TM_FRAMES  6u   /* the session's */
#define TM_UP_W    514u /* the scaler's: twice */
#define TM_UP_H    262u
#define TM_DEBAND  0u /* the built-in chains of tm_session: the debanding of strength 1 */
#define TM_DEBLOCK 1u /* the deblocking of strength 1 (tm_frames: the blocks frame) */
#define TM_NONE    2u /* none (tm_frames: the blocks frame) */
#define TM_DERING  3u /* the deblocking of strength 1, the deringing of 2 (tm_frames: the rings) */
#define TM_DENOISE 4u /* the noise reduction of strength 1 (tm_frames: the steps frame) */
#define TM_DEV_N   4u /* tm_dev_run's frames */
#define TM_PB_W    193u /* tm_pb_run's: odd */
#define TM_PB_H    109u
#if defined(TM_VULKAN)
#define TM_KIND      HDRG_DEVICE_VULKAN
#define TM_ID(name)  "hdrg.vulkan." name
#define TM_NO_DEVICE "no Vulkan device"
#else
#define TM_KIND      HDRG_DEVICE_METAL
#define TM_ID(name)  "hdrg.metal." name
#define TM_NO_DEVICE "no Metal device"
#endif

#if defined(TM_VULKAN)
typedef hdrg_vulkan_t tm_gpu_t;
typedef hdrg_vulkan_buffer_t tm_buffer_t;
#else
typedef hdrg_metal_t tm_gpu_t;
typedef hdrg_metal_buffer_t tm_buffer_t;
#endif

/* a session of the app's frames on its GPU: the size, the preset, the output */
typedef struct {
    uint32_t width, height;
    uint8_t preset;
    bool yuv_out; /* Y'CbCr 4:2:0 10-bit, else 16-bit R'G'B' */
} tm_dev_case_t;

/* tm_dev_run's planes: in (8-bit Y'CbCr 4:2:0) and out, offsets and pitches in bytes */
typedef struct {
    size_t in_at[3], in_pitch[3], out_at[3], out_pitch[3], out_row[3];
    size_t in_size, out_size;
    uint32_t out_rows[3];
    uint8_t out_planes;
} tm_dev_layout_t;

static hdrg_allocator_t g_alloc;
static bool g_ringing;  /* tm_run's noise side data says the frame rings (the defringing works) */
static bool g_blocky;   /* ... and that it has blocks (the deblocking works) */
static uint8_t g_chain; /* tm_session's built-in chain: TM_DEBAND, TM_DEBLOCK, ... */
static uint8_t g_gpu = HDRG_GPU_FILTERS; /* ... under the context's HDRG_GPU_* (the filters
                                         alone: test_gpu_apply has the apply pass) */
static uint8_t g_latency;                /* tm_session's HDRG_LATENCY_* (PLAYBACK) */
static const char *g_next;               /* tm_session's PREFILTER filter after id (NULL: none) */
static const char *g_taps;               /* tm_run's scaler options (NULL: the defaults) */
static const char *g_antiring;
#if defined(HDRG_TEST_GPU_MODULE)
static const char *g_between; /* tm_session's module filter before g_next (NULL: none) */
#endif
static uint32_t g_dev_kind = HDRG_FRAME_BUFFERS; /* tm_dev_run's frames' */
static void *g_app_queue;                        /* tm_app_device's queue */
#if defined(TM_VULKAN)
static hdrg_context_t *g_app_holder; /* tm_app_device's device's table */
static hdrg_vulkan_t *g_app;
#else
static uint32_t g_pb_device = HDRG_DEVICE_METAL; /* tm_pb_run's session's device */

/* MTLCreateSystemDefaultDevice from C: an id<MTLDevice> with a reference of the caller's */
extern void *MTLCreateSystemDefaultDevice(void);
#endif

/* a device of the app's (Vulkan: the library's own on a table of its own) and its queue */
static void *tm_app_device(void)
{
#if defined(TM_VULKAN)
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&g_app_holder, NULL, 0));
    TEST_ASSERT_TRUE(hdrg_vulkan_create(&g_app_holder->host.api, NULL, NULL, &g_app));
    g_app_queue = hdrg_vulkan_queue(g_app);

    return (void *)hdrg_vulkan_device(g_app);
#else
    g_app_queue = NULL;

    return MTLCreateSystemDefaultDevice();
#endif
}

static void tm_app_release(void *device)
{
#if defined(TM_VULKAN)
    (void)device;
    hdrg_vulkan_destroy(g_app);
    hdrg_context_destroy(g_app_holder);
    g_app = NULL;
    g_app_holder = NULL;
#else
    CFRelease(device);
#endif
}

/* the GPU layer's device on ctx's table (false: none), buffers of host memory it reads in place */
static bool tm_gpu_create(hdrg_context_t *ctx, tm_gpu_t **gpu)
{
#if defined(TM_VULKAN)
    return hdrg_vulkan_create(&ctx->host.api, NULL, NULL, gpu);
#else
    return hdrg_metal_create(&ctx->host.api, NULL, NULL, gpu);
#endif
}

static hdrg_status_t tm_gpu_set(hdrg_context_t *ctx, tm_gpu_t *gpu)
{
#if defined(TM_VULKAN)
    return hdrg_context_set_device(ctx, TM_KIND, (void *)hdrg_vulkan_device(gpu),
                                   hdrg_vulkan_queue(gpu));
#else
    return hdrg_context_set_device(ctx, TM_KIND, hdrg_metal_device_object(gpu),
                                   hdrg_metal_queue_object(gpu));
#endif
}

static tm_buffer_t *tm_gpu_buffer(tm_gpu_t *gpu, size_t size, uint8_t **bytes)
{
#if defined(TM_VULKAN)
    return hdrg_vulkan_host_buffer(gpu, size, bytes);
#else
    return hdrg_metal_host_buffer(gpu, size, bytes);
#endif
}

static void *tm_gpu_object(const tm_buffer_t *buffer)
{
#if defined(TM_VULKAN)
    return hdrg_vulkan_buffer_object(buffer);
#else
    return hdrg_metal_buffer_object(buffer);
#endif
}

static void tm_gpu_free(tm_gpu_t *gpu, tm_buffer_t *buffer)
{
#if defined(TM_VULKAN)
    hdrg_vulkan_buffer_free(buffer);

    if (gpu != NULL) {
        hdrg_vulkan_destroy(gpu);
    }
#else
    hdrg_metal_buffer_free(buffer);

    if (gpu != NULL) {
        hdrg_metal_destroy(gpu);
    }
#endif
}

/*
 * id over pixels (RGB16, TM_W x TM_H) as a stage (PREFILTER, or SCALE to
 * out_w x out_h) of the whole frame with the frame's noise, into out, on
 * the app's device when device is not NULL; HDRG_ERR_UNSUPPORTED when the
 * filter has no device
 */
static hdrg_status_t tm_run(const char *id, uint32_t stage_kind, const uint16_t *pixels,
                            const char *strength, uint32_t out_w, uint32_t out_h, void *device,
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
    float data[HDRG_SD_NOISE_FLOATS];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_device(ctx, TM_KIND, device,
                                                           (device != NULL) ? g_app_queue : NULL));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)pixels;
    raw.width = TM_W;
    raw.height = TM_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TM_W * 6u;
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
    entry.stage = stage_kind;
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
static void tm_frame(uint16_t *pixels)
{
    uint32_t x, y;
    uint8_t c;
    double v, u1, u2, n;

    for (y = 0; y < TM_H; y++) {
        for (x = 0; x < TM_W; x++) {
            u1 = (double)hdrg_hash_uniform(TM_KEY, x, y, 0u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TM_KEY, x, y, 1u);
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

                pixels[((size_t)y * TM_W + x) * 3u + c] =
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
static void tm_blocks(uint16_t *pixels)
{
    uint32_t x, y;
    uint8_t c;
    double v, u1, u2, n, offset;

    for (y = 0; y < TM_H; y++) {
        for (x = 0; x < TM_W; x++) {
            u1 = (double)hdrg_hash_uniform(TM_KEY, x, y, 0u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TM_KEY, x, y, 1u);
            n = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
            offset = 6.0 * ((double)hdrg_hash_uniform(TM_KEY, x / 8u, y / 8u, 2u) - 0.5);

            for (c = 0; c < 3u; c++) {
                v = 70.0 + 0.3 * (double)x + 0.2 * (double)y + 25.0 * (double)c + offset + 0.5 * n;

                if (x >= 170u && x < 210u) {
                    v = (x < 190u) ? 200.0 - 20.0 * c : 40.0 + 10.0 * c;
                }

                if (x == 80u || x == 81u) {
                    v = 10.0;
                }

                pixels[((size_t)y * TM_W + x) * 3u + c] =
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
static void tm_rings(uint16_t *pixels)
{
    uint32_t x, y, k;
    uint16_t *s;
    uint8_t c;
    int32_t swing;

    tm_blocks(pixels);

    for (y = 0; y < TM_H; y++) {
        for (x = 0; x < TM_W; x++) {
            k = x % 32u;
            s = pixels + ((size_t)y * TM_W + x) * 3u;
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
static void tm_image(uint16_t *pixels)
{
    uint8_t *image;
    size_t i;

    image = hdrg_test_image(TM_W, TM_H);
    TEST_ASSERT_NOT_NULL(image);

    for (i = 0; i < (size_t)TM_W * TM_H * 3u; i++) {
        pixels[i] = (uint16_t)(image[i] * 257u);
    }

    free(image);
}

/*
 * hdrg.metal.deband against hdrg.deband: the samples the CPU filter changes
 * (enough of them to show it works), the GPU's largest difference from the
 * CPU's and the samples apart by more than 2 (16-bit) codes, on the steps
 * frame and the test image; the app's device gives the plugin's output;
 * strength 0 passes on both
 */
static void test_deband(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u;
    uint32_t round, worst, apart, changed;
    uint16_t *in, *cpu, *gpu, *app;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[160];
    void *device;

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    app = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL && app != NULL);

    tm_frame(in);

    if (tm_run(TM_ID("deband"), HDRG_STAGE_PREFILTER, in, "1", TM_W, TM_H, NULL, gpu, &gpu_pass) ==
        HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        free(app);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_image(in);
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.deband", HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                              TM_H, NULL, cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deband"), HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                              TM_H, NULL, gpu, &gpu_pass));
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
        /* a window whose residual sits at a threshold may take the other branch on another GPU's
           f32 (16 codes where the windows' choices agree: Apple's GPUs, lavapipe), and its samples
           move by the residual: within 4 8-bit codes, 2% of the samples */
        TEST_ASSERT_TRUE(worst <= 4u * 257u);
        TEST_ASSERT_TRUE((double)apart <= 0.02 * (double)samples);
    }

    device = tm_app_device();
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deband"), HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                          TM_H, device, app, &gpu_pass));
    tm_app_release(device);
    TEST_ASSERT_EQUAL_MEMORY(gpu, app, samples * sizeof(uint16_t));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deband"), HDRG_STAGE_PREFILTER, in, "0", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
    free(app);
}

/*
 * hdrg.metal.defringe against hdrg.defringe on frames said to ring: the same
 * bytes (the same integers), on the test image samples changed (the steps
 * frame has no chroma to defringe); without the ringing and with strength 0
 * it passes
 */
static void test_defringe(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u;
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tm_image(in);
    g_ringing = true;

    if (tm_run(TM_ID("defringe"), HDRG_STAGE_PREFILTER, in, "3", TM_W, TM_H, NULL, gpu,
               &gpu_pass) == HDRG_ERR_UNSUPPORTED) {
        g_ringing = false;
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_frame(in);
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.defringe", HDRG_STAGE_PREFILTER, in, "3", TM_W,
                                              TM_H, NULL, cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("defringe"), HDRG_STAGE_PREFILTER, in, "3",
                                              TM_W, TM_H, NULL, gpu, &gpu_pass));
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

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("defringe"), HDRG_STAGE_PREFILTER, in, "0", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_ringing = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("defringe"), HDRG_STAGE_PREFILTER, in, "3", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.metal.deblock against hdrg.deblock: the same bytes (the same
 * integers) on the blocks frame (its own noise: blocks found, samples
 * changed) and on the test image said to have blocks; the app's device gives
 * the same; with strength 0 it passes
 */
static void test_deblock(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u;
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];
    void *device;

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tm_blocks(in);

    if (tm_run(TM_ID("deblock"), HDRG_STAGE_PREFILTER, in, "1", TM_W, TM_H, NULL, gpu, &gpu_pass) ==
        HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_image(in);
            g_blocky = true;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.deblock", HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                              TM_H, NULL, cpu, &cpu_pass));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deblock"), HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                              TM_H, NULL, gpu, &gpu_pass));
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

    device = tm_app_device();
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deblock"), HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                          TM_H, device, gpu, &gpu_pass));
    tm_app_release(device);
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("deblock"), HDRG_STAGE_PREFILTER, in, "0", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_blocky = false;

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.metal.denoise against hdrg.denoise.spatial on the steps frame and the
 * blocks frame (their own noise) at strengths 1 and 3: the samples the CPU
 * filter changes (enough of them to show it works), the GPU's largest
 * difference from the CPU's and the samples apart by more than 2 (16-bit)
 * codes; the app's device gives the plugin's output; strength 0 passes
 */
static void test_denoise(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u;
    const char *const strengths[2] = {"1", "3"};
    uint32_t round, worst, apart, changed;
    uint16_t *in, *cpu, *gpu, *app;
    uint8_t k;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[160];
    void *device;

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    app = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL && app != NULL);

    tm_frame(in);

    if (tm_run(TM_ID("denoise"), HDRG_STAGE_PREFILTER, in, "1", TM_W, TM_H, NULL, gpu, &gpu_pass) ==
        HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        free(app);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_blocks(in);
        }

        for (k = 0; k < 2u; k++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.denoise.spatial", HDRG_STAGE_PREFILTER, in,
                                                  strengths[k], TM_W, TM_H, NULL, cpu, &cpu_pass));
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("denoise"), HDRG_STAGE_PREFILTER, in,
                                                  strengths[k], TM_W, TM_H, NULL, gpu, &gpu_pass));
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

    device = tm_app_device();
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("denoise"), HDRG_STAGE_PREFILTER, in, "3", TM_W,
                                          TM_H, device, app, &gpu_pass));
    tm_app_release(device);
    TEST_ASSERT_EQUAL_MEMORY(gpu, app, samples * sizeof(uint16_t));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("denoise"), HDRG_STAGE_PREFILTER, in, "0", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
    free(app);
}

/*
 * hdrg.metal.dering against hdrg.dering on frames said to ring: the same
 * bytes (the same integers) at strengths 1, 2 and 3 (windows of radius 3, 5
 * and 7) on the test image and the steps frame, samples changed; the app's
 * device gives the same; with strength 0 and without the ringing it passes
 */
static void test_dering(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u;
    const char *const strengths[3] = {"1", "2", "3"};
    uint32_t round, changed;
    uint16_t *in, *cpu, *gpu;
    uint8_t k;
    size_t i;
    bool cpu_pass, gpu_pass;
    char line[128];
    void *device;

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tm_image(in);
    g_ringing = true;

    if (tm_run(TM_ID("dering"), HDRG_STAGE_PREFILTER, in, "1", TM_W, TM_H, NULL, gpu, &gpu_pass) ==
        HDRG_ERR_UNSUPPORTED) {
        g_ringing = false;
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_frame(in);
        }

        for (k = 0; k < 3u; k++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.dering", HDRG_STAGE_PREFILTER, in,
                                                  strengths[k], TM_W, TM_H, NULL, cpu, &cpu_pass));
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("dering"), HDRG_STAGE_PREFILTER, in,
                                                  strengths[k], TM_W, TM_H, NULL, gpu, &gpu_pass));
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

    device = tm_app_device();
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("dering"), HDRG_STAGE_PREFILTER, in, "3", TM_W,
                                          TM_H, device, gpu, &gpu_pass));
    tm_app_release(device);
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("dering"), HDRG_STAGE_PREFILTER, in, "0", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    g_ringing = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("dering"), HDRG_STAGE_PREFILTER, in, "1", TM_W,
                                          TM_H, NULL, gpu, &gpu_pass));
    TEST_ASSERT_TRUE(gpu_pass);
    TEST_ASSERT_EQUAL_MEMORY(in, gpu, samples * sizeof(uint16_t));

    free(in);
    free(cpu);
    free(gpu);
}

/*
 * TM_FRAMES frames of the steps frame moving a column a frame through a
 * session with the filter id at stage_kind (SCALE: to TM_UP_W x TM_UP_H),
 * into out (16-bit PQ, TM_FRAMES frames); HDRG_ERR_UNSUPPORTED when it has
 * no device
 */
static hdrg_status_t tm_session(const char *id, uint32_t stage_kind, const uint16_t *frames,
                                uint16_t *out, uint32_t *latency)
{
    const size_t frame_samples = (size_t)TM_W * TM_H * 3u;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_context_desc_t desc;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_options_t *options;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, got, out_w, out_h;

    out_w = (stage_kind == HDRG_STAGE_SCALE) ? TM_UP_W : TM_W;
    out_h = (stage_kind == HDRG_STAGE_SCALE) ? TM_UP_H : TM_H;

    /* no id: the built-in chain g_chain under g_gpu */
    if (id == NULL) {
        hdrg_context_desc_init(&desc);
        desc.allocator = &g_alloc;
        desc.gpu = g_gpu;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    } else {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

        if (stage_kind == HDRG_STAGE_PREFILTER) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "strength", "1"));
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, stage_kind, id, options));

#if defined(HDRG_TEST_GPU_MODULE)
        if (g_between != NULL) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_load_plugin(ctx, HDRG_TEST_GPU_MODULE));
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, g_between, NULL));
        }
#endif

        if (g_next != NULL) {
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, g_next, options));
        }

        hdrg_options_destroy(options);
    }

    memset(&format, 0, sizeof(format));
    format.width = TM_W;
    format.height = TM_H;
    format.bit_depth = 16;
    format.stride = (size_t)TM_W * 6u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    params.mctf_strength = 0.0f; /* the filters under test alone */
    params.grain_policy = HDRG_GRAIN_PRESERVE;
    params.deblock_strength = (g_chain == TM_DEBLOCK || g_chain == TM_DERING) ? 1.0f : 0.0f;
    params.dering_strength = (g_chain == TM_DERING) ? 2.0f : 0.0f;
    params.deband_strength = (g_chain == TM_DEBAND) ? 1.0f : 0.0f;
    params.grain_policy = (g_chain == TM_DENOISE) ? HDRG_GRAIN_REDUCE : HDRG_GRAIN_PRESERVE;
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.proxy = 0;
    config.future = 0; /* the filters' delays alone */
    config.latency = g_latency;
    config.output_width = out_w;
    config.output_height = out_h;

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
        info.planes[0] = (uint8_t *)(out + (size_t)out_w * out_h * 3u * got);
        info.strides[0] = (size_t)out_w * 6u;

        status = hdrg_session_receive_frame(session, &info);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = (const uint8_t *)(const void *)(frames + frame_samples * sent);
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TM_FRAMES) ? &in : NULL));
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
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, got);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return HDRG_OK;
}

/*
 * the moving frames of the sessions: the steps frame a column further each
 * frame (g_chain other than TM_DEBAND and TM_DENOISE: the blocks frame, the
 * rings for TM_DERING, a block further, its blocks on the grid)
 */
static uint16_t *tm_frames(void)
{
    const size_t frame_samples = (size_t)TM_W * TM_H * 3u;
    uint32_t t, x, y;
    uint16_t *still, *frames;

    still = (uint16_t *)malloc(frame_samples * sizeof(uint16_t));
    frames = (uint16_t *)malloc(frame_samples * TM_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(still != NULL && frames != NULL);

    if (g_chain == TM_DERING) {
        tm_rings(still);
    } else if (g_chain != TM_DEBAND && g_chain != TM_DENOISE) {
        tm_blocks(still);
    } else {
        tm_frame(still);
    }

    for (t = 0; t < TM_FRAMES; t++) {
        for (y = 0; y < TM_H; y++) {
            for (x = 0; x < TM_W; x++) {
                memcpy(frames + frame_samples * t + ((size_t)y * TM_W + x) * 3u,
                       still +
                           ((size_t)y * TM_W +
                            (x + ((g_chain != TM_DEBAND && g_chain != TM_DENOISE) ? 8u : 1u) * t) %
                                TM_W) *
                               3u,
                       3u * sizeof(uint16_t));
            }
        }
    }

    free(still);

    return frames;
}

/*
 * A video session with hdrg.metal.deband (ASYNC, two frames in flight: a
 * frame of latency) against one with hdrg.deband: every frame in order,
 * each within the tolerance in 16-bit PQ codes (printed); the built-in
 * debanding under the context's HDRG_GPU_FILTERS is the Metal filter's, the
 * built-in deblocking the same bytes as the CPU's with its frame of latency,
 * and the deblocking and deringing in a row too (the frame handed from one
 * to the next, a frame of latency each; in LOW kept on the device between
 * them, no latency), the deringing at work
 */
static void test_session(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u * TM_FRAMES;
    uint32_t latency, cpu_latency, worst, apart;
    uint16_t *frames, *cpu, *gpu;
    size_t i;
    char line[160];

    frames = tm_frames();
    cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(cpu != NULL && gpu != NULL);

    if (tm_session(TM_ID("deband"), HDRG_STAGE_PREFILTER, frames, gpu, &latency) ==
        HDRG_ERR_UNSUPPORTED) {
        free(frames);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tm_session("hdrg.deband", HDRG_STAGE_PREFILTER, frames, cpu, &cpu_latency));
    worst = 0;
    apart = 0;

    for (i = 0; i < samples; i++) {
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
    TEST_ASSERT_TRUE((double)apart <= 0.001 * (double)samples);

    /* the context's HDRG_GPU_FILTERS: the built-in debanding is hdrg.metal.deband's */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, cpu, &latency));
    TEST_ASSERT_EQUAL_MEMORY(gpu, cpu, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 1u, latency);
    free(frames);

    /* ... and the built-in noise reduction hdrg.metal.denoise's */
    g_chain = TM_DENOISE;
    frames = tm_frames();
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tm_session(TM_ID("denoise"), HDRG_STAGE_PREFILTER, frames, gpu, &cpu_latency));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, cpu, &latency));
    TEST_ASSERT_EQUAL_MEMORY(gpu, cpu, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency, latency);
    free(frames);

    /* ... and the built-in deblocking hdrg.metal.deblock's: the CPU's bytes, apart from none */
    g_chain = TM_DEBLOCK;
    frames = tm_frames();
    g_gpu = HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tm_session(NULL, HDRG_STAGE_PREFILTER, frames, cpu, &cpu_latency));
    g_gpu = HDRG_GPU_FILTERS;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, gpu, &latency));
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 1u, latency);
    g_chain = TM_NONE;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, gpu, &latency));
    g_chain = TM_DEBAND;
    apart = 0;

    for (i = 0; i < samples; i++) {
        apart += (cpu[i] != gpu[i]) ? 1u : 0u;
    }

    TEST_ASSERT_TRUE(apart > samples / 20u);
    free(frames);

    /* ... the deblocking and the deringing in a row, both on the GPU */
    g_chain = TM_DERING;
    frames = tm_frames();
    g_gpu = HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tm_session(NULL, HDRG_STAGE_PREFILTER, frames, cpu, &cpu_latency));
    g_gpu = HDRG_GPU_FILTERS;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, gpu, &latency));
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 2u, latency);

    /* ... in LOW too (a frame in flight at most: the frame kept on the device between them) */
    g_latency = HDRG_LATENCY_LOW;
    g_gpu = HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tm_session(NULL, HDRG_STAGE_PREFILTER, frames, cpu, &cpu_latency));
    g_gpu = HDRG_GPU_FILTERS;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, gpu, &latency));
    g_latency = HDRG_LATENCY_PLAYBACK;
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency, latency);
    g_chain = TM_DEBLOCK;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_PREFILTER, frames, gpu, &latency));
    g_chain = TM_DEBAND;
    apart = 0;

    for (i = 0; i < samples; i++) {
        apart += (cpu[i] != gpu[i]) ? 1u : 0u;
    }

    snprintf(line, sizeof(line), "session: the deringing changes %.1f%% of the samples",
             100.0 * (double)apart / (double)samples);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(apart > samples / 100u);

    free(frames);
    free(cpu);
    free(gpu);
}

/*
 * hdrg.metal.lanczos against hdrg.scale.lanczos: the same bytes (the CPU's
 * tables and integers) on the test image and the steps frame, upscaled 2x
 * and by 1.6 / 1.5 and downscaled by 2 and 2.57 / 2.62, at 2, 3 and 4 lobes
 * with and without anti-ringing; the app's device gives the same. A
 * session's SCALE under HDRG_GPU_FILTERS is hdrg.metal.lanczos's: the CPU's
 * bytes with its frame of latency
 */
static void test_lanczos(void)
{
    const uint32_t sizes[4][2] = {{TM_UP_W, TM_UP_H}, {411, 197}, {128, 65}, {100, 50}};
    const size_t in_samples = (size_t)TM_W * TM_H * 3u;
    const char *const lobes[3] = {"2", "3", "4"}, *const antirings[2] = {"1", "0"};
    uint32_t round, s, k, a, latency, cpu_latency;
    uint16_t *in, *cpu, *gpu, *frames;
    size_t samples;
    bool passthrough;
    void *device;

    in = (uint16_t *)malloc(in_samples * sizeof(uint16_t));
    cpu = (uint16_t *)malloc((size_t)TM_UP_W * TM_UP_H * 3u * TM_FRAMES * sizeof(uint16_t));
    gpu = (uint16_t *)malloc((size_t)TM_UP_W * TM_UP_H * 3u * TM_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && cpu != NULL && gpu != NULL);

    tm_image(in);

    if (tm_run(TM_ID("lanczos"), HDRG_STAGE_SCALE, in, NULL, TM_UP_W, TM_UP_H, NULL, gpu,
               &passthrough) == HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(cpu);
        free(gpu);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    for (round = 0; round < 2u; round++) {
        if (round == 1u) {
            tm_frame(in);
        }

        for (s = 0; s < 4u; s++) {
            samples = (size_t)sizes[s][0] * sizes[s][1] * 3u;

            for (k = 0; k < 3u; k++) {
                for (a = 0; a < 2u; a++) {
                    g_taps = lobes[k];
                    g_antiring = antirings[a];
                    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.scale.lanczos", HDRG_STAGE_SCALE,
                                                          in, NULL, sizes[s][0], sizes[s][1], NULL,
                                                          cpu, &passthrough));
                    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("lanczos"), HDRG_STAGE_SCALE, in,
                                                          NULL, sizes[s][0], sizes[s][1], NULL, gpu,
                                                          &passthrough));
                    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
                }
            }
        }
    }

    g_taps = NULL;
    g_antiring = NULL;
    device = tm_app_device();
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run(TM_ID("lanczos"), HDRG_STAGE_SCALE, in, NULL, 411, 197,
                                          device, gpu, &passthrough));
    tm_app_release(device);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.scale.lanczos", HDRG_STAGE_SCALE, in, NULL, 411,
                                          197, NULL, cpu, &passthrough));
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, (size_t)411 * 197 * 3u * sizeof(uint16_t));

    /* the built-in scaler of a session under HDRG_GPU_FILTERS, twice the size */
    g_chain = TM_NONE;
    frames = tm_frames();
    g_gpu = HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_SCALE, frames, cpu, &cpu_latency));
    g_gpu = HDRG_GPU_FILTERS;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session(NULL, HDRG_STAGE_SCALE, frames, gpu, &latency));
    g_chain = TM_DEBAND;
    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu,
                             (size_t)TM_UP_W * TM_UP_H * 3u * TM_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(cpu_latency + 1u, latency);

    free(in);
    free(cpu);
    free(gpu);
    free(frames);
}

#if !defined(TM_VULKAN)
/*
 * hdrg.metal.metalfx, twice the size: the same output for the same frame,
 * a flat frame flat (within 128 codes: the scaler works in half floats,
 * truncating, so white comes out 65471), near the built-in Lanczos (PSNR,
 * printed: a scaler of its own, it sharpens) but not equal to it; no
 * downscaling; the frames of a session in order with the flight's frame of
 * latency
 */
static void test_metalfx(void)
{
    const size_t in_samples = (size_t)TM_W * TM_H * 3u, up_samples = (size_t)TM_UP_W * TM_UP_H * 3u;
    uint32_t worst, latency, lanczos_latency;
    uint16_t *in, *fx, *again, *lanczos, *frames, *session, *reference;
    size_t i;
    bool passthrough;
    char line[160];
    double error, psnr;

    in = (uint16_t *)malloc(in_samples * sizeof(uint16_t));
    fx = (uint16_t *)malloc(up_samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(up_samples * sizeof(uint16_t));
    lanczos = (uint16_t *)malloc(up_samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && fx != NULL && again != NULL && lanczos != NULL);

    tm_image(in);

    if (tm_run("hdrg.metal.metalfx", HDRG_STAGE_SCALE, in, NULL, TM_UP_W, TM_UP_H, NULL, fx,
               &passthrough) == HDRG_ERR_UNSUPPORTED) {
        free(in);
        free(fx);
        free(again);
        free(lanczos);
        TEST_IGNORE_MESSAGE("no MetalFX spatial scaler");
    }

    TEST_ASSERT_FALSE(passthrough);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.metal.metalfx", HDRG_STAGE_SCALE, in, NULL, TM_UP_W,
                                          TM_UP_H, NULL, again, &passthrough));
    TEST_ASSERT_EQUAL_MEMORY(fx, again, up_samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.scale.lanczos", HDRG_STAGE_SCALE, in, NULL, TM_UP_W,
                                          TM_UP_H, NULL, lanczos, &passthrough));
    error = 0.0;
    worst = 0;

    for (i = 0; i < up_samples; i++) {
        error += ((double)fx[i] - (double)lanczos[i]) * ((double)fx[i] - (double)lanczos[i]);
        worst = HDRG_MAX(worst, (uint32_t)abs((int32_t)fx[i] - (int32_t)lanczos[i]));
    }

    psnr = 10.0 * log10(65535.0 * 65535.0 / (error / (double)up_samples));
    snprintf(line, sizeof(line),
             "metalfx: %.1f dB from the built-in Lanczos (16-bit), at most %u codes apart", psnr,
             worst);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(psnr >= 30.0);
    TEST_ASSERT_TRUE(worst > 0u);

    /* a flat frame stays flat */
    for (i = 0; i < in_samples; i++) {
        in[i] = (uint16_t)(20000u + 5000u * (i % 3u));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_run("hdrg.metal.metalfx", HDRG_STAGE_SCALE, in, NULL, TM_UP_W,
                                          TM_UP_H, NULL, fx, &passthrough));
    worst = 0;

    for (i = 0; i < up_samples; i++) {
        worst = HDRG_MAX(worst, (uint32_t)abs((int32_t)fx[i] - (int32_t)in[i % 3u]));
    }

    snprintf(line, sizeof(line), "metalfx: a flat frame at most %u codes off", worst);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(worst <= 128u);

    TEST_ASSERT_NOT_EQUAL_INT(HDRG_OK, tm_run("hdrg.metal.metalfx", HDRG_STAGE_SCALE, in, NULL,
                                              TM_W / 2u, TM_H / 2u, NULL, fx, &passthrough));

    frames = tm_frames();
    session = (uint16_t *)malloc(up_samples * TM_FRAMES * sizeof(uint16_t));
    reference = (uint16_t *)malloc(up_samples * TM_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(session != NULL && reference != NULL);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tm_session("hdrg.metal.metalfx", HDRG_STAGE_SCALE, frames, session, &latency));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_session("hdrg.scale.lanczos", HDRG_STAGE_SCALE, frames,
                                              reference, &lanczos_latency));
    TEST_ASSERT_EQUAL_UINT32(lanczos_latency + 1u, latency);

    free(in);
    free(fx);
    free(again);
    free(lanczos);
    free(frames);
    free(session);
    free(reference);
}
#endif

/* the planes of a case: whole words, padded pitches, the planes after offsets */
static void tm_dev_layout(const tm_dev_case_t *c, tm_dev_layout_t *l)
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
static void tm_dev_planes(const tm_dev_case_t *c, const tm_dev_layout_t *l, uint32_t t, uint8_t *in)
{
    const uint32_t cw = (c->width + 1u) / 2u, ch = (c->height + 1u) / 2u;
    uint32_t x, y, h;

    for (y = 0; y < c->height; y++) {
        for (x = 0; x < c->width; x++) {
            h = hdrg_hash_u32(TM_KEY, x, y, t);
            in[l->in_at[0] + y * l->in_pitch[0] + x] =
                (uint8_t)(16u + ((x * 3u + y + t * 7u) % 200u) + h % 9u);
        }
    }

    for (y = 0; y < ch; y++) {
        for (x = 0; x < cw; x++) {
            h = hdrg_hash_u32(TM_KEY, x, y, t + 100u);
            in[l->in_at[1] + y * l->in_pitch[1] + x] =
                (uint8_t)(108u + ((x + t) / 8u % 40u) + h % 5u);
            in[l->in_at[2] + y * l->in_pitch[2] + x] =
                (uint8_t)(98u + ((y * 2u) / 8u % 50u) + h % 3u);
        }
    }
}

/*
 * The case's session on the app's Metal device: its frames as the app's
 * MTLBuffers (device) or in host memory (kept on the GPU too), the outputs'
 * rows packed into out. false: no Metal device
 */
static bool tm_dev_run(const tm_dev_case_t *c, bool device, uint8_t *out)
{
    tm_dev_layout_t l;
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
    tm_gpu_t *gpu;
    tm_buffer_t *in_buffer, *out_buffer;
    uint32_t sent, received, y;
    uint8_t *in_bytes, *out_bytes, *host_in, *host_out, p;
    int in_fd, out_fd;
    size_t at;
    bool dmabuf;

    tm_dev_layout(c, &l);
    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_frames = HDRG_GPU_FRAMES_GPU;

    dmabuf = g_dev_kind == HDRG_FRAME_DMABUF;
    in_fd = -1;
    out_fd = -1;

    if (!tm_gpu_create(ctx, &gpu)) {
        hdrg_context_destroy(ctx);

        return false;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_gpu_set(ctx, gpu));

    in_buffer = tm_gpu_buffer(gpu, l.in_size, &in_bytes);
    out_buffer = tm_gpu_buffer(gpu, l.out_size, &out_bytes);
    host_in = (uint8_t *)calloc(1, l.in_size);
    host_out = (uint8_t *)calloc(1, l.out_size);
    TEST_ASSERT_TRUE(in_buffer != NULL && out_buffer != NULL && host_in != NULL &&
                     host_out != NULL);

#if defined(TM_VULKAN)
    /* dma-bufs: memfds of the planes (lavapipe maps them as dma-bufs) */
    if (dmabuf) {
        in_fd = memfd_create("tm_in", MFD_CLOEXEC);
        out_fd = memfd_create("tm_out", MFD_CLOEXEC);
        TEST_ASSERT_TRUE(in_fd >= 0 && out_fd >= 0);
        TEST_ASSERT_EQUAL_INT(0, ftruncate(in_fd, (off_t)l.in_size));
        TEST_ASSERT_EQUAL_INT(0, ftruncate(out_fd, (off_t)l.out_size));
        in_bytes = (uint8_t *)mmap(NULL, l.in_size, PROT_READ | PROT_WRITE, MAP_SHARED, in_fd, 0);
        out_bytes =
            (uint8_t *)mmap(NULL, l.out_size, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
        TEST_ASSERT_TRUE(in_bytes != MAP_FAILED && out_bytes != MAP_FAILED);
    }
#endif

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
    config.device = device ? TM_KIND : 0u;

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

    in_frame.kind = g_dev_kind;
    out_frame.kind = g_dev_kind;

    for (p = 0; p < 3u; p++) {
        in_frame.handles[p] = dmabuf ? (void *)(intptr_t)in_fd : tm_gpu_object(in_buffer);
        in_frame.offsets[p] = l.in_at[p];
        in_frame.sizes[p] = (TM_KIND == HDRG_DEVICE_VULKAN) ? l.in_size : 0u;
        in_frame.pitches[p] = (uint32_t)l.in_pitch[p];
        out_frame.handles[p] = dmabuf ? (void *)(intptr_t)out_fd : tm_gpu_object(out_buffer);
        out_frame.offsets[p] = l.out_at[p];
        out_frame.sizes[p] = (TM_KIND == HDRG_DEVICE_VULKAN) ? l.out_size : 0u;
        out_frame.pitches[p] = (uint32_t)l.out_pitch[p];
    }

    sent = 0;
    received = 0;
    at = 0;

    for (;;) {
        memset(&o, 0, sizeof(o));
        o.device = device ? &out_frame : NULL;

        for (p = 0; !device && p < l.out_planes; p++) {
            o.planes[p] = host_out + l.out_at[p];
            o.strides[p] = l.out_pitch[p];
        }

        status = hdrg_session_receive_frame(session, &o);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.device = device ? &in_frame : NULL;
            in.pts = sent;

            for (p = 0; p < 3u; p++) {
                in.planes[p] = host_in + l.in_at[p];
                in.strides[p] = l.in_pitch[p];
            }

            if (sent < TM_DEV_N) {
                tm_dev_planes(c, &l, sent, device ? in_bytes : host_in);
            }

            status = hdrg_session_send_frame(session, (sent < TM_DEV_N) ? &in : NULL);

            /* a device taking no dma-bufs */
            if (status == HDRG_ERR_UNSUPPORTED && dmabuf && sent == 0) {
                break;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
            sent++;

            continue;
        }

        /* a device taking no dma-bufs */
        if (status == HDRG_EOF || (status == HDRG_ERR_UNSUPPORTED && dmabuf && received == 0)) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

        for (p = 0; p < l.out_planes; p++) {
            for (y = 0; y < l.out_rows[p]; y++) {
                memcpy(out + at,
                       (device ? out_bytes : host_out) + l.out_at[p] + (size_t)y * l.out_pitch[p],
                       l.out_row[p]);
                at += l.out_row[p];
            }
        }

        received++;
    }

    TEST_ASSERT_TRUE(received == TM_DEV_N || (dmabuf && received == 0));

    hdrg_session_destroy(session);
    tm_gpu_free(NULL, in_buffer);
    tm_gpu_free(gpu, out_buffer);
    hdrg_context_destroy(ctx);
    free(host_in);
    free(host_out);

#if defined(TM_VULKAN)
    if (dmabuf) {
        (void)munmap(in_bytes, l.in_size);
        (void)munmap(out_bytes, l.out_size);
        (void)close(in_fd);
        (void)close(out_fd);
    }
#endif

    return received == TM_DEV_N;
}

/*
 * the app's buffers (MTLBuffers, VkBuffers, dma-bufs) in and out give what
 * host memory gives; frames not of whole words fail
 */
static void test_device_frames(void)
{
    static const tm_dev_case_t cases[] = {
        {1280, 720, HDRG_PRESET_ANIME, true},
        {99, 67, HDRG_PRESET_PHOTO, false},
    };
    tm_dev_layout_t l;
    uint8_t *device, *host, i;
    size_t bytes;

    for (i = 0; i < 2u; i++) {
        tm_dev_layout(&cases[i], &l);
        bytes = (l.out_row[0] * l.out_rows[0] + l.out_row[1] * l.out_rows[1] +
                 l.out_row[2] * l.out_rows[2]) *
                TM_DEV_N;
        device = (uint8_t *)calloc(1, bytes);
        host = (uint8_t *)calloc(1, bytes);
        TEST_ASSERT_TRUE(device != NULL && host != NULL);

        if (!tm_dev_run(&cases[i], true, device)) {
            free(device);
            free(host);
            TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
        }

        TEST_ASSERT_TRUE(tm_dev_run(&cases[i], false, host));
        TEST_ASSERT_EQUAL_MEMORY(host, device, bytes);

#if defined(TM_VULKAN)
        /* the same planes as dma-bufs (a device taking none: left out) */
        memset(device, 0, bytes);
        g_dev_kind = HDRG_FRAME_DMABUF;

        if (tm_dev_run(&cases[i], true, device)) {
            TEST_ASSERT_EQUAL_MEMORY(host, device, bytes);
        } else {
            TEST_MESSAGE("the device takes no dma-bufs");
        }

        g_dev_kind = HDRG_FRAME_BUFFERS;
#endif

        free(device);
        free(host);
    }
}

/* the app's frames on its GPU need Y'CbCr input and the frames kept on the GPU, buffers its device
 */
static void test_device_refused(void)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t in;
    hdrg_device_frame_t frame;
    tm_gpu_t *gpu;
    tm_buffer_t *buffer;
    uint8_t *bytes, p;

    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    if (!tm_gpu_create(ctx, &gpu)) {
        hdrg_context_destroy(ctx);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    memset(&format, 0, sizeof(format));
    format.width = 64;
    format.height = 32;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = 64u * 3u;
    hdrg_session_config_init(&config);
    config.device = TM_KIND;

    /* R'G'B' input; buffers without an app device */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));

    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));

    buffer = tm_gpu_buffer(gpu, 65536, &bytes);
    TEST_ASSERT_NOT_NULL(buffer);
    memset(&frame, 0, sizeof(frame));
    memset(&in, 0, sizeof(in));

    for (p = 0; p < 3u; p++) {
        frame.handles[p] = tm_gpu_object(buffer);
        frame.sizes[p] = (TM_KIND == HDRG_DEVICE_VULKAN) ? 65536u : 0u;
        frame.offsets[p] = (size_t)p * 8192u;
        frame.pitches[p] = 64;
    }

    in.device = &frame;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tm_gpu_set(ctx, gpu));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));

    /* a pitch not of whole words, then host planes that are not all there */
    for (p = 0; p < 3u; p++) {
        frame.pitches[p] = 66;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));

    in.device = NULL;
    in.planes[0] = bytes;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));

    /* an AHardwareBuffer: a Vulkan session's alone, of an NV12 or P010 side */
    in.device = &frame;
    in.planes[0] = NULL;
    frame.kind = HDRG_FRAME_AHARDWAREBUFFER;
    frame.handles[0] = &frame;
    TEST_ASSERT_EQUAL_INT((TM_KIND == HDRG_DEVICE_VULKAN) ? HDRG_ERR_INVALID_ARG
                                                          : HDRG_ERR_UNSUPPORTED,
                          hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);

#if defined(TM_VULKAN) && !defined(__ANDROID__)
    /* off Android an NV12 side takes none (the device has no AHardwareBuffer import) */
    config.input.interleaved = true;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);
#endif

    tm_gpu_free(gpu, buffer);
    hdrg_context_destroy(ctx);
}

#if !defined(TM_VULKAN)
/* an IOSurface-backed pixel buffer of type for Metal (NULL: none) */
static CVPixelBufferRef tm_pb_create(OSType type)
{
    CFMutableDictionaryRef attributes;
    CFDictionaryRef surface;
    CVPixelBufferRef buffer;

    attributes = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks);
    TEST_ASSERT_TRUE(attributes != NULL && surface != NULL);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);

    if (CVPixelBufferCreate(NULL, TM_PB_W, TM_PB_H, type, attributes, &buffer) !=
        kCVReturnSuccess) {
        buffer = NULL;
    }

    CFRelease(surface);
    CFRelease(attributes);

    return buffer;
}

/* frame t's sample of plane (0: Y, 1: Cb, 2: Cr) at (x, y): a ramp, steps and hash noise */
static uint16_t tm_pb_sample(uint8_t bits, uint8_t plane, uint32_t x, uint32_t y, uint32_t t)
{
    const uint32_t h = hdrg_hash_u32(TM_KEY, x, y, t + 200u * plane), scale = 1u << (bits - 8u);

    if (plane == 0u) {
        return (uint16_t)((16u + ((x * 3u + y + t * 7u) % 200u)) * scale + h % (9u * scale));
    }

    return (uint16_t)(((plane == 1u) ? 108u + ((x + t) / 8u % 40u) : 98u + (y / 4u % 50u)) * scale +
                      h % (5u * scale));
}

/*
 * frame t (NV12 or P010: bits) into the rows of a pixel buffer's or the host's planes (Y and
 * CbCr, pitches bytes a row)
 */
static void tm_pb_fill(uint8_t bits, uint32_t t, uint8_t *const planes[2], const size_t pitches[2])
{
    const uint32_t cw = (TM_PB_W + 1u) / 2u, ch = (TM_PB_H + 1u) / 2u;
    uint32_t x, y;
    uint16_t v;
    uint8_t c;

    for (y = 0; y < TM_PB_H; y++) {
        for (x = 0; x < TM_PB_W; x++) {
            v = tm_pb_sample(bits, 0, x, y, t);

            if (bits == 8u) {
                planes[0][(size_t)y * pitches[0] + x] = (uint8_t)v;
            } else {
                v = (uint16_t)(v << 6);
                memcpy(planes[0] + (size_t)y * pitches[0] + (size_t)x * 2u, &v, sizeof(v));
            }
        }
    }

    for (y = 0; y < ch; y++) {
        for (x = 0; x < cw; x++) {
            for (c = 0; c < 2u; c++) {
                v = tm_pb_sample(bits, (uint8_t)(c + 1u), x, y, t);

                if (bits == 8u) {
                    planes[1][(size_t)y * pitches[1] + 2u * x + c] = (uint8_t)v;
                } else {
                    v = (uint16_t)(v << 6);
                    memcpy(planes[1] + (size_t)y * pitches[1] + (2u * x + c) * 2u, &v, sizeof(v));
                }
            }
        }
    }
}

/*
 * A session of NV12 (bits 8) or P010 (10) in and P010 out, the input as a
 * decoder's pixel buffers (device_in) or in host memory, the output into an
 * encoder's pixel buffer (device_out) or host memory, the outputs' rows into
 * out. false: no Metal device
 */
static bool tm_pb_run(uint8_t bits, uint8_t preset, bool device_in, bool device_out, uint8_t *out)
{
    const uint32_t cw = (TM_PB_W + 1u) / 2u, ch = (TM_PB_H + 1u) / 2u;
    const size_t host_pitches[2] = {(size_t)TM_PB_W * 2u + 6u, (size_t)cw * 4u + 10u},
                 rows[2] = {(size_t)TM_PB_W * 2u, (size_t)cw * 4u};
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
    CVPixelBufferRef in_buffer, out_buffer;
    uint32_t sent, received, y, heights[2];
    uint8_t *host_in, *host_out, *planes[2], p;
    size_t at, pitches[2];

    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_frames = HDRG_GPU_FRAMES_GPU;
    ctx->gpu_kind = g_pb_device;

    memset(&format, 0, sizeof(format));
    format.width = TM_PB_W;
    format.height = TM_PB_H;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = (size_t)TM_PB_W * 3u;

    hdrg_params_init(&params, preset);

    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = bits;
    config.input.interleaved = true;
    config.input.msb = bits > 8u;
    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.output.interleaved = true;
    config.output.msb = true;
    config.device = (device_in || device_out) ? g_pb_device : 0u;

    status = hdrg_session_create(ctx, &format, &params, &caps, &config, &session);
    if (status == HDRG_ERR_UNSUPPORTED) {
        hdrg_context_destroy(ctx);

        return false;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

    host_in = (uint8_t *)calloc(1, host_pitches[0] * TM_PB_H + host_pitches[1] * ch);
    host_out = (uint8_t *)calloc(1, host_pitches[0] * TM_PB_H + host_pitches[1] * ch);
    out_buffer = tm_pb_create(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
    TEST_ASSERT_TRUE(host_in != NULL && host_out != NULL && out_buffer != NULL);

    memset(&out_frame, 0, sizeof(out_frame));
    out_frame.kind = HDRG_FRAME_CVPIXELBUFFER;
    out_frame.handles[0] = out_buffer;
    sent = 0;
    received = 0;
    at = 0;
    heights[0] = TM_PB_H;
    heights[1] = ch;

    for (;;) {
        memset(&o, 0, sizeof(o));
        o.device = device_out ? &out_frame : NULL;
        o.planes[0] = host_out;
        o.planes[1] = host_out + host_pitches[0] * TM_PB_H;
        o.strides[0] = host_pitches[0];
        o.strides[1] = host_pitches[1];

        status = hdrg_session_receive_frame(session, &o);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            memset(&in_frame, 0, sizeof(in_frame));
            in.pts = sent;
            in_buffer = NULL;

            if (sent < TM_DEV_N && device_in) {
                in_buffer =
                    tm_pb_create((bits == 8u) ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                                              : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
                TEST_ASSERT_NOT_NULL(in_buffer);
                TEST_ASSERT_EQUAL_INT(kCVReturnSuccess, CVPixelBufferLockBaseAddress(in_buffer, 0));

                for (p = 0; p < 2u; p++) {
                    planes[p] = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(in_buffer, p);
                    pitches[p] = CVPixelBufferGetBytesPerRowOfPlane(in_buffer, p);
                }

                tm_pb_fill(bits, sent, planes, pitches);
                CVPixelBufferUnlockBaseAddress(in_buffer, 0);
                in_frame.kind = HDRG_FRAME_CVPIXELBUFFER;
                in_frame.handles[0] = in_buffer;
                in.device = &in_frame;
            } else if (sent < TM_DEV_N) {
                planes[0] = host_in;
                planes[1] = host_in + host_pitches[0] * TM_PB_H;
                tm_pb_fill(bits, sent, planes, host_pitches);
                in.planes[0] = planes[0];
                in.planes[1] = planes[1];
                in.strides[0] = host_pitches[0];
                in.strides[1] = host_pitches[1];
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < TM_DEV_N) ? &in : NULL));
            sent++;

            if (in_buffer != NULL) {
                CVPixelBufferRelease(in_buffer);
            }

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

        if (device_out) {
            TEST_ASSERT_EQUAL_INT(kCVReturnSuccess, CVPixelBufferLockBaseAddress(
                                                        out_buffer, kCVPixelBufferLock_ReadOnly));
        }

        for (p = 0; p < 2u; p++) {
            planes[p] = device_out ? (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(out_buffer, p)
                                   : (uint8_t *)(uintptr_t)o.planes[p];
            pitches[p] =
                device_out ? CVPixelBufferGetBytesPerRowOfPlane(out_buffer, p) : host_pitches[p];

            for (y = 0; y < heights[p]; y++) {
                memcpy(out + at, planes[p] + (size_t)y * pitches[p], rows[p]);
                at += rows[p];
            }
        }

        if (device_out) {
            CVPixelBufferUnlockBaseAddress(out_buffer, kCVPixelBufferLock_ReadOnly);
        }

        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TM_DEV_N, received);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    CVPixelBufferRelease(out_buffer);
    free(host_in);
    free(host_out);

    return true;
}

/*
 * A decoder's NV12 and P010 pixel buffers in and an encoder's P010 out (and
 * either with host memory on the other side) give what host memory gives,
 * on the library's own device (no app device): Metal, and WebGPU (Dawn's
 * shared texture memory, the input) where the build has it
 */
static void test_pixel_buffers(void)
{
#if defined(HDRG_WITH_WEBGPU)
    static const uint32_t devices[2] = {HDRG_DEVICE_METAL, HDRG_DEVICE_WEBGPU};
#else
    static const uint32_t devices[1] = {HDRG_DEVICE_METAL};
#endif
    static const uint8_t depths[2] = {8, 10}, presets[2] = {HDRG_PRESET_ANIME, HDRG_PRESET_PHOTO};
    const size_t bytes = ((size_t)TM_PB_W * 2u * TM_PB_H +
                          (size_t)((TM_PB_W + 1u) / 2u) * 4u * ((TM_PB_H + 1u) / 2u)) *
                         TM_DEV_N;
    uint8_t *host, *device, d, m, k;

    host = (uint8_t *)calloc(1, bytes);
    device = (uint8_t *)calloc(1, bytes);
    TEST_ASSERT_TRUE(host != NULL && device != NULL);

    for (k = 0; k < sizeof(devices) / sizeof(devices[0]); k++) {
        g_pb_device = devices[k];

        for (d = 0; d < 2u; d++) {
            TEST_ASSERT_TRUE(tm_pb_run(depths[d], presets[d], false, false, host));

            /* Dawn's IOSurface textures take no copies to them on Metal: WebGPU reads them */
            for (m = 1; m < ((devices[k] == HDRG_DEVICE_WEBGPU) ? 2u : 4u); m++) {
                memset(device, 0, bytes);

                if (!tm_pb_run(depths[d], presets[d], (m & 1u) != 0u, (m & 2u) != 0u, device)) {
                    free(host);
                    free(device);
                    g_pb_device = HDRG_DEVICE_METAL;
                    TEST_IGNORE_MESSAGE("no GPU device");
                }

                TEST_ASSERT_EQUAL_MEMORY(host, device, bytes);
            }
        }
    }

    g_pb_device = HDRG_DEVICE_METAL;
    free(host);
    free(device);
}

/* a pixel buffer of another format, buffers without the app's device, a kind not taken */
static void test_pixel_buffers_refused(void)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t in;
    hdrg_device_frame_t frame;
    hdrg_status_t status;
    CVPixelBufferRef buffer;

    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_frames = HDRG_GPU_FRAMES_GPU;

    memset(&format, 0, sizeof(format));
    format.width = TM_PB_W;
    format.height = TM_PB_H;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = (size_t)TM_PB_W * 3u;
    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 10;
    config.input.interleaved = true;
    config.input.msb = true;
    config.device = HDRG_DEVICE_METAL;

    status = hdrg_session_create(ctx, &format, NULL, NULL, &config, &session);
    if (status == HDRG_ERR_UNSUPPORTED) {
        hdrg_context_destroy(ctx);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

    buffer = tm_pb_create(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    TEST_ASSERT_NOT_NULL(buffer);
    memset(&in, 0, sizeof(in));
    memset(&frame, 0, sizeof(frame));
    in.device = &frame;
    frame.kind = HDRG_FRAME_CVPIXELBUFFER;
    frame.handles[0] = buffer;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));

    frame.kind = HDRG_FRAME_BUFFERS;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));

    frame.kind = HDRG_FRAME_DMABUF;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_session_send_frame(session, &in));

    frame.kind = 99;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &in));

    hdrg_session_destroy(session);
    CVPixelBufferRelease(buffer);
    hdrg_context_destroy(ctx);
}
#endif

#if defined(HDRG_TEST_GPU_MODULE)
/*
 * a module's copy on the device (tests/modules/gpu_copy.c: a GPU layer of its own on the context's
 * device and queue) between the library's deblocking and deringing in LOW: frames taken and given
 * on the device, the output and latency those of the two alone
 */
static void test_module_frames(void)
{
    const size_t samples = (size_t)TM_W * TM_H * 3u * TM_FRAMES;
    uint32_t latency, alone_latency, in, out, in_after, out_after;
    uint16_t *frames, *alone, *between;
    void *module;

    g_chain = TM_DERING;
    frames = tm_frames();
    alone = (uint16_t *)malloc(samples * sizeof(uint16_t));
    between = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(alone != NULL && between != NULL);

    g_latency = HDRG_LATENCY_LOW;
    g_next = TM_ID("dering");

    if (tm_session(TM_ID("deblock"), HDRG_STAGE_PREFILTER, frames, alone, &alone_latency) ==
        HDRG_ERR_UNSUPPORTED) {
        g_latency = HDRG_LATENCY_PLAYBACK;
        g_next = NULL;
        g_chain = TM_DEBAND;
        free(frames);
        free(alone);
        free(between);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

#if defined(TM_VULKAN)
    g_between = "org.test.gpu.vulkan.copy";
#else
    g_between = "org.test.gpu.metal.copy";
#endif
    module = hdrg_test_gpu_module_open(HDRG_TEST_GPU_MODULE);
    TEST_ASSERT_NOT_NULL(module);
    hdrg_test_gpu_module_jobs(module, &in, &out);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tm_session(TM_ID("deblock"), HDRG_STAGE_PREFILTER, frames, between, &latency));
    hdrg_test_gpu_module_jobs(module, &in_after, &out_after);
    hdrg_test_gpu_module_close(module);
    g_between = NULL;
    g_next = NULL;
    g_latency = HDRG_LATENCY_PLAYBACK;
    g_chain = TM_DEBAND;
    TEST_ASSERT_EQUAL_MEMORY(alone, between, samples * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_UINT32(alone_latency, latency);
    TEST_ASSERT_TRUE(in_after > in);
    TEST_ASSERT_TRUE(out_after > out);

    free(frames);
    free(alone);
    free(between);
}
#endif

/* the context's own device and queue: the same ones each time they are asked */
static void test_shared_device(void)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    void *device, *queue, *again, *again_queue;

    hdrg_context_desc_init(&desc);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    if (!hdrg_host_device(&ctx->host, TM_KIND, &device, &queue)) {
        hdrg_context_destroy(ctx);
        TEST_IGNORE_MESSAGE(TM_NO_DEVICE);
    }

    TEST_ASSERT_NOT_NULL(queue);
    TEST_ASSERT_TRUE(hdrg_host_device(&ctx->host, TM_KIND, &again, &again_queue));
    TEST_ASSERT_EQUAL_PTR(device, again);
    TEST_ASSERT_EQUAL_PTR(queue, again_queue);

    hdrg_context_destroy(ctx);
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
    RUN_TEST(test_session);
    RUN_TEST(test_lanczos);
#if !defined(TM_VULKAN)
    RUN_TEST(test_metalfx);
#endif
    RUN_TEST(test_shared_device);
    RUN_TEST(test_device_frames);
    RUN_TEST(test_device_refused);
#if defined(HDRG_TEST_GPU_MODULE)
    RUN_TEST(test_module_frames);
#endif
#if !defined(TM_VULKAN)
    RUN_TEST(test_pixel_buffers);
    RUN_TEST(test_pixel_buffers_refused);
#endif

    return UNITY_END();
}
