/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)     s, sizeof(s) - 1u
#define HDRG_IP_ID      "hdrg.interp"
#define HDRG_IP_REPEAT  "hdrg.interp.repeat"
#define HDRG_IP_BLEND   "hdrg.interp.blend"
#define HDRG_IP_LICENSE "Apache-2.0 OR MIT"
#define HDRG_IP_COPY    "Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>"
#define HDRG_IP_HALF    32768u /* the phase halfway */
#define HDRG_IP_ONE     65536u

/*
 * The built-in INTERP filters (a video session at another output
 * rate): hdrg.interp.repeat gives the nearer frame (a phase of a
 * half: the next one), hdrg.interp.blend the frames' mix by the phase, in
 * integers; hdrg.interp.mc (src/plugins/interp/interp_mc.c, HDRG_TEMPORAL_INTERP)
 * compensates the motion. Whole RGB16 frames: the job's frame and the next
 * one (job_input_frame offset 1).
 */
typedef struct {
    uint32_t width, height; /* configure's */
} hdrg_ip_t;

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ip_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    *inst = api->mem_calloc(api, 1, sizeof(hdrg_ip_t));

    return (*inst != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
}

static void HDRG_PLUGIN_CALL hdrg_ip_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    api->mem_free(api, inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ip_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_ip_t *ip = (hdrg_ip_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &ip->width, &ip->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || ip->width == 0 || ip->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    return api->nego_set_output(nego, ip->width, ip->height, HDRG_FORMAT_RGB16);
}

/* the job's frames (a: its own, b: the next one), its output and its phase */
static hdrg_status_t hdrg_ip_frames(hdrg_job_t *job, const hdrg_plugin_api_t *api,
                                    const uint8_t **a, const uint8_t **b, size_t *a_stride,
                                    size_t *b_stride, uint8_t **out, size_t *out_stride,
                                    uint32_t *phase)
{
    hdrg_status_t status;
    uint32_t begin, end;

    status = api->job_input(job, 0, a, a_stride, &begin, &end);
    if (status == HDRG_OK) {
        status = api->job_input_frame(job, 1, 0, b, b_stride, &begin, &end);
    }

    if (status == HDRG_OK) {
        status = api->job_output(job, 0, out, out_stride);
    }

    if (status == HDRG_OK) {
        status = api->job_phase(job, phase);
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ip_repeat(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const hdrg_ip_t *ip = (const hdrg_ip_t *)inst;
    const uint8_t *a, *b, *near;
    hdrg_status_t status;
    uint32_t phase, y;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride, near_stride;

    status = hdrg_ip_frames(job, api, &a, &b, &a_stride, &b_stride, &out, &out_stride, &phase);
    if (status != HDRG_OK) {
        return status;
    }

    near = (phase < HDRG_IP_HALF) ? a : b;
    near_stride = (phase < HDRG_IP_HALF) ? a_stride : b_stride;

    for (y = 0; y < ip->height; y++) {
        memcpy(out + (size_t)y * out_stride, near + (size_t)y * near_stride,
               (size_t)ip->width * 3u * sizeof(uint16_t));
    }

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_ip_blend(void *inst, hdrg_job_t *job,
                                                    const hdrg_plugin_api_t *api)
{
    const hdrg_ip_t *ip = (const hdrg_ip_t *)inst;
    const uint16_t *ra, *rb;
    const uint8_t *a, *b;
    hdrg_status_t status;
    uint32_t phase, y, i;
    uint16_t *ro;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride;

    status = hdrg_ip_frames(job, api, &a, &b, &a_stride, &b_stride, &out, &out_stride, &phase);
    if (status != HDRG_OK) {
        return status;
    }

    for (y = 0; y < ip->height; y++) {
        ra = (const uint16_t *)(const void *)(a + (size_t)y * a_stride);
        rb = (const uint16_t *)(const void *)(b + (size_t)y * b_stride);
        ro = (uint16_t *)(void *)(out + (size_t)y * out_stride);

        for (i = 0; i < ip->width * 3u; i++) {
            ro[i] = (uint16_t)(((uint64_t)ra[i] * (HDRG_IP_ONE - phase) + (uint64_t)rb[i] * phase +
                                HDRG_IP_HALF) >>
                               16u);
        }
    }

    return HDRG_OK;
}

/* a class of the plugin: INTERP, whole frames, its process */
static hdrg_status_t hdrg_ip_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                   const char *id, size_t id_len, hdrg_filter_process_fn_t process)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, id_len);
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_INTERP);
    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_DETERMINISTIC |
                                               HDRG_FILTER_NO_MOTION);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_ip_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_ip_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_ip_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_filter_interp_init(hdrg_plugin_t *plugin,
                                                              const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, HDRG_LIT(HDRG_IP_ID), HDRG_LIT("Frame interpolation"),
                                  HDRG_LIT("1.0.0"), HDRG_LIT("libhdrgrading"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, HDRG_LIT(HDRG_IP_LICENSE), HDRG_LIT(HDRG_IP_COPY));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    if (status == HDRG_OK) {
        status = hdrg_ip_class(plugin, api, HDRG_LIT(HDRG_IP_REPEAT), hdrg_ip_repeat);
    }

    if (status == HDRG_OK) {
        status = hdrg_ip_class(plugin, api, HDRG_LIT(HDRG_IP_BLEND), hdrg_ip_blend);
    }

#if defined(HDRG_INTERP_MC)
    if (status == HDRG_OK) {
        status = hdrg_interp_mc_class(plugin, api);
    }
#endif

#if defined(HDRG_INTERP_MC) && defined(HDRG_WITH_WEBGPU)
    if (status == HDRG_OK) {
        status = hdrg_webgpu_interp_register(plugin, api);
    }
#endif

#if defined(HDRG_INTERP_MC) && defined(HDRG_WITH_METAL)
    if (status == HDRG_OK) {
        status = hdrg_gk_interp_register(plugin, api, &hdrg_gk_metal_backend);
    }
#endif

#if defined(HDRG_INTERP_MC) && defined(HDRG_WITH_VULKAN)
    if (status == HDRG_OK) {
        status = hdrg_gk_interp_register(plugin, api, &hdrg_gk_vulkan_backend);
    }
#endif

    return status;
}
