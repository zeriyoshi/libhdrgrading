/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "parts.h"

/* the frames past the first an ASYNC filter declaring its depth keeps in flight, at most most */
static inline uint32_t hdrg_pipeline_flights(const hdrg_stage_t *stage, uint32_t most)
{
    const uint32_t depth = HDRG_MIN(stage->filter.max_inflight, most);

    return ((stage->cls->flags & HDRG_FILTER_ASYNC) != 0 && depth > 1u) ? depth - 1u : 0u;
}

/*
 * Rows of the proxy in 16-bit code values: the center pixel of each 2^shift
 * square of source pixels (clamped at the edges). Not their mean: a mean
 * smooths the noise and the sharp steps text / UI is told apart by, and
 * dims small highlights.
 */
static void hdrg_proxy_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_proxy_job_t *job = (const hdrg_proxy_job_t *)arg;
    const hdrg_source_t *src = job->src;
    const uint32_t half = (1u << job->shift) / 2u, scale = src->wide ? 1u : 257u;
    const uint8_t *row;
    uint32_t j, i, codes[3];
    uint16_t *out;
    uint8_t c;

    for (j = row_begin; j < row_end; j++) {
        out = job->dst + (size_t)j * job->width * 3u;
        row = hdrg_source_row(src, HDRG_MIN((j << job->shift) + half, src->height - 1u));

        for (i = 0; i < job->width; i++) {
            hdrg_source_codes(src, row, HDRG_MIN((i << job->shift) + half, src->width - 1u), codes);

            for (c = 0; c < 3u; c++) {
                out[(size_t)i * 3u + c] = (uint16_t)(codes[c] * scale);
            }
        }
    }
}

/* the proxy of the frame at pipeline->src.pixels (a session's kept on the GPU: given) */
static void hdrg_pipeline_reduce(hdrg_pipeline_t *pipeline)
{
    hdrg_proxy_job_t job;

    if (pipeline->given != NULL) {
        memcpy(pipeline->proxy_pixels, pipeline->given,
               (size_t)pipeline->proxy.width * pipeline->proxy.height * 3u * sizeof(uint16_t));

        return;
    }

    job.src = &pipeline->src;
    job.dst = pipeline->proxy_pixels;
    job.width = pipeline->proxy.width;
    job.shift = pipeline->proxy_shift;

    hdrg_exec_rows(&pipeline->exec, pipeline->proxy.height, hdrg_proxy_rows, &job);
}

/*
 * the ANALYZE stage (the params' light_predictor > 0): the app's filter of it, else the built-in
 * hdrg.analyze.predict (HDRG_ERR_UNSUPPORTED where the build left it out)
 */
static hdrg_status_t hdrg_pipeline_analyzer_setup(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    const hdrg_filter_entry_t *added;
    hdrg_filter_entry_t entry;
    hdrg_status_t status;

    added = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_ANALYZE);
    if (added != NULL) {
        return hdrg_analyzer_open(added, &ctx->host, &pipeline->analyzer);
    }

    memset(&entry, 0, sizeof(entry));
    entry.cls = hdrg_default_find(&ctx->host, HDRG_DEFAULT_PREDICT);
    entry.stage = HDRG_STAGE_ANALYZE;

    if (entry.cls == NULL) {
        hdrg_host_set_error(&ctx->host, "the built-in filter is not in this build",
                            hdrg_defaults[HDRG_DEFAULT_PREDICT].id,
                            strlen(hdrg_defaults[HDRG_DEFAULT_PREDICT].id));

        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_host_resolve_options(&ctx->host, entry.cls, NULL, &pipeline->analyzer_options);
    if (status != HDRG_OK) {
        return status;
    }

    entry.options = pipeline->analyzer_options;

    return hdrg_analyzer_open(&entry, &ctx->host, &pipeline->analyzer);
}

extern void hdrg_pipeline_copy_rows(uint32_t row_begin, uint32_t row_end, void *arg)
{
    const hdrg_copy_job_t *job = (const hdrg_copy_job_t *)arg;
    uint32_t r;

    for (r = row_begin; r < row_end; r++) {
        memcpy((uint8_t *)job->to + (size_t)r * job->row, job->from + (size_t)r * job->stride,
               job->row);
    }
}

extern void hdrg_pipeline_delays(hdrg_pipeline_t *pipeline)
{
    const hdrg_stage_t *scale = &pipeline->scale;

    hdrg_pipeline_delay(pipeline, pipeline->flight_cap, &pipeline->delay);

    pipeline->windowed = pipeline->delay.chain + pipeline->delay.chain_past > 0;
    pipeline->scale_keeps = pipeline->streaming && pipeline->scaling &&
                            (scale->past + scale->future > 0 || hdrg_pipeline_flying(scale));
}

extern hdrg_status_t hdrg_pipeline_open(hdrg_context_t *ctx, const hdrg_raw_image_t *raw,
                                        const hdrg_color_t *color, const hdrg_params_t *params,
                                        const hdrg_encode_params_t *encode,
                                        const hdrg_display_caps_t *caps, uint32_t gpu,
                                        uint8_t latency, hdrg_pipeline_t *out)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    const hdrg_source_t *frame;
    hdrg_gamut_map_t map;
    hdrg_status_t status;
    uint32_t gm_h;

    memset(out, 0, sizeof(*out));
    out->streaming = raw->pixels == NULL;
    out->latency = latency;
    out->gpu_first = gpu;
    out->flight_cap = UINT32_MAX;

    out->frame_alloc = &ctx->alloc;
    out->params = *params;

    /* a policy not resolved by its caller (a still's or a session's): PRESERVE */
    if (out->params.grain_policy == HDRG_GRAIN_AUTO) {
        out->params.grain_policy = HDRG_GRAIN_PRESERVE;
    }

    params = &out->params;
    out->encode = *encode;
    out->caps = *caps;
    hdrg_context_exec(ctx, &out->exec);

    status = hdrg_pipeline_output_size(encode, raw->width, raw->height, &out->width, &out->height);
    if (status == HDRG_OK && ((uint64_t)raw->width * raw->height > ctx->max_pixels ||
                              (uint64_t)out->width * out->height > ctx->max_pixels)) {
        status = HDRG_ERR_LIMIT;
    }

    if (status == HDRG_OK) {
        status = hdrg_color_copy(alloc, color, &out->color);
    }

    /* the filters are told of the code values as they came (before sdr_eotf) */
    hdrg_color_describe(color, out->xy, &out->told.transfer);
    out->told.xy = out->xy;
    out->told.bits = (raw->bit_depth == 16u) ? 16u : 8u;

    if (status == HDRG_OK) {
        hdrg_color_set_sdr_eotf(&out->color, params->sdr_eotf);

        status = hdrg_source_init(alloc, raw, &out->color, &out->src);
    }

    if (status == HDRG_OK && params->grain_policy == HDRG_GRAIN_RESYNTH && !out->streaming) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    out->prefiltering = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_PREFILTER) != NULL ||
                        params->grain_policy != HDRG_GRAIN_PRESERVE ||
                        params->deblock_strength > 0.0f || params->dering_strength > 0.0f ||
                        params->defringe_strength > 0.0f || params->deband_strength > 0.0f ||
                        (out->streaming && params->mctf_strength > 0.0f);

    if (status == HDRG_OK && out->prefiltering) {
        status = hdrg_pipeline_prefilter_setup(ctx, out, raw->pixels);
    }

    out->scaling = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_SCALE) != NULL ||
                   out->width != out->src.width || out->height != out->src.height;

    if (status == HDRG_OK && out->scaling) {
        status = hdrg_pipeline_scale_setup(ctx, out);
    }

    if (status == HDRG_OK && params->light_predictor > 0.0f) {
        status = hdrg_pipeline_analyzer_setup(ctx, out);
    }

    if (status == HDRG_OK) {
        hdrg_pipeline_delays(out);
    }

    /* pass C reads the source, or the scaled bands in 16-bit code values */
    frame = out->scaling ? &out->scaled : &out->src;

    if (status == HDRG_OK) {
        hdrg_gamut_map_init(params, hdrg_render_gamut(params, encode, caps, &out->color), &map);

        status = hdrg_color_lut_build(alloc, &out->exec, &out->color, frame->lut,
                                      frame->entries - 1u, &map, &out->lut);
    }

    if (status == HDRG_OK) {
        status = hdrg_apply_setup(alloc, &out->exec, frame, out->scaling, &out->lut, ctx->kernels,
                                  params, encode, &out->apply);
    }

    if (status != HDRG_OK || encode->output_mode == HDRG_OUTPUT_PQ) {
        if (status != HDRG_OK) {
            hdrg_pipeline_close(ctx, out);
        }

        return status;
    }

    /* two-layer modes: the log2 ratios are summed per stored gain-map cell in pass C */
    out->apply.gm_shift = encode->gainmap_scale_shift;
    out->apply.gm_w = (out->width + (1u << out->apply.gm_shift) - 1u) >> out->apply.gm_shift;
    gm_h = (out->height + (1u << out->apply.gm_shift) - 1u) >> out->apply.gm_shift;
    out->gm_cells = (size_t)out->apply.gm_w * gm_h;
    out->apply.gm_sum = (float *)hdrg_mem_calloc(alloc, out->gm_cells, sizeof(float));
    out->apply.chunk_max = (float *)hdrg_mem_calloc(
        alloc, hdrg_pool_row_chunks(out->height, HDRG_EXEC_GRAIN_ROWS), sizeof(float));
    if (out->apply.gm_sum == NULL || out->apply.chunk_max == NULL) {
        hdrg_pipeline_close(ctx, out);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_pipeline_proxy(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         uint8_t shift)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    hdrg_status_t status;
    uint32_t width, height;

    if (shift == 0) {
        return HDRG_OK;
    }

    if (shift > HDRG_PROXY_MAX_SHIFT || pipeline->proxy_pixels != NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    width = (pipeline->src.width + (1u << shift) - 1u) >> shift;
    height = (pipeline->src.height + (1u << shift) - 1u) >> shift;

    status = hdrg_source_init_rgb16(alloc, &pipeline->color, width, height, &pipeline->proxy);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->proxy_pixels =
        (uint16_t *)hdrg_mem_alloc(alloc, (size_t)width * height * 3u * sizeof(uint16_t));
    if (pipeline->proxy_pixels == NULL) {
        hdrg_source_free(alloc, &pipeline->proxy);

        return HDRG_ERR_NOMEM;
    }

    pipeline->proxy.pixels = (const uint8_t *)(const void *)pipeline->proxy_pixels;
    pipeline->proxy_shift = shift;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_pipeline_output(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                          uint8_t depth, bool light)
{
    const uint32_t chunks = hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS);

    if ((depth != 8 && depth != 16) ||
        (light && pipeline->encode.output_mode == HDRG_OUTPUT_GAINMAP)) {
        return HDRG_ERR_INVALID_ARG;
    }

    pipeline->apply.sample_bytes = (uint8_t)(depth / 8u);

    if (!light) {
        return HDRG_OK;
    }

    if (pipeline->apply.chunk_cll == NULL) {
        pipeline->apply.chunk_cll = (float *)hdrg_mem_calloc(&ctx->alloc, chunks, sizeof(float));
    }

    if (pipeline->apply.chunk_fall == NULL) {
        pipeline->apply.chunk_fall = (double *)hdrg_mem_calloc(&ctx->alloc, chunks, sizeof(double));
    }

    if (pipeline->apply.chunk_light == NULL) {
        pipeline->apply.chunk_light =
            (hdrg_lightstat_t *)hdrg_mem_calloc(&ctx->alloc, chunks, sizeof(hdrg_lightstat_t));
    }

    if (pipeline->apply.chunk_cll == NULL || pipeline->apply.chunk_fall == NULL ||
        pipeline->apply.chunk_light == NULL) {
        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_pipeline_analyze(hdrg_pipeline_t *pipeline, const uint8_t *pixels,
                                           const hdrg_analyze_opts_t *opts, hdrg_analysis_t *out)
{
    const hdrg_source_t *picture;
    hdrg_analyze_opts_t local;
    hdrg_analyze_given_t given;
    hdrg_source_t mosaic;
    hdrg_status_t status;

    pipeline->src.pixels = pixels;

    /* the caller's choices on the cells of the effort and the params' colored lights */
    memset(&local, 0, sizeof(local));
    local.bright_scale = 1.0f;

    if (opts != NULL) {
        local = *opts;
    }

    local.cell_shift =
        (pipeline->params.effort == HDRG_EFFORT_QUALITY) ? HDRG_FINE_SHIFT : HDRG_QUARTER_SHIFT;
    local.colored = pipeline->params.colored_lights;
    local.predictor = pipeline->params.light_predictor;
    local.judge = pipeline->analyzer.created ? hdrg_analyzer_judge : NULL;
    local.judge_user = &pipeline->analyzer;

    if (pipeline->proxy_shift > 0) {
        hdrg_pipeline_reduce(pipeline);
    }

    picture = (pipeline->proxy_shift == 0) ? &pipeline->src : &pipeline->proxy;

    /* pass A's sums on a session's GPU where it takes the picture */
    if (pipeline->gpu_stats != NULL && hdrg_gpu_analyze_takes(picture)) {
        status =
            pipeline->gpu_stats->stats(pipeline->gpu_stats->arg, picture, local.cell_shift, &given);
        if (status != HDRG_OK) {
            return status;
        }

        local.given = &given;
    }

    status = hdrg_analyze(pipeline->frame_alloc, &pipeline->exec, picture, &local, out);

    if (status != HDRG_OK) {
        return status;
    }

    /* the proxy's cells span 2^shift times the source pixels */
    if (pipeline->proxy_shift > 0) {
        out->cell_shift = (uint8_t)(out->cell_shift + pipeline->proxy_shift);
    }

    /* noise only shows at full resolution (a frame kept on the GPU: its tiles read back) */
    if (pipeline->mosaic != NULL) {
        mosaic = pipeline->src;
        mosaic.pixels = (const uint8_t *)(const void *)pipeline->mosaic;
        mosaic.stride = (size_t)pipeline->grid.nx * HDRG_NOISE_WINDOW * 6u;
        mosaic.width = pipeline->grid.nx * HDRG_NOISE_WINDOW;
        mosaic.height = pipeline->grid.ny * HDRG_NOISE_WINDOW;
        mosaic.row0 = 0;
        status = hdrg_noise_estimate_mosaic(pipeline->frame_alloc, &pipeline->exec, &mosaic,
                                            &pipeline->grid, &out->noise);
    } else {
        status = hdrg_noise_estimate(pipeline->frame_alloc, &pipeline->exec, &pipeline->src,
                                     &out->noise);
    }

    if (status != HDRG_OK) {
        hdrg_analysis_free(pipeline->frame_alloc, out);
    }

    return status;
}

extern hdrg_status_t hdrg_pipeline_plan(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        const uint8_t *pixels, hdrg_analysis_t *analysis)
{
    const hdrg_allocator_t *alloc = pipeline->frame_alloc;
    const hdrg_source_t *picture;
    const uint8_t shift =
        (pipeline->proxy_shift <= HDRG_PLAN_SHIFT_MAX) ? pipeline->proxy_shift : 0u;
    hdrg_status_t status;

    hdrg_plan_free(alloc, &pipeline->plan);

    pipeline->src.pixels = pixels;

    /* a proxy analysis plans on the frame's proxy too: the grid is the frame's */
    if (shift > 0u) {
        hdrg_pipeline_reduce(pipeline);
    }

    picture = (shift > 0u) ? &pipeline->proxy : &pipeline->src;
    status = hdrg_plan_build_with(
        alloc, &pipeline->exec, ctx->kernels, picture, analysis, shift, &pipeline->params,
        (pipeline->gpu_stats != NULL && hdrg_gpu_analyze_takes(picture))
            ? pipeline->gpu_stats->moments
            : NULL,
        (pipeline->gpu_stats != NULL) ? pipeline->gpu_stats->arg : NULL, &pipeline->plan);

    /* the planes only feed the plan; the statistics stay */
    hdrg_analysis_free(alloc, analysis);
    pipeline->analysis = *analysis;

    if (status == HDRG_OK) {
        status = hdrg_render_build(alloc, &pipeline->plan, &pipeline->params, &pipeline->encode,
                                   &pipeline->caps, &pipeline->color, &pipeline->render);
    }

    return status;
}

extern hdrg_status_t hdrg_pipeline_grid(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        const uint8_t *pixels, const hdrg_analysis_t *analysis,
                                        hdrg_plan_t *out)
{
    const hdrg_source_t *picture;
    const uint8_t shift =
        (pipeline->proxy_shift <= HDRG_PLAN_SHIFT_MAX) ? pipeline->proxy_shift : 0u;

    pipeline->src.pixels = pixels;

    if (shift > 0u) {
        hdrg_pipeline_reduce(pipeline);
    }

    picture = (shift > 0u) ? &pipeline->proxy : &pipeline->src;

    return hdrg_plan_build_with(pipeline->frame_alloc, &pipeline->exec, ctx->kernels, picture,
                                analysis, shift, &pipeline->params,
                                (pipeline->gpu_stats != NULL && hdrg_gpu_analyze_takes(picture))
                                    ? pipeline->gpu_stats->moments
                                    : NULL,
                                (pipeline->gpu_stats != NULL) ? pipeline->gpu_stats->arg : NULL,
                                out);
}

extern hdrg_status_t hdrg_pipeline_bind(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    hdrg_status_t status;

    /* (frames scaled apart: by the session) */
    if (pipeline->scale_keeps && !pipeline->scale_apart) {
        status = hdrg_pipeline_scale_next(ctx, pipeline, true);
        if (status != HDRG_OK) {
            return status;
        }
    } else if (pipeline->scaling && !pipeline->scale_apart) {
        pipeline->told.noise = NULL;
        pipeline->told.motion = NULL;
        pipeline->told.motion_size = 0;
        pipeline->told.pts = pipeline->frame_pts;
        pipeline->told.index = pipeline->frame_index;
        pipeline->told.first = pipeline->frame_index == pipeline->origin;

        status = hdrg_stage_configure(&pipeline->scale, &pipeline->src, pipeline->width,
                                      pipeline->height, &pipeline->told, &ctx->host);
        if (status != HDRG_OK) {
            return status;
        }
    }

    hdrg_apply_bind(&pipeline->apply, &pipeline->plan, &pipeline->render, &pipeline->params);

    if (pipeline->apply.gm_sum != NULL) {
        memset(pipeline->apply.gm_sum, 0, pipeline->gm_cells * sizeof(float));
        memset(pipeline->apply.chunk_max, 0,
               hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS) * sizeof(float));
    }

    if (pipeline->apply.chunk_cll != NULL) {
        memset(pipeline->apply.chunk_cll, 0,
               hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS) * sizeof(float));
        memset(pipeline->apply.chunk_fall, 0,
               hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS) * sizeof(double));
    }

    if (pipeline->apply.chunk_light != NULL) {
        memset(pipeline->apply.chunk_light, 0,
               hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS) *
                   sizeof(hdrg_lightstat_t));
        hdrg_color_to_gamut(&pipeline->color, pipeline->render.gamut, pipeline->apply.light_matrix);
        pipeline->apply.light_white = pipeline->render.sdr_white_nits;
    }

    return HDRG_OK;
}

extern void hdrg_pipeline_fixed(hdrg_pipeline_t *pipeline)
{
    hdrg_plan_free(pipeline->frame_alloc, &pipeline->plan);
    hdrg_plan_fixed(&pipeline->params, pipeline->src.width, pipeline->src.height, &pipeline->plan);
    hdrg_render_fixed(&pipeline->plan, &pipeline->params, &pipeline->encode, &pipeline->caps,
                      &pipeline->color, &pipeline->render);
}

extern hdrg_status_t hdrg_pipeline_frame(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         const uint8_t *pixels)
{
    hdrg_pipeline_out_t out;
    hdrg_analysis_t analysis;
    hdrg_status_t status;
    uint32_t index;

    /* the still's own frame of the stage's output */
    if (pipeline->prefiltering) {
        if (pipeline->filtered == NULL) {
            pipeline->filtered = (uint16_t *)hdrg_mem_calloc(
                &ctx->alloc, (size_t)pipeline->src.width * pipeline->src.height * 3u,
                sizeof(uint16_t));
        }

        if (pipeline->filtered == NULL) {
            return HDRG_ERR_NOMEM;
        }

        out.dst = pipeline->filtered;
        out.slot = UINT32_MAX;

        status =
            hdrg_pipeline_prefilter(ctx, pipeline, pixels, pipeline->input.stride, 0, &out, &index);
        if (status != HDRG_OK) {
            return status;
        }

        pixels = out.pixels;
    }

    status = hdrg_pipeline_analyze(pipeline, pixels, NULL, &analysis);
    if (status == HDRG_OK) {
        status = hdrg_pipeline_plan(ctx, pipeline, pixels, &analysis);
    }

    if (status == HDRG_OK) {
        status = hdrg_pipeline_bind(ctx, pipeline);
    }

    return status;
}

extern void hdrg_pipeline_cap_flights(hdrg_pipeline_t *pipeline, uint32_t most)
{
    uint8_t i;

    for (i = 0; i < pipeline->prefilter_count; i++) {
        hdrg_stage_cap_flights(&pipeline->stages[i], most);
    }

    if (pipeline->scaling) {
        hdrg_stage_cap_flights(&pipeline->scale, most);
    }

    pipeline->flight_cap = most;
    hdrg_pipeline_delays(pipeline);
}

extern void hdrg_pipeline_delay(const hdrg_pipeline_t *pipeline, uint32_t most,
                                hdrg_pipeline_delay_t *out)
{
    const hdrg_stage_t *stage, *scale = &pipeline->scale;
    uint32_t flight;
    uint8_t i;

    memset(out, 0, sizeof(*out));

    for (i = 0; pipeline->streaming && i < pipeline->prefilter_count; i++) {
        stage = &pipeline->stages[i];
        flight = hdrg_pipeline_flights(stage, most);
        out->chain += stage->future + flight;
        out->chain_flight += flight;
        out->chain_past += stage->past;
    }

    if (pipeline->streaming && pipeline->scaling) {
        out->scale_flight = hdrg_pipeline_flights(scale, most);
        out->scale = scale->future + out->scale_flight;
        out->scale_past = scale->past;
    }
}

extern hdrg_status_t hdrg_pipeline_rows(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline, uint32_t y0,
                                        uint32_t rows, uint8_t *dst)
{
    hdrg_status_t status;

    /* the SCALE output: a frame scaled apart, or the stage's band */
    if (pipeline->scaling && pipeline->scaled_given != NULL) {
        pipeline->scaled.pixels = (const uint8_t *)pipeline->scaled_given;
        pipeline->scaled.stride = pipeline->scaled_given_stride;
        pipeline->scaled.row0 = 0;
    } else if (pipeline->scaling) {
        status = hdrg_stage_rows(&pipeline->scale, &pipeline->exec, y0, rows, pipeline->scaled_band,
                                 &ctx->host);
        if (status != HDRG_OK) {
            return status;
        }

        pipeline->scaled.pixels = (const uint8_t *)pipeline->scaled_band;
        pipeline->scaled.stride = (size_t)pipeline->width * 3u * sizeof(uint16_t);
        pipeline->scaled.row0 = y0;
    }

    hdrg_apply_rows(&pipeline->exec, &pipeline->apply, y0, rows, dst);

    return HDRG_OK;
}

extern void hdrg_pipeline_end(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    uint32_t j;
    uint8_t i;

    hdrg_stage_close(&pipeline->scale);
    hdrg_options_destroy(pipeline->default_options);
    hdrg_analyzer_close(&pipeline->analyzer);
    hdrg_options_destroy(pipeline->analyzer_options);
    pipeline->analyzer_options = NULL;

    /* the frames the chain's last stage gave go back before the stages close */
    for (j = 0; pipeline->prefilter_count > 0 && j < pipeline->held_count; j++) {
        hdrg_stage_release(&pipeline->stages[pipeline->prefilter_count - 1u], &pipeline->held[j]);
    }

    hdrg_mem_free(alloc, pipeline->held);
    pipeline->held = NULL;
    pipeline->held_count = 0;

    for (i = 0; i < HDRG_PREFILTER_MAX; i++) {
        hdrg_stage_close(&pipeline->stages[i]);
        hdrg_options_destroy(pipeline->prefilter_options[i]);
        hdrg_options_destroy(pipeline->prefilter_cpu_options[i]);
        pipeline->prefilter_options[i] = NULL;
        pipeline->prefilter_cpu_options[i] = NULL;
    }

    hdrg_apply_free(alloc, &pipeline->apply);
    hdrg_color_lut_free(alloc, &pipeline->lut);
    hdrg_source_free(alloc, &pipeline->scaled);
    hdrg_source_free(alloc, &pipeline->scale_in);
    hdrg_mem_free(alloc, pipeline->scaled_band);
    hdrg_mem_free(alloc, pipeline->filtered);
    hdrg_mem_free(alloc, pipeline->chain);

    pipeline->default_options = NULL;
    pipeline->scaled_band = NULL;
    pipeline->filtered = NULL;
    pipeline->chain = NULL;
}

extern float hdrg_pipeline_max_ratio(const hdrg_pipeline_t *pipeline)
{
    const uint32_t chunks = hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS);
    uint32_t i;
    float max_lr;

    max_lr = 0.0f;
    for (i = 0; i < chunks; i++) {
        max_lr = HDRG_MAX(max_lr, pipeline->apply.chunk_max[i]);
    }

    return HDRG_MAX(max_lr, HDRG_MIN_RATIO);
}

extern void hdrg_pipeline_light(const hdrg_pipeline_t *pipeline, float *max_nits, float *avg_nits)
{
    const uint32_t chunks = hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS);
    uint32_t i;
    double sum;

    *max_nits = 0.0f;
    sum = 0.0;

    for (i = 0; pipeline->apply.chunk_cll != NULL && i < chunks; i++) {
        *max_nits = HDRG_MAX(*max_nits, pipeline->apply.chunk_cll[i]);
        sum += pipeline->apply.chunk_fall[i];
    }

    *avg_nits = (float)(sum / ((double)pipeline->width * (double)pipeline->height));
}

extern void hdrg_pipeline_light_of(const hdrg_pipeline_t *pipeline, hdrg_lightstat_t *out)
{
    const hdrg_lightstat_t *light;
    const uint32_t chunks = hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS);
    uint32_t i, b, c;

    memset(out, 0, sizeof(*out));

    for (i = 0; pipeline->apply.chunk_light != NULL && i < chunks; i++) {
        light = &pipeline->apply.chunk_light[i];

        for (b = 0; b < HDRG_LIGHT_BINS; b++) {
            out->count[b] += light->count[b];
            out->samples[b] += light->samples[b];
            out->ratio[b] += light->ratio[b];
            out->place[b] += light->place[b];
        }

        for (c = 0; c < 3u; c++) {
            out->channel[c] = HDRG_MAX(out->channel[c], light->channel[c]);
        }
    }
}

extern void hdrg_pipeline_stats(hdrg_context_t *ctx, const hdrg_pipeline_t *pipeline)
{
    const hdrg_noise_t *noise =
        pipeline->prefiltering ? &pipeline->input_noise : &pipeline->analysis.noise;

    ctx->last_stats.diffuse_stops = pipeline->render.diffuse;
    ctx->last_stats.light_area_ratio = pipeline->analysis.light_ratio;
    ctx->last_stats.sdr_apl_nits = pipeline->render.sdr_apl_nits;
    ctx->last_stats.apl_nits = pipeline->render.apl_nits;
    ctx->last_stats.apl_scale = pipeline->render.apl_scale;
    ctx->last_stats.noise_sigma = hdrg_noise_codes(noise->sigma);
    ctx->last_stats.noise_blockiness = noise->blockiness;
    ctx->last_stats.noise_ringing = noise->ringing;
    ctx->last_stats.simd_tier = pipeline->apply.kernels->tier;
}

extern void hdrg_pipeline_close(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    uint32_t i;

    hdrg_pipeline_end(ctx, pipeline);
    hdrg_mem_free(alloc, pipeline->apply.gm_sum);
    hdrg_mem_free(alloc, pipeline->apply.chunk_max);
    hdrg_mem_free(alloc, pipeline->apply.chunk_cll);
    hdrg_mem_free(alloc, pipeline->apply.chunk_fall);
    hdrg_mem_free(alloc, pipeline->apply.chunk_light);
    hdrg_plan_free(pipeline->frame_alloc, &pipeline->plan);
    hdrg_analysis_free(pipeline->frame_alloc, &pipeline->analysis);

    for (i = 0; i < HDRG_PIPELINE_HELD; i++) {
        hdrg_mem_free(pipeline->frame_alloc, pipeline->held_motion[i]);
        hdrg_mem_free(pipeline->frame_alloc, pipeline->added_motion[i]);
        pipeline->held_motion[i] = NULL;
        pipeline->held_motion_cap[i] = 0;
        pipeline->added_motion[i] = NULL;
        pipeline->added_motion_cap[i] = 0;
    }

    hdrg_mem_free(pipeline->frame_alloc, pipeline->chain_motion);
    hdrg_mem_free(pipeline->frame_alloc, pipeline->grain_windows);
    hdrg_mem_free(pipeline->frame_alloc, pipeline->held_film);
    pipeline->chain_motion = NULL;
    pipeline->chain_motion_cap = 0;
    pipeline->grain_windows = NULL;
    pipeline->held_film = NULL;

    hdrg_source_free(alloc, &pipeline->src);
    hdrg_source_free(alloc, &pipeline->input);
    hdrg_source_free(alloc, &pipeline->proxy);
    hdrg_mem_free(alloc, pipeline->proxy_pixels);
    hdrg_color_free(alloc, &pipeline->color);

    pipeline->apply.gm_sum = NULL;
    pipeline->apply.chunk_max = NULL;
    pipeline->apply.chunk_cll = NULL;
    pipeline->apply.chunk_fall = NULL;
    pipeline->apply.chunk_light = NULL;
    pipeline->proxy_pixels = NULL;
}
