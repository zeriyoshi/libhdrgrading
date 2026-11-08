/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_BILINEAR_PARTS_H
#define HDRG_PLUGINS_BILINEAR_PARTS_H

/*
 * hdrg.scale.bilinear: SCALE, pure, bands, deterministic (16.16 fixed point)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_bilinear_init(hdrg_plugin_t *plugin,
                                                                const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_BILINEAR_PARTS_H */
