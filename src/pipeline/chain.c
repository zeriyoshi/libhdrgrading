/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "parts.h"

/* the caller's in_film copied into the chain's held slot (false: no memory) */
static bool hdrg_pipeline_hold_film(hdrg_pipeline_t *pipeline, uint32_t slot)
{
    pipeline->held_film_set[slot] = false;

    if (pipeline->in_film == NULL) {
        return true;
    }

    if (pipeline->held_film == NULL) {
        pipeline->held_film = (uint8_t *)hdrg_mem_alloc(pipeline->frame_alloc,
                                                        HDRG_PIPELINE_HELD * HDRG_GRAIN_FILM_BYTES);
        if (pipeline->held_film == NULL) {
            return false;
        }
    }

    memcpy(pipeline->held_film + (size_t)slot * HDRG_GRAIN_FILM_BYTES, pipeline->in_film,
           HDRG_GRAIN_FILM_BYTES);

    pipeline->held_film_set[slot] = true;

    return true;
}

/* the caller's in_motion copied into the chain's held slot (false: no memory) */
static bool hdrg_pipeline_hold_motion(hdrg_pipeline_t *pipeline, uint32_t slot)
{
    uint8_t *grown;

    pipeline->held_motion_size[slot] = 0;
    pipeline->added_motion_size[slot] = 0;
    pipeline->added_fields[slot] = 0;

    if (pipeline->in_motion == NULL || pipeline->in_motion_size == 0) {
        return true;
    }

    if (pipeline->in_motion_size > pipeline->held_motion_cap[slot]) {
        grown = (uint8_t *)hdrg_mem_realloc(pipeline->frame_alloc, pipeline->held_motion[slot],
                                            pipeline->in_motion_size);
        if (grown == NULL) {
            return false;
        }

        pipeline->held_motion[slot] = grown;
        pipeline->held_motion_cap[slot] = pipeline->in_motion_size;
    }

    memcpy(pipeline->held_motion[slot], pipeline->in_motion, pipeline->in_motion_size);
    pipeline->held_motion_size[slot] = pipeline->in_motion_size;

    return true;
}

/* the HDRG_SD_MOTION stage's job of the chain's frame index gave: its fields kept (false: none) */
static bool hdrg_pipeline_add_motion(hdrg_pipeline_t *pipeline, const hdrg_stage_t *stage,
                                     uint32_t index)
{
    const uint32_t slot = index % HDRG_PIPELINE_HELD;
    const uint8_t *side;
    hdrg_sd_motion_t head;
    uint8_t *grown;
    size_t size, body;

    side = hdrg_stage_motion(stage, &size);
    if (side == NULL) {
        return true;
    }

    memcpy(&head, side, sizeof(head));
    body = size - sizeof(head);

    if (pipeline->added_motion_size[slot] + body > pipeline->added_motion_cap[slot]) {
        grown = (uint8_t *)hdrg_mem_realloc(pipeline->frame_alloc, pipeline->added_motion[slot],
                                            pipeline->added_motion_size[slot] + body);
        if (grown == NULL) {
            return false;
        }

        pipeline->added_motion[slot] = grown;
        pipeline->added_motion_cap[slot] = pipeline->added_motion_size[slot] + body;
    }

    memcpy(pipeline->added_motion[slot] + pipeline->added_motion_size[slot], side + sizeof(head),
           body);
    pipeline->added_motion_size[slot] += body;
    pipeline->added_fields[slot] += head.fields;

    return true;
}

/*
 * cls's PREFILTER entry of strength (as text without the locale: the
 * options are text) and option key of extra (key NULL: none)
 */
static hdrg_status_t hdrg_pipeline_entry(hdrg_context_t *ctx, const hdrg_class_t *cls,
                                         float strength, const char *key, const char *extra,
                                         hdrg_filter_entry_t *entry, hdrg_options_t **options)
{
    hdrg_options_t *text;
    hdrg_status_t status;
    uint32_t millionths;
    char value[32];

    entry->cls = cls;
    status = hdrg_options_create(ctx, &text);
    if (status != HDRG_OK) {
        return status;
    }

    millionths = (uint32_t)(strength * 1e6f + 0.5f);
    (void)snprintf(value, sizeof(value), "%u.%06u", millionths / 1000000u, millionths % 1000000u);
    status = hdrg_options_set(text, "strength", value);

    if (status == HDRG_OK && key != NULL) {
        status = hdrg_options_set(text, key, extra);
    }

    if (status == HDRG_OK) {
        status = hdrg_host_resolve_options(&ctx->host, entry->cls, text, options);
    }

    hdrg_options_destroy(text);

    entry->options = (status == HDRG_OK) ? *options : NULL;
    entry->stage = HDRG_STAGE_PREFILTER;

    return status;
}

/*
 * default d's filter of the PREFILTER stage at the end of its chain, its
 * strength from the params, its GPU version where the context's
 * HDRG_GPU_AUTO / FILTERS has one (hdrg_default_class: the pipeline's GPU's
 * kind first), the CPU's kept for a missing device; option key of value
 * besides (key NULL: none)
 */
static hdrg_status_t hdrg_pipeline_builtin(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                           uint8_t d, float strength, const char *key,
                                           const char *value)
{
    const hdrg_class_t *cls;
    const uint8_t i = pipeline->prefilter_count;
    const char *id = hdrg_defaults[d].id;
    hdrg_filter_entry_t entry;
    hdrg_options_t *options;
    hdrg_status_t status;
    uint8_t at;
    bool cpu;

    /* a filter the build left out (HDRG_PLUGINS_EXCLUDE): the params ask for what it lacks */
    cls = hdrg_host_find_class(&ctx->host, id, strlen(id));
    if (cls == NULL) {
        hdrg_host_set_error(&ctx->host, "the built-in filter is not in this build", id, strlen(id));

        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_pipeline_entry(ctx, cls, strength, key, value, &entry, &options);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->prefilter[i] = entry;
    pipeline->prefilter_options[i] = options;
    pipeline->prefilter_count++;
    at = 0;

    cls =
        hdrg_default_class(&ctx->host, d, ctx->gpu != HDRG_GPU_OFF, pipeline->gpu_first, &at, &cpu);
    if (cls == NULL || cpu) {
        return HDRG_OK;
    }

    status = hdrg_pipeline_entry(ctx, cls, strength, key, value, &entry, &options);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->prefilter_cpu[i] = pipeline->prefilter[i];
    pipeline->prefilter_cpu_options[i] = pipeline->prefilter_options[i];
    pipeline->prefilter[i] = entry;
    pipeline->prefilter_options[i] = options;

    return HDRG_OK;
}

/*
 * The PREFILTER chain: the filters added to the stage, else the built-in
 * deblocking, deringing, defringing, debanding, temporal (a session's) and
 * spatial noise reduction the params ask for, in that order
 */
static hdrg_status_t hdrg_pipeline_chain(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    const hdrg_params_t *params = &pipeline->params;
    hdrg_status_t status;
    uint32_t i;

    status = HDRG_OK;

    for (i = 0; i < ctx->host.filter_count; i++) {
        if (ctx->host.filters[i].stage == HDRG_STAGE_PREFILTER &&
            pipeline->prefilter_count < HDRG_PREFILTER_MAX) {
            pipeline->prefilter[pipeline->prefilter_count++] = ctx->host.filters[i];
        }
    }

    if (pipeline->prefilter_count > 0) {
        return HDRG_OK;
    }

    if (params->deblock_strength > 0.0f) {
        status = hdrg_pipeline_builtin(ctx, pipeline, HDRG_DEFAULT_DEBLOCK,
                                       params->deblock_strength, NULL, NULL);
    }

    if (status == HDRG_OK && params->dering_strength > 0.0f) {
        status = hdrg_pipeline_builtin(ctx, pipeline, HDRG_DEFAULT_DERING, params->dering_strength,
                                       NULL, NULL);
    }

    if (status == HDRG_OK && params->defringe_strength > 0.0f) {
        status = hdrg_pipeline_builtin(ctx, pipeline, HDRG_DEFAULT_DEFRINGE,
                                       params->defringe_strength, NULL, NULL);
    }

    if (status == HDRG_OK && params->deband_strength > 0.0f) {
        status = hdrg_pipeline_builtin(ctx, pipeline, HDRG_DEFAULT_DEBAND, params->deband_strength,
                                       NULL, NULL);
    }

    /* a session's frames around the frame (LOW: those before it alone) */
    if (status == HDRG_OK && pipeline->streaming && params->mctf_strength > 0.0f) {
        status =
            hdrg_pipeline_builtin(ctx, pipeline, HDRG_DEFAULT_MCTF, params->mctf_strength,
                                  (pipeline->latency == HDRG_LATENCY_LOW) ? "future" : NULL, "0");
    }

    if (status == HDRG_OK && params->grain_policy != HDRG_GRAIN_PRESERVE) {
        status = hdrg_pipeline_builtin(
            ctx, pipeline, HDRG_DEFAULT_DENOISE,
            (params->denoise_strength < 0.0f) ? 1.0f : params->denoise_strength, NULL, NULL);
    }

    return status;
}

/*
 * The chain's stages configured over src for a frame of the side data
 * noise (not processed), and how many of them do not pass it through
 */
static hdrg_status_t hdrg_pipeline_chain_configure(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                                   const hdrg_source_t *src, const float *noise,
                                                   uint8_t *active)
{
    hdrg_status_t status;
    uint8_t i;

    *active = 0;
    pipeline->told.noise = noise;
    pipeline->told.motion = NULL;
    pipeline->told.motion_size = 0;

    for (i = 0; i < pipeline->prefilter_count; i++) {
        status = hdrg_stage_configure(&pipeline->stages[i], src, src->width, src->height,
                                      &pipeline->told, &ctx->host);
        if (status != HDRG_OK) {
            return status;
        }

        *active = (uint8_t)(*active + (pipeline->stages[i].passthrough ? 0u : 1u));
    }

    return HDRG_OK;
}

/* the filters told of the chain's input frame index (its noise, motion and pts) */
static void hdrg_pipeline_tell(hdrg_pipeline_t *pipeline, uint32_t index)
{
    pipeline->told.noise = pipeline->held_side[index % HDRG_PIPELINE_HELD];
    pipeline->told.motion = hdrg_pipeline_held_motion(pipeline, index, &pipeline->told.motion_size);
    pipeline->told.pts = pipeline->held_pts[index % HDRG_PIPELINE_HELD];
    pipeline->told.index = pipeline->origin + index;
    pipeline->told.first = index == 0;
    pipeline->told.bypass = pipeline->bypass;
}

/*
 * Stages [first, end) over src, the chain's frame index (its side data
 * held for it): configured, then the working ones each on the previous one's output,
 * alternating so that the last one writes dst. With none working, the
 * segment that ends the chain has its first stage (passing the frame
 * through) write dst, another leaves the frame as it is. *out: the frame
 * after the segment.
 */
static hdrg_status_t hdrg_pipeline_segment(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                           uint8_t first, uint8_t end, const hdrg_source_t *src,
                                           uint32_t index, uint16_t *dst, hdrg_source_t *out)
{
    const uint32_t width = src->width, height = src->height;
    hdrg_stage_t *stages = pipeline->stages, *producer;
    hdrg_source_t between;
    hdrg_status_t status;
    uint32_t y0;
    uint16_t *buf;
    uint8_t i, j, active, hosts, k, h;

    *out = *src;
    active = 0;
    hdrg_pipeline_tell(pipeline, index);

    for (i = first; i < end; i++) {
        status = hdrg_stage_configure(&stages[i], src, width, height, &pipeline->told, &ctx->host);
        if (status != HDRG_OK) {
            return status;
        }

        active = (uint8_t)(active + (stages[i].passthrough ? 0u : 1u));
    }

    if (active == 0 && end < pipeline->prefilter_count) {
        return HDRG_OK;
    }

    /* the frame between two working filters, once a frame has them */
    if (active >= 2u && pipeline->chain == NULL) {
        pipeline->chain =
            (uint16_t *)hdrg_mem_alloc(&ctx->alloc, (size_t)width * height * 3u * sizeof(uint16_t));
        if (pipeline->chain == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    /* the working filters whose frame goes on to the next one on their device */
    hosts = active;

    for (i = first; active > 0 && i < end; i++) {
        for (j = (uint8_t)(i + 1u); j < end && stages[j].passthrough; j++) {
        }

        stages[i].device_out =
            !stages[i].passthrough && j < end && hdrg_pipeline_linked(&stages[i], &stages[j]);
        hosts = (uint8_t)(hosts - (stages[i].device_out ? 1u : 0u));
    }

    /* the host's frames between the filters: packed rows (src's stride is the last frame's) */
    between = pipeline->src;
    between.stride = (size_t)width * 3u * sizeof(uint16_t);
    status = HDRG_OK;
    producer = NULL;
    k = 0;
    h = 0;

    for (i = first; status == HDRG_OK && i < end; i++) {
        if (stages[i].passthrough && (active > 0 || i > first)) {
            continue;
        }

        stages[i].src = (k == 0) ? src : out;

        if (producer != NULL) {
            stages[i].device_in = producer->device_frame;
            stages[i].device_in_ready = producer->device_ready;
        }

        /* on the device: the job alone; else the rows of the host's frames, alternating */
        if (stages[i].device_out) {
            status = hdrg_stage_settle(&stages[i], &pipeline->exec, &ctx->host);
        } else {
            buf = (active == 0 || (hosts - 1u - h) % 2u == 0) ? dst : pipeline->chain;

            for (y0 = 0; status == HDRG_OK && y0 < height; y0 += HDRG_APPLY_BAND_ROWS) {
                status = hdrg_stage_rows(&stages[i], &pipeline->exec, y0,
                                         HDRG_MIN(HDRG_APPLY_BAND_ROWS, height - y0),
                                         buf + (size_t)y0 * width * 3u, &ctx->host);
            }

            between.pixels = (const uint8_t *)(const void *)buf;
            *out = between;
            h++;
        }

        /* the motion its job gave the frame */
        if (status == HDRG_OK && !stages[i].passthrough &&
            !hdrg_pipeline_add_motion(pipeline, &stages[i], index)) {
            status = HDRG_ERR_NOMEM;
        }

        if (producer != NULL) {
            hdrg_stage_device_release(producer);
        }

        producer = stages[i].device_out ? &stages[i] : NULL;
        k++;
    }

    return status;
}

/*
 * RESYNTH: the frames into the chain's stage at (the grain's before) and
 * not out of it yet at most: those its frames in flight and the windows'
 * futures after it hold, and one
 */
static uint32_t hdrg_pipeline_grain_depth(const hdrg_pipeline_t *pipeline, uint8_t at)
{
    const hdrg_stage_t *stage;
    uint32_t depth;
    uint8_t i;

    depth = 1;

    for (i = (at < pipeline->prefilter_count) ? at : 0u; i < pipeline->prefilter_count; i++) {
        stage = &pipeline->stages[i];

        if (!hdrg_pipeline_keeps(pipeline, i)) {
            continue;
        }

        depth += hdrg_pipeline_flying(stage) ? stage->max_inflight - 1u : 0u;
        depth += (i != at) ? stage->future : 0u;
    }

    return HDRG_MIN(depth, HDRG_PIPELINE_HELD);
}

/* RESYNTH: the grid's windows of frame index into the stage at (RGB16 rows), the grain's before */
static hdrg_status_t hdrg_pipeline_grain_keep(hdrg_pipeline_t *pipeline, uint8_t at, uint32_t index,
                                              const uint16_t *rgb, size_t stride)
{
    uint32_t slot;

    if (pipeline->grain_windows == NULL) {
        if (!hdrg_noise_grid(pipeline->src.width, pipeline->src.height, &pipeline->grain_grid)) {
            return HDRG_OK;
        }

        pipeline->grain_frames = hdrg_pipeline_grain_depth(pipeline, at);
        pipeline->grain_windows = (uint16_t *)hdrg_mem_alloc(
            pipeline->frame_alloc, (pipeline->grain_frames + 1u) *
                                       hdrg_grain_windows(&pipeline->grain_grid) *
                                       sizeof(uint16_t));
        if (pipeline->grain_windows == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    slot = index % pipeline->grain_frames;

    hdrg_grain_gather(&pipeline->exec, &pipeline->grain_grid, rgb, stride,
                      pipeline->grain_windows + slot * hdrg_grain_windows(&pipeline->grain_grid));

    pipeline->grain_held[slot] = index + 1u;

    return HDRG_OK;
}

/* RESYNTH: the grain the chain removed from frame index, out of it as rgb, into grain_stats */
static hdrg_status_t hdrg_pipeline_grain_measure(hdrg_pipeline_t *pipeline, uint32_t index,
                                                 const uint16_t *rgb, size_t stride)
{
    uint32_t slot;
    uint16_t *after;
    size_t windows;

    /* the grain its decoder left out, given: in place of the one the chain removed */
    if (hdrg_pipeline_film_grain(pipeline, hdrg_pipeline_held_film(pipeline, index))) {
        return HDRG_OK;
    }

    if (pipeline->grain_windows == NULL ||
        pipeline->grain_held[index % pipeline->grain_frames] != index + 1u) {
        return HDRG_OK;
    }

    slot = index % pipeline->grain_frames;
    windows = hdrg_grain_windows(&pipeline->grain_grid);
    after = pipeline->grain_windows + (size_t)pipeline->grain_frames * windows;

    hdrg_grain_gather(&pipeline->exec, &pipeline->grain_grid, rgb, stride, after);

    return hdrg_grain_measure(pipeline->frame_alloc, &pipeline->exec,
                              pipeline->grain_windows + slot * windows, after,
                              &pipeline->grain_grid, pipeline->src.coeffs, &pipeline->grain_stats);
}

/*
 * The chain from stage `from` on, src (NULL: none, the stage's ring alone)
 * the frame `index` arriving there. A stage that keeps frames (a
 * session's) takes them in: one with a time window goes on with the frame
 * it takes; an ASYNC one in flight submits what it can and goes on with
 * the earliest frame once max_inflight are in flight (ended: the input
 * ended and the stages before hold nothing: once any is). The stages up to
 * the next one that keeps frames run on that frame. A frame landed goes on
 * to a next stage keeping frames as it is (hdrg_stage_push hands it over
 * when they make their frames alike), and the chain's last stage's to the
 * held slot to->slot. *produced: a frame came out (to, *out_index).
 */
static hdrg_status_t hdrg_pipeline_chain_from(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                              uint8_t from, const hdrg_source_t *src,
                                              uint32_t index, bool ended, hdrg_pipeline_out_t *to,
                                              uint32_t *out_index, bool *produced)
{
    const uint16_t *taken;
    hdrg_source_t frame, out;
    hdrg_copy_job_t copy;
    hdrg_stage_t *stage, *lander, *last;
    hdrg_status_t status;
    uint8_t i, first, end, grain_at;
    size_t stride;
    bool dry, have;

    *produced = false;
    have = src != NULL;
    dry = ended;
    lander = NULL;
    i = from;
    grain_at = hdrg_pipeline_grain_stage(pipeline);

    /* RESYNTH without a time window: the grain is measured against the chain's input */
    if (have && from == 0 && grain_at == pipeline->prefilter_count && src->wide &&
        src->comps == 3u) {
        status = hdrg_pipeline_grain_keep(pipeline, grain_at, index,
                                          (const uint16_t *)(const void *)hdrg_source_row(src, 0),
                                          src->stride);
        if (status != HDRG_OK) {
            return status;
        }
    }

    if (have) {
        frame = *src;
    }

    while (i < pipeline->prefilter_count) {
        stage = &pipeline->stages[i];
        first = i;

        if (hdrg_pipeline_keeps(pipeline, i)) {
            if (have) {
                status = hdrg_stage_push(stage, &pipeline->exec, &frame,
                                         pipeline->held_pts[index % HDRG_PIPELINE_HELD], lander);
                if (status != HDRG_OK) {
                    return status;
                }
            }

            if (hdrg_pipeline_flying(stage)) {
                while (stage->flight_count < stage->max_inflight &&
                       hdrg_stage_take(stage, pipeline->origin, dry)) {
                    if (i == grain_at) {
                        status = hdrg_stage_taken(stage, &pipeline->exec, &taken, &stride);
                        if (status == HDRG_OK) {
                            status = hdrg_pipeline_grain_keep(pipeline, grain_at, stage->taken - 1u,
                                                              taken, stride);
                        }

                        if (status != HDRG_OK) {
                            return status;
                        }
                    }

                    pipeline->flying = pipeline->src;
                    pipeline->flying.pixels = NULL;
                    pipeline->told.noise =
                        pipeline->held_side[(stage->taken - 1u) % HDRG_PIPELINE_HELD];
                    pipeline->told.motion = hdrg_pipeline_held_motion(pipeline, (stage->taken - 1u),
                                                                      &pipeline->told.motion_size);
                    pipeline->told.pts = stage->taken_pts;
                    pipeline->told.index = pipeline->origin + stage->taken - 1u;

                    status =
                        hdrg_stage_configure(stage, &pipeline->flying, pipeline->src.width,
                                             pipeline->src.height, &pipeline->told, &ctx->host);
                    if (status == HDRG_OK) {
                        status = hdrg_stage_submit(stage, stage->taken - 1u, &ctx->host);
                    }

                    if (status != HDRG_OK) {
                        return status;
                    }
                }

                if (stage->flight_count == 0 ||
                    (!dry && stage->flight_count < stage->max_inflight)) {
                    return HDRG_OK;
                }

                status =
                    hdrg_stage_land(stage, &pipeline->exec, &taken, &stride, &index, &ctx->host);
                if (status != HDRG_OK) {
                    return status;
                }

                lander = stage;
                first = (uint8_t)(i + 1u);
            } else {
                if (!hdrg_stage_take(stage, pipeline->origin, dry)) {
                    return HDRG_OK;
                }

                status = hdrg_stage_taken(stage, &pipeline->exec, &taken, &stride);
                if (status != HDRG_OK) {
                    return status;
                }

                lander = NULL;
                index = stage->taken - 1u;

                if (i == grain_at) {
                    status = hdrg_pipeline_grain_keep(pipeline, grain_at, index, taken, stride);
                    if (status != HDRG_OK) {
                        return status;
                    }
                }
            }

            frame = pipeline->src;
            frame.pixels = (const uint8_t *)(const void *)taken;
            frame.stride = stride;

            dry = dry && stage->pushed == stage->taken && stage->flight_count == 0;
            have = true;
        }

        for (end = (uint8_t)(i + 1u);
             end < pipeline->prefilter_count && !hdrg_pipeline_keeps(pipeline, end); end++) {
        }

        if (first < end) {
            status = hdrg_pipeline_segment(ctx, pipeline, first, end, &frame, index, to->dst, &out);
            if (status != HDRG_OK) {
                return status;
            }

            lander = (out.pixels == frame.pixels) ? lander : NULL;
            frame = out;
        }

        i = end;
    }

    last = &pipeline->stages[pipeline->prefilter_count - 1u];
    to->pixels = (const uint8_t *)(const void *)to->dst;
    to->stride = (size_t)frame.width * 3u * sizeof(uint16_t);

    /* the chain's last stage landed it: its frame itself to the held slot, else copied */
    if (frame.pixels != to->pixels && lander == last && to->slot < pipeline->held_count &&
        hdrg_stage_detach(last, frame.pixels, &pipeline->held[to->slot])) {
        to->pixels = pipeline->held[to->slot].view.planes[0];
        to->stride = pipeline->held[to->slot].view.strides[0];
    } else if (frame.pixels != to->pixels) {
        copy.from = frame.pixels;
        copy.to = to->dst;
        copy.stride = frame.stride;
        copy.row = to->stride;

        hdrg_exec_rows(&pipeline->exec, frame.height, hdrg_pipeline_copy_rows, &copy);
    }

    pipeline->input_noise = pipeline->held_noise[index % HDRG_PIPELINE_HELD];

    /* RESYNTH: the grain the stage took from the frame (the temporal and spatial reduction's) */
    status = hdrg_pipeline_grain_measure(pipeline, index,
                                         (const uint16_t *)(const void *)to->pixels, to->stride);
    if (status != HDRG_OK) {
        return status;
    }

    *out_index = index;
    *produced = true;

    return HDRG_OK;
}

/* the chain's instances (destroyed again on failure) */
static hdrg_status_t hdrg_pipeline_chain_create(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline)
{
    hdrg_status_t status;
    uint8_t i, k;

    for (i = 0; i < pipeline->prefilter_count; i++) {
        status = hdrg_stage_create(&ctx->alloc, &pipeline->prefilter[i], HDRG_APPLY_BAND_ROWS,
                                   &ctx->host, &pipeline->stages[i]);

        /* a built-in on the GPU without a device: its CPU filter */
        if (status == HDRG_ERR_UNSUPPORTED && pipeline->prefilter_cpu[i].cls != NULL) {
            pipeline->prefilter[i] = pipeline->prefilter_cpu[i];
            pipeline->prefilter_cpu[i].cls = NULL;
            status = hdrg_stage_create(&ctx->alloc, &pipeline->prefilter[i], HDRG_APPLY_BAND_ROWS,
                                       &ctx->host, &pipeline->stages[i]);
        }

        if (status != HDRG_OK) {
            for (k = 0; k < i; k++) {
                hdrg_stage_close(&pipeline->stages[k]);
            }

            return status;
        }
    }

    return HDRG_OK;
}

/* the noise of the caller's frame at pixels (measured once for the frame probed at open) */
static hdrg_status_t hdrg_pipeline_input_noise(hdrg_pipeline_t *pipeline, const uint8_t *pixels,
                                               float noise[HDRG_SD_NOISE_FLOATS])
{
    hdrg_status_t status;

    pipeline->input.pixels = pixels;
    status = HDRG_OK;

    if (pixels != pipeline->probed) {
        status = hdrg_noise_estimate(pipeline->frame_alloc, &pipeline->exec, &pipeline->input,
                                     &pipeline->input_noise);
    }

    pipeline->probed = NULL;
    hdrg_noise_side_data(&pipeline->input_noise, noise);

    return status;
}

extern hdrg_status_t hdrg_pipeline_prefilter_setup(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                                   const uint8_t *pixels)
{
    hdrg_status_t status;
    uint8_t i, active;
    float noise[HDRG_SD_NOISE_FLOATS];

    status = hdrg_pipeline_chain(ctx, pipeline);
    if (status == HDRG_OK) {
        status = hdrg_pipeline_chain_create(ctx, pipeline);
    }

    if (status != HDRG_OK) {
        return status;
    }

    /* a session's chain keeps the frames of its filters' time windows */
    hdrg_pipeline_delays(pipeline);

    pipeline->input = pipeline->src;
    memset(&pipeline->src, 0, sizeof(pipeline->src));

    if (pixels != NULL) {
        status = hdrg_pipeline_input_noise(pipeline, pixels, noise);
        if (status == HDRG_OK) {
            status = hdrg_pipeline_chain_configure(ctx, pipeline, &pipeline->input, noise, &active);
        }

        if (status != HDRG_OK) {
            return status;
        }

        pipeline->probed = pixels;

        /* nothing to do: the caller's frame as it is */
        if (active == 0) {
            for (i = 0; i < pipeline->prefilter_count; i++) {
                hdrg_stage_close(&pipeline->stages[i]);
            }

            pipeline->src = pipeline->input;
            memset(&pipeline->input, 0, sizeof(pipeline->input));
            pipeline->prefiltering = false;
            pipeline->prefilter_count = 0;
            pipeline->probed = NULL;

            return HDRG_OK;
        }
    }

    return hdrg_source_init_rgb16(&ctx->alloc, &pipeline->color, pipeline->input.width,
                                  pipeline->input.height, &pipeline->src);
}

extern hdrg_status_t hdrg_pipeline_prefilter(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                             const uint8_t *pixels, size_t stride, int64_t pts,
                                             hdrg_pipeline_out_t *out, uint32_t *index)
{
    hdrg_source_t frame;
    hdrg_status_t status;
    uint32_t slot;
    uint8_t i;
    bool produced;

    /* the input ended: the frames the windows still hold, the earliest stage's first */
    if (pixels == NULL) {
        for (;;) {
            for (i = 0; i < pipeline->prefilter_count && !hdrg_pipeline_holding(pipeline, i); i++) {
            }

            if (i == pipeline->prefilter_count) {
                return HDRG_EOF;
            }

            status =
                hdrg_pipeline_chain_from(ctx, pipeline, i, NULL, 0, true, out, index, &produced);
            if (status != HDRG_OK || produced) {
                return status;
            }
        }
    }

    slot = pipeline->chain_pushed % HDRG_PIPELINE_HELD;
    pipeline->input.stride = stride;

    status = hdrg_pipeline_input_noise(pipeline, pixels, pipeline->held_side[slot]);
    if (status != HDRG_OK) {
        return status;
    }

    pipeline->held_noise[slot] = pipeline->input_noise;
    pipeline->held_pts[slot] = pts;

    if (!hdrg_pipeline_hold_motion(pipeline, slot) || !hdrg_pipeline_hold_film(pipeline, slot)) {
        return HDRG_ERR_NOMEM;
    }

    frame = pipeline->input;
    pipeline->chain_pushed++;

    status = hdrg_pipeline_chain_from(ctx, pipeline, 0, &frame, pipeline->chain_pushed - 1u, false,
                                      out, index, &produced);
    if (status != HDRG_OK) {
        return status;
    }

    return produced ? HDRG_OK : HDRG_ERR_AGAIN;
}

extern hdrg_status_t hdrg_pipeline_hold(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                        uint32_t slots)
{
    if (!pipeline->prefiltering || pipeline->held != NULL || slots == 0) {
        return HDRG_OK;
    }

    pipeline->held =
        (hdrg_stage_whole_t *)hdrg_mem_calloc(&ctx->alloc, slots, sizeof(hdrg_stage_whole_t));
    if (pipeline->held == NULL) {
        return HDRG_ERR_NOMEM;
    }

    pipeline->held_count = slots;

    return HDRG_OK;
}

extern bool hdrg_pipeline_device_chain(const hdrg_pipeline_t *pipeline, uint32_t kind)
{
    const hdrg_stage_t *stage;
    uint8_t i;

    for (i = 0; i < pipeline->prefilter_count; i++) {
        stage = &pipeline->stages[i];

        if (stage->device != kind || !hdrg_class_library_gpu(stage->cls) || stage->clip > 0) {
            return false;
        }
    }

    return true;
}

extern uint32_t hdrg_pipeline_device_frames(const hdrg_pipeline_t *pipeline)
{
    uint32_t frames;
    uint8_t i;

    frames = 0;

    for (i = 0; i < pipeline->prefilter_count; i++) {
        if (pipeline->stages[i].past + pipeline->stages[i].future > 0) {
            frames += pipeline->stages[i].past + pipeline->stages[i].future + 1u;
        }
    }

    return frames;
}

extern hdrg_status_t hdrg_pipeline_prefilter_device(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                                    void *in, int64_t pts, void **out,
                                                    uint32_t *index)
{
    hdrg_stage_t *stages = pipeline->stages, *stage, *producer;
    hdrg_source_t frame;
    hdrg_status_t status;
    uint32_t slot, at, base, ring, before;
    uint8_t i, grain_at;
    bool have, dry;
    void *current, *kept;

    *out = NULL;
    *index = 0;
    have = in != NULL;
    dry = !have;
    at = pipeline->chain_pushed;

    if (have) {
        slot = pipeline->chain_pushed % HDRG_PIPELINE_HELD;
        hdrg_noise_side_data(&pipeline->input_noise, pipeline->held_side[slot]);
        pipeline->held_noise[slot] = pipeline->input_noise;
        pipeline->held_pts[slot] = pts;

        if (!hdrg_pipeline_hold_motion(pipeline, slot) ||
            !hdrg_pipeline_hold_film(pipeline, slot)) {
            return HDRG_ERR_NOMEM;
        }

        pipeline->chain_pushed++;
    }

    /* the frames' format: their pixels are on the device */
    frame = pipeline->src;
    frame.pixels = NULL;
    current = in;
    producer = NULL;
    base = 0;
    before = UINT32_MAX;
    grain_at = hdrg_pipeline_grain_stage(pipeline);

    /*
     * each working filter on the frame before it on the device; a stage with a time window
     * keeps a copy of it in the session's frames and goes on with the frame it takes
     */
    for (i = 0; i < pipeline->prefilter_count; i++) {
        stage = &stages[i];

        if (stage->past + stage->future > 0) {
            if (have) {
                kept = (pipeline->keep != NULL)
                           ? pipeline->keep->keep(pipeline->keep->arg, current,
                                                  base + stage->pushed % stage->ring_frames)
                           : NULL;

                if (producer != NULL) {
                    hdrg_stage_device_release(producer);
                    producer = NULL;
                }

                status = (kept != NULL)
                             ? hdrg_stage_push_device(stage, kept,
                                                      pipeline->held_pts[at % HDRG_PIPELINE_HELD])
                             : HDRG_ERR_INTERNAL;

                if (status != HDRG_OK) {
                    return status;
                }
            }

            ring = base;
            base += stage->ring_frames;

            /* none out yet: it waits for its future (the input ended: the stages after go on) */
            if (!hdrg_stage_take(stage, pipeline->origin, dry)) {
                if (!dry) {
                    return HDRG_ERR_AGAIN;
                }

                have = false;

                continue;
            }

            at = stage->taken - 1u;
            current = stage->device_window[stage->past];
            before = (i == grain_at) ? ring + (stage->taken - 1u) % stage->ring_frames : before;
            dry = dry && stage->pushed == stage->taken;
            have = true;
        }

        if (!have) {
            continue;
        }

        hdrg_pipeline_tell(pipeline, at);

        status = hdrg_stage_configure(stage, &frame, frame.width, frame.height, &pipeline->told,
                                      &ctx->host);
        if (status != HDRG_OK) {
            if (producer != NULL) {
                hdrg_stage_device_release(producer);
            }

            return status;
        }

        if (stage->passthrough) {
            continue;
        }

        stage->src = &frame;
        stage->device_in = current;
        stage->device_in_ready = (producer != NULL) ? producer->device_ready : 0u;
        stage->device_out = true;
        status = hdrg_stage_settle(stage, &pipeline->exec, &ctx->host);

        if (status == HDRG_OK && !hdrg_pipeline_add_motion(pipeline, stage, at)) {
            status = HDRG_ERR_NOMEM;
        }

        if (producer != NULL) {
            hdrg_stage_device_release(producer);
        }

        if (status != HDRG_OK) {
            return status;
        }

        producer = stage;
        current = stage->device_frame;
    }

    if (!have) {
        return HDRG_EOF;
    }

    pipeline->device_last = producer;
    pipeline->device_before = before;
    pipeline->input_noise = pipeline->held_noise[at % HDRG_PIPELINE_HELD];
    *out = current;
    *index = at;

    return HDRG_OK;
}

extern void hdrg_pipeline_device_release(hdrg_pipeline_t *pipeline)
{
    if (pipeline->device_last != NULL) {
        hdrg_stage_device_release(pipeline->device_last);
        pipeline->device_last = NULL;
    }
}

extern hdrg_status_t hdrg_pipeline_intake(hdrg_pipeline_t *pipeline, uint16_t **rgb, size_t *stride)
{
    if (!pipeline->prefiltering || pipeline->prefilter_count == 0 ||
        !hdrg_pipeline_keeps(pipeline, 0)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return hdrg_stage_intake(&pipeline->stages[0], pipeline->input.width, pipeline->input.height,
                             rgb, stride);
}

extern hdrg_status_t hdrg_pipeline_grain(hdrg_context_t *ctx, hdrg_pipeline_t *pipeline,
                                         const hdrg_grain_stats_t *stats, uint32_t key)
{
    const hdrg_source_t *src = pipeline->scaling ? &pipeline->scaled : &pipeline->src;

    pipeline->apply.grain = (pipeline->params.grain_policy != HDRG_GRAIN_REDUCE &&
                             hdrg_grain_plan(stats, src, pipeline->render.sdr_white_nits,
                                             pipeline->params.grain_amount, key, &pipeline->grain))
                                ? &pipeline->grain
                                : NULL;

    /* (PRESERVE given a codec's grain: the rows at its first frame) */
    if (pipeline->apply.grain != NULL &&
        hdrg_apply_grain_rows(&ctx->alloc, &pipeline->apply) != HDRG_OK) {
        pipeline->apply.grain = NULL;

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

extern bool hdrg_pipeline_film_grain(hdrg_pipeline_t *pipeline, const uint8_t *film)
{
    pipeline->grain_stats.valid = false;

    if (film == NULL) {
        return false;
    }

    /* (a stream's parameters change seldom: the last conversion kept) */
    if (!pipeline->film_ready || memcmp(pipeline->film_cached, film, HDRG_GRAIN_FILM_BYTES) != 0) {
        hdrg_grain_film(film, &pipeline->film_stats);
        memcpy(pipeline->film_cached, film, HDRG_GRAIN_FILM_BYTES);
        pipeline->film_ready = true;
    }

    pipeline->grain_stats = pipeline->film_stats;

    return true;
}

extern const uint8_t *hdrg_pipeline_held_film(const hdrg_pipeline_t *pipeline, uint32_t index)
{
    const uint32_t slot = index % HDRG_PIPELINE_HELD;

    return (pipeline->held_film != NULL && pipeline->held_film_set[slot])
               ? pipeline->held_film + (size_t)slot * HDRG_GRAIN_FILM_BYTES
               : NULL;
}

extern const uint8_t *hdrg_pipeline_held_motion(const hdrg_pipeline_t *pipeline, uint32_t index,
                                                size_t *size)
{
    const uint32_t slot = index % HDRG_PIPELINE_HELD;

    *size = pipeline->held_motion_size[slot];

    return (*size != 0) ? pipeline->held_motion[slot] : NULL;
}

extern const uint8_t *hdrg_pipeline_chain_motion(hdrg_pipeline_t *pipeline, uint32_t index,
                                                 size_t *size, bool *ok)
{
    const uint32_t slot = index % HDRG_PIPELINE_HELD;
    const uint8_t *given;
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    uint32_t taken;
    uint8_t *grown;
    size_t given_size, bytes, need;

    *ok = true;
    given = hdrg_pipeline_held_motion(pipeline, index, &given_size);
    head.fields = 0;
    head.reserved = 0;

    if (given != NULL) {
        memcpy(&head, given, sizeof(head));
    }

    /* the filters' fields that fit after the caller's */
    bytes = 0;

    for (taken = 0;
         taken < pipeline->added_fields[slot] && head.fields + taken < HDRG_SD_MOTION_FIELDS;
         taken++) {
        memcpy(&field, pipeline->added_motion[slot] + bytes, sizeof(field));
        bytes +=
            sizeof(field) + (size_t)field.grid_w * field.grid_h * sizeof(hdrg_sd_motion_block_t);
    }

    if (taken == 0) {
        *size = given_size;

        return given;
    }

    need = sizeof(head) + ((given != NULL) ? given_size - sizeof(head) : 0u) + bytes;

    if (need > pipeline->chain_motion_cap) {
        grown = (uint8_t *)hdrg_mem_realloc(pipeline->frame_alloc, pipeline->chain_motion, need);
        if (grown == NULL) {
            *ok = false;
            *size = 0;

            return NULL;
        }

        pipeline->chain_motion = grown;
        pipeline->chain_motion_cap = need;
    }

    head.fields += taken;
    memcpy(pipeline->chain_motion, &head, sizeof(head));

    if (given != NULL) {
        memcpy(pipeline->chain_motion + sizeof(head), given + sizeof(head),
               given_size - sizeof(head));
    }

    memcpy(pipeline->chain_motion + need - bytes, pipeline->added_motion[slot], bytes);
    *size = need;

    return pipeline->chain_motion;
}
