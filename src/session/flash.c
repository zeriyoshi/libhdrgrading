/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "session.h"

/* a harmful change between two screen luminances (BT.1702-3 Annex 1, Guideline 1) */
static inline bool hdrg_flash_harmful(float a, float b)
{
    const float lo = HDRG_MIN(a, b), hi = HDRG_MAX(a, b);

    return (lo < HDRG_FLASH_KNEE) ? hi - lo >= HDRG_FLASH_DIFF
                                  : hi - lo > HDRG_FLASH_CONTRAST * (hi + lo);
}

/*
 * The screen luminance (cd/m^2) and the share of the frame of each region:
 * the means of the plan's cells rendered at gain scale (the APL model)
 */
static void hdrg_flash_regions(const hdrg_pipeline_t *pipeline, float scale, float *nits,
                               float *area)
{
    const hdrg_plan_t *plan = &pipeline->plan;
    const size_t cells = (size_t)plan->grid_w * plan->grid_h;
    const float white = pipeline->render.sdr_white_nits, peak = pipeline->render.peak_nits,
                total = (float)plan->width * (float)plan->height;
    uint32_t cx, cy, pixels;
    uint8_t r;
    size_t i;
    float v;

    memset(nits, 0, HDRG_FLASH_REGIONS * sizeof(float));
    memset(area, 0, HDRG_FLASH_REGIONS * sizeof(float));

    for (i = 0; i < cells; i++) {
        cx = (uint32_t)(i % plan->grid_w);
        cy = (uint32_t)(i / plan->grid_w);
        r = (uint8_t)((cy * HDRG_FLASH_REGIONS_Y / plan->grid_h) * HDRG_FLASH_REGIONS_X +
                      cx * HDRG_FLASH_REGIONS_X / plan->grid_w);
        pixels = hdrg_plan_cell_pixels(plan, i);
        v = white * plan->apl_y[i] *
            hdrg_fmath_exp2(HDRG_MAX(plan->apl_lift[i] +
                                         scale * (pipeline->render.full_diffuse +
                                                  plan->apl_boost[i] * pipeline->render.full_span),
                                     0.0f));
        nits[r] += HDRG_MIN(v, peak) * (float)pixels;
        area[r] += (float)pixels;
    }

    for (r = 0; r < HDRG_FLASH_REGIONS; r++) {
        nits[r] = (area[r] > 0.0f) ? nits[r] / area[r] : 0.0f;
        area[r] /= total;
    }
}

/* the frame's harmful change against ref over more than the area: +-1, 0 = none */
static int8_t hdrg_flash_dir(const float *ref, const float *nits, const float *area)
{
    uint8_t r;
    float up, down;

    up = 0.0f;
    down = 0.0f;

    for (r = 0; r < HDRG_FLASH_REGIONS; r++) {
        if (!hdrg_flash_harmful(ref[r], nits[r])) {
            continue;
        }

        if (nits[r] > ref[r]) {
            up += area[r];
        } else {
            down += area[r];
        }
    }

    if (up > HDRG_FLASH_AREA && up >= down) {
        return 1;
    }

    return (down > HDRG_FLASH_AREA) ? -1 : 0;
}

/* the changes within a second with dir next: each reversal of the direction counts once */
static uint32_t hdrg_flash_changes(const hdrg_session_t *session, int8_t dir)
{
    uint32_t i, n;
    int8_t last, d;

    n = 0;
    last = 0;

    for (i = 1; i < session->flash_window; i++) {
        d = session->flash_dirs[(session->flash_next + i) % session->flash_window];

        if (d != 0 && d != last) {
            n++;
            last = d;
        }
    }

    return (dir != 0 && dir != last) ? n + 1u : n;
}

extern hdrg_status_t hdrg_session_flash(hdrg_session_t *session, hdrg_session_output_t *out)
{
    const float *ref;
    hdrg_pipeline_t *pipeline = &session->pipeline;
    hdrg_status_t status;
    uint8_t step;
    int8_t dir;
    bool limit, source;
    float nits[HDRG_FLASH_REGIONS], zero[HDRG_FLASH_REGIONS], trial[HDRG_FLASH_REGIONS],
        area[HDRG_FLASH_REGIONS], lo, hi, mid;

    status = HDRG_OK;
    hi = pipeline->render.apl_scale;
    hdrg_flash_regions(pipeline, hi, nits, area);
    dir = session->flash_held ? hdrg_flash_dir(session->flash_prev, nits, area) : 0;
    limit = dir > 0 && hdrg_flash_changes(session, dir) > HDRG_FLASH_CHANGES;

    if (limit) {
        ref = session->flash_limited ? session->flash_zero : session->flash_prev;
        lo = 0.0f;
        hdrg_flash_regions(pipeline, lo, zero, area);

        /* the source's own rise (no gain) may already be harmful: then none */
        source = hdrg_flash_dir(ref, zero, area) > 0;

        for (step = 0; !source && step < HDRG_FLASH_STEPS; step++) {
            mid = 0.5f * (lo + hi);
            hdrg_flash_regions(pipeline, mid, trial, area);

            if (hdrg_flash_dir(ref, trial, area) <= 0) {
                lo = mid;
            } else {
                hi = mid;
            }
        }

        status = hdrg_render_rescale(pipeline->frame_alloc, &pipeline->plan, &pipeline->params,
                                     &pipeline->caps, lo, &pipeline->render);
        memcpy(session->flash_zero, zero, sizeof(session->flash_zero));
        out->flash_limited = true;
    }

    memcpy(session->flash_prev, nits, sizeof(session->flash_prev));
    session->flash_held = true;
    session->flash_limited = limit;
    session->flash_dirs[session->flash_next] = dir;
    session->flash_next = (session->flash_next + 1u) % session->flash_window;

    return status;
}
