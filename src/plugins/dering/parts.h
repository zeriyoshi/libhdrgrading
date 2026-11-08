/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_DERING_PARTS_H
#define HDRG_PLUGINS_DERING_PARTS_H

/*
 * hdrg.dering: PREFILTER, pure, bands, deterministic (integer arithmetic). Reads the frame's
 * HDRG_SD_NOISE (the params' dering_strength's built-in filter); on the GPU <backend>.dering
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
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_dering_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api);
/*
 * the window radius, range floor and whether it works on the frame (false without the side data
 * or ringing) for strength from the frame's HDRG_SD_NOISE in nego (the GPU versions' too)
 */
extern hdrg_status_t hdrg_dering_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                          double strength, uint32_t *reach, int32_t *floor,
                                          bool *active);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_dering.c) and its shader (wgsl/dering.wgsl) */
extern const char hdrg_wgsl_dering[];
extern hdrg_status_t hdrg_webgpu_dering_register(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_dering.c) and its shaders (Tint's of wgsl/dering.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_dering_contrasts, hdrg_gk_dering_dering;
extern hdrg_status_t hdrg_gk_dering_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                             const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_DERING_PARTS_H */
