/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "exec.h"

#include <stddef.h>

typedef struct {
    hdrg_row_fn_t fn;
    void *arg;
} hdrg_exec_rows_job_t;

static void hdrg_exec_rows_task(uint32_t row_begin, uint32_t row_end, uint32_t worker_index,
                                void *arg)
{
    const hdrg_exec_rows_job_t *job = (const hdrg_exec_rows_job_t *)arg;

    (void)worker_index;
    job->fn(row_begin, row_end, job->arg);
}

extern void hdrg_exec_worker_rows(const hdrg_exec_t *exec, uint32_t rows, hdrg_worker_row_fn_t fn,
                                  void *arg)
{
    hdrg_pool_parallel_rows((exec != NULL) ? exec->pool : NULL, rows, HDRG_EXEC_GRAIN_ROWS,
                            (exec != NULL) ? exec->max_workers : 1u, fn, arg);
}

extern uint32_t hdrg_exec_concurrency(const hdrg_exec_t *exec)
{
    return hdrg_pool_concurrency((exec != NULL) ? exec->pool : NULL);
}

extern void hdrg_exec_rows(const hdrg_exec_t *exec, uint32_t rows, hdrg_row_fn_t fn, void *arg)
{
    hdrg_exec_rows_job_t job;

    job.fn = fn;
    job.arg = arg;

    hdrg_pool_parallel_rows((exec != NULL) ? exec->pool : NULL, rows, HDRG_EXEC_GRAIN_ROWS,
                            (exec != NULL) ? exec->max_workers : 1u, hdrg_exec_rows_task, &job);
}
