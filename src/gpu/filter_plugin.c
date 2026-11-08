/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#if defined(HDRG_WITH_WEBGPU)
#include "hdrgrading/gpu/filter_webgpu.h"
#endif
#if defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
#include "hdrgrading/gpu/filter_gk.h"
#endif

#if defined(HDRG_WITH_WEBGPU) || defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
/* the context's device of kind (filter_gpu_share) */
static hdrg_status_t hdrg_plugin_share(hdrg_filter_t *filter, const hdrg_plugin_api_t *api,
                                       uint32_t kind, hdrg_gpu_share_t *share)
{
    memset(share, 0, sizeof(*share));
    share->struct_size = (uint32_t)sizeof(*share);
    share->version = HDRG_GPU_SHARE_VERSION;

    return api->filter_gpu_share(filter, kind, share);
}
#endif

#if defined(HDRG_WITH_WEBGPU)
extern hdrg_status_t hdrg_webgpu_device(hdrg_filter_t *filter, const hdrg_plugin_api_t *api,
                                        hdrg_gpu_t **gpu)
{
    hdrg_gpu_share_t share;
    hdrg_status_t status;

    *gpu = NULL;

    status = hdrg_plugin_share(filter, api, HDRG_DEVICE_WEBGPU, &share);
    if (status != HDRG_OK || !hdrg_gpu_adopt(api, &share, gpu)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return HDRG_OK;
}
#endif

#if defined(HDRG_WITH_METAL) || defined(HDRG_WITH_VULKAN)
extern hdrg_status_t hdrg_gk_plugin_device(hdrg_filter_t *filter, const hdrg_plugin_api_t *api,
                                           const hdrg_gk_t **gk)
{
    const hdrg_gk_backend_t *backend;
    hdrg_gpu_share_t share;
    hdrg_gk_t *made;
    hdrg_status_t status;
    void *user, *object;

    *gk = NULL;

    status = api->filter_class_user(filter, &user);
    if (status != HDRG_OK || user == NULL) {
        return (status != HDRG_OK) ? status : HDRG_ERR_INVALID_STATE;
    }

    /* the backend's object on the context's device: in the library's binary its own, shared */
    backend = (const hdrg_gk_backend_t *)user;
    status = hdrg_plugin_share(filter, api, backend->ops->kind, &share);
    if (status != HDRG_OK || !backend->ops->create(api, share.device, share.queue, &object)) {
        return HDRG_ERR_UNSUPPORTED;
    }

    made = (hdrg_gk_t *)api->mem_alloc(api, sizeof(*made));
    if (made == NULL) {
        backend->ops->destroy(object);

        return HDRG_ERR_NOMEM;
    }

    made->ops = backend->ops;
    made->device = object;
    *gk = made;

    return HDRG_OK;
}

extern void hdrg_gk_plugin_release(const hdrg_plugin_api_t *api, const hdrg_gk_t *gk)
{
    if (gk == NULL) {
        return;
    }

    gk->ops->destroy(gk->device);
    api->mem_free(api, (void *)(uintptr_t)gk);
}
#endif
