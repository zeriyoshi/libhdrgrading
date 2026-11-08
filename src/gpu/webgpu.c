/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/gpu/webgpu.h"

#include <string.h>

#include "hdrgrading/foundation/thread.h"

/*
 * a browser's WebGPU (emdawnwebgpu, a single thread): no device thread, the
 * work runs where it is asked for and its waits suspend the calling export
 * through JSPI (cmake/HdrgWasm.cmake's HDRG_WASM_WEBGPU)
 */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define HDRG_GPU_INLINE 1
#else
#define HDRG_GPU_INLINE 0
#endif

typedef struct hdrg_gpu_item hdrg_gpu_item_t;

struct hdrg_gpu_item {
    hdrg_gpu_t *gpu; /* whose work */
    hdrg_gpu_work_fn_t fn;
    void *arg;
    hdrg_gpu_item_t *next;
    bool owned; /* made by hdrg_gpu_run (freed after it ran); else its caller's (a call's) */
};

struct hdrg_gpu {
    const hdrg_plugin_api_t *api; /* its allocator */
    hdrg_gpu_t *next;             /* the shared ones (hdrg_gpu_lock) ... */
    uint32_t refs;                /* ... and their holders */
    WGPUInstance instance;
    WGPUAdapter adapter; /* NULL: the app's device */
    WGPUDevice device;
    WGPUQueue queue;
    hdrg_thread_t thread;
    hdrg_mutex_t mutex;
    hdrg_cond_t wake;
    hdrg_gpu_item_t *head, *tail; /* the work queued (mutex) */
    hdrg_atomic_u32_t lost;       /* the device was lost (any thread) */
    bool stopping;                /* mutex */
    bool strict; /* the shaders compiled with strict math (ShaderModuleCompilationOptions) */
    /* another binary's device (hdrg_gpu_adopt): its work on that one's thread (NULL: its own) */
    hdrg_status_t(HDRG_PLUGIN_CALL *run)(void *user, hdrg_gpu_share_fn_t fn, void *arg);
    void *user;
};

/* a work item hdrg_gpu_call waits for (its queue item its own) */
typedef struct {
    hdrg_gpu_item_t item;
    hdrg_gpu_work_fn_t fn;
    void *arg;
    hdrg_mutex_t mutex;
    hdrg_cond_t done;
    bool finished; /* mutex */
} hdrg_gpu_call_t;

/* a share's work (hdrg_gpu_share's run) */
typedef struct {
    hdrg_gpu_share_fn_t fn;
    void *arg;
} hdrg_gpu_shared_t;

/* what a callback hands back to the waiting thread */
typedef struct {
    void *object;
    bool ok;
} hdrg_gpu_result_t;

/*
 * every hdrg_gpu_t, by its device and table: a device is used from one
 * thread, whoever asks for it with the same table (the filters and the core
 * of a context: hdrg_host_device)
 */
static hdrg_atomic_u32_t hdrg_gpu_registry_lock;
static hdrg_gpu_t *hdrg_gpu_registry;

static void hdrg_gpu_lock(void)
{
    uint32_t expected;

    do {
        expected = 0;
    } while (!hdrg_atomic_u32_cas_acq_rel(&hdrg_gpu_registry_lock, &expected, 1u));
}

static void hdrg_gpu_unlock(void)
{
    hdrg_atomic_u32_store_release(&hdrg_gpu_registry_lock, 0u);
}

/* the one on the device for the table (a reference more), else NULL */
static hdrg_gpu_t *hdrg_gpu_find(const hdrg_plugin_api_t *api, WGPUDevice device)
{
    hdrg_gpu_t *gpu;

    hdrg_gpu_lock();

    for (gpu = hdrg_gpu_registry; gpu != NULL; gpu = gpu->next) {
        if (gpu->device == device && gpu->api == api) {
            gpu->refs++;

            break;
        }
    }

    hdrg_gpu_unlock();

    return gpu;
}

static void hdrg_gpu_register(hdrg_gpu_t *gpu)
{
    hdrg_gpu_lock();
    gpu->next = hdrg_gpu_registry;
    hdrg_gpu_registry = gpu;
    hdrg_gpu_unlock();
}

static void hdrg_gpu_on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                WGPUStringView message, void *userdata1, void *userdata2)
{
    hdrg_gpu_result_t *result = (hdrg_gpu_result_t *)userdata1;

    (void)message;
    (void)userdata2;

    result->ok = status == WGPURequestAdapterStatus_Success && adapter != NULL;
    result->object = result->ok ? (void *)adapter : NULL;
}

static void hdrg_gpu_on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                               WGPUStringView message, void *userdata1, void *userdata2)
{
    hdrg_gpu_result_t *result = (hdrg_gpu_result_t *)userdata1;

    (void)message;
    (void)userdata2;

    result->ok = status == WGPURequestDeviceStatus_Success && device != NULL;
    result->object = result->ok ? (void *)device : NULL;
}

static void hdrg_gpu_on_lost(WGPUDevice const *device, WGPUDeviceLostReason reason,
                             WGPUStringView message, void *userdata1, void *userdata2)
{
    hdrg_gpu_t *gpu = (hdrg_gpu_t *)userdata1;

    (void)device;
    (void)message;
    (void)userdata2;

    if (reason != WGPUDeviceLostReason_Destroyed &&
        reason != WGPUDeviceLostReason_CallbackCancelled) {
        hdrg_atomic_u32_store_release(&gpu->lost, 1u);
    }
}

static void hdrg_gpu_on_scope(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                              WGPUStringView message, void *userdata1, void *userdata2)
{
    hdrg_gpu_result_t *result = (hdrg_gpu_result_t *)userdata1;

    (void)message;
    (void)userdata2;

    result->ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
}

static void hdrg_gpu_on_done(WGPUQueueWorkDoneStatus status, WGPUStringView message,
                             void *userdata1, void *userdata2)
{
    hdrg_gpu_result_t *result = (hdrg_gpu_result_t *)userdata1;

    (void)message;
    (void)userdata2;

    result->ok = status == WGPUQueueWorkDoneStatus_Success;
}

static void hdrg_gpu_on_map(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1,
                            void *userdata2)
{
    hdrg_gpu_result_t *result = (hdrg_gpu_result_t *)userdata1;

    (void)message;
    (void)userdata2;

    result->ok = status == WGPUMapAsyncStatus_Success;
}

/* the item at the queue's end, the device's thread woken */
/* another binary's device: the item on its thread */
static void HDRG_PLUGIN_CALL hdrg_gpu_on_remote(void *arg)
{
    hdrg_gpu_item_t *item = (hdrg_gpu_item_t *)arg;
    hdrg_gpu_t *gpu = item->gpu;
    bool owned;

    /* a call's item ends with its caller once it runs */
    owned = item->owned;
    item->fn(gpu, item->arg);

    if (owned) {
        gpu->api->mem_free(gpu->api, item);
    }
}

/* false: another binary's device could not take it */
static bool hdrg_gpu_queue_item(hdrg_gpu_t *gpu, hdrg_gpu_item_t *item)
{
    item->gpu = gpu;
    item->next = NULL;

    if (gpu->run != NULL) {
        return gpu->run(gpu->user, hdrg_gpu_on_remote, item) == HDRG_OK;
    }

    hdrg_mutex_lock(&gpu->mutex);

    if (gpu->tail != NULL) {
        gpu->tail->next = item;
    } else {
        gpu->head = item;
    }

    gpu->tail = item;
    hdrg_cond_signal(&gpu->wake);
    hdrg_mutex_unlock(&gpu->mutex);

    return true;
}

static void hdrg_gpu_on_shared(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_gpu_shared_t *shared = (hdrg_gpu_shared_t *)arg;

    shared->fn(shared->arg);
    gpu->api->mem_free(gpu->api, shared);
}

/* the share's run: another binary's work on the device's thread */
static hdrg_status_t HDRG_PLUGIN_CALL hdrg_gpu_share_run(void *user, hdrg_gpu_share_fn_t fn,
                                                         void *arg)
{
    hdrg_gpu_t *gpu = (hdrg_gpu_t *)user;
    hdrg_gpu_shared_t *shared;

    if (HDRG_GPU_INLINE) {
        fn(arg);

        return HDRG_OK;
    }

    shared = (hdrg_gpu_shared_t *)gpu->api->mem_alloc(gpu->api, sizeof(*shared));
    if (shared == NULL) {
        return HDRG_ERR_NOMEM;
    }

    shared->fn = fn;
    shared->arg = arg;

    if (!hdrg_gpu_run(gpu, hdrg_gpu_on_shared, shared)) {
        gpu->api->mem_free(gpu->api, shared);

        return HDRG_ERR_NOMEM;
    }

    return HDRG_OK;
}

static void hdrg_gpu_on_drain(hdrg_gpu_t *gpu, void *arg)
{
    (void)gpu;
    (void)arg;
}

static void hdrg_gpu_on_call(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_gpu_call_t *call = (hdrg_gpu_call_t *)arg;

    call->fn(gpu, call->arg);

    hdrg_mutex_lock(&call->mutex);
    call->finished = true;
    hdrg_cond_signal(&call->done);
    hdrg_mutex_unlock(&call->mutex);
}

/* the future done (its callback run on this thread) */
static bool hdrg_gpu_wait(const hdrg_gpu_t *gpu, WGPUFuture future)
{
    WGPUFutureWaitInfo wait;

    memset(&wait, 0, sizeof(wait));
    wait.future = future;

    return wgpuInstanceWaitAny(gpu->instance, 1, &wait, UINT64_MAX) == WGPUWaitStatus_Success &&
           wait.completed != 0u;
}

/*
 * an adapter, then its device with the adapter's limits and those of the features the library's
 * work may use that it has: the shaders' strict math (Dawn's: its Metal backend's math is relaxed
 * otherwise), an app's pixel buffers' IOSurfaces (HDRG_FRAME_CVPIXELBUFFER) and their
 * multi-planar formats
 */
static bool hdrg_gpu_open(hdrg_gpu_t *gpu)
{
    const WGPUInstanceFeatureName feature = WGPUInstanceFeatureName_TimedWaitAny;
#if !HDRG_GPU_INLINE
    const WGPUFeatureName wanted[] = {
        WGPUFeatureName_ShaderModuleCompilationOptions,
#if defined(HDRG_WITH_CVPIXELBUFFER)
        WGPUFeatureName_SharedTextureMemoryIOSurface,
        WGPUFeatureName_SharedFenceMTLSharedEvent,
        WGPUFeatureName_DawnMultiPlanarFormats,
        WGPUFeatureName_MultiPlanarFormatExtendedUsages,
        WGPUFeatureName_MultiPlanarFormatP010,
        WGPUFeatureName_MultiPlanarFormatNv16,
        WGPUFeatureName_MultiPlanarFormatNv24,
        WGPUFeatureName_MultiPlanarFormatP210,
        WGPUFeatureName_MultiPlanarFormatP410,
        WGPUFeatureName_Unorm16TextureFormats,
#endif
    };
#endif
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo adapter_cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPURequestDeviceCallbackInfo device_cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WGPULimits limits = WGPU_LIMITS_INIT;
    hdrg_gpu_result_t result;
#if !HDRG_GPU_INLINE
    WGPUFeatureName features[sizeof(wanted) / sizeof(wanted[0])];
    size_t i, count;
#endif

    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = &feature;
    gpu->instance = wgpuCreateInstance(&instance_desc);
    if (gpu->instance == NULL) {
        return false;
    }

    memset(&result, 0, sizeof(result));
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    adapter_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    adapter_cb.callback = hdrg_gpu_on_adapter;
    adapter_cb.userdata1 = &result;

    if (!hdrg_gpu_wait(gpu, wgpuInstanceRequestAdapter(gpu->instance, &options, adapter_cb)) ||
        !result.ok) {
        return false;
    }

    gpu->adapter = (WGPUAdapter)result.object;

    if (wgpuAdapterGetLimits(gpu->adapter, &limits) != WGPUStatus_Success) {
        return false;
    }

    memset(&result, 0, sizeof(result));
    device_desc.requiredLimits = &limits;

#if !HDRG_GPU_INLINE
    count = 0;

    for (i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        if (wgpuAdapterHasFeature(gpu->adapter, wanted[i])) {
            features[count++] = wanted[i];
        }
    }

    device_desc.requiredFeatureCount = count;
    device_desc.requiredFeatures = features;
#endif

    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_desc.deviceLostCallbackInfo.callback = hdrg_gpu_on_lost;
    device_desc.deviceLostCallbackInfo.userdata1 = gpu;
    device_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    device_cb.callback = hdrg_gpu_on_device;
    device_cb.userdata1 = &result;

    if (!hdrg_gpu_wait(gpu, wgpuAdapterRequestDevice(gpu->adapter, &device_desc, device_cb)) ||
        !result.ok) {
        return false;
    }

    gpu->device = (WGPUDevice)result.object;

    return true;
}

static void hdrg_gpu_main(void *arg)
{
    hdrg_gpu_t *gpu = (hdrg_gpu_t *)arg;
    hdrg_gpu_item_t *item;
    bool owned;

    hdrg_thread_set_worker_qos();

    for (;;) {
        hdrg_mutex_lock(&gpu->mutex);

        while (gpu->head == NULL && !gpu->stopping) {
            hdrg_cond_wait(&gpu->wake, &gpu->mutex);
        }

        item = gpu->head;

        if (item != NULL) {
            gpu->head = item->next;
            gpu->tail = (gpu->head != NULL) ? gpu->tail : NULL;
        }

        hdrg_mutex_unlock(&gpu->mutex);

        if (item == NULL) {
            break;
        }

        /* a call's item ends with its caller once it runs */
        owned = item->owned;
        item->fn(gpu, item->arg);

        if (owned) {
            gpu->api->mem_free(gpu->api, item);
        }
    }
}

static void hdrg_gpu_release(hdrg_gpu_t *gpu)
{
    if (gpu->queue != NULL) {
        wgpuQueueRelease(gpu->queue);
    }

    if (gpu->device != NULL) {
        wgpuDeviceRelease(gpu->device);
    }

    if (gpu->adapter != NULL) {
        wgpuAdapterRelease(gpu->adapter);
    }

    if (gpu->instance != NULL) {
        wgpuInstanceRelease(gpu->instance);
    }
}

extern bool hdrg_gpu_create(const hdrg_plugin_api_t *api, WGPUDevice device, hdrg_gpu_t **out)
{
    hdrg_gpu_t *gpu;
    WGPUAdapter adapter;

    *out = NULL;

    /* one already on the device for the table: shared */
    gpu = (device != NULL) ? hdrg_gpu_find(api, device) : NULL;
    if (gpu != NULL) {
        *out = gpu;

        return true;
    }

    gpu = (hdrg_gpu_t *)api->mem_calloc(api, 1, sizeof(*gpu));
    if (gpu == NULL) {
        return false;
    }

    gpu->api = api;

    hdrg_atomic_u32_init(&gpu->lost, 0u);

    /* the app's device: its instance waits (a browser's: none to be had from the device) */
#if HDRG_GPU_INLINE
    adapter = NULL;
    (void)adapter;
#else
    if (device != NULL) {
        adapter = wgpuDeviceGetAdapter(device);
        gpu->instance = (adapter != NULL) ? wgpuAdapterGetInstance(adapter) : NULL;
        gpu->device = device;
        wgpuDeviceAddRef(device);

        if (adapter != NULL) {
            wgpuAdapterRelease(adapter);
        }
    }
#endif

    if ((device != NULL) ? gpu->instance == NULL : !hdrg_gpu_open(gpu)) {
        hdrg_gpu_release(gpu);
        api->mem_free(api, gpu);

        return false;
    }

    gpu->queue = wgpuDeviceGetQueue(gpu->device);
#if !HDRG_GPU_INLINE
    gpu->strict =
        wgpuDeviceHasFeature(gpu->device, WGPUFeatureName_ShaderModuleCompilationOptions) != 0;
#endif
    gpu->refs = 1;

    /* a browser's: no thread, the work runs inline */
    if (HDRG_GPU_INLINE) {
        hdrg_gpu_register(gpu);
        *out = gpu;

        return true;
    }

    if (!hdrg_mutex_init(&gpu->mutex)) {
        hdrg_gpu_release(gpu);
        api->mem_free(api, gpu);

        return false;
    }

    if (!hdrg_cond_init(&gpu->wake)) {
        hdrg_mutex_destroy(&gpu->mutex);
        hdrg_gpu_release(gpu);
        api->mem_free(api, gpu);

        return false;
    }

    if (!hdrg_thread_create(&gpu->thread, hdrg_gpu_main, gpu)) {
        hdrg_cond_destroy(&gpu->wake);
        hdrg_mutex_destroy(&gpu->mutex);
        hdrg_gpu_release(gpu);
        api->mem_free(api, gpu);

        return false;
    }

    hdrg_gpu_register(gpu);
    *out = gpu;

    return true;
}

extern bool hdrg_gpu_adopt(const hdrg_plugin_api_t *api, const hdrg_gpu_share_t *share,
                           hdrg_gpu_t **out)
{
    hdrg_gpu_t *gpu;

    *out = NULL;

    if (share->kind != HDRG_DEVICE_WEBGPU || share->device == NULL || share->queue == NULL ||
        share->instance == NULL || share->run == NULL) {
        return false;
    }

    /* the binary's own on the device for the table: the library's, shared */
    gpu = hdrg_gpu_find(api, (WGPUDevice)share->device);
    if (gpu != NULL) {
        *out = gpu;

        return true;
    }

    gpu = (hdrg_gpu_t *)api->mem_calloc(api, 1, sizeof(*gpu));
    if (gpu == NULL) {
        return false;
    }

    gpu->api = api;
    gpu->instance = (WGPUInstance)share->instance;
    gpu->device = (WGPUDevice)share->device;
    gpu->queue = (WGPUQueue)share->queue;
    gpu->run = share->run;
    gpu->user = share->user;
    gpu->refs = 1;
    hdrg_atomic_u32_init(&gpu->lost, 0u);
    wgpuInstanceAddRef(gpu->instance);
    wgpuDeviceAddRef(gpu->device);
    wgpuQueueAddRef(gpu->queue);
#if !HDRG_GPU_INLINE
    gpu->strict =
        wgpuDeviceHasFeature(gpu->device, WGPUFeatureName_ShaderModuleCompilationOptions) != 0;
#endif
    hdrg_gpu_register(gpu);
    *out = gpu;

    return true;
}

extern void hdrg_gpu_share(hdrg_gpu_t *gpu, hdrg_gpu_share_t *share)
{
    share->kind = HDRG_DEVICE_WEBGPU;
    share->device = (void *)gpu->device;
    share->queue = (void *)gpu->queue;
    share->instance = (void *)gpu->instance;
    share->run = hdrg_gpu_share_run;
    share->user = gpu;
}

extern void hdrg_gpu_destroy(hdrg_gpu_t *gpu)
{
    hdrg_gpu_t **at;
    bool last;

    if (gpu == NULL) {
        return;
    }

    /* the last holder's: out of the registry */
    hdrg_gpu_lock();
    gpu->refs--;
    last = gpu->refs == 0;

    for (at = &hdrg_gpu_registry; last && *at != NULL; at = &(*at)->next) {
        if (*at == gpu) {
            *at = gpu->next;

            break;
        }
    }

    hdrg_gpu_unlock();

    if (!last) {
        return;
    }

    /* another binary's device: the work queued on its thread run first */
    if (gpu->run != NULL) {
        (void)hdrg_gpu_call(gpu, hdrg_gpu_on_drain, NULL);
        hdrg_gpu_release(gpu);
        gpu->api->mem_free(gpu->api, gpu);

        return;
    }

    if (HDRG_GPU_INLINE) {
        hdrg_gpu_release(gpu);
        gpu->api->mem_free(gpu->api, gpu);

        return;
    }

    hdrg_mutex_lock(&gpu->mutex);
    gpu->stopping = true;
    hdrg_cond_signal(&gpu->wake);
    hdrg_mutex_unlock(&gpu->mutex);

    hdrg_thread_join(&gpu->thread);
    hdrg_cond_destroy(&gpu->wake);
    hdrg_mutex_destroy(&gpu->mutex);
    hdrg_gpu_release(gpu);
    gpu->api->mem_free(gpu->api, gpu);
}

extern WGPUDevice hdrg_gpu_device(const hdrg_gpu_t *gpu)
{
    return gpu->device;
}

extern WGPUQueue hdrg_gpu_queue(const hdrg_gpu_t *gpu)
{
    return gpu->queue;
}

extern bool hdrg_gpu_run(hdrg_gpu_t *gpu, hdrg_gpu_work_fn_t fn, void *arg)
{
    hdrg_gpu_item_t *item;

    if (HDRG_GPU_INLINE) {
        fn(gpu, arg);

        return true;
    }

    item = (hdrg_gpu_item_t *)gpu->api->mem_alloc(gpu->api, sizeof(*item));
    if (item == NULL) {
        return false;
    }

    item->fn = fn;
    item->arg = arg;
    item->owned = true;

    if (!hdrg_gpu_queue_item(gpu, item)) {
        gpu->api->mem_free(gpu->api, item);

        return false;
    }

    return true;
}

extern bool hdrg_gpu_call(hdrg_gpu_t *gpu, hdrg_gpu_work_fn_t fn, void *arg)
{
    hdrg_gpu_call_t call;

    if (HDRG_GPU_INLINE) {
        fn(gpu, arg);

        return true;
    }

    call.fn = fn;
    call.arg = arg;
    call.finished = false;

    if (!hdrg_mutex_init(&call.mutex)) {
        return false;
    }

    if (!hdrg_cond_init(&call.done)) {
        hdrg_mutex_destroy(&call.mutex);

        return false;
    }

    call.item.fn = hdrg_gpu_on_call;
    call.item.arg = &call;
    call.item.owned = false;

    if (!hdrg_gpu_queue_item(gpu, &call.item)) {
        hdrg_cond_destroy(&call.done);
        hdrg_mutex_destroy(&call.mutex);

        return false;
    }

    hdrg_mutex_lock(&call.mutex);

    while (!call.finished) {
        hdrg_cond_wait(&call.done, &call.mutex);
    }

    hdrg_mutex_unlock(&call.mutex);
    hdrg_cond_destroy(&call.done);
    hdrg_mutex_destroy(&call.mutex);

    return true;
}

extern void hdrg_gpu_scope_begin(hdrg_gpu_t *gpu)
{
    wgpuDevicePushErrorScope(gpu->device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(gpu->device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(gpu->device, WGPUErrorFilter_Validation);
}

extern bool hdrg_gpu_scope_end(hdrg_gpu_t *gpu)
{
    WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    hdrg_gpu_result_t result;
    uint8_t i;
    bool ok;

    ok = true;
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = hdrg_gpu_on_scope;
    cb.userdata1 = &result;

    for (i = 0; i < 3u; i++) {
        memset(&result, 0, sizeof(result));
        ok = hdrg_gpu_wait(gpu, wgpuDevicePopErrorScope(gpu->device, cb)) && result.ok && ok;
    }

    return ok && hdrg_atomic_u32_load_acquire(&gpu->lost) == 0u;
}

extern bool hdrg_gpu_finish(hdrg_gpu_t *gpu)
{
    WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    hdrg_gpu_result_t result;

    memset(&result, 0, sizeof(result));
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = hdrg_gpu_on_done;
    cb.userdata1 = &result;

    return hdrg_gpu_wait(gpu, wgpuQueueOnSubmittedWorkDone(gpu->queue, cb)) && result.ok;
}

extern const void *hdrg_gpu_map_read(hdrg_gpu_t *gpu, WGPUBuffer buffer, size_t size)
{
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    hdrg_gpu_result_t result;

    memset(&result, 0, sizeof(result));
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = hdrg_gpu_on_map;
    cb.userdata1 = &result;

    if (!hdrg_gpu_wait(gpu, wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, cb)) ||
        !result.ok) {
        return NULL;
    }

    return wgpuBufferGetConstMappedRange(buffer, 0, size);
}

extern WGPUShaderModule hdrg_gpu_module(hdrg_gpu_t *gpu, const char *wgsl)
{
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
#if !HDRG_GPU_INLINE
    WGPUShaderModuleCompilationOptions options = WGPU_SHADER_MODULE_COMPILATION_OPTIONS_INIT;
#endif
    WGPUShaderModule module;

    source.code.data = wgsl;
    source.code.length = strlen(wgsl);
    desc.nextInChain = &source.chain;

#if !HDRG_GPU_INLINE
    /* strict math where Dawn has it (Metal's relaxed math is not the CPU's) */
    if (gpu->strict) {
        options.strictMath = 1;
        source.chain.next = &options.chain;
    }
#endif

    hdrg_gpu_scope_begin(gpu);

    module = wgpuDeviceCreateShaderModule(gpu->device, &desc);

    if (!hdrg_gpu_scope_end(gpu) && module != NULL) {
        wgpuShaderModuleRelease(module);
        module = NULL;
    }

    return module;
}

extern WGPUComputePipeline hdrg_gpu_pipeline(hdrg_gpu_t *gpu, WGPUShaderModule module,
                                             const char *entry, WGPUPipelineLayout layout)
{
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    WGPUComputePipeline pipeline;

    desc.layout = layout;
    desc.compute.module = module;
    desc.compute.entryPoint.data = entry;
    desc.compute.entryPoint.length = strlen(entry);

    hdrg_gpu_scope_begin(gpu);

    pipeline = wgpuDeviceCreateComputePipeline(gpu->device, &desc);

    if (!hdrg_gpu_scope_end(gpu) && pipeline != NULL) {
        wgpuComputePipelineRelease(pipeline);
        pipeline = NULL;
    }

    return pipeline;
}

extern WGPUBuffer hdrg_gpu_buffer(hdrg_gpu_t *gpu, size_t size, WGPUBufferUsage usage)
{
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBuffer buffer;

    desc.size = (uint64_t)size;
    desc.usage = usage;

    hdrg_gpu_scope_begin(gpu);

    buffer = wgpuDeviceCreateBuffer(gpu->device, &desc);

    if (!hdrg_gpu_scope_end(gpu) && buffer != NULL) {
        wgpuBufferRelease(buffer);
        buffer = NULL;
    }

    return buffer;
}
