/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/video.h"

#include "hdrgrading/gpu/gk.h"
#include "hdrgrading/gpu/metal.h"

/* src/gpu/gk.h's backend over src/gpu/metal: the MSL of the shaders */

static bool hdrg_gkm_create(const hdrg_plugin_api_t *api, void *device, void *queue, void **out)
{
    return hdrg_metal_create(api, device, queue, (hdrg_metal_t **)out);
}

static bool hdrg_gkm_side(const hdrg_plugin_api_t *api, void *device, void **out)
{
    return hdrg_metal_create(api, hdrg_metal_device_object((const hdrg_metal_t *)device), NULL,
                             (hdrg_metal_t **)out);
}

static void hdrg_gkm_destroy(void *device)
{
    hdrg_metal_destroy((hdrg_metal_t *)device);
}

static hdrg_gk_buffer_t *hdrg_gkm_buffer(void *device, size_t size)
{
    return (hdrg_gk_buffer_t *)hdrg_metal_buffer((hdrg_metal_t *)device, size);
}

static hdrg_gk_buffer_t *hdrg_gkm_host_buffer(void *device, size_t size, uint8_t **contents)
{
    return (hdrg_gk_buffer_t *)hdrg_metal_host_buffer((hdrg_metal_t *)device, size, contents);
}

static hdrg_gk_buffer_t *hdrg_gkm_wrap(void *device, uint32_t kind, void *handle, size_t size)
{
    (void)size;

    if (kind != HDRG_FRAME_BUFFERS) {
        return NULL;
    }

    return (hdrg_gk_buffer_t *)hdrg_metal_buffer_wrap((hdrg_metal_t *)device, handle);
}

static void *hdrg_gkm_buffer_object(const hdrg_gk_buffer_t *buffer)
{
    return hdrg_metal_buffer_object((const hdrg_metal_buffer_t *)buffer);
}

static size_t hdrg_gkm_buffer_size(const hdrg_gk_buffer_t *buffer)
{
    return hdrg_metal_buffer_size((const hdrg_metal_buffer_t *)buffer);
}

static void hdrg_gkm_buffer_free(hdrg_gk_buffer_t *buffer)
{
    hdrg_metal_buffer_free((hdrg_metal_buffer_t *)buffer);
}

static hdrg_gk_kernel_t *hdrg_gkm_kernel(void *device, const hdrg_gk_shader_t *shader,
                                         const char *uniform, const char *const *storage, uint8_t n)
{
    if (shader->msl == NULL) {
        return NULL;
    }

    return (hdrg_gk_kernel_t *)hdrg_metal_kernel((hdrg_metal_t *)device, shader->msl, shader->entry,
                                                 uniform, storage, n);
}

static void hdrg_gkm_kernel_free(hdrg_gk_kernel_t *kernel)
{
    hdrg_metal_kernel_free((hdrg_metal_kernel_t *)kernel);
}

static hdrg_gk_pass_t *hdrg_gkm_begin(void *device)
{
    return (hdrg_gk_pass_t *)hdrg_metal_begin((hdrg_metal_t *)device);
}

static void hdrg_gkm_dispatch(hdrg_gk_pass_t *pass, const hdrg_gk_kernel_t *kernel,
                              const void *uniform, size_t uniform_size,
                              hdrg_gk_buffer_t *const *buffers, uint32_t groups_x,
                              uint32_t groups_y, uint32_t group_w, uint32_t group_h)
{
    hdrg_metal_dispatch((hdrg_metal_pass_t *)pass, (const hdrg_metal_kernel_t *)kernel, uniform,
                        uniform_size, (hdrg_metal_buffer_t *const *)buffers, groups_x, groups_y,
                        group_w, group_h);
}

static void hdrg_gkm_copy_buffer(hdrg_gk_pass_t *pass, hdrg_gk_buffer_t *src, hdrg_gk_buffer_t *dst,
                                 size_t size)
{
    hdrg_metal_copy_buffer((hdrg_metal_pass_t *)pass, (hdrg_metal_buffer_t *)src,
                           (hdrg_metal_buffer_t *)dst, size);
}

static bool hdrg_gkm_commit(hdrg_gk_pass_t *pass, hdrg_gk_done_fn_t done, void *arg)
{
    return hdrg_metal_commit((hdrg_metal_pass_t *)pass, done, arg);
}

const hdrg_gk_ops_t hdrg_gk_metal = {
    .kind = HDRG_DEVICE_METAL,
    .create = hdrg_gkm_create,
    .side = hdrg_gkm_side,
    .destroy = hdrg_gkm_destroy,
    .buffer = hdrg_gkm_buffer,
    .host_buffer = hdrg_gkm_host_buffer,
    .wrap = hdrg_gkm_wrap,
    .buffer_object = hdrg_gkm_buffer_object,
    .buffer_size = hdrg_gkm_buffer_size,
    .buffer_free = hdrg_gkm_buffer_free,
    .kernel = hdrg_gkm_kernel,
    .kernel_free = hdrg_gkm_kernel_free,
    .begin = hdrg_gkm_begin,
    .dispatch = hdrg_gkm_dispatch,
    .copy_buffer = hdrg_gkm_copy_buffer,
    .commit = hdrg_gkm_commit,
};
