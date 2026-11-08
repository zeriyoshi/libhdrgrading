/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_DENOISE_PARTS_H
#define HDRG_PLUGINS_DENOISE_PARTS_H

/*
 * hdrg.denoise.spatial: PREFILTER, pure, bands, deterministic. Reads the frame's HDRG_SD_NOISE (the
 * REDUCE grain policy's built-in filter); on the GPU <backend>.denoise (within the L2 tolerance)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/plugins/noise.h"
#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
#include "hdrgrading/gpu/filter_gk.h"
#endif

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_denoise_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_denoise.c) and its shader (wgsl/denoise.wgsl) */
extern const char hdrg_wgsl_denoise[];
extern hdrg_status_t hdrg_webgpu_denoise_register(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_denoise.c) and its shaders (Tint's of wgsl/denoise.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_denoise_coefs, hdrg_gk_denoise_guide;
extern hdrg_status_t hdrg_gk_denoise_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                              const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_DENOISE_PARTS_H */
