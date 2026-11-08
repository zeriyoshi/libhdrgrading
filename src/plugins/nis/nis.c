/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 *
 * hdrg.scale.nis ports NVIDIA Image Scaling 1.0.3 (MIT; its notice is in
 * nis.h).
 */

#include <math.h>
#include <string.h>

#include "parts.h"
#include "nis.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_NIS_PLUGIN  "hdrg.scale.nvidia"
#define HDRG_NIS_ID      "hdrg.scale.nis"
#define HDRG_NIS_LICENSE "MIT"
#define HDRG_NIS_COPY                                                                              \
    "Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES; Copyright (c) 2026 Go Kudo"
#define HDRG_NIS_UNIT      (1.0f / 65535.0f)
#define HDRG_NIS_SHARPNESS 0.5 /* NIS's samples' */

/*
 * NIS on RGB16 code values (nis.h): from the input's size up to twice it
 * (else HDRG_ERR_UNSUPPORTED; the same size is copied). Bands of output
 * rows over the luma of their input rows
 */
typedef struct {
    hdrg_nis_config_t config;
    uint32_t in_w, in_h, out_w, out_h;
    bool same; /* the input's size, copied */
    float sharpness;
} hdrg_nis_t;

static inline int64_t hdrg_nis_clamp(int64_t v, int64_t hi)
{
    return (v < 0) ? 0 : (v > hi) ? hi : v;
}

/* the input row at or before output row y's position */
static inline int64_t hdrg_nis_row(const hdrg_nis_t *nis, uint32_t y)
{
    return (int64_t)floorf((0.5f + (float)y) * nis->config.scale_y - 0.5f);
}

/* the luma at input pixel (x, y), clamped to the frame (rows from first) */
static inline float hdrg_nis_luma_at(const hdrg_nis_t *nis, const float *luma, uint32_t first,
                                     int64_t x, int64_t y)
{
    return luma[(size_t)(hdrg_nis_clamp(y, nis->in_h - 1u) - first) * nis->in_w +
                (size_t)hdrg_nis_clamp(x, nis->in_w - 1u)];
}

/* the input pixel (x, y)'s code values as floats, clamped to the frame */
static inline void hdrg_nis_rgb_at(const hdrg_nis_t *nis, const uint8_t *in, size_t stride,
                                   uint32_t first, int64_t x, int64_t y, float out[3])
{
    const uint16_t *src =
        (const uint16_t *)(const void *)(in + (size_t)(hdrg_nis_clamp(y, nis->in_h - 1u) - first) *
                                                  stride) +
        (size_t)hdrg_nis_clamp(x, nis->in_w - 1u) * 3u;
    uint8_t c;

    for (c = 0; c < 3u; c++) {
        out[c] = (float)src[c] * HDRG_NIS_UNIT;
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_nis_create(hdrg_filter_t *filter,
                                                      const hdrg_options_t *options, void **inst,
                                                      const hdrg_plugin_api_t *api)
{
    hdrg_nis_t *nis;
    hdrg_status_t status;
    double sharpness;

    (void)filter;

    nis = (hdrg_nis_t *)api->mem_calloc(api, 1, sizeof(hdrg_nis_t));
    if (nis == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("sharpness"), &sharpness);
    if (status != HDRG_OK) {
        api->mem_free(api, nis);

        return status;
    }

    nis->sharpness = (float)sharpness;
    *inst = nis;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_nis_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_nis_configure(void *inst, hdrg_negotiation_t *nego,
                                                         const hdrg_plugin_api_t *api)
{
    hdrg_nis_t *nis = (hdrg_nis_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &nis->in_w, &nis->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &nis->out_w, &nis->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || nis->in_w == 0 || nis->in_h == 0 || nis->out_w == 0 ||
        nis->out_h == 0 ||
        !hdrg_nis_setup(nis->sharpness, nis->in_w, nis->in_h, nis->out_w, nis->out_h,
                        &nis->config)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    nis->same = nis->out_w == nis->in_w && nis->out_h == nis->in_h;

    return api->nego_set_output(nego, nis->out_w, nis->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_nis_input_rows(void *inst, uint32_t out_begin,
                                                          uint32_t out_end, uint32_t *in_begin,
                                                          uint32_t *in_end,
                                                          const hdrg_plugin_api_t *api)
{
    const hdrg_nis_t *nis = (const hdrg_nis_t *)inst;

    (void)api;

    if (out_begin >= out_end || out_end > nis->out_h) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (nis->same) {
        *in_begin = out_begin;
        *in_end = out_end;

        return HDRG_OK;
    }

    /* the 6 taps from two rows before the position, the edges one more each side */
    *in_begin = (uint32_t)hdrg_nis_clamp(hdrg_nis_row(nis, out_begin) - 2, nis->in_h - 1u);
    *in_end = (uint32_t)hdrg_nis_clamp(hdrg_nis_row(nis, out_end - 1u) + 3, nis->in_h - 1u) + 1u;

    return HDRG_OK;
}

/* output pixel (x, y) of the luma and the input rows from first */
static void hdrg_nis_pixel(const hdrg_nis_t *nis, const float *luma, const uint8_t *in,
                           size_t stride, uint32_t first, uint32_t x, uint32_t y, uint16_t *dst)
{
    const hdrg_nis_config_t *c = &nis->config;
    uint32_t phase_x, phase_y;
    uint8_t i, j, k, l;
    int64_t ix, iy;
    float src_x, src_y, fx, fy, p[6][6], area[3][3], edge[2][2][4], w[4], base, op_y, s[4][3],
        rgb[3], y_rgb, corr;

    src_x = (0.5f + (float)x) * c->scale_x - 0.5f;
    src_y = (0.5f + (float)y) * c->scale_y - 0.5f;
    ix = (int64_t)floorf(src_x);
    iy = (int64_t)floorf(src_y);
    fx = src_x - floorf(src_x);
    fy = src_y - floorf(src_y);
    phase_x = (uint32_t)(fx * (float)HDRG_NIS_PHASES);
    phase_y = (uint32_t)(fy * (float)HDRG_NIS_PHASES);

    /* the edges at the four pixels around the position, interpolated there */
    for (i = 0; i < 2u; i++) {
        for (j = 0; j < 2u; j++) {
            for (k = 0; k < 3u; k++) {
                for (l = 0; l < 3u; l++) {
                    area[k][l] = hdrg_nis_luma_at(nis, luma, first, ix + j + l - 1, iy + i + k - 1);
                }
            }

            hdrg_nis_edge(area, c, edge[i][j]);
        }
    }

    for (k = 0; k < 4u; k++) {
        w[k] = hdrg_nis_lerp(hdrg_nis_lerp(edge[0][0][k], edge[0][1][k], fx),
                             hdrg_nis_lerp(edge[1][0][k], edge[1][1][k], fx), fy);
    }

    for (i = 0; i < 6u; i++) {
        for (j = 0; j < 6u; j++) {
            p[i][j] = hdrg_nis_luma_at(nis, luma, first, ix + j - 2, iy + i - 2);
        }
    }

    base = 1.0f - w[0] - w[1] - w[2] - w[3];
    op_y = 0.0f;
    op_y += hdrg_nis_normal(p, phase_x, phase_y) * base;
    op_y += hdrg_nis_directional(p, fx, fy, phase_x, phase_y, w, c);

    /* the bilinear source, moved by the filtered luma's difference */
    hdrg_nis_rgb_at(nis, in, stride, first, ix, iy, s[0]);
    hdrg_nis_rgb_at(nis, in, stride, first, ix + 1, iy, s[1]);
    hdrg_nis_rgb_at(nis, in, stride, first, ix, iy + 1, s[2]);
    hdrg_nis_rgb_at(nis, in, stride, first, ix + 1, iy + 1, s[3]);

    for (k = 0; k < 3u; k++) {
        rgb[k] = hdrg_nis_lerp(hdrg_nis_lerp(s[0][k], s[1][k], fx),
                               hdrg_nis_lerp(s[2][k], s[3][k], fx), fy);
    }

    y_rgb = hdrg_nis_y(rgb[0], rgb[1], rgb[2]);
    corr = op_y - y_rgb;

    for (k = 0; k < 3u; k++) {
        dst[k] = (uint16_t)(hdrg_nis_sat(rgb[k] + corr) * 65535.0f + 0.5f);
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_nis_process(void *inst, hdrg_job_t *job,
                                                       const hdrg_plugin_api_t *api)
{
    const hdrg_nis_t *nis = (const hdrg_nis_t *)inst;
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

    if (nis->same) {
        if (in_begin > begin || in_end < end) {
            return HDRG_ERR_INTERNAL;
        }

        for (y = begin; y < end; y++) {
            memcpy(out + (size_t)(y - begin) * out_stride, in + (size_t)(y - in_begin) * in_stride,
                   (size_t)nis->out_w * 6u);
        }

        return HDRG_OK;
    }

    if (hdrg_nis_clamp(hdrg_nis_row(nis, begin) - 2, nis->in_h - 1u) < (int64_t)in_begin ||
        hdrg_nis_clamp(hdrg_nis_row(nis, end - 1u) + 3, nis->in_h - 1u) >= (int64_t)in_end) {
        return HDRG_ERR_INTERNAL;
    }

    /* the input rows' luma */
    status =
        api->job_scratch(job, (size_t)(in_end - in_begin) * nis->in_w * sizeof(float), &scratch);
    if (status != HDRG_OK) {
        return status;
    }

    luma = (float *)scratch;

    for (y = in_begin; y < in_end; y++) {
        src = (const uint16_t *)(const void *)(in + (size_t)(y - in_begin) * in_stride);

        for (x = 0; x < nis->in_w; x++) {
            luma[(size_t)(y - in_begin) * nis->in_w + x] =
                hdrg_nis_y_codes(src[x * 3u], src[x * 3u + 1u], src[x * 3u + 2u]);
        }
    }

    for (y = begin; y < end; y++) {
        for (x = 0; x < nis->out_w; x++) {
            hdrg_nis_pixel(nis, luma, in, in_stride, in_begin, x, y,
                           (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride) +
                               (size_t)x * 3u);
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_nis_init(hdrg_plugin_t *plugin,
                                                           const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status =
        api->plugin_set_info(plugin, HDRG_LIT(HDRG_NIS_PLUGIN), HDRG_LIT("NVIDIA Image Scaling"),
                             HDRG_LIT("1.0.3"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status =
            api->plugin_set_license(plugin, HDRG_LIT(HDRG_NIS_LICENSE), HDRG_LIT(HDRG_NIS_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_NIS_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(cls, HDRG_LIT("sharpness"), HDRG_NIS_SHARPNESS, 0.0, 1.0,
                                         HDRG_LIT("NIS's sharpness (0 = none, 1 = the most)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_nis_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_nis_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_nis_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_nis_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_nis_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

#if defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_nis_register(plugin, api);
    }
#endif

#if defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_nis_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_nis_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
