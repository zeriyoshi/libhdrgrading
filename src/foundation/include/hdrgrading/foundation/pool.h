/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_POOL_H
#define HDRG_FOUNDATION_POOL_H

#include <stdint.h>

#include "hdrgrading/hdrgrading.h"

/*
 * Persistent thread pool owned by a context. No global state.
 *
 * - parallel_for runs fn(task_index, ...) exactly once for every task_index in
 *   [0, task_count) and returns when all of them have finished. The calling thread
 *   takes part, so a pool of size 0 (WASM single-thread) needs no special casing.
 * - Determinism: the caller fixes task_count and the work of each task; threads only
 *   claim whole tasks, so per-task outputs never depend on the thread count or the
 *   claim order. worker_index is for selecting scratch memory only, and reductions
 *   must combine per-task partials in task_index order.
 * - worker_index is 0 on the calling thread and 1..size on pool threads (fixed per
 *   thread), so it is < hdrg_pool_concurrency() and unique among the threads that
 *   run tasks of one call at the same time. Concurrent calls each have their own
 *   index 0 (their caller), so scratch indexed by it belongs to the call / session,
 *   never to the pool. With max_workers set, the indices that occur can be sparse:
 *   size scratch arrays by hdrg_pool_concurrency().
 * - Inline (task order, on the calling thread): pool == NULL, a pool of size 0,
 *   max_workers <= 1, task_count <= 1, and any call made from inside a task (nesting,
 *   on any pool: never deadlocks). worker_index is 0, except that a call nested on the
 *   same pool keeps the current thread's index.
 * - Thread-safe: several threads may call parallel_for on one pool at once. Active
 *   calls are served round-robin per task, so a long call cannot starve another.
 * - No allocation after create. Job state lives on the caller's stack.
 */
#define HDRG_POOL_MAX_WORKERS 1024u

typedef struct hdrg_pool hdrg_pool_t;

typedef void (*hdrg_pool_task_fn_t)(uint32_t task_index, uint32_t worker_index, void *arg);
/* Rows [row_begin, row_end) of chunk row_begin / grain. */
typedef void (*hdrg_pool_rows_fn_t)(uint32_t row_begin, uint32_t row_end, uint32_t worker_index,
                                    void *arg);
/* Per-thread setup hook (FP environment pinning, ...): runs on each pool thread at start. */
typedef void (*hdrg_pool_worker_init_fn_t)(uint32_t worker_index, void *user);

/* Fixed row chunking: [k * grain, min((k + 1) * grain, rows)). grain 0 counts as 1. */
static inline uint32_t hdrg_pool_row_chunks(uint32_t rows, uint32_t grain)
{
    uint32_t step = grain > 0 ? grain : 1u;

    return (rows / step) + (rows % step != 0 ? 1u : 0u);
}

/* chunk must be < hdrg_pool_row_chunks(rows, grain). */
static inline void hdrg_pool_row_chunk(uint32_t rows, uint32_t grain, uint32_t chunk,
                                       uint32_t *row_begin, uint32_t *row_end)
{
    uint32_t step = grain > 0 ? grain : 1u, begin = chunk * step;

    *row_begin = begin;
    *row_end = (rows - begin > step) ? begin + step : rows;
}

/*
 * worker_count pool threads in addition to the callers (<= HDRG_POOL_MAX_WORKERS).
 * alloc == NULL uses malloc / free. init (optional) runs on every pool thread with its
 * worker_index before create returns; it must not call into the pool. Returns
 * HDRG_ERR_NOMEM when memory or a thread cannot be obtained.
 */
extern hdrg_status_t hdrg_pool_create(hdrg_pool_t **pool, const hdrg_allocator_t *alloc,
                                      uint32_t worker_count, hdrg_pool_worker_init_fn_t init,
                                      void *init_user);
/* No parallel_for may be in flight. NULL is a no-op. */
extern void hdrg_pool_destroy(hdrg_pool_t *pool);
/* Pool threads (0 for NULL). */
extern uint32_t hdrg_pool_size(const hdrg_pool_t *pool);
/* Upper bound of worker_index + 1: size + 1. */
extern uint32_t hdrg_pool_concurrency(const hdrg_pool_t *pool);
/* max_workers counts the calling thread; UINT32_MAX = no limit. */
extern void hdrg_pool_parallel_for(hdrg_pool_t *pool, uint32_t task_count, uint32_t max_workers,
                                   hdrg_pool_task_fn_t fn, void *arg);
/* parallel_for over hdrg_pool_row_chunks(rows, grain) chunks. */
extern void hdrg_pool_parallel_rows(hdrg_pool_t *pool, uint32_t rows, uint32_t grain,
                                    uint32_t max_workers, hdrg_pool_rows_fn_t fn, void *arg);

#endif /* HDRG_FOUNDATION_POOL_H */
