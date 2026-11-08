/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

/*
 * capping: the bound plan's boost grid for the frame between frames a and b
 * from the INTERP stage's motion (hdrg_motion_cap) into session->capped;
 * *capped whether any cell is lower
 */
static hdrg_status_t hdrg_session_cap(hdrg_session_t *session, uint32_t a, uint32_t b,
                                      const uint8_t *motion, size_t size, bool *capped)
{
    const hdrg_plan_t *plan = &session->pipeline.plan, *pa, *pb;
    const size_t cells = (size_t)plan->grid_w * plan->grid_h;
    float *grown;

    *capped = false;
    pa = &session->pure[a % HDRG_SESSION_PURES];
    pb = &session->pure[b % HDRG_SESSION_PURES];

    if (plan->boost == NULL || plan->fixed || session->pure_index[a % HDRG_SESSION_PURES] != a ||
        session->pure_index[b % HDRG_SESSION_PURES] != b || pa->grid_w != plan->grid_w ||
        pa->grid_h != plan->grid_h || pb->grid_w != plan->grid_w || pb->grid_h != plan->grid_h) {
        return HDRG_OK;
    }

    if (cells > session->capped_cells) {
        grown = (float *)hdrg_mem_realloc(session->pipeline.frame_alloc, session->capped,
                                          cells * HDRG_PLAN_BINS * sizeof(float));
        if (grown == NULL) {
            return HDRG_ERR_NOMEM;
        }

        session->capped = grown;
        session->capped_cells = cells;
    }

    /* the motion of the frames the INTERP stage read: with SCALE, scaled */
    *capped = hdrg_motion_cap(
        plan, pa->boost, pb->boost, motion, size,
        (session->scaled != NULL) ? session->pipeline.width : session->pipeline.src.width,
        (session->scaled != NULL) ? session->pipeline.height : session->pipeline.src.height,
        session->capped);

    return HDRG_OK;
}

/*
 * out_rate with SCALE: the queued input frames scaled in order up to index
 * into the scaled ring by index (the frames no output reads, before the
 * first one's window, only taken by a SCALE keeping frames)
 */
static hdrg_status_t hdrg_session_scale_to(hdrg_session_t *session, uint32_t index)
{
    const hdrg_session_item_t *item;
    const size_t samples = (size_t)session->pipeline.width * session->pipeline.height * 3u;
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;
    uint32_t slot;
    uint16_t *out;

    while (session->scaled_next <= index) {
        if (session->scaled_next < session->queue[0].index ||
            session->scaled_next - session->queue[0].index >= session->count) {
            return HDRG_ERR_INTERNAL;
        }

        item = &session->queue[session->scaled_next - session->queue[0].index];
        slot = item->index % session->kept_frames;
        out = ((uint64_t)item->index + session->interp.past + 1u >= session->first_out)
                  ? session->scaled + (size_t)slot * samples
                  : NULL;
        pipeline->frame_index = item->index;
        pipeline->frame_pts = item->frame.pts;
        pipeline->ended = session->ended && (!pipeline->windowed || session->drained);

        status = hdrg_pipeline_scale_frame(session->ctx, pipeline, item->pixels, item->stride, out);
        if (status != HDRG_OK) {
            return status;
        }

        session->kept_index[slot] = (out != NULL) ? item->index : UINT32_MAX;
        session->kept_pts[slot] = item->frame.pts;
        session->kept_cut[slot] = item->cut ? 1u : 0u;
        session->scaled_next++;
    }

    return HDRG_OK;
}

/*
 * out_rate: the head item kept among the frames before the next ones: its
 * R'G'B' copied, or (with SCALE) scaled, if it was not
 */
static hdrg_status_t hdrg_session_keep(hdrg_session_t *session)
{
    const hdrg_session_item_t *item = &session->queue[0];
    const uint32_t slot = item->index % session->kept_frames;
    uint32_t y;
    uint8_t *frame;

    if (session->scaled != NULL) {
        return hdrg_session_scale_to(session, item->index);
    }

    frame = session->kept + (size_t)slot * session->row_bytes * session->pipeline.src.height;

    for (y = 0; y < session->pipeline.src.height; y++) {
        memcpy(frame + (size_t)y * session->row_bytes, item->pixels + (size_t)y * item->stride,
               session->row_bytes);
    }

    session->kept_index[slot] = item->index;
    session->kept_pts[slot] = item->frame.pts;
    session->kept_cut[slot] = item->cut ? 1u : 0u;

    return HDRG_OK;
}

/*
 * out_rate: input frame index's pixels (*pixels, rows of *stride bytes), pts
 * and scene cut: a kept one before the head or the queue's R'G'B', or with
 * SCALE the frame scaled (scaled first when queued); HDRG_ERR_NOT_FOUND:
 * neither kept nor queued
 */
static hdrg_status_t hdrg_session_frame_of(hdrg_session_t *session, uint32_t index,
                                           const uint8_t **pixels, size_t *stride, int64_t *pts,
                                           bool *cut)
{
    const hdrg_session_item_t *item;
    hdrg_status_t status;
    uint32_t slot;
    bool queued;

    queued = index >= session->queue[0].index && index - session->queue[0].index < session->count;
    slot = index % session->kept_frames;

    if (queued && session->scaled == NULL) {
        item = &session->queue[index - session->queue[0].index];
        *pixels = item->pixels;
        *stride = item->stride;
        *pts = item->frame.pts;
        *cut = item->cut;

        return HDRG_OK;
    }

    if (queued) {
        status = hdrg_session_scale_to(session, index);
        if (status != HDRG_OK) {
            return status;
        }
    }

    if (session->kept_index[slot] != index) {
        return HDRG_ERR_NOT_FOUND;
    }

    *pixels =
        (session->scaled != NULL)
            ? (const uint8_t *)(session->scaled + (size_t)slot * session->pipeline.width *
                                                      session->pipeline.height * 3u)
            : session->kept + (size_t)slot * session->row_bytes * session->pipeline.src.height;
    *stride = (session->scaled != NULL) ? (size_t)session->pipeline.width * 6u : session->row_bytes;
    *pts = session->kept_pts[slot];
    *cut = session->kept_cut[slot] != 0;

    return HDRG_OK;
}

/* out_rate: the phases of the outputs between input frames index and index + 1, ascending */
static uint32_t hdrg_session_phases(hdrg_session_t *session, uint32_t index)
{
    const uint64_t end = ((uint64_t)index + 1u) * session->rate_q;
    uint64_t j, part;
    uint32_t count;

    count = 0;

    for (j = (uint64_t)index * session->rate_q / session->rate_p + 1u;
         j * session->rate_p < end && count < session->phase_cap; j++) {
        part = j * session->rate_p % session->rate_q;
        session->phases[count++] =
            (uint32_t)HDRG_MIN((part * 65536u + session->rate_q / 2u) / session->rate_q, 65535u);
    }

    return count;
}

/*
 * out_rate: the INTERP job between input frames index and index + 1 at
 * phase: the frames of its window there are around them (kept or queued;
 * with SCALE, scaled) into sources and between (the shot's up to a cut,
 * the interval's phases), the frame configured in frame
 */
static hdrg_status_t hdrg_session_window(hdrg_session_t *session, uint32_t index, uint32_t phase,
                                         int64_t pts, hdrg_source_t *sources,
                                         hdrg_stage_between_t *between, hdrg_stage_frame_t *frame)
{
    const hdrg_source_t *src =
        (session->scaled != NULL) ? &session->pipeline.scaled : &session->pipeline.src;
    const uint8_t *at;
    hdrg_status_t status;
    uint32_t k;
    int64_t when;
    size_t stride;
    bool cuts[2u * HDRG_LIMIT_INTERP + 2u], cut;

    memset(between, 0, sizeof(*between));

    /* the frames before index and after index + 1 there are (the pair is) */
    status = HDRG_OK;

    while (status == HDRG_OK && between->past < session->interp.past && index > between->past) {
        status =
            hdrg_session_frame_of(session, index - between->past - 1u, &at, &stride, &when, &cut);
        between->past += (status == HDRG_OK) ? 1u : 0u;
    }

    status = (status == HDRG_ERR_NOT_FOUND) ? HDRG_OK : status;
    between->future = 1;

    while (status == HDRG_OK && between->future < session->interp.future) {
        status =
            hdrg_session_frame_of(session, index + between->future + 1u, &at, &stride, &when, &cut);
        between->future += (status == HDRG_OK) ? 1u : 0u;
    }

    status = (status == HDRG_ERR_NOT_FOUND) ? HDRG_OK : status;

    for (k = 0; status == HDRG_OK && k <= between->past + between->future; k++) {
        status = hdrg_session_frame_of(session, index - between->past + k, &at, &stride, &when,
                                       &cuts[k]);
        if (status != HDRG_OK) {
            break;
        }

        sources[k] = *src;
        sources[k].pixels = at;
        sources[k].stride = stride;
        sources[k].row0 = 0;
        between->frames[k] = &sources[k];
    }

    if (status != HDRG_OK) {
        return (status == HDRG_ERR_NOT_FOUND) ? HDRG_ERR_INTERNAL : status;
    }

    /* the shot: up to a cut (one at a frame: before it) */
    while (between->shot_past < between->past && !cuts[between->past - between->shot_past]) {
        between->shot_past++;
    }

    while (between->shot_future < between->future &&
           !cuts[between->past + between->shot_future + 1u]) {
        between->shot_future++;
    }

    between->phase = phase;
    between->phases = session->phases;
    between->phase_count = hdrg_session_phases(session, index);
    *frame = session->pipeline.told;
    frame->pts = pts;
    frame->index = index;
    frame->first = false;
    frame->bypass = false;
    frame->motion = hdrg_session_motion_of(session, index, &frame->motion_size);

    return HDRG_OK;
}

/*
 * out_rate: the INTERP stage's output (rgb, rows of rgb_stride bytes) in the
 * source's layout (with SCALE, or a 16-bit R'G'B' source: itself): *pixels
 * and *stride
 */
static void hdrg_session_layout(hdrg_session_t *session, const uint16_t *rgb, size_t rgb_stride,
                                const uint8_t **pixels, size_t *stride)
{
    const hdrg_source_t *src = &session->pipeline.src;
    const uint16_t *row;
    uint32_t x, y;
    uint8_t c, *dst;

    if (session->synth == NULL) {
        *pixels = (const uint8_t *)rgb;
        *stride = rgb_stride;

        return;
    }

    for (y = 0; y < src->height; y++) {
        row = (const uint16_t *)(const void *)((const uint8_t *)rgb + (size_t)y * rgb_stride);
        dst = session->synth + (size_t)y * session->row_bytes;

        for (x = 0; x < src->width; x++) {
            for (c = 0; c < src->comps; c++) {
                if (src->wide) {
                    ((uint16_t *)(void *)dst)[(size_t)x * src->comps + c] =
                        (c < 3u) ? row[(size_t)x * 3u + c] : UINT16_MAX;
                } else {
                    dst[(size_t)x * src->comps + c] =
                        (uint8_t)((c < 3u) ? (row[(size_t)x * 3u + c] + 128u) / 257u : UINT8_MAX);
                }
            }
        }
    }

    *pixels = session->synth;
    *stride = session->row_bytes;
}

/*
 * out_rate, INTERP in flight: the jobs of the outputs from the next one
 * not submitted, in order, while the stage has room and their frames are
 * queued (an output at an input frame, without a frame after, across a cut
 * or before the frames sent has none): each the window it would have
 * alone
 */
static hdrg_status_t hdrg_session_ahead(hdrg_session_t *session)
{
    const uint8_t *at;
    hdrg_source_t sources[2u * HDRG_LIMIT_INTERP + 2u];
    hdrg_stage_between_t between;
    hdrg_stage_frame_t frame;
    hdrg_status_t status;
    uint64_t j, whole, part, last;
    int64_t pts_a, when;
    size_t stride;
    bool cut;

    last = (uint64_t)session->queue[0].index + session->count;
    status = HDRG_OK;

    for (j = HDRG_MAX(session->interp_scan, session->out_next);
         status == HDRG_OK && session->interp_count < session->interp.max_inflight; j++) {
        whole = j * session->rate_p / session->rate_q;
        part = j * session->rate_p % session->rate_q;

        /*
         * the frames up to its window's future queued (or the input ended);
         * none after the last frame
         */
        if ((!session->ended && whole + session->interp.future >= last) || whole + 1u >= last) {
            break;
        }

        session->interp_scan = j + 1u;

        if (part == 0 ||
            hdrg_session_frame_of(session, (uint32_t)whole + 1u, &at, &stride, &when, &cut) !=
                HDRG_OK ||
            cut) {
            continue;
        }

        status = hdrg_session_frame_of(session, (uint32_t)whole, &at, &stride, &pts_a, &cut);
        if (status == HDRG_ERR_NOT_FOUND) {
            status = HDRG_OK;

            continue;
        }

        if (status == HDRG_OK) {
            status = hdrg_session_window(
                session, (uint32_t)whole,
                (uint32_t)HDRG_MIN((part * 65536u + session->rate_q / 2u) / session->rate_q,
                                   65535u),
                pts_a, sources, &between, &frame);
        }

        if (status == HDRG_OK) {
            status = hdrg_stage_interp_submit(&session->interp, &session->pipeline.exec, &between,
                                              &frame, &session->ctx->host);
        }

        if (status == HDRG_OK) {
            session->interp_outs[session->interp_count++] = j;
        }
    }

    return status;
}

/*
 * out_rate: the frame at phase between input frames index and index + 1
 * from the INTERP stage (in flight: its job landed, the earlier ones
 * dropped), in the source's layout (with SCALE: the scaled frames' size,
 * packed RGB16): *pixels and *stride, and the HDRG_SD_MOTION the stage gave
 * (NULL: none)
 */
static hdrg_status_t hdrg_session_between(hdrg_session_t *session, uint32_t index, uint32_t phase,
                                          int64_t pts, const uint8_t **pixels, size_t *stride,
                                          const uint8_t **motion, size_t *motion_size)
{
    const uint16_t *rgb;
    hdrg_source_t sources[2u * HDRG_LIMIT_INTERP + 2u];
    hdrg_stage_between_t between;
    hdrg_stage_frame_t frame;
    hdrg_status_t status;
    uint64_t t;
    uint32_t k;
    size_t rgb_stride;

    t = hdrg_clock_ns();
    status = session->interp_ahead ? hdrg_session_ahead(session) : HDRG_OK;

    /* in flight: the outputs before this one dropped, this one's job landed */
    while (status == HDRG_OK && session->interp_ahead && session->interp_count > 0 &&
           session->interp_outs[0] < session->out_next) {
        status =
            hdrg_stage_interp_land(&session->interp, &session->pipeline.exec, &session->ctx->host,
                                   &rgb, &rgb_stride, motion, motion_size);

        for (k = 1; k < session->interp_count; k++) {
            session->interp_outs[k - 1u] = session->interp_outs[k];
        }

        session->interp_count--;
    }

    if (status == HDRG_OK && session->interp_ahead) {
        status = (session->interp_count > 0 && session->interp_outs[0] == session->out_next)
                     ? hdrg_stage_interp_land(&session->interp, &session->pipeline.exec,
                                              &session->ctx->host, &rgb, &rgb_stride, motion,
                                              motion_size)
                     : HDRG_ERR_INTERNAL;

        for (k = 1; status == HDRG_OK && k < session->interp_count; k++) {
            session->interp_outs[k - 1u] = session->interp_outs[k];
        }

        session->interp_count -= (status == HDRG_OK) ? 1u : 0u;
    } else if (status == HDRG_OK) {
        status = hdrg_session_window(session, index, phase, pts, sources, &between, &frame);

        if (status == HDRG_OK) {
            status = hdrg_stage_interp(&session->interp, &session->pipeline.exec, &between, &frame,
                                       &session->ctx->host, &rgb, &rgb_stride, motion, motion_size);
        }
    }

    (void)hdrg_session_part(session, HDRG_PART_INTERP, t);

    if (status != HDRG_OK) {
        return status;
    }

    hdrg_session_layout(session, rgb, rgb_stride, pixels, stride);

    return HDRG_OK;
}

extern bool hdrg_session_store_motion(hdrg_session_t *session, uint32_t index, const void *motion,
                                      size_t size)
{
    const uint32_t slot = index % HDRG_SESSION_MOTIONS;
    uint8_t *grown;

    session->sd_motion_index[slot] = index;
    session->sd_motion_size[slot] = 0;

    if (motion == NULL) {
        return true;
    }

    if (size > session->sd_motion_cap[slot]) {
        grown = (uint8_t *)hdrg_mem_realloc(session->pipeline.frame_alloc, session->sd_motion[slot],
                                            size);
        if (grown == NULL) {
            session->sd_motion_index[slot] = UINT32_MAX;

            return false;
        }

        session->sd_motion[slot] = grown;
        session->sd_motion_cap[slot] = size;
    }

    memcpy(session->sd_motion[slot], motion, size);
    session->sd_motion_size[slot] = size;

    return true;
}

extern const uint8_t *hdrg_session_motion_of(const hdrg_session_t *session, uint32_t index,
                                             size_t *size)
{
    const uint32_t slot = index % HDRG_SESSION_MOTIONS;

    *size = 0;

    if (session->sd_motion_index[slot] != index || session->sd_motion_size[slot] == 0) {
        return NULL;
    }

    *size = session->sd_motion_size[slot];

    return session->sd_motion[slot];
}

extern hdrg_status_t hdrg_session_pure(hdrg_session_t *session, const hdrg_session_item_t *item)
{
    const uint32_t slot = item->index % HDRG_SESSION_PURES;
    const uint16_t *given = session->pipeline.given;
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;

    if (session->pure_index[slot] == item->index) {
        return HDRG_OK;
    }

    hdrg_plan_free(pipeline->frame_alloc, &session->pure[slot]);
    session->pure_index[slot] = UINT32_MAX;
    pipeline->src.stride = item->stride;
    pipeline->given = item->given;

    session->stats_slot = item->slot;
    status = hdrg_pipeline_grid(session->ctx, pipeline, item->pixels, &item->analysis,
                                &session->pure[slot]);
    session->stats_slot = HDRG_GC_SLOTS;

    pipeline->given = given;

    if (status != HDRG_OK) {
        return status;
    }

    session->pure_index[slot] = item->index;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_pass(hdrg_session_t *session)
{
    hdrg_status_t status;

    status = (session->rate_q != 0) ? hdrg_session_keep(session) : HDRG_OK;
    hdrg_session_pop(session);

    return status;
}

extern hdrg_status_t hdrg_session_receive_rate(hdrg_session_t *session, const hdrg_yuv_t *yuv,
                                               hdrg_session_output_t *out)
{
    const hdrg_session_item_t *head, *next;
    const uint8_t *pixels, *motion, *kept;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint64_t at, whole, part;
    uint32_t nearest, phase, index_a, index_b;
    int64_t pts_a, pts_b;
    size_t stride, motion_size, kept_stride;
    bool between, has_a, has_b, capped, kept_cut;
    float *boost;

    for (;;) {
        /* the frames the PREFILTER chain still holds at the end, as a head is dropped */
        status = hdrg_session_drain(session);
        if (status != HDRG_OK) {
            return status;
        }

        if (session->count == 0) {
            return session->ended ? HDRG_EOF : HDRG_ERR_AGAIN;
        }

        if (!session->ended && session->count <= session->depth) {
            return HDRG_ERR_AGAIN;
        }

        head = &session->queue[0];
        next = (session->count > 1u) ? &session->queue[1] : NULL;
        at = session->out_next * session->rate_p;
        whole = at / session->rate_q;
        part = at % session->rate_q;
        nearest = (uint32_t)((2u * part >= session->rate_q) ? whole + 1u : whole);

        /* an output before the head (none: the outputs begin at the stream's start) */
        if (nearest < head->index) {
            session->out_next++;

            continue;
        }

        /* the head's outputs are out (a frame with none renders for the state); the last frame
           has none after it */
        if (nearest > head->index || (part != 0 && whole == head->index && next == NULL)) {
            if (session->bound != head->index) {
                hdrg_fpenv_enter(&fpenv);
                status = hdrg_session_render(session, yuv, HDRG_SESSION_STATE, &session->anchor);
                hdrg_fpenv_leave(&fpenv);

                if (status != HDRG_OK) {
                    return status;
                }
            }

            status = hdrg_session_pass(session);
            if (status != HDRG_OK) {
                return status;
            }

            continue;
        }

        break;
    }

    hdrg_fpenv_enter(&fpenv);
    status = HDRG_OK;

    if (session->bound != head->index) {
        status = hdrg_session_render(session, yuv, HDRG_SESSION_BIND, &session->anchor);
        session->bound = (status == HDRG_OK) ? head->index : UINT32_MAX;
    }

    between = part != 0 || whole != head->index;
    phase = (uint32_t)HDRG_MIN((part * 65536u + session->rate_q / 2u) / session->rate_q, 65535u);
    pixels = head->pixels;
    stride = head->stride;

    /* the pair around t: the frame before and the head, or the head and the next one */
    index_a = (whole < head->index) ? head->index - 1u : head->index;
    index_b = index_a + 1u;
    pts_a = head->frame.pts;
    has_a = false;

    if (status == HDRG_OK) {
        status = hdrg_session_frame_of(session, index_a, &kept, &kept_stride, &pts_a, &kept_cut);
        has_a = status == HDRG_OK;
        status = (status == HDRG_ERR_NOT_FOUND) ? HDRG_OK : status;
    }

    has_b = whole < head->index || next != NULL;
    pts_b = (whole < head->index) ? head->frame.pts : ((next != NULL) ? next->frame.pts : pts_a);
    motion = NULL;
    motion_size = 0;
    capped = false;
    boost = session->pipeline.plan.boost;

    if (status == HDRG_OK && between && has_a && has_b &&
        !((whole < head->index) ? head->cut : next->cut)) {
        status = hdrg_session_between(session, index_a, phase, pts_a, &pixels, &stride, &motion,
                                      &motion_size);
    }

    /* the stage's motion caps the bound plan's boost where it is unsure (the next one's planned) */
    if (status == HDRG_OK && motion != NULL && session->capping && whole >= head->index) {
        status = hdrg_session_pure(session, next);
    }

    if (status == HDRG_OK && motion != NULL && session->capping) {
        status = hdrg_session_cap(session, index_a, index_b, motion, motion_size, &capped);
    }

    if (status == HDRG_OK && capped) {
        session->pipeline.plan.boost = session->capped;
    }

    /* with SCALE the apply pass reads a frame scaled: the head's, or the one between */
    if (status == HDRG_OK && session->scaled != NULL && pixels == head->pixels) {
        status = hdrg_session_frame_of(session, head->index, &pixels, &stride, &pts_a, &kept_cut);
    }

    session->pipeline.scaled_given =
        (session->scaled != NULL) ? (const uint16_t *)(const void *)pixels : NULL;
    session->pipeline.scaled_given_stride = stride;

    /* the grain of the input frame, or of its place between two (the input frames' keep theirs) */
    if (status == HDRG_OK) {
        status = hdrg_session_apply(
            session, yuv, pixels, stride, &head->grain,
            between ? hdrg_grain_key_of(index_a, phase) : hdrg_grain_key_of(head->index, 0), out);
    }

    session->pipeline.plan.boost = boost;
    session->pipeline.scaled_given = NULL;

    hdrg_fpenv_leave(&fpenv);

    if (status != HDRG_OK) {
        return status;
    }

    out->pts = session->anchor.pts;
    out->scene_cut = session->anchor.scene_cut && !between;
    out->shed = session->anchor.shed;
    out->peak_nits = session->anchor.peak_nits;
    out->apl_nits = session->anchor.apl_nits;
    out->sdr_apl_nits = session->anchor.sdr_apl_nits;
    out->apl_scale = session->anchor.apl_scale;
    out->anchor_luma = session->anchor.anchor_luma;
    out->key_luma = session->anchor.key_luma;
    out->anchor_used = session->anchor.anchor_used;
    out->apl_fit = session->anchor.apl_fit;
    out->noise_sigma = session->anchor.noise_sigma;
    out->blockiness = session->anchor.blockiness;
    out->ringing = session->anchor.ringing;
    out->flash_limited = session->anchor.flash_limited;
    out->fading = session->anchor.fading;
    out->synthetic = between;
    out->phase = between ? phase : 0u;
    out->src_pts[0] = between ? pts_a : head->frame.pts;
    out->src_pts[1] = between ? pts_b : head->frame.pts;
    out->motion = between ? motion : hdrg_session_motion_of(session, head->index, &motion_size);
    out->motion_size = (out->motion != NULL) ? motion_size : 0u;

    /* between two frames: its pts at its place between theirs */
    if (between) {
        out->pts = pts_a + hdrg_session_share(pts_b - pts_a, part, session->rate_q);
    }

    session->out_next++;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_session_rate_open(hdrg_context_t *ctx, hdrg_session_t *session)
{
    const hdrg_filter_entry_t *added;
    const hdrg_source_t *src = &session->pipeline.src;
    const uint64_t first = (uint64_t)session->config.start + session->config.preroll;
    const bool direct = src->wide && src->comps == 3u;
    hdrg_status_t status;
    uint64_t frame;
    uint32_t i, ahead;
    uint8_t k, at;
    bool cpu;

    added = hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_INTERP);
    status = HDRG_ERR_UNSUPPORTED;

    if (added != NULL) {
        session->interp_entry = *added;
        status = hdrg_stage_create(&ctx->alloc, &session->interp_entry, HDRG_APPLY_BAND_ROWS,
                                   &ctx->host, &session->interp);
    }

    /* the default: a GPU version on a GPU context (the session's kind first), the CPU's without */
    at = 0;

    while (added == NULL) {
        session->interp_entry.cls =
            hdrg_default_class(&ctx->host, HDRG_DEFAULT_INTERP, ctx->gpu != HDRG_GPU_OFF,
                               session->pipeline.gpu_first, &at, &cpu);
        if (session->interp_entry.cls == NULL) {
            break;
        }

        hdrg_options_destroy(session->interp_options);
        session->interp_options = NULL;

        status = hdrg_host_resolve_options(&ctx->host, session->interp_entry.cls, NULL,
                                           &session->interp_options);
        if (status != HDRG_OK) {
            return status;
        }

        session->interp_entry.options = session->interp_options;
        session->interp_entry.stage = HDRG_STAGE_INTERP;
        status = hdrg_stage_create(&ctx->alloc, &session->interp_entry, HDRG_APPLY_BAND_ROWS,
                                   &ctx->host, &session->interp);

        if (status != HDRG_ERR_UNSUPPORTED || cpu) {
            break;
        }
    }

    if (status != HDRG_OK) {
        return status;
    }

    /*
     * the frame before the head and the window's past kept, its future
     * queued. With SCALE the INTERP stage reads the frames scaled (scaled
     * apart, in order, from the past's to the future's: the head waits for
     * the future's SCALE outputs too) and its output is the apply's
     */
    session->row_bytes = (size_t)src->width * src->comps * (src->wide ? 2u : 1u);
    session->phase_cap = (uint32_t)HDRG_MIN(session->rate_q / session->rate_p + 2u, 65538u);
    session->phases =
        (uint32_t *)hdrg_mem_alloc(&ctx->alloc, (size_t)session->phase_cap * sizeof(uint32_t));
    session->depth = HDRG_MAX(session->depth, session->interp.future);
    session->bound = UINT32_MAX;

    /* INTERP in flight: its jobs submitted ahead, the head waiting for their intervals' frames */
    session->interp_ahead = (session->interp.cls->flags & HDRG_FILTER_ASYNC) != 0 &&
                            session->interp.max_inflight > 1u &&
                            session->config.latency != HDRG_LATENCY_LOW &&
                            session->phase_cap <= HDRG_STAGE_PHASES;
    ahead = session->interp_ahead
                ? (uint32_t)(((uint64_t)(session->interp.max_inflight - 1u) * session->rate_p +
                              session->rate_q - 1u) /
                             session->rate_q)
                : 0u;
    session->depth = HDRG_MAX(session->depth, session->interp.future + ahead);

    /* the jobs in flight read their windows in the stage's ring together: it spans them all */
    if (session->interp_ahead) {
        hdrg_stage_reserve(&session->interp,
                           session->interp.past + 2u + session->interp.future + ahead + 1u);
    }

    if (session->pipeline.scaling) {
        session->kept_frames = session->interp.past + 2u + session->interp.future + ahead;
        session->scaled_next = session->config.start;
        session->first_out = (uint32_t)first;
        session->pipeline.scale_apart = true;
        session->depth = HDRG_MAX(session->depth,
                                  session->interp.future + ahead + session->pipeline.delay.scale);
        hdrg_pipeline_scale_depth(&session->pipeline, session->depth);
    } else {
        session->kept_frames = session->interp.past + 1u;
    }

    /* (the queue holds the stream's window at most: refused before the frames are made) */
    if (session->depth > HDRG_SESSION_MAX_WINDOW) {
        return HDRG_ERR_UNSUPPORTED;
    }

    frame = session->pipeline.scaling ? (uint64_t)session->pipeline.width *
                                            session->pipeline.height * 3u * sizeof(uint16_t)
                                      : (uint64_t)session->row_bytes * src->height;
    if (frame > SIZE_MAX / (session->kept_frames + 1u)) {
        return HDRG_ERR_NOMEM;
    }

    if (session->pipeline.scaling) {
        session->scaled =
            (uint16_t *)hdrg_mem_alloc(&ctx->alloc, (size_t)frame * session->kept_frames);
    } else {
        session->kept =
            (uint8_t *)hdrg_mem_alloc(&ctx->alloc, (size_t)frame * session->kept_frames);
        session->synth = direct ? NULL : (uint8_t *)hdrg_mem_alloc(&ctx->alloc, (size_t)frame);
    }

    if (session->phases == NULL || (session->pipeline.scaling && session->scaled == NULL) ||
        (!session->pipeline.scaling &&
         (session->kept == NULL || (!direct && session->synth == NULL)))) {
        return HDRG_ERR_NOMEM;
    }

    for (i = 0; i < HDRG_SESSION_KEPT; i++) {
        session->kept_index[i] = UINT32_MAX;
    }

    /* a filter moving nothing (a repeat, a blend): its frames between need no cap */
    session->capping = session->config.temporal != HDRG_TEMPORAL_STATIC &&
                       (session->interp_entry.cls->flags & HDRG_FILTER_NO_MOTION) == 0;

    for (k = 0; k < HDRG_SESSION_PURES; k++) {
        session->pure_index[k] = UINT32_MAX;
    }

    for (i = 0; i < HDRG_SESSION_MOTIONS; i++) {
        session->sd_motion_index[i] = UINT32_MAX;
    }

    session->out_next =
        (first == 0)
            ? 0u
            : (2u * session->rate_q * first - session->rate_q + 2u * session->rate_p - 1u) /
                  (2u * session->rate_p);
    session->interp_scan = session->out_next;

    return HDRG_OK;
}
