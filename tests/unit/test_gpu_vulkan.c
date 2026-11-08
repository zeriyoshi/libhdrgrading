/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* memfd_create */
#endif

#include <unity.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/gpu/gk.h"
#include "hdrgrading/gpu/vulkan.h"
#include "priv.h"

/*
 * The Vulkan layer (src/gpu/vulkan) (HDRG_WITH_VULKAN): a device of its own
 * or the app's (one of its own described as an app's, on another table),
 * host buffers the GPU reads and writes in place, Tint's SPIR-V with its
 * buffers found by their WGSL names, passes ended on the device's thread,
 * buffers freed while a pass uses them, a pass's uniforms past its first
 * buffer of them, dma-bufs (memfds, which lavapipe takes as dma-bufs).
 * Without a Vulkan device the tests are ignored.
 */
#define TV_W     37u /* odd: a row's last word half used */
#define TV_H     19u
#define TV_KEY   UINT64_C(0x76756c6b616e)
#define TV_MANY  2600u /* tv_lumas's dispatches of a pass: past the uniforms' first buffer */
#define TV_BYTES ((((size_t)TV_W * 6u + 3u) / 4u) * 4u * TV_H)

/* a pass's end as hdrg_vulkan_commit reports it */
typedef struct {
    hdrg_mutex_t mutex;
    hdrg_cond_t cond;
    bool done, ok;
} tv_wait_t;

/* the uniform of src/plugins/deband/wgsl/deband.wgsl (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride;
    float strength, pad[3];
} tv_params_t;

/* hdrg.deband's plugin's shaders (src/plugins/deband: Tint's of its WGSL) */
extern const hdrg_gk_shader_t hdrg_gk_deband_lumas, hdrg_gk_deband_edges;

static hdrg_context_t *g_ctx; /* its plugin table: the allocator of the layer */
static uint32_t g_locks[2];   /* tv_lock: the app's queue locked, unlocked */
static hdrg_vulkan_t *g_vulkan;

/* the app's queue lock: counted (one thread submits at a time: its calls pair) */
static void tv_lock(void *user, bool lock)
{
    (void)user;

    TEST_ASSERT_EQUAL_UINT32(g_locks[0], g_locks[1] + (lock ? 0u : 1u));
    g_locks[lock ? 0 : 1]++;
}

static void tv_done(void *arg, bool ok)
{
    tv_wait_t *wait = (tv_wait_t *)arg;

    hdrg_mutex_lock(&wait->mutex);
    wait->ok = ok;
    wait->done = true;
    hdrg_cond_signal(&wait->cond);
    hdrg_mutex_unlock(&wait->mutex);
}

/* the frame's samples: hashed */
static void tv_fill(uint8_t *in)
{
    const size_t stride = ((size_t)TV_W * 6u + 3u) / 4u * 4u;
    uint32_t x, y;
    uint16_t *row;

    for (y = 0; y < TV_H; y++) {
        row = (uint16_t *)(void *)(in + y * stride);

        for (x = 0; x < TV_W * 3u; x++) {
            row[x] = (uint16_t)hdrg_hash_u32(TV_KEY, x, y, 0u);
        }
    }
}

/* the deband shader's Y' of every pixel of in against the CPU's */
static void tv_check(const uint8_t *in, const uint8_t *out)
{
    const uint16_t *row;
    const int32_t *luma = (const int32_t *)(const void *)out;
    const size_t stride = ((size_t)TV_W * 6u + 3u) / 4u * 4u;
    uint32_t x, y, expect;

    for (y = 0; y < TV_H; y++) {
        row = (const uint16_t *)(const void *)(in + y * stride);

        for (x = 0; x < TV_W; x++) {
            expect = ((uint32_t)row[x * 3u] * 13933u + (uint32_t)row[x * 3u + 1u] * 46871u +
                      (uint32_t)row[x * 3u + 2u] * 4732u + 32768u) >>
                     16;
            TEST_ASSERT_EQUAL_INT32((int32_t)expect, luma[y * TV_W + x]);
        }
    }
}

/*
 * the deband shader's Y' through vulkan: dispatches of a pass (the last one's
 * the output's), its end waited for inline or on the device's thread, the
 * buffers freed (when early) as soon as it is committed; source's bytes
 * (NULL: a host buffer of the frame's)
 */
static void tv_lumas(hdrg_vulkan_t *vulkan, bool wait_inline, uint32_t dispatches, bool early,
                     hdrg_vulkan_buffer_t *source, uint8_t *source_bytes)
{
    const size_t stride = ((size_t)TV_W * 6u + 3u) / 4u * 4u;
    const char *const storage[2] = {"source", "luma"};
    hdrg_vulkan_buffer_t *buffers[2];
    hdrg_vulkan_kernel_t *kernel;
    hdrg_vulkan_pass_t *pass;
    tv_params_t params;
    tv_wait_t wait;
    uint32_t d;
    uint8_t *in, *out, *kept;

    buffers[0] = (source != NULL) ? source : hdrg_vulkan_host_buffer(vulkan, TV_BYTES, &in);
    in = (source != NULL) ? source_bytes : in;
    buffers[1] = hdrg_vulkan_host_buffer(vulkan, (size_t)TV_W * TV_H * 4u, &out);
    TEST_ASSERT_TRUE(buffers[0] != NULL && buffers[1] != NULL);

    tv_fill(in);
    kernel = hdrg_vulkan_kernel(vulkan, hdrg_gk_deband_lumas.spirv,
                                *hdrg_gk_deband_lumas.spirv_size, "lumas", "params", storage, 2);
    TEST_ASSERT_NOT_NULL(kernel);

    memset(&params, 0, sizeof(params));
    params.width = TV_W;
    params.height = TV_H;
    params.in_stride = (uint32_t)(stride / 4u);
    params.out_stride = params.in_stride;
    memset(out, 0xff, (size_t)TV_W * TV_H * 4u);
    kept = (uint8_t *)malloc(TV_BYTES);
    TEST_ASSERT_NOT_NULL(kept);
    memcpy(kept, in, TV_BYTES);

    pass = hdrg_vulkan_begin(vulkan);
    TEST_ASSERT_NOT_NULL(pass);

    /* the earlier dispatches with a width of 1 (their uniforms differ from the last one's) */
    for (d = 0; d < dispatches; d++) {
        params.width = (d + 1u == dispatches) ? TV_W : 1u;
        hdrg_vulkan_dispatch(pass, kernel, &params, sizeof(params), buffers, (TV_W + 7u) / 8u,
                             (TV_H + 7u) / 8u);
    }

    memset(&wait, 0, sizeof(wait));
    TEST_ASSERT_TRUE(hdrg_mutex_init(&wait.mutex));
    TEST_ASSERT_TRUE(hdrg_cond_init(&wait.cond));

    if (wait_inline) {
        TEST_ASSERT_TRUE(hdrg_vulkan_commit(pass, NULL, NULL));
        wait.ok = true;
    } else {
        TEST_ASSERT_TRUE(hdrg_vulkan_commit(pass, tv_done, &wait));
    }

    /* the pass keeps what it uses: the caller's references may go at once */
    if (early) {
        hdrg_vulkan_kernel_free(kernel);
        kernel = NULL;
    }

    hdrg_mutex_lock(&wait.mutex);

    while (!wait_inline && !wait.done) {
        hdrg_cond_wait(&wait.cond, &wait.mutex);
    }

    hdrg_mutex_unlock(&wait.mutex);
    hdrg_cond_destroy(&wait.cond);
    hdrg_mutex_destroy(&wait.mutex);
    TEST_ASSERT_TRUE(wait.ok);

    tv_check(kept, out);

    hdrg_vulkan_kernel_free(kernel);

    if (source == NULL) {
        hdrg_vulkan_buffer_free(buffers[0]);
    }

    hdrg_vulkan_buffer_free(buffers[1]);
    free(kept);
}

/* a pass ended on the device's thread, one the commit waits for, the caller's references freed */
static void test_kernel(void)
{
    if (g_vulkan == NULL) {
        TEST_IGNORE_MESSAGE("no Vulkan device");
    }

    tv_lumas(g_vulkan, false, 1, false, NULL, NULL);
    tv_lumas(g_vulkan, true, 1, false, NULL, NULL);
    tv_lumas(g_vulkan, false, 1, true, NULL, NULL);
}

/* the uniforms of a pass past its first buffer of them */
static void test_many_dispatches(void)
{
    if (g_vulkan == NULL) {
        TEST_IGNORE_MESSAGE("no Vulkan device");
    }

    tv_lumas(g_vulkan, false, TV_MANY, false, NULL, NULL);
    tv_lumas(g_vulkan, true, TV_MANY, true, NULL, NULL);
}

/*
 * the app's device (HDRG_DEVICE_VULKAN): a device of the layer's own on
 * another table described as an app's, its buffers wrapped
 */
static void test_app_device(void)
{
    hdrg_vulkan_device_t desc;
    hdrg_context_t *other;
    hdrg_vulkan_t *vulkan, *again;
    hdrg_vulkan_buffer_t *wrapped, *owned;
    uint8_t *bytes;

    if (g_vulkan == NULL) {
        TEST_IGNORE_MESSAGE("no Vulkan device");
    }

    TEST_ASSERT_TRUE(hdrg_vulkan_create(&g_ctx->host.api, hdrg_vulkan_device(g_vulkan),
                                        hdrg_vulkan_queue(g_vulkan), &again));
    TEST_ASSERT_EQUAL_PTR(g_vulkan, again);
    hdrg_vulkan_destroy(again);

    /* another table: a device of its own on the app's (the same queue, used by one at a time) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&other, NULL, 1));
    TEST_ASSERT_TRUE(hdrg_vulkan_create(&other->host.api, hdrg_vulkan_device(g_vulkan),
                                        hdrg_vulkan_queue(g_vulkan), &vulkan));
    TEST_ASSERT_TRUE(vulkan != g_vulkan);

    owned = hdrg_vulkan_host_buffer(g_vulkan, TV_BYTES, &bytes);
    TEST_ASSERT_NOT_NULL(owned);
    wrapped = hdrg_vulkan_buffer_wrap(vulkan, hdrg_vulkan_buffer_object(owned), TV_BYTES);
    TEST_ASSERT_NOT_NULL(wrapped);
    TEST_ASSERT_EQUAL_PTR(hdrg_vulkan_buffer_object(owned), hdrg_vulkan_buffer_object(wrapped));
    TEST_ASSERT_EQUAL_UINT64(TV_BYTES, hdrg_vulkan_buffer_size(wrapped));
    tv_lumas(vulkan, false, 1, false, wrapped, bytes);
    hdrg_vulkan_buffer_free(wrapped);
    hdrg_vulkan_buffer_free(owned);
    hdrg_vulkan_destroy(vulkan);
    hdrg_context_destroy(other);

    /* the app's lock around each of the library's submissions */
    desc = *hdrg_vulkan_device(g_vulkan);
    desc.lock_queue = tv_lock;
    g_locks[0] = 0;
    g_locks[1] = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&other, NULL, 1));
    TEST_ASSERT_TRUE(
        hdrg_vulkan_create(&other->host.api, &desc, hdrg_vulkan_queue(g_vulkan), &vulkan));
    tv_lumas(vulkan, false, 3, false, NULL, NULL);
    tv_lumas(vulkan, true, 3, true, NULL, NULL);
    hdrg_vulkan_destroy(vulkan);
    hdrg_context_destroy(other);
    TEST_ASSERT_TRUE(g_locks[0] >= 2u);
    TEST_ASSERT_EQUAL_UINT32(g_locks[0], g_locks[1]);
}

/* a dma-buf's bytes read in place (a memfd lavapipe takes as one; others may take none) */
static void test_dmabuf(void)
{
#if defined(__linux__)
    hdrg_vulkan_buffer_t *buffer;
    uint8_t *bytes;
    int fd;

    if (g_vulkan == NULL) {
        TEST_IGNORE_MESSAGE("no Vulkan device");
    }

    fd = memfd_create("tv", MFD_CLOEXEC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 65536));
    bytes = (uint8_t *)mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    TEST_ASSERT_TRUE(bytes != MAP_FAILED);

    buffer = hdrg_vulkan_dmabuf(g_vulkan, fd, 0);

    if (buffer == NULL) {
        (void)munmap(bytes, 65536);
        (void)close(fd);
        TEST_IGNORE_MESSAGE("the device takes no dma-bufs");
    }

    TEST_ASSERT_EQUAL_UINT64(65536, hdrg_vulkan_buffer_size(buffer));
    tv_lumas(g_vulkan, false, 1, false, buffer, bytes);
    hdrg_vulkan_buffer_free(buffer);

    /* larger than the dma-buf, a closed fd */
    TEST_ASSERT_NULL(hdrg_vulkan_dmabuf(g_vulkan, fd, 65537));
    (void)munmap(bytes, 65536);
    (void)close(fd);
    TEST_ASSERT_NULL(hdrg_vulkan_dmabuf(g_vulkan, fd, 0));
    TEST_ASSERT_NULL(hdrg_vulkan_dmabuf(g_vulkan, -1, 0));
#else
    TEST_IGNORE_MESSAGE("dma-bufs are Linux's");
#endif
}

/* the kernels' refusals: names not the module's buffers, another entry, SPIR-V that is not */
static void test_refusals(void)
{
    const uint32_t junk[8] = {0x07230203u, 0x00010300u, 0, 64, 0, 0, 0, 0};
    const char *const storage[3] = {"source", "luma", "edge"}, *const swapped[2] = {"luma", "luma"};
    hdrg_vulkan_device_t device;
    hdrg_vulkan_t *vulkan;
    uint8_t *contents;

    if (g_vulkan == NULL) {
        TEST_IGNORE_MESSAGE("no Vulkan device");
    }

    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv,
                                        *hdrg_gk_deband_lumas.spirv_size, "lumas", "params",
                                        storage, 1));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv,
                                        *hdrg_gk_deband_lumas.spirv_size, "lumas", "params",
                                        storage, 3));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv,
                                        *hdrg_gk_deband_lumas.spirv_size, "lumas", "params",
                                        swapped, 2));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv,
                                        *hdrg_gk_deband_lumas.spirv_size, "lumas", NULL, storage,
                                        2));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv,
                                        *hdrg_gk_deband_lumas.spirv_size, "edges", "params",
                                        storage, 2));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_edges.spirv, 40u, "edges",
                                        "params", storage + 1, 2));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, junk, sizeof(junk), "lumas", NULL, storage, 0));
    TEST_ASSERT_NULL(hdrg_vulkan_kernel(g_vulkan, hdrg_gk_deband_lumas.spirv, 6u, "lumas", "params",
                                        storage, 2));

    TEST_ASSERT_NULL(hdrg_vulkan_host_buffer(g_vulkan, 0, &contents));
    TEST_ASSERT_NULL(contents);
    TEST_ASSERT_NULL(hdrg_vulkan_buffer(g_vulkan, 0));
    TEST_ASSERT_NULL(hdrg_vulkan_buffer_wrap(g_vulkan, NULL, 64));

    /* an app's device without push descriptors, too old, of a smaller struct */
    device = *hdrg_vulkan_device(g_vulkan);
    device.extension_count = 0;
    TEST_ASSERT_FALSE(
        hdrg_vulkan_create(&g_ctx->host.api, &device, (void *)(uintptr_t)1u, &vulkan));
    device = *hdrg_vulkan_device(g_vulkan);
    device.api_version = (1u << 22);
    TEST_ASSERT_FALSE(
        hdrg_vulkan_create(&g_ctx->host.api, &device, (void *)(uintptr_t)1u, &vulkan));
    device = *hdrg_vulkan_device(g_vulkan);
    device.struct_size = 8u;
    TEST_ASSERT_FALSE(
        hdrg_vulkan_create(&g_ctx->host.api, &device, (void *)(uintptr_t)1u, &vulkan));
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

    if (!hdrg_vulkan_create(&g_ctx->host.api, NULL, NULL, &g_vulkan)) {
        g_vulkan = NULL;
    }

    UNITY_BEGIN();
    RUN_TEST(test_kernel);
    RUN_TEST(test_many_dispatches);
    RUN_TEST(test_app_device);
    RUN_TEST(test_dmabuf);
    RUN_TEST(test_refusals);
    result = UNITY_END();

    hdrg_vulkan_destroy(g_vulkan);
    hdrg_context_destroy(g_ctx);

    return result;
}
