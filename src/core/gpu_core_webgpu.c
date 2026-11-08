/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/gpu/webgpu.h"
#include "gpu_core.h"

#if defined(HDRG_WITH_CVPIXELBUFFER)
#include <IOSurface/IOSurfaceRef.h>
#endif

#define HDRG_GCW_ENTRIES 4u /* a program's entry points at most */
#define HDRG_GCW_USAGE (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc)

/*
 * The core's GPU work on WebGPU (src/core/wgsl, src/gpu/webgpu): the
 * buffers' CPU views are host memory of the backend's, uploaded before the
 * dispatches and filled from read-back buffers after them, all on the
 * device's thread
 */
typedef struct {
    const hdrg_plugin_api_t *api;
    hdrg_gpu_t *gpu;
    WGPUBindGroupLayout group_layouts[HDRG_GC_PROGRAMS]; /* the device's thread */
    WGPUPipelineLayout layouts[HDRG_GC_PROGRAMS];
    WGPUComputePipeline pipelines[HDRG_GC_PROGRAMS][HDRG_GCW_ENTRIES];
    WGPUBuffer params[HDRG_GC_PROGRAMS];
    WGPUBuffer buffers[HDRG_GC_SLOTS], readback[HDRG_GC_SLOTS];
    hdrg_gpu_share_frame_t frames[HDRG_GC_SLOTS]; /* the slots as frames of the library's */
    uint8_t *views[HDRG_GC_SLOTS];
    size_t sizes[HDRG_GC_SLOTS];     /* the views' */
    size_t used[HDRG_GC_SLOTS];      /* the run's */
    size_t devices[HDRG_GC_SLOTS];   /* the device buffers' */
    size_t readbacks[HDRG_GC_SLOTS]; /* the read-back buffers' */
    bool built[HDRG_GC_PROGRAMS];
    bool imported[HDRG_GC_SLOTS]; /* the slot holds the app's buffer (hdrg_gcw_import) */
} hdrg_gcw_t;

/* a take on the device's thread */
typedef struct {
    const hdrg_gpu_share_frame_t *from;
    hdrg_gcw_t *gcw;
    size_t size;
    uint8_t slot;
    bool ok;
} hdrg_gcw_take_t;

/* a run on the device's thread */
typedef struct {
    const void *params;
    const uint8_t *slots;
    const hdrg_gc_dispatch_t *dispatches;
    hdrg_gcw_t *gcw;
    uint64_t in, out;
    uint8_t program, count;
    bool ok;
} hdrg_gcw_work_t;

/* a surface plane's copy on the device's thread (the surface op) */
typedef struct {
    hdrg_gcw_t *gcw;
    void *surface;
    hdrg_status_t status;
    size_t at, pitch;
    uint32_t texels, rows;
    uint8_t plane, texel, slot;
    bool in;
} hdrg_gcw_surface_t;

static void hdrg_gcw_release_buffer(WGPUBuffer *buffer)
{
    if (*buffer != NULL) {
        wgpuBufferRelease(*buffer);
        *buffer = NULL;
    }
}

/* the device's thread: the program's layouts, pipelines and uniform, once */
/* the device's thread: the objects of a program (a build's, failed or done) released */
static void hdrg_gcw_drop(hdrg_gcw_t *gcw, uint8_t program)
{
    uint8_t e;

    for (e = 0; e < HDRG_GCW_ENTRIES; e++) {
        if (gcw->pipelines[program][e] != NULL) {
            wgpuComputePipelineRelease(gcw->pipelines[program][e]);
            gcw->pipelines[program][e] = NULL;
        }
    }

    if (gcw->layouts[program] != NULL) {
        wgpuPipelineLayoutRelease(gcw->layouts[program]);
        gcw->layouts[program] = NULL;
    }

    if (gcw->group_layouts[program] != NULL) {
        wgpuBindGroupLayoutRelease(gcw->group_layouts[program]);
        gcw->group_layouts[program] = NULL;
    }

    hdrg_gcw_release_buffer(&gcw->params[program]);
}

static bool hdrg_gcw_build(hdrg_gpu_t *gpu, hdrg_gcw_t *gcw, uint8_t program)
{
    const hdrg_gc_program_t *prog = &hdrg_gc_programs[program];
    WGPUBindGroupLayoutEntry entries[1u + HDRG_GC_BINDINGS];
    WGPUBindGroupLayoutDescriptor group_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUShaderModule module;
    uint32_t i;
    uint8_t e;

    if (gcw->built[program]) {
        return true;
    }

    /* what a failed build before made, made again */
    hdrg_gcw_drop(gcw, program);

    for (i = 0; i <= prog->buffer_count; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = (i == 0u) ? WGPUBufferBindingType_Uniform
                                 : ((prog->writes & (1u << (i - 1u))) != 0u)
                                     ? WGPUBufferBindingType_Storage
                                     : WGPUBufferBindingType_ReadOnlyStorage;
    }

    group_desc.entryCount = 1u + prog->buffer_count;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    gcw->group_layouts[program] =
        wgpuDeviceCreateBindGroupLayout(hdrg_gpu_device(gpu), &group_desc);
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &gcw->group_layouts[program];
    gcw->layouts[program] = wgpuDeviceCreatePipelineLayout(hdrg_gpu_device(gpu), &layout_desc);

    if (!hdrg_gpu_scope_end(gpu)) {
        return false;
    }

    module = hdrg_gpu_module(gpu, prog->wgsl);
    if (module == NULL) {
        return false;
    }

    for (e = 0; e < prog->entry_count; e++) {
        gcw->pipelines[program][e] =
            hdrg_gpu_pipeline(gpu, module, prog->entries[e].shader.entry, gcw->layouts[program]);
    }

    gcw->params[program] =
        hdrg_gpu_buffer(gpu, prog->params_size, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    wgpuShaderModuleRelease(module);

    for (e = 0; e < prog->entry_count; e++) {
        if (gcw->pipelines[program][e] == NULL) {
            return false;
        }
    }

    gcw->built[program] = gcw->params[program] != NULL;

    return gcw->built[program];
}

/* the device's thread: every object */
static void hdrg_gcw_release(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)arg;
    uint8_t p, i;

    (void)gpu;

    for (p = 0; p < HDRG_GC_PROGRAMS; p++) {
        hdrg_gcw_drop(gcw, p);
    }

    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        hdrg_gcw_release_buffer(&gcw->buffers[i]);
        hdrg_gcw_release_buffer(&gcw->readback[i]);
    }
}

/*
 * the device's thread: the device buffers of the run's slots and the
 * read-back buffers of its outputs, kept while large enough
 */
static bool hdrg_gcw_devices(hdrg_gpu_t *gpu, hdrg_gcw_work_t *work)
{
    const hdrg_gc_program_t *prog = &hdrg_gc_programs[work->program];
    hdrg_gcw_t *gcw = work->gcw;
    uint8_t b, s;

    for (b = 0; b < prog->buffer_count; b++) {
        s = work->slots[b];

        if (gcw->used[s] > gcw->devices[s]) {
            hdrg_gcw_release_buffer(&gcw->buffers[s]);
            gcw->buffers[s] = hdrg_gpu_buffer(gpu, gcw->used[s], HDRG_GCW_USAGE);
            gcw->imported[s] = false;
            gcw->devices[s] = (gcw->buffers[s] != NULL) ? gcw->used[s] : 0u;
            gcw->frames[s].kind = HDRG_DEVICE_WEBGPU;
            gcw->frames[s].buffer = (uint64_t)(uintptr_t)gcw->buffers[s];
            gcw->frames[s].size = gcw->devices[s];

            if (gcw->devices[s] == 0u) {
                return false;
            }
        }

        if ((work->out & (UINT64_C(1) << s)) != 0u && gcw->used[s] > gcw->readbacks[s]) {
            hdrg_gcw_release_buffer(&gcw->readback[s]);
            gcw->readback[s] = hdrg_gpu_buffer(gpu, gcw->used[s],
                                               WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
            gcw->readbacks[s] = (gcw->readback[s] != NULL) ? gcw->used[s] : 0u;

            if (gcw->readbacks[s] == 0u) {
                return false;
            }
        }
    }

    return true;
}

/* the device's thread: the uploads, the dispatches and the read-backs */
static void hdrg_gcw_work(hdrg_gpu_t *gpu, void *arg)
{
    const hdrg_gc_program_t *prog;
    const void *mapped;
    hdrg_gcw_work_t *work = (hdrg_gcw_work_t *)arg;
    hdrg_gcw_t *gcw = work->gcw;
    WGPUBindGroupEntry entries[1u + HDRG_GC_BINDINGS];
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroup group;
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer commands;
    uint32_t i;
    uint8_t d;
    bool ok;

    prog = &hdrg_gc_programs[work->program];
    work->ok = false;

    if (!hdrg_gcw_build(gpu, gcw, work->program) || !hdrg_gcw_devices(gpu, work)) {
        return;
    }

    for (i = 0; i <= prog->buffer_count; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].binding = i;
        entries[i].buffer =
            (i == 0u) ? gcw->params[work->program] : gcw->buffers[work->slots[i - 1u]];
        entries[i].size = WGPU_WHOLE_SIZE;
    }

    group_desc.layout = gcw->group_layouts[work->program];
    group_desc.entryCount = 1u + prog->buffer_count;
    group_desc.entries = entries;

    hdrg_gpu_scope_begin(gpu);

    wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), gcw->params[work->program], 0, work->params,
                         prog->params_size);

    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        if ((work->in & (UINT64_C(1) << i)) != 0u) {
            wgpuQueueWriteBuffer(hdrg_gpu_queue(gpu), gcw->buffers[i], 0, gcw->views[i],
                                 gcw->used[i]);
        }
    }

    group = wgpuDeviceCreateBindGroup(hdrg_gpu_device(gpu), &group_desc);
    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    pass = wgpuCommandEncoderBeginComputePass(encoder, NULL);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);

    for (d = 0; d < work->count; d++) {
        wgpuComputePassEncoderSetPipeline(pass,
                                          gcw->pipelines[work->program][work->dispatches[d].entry]);
        wgpuComputePassEncoderDispatchWorkgroups(pass, work->dispatches[d].groups_x,
                                                 work->dispatches[d].groups_y, 1);
    }

    wgpuComputePassEncoderEnd(pass);

    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        if ((work->out & (UINT64_C(1) << i)) != 0u) {
            wgpuCommandEncoderCopyBufferToBuffer(encoder, gcw->buffers[i], 0, gcw->readback[i], 0,
                                                 gcw->used[i]);
        }
    }

    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupRelease(group);

    ok = hdrg_gpu_scope_end(gpu);

    for (i = 0; ok && i < HDRG_GC_SLOTS; i++) {
        if ((work->out & (UINT64_C(1) << i)) == 0u) {
            continue;
        }

        mapped = hdrg_gpu_map_read(gpu, gcw->readback[i], gcw->used[i]);
        ok = mapped != NULL;

        if (ok) {
            memcpy(gcw->views[i], mapped, gcw->used[i]);
            wgpuBufferUnmap(gcw->readback[i]);
        }
    }

    work->ok = ok;
}

/* the device's thread: the slot's device buffer of its use (false: none to be had) */
static bool hdrg_gcw_slot(hdrg_gpu_t *gpu, hdrg_gcw_t *gcw, uint8_t slot, size_t size)
{
    if (size > gcw->used[slot]) {
        gcw->used[slot] = size;
    }

    /* the app's buffer imported here is never the library's to write into: its own made */
    if (gcw->buffers[slot] == NULL || gcw->used[slot] > gcw->devices[slot] || gcw->imported[slot]) {
        gcw->imported[slot] = false;
        hdrg_gcw_release_buffer(&gcw->buffers[slot]);
        hdrg_gcw_release_buffer(&gcw->readback[slot]);
        gcw->readbacks[slot] = 0;
        gcw->buffers[slot] = hdrg_gpu_buffer(gpu, gcw->used[slot], HDRG_GCW_USAGE);
        gcw->devices[slot] = (gcw->buffers[slot] != NULL) ? gcw->used[slot] : 0u;
        gcw->frames[slot].kind = HDRG_DEVICE_WEBGPU;
        gcw->frames[slot].buffer = (uint64_t)(uintptr_t)gcw->buffers[slot];
        gcw->frames[slot].size = gcw->devices[slot];
    }

    return gcw->buffers[slot] != NULL;
}

/* the device's thread: the frame's rows copied into the slot's buffer (made), submitted */
static void hdrg_gcw_take_work(hdrg_gpu_t *gpu, void *arg)
{
    hdrg_gcw_take_t *take = (hdrg_gcw_take_t *)arg;
    hdrg_gcw_t *gcw = take->gcw;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;

    take->ok = false;

    if (take->from->kind != HDRG_DEVICE_WEBGPU || take->from->buffer == 0u ||
        take->size > take->from->size || !hdrg_gcw_slot(gpu, gcw, take->slot, take->size)) {
        return;
    }

    hdrg_gpu_scope_begin(gpu);

    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, hdrg_gpu_frame_buffer(take->from), 0,
                                         gcw->buffers[take->slot], 0, take->size);
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);

    take->ok = hdrg_gpu_scope_end(gpu);
}

#if defined(HDRG_WITH_CVPIXELBUFFER)
/* the device's thread: the copy's commands between the texture's plane and rows of buffer */
static bool hdrg_gcw_texel_copy(hdrg_gpu_t *gpu, WGPUTexture texture, WGPUTextureAspect aspect,
                                WGPUBuffer buffer, uint32_t pitch, uint32_t texels, uint32_t rows,
                                bool in)
{
    WGPUTexelCopyTextureInfo image = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo lines = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent = WGPU_EXTENT_3D_INIT;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer commands;

    image.texture = texture;
    image.aspect = aspect;
    lines.buffer = buffer;
    lines.layout.bytesPerRow = pitch;
    lines.layout.rowsPerImage = rows;
    extent.width = texels;
    extent.height = rows;
    extent.depthOrArrayLayers = 1;

    hdrg_gpu_scope_begin(gpu);

    encoder = wgpuDeviceCreateCommandEncoder(hdrg_gpu_device(gpu), NULL);

    if (in) {
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &image, &lines, &extent);
    } else {
        wgpuCommandEncoderCopyBufferToTexture(encoder, &lines, &image, &extent);
    }

    commands = wgpuCommandEncoderFinish(encoder, NULL);
    wgpuQueueSubmit(hdrg_gpu_queue(gpu), 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);

    return hdrg_gpu_scope_end(gpu);
}

/*
 * The device's thread: an IOSurface's plane through Dawn's shared texture
 * memory (its multi-planar texture, a plane's aspect) and rows of 256-byte
 * aligned pitch in the plane's app slot, copied by io.wgsl between those and
 * the slot's rows. Two-plane surfaces alone (Dawn has no 3-plane Y'CbCr), and
 * written only where the texture takes copies to it (Dawn's Metal backend
 * gives multi-planar textures CopySrc and TextureBinding alone)
 */
static void hdrg_gcw_surface_work(hdrg_gpu_t *gpu, void *arg)
{
    static const WGPUTextureAspect aspects[2] = {WGPUTextureAspect_Plane0Only,
                                                 WGPUTextureAspect_Plane1Only};
    static const uint8_t sizes[5] = {0, 1, 2, 2, 4};
    hdrg_gcw_surface_t *job = (hdrg_gcw_surface_t *)arg;
    hdrg_gcw_t *gcw = job->gcw;
    WGPUSharedTextureMemoryIOSurfaceDescriptor io =
        WGPU_SHARED_TEXTURE_MEMORY_IO_SURFACE_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryDescriptor desc = WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryBeginAccessDescriptor begin =
        WGPU_SHARED_TEXTURE_MEMORY_BEGIN_ACCESS_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryEndAccessState end = WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
    WGPUDevice device = hdrg_gpu_device(gpu);
    WGPUSharedTextureMemory memory;
    WGPUTexture texture;
    hdrg_gc_copy_params_t params;
    hdrg_gcw_work_t work;
    hdrg_gc_dispatch_t dispatch;
    uint32_t aligned;
    uint8_t slots[2], app;
    bool ok;

    job->status = HDRG_ERR_UNSUPPORTED;

    if (!wgpuDeviceHasFeature(device, WGPUFeatureName_SharedTextureMemoryIOSurface) ||
        !wgpuDeviceHasFeature(device, WGPUFeatureName_DawnMultiPlanarFormats) ||
        IOSurfaceGetPlaneCount((IOSurfaceRef)job->surface) != 2u || job->plane > 1u) {
        return;
    }

    app = (uint8_t)(HDRG_GC_APP0 + job->plane);
    aligned = (uint32_t)((((size_t)job->texels * sizes[job->texel]) + 255u) & ~(size_t)255u);

    if (!hdrg_gcw_slot(gpu, gcw, app, (size_t)aligned * job->rows) ||
        !hdrg_gcw_slot(gpu, gcw, job->slot, 0)) {
        job->status = HDRG_ERR_NOMEM;

        return;
    }

    if (job->at + (job->pitch * (job->rows - 1u)) + ((size_t)job->texels * sizes[job->texel]) >
        gcw->devices[job->slot]) {
        job->status = HDRG_ERR_INVALID_ARG;

        return;
    }

    io.ioSurface = job->surface;
    desc.nextInChain = &io.chain;

    hdrg_gpu_scope_begin(gpu);

    memory = wgpuDeviceImportSharedTextureMemory(device, &desc);
    texture = (memory != NULL) ? wgpuSharedTextureMemoryCreateTexture(memory, NULL) : NULL;

    begin.initialized = true;
    ok = hdrg_gpu_scope_end(gpu) && texture != NULL &&
         (wgpuTextureGetUsage(texture) &
          (job->in ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_CopyDst)) != 0u &&
         wgpuSharedTextureMemoryBeginAccess(memory, texture, &begin) == WGPUStatus_Success;

    if (ok) {
        memset(&params, 0, sizeof(params));
        params.rows = job->rows;
        params.words = (uint32_t)((((size_t)job->texels * sizes[job->texel]) + 3u) / 4u);
        params.src_at = job->in ? 0u : (uint32_t)(job->at / 4u);
        params.src_pitch = job->in ? aligned / 4u : (uint32_t)(job->pitch / 4u);
        params.dst_at = job->in ? (uint32_t)(job->at / 4u) : 0u;
        params.dst_pitch = job->in ? (uint32_t)(job->pitch / 4u) : aligned / 4u;
        slots[0] = job->in ? app : job->slot;
        slots[1] = job->in ? job->slot : app;
        dispatch.entry = 0;
        dispatch.groups_x = (params.words + HDRG_GC_GROUP - 1u) / HDRG_GC_GROUP;
        dispatch.groups_y = params.rows;
        memset(&work, 0, sizeof(work));
        work.gcw = gcw;
        work.params = &params;
        work.slots = slots;
        work.dispatches = &dispatch;
        work.program = HDRG_GC_COPY;
        work.count = 1;

        if (job->in) {
            ok = hdrg_gcw_texel_copy(gpu, texture, aspects[job->plane], gcw->buffers[app], aligned,
                                     job->texels, job->rows, true);
            hdrg_gcw_work(gpu, &work);
            ok = ok && work.ok;
        } else {
            hdrg_gcw_work(gpu, &work);
            ok =
                work.ok && hdrg_gcw_texel_copy(gpu, texture, aspects[job->plane], gcw->buffers[app],
                                               aligned, job->texels, job->rows, false);
        }

        ok = hdrg_gpu_finish(gpu) && ok;
        (void)wgpuSharedTextureMemoryEndAccess(memory, texture, &end);
        wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);
        job->status = ok ? HDRG_OK : HDRG_ERR_INTERNAL;
    }

    if (texture != NULL) {
        wgpuTextureRelease(texture);
    }

    if (memory != NULL) {
        wgpuSharedTextureMemoryRelease(memory);
    }
}
#endif

static void hdrg_gcw_close(void *state)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;
    uint8_t i;

    (void)hdrg_gpu_call(gcw->gpu, hdrg_gcw_release, gcw);
    hdrg_gpu_destroy(gcw->gpu);

    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        gcw->api->mem_free(gcw->api, gcw->views[i]);
    }

    gcw->api->mem_free(gcw->api, gcw);
}

static bool hdrg_gcw_open(const hdrg_plugin_api_t *api, void *device, void *queue, void **state)
{
    hdrg_gcw_t *gcw;
    hdrg_gpu_t *gpu;

    (void)queue;

    if (!hdrg_gpu_create(api, (WGPUDevice)device, &gpu)) {
        return false;
    }

    gcw = (hdrg_gcw_t *)api->mem_calloc(api, 1, sizeof(*gcw));
    if (gcw == NULL) {
        hdrg_gpu_destroy(gpu);

        return false;
    }

    gcw->api = api;
    gcw->gpu = gpu;
    *state = gcw;

    return true;
}

static bool hdrg_gcw_buffer(void *state, uint8_t slot, size_t size, uint8_t **view)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;

    gcw->used[slot] = size;

    if (view == NULL) {
        return true;
    }

    if (size > gcw->sizes[slot]) {
        gcw->api->mem_free(gcw->api, gcw->views[slot]);
        gcw->views[slot] = (uint8_t *)gcw->api->mem_alloc(gcw->api, size);
        gcw->sizes[slot] = (gcw->views[slot] != NULL) ? size : 0u;
    }

    *view = gcw->views[slot];

    return *view != NULL;
}

static bool hdrg_gcw_run(void *state, uint8_t program, const void *params, const uint8_t *slots,
                         const hdrg_gc_dispatch_t *dispatches, uint8_t count, uint64_t in,
                         uint64_t out)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;
    hdrg_gcw_work_t work;

    work.gcw = gcw;
    work.params = params;
    work.slots = slots;
    work.dispatches = dispatches;
    work.in = in;
    work.out = out;
    work.program = program;
    work.count = count;
    work.ok = false;

    return hdrg_gpu_call(gcw->gpu, hdrg_gcw_work, &work) && work.ok;
}

static void *hdrg_gcw_frame(void *state, uint8_t slot)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;

    return (gcw->frames[slot].buffer != 0u) ? &gcw->frames[slot] : NULL;
}

static bool hdrg_gcw_take(void *state, const void *frame, uint8_t slot, size_t size)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;
    hdrg_gcw_take_t take;

    take.from = (const hdrg_gpu_share_frame_t *)frame;
    take.gcw = gcw;
    take.size = size;
    take.slot = slot;
    take.ok = false;

    return hdrg_gpu_call(gcw->gpu, hdrg_gcw_take_work, &take) && take.ok;
}

/* the app's buffer kept in the slot with a reference (its size: used and made alike) */
static bool hdrg_gcw_import(void *state, uint8_t slot, uint32_t kind, void *buffer, size_t bytes,
                            size_t *size)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;

    (void)bytes;

    if (kind != HDRG_FRAME_BUFFERS || buffer == NULL) {
        return false;
    }

    if (gcw->buffers[slot] != (WGPUBuffer)buffer) {
        hdrg_gcw_release_buffer(&gcw->buffers[slot]);
        hdrg_gcw_release_buffer(&gcw->readback[slot]);
        wgpuBufferAddRef((WGPUBuffer)buffer);
        gcw->buffers[slot] = (WGPUBuffer)buffer;
        gcw->imported[slot] = true;
        gcw->devices[slot] = (size_t)wgpuBufferGetSize((WGPUBuffer)buffer);
        gcw->used[slot] = gcw->devices[slot];
        gcw->readbacks[slot] = 0;
        gcw->frames[slot].kind = HDRG_DEVICE_WEBGPU;
        gcw->frames[slot].buffer = (uint64_t)(uintptr_t)gcw->buffers[slot];
        gcw->frames[slot].size = gcw->devices[slot];
    }

    *size = gcw->devices[slot];

    return true;
}

#if defined(HDRG_WITH_CVPIXELBUFFER)
static hdrg_status_t hdrg_gcw_surface(void *state, void *surface, uint8_t plane, uint8_t texel,
                                      uint8_t slot, size_t at, size_t pitch, uint32_t texels,
                                      uint32_t rows, bool in)
{
    hdrg_gcw_t *gcw = (hdrg_gcw_t *)state;
    hdrg_gcw_surface_t job;

    if (texel == 0 || texel > HDRG_GC_TEXEL_RG16 || rows == 0 || at % 4u != 0 || pitch % 4u != 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    job.gcw = gcw;
    job.surface = surface;
    job.status = HDRG_ERR_INTERNAL;
    job.at = at;
    job.pitch = pitch;
    job.texels = texels;
    job.rows = rows;
    job.plane = plane;
    job.texel = texel;
    job.slot = slot;
    job.in = in;

    return hdrg_gpu_call(gcw->gpu, hdrg_gcw_surface_work, &job) ? job.status : HDRG_ERR_NOMEM;
}
#endif

const hdrg_gc_ops_t hdrg_gc_webgpu = {
    .kind = HDRG_DEVICE_WEBGPU,
    .open = hdrg_gcw_open,
    .close = hdrg_gcw_close,
    .buffer = hdrg_gcw_buffer,
    .run = hdrg_gcw_run,
    .frame = hdrg_gcw_frame,
    .take = hdrg_gcw_take,
    .import = hdrg_gcw_import,
#if defined(HDRG_WITH_CVPIXELBUFFER)
    .surface = hdrg_gcw_surface,
#endif
};
