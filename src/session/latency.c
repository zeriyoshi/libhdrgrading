/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

/* a part's time a frame sent, in milliseconds */
static float hdrg_session_part_ms(const hdrg_session_t *session, uint8_t part)
{
    return (session->taken > 0u)
               ? (float)((double)session->part_ns[part] / (double)session->taken / 1e6)
               : 0.0f;
}

/*
 * What to change for the part of the calls that took the most time
 * (HDRG_ADVICE_*), and more threads when the context has fewer than the
 * CPUs; under LOW, PLAYBACK when an ASYNC filter would keep frames in flight
 */
static uint32_t hdrg_session_advice(const hdrg_session_t *session)
{
    const hdrg_pipeline_t *pipeline = &session->pipeline;
    const uint64_t prefilter = session->part_ns[HDRG_PART_PREFILTER],
                   analyze = session->part_ns[HDRG_PART_ANALYZE],
                   apply = session->part_ns[HDRG_PART_APPLY];
    hdrg_pipeline_delay_t delay;
    uint32_t advice;

    if (prefilter >= analyze && prefilter >= apply) {
        advice = HDRG_ADVICE_PREFILTER;
    } else if (analyze >= apply) {
        advice = (pipeline->params.effort == HDRG_EFFORT_QUALITY) ? HDRG_ADVICE_EFFORT
                                                                  : HDRG_ADVICE_PROXY;
    } else {
        advice =
            (pipeline->width != pipeline->src.width || pipeline->height != pipeline->src.height)
                ? HDRG_ADVICE_SCALE
                : 0u;
    }

    if (hdrg_exec_concurrency(&pipeline->exec) < hdrg_thread_cpu_count()) {
        advice |= HDRG_ADVICE_THREADS;
    }

    hdrg_pipeline_delay(pipeline, UINT32_MAX, &delay);

    if (session->config.latency == HDRG_LATENCY_LOW &&
        (delay.chain_flight > 0u || delay.scale_flight > 0u)) {
        advice |= HDRG_ADVICE_LATENCY;
    }

    return advice;
}

/*
 * The frames a frame's output waits for under profile with the session's filters and
 * settings, by the rules hdrg_session_open and hdrg_session_rate_open set the depth with:
 * the PREFILTER chain's delay, then the deepest of the statistics window's future (LOW:
 * none), the lights' long steadying (OFFLINE), the SCALE stage and the INTERP window with
 * its jobs ahead. The session's own profile takes its flights as capped (LOW, frames kept on
 * the GPU): hdrg_session_latency
 */
static uint32_t hdrg_session_delay_of(const hdrg_session_t *session, uint8_t profile)
{
    const hdrg_session_config_t *config = &session->config;
    const bool low = profile == HDRG_LATENCY_LOW;
    hdrg_pipeline_delay_t delay;
    uint32_t depth, ahead, past, future;

    hdrg_pipeline_delay(&session->pipeline,
                        (profile == config->latency) ? session->pipeline.flight_cap
                                                     : (low ? 1u : UINT32_MAX),
                        &delay);
    depth = HDRG_MAX(low ? 0u : config->future, delay.scale);

    /* the STATS stage's frames ahead under the profile (OFFLINE: the lights steadied by place) */
    hdrg_stats_window(&session->stats, profile, &past, &future);
    depth = HDRG_MAX(depth, future);

    /* out_rate: the INTERP window's future, the jobs ahead in flight (not LOW) and the SCALE */
    if (session->rate_q != 0 && session->interp.cls != NULL) {
        ahead =
            ((session->interp.cls->flags & HDRG_FILTER_ASYNC) != 0 &&
             session->interp.max_inflight > 1u && !low && session->phase_cap <= HDRG_STAGE_PHASES)
                ? (uint32_t)(((uint64_t)(session->interp.max_inflight - 1u) * session->rate_p +
                              session->rate_q - 1u) /
                             session->rate_q)
                : 0u;
        depth = HDRG_MAX(depth, session->interp.future + ahead +
                                    (session->pipeline.scaling ? delay.scale : 0u));
    }

    return depth + delay.chain;
}

extern void hdrg_session_late(hdrg_session_t *session, hdrg_session_output_t *out)
{
    const uint64_t interval = (session->rate_q != 0)
                                  ? (uint64_t)(1e9 * (double)session->config.out_rate_den /
                                               (double)session->config.out_rate_num)
                                  : (uint64_t)(1e9 / (double)session->rate),
                   cost = session->work_ns - session->work_mark;
    const uint32_t window = session->flash_window;
    bool late;

    session->work_mark = session->work_ns;

    if (session->cost_count == window) {
        session->cost_sum -= session->costs[session->cost_next];
    } else {
        session->cost_count++;
    }

    session->costs[session->cost_next] = cost;
    session->cost_sum += cost;
    session->cost_next = (session->cost_next + 1u) % window;

    switch (session->config.latency) {
    case HDRG_LATENCY_PLAYBACK:
        late = session->cost_count == window && session->cost_sum > interval * window;

        break;
    case HDRG_LATENCY_LOW:
        late = session->received > 1u && cost > interval;

        break;
    default:
        late = false;

        break;
    }

    session->late = late;
    session->late_frames += late ? 1u : 0u;
    out->late = late;

    /* shed: the frames sent from now on pass the PREFILTER filters through */
    if (late && session->config.shed && !session->pipeline.bypass &&
        session->pipeline.prefilter_count > 0) {
        session->pipeline.bypass = true;
        session->shed_at = session->config.start + session->taken;
    }
}

extern uint32_t hdrg_session_latency(const hdrg_session_t *session)
{
    if (session == NULL) {
        return 0;
    }

    return session->depth + session->pipeline.delay.chain;
}

extern hdrg_status_t hdrg_session_latency_report(const hdrg_session_t *session,
                                                 hdrg_session_latency_t *out)
{
    const hdrg_pipeline_t *pipeline;
    uint32_t past, future;
    uint8_t profile;

    if (session == NULL || out == NULL || out->struct_size != sizeof(*out)) {
        return HDRG_ERR_INVALID_ARG;
    }

    pipeline = &session->pipeline;
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->profile = session->config.latency;
    out->frames = hdrg_session_latency(session);
    hdrg_stats_window(&session->stats, session->config.latency, &past, &future);
    out->window = HDRG_MAX(session->config.future, future);
    out->prefilter = pipeline->delay.chain - pipeline->delay.chain_flight;
    out->prefilter_flight = pipeline->delay.chain_flight;
    out->scale = pipeline->delay.scale - pipeline->delay.scale_flight;
    out->scale_flight = pipeline->delay.scale_flight;

    /* the same filters and window under each profile (LOW takes no window future) */
    for (profile = 0; profile < HDRG_LATENCY_PROFILES; profile++) {
        out->profiles[profile] = hdrg_session_delay_of(session, profile);
    }

    out->preroll = hdrg_session_preroll(session);
    out->sent = session->taken;
    out->received = session->received;
    out->rate = session->rate;
    out->frames_ms = (float)out->frames * 1000.0f / session->rate;
    out->work_ms = (session->taken > 0)
                       ? (float)((double)session->work_ns / (double)session->taken / 1e6)
                       : 0.0f;
    out->work_max_ms = (float)((double)session->work_max_ns / 1e6);
    out->total_ms = out->frames_ms + out->work_ms;
    out->interval_ms = 1000.0f / session->rate;
    out->convert_ms = hdrg_session_part_ms(session, HDRG_PART_CONVERT);
    out->prefilter_ms = hdrg_session_part_ms(session, HDRG_PART_PREFILTER);
    out->analyze_ms = hdrg_session_part_ms(session, HDRG_PART_ANALYZE);
    out->apply_ms = hdrg_session_part_ms(session, HDRG_PART_APPLY);
    out->interp_ms = hdrg_session_part_ms(session, HDRG_PART_INTERP);
    out->late_frames = session->late_frames;
    out->shed_at = session->shed_at;
    out->convert_device = session->gpu_convert ? hdrg_gpu_core_device(session->gpu) : 0u;
    out->apply_device = session->gpu_apply ? hdrg_gpu_core_device(session->gpu) : 0u;
    out->frames_on_gpu = session->resident;
    out->late = session->late;
    out->advice = (session->late_frames > 0u) ? hdrg_session_advice(session) : 0u;

    return HDRG_OK;
}
