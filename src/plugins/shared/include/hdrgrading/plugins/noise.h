/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_NOISE_H
#define HDRG_PLUGINS_NOISE_H

/*
 * The noise of a frame's HDRG_SD_NOISE as the denoisers read it: hdrg.denoise.spatial and its GPU
 * versions, hdrg.mctf
 */
#include <stdbool.h>

#include "hdrgrading/filter.h"

#define HDRG_DENOISE_BINS 8u /* HDRG_SD_NOISE's Y' noise levels */

/*
 * the noise for strength from the frame's HDRG_SD_NOISE in nego: the Y' level sigma, by level
 * in HDRG_DENOISE_BINS bins (< 0: no data) and the Cb / Cr levels in 16-bit codes, and whether a
 * denoiser works on the frame (false without the side data or noise)
 */
extern hdrg_status_t hdrg_denoise_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                           double strength, double *sigma,
                                           double level[HDRG_DENOISE_BINS], double chroma_sigma[2],
                                           bool *active);
/*
 * the Y' noise at level y in [0, 1] of hdrg_denoise_settings' level (16-bit codes): the bins with
 * data interpolated, held past the ends; sigma without any
 */
extern double hdrg_denoise_level(const double level[HDRG_DENOISE_BINS], double sigma, double y);

#endif /* HDRG_PLUGINS_NOISE_H */
