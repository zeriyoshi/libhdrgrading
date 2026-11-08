/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_PREDICT_H
#define HDRG_CORE_PREDICT_H

#include <stdint.h>

#include "analyze.h"

/*
 * The judged lights checked by an ANALYZE class (hdrg_params_t.light_predictor > 0; the built-in
 * hdrg.analyze.predict, src/plugins/predict): each light's record (its core's box, its boost and
 * its features from the cells of its core and its surround and from the frame), the class's
 * chance for it, and its boost (the light list's and its cells') scaled by (1 - amount) + amount
 * x chance: the check lowers a doubtful light, never raises one, so the rules' judgment stays
 * the bound.
 */

/*
 * light k's features (HDRG_LIGHT_FEATURES, hdrg_light_record_t's) from the cells' planes
 * (hdrg_analyze's: mean La, brightest La, brightest channel, mean Y, chroma; qw x qh) and the
 * frame's statistics
 */
extern void hdrg_predict_features(const hdrg_analysis_t *analysis, const float *cells, uint32_t qw,
                                  uint32_t qh, uint32_t k, float out[HDRG_LIGHT_FEATURES]);
/*
 * every judged light's boost scaled by the chance opts->judge gives it (opts->predictor: the
 * amount 0..1; the cells of 2^shift pixels of src), the light plane's cells with it (light_of);
 * the features into analysis->light_features when opts->features (else not kept)
 */
extern hdrg_status_t hdrg_predict_lights(const hdrg_allocator_t *alloc, hdrg_analysis_t *analysis,
                                         const hdrg_source_t *src, const float *cells, uint32_t qw,
                                         uint32_t qh, uint8_t shift,
                                         const hdrg_analyze_opts_t *opts);

#endif /* HDRG_CORE_PREDICT_H */
