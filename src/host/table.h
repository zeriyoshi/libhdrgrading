/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_TABLE_H
#define HDRG_HOST_TABLE_H

#include "host.h"

#include <float.h>
#include <string.h>

#include "../core/motion.h"
#include "../priv.h"
#include "analyzer.h"
#include "stats.h"
#include "codec_host.h"
#include "validate.h"

#include "hdrgrading/foundation/gpu_share.h"
#if defined(HDRG_WITH_METAL)
#include "hdrgrading/gpu/metal.h"
#endif
#if defined(HDRG_WITH_VULKAN)
#include "hdrgrading/gpu/vulkan.h"
#endif
#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/webgpu.h"
#endif

/*
 * The host (host.h) inside: the API table's functions in its files and what they share. host.c:
 * the registration, the classes, the options, the table and the registry; job.c: a filter's
 * instance, negotiation and jobs; device.c: the context's GPU devices and their share
 */

#define HDRG_PLUGIN_MAGIC UINT32_C(0x48504C47)
#define HDRG_CLASS_MAGIC  UINT32_C(0x48434C53)
#define HDRG_STAGES_ALL   UINT32_C(0x2F)
#define HDRG_FLAGS_ALL    UINT32_C(0x7F)
#define HDRG_RESERVED_ID  "hdrg."

/* what a text may contain */
#define HDRG_KIND_LINE    0u /* UTF-8 without controls */
#define HDRG_KIND_LINES   1u /* ... and line feeds */
#define HDRG_KIND_BLOCK   2u /* ... line feeds and tabs */
#define HDRG_KIND_ID      3u
#define HDRG_KIND_VERSION 4u
#define HDRG_KIND_KEY     5u
#define HDRG_KIND_SPDX    6u
#define HDRG_KIND_REF     7u

/* the frames a window or a clip of the filter's stage may span each way (INTERP: before t) */
static inline uint32_t hdrg_window_limit(const hdrg_filter_t *filter)
{
    return (filter->stage == HDRG_STAGE_SCALE)
               ? HDRG_LIMIT_WINDOW_MAX
               : ((filter->stage == HDRG_STAGE_INTERP) ? HDRG_LIMIT_INTERP : HDRG_LIMIT_WINDOW);
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_input(const hdrg_negotiation_t *nego,
                                                          uint32_t *width, uint32_t *height,
                                                          uint32_t *format);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_requested_size(const hdrg_negotiation_t *nego,
                                                                   uint32_t *width,
                                                                   uint32_t *height);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_output(hdrg_negotiation_t *nego,
                                                               uint32_t width, uint32_t height,
                                                               uint32_t format);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_side_data(const hdrg_negotiation_t *nego,
                                                              uint32_t type, uint32_t *version,
                                                              void *buf, size_t cap,
                                                              size_t *needed);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_input_color(const hdrg_negotiation_t *nego,
                                                                double *xy, uint32_t *transfer,
                                                                uint32_t *bits);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_halo(hdrg_negotiation_t *nego,
                                                             uint32_t rows);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_passthrough(hdrg_negotiation_t *nego);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_rows(const hdrg_job_t *job, uint32_t *begin,
                                                        uint32_t *end);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input(const hdrg_job_t *job, uint32_t plane,
                                                         const uint8_t **data, size_t *stride,
                                                         uint32_t *row_begin, uint32_t *row_end);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_scratch(const hdrg_job_t *job, size_t size,
                                                           void **data);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_output(const hdrg_job_t *job, uint32_t plane,
                                                          uint8_t **data, size_t *stride);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_done(hdrg_job_t *job, hdrg_status_t status);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_max_inflight(hdrg_filter_t *filter,
                                                                       uint32_t frames);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_window(hdrg_filter_t *filter,
                                                                 uint32_t past, uint32_t future);

/* the objects around the app's devices, after the plugins that shared them */
extern void hdrg_host_wrapped_destroy(hdrg_plugin_host_t *host);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_device(const hdrg_filter_t *filter,
                                                             uint32_t kind, void **device,
                                                             void **queue);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_gpu_share(const hdrg_filter_t *filter,
                                                                uint32_t kind, void *out);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_device(hdrg_filter_t *filter,
                                                                 uint32_t kind);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_input(hdrg_filter_t *filter,
                                                                uint32_t format, uint32_t matrix,
                                                                uint8_t full_range);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_clip(hdrg_filter_t *filter,
                                                               uint32_t frames, uint32_t overlap);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_window(const hdrg_job_t *job, uint32_t *index,
                                                          uint32_t *past, uint32_t *future);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input_frame(const hdrg_job_t *job,
                                                               int32_t offset, uint32_t plane,
                                                               const uint8_t **data, size_t *stride,
                                                               uint32_t *row_begin,
                                                               uint32_t *row_end);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_device(const hdrg_job_t *job, void **in,
                                                          uint64_t *ready, uint8_t *out_wanted);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_device_window(const hdrg_job_t *job,
                                                                 int32_t offset, void **frame);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_set_side_data(hdrg_job_t *job, uint32_t type,
                                                                 uint32_t version, const void *data,
                                                                 size_t size);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_set_output_device(hdrg_job_t *job, void *frame,
                                                                     uint64_t ready);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_frame(const hdrg_job_t *job, int64_t *pts,
                                                         uint32_t *flags);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_phase(const hdrg_job_t *job, uint32_t *phase);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_phases(const hdrg_job_t *job, uint32_t *phases,
                                                          uint32_t cap, uint32_t *count);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_shot(const hdrg_job_t *job, uint32_t *past,
                                                        uint32_t *future);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input_handle(const hdrg_job_t *job,
                                                                int32_t offset, void **handle);

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_output_handle(const hdrg_job_t *job,
                                                                 void **handle);

#endif /* HDRG_HOST_TABLE_H */
