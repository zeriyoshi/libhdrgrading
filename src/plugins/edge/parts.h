/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_EDGE_PARTS_H
#define HDRG_PLUGINS_EDGE_PARTS_H

/*
 * hdrg.scale.edge: the library's edge-adaptive upscale (edge.c, edge.h), on the GPU <backend>.edge,
 * and with HDRG_TEMPORAL_SR its multi-frame upscale hdrg.scale.temporal (temporal.c)
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
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_edge_init(hdrg_plugin_t *plugin,
                                                            const hdrg_plugin_api_t *api);
/* hdrg.scale.temporal's class (temporal.c, HDRG_TEMPORAL_SR) */
extern hdrg_status_t hdrg_filter_temporal_register(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_edge.c) and its shader (wgsl/edge.wgsl) */
extern const char hdrg_wgsl_edge[];
extern hdrg_status_t hdrg_webgpu_edge_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_edge.c) and its shaders (Tint's of wgsl/edge.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_edge_luma, hdrg_gk_edge_scale;
extern hdrg_status_t hdrg_gk_edge_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_EDGE_PARTS_H */
