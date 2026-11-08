/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <string.h>

#include "edge.h"
#include "parts.h"

#define HDRG_LIT(s)       s, sizeof(s) - 1u
#define HDRG_EDGE_PLUGIN  "hdrg.scale.edge"
#define HDRG_EDGE_ID      "hdrg.scale.edge"
#define HDRG_EDGE_LICENSE "Apache-2.0 OR MIT"
#define HDRG_EDGE_COPY    "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_EDGE_UNIT    (1.0f / 65535.0f)

/*
 * The library's edge-adaptive upscale (edge.h) on RGB16 code values:
 * upscaling only (a smaller output HDRG_ERR_UNSUPPORTED, the same size is
 * copied). Bands of output rows over the luma of their input rows
 */
typedef struct {
    hdrg_edge_config_t config;
    uint32_t in_w, in_h, out_w, out_h;
    bool same; /* the input's size, copied */
} hdrg_edge_t;

static inline int64_t hdrg_edge_clamp(int64_t v, int64_t hi)
{
    return (v < 0) ? 0 : (v > hi) ? hi : v;
}

/* the input row at or before output row y's place */
static inline int64_t hdrg_edge_row(const hdrg_edge_t *edge, uint32_t y)
{
    return (int64_t)floorf((0.5f + (float)y) * edge->config.scale_y - 0.5f);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_edge_create(hdrg_filter_t *filter,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    hdrg_edge_t *edge;
    hdrg_status_t status;
    double stretch, sharpen;

    (void)filter;

    edge = (hdrg_edge_t *)api->mem_calloc(api, 1, sizeof(hdrg_edge_t));
    if (edge == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("stretch"), &stretch);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, HDRG_LIT("sharpen"), &sharpen);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, edge);

        return status;
    }

    edge->config.stretch = (float)stretch;
    edge->config.sharpen = (float)sharpen;
    *inst = edge;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_edge_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_edge_configure(void *inst, hdrg_negotiation_t *nego,
                                                          const hdrg_plugin_api_t *api)
{
    hdrg_edge_t *edge = (hdrg_edge_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &edge->in_w, &edge->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &edge->out_w, &edge->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || edge->in_w == 0 || edge->in_h == 0 ||
        edge->out_w < edge->in_w || edge->out_h < edge->in_h) {
        return HDRG_ERR_UNSUPPORTED;
    }

    edge->config.scale_x = (float)edge->in_w / (float)edge->out_w;
    edge->config.scale_y = (float)edge->in_h / (float)edge->out_h;
    edge->same = edge->out_w == edge->in_w && edge->out_h == edge->in_h;

    return api->nego_set_output(nego, edge->out_w, edge->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_edge_input_rows(void *inst, uint32_t out_begin,
                                                           uint32_t out_end, uint32_t *in_begin,
                                                           uint32_t *in_end,
                                                           const hdrg_plugin_api_t *api)
{
    const hdrg_edge_t *edge = (const hdrg_edge_t *)inst;

    (void)api;

    if (out_begin >= out_end || out_end > edge->out_h) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (edge->same) {
        *in_begin = out_begin;
        *in_end = out_end;

        return HDRG_OK;
    }

    /* the 6 taps from two rows before the place */
    *in_begin = (uint32_t)hdrg_edge_clamp(hdrg_edge_row(edge, out_begin) - 2, edge->in_h - 1u);
    *in_end =
        (uint32_t)hdrg_edge_clamp(hdrg_edge_row(edge, out_end - 1u) + 3, edge->in_h - 1u) + 1u;

    return HDRG_OK;
}

/* output pixel (x, y) of the input rows (and their luma) from first */
static void hdrg_edge_out(const hdrg_edge_t *edge, const float *luma, const uint8_t *in,
                          size_t stride, uint32_t first, uint32_t x, uint32_t y, uint16_t *dst)
{
    const uint16_t *src;
    uint8_t i, j, c;
    int64_t ix, iy, sx, sy;
    float src_x, src_y, taps[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS][3], l[HDRG_EDGE_TAPS][HDRG_EDGE_TAPS],
        out[3];

    src_x = (0.5f + (float)x) * edge->config.scale_x - 0.5f;
    src_y = (0.5f + (float)y) * edge->config.scale_y - 0.5f;
    ix = (int64_t)floorf(src_x);
    iy = (int64_t)floorf(src_y);

    for (i = 0; i < HDRG_EDGE_TAPS; i++) {
        sy = hdrg_edge_clamp(iy + i - 2, edge->in_h - 1u);

        for (j = 0; j < HDRG_EDGE_TAPS; j++) {
            sx = hdrg_edge_clamp(ix + j - 2, edge->in_w - 1u);
            src = (const uint16_t *)(const void *)(in + (size_t)(sy - first) * stride) +
                  (size_t)sx * 3u;
            l[i][j] = luma[(size_t)(sy - first) * edge->in_w + (size_t)sx];

            for (c = 0; c < 3u; c++) {
                taps[i][j][c] = (float)src[c] * HDRG_EDGE_UNIT;
            }
        }
    }

    hdrg_edge_pixel(taps, l, src_x - floorf(src_x), src_y - floorf(src_y), &edge->config, out);

    for (c = 0; c < 3u; c++) {
        dst[c] = (uint16_t)(out[c] * 65535.0f + 0.5f);
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_edge_process(void *inst, hdrg_job_t *job,
                                                        const hdrg_plugin_api_t *api)
{
    const hdrg_edge_t *edge = (const hdrg_edge_t *)inst;
    const uint16_t *src;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, x, y;
    uint8_t *out;
    size_t in_stride, out_stride;
    float *luma;
    void *scratch;

    status = api->job_rows(job, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (edge->same) {
        if (in_begin > begin || in_end < end) {
            return HDRG_ERR_INTERNAL;
        }

        for (y = begin; y < end; y++) {
            memcpy(out + (size_t)(y - begin) * out_stride, in + (size_t)(y - in_begin) * in_stride,
                   (size_t)edge->out_w * 6u);
        }

        return HDRG_OK;
    }

    if (hdrg_edge_clamp(hdrg_edge_row(edge, begin) - 2, edge->in_h - 1u) < (int64_t)in_begin ||
        hdrg_edge_clamp(hdrg_edge_row(edge, end - 1u) + 3, edge->in_h - 1u) >= (int64_t)in_end) {
        return HDRG_ERR_INTERNAL;
    }

    /* the input rows' luma */
    status =
        api->job_scratch(job, (size_t)(in_end - in_begin) * edge->in_w * sizeof(float), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    luma = (float *)scratch;

    for (y = in_begin; y < in_end; y++) {
        src = (const uint16_t *)(const void *)(in + (size_t)(y - in_begin) * in_stride);

        for (x = 0; x < edge->in_w; x++) {
            luma[(size_t)(y - in_begin) * edge->in_w + x] =
                hdrg_edge_luma(src[x * 3u], src[x * 3u + 1u], src[x * 3u + 2u]);
        }
    }

    for (y = begin; y < end; y++) {
        for (x = 0; x < edge->out_w; x++) {
            hdrg_edge_out(edge, luma, in, in_stride, in_begin, x, y,
                          (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride) +
                              (size_t)x * 3u);
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_edge_init(hdrg_plugin_t *plugin,
                                                            const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status =
        api->plugin_set_info(plugin, HDRG_LIT(HDRG_EDGE_PLUGIN), HDRG_LIT("Edge-adaptive upscale"),
                             HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status =
            api->plugin_set_license(plugin, HDRG_LIT(HDRG_EDGE_LICENSE), HDRG_LIT(HDRG_EDGE_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_EDGE_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("stretch"), HDRG_EDGE_STRETCH, 0.0, 3.0,
        HDRG_LIT("the kernel's widening along an edge, by its coherence (0 = none)"));
    if (status == HDRG_OK) {
        status = api->class_add_option_float(
            cls, HDRG_LIT("sharpen"), HDRG_EDGE_SHARPEN, 0.0, 2.0,
            HDRG_LIT("the kernel's narrowing across an edge, by its coherence (0 = none)"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_edge_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_edge_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_edge_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_edge_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_edge_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_SCALE_TEMPORAL)
    if (status == HDRG_OK) {
        status = hdrg_filter_temporal_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_edge_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_edge_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_edge_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
