/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/thread.h"

#define TEST_THREADS     4u
#define TEST_ITERATIONS  20000u
#define TEST_PING_ROUNDS 2000u
#define TEST_PAYLOAD     64u

typedef struct {
    hdrg_mutex_t mutex;
    hdrg_cond_t cond;
    hdrg_atomic_u32_t counter, ready;
    uint32_t plain, turn, child_rounds;
    uint32_t payload[TEST_PAYLOAD];
} test_shared_t;

static test_shared_t shared;

static void count_atomic(void *arg)
{
    uint32_t i;

    (void)arg;

    for (i = 0; i < TEST_ITERATIONS; i++) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&shared.counter, 1);
    }
}

static void count_locked(void *arg)
{
    uint32_t i;

    (void)arg;

    for (i = 0; i < TEST_ITERATIONS; i++) {
        hdrg_mutex_lock(&shared.mutex);
        shared.plain++;
        hdrg_mutex_unlock(&shared.mutex);
    }
}

/* 子スレッド側: turn == 1 を待って 0 に戻す */
static void ping_child(void *arg)
{
    uint32_t i;

    (void)arg;

    for (i = 0; i < TEST_PING_ROUNDS; i++) {
        hdrg_mutex_lock(&shared.mutex);
        while (shared.turn != 1) {
            hdrg_cond_wait(&shared.cond, &shared.mutex);
        }

        shared.child_rounds++;
        shared.turn = 0;
        hdrg_cond_broadcast(&shared.cond);
        hdrg_mutex_unlock(&shared.mutex);
    }
}

/* 普通の書き込みを release ストアで公開する */
static void publish(void *arg)
{
    uint32_t i;

    (void)arg;

    for (i = 0; i < TEST_PAYLOAD; i++) {
        shared.payload[i] = (i * 7u) + 1u;
    }

    hdrg_atomic_u32_store_release(&shared.ready, 1);
}

static void test_atomic_single_thread(void)
{
    hdrg_atomic_u32_t a;
    uint32_t expected;

    hdrg_atomic_u32_init(&a, 5);
    TEST_ASSERT_EQUAL_UINT32(5, hdrg_atomic_u32_load_relaxed(&a));
    TEST_ASSERT_EQUAL_UINT32(5, hdrg_atomic_u32_fetch_add_relaxed(&a, 3));
    TEST_ASSERT_EQUAL_UINT32(8, hdrg_atomic_u32_load_acquire(&a));

    /* 符号なしの折り返し: 8 + (2^32 - 1) = 7 */
    TEST_ASSERT_EQUAL_UINT32(8, hdrg_atomic_u32_fetch_add_acq_rel(&a, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(7, hdrg_atomic_u32_load_relaxed(&a));

    hdrg_atomic_u32_store_release(&a, 100);
    TEST_ASSERT_EQUAL_UINT32(100, hdrg_atomic_u32_load_acquire(&a));
    hdrg_atomic_u32_store_relaxed(&a, 1);
    TEST_ASSERT_EQUAL_UINT32(1, hdrg_atomic_u32_load_relaxed(&a));

    /* CAS: 失敗すると expected に現在値が入り、成功すると書き換わる */
    expected = 2;
    TEST_ASSERT_FALSE(hdrg_atomic_u32_cas_acq_rel(&a, &expected, 9));
    TEST_ASSERT_EQUAL_UINT32(1, expected);
    TEST_ASSERT_EQUAL_UINT32(1, hdrg_atomic_u32_load_relaxed(&a));
    TEST_ASSERT_TRUE(hdrg_atomic_u32_cas_acq_rel(&a, &expected, 9));
    TEST_ASSERT_EQUAL_UINT32(9, hdrg_atomic_u32_load_acquire(&a));
}

static void test_atomic_counter_threads(void)
{
    hdrg_thread_t threads[TEST_THREADS];
    uint32_t i;

    HDRG_TEST_REQUIRE_THREADS();

    hdrg_atomic_u32_init(&shared.counter, 0);

    for (i = 0; i < TEST_THREADS; i++) {
        TEST_ASSERT_TRUE(hdrg_thread_create(&threads[i], count_atomic, NULL));
    }

    for (i = 0; i < TEST_THREADS; i++) {
        hdrg_thread_join(&threads[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(TEST_THREADS * TEST_ITERATIONS,
                             hdrg_atomic_u32_load_acquire(&shared.counter));
}

static void test_mutex_counter_threads(void)
{
    hdrg_thread_t threads[TEST_THREADS];
    uint32_t i;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_TRUE(hdrg_mutex_init(&shared.mutex));
    shared.plain = 0;

    for (i = 0; i < TEST_THREADS; i++) {
        TEST_ASSERT_TRUE(hdrg_thread_create(&threads[i], count_locked, NULL));
    }

    for (i = 0; i < TEST_THREADS; i++) {
        hdrg_thread_join(&threads[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(TEST_THREADS * TEST_ITERATIONS, shared.plain);
    hdrg_mutex_destroy(&shared.mutex);
}

/* condvar で 2 スレッドが交互に進む (取りこぼしがあれば止まる) */
static void test_cond_ping_pong(void)
{
    hdrg_thread_t child;
    uint32_t i, main_rounds;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_TRUE(hdrg_mutex_init(&shared.mutex));
    TEST_ASSERT_TRUE(hdrg_cond_init(&shared.cond));
    shared.turn = 0;
    shared.child_rounds = 0;
    main_rounds = 0;

    TEST_ASSERT_TRUE(hdrg_thread_create(&child, ping_child, NULL));

    for (i = 0; i < TEST_PING_ROUNDS; i++) {
        hdrg_mutex_lock(&shared.mutex);
        while (shared.turn != 0) {
            hdrg_cond_wait(&shared.cond, &shared.mutex);
        }

        TEST_ASSERT_EQUAL_UINT32(i, shared.child_rounds);
        main_rounds++;
        shared.turn = 1;
        hdrg_cond_broadcast(&shared.cond);
        hdrg_mutex_unlock(&shared.mutex);
    }

    hdrg_thread_join(&child);

    TEST_ASSERT_EQUAL_UINT32(TEST_PING_ROUNDS, main_rounds);
    TEST_ASSERT_EQUAL_UINT32(TEST_PING_ROUNDS, shared.child_rounds);
    TEST_ASSERT_EQUAL_UINT32(0, shared.turn);
    hdrg_cond_destroy(&shared.cond);
    hdrg_mutex_destroy(&shared.mutex);
}

/* release / acquire の組で、フラグより前の普通の書き込みが見える */
static void test_release_acquire_publish(void)
{
    hdrg_thread_t child;
    uint32_t i;

    HDRG_TEST_REQUIRE_THREADS();

    hdrg_atomic_u32_init(&shared.ready, 0);
    TEST_ASSERT_TRUE(hdrg_thread_create(&child, publish, NULL));

    while (hdrg_atomic_u32_load_acquire(&shared.ready) == 0) {
    }

    for (i = 0; i < TEST_PAYLOAD; i++) {
        TEST_ASSERT_EQUAL_UINT32((i * 7u) + 1u, shared.payload[i]);
    }

    hdrg_thread_join(&child);
}

static void test_cpu_count(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, hdrg_thread_cpu_count());
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_atomic_single_thread);
    RUN_TEST(test_atomic_counter_threads);
    RUN_TEST(test_mutex_counter_threads);
    RUN_TEST(test_cond_ping_pong);
    RUN_TEST(test_release_acquire_publish);
    RUN_TEST(test_cpu_count);

    return UNITY_END();
}
