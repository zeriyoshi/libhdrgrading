/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/video.h"
#include "pipeline.h"
#include "hdrgrading/foundation/fpenv.h"

/* the size^3 lattice of 16-bit code values, R fastest, then the pipeline's fixed rows over it */
static hdrg_status_t hdrg_lut3d_render(hdrg_context_t *ctx, const hdrg_color_t *color,
                                       const hdrg_params_t *params, const hdrg_display_caps_t *caps,
                                       uint16_t size, uint16_t *lattice, uint16_t *out)
{
    const size_t nodes = (size_t)size * size * size;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_pipeline_t pipeline;
    hdrg_status_t status;
    uint32_t height, y0, step[3];
    uint8_t c;
    size_t k;

    for (k = 0; k < nodes; k++) {
        step[0] = (uint32_t)(k % size);
        step[1] = (uint32_t)((k / size) % size);
        step[2] = (uint32_t)(k / ((size_t)size * size));

        for (c = 0; c < 3u; c++) {
            lattice[k * 3u + c] =
                (uint16_t)((step[c] * 65535u + (size - 1u) / 2u) / (uint32_t)(size - 1u));
        }
    }

    height = (uint32_t)size * size;
    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)lattice;
    raw.width = size;
    raw.height = height;
    raw.bit_depth = 16;
    raw.stride = (size_t)size * 6u;

    hdrg_encode_params_init(&encode);
    encode.output_mode = HDRG_OUTPUT_PQ;

    status = hdrg_pipeline_open(ctx, &raw, color, params, &encode, caps, 0, HDRG_LATENCY_OFFLINE,
                                &pipeline);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_pipeline_output(ctx, &pipeline, 16, false);

    if (status == HDRG_OK) {
        hdrg_pipeline_fixed(&pipeline);
        pipeline.src.pixels = raw.pixels;

        status = hdrg_pipeline_bind(ctx, &pipeline);
    }

    for (y0 = 0; status == HDRG_OK && y0 < height; y0 += HDRG_APPLY_BAND_ROWS) {
        status = hdrg_pipeline_rows(ctx, &pipeline, y0, HDRG_MIN(HDRG_APPLY_BAND_ROWS, height - y0),
                                    (uint8_t *)(void *)(out + (size_t)y0 * size * 3u));
    }

    hdrg_pipeline_close(ctx, &pipeline);

    return status;
}

extern hdrg_status_t hdrg_lut3d_export(hdrg_context_t *ctx, uint8_t primaries, uint8_t transfer,
                                       const hdrg_params_t *params, const hdrg_display_caps_t *caps,
                                       uint16_t size, float *rgb)
{
    hdrg_params_t effective;
    hdrg_display_caps_t display;
    hdrg_color_t color;
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;
    uint16_t *lattice, *out;
    size_t samples, i;

    if (ctx == NULL || rgb == NULL || size < 2u || size > HDRG_LUT3D_MAX_SIZE ||
        !hdrg_params_resolve(params, &effective) || !hdrg_display_resolve(caps, &display)) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* the session's single-layer PQ at the source size, the color mapping alone (no PREFILTER) */
    if (hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_SCALE) != NULL ||
        hdrg_host_stage_filter(&ctx->host, HDRG_STAGE_PREFILTER) != NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    effective.grain_policy = HDRG_GRAIN_PRESERVE;
    effective.deblock_strength = 0.0f;
    effective.dering_strength = 0.0f;
    effective.defringe_strength = 0.0f;
    effective.deband_strength = 0.0f;
    effective.mctf_strength = 0.0f;

    if (!hdrg_color_from_codes(primaries, transfer, &color)) {
        return HDRG_ERR_INVALID_ARG;
    }

    samples = (size_t)size * size * size * 3u;
    lattice = (uint16_t *)hdrg_mem_alloc(&ctx->alloc, samples * sizeof(uint16_t));
    out = (uint16_t *)hdrg_mem_alloc(&ctx->alloc, samples * sizeof(uint16_t));
    status = (lattice != NULL && out != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;

    if (status == HDRG_OK) {
        hdrg_fpenv_enter(&fpenv);
        status = hdrg_lut3d_render(ctx, &color, &effective, &display, size, lattice, out);
        hdrg_fpenv_leave(&fpenv);
    }

    for (i = 0; status == HDRG_OK && i < samples; i++) {
        rgb[i] = (float)out[i] / 65535.0f;
    }

    hdrg_color_free(&ctx->alloc, &color);
    hdrg_mem_free(&ctx->alloc, lattice);
    hdrg_mem_free(&ctx->alloc, out);

    return status;
}
