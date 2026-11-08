/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/gpu/filter_gk.h"

#define HDRG_GK_ROW_ALIGN 256u /* a frame row's bytes: the host's own frames' */
#define HDRG_GK_ID_MAX    64u  /* a class's id's bytes */

/* a committed job: what its end needs */
typedef struct {
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    hdrg_gpu_share_frame_t *in_frame, *out_frame;
} hdrg_gk_work_t;

/* a job's pass through: its output on the device marked, or the job ended */
static void hdrg_gk_job_end(void *arg, bool ok)
{
    const hdrg_gk_work_t *work = (const hdrg_gk_work_t *)arg;
    const hdrg_plugin_api_t *api = work->api;
    hdrg_gpu_share_frame_t *in = work->in_frame, *out = work->out_frame;
    hdrg_job_t *job = work->job;

    /* freed before the end is told: the host may tear the context down once it is */
    api->mem_free(api, arg);
    ok = ok && (in == NULL || hdrg_atomic_u32_load_acquire(&in->failed) == 0);

    if (out != NULL) {
        hdrg_atomic_u32_store_release(&out->failed, ok ? 0u : 1u);
        hdrg_gpu_share_drop(in, out);
    } else {
        hdrg_gpu_share_drop(in, NULL);
        (void)api->job_done(job, ok ? HDRG_OK : HDRG_ERR_INTERNAL);
    }
}

extern hdrg_class_t *hdrg_gk_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                   const hdrg_gk_backend_t *backend, const char *name)
{
    const size_t prefix = strlen(backend->prefix), len = strlen(name);
    hdrg_class_t *cls;
    char id[HDRG_GK_ID_MAX];

    if (prefix + 1u + len > sizeof(id)) {
        return NULL;
    }

    memcpy(id, backend->prefix, prefix);
    id[prefix] = '.';
    memcpy(id + prefix + 1u, name, len);

    /* the class's instances find their backend (hdrg_gk_plugin_device) */
    cls = api->class_create(plugin, id, prefix + 1u + len);
    if (cls != NULL && api->class_set_user(cls, (void *)(uintptr_t)backend) != HDRG_OK) {
        return NULL;
    }

    return cls;
}

extern hdrg_status_t hdrg_gk_frame_alloc(const hdrg_gk_t *gk, uint32_t format, uint32_t width,
                                         uint32_t height, uint8_t **planes, size_t *strides,
                                         void **handle)
{
    hdrg_gk_buffer_t *buffer;
    uint64_t stride, bytes;
    uint32_t pixel;

    if (format != HDRG_FORMAT_RGB16 && format != HDRG_FORMAT_RGBA16) {
        return HDRG_ERR_UNSUPPORTED;
    }

    pixel = (format == HDRG_FORMAT_RGB16) ? 6u : 8u;
    stride =
        ((uint64_t)width * pixel + HDRG_GK_ROW_ALIGN - 1u) / HDRG_GK_ROW_ALIGN * HDRG_GK_ROW_ALIGN;
    bytes = stride * height;

    if (width == 0 || height == 0 || bytes > (uint64_t)SIZE_MAX) {
        return HDRG_ERR_UNSUPPORTED;
    }

    buffer = hdrg_gk_host_buffer(gk, (size_t)bytes, &planes[0]);
    if (buffer == NULL) {
        return HDRG_ERR_NOMEM;
    }

    strides[0] = (size_t)stride;
    *handle = buffer;

    return HDRG_OK;
}

extern void hdrg_gk_frame_free(const hdrg_gk_t *gk, void *handle)
{
    hdrg_gk_buffer_free(gk, (hdrg_gk_buffer_t *)handle);
}

extern hdrg_status_t hdrg_gk_job_frames(const hdrg_gk_t *gk, hdrg_gk_device_t *device,
                                        hdrg_job_t *job, const hdrg_plugin_api_t *api,
                                        uint32_t width, uint32_t height, uint32_t pixel,
                                        hdrg_gk_job_t *out)
{
    const size_t pitch =
        ((size_t)width * pixel + HDRG_GK_ROW_ALIGN - 1u) / HDRG_GK_ROW_ALIGN * HDRG_GK_ROW_ALIGN;
    hdrg_gk_frame_t *frame;
    hdrg_status_t status;
    uint64_t ready;
    uint32_t begin, end, row_begin, row_end, k;
    uint8_t wanted;
    void *in_device, *handle;

    memset(out, 0, sizeof(*out));

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_device(job, &in_device, &ready, &wanted);
    }

    if (status != HDRG_OK || begin != 0u || end != height) {
        return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
    }

    /* the input: the previous filter's device frame (its buffer wrapped), or the host's */
    if (in_device != NULL) {
        out->in_frame = (hdrg_gpu_share_frame_t *)in_device;
        out->in = hdrg_gk_wrap(gk, &device->read, out->in_frame);
        out->in_stride = pitch;

        if (out->in == NULL) {
            return HDRG_ERR_INTERNAL;
        }
    } else {
        status = api->job_input(job, 0, &out->in_host, &out->in_stride, &row_begin, &row_end);
        if (status == HDRG_OK) {
            status = api->job_input_handle(job, 0, &handle);
        }

        if (status != HDRG_OK || row_begin != 0u || row_end != height || handle == NULL) {
            return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
        }

        out->in = (hdrg_gk_buffer_t *)handle;
    }

    /* the output: a device frame of the filter's own for the next filter, or the host's */
    if (wanted != 0u) {
        for (k = 0; k < HDRG_GK_DEVICE_FRAMES && device->frames[k].share.busy; k++) {
        }

        if (k == HDRG_GK_DEVICE_FRAMES) {
            return HDRG_ERR_INTERNAL;
        }

        frame = &device->frames[k];

        if (frame->buffer == NULL || frame->share.size < pitch * height) {
            hdrg_gk_buffer_free(gk, frame->buffer);
            hdrg_gk_frame_set(gk, frame, hdrg_gk_buffer(gk, pitch * height));
        }

        if (frame->buffer == NULL) {
            return HDRG_ERR_NOMEM;
        }

        status = api->job_set_output_device(job, &frame->share, 0);
        if (status != HDRG_OK) {
            return status;
        }

        frame->share.busy = 1u;
        hdrg_atomic_u32_store_relaxed(&frame->share.failed, 0u);
        out->out_frame = frame;
        out->out = frame->buffer;
        out->out_stride = pitch;
    } else {
        status = api->job_output(job, 0, &out->out_host, &out->out_stride);
        if (status == HDRG_OK) {
            status = api->job_output_handle(job, &handle);
        }

        if (status != HDRG_OK || handle == NULL) {
            return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
        }

        out->out = (hdrg_gk_buffer_t *)handle;
    }

    return (out->in_stride % 4u == 0u && out->out_stride % 4u == 0u) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern bool hdrg_gk_job_commit(const hdrg_gk_t *gk, hdrg_gk_pass_t *pass, hdrg_job_t *job,
                               const hdrg_plugin_api_t *api, const hdrg_gk_job_t *frames)
{
    hdrg_gk_work_t *work;
    bool device;

    work = (hdrg_gk_work_t *)api->mem_alloc(api, sizeof(*work));
    if (work == NULL) {
        (void)hdrg_gk_commit(gk, pass, NULL, NULL);

        return false;
    }

    work->job = job;
    work->api = api;
    work->in_frame = frames->in_frame;
    work->out_frame = (frames->out_frame != NULL) ? &frames->out_frame->share : NULL;
    device = frames->out_frame != NULL;

    hdrg_gpu_share_hold(work->in_frame, work->out_frame, 1u);

    if (!hdrg_gk_commit(gk, pass, hdrg_gk_job_end, work)) {
        hdrg_gpu_share_drop(work->in_frame, work->out_frame);
        api->mem_free(api, work);

        return false;
    }

    /* the next filter's passes follow on the queue: the job is through for the host */
    if (device) {
        (void)api->job_done(job, HDRG_OK);
    }

    return true;
}

extern hdrg_status_t hdrg_gk_job_pass(hdrg_job_t *job, const hdrg_plugin_api_t *api,
                                      const hdrg_gk_job_t *frames, uint32_t width, uint32_t height,
                                      uint32_t pixel)
{
    uint32_t y;

    if (frames->in_host == NULL || frames->out_host == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    for (y = 0; y < height; y++) {
        memcpy(frames->out_host + (size_t)y * frames->out_stride,
               frames->in_host + (size_t)y * frames->in_stride, (size_t)width * pixel);
    }

    return api->job_done(job, HDRG_OK);
}

extern void hdrg_gk_device_frames_free(const hdrg_gk_t *gk, hdrg_gk_device_t *device)
{
    uint32_t k;

    for (k = 0; k < HDRG_GK_DEVICE_FRAMES; k++) {
        hdrg_gpu_share_wait(&device->frames[k].share);
        hdrg_gk_buffer_free(gk, device->frames[k].buffer);
        device->frames[k].buffer = NULL;
        device->frames[k].share.buffer = 0u;
        device->frames[k].share.size = 0;
        device->frames[k].share.busy = 0u;
    }

    hdrg_gk_wraps_free(gk, &device->read);
}

extern void HDRG_PLUGIN_CALL hdrg_gk_device_release(void *inst, void *frame,
                                                    const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;

    /* its completion's writes done before the frame is given again */
    hdrg_gpu_share_wait((hdrg_gpu_share_frame_t *)frame);
    ((hdrg_gpu_share_frame_t *)frame)->busy = 0u;
}
