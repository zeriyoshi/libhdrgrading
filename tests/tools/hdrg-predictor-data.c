/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * hdrg-predictor-data: the light predictor's training rows (scripts/predictor/train.py). Each
 * SDR image through the analysis of a still (PHOTO, QUALITY's 2 x 2 cells, the predictor off),
 * then per judged light a line of its core's box in source pixels and its features
 * (src/core/predict.h):
 *   # <path> <width> <height>
 *   <x0> <y0> <x1> <y1> f0 .. f9      (x1, y1 exclusive)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/analyze.h"
#include "core/color.h"
#include "core/image.h"
#include "core/predict.h"
#include "io/io.h"
#include "priv.h"

static uint8_t *pd_read(const char *path, size_t *size)
{
    FILE *fp;
    uint8_t *data;
    long length;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    data = NULL;

    if (fseek(fp, 0, SEEK_END) == 0 && (length = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0) {
        data = (uint8_t *)malloc((size_t)length);

        if (data != NULL && fread(data, 1, (size_t)length, fp) != (size_t)length) {
            free(data);
            data = NULL;
        }

        *size = (size_t)length;
    }

    (void)fclose(fp);

    return data;
}

/* one image's lights printed (false: it cannot be read) */
static bool pd_image(hdrg_context_t *ctx, const hdrg_params_t *params, const char *path)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    const float *f;
    hdrg_analyze_opts_t opts;
    hdrg_analysis_t analysis;
    hdrg_decoded_t decoded;
    hdrg_raw_image_t raw;
    hdrg_source_t src;
    hdrg_color_t color;
    hdrg_status_t status;
    hdrg_exec_t exec;
    uint32_t k, i, cell;
    uint8_t *file;
    size_t size;

    file = pd_read(path, &size);
    if (file == NULL) {
        return false;
    }

    memset(&analysis, 0, sizeof(analysis));
    memset(&src, 0, sizeof(src));
    memset(&color, 0, sizeof(color));
    hdrg_context_exec(ctx, &exec);
    status = hdrg_decode_any(alloc, &exec, file, size, UINT64_MAX, params->chroma_fit, &decoded);
    free(file);

    if (status != HDRG_OK) {
        return false;
    }

    memset(&raw, 0, sizeof(raw));
    raw.pixels = decoded.pixels;
    raw.width = decoded.width;
    raw.height = decoded.height;
    raw.bit_depth = decoded.bit_depth;
    raw.stride = decoded.stride;
    status = hdrg_color_copy(alloc, &decoded.color, &color);

    if (status == HDRG_OK) {
        hdrg_color_set_sdr_eotf(&color, params->sdr_eotf);
        status = hdrg_source_init(alloc, &raw, &color, &src);
    }

    memset(&opts, 0, sizeof(opts));
    opts.bright_scale = 1.0f;
    opts.cell_shift = HDRG_FINE_SHIFT;
    opts.colored = params->colored_lights;
    opts.lights = true;
    opts.features = true;

    if (status == HDRG_OK) {
        status = hdrg_analyze(alloc, &exec, &src, &opts, &analysis);
    }

    if (status == HDRG_OK) {
        cell = 1u << analysis.cell_shift;
        printf("# %s %u %u\n", path, raw.width, raw.height);

        for (k = 0; analysis.light_features != NULL && k < analysis.light_count; k++) {
            f = analysis.light_features + (size_t)k * HDRG_LIGHT_FEATURES;
            printf("%u %u %u %u", analysis.lights[k].x0 * cell, analysis.lights[k].y0 * cell,
                   (analysis.lights[k].x1 + 1u) * cell, (analysis.lights[k].y1 + 1u) * cell);

            for (i = 0; i < HDRG_LIGHT_FEATURES; i++) {
                printf(" %.9g", (double)f[i]);
            }

            printf("\n");
        }
    }

    hdrg_analysis_free(alloc, &analysis);
    hdrg_source_free(alloc, &src);
    hdrg_color_free(alloc, &color);
    hdrg_decoded_free(alloc, &decoded);

    return status == HDRG_OK;
}

int main(int argc, char **argv)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_params_t params;
    int32_t i, failed;

    if (argc < 2) {
        fprintf(stderr, "usage: hdrg-predictor-data <sdr image>...\n");

        return 2;
    }

    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_OFF;

    if (hdrg_context_create(&desc, &ctx) != HDRG_OK) {
        return 1;
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    failed = 0;

    for (i = 1; i < argc; i++) {
        if (!pd_image(ctx, &params, argv[i])) {
            fprintf(stderr, "%s: cannot be analyzed\n", argv[i]);
            failed++;
        }
    }

    hdrg_context_destroy(ctx);

    return (failed == 0) ? 0 : 1;
}
