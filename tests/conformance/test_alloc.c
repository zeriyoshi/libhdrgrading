/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/filter.h"
#include "hdrgrading/video.h"

/*
 * The context's allocator: every block of the library, of
 * the built-in filters and GPU layers, of libpng and lcms2 and of the plugins (their API table)
 * comes from it and goes back to it. The counting allocator hands out blocks 16 bytes into its own:
 * a block freed elsewhere, or one of elsewhere freed here, crashes.
 */
#define TA_HEADER 16u
/* the sweep's refusals: every allocation (each a run of its own), a seventh of them under Valgrind
 */
#if defined(HDRG_TEST_VALGRIND)
#define TA_SWEEP_STEP 7u
#else
#define TA_SWEEP_STEP 1u
#endif
#define TA_LIT(s) (s), (sizeof(s) - 1u)

typedef struct {
    size_t live, total_allocs, total_frees, callocs;
    size_t calls, fail_at; /* the allocations asked for, the one refused (1-based; 0: none) */
    bool mismatch;
} counting_state_t;

/* what the plugin's memory calls saw */
typedef struct {
    size_t during_init; /* the counting allocator's blocks in init */
    bool zeroed, grown, aligned, refused, deinit_freed;
} plugin_seen_t;

static hdrg_mutex_t lock; /* the pool's threads and a GPU's completions allocate too */
static counting_state_t state;
static plugin_seen_t seen;

/* an allocation asked for: refused when it is the one to fail */
static bool counting_refuse(counting_state_t *st)
{
    bool refuse;

    hdrg_mutex_lock(&lock);
    st->calls++;
    refuse = st->fail_at != 0 && st->calls == st->fail_at;
    hdrg_mutex_unlock(&lock);

    return refuse;
}

static void *counting_track(uint8_t *block, size_t size, bool zeroed, void *user)
{
    counting_state_t *st = (counting_state_t *)user;

    if (block == NULL) {
        return NULL;
    }

    memcpy(block, &size, sizeof(size));

    hdrg_mutex_lock(&lock);
    st->live++;
    st->total_allocs++;
    st->callocs += zeroed ? 1u : 0u;
    hdrg_mutex_unlock(&lock);

    return block + TA_HEADER;
}

static void *counting_malloc(size_t size, void *user)
{
    if (counting_refuse((counting_state_t *)user)) {
        return NULL;
    }

    return counting_track((uint8_t *)malloc(size + TA_HEADER), size, false, user);
}

static void *counting_calloc(size_t count, size_t size, void *user)
{
    if (counting_refuse((counting_state_t *)user) ||
        (size != 0 && count > (SIZE_MAX - TA_HEADER) / size)) {
        return NULL;
    }

    return counting_track((uint8_t *)calloc(1, count * size + TA_HEADER), count * size, true, user);
}

static void *counting_realloc(void *ptr, size_t size, void *user)
{
    uint8_t *block;

    if (ptr == NULL) {
        return counting_malloc(size, user);
    }

    /* refused: the block stays as it was */
    if (counting_refuse((counting_state_t *)user)) {
        return NULL;
    }

    block = (uint8_t *)realloc((uint8_t *)ptr - TA_HEADER, size + TA_HEADER);
    if (block == NULL) {
        return NULL;
    }

    memcpy(block, &size, sizeof(size));

    return block + TA_HEADER;
}

static void counting_free(void *ptr, void *user)
{
    counting_state_t *st = (counting_state_t *)user;
    bool bad;

    hdrg_mutex_lock(&lock);
    bad = ptr == NULL || st->live == 0;
    st->mismatch = st->mismatch || bad;

    if (!bad) {
        st->live--;
        st->total_frees++;
    }

    hdrg_mutex_unlock(&lock);

    if (!bad) {
        free((uint8_t *)ptr - TA_HEADER);
    }
}

static hdrg_allocator_t counting_allocator(void)
{
    hdrg_allocator_t alloc;

    alloc.malloc_fn = counting_malloc;
    alloc.calloc_fn = counting_calloc;
    alloc.realloc_fn = counting_realloc;
    alloc.free_fn = counting_free;
    alloc.user = &state;

    return alloc;
}

static void HDRG_PLUGIN_CALL memory_deinit(void *user, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, user);
    seen.deinit_freed = true;
}

/* the plugin table's memory calls in init; a block kept to deinit */
static hdrg_status_t HDRG_PLUGIN_CALL memory_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    const size_t before = state.total_allocs;
    hdrg_status_t status;
    uint8_t *block, *zeros, *aligned, *page;
    size_t i;
    void *kept;

    block = (uint8_t *)api->mem_alloc(api, 100);
    zeros = (uint8_t *)api->mem_calloc(api, 10, 10);
    seen.zeroed = zeros != NULL;

    for (i = 0; seen.zeroed && i < 100; i++) {
        seen.zeroed = zeros[i] == 0;
    }

    block = (block != NULL) ? (uint8_t *)api->mem_realloc(api, block, 4000) : NULL;
    seen.grown = block != NULL;
    aligned = (uint8_t *)api->mem_aligned_alloc(api, 1000, 64);
    page = (uint8_t *)api->mem_aligned_alloc(api, 3, 16384);
    seen.aligned = aligned != NULL && page != NULL && (uintptr_t)aligned % 64u == 0 &&
                   (uintptr_t)page % 16384u == 0;
    /* refused: an alignment of no power of two or below a pointer's, sizes past size_t */
    seen.refused = api->mem_aligned_alloc(api, 8, 24) == NULL &&
                   api->mem_aligned_alloc(api, 8, 2) == NULL &&
                   api->mem_aligned_alloc(api, SIZE_MAX - 8u, 64) == NULL &&
                   api->mem_calloc(api, SIZE_MAX / 2u, 4) == NULL;

    if (seen.aligned) {
        memset(aligned, 0xA5, 1000);
        memset(page, 0x5A, 3);
    }

    api->mem_free(api, block);
    api->mem_free(api, zeros);
    api->mem_aligned_free(api, aligned);
    api->mem_aligned_free(api, page);

    kept = api->mem_alloc(api, 32);
    seen.during_init = state.total_allocs - before;

    status = api->plugin_set_info(plugin, TA_LIT("org.test.memory"), TA_LIT("Memory"),
                                  TA_LIT("1.0.0"), TA_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TA_LIT("MIT"),
                                                           TA_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->plugin_set_deinit(plugin, memory_deinit) : status;
    status = (status == HDRG_OK) ? api->plugin_set_user(plugin, kept) : status;

    if (status != HDRG_OK) {
        api->mem_free(api, kept);
    }

    return status;
}

static uint8_t *read_asset(const char *path, size_t *size)
{
    FILE *fp;
    uint8_t *data;
    long length;

    fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));

    length = ftell(fp);
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_SET));

    data = (uint8_t *)malloc((size_t)length);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_size_t((size_t)length, fread(data, 1, (size_t)length, fp));

    (void)fclose(fp);
    *size = (size_t)length;

    return data;
}

static void assert_balanced(void)
{
    TEST_ASSERT_FALSE(state.mismatch);
    TEST_ASSERT_EQUAL_size_t(0, state.live);
    TEST_ASSERT_EQUAL_size_t(state.total_allocs, state.total_frees);
}

static void test_all_allocations_balanced_and_output_outlives_context(void)
{
    hdrg_allocator_t alloc;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    uint8_t *pixels, *output;
    size_t output_size;

    alloc = counting_allocator();
    pixels = hdrg_test_image(128, 96);
    TEST_ASSERT_NOT_NULL(pixels);

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 128;
    raw.height = 96;
    raw.bit_depth = 8;
    raw.stride = 128 * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &output_size));
    TEST_ASSERT_TRUE(state.total_allocs > 0);
    TEST_ASSERT_TRUE(state.callocs > 0);

    /* the output buffer must outlive the context and free via its allocator */
    hdrg_context_destroy(ctx);
    TEST_ASSERT_TRUE(output_size > 0);
    TEST_ASSERT_EQUAL_HEX8(0xFF, output[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, output[1]);
    hdrg_free(output);

    assert_balanced();
    free(pixels);
}

/*
 * The paths of the allocator's users at once: libpng (a PNG), the ANIME chain of PREFILTER filters
 * and the SCALE stage (the GPU's under HDRG_GPU_AUTO where a device is), lcms2 (a P3 PQ profile)
 */
static void test_every_path_balanced(void)
{
    hdrg_encode_params_t encode;
    hdrg_context_desc_t desc;
    hdrg_allocator_t alloc;
    hdrg_params_t params;
    hdrg_context_t *ctx;
    uint8_t *data, *output;
    size_t size, output_size;

    HDRG_TEST_REQUIRE_BUILTIN(PNG);

    alloc = counting_allocator();
    data = read_asset(HDRG_TEST_ASSETS_DIR "/kodim23.png", &size);

    hdrg_context_desc_init(&desc);
    desc.allocator = &alloc;
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    hdrg_params_init(&params, HDRG_PRESET_ANIME);
    hdrg_encode_params_init(&encode);
#ifdef HDRG_WITH_LCMS2
    encode.output_mode = HDRG_OUTPUT_PQ;
#endif
    encode.output_width = 1024;

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert(ctx, data, size, &params, NULL, &encode, &output, &output_size));
    hdrg_free(output);
    hdrg_context_destroy(ctx);

    TEST_ASSERT_TRUE(state.total_allocs > 100);
    TEST_ASSERT_TRUE(state.callocs > 0);
    assert_balanced();
    free(data);
}

/*
 * A video session's paths under HDRG_GPU_AUTO: its frames, the PREFILTER chain's and the apply
 * pass on the GPU (src/core/gpu_apply) where a device is
 */
static void test_session_balanced(void)
{
    hdrg_context_desc_t desc;
    hdrg_allocator_t alloc;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent;
    uint8_t *image, *rows;

    alloc = counting_allocator();
    image = hdrg_test_image(64, 48);
    rows = (uint8_t *)malloc((size_t)64 * 48 * 3u);
    TEST_ASSERT_TRUE(image != NULL && rows != NULL);

    hdrg_context_desc_init(&desc);
    desc.allocator = &alloc;
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    memset(&format, 0, sizeof(format));
    format.width = 64;
    format.height = 48;
    format.bit_depth = 8;
    format.stride = (size_t)64 * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, NULL, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = image;
    sent = 0;

    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows;
        out.strides[0] = (size_t)64 * 3u;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < 3u) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    TEST_ASSERT_TRUE(state.total_allocs > 10);
    assert_balanced();
    free(image);
    free(rows);
}

/* a run of the sweep: its status (its context made and destroyed in it) */
typedef hdrg_status_t (*sweep_fn_t)(const hdrg_allocator_t *alloc);

static uint8_t *g_sweep_image;

/* a single-thread context of the allocator, without a GPU (the allocations in order) */
static hdrg_status_t sweep_context(const hdrg_allocator_t *alloc, hdrg_context_t **ctx)
{
    hdrg_context_desc_t desc;

    hdrg_context_desc_init(&desc);
    desc.allocator = alloc;
    desc.thread_count = 1;

    return hdrg_context_create(&desc, ctx);
}

/* a still of the test image (48 x 32) in the encode's form and the params' preset */
static hdrg_status_t sweep_still(const hdrg_allocator_t *alloc, uint8_t preset, uint8_t mode,
                                 uint32_t output_width)
{
    hdrg_encode_params_t encode;
    hdrg_params_t params;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t *output;
    size_t output_size;

    status = sweep_context(alloc, &ctx);
    if (status != HDRG_OK) {
        return status;
    }

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_sweep_image;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 48;
    raw.height = 32;
    raw.bit_depth = 8;
    raw.stride = 48 * 3;
    hdrg_params_init(&params, preset);
    hdrg_encode_params_init(&encode);
    encode.output_mode = mode;
    encode.output_width = output_width;
    output = NULL;

    status = hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, &output, &output_size);
    hdrg_free(output);
    hdrg_context_destroy(ctx);

    return status;
}

static hdrg_status_t sweep_gainmap(const hdrg_allocator_t *alloc)
{
    return sweep_still(alloc, HDRG_PRESET_PHOTO, HDRG_OUTPUT_GAINMAP, 0);
}

/* the ANIME chain of PREFILTER filters and the SCALE stage (twice the size) */
static hdrg_status_t sweep_anime(const hdrg_allocator_t *alloc)
{
    return sweep_still(alloc, HDRG_PRESET_ANIME, HDRG_OUTPUT_GAINMAP, 96);
}

#ifdef HDRG_WITH_LCMS2
/* lcms2's profiles (HYBRID's ICC and HAGC) */
static hdrg_status_t sweep_hybrid(const hdrg_allocator_t *alloc)
{
    return sweep_still(alloc, HDRG_PRESET_PHOTO, HDRG_OUTPUT_HYBRID, 0);
}
#endif

/* a session of three frames (PHOTO: its PREFILTER chain with MCTF, the grain resynthesized) */
static hdrg_status_t sweep_session(const hdrg_allocator_t *alloc)
{
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent;
    uint8_t rows[48 * 32 * 3];

    status = sweep_context(alloc, &ctx);
    if (status != HDRG_OK) {
        return status;
    }

    memset(&format, 0, sizeof(format));
    format.width = 48;
    format.height = 32;
    format.bit_depth = 8;
    format.stride = (size_t)48 * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);

    status = hdrg_session_create(ctx, &format, &params, NULL, NULL, &session);
    if (status != HDRG_OK) {
        hdrg_context_destroy(ctx);

        return status;
    }

    memset(&in, 0, sizeof(in));
    in.planes[0] = g_sweep_image;
    sent = 0;

    while (status == HDRG_OK) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows;
        out.strides[0] = (size_t)48 * 3u;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            status = hdrg_session_send_frame(session, (sent < 3u) ? &in : NULL);
            sent++;
        }
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return (status == HDRG_EOF) ? HDRG_OK : status;
}

/*
 * Every allocation of a run refused in turn (the k-th of a fresh run, for each of the run's
 * allocations): the run ends with a status (OK where the library does without the block) and
 * every block back to the allocator whatever it was: no leak, no double free, no crash
 */
static void sweep(sweep_fn_t fn, const char *name)
{
    hdrg_allocator_t alloc;
    hdrg_status_t status;
    size_t n, k, failed;
    char line[96];

    alloc = counting_allocator();
    memset(&state, 0, sizeof(state));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, fn(&alloc));
    assert_balanced();
    n = state.calls;
    TEST_ASSERT_TRUE(n > 10u);
    failed = 0;

    for (k = 1; k <= n; k += TA_SWEEP_STEP) {
        memset(&state, 0, sizeof(state));
        state.fail_at = k;
        status = fn(&alloc);
        failed += (status != HDRG_OK) ? 1u : 0u;

        snprintf(line, sizeof(line), "%s: allocation %zu of %zu refused (status %u)", name, k, n,
                 (unsigned)status);
        TEST_ASSERT_FALSE_MESSAGE(state.mismatch, line);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, state.live, line);
    }

    TEST_ASSERT_TRUE(failed > 0u);
}

static void test_refused_allocations_unwound(void)
{
    g_sweep_image = hdrg_test_image(48, 32);
    TEST_ASSERT_NOT_NULL(g_sweep_image);

    sweep(sweep_gainmap, "gain map");
    sweep(sweep_anime, "ANIME, 2x");
#ifdef HDRG_WITH_LCMS2
    sweep(sweep_hybrid, "hybrid");
#endif
    sweep(sweep_session, "session");

    free(g_sweep_image);
    g_sweep_image = NULL;
}

/* a plugin's mem_* reach the context's allocator, from init to deinit */
static void test_plugin_memory_through_the_allocator(void)
{
    hdrg_allocator_t alloc;
    hdrg_context_t *ctx;

    alloc = counting_allocator();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, memory_init));
    TEST_ASSERT_TRUE(seen.zeroed);
    TEST_ASSERT_TRUE(seen.grown);
    TEST_ASSERT_TRUE(seen.aligned);
    TEST_ASSERT_TRUE(seen.refused);
    /* malloc, calloc, two aligned blocks and the kept one (realloc grows the first in place) */
    TEST_ASSERT_TRUE(seen.during_init >= 5u);
    TEST_ASSERT_FALSE(seen.deinit_freed);

    hdrg_context_destroy(ctx);
    TEST_ASSERT_TRUE(seen.deinit_freed);
    assert_balanced();
}

/* without an allocator: the build's defaults, HDRG_ALIGNED_ALLOC for the aligned blocks */
static void test_plugin_memory_default_allocator(void)
{
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, memory_init));
    TEST_ASSERT_TRUE(seen.zeroed);
    TEST_ASSERT_TRUE(seen.grown);
    TEST_ASSERT_TRUE(seen.aligned);
    TEST_ASSERT_TRUE(seen.refused);
    TEST_ASSERT_EQUAL_size_t(0, seen.during_init);

    hdrg_context_destroy(ctx);
    TEST_ASSERT_TRUE(seen.deinit_freed);
}

static void test_incomplete_allocator_rejected(void)
{
    hdrg_allocator_t alloc;
    hdrg_context_t *ctx;

    alloc = counting_allocator();
    alloc.free_fn = NULL;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_test_context(&ctx, &alloc, 0));

    /* calloc_fn alone may be missing */
    alloc = counting_allocator();
    alloc.calloc_fn = NULL;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, 1));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_size_t(0, state.callocs);
    assert_balanced();
}

void setUp(void)
{
    memset(&state, 0, sizeof(state));
    memset(&seen, 0, sizeof(seen));
}

void tearDown(void)
{
}

int main(void)
{
    if (!hdrg_mutex_init(&lock)) {
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_all_allocations_balanced_and_output_outlives_context);
    RUN_TEST(test_every_path_balanced);
    RUN_TEST(test_session_balanced);
    RUN_TEST(test_plugin_memory_through_the_allocator);
    RUN_TEST(test_plugin_memory_default_allocator);
    RUN_TEST(test_incomplete_allocator_rejected);
    RUN_TEST(test_refused_allocations_unwound);

    hdrg_mutex_destroy(&lock);

    return UNITY_END();
}
