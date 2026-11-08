/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_STATS_H
#define HDRG_HOST_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host.h"

/*
 * The STATS stage: a STATS class's instance in a video session, the
 * statistics across its frames (its temporal mode)
 */
typedef struct {
    hdrg_filter_t filter;
    const hdrg_class_t *cls;
    void *inst;
    bool created;
} hdrg_stats_stage_t;

/* the session's queued frames a render reads: the k-th after the head into *out */
typedef hdrg_status_t (*hdrg_stats_queued_fn_t)(void *user, uint32_t k, hdrg_stats_frame_t *out);

/* a frame rendering (the API's stats_*) */
struct hdrg_rendering {
    uint32_t magic;
    hdrg_stats_head_t *head;
    hdrg_stats_queued_fn_t queued;
    void *user;
    hdrg_stats_frame_t frames[HDRG_SESSION_MAX_WINDOW + 1u]; /* stats_queued's, by k */
};

/*
 * entry's instance in the session info describes (kept: it outlives the
 * instance); *out zeroed first, closing it is safe. HDRG_ERR_UNSUPPORTED,
 * INVALID_ARG and NOMEM kept
 */
extern hdrg_status_t hdrg_stats_open(const hdrg_filter_entry_t *entry, hdrg_plugin_host_t *host,
                                     const hdrg_stats_session_t *info, hdrg_stats_stage_t *out);
extern void hdrg_stats_close(hdrg_stats_stage_t *stage);
extern void hdrg_stats_window(const hdrg_stats_stage_t *stage, uint32_t latency, uint32_t *past,
                              uint32_t *future);
extern hdrg_status_t hdrg_stats_fade(hdrg_stats_stage_t *stage, const hdrg_stats_sent_t *sent,
                                     hdrg_stats_fade_t *out);
extern hdrg_status_t hdrg_stats_frame(hdrg_stats_stage_t *stage, const hdrg_stats_frame_t *frame);
/* the head rendering, the queue's frames after it through queued */
extern hdrg_status_t hdrg_stats_render(hdrg_stats_stage_t *stage, hdrg_stats_head_t *head,
                                       hdrg_stats_queued_fn_t queued, void *user);
extern bool hdrg_stats_fit(hdrg_stats_stage_t *stage, uint32_t index, float fit, float *used);
extern void hdrg_stats_rendered(hdrg_stats_stage_t *stage, uint32_t index, float anchor, float key,
                                float fit);
extern hdrg_status_t HDRG_PLUGIN_CALL
hdrg_api_filter_stats_session(const hdrg_filter_t *filter, const hdrg_stats_session_t **session);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_stats_head(hdrg_rendering_t *stats,
                                                          hdrg_stats_head_t **head);
extern hdrg_status_t HDRG_PLUGIN_CALL hdrg_api_stats_queued(hdrg_rendering_t *stats, uint32_t k,
                                                            const hdrg_stats_frame_t **frame);

#endif /* HDRG_HOST_STATS_H */
