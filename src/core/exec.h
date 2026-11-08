/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_EXEC_H
#define HDRG_CORE_EXEC_H

#include <stdint.h>

#include "hdrgrading/foundation/pool.h"

/* fixed row grain: the split of a row range never depends on the worker count */
#define HDRG_EXEC_GRAIN_ROWS 16u

/*
 * Where a stage may run its row loops: the context's pool and how many
 * workers (including the calling thread) one call may use. NULL, a NULL
 * pool or max_workers <= 1 runs inline on the calling thread.
 */
typedef struct {
    hdrg_pool_t *pool;
    uint32_t max_workers;
} hdrg_exec_t;

typedef void (*hdrg_row_fn_t)(uint32_t row_begin, uint32_t row_end, void *arg);
/* worker < hdrg_exec_concurrency(): selects per-worker scratch, never the result */
typedef void (*hdrg_worker_row_fn_t)(uint32_t row_begin, uint32_t row_end, uint32_t worker,
                                     void *arg);

/*
 * Runs fn over [0, rows) in chunks of HDRG_EXEC_GRAIN_ROWS rows. Chunks write
 * disjoint outputs, so the result does not depend on the worker count.
 */
extern void hdrg_exec_rows(const hdrg_exec_t *exec, uint32_t rows, hdrg_row_fn_t fn, void *arg);
extern void hdrg_exec_worker_rows(const hdrg_exec_t *exec, uint32_t rows, hdrg_worker_row_fn_t fn,
                                  void *arg);
/* number of per-worker scratch slots a hdrg_exec_worker_rows call may touch */
extern uint32_t hdrg_exec_concurrency(const hdrg_exec_t *exec);

#endif /* HDRG_CORE_EXEC_H */
