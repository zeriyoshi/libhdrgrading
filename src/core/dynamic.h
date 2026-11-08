/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_DYNAMIC_H
#define HDRG_CORE_DYNAMIC_H

#include <stdint.h>

#include "apply.h"
#include "hdrgrading/metadata.h"

/*
 * The dynamic metadata of an output from the light its apply pass measured
 * (hdrg_lightstat_t): HDR10+ (a session's frames) and the ST 2094-50 gain
 * curve back to the SDR (a session's frames, a still's HAGC). Their T.35
 * payloads (hdrg_hdr10plus_t35, hdrg_st2094_50_t35 of metadata.h) are
 * written here too: the quantization's rules in one place.
 */

/*
 * HDR10+ of light: avg_nits its mean, each percentile between its bin's
 * bounds, at most max_nits (none measured: not valid)
 */
extern void hdrg_dynamic_hdr10plus(const hdrg_lightstat_t *light, float avg_nits, float max_nits,
                                   hdrg_hdr10plus_t *out);
/*
 * The ST 2094-50 gain curve of a rendition (baseline: its headroom, stops;
 * gamut: its HDRG_GAMUT_*) back to the SDR of white cd/m² from its light: a
 * point for each HDRG_DYNAMIC_POINT_BINS bins of enough samples, at their
 * mean level (their places in their bins) over the reference white and
 * their mean log2 ratio, smoothed [1 2 1] with the points of the bins next
 * to them, held within -6 and 0 stops and raised so the output it gives
 * never falls (none measured, no headroom: not valid)
 */
extern void hdrg_dynamic_gain_curve(const hdrg_lightstat_t *light, float white, uint8_t gamut,
                                    float baseline, hdrg_st2094_50_t *curve);

#endif /* HDRG_CORE_DYNAMIC_H */
