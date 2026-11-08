/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "core/analyze.h"

/* two blobs: a U-shape (single component despite the gap in row 0) and an
 * isolated dot at (7,7) diagonally touching the square blob */
static const uint8_t mask[8 * 8] = {
    1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1,
};

static uint32_t labels[8 * 8];
static uint32_t count;

static void test_component_count(void)
{
    TEST_ASSERT_EQUAL_UINT32(3, count);
}

static void test_u_shape_merged(void)
{
    TEST_ASSERT_NOT_EQUAL(0, labels[0]);
    TEST_ASSERT_EQUAL_UINT32(labels[0], labels[3]);
    TEST_ASSERT_EQUAL_UINT32(labels[0], labels[2 * 8 + 3]);
}

static void test_square_blob_distinct(void)
{
    TEST_ASSERT_NOT_EQUAL(0, labels[5 * 8 + 5]);
    TEST_ASSERT_NOT_EQUAL(labels[0], labels[5 * 8 + 5]);
    TEST_ASSERT_EQUAL_UINT32(labels[5 * 8 + 5], labels[6 * 8 + 6]);
}

static void test_diagonal_not_connected(void)
{
    TEST_ASSERT_NOT_EQUAL(0, labels[7 * 8 + 7]);
    TEST_ASSERT_NOT_EQUAL(labels[6 * 8 + 6], labels[7 * 8 + 7]);
}

static void test_background_stays_zero(void)
{
    uint32_t i;

    for (i = 0; i < 64; i++) {
        if (!mask[i]) {
            TEST_ASSERT_EQUAL_UINT32(0, labels[i]);
        }
    }
}

void setUp(void)
{
    hdrg_allocator_t alloc;

    alloc = hdrg_test_allocator();
    memset(labels, 0, sizeof(labels));
    count = 0;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_label_components(&alloc, mask, 8, 8, labels, &count));
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_component_count);
    RUN_TEST(test_u_shape_merged);
    RUN_TEST(test_square_blob_distinct);
    RUN_TEST(test_diagonal_not_connected);
    RUN_TEST(test_background_stays_zero);

    return UNITY_END();
}
