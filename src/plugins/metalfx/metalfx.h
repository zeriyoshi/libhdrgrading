/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_METALFX_H
#define HDRG_PLUGINS_METALFX_H

/*
 * hdrg.metalfx (HDRG_WITH_METAL): the class hdrg.metal.metalfx, MetalFX's spatial scaler as a
 * SCALE filter on the GPU layer's Metal backend (hdrgrading/gpu/filter_gk.h)
 */
#include "hdrgrading/gpu/filter_gk.h"
#include "hdrgrading/gpu/metal.h"

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_metalfx_init(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_METALFX_H */
