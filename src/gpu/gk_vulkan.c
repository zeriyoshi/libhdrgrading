/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdint.h>

#include "hdrgrading/video.h"

#include "hdrgrading/gpu/gk.h"
#include "hdrgrading/gpu/vulkan.h"

/* src/gpu/gk.h's backend over src/gpu/vulkan: the SPIR-V of the shaders */

static bool hdrg_gkv_create(const hdrg_plugin_api_t *api, void *device, void *queue, void **out)
{
    return hdrg_vulkan_create(api, (const hdrg_vulkan_device_t *)device, queue,
                              (hdrg_vulkan_t **)out);
}

static void hdrg_gkv_destroy(void *device)
{
    hdrg_vulkan_destroy((hdrg_vulkan_t *)device);
}

static hdrg_gk_buffer_t *hdrg_gkv_buffer(void *device, size_t size)
{
    return (hdrg_gk_buffer_t *)hdrg_vulkan_buffer((hdrg_vulkan_t *)device, size);
}

static hdrg_gk_buffer_t *hdrg_gkv_host_buffer(void *device, size_t size, uint8_t **contents)
{
    return (hdrg_gk_buffer_t *)hdrg_vulkan_host_buffer((hdrg_vulkan_t *)device, size, contents);
}

static hdrg_gk_buffer_t *hdrg_gkv_wrap(void *device, uint32_t kind, void *handle, size_t size)
{
    if (kind == HDRG_FRAME_DMABUF) {
        return (hdrg_gk_buffer_t *)hdrg_vulkan_dmabuf((hdrg_vulkan_t *)device,
                                                      (int)(intptr_t)handle, size);
    }

    if (kind != HDRG_FRAME_BUFFERS) {
        return NULL;
    }

    return (hdrg_gk_buffer_t *)hdrg_vulkan_buffer_wrap((hdrg_vulkan_t *)device, handle, size);
}

static void *hdrg_gkv_buffer_object(const hdrg_gk_buffer_t *buffer)
{
    return hdrg_vulkan_buffer_object((const hdrg_vulkan_buffer_t *)buffer);
}

static size_t hdrg_gkv_buffer_size(const hdrg_gk_buffer_t *buffer)
{
    return hdrg_vulkan_buffer_size((const hdrg_vulkan_buffer_t *)buffer);
}

static void hdrg_gkv_buffer_free(hdrg_gk_buffer_t *buffer)
{
    hdrg_vulkan_buffer_free((hdrg_vulkan_buffer_t *)buffer);
}

static hdrg_gk_kernel_t *hdrg_gkv_kernel(void *device, const hdrg_gk_shader_t *shader,
                                         const char *uniform, const char *const *storage, uint8_t n)
{
    if (shader->spirv == NULL) {
        return NULL;
    }

    return (hdrg_gk_kernel_t *)hdrg_vulkan_kernel((hdrg_vulkan_t *)device, shader->spirv,
                                                  *shader->spirv_size, shader->entry, uniform,
                                                  storage, n);
}

static void hdrg_gkv_kernel_free(hdrg_gk_kernel_t *kernel)
{
    hdrg_vulkan_kernel_free((hdrg_vulkan_kernel_t *)kernel);
}

static hdrg_gk_pass_t *hdrg_gkv_begin(void *device)
{
    return (hdrg_gk_pass_t *)hdrg_vulkan_begin((hdrg_vulkan_t *)device);
}

static void hdrg_gkv_dispatch(hdrg_gk_pass_t *pass, const hdrg_gk_kernel_t *kernel,
                              const void *uniform, size_t uniform_size,
                              hdrg_gk_buffer_t *const *buffers, uint32_t groups_x,
                              uint32_t groups_y, uint32_t group_w, uint32_t group_h)
{
    (void)group_w;
    (void)group_h;

    hdrg_vulkan_dispatch((hdrg_vulkan_pass_t *)pass, (const hdrg_vulkan_kernel_t *)kernel, uniform,
                         uniform_size, (hdrg_vulkan_buffer_t *const *)buffers, groups_x, groups_y);
}

static void hdrg_gkv_copy_buffer(hdrg_gk_pass_t *pass, hdrg_gk_buffer_t *src, hdrg_gk_buffer_t *dst,
                                 size_t size)
{
    hdrg_vulkan_copy_buffer((hdrg_vulkan_pass_t *)pass, (hdrg_vulkan_buffer_t *)src,
                            (hdrg_vulkan_buffer_t *)dst, size);
}

static bool hdrg_gkv_commit(hdrg_gk_pass_t *pass, hdrg_gk_done_fn_t done, void *arg)
{
    return hdrg_vulkan_commit((hdrg_vulkan_pass_t *)pass, done, arg);
}

const hdrg_gk_ops_t hdrg_gk_vulkan = {
    .kind = HDRG_DEVICE_VULKAN,
    .create = hdrg_gkv_create,
    .side = NULL,
    .destroy = hdrg_gkv_destroy,
    .buffer = hdrg_gkv_buffer,
    .host_buffer = hdrg_gkv_host_buffer,
    .wrap = hdrg_gkv_wrap,
    .buffer_object = hdrg_gkv_buffer_object,
    .buffer_size = hdrg_gkv_buffer_size,
    .buffer_free = hdrg_gkv_buffer_free,
    .kernel = hdrg_gkv_kernel,
    .kernel_free = hdrg_gkv_kernel_free,
    .begin = hdrg_gkv_begin,
    .dispatch = hdrg_gkv_dispatch,
    .copy_buffer = hdrg_gkv_copy_buffer,
    .commit = hdrg_gkv_commit,
};
