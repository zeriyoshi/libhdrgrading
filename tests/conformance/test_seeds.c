/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <dirent.h>
#include <stdio.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"

/*
 * The decoders over fuzz/fuzz_seeds.c's corpus (HDRG_SEEDS_DIR, written by the test_seeds_corpus
 * fixture: PNG of every color type and depth with their color chunks, GIF, JPEG with ICC / EXIF and
 * subsampled, the library's own outputs) and over each file cut short and with bytes changed: a
 * status for every input (the seeds' own converted), no crash, every block back to the allocator
 */
#define TS_CUTS  12u /* each file cut at size * k / TS_CUTS */
#define TS_FLIPS 12u /* and with one byte changed at a hashed place */
#define TS_PATH  512u

typedef struct {
    size_t live;
    bool mismatch;
} ts_count_t;

static ts_count_t g_count;
static char g_dir[TS_PATH];

static void *ts_malloc(size_t size, void *user)
{
    uint8_t *block;

    (void)user;
    block = (uint8_t *)malloc(size + 16u);
    g_count.live += (block != NULL) ? 1u : 0u;

    return (block != NULL) ? block + 16u : NULL;
}

static void *ts_realloc(void *ptr, size_t size, void *user)
{
    uint8_t *block;

    if (ptr == NULL) {
        return ts_malloc(size, user);
    }

    block = (uint8_t *)realloc((uint8_t *)ptr - 16u, size + 16u);

    return (block != NULL) ? block + 16u : NULL;
}

static void ts_free(void *ptr, void *user)
{
    (void)user;

    if (ptr == NULL || g_count.live == 0) {
        g_count.mismatch = true;

        return;
    }

    g_count.live--;
    free((uint8_t *)ptr - 16u);
}

static uint8_t *ts_read(const char *path, size_t *size)
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

/* one input through a context of its own: its status, every block back */
static hdrg_status_t ts_convert(const uint8_t *data, size_t size, const char *what)
{
    hdrg_allocator_t alloc;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t *output;
    size_t output_size;

    memset(&g_count, 0, sizeof(g_count));
    alloc.malloc_fn = ts_malloc;
    alloc.calloc_fn = NULL;
    alloc.realloc_fn = ts_realloc;
    alloc.free_fn = ts_free;
    alloc.user = NULL;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, 1));
    output = NULL;
    status = hdrg_convert(ctx, data, size, NULL, NULL, NULL, &output, &output_size);
    hdrg_free(output);
    hdrg_context_destroy(ctx);

    TEST_ASSERT_FALSE_MESSAGE(g_count.mismatch, what);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, g_count.live, what);

    return status;
}

/* a seed of a reader the build left out (HDRG_PLUGINS_EXCLUDE): no reader takes it */
static bool ts_left_out(const char *name)
{
    const size_t n = strlen(name);

    return n > 4u && ((HDRG_BUILTIN_PNG == 0 && strcmp(name + n - 4u, ".png") == 0) ||
                      (HDRG_BUILTIN_GIF == 0 && strcmp(name + n - 4u, ".gif") == 0));
}

static void test_seeds(void)
{
    struct dirent *entry;
    DIR *dir;
    hdrg_status_t status;
    uint32_t files, converted, k, at;
    uint8_t *data, *bent;
    size_t size;
    char path[TS_PATH + 264u], what[TS_PATH + 320u];

    dir = opendir(g_dir);
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "HDRG_SEEDS_DIR: the fixture's corpus");
    files = 0;
    converted = 0;

    for (entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", g_dir, entry->d_name);
        data = ts_read(path, &size);
        bent = (uint8_t *)malloc(size);
        TEST_ASSERT_NOT_NULL(bent);
        files++;

        /* the seed itself: converted, or refused for what it is (a PQ input), never a fault */
        status = ts_convert(data, size, entry->d_name);
        snprintf(what, sizeof(what), "%s: status %u", entry->d_name, (unsigned)status);
        TEST_ASSERT_TRUE_MESSAGE(status != HDRG_ERR_INTERNAL && status != HDRG_ERR_NOMEM, what);
        converted += (status == HDRG_OK ||
                      (status == HDRG_ERR_UNSUPPORTED_FORMAT && ts_left_out(entry->d_name)))
                         ? 1u
                         : 0u;

        if (status != HDRG_OK) {
            TEST_MESSAGE(what);
        }

        for (k = 0; k < TS_CUTS; k++) {
            snprintf(what, sizeof(what), "%s cut at %u / %u", entry->d_name, k, TS_CUTS);
            (void)ts_convert(data, size * k / TS_CUTS, what);
        }

        for (k = 0; k < TS_FLIPS; k++) {
            memcpy(bent, data, size);
            at = hdrg_hash_u32(k, (uint32_t)size, files, 0u) % (uint32_t)size;
            bent[at] = (uint8_t)(bent[at] ^ (uint8_t)(1u + hdrg_hash_u32(k, at, files, 1u) % 255u));
            snprintf(what, sizeof(what), "%s byte %u changed", entry->d_name, at);
            (void)ts_convert(bent, size, what);
        }

        free(bent);
        free(data);
    }

    /* the 18 forms of every build (lcms2 adds 5 with an ICC), every one converted */
    (void)closedir(dir);
    TEST_ASSERT_TRUE(files >= 18u);
    TEST_ASSERT_EQUAL_UINT32(files, converted);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    const char *dir = getenv("HDRG_SEEDS_DIR");

    if (dir == NULL || strlen(dir) >= sizeof(g_dir)) {
        fprintf(stderr, "HDRG_SEEDS_DIR is not set\n");

        return 1;
    }

    memcpy(g_dir, dir, strlen(dir) + 1u);

    UNITY_BEGIN();
    RUN_TEST(test_seeds);

    return UNITY_END();
}
