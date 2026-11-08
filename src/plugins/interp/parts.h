/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_INTERP_PARTS_H
#define HDRG_PLUGINS_INTERP_PARTS_H

/*
 * hdrg.interp: INTERP (a video session's frames between two input frames), whole RGB16 frames,
 * deterministic: hdrg.interp.repeat (the nearer frame), hdrg.interp.blend (the mix by the phase)
 * and, with HDRG_TEMPORAL_INTERP, hdrg.interp.mc (motion compensated, interp_mc.c) and on the GPU
 * <backend>.interp.mc (equal to it)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#if defined(HDRG_INTERP_MC) && defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if defined(HDRG_INTERP_MC) && (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
#include "hdrgrading/gpu/filter_gk.h"
#endif

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_interp_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api);
/* the class hdrg.interp.mc of the plugin (HDRG_TEMPORAL_INTERP) */
extern hdrg_status_t hdrg_interp_mc_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api);
/*
 * hdrg.interp.mc's (and its GPU versions') output HDRG_SD_MOTION (hdrg_bme_motion_size of one
 * field): its header (the field to the frame after at phase) into side, and the GPU versions'
 * records (two words a block, interp.wgsl's confidence) as its blocks
 */
extern void hdrg_interp_motion_head(uint32_t blocks_w, uint32_t blocks_h, uint32_t phase,
                                    uint8_t *side);
extern void hdrg_interp_motion_records(const uint8_t *records, uint32_t blocks_w, uint32_t blocks_h,
                                       uint8_t *side);

#if defined(HDRG_INTERP_MC) && defined(HDRG_WITH_WEBGPU)
/* the WebGPU class (webgpu_interp.c) and its shader (wgsl/interp.wgsl) */
extern const char hdrg_wgsl_interp[];
extern hdrg_status_t hdrg_webgpu_interp_register(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api);
#endif
#if defined(HDRG_INTERP_MC) && (defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN))
/* the class on a native API (gk_interp.c) and its shaders (Tint's of wgsl/interp.wgsl) */
extern const hdrg_gk_shader_t hdrg_gk_interp_lumas, hdrg_gk_interp_down, hdrg_gk_interp_coarse,
    hdrg_gk_interp_refine, hdrg_gk_interp_median, hdrg_gk_interp_compose, hdrg_gk_interp_confidence;
extern hdrg_status_t hdrg_gk_interp_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                             const hdrg_gk_backend_t *backend);
#endif

#endif /* HDRG_PLUGINS_INTERP_PARTS_H */
