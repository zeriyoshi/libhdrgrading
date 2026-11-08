/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "stats.h"

#include <string.h>

#define HDRG_STATS_MAGIC UINT32_C(0x48535441)

extern hdrg_status_t hdrg_stats_open(const hdrg_filter_entry_t *entry, hdrg_plugin_host_t *host,
                                     const hdrg_stats_session_t *info, hdrg_stats_stage_t *out)
{
    const hdrg_class_t *cls = entry->cls;
    hdrg_status_t status;

    memset(out, 0, sizeof(*out));

    if (cls->stats_render == NULL) {
        hdrg_host_set_error(host, "the filter keeps no statistics", cls->id.data, cls->id.size);

        return HDRG_ERR_INVALID_ARG;
    }

    out->filter.magic = HDRG_FILTER_MAGIC;
    out->filter.cls = cls;
    out->filter.host = host;
    out->filter.stats = info;
    out->filter.stage = HDRG_STAGE_STATS;
    out->cls = cls;
    out->filter.creating = true;

    status = cls->create(&out->filter, entry->options, &out->inst, hdrg_class_api(cls));

    out->filter.creating = false;

    /* a session's records refused (a first pass's) stay so */
    if (status != HDRG_OK) {
        hdrg_host_set_error(host, "filter create failed", cls->id.data, cls->id.size);
        memset(out, 0, sizeof(*out));

        return (status == HDRG_ERR_UNSUPPORTED || status == HDRG_ERR_INVALID_ARG ||
                status == HDRG_ERR_NOMEM)
                   ? status
                   : HDRG_ERR_PLUGIN;
    }

    out->created = true;

    return HDRG_OK;
}

extern void hdrg_stats_close(hdrg_stats_stage_t *stage)
{
    if (stage->created) {
        stage->cls->destroy(stage->inst, hdrg_class_api(stage->cls));
    }

    memset(stage, 0, sizeof(*stage));
}

extern void hdrg_stats_window(const hdrg_stats_stage_t *stage, uint32_t latency, uint32_t *past,
                              uint32_t *future)
{
    *past = 0;
    *future = 0;

    if (stage->created) {
        stage->cls->stats_window(stage->inst, latency, past, future, hdrg_class_api(stage->cls));
    }
}

extern hdrg_status_t hdrg_stats_fade(hdrg_stats_stage_t *stage, const hdrg_stats_sent_t *sent,
                                     hdrg_stats_fade_t *out)
{
    hdrg_status_t status;

    memset(out, 0, sizeof(*out));
    out->from = sent->index;
    out->cut = sent->cut;
    out->dim = 1.0f;

    status = stage->cls->stats_fade(stage->inst, sent, out, hdrg_class_api(stage->cls));

    /* a run back past the stream's frames, or a dimming that brightens: none */
    if (status == HDRG_OK && (out->run > sent->index || out->from != sent->index - out->run ||
                              !(out->dim >= 0.0f && out->dim <= 1.0f))) {
        status = HDRG_ERR_PLUGIN;
    }

    return (status == HDRG_OK || status == HDRG_ERR_NOMEM) ? status : HDRG_ERR_PLUGIN;
}

extern hdrg_status_t hdrg_stats_frame(hdrg_stats_stage_t *stage, const hdrg_stats_frame_t *frame)
{
    hdrg_status_t status;

    status = stage->cls->stats_frame(stage->inst, frame, hdrg_class_api(stage->cls));

    return (status == HDRG_OK || status == HDRG_ERR_NOMEM) ? status : HDRG_ERR_PLUGIN;
}

extern hdrg_status_t hdrg_stats_render(hdrg_stats_stage_t *stage, hdrg_stats_head_t *head,
                                       hdrg_stats_queued_fn_t queued, void *user)
{
    hdrg_rendering_t stats;
    hdrg_status_t status;

    memset(&stats, 0, sizeof(stats));
    stats.magic = HDRG_STATS_MAGIC;
    stats.head = head;
    stats.queued = queued;
    stats.user = user;

    status = stage->cls->stats_render(stage->inst, &stats, hdrg_class_api(stage->cls));

    stats.magic = 0;

    return (status == HDRG_OK || status == HDRG_ERR_NOMEM) ? status : HDRG_ERR_PLUGIN;
}

extern bool hdrg_stats_fit(hdrg_stats_stage_t *stage, uint32_t index, float fit, float *used)
{
    uint8_t replace;

    replace = 0;
    *used = fit;
    stage->cls->stats_fit(stage->inst, index, fit, &replace, used, hdrg_class_api(stage->cls));

    return replace != 0;
}

extern void hdrg_stats_rendered(hdrg_stats_stage_t *stage, uint32_t index, float anchor, float key,
                                float fit)
{
    stage->cls->stats_rendered(stage->inst, index, anchor, key, fit, hdrg_class_api(stage->cls));
}

extern hdrg_status_t HDRG_PLUGIN_CALL
hdrg_api_filter_stats_session(const hdrg_filter_t *filter, const hdrg_stats_session_t **session)
{
    if (filter == NULL || filter->magic != HDRG_FILTER_MAGIC || session == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *session = filter->stats;

    return (filter->stats != NULL) ? HDRG_OK : HDRG_ERR_NOT_FOUND;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_stats_head(hdrg_rendering_t *stats,
                                                          hdrg_stats_head_t **head)
{
    if (stats == NULL || stats->magic != HDRG_STATS_MAGIC || head == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    *head = stats->head;

    return HDRG_OK;
}

extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_stats_queued(hdrg_rendering_t *stats, uint32_t k,
                                                            const hdrg_stats_frame_t **frame)
{
    hdrg_status_t status;

    if (stats == NULL || stats->magic != HDRG_STATS_MAGIC || frame == NULL || k == 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    *frame = NULL;

    /* past the queue's room: none queued there */
    if (k > HDRG_SESSION_MAX_WINDOW) {
        return HDRG_ERR_NOT_FOUND;
    }

    status = stats->queued(stats->user, k, &stats->frames[k]);
    if (status == HDRG_OK) {
        *frame = &stats->frames[k];
    }

    return status;
}
