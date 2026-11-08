/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/video.h"

/*
 * FFmpeg's libhdrgrading filter (src/bindings/ffmpeg/patches)
 * through libavfilter's API: buffer → libhdrgrading → format → buffersink
 * over 4:2:0 8-bit BT.709 frames (a moving square, a cut). Every frame out
 * equals a session's output for the same frames byte for byte (yuv420p10,
 * p010 and rgb48 out, a setting by its name, twice the frame rate with the
 * frames between where the build interpolates), with its pts, PQ over BT.2020, the mastering
 * display up to the session's peak and the session's HDR10+ and ST 2094-50 as FFmpeg writes
 * hdrg_hdr10plus_t35's and hdrg_st2094_50_t35's bytes back; HDR input and an unknown setting are
 * refused. An FFmpeg without the filter: ignored (a failure with HDRG_TEST_REQUIRE_FILTER set). The
 * filter's library is this build's (a shared one, through the loader's path).
 */
#define TF_W       96
#define TF_H       64
#define TF_FRAMES  24
#define TF_RATE    24
#define TF_OUT_MAX (2 * TF_FRAMES)
#define TF_TB      90000 /* the filter's session time base with another frame rate */

/* a session's output: its planes' rows packed, its pts and its T.35 payloads */
typedef struct {
    uint8_t *planes[3];
    uint8_t plus[HDRG_HDR10PLUS_T35_BYTES], curve[HDRG_ST2094_50_T35_MAX];
    int64_t pts;
    size_t plus_size, curve_size;
} tf_output_t;

static tf_output_t g_reference[TF_OUT_MAX];
static int32_t g_count; /* the reference's outputs */
static float g_peak;    /* the reference session's peak */

/* an FFmpeg without the filter: ignored, a failure where HDRG_TEST_REQUIRE_FILTER is set (CI) */
static void tf_require(void)
{
    if (avfilter_get_by_name("libhdrgrading") != NULL) {
        return;
    }

    if (getenv("HDRG_TEST_REQUIRE_FILTER") != NULL) {
        TEST_FAIL_MESSAGE("an FFmpeg without the libhdrgrading filter");
    }

    TEST_IGNORE_MESSAGE("an FFmpeg without the libhdrgrading filter");
}

/* whether the build interpolates (an out_rate session: hdrg.interp.mc, HDRG_WITH_TEMPORAL) */
static bool tf_interpolates(void)
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
    config.rate_num = TF_RATE;
    config.rate_den = 1;
    config.out_rate_num = 2 * TF_RATE;
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

/* format's planes: their rows and bytes a row */
static uint8_t tf_planes(enum AVPixelFormat format, size_t rows[3], size_t bytes[3])
{
    rows[0] = TF_H;
    rows[1] = TF_H / 2;
    rows[2] = TF_H / 2;

    if (format == AV_PIX_FMT_RGB48) {
        bytes[0] = (size_t)TF_W * 6u;

        return 1;
    }

    bytes[0] = (size_t)TF_W * 2u;

    if (format == AV_PIX_FMT_P010) {
        bytes[1] = (size_t)TF_W * 2u;

        return 2;
    }

    bytes[1] = (size_t)TF_W;
    bytes[2] = (size_t)TF_W;

    return 3;
}

/* source frame t: a gradient with a moving bright square; a cut to a darker picture at 12 */
static AVFrame *tf_source(int32_t t, bool hdr)
{
    AVFrame *frame;
    uint8_t *row;
    int32_t x, y, level;

    frame = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = TF_W;
    frame->height = TF_H;
    TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
    frame->pts = t;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = hdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;

    for (y = 0; y < TF_H; y++) {
        row = frame->data[0] + y * frame->linesize[0];

        for (x = 0; x < TF_W; x++) {
            level = 16 + (x * 2 + y) % 180;
            level = (t >= 12) ? 16 + (level - 16) / 3 : level;
            row[x] = (uint8_t)((x >= t * 3 && x < t * 3 + 12 && y >= 20 && y < 32) ? 235 : level);
        }
    }

    for (y = 0; y < TF_H / 2; y++) {
        for (x = 0; x < TF_W / 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = (uint8_t)(110 + x % 24);
            frame->data[2][y * frame->linesize[2] + x] = (uint8_t)(120 + y % 16);
        }
    }

    return frame;
}

/*
 * the session the filter makes of the options' defaults (the setting, when
 * a name: by its name) out in format, at out_rate (0: the input's)
 */
static void tf_reference(enum AVPixelFormat format, uint32_t out_rate, const char *setting,
                         double value)
{
    hdrg_session_config_t config;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_format_t raw;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    tf_output_t *ref;
    AVFrame *src;
    uint8_t p, planes;
    int32_t sent, received;
    size_t rows[3], bytes[3];

    memset(&raw, 0, sizeof(raw));
    raw.width = TF_W;
    raw.height = TF_H;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_BT709;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;

    if (setting != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_setting_set(setting, value, &params, &caps, NULL));
    }

    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    config.output.layout = (format == AV_PIX_FMT_RGB48) ? 0 : HDRG_YUV_420;
    config.output.matrix = (format == AV_PIX_FMT_RGB48) ? 0 : HDRG_MATRIX_BT2020;
    config.output.bits = (format == AV_PIX_FMT_RGB48) ? 16 : 10;
    config.output.interleaved = format == AV_PIX_FMT_P010;
    config.output.msb = format == AV_PIX_FMT_P010;
    config.rate_num = TF_RATE;
    config.rate_den = 1;
    config.out_rate_num = out_rate;
    config.out_rate_den = (out_rate != 0) ? 1u : 0u;
    config.latency = HDRG_LATENCY_OFFLINE;
    config.future = HDRG_SESSION_AUTO;
    planes = tf_planes(format, rows, bytes);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, &params, &caps, &config, &session));
    g_peak = hdrg_session_peak(session);

    sent = 0;
    received = 0;

    for (;;) {
        TEST_ASSERT_TRUE(received < TF_OUT_MAX);
        ref = &g_reference[received];
        memset(&out, 0, sizeof(out));

        for (p = 0; p < planes; p++) {
            free(ref->planes[p]);
            ref->planes[p] = (uint8_t *)calloc(rows[p], bytes[p]);
            TEST_ASSERT_NOT_NULL(ref->planes[p]);
            out.planes[p] = ref->planes[p];
            out.strides[p] = bytes[p];
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            src = (sent < TF_FRAMES) ? tf_source(sent, false) : NULL;

            for (p = 0; src != NULL && p < 3u; p++) {
                in.planes[p] = src->data[p];
                in.strides[p] = (size_t)src->linesize[p];
            }

            in.pts = (out_rate != 0) ? (int64_t)sent * TF_TB / TF_RATE : sent;
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, src ? &in : NULL));
            av_frame_free(&src);
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        ref->pts = out.pts;
        ref->plus_size = hdrg_hdr10plus_t35(&out.hdr10plus, ref->plus, sizeof(ref->plus));
        ref->curve_size = hdrg_st2094_50_t35(&out.st2094_50, ref->curve, sizeof(ref->curve));
        received++;
    }

    g_count = received;
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* the frames through buffer → libhdrgrading (options: name, value, ..., NULL) → format */
static int32_t tf_run(enum AVPixelFormat format, const char *const *options, bool hdr,
                      AVFrame **outs, int32_t *count)
{
    AVFilterGraph *graph;
    AVFilterContext *src, *grade, *convert, *sink;
    AVFrame *frame;
    int32_t t, status, ret;
    char args[256];

    graph = avfilter_graph_alloc();
    TEST_ASSERT_NOT_NULL(graph);
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1:frame_rate=%d/1:"
             "colorspace=bt709:range=tv",
             TF_W, TF_H, (int)AV_PIX_FMT_YUV420P, TF_RATE, TF_RATE);
    TEST_ASSERT_TRUE(avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args,
                                                  NULL, graph) >= 0);
    grade = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("libhdrgrading"), "grade");
    TEST_ASSERT_NOT_NULL(grade);

    for (t = 0; options != NULL && options[t] != NULL; t += 2) {
        TEST_ASSERT_TRUE(av_opt_set(grade, options[t], options[t + 1], AV_OPT_SEARCH_CHILDREN) >=
                         0);
    }

    *count = 0;
    status = avfilter_init_str(grade, NULL);
    if (status < 0) {
        avfilter_graph_free(&graph);

        return status;
    }

    TEST_ASSERT_TRUE(avfilter_graph_create_filter(&convert, avfilter_get_by_name("format"),
                                                  "format", av_get_pix_fmt_name(format), NULL,
                                                  graph) >= 0);
    TEST_ASSERT_TRUE(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out",
                                                  NULL, NULL, graph) >= 0);
    TEST_ASSERT_TRUE(avfilter_link(src, 0, grade, 0) >= 0 &&
                     avfilter_link(grade, 0, convert, 0) >= 0 &&
                     avfilter_link(convert, 0, sink, 0) >= 0);
    TEST_ASSERT_TRUE(avfilter_graph_config(graph, NULL) >= 0);

    /* each frame in, the frames ready out; then the end and the rest */
    status = 0;

    for (t = 0; t <= TF_FRAMES && status >= 0; t++) {
        frame = (t < TF_FRAMES) ? tf_source(t, hdr) : NULL;
        status = av_buffersrc_add_frame(src, frame);
        av_frame_free(&frame);

        for (;;) {
            TEST_ASSERT_TRUE(*count < TF_OUT_MAX);
            outs[*count] = av_frame_alloc();
            ret = av_buffersink_get_frame(sink, outs[*count]);
            if (ret < 0) {
                av_frame_free(&outs[*count]);

                break;
            }

            (*count)++;
        }

        status = (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? status : ret;
    }

    avfilter_graph_free(&graph);

    return status;
}

/* frame n out of the filter against the reference */
static void tf_check(const AVFrame *frame, int32_t n, enum AVPixelFormat format, uint32_t out_rate)
{
    const tf_output_t *ref = &g_reference[n];
    const AVFrameSideData *sd;
    const AVMasteringDisplayMetadata *master;
    uint8_t p, planes, *t35;
    size_t rows[3], bytes[3], y, size;

    TEST_ASSERT_EQUAL_INT(format, frame->format);
    TEST_ASSERT_EQUAL_INT(TF_W, frame->width);
    TEST_ASSERT_EQUAL_INT(TF_H, frame->height);
    planes = tf_planes(format, rows, bytes);

    for (p = 0; p < planes; p++) {
        for (y = 0; y < rows[p]; y++) {
            if (memcmp(frame->data[p] + y * (size_t)frame->linesize[p],
                       ref->planes[p] + y * bytes[p], bytes[p]) != 0) {
                TEST_FAIL_MESSAGE("a frame differs from the session's");
            }
        }
    }

    TEST_ASSERT_EQUAL_INT64((out_rate != 0) ? av_rescale_q(ref->pts, (AVRational){1, TF_TB},
                                                           (AVRational){1, (int)out_rate})
                                            : ref->pts,
                            frame->pts);
    TEST_ASSERT_EQUAL_INT(AVCOL_TRC_SMPTE2084, frame->color_trc);
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_BT2020, frame->color_primaries);
    TEST_ASSERT_EQUAL_INT((format == AV_PIX_FMT_RGB48) ? AVCOL_SPC_RGB : AVCOL_SPC_BT2020_NCL,
                          frame->colorspace);

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    TEST_ASSERT_NOT_NULL(sd);
    master = (const AVMasteringDisplayMetadata *)(const void *)sd->data;
    TEST_ASSERT_TRUE(master->has_luminance && master->has_primaries);
    TEST_ASSERT_TRUE(fabs(av_q2d(master->max_luminance) - (double)g_peak) < 1e-3);

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    TEST_ASSERT_EQUAL_INT(ref->plus_size > 0, sd != NULL);

    if (sd != NULL) {
        t35 = NULL;
        size = 0;
        TEST_ASSERT_TRUE(av_dynamic_hdr_plus_to_t35(
                             (const AVDynamicHDRPlus *)(const void *)sd->data, &t35, &size) >= 0);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)ref->plus_size, (uint32_t)size);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(ref->plus, t35, size);
        av_free(t35);
    }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5);
    TEST_ASSERT_EQUAL_INT(ref->curve_size > 0, sd != NULL);

    if (sd != NULL) {
        t35 = NULL;
        size = 0;
        TEST_ASSERT_TRUE(
            av_dynamic_hdr_smpte2094_app5_to_t35(
                (const AVDynamicHDRSmpte2094App5 *)(const void *)sd->data, &t35, &size) >= 0);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)ref->curve_size, (uint32_t)size);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(ref->curve, t35, size);
        av_free(t35);
    }
#endif
}

/* the filter's run of format and options against the session's of the same */
static void tf_same(enum AVPixelFormat format, const char *const *options, uint32_t out_rate,
                    const char *setting, double value)
{
    AVFrame *outs[TF_OUT_MAX];
    int32_t count, n;

    tf_require();
    tf_reference(format, out_rate, setting, value);
    TEST_ASSERT_TRUE(tf_run(format, options, false, outs, &count) >= 0);
    TEST_ASSERT_EQUAL_INT(g_count, count);

    for (n = 0; n < count; n++) {
        tf_check(outs[n], n, format, out_rate);
        av_frame_free(&outs[n]);
    }
}

static void test_yuv420p10(void)
{
    tf_same(AV_PIX_FMT_YUV420P10, NULL, 0, NULL, 0.0);
    TEST_ASSERT_EQUAL_INT(TF_FRAMES, g_count);
}

static void test_p010(void)
{
    tf_same(AV_PIX_FMT_P010, NULL, 0, NULL, 0.0);
}

static void test_rgb48(void)
{
    tf_same(AV_PIX_FMT_RGB48, NULL, 0, NULL, 0.0);
}

/* a setting by its name: the session's with it (which differs from the default's) */
static void test_setting(void)
{
    static const char *const options[] = {"settings", "diffuse-max=1.5", NULL};
    uint8_t *plain;
    size_t rows[3], bytes[3];

    (void)tf_planes(AV_PIX_FMT_YUV420P10, rows, bytes);
    tf_reference(AV_PIX_FMT_YUV420P10, 0, NULL, 0.0);
    plain = (uint8_t *)malloc(rows[0] * bytes[0]);
    TEST_ASSERT_NOT_NULL(plain);
    memcpy(plain, g_reference[0].planes[0], rows[0] * bytes[0]);

    tf_same(AV_PIX_FMT_YUV420P10, options, 0, "diffuse-max", 1.5);
    TEST_ASSERT_TRUE(memcmp(plain, g_reference[0].planes[0], rows[0] * bytes[0]) != 0);
    free(plain);
}

/* twice the frame rate: 2 n - 1 frames, those between the session's at that rate */
static void test_rate(void)
{
    static const char *const options[] = {"fps", "48", NULL};

    if (!tf_interpolates()) {
        TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
    }

    tf_same(AV_PIX_FMT_YUV420P10, options, 2u * TF_RATE, NULL, 0.0);
    TEST_ASSERT_EQUAL_INT(2 * TF_FRAMES - 1, g_count);
}

static void test_refused(void)
{
    static const char *const unknown[] = {"settings", "no-such-setting=1", NULL};
    AVFrame *outs[TF_OUT_MAX];
    int32_t count;

    tf_require();
    TEST_ASSERT_TRUE(tf_run(AV_PIX_FMT_YUV420P10, NULL, true, outs, &count) < 0);
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_TRUE(tf_run(AV_PIX_FMT_YUV420P10, unknown, false, outs, &count) < 0);
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
    int32_t n;

    UNITY_BEGIN();
    RUN_TEST(test_yuv420p10);
    RUN_TEST(test_p010);
    RUN_TEST(test_rgb48);
    RUN_TEST(test_setting);
    RUN_TEST(test_rate);
    RUN_TEST(test_refused);

    for (n = 0; n < TF_OUT_MAX; n++) {
        for (p = 0; p < 3u; p++) {
            free(g_reference[n].planes[p]);
        }
    }

    return UNITY_END();
}
