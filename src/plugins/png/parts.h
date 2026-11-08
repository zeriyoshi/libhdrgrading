/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_PNG_PARTS_H
#define HDRG_PLUGINS_PNG_PARTS_H

/*
 * hdrg.png (libpng): reads PNG (cICP > iCCP > sRGB > cHRM / gAMA)
 */
#include "hdrgrading/filter.h"

#define HDRG_CODEC_LIT(s) s, sizeof(s) - 1u

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_png_init(hdrg_plugin_t *plugin,
                                                          const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_PNG_PARTS_H */
