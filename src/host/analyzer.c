/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "analyzer.h"

#include <string.h>

#include "../priv.h"

#define HDRG_LIGHTS_MAGIC UINT32_C(0x484C4954)

static inline bool hdrg_lights_ok(const hdrg_lights_t *lights)
{
    return lights != NULL && lights->magic == HDRG_LIGHTS_MAGIC;
}

extern hdrg_status_t hdrg_analyzer_open(const hdrg_filter_entry_t *entry, hdrg_plugin_host_t *host,
                                        hdrg_analyzer_t *out)
{
    const hdrg_class_t *cls = entry->cls;
    hdrg_status_t status;

    memset(out, 0, sizeof(*out));

    if (cls->analyze == NULL) {
        hdrg_host_set_error(host, "the filter judges no lights", cls->id.data, cls->id.size);

        return HDRG_ERR_INVALID_ARG;
    }

    out->filter.magic = HDRG_FILTER_MAGIC;
    out->filter.cls = cls;
    out->filter.host = host;
    out->filter.stage = HDRG_STAGE_ANALYZE;
    out->cls = cls;
    out->filter.creating = true;

    status = cls->create(&out->filter, entry->options, &out->inst, hdrg_class_api(cls));

    out->filter.creating = false;

    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter create failed", cls->id.data, cls->id.size);
        memset(out, 0, sizeof(*out));

        return (status == HDRG_ERR_UNSUPPORTED) ? HDRG_ERR_UNSUPPORTED : HDRG_ERR_PLUGIN;
    }

    out->created = true;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_analyzer_judge(void *user, const hdrg_source_t *src,
                                         const hdrg_light_record_t *records, uint32_t count,
                                         float *chances)
{
    hdrg_analyzer_t *analyzer = (hdrg_analyzer_t *)user;
    hdrg_lights_t lights;
    hdrg_status_t status;
    uint32_t i;

    for (i = 0; i < count; i++) {
        chances[i] = 1.0f;
    }

    if (!analyzer->created || count == 0) {
        return HDRG_OK;
    }

    memset(&lights, 0, sizeof(lights));
    lights.magic = HDRG_LIGHTS_MAGIC;
    lights.alloc = &analyzer->filter.host->alloc;
    lights.src = src;
    lights.records = records;
    lights.chances = chances;
    lights.count = count;

    status = analyzer->cls->analyze(analyzer->inst, &lights, hdrg_class_api(analyzer->cls));

    hdrg_mem_free(lights.alloc, lights.frame);
    lights.magic = 0;

    return (status == HDRG_OK) ? HDRG_OK : HDRG_ERR_PLUGIN;
}

extern void hdrg_analyzer_close(hdrg_analyzer_t *analyzer)
{
    if (analyzer->created) {
        analyzer->cls->destroy(analyzer->inst, hdrg_class_api(analyzer->cls));
    }

    memset(analyzer, 0, sizeof(*analyzer));
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_get(const hdrg_lights_t *lights,
                                                          const hdrg_light_record_t **records,
                                                          uint32_t *count)
{
    if (!hdrg_lights_ok(lights) || records == NULL || count == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *records = lights->records;
    *count = lights->count;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_frame(hdrg_lights_t *lights,
                                                            const uint16_t **rgb, size_t *stride,
                                                            uint32_t *width, uint32_t *height)
{
    const hdrg_source_t *src;
    const uint8_t *row;
    uint32_t x, y, codes[3];
    uint16_t *out;
    uint8_t c;

    if (!hdrg_lights_ok(lights) || rgb == NULL || stride == NULL || width == NULL ||
        height == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    src = lights->src;

    if (src == NULL || src->pixels == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    /* the codes widened to 16 bits (8-bit ones x 257), the alpha dropped */
    if (lights->frame == NULL) {
        lights->frame = (uint16_t *)hdrg_mem_alloc(lights->alloc, (size_t)src->width * src->height *
                                                                      3u * sizeof(uint16_t));
        if (lights->frame == NULL) {
            return HDRG_ERR_NOMEM;
        }

        for (y = 0; y < src->height; y++) {
            row = hdrg_source_row(src, src->row0 + y);
            out = lights->frame + (size_t)y * src->width * 3u;

            for (x = 0; x < src->width; x++) {
                hdrg_source_codes(src, row, x, codes);

                for (c = 0; c < 3u; c++) {
                    out[(size_t)x * 3u + c] = (uint16_t)(src->wide ? codes[c] : codes[c] * 257u);
                }
            }
        }
    }

    *rgb = lights->frame;
    *stride = (size_t)src->width * 3u * sizeof(uint16_t);
    *width = src->width;
    *height = src->height;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_set_chance(hdrg_lights_t *lights,
                                                                 uint32_t index, float chance)
{
    if (!hdrg_lights_ok(lights) || index >= lights->count || !(chance >= 0.0f && chance <= 1.0f)) {
        return HDRG_ERR_INVALID_ARG;
    }

    lights->chances[index] = chance;

    return HDRG_OK;
}
