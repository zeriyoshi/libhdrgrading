/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_GPU_METAL_H
#define HDRG_GPU_METAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/filter.h"

/*
 * A Metal device (HDRG_WITH_METAL) for the
 * Metal filters, in plain C over src/gpu/metal.m: the app's device and
 * queue (HDRG_DEVICE_METAL) or the system's default device with a queue of
 * its own. Unlike WebGPU's (src/gpu/webgpu) every object here may be used from any
 * thread: a pass is encoded on its caller's thread, its end reported on
 * Metal's (hdrg_metal_commit). The kernels are Tint's MSL of the WGSL
 * (src/plugins/<name>/msl/, scripts/gen_shaders.sh), compiled in the safe math mode.
 */
#define HDRG_METAL_BUFFERS 8u /* a kernel's storage buffers at most */

/* the texture formats (hdrg_metal_texture, hdrg_metal_surface_texture) */
#define HDRG_METAL_RGBA16  UINT32_C(1) /* RGBA16Unorm */
#define HDRG_METAL_RGBA16F UINT32_C(2) /* RGBA16Float */
#define HDRG_METAL_R8U     UINT32_C(3) /* R8Uint: a surface plane's 8-bit samples */
#define HDRG_METAL_R16U    UINT32_C(4) /* R16Uint */
#define HDRG_METAL_RG8U    UINT32_C(5) /* RG8Uint: Cb, Cr */
#define HDRG_METAL_RG16U   UINT32_C(6) /* RG16Uint */

typedef struct hdrg_metal hdrg_metal_t;
typedef struct hdrg_metal_buffer hdrg_metal_buffer_t;
typedef struct hdrg_metal_texture hdrg_metal_texture_t;
typedef struct hdrg_metal_kernel hdrg_metal_kernel_t;
typedef struct hdrg_metal_pass hdrg_metal_pass_t;
typedef struct hdrg_metal_spatial hdrg_metal_spatial_t;

/* a committed pass's end, on a thread of Metal's: ok false when the GPU failed it */
typedef void (*hdrg_metal_done_fn_t)(void *arg, bool ok);

/*
 * device NULL: the system's default device with a queue of its own (false:
 * none); otherwise the app's id<MTLDevice> and its id<MTLCommandQueue>
 * (NULL: one of its own on the device), both retained, or one an hdrg_metal_t has with the same
 * table (that one, shared). api: the plugin table whose allocator it and its objects use until
 * destroyed by its last holder (destroy waits for the host buffers' memory to go)
 */
extern bool hdrg_metal_create(const hdrg_plugin_api_t *api, void *device, void *queue,
                              hdrg_metal_t **out);
extern void hdrg_metal_destroy(hdrg_metal_t *metal);
/* its id<MTLDevice> and id<MTLCommandQueue> (the metal's references: retain them to keep them) */
extern void *hdrg_metal_device_object(const hdrg_metal_t *metal);
extern void *hdrg_metal_queue_object(const hdrg_metal_t *metal);
/* the alignment and length unit of host buffers: the VM page, at least 16 KiB */
extern size_t hdrg_metal_page(void);
/* a buffer of size bytes of the GPU's alone (private storage); NULL: failed */
extern hdrg_metal_buffer_t *hdrg_metal_buffer(hdrg_metal_t *metal, size_t size);
/*
 * host memory of the buffer's own, page aligned and rounded up to pages,
 * that the GPU reads and writes in place (newBufferWithBytesNoCopy, shared
 * storage): *contents, the CPU's view of it; NULL: failed
 */
extern hdrg_metal_buffer_t *hdrg_metal_host_buffer(hdrg_metal_t *metal, size_t size,
                                                   uint8_t **contents);
/* the app's id<MTLBuffer> of the device, retained (NULL: none, or failed) */
extern hdrg_metal_buffer_t *hdrg_metal_buffer_wrap(hdrg_metal_t *metal, void *object);
/* its id<MTLBuffer> (the buffer's reference) and bytes */
extern void *hdrg_metal_buffer_object(const hdrg_metal_buffer_t *buffer);
extern size_t hdrg_metal_buffer_size(const hdrg_metal_buffer_t *buffer);
extern void hdrg_metal_buffer_free(hdrg_metal_buffer_t *buffer);
/*
 * a 2D texture of format (HDRG_METAL_*) and size; private storage (the GPU's
 * alone), read and written by shaders and render passes. NULL: failed
 */
extern hdrg_metal_texture_t *hdrg_metal_texture(hdrg_metal_t *metal, uint32_t format,
                                                uint32_t width, uint32_t height);
/*
 * plane of an IOSurface (IOSurfaceRef: an app's pixel buffer) as a texture
 * of format (its texels the plane's elements) and the plane's size, read and
 * written by blits in place. NULL: failed
 */
extern hdrg_metal_texture_t *hdrg_metal_surface_texture(hdrg_metal_t *metal, void *surface,
                                                        uint32_t plane, uint32_t format);
extern void hdrg_metal_texture_free(hdrg_metal_texture_t *texture);
/*
 * the compute pipeline of entry in msl (Tint's MSL of a WGSL entry point):
 * its uniform named uniform (NULL: none), set as bytes, and its n storage
 * buffers named storage, bound in that order. Tint numbers an entry point's
 * buffers in the order it uses them and clamps the storage arrays to sizes
 * it reads from its immediate data at buffer 30, ranked by those numbers:
 * both come from the pipeline's reflection. NULL: it does not compile, or
 * the names are not the entry point's
 */
extern hdrg_metal_kernel_t *hdrg_metal_kernel(hdrg_metal_t *metal, const char *msl,
                                              const char *entry, const char *uniform,
                                              const char *const *storage, uint8_t n);
extern void hdrg_metal_kernel_free(hdrg_metal_kernel_t *kernel);
/* a command buffer of the queue to encode; NULL: failed */
extern hdrg_metal_pass_t *hdrg_metal_begin(hdrg_metal_t *metal);
/*
 * kernel over groups_x x groups_y threadgroups of group_w x group_h threads
 * (the WGSL's workgroup size): the uniform's bytes and the storage buffers in
 * the kernel's order (each read as a whole: Tint's array lengths)
 */
extern void hdrg_metal_dispatch(hdrg_metal_pass_t *pass, const hdrg_metal_kernel_t *kernel,
                                const void *uniform, size_t uniform_size,
                                hdrg_metal_buffer_t *const *buffers, uint32_t groups_x,
                                uint32_t groups_y, uint32_t group_w, uint32_t group_h);
/*
 * a copy between a buffer of rows of pitch bytes (a multiple of 4: RGBA16
 * frames of 8 bytes a pixel) and a texture, the buffer's rows first: to the
 * texture when upload, from it otherwise
 */
extern void hdrg_metal_copy(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *buffer, size_t pitch,
                            hdrg_metal_texture_t *texture, bool upload);
/*
 * a copy between rows of pitch bytes from byte offset of a buffer and the
 * texture's top-left width x height texels: to the texture when upload,
 * from it otherwise
 */
extern void hdrg_metal_copy_region(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *buffer,
                                   size_t offset, size_t pitch, hdrg_metal_texture_t *texture,
                                   uint32_t width, uint32_t height, bool upload);
/* a copy of size bytes from the start of src to the start of dst */
extern void hdrg_metal_copy_buffer(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *src,
                                   hdrg_metal_buffer_t *dst, size_t size);
/*
 * ends the pass and commits it: done(arg, ok) once the GPU is through (done
 * NULL: the call waits and returns ok; false otherwise only when it could
 * not commit, and done is not called). Frees the pass
 */
extern bool hdrg_metal_commit(hdrg_metal_pass_t *pass, hdrg_metal_done_fn_t done, void *arg);
/*
 * MetalFX's spatial scaler (macOS 13, iOS 16) from in_w x in_h to out_w x
 * out_h textures of format, its color perceptual (gamma-encoded values in
 * [0, 1]); NULL: the device has none
 */
extern hdrg_metal_spatial_t *hdrg_metal_spatial(hdrg_metal_t *metal, uint32_t format, uint32_t in_w,
                                                uint32_t in_h, uint32_t out_w, uint32_t out_h);
extern void hdrg_metal_spatial_free(hdrg_metal_spatial_t *spatial);
/* the scaler from in to out, encoded into the pass */
extern void hdrg_metal_spatial_encode(hdrg_metal_pass_t *pass, hdrg_metal_spatial_t *spatial,
                                      hdrg_metal_texture_t *in, hdrg_metal_texture_t *out);

#endif /* HDRG_GPU_METAL_H */
