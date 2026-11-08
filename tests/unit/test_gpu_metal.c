/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <CoreFoundation/CoreFoundation.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/gpu/gk.h"
#include "hdrgrading/gpu/metal.h"
#include "priv.h"

/*
 * The Metal layer (src/gpu/metal) (HDRG_WITH_METAL): a device of its own or the app's,
 * host buffers the GPU reads and writes in place, Tint's MSL with its
 * buffers found by their WGSL names, passes ended on a thread of Metal's.
 * Without a Metal device the tests are ignored.
 */
#define TM_W   37u /* odd: a row's last word half used */
#define TM_H   19u
#define TM_KEY UINT64_C(0x6d6574616c)

/* a pass's end as hdrg_metal_commit reports it */
typedef struct {
    hdrg_mutex_t mutex;
    hdrg_cond_t cond;
    bool done, ok;
} tm_wait_t;

/* the uniform of src/plugins/deband/wgsl/deband.wgsl (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride;
    float strength, pad[3];
} tm_params_t;

/* hdrg.deband's plugin's shader (src/plugins/deband: Tint's of its WGSL) */
extern const hdrg_gk_shader_t hdrg_gk_deband_lumas;

/* MTLCreateSystemDefaultDevice from C: an id<MTLDevice> with a reference of the caller's */
extern void *MTLCreateSystemDefaultDevice(void);

static hdrg_context_t *g_ctx; /* its plugin table: the allocator of the layer */
static hdrg_metal_t *g_metal;

static void tm_done(void *arg, bool ok)
{
    tm_wait_t *wait = (tm_wait_t *)arg;

    hdrg_mutex_lock(&wait->mutex);
    wait->ok = ok;
    wait->done = true;
    hdrg_cond_signal(&wait->cond);
    hdrg_mutex_unlock(&wait->mutex);
}

/* the deband shader's Y' of every pixel of a frame through metal, against the CPU's */
static void tm_lumas(hdrg_metal_t *metal, bool wait_inline)
{
    const int32_t *luma;
    const size_t stride = ((size_t)TM_W * 6u + 3u) / 4u * 4u;
    const char *const storage[2] = {"source", "luma"};
    hdrg_metal_buffer_t *buffers[2];
    hdrg_metal_kernel_t *kernel;
    hdrg_metal_pass_t *pass;
    tm_params_t params;
    tm_wait_t wait;
    uint32_t x, y, expect, rgb[3];
    uint16_t *row;
    uint8_t *in, *out, c;

    buffers[0] = hdrg_metal_host_buffer(metal, stride * TM_H, &in);
    buffers[1] = hdrg_metal_host_buffer(metal, (size_t)TM_W * TM_H * 4u, &out);
    TEST_ASSERT_TRUE(buffers[0] != NULL && buffers[1] != NULL);

    for (y = 0; y < TM_H; y++) {
        row = (uint16_t *)(void *)(in + y * stride);

        for (x = 0; x < TM_W * 3u; x++) {
            row[x] = (uint16_t)hdrg_hash_u32(TM_KEY, x, y, 0u);
        }
    }

    kernel = hdrg_metal_kernel(metal, hdrg_gk_deband_lumas.msl, "lumas", "params", storage, 2);
    TEST_ASSERT_NOT_NULL(kernel);

    memset(&params, 0, sizeof(params));
    params.width = TM_W;
    params.height = TM_H;
    params.in_stride = (uint32_t)(stride / 4u);
    params.out_stride = params.in_stride;
    memset(out, 0xff, (size_t)TM_W * TM_H * 4u);

    pass = hdrg_metal_begin(metal);
    TEST_ASSERT_NOT_NULL(pass);
    hdrg_metal_dispatch(pass, kernel, &params, sizeof(params), buffers, (TM_W + 7u) / 8u,
                        (TM_H + 7u) / 8u, 8u, 8u);

    if (wait_inline) {
        TEST_ASSERT_TRUE(hdrg_metal_commit(pass, NULL, NULL));
    } else {
        memset(&wait, 0, sizeof(wait));
        TEST_ASSERT_TRUE(hdrg_mutex_init(&wait.mutex));
        TEST_ASSERT_TRUE(hdrg_cond_init(&wait.cond));
        TEST_ASSERT_TRUE(hdrg_metal_commit(pass, tm_done, &wait));

        hdrg_mutex_lock(&wait.mutex);

        while (!wait.done) {
            hdrg_cond_wait(&wait.cond, &wait.mutex);
        }

        hdrg_mutex_unlock(&wait.mutex);
        hdrg_cond_destroy(&wait.cond);
        hdrg_mutex_destroy(&wait.mutex);
        TEST_ASSERT_TRUE(wait.ok);
    }

    luma = (const int32_t *)(const void *)out;

    for (y = 0; y < TM_H; y++) {
        row = (uint16_t *)(void *)(in + y * stride);

        for (x = 0; x < TM_W; x++) {
            for (c = 0; c < 3u; c++) {
                rgb[c] = row[x * 3u + c];
            }

            expect = (rgb[0] * 13933u + rgb[1] * 46871u + rgb[2] * 4732u + 32768u) >> 16;
            TEST_ASSERT_EQUAL_INT32((int32_t)expect, luma[y * TM_W + x]);
        }
    }

    hdrg_metal_kernel_free(kernel);
    hdrg_metal_buffer_free(buffers[0]);
    hdrg_metal_buffer_free(buffers[1]);
}

/* a pass ended on Metal's thread, and one the commit waits for */
static void test_kernel(void)
{
    if (g_metal == NULL) {
        TEST_IGNORE_MESSAGE("no Metal device");
    }

    tm_lumas(g_metal, false);
    tm_lumas(g_metal, true);
}

/* the app's device (HDRG_DEVICE_METAL) without a queue: one of its own on it */
static void test_app_device(void)
{
    hdrg_metal_t *metal;
    void *device;

    device = MTLCreateSystemDefaultDevice();
    if (device == NULL) {
        TEST_IGNORE_MESSAGE("no Metal device");
    }

    TEST_ASSERT_TRUE(hdrg_metal_create(&g_ctx->host.api, device, NULL, &metal));
    CFRelease(device);
    tm_lumas(metal, false);
    hdrg_metal_destroy(metal);
}

/* the buffers' and the kernels' refusals: names not the entry point's, MSL that does not compile */
static void test_refusals(void)
{
    const char *const storage[3] = {"source", "luma", "edge"}, *const swapped[2] = {"luma", "luma"},
                      *msl = hdrg_gk_deband_lumas.msl;
    hdrg_metal_buffer_t *buffer;
    uint8_t *contents;

    if (g_metal == NULL) {
        TEST_IGNORE_MESSAGE("no Metal device");
    }

    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, msl, "lumas", "params", storage, 1));
    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, msl, "lumas", "params", storage, 3));
    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, msl, "lumas", "params", swapped, 2));
    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, msl, "lumas", NULL, storage, 2));
    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, msl, "edges", "params", storage, 2));
    TEST_ASSERT_NULL(hdrg_metal_kernel(g_metal, "kernel void lumas(", "lumas", NULL, storage, 0));

    TEST_ASSERT_NULL(hdrg_metal_host_buffer(g_metal, 0, &contents));
    TEST_ASSERT_NULL(contents);
    TEST_ASSERT_NULL(hdrg_metal_buffer(g_metal, 0));
    TEST_ASSERT_NULL(hdrg_metal_texture(g_metal, 0, 16, 16));

    buffer = hdrg_metal_host_buffer(g_metal, 1, &contents);
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)contents % hdrg_metal_page());
    TEST_ASSERT_EQUAL_UINT64(0, hdrg_metal_page() % 16384u);
    hdrg_metal_buffer_free(buffer);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    int result;

    if (hdrg_test_context(&g_ctx, NULL, 1) != HDRG_OK) {
        return 1;
    }

    (void)hdrg_metal_create(&g_ctx->host.api, NULL, NULL, &g_metal);

    UNITY_BEGIN();
    RUN_TEST(test_kernel);
    RUN_TEST(test_app_device);
    RUN_TEST(test_refusals);
    result = UNITY_END();

    hdrg_metal_destroy(g_metal);
    hdrg_context_destroy(g_ctx);

    return result;
}
