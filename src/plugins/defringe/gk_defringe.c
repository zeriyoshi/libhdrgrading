/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "parts.h"

#define HDRG_LIT(s)      s, sizeof(s) - 1u
#define HDRG_MD_INFLIGHT 2u /* frames in flight: the next one's encoding meets this one */
#define HDRG_MD_PLANES   5u /* Y', B' - Y', R' - Y', the edges, the dilated edges */
#define HDRG_MD_GROUP    8u /* the shader's workgroups: 8 x 8 */

/*
 * hdrg.defringe (src/plugins/defringe/defringe.c) on Metal and Vulkan
 * (hdrgrading/gpu/filter_gk.h): the WGSL of hdrg.webgpu.defringe
 * (src/plugins/defringe/wgsl/defringe.wgsl) through Tint (src/plugins/defringe/msl/defringe_*,
 * src/plugins/defringe/spirv/defringe_*), the same windows, means and passthrough; the
 * output equals the CPU's. The jobs' RGB16 frames are the filter's host
 * buffers (class_set_frames), read and written by the GPU in place; four
 * dispatches a frame, ended by the pass's completion. ASYNC,
 * HDRG_MD_INFLIGHT frames in flight, each with planes of its own.
 */
typedef struct {
    const hdrg_gk_t *gk;     /* first: the backend's frame_alloc's */
    hdrg_gk_device_t device; /* its frames to the next filter, the others it read */
    hdrg_gk_kernel_t *planes, *edges, *wides, *fringe;
    hdrg_gk_buffer_t *plane[HDRG_MD_INFLIGHT][HDRG_MD_PLANES];
    size_t pixels;                 /* the planes' (0: none yet) */
    uint32_t width, height, reach; /* configure's */
    uint32_t slot;                 /* the next job's planes */
    bool active;
    double strength;
} hdrg_md_t;

/* the shader's uniform (Params) */
typedef struct {
    uint32_t width, height, in_stride, out_stride, reach, pad[3];
} hdrg_md_params_t;

static void hdrg_md_release(hdrg_md_t *md, const hdrg_plugin_api_t *api)
{
    uint8_t i, p;

    hdrg_gk_kernel_free(md->gk, md->planes);
    hdrg_gk_kernel_free(md->gk, md->edges);
    hdrg_gk_kernel_free(md->gk, md->wides);
    hdrg_gk_kernel_free(md->gk, md->fringe);

    for (i = 0; i < HDRG_MD_INFLIGHT; i++) {
        for (p = 0; p < HDRG_MD_PLANES; p++) {
            hdrg_gk_buffer_free(md->gk, md->plane[i][p]);
        }
    }

    hdrg_gk_device_frames_free(md->gk, &md->device);
    hdrg_gk_plugin_release(api, md->gk);
    api->mem_free(api, md);
}

/* the planes of every slot for pixels, kept while they are large enough */
static bool hdrg_md_buffers(hdrg_md_t *md, size_t pixels)
{
    uint8_t i, p;
    bool ok;

    if (pixels <= md->pixels) {
        return true;
    }

    ok = true;

    for (i = 0; i < HDRG_MD_INFLIGHT; i++) {
        for (p = 0; p < HDRG_MD_PLANES; p++) {
            hdrg_gk_buffer_free(md->gk, md->plane[i][p]);
            md->plane[i][p] = hdrg_gk_buffer(md->gk, pixels * 4u);
            ok = ok && md->plane[i][p] != NULL;
        }
    }

    md->pixels = ok ? pixels : 0u;

    return ok;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_md_create(hdrg_filter_t *filter,
                                                     const hdrg_options_t *options, void **inst,
                                                     const hdrg_plugin_api_t *api)
{
    const char *const planes[4] = {"source", "luma", "db", "dr"},
                      *const edges[2] = {"luma", "edge"}, *const wides[2] = {"edge", "wide"},
                      *const fringe[7] = {"source", "luma", "db", "dr", "edge", "wide", "dst"};
    hdrg_md_t *md;
    hdrg_status_t status;

    md = (hdrg_md_t *)api->mem_calloc(api, 1, sizeof(*md));
    if (md == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_float(options, HDRG_LIT("strength"), &md->strength);
    if (status == HDRG_OK) {
        status = api->filter_set_max_inflight(filter, HDRG_MD_INFLIGHT);
    }

    if (status == HDRG_OK) {
        status = hdrg_gk_plugin_device(filter, api, &md->gk);
    }

    if (status == HDRG_OK) {
        status = api->filter_set_device(filter, md->gk->ops->kind);
    }

    if (status == HDRG_OK) {
        md->planes = hdrg_gk_kernel(md->gk, &hdrg_gk_defringe_planes, "params", planes, 4);
        md->edges = hdrg_gk_kernel(md->gk, &hdrg_gk_defringe_edges, "params", edges, 2);
        md->wides = hdrg_gk_kernel(md->gk, &hdrg_gk_defringe_wides, "params", wides, 2);
        md->fringe = hdrg_gk_kernel(md->gk, &hdrg_gk_defringe_fringe, "params", fringe, 7);
        status =
            (md->planes != NULL && md->edges != NULL && md->wides != NULL && md->fringe != NULL)
                ? HDRG_OK
                : HDRG_ERR_UNSUPPORTED;
    }

    if (status != HDRG_OK) {
        hdrg_md_release(md, api);

        return status;
    }

    *inst = md;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL hdrg_md_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    hdrg_md_release((hdrg_md_t *)inst, api);
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_md_configure(void *inst, hdrg_negotiation_t *nego,
                                                        const hdrg_plugin_api_t *api)
{
    hdrg_md_t *md = (hdrg_md_t *)inst;
    hdrg_status_t status;
    uint32_t format;

    status = api->nego_input(nego, &md->width, &md->height, &format);
    if (status != HDRG_OK) {
        return status;
    }

    if (format != HDRG_FORMAT_RGB16 || md->width == 0 || md->height == 0) {
        return HDRG_ERR_UNSUPPORTED;
    }

    status = hdrg_defringe_settings(nego, api, md->strength, &md->reach, &md->active);
    if (status != HDRG_OK) {
        return status;
    }

    status = api->nego_set_output(nego, md->width, md->height, HDRG_FORMAT_RGB16);

    return (status == HDRG_OK && !md->active) ? api->nego_set_passthrough(nego) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL hdrg_md_submit(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    hdrg_md_t *md = (hdrg_md_t *)inst;
    hdrg_gk_buffer_t *buffers[7], *edge_pair[2];
    hdrg_md_params_t params;
    hdrg_gk_pass_t *pass;
    hdrg_gk_job_t frames;
    hdrg_status_t status;
    uint32_t groups_x, groups_y;

    status = hdrg_gk_job_frames(md->gk, &md->device, job, api, md->width, md->height, 6u, &frames);
    if (status != HDRG_OK) {
        return status;
    }

    if (!hdrg_md_buffers(md, (size_t)md->width * md->height)) {
        return HDRG_ERR_NOMEM;
    }

    pass = hdrg_gk_begin(md->gk);
    if (pass == NULL) {
        return HDRG_ERR_INTERNAL;
    }

    memset(&params, 0, sizeof(params));
    params.width = md->width;
    params.height = md->height;
    params.in_stride = (uint32_t)(frames.in_stride / 4u);
    params.out_stride = (uint32_t)(frames.out_stride / 4u);
    params.reach = md->reach;
    buffers[0] = frames.in;
    buffers[1] = md->plane[md->slot][0];
    buffers[2] = md->plane[md->slot][1];
    buffers[3] = md->plane[md->slot][2];
    buffers[4] = md->plane[md->slot][3];
    buffers[5] = md->plane[md->slot][4];
    buffers[6] = frames.out;
    edge_pair[0] = buffers[1];
    edge_pair[1] = buffers[4];
    md->slot = (md->slot + 1u) % HDRG_MD_INFLIGHT;
    groups_x = (md->width + HDRG_MD_GROUP - 1u) / HDRG_MD_GROUP;
    groups_y = (md->height + HDRG_MD_GROUP - 1u) / HDRG_MD_GROUP;

    hdrg_gk_dispatch(md->gk, pass, md->planes, &params, sizeof(params), buffers, groups_x, groups_y,
                     HDRG_MD_GROUP, HDRG_MD_GROUP);
    hdrg_gk_dispatch(md->gk, pass, md->edges, &params, sizeof(params), edge_pair, groups_x,
                     groups_y, HDRG_MD_GROUP, HDRG_MD_GROUP);
    hdrg_gk_dispatch(md->gk, pass, md->wides, &params, sizeof(params), buffers + 4, groups_x,
                     groups_y, HDRG_MD_GROUP, HDRG_MD_GROUP);
    hdrg_gk_dispatch(md->gk, pass, md->fringe, &params, sizeof(params), buffers,
                     ((md->width + 1u) / 2u + HDRG_MD_GROUP - 1u) / HDRG_MD_GROUP, groups_y,
                     HDRG_MD_GROUP, HDRG_MD_GROUP);

    return hdrg_gk_job_commit(md->gk, pass, job, api, &frames) ? HDRG_OK : HDRG_ERR_INTERNAL;
}

extern hdrg_status_t hdrg_gk_defringe_register(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                               const hdrg_gk_backend_t *backend)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = hdrg_gk_class(plugin, api, backend, "defringe");
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_add_option_float(
        cls, HDRG_LIT("strength"), 3.0, 0.0, 3.0,
        HDRG_LIT("sets the window's radius, 1 + 2 x strength (0 = off)"));
    if (status == HDRG_OK) {
        status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    }

    if (status == HDRG_OK) {
        status = api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_ASYNC |
                                               HDRG_FILTER_GPU_FRAMES);
    }

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, hdrg_md_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, hdrg_md_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, hdrg_md_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_submit(cls, hdrg_md_submit);
    }

    if (status == HDRG_OK) {
        status = api->class_set_frames(cls, backend->frame_alloc, backend->frame_free);
    }

    if (status == HDRG_OK) {
        status = api->class_set_device_release(cls, hdrg_gk_device_release);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}
