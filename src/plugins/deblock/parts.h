/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_DEBLOCK_PARTS_H
#define HDRG_PLUGINS_DEBLOCK_PARTS_H

/*
 * hdrg.deblock: PREFILTER, pure, bands, deterministic (integer arithmetic). Reads the frame's
 * HDRG_SD_NOISE (the params' deblock_strength's built-in filter); on the GPU <backend>.deblock
 * (equal to it)
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
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_deblock_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api);
/*
 * the thresholds for strength from the frame's HDRG_SD_NOISE in nego (the GPU versions' too), and
 * whether it works on the frame (false without the side data or blocks)
 */
extern hdrg_status_t hdrg_deblock_thresholds(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                             double strength, int32_t *alpha, int32_t *beta,
                                             int32_t *tc, bool *active);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_deblock.c) and its shader (wgsl/deblock.wgsl) */
extern const char hdrg_wgsl_deblock[];
extern hdrg_status_t hdrg_webgpu_deblock_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_deblock.c) and its shaders (Tint's of wgsl/deblock.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_deblock_verticals, hdrg_gk_deblock_horizontals;
extern hdrg_status_t hdrg_gk_deblock_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_DEBLOCK_PARTS_H */
