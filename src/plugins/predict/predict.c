/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "parts.h"

#define HDRG_LIT(s)              s, sizeof(s) - 1u
#define HDRG_PR_ID               "hdrg.analyze.predict"
#define HDRG_PR_LICENSE          "Apache-2.0 OR MIT"
#define HDRG_PR_COPY             "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_PR_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define HDRG_PR_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
/* the features' range in the fixed point (|feature| <= 8) */
#define HDRG_PREDICT_LIMIT (8 * HDRG_PREDICT_ONE)

/* a feature in the fixed point: clamped to +-8, rounded half up */
static inline int32_t hdrg_predict_fixed(float value)
{
    float v;

    v = HDRG_PR_CLAMP(value, -8.0f, 8.0f) * (float)HDRG_PREDICT_ONE;

    return (int32_t)(v + (float)HDRG_PREDICT_LIMIT + 0.5f) - HDRG_PREDICT_LIMIT;
}

/* a ReLU layer's activation in the fixed point (from the weights' and the inputs' product) */
static inline int32_t hdrg_predict_relu(int32_t acc)
{
    return (acc > 0) ? HDRG_PR_MIN(acc / HDRG_PREDICT_WEIGHT, HDRG_PREDICT_LIMIT) : 0;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pr_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    (void)api;

    *inst = NULL;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_pr_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

/* each light's chance of its features: the integer's, exactly (a power of two's fraction) */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_pr_analyze(void *inst, hdrg_lights_t *lights,
                                                      const hdrg_plugin_api_t *api)
{
    const hdrg_light_record_t *records;
    hdrg_status_t status;
    uint32_t count, k;

    (void)inst;

    status = api->lights_get(lights, &records, &count);

    for (k = 0; status == HDRG_OK && k < count; k++) {
        status = api->lights_set_chance(
            lights, k, (float)hdrg_predict_chance(records[k].features) / (float)HDRG_PREDICT_ONE);
    }

    return status;
}

extern int32_t hdrg_predict_chance(const float features[HDRG_LIGHT_FEATURES])
{
    uint32_t i, j;
    int32_t in[HDRG_LIGHT_FEATURES], h1[HDRG_PREDICT_HIDDEN1], h2[HDRG_PREDICT_HIDDEN2], acc;

    for (i = 0; i < HDRG_LIGHT_FEATURES; i++) {
        in[i] = hdrg_predict_fixed(features[i]);
    }

    for (j = 0; j < HDRG_PREDICT_HIDDEN1; j++) {
        acc = hdrg_predict_b1[j];

        for (i = 0; i < HDRG_LIGHT_FEATURES; i++) {
            acc += (int32_t)hdrg_predict_w1[j][i] * in[i];
        }

        h1[j] = hdrg_predict_relu(acc);
    }

    for (j = 0; j < HDRG_PREDICT_HIDDEN2; j++) {
        acc = hdrg_predict_b2[j];

        for (i = 0; i < HDRG_PREDICT_HIDDEN1; i++) {
            acc += (int32_t)hdrg_predict_w2[j][i] * h1[i];
        }

        h2[j] = hdrg_predict_relu(acc);
    }

    acc = hdrg_predict_b3;

    for (i = 0; i < HDRG_PREDICT_HIDDEN2; i++) {
        acc += (int32_t)hdrg_predict_w3[i] * h2[i];
    }

    return HDRG_PR_CLAMP(acc / HDRG_PREDICT_WEIGHT, 0, HDRG_PREDICT_ONE);
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_predict_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_PR_ID), HDRG_LIT("Light predictor"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_PR_LICENSE), HDRG_LIT(HDRG_PR_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, HDRG_LIT(HDRG_PR_ID));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_ANALYZE);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, hdrg_pr_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, hdrg_pr_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_analyze(cls, hdrg_pr_analyze) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}
