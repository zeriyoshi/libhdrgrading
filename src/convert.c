/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "container.h"
#include "io/io.h"
#include "pipeline.h"
#include "host/codec_host.h"
#include "priv.h"
#include "hdrgrading/foundation/fpenv.h"

/* the three settings of a conversion, checked */
typedef struct {
    hdrg_params_t params;
    hdrg_display_caps_t display;
    hdrg_encode_params_t encode;
} hdrg_convert_settings_t;

/*
 * Physically normalizes EXIF orientation 2..8 by rewriting the pixel buffer,
 * then patches the matching EXIF tag to 1.
 */
static hdrg_status_t hdrg_normalize_orientation(const hdrg_allocator_t *alloc,
                                                hdrg_decoded_t *decoded)
{
    const uint32_t sample_size = decoded->bit_depth / 8u * 3u;
    const uint8_t *src;
    uint32_t x, y, sx, sy, dst_w, dst_h;
    uint8_t *rotated, *dst;
    size_t dst_stride;
    bool swap_axes;

    if (decoded->orientation <= 1) {
        return HDRG_OK;
    }

    swap_axes = decoded->orientation >= 5;
    dst_w = swap_axes ? decoded->height : decoded->width;
    dst_h = swap_axes ? decoded->width : decoded->height;
    dst_stride = (size_t)dst_w * sample_size;

    rotated = (uint8_t *)hdrg_mem_alloc(alloc, dst_stride * dst_h);
    if (rotated == NULL) {
        return HDRG_ERR_NOMEM;
    }

    for (y = 0; y < dst_h; y++) {
        dst = rotated + (size_t)y * dst_stride;
        for (x = 0; x < dst_w; x++) {
            switch (decoded->orientation) {
            case 2: /* mirror horizontal */
                sx = decoded->width - 1 - x;
                sy = y;

                break;
            case 3: /* rotate 180 */
                sx = decoded->width - 1 - x;
                sy = decoded->height - 1 - y;

                break;
            case 4: /* mirror vertical */
                sx = x;
                sy = decoded->height - 1 - y;

                break;
            case 5: /* transpose */
                sx = y;
                sy = x;

                break;
            case 6: /* rotate 90 cw */
                sx = y;
                sy = decoded->height - 1 - x;

                break;
            case 7: /* transverse */
                sx = decoded->width - 1 - y;
                sy = decoded->height - 1 - x;

                break;
            case 8: /* rotate 270 cw */
            default:
                sx = decoded->width - 1 - y;
                sy = x;

                break;
            }

            src = decoded->pixels + (size_t)sy * decoded->stride + (size_t)sx * sample_size;
            memcpy(dst + (size_t)x * sample_size, src, sample_size);
        }
    }

    hdrg_mem_free(alloc, decoded->pixels);

    decoded->pixels = rotated;
    decoded->width = dst_w;
    decoded->height = dst_h;
    decoded->stride = dst_stride;
    decoded->orientation = 1;

    if (decoded->exif != NULL && decoded->exif_size > 6) {
        (void)hdrg_exif_set_orientation(decoded->exif + 6, decoded->exif_size - 6, 1);
    }

    return HDRG_OK;
}

/* one still image: the core pipeline, written as the params' file form */
/* the settings given (NULL = the defaults) checked into out */
static bool hdrg_convert_resolve(const hdrg_params_t *params, const hdrg_display_caps_t *display,
                                 const hdrg_encode_params_t *encode, hdrg_convert_settings_t *out)
{
    if (!hdrg_params_resolve(params, &out->params) ||
        !hdrg_display_resolve(display, &out->display) ||
        !hdrg_encode_resolve(encode, &out->encode)) {
        return false;
    }

    /* a still image has no time budget, nor the frames around it to resynthesize grain in */
    if (out->params.effort == HDRG_EFFORT_AUTO) {
        out->params.effort = HDRG_EFFORT_QUALITY;
    }

    if (out->params.grain_policy == HDRG_GRAIN_AUTO) {
        out->params.grain_policy = HDRG_GRAIN_PRESERVE;
    }

    return true;
}

static hdrg_status_t hdrg_convert_frame(hdrg_context_t *ctx, const hdrg_raw_image_t *raw,
                                        const hdrg_color_t *color, const uint8_t *exif,
                                        size_t exif_size, const hdrg_convert_settings_t *settings,
                                        uint8_t **output, size_t *output_size)
{
    hdrg_container_t container;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_pipeline_t pipeline;
    hdrg_status_t status;

    /* the writer's form for HDRG_OUTPUT_AUTO, the threads its codec may use */
    encode = settings->encode;
    hdrg_context_exec(ctx, &ctx->host.exec);

    status = hdrg_container_select(ctx, &encode, &container);
    if (status != HDRG_OK) {
        return status;
    }

    hdrg_container_caps(&container, &settings->display, &encode, &caps);

    status = hdrg_pipeline_open(ctx, raw, color, &settings->params, &encode, &caps, 0,
                                HDRG_LATENCY_OFFLINE, &pipeline);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_container_prepare(&container, ctx, &pipeline, &encode);
    if (status == HDRG_OK) {
        status = hdrg_pipeline_frame(ctx, &pipeline, raw->pixels);
    }

    if (status == HDRG_OK) {
        status = hdrg_container_write(&container, ctx, &pipeline, &encode, exif, exif_size, output,
                                      output_size);
    }

    hdrg_pipeline_close(ctx, &pipeline);

    return status;
}

static hdrg_status_t hdrg_convert_pinned(hdrg_context_t *ctx, const uint8_t *input,
                                         size_t input_size, const hdrg_params_t *params,
                                         const hdrg_display_caps_t *display,
                                         const hdrg_encode_params_t *encode, uint8_t **output,
                                         size_t *output_size)
{
    hdrg_convert_settings_t settings;
    hdrg_raw_image_t raw;
    hdrg_decoded_t decoded;
    hdrg_status_t status;

    if (ctx == NULL || input == NULL || input_size == 0 || output == NULL || output_size == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *output = NULL;
    *output_size = 0;

    if (!hdrg_convert_resolve(params, display, encode, &settings)) {
        return HDRG_ERR_INVALID_ARG;
    }

    hdrg_context_exec(ctx, &ctx->host.exec);

    status = hdrg_codec_read(&ctx->host, input, input_size, ctx->max_pixels,
                             settings.params.chroma_fit, &decoded);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_normalize_orientation(&ctx->alloc, &decoded);
    if (status != HDRG_OK) {
        hdrg_decoded_free(&ctx->alloc, &decoded);

        return status;
    }

    memset(&raw, 0, sizeof(raw));
    raw.pixels = decoded.pixels;
    raw.width = decoded.width;
    raw.height = decoded.height;
    raw.bit_depth = decoded.bit_depth;
    raw.stride = decoded.stride;
    raw.has_alpha = false;

    status = hdrg_convert_frame(ctx, &raw, &decoded.color, decoded.exif, decoded.exif_size,
                                &settings, output, output_size);

    hdrg_decoded_free(&ctx->alloc, &decoded);

    return status;
}

static hdrg_status_t hdrg_convert_raw_pinned(hdrg_context_t *ctx, const hdrg_raw_image_t *image,
                                             const hdrg_params_t *params,
                                             const hdrg_display_caps_t *display,
                                             const hdrg_encode_params_t *encode, uint8_t **output,
                                             size_t *output_size)
{
    hdrg_convert_settings_t settings;
    hdrg_color_t color;

    if (ctx == NULL || image == NULL || image->pixels == NULL || image->width == 0 ||
        image->height == 0 || output == NULL || output_size == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if ((image->bit_depth != 8 && image->bit_depth != 16) ||
        !hdrg_color_from_codes(image->primaries, image->transfer, &color)) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* 16-bit samples are read as uint16_t: the pixels and the rows at 2 bytes */
    if (image->stride <
            (size_t)image->width * (image->has_alpha ? 4u : 3u) * (image->bit_depth / 8u) ||
        (image->bit_depth == 16 &&
         (((uintptr_t)image->pixels | (uintptr_t)image->stride) & 1u) != 0)) {
        return HDRG_ERR_INVALID_ARG;
    }

    *output = NULL;
    *output_size = 0;

    if (!hdrg_convert_resolve(params, display, encode, &settings)) {
        return HDRG_ERR_INVALID_ARG;
    }

    return hdrg_convert_frame(ctx, image, &color, NULL, 0, &settings, output, output_size);
}

extern hdrg_status_t hdrg_decode_any(const hdrg_allocator_t *alloc, const hdrg_exec_t *exec,
                                     const uint8_t *input, size_t input_size, uint64_t max_pixels,
                                     uint8_t fit, hdrg_decoded_t *decoded)
{
    hdrg_plugin_host_t host;
    hdrg_status_t status;
    uint32_t i;

    memset(decoded, 0, sizeof(*decoded));
    hdrg_host_init(&host, alloc, NULL);

    if (exec != NULL) {
        host.exec = *exec;
    }

    /* the built-in codecs' plugins (no filters' to read a file) */
    status = HDRG_OK;

    for (i = hdrg_builtin_codecs_first; i < hdrg_builtin_plugin_count && status == HDRG_OK; i++) {
        status =
            hdrg_host_register(&host, hdrg_builtin_plugins[i], NULL, HDRG_PLUGIN_ABI_VERSION, true);
    }

    if (status == HDRG_OK) {
        status = hdrg_codec_read(&host, input, input_size, max_pixels, fit, decoded);
    }

    hdrg_host_destroy(&host);

    return status;
}

extern hdrg_status_t hdrg_decode_with(hdrg_context_t *ctx, const hdrg_exec_t *exec,
                                      const uint8_t *input, size_t input_size, uint64_t max_pixels,
                                      uint8_t fit, hdrg_decoded_t *decoded)
{
    hdrg_exec_t kept;
    hdrg_status_t status;

    memset(decoded, 0, sizeof(*decoded));
    kept = ctx->host.exec;

    if (exec != NULL) {
        ctx->host.exec = *exec;
    }

    status = hdrg_codec_read(&ctx->host, input, input_size, max_pixels, fit, decoded);
    ctx->host.exec = kept;

    return status;
}

/*
 * Library work runs with a pinned floating-point environment (round to
 * nearest, no FTZ / DAZ), so the output does not depend on the caller's
 * state; pool threads pin theirs at start.
 */
extern hdrg_status_t hdrg_convert(hdrg_context_t *ctx, const uint8_t *input, size_t input_size,
                                  const hdrg_params_t *params, const hdrg_display_caps_t *display,
                                  const hdrg_encode_params_t *encode, uint8_t **output,
                                  size_t *output_size)
{
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;

    hdrg_fpenv_enter(&fpenv);

    status =
        hdrg_convert_pinned(ctx, input, input_size, params, display, encode, output, output_size);

    hdrg_fpenv_leave(&fpenv);

    return status;
}

extern hdrg_status_t hdrg_convert_raw(hdrg_context_t *ctx, const hdrg_raw_image_t *image,
                                      const hdrg_params_t *params,
                                      const hdrg_display_caps_t *display,
                                      const hdrg_encode_params_t *encode, uint8_t **output,
                                      size_t *output_size)
{
    hdrg_fpenv_t fpenv;
    hdrg_status_t status;

    hdrg_fpenv_enter(&fpenv);

    status = hdrg_convert_raw_pinned(ctx, image, params, display, encode, output, output_size);

    hdrg_fpenv_leave(&fpenv);

    return status;
}
