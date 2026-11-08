/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_PREDICT_PARTS_H
#define HDRG_PLUGINS_PREDICT_PARTS_H

/*
 * hdrg.analyze.predict: ANALYZE, deterministic. A light's features
 * (hdrg_light_record_t) through a small multilayer perceptron in fixed
 * point (integers alone: the same bytes on every build and tier) giving
 * the chance it is a light. The weights (predict_weights.c) come from
 * scripts/predictor/train.py
 */
#include <stdint.h>

#include "hdrgrading/filter.h"

#define HDRG_PREDICT_HIDDEN1 16u
#define HDRG_PREDICT_HIDDEN2 8u
#define HDRG_PREDICT_ONE     2048 /* the fixed point's 1 (features, activations, the chance) */
#define HDRG_PREDICT_WEIGHT  64   /* a weight's 1 */

/* the weights: int8 of the weight's 1, biases of the activations' 1 x the weight's */
extern const int8_t hdrg_predict_w1[HDRG_PREDICT_HIDDEN1][HDRG_LIGHT_FEATURES];
extern const int32_t hdrg_predict_b1[HDRG_PREDICT_HIDDEN1];
extern const int8_t hdrg_predict_w2[HDRG_PREDICT_HIDDEN2][HDRG_PREDICT_HIDDEN1];
extern const int32_t hdrg_predict_b2[HDRG_PREDICT_HIDDEN2];
extern const int8_t hdrg_predict_w3[HDRG_PREDICT_HIDDEN2];
extern const int32_t hdrg_predict_b3;

/* the chance (0..HDRG_PREDICT_ONE) of a light's features */
extern int32_t hdrg_predict_chance(const float features[HDRG_LIGHT_FEATURES]);
/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_predict_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_PREDICT_PARTS_H */
