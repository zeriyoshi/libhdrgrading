/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "table.h"

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_input(const hdrg_negotiation_t *nego,
                                                          uint32_t *width, uint32_t *height,
                                                          uint32_t *format)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || width == NULL || height == NULL ||
        format == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *width = nego->in_width;
    *height = nego->in_height;
    *format = nego->in_format;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_requested_size(const hdrg_negotiation_t *nego,
                                                                   uint32_t *width,
                                                                   uint32_t *height)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || width == NULL || height == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *width = nego->req_width;
    *height = nego->req_height;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_output(hdrg_negotiation_t *nego,
                                                               uint32_t width, uint32_t height,
                                                               uint32_t format)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || width == 0 || height == 0 ||
        width > HDRG_MAX_DIMENSION || height > HDRG_MAX_DIMENSION || !hdrg_format_known(format)) {
        return HDRG_ERR_INVALID_ARG;
    }

    nego->out_width = width;
    nego->out_height = height;
    nego->out_format = format;
    nego->output_set = true;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_side_data(const hdrg_negotiation_t *nego,
                                                              uint32_t type, uint32_t *version,
                                                              void *buf, size_t cap, size_t *needed)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || version == NULL || needed == NULL ||
        (buf == NULL && cap != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (type == HDRG_SD_MOTION && nego->motion != NULL) {
        *version = HDRG_SD_MOTION_VERSION;
        *needed = nego->motion_size;

        if (cap >= *needed) {
            memcpy(buf, nego->motion, *needed);
        }

        return HDRG_OK;
    }

    if (type != HDRG_SD_NOISE || nego->noise == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    *version = HDRG_SD_NOISE_VERSION;
    *needed = HDRG_SD_NOISE_FLOATS * sizeof(float);

    if (cap >= *needed) {
        memcpy(buf, nego->noise, *needed);
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_input_color(const hdrg_negotiation_t *nego,
                                                                double *xy, uint32_t *transfer,
                                                                uint32_t *bits)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || xy == NULL || transfer == NULL ||
        bits == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (nego->xy == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    memcpy(xy, nego->xy, 8u * sizeof(double));
    *transfer = nego->transfer;
    *bits = nego->bits;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_halo(hdrg_negotiation_t *nego,
                                                             uint32_t rows)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC || rows > HDRG_MAX_DIMENSION) {
        return HDRG_ERR_INVALID_ARG;
    }

    nego->halo = rows;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_nego_set_passthrough(hdrg_negotiation_t *nego)
{
    if (nego == NULL || nego->magic != HDRG_NEGO_MAGIC) {
        return HDRG_ERR_INVALID_ARG;
    }

    nego->passthrough = true;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_rows(const hdrg_job_t *job, uint32_t *begin,
                                                        uint32_t *end)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || begin == NULL || end == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *begin = job->begin;
    *end = job->end;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input(const hdrg_job_t *job, uint32_t plane,
                                                         const uint8_t **data, size_t *stride,
                                                         uint32_t *row_begin, uint32_t *row_end)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || data == NULL || stride == NULL ||
        row_begin == NULL || row_end == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* on the device */
    if (job->in_device != NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (plane >= job->in_planes_n) {
        return HDRG_ERR_INVALID_ARG;
    }

    *data = job->in_planes[plane];
    *stride = job->in_strides[plane];
    *row_begin = job->in_begin;
    *row_end = job->in_end;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_output(const hdrg_job_t *job, uint32_t plane,
                                                          uint8_t **data, size_t *stride)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || data == NULL || stride == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* to the device */
    if (job->out_wanted != 0) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (plane >= job->out_planes_n) {
        return HDRG_ERR_INVALID_ARG;
    }

    *data = job->out_planes[plane];
    *stride = job->out_strides[plane];

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_scratch(const hdrg_job_t *job, size_t size,
                                                           void **data)
{
    hdrg_job_scratch_t *scratch;
    void *block;

    if (job == NULL || job->magic != HDRG_JOB_MAGIC || data == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *data = NULL;
    scratch = job->scratch;

    if (scratch == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (job->asked != NULL && size > *job->asked) {
        *job->asked = size;
    }

    /* grown, never shrunk: the worker's next jobs take it as it is */
    if (scratch->data == NULL || size > scratch->bytes) {
        block = hdrg_mem_aligned_alloc(scratch->alloc, (size > 0) ? size : 1u, 64u);
        if (block == NULL) {
            return HDRG_ERR_NOMEM;
        }

        hdrg_mem_aligned_free(scratch->alloc, scratch->data);
        scratch->data = block;
        scratch->bytes = size;
    }

    *data = scratch->data;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_done(hdrg_job_t *job, hdrg_status_t status)
{
    hdrg_job_end_t *end;

    if (job == NULL || job->magic != HDRG_JOB_MAGIC || job->end_of == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    end = job->end_of;

    hdrg_mutex_lock(&end->mutex);

    if (end->ended) {
        hdrg_mutex_unlock(&end->mutex);

        return HDRG_ERR_INVALID_STATE;
    }

    end->result = status;
    end->ended = true;
    hdrg_cond_broadcast(&end->cond);
    hdrg_mutex_unlock(&end->mutex);

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_max_inflight(hdrg_filter_t *filter,
                                                                       uint32_t frames)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || frames == 0 ||
        frames > HDRG_LIMIT_INFLIGHT) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!filter->creating) {
        return HDRG_ERR_INVALID_STATE;
    }

    filter->max_inflight = frames;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_window(hdrg_filter_t *filter,
                                                                 uint32_t past, uint32_t future)
{
    /* INTERP: t + 1 always, and up to the limit after it */
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || past > hdrg_window_limit(filter) ||
        (filter->stage == HDRG_STAGE_INTERP ? (future == 0 || future > HDRG_LIMIT_INTERP + 1u)
                                            : future > hdrg_window_limit(filter))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!filter->creating) {
        return HDRG_ERR_INVALID_STATE;
    }

    filter->past = past;
    filter->future = future;
    filter->clip = 0;
    filter->overlap = 0;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_device(hdrg_filter_t *filter,
                                                                 uint32_t kind)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || kind == 0 ||
        kind > HDRG_DEVICE_KINDS) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!filter->creating) {
        return HDRG_ERR_INVALID_STATE;
    }

    filter->device = kind;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_input(hdrg_filter_t *filter,
                                                                uint32_t format, uint32_t matrix,
                                                                uint8_t full_range)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || !hdrg_format_known(format) ||
        (matrix != 1u && matrix != 5u && matrix != 6u && matrix != 9u) || full_range > 1u) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!filter->creating) {
        return HDRG_ERR_INVALID_STATE;
    }

    filter->in_format = format;
    filter->yuv.matrix = (uint8_t)matrix;
    filter->yuv.full_range = full_range != 0;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_set_clip(hdrg_filter_t *filter,
                                                               uint32_t frames, uint32_t overlap)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || frames < 2u ||
        filter->stage == HDRG_STAGE_INTERP || overlap > hdrg_window_limit(filter) ||
        frames - 1u > hdrg_window_limit(filter) - overlap) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!filter->creating) {
        return HDRG_ERR_INVALID_STATE;
    }

    filter->past = frames - 1u + overlap;
    filter->future = frames - 1u + overlap;
    filter->clip = frames;
    filter->overlap = overlap;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_window(const hdrg_job_t *job, uint32_t *index,
                                                          uint32_t *past, uint32_t *future)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || index == NULL || past == NULL ||
        future == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *index = job->index;
    *past = job->past;
    *future = job->future;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input_frame(const hdrg_job_t *job,
                                                               int32_t offset, uint32_t plane,
                                                               const uint8_t **data, size_t *stride,
                                                               uint32_t *row_begin,
                                                               uint32_t *row_end)
{
    const hdrg_format_frame_t *frame;

    if (job == NULL || job->magic != HDRG_JOB_MAGIC || plane >= job->in_planes_n || data == NULL ||
        stride == NULL || row_begin == NULL || row_end == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (offset < -(int32_t)job->past || offset > (int32_t)job->future) {
        return HDRG_ERR_NOT_FOUND;
    }

    if (offset == 0) {
        return hdrg_api_job_input(job, plane, data, stride, row_begin, row_end);
    }

    frame = job->window[offset];
    *data = frame->planes[plane] +
            (size_t)hdrg_format_plane_row(job->in_format, (uint8_t)plane, job->in_begin) *
                frame->strides[plane];
    *stride = frame->strides[plane];
    *row_begin = job->in_begin;
    *row_end = job->in_end;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_device(const hdrg_job_t *job, void **in,
                                                          uint64_t *ready, uint8_t *out_wanted)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || in == NULL || ready == NULL ||
        out_wanted == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *in = job->in_device;
    *ready = job->in_ready;
    *out_wanted = job->out_wanted;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_device_window(const hdrg_job_t *job,
                                                                 int32_t offset, void **frame)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || frame == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (offset < -(int32_t)job->past || offset > (int32_t)job->future) {
        return HDRG_ERR_NOT_FOUND;
    }

    *frame = (job->device_window != NULL) ? job->device_window[offset] : NULL;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_set_side_data(hdrg_job_t *job, uint32_t type,
                                                                 uint32_t version, const void *data,
                                                                 size_t size)
{
    hdrg_job_side_t *side;
    hdrg_status_t status;
    uint8_t *grown;

    if (job == NULL || job->magic != HDRG_JOB_MAGIC || (data == NULL && size != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (type != HDRG_SD_MOTION || job->side == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (version != HDRG_SD_MOTION_VERSION || !hdrg_motion_check(data, size)) {
        return HDRG_ERR_INVALID_ARG;
    }

    side = job->side;
    status = HDRG_OK;

    hdrg_mutex_lock(&side->lock);

    if (size > side->cap) {
        grown = (uint8_t *)hdrg_mem_realloc(side->alloc, side->data, size);
        if (grown != NULL) {
            side->data = grown;
            side->cap = size;
        } else {
            status = HDRG_ERR_NOMEM;
        }
    }

    if (status == HDRG_OK) {
        memcpy(side->data, data, size);
        side->size = size;
    }

    hdrg_mutex_unlock(&side->lock);

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_set_output_device(hdrg_job_t *job, void *frame,
                                                                     uint64_t ready)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || frame == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (job->out_wanted == 0 || job->out_device != NULL) {
        return HDRG_ERR_INVALID_STATE;
    }

    job->out_device = frame;
    job->out_ready = ready;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_frame(const hdrg_job_t *job, int64_t *pts,
                                                         uint32_t *flags)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || pts == NULL || flags == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *pts = job->pts;
    *flags = job->flags;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_phase(const hdrg_job_t *job, uint32_t *phase)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || phase == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *phase = job->phase;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_phases(const hdrg_job_t *job, uint32_t *phases,
                                                          uint32_t cap, uint32_t *count)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || count == NULL ||
        (phases == NULL && cap > 0u)) {
        return HDRG_ERR_INVALID_ARG;
    }

    *count = job->phase_count;

    if (cap > 0u && job->phase_count > 0u) {
        memcpy(phases, job->phases, (size_t)HDRG_MIN(cap, job->phase_count) * sizeof(uint32_t));
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_shot(const hdrg_job_t *job, uint32_t *past,
                                                        uint32_t *future)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || past == NULL || future == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!job->shots) {
        return HDRG_ERR_NOT_FOUND;
    }

    *past = job->shot_past;
    *future = job->shot_future;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_input_handle(const hdrg_job_t *job,
                                                                int32_t offset, void **handle)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || handle == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (offset < -(int32_t)job->past || offset > (int32_t)job->future) {
        return HDRG_ERR_NOT_FOUND;
    }

    *handle = (offset == 0) ? job->in_handle
                            : ((job->window_handles != NULL) ? job->window_handles[offset] : NULL);

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_job_output_handle(const hdrg_job_t *job,
                                                                 void **handle)
{
    if (job == NULL || job->magic != HDRG_JOB_MAGIC || handle == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *handle = job->out_handle;

    return HDRG_OK;
}
