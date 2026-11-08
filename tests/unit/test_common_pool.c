/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/pool.h"
#include "hdrgrading/foundation/thread.h"

#define TEST_TASKS         1000u
#define TEST_POOL_SIZES    5u
#define TEST_OUTER         64u
#define TEST_INNER         16u
#define TEST_ROWS_MAX      1024u
#define TEST_CALLERS       3u
#define TEST_CALLER_ROUNDS 100u
#define TEST_CALLER_TASKS  500u
#define TEST_HOG_TASKS     1000000u
#define TEST_HOG_SPIN_US   50u
#define TEST_PAIR_WAIT_US  2000000u
#define TEST_START_WAIT_US 10000000u
#define TEST_INIT_SLOTS    9u

typedef struct {
    hdrg_atomic_u32_t runs[TEST_TASKS];
    hdrg_atomic_u32_t bad_worker;
    uint64_t values[TEST_TASKS];
    uint32_t concurrency;
    float floats[TEST_TASKS];
} test_job_t;

typedef struct {
    uint32_t order[TEST_TASKS];
    uint32_t count, worker_sum;
} test_order_t;

typedef struct {
    hdrg_pool_t *outer_pool, *inner_pool;
    uint32_t results[TEST_OUTER];
} test_nest_t;

typedef struct {
    uint32_t order[TEST_INNER];
    uint32_t outer, expected_worker, count;
    bool ok;
} test_inner_t;

typedef struct {
    hdrg_atomic_u32_t chunk_runs[TEST_ROWS_MAX], row_runs[TEST_ROWS_MAX];
    hdrg_atomic_u32_t bad;
    uint32_t begins[TEST_ROWS_MAX], ends[TEST_ROWS_MAX];
    uint32_t rows, step, concurrency;
} test_rows_t;

typedef struct {
    hdrg_pool_t *pool;
    test_job_t *job;
    uint32_t max_workers, tasks, rounds, failures;
} test_caller_t;

typedef struct {
    hdrg_pool_t *pool;
    hdrg_atomic_u32_t worker_hits, hog_runs, stop, arrivals, paired;
} test_fair_t;

typedef struct {
    hdrg_atomic_u32_t active, peak, bad_worker;
    uint32_t concurrency;
} test_limit_t;

typedef struct {
    hdrg_atomic_u32_t calls[TEST_INIT_SLOTS];
} test_init_t;

typedef struct {
    hdrg_atomic_u32_t mallocs, frees;
} test_alloc_count_t;

static const uint32_t pool_sizes[TEST_POOL_SIZES] = {0, 1, 2, 4, 8};
static test_job_t reference_job, jobs[TEST_CALLERS];
static test_order_t order;
static test_nest_t nest;
static test_rows_t row_record;
static test_fair_t fair;
static test_limit_t limit;
static test_init_t inits;
static test_alloc_count_t alloc_count;
static hdrg_atomic_u32_t never_calls;
static HDRG_THREAD_LOCAL uint32_t current_outer;

/* タスク番号だけで決まる重めの計算 (スレッド数に依存しない出力の検査用) */
static inline uint64_t task_value(uint32_t task)
{
    uint64_t x;
    uint32_t i;

    x = ((uint64_t)task * UINT64_C(0x9E3779B97F4A7C15)) + 1u;
    for (i = 0; i < 32; i++) {
        x ^= x >> 29;
        x *= UINT64_C(0xBF58476D1CE4E5B9);
        x ^= x >> 32;
    }

    return x;
}

static inline float task_float(uint32_t task)
{
    uint32_t i;
    float v;

    v = (float)task * 0.37f;
    for (i = 0; i < 32; i++) {
        v = (v * 1.0001f) + (0.5f / (1.0f + v));
    }

    return v;
}

/* float はビット列で比べる (スレッド数によらずバイト一致すること) */
static inline uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));

    return bits;
}

static inline uint64_t now_us(void)
{
    struct timespec ts;

    (void)timespec_get(&ts, TIME_UTC);

    return ((uint64_t)ts.tv_sec * UINT64_C(1000000)) + ((uint64_t)ts.tv_nsec / UINT64_C(1000));
}

static inline void spin_us(uint64_t us)
{
    uint64_t end;

    end = now_us() + us;
    while (now_us() < end) {
    }
}

static void job_reset(test_job_t *job, uint32_t concurrency)
{
    uint32_t i;

    for (i = 0; i < TEST_TASKS; i++) {
        hdrg_atomic_u32_init(&job->runs[i], 0);
        job->values[i] = 0;
        job->floats[i] = 0.0f;
    }

    hdrg_atomic_u32_init(&job->bad_worker, 0);
    job->concurrency = concurrency;
}

/* 各タスクがちょうど 1 回走り、インライン実行の参照とビット一致するか */
static uint32_t job_errors(test_job_t *job, uint32_t tasks)
{
    uint32_t errors, i;

    errors = hdrg_atomic_u32_load_acquire(&job->bad_worker);
    for (i = 0; i < tasks; i++) {
        if (hdrg_atomic_u32_load_acquire(&job->runs[i]) != 1 ||
            job->values[i] != reference_job.values[i] ||
            float_bits(job->floats[i]) != float_bits(reference_job.floats[i])) {
            errors++;
        }
    }

    return errors;
}

static void record_task(uint32_t task, uint32_t worker, void *arg)
{
    test_job_t *job = (test_job_t *)arg;

    (void)hdrg_atomic_u32_fetch_add_relaxed(&job->runs[task], 1);
    job->values[task] = task_value(task);
    job->floats[task] = task_float(task);

    if (worker >= job->concurrency) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&job->bad_worker, 1);
    }
}

static void record_order(uint32_t task, uint32_t worker, void *arg)
{
    test_order_t *record = (test_order_t *)arg;

    if (record->count < TEST_TASKS) {
        record->order[record->count] = task;
    }

    record->count++;
    record->worker_sum += worker;
}

static void never_task(uint32_t task, uint32_t worker, void *arg)
{
    (void)task;
    (void)worker;
    (void)arg;
    (void)hdrg_atomic_u32_fetch_add_relaxed(&never_calls, 1);
}

/* 入れ子の内側: 外側と同じスレッドで、タスク順に走るはず */
static void nested_inner(uint32_t task, uint32_t worker, void *arg)
{
    test_inner_t *inner = (test_inner_t *)arg;

    if (current_outer != inner->outer + 1 || worker != inner->expected_worker ||
        inner->count >= TEST_INNER) {
        inner->ok = false;

        return;
    }

    inner->order[inner->count] = task;
    inner->count++;
}

static void nested_outer(uint32_t task, uint32_t worker, void *arg)
{
    test_nest_t *record = (test_nest_t *)arg;
    test_inner_t inner;
    uint32_t i;

    inner.outer = task;
    inner.expected_worker = (record->inner_pool == record->outer_pool) ? worker : 0;
    inner.count = 0;
    inner.ok = true;
    current_outer = task + 1;

    hdrg_pool_parallel_for(record->inner_pool, TEST_INNER, UINT32_MAX, nested_inner, &inner);

    current_outer = 0;

    for (i = 0; i < inner.count; i++) {
        if (inner.order[i] != i) {
            inner.ok = false;
        }
    }

    record->results[task] = (inner.ok && inner.count == TEST_INNER) ? 1u : 2u;
}

static void record_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    test_rows_t *record = (test_rows_t *)arg;
    uint32_t chunk, row;

    chunk = row_begin / record->step;
    if (row_end <= row_begin || row_end > record->rows || chunk >= TEST_ROWS_MAX ||
        worker >= record->concurrency) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->bad, 1);

        return;
    }

    (void)hdrg_atomic_u32_fetch_add_relaxed(&record->chunk_runs[chunk], 1);
    record->begins[chunk] = row_begin;
    record->ends[chunk] = row_end;

    for (row = row_begin; row < row_end; row++) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->row_runs[row], 1);
    }
}

static void caller_main(void *arg)
{
    test_caller_t *caller = (test_caller_t *)arg;
    uint32_t round;

    for (round = 0; round < caller->rounds; round++) {
        job_reset(caller->job, hdrg_pool_concurrency(caller->pool));
        hdrg_pool_parallel_for(caller->pool, caller->tasks, caller->max_workers, record_task,
                               caller->job);
        caller->failures += job_errors(caller->job, caller->tasks);
    }
}

/* 長い呼び出し: 細かいタスクを大量に持ち、stop 後はすぐ終わる */
static void hog_task(uint32_t task, uint32_t worker, void *arg)
{
    test_fair_t *record = (test_fair_t *)arg;

    (void)task;
    (void)hdrg_atomic_u32_fetch_add_relaxed(&record->hog_runs, 1);
    if (hdrg_atomic_u32_load_acquire(&record->stop) != 0) {
        return;
    }

    if (worker != 0) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->worker_hits, 1);
    }

    spin_us(TEST_HOG_SPIN_US);
}

static void hog_main(void *arg)
{
    test_fair_t *record = (test_fair_t *)arg;

    hdrg_pool_parallel_for(record->pool, TEST_HOG_TASKS, UINT32_MAX, hog_task, record);
}

/* 別スレッドで長い呼び出しを始め、プールのスレッドが hits 個のタスクを取るまで待つ */
static bool hog_start(hdrg_thread_t *thread, hdrg_pool_t *pool, uint32_t hits)
{
    uint64_t deadline;

    fair.pool = pool;
    hdrg_atomic_u32_init(&fair.worker_hits, 0);
    hdrg_atomic_u32_init(&fair.hog_runs, 0);
    hdrg_atomic_u32_init(&fair.stop, 0);
    hdrg_atomic_u32_init(&fair.arrivals, 0);
    hdrg_atomic_u32_init(&fair.paired, 0);

    if (!hdrg_thread_create(thread, hog_main, &fair)) {
        return false;
    }

    deadline = now_us() + TEST_START_WAIT_US;
    while (hdrg_atomic_u32_load_acquire(&fair.worker_hits) < hits && now_us() < deadline) {
    }

    return hdrg_atomic_u32_load_acquire(&fair.worker_hits) >= hits;
}

static void hog_stop(hdrg_thread_t *thread)
{
    hdrg_atomic_u32_store_release(&fair.stop, 1);
    hdrg_thread_join(thread);
}

/* 2 つのタスクが同時に走らないと揃わない (呼び出し元 + プールのスレッドが要る) */
static void pair_task(uint32_t task, uint32_t worker, void *arg)
{
    test_fair_t *record = (test_fair_t *)arg;
    uint64_t deadline;

    (void)task;
    (void)worker;
    deadline = now_us() + TEST_PAIR_WAIT_US;
    (void)hdrg_atomic_u32_fetch_add_acq_rel(&record->arrivals, 1);

    while (hdrg_atomic_u32_load_acquire(&record->arrivals) < 2 && now_us() < deadline) {
    }

    if (hdrg_atomic_u32_load_acquire(&record->arrivals) >= 2) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->paired, 1);
    }
}

static void limit_task(uint32_t task, uint32_t worker, void *arg)
{
    test_limit_t *record = (test_limit_t *)arg;
    uint32_t active, peak;

    (void)task;
    active = hdrg_atomic_u32_fetch_add_acq_rel(&record->active, 1) + 1;
    peak = hdrg_atomic_u32_load_relaxed(&record->peak);
    while (active > peak && !hdrg_atomic_u32_cas_acq_rel(&record->peak, &peak, active)) {
    }

    if (worker >= record->concurrency) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->bad_worker, 1);
    }

    spin_us(20);
    (void)hdrg_atomic_u32_fetch_add_acq_rel(&record->active, UINT32_MAX);
}

static void count_init(uint32_t worker_index, void *user)
{
    test_init_t *record = (test_init_t *)user;

    if (worker_index < TEST_INIT_SLOTS) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->calls[worker_index], 1);
    }
}

static void *counting_malloc(size_t size, void *user)
{
    test_alloc_count_t *record = (test_alloc_count_t *)user;

    (void)hdrg_atomic_u32_fetch_add_relaxed(&record->mallocs, 1);

    return malloc(size);
}

static void *counting_realloc(void *ptr, size_t size, void *user)
{
    test_alloc_count_t *record = (test_alloc_count_t *)user;

    (void)hdrg_atomic_u32_fetch_add_relaxed(&record->mallocs, 1);

    return realloc(ptr, size);
}

static void counting_free(void *ptr, void *user)
{
    test_alloc_count_t *record = (test_alloc_count_t *)user;

    if (ptr != NULL) {
        (void)hdrg_atomic_u32_fetch_add_relaxed(&record->frees, 1);
    }

    free(ptr);
}

static void test_each_task_once_identical_outputs(void)
{
    static const uint32_t max_workers[3] = {UINT32_MAX, 2, 3};
    hdrg_pool_t *pool;
    uint32_t s, m, repeat;

    HDRG_TEST_REQUIRE_THREADS();

    for (s = 0; s < TEST_POOL_SIZES; s++) {
        TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, pool_sizes[s], NULL, NULL));
        for (m = 0; m < 3; m++) {
            for (repeat = 0; repeat < 10; repeat++) {
                job_reset(&jobs[0], hdrg_pool_concurrency(pool));
                hdrg_pool_parallel_for(pool, TEST_TASKS, max_workers[m], record_task, &jobs[0]);
                TEST_ASSERT_EQUAL_UINT32(0, job_errors(&jobs[0], TEST_TASKS));
            }
        }

        hdrg_pool_destroy(pool);
    }
}

static void test_inline_runs_in_task_order(void)
{
    hdrg_pool_t *pool, *empty;
    uint32_t c, i;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, 4, NULL, NULL));
    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&empty, NULL, 0, NULL, NULL));

    /* NULL / max_workers 1 / max_workers 0 / サイズ 0 のプールはすべて呼び出し元でタスク順 */
    for (c = 0; c < 4; c++) {
        order.count = 0;
        order.worker_sum = 0;

        if (c == 0) {
            hdrg_pool_parallel_for(NULL, 100, UINT32_MAX, record_order, &order);
        } else if (c == 1) {
            hdrg_pool_parallel_for(pool, 100, 1, record_order, &order);
        } else if (c == 2) {
            hdrg_pool_parallel_for(pool, 100, 0, record_order, &order);
        } else {
            hdrg_pool_parallel_for(empty, 100, UINT32_MAX, record_order, &order);
        }

        TEST_ASSERT_EQUAL_UINT32(100, order.count);
        TEST_ASSERT_EQUAL_UINT32(0, order.worker_sum);

        for (i = 0; i < 100; i++) {
            TEST_ASSERT_EQUAL_UINT32(i, order.order[i]);
        }
    }

    /* タスク 1 個は起床させずに呼び出し元で走る */
    order.count = 0;
    order.worker_sum = 0;
    hdrg_pool_parallel_for(pool, 1, UINT32_MAX, record_order, &order);
    TEST_ASSERT_EQUAL_UINT32(1, order.count);
    TEST_ASSERT_EQUAL_UINT32(0, order.worker_sum);

    hdrg_pool_destroy(empty);
    hdrg_pool_destroy(pool);
}

static void test_nested_runs_inline(void)
{
    hdrg_pool_t *pool, *other, *inner_pools[3];
    uint32_t c, i;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, 4, NULL, NULL));
    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&other, NULL, 2, NULL, NULL));
    inner_pools[0] = pool;
    inner_pools[1] = other;
    inner_pools[2] = NULL;

    /* 同じプール / 別のプール / NULL への入れ子。どれもデッドロックせずインラインで走る */
    for (c = 0; c < 3; c++) {
        nest.outer_pool = pool;
        nest.inner_pool = inner_pools[c];
        memset(nest.results, 0, sizeof(nest.results));

        hdrg_pool_parallel_for(pool, TEST_OUTER, UINT32_MAX, nested_outer, &nest);

        for (i = 0; i < TEST_OUTER; i++) {
            TEST_ASSERT_EQUAL_UINT32(1, nest.results[i]);
        }
    }

    hdrg_pool_destroy(other);
    hdrg_pool_destroy(pool);
}

static void test_concurrent_callers(void)
{
    static const uint32_t max_workers[TEST_CALLERS] = {UINT32_MAX, 2, 3};
    test_caller_t callers[TEST_CALLERS];
    hdrg_thread_t threads[TEST_CALLERS - 1];
    hdrg_pool_t *pool;
    uint32_t i;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, 4, NULL, NULL));

    for (i = 0; i < TEST_CALLERS; i++) {
        callers[i].pool = pool;
        callers[i].job = &jobs[i];
        callers[i].max_workers = max_workers[i];
        callers[i].tasks = TEST_CALLER_TASKS - (i * 37u);
        callers[i].rounds = TEST_CALLER_ROUNDS;
        callers[i].failures = 0;
    }

    for (i = 0; i + 1 < TEST_CALLERS; i++) {
        TEST_ASSERT_TRUE(hdrg_thread_create(&threads[i], caller_main, &callers[i]));
    }

    caller_main(&callers[TEST_CALLERS - 1]);

    for (i = 0; i + 1 < TEST_CALLERS; i++) {
        hdrg_thread_join(&threads[i]);
    }

    for (i = 0; i < TEST_CALLERS; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, callers[i].failures);
    }

    hdrg_pool_destroy(pool);
}

/*
 * 長い呼び出しが走っている間に来た短い呼び出しにも、プールのスレッドが回ってくる。
 * 先着順で長い方に張り付く実装だと pair_task が揃わず (TEST_PAIR_WAIT_US で打ち切り) 失敗する。
 */
static void test_fair_between_callers(void)
{
    hdrg_thread_t hog;
    hdrg_pool_t *pool;
    bool started;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, 2, NULL, NULL));
    started = hog_start(&hog, pool, 4);

    hdrg_pool_parallel_for(pool, 2, UINT32_MAX, pair_task, &fair);

    hog_stop(&hog);
    hdrg_pool_destroy(pool);

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_EQUAL_UINT32(2, hdrg_atomic_u32_load_acquire(&fair.paired));
    TEST_ASSERT_EQUAL_UINT32(TEST_HOG_TASKS, hdrg_atomic_u32_load_acquire(&fair.hog_runs));
}

/* 起きているスレッドが多くても (別の呼び出しの間を回っていても) max_workers を超えない */
static void test_max_workers_limit(void)
{
    static const uint32_t max_workers[3] = {2, 3, 5};
    hdrg_thread_t hog;
    hdrg_pool_t *pool;
    uint32_t peaks[3], actives[3], bad[3], m;
    bool started;

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, 8, NULL, NULL));
    started = hog_start(&hog, pool, 16);

    for (m = 0; m < 3; m++) {
        hdrg_atomic_u32_init(&limit.active, 0);
        hdrg_atomic_u32_init(&limit.peak, 0);
        hdrg_atomic_u32_init(&limit.bad_worker, 0);
        limit.concurrency = hdrg_pool_concurrency(pool);

        hdrg_pool_parallel_for(pool, 200, max_workers[m], limit_task, &limit);

        peaks[m] = hdrg_atomic_u32_load_acquire(&limit.peak);
        actives[m] = hdrg_atomic_u32_load_acquire(&limit.active);
        bad[m] = hdrg_atomic_u32_load_acquire(&limit.bad_worker);
    }

    hog_stop(&hog);
    hdrg_pool_destroy(pool);

    TEST_ASSERT_TRUE(started);

    for (m = 0; m < 3; m++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(max_workers[m], peaks[m]);
        TEST_ASSERT_EQUAL_UINT32(0, actives[m]);
        TEST_ASSERT_EQUAL_UINT32(0, bad[m]);
    }
}

/* 行の分割はスレッド数によらず [k * grain, min((k + 1) * grain, rows)) */
static void test_rows_boundaries_independent(void)
{
    static const uint32_t row_counts[5] = {0, 1, 7, 1000, 1001}, grains[5] = {0, 1, 7, 64, 2000};
    hdrg_pool_t *pool;
    uint32_t s, r, g, k, step, chunks, begin, end;

    HDRG_TEST_REQUIRE_THREADS();

    for (s = 0; s < TEST_POOL_SIZES; s++) {
        TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, pool_sizes[s], NULL, NULL));
        for (r = 0; r < 5; r++) {
            for (g = 0; g < 5; g++) {
                step = grains[g] > 0 ? grains[g] : 1u;

                for (k = 0; k < TEST_ROWS_MAX; k++) {
                    hdrg_atomic_u32_init(&row_record.chunk_runs[k], 0);
                    hdrg_atomic_u32_init(&row_record.row_runs[k], 0);
                    row_record.begins[k] = UINT32_MAX;
                    row_record.ends[k] = UINT32_MAX;
                }

                hdrg_atomic_u32_init(&row_record.bad, 0);
                row_record.rows = row_counts[r];
                row_record.step = step;
                row_record.concurrency = hdrg_pool_concurrency(pool);

                hdrg_pool_parallel_rows(pool, row_counts[r], grains[g], UINT32_MAX, record_rows,
                                        &row_record);

                chunks = hdrg_pool_row_chunks(row_counts[r], grains[g]);
                TEST_ASSERT_EQUAL_UINT32((row_counts[r] + step - 1) / step, chunks);
                TEST_ASSERT_EQUAL_UINT32(0, hdrg_atomic_u32_load_acquire(&row_record.bad));

                for (k = 0; k < chunks; k++) {
                    hdrg_pool_row_chunk(row_counts[r], grains[g], k, &begin, &end);
                    TEST_ASSERT_EQUAL_UINT32(k * step, begin);
                    TEST_ASSERT_EQUAL_UINT32(
                        (begin + step < row_counts[r]) ? begin + step : row_counts[r], end);
                    TEST_ASSERT_EQUAL_UINT32(
                        1, hdrg_atomic_u32_load_acquire(&row_record.chunk_runs[k]));
                    TEST_ASSERT_EQUAL_UINT32(begin, row_record.begins[k]);
                    TEST_ASSERT_EQUAL_UINT32(end, row_record.ends[k]);
                }

                TEST_ASSERT_EQUAL_UINT32(
                    0, hdrg_atomic_u32_load_acquire(&row_record.chunk_runs[chunks]));
                for (k = 0; k < row_counts[r]; k++) {
                    TEST_ASSERT_EQUAL_UINT32(1,
                                             hdrg_atomic_u32_load_acquire(&row_record.row_runs[k]));
                }
            }
        }

        hdrg_pool_destroy(pool);
    }
}

static void test_create_destroy_repeated(void)
{
    static const uint32_t sizes[5] = {0, 1, 2, 3, 8};
    hdrg_pool_t *pool;
    uint32_t cycle, s, i;

    HDRG_TEST_REQUIRE_THREADS();

    hdrg_atomic_u32_init(&never_calls, 0);

    for (cycle = 0; cycle < 40; cycle++) {
        for (s = 0; s < 5; s++) {
            for (i = 0; i < TEST_INIT_SLOTS; i++) {
                hdrg_atomic_u32_init(&inits.calls[i], 0);
            }

            TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, NULL, sizes[s], count_init, &inits));
            TEST_ASSERT_EQUAL_UINT32(sizes[s], hdrg_pool_size(pool));
            TEST_ASSERT_EQUAL_UINT32(sizes[s] + 1, hdrg_pool_concurrency(pool));

            /* create から戻った時点で、各プールスレッドの初期化フックが 1 回ずつ済んでいる */
            for (i = 0; i < TEST_INIT_SLOTS; i++) {
                TEST_ASSERT_EQUAL_UINT32((i >= 1 && i <= sizes[s]) ? 1u : 0u,
                                         hdrg_atomic_u32_load_acquire(&inits.calls[i]));
            }

            hdrg_pool_parallel_for(pool, 0, UINT32_MAX, never_task, NULL);
            hdrg_pool_parallel_rows(pool, 0, 16, UINT32_MAX, record_rows, &row_record);

            if (cycle % 8 == 0) {
                job_reset(&jobs[0], hdrg_pool_concurrency(pool));
                hdrg_pool_parallel_for(pool, TEST_TASKS, UINT32_MAX, record_task, &jobs[0]);
                TEST_ASSERT_EQUAL_UINT32(0, job_errors(&jobs[0], TEST_TASKS));
            }

            hdrg_pool_destroy(pool);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(0, hdrg_atomic_u32_load_acquire(&never_calls));
}

static void test_invalid_arguments(void)
{
    hdrg_allocator_t alloc;
    hdrg_pool_t *pool;

    TEST_ASSERT_EQUAL(HDRG_ERR_INVALID_ARG, hdrg_pool_create(NULL, NULL, 1, NULL, NULL));

    pool = (hdrg_pool_t *)&alloc;
    TEST_ASSERT_EQUAL(HDRG_ERR_INVALID_ARG,
                      hdrg_pool_create(&pool, NULL, HDRG_POOL_MAX_WORKERS + 1, NULL, NULL));
    TEST_ASSERT_NULL(pool);

    alloc.malloc_fn = NULL;

    alloc.calloc_fn = NULL;
    alloc.realloc_fn = NULL;
    alloc.free_fn = counting_free;
    alloc.user = &alloc_count;
    TEST_ASSERT_EQUAL(HDRG_ERR_INVALID_ARG, hdrg_pool_create(&pool, &alloc, 1, NULL, NULL));

    TEST_ASSERT_EQUAL_UINT32(0, hdrg_pool_size(NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hdrg_pool_concurrency(NULL));
    hdrg_pool_destroy(NULL);
}

/* 定常状態ではメモリ確保をしない (確保は create の 1 回だけ) */
static void test_no_allocation_after_create(void)
{
    hdrg_allocator_t alloc;
    hdrg_pool_t *pool;
    uint32_t round;

    HDRG_TEST_REQUIRE_THREADS();

    hdrg_atomic_u32_init(&alloc_count.mallocs, 0);
    hdrg_atomic_u32_init(&alloc_count.frees, 0);
    alloc.malloc_fn = counting_malloc;
    alloc.calloc_fn = NULL;
    alloc.realloc_fn = counting_realloc;
    alloc.free_fn = counting_free;
    alloc.user = &alloc_count;

    TEST_ASSERT_EQUAL(HDRG_OK, hdrg_pool_create(&pool, &alloc, 4, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hdrg_atomic_u32_load_acquire(&alloc_count.mallocs));

    for (round = 0; round < 200; round++) {
        job_reset(&jobs[0], hdrg_pool_concurrency(pool));
        hdrg_pool_parallel_for(pool, TEST_TASKS, UINT32_MAX, record_task, &jobs[0]);
        TEST_ASSERT_EQUAL_UINT32(0, job_errors(&jobs[0], TEST_TASKS));
    }

    TEST_ASSERT_EQUAL_UINT32(1, hdrg_atomic_u32_load_acquire(&alloc_count.mallocs));
    TEST_ASSERT_EQUAL_UINT32(0, hdrg_atomic_u32_load_acquire(&alloc_count.frees));

    hdrg_pool_destroy(pool);
    TEST_ASSERT_EQUAL_UINT32(1, hdrg_atomic_u32_load_acquire(&alloc_count.frees));
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    job_reset(&reference_job, 1);
    hdrg_pool_parallel_for(NULL, TEST_TASKS, UINT32_MAX, record_task, &reference_job);

    UNITY_BEGIN();
    RUN_TEST(test_each_task_once_identical_outputs);
    RUN_TEST(test_inline_runs_in_task_order);
    RUN_TEST(test_nested_runs_inline);
    RUN_TEST(test_concurrent_callers);
    RUN_TEST(test_fair_between_callers);
    RUN_TEST(test_max_workers_limit);
    RUN_TEST(test_rows_boundaries_independent);
    RUN_TEST(test_create_destroy_repeated);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_no_allocation_after_create);

    return UNITY_END();
}
