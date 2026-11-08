/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TEST_STAGE_H
#define HDRG_TEST_STAGE_H

#include <string.h>

#include "host/stage.h"

/* a stage created and configured for one frame of noise (closed on failure) */
static inline hdrg_status_t hdrg_test_stage_open(const hdrg_allocator_t *alloc,
                                                 const hdrg_filter_entry_t *entry,
                                                 const hdrg_source_t *src, uint32_t out_width,
                                                 uint32_t out_height, uint32_t max_band_rows,
                                                 const float *noise, hdrg_plugin_host_t *host,
                                                 hdrg_stage_t *out)
{
    hdrg_stage_frame_t frame;
    hdrg_status_t status;

    memset(&frame, 0, sizeof(frame));
    frame.noise = noise;

    status = hdrg_stage_create(alloc, entry, max_band_rows, host, out);
    if (status != HDRG_OK) {
        return status;
    }

    status = hdrg_stage_configure(out, src, out_width, out_height, &frame, host);
    if (status != HDRG_OK) {
        hdrg_stage_close(out);
    }

    return status;
}

#endif
