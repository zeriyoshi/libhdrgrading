/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_JXL_PARTS_H
#define HDRG_PLUGINS_JXL_PARTS_H

/*
 * hdrg.jxl (the bundled libjxl): writes JPEG XL in the three forms, HYBRID by default (the PQ
 * base and the jhgm gain map back to the SDR rendition); no reader
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

#define HDRG_CODEC_LIT(s) s, sizeof(s) - 1u

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_jxl_init(hdrg_plugin_t *plugin,
                                                          const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_JXL_PARTS_H */
