/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "render.h"

#include <math.h>
#include <string.h>

#include "hdrgrading/foundation/math.h"

#define HDRG_APL_ITERATIONS 20u

/*
 * Rendition peak for content whose largest gain is stops (diffuse-white
 * units): the display's ceiling when known, else the content's own peak up
 * to HDRG_PEAK_CAP_NITS; never above the display headroom
 */
static float hdrg_render_peak(const hdrg_display_caps_t *caps, float white, float stops)
{
    float peak;

    peak = (caps->peak_nits > 0.0f) ? caps->peak_nits
                                    : HDRG_MIN(white * hdrg_math_exp2f(stops), HDRG_PEAK_CAP_NITS);

    if (caps->headroom_stops >= 0.0f) {
        peak = HDRG_MIN(peak, white * hdrg_math_exp2f(caps->headroom_stops));
    }

    return peak;
}

/* the plan's diffuse level and the headroom above it, at full strength and no APL scale */
static void hdrg_render_levels(const hdrg_plan_t *plan, const hdrg_params_t *params,
                               const hdrg_display_caps_t *caps, float white, float *diffuse,
                               float *span)
{
    const float headroom = hdrg_math_log2f(hdrg_render_peak(caps, white, 16.0f) / white);

    *diffuse = HDRG_MIN(plan->diffuse, HDRG_MAX(headroom, 0.0f)) * params->global_strength;
    *span = HDRG_MAX(headroom - plan->diffuse, 0.0f) * params->global_strength;
}

static float hdrg_apl_eval(const hdrg_plan_t *plan, const float *y_nits, const uint32_t *count,
                           uint64_t pixels, float peak, float diffuse, float span, float scale)
{
    const size_t cells = (size_t)plan->grid_w * plan->grid_h;
    size_t i;
    double sum;
    float v;

    sum = 0.0;
    for (i = 0; i < cells; i++) {
        v = y_nits[i] *
            hdrg_math_exp2f(
                HDRG_MAX(plan->apl_lift[i] + scale * (diffuse + plan->apl_boost[i] * span), 0.0f));
        sum += (double)HDRG_MIN(v, peak) * (double)count[i];
    }

    return (float)(sum / (double)pixels);
}

/* the gain at out->apl_scale: the diffuse level, the span, the largest gain and the peaks */
static void hdrg_render_scaled(const hdrg_plan_t *plan, const hdrg_display_caps_t *caps,
                               float white, float lift, float diffuse, float span,
                               hdrg_render_t *out)
{
    out->full_diffuse = diffuse;
    out->full_span = span;
    out->diffuse = diffuse * out->apl_scale;
    out->span = span * out->apl_scale;
    out->max_gain_stops = out->diffuse + plan->boost_max * out->span;

    /* the contrast slope lifts white by (0 - pivot) * (contrast - 1) stops beyond the gain */
    out->peak_nits = hdrg_render_peak(caps, white, 16.0f);
    out->top_nits = HDRG_MIN(white * hdrg_math_exp2f(out->max_gain_stops + lift), out->peak_nits);
}

extern void hdrg_display_caps_for_file(const hdrg_display_caps_t *display,
                                       const hdrg_encode_params_t *encode, bool p3_container,
                                       hdrg_display_caps_t *out)
{
    float white;

    if (display != NULL) {
        *out = *display;
    } else {
        hdrg_display_caps_init(out);
    }

    /*
     * GAINMAP: ISO 21496-1 viewers scale the whole up map by
     * w = display_headroom / declared_headroom, so the declared headroom is
     * capped (gainmap_headroom_cap_stops); a file for an unknown display
     * declares that much (its peak at the cap, not HDRG_PEAK_CAP_NITS: the
     * viewer scales it down where the display has less). HYBRID keeps the
     * PQ peak: its primary is the HDR rendition and must match the PQ look
     * for ICC-only viewers; the down map only has to recover SDR.
     */
    if (encode->output_mode == HDRG_OUTPUT_GAINMAP && encode->gainmap_headroom_cap_stops > 0.0f) {
        white = (out->sdr_white_nits > 0.0f) ? out->sdr_white_nits : HDRG_SDR_WHITE_NITS;
        out->headroom_stops =
            (out->headroom_stops >= 0.0f)
                ? HDRG_MIN(out->headroom_stops, encode->gainmap_headroom_cap_stops)
                : encode->gainmap_headroom_cap_stops;

        if (out->peak_nits <= 0.0f) {
            out->peak_nits = white * hdrg_math_exp2f(out->headroom_stops);
        }
    }

    out->gamut = p3_container ? HDRG_MIN(out->gamut, HDRG_GAMUT_P3) : HDRG_GAMUT_SRGB;
}

/*
 * APL budget: estimates the rendered average luminance per plan cell (mean
 * SDR luminance, contrast lift and the luminance-weighted boost share) and
 * finds by bisection the common gain factor that fits min(apl_budget_nits,
 * apl_peak_fraction x peak, max_full_frame_nits), the peak being the
 * display's (or the file's) ceiling. Evaluated sequentially in cell order,
 * so it does not depend on the thread count.
 */
extern hdrg_status_t hdrg_render_build(const hdrg_allocator_t *alloc, const hdrg_plan_t *plan,
                                       const hdrg_params_t *params,
                                       const hdrg_encode_params_t *encode,
                                       const hdrg_display_caps_t *caps, const hdrg_color_t *source,
                                       hdrg_render_t *out)
{
    const uint64_t pixels = (uint64_t)plan->width * plan->height;
    const size_t cells = (size_t)plan->grid_w * plan->grid_h;
    const float white = (caps->sdr_white_nits > 0.0f) ? caps->sdr_white_nits : HDRG_SDR_WHITE_NITS,
                lift = (params->contrast > 1.0f) ? -HDRG_GRADE_PIVOT * (params->contrast - 1.0f)
                                                 : 0.0f;
    uint32_t *count, iter;
    size_t i;
    float *y_nits, budget, peak, diffuse, span, lo, hi, mid;

    memset(out, 0, sizeof(*out));

    y_nits = (float *)hdrg_mem_alloc(alloc, cells * sizeof(float));
    count = (uint32_t *)hdrg_mem_alloc(alloc, cells * sizeof(uint32_t));
    if (y_nits == NULL || count == NULL) {
        hdrg_mem_free(alloc, y_nits);
        hdrg_mem_free(alloc, count);

        return HDRG_ERR_NOMEM;
    }

    out->sdr_white_nits = white;

    for (i = 0; i < cells; i++) {
        count[i] = hdrg_plan_cell_pixels(plan, i);
        y_nits[i] = white * plan->apl_y[i];
        out->sdr_apl_nits += y_nits[i] * (float)count[i];
    }

    out->sdr_apl_nits /= (float)pixels;

    hdrg_render_levels(plan, params, caps, white, &diffuse, &span);
    peak = hdrg_render_peak(caps, white, 16.0f);

    /* each limit is off at 0; the strictest applies */
    budget = (params->apl_budget_nits > 0.0f) ? params->apl_budget_nits : HUGE_VALF;

    if (params->apl_peak_fraction > 0.0f) {
        budget = HDRG_MIN(budget, params->apl_peak_fraction * peak);
    }

    if (caps->max_full_frame_nits > 0.0f) {
        budget = HDRG_MIN(budget, caps->max_full_frame_nits);
    }

    out->apl_scale = 1.0f;
    out->apl_nits = hdrg_apl_eval(plan, y_nits, count, pixels, peak, diffuse, span, 1.0f);

    if (out->apl_nits > budget) {
        lo = 0.0f;
        hi = 1.0f;
        for (iter = 0; iter < HDRG_APL_ITERATIONS; iter++) {
            mid = 0.5f * (lo + hi);
            if (hdrg_apl_eval(plan, y_nits, count, pixels, peak, diffuse, span, mid) > budget) {
                hi = mid;
            } else {
                lo = mid;
            }
        }

        out->apl_scale = lo;
        out->apl_nits = hdrg_apl_eval(plan, y_nits, count, pixels, peak, diffuse, span, lo);
    }

    hdrg_mem_free(alloc, y_nits);
    hdrg_mem_free(alloc, count);

    out->apl_fit = out->apl_scale;
    out->dim = 1.0f;
    hdrg_render_scaled(plan, caps, white, lift, diffuse, span, out);
    out->gamut = hdrg_render_gamut(params, encode, caps, source);
    out->knee = encode->tonemap_knee;
    out->mode = encode->output_mode;

    return HDRG_OK;
}

extern void hdrg_render_fixed(const hdrg_plan_t *plan, const hdrg_params_t *params,
                              const hdrg_encode_params_t *encode, const hdrg_display_caps_t *caps,
                              const hdrg_color_t *source, hdrg_render_t *out)
{
    const float white = (caps->sdr_white_nits > 0.0f) ? caps->sdr_white_nits : HDRG_SDR_WHITE_NITS,
                lift = (params->contrast > 1.0f) ? -HDRG_GRADE_PIVOT * (params->contrast - 1.0f)
                                                 : 0.0f;
    float diffuse, span;

    memset(out, 0, sizeof(*out));

    out->sdr_white_nits = white;
    out->apl_scale = 1.0f;
    out->apl_fit = 1.0f;
    out->dim = 1.0f;
    hdrg_render_levels(plan, params, caps, white, &diffuse, &span);
    hdrg_render_scaled(plan, caps, white, lift, diffuse, span, out);
    out->gamut = hdrg_render_gamut(params, encode, caps, source);
    out->knee = encode->tonemap_knee;
    out->mode = encode->output_mode;
}

extern hdrg_status_t hdrg_render_rescale(const hdrg_allocator_t *alloc, const hdrg_plan_t *plan,
                                         const hdrg_params_t *params,
                                         const hdrg_display_caps_t *caps, float scale,
                                         hdrg_render_t *render)
{
    const uint64_t pixels = (uint64_t)plan->width * plan->height;
    const size_t cells = (size_t)plan->grid_w * plan->grid_h;
    const float white = render->sdr_white_nits,
                lift = (params->contrast > 1.0f) ? -HDRG_GRADE_PIVOT * (params->contrast - 1.0f)
                                                 : 0.0f;
    uint32_t *count;
    size_t i;
    float *y_nits, diffuse, span;

    y_nits = (float *)hdrg_mem_alloc(alloc, cells * sizeof(float));
    count = (uint32_t *)hdrg_mem_alloc(alloc, cells * sizeof(uint32_t));
    if (y_nits == NULL || count == NULL) {
        hdrg_mem_free(alloc, y_nits);
        hdrg_mem_free(alloc, count);

        return HDRG_ERR_NOMEM;
    }

    for (i = 0; i < cells; i++) {
        count[i] = hdrg_plan_cell_pixels(plan, i);
        y_nits[i] = white * plan->apl_y[i];
    }

    hdrg_render_levels(plan, params, caps, white, &diffuse, &span);
    render->apl_scale = scale;
    render->apl_nits = hdrg_apl_eval(plan, y_nits, count, pixels,
                                     hdrg_render_peak(caps, white, 16.0f), diffuse, span, scale);

    hdrg_mem_free(alloc, y_nits);
    hdrg_mem_free(alloc, count);

    hdrg_render_scaled(plan, caps, white, lift, diffuse, span, render);

    return HDRG_OK;
}

extern uint8_t hdrg_render_gamut(const hdrg_params_t *params, const hdrg_encode_params_t *encode,
                                 const hdrg_display_caps_t *caps, const hdrg_color_t *source)
{
    if (caps->gamut == HDRG_GAMUT_BT2020) {
        return HDRG_GAMUT_BT2020;
    }

    return (caps->gamut == HDRG_GAMUT_P3 &&
            (encode->output_mode != HDRG_OUTPUT_GAINMAP || params->expand_strength > 0.0f ||
             !hdrg_color_within(source, HDRG_GAMUT_SRGB)))
               ? HDRG_GAMUT_P3
               : HDRG_GAMUT_SRGB;
}

extern float hdrg_render_peak_bound(const hdrg_display_caps_t *caps)
{
    const float white = (caps->sdr_white_nits > 0.0f) ? caps->sdr_white_nits : HDRG_SDR_WHITE_NITS;

    return hdrg_render_peak(caps, white, 16.0f);
}
