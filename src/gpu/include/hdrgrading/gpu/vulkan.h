/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_VULKAN_H
#define HDRG_GPU_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

/*
 * A Vulkan device (HDRG_WITH_VULKAN) for the Vulkan filters
 * and the core, in the shape of src/gpu/metal: the app's device and queue
 * (HDRG_DEVICE_VULKAN: hdrg_vulkan_device_t) or one of its own, the system's
 * loader's (dlopen at create: the library does not link it). Every object
 * here may be used from any thread: a pass is recorded on its caller's
 * thread into a command buffer of its own, its end reported on the device's
 * thread waiting for the fences in order (hdrg_vulkan_commit). The kernels
 * are Tint's SPIR-V of the WGSL (src/<filters|core>/spirv/,
 * scripts/gen_shaders.sh), their buffers pushed (VK_KHR_push_descriptor).
 * A pass keeps the buffers and kernels it uses until the GPU is through
 * with them (freeing one meanwhile only drops the caller's reference), and
 * orders its commands with full barriers, the ones before it on the queue
 * included. One device for one VkDevice, VkQueue and table: the filters
 * and the core of a context share it (its lock).
 */
#define HDRG_VULKAN_BUFFERS  8u /* a kernel's storage buffers at most */
#define HDRG_VULKAN_AHB_NV12 1u /* an AHardwareBuffer's Y8Cb8Cr8_420: 8-bit Y, CbCr interleaved */
#define HDRG_VULKAN_AHB_P010 2u /* ... YCbCr_P010: 10 bits in 16 (the top), CbCr interleaved */

typedef struct hdrg_vulkan hdrg_vulkan_t;
typedef struct hdrg_vulkan_buffer hdrg_vulkan_buffer_t;
typedef struct hdrg_vulkan_image hdrg_vulkan_image_t;
typedef struct hdrg_vulkan_kernel hdrg_vulkan_kernel_t;
typedef struct hdrg_vulkan_pass hdrg_vulkan_pass_t;

/* a committed pass's end, on the device's thread: ok false when the GPU failed it */
typedef void (*hdrg_vulkan_done_fn_t)(void *arg, bool ok);

/*
 * device NULL: a device of its own (the first of the system's of the kinds
 * discrete, integrated, virtual, CPU with a compute queue and push
 * descriptors; false: none); otherwise the app's and queue its VkQueue
 * (shared with the others on them for the table). api: the plugin table
 * whose allocator it uses until destroyed
 */
extern bool hdrg_vulkan_create(const hdrg_plugin_api_t *api, const hdrg_vulkan_device_t *device,
                               void *queue, hdrg_vulkan_t **out);
extern void hdrg_vulkan_destroy(hdrg_vulkan_t *vulkan);
/* its device (its own: one of its own describing it) and VkQueue, for others to share */
extern const hdrg_vulkan_device_t *hdrg_vulkan_device(const hdrg_vulkan_t *vulkan);
/*
 * ... and the device as another binary's object on it takes it (filter_gpu_share): its
 * lock_queue the queue's lock this one's submissions take (the app's after it)
 */
extern const hdrg_vulkan_device_t *hdrg_vulkan_share(const hdrg_vulkan_t *vulkan);
extern void *hdrg_vulkan_queue(const hdrg_vulkan_t *vulkan);
/*
 * a buffer of size bytes of the GPU's (device local memory where there is
 * any); NULL: failed, or size 0
 */
extern hdrg_vulkan_buffer_t *hdrg_vulkan_buffer(hdrg_vulkan_t *vulkan, size_t size);
/*
 * a buffer of size bytes in host visible, coherent memory, mapped:
 * *contents, the CPU's view of it, read and written by the GPU in place;
 * NULL: failed
 */
extern hdrg_vulkan_buffer_t *hdrg_vulkan_host_buffer(hdrg_vulkan_t *vulkan, size_t size,
                                                     uint8_t **contents);
/* the app's VkBuffer of size bytes on the device (not destroyed by the library) */
extern hdrg_vulkan_buffer_t *hdrg_vulkan_buffer_wrap(hdrg_vulkan_t *vulkan, void *object,
                                                     size_t size);
/*
 * the dma-buf fd (a duplicate of it imported, fd kept the caller's) as a
 * buffer of its bytes (size 0: the dma-buf's own); NULL: the device takes
 * none, or failed
 */
extern hdrg_vulkan_buffer_t *hdrg_vulkan_dmabuf(hdrg_vulkan_t *vulkan, int fd, size_t size);
/*
 * An AHardwareBuffer (Android: the device with VK_ANDROID_external_memory_android_hardware_buffer
 * and VK_EXT_queue_family_foreign) as an image of the device, its memory imported (the buffer's
 * reference while the image lives), for the transfers of reading it or writing it (write): its
 * format (HDRG_VULKAN_AHB_*: a two-plane 4:2:0 of Vulkan's own), width and height. NULL: the
 * device takes none, a vendor's format alone, a format feature missing, odd sides, or failed
 */
extern hdrg_vulkan_image_t *hdrg_vulkan_ahb_image(hdrg_vulkan_t *vulkan, void *ahb, bool write,
                                                  uint8_t *format, uint32_t *width,
                                                  uint32_t *height);
extern void hdrg_vulkan_image_free(hdrg_vulkan_image_t *image);
/* its VkBuffer and bytes */
extern void *hdrg_vulkan_buffer_object(const hdrg_vulkan_buffer_t *buffer);
extern size_t hdrg_vulkan_buffer_size(const hdrg_vulkan_buffer_t *buffer);
extern void hdrg_vulkan_buffer_free(hdrg_vulkan_buffer_t *buffer);
/*
 * the compute pipeline of entry in spirv (size bytes, word aligned: Tint's
 * SPIR-V of a WGSL entry point): its uniform named uniform (NULL: none) and
 * its n storage buffers named storage, bound in that order, their bindings
 * the WGSL's (Tint names a buffer's block <name>_block_tint_explicit_layout).
 * NULL: it does not build, or the names are not all the module's buffers
 */
extern hdrg_vulkan_kernel_t *hdrg_vulkan_kernel(hdrg_vulkan_t *vulkan, const void *spirv,
                                                size_t size, const char *entry, const char *uniform,
                                                const char *const *storage, uint8_t n);
extern void hdrg_vulkan_kernel_free(hdrg_vulkan_kernel_t *kernel);
/* a command buffer to record; NULL: failed */
extern hdrg_vulkan_pass_t *hdrg_vulkan_begin(hdrg_vulkan_t *vulkan);
/*
 * kernel over groups_x x groups_y workgroups (their size the SPIR-V's): the
 * uniform's bytes and the storage buffers in the kernel's order, each bound
 * whole
 */
extern void hdrg_vulkan_dispatch(hdrg_vulkan_pass_t *pass, const hdrg_vulkan_kernel_t *kernel,
                                 const void *uniform, size_t uniform_size,
                                 hdrg_vulkan_buffer_t *const *buffers, uint32_t groups_x,
                                 uint32_t groups_y);
/* a copy of size bytes from the start of src to the start of dst */
extern void hdrg_vulkan_copy_buffer(hdrg_vulkan_pass_t *pass, hdrg_vulkan_buffer_t *src,
                                    hdrg_vulkan_buffer_t *dst, size_t size);
/* a plane's copy (hdrg_vulkan_copy_image): its top-left texels x rows, pitch bytes from at */
typedef struct {
    size_t at, pitch;
    uint32_t texels, rows;
    uint8_t plane, texel_bytes; /* 0: Y, 1: CbCr */
} hdrg_vulkan_plane_copy_t;

/*
 * count planes of an AHardwareBuffer's image copied from the buffer's rows (to_image) or into
 * them in the pass, the image acquired once from its producer outside Vulkan
 * (VK_QUEUE_FAMILY_FOREIGN_EXT, layout GENERAL) and released back to it after the copies
 */
extern void hdrg_vulkan_copy_image(hdrg_vulkan_pass_t *pass, hdrg_vulkan_buffer_t *buffer,
                                   const hdrg_vulkan_image_t *image,
                                   const hdrg_vulkan_plane_copy_t *copies, uint8_t count,
                                   bool to_image);
/*
 * ends the pass and submits it: done(arg, ok) once the GPU is through (done
 * NULL: the call waits and returns ok; false otherwise only when it could
 * not submit, and done is not called). Frees the pass
 */
extern bool hdrg_vulkan_commit(hdrg_vulkan_pass_t *pass, hdrg_vulkan_done_fn_t done, void *arg);

#endif /* HDRG_GPU_VULKAN_H */
