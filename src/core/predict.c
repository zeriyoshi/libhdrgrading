/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "predict.h"

#include <string.h>

#include "hdrgrading/foundation/math.h"
#include "../priv.h"

/* the cells around a light's core box its surround spans */
#define HDRG_PREDICT_SURROUND 2u

extern void hdrg_predict_features(const hdrg_analysis_t *analysis, const float *cells, uint32_t qw,
                                  uint32_t qh, uint32_t k, float out[HDRG_LIGHT_FEATURES])
{
    const hdrg_light_t *light = &analysis->lights[k];
    const size_t total = (size_t)qw * qh;
    uint32_t x, y, x0, y0, x1, y1, n, m;
    size_t i;
    float sum, brightest, channel, chroma, around;

    n = 0;
    m = 0;
    sum = 0.0f;
    brightest = 0.0f;
    channel = 0.0f;
    chroma = 0.0f;
    around = 0.0f;

    for (y = light->y0; y <= light->y1 && y < qh; y++) {
        for (x = light->x0; x <= light->x1 && x < qw; x++) {
            i = (size_t)y * qw + x;
            sum += cells[i];
            brightest = HDRG_MAX(brightest, cells[total + i]);
            channel = HDRG_MAX(channel, cells[2u * total + i]);
            chroma += cells[4u * total + i];
            n++;
        }
    }

    x0 = (light->x0 > HDRG_PREDICT_SURROUND) ? light->x0 - HDRG_PREDICT_SURROUND : 0u;
    y0 = (light->y0 > HDRG_PREDICT_SURROUND) ? light->y0 - HDRG_PREDICT_SURROUND : 0u;
    x1 = HDRG_MIN(qw - 1u, light->x1 + HDRG_PREDICT_SURROUND);
    y1 = HDRG_MIN(qh - 1u, light->y1 + HDRG_PREDICT_SURROUND);

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            if (x >= light->x0 && x <= light->x1 && y >= light->y0 && y <= light->y1) {
                continue;
            }

            around += cells[(size_t)y * qw + x];
            m++;
        }
    }

    sum = (n > 0) ? sum / (float)n : 0.0f;
    around = (m > 0) ? around / (float)m : sum;
    out[0] = hdrg_math_log2f(1.0f + (float)n) / 8.0f;
    out[1] = sum;
    out[2] = brightest;
    out[3] = channel;
    out[4] = (n > 0) ? chroma / (float)n : 0.0f;
    out[5] = around;
    out[6] = sum - around;
    out[7] = light->boost;
    out[8] = hdrg_math_log2f(HDRG_MAX(analysis->key_luma, HDRG_KEY_FLOOR)) / 8.0f;
    out[9] = analysis->p995_luma;
}

extern hdrg_status_t hdrg_predict_lights(const hdrg_allocator_t *alloc, hdrg_analysis_t *analysis,
                                         const hdrg_source_t *src, const float *cells, uint32_t qw,
                                         uint32_t qh, uint8_t shift,
                                         const hdrg_analyze_opts_t *opts)
{
    const hdrg_light_t *light;
    const size_t total = (size_t)qw * qh;
    const bool judged = opts->predictor > 0.0f && opts->judge != NULL;
    const float amount = opts->predictor;
    hdrg_light_record_t *records;
    hdrg_status_t status;
    uint32_t k;
    size_t c;
    float *features, *chances;

    if (analysis->lights == NULL || analysis->light_of == NULL || analysis->light_count == 0) {
        return HDRG_OK;
    }

    features = (float *)hdrg_mem_alloc(alloc, (size_t)analysis->light_count *
                                                  (HDRG_LIGHT_FEATURES + 1u) * sizeof(float));
    records = judged ? (hdrg_light_record_t *)hdrg_mem_alloc(alloc, (size_t)analysis->light_count *
                                                                        sizeof(hdrg_light_record_t))
                     : NULL;
    if (features == NULL || (judged && records == NULL)) {
        hdrg_mem_free(alloc, features);
        hdrg_mem_free(alloc, records);

        return HDRG_ERR_NOMEM;
    }

    chances = features + (size_t)analysis->light_count * HDRG_LIGHT_FEATURES;

    for (k = 0; k < analysis->light_count; k++) {
        hdrg_predict_features(analysis, cells, qw, qh, k,
                              features + (size_t)k * HDRG_LIGHT_FEATURES);
    }

    /* each light's record: its core's box in the analyzed frame's pixels, boost and features */
    for (k = 0; judged && k < analysis->light_count; k++) {
        light = &analysis->lights[k];
        records[k].x = HDRG_MIN(light->x0 << shift, src->width - 1u);
        records[k].y = HDRG_MIN(light->y0 << shift, src->height - 1u);
        records[k].width = HDRG_MIN((light->x1 + 1u) << shift, src->width) - records[k].x;
        records[k].height = HDRG_MIN((light->y1 + 1u) << shift, src->height) - records[k].y;
        records[k].boost = light->boost;
        memcpy(records[k].features, features + (size_t)k * HDRG_LIGHT_FEATURES,
               sizeof(records[k].features));
    }

    status = judged ? opts->judge(opts->judge_user, src, records, analysis->light_count, chances)
                    : HDRG_OK;
    hdrg_mem_free(alloc, records);

    if (status != HDRG_OK) {
        hdrg_mem_free(alloc, features);

        return status;
    }

    /* the boost scaled by the chance: lowered where in doubt, never raised */
    if (judged) {
        for (k = 0; k < analysis->light_count; k++) {
            chances[k] = 1.0f - amount + amount * chances[k];
            analysis->lights[k].boost *= chances[k];
        }

        for (c = 0; analysis->light.data != NULL && c < total; c++) {
            if (analysis->light_of[c] != 0u && analysis->light_of[c] <= analysis->light_count) {
                analysis->light.data[c] *= chances[analysis->light_of[c] - 1u];
            }
        }
    }

    if (opts->features) {
        analysis->light_features = features;
    } else {
        hdrg_mem_free(alloc, features);
    }

    return HDRG_OK;
}
