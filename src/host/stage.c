/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "stage.h"

#include <string.h>

#include "../priv.h"
#include "defaults.h"

/* one band of hdrg_stage_rows: its input (frame row in_first at in) and output (row y0 at out) */
typedef struct {
    hdrg_stage_t *scale;
    const hdrg_format_frame_t *in, *out;
    uint32_t y0, in_first, in_begin, in_end;
} hdrg_stage_band_t;

typedef struct {
    const hdrg_source_t *src;
    uint8_t *rows;
    size_t stride;
    uint32_t begin;
} hdrg_rgb16_job_t;

static void hdrg_rgb16_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_rgb16_job_t *job = (const hdrg_rgb16_job_t *)arg;
    uint32_t r;

    for (r = row_begin; r < row_end; r++) {
        hdrg_source_rgb16_row(job->src, job->begin + r,
                              (uint16_t *)(void *)(job->rows + (size_t)r * job->stride));
    }
}

/* buf holds at least bytes (what it held is not kept) */
static hdrg_status_t hdrg_buffer_fit(const hdrg_allocator_t *alloc, hdrg_stage_buffer_t *buf,
                                     size_t bytes)
{
    void *block;

    if (bytes <= buf->bytes) {
        return HDRG_OK;
    }

    if (bytes > SIZE_MAX - HDRG_FORMAT_PAGE) {
        return HDRG_ERR_NOMEM;
    }

    block = hdrg_mem_alloc(alloc, bytes + HDRG_FORMAT_PAGE);
    if (block == NULL) {
        return HDRG_ERR_NOMEM;
    }

    hdrg_mem_free(alloc, buf->block);

    buf->block = block;
    buf->data = (uint8_t *)block +
                (HDRG_FORMAT_PAGE - (uintptr_t)block % HDRG_FORMAT_PAGE) % HDRG_FORMAT_PAGE;
    buf->bytes = bytes;

    return HDRG_OK;
}

static void hdrg_buffer_free(const hdrg_allocator_t *alloc, hdrg_stage_buffer_t *buf)
{
    hdrg_mem_free(alloc, buf->block);
    memset(buf, 0, sizeof(*buf));
}

/* the workers' scratch (job_scratch): count slots at least, kept (false: no memory) */
static bool hdrg_stage_scratches(hdrg_stage_t *stage, uint32_t count)
{
    hdrg_job_scratch_t *grown;
    uint32_t i;

    if (count <= stage->scratch_count) {
        return true;
    }

    grown = (hdrg_job_scratch_t *)hdrg_mem_realloc(stage->alloc, stage->scratches,
                                                   (size_t)count * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }

    for (i = stage->scratch_count; i < count; i++) {
        grown[i].alloc = stage->alloc;
        grown[i].data = NULL;
        grown[i].bytes = 0;
    }

    stage->scratches = grown;
    stage->scratch_count = count;

    return true;
}

/* every worker's scratch grown to the most a band's job asked for (false: no memory) */
static bool hdrg_stage_scratch_grow(hdrg_stage_t *stage)
{
    hdrg_job_scratch_t *scratch;
    uint32_t i;
    void *block;

    for (i = 0; i < stage->scratch_count; i++) {
        scratch = &stage->scratches[i];

        if (stage->scratch_want > 0 && scratch->bytes < stage->scratch_want) {
            block = hdrg_mem_aligned_alloc(stage->alloc, stage->scratch_want, 64u);
            if (block == NULL) {
                return false;
            }

            hdrg_mem_aligned_free(stage->alloc, scratch->data);
            scratch->data = block;
            scratch->bytes = stage->scratch_want;
        }
    }

    return true;
}

/* a whole frame of layout: the filter's own (frame_alloc) or the host's, made once */
static hdrg_status_t hdrg_stage_whole(hdrg_stage_t *stage, hdrg_stage_whole_t *whole,
                                      const hdrg_format_layout_t *layout, hdrg_plugin_host_t *host)
{
    hdrg_status_t status;
    uint8_t p;

    if (whole->ready) {
        return HDRG_OK;
    }

    if (stage->cls->frame_alloc == NULL) {
        status = hdrg_buffer_fit(stage->alloc, &whole->buffer, layout->bytes);
        if (status != HDRG_OK) {
            return status;
        }

        hdrg_format_view(layout, whole->buffer.data, &whole->view);
        whole->ready = true;

        return HDRG_OK;
    }

    memset(&whole->view, 0, sizeof(whole->view));
    whole->handle = NULL;

    status = stage->cls->frame_alloc(stage->inst, layout->format, layout->width, layout->height,
                                     whole->view.planes, whole->view.strides, &whole->handle,
                                     hdrg_class_api(stage->cls));

    /* the host reads and writes the samples in place: whole rows, aligned to the samples */
    for (p = 0; status == HDRG_OK && p < layout->planes; p++) {
        if (whole->view.planes[p] == NULL ||
            whole->view.strides[p] < hdrg_format_row_bytes(layout, p) ||
            (uintptr_t)whole->view.planes[p] % hdrg_format_sample_bytes(layout->format) != 0 ||
            whole->view.strides[p] % hdrg_format_sample_bytes(layout->format) != 0) {
            status = HDRG_ERR_PLUGIN;
        }
    }

    if (status != HDRG_OK) {
        if (whole->handle != NULL) {
            stage->cls->frame_free(stage->inst, whole->handle, hdrg_class_api(stage->cls));
        }

        whole->handle = NULL;

        if (host != NULL) {
            hdrg_host_set_error(host, "filter frame allocation failed", stage->cls->id.data,
                                stage->cls->id.size);
        }

        return HDRG_ERR_PLUGIN;
    }

    whole->ready = true;

    return HDRG_OK;
}

/* the frames of stage's ring and from's outputs are made alike: one may take the other's place */
static bool hdrg_stage_alike(const hdrg_stage_t *stage, const hdrg_stage_t *from)
{
    const hdrg_format_layout_t *in = &stage->in_layout, *out = &from->out_layout;

    if (in->format != out->format || in->width != out->width || in->height != out->height ||
        in->bytes != out->bytes) {
        return false;
    }

    if (stage->cls->frame_alloc == NULL || from->cls->frame_alloc == NULL) {
        return stage->cls->frame_alloc == from->cls->frame_alloc && stage->alloc == from->alloc;
    }

    return stage->cls->frame_alloc == from->cls->frame_alloc &&
           stage->cls->frame_free == from->cls->frame_free;
}

static void hdrg_stage_whole_free(hdrg_stage_t *stage, hdrg_stage_whole_t *whole)
{
    if (whole->handle != NULL) {
        stage->cls->frame_free(stage->inst, whole->handle, hdrg_class_api(stage->cls));
    }

    hdrg_buffer_free(stage->alloc, &whole->buffer);
    memset(whole, 0, sizeof(*whole));
}

/* the ring for frames of width x height (made at the first): its frames stay where they are */
static hdrg_status_t hdrg_stage_ring(hdrg_stage_t *stage, uint32_t width, uint32_t height)
{
    hdrg_format_layout_t layout;
    uint32_t k;

    if (!hdrg_format_layout(stage->filter.in_format, width, height, true, &layout)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (stage->ring == NULL) {
        stage->ring = (hdrg_stage_whole_t *)hdrg_mem_calloc(stage->alloc, stage->ring_frames,
                                                            sizeof(*stage->ring));
        stage->ring_pts =
            (int64_t *)hdrg_mem_calloc(stage->alloc, stage->ring_frames, sizeof(int64_t));
        stage->ring_index =
            (uint32_t *)hdrg_mem_alloc(stage->alloc, stage->ring_frames * sizeof(uint32_t));
        stage->ring_cut = (uint8_t *)hdrg_mem_calloc(stage->alloc, stage->ring_frames, 1);
        if (stage->ring == NULL || stage->ring_pts == NULL || stage->ring_index == NULL ||
            stage->ring_cut == NULL) {
            hdrg_mem_free(stage->alloc, stage->ring);
            hdrg_mem_free(stage->alloc, stage->ring_pts);
            hdrg_mem_free(stage->alloc, stage->ring_index);
            hdrg_mem_free(stage->alloc, stage->ring_cut);
            stage->ring = NULL;
            stage->ring_pts = NULL;
            stage->ring_index = NULL;
            stage->ring_cut = NULL;

            return HDRG_ERR_NOMEM;
        }

        for (k = 0; k < stage->ring_frames; k++) {
            stage->ring_index[k] = UINT32_MAX;
        }

        stage->rings_ready = stage->ring_frames;
        stage->in_layout = layout;
    }

    return (layout.bytes == stage->in_layout.bytes) ? HDRG_OK : HDRG_ERR_INVALID_STATE;
}

/* rows [begin, end) of src as RGB16 into rows of stride bytes at rows */
static void hdrg_stage_source(const hdrg_exec_t *exec, const hdrg_source_t *src, uint32_t begin,
                              uint32_t end, uint8_t *rows, size_t stride)
{
    hdrg_rgb16_job_t job;

    job.src = src;
    job.rows = rows;
    job.stride = stride;
    job.begin = begin;

    hdrg_exec_rows(exec, end - begin, hdrg_rgb16_rows, &job);
}

/* rows [begin, end) of src in the input format into frame (of layout: rows end - begin) */
static hdrg_status_t hdrg_stage_pack(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const hdrg_source_t *src, uint32_t begin, uint32_t end,
                                     const hdrg_format_layout_t *layout,
                                     const hdrg_format_frame_t *frame)
{
    const size_t tight = (size_t)src->width * 3u * sizeof(uint16_t);
    hdrg_status_t status;

    if (layout->format == HDRG_FORMAT_RGB16) {
        hdrg_stage_source(exec, src, begin, end, frame->planes[0], frame->strides[0]);

        return HDRG_OK;
    }

    status = hdrg_buffer_fit(stage->alloc, &stage->in16, tight * (end - begin));
    if (status == HDRG_OK) {
        status = hdrg_buffer_fit(stage->alloc, &stage->scratch, hdrg_format_scratch(layout));
    }

    if (status != HDRG_OK) {
        return status;
    }

    hdrg_stage_source(exec, src, begin, end, stage->in16.data, tight);
    hdrg_format_pack(exec, layout, &stage->filter.yuv,
                     (const uint16_t *)(const void *)stage->in16.data, tight, 0, end - begin, frame,
                     stage->scratch.data);

    return HDRG_OK;
}

/* a whole frame of layout as RGB16: *rgb (in place when it is RGB16, else into buf) */
static hdrg_status_t hdrg_stage_view(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const hdrg_format_layout_t *layout,
                                     const hdrg_format_frame_t *frame, hdrg_stage_buffer_t *buf,
                                     const uint16_t **rgb, size_t *stride)
{
    const size_t tight = (size_t)layout->width * 3u * sizeof(uint16_t);
    hdrg_status_t status;

    if (layout->format == HDRG_FORMAT_RGB16) {
        *rgb = (const uint16_t *)(const void *)frame->planes[0];
        *stride = frame->strides[0];

        return HDRG_OK;
    }

    status = hdrg_buffer_fit(stage->alloc, buf, tight * layout->height);
    if (status == HDRG_OK) {
        status = hdrg_buffer_fit(stage->alloc, &stage->scratch, hdrg_format_scratch(layout));
    }

    if (status != HDRG_OK) {
        return status;
    }

    hdrg_format_unpack(exec, layout, &stage->filter.yuv, frame, 0, layout->height,
                       (uint16_t *)(void *)buf->data, tight, stage->scratch.data);

    *rgb = (const uint16_t *)(const void *)buf->data;
    *stride = tight;

    return HDRG_OK;
}

/* input rows of output rows [begin, end): the filter's, else scaled and widened by the halo */
static hdrg_status_t hdrg_stage_input(const hdrg_stage_t *scale, uint32_t begin, uint32_t end,
                                      uint32_t *in_begin, uint32_t *in_end)
{
    const uint32_t in_h = scale->src->height;
    uint64_t lo, hi;

    if (scale->cls->input_rows != NULL) {
        if (scale->cls->input_rows(scale->inst, begin, end, in_begin, in_end,
                                   hdrg_class_api(scale->cls)) != HDRG_OK) {
            return HDRG_ERR_PLUGIN;
        }
    } else {
        lo = (uint64_t)begin * in_h / scale->out_height;
        hi = ((uint64_t)end * in_h + scale->out_height - 1u) / scale->out_height;
        *in_begin = (uint32_t)((lo > scale->halo) ? lo - scale->halo : 0u);
        *in_end = (uint32_t)HDRG_MIN(hi + scale->halo, (uint64_t)in_h);
    }

    return (*in_begin < *in_end && *in_end <= in_h) ? HDRG_OK : HDRG_ERR_PLUGIN;
}

/* the submitted job's end: job_done from any thread (in builds without threads, within submit) */
static hdrg_status_t hdrg_stage_wait(hdrg_job_end_t *end)
{
    hdrg_status_t result;
    bool ended;

    hdrg_mutex_lock(&end->mutex);

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    ended = end->ended;
#else
    while (!end->ended) {
        hdrg_cond_wait(&end->cond, &end->mutex);
    }

    ended = true;
#endif

    result = end->result;
    hdrg_mutex_unlock(&end->mutex);

    return (ended && result == HDRG_OK) ? HDRG_OK : HDRG_ERR_PLUGIN;
}

/*
 * The job of output rows [begin, end) from input rows [in_begin, in_end):
 * the input frame's row in_first in in, the output's row out_first in out
 */
static void hdrg_stage_job(const hdrg_stage_t *stage, hdrg_job_t *job,
                           const hdrg_format_frame_t *in, uint32_t in_first,
                           const hdrg_format_frame_t *out, uint32_t out_first, uint32_t begin,
                           uint32_t end, uint32_t in_begin, uint32_t in_end)
{
    const uint32_t in_format = stage->filter.in_format, out_format = stage->out_format;
    uint8_t p;

    memset(job, 0, sizeof(*job));
    job->magic = HDRG_JOB_MAGIC;
    job->in_planes_n = hdrg_format_planes(in_format);
    job->out_planes_n = hdrg_format_planes(out_format);

    for (p = 0; p < job->in_planes_n; p++) {
        job->in_planes[p] =
            in->planes[p] +
            (size_t)hdrg_format_plane_row(in_format, p, in_begin - in_first) * in->strides[p];
        job->in_strides[p] = in->strides[p];
    }

    for (p = 0; p < job->out_planes_n; p++) {
        job->out_planes[p] =
            out->planes[p] +
            (size_t)hdrg_format_plane_row(out_format, p, begin - out_first) * out->strides[p];
        job->out_strides[p] = out->strides[p];
    }

    job->window = (stage->current != NULL) ? stage->window + stage->past : NULL;
    job->window_handles = (stage->current != NULL) ? stage->handles + stage->past : NULL;
    job->device_window = (stage->device_ring != NULL) ? stage->device_window + stage->past : NULL;
    job->in_handle = (stage->current != NULL) ? stage->current->handle : NULL;
    job->in_format = in_format;
    job->begin = begin;
    job->end = end;
    job->in_begin = in_begin;
    job->in_end = in_end;
    job->index = stage->index;
    job->pts = stage->pts;
    job->flags = stage->flags;
    job->past = stage->have_past;
    job->future = stage->have_future;
    job->shot_past = stage->shots ? stage->shot_past : stage->have_past;
    job->shot_future = stage->shots ? stage->shot_future : stage->have_future;
    job->shots = stage->filter.stage != HDRG_STAGE_PREFILTER &&
                 (stage->shots || stage->have_past + stage->have_future == 0);
}

/* a job run: processed, or submitted and waited for (ASYNC) */
static hdrg_status_t hdrg_stage_run(hdrg_stage_t *scale, hdrg_job_t *job)
{
    hdrg_status_t status;

    job->end_of = (scale->flights != NULL) ? &scale->flights[0].end : NULL;

    if (job->end_of != NULL) {
        hdrg_mutex_lock(&job->end_of->mutex);
        job->end_of->ended = false;
        job->end_of->result = HDRG_OK;
        hdrg_mutex_unlock(&job->end_of->mutex);

        status = scale->cls->submit(scale->inst, job, hdrg_class_api(scale->cls));
        if (status == HDRG_OK) {
            status = hdrg_stage_wait(job->end_of);
        }
    } else {
        /* (a band's job has its worker's; the others the first) */
        if (job->scratch == NULL && hdrg_stage_scratches(scale, 1u)) {
            job->scratch = &scale->scratches[0];
        }

        status = scale->cls->process(scale->inst, job, hdrg_class_api(scale->cls));
    }

    job->magic = 0;

    return (status == HDRG_OK) ? HDRG_OK : HDRG_ERR_PLUGIN;
}

/* INTERP: each flight's output side data and phases, made at the first (none on a failure) */
static hdrg_status_t hdrg_stage_flight_sides(hdrg_stage_t *stage)
{
    uint32_t i;
    bool ok;

    if (stage->sides != NULL) {
        return HDRG_OK;
    }

    stage->sides = (hdrg_job_side_t *)hdrg_mem_calloc(stage->alloc, stage->max_inflight,
                                                      sizeof(hdrg_job_side_t));
    stage->phase_store = (uint32_t *)hdrg_mem_alloc(
        stage->alloc, (size_t)stage->max_inflight * HDRG_STAGE_PHASES * sizeof(uint32_t));
    ok = stage->sides != NULL && stage->phase_store != NULL;

    for (i = 0; ok && i < stage->max_inflight; i++) {
        ok = hdrg_mutex_init(&stage->sides[i].lock);
        stage->sides[i].alloc = stage->alloc;
        stage->sides_ready += ok ? 1u : 0u;
    }

    if (ok) {
        return HDRG_OK;
    }

    for (i = 0; i < stage->sides_ready; i++) {
        hdrg_mutex_destroy(&stage->sides[i].lock);
    }

    hdrg_mem_free(stage->alloc, stage->sides);
    hdrg_mem_free(stage->alloc, stage->phase_store);
    stage->sides = NULL;
    stage->phase_store = NULL;
    stage->sides_ready = 0;

    return HDRG_ERR_NOMEM;
}

/*
 * INTERP: the job's frame configured and its window's frames in the ring
 * by their index (at most past + future + max_inflight apart: once each),
 * the window's views in stage->window; *own: the frame t's slot
 */
static hdrg_status_t hdrg_stage_between(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                        const hdrg_stage_between_t *between,
                                        const hdrg_stage_frame_t *frame, hdrg_plugin_host_t *host,
                                        hdrg_stage_whole_t **own)
{
    const hdrg_source_t *t = between->frames[between->past], *src;
    hdrg_stage_whole_t *slot;
    hdrg_status_t status;
    uint32_t k, at, place;

    *own = NULL;

    if (between->future == 0 || between->past > stage->past || between->future > stage->future ||
        between->past > frame->index) {
        return HDRG_ERR_INVALID_ARG;
    }

    for (k = 0; k <= between->past + between->future; k++) {
        src = between->frames[k];

        if (src == NULL || src->width != t->width || src->height != t->height) {
            return HDRG_ERR_INVALID_ARG;
        }
    }

    status = hdrg_stage_configure(stage, t, t->width, t->height, frame, host);
    if (status == HDRG_OK) {
        status = hdrg_stage_ring(stage, t->width, t->height);
    }

    for (k = 0; k < 2u * HDRG_LIMIT_WINDOW_MAX + 1u; k++) {
        stage->window[k] = NULL;
        stage->handles[k] = NULL;
    }

    for (k = 0; status == HDRG_OK && k <= between->past + between->future; k++) {
        at = frame->index - between->past + k;
        place = at % stage->ring_frames;
        slot = &stage->ring[place];

        if (stage->ring_index[place] != at) {
            stage->ring_index[place] = UINT32_MAX;
            status = hdrg_stage_whole(stage, slot, &stage->in_layout, host);

            if (status == HDRG_OK) {
                status = hdrg_stage_pack(stage, exec, between->frames[k], 0, t->height,
                                         &stage->in_layout, &slot->view);
            }

            if (status == HDRG_OK) {
                stage->ring_index[place] = at;
            }
        }

        stage->window[stage->past - between->past + k] = &slot->view;
        stage->handles[stage->past - between->past + k] = slot->handle;
        *own = (k == between->past) ? slot : *own;
    }

    return status;
}

/* INTERP: a job's window, phase and shot (its views in stage->window until submitted) */
static void hdrg_stage_between_job(hdrg_stage_t *stage, const hdrg_stage_between_t *between,
                                   const hdrg_stage_whole_t *own, const hdrg_stage_whole_t *output,
                                   hdrg_job_t *job)
{
    job->window = stage->window + stage->past;
    job->window_handles = stage->handles + stage->past;
    job->in_handle = own->handle;
    job->out_handle = output->handle;
    job->past = between->past;
    job->future = between->future;
    job->phase = between->phase;
    job->phase_count = between->phase_count;
    job->shot_past = between->shot_past;
    job->shot_future = between->shot_future;
    job->shots = true;
}

/* jobs of a band: each sees exactly the input rows it declared */
static void hdrg_stage_band_rows(uint32_t row_begin, uint32_t row_end, uint32_t worker, void *arg)
{
    const hdrg_stage_band_t *band = (const hdrg_stage_band_t *)arg;
    hdrg_stage_t *scale = band->scale;
    hdrg_job_t job;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end;

    begin = band->y0 + row_begin;
    end = band->y0 + row_end;

    status = hdrg_stage_input(scale, begin, end, &in_begin, &in_end);
    if (status == HDRG_OK && (in_begin < band->in_begin || in_end > band->in_end)) {
        status = HDRG_ERR_PLUGIN;
    }

    if (status == HDRG_OK) {
        hdrg_stage_job(scale, &job, band->in, band->in_first, band->out, band->y0, begin, end,
                       in_begin, in_end);
        job.scratch = &scale->scratches[worker];
        job.asked = &scale->asked[row_begin / HDRG_EXEC_GRAIN_ROWS];

        status = hdrg_stage_run(scale, &job);
    }

    scale->results[row_begin / HDRG_EXEC_GRAIN_ROWS] = status;
}

/*
 * a filter without BANDS: the whole frame once, at the frame's first rows
 * (its input and output on the device instead when the chain keeps them
 * there)
 */
/* the output's side data made ready for a job: none until it gives some (false: no lock) */
static bool hdrg_stage_side(hdrg_stage_t *stage)
{
    if (!stage->side_ready) {
        if (!hdrg_mutex_init(&stage->side.lock)) {
            return false;
        }

        stage->side.alloc = stage->alloc;
        stage->side_ready = true;
    }

    stage->side.size = 0;

    return true;
}

static hdrg_status_t hdrg_stage_frame(hdrg_stage_t *scale, const hdrg_exec_t *exec,
                                      hdrg_plugin_host_t *host)
{
    static const hdrg_format_frame_t none;
    const hdrg_format_frame_t *in, *out;
    hdrg_job_t job;
    hdrg_status_t status;

    status = scale->device_out ? HDRG_OK
                               : hdrg_stage_whole(scale, &scale->output, &scale->out_layout, host);

    /* the input: the taken frame of the ring, or the source in the input format */
    if (status == HDRG_OK && scale->current == NULL && scale->device_in == NULL) {
        status = hdrg_stage_whole(scale, &scale->input, &scale->in_layout, host);
        if (status == HDRG_OK) {
            status = hdrg_stage_pack(scale, exec, scale->src, 0, scale->src->height,
                                     &scale->in_layout, &scale->input.view);
        }
    }

    in = (scale->current != NULL) ? &scale->current->view : &scale->input.view;
    in = (scale->device_in != NULL) ? &none : in;
    out = scale->device_out ? &none : &scale->output.view;

    if (status == HDRG_OK) {
        hdrg_stage_job(scale, &job, in, 0, out, 0, 0, scale->out_height, 0, scale->src->height);

        job.in_handle = (scale->current != NULL) ? scale->current->handle : scale->input.handle;
        job.out_handle = scale->output.handle;

        /* the device frames: the previous filter's in, one of the filter's own out */
        if (scale->device_in != NULL) {
            job.in_planes_n = 0;
            job.in_handle = NULL;
            job.in_device = scale->device_in;
            job.in_ready = scale->device_in_ready;
        }

        if (scale->device_out) {
            job.out_planes_n = 0;
            job.out_handle = NULL;
            job.out_wanted = 1;
        }

        /* a PREFILTER job's side data (the frame's motion it adds) */
        if (scale->filter.stage == HDRG_STAGE_PREFILTER) {
            status = hdrg_stage_side(scale) ? HDRG_OK : HDRG_ERR_NOMEM;
            job.side = &scale->side;
        }

        status = (status == HDRG_OK) ? hdrg_stage_run(scale, &job) : status;

        if (status == HDRG_OK && scale->device_out && job.out_device == NULL) {
            status = HDRG_ERR_PLUGIN;
        }

        if (status == HDRG_ERR_PLUGIN) {
            hdrg_host_set_error(host, "filter process failed", scale->cls->id.data,
                                scale->cls->id.size);
        }

        /* a failed job's device frame back to its filter: nothing reads it */
        if (status != HDRG_OK && job.out_device != NULL) {
            scale->cls->device_release(scale->inst, job.out_device, hdrg_class_api(scale->cls));
            job.out_device = NULL;
        }

        scale->device_frame = job.out_device;
        scale->device_ready = job.out_ready;
    }

    /* 4:2:0 converts back whole */
    if (status == HDRG_OK && !scale->device_out && scale->out_format == HDRG_FORMAT_P016) {
        status = hdrg_stage_view(scale, exec, &scale->out_layout, &scale->output.view,
                                 &scale->frame16, &scale->done_rgb, &scale->done_stride);
    }

    scale->done = status == HDRG_OK;

    return status;
}

/*
 * SCALE: the stage fitting the filter's size to the requested one, made at
 * the first (none on a failure)
 */
static hdrg_status_t hdrg_stage_fitter(hdrg_stage_t *stage, hdrg_plugin_host_t *host)
{
    hdrg_status_t status;

    if (stage->fit != NULL) {
        return HDRG_OK;
    }

    stage->fit_entry.cls = hdrg_default_find(host, HDRG_DEFAULT_LANCZOS);
    stage->fit_entry.stage = HDRG_STAGE_SCALE;
    if (stage->fit_entry.cls == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_host_resolve_options(host, stage->fit_entry.cls, NULL, &stage->fit_options);
    if (status != HDRG_OK) {
        return status;
    }

    stage->fit_entry.options = stage->fit_options;
    stage->fit = (hdrg_stage_t *)hdrg_mem_calloc(stage->alloc, 1, sizeof(hdrg_stage_t));
    status = (stage->fit != NULL) ? hdrg_stage_create(stage->alloc, &stage->fit_entry,
                                                      stage->max_band_rows, host, stage->fit)
                                  : HDRG_ERR_NOMEM;

    /* a stage not made: none kept (its create closed it) */
    if (status != HDRG_OK) {
        hdrg_mem_free(stage->alloc, stage->fit);
        hdrg_options_destroy(stage->fit_options);
        stage->fit = NULL;
        stage->fit_options = NULL;
    }

    return status;
}

/*
 * SCALE, fitting: rows [y0, y0 + rows) of the requested size, the filter's
 * whole output (run, landed or converted to RGB16 first) through the fit
 */
static hdrg_status_t hdrg_stage_fit_rows(hdrg_stage_t *scale, const hdrg_exec_t *exec, uint32_t y0,
                                         uint32_t rows, uint16_t *dst, hdrg_plugin_host_t *host)
{
    const uint16_t *rgb;
    hdrg_status_t status;
    size_t stride;

    status = HDRG_OK;

    if (!scale->fit_ready && !scale->done) {
        status = hdrg_stage_frame(scale, exec, host);
    }

    if (!scale->fit_ready && status == HDRG_OK) {
        rgb = scale->done_rgb;
        stride = scale->done_stride;

        if (rgb == NULL) {
            status = hdrg_stage_view(scale, exec, &scale->out_layout, &scale->output.view,
                                     &scale->frame16, &rgb, &stride);
        }

        scale->fit_src = *scale->src;
        scale->fit_src.pixels = (const uint8_t *)rgb;
        scale->fit_src.lut = NULL;
        scale->fit_src.stride = stride;
        scale->fit_src.width = scale->out_width;
        scale->fit_src.height = scale->out_height;
        scale->fit_src.row0 = 0;
        scale->fit_src.entries = 65536;
        scale->fit_src.comps = 3;
        scale->fit_src.wide = true;
    }

    if (!scale->fit_ready && status == HDRG_OK) {
        status = hdrg_stage_configure(scale->fit, &scale->fit_src, scale->req_width,
                                      scale->req_height, NULL, host);
        scale->fit_ready = status == HDRG_OK;
    }

    if (status != HDRG_OK) {
        return status;
    }

    return hdrg_stage_rows(scale->fit, exec, y0, rows, dst, host);
}

/* rows [y0, y0 + rows) of a filter's band in BANDS */
static hdrg_status_t hdrg_stage_band(hdrg_stage_t *scale, const hdrg_exec_t *exec, uint32_t y0,
                                     uint32_t rows, uint16_t *dst, hdrg_plugin_host_t *host)
{
    hdrg_format_layout_t in_band, out_band;
    hdrg_format_frame_t in_view, out_view;
    hdrg_stage_band_t band;
    hdrg_status_t status;
    uint32_t chunks, i;

    status = hdrg_stage_input(scale, y0, y0 + rows, &band.in_begin, &band.in_end);
    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter input rows out of range", scale->cls->id.data,
                            scale->cls->id.size);

        return status;
    }

    band.scale = scale;
    band.y0 = y0;

    /* the input: the taken frame's rows, or the band's source rows in the input format */
    if (scale->current != NULL) {
        band.in = &scale->current->view;
        band.in_first = 0;
    } else {
        /* a part of the frame configure took: its layout exists */
        if (!hdrg_format_layout(scale->filter.in_format, scale->src->width,
                                band.in_end - band.in_begin, false, &in_band)) {
            return HDRG_ERR_INTERNAL;
        }

        status = hdrg_buffer_fit(scale->alloc, &scale->in_rows, in_band.bytes);
        if (status != HDRG_OK) {
            return status;
        }

        hdrg_format_view(&in_band, scale->in_rows.data, &in_view);

        status = hdrg_stage_pack(scale, exec, scale->src, band.in_begin, band.in_end, &in_band,
                                 &in_view);
        if (status != HDRG_OK) {
            return status;
        }

        band.in = &in_view;
        band.in_first = band.in_begin;
    }

    /* the output: dst itself (RGB16), or the band's rows in the output format */
    if (!hdrg_format_layout(scale->out_format, scale->out_width, rows, false, &out_band)) {
        return HDRG_ERR_INTERNAL;
    }

    if (scale->out_format == HDRG_FORMAT_RGB16) {
        hdrg_format_view(&out_band, (uint8_t *)dst, &out_view);
    } else {
        status = hdrg_buffer_fit(scale->alloc, &scale->out_rows, out_band.bytes);
        if (status != HDRG_OK) {
            return status;
        }

        hdrg_format_view(&out_band, scale->out_rows.data, &out_view);
    }

    band.out = &out_view;

    /*
     * each worker its scratch, grown to what the earlier bands' jobs asked for: whichever
     * worker takes a job, the band allocates nothing once the frames' sizes are reached
     */
    if (!hdrg_stage_scratches(scale, ((scale->cls->flags & HDRG_FILTER_PURE) != 0)
                                         ? hdrg_exec_concurrency(exec)
                                         : 1u) ||
        !hdrg_stage_scratch_grow(scale)) {
        return HDRG_ERR_NOMEM;
    }

    memset(scale->asked, 0,
           (size_t)hdrg_pool_row_chunks(rows, HDRG_EXEC_GRAIN_ROWS) * sizeof(size_t));

    /* a filter that is not PURE gets the band as one job */
    if ((scale->cls->flags & HDRG_FILTER_PURE) != 0) {
        chunks = hdrg_pool_row_chunks(rows, HDRG_EXEC_GRAIN_ROWS);

        hdrg_exec_worker_rows(exec, rows, hdrg_stage_band_rows, &band);
    } else {
        chunks = 1;

        hdrg_stage_band_rows(0, rows, 0, &band);
    }

    for (i = 0; i < chunks; i++) {
        scale->scratch_want = HDRG_MAX(scale->scratch_want, scale->asked[i]);
    }

    for (i = 0; i < chunks; i++) {
        if (scale->results[i] != HDRG_OK) {
            hdrg_host_set_error(host, "filter process failed", scale->cls->id.data,
                                scale->cls->id.size);

            return HDRG_ERR_PLUGIN;
        }
    }

    if (scale->out_format != HDRG_FORMAT_RGB16) {
        hdrg_format_unpack(exec, &out_band, &scale->filter.yuv, &out_view, 0, rows, dst,
                           (size_t)scale->out_width * 3u * sizeof(uint16_t), NULL);
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_create(const hdrg_allocator_t *alloc,
                                       const hdrg_filter_entry_t *entry, uint32_t max_band_rows,
                                       hdrg_plugin_host_t *host, hdrg_stage_t *out)
{
    const hdrg_class_t *cls = entry->cls;
    const uint32_t jobs = max_band_rows / HDRG_EXEC_GRAIN_ROWS + 1u;
    hdrg_stage_flight_t *flight;
    hdrg_status_t status;
    uint32_t i;

    memset(out, 0, sizeof(*out));

    out->filter.magic = HDRG_FILTER_MAGIC;
    out->filter.cls = cls;
    out->filter.host = host;
    out->filter.stage = entry->stage;
    out->filter.in_format = HDRG_FORMAT_RGB16;
    out->filter.yuv.matrix = 1u; /* BT.709 */
    out->cls = cls;
    out->alloc = alloc;
    out->max_band_rows = max_band_rows;
    out->max_inflight = 1;

    out->results = (hdrg_status_t *)hdrg_mem_calloc(alloc, jobs, sizeof(hdrg_status_t));
    out->asked = (size_t *)hdrg_mem_calloc(alloc, jobs, sizeof(size_t));
    if (out->results == NULL || out->asked == NULL) {
        return HDRG_ERR_NOMEM;
    }

    out->filter.creating = true;

    status = cls->create(&out->filter, entry->options, &out->inst, hdrg_class_api(cls));

    out->filter.creating = false;
    out->past = out->filter.past;
    out->future =
        (entry->stage == HDRG_STAGE_INTERP) ? HDRG_MAX(out->filter.future, 1u) : out->filter.future;
    out->clip = out->filter.clip;
    out->overlap = out->filter.overlap;

    /* (HDRG_ERR_UNSUPPORTED kept: a GPU filter without a device, which the caller may replace) */
    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter create failed", cls->id.data, cls->id.size);
        hdrg_stage_close(out);

        return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_ERR_UNSUPPORTED : HDRG_ERR_PLUGIN;
    }

    /* the state a SEQUENTIAL filter keeps resets at clips */
    if ((cls->flags & HDRG_FILTER_SEQUENTIAL) != 0 && out->clip == 0) {
        out->created = true;
        hdrg_host_set_error(host, "a SEQUENTIAL filter needs a clip", cls->id.data, cls->id.size);
        hdrg_stage_close(out);

        return HDRG_ERR_PLUGIN;
    }

    out->created = true;
    out->device = out->filter.device;
    out->ring_frames = out->past + out->future + 1u;

    /* frames on the device are whole frames the filter releases */
    if (out->device != 0 &&
        ((cls->flags & HDRG_FILTER_BANDS) != 0 || cls->device_release == NULL)) {
        hdrg_host_set_error(host, "a filter on a device needs whole frames and device_release",
                            cls->id.data, cls->id.size);
        hdrg_stage_close(out);

        return HDRG_ERR_PLUGIN;
    }

    /* 4:2:0 converts whole frames */
    if ((cls->flags & HDRG_FILTER_BANDS) != 0 && !hdrg_format_banded(out->filter.in_format)) {
        hdrg_host_set_error(host, "a filter in BANDS cannot take 4:2:0", cls->id.data,
                            cls->id.size);
        hdrg_stage_close(out);

        return HDRG_ERR_PLUGIN;
    }

    if ((cls->flags & HDRG_FILTER_ASYNC) == 0) {
        return HDRG_OK;
    }

    /* the jobs' ends: as many as the filter said it keeps in flight */
    if (out->filter.max_inflight == 0) {
        hdrg_host_set_error(host, "an ASYNC filter declares its max_inflight", cls->id.data,
                            cls->id.size);
        hdrg_stage_close(out);

        return HDRG_ERR_PLUGIN;
    }

    out->max_inflight = out->filter.max_inflight;
    out->ring_frames = out->past + out->future + out->max_inflight;

    out->flights =
        (hdrg_stage_flight_t *)hdrg_mem_calloc(alloc, out->max_inflight, sizeof(*out->flights));
    if (out->flights == NULL) {
        hdrg_stage_close(out);

        return HDRG_ERR_NOMEM;
    }

    for (i = 0; i < out->max_inflight; i++) {
        flight = &out->flights[i];

        if (!hdrg_mutex_init(&flight->end.mutex)) {
            hdrg_stage_close(out);

            return HDRG_ERR_NOMEM;
        }

        if (!hdrg_cond_init(&flight->end.cond)) {
            hdrg_mutex_destroy(&flight->end.mutex);
            hdrg_stage_close(out);

            return HDRG_ERR_NOMEM;
        }

        out->flights_ready++;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_configure(hdrg_stage_t *stage, const hdrg_source_t *src,
                                          uint32_t out_width, uint32_t out_height,
                                          const hdrg_stage_frame_t *frame, hdrg_plugin_host_t *host)
{
    const hdrg_class_t *cls = stage->cls;
    hdrg_format_layout_t in_layout, out_layout;
    hdrg_negotiation_t nego;
    hdrg_status_t status;
    bool fitting;

    stage->src = src;
    stage->out_width = out_width;
    stage->out_height = out_height;
    stage->req_width = out_width;
    stage->req_height = out_height;
    stage->fitting = false;
    stage->fit_ready = false;
    stage->configured = false;
    stage->done = false;
    stage->passthrough = false;
    stage->done_rgb = NULL;
    stage->device_in = NULL;
    stage->device_out = false;
    stage->halo = 0;

    memset(&nego, 0, sizeof(nego));
    nego.magic = HDRG_NEGO_MAGIC;
    nego.in_width = src->width;
    nego.in_height = src->height;
    nego.in_format = stage->filter.in_format;
    nego.req_width = out_width;
    nego.req_height = out_height;
    nego.noise = (frame != NULL) ? frame->noise : NULL;
    nego.motion = (frame != NULL) ? frame->motion : NULL;
    nego.motion_size = (frame != NULL) ? frame->motion_size : 0u;
    nego.xy = (frame != NULL) ? frame->xy : NULL;
    nego.transfer = (frame != NULL) ? frame->transfer : 2u;
    nego.bits = (frame != NULL) ? frame->bits : 16u;
    stage->index = (frame != NULL) ? frame->index : 0u;
    stage->pts = (frame != NULL) ? frame->pts : 0;
    stage->flags = (stage->current != NULL || stage->device_ring != NULL)
                       ? stage->taken_flags
                       : ((frame == NULL || frame->first) ? HDRG_JOB_FIRST : 0u);
    status = cls->configure(stage->inst, &nego, hdrg_class_api(cls));

    nego.magic = 0;

    /* SCALE, whole frames: a size of the filter's own (a model's factor), fitted to the request */
    fitting = status == HDRG_OK && nego.output_set && !nego.passthrough &&
              stage->filter.stage == HDRG_STAGE_SCALE && (cls->flags & HDRG_FILTER_BANDS) == 0 &&
              (nego.out_width != out_width || nego.out_height != out_height) &&
              (uint64_t)nego.out_width * nego.out_height <= HDRG_MAX_PIXELS_CAP;

    if (fitting) {
        out_width = nego.out_width;
        out_height = nego.out_height;
        stage->out_width = out_width;
        stage->out_height = out_height;
        status = hdrg_stage_fitter(stage, host);

        if (status != HDRG_OK) {
            return status;
        }
    }

    /* the output's format stays: its frames are made once */
    if (status != HDRG_OK || !nego.output_set || nego.out_width != out_width ||
        nego.out_height != out_height ||
        ((cls->flags & HDRG_FILTER_BANDS) != 0 && !hdrg_format_banded(nego.out_format)) ||
        (stage->out_format != 0 && nego.out_format != stage->out_format)) {
        hdrg_host_set_error(host, "filter cannot produce the requested size", cls->id.data,
                            cls->id.size);

        return HDRG_ERR_PLUGIN;
    }

    if (nego.passthrough && (out_width != src->width || out_height != src->height)) {
        hdrg_host_set_error(host, "a filter passing frames through cannot resize them",
                            cls->id.data, cls->id.size);

        return HDRG_ERR_PLUGIN;
    }

    /* frames past the formats' sides (HDRG_MAX_DIMENSION) have no layout: refused */
    if (!hdrg_format_layout(stage->filter.in_format, src->width, src->height, true, &in_layout) ||
        !hdrg_format_layout(nego.out_format, out_width, out_height, true, &out_layout)) {
        hdrg_host_set_error(host, "frame too large for the filter's formats", cls->id.data,
                            cls->id.size);

        return HDRG_ERR_PLUGIN;
    }

    /* the frames made for the stage (the ring's, the outputs) are reused: they keep their size */
    if ((stage->in_layout.bytes != 0 && (in_layout.width != stage->in_layout.width ||
                                         in_layout.height != stage->in_layout.height)) ||
        (stage->out_layout.bytes != 0 && (out_layout.width != stage->out_layout.width ||
                                          out_layout.height != stage->out_layout.height))) {
        hdrg_host_set_error(host, "a stage's frames keep their size", cls->id.data, cls->id.size);

        return HDRG_ERR_INTERNAL;
    }

    stage->in_layout = in_layout;
    stage->out_layout = out_layout;
    stage->out_format = nego.out_format;
    stage->halo = nego.halo;
    stage->passthrough = nego.passthrough || (frame != NULL && frame->bypass && stage->past == 0 &&
                                              stage->future == 0 && stage->clip == 0);
    stage->fitting = fitting;
    stage->configured = true;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_push(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const hdrg_source_t *src, int64_t pts, hdrg_stage_t *from)
{
    hdrg_stage_whole_t *slot, held;
    hdrg_status_t status;

    status = hdrg_stage_ring(stage, src->width, src->height);
    if (status != HDRG_OK) {
        return status;
    }

    slot = &stage->ring[stage->pushed % stage->ring_frames];

    /* the frame from landed, handed over: the slot's goes to from's flight for its next output */
    if (from != NULL && from->landed != NULL &&
        src->pixels == (const uint8_t *)from->landed->view.planes[0] &&
        src->stride == from->landed->view.strides[0] && hdrg_stage_alike(stage, from)) {
        held = *slot;
        *slot = *from->landed;
        *from->landed = held;
        from->landed = NULL;
    } else if (!slot->ready || stage->in_layout.format != HDRG_FORMAT_RGB16 ||
               src->pixels != (const uint8_t *)slot->view.planes[0] ||
               src->stride != slot->view.strides[0]) {
        /* (a frame written into the slot already, hdrg_stage_intake's, stays as it is) */
        status = hdrg_stage_whole(stage, slot, &stage->in_layout, NULL);
        if (status == HDRG_OK) {
            status =
                hdrg_stage_pack(stage, exec, src, 0, src->height, &stage->in_layout, &slot->view);
        }

        if (status != HDRG_OK) {
            return status;
        }
    }

    stage->ring_pts[stage->pushed % stage->ring_frames] = pts;
    stage->ring_cut[stage->pushed % stage->ring_frames] = 0;
    stage->pushed++;

    return HDRG_OK;
}

extern void hdrg_stage_cut(hdrg_stage_t *stage, bool cut)
{
    if (stage->ring_cut != NULL && stage->pushed > 0) {
        stage->ring_cut[(stage->pushed - 1u) % stage->ring_frames] = cut ? 1u : 0u;
        stage->shots = true;
    }
}

extern hdrg_status_t hdrg_stage_push_device(hdrg_stage_t *stage, void *frame, int64_t pts)
{
    if (stage->ring != NULL || frame == NULL) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (stage->device_ring == NULL) {
        stage->device_ring =
            (void **)hdrg_mem_calloc(stage->alloc, stage->ring_frames, sizeof(void *));
        stage->ring_pts =
            (int64_t *)hdrg_mem_calloc(stage->alloc, stage->ring_frames, sizeof(int64_t));
        stage->ring_cut = (uint8_t *)hdrg_mem_calloc(stage->alloc, stage->ring_frames, 1);
        if (stage->device_ring == NULL || stage->ring_pts == NULL || stage->ring_cut == NULL) {
            hdrg_mem_free(stage->alloc, stage->device_ring);
            hdrg_mem_free(stage->alloc, stage->ring_pts);
            hdrg_mem_free(stage->alloc, stage->ring_cut);
            stage->device_ring = NULL;
            stage->ring_pts = NULL;
            stage->ring_cut = NULL;

            return HDRG_ERR_NOMEM;
        }
    }

    stage->device_ring[stage->pushed % stage->ring_frames] = frame;
    stage->ring_pts[stage->pushed % stage->ring_frames] = pts;
    stage->ring_cut[stage->pushed % stage->ring_frames] = 0;
    stage->pushed++;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_intake(hdrg_stage_t *stage, uint32_t width, uint32_t height,
                                       uint16_t **rgb, size_t *stride)
{
    hdrg_stage_whole_t *slot;
    hdrg_status_t status;

    if (stage->filter.in_format != HDRG_FORMAT_RGB16) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_stage_ring(stage, width, height);
    if (status != HDRG_OK) {
        return status;
    }

    slot = &stage->ring[stage->pushed % stage->ring_frames];

    status = hdrg_stage_whole(stage, slot, &stage->in_layout, NULL);
    if (status != HDRG_OK) {
        return status;
    }

    *rgb = (uint16_t *)(void *)slot->view.planes[0];
    *stride = slot->view.strides[0];

    return HDRG_OK;
}

extern bool hdrg_stage_take(hdrg_stage_t *stage, uint32_t origin, bool ended)
{
    const hdrg_stage_whole_t *frame;
    uint32_t k, at;

    if ((stage->ring == NULL && stage->device_ring == NULL) || stage->pushed == stage->taken ||
        (!ended && stage->pushed - stage->taken <= stage->future)) {
        return false;
    }

    stage->have_past = HDRG_MIN(stage->past, stage->taken);
    stage->have_future = HDRG_MIN(stage->future, stage->pushed - 1u - stage->taken);

    /* a clip's window ends at its clip and its overlap; state resets at its first frame */
    at = (stage->clip > 0) ? (origin + stage->taken) % stage->clip : 1u;
    stage->taken_flags = (stage->taken == 0 || at == 0) ? HDRG_JOB_FIRST : 0u;

    if (stage->clip > 0) {
        stage->have_past = HDRG_MIN(stage->have_past, at + stage->overlap);
        stage->have_future = HDRG_MIN(stage->have_future, stage->clip - 1u - at + stage->overlap);
    }

    for (k = 0; k < 2u * HDRG_LIMIT_WINDOW_MAX + 1u; k++) {
        stage->window[k] = NULL;
        stage->handles[k] = NULL;
        stage->device_window[k] = NULL;
    }

    /* the window on the device: the caller's frames (the input at past: device_in's) */
    for (k = stage->past - stage->have_past;
         stage->device_ring != NULL && k <= stage->past + stage->have_future; k++) {
        stage->device_window[k] =
            stage->device_ring[(stage->taken + k - stage->past) % stage->ring_frames];
    }

    for (k = stage->past - stage->have_past;
         stage->ring != NULL && k <= stage->past + stage->have_future; k++) {
        frame = &stage->ring[(stage->taken + k - stage->past) % stage->ring_frames];
        stage->window[k] = &frame->view;
        stage->handles[k] = frame->handle;
    }

    /* the window's frames in the taken one's shot: up to a cut (one at a frame: before it) */
    stage->shot_past = 0;
    stage->shot_future = 0;

    while (stage->shot_past < stage->have_past &&
           stage->ring_cut[(stage->taken - stage->shot_past) % stage->ring_frames] == 0) {
        stage->shot_past++;
    }

    while (stage->shot_future < stage->have_future &&
           stage->ring_cut[(stage->taken + stage->shot_future + 1u) % stage->ring_frames] == 0) {
        stage->shot_future++;
    }

    stage->current = (stage->ring != NULL) ? &stage->ring[stage->taken % stage->ring_frames] : NULL;
    stage->taken_pts = stage->ring_pts[stage->taken % stage->ring_frames];
    stage->taken++;

    return true;
}

extern const uint8_t *hdrg_stage_motion(const hdrg_stage_t *stage, size_t *size)
{
    *size = stage->side_ready ? stage->side.size : 0u;

    return (*size != 0) ? stage->side.data : NULL;
}

extern hdrg_status_t hdrg_stage_taken(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                      const uint16_t **rgb, size_t *stride)
{
    return hdrg_stage_view(stage, exec, &stage->in_layout, &stage->current->view, &stage->taken16,
                           rgb, stride);
}

extern hdrg_status_t hdrg_stage_settle(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                       hdrg_plugin_host_t *host)
{
    if (!stage->configured || stage->passthrough || stage->done ||
        (stage->cls->flags & HDRG_FILTER_BANDS) != 0) {
        return HDRG_OK;
    }

    return hdrg_stage_frame(stage, exec, host);
}

extern void hdrg_stage_device_release(hdrg_stage_t *stage)
{
    if (stage->device_frame != NULL) {
        stage->cls->device_release(stage->inst, stage->device_frame, hdrg_class_api(stage->cls));
    }

    stage->device_frame = NULL;
}

extern void hdrg_stage_cap_flights(hdrg_stage_t *stage, uint32_t most)
{
    if (stage->ring != NULL || stage->flights == NULL || stage->max_inflight <= most) {
        return;
    }

    stage->max_inflight = most;
    stage->ring_frames = HDRG_MAX(stage->past + stage->future + most, stage->reserved);
}

extern void hdrg_stage_reserve(hdrg_stage_t *stage, uint32_t frames)
{
    if (stage->ring == NULL) {
        stage->reserved = frames;
        stage->ring_frames = HDRG_MAX(stage->ring_frames, frames);
    }
}

extern void hdrg_stage_serve(hdrg_stage_t *stage, const uint16_t *rgb, size_t stride)
{
    stage->fit_ready = false;
    stage->done_rgb = rgb;
    stage->done_stride = stride;
    stage->passthrough = false;
    stage->configured = true;
    stage->done = true;
}

extern hdrg_status_t hdrg_stage_submit(hdrg_stage_t *stage, uint32_t index,
                                       hdrg_plugin_host_t *host)
{
    hdrg_stage_flight_t *flight;
    hdrg_job_t *job;
    hdrg_status_t status;

    if (stage->flights == NULL || !stage->configured || stage->current == NULL ||
        stage->flight_count == stage->max_inflight) {
        return HDRG_ERR_INVALID_STATE;
    }

    flight = &stage->flights[(stage->flight_head + stage->flight_count) % stage->max_inflight];
    flight->index = index;
    flight->submitted = false;
    flight->passed = NULL;
    memcpy(flight->window, stage->window, sizeof(flight->window));
    memcpy(flight->handles, stage->handles, sizeof(flight->handles));

    /* passed through: lands as it is, after the frames before it */
    if (stage->passthrough) {
        flight->passed = &stage->current->view;
        stage->flight_count++;

        return HDRG_OK;
    }

    status = hdrg_stage_whole(stage, &flight->output, &stage->out_layout, host);
    if (status != HDRG_OK) {
        return status;
    }

    job = &flight->job;

    hdrg_stage_job(stage, job, &stage->current->view, 0, &flight->output.view, 0, 0,
                   stage->out_height, 0, stage->in_layout.height);

    job->window = flight->window + stage->past;
    job->window_handles = flight->handles + stage->past;
    job->out_handle = flight->output.handle;
    job->end_of = &flight->end;

    hdrg_mutex_lock(&flight->end.mutex);
    flight->end.ended = false;
    flight->end.result = HDRG_OK;
    hdrg_mutex_unlock(&flight->end.mutex);

    status = stage->cls->submit(stage->inst, job, hdrg_class_api(stage->cls));
    if (status != HDRG_OK) {
        job->magic = 0;
        hdrg_host_set_error(host, "filter submit failed", stage->cls->id.data, stage->cls->id.size);

        return HDRG_ERR_PLUGIN;
    }

    flight->submitted = true;
    stage->flight_count++;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_land(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                     const uint16_t **rgb, size_t *stride, uint32_t *index,
                                     hdrg_plugin_host_t *host)
{
    hdrg_stage_flight_t *flight;
    hdrg_status_t status;

    if (stage->flight_count == 0) {
        return HDRG_ERR_INVALID_STATE;
    }

    flight = &stage->flights[stage->flight_head];
    status = HDRG_OK;
    stage->landed = NULL;

    if (flight->submitted) {
        status = hdrg_stage_wait(&flight->end);
        flight->job.magic = 0;
        flight->submitted = false;
    }

    stage->flight_head = (stage->flight_head + 1u) % stage->max_inflight;
    stage->flight_count--;

    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter process failed", stage->cls->id.data,
                            stage->cls->id.size);

        return status;
    }

    *index = flight->index;

    if (flight->passed != NULL) {
        return hdrg_stage_view(stage, exec, &stage->in_layout, flight->passed, &stage->frame16, rgb,
                               stride);
    }

    stage->landed = (stage->out_layout.format == HDRG_FORMAT_RGB16) ? &flight->output : NULL;

    return hdrg_stage_view(stage, exec, &stage->out_layout, &flight->output.view, &stage->frame16,
                           rgb, stride);
}

extern bool hdrg_stage_detach(hdrg_stage_t *stage, const uint8_t *pixels, hdrg_stage_whole_t *spare)
{
    hdrg_stage_whole_t held;

    if (stage->landed == NULL || pixels != (const uint8_t *)stage->landed->view.planes[0]) {
        return false;
    }

    held = *spare;
    *spare = *stage->landed;
    *stage->landed = held;
    stage->landed = NULL;

    return true;
}

extern void hdrg_stage_release(hdrg_stage_t *stage, hdrg_stage_whole_t *whole)
{
    hdrg_stage_whole_free(stage, whole);
}

extern hdrg_status_t hdrg_stage_interp(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                       const hdrg_stage_between_t *between,
                                       const hdrg_stage_frame_t *frame, hdrg_plugin_host_t *host,
                                       const uint16_t **rgb, size_t *stride, const uint8_t **motion,
                                       size_t *motion_size)
{
    hdrg_stage_whole_t *own;
    hdrg_job_t job;
    hdrg_status_t status;

    *motion = NULL;
    *motion_size = 0;

    /* the output's side data: the job's (none until it gives some) */
    if (!hdrg_stage_side(stage)) {
        return HDRG_ERR_NOMEM;
    }

    status = hdrg_stage_between(stage, exec, between, frame, host, &own);
    if (status == HDRG_OK && !stage->passthrough) {
        status = hdrg_stage_whole(stage, &stage->output, &stage->out_layout, host);
    }

    if (status != HDRG_OK) {
        return status;
    }

    /* passed through: the frame itself */
    if (stage->passthrough) {
        return hdrg_stage_view(stage, exec, &stage->in_layout, &own->view, &stage->frame16, rgb,
                               stride);
    }

    hdrg_stage_job(stage, &job, &own->view, 0, &stage->output.view, 0, 0, stage->out_height, 0,
                   stage->in_layout.height);
    hdrg_stage_between_job(stage, between, own, &stage->output, &job);
    job.side = &stage->side;
    job.phases = between->phases;

    status = hdrg_stage_run(stage, &job);
    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter process failed", stage->cls->id.data,
                            stage->cls->id.size);

        return status;
    }

    *motion = (stage->side.size != 0) ? stage->side.data : NULL;
    *motion_size = stage->side.size;

    return hdrg_stage_view(stage, exec, &stage->out_layout, &stage->output.view, &stage->frame16,
                           rgb, stride);
}

extern hdrg_status_t hdrg_stage_interp_submit(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                              const hdrg_stage_between_t *between,
                                              const hdrg_stage_frame_t *frame,
                                              hdrg_plugin_host_t *host)
{
    hdrg_stage_flight_t *flight;
    hdrg_stage_whole_t *own;
    hdrg_job_side_t *side;
    hdrg_job_t *job;
    hdrg_status_t status;
    uint32_t slot;

    if (stage->flights == NULL || stage->flight_count == stage->max_inflight ||
        between->phase_count > HDRG_STAGE_PHASES) {
        return HDRG_ERR_INVALID_STATE;
    }

    status = hdrg_stage_flight_sides(stage);
    if (status == HDRG_OK) {
        status = hdrg_stage_between(stage, exec, between, frame, host, &own);
    }

    if (status != HDRG_OK) {
        return status;
    }

    slot = (stage->flight_head + stage->flight_count) % stage->max_inflight;
    flight = &stage->flights[slot];
    side = &stage->sides[slot];
    flight->index = frame->index;
    flight->submitted = false;
    flight->passed = NULL;
    side->size = 0;
    memcpy(flight->window, stage->window, sizeof(flight->window));
    memcpy(flight->handles, stage->handles, sizeof(flight->handles));

    /* passed through: lands as it is, after the jobs before it */
    if (stage->passthrough) {
        flight->passed = &own->view;
        stage->flight_count++;

        return HDRG_OK;
    }

    status = hdrg_stage_whole(stage, &flight->output, &stage->out_layout, host);
    if (status != HDRG_OK) {
        return status;
    }

    job = &flight->job;

    hdrg_stage_job(stage, job, &own->view, 0, &flight->output.view, 0, 0, stage->out_height, 0,
                   stage->in_layout.height);
    hdrg_stage_between_job(stage, between, own, &flight->output, job);
    job->window = flight->window + stage->past;
    job->window_handles = flight->handles + stage->past;
    job->side = side;
    job->end_of = &flight->end;
    memcpy(stage->phase_store + (size_t)slot * HDRG_STAGE_PHASES, between->phases,
           (size_t)between->phase_count * sizeof(uint32_t));
    job->phases = stage->phase_store + (size_t)slot * HDRG_STAGE_PHASES;

    hdrg_mutex_lock(&flight->end.mutex);
    flight->end.ended = false;
    flight->end.result = HDRG_OK;
    hdrg_mutex_unlock(&flight->end.mutex);

    status = stage->cls->submit(stage->inst, job, hdrg_class_api(stage->cls));
    if (status != HDRG_OK) {
        job->magic = 0;
        hdrg_host_set_error(host, "filter submit failed", stage->cls->id.data, stage->cls->id.size);

        return HDRG_ERR_PLUGIN;
    }

    flight->submitted = true;
    stage->flight_count++;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_interp_land(hdrg_stage_t *stage, const hdrg_exec_t *exec,
                                            hdrg_plugin_host_t *host, const uint16_t **rgb,
                                            size_t *stride, const uint8_t **motion,
                                            size_t *motion_size)
{
    const uint32_t slot = stage->flight_head;
    hdrg_status_t status;
    uint32_t index;

    *motion = NULL;
    *motion_size = 0;

    status = hdrg_stage_land(stage, exec, rgb, stride, &index, host);
    if (status != HDRG_OK || stage->sides == NULL) {
        return status;
    }

    *motion = (stage->sides[slot].size != 0) ? stage->sides[slot].data : NULL;
    *motion_size = stage->sides[slot].size;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_stage_rows(hdrg_stage_t *scale, const hdrg_exec_t *exec, uint32_t y0,
                                     uint32_t rows, uint16_t *dst, hdrg_plugin_host_t *host)
{
    const size_t tight = (size_t)scale->out_width * 3u * sizeof(uint16_t);
    hdrg_status_t status;
    uint32_t r;

    /* passed through: the source's rows */
    if (scale->passthrough) {
        hdrg_stage_source(exec, scale->src, y0, y0 + rows, (uint8_t *)dst, tight);

        return HDRG_OK;
    }

    if (!scale->configured) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (scale->fitting) {
        return hdrg_stage_fit_rows(scale, exec, y0, rows, dst, host);
    }

    if ((scale->cls->flags & HDRG_FILTER_BANDS) == 0 && !scale->done) {
        status = hdrg_stage_frame(scale, exec, host);
        if (status != HDRG_OK) {
            return status;
        }
    }

    if (!scale->done) {
        return hdrg_stage_band(scale, exec, y0, rows, dst, host);
    }

    /* the frame's output: as RGB16 already, or converted from the output format */
    if (scale->done_rgb != NULL) {
        for (r = 0; r < rows; r++) {
            memcpy((uint8_t *)dst + (size_t)r * tight,
                   (const uint8_t *)scale->done_rgb + (size_t)(y0 + r) * scale->done_stride, tight);
        }

        return HDRG_OK;
    }

    hdrg_format_unpack(exec, &scale->out_layout, &scale->filter.yuv, &scale->output.view, y0, rows,
                       dst, tight, NULL);

    return HDRG_OK;
}

extern void hdrg_stage_close(hdrg_stage_t *scale)
{
    uint32_t i;

    /* the jobs still in flight end first (the filter may still write their frames): soon */
    for (i = 0; scale->cls != NULL && scale->cls->cancel != NULL && i < scale->flight_count; i++) {
        if (scale->flights[(scale->flight_head + i) % scale->max_inflight].submitted) {
            scale->cls->cancel(scale->inst, hdrg_class_api(scale->cls));

            break;
        }
    }

    for (; scale->flight_count > 0; scale->flight_count--) {
        if (scale->flights[scale->flight_head].submitted) {
            (void)hdrg_stage_wait(&scale->flights[scale->flight_head].end);
            scale->flights[scale->flight_head].submitted = false;
        }

        scale->flight_head = (scale->flight_head + 1u) % scale->max_inflight;
    }

    /* the filter's own frames go back before the instance does */
    if (scale->created) {
        hdrg_stage_device_release(scale);
    }

    if (scale->alloc != NULL) {
        for (i = 0; i < scale->flights_ready; i++) {
            hdrg_stage_whole_free(scale, &scale->flights[i].output);
        }

        for (i = 0; scale->ring != NULL && i < scale->rings_ready; i++) {
            hdrg_stage_whole_free(scale, &scale->ring[i]);
        }

        hdrg_stage_whole_free(scale, &scale->input);
        hdrg_stage_whole_free(scale, &scale->output);
    }

    if (scale->created) {
        scale->cls->destroy(scale->inst, hdrg_class_api(scale->cls));
    }

    if (scale->fit != NULL) {
        hdrg_stage_close(scale->fit);
        hdrg_mem_free(scale->alloc, scale->fit);
        hdrg_options_destroy(scale->fit_options);
        scale->fit = NULL;
        scale->fit_options = NULL;
    }

    for (i = 0; i < scale->flights_ready; i++) {
        hdrg_cond_destroy(&scale->flights[i].end.cond);
        hdrg_mutex_destroy(&scale->flights[i].end.mutex);
    }

    if (scale->alloc != NULL) {
        hdrg_mem_free(scale->alloc, scale->flights);
        hdrg_mem_free(scale->alloc, scale->ring);
        hdrg_mem_free(scale->alloc, scale->device_ring);
        hdrg_mem_free(scale->alloc, scale->ring_pts);
        hdrg_mem_free(scale->alloc, scale->ring_index);
        hdrg_mem_free(scale->alloc, scale->ring_cut);
        hdrg_mem_free(scale->alloc, scale->results);

        for (i = 0; i < scale->scratch_count; i++) {
            hdrg_mem_aligned_free(scale->alloc, scale->scratches[i].data);
        }

        hdrg_mem_free(scale->alloc, scale->scratches);
        hdrg_mem_free(scale->alloc, scale->asked);
        hdrg_buffer_free(scale->alloc, &scale->in_rows);
        hdrg_buffer_free(scale->alloc, &scale->in16);
        hdrg_buffer_free(scale->alloc, &scale->out_rows);
        hdrg_buffer_free(scale->alloc, &scale->frame16);
        hdrg_buffer_free(scale->alloc, &scale->taken16);
        hdrg_buffer_free(scale->alloc, &scale->scratch);
        hdrg_mem_free(scale->alloc, scale->side.data);

        for (i = 0; scale->sides != NULL && i < scale->max_inflight; i++) {
            hdrg_mem_free(scale->alloc, scale->sides[i].data);
        }
    }

    if (scale->side_ready) {
        hdrg_mutex_destroy(&scale->side.lock);
    }

    for (i = 0; i < scale->sides_ready; i++) {
        hdrg_mutex_destroy(&scale->sides[i].lock);
    }

    if (scale->alloc != NULL) {
        hdrg_mem_free(scale->alloc, scale->sides);
        hdrg_mem_free(scale->alloc, scale->phase_store);
    }

    scale->sides = NULL;
    scale->phase_store = NULL;
    scale->sides_ready = 0;

    scale->side.data = NULL;
    scale->side.size = 0;
    scale->side.cap = 0;
    scale->side_ready = false;

    scale->created = false;
    scale->configured = false;
    scale->done = false;
    scale->flights = NULL;
    scale->ring = NULL;
    scale->device_ring = NULL;
    scale->ring_pts = NULL;
    scale->ring_index = NULL;
    scale->ring_cut = NULL;
    scale->results = NULL;
    scale->scratches = NULL;
    scale->asked = NULL;
    scale->scratch_want = 0;
    scale->scratch_count = 0;
    scale->current = NULL;
    scale->landed = NULL;
    scale->done_rgb = NULL;
    scale->flights_ready = 0;
    scale->rings_ready = 0;
    scale->flight_head = 0;
}
