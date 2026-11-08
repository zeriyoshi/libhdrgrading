/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/gpu/filter_gk.h"

/* the Vulkan filters' backend (HDRG_WITH_VULKAN) */

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_vulkan_frame_alloc(void *inst, uint32_t format,
                                                              uint32_t width, uint32_t height,
                                                              uint8_t **planes, size_t *strides,
                                                              void **handle,
                                                              const hdrg_plugin_api_t *api)
{
    (void)api;

    return hdrg_gk_frame_alloc(*(const hdrg_gk_t *const *)inst, format, width, height, planes,
                               strides, handle);
}

static void HDRG_PLUGIN_CALL hdrg_vulkan_frame_free(void *inst, void *handle,
                                                    const hdrg_plugin_api_t *api)
{
    (void)api;

    hdrg_gk_frame_free(*(const hdrg_gk_t *const *)inst, handle);
}

/* the backend: its frames its own functions (the host hands them between its filters) */
const hdrg_gk_backend_t hdrg_gk_vulkan_backend = {
    &hdrg_gk_vulkan,
    "hdrg.vulkan",
    hdrg_vulkan_frame_alloc,
    hdrg_vulkan_frame_free,
};
