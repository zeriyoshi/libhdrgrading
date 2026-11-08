/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

/* resident RESYNTH: the grid's windows of the frame at slot into the grain's (1: after) */
static hdrg_status_t hdrg_session_grain_windows(hdrg_session_t *session, uint8_t slot,
                                                uint8_t after)
{
    const size_t windows = hdrg_grain_windows(&session->grid);

    if (session->grain_windows == NULL) {
        return HDRG_OK;
    }

    return hdrg_gpu_gather(session->gpu, &session->pipeline.exec, slot, session->pipeline.src.width,
                           session->pipeline.src.height, 0, 0, 0, NULL, &session->grid,
                           session->grain_windows + after * windows);
}

/*
 * resident: the grain the chain removed from frame index now at slot (its
 * windows into the first time window, else the input's gathered already;
 * RESYNTH), or the one its decoder left out, into the pipeline's grain_stats
 */
static hdrg_status_t hdrg_session_resident_grain(hdrg_session_t *session, uint8_t slot,
                                                 uint32_t index)
{
    const size_t windows = hdrg_grain_windows(&session->grid);
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;

    /* the grain its decoder left out, given: in place of the one the chain removed */
    if (hdrg_pipeline_film_grain(pipeline, hdrg_pipeline_held_film(pipeline, index))) {
        return HDRG_OK;
    }

    /* (a time window that gave no frame of its own: none) */
    if (session->grain_windows == NULL ||
        (pipeline->device_before == UINT32_MAX && hdrg_pipeline_device_frames(pipeline) > 0)) {
        return HDRG_OK;
    }

    status = (pipeline->device_before != UINT32_MAX)
                 ? hdrg_session_grain_windows(
                       session,
                       (uint8_t)(HDRG_GC_FRAME0 + session->depth + 1u + pipeline->device_before), 0)
                 : HDRG_OK;

    if (status == HDRG_OK) {
        status = hdrg_session_grain_windows(session, slot, 1);
    }

    if (status != HDRG_OK) {
        return status;
    }

    return hdrg_grain_measure(pipeline->frame_alloc, &pipeline->exec, session->grain_windows,
                              session->grain_windows + windows, &session->grid,
                              pipeline->src.coeffs, &pipeline->grain_stats);
}

/*
 * resident: the chain's output of frame (in the ingest slot on the GPU)
 * analyzed and queued, its proxy (or itself, analyzed whole) and noise
 * windows read back; t: when the analysis began
 */
static hdrg_status_t hdrg_session_resident_queue(hdrg_session_t *session,
                                                 const hdrg_session_frame_t *frame, uint64_t t)
{
    const uint32_t width = session->pipeline.src.width, height = session->pipeline.src.height;
    const uint8_t slot = (uint8_t)(HDRG_GC_FRAME0 + session->ingest_next);
    const bool proxied = session->pipeline.proxy_shift > 0,
               windows = session->gridded && (proxied || session->pipeline.gpu_stats != NULL),
               whole = !proxied && !windows; /* the frame read back: its sums the CPU's */
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_session_item_t *item;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint16_t *gathered;

    gathered = (session->gathered != NULL)
                   ? session->gathered + (size_t)session->ingest_next * session->gathered_samples
                   : NULL;

    hdrg_fpenv_enter(&fpenv);
    status = hdrg_gpu_gather(
        session->gpu, &pipeline->exec, slot, width, height, pipeline->proxy_shift,
        proxied ? pipeline->proxy.width : width, proxied ? pipeline->proxy.height : height,
        (proxied || whole) ? gathered : NULL, windows ? &session->grid : NULL, session->mosaic);
    hdrg_fpenv_leave(&fpenv);

    if (status != HDRG_OK) {
        return status;
    }

    /*
     * the analysis on what was read back (gpu_analysis FAST, a frame analyzed whole: its sums on
     * the GPU, its noise windows read back, not the frame)
     */
    pipeline->given = proxied ? gathered : NULL;
    pipeline->mosaic = windows ? session->mosaic : NULL;
    pipeline->grid = session->grid;

    session->stats_slot = slot;
    status = hdrg_session_queue(session, frame, whole ? (const uint8_t *)(void *)gathered : NULL,
                                (size_t)width * 6u,
                                pipeline->prefiltering ? &pipeline->input_noise : NULL);
    session->stats_slot = HDRG_GC_SLOTS;
    (void)hdrg_session_part(session, HDRG_PART_ANALYZE, t);

    pipeline->given = NULL;
    pipeline->mosaic = NULL;

    if (status != HDRG_OK) {
        return status;
    }

    /* the frame is on the GPU from here */
    item = &session->queue[session->count - 1u];
    item->slot = slot;
    item->frame.release = NULL;
    session->ingest_next = (session->ingest_next + 1u) % (session->depth + 1u);

    return HDRG_OK;
}

/*
 * The picture the GPU sums for src (the pipeline's source or proxy): the
 * frame kept on the GPU now (its slot, the proxy sampled from it), else src's
 * rows uploaded
 */
static void hdrg_session_picture(const hdrg_session_t *session, const hdrg_source_t *src,
                                 hdrg_gpu_picture_t *picture)
{
    const hdrg_pipeline_t *pipeline = &session->pipeline;
    const bool proxy = src == &pipeline->proxy;

    memset(picture, 0, sizeof(*picture));
    picture->src = src;
    picture->width = src->width;
    picture->height = src->height;
    picture->src_width = src->width;
    picture->src_height = src->height;
    picture->slot = HDRG_GC_SLOTS;

    if (session->stats_slot < HDRG_GC_SLOTS) {
        picture->src = &pipeline->src;
        picture->slot = session->stats_slot;
        picture->src_width = pipeline->src.width;
        picture->src_height = pipeline->src.height;
        picture->stride = (uint32_t)(hdrg_gpu_convert_pitch(pipeline->src.width) / 4u);
        picture->step_shift = proxy ? pipeline->proxy_shift : 0u;
    }
}

/* frames on the GPU: the noise of the frame at slot from its windows read back */
static hdrg_status_t hdrg_session_resident_noise(hdrg_session_t *session, uint8_t slot,
                                                 hdrg_noise_t *out)
{
    const hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_noise_grid_t none;
    hdrg_source_t mosaic;
    hdrg_status_t status;

    memset(&none, 0, sizeof(none));
    mosaic = pipeline->input;

    if (!session->gridded) {
        return hdrg_noise_estimate_mosaic(pipeline->frame_alloc, &pipeline->exec, &mosaic, &none,
                                          out);
    }

    status = hdrg_gpu_gather(session->gpu, &pipeline->exec, slot, pipeline->src.width,
                             pipeline->src.height, 0, 0, 0, NULL, &session->grid, session->mosaic);
    if (status != HDRG_OK) {
        return status;
    }

    mosaic.pixels = (const uint8_t *)(const void *)session->mosaic;
    mosaic.stride = (size_t)session->grid.nx * HDRG_NOISE_WINDOW * 6u;
    mosaic.width = session->grid.nx * HDRG_NOISE_WINDOW;
    mosaic.height = session->grid.ny * HDRG_NOISE_WINDOW;
    mosaic.row0 = 0;

    return hdrg_noise_estimate_mosaic(pipeline->frame_alloc, &pipeline->exec, &mosaic,
                                      &session->grid, out);
}

extern void *hdrg_session_window_keep(void *arg, const void *frame, uint32_t k)
{
    const hdrg_session_t *session = (const hdrg_session_t *)arg;
    const uint8_t slot = (uint8_t)(HDRG_GC_FRAME0 + session->depth + 1u + k);
    const size_t size =
        hdrg_gpu_convert_pitch(session->pipeline.src.width) * (size_t)session->pipeline.src.height;

    if (!hdrg_gpu_core_buffer(session->gpu, slot, size, NULL) ||
        !hdrg_gpu_core_take(session->gpu, frame, slot, size)) {
        return NULL;
    }

    return hdrg_gpu_core_frame(session->gpu, slot);
}

extern hdrg_status_t hdrg_session_drain_resident(hdrg_session_t *session)
{
    const size_t size =
        hdrg_gpu_convert_pitch(session->pipeline.src.width) * (size_t)session->pipeline.src.height;
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_session_frame_t frame;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint64_t t;
    uint32_t index;
    bool kept;
    void *last;

    while (session->ended && pipeline->windowed && !session->drained &&
           session->count <= session->depth) {
        t = hdrg_clock_ns();

        hdrg_fpenv_enter(&fpenv);
        status = hdrg_pipeline_prefilter_device(session->ctx, pipeline, NULL, 0, &last, &index);

        /* the ingest slot (not converted into, maybe) made for it */
        if (status == HDRG_OK &&
            (!hdrg_gpu_core_buffer(session->gpu, (uint8_t)(HDRG_GC_FRAME0 + session->ingest_next),
                                   size, NULL) ||
             !hdrg_gpu_core_take(session->gpu, last,
                                 (uint8_t)(HDRG_GC_FRAME0 + session->ingest_next), size))) {
            status = HDRG_ERR_INTERNAL;
        }

        hdrg_pipeline_device_release(pipeline);

        if (status == HDRG_OK) {
            status = hdrg_session_resident_grain(
                session, (uint8_t)(HDRG_GC_FRAME0 + session->ingest_next), index);
        }

        hdrg_fpenv_leave(&fpenv);

        t = hdrg_session_part(session, HDRG_PART_PREFILTER, t);

        if (status == HDRG_EOF) {
            session->drained = true;

            break;
        }

        if (status != HDRG_OK) {
            return status;
        }

        memset(&frame, 0, sizeof(frame));
        frame.pts = pipeline->held_pts[index % HDRG_PIPELINE_HELD];
        frame.motion = hdrg_pipeline_chain_motion(pipeline, index, &frame.motion_size, &kept);
        status = kept ? hdrg_session_resident_queue(session, &frame, t) : HDRG_ERR_NOMEM;

        if (status != HDRG_OK) {
            return status;
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_gpu_sums(void *arg, const hdrg_source_t *src, uint8_t cell_shift,
                                           hdrg_analyze_given_t *given)
{
    hdrg_session_t *session = (hdrg_session_t *)arg;
    hdrg_gpu_picture_t picture;

    hdrg_session_picture(session, src, &picture);

    return hdrg_gpu_analyze_stats(session->gpu, &session->ctx->alloc, &session->pipeline.exec,
                                  &session->gpu_work, &picture, cell_shift, given);
}

extern hdrg_status_t hdrg_session_gpu_moments(void *arg, const hdrg_source_t *src,
                                              const hdrg_analysis_t *analysis, uint8_t light_shift,
                                              uint32_t cell, uint32_t grid_w, uint32_t grid_h,
                                              double *sums, float *grid)
{
    hdrg_session_t *session = (hdrg_session_t *)arg;
    hdrg_gpu_picture_t picture;

    hdrg_session_picture(session, src, &picture);

    return hdrg_gpu_analyze_moments(session->gpu, &session->pipeline.exec, &session->gpu_work,
                                    &picture, analysis, light_shift, cell, grid_w, grid_h, sums,
                                    grid);
}

extern hdrg_status_t hdrg_session_resident_buffers(hdrg_session_t *session)
{
    const hdrg_allocator_t *alloc = &session->ctx->alloc;
    const hdrg_pipeline_t *pipeline = &session->pipeline;
    const size_t frames = (size_t)session->depth + 1u;
    bool read;

    /*
     * what the CPU reads back of a frame: its proxy, or itself analyzed whole (none when the
     * GPU sums it and its noise windows come apart)
     */
    session->gridded = hdrg_noise_grid(pipeline->src.width, pipeline->src.height, &session->grid);
    read = pipeline->proxy_shift > 0 || !session->gridded || pipeline->gpu_stats == NULL;
    session->gathered_samples = !read ? 0u
                                : (pipeline->proxy_shift > 0)
                                    ? (size_t)pipeline->proxy.width * pipeline->proxy.height * 3u
                                    : (size_t)pipeline->src.width * pipeline->src.height * 3u;

    if (read) {
        if (session->gathered_samples > SIZE_MAX / sizeof(uint16_t) / frames) {
            return HDRG_ERR_NOMEM;
        }

        session->gathered = (uint16_t *)hdrg_mem_alloc(alloc, session->gathered_samples * frames *
                                                                  sizeof(uint16_t));
        if (session->gathered == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    if (session->gridded) {
        session->mosaic = (uint16_t *)hdrg_mem_alloc(
            alloc, (size_t)session->grid.nx * session->grid.ny * HDRG_NOISE_WINDOW *
                       HDRG_NOISE_WINDOW * 3u * sizeof(uint16_t));
        if (session->mosaic == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    /* RESYNTH: the grain's windows, before and after */
    if (session->gridded && pipeline->params.grain_policy == HDRG_GRAIN_RESYNTH) {
        session->grain_windows = (uint16_t *)hdrg_mem_alloc(
            alloc, 2u * hdrg_grain_windows(&session->grid) * sizeof(uint16_t));
        if (session->grain_windows == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_send_resident(hdrg_session_t *session,
                                                const hdrg_session_frame_t *in,
                                                const hdrg_yuv_t *yuv)
{
    const uint32_t width = session->pipeline.src.width, height = session->pipeline.src.height;
    const uint8_t slot = (uint8_t)(HDRG_GC_FRAME0 + session->ingest_next);
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_session_frame_t frame;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint64_t t;
    uint32_t index;
    bool kept;
    void *own, *last;

    frame = *in;

    hdrg_fpenv_enter(&fpenv);
    t = hdrg_clock_ns();
    status = hdrg_gpu_convert_frame(session->gpu, &pipeline->exec, yuv, NULL, 0, slot, in->device);
    t = hdrg_session_part(session, HDRG_PART_CONVERT, t);
    own = hdrg_gpu_core_frame(session->gpu, slot);

    /* RESYNTH without a time window: the grain's before is the input, gathered before the chain */
    if (status == HDRG_OK && pipeline->prefiltering && hdrg_pipeline_device_frames(pipeline) == 0) {
        status = hdrg_session_grain_windows(session, slot, 0);
    }

    /* the frame the chain gives may be an earlier one (time windows) */
    if (status == HDRG_OK && pipeline->prefiltering) {
        status = hdrg_session_resident_noise(session, slot, &pipeline->input_noise);

        if (status == HDRG_OK) {
            pipeline->in_motion = (const uint8_t *)in->motion;
            pipeline->in_motion_size = in->motion_size;
            pipeline->in_film = (const uint8_t *)in->film_grain;
            status =
                hdrg_pipeline_prefilter_device(session->ctx, pipeline, own, in->pts, &last, &index);
            pipeline->in_motion = NULL;
            pipeline->in_motion_size = 0;
            pipeline->in_film = NULL;
        }

        if (status == HDRG_OK) {
            frame.pts = pipeline->held_pts[index % HDRG_PIPELINE_HELD];
            frame.motion = hdrg_pipeline_chain_motion(pipeline, index, &frame.motion_size, &kept);
            status = kept ? HDRG_OK : HDRG_ERR_NOMEM;
        }

        if (status == HDRG_OK && last != own &&
            !hdrg_gpu_core_take(session->gpu, last, slot, hdrg_gpu_convert_pitch(width) * height)) {
            status = HDRG_ERR_INTERNAL;
        }

        hdrg_pipeline_device_release(pipeline);

        /* the grain the chain removed from the frame it gave (or its decoder left out) */
        if (status == HDRG_OK) {
            status = hdrg_session_resident_grain(session, slot, index);
        }

        t = hdrg_session_part(session, HDRG_PART_PREFILTER, t);
    } else {
        (void)hdrg_pipeline_film_grain(pipeline, (const uint8_t *)in->film_grain);
    }

    hdrg_fpenv_leave(&fpenv);

    /* kept for the windows (the frame is on the GPU): a frame comes out with a later one */
    if (status == HDRG_ERR_AGAIN) {
        session->taken++;
        hdrg_session_release(in);

        return HDRG_OK;
    }

    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_session_resident_queue(session, &frame, t);
    if (status != HDRG_OK) {
        return status;
    }

    session->taken++;
    hdrg_session_release(in);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_device_frame(const hdrg_session_t *session,
                                               const hdrg_device_frame_t *frame,
                                               const hdrg_session_pixfmt_t *pixfmt)
{
#if defined(HDRG_WITH_CVPIXELBUFFER)
    hdrg_apple_surface_t surface;
#endif

    if (frame->kind == HDRG_FRAME_BUFFERS) {
        return (session->ctx->host.devices[session->config.device - 1u][0] != NULL)
                   ? HDRG_OK
                   : HDRG_ERR_INVALID_ARG;
    }

    if (frame->kind == HDRG_FRAME_DMABUF) {
        return (session->config.device == HDRG_DEVICE_VULKAN) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
    }

    /* an AHardwareBuffer of a Vulkan session: the two-plane 4:2:0 of 8 bits or of 10 in 16 */
    if (frame->kind == HDRG_FRAME_AHARDWAREBUFFER) {
        if (session->config.device != HDRG_DEVICE_VULKAN) {
            return HDRG_ERR_UNSUPPORTED;
        }

        return (pixfmt->layout == HDRG_YUV_420 && pixfmt->interleaved &&
                ((pixfmt->bits == 8u && !pixfmt->msb) || (pixfmt->bits == 10u && pixfmt->msb)))
                   ? HDRG_OK
                   : HDRG_ERR_INVALID_ARG;
    }

#if defined(HDRG_WITH_CVPIXELBUFFER)
    if (frame->kind == HDRG_FRAME_CVPIXELBUFFER) {
        if (!hdrg_apple_surface(frame->handles[0], &surface) ||
            surface.pixfmt.layout != pixfmt->layout || surface.pixfmt.bits != pixfmt->bits ||
            surface.pixfmt.interleaved != pixfmt->interleaved ||
            surface.pixfmt.msb != pixfmt->msb || surface.pixfmt.full_range != pixfmt->full_range) {
            return HDRG_ERR_INVALID_ARG;
        }

        return HDRG_OK;
    }
#endif

    return (frame->kind <= HDRG_FRAME_AHARDWAREBUFFER) ? HDRG_ERR_UNSUPPORTED
                                                       : HDRG_ERR_INVALID_ARG;
}
