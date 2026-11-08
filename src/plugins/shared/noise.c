/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/plugins/noise.h"

#define HDRG_NOISE_TOP       65535.0
#define HDRG_NOISE_MAX(a, b) ((a) > (b) ? (a) : (b))

extern hdrg_status_t hdrg_denoise_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                           double strength, double *sigma,
                                           double level[HDRG_DENOISE_BINS], double chroma_sigma[2],
                                           bool *active)
{
    hdrg_status_t status;
    uint32_t version, i;
    size_t needed;
    float noise[HDRG_SD_NOISE_FLOATS];

    *sigma = 0.0;
    chroma_sigma[0] = 0.0;
    chroma_sigma[1] = 0.0;
    *active = false;

    for (i = 0; i < HDRG_DENOISE_BINS; i++) {
        level[i] = -1.0;
    }

    status = api->nego_side_data(nego, HDRG_SD_NOISE, &version, noise, sizeof(noise), &needed);

    if (status == HDRG_OK && version == HDRG_SD_NOISE_VERSION && needed == sizeof(noise) &&
        noise[0] >= 0.0f) {
        *sigma = (double)noise[0] * HDRG_NOISE_TOP;

        for (i = 0; i < HDRG_DENOISE_BINS; i++) {
            level[i] = (noise[1u + i] < 0.0f) ? -1.0 : (double)noise[1u + i] * HDRG_NOISE_TOP;
        }

        chroma_sigma[0] = HDRG_NOISE_MAX((double)noise[9], 0.0) * HDRG_NOISE_TOP;
        chroma_sigma[1] = HDRG_NOISE_MAX((double)noise[10], 0.0) * HDRG_NOISE_TOP;
        *active =
            strength > 0.0 && (*sigma > 0.0 || chroma_sigma[0] > 0.0 || chroma_sigma[1] > 0.0);
    } else if (status != HDRG_OK && status != HDRG_ERR_NOT_FOUND) {
        return status;
    }

    return HDRG_OK;
}

extern double hdrg_denoise_level(const double level[HDRG_DENOISE_BINS], double sigma, double y)
{
    uint32_t i, lo, hi;
    double pos, t;

    pos = y * (double)HDRG_DENOISE_BINS - 0.5;
    lo = UINT32_MAX;
    hi = UINT32_MAX;

    for (i = 0; i < HDRG_DENOISE_BINS; i++) {
        if (level[i] < 0.0) {
            continue;
        }

        if ((double)i <= pos) {
            lo = i;
        }

        if ((double)i >= pos && hi == UINT32_MAX) {
            hi = i;
        }
    }

    if (lo == UINT32_MAX && hi == UINT32_MAX) {
        return sigma;
    }

    if (lo == UINT32_MAX || hi == UINT32_MAX || lo == hi) {
        return level[(lo == UINT32_MAX) ? hi : lo];
    }

    t = (pos - (double)lo) / (double)(hi - lo);

    return level[lo] * (1.0 - t) + level[hi] * t;
}
