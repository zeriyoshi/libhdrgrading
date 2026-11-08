/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_NIS_PARTS_H
#define HDRG_PLUGINS_NIS_PARTS_H

/*
 * hdrg.scale.nvidia: hdrg.scale.nis, NVIDIA Image Scaling (nis.c, nis.h), and on the GPU
 * <backend>.nis
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
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_nis_init(hdrg_plugin_t *plugin,
                                                           const hdrg_plugin_api_t *api);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_nis.c) and its shader (wgsl/nis.wgsl) */
extern const char hdrg_wgsl_nis[];
extern hdrg_status_t hdrg_webgpu_nis_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_nis.c) and its shaders (Tint's of wgsl/nis.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_nis_luma, hdrg_gk_nis_scale;
extern hdrg_status_t hdrg_gk_nis_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                          const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_NIS_PARTS_H */
