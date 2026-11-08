/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * FFmpeg (HDRG_GUI_MEDIA ffmpeg, Linux): libavformat and libavcodec, the frames repacked by
 * libswscale into NV12, P010 (a source above 8 bits) or RGBA (a still: an image demuxer); a
 * decoder's NV12 or P010 frames as they are, the repacked frames' kept for the next ones
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define HDRG_GUI_SPARES 4u /* the repacked frames kept for the next ones at most */

struct hdrg_gui_media {
    AVFormatContext *format;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *decoded;
    AVFrame *spares[HDRG_GUI_SPARES]; /* repacked frames released, their buffers kept */
    struct SwsContext *scale;
    hdrg_gui_info_t info;
    uint32_t spare_count;
    int32_t stream;
    bool draining, still_read;
    enum AVPixelFormat target;
};

static uint8_t hdrg_gui_matrix_av(enum AVColorSpace space)
{
    if (space == AVCOL_SPC_BT470BG || space == AVCOL_SPC_SMPTE170M) {
        return HDRG_MATRIX_BT601;
    }

    if (space == AVCOL_SPC_BT2020_NCL) {
        return HDRG_MATRIX_BT2020;
    }

    return HDRG_MATRIX_BT709;
}

static uint8_t hdrg_gui_site_av(enum AVChromaLocation location)
{
    if (location == AVCHROMA_LOC_CENTER) {
        return HDRG_CHROMA_CENTER;
    }

    if (location == AVCHROMA_LOC_TOPLEFT) {
        return HDRG_CHROMA_TOPLEFT;
    }

    return HDRG_CHROMA_LEFT;
}

static bool hdrg_gui_is_still(const AVFormatContext *format)
{
    const char *name = format->iformat->name;

    return strstr(name, "image2") != NULL || strstr(name, "_pipe") != NULL;
}

/* the next decoded frame into media->decoded (false: the end or an error, *failed says which) */
static bool hdrg_gui_decode(hdrg_gui_media_t *media, bool *failed)
{
    int32_t result;

    *failed = false;

    for (;;) {
        result = avcodec_receive_frame(media->codec, media->decoded);
        if (result == 0) {
            return true;
        }

        if (result == AVERROR_EOF) {
            return false;
        }

        if (result != AVERROR(EAGAIN)) {
            *failed = true;

            return false;
        }

        if (media->draining) {
            return false;
        }

        result = av_read_frame(media->format, media->packet);

        if (result < 0) {
            media->draining = true;
            avcodec_send_packet(media->codec, NULL);

            continue;
        }

        if (media->packet->stream_index == media->stream) {
            avcodec_send_packet(media->codec, media->packet);
        }

        av_packet_unref(media->packet);
    }
}

/* a frame to repack into: a released one's no one else references, else a new one */
static AVFrame *hdrg_gui_packed(hdrg_gui_media_t *media)
{
    AVFrame *packed;

    while (media->spare_count > 0) {
        packed = media->spares[--media->spare_count];
        if (av_frame_is_writable(packed)) {
            return packed;
        }

        av_frame_free(&packed);
    }

    packed = av_frame_alloc();
    if (packed == NULL) {
        return NULL;
    }

    packed->format = media->target;
    packed->width = (int)media->info.width;
    packed->height = (int)media->info.height;

    /* a still's rows tight (the session reads R'G'B' rows at width x 4), a video's aligned */
    if (av_frame_get_buffer(packed, (media->info.format == HDRG_GUI_RGBA) ? 1 : 0) < 0) {
        av_frame_free(&packed);
    }

    return packed;
}

extern bool hdrg_gui_media_open(const char *path, hdrg_gui_media_t **out, hdrg_gui_info_t *info,
                                char *error, size_t error_size)
{
    const AVPixFmtDescriptor *pixel;
    const AVCodec *decoder;
    AVCodecParameters *par;
    AVStream *stream;
    hdrg_gui_media_t *media;
    bool still;

    *out = NULL;
    media = (hdrg_gui_media_t *)calloc(1, sizeof(*media));
    if (media == NULL) {
        snprintf(error, error_size, "out of memory");

        return false;
    }

    decoder = NULL;

    if (avformat_open_input(&media->format, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(media->format, NULL) < 0 ||
        (media->stream =
             av_find_best_stream(media->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0)) < 0) {
        snprintf(error, error_size, "not a still or a video FFmpeg reads");
        hdrg_gui_media_close(media);

        return false;
    }

    stream = media->format->streams[media->stream];
    par = stream->codecpar;

    if (par->color_trc == AVCOL_TRC_SMPTE2084 || par->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        snprintf(error, error_size, "the video is already HDR (PQ / HLG)");
        hdrg_gui_media_close(media);

        return false;
    }

    media->codec = avcodec_alloc_context3(decoder);
    media->packet = av_packet_alloc();
    media->decoded = av_frame_alloc();

    if (media->codec == NULL || media->packet == NULL || media->decoded == NULL ||
        avcodec_parameters_to_context(media->codec, par) < 0 ||
        avcodec_open2(media->codec, decoder, NULL) < 0) {
        snprintf(error, error_size, "the decoder cannot be opened");
        hdrg_gui_media_close(media);

        return false;
    }

    still = hdrg_gui_is_still(media->format);
    pixel = av_pix_fmt_desc_get((enum AVPixelFormat)par->format);
    media->info.width = (uint32_t)par->width;
    media->info.height = (uint32_t)par->height;
    media->info.primaries = HDRG_PRIMARIES_BT709;
    media->info.transfer = still ? HDRG_TRANSFER_SRGB : HDRG_TRANSFER_BT709;
    media->info.matrix = hdrg_gui_matrix_av(par->color_space);
    media->info.full_range = par->color_range == AVCOL_RANGE_JPEG;
    media->info.chroma_loc = hdrg_gui_site_av(par->chroma_location);

    if (still) {
        media->info.format = HDRG_GUI_RGBA;
        media->target = AV_PIX_FMT_RGBA;
    } else {
        media->info.format =
            (pixel != NULL && pixel->comp[0].depth > 8) ? HDRG_GUI_P010 : HDRG_GUI_NV12;
        media->target = (media->info.format == HDRG_GUI_P010) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        media->info.rate_num = (uint32_t)stream->avg_frame_rate.num;
        media->info.rate_den = (uint32_t)stream->avg_frame_rate.den;
        media->info.duration_us =
            (media->format->duration > 0) ? media->format->duration * 1000000 / AV_TIME_BASE : 0;
    }

    if (media->info.width == 0 || media->info.height == 0 ||
        (!still && (media->info.rate_num == 0 || media->info.rate_den == 0))) {
        snprintf(error, error_size, "the video has no size or rate");
        hdrg_gui_media_close(media);

        return false;
    }

    *info = media->info;
    *out = media;

    return true;
}

extern uint8_t hdrg_gui_media_read(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    const AVRational base = media->format->streams[media->stream]->time_base;
    AVFrame *packed;
    int64_t pts;
    bool failed;

    memset(frame, 0, sizeof(*frame));

    if ((media->info.format == HDRG_GUI_RGBA && media->still_read) ||
        !hdrg_gui_decode(media, &failed)) {
        return (media->info.format != HDRG_GUI_RGBA && failed) ? HDRG_GUI_READ_ERROR
                                                               : HDRG_GUI_READ_END;
    }

    media->still_read = true;
    pts = media->decoded->best_effort_timestamp;

    /* the decoder's NV12 / P010 of the size as it is (its reference: no copy) */
    if (media->info.format != HDRG_GUI_RGBA && media->decoded->format == media->target &&
        media->decoded->width == (int)media->info.width &&
        media->decoded->height == (int)media->info.height) {
        packed = av_frame_alloc();
        if (packed == NULL) {
            return HDRG_GUI_READ_ERROR;
        }

        av_frame_move_ref(packed, media->decoded);
        packed->opaque = NULL;
    } else {
        media->scale = sws_getCachedContext(
            media->scale, media->decoded->width, media->decoded->height,
            (enum AVPixelFormat)media->decoded->format, (int)media->info.width,
            (int)media->info.height, media->target, SWS_POINT, NULL, NULL, NULL);
        packed = (media->scale != NULL) ? hdrg_gui_packed(media) : NULL;

        if (packed == NULL || sws_scale(media->scale, (const uint8_t *const *)media->decoded->data,
                                        media->decoded->linesize, 0, media->decoded->height,
                                        packed->data, packed->linesize) < 0) {
            av_frame_free(&packed);

            return HDRG_GUI_READ_ERROR;
        }

        /* (repacked: kept when released) */
        packed->opaque = media;
    }

    frame->planes[0] = packed->data[0];
    frame->planes[1] = packed->data[1];
    frame->strides[0] = (size_t)packed->linesize[0];
    frame->strides[1] = (size_t)packed->linesize[1];
    frame->pts_us = (pts != AV_NOPTS_VALUE) ? av_rescale_q(pts, base, AV_TIME_BASE_Q) : 0;
    frame->handle = packed;
    av_frame_unref(media->decoded);

    return HDRG_GUI_READ_FRAME;
}

extern void hdrg_gui_media_release(hdrg_gui_media_t *media, hdrg_gui_frame_t *frame)
{
    AVFrame *packed;

    packed = (AVFrame *)frame->handle;
    frame->handle = NULL;

    /* a repacked frame's buffer for the next one (a decoder's frame goes back to it) */
    if (packed != NULL && packed->opaque == media && media->spare_count < HDRG_GUI_SPARES) {
        media->spares[media->spare_count++] = packed;

        return;
    }

    av_frame_free(&packed);
}

extern bool hdrg_gui_media_seek(hdrg_gui_media_t *media, int64_t pts_us)
{
    const AVRational base = media->format->streams[media->stream]->time_base;

    media->still_read = false;
    media->draining = false;
    avcodec_flush_buffers(media->codec);

    return av_seek_frame(media->format, media->stream,
                         av_rescale_q((pts_us > 0) ? pts_us : 0, AV_TIME_BASE_Q, base),
                         AVSEEK_FLAG_BACKWARD) >= 0;
}

extern void hdrg_gui_media_close(hdrg_gui_media_t *media)
{
    if (media == NULL) {
        return;
    }

    while (media->spare_count > 0) {
        av_frame_free(&media->spares[--media->spare_count]);
    }

    sws_freeContext(media->scale);
    av_frame_free(&media->decoded);
    av_packet_free(&media->packet);
    avcodec_free_context(&media->codec);
    avformat_close_input(&media->format);
    free(media);
}
