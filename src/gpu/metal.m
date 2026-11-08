/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <IOSurface/IOSurfaceRef.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdrgrading/gpu/metal.h"

#include "hdrgrading/foundation/thread.h"

#if !__has_feature(objc_arc)
#error "src/gpu/metal.m is built with ARC (-fobjc-arc)"
#endif

#define HDRG_METAL_IMMEDIATE 30u    /* Tint's immediate data: the storage arrays' sizes ... */
#define HDRG_METAL_SIZES     0x800u /* ... from this offset of it */
#define HDRG_METAL_PAGE_MIN  16384u /* Apple silicon's page (a Rosetta process reports 4 KiB) */
#define HDRG_METAL_NONE      UINT32_MAX

/* the Objective-C objects: retained references (CFBridgingRetain) in plain C structs */
/*
 * api: the allocator of everything here. The host buffers' memory goes when Metal lets the buffer
 * go (its deallocator, perhaps after a command buffer's last reference): destroy waits for them all
 * (outstanding), the allocator being the context's
 */
struct hdrg_metal {
    const hdrg_plugin_api_t *api;
    hdrg_metal_t *next; /* the shared ones (hdrg_metal_registry_lock) ... */
    uint32_t refs;      /* ... and their holders */
    void *device;       /* id<MTLDevice> */
    void *queue;        /* id<MTLCommandQueue> */
    hdrg_mutex_t lock;
    hdrg_cond_t idle;
    uint32_t outstanding; /* lock */
};

struct hdrg_metal_buffer {
    const hdrg_plugin_api_t *api;
    void *object; /* id<MTLBuffer> */
    size_t size;
};

struct hdrg_metal_texture {
    const hdrg_plugin_api_t *api;
    void *object; /* id<MTLTexture> */
    uint32_t width, height;
};

struct hdrg_metal_kernel {
    const hdrg_plugin_api_t *api;
    void *pipeline;                       /* id<MTLComputePipelineState> */
    uint32_t uniform;                     /* its buffer index (HDRG_METAL_NONE: none) */
    uint32_t buffers[HDRG_METAL_BUFFERS]; /* the storage buffers' indices ... */
    uint8_t slots[HDRG_METAL_BUFFERS];    /* ... and their sizes' places in the immediate data */
    uint8_t count;
    size_t immediate;  /* the immediate data's size (0: none) */
    size_t local;      /* the workgroup memory's size (0: none) ... */
    uint32_t local_at; /* ... and its threadgroup index */
};

struct hdrg_metal_pass {
    const hdrg_plugin_api_t *api;
    void *commands; /* id<MTLCommandBuffer> */
    void *compute;  /* id<MTLComputeCommandEncoder> open (NULL: none) */
};

struct hdrg_metal_spatial {
    const hdrg_plugin_api_t *api;
    void *scaler; /* id<MTLFXSpatialScaler> */
};

/*
 * the commits so far: a completion handler reads it (acquire) after its
 * committing thread added to it (release), so what that thread wrote for the
 * handler is ordered before it in a way ThreadSanitizer sees (Metal's own
 * ordering is in code it does not instrument)
 */
static _Atomic uint32_t hdrg_metal_commits;

static inline id hdrg_metal_object(void *object)
{
    return (__bridge id)object;
}

static void hdrg_metal_release(void **object)
{
    if (*object != NULL) {
        CFRelease(*object);
        *object = NULL;
    }
}

/*
 * every hdrg_metal_t, by its device, queue and table: one object for a queue, whoever asks for it
 * with the same table (the filters and the core of a context)
 */
static hdrg_atomic_u32_t hdrg_metal_registry_lock;
static hdrg_metal_t *hdrg_metal_registry;

static void hdrg_metal_lock_registry(void)
{
    uint32_t expected;

    do {
        expected = 0;
    } while (!hdrg_atomic_u32_cas_acq_rel(&hdrg_metal_registry_lock, &expected, 1u));
}

static void hdrg_metal_unlock_registry(void)
{
    hdrg_atomic_u32_store_release(&hdrg_metal_registry_lock, 0u);
}

/* a host buffer's memory gone (Metal's deallocator, any thread): destroy may go on when it is the
 * last */
static void hdrg_metal_host_release(hdrg_metal_t *metal, void *bytes)
{
    metal->api->mem_aligned_free(metal->api, bytes);

    hdrg_mutex_lock(&metal->lock);
    metal->outstanding--;

    if (metal->outstanding == 0) {
        hdrg_cond_broadcast(&metal->idle);
    }

    hdrg_mutex_unlock(&metal->lock);
}

static MTLPixelFormat hdrg_metal_format(uint32_t format)
{
    switch (format) {
    case HDRG_METAL_RGBA16:
        return MTLPixelFormatRGBA16Unorm;
    case HDRG_METAL_RGBA16F:
        return MTLPixelFormatRGBA16Float;
    case HDRG_METAL_R8U:
        return MTLPixelFormatR8Uint;
    case HDRG_METAL_R16U:
        return MTLPixelFormatR16Uint;
    case HDRG_METAL_RG8U:
        return MTLPixelFormatRG8Uint;
    case HDRG_METAL_RG16U:
        return MTLPixelFormatRG16Uint;
    default:
        return MTLPixelFormatInvalid;
    }
}

/* the open compute encoder of the pass, made when none */
static id<MTLComputeCommandEncoder> hdrg_metal_compute(hdrg_metal_pass_t *pass)
{
    id<MTLCommandBuffer> commands = hdrg_metal_object(pass->commands);
    id<MTLComputeCommandEncoder> encoder;

    if (pass->compute != NULL) {
        return hdrg_metal_object(pass->compute);
    }

    encoder = [commands computeCommandEncoder];
    pass->compute = (encoder != nil) ? (void *)CFBridgingRetain(encoder) : NULL;

    return encoder;
}

/* the pass's compute encoder ended: other encoders may follow */
static void hdrg_metal_end_compute(hdrg_metal_pass_t *pass)
{
    id<MTLComputeCommandEncoder> encoder;

    if (pass->compute != NULL) {
        encoder = CFBridgingRelease(pass->compute);
        pass->compute = NULL;
        [encoder endEncoding];
    }
}

extern bool hdrg_metal_create(const hdrg_plugin_api_t *api, void *device, void *queue,
                              hdrg_metal_t **out)
{
    hdrg_metal_t *metal;
    id<MTLDevice> chosen;
    id<MTLCommandQueue> commands;

    *out = NULL;

    /* one already on the device and queue for the table: shared */
    if (device != NULL && queue != NULL) {
        hdrg_metal_lock_registry();

        for (metal = hdrg_metal_registry; metal != NULL; metal = metal->next) {
            if (metal->device == device && metal->queue == queue && metal->api == api) {
                metal->refs++;

                break;
            }
        }

        hdrg_metal_unlock_registry();

        if (metal != NULL) {
            *out = metal;

            return true;
        }
    }

    @autoreleasepool {
        chosen = (device != NULL) ? (__bridge id<MTLDevice>)device : MTLCreateSystemDefaultDevice();
        if (chosen == nil) {
            return false;
        }

        commands = (device != NULL && queue != NULL) ? (__bridge id<MTLCommandQueue>)queue
                                                     : [chosen newCommandQueue];
        if (commands == nil) {
            return false;
        }

        metal = (hdrg_metal_t *)api->mem_calloc(api, 1, sizeof(*metal));
        if (metal == NULL) {
            return false;
        }

        if (!hdrg_mutex_init(&metal->lock)) {
            api->mem_free(api, metal);

            return false;
        }

        if (!hdrg_cond_init(&metal->idle)) {
            hdrg_mutex_destroy(&metal->lock);
            api->mem_free(api, metal);

            return false;
        }

        metal->api = api;
        metal->refs = 1;
        metal->device = (void *)CFBridgingRetain(chosen);
        metal->queue = (void *)CFBridgingRetain(commands);
    }

    hdrg_metal_lock_registry();
    metal->next = hdrg_metal_registry;
    hdrg_metal_registry = metal;
    hdrg_metal_unlock_registry();
    *out = metal;

    return true;
}

extern void hdrg_metal_destroy(hdrg_metal_t *metal)
{
    hdrg_metal_t **at;
    bool last;

    if (metal == NULL) {
        return;
    }

    /* the last holder's: out of the registry */
    hdrg_metal_lock_registry();
    metal->refs--;
    last = metal->refs == 0;

    for (at = &hdrg_metal_registry; last && *at != NULL; at = &(*at)->next) {
        if (*at == metal) {
            *at = metal->next;

            break;
        }
    }

    hdrg_metal_unlock_registry();

    if (!last) {
        return;
    }

    hdrg_mutex_lock(&metal->lock);

    while (metal->outstanding > 0) {
        hdrg_cond_wait(&metal->idle, &metal->lock);
    }

    hdrg_mutex_unlock(&metal->lock);

    hdrg_metal_release(&metal->queue);
    hdrg_metal_release(&metal->device);
    hdrg_cond_destroy(&metal->idle);
    hdrg_mutex_destroy(&metal->lock);
    metal->api->mem_free(metal->api, metal);
}

extern void *hdrg_metal_device_object(const hdrg_metal_t *metal)
{
    return metal->device;
}

extern void *hdrg_metal_queue_object(const hdrg_metal_t *metal)
{
    return metal->queue;
}

extern size_t hdrg_metal_page(void)
{
    long page;

    page = sysconf(_SC_PAGESIZE);

    return (page > (long)HDRG_METAL_PAGE_MIN) ? (size_t)page : HDRG_METAL_PAGE_MIN;
}

extern hdrg_metal_buffer_t *hdrg_metal_buffer(hdrg_metal_t *metal, size_t size)
{
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    hdrg_metal_buffer_t *buffer;
    id<MTLBuffer> object;

    if (size == 0 || size > device.maxBufferLength) {
        return NULL;
    }

    buffer = (hdrg_metal_buffer_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->api = metal->api;

    @autoreleasepool {
        object = [device newBufferWithLength:size options:MTLResourceStorageModePrivate];
        buffer->object = (object != nil) ? (void *)CFBridgingRetain(object) : NULL;
    }

    if (buffer->object == NULL) {
        metal->api->mem_free(metal->api, buffer);

        return NULL;
    }

    buffer->size = size;

    return buffer;
}

extern hdrg_metal_buffer_t *hdrg_metal_host_buffer(hdrg_metal_t *metal, size_t size,
                                                   uint8_t **contents)
{
    const size_t page = hdrg_metal_page();
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    hdrg_metal_buffer_t *buffer;
    id<MTLBuffer> object;
    size_t length;
    void *bytes;

    *contents = NULL;

    if (size == 0 || size > SIZE_MAX - page) {
        return NULL;
    }

    length = (size + page - 1u) / page * page;

    if (length > device.maxBufferLength) {
        return NULL;
    }

    bytes = metal->api->mem_aligned_alloc(metal->api, length, page);
    buffer = (hdrg_metal_buffer_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*buffer));
    if (bytes == NULL || buffer == NULL) {
        metal->api->mem_aligned_free(metal->api, bytes);
        metal->api->mem_free(metal->api, buffer);

        return NULL;
    }

    buffer->api = metal->api;

    hdrg_mutex_lock(&metal->lock);
    metal->outstanding++;
    hdrg_mutex_unlock(&metal->lock);

    /* the buffer frees the memory when it goes (its last reference may be a command buffer's) */
    @autoreleasepool {
        object = [device newBufferWithBytesNoCopy:bytes
                                           length:length
                                          options:MTLResourceStorageModeShared
                                      deallocator:^(void *pointer, NSUInteger unused) {
                                        (void)unused;
                                        hdrg_metal_host_release(metal, pointer);
                                      }];
        buffer->object = (object != nil) ? (void *)CFBridgingRetain(object) : NULL;
    }

    if (buffer->object == NULL) {
        hdrg_metal_host_release(metal, bytes);
        metal->api->mem_free(metal->api, buffer);

        return NULL;
    }

    buffer->size = length;
    *contents = (uint8_t *)bytes;

    return buffer;
}

extern hdrg_metal_buffer_t *hdrg_metal_buffer_wrap(hdrg_metal_t *metal, void *object)
{
    id<MTLBuffer> app = (__bridge id<MTLBuffer>)object;
    hdrg_metal_buffer_t *buffer;

    if (app == nil) {
        return NULL;
    }

    buffer = (hdrg_metal_buffer_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*buffer));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->api = metal->api;
    buffer->object = (void *)CFBridgingRetain(app);
    buffer->size = (size_t)app.length;

    return buffer;
}

extern void *hdrg_metal_buffer_object(const hdrg_metal_buffer_t *buffer)
{
    return buffer->object;
}

extern size_t hdrg_metal_buffer_size(const hdrg_metal_buffer_t *buffer)
{
    return buffer->size;
}

extern void hdrg_metal_buffer_free(hdrg_metal_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    hdrg_metal_release(&buffer->object);
    buffer->api->mem_free(buffer->api, buffer);
}

extern hdrg_metal_texture_t *hdrg_metal_texture(hdrg_metal_t *metal, uint32_t format,
                                                uint32_t width, uint32_t height)
{
    const MTLPixelFormat pixel = hdrg_metal_format(format);
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    MTLTextureDescriptor *desc;
    hdrg_metal_texture_t *texture;
    id<MTLTexture> object;

    if (pixel == MTLPixelFormatInvalid || width == 0 || height == 0) {
        return NULL;
    }

    texture = (hdrg_metal_texture_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*texture));
    if (texture == NULL) {
        return NULL;
    }

    texture->api = metal->api;

    @autoreleasepool {
        desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel
                                                                  width:width
                                                                 height:height
                                                              mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage =
            MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        object = [device newTextureWithDescriptor:desc];
        texture->object = (object != nil) ? (void *)CFBridgingRetain(object) : NULL;
    }

    if (texture->object == NULL) {
        metal->api->mem_free(metal->api, texture);

        return NULL;
    }

    texture->width = width;
    texture->height = height;

    return texture;
}

extern hdrg_metal_texture_t *hdrg_metal_surface_texture(hdrg_metal_t *metal, void *surface,
                                                        uint32_t plane, uint32_t format)
{
    const MTLPixelFormat pixel = hdrg_metal_format(format);
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    IOSurfaceRef iosurface = (IOSurfaceRef)surface;
    MTLTextureDescriptor *desc;
    hdrg_metal_texture_t *texture;
    id<MTLTexture> object;
    uint32_t width, height;

    if (pixel == MTLPixelFormatInvalid || iosurface == NULL ||
        plane >= IOSurfaceGetPlaneCount(iosurface)) {
        return NULL;
    }

    width = (uint32_t)IOSurfaceGetWidthOfPlane(iosurface, plane);
    height = (uint32_t)IOSurfaceGetHeightOfPlane(iosurface, plane);
    texture = (hdrg_metal_texture_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*texture));
    if (texture == NULL) {
        return NULL;
    }

    texture->api = metal->api;

    @autoreleasepool {
        desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel
                                                                  width:width
                                                                 height:height
                                                              mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        object = [device newTextureWithDescriptor:desc iosurface:iosurface plane:plane];
        texture->object = (object != nil) ? (void *)CFBridgingRetain(object) : NULL;
    }

    if (texture->object == NULL) {
        metal->api->mem_free(metal->api, texture);

        return NULL;
    }

    texture->width = width;
    texture->height = height;

    return texture;
}

extern void hdrg_metal_texture_free(hdrg_metal_texture_t *texture)
{
    if (texture == NULL) {
        return;
    }

    hdrg_metal_release(&texture->object);
    texture->api->mem_free(texture->api, texture);
}

extern hdrg_metal_kernel_t *hdrg_metal_kernel(hdrg_metal_t *metal, const char *msl,
                                              const char *entry, const char *uniform,
                                              const char *const *storage, uint8_t n)
{
    const char *name;
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    MTLCompileOptions *options;
    MTLComputePipelineReflection *reflection;
    NSError *error;
    hdrg_metal_kernel_t *kernel;
    id<MTLLibrary> library;
    id<MTLFunction> function;
    id<MTLComputePipelineState> pipeline;
    id<MTLBinding> binding;
    uint32_t found;
    uint8_t i, j;
    bool ok;

    if (n > HDRG_METAL_BUFFERS) {
        return NULL;
    }

    kernel = (hdrg_metal_kernel_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*kernel));
    if (kernel == NULL) {
        return NULL;
    }

    kernel->api = metal->api;
    kernel->uniform = HDRG_METAL_NONE;
    kernel->count = n;
    found = 0;
    ok = false;

    for (i = 0; i < n; i++) {
        kernel->buffers[i] = HDRG_METAL_NONE;
    }

    @autoreleasepool {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            options = [MTLCompileOptions new];

            /* not the default fast math: the results stay within L2 of the CPU's */
            if (@available(macOS 15.0, iOS 18.0, *)) {
                options.mathMode = MTLMathModeSafe;
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                options.fastMathEnabled = NO;
#pragma clang diagnostic pop
            }

            error = nil;
            reflection = nil;
            library = [device newLibraryWithSource:@(msl) options:options error:&error];
            function = (library != nil) ? [library newFunctionWithName:@(entry)] : nil;
            pipeline =
                (function != nil)
                    ? [device newComputePipelineStateWithFunction:function
                                                          options:MTLPipelineOptionBindingInfo
                                                       reflection:&reflection
                                                            error:&error]
                    : nil;
            ok = pipeline != nil && reflection != nil;

            /*
             * each binding a buffer (the uniform, a storage buffer or Tint's
             * immediate data), or the workgroup memory (one block of it)
             */
            for (binding in (ok ? reflection.bindings : @[])) {
                name = binding.name.UTF8String;

                if (ok && binding.type == MTLBindingTypeThreadgroupMemory && kernel->local == 0) {
                    kernel->local = ((id<MTLThreadgroupBinding>)binding).threadgroupMemoryDataSize;
                    kernel->local_at = (uint32_t)binding.index;
                    ok = kernel->local > 0;

                    continue;
                }

                ok = ok && binding.type == MTLBindingTypeBuffer && name != NULL;

                if (!ok) {
                    break;
                }

                if (binding.index == HDRG_METAL_IMMEDIATE &&
                    strcmp(name, "tint_immediate_data") == 0) {
                    kernel->immediate = ((id<MTLBufferBinding>)binding).bufferDataSize;

                    continue;
                }

                if (uniform != NULL && strcmp(name, uniform) == 0 &&
                    kernel->uniform == HDRG_METAL_NONE) {
                    kernel->uniform = (uint32_t)binding.index;
                    found++;

                    continue;
                }

                for (i = 0; i < n; i++) {
                    if (strcmp(name, storage[i]) == 0 && kernel->buffers[i] == HDRG_METAL_NONE) {
                        kernel->buffers[i] = (uint32_t)binding.index;
                        found++;
                        break;
                    }
                }

                ok = i < n;
            }

            kernel->pipeline = ok ? (void *)CFBridgingRetain(pipeline) : NULL;
        }
    }

    ok = ok && found == n + ((uniform != NULL) ? 1u : 0u);

    /* the sizes in the storage buffers' order of index; all of them, or none (no robustness) */
    for (i = 0; ok && i < n; i++) {
        kernel->slots[i] = 0;

        for (j = 0; j < n; j++) {
            kernel->slots[i] += (kernel->buffers[j] < kernel->buffers[i]) ? 1u : 0u;
        }
    }

    if (ok && kernel->immediate != 0 && n > 0) {
        ok = kernel->immediate == HDRG_METAL_SIZES + 4u * (size_t)n;
    }

    if (!ok) {
        hdrg_metal_kernel_free(kernel);

        return NULL;
    }

    return kernel;
}

extern void hdrg_metal_kernel_free(hdrg_metal_kernel_t *kernel)
{
    if (kernel == NULL) {
        return;
    }

    hdrg_metal_release(&kernel->pipeline);
    kernel->api->mem_free(kernel->api, kernel);
}

extern hdrg_metal_pass_t *hdrg_metal_begin(hdrg_metal_t *metal)
{
    id<MTLCommandQueue> queue = hdrg_metal_object(metal->queue);
    hdrg_metal_pass_t *pass;
    id<MTLCommandBuffer> commands;

    pass = (hdrg_metal_pass_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*pass));
    if (pass == NULL) {
        return NULL;
    }

    pass->api = metal->api;

    @autoreleasepool {
        commands = [queue commandBuffer];
        pass->commands = (commands != nil) ? (void *)CFBridgingRetain(commands) : NULL;
    }

    if (pass->commands == NULL) {
        metal->api->mem_free(metal->api, pass);

        return NULL;
    }

    return pass;
}

extern void hdrg_metal_dispatch(hdrg_metal_pass_t *pass, const hdrg_metal_kernel_t *kernel,
                                const void *uniform, size_t uniform_size,
                                hdrg_metal_buffer_t *const *buffers, uint32_t groups_x,
                                uint32_t groups_y, uint32_t group_w, uint32_t group_h)
{
    id<MTLComputeCommandEncoder> encoder;
    uint32_t immediate[HDRG_METAL_SIZES / 4u + HDRG_METAL_BUFFERS];
    uint8_t i;

    memset(immediate, 0, sizeof(immediate));

    @autoreleasepool {
        encoder = hdrg_metal_compute(pass);
        if (encoder == nil) {
            return;
        }

        [encoder setComputePipelineState:hdrg_metal_object(kernel->pipeline)];

        if (kernel->uniform != HDRG_METAL_NONE) {
            [encoder setBytes:uniform length:uniform_size atIndex:kernel->uniform];
        }

        /* Tint's array lengths: the buffers' whole sizes, in 32-bit words of bytes */
        for (i = 0; i < kernel->count; i++) {
            [encoder setBuffer:hdrg_metal_object(buffers[i]->object)
                        offset:0
                       atIndex:kernel->buffers[i]];
            immediate[HDRG_METAL_SIZES / 4u + kernel->slots[i]] =
                (uint32_t)((buffers[i]->size < UINT32_MAX) ? buffers[i]->size : UINT32_MAX - 3u);
        }

        if (kernel->immediate != 0) {
            [encoder setBytes:immediate length:kernel->immediate atIndex:HDRG_METAL_IMMEDIATE];
        }

        /* in multiples of 16 bytes */
        if (kernel->local != 0) {
            [encoder setThreadgroupMemoryLength:(kernel->local + 15u) & ~(size_t)15u
                                        atIndex:kernel->local_at];
        }

        [encoder dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, 1)
                threadsPerThreadgroup:MTLSizeMake(group_w, group_h, 1)];
    }
}

extern void hdrg_metal_copy(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *buffer, size_t pitch,
                            hdrg_metal_texture_t *texture, bool upload)
{
    hdrg_metal_copy_region(pass, buffer, 0, pitch, texture, texture->width, texture->height,
                           upload);
}

extern void hdrg_metal_copy_region(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *buffer,
                                   size_t offset, size_t pitch, hdrg_metal_texture_t *texture,
                                   uint32_t width, uint32_t height, bool upload)
{
    id<MTLCommandBuffer> commands = hdrg_metal_object(pass->commands);
    id<MTLBuffer> rows = hdrg_metal_object(buffer->object);
    id<MTLTexture> image = hdrg_metal_object(texture->object);
    id<MTLBlitCommandEncoder> blit;
    MTLSize size;

    hdrg_metal_end_compute(pass);

    size = MTLSizeMake(width, height, 1);

    @autoreleasepool {
        blit = [commands blitCommandEncoder];

        if (upload) {
            [blit copyFromBuffer:rows
                       sourceOffset:offset
                  sourceBytesPerRow:pitch
                sourceBytesPerImage:pitch * height
                         sourceSize:size
                          toTexture:image
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        } else {
            [blit copyFromTexture:image
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:size
                                toBuffer:rows
                       destinationOffset:offset
                  destinationBytesPerRow:pitch
                destinationBytesPerImage:pitch * height];
        }

        [blit endEncoding];
    }
}

extern void hdrg_metal_copy_buffer(hdrg_metal_pass_t *pass, hdrg_metal_buffer_t *src,
                                   hdrg_metal_buffer_t *dst, size_t size)
{
    id<MTLCommandBuffer> commands = hdrg_metal_object(pass->commands);
    id<MTLBlitCommandEncoder> blit;

    hdrg_metal_end_compute(pass);

    @autoreleasepool {
        blit = [commands blitCommandEncoder];
        [blit copyFromBuffer:hdrg_metal_object(src->object)
                 sourceOffset:0
                     toBuffer:hdrg_metal_object(dst->object)
            destinationOffset:0
                         size:size];
        [blit endEncoding];
    }
}

extern bool hdrg_metal_commit(hdrg_metal_pass_t *pass, hdrg_metal_done_fn_t done, void *arg)
{
    id<MTLCommandBuffer> commands;
    bool ok;

    hdrg_metal_end_compute(pass);

    ok = true;

    @autoreleasepool {
        commands = CFBridgingRelease(pass->commands);
        pass->api->mem_free(pass->api, pass);

        if (done != NULL) {
            [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) {
              (void)atomic_load_explicit(&hdrg_metal_commits, memory_order_acquire);
              done(arg,
                   finished.status == MTLCommandBufferStatusCompleted && finished.error == nil);
            }];
        }

        (void)atomic_fetch_add_explicit(&hdrg_metal_commits, 1u, memory_order_release);
        [commands commit];

        if (done == NULL) {
            [commands waitUntilCompleted];
            ok = commands.status == MTLCommandBufferStatusCompleted && commands.error == nil;
        }
    }

    return ok;
}

extern hdrg_metal_spatial_t *hdrg_metal_spatial(hdrg_metal_t *metal, uint32_t format, uint32_t in_w,
                                                uint32_t in_h, uint32_t out_w, uint32_t out_h)
{
    const MTLPixelFormat pixel = hdrg_metal_format(format);
    id<MTLDevice> device = hdrg_metal_object(metal->device);
    MTLFXSpatialScalerDescriptor *desc;
    hdrg_metal_spatial_t *spatial;
    id<MTLFXSpatialScaler> scaler;

    if (pixel == MTLPixelFormatInvalid) {
        return NULL;
    }

    spatial = (hdrg_metal_spatial_t *)metal->api->mem_calloc(metal->api, 1, sizeof(*spatial));
    if (spatial == NULL) {
        return NULL;
    }

    spatial->api = metal->api;

    @autoreleasepool {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            if ([MTLFXSpatialScalerDescriptor supportsDevice:device]) {
                desc = [MTLFXSpatialScalerDescriptor new];
                desc.inputWidth = in_w;
                desc.inputHeight = in_h;
                desc.outputWidth = out_w;
                desc.outputHeight = out_h;
                desc.colorTextureFormat = pixel;
                desc.outputTextureFormat = pixel;
                desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
                scaler = [desc newSpatialScalerWithDevice:device];
                spatial->scaler = (scaler != nil) ? (void *)CFBridgingRetain(scaler) : NULL;
            }
        }
    }

    if (spatial->scaler == NULL) {
        metal->api->mem_free(metal->api, spatial);

        return NULL;
    }

    return spatial;
}

extern void hdrg_metal_spatial_free(hdrg_metal_spatial_t *spatial)
{
    if (spatial == NULL) {
        return;
    }

    hdrg_metal_release(&spatial->scaler);
    spatial->api->mem_free(spatial->api, spatial);
}

extern void hdrg_metal_spatial_encode(hdrg_metal_pass_t *pass, hdrg_metal_spatial_t *spatial,
                                      hdrg_metal_texture_t *in, hdrg_metal_texture_t *out)
{
    id<MTLFXSpatialScaler> scaler;

    hdrg_metal_end_compute(pass);

    @autoreleasepool {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            scaler = hdrg_metal_object(spatial->scaler);
            scaler.colorTexture = hdrg_metal_object(in->object);
            scaler.outputTexture = hdrg_metal_object(out->object);
            scaler.inputContentWidth = in->width;
            scaler.inputContentHeight = in->height;
            [scaler encodeToCommandBuffer:hdrg_metal_object(pass->commands)];
        }
    }
}
