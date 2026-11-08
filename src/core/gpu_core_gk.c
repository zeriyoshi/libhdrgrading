/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if defined(HDRG_WITH_METAL)
#include "hdrgrading/gpu/metal.h"
#endif
#if defined(HDRG_WITH_VULKAN)
#include "hdrgrading/gpu/vulkan.h"
#endif
#include "hdrgrading/gpu/gk.h"
#include "gpu_core.h"

#define HDRG_GCK_ENTRIES    4u /* a program's entry points at most */
#define HDRG_GCK_AHB_PLANES 2u /* an AHardwareBuffer's planes (Y, CbCr) */

/*
 * The core's GPU work on a native API (src/gpu/gk.h: Metal, Vulkan): the
 * WGSL (src/core/wgsl) through Tint (src/core/msl, src/core/spirv), its
 * buffers the backend's host buffers (the CPU writes and reads them in
 * place), a run's dispatches in one pass, waited for. The analysis's runs go
 * on a queue of their own where the backend has one (what they read is
 * done: the frames they read were waited for, or uploaded), so they do not
 * wait for the filters' jobs in flight
 */
typedef struct {
    const hdrg_plugin_api_t *api;
    hdrg_gk_t gk;
    hdrg_gk_t side; /* the analysis's queue on the device (device NULL: the first's) */
    hdrg_gk_kernel_t *kernels[HDRG_GC_PROGRAMS][HDRG_GCK_ENTRIES];
    hdrg_gk_buffer_t *buffers[HDRG_GC_SLOTS];
    hdrg_gk_frame_t frames[HDRG_GC_SLOTS]; /* the slots as frames of the library's */
    hdrg_gk_wraps_t taken;                 /* the filters' frames taken, wrapped */
    uint8_t *views[HDRG_GC_SLOTS];
    size_t sizes[HDRG_GC_SLOTS];
    size_t asked[HDRG_GC_SLOTS]; /* the app's buffers' bytes as the app gave them */
    bool host[HDRG_GC_SLOTS];    /* a host buffer (else the GPU's alone) */
    bool built[HDRG_GC_PROGRAMS];
    bool taken_failed; /* a frame taken since the last run: its filter's work failed */
#if defined(HDRG_WITH_VULKAN)
    /*
     * an AHardwareBuffer's planes of a frame: its image imported once, the planes' copies
     * recorded until its last (CbCr), then copied in one pass and waited for
     */
    hdrg_vulkan_image_t *ahb_image;
    void *ahb_buffer;
    hdrg_vulkan_plane_copy_t ahb_copies[HDRG_GCK_AHB_PLANES];
    uint32_t ahb_width, ahb_height;
    uint8_t ahb_slot, ahb_format, ahb_count;
    bool ahb_write;
#endif
} hdrg_gck_t;

/* a take's end: the next run waits for it on the queue */
static void hdrg_gck_taken(void *arg, bool ok)
{
    (void)arg;
    (void)ok;
}

/* the program's kernels, once */
static bool hdrg_gck_build(hdrg_gck_t *gck, uint8_t program)
{
    const hdrg_gc_program_t *prog = &hdrg_gc_programs[program];
    const hdrg_gc_entry_t *entry;
    const char *names[HDRG_GC_BINDINGS];
    uint8_t e, b;

    if (gck->built[program]) {
        return true;
    }

    for (e = 0; e < prog->entry_count; e++) {
        entry = &prog->entries[e];

        for (b = 0; b < entry->count; b++) {
            names[b] = prog->names[entry->buffers[b]];
        }

        /* a kernel of a failed build before, made again */
        hdrg_gk_kernel_free(&gck->gk, gck->kernels[program][e]);
        gck->kernels[program][e] =
            hdrg_gk_kernel(&gck->gk, &entry->shader, "params", names, entry->count);
        if (gck->kernels[program][e] == NULL) {
            return false;
        }
    }

    gck->built[program] = true;

    return true;
}

#if defined(HDRG_WITH_VULKAN)
/* the AHardwareBuffer's recorded copies run in one pass, waited for; its image released */
static bool hdrg_gck_ahb_flush(hdrg_gck_t *gck)
{
    hdrg_vulkan_pass_t *pass;
    bool ok;

    ok = true;

    if (gck->ahb_image != NULL && gck->ahb_count > 0) {
        pass = hdrg_vulkan_begin((hdrg_vulkan_t *)gck->gk.device);
        ok = pass != NULL;

        if (ok) {
            hdrg_vulkan_copy_image(pass, (hdrg_vulkan_buffer_t *)gck->buffers[gck->ahb_slot],
                                   gck->ahb_image, gck->ahb_copies, gck->ahb_count, gck->ahb_write);
            ok = hdrg_vulkan_commit(pass, NULL, NULL);
        }
    }

    if (gck->ahb_image != NULL) {
        hdrg_vulkan_image_free(gck->ahb_image);
    }

    gck->ahb_image = NULL;
    gck->ahb_buffer = NULL;
    gck->ahb_count = 0;

    return ok;
}
#endif

static void hdrg_gck_close(void *state)
{
    hdrg_gck_t *gck = (hdrg_gck_t *)state;
    uint8_t p, e, i;

#if defined(HDRG_WITH_VULKAN)
    (void)hdrg_gck_ahb_flush(gck);
#endif

    for (p = 0; p < HDRG_GC_PROGRAMS; p++) {
        for (e = 0; e < HDRG_GCK_ENTRIES; e++) {
            hdrg_gk_kernel_free(&gck->gk, gck->kernels[p][e]);
        }
    }

    /* the filters' completions reading the slots' frames run first */
    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        hdrg_gpu_share_wait(&gck->frames[i].share);
        hdrg_gk_buffer_free(&gck->gk, gck->buffers[i]);
    }

    hdrg_gk_wraps_free(&gck->gk, &gck->taken);

    if (gck->side.device != NULL) {
        gck->side.ops->destroy(gck->side.device);
    }

    gck->gk.ops->destroy(gck->gk.device);
    gck->api->mem_free(gck->api, gck);
}

static bool hdrg_gck_open(const hdrg_gk_ops_t *ops, const hdrg_plugin_api_t *api, void *device,
                          void *queue, void **state)
{
    hdrg_gck_t *gck;
    uint8_t i;
    void *made;

    if (!ops->create(api, device, queue, &made)) {
        return false;
    }

    gck = (hdrg_gck_t *)api->mem_calloc(api, 1, sizeof(*gck));
    if (gck == NULL) {
        ops->destroy(made);

        return false;
    }

    gck->api = api;
    gck->gk.ops = ops;
    gck->gk.device = made;
    gck->side.ops = ops;

    if (ops->side == NULL || !ops->side(api, made, &gck->side.device)) {
        gck->side.device = NULL;
    }

    for (i = 0; i < HDRG_GC_SLOTS; i++) {
        hdrg_atomic_u32_init(&gck->frames[i].share.failed, 0u);
        hdrg_atomic_u32_init(&gck->frames[i].share.pending, 0u);
    }

    *state = gck;

    return true;
}

static bool hdrg_gck_buffer(void *state, uint8_t slot, size_t size, uint8_t **view)
{
    const bool host = view != NULL;
    hdrg_gck_t *gck = (hdrg_gck_t *)state;

    if (size > gck->sizes[slot] || host != gck->host[slot]) {
        hdrg_gk_buffer_free(&gck->gk, gck->buffers[slot]);
        gck->views[slot] = NULL;
        gck->buffers[slot] = host ? hdrg_gk_host_buffer(&gck->gk, size, &gck->views[slot])
                                  : hdrg_gk_buffer(&gck->gk, size);
        gck->sizes[slot] = (gck->buffers[slot] != NULL) ? size : 0u;
        gck->host[slot] = host;
        hdrg_gk_frame_set(&gck->gk, &gck->frames[slot], gck->buffers[slot]);
    }

    if (host) {
        *view = gck->views[slot];
    }

    return gck->buffers[slot] != NULL;
}

static bool hdrg_gck_run(void *state, uint8_t program, const void *params, const uint8_t *slots,
                         const hdrg_gc_dispatch_t *dispatches, uint8_t count, uint64_t in,
                         uint64_t out)
{
    const hdrg_gc_program_t *prog = &hdrg_gc_programs[program];
    const hdrg_gc_entry_t *entry;
    const hdrg_gk_t *gk;
    hdrg_gck_t *gck = (hdrg_gck_t *)state;
    hdrg_gk_buffer_t *buffers[HDRG_GC_BINDINGS];
    hdrg_gk_pass_t *pass;
    uint8_t d, b;

    (void)in;
    (void)out;

    if (!hdrg_gck_build(gck, program)) {
        return false;
    }

    gk = (program == HDRG_GC_ANALYZE && gck->side.device != NULL) ? &gck->side : &gck->gk;
    pass = hdrg_gk_begin(gk);

    if (pass == NULL) {
        return false;
    }

    for (d = 0; d < count; d++) {
        entry = &prog->entries[dispatches[d].entry];

        for (b = 0; b < entry->count; b++) {
            buffers[b] = gck->buffers[slots[entry->buffers[b]]];
        }

        hdrg_gk_dispatch(gk, pass, gck->kernels[program][dispatches[d].entry], params,
                         prog->params_size, buffers, dispatches[d].groups_x, dispatches[d].groups_y,
                         HDRG_GC_GROUP, 1);
    }

    /* a frame taken from a filter whose work failed fails the run after it */
    if (!hdrg_gk_commit(gk, pass, NULL, NULL) || gck->taken_failed) {
        gck->taken_failed = false;

        return false;
    }

    return true;
}

static void *hdrg_gck_frame(void *state, uint8_t slot)
{
    hdrg_gck_t *gck = (hdrg_gck_t *)state;

    return (gck->buffers[slot] != NULL) ? &gck->frames[slot] : NULL;
}

static bool hdrg_gck_take(void *state, const void *frame, uint8_t slot, size_t size)
{
    hdrg_gpu_share_frame_t *from = (hdrg_gpu_share_frame_t *)(uintptr_t)frame;
    hdrg_gck_t *gck = (hdrg_gck_t *)state;
    hdrg_gk_buffer_t *buffer;
    hdrg_gk_pass_t *pass;

    buffer = hdrg_gk_wrap(&gck->gk, &gck->taken, from);
    if (gck->buffers[slot] == NULL || buffer == NULL || size > gck->sizes[slot] ||
        size > from->size) {
        return false;
    }

    pass = hdrg_gk_begin(&gck->gk);
    if (pass == NULL) {
        return false;
    }

    hdrg_gk_copy_buffer(&gck->gk, pass, buffer, gck->buffers[slot], size);

    if (!hdrg_gk_commit(&gck->gk, pass, hdrg_gck_taken, NULL)) {
        return false;
    }

    /* the filter's work through (its completion run): its failure final, the frame free to go back
     */
    hdrg_gpu_share_wait(from);
    gck->taken_failed = gck->taken_failed || hdrg_atomic_u32_load_acquire(&from->failed) != 0u;

    return true;
}

/*
 * the app's buffer in the slot: kept while the same comes again (its
 * reference the slot's), a dma-buf imported anew each time (its fd's number
 * may come again for another)
 */
static bool hdrg_gck_import(void *state, uint8_t slot, uint32_t kind, void *handle, size_t bytes,
                            size_t *size)
{
    hdrg_gck_t *gck = (hdrg_gck_t *)state;

    if (kind == HDRG_FRAME_DMABUF || gck->buffers[slot] == NULL ||
        gck->gk.ops->buffer_object(gck->buffers[slot]) != handle || gck->asked[slot] != bytes) {
        hdrg_gk_buffer_free(&gck->gk, gck->buffers[slot]);
        gck->buffers[slot] = gck->gk.ops->wrap(gck->gk.device, kind, handle, bytes);
        gck->sizes[slot] =
            (gck->buffers[slot] != NULL) ? gck->gk.ops->buffer_size(gck->buffers[slot]) : 0u;
        gck->asked[slot] = bytes;
        gck->views[slot] = NULL;
        gck->host[slot] = false;
        hdrg_gk_frame_set(&gck->gk, &gck->frames[slot], gck->buffers[slot]);
    }

    *size = gck->sizes[slot];

    return gck->buffers[slot] != NULL;
}

#if defined(HDRG_WITH_METAL)
static bool hdrg_gck_open_metal(const hdrg_plugin_api_t *api, void *device, void *queue,
                                void **state)
{
    return hdrg_gck_open(&hdrg_gk_metal, api, device, queue, state);
}

static hdrg_status_t hdrg_gck_surface(void *state, void *surface, uint8_t plane, uint8_t texel,
                                      uint8_t slot, size_t at, size_t pitch, uint32_t texels,
                                      uint32_t rows, bool in)
{
    static const uint32_t formats[5] = {0, HDRG_METAL_R8U, HDRG_METAL_R16U, HDRG_METAL_RG8U,
                                        HDRG_METAL_RG16U};
    static const uint8_t sizes[5] = {0, 1, 2, 2, 4};
    hdrg_gck_t *gck = (hdrg_gck_t *)state;
    hdrg_metal_texture_t *texture;
    hdrg_metal_pass_t *pass;
    hdrg_metal_t *metal;
    bool ok;

    if (texel == 0 || texel > HDRG_GC_TEXEL_RG16 || gck->buffers[slot] == NULL || rows == 0 ||
        at + (pitch * (rows - 1u)) + ((size_t)texels * sizes[texel]) > gck->sizes[slot]) {
        return HDRG_ERR_INVALID_ARG;
    }

    metal = (hdrg_metal_t *)gck->gk.device;
    texture = hdrg_metal_surface_texture(metal, surface, plane, formats[texel]);

    if (texture == NULL) {
        return HDRG_ERR_UNSUPPORTED;
    }

    pass = hdrg_metal_begin(metal);
    ok = pass != NULL;

    if (ok) {
        hdrg_metal_copy_region(pass, (hdrg_metal_buffer_t *)gck->buffers[slot], at, pitch, texture,
                               texels, rows, !in);
        ok = hdrg_metal_commit(pass, NULL, NULL);
    }

    hdrg_metal_texture_free(texture);

    return ok ? HDRG_OK : HDRG_ERR_INTERNAL;
}
#endif

#if defined(HDRG_WITH_VULKAN)
static bool hdrg_gck_open_vulkan(const hdrg_plugin_api_t *api, void *device, void *queue,
                                 void **state)
{
    return hdrg_gck_open(&hdrg_gk_vulkan, api, device, queue, state);
}

/*
 * an AHardwareBuffer's plane and the slot's rows (its planes the format's: Y, then CbCr of half
 * the width and height): the buffer imported as an image at its first plane, the copies of its
 * planes into or out of one slot recorded and run at its last in one pass, waited for (one
 * import, one submit, one wait a frame and way)
 */
static hdrg_status_t hdrg_gck_ahb(void *state, void *buffer, uint8_t plane, uint8_t texel,
                                  uint8_t slot, size_t at, size_t pitch, uint32_t texels,
                                  uint32_t rows, bool in)
{
    hdrg_gck_t *gck = (hdrg_gck_t *)state;
    hdrg_vulkan_plane_copy_t *copy;
    uint32_t plane_w, plane_h;
    uint8_t wanted;

    if (texel == 0 || texel > HDRG_GC_TEXEL_RG16 || plane > 1u || gck->buffers[slot] == NULL ||
        rows == 0 ||
        at + (pitch * (rows - 1u)) + ((size_t)texels * hdrg_gc_texel_bytes(texel)) >
            gck->sizes[slot]) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* another buffer, way or slot, or a frame's first plane: the copies recorded before run */
    if ((plane == 0 || gck->ahb_buffer != buffer || gck->ahb_write != !in ||
         gck->ahb_slot != slot) &&
        !hdrg_gck_ahb_flush(gck)) {
        return HDRG_ERR_INTERNAL;
    }

    if (gck->ahb_image == NULL) {
        gck->ahb_image = hdrg_vulkan_ahb_image((hdrg_vulkan_t *)gck->gk.device, buffer, !in,
                                               &gck->ahb_format, &gck->ahb_width, &gck->ahb_height);
        if (gck->ahb_image == NULL) {
            return HDRG_ERR_UNSUPPORTED;
        }

        gck->ahb_buffer = buffer;
        gck->ahb_write = !in;
        gck->ahb_slot = slot;
    }

    /* the texels the format's: 8-bit Y and CbCr pairs, or the 16-bit ones */
    if (gck->ahb_format == HDRG_VULKAN_AHB_NV12) {
        wanted = (plane == 0) ? HDRG_GC_TEXEL_R8 : HDRG_GC_TEXEL_RG8;
    } else {
        wanted = (plane == 0) ? HDRG_GC_TEXEL_R16 : HDRG_GC_TEXEL_RG16;
    }

    plane_w = (plane == 0) ? gck->ahb_width : gck->ahb_width / 2u;
    plane_h = (plane == 0) ? gck->ahb_height : gck->ahb_height / 2u;

    if (texel != wanted || texels > plane_w || rows > plane_h ||
        gck->ahb_count >= HDRG_GCK_AHB_PLANES) {
        gck->ahb_count = 0;
        (void)hdrg_gck_ahb_flush(gck);

        return HDRG_ERR_INVALID_ARG;
    }

    copy = &gck->ahb_copies[gck->ahb_count++];
    copy->at = at;
    copy->pitch = pitch;
    copy->texels = texels;
    copy->rows = rows;
    copy->plane = plane;
    copy->texel_bytes = hdrg_gc_texel_bytes(texel);

    /* its last plane: the frame's copies run */
    if (plane == 1u) {
        return hdrg_gck_ahb_flush(gck) ? HDRG_OK : HDRG_ERR_INTERNAL;
    }

    return HDRG_OK;
}
#endif

#if defined(HDRG_WITH_METAL)
const hdrg_gc_ops_t hdrg_gc_metal = {
    .kind = HDRG_DEVICE_METAL,
    .open = hdrg_gck_open_metal,
    .close = hdrg_gck_close,
    .buffer = hdrg_gck_buffer,
    .run = hdrg_gck_run,
    .frame = hdrg_gck_frame,
    .take = hdrg_gck_take,
    .import = hdrg_gck_import,
    .surface = hdrg_gck_surface,
};
#endif

#if defined(HDRG_WITH_VULKAN)
const hdrg_gc_ops_t hdrg_gc_vulkan = {
    .kind = HDRG_DEVICE_VULKAN,
    .open = hdrg_gck_open_vulkan,
    .close = hdrg_gck_close,
    .buffer = hdrg_gck_buffer,
    .run = hdrg_gck_run,
    .frame = hdrg_gck_frame,
    .take = hdrg_gck_take,
    .import = hdrg_gck_import,
    .surface = NULL,
    .ahb = hdrg_gck_ahb,
};
#endif
