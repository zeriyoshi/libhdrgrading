/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unity.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>

#include "hdrg_ffmpeg.h"
#include "../test_util.h"
#include "hdrgrading/video.h"

/*
 * hdrg-ffmpeg end to end: a Matroska file of 4:2:0 8-bit
 * BT.709 frames (FFV1, lossless) and PCM audio goes through the tool with
 * the lossless FFV1 encoder; every decoded output frame equals the session's
 * output for the same frames, byte for byte, and the stream carries PQ over
 * BT.2020, the mastering display, each frame's HDR10+ and ST 2094-50 (FFmpeg 9)
 * (BlockAdditional) and
 * the copied audio. On Apple, an H.264
 * file of VideoToolbox's decoded on VideoToolbox (--hwaccel: the pixel
 * buffers on the GPU into the session, or read in place) gives what its
 * software decoding gives, and a VideoToolbox encoder takes P010 pixel
 * buffers (on the GPU, or written in place). With --out-rate (twice the
 * clip's), the frames between are a session's at that output rate, their
 * timestamps the output rate's frames. An AV1 file with film grain (libaom's
 * estimate of a noisy clip): its decoder (libdav1d) leaves the grain out and
 * gives its parameters, and the output equals a session's fed the same
 * frames and parameters through tools/ffmpeg; --decoder-grain gives
 * another.
 */
#define TT_W       96
#define TT_H       64
#define TT_FRAMES  30
#define TT_RATE    24
#define TT_INPUT   "test_transcode_in.mkv"
#define TT_OUTPUT  "test_transcode_out.mkv"
#define TT_VT_W    320 /* the VideoToolbox clip's */
#define TT_VT_H    240
#define TT_VT_N    12
#define TT_VT_IN   "test_transcode_vt.mp4"
#define TT_OUT_MAX (2 * TT_FRAMES) /* the reference's outputs at most */
#define TT_AV1_IN  "test_transcode_av1.mkv"
#define TT_AV1_OUT "test_transcode_av1_out.mkv"
#define TT_AV1_N   12

static uint16_t *g_reference[TT_OUT_MAX][3];
static int32_t g_count;                /* the reference's outputs */
static int32_t g_w = TT_W, g_h = TT_H; /* tt_source's size */

/* source frame t: a gradient with a moving bright square; a cut to a darker picture at 15 */
static AVFrame *tt_source(int32_t t)
{
    AVFrame *frame;
    uint8_t *row;
    int32_t x, y, level;

    frame = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = g_w;
    frame->height = g_h;
    TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
    frame->pts = t;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;

    for (y = 0; y < g_h; y++) {
        row = frame->data[0] + y * frame->linesize[0];

        for (x = 0; x < g_w; x++) {
            level = 16 + (x * 2 + y) % 180;
            level = (t >= 15) ? 16 + (level - 16) / 3 : level;
            row[x] = (uint8_t)((x >= t * 2 && x < t * 2 + 12 && y >= 20 && y < 32) ? 235 : level);
        }
    }

    for (y = 0; y < g_h / 2; y++) {
        for (x = 0; x < g_w / 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = (uint8_t)(110 + x % 24);
            frame->data[2][y * frame->linesize[2] + x] = (uint8_t)(120 + y % 16);
        }
    }

    return frame;
}

static void tt_encode(AVCodecContext *enc, AVFormatContext *mux, AVStream *st, const AVFrame *frame)
{
    AVPacket *packet;

    packet = av_packet_alloc();
    TEST_ASSERT_NOT_NULL(packet);
    TEST_ASSERT_TRUE(avcodec_send_frame(enc, frame) >= 0);

    while (avcodec_receive_packet(enc, packet) >= 0) {
        av_packet_rescale_ts(packet, enc->time_base, st->time_base);
        packet->stream_index = st->index;
        TEST_ASSERT_TRUE(av_interleaved_write_frame(mux, packet) >= 0);
    }

    av_packet_free(&packet);
}

/* the input file: FFV1 video of the source frames and a second of silent PCM */
static void tt_write_input(void)
{
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVFormatContext *mux;
    AVCodecContext *video, *audio;
    AVStream *vst, *ast;
    AVFrame *frame, *samples;
    int32_t t;

    TEST_ASSERT_TRUE(avformat_alloc_output_context2(&mux, NULL, NULL, TT_INPUT) >= 0);

    video = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_FFV1));
    video->width = TT_W;
    video->height = TT_H;
    video->pix_fmt = AV_PIX_FMT_YUV420P;
    video->time_base = (AVRational){1, TT_RATE};
    video->framerate = (AVRational){TT_RATE, 1};
    video->color_primaries = AVCOL_PRI_BT709;
    video->color_trc = AVCOL_TRC_BT709;
    video->colorspace = AVCOL_SPC_BT709;
    video->color_range = AVCOL_RANGE_MPEG;
    TEST_ASSERT_TRUE(avcodec_open2(video, video->codec, NULL) >= 0);
    vst = avformat_new_stream(mux, NULL);
    TEST_ASSERT_TRUE(avcodec_parameters_from_context(vst->codecpar, video) >= 0);
    vst->time_base = video->time_base;
    vst->avg_frame_rate = video->framerate;

    audio = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE));
    audio->sample_fmt = AV_SAMPLE_FMT_S16;
    audio->sample_rate = 48000;
    audio->time_base = (AVRational){1, 48000};
    TEST_ASSERT_TRUE(av_channel_layout_copy(&audio->ch_layout, &stereo) >= 0);
    TEST_ASSERT_TRUE(avcodec_open2(audio, audio->codec, NULL) >= 0);
    ast = avformat_new_stream(mux, NULL);
    TEST_ASSERT_TRUE(avcodec_parameters_from_context(ast->codecpar, audio) >= 0);
    ast->time_base = audio->time_base;

    TEST_ASSERT_TRUE(avio_open(&mux->pb, TT_INPUT, AVIO_FLAG_WRITE) >= 0);
    TEST_ASSERT_TRUE(avformat_write_header(mux, NULL) >= 0);

    samples = av_frame_alloc();
    samples->format = AV_SAMPLE_FMT_S16;
    samples->sample_rate = 48000;
    samples->nb_samples = 48000;
    TEST_ASSERT_TRUE(av_channel_layout_copy(&samples->ch_layout, &stereo) >= 0);
    TEST_ASSERT_TRUE(av_frame_get_buffer(samples, 0) >= 0);
    memset(samples->data[0], 0, (size_t)samples->linesize[0]);
    samples->pts = 0;
    tt_encode(audio, mux, ast, samples);
    tt_encode(audio, mux, ast, NULL);
    av_frame_free(&samples);

    for (t = 0; t < TT_FRAMES; t++) {
        frame = tt_source(t);
        tt_encode(video, mux, vst, frame);
        av_frame_free(&frame);
    }

    tt_encode(video, mux, vst, NULL);
    TEST_ASSERT_TRUE(av_write_trailer(mux) >= 0);

    avcodec_free_context(&video);
    avcodec_free_context(&audio);
    avio_closep(&mux->pb);
    avformat_free_context(mux);
}

/*
 * the session's output for the source frames, with the tool's defaults at
 * the clip's rate (out_rate frames a second: 0, the clip's)
 */
static void tt_reference(uint32_t out_rate)
{
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_session_format_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    AVFrame *src;
    uint8_t p;
    int32_t sent, received;

    memset(&format, 0, sizeof(format));
    format.width = TT_W;
    format.height = TT_H;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_BT709;
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.rate_num = TT_RATE;
    config.rate_den = 1;
    config.out_rate_num = out_rate;
    config.out_rate_den = (out_rate != 0) ? 1u : 0u;
    config.latency = HDRG_LATENCY_OFFLINE;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, &caps, &config, &session));

    sent = 0;
    received = 0;

    for (;;) {
        TEST_ASSERT_TRUE(received < TT_OUT_MAX);
        memset(&out, 0, sizeof(out));

        for (p = 0; p < 3u; p++) {
            if (g_reference[received][p] == NULL) {
                g_reference[received][p] = (uint16_t *)calloc((size_t)(TT_W * TT_H), 2);
            }

            out.planes[p] = (uint8_t *)g_reference[received][p];
            out.strides[p] = (size_t)((p == 0) ? TT_W : TT_W / 2) * 2u;
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            src = (sent < TT_FRAMES) ? tt_source(sent) : NULL;

            for (p = 0; src != NULL && p < 3u; p++) {
                in.planes[p] = src->data[p];
                in.strides[p] = (size_t)src->linesize[p];
            }

            in.pts = sent;
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, src ? &in : NULL));
            av_frame_free(&src);
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    g_count = received;
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* frame n of the output against the reference */
static void tt_check(const AVFrame *frame, int32_t n)
{
    const uint16_t *row;
    int32_t p, x, y, w, h;

    TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_YUV420P10, frame->format);

    for (p = 0; p < 3; p++) {
        w = (p == 0) ? TT_W : TT_W / 2;
        h = (p == 0) ? TT_H : TT_H / 2;

        for (y = 0; y < h; y++) {
            row = (const uint16_t *)(const void *)(frame->data[p] + y * frame->linesize[p]);

            for (x = 0; x < w; x++) {
                if (row[x] != g_reference[n][p][y * w + x]) {
                    TEST_FAIL_MESSAGE("an output frame differs from the session's");
                }
            }
        }
    }
}

static void test_transcode(void)
{
    const AVCodecParameters *par;
    const AVPacketSideData *sd;
    const AVMasteringDisplayMetadata *master;
    const AVCodec *codec;
    AVFormatContext *demux;
    AVCodecContext *dec;
    AVPacket *packet;
    AVFrame *frame;
    uint32_t i;
    int32_t video, decoded, audio, plus, curves;
    char command[1024];

    tt_write_input();
    tt_reference(0);
    TEST_ASSERT_EQUAL_INT(TT_FRAMES, g_count);

    snprintf(command, sizeof(command), "\"%s\" --encoder ffv1 %s %s", HDRG_TEST_FFMPEG_TOOL,
             TT_INPUT, TT_OUTPUT);
    TEST_ASSERT_EQUAL_INT(0, system(command));

    demux = NULL;
    TEST_ASSERT_TRUE(avformat_open_input(&demux, TT_OUTPUT, NULL, NULL) >= 0);
    TEST_ASSERT_TRUE(avformat_find_stream_info(demux, NULL) >= 0);
    video = av_find_best_stream(demux, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    TEST_ASSERT_TRUE(video >= 0);
    audio = -1;

    for (i = 0; i < demux->nb_streams; i++) {
        audio =
            (demux->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ? (int32_t)i : audio;
    }

    TEST_ASSERT_TRUE(audio >= 0);

    par = demux->streams[video]->codecpar;
    TEST_ASSERT_EQUAL_INT(AVCOL_TRC_SMPTE2084, par->color_trc);
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_BT2020, par->color_primaries);
    TEST_ASSERT_EQUAL_INT(AVCOL_SPC_BT2020_NCL, par->color_space);
    TEST_ASSERT_EQUAL_INT(AVCOL_RANGE_MPEG, par->color_range);
    sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                 AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    TEST_ASSERT_NOT_NULL(sd);
    master = (const AVMasteringDisplayMetadata *)(const void *)sd->data;
    TEST_ASSERT_TRUE(master->has_primaries && master->has_luminance);
    TEST_ASSERT_TRUE(av_q2d(master->max_luminance) > 100.0);

    dec = avcodec_alloc_context3(codec);
    TEST_ASSERT_TRUE(avcodec_parameters_to_context(dec, par) >= 0);
    TEST_ASSERT_TRUE(avcodec_open2(dec, codec, NULL) >= 0);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    decoded = 0;
    plus = 0;
    curves = 0;

    while (av_read_frame(demux, packet) >= 0) {
        if (packet->stream_index == video) {
            plus += (av_packet_get_side_data(packet, AV_PKT_DATA_DYNAMIC_HDR10_PLUS, NULL) != NULL)
                        ? 1
                        : 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
            curves += (av_packet_get_side_data(packet, AV_PKT_DATA_DYNAMIC_HDR_SMPTE_2094_APP5,
                                               NULL) != NULL)
                          ? 1
                          : 0;
#endif
            TEST_ASSERT_TRUE(avcodec_send_packet(dec, packet) >= 0);

            while (avcodec_receive_frame(dec, frame) >= 0) {
                TEST_ASSERT_TRUE(decoded < TT_FRAMES);
                tt_check(frame, decoded);
                decoded++;
            }
        }

        av_packet_unref(packet);
    }

    TEST_ASSERT_TRUE(avcodec_send_packet(dec, NULL) >= 0);

    while (avcodec_receive_frame(dec, frame) >= 0) {
        tt_check(frame, decoded);
        decoded++;
    }

    TEST_ASSERT_EQUAL_INT(TT_FRAMES, decoded);

    /* each frame's HDR10+ (and ST 2094-50 in FFmpeg 9) as a BlockAdditional */
    TEST_ASSERT_EQUAL_INT(TT_FRAMES, plus);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
    TEST_ASSERT_EQUAL_INT(TT_FRAMES, curves);
#else
    (void)curves;
#endif

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
    avformat_close_input(&demux);
    remove(TT_INPUT);
    remove(TT_OUTPUT);
}

/* whether the build interpolates (an out_rate session: hdrg.interp.mc, HDRG_WITH_TEMPORAL) */
static bool tt_interpolates(void)
{
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;

    memset(&format, 0, sizeof(format));
    format.width = 16;
    format.height = 16;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = 48;
    hdrg_session_config_init(&config);
    config.rate_num = 30;
    config.rate_den = 1;
    config.out_rate_num = 60;
    config.out_rate_den = 1;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    status = hdrg_session_create(ctx, &format, NULL, NULL, &config, &session);

    if (status == HDRG_OK) {
        hdrg_session_destroy(session);
    }

    hdrg_context_destroy(ctx);
    TEST_ASSERT_TRUE(status == HDRG_OK || status == HDRG_ERR_UNSUPPORTED);

    return status == HDRG_OK;
}

/*
 * --out-rate twice the clip's: 2 n - 1 frames (none after the last input
 * frame), each the session's at that rate, the stream's rate and the
 * frames' timestamps the output rate's
 */
static void test_out_rate(void)
{
    const AVCodec *codec;
    AVFormatContext *demux;
    AVCodecContext *dec;
    AVPacket *packet;
    AVFrame *frame;
    AVRational tb;
    int32_t video, decoded;
    char command[1024];

    if (!tt_interpolates()) {
        TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
    }

    tt_write_input();
    tt_reference(2 * TT_RATE);
    TEST_ASSERT_EQUAL_INT(2 * TT_FRAMES - 1, g_count);

    snprintf(command, sizeof(command), "\"%s\" --encoder ffv1 --out-rate %d:1 %s %s",
             HDRG_TEST_FFMPEG_TOOL, 2 * TT_RATE, TT_INPUT, TT_OUTPUT);
    TEST_ASSERT_EQUAL_INT(0, system(command));

    demux = NULL;
    TEST_ASSERT_TRUE(avformat_open_input(&demux, TT_OUTPUT, NULL, NULL) >= 0);
    TEST_ASSERT_TRUE(avformat_find_stream_info(demux, NULL) >= 0);
    video = av_find_best_stream(demux, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    TEST_ASSERT_TRUE(video >= 0);
    tb = demux->streams[video]->time_base;
    TEST_ASSERT_EQUAL_INT(
        0, av_cmp_q(demux->streams[video]->avg_frame_rate, (AVRational){2 * TT_RATE, 1}));

    dec = avcodec_alloc_context3(codec);
    TEST_ASSERT_TRUE(avcodec_parameters_to_context(dec, demux->streams[video]->codecpar) >= 0);
    TEST_ASSERT_TRUE(avcodec_open2(dec, codec, NULL) >= 0);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    decoded = 0;

    while (av_read_frame(demux, packet) >= 0) {
        if (packet->stream_index == video) {
            TEST_ASSERT_TRUE(avcodec_send_packet(dec, packet) >= 0);

            while (avcodec_receive_frame(dec, frame) >= 0) {
                TEST_ASSERT_TRUE(decoded < g_count);
                tt_check(frame, decoded);
                TEST_ASSERT_EQUAL_INT64(decoded, av_rescale_q(frame->best_effort_timestamp, tb,
                                                              (AVRational){1, 2 * TT_RATE}));
                decoded++;
            }
        }

        av_packet_unref(packet);
    }

    TEST_ASSERT_TRUE(avcodec_send_packet(dec, NULL) >= 0);

    while (avcodec_receive_frame(dec, frame) >= 0) {
        tt_check(frame, decoded);
        decoded++;
    }

    TEST_ASSERT_EQUAL_INT(g_count, decoded);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec);
    avformat_close_input(&demux);
    remove(TT_INPUT);
    remove(TT_OUTPUT);
}

/* the video frames of a file, decoded on the CPU, into frames (their count; at most max) */
static int32_t tt_decode_all(const char *path, AVFrame **frames, int32_t max, bool film)
{
    const AVCodec *codec;
    AVFormatContext *demux;
    AVCodecContext *dec;
    AVPacket *packet;
    int32_t video, n;
    bool draining;

    demux = NULL;
    TEST_ASSERT_TRUE(avformat_open_input(&demux, path, NULL, NULL) >= 0);
    TEST_ASSERT_TRUE(avformat_find_stream_info(demux, NULL) >= 0);
    video = av_find_best_stream(demux, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    TEST_ASSERT_TRUE(video >= 0);
    dec = avcodec_alloc_context3(codec);
    TEST_ASSERT_TRUE(avcodec_parameters_to_context(dec, demux->streams[video]->codecpar) >= 0);
    dec->export_side_data |= film ? AV_CODEC_EXPORT_DATA_FILM_GRAIN : 0;
    TEST_ASSERT_TRUE(avcodec_open2(dec, codec, NULL) >= 0);
    packet = av_packet_alloc();
    n = 0;
    draining = false;

    for (;;) {
        if (!draining && av_read_frame(demux, packet) < 0) {
            draining = true;
            TEST_ASSERT_TRUE(avcodec_send_packet(dec, NULL) >= 0);
        } else if (!draining) {
            if (packet->stream_index == video) {
                TEST_ASSERT_TRUE(avcodec_send_packet(dec, packet) >= 0);
            }

            av_packet_unref(packet);
        }

        for (;;) {
            frames[n] = av_frame_alloc();
            TEST_ASSERT_NOT_NULL(frames[n]);

            if (avcodec_receive_frame(dec, frames[n]) < 0) {
                av_frame_free(&frames[n]);

                break;
            }

            TEST_ASSERT_TRUE(n < max);
            n++;
        }

        if (draining) {
            break;
        }
    }

    av_packet_free(&packet);
    avcodec_free_context(&dec);
    avformat_close_input(&demux);

    return n;
}

/* the tool on the VideoToolbox clip with options into path; the files' frames the same */
static void tt_vt_same(const char *options_a, const char *options_b)
{
    AVFrame *a[TT_VT_N + 1], *b[TT_VT_N + 1];
    int32_t n, i, p, y, w, h;
    char command[1024];

    snprintf(command, sizeof(command), "\"%s\" %s --encoder ffv1 %s vt_a.mkv",
             HDRG_TEST_FFMPEG_TOOL, options_a, TT_VT_IN);
    TEST_ASSERT_EQUAL_INT(0, system(command));
    snprintf(command, sizeof(command), "\"%s\" %s --encoder ffv1 %s vt_b.mkv",
             HDRG_TEST_FFMPEG_TOOL, options_b, TT_VT_IN);
    TEST_ASSERT_EQUAL_INT(0, system(command));

    n = tt_decode_all("vt_a.mkv", a, TT_VT_N, false);
    TEST_ASSERT_EQUAL_INT(TT_VT_N, n);
    TEST_ASSERT_EQUAL_INT(TT_VT_N, tt_decode_all("vt_b.mkv", b, TT_VT_N, false));

    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_YUV420P10, a[i]->format);
        TEST_ASSERT_EQUAL_INT(a[i]->format, b[i]->format);

        for (p = 0; p < 3; p++) {
            w = ((p == 0) ? TT_VT_W : TT_VT_W / 2) * 2;
            h = (p == 0) ? TT_VT_H : TT_VT_H / 2;

            for (y = 0; y < h; y++) {
                TEST_ASSERT_EQUAL_MEMORY(a[i]->data[p] + y * a[i]->linesize[p],
                                         b[i]->data[p] + y * b[i]->linesize[p], (size_t)w);
            }
        }

        av_frame_free(&a[i]);
        av_frame_free(&b[i]);
    }

    remove("vt_a.mkv");
    remove("vt_b.mkv");
}

/*
 * VideoToolbox (Apple): its decoder's pixel buffers into the session on the
 * GPU (PLAYBACK) or read in place (OFFLINE) give what the software decoder's
 * frames give; its HEVC encoder takes P010 pixel buffers either way
 */
static void test_videotoolbox(void)
{
    const AVCodec *codec;
    AVFormatContext *mux, *demux;
    AVCodecContext *enc;
    AVStream *st;
    AVFrame *frame, *frames[TT_VT_N + 1];
    int32_t t, n;
    char command[1024];

    codec = avcodec_find_encoder_by_name("h264_videotoolbox");
    if (codec == NULL || avcodec_find_encoder_by_name("hevc_videotoolbox") == NULL) {
        TEST_IGNORE_MESSAGE("no VideoToolbox");
    }

    g_w = TT_VT_W;
    g_h = TT_VT_H;
    TEST_ASSERT_TRUE(avformat_alloc_output_context2(&mux, NULL, NULL, TT_VT_IN) >= 0);
    enc = avcodec_alloc_context3(codec);
    enc->width = TT_VT_W;
    enc->height = TT_VT_H;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = (AVRational){1, TT_RATE};
    enc->framerate = (AVRational){TT_RATE, 1};
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc = AVCOL_TRC_BT709;
    enc->colorspace = AVCOL_SPC_BT709;
    enc->color_range = AVCOL_RANGE_MPEG;
    enc->bit_rate = 4000000;

    if (avcodec_open2(enc, codec, NULL) < 0) {
        avcodec_free_context(&enc);
        avformat_free_context(mux);
        g_w = TT_W;
        g_h = TT_H;
        TEST_IGNORE_MESSAGE("VideoToolbox's H.264 encoder does not open");
    }

    st = avformat_new_stream(mux, NULL);
    TEST_ASSERT_TRUE(avcodec_parameters_from_context(st->codecpar, enc) >= 0);
    st->time_base = enc->time_base;
    TEST_ASSERT_TRUE(avio_open(&mux->pb, TT_VT_IN, AVIO_FLAG_WRITE) >= 0);
    TEST_ASSERT_TRUE(avformat_write_header(mux, NULL) >= 0);

    for (t = 0; t < TT_VT_N; t++) {
        frame = tt_source(t);
        tt_encode(enc, mux, st, frame);
        av_frame_free(&frame);
    }

    tt_encode(enc, mux, st, NULL);
    TEST_ASSERT_TRUE(av_write_trailer(mux) >= 0);
    avcodec_free_context(&enc);
    avio_closep(&mux->pb);
    avformat_free_context(mux);
    g_w = TT_W;
    g_h = TT_H;

    /* on the GPU (a session that keeps its frames there, hdrg.mctf's windows too), in place */
    tt_vt_same("--gpu auto --latency playback", "--hwaccel videotoolbox --latency playback");
    tt_vt_same("--gpu auto", "--hwaccel videotoolbox");

    /* VideoToolbox's HEVC of P010 pixel buffers, on the GPU and written in place */
    snprintf(command, sizeof(command),
             "\"%s\" --hwaccel videotoolbox --latency playback --encoder hevc_videotoolbox %s "
             "vt_hevc.mp4",
             HDRG_TEST_FFMPEG_TOOL, TT_VT_IN);
    TEST_ASSERT_EQUAL_INT(0, system(command));
    snprintf(command, sizeof(command), "\"%s\" --encoder hevc_videotoolbox %s vt_hevc2.mp4",
             HDRG_TEST_FFMPEG_TOOL, TT_VT_IN);
    TEST_ASSERT_EQUAL_INT(0, system(command));

    demux = NULL;
    TEST_ASSERT_TRUE(avformat_open_input(&demux, "vt_hevc.mp4", NULL, NULL) >= 0);
    TEST_ASSERT_TRUE(avformat_find_stream_info(demux, NULL) >= 0);
    TEST_ASSERT_EQUAL_INT(AVCOL_TRC_SMPTE2084, demux->streams[0]->codecpar->color_trc);
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_BT2020, demux->streams[0]->codecpar->color_primaries);
    avformat_close_input(&demux);

    n = tt_decode_all("vt_hevc2.mp4", frames, TT_VT_N, false);
    TEST_ASSERT_EQUAL_INT(TT_VT_N, n);

    for (t = 0; t < n; t++) {
        av_frame_free(&frames[t]);
    }

    remove("vt_hevc.mp4");
    remove("vt_hevc2.mp4");
    remove(TT_VT_IN);
}

/*
 * the AV1 input: the VideoToolbox clip's frames with white noise of sigma 4
 * on the luma, libaom's film grain estimated from it (false: no libaom or
 * libdav1d)
 */
static bool tt_write_av1(void)
{
    const AVCodec *codec;
    AVFormatContext *mux;
    AVCodecContext *enc;
    AVDictionary *opts;
    AVStream *st;
    AVFrame *frame;
    uint32_t h;
    int32_t t, x, y, v;

    codec = avcodec_find_encoder_by_name("libaom-av1");
    if (codec == NULL || avcodec_find_decoder_by_name("libdav1d") == NULL) {
        return false;
    }

    /* (under Valgrind: FFmpeg 7.1's libaomenc writes freed memory as it opens) */
#if defined(HDRG_TEST_VALGRIND)
    return false;
#endif

    g_w = TT_VT_W;
    g_h = TT_VT_H;
    TEST_ASSERT_TRUE(avformat_alloc_output_context2(&mux, NULL, NULL, TT_AV1_IN) >= 0);
    enc = avcodec_alloc_context3(codec);
    enc->width = g_w;
    enc->height = g_h;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = (AVRational){1, TT_RATE};
    enc->framerate = (AVRational){TT_RATE, 1};
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc = AVCOL_TRC_BT709;
    enc->colorspace = AVCOL_SPC_BT709;
    enc->color_range = AVCOL_RANGE_MPEG;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    opts = NULL;
    av_dict_set(&opts, "cpu-used", "8", 0);
    av_dict_set(&opts, "denoise-noise-level", "8", 0);
    TEST_ASSERT_TRUE(avcodec_open2(enc, codec, &opts) >= 0);
    av_dict_free(&opts);
    st = avformat_new_stream(mux, NULL);
    TEST_ASSERT_TRUE(avcodec_parameters_from_context(st->codecpar, enc) >= 0);
    st->time_base = enc->time_base;
    st->avg_frame_rate = enc->framerate;
    TEST_ASSERT_TRUE(avio_open(&mux->pb, TT_AV1_IN, AVIO_FLAG_WRITE) >= 0);
    TEST_ASSERT_TRUE(avformat_write_header(mux, NULL) >= 0);
    h = 0x2545F491u;

    for (t = 0; t < TT_AV1_N; t++) {
        frame = tt_source(t);

        for (y = 0; y < g_h; y++) {
            for (x = 0; x < g_w; x++) {
                h ^= h << 13;
                h ^= h >> 17;
                h ^= h << 5;
                v = frame->data[0][y * frame->linesize[0] + x] +
                    ((int32_t)((h & 0xffu) + ((h >> 8) & 0xffu) + ((h >> 16) & 0xffu) + (h >> 24)) -
                     510) *
                        4 / 148;
                frame->data[0][y * frame->linesize[0] + x] = (uint8_t)((v < 16)    ? 16
                                                                       : (v > 235) ? 235
                                                                                   : v);
            }
        }

        tt_encode(enc, mux, st, frame);
        av_frame_free(&frame);
    }

    tt_encode(enc, mux, st, NULL);
    TEST_ASSERT_TRUE(av_write_trailer(mux) >= 0);
    avcodec_free_context(&enc);
    avio_closep(&mux->pb);
    avformat_free_context(mux);

    return true;
}

/*
 * the session's outputs (the tool's defaults) for n decoded frames through
 * tools/ffmpeg (their film grain with them) into out (n + 1 of them, the
 * last the end's): each its planes of 10-bit samples in rows of g_w and
 * g_w / 2
 */
static void tt_binding_reference(AVFrame **frames, int32_t n, uint16_t **out)
{
    const size_t luma = (size_t)g_w * (size_t)g_h;
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_session_format_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t output;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    int32_t sent, received;

    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frames[0], &format, &config.input));
    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.rate_num = TT_RATE;
    config.rate_den = 1;
    config.latency = HDRG_LATENCY_OFFLINE;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, &caps, &config, &session));

    sent = 0;
    received = 0;

    for (;;) {
        TEST_ASSERT_TRUE(received <= n);
        memset(&output, 0, sizeof(output));
        output.planes[0] = (uint8_t *)out[received];
        output.planes[1] = (uint8_t *)(out[received] + luma);
        output.planes[2] = (uint8_t *)(out[received] + luma + luma / 4u);
        output.strides[0] = (size_t)g_w * 2u;
        output.strides[1] = (size_t)g_w;
        output.strides[2] = (size_t)g_w;

        status = hdrg_session_receive_frame(session, &output);
        if (status == HDRG_ERR_AGAIN) {
            if (sent < n) {
                TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                      hdrg_ff_frame(frames[sent], &format, &config.input, &in));
                in.pts = sent;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < n) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    TEST_ASSERT_EQUAL_INT(n, received);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* whether the decoded frame equals an output of the reference */
static bool tt_same(const AVFrame *frame, const uint16_t *out)
{
    const size_t luma = (size_t)g_w * (size_t)g_h;
    int32_t p, y, w, h;

    TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_YUV420P10, frame->format);

    for (p = 0; p < 3; p++) {
        w = (p == 0) ? g_w : g_w / 2;
        h = (p == 0) ? g_h : g_h / 2;

        for (y = 0; y < h; y++) {
            if (memcmp(frame->data[p] + y * frame->linesize[p],
                       out + ((p == 0) ? 0u : luma + (size_t)(p - 1) * luma / 4u) +
                           (size_t)y * (size_t)w,
                       (size_t)w * 2u) != 0) {
                return false;
            }
        }
    }

    return true;
}

/* the noise of a 10-bit frame's Y' (Immerkaer's estimate, its codes) */
static double tt_noise(const AVFrame *frame)
{
    const uint16_t *a, *b, *c;
    int32_t x, y, k;
    double sum;

    sum = 0.0;

    for (y = 1; y < frame->height - 1; y++) {
        a = (const uint16_t *)(const void *)(frame->data[0] + (y - 1) * frame->linesize[0]);
        b = (const uint16_t *)(const void *)(frame->data[0] + y * frame->linesize[0]);
        c = (const uint16_t *)(const void *)(frame->data[0] + (y + 1) * frame->linesize[0]);

        for (x = 1; x < frame->width - 1; x++) {
            k = a[x - 1] - 2 * a[x] + a[x + 1] - 2 * (b[x - 1] - 2 * b[x] + b[x + 1]) + c[x - 1] -
                2 * c[x] + c[x + 1];
            sum += (k < 0) ? -k : k;
        }
    }

    return sum * 1.2533141373155 / (6.0 * (frame->width - 2) * (frame->height - 2));
}

/*
 * AV1's film grain: libdav1d leaves the grain out and gives its parameters;
 * the tool's output equals a session's fed those frames and parameters
 * through tools/ffmpeg, and --decoder-grain (the decoder's grain, measured
 * on the way) gives another
 */
static void test_film_grain(void)
{
    AVFrame *frames[TT_AV1_N + 1], *outs[TT_AV1_N + 1];
    uint16_t *reference[TT_AV1_N + 1];
    int32_t n, i, given, differs;
    char command[1024];
    double noise[2];

    if (!tt_write_av1()) {
        g_w = TT_W;
        g_h = TT_H;
        TEST_IGNORE_MESSAGE("no libaom-av1 or libdav1d (or under Valgrind)");
    }

    n = tt_decode_all(TT_AV1_IN, frames, TT_AV1_N, true);
    TEST_ASSERT_EQUAL_INT(TT_AV1_N, n);
    given = 0;

    for (i = 0; i < n; i++) {
        given +=
            (av_frame_get_side_data(frames[i], AV_FRAME_DATA_FILM_GRAIN_PARAMS) != NULL) ? 1 : 0;
        reference[i] = (uint16_t *)malloc((size_t)g_w * (size_t)g_h * 3u);
        TEST_ASSERT_NOT_NULL(reference[i]);
    }

    reference[n] = (uint16_t *)malloc((size_t)g_w * (size_t)g_h * 3u);
    TEST_ASSERT_NOT_NULL(reference[n]);
    TEST_ASSERT_TRUE(given > 0);
    tt_binding_reference(frames, n, reference);

    snprintf(command, sizeof(command), "\"%s\" --encoder ffv1 %s %s", HDRG_TEST_FFMPEG_TOOL,
             TT_AV1_IN, TT_AV1_OUT);
    TEST_ASSERT_EQUAL_INT(0, system(command));
    TEST_ASSERT_EQUAL_INT(n, tt_decode_all(TT_AV1_OUT, outs, TT_AV1_N, false));

    noise[0] = 0.0;

    for (i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(tt_same(outs[i], reference[i]));
        noise[0] += tt_noise(outs[i]) / n;
        av_frame_free(&outs[i]);
    }

    snprintf(command, sizeof(command), "\"%s\" --decoder-grain --encoder ffv1 %s %s",
             HDRG_TEST_FFMPEG_TOOL, TT_AV1_IN, TT_AV1_OUT);
    TEST_ASSERT_EQUAL_INT(0, system(command));
    TEST_ASSERT_EQUAL_INT(n, tt_decode_all(TT_AV1_OUT, outs, TT_AV1_N, false));
    differs = 0;
    noise[1] = 0.0;

    for (i = 0; i < n; i++) {
        differs += tt_same(outs[i], reference[i]) ? 0 : 1;
        noise[1] += tt_noise(outs[i]) / n;
        av_frame_free(&outs[i]);
        av_frame_free(&frames[i]);
        free(reference[i]);
    }

    free(reference[n]);
    TEST_ASSERT_TRUE(differs > 0);

    /* (the grain's level either way: the parameters' and the decoder's measured) */
    snprintf(command, sizeof(command), "output Y' noise: %.2f codes (parameters), %.2f (decoder)",
             noise[0], noise[1]);
    TEST_MESSAGE(command);
    TEST_ASSERT_TRUE(noise[0] > 0.5 * noise[1] && noise[0] < 2.0 * noise[1]);

    g_w = TT_W;
    g_h = TT_H;
    remove(TT_AV1_IN);
    remove(TT_AV1_OUT);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    uint8_t p;
    int32_t failures, n;

    UNITY_BEGIN();
    RUN_TEST(test_transcode);
    RUN_TEST(test_out_rate);
    RUN_TEST(test_videotoolbox);
    RUN_TEST(test_film_grain);
    failures = UNITY_END();

    for (n = 0; n < TT_OUT_MAX; n++) {
        for (p = 0; p < 3u; p++) {
            free(g_reference[n][p]);
        }
    }

    return failures;
}
