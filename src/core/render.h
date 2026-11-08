/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_RENDER_H
#define HDRG_CORE_RENDER_H

#include <stdint.h>

#include "color.h"
#include "plan.h"

/*
 * Display-dependent rendering of a plan: the headroom the display (or the
 * file) leaves, the APL budget fitted for it, the rendition peak (the
 * shoulder saturates toward it), the gamut the rendition is encoded in and
 * how the output is encoded. A pixel's gain in stops is
 * diffuse + h * span, h its boost share on the plan's grid. Rebuilding it for
 * another display reuses the plan as is.
 */
typedef struct {
    float diffuse; /* stops of the diffuse level: the plan's x global_strength x apl_scale */
    float span;    /* stops a boost share of 1 adds: the headroom above the diffuse level */
    float full_diffuse, full_span; /* the same at an apl_scale of 1 */
    float sdr_white_nits;
    float peak_nits; /* the rendition ceiling (the display's or the format's, not the
                        content's: a frame's lights do not move the shoulder) */
    float top_nits;  /* the content's brightest: white x 2^(max_gain + lift), at most peak */
    float dim; /* a darkening frame's dimming (1 = as is): the shoulder's knee and peak and the
                  contrast's pivot follow it, so the output dims in proportion */
    float max_gain_stops; /* largest gain anywhere */
    float sdr_apl_nits;   /* source average luminance at sdr_white_nits */
    float apl_nits;       /* estimated rendered average luminance */
    float apl_scale;      /* common factor of the gain applied by the APL budget */
    float apl_fit;        /* the factor fitted to this frame alone (a session may smooth it) */
    float knee;           /* two-layer modes: SDR rendition tone-map knee */
    uint8_t gamut;        /* HDRG_GAMUT_* of the rendition */
    uint8_t mode;         /* hdrg_output_mode_t */
} hdrg_render_t;

/*
 * Still-image files: the viewer adapts the output to its display (gain-map
 * weighting, the OS tone mapping of PQ), so the render targets the display
 * given (NULL: the assumed viewing, hdrg_display_caps_init) within what the
 * file can carry: in GAINMAP mode the headroom at most
 * gainmap_headroom_cap_stops, the gamut at most Display P3, and sRGB when
 * the container cannot tag P3
 */
extern void hdrg_display_caps_for_file(const hdrg_display_caps_t *display,
                                       const hdrg_encode_params_t *encode, bool p3_container,
                                       hdrg_display_caps_t *out);

/* source = color of the source pixels; the gamut is hdrg_render_gamut's */
extern hdrg_status_t hdrg_render_build(const hdrg_allocator_t *alloc, const hdrg_plan_t *plan,
                                       const hdrg_params_t *params,
                                       const hdrg_encode_params_t *encode,
                                       const hdrg_display_caps_t *caps, const hdrg_color_t *source,
                                       hdrg_render_t *out);
/* The render of a fixed plan (STATIC): no APL budget */
extern void hdrg_render_fixed(const hdrg_plan_t *plan, const hdrg_params_t *params,
                              const hdrg_encode_params_t *encode, const hdrg_display_caps_t *caps,
                              const hdrg_color_t *source, hdrg_render_t *out);
/*
 * The render of the same plan with apl_scale = scale instead of the fitted
 * factor (a session's temporally smoothed one): the gain, the peak and the
 * estimated average luminance follow it
 */
extern hdrg_status_t hdrg_render_rescale(const hdrg_allocator_t *alloc, const hdrg_plan_t *plan,
                                         const hdrg_params_t *params,
                                         const hdrg_display_caps_t *caps, float scale,
                                         hdrg_render_t *render);
/*
 * HDRG_GAMUT_* of the rendition (hdrg_render_build): a BT.2020 container
 * when the display takes one (the content stays within Display P3); Display
 * P3 when the display carries it and the output is HDR-primary, the gamut
 * expands, or the source reaches beyond sRGB. It does not depend on the
 * frame, so the color LUT is built once for a sequence.
 */
extern uint8_t hdrg_render_gamut(const hdrg_params_t *params, const hdrg_encode_params_t *encode,
                                 const hdrg_display_caps_t *caps, const hdrg_color_t *source);
/*
 * The highest rendition peak caps allow any frame (a stream's mastering
 * display): the display's peak, else HDRG_PEAK_CAP_NITS, within its headroom
 */
extern float hdrg_render_peak_bound(const hdrg_display_caps_t *caps);

#endif /* HDRG_CORE_RENDER_H */
