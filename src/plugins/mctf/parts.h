/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_MCTF_PARTS_H
#define HDRG_PLUGINS_MCTF_PARTS_H

/*
 * hdrg.mctf: PREFILTER, pure, deterministic (integers), a time window of 2 frames each way (the
 * params' mctf_strength's built-in filter, a video session's alone); on the GPU <backend>.mctf
 * (equal to it)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/plugins/noise.h"
#include "mctf.h"
#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
#include "hdrgrading/gpu/filter_gk.h"
#endif

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_mctf_init(hdrg_plugin_t *plugin,
                                                            const hdrg_plugin_api_t *api);

#if defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_mctf.c) and its shader (wgsl/mctf.wgsl) */
extern const char hdrg_wgsl_mctf[];
extern hdrg_status_t hdrg_webgpu_mctf_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api);
#endif
#if (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_mctf.c) and its shaders (Tint's of wgsl/mctf.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_mctf_lumas, hdrg_gk_mctf_down, hdrg_gk_mctf_coarse,
    hdrg_gk_mctf_refine, hdrg_gk_mctf_median, hdrg_gk_mctf_blocks, hdrg_gk_mctf_average;
extern hdrg_status_t hdrg_gk_mctf_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_MCTF_PARTS_H */
