/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PLUGINS_STATS_PARTS_H
#define HDRG_PLUGINS_STATS_PARTS_H

/*
 * hdrg.stats.temporal: STATS, deterministic. A video session's statistics
 * across its frames: WINDOWED's mean of the anchor statistics and the APL
 * factor over its scene's window, RECURSIVE's asymmetric EMA, TWO_PASS's
 * scene means of the first pass; the darkening runs (a fade-out holds the
 * statistics of the frame before it); the lights steadied over the frames
 * (the median of their judgments, PLAYBACK and OFFLINE by place moved with
 * the camera, TWO_PASS on the tracks of the first pass)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

/* the plugin's entry point */
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_stats_init(hdrg_plugin_t *plugin,
                                                             const hdrg_plugin_api_t *api);

#endif /* HDRG_PLUGINS_STATS_PARTS_H */
