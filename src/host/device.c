/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "table.h"

/* the context's own device of kind (hdrg_host_device): its object, NULL when none */
static void *hdrg_host_own_create(const hdrg_plugin_host_t *host, uint32_t kind)
{
#if defined(HDRG_WITH_METAL)
    hdrg_metal_t *metal;
#endif
#if defined(HDRG_WITH_VULKAN)
    hdrg_vulkan_t *vulkan;
#endif
#if defined(HDRG_WITH_WEBGPU)
    hdrg_gpu_t *gpu;
#endif

    (void)host;

#if defined(HDRG_WITH_METAL)
    if (kind == HDRG_DEVICE_METAL) {
        return hdrg_metal_create(&host->api, NULL, NULL, &metal) ? (void *)metal : NULL;
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (kind == HDRG_DEVICE_VULKAN) {
        return hdrg_vulkan_create(&host->api, NULL, NULL, &vulkan) ? (void *)vulkan : NULL;
    }
#endif

#if defined(HDRG_WITH_WEBGPU)
    if (kind == HDRG_DEVICE_WEBGPU) {
        return hdrg_gpu_create(&host->api, NULL, &gpu) ? (void *)gpu : NULL;
    }
#endif

    (void)kind;

    return NULL;
}

/*
 * the context's object of kind as its share (share_lock): the own device's, or one of its own
 * around the app's device (own->wrapped, made on the first ask, destroyed with the host); NULL:
 * none
 */
static void *hdrg_host_share_object(const hdrg_plugin_host_t *host, uint32_t kind)
{
    hdrg_host_own_t *own = host->own;
    hdrg_host_retired_t *grown;
    void *device, *queue, *object;

    if (!hdrg_host_device(host, kind, &device, &queue)) {
        return NULL;
    }

    if (host->devices[kind - 1u][0] == NULL) {
        return own->objects[kind - 1u];
    }

    if (own->wrap_tried[kind - 1u] && own->wrapped_of[kind - 1u][0] == device &&
        own->wrapped_of[kind - 1u][1] == queue) {
        return own->wrapped[kind - 1u];
    }

    /* the app set another device: the old one's wrap kept to the end, the new one wrapped */
    if (own->wrapped[kind - 1u] != NULL) {
        grown = (hdrg_host_retired_t *)hdrg_mem_realloc(
            &host->alloc, own->retired, (own->retired_count + 1u) * sizeof(*own->retired));
        if (grown == NULL) {
            return NULL;
        }

        own->retired = grown;
        own->retired[own->retired_count].object = own->wrapped[kind - 1u];
        own->retired[own->retired_count].kind = kind;
        own->retired_count++;
        own->wrapped[kind - 1u] = NULL;
    }

    own->wrap_tried[kind - 1u] = true;
    own->wrapped_of[kind - 1u][0] = device;
    own->wrapped_of[kind - 1u][1] = queue;
    object = NULL;

#if defined(HDRG_WITH_METAL)
    if (kind == HDRG_DEVICE_METAL &&
        !hdrg_metal_create(&host->api, device, queue, (hdrg_metal_t **)&object)) {
        object = NULL;
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (kind == HDRG_DEVICE_VULKAN &&
        !hdrg_vulkan_create(&host->api, (const hdrg_vulkan_device_t *)device, queue,
                            (hdrg_vulkan_t **)&object)) {
        object = NULL;
    }
#endif

#if defined(HDRG_WITH_WEBGPU)
    if (kind == HDRG_DEVICE_WEBGPU &&
        !hdrg_gpu_create(&host->api, (WGPUDevice)device, (hdrg_gpu_t **)&object)) {
        object = NULL;
    }
#endif

    (void)queue;
    own->wrapped[kind - 1u] = object;

    return object;
}

/* a wrap of the app's device of kind destroyed */
static void hdrg_host_wrap_destroy(void *object, uint32_t kind)
{
#if defined(HDRG_WITH_METAL)
    if (kind == HDRG_DEVICE_METAL) {
        hdrg_metal_destroy((hdrg_metal_t *)object);
    }

#endif
#if defined(HDRG_WITH_VULKAN)
    if (kind == HDRG_DEVICE_VULKAN) {
        hdrg_vulkan_destroy((hdrg_vulkan_t *)object);
    }

#endif
#if defined(HDRG_WITH_WEBGPU)
    if (kind == HDRG_DEVICE_WEBGPU) {
        hdrg_gpu_destroy((hdrg_gpu_t *)object);
    }
#endif

    (void)object;
    (void)kind;
}

extern void hdrg_host_wrapped_destroy(hdrg_plugin_host_t *host)
{
    hdrg_host_own_t *own = host->own;
    uint32_t i;

    for (i = 0; i < HDRG_DEVICE_KINDS; i++) {
        hdrg_host_wrap_destroy(own->wrapped[i], i + 1u);
    }

    for (i = 0; i < own->retired_count; i++) {
        hdrg_host_wrap_destroy(own->retired[i].object, own->retired[i].kind);
    }

    hdrg_mem_free(&host->alloc, own->retired);
    own->retired = NULL;
    own->retired_count = 0;
    memset(own->wrapped, 0, sizeof(own->wrapped));
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_device(const hdrg_filter_t *filter,
                                                             uint32_t kind, void **device,
                                                             void **queue)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || kind == 0 ||
        kind > HDRG_DEVICE_KINDS || device == NULL || queue == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (filter->host == NULL || !hdrg_host_device(filter->host, kind, device, queue)) {
        return HDRG_ERR_NOT_FOUND;
    }

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_filter_gpu_share(const hdrg_filter_t *filter,
                                                                uint32_t kind, void *out)
{
    hdrg_gpu_share_t *share = (hdrg_gpu_share_t *)out;
    hdrg_host_own_t *own;
    void *object;

    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || kind == 0 ||
        kind > HDRG_DEVICE_KINDS || share == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* a plugin built against another share: nothing it could read */
    if (share->struct_size < sizeof(*share) || share->version != HDRG_GPU_SHARE_VERSION) {
        return HDRG_ERR_UNSUPPORTED;
    }

    share->kind = kind;
    share->device = NULL;
    share->queue = NULL;
    share->instance = NULL;
    share->run = NULL;
    share->user = NULL;
    own = (filter->host != NULL) ? filter->host->own : NULL;

    if (own == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    hdrg_mutex_lock(&own->share_lock);
    object = hdrg_host_share_object(filter->host, kind);
    hdrg_mutex_unlock(&own->share_lock);

    if (object == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

#if defined(HDRG_WITH_METAL)
    if (kind == HDRG_DEVICE_METAL) {
        share->device = hdrg_metal_device_object((const hdrg_metal_t *)object);
        share->queue = hdrg_metal_queue_object((const hdrg_metal_t *)object);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (kind == HDRG_DEVICE_VULKAN) {
        share->device = (void *)(uintptr_t)hdrg_vulkan_share((const hdrg_vulkan_t *)object);
        share->queue = hdrg_vulkan_queue((const hdrg_vulkan_t *)object);
    }
#endif

#if defined(HDRG_WITH_WEBGPU)
    if (kind == HDRG_DEVICE_WEBGPU) {
        hdrg_gpu_share((hdrg_gpu_t *)object, share);
    }
#endif

    return (share->device != NULL) ? HDRG_OK : HDRG_ERR_UNSUPPORTED;
}

extern bool hdrg_host_device(const hdrg_plugin_host_t *host, uint32_t kind, void **device,
                             void **queue)
{
    hdrg_host_own_t *own = host->own;
    void *object;

    *device = NULL;
    *queue = NULL;

    if (kind == 0 || kind > HDRG_DEVICE_KINDS) {
        return false;
    }

    if (host->devices[kind - 1u][0] != NULL) {
        *device = host->devices[kind - 1u][0];
        *queue = host->devices[kind - 1u][1];

        return true;
    }

    if (own == NULL) {
        return false;
    }

    hdrg_mutex_lock(&own->lock);

    if (!own->tried[kind - 1u]) {
        own->tried[kind - 1u] = true;
        own->objects[kind - 1u] = hdrg_host_own_create(host, kind);
    }

    object = own->objects[kind - 1u];
    hdrg_mutex_unlock(&own->lock);

    if (object == NULL) {
        return false;
    }

#if defined(HDRG_WITH_METAL)
    if (kind == HDRG_DEVICE_METAL) {
        *device = hdrg_metal_device_object((const hdrg_metal_t *)object);
        *queue = hdrg_metal_queue_object((const hdrg_metal_t *)object);
    }
#endif

#if defined(HDRG_WITH_VULKAN)
    if (kind == HDRG_DEVICE_VULKAN) {
        *device = (void *)(uintptr_t)hdrg_vulkan_share((const hdrg_vulkan_t *)object);
        *queue = hdrg_vulkan_queue((const hdrg_vulkan_t *)object);
    }
#endif

#if defined(HDRG_WITH_WEBGPU)
    if (kind == HDRG_DEVICE_WEBGPU) {
        *device = (void *)hdrg_gpu_device((const hdrg_gpu_t *)object);
    }
#endif

    return *device != NULL;
}
