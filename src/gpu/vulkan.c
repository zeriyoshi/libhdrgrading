/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* dup, close, lseek */
#endif

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "hdrgrading/foundation/thread.h"
#include "hdrgrading/gpu/vulkan.h"

#define HDRG_VK_ARENA     65536u    /* a pass's uniforms' buffer at first ... */
#define HDRG_VK_ARENA_MAX 16777216u /* ... and at most */
#define HDRG_VK_HELD      16u       /* a pass's references at first */
#define HDRG_VK_DEVICES   16u       /* the system's devices looked at */
#define HDRG_VK_FAMILIES  32u       /* a device's queue families looked at */
#define HDRG_VK_EXTENSION 5u        /* the extensions of a device of its own */
#define HDRG_VK_BLOCK     "_block_tint_explicit_layout"
#define HDRG_VK_PUSH      "VK_KHR_push_descriptor"
#define HDRG_VK_FD        "VK_KHR_external_memory_fd"
#define HDRG_VK_DMABUF    "VK_EXT_external_memory_dma_buf"
#define HDRG_VK_AHB       "VK_ANDROID_external_memory_android_hardware_buffer"
#define HDRG_VK_FOREIGN   "VK_EXT_queue_family_foreign"
#define HDRG_VK_ACCESS    (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
#define HDRG_VK_LOAD(table, get, handle, name)                                                     \
    ((table)->name = (PFN_vk##name)(get)((handle), "vk" #name), (table)->name != NULL)

/* Android's AHardwareBuffer_describe: libnativewindow (API 26), else libandroid */
#define HDRG_VK_NATIVE_WINDOW "libnativewindow.so"
#define HDRG_VK_ANDROID       "libandroid.so"

#if defined(_WIN32)
#define HDRG_VK_LIBRARY "vulkan-1.dll"
#elif defined(__APPLE__)
#define HDRG_VK_LIBRARY "libvulkan.1.dylib"
#else
#define HDRG_VK_LIBRARY "libvulkan.so.1"
#endif

/* the NDK's AHardwareBuffer_Desc (android/hardware_buffer.h, API 26) and AHardwareBuffer_describe
 */
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} hdrg_vk_ahb_desc_t;

typedef void (*hdrg_vk_ahb_describe_fn_t)(const void *buffer, hdrg_vk_ahb_desc_t *desc);

/* the functions: the instance's, then the device's */
typedef struct {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2; /* NULL: none (1.0) */
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkResetCommandPool ResetCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkResetFences ResetFences;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR; /* NULL: no dma-buf */
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID
        GetAndroidHardwareBufferPropertiesANDROID; /* NULL: no AHardwareBuffer */
} hdrg_vk_fns_t;

struct hdrg_vulkan {
    const hdrg_plugin_api_t *api; /* its allocator */
    hdrg_vulkan_t *next;          /* the shared ones (hdrg_vk_lock) ... */
    uint32_t refs;                /* ... and their holders */
    hdrg_vk_fns_t fn;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkDeviceSize uniform_align;
    hdrg_vulkan_device_t self;  /* its own: what describes it; the app's: a copy */
    hdrg_vulkan_device_t share; /* self, its lock_queue the queue's lock (hdrg_vulkan_share) */
    const char *extensions[HDRG_VK_EXTENSION];
    hdrg_mutex_t lock;  /* the queue's submissions and the passes to reuse */
    hdrg_mutex_t mutex; /* the passes waited for */
    hdrg_cond_t wake;
    hdrg_thread_t thread;
    hdrg_vulkan_pass_t *head, *tail;        /* mutex: submitted, in order */
    hdrg_vulkan_pass_t *spare;              /* lock: to reuse */
    hdrg_atomic_u32_t lost;                 /* a fence failed: the device is gone */
    bool own;                               /* the device and instance its own */
    bool stopping;                          /* mutex */
    void *library;                          /* the loader it opened (NULL: none) */
    void *android;                          /* libnativewindow (NULL: none) ... */
    hdrg_vk_ahb_describe_fn_t ahb_describe; /* ... its AHardwareBuffer_describe (NULL: none) */
};

struct hdrg_vulkan_buffer {
    hdrg_vulkan_t *vulkan;
    VkBuffer buffer;
    VkDeviceMemory memory; /* VK_NULL_HANDLE: the app's buffer */
    hdrg_atomic_u32_t refs;
    size_t size;
};

struct hdrg_vulkan_image {
    hdrg_vulkan_t *vulkan;
    VkImage image;
    VkDeviceMemory memory;
};

struct hdrg_vulkan_kernel {
    hdrg_vulkan_t *vulkan;
    VkShaderModule module;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    hdrg_atomic_u32_t refs;
    uint32_t uniform; /* its binding */
    uint32_t bindings[HDRG_VULKAN_BUFFERS];
    uint8_t count;
    bool has_uniform;
};

struct hdrg_vulkan_pass {
    hdrg_vulkan_t *vulkan;
    hdrg_vulkan_pass_t *next;
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkFence fence;
    hdrg_vulkan_buffer_t *arena; /* the uniforms */
    hdrg_vulkan_buffer_t **buffers;
    hdrg_vulkan_kernel_t **kernels;
    hdrg_vulkan_done_fn_t done;
    uint8_t *arena_view;
    size_t arena_size; /* the uniforms' buffer's: grown when a pass fills it, kept for its reuse */
    size_t arena_used, buffer_count, buffer_cap, kernel_count, kernel_cap;
    bool failed; /* recording ran out of memory: the commit fails */
    void *arg;
};

/*
 * every hdrg_vulkan_t, by its device, queue and table: one lock for a
 * queue, whoever asks for it with the same table (the filters and the core
 * of a context: hdrg_host_device)
 */
static hdrg_atomic_u32_t hdrg_vk_registry_lock;
static hdrg_vulkan_t *hdrg_vk_registry;

static void hdrg_vk_lock(void)
{
    uint32_t expected;

    do {
        expected = 0;
    } while (!hdrg_atomic_u32_cas_acq_rel(&hdrg_vk_registry_lock, &expected, 1u));
}

static void hdrg_vk_unlock(void)
{
    hdrg_atomic_u32_store_release(&hdrg_vk_registry_lock, 0u);
}

/* the share's lock_queue: the object's queue lock, the app's after it when the device is the app's
 */
static void hdrg_vk_share_lock(void *user, bool lock)
{
    hdrg_vulkan_t *vulkan = (hdrg_vulkan_t *)user;

    if (lock) {
        hdrg_mutex_lock(&vulkan->lock);

        if (vulkan->self.lock_queue != NULL) {
            vulkan->self.lock_queue(vulkan->self.user, true);
        }

        return;
    }

    if (vulkan->self.lock_queue != NULL) {
        vulkan->self.lock_queue(vulkan->self.user, false);
    }

    hdrg_mutex_unlock(&vulkan->lock);
}

static void *hdrg_vk_library_open(void)
{
#if defined(_WIN32)
    return (void *)LoadLibraryA(HDRG_VK_LIBRARY);
#else
    return dlopen(HDRG_VK_LIBRARY, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void hdrg_vk_library_close(void *library)
{
    if (library == NULL) {
        return;
    }

#if defined(_WIN32)
    (void)FreeLibrary((HMODULE)library);
#else
    (void)dlclose(library);
#endif
}

static PFN_vkGetInstanceProcAddr hdrg_vk_library_getter(void *library)
{
    PFN_vkGetInstanceProcAddr getter;
#if !defined(_WIN32)
    void *symbol;
#endif

#if defined(_WIN32)
    getter = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress((HMODULE)library,
                                                                       "vkGetInstanceProcAddr");
#else
    symbol = dlsym(library, "vkGetInstanceProcAddr");
    memcpy(&getter, &symbol, sizeof(getter));
#endif

    return getter;
}

/* Android's AHardwareBuffer_describe, for a device that takes AHardwareBuffers */
static void hdrg_vk_android_open(hdrg_vulkan_t *vulkan)
{
#if !defined(_WIN32)
    void *symbol;

    if (vulkan->fn.GetAndroidHardwareBufferPropertiesANDROID == NULL) {
        return;
    }

    vulkan->android = dlopen(HDRG_VK_NATIVE_WINDOW, RTLD_NOW | RTLD_LOCAL);
    if (vulkan->android == NULL) {
        vulkan->android = dlopen(HDRG_VK_ANDROID, RTLD_NOW | RTLD_LOCAL);
    }

    if (vulkan->android == NULL) {
        return;
    }

    symbol = dlsym(vulkan->android, "AHardwareBuffer_describe");
    memcpy(&vulkan->ahb_describe, &symbol, sizeof(vulkan->ahb_describe));
#else
    (void)vulkan;
#endif
}

/* the bytes of s before its NUL, at most max */
static size_t hdrg_vk_length(const char *s, size_t max)
{
    const char *end = (const char *)memchr(s, '\0', max);

    return (end != NULL) ? (size_t)(end - s) : max;
}

static bool hdrg_vk_has(const char *const *names, uint32_t count, const char *name)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (names[i] != NULL && strcmp(names[i], name) == 0) {
            return true;
        }
    }

    return false;
}

static bool hdrg_vk_instance_fns(hdrg_vk_fns_t *fn, VkInstance instance)
{
    const PFN_vkGetInstanceProcAddr get = fn->GetInstanceProcAddr;

    (void)HDRG_VK_LOAD(fn, get, instance, GetPhysicalDeviceFeatures2);

    return HDRG_VK_LOAD(fn, get, instance, GetDeviceProcAddr) &&
           HDRG_VK_LOAD(fn, get, instance, DestroyInstance) &&
           HDRG_VK_LOAD(fn, get, instance, EnumeratePhysicalDevices) &&
           HDRG_VK_LOAD(fn, get, instance, GetPhysicalDeviceProperties) &&
           HDRG_VK_LOAD(fn, get, instance, GetPhysicalDeviceMemoryProperties) &&
           HDRG_VK_LOAD(fn, get, instance, GetPhysicalDeviceQueueFamilyProperties) &&
           HDRG_VK_LOAD(fn, get, instance, EnumerateDeviceExtensionProperties) &&
           HDRG_VK_LOAD(fn, get, instance, CreateDevice);
}

static bool hdrg_vk_device_fns(hdrg_vk_fns_t *fn, VkDevice device, bool dmabuf, bool ahb)
{
    const PFN_vkGetDeviceProcAddr get = fn->GetDeviceProcAddr;

    fn->GetMemoryFdPropertiesKHR = NULL;
    fn->GetAndroidHardwareBufferPropertiesANDROID = NULL;

    if (dmabuf) {
        (void)HDRG_VK_LOAD(fn, get, device, GetMemoryFdPropertiesKHR);
    }

    if (ahb) {
        (void)HDRG_VK_LOAD(fn, get, device, GetAndroidHardwareBufferPropertiesANDROID);
    }

    return HDRG_VK_LOAD(fn, get, device, DestroyDevice) &&
           HDRG_VK_LOAD(fn, get, device, GetDeviceQueue) &&
           HDRG_VK_LOAD(fn, get, device, CreateBuffer) &&
           HDRG_VK_LOAD(fn, get, device, DestroyBuffer) &&
           HDRG_VK_LOAD(fn, get, device, GetBufferMemoryRequirements) &&
           HDRG_VK_LOAD(fn, get, device, AllocateMemory) &&
           HDRG_VK_LOAD(fn, get, device, FreeMemory) &&
           HDRG_VK_LOAD(fn, get, device, BindBufferMemory) &&
           HDRG_VK_LOAD(fn, get, device, CreateImage) &&
           HDRG_VK_LOAD(fn, get, device, DestroyImage) &&
           HDRG_VK_LOAD(fn, get, device, BindImageMemory) &&
           HDRG_VK_LOAD(fn, get, device, MapMemory) &&
           HDRG_VK_LOAD(fn, get, device, CreateShaderModule) &&
           HDRG_VK_LOAD(fn, get, device, DestroyShaderModule) &&
           HDRG_VK_LOAD(fn, get, device, CreateDescriptorSetLayout) &&
           HDRG_VK_LOAD(fn, get, device, DestroyDescriptorSetLayout) &&
           HDRG_VK_LOAD(fn, get, device, CreatePipelineLayout) &&
           HDRG_VK_LOAD(fn, get, device, DestroyPipelineLayout) &&
           HDRG_VK_LOAD(fn, get, device, CreateComputePipelines) &&
           HDRG_VK_LOAD(fn, get, device, DestroyPipeline) &&
           HDRG_VK_LOAD(fn, get, device, CreateCommandPool) &&
           HDRG_VK_LOAD(fn, get, device, DestroyCommandPool) &&
           HDRG_VK_LOAD(fn, get, device, ResetCommandPool) &&
           HDRG_VK_LOAD(fn, get, device, AllocateCommandBuffers) &&
           HDRG_VK_LOAD(fn, get, device, BeginCommandBuffer) &&
           HDRG_VK_LOAD(fn, get, device, EndCommandBuffer) &&
           HDRG_VK_LOAD(fn, get, device, CmdBindPipeline) &&
           HDRG_VK_LOAD(fn, get, device, CmdPushDescriptorSetKHR) &&
           HDRG_VK_LOAD(fn, get, device, CmdDispatch) &&
           HDRG_VK_LOAD(fn, get, device, CmdPipelineBarrier) &&
           HDRG_VK_LOAD(fn, get, device, CmdCopyBuffer) &&
           HDRG_VK_LOAD(fn, get, device, CmdCopyImageToBuffer) &&
           HDRG_VK_LOAD(fn, get, device, CmdCopyBufferToImage) &&
           HDRG_VK_LOAD(fn, get, device, CreateFence) &&
           HDRG_VK_LOAD(fn, get, device, DestroyFence) &&
           HDRG_VK_LOAD(fn, get, device, ResetFences) &&
           HDRG_VK_LOAD(fn, get, device, WaitForFences) &&
           HDRG_VK_LOAD(fn, get, device, QueueSubmit);
}

/* the device's extension named, from its list */
static bool hdrg_vk_extension(const hdrg_vulkan_t *vulkan, VkPhysicalDevice physical,
                              const char *name)
{
    VkExtensionProperties properties[256];
    uint32_t count, i;

    count = 256u;

    if (vulkan->fn.EnumerateDeviceExtensionProperties(physical, NULL, &count, properties) < 0) {
        return false;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(properties[i].extensionName, name) == 0) {
            return true;
        }
    }

    return false;
}

/* the first compute queue family of physical (UINT32_MAX: none) */
static uint32_t hdrg_vk_family(const hdrg_vulkan_t *vulkan, VkPhysicalDevice physical)
{
    VkQueueFamilyProperties families[HDRG_VK_FAMILIES];
    uint32_t count, i;

    count = HDRG_VK_FAMILIES;
    vulkan->fn.GetPhysicalDeviceQueueFamilyProperties(physical, &count, families);

    for (i = 0; i < count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && families[i].queueCount > 0) {
            return i;
        }
    }

    return UINT32_MAX;
}

/* a device of its own: the instance, the system's best device, its queue */
static bool hdrg_vk_open_own(hdrg_vulkan_t *vulkan)
{
    static const VkPhysicalDeviceType ranks[4] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU, VK_PHYSICAL_DEVICE_TYPE_CPU};
    const float priority = 1.0f;
    PFN_vkEnumerateInstanceVersion version_of;
    PFN_vkCreateInstance create_instance;
    VkPhysicalDevice physicals[HDRG_VK_DEVICES];
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr;
    VkPhysicalDeviceFeatures2 features;
    VkPhysicalDeviceProperties properties;
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    uint32_t version, count, family, rank, best_rank, best_family, i, enabled;
    bool dmabuf, ahb;

    vulkan->library = hdrg_vk_library_open();
    if (vulkan->library == NULL) {
        return false;
    }

    vulkan->fn.GetInstanceProcAddr = hdrg_vk_library_getter(vulkan->library);
    if (vulkan->fn.GetInstanceProcAddr == NULL) {
        return false;
    }

    version_of = (PFN_vkEnumerateInstanceVersion)vulkan->fn.GetInstanceProcAddr(
        NULL, "vkEnumerateInstanceVersion");
    create_instance =
        (PFN_vkCreateInstance)vulkan->fn.GetInstanceProcAddr(NULL, "vkCreateInstance");
    version = VK_API_VERSION_1_0;

    if (version_of == NULL || create_instance == NULL || version_of(&version) != VK_SUCCESS ||
        version < VK_API_VERSION_1_1) {
        return false;
    }

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "libhdrgrading";
    app.pEngineName = "libhdrgrading";
    app.apiVersion = (version < VK_API_VERSION_1_3) ? version : VK_API_VERSION_1_3;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;

    if (create_instance(&instance_info, NULL, &vulkan->instance) != VK_SUCCESS) {
        vulkan->instance = VK_NULL_HANDLE;

        return false;
    }

    if (!hdrg_vk_instance_fns(&vulkan->fn, vulkan->instance)) {
        return false;
    }

    count = HDRG_VK_DEVICES;

    if (vulkan->fn.EnumeratePhysicalDevices(vulkan->instance, &count, physicals) < 0) {
        return false;
    }

    best_rank = 4u;
    best_family = UINT32_MAX;

    for (i = 0; i < count; i++) {
        vulkan->fn.GetPhysicalDeviceProperties(physicals[i], &properties);
        family = hdrg_vk_family(vulkan, physicals[i]);

        for (rank = 0; rank < 4u && ranks[rank] != properties.deviceType; rank++) {
        }

        if (rank >= best_rank || properties.apiVersion < VK_API_VERSION_1_1 ||
            family == UINT32_MAX || !hdrg_vk_extension(vulkan, physicals[i], HDRG_VK_PUSH)) {
            continue;
        }

        best_rank = rank;
        best_family = family;
        vulkan->physical = physicals[i];
        vulkan->self.api_version =
            (properties.apiVersion < app.apiVersion) ? properties.apiVersion : app.apiVersion;
    }

    if (best_family == UINT32_MAX) {
        return false;
    }

    dmabuf = hdrg_vk_extension(vulkan, vulkan->physical, HDRG_VK_FD) &&
             hdrg_vk_extension(vulkan, vulkan->physical, HDRG_VK_DMABUF);
    enabled = 0;
    vulkan->extensions[enabled++] = HDRG_VK_PUSH;

    if (dmabuf) {
        vulkan->extensions[enabled++] = HDRG_VK_FD;
        vulkan->extensions[enabled++] = HDRG_VK_DMABUF;
    }

    /* Android's AHardwareBuffers: their multi-planar images need the Y'CbCr conversion's feature */
    memset(&ycbcr, 0, sizeof(ycbcr));
    ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    memset(&features, 0, sizeof(features));
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &ycbcr;
    ahb = vulkan->fn.GetPhysicalDeviceFeatures2 != NULL &&
          hdrg_vk_extension(vulkan, vulkan->physical, HDRG_VK_AHB) &&
          hdrg_vk_extension(vulkan, vulkan->physical, HDRG_VK_FOREIGN);

    if (ahb) {
        vulkan->fn.GetPhysicalDeviceFeatures2(vulkan->physical, &features);
        ahb = ycbcr.samplerYcbcrConversion == VK_TRUE;
    }

    if (ahb) {
        vulkan->extensions[enabled++] = HDRG_VK_AHB;
        vulkan->extensions[enabled++] = HDRG_VK_FOREIGN;
    }

    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = best_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = enabled;
    device_info.ppEnabledExtensionNames = vulkan->extensions;
    device_info.pNext = ahb ? &ycbcr : NULL;

    if (vulkan->fn.CreateDevice(vulkan->physical, &device_info, NULL, &vulkan->device) !=
        VK_SUCCESS) {
        vulkan->device = VK_NULL_HANDLE;

        return false;
    }

    if (!hdrg_vk_device_fns(&vulkan->fn, vulkan->device, dmabuf, ahb)) {
        return false;
    }

    vulkan->fn.GetDeviceQueue(vulkan->device, best_family, 0, &vulkan->queue);
    vulkan->self.struct_size = (uint32_t)sizeof(vulkan->self);
    vulkan->self.queue_family = best_family;
    vulkan->self.extension_count = enabled;
    vulkan->self.extensions = vulkan->extensions;
    memcpy(&vulkan->self.get_instance_proc_addr, &vulkan->fn.GetInstanceProcAddr,
           sizeof(vulkan->self.get_instance_proc_addr));
    vulkan->self.instance = (void *)vulkan->instance;
    vulkan->self.physical_device = (void *)vulkan->physical;
    vulkan->self.device = (void *)vulkan->device;

    return vulkan->queue != VK_NULL_HANDLE;
}

/* the app's device: its functions, and what it takes */
static bool hdrg_vk_open_app(hdrg_vulkan_t *vulkan, const hdrg_vulkan_device_t *device, void *queue)
{
    bool dmabuf, ahb;

    if (device->struct_size < sizeof(*device) || device->api_version < VK_API_VERSION_1_1 ||
        device->instance == NULL || device->physical_device == NULL || device->device == NULL ||
        queue == NULL || !hdrg_vk_has(device->extensions, device->extension_count, HDRG_VK_PUSH)) {
        return false;
    }

    if (device->get_instance_proc_addr != NULL) {
        memcpy(&vulkan->fn.GetInstanceProcAddr, &device->get_instance_proc_addr,
               sizeof(vulkan->fn.GetInstanceProcAddr));
    } else {
        vulkan->library = hdrg_vk_library_open();
        if (vulkan->library == NULL) {
            return false;
        }

        vulkan->fn.GetInstanceProcAddr = hdrg_vk_library_getter(vulkan->library);
    }

    vulkan->instance = (VkInstance)device->instance;
    vulkan->physical = (VkPhysicalDevice)device->physical_device;
    vulkan->device = (VkDevice)device->device;
    vulkan->queue = (VkQueue)queue;
    vulkan->self = *device;
    dmabuf = hdrg_vk_has(device->extensions, device->extension_count, HDRG_VK_FD) &&
             hdrg_vk_has(device->extensions, device->extension_count, HDRG_VK_DMABUF);
    ahb = hdrg_vk_has(device->extensions, device->extension_count, HDRG_VK_AHB) &&
          hdrg_vk_has(device->extensions, device->extension_count, HDRG_VK_FOREIGN);

    return vulkan->fn.GetInstanceProcAddr != NULL &&
           hdrg_vk_instance_fns(&vulkan->fn, vulkan->instance) &&
           hdrg_vk_device_fns(&vulkan->fn, vulkan->device, dmabuf, ahb);
}

/* a memory type of bits with the flags wanted (then the ones needed); UINT32_MAX: none */
static uint32_t hdrg_vk_memory_type(const hdrg_vulkan_t *vulkan, uint32_t bits,
                                    VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags needed)
{
    const VkMemoryPropertyFlags flags[2] = {wanted, needed};
    uint32_t pass, i;

    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < vulkan->memory.memoryTypeCount; i++) {
            if ((bits & (1u << i)) != 0 &&
                (vulkan->memory.memoryTypes[i].propertyFlags & flags[pass]) == flags[pass]) {
                return i;
            }
        }
    }

    return UINT32_MAX;
}

static void hdrg_vk_buffer_release(hdrg_vulkan_buffer_t *buffer)
{
    hdrg_vulkan_t *vulkan;

    if (buffer == NULL || hdrg_atomic_u32_fetch_add_acq_rel(&buffer->refs, UINT32_MAX) != 1u) {
        return;
    }

    vulkan = buffer->vulkan;

    if (buffer->memory != VK_NULL_HANDLE) {
        vulkan->fn.DestroyBuffer(vulkan->device, buffer->buffer, NULL);
        vulkan->fn.FreeMemory(vulkan->device, buffer->memory, NULL);
    }

    vulkan->api->mem_free(vulkan->api, buffer);
}

static void hdrg_vk_kernel_release(hdrg_vulkan_kernel_t *kernel)
{
    hdrg_vulkan_t *vulkan;

    if (kernel == NULL || hdrg_atomic_u32_fetch_add_acq_rel(&kernel->refs, UINT32_MAX) != 1u) {
        return;
    }

    vulkan = kernel->vulkan;

    if (kernel->pipeline != VK_NULL_HANDLE) {
        vulkan->fn.DestroyPipeline(vulkan->device, kernel->pipeline, NULL);
    }

    if (kernel->layout != VK_NULL_HANDLE) {
        vulkan->fn.DestroyPipelineLayout(vulkan->device, kernel->layout, NULL);
    }

    if (kernel->set != VK_NULL_HANDLE) {
        vulkan->fn.DestroyDescriptorSetLayout(vulkan->device, kernel->set, NULL);
    }

    if (kernel->module != VK_NULL_HANDLE) {
        vulkan->fn.DestroyShaderModule(vulkan->device, kernel->module, NULL);
    }

    vulkan->api->mem_free(vulkan->api, kernel);
}

/* a buffer of size bytes in memory of the flags wanted (else needed), mapped when view */
static hdrg_vulkan_buffer_t *hdrg_vk_buffer_make(hdrg_vulkan_t *vulkan, size_t size,
                                                 VkMemoryPropertyFlags wanted,
                                                 VkMemoryPropertyFlags needed, uint8_t **view)
{
    const hdrg_plugin_api_t *api = vulkan->api;
    hdrg_vulkan_buffer_t *buffer;
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    uint32_t type;
    void *mapped;

    if (view != NULL) {
        *view = NULL;
    }

    if (size == 0) {
        return NULL;
    }

    buffer = (hdrg_vulkan_buffer_t *)api->mem_calloc(api, 1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->vulkan = vulkan;
    buffer->size = size;
    hdrg_atomic_u32_init(&buffer->refs, 1u);
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vulkan->fn.CreateBuffer(vulkan->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) {
        api->mem_free(api, buffer);

        return NULL;
    }

    vulkan->fn.GetBufferMemoryRequirements(vulkan->device, buffer->buffer, &requirements);
    type = hdrg_vk_memory_type(vulkan, requirements.memoryTypeBits, wanted, needed);
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    if (type == UINT32_MAX ||
        vulkan->fn.AllocateMemory(vulkan->device, &allocate, NULL, &buffer->memory) != VK_SUCCESS) {
        vulkan->fn.DestroyBuffer(vulkan->device, buffer->buffer, NULL);
        api->mem_free(api, buffer);

        return NULL;
    }

    mapped = NULL;

    if (vulkan->fn.BindBufferMemory(vulkan->device, buffer->buffer, buffer->memory, 0) !=
            VK_SUCCESS ||
        (view != NULL && vulkan->fn.MapMemory(vulkan->device, buffer->memory, 0, VK_WHOLE_SIZE, 0,
                                              &mapped) != VK_SUCCESS)) {
        hdrg_vk_buffer_release(buffer);

        return NULL;
    }

    if (view != NULL) {
        *view = (uint8_t *)mapped;
    }

    return buffer;
}

/* the reference grown into a pass's list (false: no memory, the pass fails) */
static bool hdrg_vk_hold(hdrg_vulkan_pass_t *pass, void ***list, size_t *count, size_t *cap,
                         void *object)
{
    const hdrg_plugin_api_t *api = pass->vulkan->api;
    size_t grown;
    void **items;

    if (*count == *cap) {
        grown = (*cap == 0) ? HDRG_VK_HELD : *cap * 2u;
        items = (void **)api->mem_realloc(api, *list, grown * sizeof(*items));
        if (items == NULL) {
            pass->failed = true;

            return false;
        }

        *list = items;
        *cap = grown;
    }

    (*list)[(*count)++] = object;

    return true;
}

static bool hdrg_vk_hold_buffer(hdrg_vulkan_pass_t *pass, hdrg_vulkan_buffer_t *buffer)
{
    if (!hdrg_vk_hold(pass, (void ***)&pass->buffers, &pass->buffer_count, &pass->buffer_cap,
                      buffer)) {
        return false;
    }

    (void)hdrg_atomic_u32_fetch_add_acq_rel(&buffer->refs, 1u);

    return true;
}

/* everything the commands before see done for the next (the queue's earlier passes too) */
static void hdrg_vk_barrier(hdrg_vulkan_pass_t *pass, VkPipelineStageFlags stage,
                            VkAccessFlags access)
{
    VkMemoryBarrier barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = access;
    pass->vulkan->fn.CmdPipelineBarrier(pass->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, stage,
                                        0, 1, &barrier, 0, NULL, 0, NULL);
}

/* the pass's references dropped and the pass kept for reuse */
static void hdrg_vk_recycle(hdrg_vulkan_pass_t *pass)
{
    hdrg_vulkan_t *const vulkan = pass->vulkan;
    size_t i;

    for (i = 0; i < pass->buffer_count; i++) {
        hdrg_vk_buffer_release(pass->buffers[i]);
    }

    for (i = 0; i < pass->kernel_count; i++) {
        hdrg_vk_kernel_release(pass->kernels[i]);
    }

    pass->buffer_count = 0;
    pass->kernel_count = 0;
    pass->arena_used = 0;
    pass->failed = false;
    pass->done = NULL;
    pass->arg = NULL;
    (void)vulkan->fn.ResetFences(vulkan->device, 1, &pass->fence);
    (void)vulkan->fn.ResetCommandPool(vulkan->device, pass->pool, 0);

    hdrg_mutex_lock(&vulkan->lock);
    pass->next = vulkan->spare;
    vulkan->spare = pass;
    hdrg_mutex_unlock(&vulkan->lock);
}

static void hdrg_vk_pass_free(hdrg_vulkan_pass_t *pass)
{
    hdrg_vulkan_t *const vulkan = pass->vulkan;

    if (pass->fence != VK_NULL_HANDLE) {
        vulkan->fn.DestroyFence(vulkan->device, pass->fence, NULL);
    }

    if (pass->pool != VK_NULL_HANDLE) {
        vulkan->fn.DestroyCommandPool(vulkan->device, pass->pool, NULL);
    }

    hdrg_vk_buffer_release(pass->arena);
    vulkan->api->mem_free(vulkan->api, pass->buffers);
    vulkan->api->mem_free(vulkan->api, pass->kernels);
    vulkan->api->mem_free(vulkan->api, pass);
}

/* the device's thread: the submitted passes' ends, in order */
static void hdrg_vk_main(void *arg)
{
    hdrg_vulkan_t *vulkan = (hdrg_vulkan_t *)arg;
    hdrg_vulkan_pass_t *pass;
    bool ok;

    for (;;) {
        hdrg_mutex_lock(&vulkan->mutex);

        while (vulkan->head == NULL && !vulkan->stopping) {
            hdrg_cond_wait(&vulkan->wake, &vulkan->mutex);
        }

        pass = vulkan->head;

        if (pass != NULL) {
            vulkan->head = pass->next;
            vulkan->tail = (vulkan->head != NULL) ? vulkan->tail : NULL;
        }

        hdrg_mutex_unlock(&vulkan->mutex);

        if (pass == NULL) {
            break;
        }

        ok = vulkan->fn.WaitForFences(vulkan->device, 1, &pass->fence, VK_TRUE, UINT64_MAX) ==
             VK_SUCCESS;

        if (!ok) {
            hdrg_atomic_u32_store_release(&vulkan->lost, 1u);
        }

        pass->done(pass->arg, ok);
        hdrg_vk_recycle(pass);
    }
}

/* the module's buffers (Uniform, StorageBuffer) by the names of their blocks into the kernel */
static bool hdrg_vk_reflect(const hdrg_plugin_api_t *api, const uint32_t *words, size_t count,
                            const char *entry, const char *uniform, const char *const *storage,
                            uint8_t n, hdrg_vulkan_kernel_t *kernel)
{
    const size_t block = sizeof(HDRG_VK_BLOCK) - 1u;
    const char *name;
    uint32_t bound, op, length, id, type, i, found, matched, *pointee, *binding, *named, *variables;
    uint8_t k;
    size_t at, len;
    bool entered, ok;

    if (count < 5u || words[0] != 0x07230203u || words[3] == 0 || words[3] > (1u << 22)) {
        return false;
    }

    bound = words[3];
    pointee = (uint32_t *)api->mem_calloc(api, (size_t)bound * 5u, sizeof(uint32_t));
    if (pointee == NULL) {
        return false;
    }

    binding = pointee + bound;
    named = binding + bound;
    variables = named + bound;
    entered = false;
    found = 0;
    ok = true;

    /* pointers' pointees, bindings (+1), names' word offsets, the buffers and their types */
    for (at = 5u; ok && at < count; at += length) {
        op = words[at] & 0xffffu;
        length = words[at] >> 16;
        ok = length != 0 && at + length <= count;

        if (!ok) {
            break;
        }

        if (op == 5u && length >= 3u && words[at + 1u] < bound) {
            named[words[at + 1u]] = (uint32_t)at + 2u;
        } else if (op == 15u && length >= 4u) {
            len = hdrg_vk_length((const char *)&words[at + 3u], ((size_t)length - 3u) * 4u);
            entered = entered || (strlen(entry) == len && memcmp(&words[at + 3u], entry, len) == 0);
        } else if (op == 32u && length == 4u && words[at + 1u] < bound) {
            pointee[words[at + 1u]] = words[at + 3u];
        } else if (op == 71u && length >= 4u && words[at + 1u] < bound && words[at + 2u] == 33u) {
            binding[words[at + 1u]] = words[at + 3u] + 1u;
        } else if (op == 71u && length >= 4u && words[at + 2u] == 34u) {
            ok = words[at + 3u] == 0;
        } else if (op == 59u && length >= 4u && words[at + 1u] < bound && words[at + 2u] < bound &&
                   found < bound && (words[at + 3u] == 2u || words[at + 3u] == 12u)) {
            variables[2u * found] = words[at + 2u] | ((words[at + 3u] == 2u) ? 0x80000000u : 0u);
            variables[2u * found + 1u] = words[at + 1u];
            found++;
        }
    }

    ok = ok && entered && found > 0;
    matched = 0;

    /* each buffer named: its block's name less the suffix, the uniform's or a storage one's */
    for (i = 0; ok && i < found; i++) {
        id = variables[2u * i] & 0x7fffffffu;
        type = variables[2u * i + 1u];
        ok = false;

        if (binding[id] == 0 || pointee[type] >= bound || named[pointee[type]] == 0) {
            break;
        }

        at = named[pointee[type]];
        name = (const char *)&words[at];
        len = hdrg_vk_length(name, ((size_t)(words[at - 2u] >> 16) - 2u) * 4u);

        if (len <= block || memcmp(name + len - block, HDRG_VK_BLOCK, block) != 0) {
            break;
        }

        len -= block;

        if ((variables[2u * i] & 0x80000000u) != 0) {
            ok = uniform != NULL && strlen(uniform) == len && memcmp(uniform, name, len) == 0;
            kernel->uniform = binding[id] - 1u;
            kernel->has_uniform = ok;
            matched += ok ? 1u : 0u;

            continue;
        }

        for (k = 0; k < n; k++) {
            if (strlen(storage[k]) == len && memcmp(storage[k], name, len) == 0) {
                kernel->bindings[k] = binding[id] - 1u;
                ok = true;
                matched++;
            }
        }
    }

    api->mem_free(api, pointee);

    return ok && matched == (uint32_t)n + ((uniform != NULL) ? 1u : 0u);
}

extern bool hdrg_vulkan_create(const hdrg_plugin_api_t *api, const hdrg_vulkan_device_t *device,
                               void *queue, hdrg_vulkan_t **out)
{
    VkPhysicalDeviceProperties properties;
    hdrg_vulkan_t *vulkan;
    bool opened;

    *out = NULL;

    /* one already on the device and queue for the table: shared */
    if (device != NULL) {
        hdrg_vk_lock();

        for (vulkan = hdrg_vk_registry; vulkan != NULL; vulkan = vulkan->next) {
            if (vulkan->device == (VkDevice)device->device && vulkan->queue == (VkQueue)queue &&
                vulkan->api == api) {
                vulkan->refs++;

                break;
            }
        }

        hdrg_vk_unlock();

        if (vulkan != NULL) {
            *out = vulkan;

            return true;
        }
    }

    vulkan = (hdrg_vulkan_t *)api->mem_calloc(api, 1, sizeof(*vulkan));
    if (vulkan == NULL) {
        return false;
    }

    vulkan->api = api;
    vulkan->own = device == NULL;
    hdrg_atomic_u32_init(&vulkan->lost, 0u);
    opened = (device == NULL) ? hdrg_vk_open_own(vulkan) : hdrg_vk_open_app(vulkan, device, queue);

    if (opened) {
        hdrg_vk_android_open(vulkan);
        vulkan->fn.GetPhysicalDeviceMemoryProperties(vulkan->physical, &vulkan->memory);
        vulkan->fn.GetPhysicalDeviceProperties(vulkan->physical, &properties);
        vulkan->uniform_align = (properties.limits.minUniformBufferOffsetAlignment > 0)
                                    ? properties.limits.minUniformBufferOffsetAlignment
                                    : 1u;
    }

    if (opened && hdrg_mutex_init(&vulkan->lock)) {
        if (hdrg_mutex_init(&vulkan->mutex)) {
            if (hdrg_cond_init(&vulkan->wake)) {
                if (hdrg_thread_create(&vulkan->thread, hdrg_vk_main, vulkan)) {
                    vulkan->refs = 1;
                    vulkan->share = vulkan->self;
                    vulkan->share.lock_queue = hdrg_vk_share_lock;
                    vulkan->share.user = vulkan;

                    hdrg_vk_lock();
                    vulkan->next = hdrg_vk_registry;
                    hdrg_vk_registry = vulkan;
                    hdrg_vk_unlock();

                    *out = vulkan;

                    return true;
                }

                hdrg_cond_destroy(&vulkan->wake);
            }

            hdrg_mutex_destroy(&vulkan->mutex);
        }

        hdrg_mutex_destroy(&vulkan->lock);
    }

    if (vulkan->own && vulkan->device != VK_NULL_HANDLE && vulkan->fn.DestroyDevice != NULL) {
        vulkan->fn.DestroyDevice(vulkan->device, NULL);
    }

    if (vulkan->own && vulkan->instance != VK_NULL_HANDLE && vulkan->fn.DestroyInstance != NULL) {
        vulkan->fn.DestroyInstance(vulkan->instance, NULL);
    }

    hdrg_vk_library_close(vulkan->library);
    hdrg_vk_library_close(vulkan->android);
    api->mem_free(api, vulkan);

    return false;
}

extern void hdrg_vulkan_destroy(hdrg_vulkan_t *vulkan)
{
    hdrg_vulkan_pass_t *pass;
    hdrg_vulkan_t **at;
    bool last;

    if (vulkan == NULL) {
        return;
    }

    /* the last holder's: out of the registry */
    hdrg_vk_lock();
    vulkan->refs--;
    last = vulkan->refs == 0;

    for (at = &hdrg_vk_registry; last && *at != NULL; at = &(*at)->next) {
        if (*at == vulkan) {
            *at = vulkan->next;

            break;
        }
    }

    hdrg_vk_unlock();

    if (!last) {
        return;
    }

    /* the passes submitted ended first */
    hdrg_mutex_lock(&vulkan->mutex);
    vulkan->stopping = true;
    hdrg_cond_broadcast(&vulkan->wake);
    hdrg_mutex_unlock(&vulkan->mutex);
    hdrg_thread_join(&vulkan->thread);

    while (vulkan->spare != NULL) {
        pass = vulkan->spare;
        vulkan->spare = pass->next;
        hdrg_vk_pass_free(pass);
    }

    hdrg_cond_destroy(&vulkan->wake);
    hdrg_mutex_destroy(&vulkan->mutex);
    hdrg_mutex_destroy(&vulkan->lock);

    if (vulkan->own) {
        vulkan->fn.DestroyDevice(vulkan->device, NULL);
        vulkan->fn.DestroyInstance(vulkan->instance, NULL);
    }

    hdrg_vk_library_close(vulkan->library);
    hdrg_vk_library_close(vulkan->android);
    vulkan->api->mem_free(vulkan->api, vulkan);
}

extern const hdrg_vulkan_device_t *hdrg_vulkan_device(const hdrg_vulkan_t *vulkan)
{
    return &vulkan->self;
}

extern const hdrg_vulkan_device_t *hdrg_vulkan_share(const hdrg_vulkan_t *vulkan)
{
    return &vulkan->share;
}

extern void *hdrg_vulkan_queue(const hdrg_vulkan_t *vulkan)
{
    return (void *)vulkan->queue;
}

extern hdrg_vulkan_buffer_t *hdrg_vulkan_buffer(hdrg_vulkan_t *vulkan, size_t size)
{
    return hdrg_vk_buffer_make(vulkan, size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL);
}

extern hdrg_vulkan_buffer_t *hdrg_vulkan_host_buffer(hdrg_vulkan_t *vulkan, size_t size,
                                                     uint8_t **contents)
{
    return hdrg_vk_buffer_make(
        vulkan, size,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, contents);
}

extern hdrg_vulkan_buffer_t *hdrg_vulkan_buffer_wrap(hdrg_vulkan_t *vulkan, void *object,
                                                     size_t size)
{
    hdrg_vulkan_buffer_t *buffer;

    if (object == NULL || size == 0) {
        return NULL;
    }

    buffer = (hdrg_vulkan_buffer_t *)vulkan->api->mem_calloc(vulkan->api, 1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->vulkan = vulkan;
    buffer->buffer = (VkBuffer)object;
    buffer->size = size;
    hdrg_atomic_u32_init(&buffer->refs, 1u);

    return buffer;
}

#if defined(_WIN32)
extern hdrg_vulkan_buffer_t *hdrg_vulkan_dmabuf(hdrg_vulkan_t *vulkan, int fd, size_t size)
{
    (void)vulkan;
    (void)fd;
    (void)size;

    return NULL;
}
#else
extern hdrg_vulkan_buffer_t *hdrg_vulkan_dmabuf(hdrg_vulkan_t *vulkan, int fd, size_t size)
{
    const hdrg_plugin_api_t *api = vulkan->api;
    hdrg_vulkan_buffer_t *buffer;
    VkExternalMemoryBufferCreateInfo external;
    VkMemoryFdPropertiesKHR fd_properties;
    VkImportMemoryFdInfoKHR import;
    VkMemoryDedicatedAllocateInfo dedicated;
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    off_t end;
    uint32_t type;
    int copy;

    if (vulkan->fn.GetMemoryFdPropertiesKHR == NULL || fd < 0) {
        return NULL;
    }

    end = lseek(fd, 0, SEEK_END);

    if (size == 0) {
        size = (end > 0) ? (size_t)end : 0u;
    }

    if (size == 0 || (end > 0 && (uint64_t)size > (uint64_t)end)) {
        return NULL;
    }

    buffer = (hdrg_vulkan_buffer_t *)api->mem_calloc(api, 1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->vulkan = vulkan;
    buffer->size = size;
    hdrg_atomic_u32_init(&buffer->refs, 1u);
    memset(&external, 0, sizeof(external));
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext = &external;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vulkan->fn.CreateBuffer(vulkan->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) {
        api->mem_free(api, buffer);

        return NULL;
    }

    copy = dup(fd);
    memset(&fd_properties, 0, sizeof(fd_properties));
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    vulkan->fn.GetBufferMemoryRequirements(vulkan->device, buffer->buffer, &requirements);

    if (copy < 0 || vulkan->fn.GetMemoryFdPropertiesKHR(
                        vulkan->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, copy,
                        &fd_properties) != VK_SUCCESS) {
        type = UINT32_MAX;
    } else {
        type = hdrg_vk_memory_type(
            vulkan, requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0, 0);
    }

    memset(&dedicated, 0, sizeof(dedicated));
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = buffer->buffer;
    memset(&import, 0, sizeof(import));
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = copy;
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &import;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    /* the fd is the memory's once imported */
    if (type == UINT32_MAX || (end > 0 && requirements.size > (VkDeviceSize)end) ||
        vulkan->fn.AllocateMemory(vulkan->device, &allocate, NULL, &buffer->memory) != VK_SUCCESS) {
        if (copy >= 0) {
            (void)close(copy);
        }

        buffer->memory = VK_NULL_HANDLE;
        vulkan->fn.DestroyBuffer(vulkan->device, buffer->buffer, NULL);
        api->mem_free(api, buffer);

        return NULL;
    }

    if (vulkan->fn.BindBufferMemory(vulkan->device, buffer->buffer, buffer->memory, 0) !=
        VK_SUCCESS) {
        hdrg_vk_buffer_release(buffer);

        return NULL;
    }

    return buffer;
}
#endif

extern void *hdrg_vulkan_buffer_object(const hdrg_vulkan_buffer_t *buffer)
{
    return (void *)buffer->buffer;
}

extern size_t hdrg_vulkan_buffer_size(const hdrg_vulkan_buffer_t *buffer)
{
    return buffer->size;
}

extern void hdrg_vulkan_buffer_free(hdrg_vulkan_buffer_t *buffer)
{
    hdrg_vk_buffer_release(buffer);
}

extern hdrg_vulkan_kernel_t *hdrg_vulkan_kernel(hdrg_vulkan_t *vulkan, const void *spirv,
                                                size_t size, const char *entry, const char *uniform,
                                                const char *const *storage, uint8_t n)
{
    const hdrg_plugin_api_t *api = vulkan->api;
    hdrg_vulkan_kernel_t *kernel;
    VkDescriptorSetLayoutBinding bindings[HDRG_VULKAN_BUFFERS + 1u];
    VkDescriptorSetLayoutCreateInfo set_info;
    VkPipelineLayoutCreateInfo layout_info;
    VkShaderModuleCreateInfo module_info;
    VkComputePipelineCreateInfo pipeline_info;
    uint8_t i, count;

    if (spirv == NULL || size % 4u != 0 || ((uintptr_t)spirv % 4u) != 0 ||
        n > HDRG_VULKAN_BUFFERS) {
        return NULL;
    }

    kernel = (hdrg_vulkan_kernel_t *)api->mem_calloc(api, 1, sizeof(*kernel));
    if (kernel == NULL) {
        return NULL;
    }

    kernel->vulkan = vulkan;
    kernel->count = n;
    hdrg_atomic_u32_init(&kernel->refs, 1u);

    if (!hdrg_vk_reflect(api, (const uint32_t *)spirv, size / 4u, entry, uniform, storage, n,
                         kernel)) {
        hdrg_vk_kernel_release(kernel);

        return NULL;
    }

    memset(bindings, 0, sizeof(bindings));
    count = 0;

    if (kernel->has_uniform) {
        bindings[count].binding = kernel->uniform;
        bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[count].descriptorCount = 1;
        bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        count++;
    }

    for (i = 0; i < n; i++) {
        bindings[count].binding = kernel->bindings[i];
        bindings[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[count].descriptorCount = 1;
        bindings[count].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        count++;
    }

    memset(&set_info, 0, sizeof(set_info));
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set_info.bindingCount = count;
    set_info.pBindings = bindings;
    memset(&module_info, 0, sizeof(module_info));
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = size;
    module_info.pCode = (const uint32_t *)spirv;

    if (vulkan->fn.CreateDescriptorSetLayout(vulkan->device, &set_info, NULL, &kernel->set) !=
            VK_SUCCESS ||
        vulkan->fn.CreateShaderModule(vulkan->device, &module_info, NULL, &kernel->module) !=
            VK_SUCCESS) {
        hdrg_vk_kernel_release(kernel);

        return NULL;
    }

    memset(&layout_info, 0, sizeof(layout_info));
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &kernel->set;

    if (vulkan->fn.CreatePipelineLayout(vulkan->device, &layout_info, NULL, &kernel->layout) !=
        VK_SUCCESS) {
        hdrg_vk_kernel_release(kernel);

        return NULL;
    }

    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = kernel->module;
    pipeline_info.stage.pName = entry;
    pipeline_info.layout = kernel->layout;

    if (vulkan->fn.CreateComputePipelines(vulkan->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                          &kernel->pipeline) != VK_SUCCESS) {
        kernel->pipeline = VK_NULL_HANDLE;
        hdrg_vk_kernel_release(kernel);

        return NULL;
    }

    return kernel;
}

extern void hdrg_vulkan_kernel_free(hdrg_vulkan_kernel_t *kernel)
{
    hdrg_vk_kernel_release(kernel);
}

extern hdrg_vulkan_pass_t *hdrg_vulkan_begin(hdrg_vulkan_t *vulkan)
{
    const hdrg_plugin_api_t *api = vulkan->api;
    hdrg_vulkan_pass_t *pass;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo allocate;
    VkFenceCreateInfo fence_info;
    VkCommandBufferBeginInfo begin;

    if (hdrg_atomic_u32_load_acquire(&vulkan->lost) != 0) {
        return NULL;
    }

    hdrg_mutex_lock(&vulkan->lock);
    pass = vulkan->spare;

    if (pass != NULL) {
        vulkan->spare = pass->next;
    }

    hdrg_mutex_unlock(&vulkan->lock);

    if (pass == NULL) {
        pass = (hdrg_vulkan_pass_t *)api->mem_calloc(api, 1, sizeof(*pass));
        if (pass == NULL) {
            return NULL;
        }

        pass->vulkan = vulkan;
        memset(&pool_info, 0, sizeof(pool_info));
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = vulkan->self.queue_family;
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        if (vulkan->fn.CreateCommandPool(vulkan->device, &pool_info, NULL, &pass->pool) !=
                VK_SUCCESS ||
            vulkan->fn.CreateFence(vulkan->device, &fence_info, NULL, &pass->fence) != VK_SUCCESS) {
            hdrg_vk_pass_free(pass);

            return NULL;
        }

        memset(&allocate, 0, sizeof(allocate));
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pass->pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        pass->arena = hdrg_vulkan_host_buffer(vulkan, HDRG_VK_ARENA, &pass->arena_view);
        pass->arena_size = HDRG_VK_ARENA;

        if (pass->arena == NULL || vulkan->fn.AllocateCommandBuffers(
                                       vulkan->device, &allocate, &pass->commands) != VK_SUCCESS) {
            hdrg_vk_pass_free(pass);

            return NULL;
        }
    }

    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vulkan->fn.BeginCommandBuffer(pass->commands, &begin) != VK_SUCCESS) {
        hdrg_vk_recycle(pass);

        return NULL;
    }

    return pass;
}

extern void hdrg_vulkan_dispatch(hdrg_vulkan_pass_t *pass, const hdrg_vulkan_kernel_t *kernel,
                                 const void *uniform, size_t uniform_size,
                                 hdrg_vulkan_buffer_t *const *buffers, uint32_t groups_x,
                                 uint32_t groups_y)
{
    hdrg_vulkan_t *const vulkan = pass->vulkan;
    hdrg_vulkan_buffer_t *arena;
    VkWriteDescriptorSet writes[HDRG_VULKAN_BUFFERS + 1u];
    VkDescriptorBufferInfo infos[HDRG_VULKAN_BUFFERS + 1u];
    VkDeviceSize offset, size;
    uint8_t i, count, *view;

    if (pass->failed) {
        return;
    }

    offset = (pass->arena_used + vulkan->uniform_align - 1u) / vulkan->uniform_align *
             vulkan->uniform_align;

    /*
     * the uniforms' buffer full: on to one twice its size (kept for the pass's next uses, so a
     * pass that fills one does so once), the old one kept till the GPU is through
     */
    if (kernel->has_uniform && offset + uniform_size > pass->arena_size &&
        uniform_size <= HDRG_VK_ARENA_MAX) {
        size = pass->arena_size;

        while (size < uniform_size || size < 2u * pass->arena_size) {
            size *= 2u;
        }

        size = (size < HDRG_VK_ARENA_MAX) ? size : HDRG_VK_ARENA_MAX;

        /* the pass keeps its buffer and its view until the new one is in place */
        arena = hdrg_vulkan_host_buffer(vulkan, (size_t)size, &view);
        if (arena == NULL || !hdrg_vk_hold_buffer(pass, pass->arena)) {
            hdrg_vk_buffer_release(arena);
            pass->failed = true;

            return;
        }

        hdrg_vk_buffer_release(pass->arena);
        pass->arena = arena;
        pass->arena_view = view;
        pass->arena_size = (size_t)size;
        offset = 0;
    }

    if ((size_t)offset + uniform_size > pass->arena_size ||
        !hdrg_vk_hold(pass, (void ***)&pass->kernels, &pass->kernel_count, &pass->kernel_cap,
                      (void *)kernel)) {
        pass->failed = true;

        return;
    }

    (void)hdrg_atomic_u32_fetch_add_acq_rel(&((hdrg_vulkan_kernel_t *)kernel)->refs, 1u);

    for (i = 0; i < kernel->count; i++) {
        if (!hdrg_vk_hold_buffer(pass, buffers[i])) {
            return;
        }
    }

    memset(writes, 0, sizeof(writes));
    count = 0;

    if (kernel->has_uniform) {
        memcpy(pass->arena_view + offset, uniform, uniform_size);
        pass->arena_used = (size_t)offset + uniform_size;
        infos[count].buffer = pass->arena->buffer;
        infos[count].offset = offset;
        infos[count].range = uniform_size;
        writes[count].dstBinding = kernel->uniform;
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        count++;
    }

    for (i = 0; i < kernel->count; i++) {
        infos[count].buffer = buffers[i]->buffer;
        infos[count].offset = 0;
        infos[count].range = VK_WHOLE_SIZE;
        writes[count].dstBinding = kernel->bindings[i];
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        count++;
    }

    for (i = 0; i < count; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &infos[i];
    }

    hdrg_vk_barrier(pass, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, HDRG_VK_ACCESS);
    vulkan->fn.CmdBindPipeline(pass->commands, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
    vulkan->fn.CmdPushDescriptorSetKHR(pass->commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       kernel->layout, 0, count, writes);
    vulkan->fn.CmdDispatch(pass->commands, groups_x, groups_y, 1);
}

extern void hdrg_vulkan_copy_buffer(hdrg_vulkan_pass_t *pass, hdrg_vulkan_buffer_t *src,
                                    hdrg_vulkan_buffer_t *dst, size_t size)
{
    VkBufferCopy region;

    if (pass->failed || size == 0 || !hdrg_vk_hold_buffer(pass, src) ||
        !hdrg_vk_hold_buffer(pass, dst)) {
        return;
    }

    memset(&region, 0, sizeof(region));
    region.size = size;
    hdrg_vk_barrier(pass, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, HDRG_VK_ACCESS);
    pass->vulkan->fn.CmdCopyBuffer(pass->commands, src->buffer, dst->buffer, 1, &region);
}

extern bool hdrg_vulkan_commit(hdrg_vulkan_pass_t *pass, hdrg_vulkan_done_fn_t done, void *arg)
{
    hdrg_vulkan_t *const vulkan = pass->vulkan;
    VkSubmitInfo submit;
    VkResult result;
    bool ok;

    hdrg_vk_barrier(pass, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);

    if (vulkan->fn.EndCommandBuffer(pass->commands) != VK_SUCCESS || pass->failed) {
        hdrg_vk_recycle(pass);

        return false;
    }

    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &pass->commands;

    hdrg_mutex_lock(&vulkan->lock);

    if (vulkan->self.lock_queue != NULL) {
        vulkan->self.lock_queue(vulkan->self.user, true);
    }

    result = vulkan->fn.QueueSubmit(vulkan->queue, 1, &submit, pass->fence);

    if (vulkan->self.lock_queue != NULL) {
        vulkan->self.lock_queue(vulkan->self.user, false);
    }

    hdrg_mutex_unlock(&vulkan->lock);

    if (result != VK_SUCCESS) {
        hdrg_vk_recycle(pass);

        return false;
    }

    if (done == NULL) {
        ok = vulkan->fn.WaitForFences(vulkan->device, 1, &pass->fence, VK_TRUE, UINT64_MAX) ==
             VK_SUCCESS;

        if (!ok) {
            hdrg_atomic_u32_store_release(&vulkan->lost, 1u);
        }

        hdrg_vk_recycle(pass);

        return ok;
    }

    pass->done = done;
    pass->arg = arg;
    pass->next = NULL;

    hdrg_mutex_lock(&vulkan->mutex);

    if (vulkan->tail != NULL) {
        vulkan->tail->next = pass;
    } else {
        vulkan->head = pass;
    }

    vulkan->tail = pass;
    hdrg_cond_signal(&vulkan->wake);
    hdrg_mutex_unlock(&vulkan->mutex);

    return true;
}

extern hdrg_vulkan_image_t *hdrg_vulkan_ahb_image(hdrg_vulkan_t *vulkan, void *ahb, bool write,
                                                  uint8_t *format, uint32_t *width,
                                                  uint32_t *height)
{
    const hdrg_plugin_api_t *api = vulkan->api;
    const VkFormatFeatureFlags needed =
        write ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT : VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkAndroidHardwareBufferFormatPropertiesANDROID formats;
    VkAndroidHardwareBufferPropertiesANDROID properties;
    VkImportAndroidHardwareBufferInfoANDROID import;
    VkExternalMemoryImageCreateInfo external;
    VkMemoryDedicatedAllocateInfo dedicated;
    VkMemoryAllocateInfo allocate;
    VkImageCreateInfo info;
    hdrg_vk_ahb_desc_t desc;
    hdrg_vulkan_image_t *image;
    uint32_t type;

    if (vulkan->fn.GetAndroidHardwareBufferPropertiesANDROID == NULL ||
        vulkan->ahb_describe == NULL || ahb == NULL) {
        return NULL;
    }

    memset(&desc, 0, sizeof(desc));
    vulkan->ahb_describe(ahb, &desc);
    memset(&formats, 0, sizeof(formats));
    formats.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    memset(&properties, 0, sizeof(properties));
    properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    properties.pNext = &formats;

    if (vulkan->fn.GetAndroidHardwareBufferPropertiesANDROID(
            vulkan->device, (const struct AHardwareBuffer *)ahb, &properties) != VK_SUCCESS ||
        desc.width == 0 || desc.height == 0 || desc.layers > 1u ||
        (formats.formatFeatures & needed) != needed) {
        return NULL;
    }

    /* the 4:2:0 formats' images have even sides */
    if ((desc.width % 2u) != 0 || (desc.height % 2u) != 0) {
        return NULL;
    }

    /* the formats of Vulkan's own (a vendor's external format alone: none) */
    if (formats.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        *format = HDRG_VULKAN_AHB_NV12;
    } else if (formats.format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) {
        *format = HDRG_VULKAN_AHB_P010;
    } else {
        return NULL;
    }

    image = (hdrg_vulkan_image_t *)api->mem_calloc(api, 1, sizeof(*image));
    if (image == NULL) {
        return NULL;
    }

    image->vulkan = vulkan;
    memset(&external, 0, sizeof(external));
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = formats.format;
    info.extent.width = desc.width;
    info.extent.height = desc.height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = write ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vulkan->fn.CreateImage(vulkan->device, &info, NULL, &image->image) != VK_SUCCESS) {
        api->mem_free(api, image);

        return NULL;
    }

    /* an AHardwareBuffer's image: its memory dedicated, imported (the buffer's reference) */
    memset(&dedicated, 0, sizeof(dedicated));
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image->image;
    memset(&import, 0, sizeof(import));
    import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import.pNext = &dedicated;
    import.buffer = (struct AHardwareBuffer *)ahb;
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &import;
    allocate.allocationSize = properties.allocationSize;
    type = hdrg_vk_memory_type(vulkan, properties.memoryTypeBits, 0, 0);
    allocate.memoryTypeIndex = type;

    if (type == UINT32_MAX ||
        vulkan->fn.AllocateMemory(vulkan->device, &allocate, NULL, &image->memory) != VK_SUCCESS) {
        image->memory = VK_NULL_HANDLE;
        hdrg_vulkan_image_free(image);

        return NULL;
    }

    if (vulkan->fn.BindImageMemory(vulkan->device, image->image, image->memory, 0) != VK_SUCCESS) {
        hdrg_vulkan_image_free(image);

        return NULL;
    }

    *width = desc.width;
    *height = desc.height;

    return image;
}

extern void hdrg_vulkan_image_free(hdrg_vulkan_image_t *image)
{
    hdrg_vulkan_t *vulkan;

    if (image == NULL) {
        return;
    }

    vulkan = image->vulkan;
    vulkan->fn.DestroyImage(vulkan->device, image->image, NULL);

    if (image->memory != VK_NULL_HANDLE) {
        vulkan->fn.FreeMemory(vulkan->device, image->memory, NULL);
    }

    vulkan->api->mem_free(vulkan->api, image);
}

extern void hdrg_vulkan_copy_image(hdrg_vulkan_pass_t *pass, hdrg_vulkan_buffer_t *buffer,
                                   const hdrg_vulkan_image_t *image,
                                   const hdrg_vulkan_plane_copy_t *copies, uint8_t count,
                                   bool to_image)
{
    hdrg_vulkan_t *const vulkan = pass->vulkan;
    VkImageMemoryBarrier barrier;
    VkBufferImageCopy region;
    uint8_t k;

    if (pass->failed || count == 0 || !hdrg_vk_hold_buffer(pass, buffer)) {
        return;
    }

    for (k = 0; k < count; k++) {
        if (copies[k].texel_bytes == 0 || copies[k].pitch % copies[k].texel_bytes != 0) {
            pass->failed = true;

            return;
        }
    }

    /*
     * the image from the producer outside Vulkan (its layout GENERAL kept: no transition that
     * could discard it), back to it after the copies
     */
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = vulkan->self.queue_family;
    barrier.image = image->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vulkan->fn.CmdPipelineBarrier(pass->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    hdrg_vk_barrier(pass, VK_PIPELINE_STAGE_TRANSFER_BIT, HDRG_VK_ACCESS);

    for (k = 0; k < count; k++) {
        memset(&region, 0, sizeof(region));
        region.bufferOffset = copies[k].at;
        region.bufferRowLength = (uint32_t)(copies[k].pitch / copies[k].texel_bytes);
        region.imageSubresource.aspectMask =
            (copies[k].plane == 0) ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = copies[k].texels;
        region.imageExtent.height = copies[k].rows;
        region.imageExtent.depth = 1;

        if (to_image) {
            vulkan->fn.CmdCopyBufferToImage(pass->commands, buffer->buffer, image->image,
                                            VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        } else {
            vulkan->fn.CmdCopyImageToBuffer(pass->commands, image->image, VK_IMAGE_LAYOUT_GENERAL,
                                            buffer->buffer, 1, &region);
        }
    }

    barrier.srcAccessMask = to_image ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    barrier.dstAccessMask = 0;
    barrier.srcQueueFamilyIndex = vulkan->self.queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vulkan->fn.CmdPipelineBarrier(pass->commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                                  &barrier);
}
