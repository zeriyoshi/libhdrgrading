/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * A module built against a newer plugin ABI than the library: the host
 * must reject it at load without calling its entry point (which aborts).
 */
#include <stdlib.h>

#include "hdrgrading/filter.h"

HDRG_PLUGIN_EXTERN_C HDRG_PLUGIN_EXPORT uint32_t HDRG_PLUGIN_CALL hdrg_plugin_abi_version(void)
{
    return HDRG_PLUGIN_ABI_VERSION + 1u;
}

HDRG_PLUGIN_EXTERN_C HDRG_PLUGIN_EXPORT hdrg_status_t HDRG_PLUGIN_CALL
hdrg_plugin_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    (void)plugin;
    (void)api;

    abort();
}
