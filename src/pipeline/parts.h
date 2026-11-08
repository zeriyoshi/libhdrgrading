/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PIPELINE_PARTS_H
#define HDRG_PIPELINE_PARTS_H

#include <stdio.h>
#include <string.h>

#include "../core/gpu_analyze.h"
#include "../host/defaults.h"
#include "../pipeline.h"
#include "hdrgrading/foundation/math.h"

/*
 * The pipeline (src/pipeline.h) inside: the parts its files share. pipeline.c: open, the
 * analysis, the plan and the apply pass's bands; chain.c: the PREFILTER chain (its stages, the
 * frames it holds, their motion and grain, the chain on the device); scale.c: the SCALE stage
 */

#define HDRG_MIN_RATIO 0.05f /* smallest declared gain-map headroom, stops */

/* the reduction of a frame for its analysis */
typedef struct {
    const hdrg_source_t *src;
    uint16_t *dst;
    uint32_t width; /* the proxy's */
    uint8_t shift;
} hdrg_proxy_job_t;

/* a frame's RGB16 rows copied into tight rows */
typedef struct {
    const uint8_t *from;
    uint16_t *to;
    size_t stride; /* from's */
    size_t row;    /* the bytes of a row */
} hdrg_copy_job_t;

/* an ASYNC filter keeping frames in flight (a session's) */
static inline bool hdrg_pipeline_flying(const hdrg_stage_t *stage)
{
    return (stage->cls->flags & HDRG_FILTER_ASYNC) != 0 && stage->max_inflight > 1u;
}

/* two neighboring working filters keep the frame between them on their device */
static inline bool hdrg_pipeline_linked(const hdrg_stage_t *from, const hdrg_stage_t *to)
{
    return from->device != 0 && from->device == to->device && from->past + from->future == 0 &&
           to->past + to->future == 0 && !hdrg_pipeline_flying(from) && !hdrg_pipeline_flying(to);
}

/* stage i of a session's chain keeps frames: for its time window or in flight */
static inline bool hdrg_pipeline_keeps(const hdrg_pipeline_t *pipeline, uint8_t i)
{
    return pipeline->windowed && (pipeline->stages[i].past + pipeline->stages[i].future > 0 ||
                                  hdrg_pipeline_flying(&pipeline->stages[i]));
}

/* ... and holds some */
static inline bool hdrg_pipeline_holding(const hdrg_pipeline_t *pipeline, uint8_t i)
{
    return hdrg_pipeline_keeps(pipeline, i) &&
           (pipeline->stages[i].pushed > pipeline->stages[i].taken ||
            pipeline->stages[i].flight_count > 0);
}

/*
 * RESYNTH: the chain's first stage keeping a time window, the grain's
 * before at its input (prefilter_count: none, the chain's input; UINT8_MAX:
 * no grain)
 */
static inline uint8_t hdrg_pipeline_grain_stage(const hdrg_pipeline_t *pipeline)
{
    uint8_t i;

    if (pipeline->params.grain_policy != HDRG_GRAIN_RESYNTH) {
        return UINT8_MAX;
    }

    for (i = 0; i < pipeline->prefilter_count &&
                !(hdrg_pipeline_keeps(pipeline, i) &&
                  pipeline->stages[i].past + pipeline->stages[i].future > 0);
         i++) {
    }

    return i;
}

extern void hdrg_pipeline_copy_rows(uint32_t row_begin, uint32_t row_end, void *arg);

/* the output size of encode for a width x height source; a zero side keeps the aspect ratio */
extern hdrg_status_t hdrg_pipeline_output_size(const hdrg_encode_params_t *encode, uint32_t width,
                                               uint32_t height, uint32_t *out_width,
                                               uint32_t *out_height);

/*
 * SCALE: the analysis and the plan stay at the source size; the apply pass
 * reads the scaled code values band by band. A size change without a filter
 * on the stage uses a built-in scaler: params' upscaler for an output larger
 * on both sides, else Lanczos (under the context's HDRG_GPU_AUTO / FILTERS
 * its Metal, Vulkan or WebGPU version, the CPU's without a device). The
 * filter instance lives as long as the pipeline and is configured per frame
 * (a filter without BANDS processes the whole frame then).
 */
extern hdrg_status_t hdrg_pipeline_scale_setup(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline);

/* a session's delays, its flights at the cap; the chain and SCALE keep frames for them */
extern void hdrg_pipeline_delays(hdrg_pipeline_t *pipeline);

/*
 * A session's SCALE keeping frames: its next frame taken and configured
 * (or landed, in flight); use: for the apply pass (else only dropped)
 */
extern hdrg_status_t hdrg_pipeline_scale_next(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                              bool use);

/*
 * PREFILTER: src becomes the stage's RGB16 output and input the caller's
 * frames. A still's frame (raw's pixels) is probed first: when every filter
 * of the chain passes it through, there is no stage and the core reads the
 * caller's pixels as without one.
 */
extern hdrg_status_t hdrg_pipeline_prefilter_setup(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                                   const uint8_t *pixels);

#endif /* HDRG_PIPELINE_PARTS_H */
