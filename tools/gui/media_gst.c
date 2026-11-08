/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * Linux: GStreamer (uridecodebin: the platform's decoders, VA-API among them). A first pipeline
 * with any raw caps tells the stream (its rate and colorimetry); the second one converts to
 * RGBA (a still, copied once) or P010 (a video: 8-bit sources widened)
 */

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

/* the time a pull waits (ns) before it reads the bus */
#define HDRG_GUI_PULL ((GstClockTime)200000000u)

struct hdrg_gui_media {
    GstElement *pipeline, *sink;
    hdrg_gui_info_t info;
    uint8_t *still;
    bool still_read;
};

/* a frame handed out: its sample and the mapping */
typedef struct {
    GstSample *sample;
    GstVideoFrame frame;
} hdrg_gui_mapped_t;

static GstElement *hdrg_gui_pipeline(const char *uri, const char *caps, GstElement **sink)
{
    GstElement *pipeline;
    GError *failure;
    gchar *description;

    failure = NULL;
    description = g_strdup_printf("uridecodebin uri=\"%s\" ! videoconvert ! appsink name=sink "
                                  "sync=false max-buffers=4 caps=\"%s\"",
                                  uri, caps);
    pipeline = gst_parse_launch(description, &failure);
    g_free(description);

    if (failure != NULL) {
        g_error_free(failure);

        if (pipeline != NULL) {
            gst_object_unref(pipeline);
        }

        return NULL;
    }

    *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    return pipeline;
}

/* PAUSED, prerolled (false: an error or no video) */
static bool hdrg_gui_preroll(GstElement *pipeline)
{
    GstStateChangeReturn state;

    state = gst_element_set_state(pipeline, GST_STATE_PAUSED);
    if (state == GST_STATE_CHANGE_FAILURE) {
        return false;
    }

    state = gst_element_get_state(pipeline, NULL, NULL, 10 * GST_SECOND);

    return state == GST_STATE_CHANGE_SUCCESS || state == GST_STATE_CHANGE_NO_PREROLL;
}

static void hdrg_gui_drop(GstElement **pipeline, GstElement **sink)
{
    if (*pipeline != NULL) {
        gst_element_set_state(*pipeline, GST_STATE_NULL);
        gst_object_unref(*pipeline);
        *pipeline = NULL;
    }

    if (*sink != NULL) {
        gst_object_unref(*sink);
        *sink = NULL;
    }
}

/* the stream's caps from a pipeline of any raw video */
static bool hdrg_gui_probe(const char *uri, GstVideoInfo *video)
{
    GstElement *pipeline, *sink;
    GstSample *sample;
    GstCaps *caps;
    bool ok;

    sink = NULL;
    pipeline = hdrg_gui_pipeline(uri, "video/x-raw", &sink);
    ok = pipeline != NULL && sink != NULL && hdrg_gui_preroll(pipeline);
    sample = ok ? gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 5 * GST_SECOND) : NULL;
    caps = (sample != NULL) ? gst_sample_get_caps(sample) : NULL;
    ok = caps != NULL && gst_video_info_from_caps(video, caps);

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    hdrg_gui_drop(&pipeline, &sink);

    return ok;
}

static uint8_t hdrg_gui_matrix_of(GstVideoColorMatrix matrix)
{
    if (matrix == GST_VIDEO_COLOR_MATRIX_BT601 || matrix == GST_VIDEO_COLOR_MATRIX_SMPTE240M) {
        return HDRG_MATRIX_BT601;
    }

    if (matrix == GST_VIDEO_COLOR_MATRIX_BT2020) {
        return HDRG_MATRIX_BT2020;
    }

    return HDRG_MATRIX_BT709;
}

static uint8_t hdrg_gui_site_of(GstVideoChromaSite site)
{
    if (site == GST_VIDEO_CHROMA_SITE_JPEG) {
        return HDRG_CHROMA_CENTER;
    }

    if (site == GST_VIDEO_CHROMA_SITE_COSITED) {
        return HDRG_CHROMA_TOPLEFT;
    }

    return HDRG_CHROMA_LEFT;
}

/* a still's one frame copied out of the pipeline */
static bool hdrg_gui_copy_still(hdrg_gui_media_t *media)
{
    hdrg_gui_mapped_t mapped;
    GstVideoInfo video;
    uint32_t y;
    size_t row;

    mapped.sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(media->sink), 5 * GST_SECOND);
    if (mapped.sample == NULL ||
        !gst_video_info_from_caps(&video, gst_sample_get_caps(mapped.sample)) ||
        !gst_video_frame_map(&mapped.frame, &video, gst_sample_get_buffer(mapped.sample),
                             GST_MAP_READ)) {
        if (mapped.sample != NULL) {
            gst_sample_unref(mapped.sample);
        }

        return false;
    }

    row = (size_t)media->info.width * 4u;
    media->still = (uint8_t *)malloc(row * media->info.height);

    for (y = 0; media->still != NULL && y < media->info.height; y++) {
        memcpy(media->still + (size_t)y * row,
               (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&mapped.frame, 0) +
                   (size_t)y * (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&mapped.frame, 0),
               row);
    }

    gst_video_frame_unmap(&mapped.frame);
    gst_sample_unref(mapped.sample);
    hdrg_gui_drop(&media->pipeline, &media->sink);

    return media->still != NULL;
}

extern bool hdrg_gui_media_open(const char *path, hdrg_gui_media_t **out, hdrg_gui_info_t *info,
                                char *error, size_t error_size)
{
    hdrg_gui_media_t *media;
    GstVideoInfo probed;
    GError *failure;
    gint64 duration;
    gchar *uri;
    bool still;

    *out = NULL;
    gst_init(NULL, NULL);
    failure = NULL;
    uri = gst_filename_to_uri(path, &failure);
    if (uri == NULL) {
        snprintf(error, error_size, "%s", (failure != NULL) ? failure->message : "bad path");
        g_clear_error(&failure);

        return false;
    }

    if (!hdrg_gui_probe(uri, &probed)) {
        snprintf(error, error_size, "not a still or a video GStreamer decodes");
        g_free(uri);

        return false;
    }

    if (probed.colorimetry.transfer == GST_VIDEO_TRANSFER_SMPTE2084 ||
        probed.colorimetry.transfer == GST_VIDEO_TRANSFER_ARIB_STD_B67) {
        snprintf(error, error_size, "the video is already HDR (PQ / HLG)");
        g_free(uri);

        return false;
    }

    media = (hdrg_gui_media_t *)calloc(1, sizeof(*media));
    if (media == NULL) {
        snprintf(error, error_size, "out of memory");
        g_free(uri);

        return false;
    }

    still = GST_VIDEO_INFO_FPS_N(&probed) == 0;
    media->info.width = (uint32_t)GST_VIDEO_INFO_WIDTH(&probed);
    media->info.height = (uint32_t)GST_VIDEO_INFO_HEIGHT(&probed);
    media->info.primaries = HDRG_PRIMARIES_BT709;
    media->info.transfer = still ? HDRG_TRANSFER_SRGB : HDRG_TRANSFER_BT709;
    media->info.format = still ? HDRG_GUI_RGBA : HDRG_GUI_P010;
    media->info.matrix = hdrg_gui_matrix_of(probed.colorimetry.matrix);
    media->info.full_range = probed.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    media->info.chroma_loc = hdrg_gui_site_of(probed.chroma_site);

    if (!still) {
        media->info.rate_num = (uint32_t)GST_VIDEO_INFO_FPS_N(&probed);
        media->info.rate_den = (uint32_t)GST_VIDEO_INFO_FPS_D(&probed);
    }

    media->pipeline = hdrg_gui_pipeline(
        uri, still ? "video/x-raw,format=RGBA" : "video/x-raw,format=P010_10LE", &media->sink);
    g_free(uri);

    if (media->pipeline == NULL || media->sink == NULL || !hdrg_gui_preroll(media->pipeline) ||
        (still && !hdrg_gui_copy_still(media))) {
        snprintf(error, error_size, "the file cannot be decoded");
        hdrg_gui_media_close(media);

        return false;
    }

    if (!still) {
        if (gst_element_query_duration(media->pipeline, GST_FORMAT_TIME, &duration)) {
            media->info.duration_us = duration / 1000;
        }

        gst_element_set_state(media->pipeline, GST_STATE_PLAYING);
    }

    *info = media->info;
    *out = media;

    return true;
}

extern uint8_t hdrg_gui_media_read(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    hdrg_gui_mapped_t *mapped;
    GstBuffer *buffer;
    GstVideoInfo video;

    memset(frame, 0, sizeof(*frame));

    if (media->still != NULL) {
        if (media->still_read) {
            return HDRG_GUI_READ_END;
        }

        media->still_read = true;
        frame->planes[0] = media->still;
        frame->strides[0] = (size_t)media->info.width * 4u;

        return HDRG_GUI_READ_FRAME;
    }

    mapped = (hdrg_gui_mapped_t *)calloc(1, sizeof(*mapped));
    if (mapped == NULL) {
        return HDRG_GUI_READ_ERROR;
    }

    while ((mapped->sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(media->sink), HDRG_GUI_PULL)) == NULL) {
        if (gst_app_sink_is_eos(GST_APP_SINK(media->sink))) {
            free(mapped);

            return HDRG_GUI_READ_END;
        }
    }

    buffer = gst_sample_get_buffer(mapped->sample);
    /* the session reads the size and format it was made with: another frame is refused */
    if (!gst_video_info_from_caps(&video, gst_sample_get_caps(mapped->sample)) ||
        GST_VIDEO_INFO_WIDTH(&video) != (gint)media->info.width ||
        GST_VIDEO_INFO_HEIGHT(&video) != (gint)media->info.height ||
        GST_VIDEO_INFO_FORMAT(&video) != GST_VIDEO_FORMAT_P010_10LE ||
        !gst_video_frame_map(&mapped->frame, &video, buffer, GST_MAP_READ)) {
        gst_sample_unref(mapped->sample);
        free(mapped);

        return HDRG_GUI_READ_ERROR;
    }

    frame->planes[0] = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&mapped->frame, 0);
    frame->planes[1] = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&mapped->frame, 1);
    frame->strides[0] = (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&mapped->frame, 0);
    frame->strides[1] = (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&mapped->frame, 1);
    frame->pts_us = GST_BUFFER_PTS_IS_VALID(buffer) ? (int64_t)(GST_BUFFER_PTS(buffer) / 1000u) : 0;
    frame->handle = mapped;

    return HDRG_GUI_READ_FRAME;
}

extern void hdrg_gui_media_release(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    hdrg_gui_mapped_t *mapped;

    (void)media;
    mapped = (hdrg_gui_mapped_t *)frame->handle;

    if (mapped != NULL) {
        gst_video_frame_unmap(&mapped->frame);
        gst_sample_unref(mapped->sample);
        free(mapped);
        frame->handle = NULL;
    }
}

extern bool hdrg_gui_media_seek(hdrg_gui_media_t *media, int64_t pts_us)
{
    if (media->still != NULL) {
        media->still_read = false;

        return true;
    }

    return gst_element_seek_simple(media->pipeline, GST_FORMAT_TIME,
                                   (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                   (gint64)((pts_us > 0) ? pts_us : 0) * 1000);
}

extern void hdrg_gui_media_close(hdrg_gui_media_t *media)
{
    if (media == NULL) {
        return;
    }

    hdrg_gui_drop(&media->pipeline, &media->sink);
    free(media->still);
    free(media);
}
