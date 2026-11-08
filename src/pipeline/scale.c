/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "parts.h"

extern hdrg_status_t hdrg_pipeline_output_size(const hdrg_encode_params_t *encode, uint32_t width,
                                               uint32_t height, uint32_t *out_width,
                                               uint32_t *out_height)
{
    uint64_t side;

    *out_width = encode->output_width;
    *out_height = encode->output_height;

    if (*out_width == 0 && *out_height == 0) {
        *out_width = width;
        *out_height = height;

        return HDRG_OK;
    }

    if (*out_width == 0) {
        side = ((uint64_t)width * *out_height + height / 2u) / height;
        *out_width = (uint32_t)HDRG_MAX(side, 1u);
    } else if (*out_height == 0) {
        side = ((uint64_t)height * *out_width + width / 2u) / width;
        *out_height = (uint32_t)HDRG_MAX(side, 1u);
    } else {
        side = 0;
    }

    return (side <= HDRG_MAX_DIMENSION && *out_width <= HDRG_MAX_DIMENSION &&
            *out_height <= HDRG_MAX_DIMENSION)
               ? HDRG_OK
               : HDRG_ERR_INVALID_ARG;
}

extern hdrg_status_t hdrg_pipeline_scale_setup(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    const hdrg_filter_entry_t *scaler;
    hdrg_status_t status;
    uint8_t upscaler, at;
    bool cpu;

    /* the built-in scaler asked for: UNSUPPORTED where the build left it out */
    scaler = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_SCALE);
    status = HDRG_ERR_UNSUPPORTED;
    upscaler = (pipeline->width >= pipeline->src.width &&
                pipeline->height >= pipeline->src.height && pipeline->params.upscaler < 4u)
                   ? pipeline->params.upscaler
                   : HDRG_UPSCALER_LANCZOS;

    /* NIS scales by 1 to 2 alone: past that, Lanczos */
    if (upscaler == HDRG_UPSCALER_NIS &&
        ((uint64_t)pipeline->width > 2u * (uint64_t)pipeline->src.width ||
         (uint64_t)pipeline->height > 2u * (uint64_t)pipeline->src.height)) {
        upscaler = HDRG_UPSCALER_LANCZOS;
    }

    /* the built-in scaler: a GPU version on a GPU context, the CPU's without a device */
    at = 0;

    while (scaler == NULL) {
        pipeline->scaler.cls =
            hdrg_default_class(&ctx->host, (uint8_t)(HDRG_DEFAULT_LANCZOS + upscaler),
                               ctx->gpu != HDRG_GPU_OFF, pipeline->gpu_first, &at, &cpu);
        if (pipeline->scaler.cls == NULL) {
            break;
        }

        hdrg_options_destroy(pipeline->default_options);
        pipeline->default_options = NULL;

        status = hdrg_host_resolve_options(&ctx->host, pipeline->scaler.cls, NULL,
                                           &pipeline->default_options);
        if (status != HDRG_OK) {
            return status;
        }

        pipeline->scaler.options = pipeline->default_options;
        pipeline->scaler.stage = HDRG_STAGE_SCALE;

        status = hdrg_stage_create(alloc, &pipeline->scaler, HDRG_APPLY_BAND_ROWS, &ctx->host,
                                   &pipeline->scale);
        if (status != HDRG_ERR_UNSUPPORTED || cpu) {
            break;
        }
    }

    if (scaler != NULL) {
        pipeline->scaler = *scaler;
        status = hdrg_stage_create(alloc, &pipeline->scaler, HDRG_APPLY_BAND_ROWS, &ctx->host,
                                   &pipeline->scale);
    }

    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_source_init_rgb16(alloc, &pipeline->color, pipeline->width, pipeline->height,
                                    &pipeline->scaled);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->scaled_band =
        (uint16_t *)hdrg_mem_alloc(alloc, pipeline->scaled.stride * HDRG_APPLY_BAND_ROWS);
    if (pipeline->scaled_band == NULL) {
        return HDRG_ERR_NOMEM;
    }

    pipeline->scaled.pixels = (const uint8_t *)pipeline->scaled_band;

    /* a session's SCALE with a time window or frames in flight keeps its input frames */
    if (!pipeline->streaming || (pipeline->scale.past + pipeline->scale.future == 0 &&
                                 !hdrg_pipeline_flying(&pipeline->scale))) {
        return HDRG_OK;
    }

    return hdrg_source_init_rgb16(alloc, &pipeline->color, pipeline->src.width,
                                  pipeline->src.height, &pipeline->scale_in);
}

extern hdrg_status_t hdrg_pipeline_scale_next(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                              bool use)
{
    const uint16_t *frame;
    hdrg_stage_t *scale = &pipeline->scale;
    hdrg_status_t status;
    uint32_t index;
    size_t stride;

    if (hdrg_pipeline_flying(scale)) {
        status = hdrg_pipeline_scale_feed(ctx, pipeline, NULL, 0, false);
        if (status == HDRG_OK) {
            status = hdrg_stage_land(scale, &pipeline->exec, &frame, &stride, &index, &ctx->host);
        }

        if (status == HDRG_OK && use) {
            hdrg_stage_serve(scale, frame, stride);
        }

        return status;
    }

    if (!hdrg_stage_take(scale, pipeline->origin, pipeline->ended)) {
        return HDRG_ERR_INTERNAL;
    }

    /* a SEQUENTIAL filter's jobs run even for frames not shown */
    if (!use && (scale->cls->flags & HDRG_FILTER_SEQUENTIAL) == 0) {
        return HDRG_OK;
    }

    status = hdrg_stage_taken(scale, &pipeline->exec, &frame, &stride);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->scale_in.pixels = (const uint8_t *)(const void *)frame;
    pipeline->scale_in.stride = stride;
    pipeline->told.noise = NULL;
    pipeline->told.motion = NULL;
    pipeline->told.motion_size = 0;
    pipeline->told.pts = pipeline->frame_pts;
    pipeline->told.index = pipeline->frame_index;
    pipeline->told.first = pipeline->frame_index == pipeline->origin;

    status = hdrg_stage_configure(scale, &pipeline->scale_in, pipeline->width, pipeline->height,
                                  &pipeline->told, &ctx->host);
    if (status != HDRG_OK || use) {
        return status;
    }

    return hdrg_stage_settle(scale, &pipeline->exec, &ctx->host);
}

extern hdrg_status_t hdrg_pipeline_scale_feed(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                              const uint8_t *pixels, int64_t pts, bool cut)
{
    hdrg_stage_t *scale = &pipeline->scale;
    hdrg_source_t frame;
    hdrg_status_t status;

    if (!pipeline->scale_keeps) {
        return HDRG_OK;
    }

    if (pixels != NULL) {
        frame = pipeline->src;
        frame.pixels = pixels;

        status = hdrg_stage_push(scale, &pipeline->exec, &frame, pts, NULL);
        if (status != HDRG_OK) {
            return status;
        }

        hdrg_stage_cut(scale, cut);
    }

    while (hdrg_pipeline_flying(scale) && scale->flight_count < scale->max_inflight &&
           hdrg_stage_take(scale, pipeline->origin, pipeline->ended)) {
        pipeline->scale_in.pixels = NULL;
        pipeline->told.noise = NULL;
        pipeline->told.motion = NULL;
        pipeline->told.motion_size = 0;
        pipeline->told.pts = scale->taken_pts;
        pipeline->told.index = pipeline->origin + scale->taken - 1u;

        status = hdrg_stage_configure(scale, &pipeline->scale_in, pipeline->width, pipeline->height,
                                      &pipeline->told, &ctx->host);
        if (status == HDRG_OK) {
            status = hdrg_stage_submit(scale, scale->taken - 1u, &ctx->host);
        }

        if (status != HDRG_OK) {
            return status;
        }
    }

    return HDRG_OK;
}

extern void hdrg_pipeline_scale_depth(hdrg_pipeline_t *pipeline, uint32_t depth)
{
    if (pipeline->scale_keeps) {
        hdrg_stage_reserve(&pipeline->scale, pipeline->delay.scale_past + depth + 1u);
    }
}

extern hdrg_status_t hdrg_pipeline_scale_skip(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    return (pipeline->scale_keeps && !pipeline->scale_apart)
               ? hdrg_pipeline_scale_next(ctx, pipeline, false)
               : HDRG_OK;
}

extern hdrg_status_t hdrg_pipeline_scale_frame(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                               const uint8_t *pixels, size_t stride, uint16_t *out)
{
    hdrg_source_t frame;
    hdrg_status_t status;
    uint32_t y0, rows;

    if (pipeline->scale_keeps) {
        status = hdrg_pipeline_scale_next(ctx, pipeline, out != NULL);
    } else if (out == NULL) {
        return HDRG_OK;
    } else {
        frame = pipeline->src;
        frame.pixels = pixels;
        frame.stride = stride;
        frame.row0 = 0;
        pipeline->told.noise = NULL;
        pipeline->told.motion = NULL;
        pipeline->told.motion_size = 0;
        pipeline->told.pts = pipeline->frame_pts;
        pipeline->told.index = pipeline->frame_index;
        pipeline->told.first = pipeline->frame_index == pipeline->origin;

        status = hdrg_stage_configure(&pipeline->scale, &frame, pipeline->width, pipeline->height,
                                      &pipeline->told, &ctx->host);
    }

    for (y0 = 0; status == HDRG_OK && out != NULL && y0 < pipeline->height;
         y0 += HDRG_APPLY_BAND_ROWS) {
        rows = HDRG_MIN(HDRG_APPLY_BAND_ROWS, pipeline->height - y0);
        status = hdrg_stage_rows(&pipeline->scale, &pipeline->exec, y0, rows,
                                 out + (size_t)y0 * pipeline->width * 3u, &ctx->host);
    }

    return status;
}
