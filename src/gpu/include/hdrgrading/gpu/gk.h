/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_GK_H
#define HDRG_GPU_GK_H

#define HDRG_GK_WRAPS 8u /* the others' frames a part keeps wrapped: more than a job reads */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/foundation/gpu_share.h"
#include "hdrgrading/foundation/thread.h"

/*
 * The compute of a native GPU API (Metal: src/gpu/metal, Vulkan:
 * src/gpu/vulkan) as the GPU filters (src/plugins/<name>/gk_*.c) and the core
 * (src/core/gpu_core_gk.c) use it, one code for both: buffers (the GPU's
 * alone, or host memory the GPU reads and writes in place), kernels of
 * Tint's shaders of the WGSL, passes of dispatches and copies committed
 * with their ends reported on a thread of the backend's. Every object may
 * be used from any thread, and a pass keeps what it uses until the GPU is
 * through with it (freeing one meanwhile drops the caller's reference).
 */

typedef struct hdrg_gk_buffer hdrg_gk_buffer_t;
typedef struct hdrg_gk_kernel hdrg_gk_kernel_t;
typedef struct hdrg_gk_pass hdrg_gk_pass_t;

/* a committed pass's end, on a thread of the backend's: ok false when the GPU failed it */
typedef void (*hdrg_gk_done_fn_t)(void *arg, bool ok);

/* a WGSL entry point as the backends take it: Tint's MSL and SPIR-V (NULL: not built in) */
typedef struct {
    const char *entry;
    const char *msl;
    const unsigned char *spirv;
    const size_t *spirv_size;
} hdrg_gk_shader_t;

/*
 * A frame on the device the library's GPU work hands from one part to the
 * next (the GPU filters' job_device / job_set_output_device, a session's
 * frames of the core: src/core/gpu_core): its share first (what the next
 * part gets: the native buffer, its failure and completions), then the
 * producer's buffer
 */
typedef struct {
    hdrg_gpu_share_frame_t share;
    hdrg_gk_buffer_t *buffer;
} hdrg_gk_frame_t;

/* another part's frame wrapped: its native buffer as the reader's (hdrg_gk_wrap) */
typedef struct {
    uint64_t native;
    size_t size;
    hdrg_gk_buffer_t *buffer;
} hdrg_gk_wrap_t;

/* the frames of others a part read, wrapped, the latest kept (hdrg_gk_wraps_free at its end) */
typedef struct {
    hdrg_gk_wrap_t items[HDRG_GK_WRAPS];
    uint8_t next;
} hdrg_gk_wraps_t;

/*
 * A backend: create on the app's device and queue of the kind (NULL: one of
 * its own; false: none), side a second queue on the device for work that
 * should not wait for the first's (NULL, false: none); buffer of the GPU's
 * alone, host_buffer of host memory (*contents its CPU's view); wrap the
 * app's buffer of a frame of the kind (HDRG_FRAME_BUFFERS: its handle,
 * HDRG_FRAME_DMABUF: an fd as a pointer) of size bytes (0: the API's own
 * count; NULL: none, or failed); kernel the shader's entry with its uniform
 * named uniform (NULL: none) and its n storage buffers named storage, bound
 * in that order; dispatch over groups_x x groups_y workgroups of group_w x
 * group_h; commit done(arg, ok) once the GPU is through (done NULL: waits
 * and returns ok; false otherwise only when it could not commit, done not
 * called), freeing the pass
 */
typedef struct {
    uint32_t kind; /* HDRG_DEVICE_* */
    bool (*create)(const hdrg_plugin_api_t *api, void *device, void *queue, void **out);
    bool (*side)(const hdrg_plugin_api_t *api, void *device, void **out);
    void (*destroy)(void *device);
    hdrg_gk_buffer_t *(*buffer)(void *device, size_t size);
    hdrg_gk_buffer_t *(*host_buffer)(void *device, size_t size, uint8_t **contents);
    hdrg_gk_buffer_t *(*wrap)(void *device, uint32_t kind, void *handle, size_t size);
    void *(*buffer_object)(const hdrg_gk_buffer_t *buffer);
    size_t (*buffer_size)(const hdrg_gk_buffer_t *buffer);
    void (*buffer_free)(hdrg_gk_buffer_t *buffer);
    hdrg_gk_kernel_t *(*kernel)(void *device, const hdrg_gk_shader_t *shader, const char *uniform,
                                const char *const *storage, uint8_t n);
    void (*kernel_free)(hdrg_gk_kernel_t *kernel);
    hdrg_gk_pass_t *(*begin)(void *device);
    void (*dispatch)(hdrg_gk_pass_t *pass, const hdrg_gk_kernel_t *kernel, const void *uniform,
                     size_t uniform_size, hdrg_gk_buffer_t *const *buffers, uint32_t groups_x,
                     uint32_t groups_y, uint32_t group_w, uint32_t group_h);
    void (*copy_buffer)(hdrg_gk_pass_t *pass, hdrg_gk_buffer_t *src, hdrg_gk_buffer_t *dst,
                        size_t size);
    bool (*commit)(hdrg_gk_pass_t *pass, hdrg_gk_done_fn_t done, void *arg);
} hdrg_gk_ops_t;

/* a device of a backend */
typedef struct {
    const hdrg_gk_ops_t *ops;
    void *device;
} hdrg_gk_t;

static inline hdrg_gk_buffer_t *hdrg_gk_buffer(const hdrg_gk_t *gk, size_t size)
{
    return gk->ops->buffer(gk->device, size);
}

static inline hdrg_gk_buffer_t *hdrg_gk_host_buffer(const hdrg_gk_t *gk, size_t size,
                                                    uint8_t **contents)
{
    return gk->ops->host_buffer(gk->device, size, contents);
}

static inline void hdrg_gk_buffer_free(const hdrg_gk_t *gk, hdrg_gk_buffer_t *buffer)
{
    if (buffer != NULL) {
        gk->ops->buffer_free(buffer);
    }
}

static inline hdrg_gk_kernel_t *hdrg_gk_kernel(const hdrg_gk_t *gk, const hdrg_gk_shader_t *shader,
                                               const char *uniform, const char *const *storage,
                                               uint8_t n)
{
    return gk->ops->kernel(gk->device, shader, uniform, storage, n);
}

static inline void hdrg_gk_kernel_free(const hdrg_gk_t *gk, hdrg_gk_kernel_t *kernel)
{
    if (kernel != NULL) {
        gk->ops->kernel_free(kernel);
    }
}

static inline hdrg_gk_pass_t *hdrg_gk_begin(const hdrg_gk_t *gk)
{
    return gk->ops->begin(gk->device);
}

static inline void hdrg_gk_dispatch(const hdrg_gk_t *gk, hdrg_gk_pass_t *pass,
                                    const hdrg_gk_kernel_t *kernel, const void *uniform,
                                    size_t uniform_size, hdrg_gk_buffer_t *const *buffers,
                                    uint32_t groups_x, uint32_t groups_y, uint32_t group_w,
                                    uint32_t group_h)
{
    gk->ops->dispatch(pass, kernel, uniform, uniform_size, buffers, groups_x, groups_y, group_w,
                      group_h);
}

static inline void hdrg_gk_copy_buffer(const hdrg_gk_t *gk, hdrg_gk_pass_t *pass,
                                       hdrg_gk_buffer_t *src, hdrg_gk_buffer_t *dst, size_t size)
{
    gk->ops->copy_buffer(pass, src, dst, size);
}

static inline bool hdrg_gk_commit(const hdrg_gk_t *gk, hdrg_gk_pass_t *pass, hdrg_gk_done_fn_t done,
                                  void *arg)
{
    return gk->ops->commit(pass, done, arg);
}

/*
 * another part's frame as a buffer of gk (its native buffer wrapped, kept
 * in wraps for the jobs after: a frame comes again); NULL: not one of the
 * kind, or failed
 */
static inline hdrg_gk_buffer_t *hdrg_gk_wrap(const hdrg_gk_t *gk, hdrg_gk_wraps_t *wraps,
                                             const hdrg_gpu_share_frame_t *frame)
{
    hdrg_gk_wrap_t *item;
    uint8_t i;

    if (frame->kind != gk->ops->kind || frame->buffer == 0u) {
        return NULL;
    }

    for (i = 0; i < HDRG_GK_WRAPS; i++) {
        item = &wraps->items[i];

        if (item->buffer != NULL && item->native == frame->buffer && item->size == frame->size) {
            return item->buffer;
        }
    }

    /* the oldest replaced (a pass in flight keeps its own reference) */
    item = &wraps->items[wraps->next];
    wraps->next = (uint8_t)((wraps->next + 1u) % HDRG_GK_WRAPS);
    hdrg_gk_buffer_free(gk, item->buffer);
    item->buffer = gk->ops->wrap(gk->device, HDRG_FRAME_BUFFERS, (void *)(uintptr_t)frame->buffer,
                                 frame->size);
    item->native = frame->buffer;
    item->size = frame->size;

    return item->buffer;
}

static inline void hdrg_gk_wraps_free(const hdrg_gk_t *gk, hdrg_gk_wraps_t *wraps)
{
    uint8_t i;

    for (i = 0; i < HDRG_GK_WRAPS; i++) {
        hdrg_gk_buffer_free(gk, wraps->items[i].buffer);
        wraps->items[i].buffer = NULL;
    }

    wraps->next = 0;
}

/* frame's buffer: its share's native handle and size, the kind's */
static inline void hdrg_gk_frame_set(const hdrg_gk_t *gk, hdrg_gk_frame_t *frame,
                                     hdrg_gk_buffer_t *buffer)
{
    frame->buffer = buffer;
    frame->share.kind = gk->ops->kind;
    frame->share.buffer =
        (buffer != NULL) ? (uint64_t)(uintptr_t)gk->ops->buffer_object(buffer) : 0u;
    frame->share.size = (buffer != NULL) ? gk->ops->buffer_size(buffer) : 0u;
}

#if defined(HDRG_WITH_METAL)
extern const hdrg_gk_ops_t hdrg_gk_metal;
#endif
#if defined(HDRG_WITH_VULKAN)
extern const hdrg_gk_ops_t hdrg_gk_vulkan;
#endif

#endif /* HDRG_GPU_GK_H */
