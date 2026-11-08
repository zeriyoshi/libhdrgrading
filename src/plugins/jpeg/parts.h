/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_JPEG_PARTS_H
#define HDRG_PLUGINS_JPEG_PARTS_H

/*
 * hdrg.jpeg (jpegli): reads JPEG (its 4:2:0 / 4:2:2 chroma through the library's chroma from
 * luma, JFIF siting; ICC, Exif), writes the three forms (jpeg.c, jpeg_mux.c: the gain map JPEG's
 * MPF, ISO 21496-1 and XMP)
 */
#include "hdrgrading/filter.h"

#define HDRG_CODEC_LIT(s) s, sizeof(s) - 1u

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_codec_jpeg_init(hdrg_plugin_t *plugin,
                                                           const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_JPEG_PARTS_H */
