/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_DEFRINGE_PARTS_H
#define HDRG_PLUGINS_DEFRINGE_PARTS_H

/*
 * hdrg.defringe: PREFILTER, pure, bands, deterministic (integer arithmetic). Reads the frame's
 * HDRG_SD_NOISE (the params' defringe_strength's built-in filter); on the GPU <backend>.defringe
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
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_defringe_init(hdrg_plugin_t *plugin,
                                                                const hdrg_plugin_api_t *api);
/*
 * the window reach for strength and whether it works on the frame (the ringing of a coarse
 * quantizer in its HDRG_SD_NOISE in nego; the GPU versions' too)
 */
extern hdrg_status_t hdrg_defringe_settings(hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api,
                                            double strength, uint32_t *reach, bool *active);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_defringe.c) and its shader (wgsl/defringe.wgsl) */
extern const char hdrg_wgsl_defringe[];
extern hdrg_status_t hdrg_webgpu_defringe_register(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_defringe.c) and its shaders (Tint's of wgsl/defringe.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_defringe_planes, hdrg_gk_defringe_edges,
    hdrg_gk_defringe_wides, hdrg_gk_defringe_fringe;
extern hdrg_status_t hdrg_gk_defringe_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                               const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_DEFRINGE_PARTS_H */
