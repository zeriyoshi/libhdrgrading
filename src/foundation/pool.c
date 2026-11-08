/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/foundation/pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hdrgrading/foundation/memory.h"
#include "hdrgrading/foundation/thread.h"

/* Every participant overshoots the claim counter at most once per call. */
#define HDRG_POOL_MAX_TASKS (UINT32_MAX - (2u * (HDRG_POOL_MAX_WORKERS + 1u)))

typedef struct hdrg_pool_job hdrg_pool_job_t;

/*
 * One parallel_for call. It lives on the caller's stack; the caller returns only after
 * unlinking it and seeing workers_in == 0, so no pool thread touches it afterwards.
 */
struct hdrg_pool_job {
    hdrg_pool_task_fn_t fn;
    void *arg;
    hdrg_pool_job_t *prev, *next;
    hdrg_atomic_u32_t next_task;
    uint32_t task_count;
    uint32_t max_workers;
    uint32_t workers_in; /* pool threads inside the job (mutex) */
    bool waiting;        /* the caller sleeps on pool->done (mutex) */
};

typedef struct {
    hdrg_thread_t thread;
    hdrg_pool_t *pool;
    uint32_t index;
} hdrg_pool_worker_t;

typedef struct {
    hdrg_pool_rows_fn_t fn;
    void *arg;
    uint32_t rows, grain;
} hdrg_pool_rows_job_t;

/* What the current thread is running, for nesting detection. */
typedef struct {
    const hdrg_pool_t *pool;
    uint32_t worker_index;
    bool active;
} hdrg_pool_scope_t;

struct hdrg_pool {
    hdrg_allocator_t alloc;
    hdrg_mutex_t mutex;
    hdrg_cond_t wake; /* idle pool threads */
    hdrg_cond_t done; /* create (startup) and callers waiting for their job */
    hdrg_pool_worker_init_fn_t init;
    void *init_user;
    hdrg_pool_job_t *head, *tail, *cursor; /* active jobs and the round-robin position (mutex) */
    hdrg_atomic_u32_t job_count;           /* written under the mutex, read relaxed for rotation */
    uint32_t worker_count;
    uint32_t started;  /* mutex */
    uint32_t sleeping; /* mutex */
    bool stopping;     /* mutex */
    hdrg_pool_worker_t workers[];
};

static HDRG_THREAD_LOCAL hdrg_pool_scope_t hdrg_pool_scope;

/*
 * Claims tasks until the job runs out. A pool thread (rotate) also leaves after each
 * task while other calls are active and picks again, which serves the calls
 * round-robin; the calling thread stays on its own job.
 */
static inline void hdrg_pool_run(hdrg_pool_t *pool, hdrg_pool_job_t *job, uint32_t worker_index,
                                 bool rotate)
{
    hdrg_pool_scope_t saved;
    uint32_t task;

    saved = hdrg_pool_scope;
    hdrg_pool_scope.pool = pool;
    hdrg_pool_scope.worker_index = worker_index;
    hdrg_pool_scope.active = true;

    for (;;) {
        task = hdrg_atomic_u32_fetch_add_relaxed(&job->next_task, 1);
        if (task >= job->task_count) {
            break;
        }

        job->fn(task, worker_index, job->arg);

        if (rotate && hdrg_atomic_u32_load_relaxed(&pool->job_count) > 1) {
            break;
        }
    }

    hdrg_pool_scope = saved;
}

/* A nested call keeps the thread's worker_index on the same pool (still unique per thread). */
static inline void hdrg_pool_run_inline(hdrg_pool_t *pool, uint32_t task_count,
                                        hdrg_pool_task_fn_t fn, void *arg)
{
    hdrg_pool_scope_t saved;
    uint32_t worker_index, task;

    saved = hdrg_pool_scope;
    worker_index = (saved.active && saved.pool == pool) ? saved.worker_index : 0;
    hdrg_pool_scope.pool = pool;
    hdrg_pool_scope.worker_index = worker_index;
    hdrg_pool_scope.active = true;

    for (task = 0; task < task_count; task++) {
        fn(task, worker_index, arg);
    }

    hdrg_pool_scope = saved;
}

static void *hdrg_pool_default_malloc(size_t size, void *user)
{
    (void)user;

    return HDRG_MALLOC(size);
}

static void hdrg_pool_default_free(void *ptr, void *user)
{
    (void)user;
    HDRG_FREE(ptr);
}

/* Next job after the cursor that has unclaimed tasks and room for one more thread. */
static hdrg_pool_job_t *hdrg_pool_pick(hdrg_pool_t *pool)
{
    hdrg_pool_job_t *job;
    uint32_t count, i;

    count = hdrg_atomic_u32_load_relaxed(&pool->job_count);
    job = pool->cursor;
    for (i = 0; i < count; i++) {
        if (job == NULL) {
            job = pool->head;
        }

        if (hdrg_atomic_u32_load_relaxed(&job->next_task) < job->task_count &&
            job->workers_in + 1 < job->max_workers) {
            pool->cursor = job->next;

            return job;
        }

        job = job->next;
    }

    return NULL;
}

static void hdrg_pool_link(hdrg_pool_t *pool, hdrg_pool_job_t *job)
{
    job->prev = pool->tail;
    job->next = NULL;

    if (pool->tail != NULL) {
        pool->tail->next = job;
    } else {
        pool->head = job;
    }

    pool->tail = job;
    hdrg_atomic_u32_store_relaxed(&pool->job_count,
                                  hdrg_atomic_u32_load_relaxed(&pool->job_count) + 1);
}

static void hdrg_pool_unlink(hdrg_pool_t *pool, hdrg_pool_job_t *job)
{
    if (job->prev != NULL) {
        job->prev->next = job->next;
    } else {
        pool->head = job->next;
    }

    if (job->next != NULL) {
        job->next->prev = job->prev;
    } else {
        pool->tail = job->prev;
    }

    if (pool->cursor == job) {
        pool->cursor = job->next;
    }

    hdrg_atomic_u32_store_relaxed(&pool->job_count,
                                  hdrg_atomic_u32_load_relaxed(&pool->job_count) - 1);
}

static void hdrg_pool_worker_main(void *arg)
{
    hdrg_pool_worker_t *worker = (hdrg_pool_worker_t *)arg;
    hdrg_pool_t *pool = worker->pool;
    hdrg_pool_job_t *job;

    hdrg_thread_set_worker_qos();

    /* Per-thread setup hook: FP environment pinning (fpenv.h) and future thread setup. */
    if (pool->init != NULL) {
        pool->init(worker->index, pool->init_user);
    }

    hdrg_mutex_lock(&pool->mutex);
    pool->started++;
    hdrg_cond_broadcast(&pool->done);

    while (!pool->stopping) {
        job = hdrg_pool_pick(pool);
        if (job == NULL) {
            pool->sleeping++;
            hdrg_cond_wait(&pool->wake, &pool->mutex);
            pool->sleeping--;

            continue;
        }

        job->workers_in++;
        hdrg_mutex_unlock(&pool->mutex);

        hdrg_pool_run(pool, job, worker->index, true);

        hdrg_mutex_lock(&pool->mutex);
        job->workers_in--;
        if (job->workers_in == 0 && job->waiting) {
            hdrg_cond_broadcast(&pool->done);
        }
    }

    hdrg_mutex_unlock(&pool->mutex);
}

static void hdrg_pool_rows_task(uint32_t task_index, uint32_t worker_index, void *arg)
{
    hdrg_pool_rows_job_t *job = (hdrg_pool_rows_job_t *)arg;
    uint32_t begin, end;

    hdrg_pool_row_chunk(job->rows, job->grain, task_index, &begin, &end);
    job->fn(begin, end, worker_index, job->arg);
}

static bool hdrg_pool_init_sync(hdrg_pool_t *pool)
{
    if (!hdrg_mutex_init(&pool->mutex)) {
        return false;
    }

    if (!hdrg_cond_init(&pool->wake)) {
        hdrg_mutex_destroy(&pool->mutex);

        return false;
    }

    if (!hdrg_cond_init(&pool->done)) {
        hdrg_cond_destroy(&pool->wake);
        hdrg_mutex_destroy(&pool->mutex);

        return false;
    }

    return true;
}

/* Stops and joins the first `threads` pool threads, then releases the pool. */
static void hdrg_pool_release(hdrg_pool_t *pool, uint32_t threads)
{
    uint32_t i;

    hdrg_mutex_lock(&pool->mutex);
    pool->stopping = true;
    hdrg_cond_broadcast(&pool->wake);
    hdrg_mutex_unlock(&pool->mutex);

    for (i = 0; i < threads; i++) {
        hdrg_thread_join(&pool->workers[i].thread);
    }

    hdrg_cond_destroy(&pool->done);
    hdrg_cond_destroy(&pool->wake);
    hdrg_mutex_destroy(&pool->mutex);
    pool->alloc.free_fn(pool, pool->alloc.user);
}

extern hdrg_status_t hdrg_pool_create(hdrg_pool_t **pool, const hdrg_allocator_t *alloc,
                                      uint32_t worker_count, hdrg_pool_worker_init_fn_t init,
                                      void *init_user)
{
    hdrg_allocator_t effective;
    hdrg_pool_t *created;
    uint32_t i;
    size_t size;

    if (pool == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *pool = NULL;

    if (worker_count > HDRG_POOL_MAX_WORKERS ||
        (alloc != NULL && (alloc->malloc_fn == NULL || alloc->free_fn == NULL))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (alloc != NULL) {
        effective = *alloc;
    } else {
        effective.malloc_fn = hdrg_pool_default_malloc;
        effective.calloc_fn = NULL;
        effective.realloc_fn = NULL;
        effective.free_fn = hdrg_pool_default_free;
        effective.user = NULL;
    }

    size = offsetof(hdrg_pool_t, workers) + (sizeof(hdrg_pool_worker_t) * worker_count);
    created = (hdrg_pool_t *)effective.malloc_fn(size, effective.user);
    if (created == NULL) {
        return HDRG_ERR_NOMEM;
    }

    memset(created, 0, size);
    created->alloc = effective;
    created->init = init;
    created->init_user = init_user;
    created->worker_count = worker_count;
    hdrg_atomic_u32_init(&created->job_count, 0);

    if (!hdrg_pool_init_sync(created)) {
        effective.free_fn(created, effective.user);

        return HDRG_ERR_NOMEM;
    }

    for (i = 0; i < worker_count; i++) {
        created->workers[i].pool = created;
        created->workers[i].index = i + 1;
        if (!hdrg_thread_create(&created->workers[i].thread, hdrg_pool_worker_main,
                                &created->workers[i])) {
            break;
        }
    }

    if (i < worker_count) {
        hdrg_pool_release(created, i);

        return HDRG_ERR_NOMEM;
    }

    hdrg_mutex_lock(&created->mutex);
    while (created->started < worker_count) {
        hdrg_cond_wait(&created->done, &created->mutex);
    }

    hdrg_mutex_unlock(&created->mutex);

    *pool = created;

    return HDRG_OK;
}

extern void hdrg_pool_destroy(hdrg_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }

    hdrg_pool_release(pool, pool->worker_count);
}

extern uint32_t hdrg_pool_size(const hdrg_pool_t *pool)
{
    return pool != NULL ? pool->worker_count : 0;
}

extern uint32_t hdrg_pool_concurrency(const hdrg_pool_t *pool)
{
    return hdrg_pool_size(pool) + 1;
}

extern void hdrg_pool_parallel_for(hdrg_pool_t *pool, uint32_t task_count, uint32_t max_workers,
                                   hdrg_pool_task_fn_t fn, void *arg)
{
    hdrg_pool_job_t job;
    uint32_t wake, i;

    if (pool == NULL || pool->worker_count == 0 || max_workers <= 1 || task_count <= 1 ||
        task_count > HDRG_POOL_MAX_TASKS || hdrg_pool_scope.active) {
        hdrg_pool_run_inline(pool, task_count, fn, arg);

        return;
    }

    job.fn = fn;
    job.arg = arg;
    job.prev = NULL;
    job.next = NULL;
    job.task_count = task_count;
    job.max_workers = max_workers < pool->worker_count + 1 ? max_workers : pool->worker_count + 1;
    job.workers_in = 0;
    job.waiting = false;
    hdrg_atomic_u32_init(&job.next_task, 0);
    wake = (task_count < job.max_workers ? task_count : job.max_workers) - 1;

    hdrg_mutex_lock(&pool->mutex);
    hdrg_pool_link(pool, &job);

    if (wake > pool->sleeping) {
        wake = pool->sleeping;
    }

    hdrg_mutex_unlock(&pool->mutex);

    /* `sleeping` waiters stay blocked until signalled (a spurious wake just picks again). */
    for (i = 0; i < wake; i++) {
        hdrg_cond_signal(&pool->wake);
    }

    hdrg_pool_run(pool, &job, 0, false);

    hdrg_mutex_lock(&pool->mutex);
    hdrg_pool_unlink(pool, &job);
    job.waiting = true;
    while (job.workers_in > 0) {
        hdrg_cond_wait(&pool->done, &pool->mutex);
    }

    hdrg_mutex_unlock(&pool->mutex);
}

extern void hdrg_pool_parallel_rows(hdrg_pool_t *pool, uint32_t rows, uint32_t grain,
                                    uint32_t max_workers, hdrg_pool_rows_fn_t fn, void *arg)
{
    hdrg_pool_rows_job_t job;

    job.fn = fn;
    job.arg = arg;
    job.rows = rows;
    job.grain = grain;

    hdrg_pool_parallel_for(pool, hdrg_pool_row_chunks(rows, grain), max_workers,
                           hdrg_pool_rows_task, &job);
}
