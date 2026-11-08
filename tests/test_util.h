/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TEST_UTIL_H
#define HDRG_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

#define HDRG_TEST_MAX(a, b) ((a) > (b) ? (a) : (b))
/* a session's format of a still's image (its pixels left out) */
#define HDRG_TEST_FORMAT(raw)                                                                      \
    (&(hdrg_session_format_t){(raw)->width, (raw)->height, (raw)->bit_depth, (raw)->stride,        \
                              (raw)->primaries, (raw)->transfer, (raw)->has_alpha})
#define HDRG_TEST_MIN(a, b) ((a) < (b) ? (a) : (b))
/* |expected - actual| <= tol in double (Unity is built without double support) */
#define HDRG_TEST_NEAR(tol, expected, actual)                                                      \
    TEST_ASSERT_TRUE(fabs((double)(expected) - (double)(actual)) <= (double)(tol))

/*
 * Builds without threads (the single-thread WASM builds): the tests of
 * threads and worker pools ignore themselves (the library then runs on the
 * calling thread)
 */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define HDRG_TEST_REQUIRE_THREADS() TEST_IGNORE_MESSAGE("no threads in this build")
#define HDRG_TEST_THREADS(n)        1u
#else
#define HDRG_TEST_REQUIRE_THREADS() ((void)0)
#define HDRG_TEST_THREADS(n)        (n) /* n threads, 1 in builds without */
#endif

/*
 * the optional built-in plugins the build has (HDRG_BUILTIN_*, HDRG_PLUGINS_EXCLUDE): a test of
 * one the build left out ignored; what a use of one returns (HDRG_ERR_UNSUPPORTED without it); a
 * preset's default strength (0 without it)
 */
#define HDRG_TEST_REQUIRE_BUILTIN(name)                                                            \
    do {                                                                                           \
        if (HDRG_BUILTIN_##name == 0) {                                                            \
            TEST_IGNORE_MESSAGE("the built-in plugin is not in this build");                       \
        }                                                                                          \
    } while (0)
#define HDRG_TEST_BUILTIN_STATUS(name) ((HDRG_BUILTIN_##name != 0) ? HDRG_OK : HDRG_ERR_UNSUPPORTED)
#define HDRG_TEST_BUILTIN_STRENGTH(name, strength) ((HDRG_BUILTIN_##name != 0) ? (strength) : 0.0f)

static inline void *hdrg_test_malloc(size_t size, void *user)
{
    (void)user;

    return malloc(size);
}

static inline void *hdrg_test_realloc(void *ptr, size_t size, void *user)
{
    (void)user;

    return realloc(ptr, size);
}

static inline void hdrg_test_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

static inline hdrg_allocator_t hdrg_test_allocator(void)
{
    hdrg_allocator_t alloc;

    alloc.malloc_fn = hdrg_test_malloc;
    alloc.calloc_fn = NULL;
    alloc.realloc_fn = hdrg_test_realloc;
    alloc.free_fn = hdrg_test_free;
    alloc.user = NULL;

    return alloc;
}

/* a context of the allocator (NULL = the default) and thread count (0 = one per core) */
static inline hdrg_status_t hdrg_test_context(hdrg_context_t **ctx, const hdrg_allocator_t *alloc,
                                              uint32_t threads)
{
    hdrg_context_desc_t desc;

    hdrg_context_desc_init(&desc);
    desc.allocator = alloc;
    desc.thread_count = threads;

    return hdrg_context_create(&desc, ctx);
}

/*
 * The writer of the context's conversions (id NULL: hdrg.jpeg) with its options: count "key=value"
 * texts (the CLI's --format-opt)
 */
static inline hdrg_status_t hdrg_test_writer(hdrg_context_t *ctx, const char *id,
                                             const char *const *opts, uint32_t count)
{
    hdrg_options_t *options;
    hdrg_status_t status;
    uint32_t i;
    size_t key_len;
    char key[65];

    status = hdrg_options_create(ctx, &options);

    for (i = 0; status == HDRG_OK && i < count; i++) {
        key_len = (size_t)(strchr(opts[i], '=') - opts[i]);
        memcpy(key, opts[i], key_len);
        key[key_len] = '\0';

        status = hdrg_options_set(options, key, opts[i] + key_len + 1u);
    }

    if (status == HDRG_OK) {
        status = hdrg_context_set_writer(ctx, id, options);
    }

    hdrg_options_destroy(options);

    return status;
}

/*
 * Synthetic test image: vertical sky-like gradient, a small specular dot
 * with a soft glow (a light source, not a flat graphic), and a large white
 * patch — exercises every analysis branch.
 */
static inline uint8_t *hdrg_test_image(uint32_t width, uint32_t height)
{
    uint32_t x, y, dx, dy, x0, y0, x1, y1;
    uint8_t *pixels, value, c;
    size_t idx;
    float ex, ey, d, w;

    pixels = (uint8_t *)malloc((size_t)width * height * 3);
    if (pixels == NULL) {
        return NULL;
    }

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            idx = ((size_t)y * width + x) * 3;
            value = (uint8_t)(140 + (110 * y) / (height ? height : 1));
            pixels[idx + 0] = (uint8_t)(value / 2);
            pixels[idx + 1] = (uint8_t)(value / 2 + 40);
            pixels[idx + 2] = value;
        }
    }

    /* specular dot near the top-left: glow fading over 6 px around it */
    x0 = width / 8;
    y0 = height / 8;
    x1 = x0 + width / 32 + 2;
    y1 = y0 + height / 32 + 2;
    for (y = (y0 > 6 ? y0 - 6 : 0); y < y1 + 6 && y < height; y++) {
        for (x = (x0 > 6 ? x0 - 6 : 0); x < x1 + 6 && x < width; x++) {
            ex = (x < x0) ? (float)(x0 - x) : (x >= x1 ? (float)(x - x1 + 1) : 0.0f);
            ey = (y < y0) ? (float)(y0 - y) : (y >= y1 ? (float)(y - y1 + 1) : 0.0f);
            d = sqrtf(ex * ex + ey * ey);
            w = 1.0f - d / 7.0f;

            if (w <= 0.0f) {
                continue;
            }

            w = 0.9f * w * w;
            idx = ((size_t)y * width + x) * 3;
            for (c = 0; c < 3; c++) {
                pixels[idx + c] =
                    (uint8_t)((float)pixels[idx + c] + (255.0f - (float)pixels[idx + c]) * w);
            }
        }
    }

    for (dy = 0; dy < height / 32 + 2; dy++) {
        for (dx = 0; dx < width / 32 + 2; dx++) {
            x = width / 8 + dx;
            y = height / 8 + dy;
            if (x < width && y < height) {
                idx = ((size_t)y * width + x) * 3;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
            }
        }
    }

    /* large white patch bottom half-right */
    for (y = height / 2; y < height; y++) {
        for (x = width / 2; x < width; x++) {
            idx = ((size_t)y * width + x) * 3;
            pixels[idx + 0] = 250;
            pixels[idx + 1] = 250;
            pixels[idx + 2] = 250;
        }
    }

    return pixels;
}

static inline bool hdrg_test_contains(const uint8_t *haystack, size_t size, const void *needle,
                                      size_t needle_len)
{
    size_t i;

    if (needle_len > size) {
        return false;
    }

    for (i = 0; i + needle_len <= size; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }

    return false;
}

#endif /* HDRG_TEST_UTIL_H */
