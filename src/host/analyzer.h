/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_ANALYZER_H
#define HDRG_HOST_ANALYZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/image.h"
#include "host.h"

/*
 * The ANALYZE stage: an ANALYZE class's instance in a pipeline, the
 * chances it gives a frame's judged lights (hdrg_params_t.light_predictor
 * scales their boost by them: src/core/predict)
 */
typedef struct {
    hdrg_filter_t filter;
    const hdrg_class_t *cls;
    void *inst;
    bool created;
} hdrg_analyzer_t;

/* a frame's judged lights in analyze (the API's lights_*) */
struct hdrg_lights {
    uint32_t magic;
    const hdrg_allocator_t *alloc;
    const hdrg_source_t *src;           /* the analyzed frame (its pixels NULL: on the GPU alone) */
    const hdrg_light_record_t *records; /* count of them */
    float *chances;                     /* each 1 unless the class sets it */
    uint16_t *frame;                    /* lights_frame's (NULL: not asked) */
    uint32_t count;
};

/* entry's instance (*out zeroed first: closing it is safe); HDRG_ERR_UNSUPPORTED kept */
extern hdrg_status_t hdrg_analyzer_open(const hdrg_filter_entry_t *entry, hdrg_plugin_host_t *host,
                                        hdrg_analyzer_t *out);
/* the chances of count lights of src (hdrg_analyze_opts_t.judge: user the analyzer) */
extern hdrg_status_t hdrg_analyzer_judge(void *user, const hdrg_source_t *src,
                                         const hdrg_light_record_t *records, uint32_t count,
                                         float *chances);
extern void hdrg_analyzer_close(hdrg_analyzer_t *analyzer);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_get(const hdrg_lights_t *lights,
                                                          const hdrg_light_record_t **records,
                                                          uint32_t *count);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_frame(hdrg_lights_t *lights,
                                                            const uint16_t **rgb, size_t *stride,
                                                            uint32_t *width, uint32_t *height);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_lights_set_chance(hdrg_lights_t *lights,
                                                                 uint32_t index, float chance);

#endif /* HDRG_HOST_ANALYZER_H */
