/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "container.h"

#include <math.h>
#include <string.h>

#include "core/dynamic.h"
#include "io/io.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"
#include "host/codec_host.h"

/* dither key of the counter-based hash for the stored gain map */
#define HDRG_KEY_GAINMAP UINT64_C(0x504E475F5051584D)
/* H.273 transfer characteristics: SMPTE ST 2084 (PQ) */
#define HDRG_CONTAINER_TRANSFER_PQ 16u

/* what the writer's write_icc asks the conversion for */
typedef struct {
    const hdrg_allocator_t *alloc;
    const hdrg_pipeline_t *pipeline;
    uint32_t transfer;
} hdrg_container_icc_t;

/* H.273 colour primaries of a rendition gamut */
static uint8_t hdrg_container_primaries(uint8_t gamut)
{
    if (gamut == HDRG_GAMUT_BT2020) {
        return HDRG_PRIMARIES_BT2020;
    }

    return (gamut == HDRG_GAMUT_P3) ? HDRG_PRIMARIES_P3_D65 : HDRG_PRIMARIES_BT709;
}

#ifdef HDRG_WITH_LCMS2
/*
 * HAGC payload: the rendition's (single-layer PQ, the hybrid's PQ primary)
 * gain curve back to its SDR source, fitted to the light pass C measured
 * (hdrg_dynamic_gain_curve, the video's ST 2094-50 alike) at the content's
 * headroom over the SDR white; one alternate at headroom 0 (a display
 * between weighs it by its headroom). NULL when none fitted
 */
static hdrg_status_t hdrg_make_hagc(const hdrg_allocator_t *alloc, const hdrg_pipeline_t *pipeline,
                                    uint8_t **out, size_t *out_size)
{
    hdrg_st2094_50_t curve;
    hdrg_lightstat_t light;
    uint8_t *payload;
    size_t size;

    *out = NULL;
    *out_size = 0;

    if (pipeline->apply.chunk_light == NULL) {
        return HDRG_OK;
    }

    hdrg_pipeline_light_of(pipeline, &light);
    hdrg_dynamic_gain_curve(
        &light, pipeline->render.sdr_white_nits, pipeline->render.gamut,
        hdrg_math_log2f(pipeline->render.top_nits / pipeline->render.sdr_white_nits), &curve);

    size = hdrg_st2094_50_t35(&curve, NULL, 0);
    if (size == 0 || curve.baseline_headroom > 6.0f) {
        return HDRG_OK;
    }

    payload = (uint8_t *)hdrg_mem_alloc(alloc, size);
    if (payload == NULL) {
        return HDRG_ERR_NOMEM;
    }

    (void)hdrg_st2094_50_t35(&curve, payload, size);
    *out = payload;
    *out_size = size;

    return HDRG_OK;
}
#endif

/*
 * the writer's write_icc: "P3 PQ" for a PQ base (its HAGC fitted to the rows' light when hagc),
 * Display P3 for an SDR one (the host does not ask for sRGB's)
 */
static hdrg_status_t hdrg_container_icc(void *user, bool hagc, float lumi_nits, uint8_t **out,
                                        size_t *size)
{
#ifdef HDRG_WITH_LCMS2
    const hdrg_container_icc_t *icc = (const hdrg_container_icc_t *)user;
    hdrg_status_t status;
    uint8_t *payload;
    size_t payload_size;

    *out = NULL;
    *size = 0;

    if (icc->transfer != HDRG_CONTAINER_TRANSFER_PQ) {
        return hdrg_icc_create_p3(icc->alloc, out, size);
    }

    payload = NULL;
    payload_size = 0;
    status = hagc ? hdrg_make_hagc(icc->alloc, icc->pipeline, &payload, &payload_size) : HDRG_OK;

    if (status == HDRG_OK) {
        status = hdrg_icc_create_p3_pq(icc->alloc, lumi_nits, payload, payload_size, out, size);
    }

    hdrg_mem_free(icc->alloc, payload);

    return status;
#else
    (void)user;
    (void)hagc;
    (void)lumi_nits;

    *out = NULL;
    *size = 0;

    return HDRG_ERR_ICC_UNSUPPORTED;
#endif
}

/*
 * The stored gain map of a finished two-layer frame (1 / 2^gm_shift of its
 * size, gw x gh): each cell's mean log2 ratio over max_lr, an up map (GAINMAP)
 * or a down map (HYBRID: 1 - it), at the encoding gamma, 8-bit with the
 * triangular dither
 */
static hdrg_status_t hdrg_container_gainmap_pixels(const hdrg_allocator_t *alloc,
                                                   const hdrg_pipeline_t *pipeline,
                                                   const hdrg_encode_params_t *encode, float max_lr,
                                                   uint8_t **out, uint32_t *out_w, uint32_t *out_h)
{
    const hdrg_apply_t *apply = &pipeline->apply;
    const uint32_t width = pipeline->width, height = pipeline->height;
    const uint8_t shift = apply->gm_shift;
    const bool hybrid = encode->output_mode == HDRG_OUTPUT_HYBRID;
    uint32_t gw, gh, gx, gy, count;
    uint8_t *pixels;
    size_t idx;
    float value, tri;

    gw = apply->gm_w;
    gh = (height + (1u << shift) - 1u) >> shift;

    pixels = (uint8_t *)hdrg_mem_alloc(alloc, (size_t)gw * gh);
    if (pixels == NULL) {
        return HDRG_ERR_NOMEM;
    }

    for (gy = 0; gy < gh; gy++) {
        for (gx = 0; gx < gw; gx++) {
            idx = (size_t)gy * gw + gx;
            count = (HDRG_MIN((gx + 1u) << shift, width) - (gx << shift)) *
                    (HDRG_MIN((gy + 1u) << shift, height) - (gy << shift));
            value = HDRG_CLAMP(apply->gm_sum[idx] / (float)count / max_lr, 0.0f, 1.0f);

            if (hybrid) {
                /* down map: decoded gain = gain_map_min * (1 - stored^gamma) */
                value = 1.0f - value;
            }

            value = hdrg_math_powf(value, 1.0f / encode->gainmap_gamma) * 255.0f;

            if (encode->dither) {
                tri = hdrg_hash_triangular(HDRG_KEY_GAINMAP, gx, gy, 0);
                value += tri;
            }

            pixels[idx] = (uint8_t)HDRG_CLAMP((int32_t)lrintf(value), 0, 255);
        }
    }

    *out = pixels;
    *out_w = gw;
    *out_h = gh;

    return HDRG_OK;
}

/* the finished frame's gain map, metadata and light for the writer's finish */
static hdrg_status_t hdrg_container_finish(hdrg_context_t *ctx, const hdrg_pipeline_t *pipeline,
                                           const hdrg_encode_params_t *encode,
                                           hdrg_writer_t *writer, uint8_t **gm_pixels)
{
    const uint8_t mode = encode->output_mode;
    float max_nits, avg_nits, max_lr;

    *gm_pixels = NULL;
    writer->peak = (uint32_t)HDRG_CLAMP(pipeline->render.peak_nits + 0.5f, 0.0f, 10000.0f);

    if (mode != HDRG_OUTPUT_GAINMAP) {
        hdrg_pipeline_light(pipeline, &max_nits, &avg_nits);
        writer->max_cll = (uint32_t)HDRG_CLAMP(max_nits + 0.5f, 0.0f, 65535.0f);
        writer->max_fall = (uint32_t)HDRG_CLAMP(avg_nits + 0.5f, 0.0f, 65535.0f);
    }

    if (mode == HDRG_OUTPUT_PQ) {
        return HDRG_OK;
    }

    max_lr = hdrg_pipeline_max_ratio(pipeline);
    writer->gm_values[HDRG_GAINMAP_GAMMA] = encode->gainmap_gamma;
    writer->alt_primaries = writer->primaries;

    if (mode == HDRG_OUTPUT_HYBRID) {
        /* the SDR alternate, recovered in the base's colors */
        writer->gm_values[HDRG_GAINMAP_BASE_HEADROOM] = max_lr;
        writer->gm_values[HDRG_GAINMAP_MIN] = -max_lr;
        writer->alt_transfer = HDRG_TRANSFER_SRGB;
        writer->alt_bits = 8;
    } else {
        writer->gm_values[HDRG_GAINMAP_ALT_HEADROOM] = max_lr;
        writer->gm_values[HDRG_GAINMAP_MAX] = max_lr;
        writer->alt_transfer = HDRG_CONTAINER_TRANSFER_PQ;
        writer->alt_bits = 10;
    }

    writer->gm_stride = pipeline->apply.gm_w;

    return hdrg_container_gainmap_pixels(&ctx->alloc, pipeline, encode, max_lr, gm_pixels,
                                         &writer->gm_width, &writer->gm_height);
}

extern hdrg_status_t hdrg_container_select(hdrg_context_t *ctx, hdrg_encode_params_t *encode,
                                           hdrg_container_t *out)
{
    out->writer = hdrg_codec_writer(&ctx->host, &out->options);
    if (out->writer == NULL) {
        return HDRG_ERR_UNSUPPORTED_FORMAT;
    }

    if (encode->output_mode == HDRG_OUTPUT_AUTO) {
        encode->output_mode = (uint8_t)out->writer->default_form;
    }

    if ((out->writer->forms & HDRG_CODEC_FORM(encode->output_mode)) == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

#ifndef HDRG_WITH_LCMS2
    /* the PQ bases' "P3 PQ" profile is generated */
    if (out->writer->color == HDRG_CODEC_COLOR_ICC && encode->output_mode != HDRG_OUTPUT_GAINMAP) {
        return HDRG_ERR_ICC_UNSUPPORTED;
    }
#endif

    return HDRG_OK;
}

extern void hdrg_container_caps(const hdrg_container_t *container,
                                const hdrg_display_caps_t *display,
                                const hdrg_encode_params_t *encode, hdrg_display_caps_t *out)
{
    /*
     * without lcms2 no P3 profile can be written: an ICC writer's file is sRGB. Code points
     * name any gamut, their PQ bases in a BT.2020 container (the colors within P3)
     */
    if (container->writer->color == HDRG_CODEC_COLOR_CICP) {
        hdrg_display_caps_for_file(display, encode, true, out);

        if (encode->output_mode != HDRG_OUTPUT_GAINMAP && out->gamut == HDRG_GAMUT_P3) {
            out->gamut = HDRG_GAMUT_BT2020;
        }

        return;
    }

#ifdef HDRG_WITH_LCMS2
    hdrg_display_caps_for_file(display, encode, true, out);
#else
    hdrg_display_caps_for_file(display, encode, false, out);
#endif
}

extern hdrg_status_t hdrg_container_prepare(const hdrg_container_t *container, hdrg_context_t *ctx,
                                            hdrg_pipeline_t *pipeline,
                                            const hdrg_encode_params_t *encode)
{
    /* refused before the work */
    if (pipeline->width > container->writer->max_side ||
        pipeline->height > container->writer->max_side) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* the rows the writer takes; the PQ bases' light (MaxCLL / FALL, a profile's HAGC) */
    return hdrg_pipeline_output(ctx, pipeline,
                                (container->writer->rows == HDRG_CODEC_ROWS_RGB16) ? 16u : 8u,
                                encode->output_mode != HDRG_OUTPUT_GAINMAP);
}

extern hdrg_status_t hdrg_container_write(const hdrg_container_t *container, hdrg_context_t *ctx,
                                          hdrg_pipeline_t *pipeline,
                                          const hdrg_encode_params_t *encode, const uint8_t *exif,
                                          size_t exif_size, uint8_t **output, size_t *output_size)
{
    const hdrg_allocator_t *alloc = &ctx->alloc;
    const uint8_t mode = encode->output_mode;
    hdrg_container_icc_t icc;
    hdrg_writer_t writer;
    hdrg_status_t status;
    uint32_t y0, rows;
    uint8_t *band, *gm_pixels;
    size_t row;

    *output = NULL;
    *output_size = 0;
    gm_pixels = NULL;
    row = (size_t)pipeline->width * 3u *
          ((container->writer->rows == HDRG_CODEC_ROWS_RGB16) ? 2u : 1u);

    band = (uint8_t *)hdrg_mem_alloc(alloc, row * HDRG_APPLY_BAND_ROWS);
    if (band == NULL) {
        return HDRG_ERR_NOMEM;
    }

    hdrg_writer_init(&writer, &ctx->host, container->writer);
    writer.width = pipeline->width;
    writer.height = pipeline->height;
    writer.form = mode;
    writer.primaries = hdrg_container_primaries(pipeline->render.gamut);
    writer.transfer =
        (mode == HDRG_OUTPUT_GAINMAP) ? HDRG_TRANSFER_SRGB : HDRG_CONTAINER_TRANSFER_PQ;
    writer.threads = pipeline->exec.max_workers;
    writer.exif = (exif != NULL && exif_size > 6u) ? exif + 6 : NULL;
    writer.exif_size = (writer.exif != NULL) ? exif_size - 6u : 0u;
    icc.alloc = alloc;
    icc.pipeline = pipeline;
    icc.transfer = writer.transfer;
    writer.icc = hdrg_container_icc;
    writer.icc_user = &icc;

    status = hdrg_writer_create(&writer, container->options);

    for (y0 = 0; status == HDRG_OK && y0 < pipeline->height; y0 += HDRG_APPLY_BAND_ROWS) {
        rows = HDRG_MIN(HDRG_APPLY_BAND_ROWS, pipeline->height - y0);

        status = hdrg_pipeline_rows(ctx, pipeline, y0, rows, band);
        if (status == HDRG_OK) {
            status = hdrg_writer_rows(&writer, y0, rows, band, row);
        }
    }

    hdrg_mem_free(alloc, band);

    if (status == HDRG_OK) {
        hdrg_pipeline_end(ctx, pipeline);

        status = hdrg_container_finish(ctx, pipeline, encode, &writer, &gm_pixels);
    }

    if (status == HDRG_OK) {
        writer.gm_pixels = gm_pixels;

        status = hdrg_writer_finish(&writer, output, output_size);
    }

    if (status == HDRG_OK) {
        ctx->last_stats.applied_max_gain_stops =
            (mode == HDRG_OUTPUT_PQ)
                ? pipeline->render.max_gain_stops
                : (float)writer.gm_values[(mode == HDRG_OUTPUT_HYBRID) ? HDRG_GAINMAP_BASE_HEADROOM
                                                                       : HDRG_GAINMAP_ALT_HEADROOM];
        hdrg_pipeline_stats(ctx, pipeline);
        ctx->has_stats = true;
    }

    hdrg_writer_destroy(&writer);
    hdrg_mem_free(alloc, gm_pixels);

    return status;
}
