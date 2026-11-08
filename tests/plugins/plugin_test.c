/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Test plugin module (loaded with hdrg_context_load_plugin). Two SCALE
 * classes doing nearest-neighbor resampling plus an option-driven offset:
 * one on bands (the default input rows from its halo), one on whole frames.
 * Includes only the public headers, as any external plugin.
 */
#include <stdlib.h>

#include "hdrgrading/filter.h"

#define TP_LIT(s) s, sizeof(s) - 1u

typedef struct {
    int64_t offset;
    double gain;
    uint32_t in_w, in_h, out_w, out_h;
} tp_nearest_t;

static const char tp_notice[] = "Test data, CC-BY-4.0.\n";
static const char tp_text[] = "Permission to use this test data for testing only.\n";

static hdrg_status_t HDRG_PLUGIN_CALL tp_create(hdrg_filter_t *filter,
                                                const hdrg_options_t *options, void **inst,
                                                const hdrg_plugin_api_t *api)
{
    tp_nearest_t *tp;
    hdrg_status_t status;

    (void)filter;

    tp = (tp_nearest_t *)api->mem_calloc(api, 1, sizeof(*tp));
    if (tp == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, TP_LIT("offset"), &tp->offset);
    if (status == HDRG_OK) {
        status = api->option_get_float(options, TP_LIT("gain"), &tp->gain);
    }

    if (status != HDRG_OK) {
        api->mem_free(api, tp);

        return status;
    }

    *inst = tp;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL tp_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL tp_configure(void *inst, hdrg_negotiation_t *nego,
                                                   const hdrg_plugin_api_t *api)
{
    tp_nearest_t *tp = (tp_nearest_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &tp->in_w, &tp->in_h, &format);
    if (status == HDRG_OK) {
        status = api->nego_requested_size(nego, &tp->out_w, &tp->out_h);
    }

    if (status == HDRG_OK) {
        status = api->nego_set_halo(nego, 1);
    }

    if (status != HDRG_OK) {
        return status;
    }

    return api->nego_set_output(nego, tp->out_w, tp->out_h, HDRG_FORMAT_RGB16);
}

static hdrg_status_t HDRG_PLUGIN_CALL tp_process(void *inst, hdrg_job_t *job,
                                                 const hdrg_plugin_api_t *api)
{
    const tp_nearest_t *tp = (const tp_nearest_t *)inst;
    const uint16_t *src;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x, sy, sx, c;
    uint16_t *dst;
    uint8_t *out;
    size_t in_stride, out_stride;
    double v;

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
        sy = (uint32_t)(((uint64_t)y * 2u + 1u) * tp->in_h / (2u * (uint64_t)tp->out_h));
        if (sy < in_begin || sy >= in_end) {
            return HDRG_ERR_INTERNAL;
        }

        src = (const uint16_t *)(const void *)(in + (size_t)(sy - in_begin) * in_stride);
        dst = (uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride);
        for (x = 0; x < tp->out_w; x++) {
            sx = (uint32_t)(((uint64_t)x * 2u + 1u) * tp->in_w / (2u * (uint64_t)tp->out_w));
            for (c = 0; c < 3; c++) {
                v = (double)src[(size_t)sx * 3u + c] * tp->gain + (double)tp->offset;
                dst[(size_t)x * 3u + c] = (uint16_t)((v > 65535.0) ? 65535.0 : v);
            }
        }
    }

    return HDRG_OK;
}

static hdrg_status_t tp_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api, const char *id,
                              size_t id_len, uint32_t flags)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, id_len);
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, flags);
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_int(cls, TP_LIT("offset"), 0, 0, 1000,
                                           TP_LIT("added to every code value"));
    }

    if (status == HDRG_OK) {
        status = api->class_add_option_float(cls, TP_LIT("gain"), 1.0, 0.0, 4.0,
                                             TP_LIT("multiplies every code value"));
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, tp_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, tp_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, tp_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, tp_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL tp_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.hdrgrading.test"), TP_LIT("Test plugin"),
                                  TP_LIT("0.1.0-test+1"), TP_LIT("libhdrgrading tests"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, TP_LIT("MIT AND LicenseRef-Test-Data"),
                                         TP_LIT("Copyright (c) 2026 Go Kudo"));
    }

    if (status == HDRG_OK) {
        status =
            api->plugin_add_license_text(plugin, TP_LIT("LicenseRef-Test-Data"), TP_LIT(tp_text));
    }

    if (status == HDRG_OK) {
        status = api->plugin_add_component(plugin, TP_LIT("test-weights"), TP_LIT("2"),
                                           TP_LIT("CC-BY-4.0"), TP_LIT("Copyright (c) 2026 Tests"),
                                           TP_LIT(tp_notice));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_DEGRADED, HDRG_REASON_NO_DEVICE,
                                        TP_LIT("no GPU, running on the CPU"));
    }

    if (status == HDRG_OK) {
        status = tp_class(plugin, api, TP_LIT("org.hdrgrading.test.nearest"),
                          HDRG_FILTER_PURE | HDRG_FILTER_BANDS | HDRG_FILTER_DETERMINISTIC);
    }

    if (status == HDRG_OK) {
        status = tp_class(plugin, api, TP_LIT("org.hdrgrading.test.whole"), 0);
    }

    return status;
}

HDRG_PLUGIN_DEFINE(tp_init)
