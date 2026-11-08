/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_GPU_CORE_H
#define HDRG_CORE_GPU_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hdrgrading/filter.h>
#include <hdrgrading/video.h>

#include "hdrgrading/gpu/gk.h"
#include "../host/host.h"
#include "exec.h"

#define HDRG_GC_SLOTS                                                                              \
    40u /* the buffers a core holds: the programs' (0..11, 15, 32..), the app's (12..14), frames   \
           (16..31) */
#define HDRG_GC_APP0     12u /* the app's buffers' slots (hdrg_gpu_core_import): its planes */
#define HDRG_GC_FRAME0   16u /* the first frame's slot (hdrg_gpu_core_frame) */
#define HDRG_GC_FRAMES   16u
#define HDRG_GC_ANALYZE0 32u /* the analysis's (src/core/gpu_analyze.c) */
#define HDRG_GC_BINDINGS 7u  /* a program's storage buffers at most (WebGPU's default limit: 8) */
#define HDRG_GC_GROUP    64u /* the shaders' workgroups: 64 x 1 */

/* the core's programs: a WGSL module of src/core/wgsl and its entry points */
#define HDRG_GC_APPLY    0u /* apply.wgsl: apply, yuv */
#define HDRG_GC_CONVERT  1u /* convert.wgsl: sites, slopes, pixels */
#define HDRG_GC_GATHER   2u /* gather.wgsl: proxy, tiles */
#define HDRG_GC_COPY     3u /* io.wgsl: copy_rows */
#define HDRG_GC_ANALYZE  4u /* analyze.wgsl: edges, cells_of, hist, moments_of */
#define HDRG_GC_PROGRAMS 5u

/*
 * an entry point: its name and Tint's MSL (Metal) and SPIR-V (Vulkan) of it,
 * and the program's buffers it uses, in their order
 */
typedef struct {
    hdrg_gk_shader_t shader;
    uint8_t buffers[HDRG_GC_BINDINGS];
    uint8_t count;
} hdrg_gc_entry_t;

/*
 * A program: the WGSL module (WebGPU), its uniform of params_size bytes at
 * binding 0 and its storage buffers at bindings 1.. (names: the WGSL's;
 * bit i of writes: buffer i is read_write), its entry points
 */
typedef struct {
    const char *wgsl;
    const char *const *names;
    const hdrg_gc_entry_t *entries;
    uint32_t params_size;
    uint32_t writes;
    uint8_t buffer_count;
    uint8_t entry_count;
} hdrg_gc_program_t;

/* the copy's uniform (Params of src/core/wgsl/io.wgsl), 32 bytes: words, offsets, pitches */
typedef struct {
    uint32_t rows, words, src_at, src_pitch, dst_at, dst_pitch, pad0, pad1;
} hdrg_gc_copy_params_t;

/* a surface plane's texels (the surface op): their bytes and components */
#define HDRG_GC_TEXEL_R8   1u /* 1 byte */
#define HDRG_GC_TEXEL_R16  2u /* 2 bytes */
#define HDRG_GC_TEXEL_RG8  3u /* 2 bytes: Cb, Cr */
#define HDRG_GC_TEXEL_RG16 4u /* 4 bytes: Cb, Cr */

/* an entry point of the program over groups_x x groups_y workgroups of HDRG_GC_GROUP x 1 */
typedef struct {
    uint32_t groups_x, groups_y;
    uint8_t entry;
} hdrg_gc_dispatch_t;

/*
 * A backend (Metal, Vulkan, WebGPU): open on the app's device and queue (NULL: one
 * of its own; false: none); buffer slot of at least size bytes (kept while
 * large enough) and its CPU view in *view, written before run and read
 * after it (view NULL: the GPU's alone, never uploaded nor read back; false:
 * failed); run builds the program on its first run, uploads the
 * slots of the mask in (bit s: slot s), dispatches in order with buffer i
 * of the program bound to slots[i], waits, and reads back the slots of the
 * mask out; frame: slot's buffer as a frame on the device
 * (hdrg_gpu_share_frame_t of foundation/gpu_share.h, a GPU filter's
 * job_device input; NULL: none); take: a GPU filter's output frame (its
 * native buffer) copied into slot on the device, after the work queued
 * before (false: failed);
 * import: the app's buffer of a frame of the kind (HDRG_FRAME_BUFFERS:
 * id<MTLBuffer>, VkBuffer, WGPUBuffer; HDRG_FRAME_DMABUF: an fd) of bytes
 * (0: the API's own count) as slot, the GPU's alone, with a reference of the
 * slot's (*size: its bytes; false: failed);
 * surface (NULL: none): the top-left texels x rows of plane of an IOSurface
 * (texels of texel, HDRG_GC_TEXEL_*) copied on the GPU into slot (rows of
 * pitch bytes from byte at; in) or out of it, after the work queued before
 * and waited for (HDRG_ERR_UNSUPPORTED: a surface or device the backend
 * cannot take); ahb (NULL: none) the same of an Android AHardwareBuffer
 * (HDRG_ERR_INVALID_ARG: its format not the texels', a plane smaller than
 * the rows)
 */
typedef struct {
    uint32_t kind; /* HDRG_DEVICE_* */
    bool (*open)(const hdrg_plugin_api_t *api, void *device, void *queue, void **state);
    void (*close)(void *state);
    bool (*buffer)(void *state, uint8_t slot, size_t size, uint8_t **view);
    bool (*run)(void *state, uint8_t program, const void *params, const uint8_t *slots,
                const hdrg_gc_dispatch_t *dispatches, uint8_t count, uint64_t in, uint64_t out);
    void *(*frame)(void *state, uint8_t slot);
    bool (*take)(void *state, const void *frame, uint8_t slot, size_t size);
    bool (*import)(void *state, uint8_t slot, uint32_t kind, void *handle, size_t bytes,
                   size_t *size);
    hdrg_status_t (*surface)(void *state, void *surface, uint8_t plane, uint8_t texel, uint8_t slot,
                             size_t at, size_t pitch, uint32_t texels, uint32_t rows, bool in);
    hdrg_status_t (*ahb)(void *state, void *buffer, uint8_t plane, uint8_t texel, uint8_t slot,
                         size_t at, size_t pitch, uint32_t texels, uint32_t rows, bool in);
} hdrg_gc_ops_t;

/* the core's GPU (hdrg_gpu_core_create) */
typedef struct hdrg_gpu_core {
    const hdrg_plugin_api_t *api;
    const hdrg_gc_ops_t *ops;
    const float *eotf; /* the apply's tables in its LUT buffer (NULL: none yet, gpu_apply.c) */
    uint32_t entries;
    void *state;
} hdrg_gpu_core_t;

/* the bytes of a texel (HDRG_GC_TEXEL_*) */
static inline uint8_t hdrg_gc_texel_bytes(uint8_t texel)
{
    if (texel == HDRG_GC_TEXEL_R8) {
        return 1u;
    }

    return (texel == HDRG_GC_TEXEL_RG16) ? 4u : 2u;
}

/* a Y'CbCr plane's texels of bytes a sample on a surface: Cb, Cr together when interleaved */
static inline uint8_t hdrg_gc_texel(uint32_t bytes, bool interleaved)
{
    if (interleaved) {
        return (bytes == 1u) ? HDRG_GC_TEXEL_RG8 : HDRG_GC_TEXEL_RG16;
    }

    return (bytes == 1u) ? HDRG_GC_TEXEL_R8 : HDRG_GC_TEXEL_R16;
}

#if defined(HDRG_WITH_METAL)
extern const hdrg_gc_ops_t hdrg_gc_metal;
#endif
#if defined(HDRG_WITH_VULKAN)
extern const hdrg_gc_ops_t hdrg_gc_vulkan;
#endif
#if defined(HDRG_WITH_WEBGPU)
extern const hdrg_gc_ops_t hdrg_gc_webgpu;
#endif
/* the programs, by HDRG_GC_* */
extern const hdrg_gc_program_t hdrg_gc_programs[HDRG_GC_PROGRAMS];

/*
 * The core's GPU work for a session: Metal, else
 * Vulkan, else WebGPU (kind: that one alone; 0: any), on the context's device of the
 * kind, the one its filters use (hdrg_host_device). HDRG_ERR_UNSUPPORTED:
 * no GPU (the CPU's passes stay)
 */
extern hdrg_status_t hdrg_gpu_core_create(const hdrg_plugin_host_t *host, uint32_t kind,
                                          hdrg_gpu_core_t **out);
extern void hdrg_gpu_core_destroy(hdrg_gpu_core_t *gpu);
/* the backend's HDRG_DEVICE_* (the reports) */
extern uint32_t hdrg_gpu_core_device(const hdrg_gpu_core_t *gpu);
/* the ops' buffer and run */
extern bool hdrg_gpu_core_buffer(hdrg_gpu_core_t *gpu, uint8_t slot, size_t size, uint8_t **view);
extern bool hdrg_gpu_core_run(hdrg_gpu_core_t *gpu, uint8_t program, const void *params,
                              const uint8_t *slots, const hdrg_gc_dispatch_t *dispatches,
                              uint8_t count, uint64_t in, uint64_t out);
/* the ops' frame, take and import */
extern void *hdrg_gpu_core_frame(hdrg_gpu_core_t *gpu, uint8_t slot);
extern bool hdrg_gpu_core_take(hdrg_gpu_core_t *gpu, const void *frame, uint8_t slot, size_t size);
extern bool hdrg_gpu_core_import(hdrg_gpu_core_t *gpu, uint8_t slot, uint32_t kind, void *handle,
                                 size_t bytes, size_t *size);
/*
 * Plane p of the app's frame on the GPU (video.h; Y'CbCr interleaved: Y and
 * CbCr) and rows of bytes of the core's slot (rows of pitch words from word
 * at), copied on the GPU into the slot (in) or out of it, texels of texel
 * (HDRG_GC_TEXEL_*: a surface's planes). HDRG_ERR_INVALID_ARG for buffers
 * not in whole words, with a pitch under the bytes or past the buffer's
 * end, a surface plane smaller than the rows; HDRG_ERR_UNSUPPORTED for a
 * kind the build or the backend does not take
 */
extern hdrg_status_t hdrg_gpu_core_plane(hdrg_gpu_core_t *gpu, const hdrg_device_frame_t *frame,
                                         uint8_t p, uint8_t slot, uint32_t at, uint32_t pitch,
                                         uint32_t bytes, uint32_t rows, uint8_t texel, bool in);
/* rows bytes long between two layouts of rows (the copies in and out), on exec */
extern void hdrg_gpu_core_copy(const hdrg_exec_t *exec, const uint8_t *src, size_t src_stride,
                               uint8_t *dst, size_t dst_stride, size_t bytes, uint32_t rows);

#endif /* HDRG_CORE_GPU_CORE_H */
