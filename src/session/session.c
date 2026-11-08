/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

static inline uint64_t hdrg_session_gcd(uint64_t a, uint64_t b)
{
    uint64_t t;

    while (b != 0) {
        t = a % b;
        a = b;
        b = t;
    }

    return a;
}

/* a judged light's box in the source's pixels, its cells 2^shift pixels across */
static inline void hdrg_session_light(const hdrg_light_t *light, uint8_t shift, uint32_t width,
                                      uint32_t height, hdrg_session_light_t *out)
{
    out->x0 = HDRG_MIN(light->x0 << shift, width - 1u);
    out->y0 = HDRG_MIN(light->y0 << shift, height - 1u);
    out->x1 = HDRG_MIN(((light->x1 + 1u) << shift) - 1u, width - 1u);
    out->y1 = HDRG_MIN(((light->y1 + 1u) << shift) - 1u, height - 1u);
    out->boost = light->boost;
}

/*
 * The L1 distance of two scene histograms, the earlier one moved by shift
 * bins first (what a change of the exposure alone does; its mass past the
 * ends piles into the end bins)
 */
static inline float hdrg_scene_distance(const float *earlier, const float *later, float shift)
{
    const int32_t last = (int32_t)HDRG_SCENE_BINS - 1;
    uint8_t i;
    int32_t lo;
    float moved[HDRG_SCENE_BINS], at, w, distance;

    memset(moved, 0, sizeof(moved));

    for (i = 0; i < HDRG_SCENE_BINS; i++) {
        at = (float)i + shift;
        lo = (int32_t)at;
        lo = ((float)lo > at) ? lo - 1 : lo;
        w = at - (float)lo;
        moved[HDRG_CLAMP(lo, 0, last)] += earlier[i] * (1.0f - w);
        moved[HDRG_CLAMP(lo + 1, 0, last)] += earlier[i] * w;
    }

    distance = 0.0f;

    for (i = 0; i < HDRG_SCENE_BINS; i++) {
        distance += fabsf(later[i] - moved[i]);
    }

    return distance;
}

/*
 * The analysis of a sent frame starts a scene. A step of a ramp of the
 * exposure (the log-average luminance and p99.5 moving the same way together,
 * as the fade-out test measures them) that continues the step of the frame
 * before is compared against the previous histogram moved by it, and the
 * size of the change alone does not cut it: the steps of a fade near black
 * move a stop or more
 */
static bool hdrg_session_cut(hdrg_session_t *session, const hdrg_analysis_t *analysis)
{
    int8_t dir;
    bool candidate, ramp, continuing;
    float key, anchor, dk, da;

    key = hdrg_math_log2f(HDRG_MAX(analysis->key_luma, HDRG_KEY_FLOOR));
    anchor = hdrg_math_log2f(HDRG_MAX(analysis->p995_luma, HDRG_KEY_FLOOR));
    dk = key - session->last_key;
    da = anchor - session->last_anchor;
    dir = (dk <= -HDRG_FADE_STEP && da < 0.0f) ? -1 : (dk >= HDRG_FADE_STEP && da > 0.0f) ? 1 : 0;
    ramp = dir != 0 && fabsf(da - dk) <= HDRG_FADE_SPREAD * fabsf(dk) + HDRG_FADE_SLACK;
    continuing = session->sent_any && ramp && dir == session->ramp;
    candidate =
        !session->sent_any ||
        hdrg_scene_distance(session->last_hist, analysis->scene_hist,
                            continuing ? dk / HDRG_SCENE_BIN_STOPS : 0.0f) >= HDRG_SCENE_HIST_CUT ||
        (!continuing && fabsf(dk) >= HDRG_SCENE_KEY_CUT);

    memcpy(session->last_hist, analysis->scene_hist, sizeof(session->last_hist));
    session->last_key = key;
    session->last_anchor = anchor;
    session->ramp = ramp ? dir : 0;
    session->sent_any = true;
    session->cands = ((session->cands << 1) | (candidate ? 1u : 0u)) & HDRG_SCENE_MASK;

    return candidate && (session->cands >> 1) == 0;
}

/* a rendered frame's statistics without a hold: NULL = not rendered in this session */
static const hdrg_session_used_t *hdrg_session_unheld(const hdrg_session_t *session, uint32_t index)
{
    const hdrg_session_used_t *used = &session->unheld[index % HDRG_FADE_SLOTS];

    return (used->index == index) ? used : NULL;
}

/* an item's analysis as the STATS stage's frame: its first's statistics, its rendered one's lights
 */
static void hdrg_session_stats_frame(const hdrg_session_item_t *item, hdrg_stats_frame_t *out)
{
    const hdrg_analysis_t *rendered = (item->run > 0) ? &item->faded : &item->analysis;

    out->index = item->index;
    out->width = rendered->luma.width;
    out->height = rendered->luma.height;
    out->light_count = rendered->light_count;
    out->cell_shift = rendered->cell_shift;
    out->cut = item->cut ? 1u : 0u;
    out->anchor = item->analysis.p995_luma;
    out->key = item->analysis.key_luma;
    out->luma = item->analysis.luma.data;
    out->light = rendered->light.data;
    out->light_of = rendered->light_of;
    out->lights = (const hdrg_stats_light_t *)(const void *)rendered->lights;
}

/* the STATS stage's queued frames: the k-th after the head */
static hdrg_status_t hdrg_session_stats_queued(void *user, uint32_t k, hdrg_stats_frame_t *out)
{
    const hdrg_session_t *session = (const hdrg_session_t *)user;

    if (k >= session->count) {
        return HDRG_ERR_NOT_FOUND;
    }

    hdrg_session_stats_frame(&session->queue[k], out);

    return HDRG_OK;
}

/*
 * The head item through the STATS stage: its rendered analysis's lights
 * steadied in place, its anchor statistics (its first analysis's) those its
 * plan renders with
 */
static hdrg_status_t hdrg_session_stats_head(hdrg_session_t *session, hdrg_session_item_t *item)
{
    hdrg_analysis_t *rendered = (item->run > 0) ? &item->faded : &item->analysis;
    hdrg_stats_head_t head;
    hdrg_status_t status;

    head.index = item->index;
    head.width = rendered->luma.width;
    head.height = rendered->luma.height;
    head.light_count = rendered->light_count;
    head.cell_shift = rendered->cell_shift;
    head.cut = item->cut ? 1u : 0u;
    head.anchor = item->analysis.p995_luma;
    head.key = item->analysis.key_luma;
    head.light = rendered->light.data;
    head.light_of = rendered->light_of;
    head.lights = (hdrg_stats_light_t *)(void *)rendered->lights;

    status = hdrg_stats_render(&session->stats, &head, hdrg_session_stats_queued, session);
    if (status == HDRG_OK) {
        item->analysis.p995_luma = head.anchor;
        item->analysis.key_luma = head.key;
    }

    return status;
}

/*
 * The head item's plan and render: first without a fade-out's hold (the
 * smoothed anchor statistics and APL factor, kept for the frames after it),
 * then for a darkening frame again from its second analysis, in a fade-out
 * with the statistics and the factor the frame before the run had without a
 * hold
 */
static hdrg_status_t hdrg_session_prepare(hdrg_session_t *session, hdrg_session_item_t *item,
                                          hdrg_session_output_t *out, hdrg_session_stat_t *stat)
{
    const hdrg_session_used_t *held;
    hdrg_session_used_t *unheld = &session->unheld[item->index % HDRG_FADE_SLOTS];
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;
    float fit;

    stat->anchor = hdrg_math_log2f(HDRG_MAX(item->analysis.p995_luma, 1e-6f));
    stat->key = hdrg_math_log2f(HDRG_MAX(item->analysis.key_luma, 1e-6f));
    out->anchor_luma = item->analysis.p995_luma;
    out->key_luma = item->analysis.key_luma;
    status = session->capping ? hdrg_session_pure(session, item) : HDRG_OK;

    /* the STATS stage: the lights steadied, the anchor statistics it renders with */
    if (status == HDRG_OK && session->stats.created) {
        status = hdrg_session_stats_head(session, item);
    }

    if (status != HDRG_OK) {
        return status;
    }

    pipeline->src.stride = item->stride;

    session->stats_slot = item->slot;
    status = hdrg_pipeline_plan(session->ctx, pipeline, item->pixels, &item->analysis);
    session->stats_slot = HDRG_GC_SLOTS;

    if (status != HDRG_OK) {
        return status;
    }

    stat->fit = hdrg_math_log2f(HDRG_MAX(pipeline->render.apl_fit, 1e-6f));

    if (session->stats.created && hdrg_stats_fit(&session->stats, item->index, stat->fit, &fit)) {
        status = hdrg_render_rescale(pipeline->frame_alloc, &pipeline->plan, &pipeline->params,
                                     &pipeline->caps, HDRG_MIN(hdrg_math_exp2f(fit), 1.0f),
                                     &pipeline->render);
    }

    if (status != HDRG_OK) {
        return status;
    }

    unheld->index = item->index;
    unheld->anchor = item->analysis.p995_luma;
    unheld->key = item->analysis.key_luma;
    unheld->scale = pipeline->render.apl_scale;
    out->anchor_used = item->analysis.p995_luma;

    if (item->run == 0) {
        return HDRG_OK;
    }

    held = item->fading ? hdrg_session_unheld(session, item->fade_from) : NULL;
    if (held == NULL) {
        held = unheld;
    } else {
        out->fading = true;
    }

    item->faded.p995_luma = held->anchor;
    item->faded.key_luma = held->key;
    out->anchor_used = held->anchor;
    pipeline->src.stride = item->stride;

    session->stats_slot = item->slot;
    status = hdrg_pipeline_plan(session->ctx, pipeline, item->pixels, &item->faded);
    session->stats_slot = HDRG_GC_SLOTS;

    if (status == HDRG_OK) {
        status = hdrg_render_rescale(pipeline->frame_alloc, &pipeline->plan, &pipeline->params,
                                     &pipeline->caps, held->scale, &pipeline->render);
    }

    /* the shoulder and the contrast's pivot dim with the frame: the output in proportion */
    pipeline->render.dim = item->dim;

    return status;
}

/* the output's dynamic metadata from the frame's light: HDR10+ and ST 2094-50 */
static void hdrg_session_dynamic(const hdrg_pipeline_t *pipeline, hdrg_session_output_t *out)
{
    hdrg_lightstat_t light;

    hdrg_pipeline_light_of(pipeline, &light);
    hdrg_dynamic_hdr10plus(&light, out->avg_nits, out->max_nits, &out->hdr10plus);

    /* (ST 2094-50 at the rendition's headroom: the stream's) */
    if (out->hdr10plus.valid) {
        hdrg_dynamic_gain_curve(
            &light, pipeline->render.sdr_white_nits, pipeline->render.gamut,
            hdrg_math_log2f(pipeline->render.peak_nits / pipeline->render.sdr_white_nits),
            &out->st2094_50);
    } else {
        memset(&out->st2094_50, 0, sizeof(out->st2094_50));
    }
}

/*
 * TWO_PASS, first pass: the head item's statistics, its judged lights and
 * the APL factor of its own plan
 */
static hdrg_status_t hdrg_session_first_pass(hdrg_session_t *session, hdrg_session_output_t *out)
{
    const hdrg_analysis_t *analysis =
        (session->queue[0].run > 0) ? &session->queue[0].faded : &session->queue[0].analysis;
    hdrg_session_item_t *item = &session->queue[0];
    hdrg_session_light_t *grown;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint32_t k;

    if (session->judged_cap < analysis->light_count) {
        grown = (hdrg_session_light_t *)hdrg_mem_alloc(
            &session->ctx->alloc, (size_t)analysis->light_count * 2u * sizeof(*grown));
        if (grown == NULL) {
            return HDRG_ERR_NOMEM;
        }

        hdrg_mem_free(&session->ctx->alloc, session->judged);
        session->judged = grown;
        session->judged_cap = analysis->light_count * 2u;
    }

    for (k = 0; k < analysis->light_count; k++) {
        hdrg_session_light(&analysis->lights[k], analysis->cell_shift, session->pipeline.src.width,
                           session->pipeline.src.height, &session->judged[k]);
    }

    out->lights = (analysis->light_count > 0u) ? session->judged : NULL;
    out->light_count = analysis->light_count;
    out->pts = item->frame.pts;
    out->scene_cut = item->cut;
    out->shed = item->index >= session->shed_at;
    out->anchor_luma = item->analysis.p995_luma;
    out->key_luma = item->analysis.key_luma;
    out->anchor_used = item->analysis.p995_luma;
    session->pipeline.src.stride = item->stride;
    session->pipeline.given = item->given;

    hdrg_fpenv_enter(&fpenv);
    session->stats_slot = item->slot;
    status = hdrg_pipeline_plan(session->ctx, &session->pipeline, item->pixels, &item->analysis);
    session->stats_slot = HDRG_GC_SLOTS;
    hdrg_fpenv_leave(&fpenv);

    session->pipeline.given = NULL;

    out->apl_fit = session->pipeline.render.apl_fit;
    out->noise_sigma = hdrg_noise_codes(item->input_noise.sigma);
    out->blockiness = item->input_noise.blockiness;
    out->ringing = item->input_noise.ringing;

    return status;
}

/* a side's Y'CbCr picture of the format's size (layout 0: none) */
static bool hdrg_session_yuv(const hdrg_raw_image_t *format, const hdrg_session_pixfmt_t *pixfmt,
                             hdrg_yuv_t *out)
{
    memset(out, 0, sizeof(*out));

    if (pixfmt->layout == 0) {
        return true;
    }

    out->width = format->width;
    out->height = format->height;
    out->layout = pixfmt->layout;
    out->matrix = pixfmt->matrix;
    out->bits = pixfmt->bits;
    out->siting = pixfmt->chroma_loc;
    out->full_range = pixfmt->full_range;
    out->interleaved = pixfmt->interleaved;
    out->msb = pixfmt->msb;

    return hdrg_yuv_valid(out);
}

/* what hdrg_session_open made: its GPU, the INTERP stage and its frames, the pipeline */
static void hdrg_session_close(hdrg_context_t *ctx, hdrg_session_t *session)
{
    uint32_t i;

    hdrg_gpu_analyze_free(&ctx->alloc, &session->gpu_work);
    hdrg_gpu_core_destroy(session->gpu);
    hdrg_stage_close(&session->interp);
    hdrg_options_destroy(session->interp_options);
    hdrg_mem_free(&ctx->alloc, session->kept);
    hdrg_mem_free(&ctx->alloc, session->scaled);
    hdrg_mem_free(&ctx->alloc, session->phases);
    hdrg_mem_free(&ctx->alloc, session->synth);

    for (i = 0; i < HDRG_SESSION_MOTIONS; i++) {
        hdrg_mem_free(session->pipeline.frame_alloc, session->sd_motion[i]);
    }

    for (i = 0; i < HDRG_SESSION_PURES; i++) {
        hdrg_plan_free(session->pipeline.frame_alloc, &session->pure[i]);
    }

    hdrg_mem_free(session->pipeline.frame_alloc, session->capped);
    hdrg_pipeline_close(ctx, &session->pipeline);
}

/* the STATS stage's frames and lights the session's analysis keeps (the same layout) */
_Static_assert(sizeof(hdrg_light_t) == sizeof(hdrg_stats_light_t) &&
                   offsetof(hdrg_light_t, boost) == offsetof(hdrg_stats_light_t, boost),
               "hdrg_light_t and hdrg_stats_light_t");

/* whether a STATS filter is there: the app's added to the stage, else the built-in one */
static bool hdrg_session_stats_there(const hdrg_context_t *ctx)
{
    return hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_STATS) != NULL ||
           hdrg_default_find(&ctx->host, HDRG_DEFAULT_STATS) != NULL;
}

/*
 * The STATS stage of a temporal mode (WINDOWED with a window, RECURSIVE,
 * TWO_PASS: the statistics across frames): the app's filter of it, else the
 * built-in hdrg.stats.temporal (HDRG_ERR_UNSUPPORTED where the build has
 * none); none for STATIC and a WINDOWED without windows (every frame on its
 * own)
 */
static hdrg_status_t hdrg_session_stats_open(hdrg_context_t *ctx, hdrg_session_t *session,
                                             const hdrg_raw_image_t *format)
{
    const hdrg_session_config_t *config = &session->config;
    const hdrg_filter_entry_t *added;
    hdrg_filter_entry_t entry;
    hdrg_status_t status;

    if (config->temporal != HDRG_TEMPORAL_RECURSIVE && config->temporal != HDRG_TEMPORAL_TWO_PASS &&
        !(config->temporal == HDRG_TEMPORAL_WINDOWED && (config->past > 0 || config->future > 0))) {
        return HDRG_OK;
    }

    session->stats_info.temporal = config->temporal;
    session->stats_info.latency = config->latency;
    session->stats_info.past = config->past;
    session->stats_info.future = config->future;
    session->stats_info.start = config->start;
    session->stats_info.width = format->width;
    session->stats_info.height = format->height;
    session->stats_info.scene_reach = HDRG_SCENE_REACH;
    session->stats_info.rate = session->rate;
    session->stats_info.passes = config->passes;
    session->stats_info.pass_count = config->pass_count;

    added = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_STATS);
    if (added != NULL) {
        return hdrg_stats_open(added, &ctx->host, &session->stats_info, &session->stats);
    }

    memset(&entry, 0, sizeof(entry));
    entry.cls = hdrg_default_find(&ctx->host, HDRG_DEFAULT_STATS);
    entry.stage = HDRG_STAGE_STATS;

    if (entry.cls == NULL) {
        hdrg_host_set_error(&ctx->host, "the temporal statistics are not in this build",
                            hdrg_defaults[HDRG_DEFAULT_STATS].id,
                            strlen(hdrg_defaults[HDRG_DEFAULT_STATS].id));

        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_host_resolve_options(&ctx->host, entry.cls, NULL, &session->stats_options);
    if (status != HDRG_OK) {
        return status;
    }

    entry.options = session->stats_options;

    return hdrg_stats_open(&entry, &ctx->host, &session->stats_info, &session->stats);
}

static hdrg_status_t hdrg_session_open(hdrg_context_t *ctx, const hdrg_raw_image_t *format,
                                       const hdrg_params_t *params, const hdrg_display_caps_t *caps,
                                       hdrg_session_t *session)
{
    const hdrg_session_pixfmt_t *output = &session->config.output;
    hdrg_raw_image_t ingest;
    hdrg_params_t effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t file_caps;
    hdrg_color_t color;
    hdrg_status_t status;
    uint32_t side, stats_past, stats_future;
    uint8_t comps, depth, shift;

    if (!hdrg_params_resolve(params, &effective) || format->width == 0 || format->height == 0 ||
        format->width > HDRG_MAX_DIMENSION || format->height > HDRG_MAX_DIMENSION ||
        !hdrg_session_yuv(format, &session->config.input, &session->yuv_in) ||
        !hdrg_session_yuv(format, output, &session->yuv_out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    session->yuv_in.fit = effective.chroma_fit;

    /* PHOTO's video: the grain its stage removes comes back (where the noise reduction is) */
    if (effective.grain_policy == HDRG_GRAIN_AUTO) {
        effective.grain_policy = (effective.preset == HDRG_PRESET_PHOTO &&
                                  hdrg_default_find(&ctx->host, HDRG_DEFAULT_DENOISE) != NULL)
                                     ? HDRG_GRAIN_RESYNTH
                                     : HDRG_GRAIN_PRESERVE;
    }

    /* a watched session runs at the video's rate, a file's conversion has no budget */
    if (effective.effort == HDRG_EFFORT_AUTO) {
        effective.effort = (session->config.latency == HDRG_LATENCY_OFFLINE) ? HDRG_EFFORT_QUALITY
                                                                             : HDRG_EFFORT_REALTIME;
    }

    ingest = *format;
    ingest.pixels = NULL;
    comps = format->has_alpha ? 4u : 3u;

    if (session->yuv_in.layout != 0) {
        ingest.bit_depth = 16;
        ingest.stride = (size_t)format->width * 6u;
        ingest.has_alpha = false;
    } else if ((format->bit_depth != 8 && format->bit_depth != 16) ||
               format->stride < (size_t)format->width * comps * (format->bit_depth / 8u) ||
               (format->bit_depth == 16 && (format->stride & 1u) != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!hdrg_display_resolve(caps, &file_caps)) {
        return HDRG_ERR_INVALID_ARG;
    }

    caps = &file_caps;
    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;
    encode.dither = !session->config.dither_off;
    encode.output_width = session->config.output_width;
    encode.output_height = session->config.output_height;

    /* Y'CbCr output: 4:2:0 of the BT.2020 matrix over the BT.2020 container, chroma LEFT */
    if ((output->layout == 0 && output->bits != 8 && output->bits != 16) ||
        (output->layout != 0 &&
         (output->layout != HDRG_YUV_420 || output->matrix != HDRG_MATRIX_BT2020 ||
          output->chroma_loc != HDRG_CHROMA_LEFT || caps->gamut != HDRG_GAMUT_BT2020))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!hdrg_color_from_codes(format->primaries, format->transfer, &color)) {
        return HDRG_ERR_INVALID_ARG;
    }

    status =
        hdrg_pipeline_open(ctx, &ingest, &color, &effective, &encode, caps,
                           (session->config.device != 0) ? session->config.device : ctx->gpu_kind,
                           session->config.latency, &session->pipeline);
    hdrg_color_free(&ctx->alloc, &color);

    if (status != HDRG_OK) {
        return status;
    }

    /* the output at the SCALE stage's size */
    session->yuv_out.width = session->pipeline.width;
    session->yuv_out.height = session->pipeline.height;

    if (session->yuv_out.layout != 0 && !hdrg_yuv_valid(&session->yuv_out)) {
        hdrg_pipeline_close(ctx, &session->pipeline);

        return HDRG_ERR_INVALID_ARG;
    }

    /* LOW: each job of an ASYNC filter waited for at once */
    if (session->config.latency == HDRG_LATENCY_LOW) {
        hdrg_pipeline_cap_flights(&session->pipeline, 1u);
    }

    /* the stream's frames from start; a SCALE keeping frames delays the head for them */
    session->pipeline.origin = session->config.start;
    hdrg_stats_window(&session->stats, session->config.latency, &stats_past, &stats_future);
    session->depth =
        HDRG_MAX(HDRG_MAX(session->config.future, session->pipeline.delay.scale), stats_future);

    /* the queue holds HDRG_SESSION_MAX_WINDOW frames ahead: a deeper window is refused (the
       past is the preroll's, not the queue's) */
    if (session->depth > HDRG_SESSION_MAX_WINDOW) {
        hdrg_pipeline_close(ctx, &session->pipeline);

        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_pipeline_scale_depth(&session->pipeline, session->depth);

    /* the filters are told of the frames' bits as sent */
    session->pipeline.told.bits =
        (session->yuv_in.layout != 0) ? session->yuv_in.bits : format->bit_depth;

    session->pipeline.frame_alloc = &session->recycle.alloc;

    depth = (output->layout != 0) ? 16u : output->bits;
    side = HDRG_MAX(format->width, format->height);
    shift = 0;

    /* QUALITY analyzes every frame at full resolution */
    while (session->config.proxy > 0 && session->pipeline.params.effort == HDRG_EFFORT_REALTIME &&
           shift < HDRG_PROXY_MAX_SHIFT &&
           ((side + (1u << shift) - 1u) >> shift) > session->config.proxy) {
        shift++;
    }

    if (session->config.temporal == HDRG_TEMPORAL_STATIC) {
        hdrg_pipeline_fixed(&session->pipeline);
        shift = 0;
    }

    status = hdrg_pipeline_output(ctx, &session->pipeline, depth, true);
    if (status == HDRG_OK) {
        status = hdrg_pipeline_proxy(ctx, &session->pipeline, shift);
    }

    if (status != HDRG_OK) {
        hdrg_pipeline_close(ctx, &session->pipeline);

        return status;
    }

    /*
     * the Y'CbCr input's conversion and the apply pass on a GPU when there is
     * one (the SCALE stage's bands stay on the CPU)
     */
    if (ctx->gpu == HDRG_GPU_AUTO && (session->yuv_in.layout != 0 || !session->pipeline.scaling) &&
        hdrg_gpu_core_create(&ctx->host,
                             (session->config.device != 0) ? session->config.device : ctx->gpu_kind,
                             &session->gpu) == HDRG_OK) {
        session->gpu_convert = session->yuv_in.layout != 0;
        session->gpu_apply = !session->pipeline.scaling;
    }

    /*
     * the frames kept on the GPU from the conversion to the apply when they
     * can be: the analysis reads their proxy and noise windows read back
     * (or them whole: analyzed at full resolution), the PREFILTER chain runs
     * on the device (its flights capped at 1)
     */
    session->apply_slot = HDRG_GC_SLOTS;
    session->resident =
        session->gpu_convert && session->gpu_apply && ctx->gpu_frames != HDRG_GPU_FRAMES_HOST &&
        session->rate_q == 0 &&
        session->depth + 1u + hdrg_pipeline_device_frames(&session->pipeline) <= HDRG_GC_FRAMES &&
        shift <= HDRG_PLAN_SHIFT_MAX &&
        (!session->pipeline.prefiltering ||
         hdrg_pipeline_device_chain(&session->pipeline, hdrg_gpu_core_device(session->gpu)));

    /*
     * Metal's and Vulkan's frames are host memory the GPU reads in place,
     * and the filters' frames in flight keep the GPU busy while the CPU
     * analyzes (the frames kept on it run the chain one frame at a time):
     * kept on the GPU only under LOW, where the flights are 1 anyway
     */
    if (session->resident && ctx->gpu_frames == HDRG_GPU_FRAMES_CHOSEN &&
        session->config.device == 0 && hdrg_gpu_core_device(session->gpu) != HDRG_DEVICE_WEBGPU &&
        session->config.latency != HDRG_LATENCY_LOW) {
        session->resident = false;
    }

    /* the app's frames on its GPU: on the GPU of the kind to the apply */
    if (session->config.device != 0 &&
        (!session->resident || hdrg_gpu_core_device(session->gpu) != session->config.device)) {
        hdrg_session_close(ctx, session);

        return HDRG_ERR_UNSUPPORTED;
    }

    if (session->resident) {
        hdrg_pipeline_cap_flights(&session->pipeline, 1u);
        session->window_keep.keep = hdrg_session_window_keep;
        session->window_keep.arg = session;
        session->pipeline.keep = &session->window_keep;
    }

    /* the analysis's sums on the GPU (FAST: within L2 of the CPU's) */
    session->stats_slot = HDRG_GC_SLOTS;

    if (session->gpu != NULL && ctx->gpu_analysis == HDRG_GPU_ANALYSIS_FAST) {
        session->gpu_stats.stats = hdrg_session_gpu_sums;
        session->gpu_stats.moments = hdrg_session_gpu_moments;
        session->gpu_stats.arg = session;
        session->pipeline.gpu_stats = &session->gpu_stats;
    }

    if (session->rate_q == 0) {
        return HDRG_OK;
    }

    status = hdrg_session_rate_open(ctx, session);
    if (status != HDRG_OK) {
        hdrg_session_close(ctx, session);
    }

    return status;
}

/* the ingest frames of Y'CbCr input or PREFILTER and the band of Y'CbCr output */
static hdrg_status_t hdrg_session_buffers(hdrg_session_t *session)
{
    const hdrg_allocator_t *alloc = &session->ctx->alloc;
    const size_t frames = (size_t)session->depth + 1u,
                 samples = (size_t)session->pipeline.src.width * session->pipeline.src.height * 3u;
    const bool yuv = session->yuv_in.layout != 0, prefilter = session->pipeline.prefiltering;

    if (session->resident) {
        return hdrg_session_resident_buffers(session);
    }

    if (yuv || prefilter) {
        if (samples > SIZE_MAX / sizeof(uint16_t) / (frames + 1u)) {
            return HDRG_ERR_NOMEM;
        }

        session->ingest_samples = samples;
        session->ingest = (uint16_t *)hdrg_mem_alloc(alloc, samples * frames * sizeof(uint16_t));
        if (session->ingest == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    if (yuv && prefilter) {
        session->scratch = (uint16_t *)hdrg_mem_alloc(alloc, samples * sizeof(uint16_t));
        if (session->scratch == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    /* the chain's last stage gives its frames to the ingest's slots instead of copying them */
    if (prefilter &&
        hdrg_pipeline_hold(session->ctx, &session->pipeline, (uint32_t)frames) != HDRG_OK) {
        return HDRG_ERR_NOMEM;
    }

    if (yuv && hdrg_yuv_scratch(&session->yuv_in) > 0) {
        session->yuv_scratch =
            (float *)hdrg_mem_alloc(alloc, hdrg_yuv_scratch(&session->yuv_in) * sizeof(float));
        if (session->yuv_scratch == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    if (session->yuv_out.layout != 0) {
        session->band = (uint8_t *)hdrg_mem_alloc(alloc, (size_t)session->pipeline.width * 6u *
                                                             HDRG_APPLY_BAND_ROWS);
        if (session->band == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    return HDRG_OK;
}

static hdrg_status_t hdrg_session_send(hdrg_session_t *session, const hdrg_session_frame_t *in)
{
    const uint8_t *pixels;
    hdrg_session_frame_t frame;
    hdrg_pipeline_out_t out;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    hdrg_yuv_t yuv;
    uint64_t t;
    uint32_t index;
    uint16_t *rgb, *converted;
    uint8_t p;
    size_t stride, tight;
    bool gpu, kept;

    /* the app's frames on its GPU (the session's device) or in host memory; their motion */
    if (in != NULL &&
        ((in->device != NULL && session->config.device == 0) ||
         (in->device == NULL && in->planes[0] == NULL) ||
         (in->motion == NULL && in->motion_size != 0) ||
         (in->motion != NULL && !hdrg_motion_check(in->motion, in->motion_size)) ||
         (in->film_grain == NULL && in->film_grain_size != 0) ||
         (in->film_grain != NULL && !hdrg_grain_film_check(in->film_grain, in->film_grain_size)))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (in != NULL && in->device != NULL) {
        status = hdrg_session_device_frame(session, in->device, &session->config.input);
        if (status != HDRG_OK) {
            return status;
        }
    }

    if (session->ended) {
        return HDRG_ERR_INVALID_STATE;
    }

    if (in == NULL) {
        session->ended = true;

        return HDRG_OK;
    }

    if (session->count == session->depth + 1u) {
        return HDRG_ERR_AGAIN;
    }

    /* TWO_PASS: no more frames than the first pass saw */
    if (session->config.passes != NULL &&
        session->config.start + session->taken >= session->config.pass_count) {
        return HDRG_ERR_INVALID_STATE;
    }

    pixels = in->planes[0];
    stride = session->pipeline.prefiltering ? session->pipeline.input.stride
                                            : session->pipeline.src.stride;

    /* 16-bit R'G'B' samples are read as uint16_t: the plane at 2 bytes */
    if (session->yuv_in.layout == 0 && in->device == NULL &&
        (session->pipeline.prefiltering ? session->pipeline.input.wide
                                        : session->pipeline.src.wide) &&
        ((uintptr_t)pixels & 1u) != 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    tight = (size_t)session->pipeline.src.width * 3u * sizeof(uint16_t);
    rgb = NULL;
    yuv = session->yuv_in;
    frame = *in;

    if (yuv.layout != 0) {
        for (p = 0; p < 3u; p++) {
            yuv.planes[p] = in->planes[p];
            yuv.strides[p] = in->strides[p];
        }

        if (in->device == NULL && !hdrg_yuv_fits(&yuv)) {
            return HDRG_ERR_INVALID_ARG;
        }
    }

    if (session->resident) {
        return hdrg_session_send_resident(session, in, &yuv);
    }

    /* the ingest buffer holds what the pipeline reads: the converted frame or the stage's output */
    if (yuv.layout != 0 || session->pipeline.prefiltering) {
        rgb = session->ingest + (size_t)session->ingest_next * session->ingest_samples;
    }

    hdrg_fpenv_enter(&fpenv);
    status = HDRG_OK;
    gpu = false;
    t = hdrg_clock_ns();

    /* Y'CbCr: converted where the chain's first stage takes it, without a copy, when it can */
    if (yuv.layout != 0) {
        converted = session->pipeline.prefiltering ? session->scratch : rgb;
        stride = tight;

        if (session->pipeline.prefiltering) {
            status = hdrg_pipeline_intake(&session->pipeline, &converted, &stride);
            status = (status == HDRG_ERR_UNSUPPORTED) ? HDRG_OK : status;
        }

        /* on the GPU while it works (else the CPU's from this frame on) */
        if (status == HDRG_OK && session->gpu_convert) {
            session->gpu_convert =
                hdrg_gpu_convert_frame(session->gpu, &session->pipeline.exec, &yuv, converted,
                                       stride, HDRG_GC_SLOTS, NULL) == HDRG_OK;
            gpu = session->gpu_convert;
        }

        if (status == HDRG_OK && !gpu) {
            hdrg_yuv_to_rgb16(&session->pipeline.exec, &yuv, session->yuv_scratch, converted,
                              stride);
        }

        pixels = (const uint8_t *)(const void *)converted;
        t = hdrg_session_part(session, HDRG_PART_CONVERT, t);
    }

    /* the frame the chain gives may be an earlier one (time windows) */
    if (status == HDRG_OK && session->pipeline.prefiltering) {
        out.dst = rgb;
        out.slot = session->ingest_next;
        session->pipeline.in_motion = (const uint8_t *)in->motion;
        session->pipeline.in_motion_size = in->motion_size;
        session->pipeline.in_film = (const uint8_t *)in->film_grain;
        status = hdrg_pipeline_prefilter(session->ctx, &session->pipeline, pixels, stride, in->pts,
                                         &out, &index);
        session->pipeline.in_motion = NULL;
        session->pipeline.in_motion_size = 0;
        session->pipeline.in_film = NULL;
        t = hdrg_session_part(session, HDRG_PART_PREFILTER, t);

        if (status == HDRG_OK) {
            frame.pts = session->pipeline.held_pts[index % HDRG_PIPELINE_HELD];
            frame.motion =
                hdrg_pipeline_chain_motion(&session->pipeline, index, &frame.motion_size, &kept);
            status = kept ? HDRG_OK : HDRG_ERR_NOMEM;
            pixels = out.pixels;
            stride = out.stride;
        }
    } else if (status == HDRG_OK && rgb != NULL) {
        pixels = (const uint8_t *)(const void *)rgb;
        stride = tight;
    }

    /* without the stage: the grain its decoder left out, as given */
    if (!session->pipeline.prefiltering) {
        (void)hdrg_pipeline_film_grain(&session->pipeline, (const uint8_t *)in->film_grain);
    }

    hdrg_fpenv_leave(&fpenv);

    /* kept for the windows: a frame comes out with a later one */
    if (status == HDRG_ERR_AGAIN) {
        session->taken++;
        hdrg_session_release(in);

        return HDRG_OK;
    }

    if (status != HDRG_OK) {
        return status;
    }

    status =
        hdrg_session_queue(session, &frame, pixels, stride,
                           session->pipeline.prefiltering ? &session->pipeline.input_noise : NULL);
    (void)hdrg_session_part(session, HDRG_PART_ANALYZE, t);

    if (status != HDRG_OK) {
        return status;
    }

    session->taken++;

    /* the ingest buffer holds the frame from here */
    if (rgb != NULL) {
        hdrg_session_release(in);
        session->queue[session->count - 1u].frame.release = NULL;
        session->ingest_next = (session->ingest_next + 1u) % (session->depth + 1u);
    }

    return HDRG_OK;
}

extern void hdrg_session_release(const hdrg_session_frame_t *frame)
{
    if (frame->release != NULL) {
        frame->release(frame->opaque);
    }
}

extern hdrg_status_t hdrg_session_apply(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                        const uint8_t *pixels, size_t stride,
                                        const hdrg_grain_stats_t *grain, uint32_t key,
                                        hdrg_session_output_t *out)
{
    const size_t row = (size_t)session->pipeline.width * 3u * session->pipeline.apply.sample_bytes;
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;
    uint64_t t;
    uint32_t y0, rows, r, chunks;
    uint8_t *dst;
    bool gpu;

    t = hdrg_clock_ns();
    pipeline->src.pixels = pixels;
    pipeline->src.stride = stride;
    gpu = false;

    /* the frame's grain (the one its chain removed or its decoder left out; keyed by its place) */
    status = hdrg_pipeline_grain(session->ctx, pipeline, grain, key);
    if (status != HDRG_OK) {
        return status;
    }

    chunks = hdrg_pool_row_chunks(pipeline->height, HDRG_EXEC_GRAIN_ROWS);

    if (pipeline->apply.chunk_cll != NULL) {
        memset(pipeline->apply.chunk_cll, 0, chunks * sizeof(float));
        memset(pipeline->apply.chunk_fall, 0, chunks * sizeof(double));
        memset(pipeline->apply.chunk_light, 0, chunks * sizeof(hdrg_lightstat_t));
    }

    /* on the GPU while it works (else the CPU's from this frame on; frames on it: a failure) */
    if (session->gpu_apply) {
        status =
            hdrg_gpu_apply_frame(session->gpu, &pipeline->exec, &pipeline->apply, out->planes[0],
                                 out->strides[0], yuv, session->apply_slot, session->out_device);
        gpu = status == HDRG_OK;
        session->gpu_apply = gpu || session->resident;

        /* frames on the GPU: the frame's error as the send's would be (a device's failure INTERNAL)
         */
        if (gpu || !session->resident) {
            status = HDRG_OK;
        } else if (status != HDRG_ERR_INVALID_ARG && status != HDRG_ERR_UNSUPPORTED &&
                   status != HDRG_ERR_NOMEM) {
            status = HDRG_ERR_INTERNAL;
        }
    }

    for (y0 = 0; status == HDRG_OK && !gpu && y0 < pipeline->height; y0 += HDRG_APPLY_BAND_ROWS) {
        rows = HDRG_MIN(HDRG_APPLY_BAND_ROWS, pipeline->height - y0);
        dst =
            (session->band != NULL) ? session->band : out->planes[0] + (size_t)y0 * out->strides[0];

        status = hdrg_pipeline_rows(session->ctx, pipeline, y0, rows, dst);

        if (status == HDRG_OK && yuv->layout != 0) {
            hdrg_rgb16_to_yuv420(&pipeline->exec, (const uint16_t *)(const void *)session->band, y0,
                                 rows, yuv);

            continue;
        }

        for (r = 0; status == HDRG_OK && session->band != NULL && r < rows; r++) {
            memcpy(out->planes[0] + (size_t)(y0 + r) * out->strides[0],
                   session->band + (size_t)r * row, row);
        }
    }

    (void)hdrg_session_part(session, HDRG_PART_APPLY, t);

    if (status != HDRG_OK) {
        return status;
    }

    hdrg_pipeline_light(pipeline, &out->max_nits, &out->avg_nits);
    hdrg_session_dynamic(pipeline, out);
    hdrg_pipeline_stats(session->ctx, pipeline);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_render(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                         uint8_t mode, hdrg_session_output_t *out)
{
    const bool fixed = session->config.temporal == HDRG_TEMPORAL_STATIC,
               state_only = mode == HDRG_SESSION_STATE;
    hdrg_session_item_t *item = &session->queue[0];
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_session_stat_t stat;
    hdrg_status_t status;
    uint64_t t;

    memset(&stat, 0, sizeof(stat));
    status = HDRG_OK;
    out->flash_limited = false;
    out->fading = false;
    t = hdrg_clock_ns();
    pipeline->given = item->given;
    session->apply_slot = item->slot;

    if (fixed) {
        pipeline->src.pixels = item->pixels;
        pipeline->src.stride = item->stride;
        out->anchor_used = 1.0f;
    } else {
        status = hdrg_session_prepare(session, item, out, &stat);

        /* what it rendered with into the STATS stage's history */
        if (status == HDRG_OK && session->stats.created) {
            hdrg_stats_rendered(&session->stats, item->index, stat.anchor, stat.key, stat.fit);
        }
    }

    if (status == HDRG_OK && !fixed && !session->config.flash_off) {
        status = hdrg_session_flash(session, out);
    }

    (void)hdrg_session_part(session, HDRG_PART_ANALYZE, t);

    /* the SCALE stage is told of the frame; a SCALE keeping frames gives the frames in order */
    pipeline->frame_index = item->index;
    pipeline->frame_pts = item->frame.pts;
    pipeline->ended = session->ended && (!pipeline->windowed || session->drained);

    if (status == HDRG_OK) {
        status = state_only ? hdrg_pipeline_scale_skip(session->ctx, pipeline)
                            : hdrg_pipeline_bind(session->ctx, pipeline);
    }

    if (status == HDRG_OK && mode == HDRG_SESSION_FULL) {
        status = hdrg_session_apply(session, yuv, item->pixels, item->stride, &item->grain,
                                    hdrg_grain_key_of(item->index, 0), out);
    }

    pipeline->given = NULL;
    session->apply_slot = HDRG_GC_SLOTS;

    if (status != HDRG_OK) {
        return status;
    }

    if (state_only) {
        return HDRG_OK;
    }

    out->pts = item->frame.pts;
    out->scene_cut = item->cut;
    out->shed = item->index >= session->shed_at;
    out->peak_nits = pipeline->render.peak_nits;
    out->apl_nits = pipeline->render.apl_nits;
    out->sdr_apl_nits = pipeline->render.sdr_apl_nits;
    out->apl_scale = pipeline->render.apl_scale;
    out->apl_fit = pipeline->render.apl_fit;
    out->noise_sigma = hdrg_noise_codes(item->input_noise.sigma);
    out->blockiness = item->input_noise.blockiness;
    out->ringing = item->input_noise.ringing;
    out->synthetic = false;
    out->phase = 0;
    out->src_pts[0] = item->frame.pts;
    out->src_pts[1] = item->frame.pts;
    out->motion = hdrg_session_motion_of(session, item->index, &out->motion_size);

    return HDRG_OK;
}

extern void hdrg_session_pop(hdrg_session_t *session)
{
    hdrg_session_release(&session->queue[0].frame);
    hdrg_analysis_free(session->pipeline.frame_alloc, &session->queue[0].analysis);
    hdrg_analysis_free(session->pipeline.frame_alloc, &session->queue[0].faded);
    memmove(&session->queue[0], &session->queue[1],
            (size_t)(session->count - 1u) * sizeof(session->queue[0]));
    session->count--;
}

extern hdrg_status_t hdrg_session_queue(hdrg_session_t *session, const hdrg_session_frame_t *frame,
                                        const uint8_t *pixels, size_t stride,
                                        const hdrg_noise_t *input_noise)
{
    hdrg_analyze_opts_t opts;
    hdrg_session_item_t *item;
    hdrg_stats_sent_t sent;
    hdrg_stats_fade_t fade;
    hdrg_stats_frame_t record;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;

    item = &session->queue[session->count];
    item->grain = session->pipeline.grain_stats;

    if (!hdrg_session_store_motion(session, session->config.start + session->sent, frame->motion,
                                   frame->motion_size)) {
        return HDRG_ERR_NOMEM;
    }

    session->pipeline.src.stride = stride;
    memset(&item->faded, 0, sizeof(item->faded));
    item->dim = 1.0f;
    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.bars = true;
    opts.lights = session->stats.created;

    /* STATIC reads no statistics */
    if (session->config.temporal == HDRG_TEMPORAL_STATIC) {
        memset(&item->analysis, 0, sizeof(item->analysis));
        item->analysis.noise.sigma = -1.0f;
        item->analysis.noise.blockiness = 1.0f;
        item->analysis.noise.ringing = 1.0f;
    } else {
        hdrg_fpenv_enter(&fpenv);
        status = hdrg_pipeline_analyze(&session->pipeline, pixels, &opts, &item->analysis);
        hdrg_fpenv_leave(&fpenv);

        if (status != HDRG_OK) {
            return status;
        }
    }

    item->frame = *frame;
    item->pixels = pixels;
    item->stride = stride;
    item->given = session->pipeline.given;
    item->slot = HDRG_GC_SLOTS;
    item->input_noise = (input_noise != NULL) ? *input_noise : item->analysis.noise;
    item->index = session->config.start + session->sent;
    item->cut = (session->config.temporal == HDRG_TEMPORAL_STATIC)
                    ? item->index == 0
                    : hdrg_session_cut(session, &item->analysis);
    item->run = 0;
    item->fading = false;
    item->fade_from = item->index;
    memset(&fade, 0, sizeof(fade));
    fade.cut = item->cut ? 1u : 0u;

    /* the STATS stage: the darkening run up to the frame (a fade-out's hold), the cut it takes */
    if (session->stats.created) {
        sent.index = item->index;
        sent.cut = item->cut ? 1u : 0u;
        sent.anchor = item->analysis.p995_luma;
        sent.key = item->analysis.key_luma;
        status = hdrg_stats_fade(&session->stats, &sent, &fade);

        if (status != HDRG_OK) {
            hdrg_analysis_free(session->pipeline.frame_alloc, &item->analysis);

            return status;
        }

        item->run = fade.run;
        item->fading = fade.hold != 0u;
        item->fade_from = fade.from;
        opts.bright_scale = fade.dim;
    }

    /* a darkening frame again, its bright-component threshold following the dimming */
    if (item->run > 0) {
        item->dim = opts.bright_scale;

        hdrg_fpenv_enter(&fpenv);
        status = hdrg_pipeline_analyze(&session->pipeline, pixels, &opts, &item->faded);
        hdrg_fpenv_leave(&fpenv);

        if (status != HDRG_OK) {
            hdrg_analysis_free(session->pipeline.frame_alloc, &item->analysis);

            return status;
        }
    }

    /* the frame's final analysis to the STATS stage (the camera's motion) */
    item->cut = fade.cut != 0u;

    if (session->stats.created) {
        hdrg_session_stats_frame(item, &record);
        status = hdrg_stats_frame(&session->stats, &record);

        if (status != HDRG_OK) {
            hdrg_analysis_free(session->pipeline.frame_alloc, &item->analysis);
            hdrg_analysis_free(session->pipeline.frame_alloc, &item->faded);

            return status;
        }
    }

    session->count++;
    session->sent++;

    /* the SCALE stage's frames (not under a TWO_PASS first pass: nothing renders) */
    if (session->config.temporal == HDRG_TEMPORAL_TWO_PASS && session->config.passes == NULL) {
        return HDRG_OK;
    }

    hdrg_fpenv_enter(&fpenv);
    status =
        hdrg_pipeline_scale_feed(session->ctx, &session->pipeline, pixels, frame->pts, item->cut);
    hdrg_fpenv_leave(&fpenv);

    /* a frame that fails is not taken: out of the queue again (its release stays the caller's) */
    if (status != HDRG_OK) {
        session->count--;
        session->sent--;
        hdrg_analysis_free(session->pipeline.frame_alloc, &item->analysis);
        hdrg_analysis_free(session->pipeline.frame_alloc, &item->faded);
    }

    return status;
}

extern hdrg_status_t hdrg_session_drain(hdrg_session_t *session)
{
    hdrg_session_frame_t frame;
    hdrg_pipeline_out_t out;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint64_t t;
    uint32_t index;
    bool kept;

    if (session->resident) {
        return hdrg_session_drain_resident(session);
    }

    while (session->ended && session->pipeline.windowed && !session->drained &&
           session->count <= session->depth) {
        out.dst = session->ingest + (size_t)session->ingest_next * session->ingest_samples;
        out.slot = session->ingest_next;
        t = hdrg_clock_ns();

        hdrg_fpenv_enter(&fpenv);
        status =
            hdrg_pipeline_prefilter(session->ctx, &session->pipeline, NULL, 0, 0, &out, &index);
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
        frame.pts = session->pipeline.held_pts[index % HDRG_PIPELINE_HELD];
        frame.motion =
            hdrg_pipeline_chain_motion(&session->pipeline, index, &frame.motion_size, &kept);
        status = kept ? hdrg_session_queue(session, &frame, out.pixels, out.stride,
                                           &session->pipeline.input_noise)
                      : HDRG_ERR_NOMEM;
        (void)hdrg_session_part(session, HDRG_PART_ANALYZE, t);

        if (status != HDRG_OK) {
            return status;
        }

        session->ingest_next = (session->ingest_next + 1u) % (session->depth + 1u);
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_receive(hdrg_session_t *session, hdrg_session_output_t *out,
                                          hdrg_apply_state_t *state)
{
    const hdrg_allocator_t *alloc;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    hdrg_yuv_t yuv;
    uint8_t p;
    size_t row;

    if (out == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    status = hdrg_session_drain(session);
    if (status != HDRG_OK) {
        return status;
    }

    out->lights = NULL;
    out->light_count = 0;

    /* TWO_PASS, first pass: statistics only */
    if (session->config.temporal == HDRG_TEMPORAL_TWO_PASS && session->config.passes == NULL) {
        if (session->count == 0) {
            return session->ended ? HDRG_EOF : HDRG_ERR_AGAIN;
        }

        status = hdrg_session_first_pass(session, out);
        hdrg_session_pop(session);

        return status;
    }

    row = (size_t)session->pipeline.width * 3u * session->pipeline.apply.sample_bytes;
    yuv = session->yuv_out;

    for (p = 0; p < 3u; p++) {
        yuv.planes[p] = out->planes[p];
        yuv.strides[p] = out->strides[p];
    }

    /* the app's frames on its GPU (the session's device) or in host memory; the state: none */
    if (state == NULL && ((out->device != NULL && session->config.device == 0) ||
                          (out->device == NULL && ((yuv.layout == 0 && (out->planes[0] == NULL ||
                                                                        out->strides[0] < row)) ||
                                                   (yuv.layout != 0 && !hdrg_yuv_fits(&yuv)))))) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (state == NULL && out->device != NULL) {
        status = hdrg_session_device_frame(session, out->device, &session->config.output);
        if (status != HDRG_OK) {
            return status;
        }
    }

    session->out_device = (state == NULL) ? out->device : NULL;

    /* the preroll: rendered for the state of the frames after it alone */
    while (session->count > 0 && (session->ended || session->count > session->depth) &&
           session->queue[0].index < session->config.start + session->config.preroll) {
        hdrg_fpenv_enter(&fpenv);
        status = hdrg_session_render(session, &yuv, HDRG_SESSION_STATE, out);
        hdrg_fpenv_leave(&fpenv);

        status = (status == HDRG_OK) ? hdrg_session_pass(session) : status;

        if (status != HDRG_OK) {
            return status;
        }

        status = hdrg_session_drain(session);
        if (status != HDRG_OK) {
            return status;
        }
    }

    if (session->count == 0) {
        return session->ended ? HDRG_EOF : HDRG_ERR_AGAIN;
    }

    /* the head waits for its future window unless the stream ended */
    if (!session->ended && session->count <= session->depth) {
        return HDRG_ERR_AGAIN;
    }

    alloc = &session->ctx->alloc;

    if (state == NULL && out->device == NULL && out->strides[0] != row && session->band == NULL) {
        session->band = (uint8_t *)hdrg_mem_alloc(alloc, row * HDRG_APPLY_BAND_ROWS);
        if (session->band == NULL) {
            return HDRG_ERR_NOMEM;
        }
    }

    if (session->rate_q != 0) {
        return hdrg_session_receive_rate(session, &yuv, out);
    }

    /* the state alone: bound to its plan, its apply the app's */
    hdrg_fpenv_enter(&fpenv);
    status = hdrg_session_render(session, &yuv,
                                 (state != NULL) ? HDRG_SESSION_BIND : HDRG_SESSION_FULL, out);

    /* (the frame's grain in its words: the app's apply adds it) */
    if (status == HDRG_OK && state != NULL) {
        out->max_nits = 0.0f;
        out->avg_nits = 0.0f;
        status = hdrg_pipeline_grain(session->ctx, &session->pipeline, &session->queue[0].grain,
                                     hdrg_grain_key_of(session->queue[0].index, 0));
    }

    if (status == HDRG_OK && state != NULL) {
        status = hdrg_session_export(session, &yuv, state);
    }

    hdrg_fpenv_leave(&fpenv);

    hdrg_session_pop(session);

    return status;
}

extern void hdrg_session_config_init(hdrg_session_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->past = HDRG_SESSION_AUTO;
    config->future = HDRG_SESSION_AUTO;
    config->output.bits = 8;
    config->proxy = HDRG_SESSION_PROXY;
}

/*
 * the windows by default: the past where a STATS filter makes the statistics
 * across frames (else none), the future the past's (LOW's none)
 */
extern void hdrg_session_config_resolve(const hdrg_context_t *ctx, hdrg_session_config_t *config)
{
    if (ctx == NULL || config == NULL) {
        return;
    }

    if (config->past == HDRG_SESSION_AUTO) {
        config->past = hdrg_session_stats_there(ctx) ? HDRG_SESSION_PAST : 0u;
    }

    if (config->future == HDRG_SESSION_AUTO) {
        config->future =
            (config->temporal == HDRG_TEMPORAL_WINDOWED && config->latency != HDRG_LATENCY_LOW)
                ? config->past
                : 0u;
    }
}

extern hdrg_status_t hdrg_session_create(hdrg_context_t *ctx, const hdrg_session_format_t *format,
                                         const hdrg_params_t *params,
                                         const hdrg_display_caps_t *caps,
                                         const hdrg_session_config_t *config, hdrg_session_t **out)
{
    hdrg_session_config_t defaults;
    hdrg_raw_image_t raw;
    hdrg_session_t *session;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint64_t rate_gcd;
    uint32_t i;
    float rate;

    hdrg_session_config_init(&defaults);
    config = (config != NULL) ? config : &defaults;

    if (ctx == NULL || format == NULL || out == NULL || config->struct_size != sizeof(*config) ||
        (config->past > HDRG_SESSION_MAX_WINDOW && config->past != HDRG_SESSION_AUTO) ||
        (config->future > HDRG_SESSION_MAX_WINDOW && config->future != HDRG_SESSION_AUTO) ||
        config->temporal > HDRG_TEMPORAL_STATIC ||
        (config->temporal != HDRG_TEMPORAL_WINDOWED && config->future != 0 &&
         config->future != HDRG_SESSION_AUTO) ||
        (config->temporal == HDRG_TEMPORAL_TWO_PASS && config->passes != NULL &&
         config->start >= config->pass_count) ||
        (config->temporal == HDRG_TEMPORAL_TWO_PASS && config->passes == NULL &&
         (config->start != 0 || config->preroll != 0)) ||
        config->preroll > UINT32_MAX - config->start ||
        (config->rate_num == 0) != (config->rate_den == 0) ||
        config->latency > HDRG_LATENCY_OFFLINE ||
        (config->latency == HDRG_LATENCY_LOW && config->future != 0 &&
         config->future != HDRG_SESSION_AUTO) ||
        (config->out_rate_num == 0) != (config->out_rate_den == 0) ||
        (config->out_rate_num != 0 && config->rate_num == 0) ||
        (config->device != 0 && config->device != HDRG_DEVICE_METAL &&
         config->device != HDRG_DEVICE_VULKAN && config->device != HDRG_DEVICE_WEBGPU)) {
        return HDRG_ERR_INVALID_ARG;
    }

    *out = NULL;

    session = (hdrg_session_t *)hdrg_mem_calloc(&ctx->alloc, 1, sizeof(*session));
    if (session == NULL) {
        return HDRG_ERR_NOMEM;
    }

    session->ctx = ctx;

    session->shed_at = UINT32_MAX;

    for (i = 0; i < HDRG_FADE_SLOTS; i++) {
        session->unheld[i].index = UINT32_MAX;
    }

    session->config = *config;

    /* out_rate (not a TWO_PASS first pass): output frame j at input time j p / q, reduced */
    if (config->out_rate_num != 0 &&
        !(config->temporal == HDRG_TEMPORAL_TWO_PASS && config->passes == NULL)) {
        session->rate_p = (uint64_t)config->out_rate_den * config->rate_num;
        session->rate_q = (uint64_t)config->out_rate_num * config->rate_den;
        rate_gcd = hdrg_session_gcd(session->rate_p, session->rate_q);
        session->rate_p /= rate_gcd;
        session->rate_q /= rate_gcd;
    }

    hdrg_session_config_resolve(ctx, &session->config);

    rate = (session->config.rate_num != 0)
               ? (float)session->config.rate_num / (float)session->config.rate_den
               : (float)HDRG_RECURSIVE_RATE;
    session->rate = rate;
    session->flash_window = (uint32_t)HDRG_CLAMP(rate + 0.999f, 1.0f, (float)HDRG_FLASH_MAX_RATE);

    /* the STATS stage of a temporal mode (its windows size the queue) */
    /* the frames' format as the still pipeline's image without pixels */
    memset(&raw, 0, sizeof(raw));
    raw.width = format->width;
    raw.height = format->height;
    raw.bit_depth = format->bit_depth;
    raw.stride = format->stride;
    raw.primaries = format->primaries;
    raw.transfer = format->transfer;
    raw.has_alpha = format->has_alpha;

    status = hdrg_session_stats_open(ctx, session, &raw);
    if (status != HDRG_OK || !hdrg_recycle_init(&session->recycle, &ctx->alloc)) {
        hdrg_stats_close(&session->stats);
        hdrg_options_destroy(session->stats_options);
        hdrg_mem_free(&ctx->alloc, session);

        return (status != HDRG_OK) ? status : HDRG_ERR_INTERNAL;
    }

    hdrg_fpenv_enter(&fpenv);
    status = hdrg_session_open(ctx, &raw, params, caps, session);
    hdrg_fpenv_leave(&fpenv);

    if (status != HDRG_OK) {
        hdrg_recycle_destroy(&session->recycle);
        hdrg_stats_close(&session->stats);
        hdrg_options_destroy(session->stats_options);
        hdrg_mem_free(&ctx->alloc, session);

        return status;
    }

    status = hdrg_session_buffers(session);

    if (status != HDRG_OK) {
        hdrg_session_destroy(session);

        return status;
    }

    *out = session;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_send_frame(hdrg_session_t *session,
                                             const hdrg_session_frame_t *in)
{
    hdrg_status_t status;
    uint64_t start;

    if (session == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    start = hdrg_clock_ns();
    status = hdrg_session_send(session, in);
    hdrg_session_clock(session, start);

    return status;
}

extern hdrg_status_t hdrg_session_receive_frame(hdrg_session_t *session, hdrg_session_output_t *out)
{
    hdrg_status_t status;
    uint64_t start;

    if (session == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    start = hdrg_clock_ns();
    status = hdrg_session_receive(session, out, NULL);
    hdrg_session_clock(session, start);

    if (status == HDRG_OK) {
        session->received++;
        hdrg_session_late(session, out);
    }

    return status;
}

extern void hdrg_session_size(const hdrg_session_t *session, uint32_t *width, uint32_t *height)
{
    if (width != NULL) {
        *width = (session != NULL) ? session->pipeline.width : 0u;
    }

    if (height != NULL) {
        *height = (session != NULL) ? session->pipeline.height : 0u;
    }
}

extern float hdrg_session_peak(const hdrg_session_t *session)
{
    if (session == NULL) {
        return 0.0f;
    }

    return hdrg_render_peak_bound(&session->pipeline.caps);
}

/*
 * WINDOWED: a frame's cut reads the HDRG_SCENE_REACH frames before it;
 * its smoothed statistics the cuts and frames past before it, and their APL
 * factors those past before them; a fade-out's hold the frame the reach back
 * at most. The flash limiter then reads the second before that (each frame's
 * change against the previous)
 */
extern uint32_t hdrg_session_preroll(const hdrg_session_t *session)
{
    const hdrg_session_config_t *config;
    uint32_t frames, ahead;
    bool flash;

    if (session == NULL) {
        return 0;
    }

    config = &session->config;
    flash = !config->flash_off && config->temporal != HDRG_TEMPORAL_STATIC;

    /* the STATS stage's frames behind (its temporal mode), else a WINDOWED's scene-cut test's */
    frames = (config->temporal == HDRG_TEMPORAL_WINDOWED) ? HDRG_SCENE_REACH : 0u;

    if (session->stats.created) {
        hdrg_stats_window(&session->stats, config->latency, &frames, &ahead);
    }

    frames = flash ? frames + session->flash_window + 1u : frames;

    /* out_rate: the frame before the earliest one too (the outputs between them), and the
       INTERP window's past before that */
    frames += (session->rate_q != 0) ? 1u + session->interp.past : 0u;

    /* the frames the PREFILTER and SCALE windows read before the earliest one exact */
    return frames + session->pipeline.delay.chain_past + session->pipeline.delay.scale_past;
}

extern void hdrg_session_destroy(hdrg_session_t *session)
{
    hdrg_context_t *ctx;

    if (session == NULL) {
        return;
    }

    ctx = session->ctx;

    while (session->count > 0) {
        hdrg_session_pop(session);
    }

    hdrg_session_close(ctx, session);
    hdrg_recycle_destroy(&session->recycle);
    hdrg_stats_close(&session->stats);
    hdrg_options_destroy(session->stats_options);
    hdrg_mem_free(&ctx->alloc, session->judged);
    hdrg_mem_free(&ctx->alloc, session->ingest);
    hdrg_mem_free(&ctx->alloc, session->scratch);
    hdrg_mem_free(&ctx->alloc, session->yuv_scratch);
    hdrg_mem_free(&ctx->alloc, session->band);
    hdrg_mem_free(&ctx->alloc, session->gathered);
    hdrg_mem_free(&ctx->alloc, session->mosaic);
    hdrg_mem_free(&ctx->alloc, session->grain_windows);
    hdrg_mem_free(&ctx->alloc, session->export_luts);
    hdrg_mem_free(&ctx->alloc, session->export_frame);

    hdrg_mem_free(&ctx->alloc, session);
}
