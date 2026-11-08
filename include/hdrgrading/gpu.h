/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — the app's GPU: the device the app shares with the library
 * and its filters (hdrg_context_set_device), the frames a session takes and
 * gives on that GPU (hdrg_device_frame_t), and the shaders and the state to
 * run the apply pass on the app's own GPU (hdrg_shader, hdrg_apply_state_t)
 */
#ifndef HDRGRADING_GPU_H
#define HDRGRADING_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPU APIs whose device the app shares with the filters (hdrg_context_set_device) */
#define HDRG_DEVICE_CUDA   UINT32_C(1) /* device: CUcontext, queue: CUstream */
#define HDRG_DEVICE_METAL  UINT32_C(2) /* id<MTLDevice>, id<MTLCommandQueue> */
#define HDRG_DEVICE_VULKAN UINT32_C(3) /* const hdrg_vulkan_device_t *, VkQueue */
#define HDRG_DEVICE_WEBGPU UINT32_C(4) /* WGPUDevice, WGPUQueue (its instance has TimedWaitAny) */
#define HDRG_DEVICE_KINDS  4u

/*
 * HDRG_DEVICE_VULKAN's device (Vulkan 1.1 or later): the handles the app
 * made it with, kept alive by the app as the device is. The queue (VkQueue,
 * of queue_family, compute) takes the library's submissions from its own
 * threads: lock_queue (NULL: none, the app submits nothing to the queue
 * meanwhile) is called around each, lock true then false. extensions: the
 * device's enabled ones, VK_KHR_push_descriptor among them;
 * VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf for dma-buf
 * frames (HDRG_FRAME_DMABUF). The handles' types are Vulkan's
 */
typedef struct {
    uint32_t struct_size;  /* sizeof(hdrg_vulkan_device_t) */
    uint32_t api_version;  /* the device's VK_API_VERSION */
    uint32_t queue_family; /* the queue's */
    uint32_t extension_count;
    const char *const *extensions;
    void *get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr (NULL: the system's loader's) */
    void *instance, *physical_device, *device; /* VkInstance, VkPhysicalDevice, VkDevice */
    void (*lock_queue)(void *user, bool lock);
    void *user;
} hdrg_vulkan_device_t;

/* the kinds of a frame on the app's GPU (hdrg_device_frame_t.kind) */
#define HDRG_FRAME_BUFFERS         UINT32_C(0) /* buffers of the context's device */
#define HDRG_FRAME_CVPIXELBUFFER   UINT32_C(1) /* Apple: a CVPixelBufferRef (IOSurface) */
#define HDRG_FRAME_DMABUF          UINT32_C(2) /* Linux: dma-buf fds (Vulkan) */
#define HDRG_FRAME_AHARDWAREBUFFER UINT32_C(3) /* Android: an AHardwareBuffer (Vulkan) */

/*
 * A frame on the app's GPU (hdrg_session_config_t.device), its Y'CbCr
 * planes (Y, Cb, Cr; Y, CbCr interleaved) or its R'G'B' (plane 0):
 * HDRG_FRAME_BUFFERS: plane i is rows of pitches[i] bytes from offsets[i]
 *   in handles[i] (planes may share a buffer; offsets and pitches whole
 *   words: multiples of 4), the buffers of the context's device of the
 *   kind (hdrg_context_set_device): HDRG_DEVICE_METAL an id<MTLBuffer>,
 *   HDRG_DEVICE_VULKAN a VkBuffer of storage usage of sizes[i] bytes (Vulkan
 *   cannot tell them), HDRG_DEVICE_WEBGPU a WGPUBuffer of Storage usage.
 * HDRG_FRAME_DMABUF: as HDRG_FRAME_BUFFERS, handles[i] a dma-buf's fd cast
 *   to a pointer ((void *)(intptr_t)fd; planes may share one), its layout
 *   linear (DRM_FORMAT_MOD_LINEAR), sizes[i] its bytes (0: the dma-buf's
 *   own), imported while the library reads or writes it (the fd stays the
 *   app's); the context's device HDRG_DEVICE_VULKAN (its own or the app's,
 *   with VK_EXT_external_memory_dma_buf).
 * HDRG_FRAME_CVPIXELBUFFER: handles[0] a CVPixelBufferRef backed by an
 *   IOSurface (a decoder's, an encoder's pool), its planes and pitches its
 *   own, its pixel format the side's (420v / 420f / x420 / xf20 / y420 and
 *   their 4:2:2 and 4:4:4 kinds; any device on Apple: HDRG_DEVICE_METAL or
 *   HDRG_DEVICE_WEBGPU, the context's own too). Not locked by the library.
 * HDRG_FRAME_AHARDWAREBUFFER: handles[0] an AHardwareBuffer * (a decoder's
 *   image, an encoder's input; Android 8 / API 26 and later), its planes
 *   and pitches its own (offsets, pitches and sizes not read), its format
 *   one of Vulkan's own (AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 for an NV12
 *   side: 4:2:0, 8 bits, interleaved; AHARDWAREBUFFER_FORMAT_YCbCr_P010
 *   for a P010 side: 10 bits, msb; a vendor's external format alone:
 *   HDRG_ERR_UNSUPPORTED), its size the frame's (even sides: the formats'
 *   4:2:0; odd ones HDRG_ERR_UNSUPPORTED); the context's device
 *   HDRG_DEVICE_VULKAN (its own, or the app's with
 *   VK_ANDROID_external_memory_android_hardware_buffer,
 *   VK_EXT_queue_family_foreign and the samplerYcbcrConversion feature).
 *   Imported while the library reads or writes it, taken from and given
 *   back to its producer outside Vulkan (VK_QUEUE_FAMILY_FOREIGN_EXT, layout
 *   GENERAL): the app waits for the producer's fence before the send, and
 *   the receive returns once the output is written. Not locked by the
 *   library
 */
typedef struct {
    uint32_t kind; /* HDRG_FRAME_* */
    void *handles[3];
    uint64_t offsets[3];
    uint64_t sizes[3]; /* the handles' bytes where the API cannot tell them (else 0) */
    uint32_t pitches[3];
} hdrg_device_frame_t;

/* the shaders' layout (hdrg_shader, hdrg_apply_state_t): their bindings and uniforms */
#define HDRG_SHADER_LAYOUT UINT32_C(3)

/* hdrg_shader's languages */
#define HDRG_SHADER_WGSL  UINT32_C(1) /* a module's text: its entry points all (WebGPU) */
#define HDRG_SHADER_SPIRV UINT32_C(2) /* an entry point's SPIR-V words (Vulkan) */
#define HDRG_SHADER_HLSL  UINT32_C(3) /* an entry point's HLSL text (Direct3D) */

/* hdrg_shader's modules and their entry points */
#define HDRG_SHADER_APPLY UINT32_C(1) /* the apply pass: "apply", "yuv" */
#define HDRG_SHADER_CONVERT                                                                        \
    UINT32_C(2) /* Y'CbCr input to 16-bit R'G'B': "sites", "slopes", "pixels" */

/*
 * A frame's work for the app's own GPU (hdrg_session_receive_state): the
 * state to run the shaders of hdrg_shader with over the app's copy of the
 * frame sent, the bindings of group 0 (SPIR-V: set 0; HLSL: Tint's space 0
 * registers, the uniform a cbuffer, the storage byte address buffers) as
 * below, every workgroup 64 x 1. Bytes and pitches in bytes.
 * Y'CbCr input first: HDRG_SHADER_CONVERT with
 *   0 uniform: convert_params, convert_params_size bytes
 *   1 read-only storage: the frame sent, its planes packed into yuv_size
 *     bytes: Y' rows of yuv_y_pitch from 0, Cb rows of yuv_c_pitch from
 *     yuv_cb_at, Cr from yuv_cr_at (interleaved input: Cb, Cr alternating in
 *     the rows from yuv_cb_at = yuv_cr_at), the samples as the session's
 *   2, 3, 4 storage: base_size, knots_size, values_size bytes (the GPU's)
 *   5 storage: the 16-bit R'G'B' rows, source_pitch each: the apply's 1
 * and entry points "sites", "slopes", "pixels" over convert_groups (a
 * group of 0 x 0: none). Then HDRG_SHADER_APPLY with
 *   0 uniform: params, params_size bytes
 *   1 read-only storage: the frame sent (R'G'B' input: its rows packed to
 *     source_pitch bytes, the session's format: its bit depth and alpha) or
 *     the conversion's rows
 *   2 read-only storage: luts (the same bytes while luts_serial stays)
 *   3 read-only storage: frame
 *   4 storage: the PQ R'G'B' output, dst_size bytes, rows of dst_pitch
 *   5 storage: 4 bytes (not written)
 *   6 storage: the 4:2:0 output, planes_size bytes (4 when none): Y' rows
 *     of y_pitch from 0, Cb rows of c_pitch from cb_at, Cr from cr_at (the
 *     output interleaved: Cb, Cr alternating in the rows from cb_at = cr_at)
 * and entry point "apply" over groups and, for 4:2:0 output, "yuv" over
 * yuv_groups
 */
typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_apply_state_t) */
    uint32_t layout;      /* HDRG_SHADER_LAYOUT */
    const void *params;
    const void *luts;
    const void *frame;
    const void *convert_params; /* Y'CbCr input (else NULL) */
    size_t params_size, luts_size, frame_size, convert_params_size;
    uint64_t luts_serial;
    size_t source_pitch;
    size_t dst_size, dst_pitch;
    size_t planes_size, y_pitch, c_pitch, cb_at, cr_at;
    size_t yuv_size, yuv_y_pitch, yuv_c_pitch, yuv_cb_at, yuv_cr_at;
    size_t base_size, knots_size, values_size;
    uint32_t groups[2];
    uint32_t yuv_groups[2];        /* 0 x 0: none */
    uint32_t convert_groups[3][2]; /* sites, slopes, pixels (0 x 0: none) */
} hdrg_apply_state_t;

/*
 * The app's device and queue of a GPU API (HDRG_DEVICE_*) for the filters
 * of the following conversions and sessions (device NULL: none): the
 * plugins' (filter_device), and of a kind the build has (Metal, Vulkan,
 * WebGPU) the library's own GPU filters and its core's GPU passes, in place
 * of a device of the library's own. The app keeps them alive and usable from
 * the library's and the filters' threads while the context is.
 */
HDRG_API hdrg_status_t hdrg_context_set_device(hdrg_context_t *ctx, uint32_t kind, void *device,
                                               void *queue);

/*
 * A shader of hdrg_apply_state_t in lang (HDRG_SHADER_*): module's WGSL
 * (entry not read) or entry's SPIR-V or HLSL, the library's; text
 * NUL-terminated, *size its bytes (before the NUL; NULL: not wanted). NULL:
 * none of those
 */
HDRG_API const void *hdrg_shader(uint32_t lang, uint32_t module, const char *entry, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_GPU_H */
