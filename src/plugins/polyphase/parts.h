/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_POLYPHASE_PARTS_H
#define HDRG_PLUGINS_POLYPHASE_PARTS_H

/*
 * hdrg.scale.polyphase: the classes hdrg.scale.lanczos (the default scaler) and hdrg.scale.bicubic
 * (SCALE, pure, bands, deterministic: integer passes) and, on the GPU, <backend>.lanczos (SCALE,
 * pure, ASYNC: equal to hdrg.scale.lanczos)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
#include "hdrgrading/gpu/filter_gk.h"
#endif

#define HDRG_POLYPHASE_SHIFT 14u /* the weights: Q14 ... */
#define HDRG_POLYPHASE_EXTRA 2u  /* ... the vertical pass's rows: two bits more ... */
#define HDRG_POLYPHASE_AR    8u  /* ... and the anti-ringing strength: Q8 */

/* an axis of the resampling (hdrg_polyphase_axis) */
typedef struct {
    uint32_t *index; /* taps per output sample: source indices, edges repeated */
    int32_t *weight; /* Q HDRG_POLYPHASE_SHIFT, summing to one */
    uint32_t *near;  /* per output sample: the source samples on either side of it */
    uint32_t taps;
    uint32_t n, m; /* made for n samples to m (0: none) */
    bool upscale;  /* anti-ringing on this axis */
} hdrg_polyphase_axis_t;

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_polyphase_init(hdrg_plugin_t *plugin,
                                                                 const hdrg_plugin_api_t *api);
/*
 * the resampling of an axis of n samples to m (its GPU versions' too): per output sample, taps
 * source indices (edges repeated) and weights in Q HDRG_POLYPHASE_SHIFT summing to one exactly,
 * the two source samples around it, and whether the axis upscales (anti-ringing). The kernel:
 * Lanczos of lobes, or (lobes 0) the bicubic of b and c
 */
extern hdrg_status_t hdrg_polyphase_axis(const hdrg_plugin_api_t *api, uint8_t lobes, double b,
                                         double c, uint32_t n, uint32_t m,
                                         hdrg_polyphase_axis_t *axis);
extern void hdrg_polyphase_axis_free(const hdrg_plugin_api_t *api, hdrg_polyphase_axis_t *axis);
/*
 * an out_w x out_h resampling's rows and cols packed for a shader: the rows' indices, weights
 * and near samples, then the columns', starting at offsets[0..5] of the table (*count int32
 * values, freed with mem_free)
 */
extern int32_t *hdrg_polyphase_pack(const hdrg_plugin_api_t *api, const hdrg_polyphase_axis_t *rows,
                                    const hdrg_polyphase_axis_t *cols, uint32_t out_w,
                                    uint32_t out_h, uint32_t offsets[6], size_t *count);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_lanczos.c) and its shader (wgsl/lanczos.wgsl) */
extern const char hdrg_wgsl_lanczos[];
extern hdrg_status_t hdrg_webgpu_lanczos_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_lanczos.c) and its shaders (Tint's of wgsl/lanczos.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_lanczos_vertical, hdrg_gk_lanczos_horizontal;
extern hdrg_status_t hdrg_gk_lanczos_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_POLYPHASE_PARTS_H */
