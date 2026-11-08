/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)     s, sizeof(s) - 1u
#define HDRG_BL_SHIFT   16u
#define HDRG_BL_ONE     (UINT64_C(1) << HDRG_BL_SHIFT)
#define HDRG_BL_ROUND   (UINT64_C(1) << (2u * HDRG_BL_SHIFT - 1u))
#define HDRG_BL_ID      "hdrg.scale.bilinear"
#define HDRG_BL_LICENSE "Apache-2.0 OR MIT"
#define HDRG_BL_COPY    "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"

/*
 * Pixel-center-aligned bilinear resampling of RGB16 code values in 16.16
 * fixed point: integer arithmetic only, so every compiler and band split
 * gives the same output. Column positions are tabulated at configure.
 */
typedef struct {
    uint32_t *x0, *x1, *fx;       /* per output column: source columns and weight of x1 ... */
    uint32_t table_in, table_out; /* ... made for these widths (0: none) */
    uint32_t in_w, in_h, out_w, out_h;
} hdrg_bilinear_t;

/* source samples i0 / i1 and the weight of i1 for output index i of an n -> m resampling */
static inline void hdrg_bl_position(uint32_t i, uint32_t n, uint32_t m, uint32_t *i0, uint32_t *i1,
                                    uint32_t *frac)
{
    int64_t p;

    p = (int64_t)(((uint64_t)(2u * (uint64_t)i + 1u) * n * HDRG_BL_ONE) / (2u * (uint64_t)m)) -
        (int64_t)(HDRG_BL_ONE / 2u);
    if (p < 0) {
        p = 0;
    }

    *i0 = (uint32_t)((uint64_t)p >> HDRG_BL_SHIFT);
    *frac = (uint32_t)((uint64_t)p & (HDRG_BL_ONE - 1u));

    if (*i0 >= n - 1u) {
        *i0 = n - 1u;
        *frac = 0;
    }

    *i1 = (*i0 + 1u < n) ? *i0 + 1u : *i0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_bl_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    *inst = api->mem_calloc(api, 1, sizeof(hdrg_bilinear_t));

    return (*inst != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
}

static void HDRG_PLUGIN_CALL hdrg_bl_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_bilinear_t *bl = (hdrg_bilinear_t *)inst;

    api->mem_free(api, bl->x0);
    api->mem_free(api, bl);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_bl_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_bilinear_t *bl = (hdrg_bilinear_t *)inst;
    hdrg_status_t status;
    uint32_t format, x;

    status = api->nego_input(nego, &bl->in_w, &bl->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &bl->out_w, &bl->out_h);
    }

    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || bl->in_w == 0 || bl->in_h == 0 || bl->out_w == 0 ||
        bl->out_h == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    /* the columns, kept while the widths stay */
    if (bl->table_in != bl->in_w || bl->table_out != bl->out_w) {
        api->mem_free(api, bl->x0);
        bl->table_in = 0;
        bl->table_out = 0;

        bl->x0 = (uint32_t *)api->mem_alloc(api, (size_t)bl->out_w * 3u * sizeof(uint32_t));
        if (bl->x0 == NULL) {
            return HDRG_ERR_NOMEM;
        }

        bl->x1 = bl->x0 + bl->out_w;
        bl->fx = bl->x1 + bl->out_w;
        for (x = 0; x < bl->out_w; x++) {
            hdrg_bl_position(x, bl->in_w, bl->out_w, &bl->x0[x], &bl->x1[x], &bl->fx[x]);
        }

        bl->table_in = bl->in_w;
        bl->table_out = bl->out_w;
    }

    return api->nego_set_output(nego, bl->out_w, bl->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_bl_input_rows(void *inst, uint32_t out_begin,
                                                         uint32_t out_end, uint32_t *in_begin,
                                                         uint32_t *in_end,
                                                         const hdrg_plugin_api_t *api)
{
    const hdrg_bilinear_t *bl = (const hdrg_bilinear_t *)inst;
    uint32_t y0, y1, frac;

    (void)api;

    if (out_begin >= out_end || out_end > bl->out_h) {
        return HDRG_ERR_INVALID_ARG;
    }

    hdrg_bl_position(out_begin, bl->in_h, bl->out_h, &y0, &y1, &frac);
    *in_begin = y0;

    hdrg_bl_position(out_end - 1u, bl->in_h, bl->out_h, &y0, &y1, &frac);
    *in_end = y1 + 1u;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_bl_process(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_bilinear_t *bl = (const hdrg_bilinear_t *)inst;
    const uint16_t *r0, *r1;
    const uint8_t *in;
    hdrg_status_t status;
    uint64_t top, bottom;
    uint32_t begin, end, in_begin, in_end, y, x, y0, y1, fy, a, b, f;
    uint16_t *dst;
    uint8_t *out, c;
    size_t in_stride, out_stride;

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

    for (y = begin; y < end; y++) {
        hdrg_bl_position(y, bl->in_h, bl->out_h, &y0, &y1, &fy);
        if (y0 < in_begin || y1 >= in_end) {
            return HDRG_ERR_INTERNAL;
        }

        r0 = (const uint16_t *)(const void *)(in + (size_t)(y0 - in_begin) * in_stride);
        r1 = (const uint16_t *)(const void *)(in + (size_t)(y1 - in_begin) * in_stride);
        dst = (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride);

        for (x = 0; x < bl->out_w; x++) {
            a = bl->x0[x] * 3u;
            b = bl->x1[x] * 3u;
            f = bl->fx[x];
            for (c = 0; c < 3; c++) {
                top = (uint64_t)r0[a + c] * (HDRG_BL_ONE - f) + (uint64_t)r0[b + c] * f;
                bottom = (uint64_t)r1[a + c] * (HDRG_BL_ONE - f) + (uint64_t)r1[b + c] * f;
                dst[x * 3u + c] =
                    (uint16_t)((top * (HDRG_BL_ONE - fy) + bottom * fy + HDRG_BL_ROUND) >>
                               (2u * HDRG_BL_SHIFT));
            }
        }
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_bilinear_init(hdrg_plugin_t *plugin,
                                                                const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_BL_ID), HDRG_LIT("Bilinear scaler"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_BL_LICENSE), HDRG_LIT(HDRG_BL_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_BL_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_BANDS |
                                               HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_bl_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_bl_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_bl_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, hdrg_bl_process);
    }

    if (status == HDRG_OK) {
        status = api->class_set_input_rows(cls, hdrg_bl_input_rows);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
