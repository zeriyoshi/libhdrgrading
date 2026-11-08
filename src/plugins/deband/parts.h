/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_DEBAND_PARTS_H
#define HDRG_PLUGINS_DEBAND_PARTS_H

/*
 * hdrg.deband: PREFILTER, pure, bands, deterministic (exact integer sums) (the params'
 * deband_strength's built-in filter); on the GPU <backend>.deband (within the L2 tolerance)
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

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_deband_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api);
/*
 * whether it works on the frame at strength (the GPU versions' too): not at 0, nor where the
 * quietest level's noise of its HDRG_SD_NOISE in nego alone leaves a residual of no weight
 */
extern hdrg_status_t hdrg_deband_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                          double strength, bool *active);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_deband.c) and its shader (wgsl/deband.wgsl) */
extern const char hdrg_wgsl_deband[];
extern hdrg_status_t hdrg_webgpu_deband_register(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_deband.c) and its shaders (Tint's of wgsl/deband.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_deband_lumas, hdrg_gk_deband_edges, hdrg_gk_deband_fit;
extern hdrg_status_t hdrg_gk_deband_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                             const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_DEBAND_PARTS_H */
